-- VACUUM of a lion index: ambulkdelete and amvacuumcleanup
-- (DESIGN.md section 5, VACUUM, and section 11).
\set VERBOSITY terse
SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS pg_lion;

SELECT setseed(0.42);

CREATE OR REPLACE FUNCTION lion_cmp(tbl text, pred text) RETURNS text
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
CREATE TABLE lion_vac (i int4, k int4, t text);
INSERT INTO lion_vac
SELECT i, i / 5000, 'v' || (i % 11) FROM generate_series(1, 200000) i;
CREATE INDEX lion_vac_k ON lion_vac USING lion (k);
CREATE INDEX lion_vac_t ON lion_vac USING lion (t);

SELECT entries, inline_entries, run_containers > 0 AS has_runs, ntids
  FROM lion_index_stats('lion_vac_k');
SELECT entries, inline_entries, container_pages > 0 AS has_chains, ntids
  FROM lion_index_stats('lion_vac_t');

-- Every other row of a dense, clustered key: the runs split into singletons,
-- which no longer fit inline, so the entries spill onto container pages.
DELETE FROM lion_vac WHERE i % 2 = 0;
VACUUM lion_vac;

SELECT entries, inline_entries, bitset_containers > 0 AS has_bitsets, ntids
  FROM lion_index_stats('lion_vac_k');
SELECT entries, ntids FROM lion_index_stats('lion_vac_t');
SELECT lion_index_verify('lion_vac_k', true);
SELECT lion_index_verify('lion_vac_t', true);
SELECT lion_cmp('lion_vac', 'k = 7');
SELECT lion_cmp('lion_vac', 'k = 39');
SELECT lion_cmp('lion_vac', 't = ''v3''');

-- The index statistics VACUUM reported match what the index holds.
SELECT (SELECT ntids FROM lion_index_stats('lion_vac_k')) =
	   (SELECT reltuples::int8 FROM pg_class WHERE relname = 'lion_vac_k')
	   AS reltuples_matches_ntids;

-- Whole keys: the entries stay (the key remains known) but hold no TIDs.
DELETE FROM lion_vac WHERE k IN (3, 4);
VACUUM lion_vac;
SELECT entries, ntids FROM lion_index_stats('lion_vac_k');
SELECT lion_index_verify('lion_vac_k', true);
SELECT lion_cmp('lion_vac', 'k = 3');
SELECT lion_cmp('lion_vac', 'k = 5');

-- Scattered rows.
DELETE FROM lion_vac WHERE i % 7 = 0;
VACUUM lion_vac;
SELECT ntids FROM lion_index_stats('lion_vac_k');
SELECT ntids FROM lion_index_stats('lion_vac_t');
SELECT lion_index_verify('lion_vac_k', true);
SELECT lion_index_verify('lion_vac_t', true);
SELECT lion_cmp('lion_vac', 'k = 7');
SELECT lion_cmp('lion_vac', 't = ''v3''');

-- A second VACUUM has nothing to do and must change nothing.
CREATE TEMP TABLE lion_vac_snap AS
	SELECT * FROM lion_index_stats('lion_vac_k');
VACUUM lion_vac;
SELECT count(*) AS unchanged_stats_rows
  FROM (SELECT * FROM lion_index_stats('lion_vac_k')
		INTERSECT
		SELECT * FROM lion_vac_snap) s;
SELECT lion_index_verify('lion_vac_k', true);

-- VACUUM FREEZE walks the same code path with a different cutoff.
VACUUM FREEZE lion_vac;
SELECT lion_index_verify('lion_vac_k', true);
SELECT lion_index_verify('lion_vac_t', true);
SELECT lion_cmp('lion_vac', 'k = 7');

-- Inserts after VACUUM reuse the heap pages it freed, so their container
-- keys land in the middle of the chains that are already there.
INSERT INTO lion_vac
SELECT i, i % 41, 'v' || (i % 11) FROM generate_series(200001, 260000) i;
SELECT lion_index_verify('lion_vac_k', true);
SELECT lion_index_verify('lion_vac_t', true);
SELECT lion_cmp('lion_vac', 'k = 7');
SELECT lion_cmp('lion_vac', 'k = 3');
SELECT lion_cmp('lion_vac', 't = ''v3''');
SELECT ntids FROM lion_index_stats('lion_vac_k');

-- ... and VACUUM after those inserts.
DELETE FROM lion_vac WHERE i > 240000;
VACUUM lion_vac;
SELECT lion_index_verify('lion_vac_k', true);
SELECT lion_cmp('lion_vac', 'k = 7');

-- A rebuilt index holds exactly the same TIDs under the same keys.
CREATE TEMP TABLE lion_vac_before AS
	SELECT entries, ntids FROM lion_index_stats('lion_vac_k');
REINDEX INDEX lion_vac_k;
SELECT (SELECT entries FROM lion_index_stats('lion_vac_k')) =
	   (SELECT entries FROM lion_vac_before) AS same_entries,
	   (SELECT ntids FROM lion_index_stats('lion_vac_k')) =
	   (SELECT ntids FROM lion_vac_before) AS same_ntids;
SELECT lion_index_verify('lion_vac_k', true);
SELECT lion_cmp('lion_vac', 'k = 7');
SELECT lion_cmp('lion_vac', 'k = 3');

/*
 * Table 2.  Half the rows of each key, in runs of 20: the build turns them
 * into run containers of a few hundred runs, and deleting every other row
 * splits every run into singletons, so every container has to grow into a
 * bitset that no longer fits in its slot.  This is the container page split
 * path of ambulkdelete.
 */
CREATE TABLE lion_vacrun (i int4, k int4);
INSERT INTO lion_vacrun
SELECT i, CASE WHEN (i % 40) < 20 THEN 1 ELSE 2 END
  FROM generate_series(1, 200000) i;
CREATE INDEX lion_vacrun_k ON lion_vacrun USING lion (k)
	WITH (inline_limit = 64);

CREATE TEMP TABLE lion_vacrun_before AS
	SELECT container_pages, containers FROM lion_index_stats('lion_vacrun_k');
SELECT run_containers, ntids FROM lion_index_stats('lion_vacrun_k');

DELETE FROM lion_vacrun WHERE i % 2 = 0;
VACUUM lion_vacrun;

SELECT bitset_containers, run_containers, ntids
  FROM lion_index_stats('lion_vacrun_k');
SELECT (SELECT container_pages FROM lion_index_stats('lion_vacrun_k')) >
	   (SELECT container_pages FROM lion_vacrun_before) AS pages_split,
	   (SELECT containers FROM lion_index_stats('lion_vacrun_k')) =
	   (SELECT containers FROM lion_vacrun_before) AS same_containers;
SELECT lion_index_verify('lion_vacrun_k', true);
SELECT lion_cmp('lion_vacrun', 'k = 1');
SELECT lion_cmp('lion_vacrun', 'k = 2');

-- Delete everything: every container disappears, the entries stay.
DELETE FROM lion_vacrun;
VACUUM lion_vacrun;
SELECT entries, containers, ntids FROM lion_index_stats('lion_vacrun_k');
SELECT lion_index_verify('lion_vacrun_k', true);
SELECT lion_cmp('lion_vacrun', 'k = 1');

-- The emptied chains still accept new rows.
INSERT INTO lion_vacrun SELECT i, 1 + (i % 3) FROM generate_series(1, 30000) i;
SELECT entries, containers > 0 AS has_containers, ntids
  FROM lion_index_stats('lion_vacrun_k');
SELECT lion_index_verify('lion_vacrun_k', true);
SELECT lion_cmp('lion_vacrun', 'k = 1');
SELECT lion_cmp('lion_vacrun', 'k = 3');

/*
 * Table 3.  Sparse segments (DESIGN.md section 13): a high-cardinality key
 * whose posting sets are pairs, not containers.  VACUUM filters the pairs
 * through the same TID callback, drops segments that lose every pair, and
 * leaves a container that falls below the threshold a container.
 */
CREATE TABLE lion_vacsp (i int4, k int4, d int4);
INSERT INTO lion_vacsp
SELECT i, ((i::bigint * 7919) % 60000)::int4, i % 40
  FROM generate_series(1, 120000) i;
CREATE INDEX lion_vacsp_k ON lion_vacsp USING lion (k) WITH (inline_limit = 64);
CREATE INDEX lion_vacsp_d ON lion_vacsp USING lion (d);

-- k: two rows per key, never four in one container key, so all pairs
SELECT containers, sparse_segments > 0 AS has_segments,
	   sparse_members = ntids AS all_pairs, ntids
  FROM lion_index_stats('lion_vacsp_k');
-- d: 3000 rows per key spread over the heap, so containers everywhere
SELECT containers > 0 AS has_containers, sparse_segments, ntids
  FROM lion_index_stats('lion_vacsp_d');

-- Delete most rows: segments shrink, and the ones that lose every pair go.
CREATE TEMP TABLE lion_vacsp_before AS
	SELECT sparse_segments, sparse_members, containers
	  FROM lion_index_stats('lion_vacsp_k');
DELETE FROM lion_vacsp WHERE i % 4 <> 0;
VACUUM lion_vacsp;

SELECT (SELECT sparse_segments FROM lion_index_stats('lion_vacsp_k')) <
	   (SELECT sparse_segments FROM lion_vacsp_before) AS fewer_segments,
	   (SELECT sparse_members FROM lion_index_stats('lion_vacsp_k')) <
	   (SELECT sparse_members FROM lion_vacsp_before) AS fewer_members;
SELECT sparse_members = ntids AS still_all_pairs,
	   ntids = (SELECT count(*) FROM lion_vacsp) AS ntids_matches_heap,
	   entries AS entries_are_kept
  FROM lion_index_stats('lion_vacsp_k');
/*
 * d keeps its containers even where they are now down to a handful of
 * members: converting back to pairs is optional and VACUUM does not do it.
 */
SELECT containers > 0 AS still_containers, sparse_segments,
	   ntids = (SELECT count(*) FROM lion_vacsp) AS ntids_matches_heap
  FROM lion_index_stats('lion_vacsp_d');
SELECT lion_index_verify('lion_vacsp_k', true);
SELECT lion_index_verify('lion_vacsp_d', true);
SELECT lion_cmp('lion_vacsp', 'k = 31676');	-- the row of i = 4, still there
SELECT lion_cmp('lion_vacsp', 'k = 7919');	-- the row of i = 1, deleted
SELECT lion_cmp('lion_vacsp', 'd = 8');		-- d = 8 is i % 40 = 8, i. e. i % 4 = 0
SELECT lion_cmp('lion_vacsp', 'd = 7');		-- d = 7 needs an odd i: all gone

-- Delete the rest: every segment goes away, the entries stay.
DELETE FROM lion_vacsp;
VACUUM lion_vacsp;
SELECT entries > 0 AS entries_are_kept, containers, sparse_segments,
	   sparse_members, ntids
  FROM lion_index_stats('lion_vacsp_k');
SELECT lion_index_verify('lion_vacsp_k', true);
SELECT lion_index_verify('lion_vacsp_d', true);
SELECT lion_cmp('lion_vacsp', 'k = 7919');

-- ... and the emptied posting sets accept new rows again
INSERT INTO lion_vacsp
SELECT i, ((i::bigint * 7919) % 60000)::int4, i % 40
  FROM generate_series(1, 20000) i;
SELECT sparse_segments > 0 AS has_segments, ntids
  FROM lion_index_stats('lion_vacsp_k');
SELECT lion_index_verify('lion_vacsp_k', true);
SELECT lion_index_verify('lion_vacsp_d', true);
SELECT lion_cmp('lion_vacsp', 'd = 7');

DROP TABLE lion_vac, lion_vacrun, lion_vacsp;
