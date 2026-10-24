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
 * A relation whose restrictions equate the key to a value the plan does not
 * know - a correlated subquery's scan, whose calls are the loop, or a generic
 * plan's - is weighed against core's own scans of it alone, the AM's scans of
 * a lion index left out and core's index paths built again without them
 * (lion_skew_rel_note()), and the AM's scans of a lion index that meet the key
 * are priced in the same units as lion's other paths: at the rows a probed
 * key meets (lioncostestimate()) and put back into core's units by the same
 * scale (lion_skew_am_paths(); DESIGN.md §40.12).  Where the query level is
 * that one relation and stops at a count of rows - a LIMIT, an EXISTS's first
 * row (lion_skew_limit_rows()) - a scan is read only until the count's rows
 * are found: core's for the rows that takes at the share of its key's rows
 * its filter passes, lion's for the same share of its run.
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
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
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
 * Does a scan path read lion indexes alone: an index scan of one, or a bitmap
 * heap scan whose every index scan is of one?  The paths a scratch relation
 * of the lion indexes builds again (lion_skew_am_paths()).
 */
static bool
lion_skew_lion_only(Path *scan)
{
	Oid			lionam = lion_get_am_oid();
	List	   *leaves = NIL;
	ListCell   *lc;
	bool		only = true;

	if (!OidIsValid(lionam))
		return false;
	if (IsA(scan, IndexPath))
		return ((IndexPath *) scan)->indexinfo->relam == lionam;
	if (!IsA(scan, BitmapHeapPath))
		return false;
	lion_skew_bitmap_leaves(((BitmapHeapPath *) scan)->bitmapqual, &leaves);
	foreach(lc, leaves)
	{
		if (((IndexPath *) lfirst(lc))->indexinfo->relam != lionam)
		{
			only = false;
			break;
		}
	}
	list_free(leaves);
	return only && leaves != NIL;
}

/*
 * How many times core's rows per key a scan path of rel meets through its
 * own index clauses, whatever index it reads: an index scan's, or the largest
 * of a bitmap heap scan's index scans'.  1 for any other path.
 */
static double
lion_skew_path_factor(PlannerInfo *root, RelOptInfo *rel, Path *scan)
{
	double		factor = 1.0;
	List	   *leaves = NIL;
	ListCell   *lc;

	if (IsA(scan, IndexPath))
		return lion_skew_clauses_factor(root, rel,
										((IndexPath *) scan)->indexclauses);
	if (!IsA(scan, BitmapHeapPath))
		return 1.0;
	lion_skew_bitmap_leaves(((BitmapHeapPath *) scan)->bitmapqual, &leaves);
	foreach(lc, leaves)
		factor = Max(factor,
					 lion_skew_clauses_factor(root, rel,
											  ((IndexPath *) lfirst(lc))->indexclauses));
	list_free(leaves);
	return factor;
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
 * The count of rows the query level rel is planned in takes from rel's scan,
 * and then stops: a LIMIT's (core's limit_tuples, the LIMIT and OFFSET
 * together), or the absolute count of rows a caller asked the level for -
 * an EXISTS sub-select's first (core's tuple_fraction at 1 or more).  Only
 * where the level is rel alone and nothing between the scan and the count
 * reads more of it - no grouping, aggregate, DISTINCT, window, set-returning
 * function, ORDER BY or set operation - so that the scan's rows are the
 * level's.  0 for none.
 */
double
lion_skew_limit_rows(PlannerInfo *root, RelOptInfo *rel)
{
	Query	   *parse;

	if (root == NULL || rel == NULL || rel->reloptkind != RELOPT_BASEREL ||
		root->parse == NULL ||
		bms_membership(root->all_baserels) != BMS_SINGLETON ||
		!bms_is_member(rel->relid, root->all_baserels))
		return 0.0;
	parse = root->parse;
	if (parse->groupClause != NIL || parse->groupingSets != NIL ||
		parse->distinctClause != NIL || parse->hasAggs ||
		parse->hasWindowFuncs || parse->hasTargetSRFs ||
		root->hasHavingQual || parse->sortClause != NIL ||
		parse->setOperations != NULL)
		return 0.0;
	if (root->limit_tuples > 0.0)
		return root->limit_tuples;
	if (root->tuple_fraction >= 1.0)
		return root->tuple_fraction;
	return 0.0;
}

/*
 * The share of rel's rows its restriction clauses pass, as core estimates
 * them (set_baserel_size_estimates()), before rel->rows rounds it to a row.
 */
static Selectivity
lion_skew_rel_sel(PlannerInfo *root, RelOptInfo *rel)
{
	return clauselist_selectivity(root, rel->baserestrictinfo, 0, JOIN_INNER,
								  NULL);
}

/*
 * The rows of its key a scan reads when it meets `more` of them where core
 * has `rows` - its index clauses passing `keysel` of rel's rows - and the
 * query stops at `limit` of its own (above): the key's rows until `limit`
 * pass the rest of its WHERE, at the share of them that rel's restriction
 * clauses pass of its index clauses' - `more` where no count stops it, or
 * more would not reach it; `rows` where core's own rows reach it already.
 */
static double
lion_skew_limit_read(PlannerInfo *root, RelOptInfo *rel, Selectivity keysel,
					 double rows, double more, double limit)
{
	double		pass;

	if (limit <= 0.0 || more <= rows || keysel <= 0.0)
		return more;
	pass = Min(lion_skew_rel_sel(root, rel) / keysel, 1.0);
	if (pass <= 0.0 || more * pass <= limit)
		return more;
	if (rows * pass >= limit)
		return rows;
	return Max(Min(limit / pass, more), rows);
}

/*
 * The share of its run that a scan of rel lion priced at `factor` times
 * core's rows per key is to be charged when the query stops at `limit` of its
 * rows (lion_skew_limit_rows()): it reads `limit` over the rows it returns at
 * that factor, where those pass `limit`, and core's Limit takes `limit` over
 * the scan's `rows` - core's - of what it is charged, where those pass it, so
 * the run is charged the first share over the second.  1 where the rows at
 * the factor do not pass `limit`.
 */
double
lion_skew_limit_share(PlannerInfo *root, RelOptInfo *rel, double rows,
					  double factor, double limit)
{
	double		out;
	double		share;

	if (limit <= 0.0 || factor <= 1.0)
		return 1.0;
	out = Max(lion_skew_rel_sel(root, rel) * Max(rel->tuples, 1.0) * factor,
			  rows);
	if (out <= limit)
		return 1.0;
	share = limit / out;
	if (rows > limit)
		share *= rows / limit;
	return Min(share, 1.0);
}

/*
 * What one of `loops` scans of a scan path of core's - an index scan, an
 * index-only scan or a bitmap heap scan - costs more when it meets `factor`
 * times the rows its index clauses select (above).  0 for any other path, a
 * scan of a lion index (lion_skew_reads_lion()), or a factor of 1.
 *
 * With `limit` above 0 the query takes that many of the scan's rows and stops
 * (lion_skew_limit_rows()): of its key's rows the share its filter passes is
 * the path's rows over its index's, and it reads the key's rows until `limit`
 * of them pass - all of them where fewer would.  An index scan's every term
 * is then priced at the rows it reads.  A bitmap heap scan builds its whole
 * bitmap first: its index tuples are the key's every row, its heap pages and
 * rows those it reads.  Where core's own rows already pass `limit` its scan
 * stops as soon at either count of rows, and nothing is added.
 */
Cost
lion_skew_scan_extra(PlannerInfo *root, Path *scan, double factor,
					 double loops, double limit)
{
	RelOptInfo *baserel;
	double		tuples;
	double		rows;
	double		more;
	double		read;
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
		read = lion_skew_limit_read(root, baserel, ip->indexselectivity, rows,
									more, limit);
		if (read <= rows)
			return 0.0;

		/* the correlation cost_index() interpolates by, from the AM */
		index->amcostestimate(root, ip, loops, &s, &t, &isel, &corr, &ipages);
		get_tablespace_page_costs(baserel->reltablespace, &spc_random,
								  &spc_seq);
		get_tablespace_page_costs(index->reltablespace, &idx_random,
								  &idx_seq);
		extra = lion_skew_index_heap(root, ip, read, loops, corr * corr,
									 spc_random, spc_seq) -
			lion_skew_index_heap(root, ip, rows, loops, corr * corr,
								 spc_random, spc_seq);
		extra += lion_skew_index_leaves(root, index, read, loops, idx_random) -
			lion_skew_index_leaves(root, index, rows, loops, idx_random);
		extra += (read - rows) *
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
		read = lion_skew_limit_read(root, baserel, sel, rows, more, limit);
		if (more <= rows)
			return 0.0;
		lion_skew_bitmap_leaves(bp->bitmapqual, &leaves);
		foreach(lc, leaves)
			idxpages += (double) ((IndexPath *) lfirst(lc))->indexinfo->pages;
		list_free(leaves);
		get_tablespace_page_costs(baserel->reltablespace, &spc_random,
								  &spc_seq);
		extra = (more - rows) *
			(cpu_index_tuple_cost + 0.1 * cpu_operator_cost);
		if (read > rows)
			extra += lion_skew_bitmap_heap(root, baserel, read, idxpages,
										   loops, spc_random, spc_seq) -
				lion_skew_bitmap_heap(root, baserel, rows, idxpages, loops,
									  spc_random, spc_seq);
	}
	else
		return 0.0;

	/* each row read, and the quals it is tested by */
	if (read > rows)
		extra += (read - rows) *
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
		more = lion_skew_scan_extra(root, best, factor, loops,
									(required_outer == NULL) ?
									lion_skew_limit_rows(root, rel) : 0.0);
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
 * The relation whose scans of a lion index lioncostestimate() is pricing at
 * the rows a probed key meets (lion_skew_am_paths()), or NULL.
 */
static RelOptInfo *lion_skew_am_rel = NULL;

/*
 * How many times core's rows per key the AM's scan `path` of a lion index is
 * to be priced at (lioncostestimate()): its index clauses' factor while
 * lion_skew_am_paths() prices rel's scans again, 1 otherwise - and for a
 * parameterized path, which lion_skew_am_paths() does not take.
 */
double
lion_skew_am_factor(PlannerInfo *root, IndexPath *path)
{
	if (lion_skew_am_rel == NULL || path->indexinfo->rel != lion_skew_am_rel ||
		path->path.param_info != NULL)
		return 1.0;
	return lion_skew_clauses_factor(root, lion_skew_am_rel,
									path->indexclauses);
}

/*
 * What a lion access of rel - the bitmap tree a walk's set is built from
 * (lo_cost()) - costs more where its scans of a lion index meet the rows a
 * probed key meets: each unparameterized one whose index clauses equate the
 * skewed key to a value the plan does not know, its index side priced again
 * at those rows (lioncostestimate(), lion_skew_am_factor()) and the
 * difference summed, a TID a row as cost_bitmap_tree_node() charges it.
 * *factor is the largest of their factors, 1 where none meets the key.
 */
Cost
lion_skew_set_extra(PlannerInfo *root, RelOptInfo *rel, Path *lion,
					double *factor)
{
	Oid			lionam = lion_get_am_oid();
	List	   *leaves = NIL;
	Cost		extra = 0.0;
	int			mark;
	ListCell   *lc;

	*factor = 1.0;
	if (lion == NULL || rel == NULL || !OidIsValid(lionam))
		return 0.0;
	mark = lion_plain_remainders_mark(root);
	lion_skew_bitmap_leaves(lion, &leaves);
	foreach(lc, leaves)
	{
		IndexPath  *ip = (IndexPath *) lfirst(lc);
		double		f;
		Cost		s;
		Cost		t;
		Selectivity sel;
		double		corr;
		double		pages;

		if (ip->path.param_info != NULL || ip->indexinfo->relam != lionam ||
			ip->indexinfo->rel != rel)
			continue;
		f = lion_skew_clauses_factor(root, rel, ip->indexclauses);
		if (f <= 1.0)
			continue;
		*factor = Max(*factor, f);
		lion_skew_am_rel = rel;
		PG_TRY();
		{
			ip->indexinfo->amcostestimate(root, ip, 1.0, &s, &t, &sel, &corr,
										  &pages);
		}
		PG_FINALLY();
		{
			lion_skew_am_rel = NULL;
		}
		PG_END_TRY();
		extra += Max(t - ip->indextotalcost, 0.0) +
			0.1 * cpu_operator_cost *
			Max(sel - ip->indexselectivity, 0.0) * Max(rel->tuples, 1.0);
	}
	list_free(leaves);
	/* a remainder the prices above noted is no plain path's to pay */
	lion_plain_remainders_drop(root, mark);
	return extra;
}

/*
 * A copy of rel that sees, of its indexes, the lion indexes alone (`lion`) or
 * every other one, its paths none and no join clauses: create_index_paths()
 * on it builds the unparameterized index paths of those indexes as it builds
 * them for rel, competing with nothing else.  Parallel paths as rel has them.
 * The copy of core's indexes is given the sequential scan too, which
 * add_path() may have dropped from rel as it drops the index paths.
 */
static RelOptInfo *
lion_skew_scratch(PlannerInfo *root, RelOptInfo *rel, bool lion)
{
	Oid			lionam = lion_get_am_oid();
	RelOptInfo *scratch = makeNode(RelOptInfo);
	ListCell   *lc;

	memcpy(scratch, rel, sizeof(RelOptInfo));
	scratch->indexlist = NIL;
	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);

		if ((idx->relam == lionam) == lion)
			scratch->indexlist = lappend(scratch->indexlist, idx);
	}
	scratch->pathlist = NIL;
	scratch->ppilist = NIL;
	scratch->partial_pathlist = NIL;
	scratch->cheapest_startup_path = NULL;
	scratch->cheapest_total_path = NULL;
	scratch->cheapest_parameterized_paths = NIL;
	scratch->joininfo = NIL;
	scratch->has_eclass_joins = false;
	if (!lion && rel->rtekind == RTE_RELATION &&
		planner_rt_fetch(rel->relid, root)->tablesample == NULL)
		add_path(scratch, create_seqscan_path(root, scratch,
											  rel->lateral_relids, 0));
	if (scratch->indexlist != NIL)
		create_index_paths(root, scratch);
	return scratch;
}

/* A scratch relation's path made rel's, its bitmap's index scans with it. */
static void
lion_skew_adopt(Path *path, RelOptInfo *rel)
{
	ListCell   *lc;

	path->parent = rel;
	if (IsA(path, BitmapHeapPath))
		lion_skew_adopt(((BitmapHeapPath *) path)->bitmapqual, rel);
	else if (IsA(path, BitmapAndPath))
	{
		foreach(lc, ((BitmapAndPath *) path)->bitmapquals)
			lion_skew_adopt((Path *) lfirst(lc), rel);
	}
	else if (IsA(path, BitmapOrPath))
	{
		foreach(lc, ((BitmapOrPath *) path)->bitmapquals)
			lion_skew_adopt((Path *) lfirst(lc), rel);
	}
}

/*
 * A path priced at the rows a probed key meets, put into core's units: its
 * run cut to `share` - the part of it a LIMIT takes at those rows - and the
 * whole multiplied by `scale`.  Before PostgreSQL 18 a disabled path carries
 * disable_cost in its costs (cost_index(), cost_bitmap_heap_scan()), which is
 * left whole: no scale turns a disabled path into an enabled one.
 */
void
lion_skew_reprice(Path *path, double share, double scale)
{
	Cost		disabled = 0.0;
	Cost		startup;
	Cost		run;

#if PG_VERSION_NUM < 180000
	if (path->startup_cost >= disable_cost)
		disabled = disable_cost * floor(path->startup_cost / disable_cost);
#endif
	startup = path->startup_cost - disabled;
	run = Max(path->total_cost - path->startup_cost, 0.0);
	path->startup_cost = startup * scale + disabled;
	path->total_cost = (startup + run * share) * scale + disabled;
}

/*
 * The AM's own scans of rel's lion indexes that meet the skewed key, priced
 * in the units lion's other paths of rel are (DESIGN.md §40.12): at the rows a
 * probed key meets and put back into core's by `scale`, where core's own
 * cheapest scan of rel meets them too (lion_skew_rel_note()).  The scans
 * core priced at its own rows are taken out of rel's paths, and with them out
 * of the comparison: those of rel's paths, unparameterized, serial or
 * partial, whose index scans are all of lion indexes and whose index clauses
 * equate the key to the value the plan does not know (a scan of a lion index
 * on other columns meets no more rows at a heavier key, and keeps its price;
 * a BitmapAnd of a lion index and another index is core's mixture, and keeps
 * it too).  add_path() may have dropped core's own index scans in their
 * favour: core's paths, which `core` built again over its other indexes, are
 * offered again at core's prices.  Then the lion indexes' paths are built
 * again on a copy of rel that sees them alone, lioncostestimate() pricing
 * each scan's sets, index tuples and selectivity - and core, from it, the
 * heap - at the rows a probed key meets (lion_skew_am_factor()); each that
 * meets the key has its run cut to the share of it the query's count of rows
 * takes at those rows, over the share core's Limit takes at core's
 * (lion_skew_limit_rows(), lion_skew_limit_share()), is multiplied by
 * `scale` and offered.
 */
static void
lion_skew_am_paths(PlannerInfo *root, RelOptInfo *rel, RelOptInfo *core,
				   double scale, double limit)
{
	RelOptInfo *lion;
	ListCell   *lc;

	foreach(lc, rel->pathlist)
	{
		Path	   *p = (Path *) lfirst(lc);

		if (p->param_info == NULL && lion_skew_lion_only(p) &&
			lion_skew_path_factor(root, rel, p) > 1.0)
			rel->pathlist = foreach_delete_current(rel->pathlist, lc);
	}
	foreach(lc, rel->partial_pathlist)
	{
		Path	   *p = (Path *) lfirst(lc);

		if (p->param_info == NULL && lion_skew_lion_only(p) &&
			lion_skew_path_factor(root, rel, p) > 1.0)
			rel->partial_pathlist = foreach_delete_current(rel->partial_pathlist,
														   lc);
	}
	if (core != NULL)
	{
		foreach(lc, core->pathlist)
		{
			Path	   *p = (Path *) lfirst(lc);

			if (p->param_info == NULL)
				add_path(rel, p);
		}
		foreach(lc, core->partial_pathlist)
		{
			Path	   *p = (Path *) lfirst(lc);

			if (p->param_info == NULL)
				add_partial_path(rel, p);
		}
	}

	lion_skew_am_rel = rel;
	PG_TRY();
	{
		lion = lion_skew_scratch(root, rel, true);
	}
	PG_FINALLY();
	{
		lion_skew_am_rel = NULL;
	}
	PG_END_TRY();
	foreach(lc, lion->pathlist)
	{
		Path	   *p = (Path *) lfirst(lc);
		double		factor;
		double		share;

		if (p->param_info != NULL || !lion_skew_lion_only(p))
			continue;
		factor = lion_skew_path_factor(root, rel, p);
		if (factor <= 1.0)
			continue;
		lion_skew_adopt(p, rel);
		share = lion_skew_limit_share(root, rel, p->rows, factor, limit);
		elog(DEBUG2, "lion: the AM's scan of a skewed key, %.2f at %g times its rows, %.2f in core's units",
			 p->total_cost, factor,
			 (p->startup_cost + (p->total_cost - p->startup_cost) * share) *
			 scale);
		lion_skew_reprice(p, share, scale);
		if (IsA(p, IndexPath))
			lion_plain_scale_remainder(root, (IndexPath *) p, share, scale);
		add_path(rel, p);
	}
	foreach(lc, lion->partial_pathlist)
	{
		Path	   *p = (Path *) lfirst(lc);

		if (p->param_info != NULL || !lion_skew_lion_only(p) ||
			lion_skew_path_factor(root, rel, p) <= 1.0)
			continue;
		lion_skew_adopt(p, rel);
		lion_skew_reprice(p, 1.0, scale);
		add_partial_path(rel, p);
	}
}

/*
 * set_rel_pathlist_hook, before any of lion's paths of rel: the scale of an
 * unparameterized scan of rel whose restrictions equate a skewed key column to
 * a value the plan does not know (lion_skew_rel_scale()), kept for the planner
 * run - a correlated subquery's scan, whose calls are the loop, or a generic
 * plan's.  The competitor is the cheapest of core's own scans of rel: its
 * paths that read no lion index, with its index paths built again over its
 * other indexes (lion_skew_scratch()), since add_path() drops those an AM
 * scan of a lion index beat at core's rows.  The AM's scans are lion's plans
 * as much as LionCount and the walks are, and are priced in the same units
 * (lion_skew_am_paths()).
 */
void
lion_skew_rel_note(PlannerInfo *root, RelOptInfo *rel)
{
	RelOptInfo *core;
	Path	   *best = NULL;
	double		factor;
	double		limit;
	double		scale = 1.0;
	Cost		more;
	ListCell   *lc;

	if (rel == NULL || rel->reloptkind != RELOPT_BASEREL ||
		rel->rtekind != RTE_RELATION || rel->baserestrictinfo == NIL)
		return;
	if (lion_skew_clauses_factor(root, rel, rel->baserestrictinfo) <= 1.0 ||
		lion_skew_scale_find(root, rel, NULL, 1.0, &scale))
		return;

	core = lion_skew_scratch(root, rel, false);
	foreach(lc, core->pathlist)
		lion_skew_adopt((Path *) lfirst(lc), rel);
	foreach(lc, core->partial_pathlist)
		lion_skew_adopt((Path *) lfirst(lc), rel);
	foreach(lc, list_concat_copy(rel->pathlist, core->pathlist))
	{
		Path	   *p = (Path *) lfirst(lc);

		if (IsA(p, CustomPath) || lion_skew_path_disabled(p) ||
			p->param_info != NULL || lion_skew_reads_lion(p))
			continue;
		if (best == NULL || p->total_cost < best->total_cost)
			best = p;
	}
	limit = lion_skew_limit_rows(root, rel);
	if (best != NULL && best->total_cost > 0.0)
	{
		factor = lion_skew_scan_factor(root, best);
		more = lion_skew_scan_extra(root, best, factor, 1.0, limit);
		if (more > 0.0)
		{
			scale = best->total_cost / (best->total_cost + more);
			elog(DEBUG2, "lion: a scan of a skewed key priced against a path of cost %.2f, %.2f more at %g times its rows%s",
				 best->total_cost, more, factor,
				 (limit > 0.0) ? " under the query's count of rows" : "");
		}
	}
	lion_skew_scale_keep(root, rel, NULL, 1.0, scale);
	if (scale < 1.0)
		lion_skew_am_paths(root, rel, core, scale, limit);
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
