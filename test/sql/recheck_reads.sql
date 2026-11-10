-- How a count reads the heap blocks the visibility map does not vouch for
-- (DESIGN.md §9, "The recheck's reads"): the tuple headers of a block's
-- candidates are asked for ahead of their turn, the blocks come through a
-- read stream from PostgreSQL 17 on - one that skips the blocks the
-- per-query visibility cache already answers - and a container's members on
-- such blocks are queued and counted in one pass.  None of it may change an
-- answer, so every count below is compared with a sequential scan, on a heap
-- whose not-all-visible pages hold every kind of root line pointer: plain
-- tuples, HOT chains, redirects left by pruning, dead and unused slots, rows
-- of an aborted insert, and pages that VACUUM left all-visible beside them.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
/* VACUUM can only set all-visible for commits that reached disk (citext.sql). */
SET synchronous_commit = on;

/*
 * lion_rrcmp() runs one query with the count pushdown, every other scan of
 * the table disabled, and again as a sequential scan, and reports whether
 * the pushdown answered it and whether the two agree as multisets.
 */
CREATE FUNCTION lion_rrcmp(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	a text;
	b text;
	pushed boolean := false;
	ln text;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			pushed := true;
		END IF;
	END LOOP;
	EXECUTE 'SELECT array_agg(r ORDER BY r)::text FROM (' || q || ') r' INTO a;
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	EXECUTE 'SELECT array_agg(r ORDER BY r)::text FROM (' || q || ') r' INTO b;
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH: %s against %s', left(a, 120), left(b, 120));
	END IF;
	RETURN CASE WHEN pushed THEN 'pushed down, ' ELSE 'NOT pushed down, ' END ||
		   'same answer';
END $$;

/*
 * 40 rows a page at fillfactor 50, so that updates stay on their page (HOT)
 * and a page holds chains as well as plain rows.
 */
CREATE TABLE lion_rr (id int, g int, k int, pad text) WITH (fillfactor = 50);
INSERT INTO lion_rr
SELECT i, i % 37, i % 3, repeat('x', 60) FROM generate_series(1, 30000) i;
CREATE INDEX lion_rr_g ON lion_rr USING lion (g);
CREATE INDEX lion_rr_k ON lion_rr USING lion (k);

-- HOT chains on every page, then VACUUM: it prunes them to redirects and
-- dead line pointers and leaves every page all-visible ...
UPDATE lion_rr SET pad = repeat('y', 60) WHERE id % 4 = 0;
UPDATE lion_rr SET pad = repeat('z', 60) WHERE id % 4 = 0;
VACUUM lion_rr;
-- ... and now part of the pages lose the bit again: new chains on top of the
-- redirects, deleted rows, the slots of an aborted insert, and pages left
-- all-visible in between (the update touches the first half of the table
-- only, the delete every page)
UPDATE lion_rr SET pad = repeat('w', 60) WHERE id % 8 = 0 AND id <= 15000;
DELETE FROM lion_rr WHERE id % 11 = 0 AND id <= 22000;
BEGIN;
INSERT INTO lion_rr SELECT i, i % 37, i % 3, 'aborted' FROM generate_series(30001, 31000) i;
ROLLBACK;
SELECT count = (SELECT count(*) FROM lion_rr WHERE k = 0) AS count_ok,
	   blocks_skipped > 0 AS some_pages_all_visible,
	   blocks_rechecked > 0 AS some_pages_not
  FROM lion_index_count_stats('lion_rr_k', 0);

-- a plain count, one value and an AND, and a grouped count whose groups
-- share every dirty page, so that the visibility cache answers the later
-- groups' blocks while it reads the rest (a stream skips those blocks)
SELECT lion_rrcmp('SELECT count(*) FROM lion_rr WHERE g = 5');
SELECT lion_rrcmp('SELECT count(*) FROM lion_rr WHERE g = 5 AND k = 1');
SELECT lion_rrcmp('SELECT count(*) FROM lion_rr WHERE k = 2');
SELECT lion_rrcmp('SELECT g, count(*) FROM lion_rr GROUP BY g');
SELECT lion_rrcmp('SELECT g, count(*) FROM lion_rr WHERE k = 0 GROUP BY g');
-- a count with a row filter, which reads every candidate's page
SELECT lion_rrcmp('SELECT count(*) FROM lion_rr WHERE g = 7 AND id % 5 = 1');
SELECT lion_rrcmp('SELECT g, count(*) FROM lion_rr WHERE k = 1 AND id % 5 = 1 GROUP BY g');

-- the same at the least work_mem: the recheck list and the cache are small,
-- so a count rechecks in several batches, each with its own stream, and the
-- cache runs out of room part way
SET work_mem = '64kB';
SELECT lion_rrcmp('SELECT count(*) FROM lion_rr WHERE g = 5');
SELECT lion_rrcmp('SELECT count(*) FROM lion_rr WHERE k = 2');
SELECT lion_rrcmp('SELECT g, count(*) FROM lion_rr GROUP BY g');
SELECT lion_rrcmp('SELECT k, count(*) FROM lion_rr WHERE g < 30 GROUP BY k');
SELECT lion_rrcmp('SELECT g, count(*) FROM lion_rr WHERE k = 1 AND id % 5 = 1 GROUP BY g');
RESET work_mem;

-- every page dirty: the whole set goes through the recheck
UPDATE lion_rr SET pad = repeat('v', 60) WHERE id % 20 = 1;
SELECT count = (SELECT count(*) FROM lion_rr WHERE k = 0) AS count_ok,
	   blocks_skipped AS pages_all_visible
  FROM lion_index_count_stats('lion_rr_k', 0);
SELECT lion_rrcmp('SELECT count(*) FROM lion_rr WHERE k = 1');
SELECT lion_rrcmp('SELECT g, count(*) FROM lion_rr GROUP BY g');

DROP TABLE lion_rr;
DROP FUNCTION lion_rrcmp(text);
