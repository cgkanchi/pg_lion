-- Lion-filtered, btree-ordered scans: the LionOrdered CustomScan (DESIGN.md §30).
--
-- SELECT ... WHERE <lion-answerable clauses> ORDER BY <a btree's columns>
-- [LIMIT n]: the node builds lion's exact TID set for the WHERE once, walks
-- the btree in order reading index tuples only, and fetches only the TIDs in
-- the set.  Every query below is run through the node (with every core scan
-- disabled, so that nothing else can be chosen) and through the ordinary plan
-- (the node switched off, everything else enabled), and the two answers are
-- compared ROW BY ROW IN ORDER: every ORDER BY ends in a unique key.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;
SET default_statistics_target = 1000;
SET work_mem = '64MB';

CREATE TABLE lo (
	id int PRIMARY KEY,
	k int,				-- a permutation of the ids, NULL on every 97th row
	k2 int,				-- 7 values
	c200 int, c2 int, c20 int, c1k int,
	nl int,				-- NULL on every 10th row
	tags text[], tsv tsvector,
	note text,			-- indexed by nothing: updating it is HOT
	payload text
) WITH (autovacuum_enabled = off);
INSERT INTO lo
SELECT i,
	   CASE WHEN i % 97 = 0 THEN NULL ELSE (i::bigint * 7919 % 100003)::int END,
	   i % 7,
	   ((hashint8extended(i::bigint, 33) & 9223372036854775807) % 200)::int,
	   ((hashint8extended(i::bigint, 11) & 9223372036854775807) % 2)::int,
	   ((hashint8extended(i::bigint, 22) & 9223372036854775807) % 20)::int,
	   (i * 31) % 1000,
	   CASE WHEN i % 10 = 0 THEN NULL ELSE i % 13 END,
	   ARRAY['t' || (i % 10), 'u' || (i % 37)],
	   to_tsvector('simple', 'w' || (i % 10) || ' x' || (i % 53)),
	   'n',
	   md5(i::text)
  FROM generate_series(1, 100000) i;

-- the ordered side: btrees
CREATE INDEX lo_k_id ON lo (k, id);
CREATE INDEX lo_knf ON lo (k NULLS FIRST, id);
CREATE INDEX lo_k2 ON lo (k2, k, id);
CREATE INDEX lo_expr ON lo ((k % 1000), id);
-- the filter side: lion
CREATE INDEX lo_c200 ON lo USING lion (c200);
CREATE INDEX lo_c2 ON lo USING lion (c2);
CREATE INDEX lo_c20 ON lo USING lion (c20);
CREATE INDEX lo_c1k ON lo USING lion (c1k);
CREATE INDEX lo_nl ON lo USING lion (nl);
CREATE INDEX lo_mc ON lo USING lion (k2, nl);
CREATE INDEX lo_tags ON lo USING lion (tags);
CREATE INDEX lo_tsv ON lo USING lion (tsv);
CREATE INDEX lo_part ON lo USING lion (c1k) WHERE c2 = 0;
VACUUM ANALYZE lo;

/*
 * lion_ord() runs q through the node - every core scan disabled, so that
 * LionOrdered is the only path left that is not - and through the ordinary
 * plan (the node off, every core scan enabled), compares the two answers in
 * order, and says whether the first plan really was the node.  A prepared
 * statement's cached plan does not change with the GUC, so for one the
 * ordinary answer comes from the query it stands for, given as `ordinary`.
 */
CREATE FUNCTION lion_ord(q text, ordinary text DEFAULT NULL) RETURNS text
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
		IF ln ~ 'Custom Scan \(LionOrdered\)' THEN
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
				  CASE WHEN used THEN 'LionOrdered' ELSE 'no LionOrdered' END,
				  coalesce(array_length(a, 1), 0));
END $$;

/*
 * lion_ord_plan() prints the plan's NODES with nothing disabled (the plan
 * choice is the test), one per line, without the lines whose contents vary
 * between majors.
 */
CREATE FUNCTION lion_ord_plan(q text) RETURNS SETOF text
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
 * lion_ord_run() runs q through the node under EXPLAIN ANALYZE and prints the
 * node's own counters and the filter's.
 */
CREATE FUNCTION lion_ord_run(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF ln ~ '(LionOrdered|Index Entries Walked|Lion Set|Heap Fetches|Rows Removed by|Switched)' THEN
			RETURN NEXT btrim(regexp_replace(ln, '\s*\(actual.*\)', ''));
		END IF;
	END LOOP;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
END $$;

-- 1. The plan, as EXPLAIN shows it: the btree it walks, what lion answers.
SET enable_seqscan = off; SET enable_bitmapscan = off;
SET enable_indexscan = off; SET enable_indexonlyscan = off;
SELECT * FROM lion_ord_plan('SELECT id FROM lo WHERE c200 = 17 AND c2 = 1 ORDER BY k, id LIMIT 10');
SELECT * FROM lion_ord_plan('SELECT id FROM lo WHERE c200 = 17 AND k > 50000 AND length(payload) = 32 ORDER BY k DESC, id DESC LIMIT 10');
RESET enable_seqscan; RESET enable_bitmapscan;
RESET enable_indexscan; RESET enable_indexonlyscan;

-- 2. Every kind of lion filter, ORDER BY k [DESC] [NULLS FIRST] LIMIT n.
SELECT lion_ord('SELECT id, k FROM lo WHERE c200 = 17 ORDER BY k, id LIMIT 10');
SELECT lion_ord('SELECT id, k FROM lo WHERE c200 = 17 ORDER BY k DESC, id DESC LIMIT 10');
SELECT lion_ord('SELECT id, k FROM lo WHERE c200 = 17 ORDER BY k NULLS FIRST, id LIMIT 10');
SELECT lion_ord('SELECT id, k FROM lo WHERE c200 = 17 ORDER BY k DESC NULLS LAST, id DESC LIMIT 10');
SELECT lion_ord('SELECT id, k FROM lo WHERE c200 = 17 AND c2 = 1 ORDER BY k, id LIMIT 10');
SELECT lion_ord('SELECT id, k FROM lo WHERE c200 IN (17, 18, 250, NULL) ORDER BY k, id LIMIT 25');
SELECT lion_ord('SELECT id, k FROM lo WHERE c200 = ANY (ARRAY[3, 5, 7]) ORDER BY k DESC, id DESC LIMIT 25');
SELECT lion_ord('SELECT id, k FROM lo WHERE c1k BETWEEN 10 AND 14 ORDER BY k, id LIMIT 25');
SELECT lion_ord('SELECT id, k FROM lo WHERE c1k > 990 AND c2 = 0 ORDER BY k, id LIMIT 25');
SELECT lion_ord('SELECT id, k FROM lo WHERE nl IS NULL AND c20 = 3 ORDER BY k, id LIMIT 25');
SELECT lion_ord('SELECT id, k FROM lo WHERE k2 = 3 AND nl = 5 ORDER BY k, id LIMIT 25');
SELECT lion_ord('SELECT id, k FROM lo WHERE c200 = 17 OR c20 = 3 ORDER BY k, id LIMIT 25');
SELECT lion_ord('SELECT id, k FROM lo WHERE (c200 = 17 OR c1k = 5) AND c2 = 1 ORDER BY k DESC, id DESC LIMIT 25');
SELECT lion_ord($$SELECT id, k FROM lo WHERE tags @> ARRAY['t3', 'u5'] ORDER BY k, id LIMIT 25$$);
SELECT lion_ord($$SELECT id, k FROM lo WHERE tags && ARRAY['u5', 'u6'] AND c200 < 20 ORDER BY k, id LIMIT 25$$);
SELECT lion_ord($$SELECT id, k FROM lo WHERE tsv @@ 'w3 & x5'::tsquery ORDER BY k, id LIMIT 25$$);
SELECT lion_ord('SELECT id, k FROM lo WHERE c1k = 17 AND c2 = 0 ORDER BY k, id LIMIT 25');
-- a long IN list, located a batch at a time (DESIGN.md §29.4)
SET work_mem = '1MB';
SELECT lion_ord('SELECT id, k FROM lo WHERE c1k IN (' ||
				(SELECT string_agg((i * 7)::text, ',') FROM generate_series(1, 60) i) ||
				') ORDER BY k, id LIMIT 40');
RESET work_mem;

-- 3. Multi-column ORDER BY, an expression index, OFFSET/LIMIT.
SELECT lion_ord('SELECT id, k2, k FROM lo WHERE c200 = 17 ORDER BY k2, k, id LIMIT 30');
SELECT lion_ord('SELECT id, k2, k FROM lo WHERE c200 = 17 ORDER BY k2 DESC, k DESC, id DESC LIMIT 30');
SELECT lion_ord('SELECT id, k % 1000 FROM lo WHERE c200 = 17 ORDER BY k % 1000, id LIMIT 30');
SELECT lion_ord('SELECT id, k FROM lo WHERE c200 = 17 ORDER BY k, id OFFSET 40 LIMIT 10');
SELECT lion_ord('SELECT id, k FROM lo WHERE c200 = 17 ORDER BY k DESC, id DESC OFFSET 495 LIMIT 10');

-- 4. No LIMIT; a LIMIT beyond the match count; an empty answer.
SELECT lion_ord('SELECT id, k FROM lo WHERE c200 = 17 ORDER BY k, id');
SELECT lion_ord('SELECT id, k FROM lo WHERE c200 = 17 AND c2 = 1 ORDER BY k, id LIMIT 100000');
SELECT lion_ord('SELECT id, k FROM lo WHERE c200 = -1 ORDER BY k, id LIMIT 10');
SELECT lion_ord('SELECT id, k FROM lo WHERE c200 = 17 AND c2 = 5 ORDER BY k, id LIMIT 10');

-- 5. Filters on the btree column itself, a lion index on the order column,
--    and a residual filter.
SELECT lion_ord('SELECT id, k FROM lo WHERE c200 = 17 AND k > 50000 ORDER BY k, id LIMIT 10');
SELECT lion_ord('SELECT id, k FROM lo WHERE c200 = 17 AND k BETWEEN 1000 AND 30000 ORDER BY k DESC, id DESC LIMIT 10');
SELECT lion_ord('SELECT id, k FROM lo WHERE c200 = 17 AND k IS NULL ORDER BY k, id LIMIT 10');
SELECT lion_ord('SELECT id, k2, k FROM lo WHERE k2 IN (2, 5) AND c200 = 17 ORDER BY k2, k, id LIMIT 30');
SELECT lion_ord('SELECT id, k FROM lo WHERE c200 = 17 AND payload > ''8'' ORDER BY k, id LIMIT 10');
SELECT lion_ord('SELECT id, k FROM lo WHERE c200 = 17 AND id % 3 = 0 AND k < 90000 ORDER BY k, id LIMIT 10');
-- row locks above the node (EvalPlanQual has nothing to recheck here)
SELECT lion_ord('SELECT id, k FROM lo WHERE c200 = 17 ORDER BY k, id LIMIT 4 FOR UPDATE');

-- 6. Params: a generic plan, and a LATERAL subquery rescanned with the outer
--    row's value - in the lion filter (the set is rebuilt per outer row) and
--    in the btree's quals only (it is not).
SET plan_cache_mode = force_generic_plan;
PREPARE lo_p(int, int) AS SELECT id, k FROM lo WHERE c200 = $1 AND k > $2 ORDER BY k, id LIMIT 5;
SELECT lion_ord('EXECUTE lo_p(17, 50000)',
				'SELECT id, k FROM lo WHERE c200 = 17 AND k > 50000 ORDER BY k, id LIMIT 5');
SELECT lion_ord('EXECUTE lo_p(18, 0)',
				'SELECT id, k FROM lo WHERE c200 = 18 AND k > 0 ORDER BY k, id LIMIT 5');
SELECT lion_ord('EXECUTE lo_p(NULL, 0)',
				'SELECT id, k FROM lo WHERE c200 = NULL AND k > 0 ORDER BY k, id LIMIT 5');
DEALLOCATE lo_p;
PREPARE lo_q(int[]) AS SELECT id, k FROM lo WHERE c200 = ANY ($1) ORDER BY k DESC, id DESC LIMIT 5;
SELECT lion_ord('EXECUTE lo_q(ARRAY[17, 18])',
				'SELECT id, k FROM lo WHERE c200 = ANY (ARRAY[17, 18]) ORDER BY k DESC, id DESC LIMIT 5');
DEALLOCATE lo_q;
RESET plan_cache_mode;
SELECT lion_ord('SELECT o.v, x.id, x.k FROM (VALUES (17), (18), (250), (3)) o(v),
				 LATERAL (SELECT id, k FROM lo WHERE c200 = o.v ORDER BY k, id LIMIT 3) x');
SELECT lion_ord('SELECT o.v, x.id, x.k FROM (VALUES (100), (50000), (99990)) o(v),
				 LATERAL (SELECT id, k FROM lo WHERE c200 = 17 AND k > o.v ORDER BY k, id LIMIT 3) x');
SELECT * FROM lion_ord_run('SELECT o.v, x.id FROM (VALUES (17), (18), (250), (3)) o(v),
				 LATERAL (SELECT id FROM lo WHERE c200 = o.v ORDER BY k, id LIMIT 3) x');
SELECT * FROM lion_ord_run('SELECT o.v, x.id FROM (VALUES (100), (50000), (99990)) o(v),
				 LATERAL (SELECT id FROM lo WHERE c200 = 17 AND k > o.v ORDER BY k, id LIMIT 3) x');

-- 7. The counters: entries walked, members, heap fetches; an exact set is
--    never rechecked, a multi-key one always is.
SELECT * FROM lion_ord_run('SELECT id FROM lo WHERE c200 = 17 AND c2 = 1 ORDER BY k, id LIMIT 10');
SELECT * FROM lion_ord_run('SELECT id FROM lo WHERE c200 = 17 AND length(note) = 5 ORDER BY k, id LIMIT 10');
SELECT * FROM lion_ord_run($$SELECT id FROM lo WHERE tags @> ARRAY['t3', 'u5'] ORDER BY k, id LIMIT 10$$);

-- 8. Cursors: forward, a SCROLL cursor (a Material under it), NO SCROLL.
SET enable_seqscan = off; SET enable_bitmapscan = off;
SET enable_indexscan = off; SET enable_indexonlyscan = off;
EXPLAIN (COSTS OFF) DECLARE lo_c SCROLL CURSOR FOR SELECT id, k FROM lo WHERE c200 = 17 ORDER BY k, id LIMIT 8;
BEGIN;
DECLARE lo_c CURSOR FOR SELECT id, k FROM lo WHERE c200 = 17 ORDER BY k, id LIMIT 8;
FETCH 3 FROM lo_c;
FETCH 2 FROM lo_c;
FETCH ALL FROM lo_c;
FETCH BACKWARD 1 FROM lo_c;
ROLLBACK;
BEGIN;
DECLARE lo_c SCROLL CURSOR FOR SELECT id, k FROM lo WHERE c200 = 17 ORDER BY k, id LIMIT 8;
FETCH 4 FROM lo_c;
FETCH BACKWARD 2 FROM lo_c;
FETCH ABSOLUTE 7 FROM lo_c;
FETCH ALL FROM lo_c;
FETCH FIRST FROM lo_c;
COMMIT;
RESET enable_seqscan; RESET enable_bitmapscan;
RESET enable_indexscan; RESET enable_indexonlyscan;
SELECT lion_ord('SELECT id, k FROM lo WHERE c200 = 17 ORDER BY k, id LIMIT 8');

-- 9. A dirty heap - lion-indexed columns updated (not HOT), the order column
--    updated, HOT updates, deletes - and the same after VACUUM.
UPDATE lo SET c200 = 17 WHERE id % 1000 = 1;
UPDATE lo SET c200 = 16 WHERE c200 = 17 AND id % 5 = 0;
UPDATE lo SET k = -k WHERE c200 = 17 AND id % 7 = 0;
UPDATE lo SET note = 'm' WHERE c200 = 17 AND id % 3 = 0;
UPDATE lo SET note = 'o' WHERE c200 = 17 AND id % 3 = 0;
DELETE FROM lo WHERE c200 = 17 AND id % 11 = 0;
SELECT lion_ord('SELECT id, k, note FROM lo WHERE c200 = 17 ORDER BY k, id LIMIT 50');
SELECT lion_ord('SELECT id, k, note FROM lo WHERE c200 = 17 ORDER BY k DESC, id DESC');
SELECT lion_ord('SELECT id, k FROM lo WHERE c200 = 16 AND c2 = 0 ORDER BY k, id LIMIT 50');
SELECT * FROM lion_ord_run('SELECT id FROM lo WHERE c200 = 17 ORDER BY k, id LIMIT 50');
VACUUM lo;
SELECT lion_ord('SELECT id, k, note FROM lo WHERE c200 = 17 ORDER BY k, id LIMIT 50');
SELECT lion_ord('SELECT id, k, note FROM lo WHERE c200 = 17 ORDER BY k DESC, id DESC');
SELECT lion_ord('SELECT id, k FROM lo WHERE c200 = 16 AND c2 = 0 ORDER BY k, id LIMIT 50');
SELECT * FROM lion_ord_run('SELECT id FROM lo WHERE c200 = 17 ORDER BY k, id LIMIT 50');
ANALYZE lo;

-- 10. A set that outgrows hash_mem at run time degrades to container keys
--     and rechecks every member: planned at 64MB (a generic plan keeps it),
--     run at 64kB.
SET plan_cache_mode = force_generic_plan;
PREPARE lo_big(int) AS SELECT id, k FROM lo WHERE c2 = $1 ORDER BY k, id LIMIT 20;
SET enable_seqscan = off; SET enable_bitmapscan = off;
SET enable_indexscan = off; SET enable_indexonlyscan = off;
EXPLAIN (COSTS OFF) EXECUTE lo_big(1);
RESET enable_seqscan; RESET enable_bitmapscan;
RESET enable_indexscan; RESET enable_indexonlyscan;
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SELECT * FROM lion_ord_run('EXECUTE lo_big(1)');
SELECT lion_ord('EXECUTE lo_big(1)',
				'SELECT id, k FROM lo WHERE c2 = 1 ORDER BY k, id LIMIT 20');
RESET hash_mem_multiplier;
RESET work_mem;
DEALLOCATE lo_big;
RESET plan_cache_mode;

-- 11. The plan choice, with nothing disabled: the node for a selective lion
--     filter under ORDER BY a btree column LIMIT 10; core's ordered btree
--     walk when the filter is unselective; no node for a large answer and
--     no LIMIT.
SELECT * FROM lion_ord_plan('SELECT id, payload FROM lo WHERE c200 = 17 AND c2 = 1 ORDER BY k, id LIMIT 10');
SELECT * FROM lion_ord_plan('SELECT id, payload FROM lo WHERE c200 = 17 AND c2 = 1 ORDER BY k DESC, id DESC LIMIT 10');
SELECT * FROM lion_ord_plan('SELECT id, payload FROM lo WHERE c2 = 1 ORDER BY k, id LIMIT 10');
SELECT * FROM lion_ord_plan('SELECT id, payload FROM lo WHERE c2 = 1 ORDER BY k, id');
SELECT * FROM lion_ord_plan('SELECT id, payload FROM lo WHERE c20 IN (1, 2, 3) ORDER BY k, id');

-- 12. Declines: the GUC off, no clause lion answers, a TABLESAMPLE, RLS.
SET enable_seqscan = off; SET enable_bitmapscan = off;
SET enable_indexscan = off; SET enable_indexonlyscan = off;
SET pg_lion.enable_ordered_scan = off;
SELECT * FROM lion_ord_plan('SELECT id FROM lo WHERE c200 = 17 ORDER BY k, id LIMIT 10');
RESET pg_lion.enable_ordered_scan;
SELECT * FROM lion_ord_plan('SELECT id FROM lo WHERE length(payload) = 32 ORDER BY k, id LIMIT 10');
SELECT * FROM lion_ord_plan('SELECT id FROM lo TABLESAMPLE SYSTEM (50) REPEATABLE (1) WHERE c200 = 17 ORDER BY k, id LIMIT 10');
RESET enable_seqscan; RESET enable_bitmapscan;
RESET enable_indexscan; RESET enable_indexonlyscan;

CREATE ROLE lion_ord_user;
CREATE TABLE lo_rls (id int PRIMARY KEY, k int, c int, tenant text);
INSERT INTO lo_rls SELECT i, (i * 37) % 5003, i % 50, 't' || (i % 2) FROM generate_series(1, 5000) i;
CREATE INDEX lo_rls_k ON lo_rls (k, id);
CREATE INDEX lo_rls_c ON lo_rls USING lion (c);
ANALYZE lo_rls;
GRANT SELECT ON lo_rls, lo TO lion_ord_user;
ALTER TABLE lo_rls ENABLE ROW LEVEL SECURITY;
CREATE POLICY lo_rls_t1 ON lo_rls FOR SELECT TO lion_ord_user USING (tenant = 't1');
SET ROLE lion_ord_user;
SET enable_seqscan = off; SET enable_bitmapscan = off;
SET enable_indexscan = off; SET enable_indexonlyscan = off;
SELECT * FROM lion_ord_plan('SELECT id FROM lo_rls WHERE c = 7 ORDER BY k, id LIMIT 5');
SELECT id, tenant FROM lo_rls WHERE c = 7 ORDER BY k, id LIMIT 5;
RESET enable_seqscan; RESET enable_bitmapscan;
RESET enable_indexscan; RESET enable_indexonlyscan;
RESET ROLE;
-- the owner is not subject to the policy, so the node plans for it
SELECT lion_ord('SELECT id, tenant FROM lo_rls WHERE c = 7 ORDER BY k, id LIMIT 5');

-- 13. EXECUTE on what the node evaluates (DESIGN.md §30.6): a revoked
--     equality is refused by the node exactly as by the ordinary plan, and
--     so is a revoked comparison in the btree's own quals.
CREATE FUNCTION lion_ord_try(q text, ordered boolean) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	r text;
	ln text;
	used boolean := false;
BEGIN
	PERFORM set_config('pg_lion.enable_ordered_scan', ordered::text, true);
	IF ordered THEN
		PERFORM set_config('enable_seqscan', 'off', true);
		PERFORM set_config('enable_bitmapscan', 'off', true);
		PERFORM set_config('enable_indexscan', 'off', true);
		PERFORM set_config('enable_indexonlyscan', 'off', true);
	END IF;
	BEGIN
		EXECUTE format('SELECT string_agg(s::text, '' '') FROM (%s) s', q) INTO r;
	EXCEPTION WHEN insufficient_privilege THEN
		r := 'ERROR: ' || SQLERRM;
	END;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_ordered_scan', 'on', true);
	RETURN r;
END $$;
REVOKE EXECUTE ON FUNCTION int4eq(int4, int4) FROM PUBLIC;
SET ROLE lion_ord_user;
SELECT lion_ord_try('SELECT id FROM lo WHERE c200 = 17 ORDER BY k, id LIMIT 3', true);
SELECT lion_ord_try('SELECT id FROM lo WHERE c200 = 17 ORDER BY k, id LIMIT 3', false);
RESET ROLE;
GRANT EXECUTE ON FUNCTION int4eq(int4, int4) TO PUBLIC;
REVOKE EXECUTE ON FUNCTION int4gt(int4, int4) FROM PUBLIC;
SET ROLE lion_ord_user;
SELECT lion_ord_try('SELECT id FROM lo WHERE c2 = 1 AND c200 = 17 AND k > 99000 ORDER BY k, id LIMIT 3', true);
SELECT lion_ord_try('SELECT id FROM lo WHERE c2 = 1 AND c200 = 17 AND k > 99000 ORDER BY k, id LIMIT 3', false);
RESET ROLE;
GRANT EXECUTE ON FUNCTION int4gt(int4, int4) TO PUBLIC;
SET ROLE lion_ord_user;
SELECT lion_ord_try('SELECT id FROM lo WHERE c2 = 1 AND c200 = 17 AND k > 99000 ORDER BY k, id LIMIT 3', true);
RESET ROLE;
-- nothing is left revoked
SELECT p.proname FROM pg_proc p
 WHERE p.proname IN ('int4eq', 'int4gt')
   AND NOT has_function_privilege('lion_ord_user', p.oid, 'EXECUTE');

-- 14. A restriction clause that one ARM of a BitmapOr reuses is still a
--     filter: the lion condition ((a = 3 AND b = 5) OR c = 7) does not imply
--     a = 3 (2026-09-25 review; wrong at default settings).
CREATE TABLE orb (id int, k int, a int, b int, c int);
INSERT INTO orb SELECT i, i, i % 7, i % 11, i % 13 FROM generate_series(1, 20000) i;
CREATE INDEX orb_k ON orb (k);
CREATE INDEX orb_ab ON orb USING lion (a, b);
CREATE INDEX orb_c ON orb USING lion (c);
VACUUM ANALYZE orb;
SELECT * FROM lion_ord_plan('SELECT * FROM orb WHERE a = 3 AND (b = 5 OR c = 7) ORDER BY k LIMIT 50');
SELECT count(*) FILTER (WHERE a <> 3) AS wrong, count(*)
  FROM (SELECT * FROM orb WHERE a = 3 AND (b = 5 OR c = 7) ORDER BY k LIMIT 50) s;
SELECT lion_ord('SELECT id, k FROM orb WHERE a = 3 AND (b = 5 OR c = 7) ORDER BY k LIMIT 50');
SELECT lion_ord('SELECT id, k FROM orb WHERE a = 3 AND (b = 5 OR c = 7) ORDER BY k DESC');
DROP TABLE orb;
DROP INDEX lo_k2;
SELECT lion_ord('SELECT id, k FROM lo WHERE k2 = 3 AND (nl = 5 OR c1k = 7) ORDER BY k, id');

-- 15. A filter correlated with the order (DESIGN.md §30.3): the members lie
--     at the far end of the btree.  The walk switches to fetching the
--     members it has not met and sorting them once it has walked as much as
--     that would cost, rows it already returned included exactly once.
CREATE TABLE lcor (id int PRIMARY KEY, k int, k3 int, c int, h int, h2 int, g int);
INSERT INTO lcor
SELECT i, i, CASE WHEN i % 50 = 0 THEN NULL ELSE i END, (i - 1) / 1000,
	   CASE WHEN i IN (5, 10, 15) OR i BETWEEN 190001 AND 191000 THEN 1 ELSE 0 END,
	   CASE WHEN i IN (199990, 199995) OR i BETWEEN 1001 AND 2000 THEN 1 ELSE 0 END,
	   ((i - 1) / 1000) % 2
  FROM generate_series(1, 200000) i;
CREATE INDEX lcor_k ON lcor (k);
CREATE INDEX lcor_k3 ON lcor (k3, id);
CREATE INDEX lcor_c ON lcor USING lion (c);
CREATE INDEX lcor_h ON lcor USING lion (h);
CREATE INDEX lcor_h2 ON lcor USING lion (h2);
VACUUM ANALYZE lcor;
-- whatever the planner picks (it cannot see the correlation), forced here
SELECT * FROM lion_ord_plan('SELECT id FROM lcor WHERE c = 190 ORDER BY k LIMIT 10');
SELECT lion_ord('SELECT id, k FROM lcor WHERE c = 190 ORDER BY k LIMIT 10');
SELECT * FROM lion_ord_run('SELECT id FROM lcor WHERE c = 190 ORDER BY k LIMIT 10');
-- no row at all: every c = 190 row has g = 0
SELECT lion_ord('SELECT id, k FROM lcor WHERE c = 190 AND g = 1 ORDER BY k LIMIT 10');
SELECT * FROM lion_ord_run('SELECT id FROM lcor WHERE c = 190 AND g = 1 ORDER BY k LIMIT 10');
-- three members early, the rest late: the switch comes after rows were returned
SELECT lion_ord('SELECT id, k FROM lcor WHERE h = 1 ORDER BY k LIMIT 20');
SELECT lion_ord('SELECT id, k FROM lcor WHERE h = 1 ORDER BY k');
SELECT * FROM lion_ord_run('SELECT id FROM lcor WHERE h = 1 ORDER BY k LIMIT 20');
SELECT lion_ord('SELECT id, k FROM lcor WHERE h = 1 AND k > 12 ORDER BY k LIMIT 20');
-- NULLs first (DESC), nulls among the members met before the switch
SELECT lion_ord('SELECT id, k3 FROM lcor WHERE h2 = 1 ORDER BY k3 DESC, id DESC LIMIT 40');
SELECT lion_ord('SELECT id, k3 FROM lcor WHERE h2 = 1 ORDER BY k3 DESC NULLS FIRST, id DESC');
SELECT * FROM lion_ord_run('SELECT id FROM lcor WHERE h2 = 1 ORDER BY k3 DESC, id DESC LIMIT 40');
-- a paused cursor that switches, and a rescan
BEGIN;
SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off;
SET LOCAL enable_indexscan = off; SET LOCAL enable_indexonlyscan = off;
DECLARE lcor_cur CURSOR FOR SELECT id FROM lcor WHERE h = 1 ORDER BY k;
FETCH 4 FROM lcor_cur;
FETCH 2 FROM lcor_cur;
MOVE FORWARD 990 IN lcor_cur;
FETCH ALL FROM lcor_cur;
COMMIT;
SELECT lion_ord('SELECT o.v, x.id FROM (VALUES (1), (190)) o(v),
				 LATERAL (SELECT id FROM lcor WHERE c = o.v ORDER BY k LIMIT 3) x');
DROP TABLE lcor;

DROP FUNCTION lion_ord_try(text, boolean);
DROP TABLE lo_rls;
REVOKE SELECT ON lo FROM lion_ord_user;
DROP ROLE lion_ord_user;
DROP FUNCTION lion_ord(text, text);
DROP FUNCTION lion_ord_plan(text);
DROP FUNCTION lion_ord_run(text);
DROP TABLE lo;
