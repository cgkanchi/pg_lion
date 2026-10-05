-- The window store (DESIGN.md §40): the values of an index's stored columns
-- - its scalar key columns under store_values, its INCLUDE columns always -
-- kept per 64-page window of the heap and addressed by TID.
--
-- lion_index_stored(index, ctid) reads one row's slots through the store's
-- own gather, so "the store gives back what the heap has" is checked row by
-- row against the table, after the build (serial and parallel), every kind
-- of insert, UPDATE, DELETE and VACUUM; lion_index_verify(.., true) checks
-- the structure and every visible row's stored values at each step, and
-- lion_index_stats() shows the pages each step made.  The order of the
-- sections:
--
--	1. the options and every refusal
--	2. every storable type, NULLs, serial and parallel builds
--	3. the insert path: a window's first page, codes widened at each width,
--	   DICT to RAW, pages appended and split at the end of a chain
--	4. a split in the middle of a page's range
--	5. ABSENT heap pages, at build and at insert
--	6. the map growing past its first leaf
--	7. VACUUM: dead slots cleared in place, pages rewritten
--	8. the windows past a truncated heap's end freed, and filled again
--	9. key-ordered windows (§41): the order column, the permutation, the
--	   append region, VACUUM's clearing and sorting, cleanup, REINDEX
\set VERBOSITY terse
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;

-- The rows of tbl whose stored values the store does not give back as the
-- heap has them; vals is the stored columns as a text[], in index column
-- order, as their output functions spell them.
CREATE FUNCTION lion_st_mismatch(idx regclass, tbl regclass, vals text)
RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE
	n bigint;
BEGIN
	EXECUTE format('SELECT count(*) FROM %s WHERE lion_index_stored(%L, ctid) IS DISTINCT FROM %s',
				   tbl, idx, vals) INTO n;
	RETURN n;
END $$;

-- The store's columns of lion_index_stats()
CREATE FUNCTION lion_st_stats(idx regclass)
RETURNS TABLE (attno int2, pages int8, map_pages int8, dict_pages int8,
			   raw_pages int8, absent int8, dict_bytes int8, slot_bytes int8)
LANGUAGE sql AS $$
	SELECT attno, store_pages, store_map_pages, store_dict_pages,
		   store_raw_pages, store_absent, store_dict_bytes, store_slot_bytes
	  FROM lion_index_stats(idx) ORDER BY attno $$;

-- ---------------------------------------------------------------------
-- 1. The options, and every refusal
-- ---------------------------------------------------------------------

CREATE TABLE lion_st_opt (k int, t text, a int[], tv tsvector);
INSERT INTO lion_st_opt VALUES (1, 'one', '{1}', 'a b'), (2, NULL, NULL, NULL);

CREATE INDEX ON lion_st_opt USING lion (k) WITH (store_values = maybe);
CREATE INDEX ON lion_st_opt USING lion (k) WITH (store_max_len = -1);
CREATE INDEX ON lion_st_opt USING lion (k) WITH (store_max_len = 2001);

-- a multi-key key column is left out, with a NOTICE; the others are stored
CREATE INDEX lion_st_opt_ka ON lion_st_opt USING lion (k, a)
	WITH (store_values = true);
SELECT attno, dict_pages IS NOT NULL AS stored FROM lion_st_stats('lion_st_opt_ka');
SELECT ctid, lion_index_stored('lion_st_opt_ka', ctid) FROM lion_st_opt ORDER BY k;

-- store_values with nothing it can store is an ERROR
CREATE INDEX ON lion_st_opt USING lion (a, tv) WITH (store_values = true);

-- store_max_len on an index that stores nothing: a NOTICE, and no store
CREATE INDEX lion_st_opt_none ON lion_st_opt USING lion (k)
	WITH (store_max_len = 8);
SELECT pages, map_pages, dict_pages FROM lion_st_stats('lion_st_opt_none');
SELECT lion_index_stored('lion_st_opt_none', '(0,1)');

-- INCLUDE columns are always stored, an array as one plain value
CREATE INDEX lion_st_opt_inc ON lion_st_opt USING lion (k) INCLUDE (a, t, tv);
SELECT ctid, lion_index_stored('lion_st_opt_inc', ctid) FROM lion_st_opt ORDER BY k;
SELECT lion_index_stored('lion_st_opt_inc', '(0,7)') AS no_such_row;
SELECT lion_index_stored('lion_st_opt_inc', '(0,0)');
SELECT lion_index_stored('lion_st_opt_inc', '(0,600)');

-- store_max_len caps a varlena value, at build and at insert
CREATE INDEX ON lion_st_opt USING lion (k) INCLUDE (t) WITH (store_max_len = 2);
CREATE INDEX lion_st_opt_cap ON lion_st_opt USING lion (k) INCLUDE (t)
	WITH (store_max_len = 3);
INSERT INTO lion_st_opt (k, t) VALUES (3, 'four');
INSERT INTO lion_st_opt (k, t) VALUES (3, 'two');
SELECT k, lion_index_stored('lion_st_opt_cap', ctid) FROM lion_st_opt ORDER BY k;
DROP INDEX lion_st_opt_cap;

-- a value of more than 2000 bytes, detoasted, is an ERROR, as a key is
INSERT INTO lion_st_opt (k, t) VALUES (4, repeat('x', 2100));
CREATE TABLE lion_st_long (k int, t text);
INSERT INTO lion_st_long VALUES (1, repeat('y', 1996)), (2, repeat('y', 1997));
CREATE INDEX ON lion_st_long USING lion (k) INCLUDE (t);
DELETE FROM lion_st_long WHERE k = 2;
CREATE INDEX lion_st_long_i ON lion_st_long USING lion (k) INCLUDE (t);
SELECT length((lion_index_stored('lion_st_long_i', ctid))[1]) FROM lion_st_long;
DROP TABLE lion_st_long;

-- which columns are stored is the build's: ALTER INDEX changes nothing until
-- a REINDEX
ALTER INDEX lion_st_opt_inc SET (store_values = true);
SELECT attno, dict_pages IS NOT NULL AS stored FROM lion_st_stats('lion_st_opt_inc');
REINDEX INDEX lion_st_opt_inc;
SELECT attno, dict_pages IS NOT NULL AS stored FROM lion_st_stats('lion_st_opt_inc');
SELECT k, lion_index_stored('lion_st_opt_inc', ctid) FROM lion_st_opt ORDER BY k;
SELECT lion_index_verify('lion_st_opt_inc', true);
DROP TABLE lion_st_opt;

-- ---------------------------------------------------------------------
-- 2. Every storable type, NULLs among them, DICT and RAW; serial and
--    parallel builds
-- ---------------------------------------------------------------------

CREATE TABLE lion_st_types (
	id int, k int, i2 int2, i4 int4, i8 int8, b bool, d date,
	ts timestamptz, u uuid, n numeric, t text, vc varchar(20), by bytea,
	ia int[]) WITH (parallel_workers = 2);
INSERT INTO lion_st_types
SELECT g, g % 13,
	   CASE WHEN g % 7 = 0 THEN NULL ELSE (g % 100)::int2 END,
	   CASE WHEN g % 11 = 0 THEN NULL ELSE g * 7 END,
	   CASE WHEN g % 13 = 0 THEN NULL ELSE g::int8 * 1000003 END,
	   CASE WHEN g % 5 = 0 THEN NULL ELSE g % 3 = 0 END,
	   CASE WHEN g % 17 = 0 THEN NULL ELSE date '2024-01-01' + g % 400 END,
	   CASE WHEN g % 19 = 0 THEN NULL
			ELSE timestamptz '2024-01-01 00:00:00+00' + g * interval '17 minutes' END,
	   CASE WHEN g % 23 = 0 THEN NULL ELSE md5(g::text)::uuid END,
	   CASE WHEN g % 29 = 0 THEN NULL ELSE g / 7.0 END,
	   CASE WHEN g % 31 = 0 THEN NULL ELSE 'text ' || (g % 50) END,
	   CASE WHEN g % 37 = 0 THEN NULL ELSE 'v' || g END,
	   CASE WHEN g % 41 = 0 THEN NULL ELSE decode(md5(g::text), 'hex') END,
	   CASE WHEN g % 43 = 0 THEN NULL WHEN g % 43 = 1 THEN '{}'
			ELSE ARRAY[g, NULL, g % 5] END
  FROM generate_series(1, 6000) g;

\set vals 'ARRAY[k::text, i4::text, t, i2::text, i8::text, CASE b WHEN true THEN ''t'' WHEN false THEN ''f'' END, d::text, ts::text, u::text, n::text, vc::text, by::text, ia::text]'

-- key columns k, i4 and t under store_values, and the rest INCLUDE
CREATE INDEX lion_st_types_s ON lion_st_types USING lion (k, i4, t)
	INCLUDE (i2, i8, b, d, ts, u, n, vc, by, ia) WITH (store_values = true);
-- the same with capped varlena columns, which can be RAW
CREATE INDEX lion_st_types_c ON lion_st_types USING lion (k, i4, t)
	INCLUDE (i2, i8, b, d, ts, u, n, vc, by, ia)
	WITH (store_values = true, store_max_len = 40);
-- and in parallel, where the leader builds the store in a scan of its own
SET max_parallel_maintenance_workers = 2;
CREATE INDEX lion_st_types_p ON lion_st_types USING lion (k, i4, t)
	INCLUDE (i2, i8, b, d, ts, u, n, vc, by, ia) WITH (store_values = true);
RESET max_parallel_maintenance_workers;

SELECT * FROM lion_st_stats('lion_st_types_s');
SELECT * FROM lion_st_stats('lion_st_types_c');
SELECT (SELECT array_agg(s ORDER BY s.attno) FROM lion_st_stats('lion_st_types_p') s) =
	   (SELECT array_agg(s ORDER BY s.attno) FROM lion_st_stats('lion_st_types_s') s)
	   AS parallel_same;
SELECT lion_st_mismatch('lion_st_types_s', 'lion_st_types', :'vals') AS serial,
	   lion_st_mismatch('lion_st_types_c', 'lion_st_types', :'vals') AS capped,
	   lion_st_mismatch('lion_st_types_p', 'lion_st_types', :'vals') AS parallel;
SELECT lion_index_stored('lion_st_types_s', ctid) FROM lion_st_types
 WHERE id IN (1, 2, 43, 44, 2639) ORDER BY id;
SELECT lion_index_verify('lion_st_types_s', true),
	   lion_index_verify('lion_st_types_c', true),
	   lion_index_verify('lion_st_types_p', true);

-- INSERT, UPDATE (never HOT: every column is in an index) and DELETE
INSERT INTO lion_st_types
SELECT g, g % 13, (g % 100)::int2, g * 7, NULL, g % 2 = 0, date '2025-06-01',
	   NULL, md5((g * 3)::text)::uuid, g, 'new ' || g, 'w' || g,
	   '\x00ff'::bytea, ARRAY[g]
  FROM generate_series(6001, 8000) g;
UPDATE lion_st_types SET t = 'updated ' || (id % 9), n = n * 2 WHERE id % 4 = 0;
UPDATE lion_st_types SET ia = NULL, by = NULL, b = NOT b WHERE id % 6 = 1;
DELETE FROM lion_st_types WHERE id % 10 = 3;
SELECT lion_st_mismatch('lion_st_types_s', 'lion_st_types', :'vals') AS serial,
	   lion_st_mismatch('lion_st_types_c', 'lion_st_types', :'vals') AS capped,
	   lion_st_mismatch('lion_st_types_p', 'lion_st_types', :'vals') AS parallel;
SELECT lion_index_verify('lion_st_types_s', true),
	   lion_index_verify('lion_st_types_c', true),
	   lion_index_verify('lion_st_types_p', true);
VACUUM lion_st_types;
SELECT lion_st_mismatch('lion_st_types_s', 'lion_st_types', :'vals') AS serial,
	   lion_st_mismatch('lion_st_types_c', 'lion_st_types', :'vals') AS capped,
	   lion_st_mismatch('lion_st_types_p', 'lion_st_types', :'vals') AS parallel;
SELECT lion_index_verify('lion_st_types_s', true),
	   lion_index_verify('lion_st_types_c', true),
	   lion_index_verify('lion_st_types_p', true);
SELECT * FROM lion_st_stats('lion_st_types_c');
DROP TABLE lion_st_types;

-- ---------------------------------------------------------------------
-- 3. The insert path, on an index built empty: the window's first page,
--    a code widened at each width (the 2nd, 4th, 16th and 256th distinct
--    value), a page's dictionary turned RAW when it stops paying, and the
--    chain grown at its end - its last page's range extended, a page
--    appended, its last heap page split off.
-- ---------------------------------------------------------------------

CREATE TABLE lion_st_ins (v int8, w int8);
CREATE INDEX lion_st_ins_i ON lion_st_ins USING lion (v)
	WITH (store_values = true);
SELECT pages, map_pages, dict_pages FROM lion_st_stats('lion_st_ins_i');
INSERT INTO lion_st_ins VALUES (1, 1);
SELECT * FROM lion_st_stats('lion_st_ins_i');
INSERT INTO lion_st_ins VALUES (1, 1), (NULL, NULL);
SELECT * FROM lion_st_stats('lion_st_ins_i');
INSERT INTO lion_st_ins VALUES (2, 2);
SELECT * FROM lion_st_stats('lion_st_ins_i');
INSERT INTO lion_st_ins SELECT g, g FROM generate_series(3, 4) g;
SELECT * FROM lion_st_stats('lion_st_ins_i');
INSERT INTO lion_st_ins SELECT g, g FROM generate_series(5, 16) g;
SELECT * FROM lion_st_stats('lion_st_ins_i');
INSERT INTO lion_st_ins SELECT g, g FROM generate_series(17, 256) g;
SELECT * FROM lion_st_stats('lion_st_ins_i');
SELECT lion_st_mismatch('lion_st_ins_i', 'lion_st_ins', 'ARRAY[v::text]');
SELECT lion_index_verify('lion_st_ins_i', true);
-- unique values until DICT stops paying, a row at a time
DO $$ BEGIN
	FOR i IN 257 .. 1200 LOOP
		INSERT INTO lion_st_ins VALUES (i, i);
	END LOOP;
END $$;
SELECT * FROM lion_st_stats('lion_st_ins_i');
-- and on, to pages that fill and split their last heap page off
INSERT INTO lion_st_ins SELECT g, CASE WHEN g % 9 = 0 THEN NULL ELSE g END
  FROM generate_series(1201, 30000) g;
SELECT * FROM lion_st_stats('lion_st_ins_i');
SELECT lion_st_mismatch('lion_st_ins_i', 'lion_st_ins', 'ARRAY[v::text]');
SELECT lion_index_verify('lion_st_ins_i', true);
-- the same rows built at once
CREATE INDEX lion_st_ins_b ON lion_st_ins USING lion (v)
	WITH (store_values = true);
SELECT * FROM lion_st_stats('lion_st_ins_b');
SELECT lion_st_mismatch('lion_st_ins_b', 'lion_st_ins', 'ARRAY[v::text]');
-- and a second stored column, INCLUDE, written into the windows the first
-- already has
CREATE INDEX lion_st_ins_w ON lion_st_ins USING lion (v) INCLUDE (w);
INSERT INTO lion_st_ins SELECT g, -g FROM generate_series(30001, 31000) g;
SELECT lion_st_mismatch('lion_st_ins_w', 'lion_st_ins', 'ARRAY[w::text]');
SELECT lion_index_verify('lion_st_ins_w', true);
DROP TABLE lion_st_ins;

-- ---------------------------------------------------------------------
-- 4. A split in the middle of a page's range: VACUUM empties a heap page,
--    and rows half the size fill it again with twice the offsets, which a
--    full store page has no room for.
-- ---------------------------------------------------------------------

CREATE TABLE lion_st_mid (v int8, pad text);
INSERT INTO lion_st_mid SELECT g, repeat('p', 200) FROM generate_series(1, 2000) g;
CREATE INDEX lion_st_mid_i ON lion_st_mid USING lion (v) WITH (store_values = true);
SELECT * FROM lion_st_stats('lion_st_mid_i');
DELETE FROM lion_st_mid WHERE (ctid::text::point)[0] = 5;
VACUUM lion_st_mid;
INSERT INTO lion_st_mid SELECT g, NULL FROM generate_series(2001, 2200) g;
SELECT count(*) > 35 AS refilled FROM lion_st_mid WHERE (ctid::text::point)[0] = 5;
SELECT * FROM lion_st_stats('lion_st_mid_i');
SELECT lion_st_mismatch('lion_st_mid_i', 'lion_st_mid', 'ARRAY[v::text]');
SELECT lion_index_verify('lion_st_mid_i', true);
DROP TABLE lion_st_mid;

-- ---------------------------------------------------------------------
-- 5. ABSENT: a heap page whose values no store page can hold - dozens of
--    distinct strings of 1,984 bytes, which the heap compresses inline once
--    a wide row makes it try - is left to the heap, at build and at insert;
--    lion_index_stored() says NULL for its rows, and verify() takes it.
-- ---------------------------------------------------------------------

CREATE TABLE lion_st_abs (id int, s text, f text) WITH (toast_tuple_target = 128);
INSERT INTO lion_st_abs SELECT g, repeat(md5(g::text), 62), repeat('f', 100) FROM generate_series(1, 400) g;
INSERT INTO lion_st_abs SELECT g, 'short ' || g FROM generate_series(401, 900) g;
-- The long rows take several heap pages - how many depends on the heap's
-- page layout, which differs between majors - and those pages are the ABSENT
-- ones: the stats count them, and their rows, long and short alike, are the
-- rows the store has no value for.
CREATE FUNCTION lion_st_long_pages(tbl regclass) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	EXECUTE format('SELECT count(DISTINCT (ctid::text::point)[0]) FROM %s WHERE id <= 400', tbl) INTO n;
	RETURN n;
END $$;
SELECT lion_st_long_pages('lion_st_abs') > 3 AS several_long_pages;
CREATE INDEX lion_st_abs_b ON lion_st_abs USING lion (id) INCLUDE (s);
CREATE TABLE lion_st_abs2 (LIKE lion_st_abs) WITH (toast_tuple_target = 128);
CREATE INDEX lion_st_abs_i ON lion_st_abs2 USING lion (id) INCLUDE (s);
INSERT INTO lion_st_abs2 SELECT * FROM lion_st_abs ORDER BY id;
SELECT lion_st_long_pages('lion_st_abs') = lion_st_long_pages('lion_st_abs2') AS same_layout;
SELECT attno, pages > 0 AS some_pages, map_pages, dict_pages = pages AS all_dict,
	   raw_pages, absent = lion_st_long_pages('lion_st_abs') AS absent_long_pages,
	   dict_bytes > 0 AS some_dict, slot_bytes > 0 AS some_slots
  FROM lion_st_stats('lion_st_abs_b');
SELECT attno, pages > 0 AS some_pages, map_pages, dict_pages = pages AS all_dict,
	   raw_pages, absent = lion_st_long_pages('lion_st_abs2') AS absent_long_pages,
	   dict_bytes > 0 AS some_dict, slot_bytes > 0 AS some_slots
  FROM lion_st_stats('lion_st_abs_i');
SELECT count(*) AS rows,
	   count(*) FILTER (WHERE st IS NULL) = count(*) FILTER (WHERE longpage) AS absent_long_pages_rows,
	   count(*) FILTER (WHERE st IS NULL AND NOT longpage) AS absent_elsewhere,
	   count(*) FILTER (WHERE st IS NOT NULL) > 0 AS some_stored,
	   count(*) FILTER (WHERE st IS NOT NULL AND st IS DISTINCT FROM s) AS wrong
  FROM (SELECT s, (lion_index_stored('lion_st_abs_b', ctid))[1] AS st,
			   (ctid::text::point)[0] IN (SELECT (ctid::text::point)[0] FROM lion_st_abs WHERE id <= 400) AS longpage
		  FROM lion_st_abs) x;
SELECT count(*) AS rows,
	   count(*) FILTER (WHERE st IS NULL) = count(*) FILTER (WHERE longpage) AS absent_long_pages_rows,
	   count(*) FILTER (WHERE st IS NULL AND NOT longpage) AS absent_elsewhere,
	   count(*) FILTER (WHERE st IS NOT NULL) > 0 AS some_stored,
	   count(*) FILTER (WHERE st IS NOT NULL AND st IS DISTINCT FROM s) AS wrong
  FROM (SELECT s, (lion_index_stored('lion_st_abs_i', ctid))[1] AS st,
			   (ctid::text::point)[0] IN (SELECT (ctid::text::point)[0] FROM lion_st_abs2 WHERE id <= 400) AS longpage
		  FROM lion_st_abs2) x;
SELECT lion_index_verify('lion_st_abs_b', true), lion_index_verify('lion_st_abs_i', true);
DROP TABLE lion_st_abs, lion_st_abs2;
DROP FUNCTION lion_st_long_pages(regclass);

-- ---------------------------------------------------------------------
-- 6. The map past its first leaf: with 32 stored columns a leaf holds the
--    slots of 63.5 windows, and a row on heap page 4096 is in window 64.
--    The heap gets there with a row a page; the rows of every page but the
--    first are deleted before the build, which skips them, so the insert is
--    the one that adds the leaf.
-- ---------------------------------------------------------------------

DO $$ BEGIN
	EXECUTE 'CREATE TABLE lion_st_map (k int, ' ||
			(SELECT string_agg(format('c%s int', i), ', ') FROM generate_series(1, 31) i) ||
			', pad text) WITH (fillfactor = 10, autovacuum_enabled = off)';
END $$;
INSERT INTO lion_st_map (k, c1, c31, pad)
SELECT g, g, -g, repeat('m', 500) FROM generate_series(0, 4096) g;
SELECT max((ctid::text::point)[0]) AS last_block FROM lion_st_map;
DELETE FROM lion_st_map WHERE k > 0;
DO $$ BEGIN
	EXECUTE 'CREATE INDEX lion_st_map_i ON lion_st_map USING lion (k) INCLUDE (' ||
			(SELECT string_agg(format('c%s', i), ', ') FROM generate_series(1, 31) i) ||
			') WITH (store_values = true)';
END $$;
SELECT pages, map_pages FROM lion_st_stats('lion_st_map_i') WHERE attno = 1;
INSERT INTO lion_st_map (k, c1, c2, c31) VALUES (5000, 5, 6, 7);
SELECT (ctid::text::point)[0] >= 4096 AS past_the_leaf FROM lion_st_map WHERE k = 5000;
SELECT pages, map_pages FROM lion_st_stats('lion_st_map_i') WHERE attno = 1;
SELECT k, lion_index_stored('lion_st_map_i', ctid) FROM lion_st_map ORDER BY k;
SELECT lion_index_verify('lion_st_map_i', true);
DROP TABLE lion_st_map;

-- ---------------------------------------------------------------------
-- 7. VACUUM: a few dead rows a page are cleared in place; a page a quarter
--    of whose written slots are dead is written again, which drops the
--    dictionary entries no row names any more.  A cleared slot reads NULL.
-- ---------------------------------------------------------------------

CREATE TABLE lion_st_vac (id int, k int, t text);
INSERT INTO lion_st_vac SELECT g, g % 5, 'value ' || (g % 400) FROM generate_series(1, 20000) g;
CREATE INDEX lion_st_vac_i ON lion_st_vac USING lion (k) INCLUDE (id, t);
SELECT * FROM lion_st_stats('lion_st_vac_i');
CREATE TABLE lion_st_vac_dead AS SELECT ctid AS tid FROM lion_st_vac WHERE id % 10 = 7;
DELETE FROM lion_st_vac WHERE id % 10 = 7;
SELECT count(*) FILTER (WHERE array_remove(lion_index_stored('lion_st_vac_i', tid), NULL) <> '{}')
	   AS written_before
  FROM lion_st_vac_dead;
VACUUM lion_st_vac;
SELECT count(*) FILTER (WHERE array_remove(lion_index_stored('lion_st_vac_i', tid), NULL) <> '{}')
	   AS written_after
  FROM lion_st_vac_dead;
SELECT * FROM lion_st_stats('lion_st_vac_i');
SELECT lion_st_mismatch('lion_st_vac_i', 'lion_st_vac', 'ARRAY[id::text, t]');
-- every value of t but a few gone from most windows: their pages are rewritten
DELETE FROM lion_st_vac WHERE id % 400 >= 20;
VACUUM lion_st_vac;
SELECT * FROM lion_st_stats('lion_st_vac_i');
SELECT lion_st_mismatch('lion_st_vac_i', 'lion_st_vac', 'ARRAY[id::text, t]');
SELECT lion_index_verify('lion_st_vac_i', true);
-- the TIDs come back, with other values
INSERT INTO lion_st_vac SELECT g, g % 5, 'again ' || g FROM generate_series(20001, 30000) g;
SELECT lion_st_mismatch('lion_st_vac_i', 'lion_st_vac', 'ARRAY[id::text, t]');
SELECT lion_index_verify('lion_st_vac_i', true);

-- ---------------------------------------------------------------------
-- 8. A truncated heap: VACUUM clears the slots of the dead rows, then cuts
--    the heap; the next VACUUM finds every window past the heap's end and
--    frees its pages.  Rows inserted afterwards start new chains.
-- ---------------------------------------------------------------------

DELETE FROM lion_st_vac;
VACUUM lion_st_vac;
SELECT pg_relation_size('lion_st_vac') AS heap_bytes;
SELECT attno, pages, map_pages FROM lion_st_stats('lion_st_vac_i');
VACUUM lion_st_vac;
SELECT attno, pages, map_pages FROM lion_st_stats('lion_st_vac_i');
SELECT deleted_pages > 0 AS freed FROM lion_index_stats('lion_st_vac_i') WHERE attno = 1;
SELECT lion_index_verify('lion_st_vac_i', true);
INSERT INTO lion_st_vac SELECT g, g % 5, 'third ' || (g % 7) FROM generate_series(1, 3000) g;
SELECT * FROM lion_st_stats('lion_st_vac_i');
SELECT lion_st_mismatch('lion_st_vac_i', 'lion_st_vac', 'ARRAY[id::text, t]');
SELECT lion_index_verify('lion_st_vac_i', true);
DROP TABLE lion_st_vac, lion_st_vac_dead;

-- ---------------------------------------------------------------------
-- 9. Key-ordered windows (DESIGN.md §41): an index with an order column,
--    the stored column cluster_column names, lays each window out in that
--    column's order behind a permutation, in buckets of the column's value
--    ranges.  A build sorts every window into buckets; an insert takes a
--    slot in the bucket of its value, and a full bucket splits; VACUUM
--    clears dead rows and sorts nothing.  Sections 2 to 8 build the heap
--    order of §40 (format version 9), as every index without
--    cluster_column is.
-- ---------------------------------------------------------------------

-- A window's state beside the rows the heap has in it.
CREATE FUNCTION lion_st_windows(idx regclass, tbl regclass)
RETURNS TABLE (win int, nrows bigint, generation int8, buckets int4, slots bigint,
			   entries bigint, orphaned bigint, directory bool, perm_pages int4)
LANGUAGE plpgsql AS $$
BEGIN
	RETURN QUERY EXECUTE format(
		'SELECT c.w, c.n, s.generation, s.buckets, s.slots, s.entries, s.orphaned,
				s.directory > 0, s.perm_pages
		   FROM (SELECT floor((ctid::text::point)[0] / 64)::int AS w, count(*) AS n
				   FROM %s GROUP BY 1) c,
				lion_index_store_window(%L, c.w) s
		  ORDER BY c.w', tbl, idx);
END $$;

-- Every column in key order, however short its chains: this section is
-- about the buckets, and section 10 about which columns an index orders.
SET pg_lion.store_order_min_pages = 0;

-- Eight heap pages of 75 rows a window would be too few to sort anything
-- worth the name: a narrow row, 40,000 of them, in about eight windows.
CREATE TABLE lion_st_ord (id int, k int, g int, tags int[], t text, p point)
	WITH (autovacuum_enabled = off, fillfactor = 90);
INSERT INTO lion_st_ord
SELECT i, i % 7, (i * 7919) % 101, ARRAY[i % 5, 10 + i % 11], 'v' || (i % 333),
	   point(i, i)
  FROM generate_series(1, 40000) i;

-- the order column is cluster_column's: the first key column, stored ...
CREATE INDEX lion_st_ord_k ON lion_st_ord USING lion (k) INCLUDE (g, t)
	WITH (store_values = on, cluster_column = k);
-- ... an INCLUDE column of an index whose first key column is multi-key ...
CREATE INDEX lion_st_ord_tags ON lion_st_ord USING lion (tags) INCLUDE (g, t)
	WITH (cluster_column = g);
-- ... or a column other than the first key column, a collatable one
CREATE INDEX lion_st_ord_kt ON lion_st_ord USING lion (k) INCLUDE (g, t)
	WITH (store_values = on, cluster_column = t);
-- no cluster_column: heap order, the first key column stored or not
CREATE INDEX lion_st_ord_heap ON lion_st_ord USING lion (k) INCLUDE (g)
	WITH (store_values = on);
-- and pg_lion.store_heap_order: heap order, cluster_column or not
SET pg_lion.store_heap_order = on;
CREATE INDEX lion_st_ord_v9 ON lion_st_ord USING lion (k) INCLUDE (g)
	WITH (store_values = on, cluster_column = g);
RESET pg_lion.store_heap_order;
SELECT i::regclass AS index, (lion_index_store_window(i, 0)).order_attno
  FROM unnest(ARRAY['lion_st_ord_k', 'lion_st_ord_tags', 'lion_st_ord_kt',
					'lion_st_ord_heap', 'lion_st_ord_v9']::regclass[]) i;

-- cluster_column: not a column, not a stored one, no ordering
CREATE INDEX ON lion_st_ord USING lion (k) INCLUDE (g) WITH (cluster_column = nosuch);
CREATE INDEX ON lion_st_ord USING lion (k) INCLUDE (g) WITH (cluster_column = k);
CREATE INDEX ON lion_st_ord USING lion (k) INCLUDE (p) WITH (cluster_column = p);
CREATE INDEX ON lion_st_ord USING lion (k, tags) WITH (store_values = on, cluster_column = tags);

-- every window sorted by the build into 64 buckets, every row with an entry
-- and every slot handed out
SELECT * FROM lion_st_windows('lion_st_ord_k', 'lion_st_ord');
SELECT * FROM lion_st_windows('lion_st_ord_tags', 'lion_st_ord');
SELECT lion_st_mismatch('lion_st_ord_k', 'lion_st_ord', 'ARRAY[k::text, g::text, t]');
SELECT lion_st_mismatch('lion_st_ord_tags', 'lion_st_ord', 'ARRAY[g::text, t]');
SELECT lion_st_mismatch('lion_st_ord_kt', 'lion_st_ord', 'ARRAY[k::text, g::text, t]');
SELECT lion_st_mismatch('lion_st_ord_v9', 'lion_st_ord', 'ARRAY[k::text, g::text]');
SELECT lion_index_verify('lion_st_ord_k', true), lion_index_verify('lion_st_ord_tags', true),
	   lion_index_verify('lion_st_ord_kt', true), lion_index_verify('lion_st_ord_v9', true);

-- the readers: an index-only scan and a count read the store through the
-- permutation, and agree with the heap
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT k, count(*), sum(g), min(t), max(t) FROM lion_st_ord WHERE k IN (2, 5) GROUP BY k ORDER BY k;
SELECT g, count(*), min(t) FROM lion_st_ord WHERE tags @> ARRAY[3] AND g < 4 GROUP BY g ORDER BY g;
-- rows, three columns from one store: the readers of an index share its
-- permutation read for each window
SELECT count(*), md5(string_agg(k || ',' || g || ',' || t, ';' ORDER BY k, g, t))
  FROM lion_st_ord WHERE k IN (2, 5);
RESET enable_seqscan;
RESET enable_bitmapscan;
SELECT k, count(*), sum(g), min(t), max(t) FROM lion_st_ord WHERE k IN (2, 5) GROUP BY k ORDER BY k;
SELECT g, count(*), min(t) FROM lion_st_ord WHERE tags @> ARRAY[3] AND g < 4 GROUP BY g ORDER BY g;
SELECT count(*), md5(string_agg(k || ',' || g || ',' || t, ';' ORDER BY k, g, t))
  FROM lion_st_ord WHERE k IN (2, 5);

-- inserts: into the last window's buckets, which split as they fill, and
-- windows of their own, which start with one bucket and split into more
INSERT INTO lion_st_ord
SELECT i, i % 7, (i * 7919) % 101, ARRAY[i % 5, 10 + i % 11], 'w' || (i % 333),
	   point(i, i)
  FROM generate_series(40001, 52000) i;
SELECT * FROM lion_st_windows('lion_st_ord_k', 'lion_st_ord');
SELECT lion_st_mismatch('lion_st_ord_k', 'lion_st_ord', 'ARRAY[k::text, g::text, t]');
SELECT lion_st_mismatch('lion_st_ord_tags', 'lion_st_ord', 'ARRAY[g::text, t]');
SELECT lion_index_verify('lion_st_ord_k', true), lion_index_verify('lion_st_ord_tags', true);

-- VACUUM: a tenth of the rows dead, cleared from the data chains and the
-- permutation; nothing is sorted again, and no generation moves
DELETE FROM lion_st_ord WHERE id % 10 = 3 AND id <= 40000;
VACUUM lion_st_ord;
SELECT * FROM lion_st_windows('lion_st_ord_k', 'lion_st_ord');
SELECT lion_st_mismatch('lion_st_ord_k', 'lion_st_ord', 'ARRAY[k::text, g::text, t]');
SELECT lion_st_mismatch('lion_st_ord_tags', 'lion_st_ord', 'ARRAY[g::text, t]');
SELECT lion_index_verify('lion_st_ord_k', true), lion_index_verify('lion_st_ord_tags', true);

-- the freed positions taken by new rows, each in the bucket of its value
INSERT INTO lion_st_ord
SELECT i, i % 7, (i * 7919) % 101, ARRAY[i % 5, 10 + i % 11], 'x' || (i % 333),
	   point(i, i)
  FROM generate_series(52001, 54000) i;
SELECT count(*) AS reused FROM lion_st_ord WHERE id > 52000 AND (ctid::text::point)[0] < 64;
SELECT * FROM lion_st_windows('lion_st_ord_k', 'lion_st_ord');
SELECT lion_st_mismatch('lion_st_ord_k', 'lion_st_ord', 'ARRAY[k::text, g::text, t]');
SELECT lion_st_mismatch('lion_st_ord_tags', 'lion_st_ord', 'ARRAY[g::text, t]');
SELECT lion_index_verify('lion_st_ord_k', true), lion_index_verify('lion_st_ord_tags', true);

-- two thirds of the first window dead and as many rows again: each bucket
-- hands out more of its slots, and moves no row
DELETE FROM lion_st_ord WHERE id % 3 <> 0 AND (ctid::text::point)[0] < 64;
VACUUM lion_st_ord;
INSERT INTO lion_st_ord
SELECT i, i % 7, (i * 7919) % 101, ARRAY[i % 5, 10 + i % 11], 'y' || (i % 333),
	   point(i, i)
  FROM generate_series(54001, 60000) i;
SELECT * FROM lion_st_windows('lion_st_ord_k', 'lion_st_ord') WHERE win = 0;
SELECT lion_st_mismatch('lion_st_ord_k', 'lion_st_ord', 'ARRAY[k::text, g::text, t]');
SELECT lion_st_mismatch('lion_st_ord_tags', 'lion_st_ord', 'ARRAY[g::text, t]');
SELECT lion_st_mismatch('lion_st_ord_kt', 'lion_st_ord', 'ARRAY[k::text, g::text, t]');
SELECT lion_index_verify('lion_st_ord_k', true), lion_index_verify('lion_st_ord_tags', true),
	   lion_index_verify('lion_st_ord_kt', true);
VACUUM lion_st_ord;
SELECT * FROM lion_st_windows('lion_st_ord_k', 'lion_st_ord');
SELECT lion_st_mismatch('lion_st_ord_k', 'lion_st_ord', 'ARRAY[k::text, g::text, t]');
SELECT lion_index_verify('lion_st_ord_k', true), lion_index_verify('lion_st_ord_tags', true);

-- a heap truncated: the windows past its end freed, the permutation last
DELETE FROM lion_st_ord WHERE (ctid::text::point)[0] >= 64;
VACUUM lion_st_ord;
VACUUM lion_st_ord;
SELECT * FROM lion_st_windows('lion_st_ord_k', 'lion_st_ord');
SELECT (lion_index_store_window('lion_st_ord_k', 3)).*;
SELECT lion_st_mismatch('lion_st_ord_k', 'lion_st_ord', 'ARRAY[k::text, g::text, t]');
SELECT lion_index_verify('lion_st_ord_k', true), lion_index_verify('lion_st_ord_tags', true);

-- an index built empty, and every row inserted: each window starts with
-- one bucket and splits into more; VACUUM has nothing to do
CREATE TABLE lion_st_ord2 (id int, k int, t text) WITH (autovacuum_enabled = off);
CREATE INDEX lion_st_ord2_i ON lion_st_ord2 USING lion (k) INCLUDE (t, id)
	WITH (store_values = on, cluster_column = k);
INSERT INTO lion_st_ord2 SELECT i, i % 13, 'z' || (i % 77) FROM generate_series(1, 20000) i;
SELECT * FROM lion_st_windows('lion_st_ord2_i', 'lion_st_ord2');
SELECT lion_st_mismatch('lion_st_ord2_i', 'lion_st_ord2', 'ARRAY[k::text, t, id::text]');
SELECT lion_index_verify('lion_st_ord2_i', true);
VACUUM lion_st_ord2;
SELECT * FROM lion_st_windows('lion_st_ord2_i', 'lion_st_ord2');
-- REINDEX writes it again, every window sorted at build
INSERT INTO lion_st_ord2 SELECT i, i % 13, 'z' || (i % 77) FROM generate_series(20001, 21000) i;
REINDEX INDEX lion_st_ord2_i;
SELECT * FROM lion_st_windows('lion_st_ord2_i', 'lion_st_ord2');
SELECT lion_st_mismatch('lion_st_ord2_i', 'lion_st_ord2', 'ARRAY[k::text, t, id::text]');
SELECT lion_index_verify('lion_st_ord2_i', true);

-- the order column rising with the heap: a row past every row of its bucket
-- opens a new bucket, and no row moves - the generation stays where it was
CREATE TABLE lion_st_ord3 (id int, k int, t text) WITH (autovacuum_enabled = off);
CREATE INDEX lion_st_ord3_i ON lion_st_ord3 USING lion (k) INCLUDE (t)
	WITH (store_values = on, cluster_column = k);
INSERT INTO lion_st_ord3 SELECT i, i, 'r' || i FROM generate_series(1, 12000) i;
SELECT * FROM lion_st_windows('lion_st_ord3_i', 'lion_st_ord3');
SELECT lion_st_mismatch('lion_st_ord3_i', 'lion_st_ord3', 'ARRAY[k::text, t]');
SELECT lion_index_verify('lion_st_ord3_i', true);

-- values too wide for a bucket's slots to fit a page: a bucket splits when
-- its values no longer fit, before it has handed out every slot
CREATE TABLE lion_st_ord4 (id int, k int, t text) WITH (autovacuum_enabled = off);
CREATE INDEX lion_st_ord4_i ON lion_st_ord4 USING lion (k) INCLUDE (t)
	WITH (store_values = on, cluster_column = k);
INSERT INTO lion_st_ord4 SELECT i, i % 5, repeat(chr(65 + i % 26), 300) || i
  FROM generate_series(1, 3000) i;
SELECT * FROM lion_st_windows('lion_st_ord4_i', 'lion_st_ord4');
SELECT lion_st_mismatch('lion_st_ord4_i', 'lion_st_ord4', 'ARRAY[k::text, t]');
SELECT lion_index_verify('lion_st_ord4_i', true);

-- order values too long for the window header to keep many fences: the
-- buckets stop splitting, and the rows they have no room for keep no values
-- in the store - each such row has no entry, and readers take it from the
-- heap
CREATE TABLE lion_st_ord5 (id int, k text, t text) WITH (autovacuum_enabled = off);
CREATE INDEX lion_st_ord5_i ON lion_st_ord5 USING lion (k) INCLUDE (t)
	WITH (store_values = on, cluster_column = k);
INSERT INTO lion_st_ord5 SELECT i, lpad((i * 7919 % 4000)::text, 240, '0'), 'u' || i
  FROM generate_series(1, 4000) i;
SELECT * FROM lion_st_windows('lion_st_ord5_i', 'lion_st_ord5');
SELECT count(*) FILTER (WHERE s = ARRAY[NULL, NULL]::text[]) AS left_to_heap,
	   count(*) FILTER (WHERE s <> ARRAY[NULL, NULL]::text[] AND s IS DISTINCT FROM ARRAY[k, t])
		 AS mismatched
  FROM (SELECT k, t, lion_index_stored('lion_st_ord5_i', ctid) s FROM lion_st_ord5) q;
SELECT lion_index_verify('lion_st_ord5_i', true);
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*), md5(string_agg(t, ',' ORDER BY t)) FROM lion_st_ord5 WHERE k > '0';
RESET enable_seqscan;
RESET enable_bitmapscan;
SELECT count(*), md5(string_agg(t, ',' ORDER BY t)) FROM lion_st_ord5 WHERE k > '0';

-- a window whose rows are deleted and replaced again and again: its
-- buckets hand out every slot they have, and then, holding few live rows,
-- are compacted into a new bucket each rather than split in two
CREATE TABLE lion_st_ord6 (id int, k int, t text) WITH (autovacuum_enabled = off);
INSERT INTO lion_st_ord6 SELECT i, i % 101, 'c' || i FROM generate_series(1, 4000) i;
CREATE INDEX lion_st_ord6_i ON lion_st_ord6 USING lion (k) INCLUDE (t)
	WITH (store_values = on, cluster_column = k);
DELETE FROM lion_st_ord6 WHERE id % 4 <> 0;
VACUUM lion_st_ord6;
INSERT INTO lion_st_ord6 SELECT i * 4 + 1, i % 101, 'c' || i FROM generate_series(4001, 7000) i;
DELETE FROM lion_st_ord6 WHERE id % 4 <> 0;
VACUUM lion_st_ord6;
INSERT INTO lion_st_ord6 SELECT i * 4 + 1, i % 101, 'c' || i FROM generate_series(7001, 10000) i;
DELETE FROM lion_st_ord6 WHERE id % 4 <> 0;
VACUUM lion_st_ord6;
INSERT INTO lion_st_ord6 SELECT i * 4 + 1, i % 101, 'c' || i FROM generate_series(10001, 13000) i;
DELETE FROM lion_st_ord6 WHERE id % 4 <> 0;
VACUUM lion_st_ord6;
INSERT INTO lion_st_ord6 SELECT i * 4 + 1, i % 101, 'c' || i FROM generate_series(13001, 16000) i;
DELETE FROM lion_st_ord6 WHERE id % 4 <> 0;
VACUUM lion_st_ord6;
INSERT INTO lion_st_ord6 SELECT i * 4 + 1, i % 101, 'c' || i FROM generate_series(16001, 19000) i;
DELETE FROM lion_st_ord6 WHERE id % 4 <> 0;
VACUUM lion_st_ord6;
INSERT INTO lion_st_ord6 SELECT i * 4 + 1, i % 101, 'c' || i FROM generate_series(19001, 22000) i;
DELETE FROM lion_st_ord6 WHERE id % 4 <> 0;
VACUUM lion_st_ord6;
INSERT INTO lion_st_ord6 SELECT i * 4 + 1, i % 101, 'c' || i FROM generate_series(22001, 25000) i;
SELECT * FROM lion_st_windows('lion_st_ord6_i', 'lion_st_ord6');
SELECT lion_st_mismatch('lion_st_ord6_i', 'lion_st_ord6', 'ARRAY[k::text, t]');
SELECT lion_index_verify('lion_st_ord6_i', true);

DROP TABLE lion_st_ord, lion_st_ord2, lion_st_ord3, lion_st_ord4, lion_st_ord5, lion_st_ord6;
RESET pg_lion.store_order_min_pages;

-- ---------------------------------------------------------------------
-- 10. Short chains in heap layout (DESIGN.md §41, "Revision 2"): an
--     ordered index lays out in key order only the columns whose chain in
--     a build's first window takes pg_lion.store_order_min_pages pages or
--     more (3 by default), and the order column with them; the others keep
--     §40's heap layout beside them.  No long column at all: heap order,
--     version 9.  With no rows to measure, a column is long by its type.
-- ---------------------------------------------------------------------

CREATE TABLE lion_st_mix (id int, grp int, tags int[], pay text)
	WITH (autovacuum_enabled = off);
INSERT INTO lion_st_mix SELECT i, i % 50, ARRAY[i % 97], repeat('x', 40) || i
  FROM generate_series(1, 30000) i;
-- grp (one page a window) is the order column, pay (about thirty) is long,
-- id (three) is long at the default ...
CREATE INDEX lion_st_mix_i ON lion_st_mix USING lion (tags) INCLUDE (grp, id, pay)
	WITH (cluster_column = grp);
-- ... and short at 4
SET pg_lion.store_order_min_pages = 4;
CREATE INDEX lion_st_mix_j ON lion_st_mix USING lion (tags) INCLUDE (grp, id, pay)
	WITH (cluster_column = grp);
-- no column long: heap order
SET pg_lion.store_order_min_pages = 100;
CREATE INDEX lion_st_mix_h ON lion_st_mix USING lion (tags) INCLUDE (grp, id, pay)
	WITH (cluster_column = grp);
RESET pg_lion.store_order_min_pages;
SELECT i::regclass AS index, w.order_attno, w.buckets, w.perm_pages, w.pages > 0 AS ordered,
	   w.heap_pages
  FROM unnest(ARRAY['lion_st_mix_i', 'lion_st_mix_j', 'lion_st_mix_h']::regclass[]) i,
	   lion_index_store_window(i, 0) w;
SELECT lion_st_mismatch('lion_st_mix_i', 'lion_st_mix', 'ARRAY[grp::text, id::text, pay]'),
	   lion_st_mismatch('lion_st_mix_j', 'lion_st_mix', 'ARRAY[grp::text, id::text, pay]'),
	   lion_st_mismatch('lion_st_mix_h', 'lion_st_mix', 'ARRAY[grp::text, id::text, pay]');

-- inserts split the buckets of the columns in key order and write the others
-- at their heap positions; VACUUM clears both
INSERT INTO lion_st_mix SELECT i, i % 50, ARRAY[i % 97], repeat('y', 40) || i
  FROM generate_series(30001, 40000) i;
DELETE FROM lion_st_mix WHERE id % 7 = 0;
VACUUM lion_st_mix;
INSERT INTO lion_st_mix SELECT i, i % 50, ARRAY[i % 97], repeat('z', 40) || i
  FROM generate_series(40001, 45000) i;
SELECT * FROM lion_st_windows('lion_st_mix_j', 'lion_st_mix');
SELECT lion_st_mismatch('lion_st_mix_i', 'lion_st_mix', 'ARRAY[grp::text, id::text, pay]'),
	   lion_st_mismatch('lion_st_mix_j', 'lion_st_mix', 'ARRAY[grp::text, id::text, pay]'),
	   lion_st_mismatch('lion_st_mix_h', 'lion_st_mix', 'ARRAY[grp::text, id::text, pay]');
SELECT lion_index_verify('lion_st_mix_i', true), lion_index_verify('lion_st_mix_j', true),
	   lion_index_verify('lion_st_mix_h', true);

-- the readers of either layout, through one index
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT sum(id), count(pay), sum(grp), md5(string_agg(pay, ',' ORDER BY pay))
  FROM lion_st_mix WHERE tags @> ARRAY[3];
SELECT sum(id), count(*) FROM lion_st_mix WHERE tags @> ARRAY[3] AND grp = 3;
RESET enable_seqscan;
RESET enable_bitmapscan;
SELECT sum(id), count(pay), sum(grp), md5(string_agg(pay, ',' ORDER BY pay))
  FROM lion_st_mix WHERE tags @> ARRAY[3];
SELECT sum(id), count(*) FROM lion_st_mix WHERE tags @> ARRAY[3] AND grp = 3;

-- no rows to measure: by type, the text column in key order with grp, the
-- ints in heap layout; and with no column of a long type, heap order
TRUNCATE lion_st_mix;
REINDEX INDEX lion_st_mix_i;
CREATE INDEX lion_st_mix_n ON lion_st_mix USING lion (tags) INCLUDE (grp, id)
	WITH (cluster_column = grp);
INSERT INTO lion_st_mix SELECT i, i % 50, ARRAY[i % 97], repeat('x', 40) || i
  FROM generate_series(1, 6000) i;
SELECT i::regclass AS index, w.order_attno, w.perm_pages, w.pages > 0 AS ordered,
	   w.heap_pages
  FROM unnest(ARRAY['lion_st_mix_i', 'lion_st_mix_n']::regclass[]) i,
	   lion_index_store_window(i, 0) w;
SELECT lion_st_mismatch('lion_st_mix_i', 'lion_st_mix', 'ARRAY[grp::text, id::text, pay]'),
	   lion_st_mismatch('lion_st_mix_n', 'lion_st_mix', 'ARRAY[grp::text, id::text]');
SELECT lion_index_verify('lion_st_mix_i', true), lion_index_verify('lion_st_mix_n', true);

DROP TABLE lion_st_mix;
DROP FUNCTION lion_st_windows(regclass, regclass);

DROP FUNCTION lion_st_mismatch(regclass, regclass, text);
DROP FUNCTION lion_st_stats(regclass);
