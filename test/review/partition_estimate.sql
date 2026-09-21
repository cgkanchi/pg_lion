-- Run only in a disposable database with pg_lion installed.
-- Stale parent statistics must still allow core final aggregation to spill.
\set ON_ERROR_STOP on
LOAD 'pg_lion';
SET statement_timeout = '60s';
SET max_parallel_workers_per_gather = 0;
CREATE TABLE review_groups (part int, k int) PARTITION BY LIST (part);
CREATE TABLE review_groups_0 PARTITION OF review_groups FOR VALUES IN (0);
CREATE TABLE review_groups_1 PARTITION OF review_groups FOR VALUES IN (1);
INSERT INTO review_groups SELECT i%2, i%10 FROM generate_series(1,20000) i;
CREATE INDEX ON review_groups USING lion (k);
VACUUM (FREEZE, ANALYZE) review_groups;
-- Keep inherited parent statistics at ten groups while cardinality grows.
INSERT INTO review_groups SELECT i%2, i FROM generate_series(20001,40000) i;
VACUUM (FREEZE) review_groups;
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SET enable_seqscan = off;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF)
SELECT k,count(*) FROM review_groups GROUP BY k;
-- The lateral dependency delays inspection until the grouping node has emitted
-- its first row. The removed extension-owned hash contexts should be absent;
-- the EXPLAIN above is the evidence that core final aggregation spills.
SELECT current_setting('work_mem') AS work_mem, groups.n,
       memory.bytes AS group_hash_allocated_bytes, memory.used_bytes AS group_hash_used_bytes
FROM (SELECT k,count(*) AS n FROM review_groups GROUP BY k) groups
CROSS JOIN LATERAL (
    SELECT sum(total_bytes) AS bytes, sum(used_bytes) AS used_bytes
    FROM pg_backend_memory_contexts
    WHERE name LIKE 'RoaringCount group hash%' OR name LIKE 'LionCount group hash%'
    OFFSET (groups.k-groups.k)
) memory
LIMIT 1;
CREATE TEMP TABLE actual_groups AS SELECT k,count(*) AS n FROM review_groups GROUP BY k;
SET pg_lion.enable_count_pushdown = off;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF)
SELECT k,count(*) FROM review_groups GROUP BY k;
CREATE TEMP TABLE expected_groups AS SELECT k,count(*) AS n FROM review_groups GROUP BY k;
DO $$ BEGIN
    IF EXISTS ((TABLE actual_groups EXCEPT ALL TABLE expected_groups)
               UNION ALL (TABLE expected_groups EXCEPT ALL TABLE actual_groups)) THEN
        RAISE EXCEPTION 'Partitioned aggregation disagrees with ordinary aggregation';
    END IF;
END $$;
SELECT count(*) AS exact_checked_groups FROM actual_groups;
DROP TABLE actual_groups, expected_groups, review_groups;
