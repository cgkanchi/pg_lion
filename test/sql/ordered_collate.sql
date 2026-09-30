-- LionOrdered's walk of a lion column, and the collations of the clauses that
-- bound it (DESIGN.md §30.11).
--
-- The walk of a lion index's own ordered column takes the range comparisons
-- of the column as its bounds, and they leave the filter.  It compares a
-- bound with the directory's keys, which are in the order of the index
-- column's collation, so a comparison under another collation is no bound of
-- it: core's rule for an index clause, IndexCollMatchesExprColl().  The walk
-- used to take any, and `t < 'a' COLLATE "C"` over an English column returned
-- no rows, where 'A0' is below 'a' in "C" and above it in English (2026-09-29
-- review).  Every answer is compared, in order, with the plan the planner
-- makes with pg_lion.enable_ordered_scan off; every ORDER BY ends in the
-- unique id, which core sorts incrementally above the walk, so that ties come
-- out in one order.
--
-- The collations are ICU's; the test is skipped without them
-- (test/expected/ordered_collate_1.out).
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET max_parallel_workers_per_gather = 0;

-- the collations, where the build has ICU and the database is UTF8
SET client_min_messages = warning;
DO $$
BEGIN
	IF getdatabaseencoding() = 'UTF8' THEN
		CREATE COLLATION loc_en (provider = icu, locale = 'en');
		CREATE COLLATION loc_ci (provider = icu, locale = 'und-u-ks-level2',
								 deterministic = false);
	END IF;
EXCEPTION WHEN feature_not_supported THEN
	NULL;						-- a build without ICU
END $$;
RESET client_min_messages;
SELECT NOT EXISTS (SELECT 1 FROM pg_collation WHERE collname = 'loc_ci')
	   AS loc_skip \gset
\if :loc_skip
\echo 'no ICU collations, or not a UTF8 database: skipped'
\else
-- An English column, and a case-insensitive one, each with a lion index on
-- (t, k) and no btree: the walk of t is the only order of t there is.  The
-- keys are a letter in either case and a number, every combination of them.
CREATE TABLE loc (id int, t text COLLATE loc_en, k int)
  WITH (autovacuum_enabled = off);
INSERT INTO loc SELECT g, (ARRAY['A', 'a', 'B', 'b'])[1 + g % 4] || (g / 4 % 100),
	   g % 7
  FROM generate_series(1, 20000) g;
CREATE INDEX loc_l ON loc USING lion (t, k);
CREATE TABLE loc2 (LIKE loc) WITH (autovacuum_enabled = off);
ALTER TABLE loc2 ALTER COLUMN t TYPE text COLLATE loc_ci;
INSERT INTO loc2 SELECT * FROM loc;
CREATE INDEX loc2_l ON loc2 USING lion (t, k);
VACUUM (FREEZE, ANALYZE) loc;
VACUUM (FREEZE, ANALYZE) loc2;

/*
 * loc_cmp() runs q without core's Sort, so that an ordered path is what is
 * left, and says whether its plan has a LionOrdered node; runs it again - or
 * `ordinary`, the same query for a prepared statement whose plan the setting
 * does not reach - with the node off; and compares the two answers in order.
 */
CREATE FUNCTION loc_cmp(q text, ordinary text DEFAULT NULL) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	r record;
	used boolean := false;
	a text[] := '{}';
	b text[] := '{}';
BEGIN
	PERFORM set_config('enable_sort', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(LionOrdered\)' THEN
			used := true;
		END IF;
	END LOOP;
	FOR r IN EXECUTE q LOOP
		a := a || r::text;
	END LOOP;
	PERFORM set_config('enable_sort', 'on', true);

	PERFORM set_config('pg_lion.enable_ordered_scan', 'off', true);
	FOR r IN EXECUTE coalesce(ordinary, q) LOOP
		b := b || r::text;
	END LOOP;
	PERFORM set_config('pg_lion.enable_ordered_scan', 'on', true);
	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH: %s rows through the node, %s ordinary; first %s / %s',
					  cardinality(a), cardinality(b), a[1], b[1]);
	END IF;
	RETURN format('%s, %s rows, first %s',
				  CASE WHEN used THEN 'LionOrdered' ELSE 'no LionOrdered' END,
				  cardinality(a), a[1]);
END $$;

-- loc_plan() prints q's plan without core's Sort, less what varies by major.
CREATE FUNCTION loc_plan(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('enable_sort', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln !~ '^\s*(Index Searches|Disabled|Storage|Planning|Execution|Buffers)' THEN
			RETURN NEXT ln;
		END IF;
	END LOOP;
	PERFORM set_config('enable_sort', 'on', true);
END $$;

-- 1. A bound under "C" on the English column is the walk's filter, not its
--    Index Cond: 'A0' is below 'a' in "C".
SELECT * FROM loc_plan($$
	SELECT id, t FROM loc WHERE t < 'a' COLLATE "C" ORDER BY t, id LIMIT 5$$);
SELECT loc_cmp($$
	SELECT id, t FROM loc WHERE t < 'a' COLLATE "C" ORDER BY t, id LIMIT 5$$);
SELECT loc_cmp($$
	SELECT id, t FROM loc WHERE t > 'b' COLLATE "C" ORDER BY t DESC, id LIMIT 5$$);
SELECT loc_cmp($$
	SELECT id, t FROM loc WHERE 'a' COLLATE "C" > t ORDER BY t, id LIMIT 5$$);
-- a range of two bounds, one under the column's collation and one not: the
-- first bounds the walk, the second is its filter
SELECT * FROM loc_plan($$
	SELECT id, t FROM loc WHERE t >= 'a5' AND t < 'b' COLLATE "C"
	 ORDER BY t, id LIMIT 5$$);
SELECT loc_cmp($$
	SELECT id, t FROM loc WHERE t >= 'a5' AND t < 'b' COLLATE "C"
	 ORDER BY t, id LIMIT 5$$);
-- with a set: k = 3 is lion's, the "C" bound still the filter
SELECT loc_cmp($$
	SELECT id, t FROM loc WHERE k = 3 AND t < 'a' COLLATE "C"
	 ORDER BY t DESC, id LIMIT 5$$);
-- a parameter under "C", in a generic plan
PREPARE loc_p(text) AS
	SELECT id, t FROM loc WHERE t < $1 COLLATE "C" ORDER BY t, id LIMIT 5;
SET plan_cache_mode = force_generic_plan;
SELECT * FROM loc_plan('EXECUTE loc_p(''a'')');
SELECT loc_cmp('EXECUTE loc_p(''a'')', $$
	SELECT id, t FROM loc WHERE t < 'a' COLLATE "C" ORDER BY t, id LIMIT 5$$);
RESET plan_cache_mode;
DEALLOCATE loc_p;

-- 2. Under the column's own collation, named or not, the bound is the walk's.
SELECT * FROM loc_plan($$
	SELECT id, t FROM loc WHERE t < 'b' ORDER BY t, id LIMIT 5$$);
SELECT loc_cmp($$
	SELECT id, t FROM loc WHERE t < 'b' ORDER BY t, id LIMIT 5$$);
SELECT * FROM loc_plan($$
	SELECT id, t FROM loc WHERE t < 'b' COLLATE loc_en ORDER BY t DESC, id LIMIT 5$$);
SELECT loc_cmp($$
	SELECT id, t FROM loc WHERE t < 'b' COLLATE loc_en ORDER BY t DESC, id LIMIT 5$$);
-- an ORDER BY under another collation is not the walk's order at all
SELECT loc_cmp($$
	SELECT id, t FROM loc WHERE t < 'b' ORDER BY t COLLATE "C", id LIMIT 5$$);

-- 3. The case-insensitive column: a bound under "C" is the filter, and so is
--    a list under "C", which only its own case matches; under the column's
--    collation the bound is the walk's, and 'A1' and 'a1' are one key of it.
SELECT * FROM loc_plan($$
	SELECT id, t FROM loc2 WHERE t > 'a' COLLATE "C" ORDER BY t, id LIMIT 5$$);
SELECT loc_cmp($$
	SELECT id, t FROM loc2 WHERE t > 'a' COLLATE "C" ORDER BY t, id LIMIT 5$$);
SELECT loc_cmp($$
	SELECT id, t FROM loc2 WHERE t <= 'B' COLLATE "C" ORDER BY t DESC, id LIMIT 5$$);
SELECT loc_cmp($$
	SELECT id, t FROM loc2 WHERE t = ANY (ARRAY['a1', 'B2'] COLLATE "C")
	 ORDER BY t, id LIMIT 5$$);
SELECT * FROM loc_plan($$
	SELECT id, t FROM loc2 WHERE t > 'a' ORDER BY t, id LIMIT 5$$);
SELECT loc_cmp($$
	SELECT id, t FROM loc2 WHERE t > 'a' ORDER BY t, id LIMIT 5$$);
SELECT loc_cmp($$
	SELECT id, t FROM loc2 WHERE t >= 'A1' AND t < 'a2' ORDER BY t DESC, id LIMIT 5$$);

DROP TABLE loc, loc2;
DROP FUNCTION loc_cmp(text, text);
DROP FUNCTION loc_plan(text);
DROP COLLATION loc_en;
DROP COLLATION loc_ci;
\endif
