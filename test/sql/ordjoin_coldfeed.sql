-- A LionOrderedJoin's key set fed by the plan that is cheapest cold
-- (DESIGN.md §47.6, "The feed").
--
-- A key set's plan runs once, before the walk, and reads its table whole.
-- Core's cheapest path of the table prices its pages as if cached, so a
-- bitmap heap scan that reads most of the heap at random is taken over a
-- covering walk of the key's B-tree that reads a fraction of the pages in
-- order, and runs several times slower when the heap is not in the cache.
-- The node picks the key set's plan itself: of the table's unparameterized
-- paths, and a Gather over each partial one, the cheapest.  A covering
-- walk's price carries the cold reads of the bitmap heap scan it competes
-- with, as effective_cache_size says (bitmap_cold.sql).  Every answer is the
-- same whichever plan is taken.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;
SET default_statistics_target = 1000;
SET work_mem = '64MB';
-- the lion indexes' bitmap scans only, as a GIN index's: no plain scan of
-- them competes for the key set
SET pg_lion.enable_plain_scan = off;
CREATE SCHEMA ojcf;
SET search_path = ojcf, public;

-- the dimension: three tiers
CREATE TABLE co (
	pk int PRIMARY KEY,
	tier int NOT NULL,			-- lion: three values
	name text NOT NULL
) WITH (autovacuum_enabled = off);
INSERT INTO co
SELECT i, 1 + ((hashint8extended(i::bigint, 1) & 9223372036854775807) % 3)::int,
	   'c' || i
  FROM generate_series(1, 20000) i;
CREATE INDEX co_tier ON co USING lion (tier);

-- the fact: ten rows a key, walked by a B-tree of a rank
CREATE TABLE f (
	id int PRIMARY KEY,
	fk int NOT NULL,
	rank int NOT NULL
) WITH (autovacuum_enabled = off);
INSERT INTO f
SELECT i, 1 + ((hashint8extended(i::bigint, 3) & 9223372036854775807) % 20000)::int,
	   ((hashint8extended(i::bigint, 5) & 9223372036854775807) % 1000000)::int
  FROM generate_series(1, 200000) i;
CREATE INDEX f_rank ON f (rank DESC);

-- the key table: 50,000 rows of 300 bytes, 23 to a page, its key k under a
-- B-tree and c of 100 values at random under a lion index - two values of c
-- are a bitmap heap scan of some 800 of the 2,200 heap pages, or a walk of
-- the B-tree's 140, index-only
CREATE TABLE kt (id int, k int NOT NULL, c int NOT NULL, pad text)
	WITH (autovacuum_enabled = off);
INSERT INTO kt
SELECT i, 1 + (i::bigint * 7919 % 20000)::int,
	   ((hashint8extended(i::bigint, 7) & 9223372036854775807) % 100)::int,
	   repeat('x', 300)
  FROM generate_series(1, 50000) i;
CREATE INDEX kt_k ON kt (k);
CREATE INDEX kt_c ON kt USING lion (c);

VACUUM (FREEZE, ANALYZE) co;
VACUUM (FREEZE, ANALYZE) f;
VACUUM (FREEZE, ANALYZE) kt;

/*
 * ocf_set() sets, for the rest of the statement, every setting this test
 * moves to its default, no workers, core's joins off - so that the plan is
 * the node, whichever plan feeds its key set - and then the settings in sw,
 * name and value in turn.
 */
CREATE FUNCTION ocf_set(sw text[]) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE
	n text;
	i int;
BEGIN
	FOREACH n IN ARRAY ARRAY['effective_cache_size', 'pg_lion.cold_stream_page_cost',
							 'pg_lion.cold_page_cost',
							 'enable_bitmapscan', 'pg_lion.enable_ordered_join',
							 'parallel_setup_cost', 'parallel_tuple_cost',
							 'min_parallel_table_scan_size',
							 'min_parallel_index_scan_size'] LOOP
		PERFORM set_config(n, (SELECT boot_val FROM pg_settings WHERE name = n),
						   true);
	END LOOP;
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	FOR i IN 1 .. coalesce(array_length(sw, 1), 0) BY 2 LOOP
		PERFORM set_config(sw[i], sw[i + 1], true);
	END LOOP;
END $$;
/*
 * What feeds the node's key set under the settings in sw: the first scan of
 * kt in the plan - a bitmap heap scan, lion's covering walk, or another -
 * "under a Gather" where a Gather or Gather Merge between the node and it
 * runs it in workers, and whether the node is the plan at all.
 */
CREATE FUNCTION ocf_feed(q text, sw text[] DEFAULT '{}') RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	node boolean := false;
	gather text := '';
	kind text;
BEGIN
	PERFORM ocf_set(sw);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		kind := NULL;
		IF ln ~ 'Custom Scan \(LionOrdered(Join|Split)\)' THEN
			node := true;
		ELSIF node AND ln ~ '->  Gather' THEN
			gather := ' under a Gather';
		ELSIF ln ~ 'Bitmap Heap Scan on kt' THEN
			kind := 'bitmap heap scan';
		ELSIF ln ~ 'Custom Scan \(Lion(Ordered|BtreeScan)\) on kt' THEN
			kind := 'covering walk';
		ELSIF ln ~ ' on kt' THEN
			kind := 'other';
		END IF;
		IF kind IS NOT NULL THEN
			RETURN CASE WHEN node THEN kind || gather ELSE 'no node' END;
		END IF;
	END LOOP;
	RETURN 'no key set';
END $$;
/* q's rows under the settings in sw, in order, as one md5 */
CREATE FUNCTION ocf_rows(q text, sw text[] DEFAULT '{}') RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	r record;
	a text := '';
BEGIN
	PERFORM ocf_set(sw);
	FOR r IN EXECUTE q LOOP
		a := a || r::text || ' ';
	END LOOP;
	RETURN md5(a);
END $$;

-- ---------- 1. the feed ----------
-- The tables fit in the cache: the bitmap heap scan, at core's price, below
-- the walk's.  A cache of a megabyte holds a few percent of them: the
-- scan's 800 heap pages are cold, the walk's 140 B-tree pages hardly less
-- so, and the walk, its price carrying the bitmap heap scan's cold reads
-- (lo_cold_price()), feeds the key set - and not with the cold charge off.
-- With workers the walk is taken in them, under a Gather the node puts over
-- its partial path.
SELECT n, ocf_feed('SELECT f.id, co.name FROM f JOIN co ON co.pk = f.fk
					 WHERE co.tier = 1
					   AND f.fk IN (SELECT k FROM kt WHERE c IN (1, 2))
					 ORDER BY f.rank DESC, f.id LIMIT 20', sw) AS feed
  FROM (VALUES ('in the cache', '{}'::text[]),
			   ('a cache of 1MB', '{effective_cache_size, 1MB}'),
			   ('... no stream charge', '{effective_cache_size, 1MB, pg_lion.cold_stream_page_cost, 0}'),
			   ('... no bitmap scans', '{effective_cache_size, 1MB, enable_bitmapscan, off}'),
			   ('... in workers', '{effective_cache_size, 1MB, max_parallel_workers_per_gather, 2, parallel_setup_cost, 0, parallel_tuple_cost, 0, min_parallel_table_scan_size, 0, min_parallel_index_scan_size, 0}')) v(n, sw);

-- an EXISTS, the same key set
SELECT n, ocf_feed('SELECT f.id, co.name FROM f JOIN co ON co.pk = f.fk
					 WHERE co.tier = 2
					   AND EXISTS (SELECT 1 FROM kt WHERE kt.k = f.fk AND kt.c = 5)
					 ORDER BY f.rank DESC, f.id LIMIT 20', sw) AS feed
  FROM (VALUES ('in the cache', '{}'::text[]),
			   ('a cache of 1MB', '{effective_cache_size, 1MB}')) v(n, sw);

-- ---------- 2. the answers ----------
-- the same rows, in the same order, whatever feeds the key set, and with
-- the node off
SELECT count(DISTINCT ocf_rows(q, sw)) AS answers
  FROM (VALUES ('SELECT f.id, co.name FROM f JOIN co ON co.pk = f.fk
				  WHERE co.tier = 1
					AND f.fk IN (SELECT k FROM kt WHERE c IN (1, 2))
				  ORDER BY f.rank DESC, f.id LIMIT 20')) q(q),
	   (VALUES ('{}'::text[]),
			   ('{effective_cache_size, 1MB}'),
			   ('{effective_cache_size, 1MB, max_parallel_workers_per_gather, 2, parallel_setup_cost, 0, parallel_tuple_cost, 0, min_parallel_table_scan_size, 0, min_parallel_index_scan_size, 0}'),
			   ('{pg_lion.enable_ordered_join, off, enable_hashjoin, on}')) v(sw);
SELECT count(DISTINCT ocf_rows(q, sw)) AS answers
  FROM (VALUES ('SELECT f.id, co.name FROM f JOIN co ON co.pk = f.fk
				  WHERE co.tier = 2
					AND EXISTS (SELECT 1 FROM kt WHERE kt.k = f.fk AND kt.c = 5)
				  ORDER BY f.rank DESC, f.id LIMIT 20')) q(q),
	   (VALUES ('{}'::text[]),
			   ('{effective_cache_size, 1MB}'),
			   ('{pg_lion.enable_ordered_join, off, enable_hashjoin, on}')) v(sw);

-- ---------- 3. run through the walk ----------
-- the key set's keys, read through the walk under EXPLAIN ANALYZE: every
-- key of the 1,000 rows, once
DO $$
DECLARE
	ln text;
BEGIN
	PERFORM ojcf.ocf_set('{effective_cache_size, 1MB}');
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
		SELECT f.id, co.name FROM ojcf.f JOIN ojcf.co ON co.pk = f.fk
		 WHERE co.tier = 1
		   AND f.fk IN (SELECT k FROM ojcf.kt WHERE c IN (1, 2))
		 ORDER BY f.rank DESC, f.id LIMIT 20' LOOP
		IF ln ~ 'Key Set Keys|Custom Scan \(Lion(Ordered|BtreeScan)\) on kt' THEN
			RAISE NOTICE '%', regexp_replace(ln, '^\s+|\s*\(actual.*$', '', 'g');
		END IF;
	END LOOP;
END $$;
SELECT count(DISTINCT k) AS keys FROM kt WHERE c IN (1, 2);

DROP SCHEMA ojcf CASCADE;
