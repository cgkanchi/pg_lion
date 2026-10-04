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
 * The scans of a base relation have rates too - a plain index scan, an
 * index-only scan, a bitmap heap scan of one index - which only the row
 * gather (LionStoreScan) meets: lion's other paths of a base relation are
 * LionOrdered, not converted, and the count's and the joins' competitors are
 * aggregates and joins over those scans (DESIGN.md §40, "As built: the row
 * gather, priced").
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

#include "lion_customscan.h"

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
	[LION_COMPETITOR_BITMAP_HEAP] = "bitmap heap scan of one index",
	[LION_COMPETITOR_OTHER] = "other",
};

/*
 * Is cp one of lion's own custom paths?  By the name custom scans are known
 * by (RegisterCustomScanMethods()): LionCount, LionSemiJoin, LionAntiJoin,
 * LionJoinAgg, LionOrdered and LionStoreScan.
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
		 strcmp(name, "LionOrdered") == 0 ||
		 strcmp(name, "LionStoreScan") == 0);
}

/* Is cp LionStoreScan, the row gather - a scan of lion's, not a plan? */
static bool
lion_custom_is_store_scan(const CustomPath *cp)
{
	return cp->methods != NULL && cp->methods->CustomName != NULL &&
		strcmp(cp->methods->CustomName, "LionStoreScan") == 0;
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
static bool lion_path_holds_lion(Path *path, bool storescan);

bool
lion_path_has_lion(Path *path)
{
	return lion_path_holds_lion(path, true);
}

/*
 * lion_path_has_lion(), with LionStoreScan counted as lion's or not as
 * storescan says.
 */
static bool
lion_path_holds_lion(Path *path, bool storescan)
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

				if (lion_custom_is_lion(cp) &&
					(storescan || !lion_custom_is_store_scan(cp)))
					return true;
				foreach(lc, cp->custom_paths)
					if (lion_path_holds_lion((Path *) lfirst(lc), storescan))
						return true;
				return false;
			}
		case T_AppendPath:
			foreach(lc, ((AppendPath *) path)->subpaths)
				if (lion_path_holds_lion((Path *) lfirst(lc), storescan))
					return true;
			return false;
		case T_MergeAppendPath:
			foreach(lc, ((MergeAppendPath *) path)->subpaths)
				if (lion_path_holds_lion((Path *) lfirst(lc), storescan))
					return true;
			return false;
		case T_NestPath:
		case T_MergePath:
		case T_HashPath:
			return lion_path_holds_lion(((JoinPath *) path)->outerjoinpath, storescan) ||
				lion_path_holds_lion(((JoinPath *) path)->innerjoinpath, storescan);
		case T_ForeignPath:
			return lion_path_holds_lion(((ForeignPath *) path)->fdw_outerpath, storescan);
		default:
			return lion_path_holds_lion(lion_path_input(path), storescan);
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
 *	- a bitmap heap scan of one index, unaggregated, is a BITMAP HEAP scan
 *	  of one index; any other, aggregated by a plain or sorted aggregate or
 *	  not, a BITMAP heap scan;
 *	- a plain or sorted aggregate over a sequential, index-only or index scan
 *	  is an AGGREGATE over a scan, and the scan alone its own kind;
 *	- a nested loop over anything else is OTHER, and so is anything else
 *	  but under a hashed aggregate;
 *	- an Append is the kind of its dearest child, which most of its time is;
 *	- LionStoreScan, a scan priced in core's units, is a scan under core's
 *	  aggregate (lion_competitor_path()): HASHAGG or AGG over it.
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
				if (hashed)
					return LION_COMPETITOR_HASHAGG;
				if (!aggregated &&
					IsA(((BitmapHeapPath *) path)->bitmapqual, IndexPath))
					return LION_COMPETITOR_BITMAP_HEAP;
				return LION_COMPETITOR_BITMAP;
			case T_IndexPath:
				if (hashed)
					return LION_COMPETITOR_HASHAGG;
				if (aggregated)
					return LION_COMPETITOR_AGG;
				return (path->pathtype == T_IndexOnlyScan) ?
					LION_COMPETITOR_INDEXONLY : LION_COMPETITOR_INDEX;
			case T_CustomPath:
				/* the gather, under core's aggregate (see above) */
				if (!lion_custom_is_store_scan((CustomPath *) path))
					break;
				if (hashed)
					return LION_COMPETITOR_HASHAGG;
				return aggregated ? LION_COMPETITOR_AGG :
					LION_COMPETITOR_OTHER;
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
		case LION_COMPETITOR_BITMAP_HEAP:
			return lion_bitmap_heap_rate;
		case LION_COMPETITOR_INDEX:
			return lion_indexscan_rate;
		case LION_COMPETITOR_INDEXONLY:
			return lion_indexonly_rate;
		case LION_COMPETITOR_NONE:
		case LION_COMPETITOR_DISABLED:
		case LION_COMPETITOR_SEQSCAN:
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
 * Is path rel's LionStoreScan, under nodes that pass its rows up but neither
 * aggregate nor join them?
 */
static bool
lion_path_is_store_scan(Path *path)
{
	while (path != NULL)
	{
		switch (nodeTag(path))
		{
			case T_CustomPath:
				return lion_custom_is_store_scan((CustomPath *) path);
			case T_AggPath:
			case T_GroupPath:
			case T_GroupingSetsPath:
#if PG_VERSION_NUM >= 190000
			case T_UniquePath:
#else
			case T_UpperUniquePath:
			case T_UniquePath:
#endif
			case T_WindowAggPath:
				return false;
			default:
				break;
		}
		path = lion_path_input(path);
	}
	return false;
}

/*
 * The cheapest of core's paths in rel so far: unparameterized, and with no
 * lion path inside (lion_path_has_lion()) - but for LionStoreScan under an
 * aggregate or a join, and not rel's scan itself (lion_path_is_store_scan()).
 * The gather is a scan priced in the units of the scan of core's it
 * displaced, the plan of core's over it is core's, and core builds its
 * GROUP BY and joins over the cheapest scan only: with the gather cheapest,
 * the upper rel has no other plan to price LionCount against, and the
 * reference rate core's hashed aggregate was a third of priced it at three
 * times its own (DESIGN.md §40, "As built: the row gather, priced").
 * NULL when there is none.  The
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
		if (lion_path_is_store_scan(p) || lion_path_holds_lion(p, false))
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
static bool
lion_path_disabled(const Path *p)
{
#if PG_VERSION_NUM >= 180000
	return p->disabled_nodes > 0;
#else
	return p->total_cost >= disable_cost;
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
 * or bitmap scan of one, LionStoreScan, or an Append or MergeAppend of such
 * scans of a partitioned table's partitions, under the nodes that pass its
 * rows up and no join?  Its price is lion's own model's (lioncostestimate(),
 * the gather's own), with lion's errors.
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
			case T_CustomPath:
				return lion_custom_is_store_scan((CustomPath *) path);
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
 * What a lion path of rel competes with: the kind of the cheapest core path
 * (lion_competitor_path()) and its cost; NONE when core has no path there yet,
 * and DISABLED when every one it has is disabled - a plan forced by core's
 * enable_* settings, which leave a lion path nothing to compete with.
 * *ownscan says whether that path is one of the AM's own scans of a lion
 * index (lion_path_is_lion_scan()).
 */
static LionCompetitor
lion_competitor_of(RelOptInfo *rel, Cost *cost, bool *ownscan)
{
	Path	   *best = lion_competitor_path(rel);

	*cost = (best != NULL) ? best->total_cost : 0.0;
	*ownscan = false;
	if (best == NULL)
		return LION_COMPETITOR_NONE;
	if (lion_path_disabled(best))
		return LION_COMPETITOR_DISABLED;
	*ownscan = lion_path_is_lion_scan(best);
	return lion_competitor_kind(best);
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

/*
 * The units a lion path of rel is to be priced in - the cheapest core path's
 * kind and its rate, the reference when there is none - and the margin it is
 * offered at (lion_competitor_margin()).
 */
void
lion_units_for(RelOptInfo *rel, LionUnits *u)
{
	Cost		cost;
	bool		ownscan;

	if (rel != NULL && rel == lion_units_pinned_rel)
	{
		u->kind = lion_units_pinned_kind;
		cost = lion_units_pinned_cost;
		ownscan = lion_units_pinned_ownscan;
	}
	else
		u->kind = lion_competitor_of(rel, &cost, &ownscan);
	u->rate = lion_competitor_rate(u->kind);
	if (u->rate <= 0.0)
		u->rate = 1.0;
	u->margin = lion_competitor_margin(u->kind, ownscan);
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
lion_units_pin(RelOptInfo *rel)
{
	lion_units_pinned_rel = NULL;
	lion_units_pinned_kind = lion_competitor_of(rel, &lion_units_pinned_cost,
												&lion_units_pinned_ownscan);
	lion_units_pinned_rel = rel;
}

void
lion_units_unpin(void)
{
	lion_units_pinned_rel = NULL;
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
	LionCompetitor kind = lion_competitor_of(rel, &cost, &ownscan);

	return lion_competitor_margin(kind, false);
}

/*
 * A lion path's own price, in lion's units, converted into the competitor's
 * and offered at the margin.
 */
Cost
lion_units_price(const LionUnits *u, Cost own)
{
	return own * u->rate / u->margin;
}
