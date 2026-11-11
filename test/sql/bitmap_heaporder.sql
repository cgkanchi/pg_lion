-- The TIDs of many keys into one bitmap, by heap block (DESIGN.md §28, "Many
-- keys into one bitmap").
--
-- A bitmap scan's walk of many keys - a range, a list, IS NOT NULL, a
-- multi-key query that needs the whole index - keeps the TIDs of keys whose
-- rows are spread over the heap, gathered by container key, and gives them to
-- the TIDBitmap a heap block at a time.  It starts once the bitmap has been
-- given as many TIDs as the heap had pages at its last VACUUM, and empties
-- what it keeps whenever that outgrows work_mem.  The DEBUG1 line of each scan
-- says how many TIDs went that way, of how many, and in how many flushes.
-- Every answer is checked against a sequential scan.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET max_parallel_workers_per_gather = 0;

-- k: 6,000 values, ten rows each, in no heap order, so a key's rows are on ten
-- pages and its posting set is a sparse segment or a few small containers.
-- t: two of 300 elements a row.  500 heap pages, eight container keys: a wide
-- range keeps thousands of TIDs a container key, folded into its bitset.
CREATE TABLE bho (id int, k int, t int[]) WITH (autovacuum_enabled = off);
INSERT INTO bho
SELECT i, (hashint4(i) & 2147483647) % 6000,
       ARRAY[(hashint4(i + 1) & 2147483647) % 300,
             (hashint4(i + 2) & 2147483647) % 300]
  FROM generate_series(1, 60000) i;
CREATE INDEX bho_k ON bho USING lion (k);
CREATE INDEX bho_t ON bho USING lion (t);
VACUUM (FREEZE, ANALYZE) bho;
SELECT relpages FROM pg_class WHERE relname = 'bho';

-- The same rows at fillfactor 10: 2,728 pages, more than the bitmap holds
-- exactly at work_mem = 64kB.
CREATE TABLE bhw (id int, k int) WITH (autovacuum_enabled = off, fillfactor = 10);
INSERT INTO bhw SELECT id, k FROM bho;
CREATE INDEX bhw_k ON bhw USING lion (k);
VACUUM (FREEZE, ANALYZE) bhw;
SELECT relpages FROM pg_class WHERE relname = 'bhw';

-- o: ten rows a value in heap order, so a key's rows are on one or two pages
-- and the walk already gives the bitmap its TIDs a page at a time.
CREATE TABLE bhs (id int, o int) WITH (autovacuum_enabled = off);
INSERT INTO bhs SELECT i, i / 10 FROM generate_series(1, 60000) i;
CREATE INDEX bhs_o ON bhs USING lion (o);
VACUUM (FREEZE, ANALYZE) bhs;

/*
 * bho_same() runs q as a bitmap scan and as a sequential scan and says
 * whether the two return the same rows, as multisets.
 */
CREATE FUNCTION bho_same(q text) RETURNS boolean
LANGUAGE plpgsql AS $$
DECLARE
	q2 text := 'SELECT count(*), md5(string_agg(r::text, '','' ORDER BY r::text)) FROM ('
		|| q || ') r';
	on_n bigint;
	on_h text;
	off_n bigint;
	off_h text;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	EXECUTE q2 INTO on_n, on_h;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	EXECUTE q2 INTO off_n, off_h;
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	RETURN on_n = off_n AND on_h IS NOT DISTINCT FROM off_h;
END
$$;

/* the plan's bitmap index scan and its rows, and whether the heap went lossy */
CREATE FUNCTION bho_plan(q text) RETURNS TABLE (index_rows bigint, lossy boolean)
LANGUAGE plpgsql AS $$
DECLARE
	j jsonb;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, FORMAT JSON) ' || q
		INTO j;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	RETURN QUERY SELECT
		(jsonb_path_query_first(j, 'strict $.** ? (@."Node Type" == "Bitmap Index Scan")') ->> 'Actual Rows')::numeric::bigint,
		coalesce((jsonb_path_query_first(j, 'strict $.** ? (@."Node Type" == "Bitmap Heap Scan")') ->> 'Lossy Heap Blocks')::int, 0) > 0;
END
$$;

SET client_min_messages = debug1;

-- a range over nearly every key: all but about the first 500 TIDs kept, each
-- container key's thousands folded into its bitset
SELECT bho_same('SELECT id, k FROM bho WHERE k BETWEEN 100 AND 5900');
-- a narrow range above the 500 pages: a few TIDs a container key, sorted by
-- block from the array
SELECT bho_same('SELECT id, k FROM bho WHERE k BETWEEN 100 AND 200');
-- below them: every TID at once, and no line
SELECT bho_same('SELECT id, k FROM bho WHERE k BETWEEN 100 AND 140');
-- every key; the widest bound of an ANY; a list of 2,000 keys
SELECT bho_same('SELECT id, k FROM bho WHERE k IS NOT NULL');
SELECT bho_same('SELECT id, k FROM bho WHERE k < ANY (''{3000, 5900, 10}''::int[])');
SELECT bho_same('SELECT id, k FROM bho WHERE k = ANY (ARRAY(SELECT generate_series(1, 6000, 3)))');
-- a multi-key query that needs the whole index: every element's set walked,
-- each row under two of them, and every TID rechecked
SELECT bho_same('SELECT id, t FROM bho WHERE t <@ ARRAY(SELECT generate_series(0, 250))');
-- a walk in heap order: its first TIDs come many a page, and it keeps none
-- (no line)
SELECT bho_same('SELECT id, o FROM bhs WHERE o BETWEEN 100 AND 5900');
SELECT bho_same('SELECT id, o FROM bhs WHERE o IS NOT NULL');

-- work_mem = 64kB: what is kept is given to the bitmap in several flushes
SET work_mem = '64kB';
SELECT bho_same('SELECT id, k FROM bho WHERE k BETWEEN 100 AND 5900');
SELECT bho_same('SELECT id, k FROM bho WHERE k = ANY (ARRAY(SELECT generate_series(1, 6000, 3)))');
SELECT bho_same('SELECT id, t FROM bho WHERE t <@ ARRAY(SELECT generate_series(0, 250))');
-- ... and over more pages than the bitmap holds exactly: it goes lossy, and
-- the recheck has the last word
SELECT bho_same('SELECT id, k FROM bhw WHERE k BETWEEN 100 AND 5900');
SELECT bho_same('SELECT id, k FROM bhw WHERE k IS NOT NULL');
RESET client_min_messages;
SELECT * FROM bho_plan('SELECT id FROM bhw WHERE k BETWEEN 100 AND 5900');
RESET work_mem;
SELECT * FROM bho_plan('SELECT id FROM bhw WHERE k BETWEEN 100 AND 5900');
SELECT * FROM bho_plan('SELECT id FROM bho WHERE t <@ ARRAY(SELECT generate_series(0, 250))');

-- rows deleted and not vacuumed: their TIDs are still in the index, and the
-- heap scan leaves them out
DELETE FROM bho WHERE id % 7 = 0;
SET client_min_messages = debug1;
SELECT bho_same('SELECT id, k FROM bho WHERE k BETWEEN 100 AND 5900');
RESET client_min_messages;

DROP TABLE bho;
DROP TABLE bhw;
DROP TABLE bhs;
DROP FUNCTION bho_same(text);
DROP FUNCTION bho_plan(text);
