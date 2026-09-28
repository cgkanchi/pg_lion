-- FK-side join pushdown, part five (DESIGN.md §27, "Every aggregate over the
-- node's rows"): counts of join pairs beside aggregates that need the
-- dimension rows themselves, in one query.
--
--   SELECT count(DISTINCT d.pk), count(*) FROM fact f JOIN dim d ON d.pk = f.fk
--   WHERE <f filters> AND <d filters>
--
-- The node emits one row per dimension row that joins and, for an inner join,
-- that row's count of join pairs beside it; core's plain Agg computes
-- count(DISTINCT), min, max and the like over the rows as they stand, and the
-- counts as lion_join_count() of the rows' counts, below a LionJoinAgg that
-- hands them up as the query's own.  A semi or anti join's rows are its result
-- rows, over which every aggregate is computed as it stands.  Every answer is
-- checked against the same query with the pushdown off, as a multiset in both
-- directions.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET default_statistics_target = 1000;

/*
 * lion_mj() runs a query through the pushdown - with every join method
 * disabled when force is set, so that a shape the cost model would not pick
 * is still EXERCISED - and again with the pushdown off and sequential scans
 * only, so that the reference reads no lion posting set, and compares the
 * two.  It reports the plan's form: counted rows (a LionJoinAgg over the
 * Agg), rows (the Agg straight over the node's rows), partial counts, or not
 * pushed down.
 */
CREATE FUNCTION lion_mj_form(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	how text := 'not pushed down';
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (LionJoinAgg)%' THEN
			how := 'counted rows';
		ELSIF ln LIKE '%Join Rows:%' AND how <> 'counted rows' THEN
			how := 'rows';
		ELSIF ln LIKE '%Custom Scan (LionCount)%' AND how = 'not pushed down' THEN
			how := 'partial counts';
		END IF;
	END LOOP;
	RETURN how;
END $$;

CREATE FUNCTION lion_mj(q text, force boolean DEFAULT true) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	how text;
	nrows bigint;
	ndiff bigint;
BEGIN
	IF force THEN
		PERFORM set_config('enable_hashjoin', 'off', true);
		PERFORM set_config('enable_mergejoin', 'off', true);
		PERFORM set_config('enable_nestloop', 'off', true);
	END IF;
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	how := lion_mj_form(q);
	EXECUTE format('CREATE TEMP TABLE lion_mj_on AS SELECT s::text AS r FROM (%s) s', q);

	/*
	 * The reference: the pushdown off, and sequential scans only - no bitmap,
	 * index or index-only scan, which could read the very lion posting sets
	 * the node reads and agree with it about a wrong answer.
	 */
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_nestloop', 'on', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	EXECUTE format('CREATE TEMP TABLE lion_mj_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM lion_mj_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_mj_on EXCEPT ALL SELECT * FROM lion_mj_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_mj_off EXCEPT ALL SELECT * FROM lion_mj_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lion_mj_on, lion_mj_off';

	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows', how, nrows);
END $$;

/* The answer itself, through the pushdown, joins disabled. */
CREATE FUNCTION lion_mj_val(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	RETURN QUERY EXECUTE format('SELECT s::text FROM (%s) s', q);
END $$;

/* A generic prepared plan, which keeps $n a Param, against the same with the pushdown off. */
CREATE FUNCTION lion_mj_prep(q text, args text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	how text;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('plan_cache_mode', 'force_generic_plan', true);
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE 'PREPARE lion_mjp_on AS ' || q;
	how := lion_mj_form('EXECUTE lion_mjp_on(' || args || ')');
	EXECUTE format('CREATE TEMP TABLE lion_mj_on AS EXECUTE lion_mjp_on(%s)', args);

	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_nestloop', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	EXECUTE 'PREPARE lion_mjp_off AS ' || q;
	EXECUTE format('CREATE TEMP TABLE lion_mj_off AS EXECUTE lion_mjp_off(%s)', args);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE 'DEALLOCATE lion_mjp_on';
	EXECUTE 'DEALLOCATE lion_mjp_off';

	EXECUTE 'SELECT count(*) FROM lion_mj_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_mj_on EXCEPT ALL SELECT * FROM lion_mj_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_mj_off EXCEPT ALL SELECT * FROM lion_mj_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lion_mj_on, lion_mj_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows', how, nrows);
END $$;

/*
 * lion_mj_par() runs a query with every join method disabled three ways: as
 * planned - in parallel, when the plan says so - with
 * max_parallel_workers_per_gather at 0, and with the pushdown off and
 * sequential scans only, and compares both of the first two with the third.
 */
CREATE FUNCTION lion_mj_par(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	how text := 'not pushed down';
	nrows bigint;
	ndiff bigint;
	sdiff bigint;
BEGIN
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Parallel Custom Scan (LionCount)%' THEN
			how := 'parallel';
		ELSIF ln LIKE '%Custom Scan (LionCount)%' AND how <> 'parallel' THEN
			how := 'serial';
		END IF;
	END LOOP;
	how := how || ', ' || lion_mj_form(q);
	EXECUTE format('CREATE TEMP TABLE lion_mj_par AS SELECT s::text AS r FROM (%s) s', q);

	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	EXECUTE format('CREATE TEMP TABLE lion_mj_ser AS SELECT s::text AS r FROM (%s) s', q);

	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_nestloop', 'on', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	EXECUTE format('CREATE TEMP TABLE lion_mj_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('max_parallel_workers_per_gather', '2', true);

	EXECUTE 'SELECT count(*) FROM lion_mj_par' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_mj_par EXCEPT ALL SELECT * FROM lion_mj_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_mj_off EXCEPT ALL SELECT * FROM lion_mj_par) b)'
		INTO ndiff;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_mj_ser EXCEPT ALL SELECT * FROM lion_mj_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_mj_off EXCEPT ALL SELECT * FROM lion_mj_ser) b)'
		INTO sdiff;
	EXECUTE 'DROP TABLE lion_mj_par, lion_mj_ser, lion_mj_off';

	IF ndiff <> 0 OR sdiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ in parallel, %s serially', ndiff, sdiff);
	END IF;
	RETURN format('%s, %s rows', how, nrows);
END $$;

/* One counter of the node's EXPLAIN ANALYZE output, joins disabled. */
CREATE FUNCTION lion_mj_counter(q text, counter text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	val bigint;
BEGIN
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF btrim(split_part(ln, ':', 1)) = counter THEN
			val := btrim(split_part(ln, ':', 2))::bigint;
		END IF;
	END LOOP;
	RETURN val;
END $$;

/*
 * The dimension: 3000 rows over some twenty pages, so that two workers both
 * have a share, int8 primary key 1..3000; kept true on two rows in three;
 * attr 0..6 with NULL on every 13th, small an int2 0..4 with NULL on every
 * 7th, five groups, and a name with NULL on every 11th.  The fact: 60000 rows
 * in random key order; fk over 1..3600, so keys 3001..3600 join no dimension
 * row, NULL on every 40th row, and no row at all for the keys that are
 * multiples of 17, so those dimension rows join no fact row; fk4 is fk as
 * int4.  hot is true on three rows in four, x has 10 values, y 5 and NULLs.
 */
CREATE TABLE lion_md (
	pk		int8	PRIMARY KEY,
	kept	bool	NOT NULL,
	attr	int,
	small	int2,
	grp		text	NOT NULL,
	name	text
) WITH (parallel_workers = 2);
INSERT INTO lion_md
SELECT i, i % 3 <> 0, CASE WHEN i % 13 = 0 THEN NULL ELSE i % 7 END,
	   CASE WHEN i % 7 = 0 THEN NULL ELSE (i % 5)::int2 END,
	   'g' || (i % 5),
	   CASE WHEN i % 11 = 0 THEN NULL ELSE 'n' || lpad((i % 97)::text, 2, '0') END
FROM generate_series(1, 3000) i;

CREATE TABLE lion_mf (
	id		int		NOT NULL,
	fk		int8,
	fk4		int4,
	hot		bool	NOT NULL,
	x		int		NOT NULL,
	y		int
);
INSERT INTO lion_mf
SELECT i, k, k, abs(hashint4(i + 3000000)) % 4 <> 0, h2 % 10,
	   CASE WHEN h2 % 9 = 0 THEN NULL ELSE h2 / 10 % 5 END
FROM (SELECT i,
			 CASE WHEN i % 40 = 0 THEN NULL
				  WHEN (abs(hashint4(i)) % 3600 + 1) % 17 = 0 THEN NULL
				  ELSE abs(hashint4(i)) % 3600 + 1 END AS k,
			 abs(hashint4(i + 1000000)) AS h2
	  FROM generate_series(1, 60000) i) s;
CREATE INDEX lion_mf_fk ON lion_mf USING lion (fk);
CREATE INDEX lion_mf_fk4 ON lion_mf USING lion (fk4);
CREATE INDEX lion_mf_hot ON lion_mf USING lion (hot);
CREATE INDEX lion_mf_x ON lion_mf USING lion (x);
CREATE INDEX lion_mf_y ON lion_mf USING lion (y);

-- a dimension whose key is NOT unique: every key twice, and NULLs
CREATE TABLE lion_mdn (k int8, attr int NOT NULL, val int) WITH (parallel_workers = 2);
INSERT INTO lion_mdn
SELECT CASE WHEN i % 50 = 0 THEN NULL ELSE i % 1500 + 1 END, i % 4,
	   CASE WHEN i % 6 = 0 THEN NULL ELSE i % 9 END
FROM generate_series(1, 3000) i;
CREATE INDEX lion_mdn_k ON lion_mdn (k);
VACUUM (FREEZE, ANALYZE) lion_mf;
VACUUM (FREEZE, ANALYZE) lion_md;
VACUUM (FREEZE, ANALYZE) lion_mdn;

-- ---- 1. counts beside count(DISTINCT) ---------------------------------------
-- how many dimension rows join, and how many join pairs there are
SELECT lion_mj('SELECT count(DISTINCT d.pk), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.hot AND d.kept');
SELECT lion_mj_val('SELECT count(DISTINCT d.pk), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.hot AND d.kept');
-- the filtered dimension rows as a CTE, which the planner inlines
SELECT lion_mj('WITH c AS (SELECT pk FROM lion_md WHERE kept) SELECT count(DISTINCT c.pk), count(*) FROM c JOIN lion_mf f ON f.fk = c.pk WHERE f.hot');
-- the fact's key counted distinct; count(1) and count of either side of the key
SELECT lion_mj('SELECT count(DISTINCT f.fk), count(1), count(f.fk), count(d.pk), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 3');
SELECT lion_mj('SELECT count(DISTINCT d.attr), count(DISTINCT d.grp), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.y IN (1, 2) AND d.grp <> ''g3''');
-- an int4 fk against the int8 key: counts of either side of it beside a
-- dimension column counted distinct
SELECT lion_mj('SELECT count(DISTINCT d.attr), count(f.fk4), count(d.pk), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk4 WHERE f.x = 3');
-- grouped, with a HAVING on either kind of aggregate, ORDER BY and LIMIT
SELECT lion_mj('SELECT d.attr, count(DISTINCT d.pk), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.hot AND d.kept GROUP BY d.attr');
SELECT lion_mj('SELECT d.attr, count(DISTINCT d.pk), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.hot AND d.kept GROUP BY d.attr HAVING count(*) > 1800');
SELECT lion_mj('SELECT d.attr, count(DISTINCT d.pk), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.hot AND d.kept GROUP BY d.attr HAVING count(DISTINCT d.pk) < 248');
SELECT lion_mj('SELECT d.grp, count(DISTINCT d.attr) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x IN (1, 2) GROUP BY d.grp HAVING count(*) > 1850');
SELECT lion_mj('SELECT d.grp, d.kept, count(*) - count(DISTINCT d.pk) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.y = 1 GROUP BY d.grp, d.kept ORDER BY count(*) DESC, 1, 2 LIMIT 3');
SELECT lion_mj('SELECT upper(d.grp), count(*), count(DISTINCT d.attr) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE NOT f.hot GROUP BY upper(d.grp)');
-- expressions of aggregates, and one aggregate twice
SELECT lion_mj('SELECT count(*) * 2 + count(DISTINCT d.pk), count(*), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 4');
-- a GROUP BY folded to a constant, over something and over nothing
SELECT lion_mj('SELECT d.attr, count(DISTINCT d.pk), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 3 AND d.attr = 2 GROUP BY d.attr');
SELECT lion_mj('SELECT d.attr, count(DISTINCT d.pk), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 77 AND d.attr = 2 GROUP BY d.attr');
-- ungrouped over nothing: one row, zeros
SELECT lion_mj('SELECT count(DISTINCT d.pk), count(*), count(d.attr) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 77');
SELECT lion_mj_val('SELECT count(DISTINCT d.pk), count(*), count(d.attr), sum(d.attr), max(d.name) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 77');
SELECT lion_mj_val('SELECT count(DISTINCT d.pk), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE d.pk = 17');

-- ---- 2. min, max and the others that do not see multiplicity -----------------
SELECT lion_mj('SELECT min(d.name), max(d.name), min(d.pk), max(d.attr), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 3');
SELECT lion_mj('SELECT d.grp, min(d.attr), max(d.small), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk GROUP BY d.grp');
SELECT lion_mj('SELECT d.kept, max(d.attr + d.small), min(upper(d.name)), count(DISTINCT d.small) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.y = 3 GROUP BY d.kept');
SELECT lion_mj('SELECT bool_and(d.kept), bool_or(d.attr > 5), every(d.pk < 2900), bit_and(d.attr), bit_or(d.small), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.y = 1');
-- alone they need no counts: the rows as they stand
SELECT lion_mj('SELECT max(d.name), min(d.attr) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.hot');
SELECT lion_mj('SELECT d.grp, max(d.name) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 5 GROUP BY d.grp');
-- dimension rows whose values are all NULL
SELECT lion_mj_val('SELECT min(d.name), max(d.attr), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE d.pk IN (143, 286)');

-- ---- 3. count and sum of a dimension column --------------------------------
-- count(col) is the pairs whose col is not NULL
SELECT lion_mj('SELECT count(d.attr), count(d.name), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 3');
SELECT lion_mj('SELECT d.grp, count(d.attr), count(d.small), count(DISTINCT d.attr) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk GROUP BY d.grp');
SELECT lion_mj('SELECT count(d.attr + d.small), count(NULLIF(d.grp, ''g1'')), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.hot');
SELECT lion_mj('SELECT d.attr, count(d.name), max(d.pk) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x < 3 GROUP BY d.attr HAVING count(d.name) > 1690');
-- ... and 0 where every joining row's col is NULL
SELECT lion_mj_val('SELECT count(d.attr), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE d.attr IS NULL');
-- sum of an int2 or int4 column is the column times the pairs
SELECT lion_mj('SELECT sum(d.attr), sum(d.small), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 3');
SELECT lion_mj('SELECT d.grp, sum(d.attr), sum(d.small * 2::int2), count(DISTINCT d.pk) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk GROUP BY d.grp HAVING sum(d.attr) > 25400');
-- ... and NULL where every joining row's column is NULL, as sum() is
SELECT lion_mj_val('SELECT sum(d.attr), count(d.attr), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE d.attr IS NULL');

-- ---- 4. semi and anti joins: their rows are the join's rows -------------------
SELECT lion_mj('SELECT count(*), count(DISTINCT d.attr), min(d.name), count(d.name), sum(d.attr) FROM lion_md d WHERE EXISTS (SELECT 1 FROM lion_mf f WHERE f.fk = d.pk AND f.x = 3)');
SELECT lion_mj('SELECT d.grp, count(*), max(d.attr), count(d.attr), bool_or(d.kept) FROM lion_md d WHERE d.pk IN (SELECT f.fk FROM lion_mf f WHERE f.hot AND f.y = 2) GROUP BY d.grp');
SELECT lion_mj('SELECT count(1), count(d.pk), count(DISTINCT d.small) FROM lion_md d WHERE d.kept AND EXISTS (SELECT 1 FROM lion_mf f WHERE f.fk = d.pk AND f.x = 8)');
SELECT lion_mj('SELECT count(*), count(d.attr), min(d.name), sum(d.small), bool_and(d.kept) FROM lion_md d WHERE NOT EXISTS (SELECT 1 FROM lion_mf f WHERE f.fk = d.pk AND f.x = 3)');
SELECT lion_mj('SELECT d.attr, count(*), count(DISTINCT d.grp), max(d.name) FROM lion_md d WHERE NOT EXISTS (SELECT 1 FROM lion_mf f WHERE f.fk = d.pk) GROUP BY d.attr');
SELECT lion_mj('SELECT count(*), count(d.pk), max(d.pk) FROM lion_md d WHERE NOT EXISTS (SELECT 1 FROM lion_mf f WHERE f.fk = d.pk AND f.x = 77)');
-- duplicated and NULL keys: each dimension row is its own row, and a NULL key
-- is a row of the anti join and no value of count(k)
SELECT lion_mj('SELECT count(*), count(d.k), count(DISTINCT d.k), max(d.k) FROM lion_mdn d WHERE NOT EXISTS (SELECT 1 FROM lion_mf f WHERE f.fk = d.k AND f.x = 2)');
SELECT lion_mj('SELECT d.attr, count(*), count(d.k), count(d.val), sum(d.val) FROM lion_mdn d WHERE NOT EXISTS (SELECT 1 FROM lion_mf f WHERE f.fk = d.k AND f.x = 2 AND f.hot) GROUP BY d.attr');
SELECT lion_mj('SELECT count(*), count(d.k), count(DISTINCT d.attr), sum(d.val), min(d.val) FROM lion_mdn d WHERE EXISTS (SELECT 1 FROM lion_mf f WHERE f.fk = d.k AND f.y = 4)');
-- the forward semi join over a non-unique key, counted over its distinct keys,
-- which carry their counts
SELECT lion_mj('SELECT count(*), count(DISTINCT f.fk) FROM lion_mf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_mdn d WHERE d.k = f.fk AND d.attr = 1)');
SELECT lion_mj('SELECT count(DISTINCT f.fk), count(f.fk), count(1) FROM lion_mf f WHERE f.fk IN (SELECT d.k FROM lion_mdn d WHERE d.attr < 2)');
SELECT lion_mj('SELECT count(DISTINCT f.fk), count(*) FROM lion_mf f WHERE f.x = 77 AND EXISTS (SELECT 1 FROM lion_mdn d WHERE d.k = f.fk)');

-- ---- 5. declined ---------------------------------------------------------------
-- a fact column other than the key
SELECT lion_mj('SELECT count(DISTINCT d.pk), sum(f.x) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.hot');
SELECT lion_mj('SELECT count(DISTINCT d.pk), count(f.y) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.hot');
SELECT lion_mj('SELECT max(f.x), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.hot');
-- min and max of the fact's key: the dimension's key, which answers the
-- same, is taken
SELECT lion_mj('SELECT min(f.fk), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 3');
SELECT lion_mj('SELECT min(d.pk), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 3');
-- FILTER, ORDER BY in an aggregate, DISTINCT in another aggregate
SELECT lion_mj('SELECT count(*) FILTER (WHERE d.attr = 1), count(DISTINCT d.pk) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 3');
SELECT lion_mj('SELECT max(d.name) FILTER (WHERE d.attr = 1), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 3');
SELECT lion_mj('SELECT string_agg(d.name, '','' ORDER BY d.name), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE d.pk < 30');
SELECT lion_mj('SELECT sum(DISTINCT d.attr), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 3');
-- aggregates that see multiplicity and are not counts or int2/int4 sums
SELECT lion_mj('SELECT avg(d.attr), count(DISTINCT d.pk) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 3');
SELECT lion_mj('SELECT sum(d.pk), count(DISTINCT d.attr) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 3');
SELECT lion_mj('SELECT bit_xor(d.attr), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 3');
SELECT lion_mj('SELECT count(*), array_length(array_agg(d.attr), 1) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 3');
-- a volatile expression
SELECT lion_mj('SELECT max(d.attr + (random() * 0)::int), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 3');

-- ---- 6. the plans --------------------------------------------------------------
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
-- the counts rewritten over the rows' own counts, below the LionJoinAgg that
-- hands them up as the query's
EXPLAIN (VERBOSE, COSTS OFF) SELECT count(DISTINCT d.pk), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.hot AND d.kept;
EXPLAIN (VERBOSE, COSTS OFF) SELECT d.attr, count(DISTINCT d.pk), count(*), count(d.name), sum(d.small), max(d.name) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 3 GROUP BY d.attr HAVING count(*) > 10 ORDER BY count(*) DESC;
-- no DISTINCT: hashed, with no Sort
EXPLAIN (COSTS OFF) SELECT d.grp, count(*), min(d.attr), count(d.attr) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 3 GROUP BY d.grp;
-- the fact's key counted distinct: the rows carry the dimension's key in its place
EXPLAIN (VERBOSE, COSTS OFF) SELECT count(DISTINCT f.fk), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 3;
-- the forward semi join's distinct keys, each with its count
EXPLAIN (COSTS OFF) SELECT count(*), count(DISTINCT f.fk) FROM lion_mf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_mdn d WHERE d.k = f.fk AND d.attr = 1);
-- a semi or anti join's rows are the join's own: the aggregates as they stand
EXPLAIN (VERBOSE, COSTS OFF) SELECT count(*), count(DISTINCT d.attr), count(d.name), sum(d.attr) FROM lion_md d WHERE EXISTS (SELECT 1 FROM lion_mf f WHERE f.fk = d.pk AND f.x = 3);
EXPLAIN (COSTS OFF) SELECT count(*), count(d.k) FROM lion_mdn d WHERE NOT EXISTS (SELECT 1 FROM lion_mf f WHERE f.fk = d.k AND f.x = 2);
-- counts alone are partial counts, as ever: summed in the node, or one a
-- dimension row under a GROUP BY
EXPLAIN (COSTS OFF) SELECT count(*), count(d.pk) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.hot AND d.kept;
EXPLAIN (COSTS OFF) SELECT d.attr, count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.hot AND d.kept GROUP BY d.attr;
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_nestloop;
-- an inner join's counted rows are counts, not existence tests: they read
-- what the partial counts read
SELECT lion_mj_counter('SELECT count(DISTINCT d.pk), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 3', 'Containers Visited')
	= lion_mj_counter('SELECT count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 3', 'Containers Visited') AS counted_rows_count;
SELECT lion_mj_counter('SELECT count(DISTINCT d.pk) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 3', 'Containers Visited')
	< lion_mj_counter('SELECT count(DISTINCT d.pk), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 3', 'Containers Visited') AS rows_alone_test;
-- the cost model's choice, nothing disabled: counts alone take the partial
-- counts, as they did, and the same query with an aggregate that needs the
-- rows is pushed down where they are, and left to the hash join where they
-- are not
SELECT lion_mj('SELECT count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.hot AND d.kept AND d.attr = 2', false);
SELECT lion_mj('SELECT count(DISTINCT d.pk), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.hot AND d.kept AND d.attr = 2', false);
SELECT lion_mj('SELECT d.grp, count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 3 AND d.attr = 2 AND d.small = 1 GROUP BY d.grp', false);
SELECT lion_mj('SELECT d.grp, count(*), max(d.name) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 3 AND d.attr = 2 AND d.small = 1 GROUP BY d.grp', false);
SELECT lion_mj('SELECT d.grp, count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 3 AND d.attr = 2 GROUP BY d.grp', false);
SELECT lion_mj('SELECT d.grp, count(*), max(d.name) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 3 AND d.attr = 2 GROUP BY d.grp', false);

-- ---- 7. parameters and rescans ----------------------------------------------
SELECT lion_mj_prep('SELECT count(DISTINCT d.pk) AS nd, count(*) AS n FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = $1 AND d.grp = $2', '3, ''g1''');
SELECT lion_mj_prep('SELECT d.attr, count(DISTINCT d.pk) AS nd, count(*) AS n, sum(d.small) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = $1 GROUP BY d.attr', '3');
SELECT lion_mj_prep('SELECT d.attr, count(DISTINCT d.pk) AS nd, count(*) AS n, sum(d.small) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = $1 GROUP BY d.attr', 'NULL');
SELECT lion_mj_prep('SELECT count(DISTINCT d.pk) AS nd, count(*) AS n FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = $1', 'NULL');
SELECT lion_mj_prep('SELECT count(*), max(d.name) FROM lion_md d WHERE d.grp = $2 AND EXISTS (SELECT 1 FROM lion_mf f WHERE f.fk = d.pk AND f.y = $1)', '2, ''g4''');
-- a correlated subquery: the LionJoinAgg, its Agg and the node rescanned per
-- outer row, with a new fact filter and with a new dimension filter
SELECT lion_mj('SELECT g, (SELECT count(DISTINCT d.pk) * 100000 + count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = g.g AND d.kept) FROM generate_series(0, 10) g');
SELECT lion_mj('SELECT g, (SELECT count(d.name) - count(DISTINCT d.name) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE d.attr = g.g AND f.y = 2) FROM generate_series(0, 7) g');

-- ---- 8. parallel: a plain Agg above a Gather of the rows ----------------------
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET parallel_leader_participation = off;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
EXPLAIN (COSTS OFF) SELECT count(DISTINCT d.pk), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.hot AND d.kept;
EXPLAIN (COSTS OFF) SELECT d.grp, count(*), max(d.name) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 3 GROUP BY d.grp;
-- counts alone keep the partial counts: one summed row a participant
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.hot AND d.kept;
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_nestloop;
SELECT lion_mj_par('SELECT count(DISTINCT d.pk), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.hot AND d.kept');
SELECT lion_mj_par('SELECT d.attr, count(DISTINCT d.pk), count(*), count(d.name), sum(d.small), max(d.name) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk GROUP BY d.attr HAVING count(*) > 10');
SELECT lion_mj_par('SELECT d.grp, count(*), min(d.attr), count(d.attr) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 3 GROUP BY d.grp');
SELECT lion_mj_par('SELECT count(DISTINCT f.fk), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.y IS NULL');
SELECT lion_mj_par('SELECT count(*), count(DISTINCT d.attr), max(d.name) FROM lion_md d WHERE EXISTS (SELECT 1 FROM lion_mf f WHERE f.fk = d.pk AND f.x = 3)');
SELECT lion_mj_par('SELECT count(*), count(d.k), min(d.attr) FROM lion_mdn d WHERE NOT EXISTS (SELECT 1 FROM lion_mf f WHERE f.fk = d.k AND f.x = 2)');
SELECT lion_mj_par('SELECT count(*), count(DISTINCT f.fk) FROM lion_mf f WHERE EXISTS (SELECT 1 FROM lion_mdn d WHERE d.k = f.fk AND d.attr = 1)');
-- every dimension row is looked up once, whichever participant it went to
SELECT lion_mj_counter('SELECT count(DISTINCT d.pk), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.hot', 'Join Keys Looked Up');
-- the serial plan run whole in a worker (debug_parallel_query)
ALTER TABLE lion_md SET (parallel_workers = 0);
SET debug_parallel_query = on;
SELECT lion_mj_par('SELECT d.attr, count(DISTINCT d.pk), count(*), sum(d.attr) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 3 GROUP BY d.attr');
RESET debug_parallel_query;
ALTER TABLE lion_md SET (parallel_workers = 2);
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET min_parallel_index_scan_size;
RESET parallel_leader_participation;

-- ---- 9. the aggregates themselves ------------------------------------------
-- lion_join_count() is count(): 0 over no rows, NULLs skipped; lion_join_sum()
-- is sum(): NULL over no rows or NULLs alone; both fail past int8, as count()
-- and sum() of an int4 do
SELECT lion_join_count(v), lion_join_sum(v) FROM (VALUES (3::int8), (NULL), (4)) t(v);
SELECT lion_join_count(v), lion_join_sum(v) FROM (VALUES (3::int8)) t(v) WHERE false;
SELECT lion_join_count(v), lion_join_sum(v) FROM (VALUES (NULL::int8)) t(v);
SELECT lion_join_count(v) FROM (VALUES (9223372036854775807::int8), (1)) t(v);
SELECT lion_join_sum(v) FROM (VALUES (-9223372036854775807::int8), (-2)) t(v);

-- ---- 10. privileges ------------------------------------------------------------
-- The Agg asks EXECUTE on the aggregates it runs, lion_join_count() among
-- them, of whoever runs it: a role that may not run it gets the ordinary plan.
CREATE ROLE lion_mj_reader;
GRANT SELECT ON lion_mf, lion_md TO lion_mj_reader;
REVOKE EXECUTE ON FUNCTION lion_join_count(bigint) FROM PUBLIC;
SET ROLE lion_mj_reader;
SELECT lion_mj('SELECT count(DISTINCT d.pk), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.hot AND d.kept');
SELECT lion_mj('SELECT count(DISTINCT d.pk), max(d.name) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.hot AND d.kept');
RESET ROLE;
GRANT EXECUTE ON FUNCTION lion_join_count(bigint) TO PUBLIC;
SET ROLE lion_mj_reader;
SELECT lion_mj('SELECT count(DISTINCT d.pk), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.hot AND d.kept');
RESET ROLE;
-- ... and the planner finds them with no USAGE on the extension's schema,
-- which running them does not ask for either
SELECT n.nspname AS lion_schema FROM pg_extension e JOIN pg_namespace n ON n.oid = e.extnamespace
 WHERE e.extname = 'pg_lion' \gset
CREATE SCHEMA lion_mj_ext;
ALTER EXTENSION pg_lion SET SCHEMA lion_mj_ext;
SET ROLE lion_mj_reader;
SELECT lion_mj('SELECT count(DISTINCT d.pk), count(*), sum(d.small) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.hot AND d.kept');
RESET ROLE;
ALTER EXTENSION pg_lion SET SCHEMA :"lion_schema";
DROP SCHEMA lion_mj_ext;
REVOKE ALL ON lion_mf, lion_md FROM lion_mj_reader;
DROP ROLE lion_mj_reader;

-- ---- 11. a dirty heap: deletes and updates on both sides, not yet vacuumed ----
DELETE FROM lion_mf WHERE fk = 7;
DELETE FROM lion_mf WHERE fk = 8 AND x < 5;
UPDATE lion_mf SET fk = 9, fk4 = 9 WHERE fk = 10 AND x = 1;
UPDATE lion_mf SET x = 3 WHERE id % 97 = 0;
UPDATE lion_mf SET fk = NULL, fk4 = NULL WHERE fk = 11;
INSERT INTO lion_mf SELECT 60000 + i, 34, 34, true, 3, 1 FROM generate_series(1, 5) i;
DELETE FROM lion_md WHERE pk IN (12, 1200, 2400);
UPDATE lion_md SET attr = NULL, name = 'zz' WHERE pk IN (1, 2, 4, 1500);
UPDATE lion_md SET small = 4::int2, kept = false WHERE pk IN (5, 6);
SELECT lion_mj('SELECT count(DISTINCT d.pk), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.hot AND d.kept');
SELECT lion_mj('SELECT d.attr, count(DISTINCT d.pk), count(*), count(d.name), sum(d.small), max(d.name) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 3 GROUP BY d.attr');
SELECT lion_mj('SELECT count(*), count(d.attr), max(d.name) FROM lion_md d WHERE NOT EXISTS (SELECT 1 FROM lion_mf f WHERE f.fk = d.pk AND f.x = 3)');
SELECT lion_mj_counter('SELECT count(DISTINCT d.pk), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 3', 'Heap TIDs Rechecked') > 0 AS dirty_heap_rechecks;
VACUUM lion_mf;
VACUUM lion_md;
SELECT lion_mj('SELECT count(DISTINCT d.pk), count(*) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.hot AND d.kept');
SELECT lion_mj('SELECT d.attr, count(DISTINCT d.pk), count(*), count(d.name), sum(d.small), max(d.name) FROM lion_mf f JOIN lion_md d ON d.pk = f.fk WHERE f.x = 3 GROUP BY d.attr');

DROP TABLE lion_mf, lion_md, lion_mdn;
DROP FUNCTION lion_mj(text, boolean);
DROP FUNCTION lion_mj_form(text);
DROP FUNCTION lion_mj_val(text);
DROP FUNCTION lion_mj_prep(text, text);
DROP FUNCTION lion_mj_par(text);
DROP FUNCTION lion_mj_counter(text, text);
