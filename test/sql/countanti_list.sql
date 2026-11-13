-- The anti join as a negated list (DESIGN.md §27, "The anti join as a
-- negated list"):
--
--   SELECT count(*) FROM rows r WHERE <r's clauses>
--     AND NOT EXISTS (SELECT 1 FROM skip s WHERE s.a = r.k <s's filters>)
--
-- folded into the count's WHERE: the rows whose k is none of the inner side's
-- keys, NULL rows kept - the count of r's WHERE less the rows of the IN list
-- `k = ANY (keys)`, which the lion index on k answers, the keys the node's
-- child returns at the start of each run.  No B-tree on k is needed.  Every
-- answer is checked against the same query with the pushdown off, as a
-- multiset in both directions.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET default_statistics_target = 1000;

/*
 * lcl() runs a query through the pushdown - every join method disabled, so
 * that core has no plan the node must beat on price - and again with the
 * pushdown off and sequential scans only, and compares the two.  It says
 * whether the node was used.
 */
CREATE FUNCTION lcl(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	how text := 'not pushed down';
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%NOT EXISTS (%' THEN
			how := 'folded';
		ELSIF ln LIKE '%Custom Scan (LionCount)%' AND how = 'not pushed down' THEN
			how := 'pushed down';
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lcl_on AS SELECT s::text AS r FROM (%s) s', q);

	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_nestloop', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	EXECUTE format('CREATE TEMP TABLE lcl_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM lcl_off' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (TABLE lcl_on EXCEPT ALL TABLE lcl_off) a)'
			' + (SELECT count(*) FROM (TABLE lcl_off EXCEPT ALL TABLE lcl_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lcl_on, lcl_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows', how, nrows);
END $$;

/* The same, as a generic prepared plan, which keeps $n a Param. */
CREATE FUNCTION lcl_prep(q text, args text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	how text := 'not pushed down';
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('plan_cache_mode', 'force_generic_plan', true);
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	EXECUTE 'PREPARE lcl_p_on AS ' || q;
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) EXECUTE lcl_p_on(' || args || ')' LOOP
		IF ln LIKE '%NOT EXISTS (%' THEN
			how := 'folded';
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lcl_on AS EXECUTE lcl_p_on(%s)', args);

	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_nestloop', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	EXECUTE 'PREPARE lcl_p_off AS ' || q;
	EXECUTE format('CREATE TEMP TABLE lcl_off AS EXECUTE lcl_p_off(%s)', args);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE 'DEALLOCATE lcl_p_on';
	EXECUTE 'DEALLOCATE lcl_p_off';

	EXECUTE 'SELECT count(*) FROM lcl_off' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (TABLE lcl_on EXCEPT ALL TABLE lcl_off) a)'
			' + (SELECT count(*) FROM (TABLE lcl_off EXCEPT ALL TABLE lcl_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lcl_on, lcl_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows', how, nrows);
END $$;

/*
 * EXPLAIN, joins disabled, in a form every supported release prints the same
 * way: no "Disabled: true" lines, and no ".00" on a row count.
 */
CREATE FUNCTION lcl_explain(q text, opts text DEFAULT 'COSTS OFF') RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	l text;
BEGIN
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	FOR l IN EXECUTE 'EXPLAIN (' || opts || ') ' || q LOOP
		CONTINUE WHEN l ~ '^\s*Disabled: true$';
		RETURN NEXT regexp_replace(l, 'rows=(\d+)\.00 ', 'rows=\1 ', 'g');
	END LOOP;
END $$;

/* What EXPLAIN ANALYZE says of the lists, joins disabled. */
CREATE FUNCTION lcl_counters(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	out text := '';
BEGIN
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF btrim(split_part(ln, ':', 1)) IN ('Anti Join Keys', 'Lists Collected',
											 'Lists Spilled') THEN
			out := out || CASE WHEN out = '' THEN '' ELSE ', ' END || btrim(ln);
		END IF;
	END LOOP;
	RETURN out;
END $$;

/*
 * The table counted: 20000 rows.  k has 400 values, some 50 rows each, and
 * NULL on every 37th row; k8 is k as an int8 and kt as a text; g has 6 values
 * and NULLs, y 7 and NULLs, x 10 values; w has no index at all.  Lion
 * indexes on k, k8, kt, x, g and (g, y) - and no B-tree but the primary key.
 */
CREATE TABLE lcl_rows (
	id		int		PRIMARY KEY,
	k		int,
	k8		int8,
	kt		text,
	g		int,
	y		int,
	x		int		NOT NULL,
	w		int		NOT NULL,
	pad		text	NOT NULL
) WITH (autovacuum_enabled = off);
INSERT INTO lcl_rows
SELECT i,
	   CASE WHEN i % 37 = 0 THEN NULL ELSE i % 400 END,
	   CASE WHEN i % 37 = 0 THEN NULL ELSE i % 400 END,
	   CASE WHEN i % 37 = 0 THEN NULL ELSE 'k' || (i % 400) END,
	   CASE WHEN i % 11 = 0 THEN NULL ELSE i % 6 END,
	   CASE WHEN i % 13 = 0 THEN NULL ELSE i % 7 END,
	   (i / 400) % 10,
	   i % 400,
	   repeat('p', 60)
FROM generate_series(1, 20000) i;
CREATE INDEX lcl_rows_k ON lcl_rows USING lion (k);
CREATE INDEX lcl_rows_k8 ON lcl_rows USING lion (k8);
CREATE INDEX lcl_rows_kt ON lcl_rows USING lion (kt);
CREATE INDEX lcl_rows_x ON lcl_rows USING lion (x);
CREATE INDEX lcl_rows_g ON lcl_rows USING lion (g);
CREATE INDEX lcl_rows_gy ON lcl_rows USING lion (g, y);

/*
 * The inner side: a few keys - two rows of key 2, NULL, keys of no row of
 * lcl_rows, and two rows of another f.  c is a as an int8, t as a text.
 */
CREATE TABLE lcl_skip (a int, c int8, t text, f int NOT NULL);
INSERT INTO lcl_skip VALUES
	(1, 1, 'k1', 0), (2, 2, 'k2', 0), (2, 2, 'k2', 0), (3, 3, 'k3', 0),
	(NULL, NULL, NULL, 0), (6, 6, 'k6', 0), (5, 5, 'k5', 0),
	(450, 450, 'k450', 0), (-1, -1, 'k-1', 0),
	(7, 7, 'k7', 1), (8, 8, 'k8', 1);
CREATE TABLE lcl_tag (a int, tag text);
INSERT INTO lcl_tag VALUES (1, 'on'), (2, 'off'), (5, 'on'), (7, 'on'), (450, 'on');
-- many keys: every third value of k, and as many of no row
CREATE TABLE lcl_many (a int NOT NULL, b int NOT NULL);
INSERT INTO lcl_many SELECT (i * 3) % 400, i FROM generate_series(1, 6000) i;
INSERT INTO lcl_many SELECT 1000 + i, i FROM generate_series(1, 6000) i;
VACUUM (FREEZE, ANALYZE) lcl_rows, lcl_skip, lcl_tag, lcl_many;

-- ---------- 1. the plan ----------
SELECT * FROM lcl_explain('SELECT count(*) FROM lcl_rows r WHERE r.x = 3 AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k AND s.f = 0)');
-- with nothing else in the WHERE: every row's count, less the list's
SELECT * FROM lcl_explain('SELECT count(*) FROM lcl_rows r WHERE NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k)');
-- grouped
SELECT * FROM lcl_explain('SELECT r.g, count(*) FROM lcl_rows r WHERE r.x < 5 AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k) GROUP BY r.g');
-- the keys collected, NULL ones left out, and the one list
SELECT * FROM lcl_explain('SELECT count(*) FROM lcl_rows r WHERE r.x = 3 AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k AND s.f = 0)',
						  'ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF') AS ln
	WHERE ln ~ '(Custom Scan|Lion Indexes|Anti Join|Lists|Seq Scan|Filter)';

-- ---------- 2. the answers ----------
SELECT n, lcl(q) FROM (VALUES
	-- the WHERE: nothing, an equality, a range, a list, null tests, `<>`
	(1, 'SELECT count(*) FROM lcl_rows r WHERE NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k)'),
	(2, 'SELECT count(*) FROM lcl_rows r WHERE r.x = 3 AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k AND s.f = 0)'),
	(3, 'SELECT count(*) FROM lcl_rows r WHERE r.x < 5 AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k)'),
	(4, 'SELECT count(*) FROM lcl_rows r WHERE r.x IN (1, 4) AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k)'),
	(5, 'SELECT count(*) FROM lcl_rows r WHERE r.k IS NULL AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k)'),
	(6, 'SELECT count(*) FROM lcl_rows r WHERE r.k IS NOT NULL AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k)'),
	(7, 'SELECT count(*) FROM lcl_rows r WHERE r.k <> 2 AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k)'),
	(8, 'SELECT count(*) FROM lcl_rows r WHERE (r.x = 1 OR r.g = 2) AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k)'),
	(9, 'SELECT count(*) FROM lcl_rows r WHERE r.x = 3 AND length(r.pad) > 0 AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k)'),
	-- what is counted
	(10, 'SELECT count(r.k) FROM lcl_rows r WHERE r.x < 5 AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k)'),
	(30, 'SELECT count(r.g) FROM lcl_rows r WHERE r.x < 5 AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k)'),
	-- grouped by one column, by two, with a HAVING, and in order
	(11, 'SELECT r.g, count(*) FROM lcl_rows r WHERE NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k) GROUP BY r.g'),
	(12, 'SELECT r.g, r.y, count(*) FROM lcl_rows r WHERE r.x < 5 AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k) GROUP BY r.g, r.y'),
	(13, 'SELECT r.x, count(*) FROM lcl_rows r WHERE NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k AND s.a < 400) GROUP BY r.x HAVING count(*) > 1945'),
	(14, 'SELECT r.k, count(*) FROM lcl_rows r WHERE r.x = 3 AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k) GROUP BY r.k'),
	-- an inner side that is empty, of NULL keys alone, of keys of no row, a join
	(15, 'SELECT count(*) FROM lcl_rows r WHERE NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k AND s.f = 42)'),
	(16, 'SELECT count(*) FROM lcl_rows r WHERE NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k AND s.a IS NULL)'),
	(17, 'SELECT count(*) FROM lcl_rows r WHERE NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k AND s.a IN (450, -1))'),
	(18, 'SELECT count(*) FROM lcl_rows r WHERE r.x = 3 AND NOT EXISTS (SELECT 1 FROM lcl_skip s JOIN lcl_tag t ON t.a = s.a WHERE s.a = r.k AND t.tag = ''on'')'),
	-- across types, a text key, and the operands the other way round
	(19, 'SELECT count(*) FROM lcl_rows r WHERE r.x < 5 AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.c = r.k)'),
	(20, 'SELECT count(*) FROM lcl_rows r WHERE r.x < 5 AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k8)'),
	(21, 'SELECT count(*) FROM lcl_rows r WHERE r.x < 5 AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.t = r.kt)'),
	(22, 'SELECT count(*) FROM lcl_rows r WHERE r.x < 5 AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE r.k = s.a)'),
	-- two NOT EXISTS, on two columns and on one, and the LEFT JOIN spelling
	(23, 'SELECT count(*) FROM lcl_rows r WHERE NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k) AND NOT EXISTS (SELECT 1 FROM lcl_skip s2 WHERE s2.c = r.k8 AND s2.f = 1)'),
	(24, 'SELECT count(*) FROM lcl_rows r WHERE NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k AND s.f = 0) AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.t = r.kt AND s.f = 1)'),
	(25, 'SELECT r.g, count(*) FROM lcl_rows r WHERE NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k) AND NOT EXISTS (SELECT 1 FROM lcl_tag t WHERE t.a = r.k) GROUP BY r.g'),
	(26, 'SELECT count(*) FROM lcl_rows r LEFT JOIN lcl_skip s ON s.a = r.k WHERE r.x = 3 AND s.a IS NULL'),
	(27, 'SELECT r.g, count(*) FROM lcl_rows r LEFT JOIN lcl_skip s ON s.a = r.k AND s.f = 0 WHERE s.a IS NULL GROUP BY r.g'),
	-- many keys, every third value of k's among them
	(28, 'SELECT count(*) FROM lcl_rows r WHERE NOT EXISTS (SELECT 1 FROM lcl_many m WHERE m.a = r.k)'),
	(29, 'SELECT r.g, count(*) FROM lcl_rows r WHERE r.x <> 4 AND NOT EXISTS (SELECT 1 FROM lcl_many m WHERE m.a = r.k AND m.b % 5 <> 0) GROUP BY r.g')
) v(n, q) ORDER BY n;

-- ---------- 3. what is not folded ----------
SELECT n, lcl(q) FROM (VALUES
	-- a second join clause, an inner expression, a column of no lion index
	(1, 'SELECT count(*) FROM lcl_rows r WHERE r.x = 3 AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k AND s.f = r.g)'),
	(2, 'SELECT count(*) FROM lcl_rows r WHERE r.x = 3 AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a + 1 = r.k)'),
	(3, 'SELECT count(*) FROM lcl_rows r WHERE r.x = 3 AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.w)'),
	-- a correlation by no equality, and a count(DISTINCT)
	(4, 'SELECT count(*) FROM lcl_rows r WHERE r.x = 3 AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a < r.k)'),
	(5, 'SELECT count(DISTINCT r.x) FROM lcl_rows r WHERE NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k)')
) v(n, q) ORDER BY n;
-- the GUC, and the anti join's own
SET pg_lion.enable_count_anti_list = off;
SELECT lcl('SELECT count(*) FROM lcl_rows r WHERE r.x = 3 AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k)');
RESET pg_lion.enable_count_anti_list;
SET pg_lion.enable_count_anti = off;
SELECT lcl('SELECT count(*) FROM lcl_rows r WHERE r.x = 3 AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k)');
RESET pg_lion.enable_count_anti;

-- ---------- 4. runs again ----------
-- a rescan under a correlated subquery: a parameter of the WHERE, and of the
-- inner side, whose child runs again
SELECT lcl('SELECT o.v, o.w, (SELECT count(*) FROM lcl_rows r WHERE r.x = o.v AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k AND s.f = o.w)) FROM (VALUES (1, 0), (3, 0), (3, 1), (4, 42), (3, 0)) o(v, w)');
SELECT lcl('SELECT o.v, (SELECT count(*) FROM lcl_rows r WHERE r.x = o.v AND NOT EXISTS (SELECT 1 FROM lcl_many m WHERE m.a = r.k AND m.b < o.v * 1000) GROUP BY r.g ORDER BY 1 DESC LIMIT 1) FROM (VALUES (3), (1), (3)) o(v)');
-- a generic plan, its parameters in the WHERE and in the inner side
SELECT lcl_prep('SELECT count(*) FROM lcl_rows r WHERE r.x = $1 AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k AND s.f = $2)', '3, 0');
SELECT lcl_prep('SELECT r.g, count(*) FROM lcl_rows r WHERE NOT EXISTS (SELECT 1 FROM lcl_many m WHERE m.a = r.k AND m.b < $1) GROUP BY r.g', '2500');

-- ---------- 5. past work_mem ----------
-- the keys past a batch go to a tuplestore, which spills, and the list's set
-- is collected into a file: the answers are the same
SET work_mem = '64kB';
SELECT lcl_counters('SELECT count(*) FROM lcl_rows r WHERE NOT EXISTS (SELECT 1 FROM lcl_many m WHERE m.a = r.k)');
SELECT lcl('SELECT count(*) FROM lcl_rows r WHERE NOT EXISTS (SELECT 1 FROM lcl_many m WHERE m.a = r.k)');
SELECT lcl('SELECT r.g, r.y, count(*) FROM lcl_rows r WHERE NOT EXISTS (SELECT 1 FROM lcl_many m WHERE m.a = r.k AND m.b % 7 <> 0) GROUP BY r.g, r.y');
SELECT lcl('SELECT o.v, (SELECT count(*) FROM lcl_rows r WHERE r.x = o.v AND NOT EXISTS (SELECT 1 FROM lcl_many m WHERE m.a = r.k)) FROM (VALUES (3), (1), (3)) o(v)');
RESET work_mem;

-- ---------- 6. the snapshot ----------
-- the inner side's rows the transaction deleted, and the table's, before any
-- VACUUM: the child and the count both read the transaction's snapshot
BEGIN;
DELETE FROM lcl_skip WHERE a = 2;
DELETE FROM lcl_rows WHERE id % 5 = 0;
SELECT lcl('SELECT count(*) FROM lcl_rows r WHERE NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k)');
SELECT lcl('SELECT r.g, count(*) FROM lcl_rows r WHERE r.x < 5 AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k) GROUP BY r.g');
ROLLBACK;

-- ---------- 7. privileges ----------
-- EXECUTE on the function of the operator the query wrote, asked of the
-- fold as of any clause lion answers (core's hash join, which commutes the
-- clause, asks its commutator's instead)
CREATE ROLE lcl_reader;
GRANT SELECT ON lcl_rows, lcl_skip TO lcl_reader;
REVOKE EXECUTE ON FUNCTION int84eq(int8, int4) FROM PUBLIC;
SET ROLE lcl_reader;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lcl_rows r WHERE r.x < 5 AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.c = r.k);
SELECT count(*) FROM lcl_rows r WHERE r.x < 5 AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.c = r.k);
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_nestloop;
SET pg_lion.enable_count_pushdown = off;
SELECT count(*) FROM lcl_rows r WHERE r.x < 5 AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.c = r.k);
RESET pg_lion.enable_count_pushdown;
RESET ROLE;
GRANT EXECUTE ON FUNCTION int84eq(int8, int4) TO PUBLIC;
-- row-level security on the table counted: not folded, the right count
ALTER TABLE lcl_rows ENABLE ROW LEVEL SECURITY;
CREATE POLICY lcl_rows_some ON lcl_rows FOR SELECT TO lcl_reader USING (g = 1);
SET ROLE lcl_reader;
SELECT lcl('SELECT count(*) FROM lcl_rows r WHERE r.x < 5 AND NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k)');
RESET ROLE;
ALTER TABLE lcl_rows DISABLE ROW LEVEL SECURITY;
DROP POLICY lcl_rows_some ON lcl_rows;
REVOKE ALL ON lcl_rows, lcl_skip FROM lcl_reader;
DROP ROLE lcl_reader;

-- ---------- 8. the cost model ----------
-- with nothing disabled, over a table with no B-tree on its key: the folded
-- count at a few keys; at 12000 keys, every value of k among them, over a
-- table of 20000 rows, the hash anti join, which reads fewer rows than the
-- list looks up
EXPLAIN (COSTS OFF) SELECT count(*) FROM lcl_rows r WHERE NOT EXISTS (SELECT 1 FROM lcl_skip s WHERE s.a = r.k AND s.f = 0);
EXPLAIN (COSTS OFF) SELECT r.g, count(*) FROM lcl_rows r WHERE NOT EXISTS (SELECT 1 FROM lcl_many m WHERE m.a = r.k) GROUP BY r.g;
DROP TABLE lcl_rows, lcl_skip, lcl_tag, lcl_many;
DROP FUNCTION lcl(text), lcl_prep(text, text), lcl_explain(text, text), lcl_counters(text);
