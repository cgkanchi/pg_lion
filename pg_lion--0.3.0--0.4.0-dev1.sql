/*
 * pg_lion 0.3.0 to 0.4.0-dev1: the bounds of a day filter's ranges that are
 * known only as the query runs.
 */

\echo Use "ALTER EXTENSION pg_lion UPDATE TO '0.4.0-dev1'" to load this file. \quit

/* ---------------------------------------------------------------------
 * bounds of a day filter's ranges (lion_daybounds.c, DESIGN.md §54, "Bounds
 * known when the query runs")
 *
 * The planner writes `ts::date OP c` and `date_trunc(unit, ts) OP c` over a
 * lion-indexed timestamp or timestamptz column as ranges of ts.  Where the
 * bounds are known only as the query runs - c a generic plan's $1 or
 * current_date, or ts a timestamptz, whose day is the session's TimeZone's -
 * they are calls of these, which an index qual evaluates once per scan:
 *
 *	lion_day_first(c), lion_day_last(c)
 *		the first and last timestamp whose date is c;
 *	lion_day_first(unit, c), lion_day_last(unit, c)
 *		the first timestamp whose date_trunc(unit, ...) is c or later, and the
 *		last whose is c or earlier;
 *	lion_day_lo(c, op, piece), lion_day_hi(c, op, piece)
 *	lion_day_lo(unit, c, op, piece), lion_day_hi(unit, c, op, piece)
 *		the first and last instant of piece 1 or 2 of the timestamptz
 *		instants whose day (or bucket) compares with c as op does, in the
 *		session's TimeZone: one range nearly everywhere, a second across a
 *		step of the clocks back over midnight, and an empty piece [p, p - 1
 *		microsecond] where there is none.
 *
 * None raises an error for an argument such a clause accepts: a bound past
 * what the type holds is its infinity or the end of its range.
 * --------------------------------------------------------------------- */

CREATE FUNCTION lion_day_first(d date)
RETURNS timestamp
AS 'MODULE_PATHNAME', 'lion_day_first_date'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

COMMENT ON FUNCTION lion_day_first(date) IS
	'the first timestamp whose date is d: the lower bound of ts::date = d as a range';

CREATE FUNCTION lion_day_last(d date)
RETURNS timestamp
AS 'MODULE_PATHNAME', 'lion_day_last_date'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

COMMENT ON FUNCTION lion_day_last(date) IS
	'the last timestamp whose date is d: the upper bound of ts::date = d as a range';

CREATE FUNCTION lion_day_first(unit text, c timestamp)
RETURNS timestamp
AS 'MODULE_PATHNAME', 'lion_day_first_trunc'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

COMMENT ON FUNCTION lion_day_first(text, timestamp) IS
	'the first timestamp whose date_trunc(unit, ...) is c or later: the lower bound of date_trunc(unit, ts) >= c as a range';

CREATE FUNCTION lion_day_last(unit text, c timestamp)
RETURNS timestamp
AS 'MODULE_PATHNAME', 'lion_day_last_trunc'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

COMMENT ON FUNCTION lion_day_last(text, timestamp) IS
	'the last timestamp whose date_trunc(unit, ...) is c or earlier: the upper bound of date_trunc(unit, ts) <= c as a range';

CREATE FUNCTION lion_day_lo(d date, op text, piece int4)
RETURNS timestamptz
AS 'MODULE_PATHNAME', 'lion_day_lo_date'
LANGUAGE C STABLE STRICT PARALLEL SAFE;

COMMENT ON FUNCTION lion_day_lo(date, text, int4) IS
	'the first instant of piece 1 or 2 of the timestamptz values whose date in the session''s TimeZone compares with d as op does';

CREATE FUNCTION lion_day_hi(d date, op text, piece int4)
RETURNS timestamptz
AS 'MODULE_PATHNAME', 'lion_day_hi_date'
LANGUAGE C STABLE STRICT PARALLEL SAFE;

COMMENT ON FUNCTION lion_day_hi(date, text, int4) IS
	'the last instant of piece 1 or 2 of the timestamptz values whose date in the session''s TimeZone compares with d as op does';

CREATE FUNCTION lion_day_lo(unit text, c timestamptz, op text, piece int4)
RETURNS timestamptz
AS 'MODULE_PATHNAME', 'lion_day_lo_trunc'
LANGUAGE C STABLE STRICT PARALLEL SAFE;

COMMENT ON FUNCTION lion_day_lo(text, timestamptz, text, int4) IS
	'the first instant of piece 1 or 2 of the timestamptz values whose date_trunc(unit, ...) in the session''s TimeZone compares with c as op does';

CREATE FUNCTION lion_day_hi(unit text, c timestamptz, op text, piece int4)
RETURNS timestamptz
AS 'MODULE_PATHNAME', 'lion_day_hi_trunc'
LANGUAGE C STABLE STRICT PARALLEL SAFE;

COMMENT ON FUNCTION lion_day_hi(text, timestamptz, text, int4) IS
	'the last instant of piece 1 or 2 of the timestamptz values whose date_trunc(unit, ...) in the session''s TimeZone compares with c as op does';
