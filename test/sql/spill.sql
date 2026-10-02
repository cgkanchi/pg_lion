-- Collected sets past their memory spill to a temporary file (2026-09-28
-- review; DESIGN.md §27, "The fact filters, collected once", and §32, "A
-- range as a source").
--
-- A range that is an OR's leaf cannot be walked, and used to be collected in
-- memory whatever it took; the FK-side join's copy of its fact filters used
-- to be abandoned past a hash join's memory, after which every dimension
-- row's count read the filters - a union of every set of an IN list, built
-- again for each row.  Both now go on in a temporary file.  Each plan below
-- is made where the set fits and run where it does not, and every answer is
-- checked against the ordinary plan's.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
/* VACUUM can only set all-visible for commits that reached disk (citext.sql). */
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;
/*
 * The session's first temporary table changes its search_path, which makes
 * every cached plan be made again; the plans below have to stay the ones
 * made with the default work_mem.
 */
CREATE TEMP TABLE lion_sp_first (x int);
DROP TABLE lion_sp_first;

CREATE FUNCTION lion_sp_analyze(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		RETURN NEXT ln;
	END LOOP;
END $$;

-- ---------- a range in an OR, larger than the memory it gets ----------
/*
 * Half of 360000 rows of a column in no order is a NARROW of width 2
 * (DESIGN.md §38), 1 kB, at each of some ninety container keys, some 90 kB:
 * more than a hash table's memory of 64 kB.  The pad makes the rows 65 to a
 * page, just past the 63 offsets a NARROW of width 1 holds, which makes a
 * NARROW the largest for the rows it holds: narrow rows, 185 to a page,
 * would need nearly twice as many for the same set.  The plan is made with the default
 * work_mem, where the range is collected into memory, and run again with
 * 64 kB, where it is collected a window of container keys at a time into a
 * temporary file.
 */
CREATE TABLE lion_sp_r (r int NOT NULL, g int NOT NULL, a int NOT NULL,
						pad text) WITH (autovacuum_enabled = off);
INSERT INTO lion_sp_r
SELECT hashint4(i) & 1048575, i % 7, i % 30, repeat('x', 80)
  FROM generate_series(1, 360000) i;
CREATE INDEX lion_sp_r_r ON lion_sp_r USING lion (r)
	WITH (summaries = on, summary_tids = 1024);
CREATE INDEX lion_sp_r_g ON lion_sp_r USING lion (g);
CREATE INDEX lion_sp_r_a ON lion_sp_r USING lion (a);
VACUUM (FREEZE, ANALYZE) lion_sp_r;
SET plan_cache_mode = force_generic_plan;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
PREPARE lion_sp_or AS
	SELECT g, count(*) AS n FROM lion_sp_r WHERE (r < 524288 OR a = 3) GROUP BY g;
SELECT count(*) FILTER (WHERE p ~ 'Custom Scan \(LionCount\)') AS pushed,
	   count(*) FILTER (WHERE p ~ 'Range Sources Collected: 1$') AS collected,
	   count(*) FILTER (WHERE p ~ 'Range Sources Spilled') AS spilled
FROM lion_sp_analyze('EXECUTE lion_sp_or') AS e(p);
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SELECT count(*) FILTER (WHERE p ~ 'Custom Scan \(LionCount\)') AS pushed,
	   count(*) FILTER (WHERE p ~ 'Range Sources Collected: 1$') AS collected,
	   count(*) FILTER (WHERE p ~ 'Range Sources Spilled: 1$') AS spilled
FROM lion_sp_analyze('EXECUTE lion_sp_or') AS e(p);
CREATE TEMP TABLE lion_sp_got AS EXECUTE lion_sp_or;
RESET work_mem;
RESET hash_mem_multiplier;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
SET pg_lion.enable_count_pushdown = off;
CREATE TEMP TABLE lion_sp_want AS
	SELECT g, count(*) AS n FROM lion_sp_r WHERE (r < 524288 OR a = 3) GROUP BY g;
SET pg_lion.enable_count_pushdown = on;
SELECT (SELECT count(*) FROM lion_sp_want) AS groups,
	   (SELECT count(*) FROM (TABLE lion_sp_got EXCEPT ALL TABLE lion_sp_want) x) +
	   (SELECT count(*) FROM (TABLE lion_sp_want EXCEPT ALL TABLE lion_sp_got) y) AS differ;
DROP TABLE lion_sp_got, lion_sp_want;
-- ... and on a dirty heap
UPDATE lion_sp_r SET a = 3 WHERE g = 2 AND r % 5 = 0;
DELETE FROM lion_sp_r WHERE r % 11 = 0;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SELECT count(*) FILTER (WHERE p ~ 'Range Sources Collected: 1$') AS collected,
	   count(*) FILTER (WHERE p ~ 'Range Sources Spilled') AS spilled
FROM lion_sp_analyze('EXECUTE lion_sp_or') AS e(p);
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SELECT count(*) FILTER (WHERE p ~ 'Range Sources Collected: 1$') AS collected,
	   count(*) FILTER (WHERE p ~ 'Range Sources Spilled: 1$') AS spilled
FROM lion_sp_analyze('EXECUTE lion_sp_or') AS e(p);
CREATE TEMP TABLE lion_sp_got AS EXECUTE lion_sp_or;
RESET work_mem;
RESET hash_mem_multiplier;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
SET pg_lion.enable_count_pushdown = off;
CREATE TEMP TABLE lion_sp_want AS
	SELECT g, count(*) AS n FROM lion_sp_r WHERE (r < 524288 OR a = 3) GROUP BY g;
SET pg_lion.enable_count_pushdown = on;
SELECT (SELECT count(*) FROM lion_sp_want) AS groups,
	   (SELECT count(*) FROM (TABLE lion_sp_got EXCEPT ALL TABLE lion_sp_want) x) +
	   (SELECT count(*) FROM (TABLE lion_sp_want EXCEPT ALL TABLE lion_sp_got) y) AS differ;
DROP TABLE lion_sp_got, lion_sp_want;
DEALLOCATE lion_sp_or;
DROP TABLE lion_sp_r;

-- ---------- the FK-side join's copy of an IN list of unknown length ----------
/*
 * `x = ANY ($1)` has no length at plan time: the copy of the fact filters is
 * priced at estimate_array_length()'s guess, and planned; the run holds half
 * of the fact table's x values - a NARROW of width 2 at each of some ninety
 * container keys, which 64 kB does not hold either (rows 65 to a page
 * again).  The copy goes on in a temporary file, where it used to be
 * abandoned for a union of 5000 sets per dimension row.
 */
CREATE TABLE lion_sp_d (pk int PRIMARY KEY, attr int NOT NULL);
INSERT INTO lion_sp_d SELECT i, i % 5 FROM generate_series(1, 360) i;
CREATE TABLE lion_sp_f (fk int8, x int NOT NULL, pad text);
INSERT INTO lion_sp_f
SELECT abs(hashint4(i)) % 360 + 1, abs(hashint4(i + 1000000)) % 10000,
	   repeat('x', 80)
  FROM generate_series(1, 360000) i;
CREATE INDEX lion_sp_f_fk ON lion_sp_f USING lion (fk);
CREATE INDEX lion_sp_f_x ON lion_sp_f USING lion (x);
VACUUM ANALYZE lion_sp_d;
VACUUM ANALYZE lion_sp_f;
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
PREPARE lion_sp_fk(int[]) AS
	SELECT d.attr, count(*) AS n FROM lion_sp_f f JOIN lion_sp_d d ON f.fk = d.pk
	 WHERE f.x = ANY ($1) GROUP BY d.attr;
SELECT count(*) FILTER (WHERE p ~ 'Custom Scan \(LionCount\)') AS pushed,
	   count(*) FILTER (WHERE p ~ 'Fact Filters: collected once') AS planned_to_collect,
	   count(*) FILTER (WHERE p ~ 'Fact Filter Rows Collected: -1') AS could_not,
	   count(*) FILTER (WHERE p ~ 'Fact Filter Copies Spilled: 1$') AS spilled
FROM lion_sp_analyze(format('EXECUTE lion_sp_fk(%L)',
							(SELECT array_agg(i)::text FROM generate_series(0, 4999) i))) AS e(p);
DO $$
BEGIN
	EXECUTE format('CREATE TEMP TABLE lion_sp_got AS EXECUTE lion_sp_fk(%L)',
				   (SELECT array_agg(i)::text FROM generate_series(0, 4999) i));
END $$;
RESET work_mem;
RESET hash_mem_multiplier;
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_nestloop;
SET pg_lion.enable_count_pushdown = off;
CREATE TEMP TABLE lion_sp_want AS
	SELECT d.attr, count(*) AS n FROM lion_sp_f f JOIN lion_sp_d d ON f.fk = d.pk
	 WHERE f.x < 5000 GROUP BY d.attr;
SET pg_lion.enable_count_pushdown = on;
SELECT (SELECT count(*) FROM lion_sp_want) AS groups,
	   (SELECT count(*) FROM (TABLE lion_sp_got EXCEPT ALL TABLE lion_sp_want) x) +
	   (SELECT count(*) FROM (TABLE lion_sp_want EXCEPT ALL TABLE lion_sp_got) y) AS differ;
DROP TABLE lion_sp_got, lion_sp_want;
DEALLOCATE lion_sp_fk;
RESET plan_cache_mode;
DROP TABLE lion_sp_f, lion_sp_d;
DROP FUNCTION lion_sp_analyze(text);
