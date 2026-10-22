-- Empty arrays with dimensions (DESIGN.md §17, "The reserved EMPTY entry"
-- and "Non-empty arrays").
--
-- PostgreSQL makes an array with no element without dimensions nearly
-- everywhere, '{}', but not an empty int2vector or oidvector: one
-- dimension of length 0, `[0:-1]`, which its cast to int2[] or oid[] and
-- array_cat() keep, and ARRAY[] of such arrays has more, `[1:2][0:-1]`.
-- Neither is equal to '{}', and array_length(), array_ndims(),
-- array_lower() and array_upper() of either are not NULL, while
-- cardinality() is 0.  A lion array column keeps them in reserved entries of
-- their own, EMPTYDIM (a first dimension of length 0) and EMPTYINNER (a first
-- dimension with a length and an inner one without), apart from the EMPTY
-- entry of '{}', so that every test of whether an array is empty that the
-- count pushdown answers from the reserved entries gives core's answer for
-- each.  Every query is run through the pushdown, with every other scan
-- disabled, and again with the pushdown off and sequential scans only, which
-- read no lion posting set; the two results are compared as multisets both
-- ways round.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET default_statistics_target = 1000;
SET max_parallel_workers_per_gather = 0;

/*
 * lion_ned() runs q as planned with every scan but the pushdown disabled,
 * so that a LionCount path that is built at all is the plan; again with
 * max_parallel_workers_per_gather at 0 when that plan was parallel; and with
 * the pushdown off and sequential scans only.  It says which plan ran -
 * the parallel node, the serial one with or without a residual filter
 * (DESIGN.md §10), or neither - and the result: the row, when there is one,
 * or how many.  With args, q is PREPAREd and EXECUTEd with them under a
 * generic plan.
 */
CREATE FUNCTION lion_ned(q text, args text DEFAULT NULL) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	ex text := q;
	how text := 'not pushed';
	residual boolean := false;
	workers text := current_setting('max_parallel_workers_per_gather');
	nrows bigint;
	ndiff bigint := 0;
BEGIN
	PERFORM set_config('plan_cache_mode', 'force_generic_plan', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	IF args IS NOT NULL THEN
		EXECUTE 'PREPARE lion_ned_q AS ' || q;
		ex := 'EXECUTE lion_ned_q(' || args || ')';
	END IF;
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || ex LOOP
		IF ln LIKE '%Parallel Custom Scan (LionCount)%' THEN
			how := 'parallel LionCount';
		ELSIF ln LIKE '%Custom Scan (LionCount)%' AND how = 'not pushed' THEN
			how := 'LionCount';
		ELSIF ln LIKE '%Residual Filter%' THEN
			residual := true;
		END IF;
	END LOOP;
	EXECUTE 'CREATE TEMP TABLE lion_ned_on AS SELECT s::text AS r FROM (' ||
		CASE WHEN args IS NULL THEN q ELSE 'SELECT 1' END || ') s';
	IF args IS NOT NULL THEN
		DROP TABLE lion_ned_on;
		EXECUTE 'CREATE TEMP TABLE lion_ned_on AS ' || ex;
	END IF;
	IF how LIKE 'parallel%' THEN
		PERFORM set_config('max_parallel_workers_per_gather', '0', true);
		EXECUTE 'CREATE TEMP TABLE lion_ned_ser AS ' ||
			CASE WHEN args IS NULL THEN 'SELECT s::text AS r FROM (' || q || ') s'
				 ELSE ex END;
		PERFORM set_config('max_parallel_workers_per_gather', workers, true);
		SELECT (SELECT count(*) FROM (TABLE lion_ned_on EXCEPT ALL TABLE lion_ned_ser) a) +
			   (SELECT count(*) FROM (TABLE lion_ned_ser EXCEPT ALL TABLE lion_ned_on) b)
		  INTO ndiff;
		DROP TABLE lion_ned_ser;
	END IF;
	IF args IS NOT NULL THEN
		EXECUTE 'DEALLOCATE lion_ned_q';
	END IF;

	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	IF args IS NOT NULL THEN
		EXECUTE 'PREPARE lion_ned_q AS ' || q;
		EXECUTE 'CREATE TEMP TABLE lion_ned_off AS EXECUTE lion_ned_q(' ||
			args || ')';
		EXECUTE 'DEALLOCATE lion_ned_q';
	ELSE
		EXECUTE 'CREATE TEMP TABLE lion_ned_off AS SELECT s::text AS r FROM (' ||
			q || ') s';
	END IF;
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('plan_cache_mode', 'auto', true);

	IF ndiff <> 0 THEN
		DROP TABLE lion_ned_on, lion_ned_off;
		RETURN format('MISMATCH: %s rows differ from the serial count', ndiff);
	END IF;
	SELECT count(*) INTO nrows FROM lion_ned_off;
	SELECT (SELECT count(*) FROM (TABLE lion_ned_on EXCEPT ALL TABLE lion_ned_off) a) +
		   (SELECT count(*) FROM (TABLE lion_ned_off EXCEPT ALL TABLE lion_ned_on) b)
	  INTO ndiff;
	DROP TABLE lion_ned_on;
	IF ndiff <> 0 THEN
		DROP TABLE lion_ned_off;
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	IF nrows = 1 AND args IS NULL THEN
		SELECT r INTO ln FROM lion_ned_off;
	ELSIF nrows = 1 THEN
		SELECT s::text INTO ln FROM lion_ned_off s;
	ELSE
		ln := nrows || ' rows';
	END IF;
	DROP TABLE lion_ned_off;
	IF residual THEN
		how := how || ' with a residual filter';
	END IF;
	RETURN how || ': ' || ln;
END $$;

/*
 * The count of each predicate of preds on the table, beside `k = 3`, which
 * a lion index answers, and alone - where nothing else selects rows and the
 * count is a sum over k's entries less those the predicate rejects - each
 * through lion_ned().
 */
CREATE FUNCTION lion_ned_each(preds text[], rel text DEFAULT 'lion_ned_t')
RETURNS TABLE (pred text, with_k text, alone text)
LANGUAGE plpgsql AS $$
DECLARE
	p text;
BEGIN
	FOREACH p IN ARRAY preds LOOP
		pred := p;
		with_k := lion_ned(format('SELECT count(*) FROM %s WHERE k = 3 AND %s', rel, p));
		alone := lion_ned(format('SELECT count(*) FROM %s WHERE %s', rel, p));
		RETURN NEXT;
	END LOOP;
END $$;

/* The plan of q, as lion_ned() plans it, without 18's "Disabled" lines. */
CREATE FUNCTION lion_ned_plan(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		CONTINUE WHEN ln ~ '^\s*Disabled: true$';
		RETURN NEXT ln;
	END LOOP;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
END $$;

-- ---- the data ----------------------------------------------------------
/*
 * a is an int2[] that runs through every class by id % 11: NULL (0), '{}'
 * (1), an empty int2vector cast, `[0:-1]` (2), array_cat() of two, still
 * `[0:-1]` (3), ARRAY[] of two, `[1:2][0:-1]` (4), of one in three levels,
 * `[1:1][1:1][0:-1]` (5), a non-empty int2vector cast, `[0:1]={1,2}` (6),
 * '{NULL}' (7), a two-dimensional array (8) and one element otherwise.  o
 * is an oid[], NULL, '{}', an empty oidvector cast or one element by
 * id % 7.  k and g are scalar columns to count by.
 */
CREATE TABLE lion_ned_t (
	id		int NOT NULL,
	k		int NOT NULL,
	g		int NOT NULL,
	a		int2[],
	o		oid[],
	pad		text NOT NULL
) WITH (autovacuum_enabled = off);
INSERT INTO lion_ned_t
SELECT i, i % 10, i % 4,
	   CASE i % 11
			WHEN 0 THEN NULL
			WHEN 1 THEN '{}'::int2[]
			WHEN 2 THEN ''::int2vector::int2[]
			WHEN 3 THEN array_cat(''::int2vector::int2[], ''::int2vector::int2[])
			WHEN 4 THEN ARRAY[''::int2vector::int2[], ''::int2vector::int2[]]
			WHEN 5 THEN ARRAY[ARRAY[''::int2vector::int2[]]]
			WHEN 6 THEN '1 2'::int2vector::int2[]
			WHEN 7 THEN '{NULL}'::int2[]
			WHEN 8 THEN ARRAY[ARRAY[(i % 3)::int2, 5::int2], ARRAY[NULL, 1::int2]]
			ELSE ARRAY[(i % 5)::int2]
	   END,
	   CASE i % 7
			WHEN 0 THEN NULL
			WHEN 1 THEN '{}'::oid[]
			WHEN 2 THEN ''::oidvector::oid[]
			ELSE ARRAY[(i % 5)::oid]
	   END,
	   repeat('x', 40)
  FROM generate_series(1, 44000) i;
CREATE INDEX lion_ned_k ON lion_ned_t USING lion (k);
CREATE INDEX lion_ned_g ON lion_ned_t USING lion (g);
CREATE INDEX lion_ned_a ON lion_ned_t USING lion (a);
CREATE INDEX lion_ned_o ON lion_ned_t USING lion (o);
VACUUM (FREEZE, ANALYZE) lion_ned_t;

-- ---- 1. the classes are the reserved entries ---------------------------
-- '{}' in EMPTY, a first dimension of length 0 in EMPTYDIM, a first
-- dimension with a length and an inner one without in EMPTYINNER, as built
SELECT null_tids = (SELECT count(*) FROM lion_ned_t WHERE a IS NULL) AS nulls,
	   empty_tids = (SELECT count(*) FROM lion_ned_t WHERE a = '{}') AS empty,
	   emptydim_tids = (SELECT count(*) FROM lion_ned_t
						 WHERE array_length(a, 1) = 0) AS emptydim,
	   emptyinner_tids = (SELECT count(*) FROM lion_ned_t
						   WHERE cardinality(a) = 0 AND array_length(a, 1) > 0) AS emptyinner,
	   null_tids, empty_tids, emptydim_tids, emptyinner_tids, nullelem_tids
  FROM lion_index_stats('lion_ned_a');
SELECT null_tids, empty_tids, emptydim_tids, emptyinner_tids, nullelem_tids
  FROM lion_index_stats('lion_ned_o');
-- a scalar column has none of them
SELECT null_tids, empty_tids, emptydim_tids, emptyinner_tids, nullelem_tids
  FROM lion_index_stats('lion_ned_k');
SELECT lion_index_verify('lion_ned_a', true), lion_index_verify('lion_ned_o', true);

-- ---- 2. every spelling, against core ------------------------------------
-- Those true or false of every row of each class are answered from the
-- reserved entries; those an empty array with dimensions passes and '{}'
-- fails, or the reverse, are among them.  Those true of some rows of a class
-- and not of others - `array_length(a, 1) > 1`, `array_ndims(a) = 1` - and
-- a comparison with an empty constant that has dimensions of its own are
-- left to the heap, or to the ordinary plan.
SELECT * FROM lion_ned_each(ARRAY[
	$$a = '{}'$$, $$'{}' = a$$, $$a <> '{}'$$, $$NOT (a = '{}')$$,
	$$a IS DISTINCT FROM '{}'$$, $$a IS NOT DISTINCT FROM '{}'$$,
	$$cardinality(a) = 0$$, $$cardinality(a) > 0$$, $$cardinality(a) < 1$$,
	$$cardinality(a) IS DISTINCT FROM 0$$, $$coalesce(cardinality(a), 0) = 0$$,
	$$cardinality(a) IS NULL$$, $$cardinality(a) >= 0$$,
	$$array_length(a, 1) IS NULL$$, $$array_length(a, 1) IS NOT NULL$$,
	$$array_length(a, 1) = 0$$, $$array_length(a, 1) > 0$$,
	$$array_length(a, 1) >= 0$$, $$array_length(a, 1) >= 1$$,
	$$array_length(a, 1) <> 0$$, $$array_length(a, 1) <= 0$$,
	$$array_length(a, 1) IS DISTINCT FROM 0$$,
	$$coalesce(array_length(a, 1), 0) = 0$$,
	$$coalesce(array_length(a, 1), 0) > 0$$,
	$$coalesce(array_length(a, 1), -1) = 0$$,
	$$NOT (array_length(a, 1) = 0)$$, $$(array_length(a, 1) = 0) IS NOT TRUE$$,
	$$(array_length(a, 1) > 0) IS FALSE$$,
	$$array_ndims(a) IS NULL$$, $$array_ndims(a) IS NOT NULL$$,
	$$array_ndims(a) > 0$$, $$array_ndims(a) >= 1$$,
	$$array_lower(a, 1) IS NULL$$, $$array_upper(a, 1) IS NOT NULL$$,
	$$a IS NULL$$, $$a IS NOT NULL$$,
	-- several on one column: one set of classes
	$$a <> '{}' AND cardinality(a) = 0$$,
	$$array_length(a, 1) IS NOT NULL AND cardinality(a) = 0$$,
	$$a IS NOT NULL AND coalesce(array_length(a, 1), 0) = 0$$,
	$$a IS NOT NULL AND array_length(a, 1) IS NULL$$,
	$$array_length(a, 1) > 0 AND cardinality(a) = 0$$,
	$$cardinality(a) = 0 AND array_length(a, 1) IS DISTINCT FROM 0$$,
	-- left to the heap
	$$array_length(a, 1) > 1$$, $$array_length(a, 1) = 2$$,
	$$array_ndims(a) = 1$$, $$array_ndims(a) > 1$$, $$array_ndims(a) >= 2$$,
	$$array_lower(a, 1) = 0$$, $$array_length(a, 2) = 0$$,
	$$a = ''::int2vector::int2[]$$, $$a <> ''::int2vector::int2[]$$,
	$$a IS DISTINCT FROM ''::int2vector::int2[]$$,
	$$a = ARRAY[''::int2vector::int2[], ''::int2vector::int2[]]$$,
	-- the multi-key operators, which compare elements alone
	$$a @> '{}'$$, $$a <@ '{}'$$, $$a && '{}'$$,
	$$a @> ''::int2vector::int2[]$$, $$a <@ ''::int2vector::int2[]$$,
	$$a && ''::int2vector::int2[]$$, $$a @> '{1}'$$, $$a <@ '{1,2}'$$
]);
-- oid[]: an empty oidvector cast
SELECT * FROM lion_ned_each(ARRAY[
	$$o = '{}'$$, $$o <> '{}'$$, $$o IS DISTINCT FROM '{}'$$,
	$$cardinality(o) = 0$$, $$cardinality(o) > 0$$,
	$$array_length(o, 1) = 0$$, $$array_length(o, 1) IS NULL$$,
	$$array_length(o, 1) IS NOT NULL$$, $$o = ''::oidvector::oid[]$$,
	$$o <@ '{}'$$
]);
-- two columns at once
SELECT lion_ned($$SELECT count(*) FROM lion_ned_t WHERE a <> '{}' AND o = '{}'$$);
SELECT lion_ned($$SELECT count(*) FROM lion_ned_t WHERE g = 1 AND array_length(a, 1) = 0 AND cardinality(o) > 0$$);

-- ---- 3. the plans --------------------------------------------------------
-- One clause on the array's index, in the spelling of the classes it
-- selects; a set with no short spelling is the OR of its classes'.
SELECT lion_ned_plan($$SELECT count(*) FROM lion_ned_t WHERE k = 3 AND a = '{}'$$);
SELECT lion_ned_plan($$SELECT count(*) FROM lion_ned_t WHERE k = 3 AND a <> '{}'$$);
SELECT lion_ned_plan($$SELECT count(*) FROM lion_ned_t WHERE k = 3 AND cardinality(a) = 0$$);
SELECT lion_ned_plan($$SELECT count(*) FROM lion_ned_t WHERE k = 3 AND array_length(a, 1) = 0$$);
SELECT lion_ned_plan($$SELECT count(*) FROM lion_ned_t WHERE k = 3 AND array_length(a, 1) > 0$$);
SELECT lion_ned_plan($$SELECT count(*) FROM lion_ned_t WHERE k = 3 AND coalesce(array_length(a, 1), 0) = 0$$);
SELECT lion_ned_plan($$SELECT count(*) FROM lion_ned_t WHERE k = 3 AND array_length(a, 1) IS DISTINCT FROM 0$$);
SELECT lion_ned_plan($$SELECT count(*) FROM lion_ned_t WHERE k = 3 AND array_length(a, 1) IS NOT NULL AND cardinality(a) = 0$$);
SELECT lion_ned_plan($$SELECT count(*) FROM lion_ned_t WHERE k = 3 AND a IS NOT NULL AND coalesce(array_length(a, 1), 0) = 0$$);
SELECT lion_ned_plan($$SELECT count(*) FROM lion_ned_t WHERE k = 3 AND (a IS NULL OR array_length(a, 1) > 0) AND cardinality(a) = 0$$);
SELECT lion_ned_plan($$SELECT count(*) FROM lion_ned_t WHERE k = 3 AND a = ''::int2vector::int2[]$$);

-- ---- 4. grouped, filtered, ORed ------------------------------------------
SELECT lion_ned($$SELECT k, count(*) FROM lion_ned_t WHERE a <> '{}' GROUP BY k$$);
SELECT lion_ned($$SELECT g, count(*) FROM lion_ned_t WHERE array_length(a, 1) = 0 GROUP BY g$$);
SELECT lion_ned($$SELECT k, g, count(*) FROM lion_ned_t WHERE a IS DISTINCT FROM '{}' GROUP BY k, g$$);
SELECT lion_ned($$SELECT g, count(*) FILTER (WHERE a = '{}') AS empty,
						count(*) FILTER (WHERE cardinality(a) = 0) AS noelem,
						count(*) FILTER (WHERE array_length(a, 1) = 0) AS dim,
						count(*) FILTER (WHERE array_length(a, 1) > 0) AS len,
						count(*) FILTER (WHERE a <> '{}') AS ne
				   FROM lion_ned_t GROUP BY g$$);
SELECT lion_ned($$SELECT count(*), count(*) FILTER (WHERE array_ndims(a) > 0),
						count(*) FILTER (WHERE o <> '{}')
				   FROM lion_ned_t WHERE k = 3$$);
SELECT lion_ned($$SELECT count(DISTINCT k) FROM lion_ned_t WHERE array_length(a, 1) = 0 AND g = 2$$);
SELECT lion_ned($$SELECT count(*) FROM lion_ned_t WHERE k = 3 OR array_length(a, 1) = 0$$);
SELECT lion_ned($$SELECT count(*) FROM lion_ned_t WHERE k = 3 OR a <> '{}'$$);
SELECT lion_ned($$SELECT count(*) FROM lion_ned_t WHERE g = 1 AND (k = 3 OR cardinality(a) = 0)$$);
SELECT lion_ned($$SELECT count(*) FROM lion_ned_t WHERE g = 1 AND (k = 3 OR array_length(a, 1) IS DISTINCT FROM 0)$$);
-- a multi-key query known only at run time that narrows nothing: every row
-- is tested on the heap, the test of whether the array is empty with it, for
-- each set of reserved entries NULLs fail
SELECT lion_ned($$SELECT count(*) FROM lion_ned_t WHERE o @> $1 AND a <> '{}'$$, $$'{}'$$);
SELECT lion_ned($$SELECT count(*) FROM lion_ned_t WHERE o @> $1 AND cardinality(a) > 0$$, $$'{}'$$);
SELECT lion_ned($$SELECT count(*) FROM lion_ned_t WHERE o @> $1 AND array_length(a, 1) > 0$$, $$'{}'$$);
SELECT lion_ned($$SELECT count(*) FROM lion_ned_t WHERE o @> $1 AND array_ndims(a) IS NOT NULL$$, $$'{}'$$);
SELECT lion_ned($$SELECT count(*) FROM lion_ned_t WHERE o @> $1 AND array_length(a, 1) > 0$$, $$'{3}'$$);
SELECT lion_ned($$SELECT count(*) FROM lion_ned_t WHERE k = $1 AND array_length(a, 1) = 0$$, '3');

-- ---- 5. by cells --------------------------------------------------------
-- A multi-key dimension's outcomes: NULL for the NULL entry's rows, and for
-- every other row the queries its keys satisfy - none for a row under no
-- key, whichever reserved entry holds it.
SELECT lion_ned($$SELECT (a && '{1}') AS x, (a IS NULL) AS y, count(*) FROM lion_ned_t GROUP BY 1, 2$$);
SELECT lion_ned($$SELECT (a @> '{1}') AS x, (o && '{3}') AS y, count(*) FROM lion_ned_t GROUP BY 1, 2$$);
SELECT lion_ned($$SELECT (a && '{1,2}') AS x, count(*) FROM lion_ned_t WHERE cardinality(a) = 0 GROUP BY 1$$);
SELECT lion_ned($$SELECT (a && '{1,2}') AS x, count(*) FROM lion_ned_t WHERE array_length(a, 1) = 0 GROUP BY 1$$);
SELECT lion_ned($$SELECT (o && '{3}') AS x, (k = 3) AS y, count(*) FROM lion_ned_t WHERE a = '{}' GROUP BY 1, 2$$);

-- ---- 6. in parallel -----------------------------------------------------
-- Each participant counts its ranges of container keys from the reserved
-- entries as the serial node counts them all (DESIGN.md §10, "A count in
-- parallel").
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET pg_lion.parallel_range_keys = 1;
SELECT lion_ned($$SELECT count(*) FROM lion_ned_t WHERE k = 3 AND a <> '{}'$$);
SELECT lion_ned($$SELECT count(*) FROM lion_ned_t WHERE k = 3 AND array_length(a, 1) = 0$$);
SELECT lion_ned($$SELECT count(*) FROM lion_ned_t WHERE g = 1 AND cardinality(a) = 0$$);
SELECT lion_ned($$SELECT count(*) FROM lion_ned_t WHERE g = 1 AND a = '{}'$$);
SELECT lion_ned($$SELECT count(*) FROM lion_ned_t WHERE g = 1 AND array_length(a, 1) > 0 AND o <> '{}'$$);
SELECT lion_ned($$SELECT count(*), count(*) FILTER (WHERE a <> '{}'),
						count(*) FILTER (WHERE array_length(a, 1) = 0)
				   FROM lion_ned_t WHERE g = 2$$);
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET min_parallel_index_scan_size;
RESET pg_lion.parallel_range_keys;
SET max_parallel_workers_per_gather = 0;

-- ---- 7. a partial index -------------------------------------------------
-- A clause of the array's reserved entries selects only rows its own sets
-- hold, which in a partial index are the predicate's rows alone: the count
-- reads the partial index, and leaves out the restriction the predicate
-- implies (DESIGN.md §45, "Held to the predicate's rows").  Rows it rejects
-- (`cardinality(a) > 0`) are a complement, which holds nothing to it.
CREATE TABLE lion_ned_pt (id int NOT NULL, k int NOT NULL, a int2[])
	WITH (autovacuum_enabled = off);
INSERT INTO lion_ned_pt SELECT id, k, a FROM lion_ned_t WHERE id <= 6000;
CREATE INDEX lion_ned_pt_a ON lion_ned_pt USING lion (a) WHERE k > 6;
VACUUM (FREEZE, ANALYZE) lion_ned_pt;
SELECT lion_ned($$SELECT count(*) FROM lion_ned_pt WHERE k > 6 AND cardinality(a) = 0$$);
SELECT lion_ned($$SELECT count(*) FROM lion_ned_pt WHERE k > 6 AND a = '{}'$$);
SELECT lion_ned($$SELECT count(*) FROM lion_ned_pt WHERE k > 6 AND array_length(a, 1) = 0$$);
SELECT lion_ned($$SELECT count(*) FROM lion_ned_pt WHERE k > 6 AND coalesce(cardinality(a), 0) = 0$$);
SELECT lion_ned($$SELECT count(*) FROM lion_ned_pt WHERE k = 8 AND array_length(a, 1) IS NULL$$);
SELECT lion_ned($$SELECT count(*) FROM lion_ned_pt WHERE k > 6 AND cardinality(a) > 0$$);
SELECT lion_ned($$SELECT count(*) FROM lion_ned_pt WHERE k > 6 AND a IS NULL$$);
SELECT lion_ned_plan($$SELECT count(*) FROM lion_ned_pt WHERE k > 6 AND cardinality(a) = 0$$);
DROP TABLE lion_ned_pt;

-- ---- 8. the keys of the column ------------------------------------------
-- An array's keys are its elements, each row counted once under each: the
-- rows of the reserved entries have none, and no facet counts them.
SELECT * FROM lion_index_facets('lion_ned_a', 4);
SELECT e::text AS key, count(*)
  FROM (SELECT DISTINCT id, e FROM lion_ned_t, unnest(a) e WHERE e IS NOT NULL) r
 GROUP BY e ORDER BY count(*) DESC, e LIMIT 4;
SELECT * FROM lion_index_facets('lion_ned_a', 3, 'k = 3');
SELECT e::text AS key, count(*)
  FROM (SELECT DISTINCT id, e FROM lion_ned_t, unnest(a) e
		 WHERE e IS NOT NULL AND k = 3) r
 GROUP BY e ORDER BY count(*) DESC, e LIMIT 3;
SELECT * FROM lion_index_facets('lion_ned_o', 2, $$a && '{1}'$$);
SELECT e::text AS key, count(*)
  FROM (SELECT DISTINCT id, e FROM lion_ned_t, unnest(o) e
		 WHERE e IS NOT NULL AND a && '{1}') r
 GROUP BY e ORDER BY count(*) DESC, e LIMIT 2;

-- ---- 9. rows that move between the entries -----------------------------
-- An insert files each row in the entry its dimensions name, as the build
-- did; updated and deleted rows leave their old versions under the entries
-- they were in until VACUUM, and the count reads the heap for them.
UPDATE lion_ned_t SET a = ''::int2vector::int2[] WHERE id % 11 = 1 AND id % 3 = 0;
UPDATE lion_ned_t SET a = '{}' WHERE id % 11 = 2 AND id % 3 = 1;
UPDATE lion_ned_t SET a = ARRAY[''::int2vector::int2[]] WHERE id % 11 = 9 AND id % 3 = 2;
UPDATE lion_ned_t SET a = '7'::int2vector::int2[] WHERE id % 11 = 4 AND id % 3 = 0;
DELETE FROM lion_ned_t WHERE id % 11 = 3 AND id % 5 = 0;
INSERT INTO lion_ned_t
SELECT i, i % 10, i % 4,
	   CASE i % 3 WHEN 0 THEN ''::int2vector::int2[]
				  WHEN 1 THEN ARRAY[''::int2vector::int2[], ''::int2vector::int2[]]
				  ELSE '{}'::int2[] END,
	   ''::oidvector::oid[], 'y'
  FROM generate_series(50001, 50300) i;
SELECT * FROM lion_ned_each(ARRAY[
	$$a = '{}'$$, $$a <> '{}'$$, $$cardinality(a) = 0$$,
	$$array_length(a, 1) = 0$$, $$array_length(a, 1) > 0$$,
	$$o IS DISTINCT FROM '{}'$$
]);
VACUUM (FREEZE, ANALYZE) lion_ned_t;
SELECT null_tids = (SELECT count(*) FROM lion_ned_t WHERE a IS NULL) AS nulls,
	   empty_tids = (SELECT count(*) FROM lion_ned_t WHERE a = '{}') AS empty,
	   emptydim_tids = (SELECT count(*) FROM lion_ned_t
						 WHERE array_length(a, 1) = 0) AS emptydim,
	   emptyinner_tids = (SELECT count(*) FROM lion_ned_t
						   WHERE cardinality(a) = 0 AND array_length(a, 1) > 0) AS emptyinner
  FROM lion_index_stats('lion_ned_a');
SELECT lion_index_verify('lion_ned_a', true), lion_index_verify('lion_ned_o', true);
SELECT * FROM lion_ned_each(ARRAY[
	$$a = '{}'$$, $$a <> '{}'$$, $$cardinality(a) = 0$$,
	$$array_length(a, 1) = 0$$, $$array_length(a, 1) > 0$$,
	$$o IS DISTINCT FROM '{}'$$
]);
-- ... and a REINDEX builds the same entries
REINDEX INDEX lion_ned_a;
SELECT null_tids, empty_tids, emptydim_tids, emptyinner_tids, nullelem_tids
  FROM lion_index_stats('lion_ned_a');
SELECT lion_index_verify('lion_ned_a', true);

DROP TABLE lion_ned_t;
DROP FUNCTION lion_ned(text, text);
DROP FUNCTION lion_ned_each(text[], text);
DROP FUNCTION lion_ned_plan(text);
