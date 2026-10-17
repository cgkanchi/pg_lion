-- lionvec's vectors in TOAST, read by their chunks' places (DESIGN.md
-- §44.13).
--
-- An index whose vectors may lie in TOAST keeps a locator - for each row
-- whose vector does, its value's id and where its chunks lie, on LOC pages
-- - and, where the vectors are wide enough to leave room, the same place in
-- each row's REFINE item.  The exact path (a small restriction, LionOrdered)
-- reads its members' vectors by the locator, the probed path's rerank by
-- the REFINE items, every chunk checked, and a row read so is returned
-- only once its heap tuple, fetched under the scan's snapshot, shows the
-- same value.  pg_lion.vector_toast_direct off reads every row as before:
-- every query below is run both ways and must return the same rows, in the
-- same order, at the same distances (tt_cmp()), through each change a
-- table's rows and its TOAST relation can go through - updates HOT and
-- not, a vector set to itself or changed, deletes and inserts, an insert
-- rolled back, the TOAST relation vacuumed or rewritten on its own - none
-- of which may raise an error.  tt_check() reads the locator off the
-- index's pages and checks it against the heap and the TOAST relation.
--
-- pg_lion.vector_epsilon, vector_margin and vector_patience are set as in
-- vector.sql, so that a probing scan returns exactly a sort's rows.  The
-- byte offsets assume a little-endian server.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_lion_vector;
CREATE EXTENSION IF NOT EXISTS pageinspect;
RESET client_min_messages;
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET pg_lion.vector_epsilon = 6;
SET pg_lion.vector_margin = 100;
SET pg_lion.vector_patience = 0;

-- a pseudo-random vector of n coordinates in (-scale, scale), the same on
-- every platform
CREATE FUNCTION tt_vec(seed bigint, n int, scale float8 DEFAULT 1.0)
RETURNS vector LANGUAGE sql IMMUTABLE AS $$
	SELECT array_agg(((hashint8extended(seed * 10000 + j, 45) % 100000)::float8
					  / 100000.0 * scale)::real ORDER BY j)::vector
	  FROM generate_series(1, n) j
$$;

CREATE FUNCTION tt_u16(p bytea, o int) RETURNS int LANGUAGE sql IMMUTABLE AS
$$ SELECT get_byte(p, o) | (get_byte(p, o + 1) << 8) $$;
CREATE FUNCTION tt_u32(p bytea, o int) RETURNS bigint LANGUAGE sql IMMUTABLE AS
$$ SELECT tt_u16(p, o)::bigint | (tt_u16(p, o + 2)::bigint << 16) $$;

/*
 * tt_meta() reads the meta page's locator fields: its format (at 28), the
 * flags (34: 1 spills, 2 keeps a locator), whether the LOCDIR head (104)
 * is set, and the runs a REFINE item's place holds (132).  tt_pages()
 * counts an index's pages of a type, by the special space's last words
 * (6 LOC, 7 LOCDIR).
 */
CREATE FUNCTION tt_meta(idx regclass, fork text DEFAULT 'main') RETURNS text
LANGUAGE sql AS $$
	SELECT format('format %s, flags %s, %s, %s runs in a refine item''s place',
				  tt_u16(m, 28), tt_u16(m, 34),
				  CASE WHEN tt_u32(m, 104) = 4294967295 THEN 'no locator'
					   ELSE 'a locator' END,
				  tt_u16(m, 132))
	  FROM get_raw_page(idx::text, fork, 0) m
$$;
CREATE FUNCTION tt_pages(idx regclass, type int, fork text DEFAULT 'main')
RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE
	bs int := current_setting('block_size')::int;
	n bigint := 0;
	p bytea;
	b int;
BEGIN
	FOR b IN 1 .. pg_relation_size(idx, fork) / bs - 1 LOOP
		p := get_raw_page(idx::text, fork, b);
		IF tt_u16(p, bs - 4) = type AND tt_u16(p, bs - 2) = 65420 THEN
			n := n + 1;
		END IF;
	END LOOP;
	RETURN n;
END $$;

/*
 * tt_locs() reads the LOC entries (DESIGN.md §44.13, "The format"): the
 * row's TID (6 bytes), the runs' count, the flags, the value's id, then
 * the runs - a block of the TOAST relation (4 bytes), a line pointer and a
 * count of chunks (2 each) - as '(block,line pointer)xchunks'.
 * tt_slots() reads the places in the REFINE items, after their 8-bit codes
 * (the meta page's dims padded, at 40): a count of runs in use (at 6), the
 * value's id, and the runs.
 */
CREATE FUNCTION tt_locs(idx regclass)
RETURNS TABLE (blk int, row_tid tid, nruns int, flags int, valueid oid, runs text)
LANGUAGE plpgsql AS $$
DECLARE
	bs int := current_setting('block_size')::int;
	p bytea;
	b int;
	i int;
	k int;
	lp bigint;
	o int;
BEGIN
	FOR b IN 1 .. pg_relation_size(idx) / bs - 1 LOOP
		p := get_raw_page(idx::text, b);
		CONTINUE WHEN tt_u16(p, bs - 4) <> 6 OR tt_u16(p, bs - 2) <> 65420;
		FOR i IN 1 .. (tt_u16(p, 12) - 24) / 4 LOOP
			lp := tt_u32(p, 20 + 4 * i);
			CONTINUE WHEN (lp >> 15) & 3 <> 1;
			o := (lp & 32767)::int;
			blk := b;
			row_tid := format('(%s,%s)', (tt_u16(p, o)::bigint << 16) | tt_u16(p, o + 2),
							  tt_u16(p, o + 4))::tid;
			nruns := get_byte(p, o + 6);
			flags := get_byte(p, o + 7);
			valueid := tt_u32(p, o + 8)::oid;
			runs := '';
			FOR k IN 0 .. nruns - 1 LOOP
				runs := runs || CASE WHEN k > 0 THEN ' ' ELSE '' END ||
					format('(%s,%s)x%s', tt_u32(p, o + 12 + 8 * k),
						   tt_u16(p, o + 16 + 8 * k), tt_u16(p, o + 18 + 8 * k));
			END LOOP;
			RETURN NEXT;
		END LOOP;
	END LOOP;
END $$;
CREATE FUNCTION tt_slots(idx regclass)
RETURNS TABLE (row_tid tid, valueid oid, runs text)
LANGUAGE plpgsql AS $$
DECLARE
	bs int := current_setting('block_size')::int;
	dimsp int := tt_u32(get_raw_page(idx::text, 0), 40)::int;
	p bytea;
	b int;
	i int;
	k int;
	n int;
	lp bigint;
	o int;
BEGIN
	FOR b IN 1 .. pg_relation_size(idx) / bs - 1 LOOP
		p := get_raw_page(idx::text, b);
		CONTINUE WHEN tt_u16(p, bs - 4) <> 3 OR tt_u16(p, bs - 2) <> 65420;
		FOR i IN 1 .. (tt_u16(p, 12) - 24) / 4 LOOP
			lp := tt_u32(p, 20 + 4 * i);
			CONTINUE WHEN (lp >> 15) & 3 <> 1;
			o := (lp & 32767)::int;
			n := tt_u16(p, o + 6);
			CONTINUE WHEN n = 0;
			row_tid := format('(%s,%s)', (tt_u16(p, o)::bigint << 16) | tt_u16(p, o + 2),
							  tt_u16(p, o + 4))::tid;
			o := o + 16 + dimsp;
			valueid := tt_u32(p, o)::oid;
			runs := '';
			FOR k IN 0 .. n - 1 LOOP
				runs := runs || CASE WHEN k > 0 THEN ' ' ELSE '' END ||
					format('(%s,%s)x%s', tt_u32(p, o + 4 + 8 * k),
						   tt_u16(p, o + 8 + 8 * k), tt_u16(p, o + 10 + 8 * k));
			END LOOP;
			RETURN NEXT;
		END LOOP;
	END LOOP;
END $$;

/*
 * tt_raw() reads every tuple of a table's pages (pageinspect, whatever its
 * visibility) and its column att as stored: whether it is a TOAST pointer
 * (a one-byte header of 0x01, tag 18), uncompressed (its stored bytes, the
 * low 30 bits of the word at 6, its raw size at 2 less 4), its value's id
 * (at 10) and size.  tt_visible() is the TIDs of the rows the snapshot
 * sees.  tt_places() is where the TOAST relation holds a value's chunks,
 * as runs.
 */
CREATE FUNCTION tt_raw(tbl regclass, att int)
RETURNS TABLE (row_tid tid, normal boolean, ext boolean, plain boolean,
			   valueid oid, size bigint)
LANGUAGE plpgsql AS $$
DECLARE
	bs int := current_setting('block_size')::int;
	b int;
	r record;
	a bytea;
BEGIN
	FOR b IN 0 .. pg_relation_size(tbl) / bs - 1 LOOP
		FOR r IN SELECT * FROM heap_page_item_attrs(get_raw_page(tbl::text, b), tbl) LOOP
			row_tid := format('(%s,%s)', b, r.lp)::tid;
			normal := r.lp_flags = 1;
			ext := false;
			plain := false;
			valueid := NULL;
			size := NULL;
			a := CASE WHEN normal THEN r.t_attrs[att] END;
			IF a IS NOT NULL AND length(a) = 18 AND get_byte(a, 0) = 1 AND get_byte(a, 1) = 18 THEN
				ext := true;
				size := tt_u32(a, 6) & 1073741823;
				plain := size = tt_u32(a, 2) - 4;
				valueid := tt_u32(a, 10)::oid;
			END IF;
			RETURN NEXT;
		END LOOP;
	END LOOP;
END $$;
CREATE FUNCTION tt_visible(tbl regclass) RETURNS SETOF tid
LANGUAGE plpgsql AS $$
BEGIN
	RETURN QUERY EXECUTE format('SELECT ctid FROM %s', tbl);
END $$;
CREATE FUNCTION tt_places(toast regclass, v oid) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	r record;
	runs text := '';
	cb bigint := -1;
	co int := -1;
	cn int := 0;
BEGIN
	FOR r IN EXECUTE format('SELECT (ctid::text::point)[0]::bigint AS b, (ctid::text::point)[1]::int AS o
							   FROM %s WHERE chunk_id = $1 ORDER BY chunk_seq', toast) USING v LOOP
		IF r.b = cb AND r.o = co + cn THEN
			cn := cn + 1;
		ELSE
			IF cn > 0 THEN
				runs := runs || CASE WHEN runs <> '' THEN ' ' ELSE '' END ||
					format('(%s,%s)x%s', cb, co, cn);
			END IF;
			cb := r.b;
			co := r.o;
			cn := 1;
		END IF;
	END LOOP;
	IF cn > 0 THEN
		runs := runs || CASE WHEN runs <> '' THEN ' ' ELSE '' END ||
			format('(%s,%s)x%s', cb, co, cn);
	END IF;
	RETURN runs;
END $$;

/*
 * tt_check() checks an index's locator against its table: of its entries,
 * those of a value some row the snapshot sees stores (live) - placed where
 * the TOAST relation holds its chunks, or not (stale: the scan finds out,
 * and reads the row from the heap) - and those of rows gone; whether every
 * entry's TID holds the value it names, where a tuple is still there; and
 * of the rows the snapshot sees whose vector is in TOAST, uncompressed and
 * of the index's length, how many have a live entry.  And the places in
 * the REFINE items: how many, and how many are their rows' entries'.
 */
CREATE FUNCTION tt_check(idx regclass) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	tbl regclass := (SELECT indrelid FROM pg_index WHERE indexrelid = idx);
	att int := (SELECT indkey[0] FROM pg_index WHERE indexrelid = idx);
	toast regclass := (SELECT reltoastrelid FROM pg_class WHERE oid = tbl);
	dims int := tt_u32(get_raw_page(idx::text, 0), 36)::int;
	half boolean := (SELECT opcintype = 'halfvec'::regtype FROM pg_opclass
					  WHERE oid = (SELECT indclass[0] FROM pg_index WHERE indexrelid = idx));
	r text;
BEGIN
	CREATE TEMP TABLE tt_e AS SELECT * FROM tt_locs(idx);
	CREATE TEMP TABLE tt_r AS SELECT * FROM tt_raw(tbl, att);
	CREATE TEMP TABLE tt_v AS
		SELECT r.* FROM tt_r r WHERE r.row_tid IN (SELECT tt_visible(tbl));
	CREATE TEMP TABLE tt_s AS SELECT * FROM tt_slots(idx);
	SELECT format('%s entries: %s live (%s placed right, %s stale), %s of rows gone; '
				  '%s naming another value; %s rows in TOAST, %s with a live entry; '
				  '%s places in refine items, %s as their rows'' entries',
		(SELECT count(*) FROM tt_e),
		(SELECT count(*) FROM tt_e WHERE valueid IN (SELECT valueid FROM tt_v)),
		(SELECT count(*) FROM tt_e WHERE valueid IN (SELECT valueid FROM tt_v)
										 AND runs = tt_places(toast, valueid)),
		(SELECT count(*) FROM tt_e WHERE valueid IN (SELECT valueid FROM tt_v)
										 AND runs IS DISTINCT FROM tt_places(toast, valueid)),
		(SELECT count(*) FROM tt_e WHERE valueid NOT IN (SELECT valueid FROM tt_v
														  WHERE valueid IS NOT NULL)),
		(SELECT count(*) FROM tt_e e JOIN tt_r r USING (row_tid)
		  WHERE r.normal AND r.valueid IS DISTINCT FROM e.valueid),
		(SELECT count(*) FROM tt_v
		  WHERE ext AND plain AND size = 4 + dims * CASE WHEN half THEN 2 ELSE 4 END),
		(SELECT count(*) FROM tt_v
		  WHERE ext AND plain AND size = 4 + dims * CASE WHEN half THEN 2 ELSE 4 END
			AND valueid IN (SELECT valueid FROM tt_e)),
		(SELECT count(*) FROM tt_s),
		(SELECT count(*) FROM tt_s s JOIN tt_e e USING (row_tid)
		  WHERE s.valueid = e.valueid AND s.runs = e.runs))
	  INTO r;
	DROP TABLE tt_e, tt_r, tt_v, tt_s;
	RETURN r;
END $$;

-- how many entries take how many runs
CREATE FUNCTION tt_runs(idx regclass) RETURNS text LANGUAGE sql AS $$
	SELECT string_agg(format('%s in %s run%s', n, nruns, CASE WHEN nruns > 1 THEN 's' ELSE '' END),
					  ', ' ORDER BY nruns)
	  FROM (SELECT nruns, count(*) AS n FROM tt_locs(idx) GROUP BY nruns) s
$$;

/*
 * tt_cmp() runs q - which returns columns id and d, ORDER BY d - with
 * pg_lion.vector_toast_direct on and off, and compares the rows, their
 * order and their distances; and says how the plan read the vectors: the
 * exact path (LionOrdered, every core scan disabled) or a probing index
 * scan.
 */
CREATE FUNCTION tt_run(q text, direct boolean, OUT ids int[], OUT ds float8[])
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
CREATE FUNCTION tt_cmp(q text, exact boolean) RETURNS text
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
	SELECT * INTO a FROM tt_run(q, true);
	SELECT * INTO b FROM tt_run(q, false);
	RETURN format('%s, %s rows, %s', how, cardinality(a.ids),
				  CASE WHEN a.ids = b.ids AND a.ds IS NOT DISTINCT FROM b.ds
					   THEN 'the same' ELSE 'DIFFERENT' END);
END $$;

/*
 * tt_all() runs the queries of a table of n dims vectors, with an int
 * column cat: the ten and the hundred nearest, every row, and under a
 * filter on cat, which the exact path answers, the ten nearest and every
 * member - each against a few query vectors, by the index's operator.
 */
CREATE FUNCTION tt_all(tbl regclass, n int, op text DEFAULT '<->', cast_ text DEFAULT '')
RETURNS TABLE (query text, result text)
LANGUAGE plpgsql AS $$
DECLARE
	s int;
	q text;
BEGIN
	FOR s IN 1 .. 2 LOOP
		q := format('tt_vec(%s, %s)%s', -s, n, cast_);
		query := format('ten nearest to %s', s);
		result := tt_cmp(format('SELECT id, v %s %s AS d FROM %s ORDER BY v %s %s LIMIT 10',
								op, q, tbl, op, q), false);
		RETURN NEXT;
		query := format('every row, from %s', s);
		result := tt_cmp(format('SELECT id, v %s %s AS d FROM %s ORDER BY v %s %s',
								op, q, tbl, op, q), false);
		RETURN NEXT;
		query := format('cat = %s, ten nearest to %s', s + 2, s);
		result := tt_cmp(format('SELECT id, v %s %s AS d FROM %s WHERE cat = %s ORDER BY v %s %s LIMIT 10',
								op, q, tbl, s + 2, op, q), true);
		RETURN NEXT;
		query := format('cat in (%s, %s), every member, from %s', s, s + 5, s);
		result := tt_cmp(format('SELECT id, v %s %s AS d FROM %s WHERE cat IN (%s, %s) ORDER BY v %s %s',
								op, q, tbl, s, s + 5, op, q), true);
		RETURN NEXT;
	END LOOP;
	query := 'the hundred nearest';
	result := tt_cmp(format('SELECT id, v %s tt_vec(-3, %s)%s AS d FROM %s ORDER BY v %s tt_vec(-3, %s)%s LIMIT 100',
							op, n, cast_, tbl, op, n, cast_), false);
	RETURN NEXT;
END $$;

-- ---------------------------------------------------------------------
-- 1. A table of 1536-dimension vectors in TOAST (6,148 bytes: four chunks,
--    on one page each when loaded in order), some NULL: the build's
--    locator, an entry a row with its vector in TOAST, and the place in
--    every row's REFINE item (two runs' room)
-- ---------------------------------------------------------------------
CREATE TABLE tt (id int, cat int, note text, v vector(1536))
	WITH (autovacuum_enabled = off);
INSERT INTO tt SELECT i, i % 20, 'a', tt_vec(i, 1536) FROM generate_series(1, 600) i;
INSERT INTO tt SELECT i, i % 20, 'a', NULL FROM generate_series(601, 610) i;
CREATE INDEX tt_cat ON tt USING lion (cat);
CREATE INDEX tt_v ON tt USING lionvec (v) WITH (partitions = 4);
ANALYZE tt;
-- the setting, on by default (its library loaded by now)
SHOW pg_lion.vector_toast_direct;
SELECT tt_meta('tt_v');
SELECT tt_pages('tt_v', 6) AS loc_pages, tt_pages('tt_v', 7) AS locdir_pages;
SELECT tt_check('tt_v');
SELECT tt_runs('tt_v');
SELECT * FROM tt_all('tt', 1536);

-- What the scans read, with every page in shared buffers and nothing read
-- ahead: the exact path, its members' vectors by place, each returned row
-- checked; the probed path, each row it reranks
SET pg_lion.vector_prefetch = off;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SET enable_sort = off;
-- each query prepared, and run once, at the settings it is planned under,
-- so that its DEBUG1 runs parse and plan nothing: what a parse looks up
-- can grow a catalog cache, whose rehash DEBUG1 reports too
PREPARE tt_x AS SELECT count(*) FROM (SELECT id FROM tt WHERE cat = 3 ORDER BY v <-> tt_vec(-1, 1536) LIMIT 10) s;
EXECUTE tt_x;
RESET enable_indexscan;
PREPARE tt_p AS SELECT count(*) FROM (SELECT id FROM tt ORDER BY v <-> tt_vec(-1, 1536) LIMIT 10) s;
EXECUTE tt_p;
SET client_min_messages = debug1;
EXECUTE tt_x;
SET pg_lion.vector_toast_direct = off;
EXECUTE tt_x;
RESET pg_lion.vector_toast_direct;
EXECUTE tt_p;
SET pg_lion.vector_toast_direct = off;
EXECUTE tt_p;
RESET pg_lion.vector_toast_direct;
RESET client_min_messages;
DEALLOCATE tt_p;
RESET pg_lion.vector_prefetch;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_sort;

-- every way of reading ahead reads the same rows
SET pg_lion.vector_prefetch = fadvise;
SELECT * FROM tt_all('tt', 1536);
SET pg_lion.vector_prefetch = off;
SELECT * FROM tt_all('tt', 1536);
RESET pg_lion.vector_prefetch;

-- ---------------------------------------------------------------------
-- 2. What rows and their TOAST relation go through, each followed by the
--    queries, direct reads on and off
-- ---------------------------------------------------------------------

-- an UPDATE of another column, HOT (the vector's pointer kept, and its
-- row's root TID still the index's)
SELECT pg_stat_get_tuples_hot_updated('tt'::regclass) AS hot_before \gset
UPDATE tt SET note = 'b' WHERE id % 7 = 0;
SELECT pg_stat_force_next_flush();
SELECT pg_stat_get_tuples_hot_updated('tt'::regclass) - :hot_before > 0 AS "HOT";
SELECT tt_check('tt_v');
SELECT * FROM tt_all('tt', 1536);

-- ... and not HOT: an indexed column changed, a new TID for the same
-- vector, whose pointer the new tuple shares with the old one
UPDATE tt SET cat = cat + 20 WHERE id % 11 = 0;
SELECT tt_check('tt_v');
SELECT * FROM tt_all('tt', 1536);

-- the vector set to itself: the same pointer, HOT
SELECT pg_stat_get_tuples_hot_updated('tt'::regclass) AS hot_before \gset
UPDATE tt SET v = v WHERE id % 13 = 0;
SELECT pg_stat_force_next_flush();
SELECT pg_stat_get_tuples_hot_updated('tt'::regclass) - :hot_before > 0 AS "HOT";
SELECT tt_check('tt_v');
SELECT * FROM tt_all('tt', 1536);

-- the vector changed: a new value at a new TID, the old one's chunks
-- deleted
UPDATE tt SET v = tt_vec(id + 5000, 1536) WHERE id % 17 = 0;
SELECT tt_check('tt_v');
SELECT * FROM tt_all('tt', 1536);

-- deleted, and inserted again (the same ids, other vectors)
DELETE FROM tt WHERE id % 19 = 0;
INSERT INTO tt SELECT i, i % 20, 'c', tt_vec(i + 7000, 1536) FROM generate_series(19, 600, 19) i;
SELECT tt_check('tt_v');
SELECT * FROM tt_all('tt', 1536);

-- an insert rolled back: its rows, nearest to the queries, are in the
-- index and the locator, their chunks still there; read by place, each is
-- passed over by the check before it would be returned
BEGIN;
INSERT INTO tt SELECT 9000 + i, 3, 'd', tt_vec(-1, 1536) + tt_vec(9000 + i, 1536, 0.01)
  FROM generate_series(1, 20) i;
ROLLBACK;
SET pg_lion.vector_prefetch = off;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SET enable_sort = off;
EXECUTE tt_x;
SET client_min_messages = debug1;
EXECUTE tt_x;
RESET client_min_messages;
RESET enable_indexscan;

-- ... and once its chunks' inserter is known to have aborted (hinted, by
-- a scan of the TOAST relation), they are no longer read: each falls back
-- to its heap row, which is not there
SELECT reltoastrelid::regclass AS tt_toast FROM pg_class WHERE oid = 'tt'::regclass \gset
SELECT count(*) > 0 AS "chunks" FROM :tt_toast;
SET enable_indexscan = off;
EXECUTE tt_x;
SET client_min_messages = debug1;
EXECUTE tt_x;
RESET client_min_messages;
DEALLOCATE tt_x;
RESET pg_lion.vector_prefetch;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
RESET enable_sort;
SELECT tt_check('tt_v');
SELECT * FROM tt_all('tt', 1536);

-- an INSERT ... SELECT from the table itself: the heap stores each vector
-- anew, under a new value id, and the index takes the pointer the heap
-- stored, not the one it was handed
INSERT INTO tt SELECT id + 10000, cat, 'e', v FROM tt WHERE id <= 60 AND v IS NOT NULL;
SELECT tt_check('tt_v');
SELECT * FROM tt_all('tt', 1536);

-- the TOAST relation vacuumed on its own: the chunks of the rows deleted
-- and updated away removed, their line pointers given to the next chunks
-- stored there, while the heap keeps those rows and the index their
-- entries; then rows inserted into the space
VACUUM (PROCESS_MAIN false) tt;
INSERT INTO tt SELECT 20000 + i, i % 20, 'f', tt_vec(20000 + i, 1536) FROM generate_series(1, 60) i;
SELECT tt_check('tt_v');
SELECT * FROM tt_all('tt', 1536);

-- the TOAST relation rewritten on its own: its values keep their ids, and
-- every place may be stale; each read by place either finds its value's
-- chunks or falls back, and a scan whose first 64 mostly fail stops trying
DELETE FROM tt WHERE id % 23 = 0;
VACUUM FULL :tt_toast;
SELECT tt_check('tt_v');
SELECT * FROM tt_all('tt', 1536);
SET pg_lion.vector_prefetch = off;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SET enable_sort = off;
PREPARE tt_c AS SELECT count(*) FROM (SELECT id FROM tt WHERE cat IN (1, 2, 3, 4, 5) ORDER BY v <-> tt_vec(-1, 1536)) s;
EXECUTE tt_c;
SET client_min_messages = debug1;
EXECUTE tt_c;
RESET client_min_messages;
DEALLOCATE tt_c;
RESET pg_lion.vector_prefetch;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
RESET enable_sort;

-- VACUUM of the table: the entries of the rows gone go with their CODE
-- items; REINDEX makes every place right again
VACUUM tt;
SELECT tt_check('tt_v');
SELECT * FROM tt_all('tt', 1536);
REINDEX INDEX tt_v;
SELECT tt_check('tt_v');
SELECT * FROM tt_all('tt', 1536);

-- direct reads off: the same plans, every row from the heap
SET pg_lion.vector_toast_direct = off;
SELECT tt_cmp('SELECT id, v <-> tt_vec(-1, 1536) AS d FROM tt WHERE cat = 3 ORDER BY v <-> tt_vec(-1, 1536)', true);
RESET pg_lion.vector_toast_direct;
DROP TABLE tt;

-- ---------------------------------------------------------------------
-- 3. Other widths, and where their chunks lie: 768 dimensions (two
--    chunks, a page), 1024 (three, two values in three on one page),
--    3072 (seven, across two pages or three, a REFINE item's place three
--    runs) and a halfvec(1536) (two)
-- ---------------------------------------------------------------------
CREATE TABLE tt768 (id int, cat int, v vector(768)) WITH (autovacuum_enabled = off);
INSERT INTO tt768 SELECT i, i % 20, tt_vec(i, 768) FROM generate_series(1, 300) i;
CREATE INDEX tt768_cat ON tt768 USING lion (cat);
CREATE INDEX tt768_v ON tt768 USING lionvec (v vector_ip_ops) WITH (partitions = 4);
ANALYZE tt768;
SELECT tt_meta('tt768_v');
SELECT tt_check('tt768_v');
SELECT tt_runs('tt768_v');
SELECT * FROM tt_all('tt768', 768, '<#>');
DROP TABLE tt768;

CREATE TABLE tt1024 (id int, cat int, v vector(1024)) WITH (autovacuum_enabled = off);
INSERT INTO tt1024 SELECT i, i % 20, tt_vec(i, 1024) FROM generate_series(1, 300) i;
CREATE INDEX tt1024_cat ON tt1024 USING lion (cat);
CREATE INDEX tt1024_v ON tt1024 USING lionvec (v) WITH (partitions = 4);
ANALYZE tt1024;
SELECT tt_meta('tt1024_v');
SELECT tt_check('tt1024_v');
SELECT tt_runs('tt1024_v');
SELECT * FROM tt_all('tt1024', 1024);
DROP TABLE tt1024;

CREATE TABLE tt3072 (id int, cat int, v vector(3072)) WITH (autovacuum_enabled = off);
INSERT INTO tt3072 SELECT i, i % 20, tt_vec(i, 3072) FROM generate_series(1, 200) i;
CREATE INDEX tt3072_cat ON tt3072 USING lion (cat);
CREATE INDEX tt3072_v ON tt3072 USING lionvec (v) WITH (partitions = 4);
ANALYZE tt3072;
SELECT tt_meta('tt3072_v');
SELECT tt_check('tt3072_v');
SELECT tt_runs('tt3072_v');
SELECT * FROM tt_all('tt3072', 3072);
DROP TABLE tt3072;

CREATE TABLE tth (id int, cat int, v halfvec(1536)) WITH (autovacuum_enabled = off);
INSERT INTO tth SELECT i, i % 20, tt_vec(i, 1536)::halfvec FROM generate_series(1, 300) i;
CREATE INDEX tth_cat ON tth USING lion (cat);
CREATE INDEX tth_v ON tth USING lionvec (v halfvec_l2_ops) WITH (partitions = 4);
ANALYZE tth;
SELECT tt_meta('tth_v');
SELECT tt_check('tth_v');
SELECT tt_runs('tth_v');
SELECT * FROM tt_all('tth', 1536, '<->', '::halfvec');
DROP TABLE tth;

-- ---------------------------------------------------------------------
-- 4. Rows of every kind in one table, under cosine: NULLs, zero vectors
--    (which pglz compresses where the storage is EXTENDED, and which the
--    zero walk returns), vectors compressed, vectors inline (stored while
--    the column was PLAIN), vectors in TOAST - only those have an entry
-- ---------------------------------------------------------------------
CREATE TABLE ttm (id int, cat int, v vector(1536)) WITH (autovacuum_enabled = off);
INSERT INTO ttm SELECT i, i % 20, tt_vec(i, 1536) FROM generate_series(1, 200) i;
INSERT INTO ttm SELECT i, i % 20, NULL FROM generate_series(201, 210) i;
INSERT INTO ttm SELECT i, i % 20, array_fill(0, ARRAY[1536])::real[]::vector
  FROM generate_series(211, 220) i;
ALTER TABLE ttm ALTER COLUMN v SET STORAGE EXTENDED;
INSERT INTO ttm SELECT i, i % 20, array_fill(0, ARRAY[1536])::real[]::vector
  FROM generate_series(221, 230) i;
INSERT INTO ttm SELECT i, i % 20, (tt_vec(i, 768)::real[] || tt_vec(i, 768)::real[])::vector
  FROM generate_series(231, 240) i;
ALTER TABLE ttm ALTER COLUMN v SET STORAGE PLAIN;
INSERT INTO ttm SELECT i, i % 20, tt_vec(i, 1536) FROM generate_series(241, 250) i;
ALTER TABLE ttm ALTER COLUMN v SET STORAGE EXTERNAL;
INSERT INTO ttm SELECT i, i % 20, tt_vec(i, 1536) FROM generate_series(251, 300) i;
SELECT pg_column_compression(v) IS NOT NULL AS compressed,
	   pg_column_size(v) < 6000 AS small, count(*)
  FROM ttm WHERE v IS NOT NULL GROUP BY 1, 2 ORDER BY 1, 2;
CREATE INDEX ttm_cat ON ttm USING lion (cat);
CREATE INDEX ttm_v ON ttm USING lionvec (v vector_cosine_ops) WITH (partitions = 4);
ANALYZE ttm;
SELECT tt_meta('ttm_v');
SELECT tt_check('ttm_v');
SELECT * FROM tt_all('ttm', 1536, '<=>');

-- and an expression index, whose values no TOAST relation holds: no
-- locator, and the same rows either way
CREATE INDEX ttm_e ON ttm USING lionvec ((v + v)) WITH (partitions = 4);
SELECT tt_meta('ttm_e');
SELECT tt_pages('ttm_e', 6) AS loc_pages;
SELECT tt_cmp('SELECT id, (v + v) <-> tt_vec(-1, 1536) AS d FROM ttm ORDER BY (v + v) <-> tt_vec(-1, 1536) LIMIT 10', false);
DROP TABLE ttm;

-- ---------------------------------------------------------------------
-- 5. A parallel build's locator is a serial build's, entry for entry and
--    page for page, and neither holds the rows deleted before it
-- ---------------------------------------------------------------------
CREATE TABLE ttp (id int, cat int, v vector(1536))
	WITH (autovacuum_enabled = off, parallel_workers = 2);
INSERT INTO ttp SELECT i, i % 20, tt_vec(i, 1536) FROM generate_series(1, 400) i;
DELETE FROM ttp WHERE id % 9 = 0;
CREATE INDEX ttp_v ON ttp USING lionvec (v) WITH (partitions = 4);
CREATE TABLE ttp_serial AS
	SELECT blk - min(blk) OVER () AS page, row_tid, valueid, runs FROM tt_locs('ttp_v');
-- (built in parallel, as vector_parallel.sql's DEBUG1 lines show such a
-- build goes; core's own line of it differs on 16)
SET max_parallel_maintenance_workers = 2;
REINDEX INDEX ttp_v;
RESET max_parallel_maintenance_workers;
SELECT (SELECT count(*) FROM ttp_serial) AS entries,
	   (SELECT array_agg(s::text ORDER BY row_tid) FROM ttp_serial s) =
	   (SELECT array_agg(p::text ORDER BY row_tid)
		  FROM (SELECT blk - min(blk) OVER () AS page, row_tid, valueid, runs
				  FROM tt_locs('ttp_v')) p) AS "the same";
SELECT tt_check('ttp_v');
DROP TABLE ttp, ttp_serial;

-- ---------------------------------------------------------------------
-- 6. Indexes that keep no locator: vectors short enough to stay in their
--    rows, or a table that keeps them there (toast_tuple_target)
-- ---------------------------------------------------------------------
CREATE TABLE tti (id int, v vector(16)) WITH (autovacuum_enabled = off);
INSERT INTO tti SELECT i, tt_vec(i, 16) FROM generate_series(1, 200) i;
CREATE INDEX tti_v ON tti USING lionvec (v) WITH (partitions = 4);
SELECT tt_meta('tti_v');
SELECT tt_pages('tti_v', 6) AS loc_pages, tt_pages('tti_v', 7) AS locdir_pages;
DROP TABLE tti;
CREATE TABLE ttt (id int, v vector(1536)) WITH (autovacuum_enabled = off, toast_tuple_target = 8160);
INSERT INTO ttt SELECT i, tt_vec(i, 1536) FROM generate_series(1, 50) i;
CREATE INDEX ttt_v ON ttt USING lionvec (v) WITH (partitions = 2);
SELECT tt_meta('ttt_v');
SELECT tt_pages('ttt_v', 6) AS loc_pages, tt_pages('ttt_v', 7) AS locdir_pages;
DROP TABLE ttt;

-- an unlogged table's index: its init fork keeps the locator's first pages
CREATE UNLOGGED TABLE ttu (id int, v vector(1536));
CREATE INDEX ttu_v ON ttu USING lionvec (v) WITH (partitions = 2);
SELECT tt_meta('ttu_v', 'init');
SELECT tt_pages('ttu_v', 6, 'init') AS loc_pages, tt_pages('ttu_v', 7, 'init') AS locdir_pages;
INSERT INTO ttu SELECT i, tt_vec(i, 1536) FROM generate_series(1, 50) i;
SELECT tt_check('ttu_v');
SELECT tt_cmp('SELECT id, v <-> tt_vec(-1, 1536) AS d FROM ttu ORDER BY v <-> tt_vec(-1, 1536) LIMIT 10', false);
DROP TABLE ttu;

DROP FUNCTION tt_vec(bigint, int, float8), tt_u16(bytea, int), tt_u32(bytea, int),
	tt_meta(regclass, text), tt_pages(regclass, int, text), tt_locs(regclass),
	tt_slots(regclass), tt_raw(regclass, int), tt_visible(regclass),
	tt_places(regclass, oid), tt_check(regclass), tt_runs(regclass),
	tt_run(text, boolean), tt_cmp(text, boolean), tt_all(regclass, int, text, text);
