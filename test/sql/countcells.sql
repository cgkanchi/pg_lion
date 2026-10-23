-- Counts by cells (DESIGN.md §10, "Counts by cells").
--
-- A GROUP BY of expressions made of predicates on lion-indexed columns - a
-- CASE over equalities, IN lists, ORs, null tests, booleans, array overlaps
-- and text searches, with arithmetic over them - groups each row by the
-- truths of those predicates.  The node counts the WHERE's rows by CELL, one
-- truth (true, false or NULL) of every predicate, and computes the
-- expressions once a cell from the cell's truths; core's Finalize Agg adds
-- the cells of a group up.  A WHERE clause over such an expression is tested
-- once a cell the same way, and the rows an anti join over a small inner
-- side takes away come off their cells.  Every query below is checked
-- against a sequential scan with the pushdown off; lcc_check() says whether
-- the cells ran, how many cells the plan bounds them by, and whether rows
-- went to the heap.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

-- EXPLAIN in a form every supported release prints the same way (the
-- definition test/sql/citext.sql uses: 18's "Disabled: true" lines dropped,
-- subplans named "expr_N", integer actual row counts, sorting off while it
-- plans).
CREATE OR REPLACE FUNCTION lion_explain_norm(q text, opts text DEFAULT 'COSTS OFF')
RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	l text;
BEGIN
	PERFORM set_config('enable_sort', 'off', true);
	FOR l IN EXECUTE 'EXPLAIN (' || opts || ') ' || q LOOP
		CONTINUE WHEN l ~ '^\s*Disabled: true$';
		l := regexp_replace(l, '(InitPlan|SubPlan) (\d+)', '\1 expr_\2', 'g');
		RETURN NEXT regexp_replace(l, 'rows=(\d+)\.00 ', 'rows=\1 ', 'g');
	END LOOP;
	PERFORM set_config('enable_sort', 'on', true);
END
$$;

/*
 * The plan of q, as lion_explain_norm() prints it, without the lines that
 * repeat a long expression whole - the grouping key and the sort key above
 * the node, and its filter, which reads `Filter: (...)`.
 */
CREATE FUNCTION lcc_plan(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	l text;
BEGIN
	FOR l IN SELECT * FROM lion_explain_norm(q) LOOP
		CONTINUE WHEN l ~ '^\s*(Group|Sort) Key: ';
		IF l ~ '^\s*Filter: ' THEN
			l := substring(l FROM '^(\s*Filter: )') || '(...)';
		END IF;
		RETURN NEXT l;
	END LOOP;
END $$;

/*
 * lcc_check() runs q with every scan of the heap disabled, so that a count by
 * cells that is built at all is the plan, under EXPLAIN ANALYZE and again for
 * its rows; then with the pushdown off, sequential scans only and every join
 * method on, and proves the two answers equal as multisets.  A caller
 * disables joins to leave the node no plan of core's to beat on price, which
 * only the node's run needs: the reference answer is core's most ordinary
 * plan, and with the joins off it would be a disabled nested loop, which in
 * parallel rescans a Gather over its inner side for each outer row, starting
 * workers each time.  The caller's join settings come back after, for the
 * rest of its transaction.  It says whether the plan counted by cells - in
 * parallel or not - the cells it bounds the count by, and whether rows were
 * tested in the heap.
 */
CREATE FUNCTION lcc_check(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	cells bigint := -1;
	par boolean := false;
	rechecked bigint := 0;
	nrows bigint;
	ndiff bigint;
	hashjoin text := current_setting('enable_hashjoin');
	mergejoin text := current_setting('enable_mergejoin');
	nestloop text := current_setting('enable_nestloop');
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF ln ~ '^\s*Cells: ' THEN
			cells := substring(ln FROM 'Cells: (\d+)')::bigint;
		ELSIF ln ~ 'Parallel Custom Scan \(LionCount\)' THEN
			par := true;
		ELSIF ln ~ 'Decoded Rows Rechecked: ' THEN
			rechecked := rechecked + substring(ln FROM 'Decoded Rows Rechecked: (\d+)')::bigint;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lcc_on AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_nestloop', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE format('CREATE TEMP TABLE lcc_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_hashjoin', hashjoin, true);
	PERFORM set_config('enable_mergejoin', mergejoin, true);
	PERFORM set_config('enable_nestloop', nestloop, true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	SELECT count(*) INTO nrows FROM lcc_off;
	SELECT (SELECT count(*) FROM (TABLE lcc_on EXCEPT ALL TABLE lcc_off) x) +
		   (SELECT count(*) FROM (TABLE lcc_off EXCEPT ALL TABLE lcc_on) y)
	  INTO ndiff;
	DROP TABLE lcc_on, lcc_off;
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows%s',
				  CASE WHEN cells < 0 THEN 'not cells'
					   ELSE CASE WHEN par THEN 'parallel ' ELSE '' END ||
							cells || ' cells' END,
				  nrows,
				  CASE WHEN rechecked > 0 THEN ', rows tested in the heap' ELSE '' END);
END $$;

/*
 * 40,000 rows: k of 15 values and NULLs, s of 6 and NULLs, f a boolean with
 * NULLs and g one without, tags a small array - NULL, empty, or with a NULL
 * element in some rows - w a text search vector, r of 5 values that gates the
 * score, z with no row of value 9 and NULLs, n of 100 values for a range,
 * and pad no lion index has.
 */
CREATE TABLE lcc (id int NOT NULL, k int, s text, f bool, g bool NOT NULL,
				  tags int[], w tsvector, r text NOT NULL, z int, n int NOT NULL,
				  pad text)
	WITH (autovacuum_enabled = off);
INSERT INTO lcc
SELECT i,
	   CASE WHEN h % 23 = 0 THEN NULL ELSE h % 15 END,
	   CASE WHEN h % 11 = 0 THEN NULL ELSE 's' || ((h / 15) % 6) END,
	   CASE WHEN h % 9 = 0 THEN NULL ELSE (h / 7) % 2 = 0 END,
	   (h / 5) % 3 = 0,
	   CASE WHEN h % 17 = 0 THEN NULL
			WHEN h % 19 = 0 THEN '{}'::int[]
			WHEN h % 29 = 0 THEN ARRAY[NULL, (h / 3) % 8]
			ELSE ARRAY[(h / 3) % 8, 8 + (h / 13) % 5] END,
	   CASE WHEN h % 43 = 0 THEN NULL
			ELSE to_tsvector('simple', 'a' || ((h / 47) % 6) || ' b' || ((h / 53) % 4)) END,
	   'r' || ((h / 31) % 5),
	   CASE WHEN h % 3 = 0 THEN NULL ELSE (h / 37) % 4 END,
	   (h / 41) % 100,
	   repeat('x', 20)
  FROM (SELECT i, abs(hashint4(i)) AS h FROM generate_series(1, 40000) i) q;
CREATE INDEX lcc_ks ON lcc USING lion (k, s);
CREATE INDEX lcc_fg ON lcc USING lion (f, g);
CREATE INDEX lcc_tags ON lcc USING lion (tags);
CREATE INDEX lcc_w ON lcc USING lion (w) WITH (store_positions = false);
CREATE INDEX lcc_r ON lcc USING lion (r);
CREATE INDEX lcc_z ON lcc USING lion (z);
CREATE INDEX lcc_n ON lcc USING lion (n);
VACUUM (FREEZE, ANALYZE) lcc;

-- The score: points from k's values, a value of s, s's NULL, the overlap of
-- tags with two sets, the boolean f, a text search and z; rounded, the
-- greater of two parts and the lesser of two others, with a COALESCE; only
-- for the rows r gates, NULL for the others.  The tier is a CASE over it.
CREATE VIEW lcc_scored AS
SELECT id, n, k, s, f, g, r,
	   CASE WHEN r = 'r1' OR r = 'r3' OR r = 'r4' THEN
			round((CASE WHEN k IN (1, 2, 3) THEN 30
						WHEN k IN (4, 5) THEN 20
						WHEN k = 6 OR k = 7 THEN 12
						WHEN k IS NULL THEN 0
						ELSE 5 END
				   + GREATEST(CASE WHEN tags && '{1,2}' THEN 15 ELSE 0 END,
							  CASE WHEN tags && '{9,10}' THEN 10 END)
				   + LEAST(CASE WHEN s = 's2' THEN 25 WHEN s IS NULL THEN 3 ELSE 8 END,
						   CASE WHEN f THEN 20 ELSE 4 END)
				   + COALESCE(CASE WHEN w @@ 'a1 | (a2 & b3)'::tsquery THEN 9 END, 1)
				   + CASE WHEN z = 9 THEN 100 ELSE 0 END) * 1.1)
	   END AS score
  FROM lcc;
CREATE VIEW lcc_tiered AS
SELECT id, n, k, s, f, g, r, score,
	   CASE WHEN score >= 70 THEN 'high'
			WHEN score >= 40 THEN 'medium'
			WHEN score IS NOT NULL THEN 'low'
			ELSE 'none' END AS tier
  FROM lcc_scored;

-- ---------- 1. the plans ----------
-- the node under core's Finalize HashAggregate, its predicates and the
-- cells they make: k's equalities are one column's, which is one dimension
-- of an outcome each (1-3, 4-5, 6-7, another value, NULL)
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET pg_lion.enable_plain_scan = off;
SELECT * FROM lion_explain_norm($$
SELECT CASE WHEN k IN (1, 2, 3) THEN 'a' WHEN k = 4 OR k = 5 THEN 'b'
			WHEN k IS NULL THEN 'n' ELSE 'c' END AS t, count(*)
  FROM lcc WHERE n < 50 GROUP BY 1$$) AS p("QUERY PLAN");
-- the tier: every predicate of the score, and the column it is driven by
SELECT * FROM lcc_plan(
	'SELECT tier, count(*) FROM lcc_tiered WHERE n < 50 GROUP BY tier') AS p("QUERY PLAN");
-- a WHERE clause over predicates is the node's filter, tested once a cell
SELECT * FROM lcc_plan(
	'SELECT count(*) FROM lcc_tiered WHERE n < 50 AND tier = ''high''') AS p("QUERY PLAN");
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET pg_lion.enable_plain_scan;

-- ---------- 2. the tier and the score ----------
SELECT q, lcc_check(q) FROM (VALUES
	('SELECT tier, count(*) FROM lcc_tiered GROUP BY tier'),
	('SELECT tier, count(*) FROM lcc_tiered WHERE n < 50 GROUP BY tier'),
	('SELECT tier, count(*) FROM lcc_tiered WHERE n >= 95 GROUP BY tier'),
	('SELECT tier, count(*) FROM lcc_tiered WHERE n = 7 AND g GROUP BY tier'),
	('SELECT score, count(*) FROM lcc_scored WHERE n < 50 GROUP BY score'),
	('SELECT score, tier, count(*) AS c FROM lcc_tiered GROUP BY score, tier'),
	('SELECT tier, count(*) AS c, count(n) AS cn FROM lcc_tiered WHERE n < 30 GROUP BY tier'),
	('SELECT tier, count(*) FROM lcc_tiered WHERE n < 50 GROUP BY tier HAVING count(*) > 2000'),
	('SELECT tier, count(*) * 2 + 1 AS c FROM lcc_tiered GROUP BY tier'),
	-- a WHERE that selects no row: no group, no row
	('SELECT tier, count(*) FROM lcc_tiered WHERE n = 1000 GROUP BY tier')
) v(q);

-- ---------- 3. the predicates ----------
SELECT q, lcc_check(q) FROM (VALUES
	-- exclusive equalities of one column: one dimension, 15 values and NULL
	('SELECT CASE WHEN k = 0 THEN 0 WHEN k = 1 THEN 1 WHEN k = 2 THEN 2 WHEN k = 3 THEN 3
				  WHEN k = 4 THEN 4 WHEN k = 5 THEN 5 WHEN k = 6 THEN 6 WHEN k = 7 THEN 7
				  WHEN k = 8 THEN 8 WHEN k = 9 THEN 9 WHEN k = 10 THEN 10 WHEN k = 11 THEN 11
				  WHEN k = 12 THEN 12 WHEN k = 13 THEN 13 WHEN k = 14 THEN 14 END AS t,
			  count(*) FROM lcc GROUP BY 1'),
	-- overlapping ones: an IN list, an OR, a <>, NOT IN, IS DISTINCT FROM
	('SELECT CASE WHEN k IN (1, 2, 3) THEN ''a'' WHEN k IN (3, 4) THEN ''b''
				  WHEN k <> 5 THEN ''c'' ELSE ''d'' END AS t, count(*) FROM lcc GROUP BY 1'),
	('SELECT (k = 1 OR k = 9) AS x, (k <> 9) AS y, count(*) FROM lcc GROUP BY 1, 2'),
	('SELECT (k NOT IN (1, 2)) AS x, (k IS DISTINCT FROM 2) AS y, (k IS DISTINCT FROM NULL) AS z,
			 count(*) FROM lcc GROUP BY 1, 2, 3'),
	('SELECT (k IN (1, NULL)) AS x, (k NOT IN (2, NULL)) AS y, count(*) FROM lcc GROUP BY 1, 2'),
	('SELECT (k = 1 AND k = 2) AS x, (NOT (k = 3 OR k IS NULL)) AS y, count(*) FROM lcc GROUP BY 1, 2'),
	-- null tests, and text values
	('SELECT (s IS NULL) AS x, (s = ''s1'') AS y, (s IN (''s2'', ''s3'')) AS z, count(*)
		FROM lcc GROUP BY 1, 2, 3'),
	-- booleans: the column, NOT, IS [NOT] TRUE and IS UNKNOWN, a column of no NULL
	('SELECT NOT f AS x, NOT g AS y, count(*) FROM lcc GROUP BY 1, 2'),
	('SELECT (f IS NOT TRUE) AS x, (f IS UNKNOWN) AS y, (f IS FALSE) AS z, count(*)
		FROM lcc GROUP BY 1, 2, 3'),
	-- two predicates of different columns ANDed and ORed: 3-valued
	('SELECT (f AND k = 1) AS x, (s = ''s1'' OR z = 2) AS y, count(*) FROM lcc GROUP BY 1, 2'),
	('SELECT (NOT (f OR z IS NULL)) AS x, count(*) FROM lcc GROUP BY 1'),
	-- a value no row has, and a column all of whose rows a predicate misses
	('SELECT (z = 9) AS x, (z IS NULL) AS y, count(*) FROM lcc GROUP BY 1, 2'),
	('SELECT CASE WHEN z = 9 THEN ''nine'' WHEN k = 99 THEN ''k'' ELSE ''none'' END AS t,
			 count(*) FROM lcc GROUP BY 1'),
	-- arrays: overlap and containment, the NULL arrays, the empty ones, those
	-- with a NULL element, and an overlap with the empty array, which no
	-- row has
	('SELECT (tags && ''{1,2}'') AS x, (tags @> ''{3,9}'') AS y, count(*) FROM lcc GROUP BY 1, 2'),
	('SELECT (tags && ''{1}'') AS x, (tags IS NULL) AS y, (tags && ''{}'') AS z, count(*)
		FROM lcc GROUP BY 1, 2, 3'),
	-- a null test alone of a column only a multi-key index has: a multi-key
	-- dimension still, whose NULL entry is its rows'
	('SELECT (tags IS NULL) AS x, (k = 1) AS y, count(*) FROM lcc GROUP BY 1, 2'),
	('SELECT (tags && ''{0,1,2,3,4,5,6,7}'') AS x, (tags @> ''{8}'') AS y, (tags && ''{12}'') AS z,
			 count(*) FROM lcc WHERE n < 60 GROUP BY 1, 2, 3'),
	-- text search
	('SELECT (w @@ ''a1 & b2''::tsquery) AS x, (w @@ ''a3 | (a4 & b1)''::tsquery) AS y, count(*)
		FROM lcc GROUP BY 1, 2'),
	-- arithmetic: round, GREATEST, LEAST, COALESCE, and NULLs through them
	('SELECT round(GREATEST(CASE WHEN k IN (1, 2) THEN 2.5 END, CASE WHEN f THEN 1.25 END) * 3) AS v,
			 count(*) FROM lcc GROUP BY 1'),
	('SELECT LEAST(CASE WHEN s = ''s1'' THEN 4 END, COALESCE(CASE WHEN tags && ''{3}'' THEN 2 END, 7)) AS v,
			 count(*) FROM lcc GROUP BY 1'),
	('SELECT (CASE WHEN g THEN 1 ELSE 0 END + CASE WHEN f THEN 1 ELSE 0 END
			  + CASE WHEN z = 1 THEN 1 ELSE 0 END) % 2 AS v, count(*) FROM lcc GROUP BY 1')
) v(q);

-- ---------- 4. the WHERE form ----------
SELECT q, lcc_check(q) FROM (VALUES
	('SELECT count(*) FROM lcc_tiered WHERE tier = ''high'''),
	('SELECT count(*) FROM lcc_tiered WHERE n < 50 AND tier = ''medium'''),
	('SELECT count(*), count(n) FROM lcc_tiered WHERE n < 50 AND tier IN (''low'', ''none'')'),
	('SELECT count(*) FROM lcc_tiered WHERE n < 50 AND score > 40 AND tier <> ''high'''),
	('SELECT count(*) FROM lcc WHERE n < 50 AND CASE WHEN k = 1 THEN f ELSE g END'),
	-- ... selecting no row: a count of 0
	('SELECT count(*) FROM lcc_tiered WHERE n = 7 AND score > 1000'),
	-- ... beside a GROUP BY of expressions
	('SELECT (s IS NULL) AS x, count(*) FROM lcc_tiered WHERE tier = ''low'' GROUP BY 1')
) v(q);

-- ---------- 5. declined ----------
SELECT q, lcc_check(q) FROM (VALUES
	-- a column read outside any predicate
	('SELECT CASE WHEN k = 1 THEN n ELSE 0 END AS v, count(*) FROM lcc GROUP BY 1'),
	('SELECT k + 1 AS v, count(*) FROM lcc GROUP BY 1'),
	('SELECT CASE WHEN pad = ''x'' THEN 1 END AS v, count(*) FROM lcc GROUP BY 1'),
	-- a volatile function, and a stable one
	('SELECT CASE WHEN k = 1 AND random() < 2 THEN 1 END AS v, count(*) FROM lcc GROUP BY 1'),
	('SELECT CASE WHEN k = 1 THEN now() > ''2000-01-01'' END AS v, count(*) FROM lcc GROUP BY 1'),
	-- a CASE with an argument, and a column of the GROUP BY by itself
	('SELECT CASE k WHEN 1 THEN ''a'' ELSE ''b'' END AS v, count(*) FROM lcc GROUP BY 1'),
	('SELECT k, (f IS TRUE) AS v, count(*) FROM lcc GROUP BY 1, 2'),
	-- a count the cells do not answer, and an aggregate other than count
	('SELECT (k = 1) AS v, count(s) FROM lcc GROUP BY 1'),
	('SELECT (k = 1) AS v, sum(n) FROM lcc GROUP BY 1'),
	-- a range in the predicates, which no entry answers alone, and a text
	-- search with a NOT, which no set of keys answers
	('SELECT (n < 10) AS v, count(*) FROM lcc GROUP BY 1'),
	('SELECT (w @@ ''a3 | !b1''::tsquery) AS v, count(*) FROM lcc GROUP BY 1'),
	-- a WHERE of no positive clause, as for the decoded walk
	('SELECT tier, count(*) FROM lcc_tiered WHERE f IS NOT NULL GROUP BY tier')
) v(q);
-- more cells than pg_lion.count_cells_max
SET pg_lion.count_cells_max = 20;
SELECT lcc_check('SELECT tier, count(*) FROM lcc_tiered GROUP BY tier');
SELECT lcc_check('SELECT (k = 1) AS x, (s = ''s1'') AS y, count(*) FROM lcc GROUP BY 1, 2');
RESET pg_lion.count_cells_max;
-- and with pg_lion.enable_count_cells off
SET pg_lion.enable_count_cells = off;
SELECT lcc_check('SELECT tier, count(*) FROM lcc_tiered GROUP BY tier');
RESET pg_lion.enable_count_cells;

-- ---------- 6. generic plans: Params in the WHERE ----------
/*
 * lcc_check_exec() is lcc_check() for a prepared statement: e EXECUTEs it,
 * and q is the same query with the values written in, which is what the
 * pushdown off and sequential scans answer - a plan cached already would
 * not see the settings change.
 */
CREATE FUNCTION lcc_check_exec(e text, q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	cells bigint := -1;
	params boolean := false;
	nrows bigint;
	ndiff bigint;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || e LOOP
		IF ln ~ '^\s*Cells: ' THEN
			cells := substring(ln FROM 'Cells: (\d+)')::bigint;
		ELSIF ln ~ '^\s*Lion Indexes: .*\$1' THEN
			params := true;
		END IF;
	END LOOP;
	EXECUTE 'CREATE TEMP TABLE lcc_on AS ' || e;
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE 'CREATE TEMP TABLE lcc_off AS ' || q;
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	SELECT count(*) INTO nrows FROM lcc_off;
	SELECT (SELECT count(*) FROM (TABLE lcc_on EXCEPT ALL TABLE lcc_off) x) +
		   (SELECT count(*) FROM (TABLE lcc_off EXCEPT ALL TABLE lcc_on) y)
	  INTO ndiff;
	DROP TABLE lcc_on, lcc_off;
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s%s, %s rows',
				  CASE WHEN cells < 0 THEN 'not cells' ELSE cells || ' cells' END,
				  CASE WHEN params THEN ' over Params' ELSE '' END, nrows);
END $$;

SET plan_cache_mode = force_generic_plan;
PREPARE lcc_p(int, text) AS
	SELECT tier, count(*) FROM lcc_tiered WHERE n < $1 AND s IS DISTINCT FROM $2
	 GROUP BY tier;
SELECT * FROM lcc_plan('EXECUTE lcc_p(50, ''s1'')') AS p("QUERY PLAN");
SELECT lcc_check_exec('EXECUTE lcc_p(10, ''s1'')',
	'SELECT tier, count(*) FROM lcc_tiered WHERE n < 10 AND s IS DISTINCT FROM ''s1'' GROUP BY tier');
SELECT lcc_check_exec('EXECUTE lcc_p(90, ''s4'')',
	'SELECT tier, count(*) FROM lcc_tiered WHERE n < 90 AND s IS DISTINCT FROM ''s4'' GROUP BY tier');
DEALLOCATE lcc_p;
PREPARE lcc_w(int) AS SELECT count(*) FROM lcc_tiered WHERE n = $1 AND tier = 'medium';
SELECT * FROM lcc_plan('EXECUTE lcc_w(3)') AS p("QUERY PLAN");
SELECT lcc_check_exec('EXECUTE lcc_w(3)',
	'SELECT count(*) FROM lcc_tiered WHERE n = 3 AND tier = ''medium''');
SELECT lcc_check_exec('EXECUTE lcc_w(4)',
	'SELECT count(*) FROM lcc_tiered WHERE n = 4 AND tier = ''medium''');
DEALLOCATE lcc_w;
/*
 * An IN list of a Param on the column the cells walk - r, which gates the
 * score - longer than the values a lookup takes at once
 * (lion_array_batch_size(), 1,000 at this work_mem): a source of the WHERE
 * like any other list, collected into one set, as it is in a decoded walk
 * of r and s.
 */
SET work_mem = '64kB';
PREPARE lcc_l(text[]) AS
	SELECT tier, count(*) FROM lcc_tiered WHERE r = ANY ($1) GROUP BY tier;
PREPARE lcc_d(text[]) AS
	SELECT r, s, count(*) FROM lcc WHERE r = ANY ($1) GROUP BY r, s;
SELECT * FROM lcc_plan('EXECUTE lcc_d(''{r1,r2}'')') AS p("QUERY PLAN");
SELECT lcc_check_exec(format('EXECUTE lcc_l(%L)', a),
		format('SELECT tier, count(*) FROM lcc_tiered WHERE r = ANY (%L::text[]) GROUP BY tier', a)) AS cells,
	   lcc_check_exec(format('EXECUTE lcc_d(%L)', a),
		format('SELECT r, s, count(*) FROM lcc WHERE r = ANY (%L::text[]) GROUP BY r, s', a)) AS decoded
  FROM (SELECT (ARRAY['r1', 'r2'] || array_agg('x' || i ORDER BY i))::text AS a
		  FROM generate_series(1, 1200) i) v;
DEALLOCATE lcc_l;
DEALLOCATE lcc_d;
RESET work_mem;
RESET plan_cache_mode;

-- a rescan for each outer row, whose value is the WHERE's; and SERIALIZABLE
SELECT lcc_check('SELECT x, t.* FROM generate_series(3, 5) x,
	LATERAL (SELECT tier, count(*) FROM lcc_tiered WHERE n = x GROUP BY tier) t');
BEGIN ISOLATION LEVEL SERIALIZABLE;
SELECT lcc_check('SELECT tier, count(*) FROM lcc_tiered WHERE n < 50 GROUP BY tier');
COMMIT;

-- ---------- 7. dead and updated rows ----------
-- deleted rows and updated ones, whose pages the visibility map no longer
-- covers: their rows are tested in the heap, before VACUUM and after none
DELETE FROM lcc WHERE id % 97 = 0;
UPDATE lcc SET k = (k + 1) % 15, s = NULL WHERE id % 89 = 0;
UPDATE lcc SET tags = '{1,9}', f = NULL WHERE id % 61 = 0;
UPDATE lcc SET r = 'r1' WHERE id % 53 = 0;
UPDATE lcc SET w = NULL, z = 9 WHERE id % 47 = 0;
SELECT q, lcc_check(q) FROM (VALUES
	('SELECT tier, count(*) FROM lcc_tiered GROUP BY tier'),
	('SELECT tier, count(*) FROM lcc_tiered WHERE n < 50 GROUP BY tier'),
	('SELECT score, count(*) FROM lcc_scored GROUP BY score'),
	('SELECT count(*) FROM lcc_tiered WHERE n < 50 AND tier = ''high''')
) v(q);
-- ... a transaction's own changes, and a row it updates twice
BEGIN;
UPDATE lcc SET k = 1, tags = '{2}' WHERE id % 31 = 0;
UPDATE lcc SET k = 2 WHERE id % 62 = 0;
DELETE FROM lcc WHERE id % 37 = 0;
SELECT lcc_check('SELECT tier, count(*) FROM lcc_tiered GROUP BY tier');
ROLLBACK;
-- ... and after VACUUM, which has set the pages all-visible again
VACUUM lcc;
SELECT q, lcc_check(q) FROM (VALUES
	('SELECT tier, count(*) FROM lcc_tiered GROUP BY tier'),
	('SELECT count(*) FROM lcc_tiered WHERE n < 50 AND tier = ''high''')
) v(q);

-- ---------- 8. in parallel ----------
-- partial counts per cell, by ranges of container keys, under the Gather
-- and the Finalize Agg (DESIGN.md §10, "A GROUP BY in parallel")
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET pg_lion.parallel_range_keys = 1;
SELECT * FROM lcc_plan(
	'SELECT tier, count(*) FROM lcc_tiered WHERE g GROUP BY tier') AS p("QUERY PLAN");
SELECT q, lcc_check(q) FROM (VALUES
	('SELECT tier, count(*) FROM lcc_tiered GROUP BY tier'),
	('SELECT tier, count(*) FROM lcc_tiered WHERE g GROUP BY tier'),
	('SELECT score, count(*) FROM lcc_scored WHERE f GROUP BY score'),
	('SELECT count(*) FROM lcc_tiered WHERE g AND tier = ''high''')
) v(q);
-- ... with rows the heap is asked about, whichever participants ask
UPDATE lcc SET k = 3 WHERE id % 41 = 0;
SELECT lcc_check('SELECT tier, count(*) FROM lcc_tiered WHERE g GROUP BY tier');
SET parallel_leader_participation = off;
SELECT lcc_check('SELECT tier, count(*) FROM lcc_tiered WHERE g GROUP BY tier');
RESET parallel_leader_participation;
/*
 * The WHERE a range collects is every clause of it, an IN list on the
 * column the cells walk - r here - as much as any other: short, of no value
 * with rows, or a Param's longer than the values a lookup takes at once.
 */
SELECT * FROM lcc_plan(
	'SELECT tier, count(*) FROM lcc_tiered WHERE r IN (''r1'', ''r2'') GROUP BY tier') AS p("QUERY PLAN");
SELECT q, lcc_check(q) FROM (VALUES
	('SELECT tier, count(*) FROM lcc_tiered WHERE r IN (''r1'', ''r2'') GROUP BY tier'),
	('SELECT tier, count(*) FROM lcc_tiered WHERE r IN (''r1'', ''r2'') AND g GROUP BY tier'),
	('SELECT count(*) FROM lcc_tiered WHERE r IN (''r0'', ''r3'', ''r9'') AND tier = ''high'''),
	('SELECT tier, count(*) FROM lcc_tiered WHERE r IN (''r8'', ''r9'') GROUP BY tier')
) v(q);
SET plan_cache_mode = force_generic_plan;
PREPARE lcc_l(text[]) AS
	SELECT tier, count(*) FROM lcc_tiered WHERE r = ANY ($1) GROUP BY tier;
SELECT * FROM lcc_plan('EXECUTE lcc_l(''{r1,r2}'')') AS p("QUERY PLAN");
SET work_mem = '64kB';
SELECT lcc_check_exec(format('EXECUTE lcc_l(%L)', a),
		format('SELECT tier, count(*) FROM lcc_tiered WHERE r = ANY (%L::text[]) GROUP BY tier', a))
  FROM (SELECT (ARRAY['r1', 'r2'] || array_agg('x' || i ORDER BY i))::text AS a
		  FROM generate_series(1, 1200) i) v;
RESET work_mem;
DEALLOCATE lcc_l;
/*
 * ... and a WHERE no range can be sure to collect stays serial: a range
 * taken as a source (DESIGN.md §32), and a multi-key query that may narrow
 * nothing, counted from every row and a recheck (§17) - a Param's, which
 * '{}' is of every array, and a literal prefix the run finds too common to
 * expand into the index's lexemes: 'p:*' of 1,500 below, against 'q:*' of
 * five, and a jsonpath, whose keys are all prefixes (§43): '$.u' of 1,500
 * values.  Beside a clause positive whatever its value, a range collects
 * that.
 */
SELECT lcc_check('SELECT tier, count(*) FROM lcc_tiered WHERE n < 50 GROUP BY tier');
PREPARE lcc_m(int[]) AS
	SELECT (k IN (1, 2)) AS x, (s = 's2') AS y, count(*) FROM lcc WHERE tags @> $1 GROUP BY 1, 2;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET pg_lion.enable_plain_scan = off;
SELECT * FROM lcc_plan('EXECUTE lcc_m(''{1}'')') AS p("QUERY PLAN");
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET pg_lion.enable_plain_scan;
SELECT lcc_check_exec(format('EXECUTE lcc_m(%L)', a),
		format('SELECT (k IN (1, 2)) AS x, (s = ''s2'') AS y, count(*) FROM lcc WHERE tags @> %L GROUP BY 1, 2', a))
  FROM (VALUES ('{}'), ('{1}')) v(a);
DEALLOCATE lcc_m;
RESET plan_cache_mode;
CREATE TABLE lcc_lex (id int NOT NULL, k int NOT NULL, s text NOT NULL, w tsvector NOT NULL,
					  d jsonb NOT NULL)
	WITH (autovacuum_enabled = off);
INSERT INTO lcc_lex
SELECT i, (h / 3) % 7, 's' || ((h / 5) % 3),
	   to_tsvector('simple', CASE WHEN h % 7 = 0 THEN '' ELSE 'p' || (h % 1500) END ||
					' q' || ((h / 11) % 5)),
	   CASE WHEN h % 7 = 0 THEN jsonb_build_object('v', h % 9)
			ELSE jsonb_build_object('u', h % 1500, 'v', h % 9) END
  FROM (SELECT i, abs(hashint4(i)) AS h FROM generate_series(1, 30000) i) v;
CREATE INDEX lcc_lex_ks ON lcc_lex USING lion (k, s);
CREATE INDEX lcc_lex_w ON lcc_lex USING lion (w) WITH (store_positions = false);
CREATE INDEX lcc_lex_d ON lcc_lex USING lion (d jsonb_contains_ops);
VACUUM (FREEZE, ANALYZE) lcc_lex;
SELECT q, lcc_check(q) FROM (VALUES
	('SELECT (k IN (1, 2)) AS x, (s = ''s1'') AS y, count(*) FROM lcc_lex WHERE w @@ ''p:*''::tsquery GROUP BY 1, 2'),
	('SELECT (k IN (1, 2)) AS x, (s = ''s1'') AS y, count(*) FROM lcc_lex WHERE w @@ ''q:*''::tsquery GROUP BY 1, 2'),
	('SELECT (k IN (1, 2)) AS x, (s = ''s1'') AS y, count(*) FROM lcc_lex WHERE w @@ ''p:*''::tsquery AND w @@ ''q1''::tsquery GROUP BY 1, 2'),
	('SELECT (k IN (1, 2)) AS x, (s = ''s1'') AS y, count(*) FROM lcc_lex WHERE d @? ''$.u'' GROUP BY 1, 2'),
	('SELECT (k IN (1, 2)) AS x, (s = ''s1'') AS y, count(*) FROM lcc_lex WHERE d @? ''$.u'' AND w @@ ''q1''::tsquery GROUP BY 1, 2')
) v(q);
DROP TABLE lcc_lex;
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET min_parallel_index_scan_size;
RESET pg_lion.parallel_range_keys;
SET max_parallel_workers_per_gather = 0;

-- ---------- 9. beside an anti join over a small inner side ----------
-- NOT EXISTS of a few rows of another table: the node looks their keys up
-- in a B-tree and takes each row found from its cell, the predicates
-- evaluated on it (DESIGN.md §27, "Counts by cells beside the anti join").
-- By id, a row a key, and by n, 400 rows a key of every cell; keys of
-- deleted rows and of none, NULL keys and duplicates.  Joins are disabled,
-- so that core has no plan the node must beat on price; lcc_check() turns
-- them on again for the reference answer.
CREATE INDEX lcc_id ON lcc (id);
CREATE INDEX lcc_nb ON lcc (n);
CREATE TABLE lcc_skip (a int, b int);
INSERT INTO lcc_skip
SELECT i * 523, CASE WHEN i % 4 = 0 THEN 0 ELSE i * 523 END FROM generate_series(1, 30) i;
INSERT INTO lcc_skip VALUES
	(7, 7), (7, 7), (11, 11), (97, 97), (194, 194), (40001, 40001), (NULL, 3), (5, NULL), (-2, -2);
VACUUM (ANALYZE) lcc, lcc_skip;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET pg_lion.enable_plain_scan = off;
SELECT * FROM lcc_plan(
	'SELECT tier, count(*) FROM lcc_tiered t WHERE NOT EXISTS (SELECT 1 FROM lcc_skip s WHERE s.a = t.id) GROUP BY tier') AS p("QUERY PLAN");
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET pg_lion.enable_plain_scan;
SELECT q, lcc_check(q) FROM (VALUES
	('SELECT tier, count(*) FROM lcc_tiered t WHERE NOT EXISTS (SELECT 1 FROM lcc_skip s WHERE s.a = t.id) GROUP BY tier'),
	('SELECT tier, count(*) FROM lcc_tiered t WHERE t.n < 50 AND NOT EXISTS (SELECT 1 FROM lcc_skip s WHERE s.a = t.id) GROUP BY tier'),
	('SELECT score, count(*) FROM lcc_scored t WHERE NOT EXISTS (SELECT 1 FROM lcc_skip s WHERE s.a = t.n) GROUP BY score'),
	('SELECT tier, count(*) FROM lcc_tiered t WHERE NOT EXISTS (SELECT 1 FROM lcc_skip s WHERE s.a = t.n) GROUP BY tier'),
	('SELECT count(*) FROM lcc_tiered t WHERE t.tier = ''high'' AND NOT EXISTS (SELECT 1 FROM lcc_skip s WHERE s.a = t.n)'),
	('SELECT count(*), count(t.n) FROM lcc_tiered t WHERE t.n < 50 AND t.tier IN (''low'', ''none'') AND NOT EXISTS (SELECT 1 FROM lcc_skip s WHERE s.a = t.id)'),
	-- multi-key columns: arrays NULL, empty and with a NULL element, and
	-- text searches, by the rows of keys of n, each in every cell
	('SELECT (tags && ''{1,2}'') AS x, (tags @> ''{3,9}'') AS y, (tags IS NULL) AS z, count(*) FROM lcc c WHERE NOT EXISTS (SELECT 1 FROM lcc_skip s WHERE s.a = c.n) GROUP BY 1, 2, 3'),
	('SELECT (w @@ ''a1 & b2''::tsquery) AS x, (k IS NULL) AS y, count(*) FROM lcc c WHERE NOT EXISTS (SELECT 1 FROM lcc_skip s WHERE s.a = c.n) GROUP BY 1, 2'),
	('SELECT (f IS UNKNOWN) AS x, NOT g AS y, (s IN (''s2'', ''s3'')) AS z, count(*) FROM lcc c WHERE NOT EXISTS (SELECT 1 FROM lcc_skip s WHERE s.a = c.n) GROUP BY 1, 2, 3'),
	-- the correlation by two of the inner side's columns, each equal to the key
	('SELECT tier, count(*) FROM lcc_tiered t WHERE NOT EXISTS (SELECT 1 FROM lcc_skip s WHERE s.a = t.id AND s.b = t.id) GROUP BY tier'),
	('SELECT tier, count(*) FROM lcc_tiered t WHERE NOT EXISTS (SELECT 1 FROM lcc_skip s WHERE s.a = t.n AND s.b = t.n) GROUP BY tier'),
	-- keys of no row: nothing taken away
	('SELECT tier, count(*) FROM lcc_tiered t WHERE NOT EXISTS (SELECT 1 FROM lcc_skip s WHERE s.a = t.id AND s.a < 0) GROUP BY tier')
) v(q);
-- Many outcomes to a dimension, of which a row taken away has the one its
-- truths are of, found in a table of the dimension's outcomes by their
-- truths, made once a run (lion_cells_code_of()).  Ten leaves of tags - the
-- most a column has, some of two keys - are 1,025 outcomes, NULL's among
-- them; by n every row of four values of n is taken away, of every mask of
-- tags, NULL arrays and arrays with a NULL element too; by id some thirty
-- rows.  The WHERE form over the ten, and a rescan for each outer row,
-- which makes the table again.
SELECT q, lcc_check(q) FROM (VALUES
	('SELECT (tags && ''{0}'') AS t0, (tags && ''{1,9}'') AS t1, (tags && ''{2}'') AS t2, (tags && ''{3,10}'') AS t3, (tags && ''{4}'') AS t4, (tags && ''{5,11}'') AS t5, (tags @> ''{6}'') AS t6, (tags && ''{7,12}'') AS t7, (tags && ''{8}'') AS t8, (tags @> ''{0,8}'') AS t9, count(*) FROM lcc c WHERE NOT EXISTS (SELECT 1 FROM lcc_skip s WHERE s.a = c.n) GROUP BY 1, 2, 3, 4, 5, 6, 7, 8, 9, 10'),
	('SELECT (tags && ''{0}'') AS t0, (tags && ''{1,9}'') AS t1, (tags && ''{2}'') AS t2, (tags && ''{3,10}'') AS t3, (tags && ''{4}'') AS t4, (tags && ''{5,11}'') AS t5, (tags @> ''{6}'') AS t6, (tags && ''{7,12}'') AS t7, (tags && ''{8}'') AS t8, (tags @> ''{0,8}'') AS t9, count(*) FROM lcc c WHERE NOT EXISTS (SELECT 1 FROM lcc_skip s WHERE s.a = c.id) GROUP BY 1, 2, 3, 4, 5, 6, 7, 8, 9, 10'),
	('SELECT count(*) FROM lcc c WHERE CASE WHEN tags && ''{0}'' THEN 1 ELSE 0 END + CASE WHEN tags && ''{1,9}'' THEN 1 ELSE 0 END + CASE WHEN tags && ''{2}'' THEN 1 ELSE 0 END + CASE WHEN tags && ''{3,10}'' THEN 1 ELSE 0 END + CASE WHEN tags && ''{4}'' THEN 1 ELSE 0 END + CASE WHEN tags && ''{5,11}'' THEN 1 ELSE 0 END + CASE WHEN tags @> ''{6}'' THEN 1 ELSE 0 END + CASE WHEN tags && ''{7,12}'' THEN 1 ELSE 0 END + CASE WHEN tags && ''{8}'' THEN 1 ELSE 0 END + CASE WHEN tags @> ''{0,8}'' THEN 1 ELSE 0 END >= 2 AND NOT EXISTS (SELECT 1 FROM lcc_skip s WHERE s.a = c.n)'),
	('SELECT x, t.* FROM (VALUES (30), (60), (30), (100)) v(x), LATERAL (SELECT (tags && ''{0}'') AS t0, (tags && ''{1,9}'') AS t1, (tags && ''{2}'') AS t2, (tags && ''{3,10}'') AS t3, (tags && ''{4}'') AS t4, (tags && ''{5,11}'') AS t5, (tags @> ''{6}'') AS t6, (tags && ''{7,12}'') AS t7, (tags && ''{8}'') AS t8, (tags @> ''{0,8}'') AS t9, count(*) FROM lcc c WHERE c.n < x AND NOT EXISTS (SELECT 1 FROM lcc_skip s WHERE s.a = c.n) GROUP BY 1, 2, 3, 4, 5, 6, 7, 8, 9, 10) t')
) v(q);
/*
 * ... and the bits of a scalar column, ten IN lists of the values from 1 to
 * 1,023 with each bit set: 1,023 constants, each an outcome of its own,
 * beside another value's and, for k, NULL's.
 */
CREATE FUNCTION lcc_bits(c text) RETURNS text
LANGUAGE sql AS $$
SELECT string_agg(format('(%s IN (%s)) AS %s%s', c, l, c, j), ', ' ORDER BY j)
  FROM (SELECT j, string_agg(v::text, ', ' ORDER BY v) AS l
		  FROM generate_series(0, 9) j, generate_series(1, 1023) v
		 WHERE v & (1 << j) <> 0 GROUP BY j) b
$$;
-- k's by n: rows taken from each of k's sixteen cells, NULL's too; n's by
-- n, whose cells of those four values come to nothing, and by id, rows of
-- some thirty values; and k's beside the ten leaves of tags, a code of two
-- such dimensions
SELECT c, lk, lcc_check(format('SELECT %s, count(*) FROM lcc c WHERE NOT EXISTS (SELECT 1 FROM lcc_skip s WHERE s.a = c.%s) GROUP BY 1, 2, 3, 4, 5, 6, 7, 8, 9, 10',
							   lcc_bits(c), lk))
  FROM (VALUES ('k', 'n'), ('n', 'n'), ('n', 'id')) v(c, lk);
SELECT lcc_check(format('SELECT (tags && ''{0}'') AS t0, (tags && ''{1,9}'') AS t1, (tags && ''{2}'') AS t2, (tags && ''{3,10}'') AS t3, (tags && ''{4}'') AS t4, (tags && ''{5,11}'') AS t5, (tags @> ''{6}'') AS t6, (tags && ''{7,12}'') AS t7, (tags && ''{8}'') AS t8, (tags @> ''{0,8}'') AS t9, %s, count(*) FROM lcc c WHERE NOT EXISTS (SELECT 1 FROM lcc_skip s WHERE s.a = c.n) GROUP BY 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20',
						lcc_bits('k')));
-- the rows changed: before VACUUM, in this very transaction, and after
UPDATE lcc SET k = 1, tags = '{1}' WHERE id IN (523, 1046, 1569);
UPDATE lcc SET f = NULL, z = 9 WHERE n = 7 AND id % 3 = 0;
DELETE FROM lcc WHERE n = 11 AND id % 5 = 0;
SELECT q, lcc_check(q) FROM (VALUES
	('SELECT tier, count(*) FROM lcc_tiered t WHERE NOT EXISTS (SELECT 1 FROM lcc_skip s WHERE s.a = t.id) GROUP BY tier'),
	('SELECT tier, count(*) FROM lcc_tiered t WHERE NOT EXISTS (SELECT 1 FROM lcc_skip s WHERE s.a = t.n) GROUP BY tier')
) v(q);
BEGIN;
UPDATE lcc SET k = 2, s = NULL WHERE id IN (2092, 2615) OR (n = 7 AND id % 4 = 0);
DELETE FROM lcc WHERE n = 11 AND id % 7 = 0;
SELECT q, lcc_check(q) FROM (VALUES
	('SELECT tier, count(*) FROM lcc_tiered t WHERE NOT EXISTS (SELECT 1 FROM lcc_skip s WHERE s.a = t.id) GROUP BY tier'),
	('SELECT tier, count(*) FROM lcc_tiered t WHERE NOT EXISTS (SELECT 1 FROM lcc_skip s WHERE s.a = t.n) GROUP BY tier')
) v(q);
ROLLBACK;
VACUUM lcc;
SELECT lcc_check('SELECT tier, count(*) FROM lcc_tiered t WHERE NOT EXISTS (SELECT 1 FROM lcc_skip s WHERE s.a = t.n) GROUP BY tier');
-- in parallel: every participant looks the keys up, and takes away the rows
-- of the ranges it counts.  Without the leader, whose run of the lookups the
-- workers' share of the count would otherwise have to outweigh, as the
-- shares of 40,000 rows do not the runs of n's keys, some 400 rows each
-- (lion_add_parallel_cells_path())
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET pg_lion.parallel_range_keys = 1;
SET parallel_leader_participation = off;
SELECT q, lcc_check(q) FROM (VALUES
	('SELECT tier, count(*) FROM lcc_tiered t WHERE NOT EXISTS (SELECT 1 FROM lcc_skip s WHERE s.a = t.id) GROUP BY tier'),
	('SELECT tier, count(*) FROM lcc_tiered t WHERE NOT EXISTS (SELECT 1 FROM lcc_skip s WHERE s.a = t.n) GROUP BY tier'),
	('SELECT (tags && ''{1,2}'') AS x, (tags IS NULL) AS y, count(*) FROM lcc c WHERE c.g AND NOT EXISTS (SELECT 1 FROM lcc_skip s WHERE s.a = c.n) GROUP BY 1, 2'),
	('SELECT tier, count(*) FROM lcc_tiered t WHERE t.g AND NOT EXISTS (SELECT 1 FROM lcc_skip s WHERE s.a = t.id AND s.b = t.id) GROUP BY tier'),
	-- an IN list on r, the column the cells walk, in the WHERE beside it
	('SELECT tier, count(*) FROM lcc_tiered t WHERE t.r IN (''r1'', ''r2'') AND NOT EXISTS (SELECT 1 FROM lcc_skip s WHERE s.a = t.id) GROUP BY tier'),
	('SELECT tier, count(*) FROM lcc_tiered t WHERE t.r IN (''r1'', ''r3'') AND NOT EXISTS (SELECT 1 FROM lcc_skip s WHERE s.a = t.n) GROUP BY tier')
) v(q);
-- ... and many outcomes to a dimension: each participant makes the table of
-- them for the rows it takes away
SELECT lcc_check('SELECT (tags && ''{0}'') AS t0, (tags && ''{1,9}'') AS t1, (tags && ''{2}'') AS t2, (tags && ''{3,10}'') AS t3, (tags && ''{4}'') AS t4, (tags && ''{5,11}'') AS t5, (tags @> ''{6}'') AS t6, (tags && ''{7,12}'') AS t7, (tags && ''{8}'') AS t8, (tags @> ''{0,8}'') AS t9, count(*) FROM lcc c WHERE NOT EXISTS (SELECT 1 FROM lcc_skip s WHERE s.a = c.n) GROUP BY 1, 2, 3, 4, 5, 6, 7, 8, 9, 10');
SELECT c, lcc_check(format('SELECT %s, count(*) FROM lcc c WHERE NOT EXISTS (SELECT 1 FROM lcc_skip s WHERE s.a = c.n) GROUP BY 1, 2, 3, 4, 5, 6, 7, 8, 9, 10',
						   lcc_bits(c)))
  FROM (VALUES ('k'), ('n')) v(c);
RESET parallel_leader_participation;
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET min_parallel_index_scan_size;
RESET pg_lion.parallel_range_keys;
SET max_parallel_workers_per_gather = 0;
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_nestloop;
DROP TABLE lcc_skip;

-- ---------- 10. enums ----------
-- enum_ops is declared FOR TYPE anyenum, its equality = (anyenum, anyenum),
-- while a leaf compares the column with a constant of the column's own
-- enum - and a domain over an enum, which the parser takes cast to its
-- enum, with one of that enum's.  The values are the column's own
-- (cells_dim_valop()), for =, <>, IN, NOT IN and IS DISTINCT FROM alike.
CREATE TYPE lcc_mood AS ENUM ('live', 'dead', 'idle', 'gone');
CREATE TYPE lcc_tone AS ENUM ('hot', 'cold');
CREATE DOMAIN lcc_dmood AS lcc_mood;
/*
 * 20,000 rows: s of the four moods and NULLs, m of the two tones and no
 * NULL, dm the domain over the moods with NULLs and no row of 'gone', r of 4
 * texts, f a boolean with NULLs and n of 100 values.
 */
CREATE TABLE lcc_e (id int NOT NULL, s lcc_mood, m lcc_tone NOT NULL,
					dm lcc_dmood, r text NOT NULL, f bool, n int NOT NULL)
	WITH (autovacuum_enabled = off);
INSERT INTO lcc_e
SELECT i,
	   CASE WHEN h % 13 = 0 THEN NULL
			ELSE (enum_range(NULL::lcc_mood))[1 + (h / 3) % 4] END,
	   (enum_range(NULL::lcc_tone))[1 + (h / 7) % 2],
	   CASE WHEN h % 11 = 0 THEN NULL
			ELSE (enum_range(NULL::lcc_mood))[1 + (h / 5) % 3] END,
	   'r' || ((h / 17) % 4),
	   CASE WHEN h % 9 = 0 THEN NULL ELSE (h / 19) % 2 = 0 END,
	   (h / 23) % 100
  FROM (SELECT i, abs(hashint4(i)) AS h FROM generate_series(1, 20000) i) q;
CREATE INDEX lcc_e_sm ON lcc_e USING lion (s, m);
CREATE INDEX lcc_e_dm ON lcc_e USING lion (dm);
CREATE INDEX lcc_e_r ON lcc_e USING lion (r);
CREATE INDEX lcc_e_f ON lcc_e USING lion (f);
CREATE INDEX lcc_e_n ON lcc_e USING lion (n);
VACUUM (FREEZE, ANALYZE) lcc_e;

-- A score gated on a mood: points from the tone, r, f and dm for the rows
-- live or of no mood, 2 for the others but 'gone', NULL for those.  The
-- tier is a CASE over it.
CREATE VIEW lcc_e_scored AS
SELECT id, n, s, m, dm, r, f,
	   CASE WHEN s = 'live' OR s IS NULL THEN
			CASE WHEN m = 'hot' THEN 20 ELSE 5 END
			+ CASE WHEN r IN ('r1', 'r2') THEN 10 WHEN r = 'r3' THEN 4 ELSE 0 END
			+ CASE WHEN f THEN 7 WHEN f IS NULL THEN 1 ELSE 0 END
			+ CASE WHEN dm::lcc_mood IN ('dead', 'idle') THEN 3
				   WHEN dm IS NULL THEN 1 ELSE 0 END
			WHEN s IS DISTINCT FROM 'gone' THEN 2
	   END AS score
  FROM lcc_e;
CREATE VIEW lcc_e_tiered AS
SELECT id, n, s, m, dm, r, f, score,
	   CASE WHEN score >= 30 THEN 'high' WHEN score >= 15 THEN 'medium'
			WHEN score IS NOT NULL THEN 'low' ELSE 'none' END AS tier
  FROM lcc_e_scored;

-- the plans: each enum column a dimension of its own, the domain's too
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET pg_lion.enable_plain_scan = off;
SELECT * FROM lion_explain_norm($$
SELECT CASE WHEN s = 'live' AND r = 'r0' THEN 'a' ELSE 'c' END AS t, count(*)
  FROM lcc_e WHERE r IN ('r0', 'r1') GROUP BY 1$$) AS p("QUERY PLAN");
SELECT * FROM lcc_plan(
	'SELECT tier, count(*) FROM lcc_e_tiered WHERE n < 50 GROUP BY tier') AS p("QUERY PLAN");
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET pg_lion.enable_plain_scan;

SELECT q, lcc_check(q) FROM (VALUES
	('SELECT CASE WHEN s = ''live'' AND r = ''r0'' THEN ''a'' ELSE ''c'' END AS t, count(*)
		FROM lcc_e WHERE r IN (''r0'', ''r1'') GROUP BY 1'),
	-- every value of the column named: one dimension, NULL's outcome too
	('SELECT CASE WHEN s = ''live'' THEN 1 WHEN s = ''dead'' THEN 2 WHEN s = ''idle'' THEN 3
				  WHEN s = ''gone'' THEN 4 END AS t, count(*) FROM lcc_e GROUP BY 1'),
	-- =, <>, and the constant on the left
	('SELECT (s = ''live'') AS x, (s <> ''dead'') AS y, (''idle'' = s) AS z, count(*)
		FROM lcc_e GROUP BY 1, 2, 3'),
	-- IN and NOT IN, and with a NULL element
	('SELECT (s IN (''live'', ''idle'')) AS x, (s NOT IN (''dead'', ''gone'')) AS y, count(*)
		FROM lcc_e GROUP BY 1, 2'),
	('SELECT (s IN (''live'', NULL)) AS x, (s NOT IN (''dead'', NULL)) AS y, count(*)
		FROM lcc_e GROUP BY 1, 2'),
	-- IS [NOT] DISTINCT FROM a value and NULL, and null tests
	('SELECT (s IS DISTINCT FROM ''live'') AS x, (s IS NOT DISTINCT FROM ''idle'') AS y,
			 (s IS DISTINCT FROM NULL) AS z, count(*) FROM lcc_e GROUP BY 1, 2, 3'),
	('SELECT (s IS NULL) AS x, (s = ''gone'' OR s IS NULL) AS y, count(*) FROM lcc_e GROUP BY 1, 2'),
	-- a column of no NULL, both of whose values are named
	('SELECT CASE WHEN m = ''hot'' THEN ''h'' WHEN m = ''cold'' THEN ''c'' END AS t,
			 (m <> ''hot'') AS u, count(*) FROM lcc_e GROUP BY 1, 2'),
	-- the domain over the enum: a value no row has, and NULLs
	('SELECT (dm::lcc_mood = ''live'') AS x, (dm::lcc_mood IN (''dead'', ''gone'')) AS y,
			 (dm::lcc_mood IS DISTINCT FROM ''idle'') AS z, (dm IS NULL) AS w, count(*)
		FROM lcc_e GROUP BY 1, 2, 3, 4'),
	-- two enum columns, a text and a boolean, ANDed and ORed: 3-valued
	('SELECT (s = ''live'' AND m = ''hot'') AS x, (s IN (''dead'', ''idle'') OR f) AS y,
			 (m = ''cold'' AND r = ''r1'') AS z, count(*) FROM lcc_e GROUP BY 1, 2, 3'),
	-- the score and its tier
	('SELECT tier, count(*) FROM lcc_e_tiered GROUP BY tier'),
	('SELECT tier, count(*) FROM lcc_e_tiered WHERE n < 50 GROUP BY tier'),
	('SELECT score, count(*) FROM lcc_e_scored WHERE r IN (''r0'', ''r1'') GROUP BY score'),
	('SELECT score, tier, count(*) AS c FROM lcc_e_tiered WHERE m = ''hot'' GROUP BY score, tier'),
	-- the WHERE form
	('SELECT count(*) FROM lcc_e_tiered WHERE tier = ''high'''),
	('SELECT count(*), count(n) FROM lcc_e_tiered WHERE n < 50 AND tier IN (''low'', ''none'')'),
	('SELECT count(*) FROM lcc_e WHERE n < 50 AND CASE WHEN s = ''live'' THEN f ELSE m = ''hot'' END'),
	('SELECT (dm IS NULL) AS x, count(*) FROM lcc_e_tiered WHERE tier = ''medium'' GROUP BY 1'),
	-- the enum's order, which no entry answers alone: declined
	('SELECT (s < ''idle'') AS x, count(*) FROM lcc_e GROUP BY 1')
) v(q);

-- rows changed, whose pages the visibility map no longer covers, before
-- VACUUM and in a transaction of their own
UPDATE lcc_e SET s = 'live', dm = NULL WHERE id % 89 = 0;
DELETE FROM lcc_e WHERE id % 97 = 0;
SELECT lcc_check('SELECT tier, count(*) FROM lcc_e_tiered GROUP BY tier');
BEGIN;
UPDATE lcc_e SET s = NULL, m = 'cold' WHERE id % 31 = 0;
SELECT lcc_check('SELECT tier, count(*) FROM lcc_e_tiered GROUP BY tier');
ROLLBACK;
VACUUM lcc_e;

-- beside an anti join over a small inner side, by id and by n: the
-- predicates evaluated on each row taken away
CREATE INDEX lcc_e_id ON lcc_e (id);
CREATE INDEX lcc_e_nb ON lcc_e (n);
CREATE TABLE lcc_e_skip (a int);
INSERT INTO lcc_e_skip SELECT i * 337 FROM generate_series(1, 30) i;
INSERT INTO lcc_e_skip VALUES (7), (7), (11), (42), (NULL), (-2);
VACUUM (ANALYZE) lcc_e, lcc_e_skip;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET pg_lion.enable_plain_scan = off;
SELECT * FROM lcc_plan(
	'SELECT tier, count(*) FROM lcc_e_tiered t WHERE NOT EXISTS (SELECT 1 FROM lcc_e_skip k WHERE k.a = t.id) GROUP BY tier') AS p("QUERY PLAN");
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET pg_lion.enable_plain_scan;
SELECT q, lcc_check(q) FROM (VALUES
	('SELECT tier, count(*) FROM lcc_e_tiered t WHERE NOT EXISTS (SELECT 1 FROM lcc_e_skip k WHERE k.a = t.id) GROUP BY tier'),
	('SELECT tier, count(*) FROM lcc_e_tiered t WHERE NOT EXISTS (SELECT 1 FROM lcc_e_skip k WHERE k.a = t.n) GROUP BY tier'),
	('SELECT count(*) FROM lcc_e_tiered t WHERE t.tier = ''high'' AND NOT EXISTS (SELECT 1 FROM lcc_e_skip k WHERE k.a = t.n)'),
	('SELECT (s IN (''live'', NULL)) AS x, (dm::lcc_mood IS DISTINCT FROM ''dead'') AS y, (m = ''hot'') AS z, count(*) FROM lcc_e c WHERE NOT EXISTS (SELECT 1 FROM lcc_e_skip k WHERE k.a = c.n) GROUP BY 1, 2, 3')
) v(q);

-- in parallel, beside the anti join too (without the leader, as above)
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET pg_lion.parallel_range_keys = 1;
SET parallel_leader_participation = off;
SELECT * FROM lcc_plan(
	'SELECT tier, count(*) FROM lcc_e_tiered WHERE m = ''hot'' GROUP BY tier') AS p("QUERY PLAN");
SELECT q, lcc_check(q) FROM (VALUES
	('SELECT tier, count(*) FROM lcc_e_tiered GROUP BY tier'),
	('SELECT tier, count(*) FROM lcc_e_tiered WHERE m = ''hot'' GROUP BY tier'),
	('SELECT count(*) FROM lcc_e_tiered WHERE m = ''hot'' AND tier = ''high'''),
	('SELECT tier, count(*) FROM lcc_e_tiered t WHERE NOT EXISTS (SELECT 1 FROM lcc_e_skip k WHERE k.a = t.n) GROUP BY tier'),
	('SELECT tier, count(*) FROM lcc_e_tiered t WHERE t.m = ''cold'' AND NOT EXISTS (SELECT 1 FROM lcc_e_skip k WHERE k.a = t.id) GROUP BY tier')
) v(q);
-- an IN list on a column the score tests, the one the cells walk: a text's
-- and an enum's, alone and beside the anti join
SELECT * FROM lcc_plan(
	'SELECT tier, count(*) FROM lcc_e_tiered WHERE r IN (''r1'', ''r2'') GROUP BY tier') AS p("QUERY PLAN");
SELECT q, lcc_check(q) FROM (VALUES
	('SELECT tier, count(*) FROM lcc_e_tiered WHERE r IN (''r1'', ''r2'') GROUP BY tier'),
	('SELECT tier, count(*) FROM lcc_e_tiered t WHERE t.r IN (''r1'', ''r2'') AND NOT EXISTS (SELECT 1 FROM lcc_e_skip k WHERE k.a = t.id) GROUP BY tier'),
	('SELECT tier, count(*) FROM lcc_e_tiered t WHERE t.r IN (''r0'', ''r3'') AND NOT EXISTS (SELECT 1 FROM lcc_e_skip k WHERE k.a = t.n) GROUP BY tier'),
	('SELECT tier, count(*) FROM lcc_e_tiered WHERE s IN (''live'', ''idle'') GROUP BY tier'),
	('SELECT tier, count(*) FROM lcc_e_tiered t WHERE t.s IN (''live'', ''gone'') AND NOT EXISTS (SELECT 1 FROM lcc_e_skip k WHERE k.a = t.id) GROUP BY tier')
) v(q);
RESET parallel_leader_participation;
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET min_parallel_index_scan_size;
RESET pg_lion.parallel_range_keys;
SET max_parallel_workers_per_gather = 0;
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_nestloop;
DROP VIEW lcc_e_tiered;
DROP VIEW lcc_e_scored;
DROP TABLE lcc_e, lcc_e_skip;
DROP DOMAIN lcc_dmood;
DROP TYPE lcc_mood, lcc_tone;

DROP VIEW lcc_tiered;
DROP VIEW lcc_scored;
DROP TABLE lcc;
DROP FUNCTION lcc_bits(text);
DROP FUNCTION lcc_check(text);
DROP FUNCTION lcc_check_exec(text, text);
DROP FUNCTION lcc_plan(text);
