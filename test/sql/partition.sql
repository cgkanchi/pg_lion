-- The count pushdown over partitioned tables (DESIGN.md section 16).
--
-- The node counts one live leaf partition at a time.  Without GROUP BY it
-- adds the results up and emits one row; with GROUP BY it emits one PARTIAL
-- aggregate per group per partition, streamed as each partition is counted,
-- and core's Finalize HashAggregate above it combines them.  So what has to
-- be checked is that it finds the right partitions (the planner's pruned set,
-- recursively through sub-partitioning), the right index in each of them
-- (attnums differ when a partition has a dropped column), that the plan is
-- that two-node shape, and that the rows the whole thing produces are exactly
-- the rows the ordinary plan produces.  The last check is done with EXCEPT
-- ALL in both directions, so a wrong answer fails even if it happens to be
-- stable.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';

/*
 * The visibility map is what makes this feature worth having, and a heap page
 * only becomes all-visible once the inserting transaction's commit record has
 * reached disk.  The dev cluster runs with synchronous_commit = off, so ask
 * for flushed commits here (see pushdown.sql).
 */
SET synchronous_commit = on;
SET default_statistics_target = 1000;

CREATE EXTENSION IF NOT EXISTS pg_lion;

/*
 * lion_pp() runs one query twice, once with the pushdown enabled and once
 * without, and reports whether the custom node was used and how many rows
 * came out - after proving the two result sets are equal as multisets.
 */
CREATE FUNCTION lion_pp(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	pushed boolean := false;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			pushed := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lion_pp_on AS %s', q);

	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE format('CREATE TEMP TABLE lion_pp_off AS %s', q);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM lion_pp_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_pp_on EXCEPT ALL SELECT * FROM lion_pp_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_pp_off EXCEPT ALL SELECT * FROM lion_pp_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lion_pp_on, lion_pp_off';

	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN pushed THEN 'pushed down' ELSE 'not pushed down' END,
				  nrows);
END $$;

/* The plan of one query with the pushdown on. */
CREATE FUNCTION lion_pplan(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		RETURN NEXT ln;
	END LOOP;
END $$;

/*
 * What the top node of one executed plan is, and whether its hash table had
 * to spill.  A partitioned GROUP BY is pushed down as PARTIAL aggregates with
 * core's Finalize HashAggregate on top (DESIGN.md section 16), and the point
 * of that shape is that the finalize step spills to disk when the groups do
 * not fit in hash_mem.  The batch and byte counts themselves depend on the
 * machine, so this reads them out of EXPLAIN (FORMAT JSON) and prints only
 * the predicates.
 */
CREATE FUNCTION lion_pspill(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	pl json;
	top json;
	child json;
	batches bigint;
	disk bigint;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF,'
			' BUFFERS OFF, FORMAT JSON) ' || q INTO pl;
	top := pl -> 0 -> 'Plan';
	child := top -> 'Plans' -> 0;
	batches := coalesce((top ->> 'HashAgg Batches')::bigint, 0);
	disk := coalesce((top ->> 'Disk Usage')::bigint, 0);

	RETURN NEXT format('top: %s %s %s',
					   coalesce(top ->> 'Partial Mode', '?'),
					   coalesce(top ->> 'Strategy', '?'),
					   top ->> 'Node Type');
	RETURN NEXT format('child: %s',
					   coalesce(child ->> 'Custom Plan Provider',
								child ->> 'Node Type', 'none'));
	-- 18 prints actual rows with two decimals, earlier releases as integers
	RETURN NEXT format('rows: %s', round((top ->> 'Actual Rows')::numeric));
	RETURN NEXT format('batches > 1: %s', batches > 1);
	RETURN NEXT format('disk usage > 0: %s', disk > 0);
END $$;

-- ---- the table ----------------------------------------------------------
/*
 * Three range partitions, one of them sub-partitioned by list into two (one
 * of which is that level's default), and a default partition at the top.
 * lion_part_p1 is created separately and attached so that it can carry two
 * dropped columns: its attnums are the parent's plus two, which is exactly
 * what the AppendRelInfo translation of DESIGN.md section 16 is for.
 */
CREATE TABLE lion_part (
	id	int		NOT NULL,
	a	int		NOT NULL,		-- 10 values
	b	int		NOT NULL,		-- 7 values
	n	int,					-- 5 values and NULL
	s	text	NOT NULL,		-- the sub-partitioning key
	k	int		NOT NULL		-- the partitioning key, 0 .. 29
) PARTITION BY RANGE (k);

CREATE TABLE lion_part_p1 (
	junk1	text,
	id		int		NOT NULL,
	junk2	bigint,
	a		int		NOT NULL,
	b		int		NOT NULL,
	n		int,
	s		text	NOT NULL,
	k		int		NOT NULL
);
ALTER TABLE lion_part_p1 DROP COLUMN junk1;
ALTER TABLE lion_part_p1 DROP COLUMN junk2;
ALTER TABLE lion_part ATTACH PARTITION lion_part_p1 FOR VALUES FROM (0) TO (10);

CREATE TABLE lion_part_p2 PARTITION OF lion_part
	FOR VALUES FROM (10) TO (20) PARTITION BY LIST (s);
CREATE TABLE lion_part_p2x PARTITION OF lion_part_p2 FOR VALUES IN ('x');
CREATE TABLE lion_part_p2d PARTITION OF lion_part_p2 DEFAULT;

CREATE TABLE lion_part_def PARTITION OF lion_part DEFAULT;

/*
 * The columns are deliberately not functions of the partitioning key: k is
 * i % 30, so anything else taken modulo a divisor of 30 would be decided by
 * the partition a row lands in and the queries below would degenerate.
 */
INSERT INTO lion_part
SELECT i,
	   (i / 11) % 10,
	   (i / 3) % 7,
	   CASE WHEN i % 101 = 0 THEN NULL ELSE (i / 5) % 5 END,
	   CASE WHEN (i / 7) % 3 = 0 THEN 'x' ELSE 'y' END,
	   i % 30
  FROM generate_series(1, 120000) i;

/*
 * One partitioned index per column: PostgreSQL creates a child index on every
 * leaf, which is what the pushdown requires.  The children of lion_part_p1 are
 * on its own attnums.
 */
CREATE INDEX lion_part_a ON lion_part USING lion (a);
CREATE INDEX lion_part_b ON lion_part USING lion (b);
CREATE INDEX lion_part_n ON lion_part USING lion (n);
CREATE INDEX lion_part_s ON lion_part USING lion (s);
CREATE INDEX lion_part_k ON lion_part USING lion (k);

VACUUM ANALYZE lion_part;

-- the attnums really do differ between the partitions
SELECT c.relname, a.attname, a.attnum
  FROM pg_class c JOIN pg_attribute a ON a.attrelid = c.oid
 WHERE c.relname IN ('lion_part', 'lion_part_p1', 'lion_part_p2x')
   AND a.attnum > 0 AND NOT a.attisdropped
 ORDER BY c.relname, a.attnum;

-- every leaf really does have its five lion indexes
SELECT c.relname AS partition, count(*) AS roaring_indexes
  FROM pg_class c
  JOIN pg_index i ON i.indrelid = c.oid
  JOIN pg_class ic ON ic.oid = i.indexrelid
  JOIN pg_am am ON am.oid = ic.relam
 WHERE c.relname LIKE 'lion_part%' AND am.amname = 'lion'
   AND c.relkind = 'r'
 GROUP BY c.relname ORDER BY 1;

SELECT lion_index_verify('lion_part_p1_a_idx', true);
SELECT lion_index_verify('lion_part_p2d_n_idx', true);

-- ---- plans --------------------------------------------------------------
SELECT lion_pplan('SELECT count(*) FROM lion_part WHERE a = 3');
SELECT lion_pplan('SELECT count(*) FROM lion_part WHERE a = 3 AND b = 2');
SELECT lion_pplan('SELECT a, count(*) FROM lion_part GROUP BY a');
SELECT lion_pplan('SELECT a, count(*) FROM lion_part WHERE b = 2 GROUP BY a');
SELECT lion_pplan('SELECT count(*) FROM lion_part WHERE n IS NULL');
SELECT lion_pplan('SELECT count(*) FROM lion_part WHERE a IN (1, 4, 7)');
SELECT lion_pplan('SELECT n, count(*) AS c1, count(n) AS c2 FROM lion_part GROUP BY n');
SELECT lion_pplan('SELECT a, b, count(*) FROM lion_part WHERE b = 2 GROUP BY a, b');

/*
 * What the node itself produces for a partitioned GROUP BY: the group key and
 * a PARTIAL count per aggregate (DESIGN.md section 16).  This is the shape
 * setrefs.c matched the Finalize Agg's own references against, so EXPLAIN
 * VERBOSE printing it is the check that the two halves agree.  The third plan
 * also carries a column the WHERE clause pins (b), which the partial target
 * has to pass through untouched.
 */
EXPLAIN (VERBOSE, COSTS OFF) SELECT a, count(*) FROM lion_part GROUP BY a;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT n, count(*) AS c1, count(n) AS c2 FROM lion_part GROUP BY n;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT a, b, count(*) FROM lion_part WHERE b = 2 GROUP BY a, b;

-- ---- the shapes that must push down --------------------------------------
SELECT lion_pp('SELECT count(*) FROM lion_part WHERE a = 3');
SELECT lion_pp('SELECT count(*) FROM lion_part WHERE a = 3 AND b = 2');
SELECT lion_pp('SELECT count(*) FROM lion_part WHERE 3 = a');
SELECT lion_pp('SELECT count(*) FROM lion_part WHERE a = 3::int8');
SELECT lion_pp('SELECT count(*) FROM lion_part WHERE a = 999');
SELECT lion_pp('SELECT count(*) FROM lion_part WHERE a IN (1, 4, 7)');
SELECT lion_pp('SELECT count(*) FROM lion_part WHERE a IN (1, 4, 7) AND b = 2');
SELECT lion_pp('SELECT count(*) FROM lion_part WHERE a IN (999, 1000)');
SELECT lion_pp('SELECT count(*) FROM lion_part WHERE n IS NULL');
SELECT lion_pp('SELECT count(*) FROM lion_part WHERE n IS NULL AND a = 3');
SELECT lion_pp('SELECT count(*) FROM lion_part WHERE n IS NOT NULL AND a = 3');
SELECT lion_pp('SELECT a, count(*) FROM lion_part GROUP BY a');
SELECT lion_pp('SELECT a, count(*) FROM lion_part WHERE b = 2 GROUP BY a');
SELECT lion_pp('SELECT a, count(*) FROM lion_part WHERE b = 2 AND n = 1 GROUP BY a');
SELECT lion_pp('SELECT count(*) FROM lion_part GROUP BY a');
SELECT lion_pp('SELECT n, count(*) FROM lion_part GROUP BY n');
SELECT lion_pp('SELECT n, count(*) AS c1, count(n) AS c2 FROM lion_part GROUP BY n');
SELECT lion_pp('SELECT b, count(*) FROM lion_part WHERE a IN (1, 4, 7) GROUP BY b');
SELECT lion_pp('SELECT count(a) FROM lion_part WHERE a = 3');
SELECT lion_pp('SELECT a, count(*) FROM lion_part WHERE a = 3 GROUP BY a');
SELECT lion_pp('SELECT a, count(*) FROM lion_part WHERE a = 999 GROUP BY a');
/*
 * A column the WHERE clause pins to one value prints the key the index
 * stored, and with partitions that key comes from the first partition that
 * has it - which is why the node remembers it instead of reading it out of a
 * posting set that is long gone by the time the row is emitted.
 */
SELECT lion_pp('SELECT a, count(*) FROM lion_part WHERE a = 3::int8 GROUP BY a');
SELECT lion_pp('SELECT a, b, count(*) FROM lion_part WHERE b = 2 GROUP BY a, b');
SELECT lion_pp('SELECT n, count(*) FROM lion_part WHERE n IS NULL GROUP BY n');
/*
 * `IS NOT NULL` with nothing else to drive the merge: every entry of that
 * column's index is summed, in every partition (DESIGN.md section 14).
 */
SELECT lion_pp('SELECT count(*) FROM lion_part WHERE n IS NOT NULL');
SELECT lion_pplan('SELECT count(*) FROM lion_part WHERE n IS NOT NULL');
/* A by-reference group key: the merged table keeps a copy of every key. */
SELECT lion_pp('SELECT s, count(*) FROM lion_part GROUP BY s');
SELECT lion_pp($q$SELECT s, count(*) FROM lion_part WHERE a = 3 GROUP BY s$q$);

/*
 * Partitionwise aggregation builds the grouping out of per-partition Aggs,
 * which is a different plan shape from this node; the pushdown steps aside
 * and lets the planner have it (DESIGN.md section 16).
 */
SET enable_partitionwise_aggregate = on;
SELECT lion_pp('SELECT a, count(*) FROM lion_part GROUP BY a');
SELECT lion_pp('SELECT count(*) FROM lion_part WHERE a = 3');
RESET enable_partitionwise_aggregate;

-- ---- HAVING on the count --------------------------------------------------
-- The node emits partial counts, so the HAVING belongs to the Finalize Agg
-- above it - which also needs the count a HAVING alone mentions, produced as
-- a partial column the query never prints (DESIGN.md section 10).
EXPLAIN (VERBOSE, COSTS OFF) SELECT a, count(*) FROM lion_part GROUP BY a HAVING count(*) > 10000;
EXPLAIN (VERBOSE, COSTS OFF) SELECT a FROM lion_part GROUP BY a HAVING count(*) > 10000;
SELECT lion_pp('SELECT a, count(*) FROM lion_part GROUP BY a HAVING count(*) > 10000');
SELECT lion_pp('SELECT a FROM lion_part GROUP BY a HAVING count(*) > 10000');
SELECT lion_pp('SELECT a FROM lion_part GROUP BY a HAVING count(*) > 10000000');
SELECT lion_pp('SELECT b, count(*) FROM lion_part WHERE s = ''x'' GROUP BY b HAVING count(*) > 10000');
SELECT lion_pp('SELECT n FROM lion_part GROUP BY n HAVING count(n) < count(*)');
SELECT lion_pp('SELECT count(*) FROM lion_part WHERE a = 3 HAVING count(*) > 0');

-- ---- the answers themselves ----------------------------------------------
SELECT count(*) FROM lion_part;
SELECT a, count(*) FROM lion_part GROUP BY a ORDER BY a;
SELECT n, count(*) FROM lion_part GROUP BY n ORDER BY n;
SELECT count(*) FROM lion_part WHERE a = 3 AND b = 2;
SELECT b, count(*) FROM lion_part WHERE a IN (1, 4, 7) GROUP BY b ORDER BY b;
SELECT s, count(*) FROM lion_part GROUP BY s ORDER BY s;
SELECT count(*) FROM lion_part WHERE n IS NOT NULL;
SELECT a, b, count(*) FROM lion_part WHERE b = 2 GROUP BY a, b ORDER BY a;

-- ---- plan-time pruning ---------------------------------------------------
/*
 * A clause on the partitioning key leaves the planner with fewer partitions,
 * and the node must count exactly those - EXPLAIN says which.  The clause is
 * still a clause: the surviving partitions need a lion index on k too.
 */
SELECT lion_pplan('SELECT count(*) FROM lion_part WHERE k = 5');
SELECT lion_pplan('SELECT count(*) FROM lion_part WHERE k = 15 AND a = 3');
SELECT lion_pplan('SELECT a, count(*) FROM lion_part WHERE k IN (2, 25) GROUP BY a');
SELECT lion_pp('SELECT count(*) FROM lion_part WHERE k = 5');
SELECT lion_pp('SELECT count(*) FROM lion_part WHERE k = 15 AND a = 3');
SELECT lion_pp('SELECT a, count(*) FROM lion_part WHERE k IN (2, 25) GROUP BY a');
SELECT count(*) FROM lion_part WHERE k = 5;
SELECT count(*) FROM lion_part WHERE k = 15 AND a = 3;
SELECT count(*) FROM lion_part WHERE k IN (2, 25);

/* Grouping by the partitioning key: every partition's groups are its own. */
SELECT lion_pp('SELECT k, count(*) FROM lion_part GROUP BY k');
SELECT lion_pp('SELECT k, count(*) FROM lion_part WHERE k = 5 GROUP BY k');
SELECT lion_pplan('SELECT k, count(*) FROM lion_part WHERE k = 5 GROUP BY k');
SELECT k, count(*) FROM lion_part WHERE k IN (5, 15, 25) GROUP BY k ORDER BY k;

-- ---- a dirty heap, and then an all-visible one ---------------------------
/*
 * Deleting from one partition leaves the others all-visible: the node then
 * takes the visibility-map path for some partitions and the heap-recheck path
 * for the one that was touched, in the same execution.
 */
DELETE FROM lion_part WHERE k BETWEEN 10 AND 19 AND id % 3 = 0;

SELECT lion_pp('SELECT count(*) FROM lion_part WHERE a = 3');
SELECT lion_pp('SELECT a, count(*) FROM lion_part GROUP BY a');
SELECT lion_pp('SELECT a, count(*) FROM lion_part WHERE b = 2 GROUP BY a');
SELECT a, count(*) FROM lion_part GROUP BY a ORDER BY a;

VACUUM lion_part;

SELECT lion_pp('SELECT count(*) FROM lion_part WHERE a = 3');
SELECT lion_pp('SELECT a, count(*) FROM lion_part GROUP BY a');
SELECT lion_pp('SELECT a, count(*) FROM lion_part WHERE b = 2 GROUP BY a');
SELECT a, count(*) FROM lion_part GROUP BY a ORDER BY a;

-- ---- DETACH and ATTACH between queries -----------------------------------
ALTER TABLE lion_part DETACH PARTITION lion_part_p1;
SELECT lion_pplan('SELECT count(*) FROM lion_part WHERE a = 3');
SELECT lion_pp('SELECT count(*) FROM lion_part WHERE a = 3');
SELECT lion_pp('SELECT a, count(*) FROM lion_part GROUP BY a');
SELECT count(*) FROM lion_part;

ALTER TABLE lion_part ATTACH PARTITION lion_part_p1 FOR VALUES FROM (0) TO (10);
VACUUM ANALYZE lion_part;
SELECT lion_pplan('SELECT count(*) FROM lion_part WHERE a = 3');
SELECT lion_pp('SELECT count(*) FROM lion_part WHERE a = 3');
SELECT lion_pp('SELECT a, count(*) FROM lion_part GROUP BY a');
SELECT count(*) FROM lion_part;

/* A brand-new partition, whose index the ATTACH builds for us. */
CREATE TABLE lion_part_p3 (LIKE lion_part INCLUDING DEFAULTS);
INSERT INTO lion_part_p3
SELECT 200000 + i, i % 10, i % 7, i % 5, 'y', 30 + (i % 5)
  FROM generate_series(1, 5000) i;
ALTER TABLE lion_part ATTACH PARTITION lion_part_p3
	FOR VALUES FROM (30) TO (40);
VACUUM ANALYZE lion_part;
SELECT lion_pplan('SELECT count(*) FROM lion_part WHERE a = 3');
SELECT lion_pp('SELECT count(*) FROM lion_part WHERE a = 3');
SELECT lion_pp('SELECT a, count(*) FROM lion_part GROUP BY a');
SELECT a, count(*) FROM lion_part GROUP BY a ORDER BY a;

-- ---- an empty partitioned table -------------------------------------------
CREATE TABLE lion_parte (a int NOT NULL, k int NOT NULL) PARTITION BY RANGE (k);
CREATE TABLE lion_parte1 PARTITION OF lion_parte FOR VALUES FROM (0) TO (10);
CREATE TABLE lion_parte2 PARTITION OF lion_parte FOR VALUES FROM (10) TO (20);
CREATE INDEX ON lion_parte USING lion (a);
VACUUM ANALYZE lion_parte;
SELECT lion_pp('SELECT count(*) FROM lion_parte WHERE a = 1');
SELECT lion_pp('SELECT a, count(*) FROM lion_parte GROUP BY a');
/*
 * Scanning an empty table costs nothing, so the cost model will never pick
 * the node here.  Disable the alternatives to make it run anyway: counting
 * nothing, in every partition, must produce the same answers.
 */
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SELECT lion_pplan('SELECT count(*) FROM lion_parte WHERE a = 1');
SELECT lion_pplan('SELECT a, count(*) FROM lion_parte GROUP BY a');
SELECT count(*) FROM lion_parte WHERE a = 1;
SELECT a, count(*) FROM lion_parte GROUP BY a;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
DROP TABLE lion_parte;

-- ---- partial aggregates, finalized (and spilled) by core -------------------
/*
 * A partitioned GROUP BY does not merge the partitions' groups in the node
 * any more: it emits one PARTIAL aggregate per group per partition and core's
 * Finalize HashAggregate combines them (DESIGN.md section 16).  That is what
 * bounds the memory - the node keeps nothing between rows, and the finalize
 * step batches to disk under hash_mem exactly like any other HashAggregate -
 * so a grouping far too large for work_mem is no longer refused.
 *
 * Every other plan is disabled around the plans below, so that the pushdown
 * is what runs; the numbers themselves are reduced to predicates, because
 * how many batches a spill needs is not a property of this extension.
 */
CREATE TABLE lion_pbig (g int NOT NULL, k int NOT NULL) PARTITION BY RANGE (k);
CREATE TABLE lion_pbig1 PARTITION OF lion_pbig FOR VALUES FROM (0) TO (5);
CREATE TABLE lion_pbig2 PARTITION OF lion_pbig FOR VALUES FROM (5) TO (10);
INSERT INTO lion_pbig SELECT i % 5000, i % 10 FROM generate_series(1, 50000) i;
CREATE INDEX lion_pbig_g ON lion_pbig USING lion (g);
CREATE INDEX lion_pbig_k ON lion_pbig USING lion (k);
VACUUM ANALYZE lion_pbig;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SET hash_mem_multiplier = 1.0;
SET work_mem = '64kB';			-- 5000 groups do not fit: the finalize spills
SELECT lion_pplan('SELECT g, count(*) FROM lion_pbig GROUP BY g');
SELECT lion_pspill('SELECT g, count(*) FROM lion_pbig GROUP BY g');
SELECT lion_pp('SELECT g, count(*) FROM lion_pbig GROUP BY g');
SET work_mem = '4MB';			-- and now they fit, so it does not
SELECT lion_pplan('SELECT g, count(*) FROM lion_pbig GROUP BY g');
SELECT lion_pspill('SELECT g, count(*) FROM lion_pbig GROUP BY g');
SELECT lion_pp('SELECT g, count(*) FROM lion_pbig GROUP BY g');
/*
 * A count has no groups at all, and a single table streams its own groups -
 * neither has ever had anything above it.
 */
SET work_mem = '64kB';
SELECT lion_pplan('SELECT count(*) FROM lion_pbig WHERE g = 7');
SELECT lion_pplan('SELECT g, count(*) FROM lion_pbig1 GROUP BY g');
/*
 * A Finalize HashAggregate that has spilled rescans its input instead of
 * re-reading its own table (ExecReScanAgg), so a nested loop over a spilling
 * one is what makes the node abandon a half-scanned partition and start the
 * whole partition list again (ReScanCustomScan).
 */
SET enable_material = off;
SELECT lion_pplan('SELECT v.x, count(*) AS groups, sum(s.c) AS rows'
				 ' FROM (VALUES (1), (2)) v(x)'
				 ' LEFT JOIN LATERAL (SELECT g, count(*) c FROM lion_pbig'
				 ' GROUP BY g) s ON s.g >= v.x GROUP BY v.x');
SELECT v.x, count(*) AS groups, sum(s.c) AS rows
  FROM (VALUES (1), (2)) v(x)
  LEFT JOIN LATERAL (SELECT g, count(*) c FROM lion_pbig GROUP BY g) s
	ON s.g >= v.x
 GROUP BY v.x ORDER BY 1;
RESET enable_material;
RESET work_mem;
RESET hash_mem_multiplier;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
SELECT count(*) FROM (SELECT g, count(*) FROM lion_pbig GROUP BY g) s;
DROP TABLE lion_pbig;

-- ---- stale statistics bound nothing any more -------------------------------
/*
 * The budget this section used to enforce was a PLAN-time one, and therefore
 * only as good as estimate_num_groups(): a partitioned parent is never
 * auto-analyzed, so ten estimated groups against twenty thousand actual ones
 * let the executor build a hash table with no bound at all (the 2026-09-21
 * follow-up review).  With the merge gone the estimate bounds nothing that
 * matters: the query is pushed down, every group is counted exactly, and the
 * Finalize HashAggregate spills.
 */
CREATE TABLE lion_pstale (g int NOT NULL, k int NOT NULL) PARTITION BY RANGE (k);
CREATE TABLE lion_pstale1 PARTITION OF lion_pstale FOR VALUES FROM (0) TO (5);
CREATE TABLE lion_pstale2 PARTITION OF lion_pstale FOR VALUES FROM (5) TO (10);
INSERT INTO lion_pstale SELECT i % 10, i % 10 FROM generate_series(1, 10000) i;
CREATE INDEX lion_pstale_g ON lion_pstale USING lion (g);
VACUUM ANALYZE lion_pstale;
-- what the planner knows about the grouping column: ten groups
SELECT n_distinct FROM pg_stats
 WHERE tablename = 'lion_pstale' AND attname = 'g' AND inherited;
-- twenty thousand brand-new keys, vacuumed (all-visible) but NOT analyzed
INSERT INTO lion_pstale SELECT 1000 + i, i % 10 FROM generate_series(1, 20000) i;
VACUUM lion_pstale;
SELECT n_distinct FROM pg_stats
 WHERE tablename = 'lion_pstale' AND attname = 'g' AND inherited;
SET work_mem = '64kB';
SET hash_mem_multiplier = 1.0;
SELECT lion_pplan('SELECT g, count(*) FROM lion_pstale GROUP BY g');
SELECT lion_pspill('SELECT g, count(*) FROM lion_pstale GROUP BY g');
SELECT lion_pp('SELECT g, count(*) FROM lion_pstale GROUP BY g');
SELECT count(*) AS groups, sum(c) AS rows
  FROM (SELECT g, count(*) c FROM lion_pstale GROUP BY g) s;
RESET work_mem;
RESET hash_mem_multiplier;
DROP TABLE lion_pstale;

-- ---- a partition without a usable lion index ---------------------------
CREATE TABLE lion_pnx (id int NOT NULL, a int NOT NULL, k int NOT NULL)
	PARTITION BY RANGE (k);
CREATE TABLE lion_pnx1 PARTITION OF lion_pnx FOR VALUES FROM (0) TO (10);
CREATE TABLE lion_pnx2 PARTITION OF lion_pnx FOR VALUES FROM (10) TO (20);
INSERT INTO lion_pnx SELECT i, (i / 7) % 10, i % 20 FROM generate_series(1, 60000) i;
CREATE INDEX lion_pnx1_a ON lion_pnx1 USING lion (a);
CREATE INDEX lion_pnx2_a ON lion_pnx2 USING lion (a);
CREATE INDEX lion_pnx_k ON lion_pnx USING lion (k);
VACUUM ANALYZE lion_pnx;

-- both partitions have one
SELECT lion_pp('SELECT count(*) FROM lion_pnx WHERE a = 3');
SELECT lion_pp('SELECT a, count(*) FROM lion_pnx GROUP BY a');

-- one of them loses it: the whole query falls back to the ordinary plan
DROP INDEX lion_pnx2_a;
SELECT lion_pplan('SELECT count(*) FROM lion_pnx WHERE a = 3');
SELECT lion_pp('SELECT count(*) FROM lion_pnx WHERE a = 3');
SELECT lion_pp('SELECT a, count(*) FROM lion_pnx GROUP BY a');

-- ... unless pruning removes the partition that lacks it
SELECT lion_pplan('SELECT count(*) FROM lion_pnx WHERE k = 5 AND a = 3');
SELECT lion_pp('SELECT count(*) FROM lion_pnx WHERE k = 5 AND a = 3');
SELECT count(*) FROM lion_pnx WHERE k = 5 AND a = 3;

-- an index of the wrong access method is not one either
CREATE INDEX lion_pnx2_a_bt ON lion_pnx2 USING btree (a);
SELECT lion_pp('SELECT count(*) FROM lion_pnx WHERE a = 3');
DROP TABLE lion_pnx;

-- ---- the node inside a larger plan ----------------------------------------
/*
 * A nested loop rescans its inner side once per outer row.  With a group key
 * the node now sits under a Finalize HashAggregate, which reuses its own
 * table rather than re-reading the node unless it spilled (that case is in
 * the spill section above); a plain count is rescanned directly.
 */
SET enable_material = off;
SET enable_seqscan = off;
SELECT lion_pplan('SELECT v.x, s.g, s.c FROM (VALUES (1), (2), (3)) v(x)'
				 ' LEFT JOIN LATERAL (SELECT a g, count(*) c FROM lion_part GROUP BY a) s'
				 ' ON s.g >= v.x');
SELECT v.x, s.g, s.c
  FROM (VALUES (1), (2), (3)) v(x)
  LEFT JOIN LATERAL (SELECT a g, count(*) c FROM lion_part GROUP BY a) s
	ON s.g >= v.x
 WHERE s.g < 3
 ORDER BY 1, 2;
SELECT v.x, s.c
  FROM (VALUES (1), (2), (3)) v(x)
  LEFT JOIN LATERAL (SELECT count(*) c FROM lion_part WHERE a = 3) s
	ON s.c >= v.x
 ORDER BY 1;
RESET enable_material;
RESET enable_seqscan;

-- ---- what EXPLAIN ANALYZE reports ------------------------------------------
/*
 * Reduced to zero / non-zero: the exact counts depend on how the rows fall on
 * heap pages, but whether the heap had to be visited at all does not.
 */
CREATE FUNCTION lion_pp_counters(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	nm text;
	val bigint;
	pushed boolean := false;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			pushed := true;
		END IF;
		/*
		 * A partitioned GROUP BY has a Finalize HashAggregate above the node
		 * (DESIGN.md section 16), whose own Group Key line is not what this
		 * function is about: start at the node itself.
		 */
		CONTINUE WHEN NOT pushed;
		nm := btrim(split_part(ln, ':', 1));
		IF nm IN ('Partitions', 'Lion Indexes', 'Group Key') THEN
			RETURN NEXT btrim(ln);
		ELSIF nm IN ('Heap Blocks Skipped via VM', 'Heap TIDs Rechecked',
					 'Heap Blocks Rechecked', 'Containers Visited',
					 'Heap Blocks From Cache',
					 'Heap Blocks Past Cache Budget') THEN
			val := btrim(split_part(ln, ':', 2))::bigint;
			RETURN NEXT format('%s %s', nm,
							   CASE WHEN val = 0 THEN '= 0' ELSE '> 0' END);
		END IF;
	END LOOP;
	IF NOT pushed THEN
		RETURN NEXT 'NOT PUSHED DOWN';
	END IF;
END $$;

VACUUM lion_part;
SELECT lion_pp_counters('SELECT count(*) FROM lion_part WHERE a = 3');
SELECT lion_pp_counters('SELECT a, count(*) FROM lion_part GROUP BY a');
-- the pages a DELETE dirties are no longer all-visible
DELETE FROM lion_part WHERE id % 700 = 0;
SELECT lion_pp_counters('SELECT count(*) FROM lion_part WHERE a = 3');
/*
 * One visibility cache serves every group of every partition (DESIGN.md §9),
 * so a grouping over a heap the visibility map cannot vouch for comes back to
 * its dirty pages without pinning them again; a single count never returns to
 * a block at all and has no cache hits.
 */
SELECT lion_pp_counters('SELECT a, count(*) FROM lion_part GROUP BY a');

-- ---- the indexes are still sound -------------------------------------------
VACUUM lion_part;
SELECT lion_index_verify('lion_part_p1_a_idx', true);
SELECT lion_index_verify('lion_part_p2x_a_idx', true);
SELECT lion_index_verify('lion_part_def_n_idx', true);

DROP TABLE lion_part;
DROP FUNCTION lion_pp(text);
DROP FUNCTION lion_pp_counters(text);
DROP FUNCTION lion_pplan(text);
DROP FUNCTION lion_pspill(text);
