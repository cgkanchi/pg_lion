-- Run only in a disposable database with roaring_index installed.
-- Demonstrates a planner-estimate guard accepting a larger actual group hash.
\set ON_ERROR_STOP on
LOAD 'roaring_index';
SET statement_timeout = '60s';
SET max_parallel_workers_per_gather = 0;
CREATE TABLE review_groups (part int, k int) PARTITION BY LIST (part);
CREATE TABLE review_groups_0 PARTITION OF review_groups FOR VALUES IN (0);
CREATE TABLE review_groups_1 PARTITION OF review_groups FOR VALUES IN (1);
INSERT INTO review_groups SELECT i%2, i%10 FROM generate_series(1,20000) i;
CREATE INDEX ON review_groups USING roaring (k);
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
-- its first row, after it has accumulated all partition groups in memory.
SELECT current_setting('work_mem') AS work_mem, groups.n,
       memory.bytes AS group_hash_allocated_bytes, memory.used_bytes AS group_hash_used_bytes
FROM (SELECT k,count(*) AS n FROM review_groups GROUP BY k) groups
CROSS JOIN LATERAL (
    SELECT sum(total_bytes) AS bytes, sum(used_bytes) AS used_bytes
    FROM pg_backend_memory_contexts
    WHERE name LIKE 'RoaringCount group hash%'
    OFFSET (groups.k-groups.k)
) memory
LIMIT 1;
SET roaring_index.enable_count_pushdown = off;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF)
SELECT k,count(*) FROM review_groups GROUP BY k;
DROP TABLE review_groups;
