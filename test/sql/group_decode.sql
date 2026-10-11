-- GROUP BY three or more lion-indexed columns: the decoded walk (DESIGN.md
-- §34).
--
-- At each container key every row is in exactly one value of each grouping
-- column, so the key's rows are told apart instead of every combination of
-- values being ANDed: each column's containers there are decoded into the
-- value each row has, and the rows the WHERE keeps are counted under their
-- combinations - one pass over the rows however many combinations there are.
-- Its rows are partial counts under a Finalize Agg.  Every query below is
-- checked against a sequential scan with the pushdown off; lgd_check() says
-- whether the walk ran, in how many passes, whether its tally spilled, whether
-- a range of the WHERE was collected into a file and whether rows went to the
-- heap.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

/*
 * lgd_check() runs q with every other scan disabled, so that a LionCount path
 * that is built at all is the plan, under EXPLAIN ANALYZE and again for its
 * rows; then as a sequential scan with the pushdown off, and proves the two
 * answers equal as multisets.
 */
CREATE FUNCTION lgd_check(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	decoded boolean := false;
	passes int := 0;
	spills int := 0;
	rspill boolean := false;
	rechecked bigint := 0;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF ln ~ 'Group Strategy: Decoded' THEN
			decoded := true;
		ELSIF ln ~ 'Decoded Passes: ' THEN
			passes := substring(ln FROM 'Decoded Passes: (\d+)')::int;
		ELSIF ln ~ 'Tally Spills: ' THEN
			spills := substring(ln FROM 'Tally Spills: (\d+)')::int;
		ELSIF ln ~ 'Range Sources Spilled: ' THEN
			rspill := true;
		ELSIF ln ~ 'Decoded Rows Rechecked: ' THEN
			rechecked := substring(ln FROM 'Decoded Rows Rechecked: (\d+)')::bigint;
		END IF;
	END LOOP;
	EXECUTE 'CREATE TEMP TABLE lgd_on AS ' || q;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE 'CREATE TEMP TABLE lgd_off AS ' || q;
	SELECT count(*) INTO nrows FROM lgd_off;
	SELECT (SELECT count(*) FROM (TABLE lgd_on EXCEPT ALL TABLE lgd_off) x) +
		   (SELECT count(*) FROM (TABLE lgd_off EXCEPT ALL TABLE lgd_on) y)
	  INTO ndiff;
	DROP TABLE lgd_on;
	DROP TABLE lgd_off;
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	RETURN format('%s, %s pass%s%s%s%s, %s rows, %s',
				  CASE WHEN decoded THEN 'decoded' ELSE 'not decoded' END,
				  passes, CASE WHEN passes = 1 THEN '' ELSE 'es' END,
				  CASE WHEN spills > 0 THEN ', tally spilled' ELSE '' END,
				  CASE WHEN rspill THEN ', range spilled' ELSE '' END,
				  CASE WHEN rechecked > 0 THEN ', rows rechecked in the heap' ELSE '' END,
				  nrows,
				  CASE WHEN ndiff = 0 THEN 'same as the sequential scan' ELSE 'DIFFERENT' END);
END $$;

-- Five grouping columns of 4, 10, 25 (with NULLs), 100 values and a boolean,
-- one multicolumn lion index over them, and a column of its own index.
CREATE TABLE lgd (a int, b int, c int, d int, e bool, g int, f text);
INSERT INTO lgd
SELECT i % 4, (i / 4) % 10,
	   CASE WHEN i % 17 = 0 THEN NULL ELSE (i * 7) % 25 END,
	   (i * 13) % 100, i % 3 = 0, (i / 7) % 6, 'r' || (i % 50)
  FROM generate_series(1, 60000) i;
CREATE INDEX lgd_l ON lgd USING lion (a, b, c, d, e);
CREATE INDEX lgd_g ON lgd USING lion (g);
CREATE INDEX lgd_f ON lgd USING lion (f);
VACUUM (FREEZE, ANALYZE) lgd;

-- The plan: the node under core's Finalize HashAggregate, every column named
-- with the index it is read from.
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
EXPLAIN (COSTS OFF) SELECT a, b, c, count(*) FROM lgd GROUP BY a, b, c;
EXPLAIN (COSTS OFF) SELECT a, g, f, count(*) FROM lgd GROUP BY a, g, f;
EXPLAIN (COSTS OFF) SELECT a, b, c, count(*) FROM lgd WHERE d > 50 GROUP BY a, b, c;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
RESET enable_indexonlyscan;

-- Three, four and five columns; the NULL group of c, and count(c), which is
-- 0 there; the columns in any order in the GROUP BY and the target list.
SELECT lgd_check('SELECT a, b, c, count(*) AS n FROM lgd GROUP BY a, b, c');
SELECT lgd_check('SELECT c, a, count(c) AS nc, count(*) AS n, b FROM lgd GROUP BY b, c, a');
SELECT lgd_check('SELECT a, b, c, d, count(*) AS n FROM lgd GROUP BY a, b, c, d');
SELECT lgd_check('SELECT a, b, c, d, e, count(*) AS n FROM lgd GROUP BY a, b, c, d, e');

-- Two columns: the nested loop of DESIGN.md §20 or the walk, whichever is
-- cheaper - the loop for a few pairs, the walk for many.  With
-- pg_lion.enable_decoded_walk off, neither two columns nor three take it.
SELECT lgd_check('SELECT a, e, count(*) AS n FROM lgd GROUP BY a, e');
SELECT lgd_check('SELECT c, d, count(*) AS n FROM lgd GROUP BY c, d');
SELECT lgd_check('SELECT c, d, count(*) AS n FROM lgd WHERE e GROUP BY c, d');
SET pg_lion.enable_decoded_walk = off;
SELECT lgd_check('SELECT c, d, count(*) AS n FROM lgd GROUP BY c, d');
SELECT lgd_check('SELECT a, b, c, count(*) AS n FROM lgd GROUP BY a, b, c');
RESET pg_lion.enable_decoded_walk;

-- A column of another lion index among them, and a text column.
SELECT lgd_check('SELECT a, g, e, count(*) AS n FROM lgd GROUP BY a, g, e');
SELECT lgd_check('SELECT f, a, e, count(*) AS n FROM lgd GROUP BY f, a, e');

-- The WHERE, collected once: an equality, an IN list, a NOT, an OR across
-- columns, an IS NULL, a clause that selects nothing, and a column the WHERE
-- pins to one value printed beside the groups.
SELECT lgd_check('SELECT a, b, c, count(*) AS n FROM lgd WHERE e GROUP BY a, b, c');
SELECT lgd_check('SELECT a, b, c, count(*) AS n FROM lgd WHERE d IN (1, 2, 3, 97) GROUP BY a, b, c');
SELECT lgd_check('SELECT a, b, e, count(*) AS n FROM lgd WHERE NOT e AND g = 2 GROUP BY a, b, e');
SELECT lgd_check('SELECT a, b, d, count(*) AS n FROM lgd WHERE c IS NOT NULL AND g = 2 GROUP BY a, b, d');
SELECT lgd_check('SELECT a, b, e, count(*) AS n FROM lgd WHERE d = 5 OR g = 1 GROUP BY a, b, e');
SELECT lgd_check('SELECT a, b, d, count(*) AS n FROM lgd WHERE c IS NULL GROUP BY a, b, d');
SELECT lgd_check('SELECT a, b, c, count(*) AS n FROM lgd WHERE d = 1000 GROUP BY a, b, c');
SELECT lgd_check('SELECT g, a, b, c, count(*) AS n FROM lgd WHERE g = 3 GROUP BY a, b, c, g');

-- HAVING, ORDER BY and LIMIT, all above the Finalize Agg.
SELECT lgd_check('SELECT a, b, c, count(*) AS n FROM lgd GROUP BY a, b, c HAVING count(*) > 70');
SELECT a, b, c, count(*) FROM lgd WHERE e GROUP BY a, b, c ORDER BY count(*) DESC, a, b, c LIMIT 5;

-- A work_mem too small for one pass: the first column's values are taken a
-- few at a time and the others' in chunks.
SET work_mem = '64kB';
SELECT lgd_check('SELECT a, b, c, d, e, count(*) AS n FROM lgd GROUP BY a, b, c, d, e');
SELECT lgd_check('SELECT f, b, d, count(*) AS n FROM lgd WHERE a IN (1, 2) GROUP BY f, b, d');
RESET work_mem;

-- Prepared, as a generic plan: the WHERE's value a parameter.
SET plan_cache_mode = force_generic_plan;
PREPARE lgd_p(int) AS SELECT a, b, c, count(*) AS n FROM lgd WHERE d = $1 GROUP BY a, b, c;
CREATE TEMP TABLE lgd_p1 AS EXECUTE lgd_p(7);
CREATE TEMP TABLE lgd_p2 AS EXECUTE lgd_p(8);
SET pg_lion.enable_count_pushdown = off;
SELECT (SELECT count(*) FROM (TABLE lgd_p1 EXCEPT ALL SELECT a, b, c, count(*) FROM lgd WHERE d = 7 GROUP BY a, b, c) x) AS diff7,
	   (SELECT count(*) FROM (TABLE lgd_p2 EXCEPT ALL SELECT a, b, c, count(*) FROM lgd WHERE d = 8 GROUP BY a, b, c) y) AS diff8,
	   (SELECT count(*) FROM lgd_p1) AS rows7;
RESET pg_lion.enable_count_pushdown;
RESET plan_cache_mode;
DEALLOCATE lgd_p;

-- Rescanned, once per outer row of a nested loop.
SELECT lgd_check('SELECT v.x, s.* FROM (VALUES (1), (4), (9)) v(x), LATERAL (SELECT a, b, c, count(*) AS n FROM lgd WHERE d = v.x GROUP BY a, b, c) s');

-- A cursor that stops part way and goes on.
BEGIN;
DECLARE lgd_cur CURSOR FOR SELECT a, b, c, count(*) FROM lgd GROUP BY a, b, c ORDER BY a, b, c;
FETCH 3 FROM lgd_cur;
MOVE 200 IN lgd_cur;
FETCH 3 FROM lgd_cur;
CLOSE lgd_cur;
COMMIT;

-- Under SERIALIZABLE, the blocks counted from the visibility map are
-- predicate-locked page by page (and those locks promoted to one on the
-- relation once there are many), as an index-only scan locks them: the query
-- alone in its transaction, since a sequential scan would lock the relation.
SELECT lgd_check('SELECT a, b, c, count(*) AS n FROM lgd WHERE e GROUP BY a, b, c');
BEGIN ISOLATION LEVEL SERIALIZABLE;
SET LOCAL enable_seqscan = off;
SET LOCAL enable_bitmapscan = off;
SET LOCAL enable_indexscan = off;
SET LOCAL enable_indexonlyscan = off;
SELECT count(*) AS groups FROM (SELECT a, b, c, count(*) FROM lgd WHERE e GROUP BY a, b, c) s;
SELECT count(*) > 0 AS heap_locked FROM pg_locks
 WHERE locktype IN ('page', 'relation') AND relation = 'lgd'::regclass
   AND mode = 'SIReadLock';
COMMIT;

-- A heap the visibility map no longer vouches for: every changed block's
-- rows go to the heap, with their combinations.
UPDATE lgd SET b = (b + 1) % 10 WHERE d < 10;
DELETE FROM lgd WHERE d = 50;
INSERT INTO lgd SELECT 1, 2, NULL, 3, true, 4, 'new' FROM generate_series(1, 300);
SELECT lgd_check('SELECT a, b, c, count(*) AS n FROM lgd GROUP BY a, b, c');
SELECT lgd_check('SELECT a, b, c, d, count(*) AS n FROM lgd WHERE e GROUP BY a, b, c, d');

-- A range in the WHERE (DESIGN.md §34, "A range in the WHERE"): a source of
-- the collected copy, on a column outside the grouping or on one of its own -
-- whose NULL group it leaves out - with another clause, with a second range,
-- as an OR's leaf, empty, and contradicted; on the heap above, with rows
-- updated across the range's bounds, deleted and inserted since the VACUUM.
UPDATE lgd SET d = d + 1 WHERE d BETWEEN 48 AND 52;
INSERT INTO lgd SELECT 1, NULL, 3, NULL, NULL, NULL, NULL FROM generate_series(1, 50);
SELECT lgd_check('SELECT a, b, c, count(*) AS n FROM lgd WHERE d > 50 GROUP BY a, b, c');
SELECT lgd_check('SELECT a, b, c, count(*) AS n FROM lgd WHERE d >= 10 AND d <= 20 AND e GROUP BY a, b, c');
SELECT lgd_check('SELECT a, b, c, count(*) AS n FROM lgd WHERE c < 10 GROUP BY a, b, c');
SELECT lgd_check('SELECT a, b, e, count(*) AS n FROM lgd WHERE d > 90 AND g < 3 GROUP BY a, b, e');
SELECT lgd_check('SELECT a, b, e, count(*) AS n FROM lgd WHERE d < 5 OR g = 1 GROUP BY a, b, e');
SELECT lgd_check('SELECT a, b, c, count(*) AS n FROM lgd WHERE f > ''r3'' GROUP BY a, b, c');
SELECT lgd_check('SELECT c, d, count(*) AS n FROM lgd WHERE g > 2 GROUP BY c, d');
SELECT lgd_check('SELECT a, b, c, count(*) AS n FROM lgd WHERE d < 0 GROUP BY a, b, c');
SELECT lgd_check('SELECT a, b, c, count(*) AS n FROM lgd WHERE d > 50 AND d < 40 GROUP BY a, b, c');
SET work_mem = '64kB';
SELECT lgd_check('SELECT a, b, c, d, count(*) AS n FROM lgd WHERE g >= 1 AND d > 3 GROUP BY a, b, c, d');
RESET work_mem;
VACUUM (FREEZE, ANALYZE) lgd;
SELECT lgd_check('SELECT a, b, c, count(*) AS n FROM lgd GROUP BY a, b, c');

-- A range too large for the memory, which a count beside no decoded walk
-- walks at every count instead (DESIGN.md §32), is collected into a file:
-- 400,000 rows, half of them in the range and spread over every container
-- key, against 64 kB, on a heap with rows updated, deleted and inserted since
-- its VACUUM.
CREATE TABLE lgr (a int, b int, c int, d int);
INSERT INTO lgr
SELECT i % 3, (i / 3) % 5, CASE WHEN i % 11 = 0 THEN NULL ELSE i % 7 END,
	   (i * 37) % 1000
  FROM generate_series(1, 400000) i;
CREATE INDEX lgr_l ON lgr USING lion (a, b, c, d);
VACUUM (FREEZE, ANALYZE) lgr;
UPDATE lgr SET d = d + 1 WHERE d % 50 = 0;
DELETE FROM lgr WHERE d % 97 = 0;
INSERT INTO lgr SELECT 2, NULL, 4, 300 FROM generate_series(1, 100);
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SELECT lgd_check('SELECT a, b, c, count(*) AS n FROM lgr WHERE d < 500 GROUP BY a, b, c');
SELECT lgd_check('SELECT a, b, c, count(*) AS n FROM lgr WHERE d > 5 AND d < 990 AND b > 0 GROUP BY a, b, c');
RESET work_mem;
RESET hash_mem_multiplier;
DROP TABLE lgr;

-- Not the decoded walk: a WHERE of a negated clause alone (its copy has no
-- positive clause to begin from), a grouping expression, a column no lion
-- index has, and a partitioned table.
SELECT lgd_check('SELECT a, b, d, count(*) AS n FROM lgd WHERE c IS NOT NULL GROUP BY a, b, d');
SELECT lgd_check('SELECT a, b, c + 1 AS c1, count(*) AS n FROM lgd GROUP BY a, b, c + 1');
ALTER TABLE lgd ADD COLUMN h int;
SELECT lgd_check('SELECT a, b, h, count(*) AS n FROM lgd GROUP BY a, b, h');
CREATE TABLE lgdp (a int, b int, c int) PARTITION BY LIST (a);
CREATE TABLE lgdp0 PARTITION OF lgdp FOR VALUES IN (0, 1);
CREATE TABLE lgdp1 PARTITION OF lgdp FOR VALUES IN (2, 3);
INSERT INTO lgdp SELECT a, b, c FROM lgd;
CREATE INDEX ON lgdp USING lion (a, b, c);
VACUUM (FREEZE, ANALYZE) lgdp;
SELECT lgd_check('SELECT a, b, c, count(*) AS n FROM lgdp GROUP BY a, b, c');

DROP TABLE lgdp;
DROP TABLE lgd;
DROP FUNCTION lgd_check(text);
