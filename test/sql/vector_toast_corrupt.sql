-- A damaged locator (DESIGN.md §44.13, "Reading by place"): a place that is
-- wrong is a read that fails its checks, and the scan reads that row from
-- the heap, as it would have without one; only an entry that would make the
-- scan return another row's vector, or one that is not an entry at all, is
-- an ERROR.  And a format-4 index, from before the locator, is read, kept
-- up and vacuumed as one without a locator, until a REINDEX.
--
-- As in corrupt.sql, the pages are damaged ON DISK, under the server: each
-- case writes the index out and drops its pages from the buffer pool
-- (pg_buffercache_evict(), PostgreSQL 17 and later - on 16 the test is
-- skipped, test/expected/vector_toast_corrupt_1.out), overwrites a few
-- bytes of the file, and lets the next read see them; then a REINDEX makes
-- the index whole again.  The byte offsets assume a little-endian server.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_lion_vector;
CREATE EXTENSION IF NOT EXISTS pageinspect;
CREATE EXTENSION IF NOT EXISTS pg_buffercache;
RESET client_min_messages;
SELECT current_setting('server_version_num')::int >= 170000 AS tc_can_evict \gset
\if :tc_can_evict
SET statement_timeout = '60s';
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET pg_lion.vector_epsilon = 6;
SET pg_lion.vector_margin = 100;
SET pg_lion.vector_patience = 0;
SET pg_lion.vector_prefetch = off;

CREATE FUNCTION tc_vec(seed bigint, n int) RETURNS vector
LANGUAGE sql IMMUTABLE AS $$
	SELECT array_agg(((hashint8extended(seed * 10000 + j, 45) % 100000)::float8
					  / 100000.0)::real ORDER BY j)::vector
	  FROM generate_series(1, n) j
$$;
CREATE FUNCTION tc_u16(p bytea, o int) RETURNS int LANGUAGE sql IMMUTABLE AS
$$ SELECT get_byte(p, o) | (get_byte(p, o + 1) << 8) $$;
CREATE FUNCTION tc_u32(p bytea, o int) RETURNS bigint LANGUAGE sql IMMUTABLE AS
$$ SELECT tc_u16(p, o)::bigint | (tc_u16(p, o + 2)::bigint << 16) $$;
CREATE FUNCTION tc_le16(v int) RETURNS bytea LANGUAGE sql IMMUTABLE AS
$$ SELECT set_byte(set_byte('\x0000'::bytea, 0, v & 255), 1, (v >> 8) & 255) $$;
CREATE FUNCTION tc_le32(v bigint) RETURNS bytea LANGUAGE sql IMMUTABLE AS
$$ SELECT tc_le16((v & 65535)::int) || tc_le16(((v >> 16) & 65535)::int) $$;

-- Write an index out and drop its pages from the buffer pool.
CREATE FUNCTION tc_evict(idx regclass) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
	CHECKPOINT;
	PERFORM pg_buffercache_evict(bufferid)
	   FROM pg_buffercache
	  WHERE relfilenode = pg_relation_filenode(idx)
		AND reldatabase = (SELECT oid FROM pg_database WHERE datname = current_database());
END $$;

-- Overwrite bytes at byte pos of block blk of the file, with the page's
-- checksum (pd_checksum, at 8) made right again.
CREATE FUNCTION tc_poke(idx regclass, blk bigint, pos int, bytes bytea)
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
	page := overlay(page PLACING tc_le16(ck) FROM 9);
	f := overlay(f PLACING page FROM (blk * bs)::int + 1);
	lo := lo_from_bytea(0, f);
	PERFORM lo_export(lo, path);
	PERFORM lo_unlink(lo);
END $$;

-- Block blk of an index, read from the file, not the buffer pool: a page
-- read into the pool before the file is written would hide the damage.
CREATE FUNCTION tc_block(idx regclass, blk bigint) RETURNS bytea
LANGUAGE sql AS $$
	SELECT pg_read_binary_file(pg_relation_filepath(idx),
							   blk * current_setting('block_size')::int,
							   current_setting('block_size')::int)
$$;

/*
 * Where a row's LOC entry lies, read from the file - its block, and its byte in the page: the
 * row's TID (6 bytes), nruns, flags, the value's id (at 8), then the runs,
 * a TOAST block (4 bytes), a line pointer and a count (2 each), from 12 -
 * and the value's id and first run as they are.  tc_slot() is the same of
 * the place in a row's REFINE item, after its 8-bit codes (the meta page's
 * padded dims, at 40): the value's id, then the runs, from 4.
 */
CREATE FUNCTION tc_loc(idx regclass, t tid,
					   OUT blk int, OUT pos int, OUT valueid bigint, OUT run bytea)
LANGUAGE plpgsql AS $$
DECLARE
	bs int := current_setting('block_size')::int;
	p bytea;
	b int;
	i int;
	o int;
BEGIN
	FOR b IN 1 .. pg_relation_size(idx) / bs - 1 LOOP
		p := tc_block(idx, b);
		CONTINUE WHEN tc_u16(p, bs - 4) <> 6 OR tc_u16(p, bs - 2) <> 65420;
		FOR i IN 1 .. (tc_u16(p, 12) - 24) / 4 LOOP
			o := (tc_u32(p, 20 + 4 * i) & 32767)::int;
			IF format('(%s,%s)', (tc_u16(p, o)::bigint << 16) | tc_u16(p, o + 2),
					  tc_u16(p, o + 4))::tid = t THEN
				blk := b;
				pos := o;
				valueid := tc_u32(p, o + 8);
				run := substring(p FROM o + 13 FOR 8);
				RETURN;
			END IF;
		END LOOP;
	END LOOP;
END $$;
CREATE FUNCTION tc_slot(idx regclass, t tid,
						OUT blk int, OUT pos int, OUT valueid bigint, OUT run bytea)
LANGUAGE plpgsql AS $$
DECLARE
	bs int := current_setting('block_size')::int;
	dimsp int := tc_u32(tc_block(idx, 0), 40)::int;
	p bytea;
	b int;
	i int;
	o int;
BEGIN
	FOR b IN 1 .. pg_relation_size(idx) / bs - 1 LOOP
		p := tc_block(idx, b);
		CONTINUE WHEN tc_u16(p, bs - 4) <> 3 OR tc_u16(p, bs - 2) <> 65420;
		FOR i IN 1 .. (tc_u16(p, 12) - 24) / 4 LOOP
			o := (tc_u32(p, 20 + 4 * i) & 32767)::int;
			IF tc_u16(p, o + 6) > 0 AND
			   format('(%s,%s)', (tc_u16(p, o)::bigint << 16) | tc_u16(p, o + 2),
					  tc_u16(p, o + 4))::tid = t THEN
				blk := b;
				pos := o + 16 + dimsp;
				valueid := tc_u32(p, pos);
				run := substring(p FROM pos + 5 FOR 8);
				RETURN;
			END IF;
		END LOOP;
	END LOOP;
END $$;
CREATE FUNCTION tc_meta(idx regclass) RETURNS text LANGUAGE sql AS $$
	SELECT format('format %s, pd_lower %s, %s', tc_u16(m, 28), tc_u16(m, 12),
				  CASE WHEN tc_u16(m, 34) & 2 <> 0 THEN 'a locator' ELSE 'no locator' END)
	  FROM get_raw_page(idx::text, 0) m
$$;

/*
 * tc_cmp() runs q - columns id and d, ORDER BY d - with direct reads off,
 * then on, and compares rows, order and distances; or returns the error
 * the second run raised, its numbers out.  exact: the exact path
 * (LionOrdered), every core scan disabled; or a probing index scan.
 */
CREATE FUNCTION tc_run(q text, direct boolean, OUT ids int[], OUT ds float8[])
LANGUAGE plpgsql AS $$
DECLARE
	r record;
BEGIN
	PERFORM set_config('pg_lion.vector_toast_direct', direct::text, true);
	ids := '{}';
	ds := '{}';
	FOR r IN EXECUTE q LOOP
		ids := ids || r.id;
		ds := ds || r.d::float8;
	END LOOP;
	PERFORM set_config('pg_lion.vector_toast_direct', 'on', true);
END $$;
CREATE FUNCTION tc_cmp(q text, exact boolean) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	how text := 'no index';
	a record;
	b record;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_sort', 'off', true);
	PERFORM set_config('enable_indexscan', (NOT exact)::text, true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('enable_tidscan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(LionOrdered\)' THEN
			how := 'exact path';
		ELSIF ln ~ 'Index Scan using' AND how = 'no index' THEN
			how := 'probed';
		END IF;
	END LOOP;
	SELECT * INTO b FROM tc_run(q, false);
	BEGIN
		SELECT * INTO a FROM tc_run(q, true);
	EXCEPTION WHEN OTHERS THEN
		RETURN format('%s, %s: %s', how, SQLSTATE,
					  regexp_replace(SQLERRM, '[0-9]+', 'N', 'g'));
	END;
	RETURN format('%s, %s rows, %s', how, cardinality(a.ids),
				  CASE WHEN a.ids = b.ids AND a.ds IS NOT DISTINCT FROM b.ds
					   THEN 'the same' ELSE 'DIFFERENT' END);
END $$;

-- 200 rows, each vector in TOAST on a page of its own; the queries are
-- nearest to row 13's (cat 3), which each damage is to
CREATE TABLE tc (id int, cat int, v vector(1536)) WITH (autovacuum_enabled = off);
INSERT INTO tc SELECT i, i % 10, tc_vec(i, 1536) FROM generate_series(1, 200) i;
CREATE INDEX tc_cat ON tc USING lion (cat);
CREATE INDEX tc_v ON tc USING lionvec (v) WITH (partitions = 2);
ANALYZE tc;
SELECT ctid AS tc_13 FROM tc WHERE id = 13 \gset
SELECT ctid AS tc_23 FROM tc WHERE id = 23 \gset
SELECT ctid AS tc_100 FROM tc WHERE id = 100 \gset
\set qe 'SELECT id, v <-> tc_vec(13, 1536) AS d FROM tc WHERE cat = 3 ORDER BY v <-> tc_vec(13, 1536)'
\set qp 'SELECT id, v <-> tc_vec(13, 1536) AS d FROM tc ORDER BY v <-> tc_vec(13, 1536) LIMIT 10'
SELECT tc_cmp(:'qe', true) AS exact, tc_cmp(:'qp', false) AS probed;

-- ---------- 1. a LOC entry's run past the TOAST relation's end ----------
-- not read at all: the row is the heap's
SELECT tc_evict('tc_v');
SELECT tc_poke('tc_v', l.blk, l.pos + 12, tc_le32(1000000))
  FROM tc_loc('tc_v', :'tc_13') l;
SELECT tc_cmp(:'qe', true) AS exact;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SET enable_sort = off;
-- prepared, and run once, at the settings above, so that its DEBUG1 run
-- parses and plans nothing: what a parse looks up can grow a catalog
-- cache, whose rehash DEBUG1 reports too
PREPARE tc_scan AS SELECT count(*) FROM (SELECT id FROM tc WHERE cat = 3 ORDER BY v <-> tc_vec(13, 1536)) s;
EXECUTE tc_scan;
SET client_min_messages = debug1;
EXECUTE tc_scan;
RESET client_min_messages;
DEALLOCATE tc_scan;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
RESET enable_sort;
REINDEX INDEX tc_v;

-- ---------- 2. ... on another value's page ----------
-- read, and found to hold another value: the row is the heap's
SELECT tc_evict('tc_v');
SELECT tc_poke('tc_v', l.blk, l.pos + 12, substring(o.run FROM 1 FOR 4))
  FROM tc_loc('tc_v', :'tc_13') l, tc_loc('tc_v', :'tc_100') o;
SELECT tc_cmp(:'qe', true) AS exact;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SET enable_sort = off;
PREPARE tc_scan AS SELECT count(*) FROM (SELECT id FROM tc WHERE cat = 3 ORDER BY v <-> tc_vec(13, 1536)) s;
EXECUTE tc_scan;
SET client_min_messages = debug1;
EXECUTE tc_scan;
RESET client_min_messages;
DEALLOCATE tc_scan;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
RESET enable_sort;
REINDEX INDEX tc_v;

-- ---------- 3. ... at a line pointer the page does not have ----------
SELECT tc_evict('tc_v');
SELECT tc_poke('tc_v', l.blk, l.pos + 16, tc_le16(200))
  FROM tc_loc('tc_v', :'tc_13') l;
SELECT tc_cmp(:'qe', true) AS exact;
REINDEX INDEX tc_v;

-- ---------- 4. a LOC entry naming a value no row stores ----------
-- its chunks' ids are not the one named
SELECT tc_evict('tc_v');
SELECT tc_poke('tc_v', l.blk, l.pos + 8, tc_le32(l.valueid + 1000000))
  FROM tc_loc('tc_v', :'tc_13') l;
SELECT tc_cmp(:'qe', true) AS exact;
REINDEX INDEX tc_v;

-- ---------- 5. a LOC entry naming another row's value, and its place ----------
-- row 23's entry made row 13's: read by place, row 23's vector would be row
-- 13's, the nearest; the check before row 23 is returned finds that row 23
-- stores another value, which only a damaged index can say
SELECT tc_evict('tc_v');
SELECT tc_poke('tc_v', b.blk, b.pos + 8, tc_le32(a.valueid) || a.run)
  FROM tc_loc('tc_v', :'tc_13') a, tc_loc('tc_v', :'tc_23') b;
SELECT tc_cmp(:'qe', true) AS exact;
-- the probing scan reads no LOC entry
SELECT tc_cmp(:'qp', false) AS probed;
REINDEX INDEX tc_v;

-- ---------- 6. a LOC entry that is not one: flags set, or runs miscounted ----------
SELECT tc_evict('tc_v');
SELECT tc_poke('tc_v', l.blk, l.pos + 7, '\x01'::bytea)
  FROM tc_loc('tc_v', :'tc_13') l;
SELECT tc_cmp(:'qe', true) AS exact;
REINDEX INDEX tc_v;
SELECT tc_evict('tc_v');
SELECT tc_poke('tc_v', l.blk, l.pos + 6, '\x03'::bytea)
  FROM tc_loc('tc_v', :'tc_13') l;
SELECT tc_cmp(:'qe', true) AS exact;
REINDEX INDEX tc_v;

-- ---------- 7. a REFINE item's place wrong ----------
-- past the end, or on another value's page: the rerank reads the row's
-- vector from the heap row it fetched
SELECT tc_evict('tc_v');
SELECT tc_poke('tc_v', s.blk, s.pos + 4, tc_le32(1000000))
  FROM tc_slot('tc_v', :'tc_13') s;
SELECT tc_cmp(:'qp', false) AS probed;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_sort = off;
PREPARE tc_scan AS SELECT count(*) FROM (SELECT id FROM tc ORDER BY v <-> tc_vec(13, 1536) LIMIT 1) s;
EXECUTE tc_scan;
SET client_min_messages = debug1;
EXECUTE tc_scan;
RESET client_min_messages;
DEALLOCATE tc_scan;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_sort;
REINDEX INDEX tc_v;
SELECT tc_evict('tc_v');
SELECT tc_poke('tc_v', s.blk, s.pos + 4, substring(o.run FROM 1 FOR 4))
  FROM tc_slot('tc_v', :'tc_13') s, tc_slot('tc_v', :'tc_100') o;
SELECT tc_cmp(:'qp', false) AS probed;
-- the exact path reads no REFINE item
SELECT tc_cmp(:'qe', true) AS exact;
REINDEX INDEX tc_v;

-- ---------- 8. a REFINE item naming another value ----------
-- the row the rerank fetched stores another: an ERROR
SELECT tc_evict('tc_v');
SELECT tc_poke('tc_v', s.blk, s.pos, tc_le32(o.valueid) || o.run)
  FROM tc_slot('tc_v', :'tc_13') s, tc_slot('tc_v', :'tc_100') o;
SELECT tc_cmp(:'qp', false) AS probed;
REINDEX INDEX tc_v;

-- and undamaged, the same rows again
SELECT tc_cmp(:'qe', true) AS exact, tc_cmp(:'qp', false) AS probed;
DROP TABLE tc;

-- ---------- 9. a format-4 index ----------
-- built over vectors in their rows (PLAIN): no locator, and its REFINE
-- items no places; its meta page then made format 4's, which ends before
-- the locator's fields (pd_lower 104, from 136)
CREATE TABLE tc4 (id int, cat int, v vector(1536)) WITH (autovacuum_enabled = off);
ALTER TABLE tc4 ALTER COLUMN v SET STORAGE PLAIN;
INSERT INTO tc4 SELECT i, i % 10, tc_vec(i, 1536) FROM generate_series(1, 100) i;
CREATE INDEX tc4_cat ON tc4 USING lion (cat);
CREATE INDEX tc4_v ON tc4 USING lionvec (v) WITH (partitions = 2);
ANALYZE tc4;
SELECT tc_meta('tc4_v');
SELECT tc_evict('tc4_v');
SELECT tc_poke('tc4_v', 0, 28, tc_le16(4));
SELECT tc_poke('tc4_v', 0, 12, tc_le16(104));
SELECT tc_poke('tc4_v', 0, 104, '\x0000000000000000000000000000000000000000000000000000000000000000'::bytea);
SELECT tc_meta('tc4_v');
-- vectors in TOAST from now on, which the index does not locate
ALTER TABLE tc4 ALTER COLUMN v SET STORAGE EXTERNAL;
INSERT INTO tc4 SELECT i, i % 10, tc_vec(i, 1536) FROM generate_series(101, 200) i;
SELECT tc_cmp('SELECT id, v <-> tc_vec(113, 1536) AS d FROM tc4 WHERE cat = 3 ORDER BY v <-> tc_vec(113, 1536)', true) AS exact,
	   tc_cmp('SELECT id, v <-> tc_vec(113, 1536) AS d FROM tc4 ORDER BY v <-> tc_vec(113, 1536) LIMIT 10', false) AS probed;
DELETE FROM tc4 WHERE id % 3 = 0;
VACUUM tc4;
SELECT tc_meta('tc4_v');
SELECT tc_cmp('SELECT id, v <-> tc_vec(113, 1536) AS d FROM tc4 WHERE cat = 3 ORDER BY v <-> tc_vec(113, 1536)', true) AS exact,
	   tc_cmp('SELECT id, v <-> tc_vec(113, 1536) AS d FROM tc4 ORDER BY v <-> tc_vec(113, 1536) LIMIT 10', false) AS probed;
-- a REINDEX writes format 5, and a locator of the rows in TOAST
REINDEX INDEX tc4_v;
SELECT tc_meta('tc4_v');
SELECT tc_cmp('SELECT id, v <-> tc_vec(113, 1536) AS d FROM tc4 WHERE cat = 3 ORDER BY v <-> tc_vec(113, 1536)', true) AS exact,
	   tc_cmp('SELECT id, v <-> tc_vec(113, 1536) AS d FROM tc4 ORDER BY v <-> tc_vec(113, 1536) LIMIT 10', false) AS probed;
DROP TABLE tc4;

RESET pg_lion.vector_prefetch;
RESET pg_lion.vector_patience;
RESET pg_lion.vector_margin;
RESET pg_lion.vector_epsilon;
RESET statement_timeout;
DROP FUNCTION tc_cmp(text, boolean), tc_run(text, boolean), tc_meta(regclass),
	tc_slot(regclass, tid), tc_loc(regclass, tid), tc_poke(regclass, bigint, int, bytea),
	tc_evict(regclass), tc_le32(bigint), tc_le16(int), tc_u32(bytea, int),
	tc_u16(bytea, int), tc_block(regclass, bigint), tc_vec(bigint, int);
\else
\echo 'pg_buffercache_evict() needs PostgreSQL 17: skipped'
\endif
