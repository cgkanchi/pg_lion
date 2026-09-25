-- What the count's cursors hold open: memory and buffer pins (DESIGN.md §15,
-- "Bounded cursors"; 2026-09-25 review).
--
-- The count's evaluator built a cursor for every leaf of an expression at
-- once, and an IN list is a leaf per value.  Each held ~17 kB (an 8 kB staging
-- buffer for an INLINE entry, a page image for a CHAIN one, a second 8 kB for
-- a sparse segment) and each CHAIN cursor a buffer pin, whatever work_mem
-- said: `k = ANY (50000 values) AND x = 1` over 300k rows peaked at 880 MB,
-- and over a temporary table a list of 1100 CHAIN entries ran out of local
-- buffers.  Now the staging buffers are sized for their items, a disjoint
-- list too big for the budget is counted a batch of entries at a time, and
-- any other union too big for it is read as a pinless windowed union.  Every
-- count below must still be exact, across every batch and window boundary.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
/* VACUUM can only set all-visible for commits that reached disk (citext.sql). */
SET synchronous_commit = on;

/*
 * lion_cbcmp() runs one query with the count pushdown and again with a
 * sequential scan, and reports whether the pushdown answered it and whether
 * the two agree as multisets.
 */
CREATE FUNCTION lion_cbcmp(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	a text;
	b text;
	pushed boolean := false;
	ln text;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			pushed := true;
		END IF;
	END LOOP;
	EXECUTE 'SELECT array_agg(r ORDER BY r)::text FROM (' || q || ') r' INTO a;
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	EXECUTE 'SELECT array_agg(r ORDER BY r)::text FROM (' || q || ') r' INTO b;
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH: %s against %s', left(a, 120), left(b, 120));
	END IF;
	RETURN CASE WHEN pushed THEN 'pushed down, ' ELSE 'NOT pushed down, ' END ||
		   'same answer';
END $$;

/*
 * The table: a posting set of 30 rows per key, spread over 4300 heap pages
 * (67 container keys, so that a union window of 31 images - work_mem 64kB -
 * takes three windows).  k's index is CHAIN (inline_limit = 64): each entry is
 * one sparse segment on a posting page of its own.  ki holds the same values
 * in an index of INLINE entries.  kx is a two-column index for the bitmap
 * scan.
 */
CREATE TABLE lion_cb (id int, k int, ki int, j int, x int, t int,
					  tags int[], pad text);
INSERT INTO lion_cb
	SELECT g, g % 3000, g % 3000, g % 500, g % 7, g % 3,
		   ARRAY[g % 997, g % 13 + 2000], repeat('p', 300)
	  FROM generate_series(1, 90000) g;
CREATE INDEX lion_cb_k ON lion_cb USING lion (k) WITH (inline_limit = 64);
CREATE INDEX lion_cb_ki ON lion_cb USING lion (ki);
CREATE INDEX lion_cb_j ON lion_cb USING lion (j);
CREATE INDEX lion_cb_x ON lion_cb USING lion (x);
CREATE INDEX lion_cb_t ON lion_cb USING lion (t);
CREATE INDEX lion_cb_tags ON lion_cb USING lion (tags) WITH (inline_limit = 64);
CREATE INDEX lion_cb_kx ON lion_cb USING lion (k, x) WITH (inline_limit = 64);
VACUUM ANALYZE lion_cb;
SELECT pg_relation_size('lion_cb') / current_setting('block_size')::int / 64 > 62 AS many_container_keys;
SELECT entries = 3000 AND inline_entries = 0 AS k_is_chain FROM lion_index_stats('lion_cb_k');
SELECT entries = 3000 AND inline_entries = 3000 AS ki_is_inline FROM lion_index_stats('lion_cb_ki');

/*
 * work_mem 64kB puts the open budget at its floor, 256 kB: a batch of some
 * thirty CHAIN sets (a page image each) or four hundred INLINE ones, and a
 * union window of 31 images.  The answers do not depend on it; that is the
 * point.
 */
SET work_mem = '64kB';

-- ---------- a disjoint list ANDed with another clause, in batches ----------
-- every length from one set to four batches, CHAIN entries
SELECT n, lion_cbcmp(format(
	'SELECT count(*) FROM lion_cb WHERE k = ANY (array(SELECT (g * 7) %% 3000 FROM generate_series(1, %s) g)) AND x = 3', n))
  FROM generate_series(1, 121, 8) n;
-- INLINE entries, around the first batch boundaries and far past them
SELECT n, lion_cbcmp(format(
	'SELECT count(*) FROM lion_cb WHERE ki = ANY (array(SELECT (g * 7) %% 3000 FROM generate_series(1, %s) g)) AND x = 3', n))
  FROM (VALUES (390), (399), (400), (401), (410), (799), (801), (3000)) v(n);
-- a dense list with nothing else: the merge, not the sum, and batched
SELECT lion_cbcmp('SELECT count(*) FROM lion_cb WHERE j = ANY (array(SELECT g FROM generate_series(1, 400) g))');
SELECT lion_cbcmp('SELECT count(*) FROM lion_cb WHERE k = ANY (array(SELECT g FROM generate_series(1, 2999) g))');
-- NULLs and repeated values
SELECT lion_cbcmp('SELECT count(*) FROM lion_cb WHERE k = ANY (array(SELECT CASE WHEN g % 5 = 0 THEN NULL ELSE g % 400 END FROM generate_series(1, 1200) g)) AND t = 1');
-- a negated source
SELECT lion_cbcmp('SELECT count(*) FROM lion_cb WHERE k = ANY (array(SELECT g FROM generate_series(1, 500) g)) AND x IS NOT NULL');
-- two lists: one is batched, the other read as a windowed union
SELECT lion_cbcmp('SELECT count(*) FROM lion_cb WHERE k = ANY (array(SELECT g FROM generate_series(1, 900) g)) AND j = ANY (array(SELECT g FROM generate_series(1, 300) g))');
-- GROUP BY another column: the list is batched for every group
SELECT lion_cbcmp('SELECT x, count(*) FROM lion_cb WHERE k = ANY (array(SELECT g FROM generate_series(1, 700) g)) GROUP BY x');
SELECT lion_cbcmp('SELECT t, count(*) FROM lion_cb WHERE k = ANY (array(SELECT g FROM generate_series(1, 700) g)) AND x = 2 GROUP BY t');
-- GROUP BY the list's own column, and count(DISTINCT)
SELECT lion_cbcmp('SELECT k, count(*) FROM lion_cb WHERE k = ANY (array(SELECT g FROM generate_series(1, 300) g)) AND x = 2 GROUP BY k');
SELECT lion_cbcmp('SELECT count(DISTINCT t) FROM lion_cb WHERE k = ANY (array(SELECT g FROM generate_series(1, 700) g)) AND x = 5');

-- ---------- unions that are not disjoint: windowed ----------
-- an OR across columns (DESIGN.md §19), alone and ANDed with another clause
SELECT lion_cbcmp('SELECT count(*) FROM lion_cb WHERE k = ANY (array(SELECT g FROM generate_series(1, 600) g)) OR x = 1');
SELECT lion_cbcmp('SELECT count(*) FROM lion_cb WHERE (k = ANY (array(SELECT g FROM generate_series(1, 600) g)) OR x = 1) AND t = 2');
SELECT lion_cbcmp('SELECT count(*) FROM lion_cb WHERE (k = ANY (array(SELECT g * 3 FROM generate_series(1, 900) g)) OR j = ANY (array(SELECT g FROM generate_series(1, 200) g))) AND t = 0');
-- a multi-key OR (&&) of many CHAIN keys, and an AND (@>) of many
SELECT lion_cbcmp(format('SELECT count(*) FROM lion_cb WHERE tags && %L::int[]',
						 (SELECT array_agg(g) FROM generate_series(1, 600) g)));
SELECT lion_cbcmp(format('SELECT count(*) FROM lion_cb WHERE tags && %L::int[] AND x = 2',
						 (SELECT array_agg(g) FROM generate_series(1, 600) g)));
SELECT lion_cbcmp(format('SELECT count(*) FROM lion_cb WHERE tags @> %L::int[]',
						 (SELECT array_agg(g) FROM generate_series(1, 40) g)));
-- the bitmap scan of a two-column index walks the same evaluator
SET pg_lion.enable_count_pushdown = off;
SET enable_seqscan = off;
SET enable_indexscan = off;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM lion_cb WHERE k = ANY (array(SELECT g FROM generate_series(1, 2000) g)) AND x = 1;
SELECT count(*) FROM lion_cb WHERE k = ANY (array(SELECT g FROM generate_series(1, 2000) g)) AND x = 1;
RESET enable_indexscan;
SET enable_bitmapscan = off;
RESET enable_seqscan;
SELECT count(*) FROM lion_cb WHERE k = ANY (array(SELECT g FROM generate_series(1, 2000) g)) AND x = 1;
RESET enable_bitmapscan;
RESET pg_lion.enable_count_pushdown;

-- ---------- the same over a dirty heap: rechecks span the batches ----------
UPDATE lion_cb SET x = (x + 1) % 7 WHERE id % 11 = 0;
DELETE FROM lion_cb WHERE id % 13 = 0;
SELECT n, lion_cbcmp(format(
	'SELECT count(*) FROM lion_cb WHERE k = ANY (array(SELECT (g * 7) %% 3000 FROM generate_series(1, %s) g)) AND x = 3', n))
  FROM generate_series(1, 121, 24) n;
SELECT lion_cbcmp('SELECT count(*) FROM lion_cb WHERE k = ANY (array(SELECT g FROM generate_series(1, 2999) g))');
SELECT lion_cbcmp('SELECT count(*) FROM lion_cb WHERE k = ANY (array(SELECT g FROM generate_series(1, 600) g)) OR x = 1');
SELECT lion_cbcmp('SELECT x, count(*) FROM lion_cb WHERE k = ANY (array(SELECT g FROM generate_series(1, 700) g)) GROUP BY x');
SELECT lion_cbcmp(format('SELECT count(*) FROM lion_cb WHERE tags && %L::int[]',
						 (SELECT array_agg(g) FROM generate_series(1, 600) g)));

RESET work_mem;

-- ---------- memory ----------
/*
 * The high-water mark of this backend's resident memory (Linux; elsewhere the
 * file is missing and every delta below reads 0).  It counts the shared
 * buffers a backend touches, so each query below is preceded by a cheaper one
 * over the same pages - the disjoint SUM of the same list, which holds one
 * set's cursor at a time - and measured in a fresh session, whose high-water
 * mark nothing earlier has raised.
 */
CREATE TABLE lion_cbm (k int, ki int, x int);
INSERT INTO lion_cbm SELECT g % 5000, g % 30000, g % 7 FROM generate_series(1, 100000) g;
CREATE INDEX lion_cbm_k ON lion_cbm USING lion (k) WITH (inline_limit = 64);
CREATE INDEX lion_cbm_ki ON lion_cbm USING lion (ki);
CREATE INDEX lion_cbm_x ON lion_cbm USING lion (x);
CREATE INDEX lion_cbm_kx ON lion_cbm USING lion (k, x) WITH (inline_limit = 64);
VACUUM ANALYZE lion_cbm;
SELECT inline_entries = 0 AS k_is_chain FROM lion_index_stats('lion_cbm_k');
CREATE FUNCTION lion_cb_hwm() RETURNS bigint
LANGUAGE sql AS $$
	SELECT COALESCE((regexp_match(pg_read_file('/proc/self/status', true),
								  'VmHWM:\s+(\d+) kB'))[1]::bigint, 0)
$$;
-- 5000 CHAIN sets ANDed with x: a page image each, over 40 MB opened at once
\connect -
SET work_mem = '4MB';
SELECT count(*) FROM lion_cbm WHERE k = ANY (array(SELECT generate_series(1, 5000)));
SELECT count(*) FROM lion_cbm WHERE k = ANY (array(SELECT generate_series(1, 10))) AND x = 1;
SELECT lion_cb_hwm() AS lion_cb_h0 \gset
SELECT count(*) FROM lion_cbm WHERE k = ANY (array(SELECT generate_series(1, 5000))) AND x = 1;
SELECT lion_cb_hwm() - :lion_cb_h0 < 16 * 1024 AS chain_list_within_16mb;
-- 30000 INLINE sets ANDed with x: 499 MB before the staging buffers were sized
\connect -
SET work_mem = '4MB';
SELECT count(*) FROM lion_cbm WHERE ki = ANY (array(SELECT generate_series(1, 30000)));
SELECT count(*) FROM lion_cbm WHERE ki = ANY (array(SELECT generate_series(1, 10))) AND x = 1;
SELECT lion_cb_hwm() AS lion_cb_h0 \gset
SELECT count(*) FROM lion_cbm WHERE ki = ANY (array(SELECT generate_series(1, 30000))) AND x = 1;
SELECT lion_cb_hwm() - :lion_cb_h0 < 48 * 1024 AS inline_list_within_48mb;
-- the bitmap scan of the two-column index, the same 5000 CHAIN sets
-- (its pages and the heap's warmed by lion_index_stats() and a seqscan)
\connect -
SET work_mem = '4MB';
SET pg_lion.enable_count_pushdown = off;
SET enable_seqscan = off;
SET enable_indexscan = off;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM lion_cbm WHERE k = ANY (array(SELECT generate_series(1, 5000))) AND x = 1;
SELECT bool_and(entries > 0) AS warm FROM lion_index_stats('lion_cbm_kx');
RESET enable_seqscan;
SELECT count(*) FROM lion_cbm;
SET enable_seqscan = off;
SELECT count(*) FROM lion_cbm WHERE k = ANY (array(SELECT generate_series(1, 10))) AND x = 1;
SELECT lion_cb_hwm() AS lion_cb_h0 \gset
SELECT count(*) FROM lion_cbm WHERE k = ANY (array(SELECT generate_series(1, 5000))) AND x = 1;
SELECT lion_cb_hwm() - :lion_cb_h0 < 16 * 1024 AS bitmap_within_16mb;

-- ---------- buffer pins: a temporary table in 100 local buffers ----------
/*
 * temp_buffers can only be set before the session touches a temporary table,
 * hence the fresh session.  100 buffers is the smallest pool there is, and the
 * list pin budget is an eighth of it: a list of 1100 CHAIN entries pinned
 * 1100 posting pages and failed with "no empty local buffer available"; a
 * list of INLINE entries on 1.9 kB keys pinned a directory leaf per value or
 * two, the budget being an eighth of shared_buffers.
 */
\connect -
SET temp_buffers = 100;
SET synchronous_commit = on;
CREATE TEMP TABLE lion_cbt (k int, x int);
INSERT INTO lion_cbt SELECT g % 2000, g % 7 FROM generate_series(1, 200000) g;
CREATE INDEX lion_cbt_k ON lion_cbt USING lion (k) WITH (inline_limit = 64);
CREATE INDEX lion_cbt_x ON lion_cbt USING lion (x);
VACUUM ANALYZE lion_cbt;
SELECT lion_cbcmp('SELECT count(*) FROM lion_cbt WHERE k = ANY (array(SELECT generate_series(1, 1100))) AND x = 1');
SELECT lion_cbcmp('SELECT count(*) FROM lion_cbt WHERE k = ANY (array(SELECT generate_series(1, 1100)))');
SELECT lion_cbcmp('SELECT x, count(*) FROM lion_cbt WHERE k = ANY (array(SELECT generate_series(1, 1100))) GROUP BY x');
SELECT lion_cbcmp('SELECT count(*) FROM lion_cbt WHERE k = ANY (array(SELECT generate_series(1, 1100))) OR x = 1');
CREATE TEMP TABLE lion_cbw (k text, x int);
INSERT INTO lion_cbw SELECT repeat(md5(i::text), 59) || i, i % 2 FROM generate_series(1, 600) i;
CREATE INDEX lion_cbw_k ON lion_cbw USING lion (k);
CREATE INDEX lion_cbw_x ON lion_cbw USING lion (x);
VACUUM ANALYZE lion_cbw;
SELECT pg_relation_size('lion_cbw_k') / current_setting('block_size')::int > 250 AS many_leaves;
SELECT lion_index_count_any('lion_cbw_k', (SELECT array_agg(k) FROM lion_cbw)) AS count_any;
SELECT lion_cbcmp('SELECT count(*) FROM lion_cbw WHERE k = ANY ((SELECT array_agg(k) FROM lion_cbw)::text[]) AND x = 1');
SELECT lion_cbcmp('SELECT count(*) FROM lion_cbw WHERE k = ANY ((SELECT array_agg(k) FROM lion_cbw)::text[]) OR x = 1');

DROP TABLE lion_cbt, lion_cbw;
DROP TABLE lion_cb, lion_cbm;
DROP FUNCTION lion_cbcmp(text);
DROP FUNCTION lion_cb_hwm();
