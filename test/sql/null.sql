-- NULL keys (DESIGN.md section 14).
--
-- A roaring index keeps the rows whose key is NULL in one reserved entry of
-- bucket 0.  Everything below checks the same thing twice: that the index
-- path (bitmap scan, or the count pushdown) agrees with a plain sequential
-- scan, and that roaring_index_verify() is happy after every phase.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'roaring_index';

/*
 * The count pushdown only skips the heap for pages the visibility map
 * vouches for, and VACUUM can only set that bit once the inserting
 * transaction's commit record is on disk.  The dev cluster runs with
 * synchronous_commit = off, so ask for flushed commits before the data is
 * loaded (see citext.sql).
 */
SET synchronous_commit = on;
SET default_statistics_target = 1000;

CREATE EXTENSION IF NOT EXISTS roaring_index;

/*
 * rbi_nullcmp() runs one query two ways - through the index (with the count
 * pushdown enabled) and through a plain sequential scan - and reports whether
 * the custom node was used and how many rows came out, after proving the two
 * result sets are equal as multisets.
 */
CREATE FUNCTION rbi_nullcmp(q text) RETURNS text
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
	EXECUTE format('CREATE TEMP TABLE rbi_n_idx AS %s', q);

	PERFORM set_config('roaring_index.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	EXECUTE format('CREATE TEMP TABLE rbi_n_seq AS %s', q);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('roaring_index.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM rbi_n_idx' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM rbi_n_idx EXCEPT ALL SELECT * FROM rbi_n_seq) a)'
			' + (SELECT count(*) FROM (SELECT * FROM rbi_n_seq EXCEPT ALL SELECT * FROM rbi_n_idx) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE rbi_n_idx, rbi_n_seq';

	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN pushed THEN 'pushed down' ELSE 'index scan' END,
				  nrows);
END $$;

/* The plan of one query, with the pushdown on. */
CREATE FUNCTION rbi_nullplan(q text) RETURNS SETOF text
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

CREATE TABLE rbi_null (
	id	int	NOT NULL,
	k	int,			-- one row in seven is NULL
	t	text,			-- one row in eleven is NULL
	d	int				-- never NULL, for the intersections
);
INSERT INTO rbi_null
SELECT i,
	   CASE WHEN i % 7 = 0 THEN NULL ELSE i % 5 END,
	   CASE WHEN i % 11 = 0 THEN NULL ELSE 'v' || (i % 3) END,
	   i % 4
  FROM generate_series(1, 50000) i;

CREATE INDEX rbi_null_k ON rbi_null USING roaring (k);
CREATE INDEX rbi_null_t ON rbi_null USING roaring (t);
CREATE INDEX rbi_null_d ON rbi_null USING roaring (d);
VACUUM ANALYZE rbi_null;

-- ---- what the index holds ----------------------------------------------
SELECT roaring_index_verify('rbi_null_k', true);
SELECT roaring_index_verify('rbi_null_t', true);

-- one entry per key plus the reserved NULL entry
SELECT entries, ntids, null_tids FROM roaring_index_stats('rbi_null_k');
SELECT entries, ntids, null_tids FROM roaring_index_stats('rbi_null_t');
-- a column without NULLs has no null entry at all
SELECT entries, ntids, null_tids FROM roaring_index_stats('rbi_null_d');

-- the counts the heap really has
SELECT count(*) AS total,
	   count(k) AS k_not_null,
	   count(*) - count(k) AS k_null,
	   count(t) AS t_not_null
  FROM rbi_null;

-- ---- bitmap scans (amsearchnulls) --------------------------------------
SELECT rbi_nullplan('SELECT id FROM rbi_null WHERE k IS NULL');
SELECT rbi_nullplan('SELECT id FROM rbi_null WHERE k IS NOT NULL');
SELECT rbi_nullplan($$SELECT id FROM rbi_null WHERE t IS NULL$$);

SET roaring_index.enable_count_pushdown = off;
SELECT rbi_nullcmp('SELECT id FROM rbi_null WHERE k IS NULL');
SELECT rbi_nullcmp('SELECT id FROM rbi_null WHERE k IS NOT NULL');
SELECT rbi_nullcmp('SELECT id FROM rbi_null WHERE t IS NULL');
SELECT rbi_nullcmp('SELECT id FROM rbi_null WHERE t IS NOT NULL');
SELECT rbi_nullcmp('SELECT id FROM rbi_null WHERE k IS NULL AND t IS NULL');
SELECT rbi_nullcmp('SELECT id FROM rbi_null WHERE k IS NULL AND t = ''v1''');
SELECT rbi_nullcmp('SELECT id FROM rbi_null WHERE k = 3 AND t IS NULL');
-- a NULL comparison value is not a null test and matches nothing
SELECT rbi_nullcmp('SELECT id FROM rbi_null WHERE k = NULL');
SET roaring_index.enable_count_pushdown = on;

-- ---- the count pushdown ------------------------------------------------
SELECT rbi_nullplan('SELECT count(*) FROM rbi_null WHERE k IS NULL');
SELECT rbi_nullplan('SELECT count(*) FROM rbi_null WHERE k IS NOT NULL');
SELECT rbi_nullplan('SELECT k, count(*) FROM rbi_null GROUP BY k');
SELECT rbi_nullplan('SELECT k, count(k) FROM rbi_null GROUP BY k');

SELECT rbi_nullcmp('SELECT count(*) FROM rbi_null WHERE k IS NULL');
SELECT rbi_nullcmp('SELECT count(*) FROM rbi_null WHERE k IS NOT NULL');
SELECT rbi_nullcmp('SELECT count(*) FROM rbi_null WHERE t IS NULL');
SELECT rbi_nullcmp('SELECT count(*) FROM rbi_null WHERE t IS NOT NULL');
SELECT rbi_nullcmp('SELECT count(*) FROM rbi_null WHERE k IS NULL AND t IS NULL');
SELECT rbi_nullcmp('SELECT count(*) FROM rbi_null WHERE k IS NOT NULL AND t IS NOT NULL');
SELECT rbi_nullcmp('SELECT count(*) FROM rbi_null WHERE k IS NULL AND d = 2');
SELECT rbi_nullcmp('SELECT count(*) FROM rbi_null WHERE k IS NOT NULL AND d = 2');
SELECT rbi_nullcmp('SELECT count(*) FROM rbi_null WHERE k = 3 AND t IS NOT NULL');
-- a contradiction still has to come out as zero
SELECT rbi_nullcmp('SELECT count(*) FROM rbi_null WHERE k IS NULL AND k IS NOT NULL');
-- ... and a redundant null test must not change anything
SELECT rbi_nullcmp('SELECT count(*) FROM rbi_null WHERE k = 3 AND k IS NOT NULL');

-- GROUP BY a nullable column: the NULL group comes from the null entry
SELECT rbi_nullcmp('SELECT k, count(*) FROM rbi_null GROUP BY k');
SELECT rbi_nullcmp('SELECT t, count(*) FROM rbi_null GROUP BY t');
SELECT rbi_nullcmp('SELECT k, count(*) FROM rbi_null WHERE d = 1 GROUP BY k');
SELECT rbi_nullcmp('SELECT k, count(*) FROM rbi_null WHERE t IS NULL GROUP BY k');
SELECT rbi_nullcmp('SELECT k, count(*) FROM rbi_null WHERE k IS NOT NULL GROUP BY k');
SELECT rbi_nullcmp('SELECT k, count(*) FROM rbi_null WHERE k IS NULL GROUP BY k');
/*
 * A column with both a null test and an equality on it: the value the node
 * prints for that column must come from the equality's entry, not from the
 * null entry that happens to be listed first.
 */
SELECT rbi_nullcmp('SELECT k, count(*) FROM rbi_null WHERE k IS NOT NULL AND k = 3 GROUP BY k');
SELECT rbi_nullcmp('SELECT k, count(*) FROM rbi_null WHERE k = 3 AND k IS NOT NULL GROUP BY k');
SELECT rbi_nullcmp('SELECT k, count(k) FROM rbi_null WHERE k IS NULL GROUP BY k');

-- count(col) of the group column: count(*) for real groups, 0 for the NULL one
SELECT rbi_nullcmp('SELECT k, count(k) FROM rbi_null GROUP BY k');
SELECT rbi_nullcmp('SELECT k, count(*) AS n_all, count(k) AS n_k FROM rbi_null GROUP BY k');
SELECT rbi_nullcmp('SELECT count(k) FROM rbi_null WHERE k IS NULL');
SELECT rbi_nullcmp('SELECT count(k) FROM rbi_null WHERE k = 3');
SELECT rbi_nullcmp('SELECT count(k) FROM rbi_null WHERE k IS NOT NULL');
-- count() of a column nothing constrains is still not pushed down
SELECT rbi_nullcmp('SELECT count(t) FROM rbi_null WHERE k = 3');

-- the results themselves
SELECT k, count(*), count(k) FROM rbi_null GROUP BY k ORDER BY k NULLS LAST;
SELECT t, count(*) FROM rbi_null WHERE k IS NULL GROUP BY t ORDER BY t NULLS LAST;

-- ---- inserts of NULL keys (aminsert) -----------------------------------
INSERT INTO rbi_null
SELECT i, NULL, NULL, i % 4 FROM generate_series(50001, 60000) i;
SELECT roaring_index_verify('rbi_null_k', true);
SELECT roaring_index_verify('rbi_null_t', true);
SELECT entries, null_tids FROM roaring_index_stats('rbi_null_k');
SELECT rbi_nullcmp('SELECT count(*) FROM rbi_null WHERE k IS NULL');
SELECT rbi_nullcmp('SELECT count(*) FROM rbi_null WHERE k IS NOT NULL');
SELECT rbi_nullcmp('SELECT id FROM rbi_null WHERE k IS NULL AND d = 3');

-- a NULL key inserted into an index that had none
CREATE TABLE rbi_null_late (id int NOT NULL, k int NOT NULL);
INSERT INTO rbi_null_late SELECT i, i % 9 FROM generate_series(1, 5000) i;
CREATE INDEX rbi_null_late_k ON rbi_null_late USING roaring (k);
SELECT null_tids FROM roaring_index_stats('rbi_null_late_k');
ALTER TABLE rbi_null_late ALTER COLUMN k DROP NOT NULL;
INSERT INTO rbi_null_late VALUES (5001, NULL), (5002, NULL);
SELECT roaring_index_verify('rbi_null_late_k', true);
SELECT entries, ntids, null_tids FROM roaring_index_stats('rbi_null_late_k');
VACUUM ANALYZE rbi_null_late;
SELECT rbi_nullcmp('SELECT id FROM rbi_null_late WHERE k IS NULL');
SELECT rbi_nullcmp('SELECT count(*) FROM rbi_null_late WHERE k IS NULL');

-- ---- deletes and VACUUM ------------------------------------------------
-- before VACUUM the TIDs are still in the index and the heap recheck decides
DELETE FROM rbi_null WHERE k IS NULL AND id % 2 = 0;
SELECT roaring_index_verify('rbi_null_k', true);
SELECT rbi_nullcmp('SELECT count(*) FROM rbi_null WHERE k IS NULL');
SELECT rbi_nullcmp('SELECT id FROM rbi_null WHERE k IS NULL AND d = 1');

VACUUM rbi_null;
SELECT roaring_index_verify('rbi_null_k', true);
SELECT roaring_index_verify('rbi_null_t', true);
SELECT null_tids = (SELECT count(*) FROM rbi_null WHERE k IS NULL) AS null_tids_match
  FROM roaring_index_stats('rbi_null_k');
SELECT rbi_nullcmp('SELECT count(*) FROM rbi_null WHERE k IS NULL');
SELECT rbi_nullcmp('SELECT count(*) FROM rbi_null WHERE k IS NOT NULL');
SELECT rbi_nullcmp('SELECT k, count(*) FROM rbi_null GROUP BY k');

-- every NULL row gone: the entry stays, empty, and produces no group
DELETE FROM rbi_null WHERE k IS NULL;
VACUUM rbi_null;
SELECT roaring_index_verify('rbi_null_k', true);
SELECT entries, null_tids FROM roaring_index_stats('rbi_null_k');
SELECT rbi_nullcmp('SELECT count(*) FROM rbi_null WHERE k IS NULL');
SELECT rbi_nullcmp('SELECT k, count(*) FROM rbi_null GROUP BY k');
SELECT rbi_nullcmp('SELECT id FROM rbi_null WHERE k IS NULL');

-- ---- a table that is nothing but NULLs ---------------------------------
CREATE TABLE rbi_null_all (id int NOT NULL, k int);
INSERT INTO rbi_null_all SELECT i, NULL FROM generate_series(1, 20000) i;
CREATE INDEX rbi_null_all_k ON rbi_null_all USING roaring (k);
VACUUM ANALYZE rbi_null_all;
SELECT roaring_index_verify('rbi_null_all_k', true);
SELECT entries, ntids, null_tids, container_pages > 0 AS null_entry_spilled
  FROM roaring_index_stats('rbi_null_all_k');
SELECT rbi_nullcmp('SELECT count(*) FROM rbi_null_all WHERE k IS NULL');
SELECT rbi_nullcmp('SELECT count(*) FROM rbi_null_all WHERE k IS NOT NULL');
SELECT rbi_nullcmp('SELECT k, count(*) FROM rbi_null_all GROUP BY k');
SELECT rbi_nullcmp('SELECT count(*) FROM rbi_null_all WHERE k = 1');
-- and the same index after half of it is deleted and vacuumed
DELETE FROM rbi_null_all WHERE id % 2 = 0;
VACUUM rbi_null_all;
SELECT roaring_index_verify('rbi_null_all_k', true);
/*
 * Deleting every other row turns each run container into a bitset, so the
 * payload outgrows inline_limit and the NULL entry spills onto container
 * pages - it has to stay the NULL entry while it does.
 */
SELECT null_tids, container_pages > 0 AS null_entry_spilled
  FROM roaring_index_stats('rbi_null_all_k');
SELECT rbi_nullcmp('SELECT count(*) FROM rbi_null_all WHERE k IS NULL');
SELECT rbi_nullcmp('SELECT id FROM rbi_null_all WHERE k IS NULL');

/* The same thing built as a chain from the start, and then inserted into. */
CREATE INDEX rbi_null_all_k2 ON rbi_null_all USING roaring (k)
	WITH (inline_limit = 64);
SELECT roaring_index_verify('rbi_null_all_k2', true);
SELECT entries, null_tids, container_pages > 0 AS null_entry_spilled
  FROM roaring_index_stats('rbi_null_all_k2');
INSERT INTO rbi_null_all SELECT i, NULL FROM generate_series(20001, 20500) i;
SELECT roaring_index_verify('rbi_null_all_k2', true);
SELECT null_tids FROM roaring_index_stats('rbi_null_all_k2');
VACUUM ANALYZE rbi_null_all;
SELECT rbi_nullcmp('SELECT count(*) FROM rbi_null_all WHERE k IS NULL');

-- VACUUM filtering a NULL entry that is a chain of container pages
DELETE FROM rbi_null_all WHERE id % 3 = 0;
VACUUM rbi_null_all;
SELECT roaring_index_verify('rbi_null_all_k', true);
SELECT roaring_index_verify('rbi_null_all_k2', true);
SELECT null_tids = (SELECT count(*) FROM rbi_null_all) AS null_tids_match
  FROM roaring_index_stats('rbi_null_all_k2');
SELECT rbi_nullcmp('SELECT count(*) FROM rbi_null_all WHERE k IS NULL');
SELECT rbi_nullcmp('SELECT id FROM rbi_null_all WHERE k IS NULL AND id < 100');

-- ---- an empty index and an index built on NULL-only data ---------------
CREATE TABLE rbi_null_empty (id int NOT NULL, k int);
CREATE INDEX rbi_null_empty_k ON rbi_null_empty USING roaring (k);
SELECT roaring_index_verify('rbi_null_empty_k', true);
SELECT entries, null_tids FROM roaring_index_stats('rbi_null_empty_k');
INSERT INTO rbi_null_empty VALUES (1, NULL), (2, 7), (3, NULL);
SELECT roaring_index_verify('rbi_null_empty_k', true);
SELECT entries, ntids, null_tids FROM roaring_index_stats('rbi_null_empty_k');
SET enable_seqscan = off;
SELECT id FROM rbi_null_empty WHERE k IS NULL ORDER BY id;
SELECT id FROM rbi_null_empty WHERE k IS NOT NULL ORDER BY id;
RESET enable_seqscan;

DROP TABLE rbi_null, rbi_null_late, rbi_null_all, rbi_null_empty;
DROP FUNCTION rbi_nullcmp(text);
DROP FUNCTION rbi_nullplan(text);
