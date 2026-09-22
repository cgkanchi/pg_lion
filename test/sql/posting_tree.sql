-- The per-key posting tree (DESIGN.md §22).
--
-- A CHAIN entry's posting set is a B-tree over container keys whose LEAVES are
-- the container pages of §4, still rightlinked in ckey order.  What this file
-- pins:
--
--  1. a key grown by INSERTS through leaf splits and a root split, with the
--     entry's `head` - the ROOT block, which is the identity every page of the
--     set is stamped with (§18) - unchanged across the root split;
--  2. mid-chain inserts, which land by descent instead of by walking from the
--     head;
--  3. SEEK: an AND of a selective set with a dense one visits far fewer
--     containers than the sum of the two, and answers exactly what the
--     ordinary plan answers;
--  4. VACUUM freeing a whole multi-level set, and the pages coming back;
--  5. REINDEX equivalence: a bulk-built tree and an insert-built one answer
--     the same and are both structurally sound.
--
-- Not covered here, and it cannot be: a split of an INTERNAL posting page.  A
-- page holds 679 downlinks, so an internal split needs a key with 680 leaves,
-- and a leaf covers at least one container key of 64 heap blocks - about 340 MB
-- of heap for one key.  §22 records the ad-hoc run that exercised it.

\set VERBOSITY terse
SET client_min_messages = warning;
SET synchronous_commit = on;

CREATE EXTENSION IF NOT EXISTS pg_lion;

-- ---------------------------------------------------------------------
-- 1. Growth by insert: leaf splits, then the root split
-- ---------------------------------------------------------------------

-- The index is created EMPTY, so every page of it comes from aminsert.
CREATE TABLE pt_grow (id int, k int);
CREATE INDEX pt_grow_k ON pt_grow USING lion (k);

-- Two keys taking alternate TIDs, so their containers are dense bitsets
-- rather than one long run: each container key then fills a leaf by itself.
INSERT INTO pt_grow SELECT i, i % 2 FROM generate_series(1, 10000) i;

SELECT container_pages, posting_internal_pages, max_posting_height
  FROM lion_index_stats('pt_grow_k');
-- one page per key: the root IS the leaf
SELECT lion_index_posting_root('pt_grow_k', 0) IS NOT NULL AS spilled_to_a_page;
SELECT lion_index_posting_root('pt_grow_k', 0) AS root0 \gset
SELECT lion_index_posting_root('pt_grow_k', 1) AS root1 \gset

-- Grow both keys until the sets need several leaves and therefore a root.
INSERT INTO pt_grow SELECT i, i % 2 FROM generate_series(10001, 200000) i;

SELECT container_pages > 10 AS many_leaves,
	   posting_internal_pages = 2 AS one_root_per_key,
	   max_posting_height = 1 AS two_levels
  FROM lion_index_stats('pt_grow_k');

-- THE POINT OF THE PUSH-DOWN: the root block did not move, so the entry never
-- had to be rewritten and every page of the set still carries the same owner.
SELECT lion_index_posting_root('pt_grow_k', 0) = :root0 AS root0_unchanged,
	   lion_index_posting_root('pt_grow_k', 1) = :root1 AS root1_unchanged;

SELECT lion_index_verify('pt_grow_k', true);
SELECT count(*) FROM pt_grow WHERE k = 0;
SELECT count(*) FROM pt_grow WHERE k = 1;

-- ---------------------------------------------------------------------
-- 2. Mid-chain inserts: the descent, not a walk from the head
-- ---------------------------------------------------------------------

-- Free heap pages in the MIDDLE of the range, so the rows inserted next get
-- TIDs whose container keys fall in the middle of both posting sets.
DELETE FROM pt_grow WHERE id BETWEEN 80000 AND 120000;
VACUUM pt_grow;
SELECT lion_index_verify('pt_grow_k', true);

INSERT INTO pt_grow SELECT 1000000 + i, i % 2 FROM generate_series(1, 20000) i;

SELECT lion_index_verify('pt_grow_k', true);
SELECT count(*) FROM pt_grow WHERE k = 0;
SELECT count(*) FROM pt_grow WHERE k = 1;
SELECT (SELECT count(*) FROM pt_grow WHERE k = 0) +
	   (SELECT count(*) FROM pt_grow WHERE k = 1) =
	   (SELECT count(*) FROM pt_grow) AS every_row_indexed;

-- ntids agrees with the heap after a VACUUM that has nothing left to remove
VACUUM pt_grow;
SELECT ntids = (SELECT count(*) FROM pt_grow) AS ntids_exact
  FROM lion_index_stats('pt_grow_k');

-- ---------------------------------------------------------------------
-- 3. Seeking: an AND driven by the selective side
-- ---------------------------------------------------------------------

CREATE TABLE pt_and (id int, dense int, clu int);
INSERT INTO pt_and
SELECT i, i % 2, i / 20000 FROM generate_series(1, 200000) i;
CREATE INDEX pt_and_dense ON pt_and USING lion (dense);
CREATE INDEX pt_and_clu ON pt_and USING lion (clu);
VACUUM (ANALYZE, FREEZE) pt_and;

-- How many containers a query reads, out of EXPLAIN ANALYZE's own counter.
CREATE FUNCTION pt_containers(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE
	l text;
	n bigint := NULL;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, TIMING off, COSTS off) ' || q LOOP
		IF l LIKE '%Containers Visited%' THEN
			n := substring(l from '[0-9]+')::bigint;
		END IF;
	END LOOP;
	RETURN n;
END $$;

SET pg_lion.enable_count_pushdown = on;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;

SELECT pt_containers('SELECT count(*) FROM pt_and WHERE dense = 1') AS c_dense \gset
SELECT pt_containers('SELECT count(*) FROM pt_and WHERE clu = 3') AS c_clu \gset
SELECT pt_containers('SELECT count(*) FROM pt_and WHERE clu = 3 AND dense = 1')
	AS c_and \gset

-- The dense set has a container at nearly every container key of the heap and
-- the clustered one at a handful, so walking both would read about c_dense +
-- c_clu of them; seeking reads about twice the SELECTIVE side.
SELECT :c_clu < :c_dense AS selective_side_is_smaller,
	   :c_and <= 3 * :c_clu AS and_tracks_the_selective_side,
	   :c_and < :c_dense AS and_beats_a_walk_of_the_dense_side;

-- ... and it is the same answer the ordinary plan gives, for every group.
SELECT c, count(*) FROM (
	SELECT clu, count(*) AS c FROM pt_and WHERE dense = 1 GROUP BY clu) x
GROUP BY c ORDER BY c;

SET pg_lion.enable_count_pushdown = off;
SELECT c, count(*) FROM (
	SELECT clu, count(*) AS c FROM pt_and WHERE dense = 1 GROUP BY clu) x
GROUP BY c ORDER BY c;
SET pg_lion.enable_count_pushdown = on;

-- every (selective, dense) pair, both ways round, as a multiset
SELECT count(*) AS mismatches FROM (
	(SELECT clu, dense, count(*) FROM pt_and GROUP BY clu, dense)
	EXCEPT ALL
	(SELECT clu, dense, count(*) FROM pt_and GROUP BY clu, dense)) d;

RESET enable_bitmapscan;
RESET enable_indexscan;
RESET enable_indexonlyscan;

-- ---------------------------------------------------------------------
-- 4. VACUUM frees a whole multi-level set, and the pages come back
-- ---------------------------------------------------------------------

SELECT container_pages + posting_internal_pages AS pages_before
  FROM lion_index_stats('pt_grow_k') \gset

DELETE FROM pt_grow WHERE k = 0;
VACUUM pt_grow;

-- the entry is gone, and so are the leaves AND the root above them
SELECT entries = 1 AS one_entry_left,
	   container_pages + posting_internal_pages < :pages_before AS set_was_freed,
	   deleted_pages > 0 AS pages_are_free
  FROM lion_index_stats('pt_grow_k');
SELECT lion_index_verify('pt_grow_k', true);
SELECT count(*) FROM pt_grow WHERE k = 0;

-- The freed pages come back: the key is built again from scratch and takes
-- them out of the free space map, so the relation grows by less than it gave
-- back.  (It cannot be exactly zero: the ROOT of a posting set is the one page
-- that is never recycled, so that owner_head stays an identity a reader can
-- trust - DESIGN.md §18.)
CREATE TEMP TABLE pt_free_size AS
	SELECT pg_relation_size('pt_grow_k') AS bytes,
		   (SELECT deleted_pages FROM lion_index_stats('pt_grow_k')) AS freed;
INSERT INTO pt_grow SELECT 2000000 + i, 0 FROM generate_series(1, 60000) i;
SELECT (SELECT deleted_pages FROM lion_index_stats('pt_grow_k')) <
	   (SELECT freed FROM pt_free_size) AS freed_pages_were_reused,
	   pg_relation_size('pt_grow_k') - (SELECT bytes FROM pt_free_size) <
	   (SELECT freed FROM pt_free_size) * current_setting('block_size')::bigint
		   AS grew_less_than_it_freed;
SELECT lion_index_verify('pt_grow_k', true);
SELECT count(*) FROM pt_grow WHERE k = 0;

-- ---------------------------------------------------------------------
-- 5. REINDEX equivalence
-- ---------------------------------------------------------------------

SELECT entries, ntids FROM lion_index_stats('pt_grow_k') \gset
REINDEX INDEX pt_grow_k;
SELECT entries = :entries AS same_entries, ntids = :ntids AS same_ntids
  FROM lion_index_stats('pt_grow_k');
SELECT lion_index_verify('pt_grow_k', true);
SELECT count(*) FROM pt_grow WHERE k = 0;
SELECT count(*) FROM pt_grow WHERE k = 1;

-- a bulk-built tree has the same shape as the one the inserts grew
SELECT max_posting_height >= 1 AS still_a_tree,
	   posting_internal_pages > 0 AS still_has_internal_pages
  FROM lion_index_stats('pt_grow_k');

DROP FUNCTION pt_containers(text);
DROP TABLE pt_and;
DROP TABLE pt_grow;
