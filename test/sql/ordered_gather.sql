-- The gather of a leaf's pieces by container key (DESIGN.md §30.4,
-- "Gathering the pieces"): a WALK or a LIST source hands out a piece per
-- entry and container key, and the gather takes each key's members as they
-- come - a cache line a key, whose room is flushed into the key's container
-- when it is full: merged member by member while the key holds a few,
-- through a bitset image past that, and into a BITSET's bits in place once
-- it holds more than an ARRAY does - and gives the set its keys in order.
-- Every answer is compared, IN ORDER, with the ordinary plan's.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

/*
 * og_cmp() runs q through the node - every core scan disabled, so that
 * LionOrdered is the only path left - and through the ordinary plan (the
 * node off, every core scan enabled), compares the two answers in order,
 * and says whether the first plan really was the node; a prepared
 * statement's cached plan does not change with the GUC, so for one the
 * ordinary answer comes from the query it stands for, given as `ordinary`.
 * og_run() runs q through the node under EXPLAIN ANALYZE and prints the
 * node's counters.
 */
CREATE FUNCTION og_cmp(q text, ordinary text DEFAULT NULL) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	r record;
	used boolean := false;
	a text[] := '{}';
	b text[] := '{}';
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('enable_tidscan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(LionOrdered\)' THEN
			used := true;
		END IF;
	END LOOP;
	FOR r IN EXECUTE q LOOP
		a := a || r::text;
	END LOOP;

	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('enable_tidscan', 'on', true);
	PERFORM set_config('pg_lion.enable_ordered_scan', 'off', true);
	FOR r IN EXECUTE coalesce(ordinary, q) LOOP
		b := b || r::text;
	END LOOP;
	PERFORM set_config('pg_lion.enable_ordered_scan', 'on', true);

	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH: %s rows through the node, %s ordinary; first %s / %s',
					  coalesce(array_length(a, 1), 0), coalesce(array_length(b, 1), 0),
					  a[1], b[1]);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN used THEN 'LionOrdered' ELSE 'no LionOrdered' END,
				  coalesce(array_length(a, 1), 0));
END $$;

CREATE FUNCTION og_run(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF ln ~ '(LionOrdered|Index Entries Walked|Lion Set|Heap Fetches|Rows Removed by|Switched)' THEN
			RETURN NEXT btrim(regexp_replace(ln, '\s*\(actual.*\)', ''));
		END IF;
	END LOOP;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
END $$;

-- ogn: 400,000 narrow rows, 88 a heap page, so a container key of 64 pages
-- holds some 5,600 of them and the table has 72.  u: nearly unique, in no
-- heap order, so that a range brings a piece a member; g: 100 values of
-- 4,000 consecutive rows each, so that a range's pieces are RUNs of hundreds
-- of members; tags: a tag from 7 and one from 5 a row, so that `&&` over one
-- of each brings some rows twice; k: a permutation of the ids, with the
-- btree the node walks.
CREATE TABLE ogn (id int PRIMARY KEY, u float8, g int, tags text[], k int)
	WITH (autovacuum_enabled = off);
INSERT INTO ogn
SELECT i,
	   ((hashint8extended(i::bigint, 5) & 9223372036854775807) % 1000000) / 1000000.0,
	   (i - 1) / 4000,
	   ARRAY['t' || (i % 7), 's' || (i % 5)],
	   (i::bigint * 7919 % 400000)::int
  FROM generate_series(1, 400000) i;
CREATE INDEX ogn_u ON ogn USING lion (u);
CREATE INDEX ogn_g ON ogn USING lion (g);
CREATE INDEX ogn_tags ON ogn USING lion (tags);
CREATE INDEX ogn_k ON ogn (k, id);
VACUUM (FREEZE, ANALYZE) ogn;
SELECT pg_relation_size('ogn') / 8192 / 64 AS keys;

-- 1. A few members a key (170): merged member by member as the rooms fill.
SELECT og_cmp('SELECT id, k FROM ogn WHERE u < 0.03 ORDER BY k, id');
SELECT * FROM og_run('SELECT id FROM ogn WHERE u < 0.03 ORDER BY k, id LIMIT 5');

-- 2. More a key (560) than the merge takes (LO_MERGE_DIRECT_MAX): through
--    the image, the container an ARRAY still.
SELECT og_cmp('SELECT id, k FROM ogn WHERE u < 0.1 ORDER BY k, id LIMIT 20000');
SELECT * FROM og_run('SELECT id FROM ogn WHERE u < 0.1 ORDER BY k, id LIMIT 5');

-- 3. More a key (2,800) than an ARRAY holds: a BITSET whose bits the
--    flushes set in place, made the smallest container at the end.
SELECT og_cmp('SELECT id, k FROM ogn WHERE u < 0.5 ORDER BY k DESC, id DESC LIMIT 20000');
SELECT * FROM og_run('SELECT id FROM ogn WHERE u < 0.5 ORDER BY k, id LIMIT 5');

-- 4. Pieces of hundreds of members, RUNs: each goes into the container
--    with the room's members, and a key of many is a BITSET.
SELECT og_cmp('SELECT id, k FROM ogn WHERE g BETWEEN 10 AND 13 ORDER BY k, id');
SELECT * FROM og_run('SELECT id FROM ogn WHERE g BETWEEN 10 AND 13 ORDER BY k, id LIMIT 5');
SELECT og_cmp('SELECT id, k FROM ogn WHERE g BETWEEN 10 AND 40 ORDER BY k DESC, id DESC LIMIT 20000');
SELECT * FROM og_run('SELECT id FROM ogn WHERE g BETWEEN 10 AND 40 ORDER BY k, id LIMIT 5');

-- 5. Repeats: a row with both tags comes from two entries, once in the set.
SELECT og_cmp($$SELECT id, k FROM ogn WHERE tags && ARRAY['t1', 's2'] ORDER BY k, id LIMIT 20000$$);
SELECT * FROM og_run($$SELECT id FROM ogn WHERE tags && ARRAY['t1', 's2'] ORDER BY k, id LIMIT 5$$);

-- 6. Two gathered leaves, ANDed.
SELECT og_cmp('SELECT id, k FROM ogn WHERE u < 0.5 AND g BETWEEN 10 AND 40 ORDER BY k, id LIMIT 20000');
SELECT * FROM og_run('SELECT id FROM ogn WHERE u < 0.5 AND g BETWEEN 10 AND 40 ORDER BY k, id LIMIT 5');

-- 7. At a 64 kB hash_mem the keys of 3. outgrow it while they are gathered
--    (planned at the default, as a generic plan the planner would not make
--    at 64 kB): the set degrades to its keys, and every member is rechecked.
--    The keys of 1. fit, a container each.
SET plan_cache_mode = force_generic_plan;
PREPARE og_big AS SELECT id, k FROM ogn WHERE u < 0.5 ORDER BY k, id LIMIT 20000;
SET enable_seqscan = off; SET enable_bitmapscan = off;
SET enable_indexscan = off; SET enable_indexonlyscan = off;
EXPLAIN (COSTS OFF) EXECUTE og_big;
RESET enable_seqscan; RESET enable_bitmapscan;
RESET enable_indexscan; RESET enable_indexonlyscan;
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SELECT * FROM og_run('EXECUTE og_big');
SELECT og_cmp('EXECUTE og_big',
			  'SELECT id, k FROM ogn WHERE u < 0.5 ORDER BY k, id LIMIT 20000');
SELECT og_cmp('SELECT id, k FROM ogn WHERE u < 0.03 ORDER BY k, id');
SELECT * FROM og_run('SELECT id FROM ogn WHERE u < 0.03 ORDER BY k, id LIMIT 5');
RESET hash_mem_multiplier;
RESET work_mem;
DEALLOCATE og_big;
RESET plan_cache_mode;
DROP TABLE ogn;

-- og: 16,640 rows of 2 kB, four a heap page, 65 container keys: the
-- gather's table starts with room for one chunk of 64 keys and grows for
-- the last one.  u's index has summaries of 64 rows, the smallest pieces.
CREATE TABLE og (id int PRIMARY KEY, u float8, k int, pad text)
	WITH (autovacuum_enabled = off);
ALTER TABLE og ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO og
SELECT i,
	   ((hashint8extended(i::bigint, 6) & 9223372036854775807) % 1000000) / 1000000.0,
	   (i::bigint * 7919 % 16640)::int,
	   repeat('x', 1900)
  FROM generate_series(1, 16640) i;
CREATE INDEX og_u ON og USING lion (u) WITH (summaries = 'on', summary_tids = '64');
CREATE INDEX og_k ON og (k, id);
VACUUM (FREEZE, ANALYZE) og;
SELECT pg_relation_size('og') / 8192 / 64 AS keys;

-- 8. A range over a third of the rows, and one over a few rows of most keys.
SELECT og_cmp('SELECT id, k FROM og WHERE u < 0.3 ORDER BY k, id');
SELECT * FROM og_run('SELECT id FROM og WHERE u < 0.3 ORDER BY k, id LIMIT 5');
SELECT og_cmp('SELECT id, k FROM og WHERE u >= 0.99 ORDER BY k DESC, id DESC');
SELECT * FROM og_run('SELECT id FROM og WHERE u >= 0.99 ORDER BY k DESC, id DESC LIMIT 5');
DROP TABLE og;

DROP FUNCTION og_cmp(text, text);
DROP FUNCTION og_run(text);
