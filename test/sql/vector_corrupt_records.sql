-- Damage a lionvec insert or scan would have carried on with: an ERROR
-- before it locks the wrong page, writes, or reads past a page, never a wait
-- for ever or a PANIC.
--
-- 1. A meta page whose chain end, or whose refine_tail, names the meta page
--    itself.  An insert holds the meta page EXCLUSIVE from its first page to
--    its last (lionvec_append()), and it locked it a second time: a buffer
--    content lock is not reentrant, so the backend waited for itself, with
--    interrupts held off - no cancel, no terminate, only a restart of the
--    server ended it (lionvec_insert_readbuf()).
-- 2. The CODE tail of the NULL rows with its pd_lower inside the page
--    header, which the buffer manager reads in - it checks pd_lower against
--    pd_upper, not against the header - and which then reads as a page with
--    no items and room to spare: the next NULL row was added to it in a
--    GenericXLog copy, where PageAddItemExtended() PANICked
--    (lionvec_check_page()).
-- 3. A row's REFINE item whose line pointer's offset lies past the page,
--    its length right.  lionvec_refine_at() checked the length but not the
--    place, which lionvec_item_ok() checks of a PART or REGION item, and a
--    scan read the item - its header, then its codes - from whatever memory
--    follows the page: an offset goes up to 32,767.
-- 4. The same of a CODE item, the NULL rows' first: lionvec_code_at()
--    checked its length and not its place either, and VACUUM, which reads
--    every CODE item, read the row's flags and TID from past the page.  Every
--    item reader now asks lionvec_item_inside().
--
-- As in test/sql/corrupt_records.sql, the pages are damaged ON DISK and read
-- back from there by moving the index to another tablespace, which copies
-- the file as it is, so that this runs on PostgreSQL 16 too.  The meta
-- page's fields follow the 24-byte page header (LionVecMetaData):
-- null_tail at 72, refine_tail at 76.  The byte offsets assume a
-- little-endian server.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_lion_vector;
CREATE EXTENSION IF NOT EXISTS pageinspect;
-- left behind by a run that stopped part-way
DROP TABLESPACE IF EXISTS lion_vr_ts;
RESET client_min_messages;
SET statement_timeout = '60s';
SET max_parallel_maintenance_workers = 0;
SET allow_in_place_tablespaces = on;
CREATE TABLESPACE lion_vr_ts LOCATION '';
RESET allow_in_place_tablespaces;

CREATE FUNCTION lion_vr_u32(f bytea, off int) RETURNS bigint
LANGUAGE sql IMMUTABLE AS $$
	SELECT get_byte(f, off)::bigint | (get_byte(f, off + 1)::bigint << 8) |
		   (get_byte(f, off + 2)::bigint << 16) | (get_byte(f, off + 3)::bigint << 24)
$$;
CREATE FUNCTION lion_vr_le32(v bigint) RETURNS bytea
LANGUAGE sql IMMUTABLE AS $$
	SELECT set_byte(set_byte(set_byte(set_byte('\x00000000'::bytea,
		0, (v & 255)::int), 1, ((v >> 8) & 255)::int),
		2, ((v >> 16) & 255)::int), 3, ((v >> 24) & 255)::int)
$$;

-- Block blk of an index, read from the file.
CREATE FUNCTION lion_vr_block(idx regclass, blk bigint) RETURNS bytea
LANGUAGE sql AS $$
	SELECT pg_read_binary_file(pg_relation_filepath(idx),
							   blk * current_setting('block_size')::int,
							   current_setting('block_size')::int)
$$;

-- Write an index out, so that its file is what the buffer pool holds.
CREATE FUNCTION lion_vr_flush(idx regclass) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
	CHECKPOINT;
END $$;

-- Overwrite bytes at byte pos of block blk of the file, with the page's new
-- checksum (pd_checksum, the uint16 at byte 8).
CREATE FUNCTION lion_vr_poke(idx regclass, blk bigint, pos int, bytes bytea)
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

-- Have the server read the index from its file: move it to the other
-- tablespace, which copies the file as it is.
CREATE FUNCTION lion_vr_reread(idx regclass) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('ALTER INDEX %s SET TABLESPACE %I', idx,
				   CASE WHEN (SELECT reltablespace FROM pg_class WHERE oid = idx) = 0
						THEN 'lion_vr_ts' ELSE 'pg_default' END);
END $$;

-- Run a statement; report its SQLSTATE and its message with the numbers out.
CREATE FUNCTION lion_vr_try(q text) RETURNS text
LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
	RETURN SQLSTATE || ': ' || regexp_replace(SQLERRM, '[0-9]+', 'N', 'g');
END $$;

CREATE TABLE lion_vr (id int, v vector(3)) WITH (autovacuum_enabled = off);
INSERT INTO lion_vr SELECT i, ARRAY[i, i % 7, 1]::vector FROM generate_series(1, 50) i;
INSERT INTO lion_vr VALUES (0, NULL);

-- ---------- 1. a link to the meta page ----------
-- The NULL rows' chain end, and then refine_tail, made 0.
CREATE INDEX lion_vr_i ON lion_vr USING lionvec (v);
SELECT lion_vr_flush('lion_vr_i');
SELECT lion_vr_u32(m, 72) > 0 AS null_tail, lion_vr_u32(m, 76) > 0 AS refine_tail
  FROM lion_vr_block('lion_vr_i', 0) m;
SELECT lion_vr_poke('lion_vr_i', 0, 72, lion_vr_le32(0));
SELECT lion_vr_reread('lion_vr_i');
SELECT lion_vr_try('INSERT INTO lion_vr VALUES (100, NULL)');
DROP INDEX lion_vr_i;
CREATE INDEX lion_vr_i ON lion_vr USING lionvec (v);
SELECT lion_vr_flush('lion_vr_i');
SELECT lion_vr_poke('lion_vr_i', 0, 76, lion_vr_le32(0));
SELECT lion_vr_reread('lion_vr_i');
SELECT lion_vr_try($$INSERT INTO lion_vr VALUES (102, '[1,2,3]')$$);
DROP INDEX lion_vr_i;

-- ---------- 2. a CODE tail whose pd_lower is inside the page header ----------
-- The NULL rows' tail, its pd_lower (bytes 12 and 13) made 0.
CREATE INDEX lion_vr_i ON lion_vr USING lionvec (v);
SELECT lion_vr_flush('lion_vr_i');
SELECT lion_vr_u32(lion_vr_block('lion_vr_i', 0), 72) AS lion_vr_tail \gset
SELECT get_byte(p, 12) | (get_byte(p, 13) << 8) > 24 AS has_items
  FROM lion_vr_block('lion_vr_i', :lion_vr_tail) p;
SELECT lion_vr_poke('lion_vr_i', :lion_vr_tail, 12, '\x0000'::bytea);
SELECT lion_vr_reread('lion_vr_i');
SELECT lion_vr_try('INSERT INTO lion_vr VALUES (100, NULL)');
DROP INDEX lion_vr_i;

-- ---------- 3. a REFINE item past its page ----------
-- Row 7's, on refine_tail's page (it holds every one of these rows'), its
-- line pointer (4 bytes from byte 24 on: the offset in the low 15 bits, the
-- length in the high 15) given the block size as its offset; a scan ordered
-- by row 7's vector refines it.
CREATE INDEX lion_vr_i ON lion_vr USING lionvec (v);
SELECT lion_vr_flush('lion_vr_i');
SELECT lion_vr_u32(lion_vr_block('lion_vr_i', 0), 76) AS lion_vr_tail \gset
SELECT 20 + 4 * i AS lion_vr_lp, lp AS lion_vr_lpv
  FROM lion_vr_block('lion_vr_i', :lion_vr_tail) p,
	   generate_series(1, ((lion_vr_u32(p, 12) & 65535)::int - 24) / 4) i,
	   LATERAL (SELECT lion_vr_u32(p, 20 + 4 * i) AS lp) l,
	   LATERAL (SELECT (lp & 32767)::int AS o) io
 WHERE format('(%s,%s)', (lion_vr_u32(p, o) >> 16) | ((lion_vr_u32(p, o) & 65535) << 16),
			  lion_vr_u32(p, o + 4) & 65535)::tid = (SELECT ctid FROM lion_vr WHERE id = 7) \gset
SELECT lion_vr_poke('lion_vr_i', :lion_vr_tail, :lion_vr_lp,
					lion_vr_le32(((:lion_vr_lpv >> 15) << 15) | current_setting('block_size')::int));
SELECT lion_vr_reread('lion_vr_i');
SET enable_seqscan = off;
SELECT lion_vr_try($$SELECT id FROM lion_vr ORDER BY v <-> '[7,0,1]' LIMIT 1$$);
RESET enable_seqscan;
DROP INDEX lion_vr_i;

-- ---------- 4. a CODE item past its page ----------
-- The NULL rows' tail's first line pointer given the block size as its
-- offset, its length kept; VACUUM, with a row to remove, reads it.
CREATE INDEX lion_vr_i ON lion_vr USING lionvec (v);
SELECT lion_vr_flush('lion_vr_i');
SELECT lion_vr_u32(lion_vr_block('lion_vr_i', 0), 72) AS lion_vr_tail \gset
SELECT lion_vr_u32(lion_vr_block('lion_vr_i', :lion_vr_tail), 24) AS lion_vr_lpv \gset
SELECT lion_vr_poke('lion_vr_i', :lion_vr_tail, 24,
					lion_vr_le32(((:lion_vr_lpv >> 15) << 15) | current_setting('block_size')::int));
SELECT lion_vr_reread('lion_vr_i');
DELETE FROM lion_vr WHERE id = 50;
\set VERBOSITY sqlstate
VACUUM lion_vr;
\set VERBOSITY terse
DROP INDEX lion_vr_i;

-- ---------- an undamaged index still works ----------
CREATE INDEX lion_vr_i ON lion_vr USING lionvec (v);
SELECT lion_vr_reread('lion_vr_i');
INSERT INTO lion_vr VALUES (100, NULL), (101, '[1,2,3]');
SET enable_seqscan = off;
SELECT id FROM lion_vr ORDER BY v <-> '[1,2,3]' LIMIT 1;
RESET enable_seqscan;
RESET statement_timeout;
RESET max_parallel_maintenance_workers;

DROP TABLE lion_vr;
DROP FUNCTION lion_vr_try(text);
DROP FUNCTION lion_vr_reread(regclass);
DROP FUNCTION lion_vr_poke(regclass, bigint, int, bytea);
DROP FUNCTION lion_vr_flush(regclass);
DROP FUNCTION lion_vr_block(regclass, bigint);
DROP FUNCTION lion_vr_le32(bigint);
DROP FUNCTION lion_vr_u32(bytea, int);
DROP TABLESPACE lion_vr_ts;
