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
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/paths.h"
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
#include "lion_costs.h"
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

/* GUC pg_lion.enable_plain_scan (DESIGN.md §29.11, lioncostestimate()) */
static bool lion_enable_plain_scan = true;

static const relopt_parse_elt lion_relopt_tab[] = {
	{"buckets", RELOPT_TYPE_INT, offsetof(LionOptions, buckets)},
	{"inline_limit", RELOPT_TYPE_INT, offsetof(LionOptions, inline_limit)},
	{"max_entries", RELOPT_TYPE_INT, offsetof(LionOptions, max_entries)},
	{"fillfactor", RELOPT_TYPE_INT, offsetof(LionOptions, fillfactor)},
	{"wal_mode", RELOPT_TYPE_ENUM, offsetof(LionOptions, wal_mode)},
	{"summaries", RELOPT_TYPE_ENUM, offsetof(LionOptions, summaries)},
	{"summary_tids", RELOPT_TYPE_INT, offsetof(LionOptions, summary_tids)}
};

/* DESIGN.md §32: which key columns a build gives summary posting sets. */
static relopt_enum_elt_def lion_summaries_options[] = {
	{"off", LION_SUMOPT_OFF},
	{"on", LION_SUMOPT_ON},
	{"auto", LION_SUMOPT_AUTO},
	{(const char *) NULL}
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
 * planner GUCs and the planner hook that plants the LionCount CustomScan
 * (DESIGN.md section 10, implemented in lion_customscan.c).
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
	 * DESIGN.md §32.  Summary posting sets make a range over many keys cost
	 * a summary per bucket of keys instead of a posting set per key, and every
	 * insert into a summarized column one more posting-set insert.  "off" is
	 * the default so that nothing changes for an index that does not ask:
	 * "auto" lets the build decide per column from the data, "on" gives every
	 * ordered scalar column summaries.  Both are read at build time only and
	 * recorded on the meta page, so it is REINDEX that adds or removes them,
	 * and so is the bucket size.
	 */
	add_enum_reloption(lion_relopt_kind, "summaries",
					   "Which ordered key columns get summary posting sets",
					   lion_summaries_options, LION_SUMOPT_OFF,
					   "Valid values are \"off\", \"on\" and \"auto\".",
					   AccessExclusiveLock);
	add_int_reloption(lion_relopt_kind, "summary_tids",
					  "Rows a summary posting set holds before the next one starts",
					  LION_DEFAULT_SUMMARY_TIDS, LION_MIN_SUMMARY_TIDS,
					  LION_MAX_SUMMARY_TIDS,
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
	 * DESIGN.md §29.11, "Plain scans switched off": whether the planner may
	 * scan a lion index with a plain or an index-only scan.  Off, it plans
	 * lion indexes as it plans GIN's, for bitmap scans only, where
	 * enable_indexscan = off would take every other access method's index
	 * scans away as well.
	 */
	DefineCustomBoolVariable("pg_lion.enable_plain_scan",
							 "Enables the planner's use of plain and index-only scans of lion indexes.",
							 "Off, lion indexes are planned for bitmap scans only; other index access methods are unaffected.",
							 &lion_enable_plain_scan,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	/*
	 * DESIGN.md §29.11, "Correlated sets": whether lion's own estimates of a
	 * conjunction of set clauses on one lion index may be measured from the
	 * index when core's product of their selectivities may be far off.
	 */
	DefineCustomBoolVariable("pg_lion.enable_intersection_probe",
							 "Measures conjunctions of lion-indexed clauses from the index for lion's own cost estimates.",
							 "Off, lion prices its paths with the planner's product of the clauses' selectivities.",
							 &lion_enable_intersection_probe,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	/*
	 * The cost model's constants (DESIGN.md §31, "The settings"): one setting
	 * for each, the multiplier of the core cost it is priced in, for
	 * calibrating the model without a rebuild.
	 */
	lion_costs_init();

	/*
	 * The LionOrdered CustomScan (DESIGN.md §30): its GUC, its scan methods
	 * and the set_rel_pathlist_hook that offers it, chained like the other.
	 */
	lion_ordered_init();

	/*
	 * The endpoint probe (DESIGN.md §28): the statistics hook through which
	 * lion's own cost estimates see a range past the histogram as the
	 * directory has it.
	 */
	lion_selfuncs_init();

	MarkGUCPrefixReserved("pg_lion");

	/*
	 * The LionCount scan methods, known before any plan names them: a
	 * parallel worker reads the leader's plan - a parallel FK-side join,
	 * DESIGN.md §27 - before it has planned anything of its own.
	 */
	lion_count_scan_register();

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
		/* DESIGN.md §24, "Build"; 16 builds only btree indexes in parallel */
		.amcanbuildparallel = true,
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
		/* lioncostestimate() with the endpoint probe, DESIGN.md §28 */
		.amcostestimate = lion_amcostestimate,
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
 * What the scan answers of key column c, from the path's index quals: the
 * classification lion_scan_choose() makes of the scan keys (lion_scan.c,
 * DESIGN.md §5 SCAN step 5, §29.2), made here so that every function below
 * prices the scan the executor will run.
 *
 *	sets	every equality, list, `IS NULL` and multi-key query: a set tree
 *			each, ANDed with one another and with the other columns';
 *	walk	for a column without sets: `op ANY (array)` with a range strategy
 *			(ONE walk to its widest element), else the column's plain range
 *			comparisons, which ranges lists and which are all one walk, else
 *			`IS NOT NULL`;
 *	ndropped	the quals left to the recheck: a column's range comparisons
 *			beside its sets, and a second array range or the plain range
 *			comparisons beside an array range;
 *	nomatch	`IS NULL` beside a strict qual of the column or `IS NOT NULL`,
 *			which the scan answers with no row at all.
 *
 * `IS NOT NULL` beside a strict qual of its column - any qual but a null test
 * - is implied by it and is none of these, nor is a second `IS NOT NULL`.  A
 * set tree the scan cannot build after all, a multi-key query in mode ALL
 * (§17), is among the sets; lion_cost_qual_is_full() says which.  The lists
 * are the caller's to free.
 */
typedef struct LionCostCol
{
	List	   *sets;			/* RestrictInfos, in the path's order */
	List	   *ranges;			/* the plain range comparisons of the walk */
	RestrictInfo *walk;			/* the array range, the first range
								 * comparison, or `IS NOT NULL` */
	int			nquals;			/* index quals on the column at all */
	int			ndropped;
	bool		nomatch;
} LionCostCol;

static void
lion_cost_col_quals(IndexPath *path, int c, LionCostCol *cc)
{
	IndexOptInfo *index = path->indexinfo;
	RestrictInfo *arrayrange = NULL;
	RestrictInfo *notnull = NULL;
	int			narray = 0;
	int			nisnull = 0;
	int			nstrict = 0;
	ListCell   *lc;

	memset(cc, 0, sizeof(LionCostCol));

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

			cc->nquals++;
			if (IsA(clause, NullTest))
			{
				if (((NullTest *) clause)->nulltesttype == IS_NULL)
				{
					nisnull++;
					cc->sets = lappend(cc->sets, rinfo);
				}
				else if (notnull == NULL)
					notnull = rinfo;
				continue;
			}

			nstrict++;
			if (IsA(clause, OpExpr) &&
				lion_cost_is_range(index, c, (OpExpr *) clause))
				cc->ranges = lappend(cc->ranges, rinfo);
			else if (IsA(clause, ScalarArrayOpExpr) &&
					 lion_cost_is_range_op(index, c,
										   ((ScalarArrayOpExpr *) clause)->opno))
			{
				if (arrayrange == NULL)
					arrayrange = rinfo;
				narray++;
			}
			else
				cc->sets = lappend(cc->sets, rinfo);
		}
	}

	if (nisnull > 0 && (nstrict > 0 || notnull != NULL))
		cc->nomatch = true;

	if (cc->sets != NIL)
	{
		/* the recheck bounds the sets */
		cc->ndropped = list_length(cc->ranges) + narray;
		list_free(cc->ranges);
		cc->ranges = NIL;
	}
	else if (arrayrange != NULL)
	{
		cc->ndropped = narray - 1 + list_length(cc->ranges);
		list_free(cc->ranges);
		cc->ranges = NIL;
		cc->walk = arrayrange;
	}
	else if (cc->ranges != NIL)
		cc->walk = linitial_node(RestrictInfo, cc->ranges);
	else
		cc->walk = notnull;
}

static void
lion_cost_col_free(LionCostCol *cc)
{
	list_free(cc->sets);
	list_free(cc->ranges);
	cc->sets = NIL;
	cc->ranges = NIL;
}

/*
 * Would the scan read the whole of key column c to answer this one set qual
 * (lion_cost_col_quals())?  *all says whether it then emits every indexed
 * row as a candidate, too, for the heap to recheck.
 *
 * Only a multi-key query does: one the extractor answers with
 * LION_QMODE_ALL - a phrase, a prefix, a NOT, a weight mask, `<@`,
 * `@> '{}'`, a NULL element, or more than LION_MAX_QUERY_KEYS keys (DESIGN.md
 * §17) - and one whose value is not a plan-time Const (a Param), because the
 * MODE follows the query's shape, not just its value, and an unknown value
 * has to be priced as the expensive shape.  `op ANY (array)` is the union of
 * its elements' answers, so one element that needs every row is enough, and
 * an array not available at plan time has to be assumed to hold one.  A
 * scalar column's sets - an equality, a list, `IS NULL` - look up keys, and
 * never read the column whole.
 */
static bool
lion_cost_qual_is_full(IndexOptInfo *index, int c, Node *cl, bool *all)
{
	*all = false;
	if (IsA(cl, NullTest) || !lion_index_is_multikey(index, c))
		return false;

	if (IsA(cl, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) cl;
		StrategyNumber strategy;
		Node	   *arg;

		if (list_length(op->args) == 2 &&
			(strategy = (StrategyNumber)
			 get_op_opfamily_strategy(op->opno, index->opfamily[c])) != 0 &&
			(arg = lion_cost_strip((Node *) lsecond(op->args))) != NULL &&
			IsA(arg, Const))
		{
			/* a strict operator with NULL is never true: nothing is scanned */
			if (((Const *) arg)->constisnull ||
				!lion_query_is_full_scan(index, c, strategy,
										 ((Const *) arg)->constvalue))
				return false;
		}
		*all = true;
		return true;
	}

	if (IsA(cl, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) cl;
		StrategyNumber strategy;

		if (list_length(saop->args) == 2 &&
			(strategy = (StrategyNumber)
			 get_op_opfamily_strategy(saop->opno,
									  index->opfamily[c])) != 0 &&
			(strategy == LION_STRAT_EQUAL ||
			 !lion_array_query_is_full_scan(index, c, strategy,
											lion_cost_strip((Node *) lsecond(saop->args)))))
			return false;		/* a union of single-key lookups, or of exact queries */
		*all = true;
		return true;
	}

	return false;
}

/*
 * Will the scan have to walk the WHOLE index for this path - read every
 * bucket, every entry and every posting of every entry?
 *
 * The scan answers every set qual of a column and ANDs them with each other
 * and with the other columns' (lion_scan_choose(), lion_cost_col_quals()),
 * and the cost of the scan is the cost of what it answers, so the
 * classification is mirrored here.  A column is read whole only when
 * NOTHING it answers selects from its posting sets: every set qual of it
 * needs the whole column (lion_cost_qual_is_full()), or it is walked for
 * `IS NOT NULL`, which is every entry but the reserved NULL one (DESIGN.md
 * §14).  One qual that looks keys up is enough to keep the column off the
 * full walk - the scan drops the others and rechecks them.  A range walks
 * only the entries between its bounds, so it never makes the scan a full
 * one; what it costs per entry is lion_range_walk_cost()'s.
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
 * 2026-09-21 follow-up review).  And ranking ONE qual per column, as this
 * function did while the scan answered one, priced `tags @> '{}' AND tags &&
 * '{t1}'` as the full walk of the first and a whole-table recheck, where the
 * second one's sets answer it.
 */
static bool
lion_scan_walks_whole_index(IndexPath *path, bool *emits_all_rows)
{
	IndexOptInfo *index = path->indexinfo;
	int			ncols = index->nkeycolumns;
	int			nchosen = 0;
	int			firstcol = -1;
	bool		anyselective = false;
	int			c;

	*emits_all_rows = false;

	if (ncols < 1 || ncols > INDEX_MAX_KEYS)
		return false;

	/*
	 * Per column, would that column alone make the scan walk the whole
	 * column's entries?  The answers combine the way the scan does
	 * (DESIGN.md §24): the columns are INTERSECTED, so one column that
	 * selects from its posting sets keeps the scan off the full walk however
	 * the others are answered - the scan drops those and rechecks.  When
	 * none does, the scan walks the FIRST column, by its first qual.
	 */
	for (c = 0; c < ncols; c++)
	{
		LionCostCol cc;
		bool		colfull = true;
		bool		colall = false;
		ListCell   *lc;

		lion_cost_col_quals(path, c, &cc);
		if (cc.nquals == 0)
		{
			lion_cost_col_free(&cc);
			continue;
		}
		nchosen++;
		if (firstcol < 0)
			firstcol = c;

		if (cc.nomatch)
			colfull = false;	/* nothing is read at all */
		else if (cc.sets != NIL)
		{
			foreach(lc, cc.sets)
			{
				bool		all;

				if (!lion_cost_qual_is_full(index, c,
											(Node *) lfirst_node(RestrictInfo, lc)->clause,
											&all))
					colfull = false;
				else if (lc == list_head(cc.sets))
					colall = all;
			}
		}
		else
			colfull = IsA(cc.walk->clause, NullTest);	/* ranges are not */

		if (!colfull)
			anyselective = true;
		else if (c == firstcol)
			*emits_all_rows = colall;
		lion_cost_col_free(&cc);
	}

	/*
	 * No clause at all: a partial index whose predicate the query implies,
	 * which the scan answers by emitting every row it holds.  The rows are
	 * exactly the ones the scan selects, so the heap side keeps the
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
 * (lion_cost_col_quals(), lion_scan_col_tree()), which a range walk on col is
 * then ANDed with?  An equality, a list, `IS NULL` or a multi-key query is a
 * set; another range is a second walk and `IS NOT NULL` is dropped, neither of
 * which the scan streams beside this one, and so is a multi-key query that
 * needs every row (lion_cost_qual_is_full()).
 */
static bool
lion_cost_sets_beside(IndexPath *path, int col)
{
	IndexOptInfo *index = path->indexinfo;
	bool		beside = false;
	int			c;

	for (c = 0; c < index->nkeycolumns && !beside; c++)
	{
		LionCostCol cc;
		ListCell   *lc;

		if (c == col)
			continue;
		lion_cost_col_quals(path, c, &cc);
		foreach(lc, cc.sets)
		{
			bool		all;

			if (!lion_cost_qual_is_full(index, c,
										(Node *) lfirst_node(RestrictInfo, lc)->clause,
										&all))
			{
				beside = true;
				break;
			}
		}
		lion_cost_col_free(&cc);
	}
	return beside;
}

/*
 * The range quals of key column c that one walk answers (lion_cost_col_quals(),
 * DESIGN.md §28): none when the column has sets - an equality, a list, `IS
 * NULL`, a multi-key query, beside which a range is only rechecked - or
 * selects nothing; else an `op ANY (array)`, which for a range operator is
 * ONE walk to the widest element (lion_emit_array_range()), alone; else the
 * column's plain range comparisons, all of them one walk.
 */
static List *
lion_cost_col_ranges(IndexPath *path, int c)
{
	LionCostCol cc;
	List	   *ranges = NIL;

	lion_cost_col_quals(path, c, &cc);
	if (!cc.nomatch && cc.walk != NULL && !IsA(cc.walk->clause, NullTest))
	{
		if (cc.ranges != NIL)
		{
			ranges = cc.ranges;
			cc.ranges = NIL;
		}
		else
			ranges = list_make1(cc.walk);
	}
	lion_cost_col_free(&cc);
	return ranges;
}

/*
 * n_distinct of the heap column key column c indexes, which is how many
 * entries a scalar column has; DEFAULT_NUM_DISTINCT for an expression.
 */
static double
lion_cost_col_ndistinct(PlannerInfo *root, IndexOptInfo *index, int c)
{
	RangeTblEntry *rte;
	VariableStatData vardata;
	bool		isdefault;
	double		ndistinct;
	Var		   *var;

	if (index->indexkeys[c] <= 0)
		return DEFAULT_NUM_DISTINCT;

	rte = planner_rt_fetch(index->rel->relid, root);
	var = makeVar(index->rel->relid, index->indexkeys[c],
				  get_atttype(rte->relid, index->indexkeys[c]),
				  -1, index->indexcollations[c], 0);
	examine_variable(root, (Node *) var, index->rel->relid, &vardata);
	ndistinct = get_variable_numdistinct(&vardata, &isdefault);
	ReleaseVariableStats(vardata);
	return ndistinct;
}

/*
 * How many entries ONE walk of key column c's range reads (DESIGN.md §28):
 * the keys between its bounds, about n_distinct(col) x sel of them - or, on
 * a column with SUMMARY posting sets (§32), the keys of the buckets at the
 * range's two ends one by one and ONE summary for every bucket it covers
 * whole, which is what lion_walk_begin() hands the bitmap and the plain scan
 * alike.  The model is the count's (lion_cost_range_side(),
 * lion_customscan.c): a bucket holds max(summary_tids, rows per key) rows -
 * or what the column's first summaries hold on average, once that is more
 * than twice as many (lion_summary_shape(): keys that arrive out of order
 * grow the buckets past the reloption) - and so that many rows' worth of
 * keys, and a run of n keys covers about n / keys per bucket - 1 buckets
 * whole, half a bucket being left over at each end on average.  With no
 * whole bucket the walk is the keys', as the executor's is.
 */
static double
lion_cost_walk_entries(PlannerInfo *root, IndexPath *path, int c,
					   Selectivity sel)
{
	IndexOptInfo *index = path->indexinfo;
	double		nkeys = Max(1.0, lion_cost_col_ndistinct(root, index, c) * sel);
	Relation	indexrel;
	LionState  *col;
	LionSumShape shape;
	uint32		bucket_tids;
	bool		summarized;
	double		rowsper;
	double		bucketrows;
	double		keysper;
	double		whole;

	if (index->hypothetical)
		return nkeys;			/* nothing to read the options off */

	indexrel = index_open(index->indexoid, AccessShareLock);
	col = lion_index_column_state(indexrel, (AttrNumber) (c + 1));
	summarized = col->summarized && col->ordered;
	bucket_tids = lion_get_index_state(indexrel)->meta.summary_tids;
	memset(&shape, 0, sizeof(shape));
	if (summarized && bucket_tids > 0)
		lion_summary_shape(indexrel, col, &shape);
	index_close(indexrel, AccessShareLock);

	/* `on` over an empty table: no summary yet, and a walk of the keys */
	if (!summarized || bucket_tids == 0 || shape.nsummaries <= 0)
		return nkeys;

	rowsper = Max(Max(index->rel->tuples, 1.0) * sel / nkeys, 1.0);
	bucketrows = Max((double) bucket_tids, rowsper);
	if (shape.rows / shape.nsummaries > 2.0 * bucketrows)
		bucketrows = shape.rows / shape.nsummaries;
	keysper = Max(1.0, bucketrows / rowsper);
	whole = Max(0.0, nkeys / keysper - 1.0);
	if (whole < 1.0)
		return nkeys;
	return Max(0.0, nkeys - whole * keysper) + whole;
}

/*
 * What the ENTRIES of the range walks of this path cost (DESIGN.md §28), on
 * top of the pages and the postings genericcostestimate() prorates by the
 * selectivity.
 *
 * A range is one walk per key column, over the entries between its bounds
 * (lion_cost_walk_entries()), each decoded and compared with every bound.  On
 * a low- or mid-cardinality column that is a few hundred entries, which is
 * nothing; on a near-unique one every row is an entry, and this is the term -
 * with the index's own size, an entry header per row against btree's tuple -
 * that leaves such a column to btree, unless the column has summaries (§32),
 * whose walk is a summary per bucket.  Only a column the scan walks for its
 * range pays it (lion_cost_col_ranges()): not one with sets of its own.
 *
 * Beside another column's sets, a plain scan walks a long range once per
 * WINDOW of those sets' containers (DESIGN.md §29.3, lion_walk_window()), so
 * the entries are paid once per window: the heap's container keys over the
 * window, which is 1 below 32768 heap blocks at the default floor and an
 * upper bound above it (the sets may have containers at fewer keys).  The
 * bitmap scan, which shares the path, walks once per window of the same
 * memory too (lion_emit_intersect()), whose containers are held at their own
 * size rather than as bitset images, so it makes at most as many walks.  The
 * plain scan used to restart the other columns' stream for every entry
 * instead, a descent of each of their posting trees, and that was never
 * charged at all (2026-09-25 review).
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
lion_range_walk_cost(PlannerInfo *root, IndexPath *path, double *walkrows)
{
	IndexOptInfo *index = path->indexinfo;
	Cost		cost = 0;
	int			c;

	for (c = 0; c < index->nkeycolumns; c++)
	{
		List	   *ranges = lion_cost_col_ranges(path, c);

		if (ranges != NIL)
		{
			Selectivity sel = clauselist_selectivity(root, ranges,
													 index->rel->relid,
													 JOIN_INNER, NULL);
			double		entries = lion_cost_walk_entries(root, path, c, sel);

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
 * The sum of the squared frequencies of a column's values among its non-NULL
 * rows, from its MCV list and, for the values not in it, an even share of
 * the rest: what ANALYZE's correlation comes out at when the values are
 * placed at random (lion_var_heap_correlation()).  1 when nothing is known,
 * which leaves no correlation at all.
 */
static double
lion_stats_sum_sq_freq(VariableStatData *vardata)
{
	Form_pg_statistic stats;
	AttStatsSlot sslot;
	double		nonnull;
	double		summcv = 0.0;
	double		sumsq = 0.0;
	double		rest;
	double		ndistinct;
	bool		isdefault;
	int			nmcv = 0;
	int			i;

	if (!HeapTupleIsValid(vardata->statsTuple))
		return 1.0;
	stats = (Form_pg_statistic) GETSTRUCT(vardata->statsTuple);
	nonnull = 1.0 - stats->stanullfrac;
	if (nonnull <= 0.0)
		return 1.0;

	if (get_attstatsslot(&sslot, vardata->statsTuple, STATISTIC_KIND_MCV,
						 InvalidOid, ATTSTATSSLOT_NUMBERS))
	{
		for (i = 0; i < sslot.nnumbers; i++)
		{
			double		f = sslot.numbers[i] / nonnull;

			summcv += sslot.numbers[i];
			sumsq += f * f;
		}
		nmcv = sslot.nnumbers;
		free_attstatsslot(&sslot);
	}

	rest = Max(nonnull - summcv, 0.0) / nonnull;
	if (rest > 0.0)
	{
		ndistinct = get_variable_numdistinct(vardata, &isdefault);
		sumsq += rest * rest / Max(ndistinct - nmcv, 1.0);
	}
	return Min(sumsq, 1.0);
}

/*
 * The correlation of a heap column's values with the heap order, as the cost
 * model reads it (DESIGN.md §29.11): the ANALYZE correlation for the type's
 * default `<`, which is what ANALYZE computed it with, less the part that
 * says nothing about the heap.  0 whenever there is no statistic.
 *
 * ANALYZE sorts its sample by value and equal values by their place in the
 * heap, so a column of few values placed at random correlates by exactly the
 * sum of its values' squared frequencies: 1/k for k equally common values,
 * 0.82 for a 90/10 boolean.  cost_index() reads that as the rows of one value
 * packed on fewer pages than they touch - and lion is made for such columns.
 * So the correlation is taken beyond that baseline: (corr - S) / (1 - S), S
 * the sum of the squares, clamped to [-1, 1], which is 0 for values placed at
 * random and still 1 for a column stored in value order (and -1 for one
 * stored in reverse, whose values are packed as well).  Measured on 8M rows
 * with the values placed at random: 0.4406 for a column of three values at
 * 60/20/20% (S = 0.44), 0.8171 for 90/10 (S = 0.82), 0.6852 for 80/20
 * (S = 0.68).
 *
 * Shared with the count pushdown, which prices the heap pages its recheck
 * visits by the same number (lion_customscan.c, lion_var_correlation()): read
 * raw, a low-cardinality column placed at random looked packed there too.
 */
double
lion_var_heap_correlation(PlannerInfo *root, Index relid, Var *var)
{
	VariableStatData vardata;
	TypeCacheEntry *tce;
	double		corr = 0.0;

	examine_variable(root, (Node *) var, relid, &vardata);
	tce = lookup_type_cache(var->vartype, TYPECACHE_LT_OPR);
	if (HeapTupleIsValid(vardata.statsTuple) && OidIsValid(tce->lt_opr))
	{
		AttStatsSlot sslot;

		if (get_attstatsslot(&sslot, vardata.statsTuple,
							 STATISTIC_KIND_CORRELATION, tce->lt_opr,
							 ATTSTATSSLOT_NUMBERS))
		{
			if (sslot.nnumbers > 0)
			{
				double		sumsq = lion_stats_sum_sq_freq(&vardata);

				corr = (sumsq < 1.0) ?
					(sslot.numbers[0] - sumsq) / (1.0 - sumsq) : 0.0;
				corr = Max(-1.0, Min(corr, 1.0));
			}
			free_attstatsslot(&sslot);
		}
	}
	ReleaseVariableStats(vardata);

	return corr;
}

/*
 * The correlation a PLAIN index scan's heap fetches have with the heap order
 * (DESIGN.md §29.11), which only cost_index() reads - a bitmap heap scan
 * sorts its pages whatever the index says.  It starts from btcostestimate()'s
 * answer: the correlation of the first index column the path has a clause on
 * (of column 1 when it has none), times 0.75 for a multicolumn index, taken
 * as lion_var_heap_correlation() takes it.  Within one key a lion scan
 * returns TIDs in heap order, as btree does since its heap-TID tiebreaker, so
 * how that key's rows are spread over the heap is what the column's
 * correlation describes.  0 for a multi-key column (a row is under several of
 * its entries and the column's statistics are the ARRAY's) and for an
 * expression column.
 */
static double
lion_index_correlation(PlannerInfo *root, IndexPath *path)
{
	IndexOptInfo *index = path->indexinfo;
	RangeTblEntry *rte;
	double		corr;
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
	var = makeVar(index->rel->relid, index->indexkeys[col],
				  get_atttype(rte->relid, index->indexkeys[col]), -1,
				  index->indexcollations[col], 0);
	corr = lion_var_heap_correlation(root, index->rel->relid, var);

	if (index->nkeycolumns > 1)
		corr *= 0.75;
	return corr;
}

/*
 * The order a plain scan of this path hands out its TIDs in (DESIGN.md §29.3,
 * lion_source_build()):
 *
 *	LION_PLAIN_SORTED	heap order: the SETS, UNION and WINDOW sources
 *						(lion_source_sorted());
 *	LION_PLAIN_WALK		one column's entries in key order, each entry's TIDs
 *						in heap order: *walkcol is that column;
 *	LION_PLAIN_LIST		an IN list longer than a batch, located and streamed a
 *						batch at a time, each batch in heap order.
 *
 * Mirrors lion_source_build(): every set qual of every column is answered
 * (lion_cost_col_quals()); a long list outranks a walk; the first column
 * that walks a range walks, and a second one is rechecked; an `IS NOT NULL`
 * walks only when nothing else answers.  Beside another column's sets a
 * range is taken for a WINDOW, as lion_range_walk_cost() takes it: a walk
 * too short for one restarts the other columns' stream a few times at most
 * (lion_source_walk_is_long()), each restart in heap order.  No key at all
 * is a partial index read whole, by a walk of column 1 or, when that column
 * is multi-key, by a union.
 *
 * *rechecks says whether the scan sets xs_recheck (§29.6), and so evaluates
 * its index quals on every row it fetches: when it leaves a qual unanswered
 * (a range beside its column's sets, a second walk or long list, a walk
 * beside a list, an `IS NOT NULL` beside anything that answers), when it
 * answers a multi-key column, and for a UNION.
 */
#define LION_PLAIN_SORTED	0
#define LION_PLAIN_WALK		1
#define LION_PLAIN_LIST		2

static int
lion_plain_scan_shape(PlannerInfo *root, IndexPath *path, int *walkcol,
					  bool *rechecks)
{
	IndexOptInfo *index = path->indexinfo;
	int			ncols = index->nkeycolumns;
	int			nsets = 0;
	int			nlists = 0;
	int			rangecol = -1;
	int			notnullcol = -1;
	int			nnotnull = 0;
	int			nrangecols = 0;
	int			ndropped = 0;
	bool		anychosen = false;
	int			c;

	*walkcol = 0;
	*rechecks = false;
	if (ncols < 1 || ncols > INDEX_MAX_KEYS)
		return LION_PLAIN_LIST;

	for (c = 0; c < ncols; c++)
	{
		LionCostCol cc;
		ListCell   *lc;

		lion_cost_col_quals(path, c, &cc);
		if (cc.nquals == 0)
		{
			lion_cost_col_free(&cc);
			continue;
		}
		anychosen = true;
		ndropped += cc.ndropped;

		/*
		 * A column that selects nothing is the NONE source, and a multi-key
		 * column a set tree or a UNION: all of them in heap order.
		 */
		if (cc.nomatch || lion_index_is_multikey(index, c))
		{
			nsets++;
			if (!cc.nomatch)
				*rechecks = true;
			lion_cost_col_free(&cc);
			continue;
		}

		foreach(lc, cc.sets)
		{
			Node	   *cl = (Node *) lfirst_node(RestrictInfo, lc)->clause;

			if (IsA(cl, ScalarArrayOpExpr) &&
				get_op_opfamily_strategy(((ScalarArrayOpExpr *) cl)->opno,
										 index->opfamily[c]) == LION_STRAT_EQUAL &&
#if PG_VERSION_NUM >= 170000
				estimate_array_length(root,
									  (Node *) lsecond(((ScalarArrayOpExpr *) cl)->args))
#else
				estimate_array_length((Node *) lsecond(((ScalarArrayOpExpr *) cl)->args))
#endif
				> lion_scan_list_batch())
				nlists++;
			else
				nsets++;
		}

		if (cc.walk != NULL && IsA(cc.walk->clause, NullTest))
		{
			nnotnull++;
			if (notnullcol < 0)
				notnullcol = c;
		}
		else if (cc.walk != NULL)
		{
			nrangecols++;
			if (rangecol < 0)
				rangecol = c;
		}
		lion_cost_col_free(&cc);
	}

	if (ndropped > 0 || nrangecols > 1 || nlists > 1 ||
		(nlists > 0 && nrangecols > 0) || nnotnull > 1 ||
		(nnotnull > 0 && (nsets > 0 || nlists > 0 || nrangecols > 0)))
		*rechecks = true;

	if (!anychosen)
	{
		*rechecks = lion_index_is_multikey(index, 0);
		return lion_index_is_multikey(index, 0) ? LION_PLAIN_SORTED :
			LION_PLAIN_WALK;
	}
	if (nlists > 0)
		return LION_PLAIN_LIST;
	if (nsets > 0)
		return LION_PLAIN_SORTED;
	if (rangecol >= 0 || notnullcol >= 0)
	{
		*walkcol = (rangecol >= 0) ? rangecol : notnullcol;
		return LION_PLAIN_WALK;
	}
	return LION_PLAIN_SORTED;
}

/*
 * How many entries a WALK of key column c reads (LION_PLAIN_WALK): the
 * column's n_distinct, or the entries of its range's walk when a range bounds
 * it (lion_cost_walk_entries(), summaries and all) - the same count
 * lion_range_walk_cost() prices the entries by, and the same caveat: it is
 * the share of the column's values for a column whose rows spread evenly over
 * them.
 */
static double
lion_plain_walk_entries(PlannerInfo *root, IndexPath *path, int c)
{
	IndexOptInfo *index = path->indexinfo;
	List	   *ranges = lion_cost_col_ranges(path, c);
	double		entries;

	if (ranges == NIL)
		return Max(1.0, lion_cost_col_ndistinct(root, index, c));
	entries = lion_cost_walk_entries(root, path, c,
									 clauselist_selectivity(root, ranges,
															index->rel->relid,
															JOIN_INNER, NULL));
	list_free(ranges);
	return Max(1.0, entries);
}

/*
 * The plain scan's own prices below are planner settings (lion_costs.c):
 * each macro is the multiplier a setting holds - LION_PLAIN_FETCH_ROW_COST is
 * pg_lion.plain_fetch_row_cost - of the unit its comment names, and each
 * comment is why the setting's default is what it is.
 */

/*
 * What a plain scan pays to fetch a row past the first on its heap page, in
 * cpu_tuple_cost, beyond what a bitmap heap scan pays for the same row: an
 * amgettuple call, a buffer lock and a HOT search per row, where the bitmap
 * heap scan takes the lock and walks the page's matches once.  Measured on
 * the release build (backend CPU time, heap in shared buffers, the two scans
 * interleaved): 11.5 ns a row more for 40,000 numeric rows stored in value
 * order, 21 ns for 75,000 rows of 30 days stored in order, 20 ns for 43% of a
 * 200k-row table on every page (plaincost.sql) - one cpu_tuple_cost at the
 * 500 units a millisecond core's scans run at there (DESIGN.md §10, "The
 * units").  It was 0.5, from 20 to 50 ns on an assert build.  Where the rows
 * are scattered the plain scan spends 0.5 us a page LESS than the bitmap heap
 * scan (30% less over 25,000 rows at 0.4 a page), which is not credited.
 */
#define LION_PLAIN_FETCH_ROW_COST	lion_plain_fetch_row_cost

/*
 * cost_bitmap_tree_node()'s charge for a row's bitmap entry, in
 * cpu_operator_cost: part of the bitmap heap scan's per-row price, which the
 * plain scan is charged as well (lion_plain_heap_correlation()).
 */
#define LION_BITMAP_ROW_COST		lion_bitmap_row_cost

/*
 * What a WALK pays for an entry past the first, in cpu_tuple_cost, beyond what
 * the bitmap scan's walk of the same entry pays: the plain scan streams each
 * entry through a stream of its own, the other columns' sets beside it
 * (lion_source_next()), where the bitmap scan adds the entry's members to its
 * bitmap.  Measured on an assert-enabled build over 4,000 to 390,000 one-row
 * entries of a timestamp stored in heap order, the heap in memory: 0.30 to
 * 0.35 us an entry more than the bitmap scan, against 55 ns for a row of a
 * sequential scan.
 */
#define LION_WALK_PASS_COST		lion_walk_pass_cost

/*
 * Does a plain scan whose rows lie on `pages` heap pages compete with a
 * PARALLEL bitmap heap scan of them (DESIGN.md §29.11, "One process against
 * several")?  core's own answer, create_partial_bitmap_paths()'s: the
 * relation may be scanned in parallel and compute_parallel_worker() gives
 * that many pages a worker - never below min_parallel_table_scan_size, at a
 * max_parallel_workers_per_gather of 0 or for a table whose parallel_workers
 * is 0.  An inheritance child is given one whatever its size, for a Parallel
 * Append to combine; below that size a child's plain scan is taken as a base
 * relation's is, against one process's bitmap heap scan.
 */
static bool
lion_plain_meets_workers(RelOptInfo *rel, double pages)
{
	if (!rel->consider_parallel || max_parallel_workers_per_gather <= 0)
		return false;
	if (rel->reloptkind != RELOPT_BASEREL &&
		pages < (double) min_parallel_table_scan_size)
		return false;
	return compute_parallel_worker(rel, pages, -1,
								   max_parallel_workers_per_gather) > 0;
}

/*
 * The correlation handed to cost_index() for a plain scan (DESIGN.md
 * §29.11).  A LIST, which restarts the heap for every batch, gets the
 * column's, as btree's does (lion_index_correlation()).
 *
 * For a scan in heap order it is not a statistic but the value that makes
 * cost_index()'s heap side come out at what the scan reads and does, which
 * is what a bitmap heap scan of the same rows reads and does, and a little
 * more.  cost_index() interpolates its I/O by the correlation squared between
 * rows packed on sel x heap pages and read in order, and rows scattered at
 * random over Mackert and Lohman's count of pages, each a random read.  A
 * scan in heap order has the packed end but not the scattered one: scattered
 * rows are read as a bitmap heap scan reads them, each page once and in block
 * order, which cost_bitmap_heap_scan() prices (compute_bitmap_pages():
 * Cardenas's count of the pages the rows touch, at random_page_cost falling
 * to seq_page_cost as those approach the whole heap).  So the I/O is that,
 * interpolated towards the packed end by the column's correlation.  Priced
 * as a random read a page, a count of 2% of 8M rows scattered over 108k of
 * the heap's 190k pages came to 2.3 times the bitmap scan's price and ran 1.6
 * to 2.5 times faster than it (2026-09-27).
 *
 * Per row, cost_index() charges cpu_tuple_cost and the quals it leaves to the
 * heap, and cost_bitmap_heap_scan() that, every index qual as well (the
 * recheck) and a bitmap entry.  The plain scan is charged the bitmap heap
 * scan's price - it rechecks its index quals whenever it drops one or reads
 * a multi-key column, which cost_index() never charges - and, for every row
 * past the first on its page, LION_PLAIN_FETCH_ROW_COST more.  In the
 * measurement behind that constant the plain scan spent 0.4 us a page LESS
 * than the bitmap heap scan, which is not credited: with one row to a page
 * the two cost about the same.
 *
 * That is one process against one process.  Against a PARALLEL bitmap heap
 * scan - where core would give the bitmap heap scan of the same pages
 * workers (lion_plain_meets_workers()) - it is not.  The plain scan is one
 * process (amcanparallel is false: cost_index() divides none of its price)
 * that reads a page when it needs it; the parallel scan reads its pages in
 * several processes, each reading ahead.  core credits that scan with its CPU
 * divided and nothing for its reads, so the plain scan gives back the credit
 * it took from cost_bitmap_heap_scan() for reading in block order: a page is
 * priced as one process's read, random_page_cost but for the pages that
 * follow the one read before them, which the kernel's read-ahead of a
 * consecutive run serves - their share of the heap, where the bitmap heap
 * scan's price takes its square root.  Below that size, and wherever no
 * parallel plan can be made, it is one process against one, priced alike,
 * which is what keeps a selective result on the plain scan (DESIGN.md
 * §29.11, "One process against several").
 *
 * A WALK hands out one entry's TIDs in heap order, then the next entry's:
 * it is a pass over the heap per entry, each reading its entry's pages in
 * block order - a bitmap heap scan of one entry's rows, done once per entry
 * (lion_plain_walk_entries()).  So its pages are what compute_bitmap_pages()
 * counts for scans repeated that often, Mackert and Lohman's for all the rows
 * together (a page a later pass reads again comes from the cache while the
 * cache holds the heap), each at the price of a pass's own pages - a pass of
 * a few pages pays random_page_cost for each, and one over most of the heap
 * seq_page_cost - and its fetches are counted per pass, and each pass past
 * the first costs its stream (LION_WALK_PASS_COST).  One entry is a sorted
 * scan; entries of a row each are btree's uncorrelated end, one random read a
 * row, and a stream each.  It used to be priced as btree's whatever its
 * entries held: `flag >= true`, one entry over 80% of 1M rows on every page,
 * came to 2.4 times the bitmap scan's price for 1.14 to 1.41 times its time,
 * and two entries of 40% to 2.9 times for 1.2 to 1.5 (2026-09-27).
 *
 * cost_index() charges no more than its uncorrelated end, so at a
 * random_page_cost near seq_page_cost the heap side of a result on nearly
 * every page is clamped there, about the bitmap heap scan's, and the fetches
 * that make the plain scan dearer are lost; what the correlation cannot carry
 * is charged to the path once it is built (lion_plain_note_remainder()).
 */
static double
lion_plain_heap_correlation(PlannerInfo *root, IndexPath *path,
							double loop_count, Selectivity sel)
{
	IndexOptInfo *index = path->indexinfo;
	RelOptInfo *baserel = index->rel;
	double		corr = lion_index_correlation(root, path);
	double		T;
	double		tuples;
	double		pages;
	double		max_pages;
	double		min_pages;
	double		spc_random_page_cost;
	double		spc_seq_page_cost;
	QualCost	qcost;
	Cost		target;
	Cost		max_io;
	Cost		min_io;
	double		csquared;
	bool		rechecks;
	double		passes = 1.0;	/* heap passes: the entries of a WALK */
	double		passpages;		/* the pages one pass reads */
	double		visits;			/* the page visits of all the passes */
	double		bmpages;		/* the pages a bitmap heap scan reads */
	double		share;			/* of the price between random and seq */
	bool		serial;			/* against a parallel bitmap heap scan */
	int			walkcol;
	int			shape;

	/*
	 * An index-only scan is left as it was: lion's are of queries that need no
	 * column, so they have no qual and read a whole column, and a multi-key
	 * one - the only kind in heap order - fetches every TID it hands out
	 * (liongettuple()), which the all-visible fraction cost_index() applies
	 * would not describe.
	 */
	if (path->path.pathtype == T_IndexOnlyScan)
		return corr;
	shape = lion_plain_scan_shape(root, path, &walkcol, &rechecks);
	if (shape == LION_PLAIN_LIST)
		return corr;

	T = (baserel->pages > 1) ? (double) baserel->pages : 1.0;
	tuples = clamp_row_est(sel * baserel->tuples);
	get_tablespace_page_costs(baserel->reltablespace, &spc_random_page_cost,
							  &spc_seq_page_cost);
	if (shape == LION_PLAIN_WALK)
		passes = Min(lion_plain_walk_entries(root, path, walkcol), tuples);

	/*
	 * compute_bitmap_pages() and cost_bitmap_heap_scan(), for `passes` scans
	 * of tuples / passes rows each
	 */
	if (loop_count > 1 || passes > 1.0)
		pages = index_pages_fetched(tuples * loop_count, baserel->pages,
									(double) index->pages, root) / loop_count;
	else
		pages = (2.0 * T * tuples) / (2.0 * T + tuples);
	pages = (pages >= T) ? T : ceil(pages);
	if (passes > 1.0)
	{
		double		perpass = tuples / passes;

		passpages = Min((2.0 * T * perpass) / (2.0 * T + perpass), T);
	}
	else
		passpages = pages;
	visits = Min(passes * passpages, tuples);

	/*
	 * The price of a page.  Against one process's bitmap heap scan, that
	 * scan's: random_page_cost falling to seq_page_cost with the square root
	 * of the share of the heap a pass reads.  Against a parallel one, over
	 * the pages compute_bitmap_pages() counts for it, one process's reading a
	 * page when it needs it: random_page_cost, and seq_page_cost for a page
	 * that follows the one read before it - the share itself, not its root.
	 */
	bmpages = Min(ceil((2.0 * T * tuples) / (2.0 * T + tuples)), T);
	serial = loop_count <= 1.0 && path->path.param_info == NULL &&
		lion_plain_meets_workers(baserel, bmpages);
	if (passpages < 2.0)
		share = 0.0;
	else if (serial)
		share = passpages / T;
	else
		share = sqrt(passpages / T);
	target = pages * (spc_random_page_cost -
					  (spc_random_page_cost - spc_seq_page_cost) * share);

	/* cost_index()'s ends: uncorrelated (max_io) and packed (min_io) */
	if (loop_count > 1)
	{
		max_pages = index_pages_fetched(tuples * loop_count, baserel->pages,
										(double) index->pages, root);
		min_pages = index_pages_fetched(ceil(sel * (double) baserel->pages) *
										loop_count, baserel->pages,
										(double) index->pages, root);
		max_io = max_pages * spc_random_page_cost / loop_count;
		min_io = min_pages * spc_random_page_cost / loop_count;
	}
	else
	{
		max_pages = index_pages_fetched(tuples, baserel->pages,
										(double) index->pages, root);
		min_pages = ceil(sel * (double) baserel->pages);
		max_io = max_pages * spc_random_page_cost;
		min_io = (min_pages > 0) ? spc_random_page_cost +
			Max(min_pages - 1.0, 0.0) * spc_seq_page_cost : 0.0;
	}

	/* from scattered as the bitmap reads them to packed */
	target += corr * corr * (min_io - target);

	/*
	 * The rows: the bitmap entry, the index quals when the scan rechecks them
	 * (§29.6) - cost_bitmap_heap_scan() charges them always, but an exact
	 * scan evaluates none, and charged them it lost `lion_pdn`'s count of
	 * 40,000 of 60,000 numeric rows (4.3 ms) to the sequential scan (10 ms),
	 * whose one numeric `=` a row core prices as an int's - and the fetches.
	 */
	cost_qual_eval(&qcost, get_quals_from_indexclauses(path->indexclauses),
				   root);
	target += tuples * ((rechecks ? qcost.per_tuple : 0.0) +
						LION_BITMAP_ROW_COST * cpu_operator_cost);
	target += LION_PLAIN_FETCH_ROW_COST * cpu_tuple_cost *
		Max(tuples - visits, 0.0);
	target += LION_WALK_PASS_COST * cpu_tuple_cost * Max(passes - 1.0, 0.0);

	/* what the uncorrelated end cannot carry is charged to the path later */
	if (target > max_io && loop_count <= 1.0 && path->path.param_info == NULL)
		lion_plain_note_remainder(root, path, target - max_io);

	if (max_io <= min_io)
		return 0.0;
	csquared = (max_io - target) / (max_io - min_io);
	csquared = Max(0.0, Min(csquared, 1.0));
	return sqrt(csquared);
}

/*
 * The quals of a scan's AND of SETS (DESIGN.md §29.11, "One price for the
 * AND of sets"): every set qual the scan answers, of every key column and as
 * many as a column has (lion_cost_col_quals()) - an equality, `IS NULL`, a
 * multi-key query, or an IN list, whose sets are united - as lion_scan_choose()
 * answers them.  Two `&&` on one array column are two sets as much as two
 * columns are.  A multi-key query that needs every row is none: the scan
 * drops it beside the others and rechecks it (lion_cost_qual_is_full()).  A
 * range or `IS NOT NULL` is a walk, priced by lion_range_walk_cost(), and
 * every other qual of a column is left to the heap recheck.
 *
 * Returns how many there are, with their key columns (1-based) in *cols and
 * their RestrictInfos in *quals, in column order, both palloc'd; 0 when a
 * column selects nothing, since the scan then reads no set at all.
 */
static int
lion_scan_set_quals(IndexPath *path, AttrNumber **cols, RestrictInfo ***quals)
{
	IndexOptInfo *index = path->indexinfo;
	int			ncols = index->nkeycolumns;
	int			maxsets = 0;
	int			nsets = 0;
	bool		nomatch = false;
	ListCell   *lc;
	int			c;

	*cols = NULL;
	*quals = NULL;
	if (ncols < 1 || ncols > INDEX_MAX_KEYS)
		return 0;

	foreach(lc, path->indexclauses)
		maxsets += list_length(((IndexClause *) lfirst(lc))->indexquals);
	if (maxsets < 2)
		return 0;
	*cols = (AttrNumber *) palloc(sizeof(AttrNumber) * maxsets);
	*quals = (RestrictInfo **) palloc(sizeof(RestrictInfo *) * maxsets);

	for (c = 0; c < ncols && !nomatch; c++)
	{
		LionCostCol cc;

		lion_cost_col_quals(path, c, &cc);
		nomatch = cc.nomatch;
		foreach(lc, cc.sets)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
			bool		all;

			if (lion_cost_qual_is_full(index, c, (Node *) rinfo->clause, &all))
				continue;
			(*cols)[nsets] = (AttrNumber) (c + 1);
			(*quals)[nsets] = rinfo;
			nsets++;
		}
		lion_cost_col_free(&cc);
	}

	return nomatch ? 0 : nsets;
}

/*
 * What genericcostestimate() charged for the index pages it prorated by the
 * selectivity of the quals (costs->numIndexPages at random_page_cost, spread
 * over the repeated scans of a nested loop or a list as index_pages_fetched()
 * spreads them): the part of its estimate that the AND of sets replaces.
 */
static Cost
lion_generic_page_cost(PlannerInfo *root, IndexPath *path, double loop_count,
					   const GenericCosts *costs)
{
	IndexOptInfo *index = path->indexinfo;
	double		num_scans = costs->num_sa_scans * loop_count;

	if (num_scans > 1)
		return index_pages_fetched(costs->numIndexPages * num_scans,
								   index->pages, (double) index->pages,
								   root) *
			costs->spc_random_page_cost / loop_count;
	return costs->numIndexPages * costs->spc_random_page_cost;
}

/*
 * Cost estimate: the generic estimate, with four corrections and the heap
 * correlation.  A scan that has to walk the whole index is priced as one
 * rather than as the selective lookup its predicate's output selectivity
 * suggests, a range pays for the entries it walks (lion_range_walk_cost()),
 * an AND of two sets or more is priced as the count pushdown prices the same
 * sets, from a selectivity the intersection probe may have measured
 * (lion_cost_set_and(), lion_isect_factor()), and a plain index scan's heap
 * side is priced by the correlation: for a scan in heap order, the bitmap
 * heap scan's price for the same pages and the plain scan's per-row work
 * (lion_plain_heap_correlation()), otherwise the column's correlation as
 * btree's is (lion_index_correlation(), DESIGN.md §29.11); a bitmap path,
 * which shares this estimate, does not read that last number.  With
 * pg_lion.enable_plain_scan off there is no plain path to price.
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
	 * pg_lion.enable_plain_scan off (DESIGN.md §29.11, "Plain scans switched
	 * off"): the planner is to plan this index as it plans a GIN index, for
	 * bitmap scans only.  get_index_paths() offers an IndexPath as a plain or
	 * index-only scan only when the index's amhasgettuple says so, and asks
	 * once the path is built and costed - here - so clearing the flag now
	 * keeps every plain path of the index, parameterized ones included, out
	 * of this planning run before add_path() has compared one with anything
	 * else.  The bitmap paths are built from the same IndexPaths and do not
	 * change, nor does what reads them: LionOrdered takes its lion side from
	 * a bitmap path when there is no plain one (lion_ordered.c).  The next
	 * planning run reads the flag from the handler again.
	 */
	if (!lion_enable_plain_scan)
		path->indexinfo->amhasgettuple = false;

	/*
	 * A full walk visits every index tuple, and genericcostestimate() then
	 * prorates the index's tuples into every index page.  The planner's
	 * count of those is ROWS: the table's, or for a partial index its
	 * reltuples, which VACUUM and ANALYZE both set to rows (DESIGN.md §18,
	 * "Statistics").
	 */
	if (fullscan)
		costs.numIndexTuples = Max(path->indexinfo->tuples, 1.0);

	genericcostestimate(root, path, loop_count, &costs);

	/*
	 * genericcostestimate() charges every index tuple it expects to visit an
	 * operator call per index qual, as a btree evaluates its quals on its
	 * tuples.  A lion scan evaluates none: its "index tuples" are the members
	 * of the posting sets its quals located, turned into TIDs a container at a
	 * time (DESIGN.md §29.11).  cpu_index_tuple_cost a TID is left, which is
	 * about what a TID costs on the release build (5 to 15 ns a TID put in a
	 * bitmap, against the 10 ns cpu_index_tuple_cost stands for at 500 units a
	 * millisecond); with the operator on top the bitmap scan of 40,000 of
	 * `lion_pdn`'s 60,000 numeric rows lost to the sequential scan at 1,185
	 * against 1,116, for 2.5 ms against 5.3.
	 */
	costs.indexTotalCost -= costs.numIndexTuples * costs.num_sa_scans *
		cpu_operator_cost *
		list_length(get_quals_from_indexclauses(path->indexclauses));
	costs.indexTotalCost = Max(costs.indexTotalCost, costs.indexStartupCost);

	if (fullscan)
	{
		double		allpages = Max((double) path->indexinfo->pages, 1.0);

		/*
		 * Rows, not postings: a row is posted once per key column and once
		 * per element of a multi-key one, so the prorated page count can come
		 * out short of the index the scan really reads.  Charge the rest of
		 * it.
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

	costs.indexTotalCost += lion_range_walk_cost(root, path, &walkrows);

	/*
	 * The AND of several sets (DESIGN.md §29.11, "One price for the AND of
	 * sets"): of several columns, or several of one.  genericcostestimate()
	 * prorated the index by the selectivity of the quals together, which
	 * prices an AND by what it RETURNS; what the scan reads is the sets,
	 * located, united where a list is, and ANDed as a count ANDs them.  So the
	 * prorated pages give way to the price the count pushdown charges for the
	 * same sets (lion_cost_set_and()), and what is left of
	 * genericcostestimate() is the scan's own: a TID a result row
	 * (cpu_index_tuple_cost), and the quals' arguments.  A nested loop's inner
	 * scan repeats the AND, and its pages are spread over the repetitions as
	 * genericcostestimate() spreads its own (index_pages_fetched()).
	 *
	 * The selectivity is corrected first, by what the intersection probe
	 * measured of the same sets ("Correlated sets"): a factor on core's
	 * estimate, 1 unless the probe found it three times off or more.  It
	 * moves the TIDs charged here and everything priced from the selectivity:
	 * the heap side of a plain or bitmap path, and LionOrdered's lion side.
	 */
	if (!fullscan)
	{
		IndexOptInfo *index = path->indexinfo;
		IndexOptInfo **setidx;
		AttrNumber *setcols;
		RestrictInfo **setquals;
		int			nsets = lion_scan_set_quals(path, &setcols, &setquals);

		if (nsets >= 2)
		{
			LionAndCost setand;
			Cost		setcost;
			Cost		pagecost = lion_generic_page_cost(root, path, loop_count,
														  &costs);
			double		factor;
			int			i;

			setidx = (IndexOptInfo **) palloc(sizeof(IndexOptInfo *) * nsets);
			for (i = 0; i < nsets; i++)
				setidx[i] = index;

			factor = lion_isect_factor(root, index->rel, index, nsets, setcols,
									   (Node **) setquals);
			if (factor != 1.0)
			{
				Selectivity sel = Min(costs.indexSelectivity * factor, 1.0);
				double		tuples = rint(sel * index->rel->tuples /
										  Max(costs.num_sa_scans, 1.0));

				tuples = Max(Min(tuples, index->tuples), 1.0);
				costs.indexTotalCost += (tuples - costs.numIndexTuples) *
					costs.num_sa_scans * cpu_index_tuple_cost;
				costs.numIndexTuples = tuples;
				costs.indexSelectivity = sel;
			}

			setcost = lion_cost_set_and(root, index->rel, nsets, setidx,
										setcols, (Node **) setquals, &setand);
			if (loop_count > 1)
				setcost = setand.cpu +
					index_pages_fetched((setand.leafpages + setand.setpages) *
										loop_count,
										index->pages, (double) index->pages,
										root) *
					costs.spc_random_page_cost / loop_count;
			costs.indexTotalCost += setcost - pagecost;
			costs.indexTotalCost = Max(costs.indexTotalCost,
									   costs.indexStartupCost);
			costs.numIndexPages = Max(setand.leafpages + setand.setpages, 1.0);
			pfree(setidx);
		}
		if (setcols != NULL)
		{
			pfree(setcols);
			pfree(setquals);
		}
	}

	/*
	 * A range walked beside other columns reads the postings of its own
	 * range, which genericcostestimate() prorated by every column's
	 * selectivity together (lion_range_walk_cost()): charge the rest of
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

	/*
	 * No lion index may be read while PostgreSQL 16's old_snapshot_threshold
	 * is set (DESIGN.md §9, lion_check_old_snapshot()): price every path on
	 * one out of the running, the way 16 prices a disabled scan.  A plan that
	 * has nothing else left still gets it, and stops at the scan's ERROR.
	 */
	if (lion_old_snapshot_threshold_active())
	{
		costs.indexStartupCost += disable_cost;
		costs.indexTotalCost += disable_cost;
	}

	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = lion_enable_plain_scan ?
		lion_plain_heap_correlation(root, path, loop_count,
									costs.indexSelectivity) : 0.0;
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

	/*
	 * Summaries (DESIGN.md §32): "on" gives them to every ordered scalar
	 * column of an empty index as it would to a full one.  "auto" decides
	 * from the rows a build sees, and an empty index has none, so it gives
	 * them to nobody - which is what a build of an empty table decides too.
	 */
	{
		uint32		cols = 0;
		int			i;

		if (opts != NULL && opts->summaries == LION_SUMOPT_ON)
			for (i = 0; i < ix.ncolumns; i++)
				if (ix.cols[i].ordered && !ix.cols[i].multikey)
					cols |= ((uint32) 1) << i;
		lion_meta_record_summaries(&meta, cols,
								   opts ? (uint32) opts->summary_tids :
								   LION_DEFAULT_SUMMARY_TIDS);
	}

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
	LionPageGetMeta(BufferGetPage(buf))->version = meta.version;
	LionPageGetMeta(BufferGetPage(buf))->summary_cols = meta.summary_cols;
	LionPageGetMeta(BufferGetPage(buf))->summary_tids = meta.summary_tids;
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
