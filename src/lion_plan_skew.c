/*-------------------------------------------------------------------------
 *
 * lion_plan_skew.c
 *		What core's scan of a skewed key column costs at the rows a probed
 *		key meets (DESIGN.md §39, "A skewed key's probes").
 *
 * Core prices a scan keyed by a value it does not know - a nested loop's
 * inner index scan, a correlated subquery's, a generic plan's - at the rows
 * per key of the column (var_eq_non_const()).  On a skewed column a probed
 * key meets far more (§33, "The rows of a probed key"), and lion prices its
 * own probes of such a key at that figure.  Weighed against core's scan at
 * core's rows, lion's path would be priced for rows its competitor is not:
 * a lion path that meets them through an index while core's plan fetches
 * each from the heap would lose to the plan it beats.  So the competitor is
 * priced under the same rows - what one of its scans costs more when it
 * meets `factor` times the rows (lion_skew_scan_extra()) - and lion's price,
 * made under those rows, is put back into core's units by the ratio of the
 * competitor's two prices: the two compare as they would under the rows both
 * meet, and every other comparison the plan makes stays in core's units.
 * Where no competitor meets the rows - none, a sequential scan - lion's path
 * keeps core's rows too.
 *
 * The competitor is a nested loop's inner scan, for a lion path priced
 * against the loop (lion_competitor_skew(), lion_units_take_skew()); and for
 * lion's own scan of a relation - a walk of a B-tree keyed on the column
 * (lo_cost()), a LionCount whose WHERE equates it to a Param - the cheapest
 * of core's paths of that relation with the same parameterization
 * (lion_skew_rel_scale()), taken from core's paths before lion's join them.
 * A competitor that is one of the AM's own scans of a lion index keeps
 * core's rows, as every AM scan does, and so does the lion path against it.
 *
 * The extra is cost_index()'s and cost_bitmap_heap_scan()'s own terms for
 * the scan's rows, worked out at its rows and at `factor` times them and
 * subtracted: the heap pages (Mackert and Lohman's, over a nested loop's
 * scans as core counts a parameterized scan's, interpolated by the index's
 * correlation; for a bitmap the pages and their price per page), the index
 * leaf pages genericcostestimate() prorates, and a CPU charge per row and
 * per index tuple.  The quals a row is tested by are taken at the relation's
 * restriction cost per tuple.  Every term grows with the rows; the descent,
 * the start-up and what the plan does above the scan are left as they were.
 *
 * Part of the LionCount custom scan: lion_customscan.h describes the node
 * and declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "utils/spccache.h"

#include "lion_customscan.h"

/* The IndexPaths of a bitmap tree. */
static void
lion_skew_bitmap_leaves(Path *bitmapqual, List **leaves)
{
	ListCell   *lc;

	if (IsA(bitmapqual, IndexPath))
		*leaves = lappend(*leaves, bitmapqual);
	else if (IsA(bitmapqual, BitmapAndPath))
	{
		foreach(lc, ((BitmapAndPath *) bitmapqual)->bitmapquals)
			lion_skew_bitmap_leaves((Path *) lfirst(lc), leaves);
	}
	else if (IsA(bitmapqual, BitmapOrPath))
	{
		foreach(lc, ((BitmapOrPath *) bitmapqual)->bitmapquals)
			lion_skew_bitmap_leaves((Path *) lfirst(lc), leaves);
	}
}

/*
 * Does a scan path read a lion index - an index scan of one, or a bitmap heap
 * scan with one among its index scans?  The AM's own scans keep core's rows
 * (DESIGN.md §39, "A skewed key's probes"), and their price is lion's
 * (lioncostestimate()), which is not to be asked from inside lion's costing.
 */
static bool
lion_skew_reads_lion(Path *scan)
{
	Oid			lionam = lion_get_am_oid();
	List	   *leaves = NIL;
	ListCell   *lc;
	bool		reads = false;

	if (!OidIsValid(lionam))
		return false;
	if (IsA(scan, IndexPath))
		return ((IndexPath *) scan)->indexinfo->relam == lionam;
	if (!IsA(scan, BitmapHeapPath))
		return false;
	lion_skew_bitmap_leaves(((BitmapHeapPath *) scan)->bitmapqual, &leaves);
	foreach(lc, leaves)
	{
		if (((IndexPath *) lfirst(lc))->indexinfo->relam == lionam)
		{
			reads = true;
			break;
		}
	}
	list_free(leaves);
	return reads;
}

/*
 * How many times core's rows per key a scan path of core's meets through its
 * index clauses (lion_skew_clauses_factor()): an index scan's, or the index
 * scans' of a bitmap heap scan.  1 for any other path, and for one of the
 * AM's own scans of a lion index, which keeps core's rows: a lion path
 * weighed against it keeps them too.
 */
double
lion_skew_scan_factor(PlannerInfo *root, Path *scan)
{
	double		factor = 1.0;
	List	   *leaves = NIL;
	ListCell   *lc;

	if (scan == NULL || scan->parent == NULL || lion_skew_reads_lion(scan))
		return 1.0;
	if (IsA(scan, IndexPath))
		return lion_skew_clauses_factor(root, scan->parent,
										((IndexPath *) scan)->indexclauses);
	if (!IsA(scan, BitmapHeapPath))
		return 1.0;
	lion_skew_bitmap_leaves(((BitmapHeapPath *) scan)->bitmapqual, &leaves);
	foreach(lc, leaves)
		factor = Max(factor,
					 lion_skew_clauses_factor(root, scan->parent,
											  ((IndexPath *) lfirst(lc))->indexclauses));
	list_free(leaves);
	return factor;
}

/*
 * An index scan's heap pages at `tuples` rows a scan, over `loops` scans, as
 * cost_index() prices them: Mackert and Lohman's fetches, every one random
 * across a loop's scans, and interpolated by the index's correlation.
 */
static Cost
lion_skew_index_heap(PlannerInfo *root, IndexPath *ip, double tuples,
					 double loops, double csquared, double spc_random,
					 double spc_seq)
{
	IndexOptInfo *index = ip->indexinfo;
	RelOptInfo *baserel = index->rel;
	bool		indexonly = (ip->path.pathtype == T_IndexOnlyScan);
	double		sel = tuples / Max(baserel->tuples, 1.0);
	double		pages;
	Cost		max_io;
	Cost		min_io;

	if (loops > 1.0)
	{
		pages = index_pages_fetched(tuples * loops, baserel->pages,
									(double) index->pages, root);
		if (indexonly)
			pages = ceil(pages * (1.0 - baserel->allvisfrac));
		max_io = pages * spc_random / loops;
		pages = index_pages_fetched(ceil(sel * (double) baserel->pages) * loops,
									baserel->pages, (double) index->pages,
									root);
		if (indexonly)
			pages = ceil(pages * (1.0 - baserel->allvisfrac));
		min_io = pages * spc_random / loops;
	}
	else
	{
		pages = index_pages_fetched(tuples, baserel->pages,
									(double) index->pages, root);
		if (indexonly)
			pages = ceil(pages * (1.0 - baserel->allvisfrac));
		max_io = pages * spc_random;
		pages = ceil(sel * (double) baserel->pages);
		if (indexonly)
			pages = ceil(pages * (1.0 - baserel->allvisfrac));
		min_io = (pages > 0) ? spc_random + (pages - 1) * spc_seq : 0;
	}
	return max_io + csquared * (min_io - max_io);
}

/*
 * An index's leaf pages for `tuples` entries a scan, as
 * genericcostestimate() counts them.
 */
static Cost
lion_skew_index_leaves(PlannerInfo *root, IndexOptInfo *index, double tuples,
					   double loops, double spc_random)
{
	double		pages = ceil(tuples * (double) index->pages /
							 Max(index->tuples, 1.0));

	if (loops > 1.0)
		return index_pages_fetched(pages * loops, index->pages,
								   (double) index->pages, root) *
			spc_random / loops;
	return pages * spc_random;
}

/* A bitmap heap scan's heap pages at `tuples` rows, as core prices them. */
static Cost
lion_skew_bitmap_heap(PlannerInfo *root, RelOptInfo *baserel, double tuples,
					  double idxpages, double loops, double spc_random,
					  double spc_seq)
{
	double		T = (baserel->pages > 1) ? (double) baserel->pages : 1.0;
	double		pages;
	double		perpage;

	if (loops > 1.0)
		pages = index_pages_fetched(tuples * loops, baserel->pages, idxpages,
									root) / loops;
	else
		pages = (2.0 * T * tuples) / (2.0 * T + tuples);
	pages = (pages >= T) ? T : ceil(pages);
	perpage = (pages >= 2.0) ?
		spc_random - (spc_random - spc_seq) * sqrt(pages / T) : spc_random;
	return pages * perpage;
}

/*
 * What one of `loops` scans of a scan path of core's - an index scan, an
 * index-only scan or a bitmap heap scan - costs more when it meets `factor`
 * times the rows its index clauses select (above).  0 for any other path, a
 * scan of a lion index (lion_skew_reads_lion()), or a factor of 1.
 */
Cost
lion_skew_scan_extra(PlannerInfo *root, Path *scan, double factor,
					 double loops)
{
	RelOptInfo *baserel;
	double		tuples;
	double		rows;
	double		more;
	double		spc_random;
	double		spc_seq;
	Cost		extra;

	if (scan == NULL || factor <= 1.0 || lion_skew_reads_lion(scan))
		return 0.0;
	loops = Max(loops, 1.0);

	if (IsA(scan, IndexPath))
	{
		IndexPath  *ip = (IndexPath *) scan;
		IndexOptInfo *index = ip->indexinfo;
		Cost		s;
		Cost		t;
		Selectivity isel;
		double		corr = 0.0;
		double		ipages;
		double		idx_random;
		double		idx_seq;

		baserel = index->rel;
		tuples = Max(baserel->tuples, 1.0);
		rows = clamp_row_est(ip->indexselectivity * tuples);
		more = Min(clamp_row_est(rows * factor), tuples);
		if (more <= rows)
			return 0.0;

		/* the correlation cost_index() interpolates by, from the AM */
		index->amcostestimate(root, ip, loops, &s, &t, &isel, &corr, &ipages);
		get_tablespace_page_costs(baserel->reltablespace, &spc_random,
								  &spc_seq);
		get_tablespace_page_costs(index->reltablespace, &idx_random,
								  &idx_seq);
		extra = lion_skew_index_heap(root, ip, more, loops, corr * corr,
									 spc_random, spc_seq) -
			lion_skew_index_heap(root, ip, rows, loops, corr * corr,
								 spc_random, spc_seq);
		extra += lion_skew_index_leaves(root, index, more, loops, idx_random) -
			lion_skew_index_leaves(root, index, rows, loops, idx_random);
		extra += (more - rows) *
			(cpu_index_tuple_cost +
			 cpu_operator_cost * list_length(ip->indexclauses));
	}
	else if (IsA(scan, BitmapHeapPath))
	{
		BitmapHeapPath *bp = (BitmapHeapPath *) scan;
		List	   *leaves = NIL;
		ListCell   *lc;
		Cost		c;
		Selectivity sel;
		double		idxpages = 0;

		baserel = scan->parent;
		if (baserel == NULL)
			return 0.0;
		tuples = Max(baserel->tuples, 1.0);
		cost_bitmap_tree_node(bp->bitmapqual, &c, &sel);
		rows = clamp_row_est(sel * tuples);
		more = Min(clamp_row_est(rows * factor), tuples);
		if (more <= rows)
			return 0.0;
		lion_skew_bitmap_leaves(bp->bitmapqual, &leaves);
		foreach(lc, leaves)
			idxpages += (double) ((IndexPath *) lfirst(lc))->indexinfo->pages;
		list_free(leaves);
		get_tablespace_page_costs(baserel->reltablespace, &spc_random,
								  &spc_seq);
		extra = lion_skew_bitmap_heap(root, baserel, more, idxpages, loops,
									  spc_random, spc_seq) -
			lion_skew_bitmap_heap(root, baserel, rows, idxpages, loops,
								  spc_random, spc_seq);
		extra += (more - rows) *
			(cpu_index_tuple_cost + 0.1 * cpu_operator_cost);
	}
	else
		return 0.0;

	/* each row, and the quals it is tested by */
	extra += (more - rows) *
		(cpu_tuple_cost + Max(baserel->baserestrictcost.per_tuple, 0.0));
	return Max(extra, 0.0);
}

/* A path core's enable_* settings disabled (lion_plan_units.c has its own). */
static bool
lion_skew_path_disabled(const Path *p)
{
#if PG_VERSION_NUM >= 180000
	return p->disabled_nodes > 0;
#else
	return p->total_cost >= disable_cost;
#endif
}

/*
 * The scale that puts a scan of rel lion priced at the rows its probes of a
 * skewed key meet back into core's units, where it competes with rel's own
 * paths parameterized by the same outer rels (lo_cost(), a count's WHERE):
 * the cheapest of those that is not one of lion's custom scans nor disabled,
 * its price at core's rows over its price at the rows its own scan of a
 * skewed key meets (above), one of `loops` scans.  1 where there is none, or
 * its scan meets no skewed key - a sequential scan, whose price no key's rows
 * change - or keeps core's rows: one of the AM's own scans of a lion index.
 * Worked out once a planner run for rel, the outer rels and the loops
 * (lion_skew_scale_find()), the first time from core's paths alone
 * (lion_skew_rel_note()): add_path() drops the competitor once a lion path
 * beats it.
 */
double
lion_skew_rel_scale(PlannerInfo *root, RelOptInfo *rel, Relids required_outer,
					double loops)
{
	Path	   *best = NULL;
	double		factor;
	double		scale = 1.0;
	Cost		more;
	ListCell   *lc;

	if (rel == NULL)
		return 1.0;
	if (lion_skew_scale_find(root, rel, required_outer, loops, &scale))
		return scale;
	foreach(lc, rel->pathlist)
	{
		Path	   *p = (Path *) lfirst(lc);

		if (IsA(p, CustomPath) || lion_skew_path_disabled(p) ||
			!bms_equal(PATH_REQ_OUTER(p), required_outer))
			continue;
		if (best == NULL || p->total_cost < best->total_cost)
			best = p;
	}
	if (best != NULL && best->total_cost > 0.0)
	{
		factor = lion_skew_scan_factor(root, best);
		more = lion_skew_scan_extra(root, best, factor, loops);
		if (more > 0.0)
		{
			scale = best->total_cost / (best->total_cost + more);
			elog(DEBUG2, "lion: a scan of a skewed key priced against a path of cost %.2f, %.2f more at %g times its rows",
				 best->total_cost, more, factor);
		}
	}
	lion_skew_scale_keep(root, rel, required_outer, loops, scale);
	return scale;
}

/*
 * set_rel_pathlist_hook, before any of lion's paths of rel: the scale of an
 * unparameterized scan of rel whose restrictions equate a skewed key column to
 * a value the plan does not know (lion_skew_rel_scale()), from core's paths.
 */
void
lion_skew_rel_note(PlannerInfo *root, RelOptInfo *rel)
{
	if (rel == NULL || rel->reloptkind != RELOPT_BASEREL ||
		rel->rtekind != RTE_RELATION || rel->baserestrictinfo == NIL)
		return;
	(void) lion_skew_clauses_scale(root, rel, rel->baserestrictinfo);
}

/*
 * ... for a lion path of rel whose clauses equate a skewed key column to a
 * value the plan does not know (lion_skew_clauses_factor()), unparameterized
 * - a correlated subquery's count, a generic plan's: 1 where none does.
 */
double
lion_skew_clauses_scale(PlannerInfo *root, RelOptInfo *rel, List *clauses)
{
	if (lion_skew_clauses_factor(root, rel, clauses) <= 1.0)
		return 1.0;
	return lion_skew_rel_scale(root, rel, NULL, 1.0);
}
