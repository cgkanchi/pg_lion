-- Partial lion indexes in the count pushdown (DESIGN.md §45): a count whose
-- restrictions imply a partial index's predicate reads that index, and
-- leaves out the restrictions the predicate implies - but only where every
-- row it can count is one of the predicate's ("held to the predicate's
-- rows").  Every query is compared, as a multiset both ways round, with a
-- sequential scan with the pushdown off, on random data with dead and
-- updated rows on pages the visibility map no longer vouches for.  cp_q()
-- names the custom scans the plan used, the lion indexes it read and the
-- restrictions it left out; a query the partial index cannot answer exactly
-- shows the whole index's plan, or the ordinary one.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

SELECT setseed(0.45);
-- three dense columns (on, st, kind) every query of a workload filters on,
-- the others to count by; w is indexed nowhere, y only by the whole index
CREATE TABLE pc (id int NOT NULL, act bool, st text, kind text, a int, b int,
				 g int, x int, y int, w int, tags int[])
	WITH (autovacuum_enabled = off);
INSERT INTO pc
SELECT i, random() < 0.85, CASE WHEN random() < 0.8 THEN 'on' ELSE 'off' END,
	   CASE WHEN random() < 0.9 THEN 'k0' ELSE 'k' || (1 + (random() * 3)::int) END,
	   CASE WHEN random() < 0.05 THEN NULL ELSE (random() * 19)::int END,
	   (random() * 9)::int, (random() * 4)::int,
	   CASE WHEN random() < 0.1 THEN NULL ELSE (random() * 99)::int END,
	   (random() * 5)::int, (random() * 9)::int,
	   ARRAY[(random() * 9)::int, (random() * 9)::int]
  FROM generate_series(1, 30000) i;

-- a whole index on every column a query names, and partial ones beside it:
-- one on the dense defaults (indexed columns), one on its own key column,
-- one on a column no index has, one that excludes the NULLs and one of a
-- multi-key column
CREATE INDEX pc_full ON pc USING lion (act, st, kind, a, b, g, x, y);
CREATE INDEX pc_p1 ON pc USING lion (a, b, g, x)
	WHERE act AND st = 'on' AND kind = 'k0';
CREATE INDEX pc_px ON pc USING lion (x, g) WHERE x > 10;
CREATE INDEX pc_pw ON pc USING lion (b, g) WHERE w > 5;
CREATE INDEX pc_pn ON pc USING lion (x) WHERE x IS NOT NULL;
CREATE INDEX pc_pt ON pc USING lion (tags) WHERE act;
CREATE INDEX pc_pk ON pc USING lion (b, g) WHERE kind <> 'k0';
VACUUM (FREEZE, ANALYZE) pc;

-- the rows a CREATE INDEX reports are rows, not postings (DESIGN.md §18): a
-- partial index's reltuples is what the planner sizes it by
SELECT c.relname, c.reltuples::int8 =
	   (SELECT count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0') AS is_rows
  FROM pg_class c WHERE c.relname = 'pc_p1';

-- dead rows, and rows moved into and out of the predicates, since the VACUUM:
-- on the first pages, which the visibility map no longer vouches for, and
-- one beyond them
DELETE FROM pc WHERE id <= 5000 AND id % 17 = 0;
UPDATE pc SET act = NOT act WHERE id <= 5000 AND id % 13 = 0;
UPDATE pc SET st = 'off', a = (a + 1) % 20 WHERE id <= 5000 AND id % 11 = 0;
UPDATE pc SET x = CASE WHEN x > 10 THEN 3 ELSE 50 END WHERE id <= 5000 AND id % 7 = 0;
UPDATE pc SET kind = 'k0', w = 9 WHERE id <= 5000 AND id % 5 = 2;
UPDATE pc SET act = true, st = 'on', kind = 'k0', x = NULL WHERE id = 29000;

CREATE FUNCTION cp_q(q text, nojoins boolean DEFAULT false) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	nodes text[] := '{}';
	idxs text[] := '{}';
	implied text := NULL;
	node text;
	m text[];
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	IF nojoins THEN
		PERFORM set_config('enable_hashjoin', 'off', true);
		PERFORM set_config('enable_mergejoin', 'off', true);
		PERFORM set_config('enable_nestloop', 'off', true);
	END IF;
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		node := substring(ln from 'Custom Scan \((\w+)\)');
		IF node IS NOT NULL AND NOT node = ANY (nodes) THEN
			nodes := nodes || node;
		END IF;
		IF ln ~ 'Lion Indexes:' THEN
			FOR m IN SELECT regexp_matches(ln, '(?:: |, )(\w+)(?:\.\w+)? \(', 'g') LOOP
				IF NOT m[1] = ANY (idxs) THEN
					idxs := idxs || m[1];
				END IF;
			END LOOP;
		END IF;
		IF ln ~ 'Partial Indexes:' THEN
			FOR m IN SELECT regexp_matches(substring(ln from 'Partial Indexes: (.*)$'),
										   '(\w+)', 'g') LOOP
				IF NOT m[1] = ANY (idxs) THEN
					idxs := idxs || m[1];
				END IF;
			END LOOP;
		END IF;
		IF ln ~ 'Implied by Index Predicate:' THEN
			implied := substring(ln from 'Implied by Index Predicate: (.*)$');
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE cp_on AS SELECT s::text AS r FROM (%s) s', q);

	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_nestloop', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE format('CREATE TEMP TABLE cp_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM cp_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM cp_on EXCEPT ALL SELECT * FROM cp_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM cp_off EXCEPT ALL SELECT * FROM cp_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE cp_on, cp_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	SELECT array_agg(i ORDER BY i) INTO idxs FROM unnest(idxs) i;
	RETURN format('%s%s%s, %s rows',
				  CASE WHEN cardinality(nodes) = 0 THEN 'no lion node'
					   ELSE array_to_string(nodes, ' + ') END,
				  CASE WHEN cardinality(idxs) > 0
					   THEN ' [' || array_to_string(idxs, ' ') || ']' ELSE '' END,
				  CASE WHEN implied IS NOT NULL
					   THEN ' implied: ' || implied ELSE '' END,
				  nrows);
END $$;

-- ---- the predicate exactly, implied by stronger clauses, or not implied ----

-- (a) the dense defaults are the predicate: left out, and the count is a
-- sum over the partial index's entries, or its sets
SELECT cp_q($$SELECT count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0'$$);
SELECT cp_q($$SELECT count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0' AND a = 3$$);
SELECT cp_q($$SELECT count(*) FROM pc WHERE kind = 'k0' AND st = 'on' AND act AND a IN (1, 4, 7) AND b = 2$$);
SELECT cp_q($$SELECT count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0' AND x >= 40 AND x < 60$$);
-- (b) implied by stronger clauses: x > 20 under `WHERE x > 10`, which is
-- not left out (the predicate does not imply it); the walk of x's entries
-- above 20 is the partial index's
SELECT cp_q($$SELECT count(*) FROM pc WHERE x > 20$$);
SELECT cp_q($$SELECT count(*) FROM pc WHERE x > 20 AND g = 1$$);
SELECT cp_q($$SELECT count(*) FROM pc WHERE x = 30$$);
SELECT cp_q($$SELECT count(*) FROM pc WHERE x IN (5, 15, 25) AND x > 12$$);
-- (c) not implied: x > 5 is not x > 10, and two of the three defaults are
-- not the predicate; the whole index answers
SELECT cp_q($$SELECT count(*) FROM pc WHERE x > 5 AND g = 1$$);
SELECT cp_q($$SELECT count(*) FROM pc WHERE act AND st = 'on' AND a = 3$$);
SELECT cp_q($$SELECT count(*) FROM pc WHERE (act OR st = 'on') AND kind = 'k0' AND a = 3$$);
-- the predicate on a column no index has: w > 5 left out; w > 7 implies it
-- and is not, and no posting set answers it: a residual clause, tested on
-- the heap of the rows the others select
SELECT cp_q($$SELECT count(*) FROM pc WHERE w > 5 AND b = 4$$);
SELECT cp_q($$SELECT g, count(*) FROM pc WHERE w > 5 GROUP BY g$$);
SELECT cp_q($$SELECT count(*) FROM pc WHERE w > 7 AND b = 4$$);
-- ... and a residual clause beside a left-out predicate
SELECT cp_q($$SELECT count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0' AND a = 2 AND w > 3$$);

-- ---- NULLs, negations and complements ----

SELECT cp_q($$SELECT count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0' AND a IS NULL$$);
SELECT cp_q($$SELECT count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0' AND x IS NOT NULL$$);
SELECT cp_q($$SELECT count(x) FROM pc WHERE act AND st = 'on' AND kind = 'k0'$$);
SELECT cp_q($$SELECT count(a) FROM pc WHERE act AND st = 'on' AND kind = 'k0' AND b = 2$$);
-- count(x) where the predicate proves x is never NULL: every row's
SELECT cp_q($$SELECT count(x) FROM pc WHERE x > 10$$);
SELECT cp_q($$SELECT g, count(x) FROM pc WHERE x > 10 GROUP BY g$$);
SELECT cp_q($$SELECT count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0' AND a <> 3$$);
SELECT cp_q($$SELECT count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0' AND a IS DISTINCT FROM 3$$);
SELECT cp_q($$SELECT count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0' AND a NOT IN (1, 2, 3)$$);
SELECT cp_q($$SELECT count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0' AND b = 1 AND (a IS DISTINCT FROM 3 OR g = 1)$$);
SELECT cp_q($$SELECT count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0' AND (a = 1 OR b = 2)$$);
SELECT cp_q($$SELECT count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0' AND (a = 1 OR y = 2)$$);
SELECT cp_q($$SELECT count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0' AND (a = 1 OR y = 2) AND g = 3$$);
-- under the predicate, a `<>` on its own column, and a predicate that is a
-- `<>`, which an IN list of other values implies
SELECT cp_q($$SELECT count(*) FROM pc WHERE x > 10 AND x <> 50$$);
SELECT cp_q($$SELECT count(*) FROM pc WHERE kind <> 'k0' AND b = 3$$);
SELECT cp_q($$SELECT g, count(*) FROM pc WHERE kind IN ('k1', 'k2') AND b = 3 GROUP BY g$$);
-- x <> 3 implies `x IS NOT NULL`, whose partial index has x's sets less
-- the NULL entry it does not have: read for x <> 3 beside an equality the
-- whole index answers, the count would take the rows with x NULL for rows
-- `<>` keeps.  Not held to the predicate's rows, so not read.
SELECT cp_q($$SELECT count(*) FROM pc WHERE a = 1 AND x <> 3$$);
SELECT cp_q($$SELECT count(*) FROM pc WHERE x <> 3$$);
SELECT cp_q($$SELECT g, count(*) FROM pc WHERE x <> 3 GROUP BY g$$);
-- a multi-key query may turn out to narrow nothing: never what holds the
-- count, and with nothing else to, the partial index is not read
SELECT cp_q($$SELECT count(*) FROM pc WHERE act AND tags @> '{3}'$$);

-- ---- GROUP BY ----

SELECT cp_q($$SELECT g, count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0' GROUP BY g$$);
SELECT cp_q($$SELECT a, count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0' AND b = 3 GROUP BY a$$);
SELECT cp_q($$SELECT a, b, count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0' GROUP BY a, b$$);
SELECT cp_q($$SELECT a, b, g, count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0' AND x < 50 GROUP BY a, b, g$$);
-- grouped by a column only the whole index has: held by the partial
-- index's NULL entry, by its columns in the decoded walk - a row outside the
-- predicate has no value there - or by nothing, and left to the whole index
SELECT cp_q($$SELECT y, count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0' AND a IS NULL GROUP BY y$$);
SELECT cp_q($$SELECT a, y, g, count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0' GROUP BY a, y, g$$);
SELECT cp_q($$SELECT y, b, x, count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0' AND x IS NULL GROUP BY y, b, x$$);
SELECT cp_q($$SELECT y, count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0' GROUP BY y$$);
-- grouped by the predicate's own column, and by one the restrictions pin
SELECT cp_q($$SELECT x, count(*) FROM pc WHERE x > 20 GROUP BY x$$);
SELECT cp_q($$SELECT x, count(*) FROM pc WHERE x > 10 GROUP BY x$$);
SELECT cp_q($$SELECT act, st, count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0' AND g = 2 GROUP BY act, st$$);
SELECT cp_q($$SELECT st, a, count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0' GROUP BY st, a$$);
-- a GROUP BY folded to a constant, its column printed and not
SELECT cp_q($$SELECT kind, count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0' AND a = 5 GROUP BY kind$$);
SELECT cp_q($$SELECT count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0' AND a = 5 GROUP BY kind$$);
-- HAVING, the top k, coalesce
SELECT cp_q($$SELECT a, count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0' GROUP BY a HAVING count(*) > 900$$);
SELECT cp_q($$SELECT a, count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0' GROUP BY a ORDER BY count(*) DESC, a LIMIT 3$$);
SELECT cp_q($$SELECT coalesce(a, -1), count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0' GROUP BY coalesce(a, -1)$$);
-- SELECT DISTINCT is its keys' GROUP BY
SELECT cp_q($$SELECT DISTINCT a, g FROM pc WHERE act AND st = 'on' AND kind = 'k0' AND b = 1$$);

-- ---- filtered counts, and count(DISTINCT) ----

-- a sum over every row counts a filter with a set of its own by that set:
-- the partial index's (a = 1) holds that run, the whole index's (y = 1)
-- does not, and the count is left to the whole indexes
SELECT cp_q($$SELECT count(*), count(*) FILTER (WHERE a = 1), count(*) FILTER (WHERE b IN (1, 2)) FROM pc WHERE act AND st = 'on' AND kind = 'k0'$$);
SELECT cp_q($$SELECT count(*), count(*) FILTER (WHERE y = 1) FROM pc WHERE act AND st = 'on' AND kind = 'k0'$$);
SELECT cp_q($$SELECT count(*), count(*) FILTER (WHERE a <> 1) FROM pc WHERE act AND st = 'on' AND kind = 'k0'$$);
SELECT cp_q($$SELECT g, count(*) FILTER (WHERE y = 1), count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0' GROUP BY g$$);
SELECT cp_q($$SELECT count(DISTINCT a) FROM pc WHERE act AND st = 'on' AND kind = 'k0'$$);
SELECT cp_q($$SELECT g, count(DISTINCT b) FROM pc WHERE act AND st = 'on' AND kind = 'k0' GROUP BY g$$);
SELECT cp_q($$SELECT count(DISTINCT x) FROM pc WHERE x > 30$$);
SELECT cp_q($$SELECT count(DISTINCT y) FROM pc WHERE act AND st = 'on' AND kind = 'k0' AND b = 1$$);

-- ---- what EXPLAIN says ----

EXPLAIN (COSTS OFF)
SELECT g, count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0' AND b = 3 GROUP BY g;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM pc WHERE x > 20;
-- switched off, the whole index
SET pg_lion.enable_count_partial = off;
EXPLAIN (COSTS OFF)
SELECT g, count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0' AND b = 3 GROUP BY g;
RESET pg_lion.enable_count_partial;

-- ---- prepared statements: a Param proves no predicate ----

PREPARE cp_x(int) AS SELECT count(*) FROM pc WHERE x > $1 AND g = 1;
PREPARE cp_st(text) AS SELECT g, count(*) FROM pc WHERE act AND st = $1 AND kind = 'k0' GROUP BY g ORDER BY g;
PREPARE cp_a(int) AS SELECT count(*) FROM pc WHERE act AND st = 'on' AND kind = 'k0' AND a = $1;
SET plan_cache_mode = force_generic_plan;
EXPLAIN (COSTS OFF) EXECUTE cp_x(20);
EXECUTE cp_x(20);
EXECUTE cp_x(5);
EXPLAIN (COSTS OFF) EXECUTE cp_st('on');
EXECUTE cp_st('on');
EXPLAIN (COSTS OFF) EXECUTE cp_a(3);
EXECUTE cp_a(3);
EXECUTE cp_a(NULL);
SET plan_cache_mode = force_custom_plan;
EXPLAIN (COSTS OFF) EXECUTE cp_x(20);
EXECUTE cp_x(20);
EXECUTE cp_x(5);
EXECUTE cp_st('on');
RESET plan_cache_mode;
SET pg_lion.enable_count_pushdown = off;
EXECUTE cp_x(20);
EXECUTE cp_x(5);
EXECUTE cp_st('on');
EXECUTE cp_a(3);
RESET pg_lion.enable_count_pushdown;
DEALLOCATE cp_x;
DEALLOCATE cp_st;
DEALLOCATE cp_a;

-- ---- the FK-side join: the partial index on the fact side, and on the
-- dimension's ----

CREATE TABLE pcd (k int PRIMARY KEY, region text NOT NULL, active bool NOT NULL);
INSERT INTO pcd SELECT i, 'r' || (i % 4), i % 3 <> 0 FROM generate_series(0, 19) i;
CREATE INDEX pcd_region ON pcd USING lion (region) WHERE active;
ANALYZE pcd;
SELECT cp_q($$SELECT d.region, count(*) FROM pc f JOIN pcd d ON f.a = d.k WHERE f.act AND f.st = 'on' AND f.kind = 'k0' GROUP BY d.region$$, true);
SELECT cp_q($$SELECT d.region, count(*) FROM pc f JOIN pcd d ON f.a = d.k WHERE f.act AND f.st = 'on' AND f.kind = 'k0' AND f.b = 2 GROUP BY d.region$$, true);
SELECT cp_q($$SELECT d.region, count(*) FROM pc f JOIN pcd d ON f.a = d.k WHERE f.act AND f.st = 'on' AND f.kind = 'k0' AND d.active GROUP BY d.region$$, true);
SELECT cp_q($$SELECT d.region, count(*) FROM pc f JOIN pcd d ON f.a = d.k WHERE f.act AND f.st = 'on' AND f.kind = 'k0' AND d.active AND d.region = 'r1' GROUP BY d.region$$, true);
SELECT cp_q($$SELECT d.region, f.g, count(*) FROM pc f JOIN pcd d ON f.a = d.k WHERE f.act AND f.st = 'on' AND f.kind = 'k0' GROUP BY d.region, f.g$$, true);
SELECT cp_q($$SELECT d.region, count(f.id) FROM pcd d LEFT JOIN pc f ON f.a = d.k AND f.act AND f.st = 'on' AND f.kind = 'k0' GROUP BY d.region$$, true);
SELECT cp_q($$SELECT count(*) FROM pcd d WHERE EXISTS (SELECT 1 FROM pc f WHERE f.a = d.k AND f.act AND f.st = 'on' AND f.kind = 'k0' AND f.b = 9 AND f.g = 4 AND f.x = 7)$$);
SELECT cp_q($$SELECT count(*) FROM pcd d WHERE NOT EXISTS (SELECT 1 FROM pc f WHERE f.a = d.k AND f.act AND f.st = 'on' AND f.kind = 'k0' AND f.b = 9 AND f.g = 4 AND f.x = 7)$$);
-- ... the fact side's rows the partial index's entries of the key alone
SELECT cp_q($$SELECT count(*) FROM pcd d WHERE NOT EXISTS (SELECT 1 FROM pc f WHERE f.a = d.k AND f.act AND f.st = 'on' AND f.kind = 'k0')$$);
SELECT cp_q($$SELECT count(*) FROM pcd d WHERE EXISTS (SELECT 1 FROM pc f WHERE f.a = d.k AND f.x > 10)$$);
-- the fk read from a whole index, a fact filter from the partial one
SELECT cp_q($$SELECT d.region, count(*) FROM pc f JOIN pcd d ON f.y = d.k WHERE f.act AND f.st = 'on' AND f.kind = 'k0' AND f.a = 4 GROUP BY d.region$$, true);
-- ... and from nothing that holds the count: the whole index
SELECT cp_q($$SELECT d.region, count(*) FROM pc f JOIN pcd d ON f.y = d.k WHERE f.act AND f.st = 'on' AND f.kind = 'k0' GROUP BY d.region$$, true);

-- ---- a partitioned table ----

CREATE TABLE pcp (id int NOT NULL, act bool, st text NOT NULL, a int, g int, x int)
	PARTITION BY LIST (st);
CREATE TABLE pcp_on PARTITION OF pcp FOR VALUES IN ('on');
CREATE TABLE pcp_off PARTITION OF pcp FOR VALUES IN ('off');
CREATE TABLE pcp_other PARTITION OF pcp DEFAULT;
ALTER TABLE pcp_on SET (autovacuum_enabled = off);
ALTER TABLE pcp_off SET (autovacuum_enabled = off);
ALTER TABLE pcp_other SET (autovacuum_enabled = off);
INSERT INTO pcp SELECT id, act, CASE WHEN id % 7 = 0 THEN 'x' || (id % 3) ELSE st END,
	   a, g, x FROM pc;
CREATE INDEX pcp_full ON pcp USING lion (act, a, g, x);
CREATE INDEX pcp_act ON pcp USING lion (a, g) WHERE act;
-- one partition's own partial index, which its bounds alone imply
CREATE INDEX pcp_on_x ON pcp_on USING lion (x, g) WHERE st = 'on' AND x > 10;
VACUUM (FREEZE, ANALYZE) pcp;
UPDATE pcp SET act = NOT act WHERE id <= 5000 AND id % 11 = 0;
DELETE FROM pcp WHERE id <= 5000 AND id % 13 = 0;

SELECT cp_q($$SELECT count(*) FROM pcp WHERE act$$);
SELECT cp_q($$SELECT count(*) FROM pcp WHERE act AND a = 4$$);
SELECT cp_q($$SELECT g, count(*) FROM pcp WHERE act GROUP BY g$$);
SELECT cp_q($$SELECT a, count(*) FROM pcp WHERE act AND st IN ('on', 'off') GROUP BY a$$);
SELECT cp_q($$SELECT count(*) FROM pcp WHERE act AND a <> 4$$);
-- a partition with no partial index whose predicate the query implies
-- reads its whole index, and nothing is left out
SELECT cp_q($$SELECT count(*) FROM pcp WHERE st = 'on' AND x > 20 AND g = 2$$);
SELECT cp_q($$SELECT count(*) FROM pcp WHERE x > 20 AND g = 2$$);
EXPLAIN (COSTS OFF) SELECT g, count(*) FROM pcp WHERE act AND a = 4 GROUP BY g;
-- ... and in an FK-side join, the fact table partitioned
SELECT cp_q($$SELECT d.region, count(*) FROM pcp f JOIN pcd d ON f.a = d.k WHERE f.act GROUP BY d.region$$, true);

DROP TABLE pcp;
DROP TABLE pcd;
DROP TABLE pc;
DROP FUNCTION cp_q(text, boolean);
