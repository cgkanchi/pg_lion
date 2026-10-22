-- The index-usage statistics lion's nodes and SQL functions keep, and which of
-- core's enable_* settings disable lion's paths (REFERENCE.md, "Monitoring"
-- and "Settings").
--
-- Every lion read of an index counts as core's index scans count theirs in
-- pg_stat_user_indexes: one idx_scan for each scan or rescan that reads it,
-- the rows it returned or counted from it as idx_tup_read, and the heap rows
-- it fetched as idx_tup_fetch - an index's own where it fetches them by
-- itself, the table's where they are a recheck of a bitmap of the index, as
-- a bitmap heap scan's are.  pg_stat_user_tables adds the indexes' scans to
-- the table's idx_scan, and their fetches and its own to its idx_tup_fetch,
-- with no seq_scan.  Each case runs one statement, flushes the statistics,
-- and prints the change of every counter that moved.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

-- 3000 rows: a 10 values, b 7, c the id under a covering btree, d words
CREATE TABLE ius (id int, a int, b int, c int, d tsvector)
	WITH (autovacuum_enabled = off);
INSERT INTO ius SELECT i, i % 10, i % 7, i,
	   to_tsvector('simple', 'w' || (i % 5) || ' w' || (i % 11))
  FROM generate_series(1, 3000) i;
CREATE INDEX ius_a ON ius USING lion (a);
CREATE INDEX ius_b ON ius USING lion (b);
CREATE INDEX ius_d ON ius USING lion (d) WITH (store_positions = true);
CREATE INDEX ius_c ON ius (c) INCLUDE (id);
-- a dimension on ius.a, three of whose keys have no row
CREATE TABLE ius_dim (pk int PRIMARY KEY, name text NOT NULL)
	WITH (autovacuum_enabled = off);
INSERT INTO ius_dim SELECT i, 'n' || (i % 2) FROM generate_series(0, 12) i;
-- the same rows in two partitions, a indexed in each
CREATE TABLE iusp (id int, a int) PARTITION BY RANGE (id);
CREATE TABLE iusp1 PARTITION OF iusp FOR VALUES FROM (1) TO (1001)
	WITH (autovacuum_enabled = off);
CREATE TABLE iusp2 PARTITION OF iusp FOR VALUES FROM (1001) TO (3001)
	WITH (autovacuum_enabled = off);
INSERT INTO iusp SELECT id, a FROM ius;
CREATE INDEX iusp_a ON iusp USING lion (a);
VACUUM (FREEZE, ANALYZE) ius, ius_dim, iusp1, iusp2;

/*
 * The counters of this test's tables and their indexes as they stand, and
 * the change of each since the last call: an index's idx_scan, idx_tup_read
 * and idx_tup_fetch, a table's seq_scan, idx_scan and idx_tup_fetch.  Only
 * those that moved are listed.
 */
CREATE TABLE ius_was (rel text PRIMARY KEY, seq bigint, scans bigint,
					  rd bigint, fetched bigint);
CREATE FUNCTION ius_delta()
RETURNS TABLE (rel text, seq_scan bigint, idx_scan bigint,
			   idx_tup_read bigint, idx_tup_fetch bigint)
LANGUAGE plpgsql AS $$
BEGIN
	CREATE TEMP TABLE ius_now ON COMMIT DROP AS
	SELECT s.indexrelname::text AS rel, NULL::bigint AS seq, s.idx_scan AS scans,
		   s.idx_tup_read AS rd, s.idx_tup_fetch AS fetched
	  FROM pg_stat_user_indexes s
	 WHERE s.relname IN ('ius', 'ius_dim', 'iusp1', 'iusp2')
	UNION ALL
	SELECT 'table ' || t.relname, t.seq_scan, coalesce(t.idx_scan, 0), NULL,
		   coalesce(t.idx_tup_fetch, 0)
	  FROM pg_stat_user_tables t
	 WHERE t.relname IN ('ius', 'ius_dim', 'iusp1', 'iusp2');
	RETURN QUERY
	SELECT n.rel, n.seq - w.seq, n.scans - w.scans, n.rd - w.rd,
		   n.fetched - w.fetched
	  FROM ius_now n JOIN ius_was w USING (rel)
	 WHERE (n.seq, n.scans, n.rd, n.fetched) IS DISTINCT FROM
		   (w.seq, w.scans, w.rd, w.fetched)
	 ORDER BY n.rel LIKE 'table %', n.rel;
	DELETE FROM ius_was;
	INSERT INTO ius_was SELECT * FROM ius_now;
END $$;
/*
 * The node q's plan runs - its first custom scan, or else its first scan -
 * and, from ius_run(), the rows it returns, run as planned.
 */
CREATE FUNCTION ius_node(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	node text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF node IS NULL AND ln ~ 'Custom Scan \(' THEN
			node := substring(ln FROM 'Custom Scan \(([A-Za-z0-9]+)\)');
		END IF;
	END LOOP;
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF node IS NULL AND ln ~ '(Seq|Index|Bitmap Heap) Scan' THEN
			node := substring(ln FROM '((Seq|Index Only|Index|Bitmap Heap) Scan)');
		END IF;
	END LOOP;
	RETURN node;
END $$;
CREATE FUNCTION ius_run(q text, OUT node text, OUT nrows bigint)
LANGUAGE plpgsql AS $$
DECLARE
	r record;
BEGIN
	node := ius_node(q);
	nrows := 0;
	FOR r IN EXECUTE q LOOP
		nrows := nrows + 1;
	END LOOP;
END $$;
SELECT pg_stat_force_next_flush() AS f \gset
SELECT count(*) AS changed FROM ius_delta();

-- lion's paths over core's index scans: the AM's plain scans off, and the
-- margin marking lion's paths down against the rest
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET pg_lion.enable_plain_scan = off;
SET pg_lion.pushdown_margin = 1000;

-- ---------- 1. LionCount ----------
-- an equality: one scan of the index, the 300 rows it counted
SELECT count(*) FROM ius WHERE a = 3;
SELECT pg_stat_force_next_flush() AS f \gset
SELECT * FROM ius_delta();
-- an IN list and an AND of two indexes: a scan of each, the rows counted
SELECT * FROM ius_run('SELECT count(*) FROM ius WHERE a IN (1, 2) AND b = 3');
SELECT pg_stat_force_next_flush() AS f \gset
SELECT * FROM ius_delta();
-- a GROUP BY: the rows of every group
SELECT * FROM ius_run('SELECT a, count(*) FROM ius GROUP BY a');
SELECT pg_stat_force_next_flush() AS f \gset
SELECT * FROM ius_delta();
-- ... whose WHERE selects nothing: the WHERE's index, and not the group
-- column's, which is never walked
SELECT * FROM ius_run($$SELECT a, count(*) FROM ius WHERE d @@ 'w99' GROUP BY a$$);
SELECT pg_stat_force_next_flush() AS f \gset
SELECT * FROM ius_delta();
-- a count(DISTINCT): the distinct values it counted
SELECT * FROM ius_run('SELECT count(DISTINCT b) FROM ius WHERE a = 3');
SELECT pg_stat_force_next_flush() AS f \gset
SELECT * FROM ius_delta();
-- a text search query of two words
SELECT * FROM ius_run($$SELECT count(*) FROM ius WHERE d @@ 'w1 & w3'$$);
SELECT pg_stat_force_next_flush() AS f \gset
SELECT * FROM ius_delta();
-- a residual clause, checked in the heap: the table's rows fetched.  The
-- aggregate over lion's walk of the covering btree is a plan of lion's too,
-- which the margin does not mark the count down against, and cheaper: it is
-- turned off here.
SET pg_lion.enable_btree_scan = off;
SELECT * FROM ius_run('SELECT count(*) FROM ius WHERE a = 3 AND c BETWEEN 100 AND 400');
RESET pg_lion.enable_btree_scan;
SELECT pg_stat_force_next_flush() AS f \gset
SELECT * FROM ius_delta();
-- three rescans with another Param: three scans
SELECT * FROM ius_run('SELECT x, (SELECT count(*) FROM ius WHERE a = x) FROM generate_series(1, 3) x');
SELECT pg_stat_force_next_flush() AS f \gset
SELECT * FROM ius_delta();
-- a partitioned table: a scan of each partition's index, and its rows
SELECT * FROM ius_run('SELECT count(*) FROM iusp WHERE a = 3');
SELECT pg_stat_force_next_flush() AS f \gset
SELECT * FROM ius_delta();

-- ---------- 2. the FK-side joins ----------
-- the rows they return or count (the dimension's own scan is core's)
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
SELECT * FROM ius_run('SELECT d.pk FROM ius_dim d WHERE EXISTS (SELECT 1 FROM ius WHERE ius.a = d.pk AND ius.b = 3)');
SELECT pg_stat_force_next_flush() AS f \gset
SELECT * FROM ius_delta();
SELECT * FROM ius_run('SELECT d.pk FROM ius_dim d WHERE NOT EXISTS (SELECT 1 FROM ius WHERE ius.a = d.pk AND ius.b = 3)');
SELECT pg_stat_force_next_flush() AS f \gset
SELECT * FROM ius_delta();
SELECT * FROM ius_run('SELECT d.name, count(*) FROM ius JOIN ius_dim d ON d.pk = ius.a WHERE ius.b = 3 GROUP BY d.name');
SELECT pg_stat_force_next_flush() AS f \gset
SELECT * FROM ius_delta();
-- no dimension row: the fact filters' index, and not the fk index, which
-- has no key to look up
SELECT * FROM ius_run($$SELECT d.name, count(*) FROM ius JOIN ius_dim d ON d.pk = ius.a WHERE ius.b = 3 AND d.name = 'zz' GROUP BY d.name$$);
SELECT pg_stat_force_next_flush() AS f \gset
SELECT * FROM ius_delta();
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_nestloop;

-- ---------- 3. the scans that return rows ----------
-- LionBm25: the entries its walks gave, and a fetch of each row returned
SELECT * FROM ius_run($$SELECT id FROM ius WHERE d @@ 'w1' ORDER BY lion_bm25_score(d, 'w1', 'ius_d') DESC LIMIT 5$$);
SELECT pg_stat_force_next_flush() AS f \gset
SELECT * FROM ius_delta();
-- LionOrdered over a btree: the btree's scan as core counts it (index only
-- here, no fetch), and the lion index's rows in the set it builds
SELECT * FROM ius_run('SELECT id FROM ius WHERE a = 3 ORDER BY c LIMIT 5');
SELECT pg_stat_force_next_flush() AS f \gset
SELECT * FROM ius_delta();
-- ... over a lion column, walked in its order: the rows it walked and
-- fetched
SELECT * FROM ius_run('SELECT id, a FROM ius WHERE b = 2 ORDER BY a LIMIT 5');
SELECT pg_stat_force_next_flush() AS f \gset
SELECT * FROM ius_delta();
-- LionBtreeScan: the btree's walk, and the set's rows
SELECT * FROM ius_run('SELECT id, c FROM ius WHERE c BETWEEN 100 AND 400 AND a = 3');
SELECT pg_stat_force_next_flush() AS f \gset
SELECT * FROM ius_delta();

-- ---------- 4. core's scans of the same index, for comparison ----------
-- (lion's walk of ius_c off, which at this margin would answer the count
-- from the btree)
RESET enable_bitmapscan;
SET pg_lion.enable_count_pushdown = off;
SET pg_lion.enable_btree_scan = off;
SELECT * FROM ius_run('SELECT count(*) FROM ius WHERE a = 3');
SELECT pg_stat_force_next_flush() AS f \gset
SELECT * FROM ius_delta();
RESET pg_lion.enable_btree_scan;
RESET pg_lion.enable_count_pushdown;
SET enable_bitmapscan = off;

-- ---------- 5. rows the heap is asked for ----------
-- the rows of the pages an UPDATE left not all-visible are rechecked in the
-- heap, as a bitmap heap scan fetches them: the table's idx_tup_fetch
BEGIN;
SET LOCAL enable_seqscan = on;
UPDATE ius SET b = b WHERE id = 1503;
COMMIT;
SELECT pg_stat_force_next_flush() AS f \gset
SELECT * FROM ius_delta();
SELECT * FROM ius_run('SELECT count(*) FROM ius WHERE a = 3');
SELECT pg_stat_force_next_flush() AS f \gset
SELECT * FROM ius_delta();

RESET enable_seqscan;
RESET enable_bitmapscan;
RESET pg_lion.enable_plain_scan;
RESET pg_lion.pushdown_margin;

-- ---------- 6. the SQL functions ----------
SELECT lion_index_count('ius_a', 3);
SELECT pg_stat_force_next_flush() AS f \gset
SELECT * FROM ius_delta();
SELECT lion_index_count('ius_a', 3, 'ius_b', 4);
SELECT pg_stat_force_next_flush() AS f \gset
SELECT * FROM ius_delta();
SELECT lion_index_count_any('ius_a', ARRAY[1, 2, 99]);
SELECT pg_stat_force_next_flush() AS f \gset
SELECT * FROM ius_delta();
SELECT groups, count FROM lion_index_count_group_stats('ius_b');
SELECT pg_stat_force_next_flush() AS f \gset
SELECT * FROM ius_delta();
SELECT * FROM lion_index_facets('ius_b', 3, 'a = 3');
SELECT pg_stat_force_next_flush() AS f \gset
SELECT * FROM ius_delta();
SELECT count(*) FROM lion_bm25('ius_d', 'w1 & w3', 4);
SELECT pg_stat_force_next_flush() AS f \gset
SELECT * FROM ius_delta();
-- the score reads the index's statistics once (the rows are a sequential
-- scan's)
SELECT count(*) FILTER (WHERE lion_bm25_score(d, 'w1', 'ius_d') > 0) FROM ius WHERE id <= 3;
SELECT pg_stat_force_next_flush() AS f \gset
SELECT * FROM ius_delta();

-- every index read was last scanned, lion's as core's
SELECT indexrelname, idx_scan > 0 AS scanned, last_idx_scan IS NOT NULL AS last_scan
  FROM pg_stat_user_indexes
 WHERE relname IN ('ius', 'iusp1', 'iusp2')
 ORDER BY 1;

-- ---------- 7. core's enable_* settings ----------
-- enable_indexscan disables every lion path, as it does core's index and
-- index-only scans; enable_indexonlyscan the ones that answer from the index
-- alone - LionCount, the FK-side joins - and not LionOrdered, LionBtreeScan
-- or LionBm25, which read the rows.  A disabled path is kept and loses to an
-- enabled one, as core's do.
CREATE FUNCTION ius_switch(q text, sw text) RETURNS text
LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	IF sw <> '' THEN
		PERFORM set_config(sw, 'off', true);
	END IF;
	RETURN ius_node(q);
END $$;
SELECT n, ius_switch(q, '') AS chosen,
	   ius_switch(q, 'enable_indexscan') AS no_indexscan,
	   ius_switch(q, 'enable_indexonlyscan') AS no_indexonlyscan,
	   ius_switch(q, 'enable_bitmapscan') AS no_bitmapscan
  FROM (VALUES ('count', 'SELECT count(*) FROM ius WHERE a = 3'),
			   ('group', 'SELECT a, count(*) FROM ius GROUP BY a'),
			   ('ordered', 'SELECT id FROM ius WHERE b = 2 ORDER BY a LIMIT 5'),
			   ('bm25', $$SELECT id FROM ius WHERE d @@ 'w1' ORDER BY lion_bm25_score(d, 'w1', 'ius_d') DESC LIMIT 5$$)) v(n, q);

-- ---------- 8. the marks the statistics are read from ----------
-- A count reports a scan of each index whose directory it read
-- (lion_dir_read_since()), from the mark every directory read leaves.  The
-- marks of a transaction that leaves more than 64 of them are dropped at
-- its end: a backend that makes, reads and drops 200 indexes, each in a
-- transaction of its own, holds no more than 64, where it held one for every
-- index it ever read - and a count after that reports its reads as before.
CREATE PROCEDURE ius_churn(n int)
LANGUAGE plpgsql AS $$
BEGIN
	FOR i IN 1 .. n LOOP
		CREATE TEMP TABLE ius_tmp (a int);
		INSERT INTO ius_tmp SELECT g % 10 FROM generate_series(1, 100) g;
		CREATE INDEX ius_tmp_a ON ius_tmp USING lion (a);
		INSERT INTO ius_tmp VALUES (3);
		DROP TABLE ius_tmp;
		COMMIT;
	END LOOP;
END $$;
SELECT pg_stat_force_next_flush() AS f \gset
SELECT count(*) AS changed FROM ius_delta();
CALL ius_churn(200);
SELECT coalesce(sum(total_bytes), 0) <= 8192 AS marks_bounded
  FROM pg_backend_memory_contexts WHERE name = 'lion directory reads';
SELECT count(*) FROM ius WHERE a = 3;
SELECT pg_stat_force_next_flush() AS f \gset
SELECT * FROM ius_delta();
DROP PROCEDURE ius_churn(int);

DROP FUNCTION ius_switch(text, text);
DROP FUNCTION ius_run(text);
DROP FUNCTION ius_node(text);
DROP FUNCTION ius_delta();
DROP TABLE ius_was, ius, ius_dim, iusp;
