-- Ranges on bit-sliced columns answered from their slices by the LionCount
-- node (DESIGN.md §53): `x > c`, `x >= c`, `x < c`, `x <= c`, BETWEEN and
-- `x <> c` on a column a lion index slices (§51), folded from the slices into
-- a collected copy (O'Neil's descent), as a count's source and as the filter
-- of §52's aggregates.  Every answer is checked against the ordinary plan
-- (the pushdown off, a sequential scan), and the plan that ran it is shown.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;
SET TimeZone = 'UTC';
SET DateStyle = 'ISO, YMD';

/*
 * slr_q() runs q with the lion paths preferred - every other scan disabled,
 * and a range the slices can answer answered so (pg_lion.prefer_slice_ranges)
 * - then as a sequential scan with the pushdown off, and compares the two as
 * multisets.  It says how the node answered its ranges: from slices, or not
 * (walked or collected from the entries), or that no pushdown ran.
 */
CREATE FUNCTION slr_q(q text, prefer bool DEFAULT true) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	used boolean := false;
	sliced boolean := false;
	aggs boolean := false;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	PERFORM set_config('pg_lion.prefer_slice_ranges', prefer::text, true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(LionCount\)' THEN
			used := true;
		END IF;
		IF ln ~ 'Range From Slices:' THEN
			sliced := true;
		END IF;
		IF ln ~ 'Aggregates From Slices:' THEN
			aggs := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE slr_on AS SELECT s::text AS r FROM (%s) s', q);

	PERFORM set_config('pg_lion.prefer_slice_ranges', 'off', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE format('CREATE TEMP TABLE slr_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM slr_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM slr_on EXCEPT ALL SELECT * FROM slr_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM slr_off EXCEPT ALL SELECT * FROM slr_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE slr_on, slr_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s%s, %s rows',
				  CASE WHEN sliced THEN 'range from slices'
					   WHEN used THEN 'range not from slices'
					   ELSE 'no pushdown' END,
				  CASE WHEN aggs THEN ', aggregates from slices' ELSE '' END,
				  nrows);
END $$;

/*
 * The plan of q with the lion paths preferred, as EXPLAIN (COSTS OFF) prints
 * it - or, with analyze, the node's own lines of EXPLAIN ANALYZE that depend
 * on nothing but the data.
 */
CREATE FUNCTION slr_plan(q text, ana bool DEFAULT false, prefer bool DEFAULT true)
RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	PERFORM set_config('pg_lion.prefer_slice_ranges', prefer::text, true);
	FOR ln IN EXECUTE CASE WHEN ana
					  THEN 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) '
					  ELSE 'EXPLAIN (COSTS OFF) ' END || q LOOP
		IF NOT ana OR
		   ln ~ '(LionCount|Lion Indexes|Range From Slices|Aggregates From Slices|Slice Range|Range Sources|Group Key)' THEN
			RETURN NEXT regexp_replace(ln, '\s+\(actual.*$', '');
		END IF;
	END LOOP;
	PERFORM set_config('pg_lion.prefer_slice_ranges', 'off', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
END $$;

-- ---------- 1. every type of the allowlist ----------
-- 30,000 rows on some 500 heap pages: NULLs, negatives, and each type's
-- extremes - its least and greatest values, the infinities of date and the
-- timestamps - in rows of their own.
CREATE TABLE slr (id int, g int, h int, a int2, b int4, c int8, d date,
				  t time, ts timestamp, tz timestamptz, f float8, pad text)
	WITH (autovacuum_enabled = off);
INSERT INTO slr
SELECT i, i % 10, i % 4,
	   CASE WHEN i % 17 = 0 THEN NULL ELSE (i * 13) % 2001 - 1000 END,
	   CASE WHEN i % 19 = 0 THEN NULL ELSE (i * 7919) % 200001 - 100000 END,
	   CASE WHEN i % 23 = 0 THEN NULL ELSE (i::int8 * 1000003) % 20000000001 - 10000000000 END,
	   CASE WHEN i % 29 = 0 THEN NULL ELSE date '2000-01-01' + (i * 7) % 20000 - 10000 END,
	   CASE WHEN i % 31 = 0 THEN NULL ELSE time '00:00' + (i * 61) % 86400 * interval '1 s' END,
	   CASE WHEN i % 37 = 0 THEN NULL ELSE timestamp '2000-01-01' + (i * 7777) % 1000000 * interval '1 min' - interval '500000 min' END,
	   CASE WHEN i % 41 = 0 THEN NULL ELSE timestamptz '2020-01-01 00:00+00' + (i * 3331) % 100000 * interval '1 s' END,
	   (i % 100) / 7.0,
	   repeat('x', 80)
  FROM generate_series(1, 30000) i;
INSERT INTO slr (id, g, h, a, b, c, d, t, ts, tz, f, pad) VALUES
	(30001, 3, 1, -32768, -2147483648, -9223372036854775808,
	 '-infinity', '00:00', '-infinity', '-infinity', -1e300, 'least'),
	(30002, 3, 2, 32767, 2147483647, 9223372036854775807,
	 'infinity', '24:00', 'infinity', 'infinity', 1e300, 'greatest'),
	(30003, 3, 3, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 'nulls'),
	(30004, 3, 0, -32767, -2147483647, -9223372036854775807,
	 '4713-01-01 BC', '00:00:00.000001', '4713-01-01 00:00 BC', '4713-01-01 00:00+00 BC', -1e299, 'next least'),
	(30005, 3, 0, 32766, 2147483646, 9223372036854775806,
	 '5874897-12-31', '23:59:59.999999', '294276-12-31 23:59:59', '294276-12-31 23:59:59+00', 1e299, 'next greatest');
CREATE INDEX slr_g ON slr USING lion (g);
CREATE INDEX slr_h ON slr USING lion (h);
CREATE INDEX slr_v ON slr USING lion (a, b, c, d, t, ts, tz) WITH (slices = on);
CREATE INDEX slr_f ON slr USING lion (f) WITH (slices = on);
VACUUM (FREEZE, ANALYZE) slr;

-- The plan: the range's index and bounds, and the line that says it is
-- folded from the slices, with how many slices its column has.
SELECT slr_plan('SELECT count(*) FROM slr WHERE b > 10');
SELECT slr_plan('SELECT count(*) FROM slr WHERE c BETWEEN -5 AND 5000000000');
SELECT slr_plan('SELECT count(*) FROM slr WHERE a <> 7');
SELECT slr_plan('SELECT count(*) FROM slr WHERE b > 10', true);

-- int2: each operator, BETWEEN, `<>`, at and around the type's extremes and
-- past them (an int4 or int8 bound), and a BETWEEN reversed
SELECT q, slr_q(q) FROM unnest(ARRAY[
	'SELECT count(*) FROM slr WHERE a > 100',
	'SELECT count(*) FROM slr WHERE a >= 100',
	'SELECT count(*) FROM slr WHERE a < -100',
	'SELECT count(*) FROM slr WHERE a <= -100',
	'SELECT count(*) FROM slr WHERE a BETWEEN -500 AND 499',
	'SELECT count(*) FROM slr WHERE a BETWEEN 499 AND -500',
	'SELECT count(*) FROM slr WHERE a <> 7',
	'SELECT count(*) FROM slr WHERE a > -32768',
	'SELECT count(*) FROM slr WHERE a >= -32768',
	'SELECT count(*) FROM slr WHERE a < 32767',
	'SELECT count(*) FROM slr WHERE a <= 32767',
	'SELECT count(*) FROM slr WHERE a > 32767',
	'SELECT count(*) FROM slr WHERE a < -32768',
	'SELECT count(*) FROM slr WHERE a > 32766',
	'SELECT count(*) FROM slr WHERE a < -32767',
	'SELECT count(*) FROM slr WHERE a > 40000',
	'SELECT count(*) FROM slr WHERE a < 40000',
	'SELECT count(*) FROM slr WHERE a > -40000',
	'SELECT count(*) FROM slr WHERE a < -40000::int8',
	'SELECT count(*) FROM slr WHERE a BETWEEN -40000 AND 40000',
	'SELECT count(*) FROM slr WHERE a <> 32767',
	'SELECT count(*) FROM slr WHERE a <> -32768',
	'SELECT count(*) FROM slr WHERE a <> 40000',
	'SELECT count(*) FROM slr WHERE a > 0 AND a <> 5 AND a <> 6 AND a < 900',
	'SELECT count(*) FROM slr WHERE a > 0 AND a < 0',
	'SELECT count(*) FROM slr WHERE a >= 0 AND a <= 0',
	'SELECT count(*) FROM slr WHERE a > 5 AND a >= 3 AND a < 600 AND a <= 650'
]) q;

-- int4 and int8: the same, with bounds of the other integer types
SELECT q, slr_q(q) FROM unnest(ARRAY[
	'SELECT count(*) FROM slr WHERE b > 0',
	'SELECT count(*) FROM slr WHERE b >= -2147483648',
	'SELECT count(*) FROM slr WHERE b > -2147483648',
	'SELECT count(*) FROM slr WHERE b < 2147483647',
	'SELECT count(*) FROM slr WHERE b <= 2147483647',
	'SELECT count(*) FROM slr WHERE b > 2147483646',
	'SELECT count(*) FROM slr WHERE b > 2147483647',
	'SELECT count(*) FROM slr WHERE b > 5000000000',
	'SELECT count(*) FROM slr WHERE b < 5000000000',
	'SELECT count(*) FROM slr WHERE b > -5000000000',
	'SELECT count(*) FROM slr WHERE b < -5000000000',
	'SELECT count(*) FROM slr WHERE b BETWEEN -5000000000 AND 50000',
	'SELECT count(*) FROM slr WHERE b BETWEEN 5000::int2 AND 10',
	'SELECT count(*) FROM slr WHERE b BETWEEN -1000::int2 AND 1000::int2',
	'SELECT count(*) FROM slr WHERE b <> -2147483648',
	'SELECT count(*) FROM slr WHERE b <> 0',
	'SELECT count(*) FROM slr WHERE c > 0',
	'SELECT count(*) FROM slr WHERE c >= -9223372036854775808',
	'SELECT count(*) FROM slr WHERE c > -9223372036854775808',
	'SELECT count(*) FROM slr WHERE c < 9223372036854775807',
	'SELECT count(*) FROM slr WHERE c > 9223372036854775806',
	'SELECT count(*) FROM slr WHERE c > 9223372036854775807',
	'SELECT count(*) FROM slr WHERE c < -9223372036854775807',
	'SELECT count(*) FROM slr WHERE c BETWEEN -2147483648 AND 2147483647',
	'SELECT count(*) FROM slr WHERE c BETWEEN 9223372036854775807 AND -9223372036854775808',
	'SELECT count(*) FROM slr WHERE c > 100::int2',
	'SELECT count(*) FROM slr WHERE c <> 9223372036854775807',
	'SELECT count(*) FROM slr WHERE c < 0 AND c <> -10000000000'
]) q;

-- date, time, timestamp, timestamptz: their infinities and extremes
SELECT q, slr_q(q) FROM unnest(ARRAY[
	'SELECT count(*) FROM slr WHERE d > ''2000-01-01''',
	'SELECT count(*) FROM slr WHERE d >= ''-infinity''',
	'SELECT count(*) FROM slr WHERE d > ''-infinity''',
	'SELECT count(*) FROM slr WHERE d < ''infinity''',
	'SELECT count(*) FROM slr WHERE d <= ''infinity''',
	'SELECT count(*) FROM slr WHERE d > ''infinity''',
	'SELECT count(*) FROM slr WHERE d < ''-infinity''',
	'SELECT count(*) FROM slr WHERE d BETWEEN ''1990-01-01'' AND ''2010-12-31''',
	'SELECT count(*) FROM slr WHERE d BETWEEN ''2010-12-31'' AND ''1990-01-01''',
	'SELECT count(*) FROM slr WHERE d <> ''infinity''',
	'SELECT count(*) FROM slr WHERE d > ''5874897-12-30''',
	'SELECT count(*) FROM slr WHERE t > ''12:00''',
	'SELECT count(*) FROM slr WHERE t >= ''00:00''',
	'SELECT count(*) FROM slr WHERE t > ''00:00''',
	'SELECT count(*) FROM slr WHERE t < ''24:00''',
	'SELECT count(*) FROM slr WHERE t <= ''24:00''',
	'SELECT count(*) FROM slr WHERE t BETWEEN ''06:00'' AND ''18:00''',
	'SELECT count(*) FROM slr WHERE ts > ''2000-01-01''',
	'SELECT count(*) FROM slr WHERE ts > ''-infinity''',
	'SELECT count(*) FROM slr WHERE ts < ''infinity''',
	'SELECT count(*) FROM slr WHERE ts >= ''infinity''',
	'SELECT count(*) FROM slr WHERE ts <= ''-infinity''',
	'SELECT count(*) FROM slr WHERE ts BETWEEN ''1999-06-01'' AND ''2000-06-01''',
	'SELECT count(*) FROM slr WHERE ts <> ''-infinity''',
	'SELECT count(*) FROM slr WHERE tz > ''2020-01-01 12:00+00''',
	'SELECT count(*) FROM slr WHERE tz > ''-infinity''',
	'SELECT count(*) FROM slr WHERE tz < ''infinity''',
	'SELECT count(*) FROM slr WHERE tz BETWEEN ''2020-01-01 06:00+00'' AND ''2020-01-01 18:00+00''',
	'SELECT count(*) FROM slr WHERE tz <> ''infinity'' AND tz > ''2020-01-02''',
	'SELECT count(*) FROM slr WHERE tz < ''4713-01-01 00:00:01+00 BC'''
]) q;

-- a float column is not sliced: its ranges are walked as before
SELECT q, slr_q(q) FROM unnest(ARRAY[
	'SELECT count(*) FROM slr WHERE f > 5',
	'SELECT count(*) FROM slr WHERE f BETWEEN 1 AND 3 AND b > 0'
]) q;

-- ---------- 2. beside the other clauses ----------
-- equalities, lists, null tests, `<>` and ranges on other columns, two
-- ranges from slices on two columns, and a range and an equality or a list
-- on the same column
SELECT q, slr_q(q) FROM unnest(ARRAY[
	'SELECT count(*) FROM slr WHERE b > 0 AND g = 3',
	'SELECT count(*) FROM slr WHERE b > 0 AND g IN (1, 2, 9)',
	'SELECT count(*) FROM slr WHERE b > 0 AND a IS NULL',
	'SELECT count(*) FROM slr WHERE b > 0 AND c IS NOT NULL',
	'SELECT count(*) FROM slr WHERE b > 0 AND g <> 3',
	'SELECT count(*) FROM slr WHERE b > 0 AND f > 7',
	'SELECT count(*) FROM slr WHERE b > 0 AND c < 0',
	'SELECT count(*) FROM slr WHERE b BETWEEN -50000 AND 50000 AND d > ''2000-01-01'' AND g = 4',
	'SELECT count(*) FROM slr WHERE b > 0 AND b = 7919',
	'SELECT count(*) FROM slr WHERE b > 0 AND b IN (7919, -7919, 15838)',
	'SELECT count(*), count(b), count(a) FROM slr WHERE b < 0',
	'SELECT count(*) FILTER (WHERE g = 1), count(*) FROM slr WHERE b < 0'
]) q;

-- a GROUP BY of another column, of two, and of the range's own column,
-- whose walk the range bounds as before; count(DISTINCT) of another column
SELECT q, slr_q(q) FROM unnest(ARRAY[
	'SELECT g, count(*) FROM slr WHERE b > 0 GROUP BY g',
	'SELECT g, h, count(*) FROM slr WHERE c < 0 GROUP BY g, h',
	'SELECT a, count(*) FROM slr WHERE a > 990 GROUP BY a',
	'SELECT g, count(*) FROM slr WHERE b > 0 AND a <> 5 GROUP BY g',
	'SELECT count(DISTINCT g) FROM slr WHERE b > 99000',
	'SELECT g, count(*) FROM slr WHERE b > 0 GROUP BY g ORDER BY count(*) DESC LIMIT 3'
]) q;
SELECT slr_plan('SELECT g, count(*) FROM slr WHERE b > 0 GROUP BY g');
SELECT slr_plan('SELECT a, count(*) FROM slr WHERE a > 990 GROUP BY a');

-- the aggregates of sliced columns (DESIGN.md §52) under a range from
-- slices: of the same column - sum(x) WHERE x > c - and of another, of the
-- one row and of a GROUP BY's groups, a group at a time and decoded
SELECT q, slr_q(q) FROM unnest(ARRAY[
	'SELECT sum(b), count(b), min(b), max(b) FROM slr WHERE b > 0',
	'SELECT sum(b), avg(b) FROM slr WHERE b BETWEEN -1000 AND 1000',
	'SELECT sum(c), min(c), max(c) FROM slr WHERE c < -5000000000',
	'SELECT min(d), max(d), count(d) FROM slr WHERE d > ''1990-01-01''',
	'SELECT min(ts), max(ts), count(ts) FROM slr WHERE ts < ''2000-01-01''',
	'SELECT max(tz), min(t), count(*) FROM slr WHERE tz > ''2020-01-01 12:00+00''',
	'SELECT sum(a), max(c) FROM slr WHERE b > 0',
	'SELECT sum(b) FROM slr WHERE b <> 0 AND g = 3',
	'SELECT g, sum(b), min(b), count(*) FROM slr WHERE b > 0 GROUP BY g',
	'SELECT g, sum(c), max(a) FROM slr WHERE b < 0 AND h = 1 GROUP BY g'
]) q;
SET pg_lion.enable_slice_decode = off;
SELECT slr_q('SELECT g, sum(b), min(b), count(*) FROM slr WHERE b > 0 GROUP BY g');
RESET pg_lion.enable_slice_decode;
SELECT slr_plan('SELECT sum(b), max(b) FROM slr WHERE b > 0');
SELECT slr_plan('SELECT g, sum(b), count(*) FROM slr WHERE b < 0 GROUP BY g');

-- in an OR, a leaf of the union like any range (§32), and ANDed with E it
-- pins itself: an OR of a range alone, BETWEEN SYMMETRIC (an OR of two
-- ranges), and an arm with two columns
SELECT q, slr_q(q) FROM unnest(ARRAY[
	'SELECT count(*) FROM slr WHERE b > 90000 OR g = 2',
	'SELECT count(*) FROM slr WHERE b > 90000 OR c < -9000000000',
	'SELECT count(*) FROM slr WHERE (b BETWEEN 1 AND 50000 AND h = 1) OR a < -900',
	'SELECT count(*) FROM slr WHERE b BETWEEN SYMMETRIC 50000 AND -50000',
	'SELECT count(*) FROM slr WHERE (b > 90000 OR g = 2) AND d < ''2000-01-01''',
	'SELECT g, count(*) FROM slr WHERE b < -90000 OR a > 900 GROUP BY g'
]) q;
SELECT slr_plan('SELECT count(*) FROM slr WHERE b > 90000 OR g = 2');
SELECT slr_plan('SELECT count(*) FROM slr WHERE b BETWEEN SYMMETRIC 50000 AND -50000');

-- ---------- 3. parameters and stable expressions ----------
-- a generic plan's bounds, a NULL one, crossed ones, ones past the type's
-- range, and a stable expression; a LATERAL rescan, the range collected
-- again for each outer row
SET plan_cache_mode = force_generic_plan;
SET pg_lion.prefer_slice_ranges = on;
PREPARE slr_p1(int) AS SELECT count(*) FROM slr WHERE b > $1;
PREPARE slr_p2(int8, int8) AS SELECT count(*), sum(b) FROM slr WHERE b BETWEEN $1 AND $2;
PREPARE slr_p3(date) AS SELECT g, count(*) FROM slr WHERE d < $1 GROUP BY g ORDER BY g;
PREPARE slr_p4(int) AS SELECT count(*) FROM slr WHERE a <> $1 AND a > 0;
EXPLAIN (COSTS OFF) EXECUTE slr_p1(0);
EXPLAIN (COSTS OFF) EXECUTE slr_p2(1, 2);
EXECUTE slr_p1(0);
EXECUTE slr_p1(-100000000);
EXECUTE slr_p1(2147483647);
EXECUTE slr_p1(NULL);
EXECUTE slr_p2(-50000, 50000);
EXECUTE slr_p2(50000, -50000);
EXECUTE slr_p2(-9223372036854775808, 9223372036854775807);
EXECUTE slr_p2(3000000000, 9000000000);
EXECUTE slr_p2(NULL, 5);
EXECUTE slr_p3('2000-01-01');
EXECUTE slr_p3('infinity');
EXECUTE slr_p4(7);
EXECUTE slr_p4(NULL);
RESET plan_cache_mode;
RESET pg_lion.prefer_slice_ranges;
DEALLOCATE ALL;
SET pg_lion.enable_count_pushdown = off;
SELECT count(*) FROM slr WHERE b > 0;
SELECT count(*) FROM slr WHERE b > -100000000;
SELECT count(*) FROM slr WHERE b > 2147483647;
SELECT count(*), sum(b) FROM slr WHERE b BETWEEN -50000 AND 50000;
SELECT count(*), sum(b) FROM slr WHERE b BETWEEN -9223372036854775808 AND 9223372036854775807;
SELECT g, count(*) FROM slr WHERE d < '2000-01-01' GROUP BY g ORDER BY g;
SELECT g, count(*) FROM slr WHERE d < 'infinity' GROUP BY g ORDER BY g;
SELECT count(*) FROM slr WHERE a <> 7 AND a > 0;
RESET pg_lion.enable_count_pushdown;
SELECT q, slr_q(q) FROM unnest(ARRAY[
	'SELECT count(*) FROM slr WHERE tz < now()',
	'SELECT count(*) FROM slr WHERE d > current_date - 20000',
	'SELECT o.x, s.n FROM (VALUES (-50000), (0), (50000), (NULL)) o(x), LATERAL (SELECT count(*) AS n FROM slr WHERE slr.b > o.x) s'
]) q;

-- ---------- 4. the heap the map does not vouch for ----------
-- Rows deleted and updated and not vacuumed: the copy holds the slices'
-- rows, and every count rechecks their pages in the heap.
DELETE FROM slr WHERE id % 50 = 7;
UPDATE slr SET b = -b, c = c + 1, d = d + 1, a = -a WHERE id % 40 = 3;
SELECT q, slr_q(q) FROM unnest(ARRAY[
	'SELECT count(*) FROM slr WHERE b > 0',
	'SELECT count(*) FROM slr WHERE b BETWEEN -1000 AND 1000 AND g = 3',
	'SELECT sum(b), min(b), max(b) FROM slr WHERE b > 0',
	'SELECT g, count(*) FROM slr WHERE c < 0 GROUP BY g',
	'SELECT count(*) FROM slr WHERE a <> 0 AND a < 100'
]) q;
-- rows this transaction inserted at the extremes, deleted and updated,
-- seen by its own counts, and rolled back
BEGIN;
INSERT INTO slr (id, g, h, a, b, c, d, ts, tz) VALUES
	(40001, 3, 0, 32767, 2147483647, 9223372036854775807, 'infinity', 'infinity', 'infinity'),
	(40002, 3, 0, -32768, -2147483648, -9223372036854775808, '-infinity', '-infinity', '-infinity'),
	(40003, 3, 0, 0, 0, 0, '2000-01-01', '2000-01-01', '2000-01-01');
DELETE FROM slr WHERE g = 3 AND id % 3 = 0;
UPDATE slr SET b = b + 1 WHERE g = 4 AND id % 7 = 0;
SELECT q, slr_q(q) FROM unnest(ARRAY[
	'SELECT count(*) FROM slr WHERE b > 2147483646',
	'SELECT count(*) FROM slr WHERE b < -2147483647',
	'SELECT count(*) FROM slr WHERE b >= 0 AND g = 3',
	'SELECT count(*) FROM slr WHERE c > 9223372036854775806 OR c < -9223372036854775807',
	'SELECT count(*), max(d) FROM slr WHERE d > ''2100-01-01''',
	'SELECT g, count(*), sum(b) FROM slr WHERE b BETWEEN 0 AND 100000 GROUP BY g'
]) q;
ROLLBACK;
SELECT slr_q('SELECT count(*) FROM slr WHERE b > 2147483646');
-- ... and after VACUUM, the map's again
VACUUM (FREEZE) slr;
SELECT q, slr_q(q) FROM unnest(ARRAY[
	'SELECT count(*) FROM slr WHERE b > 0',
	'SELECT sum(b), min(b), max(b) FROM slr WHERE b > 0',
	'SELECT g, count(*) FROM slr WHERE c < 0 GROUP BY g'
]) q;
SELECT slr_plan('SELECT count(*) FROM slr WHERE b > 0', true);

-- ---------- 5. a copy past the memory ----------
-- 600,000 rows of four bytes on 42 container keys: the copy of half of them,
-- a 2 kB bitmap a key, is past the least hash table and goes to a temporary
-- file, which the counts read a container at a time - beside a GROUP BY's
-- WHERE collected once, beside another clause, and as the aggregates' WHERE.
CREATE TABLE slr_s (k int4, g int2) WITH (autovacuum_enabled = off);
INSERT INTO slr_s SELECT hashint4(i), i % 10 FROM generate_series(1, 600000) i;
CREATE INDEX slr_s_k ON slr_s USING lion (k) WITH (slices = on);
CREATE INDEX slr_s_g ON slr_s USING lion (g);
VACUUM (FREEZE, ANALYZE) slr_s;
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SELECT q, slr_q(q) FROM unnest(ARRAY[
	'SELECT count(*) FROM slr_s WHERE k > 0',
	'SELECT g, count(*) FROM slr_s WHERE k BETWEEN -1500000000 AND 1500000000 GROUP BY g',
	'SELECT count(*) FROM slr_s WHERE k > 0 AND g = 3',
	'SELECT sum(k), max(k) FROM slr_s WHERE k < 0'
]) q;
SELECT slr_plan('SELECT count(*) FROM slr_s WHERE k > 0', true);
RESET work_mem;
RESET hash_mem_multiplier;

-- ---------- 5b. in parallel ----------
-- A count of one row divided among the participants of a Gather by ranges of
-- container keys: each collects the range from the slices, as each collects
-- a range of the entries (§10, "A count in parallel").  A GROUP BY beside a
-- range taken as a source stays serial, as it does beside any.
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET pg_lion.parallel_range_keys = 1;
SELECT q, slr_q(q) FROM unnest(ARRAY[
	'SELECT count(*) FROM slr_s WHERE k > 0 AND g = 3',
	'SELECT count(*) FROM slr_s WHERE k BETWEEN -1000000000 AND 1000000000 AND g IN (1, 2)',
	'SELECT g, count(*) FROM slr_s WHERE k > 0 GROUP BY g'
]) q;
SELECT slr_plan('SELECT count(*) FROM slr_s WHERE k > 0 AND g = 3');
SELECT slr_plan('SELECT g, count(*) FROM slr_s WHERE k > 0 GROUP BY g');
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET min_parallel_index_scan_size;
RESET pg_lion.parallel_range_keys;

-- ---------- 6. the planner's choice ----------
-- 100,000 rows of unique values in no heap order, a column with summaries
-- and slices and its twin with slices alone: a selective range is summed
-- from the summaries, or walked, and a wide one is folded from the slices.
CREATE TABLE slr_c (id int, k int4, k2 int4, g int) WITH (autovacuum_enabled = off);
INSERT INTO slr_c SELECT i, (i * 7919) % 100003, (i * 7919) % 100003, i % 10
  FROM generate_series(1, 100000) i;
CREATE INDEX slr_c_k ON slr_c USING lion (k) WITH (summaries = on, slices = on);
CREATE INDEX slr_c_k2 ON slr_c USING lion (k2) WITH (slices = on);
CREATE INDEX slr_c_g ON slr_c USING lion (g);
VACUUM (FREEZE, ANALYZE) slr_c;
EXPLAIN (COSTS OFF) SELECT count(*) FROM slr_c WHERE k < 100;
EXPLAIN (COSTS OFF) SELECT count(*) FROM slr_c WHERE k < 60000;
EXPLAIN (COSTS OFF) SELECT count(*) FROM slr_c WHERE k2 < 100;
EXPLAIN (COSTS OFF) SELECT count(*) FROM slr_c WHERE k2 < 60000;
EXPLAIN (COSTS OFF) SELECT g, count(*) FROM slr_c WHERE k2 BETWEEN 1000 AND 90000 GROUP BY g;
EXPLAIN (COSTS OFF) SELECT sum(k) FROM slr_c WHERE k > 30000;
SET pg_lion.enable_slice_ranges = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM slr_c WHERE k < 60000;
RESET pg_lion.enable_slice_ranges;
SELECT q, slr_q(q, false) FROM unnest(ARRAY[
	'SELECT count(*) FROM slr_c WHERE k < 100',
	'SELECT count(*) FROM slr_c WHERE k < 60000',
	'SELECT count(*) FROM slr_c WHERE k2 < 100',
	'SELECT count(*) FROM slr_c WHERE k2 < 60000',
	'SELECT sum(k) FROM slr_c WHERE k > 30000'
]) q;

-- ---------- 7. declines ----------
-- a partitioned table, a join, and a column no index slices: their ranges
-- are answered as before
CREATE TABLE slr_part (k int, v int4) PARTITION BY RANGE (k);
CREATE TABLE slr_part1 PARTITION OF slr_part FOR VALUES FROM (0) TO (50);
CREATE TABLE slr_part2 PARTITION OF slr_part FOR VALUES FROM (50) TO (100);
INSERT INTO slr_part SELECT i % 100, i FROM generate_series(1, 2000) i;
CREATE INDEX ON slr_part USING lion (k);
CREATE INDEX ON slr_part USING lion (v) WITH (slices = on);
VACUUM ANALYZE slr_part;
-- whether the count of a partitioned table is pushed down at all is the cost
-- model's call and differs between majors; what is tested is that no range
-- comes from slices there, so the two plans without one print alike and
-- anything else - slices, or a mismatch of the rows - prints as it is
SELECT CASE WHEN r LIKE 'range not from slices%' OR r LIKE 'no pushdown%'
			THEN 'not from slices' ELSE r END AS partitioned
  FROM (SELECT slr_q('SELECT count(*) FROM slr_part WHERE v > 1000 AND k = 7') AS r) s;
CREATE TABLE slr_dim (g int PRIMARY KEY, name text);
INSERT INTO slr_dim SELECT i, 'g' || i FROM generate_series(0, 9) i;
ANALYZE slr_dim;
SELECT slr_q('SELECT d.name, count(*) FROM slr s JOIN slr_dim d ON d.g = s.g WHERE s.b > 0 GROUP BY d.name');
SELECT slr_q('SELECT count(*) FROM slr_c WHERE id > 50000 AND g = 3');
-- with the switch off, nothing is answered from slices
SET pg_lion.enable_slice_ranges = off;
SELECT slr_q('SELECT count(*) FROM slr WHERE b > 0 AND g = 3');
RESET pg_lion.enable_slice_ranges;
