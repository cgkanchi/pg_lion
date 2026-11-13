-- Day filters as ranges over a timestamptz column (DESIGN.md §54, "Bounds
-- known when the query runs"): tz::date and date_trunc(unit, tz) for units of
-- a day or more, whose values are those of the session's TimeZone when the
-- query runs, planned as unions of ranges of tz whose bounds are computed then.
--
-- Every predicate is run with the rewrite on and through lion, and with it off
-- and sequential scans only, in each of a dozen zones - among them the ones
-- whose clocks stepped back across midnight (St. John's 1987 to 2010, Havana,
-- Tehran) or skipped a whole day (Apia 2011, Kiritimati 1994) or repeated one
-- (Sitka 1867) - and the rows (ids) must be the same.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
SET synchronous_commit = on;
SET TimeZone = 'UTC';

CREATE FUNCTION lion_drz_h(i int8, seed int8) RETURNS int8
LANGUAGE sql IMMUTABLE PARALLEL SAFE
AS 'SELECT hashint8extended(i, seed) & 9223372036854775807';

/*
 * The instants the zones' clocks change at, or near, which the rows crowd
 * around: each zone's are tested with predicates about the days and buckets
 * around them.
 */
CREATE TABLE lion_drz_anchor (zone text, at timestamptz);
INSERT INTO lion_drz_anchor VALUES
	('UTC', '2026-03-05 13:07:42.123456+00'),
	('America/New_York', '2026-03-08 07:00+00'), ('America/New_York', '2026-11-01 06:00+00'),
	('America/St_Johns', '2005-10-30 02:31+00'), ('America/St_Johns', '2005-04-03 03:31+00'),
	('America/St_Johns', '2026-11-01 04:30+00'),
	('America/Santiago', '2025-04-06 03:00+00'), ('America/Santiago', '2025-09-07 04:00+00'),
	('America/Havana', '2025-11-02 05:00+00'), ('America/Havana', '2025-03-09 05:00+00'),
	('Asia/Tehran', '2021-09-21 19:30+00'), ('Asia/Tehran', '2021-03-21 20:30+00'),
	('Pacific/Apia', '2011-12-30 10:00+00'),
	('Pacific/Kiritimati', '1994-12-31 10:00+00'),
	('America/Sitka', '1867-10-19 00:31:13+00'),
	('Australia/Lord_Howe', '2025-04-05 15:00+00'), ('Australia/Lord_Howe', '2025-10-04 15:30+00'),
	('Asia/Kolkata', '2026-03-05 13:07:42.123456+00'),
	('Europe/Dublin', '2025-10-26 01:00+00'), ('Europe/Dublin', '2025-03-30 01:00+00'),
	('EST5EDT', '2026-03-08 07:00+00'),
	('<+0330>-3:30', '2026-03-05 13:07:42.123456+00');

/*
 * The rows: 4000 instants spread over 2020 to 2032 at random microseconds;
 * every 7 minutes 13 seconds within a day and a half of each anchor, with the
 * microsecond before and after; the local midnights and first instants of
 * week, month and year around each anchor in its zone, with theirs; BC
 * instants, the infinities, and NULLs.
 */
CREATE TABLE lion_drz_t (id int8, tz timestamptz, k int);
INSERT INTO lion_drz_t
SELECT i, timestamptz '2020-01-01 00:00+00' + (lion_drz_h(i, 1) % (12 * 366 * 86400000000::int8)) * interval '1 microsecond',
	   (lion_drz_h(i, 2) % 4)::int
  FROM generate_series(1, 4000) i;
INSERT INTO lion_drz_t
SELECT 100000 + row_number() OVER (), t, 1
  FROM (SELECT DISTINCT a.at + g * interval '433 seconds' + d AS t
		  FROM lion_drz_anchor a, generate_series(-300, 300) g,
			   unnest(ARRAY[interval '0', '-1 microsecond', '1 microsecond']) d) s;
INSERT INTO lion_drz_t
SELECT 200000 + row_number() OVER (), t, 2
  FROM (SELECT DISTINCT b.t + d AS t
		  FROM (SELECT DISTINCT date_trunc(u, a.at + g * interval '1 day', a.zone) AS t
				  FROM lion_drz_anchor a, generate_series(-2, 2) g,
					   unnest(ARRAY['day', 'week', 'month', 'year']) u) b,
			   unnest(ARRAY[interval '0', '-1 microsecond', '1 microsecond']) d) s;
INSERT INTO lion_drz_t VALUES
	(300001, 'infinity', 1), (300002, '-infinity', 1), (300003, NULL, 1), (300004, NULL, 2),
	(300005, '0044-03-15 12:00+00 BC', 1), (300006, '0001-01-01 00:00+00 BC', 1),
	(300007, '0001-12-31 23:59:59.999999+00 BC', 1);
CREATE INDEX lion_drz_t_tz ON lion_drz_t USING lion (tz);
CREATE INDEX lion_drz_t_k ON lion_drz_t USING lion (k);
VACUUM ANALYZE lion_drz_t;

CREATE FUNCTION lion_drz(pred text, tab text DEFAULT 'lion_drz_t') RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	how text := 'as a range';
	q text := format('SELECT id FROM %s WHERE %s', tab, pred);
	a text;
	b text;
	n bigint;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ '(date_trunc\(.*, tz(, [^)]*)?\)|\(tz\)::date)' THEN
			how := 'as written';
		END IF;
	END LOOP;
	EXECUTE format('SELECT count(*), md5(coalesce(string_agg(id::text, '','' ORDER BY id), '''')) FROM (%s) s', q)
		INTO n, a;

	PERFORM set_config('pg_lion.enable_date_ranges', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE format('SELECT md5(coalesce(string_agg(id::text, '','' ORDER BY id), '''')) FROM (%s) s', q)
		INTO b;
	PERFORM set_config('pg_lion.enable_date_ranges', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	IF a <> b THEN
		RETURN format('MISMATCH (%s rows)', n);
	END IF;
	RETURN format('%s, %s rows', how, n);
END $$;

-- ---- the plans ----
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_drz_t WHERE tz::date = '2026-03-05';
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_drz_t WHERE k = 1 AND date_trunc('month', tz) >= '2026-03-01';
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_drz_t WHERE tz::date = current_date;
-- a unit of less than a day, or a zone named in the call, is left as written
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_drz_t WHERE date_trunc('hour', tz) = '2026-03-05 13:00+00';
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_drz_t WHERE date_trunc('day', tz, 'Asia/Kolkata') = '2026-03-05 00:00+05:30';

-- ---- every zone: the cast and date_trunc() of each unit of a day or more,
-- each comparison, with a value on a bucket's edge and inside one, around
-- each of the zone's anchors ----
CREATE FUNCTION lion_drz_zone(z text) RETURNS TABLE (preds int, ranges int, mismatches int, nrows bigint)
LANGUAGE plpgsql AS $$
DECLARE
	p text;
	r text;
BEGIN
	PERFORM set_config('TimeZone', z, true);
	-- values quoted with their offsets, which every version reads back (a
	-- zone's LMT abbreviation is not read before PostgreSQL 18)
	PERFORM set_config('DateStyle', 'ISO, YMD', true);
	preds := 0; ranges := 0; mismatches := 0; nrows := 0;
	FOR p IN
		SELECT DISTINCT format('tz::date %s %L', op, (a.at + g * interval '1 day')::date)
		  FROM lion_drz_anchor a, generate_series(-1, 1) g,
			   unnest(ARRAY['<', '<=', '=', '>=', '>']) op
		 WHERE a.zone = z
		UNION
		SELECT DISTINCT format('date_trunc(%L, tz) %s %L::timestamptz', u, op, c)
		  FROM lion_drz_anchor a,
			   unnest(ARRAY['day', 'week', 'month', 'year', 'decade']) u,
			   LATERAL (VALUES (date_trunc(u, a.at - interval '26 hours')),
							   (date_trunc(u, a.at + interval '26 hours')),
							   (a.at)) cv(c),
			   unnest(ARRAY['<', '<=', '=', '>=', '>']) op
		 WHERE a.zone = z
		ORDER BY 1
	LOOP
		r := lion_drz(p);
		preds := preds + 1;
		IF r ~ '^as a range' THEN
			ranges := ranges + 1;
		END IF;
		IF r ~ 'MISMATCH' THEN
			mismatches := mismatches + 1;
			RAISE WARNING 'zone %: % is %', z, p, r;
		END IF;
		nrows := nrows + substring(r from '([0-9]+) rows')::bigint;
	END LOOP;
	RETURN NEXT;
END $$;
SELECT z, s.* FROM (SELECT DISTINCT zone FROM lion_drz_anchor) a(z), LATERAL lion_drz_zone(z) s ORDER BY z;
SHOW TimeZone;

-- ---- the two-piece days: St. John's stepped back from 00:01 to 23:01 on
-- 2005-10-30, so 2005-10-29 and 2005-10-30 each ran twice ----
SET TimeZone = 'America/St_Johns';
SELECT d, op, lion_day_lo(d, op, 1), lion_day_hi(d, op, 1), lion_day_lo(d, op, 2), lion_day_hi(d, op, 2)
  FROM unnest(ARRAY[date '2005-10-29', '2005-10-30', '2005-10-31']) d,
	   unnest(ARRAY['<', '<=', '=', '>=', '>']) op
 ORDER BY 1, 2;
SELECT p, lion_drz(p) FROM unnest(ARRAY[
	'tz::date = ''2005-10-30''',
	'tz::date = ''2005-10-29''',
	'tz::date >= ''2005-10-30''',
	'tz::date < ''2005-10-30''',
	'date_trunc(''day'', tz) = ''2005-10-30 00:00-02:30''',
	'date_trunc(''day'', tz) = ''2005-10-29 00:00-02:30''',
	'date_trunc(''day'', tz) > ''2005-10-29 12:00-02:30''',
	'date_trunc(''week'', tz) <= ''2005-10-24 00:00-02:30''',
	'date_trunc(''month'', tz) = ''2005-10-01 00:00-02:30'''
]) p;
-- Apia skipped 2011-12-30, and Sitka ran 1867-10-18 twice
SET TimeZone = 'Pacific/Apia';
SELECT d, lion_day_lo(d, '=', 1), lion_day_hi(d, '=', 1), lion_day_lo(d, '=', 2) > lion_day_hi(d, '=', 2) AS one
  FROM unnest(ARRAY[date '2011-12-29', '2011-12-30', '2011-12-31']) d;
SET TimeZone = 'America/Sitka';
SELECT d, lion_day_lo(d, '=', 1), lion_day_hi(d, '=', 1), lion_day_lo(d, '=', 2), lion_day_hi(d, '=', 2)
  FROM unnest(ARRAY[date '1867-10-17', '1867-10-18', '1867-10-19']) d;

-- ---- inside other clauses ----
SET TimeZone = 'America/St_Johns';
SELECT p, lion_drz(p) FROM unnest(ARRAY[
	'NOT (tz::date = ''2005-10-30'')',
	'tz::date = ''2005-10-30'' OR k = 3',
	'(k = 2 AND tz::date = ''2005-10-30'') OR k = 3 OR tz::date = ''2005-10-29''',
	'(k = 2 AND tz::date >= ''2005-10-30'' AND date_trunc(''month'', tz) <= ''2005-11-01 00:00-03:30'') OR (k = 1 AND tz::date = ''2005-10-30'')',
	'(tz::date >= ''2005-10-30'') IS NOT TRUE',
	'k = 2 AND (date_trunc(''month'', tz) = ''2026-03-01 00:00-03:30'' OR tz::date = ''2005-10-29'')',
	'''2005-10-30'' = tz::date',
	'''2005-10-30'' < tz::date',
	'''2005-10-30 00:00-02:30'' >= date_trunc(''day'', tz)'
]) p;

-- ---- infinities, the ends of the type's range, NULL ----
SELECT p, lion_drz(p) FROM unnest(ARRAY[
	'tz::date = ''infinity''',
	'tz::date < ''infinity''',
	'tz::date >= ''-infinity''',
	'tz::date > ''-infinity''',
	'tz::date < ''300000-01-01''',
	'tz::date > ''4713-01-01 BC''',
	'tz::date = ''0044-03-15 BC''',
	'tz::date = NULL',
	'date_trunc(''day'', tz) = ''infinity''',
	'date_trunc(''year'', tz) <= ''-infinity''',
	'date_trunc(''month'', tz) > ''294276-12-01 00:00+00''',
	'date_trunc(''week'', tz) >= ''4713-01-01 00:00+00 BC'''
]) p;

-- ---- a plan made in one zone and run in another: the bounds are the zone's
-- when the query runs, as the clause's own values are ----
SET TimeZone = 'UTC';
PREPARE lion_drz_q(date) AS SELECT count(*) FROM lion_drz_t WHERE tz::date = $1;
PREPARE lion_drz_c AS SELECT count(*) FROM lion_drz_t WHERE tz::date = '2005-10-30';
PREPARE lion_drz_m(timestamptz) AS SELECT count(*) FROM lion_drz_t WHERE date_trunc('day', tz) >= $1;
SET plan_cache_mode = force_generic_plan;
EXPLAIN (COSTS OFF) EXECUTE lion_drz_q('2005-10-30');
EXECUTE lion_drz_q('2005-10-30');
EXECUTE lion_drz_c;
EXECUTE lion_drz_m('2005-10-30 00:00-02:30');
SET TimeZone = 'America/St_Johns';
EXECUTE lion_drz_q('2005-10-30');
EXECUTE lion_drz_c;
EXECUTE lion_drz_m('2005-10-30 00:00-02:30');
SET TimeZone = 'Pacific/Apia';
EXECUTE lion_drz_q('2011-12-30');
EXECUTE lion_drz_q('2011-12-31');
-- ... and the same counts as written
SET pg_lion.enable_date_ranges = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SET pg_lion.enable_count_pushdown = off;
SET TimeZone = 'UTC';
SELECT count(*) FROM lion_drz_t WHERE tz::date = '2005-10-30';
SELECT count(*) FROM lion_drz_t WHERE date_trunc('day', tz) >= '2005-10-30 00:00-02:30';
SET TimeZone = 'America/St_Johns';
SELECT count(*) FROM lion_drz_t WHERE tz::date = '2005-10-30';
SELECT count(*) FROM lion_drz_t WHERE date_trunc('day', tz) >= '2005-10-30 00:00-02:30';
SET TimeZone = 'Pacific/Apia';
SELECT count(*) FROM lion_drz_t WHERE tz::date = '2011-12-30';
SELECT count(*) FROM lion_drz_t WHERE tz::date = '2011-12-31';
RESET pg_lion.enable_date_ranges;
RESET enable_bitmapscan;
RESET enable_indexscan;
RESET pg_lion.enable_count_pushdown;
RESET plan_cache_mode;
DEALLOCATE lion_drz_q;
DEALLOCATE lion_drz_c;
DEALLOCATE lion_drz_m;

-- current_date: the rows depend on the day the test runs
SET TimeZone = 'America/St_Johns';
SELECT p, lion_drz(p) ~ '^as a range, ' AS ok FROM unnest(ARRAY[
	'tz::date = current_date',
	'tz::date >= current_date - 1000',
	'date_trunc(''month'', tz) = date_trunc(''month'', now())'
]) p;

-- ---- a parallel scan's workers run in the leader's zone ----
SET TimeZone = 'America/St_Johns';
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SET pg_lion.enable_count_pushdown = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_drz_t WHERE tz::date = '2005-10-30';
SELECT count(*) FROM lion_drz_t WHERE tz::date = '2005-10-30';
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET max_parallel_workers_per_gather;
RESET enable_bitmapscan;
RESET enable_indexscan;
RESET pg_lion.enable_count_pushdown;

-- ---- the bound functions refuse what no rewrite makes ----
SELECT lion_day_lo(date '2026-03-05', '<>', 1);
SELECT lion_day_lo(date '2026-03-05', '=', 3);
SELECT lion_day_hi('hour', timestamptz '2026-03-05 00:00+00', '=', 1);
SELECT lion_day_hi('fortnight', timestamptz '2026-03-05 00:00+00', '=', 1);

DROP TABLE lion_drz_t, lion_drz_anchor;
DROP FUNCTION lion_drz_zone(text);
DROP FUNCTION lion_drz(text, text);
DROP FUNCTION lion_drz_h(int8, int8);
RESET TimeZone;
