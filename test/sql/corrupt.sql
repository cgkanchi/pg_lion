-- A damaged directory page is an ERROR, not a walk (DESIGN.md §21, "Readers").
--
-- The pages are damaged ON DISK, under the server: each case builds an index,
-- writes it out and drops its pages from the buffer pool
-- (pg_buffercache_evict(), PostgreSQL 17 and later - on 16 the test is
-- skipped, test/expected/corrupt_1.out), overwrites a few bytes of the file,
-- and lets the next read see them.  Each case drops its index before the next
-- one builds another: an INSERT into the table goes through every index on
-- it.  The byte offsets assume a little-endian server, as the page checksum's
-- does.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
CREATE EXTENSION IF NOT EXISTS pageinspect;
CREATE EXTENSION IF NOT EXISTS pg_buffercache;
RESET client_min_messages;
SELECT current_setting('server_version_num')::int >= 170000 AS lion_dc_can_evict \gset
\if :lion_dc_can_evict
-- a damaged index must fail fast, never loop
SET statement_timeout = '60s';

CREATE TABLE lion_dc (k int NOT NULL);
INSERT INTO lion_dc SELECT i FROM generate_series(1, 3000) i;

-- A little-endian uint32 at byte off of a bytea.
CREATE FUNCTION lion_dc_u32(f bytea, off int) RETURNS bigint
LANGUAGE sql IMMUTABLE AS $$
	SELECT get_byte(f, off)::bigint | (get_byte(f, off + 1)::bigint << 8) |
		   (get_byte(f, off + 2)::bigint << 16) | (get_byte(f, off + 3)::bigint << 24)
$$;

-- Block blk of an index's main fork, read from the file, not the buffer pool.
CREATE FUNCTION lion_dc_block(idx regclass, blk bigint) RETURNS bytea
LANGUAGE sql AS $$
	SELECT pg_read_binary_file(pg_relation_filepath(idx),
							   blk * current_setting('block_size')::int,
							   current_setting('block_size')::int)
$$;

-- The directory root's block number and height, from the meta page (block 0):
-- LionMetaPageData follows the 24-byte page header, root at +20, height at +24.
CREATE FUNCTION lion_dc_root(idx regclass, OUT root bigint, OUT height bigint)
LANGUAGE sql AS $$
	SELECT lion_dc_u32(m, 44), lion_dc_u32(m, 48)
	  FROM lion_dc_block(idx, 0) m
$$;

-- Byte offset in its page of item off's tuple: lp_off, the low 15 bits of the
-- line pointer at 24 + 4 * (off - 1).
CREATE FUNCTION lion_dc_item(page bytea, off int) RETURNS int
LANGUAGE sql IMMUTABLE AS $$
	SELECT (lion_dc_u32(page, 24 + 4 * (off - 1)) & 32767)::int
$$;

-- Write an index out and drop its pages from the buffer pool, so that the
-- file is what the next read sees.
CREATE FUNCTION lion_dc_evict(idx regclass) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
	CHECKPOINT;
	PERFORM pg_buffercache_evict(bufferid)
	   FROM pg_buffercache
	  WHERE relfilenode = pg_relation_filenode(idx)
		AND reldatabase = (SELECT oid FROM pg_database WHERE datname = current_database());
END $$;

-- Overwrite bytes at byte pos of block blk, give the page its new checksum
-- (data checksums are on by default from PostgreSQL 18), and write the file
-- back.  pd_checksum is the uint16 at byte 8 of the page.
CREATE FUNCTION lion_dc_poke(idx regclass, blk bigint, pos int, bytes bytea)
RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	path text := pg_relation_filepath(idx);
	bs int := current_setting('block_size')::int;
	f bytea := pg_read_binary_file(path);
	page bytea;
	ck int;
	lo oid;
BEGIN
	page := substring(f FROM (blk * bs)::int + 1 FOR bs);
	page := overlay(page PLACING bytes FROM pos + 1);
	ck := page_checksum(page, blk::int)::int & 65535;
	page := overlay(page PLACING set_byte(set_byte('\x0000'::bytea, 0, ck & 255), 1, ck >> 8)
					FROM 9);
	f := overlay(f PLACING page FROM (blk * bs)::int + 1);
	lo := lo_from_bytea(0, f);
	PERFORM lo_export(lo, path);
	PERFORM lo_unlink(lo);
END $$;

-- Run a statement; report its SQLSTATE and its message with the numbers out.
CREATE FUNCTION lion_dc_try(q text) RETURNS text
LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
	RETURN SQLSTATE || ': ' || regexp_replace(SQLERRM, '[0-9]+', 'N', 'g');
END $$;

CREATE TABLE lion_dc_size (pages bigint);
SET enable_seqscan = off;

-- ---------- 1. the root's first downlink names InvalidBlockNumber (P_NEW) ----------
-- ReadBuffer() on PostgreSQL 16 to 18 answered that by extending the index,
-- once per statement; the descent now refuses it.
CREATE INDEX lion_dc_i ON lion_dc USING lion (k);
SELECT lion_dc_evict('lion_dc_i');
SELECT height >= 1 AS has_internal_root FROM lion_dc_root('lion_dc_i');
SELECT lion_dc_poke('lion_dc_i', r.root,
					lion_dc_item(lion_dc_block('lion_dc_i', r.root), 1) + 8,
					'\xffffffff'::bytea)
  FROM lion_dc_root('lion_dc_i') r;
INSERT INTO lion_dc_size SELECT pg_relation_size('lion_dc_i');
SELECT lion_dc_try('SELECT count(*) FROM lion_dc WHERE k = 5');
SELECT lion_dc_try('SELECT count(*) FROM lion_dc WHERE k = 2999');
SELECT lion_dc_try('INSERT INTO lion_dc VALUES (5)');
SELECT lion_dc_try('INSERT INTO lion_dc VALUES (7)');
-- and the index did not grow
SELECT (SELECT pages FROM lion_dc_size) = pg_relation_size('lion_dc_i') AS same_size;
DROP INDEX lion_dc_i;
TRUNCATE lion_dc_size;

-- ---------- 2. an internal page with no downlink at all ----------
-- pd_lower = 24, the size of the page header: no line pointers.
CREATE INDEX lion_dc_i ON lion_dc USING lion (k);
SELECT lion_dc_evict('lion_dc_i');
SELECT lion_dc_poke('lion_dc_i', r.root, 12, '\x1800'::bytea)
  FROM lion_dc_root('lion_dc_i') r;
INSERT INTO lion_dc_size SELECT pg_relation_size('lion_dc_i');
SELECT lion_dc_try('SELECT count(*) FROM lion_dc WHERE k = 5');
SELECT lion_dc_try('INSERT INTO lion_dc VALUES (9)');
SELECT (SELECT pages FROM lion_dc_size) = pg_relation_size('lion_dc_i') AS same_size;
DROP INDEX lion_dc_i;
TRUNCATE lion_dc_size;

-- ---------- 3. a downlink back to the root itself ----------
-- The child is not one level below its parent; the descent used to go round
-- this for ever.
CREATE INDEX lion_dc_i ON lion_dc USING lion (k);
SELECT lion_dc_evict('lion_dc_i');
SELECT lion_dc_poke('lion_dc_i', r.root,
					lion_dc_item(lion_dc_block('lion_dc_i', r.root), 1) + 8,
					substring(lion_dc_block('lion_dc_i', 0) FROM 45 FOR 4))
  FROM lion_dc_root('lion_dc_i') r;
SELECT lion_dc_try('SELECT count(*) FROM lion_dc WHERE k = 5');
SELECT lion_dc_try('INSERT INTO lion_dc VALUES (11)');
DROP INDEX lion_dc_i;

-- ---------- 4. a summary whose key is not its bucket's (DESIGN.md §31) ----------
-- Buckets of 16 keys: the second summary is keyed 32 and holds the rows of
-- keys 17 to 32.  Its key is rewritten to 24, which still sorts between its
-- neighbours (a summary's hash is a constant, so nothing else changes), so
-- the directory is in order and every entry is sound on its own - but the
-- rows of keys 25 to 32 are now in the summary of a bucket that ends at 24,
-- and missing from the next one.
CREATE TABLE lion_dc_s (k int NOT NULL);
INSERT INTO lion_dc_s SELECT i FROM generate_series(1, 400) i;
CREATE INDEX lion_dc_s_i ON lion_dc_s USING lion (k)
	WITH (summaries = on, summary_tids = 16);
SELECT lion_index_verify('lion_dc_s_i', true);
SELECT lion_dc_evict('lion_dc_s_i');

-- A little-endian uint32 as four bytes.
CREATE FUNCTION lion_dc_le32(v bigint) RETURNS bytea
LANGUAGE sql IMMUTABLE AS $$
	SELECT set_byte(set_byte(set_byte(set_byte('\x00000000'::bytea,
		0, (v & 255)::int), 1, ((v >> 8) & 255)::int),
		2, ((v >> 16) & 255)::int), 3, ((v >> 24) & 255)::int)
$$;

-- The block and byte offset of the summary of an int4 column keyed k: an
-- item whose header (hash, flags, keylen at +0, +4, +6) is a SUMMARY entry's
-- (flag 0x0080, hash 0) with an 8-byte key equal to k at +32.
CREATE FUNCTION lion_dc_summary(idx regclass, k int, OUT blk bigint, OUT pos int)
LANGUAGE plpgsql AS $$
DECLARE
	nblocks bigint := pg_relation_size(idx) / current_setting('block_size')::int;
	page bytea;
	off int;
	p int;
BEGIN
	FOR b IN 1 .. nblocks - 1 LOOP
		page := lion_dc_block(idx, b);
		FOR off IN 1 .. ((lion_dc_u32(page, 12) & 65535) - 24) / 4 LOOP
			p := lion_dc_item(page, off);
			CONTINUE WHEN p < 24 OR p + 40 > length(page);
			IF lion_dc_u32(page, p) = 0
			   AND (lion_dc_u32(page, p + 4) & 128) <> 0
			   AND (lion_dc_u32(page, p + 4) >> 16) = 8
			   AND lion_dc_u32(page, p + 32) = k THEN
				blk := b;
				pos := p;
				RETURN;
			END IF;
		END LOOP;
	END LOOP;
END $$;

SELECT blk IS NOT NULL AS found FROM lion_dc_summary('lion_dc_s_i', 32);
SELECT lion_dc_poke('lion_dc_s_i', s.blk, s.pos + 32, lion_dc_le32(24))
  FROM lion_dc_summary('lion_dc_s_i', 32) s;
SELECT lion_dc_try($$SELECT lion_index_verify('lion_dc_s_i', false)$$);
DROP TABLE lion_dc_s;
DROP FUNCTION lion_dc_summary(regclass, int);
DROP FUNCTION lion_dc_le32(bigint);

-- ---------- an undamaged index still works, and the rows are all there ----------
CREATE INDEX lion_dc_i ON lion_dc USING lion (k);
SELECT count(*) FROM lion_dc WHERE k = 5;
SELECT lion_index_verify('lion_dc_i');
RESET enable_seqscan;
RESET statement_timeout;

DROP TABLE lion_dc;
DROP TABLE lion_dc_size;
DROP FUNCTION lion_dc_try(text);
DROP FUNCTION lion_dc_poke(regclass, bigint, int, bytea);
DROP FUNCTION lion_dc_evict(regclass);
DROP FUNCTION lion_dc_item(bytea, int);
DROP FUNCTION lion_dc_root(regclass);
DROP FUNCTION lion_dc_block(regclass, bigint);
DROP FUNCTION lion_dc_u32(bytea, int);
\else
\echo 'pg_buffercache_evict() needs PostgreSQL 17: skipped'
\endif
