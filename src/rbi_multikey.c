/*-------------------------------------------------------------------------
 *
 * rbi_multikey.c
 *		Multi-key opclasses for the roaring index: arrays and tsvector
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
 * into a boolean TREE (RBIKeyNode) that the set algebra in rbi_count.c can
 * evaluate, and falls back to "scan everything and recheck" for any query
 * shape that a plain AND/OR of key sets cannot express.
 *
 * The three answers a query can produce are RBI_QMODE_NONE (no rows),
 * RBI_QMODE_KEYS (exactly the rows the tree selects) and RBI_QMODE_ALL (every
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

#include "rbi.h"

/*
 * Extracting more keys than this from one query is refused rather than
 * answered: every key needs a posting set, and every posting set may hold a
 * buffer pin for as long as the scan runs (the DESIGN.md §9 rule that the
 * count pushdown rests on, and the same pin budget applies to a bitmap scan
 * that walks several chains at once).  A query with a thousand lexemes is
 * also exactly the case where a sequential scan does well.  Such a query
 * falls back to RBI_QMODE_ALL, which is correct, not an error.
 */
#define RBI_MAX_QUERY_KEYS		1000

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
#define RBI_GIN_OVERLAP		1
#define RBI_GIN_CONTAINS	2
#define RBI_GIN_CONTAINED	3
#define RBI_GIN_TSMATCH		1	/* tsvector_ops: @@ is GIN strategy 1 */

static StrategyNumber
rbi_gin_strategy(StrategyNumber strategy)
{
	switch (strategy)
	{
		case RBI_STRAT_CONTAINS:
			return RBI_GIN_CONTAINS;
		case RBI_STRAT_OVERLAP:
			return RBI_GIN_OVERLAP;
		case RBI_STRAT_CONTAINED:
			return RBI_GIN_CONTAINED;
		case RBI_STRAT_MATCH:
			return RBI_GIN_TSMATCH;
	}

	elog(ERROR, "roaring index: strategy %d is not a multi-key strategy",
		 strategy);
	return 0;					/* keep the compiler quiet */
}

/* ---------------------------------------------------------------------
 * Key trees
 * --------------------------------------------------------------------- */

static RBIKeyNode *
rbi_keynode_leaf(int keyno)
{
	RBIKeyNode *n = (RBIKeyNode *) palloc0(sizeof(RBIKeyNode));

	n->kind = RBI_KN_KEY;
	n->keyno = keyno;
	return n;
}

/*
 * An AND or OR over nargs children, which the node takes ownership of.  A
 * one-child operator is folded away: the evaluator would handle it, but the
 * tree is easier to read in a debugger without it.
 */
static RBIKeyNode *
rbi_keynode_op(RBIKeyNodeKind kind, RBIKeyNode **args, int nargs)
{
	RBIKeyNode *n;

	Assert(kind == RBI_KN_AND || kind == RBI_KN_OR);
	Assert(nargs >= 1);

	if (nargs == 1)
		return args[0];

	n = (RBIKeyNode *) palloc0(sizeof(RBIKeyNode));
	n->kind = kind;
	n->nargs = nargs;
	n->args = args;
	return n;
}

/* A flat AND (or OR) over keys 0 .. nkeys-1. */
static RBIKeyNode *
rbi_keynode_flat(RBIKeyNodeKind kind, int nkeys)
{
	RBIKeyNode **args;
	int			i;

	Assert(nkeys >= 1);
	args = (RBIKeyNode **) palloc(sizeof(RBIKeyNode *) * nkeys);
	for (i = 0; i < nkeys; i++)
		args[i] = rbi_keynode_leaf(i);

	return rbi_keynode_op(kind, args, nkeys);
}

/* ---------------------------------------------------------------------
 * extractValue
 * --------------------------------------------------------------------- */

/*
 * One extracted key's hash and its position in the extraction function's
 * array, sorted by hash so that equal keys become adjacent.  The position is
 * the tie-break, so the order is deterministic whatever qsort does with equal
 * elements.
 */
typedef struct RBIHashPos
{
	uint32		hash;
	int32		pos;
} RBIHashPos;

static int
rbi_hashpos_cmp(const void *a, const void *b)
{
	const RBIHashPos *x = (const RBIHashPos *) a;
	const RBIHashPos *y = (const RBIHashPos *) b;

	if (x->hash < y->hash)
		return -1;
	if (x->hash > y->hash)
		return 1;
	return (x->pos < y->pos) ? -1 : ((x->pos > y->pos) ? 1 : 0);
}

/*
 * Extract the keys of one indexed value.
 *
 * NULL keys are dropped: a row whose array holds a NULL element is indexed
 * under its other elements, and `@>`/`&&` with a NULL on either side is
 * answered by a rechecking scan (see rbi_extract_query()), never from the
 * posting sets, so nothing looks for a NULL key.  Duplicates are dropped too,
 * because a posting set is a set and adding the same TID twice would make
 * rbi_container_add() report "already indexed" and leave ntids wrong.
 *
 * Deduplication needs no ordering operator the key type may not have (the
 * opclass only promises a hash and an equality): every key is hashed once and
 * the keys are sorted BY THEIR HASH, which puts the only candidates for
 * equality - the keys with the same hash - next to each other.  Each key is
 * then compared with the equality proc against the distinct keys of its own
 * hash run only, which is one key in every case but a hash collision.  So a
 * tsvector of n lexemes costs n hashes, one sort and about n equality calls,
 * rather than the n^2/2 hash comparisons the first implementation of this
 * function did (a three-hundred-lexeme document: 45000 of them).
 *
 * The keys come out in hash order rather than in the order the extraction
 * function returned them.  Nothing depends on the order: each key is inserted
 * into its own bucket, and the build sorts by (hash, code) anyway.
 *
 * Returns the number of distinct non-NULL keys and puts them in *keys, which
 * is palloc'd in the current context (NULL when there are none).  Zero means
 * the row belongs in the reserved EMPTY entry (DESIGN.md §17).
 */
int
rbi_extract_value(RBIState *state, Datum value, Datum **keys)
{
	Datum	   *raw;
	int32		nraw = 0;
	bool	   *nulls = NULL;
	Datum	   *out;
	RBIHashPos *ord;
	int			nlive = 0;
	int			nout = 0;
	int			runstart = 0;	/* where this hash's distinct keys start */
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

	/* Hash every non-NULL key once, then sort those hashes. */
	ord = (RBIHashPos *) palloc(sizeof(RBIHashPos) * nraw);
	for (i = 0; i < nraw; i++)
	{
		if (nulls != NULL && nulls[i])
			continue;
		ord[nlive].hash = rbi_hash_key(state, raw[i]);
		ord[nlive].pos = i;
		nlive++;
	}

	if (nlive == 0)
	{
		pfree(ord);
		return 0;
	}
	if (nlive > 1)
		qsort(ord, (size_t) nlive, sizeof(RBIHashPos), rbi_hashpos_cmp);

	out = (Datum *) palloc(sizeof(Datum) * nlive);

	for (i = 0; i < nlive; i++)
	{
		Datum		key = raw[ord[i].pos];
		bool		dup = false;

		/* A new hash value starts a new run of possible equals. */
		if (i > 0 && ord[i].hash != ord[i - 1].hash)
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
static RBIKeyNode *
rbi_tsquery_tree(QueryItem *items, int32 size, int32 i, const int *map,
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
		return rbi_keynode_leaf(map[i]);
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
				RBIKeyNode **args;

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

				args = (RBIKeyNode **) palloc(sizeof(RBIKeyNode *) * 2);

				/* Right operand first, exactly as ts_type.h lays them out. */
				args[1] = rbi_tsquery_tree(items, size, i + 1, map, ok);
				args[0] = rbi_tsquery_tree(items, size, i + (int32) left,
										   map, ok);
				if (!*ok)
					return NULL;
				return rbi_keynode_op(item->qoperator.oper == OP_AND ?
									  RBI_KN_AND : RBI_KN_OR, args, 2);
			}

		default:
			/* OP_NOT, OP_PHRASE */
			*ok = false;
			return NULL;
	}
}

/*
 * The tsquery half of rbi_extract_query(): turn the query into a tree over
 * the keys extractQuery returned, or give up.
 */
static RBIKeyNode *
rbi_tsquery_plan(Datum query, int nkeys)
{
	TSQuery		tsq = DatumGetTSQuery(query);
	QueryItem  *items;
	int		   *map;
	int32		i;
	int32		j = 0;
	bool		ok = true;
	RBIKeyNode *tree;

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

	tree = rbi_tsquery_tree(items, tsq->size, 0, map, &ok);
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
 * sets selects" becomes RBI_QMODE_ALL: a partial (prefix) match, because the
 * keys are hashed and a range of them cannot be walked; a NULL key, because
 * no row is indexed under one; INCLUDE_EMPTY and ALL, because the rows a
 * multi-key opclass extracted nothing from are not under any key.
 */
void
rbi_extract_query(RBIState *state, Datum query, StrategyNumber strategy,
				  RBIQuery *q)
{
	int32		nkeys = 0;
	bool	   *pmatch = NULL;
	Pointer    *extra_data = NULL;
	bool	   *nulls = NULL;
	int32		searchMode = GIN_SEARCH_MODE_DEFAULT;
	Datum	   *keys;
	int			i;

	Assert(state->multikey);

	memset(q, 0, sizeof(RBIQuery));

	keys = (Datum *) DatumGetPointer(FunctionCall7Coll(&state->extractquery,
													   state->collation,
													   query,
													   PointerGetDatum(&nkeys),
													   UInt16GetDatum(rbi_gin_strategy(strategy)),
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
		q->mode = RBI_QMODE_ALL;
		return;
	}

	if (nkeys <= 0 || keys == NULL)
	{
		/*
		 * DEFAULT mode with no keys: nothing can match.  `tags && '{}'` and
		 * an empty tsquery both land here.
		 */
		q->mode = RBI_QMODE_NONE;
		return;
	}

	if (nkeys > RBI_MAX_QUERY_KEYS)
	{
		q->mode = RBI_QMODE_ALL;
		return;
	}

	for (i = 0; i < nkeys; i++)
	{
		if ((pmatch != NULL && pmatch[i]) || (nulls != NULL && nulls[i]))
		{
			q->mode = RBI_QMODE_ALL;
			return;
		}
	}

	q->nkeys = nkeys;
	q->keys = keys;

	switch (strategy)
	{
		case RBI_STRAT_CONTAINS:
			/* every element of the query array must be present */
			q->tree = rbi_keynode_flat(RBI_KN_AND, nkeys);
			break;

		case RBI_STRAT_OVERLAP:
			/* at least one of them */
			q->tree = rbi_keynode_flat(RBI_KN_OR, nkeys);
			break;

		case RBI_STRAT_MATCH:
			q->tree = rbi_tsquery_plan(query, nkeys);
			break;

		case RBI_STRAT_CONTAINED:

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
		q->mode = RBI_QMODE_ALL;
		q->nkeys = 0;
		q->keys = NULL;
		return;
	}

	q->mode = RBI_QMODE_KEYS;
}
