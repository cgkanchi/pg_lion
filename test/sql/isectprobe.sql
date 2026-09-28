-- The intersection probe (DESIGN.md §29.11, "Correlated sets"), and one
-- price for the AND of sets ("One price for the AND of sets").
--
-- The planner estimates a conjunction as the product of its clauses'
-- selectivities.  Filters that follow one hidden attribute are not
-- independent, and their AND holds far more rows than that product says:
-- lion's heap paths were priced for a handful of rows, and a count that reads
-- no heap was priced out of the plan.  Each such clause is a posting set of a
-- lion index, so lion now measures their AND from the index for its OWN
-- estimates - the smallest set's containers, sampled, with the others ANDed
-- in - while core's estimates and the relation's row count stay core's.
--
-- The table is sampled whole by ANALYZE, so its statistics - and the plans -
-- are the same on every run; and its hidden groups take turns row by row, so
-- every container of the heap holds them alike and the share of the driver's
-- rows the probe finds surviving is the table's, whichever containers it
-- samples.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
-- a sample of every row: the statistics, and the plans, do not depend on it
SET default_statistics_target = 1000;
SET max_parallel_workers_per_gather = 0;

-- 100,000 rows in 50 hidden groups h = id % 50 of 2,000 rows, each group on
-- every page.  c5 (5 values), c25 (25), c50 (50), m5 (5) and the
-- tags ('t' || h % 3, 'u' || h % 7) follow h; k23 (23 values), p (7) and
-- q (11) do not, and neither do p and q each other.
CREATE TABLE lip (id int NOT NULL, c5 int NOT NULL, c25 int NOT NULL,
				  c50 int NOT NULL, m5 int NOT NULL, tags text[] NOT NULL,
				  k23 int NOT NULL, p int NOT NULL, q int NOT NULL, pad text);
INSERT INTO lip
SELECT i, (i % 50) / 10, (i % 50) / 2, i % 50, (i % 50) % 5,
	   ARRAY['t' || ((i % 50) % 3), 'u' || ((i % 50) % 7)],
	   i % 23, i % 7, i % 11, repeat('x', 100)
  FROM generate_series(1, 100000) i;
CREATE INDEX lip_i ON lip USING lion (c5, c25, c50, m5, tags, p, q);
CREATE INDEX lip_k23 ON lip USING lion (k23);
VACUUM (FREEZE, ANALYZE) lip;

/*
 * lip_rows() is the row estimate of the Bitmap Index Scan of lip_i that
 * answers q - amcostestimate()'s selectivity, lion's own estimate - with the
 * probe on or off.
 */
CREATE FUNCTION lip_rows(q text, probe boolean) RETURNS bigint
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
	r := (regexp_match(j::text, '"Node Type": "Bitmap Index Scan",[^}]*"Index Name": "lip_i",[^}]*"Plan Rows": (\d+)'))[1];
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('pg_lion.enable_intersection_probe', 'on', true);
	RETURN r;
END $$;

/* The rows q returns, read by a sequential scan: the reference answer. */
CREATE FUNCTION lip_actual(q text) RETURNS bigint
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

/* Is q's plan, with nothing disabled, the count pushdown? */
CREATE FUNCTION lip_pushed(q text, probe boolean) RETURNS boolean
LANGUAGE plpgsql AS $$
DECLARE
	ln	text;
	res	boolean := false;
BEGIN
	PERFORM set_config('pg_lion.enable_intersection_probe',
					   CASE WHEN probe THEN 'on' ELSE 'off' END, true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(LionCount\)' THEN
			res := true;
		END IF;
	END LOOP;
	PERFORM set_config('pg_lion.enable_intersection_probe', 'on', true);
	RETURN res;
END $$;

/*
 * q's rows with nothing disabled, against the same query read by a
 * sequential scan with the pushdown off, as a multiset: how many rows differ.
 */
CREATE FUNCTION lip_diff(q text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	n	bigint;
BEGIN
	EXECUTE format('CREATE TEMP TABLE lip_on AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	EXECUTE format('CREATE TEMP TABLE lip_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lip_on EXCEPT ALL SELECT * FROM lip_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lip_off EXCEPT ALL SELECT * FROM lip_on) b)'
		INTO n;
	DROP TABLE lip_on, lip_off;
	RETURN n;
END $$;

-- ---------- 1. the estimate of the lion scan ----------
-- 1: five clauses that all follow h, and hold exactly h = 11's 2,000 rows;
--    the product of their selectivities is two rows.
-- 2: two of them, one implying the other: 2,000 rows estimated at 80.
-- 3: p and q are independent: the product is right, and the probe, which
--    finds the same, leaves the estimate exactly as it was.
-- 4: one clause is no conjunction: nothing to measure.
CREATE TABLE lip_q (n int, q text);
INSERT INTO lip_q VALUES
	(1, $$SELECT id FROM lip WHERE c5 = 1 AND c25 = 5 AND c50 = 11 AND m5 IN (1, 3) AND tags && '{t2}'$$),
	(2, $$SELECT id FROM lip WHERE c25 = 5 AND c50 = 11$$),
	(3, $$SELECT id FROM lip WHERE p = 3 AND q = 5$$),
	(4, $$SELECT id FROM lip WHERE c50 = 11$$);
SELECT n, lip_actual(q) AS actual,
	   lip_rows(q, true) BETWEEN lip_actual(q) / 2 AND lip_actual(q) * 2 AS probed_within_2x,
	   lip_rows(q, false) BETWEEN lip_actual(q) / 2 AND lip_actual(q) * 2 AS unprobed_within_2x,
	   lip_rows(q, true) = lip_rows(q, false) AS unchanged
  FROM lip_q ORDER BY n;
-- the five clauses' product is fifty times short or more
SELECT lip_rows(q, false) * 50 <= lip_actual(q) AS unprobed_50x_short
  FROM lip_q WHERE n = 1;

-- ---------- 2. the count ----------
-- The count ANDs every clause, the IN list's union of two dense sets
-- included; the lion scan may leave that list to its heap filter and AND the
-- other four sets.  Priced for the two rows core's product says, that scan
-- and its heap cost about a third of the count; priced for the 2,000 rows the
-- probe measures - the rows it fetches - over ten times the count, which reads
-- the visibility map where the scan reads the heap.
SELECT lip_pushed(q, true) AS probed, lip_pushed(q, false) AS unprobed
  FROM (VALUES ($$SELECT count(*) FROM lip WHERE c5 = 1 AND c25 = 5 AND c50 = 11 AND m5 IN (1, 3) AND tags && '{t2}'$$)) v(q);
-- A GROUP BY count merges the sets with each of k23's 23 entries, which
-- costs the same however many rows the AND holds, while a heap path fetches
-- them: priced for two rows, a heap path wins by about nine times; priced for
-- the 2,000 the probe measures, the count does by about four.
SELECT lip_pushed(q, true) AS probed, lip_pushed(q, false) AS unprobed
  FROM (VALUES ($$SELECT k23, count(*) FROM lip WHERE c5 = 1 AND c25 = 5 AND c50 = 11 AND m5 IN (1, 3) AND tags && '{t2}' GROUP BY k23$$)) v(q);
-- and the answers are the sequential scan's
SELECT count(*) FROM lip WHERE c5 = 1 AND c25 = 5 AND c50 = 11 AND m5 IN (1, 3) AND tags && '{t2}';
SELECT count(*) AS groups, sum(n) AS rows
  FROM (SELECT k23, count(*) AS n FROM lip WHERE c5 = 1 AND c25 = 5 AND c50 = 11 AND m5 IN (1, 3) AND tags && '{t2}' GROUP BY k23) s;
SELECT lip_diff($$SELECT count(*) FROM lip WHERE c5 = 1 AND c25 = 5 AND c50 = 11 AND m5 IN (1, 3) AND tags && '{t2}'$$) AS count_differs,
	   lip_diff($$SELECT k23, count(*) FROM lip WHERE c5 = 1 AND c25 = 5 AND c50 = 11 AND m5 IN (1, 3) AND tags && '{t2}' GROUP BY k23$$) AS groups_differ,
	   lip_diff($$SELECT id FROM lip WHERE p = 3 AND q = 5$$) AS control_differs;

DROP TABLE lip, lip_q;
DROP FUNCTION lip_rows(text, boolean), lip_actual(text),
	lip_pushed(text, boolean), lip_diff(text);
RESET default_statistics_target;
