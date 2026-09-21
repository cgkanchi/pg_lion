-- IN lists / ScalarArrayOpExpr (DESIGN.md section 15).
--
-- `col = ANY (array)` reaches the access method as one SK_SEARCHARRAY scan
-- key (amsearcharray) and the count pushdown as one clause whose posting set
-- is the union of the listed values'.  Every query below is answered twice,
-- through the index and through a sequential scan, and the two results must
-- be equal as multisets.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'roaring_index';

/* VACUUM can only set all-visible for commits that reached disk (citext.sql). */
SET synchronous_commit = on;
SET default_statistics_target = 1000;

CREATE EXTENSION IF NOT EXISTS roaring_index;

/*
 * rbi_incmp() runs one query through the index (count pushdown enabled) and
 * through a plain sequential scan, and reports whether the custom node was
 * used and how many rows came out, after proving the two results match.
 */
CREATE FUNCTION rbi_incmp(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	pushed boolean := false;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('roaring_index.enable_count_pushdown', 'on', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (RoaringCount)%' THEN
			pushed := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE rbi_i_idx AS %s', q);

	PERFORM set_config('roaring_index.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	EXECUTE format('CREATE TEMP TABLE rbi_i_seq AS %s', q);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('roaring_index.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM rbi_i_idx' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM rbi_i_idx EXCEPT ALL SELECT * FROM rbi_i_seq) a)'
			' + (SELECT count(*) FROM (SELECT * FROM rbi_i_seq EXCEPT ALL SELECT * FROM rbi_i_idx) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE rbi_i_idx, rbi_i_seq';

	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN pushed THEN 'pushed down' ELSE 'index scan' END,
				  nrows);
END $$;

/* The plan of one query with the index preferred. */
CREATE FUNCTION rbi_inplan(q text) RETURNS SETOF text
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

/*
 * The literal text of an array of values lo .. hi taken modulo m, so that the
 * queries below hold a Const array - which is what the pushdown requires -
 * without spelling two hundred numbers out in the test file.
 */
CREATE FUNCTION rbi_inlist(lo int, hi int, m int) RETURNS text
LANGUAGE sql AS $$
	SELECT '{' || string_agg((g % m)::text, ',' ORDER BY g) || '}'
	  FROM generate_series(lo, hi) g
$$;

CREATE TABLE rbi_in (
	id	int		NOT NULL,
	a	int		NOT NULL,		-- 500 distinct values
	b	int		NOT NULL,		-- 7 distinct values
	c	text	NOT NULL,		-- 4 distinct values
	n	int						-- nullable, 5 values
);
INSERT INTO rbi_in
SELECT i, i % 500, i % 7, 'c' || (i % 4),
	   CASE WHEN i % 101 = 0 THEN NULL ELSE i % 5 END
  FROM generate_series(1, 100000) i;

CREATE INDEX rbi_in_a ON rbi_in USING roaring (a);
CREATE INDEX rbi_in_b ON rbi_in USING roaring (b);
CREATE INDEX rbi_in_c ON rbi_in USING roaring (c);
CREATE INDEX rbi_in_n ON rbi_in USING roaring (n);
VACUUM ANALYZE rbi_in;

SELECT roaring_index_verify('rbi_in_a', true);

-- ---- plans -------------------------------------------------------------
SELECT rbi_inplan('SELECT id FROM rbi_in WHERE a IN (1, 2, 3)');
SELECT rbi_inplan('SELECT count(*) FROM rbi_in WHERE a IN (1, 2, 3)');
SELECT rbi_inplan($$SELECT count(*) FROM rbi_in WHERE a IN (1, 2) AND c = 'c1'$$);
SELECT rbi_inplan('SELECT b, count(*) FROM rbi_in WHERE b IN (1, 2) GROUP BY b');
-- = ALL is not a union of keys and must not be pushed down
SELECT rbi_inplan('SELECT count(*) FROM rbi_in WHERE a = ALL (ARRAY[1, 2])');

-- ---- bitmap scans (amsearcharray) --------------------------------------
SET roaring_index.enable_count_pushdown = off;
SELECT rbi_incmp('SELECT id FROM rbi_in WHERE a IN (7)');
SELECT rbi_incmp('SELECT id FROM rbi_in WHERE a IN (7, 8, 9)');
SELECT rbi_incmp('SELECT id FROM rbi_in WHERE a IN (7, 7, 8, 8, 8)');
SELECT rbi_incmp('SELECT id FROM rbi_in WHERE a IN (7, NULL, 8)');
SELECT rbi_incmp('SELECT id FROM rbi_in WHERE a IN (NULL, NULL)');
SELECT rbi_incmp('SELECT id FROM rbi_in WHERE a = ANY (''{}''::int[])');
SELECT rbi_incmp('SELECT id FROM rbi_in WHERE a IN (900, 901)');
SELECT rbi_incmp('SELECT id FROM rbi_in WHERE a IN (7, 900)');
-- 200 values, and the same 200 with every one of them repeated
SELECT rbi_incmp(format('SELECT id FROM rbi_in WHERE a = ANY (''%s''::int[])', rbi_inlist(0, 199, 200)));
SELECT rbi_incmp(format('SELECT id FROM rbi_in WHERE a = ANY (''%s''::int[])', rbi_inlist(0, 399, 200)));
-- cross-type arrays
SELECT rbi_incmp('SELECT id FROM rbi_in WHERE a = ANY (''{7,8}''::int8[])');
SELECT rbi_incmp('SELECT id FROM rbi_in WHERE a = ANY (''{7,8}''::int2[])');
-- text, and a list combined with other quals
SELECT rbi_incmp($$SELECT id FROM rbi_in WHERE c IN ('c1', 'c3')$$);
SELECT rbi_incmp($$SELECT id FROM rbi_in WHERE a IN (7, 8) AND c IN ('c1', 'c3')$$);
SELECT rbi_incmp($$SELECT id FROM rbi_in WHERE a IN (7, 8) AND c = 'c3'$$);
-- a nullable column: NULL is never = ANY(...)
SELECT rbi_incmp('SELECT id FROM rbi_in WHERE n IN (1, 2)');
SELECT rbi_incmp('SELECT id FROM rbi_in WHERE n IN (1, 2) OR n IS NULL');
/*
 * Two quals on the one key column reach the access method as two scan keys.
 * It answers the most selective-looking one and marks every TID for recheck,
 * so the bitmap heap scan applies both quals and the rows are exact.
 */
SELECT rbi_incmp('SELECT id FROM rbi_in WHERE b IN (1, 2) AND b IN (2, 3)');
SELECT rbi_incmp('SELECT id FROM rbi_in WHERE b IN (1, 2) AND b = 2');
SELECT rbi_incmp('SELECT id FROM rbi_in WHERE n IN (1, 2) AND n IS NOT NULL');
SELECT rbi_incmp('SELECT id FROM rbi_in WHERE n IS NOT NULL AND n = 3');
SET roaring_index.enable_count_pushdown = on;

-- ---- the count pushdown ------------------------------------------------
SELECT rbi_incmp('SELECT count(*) FROM rbi_in WHERE a IN (7)');
SELECT rbi_incmp('SELECT count(*) FROM rbi_in WHERE a IN (7, 8, 9)');
SELECT rbi_incmp('SELECT count(*) FROM rbi_in WHERE a IN (7, 7, 8, 8, 8)');
SELECT rbi_incmp('SELECT count(*) FROM rbi_in WHERE a IN (7, NULL, 8)');
SELECT rbi_incmp('SELECT count(*) FROM rbi_in WHERE a IN (NULL, NULL)');
SELECT rbi_incmp('SELECT count(*) FROM rbi_in WHERE a = ANY (''{}''::int[])');
SELECT rbi_incmp('SELECT count(*) FROM rbi_in WHERE a IN (900, 901)');
SELECT rbi_incmp('SELECT count(*) FROM rbi_in WHERE a IN (7, 900)');
SELECT rbi_incmp(format('SELECT count(*) FROM rbi_in WHERE a = ANY (''%s''::int[])', rbi_inlist(0, 199, 200)));
SELECT rbi_incmp(format('SELECT count(*) FROM rbi_in WHERE a = ANY (''%s''::int[])', rbi_inlist(0, 399, 200)));
SELECT rbi_incmp('SELECT count(*) FROM rbi_in WHERE a = ANY (''{7,8}''::int8[])');
SELECT rbi_incmp('SELECT count(*) FROM rbi_in WHERE a = ANY (''{7,8}''::int2[])');
SELECT rbi_incmp($$SELECT count(*) FROM rbi_in WHERE c IN ('c1', 'c3')$$);
SELECT rbi_incmp($$SELECT count(*) FROM rbi_in WHERE b IN (1, 2) AND c IN ('c1', 'c3')$$);
SELECT rbi_incmp($$SELECT count(*) FROM rbi_in WHERE b IN (1, 2) AND c = 'c3'$$);
SELECT rbi_incmp('SELECT count(*) FROM rbi_in WHERE n IN (1, 2)');
-- an IN list and a null test on the same column
SELECT rbi_incmp('SELECT count(*) FROM rbi_in WHERE n IN (1, 2) AND n IS NOT NULL');
-- the same list twice is a duplicate clause, two different ones are not
SELECT rbi_incmp('SELECT count(*) FROM rbi_in WHERE b IN (1, 2) AND b IN (1, 2)');
SELECT rbi_incmp('SELECT count(*) FROM rbi_in WHERE b IN (1, 2) AND b IN (2, 3)');
-- = ALL is answered by the ordinary plan
SELECT rbi_incmp('SELECT count(*) FROM rbi_in WHERE a = ALL (ARRAY[1, 2])');
SELECT rbi_incmp('SELECT count(*) FROM rbi_in WHERE a = ALL (ARRAY[1])');

-- ---- GROUP BY over the listed values -----------------------------------
SELECT rbi_incmp('SELECT b, count(*) FROM rbi_in WHERE b IN (1, 2) GROUP BY b');
SELECT rbi_incmp($$SELECT b, count(*) FROM rbi_in WHERE b IN (1, 2) AND c IN ('c1', 'c3') GROUP BY b$$);
SELECT rbi_incmp('SELECT b, count(*) FROM rbi_in WHERE b IN (900, 901) GROUP BY b');
SELECT rbi_incmp('SELECT b, count(*) FROM rbi_in WHERE b = ANY (''{}''::int[]) GROUP BY b');
SELECT rbi_incmp('SELECT n, count(*) FROM rbi_in WHERE n IN (1, 2) GROUP BY n');
SELECT rbi_incmp($$SELECT c, count(*) FROM rbi_in WHERE b IN (1, 2) GROUP BY c$$);
SELECT rbi_incmp('SELECT count(b) FROM rbi_in WHERE b IN (1, 2)');

-- the results themselves
SELECT b, count(*) FROM rbi_in WHERE b IN (1, 2) GROUP BY b ORDER BY b;
SELECT count(*) FROM rbi_in WHERE a IN (7, 8, 9);
SELECT count(*) FROM rbi_in WHERE a IN (7, NULL, 8);
SELECT count(*) FROM rbi_in WHERE a = ANY ('{}'::int[]);

-- ---- a dirty heap (the recheck path) -----------------------------------
DELETE FROM rbi_in WHERE id % 3 = 0;
SELECT roaring_index_verify('rbi_in_a', true);
SET roaring_index.enable_count_pushdown = off;
SELECT rbi_incmp('SELECT id FROM rbi_in WHERE a IN (7, 8, 9)');
SET roaring_index.enable_count_pushdown = on;
SELECT rbi_incmp('SELECT count(*) FROM rbi_in WHERE a IN (7, 8, 9)');
SELECT rbi_incmp('SELECT count(*) FROM rbi_in WHERE b IN (1, 2)');
SELECT rbi_incmp('SELECT b, count(*) FROM rbi_in WHERE b IN (1, 2) GROUP BY b');

-- ---- and after VACUUM (the visibility-map path) ------------------------
VACUUM rbi_in;
SELECT roaring_index_verify('rbi_in_a', true);
SET roaring_index.enable_count_pushdown = off;
SELECT rbi_incmp('SELECT id FROM rbi_in WHERE a IN (7, 8, 9)');
SET roaring_index.enable_count_pushdown = on;
SELECT rbi_incmp('SELECT count(*) FROM rbi_in WHERE a IN (7, 8, 9)');
SELECT rbi_incmp('SELECT count(*) FROM rbi_in WHERE b IN (1, 2)');
SELECT rbi_incmp('SELECT b, count(*) FROM rbi_in WHERE b IN (1, 2) GROUP BY b');
SELECT rbi_incmp($$SELECT count(*) FROM rbi_in WHERE b IN (1, 2) AND c IN ('c1', 'c3')$$);

-- ---- a list longer than the pushdown accepts ---------------------------
/*
 * Above RBI_MAX_ARRAY_ELEMS (1000) values the pushdown declines - every
 * listed value would need a posting set of its own, each holding a pin - but
 * the bitmap scan still answers the query, and both must agree with the heap.
 */
SELECT rbi_incmp(format('SELECT count(*) FROM rbi_in WHERE a = ANY (''%s''::int[])', rbi_inlist(0, 1200, 100000)));
SELECT count(*) FROM rbi_in WHERE a < 1201;

/*
 * The union itself: roaring_index_count_any() (DESIGN.md section 15).
 *
 * The pushdown's IN list and this function share the whole evaluation - the
 * values are located by rbi_posting_set_lookup_many(), which hashes them all
 * and then visits the bucket pages in (bucket, hash) order, and the union is
 * the k-way merge over the leaf cursors, ORed through a bitset image once
 * more than a handful of them stand at one container key.  Going through SQL
 * reaches the shapes the pushdown declines: a list longer than
 * RBI_MAX_ARRAY_ELEMS, and lists whose duplicates and NULLs are not constant
 * folded away.
 */
CREATE FUNCTION rbi_anycmp(idx text, tbl text, col text, arr text)
RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	a bigint;
	b bigint;
BEGIN
	EXECUTE format('SELECT roaring_index_count_any(%L::regclass, %s)', idx, arr)
		INTO a;
	EXECUTE format('SELECT count(*) FROM %s WHERE %I = ANY (%s)', tbl, col, arr)
		INTO b;
	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH roaring=%s select=%s', a, b);
	END IF;
	RETURN format('ok %s', a);
END $$;

SELECT rbi_anycmp('rbi_in_a', 'rbi_in', 'a', '''{7}''::int[]');
SELECT rbi_anycmp('rbi_in_a', 'rbi_in', 'a', '''{7,8,9}''::int[]');
-- duplicates: a union of a set with itself is that set
SELECT rbi_anycmp('rbi_in_a', 'rbi_in', 'a', '''{7,7,7,8,8,9}''::int[]');
-- NULLs are not = ANY(...), and an all-NULL or empty list selects nothing
SELECT rbi_anycmp('rbi_in_a', 'rbi_in', 'a', '''{7,NULL,8}''::int[]');
SELECT rbi_anycmp('rbi_in_a', 'rbi_in', 'a', '''{NULL,NULL}''::int[]');
SELECT rbi_anycmp('rbi_in_a', 'rbi_in', 'a', '''{}''::int[]');
-- values with no entry at all, alone and mixed in
SELECT rbi_anycmp('rbi_in_a', 'rbi_in', 'a', '''{900,901}''::int[]');
SELECT rbi_anycmp('rbi_in_a', 'rbi_in', 'a', '''{7,900,8,901}''::int[]');
-- every key of the index, which is every row: the union is the whole table
SELECT rbi_anycmp('rbi_in_a', 'rbi_in', 'a',
				  format('%L::int[]', rbi_inlist(0, 499, 500)));
-- ... and the same list with every value repeated four times, plus absent ones
SELECT rbi_anycmp('rbi_in_a', 'rbi_in', 'a',
				  format('%L::int[]', rbi_inlist(0, 2499, 625)));
-- a 3000-value list: longer than the pushdown accepts, and exact here
SELECT rbi_anycmp('rbi_in_a', 'rbi_in', 'a',
				  format('%L::int[]', rbi_inlist(0, 2999, 100000)));
-- by-reference keys, a two-value list, and cross-type element types
SELECT rbi_anycmp('rbi_in_c', 'rbi_in', 'c', '''{c1,c3}''::text[]');
SELECT rbi_anycmp('rbi_in_b', 'rbi_in', 'b', '''{1,2}''::int8[]');
SELECT rbi_anycmp('rbi_in_b', 'rbi_in', 'b', '''{1,2}''::int2[]');
-- a nullable column: the reserved NULL entry is not a listed value
SELECT rbi_anycmp('rbi_in_n', 'rbi_in', 'n', '''{1,2}''::int[]');
SELECT rbi_anycmp('rbi_in_n', 'rbi_in', 'n', '''{1,2,3,4,5,6}''::int[]');
-- the same errors the single-key count raises
SELECT roaring_index_count_any('rbi_in_a', '{1}'::text[]);
SELECT roaring_index_count_any('rbi_in', '{1}'::int[]);
SELECT roaring_index_count_any('rbi_in_a', NULL::int[]) IS NULL AS null_array;

-- ---- an empty table ----------------------------------------------------
CREATE TABLE rbi_in_empty (k int NOT NULL);
CREATE INDEX rbi_in_empty_k ON rbi_in_empty USING roaring (k);
VACUUM ANALYZE rbi_in_empty;
SELECT rbi_incmp('SELECT count(*) FROM rbi_in_empty WHERE k IN (1, 2)');
SELECT rbi_incmp('SELECT k, count(*) FROM rbi_in_empty WHERE k IN (1, 2) GROUP BY k');

DROP TABLE rbi_in, rbi_in_empty;
DROP FUNCTION rbi_incmp(text);
DROP FUNCTION rbi_anycmp(text, text, text, text);
DROP FUNCTION rbi_inlist(int, int, int);
DROP FUNCTION rbi_inplan(text);
