/*-------------------------------------------------------------------------
 *
 * lion_am.c
 *		The access method: its handler, reloptions, opclass validation, the
 *		table access methods it accepts, and building an empty index.
 *
 * Part of the lion access method, which lion.h declares.  See DESIGN.md
 * section 6.
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
 * one the range strategies 6 .. 9 of §28 and the `<>` of §35 (10) as well.
 */
#define LION_MULTI_STRATEGY_MASK \
	((1 << LION_STRAT_CONTAINS) | (1 << LION_STRAT_OVERLAP) | \
	 (1 << LION_STRAT_CONTAINED) | (1 << LION_STRAT_MATCH))
#define LION_RANGE_STRATEGY_MASK \
	((1 << LION_STRAT_LT) | (1 << LION_STRAT_LE) | \
	 (1 << LION_STRAT_GE) | (1 << LION_STRAT_GT) | (1 << LION_STRAT_NE))

/* Kind of relation options for lion indexes */
static relopt_kind lion_relopt_kind;

/* GUC pg_lion.enable_plain_scan (DESIGN.md §29.11, lioncostestimate()) */
bool		lion_enable_plain_scan = true;

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
 * (DESIGN.md section 10, implemented in lion_plan_*.c and lion_exec_*.c).
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

	/*
	 * DESIGN.md §10, "A GROUP BY in parallel": the fewest container keys a
	 * range of a parallel count covers - a parallel GROUP BY's, and a chunk of
	 * the FK-side join's shared copy (§27).  A testing knob: the regression
	 * suite lowers it to cut a table of a few megabytes into several ranges,
	 * and the planner prices the ranges of the default width whatever it says.
	 */
	DefineCustomIntVariable("pg_lion.parallel_range_keys",
							"Fewest container keys a range of a parallel lion count covers.",
							"A container key is 64 heap blocks; every range walks the groups' entries again.",
							&lion_parallel_range_keys,
							LION_PARALLEL_RANGE_KEYS, 1, INT_MAX,
							PGC_USERSET,
							GUC_NOT_IN_SAMPLE,
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
	 * DESIGN.md §27, "The semi and anti join as a join path": whether the
	 * planner may answer a semi or anti join whose inner side is a fact table
	 * with a lion-indexed fk by looking each outer row's key up in the fact's
	 * posting sets (LionSemiJoin, LionAntiJoin).  pg_lion.enable_count_pushdown
	 * switches it off as well: it is part of the same pushdown.
	 */
	DefineCustomBoolVariable("pg_lion.enable_semijoin",
							 "Enables lion's semi and anti join paths over a fact table's lion-indexed foreign key.",
							 "Off, a semi or anti join is left to the planner's own join methods; pg_lion.enable_count_pushdown = off turns it off too.",
							 &lion_enable_semijoin,
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
	 * DESIGN.md §29.11, "Unions probed": whether the AND of posting sets may
	 * look its running intersection up in the containers of an IN list's or
	 * a multi-key query's union at a key, where that is cheaper than building
	 * the union.  A testing knob: off, every union is built, as it was, and
	 * the answers are the same either way.
	 */
	DefineCustomBoolVariable("pg_lion.enable_union_probe",
							 "Lets a lion AND of posting sets look its few rows up in a union's containers instead of building the union.",
							 "Off, every union a lion AND meets is built; the answers are the same either way.",
							 &lion_enable_union_probe,
							 true,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);

	/*
	 * DESIGN.md §29.11, "Trees probed": the same for a nested tree - an AND
	 * of ORs, an OR of ANDs - evaluated for the running intersection's
	 * members alone.  A testing knob, as the one above.
	 */
	DefineCustomBoolVariable("pg_lion.enable_tree_probe",
							 "Lets a lion AND of posting sets look its few rows up in a nested AND/OR tree's leaves instead of building the tree's container.",
							 "Off, every nested tree a lion AND meets is built at the keys it is sought to; the answers are the same either way.",
							 &lion_enable_tree_probe,
							 true,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);

	/*
	 * DESIGN.md §27, "Probed, then collected": whether an FK-side join that
	 * probes its fact filters may collect them once probing has cost what
	 * collecting would.  A testing knob: off, a probing plan probes to its
	 * end, as it did; the answers are the same either way.
	 */
	DefineCustomBoolVariable("pg_lion.enable_filter_switch",
							 "Lets a lion FK-side join that probes its fact filters collect them part way through, once probing has cost what collecting them would.",
							 "Off, a plan that probes the fact filters probes them to its end; the answers are the same either way.",
							 &lion_enable_filter_switch,
							 true,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);

	/*
	 * DESIGN.md §34: whether a GROUP BY of several lion-indexed columns may be
	 * counted by the decoded walk - three or more columns, and two when it is
	 * cheaper than §20's nested loop.  Off, three or more go to the ordinary
	 * plan and two to the nested loop, as before §34.
	 */
	DefineCustomBoolVariable("pg_lion.enable_decoded_walk",
							 "Lets the count pushdown count a GROUP BY of several lion-indexed columns by decoding each column's values key by key.",
							 "Off, a GROUP BY of three or more columns goes to the ordinary plan and one of two to the nested loop of their entries.",
							 &lion_enable_decoded_walk,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	/*
	 * DESIGN.md §36: whether a GROUP BY ordered by its count under a LIMIT
	 * may count only the groups that can be among the first rows, found by
	 * the entries' own counts.  Off, every group is counted.
	 */
	DefineCustomBoolVariable("pg_lion.enable_topk",
							 "Lets the count pushdown count only the groups that can be among the first rows of a GROUP BY ordered by its count under a LIMIT.",
							 "Off, every group is counted and the Sort and Limit above choose among them.",
							 &lion_enable_topk,
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

	/* ... and the semi and anti join paths (DESIGN.md §27) */
	lion_prev_set_join_pathlist_hook = set_join_pathlist_hook;
	set_join_pathlist_hook = lion_set_join_pathlist;
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
		 * the range comparisons of DESIGN.md §28 and `<>` (§35) - whether it
		 * CAN is a question about the type pair's support function 4, asked
		 * with the groups below; a multi-key one answers the
		 * containment/match strategies and never equality, a range or `<>`
		 * (the keys of one row are not the row's value, so none of them
		 * could be answered from them).
		 */
		stratok = multikey ?
			(oprform->amopstrategy >= 1 &&
			 oprform->amopstrategy <= LION_NSTRATEGIES &&
			 (LION_MULTI_STRATEGY_MASK & (1 << oprform->amopstrategy)) != 0) :
			(oprform->amopstrategy == LION_STRAT_EQUAL ||
			 LION_STRAT_IS_WALK(oprform->amopstrategy));

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
