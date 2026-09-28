-- FK-side join pushdown, part two (DESIGN.md §27): semi and anti joins, the
-- ungrouped counts over them, and the fact filters collected once.
--
--   forward: SELECT count(*) FROM fact f WHERE <f filters>
--            AND EXISTS (SELECT 1 FROM dim d WHERE d.pk = f.fk AND <d filters>)
--   reverse: SELECT count(*) FROM dim d WHERE <d filters>
--            AND [NOT] EXISTS (SELECT 1 FROM fact f WHERE f.fk = d.pk AND <f filters>)
--
-- The forward form over a unique key is an inner join by the time the
-- planner is done with it; the reverse form is a semi (or anti) join whose
-- outer side is the dimension, and the node tests each dimension row's fk
-- set for a visible fact row.  Every answer is checked against the same
-- query with the pushdown off, as a multiset in both directions.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET default_statistics_target = 1000;

/*
 * lion_sj() runs a query through the pushdown - with every join method
 * disabled when force is set, so that a shape the cost model would not pick
 * is still EXERCISED - and again with the pushdown off and sequential scans
 * only, so that the reference reads no lion posting set, and compares the
 * two.  It reports whether the node was used.
 */
CREATE FUNCTION lion_sj(q text, force boolean DEFAULT true) RETURNS text
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
	EXECUTE format('CREATE TEMP TABLE lion_sj_on AS SELECT s::text AS r FROM (%s) s', q);

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
	EXECUTE format('CREATE TEMP TABLE lion_sj_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM lion_sj_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_sj_on EXCEPT ALL SELECT * FROM lion_sj_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_sj_off EXCEPT ALL SELECT * FROM lion_sj_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lion_sj_on, lion_sj_off';

	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN pushed THEN 'pushed down' ELSE 'not pushed down' END,
				  nrows);
END $$;

/* The answer itself, through the pushdown, joins disabled. */
CREATE FUNCTION lion_sj_val(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	RETURN QUERY EXECUTE format('SELECT s::text FROM (%s) s', q);
END $$;

/* Which plan the cost model picks, with nothing disabled. */
CREATE FUNCTION lion_sj_pick(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			RETURN 'pushed down';
		END IF;
	END LOOP;
	RETURN 'not pushed down';
END $$;

/* A generic prepared plan, which keeps $n a Param, against the same with the pushdown off. */
CREATE FUNCTION lion_sj_prep(q text, args text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	pushed boolean := false;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('plan_cache_mode', 'force_generic_plan', true);
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE 'PREPARE lion_sjp_on AS ' || q;
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) EXECUTE lion_sjp_on(' || args || ')' LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			pushed := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lion_sj_on AS EXECUTE lion_sjp_on(%s)', args);

	/*
	 * The reference: the pushdown off, and sequential scans only - no bitmap,
	 * index or index-only scan, which could read the very lion posting sets
	 * the node reads and agree with it about a wrong answer.
	 */
	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_nestloop', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	EXECUTE 'PREPARE lion_sjp_off AS ' || q;
	EXECUTE format('CREATE TEMP TABLE lion_sj_off AS EXECUTE lion_sjp_off(%s)', args);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE 'DEALLOCATE lion_sjp_on';
	EXECUTE 'DEALLOCATE lion_sjp_off';

	EXECUTE 'SELECT count(*) FROM lion_sj_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_sj_on EXCEPT ALL SELECT * FROM lion_sj_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_sj_off EXCEPT ALL SELECT * FROM lion_sj_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lion_sj_on, lion_sj_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN pushed THEN 'pushed down' ELSE 'not pushed down' END,
				  nrows);
END $$;

/* One counter of the node's EXPLAIN ANALYZE output, joins disabled. */
CREATE FUNCTION lion_sj_counter(q text, counter text) RETURNS bigint
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

-- EXPLAIN in a form every supported release prints the same way (the
-- definition test/sql/citext.sql uses).
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
 * The dimension: 300 rows, int8 primary key 1..300, attr 0..6 with NULL on
 * every 13th, a region and a group, and a lion index on (region, grp) for its
 * own filters.  The fact: 40000 rows in random key order; fk over 1..360, so
 * keys 301..360 join no dimension row, NULL on every 40th row, and no row at
 * all for the keys that are multiples of 17, so those dimension rows join no
 * fact row.  fk4 is fk as int4; x has 10 values, y 5 and NULLs, t 20, doc two
 * lexemes a row.  pad spreads the rows over enough heap pages for a copy of a
 * filter that selects most of them to outgrow work_mem's minimum.
 */
CREATE TABLE lion_sd (
	pk		int8	PRIMARY KEY,
	attr	int,
	region	text	NOT NULL,
	grp		text	NOT NULL
);
INSERT INTO lion_sd
SELECT i, CASE WHEN i % 13 = 0 THEN NULL ELSE i % 7 END,
	   CASE i % 3 WHEN 0 THEN 'eu' WHEN 1 THEN 'us' ELSE 'ap' END,
	   'g' || (i % 5)
FROM generate_series(1, 300) i;
CREATE INDEX lion_sd_rg ON lion_sd USING lion (region, grp);

CREATE TABLE lion_sf (
	id		int		NOT NULL,
	fk		int8,
	fk4		int4,
	x		int		NOT NULL,
	y		int,
	t		text	NOT NULL,
	doc		tsvector NOT NULL,
	pad		text
);
INSERT INTO lion_sf
SELECT i, k, k, h2 % 10, CASE WHEN h2 % 9 = 0 THEN NULL ELSE h2 / 10 % 5 END,
	   't' || (h3 % 20),
	   array_to_tsvector(ARRAY['a' || (h3 % 4), 'b' || (h3 / 4 % 6)]),
	   repeat('p', 150)
FROM (SELECT i,
			 CASE WHEN i % 40 = 0 THEN NULL
				  WHEN (abs(hashint4(i)) % 360 + 1) % 17 = 0 THEN NULL
				  ELSE abs(hashint4(i)) % 360 + 1 END AS k,
			 abs(hashint4(i + 1000000)) AS h2,
			 abs(hashint4(i + 2000000)) AS h3
	  FROM generate_series(1, 40000) i) s;
CREATE INDEX lion_sf_fk ON lion_sf USING lion (fk);
CREATE INDEX lion_sf_fk4 ON lion_sf USING lion (fk4);
CREATE INDEX lion_sf_x ON lion_sf USING lion (x);
CREATE INDEX lion_sf_yt ON lion_sf USING lion (y, t);
CREATE INDEX lion_sf_doc ON lion_sf USING lion (doc);

-- a dimension whose key is NOT unique: every key twice, and NULLs
CREATE TABLE lion_sdn (k int8, attr int NOT NULL);
INSERT INTO lion_sdn SELECT CASE WHEN i % 50 = 0 THEN NULL ELSE i % 150 + 1 END, i % 4
FROM generate_series(1, 300) i;
CREATE INDEX lion_sdn_k ON lion_sdn (k);
VACUUM (FREEZE, ANALYZE) lion_sf;
VACUUM (FREEZE, ANALYZE) lion_sd;
VACUUM (FREEZE, ANALYZE) lion_sdn;

-- ---- 1. forward: fact rows whose dimension row qualifies ---------------------
-- EXISTS and IN over a unique key are inner joins by now: the planner proved
-- the inner side unique (reduce_unique_semijoins())
SELECT lion_sj('SELECT count(*) FROM lion_sf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_sd d WHERE d.pk = f.fk AND d.region = ''eu'')');
SELECT lion_sj('SELECT count(*) FROM lion_sf f WHERE f.x = 3 AND f.fk IN (SELECT d.pk FROM lion_sd d WHERE d.region = ''eu'')');
SELECT lion_sj('SELECT count(*) FROM lion_sf f JOIN lion_sd d ON d.pk = f.fk WHERE f.x = 3 AND d.region = ''eu''');
SELECT lion_sj('SELECT count(*) FROM lion_sf f WHERE f.t IN (''t1'', ''t2'') AND EXISTS (SELECT 1 FROM lion_sd d WHERE d.pk = f.fk AND d.region = ''us'' AND d.grp IN (''g1'', ''g3''))');
SELECT lion_sj('SELECT count(*) FROM lion_sf f WHERE f.doc @@ ''(a0 | a1) & b2''::tsquery AND EXISTS (SELECT 1 FROM lion_sd d WHERE d.pk = f.fk AND d.region = ''ap'')');
SELECT lion_sj('SELECT count(*) FROM lion_sf f WHERE (f.x = 1 OR f.t = ''t4'') AND EXISTS (SELECT 1 FROM lion_sd d WHERE d.pk = f.fk AND d.grp = ''g2'')');
SELECT lion_sj('SELECT count(*) FROM lion_sf f WHERE f.x = 3 AND f.y IS NOT NULL AND EXISTS (SELECT 1 FROM lion_sd d WHERE d.pk = f.fk AND d.region = ''eu'')');
SELECT lion_sj('SELECT count(*) FROM lion_sf f WHERE f.y IS NULL AND EXISTS (SELECT 1 FROM lion_sd d WHERE d.pk = f.fk AND d.attr = 2)');
-- no fact filter, no dimension filter
SELECT lion_sj('SELECT count(*) FROM lion_sf f WHERE EXISTS (SELECT 1 FROM lion_sd d WHERE d.pk = f.fk AND d.region = ''eu'')');
SELECT lion_sj('SELECT count(*) FROM lion_sf f WHERE f.x = 5 AND EXISTS (SELECT 1 FROM lion_sd d WHERE d.pk = f.fk)');
-- a fact filter with no entry at all, a dimension filter nothing passes
SELECT lion_sj('SELECT count(*) FROM lion_sf f WHERE f.x = 77 AND EXISTS (SELECT 1 FROM lion_sd d WHERE d.pk = f.fk AND d.region = ''eu'')');
SELECT lion_sj('SELECT count(*) FROM lion_sf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_sd d WHERE d.pk = f.fk AND d.region = ''mars'')');
-- cross-type: an int4 fk against the int8 key
SELECT lion_sj('SELECT count(*) FROM lion_sf f WHERE f.t = ''t7'' AND EXISTS (SELECT 1 FROM lion_sd d WHERE d.pk = f.fk4 AND d.grp = ''g0'')');
-- grouped: only the join form can name a dimension column
SELECT lion_sj('SELECT d.region, count(*) FROM lion_sf f JOIN lion_sd d ON d.pk = f.fk WHERE f.x IN (1, 2) AND d.grp <> ''g4'' GROUP BY d.region');
-- the key is not unique: EXISTS stays a semi join whose inner side is the
-- dimension, which the node counts over the dimension's DISTINCT keys
-- (fkjoin_nonunique.sql) - the inner-join equivalence would count a fact row
-- once per duplicate
SELECT lion_sj('SELECT count(*) FROM lion_sf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_sdn d WHERE d.k = f.fk AND d.attr = 1)');
SELECT lion_sj('SELECT count(*) FROM lion_sf f WHERE f.x = 3 AND f.fk IN (SELECT d.k FROM lion_sdn d WHERE d.attr = 1)');

-- ---- 2. reverse: dimension rows with at least one matching fact row -----------
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = 3)');
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE d.pk IN (SELECT f.fk FROM lion_sf f WHERE f.x = 3)');
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE d.region = ''eu'' AND EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = 3 AND f.t = ''t5'')');
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE d.region = ''us'' AND d.grp = ''g1'' AND EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.doc @@ ''a2 & (b0 | b5)''::tsquery)');
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.t IN (''t1'', ''t2'', ''t3'') AND f.y = 4)');
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND (f.x = 9 OR f.y IS NULL) AND f.t = ''t0'')');
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = 3 AND f.y IS NOT NULL AND f.t = ''t9'')');
-- a fact filter that only subtracts: nothing to collect, nothing to intersect
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.y IS NOT NULL)');
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE NOT EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.y IS NOT NULL)');
-- rare enough that most dimension rows have none
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = 3 AND f.y = 2 AND f.t = ''t19'')');
-- no fact filter: the keys that have any fact row at all (not the multiples of 17)
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk)');
SELECT lion_sj_val('SELECT count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk)');
-- a fact filter with no entry: no dimension row qualifies
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = 77)');
-- cross-type, int8 key against the int4 fk
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE d.pk IN (SELECT f.fk4 FROM lion_sf f WHERE f.t = ''t3'' AND f.x = 4)');
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk4 = d.pk AND f.t = ''t3'')');
-- grouped, HAVING, ORDER BY and LIMIT above it
SELECT lion_sj('SELECT d.attr, count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = 3 AND f.t = ''t5'') GROUP BY d.attr');
SELECT lion_sj('SELECT d.region, d.grp, count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.y = 1) GROUP BY d.region, d.grp HAVING count(*) > 5');
SELECT lion_sj('SELECT d.grp, count(*) FROM lion_sd d WHERE d.pk IN (SELECT f.fk FROM lion_sf f WHERE f.t = ''t2'') GROUP BY d.grp ORDER BY count(*) DESC, d.grp LIMIT 2');
-- count(1) and count of the key are count(*) here: every row has a match
SELECT lion_sj('SELECT count(1), count(d.pk), count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = 8)');
-- a sum and a count of a nullable dimension column, over the join's rows as
-- they stand (fkjoin_mixed.sql)
SELECT lion_sj('SELECT sum(d.attr) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = 3)');
SELECT lion_sj('SELECT count(d.attr) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = 3)');
-- duplicated dimension keys are two rows, each tested on its own; a NULL key
-- has no match
CREATE INDEX lion_sdn_kl ON lion_sdn USING lion (k);
CREATE INDEX lion_sdn_al ON lion_sdn USING lion (attr);
ANALYZE lion_sdn;
SELECT lion_sj('SELECT count(*) FROM lion_sdn d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.k AND f.x = 2)');
SELECT lion_sj('SELECT d.attr, count(*) FROM lion_sdn d WHERE d.k IN (SELECT f.fk FROM lion_sf f WHERE f.t = ''t11'') GROUP BY d.attr');
-- ... and the forward semi join over the non-unique key, now that it has lion
-- indexes: counted over the dimension's distinct keys, or with the roles
-- exchanged, the dimension TESTED once per fact row - never the inner-join
-- count, which would count each duplicate.  The model takes the distinct keys.
SELECT lion_sj('SELECT count(*) FROM lion_sf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_sdn d WHERE d.k = f.fk AND d.attr = 1)');
SELECT lion_sj_pick('SELECT count(*) FROM lion_sf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_sdn d WHERE d.k = f.fk AND d.attr = 1)');
DROP INDEX lion_sdn_kl, lion_sdn_al;

-- ---- 3. anti: dimension rows with no matching fact row -----------------------
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE NOT EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = 3 AND f.t = ''t5'')');
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE d.region = ''eu'' AND NOT EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.doc @@ ''a1 & b1''::tsquery)');
-- no fact filter: exactly the keys without fact rows, the multiples of 17
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE NOT EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk)');
SELECT lion_sj_val('SELECT count(*) FROM lion_sd d WHERE NOT EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk)');
-- a fact filter with no entry: every dimension row
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE NOT EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = 77)');
SELECT lion_sj('SELECT d.attr, count(*) FROM lion_sd d WHERE NOT EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.y = 3 AND f.t = ''t6'') GROUP BY d.attr');
-- NULL keys join nothing, so they are rows of an anti join; duplicates count twice
CREATE INDEX lion_sdn_kl ON lion_sdn USING lion (k);
SELECT lion_sj('SELECT count(*) FROM lion_sdn d WHERE NOT EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.k AND f.x = 2 AND f.t = ''t1'')');
SELECT lion_sj('SELECT count(*) FROM lion_sdn d WHERE NOT EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.k AND f.x = 77)');
-- count of the key is not count(*) in an anti join: NULL keys are rows, so
-- there it is the count of a dimension column, over the rows as they stand
SELECT lion_sj('SELECT count(d.k), count(*) FROM lion_sdn d WHERE NOT EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.k AND f.x = 2)');
SELECT lion_sj('SELECT count(1) FROM lion_sdn d WHERE NOT EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.k AND f.x = 2)');
DROP INDEX lion_sdn_kl;
-- the anti join made from a LEFT JOIN is left alone
SELECT lion_sj('SELECT count(*) FROM lion_sd d LEFT JOIN lion_sf f ON f.fk = d.pk AND f.x = 3 WHERE f.fk IS NULL');
-- NOT IN is no anti join while the subquery's column may be NULL (one NULL
-- there makes every row's NOT IN unknown), on every major
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE d.pk NOT IN (SELECT f.fk FROM lion_sf f WHERE f.x = 3)');
-- With both sides provably not NULL, 19 plans NOT IN as the anti join it then
-- is, and the node counts it; earlier majors keep the hashed subplan.  Only
-- the answer is compared here: it is the same either way.
SELECT regexp_replace(lion_sj('SELECT count(*) FROM lion_sd d WHERE d.pk NOT IN (SELECT f.fk FROM lion_sf f WHERE f.x = 3 AND f.fk IS NOT NULL)'),
					  '^(not )?pushed down, ', '') AS not_in_not_null;

-- ---- 4. declined ---------------------------------------------------------------
-- a second correlation: two join clauses
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = d.attr)');
-- a qual on the outer side inside NOT EXISTS belongs to the join
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE NOT EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = 3 AND d.attr = 2)');
-- ... inside EXISTS it does not: it is the dimension's own filter
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = 3 AND d.attr = 2)');
-- a fact filter the posting sets cannot answer
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.pad LIKE ''q%'')');
-- another aggregate
SELECT lion_sj('SELECT avg(d.attr) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = 3)');
-- three relations
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = 3) AND EXISTS (SELECT 1 FROM lion_sdn n WHERE n.k = d.pk)');
-- a subquery the planner cannot pull up
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = 3 LIMIT 1 OFFSET 0)');

-- row-level security: a policy on the dimension is applied by the child plan
-- and changes the answer as it changes the ordinary plan's; one on the fact
-- declines, since its posting sets know nothing of the policy
CREATE ROLE lion_sj_reader;
GRANT SELECT ON lion_sf, lion_sd TO lion_sj_reader;
ALTER TABLE lion_sd ENABLE ROW LEVEL SECURITY;
CREATE POLICY lion_sd_eu ON lion_sd FOR SELECT TO lion_sj_reader USING (region = 'eu');
SET ROLE lion_sj_reader;
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = 3)');
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE NOT EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = 3)');
RESET ROLE;
ALTER TABLE lion_sd DISABLE ROW LEVEL SECURITY;
ALTER TABLE lion_sf ENABLE ROW LEVEL SECURITY;
CREATE POLICY lion_sf_some ON lion_sf FOR SELECT TO lion_sj_reader USING (x < 5);
SET ROLE lion_sj_reader;
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.t = ''t3'')');
RESET ROLE;
ALTER TABLE lion_sf DISABLE ROW LEVEL SECURITY;
DROP POLICY lion_sf_some ON lion_sf;
DROP POLICY lion_sd_eu ON lion_sd;
REVOKE ALL ON lion_sf, lion_sd FROM lion_sj_reader;
DROP ROLE lion_sj_reader;

-- ---- 5. parameters and rescans -------------------------------------------------
SELECT lion_sj_prep('SELECT count(*) FROM lion_sd d WHERE d.region = $2 AND EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = $1)', '3, ''eu''');
SELECT lion_sj_prep('SELECT count(*) FROM lion_sd d WHERE NOT EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.t = $1)', '''t4''');
SELECT lion_sj_prep('SELECT count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = $1)', 'NULL');
SELECT lion_sj_prep('SELECT count(*) FROM lion_sd d WHERE NOT EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = $1)', 'NULL');
SELECT lion_sj_prep('SELECT count(*) FROM lion_sf f WHERE f.t = $1 AND EXISTS (SELECT 1 FROM lion_sd d WHERE d.pk = f.fk AND d.grp = $2)', '''t8'', ''g3''');
-- a correlated subquery: the node and its child rescanned per outer row, with
-- a new dimension filter, and with a new FACT filter, whose collected copy
-- has to be made again
SELECT lion_sj('SELECT g, (SELECT count(*) FROM lion_sd d WHERE d.attr = g.g AND EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = 2)) FROM generate_series(0, 7) g');
SELECT lion_sj('SELECT g, (SELECT count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = g.g AND f.t = ''t1'')) FROM generate_series(0, 10) g');
SELECT lion_sj('SELECT g, (SELECT count(*) FROM lion_sf f WHERE f.x = g.g AND EXISTS (SELECT 1 FROM lion_sd d WHERE d.pk = f.fk AND d.region = ''eu'')) FROM generate_series(0, 10) g');

-- ---- 6. the fact filters, collected once or probed per count ------------------
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
-- three hundred dimension rows: one merge of the filters and a copy of what
-- survives, sought by every count
SELECT * FROM lion_explain_norm('SELECT count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = 3 AND f.t IN (''t1'', ''t2''))') AS p("QUERY PLAN");
SELECT * FROM lion_explain_norm('SELECT count(*) FROM lion_sd d WHERE NOT EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.doc @@ ''a1 & b1''::tsquery)') AS p("QUERY PLAN");
SELECT * FROM lion_explain_norm('SELECT count(*) FROM lion_sf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_sd d WHERE d.pk = f.fk AND d.region = ''eu'')') AS p("QUERY PLAN");
-- the dimension's own filters answered by its lion index, as a bitmap scan
-- (two equalities: every supported release puts both in the Index Cond)
SET enable_seqscan = off;
SELECT * FROM lion_explain_norm('SELECT count(*) FROM lion_sd d WHERE d.region = ''us'' AND d.grp = ''g1'' AND EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.t = ''t3'')') AS p("QUERY PLAN");
RESET enable_seqscan;
-- two dimension rows: two counts, each probing the filter itself.  The child
-- is a plain scan here, which every supported release chooses alike.
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
SET enable_bitmapscan = off;
SELECT * FROM lion_explain_norm('SELECT count(*) FROM lion_sd d WHERE d.pk IN (5, 6) AND EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = 3)') AS p("QUERY PLAN");
RESET enable_indexscan;
RESET enable_indexonlyscan;
RESET enable_bitmapscan;
-- no fact filter, nothing to collect
SELECT * FROM lion_explain_norm('SELECT d.grp, count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk) GROUP BY d.grp', 'COSTS OFF, VERBOSE') AS p("QUERY PLAN");
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_nestloop;
SELECT lion_sj_counter('SELECT count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = 3)', 'Fact Filter Rows Collected')
	= (SELECT count(*) FROM lion_sf WHERE x = 3) AS collected_the_filter;
-- the 300 keys are looked up; 17 of them have no entry
SELECT lion_sj_counter('SELECT count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = 3)', 'Join Keys Looked Up');
SELECT lion_sj_counter('SELECT count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = 3)', 'Join Keys Without Entry');
-- an existence test stops at the first container with a visible row, so the
-- semi join reads fewer containers than the inner join's counts of the same keys
SELECT lion_sj_counter('SELECT count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk)', 'Containers Visited')
	< lion_sj_counter('SELECT count(*) FROM lion_sd d JOIN lion_sf f ON f.fk = d.pk', 'Containers Visited') AS exists_stops_early;
-- a copy that outgrows a hash join's memory (work_mem times
-- hash_mem_multiplier): the plan was made to collect, and the run goes on in
-- a temporary file, as a hash join's batches would, and every count reads it
-- back - with the same answer.  (It used to give up, and every count read the
-- filters instead.)  Half of 250000 narrow rows is a bitset in each of 22
-- containers: some 90 kB.
CREATE TABLE lion_sfw (fk int8, x int NOT NULL);
INSERT INTO lion_sfw SELECT abs(hashint4(i)) % 360 + 1, abs(hashint4(i + 1000000)) % 10
FROM generate_series(1, 250000) i;
CREATE INDEX lion_sfw_fk ON lion_sfw USING lion (fk);
CREATE INDEX lion_sfw_x ON lion_sfw USING lion (x);
VACUUM (FREEZE, ANALYZE) lion_sfw;
SET plan_cache_mode = force_generic_plan;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
PREPARE lion_sj_big AS SELECT count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sfw f WHERE f.fk = d.pk AND f.x IN (0, 2, 4, 6, 8));
EXECUTE lion_sj_big;
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SELECT count(*) FILTER (WHERE p ~ 'Fact Filters: collected once') AS planned_to_collect,
	   count(*) FILTER (WHERE p ~ 'Fact Filter Rows Collected: -1') AS could_not,
	   count(*) FILTER (WHERE p ~ 'Fact Filter Copies Spilled: [1-9]') AS spilled
FROM (SELECT * FROM lion_explain_norm('EXECUTE lion_sj_big', 'ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF')) AS e(p);
EXECUTE lion_sj_big;
RESET work_mem;
RESET hash_mem_multiplier;
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_nestloop;
SET pg_lion.enable_count_pushdown = off;
SELECT count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sfw f WHERE f.fk = d.pk AND f.x IN (0, 2, 4, 6, 8));
RESET pg_lion.enable_count_pushdown;
DEALLOCATE lion_sj_big;
RESET plan_cache_mode;
DROP TABLE lion_sfw;

-- ---- 7. the cost model's choice, with nothing disabled ------------------------
SELECT lion_sj_pick('SELECT count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = 3)');
SELECT lion_sj_pick('SELECT count(*) FROM lion_sd d WHERE d.region = ''eu'' AND NOT EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.t = ''t5'')');
SELECT lion_sj_pick('SELECT count(*) FROM lion_sf f WHERE f.doc @@ ''(a0 | a1) & b2''::tsquery AND EXISTS (SELECT 1 FROM lion_sd d WHERE d.pk = f.fk AND d.region = ''ap'')');
-- a dimension far larger than the fact's key set: a lookup per dimension row
-- loses to the hash join, forward and reverse ...
CREATE TABLE lion_sdbig (k int8 PRIMARY KEY, attr int NOT NULL);
INSERT INTO lion_sdbig SELECT i, i % 5 FROM generate_series(1, 60000) i;
CREATE INDEX lion_sdbig_attr ON lion_sdbig USING lion (attr);
VACUUM (FREEZE, ANALYZE) lion_sdbig;
SELECT lion_sj_pick('SELECT count(*) FROM lion_sf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_sdbig d WHERE d.k = f.fk)');
SELECT lion_sj_pick('SELECT count(*) FROM lion_sdbig d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.k AND f.x = 3)');
-- ... unless the dimension's own filter leaves few rows
SELECT lion_sj_pick('SELECT count(*) FROM lion_sdbig d WHERE d.k < 200 AND EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.k AND f.x = 3)');
SELECT lion_sj('SELECT count(*) FROM lion_sdbig d WHERE d.k < 200 AND EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.k AND f.x = 3)', false);
DROP TABLE lion_sdbig;
-- the GUC
SET pg_lion.enable_count_pushdown = off;
SELECT lion_sj_pick('SELECT count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = 3)');
RESET pg_lion.enable_count_pushdown;

-- ---- 8. a dirty heap: deletes and updates on both sides, not yet vacuumed -----
DELETE FROM lion_sf WHERE fk = 7;
DELETE FROM lion_sf WHERE fk = 8 AND x < 5;
UPDATE lion_sf SET fk = 9, fk4 = 9 WHERE fk = 10 AND x = 1;
UPDATE lion_sf SET x = 3 WHERE id % 97 = 0;
UPDATE lion_sf SET fk = NULL, fk4 = NULL WHERE fk = 11 AND y = 2;
INSERT INTO lion_sf SELECT 40000 + i, 34, 34, 3, 1, 't5', 'a0 b0', 'q' FROM generate_series(1, 5) i;
DELETE FROM lion_sd WHERE pk = 12;
UPDATE lion_sd SET region = 'eu' WHERE pk IN (1, 2, 4);
UPDATE lion_sd SET attr = NULL WHERE pk = 5;
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = 3)');
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE d.region = ''eu'' AND EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = 3 AND f.t = ''t5'')');
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE NOT EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk)');
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE NOT EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = 3)');
SELECT lion_sj('SELECT d.attr, count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.y IN (1, 2)) GROUP BY d.attr');
SELECT lion_sj('SELECT count(*) FROM lion_sf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_sd d WHERE d.pk = f.fk AND d.region = ''eu'')');
SELECT lion_sj('SELECT count(*) FROM lion_sf f WHERE f.t = ''t5'' AND EXISTS (SELECT 1 FROM lion_sd d WHERE d.pk = f.fk4)');
SELECT lion_sj_counter('SELECT count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = 3)', 'Heap TIDs Rechecked') > 0 AS dirty_heap_rechecks;
-- the same after VACUUM, from the visibility map
VACUUM (FREEZE) lion_sf;
VACUUM (FREEZE) lion_sd;
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = 3)');
SELECT lion_sj('SELECT count(*) FROM lion_sd d WHERE NOT EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk)');
SELECT lion_sj('SELECT count(*) FROM lion_sf f WHERE f.x = 3 AND EXISTS (SELECT 1 FROM lion_sd d WHERE d.pk = f.fk AND d.region = ''eu'')');
SELECT lion_sj_counter('SELECT count(*) FROM lion_sd d WHERE EXISTS (SELECT 1 FROM lion_sf f WHERE f.fk = d.pk AND f.x = 3)', 'Heap TIDs Rechecked') AS vacuumed_heap_rechecks;

DROP TABLE lion_sf, lion_sd, lion_sdn;
DROP FUNCTION lion_sj(text, boolean);
DROP FUNCTION lion_sj_val(text);
DROP FUNCTION lion_sj_pick(text);
DROP FUNCTION lion_sj_prep(text, text);
DROP FUNCTION lion_sj_counter(text, text);
