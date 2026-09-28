-- A plain index scan (amgettuple) against a PARALLEL bitmap heap scan, and
-- pg_lion.enable_plain_scan (DESIGN.md §29.11, "One process against several"
-- and "Plain scans switched off").
--
-- A plain lion scan reads its heap pages in one process, a page when it needs
-- it; the bitmap heap scan of the same pages reads ahead, and in parallel in
-- several processes.  Where core would run that bitmap heap scan in parallel,
-- the plain scan's pages are priced as one process's reads, and a large
-- result goes to the bitmap scan; below that size the two are one process
-- each, priced alike, and a selective result stays on the plain scan.
-- pg_lion.enable_plain_scan = off plans lion indexes for bitmap scans only -
-- no plain or index-only scan of them, parameterized or not - and leaves the
-- other access methods' index scans alone, which enable_indexscan = off does
-- not.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET synchronous_commit = on;
-- a sample of every row: the statistics, and the plans, do not depend on it
SET default_statistics_target = 1000;
-- the count pushdown would answer the counts below from the index alone
SET pg_lion.enable_count_pushdown = off;

-- 20000 rows, 18 to a page: 1112 pages.  Every column is a hash of the row
-- number: k has 60 values, k2 1000, and flag is true on a tenth of the rows,
-- all placed at random.
CREATE TABLE lpp (id int PRIMARY KEY, k int, k2 int, flag bool, pad text);
INSERT INTO lpp
SELECT g,
	   ((hashint8extended(g, 1) & 2147483647) % 60)::int,
	   ((hashint8extended(g, 2) & 2147483647) % 1000)::int,
	   (hashint8extended(g, 3) & 2147483647) % 10 = 0,
	   repeat('x', 400)
  FROM generate_series(1, 20000) g;
CREATE INDEX lpp_k ON lpp USING lion (k);
CREATE INDEX lpp_k2 ON lpp USING lion (k2);
CREATE INDEX lpp_part ON lpp USING lion (k) WHERE flag;
VACUUM (FREEZE, ANALYZE) lpp;

-- ---------- 1. a large result, against a parallel bitmap heap scan ----------
-- k = 7 is a sixtieth of the rows, on about a quarter of the pages.  With no
-- parallel plans, and while those pages are fewer than core gives a parallel
-- worker (min_parallel_table_scan_size), the plain scan competes with one
-- process's bitmap scan, is priced alike, and is chosen for building no
-- bitmap.  Once core would run that bitmap scan in parallel, the plain scan's
-- pages are one process's reads, some 30% dearer here, and the bitmap
-- scan is chosen - serially, as it happens: over some 290 pages a parallel
-- one does not pay for starting its workers.
SET max_parallel_workers_per_gather = 0;
EXPLAIN (COSTS OFF) SELECT sum(id) FROM lpp WHERE k = 7;
SET max_parallel_workers_per_gather = 2;
SET min_parallel_table_scan_size = '8MB';
EXPLAIN (COSTS OFF) SELECT sum(id) FROM lpp WHERE k = 7;
SET min_parallel_table_scan_size = '1MB';
EXPLAIN (COSTS OFF) SELECT sum(id) FROM lpp WHERE k = 7;
-- 20 rows on 20 pages are no parallel scan's, and stay on the plain scan
EXPLAIN (COSTS OFF) SELECT sum(id) FROM lpp WHERE k2 = 5;
-- and the answers are the sequential scan's
SELECT (SELECT sum(id) FROM lpp WHERE k = 7) =
	   (SELECT sum(id) FROM lpp WHERE k + 0 = 7) AS same_answer;
RESET min_parallel_table_scan_size;
RESET max_parallel_workers_per_gather;

-- ---------- 2. pg_lion.enable_plain_scan ----------
-- Off, the lion index is planned as a GIN index is, for bitmap scans only;
-- the primary key's btree keeps its index scan.
SET pg_lion.enable_plain_scan = off;
EXPLAIN (COSTS OFF) SELECT sum(id) FROM lpp WHERE k2 = 5;
EXPLAIN (COSTS OFF) SELECT pad FROM lpp WHERE id = 5;
SELECT (SELECT sum(id) FROM lpp WHERE k2 = 5) =
	   (SELECT sum(id) FROM lpp WHERE k2 + 0 = 5) AS same_answer;
RESET pg_lion.enable_plain_scan;

/*
 * lpp_uses() says whether the plan of q has a node matching the pattern.  The
 * plans below are made with the sequential and bitmap scans and the hash and
 * merge joins disabled, so that where a plain lion scan can be made it is:
 * the index-only scan of a partial index a query needs no column of, and the
 * parameterized scan inside a nested loop.  Switched off, neither is made at
 * all, and the btree's index scan still is.  LionOrdered, with every core
 * scan disabled, builds its lion side from the bitmap path instead of the
 * plain one (DESIGN.md §30.2).
 */
CREATE FUNCTION lpp_uses(q text, pattern text) RETURNS boolean
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ pattern THEN
			RETURN true;
		END IF;
	END LOOP;
	RETURN false;
END $$;
CREATE FUNCTION lpp_ordered(q text) RETURNS boolean
LANGUAGE plpgsql AS $$
DECLARE
	used boolean;
BEGIN
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('enable_tidscan', 'off', true);
	used := lpp_uses(q, 'Custom Scan \(LionOrdered\)');
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('enable_tidscan', 'on', true);
	RETURN used;
END $$;
CREATE TABLE lpp_o (v int);
INSERT INTO lpp_o VALUES (5), (6), (7);
ANALYZE lpp_o;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SELECT lpp_uses('SELECT count(*) FROM lpp WHERE flag', 'Index Only Scan using lpp_part') AS index_only,
	   lpp_uses('SELECT count(*) FROM lpp_o o JOIN lpp ON lpp.k2 = o.v', 'Index Scan using lpp_k2') AS parameterized,
	   lpp_uses('SELECT pad FROM lpp WHERE id = 5', 'Index Scan using lpp_pkey') AS btree,
	   lpp_ordered('SELECT id FROM lpp WHERE k2 = 5 ORDER BY id LIMIT 3') AS lion_ordered;
SET pg_lion.enable_plain_scan = off;
SELECT lpp_uses('SELECT count(*) FROM lpp WHERE flag', 'Index Only Scan using lpp_part') AS index_only,
	   lpp_uses('SELECT count(*) FROM lpp_o o JOIN lpp ON lpp.k2 = o.v', 'Index Scan using lpp_k2') AS parameterized,
	   lpp_uses('SELECT pad FROM lpp WHERE id = 5', 'Index Scan using lpp_pkey') AS btree,
	   lpp_ordered('SELECT id FROM lpp WHERE k2 = 5 ORDER BY id LIMIT 3') AS lion_ordered;
-- and the answers are the sequential scan's
SELECT (SELECT count(*) FROM lpp WHERE flag) =
	   (SELECT count(*) FROM lpp WHERE flag IS TRUE AND k + 0 >= 0) AS same_count,
	   (SELECT count(*) FROM lpp_o o JOIN lpp ON lpp.k2 = o.v) =
	   (SELECT count(*) FROM lpp_o o JOIN lpp ON lpp.k2 + 0 = o.v) AS same_join,
	   ARRAY(SELECT id FROM lpp WHERE k2 = 5 ORDER BY id LIMIT 3) =
	   ARRAY(SELECT id FROM lpp WHERE k2 + 0 = 5 ORDER BY id LIMIT 3) AS same_order;
RESET pg_lion.enable_plain_scan;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_hashjoin;
RESET enable_mergejoin;

RESET pg_lion.enable_count_pushdown;
DROP TABLE lpp, lpp_o;
DROP FUNCTION lpp_ordered(text);
DROP FUNCTION lpp_uses(text, text);
