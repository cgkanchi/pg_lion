-- Both libraries preloaded, each with a custom WAL resource manager on its
-- default id: pg_lion's reserved 170 and lion_hooktest's development id 128
-- (RM_EXPERIMENTAL_ID).  The collision itself is a startup failure, which
-- test/hook-check.sh checks from the server log.
\set VERBOSITY terse
SET client_min_messages = warning;
SET synchronous_commit = on;
SELECT rm_id, rm_name FROM pg_get_wal_resource_managers()
 WHERE rm_name IN ('pg_lion', 'lion_hooktest') ORDER BY rm_id;
SHOW pg_lion.rmgr_id;
SHOW lion_hooktest.rmgr_id;

-- an index written through pg_lion's resource manager, next to the other one
CREATE TABLE hk_r (k int, v int);
INSERT INTO hk_r SELECT i % 50, i FROM generate_series(1, 20000) i;
CREATE INDEX hk_r_k ON hk_r USING lion (k) WITH (wal_mode = rmgr);
SELECT lion_index_wal_mode('hk_r_k');
INSERT INTO hk_r SELECT i % 50, i FROM generate_series(20001, 30000) i;
DELETE FROM hk_r WHERE v % 5 = 0;
VACUUM hk_r;
SELECT lion_index_verify('hk_r_k', true);
EXPLAIN (COSTS OFF) SELECT count(*) FROM hk_r WHERE k = 7;
SELECT count(*) FROM hk_r WHERE k = 7;
SET pg_lion.enable_count_pushdown = off;
SELECT count(*) FROM hk_r WHERE k = 7;
RESET pg_lion.enable_count_pushdown;
DROP TABLE hk_r;
