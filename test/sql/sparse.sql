-- Sparse segments (DESIGN.md section 13).
--
-- A container key with fewer than RBI_SPARSE_THRESHOLD (4) members does not
-- get a container of its own: its (ckey, lo) pairs live in a sparse segment,
-- an item of at most 682 pairs that shares the container header.  This file
-- checks the format end to end: what ambuild produces, what aminsert does
-- when a key crosses the threshold or a segment overflows, what VACUUM leaves
-- behind, and that scans, roaring_index_count() and the count pushdown all
-- agree with a sequential scan at every step.
--
-- roaring_index_verify(idx, true) runs after every phase; besides the
-- structural rules it checks that no container key inside a segment has
-- reached the threshold, and that item ranges never overlap or interleave.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'roaring_index';

/*
 * VACUUM can only set a heap page all-visible once the inserting
 * transaction's commit record is on disk, and the dev cluster runs with
 * synchronous_commit = off (see count.sql).
 */
SET synchronous_commit = on;

CREATE EXTENSION IF NOT EXISTS roaring_index;

/* Bitmap scan versus sequential scan, as in insert.sql. */
CREATE OR REPLACE FUNCTION rbi_sp_cmp(tbl text, pred text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	q text := format('SELECT count(*) AS c, coalesce(sum(i), 0) AS s FROM %s WHERE %s', tbl, pred);
	a record;
	b record;
	ln text;
	used boolean := false;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', false);
	PERFORM set_config('enable_bitmapscan', 'on', false);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Bitmap Index Scan%' THEN
			used := true;
		END IF;
	END LOOP;
	EXECUTE q INTO a;

	PERFORM set_config('enable_seqscan', 'on', false);
	PERFORM set_config('enable_bitmapscan', 'off', false);
	EXECUTE q INTO b;
	PERFORM set_config('enable_bitmapscan', 'on', false);

	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH index=%s seqscan=%s', a::text, b::text);
	END IF;
	IF NOT used THEN
		RETURN format('NO BITMAP INDEX SCAN (%s rows)', a.c);
	END IF;
	RETURN format('ok %s rows', a.c);
END $$;

/* roaring_index_count() versus count(*), as in count.sql. */
CREATE OR REPLACE FUNCTION rbi_sp_ccmp(idx text, tbl text, col text, val text)
RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	a bigint;
	b bigint;
BEGIN
	PERFORM set_config('roaring_index.enable_count_pushdown', 'off', true);
	EXECUTE format('SELECT roaring_index_count(%L::regclass, %s)', idx, val) INTO a;
	EXECUTE format('SELECT count(*) FROM %s WHERE %I = %s', tbl, col, val) INTO b;
	PERFORM set_config('roaring_index.enable_count_pushdown', 'on', true);
	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH roaring=%s select=%s', a, b);
	END IF;
	RETURN format('ok %s', a);
END $$;

/* The count pushdown versus the ordinary plan, as in pushdown.sql. */
CREATE OR REPLACE FUNCTION rbi_sp_pd(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	pushed boolean := false;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('roaring_index.enable_count_pushdown', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (RoaringCount)%' THEN
			pushed := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE rbi_sp_on AS %s', q);

	PERFORM set_config('roaring_index.enable_count_pushdown', 'off', true);
	EXECUTE format('CREATE TEMP TABLE rbi_sp_off AS %s', q);
	PERFORM set_config('roaring_index.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM rbi_sp_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM rbi_sp_on EXCEPT ALL SELECT * FROM rbi_sp_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM rbi_sp_off EXCEPT ALL SELECT * FROM rbi_sp_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE rbi_sp_on, rbi_sp_off';

	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN pushed THEN 'pushed down' ELSE 'not pushed down' END,
				  nrows);
END $$;

/* ------------------------------------------------------------------ *
 * 1. ambuild on a high-cardinality column: every TID in a segment
 * ------------------------------------------------------------------ */

CREATE TABLE sp_hi (i int4, k int4 NOT NULL, few int4 NOT NULL);
INSERT INTO sp_hi
SELECT i, i, ((i::bigint * 7919) % 400)::int4 FROM generate_series(1, 120000) i;

-- k is unique, so no container key of any key has more than one member
CREATE INDEX sp_hi_k ON sp_hi USING roaring (k);
SELECT entries, containers, sparse_segments,
	   sparse_members = ntids AS every_tid_in_a_segment,
	   containers = 0 AS no_containers,
	   container_pages = 0 AS all_inline,
	   ntids
  FROM roaring_index_stats('sp_hi_k');
SELECT roaring_index_verify('sp_hi_k', true);

/*
 * few has 300 rows per key spread over the whole heap, i.e. about 50 per
 * container key: dense enough for containers everywhere, so this index has
 * no segments at all and shows that the format is not paid for when it is
 * not needed.
 */
CREATE INDEX sp_hi_few ON sp_hi USING roaring (few);
SELECT containers > 0 AS has_containers, sparse_segments, sparse_members
  FROM roaring_index_stats('sp_hi_few');
SELECT roaring_index_verify('sp_hi_few', true);

-- the scan emits segment pairs as TIDs
SELECT rbi_sp_cmp('sp_hi', 'k = 1');
SELECT rbi_sp_cmp('sp_hi', 'k = 119999');
SELECT rbi_sp_cmp('sp_hi', 'k = -1');
SELECT rbi_sp_cmp('sp_hi', 'few = 7');

-- and the count reads one temporary container per container key
SELECT rbi_sp_ccmp('sp_hi_k', 'sp_hi', 'k', '1');
SELECT rbi_sp_ccmp('sp_hi_k', 'sp_hi', 'k', '60000');
SELECT rbi_sp_ccmp('sp_hi_k', 'sp_hi', 'k', '999999');
SELECT rbi_sp_ccmp('sp_hi_few', 'sp_hi', 'few', '7');

/* ------------------------------------------------------------------ *
 * 2. A medium-cardinality column: containers and segments on one page
 * ------------------------------------------------------------------ */

/*
 * 3 rows per key over 660 heap pages: a key has a couple of members in the
 * container keys it touches, so the odd dense container key gets a container
 * and the rest of the posting set stays in segments.  With inline_limit at
 * its minimum the bigger posting sets spill, and the resulting container
 * pages hold both kinds of item.
 */
CREATE TABLE sp_mix (i int4, k int4 NOT NULL);
INSERT INTO sp_mix
SELECT i, CASE WHEN i % 100 < 20 THEN 1 ELSE 1000 + (i / 3) END
  FROM generate_series(1, 120000) i;
CREATE INDEX sp_mix_k ON sp_mix USING roaring (k) WITH (inline_limit = 64);
SELECT containers > 0 AS has_containers,
	   sparse_segments > 0 AS has_segments,
	   container_pages > 0 AS has_container_pages,
	   inline_entries > 0 AS has_inline_entries,
	   sparse_members <= ntids AS members_within_ntids
  FROM roaring_index_stats('sp_mix_k');
SELECT roaring_index_verify('sp_mix_k', true);
SELECT rbi_sp_cmp('sp_mix', 'k = 1');
SELECT rbi_sp_cmp('sp_mix', 'k = 1042');
SELECT rbi_sp_ccmp('sp_mix_k', 'sp_mix', 'k', '1');
SELECT rbi_sp_ccmp('sp_mix_k', 'sp_mix', 'k', '1042');

/* ------------------------------------------------------------------ *
 * 3. aminsert: a container key crossing the threshold
 * ------------------------------------------------------------------ */

CREATE TABLE sp_thr (i int4, k int4 NOT NULL);
CREATE INDEX sp_thr_k ON sp_thr USING roaring (k);

-- one, two and three members of one container key: still one segment
INSERT INTO sp_thr SELECT i, 1 FROM generate_series(1, 3) i;
SELECT containers, sparse_segments, sparse_members, ntids
  FROM roaring_index_stats('sp_thr_k');
SELECT roaring_index_verify('sp_thr_k', true);

-- the fourth member promotes the container key to a container of its own
INSERT INTO sp_thr VALUES (4, 1);
SELECT containers, sparse_segments, sparse_members, ntids
  FROM roaring_index_stats('sp_thr_k');
SELECT roaring_index_verify('sp_thr_k', true);
SELECT rbi_sp_cmp('sp_thr', 'k = 1');

-- more members keep going into that container, not into a segment
INSERT INTO sp_thr SELECT i, 1 FROM generate_series(5, 50) i;
SELECT containers, sparse_segments, sparse_members, ntids
  FROM roaring_index_stats('sp_thr_k');
SELECT roaring_index_verify('sp_thr_k', true);
SELECT rbi_sp_cmp('sp_thr', 'k = 1');
SELECT rbi_sp_ccmp('sp_thr_k', 'sp_thr', 'k', '1');

/* ------------------------------------------------------------------ *
 * 4. Wide rows: one row per heap page, so container keys are cheap
 * ------------------------------------------------------------------ */

/*
 * A container key covers 64 heap pages whatever they hold, so a segment of
 * several hundred pairs needs a heap of several hundred container keys.  A
 * 7000-byte PLAIN column puts exactly one row on each page, which buys 64
 * rows per container key instead of about 11000.
 *
 * Key 1 takes three rows out of every 64 (three members per container key,
 * one below the threshold), so its posting set is nothing but segments, and
 * with 235 container keys it overflows the 682-pair maximum: the segment
 * being appended to is closed at 682 pairs and a new one is started.  Every
 * sixteenth container key is left without a row of key 1, which is where the
 * inserts below will go.
 */
CREATE TABLE sp_wide (i int4, k int4 NOT NULL, pad text);
ALTER TABLE sp_wide ALTER COLUMN pad SET STORAGE PLAIN;
CREATE INDEX sp_wide_k ON sp_wide USING roaring (k) WITH (buckets = 4);
INSERT INTO sp_wide
SELECT i,
	   CASE WHEN i % 64 < 3 AND (i / 64) % 16 <> 0 THEN 1 ELSE 1000 + i END,
	   repeat('x', 7000)
  FROM generate_series(1, 16000) i;

SELECT count(*) AS rows_of_key_1 FROM sp_wide WHERE k = 1;
SELECT (SELECT count(*) FROM sp_wide WHERE k = 1) > 682 AS more_than_one_segment;
SELECT containers, sparse_segments > 1 AS several_segments,
	   sparse_members = ntids AS every_tid_in_a_segment
  FROM roaring_index_stats('sp_wide_k');
SELECT roaring_index_verify('sp_wide_k', true);
SELECT rbi_sp_cmp('sp_wide', 'k = 1');
SELECT rbi_sp_ccmp('sp_wide_k', 'sp_wide', 'k', '1');

/*
 * Now insert INTO the range a segment already covers, which is the only way
 * a segment can overflow in the middle and have to split in half.
 *
 * Emptying the container keys key 1 skipped frees whole runs of heap pages
 * inside the range of the first, full segment.  (Whole ones: with only a
 * handful of dead tuples VACUUM bypasses the index scan, leaves the dead
 * line pointers in place and the space stays unusable.)  The rows inserted
 * afterwards land in those pages.
 */
CREATE TEMP TABLE sp_wide_before AS
	SELECT sparse_segments, containers FROM roaring_index_stats('sp_wide_k');
DELETE FROM sp_wide WHERE (i / 64) % 16 = 0 AND i > 512;
VACUUM sp_wide;
SELECT roaring_index_verify('sp_wide_k', true);

/*
 * Three rows, one container key, three members: below the threshold, so they
 * stay pairs -- but the segment that covers that container key is at its
 * 682-pair maximum, so it has to split in half at a container key boundary
 * first.  One more segment, still no container.
 */
INSERT INTO sp_wide
SELECT 20000 + i, 1, repeat('y', 7000) FROM generate_series(1, 3) i;
SELECT (SELECT sparse_segments FROM roaring_index_stats('sp_wide_k')) -
	   (SELECT sparse_segments FROM sp_wide_before) AS segments_added,
	   (SELECT containers FROM roaring_index_stats('sp_wide_k')) AS containers;
SELECT roaring_index_verify('sp_wide_k', true);
SELECT rbi_sp_cmp('sp_wide', 'k = 1');
SELECT rbi_sp_ccmp('sp_wide_k', 'sp_wide', 'k', '1');

/*
 * Three more rows in the same place: one of those container keys reaches the
 * threshold and is promoted to a container of its own, which splits the
 * segment around it into (left part, container, right part) -- three items
 * where there was one, written in a single WAL record.
 */
INSERT INTO sp_wide
SELECT 21000 + i, 1, repeat('z', 7000) FROM generate_series(1, 3) i;
SELECT (SELECT containers FROM roaring_index_stats('sp_wide_k')) AS containers,
	   (SELECT sparse_segments FROM roaring_index_stats('sp_wide_k')) -
	   (SELECT sparse_segments FROM sp_wide_before) AS segments_added,
	   (SELECT ntids FROM roaring_index_stats('sp_wide_k')) =
	   (SELECT count(*) FROM sp_wide) AS ntids_matches_heap;
SELECT roaring_index_verify('sp_wide_k', true);
SELECT rbi_sp_cmp('sp_wide', 'k = 1');
SELECT rbi_sp_ccmp('sp_wide_k', 'sp_wide', 'k', '1');

/* ------------------------------------------------------------------ *
 * 5. DELETE and VACUUM: segments shrink, empty ones are removed
 * ------------------------------------------------------------------ */

-- delete two thirds of the high-cardinality table and vacuum it away
SELECT sparse_members AS members_before FROM roaring_index_stats('sp_hi_k');
DELETE FROM sp_hi WHERE i % 3 <> 0;
SELECT rbi_sp_cmp('sp_hi', 'k = 1');
SELECT rbi_sp_ccmp('sp_hi_k', 'sp_hi', 'k', '1');
SELECT rbi_sp_ccmp('sp_hi_k', 'sp_hi', 'k', '3');
SELECT roaring_index_verify('sp_hi_k', true);
VACUUM sp_hi;
SELECT sparse_members = ntids AS still_all_sparse,
	   ntids = (SELECT count(*) FROM sp_hi) AS ntids_matches_heap,
	   entries AS entries_are_kept
  FROM roaring_index_stats('sp_hi_k');
SELECT roaring_index_verify('sp_hi_k', true);
SELECT rbi_sp_cmp('sp_hi', 'k = 3');
SELECT rbi_sp_cmp('sp_hi', 'k = 1');	-- deleted: no rows, entry still there
SELECT rbi_sp_ccmp('sp_hi_k', 'sp_hi', 'k', '1');
SELECT rbi_sp_ccmp('sp_hi_k', 'sp_hi', 'k', '3');

-- every segment of a spilled posting set can go away
DELETE FROM sp_mix WHERE k <> 1;
VACUUM sp_mix;
SELECT containers, sparse_segments, sparse_members,
	   ntids = (SELECT count(*) FROM sp_mix) AS ntids_matches_heap
  FROM roaring_index_stats('sp_mix_k');
SELECT roaring_index_verify('sp_mix_k', true);
SELECT rbi_sp_cmp('sp_mix', 'k = 1');
SELECT rbi_sp_cmp('sp_mix', 'k = 1042');

-- and deleting everything empties the posting sets without losing the entries
DELETE FROM sp_thr;
VACUUM sp_thr;
SELECT entries, containers, sparse_segments, ntids
  FROM roaring_index_stats('sp_thr_k');
SELECT roaring_index_verify('sp_thr_k', true);
SELECT rbi_sp_cmp('sp_thr', 'k = 1');

/* ------------------------------------------------------------------ *
 * 6. The count pushdown over segment-heavy posting sets
 * ------------------------------------------------------------------ */

VACUUM (ANALYZE) sp_hi;
ANALYZE sp_wide;

-- an all-visible heap: the count comes straight from the visibility map
SELECT rbi_sp_pd('SELECT count(*) FROM sp_hi WHERE k = 3');
SELECT rbi_sp_pd('SELECT count(*) FROM sp_hi WHERE k = 999999');
SELECT rbi_sp_pd('SELECT count(*) FROM sp_hi WHERE few = 7');
SELECT rbi_sp_pd('SELECT few, count(*) FROM sp_hi GROUP BY few');
/*
 * The intersection of a segment-only posting set with a container-only one,
 * through the two-key function rather than the planner: whether the pushdown
 * wins on cost for so few rows is a cost model question, and this is about
 * the merge.
 */
SELECT roaring_index_count('sp_hi_k', 3, 'sp_hi_few', 157) =
	   (SELECT count(*) FROM sp_hi WHERE k = 3 AND few = 157) AS and_matches;
SELECT roaring_index_count('sp_hi_k', 3, 'sp_hi_few', 158) =
	   (SELECT count(*) FROM sp_hi WHERE k = 3 AND few = 158) AS and_matches_empty;
SELECT roaring_index_count('sp_hi_few', 7, 'sp_hi_k', 7) =
	   (SELECT count(*) FROM sp_hi WHERE few = 7 AND k = 7) AS and_matches_other_way;
SELECT rbi_sp_pd('SELECT count(*) FROM sp_wide WHERE k = 1');

-- a dirty heap: the same answers, through the heap recheck
INSERT INTO sp_hi SELECT 300000 + i, 3, 7 FROM generate_series(1, 5) i;
DELETE FROM sp_hi WHERE k = 999;
SELECT rbi_sp_pd('SELECT count(*) FROM sp_hi WHERE k = 3');
SELECT rbi_sp_pd('SELECT count(*) FROM sp_hi WHERE k = 999');
SELECT rbi_sp_pd('SELECT count(*) FROM sp_hi WHERE few = 7');
SELECT rbi_sp_pd('SELECT few, count(*) FROM sp_hi GROUP BY few');
SELECT rbi_sp_ccmp('sp_hi_k', 'sp_hi', 'k', '3');
SELECT rbi_sp_ccmp('sp_hi_k', 'sp_hi', 'k', '999');
SELECT roaring_index_verify('sp_hi_k', true);

-- and again once the heap is all-visible
VACUUM (ANALYZE) sp_hi;
SELECT rbi_sp_pd('SELECT count(*) FROM sp_hi WHERE k = 3');
SELECT rbi_sp_pd('SELECT count(*) FROM sp_hi WHERE k = 999');
SELECT rbi_sp_pd('SELECT few, count(*) FROM sp_hi GROUP BY few');
SELECT roaring_index_verify('sp_hi_k', true);
SELECT roaring_index_verify('sp_hi_few', true);

DROP TABLE sp_hi, sp_mix, sp_thr, sp_wide;
DROP FUNCTION rbi_sp_cmp(text, text);
DROP FUNCTION rbi_sp_ccmp(text, text, text, text);
DROP FUNCTION rbi_sp_pd(text);
