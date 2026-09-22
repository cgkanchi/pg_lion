-- lion_index_stats() and lion_index_verify() (DESIGN.md section 7).
\set VERBOSITY terse
SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS pg_lion;

SELECT setseed(0.42);

-- What the functions look like.
\df lion_index_stats
\df lion_index_verify

/*
 * Every shape the index can be in: an empty index, one built by ambuild,
 * one filled by aminsert, one that has been vacuumed, and one of each entry
 * kind (INLINE and CHAIN).
 */
CREATE TABLE lion_vfy_empty (k int4);
CREATE INDEX lion_vfy_empty_k ON lion_vfy_empty USING lion (k)
	WITH (inline_limit = 512);
SELECT * FROM lion_index_stats('lion_vfy_empty_k');
SELECT lion_index_verify('lion_vfy_empty_k');
SELECT lion_index_verify('lion_vfy_empty_k', true);

CREATE TABLE lion_vfy (i int4, k int4, t text, n numeric, u uuid);
INSERT INTO lion_vfy
SELECT i, i % 997, 'v' || (i % 13), (i % 71)::numeric / 4,
	   md5((i % 29)::text)::uuid
  FROM generate_series(1, 100000) i;

-- built by ambuild
CREATE INDEX lion_vfy_k ON lion_vfy USING lion (k);
CREATE INDEX lion_vfy_t ON lion_vfy USING lion (t);
CREATE INDEX lion_vfy_n ON lion_vfy USING lion (n) WITH (inline_limit = 64);
CREATE INDEX lion_vfy_u ON lion_vfy USING lion (u);

SELECT ordered, entries, inline_entries, containers, ntids
  FROM lion_index_stats('lion_vfy_k');
SELECT ordered, entries, inline_entries, containers, ntids
  FROM lion_index_stats('lion_vfy_t');
SELECT entries, inline_entries, ntids FROM lion_index_stats('lion_vfy_n');
SELECT entries, inline_entries, ntids FROM lion_index_stats('lion_vfy_u');

SELECT lion_index_verify('lion_vfy_k', true);
SELECT lion_index_verify('lion_vfy_t', true);
SELECT lion_index_verify('lion_vfy_n', true);
SELECT lion_index_verify('lion_vfy_u', true);

-- container bytes and free bytes are consistent with the pages they live on
SELECT container_bytes > 0 AS has_bytes,
	   container_bytes + free_bytes <
	   (leaf_pages + internal_pages + container_pages) * current_setting('block_size')::int8
	   AS fits_in_its_pages
  FROM lion_index_stats('lion_vfy_t');

-- filled by aminsert on top of the built index
INSERT INTO lion_vfy
SELECT i, i % 997, 'v' || (i % 13), (i % 71)::numeric / 4,
	   md5((i % 29)::text)::uuid
  FROM generate_series(100001, 150000) i;
SELECT entries, ntids FROM lion_index_stats('lion_vfy_k');
SELECT lion_index_verify('lion_vfy_k', true);
SELECT lion_index_verify('lion_vfy_t', true);
SELECT lion_index_verify('lion_vfy_n', true);

-- a non-HOT update: the indexed column changes, so a new TID is indexed
UPDATE lion_vfy SET k = 998 WHERE i % 1000 = 0;
SELECT lion_index_verify('lion_vfy_k', true);
SELECT entries FROM lion_index_stats('lion_vfy_k');

/*
 * heapallindexed with HOT updates: the updated column is not indexed and the
 * table has room on its pages, so the new tuples are heap-only and the index
 * still points at the root of each HOT chain.  The heap scan has to follow
 * that mapping, or every updated row would look unindexed.
 */
CREATE TABLE lion_vfy_hot (i int4, k int4, payload text) WITH (fillfactor = 50);
INSERT INTO lion_vfy_hot
SELECT i, i % 97, 'p' || i FROM generate_series(1, 50000) i;
CREATE INDEX lion_vfy_hot_k ON lion_vfy_hot USING lion (k);
UPDATE lion_vfy_hot SET payload = 'q' || i WHERE i % 3 = 0;
SELECT pg_stat_get_xact_tuples_hot_updated('lion_vfy_hot'::regclass) > 0
	   AS had_hot_updates;
SELECT lion_index_verify('lion_vfy_hot_k', true);
SELECT entries, ntids FROM lion_index_stats('lion_vfy_hot_k');
-- the HOT chains survive a VACUUM (which prunes them) unchanged
VACUUM lion_vfy_hot;
SELECT lion_index_verify('lion_vfy_hot_k', true);
SELECT entries, ntids FROM lion_index_stats('lion_vfy_hot_k');

/*
 * Deletes that the verification has to ignore: the rows are gone from the
 * heap scan's snapshot but their TIDs are still in the index, which is not
 * an error (only missing index entries are).
 */
BEGIN;
DELETE FROM lion_vfy WHERE i % 5 = 0;
SELECT lion_index_verify('lion_vfy_k', true);
SELECT lion_index_verify('lion_vfy_t', true);
ROLLBACK;

-- ... and after the delete is rolled back, every row must be found again
SELECT lion_index_verify('lion_vfy_k', true);

-- committed deletes, before and after VACUUM removes the TIDs
DELETE FROM lion_vfy WHERE i % 5 = 0;
SELECT lion_index_verify('lion_vfy_k', true);
SELECT lion_index_verify('lion_vfy_u', true);
VACUUM lion_vfy;
SELECT lion_index_verify('lion_vfy_k', true);
SELECT lion_index_verify('lion_vfy_u', true);
SELECT entries, ntids FROM lion_index_stats('lion_vfy_k');

/*
 * Sparse segments (DESIGN.md section 13).  i is unique, so every one of its
 * container keys has a single member and the whole index is segments; the
 * verification checks their internal order, that their ranges do not overlap
 * their neighbours', and that no container key inside one has reached the
 * threshold that gives it a container.
 */
CREATE INDEX lion_vfy_i ON lion_vfy USING lion (i);
SELECT containers, sparse_segments > 0 AS has_segments,
	   sparse_members = ntids AS every_tid_in_a_segment
  FROM lion_index_stats('lion_vfy_i');
SELECT lion_index_verify('lion_vfy_i', true);
DELETE FROM lion_vfy WHERE i % 7 = 0;
VACUUM lion_vfy;
SELECT lion_index_verify('lion_vfy_i', true);
SELECT sparse_members = ntids AS still_all_segments,
	   ntids = (SELECT count(*) FROM lion_vfy WHERE i IS NOT NULL) AS ntids_matches_heap
  FROM lion_index_stats('lion_vfy_i');

-- a partial index only has to contain the rows its predicate selects
CREATE INDEX lion_vfy_part ON lion_vfy USING lion (t) WHERE k < 100;
SELECT lion_index_verify('lion_vfy_part', true);
SELECT entries, ntids FROM lion_index_stats('lion_vfy_part');
INSERT INTO lion_vfy VALUES (200001, 5, 'vpart', 1.0, NULL, NULL);
INSERT INTO lion_vfy VALUES (200002, 500, 'vpart', 1.0, NULL, NULL);
SELECT lion_index_verify('lion_vfy_part', true);
SELECT ntids FROM lion_index_stats('lion_vfy_part');

-- an expression index is verified through the same path
CREATE INDEX lion_vfy_expr ON lion_vfy USING lion ((k % 10));
SELECT lion_index_verify('lion_vfy_expr', true);
SELECT entries FROM lion_index_stats('lion_vfy_expr');

-- NULL keys are not indexed and must not be reported as missing
INSERT INTO lion_vfy VALUES (200003, NULL, NULL, NULL, NULL, NULL);
SELECT lion_index_verify('lion_vfy_k', true);
SELECT lion_index_verify('lion_vfy_t', true);

-- unlogged relations work the same way
CREATE UNLOGGED TABLE lion_vfy_unl (i int4, k int4);
CREATE INDEX lion_vfy_unl_k ON lion_vfy_unl USING lion (k);
INSERT INTO lion_vfy_unl SELECT i, i % 37 FROM generate_series(1, 20000) i;
SELECT lion_index_verify('lion_vfy_unl_k', true);
SELECT entries, ntids FROM lion_index_stats('lion_vfy_unl_k');

-- Things that are not lion indexes.
CREATE INDEX lion_vfy_bt ON lion_vfy_unl (k);
SELECT lion_index_verify('lion_vfy_bt');
SELECT lion_index_stats('lion_vfy_bt');
SELECT lion_index_verify('lion_vfy_unl');
SELECT lion_index_stats('lion_vfy_unl');
SELECT lion_index_verify('no_such_relation');

DROP TABLE lion_vfy, lion_vfy_empty, lion_vfy_unl, lion_vfy_hot;
