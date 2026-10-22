/*-------------------------------------------------------------------------
 *
 * lion_plan_nonempty.c
 *	  Whether an array is empty, answered from a lion array column's reserved
 *	  entries (DESIGN.md §17, "Non-empty arrays").
 *
 *	  Every row of a column that lion's array_ops indexes is in exactly one of
 *	  five classes, and the column's entries tell them apart: the column is
 *	  NULL (the NULL entry of §14); it is an array with no element and no
 *	  dimension (the EMPTY entry), one with no element and a first dimension
 *	  of length 0 (EMPTYDIM), or one with no element and a first dimension
 *	  of length 1 or more and an inner one of 0 (EMPTYINNER); or it has an
 *	  element - the entries of its non-NULL elements, or the NULLELEMS entry
 *	  when every element is NULL.  A predicate whose truth is the same for
 *	  every row of a class is a union of whole classes, and so of reserved
 *	  entries or of their complement: `cardinality(tags) > 0` is every row
 *	  outside the NULL and the three empty entries, `cardinality(tags) = 0`
 *	  the rows of the three empty entries, `tags = '{}'` those of EMPTY
 *	  alone.  This file decides which predicates are of that kind, by
 *	  evaluating each of them over the five classes, and gives the count
 *	  pushdown the clause it then is (lion_nonempty_leaf()) and the estimates
 *	  the clause core estimates best for the same rows (lion_nonempty_clause(),
 *	  lion_nonempty_rel_clauses()).
 *
 *	  What a class makes of a predicate follows from PostgreSQL's arrays.  An
 *	  array with no element has no dimension as PostgreSQL makes one nearly
 *	  everywhere - '{}', '{{}}', array_fill(1, ARRAY[0]), an empty slice and
 *	  the functions that remove elements all come out as the one empty array
 *	  - but an empty int2vector or oidvector has one dimension of length 0,
 *	  `[0:-1]`, which its cast to int2[] or oid[] and array_cat() keep, and
 *	  ARRAY[] of such arrays has two, `[1:2][0:-1]`.  Any other array has one
 *	  to six dimensions of one element or more, whatever its lower bounds and
 *	  whether or not its elements are NULL.  So array_length(a, 1) is NULL,
 *	  NULL, 0, at least 1 and at least 1 over the five classes;
 *	  cardinality(a) NULL, 0, 0, 0 and at least 1; array_ndims(a) NULL,
 *	  NULL, 1 to 6, 2 to 6 and 1 to 6; array_lower(a, 1) and array_upper(a,
 *	  1) NULL, NULL and some integer; and `a = '{}'` - array_eq(), which
 *	  compares the dimensions first - NULL, true, false, false and false.  A
 *	  dimension other than the first, a bound that is not a constant, an empty
 *	  constant with dimensions of its own, or a comparison that one row of a
 *	  class passes and another fails (`array_length(a, 1) > 1`) is left to the
 *	  ordinary plan.
 *
 * IDENTIFICATION
 *	  src/lion_plan_nonempty.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/stratnum.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "utils/array.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"

#include "lion_customscan.h"

/* The five classes of row, as indexes into the arrays below. */
#define LION_NE_NULL_ROWS	0	/* the column is NULL */
#define LION_NE_EMPTY_ROWS	1	/* no element, no dimension */
#define LION_NE_DIM_ROWS	2	/* no element, a first dimension of 0 */
#define LION_NE_INNER_ROWS	3	/* no element, a first dimension of 1 or
								 * more */
#define LION_NE_SOME_ROWS	4	/* an array with one element or more */
#define LION_NE_NCLASS		5

/* The reserved entry of each class, 0 for the rows under keys */
static const uint16 lion_ne_entry[LION_NE_NCLASS] = {
	LION_ENTRY_NULLKEY, LION_ENTRY_EMPTYKEY, LION_ENTRY_EMPTYDIM,
	LION_ENTRY_EMPTYINNER, 0
};

/* What a predicate is over the rows of one class. */
typedef enum LionNETruth
{
	LION_NE_FALSE,
	LION_NE_TRUE,
	LION_NE_UNKNOWN,			/* NULL, for every row of the class */
	LION_NE_MIXED				/* not the same for every row of the class */
} LionNETruth;

/*
 * An integer a function of the array computes, over the rows of each class:
 * NULL for all of them, or a value in [lo, hi] for each.
 */
typedef struct LionNEInt
{
	bool		isnull[LION_NE_NCLASS];
	int64		lo[LION_NE_NCLASS];
	int64		hi[LION_NE_NCLASS];
} LionNEInt;

/*
 * The array column `node` is, relabels stripped: a Var of an array type, or
 * of a domain over one.  NULL for anything else.
 */
static Var *
lion_ne_array_var(Node *node)
{
	node = lion_strip(node);
	if (node == NULL || !IsA(node, Var))
		return NULL;
	if (!type_is_array(getBaseType(((Var *) node)->vartype)))
		return NULL;
	return (Var *) node;
}

/*
 * Is v the column the predicate is about, or the first column it names?  A
 * predicate about two columns is no predicate on one.
 */
static bool
lion_ne_same_var(Var **var, Var *v)
{
	if (v == NULL)
		return false;
	if (*var == NULL)
	{
		*var = v;
		return true;
	}
	return (*var)->varno == v->varno && (*var)->varattno == v->varattno &&
		(*var)->varlevelsup == v->varlevelsup;
}

/* The value of a non-NULL integer constant, int2, int4 or int8. */
static bool
lion_ne_int_const(Node *node, int64 *val)
{
	Const	   *c;

	node = lion_strip(node);
	if (node == NULL || !IsA(node, Const))
		return false;
	c = (Const *) node;
	if (c->constisnull)
		return false;
	switch (c->consttype)
	{
		case INT2OID:
			*val = (int64) DatumGetInt16(c->constvalue);
			return true;
		case INT4OID:
			*val = (int64) DatumGetInt32(c->constvalue);
			return true;
		case INT8OID:
			*val = DatumGetInt64(c->constvalue);
			return true;
		default:
			return false;
	}
}

/*
 * Is node a non-NULL constant array with no element and no dimension, '{}'?
 * An empty constant with a dimension, `''::int2vector::int2[]`, is not: a
 * row of EMPTYDIM is equal to it or not by its lower bound, and one of
 * EMPTYINNER by its dimensions (array_eq()).
 */
static bool
lion_ne_empty_const(Node *node)
{
	Const	   *c;

	node = lion_strip(node);
	if (node == NULL || !IsA(node, Const))
		return false;
	c = (Const *) node;
	if (c->constisnull || !type_is_array(getBaseType(c->consttype)))
		return false;
	return lion_array_empty_flag(c->constvalue) == LION_ENTRY_EMPTYKEY;
}

/* One class: NULL, or every value in [lo, hi]. */
static void
lion_ne_set(LionNEInt *v, int cls, bool isnull, int64 lo, int64 hi)
{
	v->isnull[cls] = isnull;
	v->lo[cls] = lo;
	v->hi[cls] = hi;
}

/*
 * The integer a function of one array column computes over each class
 * (above), with the column in *var: array_length(a, 1), cardinality(a),
 * array_ndims(a), array_lower(a, 1) and array_upper(a, 1), or coalesce() of
 * one of them and an integer constant.  False for anything else, a dimension
 * other than the constant 1 included.  The function is looked at before the
 * column, whose type takes a catalog lookup.
 */
static bool
lion_ne_int_value(Node *node, Var **var, LionNEInt *out)
{
	node = lion_strip(node);
	if (node == NULL)
		return false;

	if (IsA(node, FuncExpr))
	{
		FuncExpr   *fe = (FuncExpr *) node;
		int			nargs = list_length(fe->args);
		int64		dim;

		switch (fe->funcid)
		{
			case F_ARRAY_LENGTH:
			case F_ARRAY_LOWER:
			case F_ARRAY_UPPER:
				if (nargs != 2 ||
					!lion_ne_int_const((Node *) lsecond(fe->args), &dim) ||
					dim != 1)
					return false;
				break;
			case F_CARDINALITY:
			case F_ARRAY_NDIMS:
				if (nargs != 1)
					return false;
				break;
			default:
				return false;
		}
		if (fe->funcretset ||
			!lion_ne_same_var(var,
							  lion_ne_array_var((Node *) linitial(fe->args))))
			return false;

		lion_ne_set(out, LION_NE_NULL_ROWS, true, 0, 0);
		switch (fe->funcid)
		{
			case F_ARRAY_LENGTH:
				lion_ne_set(out, LION_NE_EMPTY_ROWS, true, 0, 0);
				lion_ne_set(out, LION_NE_DIM_ROWS, false, 0, 0);
				lion_ne_set(out, LION_NE_INNER_ROWS, false, 1, PG_INT32_MAX);
				lion_ne_set(out, LION_NE_SOME_ROWS, false, 1, PG_INT32_MAX);
				break;
			case F_ARRAY_LOWER:
			case F_ARRAY_UPPER:
				lion_ne_set(out, LION_NE_EMPTY_ROWS, true, 0, 0);
				lion_ne_set(out, LION_NE_DIM_ROWS, false, PG_INT32_MIN,
							PG_INT32_MAX);
				lion_ne_set(out, LION_NE_INNER_ROWS, false, PG_INT32_MIN,
							PG_INT32_MAX);
				lion_ne_set(out, LION_NE_SOME_ROWS, false, PG_INT32_MIN,
							PG_INT32_MAX);
				break;
			case F_CARDINALITY:
				lion_ne_set(out, LION_NE_EMPTY_ROWS, false, 0, 0);
				lion_ne_set(out, LION_NE_DIM_ROWS, false, 0, 0);
				lion_ne_set(out, LION_NE_INNER_ROWS, false, 0, 0);
				lion_ne_set(out, LION_NE_SOME_ROWS, false, 1, PG_INT32_MAX);
				break;
			default:
				Assert(fe->funcid == F_ARRAY_NDIMS);
				lion_ne_set(out, LION_NE_EMPTY_ROWS, true, 0, 0);
				lion_ne_set(out, LION_NE_DIM_ROWS, false, 1, MAXDIM);
				lion_ne_set(out, LION_NE_INNER_ROWS, false, 2, MAXDIM);
				lion_ne_set(out, LION_NE_SOME_ROWS, false, 1, MAXDIM);
				break;
		}
		return true;
	}

	if (IsA(node, CoalesceExpr))
	{
		CoalesceExpr *ce = (CoalesceExpr *) node;
		int64		dflt;
		int			cls;

		if (list_length(ce->args) != 2 ||
			!lion_ne_int_const((Node *) lsecond(ce->args), &dflt) ||
			!lion_ne_int_value((Node *) linitial(ce->args), var, out))
			return false;
		for (cls = 0; cls < LION_NE_NCLASS; cls++)
		{
			if (out->isnull[cls])
				lion_ne_set(out, cls, false, dflt, dflt);
		}
		return true;
	}

	return false;
}

/*
 * `v <strategy> c` - a btree strategy, or `<>` - over every value of
 * [lo, hi]: true of all of them, of none, or MIXED.
 */
static LionNETruth
lion_ne_compare(int64 lo, int64 hi, int strategy, bool ne, int64 c)
{
	bool		all;
	bool		none;

	switch (strategy)
	{
		case BTLessStrategyNumber:
			all = (hi < c);
			none = (lo >= c);
			break;
		case BTLessEqualStrategyNumber:
			all = (hi <= c);
			none = (lo > c);
			break;
		case BTEqualStrategyNumber:
			all = (lo == c && hi == c);
			none = (c < lo || c > hi);
			break;
		case BTGreaterEqualStrategyNumber:
			all = (lo >= c);
			none = (hi < c);
			break;
		case BTGreaterStrategyNumber:
			all = (lo > c);
			none = (hi <= c);
			break;
		default:
			return LION_NE_MIXED;
	}
	if (all)
		return ne ? LION_NE_FALSE : LION_NE_TRUE;
	if (none)
		return ne ? LION_NE_TRUE : LION_NE_FALSE;
	return LION_NE_MIXED;
}

/* The btree strategy of integer comparison opno, commuted: `c < v` is `v > c`. */
static int
lion_ne_commute(int strategy)
{
	switch (strategy)
	{
		case BTLessStrategyNumber:
			return BTGreaterStrategyNumber;
		case BTLessEqualStrategyNumber:
			return BTGreaterEqualStrategyNumber;
		case BTGreaterEqualStrategyNumber:
			return BTLessEqualStrategyNumber;
		case BTGreaterStrategyNumber:
			return BTLessStrategyNumber;
		default:
			return strategy;
	}
}

/*
 * The integer comparison opno is - its btree strategy in the integer family,
 * with *ne set for `<>`, the negator of its `=` - or 0.
 */
static int
lion_ne_int_strategy(Oid opno, bool *ne)
{
	int			strategy;
	Oid			neg;

	*ne = false;
	strategy = get_op_opfamily_strategy(opno, INTEGER_BTREE_FAM_OID);
	if (strategy != 0)
		return strategy;
	neg = get_negator(opno);
	if (OidIsValid(neg) &&
		get_op_opfamily_strategy(neg, INTEGER_BTREE_FAM_OID) ==
		BTEqualStrategyNumber)
	{
		*ne = true;
		return BTEqualStrategyNumber;
	}
	return 0;
}

/*
 * A comparison of one array column's integer with a constant, `v op c` or
 * `c op v` (distinct: `IS DISTINCT FROM`, whose operator is the `=`), over
 * each class.  False when it is not one.  Its shape is looked at before the
 * operator, which takes catalog lookups: every comparison of a WHERE comes
 * here (lion_nonempty_leaf(), lion_nonempty_rel_clauses()), and nearly none
 * is of an array's integer.
 */
static bool
lion_ne_int_compare(Oid opno, List *args, bool distinct, Var **var,
					LionNETruth *t)
{
	LionNEInt	v;
	int64		c;
	int			strategy;
	bool		ne;
	bool		commuted;
	int			cls;

	if (list_length(args) != 2)
		return false;
	if (lion_ne_int_const((Node *) linitial(args), &c))
	{
		/* `c op v` is `v op' c`, op' the commuted strategy */
		if (!lion_ne_int_value((Node *) lsecond(args), var, &v))
			return false;
		commuted = true;
	}
	else if (lion_ne_int_const((Node *) lsecond(args), &c) &&
			 lion_ne_int_value((Node *) linitial(args), var, &v))
		commuted = false;
	else
		return false;

	if (!op_strict(opno))
		return false;
	strategy = lion_ne_int_strategy(opno, &ne);
	if (strategy == 0 || (distinct && (ne || strategy != BTEqualStrategyNumber)))
		return false;
	if (commuted)
		strategy = lion_ne_commute(strategy);

	for (cls = 0; cls < LION_NE_NCLASS; cls++)
	{
		if (v.isnull[cls])
			t[cls] = distinct ? LION_NE_TRUE : LION_NE_UNKNOWN;
		else
			t[cls] = lion_ne_compare(v.lo[cls], v.hi[cls], strategy,
									 ne || distinct, c);
	}
	return true;
}

/*
 * `a = '{}'`, `a <> '{}'` or `a IS DISTINCT FROM '{}'` (distinct, with the
 * `=`) - array_eq() and array_ne() themselves, the column on either side -
 * over each class.  array_eq() compares the dimensions before any element,
 * so it calls every array with one unequal to '{}', empty or not, and the
 * one with none equal, whatever its element type compares like.
 */
static bool
lion_ne_array_compare(Oid opfuncid, List *args, bool distinct, Var **var,
					  LionNETruth *t)
{
	Var		   *v;
	bool		eq;
	int			cls;

	if (list_length(args) != 2)
		return false;
	if (opfuncid == F_ARRAY_EQ)
		eq = true;
	else if (opfuncid == F_ARRAY_NE && !distinct)
		eq = false;
	else
		return false;
	if (lion_ne_empty_const((Node *) lsecond(args)))
		v = lion_ne_array_var((Node *) linitial(args));
	else if (lion_ne_empty_const((Node *) linitial(args)))
		v = lion_ne_array_var((Node *) lsecond(args));
	else
		return false;
	if (!lion_ne_same_var(var, v))
		return false;

	for (cls = 0; cls < LION_NE_NCLASS; cls++)
	{
		bool		equal = (cls == LION_NE_EMPTY_ROWS);

		if (cls == LION_NE_NULL_ROWS)
			t[cls] = distinct ? LION_NE_TRUE : LION_NE_UNKNOWN;
		else if (distinct)
			t[cls] = equal ? LION_NE_FALSE : LION_NE_TRUE;
		else
			t[cls] = (equal == eq) ? LION_NE_TRUE : LION_NE_FALSE;
	}
	return true;
}

/*
 * What clause is over each class of the rows of one array column, which it
 * puts in *var: TRUE, FALSE or UNKNOWN for every row of the class, or MIXED.
 * False when it is no predicate of the kind at all.  The shapes, nested
 * through NOT and the boolean tests: an integer comparison of
 * lion_ne_int_value()'s with a constant, `IS NULL` and `IS NOT NULL` of one
 * or of the column itself, and the column compared with '{}'.
 */
static bool
lion_ne_truth(Node *clause, Var **var, LionNETruth *t)
{
	int			cls;

	check_stack_depth();

	if (clause == NULL)
		return false;

	if (IsA(clause, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) clause;

		set_opfuncid(op);
		if (op->opfuncid == F_ARRAY_EQ || op->opfuncid == F_ARRAY_NE)
			return lion_ne_array_compare(op->opfuncid, op->args, false, var, t);
		return lion_ne_int_compare(op->opno, op->args, false, var, t);
	}

	if (IsA(clause, DistinctExpr))
	{
		DistinctExpr *de = (DistinctExpr *) clause;

		set_opfuncid((OpExpr *) de);
		if (de->opfuncid == F_ARRAY_EQ)
			return lion_ne_array_compare(de->opfuncid, de->args, true, var, t);
		return lion_ne_int_compare(de->opno, de->args, true, var, t);
	}

	if (IsA(clause, NullTest))
	{
		NullTest   *nt = (NullTest *) clause;
		bool		isnull = (nt->nulltesttype == IS_NULL);
		Var		   *v;
		LionNEInt	iv;

		if (nt->argisrow)
			return false;
		v = lion_ne_array_var((Node *) nt->arg);
		if (v != NULL)
		{
			if (!lion_ne_same_var(var, v))
				return false;
			for (cls = 0; cls < LION_NE_NCLASS; cls++)
				t[cls] = ((cls == LION_NE_NULL_ROWS) == isnull) ?
					LION_NE_TRUE : LION_NE_FALSE;
			return true;
		}
		if (!lion_ne_int_value((Node *) nt->arg, var, &iv))
			return false;
		for (cls = 0; cls < LION_NE_NCLASS; cls++)
			t[cls] = (iv.isnull[cls] == isnull) ? LION_NE_TRUE : LION_NE_FALSE;
		return true;
	}

	if (IsA(clause, BooleanTest))
	{
		BooleanTest *bt = (BooleanTest *) clause;
		LionNETruth sub[LION_NE_NCLASS];

		if (!lion_ne_truth((Node *) bt->arg, var, sub))
			return false;
		for (cls = 0; cls < LION_NE_NCLASS; cls++)
		{
			bool		r;

			if (sub[cls] == LION_NE_MIXED)
			{
				t[cls] = LION_NE_MIXED;
				continue;
			}
			switch (bt->booltesttype)
			{
				case IS_TRUE:
					r = (sub[cls] == LION_NE_TRUE);
					break;
				case IS_NOT_TRUE:
					r = (sub[cls] != LION_NE_TRUE);
					break;
				case IS_FALSE:
					r = (sub[cls] == LION_NE_FALSE);
					break;
				case IS_NOT_FALSE:
					r = (sub[cls] != LION_NE_FALSE);
					break;
				case IS_UNKNOWN:
					r = (sub[cls] == LION_NE_UNKNOWN);
					break;
				case IS_NOT_UNKNOWN:
					r = (sub[cls] != LION_NE_UNKNOWN);
					break;
				default:
					return false;
			}
			t[cls] = r ? LION_NE_TRUE : LION_NE_FALSE;
		}
		return true;
	}

	if (IsA(clause, BoolExpr) && ((BoolExpr *) clause)->boolop == NOT_EXPR &&
		list_length(((BoolExpr *) clause)->args) == 1)
	{
		LionNETruth sub[LION_NE_NCLASS];

		if (!lion_ne_truth((Node *) linitial(((BoolExpr *) clause)->args),
						   var, sub))
			return false;
		for (cls = 0; cls < LION_NE_NCLASS; cls++)
		{
			switch (sub[cls])
			{
				case LION_NE_TRUE:
					t[cls] = LION_NE_FALSE;
					break;
				case LION_NE_FALSE:
					t[cls] = LION_NE_TRUE;
					break;
				default:
					t[cls] = sub[cls];
					break;
			}
		}
		return true;
	}

	return false;
}

/*
 * Could node be one of lion_ne_int_value()'s integers, by its shape alone:
 * one of its functions, or coalesce() of one?
 */
static bool
lion_ne_int_shape(Node *node)
{
	node = lion_strip(node);
	if (node == NULL)
		return false;
	if (IsA(node, CoalesceExpr))
		return list_length(((CoalesceExpr *) node)->args) == 2 &&
			lion_ne_int_shape((Node *) linitial(((CoalesceExpr *) node)->args));
	if (!IsA(node, FuncExpr))
		return false;
	switch (((FuncExpr *) node)->funcid)
	{
		case F_ARRAY_LENGTH:
		case F_ARRAY_LOWER:
		case F_ARRAY_UPPER:
		case F_CARDINALITY:
		case F_ARRAY_NDIMS:
			return true;
		default:
			return false;
	}
}

/*
 * Could clause - bare, not a RestrictInfo - be one lion_ne_truth() takes?
 * Its shape alone, with no catalog lookup: 2 when it may be, 1 for a null
 * test of a column, which changes no estimate but beside another test of
 * that column (lion_nonempty_rel_clauses()), and 0 when it cannot be.
 * Every clause of every query on a relation lion indexes is asked, at least
 * once for its estimates and once more when a count over it is planned, and
 * nearly none is a test of whether an array is empty.
 */
static int
lion_ne_shape(Node *clause)
{
	for (;;)
	{
		if (clause == NULL)
			return 0;
		if (IsA(clause, BoolExpr) &&
			((BoolExpr *) clause)->boolop == NOT_EXPR &&
			list_length(((BoolExpr *) clause)->args) == 1)
			clause = (Node *) linitial(((BoolExpr *) clause)->args);
		else if (IsA(clause, BooleanTest))
			clause = (Node *) ((BooleanTest *) clause)->arg;
		else
			break;
	}

	if (IsA(clause, NullTest))
	{
		Node	   *arg = lion_strip((Node *) ((NullTest *) clause)->arg);

		if (arg != NULL && IsA(arg, Var))
			return 1;
		return lion_ne_int_shape(arg) ? 2 : 0;
	}
	if (IsA(clause, OpExpr) || IsA(clause, DistinctExpr))
	{
		OpExpr	   *op = (OpExpr *) clause;

		if (list_length(op->args) != 2)
			return 0;
		if (op->opfuncid == F_ARRAY_EQ || op->opfuncid == F_ARRAY_NE ||
			!OidIsValid(op->opfuncid))
			return 2;
		if (lion_ne_int_shape((Node *) linitial(op->args)) ||
			lion_ne_int_shape((Node *) lsecond(op->args)))
			return 2;
	}
	return 0;
}

/*
 * Is clause - a restriction clause, as a WHERE takes it, NULL counting as
 * not true - a predicate on one array column that is true of every row of a
 * class or of none (above)?  Fills *ne with the column and the classes it
 * selects.  The column is the clause's own; whether an index of the right
 * kind holds it is the caller's question (lion_nonempty_index()).
 */
bool
lion_nonempty_test(Node *clause, LionNonEmpty *ne)
{
	LionNETruth t[LION_NE_NCLASS];
	Var		   *var = NULL;
	int			cls;

	memset(ne, 0, sizeof(LionNonEmpty));
	if (clause != NULL && IsA(clause, RestrictInfo))
		clause = (Node *) ((RestrictInfo *) clause)->clause;
	if (!lion_ne_truth(clause, &var, t) || var == NULL)
		return false;
	for (cls = 0; cls < LION_NE_NCLASS; cls++)
	{
		if (t[cls] == LION_NE_MIXED)
			return false;
	}
	ne->var = var;
	for (cls = 0; cls < LION_NE_NCLASS; cls++)
	{
		if (t[cls] != LION_NE_TRUE)
			continue;
		if (cls == LION_NE_SOME_ROWS)
			ne->nonempty = true;
		else
			ne->reserved |= lion_ne_entry[cls];
	}
	return true;
}

/*
 * The clause core estimates best for the rows ne selects, over the
 * statistics of the column as a whole: `a <> '{}'`, `a = '{}'`, `a IS
 * DISTINCT FROM '{}'`, `NOT (a <> '{}')` (whose estimate is the empty
 * array's share plus the NULLs'), `a IS NULL` or `a IS NOT NULL`; a false
 * constant for none of them, and NULL for all, which needs no clause.
 *
 * Those clauses tell three classes apart, not five: core counts an empty
 * array with dimensions as one of the arrays unequal to '{}', and so do they.
 * The rows of the EMPTYDIM and EMPTYINNER classes - which only an
 * int2vector or an oidvector cast to an array makes, and are as rare as that
 * - are left out of the estimate where ne selects them and others: a true
 * constant for every row but some of them, and NULL, the clause's own
 * estimate, where ne selects nothing else.  An array type whose element type
 * cannot be found has NULL too.
 */
Node *
lion_nonempty_clause(const LionNonEmpty *ne)
{
	Var		   *var = ne->var;
	bool		isnull = (ne->reserved & LION_ENTRY_NULLKEY) != 0;
	bool		isempty = (ne->reserved & LION_ENTRY_EMPTYKEY) != 0;
	bool		nonempty = ne->nonempty;
	bool		withdims = (ne->reserved &
							(LION_ENTRY_EMPTYDIM | LION_ENTRY_EMPTYINNER)) != 0;
	Oid			arraytype;
	Oid			elemtype;
	Oid			neop;
	Const	   *empty;
	Expr	   *clause;

	if (isnull && isempty && nonempty)
		return (ne->reserved & LION_ENTRY_EMPTYKINDS) == LION_ENTRY_EMPTYKINDS ?
			NULL : (Node *) makeBoolConst(true, false);
	if (!isnull && !isempty && !nonempty)
		return withdims ? NULL : (Node *) makeBoolConst(false, false);
	if (isempty == nonempty)
	{
		/* NULL alone, or every row but NULL's: the column's null test */
		NullTest   *nt = makeNode(NullTest);

		nt->arg = (Expr *) copyObject(var);
		nt->nulltesttype = isnull ? IS_NULL : IS_NOT_NULL;
		nt->argisrow = false;
		nt->location = -1;
		return (Node *) nt;
	}
	arraytype = getBaseType(var->vartype);
	elemtype = get_element_type(arraytype);
	neop = get_negator(ARRAY_EQ_OP);
	if (!OidIsValid(elemtype) || !OidIsValid(neop))
		return NULL;
	empty = makeConst(arraytype, -1, var->varcollid, -1,
					  PointerGetDatum(construct_empty_array(elemtype)),
					  false, false);

	if (isnull && !isempty)
	{
		/* NULL and non-empty: `a IS DISTINCT FROM '{}'` */
		DistinctExpr *de = makeNode(DistinctExpr);

		de->opno = ARRAY_EQ_OP;
		de->opfuncid = F_ARRAY_EQ;
		de->opresulttype = BOOLOID;
		de->opretset = false;
		de->opcollid = InvalidOid;
		de->inputcollid = var->varcollid;
		de->args = list_make2(copyObject(var), empty);
		de->location = -1;
		return (Node *) de;
	}
	if (isempty && !isnull && !nonempty)
		return (Node *) make_opclause(ARRAY_EQ_OP, BOOLOID, false,
									  (Expr *) copyObject(var),
									  (Expr *) empty, InvalidOid,
									  var->varcollid);
	clause = make_opclause(neop, BOOLOID, false, (Expr *) copyObject(var),
						   (Expr *) empty, InvalidOid, var->varcollid);
	if (nonempty)
		return (Node *) clause; /* non-empty alone: `a <> '{}'` */
	/* NULL and empty: the complement of `a <> '{}'` */
	return (Node *) makeBoolExpr(NOT_EXPR, list_make1(clause), -1);
}

/*
 * The clause the count pushdown takes a non-emptiness predicate as
 * (lion_analyze_leaf()): RESERVED, the rows of the reserved entries the
 * classes it selects are, or NOTRESERVED, the rows outside those of the
 * classes it rejects - with the NULL entry alone the plain `IS NULL` or `IS
 * NOT NULL` it amounts to.  False when clause is not one (lion_nonempty_test()),
 * when it is true of every row or of none, and for a null test of the column
 * itself, which is lion_analyze_leaf()'s own.  The clause's estimate is that
 * of lion_nonempty_clause(), and whether the index on its column answers it
 * lion_nonempty_index()'s.
 */
bool
lion_nonempty_leaf(Node *clause, LionLeafInfo *out)
{
	LionNonEmpty ne;
	int32		mask;
	bool		negated;

	if (IsA(clause, RestrictInfo))
		clause = (Node *) ((RestrictInfo *) clause)->clause;
	if (IsA(clause, NullTest) &&
		IsA(lion_strip((Node *) ((NullTest *) clause)->arg), Var))
		return false;
	if (lion_ne_shape(clause) == 0 || !lion_nonempty_test(clause, &ne))
		return false;

	/* the reserved entries it selects, or those it rejects */
	negated = ne.nonempty;
	mask = negated ?
		((LION_ENTRY_NULLKEY | LION_ENTRY_EMPTYKINDS) & ~ne.reserved) :
		ne.reserved;
	if (mask == 0)
		return false;			/* true of every row, or of none */

	out->var = ne.var;
	out->opno = InvalidOid;
	out->cmptype = InvalidOid;
	out->collation = InvalidOid;
	if (mask == LION_ENTRY_NULLKEY)
	{
		out->kind = negated ? LION_CLAUSE_NOTNULL : LION_CLAUSE_NULL;
		out->val = (Node *) makeNullConst(ne.var->vartype, ne.var->vartypmod,
										  ne.var->varcollid);
	}
	else
	{
		Assert(LION_RESERVED_MASK_OK(mask));
		out->kind = negated ? LION_CLAUSE_NOTRESERVED : LION_CLAUSE_RESERVED;
		out->val = (Node *) makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
									  Int32GetDatum(mask), false, true);
	}
	out->costclause = lion_nonempty_clause(&ne);
	return true;
}

/*
 * Does key column col of idx (1-based) keep the classes apart: a column whose
 * keys are exactly the array's non-NULL elements, so that its EMPTY,
 * EMPTYDIM and EMPTYINNER entries hold exactly the arrays with no element
 * (DESIGN.md §17)?  That is lion's array_ops, whose extraction is GIN's
 * ginarrayextract() with long elements hashed, or a class of the user's that
 * extracts with ginarrayextract() itself (lion_extracts_array_elements()).
 * Another multi-key opclass's extraction need not be anything of the kind -
 * one that left some elements out would file a non-empty array under EMPTY.
 */
bool
lion_nonempty_index(IndexOptInfo *idx, AttrNumber col)
{
	int			i = col - 1;
	Oid			proc;
	FmgrInfo	finfo;

	if (col < 1 || col > idx->nkeycolumns)
		return false;
	proc = get_opfamily_proc(idx->opfamily[i], idx->opcintype[i],
							 idx->opcintype[i], LION_EXTRACTVALUE_PROC);
	if (!OidIsValid(proc))
		return false;
	fmgr_info(proc, &finfo);
	return lion_extracts_array_elements(finfo.fn_addr);
}

/*
 * clauses - rel's restriction clauses, RestrictInfos or bare - with the
 * predicates lion_nonempty_test() takes on each array column that a lion
 * array_ops index of rel holds ANDed into one clause, the one
 * lion_nonempty_clause() spells for the classes they select together, in
 * the place of the first of them; and whether that changed anything.  Core
 * estimates each of them alone and multiplies, so `array_length(a, 1) IS
 * NOT NULL AND array_length(a, 1) >= 0` - one set of rows written twice -
 * came out at 0.995 times a third of the table, whatever share of it is
 * empty.  The column's own null test, alone, is left as it is.
 *
 * Every estimate of a relation lion indexes asks this of its clauses, so a
 * list with nothing of the kind - no clause of a shape lion_ne_shape() lets
 * through, and fewer than two null tests of a column - is handed back as it
 * is, clauses itself, without a catalog lookup or an allocation.  A list
 * that changes is a new one.
 */
List *
lion_nonempty_rel_clauses(RelOptInfo *rel, List *clauses, bool *changed)
{
	int			n = list_length(clauses);
	LionNonEmpty *ne;
	int		   *group;
	bool	   *bare;
	Node	  **canon;
	List	   *result = NIL;
	ListCell   *lc;
	int			nbare = 0;
	bool		some = false;
	int			i;
	int			j;

	*changed = false;
	if (n == 0 || rel->indexlist == NIL)
		return clauses;
	foreach(lc, clauses)
	{
		Node	   *clause = (Node *) lfirst(lc);

		if (IsA(clause, RestrictInfo))
			clause = (Node *) ((RestrictInfo *) clause)->clause;
		switch (lion_ne_shape(clause))
		{
			case 1:
				nbare++;
				break;
			case 2:
				some = true;
				break;
		}
		if (some || nbare > 1)
			break;
	}
	if (!some && nbare < 2)
		return clauses;

	ne = (LionNonEmpty *) palloc0(sizeof(LionNonEmpty) * n);
	group = (int *) palloc(sizeof(int) * n);
	bare = (bool *) palloc0(sizeof(bool) * n);
	canon = (Node **) palloc0(sizeof(Node *) * n);

	/* each clause's column, and the first clause on it as its group */
	i = 0;
	foreach(lc, clauses)
	{
		Node	   *clause = (Node *) lfirst(lc);
		Node	   *expr = IsA(clause, RestrictInfo) ?
			(Node *) ((RestrictInfo *) clause)->clause : clause;
		AttrNumber	col = 0;

		group[i] = -1;
		if ((!IsA(clause, RestrictInfo) ||
			 !((RestrictInfo *) clause)->pseudoconstant) &&
			lion_ne_shape(expr) != 0 &&
			lion_nonempty_test(expr, &ne[i]) &&
			ne[i].var->varno == (int) rel->relid &&
			ne[i].var->varlevelsup == 0 && ne[i].var->varattno > 0)
		{
			IndexOptInfo *idx = lion_find_roaring_index(rel,
														ne[i].var->varattno,
														true, &col);

			if (idx != NULL && lion_nonempty_index(idx, col))
			{
				group[i] = i;
				for (j = 0; j < i; j++)
				{
					if (group[j] == j &&
						ne[j].var->varattno == ne[i].var->varattno)
					{
						group[i] = j;
						break;
					}
				}
				bare[i] = IsA(expr, NullTest) &&
					IsA(lion_strip((Node *) ((NullTest *) expr)->arg), Var);
			}
		}
		i++;
	}

	/* the classes each group selects together, in its first clause */
	for (i = 0; i < n; i++)
	{
		int			g = group[i];

		if (g < 0 || g == i)
			continue;
		ne[g].reserved &= ne[i].reserved;
		ne[g].nonempty = ne[g].nonempty && ne[i].nonempty;
		bare[g] = false;		/* two clauses are never one bare test */
	}

	/*
	 * Each group's clause, or none when it selects every row.  A group with
	 * no clause to spell its rows otherwise - an element type that cannot be
	 * found, or only empty arrays with dimensions - and a bare null test are
	 * left as they are.
	 */
	for (i = 0; i < n; i++)
	{
		bool		every;

		if (group[i] != i)
			continue;
		every = ne[i].nonempty &&
			ne[i].reserved == (LION_ENTRY_NULLKEY | LION_ENTRY_EMPTYKINDS);
		canon[i] = bare[i] ? NULL : lion_nonempty_clause(&ne[i]);
		if (bare[i] || (canon[i] == NULL && !every))
		{
			for (j = i; j < n; j++)
			{
				if (group[j] == i)
					group[j] = -1;
			}
		}
	}

	i = 0;
	foreach(lc, clauses)
	{
		if (group[i] < 0)
			result = lappend(result, lfirst(lc));
		else
		{
			/* the group's clause in its first's place; the others ANDed in */
			if (group[i] == i && canon[i] != NULL)
				result = lappend(result, canon[i]);
			*changed = true;
		}
		i++;
	}

	pfree(ne);
	pfree(group);
	pfree(bare);
	pfree(canon);
	if (!*changed)
	{
		list_free(result);
		return clauses;
	}
	return result;
}
