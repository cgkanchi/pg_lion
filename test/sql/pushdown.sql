-- The RoaringCount CustomScan (DESIGN.md section 10).
--
-- Two things are checked for every query: that the plan is (or deliberately
-- is not) the custom node, and that the rows it produces are exactly the rows
-- the ordinary plan produces.  The second check is done inside the test with
-- EXCEPT ALL in both directions, so a wrong answer fails even if it happens
-- to be stable.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'roaring_index';

/*
 * The visibility map is what makes this feature worth having, and a heap page
 * only becomes all-visible once the inserting transaction's commit record has
 * reached disk.  The dev cluster runs with synchronous_commit = off, so ask
 * for flushed commits here; otherwise whether VACUUM can set the bits depends
 * on how fast the WAL writer happens to be.
 */
SET synchronous_commit = on;

/*
 * Plans are compared literally below, so the statistics must not depend on
 * ANALYZE's random sample: a target of 1000 makes it read every row of the
 * 100k-row table used here.
 */
SET default_statistics_target = 1000;

CREATE EXTENSION IF NOT EXISTS roaring_index;

/*
 * rbi_pd() runs one query twice, once with the pushdown enabled and once
 * without, and reports whether the custom node was used and how many rows
 * came out - after proving the two result sets are equal as multisets.
 */
CREATE OR REPLACE FUNCTION rbi_pd(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	pushed boolean := false;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('roaring_index.enable_count_pushdown', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (RoaringCount)%' THEN
			pushed := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE rbi_pd_on AS %s', q);

	PERFORM set_config('roaring_index.enable_count_pushdown', 'off', true);
	EXECUTE format('CREATE TEMP TABLE rbi_pd_off AS %s', q);
	PERFORM set_config('roaring_index.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM rbi_pd_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM rbi_pd_on EXCEPT ALL SELECT * FROM rbi_pd_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM rbi_pd_off EXCEPT ALL SELECT * FROM rbi_pd_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE rbi_pd_on, rbi_pd_off';

	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN pushed THEN 'pushed down' ELSE 'not pushed down' END,
				  nrows);
END $$;

/* Show both plans for one query. */
CREATE OR REPLACE FUNCTION rbi_plans(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('roaring_index.enable_count_pushdown', 'on', true);
	RETURN NEXT '-- on:';
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		RETURN NEXT ln;
	END LOOP;
	PERFORM set_config('roaring_index.enable_count_pushdown', 'off', true);
	RETURN NEXT '-- off:';
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		RETURN NEXT ln;
	END LOOP;
	PERFORM set_config('roaring_index.enable_count_pushdown', 'on', true);
END $$;

/*
 * The instrumentation EXPLAIN ANALYZE prints for the custom node, reduced to
 * zero / non-zero: the exact counts depend on how the rows fall on heap
 * pages, but whether the heap had to be visited at all does not.
 */
CREATE OR REPLACE FUNCTION rbi_pd_counters(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	nm text;
	val bigint;
	pushed boolean := false;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (RoaringCount)%' THEN
			pushed := true;
		END IF;
		nm := btrim(split_part(ln, ':', 1));
		IF nm IN ('Heap Blocks Skipped via VM', 'Heap TIDs Rechecked',
				  'Heap Blocks Rechecked', 'Containers Visited') THEN
			val := btrim(split_part(ln, ':', 2))::bigint;
			RETURN NEXT format('%s %s', nm,
							   CASE WHEN val = 0 THEN '= 0' ELSE '> 0' END);
		END IF;
	END LOOP;
	IF NOT pushed THEN
		RETURN NEXT 'NOT PUSHED DOWN';
	END IF;
END $$;

CREATE TABLE rbi_pdt (
	id	int		NOT NULL,
	a	int		NOT NULL,
	b	int		NOT NULL,
	c	text	NOT NULL,
	n	int
);
INSERT INTO rbi_pdt
SELECT i,
	   i % 10,
	   i % 7,
	   'c' || (i % 4),
	   CASE WHEN i % 101 = 0 THEN NULL ELSE i % 5 END
  FROM generate_series(1, 100000) i;

CREATE INDEX rbi_pdt_a ON rbi_pdt USING roaring (a);
CREATE INDEX rbi_pdt_b ON rbi_pdt USING roaring (b);
CREATE INDEX rbi_pdt_c ON rbi_pdt USING roaring (c);
CREATE INDEX rbi_pdt_n ON rbi_pdt USING roaring (n);
-- The pushdown only wins when most of the heap is all-visible, which is the
-- case it exists for; VACUUM makes that true and deterministic here.
VACUUM ANALYZE rbi_pdt;

-- ---- plans -------------------------------------------------------------
SELECT rbi_plans('SELECT count(*) FROM rbi_pdt WHERE a = 3');
SELECT rbi_plans('SELECT count(*) FROM rbi_pdt WHERE a = 3 AND b = 2');
SELECT rbi_plans($$SELECT count(*) FROM rbi_pdt WHERE a = 3 AND b = 2 AND c = 'c1'$$);
SELECT rbi_plans('SELECT a, count(*) FROM rbi_pdt GROUP BY a');
SELECT rbi_plans('SELECT a, count(*) FROM rbi_pdt WHERE b = 2 GROUP BY a');
SELECT rbi_plans('SELECT count(*) FROM rbi_pdt GROUP BY a');
SELECT rbi_plans('SELECT count(a) FROM rbi_pdt WHERE a = 3');
SELECT rbi_plans('SELECT a, count(*) FROM rbi_pdt WHERE a = 3 GROUP BY a');
SELECT rbi_plans('SELECT n, count(*) FROM rbi_pdt GROUP BY n');
SELECT rbi_plans('SELECT count(*) FROM rbi_pdt WHERE n IS NULL');

-- ---- shapes that must push down ----------------------------------------
SELECT rbi_pd('SELECT count(*) FROM rbi_pdt WHERE a = 3');
SELECT rbi_pd('SELECT count(*) FROM rbi_pdt WHERE a = 3 AND b = 2');
SELECT rbi_pd($$SELECT count(*) FROM rbi_pdt WHERE a = 3 AND b = 2 AND c = 'c1'$$);
SELECT rbi_pd($$SELECT count(*) FROM rbi_pdt WHERE c = 'c2'$$);
-- the constant on the left, and a cross-type constant
SELECT rbi_pd('SELECT count(*) FROM rbi_pdt WHERE 3 = a');
SELECT rbi_pd('SELECT count(*) FROM rbi_pdt WHERE a = 3::int8');
-- a constant that matches no key at all
SELECT rbi_pd('SELECT count(*) FROM rbi_pdt WHERE a = 999');
-- the same column constrained twice with the same constant
SELECT rbi_pd('SELECT count(*) FROM rbi_pdt WHERE a = 3 AND a = 3');
-- GROUP BY, with and without a WHERE clause
SELECT rbi_pd('SELECT a, count(*) FROM rbi_pdt GROUP BY a');
SELECT rbi_pd('SELECT a, count(*) FROM rbi_pdt WHERE b = 2 GROUP BY a');
SELECT rbi_pd($$SELECT a, count(*) FROM rbi_pdt WHERE b = 2 AND c = 'c1' GROUP BY a$$);
SELECT rbi_pd('SELECT count(*) FROM rbi_pdt GROUP BY a');
SELECT rbi_pd('SELECT b, count(*) FROM rbi_pdt WHERE b = 2 GROUP BY b');
/*
 * GROUP BY a column the WHERE clause pins to a single value.  The planner
 * folds such a column out of the group clause, so the node emits at most one
 * row and reports the key the index stored for it - which is what makes a
 * cross-type constant safe here.
 */
SELECT rbi_pd('SELECT a, count(*) FROM rbi_pdt WHERE a = 3 GROUP BY a');
SELECT rbi_pd('SELECT a, count(*) FROM rbi_pdt WHERE a = 3::int8 GROUP BY a');
SELECT rbi_pd('SELECT a, count(*) FROM rbi_pdt WHERE a = 999 GROUP BY a');
SELECT rbi_pd('SELECT a, b, count(*) FROM rbi_pdt WHERE a = 3 GROUP BY a, b');
SELECT rbi_pd($$SELECT c, count(*) FROM rbi_pdt WHERE c = 'c2' GROUP BY c$$);
SELECT rbi_pd('SELECT b, count(*) FROM rbi_pdt WHERE b = 2 GROUP BY b');
-- count(col) where col cannot be NULL
SELECT rbi_pd('SELECT count(a) FROM rbi_pdt WHERE a = 3');
SELECT rbi_pd('SELECT a, count(a) FROM rbi_pdt GROUP BY a');
SELECT rbi_pd('SELECT count(*) AS c1, count(a) AS c2, count(*) AS c3 FROM rbi_pdt WHERE a = 3');

/*
 * A nullable group column is fine: the NULL keys have an entry of their own,
 * so the NULL group is produced like any other (DESIGN.md section 14, and
 * test/sql/null.sql for the whole story).
 */
SELECT rbi_pd('SELECT n, count(*) FROM rbi_pdt GROUP BY n');

-- ---- shapes that must NOT push down ------------------------------------
-- count() of a column that neither the GROUP BY nor a WHERE clause constrains
SELECT rbi_pd('SELECT count(n) FROM rbi_pdt WHERE a = 3');
-- HAVING
SELECT rbi_pd('SELECT a, count(*) FROM rbi_pdt GROUP BY a HAVING count(*) > 9000');
-- an aggregate we cannot answer
SELECT rbi_pd('SELECT sum(id) FROM rbi_pdt WHERE a = 3');
SELECT rbi_pd('SELECT count(DISTINCT b) FROM rbi_pdt WHERE a = 3');
SELECT rbi_pd('SELECT count(*) FILTER (WHERE b = 2) FROM rbi_pdt WHERE a = 3');
-- a qual that is not an indexed equality to a constant
SELECT rbi_pd('SELECT count(*) FROM rbi_pdt WHERE a = 3 AND id < 500');
SELECT rbi_pd('SELECT count(*) FROM rbi_pdt WHERE a > 3');
SELECT rbi_pd('SELECT count(*) FROM rbi_pdt WHERE a = 3 AND id = 5');
-- two different constants on one column
SELECT rbi_pd('SELECT count(*) FROM rbi_pdt WHERE a = 3 AND a = 4');
-- no equality key and no GROUP BY at all
SELECT rbi_pd('SELECT count(*) FROM rbi_pdt');
-- GROUP BY an unindexed column, and by more than one column
SELECT rbi_pd('SELECT id, count(*) FROM rbi_pdt WHERE a = 3 GROUP BY id');
SELECT rbi_pd('SELECT a, b, count(*) FROM rbi_pdt GROUP BY a, b');
-- GROUP BY an expression
SELECT rbi_pd('SELECT a + 1, count(*) FROM rbi_pdt GROUP BY a + 1');
-- a join
SELECT rbi_pd('SELECT count(*) FROM rbi_pdt x, rbi_pdt y WHERE x.a = 3 AND y.b = 2');

/*
 * Applicable, but the cost model prefers the ordinary plan: a key the
 * statistics say matches nothing makes the bitmap plan look almost free.
 */
SELECT rbi_pd('SELECT count(*) FROM rbi_pdt WHERE a = 3 AND b = 999');
-- ORDER BY is fine, but the node produces no ordering of its own
SELECT rbi_pd('SELECT a, count(*) FROM rbi_pdt GROUP BY a ORDER BY a');
SELECT rbi_plans('SELECT a, count(*) FROM rbi_pdt GROUP BY a ORDER BY a');

-- ---- the results themselves --------------------------------------------
SELECT a, count(*) FROM rbi_pdt GROUP BY a ORDER BY a;
SELECT a, count(*) FROM rbi_pdt WHERE b = 2 GROUP BY a ORDER BY a;
SELECT count(*) FROM rbi_pdt WHERE a = 3 AND b = 2;
SELECT count(*) FROM rbi_pdt WHERE a = 3 AND b = 2 AND c = 'c1';
SELECT count(*) FROM rbi_pdt WHERE a = 999;
SELECT a, count(*) FROM rbi_pdt WHERE a = 3 GROUP BY a;
SELECT a, b, count(*) FROM rbi_pdt WHERE a = 3 GROUP BY a, b ORDER BY b;

-- ---- after a DELETE, before VACUUM (the heap recheck path) -------------
DELETE FROM rbi_pdt WHERE id % 3 = 0;

SELECT rbi_pd('SELECT count(*) FROM rbi_pdt WHERE a = 3');
SELECT rbi_pd('SELECT count(*) FROM rbi_pdt WHERE a = 3 AND b = 2');
SELECT rbi_pd('SELECT a, count(*) FROM rbi_pdt GROUP BY a');
SELECT rbi_pd('SELECT a, count(*) FROM rbi_pdt WHERE b = 2 GROUP BY a');
SELECT a, count(*) FROM rbi_pdt GROUP BY a ORDER BY a;

-- ---- and after VACUUM (the visibility-map path) ------------------------
VACUUM rbi_pdt;

SELECT rbi_pd('SELECT count(*) FROM rbi_pdt WHERE a = 3');
SELECT rbi_pd('SELECT count(*) FROM rbi_pdt WHERE a = 3 AND b = 2');
SELECT rbi_pd('SELECT a, count(*) FROM rbi_pdt GROUP BY a');
SELECT rbi_pd('SELECT a, count(*) FROM rbi_pdt WHERE b = 2 GROUP BY a');
SELECT a, count(*) FROM rbi_pdt GROUP BY a ORDER BY a;

-- ---- a group whose rows have all been deleted disappears ---------------
DELETE FROM rbi_pdt WHERE a = 5;
SELECT rbi_pd('SELECT a, count(*) FROM rbi_pdt GROUP BY a');
SELECT a, count(*) FROM rbi_pdt GROUP BY a ORDER BY a;
VACUUM rbi_pdt;
SELECT rbi_pd('SELECT a, count(*) FROM rbi_pdt GROUP BY a');
SELECT a, count(*) FROM rbi_pdt GROUP BY a ORDER BY a;

-- ---- an empty table ----------------------------------------------------
CREATE TABLE rbi_pde (k int NOT NULL);
CREATE INDEX rbi_pde_k ON rbi_pde USING roaring (k);
VACUUM ANALYZE rbi_pde;
SELECT rbi_pd('SELECT count(*) FROM rbi_pde WHERE k = 1');
SELECT rbi_pd('SELECT k, count(*) FROM rbi_pde GROUP BY k');
SELECT count(*) FROM rbi_pde WHERE k = 1;

-- ---- the node inside a larger plan --------------------------------------
SELECT rbi_pd('SELECT x, c FROM (VALUES (1), (2)) v(x), LATERAL (SELECT count(*) c FROM rbi_pdt WHERE a = 3) s');
SELECT rbi_pd('SELECT * FROM (SELECT a, count(*) AS c FROM rbi_pdt GROUP BY a) s WHERE c > 0');

-- ---- what EXPLAIN ANALYZE reports ---------------------------------------
VACUUM rbi_pdt;
SELECT rbi_pd_counters('SELECT count(*) FROM rbi_pdt WHERE a = 3');
-- the pages a DELETE dirties are no longer all-visible, so their TIDs are
-- fetched from the heap and counted as rechecked blocks
DELETE FROM rbi_pdt WHERE id % 500 = 0;
SELECT rbi_pd_counters('SELECT count(*) FROM rbi_pdt WHERE a = 3');

-- ---- the cost model refuses a heap that is not all-visible --------------
/*
 * A thousand groups whose rows are spread over every heap page, on a heap the
 * visibility map cannot vouch for: every TID would have to be fetched from
 * the heap, which is more work than the sequential scan does, so the planner
 * must not choose the pushdown.
 */
CREATE TABLE rbi_pdd (g int NOT NULL, pad text NOT NULL);
INSERT INTO rbi_pdd
SELECT i % 1000, repeat('x', 200) FROM generate_series(1, 100000) i;
CREATE INDEX rbi_pdd_g ON rbi_pdd USING roaring (g);
ANALYZE rbi_pdd;			-- no VACUUM: relallvisible stays 0
SELECT rbi_pd('SELECT g, count(*) FROM rbi_pdd GROUP BY g');
-- once it is all-visible the same query is worth pushing down
VACUUM ANALYZE rbi_pdd;
SELECT rbi_pd('SELECT g, count(*) FROM rbi_pdd GROUP BY g');

DROP TABLE rbi_pdd;
DROP TABLE rbi_pde;
DROP TABLE rbi_pdt;
DROP FUNCTION rbi_pd(text);
DROP FUNCTION rbi_pd_counters(text);
DROP FUNCTION rbi_plans(text);
