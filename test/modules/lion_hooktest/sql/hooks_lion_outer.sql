-- pg_lion's create_upper_paths_hook installed AFTER another extension's, so
-- pg_lion's is the one core calls and it chains to the other.  Without a
-- preload this is LOAD-on-first-use: lion_hooktest is LOADed, and pg_lion is
-- loaded by the planner in the middle of planning the first query that
-- touches a lion index; with shared_preload_libraries = 'lion_hooktest,
-- pg_lion' the order is the same and the loads below are no-ops.
\set VERBOSITY terse
LOAD 'lion_hooktest';
SELECT lion_hooktest_reset();

-- the first query over a lion index: pushed down, and exact
EXPLAIN (COSTS OFF) SELECT count(*) FROM hk WHERE k = 3;
SELECT count(*) FROM hk WHERE k = 3;
EXPLAIN (COSTS OFF) SELECT g, count(*) FROM hk WHERE k IN (1, 2) GROUP BY g;
SELECT g, count(*) FROM hk WHERE k IN (1, 2) GROUP BY g ORDER BY g;
EXPLAIN (COSTS OFF) SELECT k, count(*) FROM hk_p GROUP BY k;
SELECT k, count(*) FROM hk_p WHERE k IN (0, 7, 19) GROUP BY k ORDER BY k;

-- the other hook ran on every query, and before pg_lion added its path
SELECT lion_hooktest_calls() > 0 AS hooktest_ran,
	   lion_hooktest_saw_lion() AS hooktest_saw_lion_path;

-- the same answers without the pushdown
SET pg_lion.enable_count_pushdown = off;
SELECT count(*) FROM hk WHERE k = 3;
SELECT g, count(*) FROM hk WHERE k IN (1, 2) GROUP BY g ORDER BY g;
SELECT k, count(*) FROM hk_p WHERE k IN (0, 7, 19) GROUP BY k ORDER BY k;
RESET pg_lion.enable_count_pushdown;

-- both prefixes are reserved
SET pg_lion.no_such_setting = 1;
SET lion_hooktest.no_such_setting = 1;
