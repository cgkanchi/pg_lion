/*-------------------------------------------------------------------------
 *
 * lion_daybounds.c
 *		The bounds of a day filter's ranges when they are known only as the
 *		query runs (DESIGN.md §54, "Bounds known when the query runs").
 *
 *		lion_plan_dayrange.c writes `ts::date OP c` and `date_trunc(unit, ts)
 *		OP c` as ranges of ts.  For a timestamp column and a constant c the
 *		bounds are constants.  Where c is known only at run time - a generic
 *		plan's $1, current_date - or ts is a timestamptz, whose day is the
 *		session's TimeZone's, the bounds are calls of the functions here,
 *		which an index qual evaluates once per scan:
 *
 *		  timestamp		ts <  lion_day_first(c)		f(ts) <  c
 *						ts <= lion_day_last(c)		f(ts) <= c
 *						ts >  lion_day_last(c)		f(ts) >  c
 *						ts >= lion_day_first(c)		f(ts) >= c
 *						both of the above			f(ts) =  c
 *
 *		first(c) the least ts whose f(ts) >= c, last(c) the greatest whose
 *		f(ts) <= c: f is monotone, so each comparison is one range.
 *
 *		  timestamptz	(ts >= lion_day_lo(c, op, 1) AND ts <= lion_day_hi(c, op, 1))
 *					 OR (ts >= lion_day_lo(c, op, 2) AND ts <= lion_day_hi(c, op, 2))
 *
 *		A timestamptz's day is the date of its local time, and local time is
 *		not monotone: where the clocks step back across midnight, the day
 *		that follows starts, is interrupted by the repeated hour and starts
 *		again.  So the instants whose day compares with c are a union of
 *		ranges - one nearly everywhere, two across such a step - found here
 *		from the zone's transitions (lion_day_from()).  None of the time zone
 *		database's zones needs a third: none steps back twice within four
 *		days (every zone checked, 2026-11-13), and PostgreSQL itself assumes
 *		its zones' transitions 48 hours apart (DetermineTimeZoneOffset()).
 *		A third would be an error, never a wrong row.  A piece the set has
 *		no use for is the empty range [p, p - 1 microsecond].
 *
 *		date_trunc(unit, ts) of a timestamptz, for a unit of a day or more,
 *		is its local time's bucket, read back as an instant in the zone
 *		(timestamptz_trunc_internal()): g(T(L(ts))), T the truncation and g
 *		the bucket's instant.  g never decreases from one bucket to the next
 *		where the zone's offsets differ by no more than a day, which
 *		PostgreSQL also assumes, so the buckets whose instant compares with c
 *		are a run of them, and the instants are those whose local time lies
 *		in the run - the cast's problem again, with buckets for days.  The
 *		run's ends are found by walking the buckets around c's own, checking
 *		g's order as it goes (lion_day_trunc_marks()).
 *
 *		None of the functions raises an error for any argument the clause
 *		accepts.  Bounds past what the type holds are its infinities or the
 *		ends of its range, so the ranges hold exactly the rows whose f(ts)
 *		compares as the clause says; a row whose f(ts) the clause itself
 *		cannot compute (date_trunc('month', ts) of the first days of 4714
 *		BC, whose month starts before the type's range) is where the clause
 *		would have failed, and is placed by its own local time.  The
 *		truncation is PostgreSQL's own (timestamp_trunc()'s), made on the
 *		fields without the range checks that raise those errors.  Every
 *		timestamptz range's edges are then checked against the clause
 *		itself, computed as PostgreSQL computes it, where it can be.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/stratnum.h"
#include "common/int.h"
#include "datatype/timestamp.h"
#include "fmgr.h"
#include "parser/scansup.h"
#include "pgtime.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/timestamp.h"

#include "lion.h"

PG_FUNCTION_INFO_V1(lion_day_first_date);
PG_FUNCTION_INFO_V1(lion_day_last_date);
PG_FUNCTION_INFO_V1(lion_day_first_trunc);
PG_FUNCTION_INFO_V1(lion_day_last_trunc);
PG_FUNCTION_INFO_V1(lion_day_lo_date);
PG_FUNCTION_INFO_V1(lion_day_hi_date);
PG_FUNCTION_INFO_V1(lion_day_lo_trunc);
PG_FUNCTION_INFO_V1(lion_day_hi_trunc);

/*
 * No zone's local time is this far from UT (PostgreSQL's own POSIX-style
 * zones stop below a week), which lion_day_from() relies on and checks.
 */
#define LION_DAY_MAXOFF		(7 * SECS_PER_DAY)
#define LION_DAY_WINDOW		(INT64CONST(8) * USECS_PER_DAY)

/* seconds from the Unix epoch to PostgreSQL's */
#define LION_DAY_EPOCH_SECS \
	((pg_time_t) (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY)

/* the days of timestamp's range: [MIN, END) as dates since 2000-01-01 */
#define LION_DAY_MIN_DAYS	(MIN_TIMESTAMP / USECS_PER_DAY)
#define LION_DAY_END_DAYS	(END_TIMESTAMP / USECS_PER_DAY)

/* where a bucket's start lies */
#define LION_DAY_BELOW		(-1)	/* before any Julian day timestamp has */
#define LION_DAY_AT			0
#define LION_DAY_ABOVE		1		/* past int64 */

/* the most spans a set may take on the way (the answer, two) */
#define LION_DAY_SPANS		8

/*
 * The unit of date_trunc() a day filter is rewritten for, as DecodeUnits()
 * names it, or -1.
 */
int
lion_day_unit(const char *str, int len)
{
	char	   *low = downcase_truncate_identifier(str, len, false);
	int			val;
	int			type = DecodeUnits(0, low, &val);

	pfree(low);
	if (type != UNITS)
		return -1;
	switch (val)
	{
		case DTK_MICROSEC:
		case DTK_MILLISEC:
		case DTK_SECOND:
		case DTK_MINUTE:
		case DTK_HOUR:
		case DTK_DAY:
		case DTK_WEEK:
		case DTK_MONTH:
		case DTK_QUARTER:
		case DTK_YEAR:
		case DTK_DECADE:
		case DTK_CENTURY:
		case DTK_MILLENNIUM:
			return val;
	}
	return -1;
}

/*
 * A unit of whole days or more: date_trunc() of a timestamptz reads such a
 * bucket's start back in the zone, and only for those is it the local
 * time's bucket (timestamptz_trunc_internal()'s redotz).
 */
bool
lion_day_unit_whole_days(int val)
{
	switch (val)
	{
		case DTK_DAY:
		case DTK_WEEK:
		case DTK_MONTH:
		case DTK_QUARTER:
		case DTK_YEAR:
		case DTK_DECADE:
		case DTK_CENTURY:
		case DTK_MILLENNIUM:
			return true;
	}
	return false;
}

static int
lion_day_unit_of(text *unit)
{
	int			val = lion_day_unit(VARDATA_ANY(unit), VARSIZE_ANY_EXHDR(unit));

	if (val < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unit \"%s\" not supported by a lion day range",
						text_to_cstring(unit))));
	return val;
}

/* "<", "<=", "=", ">=" or ">" as a btree strategy */
static int
lion_day_op_of(text *op)
{
	char	   *s = text_to_cstring(op);
	int			strategy = 0;

	if (strcmp(s, "<") == 0)
		strategy = BTLessStrategyNumber;
	else if (strcmp(s, "<=") == 0)
		strategy = BTLessEqualStrategyNumber;
	else if (strcmp(s, "=") == 0)
		strategy = BTEqualStrategyNumber;
	else if (strcmp(s, ">=") == 0)
		strategy = BTGreaterEqualStrategyNumber;
	else if (strcmp(s, ">") == 0)
		strategy = BTGreaterStrategyNumber;
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("comparison \"%s\" not supported by a lion day range",
						s)));
	pfree(s);
	return strategy;
}

/* ---------------------------------------------------------------------
 * Buckets on the local line
 *
 * A local time is a timestamp without time zone's microseconds since
 * 2000-01-01, unbounded by its range: a bucket may start before 4714 BC, a
 * local time lie a few days past either end.
 * --------------------------------------------------------------------- */

/* timestamp_trunc()'s order of the units it truncates by fields */
static int
lion_day_unit_rank(int val)
{
	switch (val)
	{
		case DTK_MILLENNIUM:
			return 10;
		case DTK_CENTURY:
			return 9;
		case DTK_DECADE:
			return 8;
		case DTK_YEAR:
			return 7;
		case DTK_QUARTER:
			return 6;
		case DTK_MONTH:
			return 5;
		case DTK_DAY:
			return 4;
		case DTK_HOUR:
			return 3;
		case DTK_MINUTE:
			return 2;
		case DTK_SECOND:
			return 1;
	}
	return 0;
}

/*
 * Truncate fields to the unit's bucket, as timestamp_trunc() and
 * timestamptz_trunc_internal() do (the same on 16 to 19), their cases'
 * fall-through spelled out by rank.
 */
static void
lion_day_trunc_tm(int val, struct pg_tm *tm, fsec_t *fsec)
{
	int			rank;

	if (val == DTK_WEEK)
	{
		int			woy = date2isoweek(tm->tm_year, tm->tm_mon, tm->tm_mday);

		/*
		 * If it is week 52/53 and the month is January, then the week must
		 * belong to the previous year.  Also, some December dates belong to
		 * the next year.
		 */
		if (woy >= 52 && tm->tm_mon == 1)
			--tm->tm_year;
		if (woy <= 1 && tm->tm_mon == MONTHS_PER_YEAR)
			++tm->tm_year;
		isoweek2date(woy, &(tm->tm_year), &(tm->tm_mon), &(tm->tm_mday));
		tm->tm_hour = 0;
		tm->tm_min = 0;
		tm->tm_sec = 0;
		*fsec = 0;
		return;
	}
	if (val == DTK_MILLISEC)
	{
		*fsec = (*fsec / 1000) * 1000;
		return;
	}
	if (val == DTK_MICROSEC)
		return;
	if (val == DTK_MILLENNIUM)
	{
		if (tm->tm_year > 0)
			tm->tm_year = ((tm->tm_year + 999) / 1000) * 1000 - 999;
		else
			tm->tm_year = -((999 - (tm->tm_year - 1)) / 1000) * 1000 + 1;
	}
	if (val == DTK_MILLENNIUM || val == DTK_CENTURY)
	{
		if (tm->tm_year > 0)
			tm->tm_year = ((tm->tm_year + 99) / 100) * 100 - 99;
		else
			tm->tm_year = -((99 - (tm->tm_year - 1)) / 100) * 100 + 1;
	}
	if (val == DTK_DECADE)
	{
		if (tm->tm_year > 0)
			tm->tm_year = (tm->tm_year / 10) * 10;
		else
			tm->tm_year = -((8 - (tm->tm_year - 1)) / 10) * 10;
	}
	rank = lion_day_unit_rank(val);
	if (rank >= 7)
		tm->tm_mon = 1;
	if (rank >= 6)
		tm->tm_mon = (3 * ((tm->tm_mon - 1) / 3)) + 1;
	if (rank >= 5)
		tm->tm_mday = 1;
	if (rank >= 4)
		tm->tm_hour = 0;
	if (rank >= 3)
		tm->tm_min = 0;
	if (rank >= 2)
		tm->tm_sec = 0;
	if (rank >= 1)
		*fsec = 0;
}

/*
 * The fields of local time x, as timestamp2tm() gives a timestamp's without
 * a zone, but for x a little before timestamp's range too (j2date() reaches
 * Julian day -32044).
 */
static bool
lion_day_tm_of(int64 x, struct pg_tm *tm, fsec_t *fsec)
{
	int64		date;
	int64		time = x;

	date = time / USECS_PER_DAY;
	time -= date * USECS_PER_DAY;
	if (time < 0)
	{
		time += USECS_PER_DAY;
		date -= 1;
	}
	date += POSTGRES_EPOCH_JDATE;
	if (date < -32044 || date > (int64) INT_MAX)
		return false;
	memset(tm, 0, sizeof(*tm));
	j2date((int) date, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
	tm->tm_hour = (int) (time / USECS_PER_HOUR);
	time -= tm->tm_hour * USECS_PER_HOUR;
	tm->tm_min = (int) (time / USECS_PER_MINUTE);
	time -= tm->tm_min * USECS_PER_MINUTE;
	tm->tm_sec = (int) (time / USECS_PER_SEC);
	*fsec = (fsec_t) (time - tm->tm_sec * USECS_PER_SEC);
	tm->tm_isdst = -1;
	return true;
}

/*
 * The local time of fields, as tm2timestamp() computes it without a zone
 * and without its range check: LION_DAY_BELOW where the fields are before
 * the Julian days it accepts, LION_DAY_ABOVE past int64.
 */
static int
lion_day_local_of(const struct pg_tm *tm, fsec_t fsec, int64 *out)
{
	int64		date;
	int64		time;
	int64		r;

	if (!IS_VALID_JULIAN(tm->tm_year, tm->tm_mon, tm->tm_mday))
		return (tm->tm_year < JULIAN_MINYEAR ||
				(tm->tm_year == JULIAN_MINYEAR &&
				 tm->tm_mon < JULIAN_MINMONTH)) ?
			LION_DAY_BELOW : LION_DAY_ABOVE;
	date = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - POSTGRES_EPOCH_JDATE;
	time = (((int64) tm->tm_hour * MINS_PER_HOUR + tm->tm_min) *
			SECS_PER_MINUTE + tm->tm_sec) * USECS_PER_SEC + fsec;
	if (pg_mul_s64_overflow(date, USECS_PER_DAY, &r) ||
		pg_add_s64_overflow(r, time, &r))
		return date < 0 ? LION_DAY_BELOW : LION_DAY_ABOVE;
	*out = r;
	return LION_DAY_AT;
}

/* A bucket's start: its fields, and its local time where it has one. */
typedef struct LionDayMark
{
	struct pg_tm tm;
	fsec_t		fsec;
	int			where;			/* LION_DAY_BELOW, _AT or _ABOVE */
	int64		at;				/* the local time, where LION_DAY_AT */
} LionDayMark;

/* the bucket that holds local time x */
static void
lion_day_mark_of(int val, int64 x, LionDayMark *m)
{
	if (!lion_day_tm_of(x, &m->tm, &m->fsec))
		elog(ERROR, "local time " INT64_FORMAT " out of reach of a lion day range",
			 x);
	lion_day_trunc_tm(val, &m->tm, &m->fsec);
	m->where = lion_day_local_of(&m->tm, m->fsec, &m->at);
}

/*
 * The bucket after m, by its fields; checked to be a bucket's own start, as
 * the truncation makes them.
 */
static void
lion_day_mark_next(int val, LionDayMark *m)
{
	int64		step = 0;
	int			months = 0;
	struct pg_tm check;
	fsec_t		checkfsec;

	switch (val)
	{
		case DTK_MICROSEC:
			step = 1;
			break;
		case DTK_MILLISEC:
			step = 1000;
			break;
		case DTK_SECOND:
			step = USECS_PER_SEC;
			break;
		case DTK_MINUTE:
			step = USECS_PER_MINUTE;
			break;
		case DTK_HOUR:
			step = USECS_PER_HOUR;
			break;
		case DTK_DAY:
		case DTK_WEEK:
			{
				int			j = date2j(m->tm.tm_year, m->tm.tm_mon,
									   m->tm.tm_mday) +
					(val == DTK_WEEK ? 7 : 1);

				j2date(j, &m->tm.tm_year, &m->tm.tm_mon, &m->tm.tm_mday);
				break;
			}
		case DTK_MONTH:
			months = 1;
			break;
		case DTK_QUARTER:
			months = 3;
			break;
		case DTK_YEAR:
			months = MONTHS_PER_YEAR;
			break;
		case DTK_DECADE:
			months = 10 * MONTHS_PER_YEAR;
			break;
		case DTK_CENTURY:
			months = 100 * MONTHS_PER_YEAR;
			break;
		case DTK_MILLENNIUM:
			months = 1000 * MONTHS_PER_YEAR;
			break;
		default:
			elog(ERROR, "unexpected unit %d", val);
	}
	if (step > 0)
	{
		/* a bucket within a day: its fields from its time */
		if (m->where != LION_DAY_AT)
			elog(ERROR, "a lion day range's bucket has no local time");
		if (pg_add_s64_overflow(m->at, step, &m->at))
		{
			m->where = LION_DAY_ABOVE;
			return;
		}
		if (!lion_day_tm_of(m->at, &m->tm, &m->fsec))
		{
			m->where = LION_DAY_ABOVE;
			return;
		}
	}
	else if (months > 0)
	{
		int64		total = (int64) m->tm.tm_year * MONTHS_PER_YEAR +
			(m->tm.tm_mon - 1) + months;
		int64		y = total / MONTHS_PER_YEAR;
		int64		mon = total - y * MONTHS_PER_YEAR;

		if (mon < 0)
		{
			mon += MONTHS_PER_YEAR;
			y -= 1;
		}
		if (y > INT_MAX / 2)
		{
			m->where = LION_DAY_ABOVE;
			return;
		}
		m->tm.tm_year = (int) y;
		m->tm.tm_mon = (int) mon + 1;
	}
	m->where = lion_day_local_of(&m->tm, m->fsec, &m->at);

	/* a bucket's start truncates to itself */
	check = m->tm;
	checkfsec = m->fsec;
	lion_day_trunc_tm(val, &check, &checkfsec);
	if (check.tm_year != m->tm.tm_year || check.tm_mon != m->tm.tm_mon ||
		check.tm_mday != m->tm.tm_mday || check.tm_hour != m->tm.tm_hour ||
		check.tm_min != m->tm.tm_min || check.tm_sec != m->tm.tm_sec ||
		checkfsec != m->fsec)
		elog(ERROR, "the next bucket of a lion day range is not a bucket's start");
}

/* ---------------------------------------------------------------------
 * timestamp: first(c) and last(c)
 * --------------------------------------------------------------------- */

/*
 * The least timestamp whose date is d or later, and the greatest whose date
 * is d or earlier, the infinities where no finite one is: a timestamp's date
 * is its local midnight's, every day of its range whole.
 */
static Timestamp
lion_day_first_of_date(DateADT d)
{
	if (DATE_IS_NOBEGIN(d))
		return DT_NOBEGIN;
	if (DATE_IS_NOEND(d))
		return DT_NOEND;
	if ((int64) d <= LION_DAY_MIN_DAYS)
		return MIN_TIMESTAMP;
	if ((int64) d >= LION_DAY_END_DAYS)
		return DT_NOEND;
	return (Timestamp) d * USECS_PER_DAY;
}

static Timestamp
lion_day_last_of_date(DateADT d)
{
	int64		next;

	if (DATE_IS_NOBEGIN(d))
		return DT_NOBEGIN;
	if (DATE_IS_NOEND(d))
		return DT_NOEND;
	next = (int64) d + 1;
	if (next <= LION_DAY_MIN_DAYS)
		return DT_NOBEGIN;
	if (next >= LION_DAY_END_DAYS)
		return END_TIMESTAMP - 1;
	return (Timestamp) next * USECS_PER_DAY - 1;
}

/*
 * The same of date_trunc(unit, ts) against a timestamp c: c's bucket [A, N)
 * holds every ts whose truncation is A, so the least ts truncating to c or
 * later is c where c is A itself and N otherwise, and the greatest
 * truncating to c or earlier is N's last microsecond.
 */
static Timestamp
lion_day_trunc_first(int val, Timestamp c)
{
	LionDayMark m;

	if (TIMESTAMP_NOT_FINITE(c))
		return c;
	lion_day_mark_of(val, c, &m);
	if (m.where == LION_DAY_AT && m.at == c)
		return c;
	lion_day_mark_next(val, &m);
	if (m.where == LION_DAY_ABOVE || (m.where == LION_DAY_AT &&
									  m.at >= END_TIMESTAMP))
		return DT_NOEND;
	if (m.where != LION_DAY_AT || m.at <= c)
		elog(ERROR, "the bucket after a timestamp lies before it");
	return m.at;
}

static Timestamp
lion_day_trunc_last(int val, Timestamp c)
{
	LionDayMark m;

	if (TIMESTAMP_NOT_FINITE(c))
		return c;
	lion_day_mark_of(val, c, &m);
	lion_day_mark_next(val, &m);
	if (m.where == LION_DAY_ABOVE || (m.where == LION_DAY_AT &&
									  m.at >= END_TIMESTAMP))
		return END_TIMESTAMP - 1;
	if (m.where != LION_DAY_AT || m.at <= c)
		elog(ERROR, "the bucket after a timestamp lies before it");
	return m.at - 1;
}

/* lion_day_first(date) and lion_day_last(date) */
Datum
lion_day_first_date(PG_FUNCTION_ARGS)
{
	PG_RETURN_TIMESTAMP(lion_day_first_of_date(PG_GETARG_DATEADT(0)));
}

Datum
lion_day_last_date(PG_FUNCTION_ARGS)
{
	PG_RETURN_TIMESTAMP(lion_day_last_of_date(PG_GETARG_DATEADT(0)));
}

/* lion_day_first(unit text, c timestamp) and lion_day_last(unit, c) */
Datum
lion_day_first_trunc(PG_FUNCTION_ARGS)
{
	int			val = lion_day_unit_of(PG_GETARG_TEXT_PP(0));

	PG_RETURN_TIMESTAMP(lion_day_trunc_first(val, PG_GETARG_TIMESTAMP(1)));
}

Datum
lion_day_last_trunc(PG_FUNCTION_ARGS)
{
	int			val = lion_day_unit_of(PG_GETARG_TEXT_PP(0));

	PG_RETURN_TIMESTAMP(lion_day_trunc_last(val, PG_GETARG_TIMESTAMP(1)));
}

/* the same, for the planner to fold a constant with (lion_plan_dayrange.c) */
int64
lion_day_bound(int val, Datum c, bool last)
{
	if (val == 0)
		return last ? lion_day_last_of_date(DatumGetDateADT(c)) :
			lion_day_first_of_date(DatumGetDateADT(c));
	return last ? lion_day_trunc_last(val, DatumGetTimestamp(c)) :
		lion_day_trunc_first(val, DatumGetTimestamp(c));
}

/* ---------------------------------------------------------------------
 * timestamptz: sets of instants
 *
 * A set is ascending disjoint closed spans over the points a timestamptz
 * holds: -infinity, [MIN_TIMESTAMP, END_TIMESTAMP) and infinity.
 * --------------------------------------------------------------------- */

typedef struct LionDaySpans
{
	int			n;
	TimestampTz lo[LION_DAY_SPANS];
	TimestampTz hi[LION_DAY_SPANS];
} LionDaySpans;

/* the next point a timestamptz holds, and the one before */
static TimestampTz
lion_day_succ(TimestampTz x)
{
	if (x == DT_NOBEGIN)
		return MIN_TIMESTAMP;
	if (x >= END_TIMESTAMP - 1)
		return DT_NOEND;
	return x + 1;
}

static TimestampTz
lion_day_pred(TimestampTz x)
{
	if (x == DT_NOEND)
		return END_TIMESTAMP - 1;
	if (x <= MIN_TIMESTAMP)
		return DT_NOBEGIN;
	return x - 1;
}

/*
 * Add [lo, hi], at or past the set's last span, its ends drawn in to the
 * points a timestamptz holds, joined to the last span where they touch.
 */
static void
lion_day_add(LionDaySpans *s, TimestampTz lo, TimestampTz hi)
{
	if (lo != DT_NOBEGIN && lo < MIN_TIMESTAMP)
		lo = MIN_TIMESTAMP;
	if (lo != DT_NOEND && lo >= END_TIMESTAMP)
		lo = DT_NOEND;
	if (hi != DT_NOEND && hi >= END_TIMESTAMP)
		hi = END_TIMESTAMP - 1;
	if (hi != DT_NOBEGIN && hi < MIN_TIMESTAMP)
		hi = DT_NOBEGIN;
	if (lo > hi)
		return;
	if (s->n > 0 && (s->hi[s->n - 1] == DT_NOEND ||
					 lo <= lion_day_succ(s->hi[s->n - 1])))
	{
		if (hi > s->hi[s->n - 1])
			s->hi[s->n - 1] = hi;
		return;
	}
	if (s->n == LION_DAY_SPANS)
		elog(ERROR, "a lion day range of more than %d spans", LION_DAY_SPANS);
	s->lo[s->n] = lo;
	s->hi[s->n] = hi;
	s->n++;
}

static void
lion_day_not(const LionDaySpans *s, LionDaySpans *out)
{
	TimestampTz start = DT_NOBEGIN;
	int			i;

	out->n = 0;
	for (i = 0; i < s->n; i++)
	{
		if (s->lo[i] > start)
			lion_day_add(out, start, lion_day_pred(s->lo[i]));
		if (s->hi[i] == DT_NOEND)
			return;
		start = lion_day_succ(s->hi[i]);
	}
	lion_day_add(out, start, DT_NOEND);
}

static void
lion_day_and(const LionDaySpans *a, const LionDaySpans *b, LionDaySpans *out)
{
	int			i = 0;
	int			j = 0;

	out->n = 0;
	while (i < a->n && j < b->n)
	{
		TimestampTz lo = Max(a->lo[i], b->lo[j]);
		TimestampTz hi = Min(a->hi[i], b->hi[j]);

		if (lo <= hi)
			lion_day_add(out, lo, hi);
		if (a->hi[i] < b->hi[j])
			i++;
		else
			j++;
	}
}

/*
 * A threshold of local time: the instants whose local time is it or later.
 * LION_DAY_ALL takes every instant, -infinity too; LION_DAY_FINITE those of
 * local time p or later, which always takes infinity and never -infinity;
 * LION_DAY_INF infinity alone, and LION_DAY_NONE none.
 */
typedef enum LionDayFrom
{
	LION_DAY_ALL,
	LION_DAY_FINITE,
	LION_DAY_INF,
	LION_DAY_NONE
} LionDayFrom;

/* a pg_time_t's second as a timestamp, and the second a timestamp is in */
static pg_time_t
lion_day_secs(TimestampTz t)
{
	int64		s = t / USECS_PER_SEC;

	if (t % USECS_PER_SEC < 0)
		s--;
	return (pg_time_t) s + LION_DAY_EPOCH_SECS;
}

/*
 * The instants whose local time under the session's zone is p or later.
 * Outside a window of eight days around p every instant's local time is on
 * one side of it, as no offset reaches a week; inside, each stretch between
 * two of the zone's transitions has one offset, and takes the instants from
 * p less it on.
 */
static void
lion_day_from(LionDayFrom kind, int64 p, LionDaySpans *out)
{
	TimestampTz wlo;
	TimestampTz whi;
	TimestampTz cur;
	int			nseg = 0;

	out->n = 0;
	if (kind == LION_DAY_NONE)
		return;
	if (kind == LION_DAY_ALL)
	{
		lion_day_add(out, DT_NOBEGIN, DT_NOEND);
		return;
	}
	if (kind == LION_DAY_INF ||
		p > END_TIMESTAMP + LION_DAY_MAXOFF * USECS_PER_SEC)
	{
		lion_day_add(out, DT_NOEND, DT_NOEND);
		return;
	}
	if (p < MIN_TIMESTAMP - LION_DAY_MAXOFF * USECS_PER_SEC)
	{
		lion_day_add(out, MIN_TIMESTAMP, DT_NOEND);
		return;
	}
	wlo = Max(MIN_TIMESTAMP, p - LION_DAY_WINDOW);
	whi = (p >= END_TIMESTAMP - LION_DAY_WINDOW) ? END_TIMESTAMP :
		p + LION_DAY_WINDOW;
	cur = wlo;
	while (cur < whi)
	{
		pg_time_t	secs = lion_day_secs(cur);
		pg_time_t	bound;
		long int	before_off;
		long int	after_off;
		int			before_isdst;
		int			after_isdst;
		TimestampTz segend = whi;
		TimestampTz thr;
		int			r;

		if (++nseg > 64)
			elog(ERROR, "time zone \"%s\" steps too often for a lion day range",
				 pg_get_timezone_name(session_timezone));
		r = pg_next_dst_boundary(&secs, &before_off, &before_isdst, &bound,
								 &after_off, &after_isdst, session_timezone);
		if (r < 0)
			elog(ERROR, "could not read the transitions of time zone \"%s\"",
				 pg_get_timezone_name(session_timezone));
		if (before_off >= LION_DAY_MAXOFF || before_off <= -LION_DAY_MAXOFF)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("time zone \"%s\" is a week or more from UTC, which a lion day range does not support",
							pg_get_timezone_name(session_timezone)),
					 errhint("Set pg_lion.enable_date_ranges to off.")));
		if (r == 1 && bound - LION_DAY_EPOCH_SECS <= whi / USECS_PER_SEC)
			segend = (TimestampTz) (bound - LION_DAY_EPOCH_SECS) * USECS_PER_SEC;
		if (segend <= cur)
			elog(ERROR, "time zone \"%s\" has a transition out of order",
				 pg_get_timezone_name(session_timezone));
		thr = p - (int64) before_off * USECS_PER_SEC;
		if (Max(cur, thr) < segend)
			lion_day_add(out, Max(cur, thr), segend - 1);
		cur = segend;
	}
	lion_day_add(out, whi == END_TIMESTAMP ? DT_NOEND : whi, DT_NOEND);
}

/* the local time of an instant, as timestamp2tm() makes its fields */
static bool
lion_day_local(TimestampTz t, int64 *local)
{
	struct pg_tm tm;
	fsec_t		fsec;
	int			tz;

	if (TIMESTAMP_NOT_FINITE(t) ||
		timestamp2tm(t, &tz, &tm, &fsec, NULL, session_timezone) != 0)
		return false;
	*local = t - (int64) tz * USECS_PER_SEC;
	return true;
}

/*
 * A bucket's instant, as timestamptz_trunc_internal() reads it back in the
 * session's zone (DetermineTimeZoneOffset(), then tm2timestamp()'s
 * arithmetic): below or above every instant where it has none.
 */
static int
lion_day_instant(const LionDayMark *m, TimestampTz *t)
{
	struct pg_tm tm = m->tm;
	int			tz;

	if (m->where != LION_DAY_AT)
		return m->where;
	tz = DetermineTimeZoneOffset(&tm, session_timezone);
	if (pg_add_s64_overflow(m->at, (int64) tz * USECS_PER_SEC, t))
		return tz < 0 ? LION_DAY_BELOW : LION_DAY_ABOVE;
	return LION_DAY_AT;
}

/* does a bucket's instant come at c or later (strict: after c)? */
static bool
lion_day_mark_reaches(int where, TimestampTz t, TimestampTz c, bool strict)
{
	if (where == LION_DAY_BELOW)
		return false;
	if (where == LION_DAY_ABOVE)
		return true;
	return strict ? t > c : t >= c;
}

/*
 * The thresholds of date_trunc(unit, ts) against a finite c: the first
 * bucket whose instant is c or later (*ge) and after c (*gt).  The walk
 * starts at a bucket whose instant is surely before c - fifteen days of
 * local time before c's own, as no offset reaches a week - and checks that
 * the buckets' instants never fall.
 */
static void
lion_day_trunc_marks(int val, TimestampTz c, LionDayFrom *gekind, int64 *ge,
					 LionDayFrom *gtkind, int64 *gt)
{
	int64		lc;
	int64		start;
	LionDayMark m;
	bool		havege = false;
	int			prevwhere = LION_DAY_BELOW;
	TimestampTz prev = 0;
	int			i;

	if (!lion_day_local(c, &lc))
		elog(ERROR, "could not read the local time of a lion day range's bound");
	start = lc - INT64CONST(15) * USECS_PER_DAY;
	if (start < MIN_TIMESTAMP - INT64CONST(20) * USECS_PER_DAY)
		start = MIN_TIMESTAMP - INT64CONST(20) * USECS_PER_DAY;
	lion_day_mark_of(val, start, &m);
	for (i = 0; i < 64; i++)
	{
		TimestampTz t = 0;
		int			where = lion_day_instant(&m, &t);

		if (where < prevwhere ||
			(where == LION_DAY_AT && prevwhere == LION_DAY_AT && t < prev))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("time zone \"%s\" steps by more than a day, which a lion day range does not support",
							pg_get_timezone_name(session_timezone)),
					 errhint("Set pg_lion.enable_date_ranges to off.")));
		if (i == 0 && lion_day_mark_reaches(where, t, c, false))
			elog(ERROR, "a lion day range's walk started too late");
		if (!havege && lion_day_mark_reaches(where, t, c, false))
		{
			havege = true;
			if (m.where == LION_DAY_ABOVE)
				*gekind = LION_DAY_INF;
			else if (m.where == LION_DAY_BELOW)
			{
				*gekind = LION_DAY_FINITE;
				*ge = MIN_TIMESTAMP - LION_DAY_WINDOW;
			}
			else
			{
				*gekind = LION_DAY_FINITE;
				*ge = m.at;
			}
		}
		if (lion_day_mark_reaches(where, t, c, true))
		{
			if (m.where == LION_DAY_ABOVE)
				*gtkind = LION_DAY_INF;
			else if (m.where == LION_DAY_BELOW)
			{
				*gtkind = LION_DAY_FINITE;
				*gt = MIN_TIMESTAMP - LION_DAY_WINDOW;
			}
			else
			{
				*gtkind = LION_DAY_FINITE;
				*gt = m.at;
			}
			return;
		}
		prevwhere = where;
		prev = t;
		lion_day_mark_next(val, &m);
	}
	elog(ERROR, "a lion day range's walk found no bucket past its bound");
}

/*
 * The thresholds of `f(ts) >= c` and `f(ts) > c`: f the cast to date (val 0,
 * c a date) or date_trunc(val, ts) (c a timestamptz).
 */
static void
lion_day_thresholds(int val, Datum c, LionDayFrom *gekind, int64 *ge,
					LionDayFrom *gtkind, int64 *gt)
{
	*ge = *gt = 0;
	if (val == 0)
	{
		DateADT		d = DatumGetDateADT(c);

		if (DATE_IS_NOBEGIN(d))
		{
			*gekind = LION_DAY_ALL;
			*gtkind = LION_DAY_FINITE;
			*gt = MIN_TIMESTAMP - LION_DAY_WINDOW;
		}
		else if (DATE_IS_NOEND(d))
		{
			*gekind = LION_DAY_INF;
			*gtkind = LION_DAY_NONE;
		}
		else
		{
			int64		days[2] = {(int64) d, (int64) d + 1};
			int64	   *out[2] = {ge, gt};
			LionDayFrom *kinds[2] = {gekind, gtkind};
			int			k;

			/* a midnight a week past either end is as good as any further */
			for (k = 0; k < 2; k++)
			{
				if (days[k] < LION_DAY_MIN_DAYS - 8)
				{
					*kinds[k] = LION_DAY_FINITE;
					*out[k] = MIN_TIMESTAMP - LION_DAY_WINDOW;
				}
				else if (days[k] > LION_DAY_END_DAYS + 7)
					*kinds[k] = LION_DAY_INF;
				else
				{
					*kinds[k] = LION_DAY_FINITE;
					*out[k] = days[k] * USECS_PER_DAY;
				}
			}
		}
	}
	else
	{
		TimestampTz t = DatumGetTimestampTz(c);

		if (TIMESTAMP_IS_NOBEGIN(t))
		{
			*gekind = LION_DAY_ALL;
			*gtkind = LION_DAY_FINITE;
			*gt = MIN_TIMESTAMP - LION_DAY_WINDOW;
		}
		else if (TIMESTAMP_IS_NOEND(t))
		{
			*gekind = LION_DAY_INF;
			*gtkind = LION_DAY_NONE;
		}
		else
			lion_day_trunc_marks(val, t, gekind, ge, gtkind, gt);
	}
}

/*
 * f(t) <op> c computed as PostgreSQL computes it, or false where computing
 * it could raise an error: the cast's date of the local time
 * (timestamptz_date()), date_trunc() itself two thousand years inside the
 * range.  For lion_day_check().
 */
static bool
lion_day_eval(int val, Datum c, int strategy, TimestampTz t, bool *result)
{
	int			cmp;

	if (val == 0)
	{
		DateADT		d = DatumGetDateADT(c);
		DateADT		dt;
		int64		local;
		int64		day;

		if (!lion_day_local(t, &local))
			return false;
		day = local / USECS_PER_DAY;
		if (local % USECS_PER_DAY < 0)
			day--;
		dt = (DateADT) day;
		cmp = (dt < d) ? -1 : (dt > d) ? 1 : 0;
	}
	else
	{
		const int64 margin = INT64CONST(2000) * 366 * USECS_PER_DAY;
		static const char *const names[] = {"day", "week", "month", "quarter",
		"year", "decade", "century", "millennium"};
		static const int vals[] = {DTK_DAY, DTK_WEEK, DTK_MONTH, DTK_QUARTER,
		DTK_YEAR, DTK_DECADE, DTK_CENTURY, DTK_MILLENNIUM};
		TimestampTz f;
		TimestampTz cc = DatumGetTimestampTz(c);
		int			k;

		if (TIMESTAMP_NOT_FINITE(t) || t < MIN_TIMESTAMP + margin ||
			t > END_TIMESTAMP - margin)
			return false;
		for (k = 0; k < (int) lengthof(vals); k++)
			if (vals[k] == val)
				break;
		if (k == (int) lengthof(vals))
			return false;
		f = DatumGetTimestampTz(DirectFunctionCall2(timestamptz_trunc,
													CStringGetTextDatum(names[k]),
													TimestampTzGetDatum(t)));
		cmp = timestamp_cmp_internal(f, cc);
	}
	switch (strategy)
	{
		case BTLessStrategyNumber:
			*result = cmp < 0;
			break;
		case BTLessEqualStrategyNumber:
			*result = cmp <= 0;
			break;
		case BTEqualStrategyNumber:
			*result = cmp == 0;
			break;
		case BTGreaterEqualStrategyNumber:
			*result = cmp >= 0;
			break;
		default:
			*result = cmp > 0;
			break;
	}
	return true;
}

/*
 * Check each span's finite ends against the clause itself: true at the end,
 * false just past it, wherever both can be computed.
 */
static void
lion_day_check(int val, Datum c, int strategy, const LionDaySpans *s)
{
	int			i;

	for (i = 0; i < s->n; i++)
	{
		TimestampTz ends[2] = {s->lo[i], s->hi[i]};
		TimestampTz outs[2] = {lion_day_pred(s->lo[i]), lion_day_succ(s->hi[i])};
		int			k;

		for (k = 0; k < 2; k++)
		{
			bool		in;
			bool		out;

			if (TIMESTAMP_NOT_FINITE(ends[k]) || TIMESTAMP_NOT_FINITE(outs[k]))
				continue;
			if (!lion_day_eval(val, c, strategy, ends[k], &in) ||
				!lion_day_eval(val, c, strategy, outs[k], &out))
				continue;
			if (!in || out)
				elog(ERROR, "a lion day range's edge " INT64_FORMAT " disagrees with its clause in time zone \"%s\"",
					 ends[k], pg_get_timezone_name(session_timezone));
		}
	}
}

/* The instants of `f(ts) <strategy> c`, as spans. */
static void
lion_day_spans(int val, Datum c, int strategy, LionDaySpans *out)
{
	LionDayFrom gekind;
	LionDayFrom gtkind;
	int64		ge;
	int64		gt;
	LionDaySpans a;
	LionDaySpans b;
	LionDaySpans nb;

	lion_day_thresholds(val, c, &gekind, &ge, &gtkind, &gt);
	switch (strategy)
	{
		case BTGreaterEqualStrategyNumber:
			lion_day_from(gekind, ge, out);
			break;
		case BTGreaterStrategyNumber:
			lion_day_from(gtkind, gt, out);
			break;
		case BTLessStrategyNumber:
			lion_day_from(gekind, ge, &a);
			lion_day_not(&a, out);
			break;
		case BTLessEqualStrategyNumber:
			lion_day_from(gtkind, gt, &a);
			lion_day_not(&a, out);
			break;
		default:
			lion_day_from(gekind, ge, &a);
			lion_day_from(gtkind, gt, &b);
			lion_day_not(&b, &nb);
			lion_day_and(&a, &nb, out);
			break;
	}
	lion_day_check(val, c, strategy, out);
	if (out->n > 2)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a day of time zone \"%s\" is more than two ranges of time, which a lion day range does not support",
						pg_get_timezone_name(session_timezone)),
				 errhint("Set pg_lion.enable_date_ranges to off.")));
}

/*
 * The spans, kept per call site: the bound is the same for every row the
 * scan tests until an argument or the session's zone changes.
 */
typedef struct LionDayCache
{
	bool		valid;
	pg_tz	   *tz;
	int			val;
	int			strategy;
	int64		c;				/* the date, or the timestamptz */
	LionDaySpans spans;
} LionDayCache;

static const LionDaySpans *
lion_day_cached(FunctionCallInfo fcinfo, int val, Datum c, int strategy)
{
	LionDayCache *cache = (LionDayCache *) fcinfo->flinfo->fn_extra;
	int64		key = (val == 0) ? (int64) DatumGetDateADT(c) :
		(int64) DatumGetTimestampTz(c);

	if (cache == NULL)
	{
		cache = MemoryContextAllocZero(fcinfo->flinfo->fn_mcxt,
									   sizeof(LionDayCache));
		fcinfo->flinfo->fn_extra = cache;
	}
	if (!cache->valid || cache->tz != session_timezone || cache->val != val ||
		cache->strategy != strategy || cache->c != key)
	{
		cache->valid = false;
		lion_day_spans(val, c, strategy, &cache->spans);
		cache->tz = session_timezone;
		cache->val = val;
		cache->strategy = strategy;
		cache->c = key;
		cache->valid = true;
	}
	return &cache->spans;
}

/*
 * Piece n's first or last instant.  A piece the set has none for is empty,
 * [p, p - 1 microsecond], p beside the first piece (or anywhere for an empty
 * set): the planner prices such a range at next to no rows.
 */
static TimestampTz
lion_day_piece(const LionDaySpans *s, int piece, bool hi)
{
	TimestampTz p;

	if (piece < 1 || piece > 2)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a lion day range has pieces 1 and 2, not %d", piece)));
	if (piece <= s->n)
		return hi ? s->hi[piece - 1] : s->lo[piece - 1];
	p = MIN_TIMESTAMP + 1;
	if (s->n > 0)
	{
		if (!TIMESTAMP_NOT_FINITE(s->lo[0]) && s->lo[0] > MIN_TIMESTAMP)
			p = s->lo[0];
		else if (!TIMESTAMP_NOT_FINITE(s->hi[0]) && s->hi[0] < END_TIMESTAMP - 1)
			p = s->hi[0] + 1;
	}
	return hi ? p - 1 : p;
}

static Datum
lion_day_tz(FunctionCallInfo fcinfo, int val, Datum c, text *op, int32 piece,
			bool hi)
{
	int			strategy = lion_day_op_of(op);
	const LionDaySpans *s = lion_day_cached(fcinfo, val, c, strategy);

	PG_RETURN_TIMESTAMPTZ(lion_day_piece(s, piece, hi));
}

/* lion_day_lo(d date, op text, piece int4) and lion_day_hi(d, op, piece) */
Datum
lion_day_lo_date(PG_FUNCTION_ARGS)
{
	return lion_day_tz(fcinfo, 0, PG_GETARG_DATUM(0), PG_GETARG_TEXT_PP(1),
					   PG_GETARG_INT32(2), false);
}

Datum
lion_day_hi_date(PG_FUNCTION_ARGS)
{
	return lion_day_tz(fcinfo, 0, PG_GETARG_DATUM(0), PG_GETARG_TEXT_PP(1),
					   PG_GETARG_INT32(2), true);
}

/*
 * lion_day_lo(unit text, c timestamptz, op text, piece int4) and
 * lion_day_hi(unit, c, op, piece), for a unit of a day or more
 */
static int
lion_day_whole_unit_of(text *unit)
{
	int			val = lion_day_unit_of(unit);

	if (!lion_day_unit_whole_days(val))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unit \"%s\" not supported by a lion day range of timestamp with time zone",
						text_to_cstring(unit))));
	return val;
}

Datum
lion_day_lo_trunc(PG_FUNCTION_ARGS)
{
	int			val = lion_day_whole_unit_of(PG_GETARG_TEXT_PP(0));

	return lion_day_tz(fcinfo, val, PG_GETARG_DATUM(1), PG_GETARG_TEXT_PP(2),
					   PG_GETARG_INT32(3), false);
}

Datum
lion_day_hi_trunc(PG_FUNCTION_ARGS)
{
	int			val = lion_day_whole_unit_of(PG_GETARG_TEXT_PP(0));

	return lion_day_tz(fcinfo, val, PG_GETARG_DATUM(1), PG_GETARG_TEXT_PP(2),
					   PG_GETARG_INT32(3), true);
}
