-- Lion-filtered walks of a covering B-tree, index-only (DESIGN.md §40).
--
-- SELECT <columns the btree has> FROM t WHERE <lion-answerable clauses>
-- [AND <the btree's quals>] [ORDER BY <the btree's columns>]: the node walks
-- the btree, tests each TID against lion's set, and returns the members'
-- values from the INDEX TUPLES, reading the heap only for pages the
-- visibility map does not call all-visible.  With an ORDER BY it is the
-- LionOrdered of ordered.sql in its index-only mode; without one it is a
-- LionBtreeScan.  Every query below is run through the node (every core scan
-- disabled) and through the ordinary plan (the node off), and the answers are
-- compared: in order where the query orders them, as multisets otherwise.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;
SET default_statistics_target = 1000;
SET work_mem = '64MB';

CREATE TABLE bs (
	id int PRIMARY KEY,
	k int NOT NULL,		-- a permutation of the ids
	grp int,			-- 100 groups
	x int,				-- what the aggregates sum
	c200 int, c20 int,	-- lion-filtered
	tags text[],
	flag bool,
	nm name,			-- a btree stores a name as a cstring
	note text,			-- indexed by nothing: updating it is HOT
	payload text		-- in no btree, so a query reading it is heap mode;
						-- wide, so the heap is six times the covering btrees
) WITH (autovacuum_enabled = off);
INSERT INTO bs
SELECT i,
	   (i::bigint * 7919 % 100003)::int,
	   i % 100,
	   i % 1000,
	   ((hashint8extended(i::bigint, 33) & 9223372036854775807) % 200)::int,
	   ((hashint8extended(i::bigint, 22) & 9223372036854775807) % 20)::int,
	   ARRAY['t' || (i % 10), 'u' || (i % 37)],
	   i % 3 = 0,
	   ('n' || (i % 50))::name,
	   'n',
	   repeat(md5(i::text), 8)
  FROM generate_series(1, 100000) i;

-- the covering btrees
CREATE INDEX bs_k ON bs (k) INCLUDE (id, grp, x, c200, c20, nm);
CREATE INDEX bs_grp ON bs (grp) INCLUDE (x, tags);
CREATE INDEX bs_part ON bs (k) INCLUDE (id, x, c200, flag) WHERE flag;
CREATE INDEX bs_part_nf ON bs (x) INCLUDE (id, c200) WHERE flag;
CREATE INDEX bs_expr ON bs ((k % 1000), id) INCLUDE (c200);
-- the filter side: lion
CREATE INDEX bs_c200 ON bs USING lion (c200);
CREATE INDEX bs_c20 ON bs USING lion (c20);
CREATE INDEX bs_tags ON bs USING lion (tags);
VACUUM (FREEZE, ANALYZE) bs;

/*
 * lion_bs() runs q through the node - every core scan disabled, so that a
 * LionOrdered or a LionBtreeScan is the only path left that is not - and
 * through the ordinary plan (both of the node's settings off, every core scan
 * on), compares the two answers - in order when `ordered`, as multisets
 * otherwise - and names the node the first plan used, and whether it was
 * index-only.  A prepared statement's cached plan does not change with the
 * settings, so for one the ordinary answer comes from the query it stands
 * for, given as `ordinary`.
 */
CREATE FUNCTION lion_bs(q text, ordered boolean DEFAULT false,
						ordinary text DEFAULT NULL) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	r record;
	node text := 'no node';
	mode text := '';
	a text[] := '{}';
	b text[] := '{}';
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_tidscan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(Lion(Ordered|BtreeScan)\)' THEN
			node := substring(ln from 'Custom Scan \((Lion\w+)\)');
		ELSIF ln ~ '^\s*(Ordered By|Index): .*index only' THEN
			mode := ' index only';
		END IF;
	END LOOP;
	FOR r IN EXECUTE q LOOP
		a := a || r::text;
	END LOOP;

	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_tidscan', 'on', true);
	PERFORM set_config('pg_lion.enable_ordered_scan', 'off', true);
	PERFORM set_config('pg_lion.enable_btree_scan', 'off', true);
	FOR r IN EXECUTE coalesce(ordinary, q) LOOP
		b := b || r::text;
	END LOOP;
	PERFORM set_config('pg_lion.enable_ordered_scan', 'on', true);
	PERFORM set_config('pg_lion.enable_btree_scan', 'on', true);

	IF NOT ordered THEN
		SELECT coalesce(array_agg(e ORDER BY e), '{}') INTO a FROM unnest(a) e;
		SELECT coalesce(array_agg(e ORDER BY e), '{}') INTO b FROM unnest(b) e;
	END IF;
	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH: %s rows through the node, %s ordinary; first %s / %s',
					  coalesce(array_length(a, 1), 0), coalesce(array_length(b, 1), 0),
					  a[1], b[1]);
	END IF;
	RETURN format('%s%s, %s rows', node, mode, coalesce(array_length(a, 1), 0));
END $$;

/*
 * lion_bs_plan() prints the plan's NODES with nothing disabled (the plan
 * choice is the test), one per line, without the lines whose contents vary
 * between majors.
 */
CREATE FUNCTION lion_bs_plan(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln !~ '^\s*(Index Searches|Disabled|Storage|Planning|Execution|Buffers)' THEN
			RETURN NEXT ln;
		END IF;
	END LOOP;
END $$;

/*
 * lion_bs_run() runs q through the node under EXPLAIN ANALYZE and prints the
 * node's name, its index and mode, and its counters.
 */
CREATE FUNCTION lion_bs_run(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF ln ~ '(LionOrdered|LionBtreeScan|Ordered By:|Index:|Index Entries Walked|Lion Set|Heap Fetches|Rows Removed by|Switched)' THEN
			RETURN NEXT btrim(regexp_replace(ln, '\s*\(actual.*\)', ''));
		END IF;
	END LOOP;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
END $$;

-- 1. The plans, as EXPLAIN shows them: a covered btree qual with a lion
--    filter, returning INCLUDE columns, with and without an ORDER BY; the
--    same query reading a column the btree lacks walks in heap mode.
SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_indexscan = off;
EXPLAIN (COSTS OFF) SELECT id, k, grp, x FROM bs WHERE k BETWEEN 1000 AND 3000 AND c200 = 17;
EXPLAIN (COSTS OFF) SELECT id, k, grp, x FROM bs WHERE k BETWEEN 1000 AND 3000 AND c200 = 17 ORDER BY k;
EXPLAIN (COSTS OFF) SELECT id, k, grp, x FROM bs WHERE c200 = 17 AND c20 = 3 ORDER BY k DESC LIMIT 10;
EXPLAIN (COSTS OFF) SELECT id, k, grp, x, payload FROM bs WHERE k BETWEEN 1000 AND 3000 AND c200 = 17 ORDER BY k;
EXPLAIN (COSTS OFF) SELECT sum(x), max(x) FROM bs WHERE tags @> ARRAY['t3'];
RESET enable_seqscan; RESET enable_bitmapscan; RESET enable_indexscan;

-- 2. The answers against the ordinary plan: a covered `=` and ranges with a
--    lion filter, IN lists on either side, a name column (a cstring in the
--    btree), DESC with a LIMIT, OFFSET, an empty answer, a LIMIT beyond.
SELECT lion_bs('SELECT id, k, grp, x FROM bs WHERE k BETWEEN 1000 AND 3000 AND c200 = 17');
SELECT lion_bs('SELECT id, k, grp, x FROM bs WHERE k BETWEEN 1000 AND 3000 AND c200 = 17 ORDER BY k', true);
SELECT lion_bs('SELECT id, k, x FROM bs WHERE k = 4711 AND c20 = 7');
SELECT lion_bs('SELECT id, k, x FROM bs WHERE k = 4711 AND c20 = (SELECT c20 FROM bs WHERE k = 4711)');
SELECT lion_bs('SELECT id, k, x, c200 FROM bs WHERE k = ANY (ARRAY[5, 50, 500, 5000, 50000, 99999]) AND c20 IN (3, 4, 5)');
SELECT lion_bs('SELECT id, k, c200, c20 FROM bs WHERE c200 IN (17, 18, 250) AND c20 = 3');
SELECT lion_bs('SELECT id, k, c200, c20 FROM bs WHERE c200 IN (17, 18, 250) AND c20 = 3 ORDER BY k', true);
SELECT lion_bs('SELECT id, k, nm FROM bs WHERE c200 = 17 AND nm = ''n17'' ORDER BY k', true);
SELECT lion_bs('SELECT id, k, nm FROM bs WHERE c200 = 17 AND nm > ''n4'' ORDER BY k DESC LIMIT 20', true);
SELECT lion_bs('SELECT id, k, x FROM bs WHERE c200 = 17 ORDER BY k DESC LIMIT 10', true);
SELECT lion_bs('SELECT id, k, x FROM bs WHERE c200 = 17 AND c20 = 3 ORDER BY k LIMIT 10', true);
SELECT lion_bs('SELECT id, k FROM bs WHERE c200 = 17 ORDER BY k LIMIT 10 OFFSET 20', true);
SELECT lion_bs('SELECT id, k FROM bs WHERE c200 = -1 ORDER BY k LIMIT 10', true);
SELECT lion_bs('SELECT id, k FROM bs WHERE c200 = 17 AND c20 = 3 ORDER BY k LIMIT 100000', true);
SELECT lion_bs('SELECT id, k FROM bs WHERE c200 = 17 AND c20 = 3 AND k > 90000');

-- 3. The aggregate shape: a full walk of the covering btree with no btree
--    qual, only a lion filter - an exact one and a multi-key one that is
--    rechecked - under a GROUP BY and under a plain aggregate.
SELECT lion_bs('SELECT grp, sum(x) FROM bs WHERE tags @> ARRAY[''t3''] GROUP BY grp');
SELECT lion_bs('SELECT grp, sum(x) FROM bs WHERE tags @> ARRAY[''t3'', ''u5''] GROUP BY grp ORDER BY grp', true);
SELECT lion_bs('SELECT sum(x), max(x) FROM bs WHERE tags @> ARRAY[''t3'']');
SELECT lion_bs('SELECT grp, x FROM bs WHERE tags && ARRAY[''u5'', ''u6'']');
SELECT lion_bs('SELECT grp, sum(x) FROM bs WHERE tags @> ARRAY[''t3''] AND grp BETWEEN 10 AND 19 GROUP BY grp ORDER BY grp', true);

-- 4. The counters: on a table VACUUM has left all-visible nothing is read
--    from the heap; a multi-key filter's members are rechecked on the index
--    tuple's values.
SELECT * FROM lion_bs_run('SELECT id, k, x FROM bs WHERE c200 = 17 ORDER BY k LIMIT 10');
SELECT * FROM lion_bs_run('SELECT id, k, x FROM bs WHERE c200 = 17 AND c20 = 3 ORDER BY k DESC');
SELECT * FROM lion_bs_run('SELECT grp, x FROM bs WHERE tags @> ARRAY[''t3'', ''u5'']');
SELECT * FROM lion_bs_run('SELECT sum(x) FROM bs WHERE tags @> ARRAY[''t3'']');

-- 5. Pages not all-visible: rows inserted in this transaction are visited in
--    the heap for their visibility (and counted), and so are updated and
--    deleted rows' pages until VACUUM; after it nothing is.
BEGIN;
INSERT INTO bs
SELECT 100000 + i, 100003 + i, i % 100, i, 17, 3, ARRAY['t3'], true,
	   'n0', 'n', 'x'
  FROM generate_series(1, 40) i;
SELECT lion_bs('SELECT id, k, x FROM bs WHERE c200 = 17 AND k > 100000 ORDER BY k', true);
SELECT * FROM lion_bs_run('SELECT id, k, x FROM bs WHERE c200 = 17 AND k > 100000 ORDER BY k');
COMMIT;
UPDATE bs SET note = 'm' WHERE c200 = 17 AND id % 3 = 0;	-- HOT
UPDATE bs SET x = x + 1 WHERE c200 = 17 AND id % 5 = 0;	-- an INCLUDE column: not HOT
UPDATE bs SET c200 = 16 WHERE c200 = 17 AND id % 7 = 0;	-- a lion column: not HOT
DELETE FROM bs WHERE c200 = 17 AND id % 11 = 0;
SELECT lion_bs('SELECT id, k, x FROM bs WHERE c200 = 17 ORDER BY k', true);
SELECT lion_bs('SELECT id, k, x, c200 FROM bs WHERE c200 IN (16, 17) AND k < 50000');
SELECT lion_bs('SELECT grp, sum(x) FROM bs WHERE tags @> ARRAY[''t3''] GROUP BY grp ORDER BY grp', true);
SELECT * FROM lion_bs_run('SELECT id, k, x FROM bs WHERE c200 = 17 AND k > 100000 ORDER BY k');
VACUUM (FREEZE) bs;
SELECT lion_bs('SELECT id, k, x FROM bs WHERE c200 = 17 ORDER BY k', true);
SELECT lion_bs('SELECT id, k, x, c200 FROM bs WHERE c200 IN (16, 17) AND k < 50000');
SELECT lion_bs('SELECT grp, sum(x) FROM bs WHERE tags @> ARRAY[''t3''] GROUP BY grp ORDER BY grp', true);
SELECT * FROM lion_bs_run('SELECT id, k, x FROM bs WHERE c200 = 17 AND k > 100000 ORDER BY k');
ANALYZE bs;

-- 6. Row locks: a row mark needs the row's ctid, which no index returns, so
--    the walk is heap mode and EvalPlanQual rechecks a heap tuple.
SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_indexscan = off;
EXPLAIN (COSTS OFF) SELECT id, k, x FROM bs WHERE c200 = 17 ORDER BY k LIMIT 5 FOR UPDATE;
RESET enable_seqscan; RESET enable_bitmapscan; RESET enable_indexscan;
SELECT lion_bs('SELECT id, k, x FROM bs WHERE c200 = 17 ORDER BY k LIMIT 5 FOR UPDATE', true);

-- 7. Cursors, fetched in pieces, in both modes.
SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_indexscan = off;
EXPLAIN (COSTS OFF) DECLARE bs_c CURSOR FOR SELECT id, k, x FROM bs WHERE c200 = 17 ORDER BY k LIMIT 8;
EXPLAIN (COSTS OFF) DECLARE bs_h CURSOR FOR SELECT id, k, left(payload, 8) FROM bs WHERE c200 = 17 ORDER BY k LIMIT 8;
BEGIN;
DECLARE bs_c CURSOR FOR SELECT id, k, x FROM bs WHERE c200 = 17 ORDER BY k LIMIT 8;
DECLARE bs_h CURSOR FOR SELECT id, k, left(payload, 8) FROM bs WHERE c200 = 17 ORDER BY k LIMIT 8;
FETCH 3 FROM bs_c;
FETCH 3 FROM bs_h;
FETCH 2 FROM bs_c;
FETCH ALL FROM bs_c;
FETCH ALL FROM bs_h;
COMMIT;
RESET enable_seqscan; RESET enable_bitmapscan; RESET enable_indexscan;

-- 8. Params: a generic plan; a LATERAL subquery rescanned with the outer
--    row's value in the lion filter (the set is rebuilt per outer row) and
--    in the btree's quals (it is not); a correlated subquery without an
--    ORDER BY, rescanned the same way.
SET plan_cache_mode = force_generic_plan;
PREPARE bs_p(int, int) AS SELECT id, k, x FROM bs WHERE c200 = $1 AND k > $2 ORDER BY k LIMIT 5;
SELECT lion_bs('EXECUTE bs_p(17, 50000)', true,
			   'SELECT id, k, x FROM bs WHERE c200 = 17 AND k > 50000 ORDER BY k LIMIT 5');
SELECT lion_bs('EXECUTE bs_p(18, 0)', true,
			   'SELECT id, k, x FROM bs WHERE c200 = 18 AND k > 0 ORDER BY k LIMIT 5');
DEALLOCATE bs_p;
RESET plan_cache_mode;
SELECT lion_bs('SELECT o.v, x.id, x.k FROM (VALUES (17), (18), (250), (3)) o(v),
				LATERAL (SELECT id, k FROM bs WHERE c200 = o.v ORDER BY k LIMIT 3) x', true);
SELECT lion_bs('SELECT o.v, x.id, x.k FROM (VALUES (100), (50000), (99990)) o(v),
				LATERAL (SELECT id, k FROM bs WHERE c200 = 17 AND k > o.v ORDER BY k LIMIT 3) x', true);
SELECT lion_bs('SELECT o.v, (SELECT sum(x) FROM bs WHERE c200 = o.v AND k < 20000)
				FROM (VALUES (17), (18), (250)) o(v)');
SELECT * FROM lion_bs_run('SELECT o.v, x.id FROM (VALUES (17), (18), (250), (3)) o(v),
				LATERAL (SELECT id FROM bs WHERE c200 = o.v ORDER BY k LIMIT 3) x');
SELECT * FROM lion_bs_run('SELECT o.v, x.id FROM (VALUES (100), (50000), (99990)) o(v),
				LATERAL (SELECT id FROM bs WHERE c200 = 17 AND k > o.v ORDER BY k LIMIT 3) x');

-- 9. A partial btree: its predicate's clause stays in the filter, so the
--    predicate column must be covered too - it is in bs_part and not in
--    bs_part_nf, whose walk is heap mode.
SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_indexscan = off;
EXPLAIN (COSTS OFF) SELECT id, k, x FROM bs WHERE flag AND c200 = 17 ORDER BY k LIMIT 10;
EXPLAIN (COSTS OFF) SELECT id, x FROM bs WHERE flag AND c200 = 17 ORDER BY x LIMIT 10;
RESET enable_seqscan; RESET enable_bitmapscan; RESET enable_indexscan;
SELECT lion_bs('SELECT id, k, x FROM bs WHERE flag AND c200 = 17 ORDER BY k LIMIT 10', true);
SELECT lion_bs('SELECT id, x FROM bs WHERE flag AND c200 = 17 ORDER BY x LIMIT 10', true);
SELECT lion_bs('SELECT id, x FROM bs WHERE flag AND c200 = 17 AND x < 100');

-- 10. An expression btree gives the order and no value: heap mode.
SELECT lion_bs('SELECT id, k % 1000 FROM bs WHERE c200 = 17 ORDER BY k % 1000, id LIMIT 10', true);

-- 11. The settings: without index-only scans there is no index-only mode
--     and no unordered walk; pg_lion.enable_btree_scan gates the unordered
--     walks alone, pg_lion.enable_ordered_scan the claimed orders alone.
SET enable_indexonlyscan = off;
SELECT lion_bs('SELECT id, k, x FROM bs WHERE c200 = 17 ORDER BY k LIMIT 10', true);
SELECT lion_bs('SELECT id, k, x FROM bs WHERE k BETWEEN 1000 AND 9000 AND c200 = 17');
RESET enable_indexonlyscan;
SET pg_lion.enable_btree_scan = off;
SELECT lion_bs('SELECT id, k, x FROM bs WHERE k BETWEEN 1000 AND 9000 AND c200 = 17');
SELECT lion_bs('SELECT id, k, x FROM bs WHERE c200 = 17 ORDER BY k LIMIT 10', true);
RESET pg_lion.enable_btree_scan;
SET pg_lion.enable_ordered_scan = off;
SELECT lion_bs('SELECT id, k, x FROM bs WHERE k BETWEEN 1000 AND 9000 AND c200 = 17');
SELECT lion_bs('SELECT id, k, x FROM bs WHERE c200 = 17 ORDER BY k LIMIT 10', true);
SELECT lion_bs('SELECT grp, sum(x) FROM bs WHERE tags @> ARRAY[''t3''] GROUP BY grp');
RESET pg_lion.enable_ordered_scan;

-- 12. The plan choice, with nothing disabled: the index-only node for a lion
--     filter under ORDER BY a btree column LIMIT 10, the heap-mode node when
--     a column is not covered, the bitmap scan and Sort when the filter is so
--     selective that the walk to a tenth member is long; the aggregate shape
--     through the covering walk when the covering btree is narrow against
--     the heap, through the bitmap scan when it is wide (bs_k) and through
--     the lion index's own scan when the filter is selective; a covered
--     btree qual with a lion filter.
SELECT * FROM lion_bs_plan('SELECT id, k, x FROM bs WHERE c200 = 17 ORDER BY k LIMIT 10');
SELECT * FROM lion_bs_plan('SELECT id, k, x, payload FROM bs WHERE c200 = 17 ORDER BY k LIMIT 10');
SELECT * FROM lion_bs_plan('SELECT id, k, x FROM bs WHERE c200 = 17 AND c20 = 3 ORDER BY k LIMIT 10');
SELECT * FROM lion_bs_plan('SELECT grp, sum(x) FROM bs WHERE tags @> ARRAY[''t3''] GROUP BY grp');
SELECT * FROM lion_bs_plan('SELECT sum(x) FROM bs WHERE tags @> ARRAY[''t3'']');
SELECT * FROM lion_bs_plan('SELECT sum(x) FROM bs WHERE c20 IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10)');
SELECT * FROM lion_bs_plan('SELECT grp, sum(x) FROM bs WHERE tags @> ARRAY[''t3'', ''u5''] GROUP BY grp');
SELECT * FROM lion_bs_plan('SELECT id, k, x FROM bs WHERE k BETWEEN 1000 AND 9000 AND c200 = 17');

DROP TABLE bs;
DROP FUNCTION lion_bs(text, boolean, text);
DROP FUNCTION lion_bs_plan(text);
DROP FUNCTION lion_bs_run(text);
