-- LionStoreScan: the row gather (DESIGN.md §40, "As built: the row gather").
--
-- A plain scan of the rows an AND/OR of lion indexes keeps - the set
-- LionOrdered builds, streamed a piece at a time under the pin of §9 - that
-- returns every column the query reads from the window stores of the
-- table's lion indexes: a key column of an index built with store_values,
-- or an INCLUDE column, of any of them, each column from whichever stores
-- it.  A row on a heap page the visibility map calls all-visible takes the
-- stores' values; every other row - a dirty page, a page a store left
-- ABSENT, a piece without the pin - is fetched from the heap, which settles
-- its visibility and its values together.  Every query below runs through
-- the node (every core scan off, so that nothing else can be the plan) and
-- as a sequential scan with lion's custom scans off, and the two answers are
-- compared as multisets; lsg_check() says which plan ran and where the rows
-- came from.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
CREATE EXTENSION IF NOT EXISTS pg_buffercache;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;
SET pg_lion.enable_store_scan = on;
-- an IN list of up to 128 values is one set tree (lion_scan_list_batch())
SET work_mem = '4MB';

/*
 * lsg_check() runs q with every core scan disabled, so that a LionStoreScan
 * path that is built at all is the plan, under EXPLAIN ANALYZE and again for
 * its rows; then as a sequential scan with lion's custom scans off - or runs
 * ref, the same query spelled out, where q is an EXECUTE whose generic plan
 * no setting changes - and proves the two answers equal as multisets.  It says whether the node ran
 * and what it gathered from which index (its Store lines), whether the rows
 * came from the store, the heap or both, whether a page of them was ABSENT,
 * whether the recheck removed any, and how many rows the query returned.
 */
CREATE FUNCTION lsg_check(q text, ref text DEFAULT NULL) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	used boolean := false;
	stores text := NULL;
	fromstore bigint := 0;
	fromheap bigint := 0;
	absent bigint := 0;
	removed bigint := 0;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(LionStoreScan\)' THEN
			used := true;
		ELSIF ln ~ '^\s*Store: ' THEN
			IF stores IS NULL OR position(substring(ln FROM 'Store: (.*)$') IN stores) = 0 THEN
				stores := concat_ws('; ', stores, substring(ln FROM 'Store: (.*)$'));
			END IF;
		ELSIF ln ~ 'Store Rows From Heap: ' THEN
			fromheap := fromheap + substring(ln FROM 'Store Rows From Heap: (\d+)')::bigint;
		ELSIF ln ~ 'Store Rows: ' THEN
			fromstore := fromstore + substring(ln FROM 'Store Rows: (\d+)')::bigint;
		ELSIF ln ~ 'Store Pages Absent: ' THEN
			absent := absent + substring(ln FROM 'Store Pages Absent: (\d+)')::bigint;
		ELSIF ln ~ 'Rows Removed by Lion Recheck: ' THEN
			removed := removed + substring(ln FROM 'Rows Removed by Lion Recheck: (\d+)')::bigint;
		END IF;
	END LOOP;
	EXECUTE 'CREATE TEMP TABLE lsg_on AS ' || q;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('pg_lion.enable_store_scan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('pg_lion.enable_ordered_scan', 'off', true);
	EXECUTE 'CREATE TEMP TABLE lsg_off AS ' || coalesce(ref, q);
	PERFORM set_config('pg_lion.enable_store_scan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('pg_lion.enable_ordered_scan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	SELECT count(*) INTO nrows FROM lsg_on;
	SELECT (SELECT count(*) FROM (SELECT o::text FROM lsg_on o EXCEPT ALL
									SELECT f::text FROM lsg_off f) x) +
		   (SELECT count(*) FROM (SELECT f::text FROM lsg_off f EXCEPT ALL
									SELECT o::text FROM lsg_on o) y)
	  INTO ndiff;
	DROP TABLE lsg_on;
	DROP TABLE lsg_off;
	RETURN format('%s, %s rows, %s',
				  CASE WHEN used THEN
					   format('store scan (%s): %s%s%s', stores,
							  CASE WHEN fromstore > 0 AND fromheap > 0 THEN 'store and heap'
								   WHEN fromstore > 0 THEN 'all from the store'
								   WHEN fromheap > 0 THEN 'all from the heap'
								   ELSE 'no rows' END,
							  CASE WHEN absent > 0 THEN ', absent pages' ELSE '' END,
							  CASE WHEN removed > 0 THEN ', rows rechecked away' ELSE '' END)
					   ELSE 'not the store scan' END,
				  nrows,
				  CASE WHEN ndiff = 0 THEN 'same as the sequential scan'
					   ELSE format('DIFFERENT in %s rows', ndiff) END);
END $$;

/* Is LionStoreScan the plan of q - with every core scan off, or as it is? */
CREATE FUNCTION lsg_uses(q text, forced boolean DEFAULT true) RETURNS boolean
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	used boolean := false;
BEGIN
	IF forced THEN
		PERFORM set_config('enable_seqscan', 'off', true);
		PERFORM set_config('enable_bitmapscan', 'off', true);
		PERFORM set_config('enable_indexscan', 'off', true);
		PERFORM set_config('enable_indexonlyscan', 'off', true);
	END IF;
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(LionStoreScan\)' THEN
			used := true;
		END IF;
	END LOOP;
	IF forced THEN
		PERFORM set_config('enable_seqscan', 'on', true);
		PERFORM set_config('enable_bitmapscan', 'on', true);
		PERFORM set_config('enable_indexscan', 'on', true);
		PERFORM set_config('enable_indexonlyscan', 'on', true);
	END IF;
	RETURN used;
END $$;

/*
 * lsg_plan() prints the plan of q, and under `actual` what it did, without
 * what differs between majors or with the heap's layout: the actual row
 * counts (18 prints them with decimals), a subplan's name (19 names it by
 * its kind), and the set's containers, which are the heap's windows.
 */
CREATE FUNCTION lsg_plan(q text, actual boolean DEFAULT false) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE CASE WHEN actual THEN
			'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) '
			ELSE 'EXPLAIN (COSTS OFF) ' END || q LOOP
		ln := regexp_replace(ln, '\s*\(actual rows=[^)]*\)', '');
		ln := regexp_replace(ln, 'Lion Set: \d+ containers', 'Lion Set: N containers');
		ln := regexp_replace(ln, 'SubPlan \S+', 'SubPlan N');
		RETURN NEXT ln;
	END LOOP;
END $$;

-- a, b and e are key columns stored with store_values; id, x, t, d and n
-- INCLUDE columns of one index each, with NULLs; c a key column of an index
-- that stores no key, and z a column nobody stores.
CREATE TABLE lsg (id int8, a int4, b int4, c int4, e int4, x int4, t text,
				  d date, n numeric, z int4)
	WITH (autovacuum_enabled = off);
INSERT INTO lsg
SELECT g, g % 7, g % 11, CASE WHEN g % 31 = 0 THEN NULL ELSE g % 13 END,
	   g % 200,
	   CASE WHEN g % 17 = 0 THEN NULL ELSE g % 1000 END,
	   CASE WHEN g % 19 = 0 THEN NULL ELSE 'v' || (g % 97) END,
	   date '2024-01-01' + g % 90,
	   CASE WHEN g % 23 = 0 THEN NULL ELSE (g % 70) / 8.0 END,
	   g % 5
  FROM generate_series(1, 30000) g;
CREATE INDEX lsg_a ON lsg USING lion (a) INCLUDE (id, x) WITH (store_values = on);
CREATE INDEX lsg_b ON lsg USING lion (b) INCLUDE (t, d) WITH (store_values = on);
CREATE INDEX lsg_c ON lsg USING lion (c) INCLUDE (n);
CREATE INDEX lsg_e ON lsg USING lion (e) WITH (store_values = on);
VACUUM (FREEZE, ANALYZE) lsg;

-- 1. The plans: the lion set's quals, the indexes read, and a Store line for
--    each index the columns come from - here two, for an AND of two of them.
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
EXPLAIN (COSTS OFF) SELECT id, a, b, x, t FROM lsg WHERE a IN (1, 2) AND b IN (3, 4, 5);
-- an OR: its arms merged window by window, a row two of them hold once
EXPLAIN (COSTS OFF) SELECT id, a, b, d FROM lsg WHERE a = 1 OR b = 3;
-- a clause the set does not answer, on columns it gathers: the filter
EXPLAIN (COSTS OFF) SELECT id, x FROM lsg WHERE a = 1 AND x > 500;
-- three indexes for the set, the columns from two of them
EXPLAIN (COSTS OFF) SELECT id, n FROM lsg WHERE a = 1 AND b = 2 AND c = 3;
SELECT * FROM lsg_plan('SELECT id, a, b, x, t FROM lsg WHERE a IN (1, 2) AND b IN (3, 4, 5)', true);
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
RESET enable_indexonlyscan;

-- 2. An AND of two indexes, the columns from both; key columns stored with
--    store_values and INCLUDE columns, NULLs among them.
SELECT lsg_check('SELECT id, a, b, x, t FROM lsg WHERE a IN (1, 2) AND b IN (3, 4, 5)');
SELECT lsg_check('SELECT a, b, d FROM lsg WHERE a = 2 AND b IN (1, 2)');
SELECT lsg_check('SELECT id, x, t, d FROM lsg WHERE a = 3 AND b = 4');
SELECT lsg_check('SELECT id, n, e FROM lsg WHERE a = 1 AND b = 2 AND c = 3');
SELECT lsg_check('SELECT id, x, n FROM lsg WHERE c IS NULL AND a < 3');
SELECT count(*), count(x), count(t), count(n) FROM lsg WHERE a = 3 AND b IN (4, 5) AND c IN (1, 2, 3);
-- ... one index alone, its key stored or not
SELECT lsg_check('SELECT id, x FROM lsg WHERE a = 5');
SELECT lsg_check('SELECT e, b, t FROM lsg WHERE e IN (7, 8, 9)');
SELECT lsg_check('SELECT id, n FROM lsg WHERE c = 6');

-- 3. An OR: of two indexes, of a list and an equality, of ANDs; and an AND
--    above an OR.
SELECT lsg_check('SELECT id, a, b, d FROM lsg WHERE a = 1 OR b = 3');
SELECT lsg_check('SELECT id, e, x FROM lsg WHERE e IN (10, 20, 30) OR a = 4');
SELECT lsg_check('SELECT id, a, b FROM lsg WHERE (a = 1 AND b = 2) OR (a = 3 AND e = 5)');
SELECT lsg_check('SELECT id, t FROM lsg WHERE (a = 1 OR a = 2) AND (b = 3 OR e = 4)');

-- 4. The driver of an AND that comes entry by entry, and batch by batch: a
--    range, and an IN list longer than a batch (at 64kB of work_mem, 32
--    values).
SELECT lsg_check('SELECT id, a, x FROM lsg WHERE a BETWEEN 2 AND 4 AND b = 3');
SELECT lsg_check('SELECT id, e FROM lsg WHERE e > 190');
SET work_mem = '64kB';
SELECT lsg_check('SELECT id, e, x FROM lsg WHERE e IN (' ||
				 (SELECT string_agg(g::text, ', ') FROM generate_series(0, 79, 2) g) ||
				 ') AND a = 1');
RESET work_mem;

-- 5. A filter on gathered columns: rows the set keeps and the filter does not.
SELECT lsg_check('SELECT id, x FROM lsg WHERE a = 1 AND x > 500');
SELECT lsg_check('SELECT id, t, d FROM lsg WHERE b = 2 AND (t LIKE ''v1%'' OR d > date ''2024-03-01'')');
SELECT lsg_check('SELECT id FROM lsg WHERE a = 6 AND b = 1 AND x IS NULL');

-- 6. Dirty pages: rows updated, deleted and inserted since the last VACUUM.
--    Their pages are not all-visible, and their rows come from the heap -
--    an old version or a deleted row is not returned - beside the store's.
UPDATE lsg SET x = x + 1 WHERE id BETWEEN 1001 AND 1400;
DELETE FROM lsg WHERE id BETWEEN 8001 AND 8300;
INSERT INTO lsg
SELECT g, g % 7, g % 11, g % 13, g % 200, g % 1000, 'w' || (g % 97),
	   date '2024-06-01' + g % 30, g / 8.0, g % 5
  FROM generate_series(30001, 30500) g;
SELECT lsg_check('SELECT id, a, b, x, t FROM lsg WHERE a IN (1, 2) AND b IN (3, 4, 5)');
SELECT lsg_check('SELECT id, a, b, d FROM lsg WHERE a = 1 OR b = 3');
SELECT lsg_check('SELECT id, x FROM lsg WHERE a = 1 AND x > 500');
-- ... in the transaction that changed them
BEGIN;
UPDATE lsg SET x = -x WHERE a = 2 AND b = 3 AND id BETWEEN 20001 AND 22000;
DELETE FROM lsg WHERE a = 2 AND b = 4 AND id BETWEEN 24001 AND 26000;
SELECT lsg_check('SELECT id, x FROM lsg WHERE a = 2 AND b IN (3, 4)');
ROLLBACK;
-- ... and once VACUUM has set them all-visible again, from the store
VACUUM lsg;
SELECT lsg_check('SELECT id, a, b, x, t FROM lsg WHERE a IN (1, 2) AND b IN (3, 4, 5)');

-- 7. An ABSENT page: a heap page whose values no store page can hold - long
--    strings that do not compress - is left to the heap, and its rows are
--    read there.  (The other kind of page the store leaves to the heap, one
--    with no store yet, has no row in any set: every insert writes the store
--    before the posting sets, lion_store_insert().)
CREATE TABLE lsg_abs (id int, s text, f text)
	WITH (toast_tuple_target = 128, autovacuum_enabled = off);
INSERT INTO lsg_abs SELECT g, repeat(md5((g % 300)::text), 62), repeat('f', 100)
  FROM generate_series(1, 400) g;
INSERT INTO lsg_abs SELECT g, 'short ' || (g % 30) FROM generate_series(401, 900) g;
CREATE INDEX lsg_abs_i ON lsg_abs USING lion (id) INCLUDE (s) WITH (store_values = on);
VACUUM (FREEZE, ANALYZE) lsg_abs;
SELECT lsg_check('SELECT id, s FROM lsg_abs WHERE id < 350');
SELECT lsg_check('SELECT id, s FROM lsg_abs WHERE id > 300');
SELECT lsg_check('SELECT id, s FROM lsg_abs WHERE id > 600');
SELECT count(*), count(DISTINCT s) FROM lsg_abs WHERE id > 300;

-- 8. Rescans: the inner side of a nested loop, read whole for every outer
--    row with a filter on the outer row's value, and stopped after its
--    first row by a semi join; each scan streams the set again from its
--    start.  And lion clauses on the outer row's value - in a correlated
--    subquery's AND, in an OR of a lateral subquery - whose keys each scan
--    evaluates again.
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_material = off;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
SELECT * FROM lsg_plan('SELECT v.k, s.id, s.x FROM (VALUES (1), (2), (3)) v(k) CROSS JOIN LATERAL (SELECT id, x FROM lsg WHERE a = 1 AND b IN (2, 3) AND x % 3 = v.k - 1 OFFSET 0) s', true);
SELECT * FROM lsg_plan('SELECT v.k FROM (VALUES (100), (500), (998), (2000)) v(k) WHERE EXISTS (SELECT 1 FROM lsg s WHERE s.a = 1 AND s.b = 2 AND s.x > v.k)', true);
SELECT * FROM lsg_plan('SELECT v.k, (SELECT sum(s.id + s.x) FROM lsg s WHERE s.a = v.k AND s.b IN (2, 3)) FROM (VALUES (1), (2), (3)) v(k)', true);
SELECT * FROM lsg_plan('SELECT v.k, s.id, s.t FROM (VALUES (1), (5)) v(k) CROSS JOIN LATERAL (SELECT id, t FROM lsg WHERE e = v.k OR b = v.k + 1 OFFSET 0) s', true);
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
RESET enable_indexonlyscan;
SELECT lsg_check('SELECT v.k, s.id, s.x FROM (VALUES (1), (2), (3)) v(k) CROSS JOIN LATERAL (SELECT id, x FROM lsg WHERE a = 1 AND b IN (2, 3) AND x % 3 = v.k - 1 OFFSET 0) s');
SELECT lsg_check('SELECT v.k FROM (VALUES (100), (500), (998), (2000)) v(k) WHERE EXISTS (SELECT 1 FROM lsg s WHERE s.a = 1 AND s.b = 2 AND s.x > v.k)');
SELECT lsg_check('SELECT v.k, (SELECT sum(s.id + s.x) FROM lsg s WHERE s.a = v.k AND s.b IN (2, 3)) FROM (VALUES (1), (2), (3)) v(k)');
SELECT lsg_check('SELECT v.k, s.id, s.t FROM (VALUES (1), (5)) v(k) CROSS JOIN LATERAL (SELECT id, t FROM lsg WHERE e = v.k OR b = v.k + 1 OFFSET 0) s');
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_material;

-- 9. A cursor: paused between two rows the node holds the pins of the piece
--    it is returning, as an index-only scan holds its leaf's; once its last
--    row is out it holds none.
CREATE FUNCTION lsg_pinned() RETURNS bigint
LANGUAGE sql AS $$
	SELECT count(*) FROM pg_buffercache
	 WHERE reldatabase = (SELECT oid FROM pg_database WHERE datname = current_database())
	   AND relfilenode IN (SELECT pg_relation_filenode(c.oid) FROM pg_class c
						   WHERE c.relname LIKE 'lsg\_%' AND c.relkind = 'i')
	   AND pinning_backends > 0
$$;
BEGIN;
SET LOCAL enable_seqscan = off;
SET LOCAL enable_bitmapscan = off;
SET LOCAL enable_indexscan = off;
SET LOCAL enable_indexonlyscan = off;
DECLARE lsg_cur CURSOR FOR SELECT id, x FROM lsg WHERE a = 1 AND b IN (2, 3);
FETCH 1 FROM lsg_cur;
SELECT lsg_pinned() > 0 AS pinned_while_paused;
MOVE ALL IN lsg_cur;
SELECT lsg_pinned() AS pinned_when_done;
COMMIT;

-- 10. A partitioned table: each partition's scan gathers from its own index.
CREATE TABLE lsgp (id int, a int, x int) PARTITION BY RANGE (id);
CREATE TABLE lsgp_1 PARTITION OF lsgp FOR VALUES FROM (0) TO (5000)
	WITH (autovacuum_enabled = off);
CREATE TABLE lsgp_2 PARTITION OF lsgp FOR VALUES FROM (5000) TO (10000)
	WITH (autovacuum_enabled = off);
INSERT INTO lsgp SELECT g, g % 9, g % 100 FROM generate_series(1, 9999) g;
CREATE INDEX lsgp_a ON lsgp USING lion (a) INCLUDE (id, x) WITH (store_values = on);
VACUUM (FREEZE, ANALYZE) lsgp_1, lsgp_2;
SELECT lsg_check('SELECT id, a, x FROM lsgp WHERE a IN (1, 2)');
SELECT lsg_check('SELECT id, x FROM lsgp WHERE a = 3 AND x < 10');

-- 11. Where the node is not offered: a column the scan returns that no index
--     stores (z, or c, which lsg_c does not store), a whole row or a system
--     column, a filter on such a column, a WHERE lion cannot answer, an OR
--     whose arm comes entry by entry (a range: its rows would have to be
--     read into a set without their pins), FOR UPDATE, no column at all to
--     gather, and the setting off.  The answers are the same either way.
SELECT lsg_uses('SELECT id, z FROM lsg WHERE a = 1') AS z_unstored,
	   lsg_uses('SELECT id, c FROM lsg WHERE a = 1') AS c_unstored,
	   lsg_uses('SELECT s FROM lsg s WHERE a = 1') AS whole_row,
	   lsg_uses('SELECT ctid, id FROM lsg WHERE a = 1') AS ctid,
	   lsg_uses('SELECT id FROM lsg WHERE a = 1 AND z = 3') AS z_filter,
	   lsg_uses('SELECT id FROM lsg WHERE x = 5') AS no_lion_clause,
	   lsg_uses('SELECT id FROM lsg WHERE a BETWEEN 2 AND 4 OR b = 3') AS or_range,
	   lsg_uses('SELECT id, x FROM lsg WHERE a = 1 FOR UPDATE') AS for_update,
	   lsg_uses('SELECT 1 FROM lsg WHERE a = 1') AS no_column;
SELECT lsg_check('SELECT id, z FROM lsg WHERE a = 1');
SELECT lsg_check('SELECT id FROM lsg WHERE a BETWEEN 2 AND 4 OR b = 3');
SET pg_lion.enable_store_scan = off;
SELECT lsg_uses('SELECT id, x FROM lsg WHERE a = 1') AS setting_off;
SET pg_lion.enable_store_scan = on;
SELECT lsg_uses('SELECT id, x FROM lsg WHERE a = 1') AS setting_on;

-- 12. The prices, with every scan on: a dense filter's rows are gathered
--     rather than fetched from the heap pages they cover, and a few rows -
--     whose store pages, a window's of every column each, are many more than
--     the heap pages they lie on - are fetched.
SELECT lsg_uses('SELECT id, a, b, x, t FROM lsg WHERE a IN (1, 2) AND b IN (3, 4, 5)', false) AS dense;
SELECT lsg_uses('SELECT id, a, b, x, t FROM lsg WHERE e = 7 AND b = 3', false) AS few;

-- 13. A set that outgrows hash_mem at run time - planned at 64MB (a generic
--     plan keeps it), run at 64kB - degrades to container keys (§30.4): the
--     mask of an AND keeps its driver's pieces whole, rechecked; and an OR's
--     arm that came as a long list - which only a generic plan's parameter
--     can make it at run time - is read into a set without its pins, and a
--     key of it that degraded is every TID of its window's heap pages, each
--     fetched and rechecked.  At 65 rows a page, half of 300000 rows is 72
--     NARROWs of width 2, 1 kB each, which 64kB does not hold.
CREATE TABLE lsg_big (id int, c2 int, c3 int, pad text)
	WITH (autovacuum_enabled = off);
INSERT INTO lsg_big
SELECT g, ((hashint8extended(g::bigint, 11) & 9223372036854775807) % 2)::int,
	   g % 100, repeat('x', 80)
  FROM generate_series(1, 300000) g;
CREATE INDEX lsg_big_c2 ON lsg_big USING lion (c2);
CREATE INDEX lsg_big_c3 ON lsg_big USING lion (c3) INCLUDE (id) WITH (store_values = on);
VACUUM (FREEZE, ANALYZE) lsg_big;
SET work_mem = '64MB';
SET plan_cache_mode = force_generic_plan;
PREPARE lsg_and(int) AS SELECT id, c3 FROM lsg_big WHERE c3 = $1 AND c2 = 1;
PREPARE lsg_or(int[]) AS SELECT id, c3 FROM lsg_big WHERE c3 = ANY ($1) OR c2 = 1;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
EXPLAIN (COSTS OFF) EXECUTE lsg_and(7);
EXPLAIN (COSTS OFF) EXECUTE lsg_or('{1,2,3}');
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
RESET enable_indexonlyscan;
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SELECT lsg_check('EXECUTE lsg_and(7)',
				 'SELECT id, c3 FROM lsg_big WHERE c3 = 7 AND c2 = 1');
SELECT * FROM lsg_plan('EXECUTE lsg_and(7)', true);
SELECT lsg_check('EXECUTE lsg_or(''{' ||
				 (SELECT string_agg(g::text, ',') FROM generate_series(0, 79, 2) g) || '}'')',
				 'SELECT id, c3 FROM lsg_big WHERE c3 % 2 = 0 AND c3 < 80 OR c2 = 1');
SELECT * FROM lsg_plan('EXECUTE lsg_or(''{' ||
					   (SELECT string_agg(g::text, ',') FROM generate_series(0, 79, 2) g) || '}'')', true);
RESET hash_mem_multiplier;
RESET work_mem;
RESET plan_cache_mode;
DEALLOCATE lsg_and;
DEALLOCATE lsg_or;

DROP TABLE lsg, lsg_abs, lsgp, lsg_big;
DROP FUNCTION lsg_check(text, text), lsg_uses(text, boolean), lsg_plan(text, boolean), lsg_pinned();
