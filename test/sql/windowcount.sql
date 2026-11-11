-- count(*) OVER () beside the rows of one table: the LionWindowCount node,
-- which takes the count from lion's count of the same table and WHERE and
-- hands up the rows of any path of the table with it (DESIGN.md §10, "A
-- count of every row beside the rows").  Each query runs with the node and
-- with pg_lion.enable_window_count off, and the two answers are compared as
-- multisets in both directions.  The table has NULLs, and dead and updated
-- rows that VACUUM has not seen, so that the count's heap checks (DESIGN.md
-- §9) are part of every answer.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

-- ts: a permutation of the ids, so that every page of rows is one answer
CREATE TABLE wc (id int, a int, b int, n int, ts int, t text)
	WITH (autovacuum_enabled = off);
INSERT INTO wc SELECT i, i % 7, i % 5,
	   CASE WHEN i % 4 = 0 THEN NULL ELSE i % 11 END,
	   (i * 7919) % 30011,
	   'v' || (i % 13)
  FROM generate_series(1, 20000) i;
CREATE INDEX wc_a ON wc USING lion (a);
CREATE INDEX wc_b ON wc USING lion (b);
CREATE INDEX wc_n ON wc USING lion (n);
CREATE INDEX wc_ts ON wc (ts);
VACUUM (FREEZE, ANALYZE) wc;
DELETE FROM wc WHERE id % 97 = 0;
UPDATE wc SET a = (a + 1) % 7, n = NULL WHERE id % 89 = 0;

/*
 * wc_q() runs q as planned and with the node off, and compares the two:
 * 'window count, N rows' when the node was in the plan and they agree.
 */
CREATE FUNCTION wc_q(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	used boolean := false;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('pg_lion.enable_window_count', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(LionWindowCount\)' THEN
			used := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE wc_on AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('pg_lion.enable_window_count', 'off', true);
	EXECUTE format('CREATE TEMP TABLE wc_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('pg_lion.enable_window_count', 'on', true);

	EXECUTE 'SELECT count(*) FROM wc_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM wc_on EXCEPT ALL SELECT * FROM wc_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM wc_off EXCEPT ALL SELECT * FROM wc_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE wc_on, wc_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN used THEN 'window count' ELSE 'no window count' END,
				  nrows);
END $$;

CREATE FUNCTION wc_plan(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		RETURN NEXT ln;
	END LOOP;
END $$;

-- the page: the count, then the rows in the btree's order up to the LIMIT
SELECT wc_plan('SELECT id, ts, count(*) OVER () FROM wc WHERE a = 3 AND b = 1 ORDER BY ts DESC LIMIT 5');
SELECT id, ts, count(*) OVER () FROM wc WHERE a = 3 AND b = 1 ORDER BY ts DESC LIMIT 5;
SET pg_lion.enable_window_count = off;
SELECT id, ts, count(*) OVER () FROM wc WHERE a = 3 AND b = 1 ORDER BY ts DESC LIMIT 5;
RESET pg_lion.enable_window_count;
-- the window functions are a count(*) in the node's target
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, count(*) OVER () AS total FROM wc WHERE a = 3 ORDER BY ts DESC LIMIT 5;

SELECT q, wc_q(q) FROM (VALUES
	('SELECT id, count(*) OVER () FROM wc WHERE a = 3 ORDER BY ts DESC LIMIT 5'),
	('SELECT id, count(*) OVER () FROM wc WHERE a = 3 AND b = 1 ORDER BY ts LIMIT 20 OFFSET 40'),
	('SELECT *, count(*) OVER () FROM wc WHERE n IS NULL ORDER BY ts DESC LIMIT 5'),
	('SELECT id, count(*) OVER () FROM wc WHERE a IN (1, 2) AND n <> 3 ORDER BY ts LIMIT 5'),
	('SELECT id, count(1) OVER (), count(*) OVER w FROM wc WHERE b = 2 WINDOW w AS () ORDER BY ts LIMIT 5'),
	('SELECT id, count(*) OVER (ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) FROM wc WHERE a = 1 ORDER BY ts LIMIT 5'),
	('SELECT id, count(*) OVER (RANGE BETWEEN CURRENT ROW AND CURRENT ROW) FROM wc WHERE a = 1 ORDER BY ts LIMIT 5'),
	('SELECT id, 100 - count(*) OVER () AS rest FROM wc WHERE a = 4 ORDER BY ts LIMIT 5'),
	('SELECT id FROM wc WHERE a = 4 ORDER BY count(*) OVER (), ts LIMIT 5'),
	('SELECT id, count(*) OVER () FROM wc WHERE a = 3'),
	('SELECT id, count(*) OVER () FROM wc ORDER BY ts LIMIT 5'),
	('SELECT id, count(*) OVER () FROM wc WHERE a = 3 AND b = 9 ORDER BY ts LIMIT 5'),
	('SELECT id, count(*) OVER () FROM wc WHERE a = 3 AND ts % 3 = 1 ORDER BY ts LIMIT 5'),
	-- not a count of every row: core's WindowAgg
	('SELECT id, count(*) OVER (ROWS UNBOUNDED PRECEDING) FROM wc WHERE a = 1 ORDER BY ts LIMIT 5'),
	('SELECT id, count(*) OVER (RANGE BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING EXCLUDE CURRENT ROW) FROM wc WHERE a = 1 ORDER BY ts LIMIT 5'),
	('SELECT id, count(*) OVER (PARTITION BY b) FROM wc WHERE a = 1 ORDER BY ts LIMIT 5'),
	('SELECT id, count(*) OVER (ORDER BY ts) FROM wc WHERE a = 1 ORDER BY ts LIMIT 5'),
	('SELECT id, count(n) OVER () FROM wc WHERE a = 1 ORDER BY ts LIMIT 5'),
	('SELECT id, count(*) FILTER (WHERE b = 1) OVER () FROM wc WHERE a = 1 ORDER BY ts LIMIT 5'),
	('SELECT id, count(*) OVER (), sum(b) OVER () FROM wc WHERE a = 1 ORDER BY ts LIMIT 5'),
	('SELECT id, count(*) OVER (), row_number() OVER () FROM wc WHERE a = 1 ORDER BY ts LIMIT 5'),
	('SELECT DISTINCT count(*) OVER () FROM wc WHERE a = 1'),
	('SELECT a, count(*) OVER () FROM wc WHERE b = 1 GROUP BY a'),
	('SELECT id, count(*) OVER () FROM wc WHERE a = 1 AND random() >= 0 ORDER BY ts LIMIT 5'),
	('SELECT w.id, count(*) OVER () FROM wc w JOIN wc v ON v.id = w.id + 1 WHERE w.a = 1 ORDER BY w.ts LIMIT 5'),
	('SELECT id, generate_series(1, 2), count(*) OVER () FROM wc WHERE a = 1 ORDER BY ts LIMIT 5')
) v(q);

-- a target finished above the windows: an expression the planner postpones
-- past the LIMIT reads the window function there, and the node is not used
CREATE FUNCTION wc_slow(int8) RETURNS int8 IMMUTABLE LANGUAGE plpgsql COST 100000
	AS 'BEGIN RETURN $1 + 1; END';
SELECT wc_q('SELECT id, wc_slow(count(*) OVER ()) FROM wc WHERE a = 3 ORDER BY ts LIMIT 5');

-- a parameter of the WHERE: a generic plan, and a rescan for each outer row
PREPARE wcp(int) AS
	SELECT id, count(*) OVER () FROM wc WHERE a = $1 ORDER BY ts DESC LIMIT 3;
SET plan_cache_mode = force_generic_plan;
SELECT wc_plan('EXECUTE wcp(2)');
EXECUTE wcp(2);
EXECUTE wcp(5);
RESET plan_cache_mode;
SET pg_lion.enable_window_count = off;
EXECUTE wcp(2);
EXECUTE wcp(5);
RESET pg_lion.enable_window_count;
DEALLOCATE wcp;
SELECT x, (SELECT count(*) OVER () FROM wc WHERE a = x ORDER BY ts LIMIT 1) AS lion,
	   (SELECT count(*) FROM wc WHERE a = x) AS truth
  FROM generate_series(0, 7) x ORDER BY x;

-- a scrollable cursor: the node reads forward only, so the cursor's plan
-- keeps its rows in a Material above it to read them back
BEGIN;
DECLARE wcc SCROLL CURSOR FOR
	SELECT id, count(*) OVER () FROM wc WHERE a = 2 AND b = 3 ORDER BY ts LIMIT 4;
FETCH 2 FROM wcc;
FETCH LAST FROM wcc;
FETCH PRIOR FROM wcc;
FETCH ABSOLUTE 1 FROM wcc;
COMMIT;

-- rescanned with no parameter of its own changed, under a correlated
-- subquery: the count is made once and the rows are read again each time
CREATE FUNCTION wc_loops(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \((LionWindowCount|LionCount)\)' THEN
			RETURN NEXT substring(ln from 'Lion[A-Za-z]+') || ': ' ||
				substring(ln from 'loops=[0-9]+');
		END IF;
	END LOOP;
END $$;
SELECT wc_loops('SELECT x, (SELECT max(s.id) + max(s.n) + x
		FROM (SELECT id, count(*) OVER () AS n FROM wc
			   WHERE a = 2 AND b = 3 ORDER BY ts LIMIT 2) s)
  FROM generate_series(1, 3) x');
SELECT x, (SELECT max(s.id) + max(s.n) + x FROM (SELECT id, count(*) OVER () AS n FROM wc
								  WHERE a = 2 AND b = 3 ORDER BY ts LIMIT 2) s)
  FROM generate_series(1, 3) x;

-- a partitioned table: the count of every partition, the rows merged by ts
CREATE TABLE wcp (id int, a int, ts int) PARTITION BY RANGE (id);
CREATE TABLE wcp1 PARTITION OF wcp FOR VALUES FROM (0) TO (10000);
CREATE TABLE wcp2 PARTITION OF wcp FOR VALUES FROM (10000) TO (20001);
INSERT INTO wcp SELECT i, i % 7, (i * 7919) % 30011 FROM generate_series(1, 20000) i;
CREATE INDEX wcp_a ON wcp USING lion (a);
CREATE INDEX wcp_ts ON wcp (ts);
VACUUM (FREEZE, ANALYZE) wcp;
DELETE FROM wcp WHERE id % 101 = 0;
SELECT wc_q('SELECT id, count(*) OVER () FROM wcp WHERE a = 3 ORDER BY ts DESC LIMIT 5');
SELECT wc_q('SELECT id, count(*) OVER () FROM wcp WHERE a = 3 AND id < 5000 ORDER BY ts LIMIT 5');

-- EXECUTE on count() and count("any") is asked as the WindowAgg asks it
CREATE ROLE lion_wc_user;
GRANT SELECT ON wc TO lion_wc_user;
REVOKE EXECUTE ON FUNCTION count("any") FROM PUBLIC;
SET ROLE lion_wc_user;
SELECT count(1) OVER () FROM wc WHERE a = 3 ORDER BY ts LIMIT 1;
SELECT count(*) OVER () FROM wc WHERE a = 3 ORDER BY ts LIMIT 1;
SET pg_lion.enable_window_count = off;
SELECT count(1) OVER () FROM wc WHERE a = 3 ORDER BY ts LIMIT 1;
RESET pg_lion.enable_window_count;
RESET ROLE;
GRANT EXECUTE ON FUNCTION count("any") TO PUBLIC;
REVOKE ALL ON wc FROM lion_wc_user;
DROP ROLE lion_wc_user;

DROP TABLE wc, wcp;
DROP FUNCTION wc_q(text), wc_plan(text), wc_loops(text), wc_slow(int8);
