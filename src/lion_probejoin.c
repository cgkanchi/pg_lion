/*-------------------------------------------------------------------------
 *
 * lion_probejoin.c
 *		LionProbeJoin, the dimension by probe (DESIGN.md §46): a CustomScan
 *		for
 *
 *			SELECT <expressions of predicates on d>, <columns of f>, count(*)
 *			FROM fact f JOIN dim d ON f.fk = d.pk
 *			WHERE <f's clauses> AND <d's clauses lion indexes answer>
 *			  [AND NOT EXISTS (SELECT 1 FROM s WHERE s.a = f.fk ...)]
 *			GROUP BY ...
 *
 *		over a fact table no lion index helps, whose rows core's plan joins
 *		to the dimension by a nested loop into the dimension's primary key:
 *		an Index Scan per fact row that fetches the dimension row from the
 *		heap to test the dimension's filter on it and to compute the group's
 *		expressions from its columns - a random heap page a fact row, read
 *		cold on a dimension the cache does not hold.
 *
 *		The node reads the fact's rows from core's own path of the fact
 *		(serial or partial), looks each row's key up in the primary key's
 *		B-tree, and tests the TID found against lion's sets of the dimension,
 *		built once before the first row: the set of its WHERE, and the sets
 *		that label each row with the outcome of every predicate the GROUP BY's
 *		expressions are made of - the cells of DESIGN.md §10 ("Counts by
 *		cells"), whose analysis is lion_plan_cells.c's.  The heap is read
 *		for a page the visibility map does not call all-visible, as the
 *		nested loop's probe of §40.10 reads it, and for every row found when
 *		the WHERE's set is inexact, to recheck the WHERE on the heap tuple.
 *		A NOT EXISTS over a small table keyed by the join's key is a set
 *		too: its rows' keys looked up in the same B-tree once, every TID
 *		found a member.  Each fact row that finds its dimension row goes up
 *		as the fact's columns and the predicates' truths, LION_TL_CELLPRED
 *		columns of custom_scan_tlist, which setrefs.c matches the GROUP BY's
 *		expressions' predicates to; the Agg above is core's, hashed, and
 *		partial below a Gather when the fact's path is.
 *
 *		The sets, the descent's price and the lion access are lion_ordered.c's
 *		(lion_probe_sets_*(), lion_ordered_*()); the shape of the join is
 *		lion_fkjoin.c's (lion_fkjoin_recognize_probe()).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/relscan.h"
#include "access/tableam.h"
#if PG_VERSION_NUM >= 200000
#include "access/tableam_indexscan.h"
#endif
#include "access/visibilitymap.h"
#include "optimizer/restrictinfo.h"
#include "storage/bufmgr.h"
#include "storage/predicate.h"
#include "utils/snapmgr.h"

#include "lion_compat.h"
#include "lion_customscan.h"

/* GUC pg_lion.enable_probe_join (lion_am.c) */
bool		lion_enable_probe_join = true;

/* ---------------------------------------------------------------------
 * The plan
 *
 * custom_private, by position:
 *
 *	PJ_PRIV_INTS	ints: PJ_INT_*
 *	PJ_PRIV_OIDS	oids: PJ_OID_*
 *	PJ_PRIV_TREES	the sets' lion trees, as lo_lion_tree() encodes each: the
 *					WHERE's first when there is one, then the labels'
 *	PJ_PRIV_LEAVES	every tree's leaves' indexes, in order
 *	PJ_PRIV_DIMS	a dimension each: [kind, radix, nullout, nlabel, then a
 *					label's tree and value for each] - a scalar label's value
 *					the outcome of its rows, a multi-key one's its leaf's
 *					bit, -1 its NULL entry's
 *	PJ_PRIV_PREDS	a predicate each: [dim, kind, bit, ntruth, truths ...]
 *	PJ_PRIV_RECHECK	the WHERE's qual over the dimension's heap tuple (its
 *					Vars INDEX_VAR), for a set built inexact; or NIL
 *
 * custom_exprs is every tree's leaves' index quals, in order, each with a
 * CaseTestExpr where the key is (pj_keyless()): setrefs.c fixes their Params
 * and functions, and finds no Var in them to match to the scan tuple;
 * lo_fix_qual() puts the index column in the key's place at run time.
 *
 * custom_scan_tlist: the fact's columns, as its child returns them; the
 * predicates (LION_TL_CELLPRED); and, resjunk and never filled, what EXPLAIN
 * prints - the probe's clause, the WHERE's qual and the anti join's clause -
 * there so that setrefs.c moves their Vars with the range table.
 * --------------------------------------------------------------------- */

#define PJ_PRIV_MAGIC		0x4c504a4e	/* "LPJN" */

#define PJ_PRIV_INTS		0
#define PJ_PRIV_OIDS		1
#define PJ_PRIV_TREES		2
#define PJ_PRIV_LEAVES		3
#define PJ_PRIV_DIMS		4
#define PJ_PRIV_PREDS		5
#define PJ_PRIV_RECHECK		6
#define PJ_PRIV_NMEMBERS	7

#define PJ_INT_MAGIC		0
#define PJ_INT_NFACT		1	/* the fact's columns */
#define PJ_INT_FKCOL		2	/* ... the key's among them, from 1 */
#define PJ_INT_WHERE		3	/* the WHERE's tree, or -1 */
#define PJ_INT_NWHERELEAF	4	/* ... and its leaves, the first ones */
#define PJ_INT_NDIM			5
#define PJ_INT_NPRED		6
#define PJ_INT_ANTICOL		7	/* the anti child's key column, or 0 */
#define PJ_INT_PROBETL		8	/* custom_scan_tlist: the probe's clause */
#define PJ_INT_WHERETL		9	/* ... the WHERE's qual, or 0 */
#define PJ_INT_ANTITL		10	/* ... the anti join's clause, or 0 */
#define PJ_INT_N			11

#define PJ_OID_PK			0	/* the B-tree probed */
#define PJ_OID_PROBEOP		1	/* pk = fk, of its family */
#define PJ_OID_COLL			2	/* ... and its collation */
#define PJ_OID_ANTIOP		3	/* pk = the anti join's key, or InvalidOid */
#define PJ_OID_N			4

/*
 * The planner's bundle in the CustomPath's custom_private: the plan's
 * custom_private, the scan tuple's expressions, [the first resjunk one, the
 * dimension's range table index], and the quals of custom_exprs.
 */
#define PJ_PATH_PRIV		0
#define PJ_PATH_TLIST		1
#define PJ_PATH_INTS		2
#define PJ_PATH_QUALS		3

/*
 * The most a scan tuple may hold: the fact's columns, the predicates and
 * three for EXPLAIN.
 */
#define PJ_MAX_COLUMNS		(MaxTupleAttributeNumber - 3)

/* A label of a dimension: its set (a tree's) and what membership says */
typedef struct PjLabel
{
	int			tree;
	int			value;
} PjLabel;

typedef struct PjDim
{
	int			kind;			/* LION_CELLDIM_* */
	int			nlabel;
	PjLabel    *labels;
} PjDim;

typedef struct LionProbeJoinState
{
	CustomScanState css;

	/* the plan */
	int			nfact;
	AttrNumber	fkcol;
	int			wheretree;		/* -1: no WHERE */
	int			ntree;
	int			ndim;
	PjDim	   *dims;
	LionCellsDimPriv *dimpriv;	/* kind, radix and nullout of each */
	int			npred;
	LionCellsPredPriv *preds;
	AttrNumber	antikeycol;		/* 0: no anti join */
	int			antiset;		/* the anti join's set: an extra one */
	Oid			pkoid;
	Oid			probeop;
	Oid			collation;
	Oid			antiop;

	/* the run */
	PlanState  *factps;
	PlanState  *antips;
	Relation	pkidx;
	IndexScanDesc scan;			/* the B-tree's, for every lookup */
	ScanKeyData probekey;
	ScanKeyData antikey;
	TupleTableSlot *tabslot;	/* a dimension row read for its visibility */
	Buffer		vmbuffer;
	LionProbeSets *sets;
	bool		built;			/* the sets are, for this scan's Params */
	bool		whereexact;
	Bitmapset  *lionparams;		/* the Params the sets are built with */
	ExprState  *recheck;		/* the WHERE over the heap tuple, or NULL */
	int		   *outs;			/* [ndim] scratch: a row's outcomes */

	/* EXPLAIN ANALYZE */
	bool		ran;
	uint64		probes;
	uint64		nullkeys;
	uint64		wherefail;
	uint64		antifail;
	uint64		recheckfail;
	uint64		invisible;
	uint64		fetched;
	uint64		vmskips;
	uint64		antirows;
	uint64		antitids;
} LionProbeJoinState;

static Plan *pj_plan_path(PlannerInfo *root, RelOptInfo *rel,
						  CustomPath *best_path, List *tlist, List *clauses,
						  List *custom_plans);
static Node *pj_create_state(CustomScan *cscan);
static void pj_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *pj_exec(CustomScanState *node);
static void pj_end(CustomScanState *node);
static void pj_rescan(CustomScanState *node);
static void pj_explain(CustomScanState *node, List *ancestors,
					   ExplainState *es);

const CustomPathMethods lion_probe_join_path_methods = {
	.CustomName = "LionProbeJoin",
	.PlanCustomPath = pj_plan_path,
	.ReparameterizeCustomPathByChild = NULL,
};

static const CustomScanMethods pj_scan_methods = {
	.CustomName = "LionProbeJoin",
	.CreateCustomScanState = pj_create_state,
};

static const CustomExecMethods pj_exec_methods = {
	.CustomName = "LionProbeJoin",
	.BeginCustomScan = pj_begin,
	.ExecCustomScan = pj_exec,
	.EndCustomScan = pj_end,
	.ReScanCustomScan = pj_rescan,
	.ExplainCustomScan = pj_explain,
};

/*
 * Make the name resolvable when a plan is read back - by a parallel worker
 * above all.  From _PG_init.
 */
void
lion_probe_join_register(void)
{
	if (GetCustomScanMethods("LionProbeJoin", true) == NULL)
		RegisterCustomScanMethods(&pj_scan_methods);
}

/* ---------------------------------------------------------------------
 * Planning
 * --------------------------------------------------------------------- */

/* What the planner has decided of one shape, for its paths and its plan */
typedef struct PjPlan
{
	LionProbeShape *shape;
	RelOptInfo *input_rel;
	RelOptInfo *output_rel;
	PathTarget *nodetarget;
	Path	   *where;			/* the WHERE's lion access, or NULL */
	List	   *wherequal;		/* ... and its original qual */
	LionCellsPlan *cells;		/* the GROUP BY's predicates, or NULL */
	List	   *labels;			/* Path: each label's lion access */
	List	   *labeldims;		/* ... its dimension (int) */
	List	   *labelvalues;	/* ... and its value (int) */
	Cost		labelcost;		/* their builds */
	double		labelbytes;		/* ... and their sets' bytes */
	double		lionpages;		/* lion index pages the builds read */
} PjPlan;

/*
 * qual with a CaseTestExpr in place of its key - the left operand of an
 * OpExpr or a ScalarArrayOpExpr, a NullTest's argument - which lo_fix_qual()
 * replaces with the index column (PJ_PRIV, custom_exprs).
 */
static Expr *
pj_keyless(Expr *qual)
{
	Expr	   *q = (Expr *) copyObject(qual);
	Node	   *key;
	CaseTestExpr *ph = makeNode(CaseTestExpr);

	if (IsA(q, OpExpr) && list_length(((OpExpr *) q)->args) == 2)
		key = (Node *) linitial(((OpExpr *) q)->args);
	else if (IsA(q, ScalarArrayOpExpr))
		key = (Node *) linitial(((ScalarArrayOpExpr *) q)->args);
	else if (IsA(q, NullTest))
		key = (Node *) ((NullTest *) q)->arg;
	else
	{
		elog(ERROR, "LionProbeJoin: unsupported index qual type %d",
			 (int) nodeTag(q));
		return NULL;			/* keep compiler quiet */
	}
	ph->typeId = exprType(key);
	ph->typeMod = exprTypmod(key);
	ph->collation = exprCollation(key);
	if (IsA(q, OpExpr))
		linitial(((OpExpr *) q)->args) = ph;
	else if (IsA(q, ScalarArrayOpExpr))
		linitial(((ScalarArrayOpExpr *) q)->args) = ph;
	else
		((NullTest *) q)->arg = (Expr *) ph;
	return q;
}

/* The dimension's Vars as INDEX_VAR Vars of the scan tuple a recheck sees */
static Node *
pj_index_vars_mutator(Node *node, void *context)
{
	Index		relid = *(Index *) context;

	if (node == NULL)
		return NULL;
	if (IsA(node, Var) && ((Var *) node)->varno == (int) relid &&
		((Var *) node)->varlevelsup == 0)
	{
		Var		   *v = (Var *) copyObject(node);

		v->varno = INDEX_VAR;
		v->varnullingrels = NULL;
		return (Node *) v;
	}
	return expression_tree_mutator(node, pj_index_vars_mutator, context);
}

/* An index qual of one lion index column, as a lion leaf of a set */
static IndexPath *
pj_label_path(PlannerInfo *root, IndexOptInfo *idx, AttrNumber col,
			  Expr *clause)
{
	IndexClause *ic = makeNode(IndexClause);

	ic->rinfo = make_simple_restrictinfo(root, clause);
	ic->indexquals = list_make1(ic->rinfo);
	ic->lossy = false;
	ic->indexcol = col - 1;
	ic->indexcols = NIL;
	return create_index_path(root, idx, list_make1(ic), NIL, NIL, NIL,
							 ForwardScanDirection, false, NULL, 1.0, false);
}

/* `var IS NULL` */
static Expr *
pj_null_test(Var *var)
{
	NullTest   *nt = makeNode(NullTest);

	nt->arg = (Expr *) copyObject(var);
	nt->nulltesttype = IS_NULL;
	nt->argisrow = false;
	nt->location = -1;
	return (Expr *) nt;
}

/*
 * `var = v` of the one value, or `var = ANY (array)` of several, by the
 * dimension's equality - an OR of equalities when their type has no array
 * type to make the list of, or they are not all of one type.
 */
static Path *
pj_values_path(PlannerInfo *root, RelOptInfo *rel, LionCellsDimPlan *dp,
			   List *vals)
{
	const LionCellsDimPriv *pv = &dp->priv;
	Const	   *first = (Const *) linitial(vals);
	Oid			arraytype = get_array_type(first->consttype);
	bool		onetype = true;
	ListCell   *lc;

	foreach(lc, vals)
	{
		Const	   *c = (Const *) lfirst(lc);

		if (c->consttype != first->consttype || c->constisnull)
			onetype = false;
	}
	if (list_length(vals) == 1 || !OidIsValid(arraytype) || !onetype)
	{
		List	   *paths = NIL;

		foreach(lc, vals)
			paths = lappend(paths,
							pj_label_path(root, dp->idx, dp->col,
										  make_opclause(pv->eqop, BOOLOID,
														false,
														(Expr *) copyObject(dp->var),
														(Expr *) copyObject(lfirst(lc)),
														InvalidOid,
														pv->collation)));
		if (list_length(paths) == 1)
			return (Path *) linitial(paths);
		return (Path *) create_bitmap_or_path(root, rel, paths);
	}
	else
	{
		int			n = list_length(vals);
		Datum	   *elems = (Datum *) palloc(sizeof(Datum) * n);
		int16		typlen;
		bool		typbyval;
		char		typalign;
		ScalarArrayOpExpr *saop = makeNode(ScalarArrayOpExpr);
		ArrayType  *arr;
		int			i = 0;

		foreach(lc, vals)
			elems[i++] = ((Const *) lfirst(lc))->constvalue;
		get_typlenbyvalalign(first->consttype, &typlen, &typbyval, &typalign);
		arr = construct_array(elems, n, first->consttype, typlen, typbyval,
							  typalign);
		saop->opno = pv->eqop;
		saop->opfuncid = InvalidOid;
		saop->useOr = true;
		saop->inputcollid = pv->collation;
		saop->args = list_make2(copyObject(dp->var),
								makeConst(arraytype, -1, first->constcollid,
										  -1, PointerGetDatum(arr), false,
										  false));
		saop->location = -1;
		return (Path *) pj_label_path(root, dp->idx, dp->col, (Expr *) saop);
	}
}

/* The lion index pages a set's build reads: each leaf's share of its index */
static double
pj_lion_pages(Path *lion)
{
	double		pages = 0.0;
	ListCell   *lc;

	if (lion == NULL)
		return 0.0;
	if (IsA(lion, IndexPath))
	{
		IndexPath  *ip = (IndexPath *) lion;

		return Max(ceil((double) ip->indexinfo->pages *
						Min(ip->indexselectivity, 1.0)), 1.0);
	}
	foreach(lc, IsA(lion, BitmapAndPath) ?
			((BitmapAndPath *) lion)->bitmapquals :
			castNode(BitmapOrPath, lion)->bitmapquals)
		pages += pj_lion_pages((Path *) lfirst(lc));
	return pages;
}

/*
 * Each dimension's labels (DESIGN.md §46, "The labels"): a scalar column's
 * set of each outcome but 0 - its values' equality, `= ANY` of several, ORed
 * with its NULL entry where NULL's outcome is it - and a multi-key column's
 * set of each leaf and of its NULL entry; each priced as a set is
 * (lion_ordered_set_price()).  False when an index cannot be read for them.
 */
static bool
pj_labels(PlannerInfo *root, RelOptInfo *dim, PjPlan *pp)
{
	LionCellsPlan *cp = pp->cells;
	int			d;

	for (d = 0; d < cp->ndim; d++)
	{
		LionCellsDimPlan *dp = &cp->dims[d];
		LionCellsDimPriv *pv = &dp->priv;
		int			o;

		if (dp->idx == NULL || dp->idx->hypothetical ||
			(dp->idx->indpred != NIL && !dp->idx->predOK) ||
			dp->col < 1 || dp->col > dp->idx->ncolumns)
			return false;

		if (pv->kind == LION_CELLDIM_SCALAR)
		{
			for (o = 1; o < pv->radix; o++)
			{
				List	   *vals = NIL;
				List	   *paths = NIL;
				ListCell   *lc;
				int			i = 0;

				foreach(lc, pv->vals)
				{
					if (pv->valout[i++] == o)
						vals = lappend(vals, lfirst(lc));
				}
				if (vals != NIL)
					paths = lappend(paths, pj_values_path(root, dim, dp, vals));
				if (pv->nullout == o)
					paths = lappend(paths,
									pj_label_path(root, dp->idx, dp->col,
												  pj_null_test(dp->var)));
				if (paths == NIL)
					continue;
				pp->labels = lappend(pp->labels,
									 list_length(paths) == 1 ?
									 linitial(paths) :
									 create_bitmap_or_path(root, dim, paths));
				pp->labeldims = lappend_int(pp->labeldims, d);
				pp->labelvalues = lappend_int(pp->labelvalues, o);
			}
		}
		else
		{
			int			b;

			pp->labels = lappend(pp->labels,
								 pj_label_path(root, dp->idx, dp->col,
											   pj_null_test(dp->var)));
			pp->labeldims = lappend_int(pp->labeldims, d);
			pp->labelvalues = lappend_int(pp->labelvalues, -1);
			for (b = 0; b < pv->nbit; b++)
			{
				if (dp->bitclause[b] == NULL || !IsA(dp->bitclause[b], OpExpr))
					return false;
				pp->labels = lappend(pp->labels,
									 pj_label_path(root, dp->idx, dp->col,
												   (Expr *) copyObject(dp->bitclause[b])));
				pp->labeldims = lappend_int(pp->labeldims, d);
				pp->labelvalues = lappend_int(pp->labelvalues, b);
			}
		}
	}

	/* each label's build, as lion prices a set's */
	{
		ListCell   *lc;

		foreach(lc, pp->labels)
		{
			double		members;
			double		bytes;

			pp->labelcost += lion_ordered_set_price(root, dim,
													(Path *) lfirst(lc),
													&members, &bytes);
			pp->labelbytes += bytes;
			pp->lionpages += pj_lion_pages((Path *) lfirst(lc));
		}
	}
	return true;
}

/*
 * The node's target (DESIGN.md §46, "The target"): the grouping input target
 * of the join, each expression of it of the fact's columns alone, or of the
 * dimension's columns inside predicates lion's cells decide - nothing of
 * the dimension anywhere else, and nothing of any other table.  The
 * dimension's expressions into *dimexprs.
 */
static bool
pj_target_ok(PathTarget *target, int factrelid, int dimrelid, List **dimexprs)
{
	ListCell   *lc;

	*dimexprs = NIL;
	foreach(lc, target->exprs)
	{
		Node	   *e = (Node *) lfirst(lc);
		List	   *vars;
		ListCell   *lv;
		bool		hasdim = false;
		bool		hasfact = false;

		if (contain_volatile_functions(e) || contain_subplans(e) ||
			expression_returns_set(e))
			return false;
		vars = pull_var_clause(e, PVC_INCLUDE_PLACEHOLDERS |
							   PVC_INCLUDE_AGGREGATES |
							   PVC_INCLUDE_WINDOWFUNCS);
		foreach(lv, vars)
		{
			Var		   *v = (Var *) lfirst(lv);

			if (!IsA(v, Var) || v->varlevelsup != 0)
				return false;
			if (v->varno == dimrelid)
				hasdim = true;
			else if (v->varno == factrelid)
				hasfact = true;
			else
				return false;
		}
		if (hasdim && hasfact)
			return false;
		if (hasdim)
			*dimexprs = lappend(*dimexprs, e);
	}
	return true;
}

/* The position of var among exprs, from 1, or 0 */
static AttrNumber
pj_position(List *exprs, Var *var)
{
	ListCell   *lc;
	AttrNumber	i = 0;

	foreach(lc, exprs)
	{
		Var		   *v = (Var *) lfirst(lc);

		i++;
		if (IsA(v, Var) && v->varno == var->varno &&
			v->varattno == var->varattno && v->varlevelsup == 0)
			return i;
	}
	return 0;
}

/* Are all of exprs plain Vars of relid? */
static bool
pj_all_vars_of(List *exprs, int relid)
{
	ListCell   *lc;

	foreach(lc, exprs)
	{
		Var		   *v = (Var *) lfirst(lc);

		if (!IsA(v, Var) || v->varno != relid || v->varlevelsup != 0)
			return false;
	}
	return true;
}

/* An int List of the label's [tree, value] pairs of dimension d, appended */
static List *
pj_dim_private(PjPlan *pp, int d, int firsttree)
{
	const LionCellsDimPriv *pv = &pp->cells->dims[d].priv;
	List	   *l = list_make3_int(pv->kind, pv->radix, pv->nullout);
	List	   *pairs = NIL;
	ListCell   *lc1;
	ListCell   *lc2;
	int			n = 0;
	int			t = firsttree;

	forboth(lc1, pp->labeldims, lc2, pp->labelvalues)
	{
		if (lfirst_int(lc1) == d)
		{
			pairs = lappend_int(pairs, t);
			pairs = lappend_int(pairs, lfirst_int(lc2));
			n++;
		}
		t++;
	}
	l = lappend_int(l, n);
	return list_concat(l, pairs);
}

/* An int List of predicate p: [dim, kind, bit, ntruth, truths ...] */
static List *
pj_pred_private(PjPlan *pp, int p)
{
	const LionCellsPredPriv *pr = &pp->cells->preds[p];
	const LionCellsDimPriv *pv = &pp->cells->dims[pr->dim].priv;
	List	   *l = list_make3_int(pr->dim, pr->kind, pr->bit);
	int			ntruth = 0;
	int			i;

	if (pr->kind == LION_CELLPRED_TABLE)
		ntruth = pv->radix;
	else if (pr->kind == LION_CELLPRED_NULLS)
		ntruth = 2;
	l = lappend_int(l, ntruth);
	for (i = 0; i < ntruth; i++)
		l = lappend_int(l, (int) pr->truth[i]);
	return l;
}

/*
 * The node's path over `child` - the fact's cheapest path, or its partial
 * one - priced (DESIGN.md §46, "Cost"): the child's and the anti join's
 * child's costs, core's own; and in the competitor's units (§39) the sets'
 * builds, the anti join's lookups, a descent of the B-tree for each fact
 * row (§40.10's scan of the probe), each member's tests and truths, and the
 * rows it emits.
 */
static CustomPath *
pj_make_path(PlannerInfo *root, PjPlan *pp, Path *child, Path *antichild,
			 List *bundle,
			 const LionUnits *units, double setbuild, double antibuild,
			 double perprobe, double permember, double memberfrac,
			 double rows, bool partial)
{
	LionProbeShape *ps = pp->shape;
	CustomPath *cpath = makeNode(CustomPath);
	double		loops = clamp_row_est(child->rows);
	Cost		once;
	Cost		run;

	once = lion_units_price(units, setbuild + antibuild);
	run = lion_units_price(units,
						   loops * perprobe +
						   loops * memberfrac * permember +
						   rows * (cpu_tuple_cost +
								   pp->nodetarget->cost.per_tuple));

	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = pp->input_rel;
	cpath->path.pathtarget = pp->nodetarget;
	cpath->path.param_info = NULL;
	cpath->path.parallel_aware = false;
	cpath->path.parallel_safe = child->parallel_safe &&
		pp->input_rel->consider_parallel && ps->dimrel->consider_parallel &&
		(antichild == NULL || antichild->parallel_safe);
	cpath->path.parallel_workers = partial ? child->parallel_workers : 0;
	cpath->path.pathkeys = NIL;
	cpath->path.rows = rows;

	/* the sets and the anti join's are built before the first fact row */
	cpath->path.startup_cost = child->startup_cost + once +
		pp->nodetarget->cost.startup;
	cpath->path.total_cost = child->total_cost + once + run +
		pp->nodetarget->cost.startup;
	if (antichild != NULL)
	{
		cpath->path.startup_cost += antichild->total_cost;
		cpath->path.total_cost += antichild->total_cost;
	}
#if PG_VERSION_NUM >= 180000
	cpath->path.disabled_nodes = child->disabled_nodes +
		(antichild != NULL ? antichild->disabled_nodes : 0);
#endif
	cpath->flags = 0;
	cpath->custom_paths = (antichild != NULL) ?
		list_make2(child, antichild) : list_make1(child);
#if PG_VERSION_NUM >= 170000
	cpath->custom_restrictinfo = NIL;
#endif
	cpath->custom_private = bundle;
	cpath->methods = &lion_probe_join_path_methods;

	/* the dimension is answered from its indexes and the visibility map */
	lion_path_core_switches(&cpath->path, LION_CORE_INDEXONLY);
	return cpath;
}

/*
 * The Agg core would put over the join, put over the node: hashed where the
 * GROUP BY can be hashed, sorted otherwise, plain without one; and its
 * partial form over the partial node, below a Gather, where the aggregates
 * can be split (DESIGN.md §46, "Above the node").
 */
static void
pj_add_aggs(PlannerInfo *root, PjPlan *pp, GroupPathExtraData *extra,
			CustomPath *serial, CustomPath *partial)
{
	Query	   *parse = root->parse;
	RelOptInfo *output_rel = pp->output_rel;
	List	   *having = (List *) extra->havingQual;
	List	   *groupexprs = NIL;
	AggClauseCosts agg_costs;
	double		numgroups = 1.0;
	bool		hashed = false;
	bool		grouped = (root->processed_groupClause != NIL);

	if (grouped)
	{
		groupexprs = get_sortgrouplist_exprs(root->processed_groupClause,
											 root->processed_tlist);
		numgroups = estimate_num_groups(root, groupexprs, serial->path.rows,
										NULL, NULL);
		hashed = (extra->flags & GROUPING_CAN_USE_HASH) != 0 &&
			root->numOrderedAggs == 0 &&
			grouping_is_hashable(root->processed_groupClause);
	}

	MemSet(&agg_costs, 0, sizeof(agg_costs));
	get_agg_clause_costs(root, AGGSPLIT_SIMPLE, &agg_costs);
	if (hashed)
		add_path(output_rel, (Path *)
				 create_agg_path(root, output_rel, &serial->path,
								 output_rel->reltarget, AGG_HASHED,
								 AGGSPLIT_SIMPLE, root->processed_groupClause,
								 having, &agg_costs, numgroups));
	else if (grouped)
	{
		Path	   *sorted;

		if (!grouping_is_sortable(root->processed_groupClause))
			return;
		sorted = (Path *) create_sort_path(root, output_rel, &serial->path,
										   root->group_pathkeys, -1.0);
		add_path(output_rel, (Path *)
				 create_agg_path(root, output_rel, sorted,
								 output_rel->reltarget, AGG_SORTED,
								 AGGSPLIT_SIMPLE, root->processed_groupClause,
								 having, &agg_costs, numgroups));
	}
	else
		add_path(output_rel, (Path *)
				 create_agg_path(root, output_rel, &serial->path,
								 output_rel->reltarget,
								 parse->groupClause != NIL ?
								 AGG_SORTED : AGG_PLAIN,
								 AGGSPLIT_SIMPLE, NIL, having, &agg_costs,
								 1.0));

	/*
	 * ---- the partial form: Finalize Agg, Gather, Partial Agg ----
	 *
	 * Each participant reads its share of the fact's rows and builds its own
	 * sets; a GROUP BY of constants alone (sorted with no key) is left
	 * serial, as are aggregates that cannot be split.
	 */
	if (partial != NULL && (hashed || parse->groupClause == NIL) &&
		(extra->flags & GROUPING_CAN_PARTIAL_AGG) != 0 &&
		output_rel->consider_parallel)
	{
		PathTarget *ptarget = lion_make_partial_target(root,
													   output_rel->reltarget,
													   having);
		AggClauseCosts pcosts;
		AggClauseCosts fcosts;
		Path	   *pagg;
		Path	   *gather;
		double		pgroups;
		double		grows;

		MemSet(&pcosts, 0, sizeof(pcosts));
		MemSet(&fcosts, 0, sizeof(fcosts));
		get_agg_clause_costs(root, AGGSPLIT_INITIAL_SERIAL, &pcosts);
		get_agg_clause_costs(root, AGGSPLIT_FINAL_DESERIAL, &fcosts);
		pgroups = grouped ?
			estimate_num_groups(root, groupexprs, partial->path.rows,
								NULL, NULL) : 1.0;
		pagg = (Path *) create_agg_path(root, output_rel, &partial->path,
										ptarget,
										hashed ? AGG_HASHED : AGG_PLAIN,
										AGGSPLIT_INITIAL_SERIAL,
										root->processed_groupClause, NIL,
										&pcosts, pgroups);
		grows = pagg->rows * Max(pagg->parallel_workers, 1);
		gather = (Path *) create_gather_path(root, output_rel, pagg, ptarget,
											 NULL, &grows);
		add_path(output_rel, (Path *)
				 create_agg_path(root, output_rel, gather,
								 output_rel->reltarget,
								 hashed ? AGG_HASHED : AGG_PLAIN,
								 AGGSPLIT_FINAL_DESERIAL,
								 root->processed_groupClause, having,
								 &fcosts, numgroups));
	}
}

/*
 * One shape: what qualifies (DESIGN.md §46), the plan's pieces, the price,
 * and the paths.
 */
static void
pj_try_shape(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel,
			 GroupPathExtraData *extra, LionProbeShape *ps)
{
	RelOptInfo *fact = ps->factrel;
	RelOptInfo *dim = ps->dimrel;
	RangeTblEntry *dimrte = root->simple_rte_array[dim->relid];
	PjPlan		pp;
	List	   *dimexprs;
	Path	   *child = fact->cheapest_total_path;
	Path	   *pchild = NULL;
	Path	   *pantichild = NULL;
	List	   *factexprs;
	AttrNumber	fkcol;
	AttrNumber	anticol = 0;
	List	   *trees = NIL;
	List	   *leaves = NIL;
	List	   *quals = NIL;
	List	   *keyless = NIL;
	List	   *tlexprs = NIL;
	List	   *dims = NIL;
	List	   *preds = NIL;
	List	   *recheck = NIL;
	List	   *priv;
	List	   *bundle;
	int			wheretree = -1;
	int			nwhereleaf = 0;
	int			probetl;
	int			wheretl = 0;
	int			antitl = 0;
	int			firstjunk;
	int			npred;
	int			d;
	int			p;
	Cost		perprobe;
	Cost		setbuild = 0;
	Cost		antibuild = 0;
	double		setbytes = 0;
	double		permember;
	double		memberfrac = 1.0;
	bool		whereexact = true;
	double		loops;
	double		rows;
	double		reads;
	double		idxpages;
	IndexPath  *probepath;
	LionUnits	units;
	CustomPath *serial;
	CustomPath *partial = NULL;
	ListCell   *lc;

	memset(&pp, 0, sizeof(pp));
	pp.shape = ps;
	pp.input_rel = input_rel;
	pp.output_rel = output_rel;
	pp.nodetarget = input_rel->reltarget;

	/* ---- the dimension: a heap table lion may read, no security quals ---- */
	if (dimrte->securityQuals != NIL || dimrte->tablesample != NULL)
		return;
	if (root->parse->resultRelation == (int) dim->relid ||
		bms_is_member((int) dim->relid, root->all_result_relids))
		return;
	foreach(lc, dim->baserestrictinfo)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		if (rinfo->security_level > 0 || rinfo->pseudoconstant)
			return;
	}
	{
		Relation	relation = table_open(dimrte->relid, NoLock);
		bool		supported = lion_table_am_supported(relation);

		table_close(relation, NoLock);
		if (!supported)
			return;
	}

	/* ---- the target: the fact's columns, the dimension's predicates ---- */
	if (!pj_target_ok(pp.nodetarget, (int) fact->relid, (int) dim->relid,
					  &dimexprs))
		return;
	if (dimexprs != NIL)
	{
		pp.cells = lion_cells_analyze(root, dim, dim->relid, dimexprs);
		if (pp.cells == NULL)
			return;
	}

	/* ---- the WHERE: every clause of the dimension, lion's ---- */
	if (!lion_ordered_where_access(root, dim, &pp.where, &pp.wherequal))
		return;
	if (pp.where == NULL && pp.cells == NULL)
		return;					/* no set: core's nested loop already is this */
	if (pp.wherequal != NIL &&
		(contain_subplans((Node *) pp.wherequal) ||
		 !bms_is_subset(pull_varnos(root, (Node *) pp.wherequal),
						dim->relids)))
		return;
	if (pp.cells != NULL && !pj_labels(root, dim, &pp))
		return;
	npred = (pp.cells != NULL) ? pp.cells->npred : 0;

	/* ---- the fact: its rows, as its path returns them ---- */
	if (child == NULL || child->param_info != NULL)
		return;
	factexprs = child->pathtarget->exprs;
	if (!pj_all_vars_of(factexprs, (int) fact->relid))
		return;
	fkcol = pj_position(factexprs, ps->fkvar);
	if (fkcol == 0)
		return;
	if (list_length(factexprs) + npred > PJ_MAX_COLUMNS)
		return;
	if (ps->antirel != NULL)
	{
		anticol = pj_position(ps->antipath->pathtarget->exprs, ps->antivar);
		if (anticol == 0)
			return;
	}
	if (fact->partial_pathlist != NIL &&
		fact->consider_parallel && dim->consider_parallel &&
		input_rel->consider_parallel)
	{
		/*
		 * Each participant reads the anti join's inner side whole, for its
		 * own set: its cheapest path that may run in a worker - never the
		 * Gather the serial node may read.
		 */
		pchild = (Path *) linitial(fact->partial_pathlist);
		if (ps->antirel != NULL)
		{
			pantichild = get_cheapest_parallel_safe_total_inner(ps->antirel->pathlist);
			if (pantichild == NULL ||
				pj_position(pantichild->pathtarget->exprs,
							ps->antivar) != anticol)
				pchild = NULL;
		}
		if (pchild != NULL &&
			(pchild->param_info != NULL || pchild->parallel_workers <= 0 ||
			 !equal(pchild->pathtarget->exprs, factexprs)))
			pchild = NULL;
	}

	/* ---- the price ---- */
	loops = clamp_row_est(child->rows);
	if (!lion_ordered_probe_price(root, dim, ps->pkidx, fact->relids,
								  pp.where, pp.wherequal, loops, &perprobe,
								  &probepath))
		return;
	if (pp.where != NULL)
	{
		double		members;
		double		bytes;

		setbuild += lion_ordered_set_price(root, dim, pp.where, &members,
										   &bytes);
		setbytes += bytes;
		memberfrac = Min(members / Max(dim->tuples, 1.0), 1.0);
		whereexact = lion_ordered_set_exact(root, pp.where);
		pp.lionpages += pj_lion_pages(pp.where);
	}
	setbuild += pp.labelcost;
	setbytes += pp.labelbytes;

	/* ... the anti join's keys, each a descent, their TIDs sorted into a set */
	if (ps->antipath != NULL)
	{
		double		arows = clamp_row_est(ps->antipath->rows);

		antibuild = arows * lion_ordered_lookup_price(probepath) +
			2.0 * cpu_operator_cost * arows * log2(Max(arows, 2.0)) +
			arows * cpu_operator_cost;
		setbytes += lion_ordered_set_bytes(arows, (double) dim->pages);
	}
	setbuild += lion_ordered_spill_price(setbytes);

	/*
	 * ... and each member's tests: the anti join's set, every label's, and
	 * the truth of every predicate, a cpu_operator_cost each
	 */
	permember = cpu_operator_cost *
		((ps->antipath != NULL ? 1 : 0) + list_length(pp.labels) + npred);

	/*
	 * The units (§39): against a nested loop into the B-tree, its cold
	 * reads against the node's own - a leaf a probe, the members' pages
	 * not all-visible (every member's where the WHERE's set is inexact:
	 * each is fetched for its recheck, as lo_cost() prices the probe), and
	 * the lion indexes' pages the builds read.  Not against a bitmap heap
	 * scan's cold reads (§39, "A bitmap heap scan's cold reads"): the node
	 * reads the fact through core's own path, as the competitor's scans of
	 * it do, and the dimension a descent at a time, synchronous reads the
	 * stream's price would under-charge.
	 */
	idxpages = Max((double) ps->pkidx->pages, 1.0);
	reads = index_pages_fetched(loops, (BlockNumber) idxpages, idxpages,
								root) +
		index_pages_fetched(loops * memberfrac, dim->pages, idxpages, root) *
		(whereexact ? 1.0 - dim->allvisfrac : 1.0) +
		pp.lionpages;
	lion_units_for(root, output_rel, &units);
	units.bmcold = 0.0;
	units.bmcoldpage = 0.0;
	lion_units_cold(&units,
					setbuild + antibuild + loops * perprobe +
					loops * memberfrac * permember, reads);

	/* ---- the plan's pieces ---- */
	if (pp.where != NULL)
	{
		List	   *tree = NIL;

		lion_ordered_lion_tree(pp.where, &tree, &leaves, &quals);
		trees = lappend(trees, tree);
		wheretree = 0;
		nwhereleaf = list_length(leaves);
		recheck = (List *) pj_index_vars_mutator((Node *) pp.wherequal,
												 &dim->relid);
	}
	foreach(lc, pp.labels)
	{
		List	   *tree = NIL;

		lion_ordered_lion_tree((Path *) lfirst(lc), &tree, &leaves, &quals);
		trees = lappend(trees, tree);
	}
	foreach(lc, quals)
		keyless = lappend(keyless, pj_keyless((Expr *) lfirst(lc)));
	if (pp.cells != NULL)
	{
		for (d = 0; d < pp.cells->ndim; d++)
			dims = lappend(dims, pj_dim_private(&pp, d,
												wheretree >= 0 ? 1 : 0));
		for (p = 0; p < npred; p++)
			preds = lappend(preds, pj_pred_private(&pp, p));
	}

	/*
	 * The scan tuple: the fact's columns, the predicates, and what EXPLAIN
	 * prints - the probe's clause with the key first, as core prints an
	 * Index Cond.
	 */
	tlexprs = list_copy(factexprs);
	if (pp.cells != NULL)
		tlexprs = list_concat(tlexprs, pp.cells->predexprs);
	firstjunk = list_length(tlexprs) + 1;
	{
		OpExpr	   *jc = (OpExpr *) ps->joinclause->clause;
		Node	   *pkarg;
		Node	   *fkarg;

		if (lion_strip((Node *) linitial(jc->args)) == (Node *) ps->pkvar ||
			equal(lion_strip((Node *) linitial(jc->args)), ps->pkvar))
		{
			pkarg = (Node *) linitial(jc->args);
			fkarg = (Node *) lsecond(jc->args);
		}
		else
		{
			pkarg = (Node *) lsecond(jc->args);
			fkarg = (Node *) linitial(jc->args);
		}
		tlexprs = lappend(tlexprs,
						  make_opclause(ps->probeop, BOOLOID, false,
										(Expr *) copyObject(pkarg),
										(Expr *) copyObject(fkarg),
										InvalidOid, ps->collation));
		probetl = list_length(tlexprs);
	}
	if (pp.where != NULL)
	{
		tlexprs = lappend(tlexprs, make_ands_explicit(pp.wherequal));
		wheretl = list_length(tlexprs);
	}
	if (ps->anticlause != NULL)
	{
		tlexprs = lappend(tlexprs, copyObject(ps->anticlause->clause));
		antitl = list_length(tlexprs);
	}

	priv = list_make1(list_make1_int(PJ_PRIV_MAGIC));
	{
		List	   *ints = (List *) linitial(priv);

		ints = lappend_int(ints, list_length(factexprs));
		ints = lappend_int(ints, fkcol);
		ints = lappend_int(ints, wheretree);
		ints = lappend_int(ints, nwhereleaf);
		ints = lappend_int(ints, pp.cells != NULL ? pp.cells->ndim : 0);
		ints = lappend_int(ints, npred);
		ints = lappend_int(ints, anticol);
		ints = lappend_int(ints, probetl);
		ints = lappend_int(ints, wheretl);
		ints = lappend_int(ints, antitl);
		Assert(list_length(ints) == PJ_INT_N);
		linitial(priv) = ints;
	}
	priv = lappend(priv, list_make4_oid(ps->pkidx->indexoid, ps->probeop,
										ps->collation, ps->antiop));
	priv = lappend(priv, trees);
	priv = lappend(priv, leaves);
	priv = lappend(priv, dims);
	priv = lappend(priv, preds);
	priv = lappend(priv, recheck);
	Assert(list_length(priv) == PJ_PRIV_NMEMBERS);
	bundle = list_make4(priv, tlexprs,
						list_make2_int(firstjunk, (int) dim->relid), keyless);

	/* ---- the paths: serial, and partial over the fact's partial path ---- */
	rows = clamp_row_est(Min(input_rel->rows, loops));
	serial = pj_make_path(root, &pp, child, ps->antipath, bundle, &units, setbuild,
						  antibuild, perprobe, permember, memberfrac, rows,
						  false);
	if (pchild != NULL)
	{
		double		divisor = lion_parallel_divisor(pchild->parallel_workers);

		partial = pj_make_path(root, &pp, pchild, pantichild, bundle, &units, setbuild,
							   antibuild, perprobe, permember, memberfrac,
							   clamp_row_est(rows / divisor), true);
		if (!partial->path.parallel_safe)
			partial = NULL;
	}
	elog(DEBUG2, "lion: LionProbeJoin of %d over %d, %.0f probes, sets %.2f, a probe %.2f, cost %.2f",
		 (int) dim->relid, (int) fact->relid, loops, setbuild, perprobe,
		 serial->path.total_cost);
	pj_add_aggs(root, &pp, extra, serial, partial);
}

/*
 * One shape, the dimension's ranges priced with the ends lion holds (§28),
 * as a base relation's WHERE is.
 */
static void
pj_try_shape_probed(PlannerInfo *root, RelOptInfo *input_rel,
					RelOptInfo *output_rel, GroupPathExtraData *extra,
					LionProbeShape *ps)
{
	RelOptInfo *dim = ps->dimrel;

	if (!lion_probe_begin(root, dim, dim->baserestrictinfo))
	{
		pj_try_shape(root, input_rel, output_rel, extra, ps);
		return;
	}
	PG_TRY();
	{
		pj_try_shape(root, input_rel, output_rel, extra, ps);
	}
	PG_FINALLY();
	{
		lion_probe_end();
	}
	PG_END_TRY();
}

/*
 * The dimension by probe (DESIGN.md §46), offered at the GROUP BY of a join
 * (lion_create_upper_paths()): each way lion_fkjoin_recognize_probe() takes
 * the join apart, priced against core's plan, which add_path() chooses
 * between.
 */
void
lion_try_probe_join(PlannerInfo *root, RelOptInfo *input_rel,
					RelOptInfo *output_rel, GroupPathExtraData *extra)
{
	LionProbeShape *shapes;
	int			n;
	int			i;

	if (!lion_enable_probe_join || extra == NULL)
		return;
	if (root->parse->groupingSets != NIL || root->parse->hasTargetSRFs)
		return;
	if (input_rel->reloptkind != RELOPT_JOINREL)
		return;
	n = lion_fkjoin_recognize_probe(root, input_rel, &shapes);
	for (i = 0; i < n; i++)
		pj_try_shape_probed(root, input_rel, output_rel, extra, &shapes[i]);
}

/*
 * The plan: the scan tuple's target list made of the planner's expressions,
 * the dimension its scan relation, so that EXPLAIN names it.
 */
static Plan *
pj_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
			 List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	List	   *bundle = best_path->custom_private;
	List	   *tlexprs;
	List	   *ints;
	int			firstjunk;
	int			resno = 0;
	ListCell   *lc;

	if (list_length(bundle) != 4)
		elog(ERROR, "LionProbeJoin: malformed path");
	tlexprs = (List *) list_nth(bundle, PJ_PATH_TLIST);
	ints = (List *) list_nth(bundle, PJ_PATH_INTS);
	firstjunk = linitial_int(ints);
	if (clauses != NIL)
		elog(ERROR, "LionProbeJoin: a restriction on the join's own rel");

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = NIL;
	cscan->scan.scanrelid = (Index) lsecond_int(ints);
	cscan->flags = best_path->flags;
	cscan->custom_plans = custom_plans;
	cscan->custom_exprs = (List *) copyObject(list_nth(bundle, PJ_PATH_QUALS));
	cscan->custom_private = (List *) copyObject(list_nth(bundle,
														 PJ_PATH_PRIV));
	foreach(lc, tlexprs)
	{
		resno++;
		cscan->custom_scan_tlist =
			lappend(cscan->custom_scan_tlist,
					makeTargetEntry((Expr *) copyObject(lfirst(lc)),
									(AttrNumber) resno, NULL,
									resno >= firstjunk));
	}
	cscan->methods = &pj_scan_methods;
	return &cscan->scan.plan;
}

/* ---------------------------------------------------------------------
 * Execution
 * --------------------------------------------------------------------- */

static Node *
pj_create_state(CustomScan *cscan)
{
	LionProbeJoinState *st = (LionProbeJoinState *)
		newNode(sizeof(LionProbeJoinState), T_CustomScanState);

	st->css.methods = &pj_exec_methods;
	st->css.slotOps = &TTSOpsVirtual;
	return (Node *) st;
}

/* member m of custom_private, a List of the type given */
static List *
pj_priv_list(List *priv, int m, NodeTag tag, bool nilok)
{
	List	   *l = (List *) list_nth(priv, m);

	if (l == NIL ? !nilok : nodeTag(l) != tag)
		elog(ERROR, "LionProbeJoin: malformed custom_private member %d", m);
	return l;
}

/* A scan key of the B-tree's one column for the equality opno */
static void
pj_scankey(ScanKey key, Relation index, Oid opno, Oid collation)
{
	int			strategy;
	Oid			lefttype;
	Oid			righttype;

	get_op_opfamily_properties(opno, index->rd_opfamily[0], false,
							   &strategy, &lefttype, &righttype);
	if (strategy != BTEqualStrategyNumber)
		elog(ERROR, "LionProbeJoin: operator %u is no equality of \"%s\"",
			 opno, RelationGetRelationName(index));
	ScanKeyEntryInitialize(key, 0, 1, (StrategyNumber) strategy, righttype,
						   collation, get_opcode(opno), (Datum) 0);
}

static void
pj_decode(LionProbeJoinState *st, CustomScan *cscan)
{
	List	   *priv = cscan->custom_private;
	List	   *ints;
	List	   *oids;
	List	   *dims;
	List	   *preds;
	int			d;
	int			p;

	if (list_length(priv) != PJ_PRIV_NMEMBERS)
		elog(ERROR, "LionProbeJoin: malformed custom_private");
	ints = pj_priv_list(priv, PJ_PRIV_INTS, T_IntList, false);
	oids = pj_priv_list(priv, PJ_PRIV_OIDS, T_OidList, false);
	if (list_length(ints) != PJ_INT_N || list_length(oids) != PJ_OID_N ||
		list_nth_int(ints, PJ_INT_MAGIC) != PJ_PRIV_MAGIC)
		elog(ERROR, "LionProbeJoin: malformed custom_private");
	st->nfact = list_nth_int(ints, PJ_INT_NFACT);
	st->fkcol = (AttrNumber) list_nth_int(ints, PJ_INT_FKCOL);
	st->wheretree = list_nth_int(ints, PJ_INT_WHERE);
	st->ndim = list_nth_int(ints, PJ_INT_NDIM);
	st->npred = list_nth_int(ints, PJ_INT_NPRED);
	st->antikeycol = (AttrNumber) list_nth_int(ints, PJ_INT_ANTICOL);
	st->pkoid = list_nth_oid(oids, PJ_OID_PK);
	st->probeop = list_nth_oid(oids, PJ_OID_PROBEOP);
	st->collation = list_nth_oid(oids, PJ_OID_COLL);
	st->antiop = list_nth_oid(oids, PJ_OID_ANTIOP);
	st->ntree = list_length((List *) list_nth(priv, PJ_PRIV_TREES));
	if (st->nfact < 1 || st->fkcol < 1 || st->fkcol > st->nfact ||
		st->wheretree < -1 || st->wheretree >= Max(st->ntree, 1) ||
		st->ndim < 0 || st->ndim > LION_CELLS_MAX_DIMS ||
		st->npred < 0 || st->npred > LION_CELLS_MAX_PREDS ||
		(st->npred > 0) != (st->ndim > 0) ||
		st->antikeycol < 0 || (st->antikeycol > 0) != OidIsValid(st->antiop) ||
		!OidIsValid(st->pkoid) || !OidIsValid(st->probeop) ||
		list_length(cscan->custom_scan_tlist) < st->nfact + st->npred)
		elog(ERROR, "LionProbeJoin: malformed custom_private");

	dims = pj_priv_list(priv, PJ_PRIV_DIMS, T_List, true);
	preds = pj_priv_list(priv, PJ_PRIV_PREDS, T_List, true);
	if (list_length(dims) != st->ndim || list_length(preds) != st->npred)
		elog(ERROR, "LionProbeJoin: malformed cells");
	st->dims = (PjDim *) palloc0(sizeof(PjDim) * Max(st->ndim, 1));
	st->dimpriv = (LionCellsDimPriv *)
		palloc0(sizeof(LionCellsDimPriv) * Max(st->ndim, 1));
	for (d = 0; d < st->ndim; d++)
	{
		List	   *l = (List *) list_nth(dims, d);
		PjDim	   *dim = &st->dims[d];
		LionCellsDimPriv *pv = &st->dimpriv[d];
		int			i;

		if (!IsA(l, IntList) || list_length(l) < 4)
			elog(ERROR, "LionProbeJoin: malformed cells");
		pv->kind = dim->kind = list_nth_int(l, 0);
		pv->radix = list_nth_int(l, 1);
		pv->nullout = list_nth_int(l, 2);
		dim->nlabel = list_nth_int(l, 3);
		if ((dim->kind != LION_CELLDIM_SCALAR &&
			 dim->kind != LION_CELLDIM_MULTI) ||
			pv->radix < 1 || pv->nullout < 0 || pv->nullout >= pv->radix ||
			dim->nlabel < 0 || list_length(l) != 4 + 2 * dim->nlabel)
			elog(ERROR, "LionProbeJoin: malformed cells");
		dim->labels = (PjLabel *) palloc0(sizeof(PjLabel) *
										  Max(dim->nlabel, 1));
		for (i = 0; i < dim->nlabel; i++)
		{
			PjLabel    *lab = &dim->labels[i];

			lab->tree = list_nth_int(l, 4 + 2 * i);
			lab->value = list_nth_int(l, 5 + 2 * i);
			if (lab->tree < 0 || lab->tree >= st->ntree ||
				lab->tree == st->wheretree ||
				(dim->kind == LION_CELLDIM_SCALAR ?
				 (lab->value < 1 || lab->value >= pv->radix) :
				 (lab->value < -1 || lab->value >= LION_CELLS_MAX_BITS ||
				  (lab->value >= 0 && (1 << lab->value) >= pv->nullout))))
				elog(ERROR, "LionProbeJoin: malformed cells");
		}
	}
	st->preds = (LionCellsPredPriv *)
		palloc0(sizeof(LionCellsPredPriv) * Max(st->npred, 1));
	for (p = 0; p < st->npred; p++)
	{
		List	   *l = (List *) list_nth(preds, p);
		LionCellsPredPriv *pr = &st->preds[p];
		int			ntruth;
		int			i;

		if (!IsA(l, IntList) || list_length(l) < 4)
			elog(ERROR, "LionProbeJoin: malformed cells");
		pr->dim = list_nth_int(l, 0);
		pr->kind = list_nth_int(l, 1);
		pr->bit = list_nth_int(l, 2);
		ntruth = list_nth_int(l, 3);
		if (pr->dim < 0 || pr->dim >= st->ndim ||
			list_length(l) != 4 + ntruth ||
			(pr->kind == LION_CELLPRED_TABLE ?
			 ntruth != st->dimpriv[pr->dim].radix :
			 pr->kind == LION_CELLPRED_NULLS ? ntruth != 2 :
			 (pr->kind != LION_CELLPRED_BIT &&
			  pr->kind != LION_CELLPRED_NONE) || ntruth != 0) ||
			(pr->kind == LION_CELLPRED_BIT &&
			 (pr->bit < 0 || pr->bit >= LION_CELLS_MAX_BITS)))
			elog(ERROR, "LionProbeJoin: malformed cells");
		pr->truth = (uint8 *) palloc(sizeof(uint8) * Max(ntruth, 1));
		for (i = 0; i < ntruth; i++)
		{
			int			t = list_nth_int(l, 4 + i);

			if (t != LION_TRUTH_FALSE && t != LION_TRUTH_TRUE &&
				t != LION_TRUTH_NULL)
				elog(ERROR, "LionProbeJoin: malformed cells");
			pr->truth[i] = (uint8) t;
		}
	}
}

static void
pj_begin(CustomScanState *node, EState *estate, int eflags)
{
	LionProbeJoinState *st = (LionProbeJoinState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	List	   *priv = cscan->custom_private;
	Relation	heap = node->ss.ss_currentRelation;
	List	   *recheck;
	Plan	   *factplan;

	pj_decode(st, cscan);
	if (list_length(cscan->custom_plans) != (st->antikeycol > 0 ? 2 : 1))
		elog(ERROR, "LionProbeJoin: malformed child plans");
	factplan = (Plan *) linitial(cscan->custom_plans);
	if (list_length(factplan->targetlist) != st->nfact)
		elog(ERROR, "LionProbeJoin: the fact's plan returns %d columns, not %d",
			 list_length(factplan->targetlist), st->nfact);
	if (heap == NULL)
		elog(ERROR, "LionProbeJoin: no dimension to probe");
	st->vmbuffer = InvalidBuffer;
	st->outs = (int *) palloc0(sizeof(int) * Max(st->ndim, 1));

	/* the children, as core starts a join's */
	st->factps = ExecInitNode(factplan, estate, eflags);
	node->custom_ps = list_make1(st->factps);
	if (st->antikeycol > 0)
	{
		Plan	   *antiplan = (Plan *) lsecond(cscan->custom_plans);

		if (st->antikeycol > list_length(antiplan->targetlist))
			elog(ERROR, "LionProbeJoin: malformed anti join");
		st->antips = ExecInitNode(antiplan, estate, eflags);
		node->custom_ps = lappend(node->custom_ps, st->antips);
		st->antiset = st->ntree;
	}

	/*
	 * The WHERE's recheck, over the heap tuple, for a set a source built
	 * inexact (lion_source_exact()): initialised, and so checked, whenever
	 * the plan starts, as the scan's own quals are.  With no parent, so that
	 * its Vars are read from the table slot it is given, deformed as that
	 * slot's type is: the node's scan slot, whose type the parent would fix,
	 * is the virtual one of custom_scan_tlist.  The planner took no WHERE
	 * with a SubPlan in it, which would need one.
	 */
	recheck = pj_priv_list(priv, PJ_PRIV_RECHECK, T_List, true);
	if (recheck != NIL)
	{
		recheck = (List *) copyObject(recheck);
		fix_opfuncids((Node *) recheck);
		st->recheck = ExecInitQual(recheck, NULL);
	}

	/* the sets' trees; EXPLAIN without ANALYZE opens nothing */
	st->sets = lion_probe_sets_begin(&node->ss.ps,
									 (List *) list_nth(priv, PJ_PRIV_TREES),
									 pj_priv_list(priv, PJ_PRIV_LEAVES,
												  T_OidList, true),
									 cscan->custom_exprs,
									 st->antikeycol > 0 ? 1 : 0,
									 (eflags & EXEC_FLAG_EXPLAIN_ONLY) == 0);
	st->lionparams = pull_paramids((Expr *) cscan->custom_exprs);
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	lion_check_table_am(heap);
	if (!IsMVCCSnapshot(estate->es_snapshot))
		elog(ERROR, "LionProbeJoin: requires an MVCC snapshot");

	/*
	 * The B-tree, one scan of it for every lookup, its index tuples asked
	 * for so that nbtree keeps the leaf's pin while the visibility map is
	 * tested (DESIGN.md §40.5).
	 */
	st->pkidx = index_open(st->pkoid, AccessShareLock);
	if (st->pkidx->rd_rel->relam != BTREE_AM_OID ||
		IndexRelationGetNumberOfKeyAttributes(st->pkidx) != 1 ||
		st->pkidx->rd_index->indrelid != RelationGetRelid(heap))
		elog(ERROR, "LionProbeJoin: \"%s\" is not a one-column B-tree of \"%s\"",
			 RelationGetRelationName(st->pkidx), RelationGetRelationName(heap));
	pj_scankey(&st->probekey, st->pkidx, st->probeop, st->collation);
	if (st->antikeycol > 0)
		pj_scankey(&st->antikey, st->pkidx, st->antiop, st->collation);
#if PG_VERSION_NUM >= 200000
	st->scan = index_beginscan(heap, st->pkidx, false, estate->es_snapshot,
							   NULL, 1, 0, SO_NONE);
#elif PG_VERSION_NUM >= 190000
	st->scan = index_beginscan(heap, st->pkidx, estate->es_snapshot, NULL,
							   1, 0, SO_NONE);
#elif PG_VERSION_NUM >= 180000
	st->scan = index_beginscan(heap, st->pkidx, estate->es_snapshot, NULL,
							   1, 0);
#else
	st->scan = index_beginscan(heap, st->pkidx, estate->es_snapshot, 1, 0);
#endif
	st->scan->xs_want_itup = true;
	st->tabslot = table_slot_create(heap, &estate->es_tupleTable);
}

/* The next TID the B-tree's scan finds, or NULL */
static ItemPointer
pj_next_tid(LionProbeJoinState *st)
{
#if PG_VERSION_NUM >= 200000
	if (!tableam_index_getnext_tid(st->scan, ForwardScanDirection))
		return NULL;
	return &st->scan->xs_heaptid;
#else
	return index_getnext_tid(st->scan, ForwardScanDirection);
#endif
}

/*
 * The version of the TID just found that the snapshot sees, into slot, as
 * an index scan fetches it (lo_fetch_btree()).
 */
static bool
pj_fetch(LionProbeJoinState *st, TupleTableSlot *slot)
{
#if PG_VERSION_NUM >= 200000
	IndexScanDesc scan = st->scan;
	ItemPointerData tid = scan->xs_heaptid;
	bool		all_dead = false;
	Snapshot	snapshot = scan->xs_snapshot;

	if (!table_fetch_tid(scan->heapRelation, &tid, snapshot, &all_dead))
	{
		if (!scan->xactStartedInRecovery)
			scan->kill_prior_tuple = all_dead;
		return false;
	}
	return table_tuple_fetch_row_version(scan->heapRelation, &tid, snapshot,
										 slot);
#else
	return index_fetch_heap(st->scan, slot);
#endif
}

/*
 * The anti join's set (DESIGN.md §46, "The anti join"): every row of its
 * child read, each non-NULL key looked up in the B-tree under the query's
 * snapshot, and every TID found - visible or not: a dimension row the probe
 * meets is tested for its visibility on its own - sorted into a set.  The
 * sort keeps within work_mem and spills past it.
 */
static void
pj_build_anti(LionProbeJoinState *st)
{
	Tuplesortstate *codes;

	codes = tuplesort_begin_datum(INT8OID, Int8LessOperator, InvalidOid,
								  false, work_mem, NULL, TUPLESORT_NONE);
	for (;;)
	{
		TupleTableSlot *aslot;
		Datum		key;
		bool		isnull;
		ItemPointer tid;

		CHECK_FOR_INTERRUPTS();
		aslot = ExecProcNode(st->antips);
		if (TupIsNull(aslot))
			break;
		st->antirows++;
		key = slot_getattr(aslot, st->antikeycol, &isnull);
		if (isnull)
			continue;			/* `= NULL` is never true */
		st->antikey.sk_argument = key;
		index_rescan(st->scan, &st->antikey, 1, NULL, 0);
		while ((tid = pj_next_tid(st)) != NULL)
		{
			tuplesort_putdatum(codes,
							   Int64GetDatum((int64) lion_tid_to_code(tid)),
							   false);
			st->antitids++;
		}
	}
	tuplesort_performsort(codes);
	lion_probe_sets_put_codes(st->sets, st->antiset, codes);
	tuplesort_end(codes);
}

/*
 * The sets, for this scan's Params: every tree's, then the anti join's.  A
 * label must be exact - the cells' analysis took only predicates a posting
 * set decides (lion_plan_cells.c) - and the WHERE is rechecked on the heap
 * tuple where its set is not.
 */
static void
pj_build(LionProbeJoinState *st)
{
	int			d;
	int			i;

	lion_probe_sets_build(st->sets);
	for (d = 0; d < st->ndim; d++)
	{
		for (i = 0; i < st->dims[d].nlabel; i++)
		{
			if (!lion_probe_sets_exact(st->sets, st->dims[d].labels[i].tree))
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("LionProbeJoin: a cell predicate's set of \"%s\" is not exact",
								RelationGetRelationName(st->css.ss.ss_currentRelation))));
		}
	}
	st->whereexact = (st->wheretree < 0 ||
					  lion_probe_sets_exact(st->sets, st->wheretree));
	if (!st->whereexact && st->recheck == NULL)
		elog(ERROR, "LionProbeJoin: an inexact WHERE with nothing to recheck");
	if (st->antips != NULL)
		pj_build_anti(st);
	st->built = true;
}

/* The outcome of dimension d of the row at tid: which of its labels hold it */
static int
pj_outcome(LionProbeJoinState *st, int d, ItemPointer tid)
{
	const PjDim *dim = &st->dims[d];
	int			mask = 0;
	int			i;

	if (dim->kind == LION_CELLDIM_SCALAR)
	{
		for (i = 0; i < dim->nlabel; i++)
		{
			if (lion_probe_sets_has(st->sets, dim->labels[i].tree, tid))
				return dim->labels[i].value;
		}
		return 0;				/* any other value */
	}
	for (i = 0; i < dim->nlabel; i++)
	{
		const PjLabel *lab = &dim->labels[i];

		if (!lion_probe_sets_has(st->sets, lab->tree, tid))
			continue;
		if (lab->value < 0)
			return st->dimpriv[d].nullout;
		mask |= 1 << lab->value;
	}
	return mask;
}

/*
 * The row that goes up: the fact row's columns, and each predicate's truth
 * in the dimension row - true, false or NULL - from its dimension's outcome
 * (lion_cells_pred_truth()).  EXPLAIN's columns are NULL.
 */
static TupleTableSlot *
pj_emit(LionProbeJoinState *st, TupleTableSlot *fslot, ItemPointer tid)
{
	TupleTableSlot *slot = st->css.ss.ss_ScanTupleSlot;
	int			natts = slot->tts_tupleDescriptor->natts;
	int			d;
	int			p;

	ExecClearTuple(slot);
	slot_getsomeattrs(fslot, st->nfact);
	memcpy(slot->tts_values, fslot->tts_values, sizeof(Datum) * st->nfact);
	memcpy(slot->tts_isnull, fslot->tts_isnull, sizeof(bool) * st->nfact);
	for (d = 0; d < st->ndim; d++)
		st->outs[d] = pj_outcome(st, d, tid);
	for (p = 0; p < st->npred; p++)
	{
		const LionCellsPredPriv *pr = &st->preds[p];
		uint8		t = lion_cells_pred_truth(&st->dimpriv[pr->dim], pr,
											  st->outs[pr->dim]);

		slot->tts_values[st->nfact + p] = BoolGetDatum(t == LION_TRUTH_TRUE);
		slot->tts_isnull[st->nfact + p] = (t == LION_TRUTH_NULL);
	}
	for (p = st->nfact + st->npred; p < natts; p++)
	{
		slot->tts_values[p] = (Datum) 0;
		slot->tts_isnull[p] = true;
	}
	return ExecStoreVirtualTuple(slot);
}

/*
 * The next row: fact rows read until one finds its dimension row - by its
 * key in the B-tree, the TID in the WHERE's set and not in the anti join's,
 * and visible, by the visibility map or the heap - the first such row of
 * the key being its only one, the key unique.  A NULL key finds none, as
 * the join's strict equality finds none.
 */
static TupleTableSlot *
pj_next(ScanState *ss)
{
	LionProbeJoinState *st = (LionProbeJoinState *) ss;
	EState	   *estate = ss->ps.state;
	ExprContext *econtext = ss->ps.ps_ExprContext;
	Relation	heap = ss->ss_currentRelation;

	st->ran = true;
	if (!st->built)
		pj_build(st);

	for (;;)
	{
		TupleTableSlot *fslot;
		ItemPointer tid;
		Datum		key;
		bool		isnull;

		CHECK_FOR_INTERRUPTS();
		fslot = ExecProcNode(st->factps);
		if (TupIsNull(fslot))
			return ExecClearTuple(ss->ss_ScanTupleSlot);
		key = slot_getattr(fslot, st->fkcol, &isnull);
		if (isnull)
		{
			st->nullkeys++;
			continue;
		}
		st->probes++;
		st->probekey.sk_argument = key;
		index_rescan(st->scan, &st->probekey, 1, NULL, 0);
		while ((tid = pj_next_tid(st)) != NULL)
		{
			ItemPointerData found = *tid;
			bool		fromheap = false;

			if (st->wheretree >= 0 &&
				!lion_probe_sets_has(st->sets, st->wheretree, &found))
			{
				st->wherefail++;
				continue;
			}
			if (st->antips != NULL &&
				lion_probe_sets_has(st->sets, st->antiset, &found))
			{
				st->antifail++;
				continue;
			}

			/*
			 * Visible: by the map, every tuple of an all-visible page is,
			 * tested under the leaf's pin; or by the heap, whose fetch
			 * follows the HOT chain from its root to the version the
			 * snapshot sees - read too for a recheck of the WHERE.
			 */
			if (!st->whereexact ||
				!VM_ALL_VISIBLE(heap, ItemPointerGetBlockNumber(&found),
								&st->vmbuffer))
			{
				st->fetched++;
				if (!pj_fetch(st, st->tabslot))
				{
					st->invisible++;
					continue;
				}
				fromheap = true;
			}
			else
				st->vmskips++;
			if (!st->whereexact)
			{
				slot_getallattrs(st->tabslot);
				ResetExprContext(econtext);
				econtext->ecxt_scantuple = st->tabslot;
				if (!ExecQual(st->recheck, econtext))
				{
					ExecClearTuple(st->tabslot);
					st->recheckfail++;
					continue;
				}
			}
			if (fromheap)
				ExecClearTuple(st->tabslot);
			else
			{
				/*
				 * A row taken without a heap visit has no tuple to lock:
				 * its page is, as core's Index Only Scan locks it.
				 */
				PredicateLockPage(heap, ItemPointerGetBlockNumber(&found),
								  estate->es_snapshot);
			}
			return pj_emit(st, fslot, &found);
		}
	}
}

/* EvalPlanQual's recheck: a row of a join is never rechecked (no row marks) */
static bool
pj_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
pj_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, (ExecScanAccessMtd) pj_next,
					(ExecScanRecheckMtd) pj_recheck);
}

/*
 * A rescan: the fact's child read again, rescanned now or by its next
 * ExecProcNode() with the Params that changed (core hands them on to
 * outer and inner plans, not to custom_ps); the sets kept unless a Param of
 * their quals or of the anti join's child changed, as LionOrdered keeps its
 * set (DESIGN.md §30.4).
 */
static void
pj_rescan(CustomScanState *node)
{
	LionProbeJoinState *st = (LionProbeJoinState *) node;
	Bitmapset  *chg = node->ss.ps.chgParam;
	bool		rebuild = false;

	ExecScanReScan(&node->ss);
	if (chg != NULL)
		UpdateChangedParamSet(st->factps, chg);
	if (st->factps->chgParam == NULL)
		ExecReScan(st->factps);
	if (st->antips != NULL)
	{
		if (chg != NULL)
			UpdateChangedParamSet(st->antips, chg);
		if (st->antips->chgParam != NULL)
			rebuild = true;
	}
	if (bms_overlap(chg, st->lionparams))
		rebuild = true;
	if (st->built && rebuild)
	{
		st->built = false;
		if (st->antips != NULL && st->antips->chgParam == NULL)
			ExecReScan(st->antips);
	}
}

static void
pj_end(CustomScanState *node)
{
	LionProbeJoinState *st = (LionProbeJoinState *) node;

	if (st->scan != NULL)
		index_endscan(st->scan);
	st->scan = NULL;
	if (BufferIsValid(st->vmbuffer))
		ReleaseBuffer(st->vmbuffer);
	st->vmbuffer = InvalidBuffer;
	if (st->tabslot != NULL)
		ExecClearTuple(st->tabslot);
	lion_probe_sets_end(st->sets);
	st->sets = NULL;
	if (st->pkidx != NULL)
		index_close(st->pkidx, AccessShareLock);
	st->pkidx = NULL;
	if (st->factps != NULL)
		ExecEndNode(st->factps);
	if (st->antips != NULL)
		ExecEndNode(st->antips);
}

/* ---------------------------------------------------------------------
 * EXPLAIN
 * --------------------------------------------------------------------- */

/* custom_scan_tlist's entry at resno, deparsed as EXPLAIN prints a qual */
static void
pj_explain_entry(CustomScanState *node, List *ancestors, ExplainState *es,
				 int resno, const char *label)
{
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	TargetEntry *tle;
	List	   *context;
	char	   *val;

	if (resno <= 0 || resno > list_length(cscan->custom_scan_tlist))
		return;
	tle = (TargetEntry *) list_nth(cscan->custom_scan_tlist, resno - 1);
	context = set_deparse_context_plan(es->deparse_cxt, &cscan->scan.plan,
									   ancestors);
	val = deparse_expression((Node *) tle->expr, context, true, false);
	ExplainPropertyText(label, val, es);
}

/* The names of leafoids[from, to), each once */
static List *
pj_index_names(List *leafoids, int from, int to)
{
	List	   *names = NIL;
	List	   *seen = NIL;
	int			i;

	for (i = from; i < to && i < list_length(leafoids); i++)
	{
		Oid			oid = list_nth_oid(leafoids, i);
		char	   *name;

		if (list_member_oid(seen, oid))
			continue;
		seen = lappend_oid(seen, oid);
		name = get_rel_name(oid);
		names = lappend(names, name != NULL ? name : "???");
	}
	return names;
}

/*
 * The B-tree probed and its clause; the WHERE lion answers and its indexes;
 * the GROUP BY's predicates and their indexes; the anti join's clause.
 * Under ANALYZE what the node did, in this process (a partial node's
 * workers keep theirs, as a nested loop's probe does, §40.10).
 */
static void
pj_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	LionProbeJoinState *st = (LionProbeJoinState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	List	   *priv = cscan->custom_private;
	List	   *ints = (List *) list_nth(priv, PJ_PRIV_INTS);
	List	   *leafoids = (List *) list_nth(priv, PJ_PRIV_LEAVES);
	int			nwhereleaf = list_nth_int(ints, PJ_INT_NWHERELEAF);
	char	   *pkname = get_rel_name(st->pkoid);

	ExplainPropertyText("Probe Index", pkname != NULL ? pkname : "???", es);
	pj_explain_entry(node, ancestors, es, list_nth_int(ints, PJ_INT_PROBETL),
					 "Probe Cond");
	if (st->wheretree >= 0)
	{
		pj_explain_entry(node, ancestors, es,
						 list_nth_int(ints, PJ_INT_WHERETL), "Lion Cond");
		ExplainPropertyList("Lion Indexes",
							pj_index_names(leafoids, 0, nwhereleaf), es);
	}
	if (st->npred > 0)
	{
		List	   *context = set_deparse_context_plan(es->deparse_cxt,
													   &cscan->scan.plan,
													   ancestors);
		List	   *texts = NIL;
		int			p;

		for (p = 0; p < st->npred; p++)
		{
			TargetEntry *tle = (TargetEntry *)
				list_nth(cscan->custom_scan_tlist, st->nfact + p);

			texts = lappend(texts,
							deparse_expression((Node *) tle->expr, context,
											   true, false));
		}
		ExplainPropertyList("Cell Predicates", texts, es);
		ExplainPropertyList("Cell Indexes",
							pj_index_names(leafoids, nwhereleaf,
										   list_length(leafoids)), es);
	}
	if (st->antikeycol > 0)
		pj_explain_entry(node, ancestors, es,
						 list_nth_int(ints, PJ_INT_ANTITL), "Anti Join Cond");

	if (es->analyze && st->ran && st->sets != NULL)
	{
		StringInfoData buf;
		uint64		members;
		uint64		ncont;
		Size		spilled;
		uint64		builds;

		ExplainPropertyInteger("Probes", NULL, (int64) st->probes, es);
		ExplainPropertyInteger("Null Keys", NULL, (int64) st->nullkeys, es);
		if (st->wheretree >= 0)
			ExplainPropertyInteger("Rows Removed by Lion Cond", NULL,
								   (int64) st->wherefail, es);
		if (st->antikeycol > 0)
			ExplainPropertyInteger("Rows Removed by Anti Join", NULL,
								   (int64) st->antifail, es);
		ExplainPropertyInteger("Heap TIDs Skipped via VM", NULL,
							   (int64) st->vmskips, es);
		ExplainPropertyInteger("Heap TIDs Rechecked", NULL,
							   (int64) st->fetched, es);
		ExplainPropertyInteger("Rows Removed by Visibility", NULL,
							   (int64) st->invisible, es);
		if (st->recheckfail > 0 || (st->built && !st->whereexact))
			ExplainPropertyInteger("Rows Removed by Lion Recheck", NULL,
								   (int64) st->recheckfail, es);
		lion_probe_sets_counts(st->sets, &members, &ncont, &spilled, &builds);
		initStringInfo(&buf);
		appendStringInfo(&buf, "%d, " UINT64_FORMAT " members in "
						 UINT64_FORMAT " containers",
						 st->ntree + (st->antikeycol > 0 ? 1 : 0),
						 members, ncont);
		if (st->built && !st->whereexact)
			appendStringInfoString(&buf, ", rechecked");
		if (spilled > 0)
			appendStringInfo(&buf, ", spilled %zu kB", (spilled + 1023) / 1024);
		if (builds > 1)
			appendStringInfo(&buf, ", " UINT64_FORMAT " builds", builds);
		ExplainPropertyText("Lion Sets", buf.data, es);
		pfree(buf.data);
		if (st->antikeycol > 0)
		{
			initStringInfo(&buf);
			appendStringInfo(&buf, UINT64_FORMAT " rows, " UINT64_FORMAT
							 " TIDs", st->antirows, st->antitids);
			ExplainPropertyText("Anti Join Keys", buf.data, es);
			pfree(buf.data);
		}
	}
}
