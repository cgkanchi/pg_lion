-- Complements in the count pushdown (DESIGN.md §35, "Complements in an
-- OR"): `IS DISTINCT FROM`, `NOT` of a multi-key query, and arms of an OR
-- that are `<>`, `IS NOT NULL`, `IS DISTINCT FROM`, `NOT IN` or such a `NOT`,
-- counted by De Morgan over the rows every count is handed.  Each query is
-- run through the LionCount node, with every other scan disabled so that a
-- node that is built at all is the plan, and again as a sequential scan with
-- the pushdown off; the two answers are compared as multisets both ways
-- round.  The table has NULLs, negative values, empty arrays and NULL arrays,
-- and rows deleted and updated after its VACUUM.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE cc (id int, a int, b int NOT NULL, n int, t text, tags text[])
	WITH (autovacuum_enabled = off);
INSERT INTO cc SELECT i, i % 7, i % 5,
	   CASE WHEN i % 4 = 0 THEN NULL ELSE i % 11 - 5 END,
	   CASE WHEN i % 9 = 0 THEN NULL ELSE 'v' || (i % 13) END,
	   CASE WHEN i % 10 = 0 THEN NULL WHEN i % 10 = 1 THEN '{}'::text[]
			WHEN i % 10 = 2 THEN ARRAY['t' || (i % 6), NULL]
			ELSE ARRAY['t' || (i % 6), 't' || (i % 17)] END
  FROM generate_series(1, 20000) i;
CREATE INDEX cc_a ON cc USING lion (a);
CREATE INDEX cc_b ON cc USING lion (b);
CREATE INDEX cc_n ON cc USING lion (n);
CREATE INDEX cc_t ON cc USING lion (t);
CREATE INDEX cc_tags ON cc USING lion (tags);
VACUUM (FREEZE, ANALYZE) cc;
-- dead and updated rows the visibility map no longer covers
DELETE FROM cc WHERE id % 97 = 0;
UPDATE cc SET a = (a + 1) % 7, n = NULL WHERE id % 89 = 0;
UPDATE cc SET n = -2, tags = '{t5}' WHERE id % 83 = 0;

/*
 * cc_q() runs q through the LionCount node and as a sequential scan with the
 * pushdown off, and compares the two: 'count, N rows' when the node was used
 * and they agree.
 */
CREATE FUNCTION cc_q(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	used boolean := false;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(LionCount\)' THEN
			used := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE cc_on AS SELECT s::text AS r FROM (%s) s', q);

	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE format('CREATE TEMP TABLE cc_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM cc_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM cc_on EXCEPT ALL SELECT * FROM cc_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM cc_off EXCEPT ALL SELECT * FROM cc_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE cc_on, cc_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows', CASE WHEN used THEN 'count' ELSE 'no count' END,
				  nrows);
END $$;

-- the plan, with the node the only scan allowed
CREATE FUNCTION cc_plan(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		RETURN NEXT ln;
	END LOOP;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
END $$;

-- ---- the plans ----
SELECT cc_plan('SELECT count(*) FROM cc WHERE n IS DISTINCT FROM -2');
SELECT cc_plan('SELECT count(*) FROM cc WHERE n <> -2 OR n IS NULL');
SELECT cc_plan('SELECT count(*) FROM cc WHERE a = 2 OR n <> -2');
SELECT cc_plan('SELECT count(*) FROM cc WHERE NOT (tags @> ''{t3}'')');
SELECT cc_plan('SELECT a, count(*) FROM cc WHERE b = 1 AND (n <> 3 OR t = ''v4'') GROUP BY a');

-- ---- IS DISTINCT FROM, and NOT of a multi-key query, alone ----
SELECT q, cc_q(q) FROM (VALUES
	('SELECT count(*) FROM cc WHERE n IS DISTINCT FROM -2'),
	('SELECT count(*) FROM cc WHERE -2 IS DISTINCT FROM n'),
	('SELECT count(*) FROM cc WHERE n IS DISTINCT FROM 99'),
	('SELECT count(*) FROM cc WHERE n IS DISTINCT FROM NULL'),
	('SELECT count(*) FROM cc WHERE n IS DISTINCT FROM 3::bigint'),
	('SELECT count(*) FROM cc WHERE t IS DISTINCT FROM ''v4'''),
	('SELECT count(*) FROM cc WHERE n IS DISTINCT FROM 3 AND a = 2'),
	('SELECT count(*) FROM cc WHERE n IS DISTINCT FROM 3 AND t IS DISTINCT FROM ''v1'''),
	('SELECT count(n), count(t) FROM cc WHERE n IS DISTINCT FROM 3'),
	('SELECT a, count(*) FROM cc WHERE n IS DISTINCT FROM 3 GROUP BY a'),
	('SELECT n, count(*) FROM cc WHERE n IS DISTINCT FROM 3 GROUP BY n'),
	('SELECT n, count(*) FROM cc WHERE n IS DISTINCT FROM 3 GROUP BY n ORDER BY count(*) DESC LIMIT 3'),
	('SELECT count(DISTINCT n) FROM cc WHERE n IS DISTINCT FROM 3'),
	('SELECT count(*) FROM cc WHERE NOT (tags @> ''{t3}'')'),
	('SELECT count(*) FROM cc WHERE NOT (tags @> ''{t3,t5}'')'),
	('SELECT count(*) FROM cc WHERE NOT (tags && ''{t1,t16}'')'),
	('SELECT count(*) FROM cc WHERE NOT (tags @> ''{nope}'')'),
	('SELECT count(*), count(tags) FROM cc WHERE NOT (tags @> ''{t3}'') AND b = 2'),
	('SELECT b, count(*) FROM cc WHERE NOT (tags @> ''{t3}'') GROUP BY b')
) v(q);

-- ---- complements in the arms of an OR ----
SELECT q, cc_q(q) FROM (VALUES
	('SELECT count(*) FROM cc WHERE n <> -2 OR n IS NULL'),
	('SELECT count(*) FROM cc WHERE n IS NULL OR n <> -2'),
	('SELECT count(*) FROM cc WHERE a = 2 OR n <> -2'),
	('SELECT count(*) FROM cc WHERE n <> -2 OR a = 2 OR t = ''v1'''),
	('SELECT count(*) FROM cc WHERE n IS NOT NULL OR t = ''v1'''),
	('SELECT count(*) FROM cc WHERE n IS DISTINCT FROM 3 OR a = 1'),
	('SELECT count(*) FROM cc WHERE n <> 3 OR t <> ''v1'''),
	('SELECT count(*) FROM cc WHERE n <> 3 OR t IS NOT NULL'),
	('SELECT count(*) FROM cc WHERE (n <> 3 AND t <> ''v1'') OR a = 4'),
	('SELECT count(*) FROM cc WHERE n NOT IN (1, 2, 3) OR a = 4'),
	('SELECT count(*) FROM cc WHERE n <> 99 OR a = 4'),
	('SELECT count(*) FROM cc WHERE (a = 1 AND b = 2) OR n <> 3'),
	('SELECT count(*) FROM cc WHERE n < 0 OR n <> 3'),
	('SELECT count(*) FROM cc WHERE a IN (1, 2) OR n <> 3'),
	('SELECT count(*) FROM cc WHERE NOT (tags @> ''{t3}'') OR a = 1'),
	('SELECT count(*) FROM cc WHERE NOT (tags @> ''{t3}'') OR tags IS NULL'),
	('SELECT count(*) FROM cc WHERE tags @> ''{t2}'' OR NOT (tags && ''{t3,t4}'')'),
	('SELECT count(*) FROM cc WHERE b = 1 AND (n <> 3 OR t = ''v4'')'),
	('SELECT count(*) FROM cc WHERE b = 1 AND (n <> 3 OR t = ''v4'') AND (a <> 2 OR n IS NULL)'),
	('SELECT count(*) FROM cc WHERE (n <> 3 OR t = ''v4'') AND n IS NOT NULL'),
	('SELECT count(n), count(t) FROM cc WHERE n <> 3 OR t = ''v4'''),
	('SELECT a, count(*) FROM cc WHERE n <> 3 OR t = ''v4'' GROUP BY a'),
	('SELECT a, count(*) FROM cc WHERE b = 1 AND (n <> 3 OR t = ''v4'') GROUP BY a'),
	('SELECT n, count(*) FROM cc WHERE n <> 3 OR a = 1 GROUP BY n'),
	('SELECT a, b, count(*) FROM cc WHERE n <> 3 OR t = ''v4'' GROUP BY a, b'),
	('SELECT a, count(*) FROM cc WHERE n <> 3 OR t = ''v4'' GROUP BY a HAVING count(*) > 2500'),
	('SELECT a, count(*) FROM cc WHERE n <> 3 OR t = ''v4'' GROUP BY a ORDER BY count(*) DESC LIMIT 2'),
	('SELECT count(DISTINCT a) FROM cc WHERE n <> 3 OR t = ''v4'''),
	('SELECT a, count(DISTINCT n) FROM cc WHERE n <> 3 OR t = ''v4'' GROUP BY a'),
	('SELECT count(*) FROM cc WHERE (n <> 3 OR t = ''v4'') AND b IN (1, 2, 3)'),
	('SELECT count(*) FROM cc WHERE (n <> 3 OR t = ''v4'') AND a >= 2 AND a < 6')
) v(q);

-- ---- what is declined: an arm that ANDs a complement with a selection, a
-- NOT of a query the posting sets only bound, more than three such ORs ----
SELECT q, cc_q(q) FROM (VALUES
	('SELECT count(*) FROM cc WHERE (a = 1 AND n <> 3) OR t = ''v4'''),
	('SELECT count(*) FROM cc WHERE NOT (tags @> ''{}'')'),
	('SELECT count(*) FROM cc WHERE NOT (tags @> ''{t3,NULL}'')'),
	('SELECT count(*) FROM cc WHERE (a <> 1 OR b = 1) AND (a <> 2 OR b = 2) AND (a <> 3 OR b = 3) AND (a <> 4 OR b = 4)')
) v(q);

-- ---- parameters: a generic plan, and a NULL value ----
SET plan_cache_mode = force_generic_plan;
PREPARE cc_d(int) AS SELECT count(*) FROM cc WHERE n IS DISTINCT FROM $1;
PREPARE cc_o(int, int) AS SELECT count(*) FROM cc WHERE a = $1 OR n <> $2;
PREPARE cc_g(int) AS SELECT a, count(*) FROM cc WHERE n <> $1 OR n IS NULL GROUP BY a ORDER BY a;
SELECT cc_plan('EXECUTE cc_o(2, 3)');
EXECUTE cc_d(3);
EXECUTE cc_d(NULL);
EXECUTE cc_o(2, 3);
EXECUTE cc_o(2, NULL);
EXECUTE cc_o(NULL, NULL);
EXECUTE cc_g(3);
EXECUTE cc_g(NULL);
SET pg_lion.enable_count_pushdown = off;
EXECUTE cc_d(3);
EXECUTE cc_d(NULL);
EXECUTE cc_o(2, 3);
EXECUTE cc_o(2, NULL);
EXECUTE cc_o(NULL, NULL);
EXECUTE cc_g(3);
EXECUTE cc_g(NULL);
RESET pg_lion.enable_count_pushdown;
DEALLOCATE cc_d;
DEALLOCATE cc_o;
DEALLOCATE cc_g;
RESET plan_cache_mode;

-- ---- a LATERAL rescan with a new value each time ----
SELECT cc_q('SELECT v.x, (SELECT count(*) FROM cc WHERE a = v.x OR n <> v.x) FROM (VALUES (1), (2), (3)) v(x)');

-- ---- a partitioned table: arms the bounds refute or imply ----
-- ---- complements beside filtered counts (DESIGN.md §10, "Filtered counts") ----
-- in the WHERE, which every run counts, and in a filter's condition, which
-- only its own run does
SELECT q, cc_q(q) FROM (VALUES
	('SELECT count(*) FILTER (WHERE b = 1), count(*) FROM cc WHERE n IS DISTINCT FROM 3'),
	('SELECT count(*) FILTER (WHERE b = 1), count(*) FROM cc WHERE a = 2 AND (b = 3 OR n <> 3)'),
	('SELECT count(*) FILTER (WHERE n IS DISTINCT FROM 3), count(*) FROM cc WHERE a = 2'),
	('SELECT count(*) FILTER (WHERE b = 1 OR n <> 3), count(*) FILTER (WHERE t = ''v1''), count(*) FROM cc WHERE a = 2'),
	('SELECT sum(CASE WHEN NOT (tags @> ''{t1}'') THEN 1 ELSE 0 END), count(*) FROM cc WHERE a = 2'),
	('SELECT b, count(*) FILTER (WHERE n IS DISTINCT FROM 3), count(*) FROM cc WHERE a = 2 GROUP BY b'),
	('SELECT b, count(*) FILTER (WHERE t = ''v1'' OR n <> 3), count(*) FROM cc WHERE a = 2 OR n IS NOT NULL GROUP BY b'),
	('SELECT a, b, count(*) FILTER (WHERE n IS DISTINCT FROM 3 OR t = ''v1''), count(*) FROM cc WHERE t IS NOT NULL GROUP BY a, b'),
	('SELECT a, b, t, count(*) FILTER (WHERE b = 1 OR n <> 3), count(*) FROM cc GROUP BY a, b, t')
) v(q);

CREATE TABLE ccp (a int, n int, t text) PARTITION BY RANGE (a);
CREATE TABLE ccp1 PARTITION OF ccp FOR VALUES FROM (0) TO (3);
CREATE TABLE ccp2 PARTITION OF ccp FOR VALUES FROM (3) TO (7);
INSERT INTO ccp SELECT a, n, t FROM cc;
CREATE INDEX ccp_a ON ccp USING lion (a);
CREATE INDEX ccp_n ON ccp USING lion (n);
CREATE INDEX ccp_t ON ccp USING lion (t);
VACUUM ANALYZE ccp;
SELECT q, cc_q(q) FROM (VALUES
	('SELECT count(*) FROM ccp WHERE n IS DISTINCT FROM 3'),
	('SELECT count(*) FROM ccp WHERE a < 3 OR n <> 3'),
	('SELECT count(*) FROM ccp WHERE a <> 4 OR t = ''v1'''),
	('SELECT count(*) FROM ccp WHERE n <> 3 OR t = ''v1'''),
	('SELECT a, count(*) FROM ccp WHERE n <> 3 OR t = ''v1'' GROUP BY a')
) v(q);
/*
 * A fourth complement OR is one more than the counts are doubled for, and so
 * a restriction no posting set answers - which every partition here implies
 * (a < 7): it is left out, all of it, and the clauses after it are sources of
 * their own, not arms of an OR it left behind.
 */
SELECT cc_plan('SELECT count(*) FROM ccp WHERE (n <> 1 OR t <> ''v1'') AND (n <> 2 OR t <> ''v2'') AND (n <> 3 OR t <> ''v3'') AND (a <> 100 OR n <> 4) AND n = 1 AND t = ''v4''');
SELECT q, cc_q(q) FROM (VALUES
	('SELECT count(*) FROM ccp WHERE (n <> 1 OR t <> ''v1'') AND (n <> 2 OR t <> ''v2'') AND (n <> 3 OR t <> ''v3'') AND (a <> 100 OR n <> 4) AND n = 1 AND t = ''v4'''),
	('SELECT a, count(*) FROM ccp WHERE (n <> 1 OR t <> ''v1'') AND (n <> 2 OR t <> ''v2'') AND (n <> 3 OR t <> ''v3'') AND (a <> 100 OR n <> 4) AND n = 1 AND t = ''v4'' GROUP BY a'),
	('SELECT count(*) FROM ccp WHERE (n <> 1 OR t <> ''v1'') AND (n <> 2 OR t <> ''v2'') AND (n <> 3 OR t <> ''v3'') AND (a <> 100 OR n <> 4) AND (n = 1 OR t = ''v4'')')
) v(q);

DROP TABLE ccp;
DROP FUNCTION cc_q(text);
DROP FUNCTION cc_plan(text);
DROP TABLE cc;
