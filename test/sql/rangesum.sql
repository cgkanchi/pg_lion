-- A range bounding the count pushdown's sum (DESIGN.md §28, "One read per
-- leaf", "Counting a walk" and "The complement").
--
-- `count(*) WHERE <range on k> [AND F]` is the sum over k's entries in the
-- range.  The node answers it from the entries the range selects ("inside"),
-- from the rows of F with a value of k minus the entries it does NOT select
-- ("complement"), or - when the range covers every key k has - from those rows
-- alone ("full domain"), choosing exactly at execution time which side of the
-- range has fewer entries.  Every answer here is checked against a sequential
-- scan with the pushdown off; the way it was evaluated and the entries it
-- summed are printed beside it.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

/*
 * lion_rs() runs q with the pushdown on and every other scan disabled, and
 * again as a sequential scan with the pushdown off, and compares the answers.
 * It reports how the node evaluated the range - EXPLAIN ANALYZE's "Range
 * Evaluation" - and how many entries it summed ("Posting Sets Summed").
 */
CREATE FUNCTION lion_rs(q text) RETURNS text
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
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE 'EXPLAIN (ANALYZE, TIMING OFF, COSTS OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) '
		|| q INTO j;
	node := jsonb_path_query_first(j, 'strict $.** ? (@."Custom Plan Provider" == "LionCount")');
	IF node IS NULL THEN
		how := 'not pushed down';
	ELSE
		how := coalesce(node ->> 'Range Evaluation', 'no range evaluated') ||
			', summed ' || (node ->> 'Posting Sets Summed');
	END IF;
	EXECUTE format('SELECT coalesce(string_agg(s::text, '' '' ORDER BY s::text), ''(none)'') FROM (%s) s', q)
		INTO got;

	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	EXECUTE format('SELECT coalesce(string_agg(s::text, '' '' ORDER BY s::text), ''(none)'') FROM (%s) s', q)
		INTO want;
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	IF got IS DISTINCT FROM want THEN
		RETURN format('MISMATCH: %s, pushed %s, seqscan %s', how, got, want);
	END IF;
	RETURN how || ': ' || got;
END $$;

/* One EXPLAIN ANALYZE property of the LionCount node of q, pushdown forced. */
CREATE FUNCTION lion_rs_prop(q text, prop text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	j jsonb;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	EXECUTE 'EXPLAIN (ANALYZE, TIMING OFF, COSTS OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) '
		|| q INTO j;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	RETURN jsonb_path_query_first(j, 'strict $.** ? (@."Custom Plan Provider" == "LionCount")') ->> prop;
END $$;

-- ---------- the table ----------
-- k: 1000 values, NULL in every 13th row; t: 500 text values, NULL in every
-- 17th; g: 7 values; u: unique, so a leaf holds many of its entries
CREATE TABLE lion_rs (id int NOT NULL, k int, t text, g int NOT NULL,
					  u int NOT NULL, pad text);
INSERT INTO lion_rs
SELECT i,
	   CASE WHEN i % 13 = 0 THEN NULL ELSE (i * 7) % 1000 END,
	   CASE WHEN i % 17 = 0 THEN NULL ELSE 'v' || lpad(((i * 11) % 500)::text, 4, '0') END,
	   i % 7, i, repeat('x', 20)
  FROM generate_series(1, 20000) i;
CREATE INDEX lion_rs_k ON lion_rs USING lion (k);
CREATE INDEX lion_rs_t ON lion_rs USING lion (t);
CREATE INDEX lion_rs_g ON lion_rs USING lion (g);
CREATE INDEX lion_rs_u ON lion_rs USING lion (u);
VACUUM (FREEZE, ANALYZE) lion_rs;

-- ---------- 1. full domain: the range covers every key ----------
-- the NULL rows of k are not counted; nothing is summed at all
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k >= 0 AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k BETWEEN -5 AND 5000 AND g = 3');
SELECT lion_rs('SELECT count(k) FROM lion_rs WHERE k > -1 AND k < 1000 AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k <= 999 AND g = 3');
-- cross-type bounds outside the int4 domain (btint48cmp compares exactly)
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k < 5000000000::int8 AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k >= -5000000000::int8 AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE u >= 0 AND g = 3');
-- text keys
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE t >= ''v0000'' AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE t BETWEEN ''a'' AND ''w'' AND g = 3');
-- without a positive source there is no count of F to take: the whole column
-- is summed, as before
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k >= 0');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k >= 0 AND t IS NOT NULL');

-- ---------- 2. near-full: the keys OUTSIDE the range are summed ----------
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k >= 5 AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k <= 990 AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k BETWEEN 3 AND 995 AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k > 3 AND k < 996 AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE u > 10 AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE u < 19990 AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE t > ''v0003'' AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE t < ''v0496'' AND g = 3');
-- two lower bounds and two upper bounds: each walk starts at the tightest bound
-- of its side (DESIGN.md §28), so the walk above the range is as short as the
-- one below, and the complement is taken
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k > 2 AND k >= 4 AND k < 996 AND k <= 993 AND g = 3');

-- ---------- 3. the inside is smaller: its entries are summed ----------
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k BETWEEN 100 AND 110 AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE u BETWEEN 100 AND 199 AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE t < ''v0010'' AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE u < 100');

-- ---------- 4. empty and inverted ranges ----------
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k > 5 AND k < 5 AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k BETWEEN 9 AND 3 AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k > 100000 AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k < -1 AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE t > ''zzz'' AND g = 3');

-- ---------- 5. beside other clauses ----------
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k >= 5 AND g IN (1, 3, 5)');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k >= 5 AND (g = 1 OR t = ''v0042'')');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k >= 0 AND t IS NULL');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k >= 5 AND t IS NULL');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k >= 5 AND k IS NOT NULL AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k >= 5 AND t IS NOT NULL AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k >= 0 AND t = ''v0042'' AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE u > 10 AND k = 7');
-- the walks that are not sums read each leaf once too, and are unchanged
SELECT lion_rs('SELECT k, count(*) FROM lion_rs WHERE k BETWEEN 10 AND 900 AND g = 3 GROUP BY k HAVING count(*) >= 3 AND k < 60');
SELECT lion_rs('SELECT count(DISTINCT k) FROM lion_rs WHERE k >= 5 AND g = 3');
SELECT lion_rs('SELECT count(DISTINCT u) FROM lion_rs WHERE u > 10 AND g = 3');

-- ---------- 6. one read per leaf ----------
-- a sum over 20000 entries of one row each reads each of u's leaves once,
-- where it used to read a leaf for every entry
SELECT lion_rs_prop('SELECT count(*) FROM lion_rs WHERE u > 0', 'Posting Sets Summed') AS summed,
	   lion_rs_prop('SELECT count(*) FROM lion_rs WHERE u > 0', 'Directory Pages Read')::int
		 <= leaf_pages + 2 * directory_height + 2 AS one_read_per_leaf,
	   leaf_pages > 20 AS several_leaves
  FROM lion_index_stats('lion_rs_u');
-- a full-domain range reads a handful of pages, however many keys it covers
SELECT lion_rs_prop('SELECT count(*) FROM lion_rs WHERE u >= 0 AND g = 3', 'Posting Sets Summed') AS summed,
	   lion_rs_prop('SELECT count(*) FROM lion_rs WHERE u >= 0 AND g = 3', 'Directory Pages Read')::int
		 <= 6 * (directory_height + 1) AS a_few_descents
  FROM lion_index_stats('lion_rs_u');

-- ---------- 7. Params: a generic plan, and a rescan per outer row ----------
SET plan_cache_mode = force_generic_plan;
PREPARE lion_rsp(int, int) AS SELECT count(*) FROM lion_rs WHERE k >= $1 AND g = $2;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
EXPLAIN (COSTS OFF) EXECUTE lion_rsp(0, 3);
EXECUTE lion_rsp(0, 3);
EXECUTE lion_rsp(5, 3);
EXECUTE lion_rsp(900, 3);
EXECUTE lion_rsp(NULL, 3);
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
SET pg_lion.enable_count_pushdown = off;
EXECUTE lion_rsp(0, 3);
EXECUTE lion_rsp(5, 3);
EXECUTE lion_rsp(900, 3);
RESET pg_lion.enable_count_pushdown;
DEALLOCATE lion_rsp;
RESET plan_cache_mode;
SELECT lion_rs('SELECT v, (SELECT count(*) FROM lion_rs WHERE k >= v AND g = 3) FROM (VALUES (-1), (5), (900), (2000)) x(v)');

-- ---------- 8. a dirty heap, new keys, and VACUUM ----------
UPDATE lion_rs SET pad = 'y' WHERE id % 50 = 0;
DELETE FROM lion_rs WHERE id % 97 = 0;
-- a key above every other one, and a row for a key below
INSERT INTO lion_rs VALUES (30001, 5000, 'v9999', 3, 30001, 'z'), (30002, -7, 'a', 3, 30002, 'z');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k >= -7 AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k >= 0 AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k < 1000 AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k BETWEEN 3 AND 995 AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE t < ''v9999'' AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE u > 10 AND g = 3');
-- the new key's rows go away again; its entry stays until VACUUM
DELETE FROM lion_rs WHERE id > 30000;
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k >= 0 AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k < 1000 AND g = 3');
VACUUM lion_rs;
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k >= 0 AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k < 1000 AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rs WHERE k BETWEEN 3 AND 995 AND g = 3');
SELECT lion_index_verify('lion_rs_k', true);
SELECT lion_index_verify('lion_rs_u', true);

-- ---------- 9. a multicolumn index: a range on each of its columns ----------
-- every column's entries are one run of the one directory (DESIGN.md §24), so
-- the walks below and above a range stop at the neighbouring columns' entries
CREATE TABLE lion_rsm (id int NOT NULL, a int NOT NULL, b int, c text);
INSERT INTO lion_rsm
SELECT i, i % 5,
	   CASE WHEN i % 11 = 0 THEN NULL ELSE i % 300 END,
	   'c' || lpad((i % 40)::text, 3, '0')
  FROM generate_series(1, 12000) i;
CREATE INDEX lion_rsm_abc ON lion_rsm USING lion (a, b, c);
VACUUM (FREEZE, ANALYZE) lion_rsm;
SELECT lion_rs('SELECT count(*) FROM lion_rsm WHERE b >= 0 AND a = 2');
SELECT lion_rs('SELECT count(*) FROM lion_rsm WHERE b > 3 AND a = 2');
SELECT lion_rs('SELECT count(*) FROM lion_rsm WHERE b < 296 AND c = ''c007''');
SELECT lion_rs('SELECT count(*) FROM lion_rsm WHERE b BETWEEN 10 AND 20 AND a = 2');
SELECT lion_rs('SELECT count(*) FROM lion_rsm WHERE a >= 0 AND b = 5');
SELECT lion_rs('SELECT count(*) FROM lion_rsm WHERE a >= 1 AND b = 5');
SELECT lion_rs('SELECT count(*) FROM lion_rsm WHERE a <= 3 AND c = ''c011''');
SELECT lion_rs('SELECT count(*) FROM lion_rsm WHERE c >= ''c000'' AND b = 7');
SELECT lion_rs('SELECT count(*) FROM lion_rsm WHERE c <= ''c038'' AND a = 1');
SELECT lion_rs('SELECT count(*) FROM lion_rsm WHERE c > ''c001'' AND b IS NULL');
SELECT lion_rs('SELECT count(*) FROM lion_rsm WHERE b > 400 AND a = 1');

-- ---------- 10. a partitioned table: each partition takes its own way ----------
CREATE TABLE lion_rsp (id int NOT NULL, k int, g int NOT NULL) PARTITION BY RANGE (id);
CREATE TABLE lion_rsp1 PARTITION OF lion_rsp FOR VALUES FROM (0) TO (10000);
CREATE TABLE lion_rsp2 PARTITION OF lion_rsp FOR VALUES FROM (10000) TO (20000);
CREATE TABLE lion_rsp3 PARTITION OF lion_rsp FOR VALUES FROM (20000) TO (30000);
INSERT INTO lion_rsp
SELECT i, CASE WHEN i % 19 = 0 THEN NULL ELSE i / 10 END, i % 7
  FROM generate_series(0, 29999) i;
CREATE INDEX lion_rsp_k ON lion_rsp USING lion (k);
CREATE INDEX lion_rsp_g ON lion_rsp USING lion (g);
VACUUM (FREEZE, ANALYZE) lion_rsp;
-- partition 1 holds k 0 .. 999, partition 2 1000 .. 1999, partition 3 the rest
SELECT lion_rs('SELECT count(*) FROM lion_rsp WHERE k >= 900 AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rsp WHERE k >= 50 AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rsp WHERE k BETWEEN 1990 AND 2995 AND g = 3');
SELECT lion_rs('SELECT count(*) FROM lion_rsp WHERE k >= 2995 AND g = 3');

DROP TABLE lion_rs, lion_rsm, lion_rsp;
DROP FUNCTION lion_rs(text), lion_rs_prop(text, text);
