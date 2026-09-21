/*-------------------------------------------------------------------------
 *
 * rbi_customscan.c
 *		A CustomScan that answers
 *
 *			SELECT count(*) [, k] FROM t WHERE <clause> [AND <clause> ...]
 *			[GROUP BY k]
 *
 *		out of roaring posting sets, visiting the heap only for pages the
 *		visibility map does not vouch for.  DESIGN.md section 10 is the
 *		specification; the counting itself lives in rbi_count.c and follows
 *		the pin/visibility-map rule of DESIGN.md section 9.
 *
 * A clause is `k = const` (§10), `k = ANY (const array)` (§15), `k IS NULL`
 * or `k IS NOT NULL` (§14), each on a column with a usable roaring index.
 * The first three select rows and are intersected; `IS NOT NULL` subtracts
 * the index's NULL entry from the result, and when it is the only clause the
 * node sums the counts of every entry of that index instead.
 *
 * The node is planted at UPPERREL_GROUP_AGG by create_upper_paths_hook, so
 * it replaces the whole Agg-over-scan subtree rather than part of it.  Its
 * scan.scanrelid is 0 (it is an upper node with no scan relation of its own)
 * and custom_scan_tlist describes the tuples it produces: the group key Var,
 * if the query asks for it, followed by the count aggregates.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/tlist.h"
#include "storage/lmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"

#include "rbi.h"
#include "rbi_count.h"

/* GUC and the previous hook, both owned here and installed by _PG_init. */
bool		rbi_enable_count_pushdown = true;
create_upper_paths_hook_type rbi_prev_create_upper_paths_hook = NULL;

/* Kinds of column in custom_scan_tlist. */
#define RBI_TL_GROUPKEY		0
#define RBI_TL_COUNT		1
#define RBI_TL_COUNT_GROUPCOL	2	/* count(group column): 0 for the NULL
									 * group, the count otherwise (§14) */
#define RBI_TL_COUNT_ZERO	3	/* count(col) where a clause pins col to NULL */
/* RBI_TL_WHEREKEY + i: the key stored in the i'th clause's entry */
#define RBI_TL_WHEREKEY		4

/*
 * Kinds of WHERE clause the pushdown understands.  EQ, ARRAY and NULL select
 * rows (they are positive sources of the count); NOTNULL removes them
 * (DESIGN.md §14: the NULL rows are exactly the members of the index's
 * reserved NULL entry, so `IS NOT NULL` is their complement).
 */
#define RBI_CLAUSE_EQ		0	/* col = const */
#define RBI_CLAUSE_ARRAY	1	/* col = ANY (const array), DESIGN.md §15 */
#define RBI_CLAUSE_NULL		2	/* col IS NULL */
#define RBI_CLAUSE_NOTNULL	3	/* col IS NOT NULL */

#define RBI_CLAUSE_IS_POSITIVE(k)	((k) != RBI_CLAUSE_NOTNULL)

/*
 * An `IN` list longer than this is not pushed down: every listed value needs
 * its own posting set, and each of those may hold a buffer pin for as long as
 * the node runs (DESIGN.md §9).  A long list is also exactly the case where
 * the ordinary bitmap plan does well.
 */
#define RBI_MAX_ARRAY_ELEMS		1000

/* Flag bits of the third integer of RBI_PRIV_INTS. */
#define RBI_FLAG_SINGLEGROUP	0x01
#define RBI_FLAG_SUMALL			0x02

/*
 * What the planner decided, in a form the executor can be handed through
 * custom_private.  Everything in there has to be a copyable/serialisable
 * node, so it is four plain lists plus one filled in at plan time:
 *
 *	0	OidList: heap Oid, group index Oid (InvalidOid if none), then one
 *		Oid per WHERE clause, in the same order as the other lists
 *	1	IntList: base RT index, group attnum (0 if none), the RBI_FLAG_* bits,
 *		then one attnum per WHERE clause
 *	2	List of Const: one per WHERE clause - the compared value, the array of
 *		an IN list, or a NULL placeholder for a null test
 *	3	IntList: RBI_CLAUSE_* for each WHERE clause
 *	4	IntList: RBI_TL_* for each custom_scan_tlist column (added at plan
 *		time, when the target list is known)
 */
#define RBI_PRIV_OIDS		0
#define RBI_PRIV_INTS		1
#define RBI_PRIV_CONSTS		2
#define RBI_PRIV_CLAUSEKINDS 3
#define RBI_PRIV_TLKINDS	4

/*
 * One WHERE clause of the pushdown, as the executor sees it.
 */
typedef struct RBIClauseState
{
	int			kind;			/* RBI_CLAUSE_* */
	Oid			idxoid;
	AttrNumber	attno;
	Const	   *con;			/* value, array, or a NULL placeholder */
	Relation	idx;
} RBIClauseState;

typedef struct RBICountScanState
{
	CustomScanState css;

	/* decoded from custom_private */
	Oid			heapoid;
	Index		scanrelid;
	Oid			groupidxoid;
	AttrNumber	groupattno;
	bool		singlegroup;	/* GROUP BY over constant columns only */
	bool		sumall;			/* no GROUP BY, but every entry of the group
								 * index is counted and summed (DESIGN.md §14,
								 * `col IS NOT NULL` with nothing else) */
	int			nclause;
	RBIClauseState *clause;
	int			ntlist;
	int		   *tlkind;

	/* runtime */
	Relation	heap;
	Relation	groupidx;

	/*
	 * The inputs of the count: slot 0 is the group (or the driving index of
	 * a sumall), slots 1 .. nclause the WHERE clauses.  The clause sources
	 * are located once per node execution and keep their pins (DESIGN.md
	 * section 9) until the node is reset or closed; the group's set is
	 * located, counted and released one group at a time.
	 */
	RBICountSource *sources;
	RBIPostingSet groupset;
	bool		located;
	bool		wheremissing;	/* a positive clause selects nothing at all */
	bool		scanning;
	bool		done;
	RBIEntryScan escan;

	MemoryContext pergroup;		/* reset before each group is counted */
	MemoryContext wherecxt;		/* the located WHERE payload copies */
	RBICountStats stats;
} RBICountScanState;

static Plan *rbi_plan_custom_path(PlannerInfo *root, RelOptInfo *rel,
								  CustomPath *best_path, List *tlist,
								  List *clauses, List *custom_plans);
static Node *rbi_create_custom_scan_state(CustomScan *cscan);
static void rbi_begin_custom_scan(CustomScanState *node, EState *estate,
								  int eflags);
static TupleTableSlot *rbi_exec_custom_scan(CustomScanState *node);
static void rbi_end_custom_scan(CustomScanState *node);
static void rbi_rescan_custom_scan(CustomScanState *node);
static void rbi_explain_custom_scan(CustomScanState *node, List *ancestors,
									ExplainState *es);

static const CustomPathMethods rbi_count_path_methods = {
	.CustomName = "RoaringCount",
	.PlanCustomPath = rbi_plan_custom_path,
	.ReparameterizeCustomPathByChild = NULL,
};

static const CustomScanMethods rbi_count_scan_methods = {
	.CustomName = "RoaringCount",
	.CreateCustomScanState = rbi_create_custom_scan_state,
};

static const CustomExecMethods rbi_count_exec_methods = {
	.CustomName = "RoaringCount",
	.BeginCustomScan = rbi_begin_custom_scan,
	.ExecCustomScan = rbi_exec_custom_scan,
	.EndCustomScan = rbi_end_custom_scan,
	.ReScanCustomScan = rbi_rescan_custom_scan,
	.ExplainCustomScan = rbi_explain_custom_scan,
};


/* =====================================================================
 * Planner
 * ===================================================================== */

/*
 * Peel binary-coercion relabels off an expression.  A varchar column
 * compared with a text constant arrives as RelabelType(Var) = Const, and the
 * roaring index on that column is a text_ops index, so the relabelled form is
 * exactly what we want to match.
 */
static Node *
rbi_strip(Node *node)
{
	while (node != NULL && IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;
	return node;
}

/*
 * A usable roaring index on one plain column of rel, or NULL.  Only indexes
 * the planner put in rel->indexlist are considered, which already excludes
 * invalid ones (get_relation_info() skips !indisvalid).
 */
static IndexOptInfo *
rbi_find_roaring_index(RelOptInfo *rel, AttrNumber attno)
{
	Oid			amoid = rbi_get_am_oid();
	ListCell   *lc;

	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);

		if (idx->relam != amoid)
			continue;
		if (idx->hypothetical)
			continue;
		if (idx->ncolumns != 1 || idx->nkeycolumns != 1)
			continue;
		if (idx->indpred != NIL || idx->indexprs != NIL)
			continue;
		if (idx->indexkeys[0] != attno)
			continue;

		return idx;
	}

	return NULL;
}

/*
 * Can the aggregate be answered by counting a posting set?
 *
 * count(*) always can.  count(col) can when every row the node counts is
 * known to have col non-null, or known to have it NULL - in which case the
 * answer is 0 - which DESIGN.md §14 makes true in more cases than the
 * attnotnull of DESIGN.md §10:
 *
 *	- col is declared NOT NULL;
 *	- a clause pins col with a strict operator (`col = c`, `col = ANY (...)`)
 *	  or with `col IS NOT NULL`;
 *	- col is the group column: the count of a non-NULL group is count(*), and
 *	  the NULL group's is 0;
 *	- a clause says `col IS NULL`: then it is 0 for every group.
 */
static bool
rbi_agg_is_count(Aggref *agg, Index rti, RelOptInfo *rel,
				 AttrNumber groupattno, const List *nonnullattnos,
				 const List *nullattnos)
{
	TargetEntry *tle;
	Node	   *arg;
	Var		   *var;

	if (agg->aggfnoid != F_COUNT_ && agg->aggfnoid != F_COUNT_ANY)
		return false;
	if (agg->aggdistinct != NIL || agg->aggorder != NIL ||
		agg->aggfilter != NULL || agg->aggvariadic)
		return false;
	if (agg->agglevelsup != 0 || agg->aggsplit != AGGSPLIT_SIMPLE)
		return false;
	if (agg->aggkind != AGGKIND_NORMAL)
		return false;

	if (agg->aggfnoid == F_COUNT_)
		return agg->aggstar && agg->args == NIL;

	/* count(col) */
	if (list_length(agg->args) != 1)
		return false;
	tle = (TargetEntry *) linitial(agg->args);
	if (!IsA(tle, TargetEntry))
		return false;
	arg = rbi_strip((Node *) tle->expr);
	if (arg == NULL || !IsA(arg, Var))
		return false;
	var = (Var *) arg;
	if (var->varno != (int) rti || var->varattno <= 0 ||
			var->varlevelsup != 0)
		return false;

	if (groupattno == var->varattno)
		return true;
	if (list_member_int((List *) nullattnos, (int) var->varattno))
		return true;
	if (list_member_int((List *) nonnullattnos, (int) var->varattno))
		return true;

	return bms_is_member(var->varattno, rel->notnullattnums);
}

/*
 * Cost the pushdown (DESIGN.md section 10).
 *
 * Proportional rather than exact.  What a count actually reads:
 *
 *	- for each WHERE key: ONE bucket page (a hash lookup), then that key's own
 *	  container chain, which was written sequentially and is a fraction of the
 *	  index's container pages proportional to the clause selectivity;
 *	- for a GROUP BY: every page of the group index;
 *	- one O(1) step per container (the visibility map is read per container);
 *	- the heap the visibility map cannot vouch for: the TIDs on blocks that
 *	  are not all-visible (pg_class.relallvisible via RelOptInfo.allvisfrac),
 *	  each rechecked in the heap, on as many distinct blocks as there can be.
 *
 * The bucket pages are NOT charged wholesale: an index carries at least
 * RBI_DEFAULT_BUCKETS of them, which would price a single-key count on a
 * small table above a sequential scan of the whole table.  The bucket count
 * comes from the index's meta page (cached in rd_amcache), as
 * btcostestimate reads the tree height from the metapage.
 *
 * It has to beat Agg-over-BitmapHeapScan when the pushdown really is cheaper
 * and lose when it is not; it is not meant to be comparable with core cost
 * estimates to the last decimal.
 */
static double
rbi_index_bucket_pages(IndexOptInfo *idx)
{
	Relation	indexrel = index_open(idx->indexoid, AccessShareLock);
	RBIState   *state = rbi_get_state(indexrel);
	double		nbuckets = (double) state->meta.nbuckets;

	index_close(indexrel, AccessShareLock);
	return nbuckets;
}

static void
rbi_cost_count_path(PlannerInfo *root, RelOptInfo *input_rel,
					CustomPath *cpath, IndexOptInfo *groupidx,
					List *whereidx, List *whereclauses, double numgroups,
					double outrows)
{
	double		heap_pages = Max((double) input_rel->pages, 1.0);
	double		dirtyfrac = 1.0 - input_rel->allvisfrac;
	double		matching = Max(input_rel->rows, 1.0);
	double		containers_per_key;
	double		random_pages = 0;	/* bucket page lookups */
	double		seq_pages = 0;	/* container chains, read in order */
	double		ncontainers;
	double		recheck_tids;
	double		recheck_pages;
	Cost		run;
	ListCell   *lc1;
	ListCell   *lc2;

	/*
	 * A key's posting set has at most one container per
	 * RBI_BLOCKS_PER_CONTAINER heap pages and at most one per matching row.
	 */
	containers_per_key = Min(heap_pages / RBI_BLOCKS_PER_CONTAINER, matching);
	containers_per_key = Max(containers_per_key, 1.0);

	forboth(lc1, whereidx, lc2, whereclauses)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc1);
		Selectivity sel = clause_selectivity(root, (Node *) lfirst(lc2),
											 0, JOIN_INNER, NULL);
		double		container_pages;

		container_pages = (double) idx->pages - 1.0 - rbi_index_bucket_pages(idx);
		container_pages = Max(container_pages, 0.0);

		random_pages += 1.0;
		seq_pages += Max(1.0, container_pages * sel);
	}
	if (groupidx != NULL)
		seq_pages += Max(1.0, (double) groupidx->pages);

	if (groupidx != NULL)
		ncontainers = numgroups * Max(1.0, Min(heap_pages / RBI_BLOCKS_PER_CONTAINER,
											   matching / Max(numgroups, 1.0)));
	else
		ncontainers = containers_per_key;
	ncontainers *= Max(1, list_length(whereidx) + (groupidx != NULL ? 1 : 0));

	/*
	 * Rechecking is what makes the pushdown expensive, and the estimate has
	 * to say so: every TID whose heap block the visibility map cannot vouch
	 * for is fetched from the heap.  Charge cpu_tuple_cost for each of them
	 * and one random page fetch per distinct block they are expected to
	 * touch.  Assuming a distinct block per rechecked TID (capped by the size
	 * of the heap) is pessimistic on purpose: a thousand groups of one TID
	 * per block on a heap that is not all-visible has to lose to a sequential
	 * scan, because that is exactly what it would do.
	 */
	recheck_tids = matching * dirtyfrac;
	recheck_pages = Min(recheck_tids, heap_pages);

	run = random_pages * random_page_cost;
	run += seq_pages * seq_page_cost;
	run += ncontainers * cpu_operator_cost * 2.0;	/* block mask + VM mask */
	run += recheck_pages * random_page_cost;
	run += recheck_tids * cpu_tuple_cost;
	run += numgroups * cpu_tuple_cost;

	cpath->path.rows = outrows;
	cpath->path.disabled_nodes = 0;
	/* Without GROUP BY the single output row needs the whole scan first. */
	cpath->path.startup_cost = (outrows <= 1.0) ? run : 0.0;
	cpath->path.total_cost = run;
}

/*
 * Number of elements of a Const array, or -1 when it is not a plain array.
 */
static int
rbi_array_const_nelems(Const *con)
{
	ArrayType  *arr;
	int			nelems;

	if (con->constisnull)
		return -1;
	if (!OidIsValid(get_element_type(con->consttype)))
		return -1;

	arr = DatumGetArrayTypeP(con->constvalue);
	if (ARR_NDIM(arr) == 0)
		nelems = 0;
	else if (ARR_NDIM(arr) != 1)
		nelems = -1;
	else
		nelems = ARR_DIMS(arr)[0];

	if ((Pointer) arr != DatumGetPointer(con->constvalue))
		pfree(arr);

	return nelems;
}

/*
 * Decide whether count(*) over input_rel can be answered from roaring
 * posting sets and, if so, add a CustomPath to output_rel.  Every failed
 * check simply returns: the normal plan is always available.
 */
static void
rbi_try_count_path(PlannerInfo *root, RelOptInfo *input_rel,
				   RelOptInfo *output_rel, GroupPathExtraData *extra)
{
	Query	   *parse = root->parse;
	RangeTblEntry *rte;
	Index		rti;
	Var		   *groupvar = NULL;
	AttrNumber	groupattno = 0;
	IndexOptInfo *groupidx = NULL;
	bool		singlegroup = false;
	bool		sumall = false;
	List	   *whereidx = NIL;		/* IndexOptInfo per clause */
	List	   *whereattnos = NIL;	/* its column */
	List	   *whereclauses = NIL; /* the clause, for selectivity */
	List	   *whereconsts = NIL;	/* its Const (a placeholder for a null test) */
	List	   *wherekinds = NIL;	/* RBI_CLAUSE_* */
	List	   *posattnos = NIL;	/* columns with a positive clause */
	List	   *eqattnos = NIL;		/* columns pinned to one value */
	List	   *nonnullattnos = NIL;	/* columns a clause proves non-null */
	List	   *nullattnos = NIL;	/* columns a clause pins to NULL */
	List	   *costidx = NIL;		/* whereidx, positive clauses only */
	List	   *costclauses = NIL;
	Var		   *notnullvar = NULL;	/* the first `IS NOT NULL` column ... */
	IndexOptInfo *notnullidx = NULL;	/* ... and its index */
	List	   *oids;
	List	   *ints;
	List	   *consts = NIL;
	List	   *ckinds = NIL;
	CustomPath *cpath;
	double		numgroups;
	double		outrows;
	bool		haveagg = false;
	bool		havepositive = false;
	ListCell   *lc;

	/* ---- the query as a whole ---- */
	if (parse->commandType != CMD_SELECT)
		return;
	if (!parse->hasAggs)
		return;
	if (parse->groupingSets != NIL || parse->havingQual != NULL ||
		root->hasHavingQual)
		return;
	if (parse->hasWindowFuncs || parse->hasTargetSRFs ||
		parse->hasDistinctOn || parse->distinctClause != NIL)
		return;
	if (parse->rowMarks != NIL || root->rowMarks != NIL)
		return;
	if (extra != NULL && extra->patype != PARTITIONWISE_AGGREGATE_NONE)
		return;
	if (extra != NULL && extra->havingQual != NULL)
		return;

	/* ---- a single ordinary base relation ---- */
	if (input_rel->reloptkind != RELOPT_BASEREL)
		return;
	if (bms_membership(input_rel->relids) != BMS_SINGLETON)
		return;
	rti = input_rel->relid;
	if (rti == 0 || rti >= (Index) root->simple_rel_array_size)
		return;
	rte = root->simple_rte_array[rti];
	if (rte == NULL || rte->rtekind != RTE_RELATION || rte->inh)
		return;
	if (rte->relkind != RELKIND_RELATION && rte->relkind != RELKIND_MATVIEW)
		return;
	if (rte->securityQuals != NIL || rte->tablesample != NULL)
		return;
	if (input_rel->indexlist == NIL)
		return;

	/* ---- GROUP BY: nothing, or one indexed column ---- */
	if (list_length(root->processed_groupClause) > 1)
		return;

	if (root->processed_groupClause == NIL)
	{
		/*
		 * Every GROUP BY column was proved constant by the planner (it does
		 * that for a column with an equality qual against a Const), so the
		 * query has exactly one group - but, unlike a plain aggregate, it
		 * must produce no row at all when nothing matches.
		 */
		singlegroup = (parse->groupClause != NIL);
	}
	else
	{
		SortGroupClause *sgc;
		TargetEntry *tle;
		Node	   *expr;

		sgc = (SortGroupClause *) linitial(root->processed_groupClause);
		tle = get_sortgroupclause_tle(sgc, root->processed_tlist);
		if (tle == NULL)
			return;
		expr = rbi_strip((Node *) tle->expr);
		if (expr == NULL || !IsA(expr, Var))
			return;
		groupvar = (Var *) expr;
		if (groupvar->varno != (int) rti || groupvar->varattno <= 0 ||
			groupvar->varlevelsup != 0)
			return;

		/*
		 * A nullable group column is fine now: NULL keys have an entry of
		 * their own, so the NULL group is produced like any other
		 * (DESIGN.md §14).
		 */
		groupattno = groupvar->varattno;
		groupidx = rbi_find_roaring_index(input_rel, groupattno);
		if (groupidx == NULL)
			return;
	}

	/* ---- every WHERE clause must be one the posting sets can answer ---- */
	foreach(lc, input_rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
		Node	   *clause;
		Var		   *var = NULL;
		Const	   *con = NULL;
		IndexOptInfo *idx;
		Oid			cmptype = InvalidOid;	/* type the index is compared with */
		int			kind;

		if (!IsA(rinfo, RestrictInfo) || rinfo->pseudoconstant)
			return;

		clause = (Node *) rinfo->clause;

		if (IsA(clause, OpExpr))
		{
			OpExpr	   *op = (OpExpr *) clause;
			Node	   *left;
			Node	   *right;

			if (list_length(op->args) != 2)
				return;
			if (!op_strict(op->opno))
				return;

			left = rbi_strip((Node *) linitial(op->args));
			right = rbi_strip((Node *) lsecond(op->args));
			if (left == NULL || right == NULL)
				return;

			if (IsA(left, Var) && IsA(right, Const))
			{
				var = (Var *) left;
				con = (Const *) right;
			}
			else if (IsA(left, Const) && IsA(right, Var))
			{
				var = (Var *) right;
				con = (Const *) left;
			}
			else
				return;

			if (con->constisnull)
				return;

			idx = rbi_find_roaring_index(input_rel, var->varattno);
			if (idx == NULL)
				return;
			if (get_op_opfamily_strategy(op->opno, idx->opfamily[0]) != 1)
				return;

			cmptype = con->consttype;
			kind = RBI_CLAUSE_EQ;
		}
		else if (IsA(clause, ScalarArrayOpExpr))
		{
			ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;
			Node	   *left;
			Node	   *right;
			int			nelems;

			/* `= ALL (...)` is not a union of keys (DESIGN.md §15). */
			if (!saop->useOr)
				return;
			if (list_length(saop->args) != 2)
				return;
			if (!op_strict(saop->opno))
				return;

			left = rbi_strip((Node *) linitial(saop->args));
			right = rbi_strip((Node *) lsecond(saop->args));
			if (left == NULL || right == NULL)
				return;
			if (!IsA(left, Var) || !IsA(right, Const))
				return;
			var = (Var *) left;
			con = (Const *) right;
			if (con->constisnull)
				return;

			nelems = rbi_array_const_nelems(con);
			if (nelems < 0 || nelems > RBI_MAX_ARRAY_ELEMS)
				return;

			idx = rbi_find_roaring_index(input_rel, var->varattno);
			if (idx == NULL)
				return;
			if (get_op_opfamily_strategy(saop->opno, idx->opfamily[0]) != 1)
				return;

			cmptype = get_element_type(con->consttype);
			if (!OidIsValid(cmptype))
				return;
			kind = RBI_CLAUSE_ARRAY;
		}
		else if (IsA(clause, NullTest))
		{
			NullTest   *nt = (NullTest *) clause;
			Node	   *arg;

			if (nt->argisrow)
				return;
			arg = rbi_strip((Node *) nt->arg);
			if (arg == NULL || !IsA(arg, Var))
				return;
			var = (Var *) arg;

			idx = rbi_find_roaring_index(input_rel, var->varattno);
			if (idx == NULL)
				return;

			kind = (nt->nulltesttype == IS_NULL) ?
				RBI_CLAUSE_NULL : RBI_CLAUSE_NOTNULL;
			/* The executor needs no value; keep the lists in step. */
			con = makeNullConst(var->vartype, var->vartypmod, var->varcollid);
		}
		else
			return;

		if (var->varno != (int) rti || var->varattno <= 0 ||
			var->varlevelsup != 0)
			return;

		/*
		 * The opfamily must be able to hash the compared type and to compare
		 * it with the indexed one, or the lookup in rbi_count.c would fail at
		 * run time.  Cross-type integer equality passes this.
		 */
		if (OidIsValid(cmptype))
		{
			if (!OidIsValid(get_opfamily_member(idx->opfamily[0],
												idx->opcintype[0], cmptype, 1)))
				return;
			if (!OidIsValid(get_opfamily_proc(idx->opfamily[0],
											  cmptype, cmptype, 1)))
				return;
		}

		if (RBI_CLAUSE_IS_POSITIVE(kind))
		{
			/*
			 * At most one positive clause per column: the same clause twice
			 * is just a duplicate, two different ones mean the query selects
			 * little or nothing and we would rather leave that to the normal
			 * plan.  `IS NOT NULL` is not subject to this - it constrains
			 * nothing by itself and is simply subtracted.
			 */
			if (list_member_int(posattnos, (int) var->varattno))
			{
				ListCell   *l1;
				ListCell   *l2;
				ListCell   *l3;
				bool		same = false;

				forthree(l1, whereattnos, l2, whereconsts, l3, wherekinds)
				{
					if (lfirst_int(l1) != (int) var->varattno ||
						lfirst_int(l3) == RBI_CLAUSE_NOTNULL)
						continue;
					same = (lfirst_int(l3) == kind &&
							equal((Const *) lfirst(l2), con));
					break;
				}
				if (!same)
					return;
				continue;
			}
			posattnos = lappend_int(posattnos, (int) var->varattno);
			havepositive = true;
			costidx = lappend(costidx, idx);
			costclauses = lappend(costclauses, clause);
		}

		switch (kind)
		{
			case RBI_CLAUSE_EQ:
				eqattnos = lappend_int(eqattnos, (int) var->varattno);
				nonnullattnos = lappend_int(nonnullattnos, (int) var->varattno);
				break;
			case RBI_CLAUSE_NOTNULL:
				if (notnullvar == NULL)
				{
					notnullvar = var;
					notnullidx = idx;
				}
				nonnullattnos = lappend_int(nonnullattnos, (int) var->varattno);
				break;
			case RBI_CLAUSE_ARRAY:
				nonnullattnos = lappend_int(nonnullattnos, (int) var->varattno);
				break;
			case RBI_CLAUSE_NULL:
				nullattnos = lappend_int(nullattnos, (int) var->varattno);
				break;
		}

		whereidx = lappend(whereidx, idx);
		whereattnos = lappend_int(whereattnos, (int) var->varattno);
		whereclauses = lappend(whereclauses, clause);
		whereconsts = lappend(whereconsts, con);
		wherekinds = lappend_int(wherekinds, kind);
	}

	/* Something has to drive the count. */
	if (groupidx == NULL && !havepositive)
	{
		/*
		 * Only `IS NOT NULL` clauses: that column's index knows every row of
		 * the table, so the count is the sum over all of its entries with the
		 * NULL one subtracted (DESIGN.md §14).  The entries of one index are
		 * disjoint - a row has one value per column - so summing them is the
		 * count of their union.
		 */
		if (notnullvar == NULL)
			return;
		groupidx = notnullidx;
		sumall = true;
	}
	/* A group folded to a constant can only have come from a WHERE key. */
	if (singlegroup && !havepositive)
		return;

	/* ---- the grouped relation's target ---- */
	foreach(lc, output_rel->reltarget->exprs)
	{
		Node	   *node = (Node *) lfirst(lc);

		if (IsA(node, Var))
		{
			Var		   *v = (Var *) node;

			if (v->varno != (int) rti || v->varattno <= 0 ||
				v->varlevelsup != 0)
				return;

			/*
			 * The column has to be either the group key or one a clause pins
			 * to a single value.  In the second case the value we output is
			 * the key the index stored, not the constant from the query: a
			 * cross-type or otherwise non-identical constant that compares
			 * equal must not change what the query prints.  A column pinned
			 * to NULL by `IS NULL` prints NULL, which the stored key of the
			 * reserved entry says as well.
			 */
			if (!(groupvar != NULL && v->varattno == groupvar->varattno) &&
				!list_member_int(eqattnos, (int) v->varattno) &&
				!list_member_int(nullattnos, (int) v->varattno))
				return;
		}
		else if (IsA(node, Aggref))
		{
			if (!rbi_agg_is_count((Aggref *) node, rti, input_rel,
								  groupattno, nonnullattnos, nullattnos))
				return;
			haveagg = true;
		}
		else
			return;
	}
	if (!haveagg)
		return;

	/* ---- build the path ---- */
	if (groupvar != NULL)
		numgroups = estimate_num_groups(root, list_make1(groupvar),
										input_rel->rows, NULL, NULL);
	else if (sumall)
	{
		/* Every entry of the driving index is visited, one group or not. */
		Assert(notnullvar != NULL);
		numgroups = estimate_num_groups(root, list_make1(notnullvar),
										input_rel->rows, NULL, NULL);
	}
	else
		numgroups = 1.0;

	outrows = (groupvar != NULL) ? numgroups : 1.0;

	oids = list_make2_oid(rte->relid,
						  groupidx ? groupidx->indexoid : InvalidOid);
	ints = list_make3_int((int) rti, (int) groupattno,
						  (singlegroup ? RBI_FLAG_SINGLEGROUP : 0) |
						  (sumall ? RBI_FLAG_SUMALL : 0));
	{
		ListCell   *l1;
		ListCell   *l2;
		ListCell   *l3;
		ListCell   *l4;

		forfour(l1, whereidx, l2, whereattnos, l3, whereconsts, l4, wherekinds)
		{
			oids = lappend_oid(oids, ((IndexOptInfo *) lfirst(l1))->indexoid);
			ints = lappend_int(ints, lfirst_int(l2));
			consts = lappend(consts, copyObject((Const *) lfirst(l3)));
			ckinds = lappend_int(ckinds, lfirst_int(l4));
		}
	}

	cpath = makeNode(CustomPath);
	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = output_rel;
	cpath->path.pathtarget = output_rel->reltarget;
	cpath->path.param_info = NULL;
	cpath->path.parallel_aware = false;
	cpath->path.parallel_safe = false;
	cpath->path.parallel_workers = 0;
	cpath->path.pathkeys = NIL;	/* groups come out in bucket order */
	cpath->flags = 0;
	cpath->custom_paths = NIL;
	cpath->custom_restrictinfo = NIL;
	cpath->custom_private = list_make4(oids, ints, consts, ckinds);
	cpath->methods = &rbi_count_path_methods;

	rbi_cost_count_path(root, input_rel, cpath, groupidx, costidx,
						costclauses, numgroups, outrows);

	add_path(output_rel, &cpath->path);
}

void
rbi_create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
					   RelOptInfo *input_rel, RelOptInfo *output_rel,
					   void *extra)
{
	if (rbi_prev_create_upper_paths_hook != NULL)
		rbi_prev_create_upper_paths_hook(root, stage, input_rel, output_rel,
										 extra);

	if (stage != UPPERREL_GROUP_AGG)
		return;
	if (!rbi_enable_count_pushdown)
		return;

	rbi_try_count_path(root, input_rel, output_rel,
					   (GroupPathExtraData *) extra);
}

/*
 * Turn the path into a CustomScan.
 *
 * scan.scanrelid is 0 because this is an upper node, so custom_scan_tlist has
 * to describe the tuple the node produces and setrefs.c rewrites the plan's
 * targetlist into INDEX_VAR references against it (set_customscan_references).
 */
static Plan *
rbi_plan_custom_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
					 List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	List	   *ctlist = NIL;
	List	   *kinds = NIL;
	List	   *ints;
	List	   *ckinds;
	AttrNumber	groupattno;
	ListCell   *lc;

	/*
	 * The name has to be resolvable before any CustomScan node of ours is
	 * written out or read back; plan time is the first moment that can
	 * happen, and registering twice is harmless.
	 */
	if (GetCustomScanMethods("RoaringCount", true) == NULL)
		RegisterCustomScanMethods(&rbi_count_scan_methods);

	ints = (List *) list_nth(best_path->custom_private, RBI_PRIV_INTS);
	ckinds = (List *) list_nth(best_path->custom_private, RBI_PRIV_CLAUSEKINDS);
	groupattno = (AttrNumber) lsecond_int(ints);

	foreach(lc, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		Node	   *expr = (Node *) tle->expr;
		int			kind;
		ListCell   *l2;
		bool		dup = false;

		if (IsA(expr, Aggref))
		{
			Aggref	   *agg = (Aggref *) expr;

			/*
			 * count(*) and every count(col) the planner accepted are the
			 * count of the group, except two cases DESIGN.md §14 spells out:
			 * count of the group column is 0 in the NULL group, and count of
			 * a column a clause pins to NULL is always 0.
			 */
			kind = RBI_TL_COUNT;
			if (agg->args != NIL)
			{
				Node	   *arg = rbi_strip((Node *)
											((TargetEntry *) linitial(agg->args))->expr);
				AttrNumber	attno;
				int			i;

				Assert(arg != NULL && IsA(arg, Var));
				attno = ((Var *) arg)->varattno;

				if (groupattno != 0 && attno == groupattno)
					kind = RBI_TL_COUNT_GROUPCOL;
				else
				{
					for (i = 0; i < list_length(ckinds); i++)
					{
						if (list_nth_int(ckinds, i) == RBI_CLAUSE_NULL &&
							list_nth_int(ints, 3 + i) == (int) attno)
						{
							kind = RBI_TL_COUNT_ZERO;
							break;
						}
					}
				}
			}
		}
		else if (IsA(expr, Var))
		{
			AttrNumber	attno = ((Var *) expr)->varattno;
			int			i;

			if (groupattno != 0 && attno == groupattno)
				kind = RBI_TL_GROUPKEY;
			else
			{
				/*
				 * The value printed for the column is the key the clause's
				 * entry stored, so only a clause that pins the column to ONE
				 * value will do: an equality, or an `IS NULL` (whose entry
				 * stores no key and prints NULL).  A column may carry an
				 * `IS NOT NULL` clause as well, and that one says nothing
				 * about the value.
				 */
				kind = -1;
				for (i = 3; i < list_length(ints); i++)
				{
					int			ckind = list_nth_int(ckinds, i - 3);

					if (ckind != RBI_CLAUSE_EQ && ckind != RBI_CLAUSE_NULL)
						continue;
					if (list_nth_int(ints, i) == (int) attno)
					{
						kind = RBI_TL_WHEREKEY + (i - 3);
						break;
					}
				}
				if (kind < 0)
					elog(ERROR, "RoaringCount: column %d is neither grouped nor constrained",
						 attno);
			}
		}
		else
			elog(ERROR, "unexpected expression in RoaringCount target list");

		foreach(l2, ctlist)
		{
			if (equal(((TargetEntry *) lfirst(l2))->expr, expr))
			{
				dup = true;
				break;
			}
		}
		if (dup)
			continue;

		ctlist = lappend(ctlist,
						 makeTargetEntry((Expr *) copyObject(expr),
										 list_length(ctlist) + 1,
										 tle->resname ? pstrdup(tle->resname) : NULL,
										 false));
		kinds = lappend_int(kinds, kind);
	}

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = NIL;
	cscan->scan.scanrelid = 0;
	cscan->flags = best_path->flags;
	cscan->custom_plans = NIL;
	cscan->custom_exprs = NIL;
	cscan->custom_scan_tlist = ctlist;
	cscan->custom_relids = rel->relids;
	cscan->custom_private = lappend(list_copy(best_path->custom_private),
									kinds);
	cscan->methods = &rbi_count_scan_methods;

	return &cscan->scan.plan;
}


/* =====================================================================
 * Executor
 * ===================================================================== */

static Node *
rbi_create_custom_scan_state(CustomScan *cscan)
{
	RBICountScanState *st = (RBICountScanState *)
		newNode(sizeof(RBICountScanState), T_CustomScanState);

	st->css.methods = &rbi_count_exec_methods;
	return (Node *) st;
}

static void
rbi_begin_custom_scan(CustomScanState *node, EState *estate, int eflags)
{
	RBICountScanState *st = (RBICountScanState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	List	   *oids = (List *) list_nth(cscan->custom_private, RBI_PRIV_OIDS);
	List	   *ints = (List *) list_nth(cscan->custom_private, RBI_PRIV_INTS);
	List	   *consts = (List *) list_nth(cscan->custom_private, RBI_PRIV_CONSTS);
	List	   *ckinds = (List *) list_nth(cscan->custom_private, RBI_PRIV_CLAUSEKINDS);
	List	   *kinds = (List *) list_nth(cscan->custom_private, RBI_PRIV_TLKINDS);
	int			flags;
	int			i;

	st->heapoid = linitial_oid(oids);
	st->groupidxoid = lsecond_oid(oids);
	st->scanrelid = (Index) linitial_int(ints);
	st->groupattno = (AttrNumber) lsecond_int(ints);
	flags = lthird_int(ints);
	st->singlegroup = (flags & RBI_FLAG_SINGLEGROUP) != 0;
	st->sumall = (flags & RBI_FLAG_SUMALL) != 0;
	st->nclause = list_length(consts);

	st->ntlist = list_length(kinds);
	st->tlkind = (int *) palloc(sizeof(int) * Max(st->ntlist, 1));
	for (i = 0; i < st->ntlist; i++)
		st->tlkind[i] = list_nth_int(kinds, i);

	st->clause = (RBIClauseState *)
		palloc0(sizeof(RBIClauseState) * Max(st->nclause, 1));
	for (i = 0; i < st->nclause; i++)
	{
		st->clause[i].kind = list_nth_int(ckinds, i);
		st->clause[i].idxoid = list_nth_oid(oids, 2 + i);
		st->clause[i].attno = (AttrNumber) list_nth_int(ints, 3 + i);
		st->clause[i].con = (Const *) list_nth(consts, i);
	}

	st->located = false;
	st->wheremissing = false;
	st->scanning = false;
	st->done = false;
	memset(&st->stats, 0, sizeof(st->stats));

	st->pergroup = AllocSetContextCreate(estate->es_query_cxt,
										 "RoaringCount per-group",
										 ALLOCSET_SMALL_SIZES);
	st->wherecxt = AllocSetContextCreate(estate->es_query_cxt,
										 "RoaringCount where keys",
										 ALLOCSET_SMALL_SIZES);

	if ((eflags & EXEC_FLAG_EXPLAIN_ONLY) != 0)
		return;

	/*
	 * The executor already holds locks on every range table entry, so the
	 * heap is opened without taking another one.  The indexes are not range
	 * table entries, so they get their own AccessShareLock.
	 */
	st->heap = table_open(st->heapoid, NoLock);
	Assert(CheckRelationLockedByMe(st->heap, AccessShareLock, true));

	for (i = 0; i < st->nclause; i++)
		st->clause[i].idx = index_open(st->clause[i].idxoid, AccessShareLock);

	if (OidIsValid(st->groupidxoid))
		st->groupidx = index_open(st->groupidxoid, AccessShareLock);

	/* slot 0 is the group's posting set, 1..nclause the WHERE clauses */
	st->sources = (RBICountSource *)
		palloc0(sizeof(RBICountSource) * (st->nclause + 1));
	st->sources[0].nsets = 1;
	st->sources[0].sets = &st->groupset;
	st->sources[0].negated = false;
}

/*
 * Locate the posting sets of one `col = ANY (array)` clause (DESIGN.md §15):
 * one set per distinct non-NULL element, which the count then unions.
 */
static void
rbi_locate_array(RBICountScanState *st, RBIClauseState *cl,
				 RBICountSource *src)
{
	ArrayType  *arr = DatumGetArrayTypeP(cl->con->constvalue);
	Oid			elemtype = ARR_ELEMTYPE(arr);
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	Datum	   *elems;
	bool	   *nulls;
	int			nelems;
	int			nsets = 0;
	int			nfound = 0;
	int			i;

	get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
	deconstruct_array(arr, elemtype, elmlen, elmbyval, elmalign,
					  &elems, &nulls, &nelems);

	src->sets = (RBIPostingSet *)
		palloc0(sizeof(RBIPostingSet) * Max(nelems, 1));

	for (i = 0; i < nelems; i++)
	{
		bool		dup = false;
		int			j;

		if (nulls[i])
			continue;			/* `col = NULL` is never true */

		/*
		 * Looking a value up twice would only make the union do the same work
		 * again; it could not change the answer, because a union of a set
		 * with itself is that set.  The test is the bytewise one, so two
		 * values that compare equal without being identical still get a set
		 * each, which is equally harmless.  The planner caps the list at
		 * RBI_MAX_ARRAY_ELEMS, which bounds this loop.
		 */
		for (j = 0; j < i; j++)
		{
			if (!nulls[j] &&
				datumIsEqual(elems[i], elems[j], elmbyval, elmlen))
			{
				dup = true;
				break;
			}
		}
		if (dup)
			continue;

		if (rbi_posting_set_lookup(cl->idx, elems[i], elemtype,
								   &src->sets[nsets]))
			nfound++;
		nsets++;
	}

	src->nsets = nsets;

	/* An empty array, an all-NULL one, or no matching key: no rows at all. */
	if (nfound == 0)
		st->wheremissing = true;

	pfree(elems);
	pfree(nulls);
	if ((Pointer) arr != DatumGetPointer(cl->con->constvalue))
		pfree(arr);
}

/*
 * Locate the posting sets of every WHERE clause once for the whole node.
 * They keep their pins (for INLINE entries) until rbi_release_where(), which
 * is exactly the DESIGN.md section 9 discipline applied for the node's
 * lifetime rather than for one container.
 */
static void
rbi_locate_where(RBICountScanState *st)
{
	MemoryContext oldcxt;
	int			i;

	oldcxt = MemoryContextSwitchTo(st->wherecxt);

	for (i = 0; i < st->nclause; i++)
	{
		RBIClauseState *cl = &st->clause[i];
		RBICountSource *src = &st->sources[i + 1];

		src->negated = (cl->kind == RBI_CLAUSE_NOTNULL);
		src->nsets = 0;
		src->sets = NULL;

		switch (cl->kind)
		{
			case RBI_CLAUSE_EQ:
				src->sets = (RBIPostingSet *) palloc0(sizeof(RBIPostingSet));
				src->nsets = 1;
				if (!rbi_posting_set_lookup(cl->idx, cl->con->constvalue,
											cl->con->consttype, &src->sets[0]))
					st->wheremissing = true;
				break;

			case RBI_CLAUSE_ARRAY:
				rbi_locate_array(st, cl, src);
				break;

			case RBI_CLAUSE_NULL:
			case RBI_CLAUSE_NOTNULL:
				src->sets = (RBIPostingSet *) palloc0(sizeof(RBIPostingSet));
				src->nsets = 1;
				if (!rbi_posting_set_lookup_null(cl->idx, &src->sets[0]))
				{
					/*
					 * No NULL entry at all: `IS NULL` selects nothing, and
					 * `IS NOT NULL` has nothing to subtract.
					 */
					if (cl->kind == RBI_CLAUSE_NULL)
						st->wheremissing = true;
					else
						src->nsets = 0;
				}
				break;

			default:
				elog(ERROR, "RoaringCount: unknown clause kind %d", cl->kind);
		}
	}

	MemoryContextSwitchTo(oldcxt);
	st->located = true;
}

static void
rbi_release_where(RBICountScanState *st)
{
	int			i;
	int			j;

	if (st->sources == NULL)
		return;

	for (i = 0; i < st->nclause; i++)
	{
		RBICountSource *src = &st->sources[i + 1];

		for (j = 0; j < src->nsets; j++)
			rbi_posting_set_release(&src->sets[j]);
		src->nsets = 0;
		src->sets = NULL;
	}
	if (st->wherecxt != NULL)
		MemoryContextReset(st->wherecxt);
	st->located = false;
	st->wheremissing = false;
}

static TupleTableSlot *
rbi_emit_tuple(RBICountScanState *st, Datum key, bool keyisnull, int64 count)
{
	TupleTableSlot *slot = st->css.ss.ss_ScanTupleSlot;
	ExprContext *econtext = st->css.ss.ps.ps_ExprContext;
	int			i;

	ExecClearTuple(slot);
	for (i = 0; i < st->ntlist; i++)
	{
		int			kind = st->tlkind[i];

		slot->tts_isnull[i] = false;
		switch (kind)
		{
			case RBI_TL_GROUPKEY:
				slot->tts_values[i] = key;
				slot->tts_isnull[i] = keyisnull;
				break;

			case RBI_TL_COUNT:
				slot->tts_values[i] = Int64GetDatum(count);
				break;

			case RBI_TL_COUNT_GROUPCOL:
				/* count(group column): 0 in the NULL group (DESIGN.md §14) */
				slot->tts_values[i] = Int64GetDatum(keyisnull ? 0 : count);
				break;

			case RBI_TL_COUNT_ZERO:
				/* count(col) where a clause pins col to NULL */
				slot->tts_values[i] = Int64GetDatum(0);
				break;

			default:
				{
					/*
					 * A column a clause pins to one value: report the key the
					 * index stored for it, which is the value the heap holds
					 * (or NULL, for an `IS NULL` clause).
					 */
					RBICountSource *src =
						&st->sources[1 + (kind - RBI_TL_WHEREKEY)];

					/*
					 * A clause with no entry at all counts zero rows, and a
					 * group of zero rows is never emitted, so the key is
					 * always there by the time we get here.
					 */
					Assert(src->nsets == 1 && src->sets[0].hasstoredkey);
					if (src->nsets == 1 && src->sets[0].hasstoredkey)
					{
						slot->tts_values[i] = src->sets[0].storedkey;
						slot->tts_isnull[i] = src->sets[0].keyisnull;
					}
					else
					{
						slot->tts_values[i] = (Datum) 0;
						slot->tts_isnull[i] = true;
					}
					break;
				}
		}
	}
	ExecStoreVirtualTuple(slot);

	econtext->ecxt_scantuple = slot;
	if (st->css.ss.ps.ps_ProjInfo != NULL)
		return ExecProject(st->css.ss.ps.ps_ProjInfo);
	return slot;
}

static TupleTableSlot *
rbi_exec_custom_scan(CustomScanState *node)
{
	RBICountScanState *st = (RBICountScanState *) node;
	EState	   *estate = node->ss.ps.state;
	MemoryContext oldcxt;
	int64		count;

	if (st->done)
		return NULL;

	if (!st->located)
		rbi_locate_where(st);

	/* ---- no index to iterate: exactly one row ---- */
	if (!OidIsValid(st->groupidxoid))
	{
		/* Without a group index a clause has to drive the count. */
		Assert(st->nclause > 0);
		st->done = true;

		if (st->wheremissing)
			count = 0;
		else
		{
			MemoryContextReset(st->pergroup);
			oldcxt = MemoryContextSwitchTo(st->pergroup);
			count = rbi_count_sources(st->heap, estate->es_snapshot,
									  st->nclause, &st->sources[1],
									  &st->stats);
			MemoryContextSwitchTo(oldcxt);
		}

		/*
		 * A plain aggregate always produces its one row; a GROUP BY whose
		 * columns the planner folded to constants produces one only if the
		 * group exists.
		 */
		if (count == 0 && st->singlegroup)
			return NULL;

		return rbi_emit_tuple(st, (Datum) 0, true, count);
	}

	/* ---- the group index drives the count ---- */
	if (st->wheremissing)
	{
		st->done = true;
		/* A sum over all entries still has to report its one row. */
		if (st->sumall)
			return rbi_emit_tuple(st, (Datum) 0, true, 0);
		return NULL;
	}

	if (!st->scanning)
	{
		rbi_entry_scan_begin(&st->escan, st->groupidx);
		st->scanning = true;
	}

	/* ---- every entry of the index, summed into one row ---- */
	if (st->sumall)
	{
		int64		total = 0;
		Datum		key;

		for (;;)
		{
			CHECK_FOR_INTERRUPTS();

			MemoryContextReset(st->pergroup);
			oldcxt = MemoryContextSwitchTo(st->pergroup);

			if (!rbi_entry_scan_next(&st->escan, &key, &st->groupset))
			{
				MemoryContextSwitchTo(oldcxt);
				break;
			}

			total += rbi_count_sources(st->heap, estate->es_snapshot,
									   st->nclause + 1, st->sources,
									   &st->stats);
			rbi_posting_set_release(&st->groupset);
			MemoryContextSwitchTo(oldcxt);
		}

		st->done = true;
		return rbi_emit_tuple(st, (Datum) 0, true, total);
	}

	/* ---- GROUP BY: one row per non-empty group ---- */
	for (;;)
	{
		Datum		key;

		CHECK_FOR_INTERRUPTS();

		/*
		 * The key of the group we returned last time lives in pergroup, and
		 * the caller is done with it by now: a scan node's tuple is only
		 * guaranteed until its next call.
		 */
		ExecClearTuple(node->ss.ss_ScanTupleSlot);
		MemoryContextReset(st->pergroup);
		oldcxt = MemoryContextSwitchTo(st->pergroup);

		if (!rbi_entry_scan_next(&st->escan, &key, &st->groupset))
		{
			MemoryContextSwitchTo(oldcxt);
			st->done = true;
			return NULL;
		}

		count = rbi_count_sources(st->heap, estate->es_snapshot,
								  st->nclause + 1, st->sources, &st->stats);
		{
			bool		keyisnull = st->groupset.keyisnull;

			rbi_posting_set_release(&st->groupset);
			MemoryContextSwitchTo(oldcxt);

			/* A group exists only if at least one of its rows is visible. */
			if (count == 0)
				continue;

			return rbi_emit_tuple(st, key, keyisnull, count);
		}
	}
}

static void
rbi_rescan_custom_scan(CustomScanState *node)
{
	RBICountScanState *st = (RBICountScanState *) node;

	if (st->scanning)
	{
		rbi_entry_scan_end(&st->escan);
		st->scanning = false;
	}
	rbi_posting_set_release(&st->groupset);
	rbi_release_where(st);
	MemoryContextReset(st->pergroup);
	st->done = false;
}

static void
rbi_end_custom_scan(CustomScanState *node)
{
	RBICountScanState *st = (RBICountScanState *) node;
	int			i;

	if (st->scanning)
	{
		rbi_entry_scan_end(&st->escan);
		st->scanning = false;
	}
	rbi_posting_set_release(&st->groupset);
	rbi_release_where(st);

	if (st->groupidx != NULL)
	{
		index_close(st->groupidx, AccessShareLock);
		st->groupidx = NULL;
	}
	for (i = 0; i < st->nclause; i++)
	{
		if (st->clause[i].idx != NULL)
		{
			index_close(st->clause[i].idx, AccessShareLock);
			st->clause[i].idx = NULL;
		}
	}
	if (st->heap != NULL)
	{
		table_close(st->heap, NoLock);
		st->heap = NULL;
	}

	if (st->pergroup != NULL)
	{
		MemoryContextDelete(st->pergroup);
		st->pergroup = NULL;
	}
	if (st->wherecxt != NULL)
	{
		MemoryContextDelete(st->wherecxt);
		st->wherecxt = NULL;
	}
}

/*
 * "col = 3", "col = ANY ('{1,2,3}')", "col IS NULL", "col IS NOT NULL".
 */
static void
rbi_explain_clause(RBICountScanState *st, RBIClauseState *cl, StringInfo buf)
{
	const char *attname = get_attname(st->heapoid, cl->attno, false);

	switch (cl->kind)
	{
		case RBI_CLAUSE_NULL:
			appendStringInfo(buf, "%s IS NULL", attname);
			break;
		case RBI_CLAUSE_NOTNULL:
			appendStringInfo(buf, "%s IS NOT NULL", attname);
			break;
		default:
			{
				Oid			outfunc;
				bool		isvarlena;
				char	   *val;

				getTypeOutputInfo(cl->con->consttype, &outfunc, &isvarlena);
				val = OidOutputFunctionCall(outfunc, cl->con->constvalue);
				if (cl->kind == RBI_CLAUSE_ARRAY)
					appendStringInfo(buf, "%s = ANY (%s)", attname, val);
				else
					appendStringInfo(buf, "%s = %s", attname, val);
				pfree(val);
				break;
			}
	}
}

static void
rbi_explain_custom_scan(CustomScanState *node, List *ancestors,
						ExplainState *es)
{
	RBICountScanState *st = (RBICountScanState *) node;
	StringInfoData buf;
	int			i;

	initStringInfo(&buf);

	if (OidIsValid(st->groupidxoid))
	{
		appendStringInfoString(&buf, get_rel_name(st->groupidxoid));
		if (st->groupattno != 0)
			appendStringInfo(&buf, " (%s)",
							 get_attname(st->heapoid, st->groupattno, false));
		else
			appendStringInfoString(&buf, " (all keys)");
	}

	for (i = 0; i < st->nclause; i++)
	{
		if (buf.len > 0)
			appendStringInfoString(&buf, ", ");
		appendStringInfo(&buf, "%s (", get_rel_name(st->clause[i].idxoid));
		rbi_explain_clause(st, &st->clause[i], &buf);
		appendStringInfoChar(&buf, ')');
	}

	ExplainPropertyText("Roaring Indexes", buf.data, es);
	pfree(buf.data);

	if (OidIsValid(st->groupidxoid) && st->groupattno != 0)
		ExplainPropertyText("Group Key",
							get_attname(st->heapoid, st->groupattno, false),
							es);

	if (es->analyze)
	{
		ExplainPropertyInteger("Heap Blocks Skipped via VM", NULL,
							   st->stats.blocks_skipped_via_vm, es);
		ExplainPropertyInteger("Heap TIDs Rechecked", NULL,
							   st->stats.tids_rechecked, es);
		ExplainPropertyInteger("Heap Blocks Rechecked", NULL,
							   st->stats.blocks_rechecked, es);
		ExplainPropertyInteger("Containers Visited", NULL,
							   st->stats.containers_visited, es);
	}
}
