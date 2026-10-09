/*-------------------------------------------------------------------------
 *
 * lion_plan_target.c
 *		The aggregates and GROUP BY columns a LionCount path answers, and the
 *		functions and targets of the plans it replaces.
 *
 * Part of the LionCount custom scan: lion_customscan.h describes the node
 * and declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_customscan.h"

/*
 * Is the aggregate a count(DISTINCT col) whose column is provably unique over
 * rel (DESIGN.md §26, "A unique column")?  Then no two rows the node counts
 * share a non-NULL value of col, so count(DISTINCT col) IS count(col) over the
 * same rows - of every group, under any WHERE - and lion_agg_is_count() takes
 * it as such: count(*), where col is also known non-NULL.
 *
 * The proof is the FK-side join's (lion_column_is_unique()): a single-column
 * btree index on col that is unique, immediate, not partial and not an
 * expression, whose opfamily has the DISTINCT's own equality as its equality
 * strategy and whose collation is the DISTINCT's - the equality and collation
 * nodeAgg would deduplicate the column under (lion_agg_distinct_var()).  For a
 * partitioned parent rel is the parent, whose unique index is a global proof;
 * a unique index on each partition is not one.
 */
static bool
lion_agg_distinct_unique(Aggref *agg, Index rti, RelOptInfo *rel)
{
	Var		   *var;
	Oid			eqop;
	Oid			collation;

	if (!lion_agg_distinct_var(agg, rti, &var, &eqop, &collation))
		return false;
	return lion_column_is_unique(rel, var->varattno, eqop, collation);
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
 *
 * count(DISTINCT col) is count(col) when col is unique over rel
 * (lion_agg_distinct_unique()), and then takes the same rules.
 */
bool
lion_agg_is_count(PlannerInfo *root, Aggref *agg, Index rti, RelOptInfo *rel,
				 const AttrNumber *groupattno, int ngroup,
				 const List *nonnullattnos, const List *nullattnos)
{
	TargetEntry *tle;
	Node	   *arg;
	Var		   *var;
	int			i;

	if (agg->aggfnoid != F_COUNT_ && agg->aggfnoid != F_COUNT_ANY)
		return false;
	if (agg->aggorder != NIL || agg->aggfilter != NULL || agg->aggvariadic)
		return false;
	if (agg->aggdistinct != NIL && !lion_agg_distinct_unique(agg, rti, rel))
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
	arg = lion_strip((Node *) tle->expr);
	if (arg == NULL || !IsA(arg, Var))
		return false;
	var = (Var *) arg;
	if (var->varno != (int) rti || var->varattno <= 0 ||
			var->varlevelsup != 0)
		return false;

	for (i = 0; i < ngroup; i++)
	{
		if (groupattno[i] == var->varattno)
			return true;
	}
	if (list_member_int((List *) nullattnos, (int) var->varattno))
		return true;
	if (list_member_int((List *) nonnullattnos, (int) var->varattno))
		return true;

	return bms_is_member(var->varattno, lion_notnullattnums(root, rel));
}

/*
 * Is the aggregate a count(DISTINCT col) of a plain column of rti (DESIGN.md
 * §26)?  Then *var is the column, *eqop the equality the DISTINCT compares
 * with and *collation the collation it compares under, which is what the
 * column's index has to agree with - checked per relation in
 * lion_collect_targets(), exactly as for a grouping column.
 *
 * The equality is the aggregate's own SortGroupClause's, the type's default
 * btree equality that nodeAgg deduplicates with, and the collation is the
 * ARGUMENT's (exprCollation(), before any relabel is stripped): that is what
 * nodeAgg sorts and compares the inputs under, so `count(DISTINCT t COLLATE
 * "C")` asks for "C" whatever the column itself says.
 */
bool
lion_agg_distinct_var(Aggref *agg, Index rti, Var **var, Oid *eqop,
					  Oid *collation)
{
	TargetEntry *tle;
	SortGroupClause *sgc;
	Node	   *arg;

	if (agg->aggfnoid != F_COUNT_ANY || agg->aggdistinct == NIL)
		return false;
	if (agg->aggorder != NIL || agg->aggfilter != NULL || agg->aggvariadic ||
		agg->aggstar)
		return false;
	if (agg->agglevelsup != 0 || agg->aggsplit != AGGSPLIT_SIMPLE ||
		agg->aggkind != AGGKIND_NORMAL)
		return false;
	if (list_length(agg->args) != 1 || list_length(agg->aggdistinct) != 1)
		return false;

	tle = (TargetEntry *) linitial(agg->args);
	sgc = (SortGroupClause *) linitial(agg->aggdistinct);
	if (!IsA(tle, TargetEntry) || !IsA(sgc, SortGroupClause))
		return false;
	if (sgc->tleSortGroupRef != tle->ressortgroupref ||
		!OidIsValid(sgc->eqop))
		return false;

	arg = lion_strip((Node *) tle->expr);
	if (arg == NULL || !IsA(arg, Var))
		return false;
	*var = (Var *) arg;
	if ((*var)->varno != (int) rti || (*var)->varattno <= 0 ||
		(*var)->varlevelsup != 0)
		return false;

	*eqop = sgc->eqop;
	*collation = exprCollation((Node *) tle->expr);
	return true;
}

/*
 * Is a GROUP BY expression `coalesce(col, c)` one the entry walk can group by
 * exactly (DESIGN.md §10, "coalesce")?  Its groups are col's, except that the
 * rows where col is NULL belong to the group of c - which is c's own group
 * when some row has col = c, and a group of their own printed as c when none
 * does.  lion_next_group() holds both back and emits them as one row after
 * the last entry, so all this has to prove is that "the entry whose key is c"
 * means exactly what the GROUP BY means by it:
 *
 *	- two arguments, a plain column of the relation and a non-NULL Const.
 *	  eval_const_expressions() has already dropped NULL arguments and
 *	  everything after the first non-NULL constant, and folded a constant
 *	  expression into its value; anything else - a Param, a stable function,
 *	  a second column - declines;
 *	- no coercion anywhere: the column, the Const and the expression all of
 *	  the column's own type, so that the grouping equality - the planner's
 *	  SortGroupClause.eqop, which lion_collect_targets() requires to be the
 *	  driving index's strategy 1, as for a bare column - compares c with the
 *	  stored keys as it compares the rows.  A cast of the column (int4 to
 *	  int8, say) makes the first argument a function call and declines, even
 *	  where the cast would preserve equality, and so does a relabelled one;
 *	- and the column's own collation for the expression: the index's
 *	  collation is checked against the column's, per relation, and so the
 *	  lookup of c is made under the very collation the GROUP BY compares
 *	  under (`coalesce(t, 'x' COLLATE "C")` declines).
 *
 * The value printed for the group of c is the Const - the constant as the
 * query wrote it, as core prints it for a group of NULL rows; for c's own
 * group that is the value the rows hold too, because a key the target list
 * prints is only ever produced by an index whose equality implies an
 * identical representation (the value gate of lion_collect_targets()).
 */
bool
lion_group_coalesce(CoalesceExpr *ce, Index rti, Var **var, Const **con)
{
	Node	   *a0;
	Node	   *a1;
	Var		   *v;
	Const	   *c;

	if (list_length(ce->args) != 2)
		return false;
	a0 = (Node *) linitial(ce->args);
	a1 = (Node *) lsecond(ce->args);
	if (!IsA(a0, Var) || !IsA(a1, Const))
		return false;
	v = (Var *) a0;
	c = (Const *) a1;
	if (v->varno != (int) rti || v->varattno <= 0 || v->varlevelsup != 0)
		return false;
	if (c->constisnull)
		return false;
	if (ce->coalescetype != v->vartype || c->consttype != v->vartype)
		return false;
	if (ce->coalescecollid != v->varcollid)
		return false;

	*var = v;
	*con = c;
	return true;
}

/*
 * The partially-grouped PathTarget a partitioned GROUP BY node produces
 * (DESIGN.md §16): the grouping column(s) unchanged, plus every aggregate
 * marked as the INITIAL phase of a split aggregate.  For count(*) and
 * count(col) the transition type is int8 and there is nothing to serialize,
 * so a partial row is just (group key, int8 partial count).
 *
 * This is make_partial_grouping_target() (src/backend/optimizer/plan/
 * planner.c) applied to our own grouped target, and it has to stay that:
 * setrefs.c re-derives the partial Aggrefs from the Finalize Agg's own ones
 * (convert_combining_aggrefs()) and matches them against the subplan's target
 * list with equal(), so an Aggref that differs in any field - the aggsplit
 * above all - would not be found.
 *
 * Our target only ever holds plain Vars and count Aggrefs at the top level
 * (the checks above refuse everything else), which is why pull_var_clause()
 * needs no SRF or window handling here.
 */
PathTarget *
lion_make_partial_target(PlannerInfo *root, PathTarget *grouping_target,
						 List *having)
{
	PathTarget *partial_target = create_empty_pathtarget();
	List	   *non_group_cols = NIL;
	List	   *non_group_exprs;
	int			i = 0;
	ListCell   *lc;

	foreach(lc, grouping_target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		Index		sgref = get_pathtarget_sortgroupref(grouping_target, i);

		if (sgref && root->processed_groupClause &&
			get_sortgroupref_clause_noerr(sgref,
										  root->processed_groupClause) != NULL)
		{
			/*
			 * A grouping column, carried through as it is - sortgroupref and
			 * all, because that is how the Finalize Agg finds it again
			 * (set_upper_references() matches group items by sortgroupref
			 * first, and make_agg() numbers its grouping columns that way).
			 */
			add_column_to_pathtarget(partial_target, expr, sgref);
		}
		else
			non_group_cols = lappend(non_group_cols, expr);
		i++;
	}

	/*
	 * A count the HAVING alone mentions has to be produced as well, or the
	 * Finalize Agg would have nothing to combine for it; core's version does
	 * the same with the havingQual.
	 */
	if (having != NIL)
		non_group_cols = lappend(non_group_cols, having);
	non_group_exprs = pull_var_clause((Node *) non_group_cols,
									  PVC_INCLUDE_AGGREGATES |
									  PVC_RECURSE_WINDOWFUNCS |
									  PVC_INCLUDE_PLACEHOLDERS);
	add_new_columns_to_pathtarget(partial_target, non_group_exprs);

	foreach(lc, partial_target->exprs)
	{
		Aggref	   *aggref = (Aggref *) lfirst(lc);

		if (IsA(aggref, Aggref))
		{
			Aggref	   *newaggref = makeNode(Aggref);

			/* Flat-copy, as core does, so no other tree is damaged. */
			memcpy(newaggref, aggref, sizeof(Aggref));
			mark_partial_aggref(newaggref, AGGSPLIT_INITIAL_SERIAL);
			lfirst(lc) = newaggref;
		}
	}

	list_free(non_group_exprs);
	list_free(non_group_cols);

	return set_pathtarget_cost_width(root, partial_target);
}

/*
 * The functions of an expression the executor would check EXECUTE on when it
 * initialised it, found as ExecInitExprRec() finds them: a function call's,
 * an operator's (OpExpr, DistinctExpr, NullIfExpr), and a ScalarArrayOpExpr's
 * comparison - its negator's for a hashed NOT IN - plus the hash function of
 * a hashed one.  Every clause the node answers is made of these, Vars and
 * values.
 */
static bool
lion_replaced_funcs_walker(Node *node, List **funcs)
{
	if (node == NULL)
		return false;
	if (IsA(node, FuncExpr))
		*funcs = list_append_unique_oid(*funcs, ((FuncExpr *) node)->funcid);
	else if (IsA(node, OpExpr) || IsA(node, DistinctExpr) ||
			 IsA(node, NullIfExpr))
	{
		OpExpr	   *op = (OpExpr *) node;

		*funcs = list_append_unique_oid(*funcs,
										OidIsValid(op->opfuncid) ?
										op->opfuncid : get_opcode(op->opno));
	}
	else if (IsA(node, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) node;

		if (OidIsValid(saop->negfuncid))
			*funcs = list_append_unique_oid(*funcs, saop->negfuncid);
		else
			*funcs = list_append_unique_oid(*funcs,
											OidIsValid(saop->opfuncid) ?
											saop->opfuncid :
											get_opcode(saop->opno));
		if (OidIsValid(saop->hashfuncid))
			*funcs = list_append_unique_oid(*funcs, saop->hashfuncid);
	}
	else if (IsA(node, Aggref))
		return false;			/* lion_replaced_aggs_walker()'s */
	return expression_tree_walker(node, lion_replaced_funcs_walker,
								  (void *) funcs);
}

/*
 * funcs plus the functions of expr the executor would check EXECUTE on: an
 * expression column's (DESIGN.md §41), which core's plan computes in the
 * scan's projection.
 */
List *
lion_expr_functions(Node *expr, List *funcs)
{
	(void) lion_replaced_funcs_walker(expr, &funcs);
	return funcs;
}

/* The aggregates of an expression, which ExecInitAgg() would check. */
static bool
lion_replaced_aggs_walker(Node *node, List **aggs)
{
	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
	{
		*aggs = list_append_unique_oid(*aggs, ((Aggref *) node)->aggfnoid);
		return false;
	}
	return expression_tree_walker(node, lion_replaced_aggs_walker,
								  (void *) aggs);
}

/*
 * What the node stands in for, as custom_private member LION_PRIV_EXECUTE:
 * the functions the executor would have asked EXECUTE on for the plan the
 * node replaces, so that lion_check_replaced_execute() can ask the same of
 * whoever runs it (DESIGN.md §9, "Privileges"; the 2026-09-23 review found a
 * pushed-down count answering after REVOKE EXECUTE on count() or int4eq).
 * Verified against core on every supported release:
 *
 *	- the Agg checks each aggregate (tlexprs, having) for the current user,
 *	  and its transition and final functions for the aggregate's owner;
 *	- the scan initialises its quals, rel's baserestrictinfo - every one of
 *	  which the node answers, OR trees and IN lists included - and so checks
 *	  each operator's function (and a hashed IN list's hash function);
 *	- a GROUP BY's Agg checks each grouping column's equality function
 *	  (ExecBuildGroupingEqual(), hashed or sorted), but not the hash or sort
 *	  support functions;
 *	- a hash or nested-loop join initialises its join clause, so an FK-side
 *	  join (fj) adds the join operator's function.  A merge join compares
 *	  through the btree support function unchecked, so a query the planner
 *	  would have merge-joined asks one privilege more of the node; the SQL
 *	  meaning of the query - it calls `=` - is the one kept.
 *
 * Not included: count(DISTINCT k)'s equality, which nodeAgg calls through an
 * unchecked FmgrInfo for a single column, so core runs the query with that
 * function revoked and so does the node; and the projection and the HAVING
 * evaluated over the finished counts, and a non-literal clause value, which
 * are initialised with ExecInitExpr() and so checked by core as they stand.
 * (A clause value that calls functions - `now() - interval '1 day'` - has
 * them in baserestrictinfo as well, so they are in the list too.)  A boolean
 * column tested by itself (`WHERE flag`) calls nothing in core's plan either:
 * the node's `flag = true` is looked up through the index, as core's index
 * scan looks it up, without asking EXECUTE on booleq.
 */
List *
lion_replaced_functions(RelOptInfo *rel, List *tlexprs, List *having,
						List *groupclause, const LionFkJoin *fj)
{
	List	   *aggs = NIL;
	List	   *funcs = NIL;
	List	   *groupfuncs = NIL;
	ListCell   *lc;

	foreach(lc, rel->baserestrictinfo)
		(void) lion_replaced_funcs_walker((Node *) ((RestrictInfo *) lfirst(lc))->clause,
										  &funcs);
	if (fj != NULL)
		funcs = list_append_unique_oid(funcs, get_opcode(fj->opno));
	foreach(lc, groupclause)
		groupfuncs = list_append_unique_oid(groupfuncs,
											get_opcode(((SortGroupClause *) lfirst(lc))->eqop));
	(void) lion_replaced_aggs_walker((Node *) tlexprs, &aggs);
	(void) lion_replaced_aggs_walker((Node *) having, &aggs);

	return list_make3(aggs, funcs, groupfuncs);
}
