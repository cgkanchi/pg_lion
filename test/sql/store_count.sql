-- The count's shapes over the window store (DESIGN.md §40, "As built: Phase
-- B, the count's shapes").
--
-- LionCount counts the rows a lion WHERE keeps - or every row - one
-- container key at a time.  When the query also wants the values of a stored
-- column of those rows - count(DISTINCT c), GROUP BY c, or sum, avg, min,
-- max and count of c - the node now gathers them from the index's window
-- store as it counts: the rows of a heap page the visibility map calls
-- all-visible take the store's values, under the container pin that makes
-- the visibility map's word good for them (§9); every other row - a dirty
-- page, an ABSENT page, a window with no store - is fetched from the heap,
-- which then says both whether the row is visible and what its values are.
-- The values are then grouped and aggregated by hash.  Every query below is
-- checked against a sequential scan with the pushdown off; lsc_check() says
-- which plan ran and where the values came from.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

/*
 * lsc_check() runs q with every other scan disabled, so that a LionCount path
 * that is built at all is the plan, under EXPLAIN ANALYZE and again for its
 * rows; then as a sequential scan with the pushdown off, and proves the two
 * answers equal as multisets.  It says whether the node gathered from the
 * store (and which columns), walked without it, or was not used; whether the
 * gathered rows came from the store, the heap or both; whether a page of
 * them was ABSENT; and how many rows the query returned.
 */
CREATE FUNCTION lsc_check(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	used boolean := false;
	cols text := NULL;
	fromstore bigint := 0;
	fromheap bigint := 0;
	absent bigint := 0;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(LionCount\)' THEN
			used := true;
		ELSIF ln ~ '^\s*Store: ' THEN
			cols := substring(ln FROM 'Store: (.*)$');
		ELSIF ln ~ 'Store Rows From Heap: ' THEN
			fromheap := substring(ln FROM 'Store Rows From Heap: (\d+)')::bigint;
		ELSIF ln ~ 'Store Rows: ' THEN
			fromstore := substring(ln FROM 'Store Rows: (\d+)')::bigint;
		ELSIF ln ~ 'Store Pages Absent: ' THEN
			absent := substring(ln FROM 'Store Pages Absent: (\d+)')::bigint;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lsc_on AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE format('CREATE TEMP TABLE lsc_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	SELECT count(*) INTO nrows FROM lsc_on;
	SELECT (SELECT count(*) FROM (TABLE lsc_on EXCEPT ALL TABLE lsc_off) x) +
		   (SELECT count(*) FROM (TABLE lsc_off EXCEPT ALL TABLE lsc_on) y)
	  INTO ndiff;
	DROP TABLE lsc_on;
	DROP TABLE lsc_off;
	RETURN format('%s, %s rows, %s',
				  CASE WHEN cols IS NOT NULL THEN
					   format('store (%s): %s%s', cols,
							  CASE WHEN fromstore > 0 AND fromheap > 0 THEN 'store and heap'
								   WHEN fromstore > 0 THEN 'all from the store'
								   WHEN fromheap > 0 THEN 'all from the heap'
								   ELSE 'nothing gathered' END,
							  CASE WHEN absent > 0 THEN ', absent pages' ELSE '' END)
					   WHEN used THEN 'walk'
					   ELSE 'no pushdown' END,
				  nrows,
				  CASE WHEN ndiff = 0 THEN 'same as the sequential scan'
					   ELSE format('DIFFERENT in %s rows', ndiff) END);
END $$;

/*
 * lsc_plan() prints the plan of q, and under `actual` what it did, without
 * what differs between majors: the actual row counts (18 prints them with
 * decimals) and a subplan's name (19 names it expr_1 where 18 numbered it).
 */
CREATE FUNCTION lsc_plan(q text, actual boolean DEFAULT false) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE CASE WHEN actual THEN
			'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) '
			ELSE 'EXPLAIN (COSTS OFF) ' END || q LOOP
		ln := regexp_replace(ln, '\s*\(actual rows=[^)]*\)', '');
		ln := regexp_replace(ln, 'SubPlan \S+', 'SubPlan N');
		RETURN NEXT ln;
	END LOOP;
END $$;

-- A key column k, a multi-key column tags, and an INCLUDE column of each
-- type, every one with NULLs; three more key columns a, b, c in an index of
-- their own that stores their values.
CREATE TABLE lsc (id int, k int4, tags int4[], i2 int2, i4 int4, i8 int8,
				  d date, ts timestamptz, u uuid, n numeric, a int4, b int4,
				  c int4)
	WITH (autovacuum_enabled = off);
INSERT INTO lsc
SELECT g, g % 50, ARRAY[g % 11, g % 13],
	   CASE WHEN g % 17 = 0 THEN NULL ELSE (g % 300 - 150)::int2 END,
	   CASE WHEN g % 19 = 0 THEN NULL ELSE g % 1000 END,
	   CASE WHEN g % 23 = 0 THEN NULL ELSE g::int8 * 1000000007 END,
	   CASE WHEN g % 29 = 0 THEN NULL ELSE date '2024-01-01' + g % 90 END,
	   CASE WHEN g % 31 = 0 THEN NULL
			ELSE timestamptz '2024-01-01 00:00+00' + (g % 500) * interval '1 hour' END,
	   CASE WHEN g % 37 = 0 THEN NULL
			ELSE ('00000000-0000-0000-0000-' || lpad((g % 40)::text, 12, '0'))::uuid END,
	   CASE WHEN g % 41 = 0 THEN NULL ELSE (g % 70) / 8.0 END,
	   g % 7, g % 5, CASE WHEN g % 53 = 0 THEN NULL ELSE g % 3 END
  FROM generate_series(1, 30000) g;
CREATE INDEX lsc_k ON lsc USING lion (k, tags) INCLUDE (i2, i4, i8, d, ts, u, n);
CREATE INDEX lsc_abc ON lsc USING lion (a, b, c) WITH (store_values = true);
VACUUM (FREEZE, ANALYZE) lsc;

-- 1. The plans: the node gathers the columns the query wants, named on its
--    Store line, under a WHERE on k or on the multi-key tags, or with none.
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
EXPLAIN (COSTS OFF) SELECT i2, count(*) FROM lsc WHERE k < 5 GROUP BY i2;
EXPLAIN (COSTS OFF) SELECT count(DISTINCT u) FROM lsc WHERE k = 3;
EXPLAIN (COSTS OFF) SELECT sum(i8), avg(i2), min(d), max(ts), count(n) FROM lsc WHERE k IN (1, 2, 3);
EXPLAIN (COSTS OFF) SELECT d, count(DISTINCT u), min(ts), max(n) FROM lsc WHERE k = 3 GROUP BY d;
EXPLAIN (COSTS OFF) SELECT i2, d, count(*) FROM lsc WHERE tags @> ARRAY[3] GROUP BY i2, d;
EXPLAIN (COSTS OFF) SELECT i2, count(*) FROM lsc GROUP BY i2;
EXPLAIN (COSTS OFF) SELECT i2, count(*) FROM lsc WHERE k < 5 GROUP BY i2 HAVING count(*) > 10;
SELECT * FROM lsc_plan('SELECT i2, count(*) FROM lsc WHERE k = 3 GROUP BY i2', true);
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
RESET enable_indexonlyscan;

-- 2. GROUP BY a stored column under a WHERE, with count(*) and count(x) per
--    group: the NULL group is one group, and count(x) skips x's NULLs.
SELECT lsc_check('SELECT i2, count(*) AS n FROM lsc WHERE k < 5 GROUP BY i2');
SELECT lsc_check('SELECT i4, count(*) AS n, count(i8) AS n8 FROM lsc WHERE k = 7 GROUP BY i4');
SELECT lsc_check('SELECT count(*) AS n, d FROM lsc WHERE k BETWEEN 10 AND 12 GROUP BY d');
SELECT i2 IS NULL AS null_group, count(*) FROM lsc WHERE k < 5 AND (i2 IS NULL OR i2 = 1)
 GROUP BY i2 ORDER BY 1;
-- ... and count(DISTINCT c), which does not count a NULL.
SELECT lsc_check('SELECT count(DISTINCT i2) FROM lsc WHERE k < 5');
SELECT lsc_check('SELECT count(DISTINCT u), count(*), count(u) FROM lsc WHERE k = 3');
SELECT count(DISTINCT u), count(*), count(u) FROM lsc WHERE k = 3;
-- ... beside a GROUP BY, the distinct values of each group.
SELECT lsc_check('SELECT d, count(DISTINCT u) AS du, count(DISTINCT i4) AS di FROM lsc WHERE k = 3 GROUP BY d');

-- 3. Aggregates over a stored column, under a WHERE and a GROUP BY: sum and
--    avg of int2, int4 and int8 (the int8 sums pass int8's range), min and
--    max under the column's sort operator, all ignoring NULLs.
SELECT lsc_check('SELECT sum(i2), avg(i2), sum(i4), avg(i4), sum(i8), avg(i8) FROM lsc WHERE k < 25');
SELECT sum(i8), avg(i8) FROM lsc WHERE k < 25;
SELECT lsc_check('SELECT min(i2), max(i2), min(i8), max(i8), min(d), max(d), min(ts), max(ts), min(n), max(n) FROM lsc WHERE k IN (4, 9)');
SELECT lsc_check('SELECT i2, sum(i4), min(n), max(d), count(ts), count(DISTINCT u) FROM lsc WHERE k > 40 GROUP BY i2');
SELECT lsc_check('SELECT u, avg(i8), max(ts) FROM lsc WHERE k = 11 GROUP BY u');
-- ... and the HAVING the node applies itself.
SELECT lsc_check('SELECT i2, count(*) AS n FROM lsc WHERE k < 5 GROUP BY i2 HAVING count(*) > 10');
SELECT lsc_check('SELECT d, sum(i4) FROM lsc WHERE k < 5 GROUP BY d HAVING max(i4) > 990');

-- 4. Every type as a GROUP BY column and under count(DISTINCT): int2, int4,
--    int8, date, timestamptz, uuid and numeric.
SELECT lsc_check('SELECT i2, count(*) AS n FROM lsc WHERE k = 1 GROUP BY i2');
SELECT lsc_check('SELECT i4, count(*) AS n FROM lsc WHERE k = 1 GROUP BY i4');
SELECT lsc_check('SELECT i8, count(*) AS n FROM lsc WHERE k = 1 GROUP BY i8');
SELECT lsc_check('SELECT d, count(*) AS n FROM lsc WHERE k = 1 GROUP BY d');
SELECT lsc_check('SELECT ts, count(*) AS n FROM lsc WHERE k = 1 GROUP BY ts');
SELECT lsc_check('SELECT u, count(*) AS n FROM lsc WHERE k = 1 GROUP BY u');
SELECT lsc_check('SELECT n, count(*) AS n FROM lsc WHERE k = 1 GROUP BY n');
SELECT lsc_check('SELECT count(DISTINCT i2), count(DISTINCT i4), count(DISTINCT i8), count(DISTINCT d) FROM lsc WHERE k < 20');
SELECT lsc_check('SELECT count(DISTINCT ts), count(DISTINCT u), count(DISTINCT n) FROM lsc WHERE k < 20');
SELECT count(DISTINCT ts), count(DISTINCT u), count(DISTINCT n) FROM lsc WHERE k < 20;

-- 5. GROUP BY two stored columns, and a WHERE on the multi-key column tags:
--    an AND of its keys, and an OR.
SELECT lsc_check('SELECT i2, d, count(*) AS n, sum(i4) FROM lsc WHERE k < 10 GROUP BY i2, d');
SELECT lsc_check('SELECT d, u, count(*) AS n FROM lsc WHERE k = 6 GROUP BY u, d');
SELECT lsc_check('SELECT i2, d, count(*) AS n FROM lsc WHERE tags @> ARRAY[3] GROUP BY i2, d');
SELECT lsc_check('SELECT u, count(*) AS n, max(n) FROM lsc WHERE tags && ARRAY[3, 4] GROUP BY u');
SELECT lsc_check('SELECT count(DISTINCT d) FROM lsc WHERE tags @> ARRAY[2, 5] AND k > 20');
-- ... and a range on k beside it: walked inside, where a count alone could
--     subtract the entries outside it from every row, which a gather cannot
--     take back; and probed at the other clause's rows once they are fewer.
SELECT lsc_check('SELECT i2, count(*) AS n, max(ts) FROM lsc WHERE k > 2 AND tags && ARRAY[1, 2] GROUP BY i2');
SELECT lsc_check('SELECT u, count(*) AS n FROM lsc WHERE k < 45 AND tags @> ARRAY[3] GROUP BY u');
SELECT lsc_check('SELECT count(DISTINCT i4), sum(i8) FROM lsc WHERE k BETWEEN 1 AND 48 AND tags @> ARRAY[4, 6]');

-- 6. No WHERE: every row, counted over a driver column, gathered.
SELECT lsc_check('SELECT i2, count(*) AS n FROM lsc GROUP BY i2');
SELECT lsc_check('SELECT count(DISTINCT u), sum(i4), max(ts) FROM lsc');

-- 7. Key columns that are both walked and stored.  With no WHERE and no
--    GROUP BY, the aggregates over keys of §37 walk the entries and gather
--    nothing.  GROUP BY a, b, c with no WHERE: the decoded walk of §34 is the
--    cheaper; with it off the store is gathered.  Under a WHERE the store
--    beats the walks, which AND each group's posting sets with the WHERE.
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
EXPLAIN (COSTS OFF) SELECT sum(a), min(b), max(c) FROM lsc;
EXPLAIN (COSTS OFF) SELECT a, b, c, count(*) FROM lsc GROUP BY a, b, c;
SET pg_lion.enable_decoded_walk = off;
EXPLAIN (COSTS OFF) SELECT a, b, c, count(*) FROM lsc GROUP BY a, b, c;
RESET pg_lion.enable_decoded_walk;
EXPLAIN (COSTS OFF) SELECT a, b, c, count(*) FROM lsc WHERE k < 10 GROUP BY a, b, c;
EXPLAIN (COSTS OFF) SELECT a, count(DISTINCT c) FROM lsc WHERE k < 30 GROUP BY a;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
RESET enable_indexonlyscan;
SELECT lsc_check('SELECT sum(a), min(b), max(c) FROM lsc');
SELECT lsc_check('SELECT a, b, c, count(*) AS n FROM lsc GROUP BY a, b, c');
SET pg_lion.enable_decoded_walk = off;
SELECT lsc_check('SELECT a, b, c, count(*) AS n FROM lsc GROUP BY a, b, c');
RESET pg_lion.enable_decoded_walk;
SELECT lsc_check('SELECT a, b, c, count(*) AS n FROM lsc WHERE k < 10 GROUP BY a, b, c');
SELECT lsc_check('SELECT a, count(DISTINCT c), sum(c) FROM lsc WHERE k < 30 GROUP BY a');
-- ... a stored key column's values beside an INCLUDE column's: two indexes'
--    stores are not gathered together.
SELECT lsc_check('SELECT a, i2, count(*) AS n FROM lsc WHERE k = 3 GROUP BY a, i2');

-- 8. Dirty pages: rows updated, deleted and inserted since the VACUUM, and
--    rows this transaction changes; their pages' rows go to the heap, for
--    their visibility and their values, and the rest still to the store.
UPDATE lsc SET i2 = i2 + 1, d = d + 1, u = NULL WHERE id % 97 = 0;
DELETE FROM lsc WHERE id % 89 = 0;
INSERT INTO lsc (id, k, tags, i2, i4, i8, d, u, n)
SELECT g, g % 50, ARRAY[g % 11], (g % 9)::int2, g % 1000, g, date '2025-01-01', NULL, 1
  FROM generate_series(30001, 30500) g;
SELECT lsc_check('SELECT i2, count(*) AS n FROM lsc WHERE k < 5 GROUP BY i2');
SELECT lsc_check('SELECT d, count(DISTINCT u) AS du, sum(i8), min(i2) FROM lsc WHERE k BETWEEN 3 AND 9 GROUP BY d');
SELECT lsc_check('SELECT count(DISTINCT u), sum(i4), max(d) FROM lsc');
SELECT lsc_check('SELECT a, b, c, count(*) AS n FROM lsc WHERE k < 10 GROUP BY a, b, c');
BEGIN;
UPDATE lsc SET i2 = 999, d = date '1999-01-01' WHERE k = 4 AND id % 7 = 0;
DELETE FROM lsc WHERE k = 3 AND id % 5 = 0;
INSERT INTO lsc (id, k, i2, d) VALUES (40000, 4, -999, date '1990-01-01');
SELECT lsc_check('SELECT i2, d, count(*) AS n FROM lsc WHERE k IN (3, 4) GROUP BY i2, d');
SELECT lsc_check('SELECT min(i2), max(i2), min(d), count(DISTINCT d) FROM lsc WHERE k IN (3, 4)');
ROLLBACK;
-- ... and after VACUUM every page is all-visible again.
VACUUM lsc;
SELECT lsc_check('SELECT i2, count(*) AS n FROM lsc WHERE k < 5 GROUP BY i2');
SELECT lsc_check('SELECT d, count(DISTINCT u) AS du, sum(i8), min(i2) FROM lsc WHERE k BETWEEN 3 AND 9 GROUP BY d');
SELECT lsc_check('SELECT count(DISTINCT u), sum(i4), max(d) FROM lsc');

-- 9. Nothing matches: one row without a GROUP BY - counts 0, the rest NULL
--    - and none with one.
SELECT lsc_check('SELECT count(DISTINCT u), sum(i4), min(d), count(n), count(*) FROM lsc WHERE k = 1000');
SELECT count(DISTINCT u), sum(i4), min(d), count(n), count(*) FROM lsc WHERE k = 1000;
SELECT lsc_check('SELECT i2, count(*) FROM lsc WHERE k = 1000 GROUP BY i2');

-- 10. Run again: each probe of a correlated subquery is counted afresh.
SELECT * FROM lsc_plan($$SELECT x, (SELECT count(DISTINCT i2) FROM lsc WHERE k = x) AS di,
	   (SELECT max(d) FROM lsc WHERE k = x) AS md
  FROM generate_series(1, 4) x ORDER BY x$$);
SELECT x, (SELECT count(DISTINCT i2) FROM lsc WHERE k = x) AS di,
	   (SELECT max(d) FROM lsc WHERE k = x) AS md
  FROM generate_series(1, 4) x ORDER BY x;
SET pg_lion.enable_count_pushdown = off;
SELECT x, (SELECT count(DISTINCT i2) FROM lsc WHERE k = x) AS di,
	   (SELECT max(d) FROM lsc WHERE k = x) AS md
  FROM generate_series(1, 4) x ORDER BY x;
RESET pg_lion.enable_count_pushdown;

-- 11. Not gathered: an argument that is not a plain column, an expression of
--    a grouping column, a sum of numeric, a key column the index does not
--    store beside a stored one, a FILTER, a column no index stores, and a
--    hash table larger than hash_mem allows.
SELECT lsc_check('SELECT sum(i4 + 1) FROM lsc WHERE k < 5');
SELECT lsc_check('SELECT i2 + 1, count(*) FROM lsc WHERE k < 5 GROUP BY i2 + 1');
SELECT lsc_check('SELECT sum(n) FROM lsc WHERE k < 5');
SELECT lsc_check('SELECT k, i2, count(*) FROM lsc WHERE k < 3 GROUP BY k, i2');
SELECT lsc_check('SELECT count(DISTINCT i2) FILTER (WHERE i4 > 5) FROM lsc WHERE k < 3');
SELECT lsc_check('SELECT id, count(*) FROM lsc WHERE k = 3 GROUP BY id');
SET work_mem = 64;
SET hash_mem_multiplier = 1;
SELECT lsc_check('SELECT i8, count(*) FROM lsc WHERE k < 40 GROUP BY i8');
RESET work_mem;
RESET hash_mem_multiplier;

-- 12. An ABSENT window: a heap page whose values no store page can hold -
--     long distinct strings the heap compresses inline - is left to the
--     heap by the build, and its rows' values are fetched from there.
CREATE TABLE lsc_abs (id int, s text, f text)
	WITH (toast_tuple_target = 128, autovacuum_enabled = off);
INSERT INTO lsc_abs SELECT g, repeat(md5((g % 300)::text), 62), repeat('f', 100)
  FROM generate_series(1, 400) g;
INSERT INTO lsc_abs SELECT g, 'short ' || (g % 30) FROM generate_series(401, 900) g;
CREATE INDEX lsc_abs_i ON lsc_abs USING lion (id) INCLUDE (s);
VACUUM (FREEZE, ANALYZE) lsc_abs;
SELECT lsc_check('SELECT count(DISTINCT s), count(s) FROM lsc_abs WHERE id < 350');
SELECT lsc_check('SELECT count(DISTINCT s), count(*) FROM lsc_abs WHERE id > 300');
SELECT lsc_check('SELECT count(DISTINCT s), count(*) FROM lsc_abs WHERE id > 600');
SELECT count(DISTINCT s), count(*) FROM lsc_abs WHERE id > 300;

-- 13. A text column under a collation: min and max in its order, and a
--     case-insensitive one whose GROUP BY and count(DISTINCT) fold case -
--     where the build has ICU and the database is UTF8.
SET client_min_messages = warning;
DO $$
BEGIN
	IF getdatabaseencoding() = 'UTF8' THEN
		CREATE COLLATION lsc_en (provider = icu, locale = 'en');
		CREATE COLLATION lsc_ci (provider = icu, locale = 'und-u-ks-level2',
								 deterministic = false);
	END IF;
EXCEPTION WHEN feature_not_supported THEN
	NULL;						-- a build without ICU
END $$;
RESET client_min_messages;
SELECT NOT EXISTS (SELECT 1 FROM pg_collation WHERE collname = 'lsc_ci')
	   AS lsc_skip \gset
\if :lsc_skip
\echo 'no ICU collations, or not a UTF8 database: skipped'
\else
CREATE TABLE lsc_t (id int, k int, t text COLLATE lsc_en, ci text COLLATE lsc_ci)
	WITH (autovacuum_enabled = off);
INSERT INTO lsc_t
SELECT g, g % 20,
	   CASE WHEN g % 43 = 0 THEN NULL
			ELSE (ARRAY['apple', 'Banana', 'cherry', 'Apple', 'banana', 'Zebra'])[1 + g % 6] END,
	   CASE WHEN g % 47 = 0 THEN NULL
			ELSE (ARRAY['abc', 'ABC', 'Abc', 'xyz', 'XYZ'])[1 + g % 5] END
  FROM generate_series(1, 5000) g;
CREATE INDEX lsc_t_k ON lsc_t USING lion (k) INCLUDE (t, ci);
VACUUM (FREEZE, ANALYZE) lsc_t;
SELECT lsc_check('SELECT min(t), max(t) FROM lsc_t WHERE k < 10');
SELECT min(t), max(t), min(t COLLATE "C"), max(t COLLATE "C") FROM lsc_t WHERE k < 10;
SELECT lsc_check('SELECT t, count(*) AS n FROM lsc_t WHERE k < 10 GROUP BY t');
SELECT lsc_check('SELECT count(*) AS n FROM lsc_t WHERE k < 10 GROUP BY ci');
SELECT lsc_check('SELECT count(DISTINCT ci), count(DISTINCT t) FROM lsc_t WHERE k < 10');
SELECT count(DISTINCT ci), count(DISTINCT t) FROM lsc_t WHERE k < 10;
SELECT lower(ci) AS ci, count(*) FROM lsc_t WHERE k < 10 GROUP BY ci ORDER BY 1;
UPDATE lsc_t SET ci = upper(ci), t = upper(t) WHERE id > 4500;
SELECT lsc_check('SELECT count(*) AS n FROM lsc_t WHERE k < 10 GROUP BY ci');
SELECT lsc_check('SELECT count(DISTINCT ci), min(t), max(t) FROM lsc_t WHERE k < 10');
DROP TABLE lsc_t;
\endif

DROP TABLE lsc, lsc_abs;
DROP FUNCTION lsc_check(text), lsc_plan(text, boolean);
SET client_min_messages = warning;
DROP COLLATION IF EXISTS lsc_en;
DROP COLLATION IF EXISTS lsc_ci;
RESET client_min_messages;
