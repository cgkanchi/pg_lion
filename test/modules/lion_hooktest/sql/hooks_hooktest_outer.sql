-- Another extension's create_upper_paths_hook installed AFTER pg_lion's, so
-- the other one is called by core and chains to pg_lion.  Without a preload,
-- pg_lion is loaded on first use by the first query below and lion_hooktest
-- is LOADed after it; with shared_preload_libraries = 'pg_lion,
-- lion_hooktest' the order is the same and the LOAD is a no-op.
\set VERBOSITY terse
SELECT count(*) FROM hk WHERE k = 3;
LOAD 'lion_hooktest';
SELECT lion_hooktest_reset();

EXPLAIN (COSTS OFF) SELECT count(*) FROM hk WHERE k = 3;
SELECT count(*) FROM hk WHERE k = 3;
EXPLAIN (COSTS OFF) SELECT g, count(*) FROM hk WHERE k IN (1, 2) GROUP BY g;
SELECT g, count(*) FROM hk WHERE k IN (1, 2) GROUP BY g ORDER BY g;
EXPLAIN (COSTS OFF) SELECT k, count(*) FROM hk_p GROUP BY k;
SELECT k, count(*) FROM hk_p WHERE k IN (0, 7, 19) GROUP BY k ORDER BY k;

-- the other hook ran, and found pg_lion's path already there each time the
-- previous hook - pg_lion's - returned from a grouped rel it could answer
SELECT lion_hooktest_calls() > 0 AS hooktest_ran,
	   lion_hooktest_saw_lion() AS hooktest_saw_lion_path;

SET pg_lion.enable_count_pushdown = off;
SELECT count(*) FROM hk WHERE k = 3;
SELECT g, count(*) FROM hk WHERE k IN (1, 2) GROUP BY g ORDER BY g;
SELECT k, count(*) FROM hk_p WHERE k IN (0, 7, 19) GROUP BY k ORDER BY k;
RESET pg_lion.enable_count_pushdown;

-- the LionOrdered scan (DESIGN.md §30), which set_rel_pathlist_hook adds,
-- chained the same way: the lion filter k = 3 over a walk of the btree on v
SET enable_seqscan = off; SET enable_bitmapscan = off;
SET enable_indexscan = off; SET enable_indexonlyscan = off;
EXPLAIN (COSTS OFF) SELECT v FROM hk WHERE k = 3 ORDER BY v LIMIT 5;
SELECT v FROM hk WHERE k = 3 ORDER BY v LIMIT 5;
RESET enable_seqscan; RESET enable_bitmapscan;
RESET enable_indexscan; RESET enable_indexonlyscan;
SET pg_lion.enable_ordered_scan = off;
SELECT v FROM hk WHERE k = 3 ORDER BY v LIMIT 5;
RESET pg_lion.enable_ordered_scan;
-- the other rel hook found pg_lion's path there both times
SELECT lion_hooktest_rel_calls() > 0 AS rel_hook_ran,
	   lion_hooktest_rel_saw_ordered() AS rel_hook_saw_ordered_path;

SET pg_lion.no_such_setting = 1;
SET lion_hooktest.no_such_setting = 1;
