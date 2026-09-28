-- A union ANDed with a running intersection, built or probed (DESIGN.md
-- §29.11, "Unions probed").
--
-- An IN list, a multi-key `&&` and an OR across columns are unions of posting
-- sets, and in an AND each is met at every container key the leapfrog
-- reaches.  The union used to be built there in full - every list member's
-- container ORed together - and only then ANDed with the running
-- intersection.  Now it is built only where that is the cheaper way: where
-- the intersection is small, its members are looked up in the members'
-- containers instead, and the union never exists.  The choice is made per
-- key, in the count's merge and in the AND node of a plain or bitmap scan
-- alike (lion_leapfrog()).
--
-- This checks that the answers are what a sequential scan says - through the
-- count, the plain scan and the bitmap scan, with probing on and off
-- (pg_lion.enable_union_probe) - over filters that probe, that build, and
-- that do both at different keys; with NULLs, lists that are empty or name
-- no entry, a multi-key `&&` and `@>`, an OR across columns; on a dirty heap
-- and after VACUUM; through a cursor paused between rows and a nested loop's
-- rescanned inner scan.  And that the count's counters show each choice.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET default_statistics_target = 1000;
SET max_parallel_workers_per_gather = 0;

-- 150,000 rows in 50 hidden groups h, which take turns over the heap.  d1
-- (0 nine times in ten) and fl (false 19 times in 20) are dense; e, k1, k2
-- and arr's first element follow the group 8 times in 10, else another
-- value; k2 is NULL one time in 20; k3 is dense, four values; z is 7 on the
-- first fifth of the heap and one row in fifty elsewhere, so that a filter on
-- it is dense at some container keys and sparse at the others.  The mixing
-- is arithmetic, so the table is the same on every run.
CREATE TABLE upt (id int NOT NULL, h int NOT NULL, d1 int NOT NULL,
				  fl boolean NOT NULL, e int NOT NULL, k1 int NOT NULL,
				  k2 int, k3 int NOT NULL, z int NOT NULL,
				  arr int[] NOT NULL, pad text);
INSERT INTO upt
SELECT i, h,
	   CASE WHEN (i * 16807 % 1000003) % 10 < 9 THEN 0 ELSE 1 + i % 3 END,
	   (i * 40014 % 1000003) % 20 = 0,
	   CASE WHEN (i * 48271 % 1000003) % 10 < 8 THEN h % 25 ELSE (i * 69621 % 1000003) % 25 END,
	   CASE WHEN (i * 39373 % 1000003) % 10 < 8 THEN h % 10 ELSE (i * 45742 % 1000003) % 10 END,
	   CASE WHEN (i * 30271 % 1000003) % 20 = 0 THEN NULL
			WHEN (i * 30271 % 1000003) % 10 < 8 THEN h % 6 ELSE (i * 52391 % 1000003) % 6 END,
	   (i * 33141 % 1000003) % 4,
	   CASE WHEN i <= 30000 OR (i * 71389 % 1000003) % 50 = 0 THEN 7
			ELSE (i * 71389 % 1000003) % 49 END,
	   ARRAY[CASE WHEN (i * 25657 % 1000003) % 10 < 8 THEN h % 17 ELSE (i * 57383 % 1000003) % 17 END,
			 17 + (i * 62089 % 1000003) % 23],
	   repeat('x', 60)
  FROM (SELECT i::bigint AS i, ((i::bigint * 7919 + i / 3) % 50)::int AS h
		  FROM generate_series(1, 150000) i) s;
CREATE INDEX upt_l ON upt USING lion (d1, fl, e, k1, k2, k3, z, arr);
VACUUM (FREEZE, ANALYZE) upt;

-- The filters.  1 to 5 look a small intersection up in the lists' members;
-- 6 and 7 are dense and build their unions; 8 does both, by where z is
-- dense; 9 to 14 are the edges.
CREATE TABLE upt_q (n int, q text);
INSERT INTO upt_q VALUES
	(1, $$e = 7 AND k1 IN (7, 2, 5) AND k2 IN (1, 3)$$),
	(2, $$d1 = 0 AND NOT fl AND e = 7 AND k1 IN (7, 2) AND k2 IN (1, 4, 5) AND arr && '{7,30,33}'$$),
	(3, $$e = 7 AND arr && '{7,24,33,39}'$$),
	(4, $$e = 3 AND (k1 = 3 OR k2 = 4) AND NOT fl$$),
	(5, $$e = 7 AND k1 IN (7, 2) AND arr @> '{7}' AND k3 IN (0, 2)$$),
	(6, $$d1 = 0 AND NOT fl AND k3 IN (0, 1, 2, 3)$$),
	(7, $$d1 = 0 AND k3 IN (1, 2, 3) AND k1 IN (0, 1, 2, 3, 4, 5, 6, 7, 8)$$),
	(8, $$z = 7 AND k1 IN (7, 2, 5) AND k3 IN (0, 1, 2)$$),
	-- NULLs: a NULL in a list is no value; IS NULL is the NULL entry
	(9, $$e = 7 AND k2 IN (1, 3, NULL) AND k1 IN (7, 2)$$),
	(10, $$e = 7 AND k2 IS NULL AND k1 IN (7, 2, 5)$$),
	-- a list that is empty, and one that names no entry
	(11, $$e = 7 AND k1 = ANY ('{}'::int[])$$),
	(12, $$e = 7 AND k1 IN (100, 200) AND k2 IN (1, 3)$$),
	-- a list naming one value and a list of one entry among values of none
	(13, $$e = 7 AND k1 IN (7) AND k2 IN (1, 300)$$),
	-- two lists on one column
	(14, $$e = 7 AND k1 IN (7, 2, 5) AND k1 IN (2, 5, 9)$$);

/*
 * How many rows the query of q over upt differs by, as a multiset, from the
 * same query read by a sequential scan with the pushdown off, when `how`
 * answers it: 'count' (the count pushdown), 'plain' (the plain index scan),
 * 'bitmap' (the bitmap scan); with probing on or off.
 */
CREATE FUNCTION upt_diff(sel text, q text, how text, probe boolean) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	n	bigint;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	EXECUTE format('CREATE TEMP TABLE upt_off AS SELECT s::text AS r FROM (SELECT %s FROM upt WHERE %s) s', sel, q);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_union_probe', CASE WHEN probe THEN 'on' ELSE 'off' END, true);
	PERFORM set_config('pg_lion.enable_count_pushdown', CASE WHEN how = 'count' THEN 'on' ELSE 'off' END, true);
	PERFORM set_config('enable_seqscan', 'off', true);
	IF how <> 'bitmap' THEN
		PERFORM set_config('enable_bitmapscan', 'off', true);
	END IF;
	IF how <> 'plain' THEN
		PERFORM set_config('enable_indexscan', 'off', true);
	END IF;
	EXECUTE format('CREATE TEMP TABLE upt_on AS SELECT s::text AS r FROM (SELECT %s FROM upt WHERE %s) s', sel, q);
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('pg_lion.enable_union_probe', 'on', true);
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM upt_on EXCEPT ALL SELECT * FROM upt_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM upt_off EXCEPT ALL SELECT * FROM upt_on) b)'
		INTO n;
	DROP TABLE upt_on, upt_off;
	RETURN n;
END $$;

/* Does the plan of `SELECT sel FROM upt WHERE q`, forced as upt_diff() forces it, show pat? */
CREATE FUNCTION upt_plan_has(sel text, q text, how text, pat text) RETURNS boolean
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
	FOR ln IN EXECUTE format('EXPLAIN (COSTS OFF) SELECT %s FROM upt WHERE %s', sel, q) LOOP
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
CREATE FUNCTION upt_rows(q text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	n	bigint;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	EXECUTE 'SELECT count(*) FROM upt WHERE ' || q INTO n;
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	RETURN n;
END $$;

/*
 * The count pushdown's counters for q: whether any union was built, and
 * whether any was probed instead, at the keys of the merge.
 */
CREATE FUNCTION upt_unions(q text, OUT built boolean, OUT probed boolean)
LANGUAGE plpgsql AS $$
DECLARE
	j	json;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) SELECT count(*) FROM upt WHERE ' || q INTO j;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	IF j::text !~ 'LionCount' THEN
		RAISE EXCEPTION 'not a count pushdown: %', j;
	END IF;
	built := coalesce((j -> 0 -> 'Plan' ->> 'Unions Built')::bigint, 0) > 0;
	probed := coalesce((j -> 0 -> 'Plan' ->> 'Unions Probed')::bigint, 0) > 0;
END $$;

-- ---------- 1. every answer, each way ----------
-- The rows each filter holds, the path each is forced through, and what the
-- count's merge did with its unions.  Where the intersection is small by the
-- time a list is met, the list is probed (5, 10); where it is dense, built (6,
-- 7); the lists of 1 to 4, 9 and 14 meet an intersection of a hundred-odd
-- members at the first list and of a few at the next, and are built there and
-- probed here, as 8's are where z is dense and where it is sparse.  A list
-- that is empty or names no entry has nothing to meet (11, 12), and one of a
-- single entry is no union (13).
SELECT n, upt_rows(q) AS rows,
	   upt_plan_has('count(*)', q, 'count', 'LionCount') AS count_plan,
	   upt_plan_has('id', q, 'plain', 'Index Scan using upt_l') AS plain_plan,
	   upt_plan_has('id', q, 'bitmap', 'Bitmap Index Scan on upt_l') AS bitmap_plan,
	   (upt_unions(q)).*
  FROM upt_q ORDER BY n;
SELECT n, upt_diff('count(*)', q, 'count', true) AS count_diff,
	   upt_diff('id, h', q, 'plain', true) AS plain_diff,
	   upt_diff('id, h', q, 'bitmap', true) AS bitmap_diff,
	   upt_diff('count(*)', q, 'count', false) AS count_unprobed_diff,
	   upt_diff('id, h', q, 'plain', false) AS plain_unprobed_diff,
	   upt_diff('id, h', q, 'bitmap', false) AS bitmap_unprobed_diff
  FROM upt_q ORDER BY n;

-- Probing off, every union is built: the counters say so.
SET pg_lion.enable_union_probe = off;
SELECT n, (upt_unions(q)).* FROM upt_q WHERE n IN (1, 2, 8) ORDER BY n;
RESET pg_lion.enable_union_probe;

-- ---------- 2. a cursor paused between rows, and rescans ----------
-- The plain scan holds its leapfrog's cursors between two amgettuple calls:
-- a cursor fetches filter 2's rows one at a time, with another scan of the
-- index run every twenty.
SET enable_bitmapscan = off;
SET enable_seqscan = off;
SET pg_lion.enable_count_pushdown = off;
CREATE TEMP TABLE upt_fetched (id int);
DO $$
DECLARE
	c	refcursor;
	v	int;
	n	int := 0;
BEGIN
	OPEN c FOR SELECT id FROM upt
		WHERE d1 = 0 AND NOT fl AND e = 7 AND k1 IN (7, 2) AND k2 IN (1, 4, 5)
		  AND arr && '{7,30,33}';
	LOOP
		FETCH c INTO v;
		EXIT WHEN NOT FOUND;
		INSERT INTO upt_fetched VALUES (v);
		n := n + 1;
		IF n % 20 = 0 THEN
			PERFORM count(*) FROM upt WHERE e = 3 AND k1 IN (3, 4) AND k2 IN (0, 1);
		END IF;
	END LOOP;
	CLOSE c;
END $$;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET pg_lion.enable_count_pushdown;
SELECT (SELECT count(*) FROM upt_fetched) AS fetched,
	   (SELECT count(*) FROM (SELECT id FROM upt_fetched
							  EXCEPT ALL
							  SELECT id FROM upt WHERE d1 = 0 AND NOT fl AND e = 7
								 AND k1 IN (7, 2) AND k2 IN (1, 4, 5)
								 AND arr && '{7,30,33}') d) AS extra,
	   (SELECT count(*) FROM (SELECT id FROM upt WHERE d1 = 0 AND NOT fl AND e = 7
								 AND k1 IN (7, 2) AND k2 IN (1, 4, 5)
								 AND arr && '{7,30,33}'
							  EXCEPT ALL
							  SELECT id FROM upt_fetched) d) AS missing;
DROP TABLE upt_fetched;

-- A nested loop's inner scan, rescanned once per outer row with a new e,
-- its AND probing the lists; and the same through the bitmap scan.
SET enable_seqscan = off;
SET pg_lion.enable_count_pushdown = off;
DO $$
DECLARE
	ln	text;
	nl	boolean := false;
	inner_param boolean := false;
BEGIN
	FOR ln IN EXPLAIN (COSTS OFF)
		SELECT v.x, c.n FROM (VALUES (3), (7), (11), (19), (23)) v(x),
		  LATERAL (SELECT count(*) AS n FROM upt WHERE upt.e = v.x
					  AND upt.k1 IN (7, 2, 3) AND upt.k2 IN (1, 3, 4)
					  AND upt.arr && '{7,11,30}') c
	LOOP
		nl := nl OR ln ~ 'Nested Loop';
		inner_param := inner_param OR ln ~ 'Index Cond: \(\(e = "\*VALUES\*"\.column1\)';
	END LOOP;
	RAISE NOTICE 'nested loop: %, lion inner scan with a parameter: %', nl, inner_param;
END $$;
CREATE TEMP TABLE upt_nl AS
SELECT v.x, c.n, b.n AS nb FROM (VALUES (3), (7), (11), (19), (23)) v(x),
  LATERAL (SELECT count(*) AS n FROM upt WHERE upt.e = v.x
			  AND upt.k1 IN (7, 2, 3) AND upt.k2 IN (1, 3, 4)
			  AND upt.arr && '{7,11,30}') c,
  LATERAL (SELECT count(*) AS n FROM upt WHERE upt.e = v.x + 1
			  AND upt.k1 IN (7, 2, 3) AND upt.k2 IN (1, 3, 4)
			  AND upt.arr && '{7,11,30}' AND NOT upt.fl) b;
SET enable_indexscan = off;
CREATE TEMP TABLE upt_nlb AS
SELECT v.x, c.n FROM (VALUES (3), (7), (11), (19), (23)) v(x),
  LATERAL (SELECT count(*) AS n FROM upt WHERE upt.e = v.x
			  AND upt.k1 IN (7, 2, 3) AND upt.k2 IN (1, 3, 4)
			  AND upt.arr && '{7,11,30}') c;
RESET enable_indexscan;
RESET enable_seqscan;
RESET pg_lion.enable_count_pushdown;
SELECT x, n = upt_rows(format($$e = %s AND k1 IN (7, 2, 3) AND k2 IN (1, 3, 4) AND arr && '{7,11,30}'$$, x)) AS plain_same,
	   (SELECT n FROM upt_nlb b WHERE b.x = upt_nl.x) = n AS bitmap_same,
	   nb = upt_rows(format($$e = %s AND k1 IN (7, 2, 3) AND k2 IN (1, 3, 4) AND arr && '{7,11,30}' AND NOT fl$$, x + 1)) AS second_same
  FROM upt_nl ORDER BY x;
DROP TABLE upt_nl, upt_nlb;

-- ---------- 3. a dirty heap, then VACUUM ----------
-- Rows moved into the filters and out of them, and rows deleted: the scans
-- and the count read dead TIDs until VACUUM, and the heap decides.
UPDATE upt SET k1 = 7 WHERE id % 97 = 0 AND e = 7;
UPDATE upt SET e = 6 WHERE id % 89 = 0 AND h = 7;
UPDATE upt SET k2 = NULL WHERE id % 71 = 0;
DELETE FROM upt WHERE id % 83 = 0;
SELECT n, upt_diff('count(*)', q, 'count', true) AS count_diff,
	   upt_diff('id, h', q, 'plain', true) AS plain_diff,
	   upt_diff('id, h', q, 'bitmap', true) AS bitmap_diff,
	   upt_diff('count(*)', q, 'count', false) AS count_unprobed_diff
  FROM upt_q ORDER BY n;
VACUUM (FREEZE, ANALYZE) upt;
SELECT n, upt_rows(q) AS rows,
	   upt_diff('count(*)', q, 'count', true) AS count_diff,
	   upt_diff('id, h', q, 'plain', true) AS plain_diff,
	   upt_diff('id, h', q, 'bitmap', true) AS bitmap_diff,
	   upt_diff('count(*)', q, 'count', false) AS count_unprobed_diff
  FROM upt_q ORDER BY n;

DROP TABLE upt, upt_q;
DROP FUNCTION upt_diff(text, text, text, boolean);
DROP FUNCTION upt_plan_has(text, text, text, text);
DROP FUNCTION upt_rows(text);
DROP FUNCTION upt_unions(text);
