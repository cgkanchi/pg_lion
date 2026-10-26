-- A nested loop's probe of a B-tree that does not cover the query, in heap
-- mode (DESIGN.md §49).
--
-- SELECT f.*, d.name FROM fact f JOIN dim d ON d.id = f.dim_id WHERE <clauses
-- lion indexes on d answer> AND <a filter on f>: the query reads d.name, which
-- d's primary key does not hold, so §40.10's index-only probe is not offered.
-- Under pg_lion.enable_heap_probe the inner side of the loop is a
-- LionBtreeScan of the primary key parameterized by the outer row: a descent
-- to the outer row's key, the entry tested against a lion set built once from
-- d's restriction clauses, and the heap row fetched for a member alone.  Core's
-- plan for the same loop is an Index Scan of the primary key that fetches every
-- outer row's dim row to test the filter on it; the probe is that scan less the
-- rows the set rejects, and is priced by the same formula over its fetches.
-- Every query is run through the probe and through the ordinary plan, and the
-- answers are compared, as multisets unless the query orders them completely.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;
SET default_statistics_target = 1000;
SET work_mem = '64MB';
SET pg_lion.competitor_pricing = off;

CREATE TYPE hp_grade AS ENUM ('delta', 'gamma', 'alpha', 'beta');
CREATE TABLE hp_dim (
	id int PRIMARY KEY,
	grade hp_grade NOT NULL,	-- lion-filtered: 'alpha' in 10% of the rows
	verified bool NOT NULL,		-- lion-filtered: two thirds true
	tags text[] NOT NULL,		-- lion-filtered: one of ten tags
	name text NOT NULL,			-- read by the queries, in no index
	pad text					-- wide: the heap is many times the B-tree
) WITH (autovacuum_enabled = off);
INSERT INTO hp_dim
SELECT i,
	   CASE WHEN h1 < 10 THEN 'alpha' WHEN h1 < 20 THEN 'beta'
			WHEN h1 < 90 THEN 'gamma' ELSE 'delta' END::hp_grade,
	   h3 % 3 <> 0,
	   ARRAY['t' || (h3 / 3 % 10)],
	   'd' || i,
	   repeat(md5(i::text), 4)
  FROM (SELECT i,
			   ((hashint8extended(i::bigint, 1) & 9223372036854775807) % 100)::int AS h1,
			   ((hashint8extended(i::bigint, 3) & 9223372036854775807) % 3000)::int AS h3
		  FROM generate_series(1, 100000) i) s;
CREATE INDEX hp_dim_grade ON hp_dim USING lion (grade);
CREATE INDEX hp_dim_verified ON hp_dim USING lion (verified);
CREATE INDEX hp_dim_tags ON hp_dim USING lion (tags);

CREATE TABLE hp_fact (
	id int PRIMARY KEY,
	dim_id int,					-- NULL in every 50th row, past dim in every 97th
	g int NOT NULL,				-- 1,000 groups
	v int NOT NULL
) WITH (autovacuum_enabled = off);
INSERT INTO hp_fact
SELECT i,
	   CASE WHEN i % 50 = 0 THEN NULL
			WHEN i % 97 = 0 THEN 100000 + i
			ELSE 1 + ((hashint8extended(i::bigint, 5) & 9223372036854775807) % 100000)::int END,
	   i % 1000,
	   i % 7
  FROM generate_series(1, 200000) i;
CREATE INDEX hp_fact_g ON hp_fact (g);
VACUUM (FREEZE, ANALYZE) hp_dim;
VACUUM (FREEZE, ANALYZE) hp_fact;

/*
 * hp() runs q through the heap probe - hash and merge joins off, LionOrderedJoin
 * off, Memoize as `memoize` says, and lion's paths priced at 1/`margin` (a
 * thousandth by default, so that the probe is the plan; 1 leaves the choice to
 * the prices) -
 * and through the ordinary plan (the probe and the walks off), compares the
 * two answers, as multisets unless `ordered`, and names the plan's join, a
 * Memoize over the probe, the probe's index and whether it is index-only.
 */
CREATE FUNCTION hp(q text, memoize boolean DEFAULT false,
				   ordered boolean DEFAULT false,
				   margin int DEFAULT 1000) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	r record;
	shape text := '';
	idx text := '';
	a text[] := '{}';
	b text[] := '{}';
BEGIN
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_memoize', memoize::text, true);
	PERFORM set_config('pg_lion.enable_heap_probe', 'on', true);
	PERFORM set_config('pg_lion.enable_ordered_join', 'off', true);
	PERFORM set_config('pg_lion.pushdown_margin', margin::text, true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ '(Nested Loop|Memoize|Custom Scan \(LionBtreeScan\)|Index Scan using|Index Only Scan using)' THEN
			shape := shape || CASE WHEN shape = '' THEN '' ELSE ' > ' END ||
				substring(ln from '(Nested Loop[ A-Za-z]*|Memoize|LionBtreeScan|Index Scan using [a-z_]+|Index Only Scan using [a-z_]+)');
		ELSIF ln ~ '^\s*Index: ' AND idx = '' THEN
			idx := substring(ln from 'Index: (.*)$');
		END IF;
	END LOOP;
	FOR r IN EXECUTE q LOOP
		a := a || r::text;
	END LOOP;

	PERFORM set_config('pg_lion.enable_heap_probe', 'off', true);
	PERFORM set_config('pg_lion.enable_btree_scan', 'off', true);
	PERFORM set_config('pg_lion.enable_ordered_scan', 'off', true);
	PERFORM set_config('pg_lion.pushdown_margin', '1', true);
	FOR r IN EXECUTE q LOOP
		b := b || r::text;
	END LOOP;
	PERFORM set_config('pg_lion.enable_btree_scan', 'on', true);
	PERFORM set_config('pg_lion.enable_ordered_scan', 'on', true);
	PERFORM set_config('pg_lion.enable_ordered_join', 'on', true);
	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_memoize', 'on', true);

	IF NOT ordered THEN
		SELECT coalesce(array_agg(e ORDER BY e), '{}') INTO a FROM unnest(a) e;
		SELECT coalesce(array_agg(e ORDER BY e), '{}') INTO b FROM unnest(b) e;
	END IF;
	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH: %s rows through the probe, %s ordinary; first %s / %s',
					  coalesce(array_length(a, 1), 0), coalesce(array_length(b, 1), 0),
					  a[1], b[1]);
	END IF;
	RETURN format('%s%s, %s rows', shape,
				  CASE WHEN idx = '' THEN '' ELSE ' (' || idx || ')' END,
				  coalesce(array_length(a, 1), 0));
END $$;

/*
 * hp_run() runs q through the probe under EXPLAIN ANALYZE, as hp() runs it,
 * and prints the probe's lines and counters, without the "actual" figures.
 */
CREATE FUNCTION hp_run(q text, memoize boolean DEFAULT false,
					   margin int DEFAULT 1000)
RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	inside boolean := false;
BEGIN
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_memoize', memoize::text, true);
	PERFORM set_config('pg_lion.enable_heap_probe', 'on', true);
	PERFORM set_config('pg_lion.pushdown_margin', margin::text, true);
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(LionBtreeScan\)' THEN
			inside := true;
		ELSIF ln ~ '->' THEN
			inside := false;
		END IF;
		IF inside OR ln ~ '(Memoize|Cache Key)' THEN
			RETURN NEXT btrim(regexp_replace(ln, '\s*\(actual.*\)', ''));
		ELSIF ln ~ 'Hits: [0-9]+ +Misses: [0-9]+' THEN
			RETURN NEXT substring(ln from 'Hits: [0-9]+ +Misses: [0-9]+');
		END IF;
	END LOOP;
	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_memoize', 'on', true);
	PERFORM set_config('pg_lion.enable_heap_probe', 'off', true);
	PERFORM set_config('pg_lion.pushdown_margin', '1', true);
END $$;

/*
 * hp_inner() plans q with hash and merge joins off, so that the loop is the
 * join and the prices choose its inner side, and names that side: the probe,
 * core's Index Scan, or something else.
 */
CREATE FUNCTION hp_inner(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	inner_side text := 'none';
BEGIN
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_memoize', 'off', true);
	PERFORM set_config('pg_lion.enable_heap_probe', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'on hp_dim d' THEN
			inner_side := substring(ln from '(Custom Scan \(LionBtreeScan\)|Index Scan using [a-z_]+|Index Only Scan using [a-z_]+|Seq Scan|Bitmap Heap Scan)');
		END IF;
	END LOOP;
	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_memoize', 'on', true);
	PERFORM set_config('pg_lion.enable_heap_probe', 'off', true);
	RETURN inner_side;
END $$;

-- 1. The shape: fact rows of a few groups joined to the dimension rows that
--    pass its lion filters, reading a column of the dimension the primary
--    key does not hold.  The probe is a LionBtreeScan of hp_dim_pkey in heap
--    mode - "Index: hp_dim_pkey" without "index only".
SELECT hp($$SELECT f.id, f.v, d.name FROM hp_fact f JOIN hp_dim d ON d.id = f.dim_id
			WHERE f.g < 10 AND d.grade = 'alpha' AND d.verified$$);
SELECT hp($$SELECT f.id, d.name, d.tags FROM hp_fact f JOIN hp_dim d ON d.id = f.dim_id
			WHERE f.g IN (3, 30, 300) AND d.tags && '{t1,t4}' AND d.grade <> 'delta'$$);
-- under a Memoize, which caches the probe's rows by the outer key, chosen
-- at the prices: 4,000 outer rows over 20 keys, the probe scanned once a key
-- and its set built once
SELECT hp($$SELECT f.id, d.name FROM hp_fact f JOIN hp_dim d ON d.id = f.g + 1
			WHERE f.g < 20 AND d.grade = 'alpha' AND d.verified$$,
		  memoize => true, margin => 1);
SELECT hp_run($$SELECT f.id, d.name FROM hp_fact f JOIN hp_dim d ON d.id = f.g + 1
				WHERE f.g < 20 AND d.grade = 'alpha' AND d.verified$$,
			  memoize => true, margin => 1);
-- a left join keeps the fact rows the probe finds nothing for
SELECT hp($$SELECT f.id, d.name FROM hp_fact f LEFT JOIN
				(SELECT * FROM hp_dim WHERE grade = 'alpha' AND verified) d
				ON d.id = f.dim_id
			WHERE f.g < 5$$);
-- ordered by the outer side's order, which the loop keeps
SELECT hp($$SELECT f.id, d.name FROM hp_fact f JOIN hp_dim d ON d.id = f.dim_id
			WHERE f.g = 7 AND d.grade = 'alpha' ORDER BY f.id LIMIT 20$$,
		  ordered => true);
-- a residual clause on the dimension, which no lion index answers, is the
-- probe's filter on the rows it fetches
SELECT hp($$SELECT f.id, d.name FROM hp_fact f JOIN hp_dim d ON d.id = f.dim_id
			WHERE f.g < 10 AND d.grade = 'alpha' AND d.name LIKE 'd1%'$$);
-- FOR SHARE of the dimension: the row is fetched, and locked, as core's is
SELECT hp($$SELECT f.id, d.name FROM hp_fact f JOIN hp_dim d ON d.id = f.dim_id
			WHERE f.g = 11 AND d.grade = 'alpha' FOR SHARE OF d$$);

-- 2. What the probe does: the set is built once and kept for every scan of
--    the loop, and the heap is read for the set's members alone - Heap
--    Fetches against Index Entries Walked.
SELECT hp_run($$SELECT f.id, d.name FROM hp_fact f JOIN hp_dim d ON d.id = f.dim_id
				WHERE f.g < 10 AND d.grade = 'alpha' AND d.verified$$);

-- 3. The choice, at the prices (pg_lion.competitor_pricing off, no margin):
--    the loop's inner side is the probe where the set rejects rows the loop
--    would fetch, and core's Index Scan where it rejects none - the probe's
--    set is then built for nothing.
SELECT hp_inner($$SELECT f.id, d.name FROM hp_fact f JOIN hp_dim d ON d.id = f.dim_id
				  WHERE f.g < 20 AND d.grade = 'alpha' AND d.verified
					AND d.tags && '{t1}'$$) AS selective;
SELECT hp_inner($$SELECT f.id, d.name FROM hp_fact f JOIN hp_dim d ON d.id = f.dim_id
				  WHERE f.g < 20 AND d.grade IN ('delta', 'gamma', 'alpha', 'beta')$$) AS rejects_none;
-- ... and a handful of outer rows does not pay for the set's build
SELECT hp_inner($$SELECT f.id, d.name FROM hp_fact f JOIN hp_dim d ON d.id = f.dim_id
				  WHERE f.id < 4 AND d.grade = 'alpha' AND d.verified$$) AS few;

-- 4. The switches: off, and with core's Index Scans off, no heap probe.
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET pg_lion.pushdown_margin = 1000;
SET pg_lion.enable_heap_probe = off;
EXPLAIN (COSTS OFF)
SELECT f.id, d.name FROM hp_fact f JOIN hp_dim d ON d.id = f.dim_id
 WHERE f.g < 10 AND d.grade = 'alpha';
SET pg_lion.enable_heap_probe = on;
SET enable_indexscan = off;
EXPLAIN (COSTS OFF)
SELECT f.id, d.name FROM hp_fact f JOIN hp_dim d ON d.id = f.dim_id
 WHERE f.g < 10 AND d.grade = 'alpha';
RESET enable_indexscan;
RESET pg_lion.enable_heap_probe;
RESET pg_lion.pushdown_margin;
RESET enable_hashjoin;
RESET enable_mergejoin;

-- 5. Rows written after the VACUUM: an updated dimension row moves out of
--    the set, a new one comes in, and the probe's answers follow the
--    snapshot as core's do.
UPDATE hp_dim SET grade = 'delta' WHERE id IN (SELECT dim_id FROM hp_fact WHERE g = 2 AND dim_id IS NOT NULL LIMIT 5);
UPDATE hp_dim SET grade = 'alpha', name = name || '-new' WHERE id IN (SELECT dim_id FROM hp_fact WHERE g = 2 AND dim_id IS NOT NULL ORDER BY id DESC LIMIT 5);
SELECT hp($$SELECT f.id, d.name FROM hp_fact f JOIN hp_dim d ON d.id = f.dim_id
			WHERE f.g < 5 AND d.grade = 'alpha'$$);

-- 6. What the probe returns (DESIGN.md §49, "The probe's projection"):
--    under a plain nested loop its scan tuple as it is, through the
--    relation's columns, which the loop reads what it needs from, as core's
--    Index Scan's heap tuple is read - in heap mode and index-only alike,
--    where the columns the index does not hold are NULL and nothing above
--    reads them; under a Memoize, which caches what the probe returns, the
--    columns the query needs alone; and those alone too where the relation's
--    tuple would be projected anyway, a dropped column in it, or holds no
--    whole-row value.  The answers are the ordinary plan's.
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_memoize = off;
SET pg_lion.enable_heap_probe = on;
SET pg_lion.pushdown_margin = 1000;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT f.id, d.name FROM hp_fact f JOIN hp_dim d ON d.id = f.dim_id
 WHERE f.g < 10 AND d.grade = 'alpha';
EXPLAIN (VERBOSE, COSTS OFF)
SELECT f.id, d.id FROM hp_fact f JOIN hp_dim d ON d.id = f.dim_id
 WHERE f.g < 10 AND d.grade = 'alpha';
EXPLAIN (VERBOSE, COSTS OFF)
SELECT f.id, d FROM hp_fact f JOIN hp_dim d ON d.id = f.dim_id
 WHERE f.g < 10 AND d.grade = 'alpha';
SET enable_memoize = on;
SET pg_lion.pushdown_margin = 1;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT f.id, d.name FROM hp_fact f JOIN hp_dim d ON d.id = f.g + 1
 WHERE f.g < 20 AND d.grade = 'alpha' AND d.verified;
RESET enable_memoize;
RESET pg_lion.pushdown_margin;
RESET pg_lion.enable_heap_probe;
RESET enable_hashjoin;
RESET enable_mergejoin;
SELECT hp($$SELECT f.id, d FROM hp_fact f JOIN hp_dim d ON d.id = f.dim_id
			WHERE f.g < 10 AND d.grade = 'alpha'$$);
ALTER TABLE hp_dim DROP COLUMN pad;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_memoize = off;
SET pg_lion.enable_heap_probe = on;
SET pg_lion.pushdown_margin = 1000;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT f.id, d.name FROM hp_fact f JOIN hp_dim d ON d.id = f.dim_id
 WHERE f.g < 10 AND d.grade = 'alpha';
RESET enable_memoize;
RESET pg_lion.pushdown_margin;
RESET pg_lion.enable_heap_probe;
RESET enable_hashjoin;
RESET enable_mergejoin;
SELECT hp($$SELECT f.id, f.v, d.name FROM hp_fact f JOIN hp_dim d ON d.id = f.dim_id
			WHERE f.g < 10 AND d.grade = 'alpha' AND d.verified$$);

DROP FUNCTION hp_inner(text);
DROP FUNCTION hp_run(text, boolean, int);
DROP FUNCTION hp(text, boolean, boolean, int);
DROP TABLE hp_fact;
DROP TABLE hp_dim;
DROP TYPE hp_grade;
