-- A run of `<>` is one exclusion (DESIGN.md §29.11, "A run of `<>` is one
-- exclusion").
--
-- `a <> 'v1' AND a <> 'v2' AND ...` and `a NOT IN ('v1', 'v2', ...)` keep the
-- rows whose a is not NULL and is none of the values.  Core estimates each
-- `<>` as 1 - freq(v) - nullfrac and multiplies them, which takes the NULLs
-- away once a clause, and sums `<> ALL` the same way until that goes below
-- zero, when it multiplies too: over a column a third NULL, thirteen of them
-- come out a hundred and fifty times short, and a few dozen at one row.  The
-- values' rows are disjoint, so lion's estimates take the run as the one
-- exclusion it is, 1 - nullfrac - the sum of the distinct values'
-- frequencies: the relation's rows, a parameterized scan's, lion's own index
-- selectivity and LionOrdered's rows.  The answers do not change.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
CREATE EXTENSION IF NOT EXISTS citext;
CREATE EXTENSION IF NOT EXISTS pg_lion_citext;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
-- a sample of every row: the statistics, and the estimates, are the same on
-- every run
SET default_statistics_target = 1000;
SET max_parallel_workers_per_gather = 0;

-- 100,000 rows.  a: 35% NULL, 150 values, ten of them eight times as common
-- as the rest.  b: 50% NULL, 2,000 values of 25 rows each - too even for any
-- of them to be a most common value, so that eqsel() takes each at the
-- average of the others.  c: a citext column 25% NULL, whose 'Red' and 'red'
-- are one value.  k: 50 values, no NULL, a run of 100 ids each, which
-- leaves it independent of the others.
CREATE TABLE lnq (id int PRIMARY KEY, a text, b int, c citext, k int NOT NULL,
				  pad text);
INSERT INTO lnq
SELECT i,
	   CASE WHEN i % 20 < 7 THEN NULL
			WHEN i % 3 = 0 THEN 'v' || (i % 10)
			ELSE 'v' || (i / 3 % 150) END,
	   CASE WHEN i % 2 = 0 THEN NULL ELSE (i * 7919) % 4000 END,
	   CASE WHEN i % 4 = 0 THEN NULL
			ELSE (ARRAY['Red', 'red', 'Blue', 'Green', 'Gray'])[1 + (i / 4) % 5] END,
	   (i / 100) % 50,
	   repeat('x', 40)
  FROM generate_series(1, 100000) i;
CREATE INDEX lnq_l ON lnq USING lion (a, b, k);
CREATE INDEX lnq_c ON lnq USING lion (c);
-- the same rows, without a lion index
CREATE TABLE lnq_nl AS SELECT * FROM lnq;
CREATE INDEX lnq_nl_k ON lnq_nl (k);
-- one row for each k, to join to
CREATE TABLE lnq_d (k int PRIMARY KEY, label text);
INSERT INTO lnq_d SELECT k, 'k' || k FROM generate_series(0, 49) k;
VACUUM (FREEZE, ANALYZE) lnq, lnq_nl, lnq_d;
SELECT count(*) FILTER (WHERE a IS NULL) AS a_null,
	   count(DISTINCT a) AS a_values,
	   count(*) FILTER (WHERE b IS NULL) AS b_null,
	   count(DISTINCT b) AS b_values,
	   count(*) FILTER (WHERE c IS NULL) AS c_null,
	   count(DISTINCT c) AS c_values
  FROM lnq;

/*
 * lnq_est() is the planner's row estimate of q's top node, or of the node
 * that scans `node`, with pg_lion.enable_rows_correction on or off: off, the
 * relation's rows are core's.
 */
CREATE FUNCTION lnq_est(q text, fix boolean, node text DEFAULT NULL)
RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	j	json;
	r	bigint;
BEGIN
	PERFORM set_config('pg_lion.enable_rows_correction',
					   CASE WHEN fix THEN 'on' ELSE 'off' END, true);
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	IF node IS NULL THEN
		r := (j->0->'Plan'->>'Plan Rows')::bigint;
	ELSE
		r := (regexp_match(j::text, '"Relation Name": "' || node ||
						   '",[^}]*"Plan Rows": (\d+)'))[1];
	END IF;
	PERFORM set_config('pg_lion.enable_rows_correction', 'on', true);
	RETURN r;
END $$;

CREATE FUNCTION lnq_actual(q text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	n	bigint;
BEGIN
	EXECUTE 'SELECT count(*) FROM (' || q || ') s' INTO n;
	RETURN n;
END $$;

-- The filters, written out in full.
-- 1: thirteen `<>` of a.
-- 2: the same thirteen values as NOT IN.
-- 3: thirteen values again: a NOT IN of seven, two of them repeated as `<>`,
--    one commuted, and six more `<>`.
-- 4: forty values, ten of them the common ones: core's estimate is one row.
-- 5: eighty values, as NOT IN.
-- 6: sixty values of b, as NOT IN, none of them a most common value.
-- 7: the thirteen of 1 beside k = 3, which is independent of them.
CREATE TABLE lnq_q (n int, w text);
INSERT INTO lnq_q
SELECT 1, string_agg(format('a <> %L', 'v' || v), ' AND ' ORDER BY v)
  FROM generate_series(1, 13) v
UNION ALL
SELECT 2, 'a NOT IN (' || string_agg(format('%L', 'v' || v), ', ' ORDER BY v) || ')'
  FROM generate_series(1, 13) v
UNION ALL
SELECT 3, $$a NOT IN ('v1', 'v2', 'v3', 'v4', 'v5', 'v6', 'v7') AND a <> 'v2' AND 'v8' <> a AND a <> 'v7' AND a <> 'v9' AND a <> 'v10' AND a <> 'v11' AND a <> 'v12' AND a <> 'v13'$$
UNION ALL
SELECT 4, string_agg(format('a <> %L', 'v' || v), ' AND ' ORDER BY v)
  FROM generate_series(0, 39) v
UNION ALL
SELECT 5, 'a NOT IN (' || string_agg(format('%L', 'v' || v), ', ' ORDER BY v) || ')'
  FROM generate_series(20, 99) v
UNION ALL
SELECT 6, 'b NOT IN (' || string_agg(((v * 7919) % 4000)::text, ', ' ORDER BY v) || ')'
  FROM generate_series(1, 119, 2) v
UNION ALL
SELECT 7, 'k = 3 AND ' || string_agg(format('a <> %L', 'v' || v), ' AND ' ORDER BY v)
  FROM generate_series(1, 13) v;

-- ---------- 1. the relation's rows ----------
-- With the correction, within twice the rows the filter keeps; without it -
-- core's product - a hundred times short or more.
SELECT n, lnq_actual('SELECT id FROM lnq WHERE ' || w) AS actual,
	   lnq_est('SELECT id FROM lnq WHERE ' || w, true)
		BETWEEN lnq_actual('SELECT id FROM lnq WHERE ' || w) / 2
			AND lnq_actual('SELECT id FROM lnq WHERE ' || w) * 2 AS within_2x,
	   lnq_est('SELECT id FROM lnq WHERE ' || w, false) * 100
		< lnq_actual('SELECT id FROM lnq WHERE ' || w) AS core_100x_short
  FROM lnq_q ORDER BY n;
-- The three spellings of one filter of thirteen values are one estimate.
SELECT count(DISTINCT lnq_est('SELECT id FROM lnq WHERE ' || w, true)) AS estimates
  FROM lnq_q WHERE n IN (1, 2, 3);
-- The intersection probe has nothing to measure here, and its switch does
-- not change the estimate.
SET pg_lion.enable_intersection_probe = off;
SELECT n, lnq_est('SELECT id FROM lnq WHERE ' || w, true)
		BETWEEN lnq_actual('SELECT id FROM lnq WHERE ' || w) / 2
			AND lnq_actual('SELECT id FROM lnq WHERE ' || w) * 2 AS within_2x
  FROM lnq_q WHERE n IN (1, 6) ORDER BY n;
RESET pg_lion.enable_intersection_probe;

-- ---------- 2. a join above it ----------
-- Sized from the relation's rows: within twice its rows with the
-- correction, a hundred times short without.
SELECT n, lnq_actual('SELECT d.label FROM lnq r JOIN lnq_d d ON d.k = r.k WHERE r.' || replace(w, ' AND ', ' AND r.')) AS actual,
	   lnq_est('SELECT d.label FROM lnq r JOIN lnq_d d ON d.k = r.k WHERE r.' || replace(w, ' AND ', ' AND r.'), true)
		BETWEEN lnq_actual('SELECT d.label FROM lnq r JOIN lnq_d d ON d.k = r.k WHERE r.' || replace(w, ' AND ', ' AND r.')) / 2
			AND lnq_actual('SELECT d.label FROM lnq r JOIN lnq_d d ON d.k = r.k WHERE r.' || replace(w, ' AND ', ' AND r.')) * 2 AS within_2x,
	   lnq_est('SELECT d.label FROM lnq r JOIN lnq_d d ON d.k = r.k WHERE r.' || replace(w, ' AND ', ' AND r.'), false) * 100
		< lnq_actual('SELECT d.label FROM lnq r JOIN lnq_d d ON d.k = r.k WHERE r.' || replace(w, ' AND ', ' AND r.')) AS core_100x_short
  FROM lnq_q WHERE n IN (1, 2) ORDER BY n;

-- ---------- 3. a parameterized scan ----------
-- The inner side of a nested loop over lnq_d, looked up on k through the
-- lion index: the run with the join clause ANDed in, a fiftieth of the
-- filter's rows a loop, where core's product says one row.
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_seqscan = off;
SELECT n, lnq_actual('SELECT r.id FROM lnq_d d JOIN lnq r ON r.k = d.k WHERE d.k = 7 AND r.' || replace(w, ' AND ', ' AND r.')) AS actual,
	   lnq_est('SELECT r.id FROM lnq_d d JOIN lnq r ON r.k = d.k WHERE d.k = 7 AND r.' || replace(w, ' AND ', ' AND r.'), true, 'lnq')
		BETWEEN lnq_actual('SELECT r.id FROM lnq_d d JOIN lnq r ON r.k = d.k WHERE d.k = 7 AND r.' || replace(w, ' AND ', ' AND r.')) / 2
			AND lnq_actual('SELECT r.id FROM lnq_d d JOIN lnq r ON r.k = d.k WHERE d.k = 7 AND r.' || replace(w, ' AND ', ' AND r.')) * 2 AS within_2x,
	   lnq_est('SELECT r.id FROM lnq_d d JOIN lnq r ON r.k = d.k WHERE d.k = 7 AND r.' || replace(w, ' AND ', ' AND r.'), false, 'lnq') AS core
  FROM lnq_q WHERE n = 1;
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_seqscan;

-- ---------- 4. lion's own estimates ----------
/*
 * The rows of q's Bitmap Index Scan - lion's selectivity for its index quals
 * (lioncostestimate()) - with every other scan disabled.  The run's `<>`
 * clauses are index quals; a NOT IN is not one, and is left to the heap.
 */
CREATE FUNCTION lnq_bitmap_rows(q text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	j	json;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	RETURN (regexp_match(j::text, '"Node Type": "Bitmap Index Scan",[^}]*"Plan Rows": (\d+)'))[1];
END $$;
SELECT n, lnq_bitmap_rows('SELECT id FROM lnq WHERE ' || w)
		BETWEEN lnq_actual('SELECT id FROM lnq WHERE ' || w) / 2
			AND lnq_actual('SELECT id FROM lnq WHERE ' || w) * 2 AS index_within_2x
  FROM lnq_q WHERE n IN (1, 4, 7) ORDER BY n;

/*
 * The rows LionOrdered is priced for - lion's estimate of the filter's rows
 * (lion_probe_rel_rows()), whether or not the correction reaches the
 * relation's rows - with every scan core would compare it with disabled.  Its
 * lion side is an index path, so a filter of `<>` clauses, not a NOT IN.
 */
CREATE FUNCTION lnq_lo_rows(q text, fix boolean) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	j	jsonb;
BEGIN
	PERFORM set_config('pg_lion.enable_rows_correction',
					   CASE WHEN fix THEN 'on' ELSE 'off' END, true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	PERFORM set_config('pg_lion.pushdown_margin', '1000', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_sort', 'off', true);
	EXECUTE 'EXPLAIN (FORMAT JSON) SELECT id FROM lnq WHERE ' || q ||
		' ORDER BY id LIMIT 10' INTO j;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
	PERFORM set_config('pg_lion.pushdown_margin', '1', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_sort', 'on', true);
	PERFORM set_config('pg_lion.enable_rows_correction', 'on', true);
	RETURN (jsonb_path_query_first(j, 'strict $.** ? (@."Custom Plan Provider" == "LionOrdered")')
			->> 'Plan Rows')::bigint;
END $$;
SELECT n, lnq_lo_rows(w, true)
		BETWEEN lnq_actual('SELECT id FROM lnq WHERE ' || w) / 2
			AND lnq_actual('SELECT id FROM lnq WHERE ' || w) * 2 AS ordered_within_2x,
	   lnq_lo_rows(w, true) = lnq_lo_rows(w, false) AS same_without_correction
  FROM lnq_q WHERE n IN (1, 4, 7) ORDER BY n;

-- ---------- 5. what is left to core ----------
-- The estimate is core's, exactly, with the correction as without it:
-- 1: one `<>`, which core estimates exactly;
-- 2: one value written twice, which is that value's `<>` - 1's estimate;
-- 3: `<>` of two columns, a value each;
-- 4: a NOT IN with a NULL among its values, which keeps no row;
-- 5: `<> ALL` of no value, which keeps every row;
-- 6: a table without a lion index.
SELECT lnq_est($$SELECT id FROM lnq WHERE a <> 'v3'$$, true) =
	   lnq_est($$SELECT id FROM lnq WHERE a <> 'v3'$$, false) AS one_value,
	   lnq_est($$SELECT id FROM lnq WHERE a <> 'v3' AND a <> 'v3'$$, true) =
	   lnq_est($$SELECT id FROM lnq WHERE a <> 'v3'$$, false) AS one_value_twice,
	   lnq_est($$SELECT id FROM lnq WHERE a <> 'v3' AND b <> 5$$, true) =
	   lnq_est($$SELECT id FROM lnq WHERE a <> 'v3' AND b <> 5$$, false) AS two_columns,
	   lnq_est($$SELECT id FROM lnq WHERE a NOT IN ('v1', NULL)$$, true) =
	   lnq_est($$SELECT id FROM lnq WHERE a NOT IN ('v1', NULL)$$, false) AS null_value,
	   lnq_est($$SELECT id FROM lnq WHERE a <> ALL ('{}'::text[])$$, true) =
	   lnq_est($$SELECT id FROM lnq WHERE a <> ALL ('{}'::text[])$$, false) AS no_value,
	   lnq_est('SELECT id FROM lnq_nl WHERE ' || (SELECT w FROM lnq_q WHERE n = 1), true) =
	   lnq_est('SELECT id FROM lnq_nl WHERE ' || (SELECT w FROM lnq_q WHERE n = 1), false) AS no_lion_index;
-- A value known only at run time: a generic plan's Params are no constants,
-- and the run stays core's.
SET plan_cache_mode = force_generic_plan;
PREPARE lnq_p(text, text, text) AS SELECT id FROM lnq WHERE a <> $1 AND a <> $2 AND a <> $3;
SELECT lnq_est($$EXECUTE lnq_p('v1', 'v2', 'v3')$$, true) =
	   lnq_est($$EXECUTE lnq_p('v1', 'v2', 'v3')$$, false) AS params_left;
DEALLOCATE lnq_p;
RESET plan_cache_mode;

-- ---------- 6. values its type calls equal ----------
-- 'Red', 'RED' and 'red' are one citext value, counted once: three
-- exclusions of it are one, estimated as core estimates it alone.  'Blue'
-- beside them is a second value.
SELECT lnq_est($$SELECT id FROM lnq WHERE c <> 'Red' AND c <> 'RED' AND c NOT IN ('red')$$, true) =
	   lnq_est($$SELECT id FROM lnq WHERE c <> 'red'$$, false) AS one_value,
	   lnq_actual($$SELECT id FROM lnq WHERE c <> 'Red' AND c <> 'RED' AND c NOT IN ('red')$$) AS actual,
	   lnq_est($$SELECT id FROM lnq WHERE c <> 'Red' AND c <> 'Blue' AND c <> 'RED'$$, true)
		BETWEEN lnq_actual($$SELECT id FROM lnq WHERE c <> 'Red' AND c <> 'Blue'$$) / 2
			AND lnq_actual($$SELECT id FROM lnq WHERE c <> 'Red' AND c <> 'Blue'$$) * 2 AS two_values_within_2x;

-- ---------- 7. the answers ----------
/*
 * q's rows with nothing disabled, against the same query read by a
 * sequential scan with the pushdown off, as a multiset: how many rows differ.
 */
CREATE FUNCTION lnq_diff(q text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	n	bigint;
BEGIN
	EXECUTE format('CREATE TEMP TABLE lnq_on AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	EXECUTE format('CREATE TEMP TABLE lnq_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lnq_on EXCEPT ALL SELECT * FROM lnq_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lnq_off EXCEPT ALL SELECT * FROM lnq_on) b)'
		INTO n;
	DROP TABLE lnq_on, lnq_off;
	RETURN n;
END $$;
SELECT n, lnq_diff('SELECT count(*) FROM lnq WHERE ' || w) AS count_differs,
	   lnq_diff('SELECT id FROM lnq WHERE ' || w) AS rows_differ,
	   lnq_diff('SELECT k, count(*) FROM lnq WHERE ' || w || ' GROUP BY k') AS groups_differ,
	   lnq_diff('SELECT id FROM lnq WHERE ' || w || ' ORDER BY id LIMIT 10') AS page_differs
  FROM lnq_q ORDER BY n;
-- The count of a run is the pushdown's, whose negated sources subtract the
-- values' entries and the NULL one (DESIGN.md §35).
EXPLAIN (COSTS OFF)
SELECT count(*) FROM lnq WHERE k = 3 AND a NOT IN ('v1', 'v2', 'v3') AND a <> 'v4';

DROP TABLE lnq, lnq_nl, lnq_d, lnq_q;
DROP FUNCTION lnq_est(text, boolean, text), lnq_actual(text),
	lnq_bitmap_rows(text), lnq_lo_rows(text, boolean), lnq_diff(text);
