/*-------------------------------------------------------------------------
 *
 * lion_ordjoin.c
 *		LionOrderedJoin: a CustomScan for the join rel of
 *
 *			SELECT f.<cols>, d.<cols> FROM fact f JOIN dim d ON f.fk = d.pk
 *			WHERE <fact quals> AND <dim quals>
 *			ORDER BY <columns a btree of the fact provides> LIMIT n
 *
 *		that walks the fact's btree in order, as LionOrdered does, over the
 *		fact's lion set; descends the dimension's unique btree for each
 *		member's key and tests the TID it finds against the dimension's own
 *		lion set BEFORE it touches the dimension's heap; and returns the two
 *		rows together, in the walk's order, until whatever is above it stops
 *		pulling.  DESIGN.md section 47 is the specification.
 *
 *		Each side is a LionOrdered walk, built by lion_ordered.c for the join
 *		and kept as a child of the join's own plan: the fact's is its ordered
 *		walk, with its set, its early stop and its fetch-and-sort switch; the
 *		dimension's a probe of its unique btree (§40.10), handed each key
 *		before its descent (lion_ordered_probe_key()).  EXPLAIN shows the
 *		join alone, its sides' lines in its own.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_customscan.h"
#include "lion_count.h"
#include "lion_compat.h"
#include "lion_fkjoin.h"

bool		lion_enable_ordered_join = true;
static set_join_pathlist_hook_type oj_prev_set_join_pathlist_hook = NULL;

/*
 * custom_private of the plan is POSITIONAL, behind a shape marker:
 *
 *	OJ_PRIV_INTS	IntList (OJ_PRIV_MAGIC, the fact's columns in
 *					custom_scan_tlist, the fk's position among them, 1-based)
 *	OJ_PRIV_INDEX	OidList: the dimension's unique btree, for EXPLAIN
 *
 * custom_plans are the fact's walk and the dimension's probe, in that order;
 * custom_scan_tlist is the fact's target list followed by the dimension's,
 * the rows the join puts together in its scan slot.
 */
#define OJ_PRIV_MAGIC		0x4c4f4a4e	/* "LOJN" */
#define OJ_PRIV_INTS		0
#define OJ_PRIV_INDEX		1
#define OJ_PRIV_NMEMBERS	2

static Plan *oj_plan_path(PlannerInfo *root, RelOptInfo *rel,
						  CustomPath *best_path, List *tlist,
						  List *clauses, List *custom_plans);
static Node *oj_create_state(CustomScan *cscan);
static void oj_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *oj_exec(CustomScanState *node);
static void oj_end(CustomScanState *node);
static void oj_rescan(CustomScanState *node);
static void oj_explain(CustomScanState *node, List *ancestors,
					   ExplainState *es);

const CustomPathMethods lion_ordjoin_path_methods = {
	.CustomName = "LionOrderedJoin",
	.PlanCustomPath = oj_plan_path,
};

static const CustomScanMethods oj_scan_methods = {
	.CustomName = "LionOrderedJoin",
	.CreateCustomScanState = oj_create_state,
};

static const CustomExecMethods oj_exec_methods = {
	.CustomName = "LionOrderedJoin",
	.BeginCustomScan = oj_begin,
	.ExecCustomScan = oj_exec,
	.EndCustomScan = oj_end,
	.ReScanCustomScan = oj_rescan,
	.ExplainCustomScan = oj_explain,
};

/* ---------------------------------------------------------------------
 * Planner (DESIGN.md §47.2)
 * --------------------------------------------------------------------- */

/* The join the node answers: its two sides and its key */
typedef struct OjJoin
{
	RelOptInfo *fact;
	RelOptInfo *dim;
	Var		   *fkvar;			/* the fact's key column */
	Var		   *pkvar;			/* the dimension's */
	IndexOptInfo *pkidx;		/* the unique btree on it */
	Expr	   *keyqual;		/* `pk = NULL`, the descent's qual */
} OjJoin;

/*
 * Does rel's target hold plain columns of its own alone?  A PlaceHolderVar
 * evaluated at it - or at the join, in the join's target - is core's to
 * place; v1 leaves such queries to it.
 */
static bool
oj_target_ok(RelOptInfo *rel, Relids relids)
{
	ListCell   *lc;

	foreach(lc, rel->reltarget->exprs)
	{
		Var		   *var = (Var *) lfirst(lc);

		if (!IsA(var, Var) || var->varlevelsup != 0 ||
			!bms_is_member(var->varno, relids))
			return false;
	}
	return true;
}

/*
 * May rel be a side of the join (DESIGN.md §47.1): one LionOrdered may scan
 * (lion_ordered_rel_ok()), whose restriction clauses the side's own filter
 * can evaluate - none pseudoconstant, which core would gate the side's plan
 * with a Result for, and no subquery, whose plan EXPLAIN could not show under
 * the join - and whose target is its columns.
 */
static bool
oj_side_ok(PlannerInfo *root, RelOptInfo *rel)
{
	ListCell   *lc;

	if (!lion_ordered_rel_ok(root, rel))
		return false;
	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		if (rinfo->pseudoconstant ||
			contain_subplans((Node *) rinfo->clause))
			return false;
	}
	return oj_target_ok(rel, rel->relids);
}

/* A plain column of the current query level, under binary-compatible casts */
static Var *
oj_plain_var(Node *node)
{
	while (node != NULL && IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;
	if (node == NULL || !IsA(node, Var) || ((Var *) node)->varlevelsup != 0 ||
		((Var *) node)->varattno <= 0)
		return NULL;
	return (Var *) node;
}

/*
 * Is the join of outerrel (the fact) and innerrel (the dimension) one the node
 * answers (DESIGN.md §47.1)?  An inner join of the two base relations alone,
 * its one clause `f.fk = d.pk` under an equality of a unique single-column
 * btree on d.pk, which each fact row's key is then looked up in.
 */
static bool
oj_recognize(PlannerInfo *root, RelOptInfo *joinrel, RelOptInfo *outerrel,
			 RelOptInfo *innerrel, JoinType jointype, JoinPathExtraData *extra,
			 OjJoin *j)
{
	Query	   *parse = root->parse;
	RestrictInfo *rinfo;
	OpExpr	   *op;
	Var		   *l;
	Var		   *r;
	Node	   *pkarg;
	Node	   *fkarg;
	Oid			opno;
	int			strategy;
	Oid			lefttype;
	Oid			righttype;

	memset(j, 0, sizeof(OjJoin));

	/* a plain query's inner join, nothing locked or written */
	if (jointype != JOIN_INNER || extra->sjinfo == NULL ||
		extra->sjinfo->jointype != JOIN_INNER)
		return false;
	if (parse->commandType != CMD_SELECT || parse->rowMarks != NIL ||
		root->rowMarks != NIL)
		return false;

	/* two base relations, nothing from outside them */
	if (outerrel->reloptkind != RELOPT_BASEREL ||
		innerrel->reloptkind != RELOPT_BASEREL ||
		!bms_equal(joinrel->relids,
				   bms_union(outerrel->relids, innerrel->relids)) ||
		!bms_is_empty(joinrel->lateral_relids) ||
		!oj_target_ok(joinrel, joinrel->relids))
		return false;

	/* one clause: f.fk = d.pk, between two columns */
	if (list_length(extra->restrictlist) != 1)
		return false;
	rinfo = linitial_node(RestrictInfo, extra->restrictlist);
	if (rinfo->pseudoconstant || !IsA(rinfo->clause, OpExpr))
		return false;
	op = (OpExpr *) rinfo->clause;
	if (list_length(op->args) != 2 || op->opretset || !op_strict(op->opno))
		return false;
	l = oj_plain_var((Node *) linitial(op->args));
	r = oj_plain_var((Node *) lsecond(op->args));
	if (l == NULL || r == NULL)
		return false;
	if (l->varno == (int) innerrel->relid &&
		r->varno == (int) outerrel->relid)
	{
		j->pkvar = l;
		j->fkvar = r;
		pkarg = (Node *) linitial(op->args);
		fkarg = (Node *) lsecond(op->args);
		opno = op->opno;
	}
	else if (l->varno == (int) outerrel->relid &&
			 r->varno == (int) innerrel->relid)
	{
		j->pkvar = r;
		j->fkvar = l;
		pkarg = (Node *) lsecond(op->args);
		fkarg = (Node *) linitial(op->args);
		opno = get_commutator(op->opno);
		if (!OidIsValid(opno))
			return false;
	}
	else
		return false;

	/*
	 * The dimension's key is unique under that equality, by a btree the
	 * descent reads (lion_column_unique_index()), the equality its with the
	 * key column on the left.
	 */
	j->pkidx = lion_column_unique_index(innerrel, j->pkvar->varattno, opno,
										op->inputcollid);
	if (j->pkidx == NULL || !op_in_opfamily(opno, j->pkidx->opfamily[0]))
		return false;
	get_op_opfamily_properties(opno, j->pkidx->opfamily[0], false, &strategy,
							   &lefttype, &righttype);
	if (strategy != BTEqualStrategyNumber ||
		lefttype != j->pkidx->opcintype[0] ||
		!IsBinaryCoercible(exprType(pkarg), lefttype) ||
		!IsBinaryCoercible(exprType(fkarg), righttype))
		return false;

	/* both sides scanned as LionOrdered scans them */
	if (!oj_side_ok(root, outerrel) || !oj_side_ok(root, innerrel))
		return false;

	j->fact = outerrel;
	j->dim = innerrel;
	j->keyqual = make_opclause(opno, BOOLOID, false, (Expr *) copyObject(pkarg),
							   (Expr *) makeNullConst(righttype, -1,
													  InvalidOid),
							   InvalidOid, op->inputcollid);
	set_opfuncid((OpExpr *) j->keyqual);
	return true;
}

/*
 * The price of one LionOrderedJoin (DESIGN.md §47.3): the fact's walk as
 * LionOrdered prices it with its set (lion_ordered_walk_price()); a test of
 * each fact row's key; for each key a descent of the dimension's unique btree
 * and a test of the TID against its set; and for each member found a heap
 * fetch, its recheck and its filter; then the join's own target.  Each
 * non-NULL key is taken to name a dimension row: the join is on a foreign
 * key.  *rows is what it returns: the keys' share of the dimension's rows
 * pass its quals (lion_probe_rel_rows()).  *share is the share of the walk
 * the query's LIMIT is expected to take, *probes the descents.
 */
static void
oj_cost(PlannerInfo *root, RelOptInfo *joinrel, OjJoin *j, LionOrderedSet *ds,
		Cost factstartup, Cost facttotal, double factrows, double *rows,
		double *probes, double *share, Cost *startup_p, Cost *total_p)
{
	RelOptInfo *dim = j->dim;
	IndexOptInfo *pkidx = j->pkidx;
	double		nonnull;
	double		descents;
	double		members;
	double		out;
	double		idxtuples = Max(pkidx->tuples, 1.0);
	double		idxpages = Max((double) pkidx->pages, 1.0);
	double		dimtuples = Max(dim->tuples, 1.0);
	double		spc_random;
	double		spc_seq;
	Cost		descent;
	Cost		startup;
	Cost		run;
	QualCost	resid;

	/* the fact's walk, and a test of each of its rows' keys */
	startup = factstartup;
	run = facttotal - factstartup;
	nonnull = nulltestsel(root, IS_NOT_NULL, (Node *) j->fkvar,
						  j->fact->relid, JOIN_INNER, NULL);
	run += factrows * cpu_operator_cost;
	descents = clamp_row_est(factrows * nonnull);

	/*
	 * Each key's descent of the dimension's btree, priced as
	 * btcostestimate() prices one: its comparisons, the pages of the
	 * descent, an index tuple; and the leaf pages the descents read, as
	 * index_pages_fetched() spreads them over the index.  Then the set's
	 * test of the TID found.
	 */
	descent = ceil(log(idxtuples) / log(2.0)) * cpu_operator_cost +
		(Max(pkidx->tree_height, 0) + 1) * 50.0 * cpu_operator_cost +
		cpu_index_tuple_cost + cpu_operator_cost;
	get_tablespace_page_costs(pkidx->reltablespace, &spc_random, &spc_seq);
	run += descents * descent +
		index_pages_fetched(descents, (BlockNumber) idxpages, idxpages, root) *
		spc_random;
	if (ds->lion != NULL)
		run += descents * cpu_operator_cost;

	/*
	 * The set's build, and the members' heap fetches - the TIDs of the set
	 * among the keys looked up, each read at random - with their recheck and
	 * filter.
	 */
	startup += ds->build;
	members = clamp_row_est(descents * ds->candsel);
	get_tablespace_page_costs(dim->reltablespace, &spc_random, &spc_seq);
	cost_qual_eval(&resid, ds->residual, root);
	startup += resid.startup;
	run += index_pages_fetched(members, dim->pages, idxpages, root) *
		spc_random +
		members * (cpu_tuple_cost + ds->recheck + resid.per_tuple);

	/* the rows it returns, and the join's target */
	out = clamp_row_est(descents * Min(1.0, lion_probe_rel_rows(root, dim) /
									   dimtuples));
	startup += joinrel->reltarget->cost.startup;
	run += out * (cpu_tuple_cost + joinrel->reltarget->cost.per_tuple);

	*rows = out;
	*probes = descents;
	*share = 1.0;
	if (root->limit_tuples > 0)
		*share = Min(1.0, root->limit_tuples / out);
	*startup_p = startup;
	*total_p = startup + run;
}

/*
 * One LionOrderedJoin path (DESIGN.md §47.2): the fact's walk of ord with its
 * set factlion, the dimension's probe with its set dimlion, priced and
 * offered to the join rel at pg_lion.pushdown_margin (§39), as LionOrdered's
 * paths are offered to their relation.
 */
static void
oj_add_path(PlannerInfo *root, RelOptInfo *joinrel, OjJoin *j,
			IndexPath *ord, List *pathkeys, Path *factlion, Path *dimlion)
{
	CustomPath *cp;
	CustomPath *factpath;
	CustomPath *dimpath;
	IndexPath  *pkpath;
	LionOrderedSet ds;
	List	   *iocols;
	double		factrows = lion_probe_rel_rows(root, j->fact);
	double		rows;
	double		probes;
	double		share;
	double		margin = lion_units_margin();
	Cost		factstartup;
	Cost		facttotal;
	Cost		startup;
	Cost		total;

	lion_ordered_walk_price(root, j->fact, ord, factlion, factrows, false,
							&iocols, &factstartup, &facttotal);
	lion_ordered_set_info(root, j->dim, dimlion, true, &ds);
	oj_cost(root, joinrel, j, &ds, factstartup, facttotal, factrows, &rows,
			&probes, &share, &startup, &total);

	factpath = lion_ordered_side_path(j->fact, ord, factlion, iocols, false,
									  share, NULL, factrows, factstartup,
									  facttotal);
	pkpath = create_index_path(root, j->pkidx, NIL, NIL, NIL, NIL,
							   ForwardScanDirection, false, NULL, 1.0, false);
	dimpath = lion_ordered_side_path(j->dim, pkpath, dimlion, NIL, true,
									 probes, j->keyqual, rows, ds.build,
									 ds.build);

	cp = makeNode(CustomPath);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = joinrel;
	cp->path.pathtarget = joinrel->reltarget;
	cp->path.param_info = NULL;
	cp->path.parallel_aware = false;
	cp->path.parallel_safe = false;
	cp->path.parallel_workers = 0;
	cp->path.rows = rows;
	cp->path.startup_cost = startup / margin;
	cp->path.total_cost = total / margin;
	cp->path.pathkeys = pathkeys;
	cp->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cp->custom_paths = list_make2(factpath, dimpath);
	cp->custom_private = list_make2(j->fkvar,
									list_make1_oid(j->pkidx->indexoid));
	cp->methods = &lion_ordjoin_path_methods;

	/* a walk of an index and descents of another, as index scans are */
	lion_path_core_switches(&cp->path, LION_CORE_INDEXSCAN);
	add_path(joinrel, &cp->path);
}

/*
 * set_join_pathlist_hook: LionOrderedJoin's paths for the join of outerrel,
 * the fact, and innerrel, the dimension (DESIGN.md §47.2) - one for each
 * ordered btree of the fact whose order the query wants, with each of the
 * fact's lion accesses and each of the dimension's: every pair is priced and
 * add_path() keeps what it keeps.  A side without a lion access is walked or
 * probed with no set, every row it meets filtered; one side has a set.  The
 * other orientation is the hook's other call.
 */
static void
oj_set_join_pathlist(PlannerInfo *root, RelOptInfo *joinrel,
					 RelOptInfo *outerrel, RelOptInfo *innerrel,
					 JoinType jointype, JoinPathExtraData *extra)
{
	OjJoin		j;
	List	   *ords;
	List	   *factcands;
	List	   *dimcands;
	ListCell   *lc;

	if (oj_prev_set_join_pathlist_hook != NULL)
		oj_prev_set_join_pathlist_hook(root, joinrel, outerrel, innerrel,
									   jointype, extra);

	if (!lion_enable_ordered_join || root->query_pathkeys == NIL)
		return;
	if (!oj_recognize(root, joinrel, outerrel, innerrel, jointype, extra, &j))
		return;

	ords = lion_ordered_orders(j.fact);
	if (ords == NIL)
		return;
	factcands = lion_ordered_accesses(root, j.fact);
	dimcands = lion_ordered_accesses(root, j.dim);
	if (factcands == NIL && dimcands == NIL)
		return;
	if (factcands == NIL)
		factcands = list_make1(NULL);
	if (dimcands == NIL)
		dimcands = list_make1(NULL);

	foreach(lc, ords)
	{
		IndexPath  *ord = (IndexPath *) lfirst(lc);
		List	   *pathkeys = build_join_pathkeys(root, joinrel, JOIN_INNER,
												   ord->path.pathkeys);
		int			ncommon = 0;
		ListCell   *lc2;
		ListCell   *lc3;

		/* the walk's order, as far as the query wants it */
		if (pathkeys == NIL)
			continue;
		(void) pathkeys_count_contained_in(root->query_pathkeys, pathkeys,
										   &ncommon);
		if (ncommon == 0)
			continue;
		foreach(lc2, factcands)
		{
			foreach(lc3, dimcands)
				oj_add_path(root, joinrel, &j, ord, pathkeys,
							(Path *) lfirst(lc2), (Path *) lfirst(lc3));
		}
	}
}

/*
 * PlanCustomPath: the scan slot is the fact's row and then the dimension's,
 * as the sides' plans return them - each its relation's target, in that
 * target's order - and the join's target list is evaluated over it.
 */
static Plan *
oj_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
			 List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	Var		   *fkvar = (Var *) linitial(best_path->custom_private);
	List	   *index = (List *) lsecond(best_path->custom_private);
	List	   *sctlist = NIL;
	AttrNumber	resno = 1;
	int			nfact = 0;
	int			fkresno = 0;
	ListCell   *lc;
	int			side;

	if (list_length(custom_plans) != 2 || clauses != NIL)
		elog(ERROR, "LionOrderedJoin: unexpected plan shape");
	for (side = 0; side < 2; side++)
	{
		Plan	   *child = (Plan *) list_nth(custom_plans, side);

		foreach(lc, child->targetlist)
		{
			TargetEntry *tle = lfirst_node(TargetEntry, lc);
			Var		   *var = (Var *) tle->expr;

			if (side == 0 && IsA(var, Var) && var->varno == fkvar->varno &&
				var->varattno == fkvar->varattno)
				fkresno = resno;
			sctlist = lappend(sctlist,
							  makeTargetEntry((Expr *) copyObject(tle->expr),
											  resno++, NULL, false));
		}
		if (side == 0)
			nfact = list_length(sctlist);
	}
	if (fkresno == 0)
		elog(ERROR, "LionOrderedJoin: the fact's key is not in its target");

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = NIL;
	cscan->scan.scanrelid = 0;
	cscan->flags = best_path->flags;
	cscan->custom_plans = custom_plans;
	cscan->custom_scan_tlist = sctlist;
	cscan->custom_exprs = NIL;
	cscan->custom_private = list_make2(list_make3_int(OJ_PRIV_MAGIC, nfact,
													  fkresno),
									   list_copy(index));
	cscan->methods = &oj_scan_methods;
	return &cscan->scan.plan;
}

/* ---------------------------------------------------------------------
 * Executor (DESIGN.md §47.4)
 * --------------------------------------------------------------------- */

typedef struct OjState
{
	CustomScanState css;
	PlanState  *fact;			/* the fact's walk */
	PlanState  *dim;			/* the dimension's probe */
	int			nfact;			/* the fact's columns in the scan slot */
	int			ndim;			/* ... the dimension's, after them */
	AttrNumber	fkresno;		/* the key among the fact's */
	Oid			pkindex;		/* the dimension's unique btree */
} OjState;

static Node *
oj_create_state(CustomScan *cscan)
{
	OjState    *st = (OjState *) newNode(sizeof(OjState), T_CustomScanState);

	st->css.methods = &oj_exec_methods;
	st->css.slotOps = &TTSOpsVirtual;
	return (Node *) st;
}

static void
oj_begin(CustomScanState *node, EState *estate, int eflags)
{
	OjState    *st = (OjState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	List	   *ints;
	List	   *index;
	int			ncols = list_length(cscan->custom_scan_tlist);

	if (list_length(cscan->custom_private) != OJ_PRIV_NMEMBERS)
		elog(ERROR, "LionOrderedJoin: unrecognized custom_private shape");
	ints = (List *) list_nth(cscan->custom_private, OJ_PRIV_INTS);
	index = (List *) list_nth(cscan->custom_private, OJ_PRIV_INDEX);
	if (ints == NIL || !IsA(ints, IntList) || list_length(ints) != 3 ||
		linitial_int(ints) != OJ_PRIV_MAGIC ||
		index == NIL || !IsA(index, OidList) || list_length(index) != 1)
		elog(ERROR, "LionOrderedJoin: unrecognized custom_private shape");
	st->nfact = lsecond_int(ints);
	st->fkresno = (AttrNumber) lthird_int(ints);
	st->pkindex = linitial_oid(index);
	st->ndim = ncols - st->nfact;
	if (list_length(cscan->custom_plans) != 2 ||
		st->nfact < 1 || st->ndim < 0 ||
		st->fkresno < 1 || st->fkresno > st->nfact ||
		list_length(((Plan *) linitial(cscan->custom_plans))->targetlist) !=
		st->nfact ||
		list_length(((Plan *) lsecond(cscan->custom_plans))->targetlist) !=
		st->ndim)
		elog(ERROR, "LionOrderedJoin: malformed plan");

	/*
	 * The sides, run by the join alone: neither scans backward nor marks.
	 * They are not among custom_ps, which EXPLAIN would show as nodes of
	 * their own; the join shows their lines in its own.
	 */
	eflags &= ~(EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK);
	st->fact = ExecInitNode((Plan *) linitial(cscan->custom_plans), estate,
							eflags);
	st->dim = ExecInitNode((Plan *) lsecond(cscan->custom_plans), estate,
						   eflags);
}

/*
 * The next joined row: the fact's walk's next row whose key is not NULL and
 * names a dimension row that is a member of its set, visible and passes its
 * filter.  The key is unique, so one descent finds that row or none.  The
 * scan slot's values are the two sides' slots', which neither touches until
 * the join asks it for its next row.
 */
static TupleTableSlot *
oj_exec(CustomScanState *node)
{
	OjState    *st = (OjState *) node;
	TupleTableSlot *scanslot = node->ss.ss_ScanTupleSlot;
	ExprContext *econtext = node->ss.ps.ps_ExprContext;

	ResetExprContext(econtext);
	for (;;)
	{
		TupleTableSlot *fslot;
		TupleTableSlot *dslot;
		Datum		key;
		bool		isnull;

		CHECK_FOR_INTERRUPTS();

		ExecClearTuple(scanslot);
		fslot = ExecProcNode(st->fact);
		if (TupIsNull(fslot))
			return NULL;
		key = slot_getattr(fslot, st->fkresno, &isnull);
		if (isnull)
			continue;			/* the clause is strict: no row matches */

		lion_ordered_probe_key(st->dim, key);
		dslot = ExecProcNode(st->dim);
		if (TupIsNull(dslot))
			continue;

		slot_getsomeattrs(fslot, st->nfact);
		memcpy(scanslot->tts_values, fslot->tts_values,
			   sizeof(Datum) * st->nfact);
		memcpy(scanslot->tts_isnull, fslot->tts_isnull,
			   sizeof(bool) * st->nfact);
		if (st->ndim > 0)
		{
			slot_getsomeattrs(dslot, st->ndim);
			memcpy(scanslot->tts_values + st->nfact, dslot->tts_values,
				   sizeof(Datum) * st->ndim);
			memcpy(scanslot->tts_isnull + st->nfact, dslot->tts_isnull,
				   sizeof(bool) * st->ndim);
		}
		ExecStoreVirtualTuple(scanslot);

		if (node->ss.ps.ps_ProjInfo == NULL)
			return scanslot;
		econtext->ecxt_scantuple = scanslot;
		return ExecProject(node->ss.ps.ps_ProjInfo);
	}
}

/*
 * The walk restarts; the dimension is rescanned at its next descent anyway.
 * A changed Param reaches each side, whose rescan rebuilds its set only when
 * its lion quals use the Param (DESIGN.md §30.4).
 */
static void
oj_rescan(CustomScanState *node)
{
	OjState    *st = (OjState *) node;

	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	if (node->ss.ps.chgParam != NULL)
	{
		UpdateChangedParamSet(st->fact, node->ss.ps.chgParam);
		UpdateChangedParamSet(st->dim, node->ss.ps.chgParam);
	}
	if (st->fact->chgParam == NULL)
		ExecReScan(st->fact);
}

static void
oj_end(CustomScanState *node)
{
	OjState    *st = (OjState *) node;

	if (st->fact != NULL)
		ExecEndNode(st->fact);
	if (st->dim != NULL)
		ExecEndNode(st->dim);
	st->fact = NULL;
	st->dim = NULL;
}

/*
 * EXPLAIN (DESIGN.md §47.5): the fact's walk's lines, as LionOrdered prints
 * them; the dimension, by its relation and its unique btree, and its probe's
 * lines behind "Dimension "; and under ANALYZE the counters of each.
 */
static void
oj_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	OjState    *st = (OjState *) node;
	List	   *anc = lcons(node->ss.ps.plan, ancestors);
	Scan	   *dscan = (Scan *) st->dim->plan;
	RangeTblEntry *rte = rt_fetch(dscan->scanrelid, es->rtable);
	char	   *refname = (char *) list_nth(es->rtable_names,
											dscan->scanrelid - 1);
	char	   *relname = get_rel_name(rte->relid);
	char	   *idxname = get_rel_name(st->pkindex);
	StringInfoData buf;

	lion_ordered_explain_side(st->fact, anc, es, NULL, false);

	initStringInfo(&buf);
	appendStringInfoString(&buf, quote_identifier(relname != NULL ?
												  relname : "???"));
	if (refname != NULL && (relname == NULL || strcmp(refname, relname) != 0))
		appendStringInfo(&buf, " %s", quote_identifier(refname));
	appendStringInfo(&buf, " via %s",
					 quote_identifier(idxname != NULL ? idxname : "???"));
	ExplainPropertyText("Dimension", buf.data, es);
	pfree(buf.data);
	lion_ordered_explain_side(st->dim, anc, es, "Dimension ", false);

	if (es->analyze)
	{
		lion_ordered_explain_side(st->fact, anc, es, NULL, true);
		lion_ordered_explain_side(st->dim, anc, es, "Dimension ", true);
	}
}

/* ---------------------------------------------------------------------
 * Initialisation, from _PG_init (lion_am.c)
 * --------------------------------------------------------------------- */

void
lion_ordjoin_init(void)
{
	DefineCustomBoolVariable("pg_lion.enable_ordered_join",
							 "Offer LionOrderedJoin: an ordered walk of a table's btree across a join to a unique key, both sides filtered by lion sets.",
							 NULL,
							 &lion_enable_ordered_join,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	RegisterCustomScanMethods(&oj_scan_methods);

	oj_prev_set_join_pathlist_hook = set_join_pathlist_hook;
	set_join_pathlist_hook = oj_set_join_pathlist;
}
