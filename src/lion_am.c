/*-------------------------------------------------------------------------
 *
 * lion_am.c
 *		Access method handler, reloptions, opclass validation, cost estimate
 *		and ambuildempty for the lion index.  See DESIGN.md section 6.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/amapi.h"
#include "access/amvalidate.h"
#include "access/generic_xlog.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/tableam.h"
#include "access/xloginsert.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "storage/bufmgr.h"
#include "storage/indexfsm.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/spccache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "lion.h"
#include "lion_count.h"

#if PG_VERSION_NUM >= 180000
PG_MODULE_MAGIC_EXT(
					.name = "pg_lion",
					.version = PG_VERSION
);
#else
PG_MODULE_MAGIC;
#endif

PG_FUNCTION_INFO_V1(lion_handler);

/*
 * Strategies and support procedure numbers live in lion.h, because build,
 * insert, scan and count all need them.  The multi-key strategies of
 * DESIGN.md §17 are 2 .. 5; a scalar opclass has strategy 1, and an ordered
 * one the range strategies 6 .. 9 of §28 as well.
 */
#define LION_MULTI_STRATEGY_MASK \
	((1 << LION_STRAT_CONTAINS) | (1 << LION_STRAT_OVERLAP) | \
	 (1 << LION_STRAT_CONTAINED) | (1 << LION_STRAT_MATCH))
#define LION_RANGE_STRATEGY_MASK \
	((1 << LION_STRAT_LT) | (1 << LION_STRAT_LE) | \
	 (1 << LION_STRAT_GE) | (1 << LION_STRAT_GT))

/* Kind of relation options for lion indexes */
static relopt_kind lion_relopt_kind;

static const relopt_parse_elt lion_relopt_tab[] = {
	{"buckets", RELOPT_TYPE_INT, offsetof(LionOptions, buckets)},
	{"inline_limit", RELOPT_TYPE_INT, offsetof(LionOptions, inline_limit)},
	{"max_entries", RELOPT_TYPE_INT, offsetof(LionOptions, max_entries)},
	{"fillfactor", RELOPT_TYPE_INT, offsetof(LionOptions, fillfactor)},
	{"wal_mode", RELOPT_TYPE_ENUM, offsetof(LionOptions, wal_mode)}
};

/* DESIGN.md §25: which WAL logger a new index is built for. */
static relopt_enum_elt_def lion_wal_mode_options[] = {
	{"auto", LION_WALOPT_AUTO},
	{"generic", LION_WALOPT_GENERIC},
	{"rmgr", LION_WALOPT_RMGR},
	{(const char *) NULL}
};

/*
 * Module initialisation: register the reloptions of the roaring AM, the
 * count-pushdown GUC and the planner hook that plants the LionCount
 * CustomScan (DESIGN.md section 10, implemented in lion_customscan.c).
 *
 * This runs the first time the library is loaded into a backend, which for
 * any query over a lion index happens in get_relation_info() when the
 * planner opens the index and fetches its handler - well before
 * create_upper_paths_hook is consulted for that same query.
 */
void
_PG_init(void)
{
	lion_relopt_kind = add_reloption_kind();

	/*
	 * `buckets` is accepted and ignored since format 4 (DESIGN.md §21): the
	 * hash directory it sized is gone, and refusing the option outright would
	 * break every CREATE INDEX script that sets it.  lionoptions() says so
	 * once, when the option is being set rather than merely read back.
	 */
	add_int_reloption(lion_relopt_kind, "buckets",
					  "Ignored since format 4; the entry directory is a B-tree",
					  0, 0, 65536,
					  AccessExclusiveLock);
	add_int_reloption(lion_relopt_kind, "fillfactor",
					  "Percentage of a directory leaf ambuild fills",
					  LION_DEFAULT_FILLFACTOR, LION_MIN_FILLFACTOR, 100,
					  AccessExclusiveLock);
	add_int_reloption(lion_relopt_kind, "inline_limit",
					  "Maximum size in bytes of a posting set kept inside its entry tuple",
					  LION_DEFAULT_INLINE_LIMIT, LION_MIN_INLINE_LIMIT,
					  LION_MAX_INLINE_LIMIT,
					  AccessExclusiveLock);
	add_int_reloption(lion_relopt_kind, "max_entries",
					  "Distinct keys above which the index warns once per backend (0 disables)",
					  LION_DEFAULT_MAX_ENTRIES, 0, INT_MAX,
					  ShareUpdateExclusiveLock);

	/*
	 * DESIGN.md §25.  The default is "auto" rather than "rmgr" so that one
	 * CREATE INDEX script works on a cluster that preloads this library and
	 * on one that does not; asking for "rmgr" by name on a server without the
	 * resource manager is an ERROR with the preload hint.  The mode is read
	 * once, at build time, and recorded on the meta page - so it is REINDEX
	 * that moves an index from one mode to the other.
	 */
	add_enum_reloption(lion_relopt_kind, "wal_mode",
					   "Which WAL logger this index is built for",
					   lion_wal_mode_options, LION_WALOPT_AUTO,
					   "Valid values are \"auto\", \"generic\" and \"rmgr\".",
					   AccessExclusiveLock);

	/*
	 * The resource manager itself, which only registers while
	 * shared_preload_libraries is being processed (DESIGN.md §25).  The GUC
	 * it takes its id from, pg_lion.rmgr_id, is a postmaster setting, which
	 * core refuses to define once the postmaster is up, so it is defined only
	 * then as well: on a server without the preload SHOW pg_lion.rmgr_id is
	 * an unknown setting.  lion_wal_init() says which of its GUCs are defined
	 * either way.
	 */
	lion_wal_init();

	/*
	 * DESIGN.md §29.3: the least memory a plain scan's window of container
	 * keys takes, whatever work_mem says.  A testing knob more than a tuning
	 * one: every window walks the entries again, which is why the default is
	 * wide, and the regression suite lowers it to cross window boundaries on
	 * a table of a few megabytes.
	 */
	DefineCustomIntVariable("pg_lion.scan_window_floor",
							"Least memory a window of a plain lion index scan takes.",
							"Every window walks the index entries again, so below this a window does not shrink with work_mem.",
							&lion_scan_window_floor,
							LION_SCAN_WINDOW_FLOOR, 64, MAX_KILOBYTES,
							PGC_USERSET,
							GUC_UNIT_KB | GUC_NOT_IN_SAMPLE,
							NULL, NULL, NULL);

	DefineCustomBoolVariable("pg_lion.enable_count_pushdown",
							 "Answer count(*) over lion indexes from the index and the visibility map.",
							 NULL,
							 &lion_enable_count_pushdown,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	/*
	 * The LionOrdered CustomScan (DESIGN.md §30): its GUC, its scan methods
	 * and the set_rel_pathlist_hook that offers it, chained like the other.
	 */
	lion_ordered_init();

	MarkGUCPrefixReserved("pg_lion");

	lion_prev_create_upper_paths_hook = create_upper_paths_hook;
	create_upper_paths_hook = lion_create_upper_paths;
}

/*
 * Handler function: return the IndexAmRoutine of the roaring AM.
 */
Datum
lion_handler(PG_FUNCTION_ARGS)
{
	static const IndexAmRoutine amroutine = {
		.type = T_IndexAmRoutine,
		.amstrategies = LION_NSTRATEGIES,
		.amsupport = LION_NPROC,
		.amoptsprocnum = 0,
		.amcanorder = false,
		.amcanorderbyop = false,
#if PG_VERSION_NUM >= 180000
		.amcanhash = false,

		/*
		 * equality_ops_are_compatible() trusts two operators that share a
		 * family of such an AM to agree on equality.  Every EQUALITY operator
		 * of a roaring opfamily does: a scalar family's equality operators
		 * (cross-type ones included) are core's, from one btree family, and
		 * its range operators - strategies 6 .. 9, DESIGN.md §28 - are
		 * btree's too and order values consistently with that equality; a
		 * multi-key family holds containment/match operators and no equality
		 * at all, so the question is never asked about two of those.
		 * Ordering is not claimed: lion is not an ordered AM (amcanorder is
		 * false, §29.8), and the btree families these operators come from
		 * already answer comparison_ops_are_compatible() for them.
		 */
		.amconsistentequality = true,
		.amconsistentordering = false,
#endif
		.amcanbackward = false,
		.amcanunique = false,
		.amcanmulticol = true,	/* DESIGN.md §24 */
		/*
		 * DESIGN.md §24: the key columns are INDEPENDENT key sets, so a query
		 * that constrains only the second one is as good as one that
		 * constrains the first - which is what this flag tells the planner,
		 * exactly as GIN does.  It also lets a PARTIAL index whose predicate
		 * the query implies be scanned with no quals at all, and
		 * liongetbitmap() answers that by emitting every row the index holds.
		 */
		.amoptionalkey = true,
		.amsearcharray = true,
		.amsearchnulls = true,
		/* multi-key opclasses store the KEY type, not the column type (§17) */
		.amstorage = true,
		.amclusterable = false,
		.ampredlocks = false,
		.amcanparallel = false,
#if PG_VERSION_NUM >= 170000
		.amcanbuildparallel = false,
#endif
		.amcaninclude = false,
		.amusemaintenanceworkmem = true,
		.amsummarizing = false,
		/*
		 * ambulkdelete may run in a parallel vacuum worker (DESIGN.md §11,
		 * §18): one index is still vacuumed start to finish by one process
		 * under the same protocol, and the heap is only marked all-visible
		 * once every index is done.  amvacuumcleanup stays with the leader -
		 * it vacuums the free space map and nothing else, which is not
		 * worth a worker.
		 */
		.amparallelvacuumoptions = VACUUM_OPTION_PARALLEL_BULKDEL,
		.amkeytype = InvalidOid,

		.ambuild = lionbuild,
		.ambuildempty = lionbuildempty,
		.aminsert = lioninsert,
#if PG_VERSION_NUM >= 170000
		.aminsertcleanup = NULL,
#endif
		.ambulkdelete = lionbulkdelete,
		.amvacuumcleanup = lionvacuumcleanup,
		.amcanreturn = NULL,
		.amcostestimate = lioncostestimate,
#if PG_VERSION_NUM >= 180000
		.amgettreeheight = NULL,
#endif
		.amoptions = lionoptions,
		.amproperty = NULL,
		.ambuildphasename = NULL,
		.amvalidate = lionvalidate,
		.amadjustmembers = NULL,
		.ambeginscan = lionbeginscan,
		.amrescan = lionrescan,
		.amgettuple = liongettuple,	/* DESIGN.md §29 */
		.amgetbitmap = liongetbitmap,
		.amendscan = lionendscan,
		.ammarkpos = NULL,
		.amrestrpos = NULL,
		.amestimateparallelscan = NULL,
		.aminitparallelscan = NULL,
		.amparallelrescan = NULL,
#if PG_VERSION_NUM >= 180000
		.amtranslatestrategy = NULL,
		.amtranslatecmptype = NULL,
#endif
	};

#if PG_VERSION_NUM >= 190000
	PG_RETURN_POINTER(&amroutine);
#else
	{
		/*
		 * Before 19 the caller owns the result and pfree()s it after copying
		 * it into the relcache, so it has to be palloc'd.
		 */
		IndexAmRoutine *copy = makeNode(IndexAmRoutine);

		*copy = amroutine;
		PG_RETURN_POINTER(copy);
	}
#endif
}

/*
 * Parse reloptions.
 */
bytea *
lionoptions(Datum reloptions, bool validate)
{
	LionOptions *opts;

	opts = (LionOptions *) build_reloptions(reloptions, validate,
											lion_relopt_kind,
											sizeof(LionOptions),
											lion_relopt_tab,
											lengthof(lion_relopt_tab));

	/*
	 * DESIGN.md §21: the hash directory `buckets` sized no longer exists.  The
	 * option is still parsed so that existing DDL keeps working, and setting
	 * it to anything says so.  validate is true only when the option is being
	 * set, not when the relcache reads it back.
	 */
	if (validate && opts != NULL && opts->wal_mode == LION_WALOPT_RMGR &&
		!lion_rmgr_registered())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("wal_mode = rmgr needs the pg_lion WAL resource manager, which this server has not registered"),
				 errhint("Add \"pg_lion\" to shared_preload_libraries and restart the server, or leave wal_mode at \"auto\".")));

	if (validate && opts != NULL && opts->buckets > 0)
		ereport(NOTICE,
				(errmsg("buckets is ignored since format 4"),
				 errdetail("The entry directory is a B-tree keyed by the index key; it grows by splitting."),
				 errhint("Use fillfactor to control how full ambuild packs its leaves.")));

	return (bytea *) opts;
}

/*
 * Peel binary-coercion relabels off an expression, so that a varchar column
 * compared with a text constant presents the constant the opclass will see.
 */
static Node *
lion_cost_strip(Node *node)
{
	while (node != NULL && IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;
	return node;
}

/*
 * Does this index's opclass extract many keys from one value (DESIGN.md §17)?
 * The presence of support function 2 is the same test lion_fill_state() makes.
 */
static bool
lion_index_is_multikey(IndexOptInfo *index, int col)
{
	return OidIsValid(get_opfamily_proc(index->opfamily[col],
										index->opcintype[col],
										index->opcintype[col],
										LION_EXTRACTVALUE_PROC));
}

/*
 * Extract one multi-key query at plan time and say whether answering it means
 * emitting every posting of every entry (DESIGN.md §17's LION_QMODE_ALL).
 *
 * lion_extract_query() wants an LionState, but only for the extractQuery
 * FmgrInfo and the collation; nothing here touches an index.  A missing
 * extraction function means the question cannot be answered, which is costed
 * as the expensive answer.
 */
static bool
lion_query_is_full_scan(IndexOptInfo *index, int col, StrategyNumber strategy,
					   Datum query)
{
	Oid			proc;
	FmgrInfo	flinfo;
	LionState	state;
	LionQuery	q;
	MemoryContext cxt;
	MemoryContext oldcxt;
	bool		full;

	proc = get_opfamily_proc(index->opfamily[col], index->opcintype[col],
							 index->opcintype[col], LION_EXTRACTQUERY_PROC);
	if (!OidIsValid(proc))
		return true;

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"roaring cost query extract",
								ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	memset(&state, 0, sizeof(state));
	state.multikey = true;
	state.collation = index->indexcollations[col];
	fmgr_info(proc, &flinfo);
	state.extractquery = flinfo;

	lion_extract_query(&state, query, strategy, &q);
	full = (q.mode == LION_QMODE_ALL);

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);

	return full;
}

/*
 * `col op ANY (array)`: each element is a query of its own and their answers
 * go into the same bitmap, so one element that needs the whole index makes
 * the scan a full one.  An array that is not available at plan time has to be
 * assumed to contain such an element.
 */
static bool
lion_array_query_is_full_scan(IndexOptInfo *index, int col,
							 StrategyNumber strategy, Node *arraynode)
{
	Const	   *con = (Const *) arraynode;
	ArrayType  *arr;
	Oid			elemtype;
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	Datum	   *elems;
	bool	   *nulls;
	int			nelems;
	int			i;
	bool		full = false;

	if (arraynode == NULL || !IsA(arraynode, Const))
		return true;
	if (con->constisnull)
		return false;			/* `col op ANY (NULL)` is never true */

	arr = DatumGetArrayTypeP(con->constvalue);
	elemtype = ARR_ELEMTYPE(arr);
	get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
	deconstruct_array(arr, elemtype, elmlen, elmbyval, elmalign,
					  &elems, &nulls, &nelems);

	for (i = 0; i < nelems && !full; i++)
	{
		if (nulls[i])
			continue;			/* never true, nothing is scanned for it */
		full = lion_query_is_full_scan(index, col, strategy, elems[i]);
	}

	pfree(elems);
	pfree(nulls);
	if ((Pointer) arr != DatumGetPointer(con->constvalue))
		pfree(arr);

	return full;
}

/*
 * Is this index qual one of the range comparisons of DESIGN.md §28 - `<`,
 * `<=`, `>=` or `>`, strategies 6 .. 9 of a SCALAR column's opfamily?
 */
static bool
lion_cost_is_range_op(IndexOptInfo *index, int col, Oid opno)
{
	return !lion_index_is_multikey(index, col) &&
		LION_STRAT_IS_RANGE(get_op_opfamily_strategy(opno,
													 index->opfamily[col]));
}

static bool
lion_cost_is_range(IndexOptInfo *index, int col, OpExpr *op)
{
	return lion_cost_is_range_op(index, col, op->opno);
}

/*
 * Will liongetbitmap() have to walk the WHOLE index for this path - read every
 * bucket, every entry and every posting of every entry?
 *
 * liongetbitmap() answers ONE qual per scan and marks the rest for recheck,
 * choosing the most selective-looking one: a plain operator first, then a
 * ScalarArrayOp, then a range (DESIGN.md §28), then a null test.  The cost of
 * the scan is the cost of the qual it answers, so the choice is mirrored
 * here.  A range walks only the entries between its bounds, so it never
 * makes the scan a full one; what it costs per entry is
 * lion_range_entry_cost()'s.
 *
 * A full walk is what the AM does for:
 *
 *	- `col IS NOT NULL`, which is every entry but the reserved NULL one
 *	  (DESIGN.md §14);
 *	- a multi-key query the extractor answers with LION_QMODE_ALL: a phrase, a
 *	  prefix, a NOT, a weight mask, `<@`, `@> '{}'`, a NULL element, or more
 *	  than LION_MAX_QUERY_KEYS keys (DESIGN.md §17).  Correctness is preserved
 *	  by the recheck, but the scan reads the whole index and hands the heap
 *	  every indexed row;
 *	- and a multi-key query whose value is not a plan-time Const (a Param):
 *	  the MODE follows the query's shape, not just its value, so an unknown
 *	  value has to be priced as the expensive shape.
 *
 * *emits_all_rows additionally says whether the CANDIDATES the walk produces
 * are every indexed row, which is what makes the heap side a full recheck
 * rather than a selective fetch.  The multi-key fallback asks for a superset
 * and the operator is re-applied to every row; `IS NOT NULL` does not - the
 * rows it emits are exactly the rows it selects, so its heap side is still
 * the clause's own selectivity.
 *
 * Handing genericcostestimate() an empty GenericCosts instead priced these
 * as selective lookups - a prefix query estimated at 192 cost units against
 * the sequential scan's 9156, for a scan that emitted 1.18M posting TIDs and
 * rechecked 198k rows and ran 2.6x slower than that sequential scan (the
 * 2026-09-21 follow-up review).
 */
static bool
lion_scan_walks_whole_index(IndexPath *path, bool *emits_all_rows)
{
	IndexOptInfo *index = path->indexinfo;
	Node	   *chosen[INDEX_MAX_KEYS];
	int			bestrank[INDEX_MAX_KEYS];
	int			ncols = index->nkeycolumns;
	int			nchosen = 0;
	int			firstcol = -1;
	bool		anyselective = false;
	ListCell   *lc;
	int			c;

	*emits_all_rows = false;

	if (ncols < 1 || ncols > INDEX_MAX_KEYS)
		return false;

	for (c = 0; c < ncols; c++)
	{
		chosen[c] = NULL;
		bestrank[c] = 4;
	}

	foreach(lc, path->indexclauses)
	{
		IndexClause *iclause = (IndexClause *) lfirst(lc);
		int			col = iclause->indexcol;
		ListCell   *lc2;

		if (col < 0 || col >= ncols)
			continue;

		foreach(lc2, iclause->indexquals)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc2);
			Node	   *clause = (Node *) rinfo->clause;
			int			rank;

			if (IsA(clause, NullTest))
				rank = 3;
			else if (IsA(clause, ScalarArrayOpExpr))
				rank = 1;
			else if (IsA(clause, OpExpr))
				rank = lion_cost_is_range(index, col, (OpExpr *) clause) ? 2 : 0;
			else
				continue;

			if (rank < bestrank[col])
			{
				bestrank[col] = rank;
				chosen[col] = clause;
			}
		}
	}

	/*
	 * Per column, would that column alone make the scan walk the whole
	 * column's entries?  The answers combine the way the scan does
	 * (DESIGN.md §24): the columns are INTERSECTED, so one column that
	 * selects from its posting sets keeps the scan off the full walk however
	 * the others are answered - liongetbitmap() drops those and rechecks.
	 */
	for (c = 0; c < ncols; c++)
	{
		bool		colfull;
		bool		colall = false;
		Node	   *cl = chosen[c];

		if (cl == NULL)
			continue;
		nchosen++;
		if (firstcol < 0)
			firstcol = c;

		if (IsA(cl, NullTest))
			colfull = (((NullTest *) cl)->nulltesttype == IS_NOT_NULL);
		else if (!lion_index_is_multikey(index, c))
		{
			/* A scalar opclass looks ONE key up, whatever the operand. */
			colfull = false;
		}
		else if (IsA(cl, OpExpr))
		{
			OpExpr	   *op = (OpExpr *) cl;
			StrategyNumber strategy;
			Node	   *arg;

			colall = true;
			colfull = true;
			if (list_length(op->args) == 2 &&
				(strategy = (StrategyNumber)
				 get_op_opfamily_strategy(op->opno, index->opfamily[c])) != 0 &&
				(arg = lion_cost_strip((Node *) lsecond(op->args))) != NULL &&
				IsA(arg, Const))
			{
				if (((Const *) arg)->constisnull)
				{
					/* a strict operator: never true, so nothing is scanned */
					colall = false;
					colfull = false;
				}
				else if (!lion_query_is_full_scan(index, c, strategy,
												  ((Const *) arg)->constvalue))
				{
					colall = false;
					colfull = false;
				}
			}
		}
		else
		{
			ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) cl;
			StrategyNumber strategy;

			colall = true;
			colfull = true;
			if (list_length(saop->args) == 2 &&
				(strategy = (StrategyNumber)
				 get_op_opfamily_strategy(saop->opno,
										  index->opfamily[c])) != 0 &&
				(strategy == LION_STRAT_EQUAL ||
				 !lion_array_query_is_full_scan(index, c, strategy,
												lion_cost_strip((Node *) lsecond(saop->args)))))
			{
				/* a union of single-key lookups, or of exact queries */
				colall = false;
				colfull = false;
			}
		}

		if (!colfull)
			anyselective = true;
		else if (c == firstcol)
			*emits_all_rows = colall;
	}

	/*
	 * No clause at all: a partial index whose predicate the query implies,
	 * which liongetbitmap() answers by emitting every row it holds.  The rows
	 * are exactly the ones the scan selects, so the heap side keeps the
	 * predicate's own selectivity (*emits_all_rows stays false).
	 */
	if (nchosen == 0)
		return true;
	if (anyselective)
	{
		*emits_all_rows = false;
		return false;
	}

	return true;
}

/* Does a column other than col have an index qual at all? */
static bool
lion_cost_other_column(IndexPath *path, int col)
{
	ListCell   *lc;

	foreach(lc, path->indexclauses)
	{
		if (((IndexClause *) lfirst(lc))->indexcol != col)
			return true;
	}
	return false;
}

/*
 * Does a column other than col have a qual the scan answers with a set tree
 * (lion_scan_col_tree()), which a range walk on col is then ANDed with?
 */
static bool
lion_cost_sets_beside(IndexPath *path, int col)
{
	ListCell   *lc;

	foreach(lc, path->indexclauses)
	{
		IndexClause *iclause = (IndexClause *) lfirst(lc);
		ListCell   *lc2;

		if (iclause->indexcol == col)
			continue;
		foreach(lc2, iclause->indexquals)
		{
			Node	   *clause = (Node *) lfirst_node(RestrictInfo, lc2)->clause;

			/*
			 * An equality, a list, `IS NULL` or a multi-key query is a set;
			 * another range is a second walk and `IS NOT NULL` is dropped,
			 * neither of which the scan streams beside this one.
			 */
			if (IsA(clause, NullTest) ?
				((NullTest *) clause)->nulltesttype == IS_NULL :
				!((IsA(clause, OpExpr) &&
				   lion_cost_is_range(path->indexinfo, iclause->indexcol,
									  (OpExpr *) clause)) ||
				  (IsA(clause, ScalarArrayOpExpr) &&
				   lion_cost_is_range_op(path->indexinfo, iclause->indexcol,
										 ((ScalarArrayOpExpr *) clause)->opno))))
				return true;
		}
	}
	return false;
}

/*
 * What the ENTRIES of the range walks of this path cost (DESIGN.md §28), on
 * top of the pages and the postings genericcostestimate() prorates by the
 * selectivity.
 *
 * A range is one walk per key column, over the entries between its bounds:
 * about n_distinct(col) x the range's selectivity of them, each decoded and
 * compared with every bound.  On a low- or mid-cardinality column that is a
 * few hundred entries, which is nothing; on a near-unique one every row is
 * an entry, and this is the term - with the index's own size, an entry header
 * per row against btree's tuple - that leaves such a column to btree.  Only
 * a column whose chosen qual IS its range pays it, ranked as liongetbitmap()
 * ranks them: an equality outranks everything, then an `op ANY (array)` -
 * which for a range operator is ONE walk to the widest element
 * (lion_emit_array_range()) and pays for the entries that walk visits - then
 * the column's plain range comparisons, all of them one walk.
 *
 * Beside another column's sets, a plain scan walks a long range once per
 * WINDOW of those sets' containers (DESIGN.md §29.3, lion_walk_window()), so
 * the entries are paid once per window: the heap's container keys over the
 * window, which is 1 below 32768 heap blocks at the default floor and an
 * upper bound above it (the sets may have containers at fewer keys).  The
 * bitmap scan, which shares the path, walks once and is overcharged by that
 * on a heap that large - the UNION's trade in lioncostestimate().  The plain
 * scan used to restart the other columns' stream for every entry instead, a
 * descent of each of their posting trees, and that was never charged at all
 * (2026-09-25 review).
 *
 * *walkrows is what the walks read beyond that, for lioncostestimate() to
 * charge: on a MULTICOLUMN path genericcostestimate() prorates the index by
 * the selectivity of every column's quals together, which is what an AND of
 * set trees - leapfrogging each other (§22) - reads about, but a range
 * column is walked whole whatever the others select (§28, §29.3), so it
 * reads the postings of every row in its range: its own selectivity's share.
 * A one-column path is prorated by that already and adds nothing.
 */
static Cost
lion_range_entry_cost(PlannerInfo *root, IndexPath *path, double *walkrows)
{
	IndexOptInfo *index = path->indexinfo;
	Cost		cost = 0;
	int			c;

	for (c = 0; c < index->nkeycolumns; c++)
	{
		List	   *ranges = NIL;
		RestrictInfo *arrayrange = NULL;
		bool		outranked = false;
		ListCell   *lc;

		foreach(lc, path->indexclauses)
		{
			IndexClause *iclause = (IndexClause *) lfirst(lc);
			ListCell   *lc2;

			if (iclause->indexcol != c)
				continue;
			foreach(lc2, iclause->indexquals)
			{
				RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc2);
				Node	   *clause = (Node *) rinfo->clause;

				if (IsA(clause, OpExpr) &&
					lion_cost_is_range(index, c, (OpExpr *) clause))
					ranges = lappend(ranges, rinfo);
				else if (IsA(clause, ScalarArrayOpExpr) &&
						 lion_cost_is_range_op(index, c,
											   ((ScalarArrayOpExpr *) clause)->opno))
				{
					if (arrayrange == NULL)
						arrayrange = rinfo;
				}
				else if (!IsA(clause, NullTest))
					outranked = true;	/* an equality or a list: no walk */
			}
		}

		/* An array range outranks the plain ones and is one walk alone. */
		if (outranked)
		{
			list_free(ranges);
			ranges = NIL;
		}
		else if (arrayrange != NULL)
		{
			list_free(ranges);
			ranges = list_make1(arrayrange);
		}

		if (ranges != NIL)
		{
			Selectivity sel = clauselist_selectivity(root, ranges,
													 index->rel->relid,
													 JOIN_INNER, NULL);
			double		ndistinct = DEFAULT_NUM_DISTINCT;
			double		entries;

			if (index->indexkeys[c] > 0)
			{
				RangeTblEntry *rte = planner_rt_fetch(index->rel->relid, root);
				VariableStatData vardata;
				bool		isdefault;
				Var		   *var = makeVar(index->rel->relid,
										  index->indexkeys[c],
										  get_atttype(rte->relid,
													  index->indexkeys[c]),
										  -1, index->indexcollations[c], 0);

				examine_variable(root, (Node *) var, index->rel->relid,
								 &vardata);
				ndistinct = get_variable_numdistinct(&vardata, &isdefault);
				ReleaseVariableStats(vardata);
			}

			entries = Max(1.0, ndistinct * sel);
			if (lion_cost_sets_beside(path, c))
			{
				double		containers = ceil((double) index->rel->pages /
											  LION_BLOCKS_PER_CONTAINER);

				entries *= Max(1.0, ceil(containers / lion_walk_window()));
			}
			cost += entries * (cpu_index_tuple_cost +
							   list_length(ranges) * cpu_operator_cost);
			if (lion_cost_other_column(path, c))
				*walkrows += sel * Max(index->rel->tuples, 0.0);
			list_free(ranges);
		}
	}

	return cost;
}

/*
 * The correlation a PLAIN index scan's heap fetches have with the heap order
 * (DESIGN.md §29.11), which only cost_index() reads - a bitmap heap scan
 * sorts its pages whatever the index says.  It is btcostestimate()'s answer:
 * the ANALYZE correlation of the first index column the path has a clause on
 * (of column 1 when it has none), for the type's default `<`, which is what
 * ANALYZE computed it with, times 0.75 for a multicolumn index.  Within one
 * key a lion scan returns TIDs in heap order, as btree does since its heap-TID
 * tiebreaker, so how that key's rows are spread over the heap is what the
 * column's correlation describes.  0 for a multi-key column (a row is under
 * several of its entries and the column's statistics are the ARRAY's), for
 * an expression column, and whenever there is no statistic.
 */
static double
lion_index_correlation(PlannerInfo *root, IndexPath *path)
{
	IndexOptInfo *index = path->indexinfo;
	VariableStatData vardata;
	RangeTblEntry *rte;
	TypeCacheEntry *tce;
	Oid			atttype;
	double		corr = 0.0;
	int			col = -1;
	ListCell   *lc;
	Var		   *var;

	foreach(lc, path->indexclauses)
	{
		IndexClause *iclause = (IndexClause *) lfirst(lc);

		if (col < 0 || iclause->indexcol < col)
			col = iclause->indexcol;
	}
	if (col < 0)
		col = 0;
	if (col >= index->nkeycolumns || index->indexkeys[col] <= 0 ||
		lion_index_is_multikey(index, col))
		return 0.0;

	rte = planner_rt_fetch(index->rel->relid, root);
	atttype = get_atttype(rte->relid, index->indexkeys[col]);
	var = makeVar(index->rel->relid, index->indexkeys[col], atttype, -1,
				  index->indexcollations[col], 0);

	examine_variable(root, (Node *) var, index->rel->relid, &vardata);
	tce = lookup_type_cache(atttype, TYPECACHE_LT_OPR);
	if (HeapTupleIsValid(vardata.statsTuple) && OidIsValid(tce->lt_opr))
	{
		AttStatsSlot sslot;

		if (get_attstatsslot(&sslot, vardata.statsTuple,
							 STATISTIC_KIND_CORRELATION, tce->lt_opr,
							 ATTSTATSSLOT_NUMBERS))
		{
			if (sslot.nnumbers > 0)
				corr = sslot.numbers[0];
			free_attstatsslot(&sslot);
		}
	}
	ReleaseVariableStats(vardata);

	if (index->nkeycolumns > 1)
		corr *= 0.75;
	return corr;
}

/*
 * Cost estimate: the generic estimate, with three corrections and the heap
 * correlation.  A scan that has to walk the whole index is priced as one
 * rather than as the selective lookup its predicate's output selectivity
 * suggests, a range pays for the entries it walks (lion_range_entry_cost()),
 * and a plain index scan's heap side is priced with the column's correlation
 * as btree's is (lion_index_correlation(), DESIGN.md §29.11); a bitmap path,
 * which shares this estimate, does not read that last number.
 */
void
lioncostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
				Cost *indexStartupCost, Cost *indexTotalCost,
				Selectivity *indexSelectivity, double *indexCorrelation,
				double *indexPages)
{
	GenericCosts costs = {0};
	bool		emits_all_rows;
	bool		fullscan = lion_scan_walks_whole_index(path, &emits_all_rows);
	double		walkrows = 0.0;

	/*
	 * A full walk visits every index tuple - every (key, row) posting, which
	 * is what pg_class.reltuples of a lion index counts - and
	 * genericcostestimate() then prorates that into every index page.
	 */
	if (fullscan)
		costs.numIndexTuples = Max(path->indexinfo->tuples, 1.0);

	genericcostestimate(root, path, loop_count, &costs);

	if (fullscan)
	{
		double		allpages = Max((double) path->indexinfo->pages, 1.0);

		/*
		 * reltuples of an index is whatever the last ANALYZE or VACUUM left
		 * there, and ANALYZE writes the HEAP's row count onto every index of
		 * a table, so the prorated page count can come out short of the
		 * index the scan really reads.  Charge the rest of it.
		 */
		if (costs.numIndexPages < allpages)
		{
			costs.indexTotalCost += (allpages - costs.numIndexPages) *
				costs.spc_random_page_cost;
			costs.numIndexPages = allpages;
		}

		/*
		 * And, for the multi-key fallback, nothing is filtered out before the
		 * heap either: every indexed row is emitted as a candidate, fetched
		 * and rechecked there, so the heap side is the cost of a full recheck
		 * and not of the predicate's own selectivity.
		 */
		if (emits_all_rows)
			costs.indexSelectivity = 1.0;
	}

	costs.indexTotalCost += lion_range_entry_cost(root, path, &walkrows);

	/*
	 * A range walked beside other columns reads the postings of its own
	 * range, which genericcostestimate() prorated by every column's
	 * selectivity together (lion_range_entry_cost()): charge the rest of
	 * them, a share of the index's pages and a tuple cost each.  The pages
	 * are those of a walk - directory leaves in key order through their right
	 * links, which ambuild lays out in that order - so they are charged as
	 * sequential reads.  Without the term `a BETWEEN 1 AND 900000 AND b = 5`
	 * over a million unique a was priced at two thirds of the sequential scan
	 * and ran 1.4 to 1.6 times as long as it; charged at random_page_cost,
	 * the 400000-row range went to the sequential scan at 2.5 times the plain
	 * scan's time (2026-09-25 review).
	 */
	if (walkrows > costs.numIndexTuples)
	{
		IndexOptInfo *index = path->indexinfo;
		double		extra = walkrows - costs.numIndexTuples;
		double		pages = ceil(extra * (double) index->pages /
								 Max(index->tuples, 1.0));
		double		spc_random_page_cost;
		double		spc_seq_page_cost;

		get_tablespace_page_costs(index->reltablespace, &spc_random_page_cost,
								  &spc_seq_page_cost);
		pages = Min(pages, Max((double) index->pages - costs.numIndexPages, 0.0));
		costs.indexTotalCost += pages * spc_seq_page_cost +
			extra * cpu_index_tuple_cost;
		costs.numIndexPages += pages;
		costs.numIndexTuples = walkrows;
	}

	/*
	 * A plain scan that reads a multi-key column whole streams the union of
	 * its entries a window of container keys at a time, and walks every entry
	 * again for each window (DESIGN.md §29.3, lion_union_window()): charge
	 * each window past the first another read of the index.  The IndexPath is
	 * shared with the bitmap scan, which reads the index once, so that side
	 * is overcharged - which only matters on a heap of more than 65536 blocks
	 * per window and a qual that reads the whole column anyway.
	 */
	if (fullscan)
	{
		IndexOptInfo *index = path->indexinfo;
		int			col = 0;
		ListCell   *lc;

		foreach(lc, path->indexclauses)
		{
			IndexClause *iclause = (IndexClause *) lfirst(lc);

			if (lc == list_head(path->indexclauses) || iclause->indexcol < col)
				col = iclause->indexcol;
		}
		if (lion_index_is_multikey(index, col))
		{
			double		containers = ceil((double) index->rel->pages /
										  LION_BLOCKS_PER_CONTAINER);
			double		windows = ceil(containers / lion_union_window());

			if (windows > 1.0)
				costs.indexTotalCost += (windows - 1.0) *
					Max((double) index->pages, 1.0) * seq_page_cost;
		}
	}

	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = lion_index_correlation(root, path);
	*indexPages = costs.numIndexPages;
}

/*
 * Before PostgreSQL 18, bool, bytea, date, xid, xid8, cid and timestamptz had
 * no hash function of their own: their hash opclasses used a physically
 * compatible function of another type, and hashvalidate() accepted exactly
 * those pairs.  The extension script names the same functions on those
 * servers (pg_lion--0.1.sql), so the same pairs are accepted here.  They are
 * accepted on EVERY major, not only below 18: pg_upgrade from 16 or 17 carries
 * the old pg_amproc rows onto an 18+ server (DESIGN.md §23 addendum), and the
 * substitutes compute the same hash as the new functions, so those indexes
 * are exactly as valid there.  The function identity is tested rather than
 * its argument type, for hashvalidate()'s reason: hashvarlena() takes
 * `internal`.
 */
static bool
lion_hash_substitution_ok(Oid funcid, Oid argtype)
{
	switch (argtype)
	{
		case DATEOID:
		case XIDOID:
		case CIDOID:
			return funcid == F_HASHINT4;
		case XID8OID:
			return funcid == F_HASHINT8;
		case TIMESTAMPTZOID:
			return funcid == F_TIMESTAMP_HASH;
		case BOOLOID:
			return funcid == F_HASHCHAR;
		case BYTEAOID:
			return funcid == F_HASHVARLENA;
	}
	return false;
}

/*
 * Validator for a roaring opclass.  Modelled on hashvalidate()/ginvalidate().
 *
 * There are two shapes of roaring opclass and they are validated differently
 * (DESIGN.md §6 and §17):
 *
 *	scalar		support proc 1 is a hash function returning int4 and strategy
 *				1 is equality, exactly as a hash opclass.  Its support
 *				function's argument type only has to be binary-coercible from
 *				the opclass input type, so that e.g. a varchar column can use
 *				text_ops, and every type an operator mentions must have a hash
 *				function in the family, which is what makes cross-type
 *				equality usable.  The range comparisons 6 .. 9 of DESIGN.md
 *				§28 are allowed for a type pair that has support function 4.
 *
 *	multi-key	support procs 2 and 3 are GIN's extractValue and extractQuery
 *				and the strategies are 2 .. 5.  The keys are of the STORAGE
 *				type, so proc 1 - when there is one - hashes THAT and not the
 *				opclass input type; a polymorphic STORAGE type (array_ops
 *				stores anyelement) cannot name one function at all and the key
 *				type's default hash opclass is used instead.
 */
bool
lionvalidate(Oid opclassoid)
{
	bool		result = true;
	HeapTuple	classtup;
	Form_pg_opclass classform;
	Oid			opfamilyoid;
	Oid			opcintype;
	Oid			opckeytype;
	char	   *opclassname;
	char	   *opfamilyname;
	CatCList   *proclist,
			   *oprlist;
	List	   *grouplist;
	OpFamilyOpFuncGroup *opclassgroup;
	List	   *hashabletypes = NIL;
	bool		multikey = false;
	bool		haveop = false;
	int			i;
	ListCell   *lc;

	classtup = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclassoid));
	if (!HeapTupleIsValid(classtup))
		elog(ERROR, "cache lookup failed for operator class %u", opclassoid);
	classform = (Form_pg_opclass) GETSTRUCT(classtup);

	opfamilyoid = classform->opcfamily;
	opcintype = classform->opcintype;
	opckeytype = classform->opckeytype;
	if (!OidIsValid(opckeytype))
		opckeytype = opcintype;
	opclassname = NameStr(classform->opcname);

	opfamilyname = get_opfamily_name(opfamilyoid, false);

	oprlist = SearchSysCacheList1(AMOPSTRATEGY, ObjectIdGetDatum(opfamilyoid));
	proclist = SearchSysCacheList1(AMPROCNUM, ObjectIdGetDatum(opfamilyoid));

	/*
	 * Which shape is this?  An extractValue procedure for the opclass's own
	 * type is what makes an opclass multi-key, and it is the same test
	 * lion_fill_state() makes at run time (through index_getprocid()).
	 */
	for (i = 0; i < proclist->n_members; i++)
	{
		Form_pg_amproc procform =
			(Form_pg_amproc) GETSTRUCT(&proclist->members[i]->tuple);

		if (procform->amprocnum == LION_EXTRACTVALUE_PROC &&
			procform->amproclefttype == opcintype &&
			procform->amprocrighttype == opcintype)
			multikey = true;
	}

	/* Check individual support functions */
	for (i = 0; i < proclist->n_members; i++)
	{
		HeapTuple	proctup = &proclist->members[i]->tuple;
		Form_pg_amproc procform = (Form_pg_amproc) GETSTRUCT(proctup);
		bool		ok;

		/*
		 * Only the ordering function of DESIGN.md §21 may be cross-type: it is
		 * what lets a search for an int8 value descend a directory of int4
		 * keys, exactly as btree's own comparison functions do.
		 */
		if (procform->amproclefttype != procform->amprocrighttype &&
			procform->amprocnum != LION_CMP_PROC)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains support function %s with different left and right input types",
							opfamilyname, "lion",
							format_procedure(procform->amproc))));
			result = false;
		}

		switch (procform->amprocnum)
		{
			case LION_HASH_PROC:
				if (multikey)
					ok = check_amproc_signature(procform->amproc, INT4OID,
												false, 1, 1, opckeytype);
				else
					ok = check_amproc_signature(procform->amproc, INT4OID,
												false, 1, 1,
												procform->amproclefttype) ||
						lion_hash_substitution_ok(procform->amproc,
												  procform->amproclefttype);
				break;
			case LION_CMP_PROC:
				/*
				 * The ordering of DESIGN.md §21: a btree comparison of the KEY
				 * type, which for a multi-key class is the STORAGE type.  It
				 * is optional - an index whose key type has no btree opclass
				 * is simply not ordered - and may be cross-type, for the
				 * families that offer cross-type equality.
				 */
				if (multikey)
					ok = check_amproc_signature(procform->amproc, INT4OID,
												true, 2, 2, opckeytype,
												opckeytype);
				else
					ok = check_amproc_signature(procform->amproc, INT4OID,
												true, 2, 2,
												procform->amproclefttype,
												procform->amprocrighttype);
				break;
			case LION_EXTRACTVALUE_PROC:
				/* GIN's extractValue; some opclasses omit nullFlags */
				ok = check_amproc_signature(procform->amproc, INTERNALOID,
											false, 2, 3,
											procform->amproclefttype,
											INTERNALOID, INTERNALOID);
				break;
			case LION_EXTRACTQUERY_PROC:
				/* GIN's extractQuery; some omit nullFlags and searchMode */
				ok = check_amproc_signature(procform->amproc, INTERNALOID,
											false, 5, 7,
											procform->amproclefttype,
											INTERNALOID, INT2OID, INTERNALOID,
											INTERNALOID, INTERNALOID,
											INTERNALOID);
				break;
			default:
				ereport(INFO,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("operator family \"%s\" of access method %s contains function %s with invalid support number %d",
								opfamilyname, "lion",
								format_procedure(procform->amproc),
								procform->amprocnum)));
				result = false;
				continue;		/* don't want additional message */
		}

		/*
		 * The two extraction procedures only make sense together: an opclass
		 * with extractQuery but no extractValue would search for keys it
		 * never stored.  Only the opclass's own type pair can be judged here,
		 * for the same reason ginvalidate() gives.
		 */
		if (procform->amprocnum != LION_HASH_PROC &&
			procform->amprocnum != LION_CMP_PROC && !multikey &&
			procform->amproclefttype == opcintype)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator class \"%s\" of access method %s has support function %s but no support function %d",
							opclassname, "lion",
							format_procedure(procform->amproc),
							LION_EXTRACTVALUE_PROC)));
			result = false;
		}

		if (!ok)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains function %s with wrong signature for support number %d",
							opfamilyname, "lion",
							format_procedure(procform->amproc),
							procform->amprocnum)));
			result = false;
		}
		else if (procform->amprocnum == LION_HASH_PROC && !multikey)
			hashabletypes = list_append_unique_oid(hashabletypes,
												   procform->amproclefttype);
	}

	/* Check individual operators */
	for (i = 0; i < oprlist->n_members; i++)
	{
		HeapTuple	oprtup = &oprlist->members[i]->tuple;
		Form_pg_amop oprform = (Form_pg_amop) GETSTRUCT(oprtup);
		bool		stratok;

		haveop = true;

		/*
		 * A scalar family answers equality and, where it can order its keys,
		 * the range comparisons of DESIGN.md §28 - whether it CAN is a
		 * question about the type pair's support function 4, asked with the
		 * groups below; a multi-key one answers the containment/match
		 * strategies and never equality or a range (the keys of one row are
		 * not the row's value, so neither could be answered from them).
		 */
		stratok = multikey ?
			(oprform->amopstrategy >= 1 &&
			 oprform->amopstrategy <= LION_NSTRATEGIES &&
			 (LION_MULTI_STRATEGY_MASK & (1 << oprform->amopstrategy)) != 0) :
			(oprform->amopstrategy == LION_STRAT_EQUAL ||
			 LION_STRAT_IS_RANGE(oprform->amopstrategy));

		if (!stratok)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains operator %s with invalid strategy number %d",
							opfamilyname, "lion",
							format_operator(oprform->amopopr),
							oprform->amopstrategy)));
			result = false;
		}

		if (oprform->amoppurpose != AMOP_SEARCH ||
			OidIsValid(oprform->amopsortfamily))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains invalid ORDER BY specification for operator %s",
							opfamilyname, "lion",
							format_operator(oprform->amopopr))));
			result = false;
		}

		if (!check_amop_signature(oprform->amopopr, BOOLOID,
								  oprform->amoplefttype,
								  oprform->amoprighttype))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains operator %s with wrong signature",
							opfamilyname, "lion",
							format_operator(oprform->amopopr))));
			result = false;
		}

		/*
		 * Every type used by an operator must be hashable by this family:
		 * that is what lets a cross-type equality use the index, because the
		 * search value is hashed with its OWN type's function.  A multi-key
		 * family hashes keys and not the types its operators mention, so the
		 * rule does not apply to it.
		 */
		if (!multikey &&
			(!list_member_oid(hashabletypes, oprform->amoplefttype) ||
			 !list_member_oid(hashabletypes, oprform->amoprighttype)))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s lacks support function for operator %s",
							opfamilyname, "lion",
							format_operator(oprform->amopopr))));
			result = false;
		}
	}

	/* Check for inconsistent groups of operators/functions */
	grouplist = identify_opfamily_groups(oprlist, proclist);
	opclassgroup = NULL;
	foreach(lc, grouplist)
	{
		OpFamilyOpFuncGroup *thisgroup = (OpFamilyOpFuncGroup *) lfirst(lc);

		if (thisgroup->lefttype == opcintype &&
			thisgroup->righttype == opcintype)
			opclassgroup = thisgroup;

		/*
		 * A scalar family must offer equality for every type pair it knows
		 * about.  A multi-key family cannot be checked that way: its
		 * operators and its support functions do not even share a type pair
		 * (tsvector_ops has @@(tsvector,tsquery) and procs on
		 * (tsvector,tsvector)), so the per-operator and per-class checks
		 * above and below are all there is.
		 */
		if (!multikey &&
			(thisgroup->operatorset & (1 << LION_STRAT_EQUAL)) == 0)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s is missing operator(s) for types %s and %s",
							opfamilyname, "lion",
							format_type_be(thisgroup->lefttype),
							format_type_be(thisgroup->righttype))));
			result = false;
		}

		/*
		 * A range comparison (DESIGN.md §28) is answered by walking the run of
		 * entries the ORDERING puts between its bounds, so it needs support
		 * function 4 for the very same type pair: without it the only answer
		 * would be to test every entry, which is not an operator an index
		 * should claim.
		 */
		if (!multikey &&
			(thisgroup->operatorset & LION_RANGE_STRATEGY_MASK) != 0 &&
			(thisgroup->functionset & (((uint64) 1) << LION_CMP_PROC)) == 0)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s has range operator(s) for types %s and %s but no support function %d",
							opfamilyname, "lion",
							format_type_be(thisgroup->lefttype),
							format_type_be(thisgroup->righttype),
							LION_CMP_PROC)));
			result = false;
		}
	}

	/* The opclass itself must be complete */
	if (multikey)
	{
		uint64		want = (((uint64) 1) << LION_EXTRACTVALUE_PROC) |
			(((uint64) 1) << LION_EXTRACTQUERY_PROC);

		/*
		 * A hash function for the key type is required unless the key type is
		 * polymorphic, in which case there is no single function to name and
		 * the key type's default hash opclass answers instead
		 * (lion_fill_state()).
		 */
		if (!IsPolymorphicType(opckeytype))
			want |= ((uint64) 1) << LION_HASH_PROC;

		if (!opclassgroup || (opclassgroup->functionset & want) != want)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator class \"%s\" of access method %s is missing support function(s)",
							opclassname, "lion")));
			result = false;
		}

		if (!haveop)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator class \"%s\" of access method %s is missing operator(s)",
							opclassname, "lion")));
			result = false;
		}
	}
	else
	{
		if (!opclassgroup ||
			(opclassgroup->functionset & (((uint64) 1) << LION_HASH_PROC)) == 0)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator class \"%s\" of access method %s is missing support function %d",
							opclassname, "lion", LION_HASH_PROC)));
			result = false;
		}
		if (!opclassgroup)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator class \"%s\" of access method %s is missing operator(s)",
							opclassname, "lion")));
			result = false;
		}
	}

	ReleaseCatCacheList(proclist);
	ReleaseCatCacheList(oprlist);
	ReleaseSysCache(classtup);

	return result;
}

/*
 * The table access methods a lion index may sit on: the heap's, and no other
 * (DESIGN.md §2 and §23).  Everything here assumes a heap underneath - the
 * TID encoding has room for the heap's per-page line pointers and no more,
 * and the count reads the heap's visibility map and relies on the interlock
 * of DESIGN.md §9, which is a statement about heap VACUUM.  Another table AM
 * may hand out TIDs of any shape (citus_columnar's are stripe row numbers,
 * with offsets far past LION_MAX_OFFSET) and keeps no visibility map, or one
 * that means something else.
 *
 * The test is the routine, not the access method's name or Oid: a table AM
 * created with `HANDLER heap_tableam_handler` is the heap under another name,
 * and is accepted; a handler that returns anything else - even a copy of the
 * heap's callbacks - is not, because nothing here can tell what it changed.
 */
bool
lion_table_am_supported(Relation heap)
{
	return heap->rd_tableam == GetHeapamTableAmRoutine();
}

/*
 * The same as an ERROR, for ambuild and the SQL-callable counts.  ambuild is
 * the gate that matters: a table changes its access method only by being
 * rewritten (ALTER TABLE ... SET ACCESS METHOD), which rebuilds its indexes,
 * and a partition gets its copy of a partitioned index through ambuild as
 * well, whether it is created, attached or indexed later.  So no lion index
 * can exist on another table AM, and the checks in the count paths are there
 * for an index that got there some other way.
 */
void
lion_check_table_am(Relation heap)
{
	if (!lion_table_am_supported(heap))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("access method \"lion\" does not support table access method \"%s\"",
						get_am_name(heap->rd_rel->relam)),
				 errdetail("Table \"%s\" is not stored in the heap; lion indexes encode heap tuple identifiers and count through the heap's visibility map.",
						   RelationGetRelationName(heap)),
				 errhint("Use a table with the heap access method.")));
}

/*
 * Build an empty index in the init fork (used for unlogged relations).
 */
void
lionbuildempty(Relation index)
{
	LionOptions *opts = (LionOptions *) index->rd_options;
	uint32		inline_limit;
	uint32		wal_mode;
	Buffer		buf;
	LionMetaPageData meta;
	LionIndexState ix;

	inline_limit = opts ? (uint32) opts->inline_limit : LION_DEFAULT_INLINE_LIMIT;
	wal_mode = lion_wal_mode_for_build(index);

	/*
	 * The order an empty directory is in is the one every later insert will
	 * keep, so it is decided and recorded now, as a build does (§21): from
	 * the catalog, with a state no meta page has recorded anything in yet.
	 */
	memset(&meta, 0, sizeof(meta));
	lion_fill_index_state(index, &ix, &meta, CurrentMemoryContext);
	lion_meta_record_order(&meta, &ix);	/* not in the critical section */

	/* Meta page, pointing at the one leaf that is also the root (§21). */
	buf = ExtendBufferedRel(BMR_REL(index), INIT_FORKNUM, NULL,
							EB_LOCK_FIRST | EB_SKIP_EXTENSION_LOCK);
	Assert(BufferGetBlockNumber(buf) == LION_METAPAGE_BLKNO);
	START_CRIT_SECTION();
	lion_init_metapage(BufferGetPage(buf), inline_limit, LION_FIRST_BLKNO,
					  0, 1, wal_mode);
	LionPageGetMeta(BufferGetPage(buf))->order_flags = meta.order_flags;
	LionPageGetMeta(BufferGetPage(buf))->ordered_cols = meta.ordered_cols;
	LionPageGetMeta(BufferGetPage(buf))->order_ident = meta.order_ident;
	MarkBufferDirty(buf);
	log_newpage_buffer(buf, true);
	END_CRIT_SECTION();
	UnlockReleaseBuffer(buf);

	buf = ExtendBufferedRel(BMR_REL(index), INIT_FORKNUM, NULL,
							EB_LOCK_FIRST | EB_SKIP_EXTENSION_LOCK);
	Assert(BufferGetBlockNumber(buf) == LION_FIRST_BLKNO);
	START_CRIT_SECTION();
	lion_init_page(BufferGetPage(buf), LION_PAGE_BUCKET | LION_PAGE_ROOT);
	MarkBufferDirty(buf);
	log_newpage_buffer(buf, true);
	END_CRIT_SECTION();
	UnlockReleaseBuffer(buf);
}
