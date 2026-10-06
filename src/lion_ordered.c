/*-------------------------------------------------------------------------
 *
 * lion_ordered.c
 *		LionOrdered: a CustomScan for
 *
 *			SELECT ... FROM t WHERE <clauses lion indexes answer> [AND ...]
 *			ORDER BY <columns an ordered index (a btree) provides> [LIMIT n]
 *
 *		that builds lion's exact TID set for the WHERE once, walks the btree
 *		in order reading only index tuples, tests each TID against the set
 *		BEFORE it touches the heap, and fetches, rechecks and returns only
 *		the members, until whatever is above it stops pulling.  DESIGN.md
 *		section 30 is the specification.
 *
 *		The ordered side may also be a walk of an ordered scalar column of a
 *		lion index itself, in its key order either way, for an ORDER BY of a
 *		column no btree orders (DESIGN.md §30.11); the lion side is then the
 *		other clauses' set, or nothing at all.
 *
 *		When the btree returns every column the node must produce, the row's
 *		values come from the index tuple and the heap is visited only for
 *		pages the visibility map does not call all-visible, as core's Index
 *		Only Scan does (DESIGN.md §40); and such a walk is offered without an
 *		ORDER BY too, under the name LionBtreeScan, since it pays without one.
 *
 * Nothing here re-implements what core already decides.  The ORDERED side is
 * one of core's own ordered IndexPaths from the relation's path list, so its
 * pathkeys, direction and index quals are core's.  The LION side is one of
 * core's own lion index accesses - a bitmap path's bitmapqual tree, or a
 * plain lion IndexPath - built by running create_index_paths() once more on a
 * scratch copy of the relation that sees only its lion indexes, so exactly
 * what a lion bitmap scan would answer is answered here.  At run time each
 * lion leaf of that tree is a lion_source (lion_scan.c, DESIGN.md §29.3/§29.8)
 * copied into a sorted array of containers, and the tree's ANDs and ORs are
 * container-wise ANDs and ORs of those arrays.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/itup.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/tableam.h"
#if PG_VERSION_NUM >= 200000
#include "access/tableam_indexscan.h"
#endif
#include "access/stratnum.h"
#include "access/visibilitymap.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "common/hashfn.h"
#if PG_VERSION_NUM >= 180000
#include "commands/explain_format.h"
#endif
#include "executor/executor.h"
#include "executor/nodeIndexscan.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/predicate.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/selfuncs.h"
#include "utils/snapmgr.h"
#include "utils/sortsupport.h"
#include "utils/spccache.h"
#include "utils/typcache.h"

#include "lion.h"
#include "lion_compat.h"
#include "lion_count.h"
#include "lion_costs.h"
#include "lion_customscan.h"

/* GUCs, and the hook this file chains (all installed by lion_ordered_init). */
bool		lion_enable_ordered_scan = true;
bool		lion_enable_btree_scan = true;
bool		lion_enable_lazy_set = true;
static double lion_ordered_switch_ratio = 32.0;
static set_rel_pathlist_hook_type lion_prev_set_rel_pathlist_hook = NULL;

/*
 * custom_private of the plan is POSITIONAL, behind a shape marker (as §10's):
 *
 *	LO_PRIV_SHAPE	IntList (LO_PRIV_MAGIC, LO_PRIV_NMEMBERS)
 *	LO_PRIV_ORD		OidList (the ordered index: a btree, or the lion index
 *					whose column is walked, LO_FLAG_LIONWALK)
 *	LO_PRIV_INTS	IntList (scan direction, flags, where a lion column's walk
 *					puts its NULL entry - LION_ORDER_NULLS_* - and the
 *					column it walks, 1-based; 0 and 0 for a btree)
 *	LO_PRIV_ORDCOLS	IntList: the index column (0-based) of each ordered
 *					index qual, in custom_exprs' order
 *	LO_PRIV_TREE	List of IntList, the lion tree in preorder: an inner node
 *					is (LO_NODE_AND or LO_NODE_OR, number of children), a leaf
 *					(LO_NODE_LEAF, number of quals, their index columns ...);
 *					NIL when a lion column's walk has no set and every row it
 *					meets is fetched
 *	LO_PRIV_LEAVES	OidList: each leaf's lion index, in preorder
 *	LO_PRIV_SORT	the path's pathkeys as sort keys over the relation's own
 *					columns, for the fetch-and-sort switch (§30.4): a List of
 *					IntList (attnos), OidList (sort operators), OidList
 *					(collations), IntList (nulls first); NIL when a pathkey
 *					is not a plain column, and then the walk never switches
 *	LO_PRIV_IOCOLS	IntList, index-only mode (LO_FLAG_INDEXONLY, §40): for
 *					each column of the btree, the relation attno it returns,
 *					0 for one it does not (an expression); NIL in heap mode
 *	LO_PRIV_EXPECTED a float8 Const: the entries the planner expects the walk
 *					to visit (the ordered index's selectivity times the
 *					relation's tuples), for the switch's density gate (§40.4);
 *					0 for a lion column's walk
 *
 * and custom_exprs holds five lists:
 *
 *	LO_EXPR_LIONQUALS	every leaf's index quals, key on the left, in leaf
 *						order (heap Vars; the executor substitutes INDEX_VAR)
 *	LO_EXPR_ORDQUALS	the ordered index's index quals, likewise
 *	LO_EXPR_LIONQUAL	the lion side's ORIGINAL qual (implicit AND), the
 *						recheck of an inexact set
 *	LO_EXPR_ORDORIG		the ordered index's original clauses (implicit AND),
 *						the recheck when the index sets xs_recheck
 *	LO_EXPR_VALUEQUAL	a lion column's walk: the restriction clauses on the
 *						walked column alone that the first visible row of an
 *						entry decides for the whole entry (implicit AND; §30.11,
 *						"A filter on the walked column"); NIL otherwise
 */
#define LO_PRIV_MAGIC		0x4c4f5244	/* "LORD" */
#define LO_PRIV_SHAPE		0
#define LO_PRIV_ORD			1
#define LO_PRIV_INTS		2
#define LO_PRIV_ORDCOLS		3
#define LO_PRIV_TREE		4
#define LO_PRIV_LEAVES		5
#define LO_PRIV_SORT		6
#define LO_PRIV_IOCOLS		7
#define LO_PRIV_EXPECTED	8
#define LO_PRIV_NMEMBERS	9

/*
 * The fetch-and-sort switch (DESIGN.md §30.4, "When the walk is not paying"):
 * once the walk of this scan has met LO_SWITCH_RATIO index entries per member
 * it has not met yet - and at least LO_SWITCH_MIN_WALK entries - the members
 * left are fetched and sorted instead.  The ratio is what one heap fetch of a
 * member costs in index entries walked, measured (§30.10: 0.04 us an entry,
 * 1-2 us a fetch, warm); pg_lion.ordered_switch_ratio sets it.  In
 * index-only mode (§40.4) the switch would read heap pages the walk never
 * touches, so there it is considered only while the walk has met markedly
 * fewer members than a uniform spread of them over the entries it is expected
 * to visit would have given it: LO_SWITCH_DENSITY of that count.
 */
#define LO_SWITCH_RATIO		32
#define LO_SWITCH_MIN_WALK	10000
#define LO_SWITCH_DENSITY	0.5

/*
 * What the walk spends on each btree entry it meets (DESIGN.md §30.3,
 * §40.3): the btree step, the set probe (a binary search over the container
 * keys and a membership test) and the met-before mark, in cpu_tuple_costs
 * over the one cpu_operator_cost indextotalcost does not hold.  Measured on
 * the quick benchmark's 5M-row table (§40.3): a full covering walk takes
 * 670 to 890 ms, 170 ns an entry, against 85 ns a row for a sequential scan
 * with its filter, which the planner charges cpu_tuple_cost +
 * cpu_operator_cost; one cpu_operator_cost for the probe was a fifth of that
 * for twice the work, and chose the walk over plans it ran three to four
 * times slower than.
 */
#define LO_PROBE_TUPLES		2

/*
 * The set evaluated lazily (DESIGN.md §30.4, "The set, lazily"), until its
 * probes have cost what building it would: a probe of a stream is a unit of
 * work for each of its posting sets, starting a stream again behind where it
 * stands LO_LAZY_RESTART_WORK more for each, and the build reads each of the
 * sets' containers once - the budget, never below LO_LAZY_MIN_WORK.  A walk
 * that has met LO_LAZY_MAX_WALK entries is built for too, so that the early
 * stop and the fetch-and-sort switch, which need the set's size, can act on
 * it; as is one whose memo has taken half of hash_mem.
 */
#define LO_LAZY_MIN_WORK		64
#define LO_LAZY_RESTART_WORK	4
#define LO_LAZY_MAX_WALK		(4 * LO_SWITCH_MIN_WALK)

/*
 * ... and a walk that does not go in heap order is built for at once: once
 * LO_LAZY_ORDER_KEYS keys have been evaluated, if one in LO_LAZY_BACK_SHARE
 * of them or more lay behind the key evaluated before it.  A btree walked in
 * an order the heap does not follow - a score, a random key - hands out TIDs
 * at random container keys, half of them behind the last, and each of those
 * seeks every set of the tree again by a descent: a probe of a key cost as
 * much as eight containers of the build, and the probes of 2,012 keys of a
 * 5M-row table (a 0.67% filter, ORDER BY a random score LIMIT 100) came to
 * 29 ms where the build and the walk took 10 (2026-09-30).  A lion column's
 * walk goes back only between entries, and a btree correlated with the heap
 * seldom does.
 */
#define LO_LAZY_ORDER_KEYS		64
#define LO_LAZY_BACK_SHARE		4

#define LO_EXPR_LIONQUALS	0
#define LO_EXPR_ORDQUALS	1
#define LO_EXPR_LIONQUAL	2
#define LO_EXPR_ORDORIG		3
#define LO_EXPR_VALUEQUAL	4
#define LO_EXPR_NLISTS		5

#define LO_NODE_LEAF		0
#define LO_NODE_AND			1
#define LO_NODE_OR			2

#define LO_FLAG_LOSSY		0x0001	/* a lion index clause was lossy */
#define LO_FLAG_LIONWALK	0x0002	/* the order is a lion column's walk */
#define LO_FLAG_INDEXONLY	0x0004	/* the values come from the btree (§40) */
#define LO_FLAG_HEAPRECHECK	0x0008	/* the lion recheck reads the heap tuple */

/* ---------------------------------------------------------------------
 * The TID set
 * --------------------------------------------------------------------- */

/*
 * A set of heap TIDs as containers (§3) sorted by container key.  A NULL
 * container means "every TID of this key's 64 heap blocks": what the set
 * degrades to when it outgrows hash_mem (DESIGN.md §30.4).
 */
typedef struct LionTidSet
{
	int			n;
	int			cap;
	int			nmerged;		/* entries the last lo_set_finish() left */
	uint32	   *keys;
	LionContainer **conts;
	bool		sorted;			/* keys strictly ascending */
} LionTidSet;

/* Bytes of directory charged per container key, on top of the container. */
#define LO_ENTRY_BYTES		((Size) (sizeof(uint32) + sizeof(LionContainer *) + 4))

/*
 * A directory of pieces (lo_set_push()) is merged when it is full only from
 * this size on: below it the pieces cost less than the merges would.
 */
#define LO_MERGE_MIN		1024

/* ---------------------------------------------------------------------
 * Executor state
 * --------------------------------------------------------------------- */

/*
 * One scan key of a leaf, for the lazy set: its posting sets, located once
 * and let go of (no pin is held between rows, §30.5), the tree over them
 * (lion_scankey_sets()), and a stream of that tree's containers which is only
 * ever sought forward - started again when a probe is behind it.
 */
typedef struct LoProbeKey
{
	int			nsets;
	LionPostingSet *sets;
	LionKeyNode *tree;			/* NULL: the key selects nothing */
	MemoryContext cxt;			/* the stream's */
	LionSetStream *stream;		/* NULL until the first probe */
	uint32		last;			/* the container key last sought */
} LoProbeKey;

typedef struct LoLeaf
{
	Oid			indexoid;
	Relation	index;
	List	   *quals;			/* fixed: INDEX_VAR on the left */
	ScanKey		keys;
	int			nkeys;
	IndexRuntimeKeyInfo *rtkeys;
	int			nrtkeys;
	LoProbeKey *pkeys;			/* [nkeys] while the set is lazy, else NULL */
} LoLeaf;

typedef struct LoNode
{
	int			kind;
	int			nchild;
	struct LoNode **child;
	LoLeaf	   *leaf;
} LoNode;

/*
 * What the lazy set is at one container key the walk met: its members there,
 * and the ones this scan's walk has met.
 */
typedef struct LoMemoEnt
{
	uint32		ckey;
	char		status;
	LionContainer *c;			/* NULL: no member at this key */
	uint64	   *visited;		/* in scancxt, NULL until a member is met */
} LoMemoEnt;

#define SH_PREFIX		lo_memo
#define SH_ELEMENT_TYPE	LoMemoEnt
#define SH_KEY_TYPE		uint32
#define SH_KEY			ckey
#define SH_HASH_KEY(tb, key)	murmurhash32(key)
#define SH_EQUAL(tb, a, b)		((a) == (b))
#define SH_SCOPE		static inline
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

/* A member fetched by the fetch-and-sort switch, with its sort keys. */
typedef struct LoSortRow
{
	HeapTuple	tup;
	Datum	   *vals;
	bool	   *nulls;
} LoSortRow;

typedef struct LionOrderedState
{
	CustomScanState css;

	/* the ordered index */
	Oid			ordoid;
	Relation	ordidx;
	ScanDirection dir;
	ScanKey		okeys;
	int			nokeys;
	IndexRuntimeKeyInfo *ortkeys;
	int			nortkeys;
	ExprContext *ortcxt;		/* its run-time keys' values live here */
	IndexScanDesc scan;
	bool		started;		/* the walk is positioned for this scan */
	bool		done;

	/*
	 * ... or a lion column's walk (DESIGN.md §30.11): the column, where its
	 * NULL entry goes, the walk of this scan, and the fetch of what it meets
	 * (the TID it hands out is a HOT chain's root, as a btree's is).
	 */
	bool		lionwalk;
	AttrNumber	walkattno;
	int			walknulls;
	LionOrderWalk *owalk;
	MemoryContext walkcxt;
	ItemPointerData walktid;
#if PG_VERSION_NUM < 200000
	struct IndexFetchTableData *fetch;
#endif
	uint64		walkkeys;		/* EXPLAIN ANALYZE: keys the walks read */

	/*
	 * ... and its filter on the walked column (§30.11, "A filter on the
	 * walked column"): the entry whose first visible row has decided for it,
	 * and how many entries were passed over for failing.
	 */
	ExprState  *valuequal;
	int64		valueentry;
	uint64		valueskips;

	/*
	 * Index-only mode (DESIGN.md §40): the row's values come from the btree's
	 * index tuple, through the mapping of its columns to the relation's, and
	 * the heap is visited - into a slot of its own, for visibility alone - on
	 * pages the visibility map does not call all-visible.  A name column the
	 * btree stores as a cstring is copied into a Name of the node's.
	 */
	bool		indexonly;
	bool		lionheaprecheck;	/* the btree lacks the lion qual's columns */
	int			niocols;		/* the btree's columns */
	AttrNumber *iocols;			/* [niocols] the attno each returns, or 0 */
	bool	   *ionames;		/* [niocols] stored as cstring, returned as name */
	NameData   *ionamebuf;		/* [niocols] where those are copied to */
	Datum	   *iovals;			/* [niocols] the index tuple, deformed */
	bool	   *ionulls;
	TupleTableSlot *tabslot;	/* the heap tuple, for its visibility */
	Buffer		vmbuffer;		/* the visibility map page last read */

	/* the lion side; noset: none, every row the walk meets is fetched */
	bool		noset;
	LoNode	   *tree;
	LoLeaf	   *leaves;
	int			nleaves;
	ExprContext *lrtcxt;		/* the leaves' run-time keys' values */
	bool		lossyqual;
	Bitmapset  *lionparams;		/* Param ids the lion quals use */
	ExprState  *lionrecheck;
	ExprState  *ordrecheck;

	/* the set, and what building it costs */
	MemoryContext setcxt;		/* the set; reset per build */
	MemoryContext buildcxt;		/* sources while building */
	LionTidSet *set;			/* NULL: not built for this scan yet */
	List	   *live;			/* sets alive during a build */
	Size		bytes;
	Size		limit;
	bool		degraded;
	bool		exact;
	uint64		members;		/* members of the set, when not degraded */

	/*
	 * ... or the set evaluated only at the container keys the walk meets
	 * (DESIGN.md §30.4, "The set, lazily"), and what it is at each of them
	 * remembered; built as above once the probes have cost what that would.
	 */
	bool		lazyok;			/* every leaf's keys are sets to seek in */
	bool		lazy;			/* the set is lazy now */
	bool		lazyempty;		/* ... and selects nothing at all */
	MemoryContext lazycxt;		/* the leaves' sets and the memo */
	MemoryContext probecxt;		/* one key's evaluation */
	lo_memo_hash *memo;
	Size		memobytes;
	double		lazywork;
	double		lazybudget;
	double		lazymembers;	/* at most this many members: the sets' hints */
	uint64		lazyevals;		/* keys evaluated since the set was started */
	uint64		lazyback;		/* ... that lay behind the key before them */
	uint32		lazylast;		/* the key evaluated last */
	uint64		lazykeys;		/* EXPLAIN ANALYZE: keys evaluated */

	/*
	 * This scan's walk (DESIGN.md §30.4): which members it has met, one
	 * bitmap per container key touched, so that a member counts once however
	 * often the walk meets its TID; and the fetch-and-sort switch.
	 */
	MemoryContext scancxt;		/* reset per scan */
	uint64	  **visited;		/* [set->n], NULL until touched */
	Size		visitedbytes;
	bool		novisit;		/* over budget: no early stop, no switch */
	uint64		distinct;		/* members met in this scan */
	uint64		scanwalked;		/* entries walked in this scan */
	int			nsort;			/* sort keys; 0: the walk never switches */
	double		switchratio;	/* entries walked a member's fetch is worth */
	double		expectwalk;		/* entries the planner expects the walk to visit */
	AttrNumber *sortattnos;
	SortSupport sortkeys;
	bool		noswitch;		/* the switch gave up for this scan */
	bool		sorting;		/* switched: returning srt[] */
	MemoryContext sortcxt;		/* srt[] and its tuples, under scancxt */
	struct LoSortRow *srt;
	int			nsrt;
	int			srtpos;

	/* EXPLAIN ANALYZE */
	uint64		walked;
	uint64		hits;
	uint64		fetched;
	uint64		removed;
	uint64		builds;
	int			ncont;
	uint64		scans;			/* walks started */
	uint64		switches;		/* scans that switched */
	uint64		sortfetched;	/* members fetched by the switch */
} LionOrderedState;

static Plan *lo_plan_path(PlannerInfo *root, RelOptInfo *rel,
						  CustomPath *best_path, List *tlist,
						  List *clauses, List *custom_plans);
static Node *lo_create_state(CustomScan *cscan);
static void lo_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *lo_exec(CustomScanState *node);
static void lo_end(CustomScanState *node);
static void lo_rescan(CustomScanState *node);
static void lo_scan_reset(LionOrderedState *st);
static void lo_explain(CustomScanState *node, List *ancestors,
					   ExplainState *es);

static const CustomPathMethods lo_path_methods = {
	.CustomName = "LionOrdered",
	.PlanCustomPath = lo_plan_path,
};

static const CustomScanMethods lo_scan_methods = {
	.CustomName = "LionOrdered",
	.CreateCustomScanState = lo_create_state,
};

/*
 * A btree's walk that claims no order (DESIGN.md §40): the same plan and the
 * same executor under a name that does not promise one.
 */
static const CustomPathMethods lo_btree_path_methods = {
	.CustomName = "LionBtreeScan",
	.PlanCustomPath = lo_plan_path,
};

static const CustomScanMethods lo_btree_scan_methods = {
	.CustomName = "LionBtreeScan",
	.CreateCustomScanState = lo_create_state,
};

static const CustomExecMethods lo_exec_methods = {
	.CustomName = "LionOrdered",
	.BeginCustomScan = lo_begin,
	.ExecCustomScan = lo_exec,
	.EndCustomScan = lo_end,
	.ReScanCustomScan = lo_rescan,
	.ExplainCustomScan = lo_explain,
};

/* ---------------------------------------------------------------------
 * Planner
 * --------------------------------------------------------------------- */

/*
 * Can the executor turn this index qual into a scan key by substituting an
 * INDEX_VAR Var for its key operand (lo_fix_qual())?  Core's index quals are
 * `key op value`, `key op ANY (array)`, `key IS [NOT] NULL` or a row
 * comparison; the last is not taken in v1 (DESIGN.md §30.8).
 */
static bool
lo_qual_ok(Node *clause)
{
	if (IsA(clause, OpExpr))
		return list_length(((OpExpr *) clause)->args) == 2;
	if (IsA(clause, ScalarArrayOpExpr))
		return true;
	if (IsA(clause, NullTest))
		return true;
	return false;
}

/* Every index qual of an IndexPath is one lo_qual_ok() takes. */
static bool
lo_indexpath_ok(IndexPath *ipath)
{
	ListCell   *lc;

	foreach(lc, ipath->indexclauses)
	{
		IndexClause *iclause = lfirst_node(IndexClause, lc);
		ListCell   *lc2;

		if (iclause->indexcols != NIL)
			return false;
		foreach(lc2, iclause->indexquals)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc2);

			if (!lo_qual_ok((Node *) rinfo->clause))
				return false;
		}
	}
	return true;
}

/*
 * A lion access the node can build a set from: a tree of BitmapAnd/BitmapOr
 * over lion IndexPaths, each with index clauses the executor can turn into
 * scan keys - or none at all, for a partial index whose predicate the query
 * implies (the source then walks the whole index, §29.3).
 */
static bool
lo_lion_tree_ok(Path *path, Oid lionam)
{
	ListCell   *lc;

	if (IsA(path, IndexPath))
	{
		IndexPath  *ipath = (IndexPath *) path;

		if (ipath->indexinfo->relam != lionam || ipath->indexinfo->hypothetical)
			return false;
		if (ipath->path.param_info != NULL || ipath->indexorderbys != NIL)
			return false;
		if (ipath->indexclauses == NIL && ipath->indexinfo->indpred == NIL)
			return false;
		return lo_indexpath_ok(ipath);
	}
	if (IsA(path, BitmapAndPath))
	{
		foreach(lc, ((BitmapAndPath *) path)->bitmapquals)
			if (!lo_lion_tree_ok((Path *) lfirst(lc), lionam))
				return false;
		return true;
	}
	if (IsA(path, BitmapOrPath))
	{
		foreach(lc, ((BitmapOrPath *) path)->bitmapquals)
			if (!lo_lion_tree_ok((Path *) lfirst(lc), lionam))
				return false;
		return true;
	}
	return false;
}

/*
 * The lion side's ORIGINAL qual, as create_bitmap_subplan() computes its
 * `qual`: an IndexPath's clauses, plus its index predicate where they do not
 * imply it; a BitmapAnd's children's, concatenated; a BitmapOr's as one OR
 * (or nothing, when an arm has none).  Also collects the RestrictInfos of the
 * index clauses of the leaves reached from the root through ANDs only - the
 * clauses the lion qual implies by construction; never an OR arm's.
 */
static List *
lo_lion_qual(Path *path, List **rinfos, bool *lossy)
{
	List	   *qual = NIL;
	ListCell   *lc;

	if (IsA(path, IndexPath))
	{
		IndexPath  *ipath = (IndexPath *) path;

		foreach(lc, ipath->indexclauses)
		{
			IndexClause *iclause = lfirst_node(IndexClause, lc);

			qual = lappend(qual, iclause->rinfo->clause);
			*rinfos = lappend(*rinfos, iclause->rinfo);
			if (iclause->lossy)
				*lossy = true;
		}
		foreach(lc, ipath->indexinfo->indpred)
		{
			Expr	   *pred = (Expr *) lfirst(lc);

			if (!predicate_implied_by(list_make1(pred), qual, false))
				qual = lappend(qual, pred);
		}
		return qual;
	}
	if (IsA(path, BitmapAndPath))
	{
		foreach(lc, ((BitmapAndPath *) path)->bitmapquals)
			qual = list_concat_unique(qual,
									  lo_lion_qual((Path *) lfirst(lc),
												   rinfos, lossy));
		return qual;
	}
	if (IsA(path, BitmapOrPath))
	{
		List	   *arms = NIL;
		bool		consttrue = false;

		foreach(lc, ((BitmapOrPath *) path)->bitmapquals)
		{
			/*
			 * An arm's own clauses are NOT implied by the OR: core builds each
			 * arm with the other top-level clauses at hand, so an arm may
			 * reuse the RestrictInfo of `a = 3` from `a = 3 AND (b = 5 OR
			 * c = 7)`, and dropping `a = 3` from the filter because of it
			 * returned rows with a <> 3 (2026-09-25 review).  Only the
			 * leaves on AND paths from the root are collected; anything else
			 * leaves the filter only when predicate_implied_by() the whole
			 * lion qual, as in create_bitmap_scan_plan().
			 */
			List	   *armrinfos = NIL;
			List	   *sub = lo_lion_qual((Path *) lfirst(lc), &armrinfos, lossy);

			if (sub == NIL)
				consttrue = true;
			else
				arms = lappend(arms, make_ands_explicit(sub));
		}
		if (consttrue || arms == NIL)
			return NIL;
		if (list_length(arms) == 1)
			return list_make1(linitial(arms));
		return list_make1(make_orclause(arms));
	}
	elog(ERROR, "LionOrdered: unexpected lion path type %d", (int) nodeTag(path));
	return NIL;					/* keep compiler quiet */
}

/*
 * The lion tree for custom_private, in preorder, with every leaf's index and
 * its index quals (key on the left) and their index columns.
 */
static void
lo_lion_tree(Path *path, List **tree, List **leaves, List **quals)
{
	ListCell   *lc;

	if (IsA(path, IndexPath))
	{
		IndexPath  *ipath = (IndexPath *) path;
		List	   *node = list_make2_int(LO_NODE_LEAF, 0);
		int			n = 0;

		foreach(lc, ipath->indexclauses)
		{
			IndexClause *iclause = lfirst_node(IndexClause, lc);
			ListCell   *lc2;

			foreach(lc2, iclause->indexquals)
			{
				RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc2);

				*quals = lappend(*quals, rinfo->clause);
				node = lappend_int(node, iclause->indexcol);
				n++;
			}
		}
		lsecond_int(node) = n;
		*tree = lappend(*tree, node);
		*leaves = lappend_oid(*leaves, ipath->indexinfo->indexoid);
		return;
	}
	else
	{
		List	   *children = IsA(path, BitmapAndPath) ?
			((BitmapAndPath *) path)->bitmapquals :
			castNode(BitmapOrPath, path)->bitmapquals;

		*tree = lappend(*tree,
						list_make2_int(IsA(path, BitmapAndPath) ?
									   LO_NODE_AND : LO_NODE_OR,
									   list_length(children)));
		foreach(lc, children)
			lo_lion_tree((Path *) lfirst(lc), tree, leaves, quals);
	}
}

/* The ordered index's non-lossy index clauses: what its walk answers. */
static List *
lo_ord_rinfos(IndexPath *ord)
{
	List	   *ordrinfos = NIL;
	ListCell   *lc;

	foreach(lc, ord->indexclauses)
	{
		IndexClause *iclause = lfirst_node(IndexClause, lc);

		if (!iclause->lossy)
			ordrinfos = lappend(ordrinfos, iclause->rinfo);
	}
	return ordrinfos;
}

/*
 * Which restriction clauses the node must still evaluate on a fetched row
 * (DESIGN.md §30.2, step 4): not the ones the ordered walk answers (the
 * ordered index's non-lossy index clauses, or the range a lion column's walk
 * is bounded by), not the lion leaves' index clauses, not what the lion qual
 * implies.
 */
static List *
lo_residual(List *rinfos, List *ordrinfos, List *lionrinfos, List *lionqual)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, rinfos)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		if (rinfo->pseudoconstant)
			continue;			/* core's gating Result evaluates it */
		if (list_member_ptr(ordrinfos, rinfo) ||
			list_member_ptr(lionrinfos, rinfo))
			continue;
		if (lionqual != NIL &&
			predicate_implied_by(list_make1(rinfo->clause), lionqual, false))
			continue;
		result = lappend(result, rinfo);
	}
	return result;
}

/*
 * The clauses of a lion access's qual that a partial index's predicate
 * implies - core's own test for which restriction clauses a partial index
 * answers by itself (check_index_predicates()), asked of the lion side: an
 * index clause of a lion leaf, or a partial lion index's own predicate.  A
 * walk of that index meets only rows that satisfy them, so among the rows it
 * walks they select nothing.
 */
static List *
lo_pred_implied(IndexOptInfo *idx, List *lionqual)
{
	List	   *result = NIL;
	ListCell   *lc;

	if (idx->indpred == NIL)
		return NIL;
	foreach(lc, lionqual)
	{
		Expr	   *clause = (Expr *) lfirst(lc);

		if (!contain_mutable_functions((Node *) clause) &&
			predicate_implied_by(list_make1(clause), idx->indpred, false))
			result = lappend(result, clause);
	}
	return result;
}

/*
 * The columns the node must produce for one (ordered path, lion access) pair
 * (DESIGN.md §40.1), offset as pull_varattnos() offsets them: every Var of
 * rel in its target; in the residual filter; in the ordered index's own
 * clauses (the recheck under xs_recheck, EvalPlanQual's); and in the lion
 * qual when a lion index clause is lossy at plan time - every member is then
 * rechecked, and a walk that read the heap for each would be heap mode under
 * another name.  A lion qual that is not lossy is rechecked only when its set
 * turns out inexact or degraded at run time, and then from the heap tuple
 * where the btree lacks its columns (§40.4), so its Vars are not required.  A
 * system column (ctid, which a row mark adds) or a whole-row Var is among
 * the Vars as one no index returns.
 */
static Bitmapset *
lo_needed_attrs(RelOptInfo *rel, List *residual, List *ordclauses,
				List *lionqual, bool lossy)
{
	Bitmapset  *attrs = NULL;
	ListCell   *lc;

	pull_varattnos((Node *) rel->reltarget->exprs, rel->relid, &attrs);
	foreach(lc, residual)
		pull_varattnos((Node *) lfirst_node(RestrictInfo, lc)->clause,
					   rel->relid, &attrs);
	foreach(lc, ordclauses)
		pull_varattnos((Node *) lfirst_node(IndexClause, lc)->rinfo->clause,
					   rel->relid, &attrs);
	if (lossy)
		pull_varattnos((Node *) lionqual, rel->relid, &attrs);
	return attrs;
}

/*
 * The relation columns a btree returns (DESIGN.md §40.1), offset as
 * pull_varattnos() offsets them: a plain column the AM can return; an
 * expression column matches no Var (it may still give the order, as today,
 * but no value).  *cols is the relation attno each index column returns, 0
 * for none - the mapping the executor fills the scan tuple through.
 */
static Bitmapset *
lo_returned_attrs(IndexOptInfo *idx, List **cols)
{
	Bitmapset  *returned = NULL;
	int			i;

	*cols = NIL;
	for (i = 0; i < idx->ncolumns; i++)
	{
		int			attno = idx->indexkeys[i];

		if (attno > 0 && idx->canreturn[i])
			returned = bms_add_member(returned,
									  attno - FirstLowInvalidHeapAttributeNumber);
		else
			attno = 0;
		*cols = lappend_int(*cols, attno);
	}
	return returned;
}

/*
 * Does the btree return every column in `needed` (DESIGN.md §40.1)?  When it
 * does, *iocols is the mapping of lo_returned_attrs().
 */
static bool
lo_index_only(IndexOptInfo *idx, Bitmapset *needed, List **iocols)
{
	List	   *cols;

	if (!enable_indexonlyscan)
		return false;
	if (!bms_is_subset(needed, lo_returned_attrs(idx, &cols)))
		return false;
	*iocols = cols;
	return true;
}

/*
 * The same restriction clause?  The RestrictInfo itself, or one spelling the
 * same clause: 18's matching of `k = 1 OR k = 7` to an index makes a fresh
 * `k = ANY ('{1,7}')` RestrictInfo for EACH index it matches, so the btree's
 * and the lion index's clause for one OR are two objects (DESIGN.md §40.2).
 */
static bool
lo_same_clause(RestrictInfo *a, RestrictInfo *b)
{
	return a == b || equal(a->clause, b->clause);
}

/*
 * Does the lion access answer a clause the btree's own index clauses do not
 * (DESIGN.md §40.2)?  One whose every leaf's clauses - an OR arm's too - are
 * among the btree's adds nothing to its walk: every entry walked is a member.
 * A leaf with no clauses is a partial lion index walked whole, whose
 * predicate is what it selects by: it adds.
 */
static bool
lo_lion_adds(Path *lion, List *ordclauses)
{
	ListCell   *lc;

	if (IsA(lion, IndexPath))
	{
		if (((IndexPath *) lion)->indexclauses == NIL)
			return true;
		foreach(lc, ((IndexPath *) lion)->indexclauses)
		{
			IndexClause *iclause = lfirst_node(IndexClause, lc);
			ListCell   *lc2;
			bool		found = false;

			foreach(lc2, ordclauses)
				if (lo_same_clause(lfirst_node(IndexClause, lc2)->rinfo,
								   iclause->rinfo))
					found = true;
			if (!found)
				return true;
		}
		return false;
	}
	foreach(lc, IsA(lion, BitmapAndPath) ?
			((BitmapAndPath *) lion)->bitmapquals :
			castNode(BitmapOrPath, lion)->bitmapquals)
	{
		if (lo_lion_adds((Path *) lfirst(lc), ordclauses))
			return true;
	}
	return false;
}

/*
 * The price of one (ordered path, lion access) pair (DESIGN.md §30.3), and the
 * size its set is expected to have.  rows is what the node returns, as lion's
 * estimates see it (lion_probe_rel_rows()).  In index-only mode (§40.3) the
 * members' heap I/O is only the share of pages not all-visible.
 */
static void
lo_cost(PlannerInfo *root, RelOptInfo *rel, IndexPath *ord, Path *lion,
		List *lionrinfos, List *lionqual, List *residual, double rows,
		bool indexonly, Cost *startup_p, Cost *total_p, double *setbytes)
{
	Cost		lioncost;
	Selectivity sel;
	Selectivity selwalk;
	List	   *shared = NIL;
	ListCell   *lc;
	double		tuples = Max(rel->tuples, 1.0);
	double		pages = Max((double) rel->pages, 1.0);
	double		members;
	double		ncont;
	double		walked;
	double		fetched;
	double		spc_random;
	double		spc_seq;
	double		max_io;
	double		min_io;
	double		pages_corr;
	double		corr = 0.0;
	QualCost	qcost;
	Cost		startup;
	Cost		run;

	cost_bitmap_tree_node(lion, &lioncost, &sel);
	members = clamp_row_est(sel * tuples);
	ncont = Min(ceil(pages / LION_BLOCKS_PER_CONTAINER), members);
	*setbytes = ncont * (LION_CONTAINER_HDRSZ + LO_ENTRY_BYTES) +
		Min(members * sizeof(uint16), ncont * LION_BITSET_BYTES);

	/* the lookups, and the copy of their answer into the set */
	startup = lioncost + ncont * cpu_operator_cost;

	/* the walk: the ordered index's own cost, and the probe of each entry */
	walked = clamp_row_est(ord->indexselectivity * tuples);
	run = ord->indextotalcost +
		walked * (LO_PROBE_TUPLES * cpu_tuple_cost + cpu_operator_cost);

	/*
	 * The members' heap fetches, priced as cost_index() prices them: of the
	 * entries walked, the share the lion access selects BEYOND what the
	 * ordered index's own quals already did.  A clause both of them answer -
	 * `g = 5` over a btree on (g, k) and a lion index on g - was counted
	 * twice: the walk meets only g = 5 entries, every one a member, yet the
	 * fetches were priced at 1% of them, and the node (a plain index scan
	 * plus the lion lookups) beat a bitmap scan and Sort that the planner's
	 * own price for that index scan had rejected, 721 against 13,909 on 1M
	 * rows (2026-09-25 second review).  So is a clause the ordered index's
	 * PREDICATE implies: a partial btree on (k) WHERE s <> '' walks only
	 * rows with s <> '', and pricing the fetches at s <> ''s share of them
	 * chose the node over a bitmap scan it ran twice as slow as.
	 */
	foreach(lc, ord->indexclauses)
	{
		IndexClause *iclause = lfirst_node(IndexClause, lc);
		ListCell   *lc2;

		foreach(lc2, lionrinfos)
		{
			if (lo_same_clause(lfirst_node(RestrictInfo, lc2), iclause->rinfo))
			{
				shared = lappend(shared, iclause->rinfo);
				break;
			}
		}
	}
	foreach(lc, lo_pred_implied(ord->indexinfo, lionqual))
	{
		Expr	   *clause = (Expr *) lfirst(lc);
		ListCell   *lc2;
		bool		dup = false;

		foreach(lc2, shared)
		{
			Node	   *c = (Node *) lfirst(lc2);

			/* an index clause's RestrictInfo, or an implied bare clause */
			if (IsA(c, RestrictInfo))
				c = (Node *) ((RestrictInfo *) c)->clause;
			if (equal(c, clause))
				dup = true;
		}
		if (!dup)
			shared = lappend(shared, clause);
	}
	selwalk = sel;
	if (shared != NIL)
	{
		Selectivity both = clauselist_selectivity(root, shared, rel->relid,
												  JOIN_INNER, NULL);

		if (both > 0)
			selwalk = Min(sel / both, 1.0);
	}
	fetched = clamp_row_est(walked * selwalk);
	get_tablespace_page_costs(rel->reltablespace, &spc_random, &spc_seq);
	{
		Cost		s;
		Cost		t;
		Selectivity isel;
		double		ipages;

		ord->indexinfo->amcostestimate(root, ord, 1.0, &s, &t, &isel, &corr,
									   &ipages);
	}
	max_io = index_pages_fetched(fetched, rel->pages,
								 (double) ord->indexinfo->pages, root);
	pages_corr = ceil(Min(fetched, walked / tuples * pages));
	if (indexonly)
	{
		/*
		 * Only the pages the visibility map does not call all-visible are
		 * visited, as cost_index() prices an index-only scan's; each member
		 * pays the map's test (DESIGN.md §40.3).
		 */
		max_io = ceil(max_io * (1.0 - rel->allvisfrac));
		pages_corr = ceil(pages_corr * (1.0 - rel->allvisfrac));
		run += fetched * cpu_operator_cost;
	}
	max_io *= spc_random;
	min_io = (pages_corr > 0) ? spc_random + (pages_corr - 1) * spc_seq : 0;
	run += max_io + corr * corr * (min_io - max_io);

	cost_qual_eval(&qcost, residual, root);
	startup += qcost.startup + rel->reltarget->cost.startup;
	run += fetched * (cpu_tuple_cost + qcost.per_tuple) +
		rows * rel->reltarget->cost.per_tuple;

	*startup_p = startup;
	*total_p = startup + run;
}

/*
 * Is rel one the node may scan at all (DESIGN.md §30.1)?  A plain table, or a
 * leaf partition reached through its parent (§30.11): the parent's Append or
 * MergeAppend is core's, over whatever path each partition has.
 */
static bool
lo_rel_ok(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	ListCell   *lc;

	if ((rel->reloptkind != RELOPT_BASEREL &&
		 rel->reloptkind != RELOPT_OTHER_MEMBER_REL) ||
		rte->rtekind != RTE_RELATION)
		return false;
	if (rte->inh || rte->tablesample != NULL)
		return false;
	if (rte->relkind != RELKIND_RELATION && rte->relkind != RELKIND_MATVIEW)
		return false;
	if (rte->securityQuals != NIL)
		return false;

	/*
	 * Proven empty already (a constant-false or NULL restriction, or
	 * constraint exclusion): core costs no index path for it, and neither
	 * does the node - the planner leaves such a relation's pages out of
	 * root->total_table_pages, which cost_index() asserts they are in.
	 */
	if (IS_DUMMY_REL(rel))
		return false;

	/*
	 * Not the target of an UPDATE, DELETE or MERGE (reached through a merge
	 * join's ordered input), nor a partition of one: v1 leaves those scans to
	 * core (DESIGN.md §30.8).
	 */
	if (root->parse->resultRelation == (int) rel->relid ||
		bms_is_member((int) rel->relid, root->all_result_relids))
		return false;
	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		if (rinfo->security_level > 0)
			return false;
	}
	if (rel->indexlist == NIL)
		return false;
	return true;
}

/*
 * A scratch copy of rel that sees only `indexes`, with `exclude` taken out of
 * its restriction clauses, no paths, no join clauses and no parallelism, so
 * that create_index_paths() run on it builds only unparameterized, non-partial
 * paths - in the copy's own path list - and leaves rel untouched (DESIGN.md
 * §30.2).  Each index is copied too when there is an `exclude`, with it taken
 * out of the clauses the index may match, which is how a lion column's walk
 * leaves the range it is bounded by out of the set (§30.11).
 */
static RelOptInfo *
lo_scratch_rel(RelOptInfo *rel, List *indexes, List *exclude)
{
	RelOptInfo *scratch;
	ListCell   *lc;

	scratch = makeNode(RelOptInfo);
	memcpy(scratch, rel, sizeof(RelOptInfo));
	scratch->indexlist = NIL;
	foreach(lc, indexes)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);

		if (exclude != NIL)
		{
			IndexOptInfo *copy = makeNode(IndexOptInfo);

			memcpy(copy, idx, sizeof(IndexOptInfo));
			copy->rel = scratch;
			copy->indrestrictinfo = list_difference_ptr(idx->indrestrictinfo,
														exclude);
			idx = copy;
		}
		scratch->indexlist = lappend(scratch->indexlist, idx);
	}
	scratch->baserestrictinfo = list_difference_ptr(rel->baserestrictinfo,
													exclude);
	scratch->pathlist = NIL;
	scratch->ppilist = NIL;
	scratch->partial_pathlist = NIL;
	scratch->cheapest_startup_path = NULL;
	scratch->cheapest_total_path = NULL;
	scratch->cheapest_parameterized_paths = NIL;
	scratch->joininfo = NIL;
	scratch->has_eclass_joins = false;
	scratch->consider_parallel = false;
	return scratch;
}

/*
 * The lion accesses core builds for rel's restriction clauses less `exclude`:
 * create_index_paths() run on a scratch copy of rel that sees only its lion
 * indexes (DESIGN.md §30.2).
 */
static List *
lo_lion_accesses(PlannerInfo *root, RelOptInfo *rel, List *lionidx,
				 List *exclude, Oid lionam)
{
	RelOptInfo *scratch = lo_scratch_rel(rel, lionidx, exclude);
	List	   *cands = NIL;
	ListCell   *lc;

	if (scratch->baserestrictinfo == NIL)
		return NIL;
	create_index_paths(root, scratch);

#if PG_VERSION_NUM < 180000

	/*
	 * ... and the paths 18's matching builds for an OR of equalities, as the
	 * IN list it spells (lion_or_list_paths(), DESIGN.md §29.11): the lion
	 * side of `k = 1 OR k = 7` is then the lion side of `k IN (1, 7)`.
	 */
	scratch->pathlist = list_concat(scratch->pathlist,
									lion_or_list_paths(root, scratch,
													   scratch->indexlist));
#endif

	foreach(lc, scratch->pathlist)
	{
		Path	   *p = (Path *) lfirst(lc);
		Path	   *q = NULL;

		if (p->param_info != NULL)
			continue;
		if (IsA(p, BitmapHeapPath))
			q = ((BitmapHeapPath *) p)->bitmapqual;
		else if (IsA(p, IndexPath))
			q = p;
		if (q != NULL && lo_lion_tree_ok(q, lionam))
			cands = lappend(cands, q);
	}
	return cands;
}

/*
 * The IndexPaths core builds for one btree of rel over the relation's
 * restriction clauses, whether or not it finds their order useful (DESIGN.md
 * §40.2): create_index_paths() on a scratch copy of rel that sees that index
 * alone, so that its paths compete with nothing else's - in the relation
 * itself the full walk of a covering index with no qual, which core builds as
 * an index-only scan, loses to the sequential scan and never reaches the
 * path list.  One add_path() still dropped for a bitmap scan of the same
 * index is taken from that scan's one-leaf bitmapqual.
 */
static List *
lo_btree_paths(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *idx)
{
	RelOptInfo *scratch = lo_scratch_rel(rel, list_make1(idx), NIL);
	List	   *result = NIL;
	ListCell   *lc;

	create_index_paths(root, scratch);
	foreach(lc, scratch->pathlist)
	{
		Path	   *p = (Path *) lfirst(lc);
		IndexPath  *ipath;

		if (IsA(p, BitmapHeapPath))
			p = ((BitmapHeapPath *) p)->bitmapqual;
		if (!IsA(p, IndexPath) || p->param_info != NULL)
			continue;
		ipath = (IndexPath *) p;
		if (ipath->indexinfo != idx || ipath->indexorderbys != NIL ||
			!lo_indexpath_ok(ipath))
			continue;
		if (!list_member_ptr(result, ipath))
			result = lappend(result, ipath);
	}
	return result;
}

/*
 * A walk of one ordered scalar key column of a lion index in the column's
 * order, as the ordered side of the node (DESIGN.md §30.11).
 */
typedef struct LoWalk
{
	IndexOptInfo *index;
	int			indexcol;		/* 0-based */
	Var		   *var;			/* the column, as rel's Var */
	PathKey    *pathkey;		/* the order it gives: the query's first */
	bool		backward;
	int			nulls;			/* LION_ORDER_NULLS_* */
	List	   *rinfos;			/* the restriction clauses it answers */
	List	   *quals;			/* ... as index quals, the key on the left */
	double		nlist;			/* the values of a list among them, or 0 */
	List	   *vrinfos;		/* the clauses an entry's first visible row
								 * decides for it (lo_walk_value_rinfos()) */
} LoWalk;

/* The walk's custom_private marker (the btree kind's first member is a Path) */
#define LO_WALK_MAGIC		0x4c57414c	/* "LWAL" */

/*
 * Can the walk of `var` answer rinfo: a range comparison of the column - or a
 * `<>`, the hole the walk steps over (DESIGN.md §35) - with
 * a value that does not depend on the row - a Const, a Param, a stable
 * expression, evaluated when the walk starts as an Index Scan's run-time keys
 * are - under the index column's collation, `IS NOT NULL`, or a list, `col =
 * ANY (array)` of such an array?  *qual is the clause with the column on the
 * left, and *list says it is a list.
 *
 * A list is walked a value at a time, each value descended to (DESIGN.md
 * §30.11, "Lists"), which the walk can do only in the directory's own order:
 * so only under the column's own equality for its own type (the opclass's
 * input type), whose probe is the column's comparison.  Left to the set, a
 * list on the walked column was walked from the column's first entry
 * whatever its values, and `here IN ('H10', 'H11') ... ORDER BY here LIMIT
 * 50` read every entry below 'H10' before its first member - until the
 * fetch-and-sort switch fetched all 1,292 of them instead (2026-09-30).
 *
 * The collation is core's rule for an index clause, IndexCollMatchesExprColl()
 * (as lion_match_index() applies it): the walk compares its bound with the
 * directory's keys in the order they were stored in, the index column's
 * collation's, so a comparison under another collation is no bound of it.
 * Taken as one it left the filter too, and `t < 'a' COLLATE "C"` over an
 * "en-x-icu" column returned no rows at all, where 'A0' is below 'a' in "C"
 * but after it in English (2026-09-29 review).  Such a clause stays in the
 * filter, as any other the walk does not answer.
 */
static bool
lo_walk_clause(RestrictInfo *rinfo, Var *var, Oid opfamily, Oid opcintype,
			   Oid idxcoll, Expr **qual, bool *list)
{
	Expr	   *clause = rinfo->clause;

	*list = false;
	if (rinfo->pseudoconstant)
		return false;
	if (IsA(clause, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;
		Node	   *l;
		Node	   *arr;
		int			strategy;
		Oid			lefttype;
		Oid			righttype;

		if (!saop->useOr || list_length(saop->args) != 2)
			return false;
		l = (Node *) linitial(saop->args);
		arr = (Node *) lsecond(saop->args);
		while (IsA(l, RelabelType))
			l = (Node *) ((RelabelType *) l)->arg;
		if (!equal(l, var))
			return false;
		if (contain_var_clause(arr) || contain_volatile_functions(arr) ||
			contain_subplans(arr))
			return false;
		if (OidIsValid(idxcoll) && saop->inputcollid != idxcoll)
			return false;
		if (!op_in_opfamily(saop->opno, opfamily))
			return false;
		get_op_opfamily_properties(saop->opno, opfamily, false, &strategy,
								   &lefttype, &righttype);
		if (strategy != LION_STRAT_EQUAL || lefttype != opcintype ||
			righttype != opcintype)
			return false;
		*qual = clause;
		*list = true;
		return true;
	}
	if (IsA(clause, NullTest))
	{
		NullTest   *nt = (NullTest *) clause;
		Node	   *arg = (Node *) nt->arg;

		while (IsA(arg, RelabelType))
			arg = (Node *) ((RelabelType *) arg)->arg;
		if (nt->nulltesttype != IS_NOT_NULL || nt->argisrow ||
			!equal(arg, var))
			return false;
		*qual = clause;
		return true;
	}
	if (IsA(clause, OpExpr) && list_length(((OpExpr *) clause)->args) == 2)
	{
		OpExpr	   *op = (OpExpr *) clause;
		Node	   *left = (Node *) linitial(op->args);
		Node	   *right = (Node *) lsecond(op->args);
		Node	   *l = left;
		Node	   *r = right;
		Oid			opno = op->opno;
		int			strategy;

		while (IsA(l, RelabelType))
			l = (Node *) ((RelabelType *) l)->arg;
		while (IsA(r, RelabelType))
			r = (Node *) ((RelabelType *) r)->arg;
		if (!equal(l, var))
		{
			if (!equal(r, var))
				return false;
			opno = get_commutator(opno);
			if (!OidIsValid(opno))
				return false;
			right = left;
			left = (Node *) lsecond(op->args);
		}
		if (contain_var_clause(right) || contain_volatile_functions(right) ||
			contain_subplans(right))
			return false;
		if (OidIsValid(idxcoll) && op->inputcollid != idxcoll)
			return false;
		strategy = get_op_opfamily_strategy(opno, opfamily);
		if (!LION_STRAT_IS_WALK(strategy))
			return false;		/* a range, or `<>`'s hole (DESIGN.md §35) */
		if (opno == op->opno)
			*qual = clause;
		else
		{
			OpExpr	   *c = (OpExpr *) copyObject(op);

			c->opno = opno;
			c->opfuncid = InvalidOid;
			set_opfuncid(c);
			c->args = list_make2(left, right);
			*qual = (Expr *) c;
		}
		return true;
	}
	return false;
}

/*
 * The restriction clauses a lion column's walk can decide once per ENTRY
 * rather than once per row (DESIGN.md §30.11, "A filter on the walked
 * column"): those that read no column but the walked one and call nothing
 * volatile, on an index whose stored keys are the rows' own values byte for
 * byte (lion_index_can_emit_value()).  Every row of an entry then holds one
 * value, so a clause is true of all of them or of none, and the first
 * visible row the walk fetches of an entry decides for the entry: one that
 * fails takes the rest of the entry with it, unread.  The clause is only
 * ever evaluated on a visible row, as the ordinary plan evaluates it - never
 * on a stored key alone, which may be a dead row's, and could make a function
 * fail that the query never calls on it (`1 / k` with k = 0 only in a
 * deleted row).  It stays in the node's filter as well.
 */
static List *
lo_walk_value_rinfos(RelOptInfo *rel, IndexOptInfo *idx, int c, Var *var,
					 List *walkrinfos)
{
	List	   *result = NIL;
	ListCell   *lc;

	if (!lion_index_can_emit_value(idx, (AttrNumber) (c + 1)))
		return NIL;
	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
		Bitmapset  *attrs = NULL;

		if (rinfo->pseudoconstant || list_member_ptr(walkrinfos, rinfo) ||
			!bms_equal(rinfo->clause_relids, rel->relids))
			continue;
		if (contain_volatile_functions((Node *) rinfo->clause) ||
			contain_subplans((Node *) rinfo->clause))
			continue;
		pull_varattnos((Node *) rinfo->clause, rel->relid, &attrs);
		if (bms_num_members(attrs) != 1 ||
			!bms_is_member(var->varattno - FirstLowInvalidHeapAttributeNumber,
						   attrs))
			continue;
		result = lappend(result, rinfo);
	}
	return result;
}

/*
 * The walks of rel's lion columns that give the query's ORDER BY (DESIGN.md
 * §30.11): an ordered scalar key column of a lion index, in its type's own
 * btree order under the column's collation, whose pathkey - the ascending
 * one, as build_expression_pathkey() finds it among the query's equivalence
 * classes (for a partition, through its member of the parent's class) - is
 * the first of the ORDER BY's.  The walk then gives that pathkey whichever
 * direction and NULLs placement it asks for: it runs either way, and takes
 * the column's NULL entry before its values or after them.  Only the first
 * pathkey: a lion column orders one column, and core sorts the rest
 * incrementally.  Only the ORDER BY's: a GROUP BY, a DISTINCT or a merge
 * join over a lion column wants every row, which the count pushdown counts
 * from the same entries and a hash or a Sort takes from the heap in its
 * order, where the walk would fetch them in key order.
 */
static List *
lo_lion_walks(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte,
			  List *lionidx)
{
	PathKey    *want;
	List	   *walks = NIL;
	ListCell   *lc;

	if (root->sort_pathkeys == NIL)
		return NIL;
	want = (PathKey *) linitial(root->sort_pathkeys);

	foreach(lc, lionidx)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);
		Relation	indexrel = NULL;
		int			c;

		/*
		 * A partial index holds the rows its predicate selects, which is all
		 * of them only when the query implies it - as core asks of an ordered
		 * index path.
		 */
		if (idx->indpred != NIL && !idx->predOK)
			continue;

		for (c = 0; c < idx->nkeycolumns; c++)
		{
			AttrNumber	attno = idx->indexkeys[c];
			LionState  *state;
			TypeCacheEntry *typentry;
			Oid			type;
			int32		typmod;
			Oid			coll;
			Var		   *var;
			List	   *pks;
			PathKey    *mine;
			LoWalk	   *w;
			ListCell   *lc2;
			bool		strict = false;

			if (attno <= 0)
				continue;		/* an expression: no Var to order by */
			get_atttypetypmodcoll(rte->relid, attno, &type, &typmod, &coll);
			if (idx->indexcollations[c] != coll)
				continue;

			if (indexrel == NULL)
				indexrel = index_open(idx->indexoid, AccessShareLock);
			state = lion_index_column_state(indexrel, (AttrNumber) (c + 1));
			if (state->multikey || !state->ordered || state->typid != type)
				continue;

			/*
			 * The directory is in the type's default btree order only when
			 * the column's comparison is that order's (the GROUP BY walk's
			 * rule, lion_index_orders_naturally()); its `<` names the
			 * operator family the pathkey is in.
			 */
			typentry = lookup_type_cache(type, TYPECACHE_CMP_PROC |
										 TYPECACHE_LT_OPR);
			if (!OidIsValid(typentry->cmp_proc) ||
				typentry->cmp_proc != state->cmpproc.fn_oid ||
				!OidIsValid(typentry->lt_opr))
				continue;

			var = makeVar(rel->relid, attno, type, typmod, coll, 0);
			pks = build_expression_pathkey(root, (Expr *) var,
										   typentry->lt_opr, rel->relids,
										   false);
			if (pks == NIL)
				continue;
			mine = (PathKey *) linitial(pks);
			if (mine->pk_eclass != want->pk_eclass ||
				mine->pk_opfamily != want->pk_opfamily)
				continue;

			w = (LoWalk *) palloc0(sizeof(LoWalk));
			w->index = idx;
			w->indexcol = c;
			w->var = var;
			w->pathkey = want;
#if PG_VERSION_NUM >= 180000
			w->backward = (want->pk_cmptype == COMPARE_GT);
#else
			w->backward = (want->pk_strategy == BTGreaterStrategyNumber);
#endif
			foreach(lc2, rel->baserestrictinfo)
			{
				RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc2);
				Expr	   *qual;
				bool		list;

				if (lo_walk_clause(rinfo, var, idx->opfamily[c],
								   idx->opcintype[c],
								   idx->indexcollations[c], &qual, &list))
				{
					/* one list a walk: any other stays the set's, or a filter */
					if (list && w->nlist > 0)
						continue;
					if (list)
					{
						Node	   *arr = (Node *) lsecond(((ScalarArrayOpExpr *) qual)->args);

#if PG_VERSION_NUM >= 170000
						w->nlist = Max(1.0, estimate_array_length(root, arr));
#else
						w->nlist = Max(1.0, (double) estimate_array_length(arr));
#endif
					}
					w->rinfos = lappend(w->rinfos, rinfo);
					w->quals = lappend(w->quals, qual);
					strict = true;	/* no NULL satisfies any kind */
				}
			}
			w->nulls = strict ? LION_ORDER_NULLS_NONE :
				want->pk_nulls_first ? LION_ORDER_NULLS_FIRST :
				LION_ORDER_NULLS_LAST;
			w->vrinfos = lo_walk_value_rinfos(rel, idx, c, var, w->rinfos);
			walks = lappend(walks, w);
		}
		if (indexrel != NULL)
			index_close(indexrel, AccessShareLock);
	}
	return walks;
}

/*
 * The price of a lion column's walk with one lion access - or none: every row
 * the walk meets is fetched and filtered - (DESIGN.md §30.11, "Cost"), and
 * the size its set is expected to have.  It is §30.3's with the walk in place
 * of the btree's: the entries and TIDs of the range, read a directory leaf
 * at a time from private copies, instead of the btree's index cost.
 */
static void
lo_cost_walk(PlannerInfo *root, RelOptInfo *rel, LoWalk *w, Path *lion,
			 List *lionqual, List *residual, double rows, Cost *startup_p,
			 Cost *total_p, double *setbytes)
{
	double		tuples = Max(rel->tuples, 1.0);
	double		pages = Max((double) rel->pages, 1.0);
	Selectivity swalk = 1.0;
	Selectivity sel = 1.0;
	double		walked;
	double		entries;
	double		ndistinct;
	double		perentry;
	double		entrybytes;
	double		leaves;
	double		fetched;
	double		spc_random;
	double		spc_seq;
	double		max_io;
	double		min_io;
	double		pages_corr;
	double		corr;
	QualCost	qcost;
	Cost		startup;
	Cost		run;
	VariableStatData vardata;
	bool		isdefault;
	int16		keywidth;

	/*
	 * The rows of the range - of a partial index, only the ones its predicate
	 * admits, the predicate's clauses the range does not already imply added
	 * as core's add_predicate_to_index_quals() adds them.
	 */
	{
		List	   *walkclauses = list_copy(w->rinfos);
		ListCell   *lc;

		foreach(lc, w->index->indpred)
		{
			Expr	   *pred = (Expr *) lfirst(lc);

			if (!predicate_implied_by(list_make1(pred), w->quals, false))
				walkclauses = lappend(walkclauses, pred);
		}
		if (walkclauses != NIL)
			swalk = clauselist_selectivity(root, walkclauses, rel->relid,
										   JOIN_INNER, NULL);
	}
	walked = clamp_row_est(tuples * swalk);
	examine_variable(root, (Node *) w->var, rel->relid, &vardata);
	ndistinct = Max(1.0, get_variable_numdistinct(&vardata, &isdefault));
	ReleaseVariableStats(vardata);
	entries = Max(1.0, Min(walked, ndistinct * swalk));
	perentry = walked / entries;

	/* the descent to where the walk starts */
	startup = 2.0 * random_page_cost;
	*setbytes = 0.0;
	if (lion != NULL)
	{
		Cost		lioncost;
		double		members;
		double		ncont;

		List	   *shared;

		cost_bitmap_tree_node(lion, &lioncost, &sel);
		members = clamp_row_est(sel * tuples);
		ncont = Min(ceil(pages / LION_BLOCKS_PER_CONTAINER), members);
		*setbytes = ncont * (LION_CONTAINER_HDRSZ + LO_ENTRY_BYTES) +
			Min(members * sizeof(uint16), ncont * LION_BITSET_BYTES);
		startup += lioncost + ncont * cpu_operator_cost;

		/*
		 * What the set selects among the rows walked: not what the walked
		 * index's predicate already guarantees (lo_cost()).
		 */
		shared = lo_pred_implied(w->index, lionqual);
		if (shared != NIL)
		{
			Selectivity both = clauselist_selectivity(root, shared, rel->relid,
													  JOIN_INNER, NULL);

			if (both > 0)
				sel = Min(sel / both, 1.0);
		}
	}

	/*
	 * The walk: each entry a small entry of a walk (a copy, a test of its key
	 * against the range, its items read), each TID one more test, and the
	 * directory leaves under them - an entry's key and its INLINE set, two
	 * bytes a row, or, past a page, a posting tree's pages.
	 */
	keywidth = get_typlen(w->var->vartype);
	entrybytes = 24.0 + (keywidth > 0 ? (double) keywidth : 16.0) +
		Min(perentry * 2.0 + 8.0, BLCKSZ / 4.0);
	leaves = ceil(entries * entrybytes / (BLCKSZ * 0.7)) +
		(perentry * 2.0 > BLCKSZ / 4.0 ? ceil(walked * 2.0 / BLCKSZ) : 0.0);
	run = leaves * seq_page_cost +
		entries * lion_range_union_entry_cost * cpu_tuple_cost +
		walked * cpu_operator_cost;

	/*
	 * The rows fetched: every row the walk meets, or only the set's members,
	 * each priced as cost_index() prices a fetch, the column's correlation
	 * with the heap interpolating between rows at random and rows in order.
	 *
	 * A filter on the walked column (lo_walk_value_rinfos()) lets an entry
	 * go after one fetched member: of the entries it rejects, the walk reads
	 * the TIDs up to that member and fetches it, and of the rest everything,
	 * as before.  The share of its clauses the set already selects by is not
	 * counted again.
	 */
	fetched = clamp_row_est(walked * sel);
	if (w->vrinfos != NIL)
	{
		List	   *vclauses = NIL;
		ListCell   *lc;

		foreach(lc, w->vrinfos)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

			if (lionqual == NIL ||
				!predicate_implied_by(list_make1(rinfo->clause), lionqual,
									  false))
				vclauses = lappend(vclauses, rinfo);
		}
		if (vclauses != NIL)
		{
			Selectivity vsel = clauselist_selectivity(root, vclauses,
													  rel->relid, JOIN_INNER,
													  NULL);
			double		rejected = entries * (1.0 - vsel);
			double		tidsto = Min(perentry, 1.0 / Max(sel, 1e-9));

			run -= walked * cpu_operator_cost;
			walked = clamp_row_est(walked * vsel + rejected * tidsto);
			run += walked * cpu_operator_cost;
			fetched = clamp_row_est(fetched * vsel +
									rejected * Min(1.0, perentry * sel));
		}
	}
	get_tablespace_page_costs(rel->reltablespace, &spc_random, &spc_seq);
	corr = lion_var_heap_correlation(root, rel->relid, w->var);
	max_io = index_pages_fetched(fetched, rel->pages,
								 (double) w->index->pages, root) * spc_random;
	pages_corr = ceil(Min(fetched, walked / tuples * pages));
	min_io = (pages_corr > 0) ? spc_random + (pages_corr - 1) * spc_seq : 0;
	run += max_io + corr * corr * (min_io - max_io);

	cost_qual_eval(&qcost, residual, root);
	startup += qcost.startup + rel->reltarget->cost.startup;
	run += fetched * (cpu_tuple_cost + qcost.per_tuple) +
		rows * rel->reltarget->cost.per_tuple;

	/* a list: one more descent for each of its values after the first */
	if (w->nlist > 1.0)
		run += (w->nlist - 1.0) * 2.0 * random_page_cost;

	*startup_p = startup;
	*total_p = startup + run;
}

/*
 * The margin the paths of the relation lion_ordered_set_rel_pathlist() is
 * adding them to are offered at (lion_units_margin_for()), found before the
 * first is added and can free a scan of core's.
 */
static double lo_margin = 1.0;

/*
 * One LionOrdered path, offered to add_path() (DESIGN.md §30.2, step 3) at
 * pg_lion.pushdown_margin (§39): its price divided by the margin, startup
 * and total alike, so that whatever it is compared with - the btree scan it
 * walks, a Sort over another path, a LIMIT's fraction of either - it is
 * taken only by that margin.  Its competitors are the relation's scans, which
 * are lion's reference units already: it has no rate.
 */
static void
lo_add_path(RelOptInfo *rel, const CustomPathMethods *methods, List *pathkeys,
			List *priv, double rows, Cost startup, Cost total)
{
	CustomPath *cp = makeNode(CustomPath);

	cp->path.pathtype = T_CustomScan;
	cp->path.parent = rel;
	cp->path.pathtarget = rel->reltarget;
	cp->path.param_info = NULL;
	cp->path.parallel_aware = false;
	cp->path.parallel_safe = false;
	cp->path.parallel_workers = 0;
	cp->path.rows = rows;
	cp->path.startup_cost = startup / lo_margin;
	cp->path.total_cost = total / lo_margin;
	cp->path.pathkeys = pathkeys;
	cp->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cp->custom_paths = NIL;
	cp->custom_private = priv;
	cp->methods = methods;
	add_path(rel, &cp->path);
}

static void
lion_ordered_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
							  RangeTblEntry *rte)
{
	Oid			lionam;
	List	   *ordpaths = NIL;
	List	   *btpaths = NIL;
	List	   *lionidx = NIL;
	List	   *cands = NIL;
	List	   *walks = NIL;
	Bitmapset  *targetattrs = NULL;
	ListCell   *lc;
	ListCell   *lc2;
	Size		limit;
	double		rows;

	if (lion_prev_set_rel_pathlist_hook != NULL)
		lion_prev_set_rel_pathlist_hook(root, rel, rti, rte);

	if (!lion_enable_ordered_scan && !lion_enable_btree_scan)
		return;
	/* no lion index is read under 16's old_snapshot_threshold (§9) */
	if (lion_old_snapshot_threshold_active())
		return;
	if (!lo_rel_ok(root, rel, rte))
		return;

	lionam = lion_get_am_oid();
	if (!OidIsValid(lionam))
		return;

	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);

		if (idx->relam == lionam && !idx->hypothetical)
			lionidx = lappend(lionidx, idx);
	}
	if (lionidx == NIL)
		return;
	lo_margin = lion_units_margin_for(rel);
	if (enable_indexonlyscan)
		pull_varattnos((Node *) rel->reltarget->exprs, rel->relid,
					   &targetattrs);

	/* the ordered side: core's ordered btree paths ... */
	if (lion_enable_ordered_scan && rel->baserestrictinfo != NIL)
	{
		foreach(lc, rel->pathlist)
		{
			Path	   *p = (Path *) lfirst(lc);
			IndexPath  *ipath;

			if (!IsA(p, IndexPath) || p->pathkeys == NIL ||
				p->param_info != NULL)
				continue;
			ipath = (IndexPath *) p;

			/*
			 * A btree: the early stop and the fetch-and-sort switch rest on
			 * its returning each heap TID once per scan, in the order its
			 * pathkeys claim (DESIGN.md §30.4).
			 */
			if (ipath->indexorderbys != NIL ||
				ipath->indexinfo->relam != BTREE_AM_OID ||
				ipath->indexinfo->hypothetical)
				continue;
			if (!lo_indexpath_ok(ipath))
				continue;
			ordpaths = lappend(ordpaths, ipath);
		}
	}

	/* ... and the walks of lion's own ordered columns (§30.11) */
	if (lion_enable_ordered_scan)
		walks = lo_lion_walks(root, rel, rte, lionidx);

	/*
	 * ... and the paths of the relation's btrees that return at least the
	 * target's columns (what every pair below needs, and the rest is the
	 * pair's), whether or not core found their order useful (DESIGN.md
	 * §40.2): with the row's values coming from the index tuple, their walk
	 * pays without an ORDER BY.
	 */
	if (lion_enable_btree_scan && enable_indexonlyscan &&
		rel->baserestrictinfo != NIL)
	{
		foreach(lc, rel->indexlist)
		{
			IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);
			List	   *iocols;

			if (idx->relam != BTREE_AM_OID || idx->hypothetical ||
				!lo_index_only(idx, targetattrs, &iocols))
				continue;
			btpaths = list_concat(btpaths, lo_btree_paths(root, rel, idx));
		}
	}
	if (ordpaths == NIL && walks == NIL && btpaths == NIL)
		return;

	/* the heap table AM only (§2); ambuild refused every other */
	{
		Relation	relation = table_open(rte->relid, NoLock);
		bool		supported = lion_table_am_supported(relation);

		table_close(relation, NoLock);
		if (!supported)
			return;
	}

	limit = get_hash_memory_limit();

	/*
	 * The rows the node returns, as lion's estimates see them: core's, times
	 * what the intersection probe measured of the set clauses among the
	 * relation's restriction clauses (lion_probe_rel_rows(), DESIGN.md
	 * §29.11, "Correlated sets"), as the lion side's selectivity already is
	 * (lioncostestimate()).  Core's LIMIT planning takes the share of the
	 * walk a LIMIT needs from them (§30.3): priced for the few rows core's
	 * product of the clauses' selectivities says, the node was charged its
	 * whole walk for a page of a correlated filter's thousands of rows, and
	 * lost to the lion scan and Sort that fetch every one of them.
	 */
	rows = lion_probe_rel_rows(root, rel);

	/*
	 * The lion side of a btree walk, as core would build it for every
	 * restriction clause.
	 */
	if (ordpaths != NIL || btpaths != NIL)
		cands = lo_lion_accesses(root, rel, lionidx, NIL, lionam);

	foreach(lc, ordpaths)
	{
		IndexPath  *ord = (IndexPath *) lfirst(lc);
		List	   *ordrinfos = lo_ord_rinfos(ord);

		foreach(lc2, cands)
		{
			Path	   *lion = (Path *) lfirst(lc2);
			List	   *lionrinfos = NIL;
			bool		lossy = false;
			List	   *lionqual = lo_lion_qual(lion, &lionrinfos, &lossy);
			List	   *residual = lo_residual(rel->baserestrictinfo, ordrinfos,
											   lionrinfos, lionqual);
			List	   *iocols = NIL;
			bool		indexonly;
			Cost		startup;
			Cost		total;
			double		setbytes;

			/* the values from the index tuple when the btree has them (§40) */
			indexonly = lo_index_only(ord->indexinfo,
									  lo_needed_attrs(rel, residual,
													  ord->indexclauses,
													  lionqual, lossy),
									  &iocols);
			lo_cost(root, rel, ord, lion, lionrinfos, lionqual, residual, rows,
					indexonly, &startup, &total, &setbytes);
			if (setbytes > (double) limit)
				continue;		/* the set would not fit (§30.3) */
			lo_add_path(rel, &lo_path_methods, ord->path.pathkeys,
						list_make3(ord, lion, iocols), rows, startup, total);
		}
	}

	/*
	 * A covering btree's paths (DESIGN.md §40.2), each paired with each lion
	 * access as above - but not with one whose every clause the btree's own
	 * quals answer already, which would add nothing to the walk - priced in
	 * index-only mode, and offered at whatever order the path has: with
	 * pathkeys it is a LionOrdered, without them a LionBtreeScan.  The twin
	 * of an ordered path above - the same index, direction and order - was
	 * offered there.  With the ordered scans off no order is claimed at all.
	 */
	foreach(lc, btpaths)
	{
		IndexPath  *ipath = (IndexPath *) lfirst(lc);
		List	   *ordrinfos = lo_ord_rinfos(ipath);
		List	   *pathkeys = lion_enable_ordered_scan ?
			ipath->path.pathkeys : NIL;
		bool		twin = false;

		foreach(lc2, ordpaths)
		{
			IndexPath  *ord = (IndexPath *) lfirst(lc2);

			if (ord->indexinfo == ipath->indexinfo &&
				ord->indexscandir == ipath->indexscandir &&
				compare_pathkeys(ord->path.pathkeys,
								 ipath->path.pathkeys) == PATHKEYS_EQUAL)
				twin = true;
		}
		if (twin)
			continue;
		foreach(lc2, cands)
		{
			Path	   *lion = (Path *) lfirst(lc2);
			List	   *lionrinfos = NIL;
			bool		lossy = false;
			List	   *lionqual;
			List	   *residual;
			List	   *iocols;
			Cost		startup;
			Cost		total;
			double		setbytes;

			if (!lo_lion_adds(lion, ipath->indexclauses))
				continue;
			lionqual = lo_lion_qual(lion, &lionrinfos, &lossy);
			residual = lo_residual(rel->baserestrictinfo, ordrinfos,
								   lionrinfos, lionqual);
			if (!lo_index_only(ipath->indexinfo,
							   lo_needed_attrs(rel, residual,
											   ipath->indexclauses,
											   lionqual, lossy),
							   &iocols))
				continue;
			lo_cost(root, rel, ipath, lion, lionrinfos, lionqual, residual,
					rows, true, &startup, &total, &setbytes);
			if (setbytes > (double) limit)
				continue;		/* the set would not fit (§30.3) */
			lo_add_path(rel, pathkeys != NIL ?
						&lo_path_methods : &lo_btree_path_methods,
						pathkeys, list_make3(ipath, lion, iocols),
						rows, startup, total);
		}
	}

	/*
	 * A lion column's walk takes the lion accesses core builds for every
	 * clause but the range it is bounded by - collecting that range into the
	 * set would read all of it, which the walk exists not to do - and no
	 * access at all: then every row the walk meets is fetched and the
	 * clauses are its filter.  The price decides (§30.11, "Cost").
	 */
	foreach(lc, walks)
	{
		LoWalk	   *w = (LoWalk *) lfirst(lc);
		List	   *wcands = lo_lion_accesses(root, rel, lionidx, w->rinfos,
											  lionam);
		List	   *walkinfo = list_make4_int(LO_WALK_MAGIC, w->indexcol,
											  w->backward ? 1 : 0, w->nulls);

		wcands = lappend(wcands, NULL);
		foreach(lc2, wcands)
		{
			Path	   *lion = (Path *) lfirst(lc2);
			List	   *lionrinfos = NIL;
			bool		lossy = false;
			List	   *lionqual = (lion != NULL) ?
				lo_lion_qual(lion, &lionrinfos, &lossy) : NIL;
			List	   *residual = lo_residual(rel->baserestrictinfo, w->rinfos,
											   lionrinfos, lionqual);
			Cost		startup;
			Cost		total;
			double		setbytes;
			List	   *priv;

			lo_cost_walk(root, rel, w, lion, lionqual, residual, rows,
						 &startup, &total, &setbytes);
			if (setbytes > (double) limit)
				continue;		/* the set would not fit (§30.3) */
			priv = list_make5(walkinfo, w->index, w->rinfos, w->quals,
							  list_make1(w->var));
			priv = lappend(priv, lion);
			priv = lappend(priv, w->vrinfos);
			lo_add_path(rel, &lo_path_methods, list_make1(w->pathkey), priv,
						rows, startup, total);
		}
	}
}

/*
 * The path's pathkeys as sort keys over plain columns of rel, for the
 * fetch-and-sort switch; NIL when one of them is not a plain column (an
 * expression index's order: its functions would be evaluated where the
 * ordinary plan calls none, which EXECUTE could tell apart, §30.6).
 */
static List *
lo_sort_keys(RelOptInfo *rel, List *pathkeys)
{
	List	   *attnos = NIL;
	List	   *ops = NIL;
	List	   *colls = NIL;
	List	   *nulls = NIL;
	ListCell   *lc;

	foreach(lc, pathkeys)
	{
		PathKey    *pk = (PathKey *) lfirst(lc);
		EquivalenceClass *ec = pk->pk_eclass;
		Var		   *var = NULL;
		Oid			type = InvalidOid;
		Oid			op;
		bool		desc;
		EquivalenceMember *em;
#if PG_VERSION_NUM >= 180000
		EquivalenceMemberIterator it;
#else
		ListCell   *lc2;
#endif

		/*
		 * rel's own member of the class: a partition's is a child member
		 * (§30.11), which 18 keeps apart from the parent's.
		 */
#if PG_VERSION_NUM >= 180000
		setup_eclass_member_iterator(&it, ec, rel->relids);
		while ((em = eclass_member_iterator_next(&it)) != NULL)
#else
		foreach(lc2, ec->ec_members)
#endif
		{
			Node	   *e;

#if PG_VERSION_NUM < 180000
			em = (EquivalenceMember *) lfirst(lc2);
#endif
			e = (Node *) em->em_expr;
			if (em->em_is_const || !bms_equal(em->em_relids, rel->relids))
				continue;
			while (IsA(e, RelabelType))
				e = (Node *) ((RelabelType *) e)->arg;
			if (IsA(e, Var) && ((Var *) e)->varno == (int) rel->relid &&
				((Var *) e)->varattno > 0 && ((Var *) e)->varlevelsup == 0)
			{
				var = (Var *) e;
				type = em->em_datatype;
				break;
			}
		}
		if (var == NULL)
			return NIL;
#if PG_VERSION_NUM >= 180000
		desc = (pk->pk_cmptype == COMPARE_GT);
#else
		desc = (pk->pk_strategy == BTGreaterStrategyNumber);
#endif
		op = get_opfamily_member(pk->pk_opfamily, type, type,
								 desc ? BTGreaterStrategyNumber : BTLessStrategyNumber);
		if (!OidIsValid(op))
			return NIL;
		attnos = lappend_int(attnos, var->varattno);
		ops = lappend_oid(ops, op);
		colls = lappend_oid(colls, ec->ec_collation);
		nulls = lappend_int(nulls, pk->pk_nulls_first ? 1 : 0);
	}
	return list_make4(attnos, ops, colls, nulls);
}

static Plan *
lo_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
			 List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	Path	   *lion;
	List	   *lionrinfos = NIL;
	bool		lossy = false;
	List	   *lionqual = NIL;
	List	   *residual;
	List	   *tree = NIL;
	List	   *leaves = NIL;
	List	   *lionquals = NIL;
	List	   *ordrinfos = NIL;
	List	   *ordquals = NIL;
	List	   *ordcols = NIL;
	List	   *ordorig = NIL;
	List	   *vrinfos = NIL;
	List	   *iocols = NIL;
	double		expected = 0.0;
	int			dir;
	int			flags = 0;
	int			nulls = LION_ORDER_NULLS_NONE;
	int			walkattno = 0;
	Oid			ordoid;
	ListCell   *lc;

	if (IsA(linitial(best_path->custom_private), IndexPath))
	{
		/*
		 * A btree's walk (DESIGN.md §30.2), its values from the index tuple
		 * when the planner found it covering (§40).
		 */
		IndexPath  *ord = (IndexPath *) linitial(best_path->custom_private);

		lion = (Path *) lsecond(best_path->custom_private);
		iocols = (List *) lthird(best_path->custom_private);
		if (iocols != NIL)
			flags |= LO_FLAG_INDEXONLY;
		ordrinfos = lo_ord_rinfos(ord);
		foreach(lc, ord->indexclauses)
		{
			IndexClause *iclause = lfirst_node(IndexClause, lc);
			ListCell   *lc2;

			ordorig = lappend(ordorig, iclause->rinfo->clause);
			foreach(lc2, iclause->indexquals)
			{
				RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc2);

				ordquals = lappend(ordquals, rinfo->clause);
				ordcols = lappend_int(ordcols, iclause->indexcol);
			}
		}
		ordoid = ord->indexinfo->indexoid;
		dir = (int) ord->indexscandir;
		expected = ord->indexselectivity * rel->tuples;
	}
	else
	{
		/*
		 * A lion column's walk (DESIGN.md §30.11): its index, the column
		 * (1-based, as a scan key names it), its direction and where its
		 * NULL entry goes; its quals are the range it is bounded by.
		 */
		List	   *walkinfo = (List *) linitial(best_path->custom_private);
		IndexOptInfo *idx = (IndexOptInfo *) lsecond(best_path->custom_private);
		int			indexcol = lsecond_int(walkinfo);

		Assert(linitial_int(walkinfo) == LO_WALK_MAGIC);
		ordrinfos = (List *) lthird(best_path->custom_private);
		ordquals = (List *) lfourth(best_path->custom_private);
		lion = (Path *) list_nth(best_path->custom_private, 5);
		vrinfos = (List *) list_nth(best_path->custom_private, 6);
		foreach(lc, ordrinfos)
		{
			ordorig = lappend(ordorig, lfirst_node(RestrictInfo, lc)->clause);
			ordcols = lappend_int(ordcols, indexcol);
		}
		ordoid = idx->indexoid;
		dir = lthird_int(walkinfo) ? (int) BackwardScanDirection :
			(int) ForwardScanDirection;
		flags |= LO_FLAG_LIONWALK;
		nulls = lfourth_int(walkinfo);
		walkattno = indexcol + 1;
	}

	if (lion != NULL)
	{
		lionqual = lo_lion_qual(lion, &lionrinfos, &lossy);
		lo_lion_tree(lion, &tree, &leaves, &lionquals);
		if (lossy)
			flags |= LO_FLAG_LOSSY;
	}
	residual = lo_residual(clauses, ordrinfos, lionrinfos, lionqual);

	/*
	 * Index-only mode: does the lion recheck of an inexact or degraded set
	 * need the heap tuple, the btree lacking a column of the lion qual (§40.4)?
	 */
	if (iocols != NIL)
	{
		Bitmapset  *lionattrs = NULL;
		Bitmapset  *returned = NULL;

		pull_varattnos((Node *) lionqual, rel->relid, &lionattrs);
		foreach(lc, iocols)
		{
			int			attno = lfirst_int(lc);

			if (attno > 0)
				returned = bms_add_member(returned,
										  attno - FirstLowInvalidHeapAttributeNumber);
		}
		if (!bms_is_subset(lionattrs, returned))
			flags |= LO_FLAG_HEAPRECHECK;
	}

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = extract_actual_clauses(residual, false);
	cscan->scan.scanrelid = rel->relid;
	cscan->flags = best_path->flags;
	cscan->custom_plans = NIL;
	cscan->custom_scan_tlist = NIL;
	cscan->custom_exprs = list_make5(lionquals, ordquals, lionqual, ordorig,
									 extract_actual_clauses(vrinfos, false));
	cscan->custom_private =
		list_make5(list_make2_int(LO_PRIV_MAGIC, LO_PRIV_NMEMBERS),
				   list_make1_oid(ordoid),
				   list_make4_int(dir, flags, nulls, walkattno),
				   ordcols,
				   tree);
	cscan->custom_private = lappend(cscan->custom_private, leaves);
	cscan->custom_private = lappend(cscan->custom_private,
									lo_sort_keys(rel, best_path->path.pathkeys));
	cscan->custom_private = lappend(cscan->custom_private, iocols);
	cscan->custom_private = lappend(cscan->custom_private,
									makeConst(FLOAT8OID, -1, InvalidOid,
											  sizeof(float8),
											  Float8GetDatum(expected),
											  false, FLOAT8PASSBYVAL));
	cscan->methods = (best_path->methods == &lo_btree_path_methods) ?
		&lo_btree_scan_methods : &lo_scan_methods;

	return &cscan->scan.plan;
}

/* ---------------------------------------------------------------------
 * The TID set
 * --------------------------------------------------------------------- */

static LionTidSet *
lo_set_new(LionOrderedState *st)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(st->setcxt);
	LionTidSet *set = (LionTidSet *) palloc0(sizeof(LionTidSet));

	set->cap = 16;
	set->keys = (uint32 *) palloc(sizeof(uint32) * set->cap);
	set->conts = (LionContainer **) palloc(sizeof(LionContainer *) * set->cap);
	set->sorted = true;
	st->live = lappend(st->live, set);
	MemoryContextSwitchTo(oldcxt);
	return set;
}

static void
lo_set_free(LionOrderedState *st, LionTidSet *set)
{
	int			i;

	for (i = 0; i < set->n; i++)
	{
		if (set->conts[i] != NULL)
		{
			st->bytes -= lion_container_size(set->conts[i]);
			pfree(set->conts[i]);
		}
	}
	st->bytes -= set->n * LO_ENTRY_BYTES;
	pfree(set->keys);
	pfree(set->conts);
	st->live = list_delete_ptr(st->live, set);
	pfree(set);
}

/*
 * The set outgrew hash_mem: keep the container KEYS of every live set and
 * drop their containers.  From here on every key means "all of its heap
 * blocks", and every fetched member is rechecked (DESIGN.md §30.4).
 */
static void
lo_degrade(LionOrderedState *st)
{
	ListCell   *lc;

	foreach(lc, st->live)
	{
		LionTidSet *set = (LionTidSet *) lfirst(lc);
		int			i;

		for (i = 0; i < set->n; i++)
		{
			if (set->conts[i] != NULL)
			{
				st->bytes -= lion_container_size(set->conts[i]);
				pfree(set->conts[i]);
				set->conts[i] = NULL;
			}
		}
	}
	st->degraded = true;
	st->exact = false;
}

static void lo_set_finish(LionOrderedState *st, LionTidSet *set);

/*
 * Append (ckey, a copy of c, or nothing when degraded).
 *
 * A WALK or a LIST source hands out a container key once per ENTRY or per
 * batch, not once (§29.3): a range over 400,000 distinct values brings
 * 400,000 one-TID pieces for 34 container keys.  The first version merged the
 * pieces only when the source was done, so every piece held a directory entry
 * and a container copy until then - the set degraded although its merged
 * form was a hundredth of hash_mem, and once degraded its directory went on
 * growing one entry per piece, past any bound (52 MB at a 1 MB hash_mem for a
 * range over 4M values; 2026-09-25 second review).  The pieces are merged
 * whenever the directory is full - it doubles only if it is still more than
 * half full after that - and before their bytes would degrade the set, once
 * there are at least as many pieces as merged entries (each merge then pays
 * for itself); only a set whose merged form does not fit degrades (§30.4).
 */
static void
lo_set_push(LionOrderedState *st, LionTidSet *set, uint32 ckey,
			const LionContainer *c)
{
	Size		size = (c != NULL && !st->degraded) ? lion_container_size(c) : 0;

	if (set->n == set->cap)
	{
		if (!set->sorted && set->cap >= LO_MERGE_MIN)
			lo_set_finish(st, set);
		if (set->n > set->cap / 2)
		{
			set->cap *= 2;
			set->keys = (uint32 *) repalloc(set->keys, sizeof(uint32) * set->cap);
			set->conts = (LionContainer **)
				repalloc(set->conts, sizeof(LionContainer *) * set->cap);
		}
	}
	if (size > 0 && !set->sorted &&
		st->bytes + LO_ENTRY_BYTES + size > st->limit &&
		set->n - set->nmerged >= set->nmerged)
		lo_set_finish(st, set);

	if (set->n > 0 && ckey <= set->keys[set->n - 1])
		set->sorted = false;
	st->bytes += LO_ENTRY_BYTES;
	set->keys[set->n] = ckey;
	set->conts[set->n] = NULL;
	if (c != NULL && !st->degraded)
	{
		if (st->bytes + size > st->limit)
			lo_degrade(st);
		else
		{
			LionContainer *copy = (LionContainer *)
				MemoryContextAlloc(st->setcxt, size);

			memcpy(copy, c, size);
			st->bytes += size;
			set->conts[set->n] = copy;
		}
	}
	set->n++;
}

static int
lo_cmp_slot(const void *a, const void *b, void *arg)
{
	const uint32 *keys = (const uint32 *) arg;
	uint32		ka = keys[*(const int *) a];
	uint32		kb = keys[*(const int *) b];

	if (ka != kb)
		return (ka < kb) ? -1 : 1;
	return (*(const int *) a < *(const int *) b) ? -1 : 1;
}

/*
 * A WALK or LIST source's containers came entry by entry (§29.3): sort them
 * by key and OR the containers of equal keys into one - when the source is
 * done, and whenever lo_set_push() finds the pieces piling up.  The pieces of
 * one key are ORed into a bitset image and turned into a container once:
 * ORing them pairwise cost a pass over the growing container per piece, 4.2 s
 * for a range over 250,000 distinct values against 0.2 s for the lion bitmap
 * scan of the same range (2026-09-25 second review).
 */
static void
lo_set_finish(LionOrderedState *st, LionTidSet *set)
{
	int		   *order;
	uint32	   *keys;
	LionContainer **conts;
	uint64	   *img;
	LionContainer *out;
	int			n = 0;
	int			i;

	if (set->sorted || set->n < 2)
	{
		set->sorted = true;
		set->nmerged = set->n;
		return;
	}

	order = (int *) MemoryContextAlloc(st->buildcxt, sizeof(int) * set->n);
	for (i = 0; i < set->n; i++)
		order[i] = i;
	qsort_arg(order, set->n, sizeof(int), lo_cmp_slot, set->keys);

	keys = (uint32 *) MemoryContextAlloc(st->setcxt, sizeof(uint32) * set->cap);
	conts = (LionContainer **)
		MemoryContextAlloc(st->setcxt, sizeof(LionContainer *) * set->cap);
	img = (uint64 *) MemoryContextAlloc(st->buildcxt, LION_BITSET_BYTES);
	out = (LionContainer *) MemoryContextAlloc(st->buildcxt,
											   LION_CONTAINER_MAX_SIZE);

	for (i = 0; i < set->n;)
	{
		uint32		key = set->keys[order[i]];
		int			j = i + 1;

		while (j < set->n && set->keys[order[j]] == key)
			j++;

		if (j == i + 1 || st->degraded)
		{
			/* one container (or only keys): moved as it is */
			conts[n] = set->conts[order[i]];
			set->conts[order[i]] = NULL;
		}
		else
		{
			int			m;
			Size		size;

			memset(img, 0, LION_BITSET_BYTES);
			for (m = i; m < j; m++)
				lion_container_or_into_bitset(set->conts[order[m]], img);
			lion_bits_to_container(img, key, out);
			size = lion_container_size(out);
			conts[n] = (LionContainer *) MemoryContextAlloc(st->setcxt, size);
			memcpy(conts[n], out, size);
			st->bytes += size;
		}
		keys[n] = key;
		n++;
		i = j;
	}

	/* what was merged away */
	for (i = 0; i < set->n; i++)
	{
		if (set->conts[i] != NULL)
		{
			st->bytes -= lion_container_size(set->conts[i]);
			pfree(set->conts[i]);
		}
	}
	st->bytes -= (set->n - n) * LO_ENTRY_BYTES;
	pfree(set->keys);
	pfree(set->conts);
	pfree(order);
	pfree(img);
	pfree(out);
	set->keys = keys;
	set->conts = conts;
	set->n = n;
	set->nmerged = n;
	set->sorted = true;

	if (!st->degraded && st->bytes > st->limit)
		lo_degrade(st);
}

/* a AND b, or a OR b, of two sorted sets; both are consumed. */
static LionTidSet *
lo_set_combine(LionOrderedState *st, LionTidSet *a, LionTidSet *b, bool isand)
{
	LionTidSet *r = lo_set_new(st);
	LionContainer *buf = (LionContainer *)
		MemoryContextAlloc(st->buildcxt, LION_CONTAINER_MAX_SIZE);
	int			i = 0;
	int			j = 0;

	while (i < a->n || j < b->n)
	{
		CHECK_FOR_INTERRUPTS();

		if (j >= b->n || (i < a->n && a->keys[i] < b->keys[j]))
		{
			if (!isand)
			{
				lo_set_push(st, r, a->keys[i], a->conts[i]);
			}
			i++;
			continue;
		}
		if (i >= a->n || b->keys[j] < a->keys[i])
		{
			if (!isand)
				lo_set_push(st, r, b->keys[j], b->conts[j]);
			j++;
			continue;
		}

		/* the same key in both */
		if (st->degraded || a->conts[i] == NULL || b->conts[j] == NULL)
		{
			/*
			 * Only a degraded set has keys without containers, and then the
			 * result keeps the key: "every TID of these blocks may be one".
			 */
			if (!isand || (a->conts[i] == NULL && b->conts[j] == NULL))
				lo_set_push(st, r, a->keys[i], NULL);
			else
				lo_set_push(st, r, a->keys[i],
							a->conts[i] != NULL ? a->conts[i] : b->conts[j]);
		}
		else if ((isand ? lion_container_and(a->conts[i], b->conts[j], buf) :
				  lion_container_or(a->conts[i], b->conts[j], buf)) > 0)
			lo_set_push(st, r, a->keys[i], buf);
		i++;
		j++;
	}
	pfree(buf);
	lo_set_free(st, a);
	lo_set_free(st, b);
	return r;
}

/* The index of tid's container key if tid is a member, else -1. */
static int
lo_set_find(const LionTidSet *set, ItemPointer tid)
{
	uint64		code = lion_tid_to_code(tid);
	uint32		ckey = lion_code_ckey(code);
	int			lo = 0;
	int			hi = set->n - 1;

	while (lo <= hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (set->keys[mid] == ckey)
			return (set->conts[mid] == NULL ||
					lion_container_contains(set->conts[mid],
											lion_code_lo(code))) ? mid : -1;
		if (set->keys[mid] < ckey)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return -1;
}

/* ---------------------------------------------------------------------
 * Building the set (DESIGN.md §30.4)
 * --------------------------------------------------------------------- */

static LionTidSet *
lo_build_leaf(LionOrderedState *st, LoLeaf *leaf)
{
	LionTidSet *set = lo_set_new(st);
	LionSource *src;
	const LionContainer *c;

	src = lion_source_open(leaf->index, leaf->keys, leaf->nkeys, false,
						   st->buildcxt);
	pgstat_count_index_scan(leaf->index);
	if (!lion_source_exact(src))
		st->exact = false;
	while ((c = lion_source_next(src)) != NULL)
	{
		CHECK_FOR_INTERRUPTS();
		if (c->cardinality > 0)
			lo_set_push(st, set, c->ckey, c);
	}
	lion_source_close(src);
	lo_set_finish(st, set);
	return set;
}

static LionTidSet *
lo_build_node(LionOrderedState *st, LoNode *node)
{
	LionTidSet *acc;
	int			i;

	if (node->kind == LO_NODE_LEAF)
		return lo_build_leaf(st, node->leaf);

	acc = lo_build_node(st, node->child[0]);
	for (i = 1; i < node->nchild; i++)
	{
		/* an empty AND stays empty: the other children need not be read */
		if (node->kind == LO_NODE_AND && acc->n == 0)
			break;
		acc = lo_set_combine(st, acc, lo_build_node(st, node->child[i]),
							 node->kind == LO_NODE_AND);
	}
	return acc;
}

/* The leaves' run-time keys, for this scan's Param values. */
static void
lo_leaf_keys(LionOrderedState *st)
{
	int			i;

	ResetExprContext(st->lrtcxt);
	for (i = 0; i < st->nleaves; i++)
	{
		if (st->leaves[i].nrtkeys > 0)
			ExecIndexEvalRuntimeKeys(st->lrtcxt, st->leaves[i].rtkeys,
									 st->leaves[i].nrtkeys);
	}
}

static void
lo_build_set(LionOrderedState *st)
{
	int			i;

	MemoryContextReset(st->setcxt);
	st->live = NIL;
	st->bytes = 0;
	st->degraded = false;
	st->exact = !st->lossyqual;
	st->limit = get_hash_memory_limit();

	lo_leaf_keys(st);
	st->set = lo_build_node(st, st->tree);
	MemoryContextReset(st->buildcxt);

	st->ncont = st->set->n;
	st->members = 0;
	for (i = 0; i < st->set->n; i++)
	{
		if (st->set->conts[i] != NULL)
			st->members += st->set->conts[i]->cardinality;
	}
	st->builds++;
}

/* ---------------------------------------------------------------------
 * The set, lazily (DESIGN.md §30.4, "The set, lazily")
 * --------------------------------------------------------------------- */

/*
 * Can every key of every leaf be sought in: a set tree - an equality or a
 * list - on a scalar column?  A range is a walk of entries, and a multi-key
 * column's query may need every row; those leaves are only ever built.
 */
static bool
lo_lazy_ok(LionOrderedState *st)
{
	int			i;
	int			k;

	if (st->noset)
		return false;
	for (i = 0; i < st->nleaves; i++)
	{
		LoLeaf	   *leaf = &st->leaves[i];

		if (leaf->nkeys == 0)
			return false;
		for (k = 0; k < leaf->nkeys; k++)
		{
			ScanKey		sk = &leaf->keys[k];
			LionState  *col = lion_index_column_state(leaf->index,
													  sk->sk_attno);

			if (col->multikey || sk->sk_strategy != LION_STRAT_EQUAL ||
				(sk->sk_flags & (SK_SEARCHNULL | SK_SEARCHNOTNULL |
								 SK_ROW_HEADER)) != 0)
				return false;
		}
	}
	return true;
}

/*
 * Let go of the lazy set: every stream, and every set - their pins before
 * the memory they live in (lion_posting_set_release()) - and the memo.
 */
static void
lo_lazy_end(LionOrderedState *st)
{
	int			i;
	int			k;
	int			s;

	for (i = 0; i < st->nleaves; i++)
	{
		LoLeaf	   *leaf = &st->leaves[i];

		if (leaf->pkeys == NULL)
			continue;
		for (k = 0; k < leaf->nkeys; k++)
		{
			LoProbeKey *pk = &leaf->pkeys[k];

			if (pk->stream != NULL)
				lion_stream_end(pk->stream);
			pk->stream = NULL;
			for (s = 0; s < pk->nsets; s++)
				lion_posting_set_release(&pk->sets[s]);
			pk->nsets = 0;
		}
		leaf->pkeys = NULL;
	}
	st->memo = NULL;
	st->memobytes = 0;
	st->lazy = false;
	if (st->lazycxt != NULL)
		MemoryContextReset(st->lazycxt);
}

/* Does node select nothing, whatever the key: a key of no entry at all? */
static bool
lo_lazy_empty(LoNode *node)
{
	int			i;

	if (node->kind == LO_NODE_LEAF)
	{
		for (i = 0; i < node->leaf->nkeys; i++)
		{
			if (node->leaf->pkeys[i].tree == NULL)
				return true;
		}
		return false;
	}
	for (i = 0; i < node->nchild; i++)
	{
		bool		empty = lo_lazy_empty(node->child[i]);

		if (node->kind == LO_NODE_AND && empty)
			return true;
		if (node->kind == LO_NODE_OR && !empty)
			return false;
	}
	return node->kind == LO_NODE_OR;
}

/*
 * How many members node has at most, by the located entries' recorded member
 * counts: an AND no more than its smallest child, an OR no more than its
 * children together.  The counts are hints; this decides only when the set
 * is built, never what it holds.
 */
static double
lo_lazy_members(LoNode *node)
{
	double		n = 0.0;
	int			i;
	int			s;

	if (node->kind == LO_NODE_LEAF)
	{
		for (i = 0; i < node->leaf->nkeys; i++)
		{
			LoProbeKey *pk = &node->leaf->pkeys[i];
			double		m = 0.0;

			if (pk->tree != NULL)
				for (s = 0; s < pk->nsets; s++)
					m += (double) pk->sets[s].ntids;
			n = (i == 0) ? m : Min(n, m);
		}
		return n;
	}
	for (i = 0; i < node->nchild; i++)
	{
		double		m = lo_lazy_members(node->child[i]);

		if (node->kind == LO_NODE_AND)
			n = (i == 0) ? m : Min(n, m);
		else
			n += m;
	}
	return n;
}

/*
 * Start the set lazily: locate every leaf key's posting sets, as a bitmap
 * scan locates them, let go of their pins, and make the memo.  False when a
 * key is no set tree after all, or a list longer than a plain scan opens at
 * once (lion_scan_list_batch(), §29.4; a stream holds a cursor for each of
 * its sets): the set is then built, a batch at a time.
 */
static bool
lo_lazy_begin(LionOrderedState *st)
{
	MemoryContext oldcxt;
	double		containers = 0.0;
	int			i;
	int			k;
	int			s;

	lo_leaf_keys(st);
	lo_lazy_end(st);
	oldcxt = MemoryContextSwitchTo(st->lazycxt);

	for (i = 0; i < st->nleaves; i++)
	{
		LoLeaf	   *leaf = &st->leaves[i];

		leaf->pkeys = (LoProbeKey *) palloc0(sizeof(LoProbeKey) * leaf->nkeys);
		for (k = 0; k < leaf->nkeys; k++)
		{
			LoProbeKey *pk = &leaf->pkeys[k];
			ScanKey		sk = &leaf->keys[k];
			bool		nomatch = false;
			bool		ok;

			if ((sk->sk_flags & (SK_SEARCHARRAY | SK_ISNULL)) == SK_SEARCHARRAY)
			{
				ArrayType  *arr = DatumGetArrayTypeP(sk->sk_argument);

				if (ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr)) >
					lion_scan_list_batch())
				{
					MemoryContextSwitchTo(oldcxt);
					lo_lazy_end(st);
					return false;
				}
			}
			ok = lion_scankey_sets(leaf->index, sk, &pk->nsets, &pk->sets,
								   &pk->tree, &nomatch);
			for (s = 0; s < pk->nsets; s++)
			{
				lion_posting_set_unpin(&pk->sets[s]);
				containers += (double) pk->sets[s].ncontainers;
			}
			if (!ok)
			{
				MemoryContextSwitchTo(oldcxt);
				lo_lazy_end(st);
				return false;
			}
			if (nomatch)
				pk->tree = NULL;
			pk->cxt = AllocSetContextCreate(st->lazycxt, "LionOrdered stream",
											ALLOCSET_SMALL_SIZES);
			CHECK_FOR_INTERRUPTS();
		}
		pgstat_count_index_scan(leaf->index);
	}

	st->memo = lo_memo_create(st->lazycxt, 256, NULL);
	MemoryContextSwitchTo(oldcxt);

	st->memobytes = 0;
	st->lazywork = 0.0;
	st->lazyevals = 0;
	st->lazyback = 0;
	st->lazylast = 0;
	st->lazybudget = Max((double) LO_LAZY_MIN_WORK, containers);
	st->limit = get_hash_memory_limit();
	st->exact = !st->lossyqual;
	st->lazyempty = lo_lazy_empty(st->tree);
	st->lazymembers = lo_lazy_members(st->tree);
	st->lazy = true;
	return true;
}

/*
 * The container of pk's tree at ckey, or NULL: valid until pk is sought
 * again.  The stream only goes forward, so a probe behind where it stands
 * starts it again, over the same located sets.
 */
static const LionContainer *
lo_probe_at(LionOrderedState *st, LoProbeKey *pk, uint32 ckey)
{
	const LionContainer *c;

	if (pk->tree == NULL)
		return NULL;
	if (pk->stream != NULL && ckey < pk->last)
	{
		lion_stream_end(pk->stream);
		pk->stream = NULL;
		MemoryContextReset(pk->cxt);
		st->lazywork += (double) (LO_LAZY_RESTART_WORK * pk->nsets);
	}
	if (pk->stream == NULL)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(pk->cxt);

		pk->stream = lion_stream_begin(pk->nsets, pk->sets, pk->tree, false);
		MemoryContextSwitchTo(oldcxt);
	}
	pk->last = ckey;
	st->lazywork += (double) Max(pk->nsets, 1);
	c = lion_stream_at(pk->stream, ckey);
	return (c != NULL && c->ckey == ckey && c->cardinality > 0) ? c : NULL;
}

/* A copy of c in probecxt, with room for what an AND or an OR makes. */
static LionContainer *
lo_probe_copy(LionOrderedState *st, const LionContainer *c)
{
	LionContainer *r = (LionContainer *)
		MemoryContextAlloc(st->probecxt, LION_CONTAINER_MAX_SIZE);

	memcpy(r, c, lion_container_size(c));
	return r;
}

/* What node selects at ckey, in probecxt, or NULL for nothing. */
static LionContainer *
lo_probe_node(LionOrderedState *st, LoNode *node, uint32 ckey)
{
	LionContainer *acc = NULL;
	int			i;

	if (node->kind == LO_NODE_LEAF)
	{
		LoLeaf	   *leaf = node->leaf;

		for (i = 0; i < leaf->nkeys; i++)
		{
			const LionContainer *c = lo_probe_at(st, &leaf->pkeys[i], ckey);
			LionContainer *r;

			if (c == NULL)
				return NULL;
			if (acc == NULL)
			{
				acc = lo_probe_copy(st, c);
				continue;
			}
			r = (LionContainer *) MemoryContextAlloc(st->probecxt,
													 LION_CONTAINER_MAX_SIZE);
			if (lion_container_and(acc, c, r) == 0)
				return NULL;
			acc = r;
		}
		return acc;
	}

	for (i = 0; i < node->nchild; i++)
	{
		LionContainer *c = lo_probe_node(st, node->child[i], ckey);
		LionContainer *r;

		if (c == NULL)
		{
			/* an empty AND stays empty: the other children need not be read */
			if (node->kind == LO_NODE_AND)
				return NULL;
			continue;
		}
		if (acc == NULL)
		{
			acc = c;
			continue;
		}
		r = (LionContainer *) MemoryContextAlloc(st->probecxt,
												 LION_CONTAINER_MAX_SIZE);
		if (node->kind == LO_NODE_AND)
		{
			if (lion_container_and(acc, c, r) == 0)
				return NULL;
		}
		else
			(void) lion_container_or(acc, c, r);
		acc = r;
	}
	return acc;
}

/*
 * The memo's entry for tid's container key if tid is a member, else NULL: a
 * key met for the first time is evaluated and remembered, members or not.
 */
static LoMemoEnt *
lo_lazy_find(LionOrderedState *st, ItemPointer tid)
{
	uint64		code = lion_tid_to_code(tid);
	uint32		ckey = lion_code_ckey(code);
	LoMemoEnt  *e = lo_memo_lookup(st->memo, ckey);

	if (e == NULL)
	{
		LionContainer *c;
		bool		found;

		/* how far the walk goes in heap order (LO_LAZY_ORDER_KEYS) */
		if (st->lazyevals > 0 && ckey < st->lazylast)
			st->lazyback++;
		st->lazyevals++;
		st->lazylast = ckey;

		MemoryContextReset(st->probecxt);
		c = lo_probe_node(st, st->tree, ckey);
		e = lo_memo_insert(st->memo, ckey, &found);
		e->c = NULL;
		e->visited = NULL;
		if (c != NULL && c->cardinality > 0)
		{
			Size		size = lion_container_size(c);

			e->c = (LionContainer *) MemoryContextAlloc(st->lazycxt, size);
			memcpy(e->c, c, size);
			st->memobytes += size;
		}
		st->memobytes += sizeof(LoMemoEnt);
		st->lazykeys++;
		CHECK_FOR_INTERRUPTS();
	}
	if (e->c == NULL || !lion_container_contains(e->c, lion_code_lo(code)))
		return NULL;
	return e;
}

/* lo_mark() for the lazy set: the memo keeps what the walk met. */
static bool
lo_lazy_mark(LionOrderedState *st, LoMemoEnt *e, ItemPointer tid)
{
	uint16		lo = lion_code_lo(lion_tid_to_code(tid));
	uint64		bit = UINT64CONST(1) << (lo & 63);

	if (e->visited == NULL)
	{
		e->visited = (uint64 *) MemoryContextAllocZero(st->scancxt,
													   LION_BITSET_BYTES);
		st->memobytes += LION_BITSET_BYTES;
	}
	if (e->visited[lo >> 6] & bit)
		return false;
	e->visited[lo >> 6] |= bit;
	return true;
}

/* The index of container key ckey in set, or -1. */
static int
lo_set_key_index(const LionTidSet *set, uint32 ckey)
{
	int			lo = 0;
	int			hi = set->n - 1;

	while (lo <= hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (set->keys[mid] == ckey)
			return mid;
		if (set->keys[mid] < ckey)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return -1;
}

/*
 * The probes have cost what the build would, or the walk has gone far: build
 * the set, and hand it what the walk met so far, so that the early stop and
 * the switch count those members as met.  A member the build does not have
 * cannot be - it is the same answer - but should one be missing, the scan
 * stops counting (novisit) rather than let the switch fetch it again.
 *
 * The lazy set goes first: the build evaluates the leaves' run-time keys
 * again, and the values the lazy sets were located from go with them.  What
 * the walk met lives in scancxt, and outlives it.
 */
static void
lo_lazy_convert(LionOrderedState *st)
{
	lo_memo_iterator it;
	LoMemoEnt  *e;
	uint32	   *mkeys;
	uint64	  **mvisited;
	int			nmet = 0;
	uint64	  **visited = NULL;
	bool		lost = false;
	int			i;

	mkeys = (uint32 *) MemoryContextAlloc(st->scancxt,
										  sizeof(uint32) * Max(st->memo->members, 1));
	mvisited = (uint64 **) MemoryContextAlloc(st->scancxt,
											  sizeof(uint64 *) * Max(st->memo->members, 1));
	lo_memo_start_iterate(st->memo, &it);
	while ((e = lo_memo_iterate(st->memo, &it)) != NULL)
	{
		if (e->visited == NULL)
			continue;
		mkeys[nmet] = e->ckey;
		mvisited[nmet++] = e->visited;
	}
	lo_lazy_end(st);

	lo_build_set(st);
	if (!st->degraded && st->set->n > 0)
		visited = (uint64 **) MemoryContextAllocZero(st->scancxt,
													 sizeof(uint64 *) * st->set->n);
	for (i = 0; i < nmet; i++)
	{
		int			idx = (visited != NULL) ?
			lo_set_key_index(st->set, mkeys[i]) : -1;

		if (idx < 0)
		{
			lost = true;
			continue;
		}
		visited[idx] = mvisited[i];
		st->visitedbytes += LION_BITSET_BYTES;
	}
	st->visited = visited;
	if (lost)
		st->novisit = true;
	pfree(mkeys);
	pfree(mvisited);
}

/* ---------------------------------------------------------------------
 * Executor
 * --------------------------------------------------------------------- */

static Node *
lo_create_state(CustomScan *cscan)
{
	LionOrderedState *st = (LionOrderedState *)
		newNode(sizeof(LionOrderedState), T_CustomScanState);
	bool		indexonly = false;

	st->css.methods = &lo_exec_methods;

	/*
	 * The scan slot's type is fixed per plan, here, before ExecInitCustomScan
	 * initialises the projection and the quals on it: heap tuples, fetched
	 * through the table AM into a buffer slot; or, in index-only mode, the
	 * index tuple's values in a virtual slot of the relation's shape
	 * (DESIGN.md §40.4).  lo_begin() checks the rest of the shape.
	 */
	if (list_length(cscan->custom_private) == LO_PRIV_NMEMBERS)
	{
		List	   *ints = (List *) list_nth(cscan->custom_private, LO_PRIV_INTS);

		indexonly = ints != NIL && IsA(ints, IntList) &&
			list_length(ints) >= 2 &&
			(lsecond_int(ints) & LO_FLAG_INDEXONLY) != 0;
	}
	st->css.slotOps = indexonly ? &TTSOpsVirtual : &TTSOpsBufferHeapTuple;
	return (Node *) st;
}

/*
 * An index qual as ExecIndexBuildScanKeys() wants it: the key operand
 * replaced by an INDEX_VAR Var for index column col (0-based), as
 * fix_indexqual_references() does for an Index Scan.
 */
static Expr *
lo_fix_qual(Expr *clause, int col)
{
	Node	   *key;
	Var		   *var;

	clause = copyObject(clause);
	if (IsA(clause, OpExpr))
		key = linitial(((OpExpr *) clause)->args);
	else if (IsA(clause, ScalarArrayOpExpr))
		key = linitial(((ScalarArrayOpExpr *) clause)->args);
	else if (IsA(clause, NullTest))
		key = (Node *) ((NullTest *) clause)->arg;
	else
	{
		elog(ERROR, "LionOrdered: unsupported index qual type %d",
			 (int) nodeTag(clause));
		return NULL;			/* keep compiler quiet */
	}

	var = makeVar(INDEX_VAR, (AttrNumber) (col + 1), exprType(key),
				  exprTypmod(key), exprCollation(key), 0);
	if (IsA(clause, OpExpr))
		linitial(((OpExpr *) clause)->args) = var;
	else if (IsA(clause, ScalarArrayOpExpr))
		linitial(((ScalarArrayOpExpr *) clause)->args) = var;
	else
		((NullTest *) clause)->arg = (Expr *) var;
	return clause;
}

/* The lion tree out of custom_private, and every leaf's quals. */
static LoNode *
lo_decode_tree(LionOrderedState *st, List *tree, ListCell **pos,
			   List *leafoids, List *lionquals, int *leafno, int *qualno)
{
	List	   *item = (List *) lfirst(*pos);
	LoNode	   *node = (LoNode *) palloc0(sizeof(LoNode));
	int			i;

	*pos = lnext(tree, *pos);
	node->kind = linitial_int(item);
	node->nchild = lsecond_int(item);

	if (node->kind == LO_NODE_LEAF)
	{
		LoLeaf	   *leaf = &st->leaves[*leafno];

		leaf->indexoid = list_nth_oid(leafoids, *leafno);
		leaf->quals = NIL;
		for (i = 0; i < node->nchild; i++)
		{
			Expr	   *q = (Expr *) list_nth(lionquals, *qualno);

			leaf->quals = lappend(leaf->quals,
								  lo_fix_qual(q, list_nth_int(item, 2 + i)));
			(*qualno)++;
		}
		node->nchild = 0;
		node->leaf = leaf;
		(*leafno)++;
		return node;
	}

	node->child = (LoNode **) palloc(sizeof(LoNode *) * node->nchild);
	for (i = 0; i < node->nchild; i++)
		node->child[i] = lo_decode_tree(st, tree, pos, leafoids, lionquals,
										leafno, qualno);
	return node;
}

static void
lo_begin(CustomScanState *node, EState *estate, int eflags)
{
	LionOrderedState *st = (LionOrderedState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	List	   *shape;
	List	   *ints;
	List	   *ordcols;
	List	   *tree;
	List	   *leafoids;
	List	   *lionquals;
	List	   *ordquals;
	List	   *fixed = NIL;
	ListCell   *pos;
	int			leafno = 0;
	int			qualno = 0;
	int			i;

	shape = (list_length(cscan->custom_private) == LO_PRIV_NMEMBERS) ?
		(List *) list_nth(cscan->custom_private, LO_PRIV_SHAPE) : NIL;
	if (shape == NIL || !IsA(shape, IntList) || list_length(shape) != 2 ||
		linitial_int(shape) != LO_PRIV_MAGIC ||
		lsecond_int(shape) != LO_PRIV_NMEMBERS ||
		list_length(cscan->custom_exprs) != LO_EXPR_NLISTS)
		elog(ERROR, "LionOrdered: unrecognized custom_private shape (%d members)",
			 list_length(cscan->custom_private));

	st->ordoid = linitial_oid((List *) list_nth(cscan->custom_private, LO_PRIV_ORD));
	ints = (List *) list_nth(cscan->custom_private, LO_PRIV_INTS);
	st->dir = (ScanDirection) linitial_int(ints);
	st->lossyqual = (lsecond_int(ints) & LO_FLAG_LOSSY) != 0;
	st->lionwalk = (lsecond_int(ints) & LO_FLAG_LIONWALK) != 0;
	st->indexonly = (lsecond_int(ints) & LO_FLAG_INDEXONLY) != 0;
	st->lionheaprecheck = (lsecond_int(ints) & LO_FLAG_HEAPRECHECK) != 0;
	st->vmbuffer = InvalidBuffer;
	st->walknulls = lthird_int(ints);
	st->walkattno = (AttrNumber) lfourth_int(ints);
	ordcols = (List *) list_nth(cscan->custom_private, LO_PRIV_ORDCOLS);
	tree = (List *) list_nth(cscan->custom_private, LO_PRIV_TREE);
	leafoids = (List *) list_nth(cscan->custom_private, LO_PRIV_LEAVES);
	lionquals = (List *) list_nth(cscan->custom_exprs, LO_EXPR_LIONQUALS);
	ordquals = (List *) list_nth(cscan->custom_exprs, LO_EXPR_ORDQUALS);

	/*
	 * What the node evaluates in place of the ordinary plan, initialised - and
	 * so checked for EXECUTE - every time the plan starts, EXPLAIN included,
	 * as an Index Scan initialises its indexqualorig (DESIGN.md §30.6).  The
	 * filter is core's (ExecInitCustomScan).
	 */
	st->lionrecheck = ExecInitQual((List *) list_nth(cscan->custom_exprs,
													  LO_EXPR_LIONQUAL),
								   &node->ss.ps);
	st->ordrecheck = ExecInitQual((List *) list_nth(cscan->custom_exprs,
													 LO_EXPR_ORDORIG),
								  &node->ss.ps);
	st->valuequal = ExecInitQual((List *) list_nth(cscan->custom_exprs,
													LO_EXPR_VALUEQUAL),
								 &node->ss.ps);
	st->valueentry = -1;

	st->nleaves = list_length(leafoids);
	st->leaves = (LoLeaf *) palloc0(sizeof(LoLeaf) * Max(st->nleaves, 1));
	st->noset = (tree == NIL);
	if (st->noset && !st->lionwalk)
		elog(ERROR, "LionOrdered: a btree's walk without a lion set");
	if (!st->noset)
	{
		pos = list_head(tree);
		st->tree = lo_decode_tree(st, tree, &pos, leafoids, lionquals,
								  &leafno, &qualno);
		if (pos != NULL)
			elog(ERROR, "LionOrdered: malformed lion tree");
	}
	if (leafno != st->nleaves || qualno != list_length(lionquals))
		elog(ERROR, "LionOrdered: malformed lion tree");

	/* EXPLAIN without ANALYZE opens nothing (it names indexes by Oid). */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	lion_check_table_am(node->ss.ss_currentRelation);
	if (!IsMVCCSnapshot(estate->es_snapshot))
		elog(ERROR, "LionOrdered: requires an MVCC snapshot");

	st->setcxt = AllocSetContextCreate(estate->es_query_cxt,
									   "LionOrdered set",
									   ALLOCSET_DEFAULT_SIZES);
	st->buildcxt = AllocSetContextCreate(estate->es_query_cxt,
										 "LionOrdered build",
										 ALLOCSET_DEFAULT_SIZES);
	st->ortcxt = CreateExprContext(estate);
	st->scancxt = AllocSetContextCreate(estate->es_query_cxt,
										"LionOrdered scan",
										ALLOCSET_DEFAULT_SIZES);
	if (st->lionwalk)
		st->walkcxt = AllocSetContextCreate(estate->es_query_cxt,
											"LionOrdered walk",
											ALLOCSET_DEFAULT_SIZES);

	/* the fetch-and-sort switch's sort keys (§30.4), when there are any */
	{
		List	   *sk = (List *) list_nth(cscan->custom_private, LO_PRIV_SORT);

		if (sk != NIL)
		{
			List	   *attnos = (List *) linitial(sk);
			List	   *ops = (List *) lsecond(sk);
			List	   *colls = (List *) lthird(sk);
			List	   *nulls = (List *) lfourth(sk);

			st->nsort = list_length(attnos);
			st->sortattnos = (AttrNumber *) palloc(sizeof(AttrNumber) * st->nsort);
			st->sortkeys = (SortSupport) palloc0(sizeof(SortSupportData) * st->nsort);
			for (i = 0; i < st->nsort; i++)
			{
				SortSupport ssup = &st->sortkeys[i];

				st->sortattnos[i] = (AttrNumber) list_nth_int(attnos, i);
				ssup->ssup_cxt = CurrentMemoryContext;
				ssup->ssup_collation = list_nth_oid(colls, i);
				ssup->ssup_nulls_first = list_nth_int(nulls, i) != 0;
				ssup->ssup_attno = st->sortattnos[i];
				ssup->abbreviate = false;
				PrepareSortSupportFromOrderingOp(list_nth_oid(ops, i), ssup);
			}
		}
	}

	/* the switch's terms (§30.4, §40.4): the setting, and the planner's walk */
	st->switchratio = lion_ordered_switch_ratio;
	{
		Const	   *c = (Const *) list_nth(cscan->custom_private,
										   LO_PRIV_EXPECTED);

		if (c == NULL || !IsA(c, Const) || c->consttype != FLOAT8OID ||
			c->constisnull)
			elog(ERROR, "LionOrdered: malformed expected walk");
		st->expectwalk = DatumGetFloat8(c->constvalue);
	}
	st->lrtcxt = CreateExprContext(estate);

	/* the ordered index and its scan keys */
	st->ordidx = index_open(st->ordoid, AccessShareLock);
	i = 0;
	foreach(pos, ordquals)
		fixed = lappend(fixed, lo_fix_qual((Expr *) lfirst(pos),
										   list_nth_int(ordcols, i++)));
	ExecIndexBuildScanKeys(&node->ss.ps, st->ordidx, fixed, false,
						   &st->okeys, &st->nokeys,
						   &st->ortkeys, &st->nortkeys, NULL, NULL);
	if (st->indexonly)
	{
		/*
		 * Index-only mode (DESIGN.md §40.4): the btree's columns mapped to
		 * the relation's, a name column the btree stores as a cstring (its
		 * name_ops) copied back into a Name, as core's Index Only Scan copies
		 * it, and a slot of the table AM's for the heap tuple of a page not
		 * all-visible, read for its visibility alone.
		 */
		Relation	heap = node->ss.ss_currentRelation;
		TupleDesc	heapdesc = RelationGetDescr(heap);
		TupleDesc	idxdesc = RelationGetDescr(st->ordidx);
		List	   *iocols = (List *) list_nth(cscan->custom_private,
											   LO_PRIV_IOCOLS);

		if (st->lionwalk || list_length(iocols) != idxdesc->natts)
			elog(ERROR, "LionOrdered: malformed index-only mapping");
		st->niocols = idxdesc->natts;
		st->iocols = (AttrNumber *) palloc(sizeof(AttrNumber) * st->niocols);
		st->ionames = (bool *) palloc0(sizeof(bool) * st->niocols);
		st->ionamebuf = (NameData *) palloc0(sizeof(NameData) * st->niocols);
		st->iovals = (Datum *) palloc(sizeof(Datum) * st->niocols);
		st->ionulls = (bool *) palloc(sizeof(bool) * st->niocols);
		for (i = 0; i < st->niocols; i++)
		{
			AttrNumber	attno = (AttrNumber) list_nth_int(iocols, i);

			if (attno < 0 || attno > heapdesc->natts)
				elog(ERROR, "LionOrdered: malformed index-only mapping");
			st->iocols[i] = attno;
			st->ionames[i] = attno > 0 &&
				TupleDescAttr(idxdesc, i)->atttypid == CSTRINGOID &&
				TupleDescAttr(heapdesc, attno - 1)->atttypid == NAMEOID;
		}
		st->tabslot = table_slot_create(heap, &estate->es_tupleTable);
	}
	if (st->lionwalk)
	{
		/*
		 * The walk reads the lion index without index_beginscan(), as the
		 * set does: a relation predicate lock first (§30.5), and the heap
		 * fetch state of an index scan for what it meets.
		 */
		PredicateLockRelation(st->ordidx, estate->es_snapshot);
#if PG_VERSION_NUM >= 200000
#elif PG_VERSION_NUM >= 190000
		st->fetch = table_index_fetch_begin(node->ss.ss_currentRelation,
											SO_NONE);
#else
		st->fetch = table_index_fetch_begin(node->ss.ss_currentRelation);
#endif
	}

	/*
	 * Every lion index, and a relation predicate lock on each before any
	 * lookup (DESIGN.md §30.5; the AM has no ampredlocks, and the node reads
	 * it without index_beginscan()).
	 */
	for (i = 0; i < st->nleaves; i++)
	{
		LoLeaf	   *leaf = &st->leaves[i];

		leaf->index = index_open(leaf->indexoid, AccessShareLock);
		ExecIndexBuildScanKeys(&node->ss.ps, leaf->index, leaf->quals, false,
							   &leaf->keys, &leaf->nkeys,
							   &leaf->rtkeys, &leaf->nrtkeys, NULL, NULL);
		PredicateLockRelation(leaf->index, estate->es_snapshot);
	}

	/* which Params a rescan has to rebuild the set for */
	st->lionparams = pull_paramids((Expr *) lionquals);

	/* the set evaluated lazily, when every leaf's keys can be sought (§30.4) */
	st->lazyok = lion_enable_lazy_set && lo_lazy_ok(st);
	if (st->lazyok)
	{
		st->lazycxt = AllocSetContextCreate(estate->es_query_cxt,
											"LionOrdered lazy set",
											ALLOCSET_DEFAULT_SIZES);
		st->probecxt = AllocSetContextCreate(estate->es_query_cxt,
											 "LionOrdered probe",
											 ALLOCSET_DEFAULT_SIZES);
	}
}

/* The next TID of the ordered index, or NULL at its end. */
static ItemPointer
lo_next_tid(LionOrderedState *st)
{
	if (st->lionwalk)
		return lion_order_walk_next(st->owalk, &st->walktid) ?
			&st->walktid : NULL;
#if PG_VERSION_NUM >= 200000
	if (!tableam_index_getnext_tid(st->scan, st->dir))
		return NULL;
	return &st->scan->xs_heaptid;
#else
	return index_getnext_tid(st->scan, st->dir);
#endif
}

/*
 * Fetch the version of the TID just returned that the snapshot sees, into
 * slot (DESIGN.md §30.4).  20 moved the heap side of index scans into the
 * table AM; there the HOT chain is searched with table_fetch_tid(), which
 * moves the TID to the visible version, and that version is then read.
 */
static bool
lo_fetch_btree(LionOrderedState *st, TupleTableSlot *slot)
{
#if PG_VERSION_NUM >= 200000
	IndexScanDesc scan = st->scan;
	ItemPointerData tid = scan->xs_heaptid;
	bool		all_dead = false;
	Snapshot	snapshot = scan->xs_snapshot;

	if (!table_fetch_tid(scan->heapRelation, &tid, snapshot, &all_dead))
	{
		/* as an index scan does: let the index mark a dead chain */
		if (!scan->xactStartedInRecovery)
			scan->kill_prior_tuple = all_dead;
		return false;
	}
	return table_tuple_fetch_row_version(scan->heapRelation, &tid, snapshot,
										 slot);
#else
	return index_fetch_heap(st->scan, slot);
#endif
}

/*
 * ... and of the TID a lion column's walk returned (DESIGN.md §30.11): the
 * root of a HOT chain, as an index's TID is, looked up the way an index scan
 * looks one up - on 16 .. 19 through the fetch state an index scan keeps,
 * which holds the pin on the last heap page between calls.  Nothing marks
 * dead entries: lion has no such hint.
 */
static bool
lo_fetch_walk(LionOrderedState *st, TupleTableSlot *slot)
{
	Snapshot	snapshot = st->css.ss.ps.state->es_snapshot;
	ItemPointerData tid = st->walktid;
	bool		all_dead = false;
#if PG_VERSION_NUM >= 200000
	Relation	heap = st->css.ss.ss_currentRelation;

	if (!table_fetch_tid(heap, &tid, snapshot, &all_dead))
		return false;
	return table_tuple_fetch_row_version(heap, &tid, snapshot, slot);
#else
	bool		call_again = false;

	return table_index_fetch_tuple(st->fetch, &tid, snapshot, slot,
								   &call_again, &all_dead);
#endif
}

static bool
lo_fetch(LionOrderedState *st, TupleTableSlot *slot)
{
	return st->lionwalk ? lo_fetch_walk(st, slot) : lo_fetch_btree(st, slot);
}

/*
 * The row of the TID the btree just returned, in index-only mode (DESIGN.md
 * §40.4), as core's Index Only Scan makes it: when the visibility map calls
 * its page all-visible every tuple there is visible to every snapshot, this
 * one included, and the heap is not read; otherwise the heap tuple is fetched
 * - into the node's own slot, for its visibility - and a TID with no visible
 * version is skipped.  Either way the values are the index tuple's, which the
 * visible version reached from the root TID shares (an update of an indexed
 * or INCLUDE column is never HOT), laid into the scan slot through the
 * mapping, every other column NULL.  *fromheap says the heap was read: the
 * row then holds its tuple predicate lock already, and its heap tuple stays
 * in the node's slot for a recheck from the heap (lo_lion_recheck()) until
 * the caller is done with the row.
 */
static bool
lo_fetch_indexonly(LionOrderedState *st, TupleTableSlot *slot, bool *fromheap)
{
	IndexScanDesc scan = st->scan;
	ItemPointer tid = &scan->xs_heaptid;
	int			i;

	*fromheap = false;
	if (!VM_ALL_VISIBLE(scan->heapRelation, ItemPointerGetBlockNumber(tid),
						&st->vmbuffer))
	{
		st->fetched++;
		if (!lo_fetch_btree(st, st->tabslot))
			return false;
		*fromheap = true;
	}

	/* the index tuple, or the heap-format tuple an AM may return instead */
	if (scan->xs_hitup != NULL)
	{
		Assert(scan->xs_hitupdesc->natts == st->niocols);
		heap_deform_tuple(scan->xs_hitup, scan->xs_hitupdesc, st->iovals,
						  st->ionulls);
	}
	else if (scan->xs_itup != NULL)
	{
		Assert(scan->xs_itupdesc->natts == st->niocols);
		index_deform_tuple(scan->xs_itup, scan->xs_itupdesc, st->iovals,
						   st->ionulls);
	}
	else
		elog(ERROR, "LionOrdered: no index tuple returned in index-only mode");

	ExecClearTuple(slot);
	memset(slot->tts_isnull, true,
		   sizeof(bool) * slot->tts_tupleDescriptor->natts);
	for (i = 0; i < st->niocols; i++)
	{
		AttrNumber	attno = st->iocols[i];

		if (attno == 0)
			continue;
		if (st->ionames[i] && !st->ionulls[i])
		{
			namestrcpy(&st->ionamebuf[i], DatumGetCString(st->iovals[i]));
			st->iovals[i] = NameGetDatum(&st->ionamebuf[i]);
		}
		slot->tts_values[attno - 1] = st->iovals[i];
		slot->tts_isnull[attno - 1] = st->ionulls[i];
	}
	ExecStoreVirtualTuple(slot);
	slot->tts_tid = *tid;
	slot->tts_tableOid = RelationGetRelid(scan->heapRelation);
	return true;
}

/*
 * The lion recheck of a row the walk met, when the set is inexact or degraded
 * (DESIGN.md §30.4): on the scan slot, as every recheck is - or, in index-only
 * mode over a btree that lacks a column of the lion qual (§40.4), on the heap
 * tuple, fetched now into the node's table slot if the visibility test did
 * not fetch it already (and counted as a heap fetch; a TID with no visible
 * version is skipped).  Evaluating the recheck on that slot is sound although
 * it was initialised for the virtual scan slot: the scan slot's type is fixed
 * (ExecInitScanTupleSlot sets scanopsfixed), so ExecInitQual emitted no
 * EEOP_SCAN_FETCHSOME step for it (ExecComputeSlotInfo drops the step for a
 * fixed virtual slot, and with it the slot type check that step makes), and
 * its Var steps read tts_values[] and tts_isnull[] directly - which
 * slot_getallattrs() fills for the heap tuple.  *fromheap is set when the heap
 * was read here: the row then holds its tuple predicate lock.
 */
static bool
lo_lion_recheck(LionOrderedState *st, TupleTableSlot *slot, bool *fromheap)
{
	ExprContext *econtext = st->css.ss.ps.ps_ExprContext;
	TupleTableSlot *rslot = slot;

	if (st->indexonly && st->lionheaprecheck)
	{
		if (!*fromheap)
		{
			st->fetched++;
			if (!lo_fetch_btree(st, st->tabslot))
				return false;
			*fromheap = true;
		}
		slot_getallattrs(st->tabslot);
		rslot = st->tabslot;
	}
	ResetExprContext(econtext);
	econtext->ecxt_scantuple = rslot;
	if (ExecQual(st->lionrecheck, econtext))
		return true;
	st->removed++;
	return false;
}

/* End this scan's walk of a lion column, keeping its count of keys. */
static void
lo_walk_end(LionOrderedState *st)
{
	int64		entries;
	int64		leaves;

	if (st->owalk == NULL)
		return;
	lion_order_walk_counts(st->owalk, &entries, &leaves);
	st->walkkeys += (uint64) entries;
	lion_order_walk_end(st->owalk);
	st->owalk = NULL;
	MemoryContextReset(st->walkcxt);
}

static void
lo_start_walk(LionOrderedState *st)
{
	EState	   *estate = st->css.ss.ps.state;

	if (st->nortkeys > 0)
	{
		ResetExprContext(st->ortcxt);
		ExecIndexEvalRuntimeKeys(st->ortcxt, st->ortkeys, st->nortkeys);
	}
	if (st->lionwalk)
	{
		lo_walk_end(st);
		st->owalk = lion_order_walk_begin(st->ordidx, st->walkattno,
										  st->okeys, st->nokeys,
										  ScanDirectionIsBackward(st->dir),
										  st->walknulls, st->walkcxt);
		st->valueentry = -1;	/* a new walk numbers its entries afresh */
		pgstat_count_index_scan(st->ordidx);
		st->scans++;
		st->started = true;
		st->done = false;
		return;
	}
	if (st->scan == NULL)
	{
#if PG_VERSION_NUM >= 200000
		st->scan = index_beginscan(st->css.ss.ss_currentRelation, st->ordidx,
								   false, estate->es_snapshot, NULL,
								   st->nokeys, 0, SO_NONE);
#elif PG_VERSION_NUM >= 190000
		st->scan = index_beginscan(st->css.ss.ss_currentRelation, st->ordidx,
								   estate->es_snapshot, NULL,
								   st->nokeys, 0, SO_NONE);
#elif PG_VERSION_NUM >= 180000
		st->scan = index_beginscan(st->css.ss.ss_currentRelation, st->ordidx,
								   estate->es_snapshot, NULL, st->nokeys, 0);
#else
		st->scan = index_beginscan(st->css.ss.ss_currentRelation, st->ordidx,
								   estate->es_snapshot, st->nokeys, 0);
#endif
		/* index-only mode reads the index tuples (§40.4), as core's does */
		st->scan->xs_want_itup = st->indexonly;
	}
	index_rescan(st->scan, st->okeys, st->nokeys, NULL, 0);
	st->scans++;
	st->started = true;
	st->done = false;
}

/* Forget this scan's walk: what it met, and a switch it made. */
static void
lo_scan_reset(LionOrderedState *st)
{
	/* what the lazy set's memo says this scan met goes with scancxt */
	if (st->memo != NULL)
	{
		lo_memo_iterator it;
		LoMemoEnt  *e;

		lo_memo_start_iterate(st->memo, &it);
		while ((e = lo_memo_iterate(st->memo, &it)) != NULL)
		{
			if (e->visited != NULL)
				st->memobytes -= LION_BITSET_BYTES;
			e->visited = NULL;
		}
	}
	if (st->scancxt != NULL)
		MemoryContextReset(st->scancxt);
	st->visited = NULL;
	st->visitedbytes = 0;
	st->novisit = false;
	st->distinct = 0;
	st->scanwalked = 0;
	st->noswitch = false;
	st->sorting = false;
	st->sortcxt = NULL;
	st->srt = NULL;
	st->nsrt = 0;
	st->srtpos = 0;
}

static inline bool
lo_visited(LionOrderedState *st, int idx, uint16 lo)
{
	return st->visited != NULL && st->visited[idx] != NULL &&
		(st->visited[idx][lo >> 6] & (UINT64CONST(1) << (lo & 63))) != 0;
}

/*
 * Note that this scan's walk met member tid (in container idx), and say
 * whether it is the first time (DESIGN.md §30.4, "Stopping early").  One
 * bitmap per container key the walk touches, within hash_mem; past that the
 * scan stops tracking - and with it no longer stops early or switches - and
 * every meeting counts as a first, which is harmless: a TID met twice is a
 * recycled slot, never visible to the snapshot.
 */
static bool
lo_mark(LionOrderedState *st, int idx, ItemPointer tid)
{
	uint16		lo = lion_code_lo(lion_tid_to_code(tid));

	if (st->novisit)
		return true;
	if (st->visited == NULL)
		st->visited = (uint64 **)
			MemoryContextAllocZero(st->scancxt, sizeof(uint64 *) * st->set->n);
	if (st->visited[idx] == NULL)
	{
		if (st->visitedbytes + LION_BITSET_BYTES > get_hash_memory_limit())
		{
			st->novisit = true;
			return true;
		}
		st->visited[idx] = (uint64 *)
			MemoryContextAllocZero(st->scancxt, LION_BITSET_BYTES);
		st->visitedbytes += LION_BITSET_BYTES;
	}
	if (lo_visited(st, idx, lo))
		return false;
	st->visited[idx][lo >> 6] |= UINT64CONST(1) << (lo & 63);
	return true;
}

static int
lo_cmp_rows(const void *a, const void *b, void *arg)
{
	const LoSortRow *ra = (const LoSortRow *) a;
	const LoSortRow *rb = (const LoSortRow *) b;
	LionOrderedState *st = (LionOrderedState *) arg;
	int			i;

	for (i = 0; i < st->nsort; i++)
	{
		int			c = ApplySortComparator(ra->vals[i], ra->nulls[i],
											rb->vals[i], rb->nulls[i],
											&st->sortkeys[i]);

		if (c != 0)
			return c;
	}
	return 0;
}

/*
 * Is the fetch-and-sort switch due (DESIGN.md §30.4, "When the walk is not
 * paying")?  This scan's walk has met at least LO_SWITCH_MIN_WALK entries and
 * switchratio of them for every member it has not met - and, in index-only
 * mode (§40.4), markedly fewer members than a uniform spread of them over
 * the entries the planner expects it to visit would have given it by now:
 * the walk of a uniform filter under no LIMIT then never switches, which
 * would fetch every member left from a heap the walk never touches, while
 * the correlated hazard of §30.3 - members at the far end of the order,
 * `distinct` near zero as the entries go by - still switches at the measured
 * ratio.  `members` is the set's count, or the lazy set's estimate of it.
 */
static bool
lo_switch_due(LionOrderedState *st, double members)
{
	if (st->nsort == 0 || st->noswitch || st->scanwalked < LO_SWITCH_MIN_WALK)
		return false;
	if ((double) st->scanwalked <
		st->switchratio * (members - (double) st->distinct))
		return false;
	if (st->indexonly && st->expectwalk > 0 &&
		(double) st->distinct >= LO_SWITCH_DENSITY * members *
		((double) st->scanwalked / st->expectwalk))
		return false;
	return true;
}

/*
 * The fetch-and-sort switch (DESIGN.md §30.4, "When the walk is not paying"):
 * fetch every member this scan's walk has not met, keep the visible ones that
 * pass the lion recheck (for an inexact set) and the ordered index's own
 * clauses (they did not come through the index), and sort them by the
 * pathkeys.  Every row the walk has not returned yet is among them - a
 * visible member's index entry lies where the walk has not been - and none
 * it has returned is.  Gives up, and lets the walk go on, if the rows do not
 * fit in work_mem.
 */
static bool
lo_switch(LionOrderedState *st)
{
	Relation	heap = st->css.ss.ss_currentRelation;
	TupleDesc	desc = RelationGetDescr(heap);
	Snapshot	snapshot = st->css.ss.ps.state->es_snapshot;
	TupleTableSlot *slot = st->css.ss.ss_ScanTupleSlot;
	/* the heap tuple lands in the table AM's slot, then in the scan slot */
	TupleTableSlot *fslot = st->indexonly ? st->tabslot : slot;
	ExprContext *econtext = st->css.ss.ps.ps_ExprContext;
	Size		budget = (Size) work_mem * 1024;
	Size		used = 0;
	int			cap = 64;
	uint16	   *los;
	MemoryContext oldcxt;
	int			i;

	st->sortcxt = AllocSetContextCreate(st->scancxt, "LionOrdered sort",
										ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(st->sortcxt);
	los = (uint16 *) palloc(sizeof(uint16) * LION_CONTAINER_RANGE);
	st->srt = (LoSortRow *) palloc(sizeof(LoSortRow) * cap);
	st->nsrt = 0;

	for (i = 0; i < st->set->n; i++)
	{
		uint32		n = lion_container_to_array(st->set->conts[i], los);
		uint32		j;

		for (j = 0; j < n; j++)
		{
			ItemPointerData tid;
			bool		all_dead = false;
			LoSortRow  *row;
			int			k;

			if (lo_visited(st, i, los[j]))
				continue;
			CHECK_FOR_INTERRUPTS();
			lion_code_to_tid(lion_make_code(st->set->keys[i], los[j]), &tid);
			st->sortfetched++;
			/* a heap visit counts as the walk counts them in that mode (§40.6) */
			if (st->indexonly)
				st->fetched++;
			if (!lion_table_fetch_tid(heap, &tid, snapshot, &all_dead) ||
				!table_tuple_fetch_row_version(heap, &tid, snapshot, fslot))
				continue;
			if (!st->indexonly)
				st->fetched++;
			if (fslot != slot)
			{
				/* the scan slot's type is fixed per plan (§40.4) */
				ExecCopySlot(slot, fslot);
				ExecClearTuple(fslot);
			}
			ResetExprContext(econtext);
			econtext->ecxt_scantuple = slot;
			if (!st->exact && !ExecQual(st->lionrecheck, econtext))
			{
				st->removed++;
				continue;
			}
			if (!ExecQual(st->ordrecheck, econtext))
				continue;

			if (st->nsrt == cap)
			{
				cap *= 2;
				st->srt = (LoSortRow *) repalloc(st->srt, sizeof(LoSortRow) * cap);
			}
			row = &st->srt[st->nsrt++];
			row->tup = ExecCopySlotHeapTuple(slot);
			/* a virtual slot's copy has no TID of its own: the row's (§40.4) */
			row->tup->t_self = tid;
			row->vals = (Datum *) palloc(sizeof(Datum) * st->nsort);
			row->nulls = (bool *) palloc(sizeof(bool) * st->nsort);
			for (k = 0; k < st->nsort; k++)
				row->vals[k] = heap_getattr(row->tup, st->sortattnos[k], desc,
											&row->nulls[k]);
			used += HEAPTUPLESIZE + row->tup->t_len +
				st->nsort * (sizeof(Datum) + sizeof(bool)) + sizeof(LoSortRow);
			if (used > budget)
			{
				/* too big to sort here: the walk goes on (§30.4) */
				MemoryContextSwitchTo(oldcxt);
				MemoryContextDelete(st->sortcxt);
				st->sortcxt = NULL;
				st->srt = NULL;
				st->nsrt = 0;
				st->noswitch = true;
				ExecClearTuple(slot);
				return false;
			}
		}
	}
	MemoryContextSwitchTo(oldcxt);
	ExecClearTuple(slot);

	qsort_arg(st->srt, st->nsrt, sizeof(LoSortRow), lo_cmp_rows, st);
	st->sorting = true;
	st->srtpos = 0;
	st->switches++;
	return true;
}

static TupleTableSlot *
lo_sort_next(LionOrderedState *st, TupleTableSlot *slot)
{
	HeapTuple	tup;

	if (st->srtpos >= st->nsrt)
	{
		st->done = true;
		return ExecClearTuple(slot);
	}
	tup = st->srt[st->srtpos++].tup;
	ExecForceStoreHeapTuple(tup, slot, false);
	slot->tts_tid = tup->t_self;
	slot->tts_tableOid = RelationGetRelid(st->css.ss.ss_currentRelation);
	return slot;
}

/* ExecScan's access method: the next member with a visible version. */
static TupleTableSlot *
lo_next(ScanState *ss)
{
	LionOrderedState *st = (LionOrderedState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;
	ExprContext *econtext = ss->ps.ps_ExprContext;

	/*
	 * The set: lazily, evaluated only where the walk goes, when its keys
	 * can be sought (§30.4, "The set, lazily"); built otherwise.
	 */
	if (st->set == NULL && !st->noset && !st->lazy &&
		!(st->lazyok && lo_lazy_begin(st)))
		lo_build_set(st);
	if (!st->started)
		lo_start_walk(st);

	/* an empty set selects nothing, and needs no walk */
	if (st->done ||
		(!st->noset && (st->lazy ? st->lazyempty : st->set->n == 0)))
		return ExecClearTuple(slot);
	if (st->sorting)
		return lo_sort_next(st, slot);

	for (;;)
	{
		ItemPointer tid;
		int			idx;
		bool		counted;
		bool		ordrecheck;
		bool		keep;
		bool		fromheap = true;

		CHECK_FOR_INTERRUPTS();

		/*
		 * The walk does not go in heap order, or the lazy set's probes have
		 * cost what building it would, or the walk has gone as far as the
		 * switch below would let it go were the set as big as its entries'
		 * counts say - or far enough anyway that the early stop and the
		 * switch should have the set's size to go by: build it (§30.4, "The
		 * set, lazily").  Whether the switch comes is decided on the set
		 * built, so a count that is off moves only when the set is built.
		 */
		if (st->lazy &&
			((st->lazyevals >= LO_LAZY_ORDER_KEYS &&
			  st->lazyback * LO_LAZY_BACK_SHARE >= st->lazyevals) ||
			 st->lazywork >= st->lazybudget ||
			 lo_switch_due(st, st->lazymembers) ||
			 st->scanwalked >= LO_LAZY_MAX_WALK ||
			 st->memobytes > st->limit / 2))
			lo_lazy_convert(st);
		counted = !st->noset && !st->lazy && !st->degraded && !st->novisit;

		/*
		 * Every member met: nothing further along can be one (§30.4,
		 * "Stopping early").  A member counts once however often the walk
		 * meets its TID; not knowable once the set has degraded.
		 */
		if (counted && st->distinct >= st->members)
			break;

		/*
		 * The walk has cost what fetching the members it has not met would:
		 * fetch and sort those instead (§30.4, "When the walk is not
		 * paying").
		 */
		if (counted && lo_switch_due(st, (double) st->members) &&
			lo_switch(st))
			return lo_sort_next(st, slot);

		tid = lo_next_tid(st);
		if (tid == NULL)
			break;
		st->walked++;
		st->scanwalked++;
		if (st->lazy)
		{
			LoMemoEnt  *e = lo_lazy_find(st, tid);

			if (e == NULL)
				continue;
			if (!lo_lazy_mark(st, e, tid))
				continue;		/* met before: a recycled slot (§30.4) */
			st->distinct++;
			st->hits++;
		}
		else if (!st->noset)
		{
			idx = lo_set_find(st->set, tid);
			if (idx < 0)
				continue;
			if (!lo_mark(st, idx, tid))
				continue;		/* met before: a recycled slot (§30.4) */
			st->distinct++;
			st->hits++;
		}
		if (st->indexonly)
		{
			/* the index tuple's values; the heap for visibility (§40.4) */
			if (!lo_fetch_indexonly(st, slot, &fromheap))
				continue;
		}
		else
		{
			if (!lo_fetch(st, slot))
				continue;
			st->fetched++;
		}

		/*
		 * A lion column's walk is exact: its entries are tested against the
		 * range with the column's own comparison (§28).  Without a set there
		 * is nothing to recheck either: the clauses are the filter.  In
		 * index-only mode the heap tuple fetched for the row, if any, is let
		 * go once the rechecks are done with it.
		 */
		ordrecheck = !st->lionwalk && st->scan->xs_recheck;
		keep = true;
		if (!st->noset && !st->exact)
			keep = lo_lion_recheck(st, slot, &fromheap);
		if (keep && ordrecheck)
		{
			ResetExprContext(econtext);
			econtext->ecxt_scantuple = slot;
			keep = ExecQual(st->ordrecheck, econtext);
		}
		if (st->indexonly && fromheap)
			ExecClearTuple(st->tabslot);
		if (!keep)
			continue;

		/*
		 * A filter on the walked column (DESIGN.md §30.11): the rows of one
		 * entry hold one value, byte for byte, so the first visible one
		 * decides for the entry - evaluated here on a visible row, as the
		 * filter above the node would, and one that fails takes the rest of
		 * the entry with it, unread.
		 */
		if (st->valuequal != NULL && st->lionwalk && st->owalk != NULL &&
			lion_order_walk_entryno(st->owalk) != st->valueentry)
		{
			st->valueentry = lion_order_walk_entryno(st->owalk);
			ResetExprContext(econtext);
			econtext->ecxt_scantuple = slot;
			if (!ExecQual(st->valuequal, econtext))
			{
				lion_order_walk_skip_entry(st->owalk);
				st->valueskips++;
				continue;
			}
		}

		/*
		 * A row returned without a heap visit has no tuple to predicate-lock
		 * (its xmin was never read): its page is locked, as core's Index Only
		 * Scan locks it (DESIGN.md §40.5).
		 */
		if (!fromheap)
			PredicateLockPage(ss->ss_currentRelation,
							  ItemPointerGetBlockNumber(&st->scan->xs_heaptid),
							  ss->ps.state->es_snapshot);
		return slot;
	}
	st->done = true;
	return ExecClearTuple(slot);
}

/* ExecScan's recheck method (EvalPlanQual): the quals the scan answered. */
static bool
lo_recheck(ScanState *ss, TupleTableSlot *slot)
{
	LionOrderedState *st = (LionOrderedState *) ss;
	ExprContext *econtext = ss->ps.ps_ExprContext;

	/*
	 * The quals were initialised on the scan slot's type.  A row mark's ctid
	 * is a system column no index returns, so a locked relation never gets an
	 * index-only plan (§40.1) and the test tuple EvalPlanQual hands in is
	 * always a heap-mode one; the recheck from the heap of lo_lion_recheck()
	 * is index-only mode's alone and never meets this path.
	 */
	Assert(slot->tts_ops == ss->ss_ScanTupleSlot->tts_ops);
	ResetExprContext(econtext);
	econtext->ecxt_scantuple = slot;
	return ExecQual(st->lionrecheck, econtext) &&
		ExecQual(st->ordrecheck, econtext);
}

static TupleTableSlot *
lo_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, (ExecScanAccessMtd) lo_next,
					(ExecScanRecheckMtd) lo_recheck);
}

static void
lo_rescan(CustomScanState *node)
{
	LionOrderedState *st = (LionOrderedState *) node;

	/* the walk restarts, with its keys evaluated again */
	st->started = false;
	st->done = false;
	lo_scan_reset(st);
	if (BufferIsValid(st->vmbuffer))
	{
		ReleaseBuffer(st->vmbuffer);
		st->vmbuffer = InvalidBuffer;
	}

	/*
	 * The set is rebuilt only when a Param of the lion quals changed; the
	 * snapshot is the same for the whole execution (DESIGN.md §30.4).
	 */
	if (node->ss.ps.chgParam != NULL &&
		bms_overlap(node->ss.ps.chgParam, st->lionparams))
	{
		st->set = NULL;
		if (st->lazy)
			lo_lazy_end(st);
	}

	ExecScanReScan(&node->ss);
}

static void
lo_end(CustomScanState *node)
{
	LionOrderedState *st = (LionOrderedState *) node;
	int			i;

	if (st->scan != NULL)
		index_endscan(st->scan);
	st->scan = NULL;
	if (BufferIsValid(st->vmbuffer))
		ReleaseBuffer(st->vmbuffer);
	st->vmbuffer = InvalidBuffer;
	if (st->tabslot != NULL)
		ExecClearTuple(st->tabslot);
	if (st->owalk != NULL)
		lo_walk_end(st);
#if PG_VERSION_NUM < 200000
	if (st->fetch != NULL)
		table_index_fetch_end(st->fetch);
	st->fetch = NULL;
#endif
	if (st->walkcxt != NULL)
		MemoryContextDelete(st->walkcxt);
	st->walkcxt = NULL;
	if (st->ordidx != NULL)
		index_close(st->ordidx, AccessShareLock);
	lo_lazy_end(st);
	for (i = 0; i < st->nleaves; i++)
	{
		if (st->leaves[i].index != NULL)
			index_close(st->leaves[i].index, AccessShareLock);
	}
	if (st->lazycxt != NULL)
		MemoryContextDelete(st->lazycxt);
	if (st->probecxt != NULL)
		MemoryContextDelete(st->probecxt);
	st->lazycxt = NULL;
	st->probecxt = NULL;
	if (st->setcxt != NULL)
		MemoryContextDelete(st->setcxt);
	if (st->buildcxt != NULL)
		MemoryContextDelete(st->buildcxt);
	if (st->scancxt != NULL)
		MemoryContextDelete(st->scancxt);
	st->scancxt = NULL;
	st->setcxt = NULL;
	st->buildcxt = NULL;
	st->set = NULL;
}

/* ---------------------------------------------------------------------
 * EXPLAIN (DESIGN.md §30.7)
 * --------------------------------------------------------------------- */

static void
lo_explain_qual(CustomScanState *node, List *qual, const char *label,
				List *ancestors, ExplainState *es)
{
	List	   *context;
	char	   *str;

	if (qual == NIL)
		return;
	context = set_deparse_context_plan(es->deparse_cxt, node->ss.ps.plan,
									   ancestors);
	str = deparse_expression((Node *) make_ands_explicit(qual), context,
							 lion_explain_useprefix(es), false);
	ExplainPropertyText(label, str, es);
}

static void
lo_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	LionOrderedState *st = (LionOrderedState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	StringInfoData buf;
	int			i;

	initStringInfo(&buf);
	appendStringInfoString(&buf, get_rel_name(st->ordoid));
	if (st->lionwalk)
	{
		/* the column walked, as the index names it (§30.11) */
		appendStringInfo(&buf, " (%s%s%s)",
						 quote_identifier(get_attname(st->ordoid,
													  st->walkattno, false)),
						 ScanDirectionIsBackward(st->dir) ? ", backward" : "",
						 st->walknulls == LION_ORDER_NULLS_FIRST ?
						 ", nulls first" :
						 st->walknulls == LION_ORDER_NULLS_LAST ?
						 ", nulls last" : "");
	}
	else if (ScanDirectionIsBackward(st->dir) || st->indexonly)
		appendStringInfo(&buf, " (%s%s%s)",
						 ScanDirectionIsBackward(st->dir) ? "backward" : "",
						 ScanDirectionIsBackward(st->dir) && st->indexonly ?
						 ", " : "",
						 st->indexonly ? "index only" : "");
	/* a LionBtreeScan claims no order: the index it walks (§40.6) */
	ExplainPropertyText(cscan->methods == &lo_btree_scan_methods ?
						"Index" : "Ordered By", buf.data, es);

	lo_explain_qual(node, (List *) list_nth(cscan->custom_exprs, LO_EXPR_ORDORIG),
					"Index Cond", ancestors, es);
	lo_explain_qual(node, (List *) list_nth(cscan->custom_exprs, LO_EXPR_LIONQUAL),
					"Lion Cond", ancestors, es);
	lo_explain_qual(node, (List *) list_nth(cscan->custom_exprs,
											LO_EXPR_VALUEQUAL),
					"Filter per Value", ancestors, es);

	resetStringInfo(&buf);
	for (i = 0; i < st->nleaves; i++)
	{
		const char *name = get_rel_name(st->leaves[i].indexoid);
		int			j;

		/* each index once, in the order the tree first names it */
		for (j = 0; j < i; j++)
			if (st->leaves[j].indexoid == st->leaves[i].indexoid)
				break;
		if (j < i)
			continue;
		if (buf.len > 0)
			appendStringInfoString(&buf, ", ");
		appendStringInfoString(&buf, name);
	}
	/* a lion column's walk without a set reads no other lion index */
	if (buf.len > 0)
		ExplainPropertyText("Lion Indexes", buf.data, es);

	if (es->analyze)
	{
		ExplainPropertyInteger("Index Entries Walked", NULL,
							   (int64) st->walked, es);
		if (st->lionwalk)
		{
			int64		entries = 0;
			int64		leaves;

			if (st->owalk != NULL)
				lion_order_walk_counts(st->owalk, &entries, &leaves);
			ExplainPropertyInteger("Lion Keys Walked", NULL,
								   (int64) st->walkkeys + entries, es);
			if (st->valuequal != NULL)
				ExplainPropertyInteger("Lion Keys Filtered", NULL,
									   (int64) st->valueskips, es);
		}
		if (!st->noset)
			ExplainPropertyInteger("Lion Set Hits", NULL, (int64) st->hits,
								   es);
		ExplainPropertyInteger("Heap Fetches", NULL, (int64) st->fetched, es);
		/* a node that never made its set has no exactness to report */
		if (st->removed > 0 ||
			((st->builds > 0 || st->lazykeys > 0) && !st->exact))
			ExplainPropertyInteger("Rows Removed by Lion Recheck", NULL,
								   (int64) st->removed, es);
		if (st->builds > 0)
		{
			resetStringInfo(&buf);
			appendStringInfo(&buf, "%d containers, %s", st->ncont,
							 st->degraded ? "degraded" :
							 st->exact ? "exact" : "rechecked");
			if (st->builds > 1)
				appendStringInfo(&buf, ", %llu builds",
								 (unsigned long long) st->builds);
			if (st->lazykeys > 0)
				appendStringInfo(&buf, ", after %llu keys probed",
								 (unsigned long long) st->lazykeys);
			ExplainPropertyText("Lion Set", buf.data, es);
		}
		else if (st->lazykeys > 0)
		{
			/* the set was never built: only the keys the walk met (§30.4) */
			resetStringInfo(&buf);
			appendStringInfo(&buf, "lazy, %llu keys probed, %s",
							 (unsigned long long) st->lazykeys,
							 st->exact ? "exact" : "rechecked");
			ExplainPropertyText("Lion Set", buf.data, es);
		}
		if (st->switches > 0)
		{
			resetStringInfo(&buf);
			appendStringInfo(&buf, "%llu of %llu scans, %llu members fetched",
							 (unsigned long long) st->switches,
							 (unsigned long long) st->scans,
							 (unsigned long long) st->sortfetched);
			ExplainPropertyText("Switched to Fetch and Sort", buf.data, es);
		}
	}
	pfree(buf.data);
}

/* ---------------------------------------------------------------------
 * Initialisation, from _PG_init (lion_am.c)
 * --------------------------------------------------------------------- */

void
lion_ordered_init(void)
{
	DefineCustomBoolVariable("pg_lion.enable_ordered_scan",
							 "Answer ORDER BY over an ordered index with a lion-filtered walk of it.",
							 NULL,
							 &lion_enable_ordered_scan,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	/*
	 * DESIGN.md §30.4, "The set, lazily": off, LionOrdered builds its set
	 * before the walk, as it did before - for comparing the two, and for the
	 * tests of the build that pin it.
	 */
	DefineCustomBoolVariable("pg_lion.enable_lazy_set",
							 "Lets LionOrdered evaluate its lion set only at the container keys its walk meets.",
							 "Off, the set is built for the whole table before the walk starts.",
							 &lion_enable_lazy_set,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	/*
	 * DESIGN.md §40: the walks of a covering btree offered without an ORDER
	 * BY.  pg_lion.enable_ordered_scan keeps gating the ordered ones.
	 */
	DefineCustomBoolVariable("pg_lion.enable_btree_scan",
							 "Offer LionBtreeScan: a lion-filtered walk of a covering B-tree that needs no ORDER BY.",
							 "The rows' values come from the index tuples; the heap is read only for pages not all-visible.",
							 &lion_enable_btree_scan,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	/*
	 * DESIGN.md §30.4: what a member's heap fetch is worth in index entries
	 * walked, the fetch-and-sort switch's term.
	 */
	DefineCustomRealVariable("pg_lion.ordered_switch_ratio",
							 "Sets the index entries LionOrdered walks per member not yet met before it fetches and sorts the rest.",
							 "The measured price of a member's heap fetch in index entries walked; a large value keeps the walk from switching.",
							 &lion_ordered_switch_ratio,
							 LO_SWITCH_RATIO,
							 1.0, 1e6,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	RegisterCustomScanMethods(&lo_scan_methods);
	RegisterCustomScanMethods(&lo_btree_scan_methods);

	lion_prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = lion_ordered_set_rel_pathlist;
}
