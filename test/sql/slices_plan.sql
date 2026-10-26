-- Aggregates of bit-sliced columns answered from their slices by the
-- LionCount node (DESIGN.md §52): count, sum, avg, min and max of a column a
-- lion index slices (§51), under a WHERE of every kind the count pushdown
-- takes, beside count(*) and filtered counts.  Every answer is checked
-- against the ordinary plan (the pushdown off, a sequential scan), and the
-- plan that ran it is shown.
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
 * slp_q() runs q with the lion paths preferred - every other scan disabled -
 * then as a sequential scan with the pushdown off, and compares the two as
 * multisets.  It says what the node did: answered aggregates from slices,
 * over keys (§37), counts only, or nothing.
 */
CREATE FUNCTION slp_q(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	used boolean := false;
	slices boolean := false;
	keys boolean := false;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(LionCount\)' THEN
			used := true;
		END IF;
		IF ln ~ 'Aggregates From Slices:' THEN
			slices := true;
		END IF;
		IF ln ~ 'Aggregates Over Keys:' THEN
			keys := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE slp_on AS SELECT s::text AS r FROM (%s) s', q);

	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE format('CREATE TEMP TABLE slp_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM slp_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM slp_on EXCEPT ALL SELECT * FROM slp_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM slp_off EXCEPT ALL SELECT * FROM slp_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE slp_on, slp_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN slices THEN 'from slices' WHEN keys THEN 'over keys'
					   WHEN used THEN 'counts only' ELSE 'no pushdown' END,
				  nrows);
END $$;

/*
 * The plan of q with the lion paths preferred, as EXPLAIN (COSTS OFF) prints
 * it - or, with analyze, the node's own lines of EXPLAIN ANALYZE, the
 * counters that depend on nothing but the data (the kernel's name aside).
 */
CREATE FUNCTION slp_plan(q text, ana bool DEFAULT false) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	FOR ln IN EXECUTE CASE WHEN ana
					  THEN 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) '
					  ELSE 'EXPLAIN (COSTS OFF) ' END || q LOOP
		IF NOT ana OR
		   ln ~ '(LionCount|Lion Indexes|Count Filter|Aggregates From Slices|Slice|Range Sources)' THEN
			RETURN NEXT regexp_replace(regexp_replace(ln, '\s+\(actual.*$', ''),
									   'Slice Kernel: .*$', 'Slice Kernel: (the CPU''s)');
		END IF;
	END LOOP;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
END $$;

-- ---------- 1. every type of the allowlist ----------
-- 30,000 rows of some 500 heap pages (eight container keys): NULLs,
-- negatives, and each type's extremes - its least and greatest values, the
-- infinities of date and timestamp - in a few rows of their own.
CREATE TABLE slp (id int, g int, h int, r int4, tags int[],
				  a int2, b int4, c int8, d date, t time, ts timestamp,
				  tz timestamptz, f float8, pad text)
	WITH (autovacuum_enabled = off);
INSERT INTO slp
SELECT i, i % 10, i % 4, (i * 37) % 1000,
	   ARRAY[i % 7, 10 + i % 5],
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
INSERT INTO slp (id, g, h, r, tags, a, b, c, d, t, ts, tz, f, pad) VALUES
	(30001, 3, 1, 5, '{1}', -32768, -2147483648, -9223372036854775808,
	 '-infinity', '00:00', '-infinity', '-infinity', -1e300, 'least'),
	(30002, 3, 2, 6, '{2}', 32767, 2147483647, 9223372036854775807,
	 'infinity', '24:00', 'infinity', 'infinity', 1e300, 'greatest'),
	(30003, 3, 3, 7, '{3}', NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 'nulls');
CREATE INDEX slp_g ON slp USING lion (g);
CREATE INDEX slp_h ON slp USING lion (h);
CREATE INDEX slp_r ON slp USING lion (r) WITH (summaries = on, summary_tids = 64);
CREATE INDEX slp_tags ON slp USING lion (tags);
CREATE INDEX slp_v ON slp USING lion (a, b, c, d, t, ts, tz) WITH (slices = on);
CREATE INDEX slp_f ON slp USING lion (f) WITH (slices = on);
VACUUM (FREEZE, ANALYZE) slp;
SELECT attno, sliced FROM lion_index_stats('slp_v') ORDER BY attno;
SELECT attno, sliced FROM lion_index_stats('slp_f') ORDER BY attno;

-- What the plan shows: the slices' index and column, which slices it counts
-- against the WHERE and which descents it makes.
SELECT * FROM slp_plan('SELECT count(b), sum(b), avg(b), min(b), max(b) FROM slp WHERE g = 3');
SELECT * FROM slp_plan('SELECT sum(b), count(*) FROM slp WHERE g = 3');
SELECT * FROM slp_plan('SELECT max(c), min(d), count(*) FROM slp WHERE g = 3');
SELECT * FROM slp_plan('SELECT count(b), sum(b), avg(b), min(b), max(b) FROM slp WHERE g = 3', true);
SELECT count(b), sum(b), avg(b), min(b), max(b) FROM slp WHERE g = 3;

SELECT slp_q('SELECT count(a), sum(a), avg(a), min(a), max(a) FROM slp WHERE g = 3');
SELECT slp_q('SELECT count(b), sum(b), avg(b), min(b), max(b) FROM slp WHERE g = 3');
SELECT slp_q('SELECT count(c), sum(c), avg(c), min(c), max(c) FROM slp WHERE g = 3');
SELECT slp_q('SELECT count(d), min(d), max(d) FROM slp WHERE g = 3');
SELECT slp_q('SELECT count(t), min(t), max(t) FROM slp WHERE g = 3');
SELECT slp_q('SELECT count(ts), min(ts), max(ts) FROM slp WHERE g = 3');
SELECT slp_q('SELECT count(tz), min(tz), max(tz) FROM slp WHERE g = 3');
SELECT count(a), sum(a), min(a), max(a), count(c), sum(c), min(c), max(c),
	   min(d), max(d), min(t), max(t), min(ts), max(ts), min(tz), max(tz)
  FROM slp WHERE g = 3;
-- several columns of one index, and every aggregate at once
SELECT slp_q('SELECT sum(a), sum(b), sum(c), max(d), min(t), max(ts), min(tz), count(*) FROM slp WHERE g = 4');
SELECT * FROM slp_plan('SELECT sum(a), sum(b), sum(c), max(d), min(t), max(ts), min(tz), count(*) FROM slp WHERE g = 4');
-- the extremes, where the WHERE takes them in, and NULLs only
SELECT slp_q('SELECT count(c), sum(c), avg(c), min(c), max(c), min(d), max(ts) FROM slp WHERE h IN (1, 2)');
SELECT sum(c), avg(c), min(c), max(c), min(a), max(a), min(d), max(d) FROM slp WHERE g = 3 AND h IN (1, 2);
SELECT slp_q('SELECT count(b), sum(b), min(b), max(b) FROM slp WHERE g = 3 AND h IN (1, 2)');
SELECT slp_q('SELECT count(b), sum(b), avg(b), min(b), max(b), count(*) FROM slp WHERE tags @> ''{3}'' AND g = 3 AND h = 3');
SELECT slp_q('SELECT sum(b), min(b), count(*) FROM slp WHERE g = 99');
-- sum(int4) of the extremes: the int8 core adds them in
SELECT slp_q('SELECT sum(b), avg(b) FROM slp WHERE r < 10 AND g = 3');

-- ---------- 2. a WHERE of every kind ----------
SELECT slp_q('SELECT sum(b), min(b), max(b), count(*) FROM slp WHERE g IN (1, 5, 9)');
SELECT slp_q('SELECT sum(b), min(b), max(b), count(*) FROM slp WHERE g <> 2');
SELECT slp_q('SELECT sum(b), min(b), max(b), count(*) FROM slp WHERE g NOT IN (2, 3)');
SELECT slp_q('SELECT sum(b), min(b), max(b), count(*) FROM slp WHERE b IS NOT NULL');
SELECT slp_q('SELECT sum(b), min(b), max(b), count(b), count(*) FROM slp WHERE b IS NULL');
SELECT slp_q('SELECT sum(b), min(b), max(b), count(*) FROM slp WHERE c IS NULL AND g = 1');
SELECT slp_q('SELECT sum(b), min(b), max(b), count(*) FROM slp WHERE g = 1 OR h = 2');
SELECT slp_q('SELECT sum(b), min(b), max(b), count(*) FROM slp WHERE (g = 1 AND h = 1) OR tags && ''{4}''');
SELECT slp_q('SELECT sum(b), min(b), max(b), count(*) FROM slp WHERE tags @> ''{2, 12}''');
SELECT slp_q('SELECT sum(b), min(b), max(b), count(*) FROM slp WHERE tags && ''{1, 6}'' AND h = 0');
-- ranges on another column: summed, and as a source beside a clause
SELECT slp_q('SELECT sum(b), min(b), max(b), count(*) FROM slp WHERE r < 300');
SELECT * FROM slp_plan('SELECT sum(b), min(b), max(b), count(*) FROM slp WHERE r < 300');
SELECT slp_q('SELECT sum(b), min(b), max(b), count(*) FROM slp WHERE r BETWEEN 200 AND 700 AND g = 3');
SELECT * FROM slp_plan('SELECT sum(b), min(b), max(b) FROM slp WHERE r BETWEEN 200 AND 700 AND g = 3');
SELECT slp_q('SELECT sum(b), max(b) FROM slp WHERE r >= 990');
SELECT slp_q('SELECT sum(b), max(b), count(*) FROM slp WHERE r > 2000');
-- a range of a column with nothing else, and no count beside it
SELECT * FROM slp_plan('SELECT sum(b), max(b) FROM slp WHERE r < 300 OR g = 2');
SELECT slp_q('SELECT sum(b), max(b) FROM slp WHERE r < 300 OR g = 2');
-- an equality on the sliced column itself, and a NULL test of it
SELECT slp_q('SELECT sum(b), min(b), max(b), count(b) FROM slp WHERE b = 15827');
SELECT slp_q('SELECT sum(c), max(c), count(*) FROM slp WHERE b IS NULL AND g = 2');
-- a stable expression, evaluated once a scan
CREATE FUNCTION slp_lim() RETURNS int LANGUAGE sql STABLE AS $$ SELECT 400 $$;
SELECT slp_q('SELECT sum(b), min(b), max(b), count(*) FROM slp WHERE r < slp_lim() AND h = 1');
SELECT slp_q('SELECT sum(b), max(b) FROM slp WHERE g = length(current_setting(''DateStyle''))');
-- no WHERE: the slices, or the aggregates over keys (§37), which the cost
-- decides between
SELECT slp_q('SELECT sum(b), max(b), min(b), count(*) FROM slp');
SELECT * FROM slp_plan('SELECT sum(b), max(b), min(b), count(*) FROM slp');
SET pg_lion.enable_count_slices = off;
SELECT slp_q('SELECT sum(b), max(b), min(b), count(*) FROM slp');
SELECT slp_q('SELECT sum(b), max(b) FROM slp WHERE g = 3');
RESET pg_lion.enable_count_slices;

-- ---------- 3. beside counts, filtered counts and a HAVING ----------
SELECT slp_q('SELECT count(*), count(b), sum(b), count(*) FILTER (WHERE h = 1), max(b) FROM slp WHERE g = 2');
SELECT * FROM slp_plan('SELECT count(*), count(b), sum(b), count(*) FILTER (WHERE h = 1), max(b) FROM slp WHERE g = 2');
SELECT slp_q('SELECT sum(CASE WHEN h = 1 THEN 1 ELSE 0 END), sum(b) FROM slp WHERE g IN (2, 3)');
SELECT slp_q('SELECT sum(b) FROM slp WHERE g = 2 HAVING sum(b) > 0');
SELECT slp_q('SELECT sum(b) FROM slp WHERE g = 2 HAVING sum(b) < 0');
SELECT slp_q('SELECT max(b) - min(b), sum(b)::numeric / count(b), round(avg(c)) FROM slp WHERE g = 2');
-- with the plane kernel off: each slice's container counted on its own, the
-- same answers
SET pg_lion.enable_slice_kernel = off;
SELECT slp_q('SELECT count(b), sum(b), avg(b), min(b), max(b) FROM slp WHERE g = 3');
SELECT * FROM slp_plan('SELECT count(b), sum(b) FROM slp WHERE g = 3', true);
RESET pg_lion.enable_slice_kernel;

-- ---------- 4. parameters ----------
SET plan_cache_mode = force_generic_plan;
PREPARE slp_p1(int) AS SELECT count(b), sum(b), min(b), max(b), count(*) FROM slp WHERE g = $1;
PREPARE slp_p2(int[]) AS SELECT sum(c), max(d) FROM slp WHERE g = ANY ($1);
PREPARE slp_p3(int, int) AS SELECT sum(b), min(b) FROM slp WHERE r BETWEEN $1 AND $2;
EXPLAIN (COSTS OFF) EXECUTE slp_p1(3);
EXECUTE slp_p1(3);
EXECUTE slp_p1(7);
EXECUTE slp_p1(NULL);
EXECUTE slp_p2('{1,2}');
EXECUTE slp_p3(100, 200);
EXECUTE slp_p3(900, 100);
RESET plan_cache_mode;
SELECT count(b), sum(b), min(b), max(b), count(*) FROM slp WHERE g = 7;
SELECT sum(c), max(d) FROM slp WHERE g = ANY ('{1,2}');
SELECT sum(b), min(b) FROM slp WHERE r BETWEEN 100 AND 200;
DEALLOCATE ALL;
-- a LATERAL rescan: the node counted again for every outer row
SELECT o.g, s.* FROM (VALUES (1), (2), (99)) o(g),
	   LATERAL (SELECT sum(b), min(c), max(ts) FROM slp WHERE slp.g = o.g) s
 ORDER BY 1;

-- ---------- 5. the heap the map does not vouch for ----------
-- Rows deleted and updated and not vacuumed: every count rechecks their
-- pages in the heap, whatever the slices still hold.
DELETE FROM slp WHERE id % 50 = 7;
UPDATE slp SET b = -b, c = c + 1, d = d + 1 WHERE id % 40 = 3;
SELECT slp_q('SELECT count(b), sum(b), avg(b), min(b), max(b), count(*) FROM slp WHERE g = 3');
SELECT slp_q('SELECT sum(c), min(d), max(d), count(*) FROM slp WHERE h = 3 OR g = 1');
SELECT slp_q('SELECT sum(b), max(b), min(b) FROM slp');
SELECT * FROM slp_plan('SELECT count(b), sum(b) FROM slp WHERE g = 3', true);
-- rows this transaction inserted, with the column's extremes, and deleted
BEGIN;
INSERT INTO slp (id, g, h, r, a, b, c, d) VALUES
	(40001, 3, 0, 1, 32767, 2147483647, 9223372036854775807, 'infinity'),
	(40002, 3, 0, 2, -32768, -2147483648, -9223372036854775808, '-infinity');
DELETE FROM slp WHERE g = 3 AND id % 3 = 0;
SELECT slp_q('SELECT count(b), sum(b), min(b), max(b), sum(c), min(c), max(c), max(d), count(*) FROM slp WHERE g = 3');
SELECT sum(b), min(b), max(b), max(d), min(a), max(a) FROM slp WHERE g = 3;
ROLLBACK;
SELECT slp_q('SELECT count(b), sum(b), min(b), max(b), sum(c), min(c), max(c), max(d), count(*) FROM slp WHERE g = 3');
-- ... and after VACUUM, the map's again
VACUUM (FREEZE) slp;
SELECT slp_q('SELECT count(b), sum(b), avg(b), min(b), max(b), count(*) FROM slp WHERE g = 3');
SELECT slp_q('SELECT sum(c), min(d), max(d), count(*) FROM slp WHERE h = 3 OR g = 1');
SELECT * FROM slp_plan('SELECT count(b), sum(b) FROM slp WHERE g = 3', true);
-- (a REPEATABLE READ snapshot across another session's changes:
-- test/isolation/slices_pushdown.spec)

-- ---------- 6. GROUP BY ----------
-- Each group's set ANDed into the WHERE's copy, and that copy counted
-- against the slices and descended as the one row is: a group of one
-- column, its values walked; of two, its pairs; of an IN list's values.
SELECT * FROM slp_plan('SELECT g, sum(b), min(b), max(b), count(*) FROM slp WHERE h = 1 GROUP BY g');
SELECT * FROM slp_plan('SELECT g, sum(b), min(b), max(b), count(*) FROM slp WHERE h = 1 GROUP BY g', true);
SELECT slp_q('SELECT g, sum(b), min(b), max(b), count(*) FROM slp WHERE h = 1 GROUP BY g');
SELECT slp_q('SELECT g, count(a), sum(a), avg(a), avg(c), min(d), max(tz) FROM slp GROUP BY g');
SELECT slp_q('SELECT h, sum(c), min(c), max(c), max(d), count(*) FROM slp WHERE tags && ''{1, 6}'' GROUP BY h');
SELECT slp_q('SELECT g, sum(b), max(b) FROM slp WHERE g IN (1, 2, 99) GROUP BY g');
SELECT slp_q('SELECT g, sum(b), max(b) FROM slp WHERE r < 300 GROUP BY g');
-- two columns: the pairs
SELECT * FROM slp_plan('SELECT g, h, sum(b), max(d) FROM slp GROUP BY g, h');
SELECT slp_q('SELECT g, h, sum(b), max(d), count(*) FROM slp GROUP BY g, h');
SELECT slp_q('SELECT g, h, min(c), avg(b) FROM slp WHERE r BETWEEN 100 AND 600 GROUP BY g, h');
-- many groups: a thousand values of r, a few rows each
SELECT slp_q('SELECT r, sum(b), min(b), max(b), count(*) FROM slp GROUP BY r');
SELECT slp_q('SELECT r, max(ts) FROM slp WHERE g = 3 GROUP BY r');
-- the group with NULL, the extremes' group, a group the WHERE empties
SELECT g, sum(c), min(c), max(c), min(a), max(a), max(d) FROM slp WHERE h IN (1, 2) GROUP BY g ORDER BY g;
-- HAVING, and ordered above the node
SELECT slp_q('SELECT g, sum(b) FROM slp GROUP BY g HAVING max(b) > 99990');
SELECT slp_q('SELECT g, sum(b) FROM slp GROUP BY g HAVING sum(b) > 0 AND count(*) > 2000');
SELECT * FROM slp_plan('SELECT g, sum(b) FROM slp GROUP BY g HAVING max(b) > 99990');
SELECT g, sum(b), max(b) FROM slp GROUP BY g ORDER BY sum(b) DESC LIMIT 3;
-- with the kernel off, the same
SET pg_lion.enable_slice_kernel = off;
SELECT slp_q('SELECT g, h, sum(b), max(d), count(*) FROM slp GROUP BY g, h');
RESET pg_lion.enable_slice_kernel;
-- a generic plan's parameter
SET plan_cache_mode = force_generic_plan;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET pg_lion.enable_plain_scan = off;
PREPARE slp_p4(int) AS SELECT g, sum(b), min(b), count(*) FROM slp WHERE h = $1 GROUP BY g ORDER BY g;
EXPLAIN (COSTS OFF) EXECUTE slp_p4(2);
EXECUTE slp_p4(2);
RESET plan_cache_mode;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET pg_lion.enable_plain_scan;
SELECT g, sum(b), min(b), count(*) FROM slp WHERE h = 2 GROUP BY g ORDER BY g;
DEALLOCATE ALL;
-- the heap the map does not vouch for, and this transaction's rows
BEGIN;
UPDATE slp SET b = b + 1 WHERE id % 11 = 0;
INSERT INTO slp (id, g, h, r, b, c) VALUES (40003, 5, 1, 3, 2147483647, -9223372036854775808);
SELECT slp_q('SELECT g, sum(b), min(b), max(b), sum(c), min(c), count(*) FROM slp WHERE h = 1 GROUP BY g');
SELECT slp_q('SELECT g, h, sum(b), max(b) FROM slp GROUP BY g, h');
ROLLBACK;
-- a grouping by a sliced column itself, its values walked as any column's
SELECT slp_q('SELECT b, sum(c) FROM slp WHERE g = 3 GROUP BY b');
-- declined: a filtered count beside a GROUP BY, three columns, a grouping
-- by an expression
SELECT slp_q('SELECT g, sum(b), count(*) FILTER (WHERE h = 1) FROM slp GROUP BY g');
SELECT slp_q('SELECT g, h, r, sum(b) FROM slp WHERE r < 5 GROUP BY g, h, r');
SELECT slp_q('SELECT g % 2, sum(b) FROM slp GROUP BY g % 2');

-- ---------- 7. declined ----------
-- a column that is not sliced, and a float column, which no slice takes
SELECT slp_q('SELECT sum(r), max(r) FROM slp WHERE g = 3');
SELECT slp_q('SELECT sum(f), max(f) FROM slp WHERE g = 3');
-- an aggregate of an expression of the column, DISTINCT, FILTER, ORDER BY
SELECT slp_q('SELECT sum(b / 2) FROM slp WHERE g = 3');
SELECT slp_q('SELECT sum(DISTINCT b) FROM slp WHERE g = 3');
SELECT slp_q('SELECT sum(b) FILTER (WHERE h = 1) FROM slp WHERE g = 3');
SELECT slp_q('SELECT min(b ORDER BY b) FROM slp WHERE g = 3');
-- a residual clause no posting set answers
SELECT slp_q('SELECT sum(b) FROM slp WHERE g = 3 AND pad LIKE ''l%''');
-- a join
CREATE TABLE slp_dim (g int PRIMARY KEY, name text);
INSERT INTO slp_dim SELECT i, 'g' || i FROM generate_series(0, 9) i;
ANALYZE slp_dim;
SELECT slp_q('SELECT sum(s.b), max(s.b) FROM slp s JOIN slp_dim d ON d.g = s.g WHERE d.name = ''g3''');
-- a partitioned table
CREATE TABLE slp_part (k int, v int4) PARTITION BY RANGE (k);
CREATE TABLE slp_part1 PARTITION OF slp_part FOR VALUES FROM (0) TO (50);
CREATE TABLE slp_part2 PARTITION OF slp_part FOR VALUES FROM (50) TO (100);
INSERT INTO slp_part SELECT i % 100, i FROM generate_series(1, 2000) i;
CREATE INDEX ON slp_part USING lion (k);
CREATE INDEX ON slp_part USING lion (v) WITH (slices = on);
VACUUM ANALYZE slp_part;
SELECT slp_q('SELECT sum(v), max(v) FROM slp_part WHERE k = 7');
SELECT slp_q('SELECT count(*) FROM slp_part WHERE k = 7');

DROP TABLE slp, slp_dim, slp_part;
DROP FUNCTION slp_lim();
DROP FUNCTION slp_plan(text, bool);
DROP FUNCTION slp_q(text);
