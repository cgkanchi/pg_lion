-- A bitmap scan of a long IN list beside other quals (2026-10-18 review;
-- DESIGN.md §15, "A list too long to locate at once").
--
-- A parameter's array has no length cap.  The bitmap scan of several quals of
-- a lion index (lion_emit_columns(), and lion_emit_intersect() beside a range)
-- located the posting set of every value of `k = ANY ($1)` at once: some 200
-- bytes a value, work_mem or not, and past nine million values an array of
-- them larger than an allocation may be - "invalid memory alloc request size"
-- where the ordinary plan, and the scan of the list on its own, answered.  It
-- now locates such a list a batch at a time, as the count does, and emits
-- each batch's union, ANDed with the other quals, into the same bitmap.  Every
-- query below is answered by a bitmap scan of a multicolumn index and by a
-- sequential scan, and the two must agree, across every batch boundary.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET max_parallel_workers_per_gather = 0;

/*
 * k and ki hold the same 1500 values, 27 rows each: k's index is CHAIN
 * (inline_limit = 64, a posting page per entry), ki's INLINE.  r is a range
 * column beside k, tags a multi-key one, t a text one.  ki and t are a table
 * of their own, so that no index of x alone draws the planner from k's.
 */
CREATE TABLE lion_sl (id int, k int, x int, r int, tags int[]);
INSERT INTO lion_sl
SELECT i, i % 1500, i % 7, i % 100, ARRAY[i % 50, 100 + i % 3]
  FROM generate_series(1, 40000) i;
CREATE INDEX lion_sl_a ON lion_sl USING lion (k, x, r, tags) WITH (inline_limit = 64);
VACUUM ANALYZE lion_sl;
CREATE TABLE lion_sli (id int, ki int, x int, t text);
INSERT INTO lion_sli
SELECT i, i % 1500, i % 7, 'v' || (i % 4000) FROM generate_series(1, 40000) i;
CREATE INDEX lion_sli_b ON lion_sli USING lion (ki, x);
CREATE INDEX lion_sli_c ON lion_sli USING lion (t, x);
VACUUM ANALYZE lion_sli;
SELECT entries > 0 AND inline_entries = 0 AS k_is_chain
  FROM lion_index_stats('lion_sl_a') WHERE attno = 1;
SELECT entries = inline_entries AS ki_is_inline
  FROM lion_index_stats('lion_sli_b') WHERE attno = 1;

/*
 * lion_slplan() says whether q, planned as a bitmap scan - the count
 * pushdown, the sequential scan and the plain index scans off - is a bitmap
 * index scan of idx alone, and whether the heap scan above it filters a qual
 * the index was not given.  lion_slcmp() runs q so, and again as a
 * sequential scan; the two answers must agree as multisets.
 */
CREATE FUNCTION lion_slplan(q text, idx text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	used boolean := false;
	other boolean := false;
	filtered boolean := false;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ ('Bitmap Index Scan on ' || idx || '$') THEN
			used := true;
		ELSIF ln ~ 'Bitmap Index Scan on|BitmapAnd|BitmapOr' THEN
			other := true;
		ELSIF ln ~ '^\s*Filter:' THEN
			filtered := true;
		END IF;
	END LOOP;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	RETURN CASE WHEN used AND NOT other THEN 'bitmap scan of ' || idx
				ELSE 'NOT a bitmap scan of ' || idx || ' alone' END ||
		   CASE WHEN filtered THEN ' and a filter' ELSE '' END;
END $$;

CREATE FUNCTION lion_slcmp(q text, idx text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	plan text := lion_slplan(q, idx);
	a text;
	b text;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	EXECUTE 'SELECT array_agg(s ORDER BY s)::text FROM (' || q || ') s' INTO a;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	EXECUTE 'SELECT array_agg(s ORDER BY s)::text FROM (' || q || ') s' INTO b;
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH: %s against %s', left(a, 120), left(b, 120));
	END IF;
	RETURN plan || ', same answer' ||
		   CASE WHEN a IS NULL THEN ' (no row)' ELSE '' END;
END $$;

-- A batch is lion_array_batch_size(): a work_mem of located sets, and never
-- fewer than a thousand values.
SET work_mem = '64kB';

-- ---------- beside an equality (lion_emit_columns()) ----------
-- one batch, and across the first boundaries
SELECT n, lion_slcmp(format(
	'SELECT id FROM lion_sl WHERE k = ANY (array(SELECT g FROM generate_series(1, %s) g)) AND x = 1', n),
	'lion_sl_a')
  FROM (VALUES (999), (1000), (1001), (1499), (2001), (3000)) v(n);
-- repeats that straddle every boundary, NULLs, and another element type
SELECT lion_slcmp('SELECT id FROM lion_sl WHERE k = ANY (array(SELECT g % 1500 FROM generate_series(0, 4499) g)) AND x = 2', 'lion_sl_a');
SELECT lion_slcmp('SELECT id FROM lion_sl WHERE k = ANY (array(SELECT CASE WHEN g % 7 = 0 THEN NULL ELSE g END FROM generate_series(0, 2999) g)) AND x = 3', 'lion_sl_a');
SELECT lion_slcmp('SELECT id FROM lion_sl WHERE k = ANY (array(SELECT g::int8 FROM generate_series(0, 2499) g)) AND x = 4', 'lion_sl_a');
-- values with no entry: in every batch, in the first batches only, and NULLs only
SELECT lion_slcmp('SELECT id FROM lion_sl WHERE k = ANY (array(SELECT g FROM generate_series(100000, 103000) g)) AND x = 1', 'lion_sl_a');
SELECT lion_slcmp('SELECT id FROM lion_sl WHERE k = ANY (array(SELECT g FROM generate_series(-2600, 1100) g)) AND x = 5', 'lion_sl_a');
SELECT lion_slcmp('SELECT id FROM lion_sl WHERE k = ANY (array(SELECT NULL::int FROM generate_series(1, 2000) g)) AND x = 5', 'lion_sl_a');
-- the list with nothing else answered: `x IS NOT NULL` is dropped and rechecked
SELECT lion_slcmp('SELECT id FROM lion_sl WHERE k = ANY (array(SELECT g * 3 FROM generate_series(0, 1999) g)) AND x IS NOT NULL', 'lion_sl_a');
-- another list beside it, short or long (the longest is batched, the other
-- located whole), on another column or on the same one
SELECT lion_slcmp('SELECT id FROM lion_sl WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) AND x = ANY (''{1, 2, 3}'')', 'lion_sl_a');
SELECT lion_slcmp('SELECT id FROM lion_sl WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) AND r = ANY (array(SELECT g % 40 FROM generate_series(1, 1200) g))', 'lion_sl_a');
SELECT lion_slcmp('SELECT id FROM lion_sl WHERE k = ANY (array(SELECT g FROM generate_series(0, 1200) g)) AND r = ANY (array(SELECT g % 40 FROM generate_series(1, 2400) g)) AND x = 6', 'lion_sl_a');
SELECT lion_slcmp('SELECT id FROM lion_sl WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) AND k = ANY (array(SELECT g * 2 FROM generate_series(200, 1300) g))', 'lion_sl_a');
-- a multi-key column beside it
SELECT lion_slcmp($$SELECT id FROM lion_sl WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) AND tags && '{3, 17}'$$, 'lion_sl_a');
SELECT lion_slcmp($$SELECT id FROM lion_sl WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) AND tags @> '{101}' AND x = 0$$, 'lion_sl_a');
-- INLINE entries, and text keys
SELECT lion_slcmp('SELECT id FROM lion_sli WHERE ki = ANY (array(SELECT g FROM generate_series(0, 2999) g)) AND x = 4', 'lion_sli_b');
SELECT lion_slcmp('SELECT id FROM lion_sli WHERE ki = ANY (array(SELECT g % 1500 FROM generate_series(0, 5999) g)) AND x = 1', 'lion_sli_b');
SELECT lion_slcmp($$SELECT id FROM lion_sli WHERE t = ANY (array(SELECT 'v' || g FROM generate_series(0, 3999) g)) AND x = 5$$, 'lion_sli_c');

-- ---------- beside a range (lion_emit_intersect()) ----------
SELECT lion_slcmp('SELECT id FROM lion_sl WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) AND r < 30', 'lion_sl_a');
SELECT lion_slcmp('SELECT id FROM lion_sl WHERE k = ANY (array(SELECT g % 1500 FROM generate_series(0, 4499) g)) AND x = 2 AND r BETWEEN 10 AND 60', 'lion_sl_a');
-- `r < ANY (array)`: one walk, to the widest element (a parameter's array on
-- r, a lower column, the planner leaves to the heap's filter)
SELECT lion_slcmp($$SELECT id FROM lion_sl WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) AND r < ANY ('{2, 1}')$$, 'lion_sl_a');
SELECT lion_slcmp($$SELECT id FROM lion_sl WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) AND r >= ANY ('{97, 99}')$$, 'lion_sl_a');
-- two range columns: every window walks both, for every batch
SELECT lion_slcmp('SELECT id FROM lion_sl WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) AND x < 3 AND r >= 50', 'lion_sl_a');
-- ... and across windows of the other columns' answer as well
SET pg_lion.scan_window_floor = '64kB';
SELECT lion_slcmp('SELECT id FROM lion_sl WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) AND x < 6 AND r >= 5', 'lion_sl_a');
RESET pg_lion.scan_window_floor;
-- a long `k < ANY (array)` beside the sets: one walk, to its widest element
-- (lion_scan_col_ranges() made a range for every element, and used one)
SELECT lion_slcmp('SELECT id FROM lion_sl WHERE x = 1 AND k < ANY (array(SELECT g % 500 FROM generate_series(1, 5000) g))', 'lion_sl_a');
SELECT lion_slcmp('SELECT id FROM lion_sl WHERE x = 3 AND k >= ANY (array(SELECT 1499 - g % 600 FROM generate_series(1, 5000) g))', 'lion_sl_a');

-- ---------- a dirty heap ----------
UPDATE lion_sl SET x = (x + 1) % 7 WHERE id % 11 = 0;
DELETE FROM lion_sl WHERE id % 13 = 0;
UPDATE lion_sli SET x = (x + 1) % 7 WHERE id % 11 = 0;
DELETE FROM lion_sli WHERE id % 13 = 0;
SELECT lion_slcmp('SELECT id FROM lion_sl WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) AND x = 1', 'lion_sl_a');
SELECT lion_slcmp('SELECT id FROM lion_sl WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) AND r < 30', 'lion_sl_a');
SELECT lion_slcmp('SELECT id FROM lion_sli WHERE ki = ANY (array(SELECT g % 1500 FROM generate_series(0, 5999) g)) AND x = 1', 'lion_sli_b');

-- ---------- the plain scan's LIST (DESIGN.md §29.4) ----------
-- A list of one value repeated is one equality class, which a batch never
-- splits: located a batch at a time, the repeats ran the batch on to the
-- whole list.  They are dropped before the list is cut now, as the count
-- drops them.  The plain index scan, compared with the sequential scan.
SET enable_bitmapscan = off;
SET enable_seqscan = off;
SET pg_lion.enable_count_pushdown = off;
SELECT count(*) AS plain_list FROM lion_sli
 WHERE ki = ANY (array(SELECT CASE WHEN g <= 3000 THEN 7 ELSE g % 1500 END FROM generate_series(1, 4500) g)) AND x = 3;
RESET enable_seqscan;
SET enable_indexscan = off;
SELECT count(*) AS seqscan FROM lion_sli
 WHERE ki = ANY (array(SELECT CASE WHEN g <= 3000 THEN 7 ELSE g % 1500 END FROM generate_series(1, 4500) g)) AND x = 3;
RESET enable_indexscan;
RESET enable_bitmapscan;
RESET pg_lion.enable_count_pushdown;
RESET work_mem;

-- ---------- one range per element (an unordered column) ----------
/*
 * A class whose proc 4 is a SQL function no btree family sorts with builds
 * its directory in hash order (DESIGN.md §21 rule 3): `a < ANY (array)` on
 * it cannot be cut down to the widest element, and is one range per element.
 * With nothing but ranges in the query the first range column is collected
 * into a set that stands for the other columns: the union of its ranges, as
 * one set in one window's memory (lion_ranges_collect()), where each range
 * was a set of its own with all of that memory.  At work_mem and the window
 * floor's least, the 25 ranges of a < 0 .. 1500, scattered over 3000 heap
 * blocks, outgrow the window as they are ORed in - the union is optimized
 * only once its walks are done (lion_range_union_finish()) - and the
 * collection goes on a window at a time, every window a walk of each range,
 * into one temporary file.
 */
CREATE FUNCTION lion_slu_cmp(int4, int4) RETURNS int4 LANGUAGE sql IMMUTABLE STRICT
	AS 'SELECT btint4cmp($1, $2)';
CREATE OPERATOR FAMILY lion_slu_fam USING lion;
CREATE OPERATOR CLASS lion_slu_ops FOR TYPE int4 USING lion FAMILY lion_slu_fam AS
	OPERATOR 1 = (int4, int4),
	OPERATOR 6 < (int4, int4),
	OPERATOR 7 <= (int4, int4),
	OPERATOR 8 >= (int4, int4),
	OPERATOR 9 > (int4, int4),
	FUNCTION 1 hashint4(int4),
	FUNCTION 4 lion_slu_cmp(int4, int4);
CREATE TABLE lion_slu (id int, a int, b int, pad text);
INSERT INTO lion_slu
SELECT i, (i * 7919) % 3000, i % 997, repeat('x', 160) FROM generate_series(1, 120000) i;
CREATE INDEX lion_slu_ab ON lion_slu USING lion (a lion_slu_ops, b);
VACUUM (FREEZE, ANALYZE) lion_slu;
SELECT ordered FROM lion_index_stats('lion_slu_ab') WHERE attno = 1;
SELECT pg_relation_size('lion_slu') / current_setting('block_size')::int >= 2900 AS heap_blocks;
SET work_mem = '64kB';
SET pg_lion.scan_window_floor = '64kB';
SELECT lion_slcmp('SELECT id FROM lion_slu WHERE a < ANY (array(SELECT g * 60 FROM generate_series(1, 25) g)) AND b < 700', 'lion_slu_ab');
SELECT lion_slcmp('SELECT id FROM lion_slu WHERE a >= ANY (array(SELECT 2999 - g * 150 FROM generate_series(1, 12) g)) AND b >= 400', 'lion_slu_ab');
SELECT lion_slcmp('SELECT id FROM lion_slu WHERE a <= ANY (array(SELECT CASE WHEN g % 3 = 0 THEN NULL ELSE g * 90 END FROM generate_series(1, 15) g)) AND b BETWEEN 100 AND 300', 'lion_slu_ab');
-- the temporary files the first one's bitmap scan alone makes
-- (pg_stat_database, flushed on demand): one, so the windows were walked
SET pg_lion.enable_count_pushdown = off;
SET enable_seqscan = off;
SET enable_indexscan = off;
SELECT pg_stat_force_next_flush();
SELECT pg_stat_clear_snapshot();
SELECT temp_files AS lion_slu_tf FROM pg_stat_database WHERE datname = current_database() \gset
SELECT count(*) AS union_rows FROM lion_slu
 WHERE a < ANY (array(SELECT g * 60 FROM generate_series(1, 25) g)) AND b < 700;
SELECT pg_stat_force_next_flush();
SELECT pg_stat_clear_snapshot();
SELECT temp_files - :lion_slu_tf AS union_temp_files
  FROM pg_stat_database WHERE datname = current_database();
RESET enable_indexscan;
RESET enable_seqscan;
RESET pg_lion.enable_count_pushdown;
RESET pg_lion.scan_window_floor;
RESET work_mem;
DROP TABLE lion_slu;
DROP OPERATOR CLASS lion_slu_ops USING lion;
DROP OPERATOR FAMILY lion_slu_fam USING lion;
DROP FUNCTION lion_slu_cmp(int4, int4);

-- ---------- arrays past an allocation's limit ----------
/*
 * The failures the review found, cheaply: an array of nine million NULLs and
 * three values is nine million sets' worth of room for the multicolumn scan
 * - 1080000360 bytes, "invalid memory alloc request size" - and one of 23
 * million NULLs a range per element for `k < ANY`, all but one never used
 * (1104000048 bytes).  The heap scans below them take no qual of the index's
 * as a filter, which would compare every row with every element; the
 * answers are compared with the same quals over the non-NULL values instead.
 */
SELECT lion_slplan('SELECT count(*) FROM lion_sl WHERE k = ANY ((SELECT array_fill(NULL::int, ARRAY[9000000]) || ARRAY[1, 2, 3])::int[]) AND x = 1', 'lion_sl_a');
SELECT lion_slplan('SELECT count(*) FROM lion_sl WHERE k = ANY ((SELECT array_fill(NULL::int, ARRAY[9000000]) || ARRAY[1, 2, 3])::int[]) AND r < 50', 'lion_sl_a');
SELECT lion_slplan('SELECT count(*) FROM lion_sl WHERE k < ANY ((SELECT array_fill(NULL::int, ARRAY[23000000]) || ARRAY[5])::int[]) AND x = 1', 'lion_sl_a');
SET pg_lion.enable_count_pushdown = off;
SET enable_seqscan = off;
SET enable_indexscan = off;
SELECT count(*) AS nine_million_list FROM lion_sl
 WHERE k = ANY ((SELECT array_fill(NULL::int, ARRAY[9000000]) || ARRAY[1, 2, 3])::int[]) AND x = 1;
SELECT count(*) AS nine_million_list_range FROM lion_sl
 WHERE k = ANY ((SELECT array_fill(NULL::int, ARRAY[9000000]) || ARRAY[1, 2, 3])::int[]) AND r < 50;
SELECT count(*) AS range_23_million FROM lion_sl
 WHERE k < ANY ((SELECT array_fill(NULL::int, ARRAY[23000000]) || ARRAY[5])::int[]) AND x = 1;
RESET enable_indexscan;
RESET enable_seqscan;
SET enable_bitmapscan = off;
SELECT count(*) AS seqscan FROM lion_sl WHERE k = ANY ('{1, 2, 3}') AND x = 1;
SELECT count(*) AS seqscan FROM lion_sl WHERE k = ANY ('{1, 2, 3}') AND r < 50;
SELECT count(*) AS seqscan FROM lion_sl WHERE k < 5 AND x = 1;
RESET enable_bitmapscan;
RESET pg_lion.enable_count_pushdown;

-- ---------- memory ----------
/*
 * The high-water mark of this backend's resident memory (Linux; elsewhere the
 * file is missing and every delta below reads 0), measured in a fresh session
 * after the scan of the same list on its own - one value at a time
 * (lion_emit_array()), which touches the same index and heap pages and holds
 * no set - and a scan of x.  300000 values, most of them without an entry,
 * ANDed with x: 84 MB before, about 10 MB now at work_mem 4MB, most of it the
 * values themselves, sorted.
 */
CREATE TABLE lion_slm (k int, x int);
INSERT INTO lion_slm SELECT g % 30000, g % 7 FROM generate_series(1, 60000) g;
CREATE INDEX lion_slm_kx ON lion_slm USING lion (k, x);
VACUUM (FREEZE, ANALYZE) lion_slm;
CREATE FUNCTION lion_sl_hwm() RETURNS bigint
LANGUAGE sql AS $$
	SELECT COALESCE((regexp_match(pg_read_file('/proc/self/status', true),
								  'VmHWM:\s+(\d+) kB'))[1]::bigint, 0)
$$;
\connect -
SET work_mem = '4MB';
SET max_parallel_workers_per_gather = 0;
SET pg_lion.enable_count_pushdown = off;
SET enable_seqscan = off;
SET enable_indexscan = off;
SELECT count(*) FROM lion_slm WHERE x = 1;
SELECT count(*) FROM lion_slm WHERE k = ANY (array(SELECT generate_series(1, 300000)));
SELECT lion_slplan('SELECT count(*) FROM lion_slm WHERE k = ANY (array(SELECT generate_series(1, 300000))) AND x = 1', 'lion_slm_kx');
SELECT lion_sl_hwm() AS lion_sl_h0 \gset
SELECT count(*) FROM lion_slm WHERE k = ANY (array(SELECT generate_series(1, 300000))) AND x = 1;
SELECT lion_sl_hwm() - :lion_sl_h0 < 32 * 1024 AS bitmap_list_within_32mb;

DROP TABLE lion_slm;
DROP FUNCTION lion_sl_hwm();
DROP TABLE lion_sl, lion_sli;
DROP FUNCTION lion_slcmp(text, text);
DROP FUNCTION lion_slplan(text, text);
