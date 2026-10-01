-- A nested tree ANDed with a running intersection, built or probed
-- (DESIGN.md §29.11, "Trees probed").
--
-- A tsquery `(a | b | c) & (d | e)` is an AND of ORs, and an OR across
-- columns of ANDed clauses an OR of ANDs: a tree of its own, which an AND of
-- posting sets used to build whole at every key it was sought to - its
-- driver union's bitset image included - however few rows the intersection
-- had left.  Now the tree is lazy under a leapfrog: wound to a key and left
-- there, then either built or evaluated for the intersection's members
-- alone, per key, whichever is the cheaper way.
--
-- This checks that the answers are what a sequential scan says - through the
-- count, the plain scan and the bitmap scan, with tree probing on and off
-- (pg_lion.enable_tree_probe) - over trees that are probed, built, and both
-- at different keys; ANDs of ORs and ORs of ANDs in a tsquery and across
-- columns, deeper ones, terms with no entry, a term twice; on a dirty heap
-- and after VACUUM; through a nested loop's rescanned inner scan.  And that
-- the count's counters show each choice.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET default_statistics_target = 1000;
SET max_parallel_workers_per_gather = 0;

-- 150,000 rows.  e has 50 values and selects some 3,000 rows scattered over
-- the heap; a, b and c are dense, four values each; z is 7 on the first
-- fifth of the heap and one row in fifty elsewhere, dense at some container
-- keys and sparse at the others.  doc holds five dense terms, each on
-- three rows in ten or so, and a sparse one.  The pad keeps the rows under
-- 128 a page, so a, b, c and the dense terms are NARROWs (DESIGN.md §38),
-- some 1,100 or 1,300 members a key in 1 kB, where they were ARRAYs of 2 kB
-- and more.  The mixing is arithmetic, so the table is the same on every
-- run.
CREATE TABLE tpt (id int NOT NULL, e int NOT NULL, a int NOT NULL,
				  b int NOT NULL, c int NOT NULL, z int NOT NULL,
				  doc tsvector NOT NULL, pad text);
INSERT INTO tpt
SELECT i,
	   (i * 48271 % 1000003) % 50,
	   (i * 39373 % 1000003) % 4,
	   (i * 30271 % 1000003) % 4,
	   (i * 33141 % 1000003) % 4,
	   CASE WHEN i <= 30000 OR (i * 71389 % 1000003) % 50 = 0 THEN 7
			ELSE (i * 71389 % 1000003) % 49 END,
	   array_to_tsvector(array_remove(ARRAY[
		   CASE WHEN (i * 16807 % 1000003) % 10 < 3 THEN 't1' END,
		   CASE WHEN (i * 40014 % 1000003) % 10 < 3 THEN 't2' END,
		   CASE WHEN (i * 25657 % 1000003) % 10 < 3 THEN 't3' END,
		   CASE WHEN (i * 57383 % 1000003) % 10 < 3 THEN 't4' END,
		   CASE WHEN (i * 62089 % 1000003) % 10 < 3 THEN 't5' END,
		   CASE WHEN (i * 69621 % 1000003) % 100 = 0 THEN 's1' END,
		   'w' || (i % 31)]::text[], NULL)),
	   repeat('x', 40)
  FROM generate_series(1::bigint, 150000) i;
CREATE INDEX tpt_l ON tpt USING lion (e, a, b, c, z, doc);
VACUUM (FREEZE, ANALYZE) tpt;

-- The filters.  1 to 5 meet their tree with e's few rows a key and probe
-- it; 6 meets an AND of three NARROWs, which costs less built than probed
-- wherever e has 80 rows or more, and does both; 7 and 8 meet it with a
-- dense intersection and build it; 9 does both, by where z is dense; 10 to
-- 14 are the edges.
CREATE TABLE tpt_q (n int, q text);
INSERT INTO tpt_q VALUES
	(1, $$e = 7 AND doc @@ '(t1 | t2 | t3) & (t4 | t5)'$$),
	(2, $$e = 7 AND doc @@ '(t1 & t2) | (t3 & t4) | t5 & s1'$$),
	(3, $$e = 7 AND ((a = 1 AND b = 2) OR (a = 3 AND c = 1))$$),
	(4, $$e = 7 AND doc @@ '((t1 | t2) & t3 | t4) & (t5 | w3)'$$),
	(5, $$e = 7 AND doc @@ '(t1 | t2 | t3) & (t4 | t5)' AND ((a = 1 AND b = 2) OR (a = 3 AND c = 1))$$),
	(6, $$e = 7 AND doc @@ 't1 & t2 & t4'$$),
	(7, $$a = 1 AND doc @@ '(t1 | t2 | t3) & (t4 | t5)'$$),
	(8, $$b IN (0, 1, 2) AND ((a = 1 AND c = 2) OR (a = 3 AND c = 1))$$),
	(9, $$z = 7 AND doc @@ '(t1 | t2 | t3) & (t4 | t5)'$$),
	-- the tree alone drives, built at every key
	(10, $$doc @@ '(t1 | t2 | s1) & (t4 | s1)'$$),
	-- a term with no entry, in an OR and in an AND
	(11, $$e = 7 AND doc @@ '(t1 | nosuch) & (t4 | t5)'$$),
	(12, $$e = 7 AND doc @@ '(t1 | t2) & nosuch'$$),
	-- the tree met after another clause has thinned the intersection
	(13, $$e = 7 AND b = 2 AND doc @@ '(t1 | t2) & (t4 | s1)'$$),
	-- one term twice in the tree
	(14, $$e = 7 AND doc @@ '(t1 | t2) & (t1 | t4)'$$);

/*
 * How many rows the query of q over tpt differs by, as a multiset, from the
 * same query read by a sequential scan with the pushdown off, when `how`
 * answers it: 'count' (the count pushdown), 'plain' (the plain index scan),
 * 'bitmap' (the bitmap scan); with tree probing on or off.
 */
CREATE FUNCTION tpt_diff(sel text, q text, how text, probe boolean) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	n	bigint;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	EXECUTE format('CREATE TEMP TABLE tpt_off AS SELECT s::text AS r FROM (SELECT %s FROM tpt WHERE %s) s', sel, q);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_tree_probe', CASE WHEN probe THEN 'on' ELSE 'off' END, true);
	PERFORM set_config('pg_lion.enable_count_pushdown', CASE WHEN how = 'count' THEN 'on' ELSE 'off' END, true);
	PERFORM set_config('enable_seqscan', 'off', true);
	IF how <> 'bitmap' THEN
		PERFORM set_config('enable_bitmapscan', 'off', true);
	END IF;
	IF how <> 'plain' THEN
		PERFORM set_config('enable_indexscan', 'off', true);
	END IF;
	EXECUTE format('CREATE TEMP TABLE tpt_on AS SELECT s::text AS r FROM (SELECT %s FROM tpt WHERE %s) s', sel, q);
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('pg_lion.enable_tree_probe', 'on', true);
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM tpt_on EXCEPT ALL SELECT * FROM tpt_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM tpt_off EXCEPT ALL SELECT * FROM tpt_on) b)'
		INTO n;
	DROP TABLE tpt_on, tpt_off;
	RETURN n;
END $$;

/* Does the plan of `SELECT sel FROM tpt WHERE q`, forced as tpt_diff() forces it, show pat? */
CREATE FUNCTION tpt_plan_has(sel text, q text, how text, pat text) RETURNS boolean
LANGUAGE plpgsql AS $$
DECLARE
	ln	text;
	res	boolean := false;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', CASE WHEN how = 'count' THEN 'on' ELSE 'off' END, true);
	PERFORM set_config('enable_seqscan', 'off', true);
	IF how <> 'bitmap' THEN
		PERFORM set_config('enable_bitmapscan', 'off', true);
	END IF;
	IF how <> 'plain' THEN
		PERFORM set_config('enable_indexscan', 'off', true);
	END IF;
	FOR ln IN EXECUTE format('EXPLAIN (COSTS OFF) SELECT %s FROM tpt WHERE %s', sel, q) LOOP
		IF ln ~ pat THEN
			res := true;
		END IF;
	END LOOP;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	RETURN res;
END $$;

/* The reference: how many rows the sequential scan returns. */
CREATE FUNCTION tpt_rows(q text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	n	bigint;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	EXECUTE 'SELECT count(*) FROM tpt WHERE ' || q INTO n;
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	RETURN n;
END $$;

/*
 * The count pushdown's counters for q: whether any tree was built past the
 * merge's driver, and whether any was probed instead; and whether any union
 * was built there.
 */
CREATE FUNCTION tpt_trees(q text, OUT built boolean, OUT probed boolean,
						  OUT unions boolean)
LANGUAGE plpgsql AS $$
DECLARE
	j	json;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) SELECT count(*) FROM tpt WHERE ' || q INTO j;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	IF j::text !~ 'LionCount' THEN
		RAISE EXCEPTION 'not a count pushdown: %', j;
	END IF;
	built := coalesce((j -> 0 -> 'Plan' ->> 'Trees Built')::bigint, 0) > 0;
	probed := coalesce((j -> 0 -> 'Plan' ->> 'Trees Probed')::bigint, 0) > 0;
	unions := coalesce((j -> 0 -> 'Plan' ->> 'Unions Built')::bigint, 0) > 0;
END $$;

-- ---------- 1. every answer, each way ----------
-- The rows each filter holds, the path each is forced through, and what the
-- count's merge did with its trees: probed where e's few rows a key meet
-- them (1 to 5, 11, 13, 14), built where the intersection is dense (7, 8),
-- both by where z is dense (9) and by how many rows e has at a key (6, built
-- at all but one).  A tree that drives is built, and counted in
-- neither (10) - but its own AND met the other OR there, an OR of an OR and
-- a term, as a tsquery's ORs of three nest, over the driver's dense union,
-- and built it.  One that names no entry has nothing to meet (12).
SELECT n, tpt_rows(q) AS rows,
	   tpt_plan_has('count(*)', q, 'count', 'LionCount') AS count_plan,
	   tpt_plan_has('id', q, 'plain', 'Index Scan using tpt_l') AS plain_plan,
	   tpt_plan_has('id', q, 'bitmap', 'Bitmap Index Scan on tpt_l') AS bitmap_plan,
	   (tpt_trees(q)).*
  FROM tpt_q ORDER BY n;
SELECT n, tpt_diff('count(*)', q, 'count', true) AS count_diff,
	   tpt_diff('id, e', q, 'plain', true) AS plain_diff,
	   tpt_diff('id, e', q, 'bitmap', true) AS bitmap_diff,
	   tpt_diff('count(*)', q, 'count', false) AS count_unprobed_diff,
	   tpt_diff('id, e', q, 'plain', false) AS plain_unprobed_diff,
	   tpt_diff('id, e', q, 'bitmap', false) AS bitmap_unprobed_diff
  FROM tpt_q ORDER BY n;

-- Probing off, every tree is built where it is sought, as before: no
-- counter says a tree was met, and the unions inside them are built - but
-- 3's, whose children are ANDs of NARROWs, a bitmap each, which e's rows
-- are looked up in for less than their union costs: it is probed
-- (pg_lion.enable_union_probe, DESIGN.md §29.11, "Unions probed").
SET pg_lion.enable_tree_probe = off;
SELECT n, (tpt_trees(q)).* FROM tpt_q WHERE n IN (1, 3, 9) ORDER BY n;
RESET pg_lion.enable_tree_probe;

-- The counters as numbers, for filter 1 and filter 3, probing on and off:
-- probed, a tree's unions are not built past the driver at all, and the
-- keys the tree is met at are the same either way - 3's union, unprobed,
-- probed by itself at every one of them.
CREATE FUNCTION tpt_counters(q text, probe boolean) RETURNS json
LANGUAGE plpgsql AS $$
DECLARE
	j	json;
BEGIN
	PERFORM set_config('pg_lion.enable_tree_probe', CASE WHEN probe THEN 'on' ELSE 'off' END, true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) SELECT count(*) FROM tpt WHERE ' || q INTO j;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_tree_probe', 'on', true);
	RETURN j -> 0 -> 'Plan';
END $$;
SELECT n, probe, (p->>'Trees Built')::bigint AS trees_built,
	   (p->>'Trees Probed')::bigint AS trees_probed,
	   (p->>'Unions Built')::bigint AS unions_built,
	   (p->>'Unions Probed')::bigint AS unions_probed
  FROM tpt_q, (VALUES (true), (false)) v(probe),
	   LATERAL tpt_counters(q, probe) p
 WHERE n IN (1, 3)
 ORDER BY n, probe DESC;

-- ---------- 2. rescans ----------
-- A nested loop's inner scan, rescanned once per outer row with a new e,
-- its AND probing the trees; and the same through the bitmap scan.
SET enable_seqscan = off;
SET pg_lion.enable_count_pushdown = off;
CREATE TEMP TABLE tpt_nl AS
SELECT v.x, c.n FROM (VALUES (3), (7), (11), (19), (23)) v(x),
  LATERAL (SELECT count(*) AS n FROM tpt WHERE tpt.e = v.x
			  AND tpt.doc @@ '(t1 | t2 | t3) & (t4 | t5)'
			  AND ((tpt.a = 1 AND tpt.b = 2) OR (tpt.a = 3 AND tpt.c = 1))) c;
SET enable_indexscan = off;
CREATE TEMP TABLE tpt_nlb AS
SELECT v.x, c.n FROM (VALUES (3), (7), (11), (19), (23)) v(x),
  LATERAL (SELECT count(*) AS n FROM tpt WHERE tpt.e = v.x
			  AND tpt.doc @@ '(t1 | t2 | t3) & (t4 | t5)'
			  AND ((tpt.a = 1 AND tpt.b = 2) OR (tpt.a = 3 AND tpt.c = 1))) c;
RESET enable_indexscan;
RESET enable_seqscan;
RESET pg_lion.enable_count_pushdown;
SELECT x, n = tpt_rows(format($$e = %s AND doc @@ '(t1 | t2 | t3) & (t4 | t5)' AND ((a = 1 AND b = 2) OR (a = 3 AND c = 1))$$, x)) AS plain_same,
	   (SELECT n FROM tpt_nlb b WHERE b.x = tpt_nl.x) = n AS bitmap_same
  FROM tpt_nl ORDER BY x;
DROP TABLE tpt_nl, tpt_nlb;

-- ---------- 3. a dirty heap, then VACUUM ----------
-- Rows moved into the filters and out of them, and rows deleted: the scans
-- and the count read dead TIDs until VACUUM, and the heap decides.
UPDATE tpt SET doc = 't1 t4'::tsvector WHERE id % 97 = 0 AND e = 7;
UPDATE tpt SET e = 6 WHERE id % 89 = 0;
UPDATE tpt SET a = 3, c = 1 WHERE id % 71 = 0;
DELETE FROM tpt WHERE id % 83 = 0;
SELECT n, tpt_diff('count(*)', q, 'count', true) AS count_diff,
	   tpt_diff('id, e', q, 'plain', true) AS plain_diff,
	   tpt_diff('id, e', q, 'bitmap', true) AS bitmap_diff,
	   tpt_diff('count(*)', q, 'count', false) AS count_unprobed_diff
  FROM tpt_q ORDER BY n;
VACUUM (FREEZE, ANALYZE) tpt;
SELECT n, tpt_rows(q) AS rows,
	   tpt_diff('count(*)', q, 'count', true) AS count_diff,
	   tpt_diff('id, e', q, 'plain', true) AS plain_diff,
	   tpt_diff('id, e', q, 'bitmap', true) AS bitmap_diff,
	   tpt_diff('count(*)', q, 'count', false) AS count_unprobed_diff
  FROM tpt_q ORDER BY n;

DROP TABLE tpt, tpt_q;
DROP FUNCTION tpt_diff(text, text, text, boolean);
DROP FUNCTION tpt_plan_has(text, text, text, text);
DROP FUNCTION tpt_rows(text);
DROP FUNCTION tpt_trees(text);
DROP FUNCTION tpt_counters(text, boolean);
