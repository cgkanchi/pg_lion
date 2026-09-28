-- One heap column listed twice in a lion index, under a multi-key and a
-- scalar opclass (DESIGN.md §24): `(tags array_ops, tags <whole arrays>)`.
-- The two key columns hold different entries - the elements under array_ops,
-- the arrays themselves under the scalar class - and the planner picks the
-- one a use needs: the scalar column to walk for count(DISTINCT tags), the
-- multi-key one to answer `tags @> '{1}'`.  The executor used to take the
-- FIRST key column on the heap column whatever its opclass, so over the first
-- index count(DISTINCT tags) counted distinct elements, and over the second
-- the multi-key query went to the scalar column, which has no extraction
-- function to call (the 2026-09-27 review).  Each query is answered through
-- the pushdown, with every other scan disabled, and through a sequential scan
-- with the pushdown off, and the two answers must be equal; the indexes line
-- of EXPLAIN says which key column answered what.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

/* A scalar class over whole arrays: each distinct array is one entry. */
CREATE OPERATOR CLASS lion_dup_whole_ops FOR TYPE anyarray USING lion AS
	OPERATOR	1	= (anyarray, anyarray),
	FUNCTION	1	hash_array(anyarray);

/*
 * lion_dup() runs q through the pushdown with every other scan disabled, then
 * as a sequential scan with the pushdown off, and returns the pushdown's
 * answer - after proving the two equal.
 */
CREATE FUNCTION lion_dup(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	pushed boolean := false;
	got text;
	want text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			pushed := true;
		END IF;
	END LOOP;
	EXECUTE format('SELECT s::text FROM (%s) s', q) INTO got;

	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	EXECUTE format('SELECT s::text FROM (%s) s', q) INTO want;
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	IF got IS DISTINCT FROM want THEN
		RETURN format('MISMATCH: %s, expected %s', got, want);
	END IF;
	RETURN format('%s, %s',
				  CASE WHEN pushed THEN 'pushed down' ELSE 'not pushed down' END,
				  got);
END $$;

/* The Lion Indexes line of the pushdown's plan. */
CREATE FUNCTION lion_dup_indexes(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Lion Indexes:%' THEN
			RETURN btrim(ln);
		END IF;
	END LOOP;
	RETURN 'not pushed down';
END $$;

/*
 * 35 distinct arrays of two elements out of 12; `@> '{1}'` holds for the 140
 * rows whose first element is 1, which hold 7 distinct arrays.  The two tables
 * are the same rows under the two orders of the index's columns.
 */
CREATE TABLE lion_dup_ab (tags int[] NOT NULL);
INSERT INTO lion_dup_ab SELECT ARRAY[i % 5, 10 + i % 7] FROM generate_series(1, 700) i;
CREATE TABLE lion_dup_ba (tags int[] NOT NULL);
INSERT INTO lion_dup_ba SELECT tags FROM lion_dup_ab;
CREATE INDEX lion_dup_ab_i ON lion_dup_ab USING lion (tags array_ops, tags lion_dup_whole_ops);
CREATE INDEX lion_dup_ba_i ON lion_dup_ba USING lion (tags lion_dup_whole_ops, tags array_ops);
VACUUM ANALYZE lion_dup_ab;
VACUUM ANALYZE lion_dup_ba;

-- the multi-key column first: the arrays are its second column's entries
SELECT lion_dup('SELECT count(DISTINCT tags) FROM lion_dup_ab');
SELECT lion_dup($$SELECT count(*) FROM lion_dup_ab WHERE tags @> '{1}'$$);
SELECT lion_dup($$SELECT count(DISTINCT tags) FROM lion_dup_ab WHERE tags @> '{1}'$$);
SELECT lion_dup_indexes('SELECT count(DISTINCT tags) FROM lion_dup_ab');
SELECT lion_dup_indexes($$SELECT count(*) FROM lion_dup_ab WHERE tags @> '{1}'$$);
SELECT lion_dup_indexes($$SELECT count(DISTINCT tags) FROM lion_dup_ab WHERE tags @> '{1}'$$);

-- the scalar column first: the elements are its second column's entries
SELECT lion_dup('SELECT count(DISTINCT tags) FROM lion_dup_ba');
SELECT lion_dup($$SELECT count(*) FROM lion_dup_ba WHERE tags @> '{1}'$$);
SELECT lion_dup($$SELECT count(DISTINCT tags) FROM lion_dup_ba WHERE tags @> '{1}'$$);
SELECT lion_dup_indexes('SELECT count(DISTINCT tags) FROM lion_dup_ba');
SELECT lion_dup_indexes($$SELECT count(*) FROM lion_dup_ba WHERE tags @> '{1}'$$);
SELECT lion_dup_indexes($$SELECT count(DISTINCT tags) FROM lion_dup_ba WHERE tags @> '{1}'$$);

DROP TABLE lion_dup_ab, lion_dup_ba;
DROP FUNCTION lion_dup(text);
DROP FUNCTION lion_dup_indexes(text);
DROP OPERATOR FAMILY lion_dup_whole_ops USING lion;
