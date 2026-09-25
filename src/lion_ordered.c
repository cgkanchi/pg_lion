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
#include "access/relscan.h"
#include "access/table.h"
#include "access/tableam.h"
#if PG_VERSION_NUM >= 200000
#include "access/tableam_indexscan.h"
#endif
#include "access/stratnum.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "commands/explain.h"
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
#include "storage/predicate.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/sortsupport.h"
#include "utils/spccache.h"

#include "lion.h"
#include "lion_count.h"

/* GUC, and the hook this file chains (both installed by lion_ordered_init). */
bool		lion_enable_ordered_scan = true;
static set_rel_pathlist_hook_type lion_prev_set_rel_pathlist_hook = NULL;

/*
 * custom_private of the plan is POSITIONAL, behind a shape marker (as §10's):
 *
 *	LO_PRIV_SHAPE	IntList (LO_PRIV_MAGIC, LO_PRIV_NMEMBERS)
 *	LO_PRIV_ORD		OidList (the ordered index)
 *	LO_PRIV_INTS	IntList (scan direction, flags)
 *	LO_PRIV_ORDCOLS	IntList: the index column (0-based) of each ordered
 *					index qual, in custom_exprs' order
 *	LO_PRIV_TREE	List of IntList, the lion tree in preorder: an inner node
 *					is (LO_NODE_AND or LO_NODE_OR, number of children), a leaf
 *					(LO_NODE_LEAF, number of quals, their index columns ...)
 *	LO_PRIV_LEAVES	OidList: each leaf's lion index, in preorder
 *	LO_PRIV_SORT	the path's pathkeys as sort keys over the relation's own
 *					columns, for the fetch-and-sort switch (§30.4): a List of
 *					IntList (attnos), OidList (sort operators), OidList
 *					(collations), IntList (nulls first); NIL when a pathkey
 *					is not a plain column, and then the walk never switches
 *
 * and custom_exprs holds four lists:
 *
 *	LO_EXPR_LIONQUALS	every leaf's index quals, key on the left, in leaf
 *						order (heap Vars; the executor substitutes INDEX_VAR)
 *	LO_EXPR_ORDQUALS	the ordered index's index quals, likewise
 *	LO_EXPR_LIONQUAL	the lion side's ORIGINAL qual (implicit AND), the
 *						recheck of an inexact set
 *	LO_EXPR_ORDORIG		the ordered index's original clauses (implicit AND),
 *						the recheck when the index sets xs_recheck
 */
#define LO_PRIV_MAGIC		0x4c4f5244	/* "LORD" */
#define LO_PRIV_SHAPE		0
#define LO_PRIV_ORD			1
#define LO_PRIV_INTS		2
#define LO_PRIV_ORDCOLS		3
#define LO_PRIV_TREE		4
#define LO_PRIV_LEAVES		5
#define LO_PRIV_SORT		6
#define LO_PRIV_NMEMBERS	7

/*
 * The fetch-and-sort switch (DESIGN.md §30.4, "When the walk is not paying"):
 * once the walk of this scan has met LO_SWITCH_RATIO index entries per member
 * it has not met yet - and at least LO_SWITCH_MIN_WALK entries - the members
 * left are fetched and sorted instead.  The ratio is what one heap fetch of a
 * member costs in index entries walked, measured (§30.10: 0.04 us an entry,
 * 1-2 us a fetch, warm).
 */
#define LO_SWITCH_RATIO		32
#define LO_SWITCH_MIN_WALK	10000

#define LO_EXPR_LIONQUALS	0
#define LO_EXPR_ORDQUALS	1
#define LO_EXPR_LIONQUAL	2
#define LO_EXPR_ORDORIG		3

#define LO_NODE_LEAF		0
#define LO_NODE_AND			1
#define LO_NODE_OR			2

#define LO_FLAG_LOSSY		0x0001	/* a lion index clause was lossy */

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
	uint32	   *keys;
	LionContainer **conts;
	bool		sorted;			/* keys strictly ascending */
} LionTidSet;

/* Bytes of directory charged per container key, on top of the container. */
#define LO_ENTRY_BYTES		((Size) (sizeof(uint32) + sizeof(LionContainer *) + 4))

/* ---------------------------------------------------------------------
 * Executor state
 * --------------------------------------------------------------------- */

typedef struct LoLeaf
{
	Oid			indexoid;
	Relation	index;
	List	   *quals;			/* fixed: INDEX_VAR on the left */
	ScanKey		keys;
	int			nkeys;
	IndexRuntimeKeyInfo *rtkeys;
	int			nrtkeys;
} LoLeaf;

typedef struct LoNode
{
	int			kind;
	int			nchild;
	struct LoNode **child;
	LoLeaf	   *leaf;
} LoNode;

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

	/* the lion side */
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

/*
 * Which restriction clauses the node must still evaluate on a fetched row
 * (DESIGN.md §30.2, step 4): not the ordered index's non-lossy index clauses,
 * not the lion leaves' index clauses, not what the lion qual implies.
 */
static List *
lo_residual(List *rinfos, IndexPath *ord, List *lionrinfos, List *lionqual)
{
	List	   *ordrinfos = NIL;
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, ord->indexclauses)
	{
		IndexClause *iclause = lfirst_node(IndexClause, lc);

		if (!iclause->lossy)
			ordrinfos = lappend(ordrinfos, iclause->rinfo);
	}

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
 * The price of one (ordered path, lion access) pair (DESIGN.md §30.3), and the
 * size its set is expected to have.
 */
static void
lo_cost(PlannerInfo *root, RelOptInfo *rel, IndexPath *ord, Path *lion,
		List *residual, Cost *startup_p, Cost *total_p, double *setbytes)
{
	Cost		lioncost;
	Selectivity sel;
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

	/* the walk: the ordered index's own cost, and one test per entry */
	walked = clamp_row_est(ord->indexselectivity * tuples);
	run = ord->indextotalcost + walked * cpu_operator_cost;

	/* the members' heap fetches, priced as cost_index() prices them */
	fetched = clamp_row_est(walked * sel);
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
								 (double) ord->indexinfo->pages, root) *
		spc_random;
	pages_corr = ceil(Min(fetched, walked / tuples * pages));
	min_io = (pages_corr > 0) ? spc_random + (pages_corr - 1) * spc_seq : 0;
	run += max_io + corr * corr * (min_io - max_io);

	cost_qual_eval(&qcost, residual, root);
	startup += qcost.startup + rel->reltarget->cost.startup;
	run += fetched * (cpu_tuple_cost + qcost.per_tuple) +
		rel->rows * rel->reltarget->cost.per_tuple;

	*startup_p = startup;
	*total_p = startup + run;
}

/* Is rel one the node may scan at all (DESIGN.md §30.1)? */
static bool
lo_rel_ok(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	ListCell   *lc;

	if (rel->reloptkind != RELOPT_BASEREL || rte->rtekind != RTE_RELATION)
		return false;
	if (rte->inh || rte->tablesample != NULL)
		return false;
	if (rte->relkind != RELKIND_RELATION && rte->relkind != RELKIND_MATVIEW)
		return false;
	if (rte->securityQuals != NIL)
		return false;

	/*
	 * Not the target of an UPDATE, DELETE or MERGE (reached through a merge
	 * join's ordered input): v1 leaves those scans to core (DESIGN.md §30.8).
	 */
	if (root->parse->resultRelation == (int) rel->relid)
		return false;
	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		if (rinfo->security_level > 0)
			return false;
	}
	if (rel->indexlist == NIL || rel->baserestrictinfo == NIL)
		return false;
	return true;
}

static void
lion_ordered_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
							  RangeTblEntry *rte)
{
	Oid			lionam;
	List	   *ordpaths = NIL;
	List	   *lionidx = NIL;
	List	   *cands = NIL;
	RelOptInfo *scratch;
	ListCell   *lc;
	ListCell   *lc2;
	Size		limit;

	if (lion_prev_set_rel_pathlist_hook != NULL)
		lion_prev_set_rel_pathlist_hook(root, rel, rti, rte);

	if (!lion_enable_ordered_scan)
		return;
	if (!lo_rel_ok(root, rel, rte))
		return;

	lionam = lion_get_am_oid();
	if (!OidIsValid(lionam))
		return;

	/* the ordered side: core's ordered index paths */
	foreach(lc, rel->pathlist)
	{
		Path	   *p = (Path *) lfirst(lc);
		IndexPath  *ipath;

		if (!IsA(p, IndexPath) || p->pathkeys == NIL || p->param_info != NULL)
			continue;
		ipath = (IndexPath *) p;
		/*
		 * A btree: the early stop and the fetch-and-sort switch rest on its
		 * returning each heap TID once per scan, in the order its pathkeys
		 * claim (DESIGN.md §30.4).
		 */
		if (ipath->indexorderbys != NIL || ipath->indexinfo->relam != BTREE_AM_OID ||
			ipath->indexinfo->hypothetical)
			continue;
		if (!lo_indexpath_ok(ipath))
			continue;
		ordpaths = lappend(ordpaths, ipath);
	}
	if (ordpaths == NIL)
		return;

	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);

		if (idx->relam == lionam && !idx->hypothetical)
			lionidx = lappend(lionidx, idx);
	}
	if (lionidx == NIL)
		return;

	/* the heap table AM only (§2); ambuild refused every other */
	{
		Relation	relation = table_open(rte->relid, NoLock);
		bool		supported = lion_table_am_supported(relation);

		table_close(relation, NoLock);
		if (!supported)
			return;
	}

	/*
	 * The lion side, as core would build it: create_index_paths() on a copy
	 * of the rel that sees only its lion indexes and no join clauses, and
	 * builds no partial paths, so that only unparameterized, non-partial
	 * paths land - in the copy's own path list.
	 */
	scratch = makeNode(RelOptInfo);
	memcpy(scratch, rel, sizeof(RelOptInfo));
	scratch->indexlist = lionidx;
	scratch->pathlist = NIL;
	scratch->ppilist = NIL;
	scratch->partial_pathlist = NIL;
	scratch->cheapest_startup_path = NULL;
	scratch->cheapest_total_path = NULL;
	scratch->cheapest_parameterized_paths = NIL;
	scratch->joininfo = NIL;
	scratch->has_eclass_joins = false;
	scratch->consider_parallel = false;
	create_index_paths(root, scratch);

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
	if (cands == NIL)
		return;

	limit = get_hash_memory_limit();

	foreach(lc, ordpaths)
	{
		IndexPath  *ord = (IndexPath *) lfirst(lc);

		foreach(lc2, cands)
		{
			Path	   *lion = (Path *) lfirst(lc2);
			List	   *lionrinfos = NIL;
			bool		lossy = false;
			List	   *lionqual = lo_lion_qual(lion, &lionrinfos, &lossy);
			List	   *residual = lo_residual(rel->baserestrictinfo, ord,
											   lionrinfos, lionqual);
			CustomPath *cp;
			Cost		startup;
			Cost		total;
			double		setbytes;

			lo_cost(root, rel, ord, lion, residual, &startup, &total,
					&setbytes);
			if (setbytes > (double) limit)
				continue;		/* the set would not fit (§30.3) */

			cp = makeNode(CustomPath);
			cp->path.pathtype = T_CustomScan;
			cp->path.parent = rel;
			cp->path.pathtarget = rel->reltarget;
			cp->path.param_info = NULL;
			cp->path.parallel_aware = false;
			cp->path.parallel_safe = false;
			cp->path.parallel_workers = 0;
			cp->path.rows = rel->rows;
			cp->path.startup_cost = startup;
			cp->path.total_cost = total;
			cp->path.pathkeys = ord->path.pathkeys;
			cp->flags = CUSTOMPATH_SUPPORT_PROJECTION;
			cp->custom_paths = NIL;
			cp->custom_private = list_make2(ord, lion);
			cp->methods = &lo_path_methods;
			add_path(rel, &cp->path);
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
		ListCell   *lc2;

		foreach(lc2, ec->ec_members)
		{
			EquivalenceMember *em = (EquivalenceMember *) lfirst(lc2);
			Node	   *e = (Node *) em->em_expr;

			if (em->em_is_const || em->em_is_child ||
				!bms_equal(em->em_relids, rel->relids))
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
	IndexPath  *ord = (IndexPath *) linitial(best_path->custom_private);
	Path	   *lion = (Path *) lsecond(best_path->custom_private);
	CustomScan *cscan = makeNode(CustomScan);
	List	   *lionrinfos = NIL;
	bool		lossy = false;
	List	   *lionqual;
	List	   *residual;
	List	   *tree = NIL;
	List	   *leaves = NIL;
	List	   *lionquals = NIL;
	List	   *ordquals = NIL;
	List	   *ordcols = NIL;
	List	   *ordorig = NIL;
	ListCell   *lc;

	lionqual = lo_lion_qual(lion, &lionrinfos, &lossy);
	residual = lo_residual(clauses, ord, lionrinfos, lionqual);
	lo_lion_tree(lion, &tree, &leaves, &lionquals);

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

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = extract_actual_clauses(residual, false);
	cscan->scan.scanrelid = rel->relid;
	cscan->flags = best_path->flags;
	cscan->custom_plans = NIL;
	cscan->custom_scan_tlist = NIL;
	cscan->custom_exprs = list_make4(lionquals, ordquals, lionqual, ordorig);
	cscan->custom_private =
		list_make5(list_make2_int(LO_PRIV_MAGIC, LO_PRIV_NMEMBERS),
				   list_make1_oid(ord->indexinfo->indexoid),
				   list_make2_int((int) ord->indexscandir,
								  lossy ? LO_FLAG_LOSSY : 0),
				   ordcols,
				   tree);
	cscan->custom_private = lappend(cscan->custom_private, leaves);
	cscan->custom_private = lappend(cscan->custom_private,
									lo_sort_keys(rel, best_path->path.pathkeys));
	cscan->methods = &lo_scan_methods;

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

/* Append (ckey, a copy of c, or nothing when degraded). */
static void
lo_set_push(LionOrderedState *st, LionTidSet *set, uint32 ckey,
			const LionContainer *c)
{
	if (set->n > 0 && ckey <= set->keys[set->n - 1])
		set->sorted = false;
	if (set->n == set->cap)
	{
		set->cap *= 2;
		set->keys = (uint32 *) repalloc(set->keys, sizeof(uint32) * set->cap);
		set->conts = (LionContainer **)
			repalloc(set->conts, sizeof(LionContainer *) * set->cap);
	}
	st->bytes += LO_ENTRY_BYTES;
	set->keys[set->n] = ckey;
	set->conts[set->n] = NULL;
	if (c != NULL && !st->degraded)
	{
		Size		size = lion_container_size(c);

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
 * by key and OR the containers of equal keys into one.
 */
static void
lo_set_finish(LionOrderedState *st, LionTidSet *set)
{
	int		   *order;
	uint32	   *keys;
	LionContainer **conts;
	LionContainer *acc;
	LionContainer *tmp;
	int			n = 0;
	int			i;

	if (set->sorted || set->n < 2)
	{
		set->sorted = true;
		return;
	}

	order = (int *) MemoryContextAlloc(st->buildcxt, sizeof(int) * set->n);
	for (i = 0; i < set->n; i++)
		order[i] = i;
	qsort_arg(order, set->n, sizeof(int), lo_cmp_slot, set->keys);

	keys = (uint32 *) MemoryContextAlloc(st->setcxt, sizeof(uint32) * set->cap);
	conts = (LionContainer **)
		MemoryContextAlloc(st->setcxt, sizeof(LionContainer *) * set->cap);
	acc = (LionContainer *) MemoryContextAlloc(st->buildcxt,
											   LION_CONTAINER_MAX_SIZE);
	tmp = (LionContainer *) MemoryContextAlloc(st->buildcxt,
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

			memcpy(acc, set->conts[order[i]],
				   lion_container_size(set->conts[order[i]]));
			for (m = i + 1; m < j; m++)
			{
				LionContainer *swap;

				lion_container_or(acc, set->conts[order[m]], tmp);
				swap = acc;
				acc = tmp;
				tmp = swap;
			}
			size = lion_container_size(acc);
			conts[n] = (LionContainer *) MemoryContextAlloc(st->setcxt, size);
			memcpy(conts[n], acc, size);
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
	pfree(acc);
	pfree(tmp);
	set->keys = keys;
	set->conts = conts;
	set->n = n;
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

	/* the leaves' run-time keys, for this scan's Param values */
	ResetExprContext(st->lrtcxt);
	for (i = 0; i < st->nleaves; i++)
	{
		if (st->leaves[i].nrtkeys > 0)
			ExecIndexEvalRuntimeKeys(st->lrtcxt, st->leaves[i].rtkeys,
									 st->leaves[i].nrtkeys);
	}

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
 * Executor
 * --------------------------------------------------------------------- */

static Node *
lo_create_state(CustomScan *cscan)
{
	LionOrderedState *st = (LionOrderedState *)
		newNode(sizeof(LionOrderedState), T_CustomScanState);

	st->css.methods = &lo_exec_methods;
	/* heap tuples, fetched through the table AM into a buffer slot */
	st->css.slotOps = &TTSOpsBufferHeapTuple;
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
		list_length(cscan->custom_exprs) != 4)
		elog(ERROR, "LionOrdered: unrecognized custom_private shape (%d members)",
			 list_length(cscan->custom_private));

	st->ordoid = linitial_oid((List *) list_nth(cscan->custom_private, LO_PRIV_ORD));
	ints = (List *) list_nth(cscan->custom_private, LO_PRIV_INTS);
	st->dir = (ScanDirection) linitial_int(ints);
	st->lossyqual = (lsecond_int(ints) & LO_FLAG_LOSSY) != 0;
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

	st->nleaves = list_length(leafoids);
	st->leaves = (LoLeaf *) palloc0(sizeof(LoLeaf) * Max(st->nleaves, 1));
	pos = list_head(tree);
	st->tree = lo_decode_tree(st, tree, &pos, leafoids, lionquals,
							  &leafno, &qualno);
	if (leafno != st->nleaves || qualno != list_length(lionquals) || pos != NULL)
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
}

/* The next TID of the ordered index, or NULL at its end. */
static ItemPointer
lo_next_tid(LionOrderedState *st)
{
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
lo_fetch(LionOrderedState *st, TupleTableSlot *slot)
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

static void
lo_start_walk(LionOrderedState *st)
{
	EState	   *estate = st->css.ss.ps.state;

	if (st->nortkeys > 0)
	{
		ResetExprContext(st->ortcxt);
		ExecIndexEvalRuntimeKeys(st->ortcxt, st->ortkeys, st->nortkeys);
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
			if (!lion_table_fetch_tid(heap, &tid, snapshot, &all_dead) ||
				!table_tuple_fetch_row_version(heap, &tid, snapshot, slot))
				continue;
			st->fetched++;
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

	if (st->set == NULL)
		lo_build_set(st);
	if (!st->started)
		lo_start_walk(st);

	/* an empty set selects nothing, and needs no walk */
	if (st->done || st->set->n == 0)
		return ExecClearTuple(slot);
	if (st->sorting)
		return lo_sort_next(st, slot);

	for (;;)
	{
		ItemPointer tid;
		int			idx;
		bool		counted = !st->degraded && !st->novisit;

		CHECK_FOR_INTERRUPTS();

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
		if (counted && st->nsort > 0 && !st->noswitch &&
			st->scanwalked >= LO_SWITCH_MIN_WALK &&
			st->scanwalked >= LO_SWITCH_RATIO * (st->members - st->distinct) &&
			lo_switch(st))
			return lo_sort_next(st, slot);

		tid = lo_next_tid(st);
		if (tid == NULL)
			break;
		st->walked++;
		st->scanwalked++;
		idx = lo_set_find(st->set, tid);
		if (idx < 0)
			continue;
		if (!lo_mark(st, idx, tid))
			continue;			/* met before: a recycled slot (§30.4) */
		st->distinct++;
		st->hits++;
		if (!lo_fetch(st, slot))
			continue;
		st->fetched++;

		if (!st->exact || st->scan->xs_recheck)
		{
			ResetExprContext(econtext);
			econtext->ecxt_scantuple = slot;
			if (!st->exact && !ExecQual(st->lionrecheck, econtext))
			{
				st->removed++;
				continue;
			}
			if (st->scan->xs_recheck && !ExecQual(st->ordrecheck, econtext))
				continue;
		}
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

	/*
	 * The set is rebuilt only when a Param of the lion quals changed; the
	 * snapshot is the same for the whole execution (DESIGN.md §30.4).
	 */
	if (st->set != NULL && node->ss.ps.chgParam != NULL &&
		bms_overlap(node->ss.ps.chgParam, st->lionparams))
		st->set = NULL;

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
	if (st->ordidx != NULL)
		index_close(st->ordidx, AccessShareLock);
	for (i = 0; i < st->nleaves; i++)
	{
		if (st->leaves[i].index != NULL)
			index_close(st->leaves[i].index, AccessShareLock);
	}
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
							 list_length(es->rtable) > 1 || es->verbose,
							 false);
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
	if (ScanDirectionIsBackward(st->dir))
		appendStringInfoString(&buf, " (backward)");
	ExplainPropertyText("Ordered By", buf.data, es);

	lo_explain_qual(node, (List *) list_nth(cscan->custom_exprs, LO_EXPR_ORDORIG),
					"Index Cond", ancestors, es);
	lo_explain_qual(node, (List *) list_nth(cscan->custom_exprs, LO_EXPR_LIONQUAL),
					"Lion Cond", ancestors, es);

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
	ExplainPropertyText("Lion Indexes", buf.data, es);

	if (es->analyze)
	{
		ExplainPropertyInteger("Index Entries Walked", NULL,
							   (int64) st->walked, es);
		ExplainPropertyInteger("Lion Set Hits", NULL, (int64) st->hits, es);
		ExplainPropertyInteger("Heap Fetches", NULL, (int64) st->fetched, es);
		if (st->removed > 0 || !st->exact)
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

	RegisterCustomScanMethods(&lo_scan_methods);

	lion_prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = lion_ordered_set_rel_pathlist;
}
