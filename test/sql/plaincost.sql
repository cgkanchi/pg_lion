-- What a plain index scan (amgettuple) is priced at, against the bitmap scan
-- of the same index (DESIGN.md §29.11).
--
-- A plain lion scan hands out its TIDs in heap order - the bitmap heap scan's
-- pages in the bitmap heap scan's order - so its heap side is priced as the
-- bitmap heap scan's, with its per-row fetches on top: a result of about a
-- row to a page goes to the plain scan, which builds no bitmap, and one of
-- many rows on every page to the bitmap scan, which fetches a page's rows at
-- once.  It used to be priced with the first column's ANALYZE correlation,
-- which for a column of few values placed at random is the sum of their
-- squared frequencies, not 0: a scattered result of a few percent was priced
-- as random reads and went to a bitmap scan up to 2.5 times slower, and at a
-- random_page_cost of 1.1 a count of 13% of the rows went to the plain scan -
-- with an always-true range beside the other columns, to one that walked
-- the range entry by entry and restarted the other columns' sets for each
-- entry (2026-09-27: on a much larger table such a query did not finish in
-- 60 s).  A range alone - one column's keys walked in key order - is
-- a heap pass per key, priced as such.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET synchronous_commit = on;
-- a sample of every row: the statistics, and the plans, do not depend on it
SET default_statistics_target = 1000;
SET max_parallel_workers_per_gather = 0;

-- 200000 rows, 44 to a page; every column a hash of the row number.  status
-- has three values (60/20/20%), supp two (90/10%), flag is 80% true, pc has
-- 200 values and tags holds 'ga' 30% of the time; all placed at random.  cl
-- has four values stored in value order.
CREATE TABLE lpc (id int, status int, supp int, flag bool, pc int, cl int,
				  tags text[], pad text);
INSERT INTO lpc
SELECT g,
	   (ARRAY[0, 0, 0, 1, 2])[1 + ((hashint8extended(g, 1) & 2147483647) % 5)],
	   CASE WHEN (hashint8extended(g, 2) & 2147483647) % 10 = 0 THEN 1 ELSE 0 END,
	   (hashint8extended(g, 3) & 2147483647) % 5 <> 0,
	   ((hashint8extended(g, 4) & 2147483647) % 200)::int,
	   (g - 1) / 50000,
	   CASE WHEN (hashint8extended(g, 6) & 2147483647) % 10 < 3
			THEN ARRAY['ga', 'p' || ((hashint8extended(g, 7) & 2147483647) % 11)]
			ELSE ARRAY['p' || ((hashint8extended(g, 7) & 2147483647) % 11)] END,
	   repeat('x', 100)
  FROM generate_series(1, 200000) g;
CREATE INDEX lpc_lion ON lpc USING lion (status, supp, flag, pc, tags);
CREATE INDEX lpc_cl ON lpc USING lion (cl);
VACUUM (FREEZE, ANALYZE) lpc;

/* The scan nodes of a plan with nothing disabled, on one line. */
CREATE FUNCTION lpc_plan(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	res text := '';
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'Scan' THEN
			res := res || CASE WHEN res = '' THEN '' ELSE ' / ' END ||
				   btrim(regexp_replace(ln, '->', ''));
		END IF;
	END LOOP;
	RETURN res;
END $$;

/*
 * lpc_bufs() runs a query as a plain index scan, or as a bitmap scan, under
 * EXPLAIN (ANALYZE, BUFFERS) and returns the shared buffers its top node
 * touched, hits and reads together.
 */
CREATE FUNCTION lpc_bufs(q text, plain boolean) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	res bigint := NULL;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', CASE WHEN plain THEN 'off' ELSE 'on' END, true);
	PERFORM set_config('enable_indexscan', CASE WHEN plain THEN 'on' ELSE 'off' END, true);
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF res IS NULL AND ln ~ 'Buffers: shared' THEN
			res := coalesce(substring(ln from 'hit=(\d+)')::bigint, 0) +
				   coalesce(substring(ln from 'read=(\d+)')::bigint, 0);
		END IF;
	END LOOP;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	RETURN res;
END $$;

-- the count pushdown would answer the counts below from the index alone
SET pg_lion.enable_count_pushdown = off;

-- ---------- 1. a large result goes to the bitmap scan, a small one to the plain scan ----------
-- At the default random_page_cost and at 1.1, as on the benchmark's server.
-- 13% of the rows, on every page; the same with an always-true range beside
-- it, the benchmark's query; 43% of the rows, on every page; and 1000, 200
-- and 20 rows, about one to a page.
CREATE TABLE lpc_q (n int, q text);
INSERT INTO lpc_q VALUES
	(1, $$SELECT count(*) FROM lpc WHERE status = 0 AND supp = 0 AND flag AND tags && '{ga}'$$),
	(2, $$SELECT count(*) FROM lpc WHERE status = 0 AND supp = 0 AND flag AND pc >= 0 AND tags && '{ga}'$$),
	(3, $$SELECT sum(id) FROM lpc WHERE status = 0 AND supp = 0 AND flag$$),
	(4, $$SELECT sum(id) FROM lpc WHERE pc = 5$$),
	(5, $$SELECT sum(id) FROM lpc WHERE pc = 5 AND status = 1$$),
	(6, $$SELECT sum(id) FROM lpc WHERE pc = 5 AND status = 1 AND supp = 1$$);
SELECT n, lpc_plan(q) FROM lpc_q ORDER BY n;
-- 43% of the rows at 1.1: with a random read priced near a sequential one,
-- cost_index() charges a result on every page no more than its I/O at
-- random_page_cost, less than the plain scan's fetches of 19 rows to a page
-- cost beyond the bitmap scan's; lion charges the rest to the path once it is
-- built, and the bitmap scan it had displaced is chosen (DESIGN.md §29.11:
-- 22 ms against the plain scan's 26 on this table).
SET random_page_cost = 1.1;
SELECT n, lpc_plan(q) FROM lpc_q ORDER BY n;
RESET random_page_cost;

-- ---------- 2. a column stored in value order keeps its correlation ----------
-- cl's four values each fill a quarter of the heap: its correlation is 1
-- after the baseline its frequencies give is taken off, as before, and a
-- quarter of the rows is read as the pages that hold them.
SELECT lpc_plan('SELECT sum(id) FROM lpc WHERE cl = 1');
SELECT lpc_plan('SELECT sum(id) FROM lpc WHERE cl IN (1, 2)');

-- ---------- 3. a range beside the other columns' sets, key by key ----------
-- Each pc key has rows at nearly every container key, so restarting the
-- other columns' stream for each key walked read their sets once per key:
-- the plain scan is a WINDOW now (DESIGN.md §29.3) and reads about what the
-- bitmap scan reads.
SELECT lpc_bufs(q, true) <= 2 * lpc_bufs(q, false) AS plain_reads_about_the_bitmaps
  FROM (VALUES ('SELECT count(*) FROM lpc WHERE status = 0 AND supp = 0 AND flag AND pc < 20'),
			   ('SELECT count(*) FROM lpc WHERE status = 0 AND supp = 0 AND flag AND pc >= 0 AND tags && ''{ga}'''),
			   ('SELECT count(*) FROM lpc WHERE status = 0 AND pc BETWEEN 3 AND 5')) v(q);
-- and the answers are the sequential scan's
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*), sum(id) FROM lpc WHERE status = 0 AND supp = 0 AND flag AND pc < 20;
SELECT count(*), sum(id) FROM lpc WHERE status = 0 AND supp = 0 AND flag AND pc >= 0 AND tags && '{ga}';
SELECT count(*), sum(id) FROM lpc WHERE status = 0 AND pc BETWEEN 3 AND 5;
RESET enable_bitmapscan;
SET enable_indexscan = off;
SET enable_seqscan = on;
SELECT count(*), sum(id) FROM lpc WHERE status = 0 AND supp = 0 AND flag AND pc < 20;
SELECT count(*), sum(id) FROM lpc WHERE status = 0 AND supp = 0 AND flag AND pc >= 0 AND tags && '{ga}';
SELECT count(*), sum(id) FROM lpc WHERE status = 0 AND pc BETWEEN 3 AND 5;
RESET enable_indexscan;
RESET enable_seqscan;

-- ---------- 4. a range alone: a walk in key order, a heap pass per key ----------
-- The plain scan hands out one key's rows in heap order, then the next key's
-- (DESIGN.md §29.3, WALK): a pass over the heap per key.  One key is exactly
-- `pc = 5`, a scan in heap order, and goes to the plain scan as that does
-- (0.54 ms against the bitmap scan's 0.73).  Twenty keys read their pages in
-- twenty passes where the bitmap scan reads them once, and one key with nine
-- rows on every page is fetched a row at a time where the bitmap scan takes a
-- page at a time: both go to the bitmap scan (10 ms against 14, and 12
-- against 18).  The one key used to be priced as a btree scan's random reads,
-- and went to the bitmap scan too.
SELECT lpc_plan('SELECT sum(id) FROM lpc WHERE pc BETWEEN 5 AND 5');
SELECT lpc_plan('SELECT sum(id) FROM lpc WHERE pc BETWEEN 1 AND 20');
SELECT lpc_plan('SELECT sum(id) FROM lpc WHERE status >= 2');
-- and the answers are the sequential scan's
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*), sum(id) FROM lpc WHERE pc BETWEEN 5 AND 5;
SELECT count(*), sum(id) FROM lpc WHERE pc BETWEEN 1 AND 20;
SELECT count(*), sum(id) FROM lpc WHERE status >= 2;
RESET enable_bitmapscan;
SET enable_indexscan = off;
SET enable_seqscan = on;
SELECT count(*), sum(id) FROM lpc WHERE pc BETWEEN 5 AND 5;
SELECT count(*), sum(id) FROM lpc WHERE pc BETWEEN 1 AND 20;
SELECT count(*), sum(id) FROM lpc WHERE status >= 2;
RESET enable_indexscan;
RESET enable_seqscan;

RESET pg_lion.enable_count_pushdown;
DROP TABLE lpc, lpc_q;
DROP FUNCTION lpc_plan(text);
DROP FUNCTION lpc_bufs(text, boolean);
