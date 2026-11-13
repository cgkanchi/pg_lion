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
 *		A value known only when the query runs - a generic plan's $1,
 *		current_date - and a timestamptz column, whose day is the session's
 *		TimeZone's then, get bounds that are calls of lion_daybounds.c's
 *		functions, evaluated once per scan (DESIGN.md §54, "Bounds known when
 *		the query runs"); see lion_dayrange_clause().
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/stratnum.h"
#include "catalog/dependency.h"
#include "catalog/pg_am.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "miscadmin.h"
#include "datatype/timestamp.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "parser/scansup.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/fmgroids.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

#include "lion.h"
#include "lion_count.h"

/* GUC pg_lion.enable_date_ranges */
bool		lion_enable_date_ranges = true;

/*
 * The extension's bound functions (lion_daybounds.c), looked up once a
 * relation needs one: first and last of a timestamp's day or bucket, lo and
 * hi of a timestamptz's pieces.
 */
typedef enum LionDayFn
{
	LION_DAYFN_FIRST_DATE,		/* lion_day_first(date) */
	LION_DAYFN_LAST_DATE,		/* lion_day_last(date) */
	LION_DAYFN_FIRST_TRUNC,		/* lion_day_first(text, timestamp) */
	LION_DAYFN_LAST_TRUNC,		/* lion_day_last(text, timestamp) */
	LION_DAYFN_LO_DATE,			/* lion_day_lo(date, text, int4) */
	LION_DAYFN_HI_DATE,			/* lion_day_hi(date, text, int4) */
	LION_DAYFN_LO_TRUNC,		/* lion_day_lo(text, timestamptz, text, int4) */
	LION_DAYFN_HI_TRUNC,		/* lion_day_hi(text, timestamptz, text, int4) */
	LION_DAYFN_N
} LionDayFn;

typedef struct LionDayRangeCtx
{
	PlannerInfo *root;
	RelOptInfo *rel;
	Bitmapset  *attnos;			/* timestamp[tz] columns with a lion key */
	Oid			datefam;		/* btree family of date's default class */
	Oid			tsfam;			/* ... of timestamp's */
	Oid			tstzfam;		/* ... and of timestamptz's */
	bool		fnlooked;		/* fns[] looked up */
	bool		fnok;			/* ... and all found, and executable */
	Oid			fns[LION_DAYFN_N];
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
 * Is expr a plain Var of a timestamp or timestamptz column of the relation,
 * one with a lion key on it?
 */
static bool
lion_dayrange_is_column(LionDayRangeCtx *cx, Node *expr)
{
	Var		   *var = (Var *) expr;

	return expr != NULL && IsA(expr, Var) &&
		var->varno == (int) cx->rel->relid && var->varlevelsup == 0 &&
		(var->vartype == TIMESTAMPOID || var->vartype == TIMESTAMPTZOID) &&
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

/* one step of a date_trunc() unit, as an interval (the constant ranges') */
static Interval *
lion_dayrange_step(int val)
{
	Interval   *iv = (Interval *) palloc0(sizeof(Interval));

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
			elog(ERROR, "unexpected date_trunc unit %d", val);
	}
	return iv;
}

/*
 * A bucket function of a column: ts::date (*unit 0), or date_trunc(unit, ts)
 * with a constant unit date_trunc() knows (*unit its DecodeUnits() value;
 * for a timestamptz, a unit of a day or more, whose bucket is the local
 * time's).  Sets *var to the column, *tz to whether it is a timestamptz,
 * and *step to one step of the unit for a timestamp's date_trunc() (NULL
 * otherwise).
 */
static bool
lion_dayrange_function(LionDayRangeCtx *cx, Node *expr, Var **var, int *unit,
					   bool *tz, Interval **step)
{
	FuncExpr   *fe = (FuncExpr *) expr;
	Const	   *u;
	text	   *t;

	if (expr == NULL || !IsA(expr, FuncExpr) || fe->funcretset)
		return false;
	*step = NULL;
	*unit = 0;
	if ((fe->funcid == F_DATE_TIMESTAMP || fe->funcid == F_DATE_TIMESTAMPTZ) &&
		list_length(fe->args) == 1)
	{
		if (!lion_dayrange_is_column(cx, linitial(fe->args)))
			return false;
		*var = (Var *) linitial(fe->args);
		*tz = (fe->funcid == F_DATE_TIMESTAMPTZ);
		return (*var)->vartype == (*tz ? TIMESTAMPTZOID : TIMESTAMPOID);
	}
	if ((fe->funcid != F_DATE_TRUNC_TEXT_TIMESTAMP &&
		 fe->funcid != F_DATE_TRUNC_TEXT_TIMESTAMPTZ) ||
		list_length(fe->args) != 2)
		return false;
	u = (Const *) linitial(fe->args);
	if (!IsA(u, Const) || u->constisnull || u->consttype != TEXTOID ||
		!lion_dayrange_is_column(cx, lsecond(fe->args)))
		return false;
	*var = (Var *) lsecond(fe->args);
	*tz = (fe->funcid == F_DATE_TRUNC_TEXT_TIMESTAMPTZ);
	if ((*var)->vartype != (*tz ? TIMESTAMPTZOID : TIMESTAMPOID))
		return false;
	t = DatumGetTextPP(u->constvalue);
	*unit = lion_day_unit(VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
	if (*unit < 0 || (*tz && !lion_day_unit_whole_days(*unit)))
		return false;
	if (!*tz)
		*step = lion_dayrange_step(*unit);
	return true;
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

		/*
		 * The day and the next one as timestamps, as date2timestamp() makes
		 * them, both inside the timestamp range (whose test there has a
		 * different name on each major version)
		 */
		if (DATE_NOT_FINITE(d) ||
			d < DATETIME_MIN_JULIAN - POSTGRES_EPOCH_JDATE ||
			d >= TIMESTAMP_END_JULIAN - POSTGRES_EPOCH_JDATE - 1)
			return false;
		*start = (Timestamp) d * USECS_PER_DAY;
		*next = (Timestamp) (d + 1) * USECS_PER_DAY;
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

/* `var op bound`, op the column type's of the strategy */
static Expr *
lion_dayrange_cmp_expr(LionDayRangeCtx *cx, Var *var, int strategy,
					   Expr *bound)
{
	Oid			type = var->vartype;
	Oid			opno = get_opfamily_member(type == TIMESTAMPOID ? cx->tsfam :
										   cx->tstzfam, type, type, strategy);
	OpExpr	   *op;

	if (!OidIsValid(opno))
		elog(ERROR, "no operator of strategy %d for %s", strategy,
			 format_type_be(type));
	op = (OpExpr *) make_opclause(opno, BOOLOID, false,
								  (Expr *) copyObject(var), bound,
								  InvalidOid, InvalidOid);
	op->opfuncid = get_opcode(opno);
	return (Expr *) op;
}

/* `var op t`, t a timestamp constant */
static Expr *
lion_dayrange_cmp(LionDayRangeCtx *cx, Var *var, int strategy, Timestamp t)
{
	return lion_dayrange_cmp_expr(cx, var, strategy,
								  (Expr *) makeConst(TIMESTAMPOID, -1,
													 InvalidOid,
													 sizeof(Timestamp),
													 TimestampGetDatum(t),
													 false,
													 get_typbyval(TIMESTAMPOID)));
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
 * Is expr a value the executor knows once per scan: no column of this level,
 * nothing volatile, no subplan - a generic plan's $1, current_date,
 * `$1::date + 1`?  It is evaluated where the clause would have been, and
 * once rather than per row: a value with neither volatile function nor
 * column is the same for every row of the scan.
 */
static bool
lion_dayrange_runtime_value(Node *expr)
{
	return expr != NULL && !contain_var_clause(expr) &&
		!contain_volatile_functions(expr) && !contain_subplans(expr) &&
		!expression_returns_set(expr);
}

/*
 * The extension's bound functions, found in its own schema and checked to be
 * its own and executable by the current user, as the executor will ask of
 * whoever runs the plan.  Read from the catalog cache, not looked up by name
 * as a query's function is (that asks USAGE on the schema, which running
 * one does not).  An extension whose SQL predates them keeps the clauses
 * that need them as written.
 *
 * Whether the plan calls them turns on the current user's EXECUTE, and the
 * query does not call them itself, so the plan is marked as made for this
 * role: a cached plan run after SET ROLE, or inside a SECURITY DEFINER
 * function, is made again for that role, which keeps the clauses as written
 * if it may not run them (rather than failing where the query would not).
 */
static bool
lion_dayrange_lookup(LionDayRangeCtx *cx)
{
	static const struct
	{
		const char *name;
		int			nargs;
		Oid			args[4];
	}			sig[LION_DAYFN_N] = {
		{"lion_day_first", 1, {DATEOID}},
		{"lion_day_last", 1, {DATEOID}},
		{"lion_day_first", 2, {TEXTOID, TIMESTAMPOID}},
		{"lion_day_last", 2, {TEXTOID, TIMESTAMPOID}},
		{"lion_day_lo", 3, {DATEOID, TEXTOID, INT4OID}},
		{"lion_day_hi", 3, {DATEOID, TEXTOID, INT4OID}},
		{"lion_day_lo", 4, {TEXTOID, TIMESTAMPTZOID, TEXTOID, INT4OID}},
		{"lion_day_hi", 4, {TEXTOID, TIMESTAMPTZOID, TEXTOID, INT4OID}},
	};
	Oid			extoid;
	Oid			nsp;
	int			i;

	if (cx->fnlooked)
		return cx->fnok;
	cx->fnlooked = true;
	cx->fnok = false;
	extoid = get_extension_oid("pg_lion", true);
	if (!OidIsValid(extoid))
		return false;
	nsp = get_extension_schema(extoid);
	if (!OidIsValid(nsp))
		return false;
	cx->root->glob->dependsOnRole = true;
	for (i = 0; i < LION_DAYFN_N; i++)
	{
		Oid			fn;

		fn = GetSysCacheOid3(PROCNAMEARGSNSP, Anum_pg_proc_oid,
							 CStringGetDatum(sig[i].name),
							 PointerGetDatum(buildoidvector(sig[i].args,
															sig[i].nargs)),
							 ObjectIdGetDatum(nsp));
		if (!OidIsValid(fn) || get_func_prokind(fn) != PROKIND_FUNCTION ||
			getExtensionOfObject(ProcedureRelationId, fn) != extoid ||
			object_aclcheck(ProcedureRelationId, fn, GetUserId(),
							ACL_EXECUTE) != ACLCHECK_OK)
			return false;
		cx->fns[i] = fn;
	}
	cx->fnok = true;
	return true;
}

/* text and int4 constants of a bound function's call */
static Expr *
lion_dayrange_text(const char *s)
{
	return (Expr *) makeConst(TEXTOID, -1, DEFAULT_COLLATION_OID, -1,
							  CStringGetTextDatum(s), false, false);
}

static Expr *
lion_dayrange_call(LionDayRangeCtx *cx, LionDayFn fn, Oid rettype, List *args)
{
	return (Expr *) makeFuncExpr(cx->fns[fn], rettype, args, InvalidOid,
								 DEFAULT_COLLATION_OID, COERCE_EXPLICIT_CALL);
}

/*
 * A timestamp's `f(ts) <strategy> c` with bounds first(c) and last(c) - the
 * least ts whose f(ts) >= c, the greatest whose f(ts) <= c - as the clauses
 * ANDed:
 *
 *	f(ts) <  c	ts <  first(c)
 *	f(ts) <= c	ts <= last(c)
 *	f(ts) >  c	ts >  last(c)
 *	f(ts) >= c	ts >= first(c)
 *	f(ts) =  c	ts >= first(c) AND ts <= last(c)
 *
 * exact for every c, the infinities and an empty bucket included, as f is
 * monotone (lion_daybounds.c).
 */
static List *
lion_dayrange_first_last(LionDayRangeCtx *cx, Var *var, int strategy,
						 Expr *first, Expr *last)
{
	switch (strategy)
	{
		case BTLessStrategyNumber:
			return list_make1(lion_dayrange_cmp_expr(cx, var, strategy, first));
		case BTLessEqualStrategyNumber:
		case BTGreaterStrategyNumber:
			return list_make1(lion_dayrange_cmp_expr(cx, var, strategy, last));
		case BTGreaterEqualStrategyNumber:
			return list_make1(lion_dayrange_cmp_expr(cx, var, strategy, first));
		case BTEqualStrategyNumber:
			return list_make2(lion_dayrange_cmp_expr(cx, var,
													 BTGreaterEqualStrategyNumber,
													 first),
							  lion_dayrange_cmp_expr(cx, var,
													 BTLessEqualStrategyNumber,
													 last));
	}
	return NIL;
}

static const char *const lion_dayrange_opnames[] = {"", "<", "<=", "=", ">=", ">"};

/*
 * The ranges a clause is, or NIL when it is not one this rewrites: a
 * comparison of a bucket function of the column with a value of the
 * function's type, either way round.
 *
 * Of a timestamp column and a constant, the ranges' bounds are computed
 * here (lion_dayrange_ranges()), or, past the edges that path keeps away
 * from, folded from first(c) and last(c).  Of a timestamp and a value known
 * at run time, they are calls of lion_day_first() and lion_day_last().  Of
 * a timestamptz, whose day is the session's TimeZone's at run time - and a
 * cached plan may run under another - always calls, and a union of two:
 *
 *	(ts >= lion_day_lo(c, op, 1) AND ts <= lion_day_hi(c, op, 1)) OR
 *	(ts >= lion_day_lo(c, op, 2) AND ts <= lion_day_hi(c, op, 2))
 *
 * (lion_daybounds.c: one range nearly everywhere, the other then empty).
 */
static List *
lion_dayrange_clause(LionDayRangeCtx *cx, Node *node)
{
	OpExpr	   *op = (OpExpr *) node;
	Node	   *left;
	Node	   *right;
	Node	   *fn;
	Node	   *c;
	Var		   *var;
	Interval   *step;
	int			unit;
	bool		tz;
	int			strategy;
	bool		commuted;
	List	   *args;

	if (node == NULL || !IsA(node, OpExpr) || list_length(op->args) != 2)
		return NIL;
	left = (Node *) linitial(op->args);
	right = (Node *) lsecond(op->args);
	if (lion_dayrange_function(cx, left, &var, &unit, &tz, &step))
	{
		fn = left;
		c = right;
		commuted = false;
	}
	else if (lion_dayrange_function(cx, right, &var, &unit, &tz, &step))
	{
		fn = right;
		c = left;
		commuted = true;
	}
	else
		return NIL;
	if (!IsA(c, Const) && !lion_dayrange_runtime_value(c))
		return NIL;
	if (unit == 0)
		strategy = lion_dayrange_strategy(op->opno, cx->datefam, DATEOID);
	else if (tz)
		strategy = lion_dayrange_strategy(op->opno, cx->tstzfam,
										  TIMESTAMPTZOID);
	else
		strategy = lion_dayrange_strategy(op->opno, cx->tsfam, TIMESTAMPOID);
	if (strategy == 0)
		return NIL;
	if (commuted)
		strategy = lion_dayrange_commute(strategy);
	if (lion_dayrange_indexed(cx, fn))
		return NIL;

	if (!tz && IsA(c, Const))
	{
		Const	   *k = (Const *) c;
		List	   *ranges;

		if (k->constisnull)
			return NIL;
		ranges = lion_dayrange_ranges(cx, (FuncExpr *) fn, var, step,
									  strategy, k);
		if (ranges != NIL)
			return ranges;

		/*
		 * An infinity, a constant near the ends of timestamp's range or past
		 * it, an `=` inside a bucket: the bounds folded now, exact for
		 * these too
		 */
		return lion_dayrange_first_last(cx, var, strategy,
										(Expr *) makeConst(TIMESTAMPOID, -1, InvalidOid,
														   sizeof(Timestamp),
														   TimestampGetDatum(lion_day_bound(unit, k->constvalue, false)),
														   false, get_typbyval(TIMESTAMPOID)),
										(Expr *) makeConst(TIMESTAMPOID, -1, InvalidOid,
														   sizeof(Timestamp),
														   TimestampGetDatum(lion_day_bound(unit, k->constvalue, true)),
														   false, get_typbyval(TIMESTAMPOID)));
	}
	if (!lion_dayrange_lookup(cx))
		return NIL;
	if (!tz)
	{
		args = (unit == 0) ? list_make1(copyObject(c)) :
			list_make2(copyObject(linitial(((FuncExpr *) fn)->args)),
					   copyObject(c));
		return lion_dayrange_first_last(cx, var, strategy,
										lion_dayrange_call(cx, unit == 0 ?
														   LION_DAYFN_FIRST_DATE :
														   LION_DAYFN_FIRST_TRUNC,
														   TIMESTAMPOID, args),
										lion_dayrange_call(cx, unit == 0 ?
														   LION_DAYFN_LAST_DATE :
														   LION_DAYFN_LAST_TRUNC,
														   TIMESTAMPOID,
														   copyObject(args)));
	}
	else
	{
		List	   *arms = NIL;
		int			piece;

		for (piece = 1; piece <= 2; piece++)
		{
			Expr	   *bounds[2];
			int			b;

			for (b = 0; b < 2; b++)
			{
				List	   *a = list_make3(copyObject(c),
										   lion_dayrange_text(lion_dayrange_opnames[strategy]),
										   makeConst(INT4OID, -1, InvalidOid,
													 sizeof(int32),
													 Int32GetDatum(piece),
													 false, true));

				if (unit != 0)
					a = lcons(copyObject(linitial(((FuncExpr *) fn)->args)), a);
				bounds[b] = lion_dayrange_call(cx,
											   unit == 0 ?
											   (b == 0 ? LION_DAYFN_LO_DATE : LION_DAYFN_HI_DATE) :
											   (b == 0 ? LION_DAYFN_LO_TRUNC : LION_DAYFN_HI_TRUNC),
											   TIMESTAMPTZOID, a);
			}
			arms = lappend(arms,
						   make_andclause(list_make2(lion_dayrange_cmp_expr(cx, var,
																			BTGreaterEqualStrategyNumber,
																			bounds[0]),
													 lion_dayrange_cmp_expr(cx, var,
																			BTLessEqualStrategyNumber,
																			bounds[1]))));
		}
		return list_make1(make_orclause(arms));
	}
}

/*
 * The clause rewritten, wherever it is in node.  An AND or OR whose argument
 * becomes one of its own kind - an AND of two bounds under an AND, the
 * timestamptz union under an OR - takes that one's arguments as its own:
 * the planner was handed its quals flat (canonicalize_qual(),
 * eval_const_expressions()), and what it does with an OR's arms after this
 * (make_sub_restrictinfos(), the TID and bitmap OR paths) takes an AND's
 * arguments to be clauses and an OR's to be no ORs.
 */
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
	if (is_andclause(node) || is_orclause(node))
	{
		BoolExpr   *b = (BoolExpr *) node;
		BoolExpr   *out = makeNode(BoolExpr);
		ListCell   *lc;

		out->boolop = b->boolop;
		out->location = b->location;
		foreach(lc, b->args)
		{
			Node	   *arg = lion_dayrange_mutator((Node *) lfirst(lc), cx);

			if (IsA(arg, BoolExpr) && ((BoolExpr *) arg)->boolop == b->boolop)
				out->args = list_concat(out->args, ((BoolExpr *) arg)->args);
			else
				out->args = lappend(out->args, arg);
		}
		return (Node *) out;
	}
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

			if (attno > 0)
			{
				Oid			type = get_atttype(rte->relid, attno);

				if (type == TIMESTAMPOID || type == TIMESTAMPTZOID)
					cx.attnos = bms_add_member(cx.attnos, attno);
			}
		}
	}
	if (cx.attnos == NULL)
		return;

	cx.root = root;
	cx.rel = rel;
	cx.datefam = lion_dayrange_family(DATEOID);
	cx.tsfam = lion_dayrange_family(TIMESTAMPOID);
	cx.tstzfam = lion_dayrange_family(TIMESTAMPTZOID);
	if (!OidIsValid(cx.datefam) || !OidIsValid(cx.tsfam) ||
		!OidIsValid(cx.tstzfam))
		return;
	lion_dayrange_jointree((Node *) root->parse->jointree, &cx);
}
