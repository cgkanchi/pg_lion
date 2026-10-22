-- An IN list too long to locate at once (2026-09-28 review; DESIGN.md §15,
-- "A list too long to locate at once").
--
-- A parameter's array has no length cap, and the pushdown held every
-- value's located posting set at once, work_mem or not - past some nine
-- million values more than an allocation may have.  A count of the relation
-- as one row now locates such a list a batch at a time and adds the batches'
-- counts up, which is exact because the entries of one scalar index are
-- disjoint and a batch never splits the values of one entry.  Every other
-- shape that counts the list as a source of its own collects its sets a batch
-- at a time into one union, as a range is collected, a list under an OR too;
-- and one that drives the groups is located a batch at a time as they are
-- walked.  Each query below is answered with the pushdown and with a
-- sequential scan, and the two must agree.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
/* VACUUM can only set all-visible for commits that reached disk (citext.sql). */
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE lion_ll (k int NOT NULL, j int, x int NOT NULL, t text NOT NULL);
INSERT INTO lion_ll
SELECT i % 5000, CASE WHEN i % 11 = 0 THEN NULL ELSE i % 13 END, i % 3, 'v' || (i % 4000)
  FROM generate_series(1, 60000) i;
CREATE INDEX lion_ll_k ON lion_ll USING lion (k);
CREATE INDEX lion_ll_j ON lion_ll USING lion (j);
CREATE INDEX lion_ll_x ON lion_ll USING lion (x);
CREATE INDEX lion_ll_t ON lion_ll USING lion (t);
VACUUM ANALYZE lion_ll;

CREATE TABLE lion_llp (k int NOT NULL, x int NOT NULL) PARTITION BY RANGE (x);
CREATE TABLE lion_llp_0 PARTITION OF lion_llp FOR VALUES FROM (0) TO (2);
CREATE TABLE lion_llp_1 PARTITION OF lion_llp FOR VALUES FROM (2) TO (10);
INSERT INTO lion_llp SELECT i % 4000, i % 5 FROM generate_series(1, 40000) i;
CREATE INDEX lion_llp_k ON lion_llp USING lion (k);
CREATE INDEX lion_llp_x ON lion_llp USING lion (x);
VACUUM ANALYZE lion_llp;

/*
 * lion_llcmp() runs q with the pushdown (every other scan disabled) and with
 * a sequential scan, compares the answers as multisets, and says whether the
 * node took the list a batch at a time, collected it or held it whole.  The
 * plain scans of Lion indexes are disabled by pg_lion.enable_plain_scan, not
 * enable_indexscan, which would disable the node too (REFERENCE.md,
 * "Settings"): before PostgreSQL 18 a disable is disable_cost on the price,
 * add_path() takes two prices that both hold it as equal within its fuzz, and
 * which plan wins is then up to its tie-breaks - sorted output, parallel
 * safety, rows - not the price.  The sequential scan runs serially, whatever
 * max_parallel_workers_per_gather says.
 */
CREATE FUNCTION lion_llcmp(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	l text;
	pushed boolean := false;
	par boolean := false;
	batches bigint := 0;
	lists bigint := 0;
	a text;
	b text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ 'Custom Scan \(LionCount\)' THEN
			pushed := true;
		END IF;
		IF l ~ 'Parallel Custom Scan \(LionCount\)' THEN
			par := true;
		END IF;
		IF l ~ 'List Batches: ' THEN
			batches := batches + substring(l from 'List Batches: ([0-9]+)')::bigint;
		END IF;
		IF l ~ 'Lists Collected: ' THEN
			lists := lists + substring(l from 'Lists Collected: ([0-9]+)')::bigint;
		END IF;
	END LOOP;
	EXECUTE 'SELECT array_agg(r ORDER BY r)::text FROM (' || q || ') r' INTO a;
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	EXECUTE 'SELECT array_agg(r ORDER BY r)::text FROM (' || q || ') r' INTO b;
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH: %s against %s', left(a, 120), left(b, 120));
	END IF;
	RETURN CASE WHEN NOT pushed THEN 'not pushed down'
				WHEN par AND lists > 0 THEN 'pushed down in parallel, collected'
				WHEN par AND batches > 1 THEN 'pushed down in parallel, in batches'
				WHEN par THEN 'pushed down in parallel'
				WHEN batches > 1 AND lists > 0 THEN 'pushed down, in batches and collected'
				WHEN batches > 1 THEN 'pushed down, in batches'
				WHEN lists > 0 THEN 'pushed down, collected'
				ELSE 'pushed down, whole' END || ', same answer';
END $$;

-- a batch is a work_mem of located sets, never fewer than a thousand values
SET work_mem = '64kB';
-- the list alone (the disjoint sum, batch by batch), and with repeats that
-- straddle every batch boundary, and NULLs
SELECT lion_llcmp('SELECT count(*) FROM lion_ll WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g))');
SELECT lion_llcmp('SELECT count(*) FROM lion_ll WHERE k = ANY (array(SELECT g % 1500 FROM generate_series(0, 5999) g))');
SELECT lion_llcmp('SELECT count(*) FROM lion_ll WHERE k = ANY (array(SELECT (g * 7) % 2600 FROM generate_series(0, 4999) g))');
SELECT lion_llcmp('SELECT count(*) FROM lion_ll WHERE k = ANY (array(SELECT CASE WHEN g % 7 = 0 THEN NULL ELSE g END FROM generate_series(0, 3499) g))');
-- ANDed with another clause, a negated one, and another list
SELECT lion_llcmp('SELECT count(*) FROM lion_ll WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) AND x = 1');
SELECT lion_llcmp('SELECT count(*) FROM lion_ll WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) AND j IS NOT NULL');
SELECT lion_llcmp('SELECT count(*) FROM lion_ll WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) AND j = ANY (array(SELECT g FROM generate_series(1, 4) g))');
-- values with no entry: all of them, and all but a few
SELECT lion_llcmp('SELECT count(*) FROM lion_ll WHERE k = ANY (array(SELECT g FROM generate_series(100000, 103000) g))');
SELECT lion_llcmp('SELECT count(*) FROM lion_ll WHERE k = ANY (array(SELECT g FROM generate_series(4990, 7990) g))');
-- another element type than the key's, and text
SELECT lion_llcmp('SELECT count(*) FROM lion_ll WHERE k = ANY (array(SELECT g::int8 FROM generate_series(0, 2999) g))');
SELECT lion_llcmp($$SELECT count(*) FROM lion_ll WHERE t = ANY (array(SELECT 'v' || g FROM generate_series(0, 3999) g)) AND x = 2$$);
-- a partitioned table: each partition's list in batches of its own
SELECT lion_llcmp('SELECT count(*) FROM lion_llp WHERE k = ANY (array(SELECT g FROM generate_series(0, 2499) g))');
-- the shapes that count the list more than once collect it, values with no
-- entry and repeats across the batches included; a second long list in a
-- plain count is collected beside the first's batches; a list under an OR is
-- collected too, and one on the grouping column, which drives the groups, is
-- located a batch at a time as they are walked
SELECT lion_llcmp('SELECT x, count(*) FROM lion_ll WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) GROUP BY x');
SELECT lion_llcmp('SELECT count(DISTINCT x) FROM lion_ll WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g))');
SELECT lion_llcmp('SELECT x, count(*) FROM lion_ll WHERE k = ANY (array(SELECT (g * 7) % 7000 FROM generate_series(0, 5999) g)) GROUP BY x');
SELECT lion_llcmp('SELECT j, count(*) FROM lion_ll WHERE k = ANY (array(SELECT CASE WHEN g % 7 = 0 THEN NULL ELSE g END FROM generate_series(0, 3499) g)) AND x = 1 GROUP BY j');
SELECT lion_llcmp($$SELECT x, count(*) FROM lion_ll WHERE t = ANY (array(SELECT 'v' || g FROM generate_series(0, 3999) g)) AND k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) GROUP BY x$$);
SELECT lion_llcmp($$SELECT count(*) FROM lion_ll WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) AND t = ANY (array(SELECT 'v' || g FROM generate_series(0, 3999) g))$$);
SELECT lion_llcmp('SELECT x, count(*) FROM lion_llp WHERE k = ANY (array(SELECT g FROM generate_series(0, 2499) g)) GROUP BY x');
SELECT lion_llcmp('SELECT count(*) FROM lion_ll WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) OR x = 1');
SELECT lion_llcmp('SELECT k, count(*) FROM lion_ll WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) GROUP BY k');
-- under an OR: in an arm beside another clause, with NULLs and in a GROUP BY,
-- in a filter, beside a list of another column, beside a range leaf
-- (rangesrc_spill.sql spills one), and with all but a few values without an
-- entry; the OR then carries no interlock, as one with a range leaf does not
SELECT lion_llcmp('SELECT count(*) FROM lion_ll WHERE (k = ANY (array(SELECT g FROM generate_series(4990, 7990) g)) AND j = 3) OR x = 1');
SELECT lion_llcmp('SELECT x, count(*) FROM lion_ll WHERE k = ANY (array(SELECT CASE WHEN g % 7 = 0 THEN NULL ELSE g END FROM generate_series(0, 3499) g)) OR j = 2 GROUP BY x');
SELECT lion_llcmp('SELECT count(*) FILTER (WHERE k = ANY (array(SELECT g % 1500 FROM generate_series(0, 5999) g)) OR x = 1), count(*) FROM lion_ll');
SELECT lion_llcmp($$SELECT count(*) FROM lion_ll WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) OR t = ANY (array(SELECT 'v' || g FROM generate_series(0, 3999) g))$$);
SELECT lion_llcmp('SELECT j, count(*) FROM lion_ll WHERE k = ANY (array(SELECT (g * 7) % 7000 FROM generate_series(0, 5999) g)) OR k < 10 GROUP BY j');
-- the list that drives the groups: with repeats across the batches, values
-- without an entry and NULLs, beside another clause, a count(DISTINCT) of it
-- with and without another, of text, and on a partitioned table
SELECT lion_llcmp('SELECT k, count(*) FROM lion_ll WHERE k = ANY (array(SELECT (g * 3) % 9000 FROM generate_series(0, 5999) g)) GROUP BY k');
SELECT lion_llcmp('SELECT k, count(*) FROM lion_ll WHERE k = ANY (array(SELECT CASE WHEN g % 7 = 0 THEN NULL ELSE g END FROM generate_series(0, 3499) g)) AND x = 1 GROUP BY k');
SELECT lion_llcmp('SELECT count(DISTINCT k) FROM lion_ll WHERE k = ANY (array(SELECT g % 4100 FROM generate_series(0, 5999) g))');
SELECT lion_llcmp('SELECT count(DISTINCT k), count(*) FROM lion_ll WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) AND j IS NOT NULL');
SELECT lion_llcmp($$SELECT t, count(*) FROM lion_ll WHERE t = ANY (array(SELECT 'v' || g FROM generate_series(0, 3999) g)) GROUP BY t$$);
SELECT lion_llcmp('SELECT k, count(*) FROM lion_llp WHERE k = ANY (array(SELECT g FROM generate_series(0, 2499) g)) GROUP BY k');
-- how many lists were collected, once each: not once for each partition
-- that collected one
CREATE FUNCTION lion_lllists(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	l text;
	pushed boolean := false;
	lists bigint := 0;
	spilled bigint := 0;
	batches bigint := 0;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ 'Custom Scan \(LionCount\)' THEN
			pushed := true;
		END IF;
		IF l ~ 'Lists Collected: ' THEN
			lists := lists + substring(l from 'Lists Collected: ([0-9]+)')::bigint;
		END IF;
		IF l ~ 'Lists Spilled: ' THEN
			spilled := spilled + substring(l from 'Lists Spilled: ([0-9]+)')::bigint;
		END IF;
		IF l ~ 'List Batches: ' THEN
			batches := batches + substring(l from 'List Batches: ([0-9]+)')::bigint;
		END IF;
	END LOOP;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
	RETURN CASE WHEN pushed THEN format('lists collected %s, spilled %s, list batches %s',
										 lists, spilled, batches)
				ELSE 'not pushed down' END;
END $$;
SELECT lion_lllists('SELECT x, count(*) FROM lion_llp WHERE k = ANY (array(SELECT g FROM generate_series(0, 2499) g)) GROUP BY x');
SELECT lion_lllists($$SELECT x, count(*) FROM lion_ll WHERE t = ANY (array(SELECT 'v' || g FROM generate_series(0, 3999) g)) AND k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) GROUP BY x$$);
SELECT lion_lllists('SELECT count(*) FILTER (WHERE k = ANY (array(SELECT g FROM generate_series(0, 2499) g))), count(*) FILTER (WHERE k = ANY (array(SELECT g * 2 FROM generate_series(0, 2499) g))) FROM lion_llp');
SELECT lion_lllists('SELECT k, count(*) FROM lion_llp WHERE k = ANY (array(SELECT g FROM generate_series(0, 2499) g)) GROUP BY k');
-- a list that is the only source of a filtered count is collected too, and
-- the filter's run is the sum over the walk that counts every row, ANDed with
-- that one set, as a run whose source is a range is - alone, beside other
-- filtered counts and a list of its own, in each form a filtered count takes,
-- with all but a few values without an entry, in a GROUP BY and on a
-- partitioned table
SELECT lion_llcmp('SELECT count(*) FILTER (WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g))) FROM lion_ll');
-- ... where the heap is all-visible the walk lets the map answer for every
-- row of it, and none is rechecked in the heap
CREATE FUNCTION lion_llrechecked(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	l text;
	pushed boolean := false;
	n bigint := 0;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ 'Custom Scan \(LionCount\)' THEN
			pushed := true;
		END IF;
		IF l ~ 'Heap TIDs Rechecked: ' THEN
			n := n + substring(l from 'Heap TIDs Rechecked: ([0-9]+)')::bigint;
		END IF;
	END LOOP;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
	RETURN CASE WHEN pushed THEN format('pushed down, %s rows rechecked', n)
				ELSE 'not pushed down' END;
END $$;
SELECT lion_llrechecked('SELECT count(*) FILTER (WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g))) FROM lion_ll');
SELECT lion_llcmp($$SELECT count(*) FILTER (WHERE k = ANY (array(SELECT (g * 7) % 7000 FROM generate_series(0, 5999) g))), count(*) FILTER (WHERE x = 1), count(*) FILTER (WHERE t = ANY (array(SELECT 'v' || g FROM generate_series(0, 3999) g))), count(*) FROM lion_ll$$);
SELECT lion_llcmp('SELECT sum(CASE WHEN k = ANY (array(SELECT CASE WHEN g % 7 = 0 THEN NULL ELSE g END FROM generate_series(0, 3499) g)) THEN 1 ELSE 0 END), count(CASE WHEN k = ANY (array(SELECT g % 1500 FROM generate_series(0, 5999) g)) THEN 1 END) FROM lion_ll');
SELECT lion_llcmp('SELECT count(*) FILTER (WHERE k = ANY (array(SELECT g FROM generate_series(4990, 7990) g))) FROM lion_ll');
SELECT lion_llcmp('SELECT x, count(*) FILTER (WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g))) FROM lion_ll GROUP BY x');
SELECT lion_llcmp('SELECT count(*) FILTER (WHERE k = ANY (array(SELECT g FROM generate_series(0, 2499) g))), count(*) FROM lion_llp');
-- a dirty heap
UPDATE lion_ll SET x = (x + 1) % 3 WHERE k % 17 = 0;
DELETE FROM lion_ll WHERE k % 19 = 0;
SELECT lion_llcmp('SELECT count(*) FROM lion_ll WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g))');
SELECT lion_llcmp('SELECT count(*) FROM lion_ll WHERE k = ANY (array(SELECT g % 1500 FROM generate_series(0, 5999) g)) AND x = 1');
SELECT lion_llcmp('SELECT x, count(*) FROM lion_ll WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) GROUP BY x');
SELECT lion_llcmp('SELECT count(*) FILTER (WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g))), count(*) FILTER (WHERE x = 1) FROM lion_ll');
SELECT lion_llcmp('SELECT x, count(*) FROM lion_ll WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) OR j = 2 GROUP BY x');
SELECT lion_llcmp('SELECT k, count(*) FROM lion_ll WHERE k = ANY (array(SELECT (g * 3) % 9000 FROM generate_series(0, 5999) g)) AND x = 1 GROUP BY k');
SELECT lion_llcmp('SELECT count(DISTINCT k) FROM lion_ll WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g))');

-- the filtered counts in parallel: each participant collects the list for
-- the ranges of the filter's run it counts
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET pg_lion.parallel_range_keys = 1;
SELECT lion_llcmp('SELECT count(*) FILTER (WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g))) FROM lion_ll');
SELECT lion_llcmp('SELECT count(*) FILTER (WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g))), count(*) FILTER (WHERE x = 1), count(*) FROM lion_ll');
-- ... one list, however many participants collected it
SELECT lion_lllists('SELECT count(*) FILTER (WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g))), count(*) FILTER (WHERE x = 1), count(*) FROM lion_ll');
-- ... and the list under an OR and the one counted by groups, each
-- participant for its ranges; the one that drives the groups is walked by
-- one process, as a short one is
SELECT lion_llcmp('SELECT count(*) FROM lion_ll WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) OR x = 1');
SELECT lion_llcmp('SELECT x, count(*) FROM lion_ll WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) GROUP BY x');
SELECT lion_llcmp('SELECT k, count(*) FROM lion_ll WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) GROUP BY k');
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET min_parallel_index_scan_size;
RESET pg_lion.parallel_range_keys;
SET max_parallel_workers_per_gather = 0;

/*
 * A generic plan, the list its parameter: the planner knows nothing of its
 * length, and the run collects it.  lion_llexec() prepares q, its $1 an int[],
 * runs the generic plan with the array arr as the pushdown answers it, and
 * compares that with q run with arr by a sequential scan - q itself, not the
 * prepared statement, whose cached plan no setting changes.
 */
CREATE FUNCTION lion_llexec(q text, arr int[]) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	l text;
	pushed boolean := false;
	lists bigint := 0;
	a text;
	b text;
BEGIN
	PERFORM set_config('plan_cache_mode', 'force_generic_plan', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	EXECUTE 'PREPARE lion_llq(int[]) AS ' || q;
	FOR l IN EXECUTE format('EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) EXECUTE lion_llq(%L)', arr) LOOP
		IF l ~ 'Custom Scan \(LionCount\)' THEN
			pushed := true;
		END IF;
		IF l ~ 'Lists Collected: ' THEN
			lists := lists + substring(l from 'Lists Collected: ([0-9]+)')::bigint;
		END IF;
	END LOOP;
	EXECUTE format('EXECUTE lion_llq(%L)', arr) INTO a;
	EXECUTE 'DEALLOCATE lion_llq';
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	EXECUTE q INTO b USING arr;
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
	PERFORM set_config('plan_cache_mode', 'auto', true);
	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH: %s against %s', a, b);
	END IF;
	RETURN CASE WHEN NOT pushed THEN 'not pushed down'
				WHEN lists > 0 THEN 'pushed down, collected'
				ELSE 'pushed down, whole' END || ', same answer: ' || a;
END $$;
SELECT lion_llexec('SELECT row(count(*) FILTER (WHERE k = ANY ($1)), count(*) FILTER (WHERE x = 1))::text FROM lion_ll',
				   array(SELECT g FROM generate_series(0, 2999) g));
SELECT lion_llexec('SELECT row(count(*) FILTER (WHERE k = ANY ($1)), count(*) FILTER (WHERE x = 1))::text FROM lion_ll',
				   array(SELECT g * 3 FROM generate_series(0, 1999) g));
-- ... and one batch: located whole
SELECT lion_llexec('SELECT row(count(*) FILTER (WHERE k = ANY ($1)), count(*) FILTER (WHERE x = 1))::text FROM lion_ll',
				   array(SELECT g FROM generate_series(0, 999) g));

-- with the default work_mem the same list is one batch: located whole
RESET work_mem;
SELECT lion_llcmp('SELECT count(*) FROM lion_ll WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g))');

-- and PostgreSQL's default work_mem, 4MB, has 16384 values a batch: one
-- more is a list past one batch, and a filter of it alone is collected, as
-- is the list under an OR; the groups it drives are walked in two batches
SET work_mem = '4MB';
CREATE TABLE lion_llw (k int NOT NULL, x int NOT NULL);
INSERT INTO lion_llw SELECT (i * 7) % 20000, i % 3 FROM generate_series(1, 4000) i;
CREATE INDEX lion_llw_k ON lion_llw USING lion (k);
CREATE INDEX lion_llw_x ON lion_llw USING lion (x);
VACUUM ANALYZE lion_llw;
SELECT lion_llcmp('SELECT count(*) FILTER (WHERE k = ANY (array(SELECT g FROM generate_series(0, 16383) g))) FROM lion_llw');
SELECT lion_llcmp('SELECT count(*) FILTER (WHERE k = ANY (array(SELECT g FROM generate_series(0, 16384) g))) FROM lion_llw');
SELECT lion_llcmp('SELECT count(*) FILTER (WHERE k = ANY (array(SELECT g FROM generate_series(0, 16384) g))), count(*) FILTER (WHERE x = 1) FROM lion_llw');
SELECT lion_llcmp('SELECT x, count(*) FILTER (WHERE k = ANY (array(SELECT g FROM generate_series(0, 16384) g))) FROM lion_llw GROUP BY x');
SELECT lion_llcmp('SELECT count(*) FROM lion_llw WHERE k = ANY (array(SELECT g FROM generate_series(0, 16384) g)) OR x = 1');
SELECT lion_llcmp('SELECT k, count(*) FROM lion_llw WHERE k = ANY (array(SELECT g FROM generate_series(0, 16384) g)) GROUP BY k');
RESET work_mem;

DROP FUNCTION lion_llcmp(text);
DROP FUNCTION lion_llexec(text, int[]);
DROP FUNCTION lion_llrechecked(text);
DROP FUNCTION lion_lllists(text);
DROP TABLE lion_ll;
DROP TABLE lion_llp;
DROP TABLE lion_llw;
