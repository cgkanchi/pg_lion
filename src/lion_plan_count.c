/*-------------------------------------------------------------------------
 *
 * lion_plan_count.c
 *		Building the LionCount path of an aggregate over one relation
 *		(lion_try_count_path()), and its parallel GROUP BY path.
 *
 * Part of the LionCount custom scan: lion_customscan.h describes the node
 * and declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_customscan.h"
#include "lion_plan_private.h"

/*
 * A PARALLEL GROUP BY (DESIGN.md §10, "A GROUP BY in parallel"): the serial
 * node `serial` made parallel-aware, below a Gather and core's Finalize
 * HashAggregate.  Its participants divide the heap's container keys into
 * ranges, a few each (lion_key_ranges()), and count every group in each range
 * they claim against the WHERE collected for that range: a partial count a
 * group a range, which the Finalize Agg adds up - and applies the HAVING to,
 * which the node's partial target leaves to it.
 *
 * WORKERS: what a parallel scan of the index pages the walk reads - the
 * pages of the grouping column's posting sets - would get, capped by
 * max_parallel_workers_per_gather; a parallel_workers setting on the table
 * decides alone, as for core's scans.
 *
 * COST, one participant's: its share of the serial walk (`serialrun`, the
 * serial node's price before its HAVING) - the collection and the groups'
 * containers divide by key, the pages each range reads being its own - plus
 * what each of its ranges pays again (rc->perrange: the entry walk, each
 * group's set set up and sought to the range, each WHERE source sought
 * there), plus a cpu_tuple_cost a partial row.  The rows are a group's per
 * range it has rows in, at most the WHERE's rows.  The Gather prices its
 * rows and set-up, core's Finalize Agg its own work.
 */
static void
lion_add_parallel_group_path(PlannerInfo *root, RelOptInfo *rel,
							 RelOptInfo *output_rel, CustomPath *serial,
							 Cost serialrun, const LionRangeCost *rc,
							 IndexOptInfo *groupidx, AttrNumber groupcol,
							 List *having, double numgroups)
{
	PathTarget *partialtarget;
	CustomPath *cpath;
	LionCountPriv priv;
	Path	   *gather;
	AggClauseCosts agg_final_costs;
	double		pages;
	double		divisor;
	double		rows;
	double		totalrows;
	uint32		ckeys;
	int			workers;
	int			nranges;

	pages = Max(1.0, (double) groupidx->pages *
				lion_index_column_posting_share(root, rel, groupidx,
												groupcol));
	workers = compute_parallel_worker(rel, -1, pages,
									  max_parallel_workers_per_gather);
	if (workers <= 0)
		return;

	divisor = lion_parallel_divisor(workers);
	nranges = lion_key_ranges(rel->pages, workers + 1,
							  LION_PARALLEL_RANGE_KEYS, &ckeys);
	totalrows = clamp_row_est(Min(numgroups * nranges,
								  Max(rel->rows, numgroups)));
	rows = clamp_row_est(totalrows / divisor);

	partialtarget = lion_make_partial_target(root, output_rel->reltarget,
											 having);

	cpath = makeNode(CustomPath);
	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = output_rel;
	cpath->path.pathtarget = partialtarget;
	cpath->path.param_info = NULL;
	cpath->path.parallel_aware = true;
	cpath->path.parallel_safe = true;
	cpath->path.parallel_workers = workers;
	cpath->path.pathkeys = NIL;
	cpath->path.rows = rows;
	cpath->path.startup_cost = 0;
	cpath->path.total_cost = serialrun / divisor +
		((double) nranges / divisor) * rc->perrange + rows * cpu_tuple_cost;
#if PG_VERSION_NUM >= 180000
	cpath->path.disabled_nodes = 0;
#endif
	cpath->flags = serial->flags;
	cpath->custom_paths = NIL;
#if PG_VERSION_NUM >= 170000
	cpath->custom_restrictinfo = NIL;
#endif
	/* the serial node's plan, but for the HAVING, which is the Agg's here */
	lion_count_priv_decode(serial->custom_private, LION_PRIV_STAGE_PATH,
						   &priv);
	priv.having = NIL;
	cpath->custom_private = lion_count_priv_encode(&priv,
												   LION_PRIV_STAGE_PATH);
	cpath->methods = serial->methods;
	lion_path_core_switches(&cpath->path, LION_CORE_INDEXONLY);

	gather = (Path *) create_gather_path(root, output_rel, &cpath->path,
										 partialtarget, NULL, &totalrows);

	MemSet(&agg_final_costs, 0, sizeof(agg_final_costs));
	get_agg_clause_costs(root, AGGSPLIT_FINAL_DESERIAL, &agg_final_costs);
	add_path(output_rel, (Path *)
			 create_agg_path(root, output_rel, gather,
							 output_rel->reltarget,
							 AGG_HASHED, AGGSPLIT_FINAL_DESERIAL,
							 root->processed_groupClause,
							 having,
							 &agg_final_costs,
							 numgroups));
}

/*
 * The child of a parallel count's anti join over a small inner side
 * (DESIGN.md §27): the inner side's cheapest path a worker may run, which
 * the serial node's - a Gather, say - need not be.  Unparameterized, and not
 * a partial one: the participant that subtracts runs it whole.  NULL when
 * the inner side has none.
 */
static Path *
lion_anti_parallel_child(RelOptInfo *innerrel)
{
	Path	   *best = NULL;
	ListCell   *lc;

	foreach(lc, innerrel->pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);

		if (!path->parallel_safe || path->parallel_aware ||
			path->param_info != NULL)
			continue;
		if (best == NULL || compare_path_costs(path, best, TOTAL_COST) < 0)
			best = path;
	}
	return best;
}

/*
 * A PARALLEL COUNT (DESIGN.md §10, "A count in parallel"): the node of one
 * row - the count of a WHERE, or a sum over a column's entries, with any
 * filtered counts beside it - made parallel-aware, its partial counts under a
 * Gather and core's Finalize Aggregate, which adds them up and applies the
 * HAVING the partial target leaves to it.  Its participants claim the
 * heap's container keys a range at a time, as a parallel GROUP BY's do, and
 * count each claimed range of each run whole (lion_count_ranged()).
 *
 * WORKERS: what a parallel scan of the pages the count reads - its sets, its
 * walk and the heap, as the serial price has them (rc->pages) - would get,
 * capped by max_parallel_workers_per_gather; a parallel_workers setting on
 * the table decides alone, as for core's scans.  A heap the executor would
 * cut into one range gets no parallel count: one participant would count it
 * all, after the others had started for nothing.
 *
 * COST, one participant's: its share of the serial count (`serialrun`, the
 * serial node's price before its HAVING) but for what every participant pays
 * whole (rc->whole: the lookups that locate the WHERE, and the ranges taken
 * as sources it collects), that whole, and what each range it counts pays
 * again (rc->countrange, every run's: each set sought to the range, a walk's
 * entries counted again), plus a cpu_tuple_cost for its one row.  The Gather
 * prices its rows and set-up, core's Finalize Agg its own work.
 *
 * The anti join over a small inner side (DESIGN.md §27) is subtracted by one
 * participant: its child and `once` - its lookups and the child's price - are
 * the partial node's, undivided.
 */
static void
lion_add_parallel_count_path(PlannerInfo *root, RelOptInfo *rel,
							 RelOptInfo *output_rel, CustomPath *serial,
							 Cost serialrun, const LionRangeCost *rc,
							 List *having, Path *child, Cost once)
{
	PathTarget *partialtarget;
	CustomPath *cpath;
	LionCountPriv priv;
	Path	   *gather;
	AggClauseCosts agg_final_costs;
	double		divisor;
	double		totalrows;
	Cost		share;
	uint32		ckeys;
	int			workers;
	int			nranges;
	int			cut;

	workers = compute_parallel_worker(rel, -1, Max(rc->pages, 1.0),
									  max_parallel_workers_per_gather);
	if (workers <= 0)
		return;

	/*
	 * A range is counted by one participant, so the count divides among no
	 * more participants than the executor will cut ranges - with
	 * pg_lion.parallel_range_keys as it is, the one thing here that does not
	 * price the default - and a heap of one range not at all.
	 */
	cut = lion_key_ranges(rel->pages, workers + 1, lion_parallel_range_keys,
						  &ckeys);
	if (cut < 2)
		return;
	divisor = Min(lion_parallel_divisor(workers), (double) cut);
	nranges = lion_key_ranges(rel->pages, workers + 1,
							  LION_PARALLEL_RANGE_KEYS, &ckeys);
	share = Max(serialrun - rc->whole, 0.0);
	totalrows = (double) (workers + 1);

	partialtarget = lion_make_partial_target(root, output_rel->reltarget,
											 having);

	cpath = makeNode(CustomPath);
	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = output_rel;
	cpath->path.pathtarget = partialtarget;
	cpath->path.param_info = NULL;
	cpath->path.parallel_aware = true;
	cpath->path.parallel_safe = true;
	cpath->path.parallel_workers = workers;
	cpath->path.pathkeys = NIL;
	cpath->path.rows = 1;
	cpath->path.total_cost = share / divisor + rc->whole +
		((double) nranges / divisor) * rc->countrange + cpu_tuple_cost +
		once;
	cpath->path.startup_cost = cpath->path.total_cost;
#if PG_VERSION_NUM >= 180000
	cpath->path.disabled_nodes = (child != NULL) ? child->disabled_nodes : 0;
#endif
	cpath->flags = serial->flags;
	cpath->custom_paths = (child != NULL) ? list_make1(child) : NIL;
#if PG_VERSION_NUM >= 170000
	cpath->custom_restrictinfo = NIL;
#endif
	/* the serial node's plan, but for the HAVING, which is the Agg's here */
	lion_count_priv_decode(serial->custom_private, LION_PRIV_STAGE_PATH,
						   &priv);
	priv.having = NIL;
	cpath->custom_private = lion_count_priv_encode(&priv,
												   LION_PRIV_STAGE_PATH);
	cpath->methods = serial->methods;
	lion_path_core_switches(&cpath->path, LION_CORE_INDEXONLY);

	gather = (Path *) create_gather_path(root, output_rel, &cpath->path,
										 partialtarget, NULL, &totalrows);

	MemSet(&agg_final_costs, 0, sizeof(agg_final_costs));
	get_agg_clause_costs(root, AGGSPLIT_FINAL_DESERIAL, &agg_final_costs);
	add_path(output_rel, (Path *)
			 create_agg_path(root, output_rel, gather,
							 output_rel->reltarget,
							 AGG_PLAIN, AGGSPLIT_FINAL_DESERIAL,
							 NIL, having, &agg_final_costs, 1.0));
}

/*
 * Turn the RANGE clauses of every range column but `keep` (an index into
 * rangevars and rangepos, or -1 for none) into bounds of ranges taken as
 * sources (DESIGN.md §32): LION_CLAUSE_RANGESRC in the clause lists, which is
 * all the executor goes by.
 *
 * Each position is written through, so it has to name a clause of the lists,
 * and a RANGE clause of its own column (lion_count_path_where_leaf()).  One
 * that does not is a position kept from a restriction whose clauses were
 * taken back - the next clause appended is at it, or none is - and an error
 * here, not a write past the lists or an equality made a range's bound.
 */
static void
lion_ranges_to_sources(List *rangevars, List *rangepos, int keep,
					   List *wherekinds, List *clauseinfos)
{
	int			nclause = list_length(clauseinfos);
	int			r = 0;
	ListCell   *l1;
	ListCell   *l2;

	if (list_length(rangevars) != list_length(rangepos) ||
		list_length(wherekinds) != nclause)
		elog(ERROR, "LionCount: the range lists are out of step with the clauses");
	forboth(l1, rangevars, l2, rangepos)
	{
		AttrNumber	attno = ((Var *) lfirst(l1))->varattno;
		ListCell   *lp;

		if (r++ == keep)
			continue;
		foreach(lp, (List *) lfirst(l2))
		{
			int			pos = lfirst_int(lp);
			ListCell   *kind;
			LionClauseInfo *ci;

			if (pos < 0 || pos >= nclause)
				elog(ERROR, "LionCount: a range bound at clause %d of %d",
					 pos, nclause);
			kind = list_nth_cell(wherekinds, pos);
			ci = (LionClauseInfo *) list_nth(clauseinfos, pos);
			if (lfirst_int(kind) != LION_CLAUSE_RANGE ||
				ci->kind != LION_CLAUSE_RANGE || ci->attno != attno)
				elog(ERROR, "LionCount: clause %d is no range bound of column %d",
					 pos, (int) attno);
			lfirst_int(kind) = LION_CLAUSE_RANGESRC;
			ci->kind = LION_CLAUSE_RANGESRC;
		}
	}
}

/*
 * In a build that answers ranges from slices (DESIGN.md §53): can the slices
 * answer a range on column attno whose bounds are compared with values of
 * type cmptype?  The index its clauses are matched to slices the column at
 * its type's width (lion_range_sliced_index()), and the encoding takes the
 * bounds' type (lion_slice_range_type_ok()).
 */
static bool
lion_slice_range_ok(RelOptInfo *rel, AttrNumber attno, Oid cmptype)
{
	AttrNumber	col;
	Oid			typid;

	if (lion_range_sliced_index(rel, attno, &col, &typid) == NULL)
		return false;
	return !OidIsValid(cmptype) || lion_slice_range_type_ok(typid, cmptype);
}

/*
 * What lion_try_count_path() works out in one of its phases and uses in a
 * later one: its arguments - input_rel being the FACT rel of an FK-side join
 * from lion_count_path_query() on - and what each phase finds, every member
 * starting as lion_count_path_init() sets it.
 */
typedef struct LionCountPathBuild
{
	PlannerInfo *root;
	RelOptInfo *input_rel;
	RelOptInfo *output_rel;
	GroupPathExtraData *extra;
	const LionFkJoin *fj;
	const LionSmallAnti *anti;	/* the anti join over a small inner side
								 * (§27), or NULL */
	List	   *antiextra;		/* ... the clauses the count adds to the
								 * WHERE, which its lookups test too */
	LionAntiPlan antiplan;		/* ... its B-tree and the lookups' price */
	Query	   *parse;
	RangeTblEntry *rte;
	Index		rti;
	Var		   *groupvar[LION_MAX_GROUPCOLS];
	AttrNumber	groupattno[LION_MAX_GROUPCOLS];
	Oid			groupeqop[LION_MAX_GROUPCOLS];
	Oid			groupcoll[LION_MAX_GROUPCOLS];	/* the grouping's collation */
	bool		groupvalueout[LION_MAX_GROUPCOLS];
	double		groupest[LION_MAX_GROUPCOLS];
	int			ngroup;
	bool		decode;			/* three or more columns: the decoded walk
								 * of DESIGN.md §34 counts them */
	AttrNumber	driveattno;		/* column whose index drives the entry scan */
	bool		singlegroup;
	bool		sumall;
	bool		partitioned;
	List	   *valueattnos;	/* pinned columns the output prints */
	PathTarget *partialtarget;	/* set for a partitioned GROUP BY */
	List	   *whereattnos;	/* its column, in the PARENT's numbering */
	List	   *clauseinfos;	/* LionClauseInfo, one per clause */
	List	   *whereclauses;	/* the clause, for selectivity */
	List	   *whereconsts;	/* its value expression - a Const, a Param,
								 * or a NULL placeholder for a null test */
	List	   *wherekinds;		/* LION_CLAUSE_* */
	List	   *whereopnos;		/* the clause's operator (0 for a null test) */
	List	   *whereinor;		/* 1 when the clause is a leaf of an OR */
	List	   *ors;			/* one IntList per OR restriction (§19) */
	List	   *rinfoclauses;	/* each restriction's clause, by position */
	List	   *impliedtexts;	/* the ones left out altogether (§16) */
	int			rinfono;		/* ... and the one being analysed */
	LionImply	imply;			/* what a partition's bounds may leave out */
	List	   *posattnos;		/* columns with a positive clause */
	List	   *eqattnos;		/* columns pinned to one value */
	List	   *nonnullattnos;	/* columns a clause proves non-null */
	List	   *nullattnos;		/* columns a clause pins to NULL */
	List	   *targets;		/* LionCountTarget, one per counted relation */
	LionCountTarget *first;
	Var		   *notnullvar;		/* the first `IS NOT NULL` column */
	Var		   *allvar;			/* a sum over all of a column's entries
								 * drives by it (§14, §35) */
	Var		   *rangevar;		/* the column the RANGE clauses bound (§28) */
	List	   *rangeclauses;	/* ... and those clauses' RestrictInfos */
	List	   *rangevars;		/* every column a range bounds (§28, §32) */
	List	   *rangecls;		/* ... its clauses' RestrictInfos, a List
								 * per column */
	List	   *rangepos;		/* ... and their positions in the clause
								 * lists, an IntList per column */
	bool		hasrangesrc;	/* a range is a source (§32) */
	bool		pinnedsrc;		/* a source outside ranges that holds the
								 * §9 pin: a clause, or an OR of them */
	Selectivity rangesel;		/* the share of its entries they select */
	LionCountPriv priv;			/* the path's custom_private */
	double		numgroups;
	double		outrows;
	bool		havepositive;
	List	   *having;
	List	   *checkexprs;
	Var		   *distvar;		/* the column count(DISTINCT) counts (§26) */
	Oid			disteqop;
	Oid			distcoll;
	bool		distcounts;		/* its walk must count, not test (§26) */
	double		distest;
	Const	   *groupcoal;		/* GROUP BY coalesce(col, c): c (§10) */
	Node	   *groupexpr;		/* ... and the whole expression */
	bool		plainpositive;	/* a positive clause outside ORs that
								 * is positive whatever its value: a
								 * WHERE a parallel GROUP BY can
								 * collect by ranges (§10) */
	List	   *groupdeps;		/* GROUP BY expressions of the columns
								 * (§36), which split no group */
	List	   *wattnos;		/* §37: the columns aggregates are taken
								 * over, weighted by their entries' rows */
	List	   *widx;			/* ... their indexes (IndexOptInfo) */
	List	   *wcols;			/* ... and key columns there */
	List	   *wnaggs;			/* ... and how many aggregates each */
	bool		wcounts;		/* the counts of the target list besides */
	bool		slicemode;		/* §52: this build answers aggregates of
								 * sliced columns from their slices */
	bool		slicedecode;	/* ... a GROUP BY's a batch of groups at a
								 * time (§52, "The decode route") */
	bool		slicerange;		/* §53: this build answers the ranges on
								 * sliced columns that do not bound the walk
								 * from their slices */
	bool		slicedsrc;		/* ... and has one outside the ORs, which
								 * carries the §9 pin with its E */
	int			nsliced;		/* ... how many ranges it so answers */
	List	   *sattnos;		/* ... the columns they are taken over */
	List	   *sidx;			/* ... the indexes whose slices they read
								 * (IndexOptInfo) */
	List	   *scols;			/* ... key columns there */
	List	   *stypes;			/* ... the types they are sliced as */
	List	   *swants;			/* ... what each is asked: LION_SWANT_* */
	int64		topkn;			/* the top k by count (§36), or 0 */
	int			topkcand;		/* ... the candidates its walk keeps */
	double		topkrows;		/* ... and the rows of their entries */
	bool		topkstrict;		/* ... every group tied with the k-th */
	List	   *vcols;			/* the expression columns (DESIGN.md §41) */
	bool		distinctstage;	/* a SELECT DISTINCT's keys as the groups */
	int			ncompl;			/* ORs with a complement arm (§35) */
	bool		runtimemulti;	/* a multi-key clause outside the ORs whose
								 * query is known only at run time (§17) */
	bool		prefixmulti;	/* ... or a literal one with prefixes the
								 * run may find too common to expand: a
								 * tsquery's `foo:*`, a jsonpath (§17, §43) */
	bool		newnegated;		/* an `IS DISTINCT FROM` or `NOT (multi-key)`
								 * clause outside the ORs (§35) */
	bool		exprnegated;	/* a negated clause on an expression
								 * column outside the ORs (§41) */
	List	   *residual;		/* clauses no posting set answers, tested on
								 * the heap (§10, "Residual clauses") */
	Bitmapset  *residable;		/* the restrictions, by position, that could
								 * be residual (§16, "Residual clauses over a
								 * partitioned table") ... */
	Bitmapset  *forceresid;		/* ... and the ones this attempt makes so,
								 * asked for by the one before */
	Bitmapset  *keyattnos;		/* the partitioning keys' columns, at every
								 * level the planner keeps */
	bool		keyattnosdone;
	int			nfilter;		/* filtered counts (§10): their conditions */
	List	   *filterexprs;	/* ... each one once, by equal() */
	List	   *filtercols;		/* ... an IntList {filter, attno} for each
								 * count(col) whose col has to be proved
								 * non-NULL in its rows */
	int			curfilter;		/* the filter being analysed, or 0 */
	int			nwhere;			/* the WHERE's clauses, before the filters' */
	bool		filterdecode;	/* every filter's run may be decoded (§34) */
	bool		cells;			/* a count by cells (§10, "Counts by
								 * cells") is being built */
	List	   *cellexprs;		/* ... its GROUP BY's expressions */
	List	   *cellquals;		/* ... the WHERE's clauses over predicates */
	LionCellsPlan *cellplan;	/* ... its dimensions and driver */
	double		cellwhererows;	/* ... and the rows the posting sets select */
} LionCountPathBuild;

static void lion_try_count_path_scoped(LionCountPathBuild *cxp);
static RelOptInfo *lion_count_first_leaf(PlannerInfo *root, RelOptInfo *rel,
										 AttrNumber *attnos, int natts);
static bool lion_count_slice_range_ok(const LionCountPathBuild *cx,
									  AttrNumber attno, Oid cmptype);

/* Does a GROUP BY or a count(DISTINCT) of the build walk column attno? */
static bool
lion_count_path_walks_column(const LionCountPathBuild *cx, AttrNumber attno)
{
	int			g;

	for (g = 0; g < cx->ngroup; g++)
		if (cx->groupattno[g] == attno)
			return true;
	return cx->distvar != NULL && cx->distvar->varattno == attno;
}
static void lion_try_count_cells(const LionCountPathBuild *cxp);
static int	lion_count_path_filter_no(LionCountPathBuild *cx, Node *fexpr);

/*
 * Start the build of lion_try_count_path(): its arguments, and every other
 * member at its starting value - zero, NIL or NULL unless set here.
 */
static void
lion_count_path_init(LionCountPathBuild *cx, PlannerInfo *root,
					 RelOptInfo *input_rel, RelOptInfo *output_rel,
					 GroupPathExtraData *extra, const LionFkJoin *fj)
{
	memset(cx, 0, sizeof(LionCountPathBuild));
	cx->root = root;
	cx->input_rel = input_rel;
	cx->output_rel = output_rel;
	cx->extra = extra;
	cx->fj = fj;
	cx->parse = root->parse;
	cx->groupvar[0] = NULL;
	cx->groupvar[1] = NULL;
	cx->groupattno[0] = 0;
	cx->groupattno[1] = 0;
	cx->groupeqop[0] = InvalidOid;
	cx->groupeqop[1] = InvalidOid;
	cx->groupcoll[0] = InvalidOid;
	cx->groupcoll[1] = InvalidOid;
	cx->groupvalueout[0] = false;
	cx->groupvalueout[1] = false;
	cx->groupest[0] = 1.0;
	cx->groupest[1] = 1.0;
	cx->ngroup = 0;
	cx->driveattno = 0;
	cx->singlegroup = false;
	cx->sumall = false;
	cx->partitioned = false;
	cx->valueattnos = NIL;
	cx->partialtarget = NULL;
	cx->whereattnos = NIL;
	cx->clauseinfos = NIL;
	cx->whereclauses = NIL;
	cx->whereconsts = NIL;
	cx->wherekinds = NIL;
	cx->whereopnos = NIL;
	cx->whereinor = NIL;
	cx->ors = NIL;
	cx->rinfoclauses = NIL;
	cx->impliedtexts = NIL;
	cx->rinfono = -1;
	cx->posattnos = NIL;
	cx->eqattnos = NIL;
	cx->nonnullattnos = NIL;
	cx->nullattnos = NIL;
	cx->targets = NIL;
	cx->notnullvar = NULL;
	cx->allvar = NULL;
	cx->rangevar = NULL;
	cx->rangeclauses = NIL;
	cx->rangevars = NIL;
	cx->rangecls = NIL;
	cx->rangepos = NIL;
	cx->hasrangesrc = false;
	cx->pinnedsrc = false;
	cx->rangesel = 1.0;
	cx->havepositive = false;
	cx->distvar = NULL;
	cx->disteqop = InvalidOid;
	cx->distcoll = InvalidOid;
	cx->distcounts = false;
	cx->distest = 0;
	cx->groupcoal = NULL;
	cx->groupexpr = NULL;
	cx->plainpositive = false;
	cx->vcols = NIL;
}

/*
 * The query as a whole: a SELECT the node may replace.  Sets the HAVING the
 * node is to apply, and - for the FK-side join - makes input_rel the fact
 * rel, which everything after this asks about.
 */
static bool
lion_count_path_query(LionCountPathBuild *cx)
{
	PlannerInfo *root = cx->root;
	Query	   *parse = cx->parse;
	GroupPathExtraData *extra = cx->extra;
	const LionFkJoin *fj = cx->fj;

	/*
	 * ---- the query as a whole ----
	 *
	 * A semi or anti join path (fj->joinpath, DESIGN.md §27, "The semi and
	 * anti join as a join path") is a join rel's rows, whatever the query
	 * does with them above - but in a SELECT only, and with no row marks:
	 * a row the statement locks or modifies would have its EvalPlanQual
	 * recheck made through a join the node replaces, which it does not do.
	 */
	if (parse->commandType != CMD_SELECT)
		return false;
	if (parse->rowMarks != NIL || root->rowMarks != NIL)
		return false;
	if (cx->distinctstage)
	{
		/* lion_try_distinct_path() has checked the query */
	}
	else if (fj == NULL || !fj->joinpath)
	{
		/*
		 * A GROUP BY with no aggregate at all is the groups' keys, each once:
		 * the groups the walk emits, which are only those with a visible row
		 * (DESIGN.md §10, "Shapes rewritten to the node's counts").
		 */
		if (!parse->hasAggs && parse->groupClause == NIL)
			return false;
		if (parse->groupingSets != NIL)
			return false;
		/*
		 * A window function is computed above the grouping, over the rows of
		 * the grouped rel, whose target (make_window_input_target()) holds
		 * every grouping column and aggregate the windows read - their
		 * arguments, PARTITION BY and ORDER BY keys - and is checked below
		 * like any other.  So a window over grouped counts needs nothing of
		 * the node beyond those (DESIGN.md §10, "Shapes rewritten to the
		 * node's counts").
		 */
		if (parse->hasTargetSRFs ||
			parse->hasDistinctOn || parse->distinctClause != NIL)
			return false;
	}

	/*
	 * HAVING (DESIGN.md §10).  By now it is an implicit-AND list of the
	 * clauses that mention an aggregate (or are volatile, or contain a
	 * subquery): the planner has already moved every other clause into WHERE
	 * (subquery_planner()).  The node knows each group's count before it
	 * emits the group, so a HAVING over the counts it computes and the
	 * columns it can print is a filter on its output - checked below against
	 * the same rules as the target list, and applied by the node itself or,
	 * for a partitioned table, by the Finalize Agg above it.
	 */
	cx->having = (extra != NULL) ? (List *) extra->havingQual :
		(List *) parse->havingQual;
	if (fj != NULL && fj->joinpath)
		cx->having = NIL;		/* a join path's rows are no groups */

	/* The FK-side join counts the fact rel's posting sets (DESIGN.md §27). */
	if (fj != NULL)
		cx->input_rel = fj->factrel;

	/* ... and the anti join over a small inner side its outer table's */
	if (cx->anti != NULL)
		cx->input_rel = cx->anti->outerrel;

	return true;
}

/*
 * The relation: one base table or materialized view with indexes, or one
 * partitioned parent.  Sets rti, rte and partitioned.
 */
static bool
lion_count_path_rel(LionCountPathBuild *cx)
{
	PlannerInfo *root = cx->root;
	RelOptInfo *input_rel = cx->input_rel;
	GroupPathExtraData *extra = cx->extra;

	/* ---- a single base relation: one table, or one partitioned parent ---- */
	if (input_rel->reloptkind != RELOPT_BASEREL)
		return false;
	if (bms_membership(input_rel->relids) != BMS_SINGLETON)
		return false;
	cx->rti = input_rel->relid;
	if (cx->rti == 0 || cx->rti >= (Index) root->simple_rel_array_size)
		return false;
	cx->rte = root->simple_rte_array[cx->rti];
	if (cx->rte == NULL || cx->rte->rtekind != RTE_RELATION)
		return false;
	if (cx->rte->securityQuals != NIL || cx->rte->tablesample != NULL)
		return false;
	if (IS_DUMMY_REL(input_rel))
		return false;			/* the planner has already proved it empty */

	/* the anti join's lookups are of one table's B-tree (§27) */
	if (cx->rte->inh && cx->anti != NULL)
		return false;

	if (cx->rte->inh)
	{
		/*
		 * A partitioned parent (DESIGN.md §16).  The parent itself has no
		 * storage and no index list; every live leaf partition is counted in
		 * turn, with its own heap and its own indexes.  Old-style inheritance
		 * parents are not handled: the parent may hold rows of its own, a
		 * child may be a foreign table, and an extension may keep part of a
		 * child's rows outside its heap (TimescaleDB's compressed chunks), so
		 * the sum over the children has no footing there.
		 */
		if (cx->rte->relkind != RELKIND_PARTITIONED_TABLE)
			return false;
		if (input_rel->part_scheme == NULL || !IS_PARTITIONED_REL(input_rel))
			return false;

		/*
		 * With partitionwise aggregation the planner builds its own per-child
		 * grouping paths; ours would only compete on cost, and the interaction
		 * is untested, so stay out of the way.  This applies to partitioned
		 * parents only: grouping_planner() sets patype whenever the GUC is on,
		 * before it knows whether the input is partitioned at all, so testing
		 * it earlier would switch the pushdown off for plain tables too.
		 */
		if (extra != NULL && extra->patype != PARTITIONWISE_AGGREGATE_NONE)
			return false;
		cx->partitioned = true;
	}
	else
	{
		if (cx->rte->relkind != RELKIND_RELATION &&
			cx->rte->relkind != RELKIND_MATVIEW)
			return false;
		/* a parent planned without its children holds part of the rows */
		if (lion_rel_is_unexpanded_parent(cx->rte))
			return false;
		if (input_rel->indexlist == NIL)
			return false;
	}
	return true;
}

/*
 * Is expr an expression of the GROUP BY's columns alone, which splits none of
 * their groups (DESIGN.md §36): `GROUP BY ip, ip - 1, ip - 2`?  It has to be
 * immutable - the same value for the same input - and made of nothing but the
 * grouping columns of this table and constants: no aggregate, window
 * function, set-returning function, subquery or parameter.  Every row of a
 * group then gives it the same value provided the rows' column values are
 * the same, not merely equal - which is what printing the column from the
 * stored key requires as well (lion_index_can_emit_value()), so each column
 * it names is one the node prints (groupvalueout).  The node emits the
 * columns, and the plan's projection computes the expression from them.
 */
static bool
lion_group_dependent(LionCountPathBuild *cx, Node *expr)
{
	List	   *vars;
	ListCell   *lc;

	if (contain_mutable_functions(expr) || contain_volatile_functions(expr) ||
		contain_agg_clause(expr) || contain_window_function(expr) ||
		expression_returns_set(expr) || contain_subplans(expr) ||
		lion_contains_param(expr))
		return false;
	vars = pull_var_clause(expr, PVC_RECURSE_PLACEHOLDERS);
	if (vars == NIL)
		return false;
	foreach(lc, vars)
	{
		Var		   *v = (Var *) lfirst(lc);
		int			g;

		if (!IsA(v, Var) || v->varno != (int) cx->rti ||
			v->varlevelsup != 0)
			return false;
		for (g = 0; g < cx->ngroup; g++)
		{
			if (cx->groupattno[g] == v->varattno)
				break;
		}
		if (g == cx->ngroup)
			return false;
		cx->groupvalueout[g] = true;
	}
	return true;
}

/*
 * GROUP BY: nothing, one indexed column, or two.  Fills in the grouping
 * columns (groupvar and the arrays beside it, ngroup), the coalesce group, or
 * singlegroup for a GROUP BY the planner folded to constants.
 */
static bool
lion_count_path_group_by(LionCountPathBuild *cx)
{
	PlannerInfo *root = cx->root;
	Query	   *parse = cx->parse;
	GroupPathExtraData *extra = cx->extra;
	const LionFkJoin *fj = cx->fj;
	Index		rti = cx->rti;
	ListCell   *lc;

	/*
	 * ---- GROUP BY: nothing, one indexed column, or two (DESIGN.md §20) ----
	 *
	 * Two columns are a nested loop over the two indexes' entries, so every
	 * rule below is made per column: its own index, its own collation, its
	 * own grouping equality, its own value-representation gate.
	 *
	 * None of it applies to the FK-side join (DESIGN.md §27), whose groups are
	 * the dimension's and are formed by the Finalize Agg above the node.
	 */
	int			nvar = 0;

	/*
	 * The grouping clauses that are columns: an expression of them adds no
	 * group, and the walk takes only the columns (lion_group_dependent()).
	 */
	foreach(lc, root->processed_groupClause)
	{
		SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
		TargetEntry *tle = get_sortgroupclause_tle(sgc, root->processed_tlist);
		Node	   *gexpr = (tle != NULL) ?
			lion_vcol_subst((Node *) tle->expr) : NULL;

		if (gexpr != NULL &&
			(IsA(gexpr, CoalesceExpr) ||
			 (lion_strip(gexpr) != NULL && IsA(lion_strip(gexpr), Var))))
			nvar++;
	}
	if (fj == NULL && nvar > LION_MAX_GROUPCOLS)
		return false;

	if (root->processed_groupClause == NIL)
	{
		/*
		 * Every GROUP BY column was proved constant by the planner (it does
		 * that for a column with an equality qual against anything that is
		 * not a Var - a literal or a parameter), so the query has one group - but, unlike a plain aggregate, it
		 * must produce no row at all when nothing matches.
		 */
		cx->singlegroup = (parse->groupClause != NIL);
	}
	else if (fj == NULL)
	{
		foreach(lc, root->processed_groupClause)
		{
			SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
			TargetEntry *tle = get_sortgroupclause_tle(sgc,
													   root->processed_tlist);
			Node	   *gexpr;
			Node	   *expr;
			int			i;

			if (tle == NULL)
				return false;

			/* an expression column is grouped as a column (DESIGN.md §41) */
			gexpr = lion_vcol_subst((Node *) tle->expr);

			/*
			 * `GROUP BY coalesce(col, c)` is col's groups with the NULL group
			 * relabelled c - and merged into c's own group when col has that
			 * value (DESIGN.md §10, "coalesce").  Only that one grouping
			 * column, and only in the plain form, where the node's entry walk
			 * can do the merge exactly (lion_group_coalesce()).
			 */
			if (IsA(gexpr, CoalesceExpr))
			{
				if (list_length(root->processed_groupClause) != 1 ||
					!lion_group_coalesce((CoalesceExpr *) gexpr, rti,
										 &cx->groupvar[cx->ngroup],
										 &cx->groupcoal) ||
					LION_ATTNO_IS_VCOL(cx->groupvar[cx->ngroup]->varattno))
					return false;
				cx->groupexpr = gexpr;
				expr = (Node *) cx->groupvar[cx->ngroup];
			}
			else
				expr = lion_strip(gexpr);
			if (expr == NULL)
				return false;

			/*
			 * An expression of the grouping columns (DESIGN.md §36, `GROUP BY
			 * ip, ip - 1`), taken once every column is known.
			 */
			if (!IsA(expr, Var))
			{
				cx->groupdeps = lappend(cx->groupdeps, gexpr);
				continue;
			}
			cx->groupvar[cx->ngroup] = (Var *) expr;
			if (cx->groupvar[cx->ngroup]->varno != (int) rti ||
				cx->groupvar[cx->ngroup]->varattno <= 0 ||
				cx->groupvar[cx->ngroup]->varlevelsup != 0)
				return false;

			/*
			 * A nullable group column is fine now: NULL keys have an entry of
			 * their own, so the NULL group is produced like any other
			 * (DESIGN.md §14).
			 */
			cx->groupattno[cx->ngroup] = cx->groupvar[cx->ngroup]->varattno;

			/*
			 * The collation the groups are under is the grouping
			 * expression's, not the column's: `GROUP BY s COLLATE c` groups
			 * under c, which lion_strip() took off with the RelabelType.
			 * The index that drives the groups has to be built under it
			 * (lion_collect_targets()).
			 */
			cx->groupcoll[cx->ngroup] = exprCollation(gexpr);

			/* The same column twice is not a grouping this node can drive. */
			for (i = 0; i < cx->ngroup; i++)
			{
				if (cx->groupattno[i] == cx->groupattno[cx->ngroup])
					return false;
			}

			/*
			 * The equality the planner chose for this column is what the index
			 * that drives the scan has to implement, whether there is one
			 * relation or many (lion_collect_targets(), finding 3 of the
			 * 2026-09-20 review), so a grouping clause without one is of no use
			 * here.
			 */
			cx->groupeqop[cx->ngroup] = sgc->eqop;
			if (!OidIsValid(cx->groupeqop[cx->ngroup]))
				return false;

			/*
			 * A partitioned GROUP BY emits one PARTIAL aggregate per group per
			 * partition and lets core's Finalize HashAggregate combine them
			 * (DESIGN.md §16), so the column has to be hashable and the planner
			 * has to consider the aggregates splittable at all.  count(*) and
			 * count(col) always are, but the answer is the planner's to give.
			 * One table needs neither: it streams its finished groups.
			 */
			if (cx->partitioned &&
				(!sgc->hashable || extra == NULL ||
				 (extra->flags & GROUPING_CAN_PARTIAL_AGG) == 0))
				return false;

			/*
			 * Three or more columns are the decoded walk of DESIGN.md §34,
			 * which emits partial counts too - a combination's rows may come
			 * out in more than one row, when a pass's tally spills - for a
			 * Finalize Agg to add up: the same two requirements.  It counts
			 * one table, and is not made on a hot standby, where no WHERE is
			 * collected.
			 */
			if (nvar > 2 &&
				(!lion_enable_decoded_walk ||
				 cx->partitioned || !sgc->hashable || extra == NULL ||
				 (extra->flags & GROUPING_CAN_PARTIAL_AGG) == 0 ||
				 RecoveryInProgress()))
				return false;

			cx->ngroup++;
		}
		cx->decode = (cx->ngroup > 2);

		/*
		 * The expressions of the grouping columns split no group: only in
		 * one table, which emits its finished groups, and with a column to
		 * be an expression of.
		 */
		foreach(lc, cx->groupdeps)
		{
			if (cx->ngroup == 0 || cx->partitioned || cx->decode ||
				!lion_group_dependent(cx, (Node *) lfirst(lc)))
				return false;
		}
	}
	return true;
}

/*
 * The clause just appended: of the restriction being analysed, or of the
 * filtered count being analysed (DESIGN.md §10, "Filtered counts"), which
 * is no restriction of the relation and never left out of a partition's
 * count.
 */
static void
lion_count_path_noted(LionCountPathBuild *cx)
{
	LionClauseInfo *ci = (LionClauseInfo *) llast(cx->clauseinfos);

	ci->filter = cx->curfilter;
	ci->rinfono = (cx->curfilter > 0) ? -1 : cx->rinfono;
}

/*
 * A WHERE restriction that is an OR (lion_count_path_where()), taken as one
 * source.  False when no posting set answers it - an arm none answers, or a
 * complement OR past the few there may be - with the leaves analysed by then
 * appended to the clause lists and nothing else of cx changed; the caller
 * takes the leaves back with the rest of the restriction's analysis.
 */
static bool
lion_count_path_where_or(LionCountPathBuild *cx, Node *clause)
{
	PlannerInfo *root = cx->root;
	Index		rti = cx->rti;
	LionLeafInfo leaf;
	List	   *arms;
	List	   *armlens = NIL;
	int			orfirst = list_length(cx->whereattnos);
	bool		thisrange = false;
	bool		thissliced = false;
	bool		compl = false;
	bool		allplain = true;
	ListCell   *la;

	/*
	 * ---- an OR across columns (DESIGN.md §19) ----
	 *
	 * Every arm has to be a positive clause the posting sets can answer,
	 * or an AND of such clauses, each on a column of this relation.  The
	 * whole restriction then becomes ONE source - the union of the arms -
	 * which is ANDed with the other sources and with the GROUP BY driver
	 * like any other.  A negated arm (`IS NOT NULL`, a NOT of anything
	 * but a boolean column) is declined: the complement of a posting set
	 * is not a posting set, and under a union there is nothing to
	 * subtract it from.  An arm `flag IS NOT TRUE` is two arms of the
	 * same union, `flag = false` and `flag IS NULL`, and an AND arm with
	 * an OR inside - `flag IS NOT TRUE` among its terms, or any other -
	 * is distributed into the arms it stands for (lion_or_arms()).
	 *
	 * The leaves are appended to the clause array like plain clauses, so
	 * that an index is matched for each of them per partition and a Param
	 * among them reaches custom_exprs; what marks them out is that they
	 * are contiguous and named by an entry in `ors`.  They constrain no
	 * column of the RESULT, so none of the attnum bookkeeping below
	 * (posattnos, eqattnos, nonnullattnos, nullattnos) takes them: the
	 * other arm may hold rows where this arm's column is NULL, or is
	 * anything at all.
	 *
	 * An arm may also be a COMPLEMENT (DESIGN.md §35, "Complements in an
	 * OR"): an AND of clauses that each reject the rows of a few posting
	 * sets - `<>`, `IS NOT NULL`, `IS DISTINCT FROM`, `NOT (tags @> ...)`.
	 * Such an arm is true of every row outside the union N of its sets, and
	 * the OR is then counted by De Morgan as two counts the executor adds
	 * up: the rows outside M, the intersection of the complement arms' N,
	 * and the rows inside M that a positive arm selects
	 * (lion_node_count()).  An arm that mixes the two kinds is declined.
	 */
	if (list_length(((BoolExpr *) clause)->args) < 2)
		return false;

	arms = lion_or_arms(clause);
	if (arms == NIL)
		return false;

	foreach(la, arms)
	{
		List	   *arm = (List *) lfirst(la);
		ListCell   *lb;
		int			nneg = 0;

		foreach(lb, arm)
		{
			bool		sliced = false;

			if (!lion_analyze_leaf(root, (Node *) lfirst(lb), rti,
								  true, true, false, &leaf))
				return false;
			if (LION_CLAUSE_IS_NEGATED(leaf.kind))
				nneg++;

			/*
			 * A range in an arm is a leaf of the union like any other:
			 * the rows whose key lies in it, collected (DESIGN.md §32) -
			 * in a build that answers ranges from slices, from those of
			 * its column where it has them (§53), and then ANDed with the
			 * column's existence slice, which pins it as any leaf.
			 */
			if (leaf.kind == LION_CLAUSE_RANGE)
			{
				leaf.kind = LION_CLAUSE_RANGESRC;
				if (cx->slicerange &&
					lion_count_slice_range_ok(cx, leaf.var->varattno,
											  leaf.cmptype))
					sliced = true;
				else
					thisrange = true;
			}
			if (!(leaf.kind == LION_CLAUSE_EQ ||
				  leaf.kind == LION_CLAUSE_ARRAY ||
				  leaf.kind == LION_CLAUSE_NULL ||
				  leaf.kind == LION_CLAUSE_RESERVED ||
				  (leaf.kind == LION_CLAUSE_MULTI && leaf.val != NULL &&
				   IsA(leaf.val, Const))))
				allplain = false;
			lion_append_clause(&leaf, (Node *) lfirst(lb), true,
							  &cx->whereattnos, &cx->clauseinfos,
							  &cx->whereclauses, &cx->whereconsts,
							  &cx->wherekinds, &cx->whereopnos,
							  &cx->whereinor);
			lion_count_path_noted(cx);
			if (sliced)
			{
				((LionClauseInfo *) llast(cx->clauseinfos))->sliced = true;
				thissliced = true;
			}
		}
		if (nneg > 0 && nneg < list_length(arm))
			return false;		/* a complement ANDed with a selection */
		if (nneg > 0)
			compl = true;
		armlens = lappend_int(armlens, list_length(arm));
	}

	/*
	 * An OR with a complement arm selects nothing by itself: the rows
	 * outside M are what something else - a group, the sum over every row
	 * (§35) - has to hand the count, so it is no positive source of the AND
	 * and carries no pin for one.  Each such OR doubles the counts every
	 * count becomes, so there may be a few of them at most - which is asked
	 * before the OR is noted anywhere: one past them is a restriction no
	 * posting set answers, residual, implied by every partition or the end
	 * of the query, and an entry in `ors` left behind would make the clauses
	 * appended after it its arms.
	 */
	if (compl && cx->ncompl >= LION_MAX_COMPL_ORS)
		return false;

	cx->ors = lappend(cx->ors,
					  list_concat(list_make2_int(orfirst,
												 list_length(armlens)),
								  armlens));
	if (thisrange || thissliced)
		cx->hasrangesrc = true;
	if (thissliced)
		cx->nsliced++;
	if (compl)
	{
		ListCell   *lc;

		for_each_from(lc, cx->clauseinfos, orfirst)
			((LionClauseInfo *) lfirst(lc))->incompl = true;
		cx->ncompl++;
		return true;
	}
	cx->havepositive = true;

	/*
	 * A union holds the §9 pin at every container key only when every
	 * leaf does, which a range collected into memory does not
	 * (DESIGN.md §32).
	 */
	if (!thisrange)
		cx->pinnedsrc = true;

	/*
	 * A filtered count's condition (DESIGN.md §10, "Filtered counts") that
	 * is such a union of leaves positive whatever their values is a source
	 * its run's decoded walk can begin from (§34).
	 */
	if (cx->curfilter > 0 && !thisrange && allplain)
		cx->plainpositive = true;
	return true;
}

/*
 * A WHERE restriction that is no OR (lion_count_path_where()): a range
 * comparison, gathered per column, or a clause the posting sets answer, with
 * what it says of its column noted.  False when no posting set answers it;
 * true, too, for the very same positive clause again, which is dropped.
 */
static bool
lion_count_path_where_leaf(LionCountPathBuild *cx, RestrictInfo *rinfo,
						   Node *clause)
{
	PlannerInfo *root = cx->root;
	Index		rti = cx->rti;
	LionLeafInfo leaf;

	if (!lion_analyze_leaf(root, clause, rti, true, true, true, &leaf))
		return false;

	/*
	 * A range in a filtered count's condition (DESIGN.md §10, "Filtered
	 * counts") bounds no walk: the walk is the WHERE's.  It is a bound of a
	 * range taken as a source (§32), with the filter's other bounds on its
	 * column, collected or walked beside the WHERE's sources.
	 */
	if (leaf.kind == LION_CLAUSE_RANGE && cx->curfilter > 0)
	{
		leaf.kind = LION_CLAUSE_RANGESRC;
		cx->hasrangesrc = true;
		cx->havepositive = true;
		cx->nonnullattnos = lappend_int(cx->nonnullattnos,
										(int) leaf.var->varattno);
		lion_append_clause(&leaf, clause, false,
						  &cx->whereattnos, &cx->clauseinfos,
						  &cx->whereclauses, &cx->whereconsts,
						  &cx->wherekinds, &cx->whereopnos,
						  &cx->whereinor);
		lion_count_path_noted(cx);
		return true;
	}

	/*
	 * A range comparison (DESIGN.md §28) bounds the entry walk that DRIVES
	 * the count - or, on a column that does not drive it, is a bound of a
	 * range taken as a source (§32) - which is decided once everything
	 * else is known.  Any number of them may name one column: they are
	 * ANDed into one range, and they are gathered per column here.  A
	 * strict comparison is never true of NULL, so the column is non-NULL
	 * in every row counted.
	 */
	if (leaf.kind == LION_CLAUSE_RANGE)
	{
		int			pos = list_length(cx->whereattnos);
		int			r = 0;
		ListCell   *lr;

		foreach(lr, cx->rangevars)
		{
			if (((Var *) lfirst(lr))->varattno == leaf.var->varattno)
				break;
			r++;
		}
		if (lr == NULL)
		{
			cx->rangevars = lappend(cx->rangevars, leaf.var);
			cx->rangecls = lappend(cx->rangecls, list_make1(rinfo));
			cx->rangepos = lappend(cx->rangepos, list_make1_int(pos));
		}
		else
		{
			ListCell   *c1 = list_nth_cell(cx->rangecls, r);
			ListCell   *c2 = list_nth_cell(cx->rangepos, r);

			lfirst(c1) = lappend((List *) lfirst(c1), rinfo);
			lfirst(c2) = lappend_int((List *) lfirst(c2), pos);
		}
		cx->nonnullattnos = lappend_int(cx->nonnullattnos,
										(int) leaf.var->varattno);
		lion_append_clause(&leaf, clause, false,
						  &cx->whereattnos, &cx->clauseinfos,
						  &cx->whereclauses, &cx->whereconsts,
						  &cx->wherekinds, &cx->whereopnos,
						  &cx->whereinor);
		lion_count_path_noted(cx);
		return true;
	}

	/*
	 * In a build that answers ranges from slices (DESIGN.md §53), `x <> c` on
	 * a column the slices answer is a range with a HOLE (§35), taken as a
	 * source from the slices - E less the rows equal to c, ANDed with E - and
	 * one with any other range on the column, whose bounds it joins in the
	 * one fold: a positive source that carries its own pin, where `<>`
	 * elsewhere only subtracts.  Not on a column a GROUP BY or a
	 * count(DISTINCT) walks, nor in a filtered count's condition.
	 */
	if (leaf.kind == LION_CLAUSE_NE && cx->slicerange && cx->curfilter == 0 &&
		!lion_count_path_walks_column(cx, leaf.var->varattno) &&
		lion_count_slice_range_ok(cx, leaf.var->varattno, leaf.cmptype))
	{
		leaf.kind = LION_CLAUSE_RANGESRC;
		cx->hasrangesrc = true;
		cx->havepositive = true;
		cx->slicedsrc = true;
		cx->nsliced++;
		cx->nonnullattnos = lappend_int(cx->nonnullattnos,
										(int) leaf.var->varattno);
		lion_append_clause(&leaf, clause, false,
						  &cx->whereattnos, &cx->clauseinfos,
						  &cx->whereclauses, &cx->whereconsts,
						  &cx->wherekinds, &cx->whereopnos,
						  &cx->whereinor);
		((LionClauseInfo *) llast(cx->clauseinfos))->sliced = true;
		lion_count_path_noted(cx);
		return true;
	}

	/*
	 * Which index answers the clause, whether its opfamily has the
	 * operator as strategy 1, and whether it can hash and compare the
	 * constant's type is settled per relation, in lion_match_index():
	 * with partitions there is one index per partition and they need not
	 * share an opclass (DESIGN.md §16).
	 */

	if (LION_CLAUSE_IS_POSITIVE(leaf.kind))
	{
		cx->havepositive = true;
		cx->pinnedsrc = true;
	}

	/*
	 * A multi-key clause whose value is only known at run time may turn
	 * out to narrow nothing, and be counted as a negated source (§17);
	 * every other positive clause stays one.  A literal tsquery with a
	 * prefix lexeme stays one only while the run can expand the prefix
	 * into the index's lexemes that have it, and narrows nothing past
	 * that (lion_tsquery_expand_prefixes()); so does a jsonpath, whose
	 * keys are all prefixes (§43): neither is a source positive whatever
	 * its value.
	 */
	if (leaf.kind == LION_CLAUSE_MULTI &&
		(leaf.val == NULL || !IsA(leaf.val, Const)))
		cx->runtimemulti = true;
	else if (leaf.kind == LION_CLAUSE_MULTI &&
			 (LION_STRAT_IS_JSONPATH(leaf.strategy) ||
			  (leaf.strategy == LION_STRAT_MATCH &&
			   lion_tsquery_has_prefix(((Const *) leaf.val)->constvalue))))
		cx->prefixmulti = true;
	else if (leaf.kind == LION_CLAUSE_EQ || leaf.kind == LION_CLAUSE_ARRAY ||
			 leaf.kind == LION_CLAUSE_NULL ||
			 leaf.kind == LION_CLAUSE_RESERVED ||
			 leaf.kind == LION_CLAUSE_MULTI)
		cx->plainpositive = true;

	if (LION_CLAUSE_IS_POSITIVE(leaf.kind) && leaf.kind != LION_CLAUSE_MULTI)
	{
		/*
		 * A second positive clause on a column is a source of its own,
		 * ANDed with the first exactly as a clause on another column is:
		 * it is matched to an index that answers ITS operator under ITS
		 * collation (lion_match_index(), per relation), and the AND of
		 * two exact sources is exact.  Only the very same clause again -
		 * the same kind, the same operator, the same input collation and
		 * an equal() value - is dropped, because the lookup the first one
		 * makes is then its answer too.
		 *
		 * Anything less than all four is not "the same clause", and a
		 * clause dropped here is one no index is ever asked about (the
		 * 2026-09-25 review).  Comparing the kind and the value alone
		 * called `v === 'A' AND v = 'A'` a duplicate over an index whose
		 * case-insensitive opclass answers `===` - and nothing at all
		 * answered `=` - so both spellings were counted, 10000 rows
		 * where the query selects 5000.  A nondeterministic collation
		 * does it with one operator: `s COLLATE ci = 'a' AND s = 'a'`
		 * differ in nothing but their input collation.  Now the second
		 * clause needs an index of its own and the query is declined
		 * when there is none.
		 *
		 * Two clauses that differ only in value used to be declined too,
		 * on the grounds that they select little or nothing.  They are
		 * answered now, because nothing in the AND is specific to one
		 * clause per column: the entry a GROUP BY or an IN list drives
		 * is the FIRST suitable clause on its index (lion_inlist_shape()
		 * and lion_locate_where() agree on that), the key a target list
		 * prints is the first pinning clause's, and every clause on the
		 * column must then be one whose index may print it.  Under a
		 * coarse equality they need not even disagree: `v === 'a' AND
		 * v === 'A'` is one entry, looked up twice.
		 *
		 * `IS NOT NULL` is not subject to any of this - it constrains
		 * nothing by itself and is simply subtracted - and neither is a
		 * multi-key clause, whose sources intersect exactly as two
		 * clauses on different columns do (`tags @> '{a}' AND
		 * tags && '{b,c}'` is one AND of three key sets).  Nor is a leaf
		 * of an OR, which says nothing about the rows the OTHER arms
		 * select and so cannot stand in for a clause that does.
		 */
		if (list_member_int(cx->posattnos, (int) leaf.var->varattno))
		{
			ListCell   *l1;
			ListCell   *l2;
			ListCell   *l3;
			bool		same = false;

			forthree(l1, cx->clauseinfos, l2, cx->whereconsts,
					 l3, cx->whereinor)
			{
				LionClauseInfo *prev = (LionClauseInfo *) lfirst(l1);

				if (lfirst_int(l3) != 0 ||
					prev->attno != leaf.var->varattno ||
					prev->filter != cx->curfilter)
					continue;
				if (prev->kind == leaf.kind &&
					prev->opno == leaf.opno &&
					prev->collation == leaf.collation &&
					equal(lfirst(l2), leaf.val))
				{
					same = true;
					break;
				}
			}
			if (same)
				return true;
		}
		else
			cx->posattnos = lappend_int(cx->posattnos,
										(int) leaf.var->varattno);
	}

	/*
	 * The same NOTRESERVED clause again - `array_length(a, 1) IS NOT NULL
	 * AND array_length(a, 1) >= 0` is one set of rows written twice
	 * (DESIGN.md §17, "Non-empty arrays") - would subtract the same entries
	 * twice, and is dropped as the same positive clause is above.
	 */
	if (leaf.kind == LION_CLAUSE_NOTRESERVED)
	{
		ListCell   *l1;
		ListCell   *l2;
		ListCell   *l3;

		forthree(l1, cx->clauseinfos, l2, cx->whereconsts, l3, cx->whereinor)
		{
			LionClauseInfo *prev = (LionClauseInfo *) lfirst(l1);

			if (lfirst_int(l3) == 0 && prev->kind == leaf.kind &&
				prev->attno == leaf.var->varattno &&
				prev->filter == cx->curfilter && equal(lfirst(l2), leaf.val))
				return true;
		}
	}

	switch (leaf.kind)
	{
		case LION_CLAUSE_EQ:
			cx->eqattnos = lappend_int(cx->eqattnos, (int) leaf.var->varattno);
			cx->nonnullattnos = lappend_int(cx->nonnullattnos,
											(int) leaf.var->varattno);
			break;
		case LION_CLAUSE_NOTNULL:
			if (cx->notnullvar == NULL)
				cx->notnullvar = leaf.var;
			cx->nonnullattnos = lappend_int(cx->nonnullattnos,
											(int) leaf.var->varattno);

			/*
			 * On an expression column (§41) it is no heap column's test, which
			 * is all the row filter of a WHERE with nothing else to take its
			 * candidates from makes (lion_filter_with_negated()): not beside a
			 * query that may narrow nothing (lion_where_scan_untested()), as
			 * every negated clause below.
			 */
			if (LION_ATTNO_IS_VCOL(leaf.var->varattno))
				cx->exprnegated = true;
			break;
		case LION_CLAUSE_NE:
			/* a strict operator: never true of NULL (DESIGN.md §35) */
			cx->nonnullattnos = lappend_int(cx->nonnullattnos,
											(int) leaf.var->varattno);
			/* ... and on an expression column, as `IS NOT NULL` above */
			if (LION_ATTNO_IS_VCOL(leaf.var->varattno))
				cx->exprnegated = true;
			break;
		case LION_CLAUSE_NOTMULTI:
			/* NOT of a strict operator: NULL, never true, of NULL (§35) */
			cx->nonnullattnos = lappend_int(cx->nonnullattnos,
											(int) leaf.var->varattno);
			cx->newnegated = true;
			if (LION_ATTNO_IS_VCOL(leaf.var->varattno))
				cx->exprnegated = true;
			break;
		case LION_CLAUSE_DISTINCT:
			/* true of NULL: it proves nothing of the column (§35) */
			cx->newnegated = true;
			if (LION_ATTNO_IS_VCOL(leaf.var->varattno))
				cx->exprnegated = true;
			break;
		case LION_CLAUSE_ARRAY:
		case LION_CLAUSE_MULTI:
			/* a strict operator with a non-NULL constant */
			cx->nonnullattnos = lappend_int(cx->nonnullattnos,
											(int) leaf.var->varattno);
			break;
		case LION_CLAUSE_NULL:
			cx->nullattnos = lappend_int(cx->nullattnos,
										 (int) leaf.var->varattno);
			break;
		case LION_CLAUSE_RESERVED:
			/* the empty entries alone: none of their rows is NULL (§17) */
			if ((DatumGetInt32(((Const *) leaf.val)->constvalue) &
				 LION_ENTRY_NULLKEY) == 0)
				cx->nonnullattnos = lappend_int(cx->nonnullattnos,
												(int) leaf.var->varattno);
			break;
		case LION_CLAUSE_NOTRESERVED:

			/*
			 * Every row outside the reserved entries named (§17).  With the
			 * NULL entry among them NULLs fail it - `col <> '{}'`,
			 * `cardinality(col) > 0` - and without it they pass, as they
			 * pass the new negated clauses of §35.  The row filter of a WHERE
			 * with nothing else to take its candidates from tests either on
			 * a heap column (lion_filter_with_negated()).
			 */
			if ((DatumGetInt32(((Const *) leaf.val)->constvalue) &
				 LION_ENTRY_NULLKEY) != 0)
				cx->nonnullattnos = lappend_int(cx->nonnullattnos,
												(int) leaf.var->varattno);
			else
				cx->newnegated = true;
			if (LION_ATTNO_IS_VCOL(leaf.var->varattno))
			{
				cx->newnegated = true;
				cx->exprnegated = true;
			}
			break;
	}

	lion_append_clause(&leaf, clause, false,
					  &cx->whereattnos, &cx->clauseinfos, &cx->whereclauses,
					  &cx->whereconsts, &cx->wherekinds, &cx->whereopnos,
					  &cx->whereinor);
	lion_count_path_noted(cx);
	return true;
}

/*
 * `col <> ALL (array)` - `col NOT IN (a, b, ...)` - as the conjunction it
 * stands for, `col <> a AND col <> b AND ...` (DESIGN.md §10, "Shapes
 * rewritten to the node's counts"): one OpExpr a literal element, or NIL
 * where the rewrite is not made.  Both forms are false or unknown for a NULL
 * col, and for every other row they agree element by element, so the rewrite
 * is exact - provided no element is NULL: one makes the ALL unknown for
 * every row, and is left to the ordinary plan.  An empty list, true of every
 * row, is left there too, and so is an array known only at run time, whose
 * elements are not there to be taken apart.  Each conjunct has the array's
 * operator and input collation; whether a posting set answers it is the
 * caller's question.
 */
List *
lion_all_as_and(Node *clause)
{
	ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;
	Node	   *right;
	Const	   *arr;
	ArrayType  *a;
	Oid			elemtype;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	Datum	   *elems;
	bool	   *nulls;
	int			n;
	int			i;
	List	   *out = NIL;

	if (!IsA(clause, ScalarArrayOpExpr) || saop->useOr ||
		list_length(saop->args) != 2)
		return NIL;
	right = lion_strip((Node *) lsecond(saop->args));
	if (right == NULL || !IsA(right, Const) || ((Const *) right)->constisnull)
		return NIL;
	arr = (Const *) right;
	a = DatumGetArrayTypeP(arr->constvalue);
	elemtype = ARR_ELEMTYPE(a);
	get_typlenbyvalalign(elemtype, &typlen, &typbyval, &typalign);
	deconstruct_array(a, elemtype, typlen, typbyval, typalign,
					  &elems, &nulls, &n);
	if (n == 0 || n > LION_MAX_ARRAY_ELEMS)
		return NIL;
	for (i = 0; i < n; i++)
	{
		Const	   *c;
		OpExpr	   *op;

		if (nulls[i])
			return NIL;
		c = makeConst(elemtype, -1, arr->constcollid, typlen,
					  datumCopy(elems[i], typbyval, typlen), false, typbyval);
		op = (OpExpr *) make_opclause(saop->opno, BOOLOID, false,
									  (Expr *) copyObject(linitial(saop->args)),
									  (Expr *) c, InvalidOid,
									  saop->inputcollid);
		op->opfuncid = saop->opfuncid;
		out = lappend(out, op);
	}
	return out;
}

/*
 * May a WHERE clause no posting set answers be tested on the heap tuple of
 * each row the others select (DESIGN.md §10, "Residual clauses")?  Of one
 * table's count, or each partition's, not an FK-side join's: an expression
 * over the table's own user columns, Params and constants that calls
 * nothing volatile and holds no subplan, which the node evaluates as the
 * ordinary plan's scan would - with ExecInitQual() - on the rows the
 * snapshot sees.  Over a partitioned table, not one on a partitioning key
 * with a Param or a stable function in it: that is a clause run-time
 * pruning decides, and the node, which keeps the planner's partitions
 * (§16), would fetch the rows of every partition pruning skips to test it.
 */
static bool
lion_residual_ok(LionCountPathBuild *cx, RestrictInfo *rinfo)
{
	Node	   *clause = (Node *) rinfo->clause;
	List	   *vars;
	ListCell   *lc;
	Bitmapset  *attnos = NULL;
	bool		ok = true;

	if (!lion_enable_count_residual || cx->fj != NULL)
		return false;
	if (contain_subplans(clause) || contain_volatile_functions(clause) ||
		contain_agg_clause(clause) || contain_window_function(clause))
		return false;
	vars = pull_var_clause(clause, PVC_INCLUDE_PLACEHOLDERS);
	foreach(lc, vars)
	{
		Var		   *var = (Var *) lfirst(lc);

		if (!IsA(var, Var) || var->varno != (int) cx->rti ||
			var->varlevelsup != 0 || var->varattno <= 0)
		{
			ok = false;
			break;
		}
		attnos = bms_add_member(attnos, (int) var->varattno);
	}
	list_free(vars);
	if (ok && cx->partitioned &&
		(lion_contains_param(clause) || contain_mutable_functions(clause)))
	{
		if (!cx->keyattnosdone)
		{
			cx->keyattnos = lion_partition_key_attnos(cx->root,
													  cx->input_rel);
			cx->keyattnosdone = true;
		}
		if (bms_overlap(attnos, cx->keyattnos))
			ok = false;
	}
	bms_free(attnos);
	return ok;
}

/*
 * Does rel - a table, or a partitioned one, through every level down to its
 * live leaves - have a lion index to answer clause ci on column attno (in
 * rel's own numbering, translated for each child)?  Of a partitioned table,
 * any live leaf's will do: a column no leaf indexes is what makes the clause
 * residual (lion_where_indexed()), and one some leaf indexes and another
 * does not is the partitions' to settle when they are matched
 * (lion_collect_targets()).
 */
static bool
lion_rel_has_index(PlannerInfo *root, RelOptInfo *rel, AttrNumber attno,
				   const LionClauseInfo *ci)
{
	AttrNumber	col = 1;

	check_stack_depth();
	if (IS_PARTITIONED_REL(rel))
	{
		int			i;

		for (i = 0; i < rel->nparts; i++)
		{
			RelOptInfo *child = rel->part_rels[i];
			AttrNumber	cattno = attno;

			if (child == NULL || !bms_is_member(i, rel->live_parts) ||
				IS_DUMMY_REL(child))
				continue;
			if (attno > 0 && !LION_ATTNO_IS_VCOL(attno))
			{
				Var		   *cvar = lion_child_var(root, child->relid, attno);

				if (cvar == NULL)
					continue;
				cattno = cvar->varattno;
			}
			if (lion_rel_has_index(root, child, cattno, ci))
				return true;
		}
		return false;
	}
	return lion_match_index(rel, attno, ci->kind, ci->opno, ci->cmptype,
							ci->strategy, ci->extractquery, ci->collation,
							ci->positions, &col) != NULL;
}

/*
 * Has every clause the analysis of one restriction appended, from position
 * `from` on, an index of this table to answer it?  A clause of the right
 * shape on a column no lion index covers - `id > 10` - is only found out
 * when the relation's indexes are matched (lion_collect_targets()), which
 * declines the whole query; asked here, it can be residual instead.  A
 * partitioned table has no indexes of its own, and its live leaves' stand
 * for them (lion_rel_has_index()): a column no leaf indexes is found out
 * here, and one some leaf indexes and another does not when the partitions
 * are matched, which may ask for a second attempt then (DESIGN.md §16,
 * "Residual clauses over a partitioned table").
 */
static bool
lion_where_indexed(LionCountPathBuild *cx, int from)
{
	ListCell   *l1;
	ListCell   *l2;
	int			i = 0;

	forboth(l1, cx->whereattnos, l2, cx->clauseinfos)
	{
		LionClauseInfo *ci = (LionClauseInfo *) lfirst(l2);

		if (i++ < from)
			continue;
		if (!lion_rel_has_index(cx->root, cx->input_rel,
								(AttrNumber) lfirst_int(l1), ci))
			return false;
	}
	return true;
}

/*
 * What the analysis of one restriction may change in cx, kept so that a
 * restriction no posting set answers - one that turns out to be residual,
 * over a count by cells' predicates, or implied by every partition - leaves
 * no trace of what it had taken already: the leaves of an OR's first arms,
 * the bounds of a range, what they said of their columns.  The struct, with
 * its own copy of every list the analysis appends to - and of the lists
 * inside rangecls and rangepos, one per column, which
 * lion_count_path_where_leaf() appends to in place: copies of the outer lists
 * alone would share those with cx, and a bound taken back would stay in its
 * column's at the position the next clause appended then has, or at one
 * past the end of the lists.
 *
 * Each copy is as long as the lists are by then, and a WHERE of n
 * restrictions takes n of them, so they are given back as soon as they are
 * done with: the snapshot's once its restriction is answered, or declines
 * the query (lion_count_path_snapshot_free()), the build's own once the
 * snapshot takes their place (lion_count_path_restore()).  Kept, they made
 * the planner's memory grow with the square of the restrictions: 174 MB for
 * 2000 `<>` clauses on one column, where the plan without the pushdown takes
 * 3 MB.
 */
static void
lion_count_path_snapshot(const LionCountPathBuild *cx,
						 LionCountPathBuild *snap)
{
	ListCell   *lc;

	*snap = *cx;
	snap->whereattnos = list_copy(cx->whereattnos);
	snap->clauseinfos = list_copy(cx->clauseinfos);
	snap->whereclauses = list_copy(cx->whereclauses);
	snap->whereconsts = list_copy(cx->whereconsts);
	snap->wherekinds = list_copy(cx->wherekinds);
	snap->whereopnos = list_copy(cx->whereopnos);
	snap->whereinor = list_copy(cx->whereinor);
	snap->ors = list_copy(cx->ors);
	snap->posattnos = list_copy(cx->posattnos);
	snap->eqattnos = list_copy(cx->eqattnos);
	snap->nonnullattnos = list_copy(cx->nonnullattnos);
	snap->nullattnos = list_copy(cx->nullattnos);
	snap->valueattnos = list_copy(cx->valueattnos);
	snap->rangevars = list_copy(cx->rangevars);
	snap->rangecls = NIL;
	foreach(lc, cx->rangecls)
		snap->rangecls = lappend(snap->rangecls,
								 list_copy((List *) lfirst(lc)));
	snap->rangepos = NIL;
	foreach(lc, cx->rangepos)
		snap->rangepos = lappend(snap->rangepos,
								 list_copy((List *) lfirst(lc)));
}

/*
 * Free the lists of b that lion_count_path_snapshot() copies - the outer
 * lists, and the per-column lists inside rangecls and rangepos - and nothing
 * they point to: the clauses, Vars and LionClauseInfos are shared between a
 * build and its snapshot, and so are the IntLists of `ors`, which the
 * snapshot copies one level deep only.  Either side may be freed so, for
 * neither shares a list with the other, nor with anything outside the
 * WHERE's analysis: every one of these lists is NIL when
 * lion_count_path_where() begins (lion_count_path_init(); no phase before
 * it fills one), and only the analysis of a restriction appends to them.
 */
static void
lion_count_path_snapshot_free(LionCountPathBuild *b)
{
	ListCell   *lc;

	list_free(b->whereattnos);
	list_free(b->clauseinfos);
	list_free(b->whereclauses);
	list_free(b->whereconsts);
	list_free(b->wherekinds);
	list_free(b->whereopnos);
	list_free(b->whereinor);
	list_free(b->ors);
	list_free(b->posattnos);
	list_free(b->eqattnos);
	list_free(b->nonnullattnos);
	list_free(b->nullattnos);
	list_free(b->valueattnos);
	list_free(b->rangevars);
	foreach(lc, b->rangecls)
		list_free((List *) lfirst(lc));
	list_free(b->rangecls);
	foreach(lc, b->rangepos)
		list_free((List *) lfirst(lc));
	list_free(b->rangepos);
}

/*
 * Take back the analysis of a restriction: cx as snap held it, the lists
 * the analysis had changed freed first.  What the restriction became -
 * residual, a cell's predicate, implied by every partition - is the
 * caller's to note, after this.
 */
static void
lion_count_path_restore(LionCountPathBuild *cx, const LionCountPathBuild *snap)
{
	lion_count_path_snapshot_free(cx);
	*cx = *snap;
}

/*
 * The columns of the relation counted that the output prints outside an
 * aggregate, other than the GROUP BY's: the target list's and the HAVING's,
 * as attnos offset by FirstLowInvalidHeapAttributeNumber (pull_varattnos()).
 * A WHERE clause that pins one is what its printed value comes from (§10),
 * so it is never left out for a partial index's predicate (DESIGN.md §45).
 */
static Bitmapset *
lion_count_path_printed(LionCountPathBuild *cx)
{
	PlannerInfo *root = cx->root;
	List	   *exprs = list_copy(cx->output_rel->reltarget->exprs);
	Bitmapset  *printed = NULL;
	Bitmapset  *grouped = NULL;
	List	   *vars;
	ListCell   *lc;

	exprs = list_concat(exprs, cx->having);
	vars = pull_var_clause((Node *) exprs,
						   PVC_INCLUDE_AGGREGATES |
						   PVC_INCLUDE_WINDOWFUNCS |
						   PVC_RECURSE_PLACEHOLDERS);
	foreach(lc, vars)
	{
		Var		   *v = (Var *) lfirst(lc);

		if (IsA(v, Var) && v->varno == (int) cx->rti && v->varlevelsup == 0)
			printed = bms_add_member(printed,
									 v->varattno -
									 FirstLowInvalidHeapAttributeNumber);
	}
	foreach(lc, root->processed_groupClause)
	{
		SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
		TargetEntry *tle = get_sortgroupclause_tle(sgc, root->processed_tlist);

		if (tle != NULL)
			pull_varattnos((Node *) tle->expr, cx->rti, &grouped);
	}
	return bms_difference(printed, grouped);
}

/*
 * Is restriction rinfo one the predicate of the partial index the count is
 * planned with implies (lion_partial_drops(), DESIGN.md §45)?  It is then
 * left out of the count, as core leaves it out of a partial index scan's
 * indrestrictinfo, and EXPLAIN says so ("Implied by Index Predicate") - but
 * not where it pins a column the output prints (`printed`).
 */
static bool
lion_count_path_pred_drops(LionCountPathBuild *cx, RestrictInfo *rinfo,
						   Bitmapset *printed)
{
	Bitmapset  *attnos = NULL;

	if (!lion_partial_active())
		return false;
	pull_varattnos((Node *) rinfo->clause, cx->rti, &attnos);
	if (bms_overlap(attnos, printed))
		return false;
	if (!lion_partial_drops(cx->root, cx->input_rel, (Node *) rinfo->clause))
		return false;
	lion_partial_note_drop(lion_deparse_rel_clause(cx->root, cx->rti,
												   (Node *) rinfo->clause));
	return true;
}

/*
 * Every WHERE clause must be one the posting sets can answer, or, over a
 * partitioned table, one every partition's bounds imply.  Builds the clause
 * lists - whereattnos, clauseinfos, whereclauses, whereconsts, wherekinds,
 * whereopnos and whereinor, in step - with ors, the ranges per column
 * (rangevars, rangecls, rangepos) and what the clauses say of each column.
 * A restriction no posting set answers that is residual, over a count by
 * cells' predicates or implied by every partition is no clause of the lists,
 * and whatever its analysis had noted of it is taken back
 * (lion_count_path_snapshot()).
 */
static bool
lion_count_path_where(LionCountPathBuild *cx)
{
	PlannerInfo *root = cx->root;
	RelOptInfo *input_rel = cx->input_rel;
	Index		rti = cx->rti;
	ListCell   *lc;
	LionCountPathBuild snap;
	Bitmapset  *printed = lion_partial_active() ?
		lion_count_path_printed(cx) : NULL;

	/*
	 * ---- every WHERE clause must be one the posting sets can answer ----
	 *
	 * ... or, over a partitioned table, one the bounds of every partition
	 * counted imply (DESIGN.md §16, "Clauses the partition bounds imply"),
	 * which selects nothing there and is left out of the count.  Each clause
	 * remembers the restriction it came from (rinfono), so that a partition
	 * whose bounds imply that restriction can leave out all of it
	 * (lion_collect_targets()).
	 */
	foreach(lc, input_rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
		Node	   *clause;
		LionLeafInfo leaf;
		int			startlen = list_length(cx->whereattnos);
		bool		answered;
		List	   *ands;

		if (!IsA(rinfo, RestrictInfo) || rinfo->pseudoconstant)
			return false;

		/* TRUE wherever the count reads (DESIGN.md §45): left out */
		if (lion_count_path_pred_drops(cx, rinfo, printed))
			continue;

		cx->rinfono++;
		cx->rinfoclauses = lappend(cx->rinfoclauses, rinfo->clause);

		/*
		 * Over a partitioned table, a restriction some partition could
		 * neither answer nor leave out in the attempt before is residual in
		 * this one, without analysis (DESIGN.md §16, "Residual clauses over
		 * a partitioned table"); and every restriction that could be is
		 * noted, for lion_collect_targets() to ask for.
		 */
		if (cx->partitioned && lion_residual_ok(cx, rinfo))
		{
			cx->residable = bms_add_member(cx->residable, cx->rinfono);
			if (bms_is_member(cx->rinfono, cx->forceresid))
			{
				cx->residual = lappend(cx->residual, rinfo->clause);
				continue;
			}
		}
		lion_count_path_snapshot(cx, &snap);

		/* a clause of an expression column is one of a column (§41) */
		clause = lion_vcol_subst((Node *) rinfo->clause);

		/*
		 * `flag IS NOT TRUE` of a boolean column is `flag = false OR flag IS
		 * NULL`, and is an OR like any other from here on
		 * (lion_boolean_not_test()).
		 */
		{
			BoolExpr   *orform = lion_boolean_not_test(clause);
			Node	   *anyform;

			if (orform != NULL)
				clause = (Node *) orform;

			/*
			 * `tsv @@ ANY (array)` is the OR of its elements' clauses
			 * (lion_multikey_any_as_or()).
			 */
			else if ((anyform = lion_multikey_any_as_or(clause)) != NULL)
				clause = anyform;
		}

		/*
		 * An OR of equalities on one column with constants is the IN list it
		 * spells (lion_or_as_array(), DESIGN.md §29.11, "An OR of equalities
		 * is its IN list"): one source, located, priced and counted as the
		 * list is - its values looked up in one walk of the directory, the
		 * disjoint sum when it is the only source, its batches past the
		 * budget - rather than a union of leaves looked up one by one.  The
		 * list is only taken when the posting sets answer it as a list; an
		 * OR whose operator is not the column's equality stays an OR.
		 */
		if (IsA(clause, BoolExpr) && ((BoolExpr *) clause)->boolop == OR_EXPR)
		{
			Node	   *arr = lion_or_as_array(clause);

			if (arr != NULL &&
				lion_analyze_leaf(root, arr, rti, true, true, true, &leaf) &&
				leaf.kind == LION_CLAUSE_ARRAY)
				clause = arr;
		}

		/*
		 * An OR across columns (lion_count_path_where_or()), `col NOT IN
		 * (...)` as its `<>` clauses (lion_all_as_and()), or a clause of one
		 * column (lion_count_path_where_leaf()).
		 */
		if (IsA(clause, BoolExpr) && ((BoolExpr *) clause)->boolop == OR_EXPR)
			answered = lion_count_path_where_or(cx, clause);
		else if ((ands = lion_all_as_and(clause)) != NIL)
		{
			ListCell   *la;

			answered = true;
			foreach(la, ands)
			{
				Node	   *conj = (Node *) lfirst(la);

				if (!lion_analyze_leaf(root, conj, rti, true, true, true,
									   &leaf) ||
					leaf.kind != LION_CLAUSE_NE ||
					!lion_count_path_where_leaf(cx, rinfo, conj))
				{
					answered = false;
					break;
				}
			}
		}
		else
			answered = lion_count_path_where_leaf(cx, rinfo, clause);
		/*
		 * ... on a column no lion index covers, where it may be residual
		 * (below) rather than decline the query when the indexes are
		 * matched.  Over a partitioned table an OR is left as answered: its
		 * arms may be what the partitions' bounds refute or imply, which
		 * narrows it to clauses their indexes answer (DESIGN.md §16, "OR
		 * arms the partition bounds refute"), and where they cannot, the
		 * partitions ask for it to be residual (lion_collect_targets()).
		 */
		if (answered &&
			((lion_enable_count_residual && cx->fj == NULL &&
			  !(cx->partitioned && IsA(clause, BoolExpr) &&
				((BoolExpr *) clause)->boolop == OR_EXPR)) || cx->cells) &&
			!lion_where_indexed(cx, startlen))
			answered = false;
		if (answered)
		{
			lion_count_path_snapshot_free(&snap);
			continue;
		}

		/*
		 * Built as a count by cells (DESIGN.md §10, "Counts by cells"), a
		 * clause no posting set answers is one over predicates the cells
		 * tell apart - `CASE ... END = 'high'` - or none at all: tested once
		 * a cell, by the plan's qual, and never on a row.
		 */
		if (cx->cells)
		{
			List	   *cellquals;

			if (!lion_cells_shape_ok(root, input_rel, rti,
									 (Node *) rinfo->clause))
			{
				lion_count_path_snapshot_free(&snap);
				return false;
			}
			cellquals = lappend(cx->cellquals, rinfo->clause);
			lion_count_path_restore(cx, &snap);
			cx->cellquals = cellquals;
			continue;
		}

		/*
		 * No posting set answers the clause.  Over a partitioned table
		 * whose every partition's bounds imply it, no row counted can fail
		 * it, and it is left out of the plan altogether (DESIGN.md §16,
		 * "Clauses the partition bounds imply").  Whatever of it was taken
		 * already goes too, all of it: the first arms of an OR, and an OR
		 * the posting sets answer but for being one complement OR too many,
		 * whose entry in `ors` would otherwise make the clauses after it its
		 * arms.
		 */
		if (cx->partitioned &&
			lion_implied_everywhere(root, input_rel, input_rel,
									(Node *) rinfo->clause))
		{
			List	   *implied =
				lappend(cx->impliedtexts,
						makeString(lion_deparse_rel_clause(root, rti,
														   (Node *) rinfo->clause)));

			lion_count_path_restore(cx, &snap);
			cx->impliedtexts = implied;
			continue;
		}

		/*
		 * Otherwise it may be RESIDUAL (DESIGN.md §10, "Residual clauses"):
		 * tested on the heap tuple of each row the other clauses select -
		 * of the table, or of each partition whose bounds do not imply it
		 * (§16) - whatever of it was taken already (the first arms of an
		 * OR) undone.  Whether the query may then be counted at all is
		 * lion_count_path_strategy()'s to say, and whether it should, the
		 * price's.
		 */
		if (lion_residual_ok(cx, rinfo))
		{
			List	   *residual = lappend(cx->residual, rinfo->clause);

			lion_count_path_restore(cx, &snap);
			cx->residual = residual;
			continue;
		}

		/* ... and otherwise it declines the query */
		lion_count_path_snapshot_free(&snap);
		return false;
	}

	/* every phase after this one reads the ORs by the runs they name */
	lion_ors_check(cx->ors, cx->clauseinfos);
	return true;
}

/*
 * May a run of the WHERE find nothing to take its candidates from, and
 * count every row the heap holds (lion_count_scan_filtered()), beside a
 * clause that scan does not test?  A multi-key query known only at run
 * time may narrow nothing (DESIGN.md §17), and so may a literal one with
 * prefixes, past what the run can expand (§17, "Prefix lexemes"; §43).  A
 * clause beside it positive whatever its value is a source in every run,
 * and so is the walk that drives each count of a GROUP BY or a
 * count(DISTINCT) - not the decoded walk's (decode), which reads the WHERE
 * whole.  The scan tests every negated clause of a heap column
 * (lion_filter_with_negated()) and makes one count: none on an expression
 * column (§41), and no OR with a complement arm (§35), which is two counts.
 */
static bool
lion_where_scan_untested(const LionCountPathBuild *cx, bool decode)
{
	if (!(cx->runtimemulti || cx->prefixmulti) || cx->plainpositive)
		return false;
	if (!decode && (cx->ngroup > 0 || cx->distvar != NULL))
		return false;
	return cx->ncompl > 0 || cx->exprnegated;
}

/*
 * The FK-side join (DESIGN.md §27) takes it from here: false once it has,
 * true when there is none and the count goes on.
 */
static bool
lion_count_path_fkjoin(LionCountPathBuild *cx)
{
	PlannerInfo *root = cx->root;
	RelOptInfo *input_rel = cx->input_rel;
	RelOptInfo *output_rel = cx->output_rel;
	GroupPathExtraData *extra = cx->extra;
	const LionFkJoin *fj = cx->fj;

	/*
	 * An OR with a complement arm is two counts (DESIGN.md §35), which the
	 * join's collected fact filters have no place for; nor do its counts
	 * have a fallback beside a multi-key query that may narrow nothing for
	 * a negated clause on an expression column (lion_where_scan_untested()).
	 *
	 * Every FK-side join is held to the same terms - the dimension LEFT JOIN
	 * the fact and a join with dimensions folded into fact filters ("A
	 * second dimension that only filters") as much as an inner join of two
	 * tables: no complement OR among the fact filters (here), no residual
	 * clause (lion_residual_ok() takes none beside a join, so a fact clause
	 * no posting set answers declines the join), and no filtered count
	 * (lion_count_path_filters() declines the join).  A folded dimension's
	 * own filters are its child's, core's; what the node adds for it is an
	 * IN list, a positive clause like any.  countcombos.sql shows each
	 * combination declined, with the ordinary plan's answer.
	 */
	if (fj != NULL &&
		(cx->ncompl > 0 || lion_where_scan_untested(cx, cx->decode)))
		return false;

	/*
	 * The FK-side join's fact filters are all sources, ANDed with every fk
	 * set, so every range among them is a range taken as a source (DESIGN.md
	 * §32); the fk set of each dimension row is what carries the §9 pin.
	 */
	if (fj != NULL && cx->rangevars != NIL)
	{
		lion_ranges_to_sources(cx->rangevars, cx->rangepos, -1,
							   cx->wherekinds, cx->clauseinfos);
		cx->hasrangesrc = true;
		cx->havepositive = true;
	}

	/*
	 * ---- the FK-side join (DESIGN.md §27) ----
	 *
	 * The fact rel's clauses are all ones the posting sets answer; the join
	 * key, the target list and the path are the join's own business.
	 */
	if (fj != NULL)
	{
		/* the fk key's set of each dimension row drives every count */
		cx->imply.toprel = input_rel;
		cx->imply.clauses = cx->rinfoclauses;
		cx->imply.driven = true;
		cx->imply.skipped = cx->impliedtexts;
		cx->imply.ors = cx->ors;
		if (fj->joinpath)
			lion_try_semijoin_path(root, input_rel, fj, cx->whereattnos,
								   cx->clauseinfos, cx->whereclauses,
								   cx->whereconsts, cx->wherekinds,
								   cx->whereopnos, cx->whereinor, cx->ors,
								   cx->partitioned ? &cx->imply : NULL);
		else
			lion_try_fkjoin_path(root, input_rel, output_rel, extra, fj,
								 cx->having, cx->whereattnos, cx->clauseinfos,
								 cx->whereclauses, cx->whereconsts,
								 cx->wherekinds, cx->whereopnos,
								 cx->whereinor, cx->ors,
								 cx->partitioned ? &cx->imply : NULL);
		return false;
	}
	return true;
}

/*
 * The expressions the node has to produce, or test (checkexprs): the grouped
 * relation's target, and the aggregates and columns the HAVING mentions.
 */
static bool
lion_count_path_having(LionCountPathBuild *cx)
{
	RelOptInfo *output_rel = cx->output_rel;

	/* ---- the grouped relation's target, and the HAVING that filters it ---- */
	cx->checkexprs = list_copy(output_rel->reltarget->exprs);
	if (cx->having != NIL)
	{
		/*
		 * A SubPlan would need the node to run a subquery per group; an
		 * uncorrelated one is an InitPlan by now and arrives as a Param,
		 * which is a plain value here.
		 */
		if (contain_subplans((Node *) cx->having))
			return false;
		cx->checkexprs =
			list_concat(cx->checkexprs,
						pull_var_clause(lion_vcol_subst((Node *) cx->having),
										PVC_INCLUDE_AGGREGATES |
										PVC_RECURSE_WINDOWFUNCS |
										PVC_INCLUDE_PLACEHOLDERS));
	}

	/*
	 * An expression column in the target list is a column the node prints,
	 * and in an aggregate's argument one it counts (DESIGN.md §41); the
	 * HAVING's were replaced before its columns were taken out of it.
	 */
	cx->checkexprs = (List *) lion_vcol_subst((Node *) cx->checkexprs);

	/*
	 * An expression over what the node produces - `count(*)::int`, `100.0 *
	 * count(*) / n`, `round(100.0 * count(*) / n, 2)` beside a GROUP BY, `g ||
	 * '!'` of a group column - is computed by the plan's projection from the
	 * counts and columns it reads, once per row the node emits, as the Agg's
	 * projection computes it once per group (DESIGN.md §10, "Expressions over
	 * the counts").  So what has to pass the tests below is what it reads:
	 * its aggregates whole, and its columns.  A subquery in it would be a
	 * SubPlan the node's projection runs, which it is not built to; a window
	 * function is computed above the grouping, never in its target.
	 */
	{
		List	   *expanded = NIL;
		ListCell   *lc;

		foreach(lc, cx->checkexprs)
		{
			Node	   *node = (Node *) lfirst(lc);

			if (IsA(node, Var) || IsA(node, Aggref) ||
				(cx->groupcoal != NULL && equal(node, cx->groupexpr)) ||
				list_member(cx->groupdeps, node))
			{
				expanded = lappend(expanded, node);
				continue;
			}
			if (contain_subplans(node) || contain_window_function(node))
				return false;
			expanded = list_concat(expanded,
								   pull_var_clause(node,
												   PVC_INCLUDE_AGGREGATES |
												   PVC_INCLUDE_PLACEHOLDERS));
		}
		cx->checkexprs = expanded;
	}
	return true;
}

/*
 * count(DISTINCT k): the one column the distinct aggregates walk (distvar,
 * disteqop, distcoll), if any, and whether it can be walked here.
 */
static bool
lion_count_path_distinct(LionCountPathBuild *cx)
{
	PlannerInfo *root = cx->root;
	RelOptInfo *input_rel = cx->input_rel;
	Index		rti = cx->rti;
	List	   *uniqattnos = NIL;	/* columns of a count(DISTINCT) proved
									 * unique: a count(col) each (§26) */
	int			g;
	ListCell   *lc;

	/*
	 * ---- count(DISTINCT k) (DESIGN.md §26) ----
	 *
	 * Found first, because it decides what drives the scan: k's own entries
	 * when there is no GROUP BY, and the inner side of the (g, k) nested loop
	 * when there is one.  Every distinct aggregate has to be over the same
	 * plain column, with the same equality and collation - one k, one walk -
	 * and k may be neither a grouping column (its entries would be groups and
	 * distinct values at once) nor the third driving column of a two-column
	 * GROUP BY.  A partitioned table declines: distinct counts do not add up
	 * across partitions, and the partial aggregates of §16 would add them.
	 */
	foreach(lc, cx->checkexprs)
	{
		Aggref	   *agg = (Aggref *) lfirst(lc);
		Var		   *dv;
		Oid			deq;
		Oid			dcoll;

		if (!IsA(agg, Aggref) || agg->aggdistinct == NIL)
			continue;
		if (!lion_agg_distinct_var(agg, rti, &dv, &deq, &dcoll))
			return false;

		/*
		 * A column the relation proves unique - a primary key, say - is not
		 * k at all: its distinct count is the count(col) of the same rows
		 * (lion_agg_distinct_unique()), answered wherever that count(col) is
		 * - which is every row count when col is known non-NULL, and the
		 * other cases of DESIGN.md §14.  Nothing is walked for it, so neither
		 * partitions nor a second distinct column stand in its way.  One
		 * whose count(col) the node cannot answer (a nullable unique column
		 * nothing proves non-NULL) is left to be walked as k, as before.
		 */
		if (lion_column_is_unique(input_rel, dv->varattno, deq, dcoll) &&
			lion_agg_is_count(root, agg, rti, input_rel, cx->groupattno,
							  cx->ngroup, cx->nonnullattnos, cx->nullattnos))
		{
			uniqattnos = list_append_unique_int(uniqattnos,
												(int) dv->varattno);
			continue;
		}
		if (cx->distvar == NULL)
		{
			cx->distvar = dv;
			cx->disteqop = deq;
			cx->distcoll = dcoll;
		}
		else if (dv->varattno != cx->distvar->varattno ||
				 deq != cx->disteqop || dcoll != cx->distcoll)
			return false;
	}
	if (cx->distvar != NULL)
	{
		/*
		 * The plan tells the two kinds apart by their column alone
		 * (lion_plan_custom_path()), so one column is not both: a distinct
		 * count proved unique under one collation beside a walked one under
		 * another is left to the ordinary plan.
		 */
		if (list_member_int(uniqattnos, (int) cx->distvar->varattno))
			return false;
		if (cx->partitioned || cx->ngroup > 1)
			return false;
		/* a coalesce group is §10's walk, not the (g, k) loop of §26 */
		if (cx->groupcoal != NULL)
			return false;
		for (g = 0; g < cx->ngroup; g++)
		{
			if (cx->groupattno[g] == cx->distvar->varattno)
				return false;
		}
	}
	return true;
}

/*
 * The columns of count(col)s that the predicate of the partial index the
 * count is planned with proves non-NULL (DESIGN.md §45), as nonnullattnos:
 * the restrictions imply that predicate in every relation counted, so every
 * row the query selects has col, and count(col) is the rows' count.  Before
 * lion_count_path_countcol() and the target list's checks, which ask.
 */
static void
lion_count_path_pred_nonnull(LionCountPathBuild *cx)
{
	ListCell   *lc;

	if (!lion_partial_active())
		return;
	foreach(lc, cx->checkexprs)
	{
		Aggref	   *agg = (Aggref *) lfirst(lc);
		Var		   *v;
		NullTest   *nt;

		if (!IsA(agg, Aggref) || agg->aggfnoid != F_COUNT_ANY ||
			list_length(agg->args) != 1)
			continue;
		v = (Var *) lion_strip((Node *) ((TargetEntry *) linitial(agg->args))->expr);
		if (v == NULL || !IsA(v, Var) || v->varno != (int) cx->rti ||
			v->varlevelsup != 0 || v->varattno <= 0 ||
			LION_ATTNO_IS_VCOL(v->varattno) ||
			list_member_int(cx->nonnullattnos, (int) v->varattno))
			continue;
		nt = makeNode(NullTest);
		nt->arg = (Expr *) copyObject(v);
		nt->nulltesttype = IS_NOT_NULL;
		nt->argisrow = false;
		nt->location = -1;
		if (lion_partial_drops(cx->root, cx->input_rel, (Node *) nt))
			cx->nonnullattnos = lappend_int(cx->nonnullattnos,
											(int) v->varattno);
	}
}

/*
 * count(col) of a nullable column (DESIGN.md §10, "Shapes rewritten to the
 * node's counts"): without a GROUP BY, where every aggregate is count(col)
 * of that one column, it is count(*) of the rows the WHERE selects AND `col
 * IS NOT NULL` - a clause the posting sets answer (§14, §35), added here as
 * if the query had written it.  A GROUP BY is not taken: a group whose rows
 * all have col NULL is a row of 0 in the query and no row at all once the
 * clause is added.  Nothing is done, and the checks after this decide, when
 * the count is one lion_agg_is_count() already answers or the target holds
 * anything else.
 */
static bool
lion_count_path_countcol(LionCountPathBuild *cx)
{
	Var		   *col = NULL;
	NullTest   *nt;
	ListCell   *lc;

	lion_count_path_pred_nonnull(cx);
	if (cx->fj != NULL || cx->ngroup != 0 || cx->singlegroup ||
		cx->groupcoal != NULL || cx->distvar != NULL)
		return true;
	foreach(lc, cx->checkexprs)
	{
		Aggref	   *agg = (Aggref *) lfirst(lc);
		Node	   *arg;

		if (!IsA(agg, Aggref))
			continue;
		if (agg->aggfnoid != F_COUNT_ANY || agg->aggdistinct != NIL ||
			agg->aggorder != NIL || agg->aggfilter != NULL ||
			agg->aggvariadic || agg->agglevelsup != 0 ||
			agg->aggsplit != AGGSPLIT_SIMPLE ||
			agg->aggkind != AGGKIND_NORMAL || list_length(agg->args) != 1)
			return true;
		arg = lion_strip((Node *) ((TargetEntry *) linitial(agg->args))->expr);
		if (arg == NULL || !IsA(arg, Var) ||
			((Var *) arg)->varno != (int) cx->rti ||
			((Var *) arg)->varattno <= 0 || ((Var *) arg)->varlevelsup != 0)
			return true;
		if (col == NULL)
			col = (Var *) arg;
		else if (col->varattno != ((Var *) arg)->varattno)
			return true;
		if (lion_agg_is_count(cx->root, agg, cx->rti, cx->input_rel,
							  cx->groupattno, cx->ngroup, cx->nonnullattnos,
							  cx->nullattnos))
			return true;
	}
	if (col == NULL)
		return true;

	nt = makeNode(NullTest);
	nt->arg = (Expr *) copyObject(col);
	nt->nulltesttype = IS_NOT_NULL;
	nt->argisrow = false;
	nt->location = -1;
	cx->rinfono++;
	cx->rinfoclauses = lappend(cx->rinfoclauses,
							   lion_vcol_unvar((Node *) nt));

	/* a row the anti join's lookups find is counted only with it (§27) */
	cx->antiextra = lappend(cx->antiextra, lion_vcol_unvar((Node *) nt));
	return lion_count_path_where_leaf(cx, NULL, (Node *) nt);
}

/*
 * An aggregate over the entries of lion column attno (DESIGN.md §37): taken
 * over one table with no WHERE and no GROUP BY, where every entry of a
 * scalar column is its rows' value, and from a whole index on the column
 * whose stored keys are the rows' own values (the value rule of §10) - the
 * argument is evaluated on the key.  Notes the column, its index and its
 * aggregates.
 */
static bool
lion_count_path_wagg(LionCountPathBuild *cx, AttrNumber attno)
{
	IndexOptInfo *idx;
	AttrNumber	col;
	Oid			atttype;
	ListCell   *lc;
	int			i = 0;

	if (cx->fj != NULL || cx->ngroup != 0 || cx->singlegroup ||
		cx->partitioned || cx->distvar != NULL || cx->groupcoal != NULL ||
		cx->input_rel->baserestrictinfo != NIL ||
		LION_ATTNO_IS_VCOL(attno))
		return false;
	foreach(lc, cx->wattnos)
	{
		if ((AttrNumber) lfirst_int(lc) == attno)
		{
			lfirst_int(list_nth_cell(cx->wnaggs, i))++;
			return true;
		}
		i++;
	}
	idx = lion_find_roaring_index(cx->input_rel, attno, false, &col);
	if (idx == NULL || idx->indpred != NIL ||
		!lion_index_can_emit_value(idx, col))
		return false;

	/* the key is the column's value in the column's own representation */
	atttype = get_atttype(cx->rte->relid, attno);
	if (!IsBinaryCoercible(atttype, idx->opcintype[col - 1]) ||
		get_typlen(atttype) != get_typlen(idx->opcintype[col - 1]) ||
		get_typbyval(atttype) != get_typbyval(idx->opcintype[col - 1]))
		return false;
	cx->wattnos = lappend_int(cx->wattnos, (int) attno);
	cx->widx = lappend(cx->widx, idx);
	cx->wcols = lappend_int(cx->wcols, (int) col);
	cx->wnaggs = lappend_int(cx->wnaggs, 1);
	return true;
}

/*
 * The whole lion index of rel slicing column attno - the first in its
 * list, lion_find_sliced_index()'s - and its key column and the type it
 * slices the column as, or NULL; for a partitioned rel, its first live
 * leaf's, attno translated through every level (lion_count_first_leaf()):
 * what the type of the aggregates of a partitioned table's sliced column is
 * taken from, which every partition is then held to
 * (lion_count_path_slice_parts()).
 */
static IndexOptInfo *
lion_count_sliced_index(const LionCountPathBuild *cx, AttrNumber attno,
						AttrNumber *colp, Oid *typid)
{
	RelOptInfo *rel = cx->input_rel;

	if (cx->partitioned)
	{
		AttrNumber	a = attno;

		rel = lion_count_first_leaf(cx->root, rel, &a, 1);
		if (rel == NULL || a == 0)
			return NULL;
		attno = a;
	}
	return lion_find_sliced_index(rel, attno, colp, typid);
}

/*
 * lion_slice_range_ok() of the table the build counts (DESIGN.md §53): a
 * partitioned table's first live leaf's, attno translated as above - every
 * leaf's is asked once the partitions are known
 * (lion_count_path_slice_range_parts()).
 */
static bool
lion_count_slice_range_ok(const LionCountPathBuild *cx, AttrNumber attno,
						  Oid cmptype)
{
	RelOptInfo *rel = cx->input_rel;

	if (cx->partitioned)
	{
		AttrNumber	a = attno;

		rel = lion_count_first_leaf(cx->root, rel, &a, 1);
		if (rel == NULL || a == 0)
			return false;
		attno = a;
	}
	return lion_slice_range_ok(rel, attno, cmptype);
}

/*
 * An aggregate of sliced column attno answered from its slices (DESIGN.md
 * §52), of kind skind taking the column as argtype (lion_sagg_classify()):
 * over one table, with no GROUP BY or one of one or two lion-indexed columns
 * walked by their entries, or over a partitioned one with no GROUP BY (§52,
 * "Partitioned tables"), and from a whole lion index that slices the column
 * as that type (lion_find_sliced_index(); a partitioned table's first live
 * leaf's here, every leaf's once the partitions are known).  Notes the
 * column, its index and what its aggregates ask of the slices.
 */
static bool
lion_count_path_sagg(LionCountPathBuild *cx, AttrNumber attno, int skind,
					 Oid argtype)
{
	IndexOptInfo *idx;
	AttrNumber	col;
	Oid			typid;
	int			want;
	ListCell   *lc;
	int			i = 0;

	if (cx->fj != NULL || cx->anti != NULL || cx->ngroup > 2 ||
		cx->singlegroup || (cx->partitioned && cx->ngroup > 0) ||
		cx->distvar != NULL ||
		cx->groupcoal != NULL || cx->cells || LION_ATTNO_IS_VCOL(attno))
		return false;

	want = (skind == LION_SAGG_SUM || skind == LION_SAGG_SUM8 ||
			skind == LION_SAGG_AVG || skind == LION_SAGG_AVG8) ?
		LION_SWANT_SUM :
		(skind == LION_SAGG_MIN) ? LION_SWANT_MIN :
		(skind == LION_SAGG_MAX) ? LION_SWANT_MAX : 0;
	foreach(lc, cx->sattnos)
	{
		if ((AttrNumber) lfirst_int(lc) == attno)
		{
			if (OidIsValid(argtype) &&
				argtype != list_nth_oid(cx->stypes, i))
				return false;
			lfirst_int(list_nth_cell(cx->swants, i)) |= want;
			return true;
		}
		i++;
	}
	idx = lion_count_sliced_index(cx, attno, &col, &typid);
	if (idx == NULL || (OidIsValid(argtype) && argtype != typid))
		return false;
	cx->sattnos = lappend_int(cx->sattnos, (int) attno);
	cx->sidx = lappend(cx->sidx, idx);
	cx->scols = lappend_int(cx->scols, (int) col);
	cx->stypes = lappend_oid(cx->stypes, typid);
	cx->swants = lappend_int(cx->swants, want);
	return true;
}

/*
 * The aggregates of sliced columns beside the rest of the plan (DESIGN.md
 * §52): their slices are counted against the WHERE collected whole, E ∧
 * WHERE, which every shape of WHERE a count of one row takes collects - a
 * range bounding the sum over its entries is collected for it - except a
 * complement OR, which is two counts, a multi-key query the run may find
 * narrows nothing or expands too far (its candidates every row's), and a
 * residual clause, every count of which would test every row in the heap.
 * Nothing is collected on a hot standby, so nothing is planned there.  And
 * the aggregates over keys of §37 are another build's.
 *
 * Beside a GROUP BY - one column's entries walked, or two's pairs (§20) -
 * every group's rows are that WHERE copy ANDed with the group's sets: not
 * beside filtered counts, whose runs go up as partial rows a Finalize Agg
 * adds up (an average or an extreme of them would be no partial state), the
 * top k, which counts a column's entries' own counts, or the decoded walk.
 */
static bool
lion_count_path_slices_ok(LionCountPathBuild *cx)
{
	if (!cx->slicemode)
		return true;
	return cx->sattnos != NIL && cx->wattnos == NIL &&
		cx->ncompl == 0 && !cx->runtimemulti && !cx->prefixmulti &&
		cx->residual == NIL && cx->topkn == 0 && !cx->decode &&
		cx->distvar == NULL && cx->ngroup <= 2 &&
		!(cx->partitioned && cx->ngroup > 0) &&
		(cx->ngroup == 0 || cx->nfilter == 0) &&
		!RecoveryInProgress();
}

/*
 * The column top numbers attno, as leaf - a partition under it, at any
 * level - numbers it (lion_leaf_expr()), or 0 where leaf has no such
 * column.
 */
static AttrNumber
lion_count_leaf_attno(LionCountPathBuild *cx, RelOptInfo *leaf,
					  AttrNumber attno)
{
	Oid			typid;
	int32		typmod;
	Oid			collid;
	Node	   *n;

	if (leaf == cx->input_rel)
		return attno;
	get_atttypetypmodcoll(cx->rte->relid, attno, &typid, &typmod, &collid);
	n = lion_leaf_expr(cx->root,
					   (Node *) makeVar(cx->input_rel->relid, attno, typid,
										typmod, collid, 0),
					   leaf, cx->input_rel);
	n = lion_strip(n);
	return (n != NULL && IsA(n, Var)) ? ((Var *) n)->varattno : 0;
}

/*
 * The aggregates of sliced columns over a partitioned table (DESIGN.md §52,
 * "Partitioned tables"): each partition the node counts answers them from
 * its own slices, so each has to have a whole lion index that slices each
 * column - its own first, as one table's is found (lion_find_sliced_index()),
 * the column as that partition numbers it - and as the one type the build
 * took from the first leaf (stypes), at that type's width: a partition that
 * has none, or slices the column as another type (a domain's base where the
 * others slice the column's own, say), declines the query, which then has
 * the ordinary plan or the counts' alone.  Notes each target's indexes.
 */
static bool
lion_count_path_slice_parts(LionCountPathBuild *cx)
{
	ListCell   *lc;
	int			n = list_length(cx->sattnos);

	if (!cx->partitioned || n == 0)
		return true;
	foreach(lc, cx->targets)
	{
		LionCountTarget *t = (LionCountTarget *) lfirst(lc);
		ListCell   *l1;
		ListCell   *l2;
		int			i = 0;

		t->sliceidx = (IndexOptInfo **) palloc0(sizeof(IndexOptInfo *) * n);
		t->slicecol = (AttrNumber *) palloc0(sizeof(AttrNumber) * n);
		forboth(l1, cx->sattnos, l2, cx->stypes)
		{
			AttrNumber	attno = lion_count_leaf_attno(cx, t->rel,
													  (AttrNumber) lfirst_int(l1));
			AttrNumber	col = 0;
			Oid			typid = InvalidOid;
			IndexOptInfo *idx;

			if (attno == 0)
				return false;
			idx = lion_find_sliced_index(t->rel, attno, &col, &typid);
			if (idx == NULL || typid != lfirst_oid(l2))
				return false;
			t->sliceidx[i] = idx;
			t->slicecol[i] = col;
			i++;
		}
	}
	return true;
}

/*
 * The ranges a build answers from slices over a partitioned table (DESIGN.md
 * §53, §52.13): the clauses were taken from the first live leaf's index
 * (lion_count_slice_range_ok()), and each partition the node counts reads
 * the index its target matched them to - which has to slice the column at
 * its type's width, and take the bounds' type, too.  The executor would
 * collect a range from the entries of a partition whose index does not
 * (lion_rangesrc_range(), as after a REINDEX), the same rows, but the build
 * is priced as from slices in every partition: one that cannot is the build
 * without them, which is offered beside it, and this one is declined.  A
 * partition that leaves the clause out, its bounds implying it (§16), reads
 * nothing for it.
 */
static bool
lion_count_path_slice_range_parts(LionCountPathBuild *cx)
{
	ListCell   *lc;

	if (!cx->partitioned || cx->nsliced == 0)
		return true;
	foreach(lc, cx->targets)
	{
		LionCountTarget *t = (LionCountTarget *) lfirst(lc);
		ListCell   *l1;
		ListCell   *l2;
		ListCell   *l3;

		forthree(l1, cx->clauseinfos, l2, t->whereidx, l3, t->wherecol)
		{
			LionClauseInfo *ci = (LionClauseInfo *) lfirst(l1);
			IndexOptInfo *idx = (IndexOptInfo *) lfirst(l2);
			IndexOptInfo *sidx;
			AttrNumber	attno;
			AttrNumber	col = 0;
			Oid			typid = InvalidOid;

			if (!ci->sliced || ci->kind != LION_CLAUSE_RANGESRC ||
				idx == NULL)
				continue;
			attno = lion_count_leaf_attno(cx, t->rel, ci->attno);
			if (attno == 0)
				return false;
			sidx = lion_range_sliced_index(t->rel, attno, &col, &typid);
			if (sidx == NULL || sidx->indexoid != idx->indexoid ||
				col != (AttrNumber) lfirst_int(l3) ||
				(OidIsValid(ci->cmptype) &&
				 !lion_slice_range_type_ok(typid, ci->cmptype)))
				return false;
		}
	}
	return true;
}

/*
 * Every expression of checkexprs has to be one the node produces: a grouping
 * column, a column a clause pins, or a count the posting sets answer - and
 * there has to be a count.  Notes the columns printed (groupvalueout,
 * valueattnos) and whether the distinct walk counts (distcounts).
 */
static bool
lion_count_path_outputs(LionCountPathBuild *cx)
{
	PlannerInfo *root = cx->root;
	RelOptInfo *input_rel = cx->input_rel;
	Index		rti = cx->rti;
	bool		haveagg = false;
	int			g;
	ListCell   *lc;

	foreach(lc, cx->checkexprs)
	{
		Node	   *node = (Node *) lfirst(lc);

		if (IsA(node, Var))
		{
			Var		   *v = (Var *) node;

			if (v->varno != (int) rti || v->varattno <= 0 ||
				v->varlevelsup != 0)
				return false;

			/*
			 * Under GROUP BY coalesce(g, c) the group's value is the whole
			 * expression, which the node prints, and never g by itself: a
			 * bare g can only reach here out of a HAVING that takes the
			 * expression apart, and the key the node holds for the merged
			 * group is not g's value in all of its rows.
			 */
			if (cx->groupcoal != NULL && v->varattno == cx->groupattno[0])
				return false;

			/*
			 * The column has to be either the group key or one a clause pins
			 * to a single value.  In the second case the value we output is
			 * the key the index stored, not the constant from the query: a
			 * cross-type or otherwise non-identical constant that compares
			 * equal must not change what the query prints.  A column pinned
			 * to NULL by `IS NULL` prints NULL, which the stored key of the
			 * reserved entry says as well.
			 *
			 * Either way the value comes out of an index entry, so the index
			 * has to be one whose entries can produce it - which is decided
			 * per relation, once the indexes are known.  `IS NULL` is exempt:
			 * NULL has one representation.
			 */
			for (g = 0; g < cx->ngroup; g++)
			{
				if (v->varattno == cx->groupattno[g])
					break;
			}
			if (g < cx->ngroup)
				cx->groupvalueout[g] = true;
			else if (list_member_int(cx->eqattnos, (int) v->varattno))
			{
				if (!list_member_int(cx->valueattnos, (int) v->varattno))
					cx->valueattnos = lappend_int(cx->valueattnos,
												  (int) v->varattno);
			}
			else if (!list_member_int(cx->nullattnos, (int) v->varattno))
				return false;
		}
		else if (cx->groupcoal != NULL && equal(node, cx->groupexpr))
		{
			/* the coalesce group's value, printed as the node emits it */
			cx->groupvalueout[0] = true;
		}
		else if (list_member(cx->groupdeps, node))
		{
			/* an expression of the grouping columns, computed from them */
		}
		else if (IsA(node, Aggref))
		{
			Aggref	   *agg = (Aggref *) node;
			AttrNumber	countcols[LION_MAX_GROUPCOLS + 1];
			int			ncountcols = cx->ngroup;
			Node	   *arg = (agg->args != NIL) ?
				lion_strip((Node *) ((TargetEntry *) linitial(agg->args))->expr) :
				NULL;
			AttrNumber	argattno = (arg != NULL && IsA(arg, Var)) ?
				((Var *) arg)->varattno : 0;
			AttrNumber	wattno;
			Node	   *fexpr;
			Var		   *fcol;
			int			form;

			/*
			 * A filtered count (DESIGN.md §10, "Filtered counts"): its
			 * condition is one more set of clauses, analysed once the WHERE's
			 * strategy is known (lion_count_path_filters()); a count(col) of
			 * it needs col non-NULL in every row it counts, which that
			 * proves or not.
			 */
			if (lion_agg_filtered(agg, &fexpr, &form, &fcol))
			{
				int			f;

				if (agg->aggsplit != AGGSPLIT_SIMPLE)
					return false;
				f = lion_count_path_filter_no(cx, fexpr);
				if (fcol != NULL)
				{
					if (fcol->varno != (int) rti || fcol->varattno <= 0 ||
						fcol->varlevelsup != 0)
						return false;
					cx->filtercols = lappend(cx->filtercols,
											 list_make2_int(f,
															(int) fcol->varattno));
				}
				haveagg = true;
				cx->wcounts = true;
				continue;
			}

			/*
			 * A count, sum, average, minimum or maximum of a bit-sliced
			 * column, from its slices (DESIGN.md §52), in the build that
			 * takes them so.
			 */
			if (cx->slicemode)
			{
				AttrNumber	sattno;
				Oid			argtype;
				int			skind = lion_sagg_classify(agg, rti, &sattno,
													   &argtype);

				if (skind != LION_SAGG_NONE &&
					lion_count_path_sagg(cx, sattno, skind, argtype))
				{
					haveagg = true;
					continue;
				}
			}

			/*
			 * A sum, an average, a minimum or a maximum over the entries of
			 * one lion column, weighted by their rows (DESIGN.md §37).
			 */
			if (lion_wagg_classify(agg, rti, &wattno, NULL) != LION_WAGG_NONE)
			{
				if (!lion_count_path_wagg(cx, wattno))
					return false;
				haveagg = true;
				continue;
			}

			if (agg->aggdistinct != NIL && cx->distvar != NULL &&
				argattno == cx->distvar->varattno)
			{
				/* checked above; a distinct count needs no other test */
				haveagg = true;
				cx->wcounts = true;
				continue;
			}

			/*
			 * GROUP BY coalesce(g, c) counts g's NULL rows in c's group
			 * (DESIGN.md §10), where count(g) - or a count(DISTINCT g) proved
			 * to be it - counts only the rest, a number the node does not
			 * keep apart.  So it is answered only where no row it counts has
			 * g NULL: g declared NOT NULL, or a clause of the WHERE that says
			 * so, and then there is nothing to merge and it is the group's
			 * count.
			 */
			if (cx->groupcoal != NULL && argattno != 0 &&
				argattno == cx->groupattno[0] &&
				!list_member_int(cx->nonnullattnos, (int) argattno) &&
				!bms_is_member(argattno, lion_notnullattnums(root, input_rel)))
				return false;

			/*
			 * count(k) of the distinct column is answered too, whatever k's
			 * nullability: it is the rows of k's non-NULL entries, which the
			 * walk visits anyway (DESIGN.md §26) - so for this purpose k is
			 * one more column whose entries are known, like a group column.
			 */
			memcpy(countcols, cx->groupattno, sizeof(AttrNumber) * cx->ngroup);
			if (cx->distvar != NULL)
				countcols[ncountcols++] = cx->distvar->varattno;
			if (!lion_agg_is_count(root, agg, rti, input_rel,
								  countcols, ncountcols, cx->nonnullattnos,
								  cx->nullattnos))
				return false;
			haveagg = true;
			cx->wcounts = true;

			/*
			 * Does the walk have to COUNT rather than test for existence?
			 * Without a GROUP BY every other count is a sum over k's entries;
			 * beside one, only count(k) is a sum over the pairs, and count(*)
			 * and count(g) are the group's own count.
			 */
			if (cx->distvar != NULL)
			{
				bool		ofk = (argattno == cx->distvar->varattno);

				if (cx->ngroup == 0 || ofk)
					cx->distcounts = true;
			}
		}
		else
			return false;
	}
	/*
	 * No aggregate at all is a GROUP BY's or a DISTINCT's keys alone, which
	 * need a group to emit (lion_count_path_query()).
	 */
	if (!haveagg &&
		(cx->parse->hasAggs || (cx->ngroup == 0 && !cx->singlegroup)))
		return false;
	return true;
}

/*
 * The number of the filtered count whose condition is fexpr (DESIGN.md §10,
 * "Filtered counts"): 1 for the first condition met, and every condition
 * equal() to one already met shares its number - and its run.
 */
static int
lion_count_path_filter_no(LionCountPathBuild *cx, Node *fexpr)
{
	ListCell   *lc;
	int			f = 0;

	foreach(lc, cx->filterexprs)
	{
		f++;
		if (equal(lfirst(lc), fexpr))
			return f;
	}
	cx->filterexprs = lappend(cx->filterexprs, fexpr);
	cx->nfilter = list_length(cx->filterexprs);
	return cx->nfilter;
}

/*
 * One conjunct of a filtered count's condition (DESIGN.md §10, "Filtered
 * counts"), as lion_count_path_where() takes a restriction of the WHERE:
 * the same rewrites, then an OR, the conjunction of a `NOT IN`, or a clause
 * of one column.  False when no posting set answers it.
 */
static bool
lion_count_path_filter_clause(LionCountPathBuild *cx, Node *clause)
{
	PlannerInfo *root = cx->root;
	Index		rti = cx->rti;
	LionLeafInfo leaf;
	BoolExpr   *orform;
	Node	   *anyform;
	List	   *ands;

	if ((orform = lion_boolean_not_test(clause)) != NULL)
		clause = (Node *) orform;
	else if ((anyform = lion_multikey_any_as_or(clause)) != NULL)
		clause = anyform;

	if (IsA(clause, BoolExpr) && ((BoolExpr *) clause)->boolop == OR_EXPR)
	{
		Node	   *arr = lion_or_as_array(clause);

		if (arr != NULL &&
			lion_analyze_leaf(root, arr, rti, true, true, true, &leaf) &&
			leaf.kind == LION_CLAUSE_ARRAY)
			clause = arr;
	}

	if (IsA(clause, BoolExpr) && ((BoolExpr *) clause)->boolop == OR_EXPR)
		return lion_count_path_where_or(cx, clause);
	if ((ands = lion_all_as_and(clause)) != NIL)
	{
		ListCell   *la;

		foreach(la, ands)
		{
			Node	   *conj = (Node *) lfirst(la);

			if (!lion_analyze_leaf(root, conj, rti, true, true, true, &leaf) ||
				leaf.kind != LION_CLAUSE_NE ||
				!lion_count_path_where_leaf(cx, NULL, conj))
				return false;
		}
		return true;
	}
	return lion_count_path_where_leaf(cx, NULL, clause);
}

/*
 * FILTERED COUNTS (DESIGN.md §10, "Filtered counts"): each condition of
 * count(*) FILTER (WHERE c) and the CASE forms lion_agg_filtered() takes is
 * clauses the posting sets answer, as a WHERE restriction is, appended to
 * the clause lists after the WHERE's, a filter's together.  The node counts
 * the WHERE alone and then the WHERE with each filter, each a count of its
 * own: what this decides per filter is that its run can be one.
 *
 * What the clauses say of their columns is the filter's alone: the WHERE's
 * pins, non-NULL columns, ranges and sources are as they were for every
 * other purpose.  The shapes the runs are not made for decline: the FK-side
 * join (whose aggregates are another node's), a count(DISTINCT), the
 * aggregates over keys of §37, a coalesce group, expressions of the
 * grouping columns, and a GROUP BY whose partial counts core cannot add up -
 * a grouped node with filters emits partial rows for a Finalize Agg.
 */
static bool
lion_count_path_filters(LionCountPathBuild *cx)
{
	PlannerInfo *root = cx->root;
	List	   *posattnos = cx->posattnos;
	List	   *eqattnos = cx->eqattnos;
	List	   *nonnullattnos = cx->nonnullattnos;
	List	   *nullattnos = cx->nullattnos;
	Var		   *notnullvar = cx->notnullvar;
	bool		havepositive = cx->havepositive;
	bool		pinnedsrc = cx->pinnedsrc;
	bool		hasrangesrc = cx->hasrangesrc;
	bool		plainpositive = cx->plainpositive;
	Bitmapset  *notnull;
	ListCell   *lc;
	int			f = 0;

	cx->nwhere = list_length(cx->whereattnos);
	cx->filterdecode = true;
	if (cx->nfilter == 0)
		return true;

	if (cx->fj != NULL || cx->distvar != NULL || cx->wattnos != NIL ||
		cx->groupcoal != NULL || cx->groupdeps != NIL)
		return false;
	if (cx->ngroup > 0 &&
		(cx->extra == NULL ||
		 (cx->extra->flags & GROUPING_CAN_PARTIAL_AGG) == 0 ||
		 !grouping_is_hashable(root->processed_groupClause)))
		return false;
	notnull = lion_notnullattnums(root, cx->input_rel);

	foreach(lc, cx->filterexprs)
	{
		Node	   *expr = (Node *) lfirst(lc);
		int			start = list_length(cx->whereattnos);
		List	   *conjuncts;
		ListCell   *l2;

		f++;
		cx->curfilter = f;
		cx->posattnos = NIL;
		cx->eqattnos = NIL;
		cx->nonnullattnos = NIL;
		cx->nullattnos = NIL;
		cx->notnullvar = NULL;
		cx->havepositive = false;
		cx->pinnedsrc = false;
		cx->hasrangesrc = false;
		cx->plainpositive = false;

		conjuncts = make_ands_implicit(canonicalize_qual((Expr *) copyObject(expr),
														 false));
		foreach(l2, conjuncts)
		{
			if (!lion_count_path_filter_clause(cx, (Node *) lfirst(l2)))
				return false;
		}
		if (list_length(cx->whereattnos) == start)
			return false;

		/*
		 * A range taken as a source holds no pin (DESIGN.md §32): the run
		 * needs a source that does - the walk, or a clause of the WHERE's or
		 * of the filter's - as the WHERE alone does.
		 */
		if (cx->hasrangesrc &&
			!(cx->ngroup > 0 || cx->sumall || pinnedsrc || cx->pinnedsrc ||
			  cx->slicedsrc))
			return false;

		/*
		 * The decoded walk (DESIGN.md §34) counts against the run's WHERE
		 * collected, which needs a source positive whatever its value to
		 * begin from, and no range it would walk.
		 */
		if (cx->hasrangesrc || !(plainpositive || cx->plainpositive))
			cx->filterdecode = false;

		/* each count(col) of the filter: col non-NULL in all its rows */
		foreach(l2, cx->filtercols)
		{
			List	   *fc = (List *) lfirst(l2);
			int			attno = lsecond_int(fc);

			if (linitial_int(fc) != f)
				continue;
			if (!list_member_int(nonnullattnos, attno) &&
				!list_member_int(cx->nonnullattnos, attno) &&
				!(attno > 0 && bms_is_member(attno, notnull)))
				return false;
		}
	}

	/* the filters' ORs follow the WHERE's, read by their runs as those are */
	lion_ors_check(cx->ors, cx->clauseinfos);

	cx->curfilter = 0;
	cx->posattnos = posattnos;
	cx->eqattnos = eqattnos;
	cx->nonnullattnos = nonnullattnos;
	cx->nullattnos = nullattnos;
	cx->notnullvar = notnullvar;
	cx->havepositive = havepositive;
	cx->pinnedsrc = pinnedsrc;
	cx->hasrangesrc = hasrangesrc;
	cx->plainpositive = plainpositive;

	if (cx->decode && !cx->filterdecode)
		return false;

	/*
	 * A filter's complements (DESIGN.md §35, "Complements in an OR") and
	 * multi-key queries that may narrow nothing (§17) answer to the terms
	 * lion_count_path_strategy() set the WHERE's, which it checked before
	 * the filters were analysed: no complement OR in the decoded walk's
	 * collected copy, none and no negated clause on an expression column
	 * beside a query that may narrow nothing where no walk drives the
	 * counts (lion_where_scan_untested(), the WHERE's clauses positive
	 * whatever their value being in every run), and no residual clause
	 * (§10, "Residual clauses") beside one known only at run time either.
	 */
	if (cx->ncompl > 0 && cx->decode)
		return false;
	if (lion_where_scan_untested(cx, cx->decode))
		return false;
	if (cx->residual != NIL && cx->runtimemulti)
		return false;
	return true;
}

/*
 * The first live leaf under a partitioned rel, and the attnums of the
 * parent's columns there, level by level (lion_child_var()): 0 where a
 * level has no such column.  A plain rel is its own leaf.
 */
static RelOptInfo *
lion_count_first_leaf(PlannerInfo *root, RelOptInfo *rel, AttrNumber *attnos,
					  int natts)
{
	while (IS_PARTITIONED_REL(rel))
	{
		RelOptInfo *child = NULL;
		int			i;

		for (i = 0; i < rel->nparts; i++)
		{
			if (rel->part_rels[i] != NULL &&
				bms_is_member(i, rel->live_parts) &&
				!IS_DUMMY_REL(rel->part_rels[i]))
			{
				child = rel->part_rels[i];
				break;
			}
		}
		if (child == NULL)
			return NULL;
		for (i = 0; i < natts; i++)
		{
			if (attnos[i] != 0)
			{
				Var		   *cvar = lion_child_var(root, child->relid,
												  attnos[i]);

				attnos[i] = (cvar != NULL) ? cvar->varattno : 0;
			}
		}
		rel = child;
	}
	return rel;
}

/*
 * The column a sum over EVERY row drives by (DESIGN.md §35, "Every row"),
 * when nothing in the WHERE selects rows: `count(*)` alone, or `IS NOT NULL`
 * and `<>` clauses only.  The entries of one SCALAR lion column are disjoint
 * and between them - the NULL entry included - hold every row of the table,
 * so the sum over them, less what the negated clauses subtract from each, is
 * the count; the column with the fewest entries is the cheapest walk.  Only a
 * column of a scalar, non-expression index will do (lion_find_roaring_index()):
 * a multi-key column's entries overlap and miss the rows with no keys, and a
 * partial index holds only its predicate's rows - which is every row the
 * count is to read when it is planned with that index (DESIGN.md §45), and
 * then its columns come first.
 * A column with an `IS NOT NULL` of its own is preferred when it is no
 * dearer, since its NULL entry is then skipped and its clause dropped
 * (lion_locate_where()).  A partitioned table's candidates are those of its
 * first live leaf, which lion_collect_targets() then asks of every partition.
 */
static Var *
lion_count_all_driver(LionCountPathBuild *cx)
{
	PlannerInfo *root = cx->root;
	RelOptInfo *rel = cx->input_rel;
	int			natts = Max((int) rel->max_attr, 0);
	AttrNumber *attnos = (AttrNumber *) palloc0(sizeof(AttrNumber) * Max(natts, 1));
	RelOptInfo *leaf;
	IndexOptInfo *part;
	Var		   *best = NULL;
	bool		bestpart = false;
	double		bestentries = 0.0;
	int			a;

	for (a = 0; a < natts; a++)
		attnos[a] = (AttrNumber) (a + 1);
	leaf = lion_count_first_leaf(root, rel, attnos, natts);
	if (leaf == NULL)
		return NULL;
	part = lion_partial_index_of(leaf);

	for (a = 0; a < natts; a++)
	{
		Oid			type;
		int32		typmod;
		Oid			coll;
		Var		   *var;
		VariableStatData vardata;
		bool		isdefault;
		double		entries;
		IndexOptInfo *idx;
		bool		inpart;

		/* an indexed column is no dropped one, nor maps to none (§16) */
		if (attnos[a] == 0 ||
			(idx = lion_find_roaring_index(leaf, attnos[a], false,
										   NULL)) == NULL)
			continue;
		inpart = (part != NULL && idx == part);
		get_atttypetypmodcoll(cx->rte->relid, (AttrNumber) (a + 1), &type,
							  &typmod, &coll);
		var = makeVar(cx->rti, (AttrNumber) (a + 1), type, typmod, coll, 0);
		examine_variable(root, (Node *) var, cx->rti, &vardata);
		entries = get_variable_numdistinct(&vardata, &isdefault);
		ReleaseVariableStats(vardata);

		/* the first `IS NOT NULL` column skips its NULL entry: no dearer */
		if (cx->notnullvar != NULL &&
			var->varattno == cx->notnullvar->varattno)
			entries = Max(entries - 1.0, 1.0);

		/*
		 * A column of the partial index the count is planned with first
		 * (DESIGN.md §45): its entries are the predicate's rows, which is
		 * what holds a count with restrictions left out to them.
		 */
		if (best == NULL || (inpart && !bestpart) ||
			(inpart == bestpart && entries < bestentries))
		{
			best = var;
			bestpart = inpart;
			bestentries = entries;
		}
	}
	pfree(attnos);
	return best;
}

/*
 * What drives the count: the grouping column, the distinct column, or the
 * sum over all of one column's entries (sumall) - bounded by the range that
 * bounds that walk (rangevar), every other range being made a source.  Sets
 * driveattno, and which clauses must print their value.
 */
static bool
lion_count_path_strategy(LionCountPathBuild *cx)
{
	PlannerInfo *root = cx->root;
	RelOptInfo *input_rel = cx->input_rel;
	Index		rti = cx->rti;
	ListCell   *lc;

	/*
	 * An OR with a complement arm is two counts wherever one is made
	 * (lion_node_count(), DESIGN.md §35, "Complements in an OR"), and the
	 * shapes that read the WHERE some other way - the decoded walk's
	 * collected copy, the FK-side join's fact filters - have no second
	 * count to make: not beside them, as yet.  Nor, where no walk drives
	 * the count, beside a multi-key query that may turn out to narrow
	 * nothing and leave the count with no source to take its rows from but
	 * the heap, which makes one count and tests no negated clause on an
	 * expression column (§41) either (lion_where_scan_untested()).
	 */
	if (cx->ncompl > 0 && cx->decode)
		return false;
	if (lion_where_scan_untested(cx, cx->decode))
		return false;

	/* Something has to drive the count. */
	cx->driveattno = cx->groupattno[0];
	if (cx->distvar != NULL && cx->ngroup == 0)
	{
		/* k's entries drive it, whatever the WHERE says (DESIGN.md §26). */
		cx->driveattno = cx->distvar->varattno;
	}

	/*
	 * Which range bounds the walk that DRIVES the count (DESIGN.md §28): the
	 * one on the column the GROUP BY or the count(DISTINCT) walks, or - with
	 * neither - the WIDEST range, whose sum is then the count: it walks the
	 * fewer sets of the two sides (the complement, and the summaries of §32,
	 * make a wide walk short) where a range taken as a source is collected
	 * row by row.  A column with an equality, a list or a null test of its
	 * own does not drive: those are positive clauses on it, which §10 leaves
	 * to the ordinary merge.  Every other range is a source (§32).
	 */
	if (cx->rangevars != NIL)
	{
		int			best = -1;
		double		bestsel = -1.0;
		int			r = 0;
		int			otherpin = -1;	/* not asked yet */
		bool	   *sliceable;
		ListCell   *l1;
		ListCell   *l2;

		/*
		 * In a build that answers ranges from slices (DESIGN.md §53), a
		 * range the slices of its column can answer is one of them, and
		 * never the one whose sum drives the count - unless it bounds the
		 * walk of the GROUP BY's or the count(DISTINCT)'s column, whose
		 * entries are what the walk reads anyway.
		 */
		sliceable = (bool *) palloc0(sizeof(bool) *
									 Max(list_length(cx->rangevars), 1));
		if (cx->slicerange)
		{
			forboth(l1, cx->rangevars, l2, cx->rangepos)
			{
				Var		   *v = (Var *) lfirst(l1);
				ListCell   *lp;
				bool		ok = !((cx->ngroup == 1 || cx->distvar != NULL) &&
								   v->varattno == cx->driveattno);

				foreach(lp, (List *) lfirst(l2))
				{
					LionClauseInfo *ci = (LionClauseInfo *)
						list_nth(cx->clauseinfos, lfirst_int(lp));

					if (!ok)
						break;
					ok = lion_count_slice_range_ok(cx, v->varattno,
												   ci->cmptype);
				}
				sliceable[r++] = ok;
			}
			r = 0;
		}

		forboth(l1, cx->rangevars, l2, cx->rangecls)
		{
			Var		   *v = (Var *) lfirst(l1);

			if (sliceable[r])
			{
				/* a source from its slices, below */
			}
			else if (!list_member_int(cx->posattnos, (int) v->varattno) &&
					 cx->ngroup <= 1)
			{
				if (cx->ngroup == 1 || cx->distvar != NULL)
				{
					if (v->varattno == cx->driveattno)
						best = r;
				}
				else if (cx->partitioned &&
						 lion_rinfos_implied_everywhere(root, input_rel,
														(List *) lfirst(l2)) &&
						 (otherpin >= 0 ? otherpin :
						  (otherpin =
						   lion_pinned_not_implied(root, input_rel,
												   cx->clauseinfos,
												   cx->rinfoclauses))))
				{
					/*
					 * A range every partition's bounds imply walks all of
					 * each partition's keys: as a source it is left out
					 * instead (DESIGN.md §16, "Clauses the partition bounds
					 * imply"), when another clause is there to select the
					 * rows and carry the §9 interlock.
					 */
				}
				else
				{
					Selectivity sel = clauselist_selectivity(root,
															 (List *) lfirst(l2),
															 rti, JOIN_INNER,
															 NULL);

					if (sel > bestsel)
					{
						bestsel = sel;
						best = r;
					}
				}
			}
			r++;
		}
		if (best >= 0)
		{
			cx->rangevar = (Var *) list_nth(cx->rangevars, best);
			cx->rangeclauses = (List *) list_nth(cx->rangecls, best);
		}
		if (list_length(cx->rangevars) > (best >= 0 ? 1 : 0))
		{
			lion_ranges_to_sources(cx->rangevars, cx->rangepos, best,
								   cx->wherekinds, cx->clauseinfos);
			cx->hasrangesrc = true;
			cx->havepositive = true;
		}

		/*
		 * ... and the ranges the slices answer are marked so, every bound:
		 * each is ANDed with its column's existence slice, located with its
		 * pin, so that it carries the §9 interlock itself.
		 */
		r = 0;
		forboth(l1, cx->rangevars, l2, cx->rangepos)
		{
			ListCell   *lp;

			if (sliceable[r++])
			{
				foreach(lp, (List *) lfirst(l2))
					((LionClauseInfo *) list_nth(cx->clauseinfos,
												 lfirst_int(lp)))->sliced = true;
				cx->slicedsrc = true;
				cx->nsliced++;
			}
		}
		pfree(sliceable);
	}

	/*
	 * A build that answers ranges from slices and answers none so is the
	 * build without it, made again: not offered twice.
	 */
	if (cx->slicerange && cx->nsliced == 0)
		return false;

	/*
	 * The decoded walk (DESIGN.md §34) reads the WHERE as one collected copy,
	 * every range in it a source - two grouping columns or more let no range
	 * bound a walk, above - collected into it however large
	 * (lion_locate_where()).  Not a WHERE of negated clauses alone (`c IS
	 * NOT NULL`), whose copy - the positive clauses' intersection, less the
	 * others - has nothing to begin from.
	 *
	 * Beside the other shapes of the WHERE: no complement OR (declined above
	 * for the decoded walk), and no residual clause, which the decoded walk
	 * never tests (lion_count_priv_check()); filtered counts are counted
	 * against the WHERE's copy, a range of the WHERE's collected into it for
	 * every filter's run as for the WHERE's own (lion_count_path_filters()
	 * keeps a filter's own range out of the decoded walk).  countcombos.sql
	 * compares each with the sequential plan.
	 */
	if (cx->decode && cx->wherekinds != NIL && !cx->havepositive)
		return false;

	if (cx->distvar != NULL && cx->ngroup == 0)
	{
		/* driven by k's entries, above */
	}
	else if (cx->ngroup == 0 && cx->rangevar != NULL)
	{
		/*
		 * A range on k with no GROUP BY (DESIGN.md §28): the sum over k's
		 * entries in the range, each intersected with the other clauses -
		 * §14's sum-over-all with the walk bounded.  The entries of one
		 * scalar index are disjoint, so the sum is the count of their union,
		 * which is exactly the rows whose k is in the range.
		 */
		cx->sumall = true;
		cx->driveattno = cx->rangevar->varattno;
	}
	else if (cx->ngroup == 0 && !cx->havepositive)
	{
		/*
		 * Nothing that selects rows: `count(*)` alone, or beside `IS NOT
		 * NULL` and `<>` clauses only, which subtract (DESIGN.md §14, §35).
		 * Any lion-indexed column's index knows every row of the table, so
		 * the count is the sum over all of its entries - disjoint, a row
		 * having one value per column or none - with the negated clauses
		 * subtracted from each (lion_count_all_driver()).  Driven by the
		 * first `IS NOT NULL` column, its own NULL entry is skipped and its
		 * clause dropped (lion_locate_where()).  The plan names the column
		 * whichever it is (LION_PRIV_ALLROWS): an `IS NOT NULL` among the
		 * clauses may be an OR's leaf or a filtered count's, which says
		 * nothing of the driver.
		 */
		Var		   *drive = lion_count_all_driver(cx);

		if (drive == NULL)
			return false;
		cx->sumall = true;
		cx->driveattno = drive->varattno;
		cx->allvar = drive;
	}
	/* A group folded to a constant can only have come from a WHERE key. */
	if (cx->singlegroup && !cx->havepositive)
		return false;

	/*
	 * Residual clauses (DESIGN.md §10) are tested on the rows the others
	 * select, which have to be fewer than the table's: a positive clause, an
	 * OR of them or a range taken as a source, and not a sum over entries -
	 * every row's, or a range's on the column it walks - that reads no row.
	 * Nor a multi-key query known only at run time, whose candidates may be
	 * every row.
	 *
	 * Beside filtered counts (DESIGN.md §10, "Filtered counts") they are
	 * taken as they are: a residual clause is a WHERE clause, and the row
	 * filter that tests it is built afresh for every run's located WHERE
	 * (lion_locate_where(), lion_build_filter()), so the WHERE's run and
	 * each filter's count only their rows that pass it - each filter's rows
	 * a subset of the WHERE's, which the share below bounds.  A filter's
	 * own condition is never residual: lion_count_path_filters() declines a
	 * clause of one that no posting set answers.  The price is paid once
	 * per run (lion_count_path_add()).
	 */
	if (cx->residual != NIL &&
		(!cx->havepositive || cx->sumall || cx->runtimemulti ||
		 cx->wattnos != NIL))
		return false;

	/*
	 * ... and they have to be few enough that fetching them one at a time
	 * beats reading the table in order (LION_RESID_MAX_SHARE), whatever the
	 * price of each says: a sequential scan reads a page's rows together.
	 * A partitioned table is asked this of each partition that tests them,
	 * once the partitions are known (lion_count_path_targets()).
	 */
	if (cx->residual != NIL && !cx->partitioned &&
		lion_residual_rows(root, input_rel, input_rel, cx->residual) >
		LION_RESID_MAX_SHARE * Max(input_rel->tuples, 1.0))
		return false;

	/*
	 * A range taken as a source (DESIGN.md §32) is collected into memory and
	 * holds no pin, so every count it takes part in needs another source that
	 * carries the §9 interlock, or it would recheck every row in the heap: the
	 * walk that drives the count, or a clause - or an OR of clauses - outside
	 * the ranges.  Without one the query is left to the ordinary plan.
	 */
	if (cx->hasrangesrc &&
		!(cx->ngroup > 0 || cx->sumall || cx->distvar != NULL ||
		  cx->pinnedsrc || cx->slicedsrc ||
		  (cx->slicemode && cx->sattnos != NIL && !cx->wcounts)))
		return false;

	/*
	 * The range left bounding a walk has to bound the one that DRIVES the
	 * count (DESIGN.md §28): k's entries under a GROUP BY k, a count(DISTINCT
	 * k) or the sum above, or g's under `g, count(DISTINCT k) ... GROUP BY g`.
	 * Every other range was made a source above (§32), so this only guards
	 * against the two halves of this function drifting apart.
	 */
	if (cx->rangevar != NULL &&
		(cx->ngroup > 1 || cx->driveattno != cx->rangevar->varattno))
		return false;

	/*
	 * Which clauses have to produce a value, rather than just select rows: a
	 * value-producing pushdown needs an index whose stored keys are a
	 * representation the rows themselves have (finding 4 of the 2026-09-20
	 * review), and that is checked per relation below.
	 */
	foreach(lc, cx->clauseinfos)
	{
		LionClauseInfo *ci = (LionClauseInfo *) lfirst(lc);

		ci->valueout = (ci->kind == LION_CLAUSE_EQ &&
						list_member_int(cx->valueattnos, (int) ci->attno));
	}
	return true;
}

/*
 * The relations to count - the table, or each live leaf partition - and the
 * indexes on each (targets, first), found for the driving columns and the
 * clauses.  Orders the two grouping columns by their estimates, and sets
 * distest and rangesel.
 */
static bool
lion_count_path_targets(LionCountPathBuild *cx)
{
	PlannerInfo *root = cx->root;
	RelOptInfo *input_rel = cx->input_rel;
	Index		rti = cx->rti;
	LionDriveInfo drive[LION_MAX_GROUPCOLS];
	int			ndrive = 0;
	int			g;

	/*
	 * ---- the relations to count, and the indexes on each of them ----
	 *
	 * One table, or one live leaf partition at a time (DESIGN.md §16).  The
	 * column numbers above are the parent's; lion_collect_targets() maps them
	 * onto each partition through its AppendRelInfo before looking an index
	 * up, because partitions may number their columns differently.
	 */
	memset(drive, 0, sizeof(drive));

	/*
	 * How many entries each grouping column's index has, which is what
	 * decides the outer/inner roles of the nested loop (DESIGN.md §20) and
	 * what the cost model multiplies: the column with FEWER distinct values
	 * drives the scan, so the other index's keys - which are read once and
	 * probed per pair - are the ones whose bucket lookups are repeated.
	 */
	for (g = 0; g < cx->ngroup; g++)
		cx->groupest[g] = estimate_num_groups(root,
											  list_make1(lion_vcol_unvar((Node *) cx->groupvar[g])),
											  input_rel->rows, NULL, NULL);

	/*
	 * ... and the decoded walk of DESIGN.md §34 takes them in that order too:
	 * its first column's sets are the ones it keeps pinned, and a pass takes
	 * only as many of its values as the pin budget allows, so the fewer the
	 * better.  An insertion sort, stable, of at most LION_MAX_GROUPCOLS.
	 */
	for (g = 1; g < cx->ngroup; g++)
	{
		int			h;

		for (h = g; h > 0 && cx->groupest[h] < cx->groupest[h - 1]; h--)
		{
			Var		   *tv = cx->groupvar[h - 1];
			AttrNumber	ta = cx->groupattno[h - 1];
			Oid			te = cx->groupeqop[h - 1];
			Oid			tc = cx->groupcoll[h - 1];
			bool		tvo = cx->groupvalueout[h - 1];
			double		tn = cx->groupest[h - 1];

			cx->groupvar[h - 1] = cx->groupvar[h];
			cx->groupattno[h - 1] = cx->groupattno[h];
			cx->groupeqop[h - 1] = cx->groupeqop[h];
			cx->groupcoll[h - 1] = cx->groupcoll[h];
			cx->groupvalueout[h - 1] = cx->groupvalueout[h];
			cx->groupest[h - 1] = cx->groupest[h];
			cx->groupvar[h] = tv;
			cx->groupattno[h] = ta;
			cx->groupeqop[h] = te;
			cx->groupcoll[h] = tc;
			cx->groupvalueout[h] = tvo;
			cx->groupest[h] = tn;
		}
	}
	if (cx->ngroup > 0)
		cx->driveattno = cx->groupattno[0];

	memset(drive, 0, sizeof(drive));
	if (cx->ngroup > 0)
	{
		for (g = 0; g < cx->ngroup; g++)
		{
			drive[g].attno = cx->groupattno[g];
			drive[g].var = cx->groupvar[g];
			drive[g].collation = cx->groupcoll[g];
			drive[g].eqop = cx->groupeqop[g];
			drive[g].valueout = cx->groupvalueout[g];
		}
		ndrive = cx->ngroup;
	}
	else if (cx->sumall)
	{
		/* §14's sum-over-all: one index, and it groups nothing. */
		drive[0].attno = cx->driveattno;
		drive[0].var = (cx->rangevar != NULL) ? cx->rangevar : cx->allvar;
		Assert(drive[0].var != NULL);
		drive[0].collation = InvalidOid;
		drive[0].eqop = InvalidOid;
		drive[0].valueout = false;
		ndrive = 1;
	}

	/*
	 * The column count(DISTINCT) counts is one more driving column (DESIGN.md
	 * §26): the only one without a GROUP BY, the inner one beside it.  Every
	 * per-column rule of lion_collect_targets() applies to it as to a grouping
	 * column - a SCALAR index (a multi-key one is never found, and
	 * count(DISTINCT tags) counts arrays, not elements), the DISTINCT's
	 * collation, and the DISTINCT's own equality as strategy 1 of the index's
	 * opfamily, so that its entries are exactly the classes DISTINCT counts.
	 * No value of it is ever printed.
	 */
	if (cx->distvar != NULL)
	{
		Assert(ndrive == cx->ngroup && ndrive < LION_MAX_GROUPCOLS);
		drive[ndrive].attno = cx->distvar->varattno;
		drive[ndrive].var = cx->distvar;
		drive[ndrive].collation = cx->distcoll;
		drive[ndrive].eqop = cx->disteqop;
		drive[ndrive].valueout = false;
		ndrive++;

		/*
		 * Every entry of k is tested, whether or not the WHERE leaves it any
		 * row, so the walk is n_distinct(k) of the whole table long.
		 */
		cx->distest = estimate_num_groups(root,
										  list_make1(lion_vcol_unvar((Node *) cx->distvar)),
										  Max(input_rel->tuples, 1.0),
										  NULL, NULL);
	}

	/*
	 * The share of the driving column's entries the range walks (DESIGN.md
	 * §28): the selectivity of the range clauses alone, which for a column
	 * whose rows spread evenly over its values is also the share of its
	 * values, and overstates the walk for a skewed one.  A count(DISTINCT k)
	 * over a range on k tests exactly those entries.
	 */
	if (cx->rangevar != NULL)
	{
		cx->rangesel = clauselist_selectivity(root, cx->rangeclauses, rti,
											  JOIN_INNER, NULL);
		if (cx->distvar != NULL && cx->ngroup == 0)
			cx->distest = lion_range_entries(root, input_rel, cx->distvar,
											 cx->rangesel);
	}

	cx->imply.toprel = input_rel;
	cx->imply.clauses = cx->rinfoclauses;
	cx->imply.driven = (ndrive > 0);
	cx->imply.skipped = cx->impliedtexts;
	cx->imply.ors = cx->ors;
	cx->imply.residual = cx->residual;
	cx->imply.residable = cx->residable;
	cx->imply.wantresid = NULL;
	cx->imply.wanted = false;
	cx->imply.hard = false;
	if (!lion_collect_targets(root, input_rel, drive, ndrive, cx->whereattnos,
							 cx->clauseinfos,
							 cx->partitioned ? &cx->imply : NULL,
							 &cx->targets))
		return false;
	if (cx->targets == NIL)
		return false;			/* everything was pruned: leave it to the planner */

	/*
	 * The residual clauses' rows have to be few enough to fetch one at a
	 * time (LION_RESID_MAX_SHARE), as a table's have
	 * (lion_count_path_strategy()): over the partitions that test them,
	 * taken together - a sub-partition of the one kind the WHERE selects
	 * fetches every row of its own, and the ordinary plan reads it in
	 * order, but that plan is the whole table's, and the question is of
	 * the whole.  Each partition's rows are its own estimate.
	 */
	if (cx->residual != NIL && cx->partitioned)
	{
		ListCell   *lc;
		double		rows = 0.0;
		double		tuples = 0.0;

		foreach(lc, cx->targets)
		{
			LionCountTarget *t = (LionCountTarget *) lfirst(lc);
			List	   *kept = lion_target_residual(cx->residual, t);

			if (kept == NIL)
				continue;
			rows += lion_residual_rows(root, input_rel, t->rel, kept);
			tuples += Max(t->rel->tuples, 1.0);
		}
		if (rows > LION_RESID_MAX_SHARE * tuples)
			return false;
	}

	/*
	 * Planned with a partial index (DESIGN.md §45): every row the count can
	 * reach has to be one of its predicate's.  A sum over every row with
	 * nothing bounding its walk counts a filtered count's run that has a
	 * source of its own by that source, and not by the walk.
	 */
	if (!lion_partial_anchored(cx->targets, cx->clauseinfos, cx->ors, ndrive,
							   cx->sumall && cx->rangevar == NULL,
							   cx->nfilter))
		return false;
	cx->first = (LionCountTarget *) linitial(cx->targets);
	return true;
}

/*
 * The estimates the path is priced by: the groups the walk visits
 * (numgroups) and the rows the node emits (outrows) - partial rows, under
 * partialtarget, for a partitioned GROUP BY.
 */
static void
lion_count_path_estimate(LionCountPathBuild *cx)
{
	PlannerInfo *root = cx->root;
	RelOptInfo *input_rel = cx->input_rel;
	RelOptInfo *output_rel = cx->output_rel;
	ListCell   *lc;

	/* ---- build the path ---- */
	if (cx->ngroup == 1)
		cx->numgroups = cx->groupest[0];
	else if (cx->ngroup >= 2)
	{
		List	   *vars = NIL;
		int			g;

		for (g = 0; g < cx->ngroup; g++)
			vars = lappend(vars, lion_vcol_unvar((Node *) cx->groupvar[g]));
		cx->numgroups = estimate_num_groups(root, vars, input_rel->rows,
											NULL, NULL);
	}
	else if (cx->sumall && cx->rangevar != NULL)
	{
		/* Every entry in the range is visited (DESIGN.md §28). */
		cx->numgroups = lion_range_entries(root, input_rel, cx->rangevar,
										   cx->rangesel);
	}
	else if (cx->sumall)
	{
		/* Every entry of the driving index is visited, one group or not. */
		Var		   *dv = cx->allvar;

		Assert(dv != NULL);
		cx->numgroups = estimate_num_groups(root,
											list_make1(lion_vcol_unvar((Node *) dv)),
											input_rel->rows, NULL, NULL);
	}
	else if (cx->distvar != NULL)
	{
		/* ... and so is every entry of k's (DESIGN.md §26). */
		cx->numgroups = cx->distest;
	}
	else
		cx->numgroups = 1.0;

	/*
	 * How many rows the node itself produces.  One per group, except for a
	 * partitioned GROUP BY, which emits one PARTIAL row per group per
	 * partition and lets the Finalize Agg above combine them (DESIGN.md §16):
	 * that is the sum of the partitions' own group estimates, each made
	 * against the partition's statistics and capped by its row count.
	 */
	if (cx->ngroup == 0)
		cx->outrows = 1.0;
	else if (cx->decode)
	{
		/*
		 * The decoded walk's rows are partial counts, one a combination with
		 * rows (DESIGN.md §34), below a Finalize Agg - as a partitioned
		 * table's are.
		 */
		cx->outrows = cx->numgroups;
		cx->partialtarget = lion_make_partial_target(root,
													 output_rel->reltarget,
													 cx->having);
	}
	else if (!cx->partitioned)
		cx->outrows = cx->numgroups;
	else
	{
		cx->outrows = 0.0;
		foreach(lc, cx->targets)
		{
			LionCountTarget *t = (LionCountTarget *) lfirst(lc);
			double		relrows = Max(t->rel->rows, 1.0);
			double		relgroups;

			if (t->drivevar[0] != NULL && cx->ngroup == 2 &&
				t->drivevar[1] != NULL)
				relgroups = estimate_num_groups(root,
												list_make2(t->drivevar[0],
														   t->drivevar[1]),
												relrows, NULL, NULL);
			else if (t->drivevar[0] != NULL)
				relgroups = estimate_num_groups(root,
												list_make1(t->drivevar[0]),
												relrows, NULL, NULL);
			else
				relgroups = cx->numgroups;
			cx->outrows += Min(relgroups, relrows);
		}
		cx->outrows = Max(cx->outrows, 1.0);

		/*
		 * ... and those rows are partial aggregates, so the node's target is
		 * the partially-grouped one and the grouped rel gets a Finalize Agg
		 * over it further down.
		 */
		cx->partialtarget = lion_make_partial_target(root,
													 output_rel->reltarget,
													 cx->having);
	}

	/*
	 * Filtered counts (DESIGN.md §10): a walk's groups come out once per run,
	 * as partial rows, below a Finalize Agg that adds them up.
	 */
	if (cx->nfilter > 0 && cx->ngroup > 0)
	{
		cx->outrows *= (double) (1 + cx->nfilter);
		if (cx->partialtarget == NULL)
			cx->partialtarget = lion_make_partial_target(root,
														 output_rel->reltarget,
														 cx->having);
	}
}

/*
 * THE TOP k BY COUNT (DESIGN.md §36): `GROUP BY g ORDER BY count(*) DESC
 * LIMIT k`, where the node may emit only the groups that can be among the
 * first k rows and leave their order and the cut to the Sort and Limit core
 * puts above it.  Sets topkn, topkcand and topkstrict, or leaves topkn 0.
 *
 * The grouped rel's paths are read by nothing but that Sort and Limit when
 * the query has no DISTINCT, window function, set-returning function or set
 * operation, so a path of it that leaves out groups the Limit would cut
 * anyway answers the query.  One grouping column walked in one table, one
 * group at a time: not an IN list driving the groups, a coalesce group, a
 * count(DISTINCT), the decoded walk or partial counts - and no HAVING, which
 * could reject groups the k were counted from.
 *
 * The first ORDER BY key has to be the group's count, descending: an
 * aggregate the node answers as the rows of the group - count(*), or
 * count(col) of a column no row of the group has NULL - sorted by int8's `>`
 * of the integer btree family.  Any key after it, or WITH TIES, makes every
 * group tied with the k-th count come out (topkstrict).  k is the LIMIT and
 * the OFFSET added up, both constants.
 *
 * The walk keeps topkcand candidates: enough, for a WHERE that keeps a share
 * s of the rows independently of g, that the k-th count - some s of its bound
 * - is passed by the bounds of the ones after it, 2k/s; never fewer than k
 * and some to spare, never more than LION_TOPK_MAX_CAND, and only when that
 * is well short of the column's entries - or every group is counted anyway.
 */
#define LION_TOPK_MAX		10000
#define LION_TOPK_MAX_CAND	16384

/*
 * The rows of the cand largest groups of var, of ngroups: the frequencies of
 * its most common values, which the statistics keep largest first, and past
 * the end of that list the frequency of a value it leaves out; at the least
 * cand average groups.
 */
static double
lion_topk_rows(PlannerInfo *root, RelOptInfo *rel, Var *var, double cand,
			   double ngroups)
{
	VariableStatData vardata;
	AttStatsSlot sslot;
	double		tuples = Max(rel->tuples, 1.0);
	double		rows = cand * tuples / Max(ngroups, 1.0);

	examine_variable(root, lion_vcol_unvar((Node *) var), 0, &vardata);
	if (HeapTupleIsValid(vardata.statsTuple) &&
		get_attstatsslot(&sslot, vardata.statsTuple, STATISTIC_KIND_MCV,
						 InvalidOid, ATTSTATSSLOT_NUMBERS))
	{
		Form_pg_statistic stats =
			(Form_pg_statistic) GETSTRUCT(vardata.statsTuple);
		double		mcv = 0.0;
		double		top = 0.0;
		int			i;

		for (i = 0; i < sslot.nnumbers; i++)
		{
			mcv += sslot.numbers[i];
			if (i < cand)
				top += sslot.numbers[i];
		}
		if (cand > sslot.nnumbers)
			top += (cand - sslot.nnumbers) *
				Max(1.0 - mcv - stats->stanullfrac, 0.0) /
				Max(ngroups - sslot.nnumbers, 1.0);
		rows = Max(rows, top * tuples);
		free_attstatsslot(&sslot);
	}
	ReleaseVariableStats(vardata);
	return Min(rows, tuples);
}

static void
lion_count_path_topk(LionCountPathBuild *cx)
{
	PlannerInfo *root = cx->root;
	Query	   *parse = cx->parse;
	LionCountTarget *first = cx->first;
	SortGroupClause *sgc;
	TargetEntry *tle;
	Aggref	   *agg;
	Oid			opfamily;
	Oid			opcintype;
	int64		limit;
	int64		offset = 0;
	bool		sumshort;
	bool		groupdrive;
	List	   *others = NIL;
	Selectivity sel;
	double		cand;
	ListCell   *lc;

	cx->topkn = 0;
	if (!lion_enable_topk || cx->fj != NULL || cx->anti != NULL ||
		cx->ngroup != 1 ||
		cx->nfilter > 0 ||
		cx->decode || cx->partitioned || cx->sumall || cx->singlegroup ||
		cx->distvar != NULL || cx->groupcoal != NULL || cx->having != NIL ||
		first == NULL || first->driveidx[0] == NULL)
		return;
	if (parse->distinctClause != NIL || parse->hasWindowFuncs ||
		parse->hasTargetSRFs || parse->groupingSets != NIL ||
		parse->setOperations != NULL || parse->sortClause == NIL ||
		parse->limitCount == NULL || !IsA(parse->limitCount, Const) ||
		((Const *) parse->limitCount)->constisnull)
		return;
	limit = DatumGetInt64(((Const *) parse->limitCount)->constvalue);
	if (parse->limitOffset != NULL)
	{
		if (!IsA(parse->limitOffset, Const))
			return;
		if (!((Const *) parse->limitOffset)->constisnull)
			offset = DatumGetInt64(((Const *) parse->limitOffset)->constvalue);
	}
	if (limit <= 0 || offset < 0 || limit > LION_TOPK_MAX ||
		offset > LION_TOPK_MAX - limit)
		return;

	/* ORDER BY the group's count, descending */
	sgc = linitial_node(SortGroupClause, parse->sortClause);
	tle = get_sortgroupclause_tle(sgc, root->processed_tlist);
	if (tle == NULL || !IsA(tle->expr, Aggref))
		return;
	agg = (Aggref *) tle->expr;
	if (agg->aggdistinct != NIL || agg->aggorder != NIL ||
		agg->aggfilter != NULL || agg->agglevelsup != 0)
		return;
	if (agg->args != NIL)
	{
		Node	   *arg = lion_strip((Node *)
									 ((TargetEntry *) linitial(agg->args))->expr);
		AttrNumber	attno = 0;

		/* count(1) of a non-NULL constant is count(*) */
		if (arg != NULL && IsA(arg, Const) && !((Const *) arg)->constisnull)
			attno = 0;
		else if (arg == NULL || !IsA(arg, Var))
			return;
		else
			attno = ((Var *) arg)->varattno;

		/*
		 * count(col) of a column a clause pins to NULL is 0, and of the group
		 * column 0 in the NULL group: only where the group has no NULL is it
		 * the group's rows (lion_plan_custom_path()'s count kinds).
		 */
		if (list_member_int(cx->nullattnos, (int) attno))
			return;
		if (attno == cx->groupattno[0] &&
			!list_member_int(cx->nonnullattnos, (int) attno) &&
			!bms_is_member(attno, lion_notnullattnums(root, cx->input_rel)))
			return;
	}
	if (!lion_ordering_op_is_gt(sgc->sortop, &opfamily, &opcintype) ||
		opcintype != INT8OID || opfamily != INTEGER_BTREE_FAM_OID)
		return;

	/* the entries of the index are the groups, not an IN list's (§15) */
	(void) lion_inlist_shape(first->driveidx[0], first->drivecol[0],
							 first->driveidx[1],
							 first->whereidx, first->wherecol,
							 cx->whereclauses, cx->wherekinds,
							 cx->ors, false, &sumshort, &groupdrive);
	if (groupdrive)
		return;

	/*
	 * The share of the rows the WHERE keeps, leaving out what it says about g
	 * alone: that takes entries out of the walk, and the counts of the rest
	 * whole.
	 */
	foreach(lc, cx->input_rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
		Bitmapset  *attnos = NULL;
		int			attno;

		pull_varattnos(lion_vcol_subst((Node *) rinfo->clause), cx->rti,
					   &attnos);
		if (bms_get_singleton_member(attnos, &attno) &&
			attno + FirstLowInvalidHeapAttributeNumber == cx->groupattno[0])
			continue;
		others = lappend(others, rinfo);
	}
	sel = (others != NIL) ?
		clauselist_selectivity(root, others, 0, JOIN_INNER, NULL) : 1.0;
	sel = Max(sel, 1e-6);

	cand = Max(ceil(2.0 * (double) (limit + offset) / sel),
			   (double) (limit + offset) + 64.0);
	cand = Min(cand, (double) Max(limit + offset, LION_TOPK_MAX_CAND));
	if (cand * 2.0 > cx->groupest[0])
		return;

	cx->topkn = limit + offset;
	cx->topkcand = (int) cand;
	cx->topkrows = lion_topk_rows(root, cx->input_rel, cx->groupvar[0], cand,
								  cx->groupest[0]);
	cx->topkstrict = (list_length(parse->sortClause) > 1 ||
					  parse->limitOption == LIMIT_OPTION_WITH_TIES);
}

/*
 * What the path's custom_private carries (lion_plan_private.h), begun again
 * for each path made: the relation and its clauses, a partitioned table's
 * partitions, and what the query asks of them.
 */
static void
lion_count_path_fill(LionCountPathBuild *cx)
{
	PlannerInfo *root = cx->root;
	LionCountTarget *first = cx->first;
	LionCountPriv *p = &cx->priv;
	List	   *exec;

	memset(p, 0, sizeof(LionCountPriv));

	/*
	 * A plain table's own Oids go in LION_PRIV_OIDS, which is where the
	 * executor and EXPLAIN have always read them.  A partitioned one leaves
	 * them invalid - there is no single index - and fills LION_PRIV_PARTS
	 * instead, one OidList per partition in the same clause order.
	 */
	p->heapoid = cx->rte->relid;
	p->groupidxoid = (!cx->partitioned && first->driveidx[0] != NULL) ?
		first->driveidx[0]->indexoid : InvalidOid;
	p->groupidxoid2 = (!cx->partitioned && !cx->decode &&
					   first->driveidx[1] != NULL) ?
		first->driveidx[1]->indexoid : InvalidOid;
	p->scanrelid = cx->rti;
	p->groupattno = cx->groupattno[0];
	p->groupattno2 = cx->decode ? 0 : cx->groupattno[1];
	p->flags = (cx->singlegroup ? LION_FLAG_SINGLEGROUP : 0) |
		(cx->sumall ? LION_FLAG_SUMALL : 0) |
		(cx->driveattno != 0 ? LION_FLAG_GROUPIDX : 0) |
		(cx->rangevar != NULL ? LION_FLAG_RANGE : 0) |
		(cx->decode ? LION_FLAG_DECODE : 0) |
		(cx->slicedecode ? LION_FLAG_SLICEDECODE : 0) |
		(cx->residual != NIL ? LION_FLAG_RESID : 0);
	p->resid = cx->residual;

	/*
	 * The strategy of a multi-key clause travels with its operator: the
	 * executor re-extracts the query and EXPLAIN prints the operator's name,
	 * and both need the Oid.
	 */
	lion_count_priv_set_where(p, cx->partitioned ? NIL : first->whereidx,
							  cx->whereattnos, cx->whereconsts,
							  cx->wherekinds, cx->whereopnos, cx->ors);
	if (cx->partitioned)
		lion_count_priv_set_parts(p, cx->targets, true);

	/* the ranges answered from slices (DESIGN.md §53), every bound */
	if (cx->nsliced > 0)
	{
		ListCell   *lc;
		int			i = 0;

		Assert(cx->slicerange);
		foreach(lc, cx->clauseinfos)
		{
			LionClauseInfo *ci = (LionClauseInfo *) lfirst(lc);

			p->clause[i++].sliced = ci->sliced &&
				ci->kind == LION_CLAUSE_RANGESRC;
		}
	}

	/* the filtered counts' clauses (DESIGN.md §10), after the WHERE's */
	if (cx->nfilter > 0)
	{
		ListCell   *lc;
		int			i = 0;

		foreach(lc, cx->clauseinfos)
			p->clause[i++].filter = ((LionClauseInfo *) lfirst(lc))->filter;
		p->nfilter = cx->nfilter;
		p->filterexprs = cx->filterexprs;
	}

	/* a partitioned GROUP BY's HAVING is the Finalize Agg's */
	p->having = (cx->partialtarget != NULL) ? NIL : cx->having;
	if (cx->distvar != NULL)
		p->distattno = cx->distvar->varattno;
	exec = lion_replaced_functions(cx->input_rel,
								   cx->output_rel->reltarget->exprs,
								   cx->having, root->processed_groupClause,
								   NULL);
	p->exec_aggs = (List *) linitial(exec);
	p->exec_funcs = (List *) lsecond(exec);
	p->exec_groupfuncs = (List *) lthird(exec);

	/*
	 * GROUP BY coalesce(col, c) (DESIGN.md §10): c, and the grouping equality
	 * the walk tells c's own entry by, under the grouping collation - which
	 * lion_collect_targets() has made the driving index's strategy 1 and its
	 * collation, per relation.
	 */
	if (cx->groupcoal != NULL)
	{
		p->coalconst = copyObject(cx->groupcoal);
		p->coaleqop = cx->groupeqop[0];
		p->coalcoll = cx->groupvar[0]->varcollid;
	}
	p->implied = cx->impliedtexts;

	/* the partial indexes read, and what their predicates imply (§45) */
	p->partialidx = lion_partial_used();
	p->predimplied = lion_partial_dropped();

	/* the decoded walk's columns (DESIGN.md §34), in the order it takes them */
	if (cx->decode)
	{
		int			g;

		p->ngroupn = cx->ngroup;
		for (g = 0; g < cx->ngroup; g++)
		{
			p->groupn_attno[g] = cx->groupattno[g];
			p->groupn_idx[g] = first->driveidx[g]->indexoid;
		}
	}

	/*
	 * the column a sum over all of a column's entries drives by, when no
	 * range bounds the walk (DESIGN.md §14, §35)
	 */
	if (cx->allvar != NULL)
		p->allattno = cx->allvar->varattno;

	/* the top k by count (DESIGN.md §36) */
	p->topkn = cx->topkn;
	p->topkcand = cx->topkcand;
	p->topkstrict = cx->topkstrict;

	/* the columns aggregates are taken over (DESIGN.md §37) */
	if (cx->wattnos != NIL)
	{
		ListCell   *l1;
		ListCell   *l2;
		ListCell   *l3;
		int			i = 0;

		p->nwcol = list_length(cx->wattnos);
		p->wcol = (LionCountPrivWCol *)
			palloc0(sizeof(LionCountPrivWCol) * p->nwcol);
		forthree(l1, cx->wattnos, l2, cx->widx, l3, cx->wcols)
		{
			p->wcol[i].attno = (AttrNumber) lfirst_int(l1);
			p->wcol[i].idxoid = ((IndexOptInfo *) lfirst(l2))->indexoid;
			p->wcol[i].idxcol = (AttrNumber) lfirst_int(l3);
			i++;
		}
	}

	/* the sliced columns aggregates are answered from (DESIGN.md §52) */
	if (cx->sattnos != NIL)
	{
		ListCell   *l1;
		ListCell   *l2;
		ListCell   *l3;
		int			i = 0;

		p->nscol = list_length(cx->sattnos);
		p->scol = (LionCountPrivSCol *)
			palloc0(sizeof(LionCountPrivSCol) * p->nscol);
		forthree(l1, cx->sattnos, l2, cx->sidx, l3, cx->scols)
		{
			p->scol[i].attno = (AttrNumber) lfirst_int(l1);
			p->scol[i].idxoid = cx->partitioned ? InvalidOid :
				((IndexOptInfo *) lfirst(l2))->indexoid;
			p->scol[i].idxcol = cx->partitioned ? 0 :
				(AttrNumber) lfirst_int(l3);
			i++;
		}

		/*
		 * ... and a partitioned table's, each partition's own index
		 * (lion_count_path_slice_parts()), in PARTS order
		 */
		if (cx->partitioned)
		{
			ListCell   *lc;
			int			j = 0;

			Assert(p->npart == list_length(cx->targets));
			foreach(lc, cx->targets)
			{
				LionCountTarget *t = (LionCountTarget *) lfirst(lc);
				LionCountPrivPart *part = &p->part[j++];

				part->sliceidxoid = (Oid *) palloc0(sizeof(Oid) * p->nscol);
				part->sliceidxcol = (AttrNumber *)
					palloc0(sizeof(AttrNumber) * p->nscol);
				for (i = 0; i < p->nscol; i++)
				{
					part->sliceidxoid[i] = t->sliceidx[i]->indexoid;
					part->sliceidxcol[i] = t->slicecol[i];
				}
			}
		}
	}

	/*
	 * The expression columns (DESIGN.md §41), when the plan uses one, and
	 * the functions of each one used: core's plan computes a grouped or
	 * counted expression in its scan's projection, and so checks EXECUTE on
	 * them (a WHERE clause's are in baserestrictinfo already).
	 */
	if (cx->vcols != NIL)
	{
		Bitmapset  *used = NULL;
		int			i;

#define LION_NOTE_VCOL(a) \
		do { \
			if (LION_ATTNO_IS_VCOL(a)) \
				used = bms_add_member(used, LION_VCOL_INDEX(a)); \
		} while (0)

		LION_NOTE_VCOL(p->groupattno);
		LION_NOTE_VCOL(p->groupattno2);
		LION_NOTE_VCOL(p->distattno);
		LION_NOTE_VCOL(p->allattno);
		for (i = 0; i < p->nclause; i++)
			LION_NOTE_VCOL(p->clause[i].attno);
		for (i = 0; i < p->ngroupn; i++)
			LION_NOTE_VCOL(p->groupn_attno[i]);
#undef LION_NOTE_VCOL

		if (used != NULL)
		{
			p->vcols = cx->vcols;
			i = -1;
			while ((i = bms_next_member(used, i)) >= 0)
				p->exec_funcs = lion_expr_functions(list_nth(cx->vcols, i),
													p->exec_funcs);
		}
	}
}

/*
 * The `<` a GROUP BY column's groups go up in when the anti join's losses
 * pass hash_mem (lion_anti_groups_next()): the type's own, of its default
 * btree opclass, where that class's equality is the grouping's - the order
 * the path's pathkeys claim (lion_count_path_make()) - or else one of any
 * btree family the grouping's equality is in.  InvalidOid for none.
 */
static Oid
lion_anti_group_order(Oid typid, Oid eqop)
{
	TypeCacheEntry *tc = lookup_type_cache(typid, TYPECACHE_LT_OPR |
										   TYPECACHE_EQ_OPR);

	if (OidIsValid(tc->lt_opr) && tc->eq_opr == eqop)
		return tc->lt_opr;
	return get_ordering_op_for_equality_op(eqop, false);
}

/*
 * THE ANTI JOIN OVER A SMALL INNER SIDE (DESIGN.md §27): the count planned so
 * far is the outer table's, of its WHERE; the query's is that count less the
 * rows some row of the inner side matches, which the node finds by looking
 * the inner side's keys up in a B-tree of the table (lion_anti_choose()) and
 * subtracts from the one row, from each row's group, or from each row's cell
 * of a count by cells (DESIGN.md §27, "Counts by cells beside the anti
 * join"), the predicates evaluated on it.  Fills the plan's ANTI member and
 * antiplan, or declines.
 *
 * What it is made for is a count of one plain table, ungrouped, grouped by
 * one or two of its columns, each of whose rows it can find the group of, or
 * by cells: not partitions, the decoded walk of a GROUP BY's columns, a
 * count(DISTINCT) walked, the aggregates over keys, a top k, filtered counts,
 * a coalesce group or expression columns (whose expressions setrefs.c could
 * take for the join clauses' own), and grouping columns it can hash under
 * their equalities and sort by a `<` of their families.  The decoded walk a
 * count by cells drives by groups nothing: its rows are the cells'.
 *
 * Every row a key finds is tested on each of the table's restrictions - and
 * the clauses the count adds to its WHERE (the `col IS NOT NULL` of a
 * count(col)): a row the count does not count is not one to subtract - and,
 * only if it passes them, on every join clause, the scan keys' first; each
 * list in the order core would test it in (lion_anti_order_quals(),
 * lion_anti_join_clauses()), so that no clause runs on a row core's plan
 * would have rejected before it.
 */
static bool
lion_count_path_anti(LionCountPathBuild *cx)
{
	const LionSmallAnti *anti = cx->anti;
	LionCountPriv *p = &cx->priv;
	LionCountPrivAnti *a = &p->anti;
	List	   *restrictions;
	List	   *joinclauses;
	List	   *keyexprs = NIL;
	List	   *rowexprs = NIL;
	int			ngroup = cx->cells ? 0 : cx->ngroup;
	Oid			groupsort[2] = {InvalidOid, InvalidOid};
	int			g;
	int			k;

	if (anti == NULL)
		return true;
	if (cx->partitioned || cx->distvar != NULL ||
		cx->wattnos != NIL || cx->topkn > 0 || cx->nfilter > 0 ||
		cx->groupcoal != NULL || p->vcols != NIL)
		return false;
	if (cx->cells ? (cx->cellplan == NULL || p->ncelldim == 0) :
		(cx->decode || cx->partialtarget != NULL || p->ngroupn > 0))
		return false;
	/* more than two grouping columns are the decoded walk's */
	Assert(ngroup <= 2);
	if (cx->cells)
		rowexprs = cx->cellplan->predexprs;
	for (g = 0; g < ngroup; g++)
	{
		Oid			lhash;
		Oid			rhash;

		if (!get_op_hash_functions(cx->groupeqop[g], &lhash, &rhash) ||
			!OidIsValid(lhash))
			return false;
		groupsort[g] = lion_anti_group_order(exprType((Node *) cx->groupvar[g]),
											 cx->groupeqop[g]);
		if (!OidIsValid(groupsort[g]))
			return false;
	}

	restrictions = list_concat_copy(cx->input_rel->baserestrictinfo,
									cx->antiextra);
	restrictions = lion_anti_order_quals(cx->root, restrictions);
	if (contain_subplans((Node *) restrictions) ||
		contain_volatile_functions((Node *) restrictions))
		return false;

	if (!lion_anti_choose(cx->root, anti, restrictions, ngroup, rowexprs,
						  &cx->antiplan))
		return false;
	joinclauses = lion_anti_join_clauses(cx->root, anti, &cx->antiplan);

	p->hasanti = true;
	memset(a, 0, sizeof(LionCountPrivAnti));
	a->idxoid = cx->antiplan.idx->indexoid;
	a->nkey = cx->antiplan.nkey;
	for (k = 0; k < a->nkey; k++)
	{
		a->keycol[k] = cx->antiplan.keycol[k];
		a->keyop[k] = cx->antiplan.keyop[k];
		a->keycoll[k] = cx->antiplan.keycoll[k];
		a->keysort[k] = cx->antiplan.keysort[k];
		keyexprs = lappend(keyexprs, cx->antiplan.keyexpr[k]);
	}
	a->njoin = list_length(joinclauses);
	a->nrestrict = list_length(restrictions);
	for (g = 0; g < ngroup; g++)
	{
		a->groupeq[g] = cx->groupeqop[g];
		a->groupcoll[g] = cx->groupcoll[g];
		a->groupsort[g] = groupsort[g];
	}
	a->exprs = list_concat(list_concat(keyexprs, joinclauses), restrictions);
	return true;
}

/*
 * The LionCount CustomPath itself: its target, the order its groups come in
 * and the plan it carries in custom_private.  Not priced yet.
 */
static CustomPath *
lion_count_path_make(LionCountPathBuild *cx)
{
	PlannerInfo *root = cx->root;
	RelOptInfo *input_rel = cx->input_rel;
	RelOptInfo *output_rel = cx->output_rel;
	LionCountTarget *first = cx->first;
	CustomPath *cpath;

	cpath = makeNode(CustomPath);
	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = output_rel;
	cpath->path.pathtarget = cx->partialtarget ? cx->partialtarget :
		output_rel->reltarget;
	cpath->path.param_info = NULL;
	cpath->path.parallel_aware = false;
	cpath->path.parallel_safe = false;
	cpath->path.parallel_workers = 0;
	/*
	 * PATHKEYS (DESIGN.md §21).  A single-column GROUP BY driven by the
	 * index's own entry scan emits its groups in DIRECTORY order, which for an
	 * index that orders by the key type's own comparison is exactly what an
	 * `ORDER BY <group col>` asks for - so the Sort above the node disappears.
	 *
	 * These disqualify it:
	 *
	 *	- two group columns: the nested loop of §20 emits (outer, inner) pairs,
	 *	  which are sorted by the outer key alone and not by the pair;
	 *	- an IN list driving the groups (§15): the node walks the located sets
	 *	  rather than the index, and claiming an order for that would tie the
	 *	  planner to a detail of how the list is located;
	 *	- a partitioned table: each partition is ordered, but the Finalize
	 *	  HashAggregate core puts on top destroys it (§16);
	 *	- a NULLABLE group column, because the reserved NULL entry sorts
	 *	  FIRST and `ORDER BY col` means NULLS LAST.  A column the planner
	 *	  knows is NOT NULL has no NULL group to emit, so the two agree;
	 *	- GROUP BY coalesce(col, c) (DESIGN.md §10), whose group c the
	 *	  walk holds back and emits after the last entry, wherever c sorts;
	 *	- and the top k by count (DESIGN.md §36), which emits the candidates
	 *	  it counted largest bound first, for the Sort core puts above it to
	 *	  order by their counts.  Only that Sort reads the path, so a key
	 *	  order claimed would be used by nothing, but it is not the node's.
	 */
	cpath->path.pathkeys = NIL;
	if (cx->ngroup == 1 && !cx->partitioned && !cx->sumall &&
		cx->nfilter == 0 && !cx->cells && cx->topkn == 0 &&
		!cx->singlegroup &&
		cx->groupcoal == NULL && cx->groupdeps == NIL &&
		first->driveidx[0] != NULL &&
		bms_is_member(cx->groupattno[0],
					  lion_notnullattnums(root, input_rel)) &&
		lion_index_orders_naturally(first->driveidx[0], first->drivecol[0]))
	{
		bool		sumshort;
		bool		groupdrive;
		Oid			sortop = InvalidOid;
		Oid			eqop = InvalidOid;
		bool		hashable;

		(void) lion_inlist_shape(first->driveidx[0], first->drivecol[0],
								 first->driveidx[1],
								 first->whereidx, first->wherecol,
								 cx->whereclauses, cx->wherekinds,
								 cx->ors, false, &sumshort, &groupdrive);
		/*
		 * The node emits ASCENDING, NULLS FIRST, always.  The query's own
		 * GROUP BY clause is not a safe source for the direction:
		 * standard_qp_callback() rewrites its sort operators to match the
		 * query's ORDER BY when it can, so `ORDER BY k DESC` would hand us a
		 * descending SortGroupClause and we would claim an order the node does
		 * not produce.  So the ordering operator is the key type's own `<`,
		 * and the clause is copied only for its sortgroupref and its equality.
		 */
		get_sort_group_operators(exprType((Node *) cx->groupvar[0]),
								 true, true, false,
								 &sortop, &eqop, NULL, &hashable);

		/*
		 * ... and beside the anti join over a small inner side, whose
		 * groups past hash_mem go up in the order of its own `<`
		 * (lion_anti_group_order()), only where that is this one.
		 */
		if (!groupdrive && OidIsValid(sortop) &&
			(!cx->priv.hasanti || cx->priv.anti.groupsort[0] == sortop))
		{
			SortGroupClause *sgc;

			sgc = copyObject((SortGroupClause *)
							 linitial(root->processed_groupClause));
			sgc->sortop = sortop;
			sgc->nulls_first = false;
			cpath->path.pathkeys =
				make_pathkeys_for_sortclauses(root, list_make1(sgc),
											  root->processed_tlist);
		}
	}
	cpath->flags = 0;

	/* the anti join's inner side is the node's child (§27) */
	cpath->custom_paths = (cx->anti != NULL) ?
		list_make1(cx->anti->innerpath) : NIL;
#if PG_VERSION_NUM >= 170000
	cpath->custom_restrictinfo = NIL;
#endif
	cpath->custom_private = lion_count_priv_encode(&cx->priv,
												   LION_PRIV_STAGE_PATH);
	cpath->methods = &lion_count_path_methods;

	return cpath;
}

/*
 * Does run r of filtered counts over a sum of every row (DESIGN.md §10,
 * "Filtered counts"; §35) count an intersection instead of the sum - a
 * filter's run with a source of its own that selects rows, an equality, a
 * list, an `IS NULL`, the rows of an array column's reserved entries (§17),
 * or an OR with no range taken as a source among its leaves and no
 * complement arm, as lion_run_has_plain_source() decides at run time from the
 * located sources?  Without a range bounding the walk.
 */
static bool
lion_count_run_plain(LionCountPathBuild *cx, int r)
{
	ListCell   *lc;

	if (r == 0 || !cx->sumall || cx->rangevar != NULL)
		return false;
	foreach(lc, cx->clauseinfos)
	{
		LionClauseInfo *ci = (LionClauseInfo *) lfirst(lc);

		if (ci->filter == r && !ci->inor &&
			(ci->kind == LION_CLAUSE_EQ || ci->kind == LION_CLAUSE_ARRAY ||
			 ci->kind == LION_CLAUSE_NULL || ci->kind == LION_CLAUSE_RESERVED))
			return true;
	}
	foreach(lc, cx->ors)
	{
		List	   *one = (List *) lfirst(lc);
		int			first = linitial_int(one);
		int			narms = lsecond_int(one);
		int			nleaves = 0;
		bool		range = false;
		LionClauseInfo *ci;
		int			a;
		int			j;

		ci = (LionClauseInfo *) list_nth(cx->clauseinfos, first);
		if (ci->filter != r || ci->incompl)
			continue;
		for (a = 0; a < narms; a++)
			nleaves += list_nth_int(one, 2 + a);
		for (j = first; j < first + nleaves; j++)
		{
			ci = (LionClauseInfo *) list_nth(cx->clauseinfos, j);
			if (ci->kind == LION_CLAUSE_RANGESRC)
				range = true;
		}
		if (!range)
			return true;
	}
	return false;
}

/*
 * Price the path and add it to output_rel: below core's Finalize Agg for a
 * partitioned GROUP BY, and otherwise as it is - after the parallel GROUP BY
 * made from it, where there is one.
 *
 * The price is the node's own, in the units of the cheapest core path
 * output_rel has (DESIGN.md §39): over the margin.  What core puts above it - the Finalize Agg of a partitioned or a
 * parallel GROUP BY, the Gather - core prices itself, in its own units, as it
 * prices the same nodes over its own plans.
 */
/*
 * The relations to count as run r of filtered counts counts them (DESIGN.md
 * §10, "Filtered counts"): each target with every other filter's clauses
 * left out of it, as a partition leaves out what its bounds imply - and with
 * no walk driving it where the run is an intersection
 * (lion_count_run_plain()).
 */
static List *
lion_count_run_targets(LionCountPathBuild *cx, List *targets, int r)
{
	List	   *out = NIL;
	int			nclause = list_length(cx->clauseinfos);
	ListCell   *lc;

	foreach(lc, targets)
	{
		LionCountTarget *t = (LionCountTarget *) lfirst(lc);
		LionCountTarget *rt = (LionCountTarget *) palloc(sizeof(LionCountTarget));
		ListCell   *l2;
		int			i = 0;

		memcpy(rt, t, sizeof(LionCountTarget));
		if (lion_count_run_plain(cx, r))
		{
			rt->driveidx[0] = NULL;
			rt->drivecol[0] = 0;
			rt->drivevar[0] = NULL;
		}
		rt->dropped = (bool *) palloc0(sizeof(bool) * Max(nclause, 1));
		rt->ndropped = 0;
		foreach(l2, cx->clauseinfos)
		{
			LionClauseInfo *ci = (LionClauseInfo *) lfirst(l2);

			rt->dropped[i] = (t->dropped != NULL && t->dropped[i]) ||
				(ci->filter != 0 && ci->filter != r);
			if (rt->dropped[i])
				rt->ndropped++;
			i++;
		}
		out = lappend(out, rt);
	}
	return out;
}

/*
 * The node's own price for counting `targets`: its startup and total cost
 * and its rows set on cpath, in cost units of its own (lion_units_price()
 * makes them the competitor's).  Every range source too large to collect is
 * priced collected in windows; what walking them at every count would save
 * instead is added to walksave, with the clauses that bound them, where the
 * plan may walk them (lion_count_path_add()).
 */
static void
lion_count_path_price(LionCountPathBuild *cx, CustomPath *cpath,
					  List *targets, LionRangeCost *rangeprice,
					  LionWalkSave *walksave, bool plain)
{
	PlannerInfo *root = cx->root;
	List	   *volatile sliced = NIL;

	/*
	 * The ranges this build answers from slices (DESIGN.md §53), by their
	 * bounds, for the cost model to price so - and none, for every other
	 * build: set at every pricing, so that nothing of one build's is left
	 * for the next.
	 */
	if (cx->nsliced > 0)
	{
		ListCell   *l1;
		ListCell   *l2;

		forboth(l1, cx->clauseinfos, l2, cx->whereclauses)
		{
			LionClauseInfo *ci = (LionClauseInfo *) lfirst(l1);

			if (ci->sliced && ci->kind == LION_CLAUSE_RANGESRC)
				sliced = lappend(sliced, lfirst(l2));
		}
	}
	lion_cost_set_sliced(sliced);

	/*
	 * Cleared on every way out, a cancellation inside the pricing included:
	 * a later pricing of another build reads the static without setting it
	 * (lion_cost_range_source()), and the list lives in this planning's
	 * memory context.
	 */
	PG_TRY();
	{
		/*
		 * Beside a GROUP BY, count(DISTINCT k) is the (g, k) nested loop of
		 * DESIGN.md §20, g outer and k inner, and is priced as those pairs
		 * (DESIGN.md §26); without one, k's entries are the "groups" of the walk.
		 */
		if (cx->decode)
		{
			lion_cost_decode_path(root, cpath, targets, cx->whereclauses,
								  cx->wherekinds, cx->ors, cx->ngroup,
								  cx->groupest, cx->numgroups, cx->outrows);
			memset(rangeprice, 0, sizeof(LionRangeCost));
		}
		else if (cx->topkn > 0)
		{
			/*
			 * The top k (DESIGN.md §36): a walk of every entry's header, and the
			 * candidates counted - the largest entries, whose rows the column's
			 * most common values say.
			 */
			lion_cost_topk_path(root, cpath, targets, cx->whereclauses,
								cx->wherekinds, cx->ors, cx->groupest[0],
								(cx->rangevar != NULL) ? cx->rangesel : 1.0,
								(double) cx->topkcand, cx->topkrows,
								Min(cx->outrows, (double) cx->topkcand));
			memset(rangeprice, 0, sizeof(LionRangeCost));
		}
		else if (plain)
			lion_cost_count_path(root, cpath, targets, cx->whereclauses,
								 cx->wherekinds, cx->ors, 1.0, 0, 0, cx->outrows,
								 LION_DISTINCT_NONE, LION_RANGED_NONE, 1.0,
								 rangeprice, walksave);
		else
			lion_cost_count_path(root, cpath, targets, cx->whereclauses,
								 cx->wherekinds, cx->ors,
								 cx->numgroups, cx->groupest[0],
								 (cx->ngroup == 2) ? cx->groupest[1] :
								 (cx->distvar != NULL && cx->ngroup == 1) ?
								 cx->distest : 0,
								 cx->outrows,
								 (cx->distvar == NULL) ? LION_DISTINCT_NONE :
								 cx->distcounts ? LION_DISTINCT_COUNT :
								 LION_DISTINCT_EXISTS,
								 (cx->rangevar == NULL) ?
								 ((cx->sumall && cx->distvar == NULL) ?
								  LION_RANGED_SUMALL :
								  LION_RANGED_NONE) :
								 cx->sumall ? LION_RANGED_SUM : LION_RANGED_WALK,
								 cx->rangesel, rangeprice, walksave);
		/*
		 * The aggregates over lion columns' entries (DESIGN.md §37): their walks,
		 * after - or, with no count in the target list, instead of - the sum over
		 * every row.
		 */
		if (cx->wattnos != NIL)
			lion_cost_wagg_path(root, cpath, cx->input_rel, cx->widx, cx->wcols,
								cx->wnaggs, cx->wcounts);
	}
	PG_FINALLY();
	{
		lion_cost_set_sliced(NIL);
	}
	PG_END_TRY();
	list_free(sliced);
}

/*
 * THE AM'S OWN SCAN OF THE COUNT'S SETS (DESIGN.md §39, "The AM's own scan
 * of the same sets"): does a lion index path, ip, read only sets the count
 * reads?  Every clause of it is one of the relation's restrictions the count
 * reads from that same index (idxfor, by the restriction's position), each
 * marked in covered.
 */
static bool
lion_count_ownscan_reads(IndexPath *ip, RelOptInfo *rel, Oid lionam,
						 IndexOptInfo **idxfor, bool *covered)
{
	ListCell   *lc;

	if (ip->indexinfo == NULL || ip->indexinfo->relam != lionam ||
		ip->path.parent != rel || ip->path.param_info != NULL)
		return false;
	foreach(lc, ip->indexclauses)
	{
		IndexClause *ic = (IndexClause *) lfirst(lc);
		int			r = 0;
		bool		found = false;
		ListCell   *l2;

		foreach(l2, rel->baserestrictinfo)
		{
			if ((RestrictInfo *) lfirst(l2) == ic->rinfo)
			{
				found = true;
				break;
			}
			r++;
		}
		if (!found || idxfor[r] == NULL ||
			idxfor[r]->indexoid != ip->indexinfo->indexoid)
			return false;
		covered[r] = true;
	}
	return true;
}

/* ... and the IndexPaths of a bitmap's tree of quals: ANDs of them only */
static bool
lion_count_ownscan_bitmap(Path *bitmapqual, RelOptInfo *rel, Oid lionam,
						  IndexOptInfo **idxfor, bool *covered)
{
	ListCell   *lc;

	if (IsA(bitmapqual, IndexPath))
		return lion_count_ownscan_reads((IndexPath *) bitmapqual, rel, lionam,
										idxfor, covered);
	if (!IsA(bitmapqual, BitmapAndPath))
		return false;
	foreach(lc, ((BitmapAndPath *) bitmapqual)->bitmapquals)
	{
		if (!lion_count_ownscan_bitmap((Path *) lfirst(lc), rel, lionam,
									   idxfor, covered))
			return false;
	}
	return true;
}

/*
 * The cheapest of output_rel's paths that count the rows of a scan of the
 * relation through the lion indexes the count reads - a plain Agg, under
 * nothing that does not pass its row up, over a plain or bitmap scan whose
 * index clauses are every restriction of the relation, each read from the
 * index the count reads it from (lion_count_ownscan_reads()).  Such a scan
 * reads every set the count reads, as the count reads them, and then
 * fetches each row it finds from the heap, where the count tests the
 * visibility map: whatever the rows, it does what the count does and more.
 * Its total cost, or 0 when there is none, or the count is not one such a
 * scan can be: one table's plain count, with no GROUP BY, range, filter,
 * HAVING or residual clause, its WHERE answered by the sets alone.
 *
 * Not a parallel scan, under a Gather: it divides the heap's side among the
 * workers, which the count, serial, does not, and its price is compared as
 * it is.  Nor a path some enable_* setting disabled, which loses to the
 * count whatever its cost.
 */
static Cost
lion_count_ownscan_cost(LionCountPathBuild *cx)
{
	RelOptInfo *rel = cx->input_rel;
	Oid			lionam = lion_get_am_oid();
	int			nbase = list_length(rel->baserestrictinfo);
	int			nclause = list_length(cx->clauseinfos);
	IndexOptInfo **idxfor;
	bool	   *covered;
	Cost		best = 0.0;
	ListCell   *lc;
	int			i;

	if (!OidIsValid(lionam) || cx->fj != NULL || cx->anti != NULL ||
		cx->partitioned ||
		cx->ngroup != 0 || cx->singlegroup || cx->sumall || cx->decode ||
		cx->distvar != NULL || cx->rangevar != NULL || cx->hasrangesrc ||
		cx->nfilter != 0 || cx->residual != NIL || cx->topkn != 0 ||
		cx->wattnos != NIL || cx->partialtarget != NULL ||
		cx->having != NIL || cx->ncompl != 0 || cx->runtimemulti ||
		cx->newnegated || cx->first == NULL ||
		list_length(cx->targets) != 1 ||
		rel->reloptkind != RELOPT_BASEREL ||
		nbase == 0 || nclause == 0 ||
		list_length(cx->first->whereidx) != nclause)
		return 0.0;

	/* the index the count reads each restriction from: one, or none */
	idxfor = (IndexOptInfo **) palloc0(sizeof(IndexOptInfo *) * nbase);
	covered = (bool *) palloc(sizeof(bool) * nbase);
	i = 0;
	foreach(lc, cx->clauseinfos)
	{
		LionClauseInfo *ci = (LionClauseInfo *) lfirst(lc);
		IndexOptInfo *idx = (IndexOptInfo *) list_nth(cx->first->whereidx, i);

		i++;
		if (ci->rinfono < 0 || ci->rinfono >= nbase || idx == NULL ||
			(idxfor[ci->rinfono] != NULL &&
			 idxfor[ci->rinfono]->indexoid != idx->indexoid))
		{
			pfree(idxfor);
			pfree(covered);
			return 0.0;
		}
		idxfor[ci->rinfono] = idx;
	}
	for (i = 0; i < nbase; i++)
	{
		if (idxfor[i] == NULL)
		{
			pfree(idxfor);
			pfree(covered);
			return 0.0;
		}
	}

	foreach(lc, cx->output_rel->pathlist)
	{
		Path	   *p = (Path *) lfirst(lc);
		AggPath    *agg;
		Path	   *scan;
		bool		reads;

		if (p->param_info != NULL || !IsA(p, AggPath) ||
			lion_path_has_lion(p))
			continue;
#if PG_VERSION_NUM >= 180000
		if (p->disabled_nodes > 0)
			continue;
#endif
		agg = (AggPath *) p;
		if (agg->aggstrategy != AGG_PLAIN || agg->aggsplit != AGGSPLIT_SIMPLE ||
			agg->groupClause != NIL || agg->qual != NIL)
			continue;
		scan = agg->subpath;
		while (scan != NULL && IsA(scan, ProjectionPath))
			scan = ((ProjectionPath *) scan)->subpath;
		if (scan == NULL || scan->parent != rel || scan->param_info != NULL ||
			scan->parallel_aware)
			continue;

		memset(covered, 0, sizeof(bool) * nbase);
		if (IsA(scan, IndexPath))
			reads = lion_count_ownscan_reads((IndexPath *) scan, rel, lionam,
											 idxfor, covered);
		else if (IsA(scan, BitmapHeapPath))
			reads = lion_count_ownscan_bitmap(((BitmapHeapPath *) scan)->bitmapqual,
											  rel, lionam, idxfor, covered);
		else
			reads = false;
		for (i = 0; reads && i < nbase; i++)
			reads = covered[i];
		if (reads && (best <= 0.0 || p->total_cost < best))
			best = p->total_cost;
	}
	pfree(idxfor);
	pfree(covered);
	return best;
}

/*
 * How far below the AM's own scan of the same sets the count is priced: past
 * the fuzz add_path() calls two costs the same within (1%, pathnode.c's
 * STD_FUZZ_FACTOR), where the scan's Agg, parallel safe, would win the tie
 * against the count, which is not.
 */
#define LION_COUNT_OWNSCAN_EDGE		1.02

/*
 * The anti join over a small inner side (DESIGN.md §27) on the price of the
 * serial node cpath, whose own work `units` priced: its lookups, the
 * node's own work, priced as the count is; and its child, core's plan in
 * core's units, as an FK-side join's dimension is.  Both before the first
 * row, which comes once every key is looked up.  Returns the lookups'
 * price, which a participant of the parallel form that makes the
 * run pays too (lion_count_anti_parallel()); 0 without the anti join.
 */
static Cost
lion_count_anti_price(const LionCountPathBuild *cx, const LionUnits *units,
					  CustomPath *cpath)
{
	Path	   *child;
	Cost		lookups;

	if (cx->anti == NULL)
		return 0.0;
	child = cx->anti->innerpath;
	lookups = lion_units_price(units, cx->antiplan.cost);
	cpath->path.startup_cost += lookups + child->total_cost;
	cpath->path.total_cost += lookups + child->total_cost;
#if PG_VERSION_NUM >= 180000
	cpath->path.disabled_nodes = child->disabled_nodes;
#endif
	return lookups;
}

/*
 * ... and of its parallel form: the child a participant runs
 * (lion_anti_parallel_child()), where the join's expressions may be
 * evaluated in a worker and the inner side has one, and into *run the price
 * of a participant's run - `lookups`, the lookups' price, and that
 * child's price.  NULL, and *run 0, when there is none.
 */
static Path *
lion_count_anti_parallel(const LionCountPathBuild *cx, Cost lookups,
						 Cost *run)
{
	Path	   *child;

	*run = 0.0;
	if (cx->anti == NULL ||
		!is_parallel_safe(cx->root, (Node *) cx->priv.anti.exprs))
		return NULL;
	child = lion_anti_parallel_child(cx->anti->innerrel);
	if (child != NULL)
		*run = lookups + child->total_cost;
	return child;
}

/*
 * The groups the executor takes in a batch of the walk of idx's entries
 * (lion_count_groups_batch()), which the slices' decode route reads the
 * slices once for (DESIGN.md §52).
 */
static int
lion_count_groups_batch_est(IndexOptInfo *idx)
{
	Relation	index;
	int			n;

	/* the planner holds the index locked since get_relation_info() */
	index = index_open(idx->indexoid, NoLock);
	n = lion_count_groups_batch(index);
	index_close(index, NoLock);
	return n;
}

/*
 * The price of counting `targets` (lion_count_path_price()), on cpath:
 * filtered counts (DESIGN.md §10, "Filtered counts") are a count of the
 * WHERE and one of the WHERE with each filter, and the node's price is
 * theirs added up - each filter one more intersection, or walk, with its
 * own clauses priced into it.
 */
static void
lion_count_path_price_runs(LionCountPathBuild *cx, CustomPath *cpath,
						   List *targets, LionRangeCost *rangeprice,
						   LionWalkSave *walksave)
{
	Cost		startup = 0;
	Cost		total = 0;
	LionRangeCost runs = {0};	/* a parallel count's, over the runs */
	int			r;

	if (cx->nfilter == 0)
	{
		lion_count_path_price(cx, cpath, targets, rangeprice, walksave,
							  false);
		return;
	}
	for (r = 0; r <= cx->nfilter; r++)
	{
		lion_count_path_price(cx, cpath,
							  lion_count_run_targets(cx, targets, r),
							  rangeprice, walksave,
							  lion_count_run_plain(cx, r));
		startup += cpath->path.startup_cost;
		total += cpath->path.total_cost;
		runs.countrange += rangeprice->countrange;
		runs.whole += rangeprice->whole;
		runs.pages += rangeprice->pages;
		runs.reads += rangeprice->reads;
	}
	cpath->path.startup_cost = startup;
	cpath->path.total_cost = total;
	rangeprice->countrange = runs.countrange;
	rangeprice->whole = runs.whole;
	rangeprice->pages = runs.pages;
	rangeprice->reads = runs.reads;
}

/*
 * The slices' work of the aggregates of sliced columns (DESIGN.md §52) over
 * relation t - the table, or one partition with its own indexes (§52,
 * "Partitioned tables") - priced onto cpath's price of counting it
 * (lion_cost_slice_path()).
 */
static void
lion_cost_slice_rel(LionCountPathBuild *cx, CustomPath *cpath,
					LionCountTarget *t)
{
	PlannerInfo *root = cx->root;
	RelOptInfo *rel = cx->partitioned ? t->rel : cx->input_rel;
	List	   *idxs = cx->sidx;
	List	   *cols = cx->scols;
	Cost		rangecollect = 0.0;

	/* a partition's own indexes, found for it (lion_count_path_slice_parts()) */
	if (cx->partitioned)
	{
		int			i;

		idxs = NIL;
		cols = NIL;
		for (i = 0; i < list_length(cx->sattnos); i++)
		{
			idxs = lappend(idxs, t->sliceidx[i]);
			cols = lappend_int(cols, (int) t->slicecol[i]);
		}
	}

	/*
	 * A range that bounds the sum over its entries is collected for the
	 * aggregates' WHERE (lion_slice_where_sources()), from its entries and
	 * summaries, as a range taken as a source is (§32).
	 */
	if (cx->sumall && cx->rangevar != NULL && t->driveidx[0] != NULL)
	{
		LionRangeMem mem = {0};
		List	   *bounds = NIL;
		bool		spills;
		bool		evicts;
		ListCell   *lc;

		foreach(lc, cx->rangeclauses)
			bounds = lappend(bounds, ((RestrictInfo *) lfirst(lc))->clause);
		rangecollect = lion_cost_range_source(root, rel, t->driveidx[0],
											  t->drivecol[0], bounds,
											  cx->rangesel, &mem, &spills,
											  &evicts);
		list_free(bounds);
	}
	lion_cost_slice_path(root, cpath, rel, idxs, cols, cx->swants, rel->rows,
						 cx->nwhere > 0 ||
						 cx->input_rel->baserestrictinfo != NIL,
						 cx->wcounts || cx->nfilter > 0 || cx->ngroup > 0,
						 (cx->ngroup > 0) ? cx->numgroups : 0.0,
						 cx->slicedecode ?
						 lion_count_groups_batch_est(t->driveidx[0]) : 0,
						 rangecollect);
	if (cx->partitioned)
	{
		list_free(idxs);
		list_free(cols);
	}
}

static void
lion_count_path_add(LionCountPathBuild *cx, CustomPath *cpath)
{
	PlannerInfo *root = cx->root;
	RelOptInfo *input_rel = cx->input_rel;
	RelOptInfo *output_rel = cx->output_rel;
	GroupPathExtraData *extra = cx->extra;
	LionCountTarget *first = cx->first;
	LionRangeCost rangeprice;	/* ... and what its ranges pay */
	LionWalkSave walksave = {0};	/* ... and what walking them would
									 * save */
	Cost		serialrun;		/* the serial node's price, without HAVING */
	Cost		antilookups;	/* the anti join's lookups (§27) */
	Cost		antirun;		/* ... and a participant's run of them */
	Path	   *antichild;		/* ... and the child it runs */
	LionUnits	units;			/* the competitor's (§39) */

	lion_units_for(root, output_rel, &units);
	if (lion_where_query_unknown(cx->whereclauses, cx->wherekinds,
								 cx->whereinor))
		units.margin = 1.0;

	lion_count_path_price_runs(cx, cpath, cx->targets, &rangeprice,
							   &walksave);

	/*
	 * The aggregates of sliced columns (DESIGN.md §52): their slices counted
	 * against the WHERE collected with each column's existence slice, after
	 * the counts - or in their place, with no count in the target list - and
	 * beside a GROUP BY, which counts its groups whatever the target list
	 * says, again against each group's sets.  Every range of their plan is
	 * collected, never walked (below).
	 *
	 * A partitioned table's are each partition's own, counted in its turn
	 * against its own WHERE (§52.13): priced as the sum
	 * of every partition's price as one table's - its count, by the same
	 * model over that partition alone, and its slices' work over its own
	 * pages, rows and indexes.
	 */
	if (cx->sattnos != NIL && !cx->partitioned)
		lion_cost_slice_rel(cx, cpath, first);
	else if (cx->sattnos != NIL)
	{
		Cost		startup = 0.0;
		Cost		total = 0.0;
		ListCell   *lc;

		foreach(lc, cx->targets)
		{
			LionCountTarget *t = (LionCountTarget *) lfirst(lc);
			LionRangeCost partprice;
			LionWalkSave partsave = {0};

			lion_count_path_price_runs(cx, cpath, list_make1(t), &partprice,
									   &partsave);
			list_free(partsave.clauses);
			lion_cost_slice_rel(cx, cpath, t);
			startup += cpath->path.startup_cost;
			total += cpath->path.total_cost;
		}
		cpath->path.startup_cost = startup;
		cpath->path.total_cost = total;
		cpath->path.rows = 1.0;
	}

	/*
	 * A range taken as a source (DESIGN.md §32) is collected - past the
	 * memory in windows into a temporary file - or walked at every count it
	 * is part of: the first is what the price takes, and the second is
	 * cheaper where there are few counts - one count, a few groups, the
	 * existence tests of a count(DISTINCT) - which the plan then says
	 * (LION_FLAG_RANGEWALK), for all of the node's ranges at once, naming
	 * the clauses of those it priced walked (LION_CLAUSE_WALK,
	 * lion_walk_save_choose()): only they are walked.  Not beside the
	 * decoded walk, whose WHERE is one copy, nor the top k, whose price
	 * leaves it out, nor where the aggregates over entries are all the node
	 * prices.
	 */
	lion_walk_save_choose(&walksave);
	if (walksave.save > 0 && !cx->decode && cx->topkn == 0 &&
		(cx->wattnos == NIL || cx->wcounts) && cx->sattnos == NIL)
	{
		bool		walks = false;
		int			i;

		for (i = 0; i < cx->priv.nclause; i++)
		{
			cx->priv.clause[i].walk =
				cx->priv.clause[i].kind == LION_CLAUSE_RANGESRC &&
				!cx->priv.clause[i].sliced &&
				list_member_ptr(walksave.clauses,
								list_nth(cx->whereclauses, i));
			walks |= cx->priv.clause[i].walk;
		}
		if (walks)
		{
			cpath->path.total_cost -= walksave.save;
			if (cpath->path.startup_cost > 0)
				cpath->path.startup_cost = Max(cpath->path.startup_cost -
											   walksave.save, 0.0);
			cx->priv.flags |= LION_FLAG_RANGEWALK;
			cpath->custom_private =
				lion_count_priv_encode(&cx->priv, LION_PRIV_STAGE_PATH);
		}
	}
	list_free(walksave.clauses);

	/*
	 * The residual clauses' test of each row the others select (§10): of
	 * the table's, or of each partition's that tests them (§16, "Residual
	 * clauses over a partitioned table"), each priced as its own heap.
	 */
	if (cx->residual != NIL)
	{
		/* ... once per run beside filtered counts, the WHERE's rows each */
		double		counts = (1 + cx->nfilter) *
			((cx->topkn > 0) ? (double) cx->topkcand : cx->numgroups);
		Cost		resid = 0.0;

		if (!cx->partitioned)
			resid = lion_cost_residual(root, input_rel, input_rel,
									   cx->residual, counts);
		else
		{
			ListCell   *lc;

			foreach(lc, cx->targets)
			{
				LionCountTarget *t = (LionCountTarget *) lfirst(lc);
				List	   *kept = lion_target_residual(cx->residual, t);

				if (kept != NIL)
					resid += lion_cost_residual(root, input_rel, t->rel, kept,
												counts);
			}
		}

		cpath->path.total_cost += resid;
		if (cpath->path.startup_cost > 0)
			cpath->path.startup_cost += resid;
	}

	/*
	 * Against a bitmap heap scan with cold reads (DESIGN.md §39, "A bitmap
	 * heap scan's cold reads"), the count's own reads beside them: its
	 * directory leaves, posting pages and heap recheck, which its price
	 * counted (rangeprice.reads), and the heap its residual clauses read.
	 * Only where the price counts them - one relation's count, of its WHERE
	 * or its groups' sets - and not the decoded walk's, the top k's, the
	 * aggregates over entries', a partitioned table's, or beside an anti
	 * join, whose prices leave them out.
	 */
	if (units.bmcold > 0.0 && list_length(cx->targets) == 1 &&
		!cx->decode && cx->topkn == 0 && cx->wattnos == NIL &&
		cx->sattnos == NIL &&
		cx->anti == NULL && cx->fj == NULL)
	{
		double		reads = rangeprice.reads;

		if (cx->residual != NIL)
		{
			LionCountTarget *t = (LionCountTarget *) linitial(cx->targets);
			List	   *kept = cx->partitioned ?
				lion_target_residual(cx->residual, t) : cx->residual;

			if (kept != NIL)
				reads += lion_residual_pages(root, input_rel, t->rel, kept,
											 (1 + cx->nfilter) *
											 cx->numgroups);
		}
		lion_units_bitmap_cold(&units, cpath->path.total_cost, reads);
	}

	cpath->path.startup_cost = lion_units_price(&units,
												cpath->path.startup_cost);
	cpath->path.total_cost = lion_units_price(&units, cpath->path.total_cost);
	rangeprice.perrange = lion_units_price(&units, rangeprice.perrange);
	rangeprice.countrange = lion_units_price(&units, rangeprice.countrange);
	rangeprice.whole = lion_units_price(&units, rangeprice.whole);
	serialrun = cpath->path.total_cost;

	/* the anti join over a small inner side (DESIGN.md §27) */
	antilookups = lion_count_anti_price(cx, &units, cpath);

	/*
	 * The HAVING the node applies itself costs an evaluation per group and
	 * lets a fraction of the groups through: the same accounting cost_agg()
	 * does for an Agg's quals, so that the two plans stay comparable.  A
	 * partitioned table's HAVING is the Finalize Agg's and is priced there.
	 */
	if (cx->having != NIL && cx->partialtarget == NULL)
	{
		QualCost	qual_cost;
		double		groups = cpath->path.rows;

		cost_qual_eval(&qual_cost, cx->having, root);
		cpath->path.startup_cost += qual_cost.startup;
		cpath->path.total_cost += qual_cost.startup +
			groups * qual_cost.per_tuple;
		cpath->path.rows = clamp_row_est(groups *
										 clauselist_selectivity(root,
																cx->having,
																0, JOIN_INNER,
																NULL));
	}

	/*
	 * Never at or within core's fuzz above the AM's own scan of the same sets
	 * feeding an Agg (DESIGN.md §39, "The AM's own scan of the same sets"),
	 * which does what the count does and fetches every row besides: whatever
	 * the rows are estimated at, which price the scan's heap and the count's
	 * visibility tests both, the count is the cheaper.
	 */
	{
		Cost		ownscan = lion_count_ownscan_cost(cx);

		if (ownscan > 0.0)
			elog(DEBUG2, "lion: count priced %.2f against the AM's own scan of its sets at %.2f",
				 cpath->path.total_cost, ownscan);
		/*
		 * ... a count's, which needs nothing of the rows: the aggregates of
		 * sliced columns (DESIGN.md §52) are made of many counts, and the
		 * scan's rows may be fewer than their work
		 */
		if (ownscan > 0.0 && cx->sattnos == NIL &&
			cpath->path.total_cost * LION_COUNT_OWNSCAN_EDGE > ownscan)
		{
			cpath->path.total_cost = ownscan / LION_COUNT_OWNSCAN_EDGE;
			cpath->path.startup_cost = Min(cpath->path.startup_cost,
										   cpath->path.total_cost);
		}
	}

	/*
	 * The node answers from its indexes without reading the rows it counts,
	 * as an index-only scan does: core's switches for those disable it (and
	 * the parallel form below, which prices itself from serialrun).
	 */
	lion_path_core_switches(&cpath->path, LION_CORE_INDEXONLY);

	/*
	 * A partitioned GROUP BY produces partial aggregates, so what goes into
	 * the grouped rel is core's Finalize HashAggregate over the node - which
	 * combines the per-partition counts and, unlike anything this node could
	 * hold, spills to disk when the groups do not fit in hash_mem
	 * (DESIGN.md §16).  Everything else is already the finished answer.
	 */
	if (cx->partialtarget != NULL)
	{
		AggClauseCosts agg_final_costs;

		MemSet(&agg_final_costs, 0, sizeof(agg_final_costs));
		get_agg_clause_costs(root, AGGSPLIT_FINAL_DESERIAL, &agg_final_costs);

		add_path(output_rel, (Path *)
				 create_agg_path(root, output_rel, &cpath->path,
								 output_rel->reltarget,
								 AGG_HASHED, AGGSPLIT_FINAL_DESERIAL,
								 root->processed_groupClause,
								 cx->having,
								 &agg_final_costs,
								 cx->numgroups));
		return;
	}

	/*
	 * ... and, where the query may run in parallel, the same GROUP BY divided
	 * among the participants of a Gather by ranges of container keys, below
	 * core's Finalize HashAggregate (DESIGN.md §10, "A GROUP BY in parallel"):
	 * one grouping column walked whole, a WHERE that is collected - priced as
	 * the groups of a walk counted together (rangeprice.batched), with a
	 * source that is positive whatever its value, and no range taken as one -
	 * and partial counts core can add up.  A GROUP BY coalesce(col, c) is
	 * one too: its two entries go up as partial rows of c, which the Agg
	 * adds together.  Before the serial path is added, which may free it.
	 */
	if (cx->ngroup == 1 && !cx->sumall && !cx->singlegroup &&
		cx->nfilter == 0 && cx->anti == NULL && cx->sattnos == NIL &&
		cx->distvar == NULL && cx->rangevar == NULL && !cx->hasrangesrc &&
		cx->plainpositive && rangeprice.batched && cx->topkn == 0 &&
		cx->groupdeps == NIL && cx->ncompl == 0 && cx->residual == NIL &&
		first->driveidx[0] != NULL &&
		input_rel->consider_parallel && output_rel->consider_parallel &&
		is_parallel_safe(root, (Node *) cx->priv.consts) &&
		extra != NULL && (extra->flags & GROUPING_CAN_PARTIAL_AGG) != 0 &&
		grouping_is_hashable(root->processed_groupClause) &&
		!RecoveryInProgress())
		lion_add_parallel_group_path(root, input_rel, output_rel, cpath,
									 serialrun, &rangeprice,
									 first->driveidx[0], first->drivecol[0],
									 cx->having, cx->numgroups);

	/*
	 * ... and a count of one row of one table, divided among them the same
	 * way (DESIGN.md §10, "A count in parallel"): the merge of its WHERE or a
	 * sum over a column's entries, with whatever filtered counts, residual
	 * clauses, complement ORs, ranges taken as sources or a range bounding
	 * the sum beside it - each range counted as the serial node counts the
	 * whole.  Not a count(DISTINCT), the aggregates over keys, the FK-side
	 * join or a partitioned table, which count in other ways, nor a group
	 * folded to a constant, whose missing row partial counts cannot say.  The
	 * anti join over a small inner side is one, its child and its lookups
	 * the work of one participant (§27), when both may run in a worker.
	 */
	antichild = lion_count_anti_parallel(cx, antilookups, &antirun);
	if (cx->ngroup == 0 && !cx->singlegroup && !cx->partitioned &&
		cx->fj == NULL && cx->distvar == NULL && cx->wattnos == NIL &&
		cx->sattnos == NIL &&
		cx->topkn == 0 && !cx->decode && cx->groupcoal == NULL &&
		input_rel->consider_parallel && output_rel->consider_parallel &&
		is_parallel_safe(root, (Node *) cx->priv.consts) &&
		(cx->anti == NULL || antichild != NULL) &&
		extra != NULL && (extra->flags & GROUPING_CAN_PARTIAL_AGG) != 0 &&
		!RecoveryInProgress())
		lion_add_parallel_count_path(root, input_rel, output_rel, cpath,
									 serialrun, &rangeprice, cx->having,
									 antichild, antirun);

	add_path(output_rel, &cpath->path);
}

/*
 * COUNTS BY CELLS (DESIGN.md §10, "Counts by cells").
 *
 * A GROUP BY of expressions of the table's columns - `CASE WHEN score >= 70
 * THEN 'high' ... END` over a score made of `col = c`, `tags && '{x}'`, `x
 * IS NULL` and their like - groups each row by the truths of the predicates
 * it is made of, as the plan's projection would compute it from them.  So
 * the WHERE's rows are counted by CELL - a truth of every predicate, true,
 * false or NULL - from the posting sets that decide each predicate in every
 * row, and the expressions are computed once a cell, from the cell's truths,
 * by the node's own projection: the predicates are the columns it emits
 * (LION_TL_CELLPRED), each cell's partial count beside them, and core's
 * Finalize Agg adds the cells of one group up.  A WHERE clause over the
 * predicates - `... AND CASE ... END = 'high'` - is the node's qual, tested
 * once a cell.
 *
 * The phases are the decoded walk's, of one column - the driver
 * (lion_cells_choose_driver()) - with the GROUP BY, the outputs and the
 * strategy of their own; lion_cells_analyze() finds the predicates and the
 * cells they make, and declines more than pg_lion.count_cells_max of them.
 */

/* Does node compute a value from truths - CASE, GREATEST or LEAST, COALESCE? */
static bool
lion_cells_candidate_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, CaseExpr) || IsA(node, MinMaxExpr) ||
		IsA(node, CoalesceExpr))
		return true;
	return expression_tree_walker(node, lion_cells_candidate_walker, context);
}

/*
 * The GROUP BY's expressions, none of them a bare column - whose groups are
 * the walk's own - on one plain table whose counts core may add up; or no
 * GROUP BY at all, when the WHERE has a clause the cells may test.  The
 * table is the query's only one, or the one an anti join over a small inner
 * side counts (§27), whose rows found the cells lose
 * (lion_count_path_anti()): not the fact of an FK-side join.
 */
static bool
lion_count_cells_group_by(LionCountPathBuild *cx)
{
	PlannerInfo *root = cx->root;
	ListCell   *lc;

	if (cx->fj != NULL || cx->partitioned ||
		cx->distinctstage || cx->extra == NULL ||
		(cx->extra->flags & GROUPING_CAN_PARTIAL_AGG) == 0 ||
		RecoveryInProgress())
		return false;
	if (root->processed_groupClause == NIL)
	{
		ListCell   *lr;
		bool		any = false;

		/* a GROUP BY folded to one group has nothing to split */
		if (cx->parse->groupClause != NIL)
			return false;

		/* the WHERE form: a clause with an expression no set answers */
		foreach(lr, cx->input_rel->baserestrictinfo)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(lr);

			if (IsA(rinfo, RestrictInfo) &&
				lion_cells_candidate_walker((Node *) rinfo->clause, NULL) &&
				lion_cells_shape_ok(root, cx->input_rel, cx->rti,
									(Node *) rinfo->clause))
				any = true;
		}
		return any;
	}
	if (!grouping_is_hashable(root->processed_groupClause))
		return false;
	foreach(lc, root->processed_groupClause)
	{
		SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
		TargetEntry *tle = get_sortgroupclause_tle(sgc, root->processed_tlist);
		Node	   *e;

		if (tle == NULL)
			return false;
		e = (Node *) tle->expr;
		if (lion_strip(e) == NULL || IsA(lion_strip(e), Var) ||
			!lion_cells_shape_ok(root, cx->input_rel, cx->rti, e))
			return false;
		cx->cellexprs = lappend(cx->cellexprs, e);
	}
	return true;
}

/*
 * The target list and the HAVING: the GROUP BY's expressions, and counts the
 * node answers - count(*), and the count(col) of a column every row counted
 * has non-NULL, or NULL - and expressions over them; no column outside the
 * GROUP BY's expressions, which the cells do not keep.
 */
static bool
lion_count_cells_outputs(LionCountPathBuild *cx)
{
	List	   *exprs = list_copy(cx->output_rel->reltarget->exprs);
	bool		haveagg = false;
	ListCell   *lc;

	if (cx->having != NIL)
		exprs = lappend(exprs, cx->having);
	foreach(lc, exprs)
	{
		Node	   *node = (Node *) lfirst(lc);
		List	   *vars;
		ListCell   *lv;

		if (list_member(cx->cellexprs, node))
			continue;
		if (contain_subplans(node) || contain_window_function(node))
			return false;
		vars = pull_var_clause(node, PVC_INCLUDE_AGGREGATES |
							   PVC_INCLUDE_PLACEHOLDERS);
		foreach(lv, vars)
		{
			Aggref	   *agg = (Aggref *) lfirst(lv);

			if (!IsA(agg, Aggref) ||
				agg->aggdistinct != NIL || agg->aggorder != NIL ||
				agg->aggfilter != NULL || agg->aggsplit != AGGSPLIT_SIMPLE ||
				!lion_agg_is_count(cx->root, agg, cx->rti, cx->input_rel,
								   NULL, 0, cx->nonnullattnos,
								   cx->nullattnos))
				return false;
			haveagg = true;
		}
	}
	return haveagg || cx->cellexprs != NIL;
}

/*
 * The WHERE as the decoded walk reads it (lion_count_path_strategy()): one
 * collected copy, every range in it a source, a positive clause to begin
 * from, no complement OR or residual clause, and none of what the heap scan
 * of a WHERE that narrows nothing does not test (lion_where_scan_untested()).
 */
static bool
lion_count_cells_strategy(LionCountPathBuild *cx)
{
	ListCell   *lc;

	if (cx->ncompl > 0 || cx->residual != NIL || cx->nfilter > 0)
		return false;
	if (lion_where_scan_untested(cx, true))
		return false;
	if (cx->rangevars != NIL)
	{
		lion_ranges_to_sources(cx->rangevars, cx->rangepos, -1,
							   cx->wherekinds, cx->clauseinfos);
		cx->hasrangesrc = true;
		cx->havepositive = true;
	}
	if (cx->wherekinds != NIL && !cx->havepositive)
		return false;
	foreach(lc, cx->clauseinfos)
		((LionClauseInfo *) lfirst(lc))->valueout = false;
	return true;
}

/*
 * A PARALLEL COUNT BY CELLS: the serial node made parallel-aware below a
 * Gather and the Finalize Agg, its participants claiming ranges of
 * container keys as a parallel GROUP BY's do (DESIGN.md §10, "A GROUP BY in
 * parallel") and counting every pass of the driver's walk in each, into one
 * tally of their own whose cells go up at the end.  WORKERS as for a
 * parallel scan of the heap, whose rows the count replaces reading; COST a
 * participant's share of the serial count and what each of its ranges pays
 * again (perrange), plus a cpu_tuple_cost a partial row.  Beside the anti
 * join over a small inner side (§27) every participant runs `child` and
 * looks its keys up, taking away the rows of the ranges it counts: `once`,
 * their price, is each participant's whole, before its first row - and as
 * many runs of it as there are participants, at the pace of the divisor's,
 * where that is more: a leader that takes part makes the run as a worker
 * does, though the divisor counts it as a part of one.  The runs are one
 * participant's work repeated, not shared out: they compete for the cores
 * the count's share is divided among, and a worker that starts late still
 * makes its whole run before it finds whether a range is left to count,
 * which the Gather waits for.  Measured (2026-10-23, release build, 3M rows
 * in 40,000 pages, 2,000 to 20,000 keys of three rows each, a box other work
 * kept busy), every participant's run made the parallel form 1.3 to 2.5
 * times the serial node's time, where without the anti join the two were
 * even or the parallel form faster: with its share of the count small
 * against the run, the serial node is the safer plan.
 */
static void
lion_add_parallel_cells_path(LionCountPathBuild *cx, CustomPath *serial,
							 Cost serialrun, Cost perrange,
							 const AggClauseCosts *costs, Path *child,
							 Cost once)
{
	PlannerInfo *root = cx->root;
	RelOptInfo *rel = cx->input_rel;
	RelOptInfo *output_rel = cx->output_rel;
	CustomPath *cpath;
	Path	   *gather;
	double		divisor;
	double		rows;
	double		totalrows;
	uint32		ckeys;
	int			workers;
	int			nranges;

	workers = compute_parallel_worker(rel, (double) rel->pages, -1,
									  max_parallel_workers_per_gather);
	if (workers <= 0)
		return;
	divisor = lion_parallel_divisor(workers);
	nranges = lion_key_ranges(rel->pages, workers + 1,
							  LION_PARALLEL_RANGE_KEYS, &ckeys);
	totalrows = clamp_row_est((workers + 1) *
							  lion_cells_nonempty(cx->cellplan,
												  cx->cellwhererows /
												  (workers + 1)));
	rows = clamp_row_est(totalrows / divisor);
	once *= Max((workers + (parallel_leader_participation ? 1.0 : 0.0)) /
				divisor, 1.0);

	cpath = makeNode(CustomPath);
	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = output_rel;
	cpath->path.pathtarget = serial->path.pathtarget;
	cpath->path.param_info = NULL;
	cpath->path.parallel_aware = true;
	cpath->path.parallel_safe = true;
	cpath->path.parallel_workers = workers;
	cpath->path.pathkeys = NIL;
	cpath->path.rows = rows;
	cpath->path.startup_cost = serialrun / divisor +
		((double) nranges / divisor) * perrange + once;
	cpath->path.total_cost = cpath->path.startup_cost +
		rows * cpu_tuple_cost;
#if PG_VERSION_NUM >= 180000
	cpath->path.disabled_nodes = (child != NULL) ? child->disabled_nodes : 0;
#endif
	cpath->flags = serial->flags;
	cpath->custom_paths = (child != NULL) ? list_make1(child) : NIL;
#if PG_VERSION_NUM >= 170000
	cpath->custom_restrictinfo = NIL;
#endif
	cpath->custom_private = serial->custom_private;
	cpath->methods = serial->methods;
	lion_path_core_switches(&cpath->path, LION_CORE_INDEXONLY);

	gather = (Path *) create_gather_path(root, output_rel, &cpath->path,
										 serial->path.pathtarget, NULL,
										 &totalrows);
	add_path(output_rel, (Path *)
			 create_agg_path(root, output_rel, gather,
							 output_rel->reltarget,
							 (root->processed_groupClause != NIL) ?
							 AGG_HASHED : AGG_PLAIN,
							 AGGSPLIT_FINAL_DESERIAL,
							 root->processed_groupClause,
							 cx->having, costs, cx->numgroups));
}

/*
 * Price the count by cells and add it, below core's Finalize Agg - a hashed
 * one by the GROUP BY's expressions, or a plain one - and its parallel form
 * where the query may run in parallel.  The price is the node's own in the
 * units of the cheapest core path (DESIGN.md §39), the qual over the
 * predicates evaluated once a cell; beside the anti join over a small inner
 * side (§27) its lookups, priced as the count is, and its child, core's
 * plan in core's units, both before the first row.
 */
static void
lion_count_cells_add(LionCountPathBuild *cx, CustomPath *cpath)
{
	PlannerInfo *root = cx->root;
	RelOptInfo *rel = cx->input_rel;
	RelOptInfo *output_rel = cx->output_rel;
	AggClauseCosts agg_final_costs;
	LionUnits	units;
	Cost		perrange = 0;
	Cost		serialrun;
	Cost		antilookups;
	Cost		antirun;
	Path	   *antichild;

	lion_units_for(root, output_rel, &units);
	if (lion_where_query_unknown(cx->whereclauses, cx->wherekinds,
								 cx->whereinor))
		units.margin = 1.0;
	lion_cost_cells_path(root, cpath, cx->targets, cx->whereclauses,
						 cx->wherekinds, cx->ors, cx->cellplan,
						 cx->cellwhererows, cx->outrows, &perrange);
	if (cx->cellquals != NIL)
	{
		QualCost	qual_cost;

		cost_qual_eval(&qual_cost, cx->cellquals, root);
		cpath->path.startup_cost += qual_cost.startup;
		cpath->path.total_cost += qual_cost.startup +
			cx->outrows * qual_cost.per_tuple;
	}
	cpath->path.startup_cost = lion_units_price(&units,
												cpath->path.startup_cost);
	cpath->path.total_cost = lion_units_price(&units, cpath->path.total_cost);
	perrange = lion_units_price(&units, perrange);
	serialrun = cpath->path.total_cost;
	antilookups = lion_count_anti_price(cx, &units, cpath);

	/* answered from the indexes alone: core's index-only switches apply */
	lion_path_core_switches(&cpath->path, LION_CORE_INDEXONLY);

	MemSet(&agg_final_costs, 0, sizeof(agg_final_costs));
	get_agg_clause_costs(root, AGGSPLIT_FINAL_DESERIAL, &agg_final_costs);

	/*
	 * ... in parallel first, add_path() may free the serial path: a WHERE
	 * collected range by range, with a positive clause outside ORs and no
	 * range taken as a source, or none at all; and the anti join's child a
	 * path a worker may run, its clauses ones it may evaluate.
	 */
	antichild = lion_count_anti_parallel(cx, antilookups, &antirun);
	if (((cx->plainpositive && !cx->hasrangesrc) || cx->wherekinds == NIL) &&
		rel->consider_parallel && output_rel->consider_parallel &&
		is_parallel_safe(root, (Node *) cx->priv.consts) &&
		is_parallel_safe(root, (Node *) cx->cellplan->predexprs) &&
		is_parallel_safe(root, (Node *) cx->cellquals) &&
		is_parallel_safe(root, (Node *) cpath->path.pathtarget->exprs) &&
		(cx->anti == NULL || antichild != NULL))
		lion_add_parallel_cells_path(cx, cpath, serialrun, perrange,
									 &agg_final_costs, antichild, antirun);

	add_path(output_rel, (Path *)
			 create_agg_path(root, output_rel, &cpath->path,
							 output_rel->reltarget,
							 (root->processed_groupClause != NIL) ?
							 AGG_HASHED : AGG_PLAIN,
							 AGGSPLIT_FINAL_DESERIAL,
							 root->processed_groupClause,
							 cx->having, &agg_final_costs, cx->numgroups));
}

/*
 * The build of a count by cells, from the start of the scoped phases
 * (lion_try_count_path_scoped()): the GROUP BY's expressions, the WHERE with
 * its clauses over predicates, the outputs, the strategy, the predicates and
 * the driver - and then the decoded walk's targets, fill and path, of the
 * driver's column alone.
 */
static void
lion_try_count_cells(const LionCountPathBuild *cxp)
{
	LionCountPathBuild cx = *cxp;
	PlannerInfo *root = cx.root;
	RelOptInfo *rel = cx.input_rel;
	LionCellsPlan *cp;
	Var		   *fallback;
	double		fbentries = 0;
	List	   *quals = NIL;
	CustomPath *cpath;
	ListCell   *lc;

	if (!lion_enable_count_cells || lion_partial_active())
		return;
	if (!lion_count_cells_group_by(&cx))
		return;
	cx.cells = true;
	if (!lion_count_path_where(&cx))
		return;
	if (cx.cellexprs == NIL && cx.cellquals == NIL)
		return;
	if (!lion_count_cells_outputs(&cx))
		return;
	if (!lion_count_cells_strategy(&cx))
		return;

	/* the predicates, the cells they make, and the column that drives */
	cp = lion_cells_analyze(root, rel, cx.rti,
							list_concat_copy(cx.cellexprs, cx.cellquals),
							false);
	if (cp == NULL)
		return;
	fallback = lion_count_all_driver(&cx);
	if (fallback != NULL)
		fbentries = estimate_num_groups(root, list_make1(fallback),
										Max(rel->tuples, 1.0), NULL, NULL) +
			1.0;
	if (!lion_cells_choose_driver(root, rel, cp, fallback, fbentries))
		return;

	/* ... the decoded walk's one column, which groups nothing */
	cx.groupvar[0] = cp->drvvar;
	cx.groupattno[0] = cp->drvvar->varattno;
	cx.groupeqop[0] = (cp->drv >= 0) ? cp->dims[cp->drv].priv.eqop : InvalidOid;
	cx.groupcoll[0] = (cp->drv >= 0) ?
		cp->dims[cp->drv].priv.collation : InvalidOid;
	cx.groupvalueout[0] = false;
	cx.ngroup = 1;
	cx.decode = true;
	cx.driveattno = cx.groupattno[0];
	if (!lion_count_path_targets(&cx))
		return;
	if (list_length(cx.targets) != 1 || cx.first->driveidx[0] == NULL)
		return;
	if (cp->drv >= 0)
		cp->dims[cp->drv].idx = cx.first->driveidx[0];
	cx.cellplan = cp;

	/*
	 * The rows the WHERE's sets select - the clauses over predicates test
	 * cells - and a partial row a cell they are in (lion_cells_nonempty()),
	 * under the Finalize Agg; the groups at most those cells.
	 */
	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		if (!list_member_ptr(cx.cellquals, rinfo->clause))
			quals = lappend(quals, rinfo);
	}
	cx.cellwhererows = clamp_row_est(Max(rel->tuples, 1.0) *
									 clauselist_selectivity(root, quals,
															cx.rti,
															JOIN_INNER, NULL));
	cx.outrows = clamp_row_est(lion_cells_nonempty(cp, cx.cellwhererows));
	cx.numgroups = (cx.cellexprs != NIL) ?
		clamp_row_est(Min(estimate_num_groups(root, cx.cellexprs,
											  cx.cellwhererows, NULL, NULL),
						  cx.outrows)) : 1.0;
	cx.partialtarget = lion_make_partial_target(root,
												cx.output_rel->reltarget,
												cx.having);

	lion_count_path_fill(&cx);
	if (cx.priv.vcols != NIL)
		return;					/* an expression column: not beside cells */
	lion_cells_fill_priv(cp, &cx.priv, cx.cellquals);

	/*
	 * The predicates' functions, which core's plan would call computing the
	 * GROUP BY's expressions and the node answers from posting sets
	 * (lion_replaced_functions(); the clauses over them are the WHERE's,
	 * there already).
	 */
	foreach(lc, cp->predexprs)
		cx.priv.exec_funcs = lion_expr_functions((Node *) lfirst(lc),
												 cx.priv.exec_funcs);

	/* ... and an anti join's rows found, each taken from its cell (§27) */
	if (!lion_count_path_anti(&cx))
		return;
	cpath = lion_count_path_make(&cx);
	lion_count_cells_add(&cx, cpath);
}

/*
 * May the aggregates of sliced columns of this GROUP BY be decoded a batch of
 * groups at a time (DESIGN.md §52, "The decode route")?  The groups of one
 * column's walk of its entries, which the executor takes a batch at a time:
 * not a coalesce group, not the top k, and not where a WHERE clause names the
 * grouping column - an IN list or an equality there drives the groups by its
 * values, one at a time (§15) - and not on a hot standby, where the slices
 * cannot be read without their pins.
 */
static bool
lion_count_path_slices_decodable(LionCountPathBuild *cx)
{
	return lion_enable_slice_decode &&
		cx->slicemode && cx->sattnos != NIL && cx->ngroup == 1 &&
		!cx->decode && cx->groupcoal == NULL && cx->topkn == 0 &&
		cx->groupattno[0] > 0 &&
		!list_member_int(cx->whereattnos, cx->groupattno[0]) &&
		cx->first != NULL && cx->first->driveidx[0] != NULL &&
		!RecoveryInProgress();
}

/*
 * May a GROUP BY of two columns, made the nested loop of DESIGN.md §20, be
 * the decoded walk of §34 as well?  On the terms of three columns
 * (lion_count_path_group_by(), lion_count_path_strategy()): one table, not a
 * partitioned one, partial counts core can add up, no count(DISTINCT) or
 * coalesce group, nothing the heap scan of a WHERE that narrows nothing does
 * not test (lion_where_scan_untested()), and not on a hot standby.  A range
 * in the WHERE is a source already, as every range beside two grouping
 * columns is.
 */
static bool
lion_count_path_decodable(LionCountPathBuild *cx)
{
	return lion_enable_decoded_walk &&
		cx->ngroup == 2 && !cx->decode && cx->fj == NULL &&
		cx->sattnos == NIL &&
		cx->anti == NULL &&
		!cx->partitioned && cx->distvar == NULL && cx->groupcoal == NULL &&
		cx->groupdeps == NIL && !cx->sumall && !cx->singlegroup &&
		cx->ncompl == 0 && cx->residual == NIL &&
		!lion_where_scan_untested(cx, true) &&
		cx->rangevar == NULL &&
		(cx->nwhere == 0 || cx->havepositive) &&
		(cx->nfilter == 0 || cx->filterdecode) &&
		cx->first != NULL && cx->first->driveidx[0] != NULL &&
		cx->first->driveidx[1] != NULL &&
		cx->extra != NULL &&
		(cx->extra->flags & GROUPING_CAN_PARTIAL_AGG) != 0 &&
		grouping_is_hashable(cx->root->processed_groupClause) &&
		!RecoveryInProgress();
}

/*
 * The rest of lion_try_count_path() for one build: inside the scope of the
 * relation's expression columns.  The expressions of the table's lion
 * indexes are columns from here on (DESIGN.md §41): of one table, which no
 * FK-side join reads.  A partition numbers its own Vars, and its indexes'
 * expressions are its own, so a partitioned table has none.
 */
static void
lion_try_count_path_vcols(LionCountPathBuild *cxp)
{
	LionCountPathBuild cx = *cxp;
	LionVColScope saved;

	if (cx.fj == NULL && !cx.partitioned)
		cx.vcols = lion_vcol_collect(cx.input_rel);
	saved = lion_vcol_enter(cx.vcols, cx.rti);
	PG_TRY();
	{
		lion_try_count_path_scoped(&cx);
	}
	PG_FINALLY();
	{
		lion_vcol_leave(saved);
	}
	PG_END_TRY();
}

/*
 * One more build of lion_try_count_path(), with each relation's rank'th
 * partial index (DESIGN.md §45), when one has any.
 */
static void
lion_try_count_path_partial(LionCountPathBuild *cx,
							const LionPartialCands *cands, int rank)
{
	void	   *saved;

	if (!lion_partial_enter(cands, rank, &saved))
		return;
	PG_TRY();
	{
		lion_try_count_path_vcols(cx);
	}
	PG_FINALLY();
	{
		lion_partial_leave(saved);
	}
	PG_END_TRY();
}

/*
 * Decide whether count(*) over input_rel can be answered from roaring
 * posting sets and, if so, add a CustomPath to output_rel.  Every failed
 * check simply returns: the normal plan is always available.
 *
 * fj is the FK-side join of DESIGN.md §27, or NULL.  With it, input_rel is
 * the JOIN rel and everything below about "the relation" - its WHERE clauses
 * and their indexes - is asked of the FACT rel, fj->factrel, unchanged; what
 * differs (the join key, the target list, the path) is lion_try_fkjoin_path().
 *
 * It runs in phases, each a function that adds to one LionCountPathBuild
 * or returns false to end it there: a check that failed, or the FK-side join
 * taking over.
 */
static void
lion_try_count_path_ext(PlannerInfo *root, RelOptInfo *input_rel,
						RelOptInfo *output_rel, GroupPathExtraData *extra,
						const LionFkJoin *fj, const LionSmallAnti *anti,
						bool distinctstage)
{
	LionCountPathBuild cx;
	LionPartialCands *cands;
	int			rank;

	lion_count_path_init(&cx, root, input_rel, output_rel, extra, fj);
	cx.anti = anti;
	cx.distinctstage = distinctstage;

	if (!lion_count_path_query(&cx))
		return;
	if (!lion_count_path_rel(&cx))
		return;

	/* with the relation's whole indexes */
	lion_try_count_path_vcols(&cx);

	/*
	 * ... and once more with each partial index whose predicate the
	 * restrictions imply (DESIGN.md §45), the smallest first: every lookup
	 * tries that index first, the restrictions it implies are left out, and
	 * the count is held to its rows.  add_path() keeps the cheapest.
	 */
	if (!lion_enable_count_partial)
		return;
	cands = lion_partial_find(root, cx.input_rel);
	for (rank = 0; rank < Min(lion_partial_ncands(cands), LION_PARTIAL_TRIES);
		 rank++)
		lion_try_count_path_partial(&cx, cands, rank);
}

void
lion_try_count_path(PlannerInfo *root, RelOptInfo *input_rel,
				   RelOptInfo *output_rel, GroupPathExtraData *extra,
				   const LionFkJoin *fj)
{
	lion_try_count_path_ext(root, input_rel, output_rel, extra, fj, NULL,
							false);
}

/*
 * The anti join over a small inner side (DESIGN.md §27, "The anti join over a
 * small inner side"): input_rel is the query's join rel, and `anti` what
 * lion_fkjoin_recognize_small_anti() found of it.  The count is planned as
 * the outer table's own, every phase unchanged, and lion_count_path_anti()
 * adds the subtraction.
 */
void
lion_try_count_anti_path(PlannerInfo *root, RelOptInfo *input_rel,
						 RelOptInfo *output_rel, GroupPathExtraData *extra,
						 const LionSmallAnti *anti)
{
	lion_try_count_path_ext(root, input_rel, output_rel, extra, NULL, anti,
							false);
}

/*
 * SELECT DISTINCT over one relation, with no GROUP BY, aggregate, window or
 * set-returning function (DESIGN.md §10, "Shapes rewritten to the node's
 * counts"): the distinct keys are the groups of a GROUP BY of the same
 * columns with no aggregate, and the path is that GROUP BY's, added to the
 * DISTINCT's upper rel.  The build reads the grouping from
 * root->processed_groupClause, parse->groupClause and output_rel's target,
 * so for its duration those are the DISTINCT's: its clauses as core kept
 * them (processed_distinctClause, columns a WHERE equality pins left out,
 * as a GROUP BY's are), and the target of the rows below, which the upper
 * rel does not set - core's own DISTINCT paths carry their input's target.
 * The grouping extra is what core would hand a GROUP BY that may be
 * partially aggregated: there is nothing to aggregate.
 */
void
lion_try_distinct_path(PlannerInfo *root, RelOptInfo *input_rel,
					   RelOptInfo *output_rel)
{
	Query	   *parse = root->parse;
	GroupPathExtraData extra;
	List	   *savedprocessed = root->processed_groupClause;
	List	   *savedgroup = parse->groupClause;
	PathTarget *savedtarget = output_rel->reltarget;

	if (parse->distinctClause == NIL || parse->hasDistinctOn ||
		parse->groupClause != NIL || parse->groupingSets != NIL ||
		parse->hasAggs || parse->hasWindowFuncs || parse->hasTargetSRFs ||
		root->hasHavingQual)
		return;

	memset(&extra, 0, sizeof(extra));
	extra.flags = GROUPING_CAN_PARTIAL_AGG;
	extra.patype = PARTITIONWISE_AGGREGATE_NONE;
	extra.havingQual = NULL;
	extra.targetList = root->processed_tlist;

	root->processed_groupClause = root->processed_distinctClause;
	parse->groupClause = parse->distinctClause;
	output_rel->reltarget = input_rel->reltarget;
	PG_TRY();
	{
		lion_try_count_path_ext(root, input_rel, output_rel, &extra, NULL,
								NULL, true);
	}
	PG_FINALLY();
	{
		root->processed_groupClause = savedprocessed;
		parse->groupClause = savedgroup;
		output_rel->reltarget = savedtarget;
	}
	PG_END_TRY();
}

/*
 * The rest of lion_try_count_path(), inside the scope of the relation's
 * expression columns.
 */
/*
 * One attempt at the path, phase by phase: false where a phase declines it.
 * Over a partitioned table an attempt may decline at the partitions, where
 * one can neither answer a restriction nor leave it out, and ask for it to
 * be residual instead (cx->imply.wantresid; DESIGN.md §16, "Residual
 * clauses over a partitioned table"): the caller's to try once more.
 */
static bool
lion_try_count_path_once(LionCountPathBuild *cx)
{
	CustomPath *cpath;

	if (!lion_count_path_group_by(cx))
		return false;
	if (!lion_count_path_where(cx))
		return false;
	if (!lion_count_path_fkjoin(cx))
		return false;
	if (!lion_count_path_having(cx))
		return false;
	if (!lion_count_path_distinct(cx))
		return false;
	if (!lion_count_path_countcol(cx))
		return false;
	if (!lion_count_path_outputs(cx))
		return false;
	if (!lion_count_path_strategy(cx))
		return false;
	if (!lion_count_path_filters(cx))
		return false;
	/* §37's aggregates beside a sum over every row, and nothing else */
	if (cx->wattnos != NIL && !cx->sumall)
		return false;
	/* ... and §52's beside a WHERE they can collect */
	if (!lion_count_path_slices_ok(cx))
		return false;
	if (!lion_count_path_targets(cx))
		return false;
	/* ... from every partition's own slices (§52.13) */
	if (!lion_count_path_slice_parts(cx))
		return false;
	/* ... and the ranges from every partition's own slices (§53) */
	if (!lion_count_path_slice_range_parts(cx))
		return false;

	lion_count_path_estimate(cx);
	lion_count_path_topk(cx);
	lion_count_path_fill(cx);
	if (!lion_count_path_anti(cx))
		return false;
	cpath = lion_count_path_make(cx);
	lion_count_path_add(cx, cpath);

	/*
	 * The aggregates of sliced columns of a GROUP BY of one column are counted
	 * a group at a time - a copy of the group's rows and the slices counted
	 * against it - or decoded a batch of groups at a time, the slices read
	 * once a batch (DESIGN.md §52, "The decode route"): both are priced, and
	 * add_path() keeps the cheaper.
	 */
	if (lion_count_path_slices_decodable(cx))
	{
		cx->slicedecode = true;
		lion_count_path_fill(cx);
		cpath = lion_count_path_make(cx);
		lion_count_path_add(cx, cpath);
		cx->slicedecode = false;
	}

	/*
	 * Two columns are the nested loop of DESIGN.md §20, which ANDs the sets of
	 * every pair, or the decoded walk of §34, which reads each column's sets
	 * once whatever the number of pairs: both are priced, and add_path()
	 * keeps the cheaper.
	 */
	if (lion_count_path_decodable(cx))
	{
		cx->decode = true;
		lion_count_path_estimate(cx);
		lion_count_path_fill(cx);
		cpath = lion_count_path_make(cx);
		lion_count_path_add(cx, cpath);
	}
	return true;
}

/*
 * Does the query take an aggregate of a column that a whole lion index of the
 * relation slices (DESIGN.md §52), which a build of its own may answer from
 * the slices?  The target list's aggregates and the HAVING's.
 */
static bool
lion_query_has_slice_aggs(const LionCountPathBuild *cx)
{
	List	   *exprs;
	ListCell   *lc;

	if (!lion_enable_count_slices || cx->fj != NULL ||
		cx->anti != NULL || cx->distinctstage || RecoveryInProgress())
		return false;
	/* a partitioned table's with no GROUP BY (§52.13) */
	if (cx->partitioned && cx->parse->groupClause != NIL)
		return false;
	exprs = pull_var_clause((Node *) cx->output_rel->reltarget->exprs,
							PVC_INCLUDE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS |
							PVC_INCLUDE_PLACEHOLDERS);
	if (cx->having != NIL)
		exprs = list_concat(exprs,
							pull_var_clause((Node *) cx->having,
											PVC_INCLUDE_AGGREGATES |
											PVC_RECURSE_WINDOWFUNCS |
											PVC_INCLUDE_PLACEHOLDERS));
	foreach(lc, exprs)
	{
		Node	   *n = (Node *) lfirst(lc);
		AttrNumber	attno;
		AttrNumber	col;
		Oid			argtype;
		Oid			typid;

		if (IsA(n, Aggref) &&
			lion_sagg_classify((Aggref *) n, cx->rti, &attno,
							   &argtype) != LION_SAGG_NONE &&
			lion_count_sliced_index(cx, attno, &col, &typid) != NULL)
			return true;
	}
	return false;
}

/*
 * May a build answer ranges from slices (DESIGN.md §53)?  A table with a lion
 * index that slices a column - the WHERE is not analysed yet, and the build
 * declines itself where no range is one the slices answer - counted on its
 * own: no FK-side or anti join, outside a partial index's scope, not on a hot
 * standby, where nothing is collected (§10).  A partitioned table asks its
 * first live leaf, and every partition its own once they are known
 * (lion_count_path_slice_range_parts()); not inside a partial index's scope
 * at all, which is a partition's at a time.
 */
static bool
lion_query_may_slice_ranges(const LionCountPathBuild *cx)
{
	RelOptInfo *rel = cx->input_rel;

	if (!lion_enable_slice_ranges || cx->fj != NULL ||
		cx->anti != NULL || cx->distinctstage || RecoveryInProgress() ||
		cx->input_rel->baserestrictinfo == NIL ||
		lion_partial_index_of(cx->input_rel) != NULL)
		return false;
	if (cx->partitioned)
	{
		if (lion_partial_active())
			return false;
		rel = lion_count_first_leaf(cx->root, rel, NULL, 0);
		if (rel == NULL)
			return false;
	}
	return lion_rel_has_sliced_index(rel);
}

static void
lion_try_count_path_scoped(LionCountPathBuild *cxp)
{
	LionCountPathBuild cx = *cxp;
	bool		may;
	bool		sliced = false;

	/* a GROUP BY of expressions, or a WHERE over them, counted by cells */
	lion_try_count_cells(cxp);

	/*
	 * The aggregates of sliced columns from their slices (DESIGN.md §52): a
	 * build of their own beside the one that answers them otherwise - from
	 * the entries' keys with no WHERE (§37), or not at all - and add_path()
	 * keeps the cheaper.
	 *
	 * The ranges on sliced columns from their slices (§53): a build of its
	 * own beside each that walks or collects them from their entries and
	 * summaries - with the slices' aggregates and without - which declines
	 * itself where no range is one the slices answer; add_path() keeps the
	 * cheapest.  With pg_lion.prefer_slice_ranges, for the tests, a build that
	 * answers its ranges so leaves the other out.
	 */
	may = lion_query_may_slice_ranges(cxp);
	if (lion_query_has_slice_aggs(cxp))
	{
		LionCountPathBuild sl;
		bool		both = false;

		if (may)
		{
			sl = *cxp;
			sl.slicemode = true;
			sl.slicerange = true;
			both = lion_try_count_path_once(&sl);
			sliced |= both;
		}
		if (!(both && lion_prefer_slice_ranges))
		{
			sl = *cxp;
			sl.slicemode = true;
			(void) lion_try_count_path_once(&sl);
		}
	}
	if (may)
	{
		LionCountPathBuild sr = *cxp;

		sr.slicerange = true;
		sliced |= lion_try_count_path_once(&sr);
	}
	if (sliced && lion_prefer_slice_ranges)
		return;

	if (lion_try_count_path_once(&cx))
		return;

	/*
	 * Over a partitioned table, a restriction that some partition could
	 * neither answer from an index nor leave out by its bounds declined the
	 * attempt; where it could be tested on the rows instead, the partitions
	 * asked for that (DESIGN.md §16, "Residual clauses over a partitioned
	 * table"), and the build is made once more, from the start, with every
	 * restriction they asked for residual.  Once: the second attempt's
	 * partitions have an index for every clause left, or leave it out, as
	 * the first attempt established.
	 */
	if (cx.partitioned && cx.forceresid == NULL && !cx.imply.hard &&
		cx.imply.wantresid != NULL)
	{
		LionCountPathBuild again = *cxp;

		again.forceresid = cx.imply.wantresid;
		(void) lion_try_count_path_once(&again);
	}
}
