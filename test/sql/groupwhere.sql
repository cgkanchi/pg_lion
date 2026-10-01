-- A GROUP BY count reads its WHERE once (DESIGN.md §10, "The WHERE sets,
-- collected once").
--
-- The count pushdown answers `g, count(*) ... WHERE <filters> GROUP BY g`
-- with one count per entry of g, each the entry's posting set ANDed with the
-- filters.  Each count used to intersect the filters again, reading every
-- filter set it kept no private copy of - an OR's leaves, a set too large to
-- copy - so a GROUP BY of many groups read the filters about once per group.
-- Now the filters are collected once per relation into one private set, and
-- every group is counted against it.  Every query below is checked against a
-- sequential scan with the pushdown off; the node's EXPLAIN ANALYZE says how
-- often it collected the filters, and in how many batches the groups were
-- then counted against them ("The groups of a walk, counted together"); and
-- the shared buffers a grouped count reads are compared with those of the
-- same count ungrouped.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
CREATE EXTENSION IF NOT EXISTS pg_buffercache;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

/*
 * lgw_check() runs q with every other scan disabled, so that a LionCount path
 * that is built at all is the plan: under EXPLAIN ANALYZE, and again for its
 * rows.  Then it runs want (q itself when NULL) as a sequential scan with the
 * pushdown off.  It says whether the node ran, how often it collected its
 * WHERE, how often the collection spilled and in how many batches the groups
 * were counted against it, and whether the answer has more than one row,
 * after proving the two answers equal as multisets.
 */
CREATE FUNCTION lgw_check(q text, want text DEFAULT NULL) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	pushed boolean := false;
	collected int := 0;
	spilled int := 0;
	batches int := 0;
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
			pushed := true;
		ELSIF ln ~ 'WHERE Sets Collected: ' THEN
			collected := substring(ln FROM 'WHERE Sets Collected: (\d+)')::int;
		ELSIF ln ~ 'WHERE Sets Spilled: ' THEN
			spilled := substring(ln FROM 'WHERE Sets Spilled: (\d+)')::int;
		ELSIF ln ~ 'Group Batches: ' THEN
			batches := substring(ln FROM 'Group Batches: (\d+)')::int;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lgw_on AS %s', q);

	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	EXECUTE format('CREATE TEMP TABLE lgw_off AS %s', coalesce(want, q));
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM lgw_off' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (TABLE lgw_on EXCEPT ALL TABLE lgw_off) x)'
			' + (SELECT count(*) FROM (TABLE lgw_off EXCEPT ALL TABLE lgw_on) y)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lgw_on, lgw_off';

	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, collected %s, spilled %s, batches %s, %s',
				  CASE WHEN pushed THEN 'pushed down' ELSE 'NOT PUSHED DOWN' END,
				  collected, spilled, batches,
				  CASE WHEN nrows > 1 THEN 'several rows'
					   WHEN nrows = 1 THEN 'one row' ELSE 'no row' END);
END $$;

/*
 * The table: g 50 groups over every heap page, and NULL in some rows; h 7
 * values; a, b and c filters of a quarter, a fifth and a third of the rows,
 * each a CHAIN set of a few posting leaves; n with NULLs; r scattered over
 * 1000 values, for a range; one a single value; u unique; tags a small
 * array.  About 55 rows a page, some 730 pages: a dozen container keys.
 */
CREATE TABLE lgw (id int NOT NULL, g int, h int NOT NULL, a int NOT NULL,
				  b int NOT NULL, c int NOT NULL, n int, r int NOT NULL,
				  one int NOT NULL, u int NOT NULL, tags int[] NOT NULL,
				  pad text NOT NULL)
	WITH (autovacuum_enabled = off);
INSERT INTO lgw
SELECT i, CASE WHEN i % 97 = 0 THEN NULL ELSE i % 50 END, i % 7, i % 4, i % 5,
	   i % 3, CASE WHEN i % 11 = 0 THEN NULL ELSE i % 13 END, (i * 7919) % 1000,
	   7, i, ARRAY[i % 6, 10 + i % 4], repeat('x', 50)
  FROM generate_series(1, 40000) i;
CREATE INDEX lgw_g ON lgw USING lion (g);
CREATE INDEX lgw_h ON lgw USING lion (h);
CREATE INDEX lgw_a ON lgw USING lion (a);
CREATE INDEX lgw_b ON lgw USING lion (b);
CREATE INDEX lgw_c ON lgw USING lion (c);
CREATE INDEX lgw_n ON lgw USING lion (n);
CREATE INDEX lgw_r ON lgw USING lion (r);
CREATE INDEX lgw_one ON lgw USING lion (one);
CREATE INDEX lgw_u ON lgw USING lion (u);
CREATE INDEX lgw_tags ON lgw USING lion (tags);
VACUUM (FREEZE, ANALYZE) lgw;

-- ---------- 1. filters collected once ----------
-- two and three filters; the NULL group is one of the groups
SELECT lgw_check('SELECT g, count(*) FROM lgw WHERE a = 1 AND b = 1 GROUP BY g');
SELECT lgw_check('SELECT g, count(*) FROM lgw WHERE a = 1 AND b = 1 AND c = 1 GROUP BY g');
-- a negated filter, and the NULL entry as a filter
SELECT lgw_check('SELECT g, count(*) FROM lgw WHERE a = 1 AND n IS NOT NULL GROUP BY g');
SELECT lgw_check('SELECT g, count(*) FROM lgw WHERE n IS NULL AND b = 2 GROUP BY g');
-- IN lists: on another column beside a filter, alone, and on the group column
SELECT lgw_check('SELECT g, count(*) FROM lgw WHERE a IN (1, 2) AND b = 1 GROUP BY g');
SELECT lgw_check('SELECT g, count(*) FROM lgw WHERE a IN (1, 3) GROUP BY g');
SELECT lgw_check('SELECT g, count(*) FROM lgw WHERE g IN (1, 2, 3, 4, 5) AND a = 1 AND c = 1 GROUP BY g');
-- a range taken as a source, beside a filter
SELECT lgw_check('SELECT g, count(*) FROM lgw WHERE r < 300 AND a = 1 GROUP BY g');
-- ORs across columns, beside a filter and alone
SELECT lgw_check('SELECT g, count(*) FROM lgw WHERE (a = 1 OR b = 1) AND c = 1 GROUP BY g');
SELECT lgw_check('SELECT g, count(*) FROM lgw WHERE a = 1 OR b = 2 GROUP BY g');
-- multi-key queries of two keys, alone and beside a filter
SELECT lgw_check($$SELECT g, count(*) FROM lgw WHERE tags @> '{1, 13}' GROUP BY g$$);
SELECT lgw_check($$SELECT g, count(*) FROM lgw WHERE tags && '{1, 12}' AND b = 1 GROUP BY g$$);
-- two GROUP BY columns: every pair is counted against the one set
SELECT lgw_check('SELECT h, g, count(*) FROM lgw WHERE a = 1 AND b = 1 GROUP BY h, g');

-- ---------- 2. and where there is nothing to gain ----------
-- no WHERE; one set, which the counts copy on their second use anyway; only
-- a set to subtract; a range alone, collected as a source already
SELECT lgw_check('SELECT g, count(*) FROM lgw GROUP BY g');
SELECT lgw_check('SELECT g, count(*) FROM lgw WHERE c = 1 GROUP BY g');
SELECT lgw_check('SELECT g, count(*) FROM lgw WHERE n IS NOT NULL GROUP BY g');
SELECT lgw_check('SELECT g, count(*) FROM lgw WHERE r < 300 GROUP BY g');
-- count(DISTINCT): tests that stop at a row, not counts
SELECT lgw_check('SELECT g, count(DISTINCT h) FROM lgw WHERE a = 1 AND b = 1 GROUP BY g');
-- one group: a count with no other to follow is never preceded by a
-- collection
SELECT lgw_check('SELECT one, count(*) FROM lgw WHERE a = 1 AND b = 1 GROUP BY one');
-- four groups of one row each: all they read of the filters together is
-- less than what collecting them reads
SELECT lgw_check('SELECT u, count(*) FROM lgw WHERE u BETWEEN 101 AND 104 AND a = 1 AND b = 1 GROUP BY u');

-- ---------- 3. parameters, rescans, partitions ----------
-- a generic plan
SET plan_cache_mode = force_generic_plan;
PREPARE lgw_prep(int, int) AS
	SELECT g, count(*) AS n FROM lgw WHERE a = $1 AND b = $2 GROUP BY g;
SELECT lgw_check('EXECUTE lgw_prep(1, 1)',
				 'SELECT g, count(*) AS n FROM lgw WHERE a = 1 AND b = 1 GROUP BY g');
DEALLOCATE lgw_prep;
RESET plan_cache_mode;
-- a LATERAL subquery, rescanned with a new value for each outer row: one
-- collection per scan
SELECT lgw_check('SELECT v.x, s.g, s.n FROM (VALUES (1), (2), (3)) v(x),
	LATERAL (SELECT g, count(*) AS n FROM lgw WHERE a = v.x AND b = 1 GROUP BY g) s');
-- a partitioned table: one collection per partition
CREATE TABLE lgw_p (id int NOT NULL, g int, a int NOT NULL, b int NOT NULL)
	PARTITION BY RANGE (id);
CREATE TABLE lgw_p1 PARTITION OF lgw_p FOR VALUES FROM (1) TO (10001)
	WITH (autovacuum_enabled = off);
CREATE TABLE lgw_p2 PARTITION OF lgw_p FOR VALUES FROM (10001) TO (20001)
	WITH (autovacuum_enabled = off);
INSERT INTO lgw_p SELECT id, g, a, b FROM lgw WHERE id <= 20000;
CREATE INDEX lgw_p_g ON lgw_p USING lion (g);
CREATE INDEX lgw_p_a ON lgw_p USING lion (a);
CREATE INDEX lgw_p_b ON lgw_p USING lion (b);
VACUUM (FREEZE, ANALYZE) lgw_p1;
VACUUM (FREEZE, ANALYZE) lgw_p2;
ANALYZE lgw_p;
SELECT lgw_check('SELECT g, count(*) FROM lgw_p WHERE a = 1 AND b = 1 GROUP BY g');
DROP TABLE lgw_p;

-- ---------- 4. a cursor paused between groups ----------
/*
 * The collected set holds no pin, and the node pins no page of the indexes
 * while the cursor waits (DESIGN.md §15, "Paused and finished counts"): two
 * rows, the pins, the rest, the pins again, and the rows against the
 * ordinary plan's.  The checkpoint leaves the background writer no dirty
 * index page to pin while the pins are counted.
 */
CHECKPOINT;
CREATE FUNCTION lgw_pinned() RETURNS bigint
LANGUAGE sql AS $$
	SELECT count(*) FROM pg_buffercache
	 WHERE reldatabase = (SELECT oid FROM pg_database WHERE datname = current_database())
	   AND relfilenode IN (SELECT pg_relation_filenode(c.oid) FROM pg_class c
						   WHERE c.relname LIKE 'lgw\_%' AND c.relkind = 'i')
	   AND pinning_backends > 0
$$;
CREATE FUNCTION lgw_cursor(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	c refcursor := 'lgw_cur';
	r record;
	paused bigint;
	done bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE 'CREATE TEMP TABLE lgw_on (r text)';
	OPEN c FOR EXECUTE q;
	FOR i IN 1 .. 2 LOOP
		FETCH c INTO r;
		EXECUTE 'INSERT INTO lgw_on VALUES ($1)' USING r::text;
	END LOOP;
	paused := lgw_pinned();
	LOOP
		FETCH c INTO r;
		EXIT WHEN NOT FOUND;
		EXECUTE 'INSERT INTO lgw_on VALUES ($1)' USING r::text;
	END LOOP;
	done := lgw_pinned();
	CLOSE c;

	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	EXECUTE format('CREATE TEMP TABLE lgw_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT (SELECT count(*) FROM (TABLE lgw_on EXCEPT ALL TABLE lgw_off) x)'
			' + (SELECT count(*) FROM (TABLE lgw_off EXCEPT ALL TABLE lgw_on) y)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lgw_on, lgw_off';

	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s pinned while paused, %s when done', paused, done);
END $$;
SELECT lgw_cursor('SELECT g, count(*) FROM lgw WHERE (a = 1 OR b = 1) AND c = 1 GROUP BY g');
SELECT lgw_cursor('SELECT h, g, count(*) FROM lgw WHERE a = 1 AND b = 1 GROUP BY h, g');
DROP FUNCTION lgw_cursor(text);
DROP FUNCTION lgw_pinned();

-- ---------- 5. what a grouped count reads ----------
/*
 * lgw_bufs() is the shared buffers the top node of q touches, hits and reads
 * together, with every other scan disabled; q runs once before, so that what
 * a first run reads of the catalogs is not counted.
 *
 * (a = 1 OR b = 1) is an OR of two CHAIN sets of a few posting leaves each,
 * which no count keeps a copy of: each of the 51 groups used to read those
 * leaves again.  Collected once, the grouped count reads them once, so beyond
 * what the walk of the groups reads - the same walk without the WHERE - it
 * reads about what the ungrouped count does, and three times that is a
 * margin, not a fit.
 */
CREATE FUNCTION lgw_bufs(q text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	res bigint := NULL;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE q;
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF res IS NULL AND ln ~ 'Buffers: shared' THEN
			res := coalesce(substring(ln FROM 'hit=(\d+)')::bigint, 0) +
				   coalesce(substring(ln FROM 'read=(\d+)')::bigint, 0);
		END IF;
	END LOOP;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	RETURN res;
END $$;
SELECT lgw_bufs('SELECT g, count(*) FROM lgw WHERE (a = 1 OR b = 1) AND c = 1 GROUP BY g') AS grouped,
	   lgw_bufs('SELECT g, count(*) FROM lgw GROUP BY g') AS walk,
	   lgw_bufs('SELECT count(*) FROM lgw WHERE (a = 1 OR b = 1) AND c = 1') AS once \gset
SELECT :grouped - :walk <= 3 * :once AS where_read_once;
DROP FUNCTION lgw_bufs(text);

-- ---------- 6. a heap the visibility map cannot vouch for ----------
UPDATE lgw SET pad = pad || 'y' WHERE id % 40 = 0;
DELETE FROM lgw WHERE id % 170 = 0;
SELECT lgw_check('SELECT g, count(*) FROM lgw WHERE a = 1 AND b = 1 GROUP BY g');
SELECT lgw_check('SELECT g, count(*) FROM lgw WHERE a = 1 AND n IS NOT NULL GROUP BY g');
SELECT lgw_check('SELECT g, count(*) FROM lgw WHERE g IN (1, 2, 3, 4, 5) AND a = 1 AND c = 1 GROUP BY g');
SELECT lgw_check('SELECT g, count(*) FROM lgw WHERE (a = 1 OR b = 1) AND c = 1 GROUP BY g');
SELECT lgw_check('SELECT h, g, count(*) FROM lgw WHERE a = 1 AND b = 1 GROUP BY h, g');
DROP TABLE lgw;

-- ---------- 7. a collection past a hash table's memory ----------
/*
 * x and y in no order, four values each: x IN (0, 1, 2) AND y IN (0, 1, 2)
 * is 9/16 of the rows, a bitset at each of some twenty container keys - over
 * 80 kB, which 64 kB of work_mem does not hold.  The rows are narrow, 185 to
 * a page (pad is empty until the update below), so that the blocks hold
 * offsets past 127 and no container key can be a NARROW (DESIGN.md §38),
 * which would put the whole collection in some 21 kB.  The collection spills
 * to a temporary file, and the groups are counted against the file.
 */
CREATE TABLE lgw_s (g int NOT NULL, x int NOT NULL, y int NOT NULL,
					pad text NOT NULL)
	WITH (autovacuum_enabled = off);
INSERT INTO lgw_s
SELECT i % 20, hashint4(i) & 3, hashint4(i + 1000000) & 3, ''
  FROM generate_series(1, 240000) i;
CREATE INDEX lgw_s_g ON lgw_s USING lion (g);
CREATE INDEX lgw_s_x ON lgw_s USING lion (x);
CREATE INDEX lgw_s_y ON lgw_s USING lion (y);
VACUUM (FREEZE, ANALYZE) lgw_s;
SELECT lgw_check('SELECT g, count(*) FROM lgw_s WHERE x IN (0, 1, 2) AND y IN (0, 1, 2) GROUP BY g');
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SELECT lgw_check('SELECT g, count(*) FROM lgw_s WHERE x IN (0, 1, 2) AND y IN (0, 1, 2) GROUP BY g');
-- ... and on a dirty heap
UPDATE lgw_s SET pad = pad || 'q' WHERE g = 3 AND x = 1;
SELECT lgw_check('SELECT g, count(*) FROM lgw_s WHERE x IN (0, 1, 2) AND y IN (0, 1, 2) GROUP BY g');
RESET work_mem;
RESET hash_mem_multiplier;
DROP TABLE lgw_s;

-- ---------- 8. the groups of a walk, counted together ----------
/*
 * Once the WHERE is collected, the rest of the walk is counted a batch of
 * groups at a time, in one walk of container keys a batch (DESIGN.md §10,
 * "The groups of a walk, counted together"): at most 256 groups, fewer when
 * work_mem holds fewer cursors.  g has 600 groups, and NULL, of about eight
 * rows a container key each - ARRAYs and sparse segments; k 300 and no NULL;
 * h two, a RUN and an ARRAY; a is every other row, so that a WHERE of
 * `a = 1` alone is a BITSET at each key; b three values, r scattered over
 * 997, all three on one multicolumn index.  About 13 container keys.
 */
CREATE TABLE lgw_b (g int, k int NOT NULL, h int NOT NULL, a int NOT NULL,
					b int NOT NULL, r int NOT NULL, pad text NOT NULL)
	WITH (autovacuum_enabled = off);
INSERT INTO lgw_b
SELECT CASE WHEN i % 101 = 0 THEN NULL ELSE (i * 7) % 600 END, (i * 13) % 300,
	   CASE WHEN i % 1000 < 900 THEN 1 ELSE 2 END, i % 2, i % 3,
	   (i * 7919) % 997, repeat('z', 30)
  FROM generate_series(1, 60000) i;
CREATE INDEX lgw_b_g ON lgw_b USING lion (g);
CREATE INDEX lgw_b_k ON lgw_b USING lion (k);
CREATE INDEX lgw_b_h ON lgw_b USING lion (h);
CREATE INDEX lgw_b_abr ON lgw_b USING lion (a, b, r);
VACUUM (FREEZE, ANALYZE) lgw_b;
-- 601 groups: the first counted on its own, the rest in three batches
SELECT lgw_check('SELECT g, count(*) FROM lgw_b WHERE a = 1 AND b = 2 GROUP BY g');
-- the WHERE a BITSET at every key, which the groups are tested against as it is
SELECT lgw_check('SELECT g, count(*) FROM lgw_b WHERE a = 1 AND b IN (0, 1, 2) GROUP BY g');
-- two groups whose containers are a RUN and an ARRAY
SELECT lgw_check('SELECT h, count(*) FROM lgw_b WHERE a = 0 AND b IN (0, 2) GROUP BY h');
-- a WHERE of a row or two a key, which each group ANDs directly
SELECT lgw_check('SELECT h, count(*) FROM lgw_b WHERE a = 0 AND r = 5 GROUP BY h');
SELECT lgw_check('SELECT g, count(*) FROM lgw_b WHERE a = 0 AND r = 5 GROUP BY g');
-- a HAVING that turns some of a batch's rows away, and a LIMIT inside one
SELECT lgw_check('SELECT g, count(*) FROM lgw_b WHERE a = 1 AND b = 2 GROUP BY g HAVING count(*) > 17');
SELECT lgw_check('SELECT k, count(*) FROM lgw_b WHERE a = 1 AND b = 2 GROUP BY k ORDER BY k LIMIT 10');
-- a work_mem of a few dozen cursors: many batches
SET work_mem = '64kB';
SELECT lgw_check('SELECT g, count(*) FROM lgw_b WHERE a = 1 AND b = 2 GROUP BY g');
RESET work_mem;
-- serializable: the pages counted from the map are predicate-locked
BEGIN ISOLATION LEVEL SERIALIZABLE;
SELECT lgw_check('SELECT g, count(*) FROM lgw_b WHERE a = 0 AND b = 1 GROUP BY g');
COMMIT;
-- the batches keep the walk's order, for which an ORDER BY of a column
-- without NULLs needs no Sort
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
EXPLAIN (COSTS OFF)
SELECT k, count(*) FROM lgw_b WHERE a = 1 AND b = 2 GROUP BY k ORDER BY k;
SELECT count(*) AS groups, array_agg(k) = array_agg(k ORDER BY k) AS in_order
  FROM (SELECT k, count(*) FROM lgw_b WHERE a = 1 AND b = 2
		 GROUP BY k ORDER BY k) s;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
RESET enable_indexonlyscan;
-- a heap the map cannot vouch for: every group's rows go to the heap
UPDATE lgw_b SET pad = pad || 'w' WHERE g % 7 = 3;
DELETE FROM lgw_b WHERE g % 11 = 4;
SELECT lgw_check('SELECT g, count(*) FROM lgw_b WHERE a = 1 AND b = 2 GROUP BY g');
SELECT lgw_check('SELECT h, count(*) FROM lgw_b WHERE a = 0 AND b IN (0, 2) GROUP BY h');
DROP TABLE lgw_b;
DROP FUNCTION lgw_check(text, text);
