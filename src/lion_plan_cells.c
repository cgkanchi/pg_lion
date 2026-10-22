/*-------------------------------------------------------------------------
 *
 * lion_plan_cells.c
 *		Counts by cells (DESIGN.md §10, "Counts by cells"): the predicates
 *		of a GROUP BY's expressions, and of WHERE clauses no posting set
 *		answers, found; their columns made dimensions, each dimension's rows
 *		told apart into the outcomes the predicates see; and the column
 *		whose entries drive the count chosen.
 *
 * Part of the LionCount custom scan: lion_customscan.h describes the node
 * and declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_customscan.h"

/*
 * A scalar leaf: a test of one column's value that a lion index on the
 * column decides for every row, by the entry the row is in.
 */
#define CL_EQ		0			/* col = v */
#define CL_NE		1			/* col <> v */
#define CL_IN		2			/* col = ANY (array) */
#define CL_NOTIN	3			/* col <> ALL (array) */
#define CL_DISTINCT 4			/* col IS DISTINCT FROM v, v perhaps NULL */
#define CL_ISNULL	5
#define CL_NOTNULL	6
#define CL_BOOL		7			/* a boolean column by itself */
#define CL_BOOLTEST 8			/* col IS [NOT] TRUE / FALSE / UNKNOWN */

typedef struct CellLeaf
{
	Node	   *node;
	int			kind;			/* CL_* */
	Bitmapset  *vals;			/* the values it names, by number */
	bool		hasnull;		/* a NULL element of a list, or IS DISTINCT
								 * FROM NULL */
	bool		empty;			/* a list of no element */
	BoolTestType btype;			/* CL_BOOLTEST */
} CellLeaf;

/* A column the predicates are of, as it is being found. */
typedef struct CellDim
{
	Var		   *var;
	bool		multi;			/* it has a multi-key leaf */
	List	   *leaves;			/* CellLeaf, of its scalar trees */
	List	   *consts;			/* the values its leaves name, distinct */
	Oid			eqop;			/* ... compared by this equality */
	Oid			valtype;
	Oid			coll;
	bool		hasvalop;		/* eqop and coll are set */
	FmgrInfo	eqfn;
	bool		equalimage;
	List	   *preds;			/* its predicates, by number */
	int			nbit;			/* its multi-key leaves */
	Node	   *bitleaf[LION_CELLS_MAX_BITS];
	LionLeafInfo bitinfo[LION_CELLS_MAX_BITS];
} CellDim;

/* A predicate, as it is being found. */
typedef struct CellPred
{
	Node	   *expr;
	int			dim;
	int			kind;			/* LION_CELLPRED_*; TABLE also for a scalar
								 * tree on a multi-key column, until the
								 * column is known to be one */
	int			bit;
} CellPred;

typedef struct CellsCx
{
	PlannerInfo *root;
	RelOptInfo *rel;
	Index		rti;
	List	   *preds;			/* CellPred */
	CellDim		dims[LION_CELLS_MAX_DIMS];
	int			ndim;
	bool		failed;
} CellsCx;

/* A column of the relation counted, plain: no expression, no system column */
static Var *
cells_plain_var(Node *node, Index rti)
{
	Node	   *n = lion_strip(node);

	if (n == NULL || !IsA(n, Var))
		return NULL;
	if (((Var *) n)->varno != (int) rti || ((Var *) n)->varattno <= 0 ||
		((Var *) n)->varlevelsup != 0)
		return NULL;
	return (Var *) n;
}

/*
 * The strategy opno has in the opfamily of the scalar lion index on the
 * column - what tells an equality and its negator apart - or 0.  The index
 * that answers the leaf is matched again, against the dimension's own
 * equality, once all of them are known (cells_dim_index()).
 */
static int
cells_scalar_strategy(CellsCx *cx, Var *var, Oid opno)
{
	AttrNumber	col;
	IndexOptInfo *idx = lion_find_roaring_index(cx->rel, var->varattno,
												 false, &col);

	if (idx == NULL || !OidIsValid(opno))
		return 0;
	return get_op_opfamily_strategy(opno, idx->opfamily[col - 1]);
}

/*
 * Is node a scalar leaf (CL_*) of a column of the relation, and which one?
 * The column in *var, its kind in *kind, and the equality its values are
 * compared by in *eqop (InvalidOid for a null test).  A pure test: nothing
 * is noted.
 */
static bool
cells_scalar_leaf(CellsCx *cx, Node *node, Var **var, int *kind, Oid *eqop)
{
	*var = NULL;
	*eqop = InvalidOid;
	if (node == NULL)
		return false;

	if (IsA(node, Var))
	{
		Var		   *v = cells_plain_var(node, cx->rti);

		if (v == NULL || getBaseType(v->vartype) != BOOLOID)
			return false;
		*var = v;
		*kind = CL_BOOL;
		*eqop = BooleanEqualOperator;
		return true;
	}
	if (IsA(node, BooleanTest))
	{
		Var		   *v = cells_plain_var((Node *) ((BooleanTest *) node)->arg,
										cx->rti);

		if (v == NULL || getBaseType(v->vartype) != BOOLOID)
			return false;
		*var = v;
		*kind = CL_BOOLTEST;
		*eqop = BooleanEqualOperator;
		return true;
	}
	if (IsA(node, NullTest))
	{
		NullTest   *nt = (NullTest *) node;
		Var		   *v;

		if (nt->argisrow)
			return false;
		v = cells_plain_var((Node *) nt->arg, cx->rti);
		if (v == NULL)
			return false;
		*var = v;
		*kind = (nt->nulltesttype == IS_NULL) ? CL_ISNULL : CL_NOTNULL;
		return true;
	}
	if (IsA(node, OpExpr) || IsA(node, DistinctExpr))
	{
		OpExpr	   *op = (OpExpr *) node;
		Node	   *l;
		Node	   *r;
		Var		   *v;
		Const	   *c;
		Oid			opno = op->opno;
		int			strategy;

		if (list_length(op->args) != 2 || op->opretset)
			return false;
		l = lion_strip((Node *) linitial(op->args));
		r = lion_strip((Node *) lsecond(op->args));
		if (l == NULL || r == NULL)
			return false;
		if ((v = cells_plain_var(l, cx->rti)) != NULL && IsA(r, Const))
			c = (Const *) r;
		else if ((v = cells_plain_var(r, cx->rti)) != NULL && IsA(l, Const))
		{
			c = (Const *) l;
			opno = get_commutator(opno);
		}
		else
			return false;

		if (IsA(node, DistinctExpr))
		{
			/* the equality IS DISTINCT FROM is made of, either way round */
			if (cells_scalar_strategy(cx, v, opno) != LION_STRAT_EQUAL)
				return false;
			*kind = CL_DISTINCT;
			*eqop = opno;
			*var = v;
			return true;
		}
		if (c->constisnull)
			return false;
		strategy = cells_scalar_strategy(cx, v, opno);
		if (strategy == LION_STRAT_EQUAL)
		{
			*kind = CL_EQ;
			*eqop = opno;
		}
		else if (cells_scalar_strategy(cx, v, get_negator(opno)) ==
				 LION_STRAT_EQUAL)
		{
			*kind = CL_NE;
			*eqop = get_negator(opno);
		}
		else
			return false;
		*var = v;
		return true;
	}
	if (IsA(node, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *sa = (ScalarArrayOpExpr *) node;
		Node	   *arr;
		Var		   *v;

		if (list_length(sa->args) != 2)
			return false;
		v = cells_plain_var((Node *) linitial(sa->args), cx->rti);
		arr = lion_strip((Node *) lsecond(sa->args));
		if (v == NULL || arr == NULL || !IsA(arr, Const) ||
			((Const *) arr)->constisnull)
			return false;
		if (sa->useOr &&
			cells_scalar_strategy(cx, v, sa->opno) == LION_STRAT_EQUAL)
		{
			*kind = CL_IN;
			*eqop = sa->opno;
		}
		else if (!sa->useOr &&
				 cells_scalar_strategy(cx, v, get_negator(sa->opno)) ==
				 LION_STRAT_EQUAL)
		{
			*kind = CL_NOTIN;
			*eqop = get_negator(sa->opno);
		}
		else
			return false;
		*var = v;
		return true;
	}
	return false;
}

/*
 * The column of a SCALAR TREE - an AND, OR or NOT of scalar leaves of one
 * column, or one leaf - or 0 when node is none.  Its truth in a row is a
 * function of the row's value of the column alone, so the whole tree is one
 * predicate.
 */
static AttrNumber
cells_scalar_tree(CellsCx *cx, Node *node)
{
	Var		   *v;
	int			kind;
	Oid			eqop;

	if (node == NULL)
		return 0;
	if (IsA(node, BoolExpr))
	{
		BoolExpr   *b = (BoolExpr *) node;
		AttrNumber	attno = 0;
		ListCell   *lc;

		foreach(lc, b->args)
		{
			AttrNumber	a = cells_scalar_tree(cx, (Node *) lfirst(lc));

			if (a == 0 || (attno != 0 && a != attno))
				return 0;
			attno = a;
		}
		return attno;
	}
	if (cells_scalar_leaf(cx, node, &v, &kind, &eqop))
		return v->varattno;
	return 0;
}

/*
 * Is node a multi-key leaf - `col @> q`, `col && q`, `col @@ q` with a
 * literal q - of a column of the relation?  *none says that its query
 * selects no row (`tags && '{}'`); otherwise the posting sets answer it
 * exactly, and *leaf is its analysis (lion_analyze_leaf()).
 */
static bool
cells_multi_leaf(CellsCx *cx, Node *node, Var **var, LionLeafInfo *leaf,
				 bool *none)
{
	OpExpr	   *op;
	Var		   *v;
	Node	   *r;
	AttrNumber	col;
	IndexOptInfo *idx;

	*none = false;
	if (node == NULL || !IsA(node, OpExpr))
		return false;
	op = (OpExpr *) node;
	if (list_length(op->args) != 2)
		return false;
	v = cells_plain_var((Node *) linitial(op->args), cx->rti);
	r = lion_strip((Node *) lsecond(op->args));
	if (v == NULL || r == NULL || !IsA(r, Const) || ((Const *) r)->constisnull)
		return false;
	idx = lion_find_roaring_index(cx->rel, v->varattno, true, &col);
	if (idx == NULL ||
		!LION_STRAT_IS_MULTI_SEARCH(get_op_opfamily_strategy(op->opno,
															 idx->opfamily[col - 1])))
		return false;
	*var = v;
	/*
	 * Exact: not a superset that the positions an index stores would narrow
	 * (leaf->positions), which no key's set says alone.
	 */
	if (lion_analyze_leaf(cx->root, node, cx->rti, false, false, false, leaf) &&
		leaf->kind == LION_CLAUSE_MULTI && !leaf->positions &&
		leaf->var != NULL && leaf->var->varattno == v->varattno &&
		IsA(leaf->val, Const))
		return true;
	if (lion_multikey_literal_mode(idx, col, op->opno, (Const *) r) ==
		LION_QMODE_NONE)
	{
		*none = true;
		return true;
	}
	return false;
}

/* The dimension of column var, made when it is first met */
static int
cells_dim_of(CellsCx *cx, Var *var)
{
	int			d;

	for (d = 0; d < cx->ndim; d++)
	{
		if (cx->dims[d].var->varattno == var->varattno)
			return d;
	}
	if (cx->ndim >= LION_CELLS_MAX_DIMS)
	{
		cx->failed = true;
		return -1;
	}
	memset(&cx->dims[cx->ndim], 0, sizeof(CellDim));
	cx->dims[cx->ndim].var = var;
	return cx->ndim++;
}

/*
 * The number of value c among the dimension's, added when it is a new one:
 * the values are told apart by the dimension's equality, so that no two of
 * them are one entry of the index.
 */
static int
cells_value_no(CellsCx *cx, CellDim *dim, Datum value, Oid type)
{
	int16		typlen;
	bool		typbyval;
	ListCell   *lc;
	Const	   *c;

	get_typlenbyval(type, &typlen, &typbyval);
	foreach(lc, dim->consts)
	{
		Const	   *e = (Const *) lfirst(lc);

		if (datumIsEqual(e->constvalue, value, typbyval, typlen))
			return foreach_current_index(lc);
		if (!dim->equalimage &&
			DatumGetBool(FunctionCall2Coll(&dim->eqfn, dim->coll,
										   e->constvalue, value)))
			return foreach_current_index(lc);
	}
	if (list_length(dim->consts) >= LION_CELLS_MAX_VALUES)
	{
		cx->failed = true;
		return 0;
	}
	c = makeConst(type, -1, dim->coll, typlen, datumCopy(value, typbyval, typlen),
				  false, typbyval);
	dim->consts = lappend(dim->consts, c);
	return list_length(dim->consts) - 1;
}

/*
 * The equality and collation the dimension's values are compared by: every
 * leaf that names a value has to name it by the same, an equality of one
 * type on both sides.
 */
static bool
cells_dim_valop(CellsCx *cx, CellDim *dim, Oid eqop, Oid coll)
{
	Oid			lt;
	Oid			rt;

	if (dim->hasvalop)
		return dim->eqop == eqop && dim->coll == coll;
	op_input_types(eqop, &lt, &rt);
	if (!OidIsValid(lt) || lt != rt)
		return false;
	dim->eqop = eqop;
	dim->valtype = rt;
	dim->coll = coll;
	dim->equalimage = lion_type_equalimage(rt, coll);
	fmgr_info(get_opcode(eqop), &dim->eqfn);
	dim->hasvalop = true;
	return true;
}

/*
 * Note the leaves of scalar tree node, a predicate of dimension dim: each
 * leaf with the values it names, numbered among the dimension's.
 */
static void
cells_note_leaves(CellsCx *cx, CellDim *dim, Node *node)
{
	CellLeaf   *leaf;
	Var		   *v;
	int			kind;
	Oid			eqop;
	Oid			coll = InvalidOid;

	if (cx->failed)
		return;
	if (IsA(node, BoolExpr))
	{
		ListCell   *lc;

		foreach(lc, ((BoolExpr *) node)->args)
			cells_note_leaves(cx, dim, (Node *) lfirst(lc));
		return;
	}
	if (!cells_scalar_leaf(cx, node, &v, &kind, &eqop))
	{
		cx->failed = true;
		return;
	}
	leaf = (CellLeaf *) palloc0(sizeof(CellLeaf));
	leaf->node = node;
	leaf->kind = kind;

	if (IsA(node, OpExpr) || IsA(node, DistinctExpr))
		coll = ((OpExpr *) node)->inputcollid;
	else if (IsA(node, ScalarArrayOpExpr))
		coll = ((ScalarArrayOpExpr *) node)->inputcollid;
	if (OidIsValid(eqop) && !cells_dim_valop(cx, dim, eqop, coll))
	{
		cx->failed = true;
		return;
	}

	switch (kind)
	{
		case CL_EQ:
		case CL_NE:
		case CL_DISTINCT:
			{
				OpExpr	   *op = (OpExpr *) node;
				Node	   *l = lion_strip((Node *) linitial(op->args));
				Const	   *c = (Const *) (IsA(l, Const) ? l :
										   lion_strip((Node *) lsecond(op->args)));

				if (c->constisnull)
					leaf->hasnull = true;
				else if (c->consttype != dim->valtype)
					cx->failed = true;
				else
					leaf->vals = bms_add_member(leaf->vals,
												cells_value_no(cx, dim,
															   c->constvalue,
															   c->consttype));
			}
			break;
		case CL_IN:
		case CL_NOTIN:
			{
				Const	   *arr = (Const *) lion_strip((Node *) lsecond(((ScalarArrayOpExpr *) node)->args));
				ArrayType  *a = DatumGetArrayTypeP(arr->constvalue);
				int16		typlen;
				bool		typbyval;
				char		typalign;
				Datum	   *elems;
				bool	   *nulls;
				int			n;
				int			i;

				if (ARR_ELEMTYPE(a) != dim->valtype)
				{
					cx->failed = true;
					break;
				}
				get_typlenbyvalalign(ARR_ELEMTYPE(a), &typlen, &typbyval,
									 &typalign);
				deconstruct_array(a, ARR_ELEMTYPE(a), typlen, typbyval,
								  typalign, &elems, &nulls, &n);
				leaf->empty = (n == 0);
				for (i = 0; i < n && !cx->failed; i++)
				{
					if (nulls[i])
						leaf->hasnull = true;
					else
						leaf->vals = bms_add_member(leaf->vals,
													cells_value_no(cx, dim,
																   elems[i],
																   ARR_ELEMTYPE(a)));
				}
			}
			break;
		case CL_BOOL:
			leaf->vals = bms_make_singleton(cells_value_no(cx, dim,
														   BoolGetDatum(true),
														   BOOLOID));
			break;
		case CL_BOOLTEST:
			leaf->btype = ((BooleanTest *) node)->booltesttype;
			/* true is value 0 or the first named, false the other */
			leaf->vals = bms_make_singleton(cells_value_no(cx, dim,
														   BoolGetDatum(true),
														   BOOLOID));
			(void) cells_value_no(cx, dim, BoolGetDatum(false), BOOLOID);
			break;
		default:
			break;
	}
	dim->leaves = lappend(dim->leaves, leaf);
}

/* A predicate found: numbered once, by equal(), and its column noted */
static void
cells_add_pred(CellsCx *cx, Node *node, Var *var, bool multi,
			   const LionLeafInfo *leaf, bool none)
{
	CellPred   *p;
	CellDim    *dim;
	ListCell   *lc;
	int			d;

	foreach(lc, cx->preds)
	{
		if (equal(((CellPred *) lfirst(lc))->expr, node))
			return;
	}
	if (list_length(cx->preds) >= LION_CELLS_MAX_PREDS)
	{
		cx->failed = true;
		return;
	}
	d = cells_dim_of(cx, var);
	if (d < 0)
		return;
	dim = &cx->dims[d];

	p = (CellPred *) palloc0(sizeof(CellPred));
	p->expr = node;
	p->dim = d;
	p->kind = LION_CELLPRED_TABLE;
	p->bit = 0;
	if (multi)
	{
		dim->multi = true;
		if (none)
			p->kind = LION_CELLPRED_NONE;
		else
		{
			if (dim->nbit >= LION_CELLS_MAX_BITS)
			{
				cx->failed = true;
				return;
			}
			p->kind = LION_CELLPRED_BIT;
			p->bit = dim->nbit;
			dim->bitleaf[dim->nbit] = node;
			dim->bitinfo[dim->nbit] = *leaf;
			dim->nbit++;
		}
	}
	else
		cells_note_leaves(cx, dim, node);
	dim->preds = lappend_int(dim->preds, list_length(cx->preds));
	cx->preds = lappend(cx->preds, p);
}

/*
 * The walk of an expression: each predicate it is made of noted, and true -
 * the end of the walk - at anything that would make its value more than a
 * function of the predicates' truths: a column outside every predicate, a
 * subquery, an aggregate, a CASE with an argument (whose WHEN tests a
 * placeholder no predicate can stand for).
 */
static bool
cells_walker(Node *node, CellsCx *cx)
{
	Var		   *var;
	AttrNumber	attno;
	LionLeafInfo leaf;
	bool		none;

	if (node == NULL)
		return false;
	if (cx->failed)
		return true;

	if ((attno = cells_scalar_tree(cx, node)) != 0)
	{
		Var		   *v;
		int			kind;
		Oid			eqop;
		Node	   *first = node;

		/* the column, from the tree's first leaf */
		while (IsA(first, BoolExpr))
			first = (Node *) linitial(((BoolExpr *) first)->args);
		(void) cells_scalar_leaf(cx, first, &v, &kind, &eqop);
		cells_add_pred(cx, node, v, false, NULL, false);
		return cx->failed;
	}
	if (cells_multi_leaf(cx, node, &var, &leaf, &none))
	{
		cells_add_pred(cx, node, var, true, &leaf, none);
		return cx->failed;
	}

	switch (nodeTag(node))
	{
		case T_Var:
		case T_PlaceHolderVar:
		case T_Aggref:
		case T_WindowFunc:
		case T_GroupingFunc:
		case T_SubLink:
		case T_SubPlan:
		case T_AlternativeSubPlan:
		case T_CaseTestExpr:
		case T_NextValueExpr:
		case T_SetToDefault:
		case T_CurrentOfExpr:
			cx->failed = true;
			return true;
		case T_CaseExpr:
			if (((CaseExpr *) node)->arg != NULL)
			{
				cx->failed = true;
				return true;
			}
			break;
		default:
			break;
	}
	return expression_tree_walker(node, cells_walker, cx);
}

/*
 * May expr be counted by cells at all, by its shape alone: immutable, no
 * set-returning function, and a function of predicates on lion-indexed
 * columns of the relation, at least one?  What lion_count_path_where() asks
 * of a WHERE clause no posting set answers before it takes it as one over
 * cells; the indexes and the outcomes are lion_cells_analyze()'s.
 */
bool
lion_cells_shape_ok(PlannerInfo *root, RelOptInfo *rel, Index rti,
					Node *expr)
{
	CellsCx		cx;

	if (expr == NULL || contain_mutable_functions(expr) ||
		expression_returns_set(expr))
		return false;
	memset(&cx, 0, sizeof(cx));
	cx.root = root;
	cx.rel = rel;
	cx.rti = rti;
	return !cells_walker(expr, &cx) && !cx.failed && cx.preds != NIL;
}

/* `var IS NULL`, for its selectivity */
static Node *
cells_null_test(Var *var)
{
	NullTest   *nt = makeNode(NullTest);

	nt->arg = (Expr *) var;
	nt->nulltesttype = IS_NULL;
	nt->argisrow = false;
	nt->location = -1;
	return (Node *) nt;
}

/*
 * The truth of scalar tree node in value u of dimension dim: u < nval one of
 * its values, nval any other, nval + 1 NULL - in SQL's three-valued logic,
 * as the executor would compute it of a row with that value.
 */
static int
cells_eval(CellDim *dim, Node *node, int u)
{
	int			nval = list_length(dim->consts);
	bool		isnull = (u == nval + 1);
	bool		isval = (u < nval);
	CellLeaf   *leaf = NULL;
	ListCell   *lc;

	if (IsA(node, BoolExpr))
	{
		BoolExpr   *b = (BoolExpr *) node;
		bool		anynull = false;

		if (b->boolop == NOT_EXPR)
		{
			int			t = cells_eval(dim, (Node *) linitial(b->args), u);

			return (t == LION_TRUTH_NULL) ? t :
				(t == LION_TRUTH_TRUE) ? LION_TRUTH_FALSE : LION_TRUTH_TRUE;
		}
		foreach(lc, b->args)
		{
			int			t = cells_eval(dim, (Node *) lfirst(lc), u);

			if (b->boolop == AND_EXPR && t == LION_TRUTH_FALSE)
				return LION_TRUTH_FALSE;
			if (b->boolop == OR_EXPR && t == LION_TRUTH_TRUE)
				return LION_TRUTH_TRUE;
			if (t == LION_TRUTH_NULL)
				anynull = true;
		}
		if (anynull)
			return LION_TRUTH_NULL;
		return (b->boolop == AND_EXPR) ? LION_TRUTH_TRUE : LION_TRUTH_FALSE;
	}

	foreach(lc, dim->leaves)
	{
		if (((CellLeaf *) lfirst(lc))->node == node)
		{
			leaf = (CellLeaf *) lfirst(lc);
			break;
		}
	}
	if (leaf == NULL)
		elog(ERROR, "lion: a leaf of a cells predicate that was never noted");

#define TV(b)	((b) ? LION_TRUTH_TRUE : LION_TRUTH_FALSE)
	switch (leaf->kind)
	{
		case CL_EQ:
			return isnull ? LION_TRUTH_NULL :
				TV(isval && bms_is_member(u, leaf->vals));
		case CL_NE:
			return isnull ? LION_TRUTH_NULL :
				TV(!(isval && bms_is_member(u, leaf->vals)));
		case CL_IN:
			/* ANY of no element is false, of a NULL too (ExecEvalScalarArrayOp) */
			if (leaf->empty)
				return LION_TRUTH_FALSE;
			if (isnull)
				return LION_TRUTH_NULL;
			if (isval && bms_is_member(u, leaf->vals))
				return LION_TRUTH_TRUE;
			return leaf->hasnull ? LION_TRUTH_NULL : LION_TRUTH_FALSE;
		case CL_NOTIN:
			if (leaf->empty)
				return LION_TRUTH_TRUE;
			if (isnull)
				return LION_TRUTH_NULL;
			if (isval && bms_is_member(u, leaf->vals))
				return LION_TRUTH_FALSE;
			return leaf->hasnull ? LION_TRUTH_NULL : LION_TRUTH_TRUE;
		case CL_DISTINCT:
			if (leaf->hasnull)
				return TV(!isnull);
			return TV(isnull || !(isval && bms_is_member(u, leaf->vals)));
		case CL_ISNULL:
			return TV(isnull);
		case CL_NOTNULL:
			return TV(!isnull);
		case CL_BOOL:
			return isnull ? LION_TRUTH_NULL :
				TV(isval && bms_is_member(u, leaf->vals));
		case CL_BOOLTEST:
			{
				/* vals holds true's number; false is the other value */
				bool		istrue = isval && bms_is_member(u, leaf->vals);
				bool		isfalse = isval && !istrue;

				switch (leaf->btype)
				{
					case IS_TRUE:
						return TV(istrue);
					case IS_NOT_TRUE:
						return TV(!istrue);
					case IS_FALSE:
						return TV(isfalse);
					case IS_NOT_FALSE:
						return TV(!isfalse);
					case IS_UNKNOWN:
						return TV(isnull);
					case IS_NOT_UNKNOWN:
						return TV(!isnull);
				}
			}
			break;
	}
#undef TV
	elog(ERROR, "lion: unknown kind %d of a cells leaf", leaf->kind);
	return LION_TRUTH_NULL;		/* keep the compiler quiet */
}

/*
 * The index dimension dim's rows are told apart by, and its key column: for
 * a scalar column, the whole index whose equality is the values' - or any
 * scalar one, for null tests alone - whose key type the values are of; for
 * a multi-key one, the index every leaf's query is answered by, which keeps
 * the column's NULL rows in its NULL entry.
 */
static bool
cells_dim_index(CellsCx *cx, CellDim *dim, LionCellsDimPlan *dp)
{
	RelOptInfo *rel = cx->rel;
	AttrNumber	attno = dim->var->varattno;
	AttrNumber	col = 0;
	IndexOptInfo *idx;
	int			b;

	if (!dim->multi)
	{
		if (dim->hasvalop)
			idx = lion_match_index(rel, attno, LION_CLAUSE_EQ, dim->eqop,
								   dim->valtype, LION_STRAT_EQUAL,
								   InvalidOid, dim->coll, false, &col);
		else
		{
			idx = lion_match_index(rel, attno, LION_CLAUSE_NULL, InvalidOid,
								   InvalidOid, 0, InvalidOid, InvalidOid,
								   false, &col);

			/*
			 * lion_match_index() answers a null test from a multi-key
			 * column's NULL entry where no scalar column has it; that column
			 * is a multi-key dimension here, whose entries are keys, not the
			 * values a scalar one's driver reads.
			 */
			if (idx != NULL &&
				lion_opfamily_is_multikey(idx->opfamily[col - 1],
										  idx->opcintype[col - 1]))
				idx = NULL;
		}
		if (idx == NULL && !dim->hasvalop &&
			lion_find_roaring_index(rel, attno, true, &col) != NULL)
		{
			/*
			 * Null tests alone, of a column only a multi-key index covers:
			 * its NULL entry holds the NULL rows as a scalar one's does
			 * (DESIGN.md §17), and every other row is in its keys or one of
			 * its keyless entries.
			 */
			dim->multi = true;
		}
		else if (idx == NULL)
			return false;
	}
	if (!dim->multi)
	{
		/* the driver compares the entries' keys with the values */
		if (dim->hasvalop &&
			idx->opcintype[col - 1] != dim->valtype &&
			!IsPolymorphicType(idx->opcintype[col - 1]) &&
			!IsBinaryCoercible(idx->opcintype[col - 1], dim->valtype))
			return false;
		dp->idx = idx;
		dp->col = col;
		return true;
	}

	/* a multi-key column: null tests its only scalar leaves */
	if (list_length(dim->consts) > 0)
		return false;
	{
		ListCell   *lc;

		foreach(lc, dim->leaves)
		{
			int			k = ((CellLeaf *) lfirst(lc))->kind;

			if (k != CL_ISNULL && k != CL_NOTNULL)
				return false;
		}
	}
	idx = NULL;
	for (b = 0; b < dim->nbit; b++)
	{
		LionLeafInfo *li = &dim->bitinfo[b];
		AttrNumber	bcol;
		IndexOptInfo *bidx = lion_match_index(rel, attno, LION_CLAUSE_MULTI,
											  li->opno, li->cmptype,
											  li->strategy, li->extractquery,
											  li->collation, false, &bcol);

		if (bidx == NULL || (idx != NULL && (bidx != idx || bcol != col)))
			return false;
		idx = bidx;
		col = bcol;
	}
	if (idx == NULL)
		idx = lion_find_roaring_index(rel, attno, true, &col);
	if (idx == NULL || idx->indpred != NIL)
		return false;
	dp->idx = idx;
	dp->col = col;
	return true;
}

/* The share of the relation's rows clause selects, by the planner's own estimate */
static double
cells_sel(CellsCx *cx, Node *clause)
{
	return clause_selectivity(cx->root, clause, 0, JOIN_INNER, NULL);
}

/*
 * Every predicate of exprs (DESIGN.md §10, "Counts by cells"), each
 * dimension's outcomes and each predicate's truth in them, and the cells
 * they make; or NULL when exprs are not functions of predicates the posting
 * sets decide, or make more cells than pg_lion.count_cells_max.  The driver
 * is lion_cells_choose_driver()'s.
 */
LionCellsPlan *
lion_cells_analyze(PlannerInfo *root, RelOptInfo *rel, Index rti, List *exprs)
{
	CellsCx		cx;
	LionCellsPlan *cp;
	Bitmapset  *notnull = lion_notnullattnums(root, rel);
	ListCell   *lc;
	int			d;
	int			i;

	memset(&cx, 0, sizeof(cx));
	cx.root = root;
	cx.rel = rel;
	cx.rti = rti;
	foreach(lc, exprs)
	{
		Node	   *e = (Node *) lfirst(lc);

		if (contain_mutable_functions(e) || expression_returns_set(e) ||
			cells_walker(e, &cx) || cx.failed)
			return NULL;
	}
	if (cx.preds == NIL || cx.ndim == 0)
		return NULL;

	cp = (LionCellsPlan *) palloc0(sizeof(LionCellsPlan));
	cp->ndim = cx.ndim;
	cp->dims = (LionCellsDimPlan *) palloc0(sizeof(LionCellsDimPlan) * cx.ndim);
	cp->npred = list_length(cx.preds);
	cp->preds = (LionCellsPredPriv *)
		palloc0(sizeof(LionCellsPredPriv) * cp->npred);
	cp->ncells = 1.0;
	cp->drv = -1;
	foreach(lc, cx.preds)
		cp->predexprs = lappend(cp->predexprs, ((CellPred *) lfirst(lc))->expr);

	for (d = 0; d < cx.ndim; d++)
	{
		CellDim    *dim = &cx.dims[d];
		LionCellsDimPlan *dp = &cp->dims[d];
		LionCellsDimPriv *pv = &dp->priv;
		Node	   *nullclause;

		if (!cells_dim_index(&cx, dim, dp))
			return NULL;
		dp->var = dim->var;
		pv->attno = dim->var->varattno;
		pv->idxoid = dp->idx->indexoid;
		dp->entries = estimate_num_groups(root, list_make1(dim->var),
										  Max(rel->tuples, 1.0), NULL, NULL) +
			1.0;
		nullclause = cells_null_test(dim->var);
		dp->nullsel = cells_sel(&cx, nullclause);

		if (dim->multi)
		{
			int			b;

			double		keys = 0;

			pv->kind = LION_CELLDIM_MULTI;
			pv->nbit = dim->nbit;
			pv->radix = (1 << dim->nbit) + 1;
			pv->nullout = 1 << dim->nbit;
			for (b = 0; b < dim->nbit; b++)
			{
				OpExpr	   *op = (OpExpr *) dim->bitleaf[b];

				pv->bitop[b] = op->opno;
				pv->bitquery[b] = (Const *) copyObject(dim->bitinfo[b].val);
				dp->bitsel[b] = cells_sel(&cx, dim->bitleaf[b]);
				dp->bitkeys[b] = lion_multikey_nkeys(dp->idx, dp->col,
													 dim->bitleaf[b]);
				dp->bitclause[b] = dim->bitleaf[b];
				keys += dp->bitkeys[b];
			}
			/* each key is a bit of a row's mask (lion_count_cells()) */
			if (keys > LION_CELLS_MAX_KEYS)
				return NULL;
		}
		else
		{
			int			nval = list_length(dim->consts);
			int			nu = nval + 2;
			int			npd = list_length(dim->preds);
			int		   *out = (int *) palloc(sizeof(int) * nu);
			int		   *rep = (int *) palloc(sizeof(int) * nu);
			uint8	   *sig = (uint8 *) palloc(sizeof(uint8) *
												 (Size) Max(npd, 1) * nu);
			int			nout = 0;
			bool		noother;
			bool		nonull;
			int			u;
			int			k;

			pv->kind = LION_CELLDIM_SCALAR;
			pv->eqop = dim->hasvalop ? dim->eqop : InvalidOid;
			pv->collation = dim->hasvalop ? dim->coll : InvalidOid;

			/* each value's truths: its signature */
			for (u = 0; u < nu; u++)
			{
				k = 0;
				foreach(lc, dim->preds)
				{
					CellPred   *p = (CellPred *) list_nth(cx.preds,
														   lfirst_int(lc));

					sig[(Size) u * npd + k++] =
						(uint8) cells_eval(dim, p->expr, u);
				}
			}

			/*
			 * Any other value's outcome is 0, then the rest as they come.  A
			 * boolean column both of whose values are named has no other
			 * value: its outcome 0 is its first value's, which then needs no
			 * label either.  A column declared NOT NULL has no NULL, which
			 * is then outcome 0 too.
			 */
			noother = dim->hasvalop && dim->valtype == BOOLOID && nval == 2;
			nonull = bms_is_member(dim->var->varattno, notnull);
			rep[nout] = noother ? 0 : nval;
			out[nval] = nout++;
			if (noother)
				out[0] = 0;
			if (nonull)
				out[nval + 1] = 0;
			for (u = 0; u < nu; u++)
			{
				int			o;

				if (u == nval || (noother && u == 0) ||
					(nonull && u == nval + 1))
					continue;
				for (o = 0; o < nout; o++)
				{
					if (memcmp(&sig[(Size) u * npd],
							   &sig[(Size) rep[o] * npd], npd) == 0)
						break;
				}
				if (o == nout)
					rep[nout++] = u;
				out[u] = o;
			}
			pv->radix = nout;
			pv->nullout = out[nval + 1];
			pv->nval = 0;
			pv->vals = NIL;
			pv->valout = (int *) palloc(sizeof(int) * Max(nval, 1));
			dp->valsel = (double *) palloc(sizeof(double) * Max(nval, 1));
			for (u = 0; u < nval; u++)
			{
				Const	   *c = (Const *) list_nth(dim->consts, u);

				if (out[u] == 0)
					continue;
				pv->vals = lappend(pv->vals, c);
				pv->valout[pv->nval] = out[u];
				dp->valsel[pv->nval] =
					cells_sel(&cx, (Node *) make_opclause(dim->eqop, BOOLOID,
														  false,
														  (Expr *) dim->var,
														  (Expr *) c,
														  InvalidOid,
														  dim->coll));
				pv->nval++;
			}

			/* each predicate's truth in each outcome */
			k = 0;
			foreach(lc, dim->preds)
			{
				int			pno = lfirst_int(lc);
				LionCellsPredPriv *pp = &cp->preds[pno];
				int			o;

				pp->dim = d;
				pp->kind = LION_CELLPRED_TABLE;
				pp->truth = (uint8 *) palloc(sizeof(uint8) * nout);
				for (o = 0; o < nout; o++)
					pp->truth[o] = sig[(Size) rep[o] * npd + k];
				k++;
			}
		}

		/* a multi-key column's predicates */
		if (dim->multi)
		{
			int			nval = list_length(dim->consts);

			Assert(nval == 0);
			foreach(lc, dim->preds)
			{
				int			pno = lfirst_int(lc);
				CellPred   *p = (CellPred *) list_nth(cx.preds, pno);
				LionCellsPredPriv *pp = &cp->preds[pno];

				pp->dim = d;
				pp->kind = p->kind;
				pp->bit = p->bit;
				if (p->kind == LION_CELLPRED_TABLE)
				{
					/* null tests: their truth of a value, and of NULL */
					pp->kind = LION_CELLPRED_NULLS;
					pp->truth = (uint8 *) palloc(2);
					pp->truth[0] = (uint8) cells_eval(dim, p->expr, nval);
					pp->truth[1] = (uint8) cells_eval(dim, p->expr, nval + 1);
				}
			}
		}
		cp->ncells *= (double) pv->radix;
	}

	for (i = 0; i < cp->npred; i++)
		Assert(cp->preds[i].kind != LION_CELLPRED_TABLE ||
			   cp->preds[i].truth != NULL);

	if (cp->ncells > (double) lion_count_cells_max)
		return NULL;
	return cp;
}

/*
 * The cells of the WHERE's rows that hold a row, as the planner estimates
 * them: each cell's share of the rows is the product of its outcomes'
 * shares - a value's selectivity, NULL's, the patterns of a multi-key
 * column's bits taken as independent - and a cell of share p holds one of
 * `rows` rows with probability 1 - exp(-rows * p).  The cells are not
 * enumerated: each dimension's outcomes, and the products so far, are kept
 * as a histogram over log2 of their share (LION_CELLS_SHARE_BINS bins of
 * half a power of two), each bin its cells and the sum of their shares,
 * which multiplies out bin by bin.  At most the rows.
 */
#define LION_CELLS_SHARE_BINS	129

typedef struct CellsShareHist
{
	double		n[LION_CELLS_SHARE_BINS];	/* cells */
	double		sum[LION_CELLS_SHARE_BINS];	/* ... and their shares */
} CellsShareHist;

static int
cells_share_bin(double p)
{
	int			b = (int) floor(-2.0 * log2(p));

	return Max(0, Min(b, LION_CELLS_SHARE_BINS - 1));
}

static void
cells_share_add(CellsShareHist *h, double n, double sum)
{
	int			b;

	if (n <= 0 || sum <= 0)
		return;
	b = cells_share_bin(sum / n);
	h->n[b] += n;
	h->sum[b] += sum;
}

double
lion_cells_nonempty(const LionCellsPlan *cp, double rows)
{
	CellsShareHist *acc;
	CellsShareHist *dimh;
	CellsShareHist *next;
	double		cells = 0;
	int			d;
	int			i;
	int			j;

	if (rows <= 0)
		return 0;
	acc = (CellsShareHist *) palloc0(sizeof(CellsShareHist));
	dimh = (CellsShareHist *) palloc(sizeof(CellsShareHist));
	next = (CellsShareHist *) palloc(sizeof(CellsShareHist));
	cells_share_add(acc, 1.0, 1.0);

	for (d = 0; d < cp->ndim; d++)
	{
		const LionCellsDimPlan *dp = &cp->dims[d];
		const LionCellsDimPriv *pv = &dp->priv;
		double	   *share = (double *) palloc0(sizeof(double) * pv->radix);
		int			o;

		if (pv->kind == LION_CELLDIM_MULTI)
		{
			double		nonnull = Max(1.0 - dp->nullsel, 0.0);
			int			m;

			for (m = 0; m < (1 << pv->nbit); m++)
			{
				double		p = nonnull;
				int			b;

				for (b = 0; b < pv->nbit; b++)
				{
					double		q = (nonnull > 0) ?
						Min(dp->bitsel[b] / nonnull, 1.0) : 0;

					p *= (m & (1 << b)) ? q : 1.0 - q;
				}
				share[m] = p;
			}
			share[pv->nullout] = dp->nullsel;
		}
		else
		{
			double		rest = 1.0;

			for (i = 0; i < pv->nval; i++)
				share[pv->valout[i]] += dp->valsel[i];
			if (pv->nullout != 0)
				share[pv->nullout] += dp->nullsel;
			for (o = 1; o < pv->radix; o++)
				rest -= share[o];
			share[0] = Max(rest, 0.0);
		}

		memset(dimh, 0, sizeof(CellsShareHist));
		for (o = 0; o < pv->radix; o++)
			cells_share_add(dimh, 1.0, Min(share[o], 1.0));
		pfree(share);

		memset(next, 0, sizeof(CellsShareHist));
		for (i = 0; i < LION_CELLS_SHARE_BINS; i++)
		{
			if (acc->n[i] <= 0)
				continue;
			for (j = 0; j < LION_CELLS_SHARE_BINS; j++)
			{
				if (dimh->n[j] <= 0)
					continue;
				cells_share_add(next, acc->n[i] * dimh->n[j],
								acc->sum[i] * dimh->sum[j]);
			}
		}
		memcpy(acc, next, sizeof(CellsShareHist));
	}

	for (i = 0; i < LION_CELLS_SHARE_BINS; i++)
	{
		if (acc->n[i] > 0)
			cells += acc->n[i] *
				(1.0 - exp(-rows * acc->sum[i] / acc->n[i]));
	}
	pfree(acc);
	pfree(dimh);
	pfree(next);
	return Min(Min(cells, cp->ncells), rows);
}

/*
 * The most passes a driver may make (DESIGN.md §10, "Counts by cells"):
 * each reads every other dimension's labels again, at the keys its entries
 * have rows at.  And the most comparisons of its entries' keys with its
 * dimension's values that mapping each entry to its outcome may take.
 */
#define LION_CELLS_MAX_PASSES		16
#define LION_CELLS_MAX_COMPARES		65536.0

/* The rows a dimension's labels hold, by the planner's estimates */
static double
cells_label_rows(const LionCellsDimPlan *dp, double tuples)
{
	const LionCellsDimPriv *pv = &dp->priv;
	double		sel = 0;
	int			i;

	if (pv->kind == LION_CELLDIM_MULTI)
	{
		for (i = 0; i < pv->nbit; i++)
			sel += dp->bitsel[i];
		sel += dp->nullsel;
	}
	else
	{
		for (i = 0; i < pv->nval; i++)
			sel += dp->valsel[i];
		if (pv->nullout != 0)
			sel += dp->nullsel;
	}
	return Min(sel, (pv->kind == LION_CELLDIM_MULTI) ? (double) pv->nbit + 1 :
			   1.0) * tuples;
}

/*
 * The column whose entries drive the count by cells: every row of the table
 * is in one entry of a scalar column, NULL's included, so its entries -
 * walked a pass of LION_GROUP_BATCH_MAX at a time, each located under its
 * pins (DESIGN.md §9) - are every row the WHERE may keep.  A scalar
 * dimension's column labels its own rows as it drives, by comparing each
 * entry's key with the dimension's values, and needs no labels of its own;
 * any other dimension's labels are read again by every pass.  So the driver
 * is the scalar dimension that leaves the fewest labels' rows to read times
 * its passes - or, where none will do, `fallback`, a lion-indexed column of
 * the table no dimension is (lion_count_all_driver()), which labels nothing.
 * False when neither will: too many entries, or none at all.
 */
bool
lion_cells_choose_driver(PlannerInfo *root, RelOptInfo *rel,
						 LionCellsPlan *cp, Var *fallback,
						 double fallbackentries)
{
	double		tuples = Max(rel->tuples, 1.0);
	double		total = 0;
	double	   *rows = (double *) palloc(sizeof(double) * cp->ndim);
	double		bestwork = -1.0;
	int			best = -2;
	int			d;

	for (d = 0; d < cp->ndim; d++)
	{
		rows[d] = cells_label_rows(&cp->dims[d], tuples);
		total += rows[d];
	}
	for (d = 0; d < cp->ndim; d++)
	{
		LionCellsDimPlan *dp = &cp->dims[d];
		double		passes = ceil(dp->entries / LION_GROUP_BATCH_MAX);
		double		work;

		if (dp->priv.kind != LION_CELLDIM_SCALAR ||
			passes > LION_CELLS_MAX_PASSES ||
			dp->entries * dp->priv.nval > LION_CELLS_MAX_COMPARES)
			continue;
		work = Max(passes, 1.0) * (total - rows[d]) +
			dp->entries * dp->priv.nval;
		if (bestwork < 0 || work < bestwork)
		{
			best = d;
			bestwork = work;
		}
	}
	if (fallback != NULL)
	{
		double		passes = ceil(fallbackentries / LION_GROUP_BATCH_MAX);
		bool		isdim = false;

		for (d = 0; d < cp->ndim; d++)
			isdim |= (cp->dims[d].var->varattno == fallback->varattno);
		if (!isdim && passes <= LION_CELLS_MAX_PASSES &&
			(bestwork < 0 || Max(passes, 1.0) * total < bestwork))
		{
			best = -1;
			bestwork = Max(passes, 1.0) * total;
		}
	}
	pfree(rows);
	if (best == -2)
		return false;
	cp->drv = best;
	cp->drvvar = (best >= 0) ? cp->dims[best].var : fallback;
	cp->drventries = (best >= 0) ? cp->dims[best].entries : fallbackentries;
	return true;
}

/*
 * The plan's CELLS member (lion_plan_private.h) from cp, with the WHERE
 * clauses over the predicates, cellquals, which the plan's qual tests in
 * each cell.
 */
void
lion_cells_fill_priv(const LionCellsPlan *cp, LionCountPriv *p,
					 List *cellquals)
{
	int			d;

	p->ncelldim = cp->ndim;
	p->celldim = (LionCellsDimPriv *)
		palloc(sizeof(LionCellsDimPriv) * cp->ndim);
	for (d = 0; d < cp->ndim; d++)
	{
		p->celldim[d] = cp->dims[d].priv;
		p->celldim[d].idxoid = cp->dims[d].idx->indexoid;
	}
	p->ncellpred = cp->npred;
	p->cellpred = (LionCellsPredPriv *)
		palloc(sizeof(LionCellsPredPriv) * cp->npred);
	memcpy(p->cellpred, cp->preds, sizeof(LionCellsPredPriv) * cp->npred);
	p->celldrv = cp->drv;
	p->cellexprs = (List *) copyObject(cp->predexprs);
	p->cellquals = (List *) copyObject(cellquals);
}
