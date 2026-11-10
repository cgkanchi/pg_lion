/*-------------------------------------------------------------------------
 *
 * lion_window.c
 *		LionWindowCount: count(*) OVER () - the number of rows the query
 *		returns, beside each of them - from the node's count of the same
 *		relation and WHERE (DESIGN.md §10, "A count of every row beside the
 *		rows").
 *
 * A count over a window with no PARTITION BY and no ORDER BY, whose frame
 * holds every row, is one number for all of them: the rows the query's FROM
 * and WHERE select.  Core's WindowAgg reads every one of those rows into a
 * tuplestore before it returns the first, so a page of them -
 *
 *		SELECT *, count(*) OVER () FROM t WHERE ... ORDER BY ts DESC LIMIT 20
 *
 * - reads all of them whatever the LIMIT takes.  The LionWindowCount node has
 * two children: a count of the relation under its WHERE, which the count
 * planner builds as for SELECT count(*) FROM t WHERE ... and the node runs
 * once, before its first row; and a path of the relation's own rows, any of
 * them, whose rows it hands up in their order with the count beside each.
 * Under a LIMIT it reads the count and as many rows as the LIMIT takes.
 *
 * The count is the number of rows the second child returns because both are
 * the same relation and clauses read under the statement's snapshot: the
 * count node's answer is exact wherever a count is (DESIGN.md §9, §11), and
 * a clause that could answer differently twice - a volatile one - declines.
 *
 * In the plan, the window functions are a count(*) aggregate the node's
 * custom_scan_tlist names last, which setrefs.c matches them against, so
 * that EXPLAIN prints them as what the node computes: no WindowAgg is left
 * to name a window after.  The nodes above take the node's columns by
 * position (a Sort, a Limit), which is why the query's target must reach
 * them as the node returns it: no projection above, no DISTINCT and no ORDER
 * BY of a window function, which would look for the window function in the
 * node's target by its expression.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "executor/executor.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/tlist.h"
#include "utils/fmgroids.h"

#include "lion_customscan.h"
#include "lion_count.h"

/*
 * Whether count(*) OVER () beside the rows may come from the node's count
 * (pg_lion.enable_window_count).
 */
bool		lion_enable_window_count = true;

static Plan *lion_plan_window_count_path(PlannerInfo *root, RelOptInfo *rel,
										 CustomPath *best_path, List *tlist,
										 List *clauses, List *custom_plans);
static Node *lion_create_window_count_state(CustomScan *cscan);
static void lion_begin_window_count(CustomScanState *node, EState *estate,
									int eflags);
static TupleTableSlot *lion_exec_window_count(CustomScanState *node);
static void lion_end_window_count(CustomScanState *node);
static void lion_rescan_window_count(CustomScanState *node);

const CustomPathMethods lion_window_count_path_methods = {
	.CustomName = "LionWindowCount",
	.PlanCustomPath = lion_plan_window_count_path,
	.ReparameterizeCustomPathByChild = NULL,
};

const CustomScanMethods lion_window_count_scan_methods = {
	.CustomName = "LionWindowCount",
	.CreateCustomScanState = lion_create_window_count_state,
};

static const CustomExecMethods lion_window_count_exec_methods = {
	.CustomName = "LionWindowCount",
	.BeginCustomScan = lion_begin_window_count,
	.ExecCustomScan = lion_exec_window_count,
	.EndCustomScan = lion_end_window_count,
	.ReScanCustomScan = lion_rescan_window_count,
};

/*
 * The executor's state: the count, once it is known, and how many columns of
 * the rows' child come before it in the scan tuple.
 */
typedef struct LionWindowCountState
{
	CustomScanState css;
	bool		counted;
	Datum		count;
	int			nrowcols;
} LionWindowCountState;

/*
 * The window functions of a target: each one's function, once.
 */
typedef struct LionWindowWalk
{
	Query	   *parse;
	List	   *fnoids;
	bool		ok;
} LionWindowWalk;

/*
 * ---- planning ----
 */

/* The window clause winref names. */
static WindowClause *
lion_window_clause(Query *parse, Index winref)
{
	ListCell   *lc;

	foreach(lc, parse->windowClause)
	{
		WindowClause *wc = lfirst_node(WindowClause, lc);

		if (wc->winref == winref)
			return wc;
	}
	return NULL;
}

/*
 * Does the frame of wc hold every row of its partition for every row, and is
 * the partition every row - no PARTITION BY, no ORDER BY?  Without an ORDER
 * BY every row is a peer of every other: a RANGE frame from or to CURRENT
 * ROW reaches every peer, so any RANGE frame without offsets (which need an
 * ORDER BY) is the whole partition; a ROWS frame is only from UNBOUNDED
 * PRECEDING to UNBOUNDED FOLLOWING.  GROUPS needs an ORDER BY too.  An
 * EXCLUDE takes rows out.
 */
static bool
lion_window_frame_whole(const WindowClause *wc)
{
	int			fo = wc->frameOptions;

	if (wc->partitionClause != NIL || wc->orderClause != NIL)
		return false;
	if ((fo & FRAMEOPTION_EXCLUSION) != 0)
		return false;
#if PG_VERSION_NUM < 170000
	/* a run condition stops the window early (16 keeps it on the clause) */
	if (wc->runCondition != NIL)
		return false;
#endif
	if ((fo & FRAMEOPTION_ROWS) != 0)
		return (fo & FRAMEOPTION_START_UNBOUNDED_PRECEDING) != 0 &&
			(fo & FRAMEOPTION_END_UNBOUNDED_FOLLOWING) != 0;
	if ((fo & FRAMEOPTION_RANGE) != 0)
		return (fo & (FRAMEOPTION_START_OFFSET | FRAMEOPTION_END_OFFSET)) == 0;
	return false;
}

/*
 * Is wf a count of every row: count(*), or count(c) of a constant that is not
 * NULL, with no FILTER?
 */
static bool
lion_window_is_count(const WindowFunc *wf)
{
	if (!wf->winagg || wf->aggfilter != NULL || wf->wintype != INT8OID)
		return false;
#if PG_VERSION_NUM >= 170000
	if (wf->runCondition != NIL)
		return false;
#endif
	if (wf->winfnoid == F_COUNT_)
		return wf->args == NIL;
	if (wf->winfnoid == F_COUNT_ANY && list_length(wf->args) == 1)
	{
		Node	   *arg = (Node *) linitial(wf->args);

		return IsA(arg, Const) && !((Const *) arg)->constisnull;
	}
	return false;
}

/*
 * Every window function in node, each a count of every row of the whole
 * result (lion_window_is_count(), lion_window_frame_whole()); w->ok false at
 * the first that is not.
 */
static bool
lion_window_walker(Node *node, LionWindowWalk *w)
{
	if (node == NULL)
		return false;
	if (IsA(node, WindowFunc))
	{
		WindowFunc *wf = (WindowFunc *) node;
		WindowClause *wc = lion_window_clause(w->parse, wf->winref);

		if (wc == NULL || !lion_window_is_count(wf) ||
			!lion_window_frame_whole(wc))
		{
			w->ok = false;
			return true;
		}
		w->fnoids = list_append_unique_oid(w->fnoids, wf->winfnoid);
		return false;
	}
	return expression_tree_walker(node, lion_window_walker, w);
}

/* The count(*) the node's count child computes, and the windows become. */
static Aggref *
lion_window_count_agg(void)
{
	Aggref	   *n = makeNode(Aggref);

	n->aggfnoid = F_COUNT_;
	n->aggtype = INT8OID;
	n->aggtranstype = INT8OID;
	n->aggstar = true;
	n->aggkind = AGGKIND_NORMAL;
	n->aggsplit = AGGSPLIT_SIMPLE;
	n->location = -1;
	return n;
}

/*
 * The count of input_rel under its WHERE - or aggs, aggregates the node
 * computes from it, when not NIL - counted into a rel of its own whose target
 * is that count, as lion_try_count_path() plans SELECT count(*) FROM t WHERE
 * ... (the grouping sets do the same, lion_plan_gsets.c): the query is made
 * that one while it plans, and put back.  Its paths are priced in the units
 * pinned for upper_rel (DESIGN.md §39), whose competitor - a WindowAgg over
 * the relation, a LIMIT over a scan of it, an Agg - they meet.  The cheapest,
 * or NULL when the node does not count the relation.  For a node of whatever
 * stage puts the count beside something else: LionWindowCount here,
 * LionCountRows (lion_count_rows.c) and LionCountMinMax (lion_minmax.c).
 */
Path *
lion_count_child_path(PlannerInfo *root, RelOptInfo *input_rel,
					  RelOptInfo *upper_rel, List *aggs)
{
	Query	   *parse = root->parse;
	RelOptInfo *inner;
	PathTarget *target;
	GroupPathExtraData extra;
	List	   *tlist = NIL;
	bool		savedaggs = parse->hasAggs;
	bool		savedwindows = parse->hasWindowFuncs;
	List	   *savedsort = parse->sortClause;
	Node	   *savedlimit = parse->limitCount;
	Node	   *savedoffset = parse->limitOffset;
	List	   *savedtlist = root->processed_tlist;
	RelOptInfo *pinned;
	Path	   *best = NULL;
	ListCell   *lc;

	if (aggs == NIL)
		aggs = list_make1(lion_window_count_agg());
	target = create_empty_pathtarget();
	foreach(lc, aggs)
	{
		Expr	   *agg = (Expr *) lfirst(lc);

		add_column_to_pathtarget(target, agg, 0);
		tlist = lappend(tlist, makeTargetEntry(agg, list_length(tlist) + 1,
											   NULL, false));
	}
	set_pathtarget_cost_width(root, target);

	inner = makeNode(RelOptInfo);
	memcpy(inner, upper_rel, sizeof(RelOptInfo));
	inner->pathlist = NIL;
	inner->ppilist = NIL;
	inner->partial_pathlist = NIL;
	inner->cheapest_startup_path = NULL;
	inner->cheapest_total_path = NULL;
	inner->cheapest_parameterized_paths = NIL;
	inner->reltarget = target;

	memset(&extra, 0, sizeof(extra));
	extra.flags = GROUPING_CAN_PARTIAL_AGG;
	extra.patype = PARTITIONWISE_AGGREGATE_NONE;
	extra.havingQual = NULL;
	extra.targetList = tlist;
	extra.target_parallel_safe = true;

	pinned = lion_units_pin_move(inner);
	parse->hasAggs = true;
	parse->hasWindowFuncs = false;
	parse->sortClause = NIL;
	parse->limitCount = NULL;
	parse->limitOffset = NULL;
	root->processed_tlist = tlist;
	PG_TRY();
	{
		lion_try_count_path(root, input_rel, inner, &extra, NULL);
	}
	PG_FINALLY();
	{
		parse->hasAggs = savedaggs;
		parse->hasWindowFuncs = savedwindows;
		parse->sortClause = savedsort;
		parse->limitCount = savedlimit;
		parse->limitOffset = savedoffset;
		root->processed_tlist = savedtlist;
		(void) lion_units_pin_move(pinned);
	}
	PG_END_TRY();

	foreach(lc, inner->pathlist)
	{
		Path	   *p = (Path *) lfirst(lc);

		if (p->param_info == NULL &&
			(best == NULL || p->total_cost < best->total_cost))
			best = p;
	}
	return best;
}

/*
 * The node over rows, one of the relation's paths, and count, the count
 * child: rows' order and rows, each with the count beside it.  The count is
 * run in full before the first row, so it is in the startup cost.
 */
static Path *
lion_window_count_wrap(RelOptInfo *window_rel, PathTarget *target,
					   Path *count, Path *rows, List *fnoids)
{
	CustomPath *cpath = makeNode(CustomPath);

	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = window_rel;
	cpath->path.pathtarget = target;
	cpath->path.param_info = NULL;
	cpath->path.parallel_aware = false;
	cpath->path.parallel_safe = window_rel->consider_parallel &&
		count->parallel_safe && rows->parallel_safe;
	cpath->path.parallel_workers = 0;
	cpath->path.pathkeys = rows->pathkeys;
	cpath->path.rows = rows->rows;
	cpath->path.startup_cost = count->total_cost + rows->startup_cost +
		target->cost.startup;
	cpath->path.total_cost = count->total_cost + rows->total_cost +
		target->cost.startup +
		(cpu_tuple_cost + target->cost.per_tuple) * rows->rows;
#if PG_VERSION_NUM >= 180000
	cpath->path.disabled_nodes = count->disabled_nodes + rows->disabled_nodes;
#endif
	cpath->flags = 0;
	cpath->custom_paths = list_make2(count, rows);
#if PG_VERSION_NUM >= 170000
	cpath->custom_restrictinfo = NIL;
#endif
	cpath->custom_private = fnoids;
	cpath->methods = &lion_window_count_path_methods;
	return &cpath->path;
}

#if PG_VERSION_NUM < 170000
/*
 * PostgreSQL 16 prices a WindowAgg's first row as its input's first row
 * (cost_windowagg()), though a window over the whole result returns nothing
 * until it has read every row; under a LIMIT the WindowAgg then looks as if
 * it stopped after a few rows, and the node, whose count runs before its
 * first row, never wins.  17 prices the first row after the rows the window
 * reads first (get_windowclause_startup_tuples()), which for these windows
 * is every row.  So, when the node is offered, core's WindowAgg paths for
 * these windows are priced as 17 prices them: a lower WindowAgg's first row
 * first, then this one's input rows but the first.
 */
static void
lion_window_price_first_row(WindowAggPath *wp)
{
	Path	   *in = wp->subpath;
	double		n = in->rows;

	if (IsA(in, WindowAggPath))
	{
		Cost		before = in->startup_cost;

		lion_window_price_first_row((WindowAggPath *) in);
		wp->path.startup_cost += in->startup_cost - before;
	}
	if (n > 1.0)
		wp->path.startup_cost += (wp->path.total_cost - wp->path.startup_cost) /
			n * (n - 1.0);
}
#endif

/*
 * UPPERREL_WINDOW over input_rel, one base relation (DESIGN.md §10, "A count
 * of every row beside the rows"): where every window function of the query
 * is a count of every row of the result, the LionWindowCount node over each
 * of the relation's paths, beside core's WindowAgg over the same.  Every
 * failed check simply returns: the ordinary plan is always there.
 */
void
lion_try_window_count_path(PlannerInfo *root, RelOptInfo *input_rel,
						   RelOptInfo *window_rel)
{
	Query	   *parse = root->parse;
	PathTarget *target = NULL;
	List	   *finalexprs = NIL;
	LionWindowWalk w;
	Path	   *count;
	ListCell   *lc;

	if (!lion_enable_window_count)
		return;

	/*
	 * The windows over the rows of one table's FROM and WHERE: no grouping
	 * below them, which makes input_rel a grouped rel; no row marks, no
	 * set-returning function or DISTINCT above, which would read the windows'
	 * results in a node of their own.
	 */
	if (parse->commandType != CMD_SELECT || parse->rowMarks != NIL ||
		root->rowMarks != NIL || parse->hasAggs ||
		parse->groupClause != NIL || parse->groupingSets != NIL ||
		root->hasHavingQual || parse->hasTargetSRFs ||
		parse->distinctClause != NIL)
		return;
	if (input_rel->reloptkind != RELOPT_BASEREL || IS_DUMMY_REL(input_rel))
		return;

	/*
	 * The count and the rows must see the same rows: a clause that could
	 * answer differently when evaluated again does not let them.
	 */
	if (contain_volatile_functions((Node *) input_rel->baserestrictinfo))
		return;

	/*
	 * The windows' target, which the WindowAgg paths core has just made
	 * return.  It must be the query's own: a target list of postponed
	 * expressions is finished by a projection above the windows, which
	 * finds the window functions it reads in the node's target by their
	 * expression, and the node's target holds a count(*) in their place.
	 */
	foreach(lc, window_rel->pathlist)
	{
		Path	   *p = (Path *) lfirst(lc);

		if (IsA(p, WindowAggPath))
		{
			target = p->pathtarget;
			break;
		}
	}
	if (target == NULL)
		return;
	foreach(lc, root->processed_tlist)
		finalexprs = lappend(finalexprs, lfirst_node(TargetEntry, lc)->expr);
	if (!equal(finalexprs, target->exprs))
		return;

	/*
	 * ... and no ORDER BY of a window function: the Sort above finds its key
	 * in the node's target by its expression too.  Every row has the same
	 * count, so such a key orders nothing anyway.
	 */
	foreach(lc, parse->sortClause)
	{
		SortGroupClause *sgc = lfirst_node(SortGroupClause, lc);
		TargetEntry *tle = get_sortgroupclause_tle(sgc, root->processed_tlist);

		if (contain_window_function((Node *) tle->expr))
			return;
	}

	/* ... every window function in it a count of every row */
	memset(&w, 0, sizeof(w));
	w.parse = parse;
	w.ok = true;
	(void) lion_window_walker((Node *) target->exprs, &w);
	if (!w.ok || w.fnoids == NIL)
		return;

	count = lion_count_child_path(root, input_rel, window_rel, NIL);
	if (count == NULL)
		return;

#if PG_VERSION_NUM < 170000
	foreach(lc, window_rel->pathlist)
	{
		Path	   *p = (Path *) lfirst(lc);

		if (IsA(p, WindowAggPath))
			lion_window_price_first_row((WindowAggPath *) p);
	}
#endif

	/* ... beside each of the relation's paths, in its order */
	foreach(lc, input_rel->pathlist)
	{
		Path	   *rows = (Path *) lfirst(lc);

		if (rows->param_info != NULL)
			continue;
		add_path(window_rel, lion_window_count_wrap(window_rel, target, count,
													rows, w.fnoids));
	}
}

/* The window functions of a target, as the count(*) n. */
static Node *
lion_window_replace(Node *node, Aggref *n)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, WindowFunc))
		return (Node *) copyObject(n);
	return expression_tree_mutator(node, lion_window_replace, n);
}

/*
 * The plan: the rows' child's columns, as its target list has them, and the
 * count last make custom_scan_tlist; the target list is the query's with
 * each window function the count(*) there, which setrefs.c makes a reference
 * to it, as it makes the columns references to theirs.  The node's
 * projection computes whatever else the target holds from them.
 */
static Plan *
lion_plan_window_count_path(PlannerInfo *root, RelOptInfo *rel,
							CustomPath *best_path, List *tlist,
							List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	Plan	   *rows;
	List	   *ctlist = NIL;
	Aggref	   *n = lion_window_count_agg();
	ListCell   *lc;

	lion_count_scan_register();

	if (list_length(custom_plans) != 2)
		elog(ERROR, "LionWindowCount: needs exactly two child plans");
	rows = (Plan *) lsecond(custom_plans);
	foreach(lc, rows->targetlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		ctlist = lappend(ctlist,
						 makeTargetEntry((Expr *) copyObject(tle->expr),
										 list_length(ctlist) + 1,
										 NULL, false));
	}
	ctlist = lappend(ctlist,
					 makeTargetEntry((Expr *) n, list_length(ctlist) + 1,
									 NULL, false));

	cscan->scan.plan.targetlist = (List *) lion_window_replace((Node *) tlist,
															   n);
	cscan->scan.plan.qual = NIL;
	cscan->scan.scanrelid = 0;
	cscan->flags = best_path->flags;
	cscan->custom_plans = custom_plans;
	cscan->custom_exprs = NIL;
	cscan->custom_private = best_path->custom_private;
	cscan->custom_scan_tlist = ctlist;
	cscan->custom_relids = rel->relids;
	cscan->methods = &lion_window_count_scan_methods;

	return &cscan->scan.plan;
}

/*
 * ---- execution ----
 */

static Node *
lion_create_window_count_state(CustomScan *cscan)
{
	LionWindowCountState *st = (LionWindowCountState *)
		newNode(sizeof(LionWindowCountState), T_CustomScanState);

	st->css.methods = &lion_window_count_exec_methods;
	return (Node *) st;
}

/*
 * The window functions are asked EXECUTE on as ExecInitWindowAgg() would
 * have (DESIGN.md §9, "Privileges"): the count child asks it of count(*),
 * the query may have written count(1).
 */
static void
lion_begin_window_count(CustomScanState *node, EState *estate, int eflags)
{
	LionWindowCountState *st = (LionWindowCountState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	PlanState  *countps;
	PlanState  *rowps;
	ListCell   *lc;

	if (list_length(cscan->custom_plans) != 2)
		elog(ERROR, "LionWindowCount: needs exactly two child plans");
	foreach(lc, cscan->custom_private)
		lion_check_window_execute(lfirst_oid(lc));

	countps = ExecInitNode((Plan *) linitial(cscan->custom_plans), estate,
						   eflags);
	rowps = ExecInitNode((Plan *) lsecond(cscan->custom_plans), estate,
						 eflags);
	node->custom_ps = list_make2(countps, rowps);
	st->counted = false;
	st->count = (Datum) 0;
	st->nrowcols = list_length(cscan->custom_scan_tlist) - 1;
	if (st->nrowcols < 0 ||
		node->ss.ss_ScanTupleSlot->tts_tupleDescriptor->natts !=
		st->nrowcols + 1)
		elog(ERROR, "LionWindowCount: its scan tuple is not its children's");
}

/*
 * The count, once, before the first row: the count child returns one row of
 * one int8 (none only where nothing is counted, which is 0).
 */
static void
lion_window_count_run(LionWindowCountState *st)
{
	PlanState  *countps = (PlanState *) linitial(st->css.custom_ps);
	TupleTableSlot *slot;
	int64		count = 0;
	MemoryContext oldcxt;

	slot = ExecProcNode(countps);
	if (!TupIsNull(slot))
	{
		bool		isnull;
		Datum		d = slot_getattr(slot, 1, &isnull);

		if (!isnull)
			count = DatumGetInt64(d);
	}
	oldcxt = MemoryContextSwitchTo(st->css.ss.ps.state->es_query_cxt);
	st->count = Int64GetDatum(count);
	MemoryContextSwitchTo(oldcxt);
	st->counted = true;
}

static TupleTableSlot *
lion_exec_window_count(CustomScanState *node)
{
	LionWindowCountState *st = (LionWindowCountState *) node;
	PlanState  *rowps = (PlanState *) lsecond(node->custom_ps);
	TupleTableSlot *scan = node->ss.ss_ScanTupleSlot;
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	TupleTableSlot *slot;
	int			i;

	CHECK_FOR_INTERRUPTS();
	if (!st->counted)
		lion_window_count_run(st);

	slot = ExecProcNode(rowps);
	if (TupIsNull(slot))
		return NULL;

	slot_getallattrs(slot);
	ExecClearTuple(scan);
	for (i = 0; i < st->nrowcols; i++)
	{
		scan->tts_values[i] = slot->tts_values[i];
		scan->tts_isnull[i] = slot->tts_isnull[i];
	}
	scan->tts_values[i] = st->count;
	scan->tts_isnull[i] = false;
	ExecStoreVirtualTuple(scan);

	if (node->ss.ps.ps_ProjInfo == NULL)
		return scan;
	ResetExprContext(econtext);
	econtext->ecxt_scantuple = scan;
	return ExecProject(node->ss.ps.ps_ProjInfo);
}

static void
lion_end_window_count(CustomScanState *node)
{
	ExecEndNode((PlanState *) linitial(node->custom_ps));
	ExecEndNode((PlanState *) lsecond(node->custom_ps));
}

/*
 * Core hands a changed parameter on to outer and inner plans but not to
 * custom_ps, so it is handed on here.  The count is made again only when a
 * parameter of its child changed; a child whose parameters changed is
 * rescanned by its next ExecProcNode().
 */
static void
lion_rescan_window_count(CustomScanState *node)
{
	LionWindowCountState *st = (LionWindowCountState *) node;
	PlanState  *countps = (PlanState *) linitial(node->custom_ps);
	PlanState  *rowps = (PlanState *) lsecond(node->custom_ps);

	if (node->ss.ps.chgParam != NULL)
	{
		UpdateChangedParamSet(countps, node->ss.ps.chgParam);
		UpdateChangedParamSet(rowps, node->ss.ps.chgParam);
	}
	if (countps->chgParam != NULL)
		st->counted = false;
	if (rowps->chgParam == NULL)
		ExecReScan(rowps);
}
