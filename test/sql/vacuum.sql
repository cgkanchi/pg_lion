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

-- Whole keys: the entries whose posting sets are now empty are DELETED
-- (DESIGN.md §18), so the index forgets those keys entirely.
CREATE TEMP TABLE lion_vac_keys AS
	SELECT entries FROM lion_index_stats('lion_vac_k');
DELETE FROM lion_vac WHERE k IN (3, 4);
VACUUM lion_vac;
SELECT entries, ntids FROM lion_index_stats('lion_vac_k');
SELECT (SELECT entries FROM lion_vac_keys) -
	   (SELECT entries FROM lion_index_stats('lion_vac_k')) AS entries_deleted;
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
	   entries AS entries_left
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

-- Delete the rest: every segment goes away and so does every entry
-- (DESIGN.md §18); an index that indexes nothing holds no entries at all.
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

/* ---------------------------------------------------------------------
 * DESIGN.md §18: entry deletion, whole-chain free and page reuse.
 * --------------------------------------------------------------------- */

/*
 * A key whose rows all go away loses its entry, and the container pages its
 * chain owned become DELETED pages in the free space map.  A later spill
 * takes them back, so the relation does not grow.
 */
CREATE TABLE lion_free (i int4, k int4);
CREATE INDEX lion_free_k ON lion_free USING lion (k) WITH (inline_limit = 64);
INSERT INTO lion_free SELECT i, i % 4 FROM generate_series(1, 200000) i;
VACUUM lion_free;
CREATE TEMP TABLE lion_free_before AS
	SELECT entries, container_pages, deleted_pages, posting_internal_pages,
		   (SELECT relpages FROM pg_class WHERE relname = 'lion_free_k') AS relpages
	  FROM lion_index_stats('lion_free_k');
SELECT entries, container_pages > 0 AS has_chains, deleted_pages,
	   posting_internal_pages > 0 AS has_posting_trees
  FROM lion_index_stats('lion_free_k');

-- Two of the four keys vanish entirely.
DELETE FROM lion_free WHERE k IN (0, 1);
VACUUM lion_free;
SELECT entries,
	   deleted_pages > 0 AS pages_were_freed,
	   container_pages < (SELECT container_pages FROM lion_free_before)
		   AS fewer_live_container_pages,
	   /* DESIGN.md §22: a freed posting set takes its INTERNAL pages with it */
	   posting_internal_pages < (SELECT posting_internal_pages
								   FROM lion_free_before)
		   AS fewer_internal_pages
  FROM lion_index_stats('lion_free_k');
SELECT lion_index_verify('lion_free_k', true);
SELECT lion_cmp('lion_free', 'k = 0');
SELECT lion_cmp('lion_free', 'k = 2');

/*
 * The freed pages come back: two new keys with as many rows as the freed ones
 * had spill into them, so the free space map hands out what it holds and the
 * relation grows by less than the pages it gave back (it cannot be exactly
 * zero: a chain's HEAD page is the one page that is never recycled, so that
 * owner_head stays a chain identity a reader can trust).
 */
CREATE TEMP TABLE lion_free_size AS
	SELECT pg_relation_size('lion_free_k') AS bytes,
		   (SELECT deleted_pages FROM lion_index_stats('lion_free_k')) AS freed;
INSERT INTO lion_free SELECT i, 4 + (i % 2) FROM generate_series(200001, 300000) i;
SELECT entries,
	   (SELECT deleted_pages FROM lion_index_stats('lion_free_k')) <
	   (SELECT freed FROM lion_free_size) AS freed_pages_were_reused,
	   pg_relation_size('lion_free_k') - (SELECT bytes FROM lion_free_size) <
	   (SELECT freed FROM lion_free_size) * current_setting('block_size')::bigint
		   AS grew_less_than_it_freed
  FROM lion_index_stats('lion_free_k');
SELECT lion_index_verify('lion_free_k', true);
SELECT lion_cmp('lion_free', 'k = 4');
SELECT lion_cmp('lion_free', 'k = 2');

-- A REINDEX of the same data holds the same keys and the same TIDs, and has
-- no freed pages at all.
CREATE TEMP TABLE lion_free_reix AS
	SELECT entries, ntids FROM lion_index_stats('lion_free_k');
REINDEX INDEX lion_free_k;
SELECT (SELECT entries FROM lion_index_stats('lion_free_k')) =
	   (SELECT entries FROM lion_free_reix) AS same_entries,
	   (SELECT ntids FROM lion_index_stats('lion_free_k')) =
	   (SELECT ntids FROM lion_free_reix) AS same_ntids,
	   (SELECT deleted_pages FROM lion_index_stats('lion_free_k')) AS deleted_after_reindex;
SELECT lion_index_verify('lion_free_k', true);

/*
 * The reserved NULL and EMPTY entries (DESIGN.md §14, §17) are deleted like
 * any other and recreated on demand.
 */
CREATE TABLE lion_vnull (i int4, k int4, a text[]);
CREATE INDEX lion_vnull_k ON lion_vnull USING lion (k);
CREATE INDEX lion_vnull_a ON lion_vnull USING lion (a);
INSERT INTO lion_vnull
SELECT i, CASE WHEN i % 3 = 0 THEN NULL ELSE i % 5 END,
		  CASE WHEN i % 7 = 0 THEN ARRAY[]::text[] ELSE ARRAY['t' || (i % 11)] END
  FROM generate_series(1, 5000) i;
SELECT null_tids > 0 AS has_null_entry FROM lion_index_stats('lion_vnull_k');
SELECT empty_tids > 0 AS has_empty_entry FROM lion_index_stats('lion_vnull_a');

DELETE FROM lion_vnull WHERE k IS NULL;
DELETE FROM lion_vnull WHERE a = ARRAY[]::text[];
VACUUM lion_vnull;
SELECT null_tids, (SELECT count(*) FROM lion_vnull WHERE k IS NULL) AS heap_nulls
  FROM lion_index_stats('lion_vnull_k');
SELECT empty_tids FROM lion_index_stats('lion_vnull_a');
SELECT lion_index_verify('lion_vnull_k', true);
SELECT lion_index_verify('lion_vnull_a', true);
SELECT lion_cmp('lion_vnull', 'k IS NULL');

-- ... and the next row of each kind recreates them.
INSERT INTO lion_vnull VALUES (90001, NULL, ARRAY[]::text[]);
SELECT null_tids FROM lion_index_stats('lion_vnull_k');
SELECT empty_tids FROM lion_index_stats('lion_vnull_a');
SELECT lion_index_verify('lion_vnull_k', true);
SELECT lion_index_verify('lion_vnull_a', true);
SELECT lion_cmp('lion_vnull', 'k IS NULL');

/*
 * Slack bounds (DESIGN.md §4 and §18).  A filtered item keeps the bytes the
 * page allotted it, so the index reports slack after a VACUUM that removed a
 * few members from every container - and verify() accepts exactly as much as
 * the bound allows, so a passing verify() IS the assertion that it is
 * bounded.
 */
CREATE TABLE lion_slackv (i int4, k int4);
CREATE INDEX lion_slackv_k ON lion_slackv USING lion (k);
INSERT INTO lion_slackv SELECT i, i % 8 FROM generate_series(1, 200000) i;
VACUUM lion_slackv;
SELECT slack_bytes AS slack_after_build FROM lion_index_stats('lion_slackv_k');
DELETE FROM lion_slackv WHERE i % 50 = 0;
VACUUM lion_slackv;
SELECT slack_bytes > 0 AS vacuum_left_slack,
	   slack_bytes < container_bytes AS slack_is_a_minority
  FROM lion_index_stats('lion_slackv_k');
SELECT lion_index_verify('lion_slackv_k', true);
SELECT lion_cmp('lion_slackv', 'k = 3');

-- The slack is reusable: new rows go into it without the index growing.
CREATE TEMP TABLE lion_slackv_size AS
	SELECT pg_relation_size('lion_slackv_k') AS bytes;
INSERT INTO lion_slackv SELECT i, i % 8 FROM generate_series(200001, 202000) i;
SELECT pg_relation_size('lion_slackv_k') <= (SELECT bytes FROM lion_slackv_size)
	   AS grew_into_the_slack;
SELECT lion_index_verify('lion_slackv_k', true);

/*
 * The same for the slack an INSERT leaves inside an INLINE entry (DESIGN.md
 * §4): VACUUM filters such a payload and writes it back into the bytes the
 * entry already has, so the two kinds of slack meet in one entry.  verify()
 * bounds both and requires every byte of them to be zero, so a passing
 * verify() is the assertion; what is pinned here is that the entries stay
 * INLINE and keep answering correctly.
 */
CREATE TABLE lion_slacki (i int4, k int4);
CREATE INDEX lion_slacki_k ON lion_slacki USING lion (k);
INSERT INTO lion_slacki SELECT i, i % 40 FROM generate_series(1, 4000) i;
SELECT inline_entries, inline_slack_bytes > 0 AS insert_left_slack
  FROM lion_index_stats('lion_slacki_k');
DELETE FROM lion_slacki WHERE i % 5 = 0;
VACUUM lion_slacki;
SELECT inline_entries, ntids,
	   inline_slack_bytes > 0 AS still_has_slack
  FROM lion_index_stats('lion_slacki_k');
SELECT lion_index_verify('lion_slacki_k', true);
SELECT lion_cmp('lion_slacki', 'k = 3');

-- ... and the next inserts go back into it.
INSERT INTO lion_slacki SELECT i, i % 40 FROM generate_series(4001, 4200) i;
SELECT lion_index_verify('lion_slacki_k', true);
SELECT lion_cmp('lion_slacki', 'k = 3');

/*
 * INLINE entries whose FIRST items lose nothing (DESIGN.md §18, "Measured,
 * 2026-09-23").  The filter makes one pass and asks the dead-TID callback
 * about every member once; the unchanged items in front of the first one that
 * loses a member are copied into the new payload from the page when that item
 * turns up.  So the deletes below touch only the LAST container of every
 * entry (the rows past 48000), or empty one container in the MIDDLE of an
 * entry and leave the ones after it alone, and every kind of item is there:
 * ARRAY containers (k), sparse segments (s) and RUN containers (r).
 */
CREATE TABLE lion_vacpre (i int4, k int4, s int4, r int4);
INSERT INTO lion_vacpre
SELECT i, i % 50, i % 3000, i / 1000 FROM generate_series(1, 60000) i;
CREATE INDEX lion_vacpre_k ON lion_vacpre USING lion (k);
CREATE INDEX lion_vacpre_s ON lion_vacpre USING lion (s);
CREATE INDEX lion_vacpre_r ON lion_vacpre USING lion (r);
VACUUM lion_vacpre;
SELECT entries, inline_entries, containers, array_containers, ntids
  FROM lion_index_stats('lion_vacpre_k');
SELECT entries, inline_entries, sparse_segments > 0 AS has_segments, ntids
  FROM lion_index_stats('lion_vacpre_s');
SELECT entries, inline_entries, run_containers > 0 AS has_runs, ntids
  FROM lion_index_stats('lion_vacpre_r');
DELETE FROM lion_vacpre WHERE i > 48000 AND i % 7 = 0;
DELETE FROM lion_vacpre WHERE k = 5 AND i BETWEEN 12001 AND 36000;
VACUUM lion_vacpre;
SELECT entries, inline_entries, containers, ntids,
	   ntids = (SELECT count(*) FROM lion_vacpre) AS ntids_matches_heap
  FROM lion_index_stats('lion_vacpre_k');
SELECT entries, inline_entries,
	   ntids = (SELECT count(*) FROM lion_vacpre) AS ntids_matches_heap
  FROM lion_index_stats('lion_vacpre_s');
SELECT entries, inline_entries,
	   ntids = (SELECT count(*) FROM lion_vacpre) AS ntids_matches_heap
  FROM lion_index_stats('lion_vacpre_r');
SELECT lion_index_verify('lion_vacpre_k', true);
SELECT lion_index_verify('lion_vacpre_s', true);
SELECT lion_index_verify('lion_vacpre_r', true);
SELECT lion_cmp('lion_vacpre', 'k = 5');
SELECT lion_cmp('lion_vacpre', 'k = 6');
SELECT lion_cmp('lion_vacpre', 's = 17');
SELECT lion_cmp('lion_vacpre', 'r = 55');
SELECT lion_cmp('lion_vacpre', 'r = 20');

/*
 * The same table vacuumed by parallel workers (DESIGN.md §18): lion indexes
 * declare VACUUM_OPTION_PARALLEL_BULKDEL, so with the size threshold out of
 * the way each of the three can be vacuumed by a worker.  Whether a worker is
 * actually free is up to the server, and the result must not depend on it;
 * test/isolation/vacuum_parallel.spec is where a worker is made to run one.
 */
SET min_parallel_index_scan_size = 0;
SET max_parallel_maintenance_workers = 2;
DELETE FROM lion_vacpre WHERE i % 11 = 0;
VACUUM (PARALLEL 2) lion_vacpre;
RESET min_parallel_index_scan_size;
RESET max_parallel_maintenance_workers;
SELECT ntids = (SELECT count(*) FROM lion_vacpre) AS k_ntids_matches_heap
  FROM lion_index_stats('lion_vacpre_k');
SELECT ntids = (SELECT count(*) FROM lion_vacpre) AS s_ntids_matches_heap
  FROM lion_index_stats('lion_vacpre_s');
SELECT ntids = (SELECT count(*) FROM lion_vacpre) AS r_ntids_matches_heap
  FROM lion_index_stats('lion_vacpre_r');
SELECT lion_index_verify('lion_vacpre_k', true);
SELECT lion_index_verify('lion_vacpre_s', true);
SELECT lion_index_verify('lion_vacpre_r', true);
SELECT lion_cmp('lion_vacpre', 'k = 5');
SELECT lion_cmp('lion_vacpre', 's = 17');
SELECT lion_cmp('lion_vacpre', 'r = 55');

/*
 * A filtered INLINE payload that grows past what a long key leaves of
 * LION_MAX_ENTRY_SIZE.  VACUUM kept a payload INLINE as long as it was within
 * inline_limit, and a ~2000-byte key leaves less than that: here 4060 rows on
 * 18 heap pages are one RUN container of 18 runs, and losing every other row
 * turns it into a 2030-member ARRAY, a 4068-byte payload next to a
 * 2032-byte header and key.  That VACUUM failed ("lion index entry of 6100
 * bytes is too large"), and failed again every time: a role that can only
 * INSERT and DELETE could so stop the table's VACUUMs, and with them
 * relfrozenxid, for good.  The set spills onto a container page instead,
 * exactly as an INSERT would spill it.
 */
CREATE TABLE lion_vacbig (i int4 NOT NULL);
INSERT INTO lion_vacbig SELECT i FROM generate_series(1, 4060) i;
CREATE INDEX lion_vacbig_k ON lion_vacbig
	USING lion ((lpad('', 1990, 'y') || (i * 0)::text));
SELECT inline_entries, run_containers, ntids FROM lion_index_stats('lion_vacbig_k');
DELETE FROM lion_vacbig WHERE i % 2 = 0;
VACUUM lion_vacbig;
SELECT inline_entries, array_containers, ntids FROM lion_index_stats('lion_vacbig_k');
SELECT lion_index_verify('lion_vacbig_k', true);
SELECT lion_index_count('lion_vacbig_k', lpad('', 1990, 'y') || '0') AS n;
-- ... and the spilled set takes the next rows and VACUUMs like any other
INSERT INTO lion_vacbig SELECT i FROM generate_series(4061, 4100) i;
DELETE FROM lion_vacbig WHERE i % 3 = 0;
VACUUM lion_vacbig;
SELECT inline_entries, ntids FROM lion_index_stats('lion_vacbig_k');
SELECT lion_index_verify('lion_vacbig_k', true);

DROP TABLE lion_vac, lion_vacrun, lion_vacsp, lion_free, lion_vnull, lion_slackv,
	lion_slacki, lion_vacpre, lion_vacbig;
