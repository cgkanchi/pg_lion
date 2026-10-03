-- The window store's gather past hash_mem (DESIGN.md §40, "As built:
-- spilling").
--
-- The gather's hash table of groups and count(DISTINCT) pairs is held to
-- hash_mem as a HashAggregate's is.  Once it is full the pass goes on in
-- spill mode: a row of a group the table does not hold is written to a batch
-- file chosen by bits of its group's hash, and read back by a pass of its
-- own once the table's groups have gone out; a count(DISTINCT) value that a
-- group in the table has not had is written to a batch of pairs, counted
-- into its group before the group goes out.  A batch that does not fit
-- either spills again, by the next bits.  Every answer here is compared with
-- the plan with the pushdown off, under a work_mem of 64kB that the tables
-- are many times larger than, and again at the default, where nothing
-- spills.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

/*
 * lss_check() runs q with every other scan disabled, so that a LionCount path
 * that is built at all is the plan, under EXPLAIN ANALYZE and again for its
 * rows; then as an ordinary plan with the pushdown off, and proves the two
 * answers equal as multisets.  It says whether the node gathered from the
 * store (and which columns), walked without it, or was not used; whether it
 * spilled - more than the one batch, and disk used; and how many rows the
 * query returned.
 */
CREATE FUNCTION lss_check(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	used boolean := false;
	cols text := NULL;
	batches bigint := 0;
	disk bigint := 0;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	/* the row gather would feed an ordinary Agg; store_gather.sql tests it */
	PERFORM set_config('pg_lion.enable_store_scan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(LionCount\)' THEN
			used := true;
		ELSIF ln ~ '^\s*Store: ' THEN
			cols := substring(ln FROM 'Store: (.*)$');
		ELSIF ln ~ '^\s*Store Batches: ' THEN
			batches := batches + substring(ln FROM ': (\d+)$')::bigint;
		ELSIF ln ~ '^\s*Store Disk Usage: ' THEN
			disk := disk + substring(ln FROM ': (\d+) kB$')::bigint;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lss_on AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE format('CREATE TEMP TABLE lss_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	SELECT count(*) INTO nrows FROM lss_on;
	SELECT (SELECT count(*) FROM (TABLE lss_on EXCEPT ALL TABLE lss_off) x) +
		   (SELECT count(*) FROM (TABLE lss_off EXCEPT ALL TABLE lss_on) y)
	  INTO ndiff;
	DROP TABLE lss_on;
	DROP TABLE lss_off;
	RETURN format('%s, %s rows, %s',
				  CASE WHEN cols IS NOT NULL THEN
					   format('store (%s), Store Batches > 1: %s, Store Disk Usage > 0: %s',
							  cols, batches > 1, disk > 0)
					   WHEN used THEN 'walk'
					   ELSE 'no pushdown' END,
				  nrows,
				  CASE WHEN ndiff = 0 THEN 'same as the ordinary plan'
					   ELSE format('DIFFERENT in %s rows', ndiff) END);
END $$;

/*
 * lss_plan() prints the plan of q, with every other scan disabled as
 * lss_check() runs it, and under `actual` what it did, without what differs
 * between majors or with the heap's page layout: the actual row counts (18
 * prints them with decimals) and a subplan's name (19 names it expr_1 where
 * 18 numbered it); the rows gathered as their total, the rows counted, and
 * where they came from as whether each part is empty; the batches and the
 * disk the spill took as whether it spilled; the groups formed, which are
 * the query's; and none of the node's other counters, which the layout
 * decides.
 */
CREATE FUNCTION lss_plan(q text, actual boolean DEFAULT false) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	fromstore bigint := 0;
	fromheap bigint;
	indent text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	/* the row gather would feed an ordinary Agg; store_gather.sql tests it */
	PERFORM set_config('pg_lion.enable_store_scan', 'off', true);
	FOR ln IN EXECUTE CASE WHEN actual THEN
			'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) '
			ELSE 'EXPLAIN (COSTS OFF) ' END || q LOOP
		ln := regexp_replace(ln, '\s*\(actual rows=[^)]*\)', '');
		ln := regexp_replace(ln, 'SubPlan \S+', 'SubPlan N');
		indent := substring(ln FROM '^(\s*)');
		IF ln ~ '^\s*Store Rows: ' THEN
			fromstore := substring(ln FROM ': (\d+)$')::bigint;
		ELSIF ln ~ '^\s*Store Rows From Heap: ' THEN
			fromheap := substring(ln FROM ': (\d+)$')::bigint;
			RETURN NEXT format('%sStore Rows, from the store and the heap: %s',
							   indent, fromstore + fromheap);
			RETURN NEXT format('%sStore Rows > 0: %s, From Heap > 0: %s',
							   indent, fromstore > 0, fromheap > 0);
		ELSIF ln ~ '^\s*Store Batches: ' THEN
			RETURN NEXT format('%sStore Batches > 1: %s', indent,
							   substring(ln FROM ': (\d+)$')::bigint > 1);
		ELSIF ln ~ '^\s*Store Disk Usage: ' THEN
			RETURN NEXT format('%sStore Disk Usage > 0: %s', indent,
							   substring(ln FROM ': (\d+) kB$')::bigint > 0);
		ELSIF actual AND ln !~ '^\s*(Store Groups|Rows Removed by Filter): ' AND
			  (ln ~ '^\s*[A-Z][A-Za-z ]*: \d+$' OR ln ~ '^\s*Range Evaluation: ') THEN
			NULL;				-- a counter of the node's, which the layout decides
		ELSE
			RETURN NEXT ln;
		END IF;
	END LOOP;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
END $$;

-- ---- the tables -----------------------------------------------------------
/*
 * 60000 rows; k of 10 values, the WHERE's column; g of 20000 groups, each
 * group's rows spread over the whole table, so that most groups have not
 * come yet when the table is full; c of 7 groups; u of 30000 values and t of
 * 20000 strings for count(DISTINCT), a value's rows spread out the same way;
 * v an int8 whose sums pass int8's range.  Every one but k has NULLs.
 */
CREATE TABLE lss (id int, k int4, g int4, c int4, u int4, t text, v int8)
	WITH (autovacuum_enabled = off);
INSERT INTO lss
SELECT i, i % 10,
	   CASE WHEN i % 101 = 0 THEN NULL ELSE (i * 7919) % 20000 END,
	   CASE WHEN i % 103 = 0 THEN NULL ELSE i % 7 END,
	   CASE WHEN i % 41 = 0 THEN NULL ELSE (i * 31) % 30000 END,
	   CASE WHEN i % 43 = 0 THEN NULL ELSE 'value ' || ((i * 13) % 20000) END,
	   CASE WHEN i % 37 = 0 THEN NULL ELSE i::int8 * 100000000000003 END
  FROM generate_series(1, 60000) i;
CREATE INDEX lss_k ON lss USING lion (k) INCLUDE (g, c, u, t, v);
VACUUM (FREEZE, ANALYZE) lss;

/*
 * The same rows over three partitions by a range of id: the rows i, i + 20000
 * and i + 40000 have the same g, so every group has rows in two partitions
 * or all three, and the groups the node forms span them.
 */
CREATE TABLE lssp (id int NOT NULL, k int4, g int4, c int4, u int4, t text,
				   v int8)
	PARTITION BY RANGE (id);
CREATE TABLE lssp1 PARTITION OF lssp FOR VALUES FROM (MINVALUE) TO (20000)
	WITH (autovacuum_enabled = off);
CREATE TABLE lssp2 PARTITION OF lssp FOR VALUES FROM (20000) TO (40000)
	WITH (autovacuum_enabled = off);
CREATE TABLE lssp3 PARTITION OF lssp FOR VALUES FROM (40000) TO (MAXVALUE)
	WITH (autovacuum_enabled = off);
INSERT INTO lssp SELECT * FROM lss;
CREATE INDEX lssp_k ON lssp USING lion (k) INCLUDE (g, c, u, t, v);
VACUUM (FREEZE, ANALYZE) lssp;
SELECT count(*) AS groups, min(parts) AS min_parts
  FROM (SELECT g, count(DISTINCT tableoid) AS parts FROM lssp GROUP BY g) x;

SET work_mem = 64;
SET hash_mem_multiplier = 1;

-- 1. The plan, and what it did: a GROUP BY of 20000 groups with count(*),
--    sum, min, max and count(DISTINCT), in more batches than one, with disk
--    used, every group formed once.
SELECT * FROM lss_plan('SELECT g, count(*), sum(v), min(t), max(u), count(DISTINCT u) FROM lss GROUP BY g');
SELECT * FROM lss_plan('SELECT g, count(*), sum(v), min(t), max(u), count(DISTINCT u) FROM lss GROUP BY g', true);
SELECT count(DISTINCT g) + 1 AS groups FROM lss;

-- 2. A GROUP BY of many groups, with every aggregate, with no WHERE and
--    under one; with a HAVING, which the node applies to every batch's
--    groups as they go out; two columns.
SELECT lss_check('SELECT g, count(*) AS n, count(u) AS nu, sum(v), avg(c), min(t), max(u), count(DISTINCT u) AS du FROM lss GROUP BY g');
SELECT lss_check('SELECT g, count(*) AS n, sum(v), min(t), max(t), count(DISTINCT t) AS dt FROM lss WHERE k < 6 GROUP BY g');
SELECT lss_check('SELECT g, count(*) AS n, sum(v) FROM lss WHERE k IN (1, 3, 5, 7) GROUP BY g HAVING count(*) > 1');
SELECT lss_check('SELECT g, c, count(*) AS n, max(v), count(DISTINCT u) AS du FROM lss GROUP BY g, c');
SELECT lss_check('SELECT t, count(*) AS n, min(g), max(v) FROM lss GROUP BY t');

-- 3. count(DISTINCT) of tens of thousands of values with no GROUP BY: only
--    pairs spill, into batches counted into the one group; two at once.
SELECT lss_check('SELECT count(DISTINCT u) FROM lss');
SELECT lss_check('SELECT count(DISTINCT u), count(DISTINCT t), count(*), sum(v) FROM lss WHERE k < 8');
SELECT lss_check('SELECT count(DISTINCT g), count(DISTINCT u), min(t), max(t) FROM lss');
SELECT count(DISTINCT u), count(DISTINCT t), count(*), sum(v) FROM lss WHERE k < 8;
SELECT * FROM lss_plan('SELECT count(DISTINCT u), count(DISTINCT t) FROM lss', true);

-- 4. Few groups, many distinct values: the groups all fit, their pairs do not.
SELECT lss_check('SELECT c, count(*) AS n, count(DISTINCT u) AS du, count(DISTINCT t) AS dt FROM lss GROUP BY c');
SELECT lss_check('SELECT c, count(DISTINCT g) AS dg, sum(v) FROM lss WHERE k BETWEEN 2 AND 8 GROUP BY c');
SELECT c, count(*), count(DISTINCT u) AS du, count(DISTINCT t) AS dt
  FROM lss GROUP BY c ORDER BY c NULLS FIRST;

-- 5. A partitioned table, whose groups span the partitions: one hash table
--    for all of them, spilled the same way.
SELECT * FROM lss_plan('SELECT g, count(*), sum(v), count(DISTINCT u) FROM lssp GROUP BY g', true);
SELECT lss_check('SELECT g, count(*) AS n, sum(v), min(t), max(u), count(DISTINCT u) AS du FROM lssp GROUP BY g');
SELECT lss_check('SELECT g, count(*) AS n, max(t) FROM lssp WHERE k < 5 GROUP BY g');
SELECT lss_check('SELECT count(DISTINCT u), count(DISTINCT t) FROM lssp');
SELECT lss_check('SELECT c, count(DISTINCT u) AS du FROM lssp WHERE k > 2 GROUP BY c');

-- 6. Run again: each probe of a correlated subquery counts and spills
--    afresh, and one that stops after a few groups, with batches not yet
--    read, leaves none of their files behind (a file left open would be
--    reported at the end of the transaction).
SELECT * FROM lss_plan($$SELECT x, (SELECT count(DISTINCT u) FROM lss WHERE k = x) AS du,
	   (SELECT sum(n) FROM (SELECT g, count(*) AS n FROM lss WHERE k <> x GROUP BY g) s) AS n,
	   (SELECT count(n) FROM (SELECT g, count(*) AS n FROM lss WHERE k <> x GROUP BY g LIMIT 5) s) AS f
  FROM generate_series(1, 3) x ORDER BY x$$);
SELECT x, (SELECT count(DISTINCT u) FROM lss WHERE k = x) AS du,
	   (SELECT sum(n) FROM (SELECT g, count(*) AS n FROM lss WHERE k <> x GROUP BY g) s) AS n,
	   (SELECT count(n) FROM (SELECT g, count(*) AS n FROM lss WHERE k <> x GROUP BY g LIMIT 5) s) AS f
  FROM generate_series(1, 3) x ORDER BY x;
SET pg_lion.enable_count_pushdown = off;
SELECT x, (SELECT count(DISTINCT u) FROM lss WHERE k = x) AS du,
	   (SELECT sum(n) FROM (SELECT g, count(*) AS n FROM lss WHERE k <> x GROUP BY g) s) AS n,
	   (SELECT count(n) FROM (SELECT g, count(*) AS n FROM lss WHERE k <> x GROUP BY g LIMIT 5) s) AS f
  FROM generate_series(1, 3) x ORDER BY x;
RESET pg_lion.enable_count_pushdown;
SELECT * FROM lss_plan($$SELECT x, (SELECT count(DISTINCT u) FROM lss WHERE k = x) AS du,
	   (SELECT sum(n) FROM (SELECT g, count(*) AS n FROM lss WHERE k <> x GROUP BY g) s) AS n
  FROM generate_series(1, 3) x$$, true);
-- ... and the node's end does the same for a query that stops early.
SELECT lss_check('SELECT count(n) FROM (SELECT g, count(*) AS n FROM lss GROUP BY g LIMIT 3) s');

-- 7. Where everything fits, the same queries do not spill: one batch, no
--    disk.  The work_mem is ./dev.sh's cluster's, set here because
--    PostgreSQL's own default of 4MB would not hold the first query's groups.
SET work_mem = '64MB';
RESET hash_mem_multiplier;
SELECT * FROM lss_plan('SELECT g, count(*), sum(v), min(t), max(u), count(DISTINCT u) FROM lss GROUP BY g', true);
SELECT lss_check('SELECT g, count(*) AS n, count(u) AS nu, sum(v), avg(c), min(t), max(u), count(DISTINCT u) AS du FROM lss GROUP BY g');
SELECT lss_check('SELECT g, count(*) AS n, sum(v), min(t), max(t), count(DISTINCT t) AS dt FROM lss WHERE k < 6 GROUP BY g');
SELECT lss_check('SELECT count(DISTINCT u), count(DISTINCT t), count(*), sum(v) FROM lss WHERE k < 8');
SELECT lss_check('SELECT c, count(*) AS n, count(DISTINCT u) AS du, count(DISTINCT t) AS dt FROM lss GROUP BY c');
SELECT lss_check('SELECT g, count(*) AS n, sum(v), min(t), max(u), count(DISTINCT u) AS du FROM lssp GROUP BY g');

DROP TABLE lss, lssp;
DROP FUNCTION lss_check(text), lss_plan(text, boolean);
