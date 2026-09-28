-- A GROUP BY count divided among the participants of a parallel plan
-- (DESIGN.md §10, "A GROUP BY in parallel").
--
-- Once its WHERE is one the node collects, a GROUP BY over one column's
-- entries is also offered parallel-aware, below a Gather and core's Finalize
-- HashAggregate: the participants cut the heap's container keys into ranges
-- and each counts every group over the ranges it claims, against the WHERE
-- collected for that range alone - a partial count per group and range, which
-- the Finalize Agg adds up.  Every answer is checked against the same query
-- counted serially by the pushdown, and with the pushdown off; the node's
-- EXPLAIN ANALYZE says how many ranges its participants counted.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET default_statistics_target = 1000;

-- parallel plans at any size, and ranges of a single container key at the
-- least, so that a table of a few megabytes is cut into several ranges
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET pg_lion.parallel_range_keys = 1;

/*
 * lgp_check() says which plan q gets - the parallel node, the serial one, or
 * neither - and runs it as planned, once more with
 * max_parallel_workers_per_gather at 0, and once with the pushdown off and
 * sequential scans only (no bitmap, index or index-only scan, which could read
 * the very posting sets the node reads and agree with it about a wrong
 * answer).  It compares the first two with the third.
 */
CREATE FUNCTION lgp_check(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	how text := 'not pushed down';
	nrows bigint;
	pdiff bigint;
	sdiff bigint;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Parallel Custom Scan (LionCount)%' THEN
			how := 'parallel';
		ELSIF ln LIKE '%Custom Scan (LionCount)%' AND how <> 'parallel' THEN
			how := 'serial';
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lgp_par AS SELECT s::text AS r FROM (%s) s', q);

	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	EXECUTE format('CREATE TEMP TABLE lgp_ser AS SELECT s::text AS r FROM (%s) s', q);

	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	EXECUTE format('CREATE TEMP TABLE lgp_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('max_parallel_workers_per_gather', '2', true);

	EXECUTE 'SELECT count(*) FROM lgp_off' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (TABLE lgp_par EXCEPT ALL TABLE lgp_off) a)'
			' + (SELECT count(*) FROM (TABLE lgp_off EXCEPT ALL TABLE lgp_par) b)'
		INTO pdiff;
	EXECUTE 'SELECT (SELECT count(*) FROM (TABLE lgp_ser EXCEPT ALL TABLE lgp_off) a)'
			' + (SELECT count(*) FROM (TABLE lgp_off EXCEPT ALL TABLE lgp_ser) b)'
		INTO sdiff;
	EXECUTE 'DROP TABLE lgp_par, lgp_ser, lgp_off';

	IF pdiff <> 0 OR sdiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ as planned, %s serially', pdiff, sdiff);
	END IF;
	RETURN format('%s, %s rows', how, nrows);
END $$;

/* The node's counters in EXPLAIN ANALYZE, as "Name: value, ...". */
CREATE FUNCTION lgp_counters(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	out text := '';
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF btrim(split_part(ln, ':', 1)) IN ('Key Ranges', 'WHERE Sets Collected',
											 'Group Batches', 'Groups Counted in Batches') THEN
			out := out || CASE WHEN out = '' THEN '' ELSE ', ' END || btrim(ln);
		END IF;
	END LOOP;
	RETURN out;
END $$;

/*
 * 60,000 rows in some 870 pages - fourteen container keys, which a leader
 * and two workers cut into twelve ranges.  g has 30 values and NULLs, k 40,
 * both scattered over every page; s is the row's place in the heap, a twelfth
 * of it a value, so that its groups and its filter lie in one range or two;
 * a, b, c filters of a quarter, a half and a third of the rows, c with NULLs;
 * tags a small array.
 */
CREATE TABLE lgp (id int NOT NULL, g int, k int NOT NULL, s int NOT NULL,
				  a int NOT NULL, b boolean NOT NULL, c int, tags int[] NOT NULL,
				  pad text NOT NULL)
	WITH (autovacuum_enabled = off);
INSERT INTO lgp
SELECT i, CASE WHEN h % 31 = 0 THEN NULL ELSE h % 30 END, (h / 30) % 40,
	   (i - 1) / 5000, (h / 7) % 4, (h / 11) % 2 = 0,
	   CASE WHEN h % 13 = 0 THEN NULL ELSE (h / 13) % 3 END,
	   ARRAY[(h / 17) % 6, 10 + (h / 19) % 4], repeat('x', 30)
  FROM (SELECT i, abs(hashint4(i)) AS h FROM generate_series(1, 60000) i) r;
CREATE INDEX lgp_gk ON lgp USING lion (g, k);
CREATE INDEX lgp_s ON lgp USING lion (s);
CREATE INDEX lgp_abc ON lgp USING lion (a, b, c);
CREATE INDEX lgp_tags ON lgp USING lion (tags);
VACUUM (FREEZE, ANALYZE) lgp;

-- ---------- 1. the plans ----------
EXPLAIN (COSTS OFF) SELECT g, count(*) FROM lgp WHERE a = 1 AND b GROUP BY g;
-- the HAVING, and every count, are the Finalize Agg's
EXPLAIN (COSTS OFF) SELECT k, count(*), count(b) FROM lgp WHERE a = 1 AND b GROUP BY k HAVING count(*) > 90;
-- as many workers as max_parallel_workers_per_gather allows, and none at all
SET max_parallel_workers_per_gather = 1;
EXPLAIN (COSTS OFF) SELECT g, count(*) FROM lgp WHERE a = 1 AND b GROUP BY g;
SET max_parallel_workers_per_gather = 0;
EXPLAIN (COSTS OFF) SELECT g, count(*) FROM lgp WHERE a = 1 AND b GROUP BY g;
SET max_parallel_workers_per_gather = 2;
-- ... and as many as the table's parallel_workers allows
ALTER TABLE lgp SET (parallel_workers = 0);
EXPLAIN (COSTS OFF) SELECT g, count(*) FROM lgp WHERE a = 1 AND b GROUP BY g;
ALTER TABLE lgp RESET (parallel_workers);

-- ---------- 2. the answers ----------
-- filters, the NULL group among the groups
SELECT lgp_check('SELECT g, count(*) FROM lgp WHERE a = 1 AND b GROUP BY g');
SELECT lgp_check('SELECT g, count(*) FROM lgp WHERE a = 1 AND b AND c = 2 GROUP BY g');
-- a negated filter, and the NULL entry as a filter
SELECT lgp_check('SELECT g, count(*) FROM lgp WHERE a = 2 AND NOT b AND c IS NOT NULL GROUP BY g');
SELECT lgp_check('SELECT k, count(*) FROM lgp WHERE c IS NULL AND NOT b GROUP BY k');
-- an IN list, an OR across columns, multi-key queries
SELECT lgp_check('SELECT k, count(*) FROM lgp WHERE a IN (1, 3) AND NOT b GROUP BY k');
SELECT lgp_check('SELECT g, count(*) FROM lgp WHERE (a = 1 OR s = 4) AND b GROUP BY g');
SELECT lgp_check($$SELECT g, count(*) FROM lgp WHERE tags && '{2, 5}' AND a = 0 GROUP BY g$$);
SELECT lgp_check($$SELECT k, count(*) FROM lgp WHERE tags && '{1, 12}' AND b GROUP BY k$$);
-- a filter that lies in two ranges of the twelve, and a group column whose
-- groups each lie in one or two: most ranges are counted without a row
SELECT lgp_check('SELECT g, count(*) FROM lgp WHERE s = 3 AND a = 1 GROUP BY g');
SELECT lgp_check('SELECT s, count(*) FROM lgp WHERE a = 1 AND b GROUP BY s');
-- a filter with no entry: no row
SELECT lgp_check('SELECT g, count(*) FROM lgp WHERE a = 9 AND b GROUP BY g');
-- counts of columns, a printed filter column, a HAVING, an ORDER BY and LIMIT
SELECT lgp_check('SELECT g, count(g), count(b), count(*) FROM lgp WHERE a = 1 AND b GROUP BY g');
SELECT lgp_check('SELECT g, a, count(*) FROM lgp WHERE a = 1 AND b GROUP BY g, a');
SELECT lgp_check('SELECT k, count(*) FROM lgp WHERE a = 1 AND b GROUP BY k HAVING count(*) > 90');
SELECT lgp_check('SELECT k, count(*) FROM lgp WHERE a = 1 AND b GROUP BY k ORDER BY count(*) DESC, k LIMIT 5');
-- the leader alone, when no worker starts: the same ranges, the same answers
SET max_parallel_workers = 0;
SELECT lgp_check('SELECT g, count(*) FROM lgp WHERE a = 1 AND b GROUP BY g');
RESET max_parallel_workers;
-- a SERIALIZABLE transaction
BEGIN ISOLATION LEVEL SERIALIZABLE;
SELECT lgp_check('SELECT g, count(*) FROM lgp WHERE a = 1 AND b GROUP BY g');
COMMIT;

-- ---------- 3. what the participants counted ----------
-- every range once, whichever participant took it: a WHERE collected for
-- each, and the groups counted in one batch a range
SELECT lgp_counters('SELECT g, count(*) FROM lgp WHERE a = 1 AND b GROUP BY g');
-- a filter in two ranges: the others collect nothing and count no group
SELECT lgp_counters('SELECT g, count(*) FROM lgp WHERE s = 3 AND a = 1 GROUP BY g');
SET max_parallel_workers = 0;
SELECT lgp_counters('SELECT g, count(*) FROM lgp WHERE a = 1 AND b GROUP BY g');
RESET max_parallel_workers;
-- ranges of 16 keys, the default: one range for this table
RESET pg_lion.parallel_range_keys;
SELECT lgp_counters('SELECT g, count(*) FROM lgp WHERE a = 1 AND b GROUP BY g');
SET pg_lion.parallel_range_keys = 1;

-- ---------- 4. a generic plan's parameters, in every participant ----------
SET plan_cache_mode = force_generic_plan;
PREPARE lgp_p(int, boolean) AS SELECT g, count(*) FROM lgp WHERE a = $1 AND b = $2 GROUP BY g ORDER BY g;
EXPLAIN (COSTS OFF) EXECUTE lgp_p(1, true);
EXECUTE lgp_p(1, true);
SET pg_lion.enable_count_pushdown = off;
SET max_parallel_workers_per_gather = 0;
PREPARE lgp_q(int, boolean) AS SELECT g, count(*) FROM lgp WHERE a = $1 AND b = $2 GROUP BY g ORDER BY g;
EXECUTE lgp_q(1, true);
RESET pg_lion.enable_count_pushdown;
SET max_parallel_workers_per_gather = 2;
DEALLOCATE lgp_p;
DEALLOCATE lgp_q;
RESET plan_cache_mode;

-- ---------- 5. rows the visibility map cannot vouch for ----------
-- new versions of a tenth of the rows, not vacuumed: their pages go to the
-- heap, in whichever participant counts their range
UPDATE lgp SET a = (a + 1) % 4 WHERE id % 10 = 0;
SELECT lgp_check('SELECT g, count(*) FROM lgp WHERE a = 1 AND b GROUP BY g');
SELECT lgp_check('SELECT k, count(*) FROM lgp WHERE a IN (1, 3) AND NOT b GROUP BY k');
VACUUM lgp;
SELECT lgp_check('SELECT g, count(*) FROM lgp WHERE a = 1 AND b GROUP BY g');

-- ---------- 6. the shapes that stay serial ----------
-- two group columns, GROUP BY coalesce, count(DISTINCT), an IN list that
-- drives the groups, no WHERE, a WHERE of one set the counts copy anyway
SELECT lgp_check('SELECT g, k, count(*) FROM lgp WHERE a = 1 AND b GROUP BY g, k');
SELECT lgp_check('SELECT coalesce(g, -1), count(*) FROM lgp WHERE a = 1 AND b GROUP BY coalesce(g, -1)');
SELECT lgp_check('SELECT g, count(DISTINCT k) FROM lgp WHERE a = 1 AND b GROUP BY g');
SELECT lgp_check('SELECT g, count(*) FROM lgp WHERE g IN (1, 2, 3) AND a = 1 AND b GROUP BY g');
SELECT lgp_check('SELECT g, count(*) FROM lgp GROUP BY g');
SELECT lgp_check('SELECT g, count(*) FROM lgp WHERE a = 1 GROUP BY g');

DROP TABLE lgp;
DROP FUNCTION lgp_check(text);
DROP FUNCTION lgp_counters(text);
