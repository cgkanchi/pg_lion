-- The FK-side semi and anti join as a JOIN path (DESIGN.md §27, "The semi and
-- anti join as a join path"): LionSemiJoin and LionAntiJoin.
--
-- A join rel whose join is a semi or anti join, with one fact table alone on
-- its inner side joined on one fk equality to a column of the outer side, gets
-- a path that emits the outer side's rows with (or without) a match: each
-- outer row's key looked up in the fact's fk index and tested under the fact
-- filters.  The outer side may be any rel, a join rel included, and its rows
-- are then the input of whatever is above: a sort, another join, core's Agg,
-- or the upper node itself as its joined dimension.  Every answer is checked
-- against the same query run serially, and with the pushdown off and
-- sequential scans only.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET default_statistics_target = 1000;

-- parallel plans at any size; lion_sp() runs every query serially as well
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET parallel_leader_participation = off;

/*
 * lion_sp() runs a query with core's join methods disabled, so that the
 * semi and anti join paths are taken wherever they are offered: once as
 * planned - in parallel, when the plan says so - once with
 * max_parallel_workers_per_gather at 0, and once with the pushdown off and
 * sequential scans only.  It compares both of the first two with the third
 * as multisets, and says which of the paths the plan took: the semi join
 * path, the anti join path, both, or neither ("not taken"), and - with
 * `show` 'plan' - whether it was parallel and whether the upper node
 * (LionCount) sat above it.
 */
CREATE FUNCTION lion_sp(q text, show text DEFAULT '') RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	semi boolean := false;
	anti boolean := false;
	par boolean := false;
	upper boolean := false;
	how text;
	nrows bigint;
	ndiff bigint;
	sdiff bigint;
BEGIN
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (LionSemiJoin)%' THEN
			semi := true;
		ELSIF ln LIKE '%Custom Scan (LionAntiJoin)%' THEN
			anti := true;
		ELSIF ln LIKE '%Custom Scan (LionCount)%' THEN
			upper := true;
		END IF;
		IF ln LIKE '%Parallel Custom Scan (Lion%Join)%' THEN
			par := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lion_sp_par AS SELECT s::text AS r FROM (%s) s', q);

	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	EXECUTE format('CREATE TEMP TABLE lion_sp_ser AS SELECT s::text AS r FROM (%s) s', q);

	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_nestloop', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	EXECUTE format('CREATE TEMP TABLE lion_sp_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('max_parallel_workers_per_gather', '2', true);

	EXECUTE 'SELECT count(*) FROM lion_sp_par' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_sp_par EXCEPT ALL SELECT * FROM lion_sp_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_sp_off EXCEPT ALL SELECT * FROM lion_sp_par) b)'
		INTO ndiff;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_sp_ser EXCEPT ALL SELECT * FROM lion_sp_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_sp_off EXCEPT ALL SELECT * FROM lion_sp_ser) b)'
		INTO sdiff;
	EXECUTE 'DROP TABLE lion_sp_par, lion_sp_ser, lion_sp_off';

	IF ndiff <> 0 OR sdiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ in parallel, %s serially', ndiff, sdiff);
	END IF;
	how := CASE WHEN semi AND anti THEN 'semi and anti join paths'
				WHEN semi THEN 'semi join path'
				WHEN anti THEN 'anti join path'
				ELSE 'not taken' END;
	IF show = 'plan' AND (semi OR anti) THEN
		IF par THEN
			how := how || ', parallel';
		END IF;
		IF upper THEN
			how := how || ', under LionCount';
		END IF;
	END IF;
	RETURN format('%s, %s rows', how, nrows);
END $$;

/* A query's rows in the order it returns them. */
CREATE FUNCTION lion_sp_rows(q text) RETURNS text[]
LANGUAGE plpgsql AS $$
DECLARE
	r record;
	a text[] := '{}';
BEGIN
	FOR r IN EXECUTE q LOOP
		a := a || r::text;
	END LOOP;
	RETURN a;
END $$;

/*
 * An ordered query, joins disabled, against the pushdown off: the same rows
 * in the same order, and whether the plan needed a Sort above the path - it
 * needs none where the path claims its child's order.
 */
CREATE FUNCTION lion_sp_seq(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	path boolean := false;
	sorted boolean := false;
	a text[];
	b text[];
BEGIN
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (Lion%Join)%' THEN
			path := true;
		ELSIF ln ~ '^\s*(->  )?(Incremental )?Sort\s*$' THEN
			sorted := true;
		END IF;
	END LOOP;
	a := lion_sp_rows(q);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_nestloop', 'on', true);
	b := lion_sp_rows(q);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	IF a IS DISTINCT FROM b THEN
		RETURN 'MISMATCH';
	END IF;
	RETURN format('%s, %s, %s rows in order',
				  CASE WHEN path THEN 'join path' ELSE 'not taken' END,
				  CASE WHEN sorted THEN 'sorted above it' ELSE 'no sort' END,
				  cardinality(a));
END $$;

/* The answer itself, joins disabled: to pin a value, not only an agreement. */
CREATE FUNCTION lion_sp_val(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	RETURN QUERY EXECUTE format('SELECT s::text FROM (%s) s', q);
END $$;

/*
 * EXPLAIN (COSTS OFF), joins disabled, without the "Disabled" lines of 18.
 * With `collapse`, the child of the (first) semi or anti join path is one
 * line saying what it is: an outer side that is a join is planned as each
 * major plans a small join, and what matters is that it is one.
 */
CREATE FUNCTION lion_sp_explain(q text, collapse boolean DEFAULT false) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	seen boolean := false;
	depth int := -1;
	isjoin boolean := false;
	child text;
BEGIN
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		CONTINUE WHEN ln ~ '^\s*Disabled: true$';
		IF depth >= 0 THEN
			IF length(ln) - length(ltrim(ln)) > depth THEN
				isjoin := isjoin OR ln ~ '(Join|Nested Loop)';
				CONTINUE;
			END IF;
			RETURN NEXT child || CASE WHEN isjoin THEN 'a join)' ELSE 'a scan)' END;
			depth := -1;
		END IF;
		IF collapse AND seen AND ln ~ '^\s*->' THEN
			depth := length(ln) - length(ltrim(ln));
			isjoin := ln ~ '(Join|Nested Loop)';
			child := regexp_replace(ln, '->.*$', '->  (the outer side''s plan: ');
			seen := false;
			CONTINUE;
		END IF;
		IF ln LIKE '%Custom Scan (Lion%Join)%' THEN
			seen := true;
		END IF;
		RETURN NEXT ln;
	END LOOP;
	IF depth >= 0 THEN
		RETURN NEXT child || CASE WHEN isjoin THEN 'a join)' ELSE 'a scan)' END;
	END IF;
END $$;

/*
 * One counter of the first semi or anti join path in EXPLAIN ANALYZE, joins
 * disabled.
 */
CREATE FUNCTION lion_sp_counter(q text, counter text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	seen boolean := false;
BEGIN
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (Lion%Join)%' THEN
			seen := true;
		ELSIF seen AND btrim(split_part(ln, ':', 1)) = counter THEN
			RETURN btrim(split_part(ln, ':', 2))::bigint;
		END IF;
	END LOOP;
	RETURN NULL;
END $$;

/* Which plan the cost model picks with nothing disabled. */
CREATE FUNCTION lion_sp_pick(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	how text := '';
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			how := how || ' LionCount';
		ELSIF ln LIKE '%Custom Scan (LionSemiJoin)%' THEN
			how := how || ' LionSemiJoin';
		ELSIF ln LIKE '%Custom Scan (LionAntiJoin)%' THEN
			how := how || ' LionAntiJoin';
		END IF;
	END LOOP;
	RETURN CASE WHEN how = '' THEN 'ordinary plan' ELSE btrim(how) END;
END $$;

CREATE FUNCTION lion_sp_h(i int8, seed int8) RETURNS int8
LANGUAGE sql IMMUTABLE PARALLEL SAFE
AS 'SELECT hashint8extended(i, seed) & 9223372036854775807';

/*
 * The dimension: 3000 rows, int8 primary key 1..3000, attr 0..6 with NULL on
 * every 13th, a region and a group under a lion index.
 */
CREATE TABLE lion_sp_d (
	pk		int8	PRIMARY KEY,
	attr	int,
	region	text	NOT NULL,
	grp		text	NOT NULL
) WITH (parallel_workers = 2);
INSERT INTO lion_sp_d
SELECT i, CASE WHEN i % 13 = 0 THEN NULL ELSE lion_sp_h(i, 1) % 7 END,
	   (ARRAY['eu', 'us', 'ap'])[1 + (lion_sp_h(i, 3) % 3)::int], 'g' || lion_sp_h(i, 4) % 4
  FROM generate_series(1, 3000) i;
CREATE INDEX lion_sp_d_rg ON lion_sp_d USING lion (region, grp);

/*
 * An outer side whose key nothing proves unique: 3000 rows over 1,500 keys,
 * duplicated, NULL on every 29th row.
 */
CREATE TABLE lion_sp_n (
	id		int		NOT NULL,
	k		int8,
	attr	int		NOT NULL
) WITH (parallel_workers = 2);
INSERT INTO lion_sp_n
SELECT i, CASE WHEN i % 29 = 0 THEN NULL ELSE 1 + lion_sp_h(i, 11) % 1500 END,
	   lion_sp_h(i, 12) % 5
  FROM generate_series(1, 3000) i;

/* A small table the dimension is inner-joined to, on the outer side. */
CREATE TABLE lion_sp_g (grp text PRIMARY KEY, label text NOT NULL, flag bool NOT NULL);
INSERT INTO lion_sp_g VALUES ('g0', 'zero', true), ('g1', 'one', false),
	('g2', 'two', true), ('g3', 'three', true);

/*
 * f1: 40000 rows LIST-partitioned by kind, which no lion index covers - 'a'
 * sub-partitioned by range on ts, 'b' alone, 'c' and 'd' together, and a
 * default partition for the rest, NULL kind included - with a partitioned lion
 * index on (tags, ts, fk), one on x, and one on kind in the partition of 'c'
 * and 'd', whose bounds do not decide it.  ts is a day in the last sixty or
 * between 180 and 780 days back, so that `ts >= now() - interval '120 days'`,
 * a stable range, keeps the recent third whatever the day.  fk runs over
 * 1..3600, a third of the rows on keys 1..60, NULL on every 40th row.
 */
CREATE TABLE lion_sp_f1 (
	id		int		NOT NULL,
	kind	text,
	fk		int8,
	x		int		NOT NULL,
	tags	text[],
	ts		timestamptz NOT NULL
) PARTITION BY LIST (kind);
CREATE TABLE lion_sp_f1_a PARTITION OF lion_sp_f1 FOR VALUES IN ('a')
	PARTITION BY RANGE (ts);
DO $$
BEGIN
	EXECUTE format('CREATE TABLE lion_sp_f1_a_old PARTITION OF lion_sp_f1_a FOR VALUES FROM (%L) TO (%L)',
				   date_trunc('day', now()) - interval '3000 days',
				   date_trunc('day', now()) - interval '150 days');
	EXECUTE format('CREATE TABLE lion_sp_f1_a_new PARTITION OF lion_sp_f1_a FOR VALUES FROM (%L) TO (%L)',
				   date_trunc('day', now()) - interval '150 days',
				   date_trunc('day', now()) + interval '30 days');
END $$;
CREATE TABLE lion_sp_f1_b PARTITION OF lion_sp_f1 FOR VALUES IN ('b');
CREATE TABLE lion_sp_f1_cd PARTITION OF lion_sp_f1 FOR VALUES IN ('c', 'd');
CREATE TABLE lion_sp_f1_def PARTITION OF lion_sp_f1 DEFAULT;
INSERT INTO lion_sp_f1
SELECT i,
	   CASE lion_sp_h(i, 21) % 10 WHEN 0 THEN 'a' WHEN 1 THEN 'a' WHEN 2 THEN 'a'
				   WHEN 3 THEN 'b' WHEN 4 THEN 'b' WHEN 5 THEN 'c'
				   WHEN 6 THEN 'd' WHEN 7 THEN 'e' WHEN 8 THEN NULL ELSE 'b' END,
	   CASE WHEN i % 40 = 0 THEN NULL
			WHEN lion_sp_h(i, 22) % 3 = 0 THEN 1 + lion_sp_h(i, 23) % 60
			ELSE 1 + lion_sp_h(i, 23) % 3600 END,
	   lion_sp_h(i, 24) % 10,
	   ARRAY['t' || lion_sp_h(i, 25) % 11, 'u' || lion_sp_h(i, 26) % 7],
	   date_trunc('day', now()) -
	   (CASE WHEN lion_sp_h(i, 27) % 3 = 0 THEN lion_sp_h(i, 28) % 60
			 ELSE 180 + lion_sp_h(i, 28) % 600 END) * interval '1 day'
  FROM generate_series(1, 40000) i;
CREATE INDEX lion_sp_f1_tsf ON lion_sp_f1 USING lion (tags, ts, fk);
CREATE INDEX lion_sp_f1_x ON lion_sp_f1 USING lion (x);
CREATE INDEX lion_sp_f1_cd_kind ON lion_sp_f1_cd USING lion (kind);

/*
 * f2: 30000 rows of a plain table, fk over 1..3300, NULL on every 37th row
 * and no row for the keys that are multiples of 19, fk4 an int4 copy of it, a
 * tsvector of two lexemes and y 0..12; lion indexes on (doc, fk), on fk4 and
 * on y.
 */
CREATE TABLE lion_sp_f2 (
	id		int		NOT NULL,
	fk		int8,
	fk4		int4,
	doc		tsvector,
	y		int		NOT NULL
) WITH (parallel_workers = 2);
INSERT INTO lion_sp_f2
SELECT i, k, k::int4,
	   array_to_tsvector(ARRAY['w' || lion_sp_h(i, 32) % 17, 'v' || lion_sp_h(i, 33) % 5]),
	   lion_sp_h(i, 34) % 13
  FROM (SELECT i, CASE WHEN i % 37 = 0 THEN NULL
					   WHEN (1 + lion_sp_h(i, 31) % 3300) % 19 = 0 THEN NULL
					   ELSE 1 + lion_sp_h(i, 31) % 3300 END AS k
		  FROM generate_series(1, 30000) i) s;
CREATE INDEX lion_sp_f2_df ON lion_sp_f2 USING lion (doc, fk);
CREATE INDEX lion_sp_f2_fk4 ON lion_sp_f2 USING lion (fk4);
CREATE INDEX lion_sp_f2_y ON lion_sp_f2 USING lion (y);

/*
 * w: 24000 rows whose fk runs over 1..12000, every key twice in an order
 * unrelated to the key's, under an fk index built at fillfactor 20, so that
 * its directory has height 2 and the keys are looked up in key order - a
 * batch of rows sorted into it, and put back into the child's order.  x has
 * 10 values.
 */
CREATE TABLE lion_sp_w (id int NOT NULL, fk int8, x int NOT NULL);
INSERT INTO lion_sp_w
SELECT i, CASE WHEN k % 23 = 0 THEN NULL ELSE k END, i % 10
  FROM (SELECT i, (i * 7919) % 12000 + 1 AS k FROM generate_series(1, 24000) i) s;
CREATE INDEX lion_sp_w_fk ON lion_sp_w USING lion (fk) WITH (fillfactor = 20);
CREATE INDEX lion_sp_w_x ON lion_sp_w USING lion (x);

/* ... and its dimension: 4000 rows over every third key */
CREATE TABLE lion_sp_wd (pk int8 PRIMARY KEY, attr int NOT NULL);
INSERT INTO lion_sp_wd SELECT i * 3, i % 7 FROM generate_series(1, 4000) i;
CREATE INDEX lion_sp_wd_attr ON lion_sp_wd (attr, pk);
VACUUM (FREEZE, ANALYZE) lion_sp_d;
VACUUM (FREEZE, ANALYZE) lion_sp_n;
VACUUM (FREEZE, ANALYZE) lion_sp_g;
VACUUM (FREEZE, ANALYZE) lion_sp_f1;
VACUUM (FREEZE, ANALYZE) lion_sp_f2;
VACUUM (FREEZE, ANALYZE) lion_sp_w;
VACUUM (FREEZE, ANALYZE) lion_sp_wd;

-- ---- 1. the plans --------------------------------------------------------------
SET max_parallel_workers_per_gather = 0;
-- a semi join over a plain fact, and an anti join
SELECT * FROM lion_sp_explain('SELECT d.pk, d.attr FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.doc @@ ''w3 | w5'')');
SELECT * FROM lion_sp_explain('SELECT d.pk, d.attr FROM lion_sp_d d WHERE NOT EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.y = 5)');
-- the IN spelling, and a fact filter that only subtracts
SELECT * FROM lion_sp_explain('SELECT d.pk FROM lion_sp_d d WHERE d.pk IN (SELECT f2.fk FROM lion_sp_f2 f2 WHERE f2.fk4 IS NOT NULL)');
-- over the partitioned fact, with a stable range and a filter per kind ORed
SELECT * FROM lion_sp_explain('SELECT d.pk, d.grp FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND ((f1.kind = ''a'' AND f1.tags && ''{t1}'') OR (f1.kind = ''b'' AND f1.x = 3) OR f1.kind = ''c'') AND f1.ts >= now() - interval ''120 days'')');
-- an outer side that is a join: the dimension inner-joined to a small table
SELECT * FROM lion_sp_explain('SELECT d.pk, g.label FROM lion_sp_d d JOIN lion_sp_g g ON g.grp = d.grp WHERE g.flag AND EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.y = 5)', true);
-- ... and one that is itself a semi join path: a dimension semi-joined to two
-- facts, the second over the rows of the first
SELECT * FROM lion_sp_explain('SELECT d.pk FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.y < 4) AND NOT EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND f1.kind = ''b'' AND f1.x = 3)');
-- the outer side's order kept: in key order, a batch at a time, and put back
SELECT * FROM lion_sp_explain('SELECT wd.pk FROM lion_sp_wd wd WHERE EXISTS (SELECT 1 FROM lion_sp_w w WHERE w.fk = wd.pk AND w.x = 3) ORDER BY wd.pk DESC');
-- the rows of the join feeding a sort, and - nothing disabled, a few outer rows
-- against a fact filter that leaves most fact rows - a hash join
SELECT * FROM lion_sp_explain('SELECT d.pk, d.region FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.y = 5) ORDER BY d.region, d.pk');
-- (from PostgreSQL 19 eager aggregation would group the path's rows below the
-- join instead: off for this plan, where the setting exists)
DO $$BEGIN IF EXISTS (SELECT 1 FROM pg_settings WHERE name = 'enable_eager_aggregate') THEN PERFORM set_config('enable_eager_aggregate', 'off', false); END IF; END$$;
EXPLAIN (COSTS OFF) SELECT g.label, count(*) FROM lion_sp_g g JOIN lion_sp_d d ON d.grp = g.grp WHERE d.region = 'eu' AND d.attr = 3 AND EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND f1.x < 8) GROUP BY g.label;
DO $$BEGIN IF EXISTS (SELECT 1 FROM pg_settings WHERE name = 'enable_eager_aggregate') THEN EXECUTE 'RESET enable_eager_aggregate'; END IF; END$$;
-- the composition: the upper node over a dimension that is a join, whose
-- child is the semi join path - (A) a dimension semi-joined to two facts
SELECT * FROM lion_sp_explain('SELECT count(*) FROM lion_sp_d d WHERE d.region = ''eu'' AND EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND f1.kind = ''b'' AND f1.ts >= now() - interval ''120 days'') AND EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.doc @@ ''w3 | w5'')');
-- (B) a fact counted against a dimension narrowed by a semi join to another
SELECT * FROM lion_sp_explain('SELECT count(*) FROM lion_sp_f2 f2 WHERE f2.doc @@ ''w3 | w5'' AND f2.fk IN (SELECT d.pk FROM lion_sp_d d WHERE d.region = ''eu'' AND EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND ((f1.kind = ''a'' AND f1.tags && ''{t1}'') OR (f1.kind = ''b'' AND f1.x = 3) OR f1.kind = ''c'') AND f1.ts >= now() - interval ''120 days''))');
SET max_parallel_workers_per_gather = 2;
-- parallel: each participant tests its share of the outer rows
SELECT * FROM lion_sp_explain('SELECT d.pk, d.attr FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.doc @@ ''w3 | w5'')');
SELECT * FROM lion_sp_explain('SELECT d.pk FROM lion_sp_d d WHERE NOT EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND f1.kind = ''b'' AND f1.x = 3)');

-- ---- 2. the answers ------------------------------------------------------------
-- semi and anti joins over the plain fact, fact filters of every kind
SELECT lion_sp('SELECT d.pk, d.attr FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.doc @@ ''w3 | w5'')', 'plan');
SELECT lion_sp('SELECT d.pk, d.attr FROM lion_sp_d d WHERE NOT EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.doc @@ ''w3 | w5'')', 'plan');
SELECT lion_sp('SELECT d.* FROM lion_sp_d d WHERE d.region = ''eu'' AND EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.y IN (1, 4, 7))');
SELECT lion_sp('SELECT d.pk FROM lion_sp_d d WHERE d.pk IN (SELECT f2.fk FROM lion_sp_f2 f2 WHERE f2.y = 3 OR f2.doc @@ ''v1'')');
SELECT lion_sp('SELECT d.pk FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk)');
SELECT lion_sp('SELECT d.pk FROM lion_sp_d d WHERE NOT EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk)');
SELECT lion_sp('SELECT d.pk, d.grp FROM lion_sp_d d WHERE NOT EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.y IS NOT NULL)');
-- a fact filter that selects nothing: no row of the semi join, every row of the anti join
SELECT lion_sp('SELECT d.pk FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.y = 77)');
SELECT lion_sp('SELECT d.pk FROM lion_sp_d d WHERE NOT EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.y = 77)');
-- cross-type: the int8 key against the int4 fk
SELECT lion_sp('SELECT d.pk FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk4 = d.pk AND f2.y = 2)');
-- over the partitioned fact: one list partition, a sub-partitioned one, a
-- filter per kind ORed with a stable range, all of them, anti joins
SELECT lion_sp('SELECT d.pk FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND f1.kind = ''b'' AND f1.x = 3)', 'plan');
SELECT lion_sp('SELECT d.pk FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND f1.kind = ''a'' AND f1.ts >= now() - interval ''120 days'')', 'plan');
SELECT lion_sp('SELECT d.pk, d.grp FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND ((f1.kind = ''a'' AND f1.tags && ''{t1}'') OR (f1.kind = ''b'' AND f1.x = 3) OR f1.kind = ''c'') AND f1.ts >= now() - interval ''120 days'')', 'plan');
SELECT lion_sp('SELECT d.pk FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND f1.tags && ''{u3}'')');
SELECT lion_sp('SELECT d.pk FROM lion_sp_d d WHERE NOT EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND f1.kind = ''b'' AND f1.x = 3)', 'plan');
SELECT lion_sp('SELECT d.pk FROM lion_sp_d d WHERE NOT EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND ((f1.kind = ''a'' AND f1.tags && ''{t1}'') OR (f1.kind = ''b'' AND f1.x = 3) OR f1.kind = ''c'') AND f1.ts >= now() - interval ''120 days'')');
-- an outer side that is a join: an inner join, the semi join path's own rows,
-- and a semi join over an anti join
SELECT lion_sp('SELECT d.pk, g.label FROM lion_sp_d d JOIN lion_sp_g g ON g.grp = d.grp WHERE g.flag AND EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.y = 5)');
SELECT lion_sp('SELECT g.label, count(*) FROM lion_sp_g g JOIN lion_sp_d d ON d.grp = g.grp WHERE d.region = ''eu'' AND d.attr = 3 AND EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND f1.x < 8) GROUP BY g.label');
SELECT lion_sp('SELECT d.pk FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.y < 4) AND EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND f1.kind = ''b'')');
SELECT lion_sp('SELECT d.pk FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.y < 4) AND NOT EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND f1.kind = ''b'' AND f1.x = 3)', 'plan');
-- an outer side that is an outer join, the key on its nullable side: the row
-- it NULL-extends has a NULL key, which the anti join emits and the semi join not
SELECT lion_sp('SELECT g.grp, n.id FROM lion_sp_g g LEFT JOIN lion_sp_n n ON n.attr = length(g.label) + 1 WHERE NOT EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = n.k AND f2.y = 2)');
SELECT lion_sp('SELECT g.grp, n.id FROM lion_sp_g g LEFT JOIN lion_sp_n n ON n.attr = length(g.label) + 1 WHERE EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = n.k AND f2.y = 2)');
-- a system column, a whole row and expressions of the outer side's columns,
-- which the path's projection computes
SELECT lion_sp('SELECT d.ctid, d, d.pk + 1, upper(d.region) FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.y = 2)');
-- two facts on one key: one equivalence class of the three columns, whose
-- join rel of the dimension and the first fact carries that fact's column
SELECT lion_sp('SELECT d.pk, d.attr FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND f1.x = 2) AND EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.doc @@ ''w1'')');
SELECT lion_sp('SELECT d.pk FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk4 = d.pk AND f2.y = 2) AND EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND f1.x = 2)');
-- NULL keys: a semi join never matches one, an anti join emits it; and
-- duplicated keys, each outer row emitted once per its own occurrence
SELECT lion_sp('SELECT n.id, n.k FROM lion_sp_n n WHERE EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = n.k AND f2.y < 6)', 'plan');
SELECT lion_sp('SELECT n.id, n.k FROM lion_sp_n n WHERE NOT EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = n.k AND f2.y < 6)', 'plan');
SELECT lion_sp('SELECT n.k FROM lion_sp_n n WHERE n.k IN (SELECT f1.fk FROM lion_sp_f1 f1 WHERE f1.kind IN (''a'', ''b'') AND f1.tags && ''{u2}'')');
SELECT lion_sp('SELECT n.k, n.attr FROM lion_sp_n n WHERE NOT EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = n.k AND f1.x < 4)');
SELECT lion_sp_val('SELECT count(*), count(n.k), count(DISTINCT n.k) FROM lion_sp_n n WHERE EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = n.k AND f2.y < 6)');
SELECT lion_sp_val('SELECT count(*), count(n.k), count(DISTINCT n.k) FROM lion_sp_n n WHERE NOT EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = n.k AND f2.y < 6)');
SELECT lion_sp_val('SELECT count(*) FILTER (WHERE n.k IS NULL) FROM lion_sp_n n');
-- a key that appears c times among the outer rows is emitted c times: how
-- many keys are emitted how many times, against the outer side's own counts
-- of the keys that have a fact row
SELECT lion_sp_val('SELECT c, count(*) FROM (SELECT n.k, count(*) AS c FROM lion_sp_n n WHERE EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = n.k) GROUP BY n.k) s GROUP BY c ORDER BY c');
SET pg_lion.enable_count_pushdown = off;
SELECT c, count(*) FROM (SELECT k, count(*) AS c FROM lion_sp_n
						 WHERE k IN (SELECT fk FROM lion_sp_f2) GROUP BY k) s
GROUP BY c ORDER BY c;
RESET pg_lion.enable_count_pushdown;
-- the rows feeding a sort, core's Agg and a hash join
SELECT lion_sp('SELECT d.pk, d.region FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.y = 5) ORDER BY d.region, d.pk');
SELECT lion_sp('SELECT d.region, avg(d.attr), string_agg(d.pk::text, '','' ORDER BY d.pk) FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.y = 5) GROUP BY d.region');
SELECT lion_sp('SELECT g.label, count(*) FROM lion_sp_g g JOIN (SELECT d.grp FROM lion_sp_d d WHERE NOT EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.y = 5) OFFSET 0) s ON s.grp = g.grp GROUP BY g.label');
-- the composition: the upper node over a dimension that is the semi join path
SELECT lion_sp('SELECT count(*) FROM lion_sp_d d WHERE d.region = ''eu'' AND EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND f1.kind = ''b'' AND f1.ts >= now() - interval ''120 days'') AND EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.doc @@ ''w3 | w5'')', 'plan');
SELECT lion_sp('SELECT count(*) FROM lion_sp_f2 f2 WHERE f2.doc @@ ''w3 | w5'' AND f2.fk IN (SELECT d.pk FROM lion_sp_d d WHERE d.region = ''eu'' AND EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND ((f1.kind = ''a'' AND f1.tags && ''{t1}'') OR (f1.kind = ''b'' AND f1.x = 3) OR f1.kind = ''c'') AND f1.ts >= now() - interval ''120 days''))', 'plan');
SELECT lion_sp('SELECT d.grp, count(*) FROM lion_sp_d d WHERE NOT EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND f1.kind IN (''a'', ''b'') AND f1.ts >= now() - interval ''120 days'') AND EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.doc @@ ''w3'') GROUP BY d.grp');
SELECT lion_sp_val('SELECT count(*) FROM lion_sp_d d WHERE d.region = ''eu'' AND EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND f1.kind = ''b'' AND f1.ts >= now() - interval ''120 days'') AND EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.doc @@ ''w3 | w5'')');
SELECT lion_sp_val('SELECT count(*) FROM lion_sp_f2 f2 WHERE f2.doc @@ ''w3 | w5'' AND f2.fk IN (SELECT d.pk FROM lion_sp_d d WHERE d.region = ''eu'' AND EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND ((f1.kind = ''a'' AND f1.tags && ''{t1}'') OR (f1.kind = ''b'' AND f1.x = 3) OR f1.kind = ''c'') AND f1.ts >= now() - interval ''120 days''))');

-- ---- 3. order ------------------------------------------------------------------
-- the fk index of w has a directory of height 2, and the keys are looked up in
-- key order a batch at a time; the path claims the child's order, and the rows
-- come back in it - descending here, the batch sorted ascending, or by
-- another column first, the outer side read through an index on it (with sorts
-- off: a sort of what is left is cheaper) - with no Sort
-- (serially: a parallel plan's Gather Merge would need a Sort below it)
SET max_parallel_workers_per_gather = 0;
SELECT directory_height FROM lion_index_stats('lion_sp_w_fk');
SELECT lion_sp_seq('SELECT wd.pk FROM lion_sp_wd wd WHERE EXISTS (SELECT 1 FROM lion_sp_w w WHERE w.fk = wd.pk AND w.x = 3) ORDER BY wd.pk DESC');
SET enable_sort = off;
SELECT lion_sp_seq('SELECT wd.attr, wd.pk FROM lion_sp_wd wd WHERE NOT EXISTS (SELECT 1 FROM lion_sp_w w WHERE w.fk = wd.pk AND w.x < 5) ORDER BY wd.attr, wd.pk');
RESET enable_sort;
-- ... over many batches, a work_mem of 64 kB
SET work_mem = '64kB';
SELECT lion_sp_seq('SELECT wd.pk FROM lion_sp_wd wd WHERE EXISTS (SELECT 1 FROM lion_sp_w w WHERE w.fk = wd.pk AND w.x = 3) ORDER BY wd.pk DESC');
SELECT lion_sp_counter('SELECT wd.pk FROM lion_sp_wd wd WHERE EXISTS (SELECT 1 FROM lion_sp_w w WHERE w.fk = wd.pk AND w.x = 3) ORDER BY wd.pk DESC', 'Join Key Batches') > 1 AS batches;
SET enable_sort = off;
SELECT lion_sp_seq('SELECT wd.attr, wd.pk FROM lion_sp_wd wd WHERE NOT EXISTS (SELECT 1 FROM lion_sp_w w WHERE w.fk = wd.pk AND w.x < 5) ORDER BY wd.attr, wd.pk');
RESET enable_sort;
RESET work_mem;
-- the partitioned fact's batches, in the child's order too
SELECT lion_sp_seq('SELECT d.pk FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND f1.kind = ''b'' AND f1.x = 3) ORDER BY d.pk DESC');

-- ---- 4. the counters -----------------------------------------------------------
SET max_parallel_workers_per_gather = 0;
-- every outer row a child row, every non-NULL key looked up
SELECT lion_sp_counter('SELECT n.id FROM lion_sp_n n WHERE EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = n.k AND f2.y < 6)', 'Join Child Rows') AS child_rows,
	   lion_sp_counter('SELECT n.id FROM lion_sp_n n WHERE EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = n.k AND f2.y < 6)', 'Join Keys Looked Up') AS looked_up,
	   (SELECT count(k) FROM lion_sp_n) AS keys;
-- a fact filter that selects nothing: no lookup at all
SELECT lion_sp_counter('SELECT d.pk FROM lion_sp_d d WHERE NOT EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.y = 77)', 'Join Keys Looked Up') AS looked_up;
SET max_parallel_workers_per_gather = 2;
-- in parallel, the participants' sums
SELECT lion_sp_counter('SELECT d.pk, d.attr FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.doc @@ ''w3 | w5'')', 'Join Keys Looked Up') AS looked_up;

-- ---- 5. rescans, a correlated subplan, generic plans ---------------------------
-- the path as the inner side of a nested loop, parameterized by nothing and
-- rescanned for every outer row; serially and as a Gather
SET enable_material = off;
SELECT lion_sp('SELECT v.i, s.pk FROM (VALUES (1), (3), (5)) v(i) JOIN (SELECT d.pk, d.attr FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.doc @@ ''w3 | w5'') OFFSET 0) s ON s.attr = v.i');
SELECT lion_sp('SELECT v.i, count(s.pk) FROM (VALUES (1), (3), (5)) v(i) JOIN (SELECT d.pk, d.attr FROM lion_sp_d d WHERE NOT EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND f1.kind = ''b'' AND f1.x = 3) OFFSET 0) s ON s.attr = v.i GROUP BY v.i');
RESET enable_material;
-- a correlated subplan, with a new outer-side filter and a new fact filter
-- for every outer row, a NULL one among them
SELECT lion_sp('SELECT o.i, o.j, ARRAY(SELECT d.pk FROM lion_sp_d d WHERE d.attr = o.i AND EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.y = o.j) ORDER BY d.pk) FROM (VALUES (1, 2), (3, 4), (5, NULL), (2, 9)) o(i, j)');
SELECT lion_sp('SELECT o.i, ARRAY(SELECT d.pk FROM lion_sp_d d WHERE d.attr = o.i AND NOT EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND f1.kind = ''b'' AND f1.x = o.i) ORDER BY d.pk) FROM (VALUES (1), (3), (6)) o(i)');
-- generic plans with Params on both sides, a NULL one included, run as
-- planned - in parallel where the plan is - against the pushdown off
CREATE FUNCTION lion_sp_prep(q text, args text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	how text := 'not taken';
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('plan_cache_mode', 'force_generic_plan', true);
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	EXECUTE 'PREPARE lion_spp_on AS ' || q;
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) EXECUTE lion_spp_on(' || args || ')' LOOP
		IF ln LIKE '%Custom Scan (LionSemiJoin)%' THEN
			how := 'semi join path';
		ELSIF ln LIKE '%Custom Scan (LionAntiJoin)%' THEN
			how := 'anti join path';
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lion_sp_on AS EXECUTE lion_spp_on(%s)', args);

	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_nestloop', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	EXECUTE 'PREPARE lion_spp_off AS ' || q;
	EXECUTE format('CREATE TEMP TABLE lion_sp_off AS EXECUTE lion_spp_off(%s)', args);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE 'DEALLOCATE lion_spp_on';
	EXECUTE 'DEALLOCATE lion_spp_off';

	EXECUTE 'SELECT count(*) FROM lion_sp_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT s::text FROM lion_sp_on s EXCEPT ALL SELECT s::text FROM lion_sp_off s) a)'
			' + (SELECT count(*) FROM (SELECT s::text FROM lion_sp_off s EXCEPT ALL SELECT s::text FROM lion_sp_on s) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lion_sp_on, lion_sp_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows', how, nrows);
END $$;
SELECT lion_sp_prep('SELECT d.pk FROM lion_sp_d d WHERE d.attr = $1 AND EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.y = $2)', '3, 5');
SELECT lion_sp_prep('SELECT d.pk FROM lion_sp_d d WHERE d.attr = $1 AND EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.y = $2)', '3, NULL');
SELECT lion_sp_prep('SELECT d.pk FROM lion_sp_d d WHERE NOT EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.y = $1)', 'NULL');
SELECT lion_sp_prep('SELECT n.id FROM lion_sp_n n WHERE n.attr = $1 AND EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = n.k AND f2.doc @@ $2)', '2, ''w3 | w5''');
SELECT lion_sp_prep('SELECT d.pk FROM lion_sp_d d WHERE NOT EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND f1.kind = ''b'' AND f1.x = $1)', '3');
SELECT lion_sp_prep('SELECT d.pk FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND f1.kind = ''a'' AND f1.ts >= now() - $1::interval)', '''120 days''');
-- ... but a generic plan's `kind = $1` proves nothing of a partition's
-- bounds, and needs a lion index on kind in every partition, which only one has
SELECT lion_sp_prep('SELECT d.pk FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND f1.kind = $1 AND f1.x = 3)', '''b''');

-- ---- 6. declined ---------------------------------------------------------------
-- a second correlation
SELECT lion_sp('SELECT d.pk FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.y = d.attr)');
-- an outer-side qual inside NOT EXISTS, which stays a join clause: a second one
SELECT lion_sp('SELECT d.pk FROM lion_sp_d d WHERE NOT EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND d.attr = 2)');
-- ... where inside EXISTS it is a filter of the outer side, and taken
SELECT lion_sp('SELECT d.pk FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND d.attr = 2)');
-- a fact filter no posting set answers
SELECT lion_sp('SELECT d.pk FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.id % 3 = 0)');
-- NOT IN is no anti join - its NULL semantics differ - and over a nullable fk
-- core makes none of it
SELECT lion_sp('SELECT d.pk FROM lion_sp_d d WHERE d.pk NOT IN (SELECT f2.fk FROM lion_sp_f2 f2 WHERE f2.y = 5)');
SELECT lion_sp('SELECT n.id FROM lion_sp_n n WHERE n.k NOT IN (SELECT f2.fk FROM lion_sp_f2 f2 WHERE f2.y = 5 AND f2.fk IS NOT NULL)');
-- the anti join of LEFT JOIN ... IS NULL, which has a range table entry of its own
SELECT lion_sp('SELECT d.pk FROM lion_sp_d d LEFT JOIN lion_sp_f2 f2 ON f2.fk = d.pk AND f2.y = 5 WHERE f2.fk IS NULL');
-- a PlaceHolderVar among the join rel's rows
SELECT lion_sp('SELECT v.i, s.pk, s.a0 FROM (VALUES (1), (2)) v(i) LEFT JOIN (SELECT d.pk, d.attr, coalesce(d.attr, 0) AS a0 FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.y = 5)) s ON s.attr = v.i');
-- the fact joined outer: a semi join of the fact's rows, which the path does
-- not emit (the upper node counts them), and the forward IN over the
-- dimension, which has no lion index on its key
SELECT lion_sp('SELECT f2.id FROM lion_sp_f2 f2 WHERE f2.y = 5 AND EXISTS (SELECT 1 FROM lion_sp_d d WHERE d.pk = f2.fk AND d.region = ''eu'')');
-- an outer side with a LATERAL reference: taken only above the rel it needs
SELECT lion_sp('SELECT g.grp, s.pk FROM lion_sp_g g, LATERAL (SELECT d.pk FROM lion_sp_d d WHERE d.grp = g.grp AND d.attr = 3 OFFSET 0) s WHERE EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = s.pk AND f2.y < 6)');
-- the pushdown switched off, and the path alone
SET pg_lion.enable_semijoin = off;
SELECT lion_sp('SELECT d.pk, d.attr FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.doc @@ ''w3 | w5'')');
RESET pg_lion.enable_semijoin;
SET pg_lion.enable_count_pushdown = off;
SELECT lion_sp_pick('SELECT d.pk, d.attr FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.doc @@ ''w3 | w5'')');
RESET pg_lion.enable_count_pushdown;

-- ---- 7. the cost model, nothing disabled -----------------------------------------
SET max_parallel_workers_per_gather = 0;
-- a few outer rows against a fact filter that leaves most fact rows: the path
SELECT lion_sp_pick('SELECT d.pk, d.attr FROM lion_sp_d d WHERE d.region = ''eu'' AND d.grp = ''g1'' AND EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND f1.x < 8)');
-- every outer row against a fact filter that leaves few fact rows: core's join
SELECT lion_sp_pick('SELECT d.pk, d.attr FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.doc @@ ''w3'' AND f2.y = 2)');
-- a count of them: the upper node, which hands up no row
SELECT lion_sp_pick('SELECT count(*) FROM lion_sp_d d WHERE d.region = ''eu'' AND d.grp = ''g1'' AND EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND f1.x < 8)');
SET max_parallel_workers_per_gather = 2;

-- ---- 8. a dirty heap, before and after VACUUM ----------------------------------------
DELETE FROM lion_sp_f2 WHERE id % 11 = 0;
UPDATE lion_sp_f2 SET y = (y + 1) % 13 WHERE id % 7 = 0;
UPDATE lion_sp_f2 SET fk = fk + 1, fk4 = fk4 + 1 WHERE id % 13 = 0;
DELETE FROM lion_sp_f1 WHERE id % 9 = 0;
UPDATE lion_sp_f1 SET x = (x + 3) % 10 WHERE id % 5 = 0;
UPDATE lion_sp_f1 SET fk = 5 WHERE id % 17 = 0 AND fk IS NOT NULL;
DELETE FROM lion_sp_d WHERE pk % 23 = 0;
UPDATE lion_sp_n SET k = NULL WHERE id % 31 = 0;
UPDATE lion_sp_n SET k = k + 1 WHERE id % 37 = 0;
DELETE FROM lion_sp_w WHERE id % 13 = 0;
SELECT lion_sp('SELECT d.pk, d.attr FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.doc @@ ''w3 | w5'')');
SELECT lion_sp('SELECT d.pk FROM lion_sp_d d WHERE NOT EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.y = 5)');
SELECT lion_sp('SELECT d.pk, d.grp FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND ((f1.kind = ''a'' AND f1.tags && ''{t1}'') OR (f1.kind = ''b'' AND f1.x = 3) OR f1.kind = ''c'') AND f1.ts >= now() - interval ''120 days'')');
SELECT lion_sp('SELECT n.id, n.k FROM lion_sp_n n WHERE NOT EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = n.k AND f1.x < 4)');
SELECT lion_sp('SELECT count(*) FROM lion_sp_f2 f2 WHERE f2.doc @@ ''w3 | w5'' AND f2.fk IN (SELECT d.pk FROM lion_sp_d d WHERE d.region = ''eu'' AND EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND f1.kind = ''b''))', 'plan');
SET max_parallel_workers_per_gather = 0;
SELECT lion_sp_seq('SELECT wd.pk FROM lion_sp_wd wd WHERE EXISTS (SELECT 1 FROM lion_sp_w w WHERE w.fk = wd.pk AND w.x = 3) ORDER BY wd.pk DESC');
SET max_parallel_workers_per_gather = 2;
VACUUM lion_sp_f1;
VACUUM lion_sp_f2;
VACUUM lion_sp_d;
VACUUM lion_sp_n;
VACUUM lion_sp_w;
SELECT lion_sp('SELECT d.pk, d.attr FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.doc @@ ''w3 | w5'')');
SELECT lion_sp('SELECT d.pk FROM lion_sp_d d WHERE NOT EXISTS (SELECT 1 FROM lion_sp_f2 f2 WHERE f2.fk = d.pk AND f2.y = 5)');
SELECT lion_sp('SELECT d.pk, d.grp FROM lion_sp_d d WHERE EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND ((f1.kind = ''a'' AND f1.tags && ''{t1}'') OR (f1.kind = ''b'' AND f1.x = 3) OR f1.kind = ''c'') AND f1.ts >= now() - interval ''120 days'')');
SELECT lion_sp('SELECT n.id, n.k FROM lion_sp_n n WHERE NOT EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = n.k AND f1.x < 4)');
SELECT lion_sp('SELECT count(*) FROM lion_sp_f2 f2 WHERE f2.doc @@ ''w3 | w5'' AND f2.fk IN (SELECT d.pk FROM lion_sp_d d WHERE d.region = ''eu'' AND EXISTS (SELECT 1 FROM lion_sp_f1 f1 WHERE f1.fk = d.pk AND f1.kind = ''b''))', 'plan');
SET max_parallel_workers_per_gather = 0;
SELECT lion_sp_seq('SELECT wd.pk FROM lion_sp_wd wd WHERE EXISTS (SELECT 1 FROM lion_sp_w w WHERE w.fk = wd.pk AND w.x = 3) ORDER BY wd.pk DESC');
SET max_parallel_workers_per_gather = 2;

DROP TABLE lion_sp_d, lion_sp_n, lion_sp_g, lion_sp_f1, lion_sp_f2, lion_sp_w, lion_sp_wd;
DROP FUNCTION lion_sp(text, text), lion_sp_rows(text), lion_sp_seq(text),
	lion_sp_val(text), lion_sp_explain(text, boolean), lion_sp_counter(text, text),
	lion_sp_pick(text), lion_sp_prep(text, text), lion_sp_h(int8, int8);
