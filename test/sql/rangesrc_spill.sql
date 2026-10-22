-- Sets too large for a hash table's memory, spilled and priced (DESIGN.md
-- §32, "Too large to collect"; §27, "Probed, then collected").
--
-- A range taken as a source that does not fit what the relation's ranges
-- share of a hash table's memory is collected in windows into a temporary
-- file, an OR's leaf as any other, and read from there by every way of
-- counting a collected set; the planner prices the windows and the reads,
-- with the memory the ranges before it hold, and walks such a range at every
-- count instead only where that is the cheaper - few counts, or existence
-- tests.  The FK-side join's copy of its fact filters spills the same way,
-- planned or switched to part way through a run.  Every answer is checked
-- against the same query with the pushdown off, at 64 kB and with the
-- memory to hold everything: the results never depend on it.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

/*
 * lion_rss() runs q with the pushdown forced (every other scan and join
 * disabled) and again with the pushdown off and the planner left alone, and
 * compares the answers as multisets.  It reports how the node took its
 * ranges - collected, and of those spilled, and walked - and the IN lists it
 * collected, and, when it did, that it collected the WHERE once and how the
 * FK-side join took its fact filters.
 */
CREATE FUNCTION lion_rss(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	j jsonb;
	node jsonb;
	how text;
	got text;
	want text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	EXECUTE 'EXPLAIN (ANALYZE, TIMING OFF, COSTS OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) '
		|| q INTO j;
	node := jsonb_path_query_first(j, 'strict $.** ? (@."Custom Plan Provider" == "LionCount")');
	IF node IS NULL THEN
		how := 'not pushed down';
	ELSE
		how := 'collected ' || coalesce(node ->> 'Range Sources Collected', '0') ||
			coalesce(' (spilled ' || (node ->> 'Range Sources Spilled') || ')', '') ||
			', walked ' || coalesce(node ->> 'Range Sources Walked', '0') ||
			coalesce(', lists collected ' || (node ->> 'Lists Collected') ||
					 coalesce(' (spilled ' || (node ->> 'Lists Spilled') || ')', ''), '') ||
			coalesce(', WHERE collected ' || (node ->> 'WHERE Sets Collected'), '') ||
			coalesce(', filters ' || (node ->> 'Fact Filters'), '') ||
			coalesce(', switched ' || (node ->> 'Fact Filter Switches'), '') ||
			coalesce(', copies spilled ' || (node ->> 'Fact Filter Copies Spilled'), '');
	END IF;
	EXECUTE format('SELECT coalesce(string_agg(s::text, '' '' ORDER BY s::text), ''(none)'') FROM (%s) s', q)
		INTO got;

	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_nestloop', 'on', true);
	EXECUTE format('SELECT coalesce(string_agg(s::text, '' '' ORDER BY s::text), ''(none)'') FROM (%s) s', q)
		INTO want;
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	IF got IS DISTINCT FROM want THEN
		RETURN 'MISMATCH ' || how || ': ' || got || ' vs ' || want;
	END IF;
	RETURN how || ': ' || left(got, 60);
END $$;

/*
 * lion_rss_cost() is the total cost of the plan the pushdown forced makes of
 * q - under `setting` = `val`, when it is given - and NULL when the node is
 * not in it.
 */
CREATE FUNCTION lion_rss_cost(q text, setting text DEFAULT NULL,
							  val text DEFAULT NULL) RETURNS float8
LANGUAGE plpgsql AS $$
DECLARE
	j jsonb;
	node jsonb;
	old text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	IF setting IS NOT NULL THEN
		old := current_setting(setting);
		PERFORM set_config(setting, val, true);
	END IF;
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	IF setting IS NOT NULL THEN
		PERFORM set_config(setting, old, true);
	END IF;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_nestloop', 'on', true);
	node := jsonb_path_query_first(j, 'strict $.** ? (@."Custom Plan Provider" == "LionCount")');
	IF node IS NULL THEN
		RETURN NULL;
	END IF;
	RETURN (j -> 0 -> 'Plan' ->> 'Total Cost')::float8;
END $$;

/*
 * lion_rss_pages() is how many directory pages the plan the pushdown forced
 * makes of q reads, at a work_mem of `mem`: every walk of a range reads its
 * own.
 */
CREATE FUNCTION lion_rss_pages(q text, mem text) RETURNS int
LANGUAGE plpgsql AS $$
DECLARE
	j jsonb;
	old text := current_setting('work_mem');
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	PERFORM set_config('work_mem', mem, true);
	EXECUTE 'EXPLAIN (ANALYZE, TIMING OFF, COSTS OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) '
		|| q INTO j;
	PERFORM set_config('work_mem', old, true);
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
	RETURN (jsonb_path_query_first(j, 'strict $.** ? (@."Custom Plan Provider" == "LionCount")')
			->> 'Directory Pages Read')::int;
END $$;

-- ---------- the tables ----------
-- 360,000 rows 65 to a page: some ninety container keys, and a set in no
-- order at most a NARROW of width 2 (DESIGN.md §38), a kilobyte, at each.
-- r is in no order, so a range over half of it is a container at each key,
-- about 360 kB as its union builds it - far past the 64 kB below - and s is
-- another such column; u is in heap order.  g has 7 values, h 300, a 30
-- (each for two rows in turn, so that a range or a list of it is RUNs of a
-- few hundred bytes a key), fk 2,000 scattered over the heap.
CREATE TABLE lion_rss_t (u int NOT NULL, r int NOT NULL, s int NOT NULL,
						 g int NOT NULL, h int NOT NULL, a int NOT NULL,
						 fk int NOT NULL, pad text);
INSERT INTO lion_rss_t
SELECT i, hashint4(i) & 1048575, hashint4(i + 3) & 1048575, i % 7, i % 300,
	   i / 2 % 30, (hashint4(i + 7) & 2147483647) % 2000 + 1, repeat('x', 67)
  FROM generate_series(1, 360000) i;
CREATE INDEX lion_rss_t_u ON lion_rss_t USING lion (u)
	WITH (summaries = on, summary_tids = 1024);
CREATE INDEX lion_rss_t_r ON lion_rss_t USING lion (r)
	WITH (summaries = on, summary_tids = 1024);
CREATE INDEX lion_rss_t_s ON lion_rss_t USING lion (s)
	WITH (summaries = on, summary_tids = 1024);
CREATE INDEX lion_rss_t_g ON lion_rss_t USING lion (g);
CREATE INDEX lion_rss_t_h ON lion_rss_t USING lion (h);
CREATE INDEX lion_rss_t_a ON lion_rss_t USING lion (a);
CREATE INDEX lion_rss_t_fk ON lion_rss_t USING lion (fk);
CREATE TABLE lion_rss_d (pk int PRIMARY KEY, attr int NOT NULL);
INSERT INTO lion_rss_d SELECT i, i % 10 FROM generate_series(1, 2000) i;
VACUUM (FREEZE, ANALYZE) lion_rss_t;
VACUUM (FREEZE, ANALYZE) lion_rss_d;

SET work_mem = '64kB';
SET hash_mem_multiplier = 1;

-- ---------- 1. an OR's leaf too large to collect: spilled, and priced ----------
-- It cannot be walked, and it is not priced out of the plan either: the
-- node's price is its collection in windows, far below what a refused path
-- costs.
SELECT lion_rss('SELECT g, count(*) FROM lion_rss_t WHERE (r < 524288 OR a = 3) GROUP BY g');
SELECT lion_rss('SELECT count(*) FROM lion_rss_t WHERE g = 1 AND (r < 524288 OR a = 3)');
SELECT lion_rss_cost('SELECT g, count(*) FROM lion_rss_t WHERE (r < 524288 OR a = 3) GROUP BY g') < 1e9
	AS priced;

-- ---------- 2. a range too large to collect: spilled, or walked if cheaper ----------
-- Many groups count against the one spilled copy, where walking the range
-- at every one of them would walk it 300 times; so do a few, the windows
-- walking it once; beside another clause the WHERE is collected once, from
-- the file, and the groups counted against that.  Existence tests stop at
-- their first row, and walking the range at each is the cheaper: that is
-- the planner's choice, priced.
SELECT lion_rss('SELECT h, count(*) FROM lion_rss_t WHERE r < 524288 GROUP BY h');
SELECT lion_rss('SELECT g, count(*) FROM lion_rss_t WHERE r < 524288 GROUP BY g');
SELECT lion_rss('SELECT g, count(*) FROM lion_rss_t WHERE r < 524288 AND a < 20 GROUP BY g');
SELECT lion_rss('SELECT count(DISTINCT a) FROM lion_rss_t WHERE r < 524288');
SELECT lion_rss('SELECT count(*) FROM lion_rss_t WHERE u >= 100 AND r BETWEEN 1000 AND 1040000');
SELECT lion_rss('SELECT g, h, count(*) FROM lion_rss_t WHERE r > 300000 GROUP BY g, h');
-- Only the ranges the planner priced walked are: beside one, a range it sized
-- to fit and whose union outgrows the memory all the same is collected in
-- windows, not walked at every set the walk of the first hands out.  The
-- existence tests of a count(DISTINCT) still walk the first; counted by
-- groups, the two are both collected in windows, each walked once, and the
-- WHERE once from their files: 11 ms, where walking the first at every
-- group took 49.
SELECT lion_rss('SELECT count(DISTINCT a) FROM lion_rss_t WHERE r < 524288 AND s < 65536');
SELECT lion_rss('SELECT g, count(*) FROM lion_rss_t WHERE r < 524288 AND s < 65536 GROUP BY g');

-- ---------- 3. the ranges of a relation share the memory ----------
-- Two ranges of about 360 kB each as the planner sizes them: at 512 kB one
-- fits and the other does not, at 2 MB both do.  The planner budgets them as
-- the executor holds them, one after the other out of the one hash table's
-- memory - so the two cost more where only one fits.  The executor's unions
-- come to about 90 kB each, optimized: at 256 kB the first fits, and the
-- second does not fit what it left - so the first goes to a file to make
-- room, and the second is collected in what that freed.  That is a walk of
-- each and the try of the second, where windows of what the first left
-- would have written and read the second's containers again for every
-- halving of it.
SELECT lion_rss_cost('SELECT g, count(*) FROM lion_rss_t WHERE r < 524288 AND s < 524288 GROUP BY g',
					 'work_mem', '512kB') >
	   lion_rss_cost('SELECT g, count(*) FROM lion_rss_t WHERE r < 524288 AND s < 524288 GROUP BY g',
					 'work_mem', '2MB') AS shared;
SET work_mem = '256kB';
SELECT lion_rss('SELECT g, count(*) FROM lion_rss_t WHERE r < 524288 AND s < 524288 GROUP BY g');
SELECT lion_rss_pages(q, '256kB') < lion_rss_pages(q, '64MB') +
	   lion_rss_pages('SELECT g, count(*) FROM lion_rss_t WHERE s < 524288 GROUP BY g', '64MB')
	   AS walked_about_once
  FROM (VALUES ('SELECT g, count(*) FROM lion_rss_t WHERE r < 524288 AND s < 524288 GROUP BY g')) v(q);
SET work_mem = '64kB';
SELECT lion_rss('SELECT g, count(*) FROM lion_rss_t WHERE r < 524288 AND s < 524288 GROUP BY g');

-- ---------- 4. the FK-side join's copy of its fact filters, spilled ----------
-- A planned copy past the memory goes to a temporary file, and every count
-- reads what it looks up of it back from there (pg_lion.spill_read_cost):
-- the rows of two ranges, 90,000 in no order, are a NARROW at each key.  A
-- range among the filters is collected the same way, spilled, and the copy
-- made of it; RUNs of a list fit.
SELECT lion_rss('SELECT d.attr, count(*) FROM lion_rss_t f JOIN lion_rss_d d ON f.fk = d.pk
	WHERE f.r < 524288 AND f.s < 524288 GROUP BY d.attr');
SELECT lion_rss('SELECT d.attr, count(*) FROM lion_rss_t f JOIN lion_rss_d d ON f.fk = d.pk
	WHERE f.a IN (1, 2, 3, 4, 5) GROUP BY d.attr');
SELECT lion_rss('SELECT d.attr, count(*) FROM lion_rss_t f JOIN lion_rss_d d ON f.fk = d.pk
	WHERE f.r < 524288 GROUP BY d.attr');
SELECT lion_rss('SELECT d.attr, count(*) FROM lion_rss_t f JOIN lion_rss_d d ON f.fk = d.pk
	WHERE (f.r < 524288 OR f.a = 3) GROUP BY d.attr');
-- the reads are priced where the copy spills, and only there
SELECT lion_rss_cost(q, 'pg_lion.spill_read_cost', '0') < lion_rss_cost(q) AS spilled_priced
  FROM (VALUES ('SELECT d.attr, count(*) FROM lion_rss_t f JOIN lion_rss_d d ON f.fk = d.pk
	WHERE f.r < 524288 AND f.s < 524288 GROUP BY d.attr')) v(q);
SET work_mem = '64MB';
SELECT lion_rss_cost(q, 'pg_lion.spill_read_cost', '0') = lion_rss_cost(q) AS fits_unpriced
  FROM (VALUES ('SELECT d.attr, count(*) FROM lion_rss_t f JOIN lion_rss_d d ON f.fk = d.pk
	WHERE f.r < 524288 AND f.s < 524288 GROUP BY d.attr')) v(q);
SET work_mem = '64kB';

-- ---------- 5. an IN list too long to locate at once, collected and spilled ----------
-- Past lion_array_batch_size() - a thousand values at 64 kB - a count that
-- is not of the relation as one row collects the list's sets, a batch of
-- values at a time, into one union as it collects a range, in the same
-- memory: 1,500 of fk's 2,000 values are 270,000 rows in no order, a union
-- far past 64 kB, spilled in windows.  Beside a range the two share the
-- memory; a plain count takes the first long list a batch at a time and
-- collects a second; the FK-side join collects a fact filter's list; a list
-- under an OR is collected as a range leaf is; and the list on the grouping
-- column drives the groups, located a batch of values at a time as they are
-- walked.
SELECT lion_rss('SELECT g, count(*) FROM lion_rss_t
	WHERE fk = ANY (array(SELECT i FROM generate_series(1, 1500) i)) GROUP BY g');
SELECT lion_rss('SELECT count(DISTINCT g) FROM lion_rss_t
	WHERE fk = ANY (array(SELECT i FROM generate_series(1, 1500) i))');
SELECT lion_rss('SELECT g, count(*) FROM lion_rss_t
	WHERE fk = ANY (array(SELECT i FROM generate_series(1, 1500) i)) AND r < 524288 GROUP BY g');
SELECT lion_rss('SELECT count(*) FROM lion_rss_t
	WHERE fk = ANY (array(SELECT i FROM generate_series(1, 1500) i))
	  AND u = ANY (array(SELECT i * 3 FROM generate_series(1, 90000) i))');
SELECT lion_rss('SELECT d.attr, count(*) FROM lion_rss_t f JOIN lion_rss_d d ON f.fk = d.pk
	WHERE f.u = ANY (array(SELECT i * 3 FROM generate_series(1, 90000) i)) GROUP BY d.attr');
SELECT lion_rss('SELECT g, count(*) FROM lion_rss_t
	WHERE fk = ANY (array(SELECT i FROM generate_series(1, 1500) i)) OR a = 3 GROUP BY g');
SELECT lion_rss('SELECT fk, count(*) FROM lion_rss_t
	WHERE fk = ANY (array(SELECT i FROM generate_series(1, 1500) i)) GROUP BY fk');

-- ---------- 6. dirty, and the same with the memory to hold it all ----------
UPDATE lion_rss_t SET a = a + 1 WHERE u % 50 = 0;
DELETE FROM lion_rss_t WHERE u % 70 = 0;
SELECT lion_rss('SELECT g, count(*) FROM lion_rss_t WHERE (r < 524288 OR a = 3) GROUP BY g');
SELECT lion_rss('SELECT h, count(*) FROM lion_rss_t WHERE r < 524288 GROUP BY h');
SELECT lion_rss('SELECT count(DISTINCT a) FROM lion_rss_t WHERE r < 524288');
SELECT lion_rss('SELECT d.attr, count(*) FROM lion_rss_t f JOIN lion_rss_d d ON f.fk = d.pk
	WHERE f.r < 524288 GROUP BY d.attr');
SELECT lion_rss('SELECT g, count(*) FROM lion_rss_t
	WHERE fk = ANY (array(SELECT i FROM generate_series(1, 1500) i)) GROUP BY g');
SELECT lion_rss('SELECT g, count(*) FROM lion_rss_t
	WHERE fk = ANY (array(SELECT i FROM generate_series(1, 1500) i)) OR a = 3 GROUP BY g');
SELECT lion_rss('SELECT fk, count(*) FROM lion_rss_t
	WHERE fk = ANY (array(SELECT i FROM generate_series(1, 1500) i)) GROUP BY fk');
RESET work_mem;
RESET hash_mem_multiplier;
SELECT lion_rss('SELECT g, count(*) FROM lion_rss_t WHERE (r < 524288 OR a = 3) GROUP BY g');
SELECT lion_rss('SELECT h, count(*) FROM lion_rss_t WHERE r < 524288 GROUP BY h');
SELECT lion_rss('SELECT g, count(*) FROM lion_rss_t
	WHERE fk = ANY (array(SELECT i FROM generate_series(1, 1500) i)) GROUP BY g');
SELECT lion_rss('SELECT count(DISTINCT a) FROM lion_rss_t WHERE r < 524288');
SELECT lion_rss('SELECT d.attr, count(*) FROM lion_rss_t f JOIN lion_rss_d d ON f.fk = d.pk
	WHERE f.r < 524288 GROUP BY d.attr');

DROP TABLE lion_rss_t, lion_rss_d;
DROP FUNCTION lion_rss(text);
DROP FUNCTION lion_rss_cost(text, text, text);
DROP FUNCTION lion_rss_pages(text, text);
