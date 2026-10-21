-- A count of one row divided among the participants of a parallel plan
-- (DESIGN.md §10, "A count in parallel").
--
-- The node of one row - the count of a WHERE, or a sum over a column's
-- entries, with any filtered counts beside it - is also offered
-- parallel-aware, below a Gather and core's Finalize Aggregate: the
-- participants cut the heap's container keys into ranges, claim them one at
-- a time, and count each claimed range as the serial node counts the whole,
-- confined to the range's keys - a partial count per participant, which the
-- Finalize Aggregate adds up.  Every answer is checked against the same query
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
 * lcp_check() says which plan q gets - the parallel node, the serial one, or
 * neither - and runs it as planned, once more with
 * max_parallel_workers_per_gather at 0, and once with the pushdown off and
 * sequential scans only (no bitmap, index or index-only scan, which could read
 * the very posting sets the node reads and agree with it about a wrong
 * answer).  It compares the first two with the third.
 */
CREATE FUNCTION lcp_check(q text) RETURNS text
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
	EXECUTE format('CREATE TEMP TABLE lcp_par AS SELECT s::text AS r FROM (%s) s', q);

	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	EXECUTE format('CREATE TEMP TABLE lcp_ser AS SELECT s::text AS r FROM (%s) s', q);

	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	EXECUTE format('CREATE TEMP TABLE lcp_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('max_parallel_workers_per_gather', '2', true);

	EXECUTE 'SELECT count(*) FROM lcp_off' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (TABLE lcp_par EXCEPT ALL TABLE lcp_off) a)'
			' + (SELECT count(*) FROM (TABLE lcp_off EXCEPT ALL TABLE lcp_par) b)'
		INTO pdiff;
	EXECUTE 'SELECT (SELECT count(*) FROM (TABLE lcp_ser EXCEPT ALL TABLE lcp_off) a)'
			' + (SELECT count(*) FROM (TABLE lcp_off EXCEPT ALL TABLE lcp_ser) b)'
		INTO sdiff;
	EXECUTE 'DROP TABLE lcp_par, lcp_ser, lcp_off';

	IF pdiff <> 0 OR sdiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ as planned, %s serially', pdiff, sdiff);
	END IF;
	RETURN format('%s, %s rows', how, nrows);
END $$;

/*
 * The node's counters in EXPLAIN ANALYZE named in names, as "Name: value,
 * ...".  Range sources are collected by each participant that counts a
 * range, as many times as there are such participants: only their name is
 * printed.
 */
CREATE FUNCTION lcp_counters(q text, names text[] DEFAULT
							 '{Key Ranges, List Batches, Heap Blocks Skipped via VM, Range Evaluation}')
RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	out text := '';
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF btrim(split_part(ln, ':', 1)) = ANY (names) THEN
			out := out || CASE WHEN out = '' THEN '' ELSE ', ' END || btrim(ln);
		ELSIF btrim(split_part(ln, ':', 1)) = 'Range Sources Collected' THEN
			out := out || CASE WHEN out = '' THEN '' ELSE ', ' END ||
				btrim(split_part(ln, ':', 1));
		END IF;
	END LOOP;
	RETURN out;
END $$;

/*
 * 60,000 rows in some 870 pages - fourteen container keys, which a leader
 * and two workers cut into twelve ranges.  g has 30 values and NULLs, k 40,
 * both scattered over every page; s is the row's place in the heap, a twelfth
 * of it a value, so that a filter on it lies in one range or two; a, b, c
 * filters of a quarter, a half and a third of the rows, c with NULLs; tags a
 * small array; w a word or two.
 */
CREATE TABLE lcp (id int NOT NULL, g int, k int NOT NULL, s int NOT NULL,
				  a int NOT NULL, b boolean NOT NULL, c int, tags int[] NOT NULL,
				  w tsvector NOT NULL, pad text NOT NULL)
	WITH (autovacuum_enabled = off);
INSERT INTO lcp
SELECT i, CASE WHEN h % 31 = 0 THEN NULL ELSE h % 30 END, (h / 30) % 40,
	   (i - 1) / 5000, (h / 7) % 4, (h / 11) % 2 = 0,
	   CASE WHEN h % 13 = 0 THEN NULL ELSE (h / 13) % 3 END,
	   ARRAY[(h / 17) % 6, 10 + (h / 19) % 4],
	   to_tsvector('simple', (ARRAY['red', 'green', 'blue', 'grey'])[1 + (h / 23) % 4] || ' ' ||
							 (ARRAY['fox', 'owl', 'cat'])[1 + (h / 29) % 3]),
	   repeat('x', 30)
  FROM (SELECT i, abs(hashint4(i)) AS h FROM generate_series(1, 60000) i) r;
CREATE INDEX lcp_gk ON lcp USING lion (g, k);
CREATE INDEX lcp_s ON lcp USING lion (s);
CREATE INDEX lcp_abc ON lcp USING lion (a, b, c);
CREATE INDEX lcp_tags ON lcp USING lion (tags);
CREATE INDEX lcp_w ON lcp USING lion (w);
CREATE INDEX lcp_id ON lcp USING lion (id);
VACUUM (FREEZE, ANALYZE) lcp;

-- ---------- 1. the plans ----------
EXPLAIN (COSTS OFF) SELECT count(*) FROM lcp WHERE a = 1 AND b;
-- the HAVING, and every count, are the Finalize Aggregate's
EXPLAIN (COSTS OFF) SELECT count(*), count(*) FILTER (WHERE s = 3) FROM lcp WHERE a = 1 AND b HAVING count(*) > 90;
-- filtered counts beside the count
EXPLAIN (COSTS OFF) SELECT count(*), count(*) FILTER (WHERE s = 3) FROM lcp WHERE a = 1;
-- a sum over every entry of a column, and one a range bounds
EXPLAIN (COSTS OFF) SELECT count(g) FROM lcp;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lcp WHERE k < 10 AND b;
-- as many workers as max_parallel_workers_per_gather allows, and none at all
SET max_parallel_workers_per_gather = 1;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lcp WHERE a = 1 AND b;
SET max_parallel_workers_per_gather = 0;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lcp WHERE a = 1 AND b;
SET max_parallel_workers_per_gather = 2;
-- ... and as many as the table's parallel_workers allows
ALTER TABLE lcp SET (parallel_workers = 0);
EXPLAIN (COSTS OFF) SELECT count(*) FROM lcp WHERE a = 1 AND b;
ALTER TABLE lcp SET (parallel_workers = 1);
EXPLAIN (COSTS OFF) SELECT count(*) FROM lcp WHERE a = 1 AND b;
ALTER TABLE lcp RESET (parallel_workers);

-- ---------- 2. the answers ----------
-- the AND of sources, the NULL entry among them
SELECT lcp_check('SELECT count(*) FROM lcp WHERE a = 1 AND b');
SELECT lcp_check('SELECT count(*) FROM lcp WHERE a = 1 AND b AND c = 2 AND k = 7');
SELECT lcp_check('SELECT count(*) FROM lcp WHERE c IS NULL AND NOT b');
-- negated sources: IS NOT NULL, <>, IS DISTINCT FROM (§14, §35)
SELECT lcp_check('SELECT count(*) FROM lcp WHERE a = 2 AND NOT b AND c IS NOT NULL');
SELECT lcp_check('SELECT count(*) FROM lcp WHERE a = 2 AND c <> 1');
SELECT lcp_check('SELECT count(*) FROM lcp WHERE b AND g IS DISTINCT FROM 4');
-- IN lists: ANDed, alone (the disjoint sum), on two columns; a few values
-- of a unique column are too little work to divide, and stay serial
SELECT lcp_check('SELECT count(*) FROM lcp WHERE a IN (1, 3) AND NOT b');
SELECT lcp_check('SELECT count(*) FROM lcp WHERE k IN (1, 5, 7, 30)');
SELECT lcp_check('SELECT count(*) FROM lcp WHERE id IN (5, 500, 5000, 50000, 59999, 70000)');
SELECT lcp_check('SELECT count(*) FROM lcp WHERE k IN (1, 5, 7) AND g IN (2, 3)');
-- ORs across columns, of ANDs, and with a complement arm (§19, §35)
SELECT lcp_check('SELECT count(*) FROM lcp WHERE (a = 1 OR s = 4) AND b');
SELECT lcp_check('SELECT count(*) FROM lcp WHERE (a = 1 AND b) OR (s = 3 AND c = 2) OR k = 9');
SELECT lcp_check('SELECT count(*) FROM lcp WHERE (a = 1 OR c <> 2) AND b');
-- multi-key queries (§17): arrays, and a tsquery of ORs of ANDs
SELECT lcp_check($$SELECT count(*) FROM lcp WHERE tags && '{2, 5}' AND a = 0$$);
SELECT lcp_check($$SELECT count(*) FROM lcp WHERE tags @> '{1, 12}'$$);
SELECT lcp_check($$SELECT count(*) FROM lcp WHERE w @@ '(red & fox) | (blue & !owl) | grey'::tsquery$$);
SELECT lcp_check($$SELECT count(*) FROM lcp WHERE w @@ '(red | blue) & cat'::tsquery AND b$$);
-- filtered counts (§10): of rows, of columns, a sum of a constant
SELECT lcp_check('SELECT count(*), count(*) FILTER (WHERE s = 3), count(*) FILTER (WHERE b AND c = 2) FROM lcp WHERE a = 1');
SELECT lcp_check('SELECT count(*) FILTER (WHERE k IN (1, 2)), count(*) FILTER (WHERE c IS NULL) FROM lcp WHERE b');
SELECT lcp_check('SELECT count(*), sum(1) FILTER (WHERE s = 99) FROM lcp WHERE a = 1');
-- count(col) (§14), and expressions over the counts
SELECT lcp_check('SELECT count(g) FROM lcp WHERE k IN (3, 4)');
SELECT lcp_check('SELECT count(c) FROM lcp WHERE a = 2');
SELECT lcp_check('SELECT count(*) * 2 + 1, count(*) - 5 FROM lcp WHERE a = 3');
-- a residual clause no posting set answers, tested on each range's rows
SELECT lcp_check('SELECT count(*) FROM lcp WHERE a = 1 AND b AND id % 7 = 0');
-- a filter on s lies in one range of the twelve or two: most ranges count
-- nothing.  Its count of two bitmaps is 5 us serially, and stays serial, as
-- a value with no entry does, which is no work to divide (section 4 runs
-- one in parallel all the same); containers priced dear divide it.
SELECT lcp_check('SELECT count(*) FROM lcp WHERE s = 3 AND a = 1');
SET pg_lion.container_cost = 1000;
SELECT lcp_check('SELECT count(*) FROM lcp WHERE s = 3 AND a = 1');
RESET pg_lion.container_cost;
SELECT lcp_check('SELECT count(*) FROM lcp WHERE a = 9 AND b');
-- sums over entries: every row, every row with a value, a range bounding
-- the sum, and ranges taken as sources beside it (§14, §28, §32)
SELECT lcp_check('SELECT count(*) FROM lcp');
SELECT lcp_check('SELECT count(g) FROM lcp');
SELECT lcp_check('SELECT count(*) FROM lcp WHERE c IS NOT NULL');
SELECT lcp_check('SELECT count(*) FROM lcp WHERE c <> 1');
SELECT lcp_check('SELECT count(*) FROM lcp WHERE k < 10 AND b');
SELECT lcp_check('SELECT count(*) FROM lcp WHERE k BETWEEN 3 AND 36');
SELECT lcp_check('SELECT count(*) FROM lcp WHERE k < 20 AND s >= 4 AND a = 2');
SELECT lcp_check('SELECT count(*) FROM lcp WHERE s BETWEEN 2 AND 9 AND k > 5 AND g < 10');
-- a HAVING, the Finalize Aggregate's
SELECT lcp_check('SELECT count(*) FROM lcp WHERE a = 1 AND b HAVING count(*) > 90');
SELECT lcp_check('SELECT count(*) FROM lcp WHERE a = 1 AND b HAVING count(*) > 90000');
-- the leader alone, when no worker starts: the same ranges, the same answers
SET max_parallel_workers = 0;
SELECT lcp_check('SELECT count(*) FROM lcp WHERE a = 1 AND b');
SELECT lcp_check('SELECT count(*), count(*) FILTER (WHERE s = 3) FROM lcp WHERE a = 1');
RESET max_parallel_workers;
-- a SERIALIZABLE transaction
BEGIN ISOLATION LEVEL SERIALIZABLE;
SELECT lcp_check('SELECT count(*) FROM lcp WHERE a = 1 AND b');
SELECT lcp_check('SELECT count(*) FROM lcp WHERE k < 10 AND b');
COMMIT;

-- ---------- 3. what the participants counted ----------
-- every range once, whichever participant took it
SELECT lcp_counters('SELECT count(*) FROM lcp WHERE a = 1 AND b');
-- every range of every run: the WHERE's, and each filter's
SELECT lcp_counters('SELECT count(*), count(*) FILTER (WHERE s = 3), count(*) FILTER (WHERE c = 0) FROM lcp WHERE a = 1');
SET max_parallel_workers = 0;
SELECT lcp_counters('SELECT count(*) FROM lcp WHERE a = 1 AND b');
RESET max_parallel_workers;
-- ranges of 16 keys, the default: one range for this table, which one
-- participant would count alone - so no parallel plan at all
RESET pg_lion.parallel_range_keys;
SELECT lcp_check('SELECT count(*) FROM lcp WHERE a = 1 AND b');
-- ... and ranges of 7 keys: two, for two participants of the three
SET pg_lion.parallel_range_keys = 7;
SELECT lcp_counters('SELECT count(*) FROM lcp WHERE a = 1 AND b');
SET pg_lion.parallel_range_keys = 1;
-- a sum over a range, and a range taken as a source: the workers' counters
-- with the leader's, and without it when it takes no part
SELECT lcp_counters('SELECT count(*) FROM lcp WHERE k < 10 AND b');
SELECT lcp_counters('SELECT count(*) FROM lcp WHERE k < 10 AND s < 5 AND b');
SET parallel_leader_participation = off;
SELECT lcp_counters('SELECT count(*) FROM lcp WHERE k < 10 AND b');
SELECT lcp_counters('SELECT count(*) FROM lcp WHERE k < 10 AND s < 5 AND b');
RESET parallel_leader_participation;

-- ---------- 4. a generic plan's parameters, in every participant ----------
SET plan_cache_mode = force_generic_plan;
PREPARE lcp_p(int, boolean, int) AS
	SELECT count(*), count(*) FILTER (WHERE k = $3) FROM lcp WHERE a = $1 AND b = $2;
EXPLAIN (COSTS OFF) EXECUTE lcp_p(1, true, 2);
EXECUTE lcp_p(1, true, 2);
EXECUTE lcp_p(2, false, 39);
-- a value with no entry: every participant counts nothing
EXECUTE lcp_p(9, true, 2);
-- a query known only at run time (§17), and one no key narrows: the heap
-- is read then, a range at a time
PREPARE lcp_q(tsquery) AS SELECT count(*) FROM lcp WHERE w @@ $1;
EXPLAIN (COSTS OFF) EXECUTE lcp_q('red | cat');
EXECUTE lcp_q('red | cat');
EXECUTE lcp_q('(grey & !fox) | (blue & owl)');
EXECUTE lcp_q('!red');
SELECT lcp_counters($$EXECUTE lcp_q('!red')$$);
PREPARE lcp_r(int[]) AS SELECT count(*) FROM lcp WHERE k = ANY ($1);
EXPLAIN (COSTS OFF) EXECUTE lcp_r('{1, 2}');
EXECUTE lcp_r('{1, 2, 3, 39, 40}');
SET pg_lion.enable_count_pushdown = off;
SET max_parallel_workers_per_gather = 0;
SELECT count(*), count(*) FILTER (WHERE k = 2) FROM lcp WHERE a = 1 AND b;
SELECT count(*), count(*) FILTER (WHERE k = 39) FROM lcp WHERE a = 2 AND NOT b;
SELECT count(*), count(*) FILTER (WHERE k = 2) FROM lcp WHERE a = 9 AND b;
SELECT count(*) FROM lcp WHERE w @@ 'red | cat'::tsquery;
SELECT count(*) FROM lcp WHERE w @@ '(grey & !fox) | (blue & owl)'::tsquery;
SELECT count(*) FROM lcp WHERE w @@ '!red'::tsquery;
SELECT count(*) FROM lcp WHERE k = ANY ('{1, 2, 3, 39, 40}');
RESET pg_lion.enable_count_pushdown;
SET max_parallel_workers_per_gather = 2;
DEALLOCATE lcp_p;
DEALLOCATE lcp_q;
DEALLOCATE lcp_r;
RESET plan_cache_mode;
-- an InitPlan's value, which the Gather hands its workers
SELECT lcp_check($$SELECT count(*) FROM lcp WHERE tags && (SELECT array_agg(g) FROM generate_series(1, 3) g)$$);
SELECT lcp_check('SELECT count(*) FROM lcp WHERE k = ANY ((SELECT array_agg(g) FROM generate_series(3, 9) g)::int[]) AND b');

-- ---------- 5. an IN list too long to locate at once ----------
-- a batch is a work_mem of located sets, never fewer than a thousand values:
-- the ranges take the batches whole, a part of them each, and every batch is
-- located once.  The list's length is the run's to see, and the plan prices
-- it short: containers priced dear make the parallel plan the cheaper.
SET work_mem = '64kB';
SET pg_lion.container_cost = 1000;
SELECT lcp_check('SELECT count(*) FROM lcp WHERE id = ANY (array(SELECT g * 3 FROM generate_series(1, 5000) g))');
SELECT lcp_check('SELECT count(*) FROM lcp WHERE id = ANY (array(SELECT g * 3 FROM generate_series(1, 5000) g)) AND b');
SELECT lcp_check('SELECT count(*) FROM lcp WHERE id = ANY (array(SELECT g * 7 FROM generate_series(1, 5000) g)) AND a IN (1, 2)');
SELECT lcp_counters('SELECT count(*) FROM lcp WHERE id = ANY (array(SELECT g * 3 FROM generate_series(1, 5000) g)) AND b');
RESET pg_lion.container_cost;
RESET work_mem;

-- ---------- 6. rows the visibility map cannot vouch for ----------
-- new versions of a tenth of the rows, and a few deleted, not vacuumed:
-- their pages go to the heap, in whichever participant counts their range
UPDATE lcp SET a = (a + 1) % 4 WHERE id % 10 = 0;
DELETE FROM lcp WHERE id % 97 = 0;
SELECT lcp_check('SELECT count(*) FROM lcp WHERE a = 1 AND b');
SELECT lcp_check('SELECT count(*) FROM lcp WHERE a IN (1, 3) AND NOT b');
SELECT lcp_check('SELECT count(*), count(*) FILTER (WHERE s = 3) FROM lcp WHERE a = 1');
SELECT lcp_check($$SELECT count(*) FROM lcp WHERE w @@ '(red & fox) | (blue & !owl) | grey'::tsquery$$);
SELECT lcp_check('SELECT count(*) FROM lcp WHERE k < 10 AND b');
SELECT lcp_check('SELECT count(g) FROM lcp');
SELECT lcp_check('SELECT count(*) FROM lcp WHERE a = 1 AND b AND id % 7 = 0');
VACUUM lcp;
SELECT lcp_check('SELECT count(*) FROM lcp WHERE a = 1 AND b');

-- ---------- 7. the shapes that stay serial ----------
-- count(DISTINCT), the aggregates over keys, a group folded to a constant
SELECT lcp_check('SELECT count(DISTINCT k) FROM lcp WHERE a = 1 AND b');
SELECT lcp_check('SELECT sum(k), count(*) FROM lcp');
SELECT lcp_check('SELECT a, count(*) FROM lcp WHERE a = 1 AND b GROUP BY a');

-- ---------- 8. a summarized driver's NULL rows ----------
-- a sum over every row whose driver has summaries (§32, §35): the walk hands
-- out the summaries, which hold no NULL row, and the NULL entry is counted
-- beside it - in each range, confined to the range's keys as every set the
-- range counts is, so that the ranges add up to the serial count and not to
-- the NULL rows once for each.  k is NULL in a tenth of the rows and g in a
-- quarter; 500 pages, eight container keys, eight ranges.
CREATE TABLE lcp_sm (k int, g int, pad text NOT NULL)
	WITH (autovacuum_enabled = off);
INSERT INTO lcp_sm
SELECT CASE WHEN h % 10 = 0 THEN NULL ELSE h % 300 END,
	   CASE WHEN h % 4 = 1 THEN NULL ELSE (h / 7) % 1009 END, repeat('x', 30)
  FROM (SELECT i, abs(hashint4(i)) AS h FROM generate_series(1, 60000) i) r;
CREATE INDEX lcp_sm_k ON lcp_sm USING lion (k) WITH (summaries = on, summary_tids = 64);
CREATE INDEX lcp_sm_g ON lcp_sm USING lion (g);
VACUUM (FREEZE, ANALYZE) lcp_sm;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lcp_sm WHERE g <> 7;
SELECT lcp_check('SELECT count(*) FROM lcp_sm');
SELECT lcp_check('SELECT count(*) FROM lcp_sm WHERE g IS NOT NULL');
SELECT lcp_check('SELECT count(*) FROM lcp_sm WHERE g <> 7');
SELECT lcp_check('SELECT count(*), count(*) FILTER (WHERE g <> 7) FROM lcp_sm');
-- the column's own IS NOT NULL, which drops the NULL entry
SELECT lcp_check('SELECT count(*) FROM lcp_sm WHERE k IS NOT NULL');
-- 270 summaries and the NULL entry, summed in each range, and serially once
SELECT lcp_counters('SELECT count(*) FROM lcp_sm WHERE g <> 7',
					'{Key Ranges, Summaries Summed, Posting Sets Summed}');
SET max_parallel_workers_per_gather = 0;
SELECT lcp_counters('SELECT count(*) FROM lcp_sm WHERE g <> 7',
					'{Key Ranges, Summaries Summed, Posting Sets Summed}');
SET max_parallel_workers_per_gather = 2;
DROP TABLE lcp_sm;

DROP TABLE lcp;
DROP FUNCTION lcp_check(text);
DROP FUNCTION lcp_counters(text, text[]);
