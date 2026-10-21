/*-------------------------------------------------------------------------
 *
 * lion_plan_anti.c
 *		The anti join over a small inner side (DESIGN.md §27, "The anti join
 *		over a small inner side"): the B-tree of the counted table its keys
 *		are looked up in, and the price of the lookups.
 *
 *			SELECT count(*) FROM t WHERE <clauses lion counts>
 *			  AND NOT EXISTS (SELECT 1 FROM s WHERE s.a = t.k ...)
 *
 * is the count of t's WHERE less the rows of it some row of s matches.  The
 * node runs s as its child, looks each distinct key of its rows up in a
 * B-tree on t.k under the query's snapshot, tests every row found on the join
 * clauses and on t's restrictions, and subtracts the ones that pass.  Which
 * B-tree, and what that costs, is decided here; lion_plan_count.c plans the
 * count and adds the two up, and lion_exec_anti.c runs it.
 *
 * Part of the LionCount custom scan: lion_customscan.h describes the node
 * and declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_customscan.h"

/*
 * The CPU price of a page a B-tree descent reads, in cpu_operator_costs: the
 * multiplier btcostestimate() charges each level of the descent with.
 */
#define LION_ANTI_PAGE_CPU		50.0

/*
 * Is `key`, a join clause of the anti join, a scan key of column c of the
 * B-tree idx?  The table's column has to be the index's key column c, and the
 * operator - turned round when the column is its right operand - an equality
 * of the column's operator family whose left type is the column's opclass
 * type, under the index's collation (or an index without one).  Fills the
 * operator the scan key is made of.
 */
static bool
lion_anti_key_of(IndexOptInfo *idx, int c, const LionAntiKey *key, Oid *keyop)
{
	Oid			op = key->clause->opno;
	int			strategy;
	Oid			lefttype;
	Oid			righttype;
	Oid			idxcoll = idx->indexcollations[c];

	if (idx->indexkeys[c] != key->outervar->varattno)
		return false;
	if (!key->outerleft)
		op = get_commutator(op);
	if (!OidIsValid(op))
		return false;
	if (get_op_opfamily_strategy(op, idx->opfamily[c]) != BTEqualStrategyNumber)
		return false;
	get_op_opfamily_properties(op, idx->opfamily[c], false, &strategy,
							   &lefttype, &righttype);
	if (lefttype != idx->opcintype[c])
		return false;
	if (OidIsValid(idxcoll) && idxcoll != key->clause->inputcollid)
		return false;
	*keyop = op;
	return true;
}

/*
 * The scan keys idx can be looked up by: for each key column from the first,
 * every key of the anti join that is one of that column's, as long as each
 * column has one - a B-tree is searched by a prefix of its columns.  Fills
 * *out's keys and returns how many columns they cover.
 */
static int
lion_anti_index_keys(const LionSmallAnti *anti, IndexOptInfo *idx,
					 LionAntiPlan *out)
{
	int			ncol = 0;
	int			c;

	out->nkey = 0;
	for (c = 0; c < idx->nkeycolumns; c++)
	{
		bool		found = false;
		ListCell   *lc;

		foreach(lc, anti->keys)
		{
			LionAntiKey *key = (LionAntiKey *) lfirst(lc);
			Oid			op;

			if (out->nkey >= LION_MAX_ANTI_KEYS ||
				!lion_anti_key_of(idx, c, key, &op))
				continue;
			out->keycol[out->nkey] = (AttrNumber) (c + 1);
			out->keyop[out->nkey] = op;
			out->keycoll[out->nkey] = key->clause->inputcollid;
			out->keyexpr[out->nkey] = key->innerexpr;
			out->keyvar[out->nkey] = key->outervar;
			out->nkey++;
			found = true;
		}
		if (!found)
			break;
		ncol++;
	}
	return ncol;
}

/*
 * May one row of the table be found by the lookups of two different keys?
 * Not when every scan key is of the column's own type, and the index's
 * equality on it is the type's own, under which equal values are the same
 * bytes (lion_index_can_emit_value()): keys the node keeps apart by their
 * bytes are then unequal, and so are the rows they find.  Otherwise - a
 * cross-type key, numeric's 1.0 and 1.00, a nondeterministic collation, an
 * opclass coarser than the type's equality - two keys may find one row, and
 * the node keeps the rows matched by TID, to subtract each once.
 */
static bool
lion_anti_needs_tids(const LionAntiPlan *plan)
{
	int			k;

	for (k = 0; k < plan->nkey; k++)
	{
		int			c = plan->keycol[k] - 1;

		if (exprType(plan->keyexpr[k]) != plan->idx->opcintype[c] ||
			!lion_index_can_emit_value(plan->idx, plan->keycol[k]))
			return true;
	}
	return false;
}

/*
 * The correlation of the table's order with the index's first column, as
 * btcostestimate() takes it: the column's own, from its statistics - three
 * quarters of it for a multicolumn index - or 0 without any.
 */
static double
lion_anti_correlation(PlannerInfo *root, const LionAntiPlan *plan)
{
	VariableStatData vardata;
	AttStatsSlot sslot;
	double		corr = 0.0;

	/* the first scan key is of the first column (lion_anti_index_keys()) */
	examine_variable(root, (Node *) plan->keyvar[0], 0, &vardata);
	if (HeapTupleIsValid(vardata.statsTuple) &&
		get_attstatsslot(&sslot, vardata.statsTuple,
						 STATISTIC_KIND_CORRELATION, InvalidOid,
						 ATTSTATSSLOT_NUMBERS))
	{
		if (sslot.nnumbers > 0)
			corr = sslot.numbers[0];
		free_attstatsslot(&sslot);
	}
	ReleaseVariableStats(vardata);
	if (plan->idx->nkeycolumns > 1)
		corr *= 0.75;
	return corr;
}

/*
 * The lookups' own price - every one a B-tree descent and the rows it finds,
 * each fetched from the heap and tested - priced as core prices an index scan
 * repeated once per outer row of a nested loop (cost_index() and
 * btcostestimate(), with their loop count): the pages of the index and of the
 * heap that many scans fetch, by index_pages_fetched() and the correlation of
 * the table's order with the index; the descent's comparisons and pages, the
 * index tuples read and their scan keys; and a cpu_tuple_cost and the clauses
 * for each row found, and the expressions it is evaluated on beside a count
 * by cells (`rowexprs`, the predicates) with a cpu_operator_cost each to find
 * its cell.  Beside them, what the node does with the child's rows: each
 * one's keys evaluated and hashed, each distinct one kept.  The child's own
 * price is the caller's.
 *
 * `lookups` is the distinct keys of the child's rows, as estimate_num_groups()
 * makes them of the inner side's key expressions, and the rows of one lookup
 * the table's rows over the distinct values of its scan-key columns.  A large
 * inner side is a price of many descents and many rows, which the ordinary
 * plan's one pass over the table undercuts: the cost model, and nothing else,
 * leaves it to core.
 */
static void
lion_anti_price(PlannerInfo *root, const LionSmallAnti *anti, List *restrictions,
				int ngroup, List *rowexprs, LionAntiPlan *plan)
{
	RelOptInfo *rel = anti->outerrel;
	IndexOptInfo *idx = plan->idx;
	Path	   *child = anti->innerpath;
	double		childrows = Max(child->rows, 1.0);
	double		tuples = Max(rel->tuples, 1.0);
	double		relpages = Max((double) rel->pages, 1.0);
	double		idxtuples = Max(idx->tuples, 1.0);
	double		idxpages = Max((double) idx->pages, 1.0);
	List	   *innerkeys = NIL;
	List	   *outerkeys = NIL;
	double		lookups;
	double		values;
	double		perlookup;
	double		fetched;
	double		idxperlookup;
	double		height;
	double		corr;
	double		minpages;
	double		maxpages;
	double		spc_random;
	double		spc_seq;
	double		idx_random;
	double		idx_seq;
	Cost		descent;
	Cost		cost;
	QualCost	qual;
	QualCost	keyeval;
	QualCost	roweval;
	int			k;

	for (k = 0; k < plan->nkey; k++)
	{
		innerkeys = lappend(innerkeys, plan->keyexpr[k]);
		outerkeys = list_append_unique(outerkeys, plan->keyvar[k]);
	}

	/* the distinct keys of the child's rows, and the rows each finds */
	lookups = estimate_num_groups(root, innerkeys, childrows, NULL, NULL);
	lookups = clamp_row_est(Min(lookups, childrows));
	values = estimate_num_groups(root, outerkeys, tuples, NULL, NULL);
	perlookup = tuples / Max(values, 1.0);
	fetched = Min(perlookup * lookups, tuples);

	get_tablespace_page_costs(rel->reltablespace, &spc_random, &spc_seq);
	get_tablespace_page_costs(idx->reltablespace, &idx_random, &idx_seq);

	/*
	 * The descent of each lookup: a comparison per level of a binary search
	 * of the index's tuples, and the CPU of the pages it reads - the tree's
	 * height, known or guessed from its pages, plus the leaf.
	 */
	height = (idx->tree_height >= 0) ? (double) idx->tree_height :
		ceil(log(idxpages) / log(256.0));
	descent = ceil(log(idxtuples) / log(2.0)) * cpu_operator_cost +
		(height + 1.0) * LION_ANTI_PAGE_CPU * cpu_operator_cost;

	/* ... the index tuples it reads and their scan keys */
	idxperlookup = Max(perlookup * idxtuples / tuples, 1.0);
	cost = lookups * (descent + idxperlookup *
					  (cpu_index_tuple_cost + plan->nkey * cpu_operator_cost));

	/* ... the index's pages, over every lookup */
	cost += index_pages_fetched(ceil(idxperlookup * idxpages / idxtuples) *
								lookups, (BlockNumber) idxpages, idxpages,
								root) * idx_random;

	/*
	 * ... the heap's: as many pages as rows, in no order, or as few as a
	 * lookup's share of the table's pages, in the index's, and as far between
	 * the two as the correlation squared puts it (cost_index()).
	 */
	maxpages = index_pages_fetched(fetched, (BlockNumber) relpages, idxpages,
								   root);
	minpages = index_pages_fetched(ceil(perlookup / tuples * relpages) *
								   lookups, (BlockNumber) relpages, idxpages,
								   root);
	corr = lion_anti_correlation(root, plan);
	cost += (maxpages + corr * corr * (minpages - maxpages)) * spc_random;

	/* ... and each row found: its tuple, the clauses it is tested on */
	cost_qual_eval(&qual, list_concat(list_copy(anti->clauses), restrictions),
				   root);
	cost += qual.startup + fetched * (cpu_tuple_cost + qual.per_tuple);

	/* ... kept once by TID, where two keys may find it */
	if (plan->tids)
		cost += fetched * cpu_operator_cost;

	/* ... and its group found, to subtract it from, or its cell */
	cost += fetched * ngroup * cpu_operator_cost;
	if (rowexprs != NIL)
	{
		cost_qual_eval(&roweval, rowexprs, root);
		cost += roweval.startup + fetched *
			(roweval.per_tuple + list_length(rowexprs) * cpu_operator_cost);
	}

	/* the child's rows: their keys evaluated, hashed and kept once */
	cost_qual_eval(&keyeval, innerkeys, root);
	cost += keyeval.startup +
		childrows * (keyeval.per_tuple + (plan->nkey + 1) * cpu_operator_cost);

	plan->lookups = lookups;
	plan->fetched = fetched;
	plan->cost = cost;
}

/*
 * The B-tree of the counted table the anti join's keys are looked up in
 * (DESIGN.md §27, "The anti join over a small inner side"), its scan keys and
 * the lookups' price, into *out; false when no B-tree looks a key up.
 *
 * A B-tree of the table's own the query may read - valid, not hypothetical,
 * whole or a partial one whose predicate the restrictions imply (predOK): a
 * row outside it does not satisfy them, so it is no row the count counts -
 * with an equality scan key on its first column at least
 * (lion_anti_index_keys()).  Of several, the one whose scan keys cover the
 * most columns, and of those the smallest: the fewest rows found that the
 * join clauses then reject, and the fewest pages.  `ngroup` and `rowexprs`
 * are what a row found is subtracted from: its group's columns, or the
 * predicates that tell its cell (lion_anti_price()).
 */
bool
lion_anti_choose(PlannerInfo *root, const LionSmallAnti *anti, List *restrictions,
				 int ngroup, List *rowexprs, LionAntiPlan *out)
{
	RelOptInfo *rel = anti->outerrel;
	LionAntiPlan best;
	int			bestcols = 0;
	ListCell   *lc;

	memset(&best, 0, sizeof(best));
	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);
		LionAntiPlan cand;
		int			ncol;

		if (idx->relam != BTREE_AM_OID || idx->hypothetical ||
			!idx->amhasgettuple)
			continue;
		if (idx->indpred != NIL && !idx->predOK)
			continue;
		memset(&cand, 0, sizeof(cand));
		cand.idx = idx;
		ncol = lion_anti_index_keys(anti, idx, &cand);
		if (ncol == 0)
			continue;
		if (ncol < bestcols ||
			(ncol == bestcols && best.idx != NULL &&
			 idx->pages >= best.idx->pages))
			continue;
		best = cand;
		bestcols = ncol;
	}
	if (best.idx == NULL)
		return false;

	best.tids = lion_anti_needs_tids(&best);
	lion_anti_price(root, anti, restrictions, ngroup, rowexprs, &best);
	*out = best;
	return true;
}
