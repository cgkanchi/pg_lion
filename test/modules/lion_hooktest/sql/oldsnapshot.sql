-- PostgreSQL 16's old_snapshot_threshold (DESIGN.md §9, "old_snapshot_threshold").
-- test/hook-check.sh starts a 16 server with old_snapshot_threshold = 60 for
-- this file alone: it is a postmaster setting, which installcheck's server
-- cannot SET, and 17 removed it, so the file runs on 16 and nowhere else.
--
-- With the threshold set, VACUUM and pruning may remove rows an old snapshot
-- still sees, and core's access methods turn that into "snapshot too old" by
-- testing every page they read.  Lion tests none, so it is not read at all
-- while the threshold is set: the planner prices every lion path out and
-- offers no LionCount, LionOrdered or LionBm25 path, and what reaches a lion
-- index under an MVCC snapshot anyway - a plan with every other path
-- disabled, a direct SQL count or ranking - is an ERROR.  Without the
-- threshold every plan below reads a lion index: two LionCounts, a plain scan
-- of lion_ost_k, a LionOrdered and a LionBm25 scan.  Inserts, VACUUM and
-- lion_index_verify() are not refused.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;
SHOW old_snapshot_threshold;

-- k is in heap order, a hundred rows a key, so one key is one heap page.
CREATE TABLE lion_ost (id int NOT NULL, k int NOT NULL, g int NOT NULL, t tsvector NOT NULL);
INSERT INTO lion_ost
SELECT i, i / 100, i % 7, to_tsvector('simple', 'w' || (i % 10) || ' x' || (i % 3))
  FROM generate_series(1, 20000) i;
CREATE INDEX lion_ost_k ON lion_ost USING lion (k);
CREATE INDEX lion_ost_g ON lion_ost USING lion (g);
CREATE INDEX lion_ost_t ON lion_ost USING lion (t);
VACUUM (FREEZE, ANALYZE) lion_ost;

-- The planner does not use lion: no LionCount, no lion scan of any kind.
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_ost WHERE k = 42;
EXPLAIN (COSTS OFF) SELECT g, count(*) FROM lion_ost GROUP BY g;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_ost WHERE k = 42 AND g = 3;
EXPLAIN (COSTS OFF) SELECT id FROM lion_ost WHERE g = 3 ORDER BY k LIMIT 3;
EXPLAIN (COSTS OFF)
SELECT id FROM lion_ost WHERE t @@ 'w3'::tsquery
 ORDER BY lion_bm25_score(t, 'w3'::tsquery, 'lion_ost_t') DESC LIMIT 3;
SELECT count(*) FROM lion_ost WHERE k = 42;
SELECT g, count(*) FROM lion_ost GROUP BY g ORDER BY g;

-- Every other path disabled: a lion path is all that is left, and its scan
-- refuses to read - amgetbitmap, then amgettuple.
SET enable_seqscan = off;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_ost WHERE k = 42;
SELECT count(*) FROM lion_ost WHERE k = 42;
SET enable_bitmapscan = off;
SET enable_indexscan = on;
EXPLAIN (COSTS OFF) SELECT id FROM lion_ost WHERE k = 42;
SELECT id FROM lion_ost WHERE k = 42;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
RESET enable_indexonlyscan;

-- The direct SQL counts and ranking read under the statement's snapshot, and
-- refuse as the scans do.
\set VERBOSITY terse
SELECT lion_index_count('lion_ost_k', 42);
SELECT lion_index_count('lion_ost_k', 42, 'lion_ost_g', 3);
SELECT lion_index_count_any('lion_ost_k', ARRAY[1, 2, 3]);
SELECT lion_index_count_stats('lion_ost_k', 42);
SELECT * FROM lion_index_count_group_stats('lion_ost_g');
SELECT * FROM lion_index_facets('lion_ost_g', 3);
SELECT * FROM lion_bm25('lion_ost_t', 'w3'::tsquery, 3);
\set VERBOSITY default

-- Writes, VACUUM and the structural check are not refused.
INSERT INTO lion_ost SELECT i, 42, i % 7, to_tsvector('simple', 'w1') FROM generate_series(20001, 20050) i;
UPDATE lion_ost SET g = 6 WHERE id BETWEEN 4201 AND 4210;
DELETE FROM lion_ost WHERE id BETWEEN 4211 AND 4220;
VACUUM (FREEZE, INDEX_CLEANUP ON) lion_ost;
SELECT lion_index_verify('lion_ost_k', true);
SELECT lion_index_verify('lion_ost_g', true);
SELECT lion_index_verify('lion_ost_t', true);
SELECT ntids = (SELECT count(*) FROM lion_ost) AS one_tid_a_row FROM lion_index_stats('lion_ost_k');
SELECT count(*) FROM lion_ost WHERE k = 42;

DROP TABLE lion_ost;
