-- FINDING (claude/lion-cost-margin 58db48c, DESIGN.md §39; plan choice only,
-- no answer changes): the margin's exemption for a count whose cheapest
-- competitor is the AM's own scan of a lion index does not reach a
-- partitioned table.
--
-- lion_competitor_margin() offers a count at no margin when the cheapest core
-- path is one of the AM's own scans (lion_path_is_lion_scan()), since both
-- prices are then lion's model's.  lion_path_is_lion_scan() returns false at
-- an AppendPath (src/lion_plan_units.c:438-440, with the joins), so the same
-- count over a partitioned table - its competitor an Aggregate over an Append
-- of the same bitmap scans of the partitions' lion indexes - is offered at
-- pg_lion.pushdown_margin, and at 0.01 it loses to core's plan over lion's
-- own scans where the plain table's count keeps its price.  DEBUG2 says
-- "priced against a lion bitmap heap scan ... margin 1" for the plain table
-- and "priced against a bitmap heap scan ... margin 0.01" for the partitioned
-- one.
--
-- 75376e0 follows the Append into its subpaths and asks that every one be a
-- lion scan.  That fixes the hash-partitioned table, but not a range-
-- partitioned one with an empty partition that VACUUM or ANALYZE has seen - an
-- empty DEFAULT, or one made ahead of the rows it is for: core's cheapest path
-- there is a Seq Scan of no pages (cost 0.00), so the Append is not all lion
-- scans and the count is still offered at the margin.  At margin 1 the count
-- costs 19.86, at 0.01 it is offered at 1986 against core's 671.16 and core's
-- plan is chosen; a partition of 20 rows does the same (18.85 against 672.49).
-- An empty partition no VACUUM or ANALYZE has seen is priced as ten pages and
-- scanned through its lion index, so it keeps the exemption.
--
-- Expected (the exemption applied alike): the count's cost is the same at
-- every margin on every table.  The script prints its verdict; on a build
-- without pg_lion.pushdown_margin it says so and stops.  The session commits
-- synchronously, so that the VACUUM right after the load can set the hint
-- bits and the visibility map (a cluster with synchronous_commit = off may
-- not have flushed the load's commit yet, and lion's count is then priced
-- with every heap page to read; the verdict is the same, the costs not).
\set VERBOSITY terse
SET client_min_messages = warning;
SET synchronous_commit = on;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
SELECT current_setting('pg_lion.pushdown_margin', true) IS NULL AS no_margin \gset
\if :no_margin
\echo 'cost_partition_ownscan: this build has no pg_lion.pushdown_margin; skipped'
\quit
\endif
DROP TABLE IF EXISTS cpo_plain, cpo_part, cpo_range;
CREATE TABLE cpo_plain (id int, g int, c int);
CREATE TABLE cpo_part (id int, g int, c int) PARTITION BY HASH (id);
CREATE TABLE cpo_part0 PARTITION OF cpo_part FOR VALUES WITH (MODULUS 3, REMAINDER 0);
CREATE TABLE cpo_part1 PARTITION OF cpo_part FOR VALUES WITH (MODULUS 3, REMAINDER 1);
CREATE TABLE cpo_part2 PARTITION OF cpo_part FOR VALUES WITH (MODULUS 3, REMAINDER 2);
INSERT INTO cpo_plain SELECT i, i % 50, (hashint4(i) & 2147483647) % 4 FROM generate_series(1, 90000) i;
CREATE TABLE cpo_range (id int, g int, c int) PARTITION BY RANGE (id);
CREATE TABLE cpo_range0 PARTITION OF cpo_range FOR VALUES FROM (1) TO (30001);
CREATE TABLE cpo_range1 PARTITION OF cpo_range FOR VALUES FROM (30001) TO (60001);
CREATE TABLE cpo_range2 PARTITION OF cpo_range FOR VALUES FROM (60001) TO (90001);
CREATE TABLE cpo_range_next PARTITION OF cpo_range FOR VALUES FROM (90001) TO (120001);
CREATE TABLE cpo_range_default PARTITION OF cpo_range DEFAULT;
INSERT INTO cpo_part SELECT * FROM cpo_plain;
INSERT INTO cpo_range SELECT * FROM cpo_plain;
CREATE INDEX ON cpo_plain USING lion (g);
CREATE INDEX ON cpo_plain USING lion (c);
CREATE INDEX ON cpo_part USING lion (g);
CREATE INDEX ON cpo_part USING lion (c);
CREATE INDEX ON cpo_range USING lion (g);
CREATE INDEX ON cpo_range USING lion (c);
VACUUM (FREEZE, ANALYZE) cpo_plain, cpo_part, cpo_range;
SET max_parallel_workers_per_gather = 0;

CREATE FUNCTION pg_temp.cpo(q text, margin text, OUT plan text, OUT cost float8)
LANGUAGE plpgsql AS $$
DECLARE j json; ln text; p text := '';
BEGIN
	PERFORM set_config('pg_lion.pushdown_margin', margin, true);
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	cost := (j->0->'Plan'->>'Total Cost')::float8;
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP p := p || ln || E'\n'; END LOOP;
	plan := coalesce(substring(p FROM 'Custom Scan \((Lion[A-Za-z]*)\)'), 'core');
END $$;

WITH q(t, q) AS (VALUES
	('plain', 'SELECT count(*) FROM cpo_plain WHERE g IN (1, 2, 3) AND c <> 2'),
	('hash-partitioned', 'SELECT count(*) FROM cpo_part WHERE g IN (1, 2, 3) AND c <> 2'),
	('range, two empty', 'SELECT count(*) FROM cpo_range WHERE g IN (1, 2, 3) AND c <> 2')),
r AS (SELECT t, (pg_temp.cpo(q, '1')).*, (pg_temp.cpo(q, '0.01')).plan AS plan_hundredth,
			 (pg_temp.cpo(q, '0.01')).cost AS cost_hundredth FROM q)
SELECT t, plan AS plan_at_1, plan_hundredth AS plan_at_hundredth,
	   cost = cost_hundredth AS same_cost_at_every_margin FROM r ORDER BY t;

SELECT CASE WHEN bool_and(same) THEN 'cost_partition_ownscan: not present'
			ELSE 'cost_partition_ownscan: PRESENT on ' || string_agg(t, ', ' ORDER BY t) FILTER (WHERE NOT same) END AS verdict
  FROM (SELECT t, (pg_temp.cpo(q, '1')).cost = (pg_temp.cpo(q, '0.01')).cost AS same
		  FROM (VALUES ('plain', 'SELECT count(*) FROM cpo_plain WHERE g IN (1, 2, 3) AND c <> 2'),
					   ('hash-partitioned', 'SELECT count(*) FROM cpo_part WHERE g IN (1, 2, 3) AND c <> 2'),
					   ('range, two empty', 'SELECT count(*) FROM cpo_range WHERE g IN (1, 2, 3) AND c <> 2')) v(t, q)) s;
DROP TABLE cpo_plain, cpo_part, cpo_range;
