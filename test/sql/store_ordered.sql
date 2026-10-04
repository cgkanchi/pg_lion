-- ORDER BY a stored column LIMIT n under a lion WHERE: LionOrdered's store
-- order (DESIGN.md §40, "The custom shapes", 2).
--
-- SELECT ... WHERE <lion-answerable clauses> ORDER BY c LIMIT n, where c is
-- a column a lion index's window store holds (an INCLUDE column, or a key
-- column under store_values): the node builds the WHERE's set a window at a
-- time, ranks every row of it by c - the store's value on an all-visible
-- heap page while the container page the row came from is pinned, the
-- heap's value otherwise - keeps the best limit + offset in a bounded heap,
-- and fetches only those, in order.  No Sort above it, no walk.
--
-- Every answer is compared with the plan the planner makes with
-- pg_lion.enable_ordered_scan off.  Rows that tie may come in any order, so
-- lso_cmp() compares the sequence of the sort keys, and checks that every
-- row the node returned is a distinct row the WHERE selects.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;
SET default_statistics_target = 1000;
SET work_mem = '64MB';
-- LionOrdered is the subject: at this table's few thousand rows a probe,
-- LionStoreScan under a Sort can be priced under it, and that plan is
-- store_gather's
SET pg_lion.enable_store_scan = off;

-- The text column's collation: ICU's English where the build has ICU and the
-- database is UTF8, a copy of "C" otherwise.  Every answer is compared with
-- core's under the same collation, so the output is the same either way.
SET client_min_messages = warning;
DO $$
BEGIN
	IF getdatabaseencoding() = 'UTF8' THEN
		CREATE COLLATION lso_coll (provider = icu, locale = 'en');
	END IF;
EXCEPTION WHEN feature_not_supported THEN
	NULL;						-- a build without ICU
END $$;
DO $$
BEGIN
	IF NOT EXISTS (SELECT 1 FROM pg_collation WHERE collname = 'lso_coll') THEN
		CREATE COLLATION lso_coll FROM "C";
	END IF;
END $$;
RESET client_min_messages;

CREATE TABLE lso (
	id int,
	a int,				-- 20 values, and 99 on five more rows
	g int,				-- 7 values
	tags text[],
	c4 int,				-- 500 values, NULL on every 11th row
	c8 bigint,			-- NULL on every 13th row
	d date,				-- NULL on every 17th row
	ts timestamptz,		-- NULL on every 19th row
	t text COLLATE lso_coll,	-- NULL on every 23rd row
	k int,				-- a permutation of the ids, NULL on every 97th row
	pad text
) WITH (autovacuum_enabled = off);
INSERT INTO lso
SELECT i, i % 20, i % 7,
	   ARRAY['t' || (i % 10), 'u' || (i % 37)],
	   CASE WHEN i % 11 = 0 THEN NULL
			ELSE ((hashint8extended(i::bigint, 1) & 9223372036854775807) % 500)::int END,
	   CASE WHEN i % 13 = 0 THEN NULL
			ELSE (hashint8extended(i::bigint, 2) % 1000000000000) END,
	   CASE WHEN i % 17 = 0 THEN NULL
			ELSE date '2000-01-01' +
				 ((hashint8extended(i::bigint, 3) & 9223372036854775807) % 3000)::int END,
	   CASE WHEN i % 19 = 0 THEN NULL
			ELSE timestamptz '2020-01-01 00:00:00+00' +
				 ((hashint8extended(i::bigint, 4) & 9223372036854775807) % 100000000) *
				 interval '1 second' END,
	   CASE WHEN i % 23 = 0 THEN NULL
			ELSE (ARRAY['A', 'a', 'B', 'b', '_a', ' b'])[1 + i % 6] ||
				 ((hashint8extended(i::bigint, 5) & 9223372036854775807) % 300) END,
	   CASE WHEN i % 97 = 0 THEN NULL ELSE (i::bigint * 7919 % 100003)::int END,
	   md5(i::text)
  FROM generate_series(1, 30000) i;
INSERT INTO lso (id, a, g, c4, c8, k, pad)
SELECT i, 99, 0, i - 30000, i, i, 'x' FROM generate_series(30001, 30005) i;

-- (a, k) with every column stored: the key columns under store_values, the
-- others INCLUDE; g with none; tags, a multi-key column, never stored, with
-- c4 INCLUDE.
CREATE INDEX lso_ak ON lso USING lion (a, k) INCLUDE (c4, c8, d, ts, t)
	WITH (store_values = true);
CREATE INDEX lso_g ON lso USING lion (g);
CREATE INDEX lso_tags ON lso USING lion (tags) INCLUDE (c4);
-- a small table to join with
CREATE TABLE lso_g_vals (g int PRIMARY KEY) WITH (autovacuum_enabled = off);
INSERT INTO lso_g_vals VALUES (1), (2);
VACUUM (FREEZE, ANALYZE) lso;

/*
 * lso_cmp() runs SELECT keys FROM tbl WHERE cond ORDER BY ord through the
 * node - every core scan off, so that the store order is what is left - and
 * through the ordinary plan (pg_lion.enable_ordered_scan off, every core
 * scan on), and compares the sequences of the sort keys; it then checks that
 * the node's rows are distinct rows of tbl that cond selects, and says
 * whether the first plan was the store order.
 */
CREATE FUNCTION lso_cmp(tbl text, cond text, ord text, keys text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	q text := format('SELECT tableoid::regclass::text || ctid::text AS lso_row, row(%s)::text AS lso_keys FROM %s WHERE %s ORDER BY %s',
					 keys, tbl, cond, ord);
	ln text;
	r record;
	used boolean := false;
	ids text[] := '{}';
	a text[] := '{}';
	b text[] := '{}';
	n bigint;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('enable_tidscan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ '^\s*Store: ' THEN
			used := true;
		END IF;
	END LOOP;
	FOR r IN EXECUTE q LOOP
		ids := ids || r.lso_row;
		a := a || r.lso_keys;
	END LOOP;

	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('enable_tidscan', 'on', true);
	PERFORM set_config('pg_lion.enable_ordered_scan', 'off', true);
	FOR r IN EXECUTE q LOOP
		b := b || r.lso_keys;
	END LOOP;
	EXECUTE format('SELECT count(*) FROM %s WHERE (%s) AND tableoid::regclass::text || ctid::text = ANY ($1)',
				   tbl, cond) INTO n USING ids;
	PERFORM set_config('pg_lion.enable_ordered_scan', 'on', true);

	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH: %s rows through the node, %s ordinary; first %s / %s',
					  coalesce(array_length(a, 1), 0), coalesce(array_length(b, 1), 0),
					  a[1], b[1]);
	END IF;
	IF n <> coalesce(array_length(ids, 1), 0) THEN
		RETURN format('WRONG ROWS: %s rows, %s of them distinct rows of the WHERE',
					  coalesce(array_length(ids, 1), 0), n);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN used THEN 'store order' ELSE 'no store order' END,
				  coalesce(array_length(a, 1), 0));
END $$;

/*
 * lso_same() runs q through the node and through the ordinary plan as
 * lso_cmp() does, and compares the two answers row by row: for a q whose
 * answer has one order.  A prepared statement's cached plan does not change
 * with the settings, so for one the ordinary answer comes from the query it
 * stands for, given as `ordinary`.
 */
CREATE FUNCTION lso_same(q text, ordinary text DEFAULT NULL) RETURNS text
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
		IF ln ~ '^\s*Store: ' THEN
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
				  CASE WHEN used THEN 'store order' ELSE 'no store order' END,
				  coalesce(array_length(a, 1), 0));
END $$;

/*
 * lso_plan() prints the plan with the settings as they are, without the
 * lines whose contents vary between majors, and a subplan's name as N (19
 * names it expr_1 where 18 numbered it).
 */
CREATE FUNCTION lso_plan(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln !~ '^\s*(Index Searches|Disabled|Storage|Planning|Execution|Buffers)' THEN
			RETURN NEXT regexp_replace(ln, 'SubPlan \S+', 'SubPlan N');
		END IF;
	END LOOP;
END $$;

/*
 * lso_run() runs q under EXPLAIN ANALYZE, every core scan off, and prints
 * the node's counters: the rows ranked, those the store gave the values of,
 * the heap fetches - the other rows, and the rows returned - and the set.
 * When the values came from both, how many from each depends on the heap's
 * page layout, which differs between majors: both counters then say "some".
 */
CREATE FUNCTION lso_run(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	lines text[] := '{}';
	ranked bigint := NULL;
	stored bigint := NULL;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF ln ~ '(LionOrdered|Store|Order:|Rows Ranked|Heap Fetches|Rows Removed by|Lion Set)' THEN
			ln := btrim(regexp_replace(ln, '\s*\(actual.*\)', ''));
			IF ln ~ '^Rows Ranked: ' THEN
				ranked := substring(ln FROM '(\d+)$')::bigint;
			ELSIF ln ~ '^Store Values: ' THEN
				stored := substring(ln FROM '(\d+)$')::bigint;
			END IF;
			lines := lines || ln;
		END IF;
	END LOOP;
	FOREACH ln IN ARRAY lines LOOP
		IF stored > 0 AND stored < ranked AND ln ~ '^(Store Values|Heap Fetches): ' THEN
			ln := regexp_replace(ln, '\d+$', 'some');
		END IF;
		RETURN NEXT ln;
	END LOOP;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
END $$;

-- 1. The plan, with nothing disabled: the store order under the LIMIT, with
--    no Sort, the columns it ranks by and their order spelled out.
SELECT * FROM lso_plan('SELECT * FROM lso WHERE a = 3 ORDER BY c4 LIMIT 10');
SELECT * FROM lso_plan('SELECT * FROM lso WHERE a = 3 AND g = 2 ORDER BY c4 DESC LIMIT 10 OFFSET 5');
SELECT * FROM lso_plan('SELECT id, t FROM lso WHERE a = 3 ORDER BY t COLLATE "C" DESC NULLS LAST, c8 LIMIT 10');
-- the store's columns: NULLS FIRST and every direction, ORDER BY two of them
SELECT * FROM lso_plan('SELECT id, c8 FROM lso WHERE a = 3 ORDER BY c8 NULLS FIRST LIMIT 10');
SELECT * FROM lso_plan('SELECT id, d, ts FROM lso WHERE a = 3 ORDER BY d DESC NULLS LAST, ts NULLS FIRST LIMIT 10');
-- what the store order is not: a column the store does not hold, first or
-- second; an expression; a WHERE lion does not answer whole, which would
-- leave a filter; FOR UPDATE; WITH TIES; a LIMIT that is a parameter; a
-- join.
SET enable_seqscan = off; SET enable_bitmapscan = off;
SET enable_indexscan = off; SET enable_indexonlyscan = off;
SELECT * FROM lso_plan('SELECT id FROM lso WHERE a = 3 ORDER BY id LIMIT 10');
SELECT * FROM lso_plan('SELECT id FROM lso WHERE a = 3 ORDER BY c4, pad LIMIT 10');
SELECT * FROM lso_plan('SELECT id FROM lso WHERE a = 3 ORDER BY c4 + 1 LIMIT 10');
SELECT * FROM lso_plan('SELECT id FROM lso WHERE a = 3 AND length(pad) = 32 ORDER BY c4 LIMIT 10');
SELECT * FROM lso_plan('SELECT id FROM lso WHERE a = 3 ORDER BY c4 LIMIT 10 FOR UPDATE');
SELECT * FROM lso_plan('SELECT id FROM lso WHERE a = 3 ORDER BY c4 FETCH FIRST 10 ROWS WITH TIES');
SET plan_cache_mode = force_generic_plan;
PREPARE lso_lim(int) AS SELECT id FROM lso WHERE a = 3 ORDER BY c4 LIMIT $1;
SELECT * FROM lso_plan('EXECUTE lso_lim(10)');
DEALLOCATE lso_lim;
RESET plan_cache_mode;
-- (the Sort above the join is the point; the join below it is core's, and
-- its shape differs between majors)
SELECT * FROM lso_plan('SELECT lso.id FROM lso JOIN lso_g_vals v ON v.g = lso.g WHERE a = 3 ORDER BY c4 LIMIT 10') LIMIT 3;
RESET enable_seqscan; RESET enable_bitmapscan;
RESET enable_indexscan; RESET enable_indexonlyscan;

-- 2. Every kind of lion WHERE, ASC and DESC, NULLS FIRST and LAST, OFFSET.
SELECT lso_cmp('lso', 'a = 3', 'c4 LIMIT 10', 'c4');
SELECT lso_cmp('lso', 'a = 3', 'c4 DESC LIMIT 10', 'c4');
SELECT lso_cmp('lso', 'a = 3', 'c4 NULLS FIRST LIMIT 10', 'c4');
SELECT lso_cmp('lso', 'a = 3', 'c4 DESC NULLS LAST LIMIT 10', 'c4');
SELECT lso_cmp('lso', 'a = 3', 'c4 LIMIT 10 OFFSET 40', 'c4');
SELECT lso_cmp('lso', 'a = 3', 'c4 DESC NULLS LAST LIMIT 5 OFFSET 300', 'c4');
SELECT lso_cmp('lso', 'a = 3 AND g = 2', 'c4 LIMIT 10', 'c4');
SELECT lso_cmp('lso', 'a = 3 OR g = 2', 'c4 DESC LIMIT 25', 'c4');
SELECT lso_cmp('lso', $$(a = 3 OR tags @> '{u4}') AND g = 2$$, 'c4 LIMIT 25', 'c4');
SELECT lso_cmp('lso', $$a = 3 AND g = 2 AND tags @> '{t3}'$$, 'c4 DESC LIMIT 5', 'c4');
SELECT lso_cmp('lso', 'a IN (3, 5, 7, NULL)', 'c4 LIMIT 25', 'c4');
SELECT lso_cmp('lso', 'a = ANY (ARRAY[2, 4])', 'c8 DESC LIMIT 25', 'c8');
SELECT lso_cmp('lso', 'a BETWEEN 3 AND 5', 'c4 LIMIT 25', 'c4');
SELECT lso_cmp('lso', 'a = 3 AND k > 50000', 'c4 LIMIT 25', 'c4');
SELECT lso_cmp('lso', 'a = 3 AND k IS NULL', 'c4 NULLS FIRST LIMIT 3', 'c4');
-- ties: 500 values among 4,286 rows; every row with the last value kept is
-- as good as any other, and the node keeps only the first it meets
SELECT lso_cmp('lso', 'g = 3', 'c4 LIMIT 100', 'c4');
SELECT lso_cmp('lso', 'g = 3', 'c4 DESC NULLS FIRST LIMIT 400 OFFSET 100', 'c4');
-- every type the table stores, and a text under its collation and another
SELECT lso_cmp('lso', 'a = 4', 'c8 LIMIT 20', 'c8');
SELECT lso_cmp('lso', 'a = 4', 'c8 DESC NULLS LAST LIMIT 20', 'c8');
SELECT lso_cmp('lso', 'a = 4', 'd LIMIT 20', 'd');
SELECT lso_cmp('lso', 'a = 4', 'd DESC LIMIT 20', 'd');
SELECT lso_cmp('lso', 'a = 4', 'ts NULLS FIRST LIMIT 20', 'ts');
SELECT lso_cmp('lso', 'a = 4', 'ts DESC NULLS LAST LIMIT 20', 'ts');
SELECT lso_cmp('lso', 'a = 4', 't LIMIT 20', 't');
SELECT lso_cmp('lso', 'a = 4', 't DESC LIMIT 20', 't');
SELECT lso_cmp('lso', 'a = 4', 't COLLATE "C" LIMIT 20', 't');
SELECT lso_cmp('lso', 'a = 4', 't COLLATE "C" DESC NULLS LAST LIMIT 20', 't');
-- two keys
SELECT lso_cmp('lso', 'a = 4', 'c4, c8 DESC LIMIT 30', 'c4, c8');
SELECT lso_cmp('lso', 'g = 1', 't DESC, d NULLS FIRST, ts LIMIT 30', 't, d, ts');
-- the multi-key column's WHERE, ranking by tags' INCLUDE column; lion
-- answers a multi-key clause and has it rechecked all the same (§29.6), so
-- its rows are ranked from the heap
SELECT lso_cmp('lso', $$tags @> ARRAY['t3']$$, 'c4 LIMIT 15', 'c4');
SELECT lso_cmp('lso', $$tags && ARRAY['u5', 'u6']$$, 'c4 DESC LIMIT 15', 'c4');
SELECT lso_cmp('lso', $$tags @> ARRAY['t3', 'u5'] OR a = 7$$, 'c4 LIMIT 15', 'c4');
-- a multi-key clause lion cannot answer exactly: every row of the
-- superset its lookup returns is fetched and rechecked
SELECT lso_cmp('lso', $$tags <@ ARRAY['t3', 'u5', 'u6']$$, 'c4 LIMIT 15', 'c4');
-- The counters: every row of a = 3 ranked by the store's value, the ten
-- kept fetched; an OR ranks each side's rows, a row both hold twice; an AND
-- whose other side is rechecked has its rows ranked from the heap, as the
-- recheck needs the row anyway; and so has a superset.
SELECT * FROM lso_run('SELECT id FROM lso WHERE a = 3 ORDER BY c4 LIMIT 10');
SELECT * FROM lso_run('SELECT id FROM lso WHERE a = 3 OR g = 2 ORDER BY c4 LIMIT 10');
SELECT * FROM lso_run($$SELECT id FROM lso WHERE (a = 3 OR tags @> '{u4}') AND g = 2 ORDER BY c4 DESC LIMIT 10$$);
SELECT * FROM lso_run($$SELECT id FROM lso WHERE tags <@ ARRAY['t3', 'u5', 'u6'] ORDER BY c4 LIMIT 10$$);

-- 3. A LIMIT the set has fewer rows than, an empty set: a generic plan,
--    priced for a = $1's average share of the table, run for the value of
--    five rows and for none.
SET plan_cache_mode = force_generic_plan;
PREPARE lso_p(int) AS SELECT id, c4 FROM lso WHERE a = $1 ORDER BY c4 DESC NULLS FIRST LIMIT 40;
SELECT * FROM lso_plan('EXECUTE lso_p(99)');
SELECT lso_same('EXECUTE lso_p(99)',
				'SELECT id, c4 FROM lso WHERE a = 99 ORDER BY c4 DESC NULLS FIRST LIMIT 40');
SELECT lso_same('EXECUTE lso_p(-1)',
				'SELECT id, c4 FROM lso WHERE a = -1 ORDER BY c4 DESC NULLS FIRST LIMIT 40');
EXECUTE lso_p(99);
DEALLOCATE lso_p;
RESET plan_cache_mode;

-- 4. Rescans: a subquery run once for each outer row, its set and its
--    candidates made again for each value of the parameter.
SELECT * FROM lso_plan('SELECT x, (SELECT c4 FROM lso WHERE a = x ORDER BY c4 DESC NULLS LAST LIMIT 1) FROM generate_series(0, 21) x');
SELECT lso_same('SELECT x, (SELECT c4 FROM lso WHERE a = x ORDER BY c4 DESC NULLS LAST LIMIT 1), (SELECT c8 FROM lso WHERE a = x ORDER BY c8 LIMIT 1 OFFSET 2) FROM generate_series(0, 21) x');
SELECT * FROM lso_run('SELECT x, (SELECT c4 FROM lso WHERE a = x ORDER BY c4 DESC NULLS LAST LIMIT 1) FROM generate_series(0, 4) x');

-- a cursor, read forward and back (a Material keeps the rows for that)
BEGIN;
SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off;
SET LOCAL enable_indexscan = off; SET LOCAL enable_indexonlyscan = off;
SELECT * FROM lso_plan('DECLARE lso_cur SCROLL CURSOR FOR SELECT c4 FROM lso WHERE a = 5 ORDER BY c4 DESC NULLS LAST LIMIT 6');
DECLARE lso_cur SCROLL CURSOR FOR SELECT c4 FROM lso WHERE a = 5 ORDER BY c4 DESC NULLS LAST LIMIT 6;
FETCH 4 FROM lso_cur;
FETCH BACKWARD 2 FROM lso_cur;
FETCH ALL FROM lso_cur;
COMMIT;
SET pg_lion.enable_ordered_scan = off;
SELECT c4 FROM lso WHERE a = 5 ORDER BY c4 DESC NULLS LAST LIMIT 6;
RESET pg_lion.enable_ordered_scan;

-- 5. The key column k is stored and is lion's own ordered column too: the
--    store order and the walk of k (DESIGN.md §30.11) are both offered, and
--    the price decides.  For a WHERE that keeps few rows the store order
--    ranks them, where the walk would meet a great many keys to find ten of
--    them; with pg_lion.enable_ordered_store off, the walk is what is left.
--    For a WHERE that keeps many, the walk finds its ten rows at once.
SELECT * FROM lso_plan('SELECT id, k FROM lso WHERE a = 3 AND g = 2 ORDER BY k LIMIT 10');
SET pg_lion.enable_ordered_store = off;
SELECT * FROM lso_plan('SELECT id, k FROM lso WHERE a = 3 AND g = 2 ORDER BY k LIMIT 10');
RESET pg_lion.enable_ordered_store;
SELECT * FROM lso_plan('SELECT id, k FROM lso WHERE a = 3 ORDER BY k DESC LIMIT 3');
SELECT lso_cmp('lso', 'a = 3 AND g = 2', 'k LIMIT 10', 'k');
SELECT lso_cmp('lso', 'a = 3 AND g = 2', 'k DESC LIMIT 10', 'k');
SELECT lso_cmp('lso', 'a = 3 AND g = 4', 'k DESC NULLS LAST LIMIT 10', 'k');
-- a key column under store_values and its WHERE at once
SELECT lso_cmp('lso', 'a IN (3, 4)', 'c4, a DESC LIMIT 10', 'c4, a');

-- 6. A LIMIT the store order refuses: more than a quarter of the rows the
--    set keeps; more candidates than work_mem holds (two thousand texts, not
--    in 64kB; in 64MB, yes).  Core's Sort it is.
SELECT * FROM lso_plan('SELECT * FROM lso WHERE a = 3 ORDER BY c4 LIMIT 1000');
SELECT lso_cmp('lso', 'a = 3', 'c4 LIMIT 1000', 'c4');
SET work_mem = '64kB';
SELECT lso_cmp('lso', 'g IN (1, 2, 3)', 't LIMIT 2000', 't');
SET work_mem = '64MB';
SELECT lso_cmp('lso', 'g IN (1, 2, 3)', 't LIMIT 2000', 't');

-- 7. Dirty pages: rows moved by UPDATEs to new values and new pages, HOT
--    updates, deletes, new rows; none VACUUMed, and all of them in the
--    first two thirds of the heap.  The rows of pages the visibility map no
--    longer calls all-visible are ranked from the heap, which says which of
--    their versions the snapshot sees; the others' from the store.
UPDATE lso SET c4 = c4 + 1000 WHERE id % 50 = 3 AND id <= 8000;
UPDATE lso SET c4 = -c4, t = 'Z' || t WHERE id % 50 = 4 AND a = 4 AND id <= 12000;
UPDATE lso SET pad = pad || 'x' WHERE id % 40 = 7 AND id BETWEEN 8000 AND 16000;
DELETE FROM lso WHERE id % 60 = 13 AND id BETWEEN 12000 AND 20000;
DELETE FROM lso WHERE a = 3 AND c4 < 5;
INSERT INTO lso (id, a, g, c4, c8, d, ts, t, k, pad)
SELECT i, i % 20, i % 7, -1, -i, date '1999-01-01', NULL, 'A', i, 'new'
  FROM generate_series(40001, 40060) i;
SELECT lso_cmp('lso', 'a = 3', 'c4 LIMIT 10', 'c4');
SELECT lso_cmp('lso', 'a = 3', 'c4 DESC LIMIT 10', 'c4');
SELECT lso_cmp('lso', 'a = 3', 'c4 NULLS FIRST LIMIT 10 OFFSET 20', 'c4');
SELECT lso_cmp('lso', 'a = 4', 'c4 LIMIT 30', 'c4');
SELECT lso_cmp('lso', 'a = 4', 't DESC LIMIT 30', 't');
SELECT lso_cmp('lso', 'a = 3 OR g = 2', 'c4 DESC NULLS LAST LIMIT 25', 'c4');
SELECT lso_cmp('lso', $$(a = 3 OR tags @> '{u4}') AND g = 2$$, 'c4 LIMIT 25', 'c4');
SELECT lso_cmp('lso', 'a IN (3, 5, 7)', 'd, c8 DESC LIMIT 25', 'd, c8');
SELECT lso_cmp('lso', $$tags @> ARRAY['t3']$$, 'c4 DESC LIMIT 15', 'c4');
SELECT * FROM lso_run('SELECT id FROM lso WHERE a = 3 ORDER BY c4 LIMIT 10');

-- 8. After VACUUM: every page all-visible again, every row's value from the
--    store.
VACUUM (FREEZE, ANALYZE) lso;
SELECT lso_cmp('lso', 'a = 3', 'c4 LIMIT 10', 'c4');
SELECT lso_cmp('lso', 'a = 3', 'c4 DESC LIMIT 10', 'c4');
SELECT lso_cmp('lso', 'a = 4', 't DESC LIMIT 30', 't');
SELECT lso_cmp('lso', 'a = 3 OR g = 2', 'c4 DESC NULLS LAST LIMIT 25', 'c4');
SELECT lso_cmp('lso', 'a IN (3, 5, 7)', 'd, c8 DESC LIMIT 25', 'd, c8');
SELECT * FROM lso_run('SELECT id FROM lso WHERE a = 3 ORDER BY c4 LIMIT 10');
-- under SERIALIZABLE, the heap pages the store answered for are predicate-
-- locked, as an index-only scan locks the pages it does not visit
BEGIN ISOLATION LEVEL SERIALIZABLE;
SELECT lso_cmp('lso', 'a = 3 OR g = 2', 'c4 DESC LIMIT 10', 'c4');
COMMIT;

-- 9. An ABSENT window: heap pages whose values no store page can hold - a
--    long string the heap compresses inline - are left to the heap (§40);
--    their rows are ranked from it, the others' from the store.
CREATE TABLE lsa (id int, g int, s text, f text)
	WITH (toast_tuple_target = 128, autovacuum_enabled = off);
INSERT INTO lsa SELECT i, i % 5, repeat(md5(i::text), 62), repeat('f', 100)
  FROM generate_series(1, 400) i;
INSERT INTO lsa SELECT i, i % 5, 'short ' || i FROM generate_series(401, 900) i;
CREATE INDEX lsa_g ON lsa USING lion (g) INCLUDE (s);
VACUUM (FREEZE, ANALYZE) lsa;
-- (how many rows the ABSENT pages hold depends on the heap's page layout,
-- which differs between majors)
SELECT count(*) AS rows, count(*) FILTER (WHERE st IS NULL) > 0 AS some_absent,
	   count(*) FILTER (WHERE st IS NOT NULL) > 0 AS some_stored
  FROM (SELECT (lion_index_stored('lsa_g', ctid))[1] AS st FROM lsa WHERE g = 2) x;
SELECT lso_cmp('lsa', 'g = 2', 's LIMIT 5', 'md5(s)');
SELECT lso_cmp('lsa', 'g = 2', 's DESC LIMIT 5', 's');
SELECT lso_cmp('lsa', 'g IN (1, 2)', 's LIMIT 10 OFFSET 50', 'md5(s)');
SELECT * FROM lso_run('SELECT id FROM lsa WHERE g = 2 ORDER BY s LIMIT 5');

-- 10. A partitioned table: each partition's own store order, merged.
CREATE TABLE lsp (id int, a int, c4 int, pad text) PARTITION BY RANGE (id);
CREATE TABLE lsp1 PARTITION OF lsp FOR VALUES FROM (1) TO (10001)
	WITH (autovacuum_enabled = off);
CREATE TABLE lsp2 PARTITION OF lsp FOR VALUES FROM (10001) TO (20001)
	WITH (autovacuum_enabled = off);
INSERT INTO lsp SELECT id, a, c4, pad FROM lso WHERE id <= 20000;
CREATE INDEX lsp_a ON lsp USING lion (a) INCLUDE (c4);
VACUUM (FREEZE, ANALYZE) lsp1;
VACUUM (FREEZE, ANALYZE) lsp2;
ANALYZE lsp;
SELECT * FROM lso_plan('SELECT * FROM lsp WHERE a = 3 ORDER BY c4 LIMIT 10');
SELECT lso_cmp('lsp', 'a = 3', 'c4 LIMIT 10', 'c4');
SELECT lso_cmp('lsp', 'a = 3', 'c4 DESC NULLS LAST LIMIT 10 OFFSET 5', 'c4');

DROP TABLE lso, lso_g_vals, lsa, lsp;
DROP FUNCTION lso_cmp(text, text, text, text), lso_same(text, text),
	lso_plan(text), lso_run(text);
DROP COLLATION lso_coll;
