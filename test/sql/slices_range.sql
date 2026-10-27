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
