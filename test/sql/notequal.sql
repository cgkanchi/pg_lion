-- `<>` (and `!=`), and a count of every row (DESIGN.md §35).
--
-- Strategy 10 of every ordered scalar class is `<>`: every entry of the
-- column but the one its value names, which the bitmap and the plain scan
-- answer as a walk with a hole in it, and the count pushdown as a negated
-- source - c's entry and the NULL one, subtracted.  With nothing in the WHERE
-- that selects rows (`count(*)` alone, or beside `IS NOT NULL` and `<>`
-- only), the count is the sum over every entry of the lion column with the
-- fewest.  A lion column's ordered walk (§30.11) steps over the hole, and
-- lets the first visible row of an entry decide a filter on the walked column
-- for the whole entry.  Every answer is checked against a SEQUENTIAL SCAN, as
-- a multiset in both directions.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
CREATE EXTENSION IF NOT EXISTS citext;
CREATE EXTENSION IF NOT EXISTS pg_lion_citext;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET default_statistics_target = 1000;
SET max_parallel_workers_per_gather = 0;

/*
 * lion_nq() runs q with one kind of scan - 'bitmap', 'plain' (an Index Scan)
 * or 'count' (the LionCount pushdown, every other scan disabled) - and again
 * as a sequential scan with the pushdown off, and compares the two.  It says
 * whether the scan asked for was really used.
 */
CREATE FUNCTION lion_nq(q text, how text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	used boolean := false;
	want text;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', (how = 'bitmap')::text, true);
	PERFORM set_config('enable_indexscan', (how = 'plain')::text, true);
	PERFORM set_config('enable_indexonlyscan', (how = 'plain')::text, true);
	PERFORM set_config('pg_lion.enable_count_pushdown', (how = 'count')::text, true);
	want := CASE how WHEN 'bitmap' THEN 'Bitmap Index Scan'
					 WHEN 'plain' THEN 'Index (Only )?Scan using'
					 ELSE 'Custom Scan \(LionCount\)' END;
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ want THEN
			used := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lion_nq_on AS SELECT s::text AS r FROM (%s) s', q);

	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE format('CREATE TEMP TABLE lion_nq_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM lion_nq_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_nq_on EXCEPT ALL SELECT * FROM lion_nq_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_nq_off EXCEPT ALL SELECT * FROM lion_nq_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lion_nq_on, lion_nq_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s%s, %s rows', CASE WHEN used THEN '' ELSE 'no ' END, how,
				  nrows);
END $$;

/* ... and the counters of a LionOrdered node under EXPLAIN ANALYZE. */
CREATE FUNCTION lion_nrun(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF ln ~ '(LionOrdered|Entries Walked|Keys Walked|Keys Filtered|Heap Fetches)' THEN
			RETURN NEXT btrim(regexp_replace(ln, '\s*\(actual.*\)', ''));
		END IF;
	END LOOP;
END $$;

/* The plan's lines that name lion's work, one per line. */
CREATE FUNCTION lion_nplan(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ '(Custom Scan|Lion|Index Cond|Filter|Ordered By|Index Scan|Seq Scan)' THEN
			RETURN NEXT btrim(ln);
		END IF;
	END LOOP;
END $$;

-- 1. The classes: `<>` beside the range comparisons of every ordered class,
--    for the same type pairs, and every class still valid.
SELECT count(*) AS ne_operators,
	   count(DISTINCT o.amopfamily) AS families
  FROM pg_amop o
 WHERE o.amopmethod = (SELECT oid FROM pg_am WHERE amname = 'lion')
   AND o.amopstrategy = 10;
SELECT count(*) AS range_pairs_without_ne
  FROM pg_amop r
 WHERE r.amopmethod = (SELECT oid FROM pg_am WHERE amname = 'lion')
   AND r.amopstrategy = 9
   AND NOT EXISTS (SELECT 1 FROM pg_amop n
					WHERE n.amopfamily = r.amopfamily AND n.amopstrategy = 10
					  AND n.amoplefttype = r.amoplefttype
					  AND n.amoprighttype = r.amoprighttype);
SELECT c.opcname FROM pg_opclass c
 WHERE c.opcmethod = (SELECT oid FROM pg_am WHERE amname = 'lion')
   AND NOT amvalidate(c.oid);

CREATE TABLE lion_ne (
	id int PRIMARY KEY,
	k int NOT NULL,			-- 50 values
	s text,					-- '' in 7 rows of 8, a few hundred values otherwise
	n int,					-- 7 values, NULL in every 13th row
	g int NOT NULL,			-- 3 values
	d int NOT NULL,			-- 20000 values, summarized
	e text,					-- an enum's worth of values in a text column
	u uuid,
	c citext
) WITH (autovacuum_enabled = off);
INSERT INTO lion_ne
SELECT i, i % 50,
	   CASE WHEN i % 8 <> 0 THEN '' ELSE 'p' || (i % 997) END,
	   CASE WHEN i % 13 = 0 THEN NULL ELSE i % 7 END,
	   i % 3, i / 5, 'v' || (i % 4),
	   ('00000000-0000-0000-0000-' || lpad((i % 9)::text, 12, '0'))::uuid,
	   CASE i % 3 WHEN 0 THEN 'Ab' WHEN 1 THEN 'aB' ELSE 'cd' END
  FROM generate_series(1, 100000) i;
CREATE INDEX lion_ne_k ON lion_ne USING lion (k);
CREATE INDEX lion_ne_s ON lion_ne USING lion (s);
CREATE INDEX lion_ne_n ON lion_ne USING lion (n);
CREATE INDEX lion_ne_g ON lion_ne USING lion (g);
CREATE INDEX lion_ne_d ON lion_ne USING lion (d) WITH (summaries = on);
CREATE INDEX lion_ne_mc ON lion_ne USING lion (e, u);
CREATE INDEX lion_ne_c ON lion_ne USING lion (c);
VACUUM (FREEZE, ANALYZE) lion_ne;

-- 2. The bitmap and the plain scan: a walk of the column with a hole in it.
SELECT lion_nq($$SELECT id FROM lion_ne WHERE k <> 3$$, 'bitmap');
SELECT lion_nq($$SELECT id FROM lion_ne WHERE k != 3$$, 'plain');
SELECT lion_nq($$SELECT id FROM lion_ne WHERE 3 <> k$$, 'bitmap');
SELECT lion_nq($$SELECT id FROM lion_ne WHERE s <> ''$$, 'bitmap');
SELECT lion_nq($$SELECT id FROM lion_ne WHERE s <> ''$$, 'plain');
SELECT lion_nq($$SELECT id FROM lion_ne WHERE n <> 2$$, 'bitmap');	-- no NULLs
SELECT lion_nq($$SELECT id FROM lion_ne WHERE n <> 2$$, 'plain');
SELECT lion_nq($$SELECT id FROM lion_ne WHERE k <> 3::int8$$, 'bitmap');	-- cross-type
SELECT lion_nq($$SELECT id FROM lion_ne WHERE k <> 3 AND k > 40$$, 'bitmap');
SELECT lion_nq($$SELECT id FROM lion_ne WHERE k <> 3 AND k <> 45 AND k < 48$$, 'plain');
SELECT lion_nq($$SELECT id FROM lion_ne WHERE k = 3 AND k <> 3$$, 'bitmap');
SELECT lion_nq($$SELECT id FROM lion_ne WHERE k <> 3 AND g = 1$$, 'plain');
SELECT lion_nq($$SELECT id FROM lion_ne WHERE k <> 999$$, 'bitmap');	-- no such entry
-- a summarized column: a bucket may hold the hole, so the keys are walked
SELECT lion_nq($$SELECT id FROM lion_ne WHERE d <> 500$$, 'bitmap');
SELECT lion_nq($$SELECT id FROM lion_ne WHERE d <> 500 AND d BETWEEN 100 AND 9000$$, 'plain');
-- a multicolumn index, either column, and other types
SELECT lion_nq($$SELECT id FROM lion_ne WHERE e <> 'v1'$$, 'bitmap');
SELECT lion_nq($$SELECT id FROM lion_ne WHERE u <> '00000000-0000-0000-0000-000000000004' AND e = 'v2'$$, 'plain');
SELECT lion_nq($$SELECT id FROM lion_ne WHERE c <> 'AB'$$, 'bitmap');	-- 'Ab' and 'aB' are one value
-- `<> ANY (array)`: every row with a value once two elements differ, the
-- one hole with one, no row with none
SELECT lion_nq($$SELECT id FROM lion_ne WHERE k <> ANY ('{3}')$$, 'bitmap');
SELECT lion_nq($$SELECT id FROM lion_ne WHERE k <> ANY ('{3,3,NULL}')$$, 'plain');
SELECT lion_nq($$SELECT id FROM lion_ne WHERE k <> ANY ('{3,4}')$$, 'bitmap');
SELECT lion_nq($$SELECT id FROM lion_ne WHERE n <> ANY ('{1,2}')$$, 'plain');
SELECT lion_nq($$SELECT id FROM lion_ne WHERE k <> ANY ('{}')$$, 'bitmap');
SELECT lion_nq($$SELECT id FROM lion_ne WHERE k <> ANY ('{3,3}'::int8[])$$, 'plain');
SELECT lion_nq($$SELECT id FROM lion_ne WHERE c <> ANY ('{AB,ab}')$$, 'bitmap');
SELECT * FROM lion_nplan($$SELECT id FROM lion_ne WHERE k <> 3 AND g = 1$$);

-- 3. The count: `<>` subtracts c's entry and the NULL one.  With nothing
--    that selects rows the sum over every entry of the column with the
--    fewest drives it - `(all rows)` - and a bare count(*) is just that.
SELECT lion_nq($$SELECT count(*) FROM lion_ne$$, 'count');
SELECT lion_nq($$SELECT count(*) FROM lion_ne WHERE k <> 3$$, 'count');
SELECT lion_nq($$SELECT count(*) FROM lion_ne WHERE 3 <> k$$, 'count');
SELECT lion_nq($$SELECT count(*) FROM lion_ne WHERE s <> ''$$, 'count');
SELECT lion_nq($$SELECT count(*) FROM lion_ne WHERE n <> 2$$, 'count');
SELECT lion_nq($$SELECT count(*) FROM lion_ne WHERE n <> 2 AND k <> 3$$, 'count');
SELECT lion_nq($$SELECT count(*) FROM lion_ne WHERE n IS NOT NULL AND k <> 3$$, 'count');
SELECT lion_nq($$SELECT count(*) FROM lion_ne WHERE n IS NOT NULL$$, 'count');
SELECT lion_nq($$SELECT count(*) FROM lion_ne WHERE k <> 3 AND g = 1$$, 'count');
SELECT lion_nq($$SELECT count(*) FROM lion_ne WHERE k <> 3 AND k IN (3, 4)$$, 'count');
SELECT lion_nq($$SELECT count(*) FROM lion_ne WHERE k <> 3 AND k > 40$$, 'count');
SELECT lion_nq($$SELECT count(*) FROM lion_ne WHERE k <> 3::int8$$, 'count');
SELECT lion_nq($$SELECT count(*) FROM lion_ne WHERE k <> 999$$, 'count');
SELECT lion_nq($$SELECT count(*) FROM lion_ne WHERE c <> 'ab'$$, 'count');
SELECT lion_nq($$SELECT count(*) FROM lion_ne WHERE e <> 'v1' AND u <> '00000000-0000-0000-0000-000000000004'$$, 'count');
SELECT lion_nq($$SELECT count(n) FROM lion_ne WHERE n <> 2$$, 'count');
SELECT lion_nq($$SELECT g, count(*) FROM lion_ne WHERE k <> 3 GROUP BY g$$, 'count');
SELECT lion_nq($$SELECT k, count(*) FROM lion_ne WHERE k <> 3 GROUP BY k$$, 'count');
SELECT lion_nq($$SELECT n, count(*) FROM lion_ne WHERE n <> 3 GROUP BY n$$, 'count');
SELECT lion_nq($$SELECT count(DISTINCT k) FROM lion_ne WHERE k <> 3$$, 'count');
SELECT lion_nq($$SELECT g, count(DISTINCT n) FROM lion_ne WHERE n <> 3 GROUP BY g$$, 'count');
-- under an OR `<>` would be a complement in a union: not taken
SELECT lion_nq($$SELECT count(*) FROM lion_ne WHERE k <> 3 OR g = 1$$, 'count');
SELECT * FROM lion_nplan($$SELECT count(*) FROM lion_ne$$);
SELECT * FROM lion_nplan($$SELECT count(*) FROM lion_ne WHERE s <> ''$$);
SELECT * FROM lion_nplan($$SELECT count(*) FROM lion_ne WHERE n IS NOT NULL$$);
SELECT * FROM lion_nplan($$SELECT count(*) FROM lion_ne WHERE k <> 3 AND g = 1$$);
-- a generic plan's `<> $1`, and a NULL one, which no row satisfies
SET plan_cache_mode = force_generic_plan;
PREPARE lion_ne_p(int) AS SELECT count(*) FROM lion_ne WHERE k <> $1;
SELECT * FROM lion_nplan('EXECUTE lion_ne_p(3)');
EXECUTE lion_ne_p(3);
EXECUTE lion_ne_p(NULL);
DEALLOCATE lion_ne_p;
RESET plan_cache_mode;
-- the pushdown checks EXECUTE on `<>`'s function, as the plain plan does
CREATE ROLE lion_ne_user;
GRANT SELECT ON lion_ne TO lion_ne_user;
REVOKE EXECUTE ON FUNCTION int4ne(int4, int4) FROM PUBLIC;
SET ROLE lion_ne_user;
SELECT count(*) FROM lion_ne WHERE k <> 3;
SET pg_lion.enable_count_pushdown = off;
SELECT count(*) FROM lion_ne WHERE k <> 3;
RESET pg_lion.enable_count_pushdown;
RESET ROLE;
GRANT EXECUTE ON FUNCTION int4ne(int4, int4) TO PUBLIC;
REVOKE SELECT ON lion_ne FROM lion_ne_user;
DROP ROLE lion_ne_user;

-- 4. The driving column of a count of every row: a scalar, whole,
--    non-expression index's, or nothing is pushed down.
CREATE TABLE lion_ne_x (id int, t int[], p int, q int) WITH (autovacuum_enabled = off);
INSERT INTO lion_ne_x SELECT i, ARRAY[i % 5], i % 4, i % 6 FROM generate_series(1, 2000) i;
CREATE INDEX lion_ne_x_t ON lion_ne_x USING lion (t);				-- multi-key
CREATE INDEX lion_ne_x_p ON lion_ne_x USING lion (p) WHERE q > 0;	-- partial
CREATE INDEX lion_ne_x_e ON lion_ne_x USING lion ((q + 1));			-- expression
VACUUM (FREEZE, ANALYZE) lion_ne_x;
SELECT lion_nq($$SELECT count(*) FROM lion_ne_x$$, 'count');
CREATE INDEX lion_ne_x_q ON lion_ne_x USING lion (q);
VACUUM ANALYZE lion_ne_x;
SELECT lion_nq($$SELECT count(*) FROM lion_ne_x$$, 'count');
SELECT lion_nq($$SELECT count(*) FROM lion_ne_x WHERE p <> 1$$, 'count');
TRUNCATE lion_ne_x;
SELECT lion_nq($$SELECT count(*) FROM lion_ne_x$$, 'count');
DROP TABLE lion_ne_x;

-- 5. A dirty heap: the counts recheck, and a partitioned table sums its
--    partitions' own drivers.
UPDATE lion_ne SET k = 3 WHERE id % 100 = 1;
DELETE FROM lion_ne WHERE id % 97 = 0;
SELECT lion_nq($$SELECT count(*) FROM lion_ne$$, 'count');
SELECT lion_nq($$SELECT count(*) FROM lion_ne WHERE k <> 3$$, 'count');
SELECT lion_nq($$SELECT g, count(*) FROM lion_ne WHERE k <> 3 GROUP BY g$$, 'count');
SELECT lion_nq($$SELECT id FROM lion_ne WHERE k <> 3$$, 'bitmap');
VACUUM (FREEZE, ANALYZE) lion_ne;
CREATE TABLE lion_ne_pt (id int, k int, g int) PARTITION BY RANGE (id);
CREATE TABLE lion_ne_pt1 PARTITION OF lion_ne_pt FOR VALUES FROM (0) TO (5000);
CREATE TABLE lion_ne_pt2 PARTITION OF lion_ne_pt FOR VALUES FROM (5000) TO (20001);
INSERT INTO lion_ne_pt SELECT i, i % 50, i % 3 FROM generate_series(1, 20000) i;
CREATE INDEX ON lion_ne_pt USING lion (k);
CREATE INDEX ON lion_ne_pt USING lion (g);
VACUUM (FREEZE, ANALYZE) lion_ne_pt;
SELECT lion_nq($$SELECT count(*) FROM lion_ne_pt$$, 'count');
SELECT lion_nq($$SELECT count(*) FROM lion_ne_pt WHERE k <> 3$$, 'count');
SELECT lion_nq($$SELECT g, count(*) FROM lion_ne_pt WHERE k <> 3 GROUP BY g$$, 'count');
SELECT * FROM lion_nplan($$SELECT count(*) FROM lion_ne_pt WHERE k <> 3$$);
DROP TABLE lion_ne_pt;

-- 6. A lion column's ordered walk (DESIGN.md §30.11) steps over the hole:
--    `s <> '' ORDER BY s LIMIT 10` no longer walks the rows of '' first.
CREATE FUNCTION lion_no(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	used boolean := false;
	a text[] := '{}';
	b text[] := '{}';
	r record;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
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
	PERFORM set_config('pg_lion.enable_ordered_scan', 'off', true);
	FOR r IN EXECUTE q LOOP
		b := b || r::text;
	END LOOP;
	PERFORM set_config('pg_lion.enable_ordered_scan', 'on', true);
	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH: %s rows through the node, %s ordinary',
					  coalesce(array_length(a, 1), 0), coalesce(array_length(b, 1), 0));
	END IF;
	RETURN format('%s, %s rows', CASE WHEN used THEN 'LionOrdered' ELSE 'no LionOrdered' END,
				  coalesce(array_length(a, 1), 0));
END $$;
SELECT lion_no($$SELECT s, id FROM lion_ne WHERE s <> '' ORDER BY s, id LIMIT 10$$);
SELECT lion_no($$SELECT s, id FROM lion_ne WHERE s <> '' ORDER BY s DESC, id DESC LIMIT 10$$);
SELECT lion_no($$SELECT k, id FROM lion_ne WHERE k <> 49 ORDER BY k DESC, id DESC LIMIT 10$$);
SELECT lion_no($$SELECT k, id FROM lion_ne WHERE k <> 3 AND k BETWEEN 2 AND 5 ORDER BY k DESC, id DESC$$);
SELECT lion_no($$SELECT k, id FROM lion_ne WHERE k <> 3 AND k IN (1, 3, 5) ORDER BY k, id$$);
SELECT lion_no($$SELECT n, id FROM lion_ne WHERE n <> 2 ORDER BY n NULLS FIRST, id LIMIT 30$$);
SELECT lion_no($$SELECT n, id FROM lion_ne WHERE n <> 6 ORDER BY n DESC NULLS FIRST, id DESC LIMIT 30$$);
SELECT lion_no($$SELECT s, id FROM lion_ne WHERE s <> '' AND g = 2 ORDER BY s, id LIMIT 10$$);
SELECT * FROM lion_nplan($$SELECT s, id FROM lion_ne WHERE s <> '' ORDER BY s LIMIT 10$$);

-- 7. A filter on the walked column: the rows of one entry hold one value,
--    so the first visible one decides for all of it, and one that fails
--    takes the rest with it unread - the '' entry after one row here.
SELECT lion_no($$SELECT s, id FROM lion_ne WHERE s LIKE 'p1%' ORDER BY s, id LIMIT 10$$);
SELECT lion_no($$SELECT s, id FROM lion_ne WHERE s LIKE 'p1%' ORDER BY s DESC, id DESC LIMIT 10$$);
SELECT lion_no($$SELECT s, id FROM lion_ne WHERE length(s) > 3 ORDER BY s, id LIMIT 10$$);
SELECT lion_no($$SELECT k, id FROM lion_ne WHERE k % 7 = 3 AND g = 1 ORDER BY k DESC, id DESC LIMIT 10$$);
SELECT lion_no($$SELECT n, id FROM lion_ne WHERE n IS NULL OR n > 4 ORDER BY n NULLS FIRST, id LIMIT 30$$);
SELECT lion_no($$SELECT n, id FROM lion_ne WHERE coalesce(n, 9) > 4 ORDER BY n DESC NULLS LAST, id DESC LIMIT 30$$);
SELECT * FROM lion_nplan($$SELECT s, id FROM lion_ne WHERE s LIKE 'p1%' ORDER BY s LIMIT 10$$);
SELECT * FROM lion_nrun($$SELECT s, id FROM lion_ne WHERE s LIKE 'p1%' ORDER BY s LIMIT 10$$);
-- ... never evaluated on a key alone: k = 0 is in deleted rows only, and
-- `100 / k` is not asked of it
CREATE TABLE lion_ne_z (id int PRIMARY KEY, k int) WITH (autovacuum_enabled = off);
INSERT INTO lion_ne_z SELECT i, i % 50 FROM generate_series(1, 20000) i;
CREATE INDEX lion_ne_z_k ON lion_ne_z USING lion (k);
VACUUM (FREEZE, ANALYZE) lion_ne_z;
DELETE FROM lion_ne_z WHERE k = 0;
SELECT lion_no($$SELECT k, id FROM lion_ne_z WHERE 100 / k > 3 ORDER BY k, id LIMIT 5$$);
-- ... and not taken over a type whose equal values may differ: 1.0 and
-- 1.00 are one entry of numeric, and the first row of it says nothing of
-- the other spelling
CREATE TABLE lion_ne_m (id int PRIMARY KEY, m numeric) WITH (autovacuum_enabled = off);
INSERT INTO lion_ne_m SELECT i, CASE WHEN i % 2 = 0 THEN 1.0 ELSE 1.00 END + i % 3
  FROM generate_series(1, 2000) i;
CREATE INDEX lion_ne_m_m ON lion_ne_m USING lion (m);
VACUUM (FREEZE, ANALYZE) lion_ne_m;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SELECT * FROM lion_nplan($$SELECT m, id FROM lion_ne_m WHERE m::text LIKE '%.00' ORDER BY m LIMIT 5$$);
SELECT count(*) FROM (SELECT m, id FROM lion_ne_m WHERE m::text LIKE '%.00' ORDER BY m LIMIT 700) s
 WHERE m::text LIKE '%.00';
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
DROP TABLE lion_ne_z, lion_ne_m;

DROP FUNCTION lion_no(text);
DROP FUNCTION lion_nrun(text);
DROP FUNCTION lion_nplan(text);
DROP FUNCTION lion_nq(text, text);
DROP TABLE lion_ne;
