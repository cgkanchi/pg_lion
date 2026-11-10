/*-------------------------------------------------------------------------
 *
 * lion_minmax.c
 *		LionCountMinMax: count(*) and the other aggregates the node counts,
 *		beside min() and max() of a column, over one table's WHERE - the
 *		count from the node, each min or max from a one-row ordered walk
 *		(DESIGN.md §10, "A count beside min or max").
 *
 * Core answers min(ts) or max(ts) alone from an index - the first row of an
 * ordered scan under the WHERE (planagg.c) - but only when every aggregate
 * of the query is a min or a max.  Beside a count(*) the query is one Agg
 * over a scan that reads every row the WHERE selects.  The node answers
 *
 *		SELECT count(*), min(ts), max(ts) FROM t WHERE ...
 *
 * as the two halves would be answered alone: its one child is the count of
 * the other aggregates, which the count planner builds as for SELECT
 * count(*) FROM t WHERE ...; each min or max is an InitPlan, the subquery
 * planagg.c would build for it -
 *
 *		(SELECT ts FROM t WHERE ... AND ts IS NOT NULL ORDER BY ts LIMIT 1)
 *
 * - planned on its own, so that its path is whichever ordered walk is
 * cheapest: a btree, a LionOrdered.  Its output Param stands for the
 * aggregate, as for core's MinMaxAgg: root->minmax_aggs, set when the plan is
 * made, has setrefs.c put the Param wherever the aggregate is.
 *
 * Each part reads the same table under the same WHERE and snapshot, so the
 * answer is the Agg's: a clause that could answer differently twice - a
 * volatile one - declines, and so does anything a subquery could not carry:
 * a clause with a SubPlan or an outer query's Param, row-level security,
 * TABLESAMPLE.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_aggregate.h"
#include "executor/executor.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/subselect.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "lion_customscan.h"
#include "lion_count.h"

/*
 * Whether count(*) beside min() or max() may come from the node's count and
 * one-row walks (pg_lion.enable_count_minmax).
 */
bool		lion_enable_count_minmax = true;

static Plan *lion_plan_count_minmax_path(PlannerInfo *root, RelOptInfo *rel,
										 CustomPath *best_path, List *tlist,
										 List *clauses, List *custom_plans);
static Node *lion_create_count_minmax_state(CustomScan *cscan);
static void lion_begin_count_minmax(CustomScanState *node, EState *estate,
									int eflags);
static TupleTableSlot *lion_exec_count_minmax(CustomScanState *node);
static void lion_end_count_minmax(CustomScanState *node);
static void lion_rescan_count_minmax(CustomScanState *node);

const CustomPathMethods lion_count_minmax_path_methods = {
	.CustomName = "LionCountMinMax",
	.PlanCustomPath = lion_plan_count_minmax_path,
	.ReparameterizeCustomPathByChild = NULL,
};

const CustomScanMethods lion_count_minmax_scan_methods = {
	.CustomName = "LionCountMinMax",
	.CreateCustomScanState = lion_create_count_minmax_state,
};

static const CustomExecMethods lion_count_minmax_exec_methods = {
	.CustomName = "LionCountMinMax",
	.BeginCustomScan = lion_begin_count_minmax,
	.ExecCustomScan = lion_exec_count_minmax,
	.EndCustomScan = lion_end_count_minmax,
	.ReScanCustomScan = lion_rescan_count_minmax,
};

/*
 * The aggregates of a target: each min or max a walk answers, as core's
 * MinMaxAggInfo (once for each aggregate and argument), and every other one,
 * once.
 */
typedef struct LionMinMaxWalk
{
	List	   *minmax;
	List	   *others;
	bool		ok;
} LionMinMaxWalk;

/*
 * ---- planning ----
 */

/* The sort operator of a min or max aggregate, or InvalidOid. */
static Oid
lion_minmax_sortop(Oid aggfnoid)
{
	HeapTuple	tup;
	Oid			sortop;

	tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(aggfnoid));
	if (!HeapTupleIsValid(tup))
		return InvalidOid;
	sortop = ((Form_pg_aggregate) GETSTRUCT(tup))->aggsortop;
	ReleaseSysCache(tup);
	return sortop;
}

/*
 * Is agg a min or max an ordered walk answers, as can_minmax_aggs() in
 * planagg.c asks it: one argument, no ORDER BY or FILTER, nothing mutable in
 * the argument, which a walk could find in an index, and not a row, whose IS
 * NOT NULL means something else.
 */
static Oid
lion_minmax_agg_sortop(const Aggref *agg)
{
	Expr	   *arg;

	if (agg->aggkind != AGGKIND_NORMAL || agg->agglevelsup != 0 ||
		list_length(agg->args) != 1 || agg->aggorder != NIL ||
		agg->aggfilter != NULL)
		return InvalidOid;
	arg = ((TargetEntry *) linitial(agg->args))->expr;
	if (contain_mutable_functions((Node *) arg) ||
		type_is_rowtype(exprType((Node *) arg)))
		return InvalidOid;
	return lion_minmax_sortop(agg->aggfnoid);
}

static bool
lion_minmax_walker(Node *node, LionMinMaxWalk *w)
{
	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
	{
		Aggref	   *agg = (Aggref *) node;
		Oid			sortop = lion_minmax_agg_sortop(agg);
		Expr	   *arg;
		MinMaxAggInfo *mm;
		ListCell   *lc;

		if (agg->agglevelsup != 0)
		{
			w->ok = false;
			return true;
		}
		if (!OidIsValid(sortop))
		{
			if (!list_member(w->others, agg))
				w->others = lappend(w->others, agg);
			return false;
		}
		arg = ((TargetEntry *) linitial(agg->args))->expr;
		foreach(lc, w->minmax)
		{
			mm = (MinMaxAggInfo *) lfirst(lc);
			if (mm->aggfnoid == agg->aggfnoid && equal(mm->target, arg))
				return false;
		}
		mm = makeNode(MinMaxAggInfo);
		mm->aggfnoid = agg->aggfnoid;
		mm->aggsortop = sortop;
		mm->target = arg;
		w->minmax = lappend(w->minmax, mm);
		return false;
	}
	return expression_tree_walker(node, lion_minmax_walker, w);
}

/*
 * Can a clause of rel's WHERE be carried into a subquery of its own: no Var
 * but rel's own, no SubPlan, no PlaceHolderVar, and no Param but an
 * external one - a parameter of the statement, not of an outer query?
 */
static bool
lion_minmax_clause_walker(Node *node, Index *relid)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		return var->varno != (int) *relid || var->varlevelsup != 0;
	}
	if (IsA(node, Param))
		return ((Param *) node)->paramkind != PARAM_EXTERN;
	if (IsA(node, SubPlan) || IsA(node, AlternativeSubPlan) ||
		IsA(node, SubLink) || IsA(node, PlaceHolderVar))
		return true;
	return expression_tree_walker(node, lion_minmax_clause_walker, relid);
}

/*
 * The subquery planagg.c builds for mm, over rel and its WHERE (quals):
 *
 *		SELECT arg FROM t WHERE quals AND arg IS NOT NULL
 *		 ORDER BY arg USING sortop LIMIT 1
 *
 * planned as a subquery of root's - its own level, its own copy of rel's
 * range table entry - with the cheapest path to its one row in mm.  False
 * where it cannot be planned so.
 */
static bool
lion_minmax_plan(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte,
				 List *quals, MinMaxAggInfo *mm)
{
	Query	   *sub = makeNode(Query);
	RangeTblEntry *subrte = copyObject(rte);
	RangeTblRef *rtr = makeNode(RangeTblRef);
	Expr	   *arg = copyObject(mm->target);
	NullTest   *ntest = makeNode(NullTest);
	TargetEntry *tle;
	SortGroupClause *sortcl = makeNode(SortGroupClause);
	PlannerInfo *subroot;
	RelOptInfo *final_rel;
	Oid			eqop;
	bool		reverse;

	eqop = get_equality_op_for_ordering_op(mm->aggsortop, &reverse);
	if (!OidIsValid(eqop) || root->plan_params != NIL)
		return false;

	ChangeVarNodes((Node *) arg, rel->relid, 1, 0);
	quals = copyObject(quals);
	ChangeVarNodes((Node *) quals, rel->relid, 1, 0);
	ntest->nulltesttype = IS_NOT_NULL;
	ntest->arg = copyObject(arg);
	ntest->argisrow = false;
	ntest->location = -1;
	quals = lappend(quals, ntest);

	sub->commandType = CMD_SELECT;
	sub->querySource = QSRC_ORIGINAL;
	sub->canSetTag = true;
	sub->rtable = list_make1(subrte);
	if (rte->perminfoindex != 0)
	{
		RTEPermissionInfo *perminfo =
			getRTEPermissionInfo(root->parse->rteperminfos, rte);

		sub->rteperminfos = list_make1(copyObject(perminfo));
		subrte->perminfoindex = 1;
	}
	rtr->rtindex = 1;
	sub->jointree = makeFromExpr(list_make1(rtr),
								 (Node *) make_ands_explicit(quals));

	tle = makeTargetEntry(arg, 1, pstrdup("agg_target"), false);
	tle->ressortgroupref = 1;
	sub->targetList = list_make1(tle);

	/*
	 * NULLS FIRST for a reverse sort, as a btree read backward returns it, as
	 * planagg.c tries first: the walk skips the NULLs by the IS NOT NULL.
	 */
	sortcl->tleSortGroupRef = 1;
	sortcl->eqop = eqop;
	sortcl->sortop = mm->aggsortop;
#if PG_VERSION_NUM >= 180000
	sortcl->reverse_sort = reverse;
#endif
	sortcl->nulls_first = reverse;
	sortcl->hashable = false;
	sub->sortClause = list_make1(sortcl);
	sub->limitCount = (Node *) makeConst(INT8OID, -1, InvalidOid,
										 sizeof(int64), Int64GetDatum(1),
										 false, FLOAT8PASSBYVAL);
	sub->limitOption = LIMIT_OPTION_COUNT;

#if PG_VERSION_NUM >= 190000
	subroot = subquery_planner(root->glob, sub,
							   choose_plan_name(root->glob, "minmax", true),
							   root, NULL, false, 0.0, NULL);
#elif PG_VERSION_NUM >= 170000
	subroot = subquery_planner(root->glob, sub, root, false, 0.0, NULL);
#else
	subroot = subquery_planner(root->glob, sub, root, false, 0.0);
#endif
	if (root->plan_params != NIL)
		elog(ERROR, "LionCountMinMax: its subquery reads the query's own rows");

	final_rel = fetch_upper_rel(subroot, UPPERREL_FINAL, NULL);
	if (final_rel->cheapest_total_path == NULL)
		return false;
	mm->subroot = subroot;
	mm->path = final_rel->cheapest_total_path;
	mm->pathcost = mm->path->total_cost;
	mm->param = SS_make_initplan_output_param(root,
											  exprType((Node *) mm->target),
											  -1,
											  exprCollation((Node *) mm->target));
	return true;
}

/*
 * The one-row walks of a query of one table - an aggregate with no GROUP BY
 * whose aggregates are a min or max and something else - planned for
 * lion_try_count_minmax_path(): the min and max aggregates of the grouped
 * target, each with its subquery, as core's MinMaxAggInfo; NIL when the
 * query is not of that shape.  Each subquery is planned on its own, with
 * lion's planner hooks, so this runs before the hook that asks for it pins
 * its competitor or begins the endpoint probe (lion_create_upper_paths()).
 */
List *
lion_minmax_prepare(PlannerInfo *root, RelOptInfo *input_rel,
					RelOptInfo *grouped_rel)
{
	Query	   *parse = root->parse;
	RangeTblEntry *rte;
	LionMinMaxWalk w;
	List	   *quals = NIL;
	ListCell   *lc;

	if (!lion_enable_count_pushdown || !lion_enable_count_minmax)
		return NIL;
	if (parse->commandType != CMD_SELECT || parse->rowMarks != NIL ||
		root->rowMarks != NIL || !parse->hasAggs ||
		parse->groupClause != NIL || parse->groupingSets != NIL ||
		root->hasHavingQual || parse->hasTargetSRFs ||
		parse->hasWindowFuncs || parse->distinctClause != NIL)
		return NIL;
	if (input_rel->reloptkind != RELOPT_BASEREL || IS_DUMMY_REL(input_rel) ||
		input_rel->rtekind != RTE_RELATION)
		return NIL;
	rte = planner_rt_fetch(input_rel->relid, root);
	if (rte->securityQuals != NIL || rte->tablesample != NULL)
		return NIL;

	/* a min or max, and an aggregate that is neither */
	memset(&w, 0, sizeof(w));
	w.ok = true;
	(void) lion_minmax_walker((Node *) grouped_rel->reltarget->exprs, &w);
	if (!w.ok || w.minmax == NIL || w.others == NIL)
		return NIL;

	/*
	 * setrefs.c puts a walk's Param in place of every aggregate of its
	 * function and argument (find_minmax_agg_replacement_param()), FILTER,
	 * ORDER BY and all: core's MinMaxAgg has no other aggregate, and here no
	 * other may be one of those.
	 */
	foreach(lc, w.others)
	{
		Aggref	   *agg = (Aggref *) lfirst(lc);
		ListCell   *lm;

		if (list_length(agg->args) != 1)
			continue;
		foreach(lm, w.minmax)
		{
			MinMaxAggInfo *mm = (MinMaxAggInfo *) lfirst(lm);

			if (mm->aggfnoid == agg->aggfnoid &&
				equal(mm->target, ((TargetEntry *) linitial(agg->args))->expr))
				return NIL;
		}
	}

	/* the WHERE, as a subquery can carry it */
	foreach(lc, input_rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		if (rinfo->pseudoconstant || rinfo->security_level != 0 ||
			lion_minmax_clause_walker((Node *) rinfo->clause,
									  &input_rel->relid))
			return NIL;
		quals = lappend(quals, rinfo->clause);
	}
	if (contain_volatile_functions((Node *) quals))
		return NIL;

	foreach(lc, w.minmax)
	{
		if (!lion_minmax_plan(root, input_rel, rte, quals,
							  (MinMaxAggInfo *) lfirst(lc)))
			return NIL;
	}
	return w.minmax;
}

/*
 * UPPERREL_GROUP_AGG over input_rel, one base relation (DESIGN.md §10, "A
 * count beside min or max"): the LionCountMinMax node over the count of the
 * aggregates that are not a min or max, with an InitPlan for each that is,
 * minmax from lion_minmax_prepare().  Every failed check simply returns: the
 * ordinary plan is always there.
 */
void
lion_try_count_minmax_path(PlannerInfo *root, RelOptInfo *input_rel,
						   RelOptInfo *grouped_rel, List *minmax)
{
	PathTarget *target = grouped_rel->reltarget;
	LionMinMaxWalk w;
	CustomPath *cpath;
	Path	   *count;
	Cost		walks = 0;
	ListCell   *lc;

	if (minmax == NIL)
		return;

	memset(&w, 0, sizeof(w));
	w.ok = true;
	(void) lion_minmax_walker((Node *) target->exprs, &w);
	if (!w.ok || w.others == NIL)
		return;

	count = lion_count_child_path(root, input_rel, grouped_rel, w.others);
	if (count == NULL)
		return;
	foreach(lc, minmax)
		walks += ((MinMaxAggInfo *) lfirst(lc))->pathcost;

	cpath = makeNode(CustomPath);
	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = grouped_rel;
	cpath->path.pathtarget = target;
	cpath->path.param_info = NULL;
	cpath->path.parallel_aware = false;
	cpath->path.parallel_safe = false;
	cpath->path.parallel_workers = 0;
	cpath->path.pathkeys = NIL;
	cpath->path.rows = 1;
	cpath->path.startup_cost = count->total_cost + walks +
		target->cost.startup;
	cpath->path.total_cost = cpath->path.startup_cost +
		cpu_tuple_cost + target->cost.per_tuple;
#if PG_VERSION_NUM >= 180000
	cpath->path.disabled_nodes = count->disabled_nodes;
	foreach(lc, minmax)
		cpath->path.disabled_nodes +=
			((MinMaxAggInfo *) lfirst(lc))->path->disabled_nodes;
#endif
	cpath->flags = 0;
	cpath->custom_paths = list_make1(count);
#if PG_VERSION_NUM >= 170000
	cpath->custom_restrictinfo = NIL;
#endif
	cpath->custom_private = minmax;
	cpath->methods = &lion_count_minmax_path_methods;
	add_path(grouped_rel, &cpath->path);
}

/*
 * The plan: each walk's plan an InitPlan of the query, as
 * create_minmaxagg_plan() makes them, and root->minmax_aggs the walks, so
 * that setrefs.c makes each min or max aggregate its Param in every plan
 * node of the query.  The count child's aggregates are custom_scan_tlist,
 * which setrefs.c makes the target's other aggregates references to.
 * custom_private is the min and max aggregates, whose EXECUTE the node asks.
 */
static Plan *
lion_plan_count_minmax_path(PlannerInfo *root, RelOptInfo *rel,
							CustomPath *best_path, List *tlist,
							List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	Plan	   *count;
	List	   *ctlist = NIL;
	List	   *fnoids = NIL;
	ListCell   *lc;

	lion_count_scan_register();

	if (list_length(custom_plans) != 1)
		elog(ERROR, "LionCountMinMax: needs exactly one child plan");
	if (root->minmax_aggs != NIL)
		elog(ERROR, "LionCountMinMax: the query's min and max are already planned");

	foreach(lc, best_path->custom_private)
	{
		MinMaxAggInfo *mm = (MinMaxAggInfo *) lfirst(lc);
		Plan	   *plan = create_plan(mm->subroot, mm->path);

		SS_make_initplan_from_plan(root, mm->subroot, plan, mm->param);
		fnoids = list_append_unique_oid(fnoids, mm->aggfnoid);
	}
	root->minmax_aggs = best_path->custom_private;

	count = (Plan *) linitial(custom_plans);
	foreach(lc, count->targetlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		ctlist = lappend(ctlist,
						 makeTargetEntry((Expr *) copyObject(tle->expr),
										 list_length(ctlist) + 1,
										 NULL, false));
	}

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = NIL;
	cscan->scan.scanrelid = 0;
	cscan->flags = best_path->flags;
	cscan->custom_plans = custom_plans;
	cscan->custom_exprs = NIL;
	cscan->custom_private = fnoids;
	cscan->custom_scan_tlist = ctlist;
	cscan->custom_relids = rel->relids;
	cscan->methods = &lion_count_minmax_scan_methods;

	return &cscan->scan.plan;
}

/*
 * ---- execution ----
 */

/*
 * The executor's state: whether the count child's one row is held in the
 * scan slot, and whether this scan has returned it.
 */
typedef struct LionCountMinMaxState
{
	CustomScanState css;
	bool		held;
	bool		hasrow;
	bool		returned;
} LionCountMinMaxState;

static Node *
lion_create_count_minmax_state(CustomScan *cscan)
{
	LionCountMinMaxState *st = (LionCountMinMaxState *)
		newNode(sizeof(LionCountMinMaxState), T_CustomScanState);

	st->css.methods = &lion_count_minmax_exec_methods;
	return (Node *) st;
}

/*
 * The min and max aggregates are asked EXECUTE on as ExecInitAgg() would
 * have (DESIGN.md §9, "Privileges"); the count child asks it of its own.
 */
static void
lion_begin_count_minmax(CustomScanState *node, EState *estate, int eflags)
{
	LionCountMinMaxState *st = (LionCountMinMaxState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	ListCell   *lc;

	if (list_length(cscan->custom_plans) != 1)
		elog(ERROR, "LionCountMinMax: needs exactly one child plan");
	foreach(lc, cscan->custom_private)
		lion_check_aggregate_execute(lfirst_oid(lc));
	node->custom_ps = list_make1(ExecInitNode((Plan *)
											  linitial(cscan->custom_plans),
											  estate, eflags));
	st->held = false;
	st->hasrow = false;
	st->returned = false;
}

/*
 * The count child's one row - an aggregate with no GROUP BY has one - and
 * the target computed over it: each min or max is its InitPlan's Param,
 * which the first read of it runs.  The row is held for a rescan in which
 * no parameter of the child changed.
 */
static TupleTableSlot *
lion_exec_count_minmax(CustomScanState *node)
{
	LionCountMinMaxState *st = (LionCountMinMaxState *) node;
	TupleTableSlot *scan = node->ss.ss_ScanTupleSlot;
	ExprContext *econtext = node->ss.ps.ps_ExprContext;

	if (!st->held)
	{
		PlanState  *child = (PlanState *) linitial(node->custom_ps);
		TupleTableSlot *slot = ExecProcNode(child);

		st->hasrow = !TupIsNull(slot);
		if (st->hasrow)
		{
			ExecCopySlot(scan, slot);
			ExecMaterializeSlot(scan);
		}
		st->held = true;
	}
	if (st->returned || !st->hasrow)
		return NULL;
	st->returned = true;

	if (node->ss.ps.ps_ProjInfo == NULL)
		return scan;
	ResetExprContext(econtext);
	econtext->ecxt_scantuple = scan;
	return ExecProject(node->ss.ps.ps_ProjInfo);
}

static void
lion_end_count_minmax(CustomScanState *node)
{
	ExecEndNode((PlanState *) linitial(node->custom_ps));
}

/*
 * Core hands a changed parameter on to outer and inner plans but not to
 * custom_ps, so it is handed on here: the child is read again only when one
 * of its own changed, rescanned by its next ExecProcNode().  The InitPlans
 * are core's to run again.
 */
static void
lion_rescan_count_minmax(CustomScanState *node)
{
	LionCountMinMaxState *st = (LionCountMinMaxState *) node;
	PlanState  *child = (PlanState *) linitial(node->custom_ps);

	if (node->ss.ps.chgParam != NULL)
		UpdateChangedParamSet(child, node->ss.ps.chgParam);
	if (child->chgParam != NULL)
		st->held = false;
	st->returned = false;
}
