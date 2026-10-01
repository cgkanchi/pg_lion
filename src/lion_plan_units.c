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
 * the cheapest core path already in its relation: the kind of plan that is
 * (lion_competitor_kind()) has a rate, pg_lion.<kind>_rate, the multiple of
 * the reference its plans run at, and lion's CPU terms are multiplied by it.
 * Its pages stay in core's convention, which lion shares (§10): a page is
 * charged as core charges the same page, whoever reads it.
 *
 * The terms are not classified one by one.  The price is summed with every
 * page cost it reads divided by the rate (lion_page_scale, which
 * LION_SEQ_PAGE_COST, LION_RANDOM_PAGE_COST and lion_heap_page_cost() apply)
 * and the sum multiplied by the rate: what was CPU comes out times the rate,
 * what was a page as it was, and every choice the model makes between two
 * ways of doing a thing - walk or probe, collect or seek - is made in the
 * competitor's units.
 *
 * Part of the LionCount custom scan: lion_customscan.h describes the node
 * and declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_customscan.h"

/* 1 but while lion_units_begin() .. lion_units_end() sums a price */
double		lion_page_scale = 1.0;

static const char *const lion_competitor_names[] = {
	[LION_COMPETITOR_NONE] = "none",
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
 * Is cp one of lion's own custom paths?  By the name custom scans are known
 * by (RegisterCustomScanMethods()): LionCount, LionSemiJoin, LionAntiJoin,
 * LionJoinAgg and LionOrdered.
 */
static bool
lion_custom_is_lion(const CustomPath *cp)
{
	const char *name = (cp->methods != NULL) ? cp->methods->CustomName : NULL;

	return name != NULL &&
		(strcmp(name, "LionCount") == 0 ||
		 strcmp(name, "LionSemiJoin") == 0 ||
		 strcmp(name, "LionAntiJoin") == 0 ||
		 strcmp(name, "LionJoinAgg") == 0 ||
		 strcmp(name, "LionOrdered") == 0);
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
		case T_UpperUniquePath:
			return ((UpperUniquePath *) path)->subpath;
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
#if PG_VERSION_NUM < 190000
		case T_UniquePath:
			return ((UniquePath *) path)->subpath;
#endif
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
 *	- an aggregate that hashes, anywhere on the way down - a Finalize Agg
 *	  over the Partial HashAggregates of a parallel plan included - is a
 *	  HASHED AGGREGATE: hashing is what core charges far below its time;
 *	- a join is its join method: a HASH JOIN, a MERGE JOIN, or a NESTED LOOP
 *	  into a parameterized index scan (an aggregate over it included; a
 *	  nested loop over anything else is OTHER);
 *	- a bitmap heap scan is a BITMAP heap scan, aggregated or not;
 *	- a plain or sorted aggregate over a sequential, index-only or index scan
 *	  is an AGGREGATE over a scan, and the scan alone its own kind;
 *	- an Append is the kind of its dearest child, which most of its time is.
 */
LionCompetitor
lion_competitor_kind(Path *path)
{
	bool		aggregated = false;

	while (path != NULL)
	{
		switch (nodeTag(path))
		{
			case T_AggPath:
				if (((AggPath *) path)->aggstrategy == AGG_HASHED ||
					((AggPath *) path)->aggstrategy == AGG_MIXED)
					return LION_COMPETITOR_HASHAGG;
				aggregated = true;
				break;
			case T_GroupingSetsPath:
				if (((GroupingSetsPath *) path)->aggstrategy == AGG_HASHED ||
					((GroupingSetsPath *) path)->aggstrategy == AGG_MIXED)
					return LION_COMPETITOR_HASHAGG;
				aggregated = true;
				break;
			case T_GroupPath:
			case T_UpperUniquePath:
				aggregated = true;
				break;
			case T_HashPath:
				return LION_COMPETITOR_HASHJOIN;
			case T_MergePath:
				return LION_COMPETITOR_MERGEJOIN;
			case T_NestPath:
				return lion_nestloop_probes_index((JoinPath *) path) ?
					LION_COMPETITOR_NESTLOOP : LION_COMPETITOR_OTHER;
			case T_BitmapHeapPath:
				return LION_COMPETITOR_BITMAP;
			case T_IndexPath:
				if (aggregated)
					return LION_COMPETITOR_AGG;
				return (path->pathtype == T_IndexOnlyScan) ?
					LION_COMPETITOR_INDEXONLY : LION_COMPETITOR_INDEX;
			case T_Path:
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
						return LION_COMPETITOR_OTHER;
					path = dearest;
					continue;
				}
			default:
				break;
		}
		path = lion_path_input(path);
	}
	return LION_COMPETITOR_OTHER;
}

/*
 * The rate of a kind of core plan: the cost units a millisecond its plans
 * run at, as a multiple of the 500 lion's CPU constants are fitted at.  The
 * scans the constants were fitted against are the reference, 1.
 */
double
lion_competitor_rate(LionCompetitor kind)
{
	switch (kind)
	{
		case LION_COMPETITOR_HASHAGG:
			return lion_hashagg_rate;
		case LION_COMPETITOR_AGG:
			return lion_agg_rate;
		case LION_COMPETITOR_HASHJOIN:
			return lion_hashjoin_rate;
		case LION_COMPETITOR_MERGEJOIN:
			return lion_mergejoin_rate;
		case LION_COMPETITOR_NESTLOOP:
			return lion_nestloop_rate;
		case LION_COMPETITOR_BITMAP:
			return lion_bitmap_rate;
		case LION_COMPETITOR_NONE:
		case LION_COMPETITOR_SEQSCAN:
		case LION_COMPETITOR_INDEXONLY:
		case LION_COMPETITOR_INDEX:
		case LION_COMPETITOR_OTHER:
			break;
	}
	return 1.0;
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
 * The units a lion path of rel is to be priced in: the cheapest core path's
 * kind and its rate, or the reference when rel has no path of core's yet.
 */
void
lion_units_for(RelOptInfo *rel, LionUnits *u)
{
	Path	   *best = lion_competitor_path(rel);

	u->kind = (best != NULL) ? lion_competitor_kind(best) :
		LION_COMPETITOR_NONE;
	u->rate = lion_competitor_rate(u->kind);
	if (u->rate <= 0.0)
		u->rate = 1.0;
	elog(DEBUG2, "lion: priced against a %s, cost %.2f, at rate %g",
		 lion_competitor_names[u->kind],
		 (best != NULL) ? best->total_cost : 0.0, u->rate);
}

/*
 * Sum a price in u's units: until lion_units_end(), every page cost a custom
 * path's price reads is divided by the rate, so that lion_units_price()'s
 * multiplication by it leaves the pages as they were.
 */
void
lion_units_begin(const LionUnits *u)
{
	lion_page_scale = 1.0 / u->rate;
}

void
lion_units_end(void)
{
	lion_page_scale = 1.0;
}

/*
 * A lion path's own price, summed between lion_units_begin() and
 * lion_units_end(), in the competitor's units: the CPU terms times the rate,
 * the pages as core prices them.
 */
Cost
lion_units_price(const LionUnits *u, Cost own)
{
	return own * u->rate;
}
