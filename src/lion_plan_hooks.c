/*-------------------------------------------------------------------------
 *
 * lion_plan_hooks.c
 *		The planner hooks that offer LionCount paths, and the plans made of
 *		those paths.
 *
 * Part of the LionCount custom scan: lion_customscan.h describes the node
 * and declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_customscan.h"
#include "lion_plan_private.h"

bool		lion_enable_count_pushdown = true;
bool		lion_enable_filter_switch = true;
bool		lion_enable_decoded_walk = true;
bool		lion_enable_topk = true;
create_upper_paths_hook_type lion_prev_create_upper_paths_hook = NULL;
bool		lion_enable_semijoin = true;
set_join_pathlist_hook_type lion_prev_set_join_pathlist_hook = NULL;

const CustomPathMethods lion_count_path_methods = {
	.CustomName = "LionCount",
	.PlanCustomPath = lion_plan_custom_path,
	.ReparameterizeCustomPathByChild = NULL,
};

static const CustomScanMethods lion_count_scan_methods = {
	.CustomName = "LionCount",
	.CreateCustomScanState = lion_create_custom_scan_state,
};

const CustomPathMethods lion_semijoin_path_methods = {
	.CustomName = "LionSemiJoin",
	.PlanCustomPath = lion_plan_custom_path,
	.ReparameterizeCustomPathByChild = NULL,
};

const CustomPathMethods lion_antijoin_path_methods = {
	.CustomName = "LionAntiJoin",
	.PlanCustomPath = lion_plan_custom_path,
	.ReparameterizeCustomPathByChild = NULL,
};

static const CustomScanMethods lion_semijoin_scan_methods = {
	.CustomName = "LionSemiJoin",
	.CreateCustomScanState = lion_create_custom_scan_state,
};

static const CustomScanMethods lion_antijoin_scan_methods = {
	.CustomName = "LionAntiJoin",
	.CreateCustomScanState = lion_create_custom_scan_state,
};

const CustomPathMethods lion_join_agg_path_methods = {
	.CustomName = "LionJoinAgg",
	.PlanCustomPath = lion_plan_join_agg_path,
	.ReparameterizeCustomPathByChild = NULL,
};

static const CustomScanMethods lion_join_agg_scan_methods = {
	.CustomName = "LionJoinAgg",
	.CreateCustomScanState = lion_create_join_agg_state,
};

void
lion_create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
					   RelOptInfo *input_rel, RelOptInfo *output_rel,
					   void *extra)
{
	if (lion_prev_create_upper_paths_hook != NULL)
		lion_prev_create_upper_paths_hook(root, stage, input_rel, output_rel,
										 extra);

	/* a competitor pinned by a hook an error left (§39) is never read */
	lion_units_unpin();

	if (stage != UPPERREL_GROUP_AGG)
		return;
	if (!lion_enable_count_pushdown)
		return;

	/*
	 * No lion index may be read while PostgreSQL 16's old_snapshot_threshold
	 * is set (DESIGN.md §9, lion_check_old_snapshot()), so there is no count
	 * to push down, into one table or into a join.
	 */
	if (lion_old_snapshot_threshold_active())
		return;

	/*
	 * Every lion path added below is priced against the core path that is
	 * cheapest now, before the first of them can free it (§39,
	 * lion_units_pin()).
	 */
	lion_units_pin(output_rel);

	/*
	 * A join may be the FK-side join of DESIGN.md §27: of two tables, either
	 * way round, or of a fact and a dimension that is itself a join ("A
	 * dimension that is a join"), with each table that could be the fact.
	 * Each way that qualifies is tried, and the cost model chooses among them
	 * and the ordinary plan.
	 */
	if (input_rel->reloptkind == RELOPT_JOINREL)
	{
		LionFkJoin *fj;
		int			nfj = lion_fkjoin_recognize(root, input_rel, &fj);
		int			i;

		for (i = 0; i < nfj; i++)
			lion_try_count_path(root, input_rel, output_rel,
							   (GroupPathExtraData *) extra, &fj[i]);
		lion_units_unpin();
		return;
	}

	/*
	 * A range past the histogram's ends is priced with the ends the
	 * directory holds (DESIGN.md §28, "The endpoint probe").
	 */
	if (!lion_probe_begin(root, input_rel, input_rel->baserestrictinfo))
	{
		lion_try_count_path(root, input_rel, output_rel,
						   (GroupPathExtraData *) extra, NULL);
		lion_units_unpin();
		return;
	}
	PG_TRY();
	{
		lion_try_count_path(root, input_rel, output_rel,
						   (GroupPathExtraData *) extra, NULL);
	}
	PG_FINALLY();
	{
		lion_probe_end();
	}
	PG_END_TRY();
	lion_units_unpin();
}

/*
 * set_join_pathlist_hook: the FK-side semi or anti join as a JOIN path
 * (DESIGN.md §27, "The semi and anti join as a join path").  A join rel of
 * an outer side - any rel - semi- or anti-joined to one fact table on one
 * key gets a path that emits the outer side's rows with (or without) a
 * match, from a lookup of each row's key in the fact's posting sets, for
 * whatever needs those rows above: another join, a sort, an aggregate, or
 * the upper node itself as its joined dimension ("A dimension that is a
 * join").  lion_fkjoin_recognize_join() says whether this call is one;
 * lion_try_count_path() then checks the fact side exactly as for the upper
 * node, and lion_try_semijoin_path() adds the paths.  Chained to any hook
 * installed before ours, which runs first.
 */
void
lion_set_join_pathlist(PlannerInfo *root, RelOptInfo *joinrel,
					   RelOptInfo *outerrel, RelOptInfo *innerrel,
					   JoinType jointype, JoinPathExtraData *extra)
{
	LionFkJoin	fj;

	if (lion_prev_set_join_pathlist_hook != NULL)
		lion_prev_set_join_pathlist_hook(root, joinrel, outerrel, innerrel,
										 jointype, extra);
	lion_units_unpin();

	if (!lion_enable_count_pushdown || !lion_enable_semijoin)
		return;
	if (jointype != JOIN_SEMI && jointype != JOIN_ANTI)
		return;
	if (lion_old_snapshot_threshold_active())
		return;
	if (!lion_fkjoin_recognize_join(root, joinrel, outerrel, innerrel,
									jointype, extra, &fj))
		return;
	lion_units_pin(joinrel);
	lion_try_count_path(root, innerrel, joinrel, NULL, &fj);
	lion_units_unpin();
}

/*
 * The position of a dimension column in the child plan's target list, which
 * the executor reads it from (DESIGN.md §27).  The child was planned with
 * CP_EXACT_TLIST from the dimension rel's own target, which holds every
 * dimension column anything above the scan needs, so a column that is not
 * there is planner drift and not a query to decline.
 */
static AttrNumber
lion_child_resno(Plan *child, Var *var)
{
	ListCell   *lc;

	foreach(lc, child->targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		Var		   *cv = (Var *) tle->expr;

		if (cv != NULL && IsA(cv, Var) && cv->varno == var->varno &&
			cv->varattno == var->varattno && cv->varlevelsup == 0)
			return tle->resno;
	}

	elog(ERROR, "LionCount: column %d of relation %d is not in the join's child plan",
		 (int) var->varattno, (int) var->varno);
	return 0;					/* keep the compiler quiet */
}

/* The target-list kinds of the plan's custom_private, from `kinds` */
static void
lion_priv_set_tlkinds(LionCountPriv *priv, List *kinds)
{
	int			i = 0;
	ListCell   *lc;

	priv->ntl = list_length(kinds);
	priv->tlkind = (int *) palloc(sizeof(int) * Max(priv->ntl, 1));
	foreach(lc, kinds)
		priv->tlkind[i++] = lfirst_int(lc);
}

/*
 * Turn an FK-side join path (DESIGN.md §27) into a CustomScan.
 *
 * The node's own tuple - custom_scan_tlist - is every dimension column the
 * target list uses (and the join key's, which the join clause's value
 * expression references), each read from the child's current row, plus the
 * partial counts.  The target list itself is the partially-grouped target and
 * may compute expressions over those columns (`upper(d.name)`); setrefs.c
 * rewrites it, and the key's value expression in custom_exprs, into INDEX_VAR
 * references against custom_scan_tlist, and the node's projection evaluates
 * it per row.  HAVING is the Agg's, so there is no qual here.  The rows of a
 * count(DISTINCT) carry no counts - but for counted rows' one partial
 * count(*) (LION_JOINFLAG_COUNTS) - and they, or the partial counts, may
 * carry the fact's join column, which is read from the key's column of the
 * child.
 *
 * A semi or anti join path (LION_JOINFLAG_OUTER) is a join rel's: its target
 * list is the join rel's rows, the outer side's columns - and a semi join's
 * fact key where an equivalence class needs it, which is read from the key's
 * column too - and it is named LionSemiJoin or LionAntiJoin.
 */
static Plan *
lion_plan_fkjoin_path(PlannerInfo *root, RelOptInfo *rel,
					  CustomPath *best_path, LionCountPriv *priv,
					  List *tlist, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	Plan	   *child;
	List	   *want;
	List	   *ctlist = NIL;
	List	   *kinds = NIL;
	int			joinclause;
	Node	   *keyexpr;
	Var		   *keyvar;
	Index		factrelid;
	AttrNumber	fkattno;
	AttrNumber	fgattno;
	AttrNumber	keyresno;
	ListCell   *lc;

	if (list_length(custom_plans) != 1)
		elog(ERROR, "LionCount: a join needs exactly one child plan");
	child = (Plan *) linitial(custom_plans);

	joinclause = priv->join.clause;
	keyexpr = (Node *) list_nth(priv->consts, joinclause);
	keyvar = (Var *) lion_strip(keyexpr);
	Assert(keyvar != NULL && IsA(keyvar, Var));
	keyresno = lion_child_resno(child, keyvar);

	/*
	 * The fact's join column, which the rows of a count(DISTINCT) carry as
	 * the key (lion_fkjoin_agg_is_distinct()), and so do the rows or partial
	 * counts of a GROUP BY of it (lion_fkjoin_fk_is_key()); the planner lets
	 * it into an inner join's target list for nothing else that reads it
	 * (a count of it is answered from the rows' counts).  The fact rel and
	 * the column are the join clause's.
	 */
	factrelid = priv->scanrelid;
	fkattno = priv->clause[joinclause].attno;

	/*
	 * ... and a fact column grouped by, whose value each row carries as the
	 * group it counts ("Grouped by a fact column"): the entry's stored key,
	 * or the value the partition's bounds give it.
	 */
	fgattno = priv->fgattno;

	want = pull_var_clause((Node *) tlist,
						   PVC_INCLUDE_AGGREGATES |
						   PVC_RECURSE_WINDOWFUNCS |
						   PVC_INCLUDE_PLACEHOLDERS);

	/*
	 * A semi or anti join path projects (CUSTOMPATH_SUPPORT_PROJECTION): the
	 * target list core hands it may be another than its path's - or none, when
	 * core means to put its own on the plan afterwards (a ProjectionPath above,
	 * create_projection_plan()) - so its tuple is every column of the join
	 * rel's rows, from which any such target list is computed.
	 */
	if ((priv->join.flags & LION_JOINFLAG_OUTER) != 0)
		want = list_concat(want,
						   pull_var_clause((Node *) best_path->path.pathtarget->exprs,
										   PVC_RECURSE_PLACEHOLDERS));
	want = lappend(want, keyvar);

	foreach(lc, want)
	{
		Node	   *expr = (Node *) lfirst(lc);
		int			kind;
		ListCell   *l2;
		bool		dup = false;

		if (IsA(expr, Var) && ((Var *) expr)->varno == (int) factrelid &&
			((Var *) expr)->varattno == fkattno &&
			priv->join.type != LION_JOIN_ANTI)
			kind = LION_TL_CHILDCOL(keyresno);
		else if (IsA(expr, Var) && fgattno != 0 &&
				 ((Var *) expr)->varno == (int) factrelid &&
				 ((Var *) expr)->varattno == fgattno)
			kind = LION_TL_GROUPKEY;
		else if (IsA(expr, Var))
			kind = LION_TL_CHILDCOL(lion_child_resno(child, (Var *) expr));
		else if (IsA(expr, Aggref))
			kind = LION_TL_COUNT;	/* every count the planner accepted */
		else
		{
			elog(ERROR, "unexpected expression in LionCount join target list");
			kind = 0;			/* keep the compiler quiet */
		}

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
										 NULL, false));
		kinds = lappend_int(kinds, kind);
	}

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = NIL;
	cscan->scan.scanrelid = 0;
	cscan->flags = best_path->flags;
	cscan->custom_plans = custom_plans;

	/* The clause values go to custom_exprs, as for every other shape. */
	cscan->custom_exprs = priv->consts;
	priv->consts = NIL;
	priv->having = NIL;
	priv->join.keyresno = keyresno;
	lion_priv_set_tlkinds(priv, kinds);

	cscan->custom_scan_tlist = ctlist;
	cscan->custom_relids = rel->relids;
	cscan->custom_private = lion_count_priv_encode(priv, LION_PRIV_STAGE_PLAN);

	/*
	 * A semi or anti join path (LION_JOINFLAG_OUTER, DESIGN.md §27, "The semi
	 * and anti join as a join path") is named for what it is in EXPLAIN.
	 */
	if ((priv->join.flags & LION_JOINFLAG_OUTER) != 0)
		cscan->methods = (priv->join.type == LION_JOIN_ANTI) ?
			&lion_antijoin_scan_methods : &lion_semijoin_scan_methods;
	else
		cscan->methods = &lion_count_scan_methods;

	return &cscan->scan.plan;
}

/*
 * Make the name "LionCount" resolvable when a plan is read back - by a
 * parallel worker, above all, which reads the leader's plan before it has
 * planned anything itself (DESIGN.md §27, "Parallel").  From _PG_init.
 */
void
lion_count_scan_register(void)
{
	if (GetCustomScanMethods("LionCount", true) == NULL)
		RegisterCustomScanMethods(&lion_count_scan_methods);
	if (GetCustomScanMethods("LionJoinAgg", true) == NULL)
		RegisterCustomScanMethods(&lion_join_agg_scan_methods);
	if (GetCustomScanMethods("LionSemiJoin", true) == NULL)
		RegisterCustomScanMethods(&lion_semijoin_scan_methods);
	if (GetCustomScanMethods("LionAntiJoin", true) == NULL)
		RegisterCustomScanMethods(&lion_antijoin_scan_methods);
}

/*
 * The LionJoinAgg above an Agg whose aggregates were rewritten over an
 * FK-side join's counted rows (lion_add_join_agg_path()).  Its tuple is the
 * Agg's, column for column, described by custom_scan_tlist as the query's own
 * target: setrefs.c rewrites the plan's target list - the same expressions -
 * into INDEX_VAR references to it, and every node above it finds the query's
 * aggregates there.  The Agg's own target list, which is the same list with
 * the aggregates rewritten, is its child's business.
 */
Plan *
lion_plan_join_agg_path(PlannerInfo *root, RelOptInfo *rel,
						CustomPath *best_path, List *tlist, List *clauses,
						List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	List	   *ctlist = NIL;
	List	   *aggtlist;
	ListCell   *lc;
	ListCell   *la;

	lion_count_scan_register();

	if (list_length(custom_plans) != 1)
		elog(ERROR, "LionJoinAgg: needs exactly one child plan");
	aggtlist = ((Plan *) linitial(custom_plans))->targetlist;
	if (list_length(tlist) != list_length(aggtlist))
		elog(ERROR, "LionJoinAgg: the Agg's target list is not the query's");

	forboth(lc, tlist, la, aggtlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		TargetEntry *atle = (TargetEntry *) lfirst(la);

		if (exprType((Node *) tle->expr) != exprType((Node *) atle->expr) ||
			exprTypmod((Node *) tle->expr) != exprTypmod((Node *) atle->expr))
			elog(ERROR, "LionJoinAgg: the Agg's target list is not the query's");
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
	cscan->custom_private = NIL;
	cscan->custom_scan_tlist = ctlist;
	cscan->custom_relids = rel->relids;
	cscan->methods = &lion_join_agg_scan_methods;

	return &cscan->scan.plan;
}

/*
 * Which of the decoded walk's GROUP BY columns (DESIGN.md §34) attno is, in
 * the order the walk takes them: 0 for the first, which is priv's group
 * column, 1 and on for the others, and -1 for a column that is not one of
 * them.
 */
static int
lion_groupn_col(const LionCountPriv *priv, AttrNumber attno)
{
	int			i;

	for (i = 0; i < priv->ngroupn; i++)
	{
		if (priv->groupn_attno[i] == attno)
			return i;
	}
	return -1;
}

/*
 * An aggregate of DESIGN.md §37 in the tuple the node produces: agg over the
 * entries of one of priv's columns, weighted by their rows.  Appends it to
 * *ctlist with its LION_TL_WAGG kind, what it computes to priv's aggregates -
 * its LION_WAGG_* kind, its column's position in priv's, the width of an
 * integer argument, the aggregate and its input collation - and its argument
 * to *wargs; and the column itself, once, as LION_TL_WKEY: the key the
 * argument is evaluated on.  False for any other aggregate.  An aggregate
 * already in *ctlist is done.  priv->wagg has room for one more.
 */
static bool
lion_wagg_add_target(Aggref *agg, LionCountPriv *priv,
					 List **ctlist, List **kinds, List **wargs)
{
	AttrNumber	attno;
	int			width = 0;
	int			wkind = lion_wagg_classify(agg, priv->scanrelid, &attno,
										   &width);
	LionCountPrivWAgg *a;
	Node	   *arg;
	List	   *vars;
	int			c;
	ListCell   *lc;

	if (wkind == LION_WAGG_NONE)
		return false;
	for (c = 0; c < priv->nwcol; c++)
	{
		if (priv->wcol[c].attno == attno)
			break;
	}
	if (c == priv->nwcol)
		elog(ERROR, "LionCount: an aggregate over column %d, which is not walked",
			 attno);

	foreach(lc, *ctlist)
	{
		if (equal(((TargetEntry *) lfirst(lc))->expr, agg))
			return true;
	}
	arg = (Node *) ((TargetEntry *) linitial(agg->args))->expr;
	a = &priv->wagg[priv->nwagg++];
	a->kind = wkind;
	a->col = c;
	a->argwidth = width;
	a->aggfnoid = agg->aggfnoid;
	a->collation = agg->inputcollid;
	*wargs = lappend(*wargs, copyObject(arg));
	*ctlist = lappend(*ctlist,
					  makeTargetEntry((Expr *) copyObject(agg),
									  list_length(*ctlist) + 1, NULL, false));
	*kinds = lappend_int(*kinds, LION_TL_WAGG(priv->nwagg - 1));

	vars = pull_var_clause(arg, PVC_RECURSE_PLACEHOLDERS);
	foreach(lc, *ctlist)
	{
		if (equal(((TargetEntry *) lfirst(lc))->expr, linitial(vars)))
			return true;
	}
	*ctlist = lappend(*ctlist,
					  makeTargetEntry((Expr *) copyObject(linitial(vars)),
									  list_length(*ctlist) + 1, NULL, true));
	*kinds = lappend_int(*kinds, LION_TL_WKEY(c));
	return true;
}

/*
 * Turn the path into a CustomScan.
 *
 * scan.scanrelid is 0 because this is an upper node, so custom_scan_tlist has
 * to describe the tuple the node produces and setrefs.c rewrites the plan's
 * targetlist into INDEX_VAR references against it (set_customscan_references).
 */
Plan *
lion_plan_custom_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
					 List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	List	   *ctlist = NIL;
	List	   *kinds = NIL;
	LionCountPriv priv;
	List	   *having;
	List	   *want;
	bool	   *inor;
	AttrNumber	groupattno;
	AttrNumber	groupattno2;
	AttrNumber	distattno;
	List	   *wargs = NIL;
	ListCell   *lc;
	int			o;

	/*
	 * The name has to be resolvable before any CustomScan node of ours is
	 * written out or read back.  _PG_init registers it
	 * (lion_count_scan_register()); this is for a library loaded some other
	 * way, and registering twice is harmless.
	 */
	lion_count_scan_register();

	lion_count_priv_decode(best_path->custom_private, LION_PRIV_STAGE_PATH,
						   &priv);
	if (priv.hasjoin)
		return lion_plan_fkjoin_path(root, rel, best_path, &priv, tlist,
									 custom_plans);

	groupattno = priv.groupattno;
	groupattno2 = priv.groupattno2;
	distattno = priv.distattno;

	/*
	 * A leaf of an OR constrains no column of the result (DESIGN.md §19), so
	 * it neither pins a value the target list may print nor makes a
	 * `count(col)` zero: the other arms select rows it says nothing about.
	 */
	inor = (bool *) palloc0(sizeof(bool) * Max(priv.nclause, 1));
	for (o = 0; o < priv.nor; o++)
	{
		int			j;

		for (j = 0; j < priv.ors[o].nleaves; j++)
			inor[priv.ors[o].first + j] = true;
	}

	/*
	 * The tuple the node produces has to hold every column and count the
	 * target list prints AND every one the HAVING compares: setrefs.c
	 * rewrites both into references to custom_scan_tlist, and an aggregate
	 * it cannot find there would be left as an Aggref, which no executor
	 * node but Agg can evaluate.  A count the HAVING alone mentions becomes
	 * a column the projection above simply does not print.
	 */
	having = priv.having;
	want = list_copy(tlist);
	if (having != NIL)
	{
		List	   *refs = lion_vcol_refs_with((Node *) having,
												 priv.vcols);

		foreach(lc, refs)
		{
			Node	   *ref = (Node *) lfirst(lc);

			want = lappend(want, makeTargetEntry((Expr *) ref,
												 list_length(want) + 1,
												 NULL, true));
		}
	}

	/* an aggregate over keys per target entry at most */
	if (priv.nwcol > 0)
		priv.wagg = (LionCountPrivWAgg *)
			palloc0(sizeof(LionCountPrivWAgg) * Max(list_length(want), 1));

	foreach(lc, want)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		Node	   *expr = (Node *) tle->expr;
		Node	   *sexpr;
		int			kind;
		ListCell   *l2;
		bool		dup = false;

		/*
		 * An expression column (DESIGN.md §41) is classified as the virtual
		 * column it stands for, and goes into custom_scan_tlist as the
		 * expression itself, which is what setrefs.c matches the plan's
		 * target list against.
		 */
		sexpr = (priv.vcols != NIL) ?
			lion_vcol_subst_with(expr, priv.vcols, priv.scanrelid) : expr;

		if (IsA(sexpr, Aggref) && priv.nwcol > 0 &&
			lion_wagg_add_target((Aggref *) sexpr, &priv, &ctlist, &kinds,
								 &wargs))
			continue;
		if (IsA(sexpr, Aggref))
		{
			Aggref	   *agg = (Aggref *) sexpr;
			AttrNumber	attno = 0;

			if (agg->args != NIL)
			{
				Node	   *arg = lion_strip((Node *)
											((TargetEntry *) linitial(agg->args))->expr);

				Assert(arg != NULL && IsA(arg, Var));
				attno = ((Var *) arg)->varattno;
			}

			/*
			 * count(*) and every count(col) the planner accepted are the
			 * count of the group, except two cases DESIGN.md §14 spells out:
			 * count of the group column is 0 in the NULL group, and count of
			 * a column a clause pins to NULL is always 0.
			 *
			 * A count(DISTINCT) is the walked one of DESIGN.md §26 when it
			 * names that column, and otherwise one of a column the planner
			 * proved unique, which is that column's count(col) and takes the
			 * same cases (lion_agg_distinct_unique()): the planner never lets
			 * one column be both.
			 */
			kind = LION_TL_COUNT;
			if (agg->aggdistinct != NIL && distattno != 0 &&
				attno == distattno)
				kind = LION_TL_COUNT_DISTINCT;
			else if (attno != 0)
			{
				int			i;

				if (distattno != 0 && attno == distattno)
					kind = LION_TL_COUNT_DISTCOL;	/* §26: k's non-NULL rows */
				else if (groupattno != 0 && attno == groupattno)
					kind = LION_TL_COUNT_GROUPCOL;
				else if (groupattno2 != 0 && attno == groupattno2)
					kind = LION_TL_COUNT_GROUPCOL2;
				else if ((i = lion_groupn_col(&priv, attno)) > 0)
					kind = (i == 1) ? LION_TL_COUNT_GROUPCOL2 :
						LION_TL_COUNT_GROUPCOLN(i);
				else
				{
					for (i = 0; i < priv.nclause; i++)
					{
						if (!inor[i] &&
							priv.clause[i].kind == LION_CLAUSE_NULL &&
							priv.clause[i].attno == attno)
						{
							kind = LION_TL_COUNT_ZERO;
							break;
						}
					}
				}
			}
		}
		else if (IsA(sexpr, Var))
		{
			AttrNumber	attno = ((Var *) sexpr)->varattno;
			int			i;

			if (groupattno != 0 && attno == groupattno)
				kind = LION_TL_GROUPKEY;
			else if (groupattno2 != 0 && attno == groupattno2)
				kind = LION_TL_GROUPKEY2;
			else if ((i = lion_groupn_col(&priv, attno)) > 0)
				kind = (i == 1) ? LION_TL_GROUPKEY2 : LION_TL_GROUPKEYN(i);
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
				for (i = 0; i < priv.nclause; i++)
				{
					int			ckind = priv.clause[i].kind;

					if (inor[i])
						continue;	/* §19: an OR leaf pins nothing */
					if (ckind != LION_CLAUSE_EQ && ckind != LION_CLAUSE_NULL)
						continue;
					if (priv.clause[i].attno == attno)
					{
						kind = LION_TL_WHEREKEY + i;
						break;
					}
				}
				if (kind < 0)
					elog(ERROR, "LionCount: column %d is neither grouped nor constrained",
						 attno);
			}
		}
		else if (IsA(sexpr, CoalesceExpr) && priv.coalconst != NULL)
		{
			/*
			 * GROUP BY coalesce(col, c) (DESIGN.md §10): the only expression
			 * the planner lets through is the grouping one, and the node
			 * emits its value - the entry's key, or c for the group of the
			 * NULL entry and c's own.
			 */
			kind = LION_TL_GROUPKEY;
		}
		else if (!contain_agg_clause(sexpr) &&
				 pull_var_clause(sexpr, 0) != NIL)
		{
			/*
			 * An expression of the grouping columns (DESIGN.md §36): the node
			 * emits the columns, and setrefs.c makes the expression over them
			 * part of the plan's projection.  The planner let through no
			 * other.  An expression column is emitted as its expression.
			 */
			List	   *vars = pull_var_clause(sexpr, 0);
			ListCell   *l3;

			foreach(l3, vars)
			{
				AttrNumber	attno = ((Var *) lfirst(l3))->varattno;
				Node	   *col = (Node *) lfirst(l3);
				int			i;

				if (groupattno != 0 && attno == groupattno)
					kind = LION_TL_GROUPKEY;
				else if (groupattno2 != 0 && attno == groupattno2)
					kind = LION_TL_GROUPKEY2;
				else if ((i = lion_groupn_col(&priv, attno)) > 0)
					kind = (i == 1) ? LION_TL_GROUPKEY2 : LION_TL_GROUPKEYN(i);
				else
					elog(ERROR, "LionCount: an expression of column %d, which is not grouped",
						 attno);

				if (LION_ATTNO_IS_VCOL(attno))
					col = (Node *) list_nth(priv.vcols, LION_VCOL_INDEX(attno));
				dup = false;
				foreach(l2, ctlist)
				{
					if (equal(((TargetEntry *) lfirst(l2))->expr, col))
					{
						dup = true;
						break;
					}
				}
				if (dup)
					continue;
				ctlist = lappend(ctlist,
								 makeTargetEntry((Expr *) copyObject(col),
												 list_length(ctlist) + 1,
												 NULL, false));
				kinds = lappend_int(kinds, kind);
			}
			continue;
		}
		else
			elog(ERROR, "unexpected expression in LionCount target list");

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
	pfree(inor);

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = having;
	cscan->scan.scanrelid = 0;
	cscan->flags = best_path->flags;
	cscan->custom_plans = NIL;

	/*
	 * The clause values move from custom_private into custom_exprs, which is
	 * the only field of a CustomScan the planner's later passes look inside:
	 * set_customscan_references() fixes its expressions up and
	 * SS_finalize_plan() collects the Param ids it finds there into the
	 * plan's extParam/allParam, which is what makes the executor rescan this
	 * node when an exec Param changes (DESIGN.md §10).  A value left only in
	 * custom_private would be invisible to both.
	 */
	cscan->custom_exprs = priv.consts;
	priv.consts = NIL;
	priv.having = NIL;

	/*
	 * The aggregates over lion columns' entries (DESIGN.md §37): what each
	 * computes is in priv already, and its argument goes after the clause
	 * values, where setrefs.c points its column at the key's place in
	 * custom_scan_tlist.
	 */
	if (priv.nwcol > 0)
		cscan->custom_exprs = list_concat(list_copy(cscan->custom_exprs),
										  wargs);
	lion_priv_set_tlkinds(&priv, kinds);

	/*
	 * The expression columns are matched to the index's expressions by the
	 * executor (lion_index_col_is_vcol()), which are of varno 1; setrefs.c
	 * does not look into custom_private, so they are renumbered here.
	 */
	if (priv.vcols != NIL)
	{
		priv.vcols = copyObject(priv.vcols);
		ChangeVarNodes((Node *) priv.vcols, priv.scanrelid, 1, 0);
	}

	cscan->custom_scan_tlist = ctlist;
	cscan->custom_relids = rel->relids;
	cscan->custom_private = lion_count_priv_encode(&priv,
												   LION_PRIV_STAGE_PLAN);
	cscan->methods = &lion_count_scan_methods;

	return &cscan->scan.plan;
}
