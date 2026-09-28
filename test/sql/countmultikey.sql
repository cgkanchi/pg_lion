-- Multi-key clauses whose query the count pushdown only has at run time
-- (DESIGN.md §17, "A query known only at run time"): `tags @> $1` in a
-- generic plan, `tsv @@ to_tsquery(current_setting(...))`, an exec Param
-- under a LATERAL nested loop.
--
-- The query's SHAPE decides whether the posting sets answer it exactly, and
-- the shape is only known once the value is: `$1 = '{}'` is every row, a
-- phrase is its lexemes' rows and more, a NULL element is under no key.  The
-- node therefore answers every value - exactly when the keys do, and from a
-- superset rechecked in the heap when they do not, or from the other clauses'
-- rows, or from a sequential scan when nothing narrows it at all - and each
-- answer here is compared with the same query's under
-- pg_lion.enable_count_pushdown = off, as a multiset both ways round, with
-- the node's recheck counters beside it.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

/*
 * lion_mk() runs a query with every other scan and join method disabled, so
 * that a LionCount path that is built at all is the plan, and compares its
 * rows with the reference plan's: the pushdown off and sequential scans
 * only, so that it reads no lion posting set.
 * It says whether the node ran, how many rows came out, and the node's two
 * recheck counters from EXPLAIN ANALYZE: the candidate TIDs it resolved in the
 * heap and the rows the recheck turned away.
 *
 * With args, q is PREPAREd and EXECUTEd with them under plan_cache_mode
 * `how` - force_generic_plan keeps `$1` a Param, force_custom_plan folds it
 * into a literal - and the reference is a statement prepared afresh with the
 * pushdown off, since a cached plan does not notice the setting change.
 */
CREATE FUNCTION lion_mk(q text, args text DEFAULT NULL,
						how text DEFAULT 'force_generic_plan') RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	ex text;
	pushed boolean := false;
	rechecked bigint := 0;
	removed bigint := 0;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('plan_cache_mode', how, true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	ex := q;
	IF args IS NOT NULL THEN
		EXECUTE 'PREPARE lion_mk_on AS ' || q;
		ex := 'EXECUTE lion_mk_on(' || args || ')';
	END IF;
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || ex LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			pushed := true;
		ELSIF btrim(split_part(ln, ':', 1)) = 'Heap TIDs Rechecked' THEN
			rechecked := rechecked + btrim(split_part(ln, ':', 2))::bigint;
		ELSIF btrim(split_part(ln, ':', 1)) = 'Rows Removed by Recheck' THEN
			removed := removed + btrim(split_part(ln, ':', 2))::bigint;
		END IF;
	END LOOP;
	EXECUTE 'CREATE TEMP TABLE lion_mk_r_on AS ' || ex;

	/*
	 * The reference: the pushdown off, and sequential scans only - no bitmap,
	 * index or index-only scan, which could read the very lion posting sets
	 * the node reads and agree with it about a wrong answer.
	 */
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	IF args IS NOT NULL THEN
		EXECUTE 'DEALLOCATE lion_mk_on';
		EXECUTE 'PREPARE lion_mk_off AS ' || q;
		ex := 'EXECUTE lion_mk_off(' || args || ')';
	END IF;
	EXECUTE 'CREATE TEMP TABLE lion_mk_r_off AS ' || ex;
	IF args IS NOT NULL THEN
		EXECUTE 'DEALLOCATE lion_mk_off';
	END IF;
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM lion_mk_r_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_mk_r_on EXCEPT ALL SELECT * FROM lion_mk_r_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_mk_r_off EXCEPT ALL SELECT * FROM lion_mk_r_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lion_mk_r_on, lion_mk_r_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	IF NOT pushed THEN
		RETURN format('not pushed, %s rows', nrows);
	END IF;
	RETURN format('LionCount, %s rows, %s rechecked, %s removed',
				  nrows, rechecked, removed);
END $$;

/* Which plan the cost model picks, with nothing disabled. */
CREATE FUNCTION lion_mk_pick(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			RETURN 'LionCount';
		END IF;
	END LOOP;
	RETURN 'not pushed';
END $$;

/*
 * The table.  k has 10 values and g 4, and n is g but NULL on every 7th row;
 * tags is NULL on every 97th row, empty on every 89th and holds a NULL
 * element on every 61st; nums is an int[] of two elements; tsv puts
 * `w<id % 13>` right before `x<id % 5>`, weighted A on every 11th row and D
 * otherwise, and adds `w<id % 3> end`, and is NULL on every 83rd row.
 */
CREATE TABLE lion_mk_t (
	id		int NOT NULL,
	k		int NOT NULL,
	g		int NOT NULL,
	n		int,
	tags	text[],
	nums	int[],
	tsv		tsvector
);
INSERT INTO lion_mk_t
SELECT i, i % 10, i % 4, CASE WHEN i % 7 = 0 THEN NULL ELSE i % 4 END,
	   CASE WHEN i % 97 = 0 THEN NULL
			WHEN i % 89 = 0 THEN '{}'::text[]
			WHEN i % 61 = 0 THEN ARRAY['t' || (i % 20), NULL]
			ELSE ARRAY['t' || (i % 20), 't' || (i % 7), 'u' || (i % 500)] END,
	   ARRAY[i % 30, i % 11],
	   CASE WHEN i % 83 = 0 THEN NULL
			ELSE setweight(to_tsvector('simple', 'w' || (i % 13) || ' x' || (i % 5)),
						   CASE WHEN i % 11 = 0 THEN 'A' ELSE 'D' END::"char") ||
				 to_tsvector('simple', 'w' || (i % 3) || ' end') END
  FROM generate_series(1, 12000) i;
CREATE INDEX lion_mk_k ON lion_mk_t USING lion (k);
CREATE INDEX lion_mk_g ON lion_mk_t USING lion (g);
CREATE INDEX lion_mk_n ON lion_mk_t USING lion (n);
CREATE INDEX lion_mk_tags ON lion_mk_t USING lion (tags);
CREATE INDEX lion_mk_nums ON lion_mk_t USING lion (nums);
CREATE INDEX lion_mk_tsv ON lion_mk_t USING lion (tsv);
VACUUM ANALYZE lion_mk_t;

-- ---- 1. a generic plan's parameter, of every shape ------------------------
-- @>: exact; exact over two keys; no row, NULL; the empty array (every row
-- but the NULL ones, from a sequential scan); a NULL element (a superset of
-- the other elements' rows, none of which qualifies); nothing but NULL
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tags @> $1', '''{t1}''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tags @> $1', '''{t1,t8}''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tags @> $1', '''{nosuch}''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tags @> $1', 'NULL');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tags @> $1', '''{}''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tags @> $1', '''{t1,NULL}''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tags @> $1', '''{NULL}''');
-- &&: exact; the empty array (nothing); a NULL element (every row, rechecked)
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tags && $1', '''{t1,u5}''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tags && $1', '''{}''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tags && $1', '''{t1,NULL}''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE nums @> $1', '''{3,3}''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE nums && $1', '''{7,29}''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE nums @> $1', '''{}''');
-- @@: AND and OR of lexemes are exact; a phrase is the AND of its lexemes and
-- a weight the lexeme at any weight, both rechecked; `a & !b` is `a`,
-- rechecked; a prefix and `!a` are every row; an empty query nothing
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tsv @@ $1', '''w1''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tsv @@ $1', '''w1 & x2''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tsv @@ $1', '''w1 | x2''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tsv @@ $1', '''w1 <-> x1''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tsv @@ $1', '''(w1 | w2) <-> x2''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tsv @@ $1', '''w1:A''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tsv @@ $1', '''w1:A & x1''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tsv @@ $1', '''w1 & !x1''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tsv @@ $1', '''w1 | !x1''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tsv @@ $1', '''!w1''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tsv @@ $1', '''w1:*''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tsv @@ $1', '''w1:* & x3''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tsv @@ $1', '''''');
-- the query built from a parameter, as an application builds it
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tsv @@ to_tsquery(''simple'', $1)', '''w2 & x2''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tsv @@ websearch_to_tsquery(''simple'', $1)', '''"w3 x3"''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tsv @@ websearch_to_tsquery(''simple'', $1)', '''w3 -x3''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tags @> ARRAY[$1, $2]', '''t1'', ''t8''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tags @> ARRAY[$1, $2]', '''t1'', NULL');

-- ---- 2. beside other clauses, grouped, distinct, counted ------------------
-- the other clause's rows are the candidates when no key narrows the query
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tags @> $1 AND k = $2', '''{t1}'', 1');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tags @> $1 AND k = $2', '''{}'', 3');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tsv @@ $1 AND k IN (1, 2)', '''w1 <-> x1''');
-- two such clauses, both every row, beside a null test: a sequential scan
-- that tests all three
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tags @> $1 AND tsv @@ $2 AND n IS NOT NULL', '''{}'', ''!w5''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tags @> $1 AND n IS NOT NULL', '''{}''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tags @> $1 AND n IS NOT NULL', '''{t2}''');
SELECT lion_mk('SELECT n, count(*) FROM lion_mk_t WHERE tags @> $1 GROUP BY n', '''{}''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tags && $1 AND tsv @@ $2', '''{t3,NULL}'', ''w3 <-> x3''');
-- the column itself counted: a strict operator leaves no NULL of it
SELECT lion_mk('SELECT count(tags) FROM lion_mk_t WHERE tags @> $1', '''{}''');
-- GROUP BY, HAVING, a GROUP BY the planner folds, count(DISTINCT)
SELECT lion_mk('SELECT g, count(*) FROM lion_mk_t WHERE tsv @@ $1 GROUP BY g', '''w1 <-> x1''');
SELECT lion_mk('SELECT g, count(*) FROM lion_mk_t WHERE tags @> $1 GROUP BY g', '''{}''');
SELECT lion_mk('SELECT g, count(*) FROM lion_mk_t WHERE tags @> $1 GROUP BY g HAVING count(*) > 2700', '''{}''');
SELECT lion_mk('SELECT g, count(*) FROM lion_mk_t WHERE tags && $1 AND g = 2 GROUP BY g', '''{t2,NULL}''');
SELECT lion_mk('SELECT k, g, count(*) FROM lion_mk_t WHERE tsv @@ $1 GROUP BY k, g', '''w2:A''');
SELECT lion_mk('SELECT count(DISTINCT k) FROM lion_mk_t WHERE tags && $1', '''{t1,NULL}''');
SELECT lion_mk('SELECT g, count(DISTINCT k) FROM lion_mk_t WHERE tsv @@ $1 GROUP BY g', '''w4 & !x4''');
-- under an OR the query has to be a literal: not pushed
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tags @> $1 OR k = 3', '''{t1}''');

-- ---- 3. custom plans: a literal, exact or declined, as always -------------
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tags @> $1', '''{t1}''', 'force_custom_plan');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tags @> $1', '''{}''', 'force_custom_plan');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tsv @@ $1', '''w1 & x2''', 'force_custom_plan');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tsv @@ $1', '''w1 <-> x1''', 'force_custom_plan');

-- What EXPLAIN prints: the parameter as core prints it, in both plans.
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
PREPARE lion_mk_e(text[], tsquery) AS
	SELECT g, count(*) FROM lion_mk_t WHERE tags @> $1 AND tsv @@ $2 GROUP BY g;
SET plan_cache_mode = force_generic_plan;
EXPLAIN (COSTS OFF) EXECUTE lion_mk_e('{t1}', 'w1');
SET plan_cache_mode = force_custom_plan;
EXPLAIN (COSTS OFF) EXECUTE lion_mk_e('{t1}', 'w1');
RESET plan_cache_mode;
DEALLOCATE lion_mk_e;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
RESET enable_indexonlyscan;

-- ---- 4. stable expressions ------------------------------------------------
-- evaluated once per scan, and anew by every execution of a cached plan
SET lion_mk.tags = 't1,t8';
SET lion_mk.q = 'w1 <-> x1';
SELECT lion_mk($$SELECT count(*) FROM lion_mk_t WHERE tags @> string_to_array(current_setting('lion_mk.tags'), ',')::text[]$$);
SELECT lion_mk($$SELECT count(*) FROM lion_mk_t WHERE tsv @@ to_tsquery('simple', current_setting('lion_mk.q'))$$);
SELECT lion_mk($$SELECT g, count(*) FROM lion_mk_t WHERE tsv @@ to_tsquery('simple', current_setting('lion_mk.q')) AND k = 1 GROUP BY g$$);
SET plan_cache_mode = force_generic_plan;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
PREPARE lion_mk_s1 AS SELECT count(*) FROM lion_mk_t
	WHERE tags @> string_to_array(current_setting('lion_mk.tags'), ',')::text[];
PREPARE lion_mk_s2 AS SELECT count(*) FROM lion_mk_t
	WHERE tsv @@ to_tsquery('simple', current_setting('lion_mk.q'));
EXPLAIN (COSTS OFF) EXECUTE lion_mk_s1;
EXPLAIN (COSTS OFF) EXECUTE lion_mk_s2;
EXECUTE lion_mk_s1;
EXECUTE lion_mk_s2;
SET lion_mk.tags = '';
SET lion_mk.q = 'w1 & x1';
EXECUTE lion_mk_s1;
EXECUTE lion_mk_s2;
SET lion_mk.tags = 'u7';
SET lion_mk.q = 'w1:*';
EXECUTE lion_mk_s1;
EXECUTE lion_mk_s2;
RESET plan_cache_mode;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
RESET enable_indexonlyscan;
SET pg_lion.enable_count_pushdown = off;
SELECT count(*) FROM lion_mk_t WHERE tags @> '{t1,t8}';
SELECT count(*) FROM lion_mk_t WHERE tsv @@ 'w1 <-> x1';
SELECT count(*) FROM lion_mk_t WHERE tags @> '{}';
SELECT count(*) FROM lion_mk_t WHERE tsv @@ 'w1 & x1';
SELECT count(*) FROM lion_mk_t WHERE tags @> '{u7}';
SELECT count(*) FROM lion_mk_t WHERE tsv @@ 'w1:*';
RESET pg_lion.enable_count_pushdown;
DEALLOCATE lion_mk_s1;
DEALLOCATE lion_mk_s2;

/*
 * An exec Param, which a nested loop changes between rescans: the node
 * evaluates and extracts the query again for every outer row, exact or not.
 */
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
EXPLAIN (COSTS OFF)
SELECT v.n, s.c FROM (VALUES (1, '{t1}'::text[]), (2, '{}'), (3, '{t1,NULL}'), (4, NULL)) v(n, a),
	 LATERAL (SELECT count(*) AS c FROM lion_mk_t WHERE tags @> v.a) s;
SELECT v.n, s.c FROM (VALUES (1, '{t1}'::text[]), (2, '{}'), (3, '{t1,NULL}'), (4, NULL)) v(n, a),
	 LATERAL (SELECT count(*) AS c FROM lion_mk_t WHERE tags @> v.a) s ORDER BY v.n;
SET pg_lion.enable_count_pushdown = off;
SELECT v.n, s.c FROM (VALUES (1, '{t1}'::text[]), (2, '{}'), (3, '{t1,NULL}'), (4, NULL)) v(n, a),
	 LATERAL (SELECT count(*) AS c FROM lion_mk_t WHERE tags @> v.a) s ORDER BY v.n;
RESET pg_lion.enable_count_pushdown;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
RESET enable_indexonlyscan;

-- ---- 5. what the cost model picks, with nothing disabled -------------------
-- A value it cannot estimate is priced as the shape that needs the most
-- rechecking, which alone is a sequential scan of its own: the ordinary
-- plan's wins.  A stable value is priced as its estimate extracts, here
-- exactly.
SET plan_cache_mode = force_generic_plan;
PREPARE lion_mk_c1(text[]) AS SELECT count(*) FROM lion_mk_t WHERE tags @> $1;
SELECT lion_mk_pick('EXECUTE lion_mk_c1(''{t1}'')');
DEALLOCATE lion_mk_c1;
RESET plan_cache_mode;
SET lion_mk.tags = 't1,t8';
SELECT lion_mk_pick($$SELECT count(*) FROM lion_mk_t WHERE tags @> string_to_array(current_setting('lion_mk.tags'), ',')::text[]$$);

-- ---- 6. a partitioned table, whose partitions number the column apart -----
CREATE TABLE lion_mk_p (id int NOT NULL, g int NOT NULL, tags text[]) PARTITION BY LIST (g);
CREATE TABLE lion_mk_p0 PARTITION OF lion_mk_p FOR VALUES IN (0, 1);
CREATE TABLE lion_mk_p1 (tags text[], junk int, g int NOT NULL, id int NOT NULL);
ALTER TABLE lion_mk_p1 DROP COLUMN junk;
ALTER TABLE lion_mk_p ATTACH PARTITION lion_mk_p1 FOR VALUES IN (2, 3);
INSERT INTO lion_mk_p (id, g, tags) SELECT id, g, tags FROM lion_mk_t;
CREATE INDEX lion_mk_p_tags ON lion_mk_p USING lion (tags);
CREATE INDEX lion_mk_p_g ON lion_mk_p USING lion (g);
VACUUM ANALYZE lion_mk_p;
SELECT lion_mk('SELECT count(*) FROM lion_mk_p WHERE tags @> $1', '''{t2}''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_p WHERE tags @> $1', '''{}''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_p WHERE tags @> $1', '''{t2,NULL}''');
SELECT lion_mk('SELECT g, count(*) FROM lion_mk_p WHERE tags && $1 GROUP BY g', '''{t3,NULL}''');
DROP TABLE lion_mk_p;

-- ---- 7. a fact filter of the FK-side join (DESIGN.md §27) -----------------
CREATE TABLE lion_mk_d (pk int PRIMARY KEY, attr int NOT NULL);
INSERT INTO lion_mk_d SELECT i, i % 3 FROM generate_series(0, 8) i;
ANALYZE lion_mk_d;
SELECT lion_mk('SELECT d.attr, count(*) FROM lion_mk_t f JOIN lion_mk_d d ON f.k = d.pk WHERE f.tags @> $1 GROUP BY d.attr', '''{t1}''');
SELECT lion_mk('SELECT d.attr, count(*) FROM lion_mk_t f JOIN lion_mk_d d ON f.k = d.pk WHERE f.tags @> $1 GROUP BY d.attr', '''{}''');
SELECT lion_mk('SELECT d.attr, count(*) FROM lion_mk_t f JOIN lion_mk_d d ON f.k = d.pk WHERE f.tsv @@ $1 AND f.g = 1 GROUP BY d.attr', '''w1 <-> x1''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_d d WHERE EXISTS (SELECT 1 FROM lion_mk_t f WHERE f.k = d.pk AND f.tags @> $1)', '''{t2,NULL}''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_d d WHERE NOT EXISTS (SELECT 1 FROM lion_mk_t f WHERE f.k = d.pk AND f.tsv @@ $1)', '''w1 <-> x3''');
SELECT lion_mk('SELECT count(DISTINCT f.k) FROM lion_mk_t f JOIN lion_mk_d d ON f.k = d.pk WHERE f.tags && $1', '''{t4,NULL}''');

-- ---- 8. a dirty heap, whose candidates are rechecked either way -----------
UPDATE lion_mk_t SET tags = ARRAY['t1', 'moved'] WHERE id % 17 = 0;
DELETE FROM lion_mk_t WHERE id % 19 = 0;
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tags @> $1', '''{t1}''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tags @> $1', '''{t1,NULL}''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tags @> $1', '''{}''');
SELECT lion_mk('SELECT g, count(*) FROM lion_mk_t WHERE tags && $1 GROUP BY g', '''{moved,NULL}''');
SELECT lion_mk('SELECT d.attr, count(*) FROM lion_mk_t f JOIN lion_mk_d d ON f.k = d.pk WHERE f.tags @> $1 GROUP BY d.attr', '''{t1,NULL}''');
VACUUM lion_mk_t;
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tags @> $1', '''{t1,NULL}''');
SELECT lion_mk('SELECT count(*) FROM lion_mk_t WHERE tags @> $1', '''{}''');

-- ---- 9. in parallel, every participant extracts and rechecks its own ------
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET parallel_leader_participation = off;
CREATE TABLE lion_mk_pd (pk int PRIMARY KEY, attr int NOT NULL) WITH (parallel_workers = 2);
INSERT INTO lion_mk_pd SELECT i, i % 5 FROM generate_series(1, 3000) i;
CREATE TABLE lion_mk_pf (fk int, x int NOT NULL, tags text[], tsv tsvector);
INSERT INTO lion_mk_pf
SELECT abs(hashint4(i)) % 3000 + 1, i % 10,
	   CASE WHEN i % 89 = 0 THEN '{}'::text[] ELSE ARRAY['t' || (i % 20), 'u' || (i % 7)] END,
	   to_tsvector('simple', 'w' || (i % 13) || ' x' || (i % 5))
  FROM generate_series(1, 30000) i;
CREATE INDEX lion_mk_pf_fk ON lion_mk_pf USING lion (fk);
CREATE INDEX lion_mk_pf_x ON lion_mk_pf USING lion (x);
CREATE INDEX lion_mk_pf_tags ON lion_mk_pf USING lion (tags);
CREATE INDEX lion_mk_pf_tsv ON lion_mk_pf USING lion (tsv);
VACUUM ANALYZE lion_mk_pd, lion_mk_pf;

/*
 * lion_mk_par() runs a prepared statement's generic plan through the
 * pushdown with the join methods disabled - in parallel, when the plan says
 * so - then with max_parallel_workers_per_gather at 0, and then with the
 * pushdown off and sequential scans only, and compares both of the first two
 * with the third.
 */
CREATE FUNCTION lion_mk_par(q text, args text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	how text := 'not pushed';
	nrows bigint;
	ndiff bigint;
	sdiff bigint;
	ex text := 'EXECUTE lion_mk_ps' ||
		CASE WHEN args = '' THEN '' ELSE '(' || args || ')' END;
BEGIN
	PERFORM set_config('plan_cache_mode', 'force_generic_plan', true);
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	EXECUTE 'PREPARE lion_mk_ps AS ' || q;
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || ex LOOP
		IF ln LIKE '%Parallel Custom Scan (LionCount)%' THEN
			how := 'parallel';
		ELSIF ln LIKE '%Custom Scan (LionCount)%' AND how <> 'parallel' THEN
			how := 'serial';
		END IF;
	END LOOP;
	EXECUTE 'CREATE TEMP TABLE lion_mk_par AS ' || ex;
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	EXECUTE 'CREATE TEMP TABLE lion_mk_ser AS ' || ex;
	EXECUTE 'DEALLOCATE lion_mk_ps';
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
	EXECUTE 'PREPARE lion_mk_ps AS ' || q;
	EXECUTE 'CREATE TEMP TABLE lion_mk_off AS ' || ex;
	EXECUTE 'DEALLOCATE lion_mk_ps';
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('max_parallel_workers_per_gather', '2', true);

	EXECUTE 'SELECT count(*) FROM lion_mk_par' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_mk_par EXCEPT ALL SELECT * FROM lion_mk_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_mk_off EXCEPT ALL SELECT * FROM lion_mk_par) b)'
		INTO ndiff;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_mk_ser EXCEPT ALL SELECT * FROM lion_mk_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_mk_off EXCEPT ALL SELECT * FROM lion_mk_ser) b)'
		INTO sdiff;
	EXECUTE 'DROP TABLE lion_mk_par, lion_mk_ser, lion_mk_off';
	IF ndiff <> 0 OR sdiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ in parallel, %s serially', ndiff, sdiff);
	END IF;
	RETURN format('%s, %s rows', how, nrows);
END $$;

SELECT lion_mk_par('SELECT d.attr, count(*) FROM lion_mk_pd d WHERE EXISTS (SELECT 1 FROM lion_mk_pf f WHERE f.fk = d.pk AND f.tags @> $1) GROUP BY d.attr', '''{t3}''');
SELECT lion_mk_par('SELECT d.attr, count(*) FROM lion_mk_pd d WHERE EXISTS (SELECT 1 FROM lion_mk_pf f WHERE f.fk = d.pk AND f.tags @> $1) GROUP BY d.attr', '''{t3,NULL}''');
SELECT lion_mk_par('SELECT d.attr, count(*) FROM lion_mk_pd d WHERE EXISTS (SELECT 1 FROM lion_mk_pf f WHERE f.fk = d.pk AND f.tags @> $1 AND f.x = 2) GROUP BY d.attr', '''{}''');
SELECT lion_mk_par('SELECT count(*) FROM lion_mk_pd d WHERE NOT EXISTS (SELECT 1 FROM lion_mk_pf f WHERE f.fk = d.pk AND f.tsv @@ $1)', '''w1 <-> x1''');
SELECT lion_mk_par('SELECT d.attr, count(*) FROM lion_mk_pf f JOIN lion_mk_pd d ON f.fk = d.pk WHERE f.tsv @@ to_tsquery(''simple'', $1) GROUP BY d.attr', '''w2 & !x2''');
SELECT lion_mk_par('SELECT count(DISTINCT d.attr) FROM lion_mk_pd d WHERE EXISTS (SELECT 1 FROM lion_mk_pf f WHERE f.fk = d.pk AND f.tags && $1)', '''{t5,NULL}''');
-- an IN list whose length the plan does not know, evaluated by every
-- participant (DESIGN.md §27): a parameter, one longer than a batch of the
-- list pin budget, and the ARRAY[] a generic plan keeps for IN ($1, $2)
SELECT lion_mk_par('SELECT d.attr, count(*) FROM lion_mk_pd d WHERE EXISTS (SELECT 1 FROM lion_mk_pf f WHERE f.fk = d.pk AND f.x = ANY ($1)) GROUP BY d.attr', '''{1,3,5}''');
SELECT lion_mk_par('SELECT d.attr, count(*) FROM lion_mk_pd d WHERE EXISTS (SELECT 1 FROM lion_mk_pf f WHERE f.fk = d.pk AND f.x = ANY ($1)) GROUP BY d.attr', 'NULL');
SELECT lion_mk_par('SELECT d.attr, count(*) FROM lion_mk_pd d WHERE EXISTS (SELECT 1 FROM lion_mk_pf f WHERE f.fk = d.pk AND f.fk = ANY ($1)) GROUP BY d.attr', (SELECT quote_literal(array_agg(i)::text) FROM generate_series(1, 3000, 2) i));
SELECT lion_mk_par('SELECT count(*) FROM lion_mk_pd d WHERE NOT EXISTS (SELECT 1 FROM lion_mk_pf f WHERE f.fk = d.pk AND f.x IN ($1, $2))', '4, 7');
SET lion_mk.xs = '2,4,6';
SELECT lion_mk_par($$SELECT d.attr, count(*) FROM lion_mk_pd d WHERE EXISTS (SELECT 1 FROM lion_mk_pf f WHERE f.fk = d.pk AND f.x = ANY (string_to_array(current_setting('lion_mk.xs'), ',')::int[])) GROUP BY d.attr$$, '');
RESET lion_mk.xs;

/*
 * The 1500 values of the long list have an entry each, and at a work_mem of
 * 64 kB their cursors do not fit the open budget together: a count takes
 * such a list in batches, and the copy of the fact filters reads it as a
 * windowed union instead - once, before the first dimension row - where it
 * used to give up on the copy and read the list again at every count
 * (DESIGN.md §27).
 */
CREATE FUNCTION lion_mk_analyze(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		RETURN NEXT ln;
	END LOOP;
END $$;
SET max_parallel_workers_per_gather = 0;
SET plan_cache_mode = force_generic_plan;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
SET work_mem = '64kB';
PREPARE lion_mk_long(int[]) AS SELECT d.attr, count(*) FROM lion_mk_pd d
	WHERE EXISTS (SELECT 1 FROM lion_mk_pf f WHERE f.fk = d.pk AND f.fk = ANY ($1))
	GROUP BY d.attr;
SELECT count(*) FILTER (WHERE p ~ 'Fact Filters: collected once') AS planned_to_collect,
	   count(*) FILTER (WHERE p ~ 'Fact Filter Rows Collected: [0-9]') AS collected
FROM lion_mk_analyze(format('EXECUTE lion_mk_long(%L)',
							(SELECT array_agg(i)::text
							   FROM generate_series(1, 3000, 2) i))) AS e(p);
SELECT lion_mk_par('SELECT d.attr, count(*) FROM lion_mk_pd d WHERE EXISTS (SELECT 1 FROM lion_mk_pf f WHERE f.fk = d.pk AND f.fk = ANY ($1)) GROUP BY d.attr', (SELECT quote_literal(array_agg(i)::text) FROM generate_series(1, 3000, 2) i));
DEALLOCATE lion_mk_long;
RESET work_mem;
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_nestloop;
RESET plan_cache_mode;
DROP FUNCTION lion_mk_analyze(text);
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET min_parallel_index_scan_size;
RESET parallel_leader_participation;
SET max_parallel_workers_per_gather = 0;

DROP TABLE lion_mk_t, lion_mk_d, lion_mk_pd, lion_mk_pf;
DROP FUNCTION lion_mk(text, text, text), lion_mk_pick(text),
	lion_mk_par(text, text);
RESET lion_mk.tags;
RESET lion_mk.q;
