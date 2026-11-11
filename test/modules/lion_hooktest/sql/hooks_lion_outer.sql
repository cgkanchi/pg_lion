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

-- the LionOrdered scan (DESIGN.md §30), which set_rel_pathlist_hook adds,
-- chained the same way: the lion filter k = 3 over a walk of the btree on v.
-- The rel hook's counters start here: pg_lion offers ordered paths for the
-- queries above too (an ORDER BY of a lion column walks that column).
SELECT lion_hooktest_reset();
SET enable_seqscan = off; SET enable_bitmapscan = off;
SET pg_lion.enable_plain_scan = off; SET enable_indexonlyscan = off;
SET pg_lion.pushdown_margin = 1000;
EXPLAIN (COSTS OFF) SELECT v FROM hk WHERE k = 3 ORDER BY v LIMIT 5;
SELECT v FROM hk WHERE k = 3 ORDER BY v LIMIT 5;
RESET enable_seqscan; RESET enable_bitmapscan;
RESET pg_lion.enable_plain_scan; RESET enable_indexonlyscan;
RESET pg_lion.pushdown_margin;
SET pg_lion.enable_ordered_scan = off;
SELECT v FROM hk WHERE k = 3 ORDER BY v LIMIT 5;
RESET pg_lion.enable_ordered_scan;
-- the other rel hook ran first, before pg_lion added its path
SELECT lion_hooktest_rel_calls() > 0 AS rel_hook_ran,
	   lion_hooktest_rel_saw_ordered() AS rel_hook_saw_ordered_path;

-- both prefixes are reserved
SET pg_lion.no_such_setting = 1;
SET lion_hooktest.no_such_setting = 1;

-- pg_lion's planner hook (DESIGN.md §56) plans a grouped subquery joined in
-- again as a LATERAL subquery, through the other planner hook, which here
-- refuses that form with an error: the error is dropped with the plan, and
-- the query runs as written, with the same rows, in a transaction that
-- carries on.
CREATE TABLE hk_u (id int PRIMARY KEY, r int);
INSERT INTO hk_u SELECT i, i % 100 FROM generate_series(1, 5000) i;
CREATE TABLE hk_e (uid int, s int);
INSERT INTO hk_e SELECT 1 + (i * 7) % 5000, i % 3 FROM generate_series(1, 100000) i;
CREATE INDEX hk_e_uid ON hk_e USING lion (uid);
CREATE INDEX hk_u_r ON hk_u (r);
VACUUM ANALYZE hk_u, hk_e;
EXPLAIN (COSTS OFF)
SELECT u.id, e.n FROM hk_u u
  JOIN (SELECT uid, count(*) AS n FROM hk_e GROUP BY uid) e ON e.uid = u.id
 WHERE u.r = 7;
SELECT count(*), sum(e.n) FROM hk_u u
  JOIN (SELECT uid, count(*) AS n FROM hk_e GROUP BY uid) e ON e.uid = u.id
 WHERE u.r = 7;
BEGIN;
SET LOCAL lion_hooktest.refuse_lateral = on;
EXPLAIN (COSTS OFF)
SELECT u.id, e.n FROM hk_u u
  JOIN (SELECT uid, count(*) AS n FROM hk_e GROUP BY uid) e ON e.uid = u.id
 WHERE u.r = 7;
SELECT count(*), sum(e.n) FROM hk_u u
  JOIN (SELECT uid, count(*) AS n FROM hk_e GROUP BY uid) e ON e.uid = u.id
 WHERE u.r = 7;
SELECT 1 AS still_in_the_transaction;
COMMIT;
-- the form itself, written by hand, is what the other hook refuses
SET lion_hooktest.refuse_lateral = on;
SELECT count(*) FROM hk_u u
  JOIN LATERAL (SELECT uid FROM hk_e WHERE uid = u.id GROUP BY uid) e ON true
 WHERE u.r = 7;
RESET lion_hooktest.refuse_lateral;
DROP TABLE hk_u, hk_e;
