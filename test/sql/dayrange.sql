-- Day filters as ranges (DESIGN.md §54): ts::date and date_trunc(unit, ts) of
-- a timestamp column with a lion index, compared with a constant, planned as
-- the range of ts they are.
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
		IF ln ~ '(date_trunc|::date)' THEN
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
	'timestamp ''2000-01-01'' > date_trunc(%L, ts)'
	]) p ORDER BY 1, 2;
-- a constant inside a bucket is equal to no truncated value: left alone
SELECT lion_dr('date_trunc(''day'', ts) = ''2026-03-05 12:00''');
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
	'(ts::date = ''2026-03-05'') IS NOT TRUE',
	'(ts::date = ''2026-03-05'') IS NULL',
	'k = 2 AND (date_trunc(''month'', ts) = ''2026-03-01'' OR date_trunc(''year'', ts) = ''2030-01-01'')',
	'CASE WHEN k = 1 THEN ts::date = ''2026-03-05'' ELSE true END'
]) p;

-- ---- left alone ----
SELECT p, lion_dr(p) FROM unnest(ARRAY[
	-- infinite constants
	'ts::date = ''infinity''',
	'ts::date < ''-infinity''',
	'date_trunc(''day'', ts) >= ''infinity''',
	-- a date past timestamp''s own range: the bound has no timestamp
	'ts::date < ''300000-01-01''',
	'ts::date > ''4713-01-01 BC''',
	-- an IN list, a cross-type comparison, a cast of the truncated value
	'ts::date IN (''2026-03-05'', ''2026-03-06'')',
	'ts::date = timestamp ''2026-03-05''',
	'date_trunc(''day'', ts)::date = ''2026-03-05'''
]) p;
-- not a constant: a unit, a bound
SELECT lion_dr('date_trunc(CASE WHEN k > 2 THEN ''month'' ELSE ''day'' END, ts) = ''2026-03-01''');
SELECT lion_dr('ts::date = (''2026-03-05''::date + k)');

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

-- ---- a generic plan's parameter is not a constant: left alone ----
PREPARE lion_dr_q(date) AS SELECT count(*) FROM lion_dr_t WHERE ts::date = $1;
SET plan_cache_mode = force_generic_plan;
EXECUTE lion_dr_q('2026-03-05');
SET plan_cache_mode = force_custom_plan;
EXECUTE lion_dr_q('2026-03-05');
RESET plan_cache_mode;
DEALLOCATE lion_dr_q;

DROP TABLE lion_dr_t, lion_dr_o, lion_dr_e, lion_dr_b, lion_dr_p;
DROP FUNCTION lion_dr(text, text);
DROP FUNCTION lion_dr_h(int8, int8);
