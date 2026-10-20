-- The height of the directory with keys near LION_MAX_KEY_SIZE (DESIGN.md §21,
-- "Height").
--
-- A pivot of a 2 kB key takes a quarter of a page, so an internal page holds
-- a high key and three downlinks at most - and only because its first
-- downlink is minus infinity, with no key.  Every internal split then leaves
-- two downlinks on each half, and the directory stays logarithmic in base two
-- at worst, whatever the order of the keys: it used to split three downlinks
-- into one and two, grow internal pages faster than leaves, and pass the 32
-- levels the meta page allows after some 15,000 random inserts, which made
-- every later query, INSERT and VACUUM fail until a REINDEX.
--
-- lion_index_verify() checks the rule on every internal page: the first
-- downlink is minus infinity, and every page but the rightmost of its level
-- has two downlinks at least.  The bound checked here is the one that rule
-- gives: a directory of height h has more than 2^(h-1) leaves.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';

CREATE EXTENSION IF NOT EXISTS pg_lion;

CREATE FUNCTION dir_height_ok(idx regclass) RETURNS TABLE (
	bounded bool, fewer_internal bool) LANGUAGE sql AS $$
	SELECT directory_height = 0 OR
		   leaf_pages > power(2, directory_height - 1),
		   internal_pages < leaf_pages
	FROM lion_index_stats(idx)
$$;

-- ---------------------------------------------------------------------
-- 1. text[] elements of 2032 bytes, inserted in md5 order, which is no
--    order at all.  Before the fix: height 25 at 4,000 rows, with three times
--    as many internal pages as leaves.
-- ---------------------------------------------------------------------
CREATE TABLE dh_arr (id int, a text[]);
CREATE INDEX dh_arr_a ON dh_arr USING lion (a);
INSERT INTO dh_arr SELECT g, ARRAY[md5(g::text) || repeat('k', 2000)]
	FROM generate_series(1, 4000) g;
SELECT lion_index_verify('dh_arr_a');
SELECT * FROM dir_height_ok('dh_arr_a');
SELECT directory_height <= 14 AS low FROM lion_index_stats('dh_arr_a');
SELECT count(*) FROM dh_arr WHERE a @> ARRAY[md5('77') || repeat('k', 2000)];

-- Descending: every insert lands on the leftmost leaf, and every split of
-- an internal page is at its left end.
CREATE TABLE dh_desc (id int, a text[]);
CREATE INDEX dh_desc_a ON dh_desc USING lion (a);
INSERT INTO dh_desc SELECT g, ARRAY[lpad(g::text, 8, '0') || repeat('d', 2024)]
	FROM generate_series(3000, 1, -1) g;
SELECT lion_index_verify('dh_desc_a');
SELECT * FROM dir_height_ok('dh_desc_a');

-- Ascending, for completeness: the rightmost split keeps two downlinks on
-- the new right page too.
CREATE TABLE dh_asc (id int, a text[]);
CREATE INDEX dh_asc_a ON dh_asc USING lion (a);
INSERT INTO dh_asc SELECT g, ARRAY[lpad(g::text, 8, '0') || repeat('d', 2024)]
	FROM generate_series(1, 3000) g;
SELECT lion_index_verify('dh_asc_a');
SELECT * FROM dir_height_ok('dh_asc_a');

-- Mixed sizes: short keys between maximum-size ones, so that the byte-halving
-- cut is pulled around by keys of very different sizes.
CREATE TABLE dh_mix (id int, a text[]);
CREATE INDEX dh_mix_a ON dh_mix USING lion (a);
INSERT INTO dh_mix SELECT g, ARRAY[md5(g::text) ||
		repeat('m', CASE WHEN g % 3 = 0 THEN 1 ELSE 2000 END)]
	FROM generate_series(1, 4000) g;
SELECT lion_index_verify('dh_mix_a');
SELECT * FROM dir_height_ok('dh_mix_a');

-- ---------------------------------------------------------------------
-- 2. The same keys built in bulk: the build writes each internal page's
--    first downlink as minus infinity too, and gives it two downlinks at
--    least.
-- ---------------------------------------------------------------------
REINDEX INDEX dh_arr_a;
SELECT lion_index_verify('dh_arr_a');
SELECT * FROM dir_height_ok('dh_arr_a');
CREATE INDEX dh_mix_b ON dh_mix USING lion (a) WITH (fillfactor = 10);
SELECT lion_index_verify('dh_mix_b');
SELECT * FROM dir_height_ok('dh_mix_b');
-- ... and inserts into a built index split its internal pages by the rule.
INSERT INTO dh_arr SELECT g, ARRAY[md5(g::text) || repeat('k', 2000)]
	FROM generate_series(4001, 6000) g;
SELECT lion_index_verify('dh_arr_a');
SELECT * FROM dir_height_ok('dh_arr_a');

-- ---------------------------------------------------------------------
-- 3. tsvector: every lexeme of 2044 bytes and more is filed under a hashed
--    key of the maximum size.
-- ---------------------------------------------------------------------
CREATE TABLE dh_tsv (id int, tsv tsvector);
CREATE INDEX dh_tsv_i ON dh_tsv USING lion (tsv);
INSERT INTO dh_tsv SELECT g, to_tsvector('simple', md5(g::text) || repeat('x', 2013))
	FROM generate_series(1, 3000) g;
SELECT lion_index_verify('dh_tsv_i');
SELECT * FROM dir_height_ok('dh_tsv_i');
SELECT count(*) FROM dh_tsv WHERE tsv @@ to_tsquery('simple', md5('5') || repeat('x', 2013));
VACUUM dh_tsv;

-- ---------------------------------------------------------------------
-- 4. jsonb: one document with many long top-level keys.
-- ---------------------------------------------------------------------
CREATE TABLE dh_doc (id int, doc jsonb);
CREATE INDEX dh_doc_i ON dh_doc USING lion (doc);
INSERT INTO dh_doc SELECT 1, jsonb_object_agg(md5(g::text) || repeat('k', 1968), g)
	FROM generate_series(1, 2000) g;
SELECT lion_index_verify('dh_doc_i');
SELECT * FROM dir_height_ok('dh_doc_i');
SELECT count(*) FROM dh_doc WHERE doc ? (md5('1234') || repeat('k', 1968));

DROP TABLE dh_arr, dh_desc, dh_asc, dh_mix, dh_tsv, dh_doc;
DROP FUNCTION dir_height_ok(regclass);
