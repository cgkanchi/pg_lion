-- Day filters as ranges (DESIGN.md §54): ts::date and date_trunc(unit, ts) of
-- a timestamp column with a lion index, compared with a constant or a value
-- known when the query runs, planned as the range of ts they are.  A
-- timestamptz column's are in dayrange_tz.sql.
--
-- Every predicate is run with the rewrite on and through lion, and with it off
-- and sequential scans only, and the rows (ids) must be the same.  lion_dr()
-- also says whether the plan kept the function ("as written") or the range.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
SET synchronous_commit = on;

CREATE FUNCTION lion_dr_h(i int8, seed int8) RETURNS int8
LANGUAGE sql IMMUTABLE PARALLEL SAFE
AS 'SELECT hashint8extended(i, seed) & 9223372036854775807';

/*
 * The rows: 6000 timestamps spread over 2020 to 2032 at random microseconds,
 * the instants around each bucket edge of every unit near 2026-03-05 and
 * 2000-01-01 (the edge itself, a microsecond before and after), BC instants,
 * the two infinities, and NULLs.
 */
CREATE TABLE lion_dr_t (id int8, ts timestamp, k int);
INSERT INTO lion_dr_t
SELECT i, timestamp '2020-01-01' + (lion_dr_h(i, 1) % (12 * 366 * 86400000000::int8)) * interval '1 microsecond',
	   (lion_dr_h(i, 2) % 4)::int
  FROM generate_series(1, 6000) i;
INSERT INTO lion_dr_t
SELECT 100000 + row_number() OVER (), e + d, 1
  FROM unnest(ARRAY[
		timestamp '2026-03-05', '2026-03-06', '2026-03-02', '2026-03-09', '2026-03-01', '2026-04-01',
		'2026-01-01', '2026-04-01', '2027-01-01', '2030-01-01', '2101-01-01', '3001-01-01',
		'2026-03-05 13:00', '2026-03-05 13:07', '2026-03-05 13:07:42', '2026-03-05 13:07:42.123',
		'2000-01-01', '1999-12-31', '2001-01-01', '2000-02-29', '1900-01-01', '0001-01-01',
		'0001-01-01 BC', '0044-03-15 BC', '0100-01-01 BC', '0101-01-01 BC']) e,
	   unnest(ARRAY[interval '0', interval '-1 microsecond', interval '1 microsecond']) d;
INSERT INTO lion_dr_t VALUES (200001, 'infinity', 1), (200002, '-infinity', 1),
	(200003, NULL, 1), (200004, NULL, 2);
CREATE INDEX lion_dr_t_ts ON lion_dr_t USING lion (ts);
CREATE INDEX lion_dr_t_k ON lion_dr_t USING lion (k);
VACUUM ANALYZE lion_dr_t;

CREATE FUNCTION lion_dr(pred text, tab text DEFAULT 'lion_dr_t') RETURNS text
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
		IF ln ~ '(date_trunc\(.*, ts\)|\(ts\)::date)' THEN
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
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_dr_t WHERE ts::date = '2026-03-05';
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_dr_t WHERE date_trunc('day', ts) = '2026-03-05';
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_dr_t WHERE k = 1 AND ts::date BETWEEN '2026-03-01' AND '2026-03-07';
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_dr_t WHERE date_trunc('month', ts) >= '2026-03-05';
-- off, the clause stays as written
SET pg_lion.enable_date_ranges = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_dr_t WHERE ts::date = '2026-03-05';
RESET pg_lion.enable_date_ranges;

-- ---- ts::date: every comparison, both ways round ----
SELECT p, lion_dr(p) FROM unnest(ARRAY[
	'ts::date = ''2026-03-05''',
	'ts::date < ''2026-03-05''',
	'ts::date <= ''2026-03-05''',
	'ts::date > ''2026-03-05''',
	'ts::date >= ''2026-03-05''',
	'''2026-03-05'' = ts::date',
	'''2026-03-05'' < ts::date',
	'''2026-03-05'' <= ts::date',
	'''2026-03-05'' > ts::date',
	'''2026-03-05'' >= ts::date',
	'CAST(ts AS date) = ''2000-01-01''',
	'date(ts) = ''1999-12-31''',
	'ts::date BETWEEN ''2026-03-01'' AND ''2026-03-07''',
	'ts::date = ''0044-03-15 BC''',
	'ts::date < ''0001-01-01''',
	'ts::date >= ''0001-01-01 BC'''
]) p;

-- ---- date_trunc(): each unit, a constant on its bucket's edge and inside it ----
SELECT u, p, lion_dr(format(p, u)) FROM unnest(ARRAY[
	'microseconds', 'milliseconds', 'second', 'minute', 'hour', 'day', 'week', 'month',
	'quarter', 'year', 'decade', 'century', 'millennium']) u,
	unnest(ARRAY[
	'date_trunc(%L, ts) = date_trunc(%1$L, timestamp ''2026-03-05 13:07:42.123456'')',
	'date_trunc(%L, ts) < date_trunc(%1$L, timestamp ''2026-03-05 13:07:42.123456'')',
	'date_trunc(%L, ts) <= timestamp ''2026-03-05 13:07:42.123456''',
	'date_trunc(%L, ts) > timestamp ''2026-03-05 13:07:42.123456''',
	'date_trunc(%L, ts) >= timestamp ''2026-03-05 13:07:42.123456''',
	'date_trunc(%L, ts) >= date_trunc(%1$L, timestamp ''2000-01-01'')',
	'date_trunc(%1$L, timestamp ''2026-03-05 13:07:42.123456'') = date_trunc(%1$L, ts)',
	'date_trunc(%1$L, timestamp ''2026-03-05 13:07:42.123456'') < date_trunc(%1$L, ts)',
	'timestamp ''2026-03-05 13:07:42.123456'' <= date_trunc(%L, ts)',
	'timestamp ''2000-01-01'' > date_trunc(%L, ts)',
	'timestamp ''2026-03-05 13:07:42.123456'' >= date_trunc(%L, ts)'
	]) p ORDER BY 1, 2;
-- a constant inside a bucket is equal to no truncated value: an empty range
SELECT lion_dr('date_trunc(''day'', ts) = ''2026-03-05 12:00''');
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_dr_t WHERE date_trunc('day', ts) = '2026-03-05 12:00';
-- the unit's other spellings
SELECT lion_dr('date_trunc(''DAY'', ts) = ''2026-03-05''');
SELECT lion_dr('date_trunc(''days'', ts) >= ''2026-03-05''');
-- BC buckets
SELECT u, lion_dr(format('date_trunc(%L, ts) = date_trunc(%1$L, timestamp ''0044-03-15 BC'')', u))
  FROM unnest(ARRAY['day', 'month', 'year', 'decade', 'century', 'millennium']) u;

-- ---- inside other clauses: the range is the clause's value, NULL included ----
-- (a NOT of = is <> by now, which is no range, and left alone)
SELECT p, lion_dr(p) FROM unnest(ARRAY[
	'NOT (ts::date = ''2026-03-05'')',
	'ts::date = ''2026-03-05'' OR k = 3',
	'(k = 2 AND ts::date = ''2026-03-05'') OR k = 3',
	'(k = 2 AND date_trunc(''month'', ts) >= ''2026-03-01'' AND ts::date <= ''2026-03-05'') OR (k = 1 AND ts::date = ''2026-03-05'')',
	'NOT (k = 2 OR ts::date = ''2026-03-05'' OR ts::date >= ''2030-01-01'')',
	'(ts::date = ''2026-03-05'') IS NOT TRUE',
	'(ts::date = ''2026-03-05'') IS NULL',
	'k = 2 AND (date_trunc(''month'', ts) = ''2026-03-01'' OR date_trunc(''year'', ts) = ''2030-01-01'')',
	'CASE WHEN k = 1 THEN ts::date = ''2026-03-05'' ELSE true END'
]) p;

-- ---- the edges: infinities, dates past timestamp's range, and constants
-- near its ends, whose bounds are folded from the first and last timestamp of
-- the bucket at or past them ----
SELECT p, lion_dr(p) FROM unnest(ARRAY[
	'ts::date = ''infinity''',
	'ts::date >= ''infinity''',
	'ts::date < ''infinity''',
	'ts::date < ''-infinity''',
	'ts::date <= ''-infinity''',
	'ts::date > ''-infinity''',
	'date_trunc(''day'', ts) >= ''infinity''',
	'date_trunc(''day'', ts) = ''-infinity''',
	'date_trunc(''month'', ts) < ''infinity''',
	'ts::date < ''300000-01-01''',
	'ts::date = ''300000-01-01''',
	'ts::date > ''4713-01-01 BC''',
	'ts::date >= ''4714-11-24 BC''',
	'ts::date <= ''4714-11-24 BC''',
	'date_trunc(''year'', ts) <= ''294276-06-01''',
	'date_trunc(''millennium'', ts) >= ''294001-01-01''',
	'date_trunc(''day'', ts) = ''4714-11-24 00:00:00 BC''',
	'date_trunc(''week'', ts) > ''294276-12-31''',
	'date_trunc(''microseconds'', ts) <= ''294276-12-31 23:59:59.999999'''
]) p;

-- ... and rows at those ends: lion_dr_x has both, lion_dr_xh only the upper
-- (a date_trunc() of a month or more of the first days of 4714 BC is out of
-- range, and the clause written so fails on those rows)
CREATE TABLE lion_dr_x (id int8, ts timestamp);
INSERT INTO lion_dr_x SELECT row_number() OVER (), e FROM unnest(ARRAY[
	timestamp '4714-11-24 00:00:00 BC', '4714-11-24 00:00:00.000001 BC',
	'4714-11-24 23:59:59.999999 BC', '4714-11-25 00:00:00 BC',
	'4714-11-30 23:59:59.999999 BC', '4714-12-01 00:00:00 BC', '2026-03-05',
	'294276-01-01', '294276-12-24 23:59:59.999999', '294276-12-25',
	'294276-12-30 23:59:59.999999', '294276-12-31', '294276-12-31 23:59:59.999999',
	'infinity', '-infinity', NULL]) e;
CREATE INDEX lion_dr_x_ts ON lion_dr_x USING lion (ts);
CREATE TABLE lion_dr_xh AS SELECT * FROM lion_dr_x WHERE NOT ts < '0001-01-01' OR ts IS NULL OR ts = '-infinity';
CREATE INDEX lion_dr_xh_ts ON lion_dr_xh USING lion (ts);
VACUUM ANALYZE lion_dr_x, lion_dr_xh;
SELECT p, lion_dr(p, 'lion_dr_x') FROM unnest(ARRAY[
	'ts::date = ''4714-11-24 BC''',
	'ts::date <= ''4714-11-24 BC''',
	'ts::date > ''4714-11-24 BC''',
	'ts::date < ''4714-11-25 BC''',
	'ts::date >= ''4714-11-25 BC''',
	'ts::date >= ''294276-12-31''',
	'ts::date = ''294276-12-31''',
	'ts::date < ''294276-12-31''',
	'ts::date > ''294277-01-01''',
	'ts::date < ''294277-01-01''',
	'date_trunc(''day'', ts) = ''294276-12-31''',
	'date_trunc(''day'', ts) > ''294276-12-30 12:00''',
	'date_trunc(''day'', ts) <= ''4714-11-24 BC''',
	'date_trunc(''week'', ts) = ''4714-11-24 BC''',
	'date_trunc(''week'', ts) >= ''294276-12-25''',
	'date_trunc(''hour'', ts) < ''4714-11-24 01:00 BC''',
	'date_trunc(''microseconds'', ts) >= ''294276-12-31 23:59:59.999999'''
]) p;
SELECT u, p, lion_dr(format(p, u), 'lion_dr_xh') FROM unnest(ARRAY[
	'month', 'quarter', 'year', 'decade', 'century', 'millennium']) u,
	unnest(ARRAY[
	'date_trunc(%L, ts) = date_trunc(%1$L, timestamp ''294276-12-31'')',
	'date_trunc(%L, ts) >= timestamp ''294276-12-31''',
	'date_trunc(%L, ts) < date_trunc(%1$L, timestamp ''294276-12-31'')',
	'date_trunc(%L, ts) > timestamp ''294276-01-01'''
	]) p ORDER BY 1, 2;
DROP TABLE lion_dr_x, lion_dr_xh;

-- ---- left alone ----
SELECT p, lion_dr(p) FROM unnest(ARRAY[
	-- an IN list, a cross-type comparison, a cast of the truncated value
	'ts::date IN (''2026-03-05'', ''2026-03-06'')',
	'ts::date = timestamp ''2026-03-05''',
	'date_trunc(''day'', ts)::date = ''2026-03-05'''
]) p;
-- not a constant: a unit, a bound of another column of the row
SELECT lion_dr('date_trunc(CASE WHEN k > 2 THEN ''month'' ELSE ''day'' END, ts) = ''2026-03-01''');
SELECT lion_dr('ts::date = (''2026-03-05''::date + k)');
-- nor a volatile one
SELECT lion_dr('ts::date = (''2026-03-05''::date + (random() * 0)::int)');

-- ---- a join: the ON clause of either side of an outer join ----
CREATE TABLE lion_dr_o (id int8, d date);
INSERT INTO lion_dr_o SELECT i, date '2026-03-01' + i FROM generate_series(1, 10) i;
SELECT count(*), count(t.id) FROM lion_dr_o o LEFT JOIN lion_dr_t t ON t.ts::date = '2026-03-05' AND t.k = o.id % 4;
SET pg_lion.enable_date_ranges = off;
SELECT count(*), count(t.id) FROM lion_dr_o o LEFT JOIN lion_dr_t t ON t.ts::date = '2026-03-05' AND t.k = o.id % 4;
RESET pg_lion.enable_date_ranges;

-- ---- an expression index on the clause's own function keeps the clause ----
CREATE TABLE lion_dr_e AS SELECT * FROM lion_dr_t;
CREATE INDEX lion_dr_e_ts ON lion_dr_e USING lion (ts);
CREATE INDEX lion_dr_e_day ON lion_dr_e USING lion ((ts::date));
VACUUM ANALYZE lion_dr_e;
SELECT lion_dr('ts::date = ''2026-03-05''', 'lion_dr_e');
SELECT lion_dr('date_trunc(''day'', ts) = ''2026-03-05''', 'lion_dr_e');

-- ---- a table with no lion index on the column is left alone ----
CREATE TABLE lion_dr_b AS SELECT * FROM lion_dr_t;
CREATE INDEX lion_dr_b_ts ON lion_dr_b (ts);
CREATE INDEX lion_dr_b_k ON lion_dr_b USING lion (k);
VACUUM ANALYZE lion_dr_b;
SELECT lion_dr('ts::date = ''2026-03-05''', 'lion_dr_b');

-- ---- a partitioned table, by month: the range prunes too ----
CREATE TABLE lion_dr_p (id int8, ts timestamp, k int) PARTITION BY RANGE (ts);
CREATE TABLE lion_dr_p1 PARTITION OF lion_dr_p FOR VALUES FROM (MINVALUE) TO ('2026-03-01');
CREATE TABLE lion_dr_p2 PARTITION OF lion_dr_p FOR VALUES FROM ('2026-03-01') TO ('2026-04-01');
CREATE TABLE lion_dr_p3 PARTITION OF lion_dr_p FOR VALUES FROM ('2026-04-01') TO (MAXVALUE);
CREATE TABLE lion_dr_p4 PARTITION OF lion_dr_p DEFAULT;
INSERT INTO lion_dr_p SELECT * FROM lion_dr_t;
CREATE INDEX lion_dr_p_ts ON lion_dr_p USING lion (ts);
CREATE INDEX lion_dr_p_k ON lion_dr_p USING lion (k);
VACUUM ANALYZE lion_dr_p;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_dr_p WHERE ts::date = '2026-03-05' AND k = 1;
SELECT lion_dr('ts::date = ''2026-03-05'' AND k = 1', 'lion_dr_p');
SELECT lion_dr('date_trunc(''month'', ts) = ''2026-02-01''', 'lion_dr_p');

-- ---- a value known when the query runs: a generic plan's parameter, a
-- stable expression, a subquery's value - bounds that are calls of
-- lion_day_first() and lion_day_last(), evaluated once ----
PREPARE lion_dr_q(date) AS SELECT count(*) FROM lion_dr_t WHERE ts::date = $1;
PREPARE lion_dr_r(timestamp, date) AS SELECT
	(SELECT count(*) FROM lion_dr_t WHERE date_trunc('day', ts) = $1) AS e1,
	(SELECT count(*) FROM lion_dr_t WHERE date_trunc('month', ts) >= $1) AS ge1,
	(SELECT count(*) FROM lion_dr_t WHERE $1 > date_trunc('week', ts)) AS lt1,
	(SELECT count(*) FROM lion_dr_t WHERE ts::date < $2) AS lt2,
	(SELECT count(*) FROM lion_dr_t WHERE ts::date <= $2) AS le2,
	(SELECT count(*) FROM lion_dr_t WHERE ts::date > $2) AS gt2,
	(SELECT count(*) FROM lion_dr_t WHERE $2 = ts::date) AS e2;
SET plan_cache_mode = force_generic_plan;
EXPLAIN (COSTS OFF) EXECUTE lion_dr_q('2026-03-05');
EXECUTE lion_dr_q('2026-03-05');
EXECUTE lion_dr_q('infinity');
EXECUTE lion_dr_q('-infinity');
EXECUTE lion_dr_q('300000-01-01');
EXECUTE lion_dr_q(NULL);
-- (the InitPlans spelled as PostgreSQL 19 names them)
CREATE FUNCTION lion_dr_plan(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		RETURN NEXT regexp_replace(l, '(InitPlan|SubPlan) (\d+)', '\1 expr_\2', 'g');
	END LOOP;
END
$$;
SELECT * FROM lion_dr_plan($$EXECUTE lion_dr_r('2026-03-05', '2026-03-05')$$) AS p("QUERY PLAN");
CREATE TABLE lion_dr_args (a timestamp, b date);
INSERT INTO lion_dr_args VALUES ('2026-03-05', '2026-03-05'),
	('2026-03-05 12:00', '0044-03-15 BC'), ('infinity', 'infinity'),
	('-infinity', '-infinity'), ('294276-12-31 23:59:59.999999', '4714-11-24 BC'),
	('4714-11-24 00:00:00 BC', '5874897-12-31'), ('2000-01-01', NULL);
CREATE FUNCTION lion_dr_run() RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE r record; x text;
BEGIN
	FOR r IN SELECT * FROM lion_dr_args LOOP
		EXECUTE format('EXECUTE lion_dr_r(%L, %L)', r.a, r.b) INTO x;
		RETURN NEXT format('%s %s: %s', r.a, r.b, x);
	END LOOP;
END $$;
-- generic, custom, and with the rewrite off: the same counts
CREATE TABLE lion_dr_res AS SELECT 'generic' AS how, x FROM lion_dr_run() x;
SET plan_cache_mode = force_custom_plan;
INSERT INTO lion_dr_res SELECT 'custom', x FROM lion_dr_run() x;
SET pg_lion.enable_date_ranges = off;
INSERT INTO lion_dr_res SELECT 'off', x FROM lion_dr_run() x;
RESET pg_lion.enable_date_ranges;
RESET plan_cache_mode;
SELECT x, string_agg(how, ',' ORDER BY how) FROM lion_dr_res GROUP BY x ORDER BY x;
DEALLOCATE lion_dr_q;
DEALLOCATE lion_dr_r;
DROP FUNCTION lion_dr_run();
DROP TABLE lion_dr_args, lion_dr_res;
-- current_date and other stable expressions; the rows depend on the day the
-- test runs, so only whether they match is shown
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_dr_t WHERE ts::date = current_date;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_dr_t WHERE date_trunc('month', ts) = date_trunc('month', localtimestamp);
SELECT p, lion_dr(p) ~ '^as a range, ' AS ok FROM unnest(ARRAY[
	'ts::date = current_date',
	'ts::date >= current_date - 1000',
	'current_date > ts::date',
	'date_trunc(''month'', ts) = date_trunc(''month'', localtimestamp)',
	'date_trunc(''week'', ts) <= localtimestamp - interval ''3 years'''
]) p;
-- a subquery's value
SELECT lion_dr('ts::date = (SELECT max(d) FROM (VALUES (date ''2026-03-05''), (''2026-03-04'')) v(d))');
SELECT lion_dr('date_trunc(''month'', ts) < (SELECT timestamp ''2026-03-05'')');

-- ---- the bound functions, called directly ----
SELECT d, lion_day_first(d), lion_day_last(d)
  FROM unnest(ARRAY[date '2026-03-05', '0044-03-15 BC', 'infinity', '-infinity',
			   '4714-11-24 BC', '294276-12-31', '294277-01-01', '5874897-12-31']) d;
SELECT u, c, lion_day_first(u, c), lion_day_last(u, c)
  FROM unnest(ARRAY['day', 'month', 'week', 'millennium', 'microseconds']) u,
	   unnest(ARRAY[timestamp '2026-03-05', '2026-03-05 12:00', 'infinity', '-infinity',
			   '4714-11-24 00:00:00 BC', '294276-12-31 23:59:59.999999']) c
 ORDER BY 1, 2;
SELECT lion_day_first('fortnight', '2026-03-05');

DROP TABLE lion_dr_t, lion_dr_o, lion_dr_e, lion_dr_b, lion_dr_p;
DROP FUNCTION lion_dr(text, text);
DROP FUNCTION lion_dr_plan(text);
DROP FUNCTION lion_dr_h(int8, int8);
