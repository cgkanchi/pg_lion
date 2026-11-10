-- A heap whose visibility map is more than one page (DESIGN.md §9, "The map
-- pages a reader keeps pinned").  A map page covers 32672 heap blocks at
-- 8 kB pages, so a heap is some 255 MB before a reader asks for a second one,
-- and every other test's heap fits in the first.  This one is 33024 blocks
-- of one row each - an 8000-byte pad stored inline - so that block b holds
-- the row blk = b: map page 0 covers blocks 0..32671, map page 1 blocks
-- 32672..33023, and lion's container 510, blocks 32640..32703, straddles
-- the two.  The table is unlogged, so its 270 MB write no WAL, and it is
-- dropped at the end.
--
-- After VACUUM every block is all-visible.  Then the rows of eight blocks are
-- deleted, six of them on map page 1, each at a byte of that page whose twin
-- on map page 0 still says all-visible, and two on map page 0, one of them
-- in the straddling container.  A reader that took a block's bits from the
-- wrong map page, or either half of the straddling container from the
-- other's page, counts a deleted row or rechecks a block it need not: each
-- answer is compared with the same query's answer without lion, and the
-- blocks each count took from the map and rechecked are counted exactly.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

CREATE UNLOGGED TABLE vmp (blk int NOT NULL, c4 int NOT NULL, fk int NOT NULL,
						   pad text) WITH (autovacuum_enabled = off);
ALTER TABLE vmp ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO vmp
SELECT i, i % 4, i % 500, repeat('v', 8000) FROM generate_series(0, 33023) i;
CREATE INDEX vmp_c4 ON vmp USING lion (c4);
CREATE INDEX vmp_fk ON vmp USING lion (fk);
CREATE INDEX vmp_blk ON vmp (blk) INCLUDE (c4);
VACUUM (FREEZE, ANALYZE) vmp;

-- the dimension of the join: region 'r1' picks 167 of the 500 keys, each of
-- whose 66 rows run over both map pages
CREATE TABLE vmd (pk int PRIMARY KEY, region text NOT NULL)
	WITH (autovacuum_enabled = off);
INSERT INTO vmd SELECT i, 'r' || (i % 3) FROM generate_series(0, 499) i;
VACUUM ANALYZE vmd;

/*
 * vmp_node() runs q under EXPLAIN ANALYZE with lion's nodes the only paths
 * left - core's scans and joins disabled - and returns the lion node's JSON
 * object, whose keys are the counters as the text format names them.
 */
CREATE FUNCTION vmp_node(q text) RETURNS jsonb
LANGUAGE plpgsql AS $$
DECLARE
	e jsonb;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_tidscan', 'off', true);
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	PERFORM set_config('pg_lion.pushdown_margin', '1000', true);
	EXECUTE 'EXPLAIN (ANALYZE, FORMAT JSON, COSTS OFF, SUMMARY OFF, TIMING OFF, BUFFERS OFF) '
		|| q INTO e;
	RETURN jsonb_path_query_first(e,
		'$.** ? (@."Custom Plan Provider" starts with "Lion")');
END $$;

/*
 * vmp_check() runs q the same way, and again with every lion path off and
 * core's scans on, and compares the rows, in order: it returns them if they
 * agree and are few, and how many if they agree and are many.
 */
CREATE FUNCTION vmp_check(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	a text[];
	b text[];
	wrapped text := format('SELECT coalesce(array_agg(s::text), ''{}'') FROM (%s) s', q);
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_tidscan', 'off', true);
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	PERFORM set_config('pg_lion.pushdown_margin', '1000', true);
	EXECUTE wrapped INTO a;

	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_nestloop', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('pg_lion.enable_ordered_scan', 'off', true);
	PERFORM set_config('pg_lion.enable_btree_scan', 'off', true);
	EXECUTE wrapped INTO b;
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('pg_lion.enable_ordered_scan', 'on', true);
	PERFORM set_config('pg_lion.enable_btree_scan', 'on', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
	PERFORM set_config('pg_lion.pushdown_margin', '1', true);

	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH: %s rows through lion, %s without; first %s / %s',
					  cardinality(a), cardinality(b), a[1], b[1]);
	END IF;
	IF cardinality(a) > 8 THEN
		RETURN format('%s rows, the same', cardinality(a));
	END IF;
	RETURN array_to_string(a, ' ');
END $$;

-- one row to a block, in order, and a map of two pages
SELECT count(*) FILTER (WHERE (ctid::text::point)[0] <> blk) AS misplaced,
	   pg_relation_size('vmp') / 8192 AS heap_blocks,
	   pg_relation_size('vmp', 'vm') / 8192 AS map_pages
FROM vmp;

DELETE FROM vmp WHERE blk IN (100, 32668,	-- map page 0
							  32672, 32675, 32700,	-- page 1, container 510
							  32704, 33020, 33023);	-- page 1, past it

-- rows in index-only mode, from a covering B-tree, first: the heap is read
-- for the six deleted blocks of the key, whose rows are not returned, and for
-- no other.  (Reading a deleted row's block can prune it and mark the
-- B-tree's entry dead, after which a walk skips the entry without asking
-- the map, so this is the walk that has to ask it.)
SELECT n->>'Custom Plan Provider' AS node, n->>'Ordered By' AS ordered_by,
	   (n->>'Actual Rows')::numeric::int AS rows,
	   (n->>'Heap Fetches')::int AS heap_fetches
FROM vmp_node('SELECT blk FROM vmp WHERE c4 = 0 ORDER BY blk') n;
SELECT vmp_check('SELECT blk FROM vmp WHERE c4 = 0 ORDER BY blk');

-- a count: 8256 rows on blocks 0 mod 4, less the six deleted; the six
-- blocks are rechecked and every other one is taken from the map, the
-- straddling container's from both pages
SELECT vmp_check('SELECT count(*) FROM vmp WHERE c4 = 0');
SELECT n->>'Custom Plan Provider' AS node,
	   (n->>'Heap Blocks Skipped via VM')::int AS from_map,
	   (n->>'Heap Blocks Rechecked')::int AS rechecked
FROM vmp_node('SELECT count(*) FROM vmp WHERE c4 = 0') n;

-- every block, by group: the eight deleted blocks rechecked
SELECT vmp_check('SELECT c4, count(*) FROM vmp GROUP BY c4 ORDER BY c4');
SELECT n->>'Custom Plan Provider' AS node,
	   (n->>'Heap Blocks Skipped via VM')::int AS from_map,
	   (n->>'Heap Blocks Rechecked')::int AS rechecked
FROM vmp_node('SELECT c4, count(*) FROM vmp GROUP BY c4') n;

-- a count per key of a join, each key's rows on both map pages: the two
-- pages are pinned once each, not once per key per page as with a single
-- pin, and the three deleted blocks whose keys join (100, 32672, 32675) are
-- rechecked
SELECT vmp_check('SELECT count(*) FROM vmp f JOIN vmd d ON f.fk = d.pk WHERE d.region = ''r1''');
SELECT n->>'Custom Plan Provider' AS node,
	   (n->>'Join Keys Looked Up')::int AS keys,
	   (n->>'Visibility Map Pages Pinned')::int AS map_pins,
	   (n->>'Heap Blocks Skipped via VM')::int AS from_map,
	   (n->>'Heap Blocks Rechecked')::int AS rechecked
FROM vmp_node('SELECT count(*) FROM vmp f JOIN vmd d ON f.fk = d.pk WHERE d.region = ''r1''') n;

DROP FUNCTION vmp_node(text);
DROP FUNCTION vmp_check(text);
DROP TABLE vmp, vmd;
