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
	   (leaf_pages + internal_pages + container_pages + posting_internal_pages)
	   * current_setting('block_size')::int8
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
-- (the xact counters are read inside the updating transaction: after it
-- commits they are pending only until the backend flushes them, which a slow
-- backend does before the next statement arrives)
BEGIN;
UPDATE lion_vfy_hot SET payload = 'q' || i WHERE i % 3 = 0;
SELECT pg_stat_get_xact_tuples_hot_updated('lion_vfy_hot'::regclass) > 0
	   AS had_hot_updates;
COMMIT;
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

-- a partial index only has to contain the rows its predicate selects: the
-- first row below is one it selects (a new key, 'vpart'), the second is not
CREATE INDEX lion_vfy_part ON lion_vfy USING lion (t) WHERE k < 100;
SELECT lion_index_verify('lion_vfy_part', true);
SELECT entries, ntids FROM lion_index_stats('lion_vfy_part');
INSERT INTO lion_vfy VALUES (200001, 5, 'vpart', 1.0, NULL);
INSERT INTO lion_vfy VALUES (200002, 500, 'vpart', 1.0, NULL);
SELECT lion_index_verify('lion_vfy_part', true);
SELECT entries, ntids FROM lion_index_stats('lion_vfy_part');

-- an expression index is verified through the same path
CREATE INDEX lion_vfy_expr ON lion_vfy USING lion ((k % 10));
SELECT lion_index_verify('lion_vfy_expr', true);
SELECT entries FROM lion_index_stats('lion_vfy_expr');

/*
 * NULL keys ARE indexed, in the column's reserved NULL entry (DESIGN.md §14),
 * and heapallindexed looks for such a row there.  The first row is NULL in
 * every column; the second is NULL only in t and satisfies the partial
 * index's predicate, so the partial index gets a NULL entry too, and the
 * expression index gets one for the NULL k of the first.
 */
INSERT INTO lion_vfy VALUES (200003, NULL, NULL, NULL, NULL);
INSERT INTO lion_vfy VALUES (200004, 7, NULL, 2.0, NULL);
SELECT lion_index_verify('lion_vfy_k', true);
SELECT lion_index_verify('lion_vfy_t', true);
SELECT lion_index_verify('lion_vfy_n', true);
SELECT lion_index_verify('lion_vfy_part', true);
SELECT lion_index_verify('lion_vfy_expr', true);
SELECT null_tids FROM lion_index_stats('lion_vfy_k');
SELECT null_tids FROM lion_index_stats('lion_vfy_t');
SELECT null_tids FROM lion_index_stats('lion_vfy_part');
SELECT null_tids FROM lion_index_stats('lion_vfy_expr');

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

/*
 * Damaged pages (2026-09-25 review).  verify() must ERROR on each of these -
 * never crash, read past a page or write to the index it checks - and
 * lion_index_stats(), which is granted to pg_stat_scan_tables and reports
 * damage to nobody, must not read past a page either: it leaves out what it
 * cannot read.
 *
 * The damage is written into the relation file with dd, through COPY TO
 * PROGRAM, and has to be read back from the file rather than from a buffer.
 * A TEMPORARY index arranges that: its build writes the pages straight to the
 * file and nothing logs them afterwards, so nothing reads them back - under
 * wal_level = minimal a small permanent index is logged at commit, through
 * shared buffers - and its buffers are this backend's own.  So every case
 * builds an index of its own and damages it before anything has read it; the
 * layout comes from the line pointers in the file, or from a twin built the
 * same way and read instead.  The page checksum is then wrong too, and
 * ignore_checksum_failure lets the read through; the WARNING that gives, and
 * whether it gives one at all, depends on the cluster, so client_min_messages
 * hides it.  Line pointers are written for a little-endian machine.
 */
CREATE FUNCTION lion_vfy_peek(idx regclass, blk int8, pos int8, len int8)
RETURNS bytea LANGUAGE sql AS $$
	SELECT pg_read_binary_file(pg_relation_filepath(idx),
							   blk * current_setting('block_size')::int8 + pos, len)
$$;
CREATE FUNCTION lion_vfy_poke(idx regclass, blk int8, pos int8, bytes bytea)
RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	oct text := '';
	b int;
BEGIN
	FOR i IN 0 .. length(bytes) - 1 LOOP
		b := get_byte(bytes, i);
		oct := oct || '\' || (b >> 6)::text || ((b >> 3) & 7)::text || (b & 7)::text;
	END LOOP;
	EXECUTE format('COPY (SELECT 1) TO PROGRAM %L',
				   format('cat >/dev/null; printf ''%s'' | dd of=%s bs=1 seek=%s conv=notrunc 2>/dev/null',
						  oct, pg_relation_filepath(idx),
						  blk * current_setting('block_size')::int8 + pos));
END $$;
-- where item off of block blk begins (lp_off, the low 15 bits)
CREATE FUNCTION lion_vfy_item(idx regclass, blk int8, off int)
RETURNS int8 LANGUAGE sql AS $$
	SELECT (get_byte(lp, 0) + (get_byte(lp, 1) & 127) * 256)::int8
	  FROM lion_vfy_peek(idx, blk, 24 + 4 * (off - 1), 4) lp
$$;

CREATE TEMP TABLE lion_vfy_bad (k int4);
INSERT INTO lion_vfy_bad SELECT i % 50 FROM generate_series(1, 20000) i;
CREATE TEMP TABLE lion_vfy_badp (id int4, k int4);
INSERT INTO lion_vfy_badp SELECT i, i % 2 FROM generate_series(1, 60000) i;
SET ignore_checksum_failure = on;

-- (1) a line pointer that points past the page
CREATE INDEX lion_vfy_bad_lp ON lion_vfy_bad USING lion (k);
SELECT lion_vfy_poke('lion_vfy_bad_lp', 1, 24 + 4 * 2, '\xf8fffeff');
SET client_min_messages = error;
SELECT lion_index_verify('lion_vfy_bad_lp');
SELECT entries FROM lion_index_stats('lion_vfy_bad_lp');
SET client_min_messages = warning;

-- (2) an entry of a key column the index does not have, which the order
-- check used to hand to lion_column() before anything had checked it
CREATE INDEX lion_vfy_bad_attno ON lion_vfy_bad USING lion (k);
SELECT lion_vfy_poke('lion_vfy_bad_attno', 1,
					 lion_vfy_item('lion_vfy_bad_attno', 1, 3) + 20, '\xffff');
SET client_min_messages = error;
SELECT lion_index_verify('lion_vfy_bad_attno');
SELECT entries FROM lion_index_stats('lion_vfy_bad_attno');
SET client_min_messages = warning;

-- (3) a key longer than its entry: the payload length computed from it used
-- to underflow, and lion_index_stats() walked the "payload" off the page
CREATE INDEX lion_vfy_bad_keylen ON lion_vfy_bad USING lion (k);
SELECT lion_vfy_poke('lion_vfy_bad_keylen', 1,
					 lion_vfy_item('lion_vfy_bad_keylen', 1, 3) + 6, '\xffff');
SET client_min_messages = error;
SELECT lion_index_verify('lion_vfy_bad_keylen');
SELECT entries FROM lion_index_stats('lion_vfy_bad_keylen');
SET client_min_messages = warning;

-- (4) a posting-tree downlink to InvalidBlockNumber, which ReadBuffer() takes
-- for P_NEW on 16 to 18: the check used to EXTEND the index it was checking
CREATE INDEX lion_vfy_badp_twin ON lion_vfy_badp USING lion (k);
SELECT lion_index_posting_root('lion_vfy_badp_twin', 0) AS root \gset
SELECT max_posting_height FROM lion_index_stats('lion_vfy_badp_twin');
CREATE INDEX lion_vfy_bad_child ON lion_vfy_badp USING lion (k);
SELECT pg_relation_size('lion_vfy_bad_child') AS size_before \gset
SELECT lion_vfy_poke('lion_vfy_bad_child', :root,
					 lion_vfy_item('lion_vfy_bad_child', :root, 1) + 4, '\xffffffff');
SET client_min_messages = error;
SELECT lion_index_verify('lion_vfy_bad_child');
SET client_min_messages = warning;
SELECT pg_relation_size('lion_vfy_bad_child') = :size_before AS not_extended;

-- (5) a container of no known type on a posting leaf
CREATE INDEX lion_vfy_bad_type ON lion_vfy_badp USING lion (k);
SELECT lion_vfy_poke('lion_vfy_bad_type', :root + 1,
					 lion_vfy_item('lion_vfy_bad_type', :root + 1, 1) + 6, '\x7f');
SET client_min_messages = error;
SELECT lion_index_verify('lion_vfy_bad_type');
SELECT containers FROM lion_index_stats('lion_vfy_bad_type');
SET client_min_messages = warning;

RESET ignore_checksum_failure;
DROP TABLE lion_vfy_bad, lion_vfy_badp;
DROP FUNCTION lion_vfy_peek(regclass, int8, int8, int8);
DROP FUNCTION lion_vfy_poke(regclass, int8, int8, bytea);
DROP FUNCTION lion_vfy_item(regclass, int8, int);

/*
 * lion_index_posting_root() takes a column VALUE, and a multi-key column's
 * entries are keys extracted from values (DESIGN.md §17): a whole tsvector
 * hashed as if it were one lexeme named no entry at all.  It refuses such a
 * column, as the count functions do.
 */
CREATE TABLE lion_vfy_mk (tags text[], doc tsvector);
INSERT INTO lion_vfy_mk VALUES ('{a,b}', 'a b');
CREATE INDEX lion_vfy_mk_tags ON lion_vfy_mk USING lion (tags);
CREATE INDEX lion_vfy_mk_doc ON lion_vfy_mk USING lion (doc);
SELECT lion_index_posting_root('lion_vfy_mk_doc', 'a b'::tsvector);
SELECT lion_index_posting_root('lion_vfy_mk_tags', '{a,b}'::text[]);
SELECT lion_index_count('lion_vfy_mk_doc', 'a b'::tsvector);
DROP TABLE lion_vfy_mk;

DROP TABLE lion_vfy, lion_vfy_empty, lion_vfy_unl, lion_vfy_hot;
