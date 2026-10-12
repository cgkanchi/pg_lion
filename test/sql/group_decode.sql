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
-- whether the walk ran, in how many passes, whether its tally spilled and
-- whether rows went to the heap.
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
	RETURN format('%s, %s pass%s%s%s, %s rows, %s',
				  CASE WHEN decoded THEN 'decoded' ELSE 'not decoded' END,
				  passes, CASE WHEN passes = 1 THEN '' ELSE 'es' END,
				  CASE WHEN spills > 0 THEN ', tally spilled' ELSE '' END,
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

-- A WHERE whose only clause that selects rows is a multi-key query no key
-- narrows (DESIGN.md §17, "A query known only at run time"): a generic plan's
-- `tags @> $1` run with '{}', which every array but a NULL one contains, and
-- a prefix that matches more lexemes than a query may expand to.  There is
-- no WHERE copy to collect: the walk reads every row and the heap tests the
-- query, and a `<>` beside it.  A narrowing array is the copy as before.
CREATE TABLE lgdm (id int, a int, b int, c int, tags int[], d tsvector);
INSERT INTO lgdm
SELECT i, i % 4, (i / 4) % 5, i % 7,
	   CASE WHEN i % 17 = 0 THEN NULL ELSE ARRAY[i % 10, 10 + i % 7] END,
	   to_tsvector('simple', 'w' || i || ' x' || (i % 7))
  FROM generate_series(1, 6000) i;
CREATE INDEX lgdm_a ON lgdm USING lion (a);
CREATE INDEX lgdm_b ON lgdm USING lion (b);
CREATE INDEX lgdm_c ON lgdm USING lion (c);
CREATE INDEX lgdm_tags ON lgdm USING lion (tags);
CREATE INDEX lgdm_d ON lgdm USING lion (d);
VACUUM (FREEZE, ANALYZE) lgdm;
DELETE FROM lgdm WHERE id % 10 = 3;
SET plan_cache_mode = force_generic_plan;
PREPARE lgdm_p(int[]) AS SELECT a, b, c, count(*) AS n FROM lgdm WHERE tags @> $1 GROUP BY a, b, c;
PREPARE lgdm_q(int[]) AS SELECT a, b, c, count(*) AS n FROM lgdm WHERE tags @> $1 AND c <> 2 GROUP BY a, b, c;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
EXPLAIN (COSTS OFF) EXECUTE lgdm_p('{}');
CREATE TEMP TABLE lgdm_p1 AS EXECUTE lgdm_p('{}');
CREATE TEMP TABLE lgdm_p2 AS EXECUTE lgdm_p('{4, 12}');
CREATE TEMP TABLE lgdm_q1 AS EXECUTE lgdm_q('{}');
CREATE TEMP TABLE lgdm_q2 AS EXECUTE lgdm_q('{4, 12}');
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
RESET enable_indexonlyscan;
SET pg_lion.enable_count_pushdown = off;
CREATE TEMP TABLE lgdm_s1 AS SELECT a, b, c, count(*) AS n FROM lgdm WHERE tags @> '{}' GROUP BY a, b, c;
CREATE TEMP TABLE lgdm_s2 AS SELECT a, b, c, count(*) AS n FROM lgdm WHERE tags @> '{4, 12}' GROUP BY a, b, c;
CREATE TEMP TABLE lgdm_t1 AS SELECT a, b, c, count(*) AS n FROM lgdm WHERE tags @> '{}' AND c <> 2 GROUP BY a, b, c;
CREATE TEMP TABLE lgdm_t2 AS SELECT a, b, c, count(*) AS n FROM lgdm WHERE tags @> '{4, 12}' AND c <> 2 GROUP BY a, b, c;
RESET pg_lion.enable_count_pushdown;
SELECT x.q, x.rows, x.n,
	   (SELECT count(*) FROM ((SELECT * FROM lgdm_p1 WHERE x.q = 'empty') EXCEPT ALL (SELECT * FROM lgdm_s1 WHERE x.q = 'empty')) d) +
	   (SELECT count(*) FROM ((SELECT * FROM lgdm_s1 WHERE x.q = 'empty') EXCEPT ALL (SELECT * FROM lgdm_p1 WHERE x.q = 'empty')) d) +
	   (SELECT count(*) FROM ((SELECT * FROM lgdm_p2 WHERE x.q = 'narrowing') EXCEPT ALL (SELECT * FROM lgdm_s2 WHERE x.q = 'narrowing')) d) +
	   (SELECT count(*) FROM ((SELECT * FROM lgdm_s2 WHERE x.q = 'narrowing') EXCEPT ALL (SELECT * FROM lgdm_p2 WHERE x.q = 'narrowing')) d) +
	   (SELECT count(*) FROM ((SELECT * FROM lgdm_q1 WHERE x.q = 'empty, c <> 2') EXCEPT ALL (SELECT * FROM lgdm_t1 WHERE x.q = 'empty, c <> 2')) d) +
	   (SELECT count(*) FROM ((SELECT * FROM lgdm_t1 WHERE x.q = 'empty, c <> 2') EXCEPT ALL (SELECT * FROM lgdm_q1 WHERE x.q = 'empty, c <> 2')) d) +
	   (SELECT count(*) FROM ((SELECT * FROM lgdm_q2 WHERE x.q = 'narrowing, c <> 2') EXCEPT ALL (SELECT * FROM lgdm_t2 WHERE x.q = 'narrowing, c <> 2')) d) +
	   (SELECT count(*) FROM ((SELECT * FROM lgdm_t2 WHERE x.q = 'narrowing, c <> 2') EXCEPT ALL (SELECT * FROM lgdm_q2 WHERE x.q = 'narrowing, c <> 2')) d)
		 AS differ
  FROM (SELECT 'empty' AS q, count(*) AS rows, sum(n) AS n FROM lgdm_p1
		UNION ALL SELECT 'narrowing', count(*), sum(n) FROM lgdm_p2
		UNION ALL SELECT 'empty, c <> 2', count(*), sum(n) FROM lgdm_q1
		UNION ALL SELECT 'narrowing, c <> 2', count(*), sum(n) FROM lgdm_q2) x
 ORDER BY x.q;
DROP TABLE lgdm_p1, lgdm_p2, lgdm_q1, lgdm_q2, lgdm_s1, lgdm_s2, lgdm_t1, lgdm_t2;
RESET plan_cache_mode;
DEALLOCATE lgdm_p;
DEALLOCATE lgdm_q;
SELECT lgd_check('SELECT a, b, c, count(*) AS n FROM lgdm WHERE d @@ ''w:*'' GROUP BY a, b, c');
SELECT lgd_check('SELECT a, b, c, count(*) AS n FROM lgdm WHERE d @@ ''w:*'' AND c <> 2 GROUP BY a, b, c');
DROP TABLE lgdm;

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
VACUUM (FREEZE, ANALYZE) lgd;
SELECT lgd_check('SELECT a, b, c, count(*) AS n FROM lgd GROUP BY a, b, c');

-- Not the decoded walk: a range in the WHERE, a WHERE of a negated clause
-- alone (its copy has no positive clause to begin from), a grouping
-- expression, a column no lion index has, and a partitioned table.
SELECT lgd_check('SELECT a, b, c, count(*) AS n FROM lgd WHERE d > 50 GROUP BY a, b, c');
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
