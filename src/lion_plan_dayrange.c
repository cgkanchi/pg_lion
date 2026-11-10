/*-------------------------------------------------------------------------
 *
 * lion_plan_dayrange.c
 *		Day filters as ranges (DESIGN.md §54):
 *
 *			WHERE ts::date = '2026-03-05'
 *			WHERE date_trunc('day', ts) = '2026-03-05'
 *			WHERE ts::date BETWEEN '2026-03-01' AND '2026-03-07'
 *
 *		over a `timestamp` column with a lion index on it, written as the
 *		ranges of ts they are:
 *
 *			WHERE ts >= '2026-03-05' AND ts < '2026-03-06'
 *
 *		A cast to date, and date_trunc() of a timestamp without time zone,
 *		are immutable and monotone (never smaller for a larger ts), and
 *		constant over buckets [A, N(A)) - a day, a month - whose bounds are
 *		known from the constant alone.  So each comparison of one of them with
 *		a constant is exactly one range of ts, and the range is what every
 *		index of ts answers: lion's count, ordered walk and bitmap scans, a
 *		B-tree, partition pruning.  The function of the column is none of
 *		them, and the plan was a sequential scan.
 *
 *		The rewrite is of the query's own quals, made once per relation from
 *		the hook after get_relation_info() (lion_get_relation_info(),
 *		lion_am.c): that runs for each base relation from
 *		add_base_rels_to_query(), before deconstruct_jointree() turns the
 *		quals into RestrictInfos, so every path, estimate and EquivalenceClass
 *		after it sees the ranges alone.  The replacement is equal to the clause
 *		for every ts, NULL included (both are NULL exactly when ts is), so it
 *		is made wherever the clause is in a qual: in WHERE, in any join's ON
 *		clause, under OR and NOT.
 *
 *		Only a relation with a lion index on the column is rewritten, and only
 *		a clause no index of the relation has as an expression key: a table
 *		without lion keeps PostgreSQL's plans as they were, and an expression
 *		index on ts::date is not starved of its clause.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/stratnum.h"
#include "catalog/pg_am.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "datatype/timestamp.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "parser/scansup.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/timestamp.h"

#include "lion.h"
#include "lion_count.h"

/* GUC pg_lion.enable_date_ranges */
bool		lion_enable_date_ranges = true;

typedef struct LionDayRangeCtx
{
	PlannerInfo *root;
	RelOptInfo *rel;
	Bitmapset  *attnos;			/* timestamp columns with a lion key */
	Oid			datefam;		/* btree family of date's default class */
	Oid			tsfam;			/* ... and of timestamp's */
	bool		changed;
} LionDayRangeCtx;

/*
 * A comparison as a btree strategy, or 0: `opno` a member of `family` between
 * two `type`s.
 */
static int
lion_dayrange_strategy(Oid opno, Oid family, Oid type)
{
	Oid			lefttype;
	Oid			righttype;
	int			strategy;

	if (!OidIsValid(family))
		return 0;
	op_input_types(opno, &lefttype, &righttype);
	if (lefttype != type || righttype != type)
		return 0;
	strategy = get_op_opfamily_strategy(opno, family);
	if (strategy < BTLessStrategyNumber || strategy > BTGreaterStrategyNumber)
		return 0;
	return strategy;
}

/* The strategy with its operands' places exchanged: c < f(ts) is f(ts) > c. */
static int
lion_dayrange_commute(int strategy)
{
	return BTMaxStrategyNumber + 1 - strategy;
}

/*
 * Is expr a plain Var of a timestamp column of the relation, one with a lion
 * key on it?
 */
static bool
lion_dayrange_is_column(LionDayRangeCtx *cx, Node *expr)
{
	Var		   *var = (Var *) expr;

	return expr != NULL && IsA(expr, Var) &&
		var->varno == (int) cx->rel->relid && var->varlevelsup == 0 &&
		var->vartype == TIMESTAMPOID &&
		var->varattno > 0 &&
		bms_is_member(var->varattno, cx->attnos);
}

/* Does an index of the relation have `expr` itself as an expression key? */
static bool
lion_dayrange_indexed(LionDayRangeCtx *cx, Node *expr)
{
	ListCell   *lc;

	foreach(lc, cx->rel->indexlist)
	{
		IndexOptInfo *ind = (IndexOptInfo *) lfirst(lc);
		ListCell   *le;

		foreach(le, ind->indexprs)
		{
			if (equal(lfirst(le), expr))
				return true;
		}
	}
	return false;
}

/*
 * The bucket function of a timestamp: date_trunc(unit, ts) with a constant
 * unit date_trunc() knows, its bucket one step of `step`; or ts::date (its
 * step is NULL).  Sets *var to the column.
 */
static bool
lion_dayrange_function(LionDayRangeCtx *cx, Node *expr, Var **var,
					   Interval **step)
{
	FuncExpr   *fe = (FuncExpr *) expr;

	if (expr == NULL || !IsA(expr, FuncExpr) || fe->funcretset)
		return false;
	if (fe->funcid == F_DATE_TIMESTAMP && list_length(fe->args) == 1)
	{
		if (!lion_dayrange_is_column(cx, linitial(fe->args)))
			return false;
		*var = (Var *) linitial(fe->args);
		*step = NULL;
		return true;
	}
	if (fe->funcid == F_DATE_TRUNC_TEXT_TIMESTAMP && list_length(fe->args) == 2)
	{
		Const	   *unit = (Const *) linitial(fe->args);
		char	   *lowunits;
		int			type;
		int			val;
		Interval   *iv;

		if (!IsA(unit, Const) || unit->constisnull ||
			unit->consttype != TEXTOID ||
			!lion_dayrange_is_column(cx, lsecond(fe->args)))
			return false;
		lowunits = TextDatumGetCString(unit->constvalue);
		lowunits = downcase_truncate_identifier(lowunits, strlen(lowunits),
												false);
		type = DecodeUnits(0, lowunits, &val);
		if (type != UNITS)
			return false;
		iv = (Interval *) palloc0(sizeof(Interval));
		switch (val)
		{
			case DTK_MICROSEC:
				iv->time = 1;
				break;
			case DTK_MILLISEC:
				iv->time = USECS_PER_SEC / 1000;
				break;
			case DTK_SECOND:
				iv->time = USECS_PER_SEC;
				break;
			case DTK_MINUTE:
				iv->time = USECS_PER_MINUTE;
				break;
			case DTK_HOUR:
				iv->time = USECS_PER_HOUR;
				break;
			case DTK_DAY:
				iv->day = 1;
				break;
			case DTK_WEEK:
				iv->day = 7;
				break;
			case DTK_MONTH:
				iv->month = 1;
				break;
			case DTK_QUARTER:
				iv->month = 3;
				break;
			case DTK_YEAR:
				iv->month = MONTHS_PER_YEAR;
				break;
			case DTK_DECADE:
				iv->month = 10 * MONTHS_PER_YEAR;
				break;
			case DTK_CENTURY:
				iv->month = 100 * MONTHS_PER_YEAR;
				break;
			case DTK_MILLENNIUM:
				iv->month = 1000 * MONTHS_PER_YEAR;
				break;
			default:
				return false;
		}
		*var = (Var *) lsecond(fe->args);
		*step = iv;
		return true;
	}
	return false;
}

/*
 * Timestamps this far from either end of the type are left alone: a bucket's
 * next bound past them could leave the range timestamp has, and computing it
 * would then raise an error the clause itself never raises.  A millennium
 * is the longest step.
 */
#define LION_DAYRANGE_MARGIN	\
	((int64) (2000 * DAYS_PER_YEAR) * USECS_PER_DAY)

static bool
lion_dayrange_inside(Timestamp t)
{
	return !TIMESTAMP_NOT_FINITE(t) &&
		t > MIN_TIMESTAMP + LION_DAYRANGE_MARGIN &&
		t < END_TIMESTAMP - LION_DAYRANGE_MARGIN;
}

/* date_trunc(unit, t), with the unit as text, as the clause would compute it */
static Timestamp
lion_dayrange_trunc(Const *unit, Timestamp t)
{
	return DatumGetTimestamp(DirectFunctionCall2(timestamp_trunc,
												 unit->constvalue,
												 TimestampGetDatum(t)));
}

/*
 * The bucket of `c` under the function: its first instant *start and the
 * first instant past it *next, for a bucket function f with f(t) = f(c) for
 * every t in [*start, *next).  For ts::date, `c` is a date and the bucket is
 * its day; for date_trunc(), `c` is a timestamp and the bucket the one that
 * holds it.  *aligned says whether c is its bucket's own value (f(start) =
 * c), which a date always is.  False where the bounds would leave the range
 * of timestamp, or where the bucket the step gives is not the one date_trunc()
 * itself makes: the bounds are checked against the function, not assumed.
 */
static bool
lion_dayrange_bucket(FuncExpr *fe, Interval *step, Const *c,
					 Timestamp *start, Timestamp *next, bool *aligned)
{
	if (step == NULL)
	{
		DateADT		d = DatumGetDateADT(c->constvalue);
		int			overflow = 0;

		if (DATE_NOT_FINITE(d) || d >= DATEVAL_NOEND - 1)
			return false;
		*start = date2timestamp_opt_overflow(d, &overflow);
		if (overflow != 0)
			return false;
		*next = date2timestamp_opt_overflow(d + 1, &overflow);
		if (overflow != 0)
			return false;
		*aligned = true;
		return lion_dayrange_inside(*start);
	}
	else
	{
		Const	   *unit = (Const *) linitial(fe->args);
		Timestamp	t = DatumGetTimestamp(c->constvalue);
		Timestamp	a;
		Timestamp	n;

		if (!lion_dayrange_inside(t))
			return false;
		a = lion_dayrange_trunc(unit, t);
		if (!lion_dayrange_inside(a))
			return false;
		n = DatumGetTimestamp(DirectFunctionCall2(timestamp_pl_interval,
												  TimestampGetDatum(a),
												  IntervalPGetDatum(step)));
		if (!lion_dayrange_inside(n) || n <= a)
			return false;
		/* the step lands on the next bucket's own start, and no further */
		if (lion_dayrange_trunc(unit, n) != n ||
			lion_dayrange_trunc(unit, n - 1) != a)
			return false;
		*start = a;
		*next = n;
		*aligned = (a == t);
		return true;
	}
}

/* `var op t`, op timestamp's of the strategy */
static Expr *
lion_dayrange_cmp(LionDayRangeCtx *cx, Var *var, int strategy, Timestamp t)
{
	Oid			opno = get_opfamily_member(cx->tsfam, TIMESTAMPOID, TIMESTAMPOID,
										   strategy);
	OpExpr	   *op;

	if (!OidIsValid(opno))
		elog(ERROR, "no operator of strategy %d for timestamp", strategy);
	op = (OpExpr *) make_opclause(opno, BOOLOID, false,
								  (Expr *) copyObject(var),
								  (Expr *) makeConst(TIMESTAMPOID, -1,
													 InvalidOid,
													 sizeof(Timestamp),
													 TimestampGetDatum(t),
													 false,
													 get_typbyval(TIMESTAMPOID)),
								  InvalidOid, InvalidOid);
	op->opfuncid = get_opcode(opno);
	return (Expr *) op;
}

/*
 * The ranges of ts that `f(ts) <strategy> c` is, as a list of clauses ANDed,
 * or NIL to leave the clause as it is.  With [A, N) the bucket of c and f
 * monotone, constant over each bucket:
 *
 *	f(ts) =  c	ts >= A AND ts < N		(c its bucket's own value; else none)
 *	f(ts) <  c	ts < A					(c aligned), ts < N (not: f(ts) <= f(c))
 *	f(ts) <= c	ts < N
 *	f(ts) >  c	ts >= N
 *	f(ts) >= c	ts >= A					(c aligned), ts >= N (not)
 *
 * An infinite ts is the infinite value of either type under f, so both
 * sides agree on it too.  An `=` with a constant inside a bucket holds for no
 * ts and is left alone, as rare as it is useless.
 */
static List *
lion_dayrange_ranges(LionDayRangeCtx *cx, FuncExpr *fe, Var *var,
					 Interval *step, int strategy, Const *c)
{
	Timestamp	start;
	Timestamp	next;
	bool		aligned;

	if (c->constisnull)
		return NIL;
	if (!lion_dayrange_bucket(fe, step, c, &start, &next, &aligned))
		return NIL;
	switch (strategy)
	{
		case BTEqualStrategyNumber:
			if (!aligned)
				return NIL;
			return list_make2(lion_dayrange_cmp(cx, var,
												BTGreaterEqualStrategyNumber,
												start),
							  lion_dayrange_cmp(cx, var, BTLessStrategyNumber,
												next));
		case BTLessStrategyNumber:
			return list_make1(lion_dayrange_cmp(cx, var, BTLessStrategyNumber,
												aligned ? start : next));
		case BTLessEqualStrategyNumber:
			return list_make1(lion_dayrange_cmp(cx, var, BTLessStrategyNumber,
												next));
		case BTGreaterStrategyNumber:
			return list_make1(lion_dayrange_cmp(cx, var,
												BTGreaterEqualStrategyNumber,
												next));
		case BTGreaterEqualStrategyNumber:
			return list_make1(lion_dayrange_cmp(cx, var,
												BTGreaterEqualStrategyNumber,
												aligned ? start : next));
	}
	return NIL;
}

/*
 * The ranges a clause is, or NIL when it is not one this rewrites: a
 * comparison of a bucket function of the column with a constant of the
 * function's type, either way round.
 */
static List *
lion_dayrange_clause(LionDayRangeCtx *cx, Node *node)
{
	OpExpr	   *op = (OpExpr *) node;
	Node	   *left;
	Node	   *right;
	Node	   *fn;
	Const	   *c;
	Var		   *var;
	Interval   *step;
	int			strategy;
	bool		commuted;

	if (node == NULL || !IsA(node, OpExpr) || list_length(op->args) != 2)
		return NIL;
	left = (Node *) linitial(op->args);
	right = (Node *) lsecond(op->args);
	if (IsA(right, Const))
	{
		fn = left;
		c = (Const *) right;
		commuted = false;
	}
	else if (IsA(left, Const))
	{
		fn = right;
		c = (Const *) left;
		commuted = true;
	}
	else
		return NIL;
	if (!lion_dayrange_function(cx, fn, &var, &step))
		return NIL;
	strategy = (step == NULL) ?
		lion_dayrange_strategy(op->opno, cx->datefam, DATEOID) :
		lion_dayrange_strategy(op->opno, cx->tsfam, TIMESTAMPOID);
	if (strategy == 0)
		return NIL;
	if (commuted)
		strategy = lion_dayrange_commute(strategy);
	if (lion_dayrange_indexed(cx, fn))
		return NIL;
	return lion_dayrange_ranges(cx, (FuncExpr *) fn, var, step, strategy, c);
}

static Node *
lion_dayrange_mutator(Node *node, LionDayRangeCtx *cx)
{
	List	   *ranges;

	if (node == NULL)
		return NULL;
	ranges = lion_dayrange_clause(cx, node);
	if (ranges != NIL)
	{
		cx->changed = true;
		return (list_length(ranges) == 1) ? (Node *) linitial(ranges) :
			(Node *) make_andclause(ranges);
	}
	/* a subquery's own quals are its own planner's to rewrite */
	if (IsA(node, Query))
		return node;
	return expression_tree_mutator(node, lion_dayrange_mutator, cx);
}

/*
 * A qual of the jointree: an implicit-AND list by now
 * (preprocess_qual_conditions()), whose members that are ranges of two
 * clauses become two members, as if the query had written them so.
 */
static Node *
lion_dayrange_quals(Node *quals, LionDayRangeCtx *cx)
{
	List	   *out = NIL;
	ListCell   *lc;

	if (quals == NULL)
		return NULL;
	if (!IsA(quals, List))
		return lion_dayrange_mutator(quals, cx);
	foreach(lc, (List *) quals)
	{
		Node	   *q = (Node *) lfirst(lc);
		List	   *ranges = lion_dayrange_clause(cx, q);

		if (ranges != NIL)
		{
			cx->changed = true;
			out = list_concat(out, ranges);
		}
		else
			out = lappend(out, lion_dayrange_mutator(q, cx));
	}
	return (Node *) out;
}

static void
lion_dayrange_jointree(Node *jtnode, LionDayRangeCtx *cx)
{
	if (jtnode == NULL)
		return;
	if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *lc;

		f->quals = lion_dayrange_quals(f->quals, cx);
		foreach(lc, f->fromlist)
			lion_dayrange_jointree((Node *) lfirst(lc), cx);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		j->quals = lion_dayrange_quals(j->quals, cx);
		lion_dayrange_jointree(j->larg, cx);
		lion_dayrange_jointree(j->rarg, cx);
	}
}

/* The btree family of a type's default btree class, or InvalidOid. */
static Oid
lion_dayrange_family(Oid type)
{
	Oid			opclass = GetDefaultOpClass(type, BTREE_AM_OID);

	return OidIsValid(opclass) ? get_opclass_family(opclass) : InvalidOid;
}

/*
 * Rewrite the query's day filters on `rel`'s columns as ranges (see the head
 * of this file).  Called for every base relation as the planner builds it,
 * with its index list final.
 */
void
lion_rewrite_day_filters(PlannerInfo *root, RelOptInfo *rel)
{
	LionDayRangeCtx cx;
	Oid			lionam;
	RangeTblEntry *rte;
	ListCell   *lc;

	if (!lion_enable_date_ranges || root == NULL || root->parse == NULL ||
		root->parse->jointree == NULL)
		return;
	/* an appendrel child's quals are its parent's, translated */
	if (rel->reloptkind != RELOPT_BASEREL || rel->indexlist == NIL)
		return;
	rte = planner_rt_fetch(rel->relid, root);
	if (rte->rtekind != RTE_RELATION)
		return;
	lionam = lion_get_am_oid();
	if (!OidIsValid(lionam))
		return;

	memset(&cx, 0, sizeof(cx));
	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *ind = (IndexOptInfo *) lfirst(lc);
		int			k;

		if (ind->relam != lionam || ind->hypothetical)
			continue;
		for (k = 0; k < ind->nkeycolumns; k++)
		{
			AttrNumber	attno = ind->indexkeys[k];

			if (attno > 0 &&
				get_atttype(rte->relid, attno) == TIMESTAMPOID)
				cx.attnos = bms_add_member(cx.attnos, attno);
		}
	}
	if (cx.attnos == NULL)
		return;

	cx.root = root;
	cx.rel = rel;
	cx.datefam = lion_dayrange_family(DATEOID);
	cx.tsfam = lion_dayrange_family(TIMESTAMPOID);
	if (!OidIsValid(cx.datefam) || !OidIsValid(cx.tsfam))
		return;
	lion_dayrange_jointree((Node *) root->parse->jointree, &cx);
}
