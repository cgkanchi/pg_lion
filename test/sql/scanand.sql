-- The scan's AND of sets is the count's leapfrog (DESIGN.md §29.11, "The AND,
-- as the count makes it").
--
-- A lion index scan - plain or bitmap - ANDs the posting sets of its quals in
-- an AND node of the expression evaluator, and the count pushdown ANDs the
-- same sets in its merge.  The two used to do it differently: the AND node
-- stepped its FIRST child, in index column order, sought every other child to
-- it at every key and ANDed all of their containers, while the merge drives
-- from the set of fewest rows, seeks the others in ascending rows and gives a
-- key up at the first set that rules it out.  Over a wide filter of dense
-- "default" columns and correlated lists the scan made the same AND at twice
-- the count's time and more.  They are now one function (lion_leapfrog()).
--
-- This checks what that must not change - every answer, through every shape
-- of the plain scan and through the bitmap scan, against a sequential scan:
-- the SETS stream, a range beside the sets (WALK and WINDOW), a long list
-- beside them (LIST), an AND inside a multi-key query, a cursor paused
-- between rows, a nested loop's rescans, an exclusion constraint's pinned
-- scan, a dirty heap and the same after VACUUM - and the plan a count over
-- such a filter takes: the count pushdown, priced below the scans that make
-- the same AND and fetch its rows as well.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
-- a sample of every row: the statistics, and the plans, do not depend on it
SET default_statistics_target = 1000;
SET max_parallel_workers_per_gather = 0;

-- 120,000 rows in 60 hidden groups h, which take turns over the heap.  d1
-- (0 nine times in ten) and fl (false 19 times in 20) are dense "default"
-- columns and come first in the index; e1, m1, m2, m3 and arr's first
-- element follow the row's group 8 times in 10, else another value; tags
-- follow the group; r and k follow nothing.  The mixing is arithmetic, so
-- the table is the same on every run.
CREATE TABLE sca (id int NOT NULL, h int NOT NULL, d1 int NOT NULL,
				  fl boolean NOT NULL, e1 int NOT NULL, m1 int NOT NULL,
				  m2 int NOT NULL, m3 int NOT NULL, arr int[] NOT NULL,
				  tags text[] NOT NULL, r int NOT NULL, k int NOT NULL,
				  pad text);
INSERT INTO sca
SELECT i, h,
	   CASE WHEN (i * 16807 % 1000003) % 10 < 9 THEN 0 ELSE 1 + i % 3 END,
	   (i * 40014 % 1000003) % 20 = 0,
	   CASE WHEN (i * 48271 % 1000003) % 10 < 8 THEN h % 8 ELSE (i * 69621 % 1000003) % 8 END,
	   CASE WHEN (i * 39373 % 1000003) % 10 < 8 THEN (h * 3) % 20 ELSE (i * 45742 % 1000003) % 20 END,
	   CASE WHEN (i * 30271 % 1000003) % 10 < 8 THEN (h * 7 + 1) % 15 ELSE (i * 52391 % 1000003) % 15 END,
	   CASE WHEN (i * 33141 % 1000003) % 10 < 8 THEN h % 12 ELSE (i * 71389 % 1000003) % 12 END,
	   ARRAY[CASE WHEN (i * 25657 % 1000003) % 10 < 8 THEN h % 17 ELSE (i * 57383 % 1000003) % 17 END,
			 17 + (i * 62089 % 1000003) % 23],
	   ARRAY['t' || h % 5, 'u' || h % 7],
	   (i * 40692 % 1000003) % 5000,
	   (i * 69621 % 1000003) % 3000,
	   repeat('x', 150)
  FROM (SELECT i::bigint AS i, ((i::bigint * 7919 + i / 3) % 60)::int AS h
		  FROM generate_series(1, 120000) i) s;
CREATE INDEX sca_l ON sca USING lion (d1, fl, e1, m1, m2, m3, arr, tags, r, k);
VACUUM (FREEZE, ANALYZE) sca;

-- The filter of group 7: the two defaults, an equality, three lists and an
-- overlap - fourteen posting sets, the densest first in the index's order.
-- And the same with more beside it.
CREATE TABLE sca_q (n int, q text);
INSERT INTO sca_q VALUES
	(1, $$d1 = 0 AND NOT fl AND e1 = 7 AND m1 IN (1, 5, 9) AND m2 IN (5, 3) AND m3 IN (7, 2, 0) AND arr && '{7,30,33}'$$),
	-- an AND inside a multi-key query: tags @> is the AND of its keys
	(2, $$d1 = 0 AND NOT fl AND m1 IN (1, 5, 9) AND tags @> '{t2,u0}' AND arr && '{7}'$$),
	-- a range beside the sets: a WINDOW, several at the least floor, and a
	-- short WALK
	(3, $$d1 = 0 AND e1 = 7 AND m3 IN (7, 2) AND r BETWEEN 100 AND 4000$$),
	(4, $$d1 = 0 AND e1 = 7 AND m1 IN (1, 5) AND r BETWEEN 100 AND 105$$),
	-- a list longer than a batch at 64 kB of work_mem: a LIST
	(5, $$NOT fl AND e1 = 7 AND m2 IN (5, 3) AND k = ANY (ARRAY(SELECT generate_series(0, 2999, 7)))$$),
	-- the dense set beside one selective set, and beside two of a few rows
	(6, $$d1 = 0 AND e1 = 7 AND m1 = 1$$),
	(7, $$d1 = 0 AND NOT fl AND e1 = 7 AND m1 = 2 AND m3 = 5$$),
	-- contradictions: inside a multi-key query, and between two lists
	(8, $$d1 = 0 AND e1 = 7 AND tags @> '{t2,t3}'$$),
	(9, $$d1 = 0 AND m1 IN (1, 5) AND m1 IN (9, 13) AND arr && '{7}'$$);

/*
 * How many rows the query of q over sca differs by, as a multiset, from the
 * same query read by a sequential scan with the pushdown off, when the lion
 * scan of `how` answers it: 'plain' (the plain index scan), 'bitmap' (the
 * bitmap scan) or 'default' (nothing disabled).
 */
CREATE FUNCTION sca_diff(sel text, q text, how text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	n	bigint;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	EXECUTE format('CREATE TEMP TABLE sca_off AS SELECT s::text AS r FROM (SELECT %s FROM sca WHERE %s) s', sel, q);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	IF how <> 'default' THEN
		PERFORM set_config('enable_seqscan', 'off', true);
		PERFORM set_config(CASE how WHEN 'plain' THEN 'enable_bitmapscan'
									ELSE 'enable_indexscan' END, 'off', true);
	END IF;
	EXECUTE format('CREATE TEMP TABLE sca_on AS SELECT s::text AS r FROM (SELECT %s FROM sca WHERE %s) s', sel, q);
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM sca_on EXCEPT ALL SELECT * FROM sca_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM sca_off EXCEPT ALL SELECT * FROM sca_on) b)'
		INTO n;
	DROP TABLE sca_on, sca_off;
	RETURN n;
END $$;

/* Does the plan of `SELECT sel FROM sca WHERE q`, forced as sca_diff() forces it, show pat? */
CREATE FUNCTION sca_plan_has(sel text, q text, how text, pat text) RETURNS boolean
LANGUAGE plpgsql AS $$
DECLARE
	ln	text;
	res	boolean := false;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	IF how <> 'default' THEN
		PERFORM set_config('enable_seqscan', 'off', true);
		PERFORM set_config(CASE how WHEN 'plain' THEN 'enable_bitmapscan'
									ELSE 'enable_indexscan' END, 'off', true);
	END IF;
	FOR ln IN EXECUTE format('EXPLAIN (COSTS OFF) SELECT %s FROM sca WHERE %s', sel, q) LOOP
		IF ln ~ pat THEN
			res := true;
		END IF;
	END LOOP;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	RETURN res;
END $$;

/* The reference: how many rows the sequential scan returns. */
CREATE FUNCTION sca_rows(q text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	n	bigint;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	EXECUTE 'SELECT count(*) FROM sca WHERE ' || q INTO n;
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	RETURN n;
END $$;

-- ---------- 1. every answer, through both scans ----------
-- The rows the filters hold, and that each is answered by the scan it is
-- forced through.  A window of 64 kB (8 containers) puts query 3 in several
-- windows, and a batch of 32 values puts query 5's list of 429 in several
-- batches.
SET pg_lion.scan_window_floor = 64;
SET work_mem = '64kB';
SELECT n, sca_rows(q) AS rows,
	   sca_plan_has('id', q, 'plain', 'Index Scan using sca_l') AS plain,
	   sca_plan_has('id', q, 'bitmap', 'Bitmap Index Scan on sca_l') AS bitmap
  FROM sca_q ORDER BY n;
SELECT n, sca_diff('id, h', q, 'plain') AS plain_diff,
	   sca_diff('id, h', q, 'bitmap') AS bitmap_diff,
	   sca_diff('id, h', q, 'default') AS default_diff
  FROM sca_q ORDER BY n;
RESET work_mem;
RESET pg_lion.scan_window_floor;

-- ---------- 2. a cursor paused between rows, and rescans ----------
-- The plain scan hands its rows out one amgettuple call at a time and holds
-- its leapfrog's cursors between two of them: a cursor fetches the filter's
-- rows one at a time, with another scan of the index run every fifty.
SET enable_bitmapscan = off;
SET enable_seqscan = off;
SET pg_lion.enable_count_pushdown = off;
CREATE TEMP TABLE sca_fetched (id int);
DO $$
DECLARE
	c	refcursor;
	v	int;
	n	int := 0;
BEGIN
	OPEN c FOR SELECT id FROM sca
		WHERE d1 = 0 AND NOT fl AND e1 = 7 AND m1 IN (1, 5, 9) AND m2 IN (5, 3)
		  AND m3 IN (7, 2, 0) AND arr && '{7,30,33}';
	LOOP
		FETCH c INTO v;
		EXIT WHEN NOT FOUND;
		INSERT INTO sca_fetched VALUES (v);
		n := n + 1;
		IF n % 50 = 0 THEN
			PERFORM count(*) FROM sca WHERE d1 = 0 AND e1 = 3 AND m1 = 9;
		END IF;
	END LOOP;
	CLOSE c;
END $$;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET pg_lion.enable_count_pushdown;
SELECT (SELECT count(*) FROM sca_fetched) AS fetched,
	   (SELECT count(*) FROM (SELECT id FROM sca_fetched
							  EXCEPT ALL
							  SELECT id FROM sca WHERE d1 = 0 AND NOT fl AND e1 = 7
								 AND m1 IN (1, 5, 9) AND m2 IN (5, 3)
								 AND m3 IN (7, 2, 0) AND arr && '{7,30,33}') d) AS extra;
DROP TABLE sca_fetched;

-- A nested loop's inner scan, rescanned once per outer row with a new m1.
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_bitmapscan = off;
SET enable_seqscan = off;
SET pg_lion.enable_count_pushdown = off;
DO $$
DECLARE
	ln	text;
	nl	boolean := false;
	inner_param boolean := false;
BEGIN
	FOR ln IN EXPLAIN (COSTS OFF)
		SELECT v.x, count(*) FROM (VALUES (1), (5), (9), (13)) v(x)
		  JOIN sca ON sca.m1 = v.x AND sca.d1 = 0 AND NOT sca.fl AND sca.e1 = 7
				  AND sca.arr && '{7,30,33}'
		 GROUP BY v.x
	LOOP
		nl := nl OR ln ~ 'Nested Loop';
		inner_param := inner_param OR ln ~ 'Index Cond: .*\(m1 = "\*VALUES\*"\.column1\)';
	END LOOP;
	RAISE NOTICE 'nested loop: %, lion inner scan with a parameter: %', nl, inner_param;
END $$;
CREATE TEMP TABLE sca_nl AS
SELECT v.x, count(*) AS n FROM (VALUES (1), (5), (9), (13)) v(x)
  JOIN sca ON sca.m1 = v.x AND sca.d1 = 0 AND NOT sca.fl AND sca.e1 = 7
		  AND sca.arr && '{7,30,33}'
 GROUP BY v.x;
RESET enable_seqscan;
RESET enable_bitmapscan;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT v.x, count(*) = (SELECT n FROM sca_nl WHERE sca_nl.x = v.x) AS same
  FROM (VALUES (1), (5), (9), (13)) v(x)
  JOIN sca ON sca.m1 = v.x AND sca.d1 = 0 AND NOT sca.fl AND sca.e1 = 7
		  AND sca.arr && '{7,30,33}'
 GROUP BY v.x ORDER BY v.x;
RESET enable_indexscan;
RESET enable_bitmapscan;
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET pg_lion.enable_count_pushdown;
DROP TABLE sca_nl;

-- ---------- 3. an exclusion constraint: the pinned scan ----------
-- A constraint's check scans under a dirty snapshot and keeps the pin on the
-- page of each batch (DESIGN.md §29.5): the AND of three equalities, one of
-- them dense, sought with pins.
CREATE TABLE sce (a int, b int, c int, EXCLUDE USING lion (a WITH =, b WITH =, c WITH =));
INSERT INTO sce SELECT i % 2, i % 100, i FROM generate_series(1, 20000) i;
INSERT INTO sce VALUES (1, 7, 7);		-- conflicts with (1, 7, 7)
INSERT INTO sce VALUES (0, 7, 7);		-- does not
INSERT INTO sce VALUES (1, 8, 20001);	-- nor does this
SELECT count(*) FROM sce;
DROP TABLE sce;

-- ---------- 4. a dirty heap, then VACUUM ----------
-- Rows moved into the filter and out of it, and rows deleted: the scans read
-- dead TIDs until VACUUM, and the heap decides.
UPDATE sca SET m1 = 1 WHERE id % 97 = 0 AND e1 = 7;
UPDATE sca SET e1 = 6 WHERE id % 89 = 0 AND h = 7;
DELETE FROM sca WHERE id % 83 = 0;
SELECT n, sca_diff('id, h', q, 'plain') AS plain_diff,
	   sca_diff('id, h', q, 'bitmap') AS bitmap_diff
  FROM sca_q WHERE n IN (1, 2, 3, 4, 6) ORDER BY n;
VACUUM (FREEZE, ANALYZE) sca;
SELECT n, sca_diff('id, h', q, 'plain') AS plain_diff,
	   sca_diff('id, h', q, 'bitmap') AS bitmap_diff,
	   sca_diff('count(*)', q, 'default') AS count_diff
  FROM sca_q ORDER BY n;

-- ---------- 5. the plan of a count over the filter ----------
-- The count pushdown makes the AND the scans make and reads no heap, where
-- the plain and the bitmap scan fetch every row it leaves: over a cached
-- heap, at a random_page_cost of 1.1, it is priced below both, by the heap
-- pages of the rows the intersection probe measured - core's product of the
-- clauses says a few rows - and it is the plan.
SET random_page_cost = 1.1;
SET effective_cache_size = '4GB';
EXPLAIN (COSTS OFF) SELECT count(*) FROM sca
 WHERE d1 = 0 AND NOT fl AND e1 = 7 AND m1 IN (1, 5, 9) AND m2 IN (5, 3)
   AND m3 IN (7, 2, 0) AND arr && '{7,30,33}';

/*
 * What the scan of `how` - 'plain' or 'bitmap' - and an Aggregate cost over
 * the count pushdown that is the plan, with the intersection probe on or off.
 */
CREATE FUNCTION sca_margin(q text, how text, probe boolean) RETURNS numeric
LANGUAGE plpgsql AS $$
DECLARE
	j	json;
	c0	numeric;
	c1	numeric;
BEGIN
	PERFORM set_config('pg_lion.enable_intersection_probe',
					   CASE WHEN probe THEN 'on' ELSE 'off' END, true);
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	IF j::text !~ 'LionCount' THEN
		RAISE EXCEPTION 'not a count pushdown: %', j;
	END IF;
	c0 := (j -> 0 -> 'Plan' ->> 'Total Cost')::numeric;
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config(CASE how WHEN 'plain' THEN 'enable_bitmapscan'
								ELSE 'enable_indexscan' END, 'off', true);
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	c1 := (j -> 0 -> 'Plan' ->> 'Total Cost')::numeric;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('pg_lion.enable_intersection_probe', 'on', true);
	RETURN c1 - c0;
END $$;

-- With the probe, each scan is priced above the count by more than half its
-- rows - the heap pages they lie on, at seq_page_cost or more each - and
-- without it, priced for core's few rows, by less.
SELECT n, sca_rows(q) AS rows,
	   sca_margin('SELECT count(*) FROM sca WHERE ' || q, 'plain', true) > sca_rows(q) / 2 AS plain_margin,
	   sca_margin('SELECT count(*) FROM sca WHERE ' || q, 'bitmap', true) > sca_rows(q) / 2 AS bitmap_margin,
	   sca_margin('SELECT count(*) FROM sca WHERE ' || q, 'plain', false) > sca_rows(q) / 2 AS plain_margin_unprobed,
	   sca_margin('SELECT count(*) FROM sca WHERE ' || q, 'bitmap', false) > sca_rows(q) / 2 AS bitmap_margin_unprobed
  FROM sca_q WHERE n IN (1, 2) ORDER BY n;
RESET effective_cache_size;
RESET random_page_cost;

DROP TABLE sca, sca_q;
DROP FUNCTION sca_diff(text, text, text);
DROP FUNCTION sca_plan_has(text, text, text, text);
DROP FUNCTION sca_rows(text);
DROP FUNCTION sca_margin(text, text, boolean);

-- ---------- 6. what the scans read: the count's pages ----------
-- d0 and d1 are half the rows each, at random: a bitset at every container
-- key, a posting page each.  b and c are 2% of the rows each, at every key,
-- and together only in the first two keys' rows and the last two's.  The
-- index lists the dense columns first, which is the order the scan's AND
-- node used to step and seek its children in: every page of d0 and d1 was
-- read, where the count, driven by c and giving up every key b rules out,
-- reads theirs at four keys.  Now the scans read what the count reads - its
-- index pages, the count's one visibility map page aside.  The rows are
-- narrow, 157 to a page and 10048 to a container key, so that the blocks
-- hold offsets past 127: d0's and d1's containers are bitsets, not NARROWs
-- (DESIGN.md §38), which would share their posting pages seven at a time.
CREATE TABLE scb (id int NOT NULL, d0 int NOT NULL, d1 int NOT NULL,
				  b int NOT NULL, c int NOT NULL);
INSERT INTO scb
SELECT i, (i * 2654435761 % 4294967296 / 65536) % 2,
	   i * 2654435761 % 4294967296 / 2147483648,
	   CASE WHEN i % 50 = 0 OR (i % 50 = 25 AND (i / 10048 < 2 OR i / 10048 >= 18))
			THEN 1 ELSE 0 END,
	   CASE WHEN i % 50 = 25 THEN 1 ELSE 0 END
  FROM generate_series(1::bigint, 10048 * 20) i;
CREATE INDEX scb_l ON scb USING lion (d0, d1, b, c);
VACUUM (FREEZE, ANALYZE) scb;
SELECT attno, containers, bitset_containers, container_pages
  FROM lion_index_stats('scb_l') ORDER BY attno;

/*
 * The shared buffers the node of q that `how` names accesses: 'count' the
 * LionCount, 'bitmap' the Bitmap Index Scan, 'plain' the Index Scan less the
 * heap pages it fetches, one each - the pages the rows lie on.
 */
CREATE FUNCTION scb_buffers(q text, how text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	j	json;
	n	bigint;
	pages bigint;
BEGIN
	IF how <> 'count' THEN
		PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
		PERFORM set_config('enable_seqscan', 'off', true);
		PERFORM set_config(CASE how WHEN 'plain' THEN 'enable_bitmapscan'
									ELSE 'enable_indexscan' END, 'off', true);
	END IF;
	EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, FORMAT JSON) SELECT count(*) FROM scb WHERE ' || q INTO j;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	n := (regexp_match(j::text,
		  CASE how WHEN 'count' THEN '"Custom Plan Provider": "LionCount",.*?"Shared Hit Blocks": (\d+),'
				   WHEN 'bitmap' THEN '"Node Type": "Bitmap Index Scan",.*?"Shared Hit Blocks": (\d+),'
				   ELSE '"Node Type": "Index Scan",.*?"Shared Hit Blocks": (\d+),' END))[1]::bigint;
	IF how = 'plain' THEN
		EXECUTE 'SELECT count(DISTINCT (ctid::text::point)[0]) FROM scb WHERE ' || q INTO pages;
		n := n - pages;
	END IF;
	RETURN n;
END $$;

SELECT count(*) FROM scb WHERE d0 = 1 AND d1 = 1 AND b = 1 AND c = 1;
SELECT scb_buffers(q, 'count') AS count_buffers,
	   scb_buffers(q, 'bitmap') < scb_buffers(q, 'count') AS bitmap_reads_less,
	   scb_buffers(q, 'plain') < scb_buffers(q, 'count') AS plain_reads_less,
	   scb_buffers(q, 'bitmap') = scb_buffers(q, 'plain') AS both_read_alike
  FROM (VALUES ('d0 = 1 AND d1 = 1 AND b = 1 AND c = 1')) v(q);
DROP TABLE scb;
DROP FUNCTION scb_buffers(text, text);
