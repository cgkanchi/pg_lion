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
VACUUM (FREEZE) lion_vfy_hot;
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
VACUUM (FREEZE) lion_vfy;
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
VACUUM (FREEZE) lion_vfy;
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
-- the little-endian uint32 at pos of block blk, and the bytes of one
CREATE FUNCTION lion_vfy_u32(idx regclass, blk int8, pos int8)
RETURNS int8 LANGUAGE sql AS $$
	SELECT get_byte(b, 0)::int8 + get_byte(b, 1)::int8 * 256 +
		   get_byte(b, 2)::int8 * 65536 + get_byte(b, 3)::int8 * 16777216
	  FROM lion_vfy_peek(idx, blk, pos, 4) b
$$;
CREATE FUNCTION lion_vfy_le32(x int8)
RETURNS bytea LANGUAGE sql AS $$
	SELECT set_byte(set_byte(set_byte(set_byte('\x00000000'::bytea,
		   0, (x & 255)::int), 1, ((x >> 8) & 255)::int),
		   2, ((x >> 16) & 255)::int), 3, ((x >> 24) & 255)::int)
$$;
-- A statement's SQLSTATE and message, or 'ok'.
CREATE FUNCTION lion_vfy_err(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
	RETURN SQLSTATE || ': ' || SQLERRM;
END $$;
-- The special area is the last 32 bytes of a block: the right link first,
-- then the left link, the min and the max at 8 and 12, the level and the
-- flags the two uint16s at 24 and 26, the page id at 28.
SELECT current_setting('block_size')::int8 - 32 AS special \gset

/*
 * The tables the cases below build their indexes on, and lion_vfy_badp's
 * twin, which they read their posting set's layout from.  A case sets every
 * psql variable it uses itself.
 */
CREATE TEMP TABLE lion_vfy_bad (k int4);
INSERT INTO lion_vfy_bad SELECT i % 50 FROM generate_series(1, 20000) i;
-- Two keys of more than 16 leaves each, so that each set has a root above
-- its leaves (DESIGN.md §22): a bulk build packs three of these containers
-- onto a leaf.
CREATE TEMP TABLE lion_vfy_badp (id int4, k int4);
INSERT INTO lion_vfy_badp SELECT i, i % 2 FROM generate_series(1, 900000) i;
CREATE INDEX lion_vfy_badp_twin ON lion_vfy_badp USING lion (k);
-- 300 keys of 108 bytes: a directory of a root over six leaves
CREATE TEMP TABLE lion_vfy_badd (k text);
INSERT INTO lion_vfy_badd
SELECT 'key' || lpad(i::text, 5, '0') || repeat('x', 100) FROM generate_series(1, 300) i;
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
					 lion_vfy_item('lion_vfy_bad_attno', 1, 3) + 4, '\xffff');
SET client_min_messages = error;
SELECT lion_index_verify('lion_vfy_bad_attno');
SELECT entries FROM lion_index_stats('lion_vfy_bad_attno');
SET client_min_messages = warning;

-- (3) a key longer than its entry: the payload length computed from it used
-- to underflow, and lion_index_stats() walked the "payload" off the page
CREATE INDEX lion_vfy_bad_keylen ON lion_vfy_bad USING lion (k);
SELECT lion_vfy_poke('lion_vfy_bad_keylen', 1,
					 lion_vfy_item('lion_vfy_bad_keylen', 1, 3) + 2, '\xffff');
SET client_min_messages = error;
SELECT lion_index_verify('lion_vfy_bad_keylen');
SELECT entries FROM lion_index_stats('lion_vfy_bad_keylen');
SET client_min_messages = warning;

-- (4) a posting-tree downlink to InvalidBlockNumber, which ReadBuffer() takes
-- for P_NEW on 16 to 18: the check used to EXTEND the index it was checking
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

-- (5) a container of no known type on a posting leaf: the root's first child
CREATE INDEX lion_vfy_bad_type ON lion_vfy_badp USING lion (k);
SELECT lion_index_posting_root('lion_vfy_badp_twin', 0) AS root \gset
SELECT get_byte(b, 0) + get_byte(b, 1) * 256 + get_byte(b, 2) * 65536 +
	   get_byte(b, 3) * 16777216 AS leaf
  FROM lion_vfy_peek('lion_vfy_bad_type', :root,
					 lion_vfy_item('lion_vfy_bad_type', :root, 1) + 4, 4) b \gset
SELECT lion_vfy_poke('lion_vfy_bad_type', :leaf,
					 lion_vfy_item('lion_vfy_bad_type', :leaf, 1) + 6, '\x7f');
SET client_min_messages = error;
SELECT lion_index_verify('lion_vfy_bad_type');
SELECT containers FROM lion_index_stats('lion_vfy_bad_type');
SET client_min_messages = warning;

-- (6) a right link that skips a leaf, and the left link that goes with it,
-- so that the leaf is reached by its downlink and by nothing else.  The walk
-- of the leaves cannot tell that from a leaf a concurrent split made after
-- the walk had passed its place - verify() lets INSERTs run beside it
-- (DESIGN.md §7) - so it is a candidate, and it is the walk of the level
-- again, after the wait for the writers in flight, that finds the downlink
-- pointing out of the level.  The first leaf is the directory leaf (flag 2)
-- without a left link.
CREATE INDEX lion_vfy_bad_link ON lion_vfy_badd USING lion (k);
SELECT b AS leaf1
  FROM generate_series(1, pg_relation_size('lion_vfy_bad_link') /
					   current_setting('block_size')::int8 - 1) b
 WHERE (lion_vfy_u32('lion_vfy_bad_link', b, :special + 24) >> 16) & 2 = 2
   AND lion_vfy_u32('lion_vfy_bad_link', b, :special + 4) = 4294967295 \gset
SELECT lion_vfy_u32('lion_vfy_bad_link', :leaf1, :special) AS leaf2 \gset
SELECT lion_vfy_u32('lion_vfy_bad_link', :leaf2, :special) AS leaf3 \gset
SELECT :leaf1 AS leaf1, :leaf2 AS leaf2, :leaf3 AS leaf3;
SELECT lion_vfy_poke('lion_vfy_bad_link', :leaf1, :special, lion_vfy_le32(:leaf3));
SELECT lion_vfy_poke('lion_vfy_bad_link', :leaf3, :special + 4, lion_vfy_le32(:leaf1));
SET client_min_messages = error;
SELECT lion_index_verify('lion_vfy_bad_link');
SET client_min_messages = warning;

-- (7) a posting leaf flagged LION_PAGE_POSITIONS, as a page of a position
-- tree: the root's second child, flags 0x0004 -> 0x0084.  Every reader of the
-- set refuses such a page and ends the set there, so the counts and the scans
-- lose every row from that leaf on - the SQL count of key 0 finds the first
-- leaf's rows of 450000 - and verify() must report it, with heapallindexed
-- too, whose lookup goes through the tree code that does not look at the
-- flag.
CREATE INDEX lion_vfy_bad_kind ON lion_vfy_badp USING lion (k);
SELECT lion_index_posting_root('lion_vfy_badp_twin', 0) AS root \gset
SELECT lion_vfy_u32('lion_vfy_bad_kind', :root,
					lion_vfy_item('lion_vfy_bad_kind', :root, 2) + 4) AS leaf \gset
SELECT lion_vfy_u32('lion_vfy_bad_kind', :leaf, :special + 26) & 65535 AS flags;
SELECT lion_vfy_poke('lion_vfy_bad_kind', :leaf, :special + 26,
					 lion_vfy_le32(lion_vfy_u32('lion_vfy_bad_kind', :leaf,
												:special + 26) | 128));
SET client_min_messages = error;
SELECT lion_index_count('lion_vfy_bad_kind', 0) AS rows_of_key_0;
SELECT lion_index_verify('lion_vfy_bad_kind');
SELECT lion_index_verify('lion_vfy_bad_kind', true);
SET client_min_messages = warning;

-- (8) a flag no page of its kind carries: a bit no writer sets on a
-- directory leaf, and ROOT - which only a directory page ever has - on a
-- posting leaf (the root's second child).  verify() must report each,
-- though each page still has one kind.
CREATE INDEX lion_vfy_bad_flag ON lion_vfy_bad USING lion (k);
SELECT lion_vfy_poke('lion_vfy_bad_flag', 1, :special + 26,
					 lion_vfy_le32(lion_vfy_u32('lion_vfy_bad_flag', 1,
												:special + 26) | 1024));
SET client_min_messages = error;
SELECT lion_index_verify('lion_vfy_bad_flag');
SET client_min_messages = warning;
CREATE INDEX lion_vfy_bad_root ON lion_vfy_badp USING lion (k);
SELECT lion_index_posting_root('lion_vfy_badp_twin', 0) AS root \gset
SELECT lion_vfy_u32('lion_vfy_bad_root', :root,
					lion_vfy_item('lion_vfy_bad_root', :root, 2) + 4) AS leaf \gset
SELECT lion_vfy_poke('lion_vfy_bad_root', :leaf, :special + 26,
					 lion_vfy_le32(lion_vfy_u32('lion_vfy_bad_root', :leaf,
												:special + 26) | 32));
SET client_min_messages = error;
SELECT lion_index_verify('lion_vfy_bad_root');
SET client_min_messages = warning;

-- (9) a position tree with an internal level (DESIGN.md §17): 600 rows of 255
-- positions each, twenty leaves of about two heap blocks each under a root.
-- verify() must report a downlink to the wrong leaf (the third goes to the
-- fourth leaf), a separator below what the leaf to its left holds (a
-- reader of that block lands on the leaf to the right, and misses the left
-- one's last chunk), and a leaf whose min - which a tree without an internal
-- level is routed by - is not its first chunk's header.  The root is the one
-- internal page of the tree: flag 128, level 1; the min is at 8 of the
-- special area.
CREATE TEMP TABLE lion_vfy_badpos (id int4, d tsvector);
INSERT INTO lion_vfy_badpos
SELECT i, (SELECT string_agg('many:' || p, ' ')
			 FROM generate_series(1, 255) p)::tsvector
  FROM generate_series(1, 600) i;
CREATE INDEX lion_vfy_badpos_twin ON lion_vfy_badpos USING lion (d)
  WITH (store_positions = true);
SELECT b AS posroot
  FROM generate_series(1, pg_relation_size('lion_vfy_badpos_twin') /
						   current_setting('block_size')::int8 - 1) b
 WHERE lion_vfy_u32('lion_vfy_badpos_twin', b, :special + 26) & 128 <> 0
   AND lion_vfy_u32('lion_vfy_badpos_twin', b, :special + 24) & 65535 = 1 \gset
SELECT lion_vfy_item('lion_vfy_badpos_twin', :posroot, 3) AS item3,
	   lion_vfy_item('lion_vfy_badpos_twin', :posroot, 4) AS item4 \gset
SELECT lion_vfy_u32('lion_vfy_badpos_twin', :posroot, :item3) AS sep3,
	   lion_vfy_u32('lion_vfy_badpos_twin', :posroot, :item3 + 4) AS leaf3,
	   lion_vfy_u32('lion_vfy_badpos_twin', :posroot, :item4 + 4) AS leaf4 \gset
SET client_min_messages = error;
SELECT lion_index_verify('lion_vfy_badpos_twin', true);
SET client_min_messages = warning;
CREATE INDEX lion_vfy_bad_poslink ON lion_vfy_badpos USING lion (d)
  WITH (store_positions = true);
SELECT lion_vfy_poke('lion_vfy_bad_poslink', :posroot, :item3 + 4,
					 lion_vfy_le32(:leaf4));
SET client_min_messages = error;
SELECT lion_index_verify('lion_vfy_bad_poslink');
SET client_min_messages = warning;
CREATE INDEX lion_vfy_bad_possep ON lion_vfy_badpos USING lion (d)
  WITH (store_positions = true);
SELECT lion_vfy_poke('lion_vfy_bad_possep', :posroot, :item3,
					 lion_vfy_le32(:sep3 - 1));
SET client_min_messages = error;
SELECT lion_index_verify('lion_vfy_bad_possep');
SET client_min_messages = warning;
CREATE INDEX lion_vfy_bad_posmin ON lion_vfy_badpos USING lion (d)
  WITH (store_positions = true);
SELECT lion_vfy_poke('lion_vfy_bad_posmin', :leaf3, :special + 8,
					 lion_vfy_le32(:sep3 + 1));
SET client_min_messages = error;
SELECT lion_index_verify('lion_vfy_bad_posmin');
SET client_min_messages = warning;

-- lion_index_stats() counts the pages of a position tree apart from the
-- posting pages (DESIGN.md §7): 'many' is one CHAIN entry whose 600 TIDs fit
-- one posting leaf, and its position tree is twenty leaves under a root.  In
-- an index of two columns they are the tsvector column's, whose entry names
-- the tree.
SELECT container_pages, posting_internal_pages, max_posting_height,
	   position_pages, position_internal_pages
  FROM lion_index_stats('lion_vfy_badpos_twin');
CREATE INDEX lion_vfy_badpos_two ON lion_vfy_badpos USING lion (id, d)
  WITH (store_positions = true);
SELECT attno, container_pages, posting_internal_pages, max_posting_height,
	   position_pages, position_internal_pages
  FROM lion_index_stats('lion_vfy_badpos_two');

-- (10) an INLINE payload whose first item has a type no item has: key 1's
-- entry is the third item of block 1, its payload follows the 12-byte header
-- and the 4-byte key, and an item's type is the byte at 6 of its header.
-- Every reader of the payload must raise INDEX_CORRUPTED (XX002) and say
-- where - verify(), the count, the scans, the ordered walk - and
-- lion_index_stats() leaves out what it cannot read: key 1's items, against
-- a twin read and dropped first.
CREATE TEMP TABLE lion_vfy_badi (k int4);
INSERT INTO lion_vfy_badi SELECT i % 50 FROM generate_series(1, 20000) i;
CREATE INDEX lion_vfy_badi_twin ON lion_vfy_badi USING lion (k);
SELECT entries, containers + sparse_segments AS items, container_bytes
  FROM lion_index_stats('lion_vfy_badi_twin');
DROP INDEX lion_vfy_badi_twin;
CREATE INDEX lion_vfy_bad_inl ON lion_vfy_badi USING lion (k);
SELECT lion_vfy_poke('lion_vfy_bad_inl', 1,
					 lion_vfy_item('lion_vfy_bad_inl', 1, 3) + 16 + 6, '\x7f');
SET client_min_messages = error;
SELECT lion_vfy_err($$SELECT lion_index_verify('lion_vfy_bad_inl')$$);
SELECT entries, containers + sparse_segments AS items, container_bytes
  FROM lion_index_stats('lion_vfy_bad_inl');
SELECT lion_vfy_err($$SELECT lion_index_count('lion_vfy_bad_inl', 1)$$);
SET enable_seqscan = off;
SELECT lion_vfy_err($$SELECT count(*) FROM lion_vfy_badi WHERE k = 1$$);
SET pg_lion.enable_count_pushdown = off;
SELECT lion_vfy_err($$SELECT count(*) FROM lion_vfy_badi WHERE k = 1$$);
RESET pg_lion.enable_count_pushdown;
SELECT lion_vfy_err($$SELECT sum(k) FROM lion_vfy_badi WHERE k IN (1, 3)$$);
SET enable_bitmapscan = off;
SELECT lion_vfy_err($$SELECT k FROM lion_vfy_badi WHERE k <= 1 ORDER BY k LIMIT 600$$);
RESET enable_bitmapscan;
RESET enable_seqscan;
SET client_min_messages = warning;

-- ... and a posting set copied into memory whose items are out of order: the
-- GROUP BY count intersects key 0's set with every group, from a copy after
-- the first (DESIGN.md §10).  The second item of the set's head gets the
-- first one's container key, the uint32 at 0 of an item.  The copy must say
-- where, as verify() does.
CREATE TEMP TABLE lion_vfy_badm (k int4, g int4);
INSERT INTO lion_vfy_badm SELECT i % 2, i % 5 FROM generate_series(1, 200000) i;
CREATE INDEX lion_vfy_badm_g ON lion_vfy_badm USING lion (g);
VACUUM (FREEZE, ANALYZE) lion_vfy_badm;
CREATE INDEX lion_vfy_bad_mat ON lion_vfy_badm USING lion (k);
SELECT lion_index_posting_root('lion_vfy_bad_mat', 0) AS head \gset
SELECT lion_vfy_poke('lion_vfy_bad_mat', :head,
					 lion_vfy_item('lion_vfy_bad_mat', :head, 2),
					 lion_vfy_le32(lion_vfy_u32('lion_vfy_bad_mat', :head,
												lion_vfy_item('lion_vfy_bad_mat',
															  :head, 1))));
SET client_min_messages = error;
SET pg_lion.enable_count_pushdown = on;
SELECT lion_vfy_err($$SELECT g, count(*) FROM lion_vfy_badm WHERE k = 0 GROUP BY g$$);
RESET pg_lion.enable_count_pushdown;
SELECT lion_index_verify('lion_vfy_bad_mat');
SET client_min_messages = warning;

-- (11) the positions of an INLINE entry for a TID the entry does not hold.
-- 'alpha' is the entry of rows 1 and 2, (0,1) and (0,2), the second item of
-- block 1, and its positions are the last pos_len bytes of it - pos_len is
-- the uint32 at 28, after the 12-byte header, the 9-byte key and pos_root -
-- one chunk: an 8-byte header, its used bytes, then for each TID the delta
-- of its code, its count of positions and the positions.  The second TID's
-- delta, at 13 of the chunk, goes from 1 to 2: the chunk names (0,1) and
-- (0,3), and a phrase query through the index loses row 2.  verify() must
-- report it.
CREATE TEMP TABLE lion_vfy_badph (id int4, d tsvector);
INSERT INTO lion_vfy_badph
VALUES (1, 'alpha:1 beta:2'), (2, 'alpha:1 beta:2'), (3, 'gamma:1');
CREATE INDEX lion_vfy_bad_inlpos ON lion_vfy_badph USING lion (d)
  WITH (store_positions = true);
SELECT lion_vfy_item('lion_vfy_bad_inlpos', 1, 2) AS item,
	   lion_vfy_u32('lion_vfy_bad_inlpos', 1, 24 + 4 * 1) >> 17 AS itemlen \gset
SELECT lion_vfy_poke('lion_vfy_bad_inlpos', 1,
					 :item + :itemlen -
					 lion_vfy_u32('lion_vfy_bad_inlpos', 1, :item + 28) + 13,
					 '\x02');
SET client_min_messages = error;
SET enable_seqscan = off;
SELECT id FROM lion_vfy_badph WHERE d @@ 'alpha <-> beta';
RESET enable_seqscan;
SELECT lion_index_verify('lion_vfy_bad_inlpos');
SET client_min_messages = warning;

-- (12) INLINE entries past the meta page's inline_limit, the uint32 at 40 of
-- block 0: each of the 50 keys holds 400 TIDs in about 800 bytes, which the
-- build put INLINE under the limit of 4096.  At 64, every writer would have
-- spilled them, and a rebuild would.
CREATE INDEX lion_vfy_bad_limit ON lion_vfy_bad USING lion (k);
SELECT lion_vfy_u32('lion_vfy_bad_limit', 0, 40) AS inline_limit;
SELECT lion_vfy_poke('lion_vfy_bad_limit', 0, 40, lion_vfy_le32(64));
SET client_min_messages = error;
SELECT lion_index_verify('lion_vfy_bad_limit');
SET client_min_messages = warning;

-- (13) the meta page's other words, each in an index of its own: sliced key
-- columns on a version 14 page (36, the word that was the bucket count,
-- DESIGN.md §50; every reader refuses it), a WAL mode the format does not have (56), an
-- order flag it does not have (bit 8 of 60), and its two counts of pages -
-- the directory's (52), which can be no page at all or fewer than the
-- directory walk finds (lion_vfy_badd's has a root and six leaves), and the
-- pages the last VACUUM could not hand out (12 of block 0's special area),
-- which can be no more than the index has.
CREATE FUNCTION lion_vfy_meta(tbl regclass, pos int8, val bytea)
RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	r text;
BEGIN
	EXECUTE format('CREATE INDEX lion_vfy_bad_meta ON %s USING lion (k)', tbl);
	PERFORM lion_vfy_poke('lion_vfy_bad_meta', 0, pos, val);
	r := lion_vfy_err($q$SELECT lion_index_verify('lion_vfy_bad_meta')$q$);
	DROP INDEX lion_vfy_bad_meta;
	RETURN r;
END $$;
SET client_min_messages = error;
SELECT what, lion_vfy_meta(tbl, pos, val) AS verify
  FROM (VALUES ('sliced columns, v14', 'lion_vfy_bad'::regclass, 36, lion_vfy_le32(5)),
			   ('wal mode', 'lion_vfy_bad', 56, lion_vfy_le32(7)),
			   ('order flags', 'lion_vfy_bad', 61, '\x01'::bytea),
			   ('no directory pages', 'lion_vfy_bad', 52, lion_vfy_le32(0)),
			   ('too few directory pages', 'lion_vfy_badd', 52, lion_vfy_le32(2)),
			   ('pages to reuse', 'lion_vfy_bad', :special + 12, lion_vfy_le32(1000000)))
	   v(what, tbl, pos, val);
SET client_min_messages = warning;

-- (14) a root that is not the only page of its level: lion_vfy_badd's
-- directory is a root over six leaves, and the meta page (root at 44, height
-- at 48) now names the first leaf as a root of height 0, with every leaf
-- flagged ROOT (32), as the root flag check wants of the root's level.  The
-- old root is zeroed - a page an interrupted allocation leaves, which
-- verify() only warns of.  A writer that splits any of these leaves takes it
-- for the root of the directory.
CREATE INDEX lion_vfy_bad_roots ON lion_vfy_badd USING lion (k);
SELECT b AS leaf1
  FROM generate_series(1, pg_relation_size('lion_vfy_bad_roots') /
					   current_setting('block_size')::int8 - 1) b
 WHERE (lion_vfy_u32('lion_vfy_bad_roots', b, :special + 24) >> 16) & 2 = 2
   AND lion_vfy_u32('lion_vfy_bad_roots', b, :special + 4) = 4294967295 \gset
SELECT lion_vfy_u32('lion_vfy_bad_roots', 0, 44) AS oldroot,
	   lion_vfy_u32('lion_vfy_bad_roots', 0, 48) AS oldheight \gset
SELECT :oldheight AS height;
SELECT count(lion_vfy_poke('lion_vfy_bad_roots', b, :special + 26,
						   lion_vfy_le32(lion_vfy_u32('lion_vfy_bad_roots', b,
													  :special + 26) | 32)))
	   AS leaves
  FROM generate_series(1, pg_relation_size('lion_vfy_bad_roots') /
						   current_setting('block_size')::int8 - 1) b
 WHERE (lion_vfy_u32('lion_vfy_bad_roots', b, :special + 24) >> 16) & 2 = 2;
SELECT lion_vfy_poke('lion_vfy_bad_roots', 0, 44,
					 lion_vfy_le32(:leaf1) || lion_vfy_le32(0));
SELECT lion_vfy_poke('lion_vfy_bad_roots', :oldroot, 0,
					 decode(repeat('00', current_setting('block_size')::int), 'hex'));
SET client_min_messages = error;
SELECT lion_index_verify('lion_vfy_bad_roots');
SET client_min_messages = warning;

-- (15) DELETED pages (DESIGN.md §18, §21): what freeing a page leaves on it,
-- and a safexid VACUUM could have given it.  Key 'big' is a CHAIN set of one
-- container page (inline_limit = 64), and the 91 keys from key00040 on fill a
-- directory leaf and more: VACUUM frees the set's page and deletes the leaf.
-- A freed page is in no buffer this backend holds only once the index is
-- moved, which copies the file: to an in-place tablespace.  Then a container
-- page's safexid (the uint64 of its body, at 24) gets an epoch (at 28) and
-- would never be reused; its pd_lower (the uint16 at 12) says the body holds
-- more than the safexid; the leaf loses its right link (the uint32 at 0 of
-- the special area), which a reader that lands on it moves right by; and its
-- safexid (the uint32s at 8 and 12 of the special area) becomes 0, which
-- every VACUUM would reuse at once.  The transaction ids are the cluster's.
-- A tablespace is the cluster's too: its name is this backend's, which no
-- other run of the test has while this one runs, and one a run that stopped
-- part-way left under it is dropped first.
SELECT 'lion_vfy_ts_' || pg_backend_pid() AS ts \gset
DROP TABLESPACE IF EXISTS :"ts";
SET allow_in_place_tablespaces = on;
CREATE TABLESPACE :"ts" LOCATION '';
RESET allow_in_place_tablespaces;
CREATE TEMP TABLE lion_vfy_baddel (k text);
INSERT INTO lion_vfy_baddel
SELECT 'key' || lpad(i::text, 5, '0') || repeat('x', 100) FROM generate_series(1, 300) i;
INSERT INTO lion_vfy_baddel
SELECT CASE WHEN i % 2 = 0 THEN 'big' ELSE 'other' END FROM generate_series(1, 4000) i;
CREATE INDEX lion_vfy_bad_delxid ON lion_vfy_baddel USING lion (k) WITH (inline_limit = 64);
CREATE INDEX lion_vfy_bad_dellow ON lion_vfy_baddel USING lion (k) WITH (inline_limit = 64);
CREATE INDEX lion_vfy_bad_dellink ON lion_vfy_baddel USING lion (k) WITH (inline_limit = 64);
CREATE INDEX lion_vfy_bad_deldxid ON lion_vfy_baddel USING lion (k) WITH (inline_limit = 64);
DELETE FROM lion_vfy_baddel WHERE k = 'big' OR k BETWEEN 'key00040' AND 'key00130';
VACUUM lion_vfy_baddel;
SELECT deleted_pages FROM lion_index_stats('lion_vfy_bad_delxid');
ALTER INDEX lion_vfy_bad_delxid SET TABLESPACE :"ts";
ALTER INDEX lion_vfy_bad_dellow SET TABLESPACE :"ts";
ALTER INDEX lion_vfy_bad_dellink SET TABLESPACE :"ts";
ALTER INDEX lion_vfy_bad_deldxid SET TABLESPACE :"ts";
SELECT min(b) FILTER (WHERE f & (4 | 8) = 12) AS delcont,
	   min(b) FILTER (WHERE f & (2 | 8) = 10) AS deldir
  FROM (SELECT b, lion_vfy_u32('lion_vfy_bad_delxid', b, :special + 24) >> 16 AS f
		  FROM generate_series(1, pg_relation_size('lion_vfy_bad_delxid') /
								   current_setting('block_size')::int8 - 1) b) p \gset
SELECT :delcont IS NOT NULL AS has_deleted_container,
	   :deldir IS NOT NULL AS has_deleted_leaf;
SELECT lion_vfy_poke('lion_vfy_bad_delxid', :delcont, 28, lion_vfy_le32(1));
SELECT lion_vfy_poke('lion_vfy_bad_dellow', :delcont, 12, '\x24');
SELECT lion_vfy_poke('lion_vfy_bad_dellink', :deldir, :special,
					 lion_vfy_le32(4294967295));
SELECT lion_vfy_poke('lion_vfy_bad_deldxid', :deldir, :special + 8,
					 lion_vfy_le32(0) || lion_vfy_le32(0));
SET client_min_messages = error;
SELECT regexp_replace(lion_vfy_err($$SELECT lion_index_verify('lion_vfy_bad_delxid')$$),
					  '(transaction|is) [0-9]+', '\1 N', 'g') AS verify;
SELECT lion_index_verify('lion_vfy_bad_dellow');
SELECT lion_index_verify('lion_vfy_bad_dellink');
SELECT regexp_replace(lion_vfy_err($$SELECT lion_index_verify('lion_vfy_bad_deldxid')$$),
					  '(transaction|is) [0-9]+', '\1 N', 'g') AS verify;
SET client_min_messages = warning;
DROP TABLE lion_vfy_baddel;
DROP TABLESPACE :"ts";

-- (16) an unfinished split's flag (64) where no split leaves one.  A split
-- flags only the left half it splits off a page, and the flag stays with
-- that page's right sibling: so never the rightmost page of a level - the
-- last leaf of lion_vfy_badd's directory, the root of key 0's posting set.
-- A WARNING would call each a split the next INSERT finishes, but that
-- INSERT fails on the page, which has no sibling to post a downlink for, so
-- verify() reports each.
CREATE INDEX lion_vfy_bad_splitd ON lion_vfy_badd USING lion (k);
SELECT b AS lastleaf
  FROM generate_series(1, pg_relation_size('lion_vfy_bad_splitd') /
					   current_setting('block_size')::int8 - 1) b
 WHERE (lion_vfy_u32('lion_vfy_bad_splitd', b, :special + 24) >> 16) & 2 = 2
   AND lion_vfy_u32('lion_vfy_bad_splitd', b, :special) = 4294967295 \gset
SELECT lion_vfy_poke('lion_vfy_bad_splitd', :lastleaf, :special + 26,
					 lion_vfy_le32(lion_vfy_u32('lion_vfy_bad_splitd', :lastleaf,
												:special + 26) | 64));
CREATE INDEX lion_vfy_bad_splitp ON lion_vfy_badp USING lion (k);
SELECT lion_index_posting_root('lion_vfy_badp_twin', 0) AS root \gset
SELECT lion_vfy_poke('lion_vfy_bad_splitp', :root, :special + 26,
					 lion_vfy_le32(lion_vfy_u32('lion_vfy_bad_splitp', :root,
												:special + 26) | 64));
SET client_min_messages = error;
SELECT lion_vfy_err($$SELECT lion_index_verify('lion_vfy_bad_splitd')$$);
SELECT lion_vfy_err($$SELECT lion_index_verify('lion_vfy_bad_splitp')$$);
SET client_min_messages = warning;
-- And an INSERT that reaches such a page - the last leaf of a posting set
-- with an internal level, which an append goes to - finds no sibling to
-- post a downlink for, and says the index is damaged (INDEX_CORRUPTED), as
-- the directory's does, not that something went wrong inside (XX000).  On
-- a table of its own, as an INSERT writes to every index of its table:
-- 90000 rows of two keys at posting_fillfactor 10 make key 0's set a tree
-- (verify_incomplete_split.spec).
CREATE TEMP TABLE lion_vfy_badsi (id int4, k int4, pad char(80) NOT NULL DEFAULT 'x');
INSERT INTO lion_vfy_badsi SELECT i, i % 2 FROM generate_series(1, 90000) i;
CREATE INDEX lion_vfy_badsi_twin ON lion_vfy_badsi USING lion (k)
  WITH (posting_fillfactor = 10);
CREATE INDEX lion_vfy_bad_splitins ON lion_vfy_badsi USING lion (k)
  WITH (posting_fillfactor = 10);
SELECT max_posting_height > 0 AS posting_is_a_tree
  FROM lion_index_stats('lion_vfy_badsi_twin');
SELECT lion_index_posting_root('lion_vfy_badsi_twin', 0) AS root \gset
SELECT b AS lastleaf
  FROM generate_series(1, pg_relation_size('lion_vfy_badsi_twin') /
					   current_setting('block_size')::int8 - 1) b
 WHERE lion_vfy_u32('lion_vfy_badsi_twin', b, :special + 20) = :root
   AND lion_vfy_u32('lion_vfy_badsi_twin', b, :special + 24) & 65535 = 0
   AND lion_vfy_u32('lion_vfy_badsi_twin', b, :special) = 4294967295 \gset
SELECT lion_vfy_poke('lion_vfy_bad_splitins', :lastleaf, :special + 26,
					 lion_vfy_le32(lion_vfy_u32('lion_vfy_badsi_twin', :lastleaf,
												:special + 26) | 64));
SET client_min_messages = error;
SELECT regexp_replace(lion_vfy_err($$INSERT INTO lion_vfy_badsi (id, k) VALUES (0, 0)$$),
					  'block [0-9]+', 'block N') AS insert;
SET client_min_messages = warning;
DROP TABLE lion_vfy_badsi;

-- (17) a position tree without positions for some TIDs of its posting set.
-- 600 rows of 'alpha:5 beta:6', whose keys are CHAIN entries (inline_limit
-- = 64) with a position tree of one leaf each: one chunk, a 10-byte header
-- and the 600 rows' three bytes - the delta of the code (1), the count of
-- positions (1), the position (20 for 'alpha': a position is stored shifted
-- left by two, over its weight).  The first member's delta goes from 1 to
-- 2, which moves every member one offset on and leaves the first row of
-- each of the five heap blocks without positions.  verify() must report it.
CREATE TEMP TABLE lion_vfy_badpc (id int4, d tsvector);
INSERT INTO lion_vfy_badpc SELECT i, 'alpha:5 beta:6' FROM generate_series(1, 600) i;
CREATE INDEX lion_vfy_bad_poscov ON lion_vfy_badpc USING lion (d)
  WITH (store_positions = true, inline_limit = 64);
SELECT b AS posleaf, lion_vfy_item('lion_vfy_bad_poscov', b, 1) AS item
  FROM generate_series(1, pg_relation_size('lion_vfy_bad_poscov') /
					   current_setting('block_size')::int8 - 1) b
 WHERE (lion_vfy_u32('lion_vfy_bad_poscov', b, :special + 24) >> 16) & 128 <> 0
   AND lion_vfy_peek('lion_vfy_bad_poscov', b,
					 lion_vfy_item('lion_vfy_bad_poscov', b, 1) + 10, 3) = '\x010114' \gset
SELECT lion_vfy_poke('lion_vfy_bad_poscov', :posleaf, :item + 10, '\x02');
SET client_min_messages = error;
SELECT lion_index_verify('lion_vfy_bad_poscov');
SET client_min_messages = warning;

-- (18) positions that are not the row's own, which only heapallindexed
-- finds: every structure is sound, and a phrase query decided from them
-- loses the row.  An INLINE entry's chunk, laid out as in (11): row 2's
-- position under 'alpha', at 15 of the chunk, goes from 1 to 3 (4 -> 12).
-- And a position tree laid out as in (17), where row 1's goes from 5 to 7.
-- The message names the key and both positions to a superuser.
CREATE TEMP TABLE lion_vfy_badhp (id int4, d tsvector);
INSERT INTO lion_vfy_badhp
VALUES (1, 'alpha:1 beta:2'), (2, 'alpha:1 beta:2'), (3, 'gamma:1');
CREATE INDEX lion_vfy_bad_hpinl ON lion_vfy_badhp USING lion (d)
  WITH (store_positions = true);
SELECT lion_vfy_item('lion_vfy_bad_hpinl', 1, 2) AS item,
	   lion_vfy_u32('lion_vfy_bad_hpinl', 1, 24 + 4 * 1) >> 17 AS itemlen \gset
SELECT lion_vfy_poke('lion_vfy_bad_hpinl', 1,
					 :item + :itemlen -
					 lion_vfy_u32('lion_vfy_bad_hpinl', 1, :item + 28) + 15,
					 '\x0c');
CREATE TEMP TABLE lion_vfy_badhc (id int4, d tsvector);
INSERT INTO lion_vfy_badhc SELECT i, 'alpha:5 beta:6' FROM generate_series(1, 600) i;
CREATE INDEX lion_vfy_bad_hpch ON lion_vfy_badhc USING lion (d)
  WITH (store_positions = true, inline_limit = 64);
SELECT b AS posleaf, lion_vfy_item('lion_vfy_bad_hpch', b, 1) AS item
  FROM generate_series(1, pg_relation_size('lion_vfy_bad_hpch') /
					   current_setting('block_size')::int8 - 1) b
 WHERE (lion_vfy_u32('lion_vfy_bad_hpch', b, :special + 24) >> 16) & 128 <> 0
   AND lion_vfy_peek('lion_vfy_bad_hpch', b,
					 lion_vfy_item('lion_vfy_bad_hpch', b, 1) + 10, 3) = '\x010114' \gset
SELECT lion_vfy_poke('lion_vfy_bad_hpch', :posleaf, :item + 12, '\x1c');
SET client_min_messages = error;
SET enable_seqscan = off;
SELECT id FROM lion_vfy_badhp WHERE d @@ 'alpha <-> beta';
SELECT count(*) FROM lion_vfy_badhc WHERE d @@ 'alpha <-> beta';
RESET enable_seqscan;
SELECT lion_index_verify('lion_vfy_bad_hpinl');
SELECT lion_index_verify('lion_vfy_bad_hpch');
\set VERBOSITY default
SELECT lion_index_verify('lion_vfy_bad_hpinl', true);
SELECT lion_index_verify('lion_vfy_bad_hpch', true);
\set VERBOSITY terse
SET client_min_messages = warning;

-- (19) a position tree bigger than the memory a check keeps (DESIGN.md §7):
-- 36000 rows of 'w' at the 256 positions a tsvector keeps of one lexeme,
-- whose position tree has over a thousand leaves of two or three heap
-- blocks each, under two internal pages and a root.  A walk keeps each
-- level for the level above it as a sequence of 16 bytes a page, in memory
-- up to a fifth of work_mem and in a temporary file past it: at 64kB the
-- leaves after the 819th go to the file, and the comparison with the level
-- above reads them back from it.  The DEBUG line gives the most the walks
-- kept in memory and the records they wrote out; at the default work_mem
-- they write none.
--
-- And in a twin, the high key of the left internal page and the root's
-- separator for the right one, which are one key, go one block down, under
-- the last block the left page's last leaf holds.  Every level is still in
-- order, and a lookup of that block - lion_postree_fetch(), or a writer
-- that puts or takes away a member of it - descends through the right
-- internal page to the leaf after that one, never to it: heapallindexed
-- finds the positions of the block's first rows gone.  verify() must report
-- the leaf above its parent's high key.
CREATE TEMP TABLE lion_vfy_big (id int4, d tsvector);
INSERT INTO lion_vfy_big
SELECT i, w.d
  FROM generate_series(1, 36000) i,
	   (SELECT to_tsvector('simple', repeat('w ', 256)) d) w;
CREATE INDEX lion_vfy_big_d ON lion_vfy_big USING lion (d)
  WITH (store_positions = true);
CREATE INDEX lion_vfy_bad_parent ON lion_vfy_big USING lion (d)
  WITH (store_positions = true);
SELECT b AS bigroot
  FROM generate_series(1, pg_relation_size('lion_vfy_bad_parent') /
						   current_setting('block_size')::int8 - 1) b
 WHERE lion_vfy_u32('lion_vfy_bad_parent', b, :special + 26) & 128 <> 0
   AND lion_vfy_u32('lion_vfy_bad_parent', b, :special + 24) & 65535 = 2 \gset
SELECT b AS bigleft, lion_vfy_u32('lion_vfy_bad_parent', b, :special) AS bigright,
	   lion_vfy_item('lion_vfy_bad_parent', b, 1) AS highitem,
	   lion_vfy_u32('lion_vfy_bad_parent', b,
					lion_vfy_item('lion_vfy_bad_parent', b, 1)) AS high
  FROM generate_series(1, pg_relation_size('lion_vfy_bad_parent') /
						   current_setting('block_size')::int8 - 1) b
 WHERE lion_vfy_u32('lion_vfy_bad_parent', b, :special + 26) & 128 <> 0
   AND lion_vfy_u32('lion_vfy_bad_parent', b, :special + 24) & 65535 = 1
   AND lion_vfy_u32('lion_vfy_bad_parent', b, :special) <> 4294967295 \gset
SELECT lion_vfy_item('lion_vfy_bad_parent', :bigroot, o) AS rootitem
  FROM generate_series(1, 2) o
 WHERE lion_vfy_u32('lion_vfy_bad_parent', :bigroot,
					lion_vfy_item('lion_vfy_bad_parent', :bigroot, o) + 4) =
	   :bigright \gset
SELECT lion_vfy_u32('lion_vfy_bad_parent', :bigroot, :rootitem) = :high
	   AS same_key;
SELECT lion_vfy_poke('lion_vfy_bad_parent', :bigleft, :highitem,
					 lion_vfy_le32(:high - 1));
SELECT lion_vfy_poke('lion_vfy_bad_parent', :bigroot, :rootitem,
					 lion_vfy_le32(:high - 1));
SET work_mem = '64kB';
SET client_min_messages = debug1;
SELECT lion_index_verify('lion_vfy_big_d');
RESET work_mem;
SELECT lion_index_verify('lion_vfy_big_d');
SET client_min_messages = error;
SET work_mem = '64kB';
SELECT lion_index_verify('lion_vfy_bad_parent');
RESET work_mem;
SET client_min_messages = warning;

-- (20) a reserved entry of a kind its column never writes: an int4 column's
-- NULL entry (flags 5, INLINE and NULLKEY) made an empty-dimension one
-- (0x1001), which only an operator class that extracts an array's elements
-- writes (DESIGN.md §17).  verify() must report it, and say "an".
CREATE TEMP TABLE lion_vfy_badr (k int4);
INSERT INTO lion_vfy_badr
SELECT CASE WHEN i % 10 = 0 THEN NULL ELSE i END FROM generate_series(1, 100) i;
CREATE INDEX lion_vfy_badr_twin ON lion_vfy_badr USING lion (k);
CREATE INDEX lion_vfy_bad_reserved ON lion_vfy_badr USING lion (k);
SELECT b AS nullblk, lion_vfy_item('lion_vfy_badr_twin', b, o) AS nullitem
  FROM generate_series(1, pg_relation_size('lion_vfy_badr_twin') /
					   current_setting('block_size')::int8 - 1) b,
	   generate_series(1, 4) o
 WHERE lion_vfy_u32('lion_vfy_badr_twin', b, :special + 24) & 65535 = 0
   AND lion_vfy_u32('lion_vfy_badr_twin', b,
					lion_vfy_item('lion_vfy_badr_twin', b, o)) = 5 \gset
SELECT lion_vfy_poke('lion_vfy_bad_reserved', :nullblk, :nullitem, '\x0110');
SET client_min_messages = error;
SELECT lion_vfy_err($$SELECT lion_index_verify('lion_vfy_bad_reserved')$$);
SET client_min_messages = warning;
DROP TABLE lion_vfy_badr;

RESET ignore_checksum_failure;
DROP TABLE lion_vfy_bad, lion_vfy_badp, lion_vfy_badd, lion_vfy_badpos,
	lion_vfy_badi, lion_vfy_badm, lion_vfy_badph, lion_vfy_badpc,
	lion_vfy_badhp, lion_vfy_badhc, lion_vfy_big;
DROP FUNCTION lion_vfy_meta(regclass, int8, bytea);
DROP FUNCTION lion_vfy_err(text);
DROP FUNCTION lion_vfy_peek(regclass, int8, int8, int8);
DROP FUNCTION lion_vfy_poke(regclass, int8, int8, bytea);
DROP FUNCTION lion_vfy_item(regclass, int8, int);
DROP FUNCTION lion_vfy_u32(regclass, int8, int8);
DROP FUNCTION lion_vfy_le32(int8);

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
