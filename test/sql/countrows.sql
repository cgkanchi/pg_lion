-- The rows a LIMIT takes from one table's rows when no column of them is
-- read - a count capped by a LIMIT in a subquery - from lion's count of the
-- table and WHERE: the LionCountRows node (DESIGN.md §10, "A count capped by
-- a LIMIT").  Each query runs with the node and with
-- pg_lion.enable_count_rows off, and the two answers are compared as
-- multisets in both directions.  The table has NULLs, and dead and updated
-- rows that VACUUM has not seen, so that the count's heap checks (DESIGN.md
-- §9) are part of every answer.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE cr (id int, a int, b int, n int, t text)
	WITH (autovacuum_enabled = off);
INSERT INTO cr SELECT i, i % 7, i % 5,
	   CASE WHEN i % 4 = 0 THEN NULL ELSE i % 11 END,
	   'v' || (i % 13)
  FROM generate_series(1, 20000) i;
CREATE INDEX cr_a ON cr USING lion (a);
CREATE INDEX cr_b ON cr USING lion (b);
CREATE INDEX cr_n ON cr USING lion (n);
VACUUM (FREEZE, ANALYZE) cr;
DELETE FROM cr WHERE id % 97 = 0;
UPDATE cr SET a = (a + 1) % 7, n = NULL WHERE id % 89 = 0;

/*
 * cr_q() runs q as planned and with the node off, and compares the two:
 * 'count rows, N rows' when the node was in the plan and they agree.
 */
CREATE FUNCTION cr_q(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	used boolean := false;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('pg_lion.enable_count_rows', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(LionCountRows\)' THEN
			used := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE cr_on AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('pg_lion.enable_count_rows', 'off', true);
	EXECUTE format('CREATE TEMP TABLE cr_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('pg_lion.enable_count_rows', 'on', true);

	EXECUTE 'SELECT count(*) FROM cr_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM cr_on EXCEPT ALL SELECT * FROM cr_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM cr_off EXCEPT ALL SELECT * FROM cr_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE cr_on, cr_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN used THEN 'count rows' ELSE 'no count rows' END,
				  nrows);
END $$;

CREATE FUNCTION cr_plan(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		RETURN NEXT ln;
	END LOOP;
END $$;

-- the capped count: the subquery's rows are the count's, up to the LIMIT
SELECT cr_plan('SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = 3 AND b = 1 LIMIT 300) s');
SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = 3 AND b = 1 LIMIT 300) s;
SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = 3 AND b = 1 LIMIT 100000) s;
SELECT count(*) FROM cr WHERE a = 3 AND b = 1;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = 3 LIMIT 1000 OFFSET 10) s;

SELECT q, cr_q(q) FROM (VALUES
	('SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = 3 LIMIT 1000) s'),
	('SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = 3 AND b = 1 LIMIT 300) s'),
	('SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = 3 LIMIT 1000 OFFSET 10) s'),
	('SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = 3 LIMIT 100000 OFFSET 2800) s'),
	('SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = 3 OFFSET 2000) s'),
	('SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = 3 OFFSET 99999) s'),
	('SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = 3 LIMIT NULL) s'),
	('SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = 3 LIMIT 1000 OFFSET NULL) s'),
	('SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = 3 LIMIT 0) s'),
	('SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = 3 FETCH FIRST 500 ROWS ONLY) s'),
	('SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = 3 LIMIT (SELECT 500) OFFSET (SELECT 7)) s'),
	('SELECT count(*) FROM (SELECT * FROM cr WHERE a = 3 AND n IS NULL LIMIT 1000) s'),
	('SELECT count(*) FROM (SELECT FROM cr WHERE a IN (1, 2) AND n <> 3 LIMIT 1000) s'),
	('SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = 3 AND b = 9 LIMIT 1000) s'),
	('SELECT count(*) FROM (SELECT 1 FROM cr LIMIT 5000) s'),
	('SELECT count(*), max(s.x) FROM (SELECT 7 AS x FROM cr WHERE a = 3 LIMIT 1000) s'),
	('SELECT s.x, s.y FROM (SELECT ''k'' AS x, NULL::int AS y FROM cr WHERE a = 3 AND b = 2 LIMIT 1000) s'),
	-- a clause no posting set answers, tested on the heap, that raises no
	-- error ...
	('SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = 3 AND t = ''v3'' LIMIT 1000) s'),
	-- ... and one that might (int4mod): core's LIMIT
	('SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = 3 AND id % 3 = 1 LIMIT 1000) s'),
	-- a window nothing reads is gone from the subquery before it is planned
	('SELECT count(*) FROM (SELECT 1, count(*) OVER () FROM cr WHERE a = 3 LIMIT 1000) s'),
	-- the rows' columns are read, or the rows chosen: core's LIMIT
	('SELECT sum(id) FROM (SELECT id FROM cr WHERE a = 3 LIMIT 1000) s'),
	('SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = 3 ORDER BY id LIMIT 1000) s'),
	('SELECT count(*) FROM (SELECT DISTINCT b FROM cr WHERE a = 3 LIMIT 1000) s'),
	('SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = 3 GROUP BY b LIMIT 1000) s'),
	('SELECT count(*) FROM (SELECT random() FROM cr WHERE a = 3 LIMIT 1000) s'),
	('SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = 3 AND random() >= 0 LIMIT 1000) s'),
	('SELECT count(*) FROM (SELECT generate_series(1, 2) FROM cr WHERE a = 3 LIMIT 1000) s'),
	('SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = 3 UNION ALL SELECT 1 FROM cr WHERE b = 1 LIMIT 1000) s'),
	('SELECT count(*) FROM (SELECT 1 FROM cr c JOIN cr d ON d.id = c.id + 1 WHERE c.a = 3 LIMIT 1000) s'),
	('SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = 3 LIMIT 1000 FOR UPDATE) s')
) v(q);

-- a clause tested on the heap that raises an error on a row past the ones
-- the LIMIT reads - id 19995, of a = 3, divides by zero - is left to core's
-- LIMIT, which stops at the 1000th row and never reaches it
SELECT cr_plan('SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = 3 AND 100 / (id - 19995) = 0 LIMIT 1000) s');
SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = 3 AND 100 / (id - 19995) = 0 LIMIT 1000) s;
SELECT a FROM cr WHERE id = 19995;

-- LIMIT and OFFSET checked as a Limit checks them, before anything is read
SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = 3 LIMIT -1) s;
SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = 3 LIMIT 5 OFFSET -1) s;
SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = 3 LIMIT -1 OFFSET -1) s;
-- LIMIT 0 counts nothing
CREATE FUNCTION cr_loops(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \((LionCountRows|LionCount)\)' THEN
			RETURN NEXT substring(ln from 'Lion[A-Za-z]+') || ': ' ||
				coalesce(substring(ln from 'loops=[0-9]+'), 'never executed');
		END IF;
	END LOOP;
END $$;
PREPARE crl(int8, int8) AS
	SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = 3 LIMIT $1 OFFSET $2) s;
SET plan_cache_mode = force_generic_plan;
SELECT cr_loops('EXECUTE crl(0, 0)');
SELECT cr_loops('EXECUTE crl(400, 0)');
EXECUTE crl(0, 0);
EXECUTE crl(400, 0);
EXECUTE crl(400, 2700);
EXECUTE crl(NULL, 10);
EXECUTE crl(400, NULL);
EXECUTE crl(-1, 0);
RESET plan_cache_mode;
DEALLOCATE crl;

-- a parameter of the WHERE: a generic plan, and a rescan for each outer row
PREPARE crp(int) AS
	SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = $1 AND b = 1 LIMIT 300) s;
SET plan_cache_mode = force_generic_plan;
SELECT cr_plan('EXECUTE crp(2)');
EXECUTE crp(2);
EXECUTE crp(5);
RESET plan_cache_mode;
DEALLOCATE crp;
SELECT cr_loops('SELECT x, (SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = x LIMIT 2000) s)
  FROM generate_series(0, 7) x');
SELECT x, (SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = x LIMIT 2000) s) AS lion,
	   least((SELECT count(*) FROM cr WHERE a = x), 2000) AS truth
  FROM generate_series(0, 7) x ORDER BY x;

-- a LIMIT that changes with each outer row: the count is made once
SELECT cr_loops('SELECT x, (SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = 3 LIMIT x * 1000) s)
  FROM generate_series(1, 4) x');
SELECT x, (SELECT count(*) FROM (SELECT 1 FROM cr WHERE a = 3 LIMIT x * 1000) s) AS lion,
	   least((SELECT count(*) FROM cr WHERE a = 3), x * 1000) AS truth
  FROM generate_series(1, 4) x ORDER BY x;

-- a scrollable cursor over the rows: the node reads forward only, so the
-- cursor's plan keeps them in a Material above it to read them back
SELECT cr_plan('DECLARE crx SCROLL CURSOR FOR SELECT ''r'' FROM cr WHERE a = 2 AND b = 3 LIMIT 1000 OFFSET 560');
BEGIN;
DECLARE crc SCROLL CURSOR FOR
	SELECT 'r' FROM cr WHERE a = 2 AND b = 3 LIMIT 1000 OFFSET 560;
FETCH ALL FROM crc;
FETCH PRIOR FROM crc;
FETCH ABSOLUTE 1 FROM crc;
COMMIT;
SELECT count(*) FROM cr WHERE a = 2 AND b = 3;

-- a partitioned table: the count of every partition
CREATE TABLE crpt (id int, a int) PARTITION BY RANGE (id);
CREATE TABLE crpt1 PARTITION OF crpt FOR VALUES FROM (0) TO (10000);
CREATE TABLE crpt2 PARTITION OF crpt FOR VALUES FROM (10000) TO (20001);
INSERT INTO crpt SELECT i, i % 7 FROM generate_series(1, 20000) i;
CREATE INDEX crpt_a ON crpt USING lion (a);
VACUUM (FREEZE, ANALYZE) crpt;
DELETE FROM crpt WHERE id % 101 = 0;
SELECT cr_q('SELECT count(*) FROM (SELECT 1 FROM crpt WHERE a = 3 LIMIT 2000) s');
SELECT cr_q('SELECT count(*) FROM (SELECT 1 FROM crpt WHERE a IN (2, 3) LIMIT 5000) s');

DROP TABLE cr, crpt;
DROP FUNCTION cr_q(text), cr_plan(text), cr_loops(text);
