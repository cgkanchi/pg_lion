/*-------------------------------------------------------------------------
 *
 * lion_multikey.c
 *		Multi-key opclasses for the lion index: arrays and tsvector
 *		(DESIGN.md §17).
 *
 * A multi-key opclass indexes one column value under many keys.  Rather than
 * writing extraction code of our own, the opclasses reuse GIN's support
 * functions verbatim:
 *
 *	 proc 2 = extractValue(value, &nkeys, &nullFlags) -> Datum *keys
 *	 proc 3 = extractQuery(query, &nkeys, strategy, &pmatch, &extra_data,
 *						   &nullFlags, &searchMode) -> Datum *keys
 *
 * so `ginarrayextract`, `ginqueryarrayextract`, `gin_extract_tsvector` and
 * `gin_extract_tsquery` are what our array_ops and tsvector_ops name.  What
 * this file does NOT reuse is GIN's consistent function: a roaring scan does
 * not test one row at a time against a bitmap of "which keys matched", it
 * combines whole posting sets.  So the query side turns the extracted keys
 * into a boolean TREE (LionKeyNode) that the set algebra in lion_expr.c can
 * evaluate, and falls back to "scan everything and recheck" for any query
 * shape that a plain AND/OR of key sets cannot express.
 *
 * The three answers a query can produce are LION_QMODE_NONE (no rows),
 * LION_QMODE_KEYS (exactly the rows the tree selects) and LION_QMODE_ALL (every
 * indexed row, with the operator re-applied by the caller).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gin.h"
#include "access/stratnum.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "tsearch/ts_type.h"
#include "tsearch/ts_utils.h"
#include "utils/fmgroids.h"
#include "utils/pg_crc.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "varatt.h"

#include "lion.h"
#include "lion_count.h"

/*
 * Extracting more keys than this from one query is refused rather than
 * answered: every key needs a posting set, and every posting set may hold a
 * buffer pin for as long as the scan runs (the DESIGN.md §9 rule that the
 * count pushdown rests on, and the same pin budget applies to a bitmap scan
 * that walks several chains at once).  A query with a thousand lexemes is
 * also exactly the case where a sequential scan does well.  Such a query
 * falls back to LION_QMODE_ALL, which is correct, not an error.
 */
#define LION_MAX_QUERY_KEYS		1000

/*
 * GIN's own strategy numbers for the operators our multi-key classes borrow
 * its extraction functions for.  They are NOT ours: GIN numbers the array
 * operators 1 &&, 2 @>, 3 <@, 4 =, and tsvector's @@ is its strategy 1, while
 * the roaring AM numbers every class from one scheme (1 =, 2 @>, 3 &&, 4 <@,
 * 5 @@) so that a scalar and a multi-key opclass can share a validator and a
 * scan.
 *
 * The strategy argument of support function 3 therefore has to be translated
 * before the call, because the function reading it is GIN's.  This is part of
 * the contract a roaring multi-key opclass signs by naming GIN's functions
 * (DESIGN.md §17): support procs 2 and 3 have GIN's signature AND GIN's
 * strategy numbering.
 *
 * Getting it wrong is not loud - ginqueryarrayextract() would answer for the
 * wrong operator and hand back INCLUDE_EMPTY, which only costs a rechecking
 * full scan - so the mapping lives in one place and is asserted to be total.
 */
#define LION_GIN_OVERLAP		1
#define LION_GIN_CONTAINS	2
#define LION_GIN_CONTAINED	3
#define LION_GIN_TSMATCH		1	/* tsvector_ops: @@ is GIN strategy 1 */

static StrategyNumber
lion_gin_strategy(StrategyNumber strategy)
{
	switch (strategy)
	{
		case LION_STRAT_CONTAINS:
			return LION_GIN_CONTAINS;
		case LION_STRAT_OVERLAP:
			return LION_GIN_OVERLAP;
		case LION_STRAT_CONTAINED:
			return LION_GIN_CONTAINED;
		case LION_STRAT_MATCH:
			return LION_GIN_TSMATCH;

			/*
			 * jsonb_contains_ops names its own extractQuery
			 * (lion_jsonb_extract_query()), which reads lion's numbers.
			 */
		case LION_STRAT_JSONB_CONTAINS:
		case LION_STRAT_JSONB_EXISTS:
		case LION_STRAT_JSONB_EXISTS_ANY:
		case LION_STRAT_JSONB_EXISTS_ALL:
			return strategy;
	}

	elog(ERROR, "lion index: strategy %d is not a multi-key strategy",
		 strategy);
	return 0;					/* keep the compiler quiet */
}

/* ---------------------------------------------------------------------
 * Key trees
 * --------------------------------------------------------------------- */

static LionKeyNode *
lion_keynode_leaf(int keyno)
{
	LionKeyNode *n = (LionKeyNode *) palloc0(sizeof(LionKeyNode));

	n->kind = LION_KN_KEY;
	n->keyno = keyno;
	return n;
}

/*
 * An AND or OR over nargs children, which the node takes ownership of.  A
 * one-child operator is folded away: the evaluator would handle it, but the
 * tree is easier to read in a debugger without it.
 */
static LionKeyNode *
lion_keynode_op(LionKeyNodeKind kind, LionKeyNode **args, int nargs)
{
	LionKeyNode *n;

	Assert(kind == LION_KN_AND || kind == LION_KN_OR);
	Assert(nargs >= 1);

	if (nargs == 1)
		return args[0];

	n = (LionKeyNode *) palloc0(sizeof(LionKeyNode));
	n->kind = kind;
	n->nargs = nargs;
	n->args = args;
	return n;
}

/* A flat AND (or OR) over keys 0 .. nkeys-1. */
static LionKeyNode *
lion_keynode_flat(LionKeyNodeKind kind, int nkeys)
{
	LionKeyNode **args;
	int			i;

	Assert(nkeys >= 1);
	args = (LionKeyNode **) palloc(sizeof(LionKeyNode *) * nkeys);
	for (i = 0; i < nkeys; i++)
		args[i] = lion_keynode_leaf(i);

	return lion_keynode_op(kind, args, nkeys);
}

/* ---------------------------------------------------------------------
 * extractValue
 * --------------------------------------------------------------------- */

/*
 * One extracted key's hash and its position in the extraction function's
 * array.  The keys are sorted by hash, then - when the key type has an
 * ordering - by that ordering, and the position is the last tie-break, so
 * the order is deterministic whatever qsort does with equal elements.
 */
typedef struct LionHashPos
{
	uint32		hash;
	int32		pos;
} LionHashPos;

typedef struct LionHashPosArg
{
	LionState  *state;
	Datum	   *raw;
} LionHashPosArg;

static int
lion_hashpos_cmp(const void *a, const void *b, void *arg)
{
	const LionHashPos *x = (const LionHashPos *) a;
	const LionHashPos *y = (const LionHashPos *) b;
	LionHashPosArg *ha = (LionHashPosArg *) arg;

	if (x->hash < y->hash)
		return -1;
	if (x->hash > y->hash)
		return 1;
	if (ha->state->ordered)
	{
		int32		c = DatumGetInt32(FunctionCall2Coll(&ha->state->cmpproc,
														ha->state->collation,
														ha->raw[x->pos],
														ha->raw[y->pos]));

		if (c != 0)
			return c < 0 ? -1 : 1;
	}
	return (x->pos < y->pos) ? -1 : ((x->pos > y->pos) ? 1 : 0);
}

/*
 * Extract the keys of one indexed value.
 *
 * NULL keys are dropped: a row whose array holds a NULL element is indexed
 * under its other elements, and `@>`/`&&` with a NULL on either side is
 * answered by a rechecking scan (see lion_extract_query()), never from the
 * posting sets, so nothing looks for a NULL key.  Duplicates are dropped too,
 * because a posting set is a set and adding the same TID twice would make
 * lion_container_add() report "already indexed" and leave ntids wrong.
 *
 * Every key is hashed once and the keys are sorted by (hash, ordering), where
 * the ordering is the key type's btree comparison (DESIGN.md §21) when it has
 * one.  Equal keys have equal hashes and compare equal, so they end up in the
 * same RUN of neighbours that tie on both; each key is compared with the
 * equality proc against the distinct keys of its own run only.
 *
 * With an ordering a run is one distinct key - two keys that compare equal
 * but are not equal would be an opclass bug - so a value with n keys costs n
 * hashes, one O(n log n) sort and about 2n comparisons whatever the keys are.
 * Sorting by the ordering matters: by the hash alone a run is every key of
 * that hash, and a caller who picks n distinct keys with one hash (int8
 * values (i << 32) | i, which hashint8() folds to the same word) makes the
 * pairwise check n^2/2 equality calls - 20000 such elements took a second,
 * 80000 seventeen, and any role that may INSERT could spend that as often as
 * it liked.
 *
 * Without an ordering (xid, cid: key types with a hash opclass and no btree
 * one) the hash is all there is, and the run is every key of one hash.  That
 * stays quadratic in the number of distinct keys that COLLIDE, which for the
 * built-in types is bounded by the hash function (a 4-byte key hashed by
 * hash_uint32() does not have thousands of preimages of one value), and the
 * inner loop checks for interrupts so that statement_timeout still applies.
 * An opclass whose hash collides freely is its author's choice, and pays the
 * same price in every same-hash run of the directory as well.
 *
 * The keys come out in (hash, ordering) order rather than in the order the
 * extraction function returned them.  Nothing depends on the order: each key
 * is inserted into its own entry, and the build sorts by the directory order
 * anyway.
 *
 * Returns the number of distinct non-NULL keys and puts them in *keys, which
 * is palloc'd in the current context (NULL when there are none).  Zero means
 * the row belongs in the reserved EMPTY entry (DESIGN.md §17).
 */
static int lion_extract_internal(LionState *state, Datum value, Datum **keys,
								 LionKeyPositions **pos);

int
lion_extract_value(LionState *state, Datum value, Datum **keys)
{
	return lion_extract_internal(state, value, keys, NULL);
}

/*
 * The same, with the positions of every key: support proc 5 returns them for
 * the raw keys of proc 2, one per key in proc 2's order, and they travel with
 * their keys through the sort.  A key extracted twice keeps the positions of
 * its first copy (a tsvector never has two; its lexemes are unique).
 */
int
lion_extract_value_pos(LionState *state, Datum value, Datum **keys,
					   LionKeyPositions **pos)
{
	Assert(state->positions);
	return lion_extract_internal(state, value, keys, pos);
}

/*
 * Is key the length key of a column that stores positions: the empty text
 * (DESIGN.md §17, "Ranking")?
 */
bool
lion_is_length_key(const LionState *state, Datum key)
{
	return state->positions && state->typid == TEXTOID &&
		VARSIZE_ANY_EXHDR(DatumGetPointer(key)) == 0;
}

static int
lion_extract_internal(LionState *state, Datum value, Datum **keys,
					  LionKeyPositions **pos)
{
	Datum	   *raw;
	int32		nraw = 0;
	bool	   *nulls = NULL;
	Datum	   *out;
	LionKeyPositions *rawpos = NULL;
	LionKeyPositions *outpos = NULL;
	LionHashPos *ord;
	LionHashPosArg ha;
	int			nlive = 0;
	int			nout = 0;
	int			runstart = 0;	/* where this run's distinct keys start */
	int			i;
	int			j;

	Assert(state->multikey);

	raw = (Datum *) DatumGetPointer(FunctionCall3Coll(&state->extractvalue,
													  state->collation,
													  value,
													  PointerGetDatum(&nraw),
													  PointerGetDatum(&nulls)));

	*keys = NULL;
	if (pos != NULL)
		*pos = NULL;
	if (nraw <= 0 || raw == NULL)
		return 0;

	if (pos != NULL)
	{
		int32		npos = 0;

		rawpos = (LionKeyPositions *)
			DatumGetPointer(FunctionCall2Coll(&state->positionsproc,
											  state->collation, value,
											  PointerGetDatum(&npos)));
		if (npos != nraw || rawpos == NULL)
			elog(ERROR, "lion index: support function %d returned positions for %d keys, but support function %d extracted %d",
				 LION_POSITIONS_PROC, npos, LION_EXTRACTVALUE_PROC, nraw);
	}

	/* Hash every non-NULL key once, then sort. */
	ord = (LionHashPos *) palloc(sizeof(LionHashPos) * nraw);
	for (i = 0; i < nraw; i++)
	{
		if (nulls != NULL && nulls[i])
			continue;
		ord[nlive].hash = lion_hash_key(state, raw[i]);
		ord[nlive].pos = i;
		nlive++;
	}

	if (nlive == 0)
	{
		pfree(ord);
		return 0;
	}
	ha.state = state;
	ha.raw = raw;
	if (nlive > 1)
		qsort_arg(ord, (size_t) nlive, sizeof(LionHashPos), lion_hashpos_cmp,
				  &ha);

	/* one more for the length key */
	out = (Datum *) palloc(sizeof(Datum) * (nlive + 1));
	if (pos != NULL)
		outpos = (LionKeyPositions *)
			palloc(sizeof(LionKeyPositions) * (nlive + 1));

	for (i = 0; i < nlive; i++)
	{
		Datum		key = raw[ord[i].pos];
		bool		dup = false;

		/*
		 * The empty key is the row's length in a column that stores
		 * positions (below), never a lexeme: tsvector_in() and
		 * array_to_tsvector() refuse an empty one, and no tsquery can name
		 * it.
		 */
		if (lion_is_length_key(state, key))
			continue;

		/*
		 * A new hash value, or a key the ordering puts after the previous
		 * one, starts a new run of possible equals.
		 */
		if (i > 0 &&
			(ord[i].hash != ord[i - 1].hash ||
			 (state->ordered &&
			  DatumGetInt32(FunctionCall2Coll(&state->cmpproc,
											  state->collation,
											  raw[ord[i - 1].pos],
											  key)) != 0)))
			runstart = nout;

		for (j = runstart; j < nout; j++)
		{
			if (DatumGetBool(FunctionCall2Coll(&state->eqproc,
											   state->collation,
											   out[j], key)))
			{
				dup = true;
				break;
			}
			CHECK_FOR_INTERRUPTS();
		}
		if (!dup)
		{
			if (outpos != NULL)
				outpos[nout] = rawpos[ord[i].pos];
			out[nout++] = key;
		}

		CHECK_FOR_INTERRUPTS();
	}

	pfree(ord);

	/*
	 * THE ROW'S LENGTH (DESIGN.md §17, "Ranking"): a column that stores
	 * positions also files every row with a lexeme under the empty key, with
	 * one "position" that is the row's length - the number of its lexeme
	 * occurrences, a stripped lexeme counted once - in all sixteen bits of a
	 * WordEntryPos, capped at 65535.  BM25 needs it for every candidate, and
	 * the empty key's position tree hands it over in TID order like any
	 * other key's.
	 */
	if (state->positions && state->typid == TEXTOID && nout > 0)
	{
		uint32		len = 0;

		for (j = 0; j < nout; j++)
			len += (outpos != NULL) ? Max((uint32) outpos[j].npos, 1) : 1;
		if (outpos != NULL)
		{
			uint16	   *lp = (uint16 *) palloc(sizeof(uint16));

			*lp = (uint16) Min(len, (uint32) PG_UINT16_MAX);
			outpos[nout].npos = 1;
			outpos[nout].pos = lp;
		}
		out[nout++] = PointerGetDatum(cstring_to_text_with_len("", 0));
	}

	if (nout == 0)
	{
		pfree(out);
		if (outpos != NULL)
			pfree(outpos);
		return 0;
	}
	*keys = out;
	if (pos != NULL)
		*pos = outpos;
	return nout;
}

/* ---------------------------------------------------------------------
 * extractQuery
 * --------------------------------------------------------------------- */

/*
 * Build the key tree of a tsquery (DESIGN.md §17).
 *
 * A TSQuery is an array of QueryItems in prefix order: an operator is
 * followed by its RIGHT operand at item + 1 and its LEFT operand at
 * item + qoperator.left (ts_type.h).  gin_extract_tsquery() numbers the keys
 * it returns by walking that array from 0 and counting QI_VAL items, so the
 * j'th key belongs to the j'th QI_VAL item in array order; map[] is the same
 * item -> key map gin_extract_tsquery() builds for its consistent function,
 * recomputed here rather than fished out of extra_data.
 *
 * Only AND and OR of plain lexemes become a tree.  Everything else sets *ok
 * false and makes the whole query a rechecking scan:
 *
 *	- OP_NOT has no complement over posting sets that does not need the set of
 *	  all rows (`!a` is "every row minus a's"), and the ALL fallback is that
 *	  set anyway;
 *	- OP_PHRASE needs lexeme positions, which the index does not store;
 *	- a prefix lexeme (`foo:*`) matches a RANGE of keys; by the time a
 *	  query gets here lion_tsquery_expand_prefixes() has replaced every prefix
 *	  it could with the OR of its keys, so one left over is past the cap;
 *	- a weight mask (`foo:A`) needs the weights, which the index does not
 *	  store either.
 *
 * The prefix case is also reported by extractQuery through pmatch, and the
 * caller checks that; it is tested here as well so that the tree builder
 * alone is enough to decide, and so that a future opclass whose extractQuery
 * does not set pmatch cannot produce a wrong tree.
 */
static LionKeyNode *
lion_tsquery_tree(QueryItem *items, int32 size, int32 i, const int *map,
				 bool *ok)
{
	QueryItem  *item;

	check_stack_depth();

	if (!*ok)
		return NULL;
	if (i < 0 || i >= size)
	{
		*ok = false;
		return NULL;
	}

	item = &items[i];

	if (item->type == QI_VAL)
	{
		if (item->qoperand.prefix || item->qoperand.weight != 0)
		{
			*ok = false;
			return NULL;
		}
		return lion_keynode_leaf(map[i]);
	}

	if (item->type != QI_OPR)
	{
		*ok = false;
		return NULL;
	}

	switch (item->qoperator.oper)
	{
		case OP_AND:
		case OP_OR:
			{
				uint32		left = item->qoperator.left;
				LionKeyNode **args;

				/*
				 * The left operand is at item + left, and the right subtree
				 * lies between them, so left is at least 2 and stays inside
				 * the array.  Checking it here rather than trusting it keeps
				 * a malformed tsquery from recursing on this very item until
				 * check_stack_depth() gives up.
				 */
				if (left < 2 || left >= (uint32) (size - i))
				{
					*ok = false;
					return NULL;
				}

				args = (LionKeyNode **) palloc(sizeof(LionKeyNode *) * 2);

				/* Right operand first, exactly as ts_type.h lays them out. */
				args[1] = lion_tsquery_tree(items, size, i + 1, map, ok);
				args[0] = lion_tsquery_tree(items, size, i + (int32) left,
										   map, ok);
				if (!*ok)
					return NULL;
				return lion_keynode_op(item->qoperator.oper == OP_AND ?
									  LION_KN_AND : LION_KN_OR, args, 2);
			}

		default:
			/* OP_NOT, OP_PHRASE */
			*ok = false;
			return NULL;
	}
}

/*
 * The tsquery half of lion_extract_query(): turn the query into a tree over
 * the keys extractQuery returned, or give up.
 */
static LionKeyNode *
lion_tsquery_plan(Datum query, int nkeys)
{
	TSQuery		tsq = DatumGetTSQuery(query);
	QueryItem  *items;
	int		   *map;
	int32		i;
	int32		j = 0;
	bool		ok = true;
	LionKeyNode *tree;

	if (tsq->size <= 0)
		return NULL;

	items = GETQUERY(tsq);

	/* item -> key number, the same numbering gin_extract_tsquery() used */
	map = (int *) palloc0(sizeof(int) * tsq->size);
	for (i = 0; i < tsq->size; i++)
	{
		if (items[i].type == QI_VAL)
			map[i] = j++;
	}

	/*
	 * A disagreement here would mean the opclass's extractQuery is not the
	 * one this code was written against; fall back rather than index into
	 * keys[] with a number that means something else.
	 */
	if (j != nkeys)
	{
		pfree(map);
		return NULL;
	}

	tree = lion_tsquery_tree(items, tsq->size, 0, map, &ok);
	pfree(map);

	return ok ? tree : NULL;
}

/*
 * One call of the opclass's extractQuery (support proc 3), with GIN's
 * conventions applied: searchMode starts at GIN_SEARCH_MODE_DEFAULT, and an
 * out-of-range answer is treated as GIN_SEARCH_MODE_ALL (ginNewScanKey() does
 * the same), except lion's own LION_SEARCH_MODE_LOSSY.  pmatch and nulls stay
 * NULL unless the function set them.
 */
typedef struct LionRawQuery
{
	int32		nkeys;
	Datum	   *keys;
	bool	   *pmatch;
	bool	   *nulls;
	int32		searchMode;
} LionRawQuery;

static void
lion_call_extractquery(LionState *state, Datum query, StrategyNumber strategy,
					  LionRawQuery *raw)
{
	Pointer    *extra_data = NULL;

	/*
	 * Only a multi-key column's state has an extractQuery: a scalar one's is
	 * left unset (lion_fill_state()), and calling through it would jump to
	 * address zero.  No caller means to hand one over, but a column mix-up
	 * once did (the 2026-09-27 review: an index listing one column under a
	 * scalar and a multi-key opclass), so this is an error, not an Assert.
	 */
	if (!state->multikey || !OidIsValid(state->extractquery.fn_oid))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("lion index key column %d is not a multi-key column",
						(int) state->attno)));

	raw->nkeys = 0;
	raw->pmatch = NULL;
	raw->nulls = NULL;
	raw->searchMode = GIN_SEARCH_MODE_DEFAULT;

	raw->keys = (Datum *)
		DatumGetPointer(FunctionCall7Coll(&state->extractquery,
										  state->collation,
										  query,
										  PointerGetDatum(&raw->nkeys),
										  UInt16GetDatum(lion_gin_strategy(strategy)),
										  PointerGetDatum(&raw->pmatch),
										  PointerGetDatum(&extra_data),
										  PointerGetDatum(&raw->nulls),
										  PointerGetDatum(&raw->searchMode)));

	if (raw->searchMode != LION_SEARCH_MODE_LOSSY &&
		(raw->searchMode < GIN_SEARCH_MODE_DEFAULT ||
		 raw->searchMode > GIN_SEARCH_MODE_ALL))
		raw->searchMode = GIN_SEARCH_MODE_ALL;
}

/*
 * Extract a query and decide how its keys combine (DESIGN.md §17).
 *
 * The GIN contract is followed to the letter (lion_call_extractquery()), and
 * pmatch/nullFlags are only read when extractQuery set them.
 *
 * Anything that is not "the rows are exactly the ones an AND/OR of whole key
 * sets selects" becomes LION_QMODE_ALL: a partial (prefix) match, which is
 * only left when lion_tsquery_expand_prefixes() could not expand it; a NULL key, because
 * no row is indexed under one; INCLUDE_EMPTY and ALL, because the rows a
 * multi-key opclass extracted nothing from are not under any key.
 */
void
lion_extract_query(LionState *state, Datum query, StrategyNumber strategy,
				  LionQuery *q)
{
	LionRawQuery raw;
	int32		nkeys;
	bool	   *pmatch;
	bool	   *nulls;
	Datum	   *keys;
	int			i;

	memset(q, 0, sizeof(LionQuery));

	lion_call_extractquery(state, query, strategy, &raw);
	nkeys = raw.nkeys;
	keys = raw.keys;
	pmatch = raw.pmatch;
	nulls = raw.nulls;

	if (raw.searchMode != GIN_SEARCH_MODE_DEFAULT)
	{
		/*
		 * INCLUDE_EMPTY and ALL both mean "every indexed row, then recheck",
		 * and LOSSY keys are not an exact answer either.
		 */
		q->mode = LION_QMODE_ALL;
		return;
	}

	if (nkeys <= 0 || keys == NULL)
	{
		/*
		 * DEFAULT mode with no keys: nothing can match.  `tags && '{}'` and
		 * an empty tsquery both land here.
		 */
		q->mode = LION_QMODE_NONE;
		return;
	}

	if (nkeys > LION_MAX_QUERY_KEYS)
	{
		q->mode = LION_QMODE_ALL;
		return;
	}

	for (i = 0; i < nkeys; i++)
	{
		if ((pmatch != NULL && pmatch[i]) || (nulls != NULL && nulls[i]))
		{
			q->mode = LION_QMODE_ALL;
			return;
		}
	}

	q->nkeys = nkeys;
	q->keys = keys;

	switch (strategy)
	{
		case LION_STRAT_CONTAINS:
		case LION_STRAT_JSONB_CONTAINS:
		case LION_STRAT_JSONB_EXISTS:
		case LION_STRAT_JSONB_EXISTS_ALL:
			/* every element of the query array (path, key) must be present */
			q->tree = lion_keynode_flat(LION_KN_AND, nkeys);
			break;

		case LION_STRAT_OVERLAP:
		case LION_STRAT_JSONB_EXISTS_ANY:
			/* at least one of them */
			q->tree = lion_keynode_flat(LION_KN_OR, nkeys);
			break;

		case LION_STRAT_MATCH:
			q->tree = lion_tsquery_plan(query, nkeys);
			break;

		case LION_STRAT_CONTAINED:

			/*
			 * `<@` cannot be answered from the keys at all - a row matches
			 * when it has NO key outside the query array, which the index
			 * cannot tell - and ginqueryarrayextract() says so with
			 * INCLUDE_EMPTY, so this is unreachable.  Fall back anyway.
			 */
			q->tree = NULL;
			break;

		default:
			q->tree = NULL;
			break;
	}

	if (q->tree == NULL)
	{
		q->mode = LION_QMODE_ALL;
		q->nkeys = 0;
		q->keys = NULL;
		return;
	}

	q->mode = LION_QMODE_KEYS;
}

/* ---------------------------------------------------------------------
 * Supersets, for a caller that rechecks (DESIGN.md §17, "A query known only
 * at run time")
 * --------------------------------------------------------------------- */

/*
 * Is key k of an extraction one that no posting set holds - a partial match,
 * or a NULL?  A superset has to do without it.
 */
static bool
lion_raw_key_unusable(const LionRawQuery *raw, int k)
{
	return (raw->pmatch != NULL && raw->pmatch[k]) ||
		(raw->nulls != NULL && raw->nulls[k]);
}

/*
 * The superset form of lion_tsquery_tree(): a tree that selects AT LEAST the
 * rows the tsquery rooted at item i matches, or NULL for "every row".  *lossy
 * is set whenever a rule below widened the answer; *ok is cleared for an item
 * the walk does not understand, which makes the whole query every row.
 *
 *	- a lexeme with a weight mask is the lexeme at any weight;
 *	- a prefix lexeme, or one extractQuery flagged partial or NULL, is every
 *	  row: an expandable prefix was already rewritten into the OR of its keys
 *	  by lion_tsquery_expand_prefixes(), so one left over is past the cap;
 *	- `!a` is every row: no posting set is the complement of another;
 *	- `a <N> b` is `a & b`: a phrase matches only where both of its operands
 *	  match, at positions the index does not store, and an operand that is
 *	  itself every row (`!a <-> b`) constrains nothing;
 *	- an AND drops the operands that are every row, and is every row when all
 *	  of them are; an OR is every row when either operand is.
 */
static LionKeyNode *
lion_tsquery_superset(QueryItem *items, int32 size, int32 i, const int *map,
					 const LionRawQuery *raw, bool *lossy, bool *ok)
{
	QueryItem  *item;

	check_stack_depth();

	if (!*ok)
		return NULL;
	if (i < 0 || i >= size)
	{
		*ok = false;
		return NULL;
	}

	item = &items[i];

	if (item->type == QI_VAL)
	{
		if (item->qoperand.prefix || lion_raw_key_unusable(raw, map[i]))
		{
			*lossy = true;
			return NULL;
		}
		if (item->qoperand.weight != 0)
			*lossy = true;
		return lion_keynode_leaf(map[i]);
	}

	if (item->type != QI_OPR)
	{
		*ok = false;
		return NULL;
	}

	switch (item->qoperator.oper)
	{
		case OP_NOT:
			*lossy = true;
			return NULL;

		case OP_AND:
		case OP_OR:
		case OP_PHRASE:
			{
				uint32		left = item->qoperator.left;
				LionKeyNode *lt;
				LionKeyNode *rt;
				LionKeyNode **args;
				int			nargs = 0;

				/* the bounds lion_tsquery_tree() checks, for the same reason */
				if (left < 2 || left >= (uint32) (size - i))
				{
					*ok = false;
					return NULL;
				}

				rt = lion_tsquery_superset(items, size, i + 1, map, raw,
										   lossy, ok);
				lt = lion_tsquery_superset(items, size, i + (int32) left, map,
										   raw, lossy, ok);
				if (!*ok)
					return NULL;

				if (item->qoperator.oper == OP_OR)
				{
					if (lt == NULL || rt == NULL)
						return NULL;
					args = (LionKeyNode **) palloc(sizeof(LionKeyNode *) * 2);
					args[0] = lt;
					args[1] = rt;
					return lion_keynode_op(LION_KN_OR, args, 2);
				}

				if (item->qoperator.oper == OP_PHRASE)
					*lossy = true;
				args = (LionKeyNode **) palloc(sizeof(LionKeyNode *) * 2);
				if (lt != NULL)
					args[nargs++] = lt;
				if (rt != NULL)
					args[nargs++] = rt;
				if (nargs == 0)
					return NULL;
				return lion_keynode_op(LION_KN_AND, args, nargs);
			}

		default:
			*ok = false;
			return NULL;
	}
}

/*
 * Renumber a tree's leaves onto the keys it names, in the order it first
 * names them, and keep only those: a superset may leave keys out - a partial
 * one, a NULL one - that must never be looked up.
 */
static void
lion_superset_renumber(LionKeyNode *node, int *newno, const Datum *from,
					   Datum *to, int *nto)
{
	int			i;

	if (node->kind == LION_KN_KEY)
	{
		if (newno[node->keyno] < 0)
		{
			newno[node->keyno] = *nto;
			to[(*nto)++] = from[node->keyno];
		}
		node->keyno = newno[node->keyno];
		return;
	}
	for (i = 0; i < node->nargs; i++)
		lion_superset_renumber(node->args[i], newno, from, to, nto);
}

/*
 * lion_extract_query() for a caller that rechecks every row it is handed: the
 * count pushdown, for a clause whose query it only has at run time - a
 * parameter of a generic plan, `to_tsquery(current_setting(...))` - and which
 * therefore cannot have declined the query when it turned out to be one the
 * key sets do not answer exactly.  It counts a SUPERSET instead and rechecks
 * each candidate in the heap, and the narrower the superset the fewer the
 * candidates:
 *
 *	- `@>` with a NULL element is the AND of the other elements (a NULL is
 *	  under no key, and an AND of fewer keys selects more); every row when
 *	  there are none;
 *	- `&&` with a NULL or partial key is every row: an OR cannot leave a key
 *	  out and stay a superset;
 *	- a tsquery is widened as lion_tsquery_superset() says;
 *	- keys extractQuery calls LION_SEARCH_MODE_LOSSY combine as they would
 *	  exactly, and select a superset;
 *	- INCLUDE_EMPTY and ALL, more keys than LION_MAX_QUERY_KEYS, `<@` and
 *	  anything else are every row, as they are for lion_extract_query().
 *
 * NONE and KEYS mean what they mean there: a query lion_extract_query()
 * answers exactly comes back KEYS with the same keys, and needs no recheck.
 */
void
lion_extract_query_superset(LionState *state, Datum query,
						   StrategyNumber strategy, LionQuery *q)
{
	LionRawQuery raw;
	LionKeyNode *tree = NULL;
	bool		lossy = false;
	bool		ok = true;
	int		   *newno;
	Datum	   *keys;
	int			nkeys = 0;
	int			i;

	memset(q, 0, sizeof(LionQuery));

	lion_call_extractquery(state, query, strategy, &raw);

	if (raw.searchMode == LION_SEARCH_MODE_LOSSY)
		lossy = true;
	else if (raw.searchMode != GIN_SEARCH_MODE_DEFAULT)
	{
		/* the rows no key was extracted from may qualify too */
		q->mode = LION_QMODE_ALL;
		return;
	}
	if (raw.nkeys <= 0 || raw.keys == NULL)
	{
		q->mode = LION_QMODE_NONE;
		return;
	}
	if (raw.nkeys > LION_MAX_QUERY_KEYS)
	{
		q->mode = LION_QMODE_ALL;
		return;
	}

	switch (strategy)
	{
		case LION_STRAT_CONTAINS:
		case LION_STRAT_JSONB_CONTAINS:
		case LION_STRAT_JSONB_EXISTS:
		case LION_STRAT_JSONB_EXISTS_ALL:
			{
				LionKeyNode **args = (LionKeyNode **)
					palloc(sizeof(LionKeyNode *) * raw.nkeys);
				int			nargs = 0;

				for (i = 0; i < raw.nkeys; i++)
				{
					if (lion_raw_key_unusable(&raw, i))
						lossy = true;
					else
						args[nargs++] = lion_keynode_leaf(i);
				}
				if (nargs > 0)
					tree = lion_keynode_op(LION_KN_AND, args, nargs);
				break;
			}

		case LION_STRAT_OVERLAP:
		case LION_STRAT_JSONB_EXISTS_ANY:
			for (i = 0; i < raw.nkeys; i++)
			{
				if (lion_raw_key_unusable(&raw, i))
					break;
			}
			if (i == raw.nkeys)
				tree = lion_keynode_flat(LION_KN_OR, raw.nkeys);
			break;

		case LION_STRAT_MATCH:
			{
				TSQuery		tsq = DatumGetTSQuery(query);
				QueryItem  *items;
				int		   *map;
				int32		j = 0;

				if (tsq->size <= 0)
					break;
				items = GETQUERY(tsq);
				map = (int *) palloc0(sizeof(int) * tsq->size);
				for (i = 0; i < tsq->size; i++)
				{
					if (items[i].type == QI_VAL)
						map[i] = j++;
				}
				/* the numbering lion_tsquery_plan() relies on, checked alike */
				if (j == raw.nkeys)
					tree = lion_tsquery_superset(items, tsq->size, 0, map,
												 &raw, &lossy, &ok);
				pfree(map);
				break;
			}

		default:
			break;
	}

	if (!ok || tree == NULL)
	{
		q->mode = LION_QMODE_ALL;
		return;
	}

	/* Only the keys the tree names are looked up. */
	newno = (int *) palloc(sizeof(int) * raw.nkeys);
	for (i = 0; i < raw.nkeys; i++)
		newno[i] = -1;
	keys = (Datum *) palloc(sizeof(Datum) * raw.nkeys);
	lion_superset_renumber(tree, newno, raw.keys, keys, &nkeys);
	pfree(newno);

	q->nkeys = nkeys;
	q->keys = keys;
	q->tree = tree;
	q->mode = lossy ? LION_QMODE_LOSSY : LION_QMODE_KEYS;
}

/*
 * For the position filter (lion_posfilter.c): the key of every QI_VAL item
 * of a tsquery, by item index - (*itemkeys)[i] for items[i] a QI_VAL, and
 * nothing for the operators - from the very extraction lion_extract_query()
 * runs, whose j'th key is the j'th QI_VAL item's (lion_tsquery_plan()).
 * False when some operand has no key a filter can follow: a prefix lexeme
 * (any number of keys), or a key extractQuery flagged partial or NULL, or an
 * extraction that is not the plain one.  The filter then stays out of the way
 * and the caller rechecks, as it always did.
 */
bool
lion_tsquery_item_keys(LionState *state, Datum query, StrategyNumber strategy,
					   Datum **itemkeys)
{
	LionRawQuery raw;
	TSQuery		tsq = DatumGetTSQuery(query);
	QueryItem  *items;
	Datum	   *out;
	int32		j = 0;
	int32		i;

	*itemkeys = NULL;
	if (strategy != LION_STRAT_MATCH || tsq->size <= 0)
		return false;

	lion_call_extractquery(state, query, strategy, &raw);
	if (raw.searchMode != GIN_SEARCH_MODE_DEFAULT || raw.nkeys <= 0 ||
		raw.keys == NULL || raw.nkeys > LION_MAX_QUERY_KEYS)
		return false;

	items = GETQUERY(tsq);
	out = (Datum *) palloc0(sizeof(Datum) * tsq->size);
	for (i = 0; i < tsq->size; i++)
	{
		if (items[i].type != QI_VAL)
			continue;
		if (j >= raw.nkeys || items[i].qoperand.prefix ||
			lion_raw_key_unusable(&raw, j))
		{
			pfree(out);
			return false;
		}
		out[i] = raw.keys[j++];
	}
	if (j != raw.nkeys)
	{
		pfree(out);
		return false;
	}

	*itemkeys = out;
	return true;
}

/*
 * Will a superset of `col op query` be decided from stored positions
 * (lion_posfilter.c), on a column of opfamily over lefttype?  When the
 * opfamily stores them (support function 5) and the query names no key a
 * position cursor cannot follow, as lion_tsquery_item_keys() says - the
 * planner's question, asked of a literal before any index is open.
 */
bool
lion_query_posexact(Oid opfamily, Oid lefttype, Oid opno, Datum query,
					Oid collation)
{
	int			strategy = get_op_opfamily_strategy(opno, opfamily);
	Oid			proc = get_opfamily_proc(opfamily, lefttype, lefttype,
										 LION_EXTRACTQUERY_PROC);
	LionState	state;
	Datum	   *itemkeys;
	MemoryContext cxt;
	MemoryContext oldcxt;
	bool		result;

	if (strategy == 0 || !OidIsValid(proc) ||
		!OidIsValid(get_opfamily_proc(opfamily, lefttype, lefttype,
									  LION_POSITIONS_PROC)))
		return false;

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"lion position query",
								ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);
	memset(&state, 0, sizeof(state));
	state.multikey = true;
	state.collation = collation;
	fmgr_info(proc, &state.extractquery);
	/* a prefix lexeme as the lexemes the executor will expand it into */
	if (strategy == LION_STRAT_MATCH)
		query = lion_tsquery_strip_prefixes(query);
	result = lion_tsquery_item_keys(&state, query, (StrategyNumber) strategy,
									&itemkeys);
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);

	return result;
}

/* ---------------------------------------------------------------------
 * Support proc 5 for tsvector (DESIGN.md §17, "Stored positions")
 * --------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(lion_tsvector_positions);

/*
 * lion_tsvector_positions(tsvector, internal) returns internal: the positions
 * of every lexeme of the tsvector, in its own order - the order
 * gin_extract_tsvector() returns the lexemes in, one key per lexeme - and the
 * count through the second argument.  The positions point into the
 * detoasted tsvector, which lives as long as the caller's memory context.
 */
Datum
lion_tsvector_positions(PG_FUNCTION_ARGS)
{
	TSVector	vector = PG_GETARG_TSVECTOR(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	WordEntry  *we = ARRPTR(vector);
	LionKeyPositions *out;
	int32		i;

	*nentries = vector->size;
	if (vector->size == 0)
		PG_RETURN_POINTER(NULL);

	out = (LionKeyPositions *) palloc(sizeof(LionKeyPositions) * vector->size);
	for (i = 0; i < vector->size; i++)
	{
		if (we[i].haspos)
		{
			out[i].npos = (uint16) POSDATALEN(vector, &we[i]);
			out[i].pos = (const uint16 *) POSDATAPTR(vector, &we[i]);
		}
		else
		{
			out[i].npos = 0;
			out[i].pos = NULL;
		}
	}
	PG_RETURN_POINTER(out);
}

/*
 * The position member of the row at code for one extracted key: its
 * positions as support proc 5 gave them, sorted and with repeats dropped
 * should a tsvector not have them so (one that came from tsvector_in() or
 * to_tsvector() always does), the higher weight kept for a repeat.
 */
void
lion_posmember_from_key(LionPosMember *m, uint64 code,
						const LionKeyPositions *kp)
{
	uint32		n = Min((uint32) kp->npos, (uint32) LION_POS_MAX_NPOS);
	uint32		i;
	uint32		j;
	bool		sorted = true;

	m->code = code;
	for (i = 0; i < n; i++)
	{
		m->pos[i] = kp->pos[i];
		if (i > 0 && LION_POS_POS(m->pos[i]) <= LION_POS_POS(m->pos[i - 1]))
			sorted = false;
	}
	if (!sorted)
	{
		/* an insertion sort: npos is at most 256 */
		for (i = 1; i < n; i++)
		{
			uint16		v = m->pos[i];

			for (j = i; j > 0 && LION_POS_POS(m->pos[j - 1]) > LION_POS_POS(v); j--)
				m->pos[j] = m->pos[j - 1];
			m->pos[j] = v;
		}
		for (i = 0, j = 0; i < n; i++)
		{
			if (j > 0 && LION_POS_POS(m->pos[j - 1]) == LION_POS_POS(m->pos[i]))
			{
				if (LION_POS_WEIGHT(m->pos[i]) > LION_POS_WEIGHT(m->pos[j - 1]))
					m->pos[j - 1] = m->pos[i];
			}
			else
				m->pos[j++] = m->pos[i];
		}
		n = j;
	}
	m->npos = (uint16) n;
}


/* ---------------------------------------------------------------------
 * Prefix lexemes (DESIGN.md §17, "Prefix lexemes")
 * --------------------------------------------------------------------- */

/*
 * Does the tsquery have a prefix operand (`foo:*`)?
 */
bool
lion_tsquery_has_prefix(Datum query)
{
	TSQuery		tsq = DatumGetTSQuery(query);
	QueryItem  *items = GETQUERY(tsq);
	int32		i;

	for (i = 0; i < tsq->size; i++)
	{
		if (items[i].type == QI_VAL && items[i].qoperand.prefix)
			return true;
	}
	return false;
}

/* A QI_VAL node for the lexeme word[0 .. len), at weight. */
static QTNode *
lion_qtn_lexeme(const char *word, int len, uint8 weight)
{
	QTNode	   *n = (QTNode *) palloc0(sizeof(QTNode));
	pg_crc32	crc;

	n->valnode = (QueryItem *) palloc0(sizeof(QueryItem));
	n->valnode->qoperand.type = QI_VAL;
	n->valnode->qoperand.weight = weight;
	n->valnode->qoperand.prefix = false;
	n->valnode->qoperand.length = len;
	INIT_LEGACY_CRC32(crc);
	COMP_LEGACY_CRC32(crc, word, len);
	FIN_LEGACY_CRC32(crc);
	n->valnode->qoperand.valcrc = (int32) crc;
	n->word = (char *) palloc(len + 1);
	memcpy(n->word, word, len);
	n->word[len] = '\0';
	n->sign = ((uint32) 1) << (((unsigned int) crc) % 32);
	return n;
}

/* The OR of nodes[0 .. n), as a balanced tree of binary ORs. */
static QTNode *
lion_qtn_or(QTNode **nodes, int n)
{
	QTNode	   *o;
	int			half;

	if (n == 1)
		return nodes[0];
	half = n / 2;
	o = (QTNode *) palloc0(sizeof(QTNode));
	o->valnode = (QueryItem *) palloc0(sizeof(QueryItem));
	o->valnode->qoperator.type = QI_OPR;
	o->valnode->qoperator.oper = OP_OR;
	o->nchild = 2;
	o->child = (QTNode **) palloc(sizeof(QTNode *) * 2);
	o->child[0] = lion_qtn_or(nodes, half);
	o->child[1] = lion_qtn_or(nodes + half, n - half);
	o->sign = o->child[0]->sign | o->child[1]->sign;
	return o;
}

/*
 * The keys of a text column that start with prefix[0 .. plen), as text
 * Datums in *keys, from a walk of the directory from the prefix up: the
 * column is ordered by bttextcmp() under C (lion_state.c), which is byte
 * order, so they are one run and the first key past it ends the walk.
 * Returns the number found, or -1 once there are more than max of them, or
 * when the walk cannot be bounded by the prefix.
 */
static int
lion_prefix_keys(Relation index, LionState *col, const char *prefix,
				 int plen, int max, Datum **keys)
{
	LionRange	range;
	LionEntryScan es;
	int			n = 0;
	int			cap = 16;

	lion_range_init(&range, index, (AttrNumber) col->attno);
	/* the bound is a key of the column's own (text) type: InvalidOid says so */
	lion_range_add(&range, index, LION_STRAT_GE, F_TEXT_GE, InvalidOid,
				   PointerGetDatum(cstring_to_text_with_len(prefix, plen)),
				   false, C_COLLATION_OID);
	if (!range.ordered)
		return -1;

	*keys = (Datum *) palloc(sizeof(Datum) * cap);
	lion_entry_scan_begin_range(&es, index, (AttrNumber) col->attno, &range);
	for (;;)
	{
		Datum		key;
		LionPostingSet ps;
		text	   *t;
		bool		inside;

		CHECK_FOR_INTERRUPTS();
		if (!lion_entry_scan_next(&es, &key, &ps))
			break;
		if (ps.keyisnull)
		{
			lion_posting_set_release(&ps);
			continue;
		}
		t = DatumGetTextPP(key);
		inside = (int) VARSIZE_ANY_EXHDR(t) >= plen &&
			memcmp(VARDATA_ANY(t), prefix, plen) == 0;
		lion_posting_set_release(&ps);
		if (!inside)
			break;
		if (n >= max)
		{
			n = -1;
			break;
		}
		if (n >= cap)
		{
			cap *= 2;
			*keys = (Datum *) repalloc(*keys, sizeof(Datum) * cap);
		}
		(*keys)[n++] = PointerGetDatum(t);
	}
	lion_entry_scan_end(&es);
	return n;
}

/*
 * Rewrite the prefix operands of a QTNode tree in place; *budget is how many
 * more lexemes the query may grow to.  False when one cannot be expanded.
 */
static bool
lion_qtn_expand(Relation index, LionState *col, QTNode *node, int *budget)
{
	int			i;

	check_stack_depth();

	if (node->valnode->type == QI_OPR)
	{
		for (i = 0; i < node->nchild; i++)
		{
			if (!lion_qtn_expand(index, col, node->child[i], budget))
				return false;
		}
		return true;
	}

	if (node->valnode->qoperand.prefix)
	{
		QueryOperand *op = &node->valnode->qoperand;
		Datum	   *keys = NULL;
		QTNode	  **leaves;
		QTNode	   *repl;
		int			n;

		n = lion_prefix_keys(index, col, node->word, op->length,
							 *budget + 1, &keys);
		if (n < 0)
			return false;

		if (n == 0)
		{
			/*
			 * No lexeme in the index starts with it, so no row does: the
			 * lexeme itself, which no row has either, answers the same.
			 */
			repl = lion_qtn_lexeme(node->word, op->length, op->weight);
		}
		else
		{
			*budget -= n - 1;
			if (*budget < 0)
				return false;
			leaves = (QTNode **) palloc(sizeof(QTNode *) * n);
			for (i = 0; i < n; i++)
			{
				text	   *t = DatumGetTextPP(keys[i]);

				leaves[i] = lion_qtn_lexeme(VARDATA_ANY(t),
											VARSIZE_ANY_EXHDR(t),
											op->weight);
			}
			repl = lion_qtn_or(leaves, n);
		}
		*node = *repl;
	}
	return true;
}

/*
 * Rewrite every prefix operand `foo:*` of a tsquery into the OR of the
 * lexemes the index holds that start with foo, keeping its weight mask:
 * `foo:*A` is `(foo1:A | foo2:A ...)`, also inside a phrase or under a NOT,
 * where an OR operand means exactly what the prefix did.
 *
 * Every row the caller's snapshot can see is indexed under each of its
 * lexemes, by an insert that happened before that row's transaction
 * committed, and so before this walk: a lexeme a visible row has is a key
 * here.  The rewritten query therefore selects exactly the rows the original
 * does, among the rows the caller can see.  Keys of rows the caller cannot
 * see may be found too, and only make the query longer.
 *
 * *expanded is false, and the query is returned as it was, when the column
 * is not one whose keys are byte strings in byte order, or when the query
 * would grow past LION_MAX_QUERY_KEYS lexemes; the caller then answers the
 * prefix the way it did before, by every row and a recheck.
 */
Datum
lion_tsquery_expand_prefixes(Relation index, LionState *col, Datum query,
							 bool *expanded)
{
	TSQuery		tsq = DatumGetTSQuery(query);
	QueryItem  *items = GETQUERY(tsq);
	QTNode	   *root;
	int			budget;
	int			nvals = 0;
	int32		i;

	*expanded = true;
	if (!lion_tsquery_has_prefix(query))
		return query;

	if (!col->multikey || !col->ordered || col->typid != TEXTOID ||
		col->collation != C_COLLATION_OID)
	{
		*expanded = false;
		return query;
	}

	for (i = 0; i < tsq->size; i++)
	{
		if (items[i].type == QI_VAL)
			nvals++;
	}
	budget = LION_MAX_QUERY_KEYS - nvals;
	if (budget < 0)
	{
		*expanded = false;
		return query;
	}

	root = QT2QTN(items, GETOPERAND(tsq));
	if (!lion_qtn_expand(index, col, root, &budget))
	{
		*expanded = false;
		return query;
	}
	return PointerGetDatum(QTN2QT(root));
}

/*
 * The planner's stand-in for lion_tsquery_expand_prefixes(), which needs the
 * index: each `foo:*` becomes the lexeme `foo` at the same weight.  The OR
 * the executor will put in its place combines like one lexeme, so the
 * question "how do the key sets answer this query" has the same answer for
 * both, except when the expansion turns out too long, which the executor
 * handles (it answers the original query by a recheck).
 */
Datum
lion_tsquery_strip_prefixes(Datum query)
{
	TSQuery		tsq;
	TSQuery		out;
	QueryItem  *items;
	int32		i;

	if (!lion_tsquery_has_prefix(query))
		return query;
	tsq = DatumGetTSQuery(query);
	out = (TSQuery) palloc(VARSIZE(tsq));
	memcpy(out, tsq, VARSIZE(tsq));
	items = GETQUERY(out);
	for (i = 0; i < out->size; i++)
	{
		if (items[i].type == QI_VAL)
			items[i].qoperand.prefix = false;
	}
	return PointerGetDatum(out);
}
