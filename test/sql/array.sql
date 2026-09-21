-- Multi-key operator classes: arrays (DESIGN.md section 17).
--
-- `array_ops` indexes one row under every distinct element of its array, so
-- `@>` is the intersection of the elements' posting sets, `&&` their union,
-- and `<@` cannot be answered from them at all.  Every query below is run
-- through the index and through a forced sequential scan and the two results
-- are compared as multisets, so the access method's own answers are checked
-- and not just the pushdown's.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'roaring_index';

/* VACUUM can only set all-visible for commits that reached disk (citext.sql). */
SET synchronous_commit = on;
SET default_statistics_target = 1000;

CREATE EXTENSION IF NOT EXISTS roaring_index;

CREATE DOMAIN tarr AS text[];

/*
 * Run one query through the index (count pushdown enabled, seqscan off) and
 * through a plain sequential scan, prove the two results are equal as
 * multisets, and report which node answered the first one.
 */
CREATE FUNCTION rbi_arrcmp(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	node text := 'seq scan';
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('roaring_index.enable_count_pushdown', 'on', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (RoaringCount)%' THEN
			node := 'pushed down';
		ELSIF ln LIKE '%Bitmap Index Scan%' THEN
			node := 'index scan';
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE rbi_a_idx AS %s', q);

	PERFORM set_config('roaring_index.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	EXECUTE format('CREATE TEMP TABLE rbi_a_seq AS %s', q);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);

	EXECUTE 'SELECT count(*) FROM rbi_a_idx' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM rbi_a_idx EXCEPT ALL SELECT * FROM rbi_a_seq) a)'
			' + (SELECT count(*) FROM (SELECT * FROM rbi_a_seq EXCEPT ALL SELECT * FROM rbi_a_idx) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE rbi_a_idx, rbi_a_seq';

	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows', node, nrows);
END $$;

/* The plan of one query with the index preferred. */
CREATE FUNCTION rbi_arrplan(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		RETURN NEXT ln;
	END LOOP;
	PERFORM set_config('enable_seqscan', 'on', true);
END $$;

-- ---- the table ---------------------------------------------------------
--
-- tags: 1 to 3 of 40 text tags, with duplicates on purpose (row i holds
-- t(i%40) twice whenever i%7 = 0) and a NULL element every 53rd row.
-- nums: two int elements.  Row 0 has empty arrays, row -1 NULL ones.
CREATE TABLE rbi_arr (
	id		int			NOT NULL,
	grp		int			NOT NULL,	-- 8 groups, scalar roaring index
	tags	text[],
	nums	int[]
);

INSERT INTO rbi_arr
SELECT i, i % 8,
	   CASE WHEN i % 53 = 0
			THEN ARRAY['t' || (i % 40), NULL, 't' || (i % 13)]
			WHEN i % 7 = 0
			THEN ARRAY['t' || (i % 40), 't' || (i % 40), 't' || (i % 13)]
			ELSE ARRAY['t' || (i % 40), 't' || (i % 13), 't' || (i % 3)]
	   END,
	   ARRAY[i % 11, i % 5]
  FROM generate_series(1, 20000) i;
INSERT INTO rbi_arr VALUES (0, 0, '{}', '{}');
INSERT INTO rbi_arr VALUES (-1, 1, NULL, NULL);

CREATE INDEX rbi_arr_tags ON rbi_arr USING roaring (tags);
CREATE INDEX rbi_arr_nums ON rbi_arr USING roaring (nums);
CREATE INDEX rbi_arr_grp ON rbi_arr USING roaring (grp);
VACUUM ANALYZE rbi_arr;

-- The opclasses validate.
SELECT amvalidate(oid) FROM pg_opclass
 WHERE opcmethod = (SELECT oid FROM pg_am WHERE amname = 'roaring')
   AND opcname IN ('array_ops', 'tsvector_ops')
 ORDER BY opcname;

-- The index stores ELEMENTS, so its key column is the element type.
SELECT a.attname, a.atttypid::regtype
  FROM pg_attribute a
 WHERE a.attrelid = 'rbi_arr_tags'::regclass AND a.attnum > 0;

SELECT roaring_index_verify('rbi_arr_tags', true);
SELECT roaring_index_verify('rbi_arr_nums', true);

-- 40 tag entries + 13 + 3 overlap; the NULL array is one entry, the empty
-- array another.  empty_tids counts the rows no key was extracted from.
SELECT entries, null_tids, empty_tids FROM roaring_index_stats('rbi_arr_tags');
SELECT entries, null_tids, empty_tids FROM roaring_index_stats('rbi_arr_nums');

-- roaring_index_count() is a single-key function: a multi-key index has no
-- equality strategy to look a value up with, and says so.
SELECT roaring_index_count('rbi_arr_tags', '{t5}'::text[]);

-- ---- plans -------------------------------------------------------------
SELECT rbi_arrplan($$SELECT id FROM rbi_arr WHERE tags @> '{t5}'$$);
SELECT rbi_arrplan($$SELECT count(*) FROM rbi_arr WHERE tags @> '{t5,t7}'$$);
SELECT rbi_arrplan($$SELECT count(*) FROM rbi_arr WHERE tags && '{t5,t7}'$$);
-- `<@` needs every row rechecked, so it is never pushed down
SELECT rbi_arrplan($$SELECT count(*) FROM rbi_arr WHERE tags <@ '{t5,t7}'$$);
-- `@> '{}'` is every non-NULL row: an ALL-mode scan, also not pushed down
SELECT rbi_arrplan($$SELECT count(*) FROM rbi_arr WHERE tags @> '{}'$$);
SELECT rbi_arrplan($$SELECT grp, count(*) FROM rbi_arr
					 WHERE tags @> '{t5}' GROUP BY grp$$);
SELECT rbi_arrplan($$SELECT count(*) FROM rbi_arr
					 WHERE tags @> '{t5}' AND nums && '{3}'$$);

-- ---- @> with one, two and three elements -------------------------------
SELECT rbi_arrcmp($$SELECT id FROM rbi_arr WHERE tags @> '{t5}'$$);
SELECT rbi_arrcmp($$SELECT id FROM rbi_arr WHERE tags @> '{t5,t5}'$$);
SELECT rbi_arrcmp($$SELECT id FROM rbi_arr WHERE tags @> '{t5,t2}'$$);
SELECT rbi_arrcmp($$SELECT id FROM rbi_arr WHERE tags @> '{t5,t2,t0}'$$);
SELECT rbi_arrcmp($$SELECT id FROM rbi_arr WHERE tags @> '{t5,nosuchtag}'$$);
SELECT rbi_arrcmp($$SELECT id FROM rbi_arr WHERE nums @> '{3,4}'$$);

-- ---- && ----------------------------------------------------------------
SELECT rbi_arrcmp($$SELECT id FROM rbi_arr WHERE tags && '{t5}'$$);
SELECT rbi_arrcmp($$SELECT id FROM rbi_arr WHERE tags && '{t5,t2}'$$);
SELECT rbi_arrcmp($$SELECT id FROM rbi_arr WHERE tags && '{nosuchtag,t39}'$$);
SELECT rbi_arrcmp($$SELECT id FROM rbi_arr WHERE tags && '{nosuchtag}'$$);

-- ---- two index quals on one column --------------------------------------
-- The access method answers ONE qual per scan and marks every TID for
-- recheck, so the bitmap heap scan applies the rest (DESIGN.md §5).
SELECT rbi_arrcmp($$SELECT id FROM rbi_arr
				   WHERE tags @> '{t5}' AND tags && '{t2,t7}'$$);
SELECT rbi_arrcmp($$SELECT id FROM rbi_arr
				   WHERE tags @> '{t5}' AND tags @> '{t2}'$$);

-- ---- <@ (recheck) ------------------------------------------------------
SELECT rbi_arrcmp($$SELECT id FROM rbi_arr WHERE tags <@ '{t5,t2,t0}'$$);
SELECT rbi_arrcmp($$SELECT id FROM rbi_arr WHERE nums <@ '{0,1,2,3,4}'$$);

-- ---- empty query arrays ------------------------------------------------
-- `@> '{}'` is true of every non-NULL array, empty ones included.
SELECT rbi_arrcmp($$SELECT id FROM rbi_arr WHERE tags @> '{}'$$);
SELECT rbi_arrcmp($$SELECT count(*) FROM rbi_arr WHERE tags @> '{}'$$);
-- `&& '{}'` is true of nothing.
SELECT rbi_arrcmp($$SELECT id FROM rbi_arr WHERE tags && '{}'$$);

-- ---- NULLs on both sides -----------------------------------------------
-- A NULL element in the QUERY: `@>` is then false everywhere, `&&` ignores it.
SELECT rbi_arrcmp($$SELECT id FROM rbi_arr WHERE tags @> ARRAY['t5', NULL]$$);
SELECT rbi_arrcmp($$SELECT id FROM rbi_arr WHERE tags && ARRAY['t5', NULL]$$);
SELECT rbi_arrcmp($$SELECT id FROM rbi_arr WHERE tags @> ARRAY[NULL]::text[]$$);
-- A NULL element in the indexed VALUE: the row is indexed under its others.
SELECT rbi_arrcmp($$SELECT id FROM rbi_arr WHERE tags @> '{t1}' AND id % 53 = 0$$);
-- A NULL array value itself.
SELECT rbi_arrcmp($$SELECT id FROM rbi_arr WHERE tags IS NULL$$);
SELECT rbi_arrcmp($$SELECT id FROM rbi_arr WHERE tags IS NOT NULL$$);
SELECT rbi_arrcmp($$SELECT count(*) FROM rbi_arr WHERE tags IS NULL$$);

-- ---- the count pushdown ------------------------------------------------
SELECT rbi_arrcmp($$SELECT count(*) FROM rbi_arr WHERE tags @> '{t5,t2}'$$);
SELECT rbi_arrcmp($$SELECT count(*) FROM rbi_arr WHERE tags && '{t5,t2}'$$);
SELECT rbi_arrcmp($$SELECT count(*) FROM rbi_arr
				   WHERE tags @> '{t5}' AND tags && '{t2,t7}'$$);
SELECT rbi_arrcmp($$SELECT count(*) FROM rbi_arr
				   WHERE tags @> '{t5}' AND nums @> '{3}'$$);
SELECT rbi_arrcmp($$SELECT count(*) FROM rbi_arr
				   WHERE tags @> '{t5}' AND grp = 3$$);
-- a key that does not exist: no rows, and no merge to run
SELECT rbi_arrcmp($$SELECT count(*) FROM rbi_arr WHERE tags @> '{t5,nosuchtag}'$$);

-- the counts themselves (rbi_arrcmp only proves the two plans agree)
SELECT count(*) FROM rbi_arr WHERE tags @> '{t5,t2}';
SELECT count(*) FROM rbi_arr WHERE tags && '{t5,t2}';
SELECT count(*) FROM rbi_arr WHERE tags @> '{t5}' AND tags && '{t2,t7}';
SELECT count(*) FROM rbi_arr WHERE tags @> '{t5,nosuchtag}';
SELECT count(*) FROM rbi_arr WHERE tags @> '{}';

-- ---- GROUP BY a scalar column with a multi-key WHERE clause ------------
SELECT rbi_arrcmp($$SELECT grp, count(*) FROM rbi_arr
				   WHERE tags @> '{t5}' GROUP BY grp$$);
SELECT rbi_arrcmp($$SELECT grp, count(*) FROM rbi_arr
				   WHERE tags && '{t5,t2}' GROUP BY grp$$);
SELECT rbi_arrcmp($$SELECT grp, count(*) FROM rbi_arr
				   WHERE tags @> '{t5}' AND nums @> '{3}' GROUP BY grp$$);
-- the real answer, for comparison
SELECT grp, count(*) FROM rbi_arr WHERE tags @> '{t5}' GROUP BY grp ORDER BY grp;

-- ---- writes ------------------------------------------------------------
INSERT INTO rbi_arr
SELECT i, i % 8, ARRAY['t' || (i % 40), 'new' || (i % 3)], ARRAY[i % 11, 99]
  FROM generate_series(20001, 21000) i;
INSERT INTO rbi_arr VALUES (30000, 2, '{}', NULL);
INSERT INTO rbi_arr VALUES (30001, 2, ARRAY[NULL]::text[], '{7}');

SELECT roaring_index_verify('rbi_arr_tags', true);
SELECT roaring_index_verify('rbi_arr_nums', true);
SELECT entries, null_tids, empty_tids FROM roaring_index_stats('rbi_arr_tags');

-- a dirty heap: the count has to visit it
SELECT rbi_arrcmp($$SELECT count(*) FROM rbi_arr WHERE tags @> '{new1}'$$);
SELECT rbi_arrcmp($$SELECT id FROM rbi_arr WHERE tags @> '{t5,new1}'$$);
SELECT rbi_arrcmp($$SELECT id FROM rbi_arr WHERE tags @> '{}' AND id > 29999$$);

UPDATE rbi_arr SET tags = ARRAY['upd', 't5'] WHERE id BETWEEN 100 AND 200;
DELETE FROM rbi_arr WHERE id BETWEEN 300 AND 400;

SELECT roaring_index_verify('rbi_arr_tags', true);
SELECT rbi_arrcmp($$SELECT id FROM rbi_arr WHERE tags @> '{upd}'$$);
SELECT rbi_arrcmp($$SELECT count(*) FROM rbi_arr WHERE tags @> '{upd,t5}'$$);
SELECT rbi_arrcmp($$SELECT count(*) FROM rbi_arr WHERE tags @> '{t5}'$$);

VACUUM rbi_arr;
SELECT roaring_index_verify('rbi_arr_tags', true);
SELECT roaring_index_verify('rbi_arr_nums', true);

-- an all-visible heap: the count comes out of the visibility map
VACUUM ANALYZE rbi_arr;
SELECT rbi_arrcmp($$SELECT count(*) FROM rbi_arr WHERE tags @> '{upd,t5}'$$);
SELECT rbi_arrcmp($$SELECT count(*) FROM rbi_arr WHERE tags @> '{t5}'$$);
SELECT rbi_arrcmp($$SELECT count(*) FROM rbi_arr WHERE tags && '{upd,new1}'$$);
SELECT rbi_arrcmp($$SELECT grp, count(*) FROM rbi_arr
				   WHERE tags @> '{t5}' GROUP BY grp$$);

-- ---- a partitioned table (DESIGN.md §16 meets §17) ---------------------
CREATE TABLE rbi_arr_part (id int, grp int, tags text[]) PARTITION BY RANGE (id);
CREATE TABLE rbi_arr_p1 PARTITION OF rbi_arr_part FOR VALUES FROM (0) TO (2000);
CREATE TABLE rbi_arr_p2 PARTITION OF rbi_arr_part FOR VALUES FROM (2000) TO (4000);
INSERT INTO rbi_arr_part
SELECT i, i % 8, ARRAY['t' || (i % 40), 't' || (i % 13)]
  FROM generate_series(0, 3999) i;
CREATE INDEX ON rbi_arr_p1 USING roaring (tags);
CREATE INDEX ON rbi_arr_p2 USING roaring (tags);
CREATE INDEX ON rbi_arr_p1 USING roaring (grp);
CREATE INDEX ON rbi_arr_p2 USING roaring (grp);
VACUUM ANALYZE rbi_arr_part;

SELECT rbi_arrplan($$SELECT count(*) FROM rbi_arr_part WHERE tags @> '{t5,t2}'$$);
SELECT rbi_arrcmp($$SELECT count(*) FROM rbi_arr_part WHERE tags @> '{t5,t2}'$$);
SELECT rbi_arrcmp($$SELECT grp, count(*) FROM rbi_arr_part
				   WHERE tags && '{t5,t2}' GROUP BY grp$$);
SELECT count(*) FROM rbi_arr_part WHERE tags @> '{t5,t2}';
DROP TABLE rbi_arr_part;

-- ---- element types the keys cannot be made from ------------------------
-- The element type needs a hash opclass, and says so when it has none.
CREATE TABLE rbi_arr_odd (p point[], m int[][], d tarr);
CREATE INDEX ON rbi_arr_odd USING roaring (p);

-- A multi-dimensional array is just its elements, and a domain its base type.
CREATE INDEX rbi_arr_odd_m ON rbi_arr_odd USING roaring (m);
CREATE INDEX rbi_arr_odd_d ON rbi_arr_odd USING roaring (d);
INSERT INTO rbi_arr_odd (m, d)
	VALUES ('{{1,2},{3,4}}', '{a,b}'), ('{{5,6},{1,9}}', '{b,c}');
SELECT roaring_index_verify('rbi_arr_odd_m', true);
SELECT roaring_index_verify('rbi_arr_odd_d', true);
SELECT rbi_arrcmp($$SELECT m FROM rbi_arr_odd WHERE m @> '{1}'$$);
SELECT rbi_arrcmp($$SELECT m FROM rbi_arr_odd WHERE m @> '{{1,2}}'$$);
SELECT rbi_arrcmp($$SELECT d FROM rbi_arr_odd WHERE d @> '{b}'$$);
DROP TABLE rbi_arr_odd;

-- ---- max_entries, the cardinality guard --------------------------------
CREATE TABLE rbi_arr_many (a text[]);
INSERT INTO rbi_arr_many
SELECT ARRAY['k' || i, 'k' || (i + 1)] FROM generate_series(1, 2000) i;

-- the build counts the distinct keys exactly, so the warning is exact
SET client_min_messages = warning;
CREATE INDEX rbi_arr_many_a ON rbi_arr_many USING roaring (a)
	WITH (max_entries = 100);
-- once per backend per index: the insert below stays quiet
INSERT INTO rbi_arr_many VALUES (ARRAY['brand', 'new']);
SELECT entries > 100 FROM roaring_index_stats('rbi_arr_many_a');

-- a fresh index in a fresh backend warns again, this time from an insert
DROP INDEX rbi_arr_many_a;
\connect -
SET client_min_messages = warning;
SET synchronous_commit = on;
CREATE INDEX rbi_arr_many_b ON rbi_arr_many USING roaring (a)
	WITH (max_entries = 1000000);
ALTER INDEX rbi_arr_many_b SET (max_entries = 50);
INSERT INTO rbi_arr_many SELECT ARRAY['z' || i] FROM generate_series(1, 200) i;
SELECT roaring_index_verify('rbi_arr_many_b', true);

DROP TABLE rbi_arr_many;
DROP TABLE rbi_arr;
DROP FUNCTION rbi_arrcmp(text);
DROP FUNCTION rbi_arrplan(text);
DROP DOMAIN tarr;
