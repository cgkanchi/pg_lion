-- On-access pruning in the count's heap recheck (DESIGN.md §11, "On-access
-- pruning sets the visibility map too").
--
-- On PostgreSQL 19 and later heap_page_prune_opt() marks a page all-visible
-- when a read-only scan prunes it, and LionCount calls it for every heap block
-- its recheck reads.  So after HOT updates the FIRST pushed-down count
-- rechecks the dirty pages and leaves them all-visible, and the SECOND one
-- counts every block from the visibility map.  On 16-18 the call is not made
-- (pruning cannot set the map there) and the second count rechecks exactly
-- what the first one did.  Every answer is checked for exactness on every
-- version; the visibility-map effect is asserted as a boolean that is true on
-- both sides of the version line by construction, so there is one expected
-- file.
--
-- Nothing but the node under test may read the table between an UPDATE and
-- the counts that follow it: any core scan of it would prune the pages itself
-- on 19.  Hence every other scan type is off, and the reference answers are
-- constants worked out from the data rather than a second query.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
SET synchronous_commit = on;	-- all-visible needs flushed commits
CREATE EXTENSION IF NOT EXISTS pg_lion;

/*
 * The node's counters for one query, from EXPLAIN (ANALYZE, FORMAT JSON), and
 * the sum of its "count" column, from running it once more AFTER that (the
 * counters must be the first execution's).  Raises if the plan is not the
 * node.
 */
CREATE FUNCTION lion_pr_run(q text, OUT answer bigint, OUT skipped bigint,
							OUT rechecked bigint, OUT from_cache bigint)
LANGUAGE plpgsql AS $$
DECLARE
	p json;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, FORMAT JSON, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) '
		|| q INTO p;
	IF p -> 0 -> 'Plan' ->> 'Custom Plan Provider' IS DISTINCT FROM 'LionCount' THEN
		RAISE EXCEPTION 'not the node: %', p -> 0 -> 'Plan' ->> 'Node Type';
	END IF;
	skipped := (p -> 0 -> 'Plan' ->> 'Heap Blocks Skipped via VM')::bigint;
	rechecked := (p -> 0 -> 'Plan' ->> 'Heap Blocks Rechecked')::bigint;
	from_cache := (p -> 0 -> 'Plan' ->> 'Heap Blocks From Cache')::bigint;
	EXECUTE 'SELECT sum(count)::bigint FROM (' || q || ') s' INTO answer;
END $$;

/*
 * 19 and later: the pages are all-visible now.  Before: nothing changed.
 */
CREATE FUNCTION lion_pr_prunes() RETURNS boolean
LANGUAGE sql AS $$ SELECT current_setting('server_version_num')::int >= 190000 $$;

SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
SET enable_tidscan = off;

/*
 * Fixed-width rows (96 bytes plus a line pointer) and fillfactor 90: inserts
 * stop with a little under 10% of each page free, so a few HOT updates per
 * page take it below the 10% heap_page_prune_opt() wants before it prunes.
 * Only the LAST page is left with room to spare, and it is never updated.
 */
CREATE TABLE lion_pr (id int NOT NULL, k int NOT NULL, v int NOT NULL,
					  pad text NOT NULL)
	WITH (fillfactor = 90, autovacuum_enabled = off);
INSERT INTO lion_pr SELECT i, i % 4, 0, repeat('x', 60)
  FROM generate_series(1, 4000) i;
CREATE INDEX lion_pr_k ON lion_pr USING lion (k);
VACUUM (FREEZE) lion_pr;

/*
 * The rows a round of HOT updates touches: a few per page, never on the last
 * page.  Read before the update, while every page is all-visible and there is
 * nothing to prune.
 */
CREATE TEMP TABLE lion_pr_hot AS
SELECT id, (ctid::text::point)[0]::int AS blk FROM lion_pr
 WHERE id % 34 IN (1, 2)
   AND (ctid::text::point)[0] < (SELECT max((ctid::text::point)[0]) FROM lion_pr);
CREATE TEMP TABLE lion_pr_dirty AS SELECT count(DISTINCT blk) AS n FROM lion_pr_hot;
SELECT n > 10 AS enough_dirty_pages FROM lion_pr_dirty;

-- Round 1: a plain count, twice.
UPDATE lion_pr SET v = v + 1 WHERE id IN (SELECT id FROM lion_pr_hot);
CREATE TEMP TABLE lion_pr_r1 AS
SELECT 1 AS run, * FROM lion_pr_run('SELECT count(*) FROM lion_pr WHERE k = 1');
INSERT INTO lion_pr_r1
SELECT 2, * FROM lion_pr_run('SELECT count(*) FROM lion_pr WHERE k = 1');
SELECT a.answer = 1000 AND b.answer = 1000 AS exact,
	   a.rechecked = (SELECT n FROM lion_pr_dirty) AS first_rechecks_the_dirty_pages,
	   CASE WHEN lion_pr_prunes()
			THEN b.rechecked = 0 AND b.skipped = a.skipped + a.rechecked
			ELSE b.rechecked = a.rechecked AND b.skipped = a.skipped
	   END AS second_as_expected
  FROM lion_pr_r1 a, lion_pr_r1 b WHERE a.run = 1 AND b.run = 2;

-- Round 2: GROUP BY.  Every dirty page holds rows of every group, so the
-- first group's recheck cleans them all and, on 19, the other three groups
-- count them from the map in the same query.  Before 19 every group rechecks
-- them (the per-query visibility cache answers some of the visits).
UPDATE lion_pr SET v = v + 1 WHERE id IN (SELECT id FROM lion_pr_hot);
CREATE TEMP TABLE lion_pr_r2 AS
SELECT r.*, (SELECT n FROM lion_pr_dirty) AS dirty
  FROM lion_pr_run('SELECT k, count(*) FROM lion_pr GROUP BY k HAVING count(*) = 1000') r;
SELECT answer = 4000 AS exact,
	   CASE WHEN lion_pr_prunes()
			THEN rechecked = dirty AND from_cache = 0
			ELSE rechecked + from_cache = 4 * dirty
	   END AS later_groups_as_expected
  FROM lion_pr_r2;
SELECT k, count(*) FROM lion_pr GROUP BY k ORDER BY k;

-- Round 3: the SQL-callable count cannot see its statement, so it prunes but
-- never sets the map (rel_read_only = false): the second call rechecks exactly
-- what the first did, on every version.
UPDATE lion_pr SET v = v + 1 WHERE id IN (SELECT id FROM lion_pr_hot);
CREATE TEMP TABLE lion_pr_r3 AS
SELECT 1 AS run, * FROM lion_index_count_stats('lion_pr_k', 2);
INSERT INTO lion_pr_r3 SELECT 2, * FROM lion_index_count_stats('lion_pr_k', 2);
SELECT a.count = 1000 AND b.count = 1000 AS exact,
	   a.blocks_rechecked = (SELECT n FROM lion_pr_dirty) AS first_rechecks_the_dirty_pages,
	   b.blocks_rechecked = a.blocks_rechecked AS second_rechecks_them_again
  FROM lion_pr_r3 a, lion_pr_r3 b WHERE a.run = 1 AND b.run = 2;

-- Round 4: a count inside a statement that MODIFIES the table is not
-- read-only for it either: the scalar subquery below is the node, and it must
-- leave the map alone.  The UPDATE itself reaches one row by TID and changes
-- nothing (v is never above the count).
VACUUM (FREEZE) lion_pr;
UPDATE lion_pr SET v = v + 1 WHERE id IN (SELECT id FROM lion_pr_hot);
SET enable_tidscan = on;
CREATE FUNCTION lion_pr_uses_node(q text) RETURNS boolean
LANGUAGE plpgsql AS $$
DECLARE
	p json;
BEGIN
	EXECUTE 'EXPLAIN (FORMAT JSON, COSTS OFF) ' || q INTO p;
	RETURN p::text LIKE '%"Custom Plan Provider": "LionCount"%'
	   AND p::text LIKE '%"Node Type": "Tid Scan"%';
END $$;
SELECT lion_pr_uses_node($q$UPDATE lion_pr SET v = v WHERE ctid = '(0,1)'
   AND v > (SELECT count(*) FROM lion_pr WHERE k = 3)$q$) AS subquery_is_the_node;
UPDATE lion_pr SET v = v WHERE ctid = '(0,1)'
   AND v > (SELECT count(*) FROM lion_pr WHERE k = 3);
SET enable_tidscan = off;
SELECT answer = 1000 AS exact,
	   rechecked = (SELECT n FROM lion_pr_dirty) AS the_update_left_the_map_alone
  FROM lion_pr_run('SELECT count(*) FROM lion_pr WHERE k = 3');

-- And the reference answers, now that nothing depends on the pages any more.
RESET enable_seqscan;
SET pg_lion.enable_count_pushdown = off;
SELECT k, count(*) FROM lion_pr GROUP BY k ORDER BY k;
RESET pg_lion.enable_count_pushdown;

DROP TABLE lion_pr;
DROP FUNCTION lion_pr_run(text);
DROP FUNCTION lion_pr_prunes();
DROP FUNCTION lion_pr_uses_node(text);
RESET enable_bitmapscan;
RESET enable_indexscan;
RESET enable_indexonlyscan;
RESET enable_tidscan;
