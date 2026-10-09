/*-------------------------------------------------------------------------
 *
 * lion_plan_count.c
 *		Building the LionCount path of an aggregate over one relation
 *		(lion_try_count_path()), and its parallel GROUP BY path.
 *
 * Part of the LionCount custom scan: lion_customscan.h describes the node
 * and declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_customscan.h"
#include "lion_plan_private.h"

/*
 * A PARALLEL GROUP BY (DESIGN.md §10, "A GROUP BY in parallel"): the serial
 * node `serial` made parallel-aware, below a Gather and core's Finalize
 * HashAggregate.  Its participants divide the heap's container keys into
 * ranges, a few each (lion_key_ranges()), and count every group in each range
 * they claim against the WHERE collected for that range: a partial count a
 * group a range, which the Finalize Agg adds up - and applies the HAVING to,
 * which the node's partial target leaves to it.
 *
 * WORKERS: what a parallel scan of the index pages the walk reads - the
 * pages of the grouping column's posting sets - would get, capped by
 * max_parallel_workers_per_gather; a parallel_workers setting on the table
 * decides alone, as for core's scans.
 *
 * COST, one participant's: its share of the serial walk (`serialrun`, the
 * serial node's price before its HAVING) - the collection and the groups'
 * containers divide by key, the pages each range reads being its own - plus
 * what each of its ranges pays again (rc->perrange: the entry walk, each
 * group's set set up and sought to the range, each WHERE source sought
 * there), plus a cpu_tuple_cost a partial row.  The rows are a group's per
 * range it has rows in, at most the WHERE's rows.  The Gather prices its
 * rows and set-up, core's Finalize Agg its own work.
 */
static void
lion_add_parallel_group_path(PlannerInfo *root, RelOptInfo *rel,
							 RelOptInfo *output_rel, CustomPath *serial,
							 Cost serialrun, const LionRangeCost *rc,
							 IndexOptInfo *groupidx, AttrNumber groupcol,
							 List *having, double numgroups)
{
	PathTarget *partialtarget;
	CustomPath *cpath;
	LionCountPriv priv;
	Path	   *gather;
	AggClauseCosts agg_final_costs;
	double		pages;
	double		divisor;
	double		rows;
	double		totalrows;
	uint32		ckeys;
	int			workers;
	int			nranges;

	pages = Max(1.0, (double) groupidx->pages *
				lion_index_column_posting_share(root, rel, groupidx,
												groupcol));
	workers = compute_parallel_worker(rel, -1, pages,
									  max_parallel_workers_per_gather);
	if (workers <= 0)
		return;

	divisor = lion_parallel_divisor(workers);
	nranges = lion_key_ranges(rel->pages, workers + 1,
							  LION_PARALLEL_RANGE_KEYS, &ckeys);
	totalrows = clamp_row_est(Min(numgroups * nranges,
								  Max(rel->rows, numgroups)));
	rows = clamp_row_est(totalrows / divisor);

	partialtarget = lion_make_partial_target(root, output_rel->reltarget,
											 having);

	cpath = makeNode(CustomPath);
	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = output_rel;
	cpath->path.pathtarget = partialtarget;
	cpath->path.param_info = NULL;
	cpath->path.parallel_aware = true;
	cpath->path.parallel_safe = true;
	cpath->path.parallel_workers = workers;
	cpath->path.pathkeys = NIL;
	cpath->path.rows = rows;
	cpath->path.startup_cost = 0;
	cpath->path.total_cost = serialrun / divisor +
		((double) nranges / divisor) * rc->perrange + rows * cpu_tuple_cost;
#if PG_VERSION_NUM >= 180000
	cpath->path.disabled_nodes = 0;
#endif
	cpath->flags = serial->flags;
	cpath->custom_paths = NIL;
#if PG_VERSION_NUM >= 170000
	cpath->custom_restrictinfo = NIL;
#endif
	/* the serial node's plan, but for the HAVING, which is the Agg's here */
	lion_count_priv_decode(serial->custom_private, LION_PRIV_STAGE_PATH,
						   &priv);
	priv.having = NIL;
	cpath->custom_private = lion_count_priv_encode(&priv,
												   LION_PRIV_STAGE_PATH);
	cpath->methods = serial->methods;

	gather = (Path *) create_gather_path(root, output_rel, &cpath->path,
										 partialtarget, NULL, &totalrows);

	MemSet(&agg_final_costs, 0, sizeof(agg_final_costs));
	get_agg_clause_costs(root, AGGSPLIT_FINAL_DESERIAL, &agg_final_costs);
	add_path(output_rel, (Path *)
			 create_agg_path(root, output_rel, gather,
							 output_rel->reltarget,
							 AGG_HASHED, AGGSPLIT_FINAL_DESERIAL,
							 root->processed_groupClause,
							 having,
							 &agg_final_costs,
							 numgroups));
}

/*
 * Turn the RANGE clauses of every range column but `keep` (an index into
 * rangepos, or -1 for none) into bounds of ranges taken as sources
 * (DESIGN.md §32): LION_CLAUSE_RANGESRC in the clause lists, which is all the
 * executor goes by.
 */
static void
lion_ranges_to_sources(List *rangepos, int keep, List *wherekinds,
					   List *clauseinfos)
{
	int			r = 0;
	ListCell   *lc;

	foreach(lc, rangepos)
	{
		ListCell   *lp;

		if (r++ == keep)
			continue;
		foreach(lp, (List *) lfirst(lc))
		{
			int			pos = lfirst_int(lp);

			lfirst_int(list_nth_cell(wherekinds, pos)) = LION_CLAUSE_RANGESRC;
			((LionClauseInfo *) list_nth(clauseinfos, pos))->kind =
				LION_CLAUSE_RANGESRC;
		}
	}
}

/*
 * What lion_try_count_path() works out in one of its phases and uses in a
 * later one: its arguments - input_rel being the FACT rel of an FK-side join
 * from lion_count_path_query() on - and what each phase finds, every member
 * starting as lion_count_path_init() sets it.
 */
typedef struct LionCountPathBuild
{
	PlannerInfo *root;
	RelOptInfo *input_rel;
	RelOptInfo *output_rel;
	GroupPathExtraData *extra;
	const LionFkJoin *fj;
	Query	   *parse;
	RangeTblEntry *rte;
	Index		rti;
	Var		   *groupvar[LION_MAX_GROUPCOLS];
	AttrNumber	groupattno[LION_MAX_GROUPCOLS];
	Oid			groupeqop[LION_MAX_GROUPCOLS];
	bool		groupvalueout[LION_MAX_GROUPCOLS];
	double		groupest[LION_MAX_GROUPCOLS];
	int			ngroup;
	bool		decode;			/* three or more columns: the decoded walk
								 * of DESIGN.md §34 counts them */
	AttrNumber	driveattno;		/* column whose index drives the entry scan */
	bool		singlegroup;
	bool		sumall;
	bool		partitioned;
	List	   *valueattnos;	/* pinned columns the output prints */
	PathTarget *partialtarget;	/* set for a partitioned GROUP BY */
	List	   *whereattnos;	/* its column, in the PARENT's numbering */
	List	   *clauseinfos;	/* LionClauseInfo, one per clause */
	List	   *whereclauses;	/* the clause, for selectivity */
	List	   *whereconsts;	/* its value expression - a Const, a Param,
								 * or a NULL placeholder for a null test */
	List	   *wherekinds;		/* LION_CLAUSE_* */
	List	   *whereopnos;		/* the clause's operator (0 for a null test) */
	List	   *whereinor;		/* 1 when the clause is a leaf of an OR */
	List	   *ors;			/* one IntList per OR restriction (§19) */
	List	   *rinfoclauses;	/* each restriction's clause, by position */
	List	   *impliedtexts;	/* the ones left out altogether (§16) */
	int			rinfono;		/* ... and the one being analysed */
	LionImply	imply;			/* what a partition's bounds may leave out */
	List	   *posattnos;		/* columns with a positive clause */
	List	   *eqattnos;		/* columns pinned to one value */
	List	   *nonnullattnos;	/* columns a clause proves non-null */
	List	   *nullattnos;		/* columns a clause pins to NULL */
	List	   *targets;		/* LionCountTarget, one per counted relation */
	LionCountTarget *first;
	Var		   *notnullvar;		/* the first `IS NOT NULL` column */
	Var		   *allvar;			/* a sum over every row drives by it (§35) */
	Var		   *rangevar;		/* the column the RANGE clauses bound (§28) */
	List	   *rangeclauses;	/* ... and those clauses' RestrictInfos */
	List	   *rangevars;		/* every column a range bounds (§28, §32) */
	List	   *rangecls;		/* ... its clauses' RestrictInfos, a List
								 * per column */
	List	   *rangepos;		/* ... and their positions in the clause
								 * lists, an IntList per column */
	bool		hasrangesrc;	/* a range is a source (§32) */
	bool		pinnedsrc;		/* a source outside ranges that holds the
								 * §9 pin: a clause, or an OR of them */
	Selectivity rangesel;		/* the share of its entries they select */
	LionCountPriv priv;			/* the path's custom_private */
	double		numgroups;
	double		outrows;
	bool		havepositive;
	List	   *having;
	List	   *checkexprs;
	Var		   *distvar;		/* the column count(DISTINCT) counts (§26) */
	Oid			disteqop;
	Oid			distcoll;
	bool		distcounts;		/* its walk must count, not test (§26) */
	double		distest;
	Const	   *groupcoal;		/* GROUP BY coalesce(col, c): c (§10) */
	Node	   *groupexpr;		/* ... and the whole expression */
	bool		plainpositive;	/* a positive clause outside ORs that
								 * is positive whatever its value: a
								 * WHERE a parallel GROUP BY can
								 * collect by ranges (§10) */
	List	   *groupdeps;		/* GROUP BY expressions of the columns
								 * (§36), which split no group */
	List	   *wattnos;		/* §37: the columns aggregates are taken
								 * over, weighted by their entries' rows */
	List	   *widx;			/* ... their indexes (IndexOptInfo) */
	List	   *wcols;			/* ... and key columns there */
	List	   *wnaggs;			/* ... and how many aggregates each */
	bool		wcounts;		/* the counts of the target list besides */
	int64		topkn;			/* the top k by count (§36), or 0 */
	int			topkcand;		/* ... the candidates its walk keeps */
	double		topkrows;		/* ... and the rows of their entries */
	bool		topkstrict;		/* ... every group tied with the k-th */
	List	   *vcols;			/* the expression columns (DESIGN.md §41) */
} LionCountPathBuild;

static void lion_try_count_path_scoped(LionCountPathBuild *cxp);

/*
 * Start the build of lion_try_count_path(): its arguments, and every other
 * member at its starting value - zero, NIL or NULL unless set here.
 */
static void
lion_count_path_init(LionCountPathBuild *cx, PlannerInfo *root,
					 RelOptInfo *input_rel, RelOptInfo *output_rel,
					 GroupPathExtraData *extra, const LionFkJoin *fj)
{
	memset(cx, 0, sizeof(LionCountPathBuild));
	cx->root = root;
	cx->input_rel = input_rel;
	cx->output_rel = output_rel;
	cx->extra = extra;
	cx->fj = fj;
	cx->parse = root->parse;
	cx->groupvar[0] = NULL;
	cx->groupvar[1] = NULL;
	cx->groupattno[0] = 0;
	cx->groupattno[1] = 0;
	cx->groupeqop[0] = InvalidOid;
	cx->groupeqop[1] = InvalidOid;
	cx->groupvalueout[0] = false;
	cx->groupvalueout[1] = false;
	cx->groupest[0] = 1.0;
	cx->groupest[1] = 1.0;
	cx->ngroup = 0;
	cx->driveattno = 0;
	cx->singlegroup = false;
	cx->sumall = false;
	cx->partitioned = false;
	cx->valueattnos = NIL;
	cx->partialtarget = NULL;
	cx->whereattnos = NIL;
	cx->clauseinfos = NIL;
	cx->whereclauses = NIL;
	cx->whereconsts = NIL;
	cx->wherekinds = NIL;
	cx->whereopnos = NIL;
	cx->whereinor = NIL;
	cx->ors = NIL;
	cx->rinfoclauses = NIL;
	cx->impliedtexts = NIL;
	cx->rinfono = -1;
	cx->posattnos = NIL;
	cx->eqattnos = NIL;
	cx->nonnullattnos = NIL;
	cx->nullattnos = NIL;
	cx->targets = NIL;
	cx->notnullvar = NULL;
	cx->allvar = NULL;
	cx->rangevar = NULL;
	cx->rangeclauses = NIL;
	cx->rangevars = NIL;
	cx->rangecls = NIL;
	cx->rangepos = NIL;
	cx->hasrangesrc = false;
	cx->pinnedsrc = false;
	cx->rangesel = 1.0;
	cx->havepositive = false;
	cx->distvar = NULL;
	cx->disteqop = InvalidOid;
	cx->distcoll = InvalidOid;
	cx->distcounts = false;
	cx->distest = 0;
	cx->groupcoal = NULL;
	cx->groupexpr = NULL;
	cx->plainpositive = false;
	cx->vcols = NIL;
}

/*
 * The query as a whole: a SELECT the node may replace.  Sets the HAVING the
 * node is to apply, and - for the FK-side join - makes input_rel the fact
 * rel, which everything after this asks about.
 */
static bool
lion_count_path_query(LionCountPathBuild *cx)
{
	PlannerInfo *root = cx->root;
	Query	   *parse = cx->parse;
	GroupPathExtraData *extra = cx->extra;
	const LionFkJoin *fj = cx->fj;

	/*
	 * ---- the query as a whole ----
	 *
	 * A semi or anti join path (fj->joinpath, DESIGN.md §27, "The semi and
	 * anti join as a join path") is a join rel's rows, whatever the query
	 * does with them above - but in a SELECT only, and with no row marks:
	 * a row the statement locks or modifies would have its EvalPlanQual
	 * recheck made through a join the node replaces, which it does not do.
	 */
	if (parse->commandType != CMD_SELECT)
		return false;
	if (parse->rowMarks != NIL || root->rowMarks != NIL)
		return false;
	if (fj == NULL || !fj->joinpath)
	{
		if (!parse->hasAggs)
			return false;
		if (parse->groupingSets != NIL)
			return false;
		if (parse->hasWindowFuncs || parse->hasTargetSRFs ||
			parse->hasDistinctOn || parse->distinctClause != NIL)
			return false;
	}

	/*
	 * HAVING (DESIGN.md §10).  By now it is an implicit-AND list of the
	 * clauses that mention an aggregate (or are volatile, or contain a
	 * subquery): the planner has already moved every other clause into WHERE
	 * (subquery_planner()).  The node knows each group's count before it
	 * emits the group, so a HAVING over the counts it computes and the
	 * columns it can print is a filter on its output - checked below against
	 * the same rules as the target list, and applied by the node itself or,
	 * for a partitioned table, by the Finalize Agg above it.
	 */
	cx->having = (extra != NULL) ? (List *) extra->havingQual :
		(List *) parse->havingQual;
	if (fj != NULL && fj->joinpath)
		cx->having = NIL;		/* a join path's rows are no groups */

	/* The FK-side join counts the fact rel's posting sets (DESIGN.md §27). */
	if (fj != NULL)
		cx->input_rel = fj->factrel;

	return true;
}

/*
 * The relation: one base table or materialized view with indexes, or one
 * partitioned parent.  Sets rti, rte and partitioned.
 */
static bool
lion_count_path_rel(LionCountPathBuild *cx)
{
	PlannerInfo *root = cx->root;
	RelOptInfo *input_rel = cx->input_rel;
	GroupPathExtraData *extra = cx->extra;

	/* ---- a single base relation: one table, or one partitioned parent ---- */
	if (input_rel->reloptkind != RELOPT_BASEREL)
		return false;
	if (bms_membership(input_rel->relids) != BMS_SINGLETON)
		return false;
	cx->rti = input_rel->relid;
	if (cx->rti == 0 || cx->rti >= (Index) root->simple_rel_array_size)
		return false;
	cx->rte = root->simple_rte_array[cx->rti];
	if (cx->rte == NULL || cx->rte->rtekind != RTE_RELATION)
		return false;
	if (cx->rte->securityQuals != NIL || cx->rte->tablesample != NULL)
		return false;
	if (IS_DUMMY_REL(input_rel))
		return false;			/* the planner has already proved it empty */

	if (cx->rte->inh)
	{
		/*
		 * A partitioned parent (DESIGN.md §16).  The parent itself has no
		 * storage and no index list; every live leaf partition is counted in
		 * turn, with its own heap and its own indexes.  Old-style inheritance
		 * parents are not handled: their children are not required to have
		 * the parent's columns at all.
		 */
		if (cx->rte->relkind != RELKIND_PARTITIONED_TABLE)
			return false;
		if (input_rel->part_scheme == NULL || !IS_PARTITIONED_REL(input_rel))
			return false;

		/*
		 * With partitionwise aggregation the planner builds its own per-child
		 * grouping paths; ours would only compete on cost, and the interaction
		 * is untested, so stay out of the way.  This applies to partitioned
		 * parents only: grouping_planner() sets patype whenever the GUC is on,
		 * before it knows whether the input is partitioned at all, so testing
		 * it earlier would switch the pushdown off for plain tables too.
		 */
		if (extra != NULL && extra->patype != PARTITIONWISE_AGGREGATE_NONE)
			return false;
		cx->partitioned = true;
	}
	else
	{
		if (cx->rte->relkind != RELKIND_RELATION &&
			cx->rte->relkind != RELKIND_MATVIEW)
			return false;
		if (input_rel->indexlist == NIL)
			return false;
	}
	return true;
}

/*
 * Is expr an expression of the GROUP BY's columns alone, which splits none of
 * their groups (DESIGN.md §36): `GROUP BY ip, ip - 1, ip - 2`?  It has to be
 * immutable - the same value for the same input - and made of nothing but the
 * grouping columns of this table and constants: no aggregate, window
 * function, set-returning function, subquery or parameter.  Every row of a
 * group then gives it the same value provided the rows' column values are
 * the same, not merely equal - which is what printing the column from the
 * stored key requires as well (lion_index_can_emit_value()), so each column
 * it names is one the node prints (groupvalueout).  The node emits the
 * columns, and the plan's projection computes the expression from them.
 */
static bool
lion_group_dependent(LionCountPathBuild *cx, Node *expr)
{
	List	   *vars;
	ListCell   *lc;

	if (contain_mutable_functions(expr) || contain_volatile_functions(expr) ||
		contain_agg_clause(expr) || contain_window_function(expr) ||
		expression_returns_set(expr) || contain_subplans(expr) ||
		lion_contains_param(expr))
		return false;
	vars = pull_var_clause(expr, PVC_RECURSE_PLACEHOLDERS);
	if (vars == NIL)
		return false;
	foreach(lc, vars)
	{
		Var		   *v = (Var *) lfirst(lc);
		int			g;

		if (!IsA(v, Var) || v->varno != (int) cx->rti ||
			v->varlevelsup != 0)
			return false;
		for (g = 0; g < cx->ngroup; g++)
		{
			if (cx->groupattno[g] == v->varattno)
				break;
		}
		if (g == cx->ngroup)
			return false;
		cx->groupvalueout[g] = true;
	}
	return true;
}

/*
 * GROUP BY: nothing, one indexed column, or two.  Fills in the grouping
 * columns (groupvar and the arrays beside it, ngroup), the coalesce group, or
 * singlegroup for a GROUP BY the planner folded to constants.
 */
static bool
lion_count_path_group_by(LionCountPathBuild *cx)
{
	PlannerInfo *root = cx->root;
	Query	   *parse = cx->parse;
	GroupPathExtraData *extra = cx->extra;
	const LionFkJoin *fj = cx->fj;
	Index		rti = cx->rti;
	ListCell   *lc;

	/*
	 * ---- GROUP BY: nothing, one indexed column, or two (DESIGN.md §20) ----
	 *
	 * Two columns are a nested loop over the two indexes' entries, so every
	 * rule below is made per column: its own index, its own collation, its
	 * own grouping equality, its own value-representation gate.
	 *
	 * None of it applies to the FK-side join (DESIGN.md §27), whose groups are
	 * the dimension's and are formed by the Finalize Agg above the node.
	 */
	int			nvar = 0;

	/*
	 * The grouping clauses that are columns: an expression of them adds no
	 * group, and the walk takes only the columns (lion_group_dependent()).
	 */
	foreach(lc, root->processed_groupClause)
	{
		SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
		TargetEntry *tle = get_sortgroupclause_tle(sgc, root->processed_tlist);
		Node	   *gexpr = (tle != NULL) ?
			lion_vcol_subst((Node *) tle->expr) : NULL;

		if (gexpr != NULL &&
			(IsA(gexpr, CoalesceExpr) ||
			 (lion_strip(gexpr) != NULL && IsA(lion_strip(gexpr), Var))))
			nvar++;
	}
	if (fj == NULL && nvar > LION_MAX_GROUPCOLS)
		return false;

	if (root->processed_groupClause == NIL)
	{
		/*
		 * Every GROUP BY column was proved constant by the planner (it does
		 * that for a column with an equality qual against anything that is
		 * not a Var - a literal or a parameter), so the query has one group - but, unlike a plain aggregate, it
		 * must produce no row at all when nothing matches.
		 */
		cx->singlegroup = (parse->groupClause != NIL);
	}
	else if (fj == NULL)
	{
		foreach(lc, root->processed_groupClause)
		{
			SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
			TargetEntry *tle = get_sortgroupclause_tle(sgc,
													   root->processed_tlist);
			Node	   *gexpr;
			Node	   *expr;
			int			i;

			if (tle == NULL)
				return false;

			/* an expression column is grouped as a column (DESIGN.md §41) */
			gexpr = lion_vcol_subst((Node *) tle->expr);

			/*
			 * `GROUP BY coalesce(col, c)` is col's groups with the NULL group
			 * relabelled c - and merged into c's own group when col has that
			 * value (DESIGN.md §10, "coalesce").  Only that one grouping
			 * column, and only in the plain form, where the node's entry walk
			 * can do the merge exactly (lion_group_coalesce()).
			 */
			if (IsA(gexpr, CoalesceExpr))
			{
				if (list_length(root->processed_groupClause) != 1 ||
					!lion_group_coalesce((CoalesceExpr *) gexpr, rti,
										 &cx->groupvar[cx->ngroup],
										 &cx->groupcoal) ||
					LION_ATTNO_IS_VCOL(cx->groupvar[cx->ngroup]->varattno))
					return false;
				cx->groupexpr = gexpr;
				expr = (Node *) cx->groupvar[cx->ngroup];
			}
			else
				expr = lion_strip(gexpr);
			if (expr == NULL)
				return false;

			/*
			 * An expression of the grouping columns (DESIGN.md §36, `GROUP BY
			 * ip, ip - 1`), taken once every column is known.
			 */
			if (!IsA(expr, Var))
			{
				cx->groupdeps = lappend(cx->groupdeps, gexpr);
				continue;
			}
			cx->groupvar[cx->ngroup] = (Var *) expr;
			if (cx->groupvar[cx->ngroup]->varno != (int) rti ||
				cx->groupvar[cx->ngroup]->varattno <= 0 ||
				cx->groupvar[cx->ngroup]->varlevelsup != 0)
				return false;

			/*
			 * A nullable group column is fine now: NULL keys have an entry of
			 * their own, so the NULL group is produced like any other
			 * (DESIGN.md §14).
			 */
			cx->groupattno[cx->ngroup] = cx->groupvar[cx->ngroup]->varattno;

			/* The same column twice is not a grouping this node can drive. */
			for (i = 0; i < cx->ngroup; i++)
			{
				if (cx->groupattno[i] == cx->groupattno[cx->ngroup])
					return false;
			}

			/*
			 * The equality the planner chose for this column is what the index
			 * that drives the scan has to implement, whether there is one
			 * relation or many (lion_collect_targets(), finding 3 of the
			 * 2026-09-20 review), so a grouping clause without one is of no use
			 * here.
			 */
			cx->groupeqop[cx->ngroup] = sgc->eqop;
			if (!OidIsValid(cx->groupeqop[cx->ngroup]))
				return false;

			/*
			 * A partitioned GROUP BY emits one PARTIAL aggregate per group per
			 * partition and lets core's Finalize HashAggregate combine them
			 * (DESIGN.md §16), so the column has to be hashable and the planner
			 * has to consider the aggregates splittable at all.  count(*) and
			 * count(col) always are, but the answer is the planner's to give.
			 * One table needs neither: it streams its finished groups.
			 */
			if (cx->partitioned &&
				(!sgc->hashable || extra == NULL ||
				 (extra->flags & GROUPING_CAN_PARTIAL_AGG) == 0))
				return false;

			/*
			 * Three or more columns are the decoded walk of DESIGN.md §34,
			 * which emits partial counts too - a combination's rows may come
			 * out in more than one row, when a pass's tally spills - for a
			 * Finalize Agg to add up: the same two requirements.  It counts
			 * one table, and is not made on a hot standby, where no WHERE is
			 * collected.
			 */
			if (nvar > 2 &&
				(!lion_enable_decoded_walk ||
				 cx->partitioned || !sgc->hashable || extra == NULL ||
				 (extra->flags & GROUPING_CAN_PARTIAL_AGG) == 0 ||
				 RecoveryInProgress()))
				return false;

			cx->ngroup++;
		}
		cx->decode = (cx->ngroup > 2);

		/*
		 * The expressions of the grouping columns split no group: only in
		 * one table, which emits its finished groups, and with a column to
		 * be an expression of.
		 */
		foreach(lc, cx->groupdeps)
		{
			if (cx->ngroup == 0 || cx->partitioned || cx->decode ||
				!lion_group_dependent(cx, (Node *) lfirst(lc)))
				return false;
		}
	}
	return true;
}

/*
 * A WHERE restriction that is an OR (lion_count_path_where()), taken as one
 * source.  False when no posting set answers it, with the leaves of any arms
 * before the one that failed already appended to the clause lists.
 */
static bool
lion_count_path_where_or(LionCountPathBuild *cx, Node *clause)
{
	PlannerInfo *root = cx->root;
	Index		rti = cx->rti;
	LionLeafInfo leaf;
	List	   *arms;
	List	   *armlens = NIL;
	int			orfirst = list_length(cx->whereattnos);
	bool		thisrange = false;
	ListCell   *la;

	/*
	 * ---- an OR across columns (DESIGN.md §19) ----
	 *
	 * Every arm has to be a positive clause the posting sets can answer,
	 * or an AND of such clauses, each on a column of this relation.  The
	 * whole restriction then becomes ONE source - the union of the arms -
	 * which is ANDed with the other sources and with the GROUP BY driver
	 * like any other.  A negated arm (`IS NOT NULL`, a NOT of anything
	 * but a boolean column) is declined: the complement of a posting set
	 * is not a posting set, and under a union there is nothing to
	 * subtract it from.  An arm `flag IS NOT TRUE` is two arms of the
	 * same union, `flag = false` and `flag IS NULL`, and an AND arm with
	 * an OR inside - `flag IS NOT TRUE` among its terms, or any other -
	 * is distributed into the arms it stands for (lion_or_arms()).
	 *
	 * The leaves are appended to the clause array like plain clauses, so
	 * that an index is matched for each of them per partition and a Param
	 * among them reaches custom_exprs; what marks them out is that they
	 * are contiguous and named by an entry in `ors`.  They constrain no
	 * column of the RESULT, so none of the attnum bookkeeping below
	 * (posattnos, eqattnos, nonnullattnos, nullattnos) takes them: the
	 * other arm may hold rows where this arm's column is NULL, or is
	 * anything at all.
	 */
	if (list_length(((BoolExpr *) clause)->args) < 2)
		return false;

	arms = lion_or_arms(clause);
	if (arms == NIL)
		return false;

	foreach(la, arms)
	{
		List	   *arm = (List *) lfirst(la);
		ListCell   *lb;

		foreach(lb, arm)
		{
			if (!lion_analyze_leaf(root, (Node *) lfirst(lb), rti,
								  false, true, false, &leaf))
				return false;

			/*
			 * A range in an arm is a leaf of the union like any other:
			 * the rows whose key lies in it, collected (DESIGN.md §32).
			 */
			if (leaf.kind == LION_CLAUSE_RANGE)
			{
				leaf.kind = LION_CLAUSE_RANGESRC;
				thisrange = true;
			}
			lion_append_clause(&leaf, (Node *) lfirst(lb), true,
							  &cx->whereattnos, &cx->clauseinfos,
							  &cx->whereclauses, &cx->whereconsts,
							  &cx->wherekinds, &cx->whereopnos,
							  &cx->whereinor);
			((LionClauseInfo *) llast(cx->clauseinfos))->rinfono = cx->rinfono;
		}
		armlens = lappend_int(armlens, list_length(arm));
	}

	cx->ors = lappend(cx->ors,
					  list_concat(list_make2_int(orfirst,
												 list_length(armlens)),
								  armlens));
	cx->havepositive = true;

	/*
	 * A union holds the §9 pin at every container key only when every
	 * leaf does, which a range collected into memory does not
	 * (DESIGN.md §32).
	 */
	if (thisrange)
		cx->hasrangesrc = true;
	else
		cx->pinnedsrc = true;
	return true;
}

/*
 * A WHERE restriction that is no OR (lion_count_path_where()): a range
 * comparison, gathered per column, or a clause the posting sets answer, with
 * what it says of its column noted.  False when no posting set answers it;
 * true, too, for the very same positive clause again, which is dropped.
 */
static bool
lion_count_path_where_leaf(LionCountPathBuild *cx, RestrictInfo *rinfo,
						   Node *clause)
{
	PlannerInfo *root = cx->root;
	Index		rti = cx->rti;
	LionLeafInfo leaf;

	if (!lion_analyze_leaf(root, clause, rti, true, true, true, &leaf))
		return false;

	/*
	 * A range comparison (DESIGN.md §28) bounds the entry walk that DRIVES
	 * the count - or, on a column that does not drive it, is a bound of a
	 * range taken as a source (§32) - which is decided once everything
	 * else is known.  Any number of them may name one column: they are
	 * ANDed into one range, and they are gathered per column here.  A
	 * strict comparison is never true of NULL, so the column is non-NULL
	 * in every row counted.
	 */
	if (leaf.kind == LION_CLAUSE_RANGE)
	{
		int			pos = list_length(cx->whereattnos);
		int			r = 0;
		ListCell   *lr;

		foreach(lr, cx->rangevars)
		{
			if (((Var *) lfirst(lr))->varattno == leaf.var->varattno)
				break;
			r++;
		}
		if (lr == NULL)
		{
			cx->rangevars = lappend(cx->rangevars, leaf.var);
			cx->rangecls = lappend(cx->rangecls, list_make1(rinfo));
			cx->rangepos = lappend(cx->rangepos, list_make1_int(pos));
		}
		else
		{
			ListCell   *c1 = list_nth_cell(cx->rangecls, r);
			ListCell   *c2 = list_nth_cell(cx->rangepos, r);

			lfirst(c1) = lappend((List *) lfirst(c1), rinfo);
			lfirst(c2) = lappend_int((List *) lfirst(c2), pos);
		}
		cx->nonnullattnos = lappend_int(cx->nonnullattnos,
										(int) leaf.var->varattno);
		lion_append_clause(&leaf, clause, false,
						  &cx->whereattnos, &cx->clauseinfos,
						  &cx->whereclauses, &cx->whereconsts,
						  &cx->wherekinds, &cx->whereopnos,
						  &cx->whereinor);
		((LionClauseInfo *) llast(cx->clauseinfos))->rinfono = cx->rinfono;
		return true;
	}

	/*
	 * Which index answers the clause, whether its opfamily has the
	 * operator as strategy 1, and whether it can hash and compare the
	 * constant's type is settled per relation, in lion_match_index():
	 * with partitions there is one index per partition and they need not
	 * share an opclass (DESIGN.md §16).
	 */

	if (LION_CLAUSE_IS_POSITIVE(leaf.kind))
	{
		cx->havepositive = true;
		cx->pinnedsrc = true;
	}

	/*
	 * A multi-key clause whose value is only known at run time may turn
	 * out to narrow nothing, and be counted as a negated source (§17);
	 * every other positive clause stays one.
	 */
	if (leaf.kind == LION_CLAUSE_EQ || leaf.kind == LION_CLAUSE_ARRAY ||
		leaf.kind == LION_CLAUSE_NULL ||
		(leaf.kind == LION_CLAUSE_MULTI && leaf.val != NULL &&
		 IsA(leaf.val, Const)))
		cx->plainpositive = true;

	if (LION_CLAUSE_IS_POSITIVE(leaf.kind) && leaf.kind != LION_CLAUSE_MULTI)
	{
		/*
		 * A second positive clause on a column is a source of its own,
		 * ANDed with the first exactly as a clause on another column is:
		 * it is matched to an index that answers ITS operator under ITS
		 * collation (lion_match_index(), per relation), and the AND of
		 * two exact sources is exact.  Only the very same clause again -
		 * the same kind, the same operator, the same input collation and
		 * an equal() value - is dropped, because the lookup the first one
		 * makes is then its answer too.
		 *
		 * Anything less than all four is not "the same clause", and a
		 * clause dropped here is one no index is ever asked about (the
		 * 2026-09-25 review).  Comparing the kind and the value alone
		 * called `v === 'A' AND v = 'A'` a duplicate over an index whose
		 * case-insensitive opclass answers `===` - and nothing at all
		 * answered `=` - so both spellings were counted, 10000 rows
		 * where the query selects 5000.  A nondeterministic collation
		 * does it with one operator: `s COLLATE ci = 'a' AND s = 'a'`
		 * differ in nothing but their input collation.  Now the second
		 * clause needs an index of its own and the query is declined
		 * when there is none.
		 *
		 * Two clauses that differ only in value used to be declined too,
		 * on the grounds that they select little or nothing.  They are
		 * answered now, because nothing in the AND is specific to one
		 * clause per column: the entry a GROUP BY or an IN list drives
		 * is the FIRST suitable clause on its index (lion_inlist_shape()
		 * and lion_locate_where() agree on that), the key a target list
		 * prints is the first pinning clause's, and every clause on the
		 * column must then be one whose index may print it.  Under a
		 * coarse equality they need not even disagree: `v === 'a' AND
		 * v === 'A'` is one entry, looked up twice.
		 *
		 * `IS NOT NULL` is not subject to any of this - it constrains
		 * nothing by itself and is simply subtracted - and neither is a
		 * multi-key clause, whose sources intersect exactly as two
		 * clauses on different columns do (`tags @> '{a}' AND
		 * tags && '{b,c}'` is one AND of three key sets).  Nor is a leaf
		 * of an OR, which says nothing about the rows the OTHER arms
		 * select and so cannot stand in for a clause that does.
		 */
		if (list_member_int(cx->posattnos, (int) leaf.var->varattno))
		{
			ListCell   *l1;
			ListCell   *l2;
			ListCell   *l3;
			bool		same = false;

			forthree(l1, cx->clauseinfos, l2, cx->whereconsts,
					 l3, cx->whereinor)
			{
				LionClauseInfo *prev = (LionClauseInfo *) lfirst(l1);

				if (lfirst_int(l3) != 0 ||
					prev->attno != leaf.var->varattno)
					continue;
				if (prev->kind == leaf.kind &&
					prev->opno == leaf.opno &&
					prev->collation == leaf.collation &&
					equal(lfirst(l2), leaf.val))
				{
					same = true;
					break;
				}
			}
			if (same)
				return true;
		}
		else
			cx->posattnos = lappend_int(cx->posattnos,
										(int) leaf.var->varattno);
	}

	switch (leaf.kind)
	{
		case LION_CLAUSE_EQ:
			cx->eqattnos = lappend_int(cx->eqattnos, (int) leaf.var->varattno);
			cx->nonnullattnos = lappend_int(cx->nonnullattnos,
											(int) leaf.var->varattno);
			break;
		case LION_CLAUSE_NOTNULL:
			if (cx->notnullvar == NULL)
				cx->notnullvar = leaf.var;
			cx->nonnullattnos = lappend_int(cx->nonnullattnos,
											(int) leaf.var->varattno);
			break;
		case LION_CLAUSE_NE:
			/* a strict operator: never true of NULL (DESIGN.md §35) */
			cx->nonnullattnos = lappend_int(cx->nonnullattnos,
											(int) leaf.var->varattno);
			break;
		case LION_CLAUSE_ARRAY:
		case LION_CLAUSE_MULTI:
			/* a strict operator with a non-NULL constant */
			cx->nonnullattnos = lappend_int(cx->nonnullattnos,
											(int) leaf.var->varattno);
			break;
		case LION_CLAUSE_NULL:
			cx->nullattnos = lappend_int(cx->nullattnos,
										 (int) leaf.var->varattno);
			break;
	}

	lion_append_clause(&leaf, clause, false,
					  &cx->whereattnos, &cx->clauseinfos, &cx->whereclauses,
					  &cx->whereconsts, &cx->wherekinds, &cx->whereopnos,
					  &cx->whereinor);
	((LionClauseInfo *) llast(cx->clauseinfos))->rinfono = cx->rinfono;
	return true;
}

/*
 * Every WHERE clause must be one the posting sets can answer, or, over a
 * partitioned table, one every partition's bounds imply.  Builds the clause
 * lists - whereattnos, clauseinfos, whereclauses, whereconsts, wherekinds,
 * whereopnos and whereinor, in step - with ors, the ranges per column
 * (rangevars, rangecls, rangepos) and what the clauses say of each column.
 */
static bool
lion_count_path_where(LionCountPathBuild *cx)
{
	PlannerInfo *root = cx->root;
	RelOptInfo *input_rel = cx->input_rel;
	Index		rti = cx->rti;
	ListCell   *lc;

	/*
	 * ---- every WHERE clause must be one the posting sets can answer ----
	 *
	 * ... or, over a partitioned table, one the bounds of every partition
	 * counted imply (DESIGN.md §16, "Clauses the partition bounds imply"),
	 * which selects nothing there and is left out of the count.  Each clause
	 * remembers the restriction it came from (rinfono), so that a partition
	 * whose bounds imply that restriction can leave out all of it
	 * (lion_collect_targets()).
	 */
	foreach(lc, input_rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
		Node	   *clause;
		LionLeafInfo leaf;
		int			startlen = list_length(cx->whereattnos);
		bool		answered;

		if (!IsA(rinfo, RestrictInfo) || rinfo->pseudoconstant)
			return false;

		cx->rinfono++;
		cx->rinfoclauses = lappend(cx->rinfoclauses, rinfo->clause);

		/* a clause of an expression column is one of a column (§41) */
		clause = lion_vcol_subst((Node *) rinfo->clause);

		/*
		 * `flag IS NOT TRUE` of a boolean column is `flag = false OR flag IS
		 * NULL`, and is an OR like any other from here on
		 * (lion_boolean_not_test()).
		 */
		{
			BoolExpr   *orform = lion_boolean_not_test(clause);
			Node	   *anyform;

			if (orform != NULL)
				clause = (Node *) orform;

			/*
			 * `tsv @@ ANY (array)` is the OR of its elements' clauses
			 * (lion_multikey_any_as_or()).
			 */
			else if ((anyform = lion_multikey_any_as_or(clause)) != NULL)
				clause = anyform;
		}

		/*
		 * An OR of equalities on one column with constants is the IN list it
		 * spells (lion_or_as_array(), DESIGN.md §29.11, "An OR of equalities
		 * is its IN list"): one source, located, priced and counted as the
		 * list is - its values looked up in one walk of the directory, the
		 * disjoint sum when it is the only source, its batches past the
		 * budget - rather than a union of leaves looked up one by one.  The
		 * list is only taken when the posting sets answer it as a list; an
		 * OR whose operator is not the column's equality stays an OR.
		 */
		if (IsA(clause, BoolExpr) && ((BoolExpr *) clause)->boolop == OR_EXPR)
		{
			Node	   *arr = lion_or_as_array(clause);

			if (arr != NULL &&
				lion_analyze_leaf(root, arr, rti, true, true, true, &leaf) &&
				leaf.kind == LION_CLAUSE_ARRAY)
				clause = arr;
		}

		/*
		 * An OR across columns (lion_count_path_where_or()), or a clause of
		 * one column (lion_count_path_where_leaf()).
		 */
		if (IsA(clause, BoolExpr) && ((BoolExpr *) clause)->boolop == OR_EXPR)
			answered = lion_count_path_where_or(cx, clause);
		else
			answered = lion_count_path_where_leaf(cx, rinfo, clause);
		if (answered)
			continue;

		/*
		 * No posting set answers the clause.  That declines the query - unless
		 * every partition it counts has bounds that imply the clause, when no
		 * row it counts can fail it, and it is left out.  Whatever of it was
		 * taken already (the first arms of an OR) goes too.
		 */
		if (!cx->partitioned ||
			!lion_implied_everywhere(root, input_rel, input_rel,
									 (Node *) rinfo->clause))
			return false;
		cx->impliedtexts =
			lappend(cx->impliedtexts,
					makeString(lion_deparse_rel_clause(root, rti,
													   (Node *) rinfo->clause)));
		cx->whereattnos = list_truncate(cx->whereattnos, startlen);
		cx->clauseinfos = list_truncate(cx->clauseinfos, startlen);
		cx->whereclauses = list_truncate(cx->whereclauses, startlen);
		cx->whereconsts = list_truncate(cx->whereconsts, startlen);
		cx->wherekinds = list_truncate(cx->wherekinds, startlen);
		cx->whereopnos = list_truncate(cx->whereopnos, startlen);
		cx->whereinor = list_truncate(cx->whereinor, startlen);
	}
	return true;
}

/*
 * The FK-side join (DESIGN.md §27) takes it from here: false once it has,
 * true when there is none and the count goes on.
 */
static bool
lion_count_path_fkjoin(LionCountPathBuild *cx)
{
	PlannerInfo *root = cx->root;
	RelOptInfo *input_rel = cx->input_rel;
	RelOptInfo *output_rel = cx->output_rel;
	GroupPathExtraData *extra = cx->extra;
	const LionFkJoin *fj = cx->fj;

	/*
	 * The FK-side join's fact filters are all sources, ANDed with every fk
	 * set, so every range among them is a range taken as a source (DESIGN.md
	 * §32); the fk set of each dimension row is what carries the §9 pin.
	 */
	if (fj != NULL && cx->rangevars != NIL)
	{
		lion_ranges_to_sources(cx->rangepos, -1, cx->wherekinds,
							   cx->clauseinfos);
		cx->hasrangesrc = true;
		cx->havepositive = true;
	}

	/*
	 * ---- the FK-side join (DESIGN.md §27) ----
	 *
	 * The fact rel's clauses are all ones the posting sets answer; the join
	 * key, the target list and the path are the join's own business.
	 */
	if (fj != NULL)
	{
		/* the fk key's set of each dimension row drives every count */
		cx->imply.toprel = input_rel;
		cx->imply.clauses = cx->rinfoclauses;
		cx->imply.driven = true;
		cx->imply.skipped = cx->impliedtexts;
		cx->imply.leafclauses = cx->whereclauses;
		cx->imply.ors = cx->ors;
		if (fj->joinpath)
			lion_try_semijoin_path(root, input_rel, fj, cx->whereattnos,
								   cx->clauseinfos, cx->whereclauses,
								   cx->whereconsts, cx->wherekinds,
								   cx->whereopnos, cx->whereinor, cx->ors,
								   cx->partitioned ? &cx->imply : NULL);
		else
			lion_try_fkjoin_path(root, input_rel, output_rel, extra, fj,
								 cx->having, cx->whereattnos, cx->clauseinfos,
								 cx->whereclauses, cx->whereconsts,
								 cx->wherekinds, cx->whereopnos,
								 cx->whereinor, cx->ors,
								 cx->partitioned ? &cx->imply : NULL);
		return false;
	}
	return true;
}

/*
 * The expressions the node has to produce, or test (checkexprs): the grouped
 * relation's target, and the aggregates and columns the HAVING mentions.
 */
static bool
lion_count_path_having(LionCountPathBuild *cx)
{
	RelOptInfo *output_rel = cx->output_rel;

	/* ---- the grouped relation's target, and the HAVING that filters it ---- */
	cx->checkexprs = list_copy(output_rel->reltarget->exprs);
	if (cx->having != NIL)
	{
		/*
		 * A SubPlan would need the node to run a subquery per group; an
		 * uncorrelated one is an InitPlan by now and arrives as a Param,
		 * which is a plain value here.
		 */
		if (contain_subplans((Node *) cx->having))
			return false;
		cx->checkexprs =
			list_concat(cx->checkexprs,
						pull_var_clause(lion_vcol_subst((Node *) cx->having),
										PVC_INCLUDE_AGGREGATES |
										PVC_RECURSE_WINDOWFUNCS |
										PVC_INCLUDE_PLACEHOLDERS));
	}

	/*
	 * An expression column in the target list is a column the node prints,
	 * and in an aggregate's argument one it counts (DESIGN.md §41); the
	 * HAVING's were replaced before its columns were taken out of it.
	 */
	cx->checkexprs = (List *) lion_vcol_subst((Node *) cx->checkexprs);
	return true;
}

/*
 * count(DISTINCT k): the one column the distinct aggregates walk (distvar,
 * disteqop, distcoll), if any, and whether it can be walked here.
 */
static bool
lion_count_path_distinct(LionCountPathBuild *cx)
{
	PlannerInfo *root = cx->root;
	RelOptInfo *input_rel = cx->input_rel;
	Index		rti = cx->rti;
	List	   *uniqattnos = NIL;	/* columns of a count(DISTINCT) proved
									 * unique: a count(col) each (§26) */
	int			g;
	ListCell   *lc;

	/*
	 * ---- count(DISTINCT k) (DESIGN.md §26) ----
	 *
	 * Found first, because it decides what drives the scan: k's own entries
	 * when there is no GROUP BY, and the inner side of the (g, k) nested loop
	 * when there is one.  Every distinct aggregate has to be over the same
	 * plain column, with the same equality and collation - one k, one walk -
	 * and k may be neither a grouping column (its entries would be groups and
	 * distinct values at once) nor the third driving column of a two-column
	 * GROUP BY.  A partitioned table declines: distinct counts do not add up
	 * across partitions, and the partial aggregates of §16 would add them.
	 */
	foreach(lc, cx->checkexprs)
	{
		Aggref	   *agg = (Aggref *) lfirst(lc);
		Var		   *dv;
		Oid			deq;
		Oid			dcoll;

		if (!IsA(agg, Aggref) || agg->aggdistinct == NIL)
			continue;
		if (!lion_agg_distinct_var(agg, rti, &dv, &deq, &dcoll))
			return false;

		/*
		 * A column the relation proves unique - a primary key, say - is not
		 * k at all: its distinct count is the count(col) of the same rows
		 * (lion_agg_distinct_unique()), answered wherever that count(col) is
		 * - which is every row count when col is known non-NULL, and the
		 * other cases of DESIGN.md §14.  Nothing is walked for it, so neither
		 * partitions nor a second distinct column stand in its way.  One
		 * whose count(col) the node cannot answer (a nullable unique column
		 * nothing proves non-NULL) is left to be walked as k, as before.
		 */
		if (lion_column_is_unique(input_rel, dv->varattno, deq, dcoll) &&
			lion_agg_is_count(root, agg, rti, input_rel, cx->groupattno,
							  cx->ngroup, cx->nonnullattnos, cx->nullattnos))
		{
			uniqattnos = list_append_unique_int(uniqattnos,
												(int) dv->varattno);
			continue;
		}
		if (cx->distvar == NULL)
		{
			cx->distvar = dv;
			cx->disteqop = deq;
			cx->distcoll = dcoll;
		}
		else if (dv->varattno != cx->distvar->varattno ||
				 deq != cx->disteqop || dcoll != cx->distcoll)
			return false;
	}
	if (cx->distvar != NULL)
	{
		/*
		 * The plan tells the two kinds apart by their column alone
		 * (lion_plan_custom_path()), so one column is not both: a distinct
		 * count proved unique under one collation beside a walked one under
		 * another is left to the ordinary plan.
		 */
		if (list_member_int(uniqattnos, (int) cx->distvar->varattno))
			return false;
		if (cx->partitioned || cx->ngroup > 1)
			return false;
		/* a coalesce group is §10's walk, not the (g, k) loop of §26 */
		if (cx->groupcoal != NULL)
			return false;
		for (g = 0; g < cx->ngroup; g++)
		{
			if (cx->groupattno[g] == cx->distvar->varattno)
				return false;
		}
	}
	return true;
}

/*
 * An aggregate over the entries of lion column attno (DESIGN.md §37): taken
 * over one table with no WHERE and no GROUP BY, where every entry of a
 * scalar column is its rows' value, and from a whole index on the column
 * whose stored keys are the rows' own values (the value rule of §10) - the
 * argument is evaluated on the key.  Notes the column, its index and its
 * aggregates.
 */
static bool
lion_count_path_wagg(LionCountPathBuild *cx, AttrNumber attno)
{
	IndexOptInfo *idx;
	AttrNumber	col;
	Oid			atttype;
	ListCell   *lc;
	int			i = 0;

	if (cx->fj != NULL || cx->ngroup != 0 || cx->singlegroup ||
		cx->partitioned || cx->distvar != NULL || cx->groupcoal != NULL ||
		cx->input_rel->baserestrictinfo != NIL ||
		LION_ATTNO_IS_VCOL(attno))
		return false;
	foreach(lc, cx->wattnos)
	{
		if ((AttrNumber) lfirst_int(lc) == attno)
		{
			lfirst_int(list_nth_cell(cx->wnaggs, i))++;
			return true;
		}
		i++;
	}
	idx = lion_find_roaring_index(cx->input_rel, attno, false, &col);
	if (idx == NULL || idx->indpred != NIL ||
		!lion_index_can_emit_value(idx, col))
		return false;

	/* the key is the column's value in the column's own representation */
	atttype = get_atttype(cx->rte->relid, attno);
	if (!IsBinaryCoercible(atttype, idx->opcintype[col - 1]) ||
		get_typlen(atttype) != get_typlen(idx->opcintype[col - 1]) ||
		get_typbyval(atttype) != get_typbyval(idx->opcintype[col - 1]))
		return false;
	cx->wattnos = lappend_int(cx->wattnos, (int) attno);
	cx->widx = lappend(cx->widx, idx);
	cx->wcols = lappend_int(cx->wcols, (int) col);
	cx->wnaggs = lappend_int(cx->wnaggs, 1);
	return true;
}

/*
 * Every expression of checkexprs has to be one the node produces: a grouping
 * column, a column a clause pins, or a count the posting sets answer - and
 * there has to be a count.  Notes the columns printed (groupvalueout,
 * valueattnos) and whether the distinct walk counts (distcounts).
 */
static bool
lion_count_path_outputs(LionCountPathBuild *cx)
{
	PlannerInfo *root = cx->root;
	RelOptInfo *input_rel = cx->input_rel;
	Index		rti = cx->rti;
	bool		haveagg = false;
	int			g;
	ListCell   *lc;

	foreach(lc, cx->checkexprs)
	{
		Node	   *node = (Node *) lfirst(lc);

		if (IsA(node, Var))
		{
			Var		   *v = (Var *) node;

			if (v->varno != (int) rti || v->varattno <= 0 ||
				v->varlevelsup != 0)
				return false;

			/*
			 * Under GROUP BY coalesce(g, c) the group's value is the whole
			 * expression, which the node prints, and never g by itself: a
			 * bare g can only reach here out of a HAVING that takes the
			 * expression apart, and the key the node holds for the merged
			 * group is not g's value in all of its rows.
			 */
			if (cx->groupcoal != NULL && v->varattno == cx->groupattno[0])
				return false;

			/*
			 * The column has to be either the group key or one a clause pins
			 * to a single value.  In the second case the value we output is
			 * the key the index stored, not the constant from the query: a
			 * cross-type or otherwise non-identical constant that compares
			 * equal must not change what the query prints.  A column pinned
			 * to NULL by `IS NULL` prints NULL, which the stored key of the
			 * reserved entry says as well.
			 *
			 * Either way the value comes out of an index entry, so the index
			 * has to be one whose entries can produce it - which is decided
			 * per relation, once the indexes are known.  `IS NULL` is exempt:
			 * NULL has one representation.
			 */
			for (g = 0; g < cx->ngroup; g++)
			{
				if (v->varattno == cx->groupattno[g])
					break;
			}
			if (g < cx->ngroup)
				cx->groupvalueout[g] = true;
			else if (list_member_int(cx->eqattnos, (int) v->varattno))
			{
				if (!list_member_int(cx->valueattnos, (int) v->varattno))
					cx->valueattnos = lappend_int(cx->valueattnos,
												  (int) v->varattno);
			}
			else if (!list_member_int(cx->nullattnos, (int) v->varattno))
				return false;
		}
		else if (cx->groupcoal != NULL && equal(node, cx->groupexpr))
		{
			/* the coalesce group's value, printed as the node emits it */
			cx->groupvalueout[0] = true;
		}
		else if (list_member(cx->groupdeps, node))
		{
			/* an expression of the grouping columns, computed from them */
		}
		else if (IsA(node, Aggref))
		{
			Aggref	   *agg = (Aggref *) node;
			AttrNumber	countcols[LION_MAX_GROUPCOLS + 1];
			int			ncountcols = cx->ngroup;
			Node	   *arg = (agg->args != NIL) ?
				lion_strip((Node *) ((TargetEntry *) linitial(agg->args))->expr) :
				NULL;
			AttrNumber	argattno = (arg != NULL && IsA(arg, Var)) ?
				((Var *) arg)->varattno : 0;
			AttrNumber	wattno;

			/*
			 * A sum, an average, a minimum or a maximum over the entries of
			 * one lion column, weighted by their rows (DESIGN.md §37).
			 */
			if (lion_wagg_classify(agg, rti, &wattno, NULL) != LION_WAGG_NONE)
			{
				if (!lion_count_path_wagg(cx, wattno))
					return false;
				haveagg = true;
				continue;
			}

			if (agg->aggdistinct != NIL && cx->distvar != NULL &&
				argattno == cx->distvar->varattno)
			{
				/* checked above; a distinct count needs no other test */
				haveagg = true;
				cx->wcounts = true;
				continue;
			}

			/*
			 * GROUP BY coalesce(g, c) counts g's NULL rows in c's group
			 * (DESIGN.md §10), where count(g) - or a count(DISTINCT g) proved
			 * to be it - counts only the rest, a number the node does not
			 * keep apart.  So it is answered only where no row it counts has
			 * g NULL: g declared NOT NULL, or a clause of the WHERE that says
			 * so, and then there is nothing to merge and it is the group's
			 * count.
			 */
			if (cx->groupcoal != NULL && argattno != 0 &&
				argattno == cx->groupattno[0] &&
				!list_member_int(cx->nonnullattnos, (int) argattno) &&
				!bms_is_member(argattno, lion_notnullattnums(root, input_rel)))
				return false;

			/*
			 * count(k) of the distinct column is answered too, whatever k's
			 * nullability: it is the rows of k's non-NULL entries, which the
			 * walk visits anyway (DESIGN.md §26) - so for this purpose k is
			 * one more column whose entries are known, like a group column.
			 */
			memcpy(countcols, cx->groupattno, sizeof(AttrNumber) * cx->ngroup);
			if (cx->distvar != NULL)
				countcols[ncountcols++] = cx->distvar->varattno;
			if (!lion_agg_is_count(root, agg, rti, input_rel,
								  countcols, ncountcols, cx->nonnullattnos,
								  cx->nullattnos))
				return false;
			haveagg = true;
			cx->wcounts = true;

			/*
			 * Does the walk have to COUNT rather than test for existence?
			 * Without a GROUP BY every other count is a sum over k's entries;
			 * beside one, only count(k) is a sum over the pairs, and count(*)
			 * and count(g) are the group's own count.
			 */
			if (cx->distvar != NULL)
			{
				bool		ofk = (argattno == cx->distvar->varattno);

				if (cx->ngroup == 0 || ofk)
					cx->distcounts = true;
			}
		}
		else
			return false;
	}
	if (!haveagg)
		return false;
	return true;
}

/*
 * The first live leaf under a partitioned rel, and the attnums of the
 * parent's columns there, level by level (lion_child_var()): 0 where a
 * level has no such column.  A plain rel is its own leaf.
 */
static RelOptInfo *
lion_count_first_leaf(PlannerInfo *root, RelOptInfo *rel, AttrNumber *attnos,
					  int natts)
{
	while (IS_PARTITIONED_REL(rel))
	{
		RelOptInfo *child = NULL;
		int			i;

		for (i = 0; i < rel->nparts; i++)
		{
			if (rel->part_rels[i] != NULL &&
				bms_is_member(i, rel->live_parts) &&
				!IS_DUMMY_REL(rel->part_rels[i]))
			{
				child = rel->part_rels[i];
				break;
			}
		}
		if (child == NULL)
			return NULL;
		for (i = 0; i < natts; i++)
		{
			if (attnos[i] != 0)
			{
				Var		   *cvar = lion_child_var(root, child->relid,
												  attnos[i]);

				attnos[i] = (cvar != NULL) ? cvar->varattno : 0;
			}
		}
		rel = child;
	}
	return rel;
}

/*
 * The column a sum over EVERY row drives by (DESIGN.md §35, "Every row"),
 * when nothing in the WHERE selects rows: `count(*)` alone, or `IS NOT NULL`
 * and `<>` clauses only.  The entries of one SCALAR lion column are disjoint
 * and between them - the NULL entry included - hold every row of the table,
 * so the sum over them, less what the negated clauses subtract from each, is
 * the count; the column with the fewest entries is the cheapest walk.  Only a
 * column of a scalar, non-partial, non-expression index will do
 * (lion_find_roaring_index()): a multi-key column's entries overlap and miss
 * the rows with no keys, and a partial index holds only its predicate's rows.
 * A column with an `IS NOT NULL` of its own is preferred when it is no
 * dearer, since its NULL entry is then skipped and its clause dropped
 * (lion_locate_where()).  A partitioned table's candidates are those of its
 * first live leaf, which lion_collect_targets() then asks of every partition.
 */
static Var *
lion_count_all_driver(LionCountPathBuild *cx)
{
	PlannerInfo *root = cx->root;
	RelOptInfo *rel = cx->input_rel;
	int			natts = Max((int) rel->max_attr, 0);
	AttrNumber *attnos = (AttrNumber *) palloc0(sizeof(AttrNumber) * Max(natts, 1));
	RelOptInfo *leaf;
	Var		   *best = NULL;
	double		bestentries = 0.0;
	int			a;

	for (a = 0; a < natts; a++)
		attnos[a] = (AttrNumber) (a + 1);
	leaf = lion_count_first_leaf(root, rel, attnos, natts);
	if (leaf == NULL)
		return NULL;

	for (a = 0; a < natts; a++)
	{
		Oid			type;
		int32		typmod;
		Oid			coll;
		Var		   *var;
		VariableStatData vardata;
		bool		isdefault;
		double		entries;

		/* an indexed column is no dropped one, nor maps to none (§16) */
		if (attnos[a] == 0 ||
			lion_find_roaring_index(leaf, attnos[a], false, NULL) == NULL)
			continue;
		get_atttypetypmodcoll(cx->rte->relid, (AttrNumber) (a + 1), &type,
							  &typmod, &coll);
		var = makeVar(cx->rti, (AttrNumber) (a + 1), type, typmod, coll, 0);
		examine_variable(root, (Node *) var, cx->rti, &vardata);
		entries = get_variable_numdistinct(&vardata, &isdefault);
		ReleaseVariableStats(vardata);

		/* the first `IS NOT NULL` column skips its NULL entry: no dearer */
		if (cx->notnullvar != NULL &&
			var->varattno == cx->notnullvar->varattno)
			entries = Max(entries - 1.0, 1.0);

		if (best == NULL || entries < bestentries)
		{
			best = var;
			bestentries = entries;
		}
	}
	pfree(attnos);
	return best;
}

/*
 * What drives the count: the grouping column, the distinct column, or the
 * sum over all of one column's entries (sumall) - bounded by the range that
 * bounds that walk (rangevar), every other range being made a source.  Sets
 * driveattno, and which clauses must print their value.
 */
static bool
lion_count_path_strategy(LionCountPathBuild *cx)
{
	PlannerInfo *root = cx->root;
	RelOptInfo *input_rel = cx->input_rel;
	Index		rti = cx->rti;
	ListCell   *lc;

	/*
	 * The decoded walk (DESIGN.md §34) reads the WHERE as one collected copy,
	 * and a range is walked rather than collected once it is too large to
	 * collect: not beside it, as yet.  Nor a WHERE of negated clauses alone
	 * (`c IS NOT NULL`), whose copy - the positive clauses' intersection,
	 * less the others - has nothing to begin from.
	 */
	if (cx->decode && (cx->hasrangesrc || cx->rangevars != NIL ||
					   (cx->wherekinds != NIL && !cx->havepositive)))
		return false;

	/* Something has to drive the count. */
	cx->driveattno = cx->groupattno[0];
	if (cx->distvar != NULL && cx->ngroup == 0)
	{
		/* k's entries drive it, whatever the WHERE says (DESIGN.md §26). */
		cx->driveattno = cx->distvar->varattno;
	}

	/*
	 * Which range bounds the walk that DRIVES the count (DESIGN.md §28): the
	 * one on the column the GROUP BY or the count(DISTINCT) walks, or - with
	 * neither - the WIDEST range, whose sum is then the count: it walks the
	 * fewer sets of the two sides (the complement, and the summaries of §32,
	 * make a wide walk short) where a range taken as a source is collected
	 * row by row.  A column with an equality, a list or a null test of its
	 * own does not drive: those are positive clauses on it, which §10 leaves
	 * to the ordinary merge.  Every other range is a source (§32).
	 */
	if (cx->rangevars != NIL)
	{
		int			best = -1;
		double		bestsel = -1.0;
		int			r = 0;
		int			otherpin = -1;	/* not asked yet */
		ListCell   *l1;
		ListCell   *l2;

		forboth(l1, cx->rangevars, l2, cx->rangecls)
		{
			Var		   *v = (Var *) lfirst(l1);

			if (!list_member_int(cx->posattnos, (int) v->varattno) &&
				cx->ngroup <= 1)
			{
				if (cx->ngroup == 1 || cx->distvar != NULL)
				{
					if (v->varattno == cx->driveattno)
						best = r;
				}
				else if (cx->partitioned &&
						 lion_rinfos_implied_everywhere(root, input_rel,
														(List *) lfirst(l2)) &&
						 (otherpin >= 0 ? otherpin :
						  (otherpin =
						   lion_pinned_not_implied(root, input_rel,
												   cx->clauseinfos,
												   cx->rinfoclauses))))
				{
					/*
					 * A range every partition's bounds imply walks all of
					 * each partition's keys: as a source it is left out
					 * instead (DESIGN.md §16, "Clauses the partition bounds
					 * imply"), when another clause is there to select the
					 * rows and carry the §9 interlock.
					 */
				}
				else
				{
					Selectivity sel = clauselist_selectivity(root,
															 (List *) lfirst(l2),
															 rti, JOIN_INNER,
															 NULL);

					if (sel > bestsel)
					{
						bestsel = sel;
						best = r;
					}
				}
			}
			r++;
		}
		if (best >= 0)
		{
			cx->rangevar = (Var *) list_nth(cx->rangevars, best);
			cx->rangeclauses = (List *) list_nth(cx->rangecls, best);
		}
		if (list_length(cx->rangevars) > (best >= 0 ? 1 : 0))
		{
			lion_ranges_to_sources(cx->rangepos, best, cx->wherekinds,
								   cx->clauseinfos);
			cx->hasrangesrc = true;
			cx->havepositive = true;
		}
	}

	if (cx->distvar != NULL && cx->ngroup == 0)
	{
		/* driven by k's entries, above */
	}
	else if (cx->ngroup == 0 && cx->rangevar != NULL)
	{
		/*
		 * A range on k with no GROUP BY (DESIGN.md §28): the sum over k's
		 * entries in the range, each intersected with the other clauses -
		 * §14's sum-over-all with the walk bounded.  The entries of one
		 * scalar index are disjoint, so the sum is the count of their union,
		 * which is exactly the rows whose k is in the range.
		 */
		cx->sumall = true;
		cx->driveattno = cx->rangevar->varattno;
	}
	else if (cx->ngroup == 0 && !cx->havepositive)
	{
		/*
		 * Nothing that selects rows: `count(*)` alone, or beside `IS NOT
		 * NULL` and `<>` clauses only, which subtract (DESIGN.md §14, §35).
		 * Any lion-indexed column's index knows every row of the table, so
		 * the count is the sum over all of its entries - disjoint, a row
		 * having one value per column or none - with the negated clauses
		 * subtracted from each (lion_count_all_driver()).  Driven by the
		 * first `IS NOT NULL` column, its own NULL entry is skipped and its
		 * clause dropped; by any other, the plan names the column.
		 */
		Var		   *drive = lion_count_all_driver(cx);

		if (drive == NULL)
			return false;
		cx->sumall = true;
		cx->driveattno = drive->varattno;
		if (cx->notnullvar == NULL ||
			drive->varattno != cx->notnullvar->varattno)
			cx->allvar = drive;
	}
	/* A group folded to a constant can only have come from a WHERE key. */
	if (cx->singlegroup && !cx->havepositive)
		return false;

	/*
	 * A range taken as a source (DESIGN.md §32) is collected into memory and
	 * holds no pin, so every count it takes part in needs another source that
	 * carries the §9 interlock, or it would recheck every row in the heap: the
	 * walk that drives the count, or a clause - or an OR of clauses - outside
	 * the ranges.  Without one the query is left to the ordinary plan.
	 */
	if (cx->hasrangesrc &&
		!(cx->ngroup > 0 || cx->sumall || cx->distvar != NULL ||
		  cx->pinnedsrc))
		return false;

	/*
	 * The range left bounding a walk has to bound the one that DRIVES the
	 * count (DESIGN.md §28): k's entries under a GROUP BY k, a count(DISTINCT
	 * k) or the sum above, or g's under `g, count(DISTINCT k) ... GROUP BY g`.
	 * Every other range was made a source above (§32), so this only guards
	 * against the two halves of this function drifting apart.
	 */
	if (cx->rangevar != NULL &&
		(cx->ngroup > 1 || cx->driveattno != cx->rangevar->varattno))
		return false;

	/*
	 * Which clauses have to produce a value, rather than just select rows: a
	 * value-producing pushdown needs an index whose stored keys are a
	 * representation the rows themselves have (finding 4 of the 2026-09-20
	 * review), and that is checked per relation below.
	 */
	foreach(lc, cx->clauseinfos)
	{
		LionClauseInfo *ci = (LionClauseInfo *) lfirst(lc);

		ci->valueout = (ci->kind == LION_CLAUSE_EQ &&
						list_member_int(cx->valueattnos, (int) ci->attno));
	}
	return true;
}

/*
 * The relations to count - the table, or each live leaf partition - and the
 * indexes on each (targets, first), found for the driving columns and the
 * clauses.  Orders the two grouping columns by their estimates, and sets
 * distest and rangesel.
 */
static bool
lion_count_path_targets(LionCountPathBuild *cx)
{
	PlannerInfo *root = cx->root;
	RelOptInfo *input_rel = cx->input_rel;
	Index		rti = cx->rti;
	LionDriveInfo drive[LION_MAX_GROUPCOLS];
	int			ndrive = 0;
	int			g;

	/*
	 * ---- the relations to count, and the indexes on each of them ----
	 *
	 * One table, or one live leaf partition at a time (DESIGN.md §16).  The
	 * column numbers above are the parent's; lion_collect_targets() maps them
	 * onto each partition through its AppendRelInfo before looking an index
	 * up, because partitions may number their columns differently.
	 */
	memset(drive, 0, sizeof(drive));

	/*
	 * How many entries each grouping column's index has, which is what
	 * decides the outer/inner roles of the nested loop (DESIGN.md §20) and
	 * what the cost model multiplies: the column with FEWER distinct values
	 * drives the scan, so the other index's keys - which are read once and
	 * probed per pair - are the ones whose bucket lookups are repeated.
	 */
	for (g = 0; g < cx->ngroup; g++)
		cx->groupest[g] = estimate_num_groups(root,
											  list_make1(lion_vcol_unvar((Node *) cx->groupvar[g])),
											  input_rel->rows, NULL, NULL);

	/*
	 * ... and the decoded walk of DESIGN.md §34 takes them in that order too:
	 * its first column's sets are the ones it keeps pinned, and a pass takes
	 * only as many of its values as the pin budget allows, so the fewer the
	 * better.  An insertion sort, stable, of at most LION_MAX_GROUPCOLS.
	 */
	for (g = 1; g < cx->ngroup; g++)
	{
		int			h;

		for (h = g; h > 0 && cx->groupest[h] < cx->groupest[h - 1]; h--)
		{
			Var		   *tv = cx->groupvar[h - 1];
			AttrNumber	ta = cx->groupattno[h - 1];
			Oid			te = cx->groupeqop[h - 1];
			bool		tvo = cx->groupvalueout[h - 1];
			double		tn = cx->groupest[h - 1];

			cx->groupvar[h - 1] = cx->groupvar[h];
			cx->groupattno[h - 1] = cx->groupattno[h];
			cx->groupeqop[h - 1] = cx->groupeqop[h];
			cx->groupvalueout[h - 1] = cx->groupvalueout[h];
			cx->groupest[h - 1] = cx->groupest[h];
			cx->groupvar[h] = tv;
			cx->groupattno[h] = ta;
			cx->groupeqop[h] = te;
			cx->groupvalueout[h] = tvo;
			cx->groupest[h] = tn;
		}
	}
	if (cx->ngroup > 0)
		cx->driveattno = cx->groupattno[0];

	memset(drive, 0, sizeof(drive));
	if (cx->ngroup > 0)
	{
		for (g = 0; g < cx->ngroup; g++)
		{
			drive[g].attno = cx->groupattno[g];
			drive[g].var = cx->groupvar[g];
			drive[g].collation = cx->groupvar[g]->varcollid;
			drive[g].eqop = cx->groupeqop[g];
			drive[g].valueout = cx->groupvalueout[g];
		}
		ndrive = cx->ngroup;
	}
	else if (cx->sumall)
	{
		/* §14's sum-over-all: one index, and it groups nothing. */
		drive[0].attno = cx->driveattno;
		drive[0].var = (cx->rangevar != NULL) ? cx->rangevar :
			(cx->allvar != NULL) ? cx->allvar : cx->notnullvar;
		drive[0].collation = InvalidOid;
		drive[0].eqop = InvalidOid;
		drive[0].valueout = false;
		ndrive = 1;
	}

	/*
	 * The column count(DISTINCT) counts is one more driving column (DESIGN.md
	 * §26): the only one without a GROUP BY, the inner one beside it.  Every
	 * per-column rule of lion_collect_targets() applies to it as to a grouping
	 * column - a SCALAR index (a multi-key one is never found, and
	 * count(DISTINCT tags) counts arrays, not elements), the DISTINCT's
	 * collation, and the DISTINCT's own equality as strategy 1 of the index's
	 * opfamily, so that its entries are exactly the classes DISTINCT counts.
	 * No value of it is ever printed.
	 */
	if (cx->distvar != NULL)
	{
		Assert(ndrive == cx->ngroup && ndrive < LION_MAX_GROUPCOLS);
		drive[ndrive].attno = cx->distvar->varattno;
		drive[ndrive].var = cx->distvar;
		drive[ndrive].collation = cx->distcoll;
		drive[ndrive].eqop = cx->disteqop;
		drive[ndrive].valueout = false;
		ndrive++;

		/*
		 * Every entry of k is tested, whether or not the WHERE leaves it any
		 * row, so the walk is n_distinct(k) of the whole table long.
		 */
		cx->distest = estimate_num_groups(root,
										  list_make1(lion_vcol_unvar((Node *) cx->distvar)),
										  Max(input_rel->tuples, 1.0),
										  NULL, NULL);
	}

	/*
	 * The share of the driving column's entries the range walks (DESIGN.md
	 * §28): the selectivity of the range clauses alone, which for a column
	 * whose rows spread evenly over its values is also the share of its
	 * values, and overstates the walk for a skewed one.  A count(DISTINCT k)
	 * over a range on k tests exactly those entries.
	 */
	if (cx->rangevar != NULL)
	{
		cx->rangesel = clauselist_selectivity(root, cx->rangeclauses, rti,
											  JOIN_INNER, NULL);
		if (cx->distvar != NULL && cx->ngroup == 0)
			cx->distest = lion_range_entries(root, input_rel, cx->distvar,
											 cx->rangesel);
	}

	cx->imply.toprel = input_rel;
	cx->imply.clauses = cx->rinfoclauses;
	cx->imply.driven = (ndrive > 0);
	cx->imply.skipped = cx->impliedtexts;
	cx->imply.leafclauses = cx->whereclauses;
	cx->imply.ors = cx->ors;
	if (!lion_collect_targets(root, input_rel, drive, ndrive, cx->whereattnos,
							 cx->clauseinfos,
							 cx->partitioned ? &cx->imply : NULL,
							 &cx->targets))
		return false;
	if (cx->targets == NIL)
		return false;			/* everything was pruned: leave it to the planner */
	cx->first = (LionCountTarget *) linitial(cx->targets);
	return true;
}

/*
 * The estimates the path is priced by: the groups the walk visits
 * (numgroups) and the rows the node emits (outrows) - partial rows, under
 * partialtarget, for a partitioned GROUP BY.
 */
static void
lion_count_path_estimate(LionCountPathBuild *cx)
{
	PlannerInfo *root = cx->root;
	RelOptInfo *input_rel = cx->input_rel;
	RelOptInfo *output_rel = cx->output_rel;
	ListCell   *lc;

	/* ---- build the path ---- */
	if (cx->ngroup == 1)
		cx->numgroups = cx->groupest[0];
	else if (cx->ngroup >= 2)
	{
		List	   *vars = NIL;
		int			g;

		for (g = 0; g < cx->ngroup; g++)
			vars = lappend(vars, lion_vcol_unvar((Node *) cx->groupvar[g]));
		cx->numgroups = estimate_num_groups(root, vars, input_rel->rows,
											NULL, NULL);
	}
	else if (cx->sumall && cx->rangevar != NULL)
	{
		/* Every entry in the range is visited (DESIGN.md §28). */
		cx->numgroups = lion_range_entries(root, input_rel, cx->rangevar,
										   cx->rangesel);
	}
	else if (cx->sumall)
	{
		/* Every entry of the driving index is visited, one group or not. */
		Var		   *dv = (cx->allvar != NULL) ? cx->allvar : cx->notnullvar;

		Assert(dv != NULL);
		cx->numgroups = estimate_num_groups(root,
											list_make1(lion_vcol_unvar((Node *) dv)),
											input_rel->rows, NULL, NULL);
	}
	else if (cx->distvar != NULL)
	{
		/* ... and so is every entry of k's (DESIGN.md §26). */
		cx->numgroups = cx->distest;
	}
	else
		cx->numgroups = 1.0;

	/*
	 * How many rows the node itself produces.  One per group, except for a
	 * partitioned GROUP BY, which emits one PARTIAL row per group per
	 * partition and lets the Finalize Agg above combine them (DESIGN.md §16):
	 * that is the sum of the partitions' own group estimates, each made
	 * against the partition's statistics and capped by its row count.
	 */
	if (cx->ngroup == 0)
		cx->outrows = 1.0;
	else if (cx->decode)
	{
		/*
		 * The decoded walk's rows are partial counts, one a combination with
		 * rows (DESIGN.md §34), below a Finalize Agg - as a partitioned
		 * table's are.
		 */
		cx->outrows = cx->numgroups;
		cx->partialtarget = lion_make_partial_target(root,
													 output_rel->reltarget,
													 cx->having);
	}
	else if (!cx->partitioned)
		cx->outrows = cx->numgroups;
	else
	{
		cx->outrows = 0.0;
		foreach(lc, cx->targets)
		{
			LionCountTarget *t = (LionCountTarget *) lfirst(lc);
			double		relrows = Max(t->rel->rows, 1.0);
			double		relgroups;

			if (t->drivevar[0] != NULL && cx->ngroup == 2 &&
				t->drivevar[1] != NULL)
				relgroups = estimate_num_groups(root,
												list_make2(t->drivevar[0],
														   t->drivevar[1]),
												relrows, NULL, NULL);
			else if (t->drivevar[0] != NULL)
				relgroups = estimate_num_groups(root,
												list_make1(t->drivevar[0]),
												relrows, NULL, NULL);
			else
				relgroups = cx->numgroups;
			cx->outrows += Min(relgroups, relrows);
		}
		cx->outrows = Max(cx->outrows, 1.0);

		/*
		 * ... and those rows are partial aggregates, so the node's target is
		 * the partially-grouped one and the grouped rel gets a Finalize Agg
		 * over it further down.
		 */
		cx->partialtarget = lion_make_partial_target(root,
													 output_rel->reltarget,
													 cx->having);
	}
}

/*
 * THE TOP k BY COUNT (DESIGN.md §36): `GROUP BY g ORDER BY count(*) DESC
 * LIMIT k`, where the node may emit only the groups that can be among the
 * first k rows and leave their order and the cut to the Sort and Limit core
 * puts above it.  Sets topkn, topkcand and topkstrict, or leaves topkn 0.
 *
 * The grouped rel's paths are read by nothing but that Sort and Limit when
 * the query has no DISTINCT, window function, set-returning function or set
 * operation, so a path of it that leaves out groups the Limit would cut
 * anyway answers the query.  One grouping column walked in one table, one
 * group at a time: not an IN list driving the groups, a coalesce group, a
 * count(DISTINCT), the decoded walk or partial counts - and no HAVING, which
 * could reject groups the k were counted from.
 *
 * The first ORDER BY key has to be the group's count, descending: an
 * aggregate the node answers as the rows of the group - count(*), or
 * count(col) of a column no row of the group has NULL - sorted by int8's `>`
 * of the integer btree family.  Any key after it, or WITH TIES, makes every
 * group tied with the k-th count come out (topkstrict).  k is the LIMIT and
 * the OFFSET added up, both constants.
 *
 * The walk keeps topkcand candidates: enough, for a WHERE that keeps a share
 * s of the rows independently of g, that the k-th count - some s of its bound
 * - is passed by the bounds of the ones after it, 2k/s; never fewer than k
 * and some to spare, never more than LION_TOPK_MAX_CAND, and only when that
 * is well short of the column's entries - or every group is counted anyway.
 */
#define LION_TOPK_MAX		10000
#define LION_TOPK_MAX_CAND	16384

/*
 * The rows of the cand largest groups of var, of ngroups: the frequencies of
 * its most common values, which the statistics keep largest first, and past
 * the end of that list the frequency of a value it leaves out; at the least
 * cand average groups.
 */
static double
lion_topk_rows(PlannerInfo *root, RelOptInfo *rel, Var *var, double cand,
			   double ngroups)
{
	VariableStatData vardata;
	AttStatsSlot sslot;
	double		tuples = Max(rel->tuples, 1.0);
	double		rows = cand * tuples / Max(ngroups, 1.0);

	examine_variable(root, lion_vcol_unvar((Node *) var), 0, &vardata);
	if (HeapTupleIsValid(vardata.statsTuple) &&
		get_attstatsslot(&sslot, vardata.statsTuple, STATISTIC_KIND_MCV,
						 InvalidOid, ATTSTATSSLOT_NUMBERS))
	{
		Form_pg_statistic stats =
			(Form_pg_statistic) GETSTRUCT(vardata.statsTuple);
		double		mcv = 0.0;
		double		top = 0.0;
		int			i;

		for (i = 0; i < sslot.nnumbers; i++)
		{
			mcv += sslot.numbers[i];
			if (i < cand)
				top += sslot.numbers[i];
		}
		if (cand > sslot.nnumbers)
			top += (cand - sslot.nnumbers) *
				Max(1.0 - mcv - stats->stanullfrac, 0.0) /
				Max(ngroups - sslot.nnumbers, 1.0);
		rows = Max(rows, top * tuples);
		free_attstatsslot(&sslot);
	}
	ReleaseVariableStats(vardata);
	return Min(rows, tuples);
}

static void
lion_count_path_topk(LionCountPathBuild *cx)
{
	PlannerInfo *root = cx->root;
	Query	   *parse = cx->parse;
	LionCountTarget *first = cx->first;
	SortGroupClause *sgc;
	TargetEntry *tle;
	Aggref	   *agg;
	Oid			opfamily;
	Oid			opcintype;
	int64		limit;
	int64		offset = 0;
	bool		sumshort;
	bool		groupdrive;
	List	   *others = NIL;
	Selectivity sel;
	double		cand;
	ListCell   *lc;

	cx->topkn = 0;
	if (!lion_enable_topk || cx->fj != NULL || cx->ngroup != 1 ||
		cx->decode || cx->partitioned || cx->sumall || cx->singlegroup ||
		cx->distvar != NULL || cx->groupcoal != NULL || cx->having != NIL ||
		first == NULL || first->driveidx[0] == NULL)
		return;
	if (parse->distinctClause != NIL || parse->hasWindowFuncs ||
		parse->hasTargetSRFs || parse->groupingSets != NIL ||
		parse->setOperations != NULL || parse->sortClause == NIL ||
		parse->limitCount == NULL || !IsA(parse->limitCount, Const) ||
		((Const *) parse->limitCount)->constisnull)
		return;
	limit = DatumGetInt64(((Const *) parse->limitCount)->constvalue);
	if (parse->limitOffset != NULL)
	{
		if (!IsA(parse->limitOffset, Const))
			return;
		if (!((Const *) parse->limitOffset)->constisnull)
			offset = DatumGetInt64(((Const *) parse->limitOffset)->constvalue);
	}
	if (limit <= 0 || offset < 0 || limit > LION_TOPK_MAX ||
		offset > LION_TOPK_MAX - limit)
		return;

	/* ORDER BY the group's count, descending */
	sgc = linitial_node(SortGroupClause, parse->sortClause);
	tle = get_sortgroupclause_tle(sgc, root->processed_tlist);
	if (tle == NULL || !IsA(tle->expr, Aggref))
		return;
	agg = (Aggref *) tle->expr;
	if (agg->aggdistinct != NIL || agg->aggorder != NIL ||
		agg->aggfilter != NULL || agg->agglevelsup != 0)
		return;
	if (agg->args != NIL)
	{
		Node	   *arg = lion_strip((Node *)
									 ((TargetEntry *) linitial(agg->args))->expr);
		AttrNumber	attno;

		if (arg == NULL || !IsA(arg, Var))
			return;
		attno = ((Var *) arg)->varattno;

		/*
		 * count(col) of a column a clause pins to NULL is 0, and of the group
		 * column 0 in the NULL group: only where the group has no NULL is it
		 * the group's rows (lion_plan_custom_path()'s count kinds).
		 */
		if (list_member_int(cx->nullattnos, (int) attno))
			return;
		if (attno == cx->groupattno[0] &&
			!list_member_int(cx->nonnullattnos, (int) attno) &&
			!bms_is_member(attno, lion_notnullattnums(root, cx->input_rel)))
			return;
	}
	if (!lion_ordering_op_is_gt(sgc->sortop, &opfamily, &opcintype) ||
		opcintype != INT8OID || opfamily != INTEGER_BTREE_FAM_OID)
		return;

	/* the entries of the index are the groups, not an IN list's (§15) */
	(void) lion_inlist_shape(first->driveidx[0], first->drivecol[0],
							 first->driveidx[1],
							 first->whereidx, first->wherecol,
							 cx->whereclauses, cx->wherekinds,
							 cx->ors, false, &sumshort, &groupdrive);
	if (groupdrive)
		return;

	/*
	 * The share of the rows the WHERE keeps, leaving out what it says about g
	 * alone: that takes entries out of the walk, and the counts of the rest
	 * whole.
	 */
	foreach(lc, cx->input_rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
		Bitmapset  *attnos = NULL;
		int			attno;

		pull_varattnos(lion_vcol_subst((Node *) rinfo->clause), cx->rti,
					   &attnos);
		if (bms_get_singleton_member(attnos, &attno) &&
			attno + FirstLowInvalidHeapAttributeNumber == cx->groupattno[0])
			continue;
		others = lappend(others, rinfo);
	}
	sel = (others != NIL) ?
		clauselist_selectivity(root, others, 0, JOIN_INNER, NULL) : 1.0;
	sel = Max(sel, 1e-6);

	cand = Max(ceil(2.0 * (double) (limit + offset) / sel),
			   (double) (limit + offset) + 64.0);
	cand = Min(cand, (double) Max(limit + offset, LION_TOPK_MAX_CAND));
	if (cand * 2.0 > cx->groupest[0])
		return;

	cx->topkn = limit + offset;
	cx->topkcand = (int) cand;
	cx->topkrows = lion_topk_rows(root, cx->input_rel, cx->groupvar[0], cand,
								  cx->groupest[0]);
	cx->topkstrict = (list_length(parse->sortClause) > 1 ||
					  parse->limitOption == LIMIT_OPTION_WITH_TIES);
}

/*
 * What the path's custom_private carries (lion_plan_private.h), begun again
 * for each path made: the relation and its clauses, a partitioned table's
 * partitions, and what the query asks of them.
 */
static void
lion_count_path_fill(LionCountPathBuild *cx)
{
	PlannerInfo *root = cx->root;
	LionCountTarget *first = cx->first;
	LionCountPriv *p = &cx->priv;
	List	   *exec;

	memset(p, 0, sizeof(LionCountPriv));

	/*
	 * A plain table's own Oids go in LION_PRIV_OIDS, which is where the
	 * executor and EXPLAIN have always read them.  A partitioned one leaves
	 * them invalid - there is no single index - and fills LION_PRIV_PARTS
	 * instead, one OidList per partition in the same clause order.
	 */
	p->heapoid = cx->rte->relid;
	p->groupidxoid = (!cx->partitioned && first->driveidx[0] != NULL) ?
		first->driveidx[0]->indexoid : InvalidOid;
	p->groupidxoid2 = (!cx->partitioned && !cx->decode &&
					   first->driveidx[1] != NULL) ?
		first->driveidx[1]->indexoid : InvalidOid;
	p->scanrelid = cx->rti;
	p->groupattno = cx->groupattno[0];
	p->groupattno2 = cx->decode ? 0 : cx->groupattno[1];
	p->flags = (cx->singlegroup ? LION_FLAG_SINGLEGROUP : 0) |
		(cx->sumall ? LION_FLAG_SUMALL : 0) |
		(cx->driveattno != 0 ? LION_FLAG_GROUPIDX : 0) |
		(cx->rangevar != NULL ? LION_FLAG_RANGE : 0) |
		(cx->decode ? LION_FLAG_DECODE : 0);

	/*
	 * The strategy of a multi-key clause travels with its operator: the
	 * executor re-extracts the query and EXPLAIN prints the operator's name,
	 * and both need the Oid.
	 */
	lion_count_priv_set_where(p, cx->partitioned ? NIL : first->whereidx,
							  cx->whereattnos, cx->whereconsts,
							  cx->wherekinds, cx->whereopnos, cx->ors);
	if (cx->partitioned)
		lion_count_priv_set_parts(p, cx->targets, true);

	/* a partitioned GROUP BY's HAVING is the Finalize Agg's */
	p->having = (cx->partialtarget != NULL) ? NIL : cx->having;
	if (cx->distvar != NULL)
		p->distattno = cx->distvar->varattno;
	exec = lion_replaced_functions(cx->input_rel,
								   cx->output_rel->reltarget->exprs,
								   cx->having, root->processed_groupClause,
								   NULL);
	p->exec_aggs = (List *) linitial(exec);
	p->exec_funcs = (List *) lsecond(exec);
	p->exec_groupfuncs = (List *) lthird(exec);

	/*
	 * GROUP BY coalesce(col, c) (DESIGN.md §10): c, and the grouping equality
	 * the walk tells c's own entry by, under the grouping collation - which
	 * lion_collect_targets() has made the driving index's strategy 1 and its
	 * collation, per relation.
	 */
	if (cx->groupcoal != NULL)
	{
		p->coalconst = copyObject(cx->groupcoal);
		p->coaleqop = cx->groupeqop[0];
		p->coalcoll = cx->groupvar[0]->varcollid;
	}
	p->implied = cx->impliedtexts;

	/* the decoded walk's columns (DESIGN.md §34), in the order it takes them */
	if (cx->decode)
	{
		int			g;

		p->ngroupn = cx->ngroup;
		for (g = 0; g < cx->ngroup; g++)
		{
			p->groupn_attno[g] = cx->groupattno[g];
			p->groupn_idx[g] = first->driveidx[g]->indexoid;
		}
	}

	/* the column a sum over every row drives by (DESIGN.md §35) */
	if (cx->allvar != NULL)
		p->allattno = cx->allvar->varattno;

	/* the top k by count (DESIGN.md §36) */
	p->topkn = cx->topkn;
	p->topkcand = cx->topkcand;
	p->topkstrict = cx->topkstrict;

	/* the columns aggregates are taken over (DESIGN.md §37) */
	if (cx->wattnos != NIL)
	{
		ListCell   *l1;
		ListCell   *l2;
		ListCell   *l3;
		int			i = 0;

		p->nwcol = list_length(cx->wattnos);
		p->wcol = (LionCountPrivWCol *)
			palloc0(sizeof(LionCountPrivWCol) * p->nwcol);
		forthree(l1, cx->wattnos, l2, cx->widx, l3, cx->wcols)
		{
			p->wcol[i].attno = (AttrNumber) lfirst_int(l1);
			p->wcol[i].idxoid = ((IndexOptInfo *) lfirst(l2))->indexoid;
			p->wcol[i].idxcol = (AttrNumber) lfirst_int(l3);
			i++;
		}
	}

	/*
	 * The expression columns (DESIGN.md §41), when the plan uses one, and
	 * the functions of each one used: core's plan computes a grouped or
	 * counted expression in its scan's projection, and so checks EXECUTE on
	 * them (a WHERE clause's are in baserestrictinfo already).
	 */
	if (cx->vcols != NIL)
	{
		Bitmapset  *used = NULL;
		int			i;

#define LION_NOTE_VCOL(a) \
		do { \
			if (LION_ATTNO_IS_VCOL(a)) \
				used = bms_add_member(used, LION_VCOL_INDEX(a)); \
		} while (0)

		LION_NOTE_VCOL(p->groupattno);
		LION_NOTE_VCOL(p->groupattno2);
		LION_NOTE_VCOL(p->distattno);
		LION_NOTE_VCOL(p->allattno);
		for (i = 0; i < p->nclause; i++)
			LION_NOTE_VCOL(p->clause[i].attno);
		for (i = 0; i < p->ngroupn; i++)
			LION_NOTE_VCOL(p->groupn_attno[i]);
#undef LION_NOTE_VCOL

		if (used != NULL)
		{
			p->vcols = cx->vcols;
			i = -1;
			while ((i = bms_next_member(used, i)) >= 0)
				p->exec_funcs = lion_expr_functions(list_nth(cx->vcols, i),
													p->exec_funcs);
		}
	}
}

/*
 * The LionCount CustomPath itself: its target, the order its groups come in
 * and the plan it carries in custom_private.  Not priced yet.
 */
static CustomPath *
lion_count_path_make(LionCountPathBuild *cx)
{
	PlannerInfo *root = cx->root;
	RelOptInfo *input_rel = cx->input_rel;
	RelOptInfo *output_rel = cx->output_rel;
	LionCountTarget *first = cx->first;
	CustomPath *cpath;

	cpath = makeNode(CustomPath);
	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = output_rel;
	cpath->path.pathtarget = cx->partialtarget ? cx->partialtarget :
		output_rel->reltarget;
	cpath->path.param_info = NULL;
	cpath->path.parallel_aware = false;
	cpath->path.parallel_safe = false;
	cpath->path.parallel_workers = 0;
	/*
	 * PATHKEYS (DESIGN.md §21).  A single-column GROUP BY driven by the
	 * index's own entry scan emits its groups in DIRECTORY order, which for an
	 * index that orders by the key type's own comparison is exactly what an
	 * `ORDER BY <group col>` asks for - so the Sort above the node disappears.
	 *
	 * Four things disqualify it:
	 *
	 *	- two group columns: the nested loop of §20 emits (outer, inner) pairs,
	 *	  which are sorted by the outer key alone and not by the pair;
	 *	- an IN list driving the groups (§15): the node walks the located sets
	 *	  rather than the index, and claiming an order for that would tie the
	 *	  planner to a detail of how the list is located;
	 *	- a partitioned table: each partition is ordered, but the Finalize
	 *	  HashAggregate core puts on top destroys it (§16);
	 *	- a NULLABLE group column, because the reserved NULL entry sorts
	 *	  FIRST and `ORDER BY col` means NULLS LAST.  A column the planner
	 *	  knows is NOT NULL has no NULL group to emit, so the two agree;
	 *	- and GROUP BY coalesce(col, c) (DESIGN.md §10), whose group c the
	 *	  walk holds back and emits after the last entry, wherever c sorts.
	 */
	cpath->path.pathkeys = NIL;
	if (cx->ngroup == 1 && !cx->partitioned && !cx->sumall &&
		!cx->singlegroup &&
		cx->groupcoal == NULL && cx->groupdeps == NIL &&
		first->driveidx[0] != NULL &&
		bms_is_member(cx->groupattno[0],
					  lion_notnullattnums(root, input_rel)) &&
		lion_index_orders_naturally(first->driveidx[0], first->drivecol[0]))
	{
		bool		sumshort;
		bool		groupdrive;
		Oid			sortop = InvalidOid;
		Oid			eqop = InvalidOid;
		bool		hashable;

		(void) lion_inlist_shape(first->driveidx[0], first->drivecol[0],
								 first->driveidx[1],
								 first->whereidx, first->wherecol,
								 cx->whereclauses, cx->wherekinds,
								 cx->ors, false, &sumshort, &groupdrive);
		/*
		 * The node emits ASCENDING, NULLS FIRST, always.  The query's own
		 * GROUP BY clause is not a safe source for the direction:
		 * standard_qp_callback() rewrites its sort operators to match the
		 * query's ORDER BY when it can, so `ORDER BY k DESC` would hand us a
		 * descending SortGroupClause and we would claim an order the node does
		 * not produce.  So the ordering operator is the key type's own `<`,
		 * and the clause is copied only for its sortgroupref and its equality.
		 */
		get_sort_group_operators(exprType((Node *) cx->groupvar[0]),
								 true, true, false,
								 &sortop, &eqop, NULL, &hashable);

		if (!groupdrive && OidIsValid(sortop))
		{
			SortGroupClause *sgc;

			sgc = copyObject((SortGroupClause *)
							 linitial(root->processed_groupClause));
			sgc->sortop = sortop;
			sgc->nulls_first = false;
			cpath->path.pathkeys =
				make_pathkeys_for_sortclauses(root, list_make1(sgc),
											  root->processed_tlist);
		}
	}
	cpath->flags = 0;
	cpath->custom_paths = NIL;
#if PG_VERSION_NUM >= 170000
	cpath->custom_restrictinfo = NIL;
#endif
	cpath->custom_private = lion_count_priv_encode(&cx->priv,
												   LION_PRIV_STAGE_PATH);
	cpath->methods = &lion_count_path_methods;

	return cpath;
}

/*
 * Price the path and add it to output_rel: below core's Finalize Agg for a
 * partitioned GROUP BY, and otherwise as it is - after the parallel GROUP BY
 * made from it, where there is one.
 *
 * The price is the node's own, in the units of the cheapest core path
 * output_rel has (DESIGN.md §39): times that plan's rate, and over the
 * margin.  What core puts above it - the Finalize Agg of a partitioned or a
 * parallel GROUP BY, the Gather - core prices itself, in its own units, as it
 * prices the same nodes over its own plans.
 */
static void
lion_count_path_add(LionCountPathBuild *cx, CustomPath *cpath)
{
	PlannerInfo *root = cx->root;
	RelOptInfo *input_rel = cx->input_rel;
	RelOptInfo *output_rel = cx->output_rel;
	GroupPathExtraData *extra = cx->extra;
	LionCountTarget *first = cx->first;
	LionRangeCost rangeprice;	/* ... and what its ranges pay */
	Cost		serialrun;		/* the serial node's price, without HAVING */
	LionUnits	units;			/* the competitor's (§39) */

	lion_units_for(output_rel, &units);
	if (lion_where_query_unknown(cx->whereclauses, cx->wherekinds,
								 cx->whereinor))
		units.margin = 1.0;

	/*
	 * Beside a GROUP BY, count(DISTINCT k) is the (g, k) nested loop of
	 * DESIGN.md §20, g outer and k inner, and is priced as those pairs
	 * (DESIGN.md §26); without one, k's entries are the "groups" of the walk.
	 */
	if (cx->decode)
	{
		lion_cost_decode_path(root, cpath, cx->targets, cx->whereclauses,
							  cx->wherekinds, cx->ors, cx->ngroup,
							  cx->groupest, cx->numgroups, cx->outrows);
		rangeprice.batched = false;
		rangeprice.perrange = 0;
	}
	else if (cx->topkn > 0)
	{
		/*
		 * The top k (DESIGN.md §36): a walk of every entry's header, and the
		 * candidates counted - the largest entries, whose rows the column's
		 * most common values say.
		 */
		lion_cost_topk_path(root, cpath, cx->targets, cx->whereclauses,
							cx->wherekinds, cx->ors, cx->groupest[0],
							(cx->rangevar != NULL) ? cx->rangesel : 1.0,
							(double) cx->topkcand, cx->topkrows,
							Min(cx->outrows, (double) cx->topkcand));
		rangeprice.batched = false;
		rangeprice.perrange = 0;
	}
	else
		lion_cost_count_path(root, cpath, cx->targets, cx->whereclauses,
							 cx->wherekinds, cx->ors,
							 cx->numgroups, cx->groupest[0],
							 (cx->ngroup == 2) ? cx->groupest[1] :
							 (cx->distvar != NULL && cx->ngroup == 1) ?
							 cx->distest : 0,
							 cx->outrows,
							 (cx->distvar == NULL) ? LION_DISTINCT_NONE :
							 cx->distcounts ? LION_DISTINCT_COUNT :
							 LION_DISTINCT_EXISTS,
							 (cx->rangevar == NULL) ?
							 ((cx->sumall && cx->distvar == NULL) ?
							  LION_RANGED_SUMALL :
							  LION_RANGED_NONE) :
							 cx->sumall ? LION_RANGED_SUM : LION_RANGED_WALK,
							 cx->rangesel, &rangeprice);
	/*
	 * The aggregates over lion columns' entries (DESIGN.md §37): their walks,
	 * after - or, with no count in the target list, instead of - the sum over
	 * every row.
	 */
	if (cx->wattnos != NIL)
		lion_cost_wagg_path(root, cpath, input_rel, cx->widx, cx->wcols,
							cx->wnaggs, cx->wcounts);

	cpath->path.startup_cost = lion_units_price(&units,
												cpath->path.startup_cost);
	cpath->path.total_cost = lion_units_price(&units, cpath->path.total_cost);
	rangeprice.perrange = lion_units_price(&units, rangeprice.perrange);
	serialrun = cpath->path.total_cost;

	/*
	 * The HAVING the node applies itself costs an evaluation per group and
	 * lets a fraction of the groups through: the same accounting cost_agg()
	 * does for an Agg's quals, so that the two plans stay comparable.  A
	 * partitioned table's HAVING is the Finalize Agg's and is priced there.
	 */
	if (cx->having != NIL && cx->partialtarget == NULL)
	{
		QualCost	qual_cost;
		double		groups = cpath->path.rows;

		cost_qual_eval(&qual_cost, cx->having, root);
		cpath->path.startup_cost += qual_cost.startup;
		cpath->path.total_cost += qual_cost.startup +
			groups * qual_cost.per_tuple;
		cpath->path.rows = clamp_row_est(groups *
										 clauselist_selectivity(root,
																cx->having,
																0, JOIN_INNER,
																NULL));
	}

	/*
	 * A partitioned GROUP BY produces partial aggregates, so what goes into
	 * the grouped rel is core's Finalize HashAggregate over the node - which
	 * combines the per-partition counts and, unlike anything this node could
	 * hold, spills to disk when the groups do not fit in hash_mem
	 * (DESIGN.md §16).  Everything else is already the finished answer.
	 */
	if (cx->partialtarget != NULL)
	{
		AggClauseCosts agg_final_costs;

		MemSet(&agg_final_costs, 0, sizeof(agg_final_costs));
		get_agg_clause_costs(root, AGGSPLIT_FINAL_DESERIAL, &agg_final_costs);

		add_path(output_rel, (Path *)
				 create_agg_path(root, output_rel, &cpath->path,
								 output_rel->reltarget,
								 AGG_HASHED, AGGSPLIT_FINAL_DESERIAL,
								 root->processed_groupClause,
								 cx->having,
								 &agg_final_costs,
								 cx->numgroups));
		return;
	}

	/*
	 * ... and, where the query may run in parallel, the same GROUP BY divided
	 * among the participants of a Gather by ranges of container keys, below
	 * core's Finalize HashAggregate (DESIGN.md §10, "A GROUP BY in parallel"):
	 * one grouping column walked whole, a WHERE that is collected - priced as
	 * the groups of a walk counted together (rangeprice.batched), with a
	 * source that is positive whatever its value, and no range taken as one -
	 * and partial counts core can add up.  Before the serial path is added,
	 * which may free it.
	 */
	if (cx->ngroup == 1 && !cx->sumall && !cx->singlegroup &&
		cx->groupcoal == NULL &&
		cx->distvar == NULL && cx->rangevar == NULL && !cx->hasrangesrc &&
		cx->plainpositive && rangeprice.batched && cx->topkn == 0 &&
		cx->groupdeps == NIL &&
		first->driveidx[0] != NULL &&
		input_rel->consider_parallel && output_rel->consider_parallel &&
		is_parallel_safe(root, (Node *) cx->priv.consts) &&
		extra != NULL && (extra->flags & GROUPING_CAN_PARTIAL_AGG) != 0 &&
		grouping_is_hashable(root->processed_groupClause) &&
		!RecoveryInProgress())
		lion_add_parallel_group_path(root, input_rel, output_rel, cpath,
									 serialrun, &rangeprice,
									 first->driveidx[0], first->drivecol[0],
									 cx->having, cx->numgroups);

	add_path(output_rel, &cpath->path);
}

/*
 * May a GROUP BY of two columns, made the nested loop of DESIGN.md §20, be
 * the decoded walk of §34 as well?  On the terms of three columns
 * (lion_count_path_group_by(), lion_count_path_strategy()): one table, not a
 * partitioned one, partial counts core can add up, no range in the WHERE, no
 * count(DISTINCT) or coalesce group, and not on a hot standby.
 */
static bool
lion_count_path_decodable(LionCountPathBuild *cx)
{
	return lion_enable_decoded_walk &&
		cx->ngroup == 2 && !cx->decode && cx->fj == NULL &&
		!cx->partitioned && cx->distvar == NULL && cx->groupcoal == NULL &&
		cx->groupdeps == NIL && !cx->sumall && !cx->singlegroup &&
		!cx->hasrangesrc && cx->rangevars == NIL &&
		(cx->wherekinds == NIL || cx->havepositive) &&
		cx->first != NULL && cx->first->driveidx[0] != NULL &&
		cx->first->driveidx[1] != NULL &&
		cx->extra != NULL &&
		(cx->extra->flags & GROUPING_CAN_PARTIAL_AGG) != 0 &&
		grouping_is_hashable(cx->root->processed_groupClause) &&
		!RecoveryInProgress();
}

/*
 * Decide whether count(*) over input_rel can be answered from roaring
 * posting sets and, if so, add a CustomPath to output_rel.  Every failed
 * check simply returns: the normal plan is always available.
 *
 * fj is the FK-side join of DESIGN.md §27, or NULL.  With it, input_rel is
 * the JOIN rel and everything below about "the relation" - its WHERE clauses
 * and their indexes - is asked of the FACT rel, fj->factrel, unchanged; what
 * differs (the join key, the target list, the path) is lion_try_fkjoin_path().
 *
 * It runs in phases, each a function that adds to one LionCountPathBuild
 * or returns false to end it there: a check that failed, or the FK-side join
 * taking over.
 */
void
lion_try_count_path(PlannerInfo *root, RelOptInfo *input_rel,
				   RelOptInfo *output_rel, GroupPathExtraData *extra,
				   const LionFkJoin *fj)
{
	LionCountPathBuild cx;
	LionVColScope saved;

	lion_count_path_init(&cx, root, input_rel, output_rel, extra, fj);

	if (!lion_count_path_query(&cx))
		return;
	if (!lion_count_path_rel(&cx))
		return;

	/*
	 * The expressions of the table's lion indexes are columns from here on
	 * (DESIGN.md §41): of one table, which no FK-side join reads.  A
	 * partition numbers its own Vars, and its indexes' expressions are its
	 * own, so a partitioned table has none.
	 */
	if (fj == NULL && !cx.partitioned)
		cx.vcols = lion_vcol_collect(cx.input_rel);
	saved = lion_vcol_enter(cx.vcols, cx.rti);
	PG_TRY();
	{
		lion_try_count_path_scoped(&cx);
	}
	PG_FINALLY();
	{
		lion_vcol_leave(saved);
	}
	PG_END_TRY();
}

/*
 * The rest of lion_try_count_path(), inside the scope of the relation's
 * expression columns.
 */
static void
lion_try_count_path_scoped(LionCountPathBuild *cxp)
{
	LionCountPathBuild cx = *cxp;
	CustomPath *cpath;

	if (!lion_count_path_group_by(&cx))
		return;
	if (!lion_count_path_where(&cx))
		return;
	if (!lion_count_path_fkjoin(&cx))
		return;
	if (!lion_count_path_having(&cx))
		return;
	if (!lion_count_path_distinct(&cx))
		return;
	if (!lion_count_path_outputs(&cx))
		return;
	if (!lion_count_path_strategy(&cx))
		return;
	/* §37's aggregates beside a sum over every row, and nothing else */
	if (cx.wattnos != NIL && !cx.sumall)
		return;
	if (!lion_count_path_targets(&cx))
		return;

	lion_count_path_estimate(&cx);
	lion_count_path_topk(&cx);
	lion_count_path_fill(&cx);
	cpath = lion_count_path_make(&cx);
	lion_count_path_add(&cx, cpath);

	/*
	 * Two columns are the nested loop of DESIGN.md §20, which ANDs the sets of
	 * every pair, or the decoded walk of §34, which reads each column's sets
	 * once whatever the number of pairs: both are priced, and add_path()
	 * keeps the cheaper.
	 */
	if (lion_count_path_decodable(&cx))
	{
		cx.decode = true;
		lion_count_path_estimate(&cx);
		lion_count_path_fill(&cx);
		cpath = lion_count_path_make(&cx);
		lion_count_path_add(&cx, cpath);
	}
}
