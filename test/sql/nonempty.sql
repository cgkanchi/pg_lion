-- Whether an array is empty, answered from a lion array column's reserved
-- entries (DESIGN.md §17, "Non-empty arrays").
--
-- A row of a column that lion's array_ops indexes is in exactly one of five
-- classes: the column is NULL (the NULL entry), it is an array with no
-- element - '{}' in the EMPTY entry, and in the EMPTYDIM and EMPTYINNER
-- entries the empty arrays with dimensions an int2vector or oidvector cast
-- makes, which nonempty_dims tests and this table has none of - or it has an
-- element - its elements' entries, or the NULLELEMS entry when every
-- element is NULL.  A predicate that is true of every row of a class or of
-- none - `cardinality(tags) > 0`, `array_length(tags, 1) IS NOT NULL`,
-- `tags <> '{}'`, `tags = '{}'` and the like - is then a union of reserved
-- entries or its complement, which the count pushdown answers with no heap
-- visit.  Every query is run through the
-- pushdown with every other scan disabled and through a sequential scan with
-- the pushdown off, and the two results are compared as multisets both ways
-- round; the data has NULL arrays, empty ones, arrays of NULLs, arrays of
-- several dimensions and arrays whose lower bound is not 1.
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
 * lion_ne() runs q with every scan but the pushdown disabled, so that a
 * LionCount path that is built at all is the plan, and again with the
 * pushdown off and sequential scans only - which read no lion posting set -
 * and compares the two.  It says whether the node ran, whether it tested a
 * clause on the heap (a residual filter, DESIGN.md §10), and the result: the
 * row, when there is one, or how many.  With args, q is PREPAREd and EXECUTEd
 * with them under a generic plan.
 */
CREATE FUNCTION lion_ne(q text, args text DEFAULT NULL) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	ex text := q;
	pushed boolean := false;
	residual boolean := false;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('plan_cache_mode', 'force_generic_plan', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	IF args IS NOT NULL THEN
		EXECUTE 'PREPARE lion_ne_on AS ' || q;
		ex := 'EXECUTE lion_ne_on(' || args || ')';
	END IF;
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || ex LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			pushed := true;
		ELSIF ln LIKE '%Residual Filter%' THEN
			residual := true;
		END IF;
	END LOOP;
	EXECUTE 'CREATE TEMP TABLE lion_ne_r_on AS SELECT s::text AS r FROM (' ||
		CASE WHEN args IS NULL THEN q ELSE 'SELECT 1' END || ') s';
	IF args IS NOT NULL THEN
		EXECUTE 'DROP TABLE lion_ne_r_on';
		EXECUTE 'CREATE TEMP TABLE lion_ne_r_on AS ' || ex;
		EXECUTE 'DEALLOCATE lion_ne_on';
	END IF;

	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	IF args IS NOT NULL THEN
		EXECUTE 'PREPARE lion_ne_off AS ' || q;
		EXECUTE 'CREATE TEMP TABLE lion_ne_r_off AS EXECUTE lion_ne_off(' ||
			args || ')';
		EXECUTE 'DEALLOCATE lion_ne_off';
	ELSE
		EXECUTE 'CREATE TEMP TABLE lion_ne_r_off AS SELECT s::text AS r FROM (' ||
			q || ') s';
	END IF;
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('plan_cache_mode', 'auto', true);

	EXECUTE 'SELECT count(*) FROM lion_ne_r_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_ne_r_on EXCEPT ALL SELECT * FROM lion_ne_r_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_ne_r_off EXCEPT ALL SELECT * FROM lion_ne_r_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lion_ne_r_on';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	IF nrows = 1 AND args IS NULL THEN
		EXECUTE 'SELECT r FROM lion_ne_r_off' INTO ln;
	ELSIF nrows = 1 THEN
		EXECUTE 'SELECT s::text FROM lion_ne_r_off s' INTO ln;
	ELSE
		ln := nrows || ' rows';
	END IF;
	DROP TABLE lion_ne_r_off;
	IF NOT pushed THEN
		RETURN 'not pushed: ' || ln;
	END IF;
	IF residual THEN
		RETURN 'LionCount with a residual filter: ' || ln;
	END IF;
	RETURN 'LionCount: ' || ln;
END $$;

/*
 * The plan lion_ne() runs - or with scan, the plan of an index scan, the
 * pushdown off - in a form every supported release prints alike: 18's
 * "Disabled: true" lines are dropped.
 */
CREATE FUNCTION lion_ne_plan(q text, scan boolean DEFAULT false)
RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	IF scan THEN
		PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	ELSE
		PERFORM set_config('enable_bitmapscan', 'off', true);
		PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	END IF;
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		CONTINUE WHEN ln ~ '^\s*Disabled: true$';
		RETURN NEXT ln;
	END LOOP;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
END $$;

/*
 * The rows the planner expects of `SELECT * FROM <rel> WHERE <cond>` - the
 * relation's own row count, which every path and join above it is sized
 * from - against the rows there are: 'close' within 10%, and the factor it
 * is off by otherwise.  With lion false it is core's estimate, lion's
 * correction of it (pg_lion.enable_rows_correction) off.
 */
CREATE FUNCTION lion_ne_rows(rel text, cond text, lion boolean DEFAULT true)
RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	plan json;
	est float8;
	actual bigint;
BEGIN
	PERFORM set_config('pg_lion.enable_rows_correction', lion::text, true);
	EXECUTE format('EXPLAIN (FORMAT JSON) SELECT * FROM %s WHERE %s', rel, cond)
		INTO plan;
	PERFORM set_config('pg_lion.enable_rows_correction', 'on', true);
	est := (plan -> 0 -> 'Plan' ->> 'Plan Rows')::float8;
	EXECUTE format('SELECT count(*) FROM %s WHERE %s', rel, cond) INTO actual;
	IF abs(est - actual) <= 0.1 * actual + 1 THEN
		RETURN format('%s rows, estimate close', actual);
	END IF;
	RETURN format('%s rows, estimate off %sx', actual,
				  round((greatest(est, actual) / greatest(least(est, actual), 1))::numeric, 1));
END $$;

-- ---- the data ----------------------------------------------------------
/*
 * tags runs through every kind of array the classes hold, by id % 13: NULL
 * (0), '{}' (1, and 2 for every other row), '{NULL}' (3), '{NULL,NULL}' (4),
 * '{{NULL},{NULL}}' (5), a two-dimensional array (6), lower bound 0 (7),
 * lower bound -3 (8) and two elements otherwise.  txt is a text[] under the
 * "C" collation, NULL or empty more rarely, and dom a domain over int[].
 */
CREATE DOMAIN lion_ne_ints AS int[];
CREATE TABLE lion_ne_t (
	id		int NOT NULL,
	k		int NOT NULL,
	g		int NOT NULL,
	tags	int[],
	txt		text[] COLLATE "C",
	dom		lion_ne_ints
);
INSERT INTO lion_ne_t
SELECT i, i % 10, i % 4,
	   CASE i % 13
			WHEN 0 THEN NULL
			WHEN 1 THEN '{}'
			WHEN 2 THEN CASE WHEN i % 2 = 0 THEN '{}'::int[] ELSE ARRAY[i % 5] END
			WHEN 3 THEN '{NULL}'
			WHEN 4 THEN '{NULL,NULL}'
			WHEN 5 THEN '{{NULL},{NULL}}'
			WHEN 6 THEN ARRAY[ARRAY[i % 7, i % 3], ARRAY[NULL, 1]]
			WHEN 7 THEN '[0:2]={7,8,9}'
			WHEN 8 THEN '[-3:-3]={4}'
			ELSE ARRAY[i % 7, i % 13]
	   END,
	   CASE WHEN i % 29 = 0 THEN NULL
			WHEN i % 23 = 0 THEN '{}'::text[]
			WHEN i % 19 = 0 THEN ARRAY[NULL]::text[]
			ELSE ARRAY['t' || (i % 9)] END,
	   CASE WHEN i % 17 = 0 THEN NULL
			WHEN i % 5 = 0 THEN '{}'::int[]
			ELSE ARRAY[i % 3] END
  FROM generate_series(1, 13000) i;
CREATE INDEX lion_ne_k ON lion_ne_t USING lion (k);
CREATE INDEX lion_ne_g ON lion_ne_t USING lion (g);
CREATE INDEX lion_ne_tags ON lion_ne_t USING lion (tags);
CREATE INDEX lion_ne_txt ON lion_ne_t USING lion (txt);
CREATE INDEX lion_ne_dom ON lion_ne_t USING lion (dom);
VACUUM (FREEZE, ANALYZE) lion_ne_t;

-- ---- 1. the classes are the reserved entries ---------------------------
-- The NULL entry holds the NULL arrays, the EMPTY entry exactly the arrays
-- with no element (none has dimensions), and the NULLELEMS entry the arrays
-- whose elements are all NULL, whatever their dimensions.
SELECT null_tids = (SELECT count(*) FROM lion_ne_t WHERE tags IS NULL) AS nulls,
	   empty_tids = (SELECT count(*) FROM lion_ne_t WHERE cardinality(tags) = 0) AS empties,
	   nullelem_tids = (SELECT count(*) FROM lion_ne_t
						 WHERE cardinality(tags) > 0 AND
							   NOT EXISTS (SELECT 1 FROM unnest(tags) e
											WHERE e IS NOT NULL)) AS nullelems,
	   null_tids, empty_tids, nullelem_tids
  FROM lion_index_stats('lion_ne_tags');
SELECT lion_index_verify('lion_ne_tags', true);

-- ---- 2. the spellings answered -----------------------------------------
-- Non-empty: every row outside the NULL and EMPTY entries - and outside
-- EMPTYDIM and EMPTYINNER too for `cardinality(tags) > 0`, while `tags <>
-- '{}'` and the array_length() and array_ndims() spellings take their rows.
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND array_length(tags, 1) IS NOT NULL$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND array_length(tags, 1) > 0$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND array_length(tags, 1) >= 1$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND array_length(tags, 1) >= 0$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND array_length(tags, 1) <> 0$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND cardinality(tags) > 0$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND cardinality(tags) >= 1$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND cardinality(tags) <> 0$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND cardinality(tags) != 0$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND 0 < cardinality(tags)$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND 1 <= array_length(tags, 1)$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND cardinality(tags) > 0::int8$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND cardinality(tags) >= 1::int2$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND tags <> '{}'$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND '{}' <> tags$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND NOT (tags = '{}')$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND array_ndims(tags) > 0$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND array_ndims(tags) IS NOT NULL$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND array_lower(tags, 1) IS NOT NULL$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND array_upper(tags, 1) IS NOT NULL$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND coalesce(array_length(tags, 1), 0) > 0$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND (cardinality(tags) > 0) IS TRUE$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND (cardinality(tags) = 0) IS FALSE$$);
-- ... with the column's own null test beside it, and the shape written twice
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND tags IS NOT NULL AND array_length(tags, 1) > 0$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND array_length(tags, 1) IS NOT NULL AND array_length(tags, 1) >= 0$$);
-- Not empty, NULLs included: every row outside the EMPTY entry (and the
-- other two empty ones, for cardinality()).
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND cardinality(tags) IS DISTINCT FROM 0$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND tags IS DISTINCT FROM '{}'$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND (cardinality(tags) = 0) IS NOT TRUE$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND coalesce(cardinality(tags), 1) > 0$$);
-- Empty: the rows of the EMPTY entry (and the other two empty ones, for
-- cardinality()).
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND cardinality(tags) = 0$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND cardinality(tags) < 1$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND cardinality(tags) <= 0$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND tags = '{}'$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND '{}' = tags$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND tags IS NOT DISTINCT FROM '{}'$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND tags IS NOT NULL AND array_length(tags, 1) IS NULL$$);
-- Empty or NULL: the rows of the EMPTY and NULL entries (and the other two
-- empty ones, for cardinality()).
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND array_length(tags, 1) IS NULL$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND array_ndims(tags) IS NULL$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND coalesce(cardinality(tags), 0) = 0$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND (cardinality(tags) > 0) IS NOT TRUE$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND NOT (tags <> '{}' AND tags IS NOT NULL)$$);
-- NULL alone, and not NULL: the NULL entry of a multi-key column, which no
-- scalar index has the column to answer (DESIGN.md §14, §17).
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND cardinality(tags) IS NULL$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND (cardinality(tags) > 0) IS UNKNOWN$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND tags IS NULL$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND tags IS NOT NULL$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND cardinality(tags) >= 0$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND (cardinality(tags) = 0) IS NOT UNKNOWN$$);
SELECT lion_ne($$SELECT count(tags), count(*) FROM lion_ne_t WHERE k = 3 AND tags IS NULL$$);

-- The plans: one clause on each index, in the spelling of the rows it is.
SELECT lion_ne_plan($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND array_length(tags, 1) IS NOT NULL AND array_length(tags, 1) >= 0$$);
SELECT lion_ne_plan($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND tags IS DISTINCT FROM '{}'$$);
SELECT lion_ne_plan($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND tags = '{}'$$);
SELECT lion_ne_plan($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND array_length(tags, 1) IS NULL$$);
SELECT lion_ne_plan($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND cardinality(tags) IS NULL$$);
SELECT lion_ne_plan($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND tags IS NOT NULL$$);

-- The actual numbers, with the pushdown, for the record.
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET pg_lion.enable_plain_scan = off;
SELECT count(*) FILTER (WHERE cardinality(tags) > 0) AS nonempty,
	   count(*) FILTER (WHERE tags = '{}') AS empty,
	   count(*) FILTER (WHERE tags IS NULL) AS nulls,
	   count(*) FILTER (WHERE array_length(tags, 1) IS NULL) AS empty_or_null,
	   count(*) FILTER (WHERE tags IS DISTINCT FROM '{}') AS not_empty
  FROM lion_ne_t WHERE k = 3;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET pg_lion.enable_plain_scan;

-- ---- 3. the spellings left to the heap ---------------------------------
-- True of some non-empty arrays and not of others, a dimension past the
-- first, a dimension that is no constant, a bound known only at run time:
-- a residual filter beside k = 3.
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND array_length(tags, 1) > 1$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND array_length(tags, 1) = 1$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND array_length(tags, 2) IS NOT NULL$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND array_length(tags, 0) IS NULL$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND array_length(tags, g) IS NOT NULL$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND array_ndims(tags) = 1$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND array_lower(tags, 1) = 1$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND cardinality(tags) > $1$$, '0');
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND tags <> '{1}'$$);
-- True of every row, or of none: nothing to read off the entries; and
-- `array_length(tags, 1) = 0`, the rows of the EMPTYDIM entry, none here.
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND coalesce(cardinality(tags), 0) >= 0$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND array_length(tags, 1) = 0$$);
SELECT lion_ne_plan($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND array_length(tags, 1) > 1$$);

-- ---- 4. beside the other shapes ----------------------------------------
-- GROUP BY a lion column
SELECT lion_ne($$SELECT k, count(*) FROM lion_ne_t WHERE cardinality(tags) > 0 GROUP BY k$$);
SELECT lion_ne($$SELECT g, count(*) FROM lion_ne_t WHERE tags = '{}' GROUP BY g$$);
SELECT lion_ne($$SELECT g, count(*) FROM lion_ne_t WHERE array_length(tags, 1) IS NULL GROUP BY g$$);
SELECT lion_ne($$SELECT k, g, count(*) FROM lion_ne_t WHERE tags IS DISTINCT FROM '{}' GROUP BY k, g$$);
-- filtered counts
SELECT lion_ne($$SELECT count(*), count(*) FILTER (WHERE cardinality(tags) > 0),
						count(*) FILTER (WHERE tags = '{}'),
						count(*) FILTER (WHERE array_length(tags, 1) IS NULL)
				   FROM lion_ne_t WHERE k = 3$$);
SELECT lion_ne($$SELECT g, count(*) FILTER (WHERE cardinality(tags) > 0),
						count(*) FILTER (WHERE tags IS NULL)
				   FROM lion_ne_t GROUP BY g$$);
SELECT lion_ne_plan($$SELECT count(*), count(*) FILTER (WHERE cardinality(tags) > 0)
						FROM lion_ne_t WHERE k = 3$$);
-- every row: nothing in the WHERE selects rows, so the sum over k's entries
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE cardinality(tags) > 0$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE tags IS NOT NULL AND tags IS DISTINCT FROM '{}'$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE cardinality(tags) = 0$$);
-- ... and over a nullable column, whose own `IS NOT NULL` may drive the sum
-- (DESIGN.md §14): a multi-key column's never does
CREATE TABLE lion_ne_nn (k int, tags int[]);
INSERT INTO lion_ne_nn
SELECT CASE WHEN i % 7 = 0 THEN NULL ELSE i % 6 END,
	   CASE WHEN i % 5 = 0 THEN NULL WHEN i % 3 = 0 THEN '{}'::int[] ELSE ARRAY[i % 4] END
  FROM generate_series(1, 2000) i;
CREATE INDEX lion_ne_nn_k ON lion_ne_nn USING lion (k);
CREATE INDEX lion_ne_nn_tags ON lion_ne_nn USING lion (tags);
VACUUM (FREEZE, ANALYZE) lion_ne_nn;
SELECT lion_ne($$SELECT count(*) FROM lion_ne_nn WHERE tags IS NOT NULL AND k IS NOT NULL$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_nn WHERE k IS NOT NULL AND tags IS NOT NULL$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_nn WHERE k IS NOT NULL AND cardinality(tags) > 0$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_nn WHERE tags IS NOT NULL AND k IS NULL$$);
SELECT lion_ne($$SELECT k, count(*) FROM lion_ne_nn WHERE tags IS NOT NULL GROUP BY k$$);
SELECT lion_ne($$SELECT k, count(*) FROM lion_ne_nn WHERE tags = '{}' AND k IS NOT NULL GROUP BY k$$);
SELECT lion_ne($$SELECT count(DISTINCT k) FROM lion_ne_nn WHERE cardinality(tags) > 0$$);
DROP TABLE lion_ne_nn;
-- ORs: a positive arm, and a complement arm (DESIGN.md §35)
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 OR tags = '{}'$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE tags IS NULL OR cardinality(tags) = 0$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE g = 1 AND (k = 3 OR cardinality(tags) > 0)$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE g = 1 AND (k = 3 OR tags IS DISTINCT FROM '{}')$$);
SELECT lion_ne_plan($$SELECT count(*) FROM lion_ne_t WHERE g = 1 AND (k = 3 OR cardinality(tags) > 0)$$);
-- beside multi-key queries of the same column and of another
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE tags @> '{4}' AND cardinality(tags) > 0$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE txt && '{t1,t2}' AND cardinality(tags) = 0$$);
-- a multi-key query known only at run time (DESIGN.md §17): `'{}'` narrows
-- nothing, and every row is then tested on the heap, `tags <> '{}'` with it,
-- and a test that NULLs pass as well, as `IS DISTINCT FROM` is (§35)
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE txt @> $1 AND cardinality(tags) > 0$$, $$'{t3}'$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE txt @> $1 AND cardinality(tags) > 0$$, $$'{}'$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE txt @> $1 AND tags IS DISTINCT FROM '{}'$$, $$'{t3}'$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE txt @> $1 AND tags IS DISTINCT FROM '{}'$$, $$'{}'$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = $1 AND cardinality(tags) > 0$$, '3');
-- a WHERE of several columns, the non-empty test spelled twice, under a
-- filtered count, in a generic plan
SELECT lion_ne($$SELECT count(*) AS n, count(*) FILTER (WHERE txt && '{t1,t3}') AS f
				   FROM lion_ne_t
				  WHERE k = $1 AND g = 1 AND dom @> '{1}' AND
						array_length(tags, 1) IS NOT NULL AND
						array_length(tags, 1) >= 0$$, '3');
SELECT lion_ne_plan($$SELECT count(*), count(*) FILTER (WHERE txt && '{t1,t3}')
						FROM lion_ne_t
					   WHERE k = 3 AND g = 1 AND dom @> '{1}' AND
							 array_length(tags, 1) IS NOT NULL AND
							 array_length(tags, 1) >= 0$$);

-- ---- 5. other array columns --------------------------------------------
-- text[] under "C", and a domain over int[]
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND cardinality(txt) > 0$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND txt = '{}'$$);
SELECT lion_ne($$SELECT k, count(*) FROM lion_ne_t WHERE array_length(dom, 1) IS NOT NULL GROUP BY k$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE g = 2 AND dom = '{}'$$);
-- two clauses of two array columns of one multicolumn index
CREATE TABLE lion_ne_mc (k int NOT NULL, a int[], b text[]);
INSERT INTO lion_ne_mc
SELECT i % 5,
	   CASE WHEN i % 3 = 0 THEN '{}'::int[] WHEN i % 7 = 0 THEN NULL ELSE ARRAY[i % 4] END,
	   CASE WHEN i % 4 = 0 THEN '{}'::text[] WHEN i % 11 = 0 THEN ARRAY[NULL]::text[]
			ELSE ARRAY['b' || (i % 3)] END
  FROM generate_series(1, 3000) i;
CREATE INDEX lion_ne_mc_kab ON lion_ne_mc USING lion (k, a, b);
VACUUM (FREEZE, ANALYZE) lion_ne_mc;
SELECT lion_ne($$SELECT count(*) FROM lion_ne_mc WHERE k = 1 AND cardinality(a) > 0 AND b = '{}'$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_mc WHERE k = 1 AND a IS NOT NULL AND array_length(b, 1) IS NULL$$);
SELECT lion_ne_plan($$SELECT count(*) FROM lion_ne_mc WHERE k = 1 AND cardinality(a) > 0 AND b IS NOT NULL$$);

-- ---- 6. which index answers --------------------------------------------
-- A class of whole arrays (one key per row) has no EMPTY entry to read: the
-- emptiness is left to the heap, while its NULL entry still answers `IS NOT
-- NULL`.  A class of the user's that extracts with ginarrayextract() itself
-- keeps the classes apart as array_ops does.
CREATE OPERATOR CLASS lion_ne_whole_ops FOR TYPE anyarray USING lion AS
	OPERATOR	1	= (anyarray, anyarray),
	FUNCTION	1	hash_array(anyarray);
CREATE FUNCTION lion_ne_xid_hash(xid) RETURNS integer
	LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE
	AS $$ SELECT (($1::text)::int8 % 5)::int4 $$;
CREATE OPERATOR CLASS lion_ne_xida_ops FOR TYPE xid[] USING lion AS
	OPERATOR	2	@> (anyarray, anyarray),
	OPERATOR	3	&& (anyarray, anyarray),
	OPERATOR	4	<@ (anyarray, anyarray),
	FUNCTION	1	lion_ne_xid_hash(xid),
	FUNCTION	2	ginarrayextract(anyarray, internal, internal),
	FUNCTION	3	ginqueryarrayextract(anyarray, internal, int2, internal, internal, internal, internal),
	STORAGE		xid;
CREATE TABLE lion_ne_cls (k int NOT NULL, w int[], x xid[]);
INSERT INTO lion_ne_cls
SELECT i % 5,
	   CASE WHEN i % 3 = 0 THEN '{}'::int[] WHEN i % 7 = 0 THEN NULL ELSE ARRAY[i % 4] END,
	   CASE WHEN i % 3 = 0 THEN '{}'::xid[] WHEN i % 7 = 0 THEN NULL
			WHEN i % 11 = 0 THEN ARRAY[NULL]::xid[] ELSE ARRAY[(i % 4)::text::xid] END
  FROM generate_series(1, 3000) i;
CREATE INDEX lion_ne_cls_k ON lion_ne_cls USING lion (k);
CREATE INDEX lion_ne_cls_w ON lion_ne_cls USING lion (w lion_ne_whole_ops);
CREATE INDEX lion_ne_cls_x ON lion_ne_cls USING lion (x lion_ne_xida_ops);
VACUUM (FREEZE, ANALYZE) lion_ne_cls;
SELECT lion_ne($$SELECT count(*) FROM lion_ne_cls WHERE k = 1 AND cardinality(w) > 0$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_cls WHERE k = 1 AND w IS NOT NULL$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_cls WHERE k = 1 AND cardinality(x) > 0$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_cls WHERE k = 1 AND x = '{}'$$);

-- ---- 7. a partitioned table --------------------------------------------
CREATE TABLE lion_ne_p (id int NOT NULL, k int NOT NULL, tags int[])
	PARTITION BY RANGE (id);
CREATE TABLE lion_ne_p1 PARTITION OF lion_ne_p FOR VALUES FROM (0) TO (2000);
CREATE TABLE lion_ne_p2 PARTITION OF lion_ne_p FOR VALUES FROM (2000) TO (4000);
INSERT INTO lion_ne_p
SELECT i, i % 6,
	   CASE WHEN i % 5 = 0 THEN '{}'::int[] WHEN i % 9 = 0 THEN NULL
			WHEN i % 13 = 0 THEN '{NULL}'::int[] ELSE ARRAY[i % 4] END
  FROM generate_series(0, 3999) i;
CREATE INDEX ON lion_ne_p1 USING lion (k);
CREATE INDEX ON lion_ne_p2 USING lion (k);
CREATE INDEX ON lion_ne_p1 USING lion (tags);
CREATE INDEX ON lion_ne_p2 USING lion (tags);
VACUUM (FREEZE, ANALYZE) lion_ne_p;
SELECT lion_ne($$SELECT count(*) FROM lion_ne_p WHERE k = 2 AND cardinality(tags) > 0$$);
SELECT lion_ne($$SELECT k, count(*) FROM lion_ne_p WHERE tags = '{}' GROUP BY k$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_p WHERE k = 2 AND array_length(tags, 1) IS NULL$$);
-- a partition whose tags no lion index holds answers nothing
CREATE TABLE lion_ne_p3 PARTITION OF lion_ne_p FOR VALUES FROM (4000) TO (5000);
CREATE INDEX ON lion_ne_p3 USING lion (k);
INSERT INTO lion_ne_p SELECT i, i % 6, '{}' FROM generate_series(4000, 4099) i;
VACUUM (FREEZE, ANALYZE) lion_ne_p;
SELECT lion_ne($$SELECT count(*) FROM lion_ne_p WHERE k = 2 AND cardinality(tags) > 0$$);
DROP TABLE lion_ne_p;

-- ---- 8. rows that move between the classes -----------------------------
-- Updated rows leave their old versions under the entries they were in, and
-- deleted ones theirs, until VACUUM: the count reads the heap for them.
UPDATE lion_ne_t SET tags = '{}' WHERE id % 13 = 9 AND id % 3 = 0;
UPDATE lion_ne_t SET tags = ARRAY[id] WHERE id % 13 = 1 AND id % 3 = 1;
UPDATE lion_ne_t SET tags = NULL WHERE id % 13 = 3 AND id % 3 = 2;
UPDATE lion_ne_t SET tags = '{NULL}' WHERE id % 13 = 0 AND id % 3 = 0;
DELETE FROM lion_ne_t WHERE id % 13 = 2 AND id % 5 = 0;
INSERT INTO lion_ne_t SELECT i, i % 10, i % 4, '{}', '{}', '{}'
  FROM generate_series(20001, 20100) i;
INSERT INTO lion_ne_t SELECT i, i % 10, i % 4, '{{NULL,NULL}}', NULL, NULL
  FROM generate_series(20101, 20150) i;
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND cardinality(tags) > 0$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND tags = '{}'$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND array_length(tags, 1) IS NULL$$);
SELECT lion_ne($$SELECT g, count(*) FROM lion_ne_t WHERE tags IS DISTINCT FROM '{}' GROUP BY g$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE cardinality(tags) > 0$$);
VACUUM (FREEZE, ANALYZE) lion_ne_t;
SELECT lion_index_verify('lion_ne_tags', true);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND cardinality(tags) > 0$$);
SELECT lion_ne($$SELECT count(*) FROM lion_ne_t WHERE k = 3 AND tags = '{}'$$);
SELECT lion_ne($$SELECT g, count(*) FROM lion_ne_t WHERE array_length(tags, 1) IS NULL GROUP BY g$$);
SELECT lion_ne($$SELECT count(*), count(*) FILTER (WHERE cardinality(tags) > 0) FROM lion_ne_t WHERE g = 1$$);

-- ---- 9. the rows the planner expects -----------------------------------
-- Core estimates each spelling from no statistics - 0.995 for a null test of
-- an expression, a third for an inequality - and multiplies them; lion
-- estimates the rows the classes they select hold, from the column's
-- statistics as a whole (lion_nonempty_rel_clauses()).
SELECT lion_ne_rows('lion_ne_t', $$array_length(tags, 1) IS NOT NULL AND array_length(tags, 1) >= 0$$);
SELECT lion_ne_rows('lion_ne_t', $$array_length(tags, 1) IS NOT NULL AND array_length(tags, 1) >= 0$$, false);
SELECT lion_ne_rows('lion_ne_t', $$cardinality(tags) > 0$$);
SELECT lion_ne_rows('lion_ne_t', $$cardinality(tags) > 0$$, false);
SELECT lion_ne_rows('lion_ne_t', $$array_length(tags, 1) IS NULL$$);
SELECT lion_ne_rows('lion_ne_t', $$array_length(tags, 1) IS NULL$$, false);
SELECT lion_ne_rows('lion_ne_t', $$coalesce(cardinality(tags), 0) = 0$$);
SELECT lion_ne_rows('lion_ne_t', $$cardinality(tags) = 0$$);
SELECT lion_ne_rows('lion_ne_t', $$tags IS DISTINCT FROM '{}'$$);
SELECT lion_ne_rows('lion_ne_t', $$cardinality(tags) IS DISTINCT FROM 0$$);
SELECT lion_ne_rows('lion_ne_t', $$cardinality(tags) IS DISTINCT FROM 0$$, false);
SELECT lion_ne_rows('lion_ne_t', $$cardinality(tags) >= 0$$);
SELECT lion_ne_rows('lion_ne_t', $$cardinality(tags) IS NULL$$);
SELECT lion_ne_rows('lion_ne_t', $$tags IS NOT NULL AND cardinality(tags) <> 0$$);
SELECT lion_ne_rows('lion_ne_t', $$k = 3 AND cardinality(tags) > 0$$);
SELECT lion_ne_rows('lion_ne_t', $$cardinality(txt) > 0 AND array_length(txt, 1) >= 1$$);
-- never true together: no row (the estimate's floor is one)
SELECT lion_ne_rows('lion_ne_t', $$cardinality(tags) > 0 AND tags = '{}'$$);

-- ---- 10. the scans ------------------------------------------------------
-- A plain or bitmap index scan is handed only the clauses whose operator
-- is in the index's opfamily, and null tests: `tags IS NOT NULL`, `tags IS
-- NULL` and `tags = '{}'` reach one, a function of the column never does,
-- nor `<>`, which is no operator of the class.
SELECT lion_ne_plan($$SELECT id FROM lion_ne_t WHERE tags IS NOT NULL$$, true);
SELECT lion_ne_plan($$SELECT id FROM lion_ne_t WHERE tags = '{}'$$, true);
SELECT lion_ne_plan($$SELECT id FROM lion_ne_t WHERE cardinality(tags) > 0 AND k = 3$$, true);
SELECT lion_ne_plan($$SELECT id FROM lion_ne_t WHERE tags <> '{}' AND k = 3$$, true);

DROP TABLE lion_ne_t, lion_ne_mc, lion_ne_cls;
DROP OPERATOR CLASS lion_ne_whole_ops USING lion;
DROP OPERATOR FAMILY lion_ne_whole_ops USING lion;
DROP OPERATOR CLASS lion_ne_xida_ops USING lion;
DROP OPERATOR FAMILY lion_ne_xida_ops USING lion;
DROP FUNCTION lion_ne_xid_hash(xid);
DROP DOMAIN lion_ne_ints;
DROP FUNCTION lion_ne(text, text);
DROP FUNCTION lion_ne_plan(text, boolean);
DROP FUNCTION lion_ne_rows(text, text, boolean);
