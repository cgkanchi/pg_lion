-- A GROUP BY of expressions of columns it does not name - GROUP BY upper(t),
-- GROUP BY g / 3 - in the LionCount pushdown (DESIGN.md §36, "Expressions of
-- columns not grouped").
--
-- The node walks the columns the expressions read, emits each group as a
-- partial row with the expressions computed from its key, and core's Finalize
-- Agg adds up the rows that share a value.  Every answer is compared with a
-- sequential scan with the pushdown off, as a multiset in both directions.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
/*
 * lion_ge() runs a query with every other scan disabled, so that a LionCount
 * path that is built at all is the plan, and again as a sequential scan with
 * the pushdown off, and compares the two.  LionOrdered (DESIGN.md §30) is
 * switched off with the rest: the core switches do not reach it, and over the
 * btree on id it offers core a presorted aggregate for count(DISTINCT id).
 */
CREATE FUNCTION lion_ge(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	pushed boolean := false;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	PERFORM set_config('pg_lion.enable_ordered_scan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			pushed := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lion_ge_on AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	EXECUTE format('CREATE TEMP TABLE lion_ge_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
	PERFORM set_config('pg_lion.enable_ordered_scan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE 'SELECT count(*) FROM lion_ge_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_ge_on EXCEPT ALL SELECT * FROM lion_ge_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_ge_off EXCEPT ALL SELECT * FROM lion_ge_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lion_ge_on, lion_ge_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN pushed THEN 'pushed down' ELSE 'not pushed down' END,
				  nrows);
END $$;
/* The same under a generic plan, which keeps $n a Param. */
CREATE FUNCTION lion_ge_prep(q text, args text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	pushed boolean := false;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('plan_cache_mode', 'force_generic_plan', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	PERFORM set_config('pg_lion.enable_ordered_scan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE 'PREPARE lion_gep_on AS ' || q;
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) EXECUTE lion_gep_on(' || args || ')' LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			pushed := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lion_ge_on AS EXECUTE lion_gep_on(%s)', args);
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE 'PREPARE lion_gep_off AS ' || q;
	EXECUTE format('CREATE TEMP TABLE lion_ge_off AS EXECUTE lion_gep_off(%s)', args);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
	PERFORM set_config('pg_lion.enable_ordered_scan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE 'DEALLOCATE lion_gep_on';
	EXECUTE 'DEALLOCATE lion_gep_off';
	EXECUTE 'SELECT count(*) FROM lion_ge_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_ge_on EXCEPT ALL SELECT * FROM lion_ge_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_ge_off EXCEPT ALL SELECT * FROM lion_ge_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lion_ge_on, lion_ge_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN pushed THEN 'pushed down' ELSE 'not pushed down' END,
				  nrows);
END $$;
/*
 * 3000 rows.  g: 0 .. 7, NULL on every 9th row.  h: 1 .. 5, NULL on every 6th
 * row.  a: 10 values.  t: 'x0' .. 'x4' and 'X0', NULL on every 13th row.
 * n: numeric 1.0 and 1.00, which are equal and print differently.  z: 0 .. 3,
 * 0 only in rows deleted below.
 */
CREATE TABLE lion_ge (
	id	int		NOT NULL,
	g	int,
	h	int,
	a	int		NOT NULL,
	t	text,
	n	numeric,
	z	int		NOT NULL
);
INSERT INTO lion_ge
SELECT i,
	   CASE WHEN i % 9 = 0 THEN NULL ELSE (i / 2) % 8 END,
	   CASE WHEN i % 6 = 0 THEN NULL ELSE 1 + (i / 5) % 5 END,
	   i % 10,
	   CASE WHEN i % 13 = 0 THEN NULL WHEN i % 17 = 0 THEN 'X0'
			ELSE 'x' || ((i / 7) % 5) END,
	   CASE WHEN i % 2 = 0 THEN 1.0 ELSE 1.00 END,
	   CASE WHEN i % 100 = 0 THEN 0 ELSE 1 + i % 3 END
  FROM generate_series(1, 3000) i;
CREATE INDEX lion_ge_g ON lion_ge USING lion (g);
CREATE INDEX lion_ge_h ON lion_ge USING lion (h);
CREATE INDEX lion_ge_a ON lion_ge USING lion (a);
CREATE INDEX lion_ge_t ON lion_ge USING lion (t);
CREATE INDEX lion_ge_n ON lion_ge USING lion (n);
CREATE INDEX lion_ge_z ON lion_ge USING lion (z);
CREATE INDEX lion_ge_tz ON lion_ge USING lion ((t || 'z'));
CREATE UNIQUE INDEX lion_ge_id ON lion_ge (id);
-- z = 0 is in deleted rows only
DELETE FROM lion_ge WHERE z = 0;
VACUUM (FREEZE, ANALYZE) lion_ge;
-- ---- 1. one column, one expression of it ---------------------------------
SELECT upper(t) AS k, count(*) FROM lion_ge GROUP BY 1 ORDER BY 1;
SELECT lion_ge('SELECT upper(t), count(*) FROM lion_ge GROUP BY 1');
SELECT lion_ge('SELECT lower(t), count(*) FROM lion_ge GROUP BY 1');
SELECT lion_ge('SELECT length(t), count(*) FROM lion_ge GROUP BY 1');
SELECT lion_ge('SELECT t || ''!'', count(*) FROM lion_ge GROUP BY 1');
SELECT g / 3 AS k, count(*) FROM lion_ge GROUP BY 1 ORDER BY 1;
SELECT lion_ge('SELECT g / 3, count(*) FROM lion_ge GROUP BY 1');
SELECT lion_ge('SELECT g % 2, count(*) FROM lion_ge GROUP BY 1');
SELECT lion_ge('SELECT g > 3, count(*) FROM lion_ge GROUP BY 1');
SELECT lion_ge('SELECT abs(g - 4), count(*) FROM lion_ge GROUP BY 1');
-- the expression only in GROUP BY, and two of the same column
SELECT lion_ge('SELECT count(*) FROM lion_ge GROUP BY upper(t)');
SELECT lion_ge('SELECT upper(t), length(t), count(*) FROM lion_ge GROUP BY 1, 2');
SELECT lion_ge('SELECT g / 3, g % 3, count(*) FROM lion_ge GROUP BY 1, 2');
-- of an expression column (DESIGN.md §41)
SELECT lion_ge('SELECT upper(t || ''z''), count(*) FROM lion_ge GROUP BY 1');
-- ---- 2. a WHERE: another column, the same column, an OR -------------------
SELECT lion_ge('SELECT upper(t), count(*) FROM lion_ge WHERE a = 3 GROUP BY 1');
SELECT lion_ge('SELECT upper(t), count(*) FROM lion_ge WHERE a IN (1, 2, 7) GROUP BY 1');
SELECT lion_ge('SELECT g / 3, count(*) FROM lion_ge WHERE a = 3 AND h = 2 GROUP BY 1');
SELECT lion_ge('SELECT g / 3, count(*) FROM lion_ge WHERE g > 2 GROUP BY 1');
SELECT lion_ge('SELECT g / 3, count(*) FROM lion_ge WHERE g IN (1, 4, 5) GROUP BY 1');
SELECT lion_ge('SELECT g / 3, count(*) FROM lion_ge WHERE g IS NOT NULL GROUP BY 1');
SELECT lion_ge('SELECT upper(t), count(*) FROM lion_ge WHERE t <> ''x1'' GROUP BY 1');
SELECT lion_ge('SELECT upper(t), count(*) FROM lion_ge WHERE a = 3 OR h = 2 GROUP BY 1');
SELECT lion_ge('SELECT upper(t), count(*) FROM lion_ge WHERE a = 99 GROUP BY 1');
-- ---- 3. what else the target list and HAVING may say ---------------------
SELECT lion_ge('SELECT upper(t), count(*), count(t) FROM lion_ge GROUP BY 1');
SELECT lion_ge('SELECT g / 3, count(g), count(*) FROM lion_ge GROUP BY 1');
SELECT lion_ge('SELECT upper(t), count(*) * 2 + 1 FROM lion_ge GROUP BY 1');
SELECT lion_ge('SELECT upper(t) FROM lion_ge GROUP BY 1 HAVING count(*) > 500');
SELECT lion_ge('SELECT upper(t), count(*) FROM lion_ge GROUP BY 1 HAVING count(*) < 500');
SELECT lion_ge('SELECT upper(t), count(*) FROM lion_ge GROUP BY 1 HAVING upper(t) > ''X1''');
SELECT lion_ge('SELECT upper(t), count(*) FROM lion_ge GROUP BY 1 ORDER BY count(*) DESC LIMIT 2');
-- ---- 4. two columns -------------------------------------------------------
SELECT lion_ge('SELECT g / 2, upper(t), count(*) FROM lion_ge GROUP BY 1, 2');
SELECT lion_ge('SELECT g + h, count(*) FROM lion_ge GROUP BY 1');
SELECT lion_ge('SELECT a, g / 3, count(*) FROM lion_ge GROUP BY 1, 2');
SELECT lion_ge('SELECT a, g / 3, count(*) FROM lion_ge WHERE h = 2 GROUP BY 1, 2');
SELECT lion_ge('SELECT g, h / 2, count(*) FROM lion_ge GROUP BY 1, 2');
-- ---- 5. errors: the expression is computed from the rows' values ----------
-- z = 0 is only in deleted rows, whose key the index may still hold
SELECT lion_ge('SELECT 12 / z, count(*) FROM lion_ge GROUP BY 1');
-- 12 / (h - 3) fails in the rows of h = 3, which the WHERE takes out
SELECT lion_ge('SELECT 12 / (h - 3), count(*) FROM lion_ge WHERE h <> 3 GROUP BY 1');
SELECT lion_ge('SELECT 12 / (h - 3), count(*) FROM lion_ge WHERE h IN (1, 5) GROUP BY 1');
-- ... and is raised where the rows are counted
SELECT 12 / (h - 3), count(*) FROM lion_ge GROUP BY 1;
-- ---- 6. declined --------------------------------------------------------
-- numeric: 1.0 and 1.00 are one entry, and print differently
SELECT lion_ge('SELECT n::text, count(*) FROM lion_ge GROUP BY 1');
-- not immutable
SELECT lion_ge('SELECT g + (random() * 0)::int, count(*) FROM lion_ge GROUP BY 1');
SELECT lion_ge('SELECT t || now()::date::text, count(*) FROM lion_ge GROUP BY 1');
-- three columns, a column with no lion index, a distinct count beside
SELECT lion_ge('SELECT g / 2, h / 2, a / 2, count(*) FROM lion_ge GROUP BY 1, 2, 3');
SELECT lion_ge('SELECT id / 1000, count(*) FROM lion_ge GROUP BY 1');
SELECT lion_ge('SELECT upper(t), count(DISTINCT h) FROM lion_ge GROUP BY 1');
-- a count of a column the GROUP BY does not read, as for a plain GROUP BY
SELECT lion_ge('SELECT upper(t), count(h) FROM lion_ge GROUP BY 1');
-- a SELECT DISTINCT
SELECT lion_ge('SELECT DISTINCT upper(t) FROM lion_ge');
-- ---- 7. a dirty heap: deleted and updated rows, not vacuumed --------------
DELETE FROM lion_ge WHERE id % 11 = 0;
UPDATE lion_ge SET t = 'x9' WHERE id % 23 = 0;
UPDATE lion_ge SET g = g + 1 WHERE id % 29 = 0;
SELECT lion_ge('SELECT upper(t), count(*) FROM lion_ge GROUP BY 1');
SELECT lion_ge('SELECT g / 3, count(*) FROM lion_ge WHERE a = 4 GROUP BY 1');
SELECT lion_ge('SELECT 12 / z, count(*) FROM lion_ge GROUP BY 1');
-- ---- 8. parameters, and a rescan per outer row ---------------------------
SELECT lion_ge_prep('SELECT upper(t), count(*) FROM lion_ge WHERE a = $1 GROUP BY 1', '3');
SELECT lion_ge_prep('SELECT g / 3, count(*) FROM lion_ge WHERE a = ANY ($1) GROUP BY 1', '''{2,4}''::int[]');
SELECT lion_ge('SELECT x, s.* FROM generate_series(0, 3) x, LATERAL (SELECT upper(t) AS k, count(*) FROM lion_ge WHERE a = x GROUP BY 1) s');
-- ---- 9. a partitioned table ---------------------------------------------
CREATE TABLE lion_gep (id int NOT NULL, g int, t text) PARTITION BY RANGE (id);
CREATE TABLE lion_gep1 PARTITION OF lion_gep FOR VALUES FROM (0) TO (1500);
CREATE TABLE lion_gep2 PARTITION OF lion_gep FOR VALUES FROM (1500) TO (4000);
INSERT INTO lion_gep SELECT id, g, t FROM lion_ge;
CREATE INDEX ON lion_gep USING lion (g);
CREATE INDEX ON lion_gep USING lion (t);
VACUUM (FREEZE, ANALYZE) lion_gep;
SELECT lion_ge('SELECT upper(t), count(*) FROM lion_gep GROUP BY 1');
SELECT lion_ge('SELECT g / 3, count(*) FROM lion_gep WHERE t = ''x2'' GROUP BY 1');
-- ---- 10. EXPLAIN ---------------------------------------------------------
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET pg_lion.enable_plain_scan = off;
SET pg_lion.enable_ordered_scan = off;
EXPLAIN (COSTS OFF) SELECT upper(t), count(*) FROM lion_ge GROUP BY 1;
EXPLAIN (COSTS OFF) SELECT g / 3, count(*) FROM lion_ge WHERE a = 3 GROUP BY 1 HAVING count(*) > 10;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET pg_lion.enable_plain_scan;
RESET pg_lion.enable_ordered_scan;
DROP TABLE lion_ge, lion_gep;
DROP FUNCTION lion_ge(text), lion_ge_prep(text, text);
