/*-------------------------------------------------------------------------
 *
 * lion_plan_units.c
 *		The units a lion path is priced in against the core plan it competes
 *		with (DESIGN.md §39).
 *
 * Lion's CPU constants are fitted at 500 cost units a millisecond, the middle
 * of core's sequential and index-only scans (§10, "The reference").  Core's
 * other plans run at rates of their own - a hash aggregate at about 200, a
 * hash join at 60 to 280, a nested loop over warm indexes at 1,000 to 2,000
 * (§10, "The units"; §31) - so a lion path priced honestly against a scan is
 * priced dearly against a hash aggregate and cheaply against a nested loop,
 * and loses to the one where it is faster and wins against the other where it
 * is slower.  Each of lion's custom paths is therefore priced in the units of
 * the cheapest core path of its relation: the kind of plan that is
 * (lion_competitor_kind()) has a rate, pg_lion.<kind>_rate, the multiple of
 * the reference its plans run at, and the path's own price is multiplied by
 * it (lion_units_price()).
 *
 * The price is converted whole, its pages with its CPU terms.  A rate is the
 * ratio of two whole plans' cost units a millisecond, pages and all, and a
 * time in one unit is a time in another by that one factor.  Converting the
 * CPU terms alone, as this file first did, priced a page differently against
 * each kind of competitor: two of lion's own forms that read pages and CPU in
 * different shares - the decoded walk and the nested loop of a GROUP BY, a
 * walk and a probe, a collected filter and a sought one - then changed order
 * with the competitor, and the executor follows the form the price picks.
 * Converted whole, every choice inside a price is made in lion's own units,
 * which its constants were fitted in.
 *
 * Part of the LionCount custom scan: lion_customscan.h describes the node
 * and declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/restrictinfo.h"

#include "lion_customscan.h"

/*
 * Whether a nested loop competitor's Memoize is priced at the hits the
 * LIMIT's share of its calls meets (DESIGN.md §39, "A Memoize under a
 * LIMIT"; lion_memoize_limit_extra()).
 */
bool		lion_enable_memoize_limit = true;

static const char *const lion_competitor_names[] = {
	[LION_COMPETITOR_NONE] = "none",
	[LION_COMPETITOR_DISABLED] = "disabled plan",
	[LION_COMPETITOR_HASHAGG] = "hashed aggregate",
	[LION_COMPETITOR_AGG] = "aggregate over a scan",
	[LION_COMPETITOR_HASHJOIN] = "hash join",
	[LION_COMPETITOR_MERGEJOIN] = "merge join",
	[LION_COMPETITOR_NESTLOOP] = "nested loop",
	[LION_COMPETITOR_SEQSCAN] = "sequential scan",
	[LION_COMPETITOR_INDEXONLY] = "index-only scan",
	[LION_COMPETITOR_INDEX] = "index scan",
	[LION_COMPETITOR_BITMAP] = "bitmap heap scan",
	[LION_COMPETITOR_OTHER] = "other",
};

/*
 * Is cp one of lion's own custom paths - LionCount, LionSemiJoin,
 * LionAntiJoin, LionJoinAgg, LionOrdered, LionBtreeScan, LionOrderedJoin,
 * LionOrderedSplit, LionBm25 or LionProbeJoin?  By its methods, which are
 * this library's own: a name is only what EXPLAIN prints, and another
 * extension's path may carry any.
 */
static bool
lion_custom_is_lion(const CustomPath *cp)
{
	const CustomPathMethods *m = cp->methods;

	return m == &lion_count_path_methods ||
		m == &lion_semijoin_path_methods ||
		m == &lion_antijoin_path_methods ||
		m == &lion_join_agg_path_methods ||
		m == &lion_ordered_path_methods ||
		m == &lion_btree_path_methods ||
		m == &lion_ordjoin_path_methods ||
		m == &lion_ordsplit_path_methods ||
		m == &lion_bm25_path_methods ||
		m == &lion_probe_join_path_methods;
}

/*
 * The one input of a path that has exactly one - the nodes core puts over a
 * scan or a join without changing what is read: aggregates, sorts,
 * projections, gathers, a LIMIT - or NULL.
 */
static Path *
lion_path_input(Path *path)
{
	switch (nodeTag(path))
	{
		case T_AggPath:
			return ((AggPath *) path)->subpath;
		case T_GroupPath:
			return ((GroupPath *) path)->subpath;
		case T_GroupingSetsPath:
			return ((GroupingSetsPath *) path)->subpath;
#if PG_VERSION_NUM >= 190000
		case T_UniquePath:		/* 19: the upper unique, PG18's UpperUniquePath */
			return ((UniquePath *) path)->subpath;
#else
		case T_UpperUniquePath:
			return ((UpperUniquePath *) path)->subpath;
		case T_UniquePath:		/* the semi-join unique, gone in 19 */
			return ((UniquePath *) path)->subpath;
#endif
		case T_ProjectionPath:
			return ((ProjectionPath *) path)->subpath;
		case T_ProjectSetPath:
			return ((ProjectSetPath *) path)->subpath;
		case T_SortPath:
			return ((SortPath *) path)->subpath;
		case T_IncrementalSortPath:
			return ((IncrementalSortPath *) path)->spath.subpath;
		case T_WindowAggPath:
			return ((WindowAggPath *) path)->subpath;
		case T_LockRowsPath:
			return ((LockRowsPath *) path)->subpath;
		case T_LimitPath:
			return ((LimitPath *) path)->subpath;
		case T_MaterialPath:
			return ((MaterialPath *) path)->subpath;
		case T_MemoizePath:
			return ((MemoizePath *) path)->subpath;
		case T_GatherPath:
			return ((GatherPath *) path)->subpath;
		case T_GatherMergePath:
			return ((GatherMergePath *) path)->subpath;
		case T_SubqueryScanPath:
			return ((SubqueryScanPath *) path)->subpath;
		default:
			return NULL;
	}
}

/*
 * Does path hold one of lion's custom paths anywhere in its tree?  A plan of
 * core's over a lion path - an Agg over LionOrdered, a hash join over a
 * LionSemiJoin, the Finalize Agg over a partitioned count - is lion's as much
 * as core's, and not what a lion path is measured against.  A node this does
 * not know is taken to hold none.
 */
bool
lion_path_has_lion(Path *path)
{
	ListCell   *lc;

	check_stack_depth();
	if (path == NULL)
		return false;

	switch (nodeTag(path))
	{
		case T_CustomPath:
			{
				CustomPath *cp = (CustomPath *) path;

				if (lion_custom_is_lion(cp))
					return true;
				foreach(lc, cp->custom_paths)
					if (lion_path_has_lion((Path *) lfirst(lc)))
						return true;
				return false;
			}
		case T_AppendPath:
			foreach(lc, ((AppendPath *) path)->subpaths)
				if (lion_path_has_lion((Path *) lfirst(lc)))
					return true;
			return false;
		case T_MergeAppendPath:
			foreach(lc, ((MergeAppendPath *) path)->subpaths)
				if (lion_path_has_lion((Path *) lfirst(lc)))
					return true;
			return false;
		case T_NestPath:
		case T_MergePath:
		case T_HashPath:
			return lion_path_has_lion(((JoinPath *) path)->outerjoinpath) ||
				lion_path_has_lion(((JoinPath *) path)->innerjoinpath);
		case T_ForeignPath:
			return lion_path_has_lion(((ForeignPath *) path)->fdw_outerpath);
		default:
			return lion_path_has_lion(lion_path_input(path));
	}
}

/*
 * Is a nested loop's inner side an index or bitmap scan parameterized by its
 * outer rows - what §10 and §31 measured, a scan of warm index pages each one
 * charged as a random read?  A nested loop over a materialized inner side is
 * CPU, at core's own rate.
 */
static bool
lion_nestloop_probes_index(JoinPath *jp)
{
	Path	   *inner = jp->innerjoinpath;

	while (inner != NULL &&
		   (IsA(inner, MaterialPath) || IsA(inner, MemoizePath) ||
			IsA(inner, ProjectionPath)))
		inner = lion_path_input(inner);
	return inner != NULL && inner->param_info != NULL &&
		(IsA(inner, IndexPath) || IsA(inner, BitmapHeapPath));
}

/*
 * What kind of plan a core path is, for its rate (DESIGN.md §39): down from
 * its top through the nodes that pass a scan's or a join's rows up, to the
 * first that says what its units are.
 *
 *	- a join is its join method: a HASH JOIN, a MERGE JOIN, or a NESTED LOOP
 *	  into a parameterized index scan - aggregated or not, hashed or not: an
 *	  aggregate over a join is the join's kind, whose rate was measured with
 *	  the aggregate over it and whose rows are what the aggregate is fed;
 *	- an aggregate that hashes, anywhere above a scan - a Finalize Agg over
 *	  the Partial HashAggregates of a parallel plan included - is a HASHED
 *	  AGGREGATE: hashing is what core charges far below its time;
 *	- a bitmap heap scan is a BITMAP heap scan, aggregated by a plain or
 *	  sorted aggregate or not;
 *	- a plain or sorted aggregate over a sequential, index-only or index scan
 *	  is an AGGREGATE over a scan, and the scan alone its own kind;
 *	- a nested loop over anything else is OTHER, and so is anything else
 *	  but under a hashed aggregate;
 *	- an Append is the kind of its dearest child, which most of its time is.
 */
LionCompetitor
lion_competitor_kind(Path *path)
{
	bool		aggregated = false;
	bool		hashed = false;

	while (path != NULL)
	{
		switch (nodeTag(path))
		{
			case T_AggPath:
				if (((AggPath *) path)->aggstrategy == AGG_HASHED ||
					((AggPath *) path)->aggstrategy == AGG_MIXED)
					hashed = true;
				aggregated = true;
				break;
			case T_GroupingSetsPath:
				if (((GroupingSetsPath *) path)->aggstrategy == AGG_HASHED ||
					((GroupingSetsPath *) path)->aggstrategy == AGG_MIXED)
					hashed = true;
				aggregated = true;
				break;
			case T_GroupPath:
#if PG_VERSION_NUM >= 190000
			case T_UniquePath:
#else
			case T_UpperUniquePath:
#endif
				aggregated = true;
				break;
			case T_HashPath:
				return LION_COMPETITOR_HASHJOIN;
			case T_MergePath:
				return LION_COMPETITOR_MERGEJOIN;
			case T_NestPath:
				if (lion_nestloop_probes_index((JoinPath *) path))
					return LION_COMPETITOR_NESTLOOP;
				return hashed ? LION_COMPETITOR_HASHAGG :
					LION_COMPETITOR_OTHER;
			case T_BitmapHeapPath:
				return hashed ? LION_COMPETITOR_HASHAGG :
					LION_COMPETITOR_BITMAP;
			case T_IndexPath:
				if (hashed)
					return LION_COMPETITOR_HASHAGG;
				if (aggregated)
					return LION_COMPETITOR_AGG;
				return (path->pathtype == T_IndexOnlyScan) ?
					LION_COMPETITOR_INDEXONLY : LION_COMPETITOR_INDEX;
			case T_Path:
				if (hashed)
					return LION_COMPETITOR_HASHAGG;
				if (path->pathtype != T_SeqScan)
					return LION_COMPETITOR_OTHER;
				return aggregated ? LION_COMPETITOR_AGG :
					LION_COMPETITOR_SEQSCAN;
			case T_AppendPath:
			case T_MergeAppendPath:
				{
					List	   *subpaths = IsA(path, AppendPath) ?
						((AppendPath *) path)->subpaths :
						((MergeAppendPath *) path)->subpaths;
					Path	   *dearest = NULL;
					ListCell   *lc;

					foreach(lc, subpaths)
					{
						Path	   *p = (Path *) lfirst(lc);

						if (dearest == NULL ||
							p->total_cost > dearest->total_cost)
							dearest = p;
					}
					if (dearest == NULL)
						return hashed ? LION_COMPETITOR_HASHAGG :
							LION_COMPETITOR_OTHER;
					path = dearest;
					continue;
				}
			default:
				break;
		}
		path = lion_path_input(path);
	}
	return hashed ? LION_COMPETITOR_HASHAGG : LION_COMPETITOR_OTHER;
}

/*
 * The rate of a kind of core plan: the cost units a millisecond its plans
 * run at, as a multiple of the 500 lion's CPU constants are fitted at.  The
 * scans the constants were fitted against are the reference, 1, and so are
 * nested loops, whose rate moved no plan (DESIGN.md §39, "Held out").
 */
double
lion_competitor_rate(LionCompetitor kind)
{
	switch (kind)
	{
		case LION_COMPETITOR_AGG:
			return lion_agg_rate;
		case LION_COMPETITOR_HASHJOIN:
			return lion_hashjoin_rate;
		case LION_COMPETITOR_MERGEJOIN:
			return lion_mergejoin_rate;
		case LION_COMPETITOR_BITMAP:
			return lion_bitmap_rate;
		case LION_COMPETITOR_HASHAGG:	/* per plan: lion_hashagg_plan_rate() */
		case LION_COMPETITOR_NONE:
		case LION_COMPETITOR_DISABLED:
		case LION_COMPETITOR_NESTLOOP:
		case LION_COMPETITOR_SEQSCAN:
		case LION_COMPETITOR_INDEXONLY:
		case LION_COMPETITOR_INDEX:
		case LION_COMPETITOR_OTHER:
			break;
	}
	return 1.0;
}

/*
 * The rate of a plan that hashes, path (DESIGN.md §39, "A hashed aggregate's
 * own rate"): not one rate for the whole plan.  What core charges far below
 * its time is the hashing alone - a cpu_operator_cost or two a row for about
 * 100 ns - and the scan or join under it runs at its own rate, near the
 * reference: a hash aggregate over a whole table spends most of its time
 * hashing, one over the few thousand rows of a selective index scan almost
 * none.  So the hashed aggregates' own cost, total less input, is taken at
 * pg_lion.hashagg_rate and the rest at the reference, and the plan's rate is
 * its cost over that: its cost units a millisecond, as a multiple of 500.
 * Down from the top through what passes rows up, every hashed aggregate on
 * the way counted (a Finalize over the Partial ones of a parallel plan), to
 * the first join or scan; an Append's dearest child, as for its kind.
 */
static double
lion_hashagg_plan_rate(Path *path)
{
	Cost		total = (path != NULL) ? path->total_cost : 0.0;
	Cost		hashing = 0.0;
	double		rate = (lion_hashagg_rate > 0.0) ? lion_hashagg_rate : 1.0;
	Cost		reference;

	while (path != NULL)
	{
		Path	   *input = NULL;
		bool		hashes = false;

		switch (nodeTag(path))
		{
			case T_AggPath:
				input = ((AggPath *) path)->subpath;
				hashes = (((AggPath *) path)->aggstrategy == AGG_HASHED ||
						  ((AggPath *) path)->aggstrategy == AGG_MIXED);
				break;
			case T_GroupingSetsPath:
				input = ((GroupingSetsPath *) path)->subpath;
				hashes = (((GroupingSetsPath *) path)->aggstrategy == AGG_HASHED ||
						  ((GroupingSetsPath *) path)->aggstrategy == AGG_MIXED);
				break;
			case T_NestPath:
			case T_MergePath:
			case T_HashPath:
			case T_BitmapHeapPath:
			case T_IndexPath:
			case T_Path:
				path = NULL;
				continue;
			case T_AppendPath:
			case T_MergeAppendPath:
				{
					List	   *subpaths = IsA(path, AppendPath) ?
						((AppendPath *) path)->subpaths :
						((MergeAppendPath *) path)->subpaths;
					Path	   *dearest = NULL;
					ListCell   *lc;

					foreach(lc, subpaths)
					{
						Path	   *p = (Path *) lfirst(lc);

						if (dearest == NULL ||
							p->total_cost > dearest->total_cost)
							dearest = p;
					}
					path = dearest;
					continue;
				}
			default:
				break;
		}
		if (hashes && input != NULL && path->total_cost > input->total_cost)
			hashing += path->total_cost - input->total_cost;
		path = (input != NULL) ? input : lion_path_input(path);
	}
	if (total <= 0.0 || hashing <= 0.0 || hashing > total)
		return 1.0;
	reference = (total - hashing) + hashing / rate;
	return total / reference;
}

/*
 * Is a the cheaper of two paths in add_path()'s order: fewer disabled nodes
 * first (PostgreSQL 18), then total cost?
 */
static bool
lion_path_cheaper(const Path *a, const Path *b)
{
#if PG_VERSION_NUM >= 180000
	if (a->disabled_nodes != b->disabled_nodes)
		return a->disabled_nodes < b->disabled_nodes;
#endif
	return a->total_cost < b->total_cost;
}

/*
 * The cheapest of core's paths in rel so far: unparameterized, and with no
 * lion path inside (lion_path_has_lion()).  NULL when there is none.  The
 * upper rel has every path of core's when create_upper_paths_hook runs, a
 * base rel when set_rel_pathlist_hook runs; a join rel has the paths of the
 * join order and join type set_join_pathlist_hook is called for, and those of
 * the ones before it.
 */
Path *
lion_competitor_path(RelOptInfo *rel)
{
	Path	   *best = NULL;
	ListCell   *lc;

	foreach(lc, rel->pathlist)
	{
		Path	   *p = (Path *) lfirst(lc);

		if (p->param_info != NULL)
			continue;
		if (lion_path_has_lion(p))
			continue;
		if (best == NULL || lion_path_cheaper(p, best))
			best = p;
	}
	return best;
}

/*
 * Is a path one core's enable_* settings have disabled: a node of it is
 * (PostgreSQL 18's disabled_nodes), or its cost holds disable_cost?
 */
bool
lion_path_disabled(const Path *p)
{
#if PG_VERSION_NUM >= 180000
	return p->disabled_nodes > 0;
#else
	return p->total_cost >= disable_cost;
#endif
}

/*
 * Disable a lion path the way core's enable_* settings disable the index
 * access it stands for (REFERENCE.md, "Settings"): kept, but marked, so that
 * it is chosen only where nothing enabled is left - a disabled node more
 * (PostgreSQL 18), or disable_cost on its price, as cost_index() charged it
 * before.  enable_indexscan disables every index scan, index-only ones
 * included, as cost_index() does; a path that answers from the index alone
 * (LION_CORE_INDEXONLY) is disabled by enable_indexonlyscan too, since it has
 * no form that reads the rows to fall back to, which core's index-only scans
 * have.  Called once its price is final: add_path() compares the price.
 */
void
lion_path_core_switches(Path *path, LionCoreScan scan)
{
	if (enable_indexscan &&
		(scan != LION_CORE_INDEXONLY || enable_indexonlyscan))
		return;
#if PG_VERSION_NUM >= 180000
	path->disabled_nodes++;
#else
	path->startup_cost += disable_cost;
	path->total_cost += disable_cost;
#endif
}

/* Does a bitmap path's tree of quals read a lion index? */
static bool
lion_bitmap_reads_lion(Path *bitmapqual, Oid lionam)
{
	ListCell   *lc;

	if (IsA(bitmapqual, IndexPath))
		return ((IndexPath *) bitmapqual)->indexinfo->relam == lionam;
	if (IsA(bitmapqual, BitmapAndPath))
	{
		foreach(lc, ((BitmapAndPath *) bitmapqual)->bitmapquals)
			if (lion_bitmap_reads_lion((Path *) lfirst(lc), lionam))
				return true;
	}
	else if (IsA(bitmapqual, BitmapOrPath))
	{
		foreach(lc, ((BitmapOrPath *) bitmapqual)->bitmapquals)
			if (lion_bitmap_reads_lion((Path *) lfirst(lc), lionam))
				return true;
	}
	return false;
}

static bool lion_path_is_lion_scan(Path *path);

/*
 * Is a partitioned table's scan - the Append or MergeAppend over its
 * partitions, subpaths - a scan of lion indexes through the AM
 * (lion_path_is_lion_scan())?  When the partitions scanned that way cost at
 * least ten times the others: an empty partition, or one of a page or two,
 * takes a sequential scan whose price is next to nothing, and the whole is
 * priced by lion's model but for that; a partition scanned another way at a
 * price that counts makes the price core's in part, where the margin's hedge
 * still belongs.  No partition at all is not a scan of anything.
 */
#define LION_APPEND_OTHER_SHARE	0.1

static bool
lion_paths_are_lion_scans(List *subpaths)
{
	ListCell   *lc;
	Cost		lion = 0.0;
	Cost		other = 0.0;

	foreach(lc, subpaths)
	{
		Path	   *sub = (Path *) lfirst(lc);

		if (lion_path_is_lion_scan(sub))
			lion += sub->total_cost;
		else
			other += sub->total_cost;
	}
	return lion > 0.0 && other <= lion * LION_APPEND_OTHER_SHARE;
}

/*
 * Is a core path a scan of a lion index through the AM - a plain, index-only
 * or bitmap scan of one, or an Append or MergeAppend of such scans of a
 * partitioned table's partitions, under the nodes that pass its rows up and
 * no join?  Its price is lion's own model's (lioncostestimate()), with lion's
 * errors.
 */
static bool
lion_path_is_lion_scan(Path *path)
{
	Oid			lionam = lion_get_am_oid();

	while (path != NULL && OidIsValid(lionam))
	{
		switch (nodeTag(path))
		{
			case T_IndexPath:
				return ((IndexPath *) path)->indexinfo->relam == lionam;
			case T_BitmapHeapPath:
				return lion_bitmap_reads_lion(((BitmapHeapPath *) path)->bitmapqual,
											  lionam);
			case T_AppendPath:
				return lion_paths_are_lion_scans(((AppendPath *) path)->subpaths);
			case T_MergeAppendPath:
				return lion_paths_are_lion_scans(((MergeAppendPath *) path)->subpaths);
			case T_NestPath:
			case T_MergePath:
			case T_HashPath:
				return false;
			default:
				break;
		}
		path = lion_path_input(path);
	}
	return false;
}

/*
 * What a page read outside the cache is charged over the random_page_cost
 * core charges it (DESIGN.md §39, "A nested loop's cold reads"):
 * LION_COLD_PAGE_COST less the tablespace's random_page_cost, in the share of
 * the query's tables and idxpages of index that effective_cache_size cannot
 * hold.  0 when it holds them all.
 */
Cost
lion_units_cold_page(PlannerInfo *root, double idxpages, Oid reltablespace)
{
	double		resident;
	double		spc_random_page_cost;
	double		spc_seq_page_cost;

	resident = Min((double) effective_cache_size /
				   Max(root->total_table_pages + idxpages, 1.0), 1.0);
	get_tablespace_page_costs(reltablespace, &spc_random_page_cost,
							  &spc_seq_page_cost);
	if (resident >= 1.0 || LION_COLD_PAGE_COST <= spc_random_page_cost)
		return 0.0;
	return (1.0 - resident) * (LION_COLD_PAGE_COST - spc_random_page_cost);
}

/*
 * A NESTED LOOP'S COLD READS (DESIGN.md §39, "A nested loop's cold reads"):
 * what the inner index scan of path - a nested loop into a parameterized
 * index scan, under whatever passes its rows up - would pay for the pages it
 * finds outside the cache, over what core charged them, into *cold; and what
 * each page a lion path reads outside the cache is charged the same way,
 * into *perpage.  Both 0 when nothing is cold.
 *
 * Core prices the inner scan's page reads at random_page_cost, as many as
 * index_pages_fetched() makes loops x its rows a probe come to - an index leaf
 * and the heap pages of the rows each probe fetches.  A page that is not in
 * the cache costs a probe far more than that: a synchronous read, the probe
 * waiting on the device, since nothing reads ahead of a parameterized scan.
 * A nested loop of a few thousand probes into a table the cache holds meets
 * no such page once warm; one of a hundred thousand probes into a table
 * several times the cache reads most of its pages from the device whenever
 * the cache holds other things.  So the pages are taken as uncached in the
 * share of the query's tables and the index that effective_cache_size cannot
 * hold - the residency lion_index_page_cost() argues from - and each is
 * charged LION_COLD_PAGE_COST less the random_page_cost core charged it.  A
 * Memoize over the inner scan, which leaves the probes it saves unknown, and
 * an inner side that is not one index scan, take no charge; probes a semi
 * join stops early are counted whole.  The heap pages are counted at the
 * rows a probe of a skewed key meets, `skew` times core's (§39, "A skewed
 * key's probes"; lion_competitor_skew()).
 */
static void
lion_nestloop_cold(PlannerInfo *root, Path *path, Cost *cold, Cost *perpage,
				   double skew)
{
	JoinPath   *jp = NULL;
	Path	   *inner;
	IndexPath  *ip;
	RelOptInfo *baserel;
	double		idxpages;
	double		loops;
	double		reads;

	*cold = 0.0;
	*perpage = 0.0;
	if (root == NULL)
		return;
	for (; path != NULL; path = lion_path_input(path))
	{
		if (IsA(path, NestPath))
		{
			jp = (JoinPath *) path;
			break;
		}
	}
	if (jp == NULL)
		return;

	inner = jp->innerjoinpath;
	while (inner != NULL &&
		   (IsA(inner, MaterialPath) || IsA(inner, ProjectionPath)))
		inner = lion_path_input(inner);
	if (inner == NULL || !IsA(inner, IndexPath) || inner->param_info == NULL)
		return;
	ip = (IndexPath *) inner;
	baserel = ip->path.parent;
	if (baserel == NULL || baserel->reloptkind != RELOPT_BASEREL)
		return;

	idxpages = Max((double) ip->indexinfo->pages, 1.0);
	*perpage = lion_units_cold_page(root, idxpages, baserel->reltablespace);
	if (*perpage <= 0.0)
		return;

	/* a leaf a probe, and the heap pages of its rows */
	loops = Max(jp->outerjoinpath->rows, 1.0);
	reads = index_pages_fetched(loops, (BlockNumber) idxpages, idxpages, root);
	if (ip->path.pathtype != T_IndexOnlyScan || baserel->allvisfrac < 1.0)
	{
		double		heap = index_pages_fetched(loops *
											   Max(Min(ip->indexselectivity *
													   skew, 1.0) *
												   baserel->tuples, 1.0),
											   baserel->pages, idxpages,
											   root);

		if (ip->path.pathtype == T_IndexOnlyScan)
			heap *= 1.0 - baserel->allvisfrac;
		reads += heap;
	}
	*cold = reads * *perpage;
}

/*
 * Does a semi or anti join's probe of a key with no match end at once: does
 * the index of the loop's inner side answer every join clause?  This is
 * core's has_indexed_join_quals() (costsize.c) restated, since that function
 * is static: final_cost_nestloop() charges an unmatched outer row a full
 * inner scan wherever it says no.  The join itself has no clause left to
 * evaluate (joinrestrictinfo); the inner side is a parameterized index or
 * index-only scan, or a bitmap heap scan over one index scan (not an AND or
 * an OR of them), with nothing above it; and each clause its
 * parameterization takes from the join's outer side
 * (join_clause_is_movable_into() the inner rel within the join) is one of
 * its index clauses or of the same equivalence class as one
 * (is_redundant_with_indexclauses()), with at least one such clause.  The
 * inner side's restriction clauses are not looked at, as core does not look
 * at them.
 */
static bool
lion_competitor_key_only(JoinPath *jp)
{
	Path	   *inner = jp->innerjoinpath;
	List	   *indexclauses;
	bool		found = false;
	ListCell   *lc;

	if (jp->joinrestrictinfo != NIL)
		return false;
	if (inner == NULL || inner->param_info == NULL || inner->parent == NULL ||
		jp->path.parent == NULL)
		return false;
	switch (inner->pathtype)
	{
		case T_IndexScan:
		case T_IndexOnlyScan:
			if (!IsA(inner, IndexPath))
				return false;
			indexclauses = ((IndexPath *) inner)->indexclauses;
			break;
		case T_BitmapHeapScan:
			{
				Path	   *bitmapqual;

				if (!IsA(inner, BitmapHeapPath))
					return false;
				bitmapqual = ((BitmapHeapPath *) inner)->bitmapqual;
				if (bitmapqual == NULL || !IsA(bitmapqual, IndexPath))
					return false;
				indexclauses = ((IndexPath *) bitmapqual)->indexclauses;
				break;
			}
		default:
			return false;
	}
	foreach(lc, indexclauses)
	{
		IndexClause *iclause = (IndexClause *) lfirst(lc);

		if (iclause == NULL || !IsA(iclause, IndexClause) ||
			iclause->rinfo == NULL)
			return false;
	}
	foreach(lc, inner->param_info->ppi_clauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		if (rinfo == NULL || !IsA(rinfo, RestrictInfo))
			return false;
		if (!join_clause_is_movable_into(rinfo, inner->parent->relids,
										 jp->path.parent->relids))
			continue;
		if (!is_redundant_with_indexclauses(rinfo, indexclauses))
			return false;
		found = true;
	}
	return found;
}

/*
 * A SKEWED KEY'S PROBES (§39): how many times core's rows per key a nested
 * loop's probes of a skewed key column meet (lion_skew_scan_factor()), in
 * *extra what the loop costs more at them - its inner side an index or a
 * bitmap heap scan parameterized by the outer rows, under a Material or a
 * projection - and in *outer what its outer side costs.  The extra is what
 * core's own model of the loop (final_cost_nestloop()) charges more at the
 * skewed rows.  An inner join's every probe reads its key's rows: the loops
 * times one scan's extra.  A probe into an inner side core proved unique
 * stops at its key's first match: 1.  A semi or anti join's matched outer
 * row reads 2 / (match_count + 1) of a scan, and the match count grows with
 * the key's rows, so the first match comes no later at a heavy key.  Its
 * unmatched outer row costs a fraction of a scan where the index answers
 * every join clause (lion_competitor_key_only()), which more rows do not
 * change: 1 for those loops, as for any other inner side or plan.  Otherwise
 * core charges an unmatched row a full scan, which grows with the key's
 * rows: the extra is those rows times one scan's extra - the outer rows less
 * the join's for a semi join, which emits the matched ones, and the join's
 * rows for an anti join, which emits the unmatched ones.  A loop below a
 * subquery scan is the subquery's, and its Vars are looked up in the
 * subquery's planner info.
 */
static double
lion_competitor_skew(PlannerInfo *root, Path *best, Cost *extra, Cost *outer)
{
	JoinPath   *jp = NULL;
	Path	   *scan;
	double		loops;
	double		probes;
	double		skew;

	*extra = 0.0;
	*outer = 0.0;
	if (root == NULL)
		return 1.0;
	for (scan = best; scan != NULL; scan = lion_path_input(scan))
	{
		if (IsA(scan, NestPath))
		{
			jp = (JoinPath *) scan;
			break;
		}
		if (IsA(scan, SubqueryScanPath))
		{
			root = (scan->parent != NULL) ? scan->parent->subroot : NULL;
			if (root == NULL)
				return 1.0;
		}
	}
	if (jp == NULL || jp->inner_unique)
		return 1.0;
	scan = jp->innerjoinpath;
	while (scan != NULL &&
		   (IsA(scan, MaterialPath) || IsA(scan, ProjectionPath)))
		scan = lion_path_input(scan);
	if (scan == NULL || scan->param_info == NULL ||
		(!IsA(scan, IndexPath) && !IsA(scan, BitmapHeapPath)))
		return 1.0;
	if ((jp->jointype == JOIN_SEMI || jp->jointype == JOIN_ANTI) &&
		lion_competitor_key_only(jp))
		return 1.0;
	loops = Max(jp->outerjoinpath->rows, 1.0);

	/* the probes core charges a full scan: the unmatched outer rows */
	if (jp->jointype == JOIN_SEMI)
		probes = Max(loops - jp->path.rows, 0.0);
	else if (jp->jointype == JOIN_ANTI)
		probes = Min(Max(jp->path.rows, 0.0), loops);
	else
		probes = loops;
	skew = lion_skew_scan_factor(root, scan);
	if (skew > 1.0 && probes > 0.0)
		*extra = probes * lion_skew_scan_extra(root, scan, skew, loops);
	*outer = jp->outerjoinpath->total_cost;
	return skew;
}

/*
 * A lion path whose own price is `own`, in lion's units, and which reads
 * `reads` pages, priced against a nested loop with cold reads
 * (lion_nestloop_cold()): both plans with their uncached pages added, the
 * loop's cost and cold reads against the path's price and its own cold reads
 * - the path at (own x rate + its cold reads) / (cost + the loop's cold
 * reads) of the loop's cost, which is a factor of its rate.  For the FK-side
 * join, whose reads lion_cost_fkjoin_path() counts: a lookup's leaf and set,
 * the fact filters located, the heap of a recheck.  Each path of a relation
 * takes its own reads, so that two of lion's forms compare as their prices
 * and their cold reads together say.  Nothing for any other kind of
 * competitor, or a loop with nothing cold.
 *
 * The factor is at most 1 - a path that reads more than the loop keeps its
 * price - and never below 1 / pg_lion.cold_max_discount (lion_cold_cap()).
 * The charge is what a cold cache costs, and a warm one costs none of it:
 * with the inner table's working set cached, the nested loop runs at core's
 * price and the lion path can be the slower, by up to that factor where its
 * own price is the dearer.
 *
 * A loop whose probes meet more rows than core priced, on a skewed key, and
 * a path priced at those rows (lion_units_take_skew()), are weighed with the
 * loop at its price at them, nlcost + nlextra: the correction is the
 * skewscale lion_units_price() applies, outside the bound, and the cold
 * reads are the factor on top of it, within it, as before.  The two do not
 * count nlextra twice: the skewscale converts the path's price into core's
 * units, and the factor is each side's cold reads over its own warm price at
 * the rows both meet, a ratio in which nlextra is only the loop's share of
 * that price.  Only the FK-side join comes here, and its skewscale is this
 * loop's (lion_units_take_skew()); a count's, from its relation's own scan
 * (lion_skew_clauses_scale()), never meets the cold reads.
 *
 * A loop over a Memoize whose LIMIT meets fewer hits than core priced
 * (lion_memoize_limit_scale()) is weighed at its price with them as well,
 * nlcost / memoscale - nlcost more: lion_memoize_limit_extra() brought into
 * the frame of its cost, added to nlextra where both apply.  memoscale itself
 * multiplies the path's price (lion_units_price()), outside the bound, as the
 * skewscale does; the factor is each side's cold reads over its own warm
 * price, in which the extra is the loop's.
 *
 * A competitor with no loop's cold reads but a bitmap heap scan's
 * (lion_bitmap_cold()) is priced against those instead
 * (lion_units_bitmap_cold()).
 */
static double lion_cold_cap(void);

void
lion_units_cold(LionUnits *u, Cost own, double reads)
{
	Cost		price = own * u->rate;
	Cost		loop = u->nlcost + ((u->skewscale < 1.0) ? u->nlextra : 0.0) +
		((u->memoscale > 0.0 && u->memoscale < 1.0) ?
		 u->nlcost / u->memoscale - u->nlcost : 0.0);
	Cost		lioncold;
	double		factor;

	if (u->bmcold > 0.0)
	{
		lion_units_bitmap_cold(u, own, reads);
		return;
	}
	if (u->kind != LION_COMPETITOR_NESTLOOP || u->nlcold <= 0.0 ||
		u->nlcost <= 0.0 || price <= 0.0)
		return;
	lioncold = Max(reads, 0.0) * u->coldpage;
	factor = (price + lioncold) * loop / ((loop + u->nlcold) * price);
	factor = Min(Max(factor, 1.0 / lion_cold_cap()), 1.0);
	elog(DEBUG2, "lion: a nested loop's cold reads, %.2f over its %.2f against %.2f over %.2f, at factor %g",
		 u->nlcold, loop, lioncold, price, factor);
	u->rate *= factor;
}

/*
 * The most a competitor's cold reads divide a lion path's price by,
 * pg_lion.cold_max_discount (DESIGN.md §39): the warm floor.
 */
static double
lion_cold_cap(void)
{
	return Max(lion_cold_max_discount, 1.0);
}

/*
 * What lion_bitmap_reads() sums over the bitmap heap scans of a core path
 */
typedef struct LionBitmapReads
{
	double		pages;			/* the pages they read, heap and index */
	Cost		over;			/* ... each at the cold price less core's */
	double		idxpages;		/* the pages of the indexes they read */
	double		heappages;		/* the most heap pages one of them reads */
	double		random;			/* ... and its tablespace's random_page_cost */
} LionBitmapReads;

/*
 * The index pages of a bitmap's tree of quals, into r: each leaf's index
 * whole in r->idxpages, as core's get_indexpath_pages() counts them for the
 * residency, and the pages a scan of it reads - its selectivity's share of
 * the index, a page at least, as genericcostestimate() counts a B-tree's and
 * as a lion index's sets hold their rows - over the loops, as
 * index_pages_fetched() counts a page that a later loop finds cached; each
 * charged the cold price over the random_page_cost core's estimate charges
 * an index page.
 */
static void
lion_bitmap_index_reads(PlannerInfo *root, Path *bitmapqual, double loops,
						LionBitmapReads *r)
{
	List	   *quals = NIL;
	ListCell   *lc;

	check_stack_depth();
	if (IsA(bitmapqual, IndexPath))
	{
		IndexPath  *ip = (IndexPath *) bitmapqual;
		double		ipages = Max((double) ip->indexinfo->pages, 1.0);
		double		read = Min(Max(ceil(ip->indexselectivity * ipages), 1.0),
							   ipages);
		double		spc_random;
		double		spc_seq;

		if (loops > 1.0)
			read = index_pages_fetched(read * loops, (BlockNumber) ipages,
									   ipages, root);
		get_tablespace_page_costs(ip->indexinfo->reltablespace,
								  &spc_random, &spc_seq);
		r->idxpages += ipages;
		r->pages += read;
		r->over += read * Max(LION_COLD_STREAM_PAGE_COST - spc_random, 0.0);
		return;
	}
	if (IsA(bitmapqual, BitmapAndPath))
		quals = ((BitmapAndPath *) bitmapqual)->bitmapquals;
	else if (IsA(bitmapqual, BitmapOrPath))
		quals = ((BitmapOrPath *) bitmapqual)->bitmapquals;
	foreach(lc, quals)
		lion_bitmap_index_reads(root, (Path *) lfirst(lc), loops, r);
}

/*
 * The calls a Memoize was priced for: the rescans its loop makes, one for
 * each outer row (PostgreSQL 19 calls them est_calls).
 */
static double
lion_memoize_calls(MemoizePath *mpath)
{
#if PG_VERSION_NUM >= 190000
	return Max(mpath->est_calls, 1.0);
#else
	return Max(mpath->calls, 1.0);
#endif
}

/*
 * The distinct values of a Memoize's keys over its calls, as
 * cost_memoize_rescan() estimates them: estimate_num_groups() of its keys,
 * all the calls where that estimate fell back on a default, at least 1 and at
 * most the calls.  PostgreSQL 19 keeps the figure on the path once it is
 * costed (est_unique_keys).
 */
static double
lion_memoize_ndistinct(PlannerInfo *root, MemoizePath *mpath)
{
	double		calls = lion_memoize_calls(mpath);
	double		ndistinct;
	EstimationInfo estinfo;

#if PG_VERSION_NUM >= 190000
	if (mpath->est_unique_keys > 0.0)
		return Min(Max(mpath->est_unique_keys, 1.0), calls);
#endif
	memset(&estinfo, 0, sizeof(estinfo));
	ndistinct = estimate_num_groups(root, mpath->param_exprs, calls, NULL,
									&estinfo);
	if ((estinfo.flags & SELFLAG_USED_DEFAULT) != 0)
		ndistinct = calls;
	return Min(Max(ndistinct, 1.0), calls);
}

/*
 * The hit ratio cost_memoize_rescan() priced a Memoize at: of its calls, the
 * distinct values of its keys (lion_memoize_ndistinct()), of which the cache
 * holds est_entries at once, hit (calls - ndistinct) / calls x est_entries /
 * ndistinct of the time.  est_entries is what cost_memoize_rescan() set it
 * to, the least of ndistinct and the entries hash_mem holds - 0 where one
 * entry does not fit, which it prices as every call a miss.  A Memoize not
 * costed yet (create_memoize_path() leaves est_entries 0, and cost_rescan()
 * is reached only for an outer side of more than one row) is taken the same
 * way: it is scanned once a call, and is called about once.  From PostgreSQL
 * 19 the path keeps the ratio itself (est_hit_ratio, 0 until it is costed).
 */
static double
lion_memoize_hit_ratio(PlannerInfo *root, MemoizePath *mpath)
{
#if PG_VERSION_NUM >= 190000
	return mpath->est_hit_ratio;
#else
	double		calls = lion_memoize_calls(mpath);
	double		ndistinct;
	double		entries;

	if (mpath->est_entries == 0)
		return 0.0;
	ndistinct = lion_memoize_ndistinct(root, mpath);
	entries = Min((double) mpath->est_entries, ndistinct);
	return ((calls - ndistinct) / calls) * (entries / ndistinct);
#endif
}

/*
 * The scans a Memoize makes of its input over `loops` rescans of it: its
 * cache misses, at the hit ratio it was priced at (lion_memoize_hit_ratio()).
 * A scan at least.
 */
static double
lion_memoize_misses(PlannerInfo *root, MemoizePath *mpath, double loops)
{
	double		hit_ratio = Min(Max(lion_memoize_hit_ratio(root, mpath), 0.0),
								1.0);

	return Max(loops * (1.0 - hit_ratio), 1.0);
}

/*
 * A MEMOIZE UNDER A LIMIT (DESIGN.md §39, "A Memoize under a LIMIT"): what a
 * nested loop over a Memoize costs more than core priced it at, when the
 * query's LIMIT takes `share` of its rows.  cost_memoize_rescan() prices
 * each of the Memoize's calls at the hit ratio hr of all of them
 * (lion_memoize_hit_ratio()), a miss at its input's total cost and a hit at
 * the lookup a miss pays too.  The loop's price of a LIMIT's share of its
 * rows is that share of its run, so it counts hr of the first n = share x
 * calls calls hits, where those n calls repeat only the keys among
 * themselves.  Of ndistinct keys drawn about uniformly, n calls hold
 *
 *	d(n) = ndistinct x (1 - (1 - 1 / ndistinct)^n)
 *
 * distinct ones, and hit n - d(n) times where the cache holds them all, of
 * which est_entries / d(n) where it holds fewer, as core takes est_entries /
 * ndistinct of the whole walk's:
 *
 *	hits(n) = (n - d(n)) x min(1, est_entries / d(n))
 *
 * and the loop's price of its first n calls lacks
 *
 *	extra = n x (hr - hits(n) / n) x the input's total cost
 *
 * where that is positive.  Over 200,000 keys, 26,000 calls hit about 6% of
 * the time; core's 625,000 calls of the whole walk, 68%.  0 with
 * pg_lion.enable_memoize_limit off, with share 1 or more, and where path is
 * not a nested loop (under a projection) whose inner side is a Memoize (under
 * a Material or a projection); a Memoize core priced as never hitting
 * (est_entries 0) lacks nothing.
 */
Cost
lion_memoize_limit_extra(PlannerInfo *root, Path *path, double share)
{
	JoinPath   *jp;
	Path	   *inner;
	MemoizePath *mpath;
	double		calls;
	double		ndistinct;
	double		hr;
	double		n;
	double		dn;
	double		hits;
	Cost		extra;

	if (!lion_enable_memoize_limit || root == NULL || path == NULL ||
		share <= 0.0 || share >= 1.0)
		return 0.0;
	while (path != NULL && IsA(path, ProjectionPath))
		path = ((ProjectionPath *) path)->subpath;
	if (path == NULL || !IsA(path, NestPath))
		return 0.0;
	jp = (JoinPath *) path;
	inner = jp->innerjoinpath;
	while (inner != NULL &&
		   (IsA(inner, MaterialPath) || IsA(inner, ProjectionPath)))
		inner = lion_path_input(inner);
	if (inner == NULL || !IsA(inner, MemoizePath))
		return 0.0;
	mpath = (MemoizePath *) inner;
	if (mpath->est_entries == 0 || mpath->subpath == NULL)
		return 0.0;
	hr = Min(Max(lion_memoize_hit_ratio(root, mpath), 0.0), 1.0);
	if (hr <= 0.0)
		return 0.0;
	calls = lion_memoize_calls(mpath);
	ndistinct = lion_memoize_ndistinct(root, mpath);
	n = Max(share * calls, 1.0);
	if (n >= calls)
		return 0.0;
	dn = (ndistinct > 1.0) ?
		ndistinct * (1.0 - exp(n * log1p(-1.0 / ndistinct))) : 1.0;
	dn = Min(Max(dn, 1.0), n);
	hits = (n - dn) * Min((double) mpath->est_entries / dn, 1.0);
	extra = n * (hr - hits / n) * mpath->subpath->total_cost;
	if (extra <= 0.0)
		return 0.0;
	elog(DEBUG2, "lion: a Memoize under a LIMIT, %.0f of its %.0f calls over %.0f keys hitting %g where core took %g: %.2f more",
		 n, calls, ndistinct, hits / n, hr, extra);
	return extra;
}

/*
 * The share of its price a nested loop competitor over a Memoize is priced
 * at under the query's LIMIT, against its price with the hits that share of
 * its calls lacks (lion_memoize_limit_extra()): its price of the LIMIT's
 * share of its rows - its startup and that share of its run, as the Limit
 * over it takes - over that and the extra.  Only where the LIMIT takes a
 * share of the loop's own rows: the query has one (limit_tuples, which the
 * planner leaves -1 under a grouping, an aggregate or a window), and the
 * loop's order gives the query at least its first sort key.  1 elsewhere.
 */
static double
lion_memoize_limit_scale(PlannerInfo *root, Path *best)
{
	double		share;
	int			ncommon = 0;
	Cost		price;
	Cost		extra;

	if (root == NULL || best == NULL || root->limit_tuples <= 0.0 ||
		best->rows <= 0.0 || best->pathkeys == NIL ||
		root->query_pathkeys == NIL)
		return 1.0;
	(void) pathkeys_count_contained_in(root->query_pathkeys, best->pathkeys,
									   &ncommon);
	if (ncommon == 0)
		return 1.0;
	share = root->limit_tuples / best->rows;
	if (share >= 1.0)
		return 1.0;
	extra = lion_memoize_limit_extra(root, best, share);
	price = best->startup_cost +
		share * (best->total_cost - best->startup_cost);
	if (extra <= 0.0 || price <= 0.0)
		return 1.0;
	return price / (price + extra);
}

/*
 * The pages a core path's bitmap heap scans read, into r: each scan's heap
 * pages as compute_bitmap_pages() counts them, over `loops` scans - the outer
 * rows of the nested loops it is the inner side of, each of which scans it
 * again; one past a Material, which keeps its rows, and a Memoize's misses
 * past a Memoize (lion_memoize_misses()) - each
 * charged the cold price over the price cost_bitmap_heap_scan() charged it:
 * random_page_cost falling to seq_page_cost with the square root of the share
 * of the heap one scan reads; and the index pages its bitmap reads
 * (lion_bitmap_index_reads()).  Down through every node that passes rows up
 * and both sides of every join, and into a subquery under its own root; a
 * custom path, lion's or another's, reads nothing of core's.
 */
static void
lion_bitmap_reads(PlannerInfo *root, Path *path, double loops,
				  LionBitmapReads *r)
{
	ListCell   *lc;

	check_stack_depth();
	if (path == NULL)
		return;
	switch (nodeTag(path))
	{
		case T_BitmapHeapPath:
			{
				BitmapHeapPath *bp = (BitmapHeapPath *) path;
				RelOptInfo *baserel = bp->path.parent;
				Cost		indexcost;
				double		tuples;
				double		pages;
				double		T;
				double		perpage;
				double		spc_random;
				double		spc_seq;

				if (baserel == NULL || !IS_SIMPLE_REL(baserel) ||
					baserel->rtekind != RTE_RELATION)
					return;
				pages = lion_compute_bitmap_pages(root, baserel,
												  bp->bitmapqual, loops,
												  &indexcost, &tuples);
				T = (baserel->pages > 1) ? (double) baserel->pages : 1.0;
				get_tablespace_page_costs(baserel->reltablespace,
										  &spc_random, &spc_seq);
				perpage = (pages >= 2.0) ?
					spc_random - (spc_random - spc_seq) * sqrt(pages / T) :
					spc_random;
				pages *= loops; /* compute_bitmap_pages() gives one loop's */
				r->pages += pages;
				r->over += pages * Max(LION_COLD_STREAM_PAGE_COST - perpage,
									   0.0);
				if (pages > r->heappages)
				{
					r->heappages = pages;
					r->random = spc_random;
				}
				lion_bitmap_index_reads(root, bp->bitmapqual, loops, r);
				return;
			}
		case T_NestPath:
			{
				JoinPath   *jp = (JoinPath *) path;

				lion_bitmap_reads(root, jp->outerjoinpath, loops, r);
				lion_bitmap_reads(root, jp->innerjoinpath,
								  loops * Max(jp->outerjoinpath->rows, 1.0),
								  r);
				return;
			}
		case T_MergePath:
		case T_HashPath:
			lion_bitmap_reads(root, ((JoinPath *) path)->outerjoinpath,
							  loops, r);
			lion_bitmap_reads(root, ((JoinPath *) path)->innerjoinpath,
							  loops, r);
			return;
		case T_AppendPath:
			foreach(lc, ((AppendPath *) path)->subpaths)
				lion_bitmap_reads(root, (Path *) lfirst(lc), loops, r);
			return;
		case T_MergeAppendPath:
			foreach(lc, ((MergeAppendPath *) path)->subpaths)
				lion_bitmap_reads(root, (Path *) lfirst(lc), loops, r);
			return;
		case T_MaterialPath:
			lion_bitmap_reads(root, lion_path_input(path), 1.0, r);
			return;
		case T_SubqueryScanPath:
			{
				/*
				 * A subquery's paths were planned under its own root: their
				 * Vars are its range table's, and estimate_num_groups() or
				 * index_pages_fetched() asked under this one would read
				 * another query's (a Memoize's keys, its tables' pages).
				 */
				RelOptInfo *subrel = path->parent;

				if (subrel == NULL || subrel->subroot == NULL)
					return;
				lion_bitmap_reads(subrel->subroot,
								  ((SubqueryScanPath *) path)->subpath, loops,
								  r);
				return;
			}
		case T_MemoizePath:
			{
				MemoizePath *mp = (MemoizePath *) path;

				lion_bitmap_reads(root, mp->subpath,
								  lion_memoize_misses(root, mp, loops), r);
				return;
			}
		case T_CustomPath:
			return;
		default:
			lion_bitmap_reads(root, lion_path_input(path), loops, r);
			return;
	}
}

/*
 * A BITMAP HEAP SCAN'S COLD READS (DESIGN.md §39, "A bitmap heap scan's cold
 * reads"): what the bitmap heap scans of a core path - one, under an
 * aggregate or a Gather, or the scan of a join's fact side - would pay for
 * the pages they find outside the cache, over what core charged them, into
 * *cold; and what each page a lion path reads outside the cache is charged
 * the same way, into *perpage.  Both 0 when nothing is cold.
 *
 * Core prices a bitmap heap scan's pages at random_page_cost falling to
 * seq_page_cost by the share of the table it reads, as if each were cached
 * or a device's cheap random read; and a page outside the cache costs it a
 * read from the device, issued ahead of the scan (effective_io_concurrency),
 * so that the reads overlap - an eighth of a nested loop's synchronous one,
 * LION_COLD_STREAM_PAGE_COST, but several times what core charged.  The
 * share taken as uncached is the one lion_nestloop_cold() takes: what
 * effective_cache_size cannot hold of the query's tables and the indexes
 * the bitmaps read.  A lion path's own reads are charged the same price:
 * its posting chains, the B-tree's leaves a covering walk reads in order and
 * the heap pages a recheck reads through a read stream are each read in an
 * order the device or the kernel reads ahead of, as the bitmap heap scan's
 * are.
 */
static void
lion_bitmap_cold(PlannerInfo *root, Path *path, Cost *cold, Cost *perpage)
{
	LionBitmapReads r;
	double		resident;

	*cold = 0.0;
	*perpage = 0.0;
	if (root == NULL || path == NULL)
		return;
	memset(&r, 0, sizeof(r));
	lion_bitmap_reads(root, path, 1.0, &r);
	if (r.heappages <= 0.0 || r.over <= 0.0)
		return;
	resident = Min((double) effective_cache_size /
				   Max(root->total_table_pages + r.idxpages, 1.0), 1.0);
	if (resident >= 1.0)
		return;
	*cold = (1.0 - resident) * r.over;
	*perpage = (1.0 - resident) *
		Max(LION_COLD_STREAM_PAGE_COST - r.random, 0.0);
	elog(DEBUG2, "lion: a bitmap heap scan's %.0f pages, %.0f%% resident, read cold for %.2f",
		 r.pages, resident * 100.0, *cold);
}

/*
 * The factor a lion path's price, `price` in its competitor's units, takes
 * against a competitor of cost `cost` with `cold` of uncached reads, its own
 * being `lioncold`: both plans with their uncached reads added, the path at
 * (price + lioncold) / (cost + cold) of the competitor's cost, at most 1 and
 * never below 1 / pg_lion.cold_max_discount - lion_units_cold()'s, for a
 * loop.
 */
static double
lion_cold_factor(Cost price, Cost lioncold, Cost cost, Cost cold)
{
	double		factor;

	if (cold <= 0.0 || cost <= 0.0 || price <= 0.0)
		return 1.0;
	factor = (price + Max(lioncold, 0.0)) * cost / ((cost + cold) * price);
	return Min(Max(factor, 1.0 / lion_cold_cap()), 1.0);
}

/*
 * A lion path whose own price is `own`, in lion's units, and which reads
 * `reads` pages, priced against a competitor with a bitmap heap scan's cold
 * reads (lion_bitmap_cold()), as lion_units_cold() prices one against a
 * nested loop's: its rate times lion_cold_factor().  For the counts and the
 * FK-side join; nothing where the competitor has no such reads.
 */
void
lion_units_bitmap_cold(LionUnits *u, Cost own, double reads)
{
	double		factor = lion_units_bitmap_factor(u, own * u->rate, reads);

	u->rate *= factor;
}

/*
 * ... the factor itself, for a path whose price in its competitor's units is
 * `own`: the covering walks of a base rel (lion_units_scan_cold()), which
 * have no rate, multiply their own price by it.
 */
double
lion_units_bitmap_factor(const LionUnits *u, Cost own, double reads)
{
	Cost		lioncold;
	double		factor;

	if (u->bmcold <= 0.0 || u->nlcost <= 0.0 || own <= 0.0 || reads < 0.0)
		return 1.0;
	lioncold = reads * u->bmcoldpage;
	factor = lion_cold_factor(own, lioncold, u->nlcost, u->bmcold);
	elog(DEBUG2, "lion: a bitmap heap scan's cold reads, %.2f over its %.2f against %.2f over %.2f, at factor %g",
		 u->bmcold, u->nlcost, lioncold, own, factor);
	return factor;
}

/*
 * The bitmap heap scans' cold reads of `best`, the competitor of kind `kind`
 * found for a lion path (lion_competitor_of()), into *bmcold and *bmcoldpage:
 * where it is a plan of core's, enabled, and a nested loop's cold reads
 * (nlcold) do not price it already.
 */
static void
lion_competitor_bitmap_cold(PlannerInfo *root, Path *best,
							LionCompetitor kind, Cost nlcold,
							Cost *bmcold, Cost *bmcoldpage)
{
	*bmcold = 0.0;
	*bmcoldpage = 0.0;
	if (best == NULL || kind == LION_COMPETITOR_NONE ||
		kind == LION_COMPETITOR_DISABLED || nlcold > 0.0)
		return;
	lion_bitmap_cold(root, best, bmcold, bmcoldpage);
}

/*
 * The competitor of a base rel's covering walks (LionOrdered and
 * LionBtreeScan index-only, DESIGN.md §40), for their cold reads alone: the
 * cheapest of rel's scans of core's, serial (lion_competitor_path()) or
 * parallel - the cheapest partial path, unparameterized and with nothing of
 * lion's, under the Gather core puts over it: its cost and parallel_setup_cost
 * with a parallel_tuple_cost a row, as cost_gather() prices it.  Where that is
 * a bitmap heap scan with cold reads, u holds them and its cost
 * (lion_units_bitmap_factor() reads nothing else); otherwise u has none.  The
 * walks are not converted (their competitors' units are the reference), and
 * their margin is lion_units_margin_for()'s.
 */
void
lion_units_scan_cold(PlannerInfo *root, RelOptInfo *rel, LionUnits *u)
{
	Path	   *best = lion_competitor_path(rel);
	Path	   *partial = NULL;
	Cost		cost = (best != NULL) ? best->total_cost : 0.0;
	ListCell   *lc;

	memset(u, 0, sizeof(LionUnits));
	u->kind = LION_COMPETITOR_NONE;
	u->rate = 1.0;
	u->margin = 1.0;
	u->skewscale = 1.0;
	u->memoscale = 1.0;
	foreach(lc, rel->partial_pathlist)
	{
		Path	   *p = (Path *) lfirst(lc);

		if (p->param_info != NULL || lion_path_has_lion(p))
			continue;
		if (partial == NULL || lion_path_cheaper(p, partial))
			partial = p;
	}
	if (partial != NULL)
	{
		Cost		gathered = partial->total_cost + parallel_setup_cost +
			parallel_tuple_cost * clamp_row_est(rel->rows);

		if (best == NULL ||
#if PG_VERSION_NUM >= 180000
			partial->disabled_nodes < best->disabled_nodes ||
			(partial->disabled_nodes == best->disabled_nodes &&
			 gathered < cost))
#else
			gathered < cost)
#endif
		{
			best = partial;
			cost = gathered;
		}
	}
	if (best == NULL || lion_path_disabled(best))
		return;
	u->kind = lion_competitor_kind(best);
	if (u->kind != LION_COMPETITOR_BITMAP)
		return;
	u->nlcost = cost;
	lion_bitmap_cold(root, best, &u->bmcold, &u->bmcoldpage);
}

/*
 * What a lion path of rel competes with: the kind of the cheapest core path
 * (lion_competitor_path()) and its cost; NONE when core has no path there yet,
 * and DISABLED when every one it has is disabled - a plan forced by core's
 * enable_* settings, which leave a lion path nothing to compete with.
 * *ownscan says whether that path is one of the AM's own scans of a lion
 * index (lion_path_is_lion_scan()), *rate its rate; *nlcold and *coldpage
 * a nested loop's cold reads (lion_nestloop_cold()), when root is given;
 * *skew, *nlextra and *nlouter the rows its probes of a skewed key meet,
 * what it costs more at them and its outer side's cost
 * (lion_competitor_skew()); and *memoscale the share of its price a loop
 * over a Memoize is priced at under the LIMIT (lion_memoize_limit_scale()).
 */
static LionCompetitor
lion_competitor_of(PlannerInfo *root, RelOptInfo *rel, Cost *cost,
				   bool *ownscan, double *rate, Cost *nlcold, Cost *coldpage,
				   Cost *nlextra, Cost *nlouter, double *skew,
				   double *memoscale)
{
	Path	   *best = lion_competitor_path(rel);
	LionCompetitor kind;

	*cost = (best != NULL) ? best->total_cost : 0.0;
	*ownscan = false;
	*rate = 1.0;
	*nlcold = 0.0;
	*coldpage = 0.0;
	*nlextra = 0.0;
	*nlouter = 0.0;
	*skew = 1.0;
	*memoscale = 1.0;
	if (best == NULL)
		return LION_COMPETITOR_NONE;
	if (lion_path_disabled(best))
		return LION_COMPETITOR_DISABLED;
	*ownscan = lion_path_is_lion_scan(best);
	kind = lion_competitor_kind(best);
	*rate = (kind == LION_COMPETITOR_HASHAGG) ?
		lion_hashagg_plan_rate(best) : lion_competitor_rate(kind);
	if (kind == LION_COMPETITOR_NESTLOOP)
	{
		*skew = lion_competitor_skew(root, best, nlextra, nlouter);
		lion_nestloop_cold(root, best, nlcold, coldpage, *skew);
		*memoscale = lion_memoize_limit_scale(root, best);
	}
	return kind;
}

/*
 * The margin a lion path is offered at against a competitor of this kind:
 * pg_lion.pushdown_margin, or none - 1 - where there is nothing to hedge.
 * With no competitor (no path of core's yet, or none left enabled) the choice
 * is not between lion and core, and a plan forced by core's enable_*
 * settings is priced in lion's own units, which its choices among its own
 * forms - a walk in order or a Sort over one, serial or parallel - were made
 * in.  Against the AM's own scan of a lion index both prices are lion's
 * model's and share its errors: the margin, a hedge against those errors in
 * lion's price alone, would only tilt a choice between two of lion's plans.
 */
static double
lion_competitor_margin(LionCompetitor kind, bool ownscan)
{
	if (kind == LION_COMPETITOR_NONE || kind == LION_COMPETITOR_DISABLED ||
		ownscan)
		return 1.0;
	return lion_units_margin();
}

/*
 * The competitor of the relation a planner hook is adding lion's paths to,
 * found before the first of them is added (lion_units_pin()).  add_path()
 * frees the paths a new one dominates, so the core path the first lion path
 * was priced against may be gone by the time the next one is priced - the
 * decoded walk after the nested loop of the same GROUP BY (§34), the parallel
 * FK-side join after the serial one, the second way round of a join - and
 * the next one would be priced against the cheapest path core has LEFT, of
 * another kind or none: two of lion's own paths in two units, compared with
 * each other.
 */
static RelOptInfo *lion_units_pinned_rel = NULL;
static LionCompetitor lion_units_pinned_kind;
static Cost lion_units_pinned_cost;
static bool lion_units_pinned_ownscan;
static double lion_units_pinned_rate;
static Cost lion_units_pinned_nlcold;
static Cost lion_units_pinned_coldpage;
static Cost lion_units_pinned_nlextra;
static Cost lion_units_pinned_nlouter;
static double lion_units_pinned_skew;
static double lion_units_pinned_memoscale;
static Cost lion_units_pinned_bmcold;
static Cost lion_units_pinned_bmcoldpage;

/*
 * The units a lion path of rel is to be priced in - the cheapest core path's
 * kind and its rate, the reference when there is none - and the margin it is
 * offered at (lion_competitor_margin()).
 */
void
lion_units_for(PlannerInfo *root, RelOptInfo *rel, LionUnits *u)
{
	Cost		cost;
	bool		ownscan;

	if (lion_units_pinned_rel != NULL && rel == lion_units_pinned_rel)
	{
		u->kind = lion_units_pinned_kind;
		cost = lion_units_pinned_cost;
		ownscan = lion_units_pinned_ownscan;
		u->rate = lion_units_pinned_rate;
		u->nlcold = lion_units_pinned_nlcold;
		u->coldpage = lion_units_pinned_coldpage;
		u->nlextra = lion_units_pinned_nlextra;
		u->nlouter = lion_units_pinned_nlouter;
		u->skew = lion_units_pinned_skew;
		u->memoscale = lion_units_pinned_memoscale;
		u->bmcold = lion_units_pinned_bmcold;
		u->bmcoldpage = lion_units_pinned_bmcoldpage;
	}
	else
	{
		u->kind = lion_competitor_of(root, rel, &cost, &ownscan, &u->rate,
									 &u->nlcold, &u->coldpage, &u->nlextra,
									 &u->nlouter, &u->skew, &u->memoscale);
		lion_competitor_bitmap_cold(root, lion_competitor_path(rel), u->kind,
									u->nlcold, &u->bmcold, &u->bmcoldpage);
	}
	if (u->rate <= 0.0)
		u->rate = 1.0;
	if (u->memoscale <= 0.0 || u->memoscale > 1.0)
		u->memoscale = 1.0;
	u->skewscale = 1.0;
	u->margin = lion_competitor_margin(u->kind, ownscan);
	u->nlcost = cost;
	elog(DEBUG2, "lion: priced against a %s%s, cost %.2f, at rate %g and margin %g",
		 ownscan ? "lion " : "", lion_competitor_names[u->kind], cost,
		 u->rate, u->margin);
}

/*
 * Pin rel's competitor for the planner hook adding lion's paths to it, before
 * it adds the first (see above); lion_units_unpin() at the hook's end, and at
 * every hook's start, so that a pin an error left behind is never read.
 */
void
lion_units_pin(PlannerInfo *root, RelOptInfo *rel)
{
	lion_units_pinned_rel = NULL;
	lion_units_pinned_kind = lion_competitor_of(root, rel,
												&lion_units_pinned_cost,
												&lion_units_pinned_ownscan,
												&lion_units_pinned_rate,
												&lion_units_pinned_nlcold,
												&lion_units_pinned_coldpage,
												&lion_units_pinned_nlextra,
												&lion_units_pinned_nlouter,
												&lion_units_pinned_skew,
												&lion_units_pinned_memoscale);
	lion_competitor_bitmap_cold(root, lion_competitor_path(rel),
								lion_units_pinned_kind,
								lion_units_pinned_nlcold,
								&lion_units_pinned_bmcold,
								&lion_units_pinned_bmcoldpage);
	lion_units_pinned_rel = rel;
}

void
lion_units_unpin(void)
{
	lion_units_pinned_rel = NULL;
}

/*
 * Price the paths of rel in the units pinned for another rel: a grouping
 * sets query's finest grouping, counted into a rel of its own whose paths are
 * the input of the grouped rel's Agg (DESIGN.md §10, "Grouping sets"), meets
 * the grouped rel's competitor.  Returns the rel the pin was for, which
 * lion_units_pin_move() of it puts back.
 */
RelOptInfo *
lion_units_pin_move(RelOptInfo *rel)
{
	RelOptInfo *was = lion_units_pinned_rel;

	if (was != NULL)
		lion_units_pinned_rel = rel;
	return was;
}

/*
 * THE MARGIN (DESIGN.md §39): the share of the best core plan's cost a lion
 * path's own price must come to, pg_lion.pushdown_margin.  The price is
 * divided by it rather than the path declined: a join rel gets paths of
 * core's after set_join_pathlist_hook has run, and a path whose worth is its
 * order or its first rows - LionOrdered under a LIMIT, the count's sorted
 * groups - meets its competitor above the relation, in a Sort or a LIMIT's
 * fraction of a path, which a decision made when it is added cannot see.
 * Divided, the price carries the margin into every comparison it is in, and
 * core's enable_* settings and lion's own switches force a plan as they did:
 * a disabled path loses to it whatever its cost.
 */
double
lion_units_margin(void)
{
	return (lion_pushdown_margin > 0.0) ? lion_pushdown_margin : 1.0;
}

/*
 * ... for a LionOrdered path of the base rel rel, which has no rate (its
 * competitors are the reference): the margin, or none when rel's scans are
 * all disabled.  Not the AM's scans' exemption: the cheapest scan of the
 * relation is rarely what a LionOrdered path is chosen over - that is an
 * ordered btree scan of core's, or a Sort, under a LIMIT - and a lion bitmap
 * scan of its filter often is.
 */
double
lion_units_margin_for(RelOptInfo *rel)
{
	Cost		cost;
	bool		ownscan;
	double		rate;
	Cost		nlcold;
	Cost		coldpage;
	Cost		nlextra;
	Cost		nlouter;
	double		skew;
	double		memoscale;
	LionCompetitor kind = lion_competitor_of(NULL, rel, &cost, &ownscan,
											 &rate, &nlcold, &coldpage,
											 &nlextra, &nlouter, &skew,
											 &memoscale);

	return lion_competitor_margin(kind, false);
}

/*
 * A lion path's own price, in lion's units, converted into the competitor's
 * and offered at the margin; priced at the rows its probes of a skewed key
 * meet, put back into core's units (lion_units_take_skew()); and, against a
 * loop over a Memoize whose LIMIT meets fewer hits than core priced, put into
 * core's units of that loop (memoscale, lion_memoize_limit_scale()): the two
 * then compare as they would with the loop at its price with those hits, and
 * the plan around them stays in core's units.  The memoscale is not bounded
 * by pg_lion.cold_max_discount, which bounds only the cold reads on top
 * (lion_units_cold()): it is the competitor's own price, corrected.
 */
Cost
lion_units_price(const LionUnits *u, Cost own)
{
	return own * u->rate * u->skewscale * u->memoscale / u->margin;
}

/*
 * A SKEWED KEY'S PROBES (DESIGN.md §39): the factor over core's rows per key
 * that a nested loop competitor's probes of a skewed key meet
 * (lion_competitor_skew()), which the path is to be priced at - its lookups
 * of the same keys meeting the same rows - and from now on its price is
 * scaled back into core's units by the loop's price at core's rows over its
 * price at those: the loop without its outer side, which the lion path takes
 * as its child at core's price, nlcost - nlouter, over that and nlextra.  The
 * two then compare as they would at the rows both meet, and the plan around
 * them stays in core's units.  The scale is not bounded: it is the
 * competitor's own price, corrected, and pg_lion.cold_max_discount
 * (lion_cold_cap()) bounds only the cold reads on top of it
 * (lion_units_cold()).  A path that does not price its lookups at the factor
 * does not call this, and keeps core's rows on both sides.  1 where the
 * competitor meets no skewed key.
 */
double
lion_units_take_skew(LionUnits *u)
{
	Cost		loop = u->nlcost - u->nlouter;

	if (u->skew <= 1.0 || u->nlextra <= 0.0 || loop <= 0.0)
		return 1.0;
	u->skewscale = loop / (loop + u->nlextra);
	elog(DEBUG2, "lion: the competitor's probes of a skewed key meet %g times its rows, %.2f more over its %.2f",
		 u->skew, u->nlextra, loop);
	return u->skew;
}
