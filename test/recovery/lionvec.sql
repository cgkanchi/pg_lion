-- Phase 5's fixture (run.sh): a lionvec index over vectors in TOAST, and
-- the checks of its locator (DESIGN.md §44.13) after a crash and on a hot
-- standby.  Needs pgvector and pageinspect; nothing here writes, so that
-- every check also runs on a standby.
SET synchronous_commit = on;
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_lion_vector;
CREATE EXTENSION IF NOT EXISTS pageinspect;
RESET client_min_messages;

DROP TABLE IF EXISTS lv_rec;
DROP SEQUENCE IF EXISTS lv_rec_seq;

-- a pseudo-random vector of 768 coordinates: 3,076 bytes, two TOAST chunks
CREATE FUNCTION lv_rec_vec(seed bigint) RETURNS vector
LANGUAGE sql IMMUTABLE AS $$
	SELECT array_agg(((hashint8extended(seed * 1000 + j, 42) % 100000)::float8
					  / 100000.0)::real ORDER BY j)::vector
	  FROM generate_series(1, 768) j
$$;

-- fillfactor leaves room for the writer's HOT updates
CREATE TABLE lv_rec (id bigint, cat int, note text, v vector(768))
	WITH (autovacuum_enabled = off, fillfactor = 70);
CREATE SEQUENCE lv_rec_seq START 3001;
INSERT INTO lv_rec SELECT i, i % 10, 'a', lv_rec_vec(i) FROM generate_series(1, 3000) i;
INSERT INTO lv_rec SELECT i, -i % 10, 'a', NULL FROM generate_series(-20, -1) i;
CREATE INDEX lv_rec_id ON lv_rec (id);
CREATE INDEX lv_rec_cat ON lv_rec USING lion (cat);
CREATE INDEX lv_rec_v ON lv_rec USING lionvec (v) WITH (partitions = 8);
ANALYZE lv_rec;

CREATE FUNCTION lv_u16(p bytea, o int) RETURNS int LANGUAGE sql IMMUTABLE AS
$$ SELECT get_byte(p, o) | (get_byte(p, o + 1) << 8) $$;
CREATE FUNCTION lv_u32(p bytea, o int) RETURNS bigint LANGUAGE sql IMMUTABLE AS
$$ SELECT lv_u16(p, o)::bigint | (lv_u16(p, o + 2)::bigint << 16) $$;

/*
 * The LOC entries of an index: the row's TID, its value's id and its runs as
 * text; and the places in its REFINE items, after their 8-bit codes (the
 * meta page's padded dims, at 40).  The formats are test/sql/vector_toast.sql's
 * tt_locs() and tt_slots().
 */
CREATE FUNCTION lv_locs(idx regclass)
RETURNS TABLE (row_tid tid, valueid oid, runs text)
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
		CONTINUE WHEN lv_u16(p, bs - 4) <> 6 OR lv_u16(p, bs - 2) <> 65420;
		FOR i IN 1 .. (lv_u16(p, 12) - 24) / 4 LOOP
			lp := lv_u32(p, 20 + 4 * i);
			CONTINUE WHEN (lp >> 15) & 3 <> 1;
			o := (lp & 32767)::int;
			row_tid := format('(%s,%s)', (lv_u16(p, o)::bigint << 16) | lv_u16(p, o + 2),
							  lv_u16(p, o + 4))::tid;
			valueid := lv_u32(p, o + 8)::oid;
			runs := '';
			FOR k IN 0 .. get_byte(p, o + 6) - 1 LOOP
				runs := runs || CASE WHEN k > 0 THEN ' ' ELSE '' END ||
					format('(%s,%s)x%s', lv_u32(p, o + 12 + 8 * k),
						   lv_u16(p, o + 16 + 8 * k), lv_u16(p, o + 18 + 8 * k));
			END LOOP;
			RETURN NEXT;
		END LOOP;
	END LOOP;
END $$;
CREATE FUNCTION lv_slots(idx regclass)
RETURNS TABLE (blk bigint, off int, row_tid tid, valueid oid, runs text)
LANGUAGE plpgsql AS $$
DECLARE
	bs int := current_setting('block_size')::int;
	dimsp int := lv_u32(get_raw_page(idx::text, 0), 40)::int;
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
		CONTINUE WHEN lv_u16(p, bs - 4) <> 3 OR lv_u16(p, bs - 2) <> 65420;
		FOR i IN 1 .. (lv_u16(p, 12) - 24) / 4 LOOP
			lp := lv_u32(p, 20 + 4 * i);
			CONTINUE WHEN (lp >> 15) & 3 <> 1;
			o := (lp & 32767)::int;
			n := lv_u16(p, o + 6);
			CONTINUE WHEN n = 0;
			blk := b;
			off := i;
			row_tid := format('(%s,%s)', (lv_u16(p, o)::bigint << 16) | lv_u16(p, o + 2),
							  lv_u16(p, o + 4))::tid;
			o := o + 16 + dimsp;
			valueid := lv_u32(p, o)::oid;
			runs := '';
			FOR k IN 0 .. n - 1 LOOP
				runs := runs || CASE WHEN k > 0 THEN ' ' ELSE '' END ||
					format('(%s,%s)x%s', lv_u32(p, o + 4 + 8 * k),
						   lv_u16(p, o + 8 + 8 * k), lv_u16(p, o + 10 + 8 * k));
			END LOOP;
			RETURN NEXT;
		END LOOP;
	END LOOP;
END $$;

/*
 * The REFINE items the CODE items point at (rblk at 20, roff at 24): those a
 * scan can reach.  A crash between VACUUM's removal of a row's CODE items
 * and of its REFINE item leaves the REFINE item where none points at it,
 * for good (lionvec_vacuum_refines()), and its TID may name a new row later.
 */
CREATE FUNCTION lv_code_refs(idx regclass)
RETURNS TABLE (blk bigint, off int)
LANGUAGE plpgsql AS $$
DECLARE
	bs int := current_setting('block_size')::int;
	p bytea;
	b int;
	i int;
	lp bigint;
	o int;
BEGIN
	FOR b IN 1 .. pg_relation_size(idx) / bs - 1 LOOP
		p := get_raw_page(idx::text, b);
		CONTINUE WHEN lv_u16(p, bs - 4) <> 2 OR lv_u16(p, bs - 2) <> 65420;
		FOR i IN 1 .. (lv_u16(p, 12) - 24) / 4 LOOP
			lp := lv_u32(p, 20 + 4 * i);
			CONTINUE WHEN (lp >> 15) & 3 <> 1;
			o := (lp & 32767)::int;
			blk := lv_u32(p, o + 20);
			off := lv_u16(p, o + 24);
			RETURN NEXT;
		END LOOP;
	END LOOP;
END $$;

/*
 * Every tuple of the heap, whatever its visibility, with its id and its
 * vector's TOAST pointer: whether it is one (a one-byte header, tag 18),
 * uncompressed, and its value's id; and the root of its HOT chain, the TID
 * the indexes name it by - its own but for a heap-only tuple's, found from
 * the page's redirects and the chains' t_ctid links (HEAP_HOT_UPDATED
 * 0x4000, HEAP_ONLY_TUPLE 0x8000 in t_infomask2).
 */
CREATE FUNCTION lv_raw()
RETURNS TABLE (row_tid tid, root_tid tid, id bigint, normal boolean, ext boolean, valueid oid)
LANGUAGE plpgsql AS $$
DECLARE
	bs int := current_setting('block_size')::int;
	b int;
	r record;
	a bytea;
	root int[];
	nxt int[];
	flags int[];
	honly boolean[];
	cur int;
	steps int;
	i int;
BEGIN
	FOR b IN 0 .. pg_relation_size('lv_rec') / bs - 1 LOOP
		root := array_fill(0, ARRAY[400]);
		nxt := array_fill(0, ARRAY[400]);
		flags := array_fill(0, ARRAY[400]);
		honly := array_fill(false, ARRAY[400]);
		FOR r IN SELECT * FROM heap_page_items(get_raw_page('lv_rec', b)) LOOP
			flags[r.lp] := r.lp_flags;
			IF r.lp_flags = 2 THEN
				nxt[r.lp] := r.lp_off;
			ELSIF r.lp_flags = 1 THEN
				honly[r.lp] := (r.t_infomask2 & 32768) <> 0;
				IF (r.t_infomask2 & 16384) <> 0 AND (r.t_ctid::text::point)[0] = b THEN
					nxt[r.lp] := (r.t_ctid::text::point)[1]::int;
				END IF;
			END IF;
		END LOOP;
		FOR i IN 1 .. 399 LOOP
			CONTINUE WHEN NOT ((flags[i] = 1 AND NOT honly[i]) OR flags[i] = 2);
			cur := i;
			steps := 0;
			LOOP
				root[cur] := i;
				EXIT WHEN nxt[cur] = 0 OR steps > 400;
				cur := nxt[cur];
				EXIT WHEN cur < 1 OR cur > 399 OR flags[cur] <> 1 OR NOT honly[cur];
				steps := steps + 1;
			END LOOP;
		END LOOP;
		FOR r IN SELECT * FROM heap_page_item_attrs(get_raw_page('lv_rec', b), 'lv_rec'::regclass) LOOP
			row_tid := format('(%s,%s)', b, r.lp)::tid;
			root_tid := CASE WHEN root[r.lp] > 0 THEN format('(%s,%s)', b, root[r.lp])::tid END;
			normal := r.lp_flags = 1;
			id := NULL;
			ext := false;
			valueid := NULL;
			IF normal THEN
				id := lv_u32(r.t_attrs[1], 0) | (lv_u32(r.t_attrs[1], 4) << 32);
				a := r.t_attrs[4];
				IF a IS NOT NULL AND length(a) = 18 AND get_byte(a, 0) = 1 AND get_byte(a, 1) = 18
				   AND (lv_u32(a, 6) & 1073741823) = lv_u32(a, 2) - 4 THEN
					ext := true;
					valueid := lv_u32(a, 10)::oid;
				END IF;
			END IF;
			RETURN NEXT;
		END LOOP;
	END LOOP;
END $$;

-- where the TOAST relation holds a value's chunks, as runs
CREATE FUNCTION lv_places(v oid) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	r record;
	runs text := '';
	cb bigint := -1;
	co int := -1;
	cn int := 0;
BEGIN
	FOR r IN EXECUTE format('SELECT (ctid::text::point)[0]::bigint AS b, (ctid::text::point)[1]::int AS o
							   FROM %s WHERE chunk_id = $1 ORDER BY chunk_seq',
							(SELECT reltoastrelid::regclass FROM pg_class WHERE oid = 'lv_rec'::regclass))
			 USING v LOOP
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
 * q - columns id and d, ORDER BY d - with direct reads on or off, under the
 * plan exact asks for: the exact path (LionOrdered, no other scan) or a
 * probing index scan; or a sort, no index at all.
 */
CREATE FUNCTION lv_run(q text, how text, direct boolean, OUT ids bigint[], OUT ds float8[])
LANGUAGE plpgsql AS $$
DECLARE
	r record;
BEGIN
	PERFORM set_config('pg_lion.vector_toast_direct', direct::text, true);
	PERFORM set_config('pg_lion.vector_epsilon', '6', true);
	PERFORM set_config('pg_lion.vector_margin', '100', true);
	PERFORM set_config('pg_lion.vector_patience', '0', true);
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	PERFORM set_config('enable_seqscan', (how = 'sort')::text, true);
	PERFORM set_config('enable_sort', (how = 'sort')::text, true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', (how = 'probed')::text, true);
	PERFORM set_config('pg_lion.enable_ordered_scan', (how = 'exact')::text, true);
	-- the sort's filtered queries are exact; a lionvec scan they would filter
	-- is otherwise an error (pg_lion.vector_postfilter)
	PERFORM set_config('pg_lion.vector_postfilter',
					   CASE how WHEN 'sort' THEN 'exact' ELSE 'error' END, true);
	ids := '{}';
	ds := '{}';
	FOR r IN EXECUTE q LOOP
		ids := ids || r.id;
		ds := ds || r.d::float8;
	END LOOP;
END $$;

/*
 * The checks, (ok, what) rows for run.sh's run_check.  Of the locator: no
 * entry names another value than the tuple at its TID stores; every row the
 * snapshot sees whose vector is in TOAST has an entry of its value; where
 * placed (strict: always, but after a rewrite of the TOAST relation), an
 * entry of a row the snapshot sees names its chunks' places; a REFINE place
 * a CODE item points at whose row has an entry is that entry's, and after a
 * VACUUM (vacuumed) every one has one.  Of the scans: queries by the exact path and probing
 * return the same rows, order and distances with direct reads on and off,
 * and the probing ones a sort's.
 */
CREATE FUNCTION lv_rec_check(strict boolean, vacuumed boolean)
RETURNS TABLE (ok boolean, what text)
LANGUAGE plpgsql AS $$
DECLARE
	n_e bigint;
	n_other bigint;
	n_rows bigint;
	n_rows_e bigint;
	n_live bigint;
	n_right bigint;
	n_s bigint;
	n_s_e bigint;
	n_s_ok bigint;
	q text;
	a record;
	b record;
	c record;
	s int;
BEGIN
	WITH e AS MATERIALIZED (SELECT * FROM lv_locs('lv_rec_v')),
		 r AS MATERIALIZED (SELECT * FROM lv_raw()),
		 -- the tuples the snapshot sees, each with its chain's root
		 v AS MATERIALIZED (SELECT r.* FROM r WHERE r.row_tid IN (SELECT ctid FROM lv_rec)),
		 sl AS MATERIALIZED (SELECT * FROM lv_slots('lv_rec_v')
							  WHERE (blk, off) IN (SELECT blk, off FROM lv_code_refs('lv_rec_v')))
	SELECT (SELECT count(*) FROM e),
		   (SELECT count(*) FROM e JOIN r USING (row_tid)
			 WHERE r.normal AND r.valueid IS DISTINCT FROM e.valueid),
		   (SELECT count(*) FROM v WHERE v.ext),
		   (SELECT count(*) FROM v WHERE v.ext AND EXISTS
				(SELECT 1 FROM e WHERE e.row_tid = v.root_tid AND e.valueid = v.valueid)),
		   (SELECT count(*) FROM e WHERE e.row_tid IN (SELECT root_tid FROM v)),
		   (SELECT count(*) FROM e WHERE e.row_tid IN (SELECT root_tid FROM v)
										 AND e.runs = lv_places(e.valueid)),
		   (SELECT count(*) FROM sl),
		   (SELECT count(*) FROM sl JOIN e USING (row_tid)),
		   (SELECT count(*) FROM sl JOIN e USING (row_tid)
			 WHERE sl.valueid = e.valueid AND sl.runs = e.runs)
	  INTO n_e, n_other, n_rows, n_rows_e, n_live, n_right, n_s, n_s_e, n_s_ok;

	ok := n_e > 0;
	what := format('the locator holds %s entries', n_e);
	RETURN NEXT;
	ok := n_other = 0;
	what := format('%s entries name another value than their TID''s tuple stores', n_other);
	RETURN NEXT;
	ok := n_rows > 0 AND n_rows_e = n_rows;
	what := format('%s of the %s rows in TOAST the snapshot sees have their entry', n_rows_e, n_rows);
	RETURN NEXT;
	IF strict THEN
		ok := n_right = n_live;
		what := format('%s of the %s entries of rows the snapshot sees are placed right', n_right, n_live);
		RETURN NEXT;
	END IF;
	ok := n_s_ok = n_s_e;
	what := format('%s of the %s REFINE places whose row has an entry are that entry''s', n_s_ok, n_s_e);
	RETURN NEXT;
	IF vacuumed THEN
		ok := n_s = n_s_e;
		what := format('%s of the %s REFINE places have an entry, after a VACUUM', n_s_e, n_s);
		RETURN NEXT;
	END IF;

	FOR s IN 1 .. 3 LOOP
		q := format('SELECT id, v <-> lv_rec_vec(%s) AS d FROM lv_rec WHERE cat = %s ORDER BY v <-> lv_rec_vec(%s)',
					-100 - s, s, -100 - s);
		SELECT * INTO a FROM lv_run(q, 'exact', true);
		SELECT * INTO b FROM lv_run(q, 'exact', false);
		SELECT * INTO c FROM lv_run(q, 'sort', false);
		ok := a.ids = b.ids AND a.ds IS NOT DISTINCT FROM b.ds AND a.ds IS NOT DISTINCT FROM c.ds
			AND cardinality(a.ids) > 0;
		what := format('cat = %s, every member: the exact path, %s rows, the same on and off and a sort''s distances',
					   s, cardinality(a.ids));
		RETURN NEXT;
		q := format('SELECT id, v <-> lv_rec_vec(%s) AS d FROM lv_rec ORDER BY v <-> lv_rec_vec(%s) LIMIT 50',
					-200 - s, -200 - s);
		SELECT * INTO a FROM lv_run(q, 'probed', true);
		SELECT * INTO b FROM lv_run(q, 'probed', false);
		SELECT * INTO c FROM lv_run(q, 'sort', false);
		ok := a.ids = b.ids AND a.ds IS NOT DISTINCT FROM b.ds AND a.ds IS NOT DISTINCT FROM c.ds
			AND cardinality(a.ids) = 50;
		what := format('the 50 nearest to query %s: probed, the same on and off and a sort''s distances', s);
		RETURN NEXT;
	END LOOP;
END $$;

-- The row of the insert parked between its items and its entry: in the
-- heap, seen by no snapshot, and with no entry.
CREATE FUNCTION lv_rec_unlocated(rid bigint) RETURNS TABLE (ok boolean, what text)
LANGUAGE sql AS $$
	WITH r AS MATERIALIZED (SELECT * FROM lv_raw() WHERE id = rid)
	SELECT (SELECT count(*) FROM r WHERE r.ext) = 1,
		   'the parked insert''s tuple is in the heap after the crash'
	UNION ALL
	SELECT NOT EXISTS (SELECT 1 FROM lv_rec WHERE id = rid),
		   'no snapshot sees it'
	UNION ALL
	SELECT NOT EXISTS (SELECT 1 FROM lv_locs('lv_rec_v') e JOIN r USING (row_tid)),
		   'it has no locator entry'
	UNION ALL
	SELECT EXISTS (SELECT 1 FROM lv_slots('lv_rec_v') s JOIN r USING (row_tid)),
		   'its REFINE item, written before the entry, is in'
$$;
