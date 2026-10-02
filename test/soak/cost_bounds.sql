-- Every answer under the cost settings at their bounds (DESIGN.md §39 on a
-- build that has them): 26 queries - counts, GROUP BYs of one and two
-- columns, count(DISTINCT), HAVING, the top k, a range, a multi-key column,
-- a hash-partitioned table, the FK-side join, semi and anti joins, a LATERAL
-- semi join and count (parameterized join rels), LionOrdered, and six of them
-- as generic plans - each under 13 settings: the defaults, the margin at a
-- hundredth, every rate at a thousandth, every rate at a thousand with the
-- margin at a hundredth, resident index pages free or dear with
-- effective_cache_size at its extremes, core's joins, aggregates or scans
-- all off (which force lion's paths), parallel plans at both ends of the
-- rates, 64kB of work_mem, and lion's own switches off.  Each row is the
-- plan's lion node, its estimated total cost, and whether its rows equal the
-- same query's with lion's paths off and sequential scans only.  A cost that
-- is NaN or infinite, or an answer that differs, is a failure; the last line
-- is the verdict.  On a build without pg_lion.pushdown_margin it says so and
-- stops.
--
--     eval "$(./dev.sh env)"; psql -X -f test/soak/cost_bounds.sql
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
SELECT current_setting('pg_lion.pushdown_margin', true) IS NULL AS no_margin \gset
\if :no_margin
\echo 'cost_bounds: this build has no pg_lion.pushdown_margin; skipped'
\quit
\endif
SET synchronous_commit = on;
DROP SCHEMA IF EXISTS cost_bounds CASCADE;
CREATE SCHEMA cost_bounds;
SET search_path = cost_bounds, public;
CREATE TABLE lcrd (pk int8 PRIMARY KEY, attr int);
INSERT INTO lcrd SELECT i, i % 7 FROM generate_series(1, 2000) i;
CREATE TABLE lcrf (id int, fk int8, x int, tags text[]);
INSERT INTO lcrf SELECT i, (i * 7919) % 2000 + 1, i % 10,
	CASE WHEN i % 5 = 0 THEN NULL ELSE ARRAY['t' || (i % 13), 't' || (i % 3)] END
  FROM generate_series(1, 40000) i;
CREATE INDEX lcrf_fk ON lcrf USING lion (fk);
CREATE INDEX lcrf_x ON lcrf USING lion (x);
CREATE INDEX lcrf_tags ON lcrf USING lion (tags);
-- a hash-partitioned fact, indexes before the load
CREATE TABLE lcp (id int, fk int8, g int, c int) PARTITION BY HASH (id);
CREATE TABLE lcp0 PARTITION OF lcp FOR VALUES WITH (MODULUS 3, REMAINDER 0);
CREATE TABLE lcp1 PARTITION OF lcp FOR VALUES WITH (MODULUS 3, REMAINDER 1);
CREATE TABLE lcp2 PARTITION OF lcp FOR VALUES WITH (MODULUS 3, REMAINDER 2);
CREATE INDEX ON lcp USING lion (g);
CREATE INDEX ON lcp USING lion (c);
CREATE INDEX ON lcp USING lion (fk);
INSERT INTO lcp SELECT i, (i * 31) % 2000 + 1, i % 50, (hashint4(i) & 2147483647) % 4 FROM generate_series(1, 90000) i;
-- a table big enough for parallel plans
CREATE TABLE lct (id int, g int, h int, c int, pad text);
INSERT INTO lct SELECT i, i % 100, (hashint4(i) & 2147483647) % 1000, i % 3, repeat('y', 40) FROM generate_series(1, 300000) i;
CREATE INDEX lct_g ON lct USING lion (g);
CREATE INDEX lct_h ON lct USING lion (h);
CREATE INDEX lct_c ON lct USING lion (c);
CREATE INDEX lct_id ON lct (id);
CREATE TABLE lsmall (id int);
INSERT INTO lsmall SELECT generate_series(0, 12);
VACUUM (FREEZE, ANALYZE) lcrd, lcrf, lcp, lct, lsmall;
-- a dirty heap on part of lct: the visibility map cannot vouch for those pages
UPDATE lct SET pad = 'z' WHERE id % 97 = 0;
DELETE FROM lct WHERE id % 89 = 0;
ANALYZE lct;
CREATE OR REPLACE FUNCTION cm_set(sw text[]) RETURNS void LANGUAGE plpgsql AS $$
DECLARE n text; i int;
BEGIN
	FOREACH n IN ARRAY ARRAY['pg_lion.pushdown_margin', 'pg_lion.hashagg_rate', 'pg_lion.agg_rate',
		'pg_lion.hashjoin_rate', 'pg_lion.mergejoin_rate', 'pg_lion.nestloop_rate', 'pg_lion.bitmap_rate',
		'pg_lion.resident_page_cost', 'effective_cache_size', 'work_mem',
		'pg_lion.enable_count_pushdown', 'pg_lion.enable_semijoin', 'pg_lion.enable_ordered_scan',
		'enable_seqscan', 'enable_indexscan', 'enable_indexonlyscan', 'enable_bitmapscan', 'enable_sort',
		'enable_hashagg', 'enable_hashjoin', 'enable_mergejoin', 'enable_nestloop',
		'max_parallel_workers_per_gather', 'parallel_setup_cost', 'parallel_tuple_cost',
		'min_parallel_table_scan_size', 'min_parallel_index_scan_size', 'plan_cache_mode'] LOOP
		PERFORM set_config(n, (SELECT boot_val FROM pg_settings WHERE name = n), true);
	END LOOP;
	FOR i IN 1 .. coalesce(array_length(sw, 1), 0) BY 2 LOOP
		PERFORM set_config(sw[i], sw[i + 1], true);
	END LOOP;
END $$;
-- q under sw: the lion node of its plan, its cost, and its rows against the
-- reference (lion off, sequential scans); with args, as a generic plan
CREATE OR REPLACE FUNCTION cm_run(q text, sw text[], args text DEFAULT NULL,
	OUT plan text, OUT cost text, OUT same text) LANGUAGE plpgsql AS $$
DECLARE ln text; p text := ''; a text[]; b text[]; j json; run text; ex text;
BEGIN
	PERFORM cm_set(sw);
	IF args IS NOT NULL THEN
		PERFORM set_config('plan_cache_mode', 'force_generic_plan', true);
		EXECUTE 'PREPARE cm_p AS ' || q;
		run := 'EXECUTE cm_p(' || args || ')';
	ELSE
		run := q;
	END IF;
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || run INTO j;
	cost := j->0->'Plan'->>'Total Cost';
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || run LOOP p := p || ln || E'\n'; END LOOP;
	plan := coalesce(substring(p FROM 'Custom Scan \((Lion[A-Za-z]*)\)'), 'core');
	IF p ~ 'Gather' THEN plan := plan || '+gather'; END IF;
	EXECUTE 'CREATE TEMP TABLE cm_a AS ' || run;
	EXECUTE 'SELECT coalesce(array_agg(x::text ORDER BY x::text), ''{}'') FROM cm_a x' INTO a;
	DROP TABLE cm_a;
	PERFORM cm_set('{pg_lion.enable_count_pushdown, off, pg_lion.enable_semijoin, off, pg_lion.enable_ordered_scan, off, enable_indexscan, off, enable_indexonlyscan, off, enable_bitmapscan, off}');
	IF args IS NOT NULL THEN
		PERFORM set_config('plan_cache_mode', 'force_custom_plan', true);
	END IF;
	EXECUTE 'CREATE TEMP TABLE cm_b AS ' || run;
	EXECUTE 'SELECT coalesce(array_agg(x::text ORDER BY x::text), ''{}'') FROM cm_b x' INTO b;
	DROP TABLE cm_b;
	IF args IS NOT NULL THEN DEALLOCATE cm_p; END IF;
	same := CASE WHEN a = b THEN 'same ' || cardinality(a) ELSE format('MISMATCH lion %s ref %s', cardinality(a), cardinality(b)) END;
	IF cost IS NULL OR cost IN ('NaN', 'Infinity', '-Infinity') OR cost::float8 > 1e300 THEN
		same := same || ' BADCOST';
	END IF;
END $$;
CREATE TEMP TABLE cm_q (qn text, q text, args text);
INSERT INTO cm_q VALUES
 ('count and', 'SELECT count(*) FROM lct WHERE g = 7 AND c = 1', NULL),
 ('group1', 'SELECT g, count(*) FROM lct GROUP BY g', NULL),
 ('group2 where', 'SELECT g, c, count(*) FROM lct WHERE h < 500 GROUP BY g, c', NULL),
 ('distinct', 'SELECT count(DISTINCT h) FROM lct WHERE c = 2', NULL),
 ('group distinct', 'SELECT c, count(DISTINCT g) FROM lct GROUP BY c', NULL),
 ('part group', 'SELECT g, count(*) FROM lcp WHERE c = 1 GROUP BY g', NULL),
 ('part count', 'SELECT count(*) FROM lcp WHERE g IN (1,2,3) AND c <> 2', NULL),
 ('fkjoin', 'SELECT count(*) FROM lcrf f JOIN lcrd d ON f.fk = d.pk WHERE d.attr = 2', NULL),
 ('fkjoin group', 'SELECT d.attr, count(*) FROM lcrf f JOIN lcrd d ON f.fk = d.pk WHERE f.x = 3 GROUP BY d.attr', NULL),
 ('semi', 'SELECT d.pk FROM lcrd d WHERE EXISTS (SELECT 1 FROM lcrf f WHERE f.fk = d.pk AND f.x = 3)', NULL),
 ('anti', 'SELECT d.pk FROM lcrd d WHERE NOT EXISTS (SELECT 1 FROM lcrf f WHERE f.fk = d.pk AND f.x = 3)', NULL),
 ('part fkjoin', 'SELECT d.attr, count(*) FROM lcp p JOIN lcrd d ON p.fk = d.pk GROUP BY d.attr', NULL),
 ('ordered', 'SELECT id FROM lct WHERE g = 17 ORDER BY id LIMIT 10', NULL),
 ('ordered and', 'SELECT id FROM lct WHERE g = 17 AND c = 2 ORDER BY id DESC LIMIT 25', NULL),
 ('tags', 'SELECT count(*) FROM lcrf WHERE tags @> ARRAY[''t3''] AND x = 2', NULL),
 ('lateral semi', 'SELECT t.id, s.pk FROM lsmall t, LATERAL (SELECT d.pk FROM lcrd d WHERE EXISTS (SELECT 1 FROM lcrf f WHERE f.fk = d.pk AND f.x = t.id)) s', NULL),
 ('lateral count', 'SELECT t.id, s.n FROM lsmall t, LATERAL (SELECT count(*) n FROM lcrf f JOIN lcrd d ON f.fk = d.pk WHERE f.x = t.id) s', NULL),
 ('topk', 'SELECT g, count(*) FROM lct GROUP BY g ORDER BY count(*) DESC, g LIMIT 5', NULL),
 ('having', 'SELECT h, count(*) FROM lct WHERE c = 0 GROUP BY h HAVING count(*) > 100', NULL),
 ('range', 'SELECT count(*) FROM lct WHERE h BETWEEN 100 AND 400 AND g = 3', NULL),
 ('gen tags', 'SELECT count(*) FROM lcrf WHERE tags @> $1 AND x = $2', '''{t3}''::text[], 2'),
 ('gen tags group', 'SELECT x, count(*) FROM lcrf WHERE tags && $1 GROUP BY x', '''{t3,t5}''::text[]'),
 ('gen group', 'SELECT g, count(*) FROM lct WHERE h < $1 GROUP BY g', '300'),
 ('gen semi', 'SELECT d.pk FROM lcrd d WHERE EXISTS (SELECT 1 FROM lcrf f WHERE f.fk = d.pk AND f.x = $1)', '3'),
 ('gen part', 'SELECT count(*) FROM lcp WHERE g = $1 AND c = $2', '5, 1'),
 ('gen fkjoin', 'SELECT d.attr, count(*) FROM lcrf f JOIN lcrd d ON f.fk = d.pk WHERE f.x = $1 GROUP BY d.attr', '4');
CREATE TEMP TABLE cm_s (sn text, sw text[]);
INSERT INTO cm_s VALUES
 ('default', '{}'),
 ('margin.01', '{pg_lion.pushdown_margin,0.01}'),
 ('rates.001', '{pg_lion.hashagg_rate,0.001,pg_lion.agg_rate,0.001,pg_lion.hashjoin_rate,0.001,pg_lion.mergejoin_rate,0.001,pg_lion.nestloop_rate,0.001,pg_lion.bitmap_rate,0.001}'),
 ('rates1000 m.01', '{pg_lion.hashagg_rate,1000,pg_lion.agg_rate,1000,pg_lion.hashjoin_rate,1000,pg_lion.mergejoin_rate,1000,pg_lion.nestloop_rate,1000,pg_lion.bitmap_rate,1000,pg_lion.pushdown_margin,0.01}'),
 ('cheap pages', '{pg_lion.hashagg_rate,0.001,pg_lion.agg_rate,0.001,pg_lion.bitmap_rate,0.001,pg_lion.resident_page_cost,0,effective_cache_size,1TB}'),
 ('dear pages', '{pg_lion.resident_page_cost,1000000,effective_cache_size,8kB,pg_lion.pushdown_margin,0.01}'),
 ('joins off m.01', '{enable_hashjoin,off,enable_mergejoin,off,enable_nestloop,off,pg_lion.pushdown_margin,0.01,pg_lion.hashjoin_rate,1000,pg_lion.nestloop_rate,1000,pg_lion.mergejoin_rate,1000}'),
 ('agg off m.01', '{enable_hashagg,off,enable_sort,off,pg_lion.pushdown_margin,0.01,pg_lion.hashagg_rate,1000,pg_lion.agg_rate,1000}'),
 ('scans off m.01', '{enable_seqscan,off,enable_indexscan,off,enable_indexonlyscan,off,enable_bitmapscan,off,pg_lion.pushdown_margin,0.01,pg_lion.agg_rate,1000,pg_lion.bitmap_rate,1000}'),
 ('par rates.001', '{max_parallel_workers_per_gather,4,parallel_setup_cost,0,parallel_tuple_cost,0,min_parallel_table_scan_size,0,min_parallel_index_scan_size,0,pg_lion.hashagg_rate,0.001,pg_lion.agg_rate,0.001,pg_lion.hashjoin_rate,0.001,pg_lion.bitmap_rate,0.001}'),
 ('par rates1000', '{max_parallel_workers_per_gather,4,parallel_setup_cost,0,parallel_tuple_cost,0,min_parallel_table_scan_size,0,min_parallel_index_scan_size,0,pg_lion.hashagg_rate,1000,pg_lion.agg_rate,1000,pg_lion.hashjoin_rate,1000,pg_lion.pushdown_margin,0.01}'),
 ('wm64k rates.001', '{work_mem,64kB,pg_lion.hashagg_rate,0.001,pg_lion.agg_rate,0.001,pg_lion.hashjoin_rate,0.001,pg_lion.nestloop_rate,0.001,pg_lion.bitmap_rate,0.001}'),
 ('lion off', '{pg_lion.enable_count_pushdown,off,pg_lion.enable_semijoin,off,pg_lion.enable_ordered_scan,off,pg_lion.hashagg_rate,0.001,pg_lion.agg_rate,0.001,pg_lion.hashjoin_rate,0.001,pg_lion.bitmap_rate,0.001,pg_lion.nestloop_rate,0.001}');

CREATE TEMP TABLE cm_r AS
SELECT q.qn, s.sn, r.plan, round(r.cost::numeric, 2) AS cost, r.same
  FROM cm_q q CROSS JOIN cm_s s CROSS JOIN LATERAL cm_run(q.q, s.sw, q.args) r;
SELECT * FROM cm_r ORDER BY qn, sn;
SELECT CASE WHEN count(*) FILTER (WHERE same !~ '^same [0-9]+$') = 0
			THEN format('cost_bounds: %s plans, every answer the same, every cost finite', count(*))
			ELSE format('cost_bounds: FAILED, %s of %s plans', count(*) FILTER (WHERE same !~ '^same [0-9]+$'), count(*)) END AS verdict
  FROM cm_r;
RESET search_path;
DROP SCHEMA cost_bounds CASCADE;
