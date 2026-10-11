/*-------------------------------------------------------------------------
 *
 * lion_plan_gsets.c
 *		GROUP BY ROLLUP, CUBE and GROUPING SETS over one relation
 *		(DESIGN.md §10, "Grouping sets").
 *
 * Every grouping set of a query is a subset of the union of its sets, so each
 * set's groups are unions of the union's groups: the finest grouping, counted
 * by the node as the plain GROUP BY of every grouping column it is, holds
 * everything the sets ask of their rows - as long as each aggregate is one
 * the counts answer.  The plan is core's own grouping sets Agg, over the
 * node's rows instead of the table's: one row a group of the finest
 * grouping, carrying its count n, with every aggregate rewritten over the
 * rows as the FK-side join's rows mode does (DESIGN.md §27) - count(*) is
 * the sum of n, count(x) of a grouping column the sum of n where x is not
 * NULL, sum(x) of an int2 or int4 grouping column the sum of x * n, and an
 * aggregate that a group's repeated rows do not change (min, max, bool_and,
 * bool_or, anything DISTINCT) is left as it stands.  The Agg goes into the
 * grouped rel below a LionJoinAgg, whose target is the query's own.
 * GROUPING() is the Agg's, as it would be over the table.
 *
 * Part of the LionCount custom scan: lion_customscan.h describes the node
 * and declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_customscan.h"

/*
 * What a query's aggregates are rewritten with over the finest grouping's
 * rows: the grouping columns, as they are below the grouping, and the count
 * column n.
 */
typedef struct LionGsetsRewrite
{
	PlannerInfo *root;
	List	   *gvars;			/* the grouping columns, below the grouping */
	Aggref	   *n;				/* the inner count(*) */
	Oid			countfn;
	Oid			sumfn;
	Oid			mul2op;
	Oid			mul4op;
	bool		ok;				/* nothing declined so far */
} LionGsetsRewrite;

/*
 * An expression as it is below the grouping step.  Since PostgreSQL 18 a
 * grouping set's grouping columns, above it, are nulled by the grouping
 * step's RT index (the sets that leave a column out make it NULL); the rows
 * the Agg reads are below it.
 */
static Node *
lion_gsets_below(PlannerInfo *root, Node *node)
{
#if PG_VERSION_NUM >= 180000
	if (root->parse->hasGroupRTE && root->group_rtindex > 0)
		return remove_nulling_relids(node,
									 bms_make_singleton(root->group_rtindex),
									 NULL);
#endif
	return node;
}

/* Is every Var of node a grouping column, and is there nothing else it reads? */
static bool
lion_gsets_reads_groups(LionGsetsRewrite *cx, Node *node)
{
	List	   *vars = pull_var_clause(node, PVC_RECURSE_AGGREGATES |
									   PVC_RECURSE_WINDOWFUNCS |
									   PVC_INCLUDE_PLACEHOLDERS);
	ListCell   *lc;

	foreach(lc, vars)
	{
		Node	   *v = lion_gsets_below(cx->root, (Node *) lfirst(lc));

		if (!IsA(v, Var) || !list_member(cx->gvars, v))
			return false;
	}
	return true;
}

/*
 * Is agg one that the repeated rows of a group do not change - an aggregate
 * with a sort operator (min, max, bool_and, bool_or: pg_aggregate.aggsortop
 * says the aggregate is the first of its input in that order), or any
 * aggregate over DISTINCT arguments?
 */
static bool
lion_gsets_agg_idempotent(Aggref *agg)
{
	HeapTuple	tup;
	bool		sortop;

	if (agg->aggdistinct != NIL)
		return true;
	tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(agg->aggfnoid));
	if (!HeapTupleIsValid(tup))
		return false;
	sortop = OidIsValid(((Form_pg_aggregate) GETSTRUCT(tup))->aggsortop);
	ReleaseSysCache(tup);
	return sortop;
}

/* lion_join_count(n) or lion_join_sum(arg) in place of orig, under filter */
static Aggref *
lion_gsets_make_agg(Oid aggfnoid, Expr *arg, Expr *filter, Aggref *orig)
{
	Aggref	   *agg = makeNode(Aggref);

	agg->aggfnoid = aggfnoid;
	agg->aggtype = INT8OID;
	agg->aggcollid = InvalidOid;
	agg->inputcollid = InvalidOid;
	agg->aggtranstype = INT8OID;
	agg->aggargtypes = list_make1_oid(INT8OID);
	agg->args = list_make1(makeTargetEntry(arg, 1, NULL, false));
	agg->aggfilter = filter;
	agg->aggkind = AGGKIND_NORMAL;
	agg->aggsplit = AGGSPLIT_SIMPLE;
	agg->aggno = orig->aggno;
	agg->aggtransno = orig->aggtransno;
	agg->location = orig->location;
	return agg;
}

/* filter AND clause, either of which may be NULL */
static Expr *
lion_gsets_and(Expr *filter, Expr *clause)
{
	if (filter == NULL)
		return clause;
	if (clause == NULL)
		return filter;
	return make_andclause(list_make2(filter, clause));
}

/*
 * The target list or HAVING of the grouping sets Agg: each aggregate over the
 * finest grouping's rows, and everything else as it stands.  Clears cx->ok
 * on anything the rows cannot answer exactly.
 */
static Node *
lion_gsets_mutator(Node *node, LionGsetsRewrite *cx)
{
	if (node == NULL || !cx->ok)
		return node;
	if (IsA(node, Aggref))
	{
		Aggref	   *agg = (Aggref *) node;
		Expr	   *filter = (Expr *) copyObject(agg->aggfilter);
		Expr	   *arg;

		/*
		 * The aggregate reads grouping columns only, and evaluates nothing
		 * whose value may change from one row to the next: it is evaluated
		 * once a group of the finest grouping instead of once a row.
		 */
		if (agg->agglevelsup != 0 || agg->aggkind != AGGKIND_NORMAL ||
			!lion_gsets_reads_groups(cx, node) ||
			contain_volatile_functions(node) || contain_subplans(node))
		{
			cx->ok = false;
			return node;
		}
		if (lion_gsets_agg_idempotent(agg))
			return node;
		if (agg->aggorder != NIL)
		{
			cx->ok = false;
			return node;
		}
		if (agg->aggfnoid == F_COUNT_)
			return (Node *) lion_gsets_make_agg(cx->countfn,
												(Expr *) copyObject(cx->n),
												filter, agg);
		if (agg->aggfnoid == F_COUNT_ANY)
		{
			NullTest   *nt = makeNode(NullTest);

			/* a NULL datum, as count() sees it: not a row's fields */
			nt->arg = (Expr *) copyObject(((TargetEntry *) linitial(agg->args))->expr);
			nt->nulltesttype = IS_NOT_NULL;
			nt->argisrow = false;
			nt->location = -1;
			return (Node *) lion_gsets_make_agg(cx->countfn,
												(Expr *) copyObject(cx->n),
												lion_gsets_and(filter,
															   (Expr *) nt),
												agg);
		}
		if ((agg->aggfnoid == F_SUM_INT2 || agg->aggfnoid == F_SUM_INT4) &&
			OidIsValid(cx->sumfn))
		{
			Expr	   *x = (Expr *) copyObject(((TargetEntry *) linitial(agg->args))->expr);
			OpExpr	   *product;

			product = (OpExpr *) make_opclause(agg->aggfnoid == F_SUM_INT2 ?
											   cx->mul2op : cx->mul4op,
											   INT8OID, false, x,
											   (Expr *) copyObject(cx->n),
											   InvalidOid, InvalidOid);
			set_opfuncid(product);
			arg = (Expr *) product;
			return (Node *) lion_gsets_make_agg(cx->sumfn, arg, filter, agg);
		}
		cx->ok = false;
		return node;
	}
	if (IsA(node, GroupingFunc))
		return node;
	if (IsA(node, Var))
	{
		/* outside an aggregate: a grouping column, above the grouping */
		if (!list_member(cx->gvars, lion_gsets_below(cx->root, node)))
			cx->ok = false;
		return node;
	}
	if (IsA(node, PlaceHolderVar) || IsA(node, WindowFunc) ||
		IsA(node, SubPlan) || IsA(node, AlternativeSubPlan))
	{
		cx->ok = false;
		return node;
	}
	return expression_tree_mutator(node, lion_gsets_mutator, (void *) cx);
}

/* qsort: grouping sets, the largest first, in the order given otherwise */
typedef struct LionGset
{
	Bitmapset  *refs;
	int			size;
	int			pos;
} LionGset;

static int
lion_gset_cmp(const void *a, const void *b)
{
	const LionGset *x = (const LionGset *) a;
	const LionGset *y = (const LionGset *) b;

	if (x->size != y->size)
		return (x->size > y->size) ? -1 : 1;
	return (x->pos < y->pos) ? -1 : (x->pos > y->pos) ? 1 : 0;
}

/*
 * The rollups of the grouping sets: chains of sets each of which holds the
 * next, made greedily from the largest set down, each set going to the first
 * chain whose last set holds it.  A chain is one sorted pass of the Agg: its
 * grouping clause is the columns of its smallest set, then those its next
 * set adds, and so on, so that each set is a prefix of it.  The first chain
 * holds the largest set and takes the Agg's sorted input; core sorts the
 * input again for each of the others.  numGroups of each set is estimated
 * over the finest grouping's rows.
 */
static List *
lion_gsets_rollups(PlannerInfo *root, List *tlist, double inrows)
{
	Query	   *parse = root->parse;
	int			nsets = list_length(parse->groupingSets);
	LionGset   *sets = palloc(sizeof(LionGset) * nsets);
	List	  **chains = palloc0(sizeof(List *) * nsets);
	int			nchains = 0;
	List	   *rollups = NIL;
	ListCell   *lc;
	int			i;

	i = 0;
	foreach(lc, parse->groupingSets)
	{
		List	   *set = (List *) lfirst(lc);
		ListCell   *lr;

		sets[i].refs = NULL;
		foreach(lr, set)
			sets[i].refs = bms_add_member(sets[i].refs, lfirst_int(lr));
		sets[i].size = bms_num_members(sets[i].refs);
		sets[i].pos = i;
		/* the same column twice in one set is left to core */
		if (sets[i].size != list_length(set))
			return NIL;
		i++;
	}
	qsort(sets, nsets, sizeof(LionGset), lion_gset_cmp);

	for (i = 0; i < nsets; i++)
	{
		int			c;

		for (c = 0; c < nchains; c++)
		{
			LionGset   *last = (LionGset *) llast(chains[c]);

			if (bms_is_subset(sets[i].refs, last->refs))
				break;
		}
		if (c == nchains)
			nchains++;
		chains[c] = lappend(chains[c], &sets[i]);
	}

	for (i = 0; i < nchains; i++)
	{
		RollupData *rollup = makeNode(RollupData);
		List	   *order = NIL;	/* the chain's columns, as sortgrouprefs */
		Bitmapset  *done = NULL;
		int			k;

		/* the columns of the smallest set first, then each larger set's */
		for (k = list_length(chains[i]) - 1; k >= 0; k--)
		{
			LionGset   *s = (LionGset *) list_nth(chains[i], k);

			foreach(lc, root->processed_groupClause)
			{
				SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);

				if (bms_is_member(sgc->tleSortGroupRef, s->refs) &&
					!bms_is_member(sgc->tleSortGroupRef, done))
				{
					order = lappend(order, sgc);
					done = bms_add_member(done, sgc->tleSortGroupRef);
				}
			}
		}
		rollup->groupClause = order;
		rollup->gsets = NIL;
		rollup->gsets_data = NIL;
		rollup->numGroups = 0;
		rollup->hashable = false;
		rollup->is_hashed = false;
		foreach(lc, chains[i])
		{
			LionGset   *s = (LionGset *) lfirst(lc);
			GroupingSetData *gs = makeNode(GroupingSetData);
			List	   *idx = NIL;
			List	   *exprs = NIL;
			ListCell   *lo;

			gs->set = NIL;
			k = 0;
			foreach(lo, order)
			{
				SortGroupClause *sgc = (SortGroupClause *) lfirst(lo);

				if (k >= s->size)
					break;
				idx = lappend_int(idx, k);
				gs->set = lappend_int(gs->set, sgc->tleSortGroupRef);
				exprs = lappend(exprs, get_sortgroupclause_expr(sgc, tlist));
				k++;
			}
			gs->numGroups = (exprs == NIL) ? 1.0 :
				estimate_num_groups(root, exprs, inrows, NULL, NULL);
			rollup->gsets = lappend(rollup->gsets, idx);
			rollup->gsets_data = lappend(rollup->gsets_data, gs);
			rollup->numGroups += gs->numGroups;
		}
		rollups = lappend(rollups, rollup);
	}
	return rollups;
}

/*
 * The finest grouping's paths into inner: lion_try_count_path() of the plain
 * GROUP BY of every grouping column, for which the query has no grouping
 * sets, no LIMIT and the target list below the grouping.
 */
static void
lion_gsets_count_finest(PlannerInfo *root, RelOptInfo *input_rel,
						RelOptInfo *inner, GroupPathExtraData *innerextra,
						List *tlist)
{
	Query	   *parse = root->parse;
	List	   *savedsets = parse->groupingSets;
	Node	   *savedlimit = parse->limitCount;
	List	   *savedtlist = root->processed_tlist;
	RelOptInfo *pinned = lion_units_pin_move(inner);

	parse->groupingSets = NIL;
	parse->limitCount = NULL;
	root->processed_tlist = tlist;
	PG_TRY();
	{
		lion_try_count_path(root, input_rel, inner, innerextra, NULL);
	}
	PG_FINALLY();
	{
		parse->groupingSets = savedsets;
		parse->limitCount = savedlimit;
		root->processed_tlist = savedtlist;
		(void) lion_units_pin_move(pinned);
	}
	PG_END_TRY();
}

/*
 * GROUP BY ROLLUP, CUBE or GROUPING SETS over input_rel, a base relation
 * (DESIGN.md §10, "Grouping sets"): the finest grouping counted into a rel of
 * its own by lion_try_count_path(), and core's grouping sets Agg over each of
 * its paths, below a LionJoinAgg, into output_rel.  Every failed check simply
 * returns: the normal plan is always available.
 */
void
lion_try_gsets_path(PlannerInfo *root, RelOptInfo *input_rel,
					RelOptInfo *output_rel, GroupPathExtraData *extra)
{
	Query	   *parse = root->parse;
	LionGsetsRewrite cx;
	PathTarget *innertarget;
	PathTarget *aggtarget;
	List	   *having = NIL;
	List	   *tlist;
	RelOptInfo *inner;
	GroupPathExtraData innerextra;
	Aggref	   *n;
	List	   *rollups;
	List	   *pathkeys;
	List	   *checkaggs;
	ListCell   *lc;

	if (extra == NULL || parse->groupingSets == NIL ||
		root->processed_groupClause == NIL ||
		parse->hasTargetSRFs || parse->hasDistinctOn ||
		parse->distinctClause != NIL ||
		!grouping_is_sortable(root->processed_groupClause))
		return;
	if (input_rel->reloptkind != RELOPT_BASEREL)
		return;

	memset(&cx, 0, sizeof(cx));
	cx.root = root;
	cx.ok = true;

	/* the grouping columns, each a column of the relation */
	tlist = (List *) lion_gsets_below(root, (Node *) root->processed_tlist);
	innertarget = create_empty_pathtarget();
	foreach(lc, root->processed_groupClause)
	{
		SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
		TargetEntry *tle = get_sortgroupclause_tle(sgc, tlist);
		Var		   *v;

		if (tle == NULL || !IsA(tle->expr, Var))
			return;
		v = (Var *) tle->expr;
		if (v->varno != (int) input_rel->relid || v->varattno <= 0 ||
			v->varlevelsup != 0)
			return;
		cx.gvars = lappend(cx.gvars, v);
		add_column_to_pathtarget(innertarget, (Expr *) v,
								 sgc->tleSortGroupRef);
	}

	/* ... and the count of each of their groups */
	n = makeNode(Aggref);
	n->aggfnoid = F_COUNT_;
	n->aggtype = INT8OID;
	n->aggtranstype = INT8OID;
	n->aggstar = true;
	n->aggkind = AGGKIND_NORMAL;
	n->aggsplit = AGGSPLIT_SIMPLE;
	n->location = -1;
	cx.n = n;
	add_column_to_pathtarget(innertarget, (Expr *) n, 0);
	set_pathtarget_cost_width(root, innertarget);

	/* the query's aggregates, over the counted groups */
	if (!lion_join_aggs_lookup(&cx.countfn, NULL, NULL, NULL))
		return;
	if (!lion_join_aggs_lookup(NULL, &cx.sumfn, &cx.mul2op, &cx.mul4op))
		cx.sumfn = InvalidOid;
	aggtarget = copy_pathtarget(output_rel->reltarget);
	aggtarget->exprs = (List *) lion_gsets_mutator((Node *) aggtarget->exprs,
												   &cx);
	if (extra->havingQual != NULL)
		having = (List *) lion_gsets_mutator(extra->havingQual, &cx);
	if (!cx.ok)
		return;

	/*
	 * The finest grouping, as the plain GROUP BY of every grouping column -
	 * not to be cut to the query's LIMIT, which is the sets' (§36's top k
	 * reads it) - into a rel of its own, priced in the units pinned for the
	 * grouped rel (§39), whose competitor it meets.
	 */
	inner = makeNode(RelOptInfo);
	memcpy(inner, output_rel, sizeof(RelOptInfo));
	inner->pathlist = NIL;
	inner->ppilist = NIL;
	inner->partial_pathlist = NIL;
	inner->cheapest_startup_path = NULL;
	inner->cheapest_total_path = NULL;
	inner->cheapest_parameterized_paths = NIL;
	inner->reltarget = innertarget;

	memset(&innerextra, 0, sizeof(innerextra));
	innerextra.flags = GROUPING_CAN_PARTIAL_AGG;
	innerextra.patype = PARTITIONWISE_AGGREGATE_NONE;
	innerextra.havingQual = NULL;
	innerextra.targetList = tlist;
	innerextra.target_parallel_safe = extra->target_parallel_safe;

	lion_gsets_count_finest(root, input_rel, inner, &innerextra, tlist);
	if (inner->pathlist == NIL)
		return;

	/*
	 * The query's aggregates, which the LionJoinAgg asks EXECUTE on as the
	 * Agg it replaces would (DESIGN.md §9, "Privileges"): the Agg below asks
	 * it of what it computes, the extension's aggregates and those left as
	 * they stand, and the node below of count().
	 */
	checkaggs = (List *) linitial(lion_replaced_functions(output_rel,
														  output_rel->reltarget->exprs,
														  (List *) extra->havingQual,
														  NIL, NULL));

	/* the Agg, over each of them sorted for its first rollup */
	foreach(lc, inner->pathlist)
	{
		Path	   *sub = (Path *) lfirst(lc);
		GroupingSetsPath *gpath;

		rollups = lion_gsets_rollups(root, tlist, sub->rows);
		if (rollups == NIL)
			return;
		pathkeys = make_pathkeys_for_sortclauses(root,
												 linitial_node(RollupData, rollups)->groupClause,
												 tlist);
		if (!pathkeys_contained_in(pathkeys, sub->pathkeys))
			sub = (Path *) create_sort_path(root, inner, sub, pathkeys, -1.0);
		gpath = create_groupingsets_path(root, output_rel, sub, having,
										 AGG_SORTED, rollups,
										 &extra->agg_final_costs);
		gpath->path.pathtarget = aggtarget;
		/* claims no order: its first rollup's need not be core's */
		gpath->path.pathkeys = NIL;
		add_path(output_rel, (Path *) lion_join_agg_wrap(root, output_rel,
														 (Path *) gpath,
														 checkaggs));
	}
}
