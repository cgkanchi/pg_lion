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
 * into a boolean TREE (LionKeyNode) that the set algebra in lion_count.c can
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
#include "miscadmin.h"
#include "tsearch/ts_type.h"
#include "utils/datum.h"
#include "utils/rel.h"

#include "lion.h"

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
int
lion_extract_value(LionState *state, Datum value, Datum **keys)
{
	Datum	   *raw;
	int32		nraw = 0;
	bool	   *nulls = NULL;
	Datum	   *out;
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
	if (nraw <= 0 || raw == NULL)
		return 0;

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

	out = (Datum *) palloc(sizeof(Datum) * nlive);

	for (i = 0; i < nlive; i++)
	{
		Datum		key = raw[ord[i].pos];
		bool		dup = false;

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
			out[nout++] = key;

		CHECK_FOR_INTERRUPTS();
	}

	pfree(ord);

	Assert(nout > 0);
	*keys = out;
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
 *	- a prefix lexeme (`foo:*`) matches a RANGE of keys, and entries are
 *	  hashed, so there is no range to walk;
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
 * Extract a query and decide how its keys combine (DESIGN.md §17).
 *
 * The GIN contract is followed to the letter: searchMode starts at
 * GIN_SEARCH_MODE_DEFAULT, an out-of-range answer is treated as
 * GIN_SEARCH_MODE_ALL (ginNewScanKey() does the same), and pmatch/nullFlags
 * are only read when extractQuery set them.
 *
 * Anything that is not "the rows are exactly the ones an AND/OR of whole key
 * sets selects" becomes LION_QMODE_ALL: a partial (prefix) match, because the
 * keys are hashed and a range of them cannot be walked; a NULL key, because
 * no row is indexed under one; INCLUDE_EMPTY and ALL, because the rows a
 * multi-key opclass extracted nothing from are not under any key.
 */
void
lion_extract_query(LionState *state, Datum query, StrategyNumber strategy,
				  LionQuery *q)
{
	int32		nkeys = 0;
	bool	   *pmatch = NULL;
	Pointer    *extra_data = NULL;
	bool	   *nulls = NULL;
	int32		searchMode = GIN_SEARCH_MODE_DEFAULT;
	Datum	   *keys;
	int			i;

	Assert(state->multikey);

	memset(q, 0, sizeof(LionQuery));

	keys = (Datum *) DatumGetPointer(FunctionCall7Coll(&state->extractquery,
													   state->collation,
													   query,
													   PointerGetDatum(&nkeys),
													   UInt16GetDatum(lion_gin_strategy(strategy)),
													   PointerGetDatum(&pmatch),
													   PointerGetDatum(&extra_data),
													   PointerGetDatum(&nulls),
													   PointerGetDatum(&searchMode)));

	if (searchMode < GIN_SEARCH_MODE_DEFAULT ||
		searchMode > GIN_SEARCH_MODE_ALL)
		searchMode = GIN_SEARCH_MODE_ALL;

	if (searchMode != GIN_SEARCH_MODE_DEFAULT)
	{
		/* INCLUDE_EMPTY and ALL both mean "every indexed row, then recheck". */
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
			/* every element of the query array must be present */
			q->tree = lion_keynode_flat(LION_KN_AND, nkeys);
			break;

		case LION_STRAT_OVERLAP:
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
