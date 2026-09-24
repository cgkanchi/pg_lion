-- FK-side join pushdown (DESIGN.md §27).
--
-- `SELECT d.attr, count(*) FROM fact f JOIN dim d ON f.fk = d.pk ... GROUP BY
-- d.attr`: the LionCount node runs the dimension side as a child plan, counts
-- each dimension row's fk posting set ANDed with the fact filters, and emits
-- partial counts that core's Finalize Agg groups by the dimension columns.
-- Every answer is checked against the same query with the pushdown off, as a
-- multiset in both directions.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET default_statistics_target = 1000;

/*
 * lion_fj() runs a query through the pushdown - with every join method
 * disabled when force is set, so that a shape the cost model would not pick
 * is still EXERCISED (the node joins nothing; the ordinary plan does) - and
 * again with the pushdown off and the planner left alone, and compares the
 * two.  It reports whether the node was used.
 */
CREATE FUNCTION lion_fj(q text, force boolean DEFAULT true) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	pushed boolean := false;
	nrows bigint;
	ndiff bigint;
BEGIN
	IF force THEN
		PERFORM set_config('enable_hashjoin', 'off', true);
		PERFORM set_config('enable_mergejoin', 'off', true);
		PERFORM set_config('enable_nestloop', 'off', true);
	END IF;
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			pushed := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lion_fj_on AS SELECT s::text AS r FROM (%s) s', q);

	/* the reference: the ordinary join, with the pushdown off */
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_nestloop', 'on', true);
	EXECUTE format('CREATE TEMP TABLE lion_fj_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM lion_fj_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_fj_on EXCEPT ALL SELECT * FROM lion_fj_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_fj_off EXCEPT ALL SELECT * FROM lion_fj_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lion_fj_on, lion_fj_off';

	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN pushed THEN 'pushed down' ELSE 'not pushed down' END,
				  nrows);
END $$;

/* Which plan the cost model picks, with nothing disabled. */
CREATE FUNCTION lion_fj_pick(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			RETURN 'pushed down';
		END IF;
	END LOOP;
	RETURN 'not pushed down';
END $$;

/* A generic prepared plan, which keeps $n a Param, against the same with the pushdown off. */
CREATE FUNCTION lion_fj_prep(q text, args text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	pushed boolean := false;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('plan_cache_mode', 'force_generic_plan', true);
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE 'PREPARE lion_fjp_on AS ' || q;
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) EXECUTE lion_fjp_on(' || args || ')' LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			pushed := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lion_fj_on AS EXECUTE lion_fjp_on(%s)', args);

	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_nestloop', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE 'PREPARE lion_fjp_off AS ' || q;
	EXECUTE format('CREATE TEMP TABLE lion_fj_off AS EXECUTE lion_fjp_off(%s)', args);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE 'DEALLOCATE lion_fjp_on';
	EXECUTE 'DEALLOCATE lion_fjp_off';

	EXECUTE 'SELECT count(*) FROM lion_fj_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_fj_on EXCEPT ALL SELECT * FROM lion_fj_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_fj_off EXCEPT ALL SELECT * FROM lion_fj_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lion_fj_on, lion_fj_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN pushed THEN 'pushed down' ELSE 'not pushed down' END,
				  nrows);
END $$;

/* One counter of the node's EXPLAIN ANALYZE output, joins disabled. */
CREATE FUNCTION lion_fj_counter(q text, counter text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	val bigint;
BEGIN
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF btrim(split_part(ln, ':', 1)) = counter THEN
			val := btrim(split_part(ln, ':', 2))::bigint;
		END IF;
	END LOOP;
	RETURN val;
END $$;

-- EXPLAIN in a form every supported release prints the same way (the
-- definition test/sql/citext.sql uses).
CREATE OR REPLACE FUNCTION lion_explain_norm(q text, opts text DEFAULT 'COSTS OFF')
RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	l text;
BEGIN
	PERFORM set_config('enable_sort', 'off', true);
	FOR l IN EXECUTE 'EXPLAIN (' || opts || ') ' || q LOOP
		CONTINUE WHEN l ~ '^\s*Disabled: true$';
		l := regexp_replace(l, '(InitPlan|SubPlan) (\d+)', '\1 expr_\2', 'g');
		RETURN NEXT regexp_replace(l, 'rows=(\d+)\.00 ', 'rows=\1 ', 'g');
	END LOOP;
	PERFORM set_config('enable_sort', 'on', true);
END
$$;

/*
 * The dimension: 100 rows, int8 primary key 1..100, attr 0..6 with NULL on
 * every 13th, a text name and a region.  The fact: 30000 rows whose int4 fk
 * runs over 1..120 - so keys 101..120 join nothing - with NULL on every 50th
 * row; x has 10 values, y 5 values and NULLs, tk the fk as text.
 */
CREATE TABLE lion_fd (
	pk		int8	PRIMARY KEY,
	attr	int,
	name	text	NOT NULL,
	region	text	NOT NULL,
	tpk		text	UNIQUE
);
INSERT INTO lion_fd
SELECT i, CASE WHEN i % 13 = 0 THEN NULL ELSE i % 7 END,
	   'n' || (i % 11), CASE WHEN i % 3 = 0 THEN 'eu' ELSE 'us' END, 'k' || i
FROM generate_series(1, 100) i;

CREATE TABLE lion_ff (
	id	int		NOT NULL,
	fk	int4,
	x	int		NOT NULL,
	y	int,
	tk	text,
	pad	text
);
INSERT INTO lion_ff
SELECT i, CASE WHEN i % 50 = 0 THEN NULL ELSE (i * 7) % 120 + 1 END,
	   i % 10, CASE WHEN i % 9 = 0 THEN NULL ELSE i % 5 END,
	   CASE WHEN i % 50 = 0 THEN NULL ELSE 'k' || ((i * 7) % 120 + 1) END,
	   repeat('p', 40)
FROM generate_series(1, 30000) i;
CREATE INDEX lion_ff_fk ON lion_ff USING lion (fk);
CREATE INDEX lion_ff_x ON lion_ff USING lion (x);
CREATE INDEX lion_ff_y ON lion_ff USING lion (y);
CREATE INDEX lion_ff_tk ON lion_ff USING lion (tk);
VACUUM ANALYZE lion_ff;
VACUUM ANALYZE lion_fd;

-- ---- 1. the shapes, on an all-visible heap ---------------------------------
SELECT lion_fj('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk GROUP BY d.attr');
SELECT lion_fj('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON d.pk = f.fk WHERE f.x = 3 GROUP BY d.attr');
SELECT lion_fj('SELECT d.attr, count(*) FROM lion_ff f, lion_fd d WHERE f.fk = d.pk AND f.x IN (1, 2, 3) AND d.region = ''eu'' GROUP BY d.attr');
SELECT lion_fj('SELECT count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk WHERE d.attr = 2');
SELECT lion_fj('SELECT count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk WHERE d.attr = 2 AND f.x = 4');
SELECT lion_fj('SELECT count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk');
-- a dimension filter nothing passes: the plain count is 0, the grouped one has no row
SELECT lion_fj('SELECT count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk WHERE d.region = ''mars''');
SELECT lion_fj('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk WHERE d.region = ''mars'' GROUP BY d.attr');
-- a fact filter with no entry at all
SELECT lion_fj('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk WHERE f.x = 77 GROUP BY d.attr');
-- two dimension columns, an expression, the key itself
SELECT lion_fj('SELECT d.attr, d.region, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk GROUP BY d.attr, d.region');
SELECT lion_fj('SELECT upper(d.name) AS n, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk GROUP BY upper(d.name)');
SELECT lion_fj('SELECT d.pk, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk WHERE f.y = 1 GROUP BY d.pk');
SELECT lion_fj('SELECT d.attr * 10 + 1 AS a, count(*) AS c FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk GROUP BY d.attr');
-- count(1) and count of either join column are count(*)
SELECT lion_fj('SELECT d.attr, count(1), count(f.fk), count(d.pk), count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk GROUP BY d.attr');
-- fact filters of every kind
SELECT lion_fj('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk WHERE f.y IS NULL GROUP BY d.attr');
SELECT lion_fj('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk WHERE f.y IS NOT NULL AND f.x = 2 GROUP BY d.attr');
SELECT lion_fj('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk WHERE f.x = 1 OR f.y = 2 GROUP BY d.attr');
SELECT lion_fj('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk WHERE f.fk IN (3, 4, 5, 110) GROUP BY d.attr');
-- HAVING, ORDER BY, LIMIT above it
SELECT lion_fj('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk GROUP BY d.attr HAVING count(*) > 3300');
SELECT lion_fj('SELECT d.region FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk GROUP BY d.region HAVING count(*) > 10000 AND d.region <> ''xx''');
SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk GROUP BY d.attr ORDER BY d.attr NULLS FIRST;
SELECT lion_fj('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk GROUP BY d.attr ORDER BY count(*) DESC, d.attr LIMIT 3');
-- text keys: the fact's text column against the dimension's unique text column
SELECT lion_fj('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON f.tk = d.tpk GROUP BY d.attr');
SELECT lion_fj('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON f.tk = d.tpk WHERE f.x = 5 GROUP BY d.attr');

-- ---- 2. the cost model's choice, with nothing disabled -----------------------
SELECT lion_fj_pick('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk GROUP BY d.attr');
SELECT lion_fj_pick('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk WHERE f.x = 3 GROUP BY d.attr');
SELECT lion_fj_pick('SELECT count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk WHERE d.attr = 2');
-- a dimension far larger than the fact's key set: a lookup per dimension row
-- loses to the hash join, and the model says so
CREATE TABLE lion_fdbig (k int8 PRIMARY KEY, attr int);
INSERT INTO lion_fdbig SELECT i, i % 5 FROM generate_series(1, 50000) i;
VACUUM ANALYZE lion_fdbig;
SELECT lion_fj_pick('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fdbig d ON f.fk = d.k GROUP BY d.attr');
-- ... unless its own quals leave few rows
SELECT lion_fj_pick('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fdbig d ON f.fk = d.k WHERE d.k < 60 GROUP BY d.attr');
SELECT lion_fj('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fdbig d ON f.fk = d.k WHERE d.k < 60 GROUP BY d.attr', false);
DROP TABLE lion_fdbig;
-- a long IN list among the fact filters: every count rebuilds the list's
-- union, so the node is refused with nothing disabled (the 2026-09-23 review:
-- priced as one set, 1000 values over 300 dimension rows were chosen at 1.2 s
-- against 2 ms); forced, it still answers exactly
SELECT * FROM lion_explain_norm('SELECT count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk WHERE f.tk IN (' ||
	(SELECT string_agg(quote_literal('k' || i), ', ') FROM generate_series(1, 60) i) || ')') AS p("QUERY PLAN");
SELECT lion_fj('SELECT count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk WHERE f.tk IN (' ||
	(SELECT string_agg(quote_literal('k' || i), ', ') FROM generate_series(1, 60) i) || ')');

-- ---- 3. EXPLAIN ---------------------------------------------------------------
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
SELECT * FROM lion_explain_norm('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk WHERE f.x = 3 AND d.region = ''eu'' GROUP BY d.attr') AS p("QUERY PLAN");
SELECT * FROM lion_explain_norm('SELECT count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk WHERE d.attr = 2') AS p("QUERY PLAN");
SELECT * FROM lion_explain_norm('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk GROUP BY d.attr HAVING count(*) > 5', 'COSTS OFF, VERBOSE') AS p("QUERY PLAN");
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_nestloop;
-- keys 1..100 are looked up; the dimension has all of them, and 101..120 are
-- never asked for; only the dimension's own quals decide which rows are
SELECT lion_fj_counter('SELECT count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk', 'Join Keys Looked Up');
SELECT lion_fj_counter('SELECT count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk WHERE d.region = ''eu''', 'Join Keys Looked Up');
SELECT lion_fj_counter('SELECT count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk', 'Heap Blocks Rechecked') AS clean_heap_rechecks;

-- ---- 4. cross-type keys, NULL keys on both sides ------------------------------
-- an int8 fact column against an int4 unique dimension key, and a dimension
-- whose unique key has NULLs (they join nothing)
CREATE TABLE lion_fd4 (k int4 UNIQUE, grp text);
INSERT INTO lion_fd4 SELECT CASE WHEN i % 10 = 0 THEN NULL ELSE i END, 'g' || (i % 4)
FROM generate_series(1, 60) i;
CREATE TABLE lion_ff8 (fk8 int8, x int NOT NULL);
INSERT INTO lion_ff8 SELECT CASE WHEN i % 17 = 0 THEN NULL ELSE i % 70 END, i % 3
FROM generate_series(1, 7000) i;
CREATE INDEX ON lion_ff8 USING lion (fk8);
CREATE INDEX ON lion_ff8 USING lion (x);
VACUUM ANALYZE lion_fd4;
VACUUM ANALYZE lion_ff8;
SELECT lion_fj('SELECT d.grp, count(*) FROM lion_ff8 f JOIN lion_fd4 d ON f.fk8 = d.k GROUP BY d.grp');
SELECT lion_fj('SELECT d.grp, count(*) FROM lion_ff8 f JOIN lion_fd4 d ON f.fk8 = d.k WHERE f.x = 1 GROUP BY d.grp');
SELECT lion_fj('SELECT count(*) FROM lion_ff8 f JOIN lion_fd4 d ON d.k = f.fk8 WHERE d.grp = ''g2''');
-- 60 dimension rows, 6 of them with a NULL key: 54 lookups, all of them found
SELECT lion_fj_counter('SELECT count(*) FROM lion_ff8 f JOIN lion_fd4 d ON f.fk8 = d.k', 'Join Keys Looked Up');
SELECT lion_fj_counter('SELECT count(*) FROM lion_ff8 f JOIN lion_fd4 d ON f.fk8 = d.k', 'Join Keys Without Entry');

-- ---- 5. parameters and rescans ------------------------------------------------
SELECT lion_fj_prep('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk WHERE f.x = $1 GROUP BY d.attr', '3');
SELECT lion_fj_prep('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk WHERE f.x = $1 AND d.region = $2 GROUP BY d.attr', '4, ''eu''');
SELECT lion_fj_prep('SELECT count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk WHERE f.x = $1', 'NULL');
-- `IN ($1, $2)` has its length at plan time; `= ANY ($1)` does not, and every
-- count would rebuild a union of however many values it brings: refused
SELECT lion_fj_prep('SELECT count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk WHERE f.x IN ($1, $2)', '1, 2');
SELECT lion_fj_prep('SELECT count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk WHERE f.x = ANY ($1)', '''{1,2}''');
-- a correlated subquery: the dimension filter is an exec Param, so the node
-- (and its child) is rescanned with a new value for every outer row
SELECT lion_fj('SELECT g, (SELECT count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk WHERE d.attr = g.g AND f.x = 2) FROM generate_series(0, 7) g');

-- ---- 6. declined --------------------------------------------------------------
-- a non-unique dimension key
CREATE TABLE lion_fdn (k int8, attr int);
INSERT INTO lion_fdn SELECT i % 50, i % 3 FROM generate_series(1, 100) i;
CREATE INDEX ON lion_fdn (k);
ANALYZE lion_fdn;
SELECT lion_fj('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fdn d ON f.fk = d.k GROUP BY d.attr');
-- a second join clause, an outer join, a fact column in the output, a
-- volatile grouping expression, another aggregate, a fact column counted
SELECT lion_fj('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk AND f.x < d.attr GROUP BY d.attr');
SELECT lion_fj('SELECT d.attr, count(*) FROM lion_ff f LEFT JOIN lion_fd d ON f.fk = d.pk GROUP BY d.attr');
SELECT lion_fj('SELECT d.attr, f.x, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk GROUP BY d.attr, f.x');
SELECT lion_fj('SELECT d.attr + (random() * 0)::int AS a, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk GROUP BY 1');
SELECT lion_fj('SELECT d.attr, sum(f.x) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk GROUP BY d.attr');
SELECT lion_fj('SELECT d.attr, count(f.y) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk GROUP BY d.attr');
-- a fact filter the posting sets cannot answer
SELECT lion_fj('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk WHERE f.x > 3 GROUP BY d.attr');
-- the dimension key pinned to a constant: no join clause is left at all
SELECT lion_fj('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk WHERE d.pk = 5 GROUP BY d.attr');
-- three relations
SELECT lion_fj('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk JOIN lion_fd4 e ON e.k = f.x GROUP BY d.attr');
-- a partitioned fact table
CREATE TABLE lion_ffp (fk int4, x int NOT NULL) PARTITION BY RANGE (x);
CREATE TABLE lion_ffp1 PARTITION OF lion_ffp FOR VALUES FROM (0) TO (5);
CREATE TABLE lion_ffp2 PARTITION OF lion_ffp FOR VALUES FROM (5) TO (10);
INSERT INTO lion_ffp SELECT i % 120 + 1, i % 10 FROM generate_series(1, 3000) i;
CREATE INDEX ON lion_ffp USING lion (fk);
ANALYZE lion_ffp;
SELECT lion_fj('SELECT d.attr, count(*) FROM lion_ffp f JOIN lion_fd d ON f.fk = d.pk GROUP BY d.attr');
DROP TABLE lion_ffp;
-- ... and a partitioned dimension, which has no index list to prove its key
-- unique from
CREATE TABLE lion_fdp (k int8 PRIMARY KEY, attr int) PARTITION BY RANGE (k);
CREATE TABLE lion_fdp1 PARTITION OF lion_fdp FOR VALUES FROM (0) TO (50);
CREATE TABLE lion_fdp2 PARTITION OF lion_fdp FOR VALUES FROM (50) TO (200);
INSERT INTO lion_fdp SELECT i, i % 4 FROM generate_series(1, 100) i;
ANALYZE lion_fdp;
SELECT lion_fj('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fdp d ON f.fk = d.k GROUP BY d.attr');
DROP TABLE lion_fdp;
-- a key that is unique only under another collation than the join's; then
-- one that is unique under the join's own
CREATE TABLE lion_fdc (k text, attr int);
CREATE UNIQUE INDEX ON lion_fdc (k COLLATE "C");
INSERT INTO lion_fdc SELECT 'k' || i, i % 3 FROM generate_series(1, 100) i;
ANALYZE lion_fdc;
SELECT lion_fj('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fdc d ON f.tk = d.k GROUP BY d.attr');
CREATE UNIQUE INDEX ON lion_fdc (k);
SELECT lion_fj('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fdc d ON f.tk = d.k GROUP BY d.attr');
DROP TABLE lion_fdc;
-- the GUC
SET pg_lion.enable_count_pushdown = off;
SELECT lion_fj_pick('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk GROUP BY d.attr');
RESET pg_lion.enable_count_pushdown;

-- ---- 7. security: dimension RLS applied, fact RLS declines, privileges -----
CREATE ROLE lion_fj_reader;
GRANT SELECT ON lion_ff TO lion_fj_reader;
GRANT SELECT (pk, region) ON lion_fd TO lion_fj_reader;
SET ROLE lion_fj_reader;
-- no privilege on d.attr: refused whichever plan runs
SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk GROUP BY d.attr;
SET pg_lion.enable_count_pushdown = off;
SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk GROUP BY d.attr;
RESET pg_lion.enable_count_pushdown;
-- the columns it may read are fine
SELECT lion_fj('SELECT d.region, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk GROUP BY d.region');
RESET ROLE;
GRANT SELECT ON lion_fd TO lion_fj_reader;
ALTER TABLE lion_fd ENABLE ROW LEVEL SECURITY;
CREATE POLICY lion_fd_eu ON lion_fd FOR SELECT TO lion_fj_reader USING (region = 'eu');
SET ROLE lion_fj_reader;
-- the policy hides the 'us' rows from the join in both plans
SELECT lion_fj('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk GROUP BY d.attr');
SELECT d.region, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk GROUP BY d.region;
RESET ROLE;
ALTER TABLE lion_fd DISABLE ROW LEVEL SECURITY;
ALTER TABLE lion_ff ENABLE ROW LEVEL SECURITY;
CREATE POLICY lion_ff_some ON lion_ff FOR SELECT TO lion_fj_reader USING (x < 5);
SET ROLE lion_fj_reader;
-- RLS on the fact side: declined, and the policy applies
SELECT lion_fj('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk GROUP BY d.attr');
RESET ROLE;
ALTER TABLE lion_ff DISABLE ROW LEVEL SECURITY;
DROP POLICY lion_ff_some ON lion_ff;
DROP POLICY lion_fd_eu ON lion_fd;
REVOKE ALL ON lion_ff, lion_fd FROM lion_fj_reader;
DROP ROLE lion_fj_reader;

-- ---- 8. a dirty heap: deletes and updates on both sides, not yet vacuumed -----
DELETE FROM lion_ff WHERE fk = 7;
DELETE FROM lion_ff WHERE fk = 8 AND x < 5;
UPDATE lion_ff SET fk = 9 WHERE fk = 10 AND x = 1;
UPDATE lion_ff SET x = 3 WHERE id % 97 = 0;
UPDATE lion_ff SET fk = NULL WHERE fk = 11 AND y = 2;
DELETE FROM lion_fd WHERE pk = 12;
UPDATE lion_fd SET attr = 6 WHERE pk IN (1, 2, 3);
UPDATE lion_fd SET attr = NULL WHERE pk = 4;
SELECT lion_fj('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk GROUP BY d.attr');
SELECT lion_fj('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk WHERE f.x = 3 GROUP BY d.attr');
SELECT lion_fj('SELECT count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk WHERE d.attr = 6');
SELECT lion_fj('SELECT d.pk, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk WHERE f.y IN (1, 2) GROUP BY d.pk');
SELECT lion_fj('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON f.tk = d.tpk WHERE f.x = 3 GROUP BY d.attr');
SELECT lion_fj_counter('SELECT count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk', 'Heap Blocks Rechecked') > 0 AS dirty_heap_rechecks;
-- the same after VACUUM, from the visibility map
VACUUM lion_ff;
VACUUM lion_fd;
SELECT lion_fj('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk GROUP BY d.attr');
SELECT lion_fj('SELECT d.attr, count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk WHERE f.x = 3 GROUP BY d.attr');
SELECT lion_fj_counter('SELECT count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk', 'Heap Blocks Rechecked') AS vacuumed_heap_rechecks;
-- VACUUM deleted the entries of fk = 7 and fk = 8, whose rows were all gone
-- (DESIGN.md §18): two dimension keys now find no entry at all
SELECT lion_fj_counter('SELECT count(*) FROM lion_ff f JOIN lion_fd d ON f.fk = d.pk', 'Join Keys Without Entry') AS keys_without_entry;

DROP TABLE lion_ff, lion_fd, lion_fd4, lion_ff8, lion_fdn;
DROP FUNCTION lion_fj(text, boolean);
DROP FUNCTION lion_fj_pick(text);
DROP FUNCTION lion_fj_prep(text, text);
DROP FUNCTION lion_fj_counter(text, text);
