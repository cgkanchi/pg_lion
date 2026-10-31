-- The intersection probe and a long IN list (DESIGN.md §29.11, "Long
-- lists").
--
-- The probe seeks at most LION_ISECT_MAX_SETS (32) sets at each container it
-- samples, and a list of more values used to be left out of it: its AND with
-- the other clauses was estimated as core's product of their selectivities.
-- A list of ids picked because they match the other filters holds many times
-- the rows that product says, and lion's plain scan, priced for the product,
-- won over the count.  A longer list is now measured through an evenly spaced
-- sample of its values, scaled by core's estimate of the list over that of
-- the sample.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET synchronous_commit = on;
SET default_statistics_target = 1000;
SET max_parallel_workers_per_gather = 0;

-- 100,000 rows in 200 hidden groups g = id % 200, each on every page; c
-- (4 values) follows g.
CREATE TABLE lil (id int NOT NULL, g int NOT NULL, c int NOT NULL, pad text);
INSERT INTO lil SELECT i, i % 200, (i % 200) % 4, repeat('x', 100)
  FROM generate_series(1, 100000) i;
CREATE INDEX lil_i ON lil USING lion (c, g);
VACUUM (FREEZE, ANALYZE) lil;

/* The row estimate of the Bitmap Index Scan of lil_i that answers q. */
CREATE FUNCTION lil_rows(q text, probe boolean) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	j	json;
	r	bigint;
BEGIN
	PERFORM set_config('pg_lion.enable_intersection_probe',
					   CASE WHEN probe THEN 'on' ELSE 'off' END, true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	r := (regexp_match(j::text, '"Node Type": "Bitmap Index Scan",[^}]*"Index Name": "lil_i",[^}]*"Plan Rows": (\d+)'))[1];
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('pg_lion.enable_intersection_probe', 'on', true);
	RETURN r;
END $$;

/* The rows q returns, read by a sequential scan. */
CREATE FUNCTION lil_actual(q text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	n	bigint;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	EXECUTE 'SELECT count(*) FROM (' || q || ') s' INTO n;
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	RETURN n;
END $$;

/* The values g takes where c = 1, from lo, n of them (n <= 50). */
CREATE FUNCTION lil_list(lo int, n int) RETURNS text
LANGUAGE sql AS $$
	SELECT string_agg((4 * (lo + i) + 1)::text, ', ' ORDER BY i)
	  FROM generate_series(0, n - 1) i
$$;

-- 1: 10 values, which the probe always measured.
-- 2: 40 values, past LION_ISECT_MAX_SETS: every one with c = 1, so the AND
--    is the list's 20,000 rows, which core's product puts at a quarter.
-- 3: the same 40 values and 40 of c = 3: half the list's rows.
-- 4: 40 values none of which has c = 1 ... their AND is empty.
-- 5: 40 values, half of them absent from the table.
CREATE TABLE lil_q (n int, q text);
INSERT INTO lil_q VALUES
	(1, format('SELECT id FROM lil WHERE c = 1 AND g IN (%s)', lil_list(0, 10))),
	(2, format('SELECT id FROM lil WHERE c = 1 AND g IN (%s)', lil_list(0, 40))),
	(3, format('SELECT id FROM lil WHERE c = 1 AND g IN (%s, %s)', lil_list(0, 40),
			   (SELECT string_agg((4 * i + 3)::text, ', ') FROM generate_series(0, 39) i))),
	(4, format('SELECT id FROM lil WHERE c = 2 AND g IN (%s)', lil_list(0, 40))),
	(5, format('SELECT id FROM lil WHERE c = 1 AND g IN (%s, %s)', lil_list(0, 20),
			   (SELECT string_agg((1000 + i)::text, ', ') FROM generate_series(0, 19) i)));
SELECT n, lil_actual(q) AS actual,
	   lil_rows(q, true) BETWEEN lil_actual(q) / 2 AND greatest(lil_actual(q) * 2, 10) AS probed_within_2x,
	   lil_rows(q, false) BETWEEN lil_actual(q) / 2 AND greatest(lil_actual(q) * 2, 10) AS unprobed_within_2x
  FROM lil_q ORDER BY n;

-- the GROUP BY of the list's column over the list is counted, and exactly
SELECT format('EXPLAIN (COSTS OFF) SELECT g, count(*) FROM lil WHERE c = 1 AND g IN (%s) GROUP BY g', lil_list(0, 40)) \gexec
SELECT count(*) AS groups, sum(n) AS rows
  FROM (SELECT g, count(*) AS n FROM lil WHERE c = 1 AND g IN (1, 5, 9, 13, 17, 21, 25, 29, 33, 37, 41, 45, 49, 53, 57, 61, 65, 69, 73, 77, 81, 85, 89, 93, 97, 101, 105, 109, 113, 117, 121, 125, 129, 133, 137, 141, 145, 149, 153, 157) GROUP BY g) s;

-- A list longer than the 1,000 values whose frequencies lion serves core
-- (DESIGN.md §15, "Long lists"): core prices each value it has no frequency
-- for as an average value of the column, so 5,000 values of which 294 are in
-- the table look like every row, and the count's price of them was the
-- table's.  The count's price uses the counted rows of an evenly spaced
-- sample of the list instead, and is chosen; the answer is the same.
CREATE TABLE lim (id int NOT NULL, k int NOT NULL, pad text);
INSERT INTO lim SELECT i, i % 5000, repeat('x', 100)
  FROM generate_series(1, 100000) i;
CREATE INDEX lim_k ON lim USING lion (k);
VACUUM (FREEZE, ANALYZE) lim;
CREATE TABLE lim_q AS
  SELECT format('SELECT count(*) FROM lim WHERE k IN (%s)',
				string_agg((17 * i)::text, ', ')) AS q
	FROM generate_series(1, 5000) i;

/* The node at the top of q's plan. */
CREATE FUNCTION lim_top(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	j	json;
BEGIN
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	RETURN coalesce(j->0->'Plan'->>'Custom Plan Provider',
					j->0->'Plan'->'Plans'->0->>'Node Type');
END $$;

/* What q returns. */
CREATE FUNCTION lim_count(q text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	n	bigint;
BEGIN
	EXECUTE q INTO n;
	RETURN n;
END $$;

SELECT lim_top(q) FROM lim_q;
SET pg_lion.enable_index_mcv = off;
SELECT lim_top(q) FROM lim_q;
RESET pg_lion.enable_index_mcv;
SELECT lim_count(q) FROM lim_q;
-- the same rows, by a filter no index answers
SELECT count(*) FROM lim WHERE k % 17 = 0 AND k BETWEEN 17 AND 85000;

DROP TABLE lil, lil_q;
DROP TABLE lim, lim_q;
DROP FUNCTION lim_top(text);
DROP FUNCTION lim_count(text);
DROP FUNCTION lil_rows(text, boolean);
DROP FUNCTION lil_actual(text);
DROP FUNCTION lil_list(int, int);
