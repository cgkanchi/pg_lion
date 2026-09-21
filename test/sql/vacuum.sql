-- VACUUM of a roaring index: ambulkdelete and amvacuumcleanup
-- (DESIGN.md section 5, VACUUM, and section 11).
\set VERBOSITY terse
SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS roaring_index;

SELECT setseed(0.42);

CREATE OR REPLACE FUNCTION rbi_cmp(tbl text, pred text) RETURNS text
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

/*
 * Table 1.  k is clustered, so each of its keys is one long run inside one
 * or two containers: tiny posting sets that live INLINE in their entry
 * tuples.  t is spread over the whole table, so its keys are CHAIN entries
 * with array containers.
 */
CREATE TABLE rbi_vac (i int4, k int4, t text);
INSERT INTO rbi_vac
SELECT i, i / 5000, 'v' || (i % 11) FROM generate_series(1, 200000) i;
CREATE INDEX rbi_vac_k ON rbi_vac USING roaring (k);
CREATE INDEX rbi_vac_t ON rbi_vac USING roaring (t);

SELECT entries, inline_entries, run_containers > 0 AS has_runs, ntids
  FROM roaring_index_stats('rbi_vac_k');
SELECT entries, inline_entries, container_pages > 0 AS has_chains, ntids
  FROM roaring_index_stats('rbi_vac_t');

-- Every other row of a dense, clustered key: the runs split into singletons,
-- which no longer fit inline, so the entries spill onto container pages.
DELETE FROM rbi_vac WHERE i % 2 = 0;
VACUUM rbi_vac;

SELECT entries, inline_entries, bitset_containers > 0 AS has_bitsets, ntids
  FROM roaring_index_stats('rbi_vac_k');
SELECT entries, ntids FROM roaring_index_stats('rbi_vac_t');
SELECT roaring_index_verify('rbi_vac_k', true);
SELECT roaring_index_verify('rbi_vac_t', true);
SELECT rbi_cmp('rbi_vac', 'k = 7');
SELECT rbi_cmp('rbi_vac', 'k = 39');
SELECT rbi_cmp('rbi_vac', 't = ''v3''');

-- The index statistics VACUUM reported match what the index holds.
SELECT (SELECT ntids FROM roaring_index_stats('rbi_vac_k')) =
	   (SELECT reltuples::int8 FROM pg_class WHERE relname = 'rbi_vac_k')
	   AS reltuples_matches_ntids;

-- Whole keys: the entries stay (the key remains known) but hold no TIDs.
DELETE FROM rbi_vac WHERE k IN (3, 4);
VACUUM rbi_vac;
SELECT entries, ntids FROM roaring_index_stats('rbi_vac_k');
SELECT roaring_index_verify('rbi_vac_k', true);
SELECT rbi_cmp('rbi_vac', 'k = 3');
SELECT rbi_cmp('rbi_vac', 'k = 5');

-- Scattered rows.
DELETE FROM rbi_vac WHERE i % 7 = 0;
VACUUM rbi_vac;
SELECT ntids FROM roaring_index_stats('rbi_vac_k');
SELECT ntids FROM roaring_index_stats('rbi_vac_t');
SELECT roaring_index_verify('rbi_vac_k', true);
SELECT roaring_index_verify('rbi_vac_t', true);
SELECT rbi_cmp('rbi_vac', 'k = 7');
SELECT rbi_cmp('rbi_vac', 't = ''v3''');

-- A second VACUUM has nothing to do and must change nothing.
CREATE TEMP TABLE rbi_vac_snap AS
	SELECT * FROM roaring_index_stats('rbi_vac_k');
VACUUM rbi_vac;
SELECT count(*) AS unchanged_stats_rows
  FROM (SELECT * FROM roaring_index_stats('rbi_vac_k')
		INTERSECT
		SELECT * FROM rbi_vac_snap) s;
SELECT roaring_index_verify('rbi_vac_k', true);

-- VACUUM FREEZE walks the same code path with a different cutoff.
VACUUM FREEZE rbi_vac;
SELECT roaring_index_verify('rbi_vac_k', true);
SELECT roaring_index_verify('rbi_vac_t', true);
SELECT rbi_cmp('rbi_vac', 'k = 7');

-- Inserts after VACUUM reuse the heap pages it freed, so their container
-- keys land in the middle of the chains that are already there.
INSERT INTO rbi_vac
SELECT i, i % 41, 'v' || (i % 11) FROM generate_series(200001, 260000) i;
SELECT roaring_index_verify('rbi_vac_k', true);
SELECT roaring_index_verify('rbi_vac_t', true);
SELECT rbi_cmp('rbi_vac', 'k = 7');
SELECT rbi_cmp('rbi_vac', 'k = 3');
SELECT rbi_cmp('rbi_vac', 't = ''v3''');
SELECT ntids FROM roaring_index_stats('rbi_vac_k');

-- ... and VACUUM after those inserts.
DELETE FROM rbi_vac WHERE i > 240000;
VACUUM rbi_vac;
SELECT roaring_index_verify('rbi_vac_k', true);
SELECT rbi_cmp('rbi_vac', 'k = 7');

-- A rebuilt index holds exactly the same TIDs under the same keys.
CREATE TEMP TABLE rbi_vac_before AS
	SELECT entries, ntids FROM roaring_index_stats('rbi_vac_k');
REINDEX INDEX rbi_vac_k;
SELECT (SELECT entries FROM roaring_index_stats('rbi_vac_k')) =
	   (SELECT entries FROM rbi_vac_before) AS same_entries,
	   (SELECT ntids FROM roaring_index_stats('rbi_vac_k')) =
	   (SELECT ntids FROM rbi_vac_before) AS same_ntids;
SELECT roaring_index_verify('rbi_vac_k', true);
SELECT rbi_cmp('rbi_vac', 'k = 7');
SELECT rbi_cmp('rbi_vac', 'k = 3');

/*
 * Table 2.  Half the rows of each key, in runs of 20: the build turns them
 * into run containers of a few hundred runs, and deleting every other row
 * splits every run into singletons, so every container has to grow into a
 * bitset that no longer fits in its slot.  This is the container page split
 * path of ambulkdelete.
 */
CREATE TABLE rbi_vacrun (i int4, k int4);
INSERT INTO rbi_vacrun
SELECT i, CASE WHEN (i % 40) < 20 THEN 1 ELSE 2 END
  FROM generate_series(1, 200000) i;
CREATE INDEX rbi_vacrun_k ON rbi_vacrun USING roaring (k)
	WITH (inline_limit = 64);

CREATE TEMP TABLE rbi_vacrun_before AS
	SELECT container_pages, containers FROM roaring_index_stats('rbi_vacrun_k');
SELECT run_containers, ntids FROM roaring_index_stats('rbi_vacrun_k');

DELETE FROM rbi_vacrun WHERE i % 2 = 0;
VACUUM rbi_vacrun;

SELECT bitset_containers, run_containers, ntids
  FROM roaring_index_stats('rbi_vacrun_k');
SELECT (SELECT container_pages FROM roaring_index_stats('rbi_vacrun_k')) >
	   (SELECT container_pages FROM rbi_vacrun_before) AS pages_split,
	   (SELECT containers FROM roaring_index_stats('rbi_vacrun_k')) =
	   (SELECT containers FROM rbi_vacrun_before) AS same_containers;
SELECT roaring_index_verify('rbi_vacrun_k', true);
SELECT rbi_cmp('rbi_vacrun', 'k = 1');
SELECT rbi_cmp('rbi_vacrun', 'k = 2');

-- Delete everything: every container disappears, the entries stay.
DELETE FROM rbi_vacrun;
VACUUM rbi_vacrun;
SELECT entries, containers, ntids FROM roaring_index_stats('rbi_vacrun_k');
SELECT roaring_index_verify('rbi_vacrun_k', true);
SELECT rbi_cmp('rbi_vacrun', 'k = 1');

-- The emptied chains still accept new rows.
INSERT INTO rbi_vacrun SELECT i, 1 + (i % 3) FROM generate_series(1, 30000) i;
SELECT entries, containers > 0 AS has_containers, ntids
  FROM roaring_index_stats('rbi_vacrun_k');
SELECT roaring_index_verify('rbi_vacrun_k', true);
SELECT rbi_cmp('rbi_vacrun', 'k = 1');
SELECT rbi_cmp('rbi_vacrun', 'k = 3');

/*
 * Table 3.  Sparse segments (DESIGN.md section 13): a high-cardinality key
 * whose posting sets are pairs, not containers.  VACUUM filters the pairs
 * through the same TID callback, drops segments that lose every pair, and
 * leaves a container that falls below the threshold a container.
 */
CREATE TABLE rbi_vacsp (i int4, k int4, d int4);
INSERT INTO rbi_vacsp
SELECT i, ((i::bigint * 7919) % 60000)::int4, i % 40
  FROM generate_series(1, 120000) i;
CREATE INDEX rbi_vacsp_k ON rbi_vacsp USING roaring (k) WITH (inline_limit = 64);
CREATE INDEX rbi_vacsp_d ON rbi_vacsp USING roaring (d);

-- k: two rows per key, never four in one container key, so all pairs
SELECT containers, sparse_segments > 0 AS has_segments,
	   sparse_members = ntids AS all_pairs, ntids
  FROM roaring_index_stats('rbi_vacsp_k');
-- d: 3000 rows per key spread over the heap, so containers everywhere
SELECT containers > 0 AS has_containers, sparse_segments, ntids
  FROM roaring_index_stats('rbi_vacsp_d');

-- Delete most rows: segments shrink, and the ones that lose every pair go.
CREATE TEMP TABLE rbi_vacsp_before AS
	SELECT sparse_segments, sparse_members, containers
	  FROM roaring_index_stats('rbi_vacsp_k');
DELETE FROM rbi_vacsp WHERE i % 4 <> 0;
VACUUM rbi_vacsp;

SELECT (SELECT sparse_segments FROM roaring_index_stats('rbi_vacsp_k')) <
	   (SELECT sparse_segments FROM rbi_vacsp_before) AS fewer_segments,
	   (SELECT sparse_members FROM roaring_index_stats('rbi_vacsp_k')) <
	   (SELECT sparse_members FROM rbi_vacsp_before) AS fewer_members;
SELECT sparse_members = ntids AS still_all_pairs,
	   ntids = (SELECT count(*) FROM rbi_vacsp) AS ntids_matches_heap,
	   entries AS entries_are_kept
  FROM roaring_index_stats('rbi_vacsp_k');
/*
 * d keeps its containers even where they are now down to a handful of
 * members: converting back to pairs is optional and VACUUM does not do it.
 */
SELECT containers > 0 AS still_containers, sparse_segments,
	   ntids = (SELECT count(*) FROM rbi_vacsp) AS ntids_matches_heap
  FROM roaring_index_stats('rbi_vacsp_d');
SELECT roaring_index_verify('rbi_vacsp_k', true);
SELECT roaring_index_verify('rbi_vacsp_d', true);
SELECT rbi_cmp('rbi_vacsp', 'k = 31676');	-- the row of i = 4, still there
SELECT rbi_cmp('rbi_vacsp', 'k = 7919');	-- the row of i = 1, deleted
SELECT rbi_cmp('rbi_vacsp', 'd = 8');		-- d = 8 is i % 40 = 8, i. e. i % 4 = 0
SELECT rbi_cmp('rbi_vacsp', 'd = 7');		-- d = 7 needs an odd i: all gone

-- Delete the rest: every segment goes away, the entries stay.
DELETE FROM rbi_vacsp;
VACUUM rbi_vacsp;
SELECT entries > 0 AS entries_are_kept, containers, sparse_segments,
	   sparse_members, ntids
  FROM roaring_index_stats('rbi_vacsp_k');
SELECT roaring_index_verify('rbi_vacsp_k', true);
SELECT roaring_index_verify('rbi_vacsp_d', true);
SELECT rbi_cmp('rbi_vacsp', 'k = 7919');

-- ... and the emptied posting sets accept new rows again
INSERT INTO rbi_vacsp
SELECT i, ((i::bigint * 7919) % 60000)::int4, i % 40
  FROM generate_series(1, 20000) i;
SELECT sparse_segments > 0 AS has_segments, ntids
  FROM roaring_index_stats('rbi_vacsp_k');
SELECT roaring_index_verify('rbi_vacsp_k', true);
SELECT roaring_index_verify('rbi_vacsp_d', true);
SELECT rbi_cmp('rbi_vacsp', 'd = 7');

DROP TABLE rbi_vac, rbi_vacrun, rbi_vacsp;
