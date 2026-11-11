/*-------------------------------------------------------------------------
 *
 * lion_count_rows.c
 *		LionCountRows: the rows a LIMIT takes from rows that are all the same
 *		- a target of no column over one table's FROM and WHERE - as many as
 *		the node's count of them, up to the LIMIT (DESIGN.md §10, "A count
 *		capped by a LIMIT").
 *
 * A count that stops at a cap is written as a count over a subquery with a
 * LIMIT:
 *
 *		SELECT count(*) FROM (SELECT 1 FROM t WHERE ... LIMIT 10001) s
 *
 * The subquery is planned on its own, and core's plan for it is a LIMIT over
 * a scan of t, which reads every row the LIMIT takes, with the heap page each
 * is on, for the count above to count them.  Its target holds no column of t
 * - the outer query reads none, and core makes each it does not read a NULL
 * (remove_unused_subquery_outputs()) - so each of its rows is the same row,
 * and how many there are is the count of t under the WHERE, less the OFFSET,
 * up to the LIMIT.  The LionCountRows node, beside core's LIMIT at the final
 * rel of the subquery, has one child: the count of t under its WHERE, which
 * the count planner builds as for SELECT count(*) FROM t WHERE ... .  The
 * node runs it once and returns that many of the target's rows.
 *
 * The LIMIT and OFFSET are evaluated as nodeLimit.c evaluates them, before
 * anything is read: the same errors for a negative one, and no count at all
 * for LIMIT 0.
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
#include "utils/ruleutils.h"

#include "lion_customscan.h"
#include "lion_count.h"

/*
 * Whether the rows a LIMIT takes from a target of no column may come from
 * the node's count (pg_lion.enable_count_rows).
 */
bool		lion_enable_count_rows = true;

static Plan *lion_plan_count_rows_path(PlannerInfo *root, RelOptInfo *rel,
									   CustomPath *best_path, List *tlist,
									   List *clauses, List *custom_plans);
static Node *lion_create_count_rows_state(CustomScan *cscan);
static void lion_begin_count_rows(CustomScanState *node, EState *estate,
								  int eflags);
static TupleTableSlot *lion_exec_count_rows(CustomScanState *node);
static void lion_end_count_rows(CustomScanState *node);
static void lion_rescan_count_rows(CustomScanState *node);
static void lion_explain_count_rows(CustomScanState *node, List *ancestors,
									ExplainState *es);

const CustomPathMethods lion_count_rows_path_methods = {
	.CustomName = "LionCountRows",
	.PlanCustomPath = lion_plan_count_rows_path,
	.ReparameterizeCustomPathByChild = NULL,
};

const CustomScanMethods lion_count_rows_scan_methods = {
	.CustomName = "LionCountRows",
	.CreateCustomScanState = lion_create_count_rows_state,
};

static const CustomExecMethods lion_count_rows_exec_methods = {
	.CustomName = "LionCountRows",
	.BeginCustomScan = lion_begin_count_rows,
	.ExecCustomScan = lion_exec_count_rows,
	.EndCustomScan = lion_end_count_rows,
	.ReScanCustomScan = lion_rescan_count_rows,
	.ExplainCustomScan = lion_explain_count_rows,
};

/*
 * The executor's state: the OFFSET and LIMIT, either absent; the count, once
 * it is known; and the rows still to return, once the LIMIT is.
 */
typedef struct LionCountRowsState
{
	CustomScanState css;
	ExprState  *offset;
	ExprState  *limit;
	bool		counted;
	bool		started;
	int64		count;
	int64		left;
} LionCountRowsState;

/*
 * ---- planning ----
 */

/*
 * Is every column of target the same in every row: no column of the
 * relation, nothing volatile, no subquery?
 */
static bool
lion_count_rows_target_fixed(PathTarget *target)
{
	Node	   *exprs = (Node *) target->exprs;

	return !contain_var_clause(exprs) &&
		!contain_volatile_functions(exprs) &&
		!contain_subplans(exprs);
}

/*
 * UPPERREL_FINAL over input_rel, one base relation (DESIGN.md §10, "A count
 * capped by a LIMIT"): where the query is a LIMIT, OFFSET or both over the
 * rows of one table's FROM and WHERE, with a target that is the same in each
 * of them, the LionCountRows node over the count of those rows, beside core's
 * LIMIT over a scan.  Every failed check simply returns: the ordinary plan
 * is always there.
 */
void
lion_try_count_rows_path(PlannerInfo *root, RelOptInfo *input_rel,
						 RelOptInfo *final_rel, FinalPathExtraData *extra)
{
	Query	   *parse = root->parse;
	PathTarget *target = root->upper_targets[UPPERREL_FINAL];
	CustomPath *cpath;
	Path	   *count;
	double		rows;
	Cost		dummystart = 0;
	Cost		dummytotal = 0;

	if (!lion_enable_count_rows || !extra->limit_needed || target == NULL)
		return;

	/*
	 * A LIMIT directly over one table's FROM and WHERE: no grouping, window,
	 * DISTINCT or set operation below it, which would make input_rel an upper
	 * rel; no ORDER BY, row marks or set-returning function, and no WITH TIES,
	 * which choose among the rows or do something with them.
	 */
	if (parse->commandType != CMD_SELECT || parse->rowMarks != NIL ||
		root->rowMarks != NIL || parse->hasAggs ||
		parse->groupClause != NIL || parse->groupingSets != NIL ||
		root->hasHavingQual || parse->hasWindowFuncs ||
		parse->hasTargetSRFs || parse->distinctClause != NIL ||
		parse->sortClause != NIL || parse->setOperations != NULL ||
		parse->limitOption != LIMIT_OPTION_COUNT)
		return;
	if (input_rel->reloptkind != RELOPT_BASEREL || IS_DUMMY_REL(input_rel))
		return;

	/*
	 * Each row the LIMIT takes is the same row, which is only how many there
	 * are when no column of it reads the relation.  A volatile clause is
	 * evaluated for as many rows as the LIMIT needs by core's plan, and for
	 * every row by a count; so is a clause the count tests on the heap
	 * (DESIGN.md §10, "Residual clauses"), which here may only be one that
	 * raises no error - not `100 / w > 20`, whose division by zero may be in
	 * a row past the ones the LIMIT reads.
	 */
	if (!lion_count_rows_target_fixed(target))
		return;
	if (contain_volatile_functions((Node *) input_rel->baserestrictinfo))
		return;

	count = lion_count_child_path(root, input_rel, final_rel, NIL, true);
	if (count == NULL)
		return;

	/* the rows a LIMIT over the relation's rows is estimated to return */
	rows = input_rel->rows;
	adjust_limit_rows_costs(&rows, &dummystart, &dummytotal,
							extra->offset_est, extra->count_est);

	cpath = makeNode(CustomPath);
	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = final_rel;
	cpath->path.pathtarget = target;
	cpath->path.param_info = NULL;
	cpath->path.parallel_aware = false;
	cpath->path.parallel_safe = final_rel->consider_parallel &&
		count->parallel_safe;
	cpath->path.parallel_workers = 0;
	cpath->path.pathkeys = NIL;
	cpath->path.rows = rows;
	cpath->path.startup_cost = count->total_cost + target->cost.startup;
	cpath->path.total_cost = cpath->path.startup_cost +
		(cpu_tuple_cost + target->cost.per_tuple) * rows;
#if PG_VERSION_NUM >= 180000
	cpath->path.disabled_nodes = count->disabled_nodes;
#endif
	cpath->flags = 0;
	cpath->custom_paths = list_make1(count);
#if PG_VERSION_NUM >= 170000
	cpath->custom_restrictinfo = NIL;
#endif
	cpath->custom_private = list_make2(parse->limitOffset, parse->limitCount);
	cpath->methods = &lion_count_rows_path_methods;
	add_path(final_rel, &cpath->path);
}

/*
 * The plan: the target's columns, which read nothing below, and the OFFSET
 * and LIMIT that are there in custom_exprs, which setrefs.c finishes;
 * custom_private says which are.
 */
static Plan *
lion_plan_count_rows_path(PlannerInfo *root, RelOptInfo *rel,
						  CustomPath *best_path, List *tlist,
						  List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	Node	   *offset;
	Node	   *limit;
	List	   *exprs = NIL;

	lion_count_scan_register();

	if (list_length(custom_plans) != 1 ||
		list_length(best_path->custom_private) != 2)
		elog(ERROR, "LionCountRows: needs one child plan and its LIMIT");
	offset = (Node *) linitial(best_path->custom_private);
	limit = (Node *) lsecond(best_path->custom_private);
	if (offset != NULL)
		exprs = lappend(exprs, copyObject(offset));
	if (limit != NULL)
		exprs = lappend(exprs, copyObject(limit));

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = NIL;
	cscan->scan.scanrelid = 0;
	cscan->flags = best_path->flags;
	cscan->custom_plans = custom_plans;
	cscan->custom_exprs = exprs;
	cscan->custom_private = list_make2_int(offset != NULL, limit != NULL);
	cscan->custom_scan_tlist = NIL;
	cscan->custom_relids = rel->relids;
	cscan->methods = &lion_count_rows_scan_methods;

	return &cscan->scan.plan;
}

/*
 * ---- execution ----
 */

static Node *
lion_create_count_rows_state(CustomScan *cscan)
{
	LionCountRowsState *st = (LionCountRowsState *)
		newNode(sizeof(LionCountRowsState), T_CustomScanState);

	st->css.methods = &lion_count_rows_exec_methods;
	return (Node *) st;
}

/* Which of the OFFSET and LIMIT the plan has, in custom_exprs' order. */
static void
lion_count_rows_exprs(CustomScan *cscan, Expr **offset, Expr **limit)
{
	int			n = 0;

	*offset = NULL;
	*limit = NULL;
	if (list_length(cscan->custom_private) != 2)
		elog(ERROR, "LionCountRows: its LIMIT is not described");
	if (linitial_int(cscan->custom_private))
		*offset = (Expr *) list_nth(cscan->custom_exprs, n++);
	if (lsecond_int(cscan->custom_private))
		*limit = (Expr *) list_nth(cscan->custom_exprs, n++);
	if (n != list_length(cscan->custom_exprs))
		elog(ERROR, "LionCountRows: its LIMIT is not the one described");
}

static void
lion_begin_count_rows(CustomScanState *node, EState *estate, int eflags)
{
	LionCountRowsState *st = (LionCountRowsState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	Expr	   *offset;
	Expr	   *limit;

	if (list_length(cscan->custom_plans) != 1)
		elog(ERROR, "LionCountRows: needs exactly one child plan");
	lion_count_rows_exprs(cscan, &offset, &limit);
	st->offset = ExecInitExpr(offset, &node->ss.ps);
	st->limit = ExecInitExpr(limit, &node->ss.ps);
	node->custom_ps = list_make1(ExecInitNode((Plan *)
											  linitial(cscan->custom_plans),
											  estate, eflags));
	st->counted = false;
	st->started = false;
	st->count = 0;
	st->left = 0;
}

/*
 * The count, once: the count child returns one row of one int8 (none only
 * where nothing is counted, which is 0).
 */
static void
lion_count_rows_run(LionCountRowsState *st)
{
	PlanState  *countps = (PlanState *) linitial(st->css.custom_ps);
	TupleTableSlot *slot;

	st->count = 0;
	slot = ExecProcNode(countps);
	if (!TupIsNull(slot))
	{
		bool		isnull;
		Datum		d = slot_getattr(slot, 1, &isnull);

		if (!isnull)
			st->count = DatumGetInt64(d);
	}
	st->counted = true;
}

/*
 * The rows to return: the OFFSET and LIMIT, as recompute_limits() in
 * nodeLimit.c evaluates them - a NULL OFFSET is none and a NULL LIMIT is
 * ALL - and, unless the LIMIT is 0, the count.
 */
static void
lion_count_rows_start(LionCountRowsState *st)
{
	ExprContext *econtext = st->css.ss.ps.ps_ExprContext;
	int64		offset = 0;
	int64		limit = 0;
	bool		nolimit = true;
	int64		left;
	Datum		val;
	bool		isnull;

	if (st->offset != NULL)
	{
		val = ExecEvalExprSwitchContext(st->offset, econtext, &isnull);
		if (!isnull)
		{
			offset = DatumGetInt64(val);
			if (offset < 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_ROW_COUNT_IN_RESULT_OFFSET_CLAUSE),
						 errmsg("OFFSET must not be negative")));
		}
	}
	if (st->limit != NULL)
	{
		val = ExecEvalExprSwitchContext(st->limit, econtext, &isnull);
		if (!isnull)
		{
			limit = DatumGetInt64(val);
			if (limit < 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_ROW_COUNT_IN_LIMIT_CLAUSE),
						 errmsg("LIMIT must not be negative")));
			nolimit = false;
		}
	}

	st->started = true;
	st->left = 0;
	if (!nolimit && limit == 0)
		return;
	if (!st->counted)
		lion_count_rows_run(st);
	left = st->count - offset;
	if (left < 0)
		left = 0;
	if (!nolimit && left > limit)
		left = limit;
	st->left = left;
}

static TupleTableSlot *
lion_exec_count_rows(CustomScanState *node)
{
	LionCountRowsState *st = (LionCountRowsState *) node;
	TupleTableSlot *scan = node->ss.ss_ScanTupleSlot;
	ExprContext *econtext = node->ss.ps.ps_ExprContext;

	CHECK_FOR_INTERRUPTS();
	if (!st->started)
		lion_count_rows_start(st);
	if (st->left <= 0)
		return NULL;
	st->left--;

	/* the scan tuple has no column: the target reads none */
	ExecClearTuple(scan);
	ExecStoreVirtualTuple(scan);
	if (node->ss.ps.ps_ProjInfo == NULL)
		return scan;
	ResetExprContext(econtext);
	econtext->ecxt_scantuple = scan;
	return ExecProject(node->ss.ps.ps_ProjInfo);
}

static void
lion_end_count_rows(CustomScanState *node)
{
	ExecEndNode((PlanState *) linitial(node->custom_ps));
}

/*
 * The OFFSET and LIMIT are evaluated again, as a Limit's are; the count is
 * made again only when a parameter of its child changed, the child then
 * rescanned by its next ExecProcNode().  Core hands a changed parameter on
 * to outer and inner plans but not to custom_ps, so it is handed on here.
 */
static void
lion_rescan_count_rows(CustomScanState *node)
{
	LionCountRowsState *st = (LionCountRowsState *) node;
	PlanState  *countps = (PlanState *) linitial(node->custom_ps);

	if (node->ss.ps.chgParam != NULL)
		UpdateChangedParamSet(countps, node->ss.ps.chgParam);
	if (countps->chgParam != NULL)
		st->counted = false;
	st->started = false;
	st->left = 0;
}

/* The OFFSET and LIMIT the count is cut to. */
static void
lion_explain_count_rows(CustomScanState *node, List *ancestors,
						ExplainState *es)
{
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	Expr	   *offset;
	Expr	   *limit;
	List	   *context;

	lion_count_rows_exprs(cscan, &offset, &limit);
	context = set_deparse_context_plan(es->deparse_cxt, &cscan->scan.plan,
									   ancestors);
	ExplainPropertyText("Rows Returned",
						"the rows counted, less the OFFSET, up to the LIMIT", es);
	if (offset != NULL)
		ExplainPropertyText("Offset",
							deparse_expression((Node *) offset, context,
											   false, false), es);
	if (limit != NULL)
		ExplainPropertyText("Limit",
							deparse_expression((Node *) limit, context,
											   false, false), es);
}
