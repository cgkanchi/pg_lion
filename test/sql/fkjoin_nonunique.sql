-- FK-side join pushdown, part five (DESIGN.md §27, "Forward semi joins over a
-- non-unique key"): the fact rows whose key some qualifying dimension row
-- has, when nothing proves the dimension's key unique.
--
--   SELECT count(*) FROM fact f
--   WHERE EXISTS (SELECT 1 FROM dim d WHERE d.k = f.fk AND <d filters>)
--
-- The semi join stays one (reduce_unique_semijoins() has nothing to reduce
-- it with), and the node counts it as an inner join over the dimension's
-- DISTINCT keys: it sorts the child's keys, keeps each once, and counts each
-- key's fact rows.  Every answer is checked against the same query with the
-- pushdown off, as a multiset in both directions.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET default_statistics_target = 1000;
SET max_parallel_workers_per_gather = 0;

/*
 * lion_nj() runs a query through the pushdown - with every join method
 * disabled when force is set, so that a shape the cost model would not pick
 * is still EXERCISED - and again with the pushdown off and the planner left
 * alone, and compares the two.  It reports whether the node was used.
 */
CREATE FUNCTION lion_nj(q text, force boolean DEFAULT true) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	pushed boolean := false;
	nrows bigint;
	ndiff bigint;
BEGIN
	IF force THEN
		PERFORM set_config('enable_hashjoin', 'off', true);
		PERFORM set_config('enable_mergejoin', 'off', true);
		PERFORM set_config('enable_nestloop', 'off', true);
	END IF;
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			pushed := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lion_nj_on AS SELECT s::text AS r FROM (%s) s', q);

	/* the reference: the ordinary plan, with the pushdown off */
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_nestloop', 'on', true);
	EXECUTE format('CREATE TEMP TABLE lion_nj_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM lion_nj_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_nj_on EXCEPT ALL SELECT * FROM lion_nj_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_nj_off EXCEPT ALL SELECT * FROM lion_nj_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lion_nj_on, lion_nj_off';

	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN pushed THEN 'pushed down' ELSE 'not pushed down' END,
				  nrows);
END $$;

/* The answer itself, through the pushdown, joins disabled. */
CREATE FUNCTION lion_nj_val(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	RETURN QUERY EXECUTE format('SELECT s::text FROM (%s) s', q);
END $$;

/* Which plan the cost model picks, with nothing disabled. */
CREATE FUNCTION lion_nj_pick(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	how text := 'not pushed down';
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Parallel Custom Scan (LionCount)%' THEN
			how := 'pushed down, parallel';
		ELSIF ln LIKE '%Custom Scan (LionCount)%' AND how = 'not pushed down' THEN
			how := 'pushed down';
		END IF;
	END LOOP;
	RETURN how;
END $$;

/* A generic prepared plan, which keeps $n a Param, against the same with the pushdown off. */
CREATE FUNCTION lion_nj_prep(q text, args text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	pushed boolean := false;
	parallel boolean := false;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('plan_cache_mode', 'force_generic_plan', true);
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE 'PREPARE lion_njp_on AS ' || q;
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) EXECUTE lion_njp_on(' || args || ')' LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			pushed := true;
		END IF;
		IF ln LIKE '%Parallel Custom Scan (LionCount)%' THEN
			parallel := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lion_nj_on AS EXECUTE lion_njp_on(%s)', args);

	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_nestloop', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE 'PREPARE lion_njp_off AS ' || q;
	EXECUTE format('CREATE TEMP TABLE lion_nj_off AS EXECUTE lion_njp_off(%s)', args);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE 'DEALLOCATE lion_njp_on';
	EXECUTE 'DEALLOCATE lion_njp_off';

	EXECUTE 'SELECT count(*) FROM lion_nj_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_nj_on EXCEPT ALL SELECT * FROM lion_nj_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_nj_off EXCEPT ALL SELECT * FROM lion_nj_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lion_nj_on, lion_nj_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN parallel THEN 'pushed down, parallel'
					   WHEN pushed THEN 'pushed down' ELSE 'not pushed down' END,
				  nrows);
END $$;

/* One counter of the node's EXPLAIN ANALYZE output, joins disabled. */
CREATE FUNCTION lion_nj_counter(q text, counter text) RETURNS bigint
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
 * EXPLAIN down to the node, joins disabled: the dimension's own plan below it
 * is whatever each major makes of the dimension's scan (16 plans some of them
 * differently), and is not what these tests are about, so it is printed as
 * one line; and 18's note on a disabled node is not printed at all.
 */
CREATE FUNCTION lion_nj_explain(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	seen boolean := false;
BEGIN
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		CONTINUE WHEN ln ~ '^\s*Disabled: true$';
		IF seen AND ln ~ '^\s*->' THEN
			RETURN NEXT regexp_replace(ln, '->.*$', '->  (the dimension''s plan)');
			RETURN;
		END IF;
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			seen := true;
		END IF;
		RETURN NEXT ln;
	END LOOP;
END $$;

/*
 * The fact: 40000 rows in random key order; fk over 1..360, NULL on every
 * 40th row, and no row at all for the keys that are multiples of 17.  fk4 is
 * fk as int4 and tk as text; x has 10 values, y 5 and NULLs, t 20, doc two
 * lexemes a row.
 */
CREATE TABLE lion_nf (
	id		int		NOT NULL,
	fk		int8,
	fk4		int4,
	tk		text,
	x		int		NOT NULL,
	y		int,
	t		text	NOT NULL,
	doc		tsvector NOT NULL
);
INSERT INTO lion_nf
SELECT i, k, k, 'k' || k, h2 % 10, CASE WHEN h2 % 9 = 0 THEN NULL ELSE h2 / 10 % 5 END,
	   't' || (h3 % 20),
	   array_to_tsvector(ARRAY['a' || (h3 % 4), 'b' || (h3 / 4 % 6)])
FROM (SELECT i,
			 CASE WHEN i % 40 = 0 THEN NULL
				  WHEN (abs(hashint4(i)) % 360 + 1) % 17 = 0 THEN NULL
				  ELSE abs(hashint4(i)) % 360 + 1 END AS k,
			 abs(hashint4(i + 1000000)) AS h2,
			 abs(hashint4(i + 2000000)) AS h3
	  FROM generate_series(1, 40000) i) s;
CREATE INDEX lion_nf_fk ON lion_nf USING lion (fk);
CREATE INDEX lion_nf_fk4 ON lion_nf USING lion (fk4);
CREATE INDEX lion_nf_tk ON lion_nf USING lion (tk);
CREATE INDEX lion_nf_x ON lion_nf USING lion (x);
CREATE INDEX lion_nf_yt ON lion_nf USING lion (y, t);
CREATE INDEX lion_nf_doc ON lion_nf USING lion (doc);

/*
 * The dimension, whose key nothing proves unique: 900 rows over the keys
 * 1..400, most of them two or three times, so keys 361..400 have no fact rows
 * (and neither have the multiples of 17), and NULL on every 30th row.  k4 is
 * k as int4 and tk as text; attr 0..5, a region and a group, a lion index on
 * those two for the dimension's own filters, and a plain btree on the key.
 */
CREATE TABLE lion_nd (
	k		int8,
	k4		int4,
	tk		text,
	attr	int		NOT NULL,
	region	text	NOT NULL,
	grp		text	NOT NULL
);
INSERT INTO lion_nd
SELECT k, k, 'k' || k, i % 6,
	   CASE i % 3 WHEN 0 THEN 'eu' WHEN 1 THEN 'us' ELSE 'ap' END,
	   'g' || (i % 5)
FROM (SELECT i, CASE WHEN i % 30 = 0 THEN NULL ELSE (i * 7) % 400 + 1 END AS k
	  FROM generate_series(1, 900) i) s;
CREATE INDEX lion_nd_rg ON lion_nd USING lion (region, grp);
CREATE INDEX lion_nd_k ON lion_nd (k);

-- heavy duplication: 3000 rows over six keys (153 has no fact rows), and NULLs
CREATE TABLE lion_ndh (k int8, attr int NOT NULL);
INSERT INTO lion_ndh
SELECT CASE WHEN i % 97 = 0 THEN NULL ELSE (i % 6) * 50 + 3 END, i % 5
FROM generate_series(1, 3000) i;
-- every row the same key
CREATE TABLE lion_nda (k int8, attr int NOT NULL);
INSERT INTO lion_nda SELECT 7, i % 4 FROM generate_series(1, 400) i;
VACUUM ANALYZE lion_nf;
VACUUM ANALYZE lion_nd;
VACUUM ANALYZE lion_ndh;
VACUUM ANALYZE lion_nda;

-- ---- 1. the plan ---------------------------------------------------------------
-- the keys are sorted and each is counted once; the partial counts are added
-- up by core's Finalize Aggregate
SELECT * FROM lion_nj_explain('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.region = ''eu'')');
SELECT * FROM lion_nj_explain('SELECT count(*) FROM lion_nf f WHERE f.fk IN (SELECT d.k FROM lion_nd d WHERE d.attr = 2)');
-- count(DISTINCT) of the fact's key: one row per distinct key with a match,
-- sorted and aggregated by core
SELECT * FROM lion_nj_explain('SELECT count(DISTINCT f.fk) FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.region = ''eu'')');

-- ---- 2. the answers --------------------------------------------------------------
-- EXISTS and IN, with fact filters of every kind
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.region = ''eu'')');
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND f.fk IN (SELECT d.k FROM lion_nd d WHERE d.region = ''eu'')');
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE f.t IN (''t1'', ''t2'') AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.region = ''us'' AND d.grp IN (''g1'', ''g3''))');
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE f.doc @@ ''(a0 | a1) & b2''::tsquery AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.region = ''ap'')');
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE (f.x = 1 OR f.t = ''t4'') AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.grp = ''g2'')');
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND f.y IS NOT NULL AND f.fk IN (SELECT d.k FROM lion_nd d WHERE d.attr < 3)');
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE f.y IS NULL AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.attr = 2)');
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE f.y IS NOT NULL AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.attr = 2)');
-- no fact filter, no dimension filter, neither
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.region = ''eu'')');
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE f.x = 5 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk)');
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE f.fk IN (SELECT d.k FROM lion_nd d)');
-- a fact filter with no entry at all; a dimension filtered to nothing; one
-- that leaves only NULL keys, which join nothing
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE f.x = 77 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.region = ''eu'')');
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.region = ''mars'')');
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.k IS NULL)');
SELECT lion_nj_val('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.k IS NULL)');
-- heavy duplication, and every row the same key: the key's fact rows, once
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_ndh d WHERE d.k = f.fk)');
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE f.fk IN (SELECT d.k FROM lion_ndh d WHERE d.attr <> 1)');
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE f.t = ''t3'' AND EXISTS (SELECT 1 FROM lion_nda d WHERE d.k = f.fk)');
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE f.fk IN (SELECT d.k FROM lion_nda d WHERE d.attr = 1)');
SELECT lion_nj_val('SELECT count(*) FROM lion_nf f WHERE f.fk IN (SELECT d.k FROM lion_nda d)') AS node,
	   (SELECT count(*) FROM lion_nf WHERE fk = 7) AS key_7;
-- cross-type keys: an int4 fk against the int8 key, an int8 fk against int4
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE f.t = ''t7'' AND f.fk4 IN (SELECT d.k FROM lion_nd d WHERE d.grp = ''g0'')');
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE f.x = 2 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k4 = f.fk AND d.attr = 4)');
-- a text key
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE f.x = 6 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.tk = f.tk AND d.region = ''us'')');
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE f.tk IN (SELECT d.tk FROM lion_nd d WHERE d.attr = 0)');
-- count(1) and count of the fact's key are count(*): a matched row's key is
-- not NULL
SELECT lion_nj('SELECT count(1), count(f.fk), count(*) FROM lion_nf f WHERE f.x = 8 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.attr = 5)');
-- HAVING, ORDER BY and LIMIT above it
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk) HAVING count(*) > 100');
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk) HAVING count(*) > 100000');
SELECT lion_nj('SELECT count(*) AS c FROM lion_nf f WHERE f.x = 1 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.grp = ''g3'') ORDER BY c LIMIT 1');
-- count(DISTINCT) of the fact's key: the distinct keys with a match
SELECT lion_nj('SELECT count(DISTINCT f.fk) FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.region = ''eu'')');
SELECT lion_nj('SELECT count(DISTINCT f.fk) FROM lion_nf f WHERE f.fk IN (SELECT d.k FROM lion_ndh d)');
SELECT lion_nj('SELECT count(DISTINCT f.fk) FROM lion_nf f WHERE f.x = 77 AND f.fk IN (SELECT d.k FROM lion_nd d)');
SELECT lion_nj('SELECT count(DISTINCT f.tk) FROM lion_nf f WHERE f.y = 2 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.tk = f.tk AND d.attr = 1)');

-- ---- 3. what EXPLAIN ANALYZE counts ------------------------------------------------
-- the keys sorted (NULLs are not), the distinct ones looked up, and those
-- without an entry: lion_ndh's six keys, 153 among them
SELECT lion_nj_counter('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_ndh d WHERE d.k = f.fk)', 'Join Keys Sorted') AS sorted,
	   lion_nj_counter('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_ndh d WHERE d.k = f.fk)', 'Join Keys Looked Up') AS looked_up,
	   lion_nj_counter('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_ndh d WHERE d.k = f.fk)', 'Join Keys Without Entry') AS without_entry;
SELECT lion_nj_counter('SELECT count(*) FROM lion_nf f WHERE f.fk IN (SELECT d.k FROM lion_nda d)', 'Join Keys Sorted') AS sorted,
	   lion_nj_counter('SELECT count(*) FROM lion_nf f WHERE f.fk IN (SELECT d.k FROM lion_nda d)', 'Join Keys Looked Up') AS looked_up;
SELECT lion_nj_counter('SELECT count(*) FROM lion_nf f WHERE EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk)', 'Join Keys Looked Up')
	= (SELECT count(DISTINCT k) FROM lion_nd) AS one_lookup_a_key;

-- ---- 4. declined ---------------------------------------------------------------
-- a fact column in the output, as for every FK-side join
SELECT lion_nj('SELECT f.x, count(*) FROM lion_nf f WHERE EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.attr = 1) GROUP BY f.x');
-- another aggregate; a count of rows beside a count of distinct values
SELECT lion_nj('SELECT sum(f.x) FROM lion_nf f WHERE EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.attr = 1)');
SELECT lion_nj('SELECT count(*), count(DISTINCT f.fk) FROM lion_nf f WHERE EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.attr = 1)');
-- a second correlation
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.attr = f.x)');
-- the fact rows WITHOUT a qualifying dimension row: an anti join the other
-- way round, which the node does not count
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND NOT EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.attr = 1)');
-- a fact filter the posting sets cannot answer
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE f.id < 100 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk)');

-- ---- 5. generic plans, and rescans ----------------------------------------------
SELECT lion_nj_prep('SELECT count(*) FROM lion_nf f WHERE f.x = $1 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.region = $2)', '3, ''eu''');
SELECT lion_nj_prep('SELECT count(*) FROM lion_nf f WHERE f.x = $1 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.region = $2)', 'NULL, ''eu''');
SELECT lion_nj_prep('SELECT count(*) FROM lion_nf f WHERE f.x = $1 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.region = $2)', '3, NULL');
SELECT lion_nj_prep('SELECT count(*) FROM lion_nf f WHERE f.t IN ($1, $2) AND f.fk IN (SELECT d.k FROM lion_ndh d WHERE d.attr = $3)', '''t1'', ''t2'', 4');
-- a correlated subquery: the node, its child and its sort run again for every
-- outer row, with a new dimension filter, and then with a new fact filter
SELECT lion_nj('SELECT g, (SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.attr = g.g)) FROM generate_series(0, 6) g');
SELECT lion_nj('SELECT g, (SELECT count(*) FROM lion_nf f WHERE f.x = g.g AND f.fk IN (SELECT d.k FROM lion_ndh d)) FROM generate_series(0, 10) g');

-- ---- 6. the cost model's choice ------------------------------------------------------
-- a few dimension rows over the forty thousand fact rows: the node
SELECT lion_nj_pick('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.region = ''eu'' AND d.grp = ''g1'')');
-- a large dimension whose thirty thousand distinct keys share a few hundred
-- values with the fact: the ordinary plan, unless the dimension's own filter
-- leaves few of them (a key without an entry costs its descent and nothing
-- more, and core's estimate of the semi join says how few have one)
CREATE TABLE lion_ndb (k int8, attr int NOT NULL);
INSERT INTO lion_ndb SELECT i % 30000 + 1, i % 100 FROM generate_series(1, 60000) i;
CREATE INDEX lion_ndb_attr ON lion_ndb USING lion (attr);
VACUUM ANALYZE lion_ndb;
SELECT lion_nj_pick('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_ndb d WHERE d.k = f.fk)');
SELECT lion_nj_pick('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_ndb d WHERE d.k = f.fk AND d.attr = 5)');
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_ndb d WHERE d.k = f.fk)');
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_ndb d WHERE d.k = f.fk AND d.attr = 5)', false);

-- ---- 7. parallel: every participant sorts all the keys, and they divide the
-- distinct ones among themselves ---------------------------------------------------
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET parallel_leader_participation = off;
ALTER TABLE lion_nd SET (parallel_workers = 2);
ALTER TABLE lion_ndh SET (parallel_workers = 2);

/*
 * lion_nj_par() runs a query through the pushdown with every join method
 * disabled: as planned - in parallel, when the plan says so - then with
 * max_parallel_workers_per_gather at 0, and with the pushdown off and the
 * planner left alone, and compares both of the first two with the third.
 */
CREATE FUNCTION lion_nj_par(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	how text := 'not pushed down';
	nrows bigint;
	ndiff bigint;
	sdiff bigint;
	workers text := current_setting('max_parallel_workers_per_gather');
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
	EXECUTE format('CREATE TEMP TABLE lion_nj_par AS SELECT s::text AS r FROM (%s) s', q);

	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	EXECUTE format('CREATE TEMP TABLE lion_nj_ser AS SELECT s::text AS r FROM (%s) s', q);

	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_nestloop', 'on', true);
	EXECUTE format('CREATE TEMP TABLE lion_nj_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('max_parallel_workers_per_gather', workers, true);

	EXECUTE 'SELECT count(*) FROM lion_nj_par' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_nj_par EXCEPT ALL SELECT * FROM lion_nj_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_nj_off EXCEPT ALL SELECT * FROM lion_nj_par) b)'
		INTO ndiff;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_nj_ser EXCEPT ALL SELECT * FROM lion_nj_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_nj_off EXCEPT ALL SELECT * FROM lion_nj_ser) b)'
		INTO sdiff;
	EXECUTE 'DROP TABLE lion_nj_par, lion_nj_ser, lion_nj_off';

	IF ndiff <> 0 OR sdiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ in parallel, %s serially', ndiff, sdiff);
	END IF;
	RETURN format('%s, %s rows', how, nrows);
END $$;

-- the node is parallel-aware over the dimension's WHOLE plan, which every
-- participant runs
SELECT * FROM lion_nj_explain('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.region = ''eu'')');
SELECT * FROM lion_nj_explain('SELECT count(DISTINCT f.fk) FROM lion_nf f WHERE f.fk IN (SELECT d.k FROM lion_nd d WHERE d.attr < 4)');
SELECT lion_nj_par('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.region = ''eu'')');
SELECT lion_nj_par('SELECT count(*) FROM lion_nf f WHERE EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk)');
SELECT lion_nj_par('SELECT count(*) FROM lion_nf f WHERE f.t IN (''t1'', ''t2'') AND f.fk IN (SELECT d.k FROM lion_nd d WHERE d.attr <> 3)');
SELECT lion_nj_par('SELECT count(*) FROM lion_nf f WHERE f.doc @@ ''a1 & (b0 | b3)''::tsquery AND f.fk4 IN (SELECT d.k FROM lion_nd d)');
SELECT lion_nj_par('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_ndh d WHERE d.k = f.fk)');
SELECT lion_nj_par('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.region = ''mars'')');
SELECT lion_nj_par('SELECT count(DISTINCT f.fk) FROM lion_nf f WHERE f.fk IN (SELECT d.k FROM lion_nd d WHERE d.attr < 4)');
SELECT lion_nj_par('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk) HAVING count(*) > 100');
-- each distinct key is looked up once, by whichever participant claimed its
-- run; every participant sorted all of them
SELECT lion_nj_counter('SELECT count(*) FROM lion_nf f WHERE EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk)', 'Join Keys Looked Up')
	= (SELECT count(DISTINCT k) FROM lion_nd) AS one_lookup_a_key,
	   lion_nj_counter('SELECT count(*) FROM lion_nf f WHERE EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk)', 'Join Keys Sorted')
	= (SELECT count(k) FROM lion_nd) AS every_key_sorted;
-- with the leader taking part too
SET parallel_leader_participation = on;
SELECT lion_nj_par('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.region = ''eu'')');
SELECT lion_nj_counter('SELECT count(*) FROM lion_nf f WHERE EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk)', 'Join Keys Looked Up')
	= (SELECT count(DISTINCT k) FROM lion_nd) AS one_lookup_a_key;
SET parallel_leader_participation = off;
-- a generic plan's parameters, evaluated in every participant
SELECT lion_nj_prep('SELECT count(*) FROM lion_nf f WHERE f.x = $1 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.region = $2)', '3, ''eu''');
-- rescans: the Gather started again for every outer row, the runs of keys
-- claimed from the first one again each time, and every key looked up once a
-- run
SET enable_material = off;
SET enable_memoize = off;
SELECT * FROM lion_nj_explain('SELECT g, s.c FROM generate_series(1, 4) g LEFT JOIN (SELECT count(*) AS c FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk)) s ON s.c > g');
SELECT lion_nj_par('SELECT g, s.c FROM generate_series(1, 4) g LEFT JOIN (SELECT count(*) AS c FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk)) s ON s.c > g');
SELECT lion_nj_counter('SELECT g, s.c FROM generate_series(1, 4) g LEFT JOIN (SELECT count(*) AS c FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk)) s ON s.c > g', 'Join Keys Looked Up')
	= 4 * (SELECT count(DISTINCT k) FROM lion_nd) AS one_lookup_a_key_a_run,
	   lion_nj_counter('SELECT g, s.c FROM generate_series(1, 4) g LEFT JOIN (SELECT count(*) AS c FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk)) s ON s.c > g', 'Join Keys Sorted')
	= 4 * (SELECT count(k) FROM lion_nd) AS every_key_sorted_a_run;
RESET enable_material;
RESET enable_memoize;
-- a volatile dimension filter could keep a row in one participant and drop it
-- in another, so the node is not offered in parallel over it (an EXISTS with
-- one is not made a semi join at all; an IN is)
CREATE FUNCTION lion_nj_vol(int) RETURNS int LANGUAGE plpgsql VOLATILE PARALLEL SAFE
	AS 'BEGIN RETURN $1; END';
SELECT lion_nj_par('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND f.fk IN (SELECT d.k FROM lion_nd d WHERE d.attr = lion_nj_vol(2))');
-- none, and none on a dimension set to 0 (the joins off: serially the hash
-- join is the faster plan here, 1.2 ms of CPU against the node's 2.3)
SET max_parallel_workers_per_gather = 0;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
SELECT lion_nj_pick('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk)');
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_nestloop;
SET max_parallel_workers_per_gather = 2;
ALTER TABLE lion_nd SET (parallel_workers = 0);
SELECT lion_nj_par('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk)');
-- the serial node in a worker (debug_parallel_query), which counts every key
SET debug_parallel_query = on;
SELECT lion_nj_par('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk)');
RESET debug_parallel_query;
ALTER TABLE lion_nd RESET (parallel_workers);
ALTER TABLE lion_ndh RESET (parallel_workers);
RESET max_parallel_workers_per_gather;
SET max_parallel_workers_per_gather = 0;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET min_parallel_index_scan_size;
RESET parallel_leader_participation;

-- ---- 8. a dirty heap: deletes and updates on both sides, not yet vacuumed -----
DELETE FROM lion_nf WHERE fk = 7;
DELETE FROM lion_nf WHERE fk = 8 AND x < 5;
UPDATE lion_nf SET fk = 9 WHERE fk = 10 AND x = 1;
UPDATE lion_nf SET x = 3 WHERE id % 97 = 0;
UPDATE lion_nf SET fk = NULL WHERE fk = 11;
INSERT INTO lion_nf SELECT 40000 + i, 3, 3, 'k3', 3, 1, 't5', 'a1 b1' FROM generate_series(1, 5) i;
DELETE FROM lion_nd WHERE k IN (12, 13);
UPDATE lion_nd SET region = 'eu', k = 3 WHERE k IN (14, 15);
DELETE FROM lion_ndh WHERE k = 53;
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.region = ''eu'')');
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk)');
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND f.fk IN (SELECT d.k FROM lion_ndh d)');
SELECT lion_nj('SELECT count(DISTINCT f.fk) FROM lion_nf f WHERE f.x = 3 AND f.fk IN (SELECT d.k FROM lion_nd d)');
SELECT lion_nj_counter('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk)', 'Heap TIDs Rechecked') > 0 AS dirty_heap_rechecks;
VACUUM lion_nf;
VACUUM lion_nd;
VACUUM lion_ndh;
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_nd d WHERE d.k = f.fk AND d.region = ''eu'')');
SELECT lion_nj('SELECT count(*) FROM lion_nf f WHERE f.x = 3 AND f.fk IN (SELECT d.k FROM lion_ndh d)');

DROP TABLE lion_nf, lion_nd, lion_ndh, lion_nda, lion_ndb;
DROP FUNCTION lion_nj(text, boolean);
DROP FUNCTION lion_nj_val(text);
DROP FUNCTION lion_nj_pick(text);
DROP FUNCTION lion_nj_prep(text, text);
DROP FUNCTION lion_nj_counter(text, text);
DROP FUNCTION lion_nj_explain(text);
DROP FUNCTION lion_nj_par(text);
DROP FUNCTION lion_nj_vol(int);
