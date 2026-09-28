-- An OR of equalities is its IN list (DESIGN.md §29.11, "An OR of equalities
-- is its IN list"), and the intersection probe on a wide filter ("Wide
-- filters").
--
-- `k = 1 OR k = 7` and `k IN (1, 7)` select the same rows.  Lion took the
-- first for a union of single-key leaves: looked up one by one, priced by
-- another formula than the list, never summed, never measured by the probe -
-- whose clauses it did not accept - and, before PostgreSQL 18, left to the
-- heap filter of every lion scan.  Now an OR of equalities on one column with
-- constants is the list it spells wherever lion answers, prices or measures
-- it: the count pushdown counts the list, the probe measures the conjunction
-- with the list, and on 16 and 17 lion builds the index paths 18's own
-- matching builds for it.  So the two spellings of a filter plan alike, at the
-- same price.
--
-- The filter is wide: nine clauses on nine key columns of one index - four
-- lists, a multi-key overlap, four equalities, a boolean among them - twenty
-- posting sets, all following one hidden group.  Their AND holds a few hundred
-- rows where the product of their selectivities says one or two, and the
-- probe, which gave up on twenty sets past its budget of a single probe, now
-- measures them - for the count, the plain and bitmap scans, and LionOrdered.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET default_statistics_target = 1000;
SET max_parallel_workers_per_gather = 0;

-- 400,000 rows in 100 hidden groups h, which take turns over the heap.  Each
-- of e1, e2, fl, e3, m1, m2, m3, m4 and arr's first element is its row's
-- group's value 7 times in 10, else another; x1 and x2 follow nothing.  The
-- mixing is arithmetic, so the table - and what the probe samples of it - is
-- the same on every run.
CREATE TABLE olw (id int NOT NULL, h int NOT NULL,
				  e1 int NOT NULL, e2 int NOT NULL, fl boolean NOT NULL,
				  e3 int NOT NULL, m1 int NOT NULL, m2 int NOT NULL,
				  m3 int NOT NULL, m4 int NOT NULL, arr int[] NOT NULL,
				  x1 int NOT NULL, x2 int NOT NULL, pad text);
INSERT INTO olw
SELECT i, h,
	   CASE WHEN (i * 48271 % 1000003) % 10 < 7 THEN h % 4 ELSE (i * 69621 % 1000003) % 4 END,
	   CASE WHEN (i * 16807 % 1000003) % 10 < 7 THEN (h / 4) % 6 ELSE (i * 40692 % 1000003) % 6 END,
	   CASE WHEN (i * 39373 % 1000003) % 10 < 7 THEN (h / 24) % 2 = 0 ELSE (i * 40014 % 1000003) % 2 = 0 END,
	   CASE WHEN (i * 45742 % 1000003) % 10 < 7 THEN (h * 7) % 5 ELSE (i * 62089 % 1000003) % 5 END,
	   CASE WHEN (i * 30271 % 1000003) % 10 < 7 THEN (h * 3) % 16 ELSE (i * 52391 % 1000003) % 16 END,
	   CASE WHEN (i * 33141 % 1000003) % 10 < 7 THEN (h * 5 + 1) % 16 ELSE (i * 71389 % 1000003) % 16 END,
	   CASE WHEN (i * 25657 % 1000003) % 10 < 7 THEN h % 9 ELSE (i * 57383 % 1000003) % 9 END,
	   CASE WHEN (i * 69621 % 1000003) % 10 < 7 THEN h % 6 ELSE (i * 39373 % 1000003) % 6 END,
	   ARRAY[CASE WHEN (i * 40692 % 1000003) % 10 < 7 THEN h % 24 ELSE (i * 45742 % 1000003) % 24 END,
			 (i * 52391 % 1000003) % 24],
	   (i * 71389 % 1000003) % 7,
	   (i * 57383 % 1000003) % 11,
	   repeat('x', 40)
  FROM (SELECT i::bigint AS i, ((i::bigint * 7919 + i / 3) % 100)::int AS h
		  FROM generate_series(1, 400000) i) s;
CREATE INDEX olw_l ON olw USING lion (e1, e2, fl, e3, m1, m2, m3, m4, arr, x1, x2);
CREATE INDEX olw_id ON olw (id);
VACUUM (FREEZE, ANALYZE) olw;

-- The filter of group 13, as lists and as ORs: the equalities pin the group,
-- each list holds its value and others'.
CREATE TABLE olw_q (n int, form text, q text);
INSERT INTO olw_q VALUES
	(1, 'in', $$e1 = 1 AND e2 = 3 AND fl AND e3 = 1 AND m1 IN (7, 1, 9, 0) AND m2 IN (2, 11, 0, 1) AND m3 IN (4, 0, 8) AND m4 IN (1, 0) AND arr && '{13,1,0}'$$),
	(1, 'or', $$e1 = 1 AND e2 = 3 AND fl AND e3 = 1 AND (m1 = 7 OR m1 = 1 OR m1 = 9 OR m1 = 0) AND (m2 = 2 OR m2 = 11 OR m2 = 0 OR 1 = m2) AND (m3 = 4 OR m3 = 0 OR m3 = 8) AND (m4 = 1 OR m4 = 0) AND arr && '{13,1,0}'$$),
	-- a list alone: the disjoint sum of its entries
	(2, 'in', $$m3 IN (4, 0, 8)$$),
	(2, 'or', $$(m3 = 4 OR m3 = 0 OR m3 = 8)$$),
	-- two lists and an equality, the one list long
	(3, 'in', $$x2 IN (1, 2, 3, 4, 5, 6, 7, 8) AND m4 IN (1, 0) AND e1 = 1$$),
	(3, 'or', $$(x2 = 1 OR x2 = 2 OR x2 = 3 OR x2 = 4 OR x2 = 5 OR x2 = 6 OR x2 = 7 OR x2 = 8) AND (m4 = 1 OR m4 = 0) AND e1 = 1$$);

/* The plan of q, costs off, and its total cost. */
CREATE FUNCTION olw_plan(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln	text;
	res	text := '';
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		res := res || ln || E'\n';
	END LOOP;
	RETURN res;
END $$;
CREATE FUNCTION olw_cost(q text) RETURNS numeric
LANGUAGE plpgsql AS $$
DECLARE
	j	json;
BEGIN
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	RETURN (j -> 0 -> 'Plan' ->> 'Total Cost')::numeric;
END $$;

/*
 * q's rows with nothing disabled, against the same query read by a
 * sequential scan with the pushdown off, as a multiset: how many rows differ.
 */
CREATE FUNCTION olw_diff(q text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	n	bigint;
BEGIN
	EXECUTE format('CREATE TEMP TABLE olw_on AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	EXECUTE format('CREATE TEMP TABLE olw_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM olw_on EXCEPT ALL SELECT * FROM olw_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM olw_off EXCEPT ALL SELECT * FROM olw_on) b)'
		INTO n;
	DROP TABLE olw_on, olw_off;
	RETURN n;
END $$;

/*
 * The rows LionOrdered is priced for - lion's estimate of the filter's rows,
 * the intersection probe's when it measured the filter (lion_probe_rel_rows())
 * - with the probe on or off: the node with every scan core would compare it
 * with disabled, so that it is the plan on every version.
 */
CREATE FUNCTION olw_lo_rows(q text, probe boolean) RETURNS numeric
LANGUAGE plpgsql AS $$
DECLARE
	j	jsonb;
BEGIN
	PERFORM set_config('pg_lion.enable_intersection_probe',
					   CASE WHEN probe THEN 'on' ELSE 'off' END, true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_sort', 'off', true);
	EXECUTE 'EXPLAIN (FORMAT JSON) SELECT id FROM olw WHERE ' || q ||
		' ORDER BY id LIMIT 10' INTO j;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_sort', 'on', true);
	PERFORM set_config('pg_lion.enable_intersection_probe', 'on', true);
	RETURN (jsonb_path_query_first(j, 'strict $.** ? (@."Custom Plan Provider" == "LionOrdered")')
			->> 'Plan Rows')::numeric;
END $$;

-- ---------- 1. one plan, at one price ----------
-- The count ANDs the lists as the count of the IN form does, whichever form
-- the query has: the same plan, the same cost.
SELECT a.n, olw_plan('SELECT count(*) FROM olw WHERE ' || a.q) =
			olw_plan('SELECT count(*) FROM olw WHERE ' || b.q) AS same_plan,
	   olw_cost('SELECT count(*) FROM olw WHERE ' || a.q) =
			olw_cost('SELECT count(*) FROM olw WHERE ' || b.q) AS same_cost
  FROM olw_q a JOIN olw_q b ON a.n = b.n AND a.form = 'in' AND b.form = 'or'
 ORDER BY a.n;
-- ... which is the count of the lists, for the OR form too
SELECT split_part(olw_plan('SELECT count(*) FROM olw WHERE ' || q), E'\n', 1) AS node,
	   olw_plan('SELECT count(*) FROM olw WHERE ' || q) ~ 'olw_l.m1 \(m1 = ANY' AS counts_the_list
  FROM olw_q WHERE n = 1 AND form = 'or';
-- and the same with the probe off
SET pg_lion.enable_intersection_probe = off;
SET enable_seqscan = off;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT a.n, olw_cost('SELECT count(*) FROM olw WHERE ' || a.q) =
			olw_cost('SELECT count(*) FROM olw WHERE ' || b.q) AS same_cost
  FROM olw_q a JOIN olw_q b ON a.n = b.n AND a.form = 'in' AND b.form = 'or'
 ORDER BY a.n;
RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_bitmapscan;
RESET pg_lion.enable_intersection_probe;

-- ---------- 2. the probe measures both forms of the wide filter ----------
-- LionOrdered is priced for lion's estimate of the rows: with the probe, what
-- it measured of the twenty sets - within twice the rows, for either form;
-- without it, core's product, a hundred times short.
SELECT form,
	   (SELECT count(*) FROM olw WHERE e1 = 1 AND e2 = 3 AND fl AND e3 = 1 AND m1 IN (7, 1, 9, 0) AND m2 IN (2, 11, 0, 1) AND m3 IN (4, 0, 8) AND m4 IN (1, 0) AND arr && '{13,1,0}') AS actual,
	   olw_lo_rows(q, true) BETWEEN 287 / 2 AND 287 * 2 AS probed_within_2x,
	   olw_lo_rows(q, false) * 100 < 287 AS unprobed_100x_short
  FROM olw_q WHERE n = 1 ORDER BY form;
SELECT olw_lo_rows(a.q, true) = olw_lo_rows(b.q, true) AS same_rows
  FROM olw_q a JOIN olw_q b ON a.n = b.n AND a.form = 'in' AND b.form = 'or'
 WHERE a.n = 1;

-- ---------- 3. the answers are the sequential scan's ----------
SELECT n, form, olw_diff('SELECT count(*) FROM olw WHERE ' || q) AS count_differs,
	   olw_diff('SELECT id FROM olw WHERE ' || q) AS rows_differ,
	   olw_diff('SELECT x1, count(*) FROM olw WHERE ' || q || ' GROUP BY x1') AS groups_differ,
	   olw_diff('SELECT id FROM olw WHERE ' || q || ' ORDER BY id LIMIT 10') AS page_differs
  FROM olw_q ORDER BY n, form;
-- An OR that is no list stays the OR it is (DESIGN.md §19): two operators,
-- a NULL arm, two columns, an arm that is not an equality.
SELECT olw_diff($$SELECT count(*) FROM olw WHERE m1 = 1 OR m1 = 2::bigint$$) AS two_operators,
	   olw_diff($$SELECT count(*) FROM olw WHERE m1 = 1 OR m1 = NULL$$) AS null_arm,
	   olw_diff($$SELECT count(*) FROM olw WHERE m1 = 1 OR m2 = 2$$) AS two_columns,
	   olw_diff($$SELECT count(*) FROM olw WHERE m1 = 1 OR m1 > 14$$) AS range_arm,
	   olw_diff($$SELECT count(*) FROM olw WHERE (m1 = 1 OR m1 = 1) AND e1 = 1$$) AS repeated_arm;

DROP TABLE olw, olw_q;
DROP FUNCTION olw_plan(text), olw_cost(text), olw_diff(text),
	olw_lo_rows(text, boolean);
RESET default_statistics_target;
