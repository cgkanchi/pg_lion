-- GROUP BY ROLLUP, CUBE and GROUPING SETS over one table (DESIGN.md §10,
-- "Grouping sets"): the finest grouping counted by the LionCount node, and
-- core's grouping sets Agg over its rows below a LionJoinAgg, with every
-- aggregate rewritten over the counts; and the same for a GROUP BY whose
-- DISTINCT aggregates read columns outside it, counted with those columns
-- added ("DISTINCT aggregates over a finer grouping").  Each query is run with every other
-- scan disabled, so that a node that is built at all is the plan, and again
-- as a sequential scan with the pushdown off; the two answers are compared as
-- multisets in both directions.  The table has NULLs in the grouping columns,
-- and dead and updated rows that VACUUM has not seen.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE gs (id int, a int, b int NOT NULL, c int, s smallint, t text, u int,
				 m numeric)
	WITH (autovacuum_enabled = off);
INSERT INTO gs SELECT i,
	   CASE WHEN i % 13 = 0 THEN NULL ELSE i % 7 END,
	   i % 5,
	   CASE WHEN i % 4 = 0 THEN NULL ELSE i % 3 END,
	   (i % 9)::smallint,
	   CASE WHEN i % 11 = 0 THEN NULL ELSE 'v' || (i % 6) END,
	   i % 17,
	   -- 1.0 and 1.00 are one value to numeric's equality, two to its output
	   CASE i % 3 WHEN 0 THEN 1.0 WHEN 1 THEN 1.00 ELSE 2 END
  FROM generate_series(1, 20000) i;
CREATE INDEX gs_a ON gs USING lion (a);
CREATE INDEX gs_b ON gs USING lion (b);
CREATE INDEX gs_c ON gs USING lion (c);
CREATE INDEX gs_s ON gs USING lion (s);
CREATE INDEX gs_t ON gs USING lion (t);
CREATE INDEX gs_m ON gs USING lion (m);
VACUUM (FREEZE, ANALYZE) gs;
-- dead and updated rows the visibility map no longer covers
DELETE FROM gs WHERE id % 97 = 0;
UPDATE gs SET a = (a + 1) % 7, c = NULL WHERE id % 89 = 0;
UPDATE gs SET t = NULL, s = 0 WHERE id % 83 = 0;

/*
 * gs_q() runs q with every scan but the node's disabled and as a sequential
 * scan with the pushdown off, and compares the two: 'counted, N rows' when
 * the plan counted the finest grouping with the node, and the answers agree.
 */
CREATE FUNCTION gs_q(q text) RETURNS text
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
	EXECUTE format('CREATE TEMP TABLE gs_on AS SELECT s::text AS r FROM (%s) s', q);

	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE format('CREATE TEMP TABLE gs_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM gs_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM gs_on EXCEPT ALL SELECT * FROM gs_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM gs_off EXCEPT ALL SELECT * FROM gs_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE gs_on, gs_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows', CASE WHEN used THEN 'counted' ELSE 'not counted' END,
				  nrows);
END $$;

-- the plan, with the node the only scan allowed
CREATE FUNCTION gs_plan(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		RETURN NEXT ln;
	END LOOP;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
END $$;

-- ---- the plans ----
-- one rollup: the finest grouping sorted once
SELECT gs_plan('SELECT a, b, count(*), GROUPING(a, b) FROM gs GROUP BY ROLLUP (a, b)');
-- a cube is two rollups, the second sorted by the Agg itself
SELECT gs_plan('SELECT a, b, count(*) FROM gs GROUP BY CUBE (a, b)');
-- a HAVING over the counts, and an ORDER BY and LIMIT over the sets, which
-- the finest grouping is not cut to
SELECT gs_plan('SELECT a, b, count(*) FROM gs WHERE c = 1 GROUP BY GROUPING SETS ((a), (b), ())
				HAVING count(*) > 10 ORDER BY count(*) DESC, a, b LIMIT 4');

-- ---- answers, against the sequential plan ----
SELECT q, gs_q(q) FROM (VALUES
	('SELECT a, b, count(*), GROUPING(a, b) FROM gs GROUP BY ROLLUP (a, b)'),
	('SELECT a, b, count(*), count(a), count(b), count(1), GROUPING(b) FROM gs GROUP BY CUBE (a, b)'),
	('SELECT a, t, count(*), count(t), GROUPING(a), GROUPING(t) FROM gs GROUP BY GROUPING SETS ((a), (t), ())'),
	('SELECT a, b, count(*) FROM gs WHERE c = 1 GROUP BY GROUPING SETS ((a), (b), ()) HAVING count(*) > 10'),
	('SELECT a, b, count(*) FROM gs WHERE c = 1 GROUP BY GROUPING SETS ((a), (b), ())
	  HAVING count(*) > 10 ORDER BY count(*) DESC, a, b LIMIT 4'),
	('SELECT a, b, sum(b), sum(s), min(a), max(t), bool_and(b > 0), bool_or(a = 3) FROM gs GROUP BY ROLLUP (a, b, s, t)'),
	('SELECT a, count(*) FILTER (WHERE b > 2), count(a) FILTER (WHERE b < 3), sum(b) FILTER (WHERE a <> 2) FROM gs GROUP BY ROLLUP (a, b)'),
	('SELECT a, count(DISTINCT b), sum(DISTINCT b), string_agg(DISTINCT t, '','' ORDER BY t) FROM gs GROUP BY GROUPING SETS ((a, b, t), (a))'),
	('SELECT a, b, count(*) FROM gs GROUP BY a, ROLLUP (b, s)'),
	('SELECT b, count(*) FROM gs GROUP BY GROUPING SETS ((b), (b), ())'),
	('SELECT a, b, count(*) * 2 + 1, round(100.0 * count(*) / 20000, 1) FROM gs GROUP BY ROLLUP (a, b)'),
	('SELECT a, b, count(*), sum(count(*)) OVER (PARTITION BY GROUPING(a, b)) FROM gs GROUP BY ROLLUP (a, b)'),
	('SELECT GROUPING(a), count(*) FROM gs GROUP BY ROLLUP (a) HAVING GROUPING(a) = 1 OR count(*) > 2700'),
	('SELECT a, b, count(*) FROM gs WHERE a = 3 GROUP BY ROLLUP (a, b)'),
	('SELECT a, b, count(*) FROM gs WHERE a IN (1, 4) AND t IS NOT NULL GROUP BY CUBE (a, b)'),
	-- no row matches: the empty grouping set is one row, of count 0
	('SELECT a, count(*), count(a), max(a) FROM gs WHERE a = 100 GROUP BY ROLLUP (a)')
) v(q);

-- ---- declined: an aggregate the counts do not answer, or a grouping the
-- node does not count ----
SELECT q, gs_q(q) FROM (VALUES
	('SELECT a, b, avg(b) FROM gs GROUP BY ROLLUP (a, b)'),
	('SELECT a, b, count(id) FROM gs GROUP BY ROLLUP (a, b)'),
	('SELECT a, b, sum(u) FROM gs GROUP BY ROLLUP (a, b)'),
	('SELECT a, array_agg(b ORDER BY b) FROM gs GROUP BY ROLLUP (a, b)'),
	('SELECT a, bit_xor(b) FROM gs GROUP BY ROLLUP (a, b)'),
	('SELECT a, count(*) FILTER (WHERE random() < 2) FROM gs GROUP BY ROLLUP (a, b)'),
	('SELECT a + 1, count(*) FROM gs GROUP BY ROLLUP (a + 1)'),
	('SELECT a, u, count(*) FROM gs GROUP BY ROLLUP (a, u)'),
	('SELECT DISTINCT a, count(*) FROM gs GROUP BY ROLLUP (a, b)'),
	-- an expression of a column whose equal values print differently
	('SELECT a, count(DISTINCT m::text) FROM gs GROUP BY ROLLUP (a, m)')
) v(q);

-- ---- DISTINCT aggregates over a finer grouping ----
-- count(DISTINCT c) GROUP BY a, b: GROUP BY a, b, c counted, and the Agg
-- reads its rows sorted by a, b and c
SELECT gs_plan('SELECT a, b, count(DISTINCT c) FROM gs GROUP BY a, b');
SELECT q, gs_q(q) FROM (VALUES
	('SELECT a, b, count(DISTINCT c) FROM gs GROUP BY a, b'),
	('SELECT a, b, count(*), count(DISTINCT c), count(c), sum(c), min(c) FROM gs GROUP BY a, b'),
	('SELECT a, count(DISTINCT c), count(DISTINCT t) FROM gs GROUP BY a'),
	('SELECT a, b, count(DISTINCT c) FROM gs WHERE s IN (1, 3, 4) GROUP BY a, b HAVING count(DISTINCT c) > 1'),
	('SELECT a, b, string_agg(DISTINCT t, '','' ORDER BY t) FROM gs GROUP BY a, b'),
	('SELECT b, count(DISTINCT c) FILTER (WHERE a > 2), count(DISTINCT c % 2) FROM gs GROUP BY b'),
	('SELECT a, b, count(DISTINCT c) FROM gs GROUP BY a, b ORDER BY 3 DESC, 1, 2 LIMIT 5'),
	('SELECT a, b, sum(DISTINCT s) FROM gs WHERE t IN (''v1'', ''v2'') GROUP BY a, b')
) v(q);
-- without the presorted DISTINCT the Agg sorts each group's values itself
SET enable_presorted_aggregate = off;
SELECT gs_q('SELECT a, b, count(DISTINCT c), string_agg(DISTINCT t, '','' ORDER BY t) FROM gs GROUP BY a, b');
RESET enable_presorted_aggregate;
-- declined: a column without a lion index, an aggregate the counts do not
-- answer, an expression of a column whose equal values print differently
SELECT q, gs_q(q) FROM (VALUES
	('SELECT a, b, count(DISTINCT u) FROM gs GROUP BY a, b'),
	('SELECT a, b, count(DISTINCT c), avg(c) FROM gs GROUP BY a, b'),
	('SELECT a, b, count(DISTINCT c), max(u) FROM gs GROUP BY a, b'),
	('SELECT a, b, count(DISTINCT m::text) FROM gs GROUP BY a, b')
) v(q);

-- ---- the finest grouping of a partitioned table, below a Finalize Agg ----
CREATE TABLE gsp (id int, a int, b int) PARTITION BY RANGE (id);
CREATE TABLE gsp1 PARTITION OF gsp FOR VALUES FROM (MINVALUE) TO (10000);
CREATE TABLE gsp2 PARTITION OF gsp FOR VALUES FROM (10000) TO (MAXVALUE);
INSERT INTO gsp SELECT i, CASE WHEN i % 13 = 0 THEN NULL ELSE i % 7 END, i % 5
  FROM generate_series(1, 20000) i;
CREATE INDEX gsp_a ON gsp USING lion (a);
CREATE INDEX gsp_b ON gsp USING lion (b);
VACUUM (FREEZE, ANALYZE) gsp;
DELETE FROM gsp WHERE id % 97 = 0;
SELECT p FROM gs_plan('SELECT a, b, count(*) FROM gsp GROUP BY CUBE (a, b)') p
 WHERE p ~ '(LionJoinAgg|Aggregate|LionCount)';
SELECT gs_q('SELECT a, b, count(*), sum(b) FROM gsp GROUP BY CUBE (a, b)');

-- ---- the Agg's costs, in a backend that has planned nothing yet ----
-- The Agg over the finer grouping's rows is priced with aggregate costs the
-- node computes, not core's GroupPathExtraData.agg_final_costs: core fills
-- those only when it builds partially aggregated paths, which grouping sets
-- and a DISTINCT aggregate never allow, and leaves them as its stack held
-- them otherwise - which in a fresh backend on PostgreSQL 16 priced the Agg
-- at 1e108, and the plan was core's.  gs_cost() reports whether the plan's
-- cost is the count's below it plus a little for the Agg over its rows.
CREATE FUNCTION gs_cost(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	j json;
	top float8;
	cnt float8;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	top := (j -> 0 -> 'Plan' ->> 'Total Cost')::float8;
	cnt := (jsonb_path_query_first(j::jsonb,
		'$.** ? (@."Custom Plan Provider" == "LionCount")."Total Cost"'))::text::float8;
	IF cnt IS NULL THEN
		RETURN 'not counted';
	END IF;
	IF top >= cnt AND top < cnt + 1000 THEN
		RETURN 'counted, at the count''s cost and the Agg''s';
	END IF;
	RETURN format('counted, at %s over a count at %s', top, cnt);
END $$;
\c -
SET max_parallel_workers_per_gather = 0;
SELECT gs_cost('SELECT a, b, sum(DISTINCT s) FROM gs WHERE t IN (''v1'', ''v2'') GROUP BY a, b');
SELECT gs_cost('SELECT a, b, count(*), GROUPING(a, b) FROM gs GROUP BY ROLLUP (a, b)');
SELECT gs_q('SELECT a, b, sum(DISTINCT s) FROM gs WHERE t IN (''v1'', ''v2'') GROUP BY a, b');

DROP TABLE gs, gsp;
DROP FUNCTION gs_q(text), gs_plan(text), gs_cost(text);
