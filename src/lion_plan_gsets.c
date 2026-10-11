/*-------------------------------------------------------------------------
 *
 * lion_plan_gsets.c
 *		A grouping counted by the node at a finer grain, and core's Agg over
 *		its groups: GROUP BY ROLLUP, CUBE and GROUPING SETS over one relation
 *		(DESIGN.md §10, "Grouping sets"), and a GROUP BY whose DISTINCT
 *		aggregates read columns outside it ("DISTINCT aggregates over a finer
 *		grouping").
 *
 * Every group of the query's is the union of some groups of a finer
 * grouping - for grouping sets the union of the sets, for a DISTINCT
 * aggregate the GROUP BY with the aggregate's columns added - so the finer
 * grouping, counted by the node as the plain GROUP BY it is, holds everything
 * the query asks of its rows, as long as each aggregate is one the counts
 * answer.  The plan is core's own Agg over the node's rows instead of the
 * table's: one row a group of the finer grouping, carrying its count n, with
 * every aggregate rewritten over the rows as the FK-side join's rows mode
 * does (DESIGN.md §27) - count(*) is the sum of n, count(x) of a column of
 * the finer grouping the sum of n where x is not NULL, sum(x) of an int2 or
 * int4 column of it the sum of x * n, and an aggregate that a group's
 * repeated rows do not change (min, max, bool_and, bool_or, anything
 * DISTINCT) is left as it stands.  The Agg goes into the grouped rel below a
 * LionJoinAgg, whose target is the query's own.  GROUPING() is the Agg's, as
 * it would be over the table.
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

/*
 * Is every Var of an aggregate a column of the finer grouping, and is there
 * nothing else it reads?  Each column's equality must also be its value's
 * identity (lion_type_equalimage()): the finer grouping keeps one value of
 * each group, which an expression of it inside the aggregate - `count(DISTINCT
 * n::text)` of a numeric n, whose 1.0 and 1.00 are one group - would
 * otherwise see in place of the others.
 */
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
		TypeCacheEntry *typentry;

		if (!IsA(v, Var) || !list_member(cx->gvars, v))
			return false;
		typentry = lookup_type_cache(((Var *) v)->vartype,
									 TYPECACHE_BTREE_OPFAMILY);
		if (!OidIsValid(typentry->btree_opintype) ||
			!lion_type_equalimage(typentry->btree_opintype,
								  ((Var *) v)->varcollid))
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
 * The finer grouping's paths into inner: lion_try_count_path() of the plain
 * GROUP BY of groupclause, for which the query has that GROUP BY, no grouping
 * sets, no LIMIT - which is the query's, and which the top k (§36) would cut
 * the finer grouping to - and tlist, the target list below the grouping.
 */
static void
lion_gsets_count_finer(PlannerInfo *root, RelOptInfo *input_rel,
					   RelOptInfo *inner, GroupPathExtraData *innerextra,
					   List *groupclause, List *tlist)
{
	Query	   *parse = root->parse;
	List	   *savedsets = parse->groupingSets;
	Node	   *savedlimit = parse->limitCount;
	List	   *savedtlist = root->processed_tlist;
	List	   *savedgroup = parse->groupClause;
	List	   *savedprocessed = root->processed_groupClause;
	RelOptInfo *pinned = lion_units_pin_move(inner);

	parse->groupingSets = NIL;
	parse->limitCount = NULL;
	root->processed_tlist = tlist;
	parse->groupClause = groupclause;
	root->processed_groupClause = groupclause;
	PG_TRY();
	{
		lion_try_count_path(root, input_rel, inner, innerextra, NULL);
	}
	PG_FINALLY();
	{
		parse->groupingSets = savedsets;
		parse->limitCount = savedlimit;
		root->processed_tlist = savedtlist;
		parse->groupClause = savedgroup;
		root->processed_groupClause = savedprocessed;
		(void) lion_units_pin_move(pinned);
	}
	PG_END_TRY();
}

/*
 * The finer grouping groupclause of input_rel, each of whose columns is a
 * column of the relation in tlist, counted into a rel of its own whose target
 * is those columns - with their sortgrouprefs, below the grouping step - and
 * a count(*); and the query's target and HAVING rewritten over its rows into
 * *aggtarget and *having.  NULL when an aggregate is not one the counts answer
 * or the node does not count the finer grouping.  The rel's paths are priced
 * in the units pinned for the grouped rel (§39), whose competitor they meet.
 */
static RelOptInfo *
lion_gsets_finer(PlannerInfo *root, RelOptInfo *input_rel,
				 RelOptInfo *output_rel, GroupPathExtraData *extra,
				 List *groupclause, List *tlist,
				 PathTarget **aggtarget, List **having)
{
	LionGsetsRewrite cx;
	PathTarget *innertarget;
	RelOptInfo *inner;
	GroupPathExtraData innerextra;
	Aggref	   *n;
	ListCell   *lc;

	memset(&cx, 0, sizeof(cx));
	cx.root = root;
	cx.ok = true;

	/* the grouping columns, each a column of the relation */
	innertarget = create_empty_pathtarget();
	foreach(lc, groupclause)
	{
		SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
		TargetEntry *tle = get_sortgroupclause_tle(sgc, tlist);
		Var		   *v;

		if (tle == NULL || !IsA(tle->expr, Var))
			return NULL;
		v = (Var *) tle->expr;
		if (v->varno != (int) input_rel->relid || v->varattno <= 0 ||
			v->varlevelsup != 0)
			return NULL;
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
		return NULL;
	if (!lion_join_aggs_lookup(NULL, &cx.sumfn, &cx.mul2op, &cx.mul4op))
		cx.sumfn = InvalidOid;
	*aggtarget = copy_pathtarget(output_rel->reltarget);
	(*aggtarget)->exprs = (List *) lion_gsets_mutator((Node *) (*aggtarget)->exprs,
													  &cx);
	*having = NIL;
	if (extra->havingQual != NULL)
		*having = (List *) lion_gsets_mutator(extra->havingQual, &cx);
	if (!cx.ok)
		return NULL;

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

	lion_gsets_count_finer(root, input_rel, inner, &innerextra, groupclause,
						   tlist);
	if (inner->pathlist == NIL)
		return NULL;
	return inner;
}

/*
 * The query's aggregates, which the LionJoinAgg asks EXECUTE on as the Agg it
 * replaces would (DESIGN.md §9, "Privileges"): the Agg below asks it of what
 * it computes, the extension's aggregates and those left as they stand, and
 * the node below of count().
 */
static List *
lion_gsets_checkaggs(RelOptInfo *output_rel, GroupPathExtraData *extra)
{
	return (List *) linitial(lion_replaced_functions(output_rel,
													 output_rel->reltarget->exprs,
													 (List *) extra->havingQual,
													 NIL, NULL));
}

/*
 * GROUP BY ROLLUP, CUBE or GROUPING SETS over input_rel, a base relation
 * (DESIGN.md §10, "Grouping sets"): the finest grouping - the union of the
 * sets - counted into a rel of its own by lion_try_count_path(), and core's
 * grouping sets Agg over each of its paths, below a LionJoinAgg, into
 * output_rel.  Every failed check simply returns: the normal plan is always
 * available.
 */
void
lion_try_gsets_path(PlannerInfo *root, RelOptInfo *input_rel,
					RelOptInfo *output_rel, GroupPathExtraData *extra)
{
	Query	   *parse = root->parse;
	PathTarget *aggtarget;
	List	   *having;
	List	   *tlist;
	RelOptInfo *inner;
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

	tlist = (List *) lion_gsets_below(root, (Node *) root->processed_tlist);
	inner = lion_gsets_finer(root, input_rel, output_rel, extra,
							 root->processed_groupClause, tlist,
							 &aggtarget, &having);
	if (inner == NULL)
		return;
	checkaggs = lion_gsets_checkaggs(output_rel, extra);

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

/*
 * A column a DISTINCT aggregate reads that the GROUP BY does not, appended to
 * *groupclause and to *tlist (as a resjunk entry) as one more grouping column
 * of the finer grouping, under the next free sortgroupref.  False for a
 * column the finer grouping cannot group by.
 */
static bool
lion_gsets_widen(Var *v, Index rti, List **groupclause, List **tlist,
				 Index *nextref)
{
	SortGroupClause *sgc;
	Oid			sortop;
	Oid			eqop;
	bool		hashable;
	ListCell   *lc;

	if (v->varno != (int) rti || v->varattno <= 0 || v->varlevelsup != 0)
		return false;
	foreach(lc, *groupclause)
	{
		TargetEntry *tle = get_sortgroupclause_tle((SortGroupClause *) lfirst(lc),
												   *tlist);

		if (tle != NULL && equal(tle->expr, v))
			return true;		/* grouped by already */
	}
	get_sort_group_operators(v->vartype, false, false, false,
							 &sortop, &eqop, NULL, &hashable);
	if (!OidIsValid(sortop) || !OidIsValid(eqop))
		return false;
	sgc = makeNode(SortGroupClause);
	sgc->tleSortGroupRef = (*nextref)++;
	sgc->eqop = eqop;
	sgc->sortop = sortop;
	sgc->nulls_first = false;
	sgc->hashable = hashable;
	*groupclause = lappend(*groupclause, sgc);
	*tlist = lappend(*tlist, makeTargetEntry((Expr *) copyObject(v),
											 list_length(*tlist) + 1, NULL,
											 true));
	((TargetEntry *) llast(*tlist))->ressortgroupref = sgc->tleSortGroupRef;
	return true;
}

/* The DISTINCT aggregates of an expression, onto *aggs. */
static bool
lion_gsets_distinct_walker(Node *node, List **aggs)
{
	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
	{
		if (((Aggref *) node)->aggdistinct != NIL)
			*aggs = lappend(*aggs, node);
		return false;
	}
	return expression_tree_walker(node, lion_gsets_distinct_walker,
								  (void *) aggs);
}

/*
 * A GROUP BY over input_rel, a base relation, whose DISTINCT aggregates read
 * columns outside it (DESIGN.md §10, "DISTINCT aggregates over a finer
 * grouping"): `count(DISTINCT c) ... GROUP BY a, b` is the number of groups
 * of `GROUP BY a, b, c` within each group of a, b with c not NULL, so the
 * GROUP BY with the aggregates' columns added is counted by the node, and
 * core's Agg forms the query's groups over its rows, the DISTINCT aggregates
 * standing as they are.  Its input is sorted by the query's group pathkeys,
 * which core extends with a DISTINCT aggregate's own keys when it marks the
 * aggregate presorted (adjust_group_pathkeys_for_groupagg()): the Agg then
 * relies on that order, as it would over the table.  Every failed check
 * simply returns.
 */
void
lion_try_distinct_agg_path(PlannerInfo *root, RelOptInfo *input_rel,
						   RelOptInfo *output_rel, GroupPathExtraData *extra)
{
	Query	   *parse = root->parse;
	List	   *aggs = NIL;
	List	   *groupclause;
	List	   *tlist;
	Index		nextref = 0;
	PathTarget *aggtarget;
	List	   *having;
	RelOptInfo *inner;
	List	   *checkaggs;
	List	   *groupexprs;
	ListCell   *lc;

	if (extra == NULL || parse->groupingSets != NIL ||
		root->processed_groupClause == NIL ||
		parse->hasTargetSRFs || parse->hasDistinctOn ||
		parse->distinctClause != NIL || root->group_pathkeys == NIL ||
		!grouping_is_sortable(root->processed_groupClause))
		return;
	if (input_rel->reloptkind != RELOPT_BASEREL)
		return;

	/*
	 * Only where the node has not counted the GROUP BY as it stands - the
	 * Distinct Key of §26, over at most one grouping column, is
	 * a walk of its own, and a plan that groups by the column too would only
	 * compete with it.
	 */
	foreach(lc, output_rel->pathlist)
	{
		if (lion_path_has_lion((Path *) lfirst(lc)))
			return;
	}

	(void) lion_gsets_distinct_walker((Node *) output_rel->reltarget->exprs,
									  &aggs);
	(void) lion_gsets_distinct_walker(extra->havingQual, &aggs);
	if (aggs == NIL)
		return;

	/* the GROUP BY, and each column of a DISTINCT aggregate after it */
	tlist = list_copy(root->processed_tlist);
	foreach(lc, tlist)
		nextref = Max(nextref, ((TargetEntry *) lfirst(lc))->ressortgroupref);
	nextref++;
	groupclause = list_copy(root->processed_groupClause);
	foreach(lc, aggs)
	{
		List	   *vars = pull_var_clause((Node *) lfirst(lc),
										   PVC_RECURSE_AGGREGATES |
										   PVC_INCLUDE_PLACEHOLDERS);
		ListCell   *lv;

		foreach(lv, vars)
		{
			if (!IsA(lfirst(lv), Var) ||
				!lion_gsets_widen((Var *) lfirst(lv), input_rel->relid,
								  &groupclause, &tlist, &nextref))
				return;
		}
	}
	if (list_length(groupclause) == list_length(root->processed_groupClause))
		return;					/* the plain GROUP BY's own */

	inner = lion_gsets_finer(root, input_rel, output_rel, extra, groupclause,
							 tlist, &aggtarget, &having);
	if (inner == NULL)
		return;
	checkaggs = lion_gsets_checkaggs(output_rel, extra);
	groupexprs = get_sortgrouplist_exprs(root->processed_groupClause, tlist);

	foreach(lc, inner->pathlist)
	{
		Path	   *sub = (Path *) lfirst(lc);
		AggPath    *apath;

		if (!pathkeys_contained_in(root->group_pathkeys, sub->pathkeys))
			sub = (Path *) create_sort_path(root, inner, sub,
											root->group_pathkeys, -1.0);
		apath = create_agg_path(root, output_rel, sub, aggtarget, AGG_SORTED,
								AGGSPLIT_SIMPLE, root->processed_groupClause,
								having, &extra->agg_final_costs,
								estimate_num_groups(root, groupexprs,
													sub->rows, NULL, NULL));
		add_path(output_rel, (Path *) lion_join_agg_wrap(root, output_rel,
														 (Path *) apath,
														 checkaggs));
	}
}
