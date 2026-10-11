-- Query shapes the count pushdown answers by rewriting them to the counts it
-- makes (DESIGN.md §10, "Shapes rewritten to the node's counts").  Each is
-- run through the LionCount node, with every other scan disabled so that a
-- node that is built at all is the plan, and again as a sequential scan with
-- the pushdown off; the two answers are compared as multisets in both
-- directions.  The table has NULLs, and dead and updated rows that VACUUM has
-- not seen, so that the node's heap checks (DESIGN.md §9) are part of every
-- answer.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE cs (id int, a int, b int NOT NULL, n int, t text, u int)
	WITH (autovacuum_enabled = off);
INSERT INTO cs SELECT i, i % 7, i % 5,
	   CASE WHEN i % 4 = 0 THEN NULL ELSE i % 11 END,
	   CASE WHEN i % 9 = 0 THEN NULL ELSE 'v' || (i % 13) END,
	   i % 3
  FROM generate_series(1, 20000) i;
CREATE INDEX cs_a ON cs USING lion (a);
CREATE INDEX cs_b ON cs USING lion (b);
CREATE INDEX cs_n ON cs USING lion (n);
CREATE INDEX cs_t ON cs USING lion (t);
VACUUM (FREEZE, ANALYZE) cs;
-- dead and updated rows the visibility map no longer covers
DELETE FROM cs WHERE id % 97 = 0;
UPDATE cs SET a = (a + 1) % 7, n = NULL WHERE id % 89 = 0;
UPDATE cs SET n = 3 WHERE id % 83 = 0;
-- a dimension on cs.a, for the FK-side join and the semi join (DESIGN.md §27)
CREATE TABLE csd (pk int PRIMARY KEY, r text NOT NULL);
INSERT INTO csd SELECT i, 'r' || (i % 3) FROM generate_series(0, 9) i;
VACUUM ANALYZE csd;

/*
 * cs_q() runs q through the LionCount node and as a sequential scan with the
 * pushdown off, and compares the two: 'count, N rows' when the node was used
 * and they agree.
 */
CREATE FUNCTION cs_q(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	used boolean := false;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(LionCount\)' THEN
			used := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE cs_on AS SELECT s::text AS r FROM (%s) s', q);

	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE format('CREATE TEMP TABLE cs_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM cs_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM cs_on EXCEPT ALL SELECT * FROM cs_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM cs_off EXCEPT ALL SELECT * FROM cs_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE cs_on, cs_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows', CASE WHEN used THEN 'count' ELSE 'no count' END,
				  nrows);
END $$;

-- the plan, with the node the only scan allowed
CREATE FUNCTION cs_plan(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		-- 18 prints each window's definition under a WindowAgg
		CONTINUE WHEN ln ~ '^\s*Window: ';
		RETURN NEXT ln;
	END LOOP;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
END $$;

-- ---- expressions over the counts ----
SELECT cs_plan('SELECT count(*)::int FROM cs WHERE a = 3 AND b = 1');
SELECT cs_plan('SELECT a, round(100.0 * count(*) / 20000, 2) FROM cs WHERE b = 1 GROUP BY a');
SELECT q, cs_q(q) FROM (VALUES
	('SELECT count(*)::int FROM cs WHERE a = 3 AND b = 1'),
	('SELECT 100.0 * count(*) / 20000 FROM cs WHERE a = 3'),
	('SELECT count(*) + count(n), count(n) * 2 FROM cs WHERE n = 4'),
	('SELECT a, round(100.0 * count(*) / 20000, 2) FROM cs WHERE b = 1 GROUP BY a'),
	('SELECT a + 1, count(*) - 1 FROM cs GROUP BY a'),
	('SELECT t || ''!'', count(*)::text FROM cs GROUP BY t'),
	('SELECT a, b, count(*) * 10 FROM cs GROUP BY a, b'),
	('SELECT a * 100 + b, count(*) FROM cs WHERE b = 2 GROUP BY a, b'),
	('SELECT 1, count(*) FROM cs WHERE a = 2'),
	('SELECT a, count(*)::int FROM cs GROUP BY a HAVING count(*)::int > 2800'),
	('SELECT coalesce(n, -1), coalesce(count(*), 0) + 1 FROM cs GROUP BY coalesce(n, -1)'),
	('SELECT count(DISTINCT n)::int FROM cs WHERE a = 1'),
	('SELECT d.r, count(*)::int, 100.0 * count(*) / 20000 FROM cs JOIN csd d ON d.pk = cs.a WHERE cs.b = 1 GROUP BY d.r'),
	('SELECT count(*)::int FROM cs WHERE cs.a IN (SELECT pk FROM csd WHERE r = ''r1'')')
) v(q);

-- the projection runs once per row the node emits: a volatile function in
-- it is called once a group, as the Agg's projection would call it
CREATE SEQUENCE cs_seq;
SELECT cs_plan('SELECT a, count(*) + 0 * nextval(''cs_seq'') FROM cs GROUP BY a');
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*) FROM (SELECT a, count(*) + 0 * nextval('cs_seq') FROM cs GROUP BY a) s;
SELECT currval('cs_seq');
RESET enable_seqscan;
RESET enable_bitmapscan;
DROP SEQUENCE cs_seq;

-- a subquery over the counts is a SubPlan the node does not run: the grouping
-- is the ordinary plan's (the subquery's own count is pushed down)
SELECT regexp_replace(btrim(p), '^(Hash|Group)Aggregate', 'Aggregate')
  FROM cs_plan('SELECT a, count(*) + (SELECT count(*) FROM cs c2 WHERE c2.a = cs.a) FROM cs GROUP BY a') p
 WHERE p ~ '(Aggregate|LionCount|SubPlan)';
SELECT cs_q('SELECT a, count(*) + (SELECT count(*) FROM cs c2 WHERE c2.a = cs.a) FROM cs GROUP BY a');

-- ---- count(1): a non-NULL constant is counted in every row ----
SELECT cs_plan('SELECT count(1) FROM cs WHERE a = 3 AND b = 1');
SELECT q, cs_q(q) FROM (VALUES
	('SELECT count(1) FROM cs WHERE a = 3 AND b = 1'),
	('SELECT count(''x''), count(1::bigint), count(*) FROM cs WHERE n = 4'),
	('SELECT a, count(1) FROM cs WHERE b = 2 GROUP BY a'),
	('SELECT n, count(1) FROM cs GROUP BY n'),
	('SELECT count(1) FROM cs'),
	('SELECT t, count(1) FROM cs GROUP BY t ORDER BY count(1) DESC LIMIT 1'),
	('SELECT d.r, count(1) FROM cs JOIN csd d ON d.pk = cs.a GROUP BY d.r')
) v(q);
-- count(NULL) is 0 in every group, and not taken
SELECT cs_q('SELECT a, count(NULL::int) FROM cs GROUP BY a');

-- ---- count(col) of a nullable column: count(*) AND col IS NOT NULL ----
SELECT cs_plan('SELECT count(n) FROM cs WHERE a = 3');
SELECT cs_plan('SELECT count(t) FROM cs');
SELECT q, cs_q(q) FROM (VALUES
	('SELECT count(n) FROM cs WHERE a = 3'),
	('SELECT count(n) FROM cs'),
	('SELECT count(t), count(t)::int + 1 FROM cs WHERE b = 2'),
	('SELECT count(n) FROM cs WHERE n <> 3'),
	('SELECT count(n) FROM cs WHERE a IN (1, 2) AND b = 4'),
	('SELECT count(n) FROM cs WHERE a = 3 OR b = 1'),
	('SELECT count(n) FROM cs WHERE a = 3 HAVING count(n) > 10'),
	('SELECT count(n) FROM cs WHERE a = 99')
) v(q);
-- beside count(*) or another column's count, or under a GROUP BY (a group
-- whose rows are all NULL counts 0), it is the ordinary plan's
SELECT q, cs_q(q) FROM (VALUES
	('SELECT count(n), count(*) FROM cs WHERE a = 3'),
	('SELECT count(n), count(t) FROM cs'),
	('SELECT a, count(n) FROM cs GROUP BY a'),
	('SELECT count(u) FROM cs')
) v(q);

-- ---- NOT IN (list): its <> clauses, ANDed ----
SELECT cs_plan('SELECT count(*) FROM cs WHERE a NOT IN (3, 4, 5)');
SELECT q, cs_q(q) FROM (VALUES
	('SELECT count(*) FROM cs WHERE a NOT IN (3, 4, 5)'),
	('SELECT count(*) FROM cs WHERE n NOT IN (1, 2)'),
	('SELECT count(*) FROM cs WHERE n <> ALL (ARRAY[1, 2, 2])'),
	('SELECT count(*) FROM cs WHERE t NOT IN (''v1'', ''v2'') AND b = 3'),
	('SELECT b, count(*) FROM cs WHERE a NOT IN (0, 6) GROUP BY b'),
	('SELECT n, count(*) FROM cs WHERE n NOT IN (1, 2) GROUP BY n'),
	('SELECT count(*) FROM cs WHERE a NOT IN (3) AND n NOT IN (3, 5)'),
	('SELECT d.r, count(*) FROM cs JOIN csd d ON d.pk = cs.a WHERE cs.n NOT IN (1, 2) GROUP BY d.r')
) v(q);
-- a NULL in the list makes it unknown for every row, and an empty one true
-- for every row: both are the ordinary plan's
SELECT q, cs_q(q) FROM (VALUES
	('SELECT count(*) FROM cs WHERE a NOT IN (3, NULL)'),
	('SELECT count(*) FROM cs WHERE n <> ALL (''{}''::int[])'),
	('SELECT count(*) FROM cs WHERE u NOT IN (1, 2)')
) v(q);

-- ---- window functions over the grouped counts ----
SELECT cs_plan('SELECT a, round(100.0 * count(*) / sum(count(*)) OVER (), 2) FROM cs GROUP BY a');
SELECT q, cs_q(q) FROM (VALUES
	('SELECT a, round(100.0 * count(*) / sum(count(*)) OVER (), 2) FROM cs GROUP BY a'),
	('SELECT a, b, count(*), sum(count(*)) OVER (PARTITION BY b), rank() OVER (PARTITION BY a ORDER BY count(*) DESC, b) FROM cs GROUP BY a, b'),
	('SELECT n, count(n), lag(n) OVER (ORDER BY n NULLS FIRST) FROM cs WHERE b = 1 GROUP BY n'),
	('SELECT count(*), count(*) OVER () FROM cs WHERE a = 3'),
	('SELECT t, count(*), row_number() OVER (ORDER BY t) FROM cs WHERE t <> ''v3'' GROUP BY t HAVING count(*) > 1300'),
	('SELECT d.r, count(*), sum(count(*)) OVER () FROM cs JOIN csd d ON d.pk = cs.a GROUP BY d.r')
) v(q);
-- a window over an aggregate the node cannot count (u is nullable and has no
-- lion index) is the ordinary plan's
SELECT cs_q('SELECT a, sum(count(u)) OVER () FROM cs GROUP BY a');

-- ---- GROUP BY or DISTINCT with no aggregate: the keys with a visible row ----
-- a key whose every row is deleted or updated away, never vacuumed
INSERT INTO cs SELECT 900000 + i, 40, 4, 40, 'gone', 0 FROM generate_series(1, 50) i;
UPDATE cs SET a = 41, t = 'moved' WHERE id = 900001;
DELETE FROM cs WHERE id > 900000;
INSERT INTO cs VALUES (900100, 42, 1, NULL, NULL, 0);
SELECT cs_plan('SELECT DISTINCT a FROM cs');
SELECT cs_plan('SELECT a, b FROM cs GROUP BY 1, 2');
SELECT cs_plan('SELECT DISTINCT a FROM cs ORDER BY a DESC');
SELECT q, cs_q(q) FROM (VALUES
	('SELECT DISTINCT a FROM cs'),
	('SELECT DISTINCT t FROM cs'),
	('SELECT DISTINCT n FROM cs WHERE b = 1'),
	('SELECT DISTINCT a, b FROM cs'),
	('SELECT DISTINCT a, b, n FROM cs'),
	('SELECT DISTINCT a FROM cs WHERE a = 3'),
	('SELECT DISTINCT a FROM cs WHERE a = 40'),
	('SELECT DISTINCT a + 1 FROM cs'),
	('SELECT DISTINCT a FROM cs WHERE n NOT IN (1, 2) ORDER BY a DESC'),
	('SELECT DISTINCT t FROM cs ORDER BY t LIMIT 3'),
	('SELECT a FROM cs GROUP BY a'),
	('SELECT a, b FROM cs GROUP BY 1, 2'),
	('SELECT t || ''!'' FROM cs WHERE b = 2 GROUP BY t'),
	('SELECT a FROM cs WHERE a = 3 GROUP BY a'),
	('SELECT a FROM cs WHERE a = 40 GROUP BY a'),
	('SELECT a, n FROM cs WHERE b IN (1, 2) GROUP BY a, n'),
	('SELECT coalesce(n, -1) FROM cs GROUP BY 1')
) v(q);
-- DISTINCT ON, DISTINCT over a GROUP BY, and a column with no lion index are
-- the ordinary plan's
SELECT q, cs_q(q) FROM (VALUES
	('SELECT DISTINCT ON (a) a, b FROM cs ORDER BY a, b'),
	('SELECT DISTINCT count(*) FROM cs GROUP BY a'),
	('SELECT DISTINCT u FROM cs')
) v(q);

DROP FUNCTION cs_q(text);
DROP FUNCTION cs_plan(text);
DROP TABLE cs, csd;
