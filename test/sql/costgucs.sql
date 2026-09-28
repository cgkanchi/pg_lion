-- The cost model's constants as planner settings (DESIGN.md §31, "The
-- settings").
--
-- Every per-operation price of lion's cost model is a setting,
-- pg_lion.<name>_cost, holding the multiplier of the core cost setting the
-- price is written in, so that the model can be calibrated on a workload as
-- core's is with random_page_cost.  The defaults are the values the prices
-- had as constants, so every other test plans exactly as before.  This one
-- lists them, shows that three of them - an index scan's, a count's and the
-- FK-side join's - are read by the planner, and that a SET LOCAL of one ends
-- with its transaction.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
-- a sample of every row: the statistics, and the plans, do not depend on it
SET default_statistics_target = 1000;
SET max_parallel_workers_per_gather = 0;

-- ---------- 1. the settings ----------
-- user settings, real-valued, each at the value its constant had
SELECT name, setting, boot_val, context, vartype
  FROM pg_settings
 WHERE name LIKE 'pg_lion.%\_cost'
 ORDER BY name;
-- from 0 up, among the customized options as every extension's settings are
SELECT count(*) AS settings,
	   bool_and(min_val::float8 = 0) AS from_zero,
	   bool_and(max_val::float8 > 1e300) AS unbounded,
	   bool_and(category = 'Customized Options') AS custom_group,
	   bool_and(short_desc ~ ', in multiples of (cpu_operator_cost|cpu_tuple_cost|seq_page_cost|random_page_cost)\.$') AS unit_named
  FROM pg_settings
 WHERE name LIKE 'pg_lion.%\_cost';

-- 50000 rows; a has 10 values and g 100, both placed at random, about 55
-- rows to a page
CREATE TABLE lcg (id int, a int, g int, pad text);
INSERT INTO lcg
SELECT i, (hashint4(i) & 2147483647) % 10,
	   (hashint4(i + 1000000) & 2147483647) % 100, repeat('x', 100)
  FROM generate_series(1, 50000) i;
CREATE INDEX lcg_a ON lcg USING lion (a);
CREATE INDEX lcg_g ON lcg USING lion (g);
VACUUM (FREEZE, ANALYZE) lcg;

-- the FK-side join: a fact of 40000 rows over 2000 keys, 20 rows a key, and
-- its dimension, 100 rows to a region
CREATE TABLE lcf (id int, fk int8, x int);
INSERT INTO lcf SELECT i, (i * 7919) % 2000 + 1, i % 10 FROM generate_series(1, 40000) i;
CREATE INDEX lcf_fk ON lcf USING lion (fk);
CREATE INDEX lcf_x ON lcf USING lion (x);
CREATE TABLE lcd (pk int8 PRIMARY KEY, attr int, region text);
INSERT INTO lcd SELECT i, i % 7, 'r' || (i % 20) FROM generate_series(1, 2000) i;
CREATE INDEX lcd_region ON lcd USING lion (region);
VACUUM (FREEZE, ANALYZE) lcf;
VACUUM (FREEZE, ANALYZE) lcd;

/*
 * lcg_cost() is the estimated total cost of q's plan with setting `guc` at
 * `val` - its default when NULL - and the other settings in sw, name and
 * value in turn, all for the call alone; with no setting named, at whatever
 * the session has.
 */
CREATE FUNCTION lcg_cost(q text, guc text DEFAULT NULL, val text DEFAULT NULL,
						 sw text[] DEFAULT '{}')
RETURNS float8
LANGUAGE plpgsql AS $$
DECLARE
	p json;
	i int;
BEGIN
	IF guc IS NOT NULL THEN
		PERFORM set_config(guc,
						   coalesce(val, (SELECT boot_val FROM pg_settings
										 WHERE name = guc)), true);
	END IF;
	FOR i IN 1 .. coalesce(array_length(sw, 1), 0) BY 2 LOOP
		PERFORM set_config(sw[i], sw[i + 1], true);
	END LOOP;
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO p;
	RETURN (p->0->'Plan'->>'Total Cost')::float8;
END $$;

/* The top two nodes of q's plan, with setting `guc` at `val` for the call. */
CREATE FUNCTION lcg_plan(q text, guc text, val text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	res text[] := '{}';
BEGIN
	PERFORM set_config(guc,
					   coalesce(val, (SELECT boot_val FROM pg_settings
									 WHERE name = guc)), true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF cardinality(res) < 2 AND
		   (cardinality(res) = 0 OR ln ~ '^\s*->') THEN
			res := res || regexp_replace(btrim(regexp_replace(ln, '->', '')),
										 ' (on|using) .*$', '');
		END IF;
	END LOOP;
	RETURN array_to_string(res, ' / ');
END $$;

-- ---------- 2. each is read ----------
-- An index scan's (DESIGN.md §29.11): what a plain scan pays to fetch a row
-- past the first on its heap page.  515 rows on some 400 pages: at 0 the
-- plain scan costs what the bitmap heap scan does, and a tie goes to it; at
-- 100 its fetches are the dearer.
SET pg_lion.enable_count_pushdown = off;
SELECT lcg_cost(q, s, '0', sw) < lcg_cost(q, s, NULL, sw) AS lowered_cheaper,
	   lcg_cost(q, s, '100', sw) > lcg_cost(q, s, NULL, sw) AS raised_dearer
  FROM (VALUES ('SELECT sum(id) FROM lcg WHERE g = 5',
				'pg_lion.plain_fetch_row_cost',
				'{enable_seqscan, off, enable_bitmapscan, off}'::text[])) v(q, s, sw);
SELECT lcg_plan('SELECT sum(id) FROM lcg WHERE g = 5', 'pg_lion.plain_fetch_row_cost', '0');
SELECT lcg_plan('SELECT sum(id) FROM lcg WHERE g = 5', 'pg_lion.plain_fetch_row_cost', '100');
RESET pg_lion.enable_count_pushdown;

-- A count's (DESIGN.md §10, "The units"): each count a GROUP BY makes, one
-- per entry of its column.  At its default the count pushdown answers the
-- hundred groups; at a hundred times that, the hash aggregate does.
SELECT lcg_cost(q, s, '0', sw) < lcg_cost(q, s, NULL, sw) AS lowered_cheaper,
	   lcg_cost(q, s, '100', sw) > lcg_cost(q, s, NULL, sw) AS raised_dearer
  FROM (VALUES ('SELECT g, count(*) FROM lcg GROUP BY g',
				'pg_lion.entry_count_cost',
				'{enable_seqscan, off, enable_bitmapscan, off, enable_indexscan, off, enable_indexonlyscan, off}'::text[])) v(q, s, sw);
SELECT lcg_plan('SELECT g, count(*) FROM lcg GROUP BY g', 'pg_lion.entry_count_cost', NULL);
SELECT lcg_plan('SELECT g, count(*) FROM lcg GROUP BY g', 'pg_lion.entry_count_cost', '5000');

-- The FK-side join's (DESIGN.md §27): each count, one per dimension row.  At
-- its default the node counts the region's hundred rows' keys; at a hundred
-- times that, a hash join is the cheaper.
SELECT lcg_cost(q, s, '0', sw) < lcg_cost(q, s, NULL, sw) AS lowered_cheaper,
	   lcg_cost(q, s, '100', sw) > lcg_cost(q, s, NULL, sw) AS raised_dearer
  FROM (VALUES ('SELECT d.attr, count(*) FROM lcf f JOIN lcd d ON f.fk = d.pk WHERE d.region = ''r1'' GROUP BY d.attr',
				'pg_lion.fkjoin_count_cost',
				'{enable_hashjoin, off, enable_mergejoin, off, enable_nestloop, off}'::text[])) v(q, s, sw);
SELECT lcg_plan('SELECT d.attr, count(*) FROM lcf f JOIN lcd d ON f.fk = d.pk WHERE d.region = ''r1'' GROUP BY d.attr', 'pg_lion.fkjoin_count_cost', NULL);
SELECT lcg_plan('SELECT d.attr, count(*) FROM lcf f JOIN lcd d ON f.fk = d.pk WHERE d.region = ''r1'' GROUP BY d.attr', 'pg_lion.fkjoin_count_cost', '2500');

-- ---------- 3. a SET LOCAL ends with its transaction ----------
SELECT lcg_cost('SELECT g, count(*) FROM lcg GROUP BY g') AS before \gset
BEGIN;
SET LOCAL pg_lion.entry_count_cost = 5000;
SHOW pg_lion.entry_count_cost;
EXPLAIN (COSTS OFF) SELECT g, count(*) FROM lcg GROUP BY g;
SELECT lcg_cost('SELECT g, count(*) FROM lcg GROUP BY g') > :before AS dearer;
ROLLBACK;
SHOW pg_lion.entry_count_cost;
EXPLAIN (COSTS OFF) SELECT g, count(*) FROM lcg GROUP BY g;
SELECT lcg_cost('SELECT g, count(*) FROM lcg GROUP BY g') = :before AS same_cost;

DROP FUNCTION lcg_cost(text, text, text, text[]);
DROP FUNCTION lcg_plan(text, text, text);
DROP TABLE lcg, lcf, lcd;
