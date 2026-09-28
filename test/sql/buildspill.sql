-- The build's spool at the least maintenance_work_mem there is (DESIGN.md
-- §24, "Build"), over a table whose HOT chains hand it most keys' codes out
-- of order and some of whose rows bring 20,000 keys each.  Every spill then
-- writes thousands of out-of-order keys, which took a page buffer each until
-- one was shared (2026-09-28), and the budget is checked in the middle of a
-- row, so runs end in the middle of heap pages.  The index has to come out
-- the one an in-memory build writes, page for page, holding exactly the
-- heap's rows.
--
-- UNLOGGED for the byte comparison, as in build.sql, and serial builds only:
-- 16 builds nothing else, and build.sql covers the parallel ones.
\set VERBOSITY terse
SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS pg_lion;
CREATE EXTENSION IF NOT EXISTS pageinspect;

-- 64kB, and 1MB before 17; the keys below take several MB either way
SELECT CASE WHEN current_setting('server_version_num')::int >= 170000
			THEN '64kB' ELSE '1MB' END AS lion_spill_least_mem \gset

CREATE FUNCTION lion_spill_digest(idx regclass) RETURNS text
	LANGUAGE sql AS $$ SELECT md5(pg_read_binary_file(pg_relation_filepath(idx))) $$;

-- k: eight consecutive rows to a key, on one heap page but for a few; u: a
-- key per row; a: one of seven keys and one of its own, and every 5000th row
-- 20,000 keys
CREATE UNLOGGED TABLE lion_spill (id int, k int, u int, a int[], pad text)
	WITH (fillfactor = 50);
INSERT INTO lion_spill
SELECT g, g / 8, g,
	   CASE WHEN g % 5000 = 0 THEN ARRAY(SELECT generate_series(g, g + 19999))
			ELSE ARRAY[g % 7, 1000000 + g] END,
	   'p'
  FROM generate_series(1, 50000) g;

-- HOT chains, made while the table has no index: a heap-only tuple is
-- reported under its chain's root offset, after rows of its page that
-- follow the root, so most keys of k and all seven small keys of a get codes
-- out of order.
UPDATE lion_spill SET pad = pad || 'q' WHERE id % 3 = 0;
UPDATE lion_spill SET pad = pad || 'r' WHERE id % 4 = 0;
SELECT count(*) > 10000 AS has_heap_only_tuples
  FROM generate_series(0, pg_relation_size('lion_spill') /
					   current_setting('block_size')::int - 1) b,
	   heap_page_items(get_raw_page('lion_spill', b::int))
 WHERE (t_infomask2 & 32768) <> 0;		-- HEAP_ONLY_TUPLE

-- In memory, then spilled: the same index, page for page
SET max_parallel_maintenance_workers = 0;
SET maintenance_work_mem = '256MB';
CREATE INDEX lion_spill_i ON lion_spill USING lion (k, u, a);
SELECT lion_spill_digest('lion_spill_i') AS lion_spill_in_memory \gset
SET maintenance_work_mem = :'lion_spill_least_mem';
REINDEX INDEX lion_spill_i;
RESET maintenance_work_mem;
RESET max_parallel_maintenance_workers;
SELECT lion_spill_digest('lion_spill_i') = :'lion_spill_in_memory' AS spilled_same;

-- ... and the right one: every heap row under every key, and as many keys
-- and TIDs in each column as the heap has
SELECT lion_index_verify('lion_spill_i', true);
SELECT s.attno, s.entries = h.keys AND s.ntids = h.pairs AS exact
  FROM lion_index_stats('lion_spill_i') s
  JOIN (SELECT 1 AS attno, count(DISTINCT k) AS keys, count(*) AS pairs
		  FROM lion_spill
		UNION ALL
		SELECT 2, count(DISTINCT u), count(*) FROM lion_spill
		UNION ALL
		SELECT 3, count(DISTINCT e), count(*) FROM lion_spill, unnest(a) e) h
	ON h.attno = s.attno
 ORDER BY s.attno;

DROP TABLE lion_spill;
DROP FUNCTION lion_spill_digest(regclass);
