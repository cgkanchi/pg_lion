-- FK-side join pushdown grouped by a FACT column (DESIGN.md §27, "Grouped by
-- a fact column").
--
-- A dimension row's count is one posting set ANDed with the fact filters, so
-- the node counts each key once per group of the fact column instead - the
-- group's own posting set ANDed in, an entry of a lion index on the column -
-- and hands up a partial count per dimension row and group, which the
-- Finalize Agg adds up per group.  In a partition whose bounds give the
-- column one value every row is of that group, and the key's count there is
-- the group's, with no index on the column at all.  Every answer is checked
-- against the same query run serially through the pushdown, and with the
-- pushdown off and sequential scans only.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
-- the upper node's plans are the ones pinned here (DESIGN.md §27)
SET pg_lion.enable_semijoin = off;
SET default_statistics_target = 1000;

-- parallel plans at any size; the suite's own setting is 0, and lion_xj()
-- runs every query serially as well
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET parallel_leader_participation = off;

/*
 * lion_xj() runs a query through the pushdown with every join method
 * disabled, so that the node is exercised: once as planned - in parallel,
 * when the plan says so - once with max_parallel_workers_per_gather at 0, and
 * once with the pushdown off and sequential scans only.  It compares both of
 * the first two with the third and says which plan the first one had.
 */
CREATE FUNCTION lion_xj(q text) RETURNS text
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
	EXECUTE format('CREATE TEMP TABLE lion_xj_par AS SELECT s::text AS r FROM (%s) s', q);

	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	EXECUTE format('CREATE TEMP TABLE lion_xj_ser AS SELECT s::text AS r FROM (%s) s', q);

	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_nestloop', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	EXECUTE format('CREATE TEMP TABLE lion_xj_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('max_parallel_workers_per_gather', '2', true);

	EXECUTE 'SELECT count(*) FROM lion_xj_par' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_xj_par EXCEPT ALL SELECT * FROM lion_xj_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_xj_off EXCEPT ALL SELECT * FROM lion_xj_par) b)'
		INTO ndiff;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_xj_ser EXCEPT ALL SELECT * FROM lion_xj_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_xj_off EXCEPT ALL SELECT * FROM lion_xj_ser) b)'
		INTO sdiff;
	EXECUTE 'DROP TABLE lion_xj_par, lion_xj_ser, lion_xj_off';

	IF ndiff <> 0 OR sdiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ in parallel, %s serially', ndiff, sdiff);
	END IF;
	RETURN format('%s, %s rows', how, nrows);
END $$;

/*
 * The node and what is above it, joins disabled, with the dimension's own plan
 * as one line: that plan is whatever each major makes of a small scan.
 */
CREATE FUNCTION lion_xj_explain(q text) RETURNS SETOF text
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

/* Whether the plan has the node, nothing disabled. */
CREATE FUNCTION lion_xj_plans(q text) RETURNS boolean
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			RETURN true;
		END IF;
	END LOOP;
	RETURN false;
END $$;

/* One counter of the node's EXPLAIN ANALYZE output, joins disabled. */
CREATE FUNCTION lion_xj_counter(q text, counter text) RETURNS bigint
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
 * The dimension: 3000 rows, int8 primary key 1..3000, a region and a group;
 * and a second one over 1,500 keys, duplicated, with NULL keys and no
 * uniqueness, the inner side of a forward semi join.
 */
CREATE TABLE lion_gd (
	pk		int8	PRIMARY KEY,
	attr	int,
	region	text	NOT NULL,
	grp		text	NOT NULL
) WITH (parallel_workers = 2);
INSERT INTO lion_gd
SELECT i, CASE WHEN i % 13 = 0 THEN NULL ELSE i % 7 END,
	   (ARRAY['eu', 'us', 'ap'])[1 + i % 3], 'g' || (i % 4)
  FROM generate_series(1, 3000) i;
CREATE TABLE lion_gdn (
	k		int8,
	region	text	NOT NULL
) WITH (parallel_workers = 2);
INSERT INTO lion_gdn
SELECT CASE WHEN i % 29 = 0 THEN NULL ELSE 1 + (i * 7) % 1500 END,
	   (ARRAY['eu', 'us', 'ap'])[1 + i % 3]
  FROM generate_series(1, 3000) i;

/*
 * The fact: 60000 rows LIST-partitioned by kind - 'a' sub-partitioned by year
 * on ts, 'b' alone, 'c' and 'd' together, and a default partition for the
 * rest, NULL kind included - so that the bounds give kind one value in three
 * partitions and not in the other two.  One partitioned lion index on (tags,
 * ts, fk), one on x, and one on kind, which only the partitions of several
 * kinds need for a GROUP BY of it.  lvl is 0..99 and has an index of its own:
 * more groups than one chunk locates at once.
 */
CREATE TABLE lion_gf (
	id		int		NOT NULL,
	kind	text,
	fk		int8,
	x		int		NOT NULL,
	lvl		int,
	amt		numeric,
	tags	text[],
	ts		timestamptz NOT NULL
) PARTITION BY LIST (kind);
CREATE TABLE lion_gf_a PARTITION OF lion_gf FOR VALUES IN ('a')
	PARTITION BY RANGE (ts);
CREATE TABLE lion_gf_a_2025 PARTITION OF lion_gf_a
	FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');
CREATE TABLE lion_gf_a_2026 PARTITION OF lion_gf_a
	FOR VALUES FROM ('2026-01-01') TO ('2027-01-01');
CREATE TABLE lion_gf_b PARTITION OF lion_gf FOR VALUES IN ('b');
CREATE TABLE lion_gf_cd PARTITION OF lion_gf FOR VALUES IN ('c', 'd');
CREATE TABLE lion_gf_def PARTITION OF lion_gf DEFAULT;
INSERT INTO lion_gf
SELECT i,
	   CASE i % 10 WHEN 0 THEN 'a' WHEN 1 THEN 'a' WHEN 2 THEN 'a'
				   WHEN 3 THEN 'b' WHEN 4 THEN 'b' WHEN 5 THEN 'c'
				   WHEN 6 THEN 'd' WHEN 7 THEN 'e' WHEN 8 THEN NULL ELSE 'b' END,
	   CASE WHEN i % 40 = 0 THEN NULL
			WHEN i % 3 = 0 THEN 1 + (i * 7919) % 60
			ELSE 1 + (i * 7919) % 3600 END,
	   i % 10,
	   CASE WHEN i % 23 = 0 THEN NULL ELSE (i * 31) % 100 END,
	   (i % 5)::numeric,
	   ARRAY['t' || (i % 11), 'u' || (i % 7)],
	   timestamptz '2025-01-01' + ((i * 37) % 700) * interval '1 day'
  FROM generate_series(1, 60000) i;
CREATE INDEX lion_gf_tsf ON lion_gf USING lion (tags, ts, fk);
CREATE INDEX lion_gf_x ON lion_gf USING lion (x);
CREATE INDEX lion_gf_kind ON lion_gf USING lion (kind);
CREATE INDEX lion_gf_lvl ON lion_gf USING lion (lvl);
CREATE INDEX lion_gf_amt ON lion_gf USING lion (amt);

/* The same rows in a plain table, with an index on fk and one on x, lvl, kind. */
CREATE TABLE lion_gp (LIKE lion_gf) WITH (parallel_workers = 2);
INSERT INTO lion_gp SELECT * FROM lion_gf;
CREATE INDEX lion_gp_fk ON lion_gp USING lion (fk, tags);
CREATE INDEX lion_gp_x ON lion_gp USING lion (x);
CREATE INDEX lion_gp_kind ON lion_gp USING lion (kind);
CREATE INDEX lion_gp_lvl ON lion_gp USING lion (lvl);
VACUUM (FREEZE, ANALYZE) lion_gd;
VACUUM (FREEZE, ANALYZE) lion_gdn;
VACUUM (FREEZE, ANALYZE) lion_gf;
VACUUM (FREEZE, ANALYZE) lion_gp;

-- ---- 1. the plans --------------------------------------------------------------
-- grouped by the partition key: three partitions' bounds give it, the other
-- two count each key once per entry of their index on kind
SELECT * FROM lion_xj_explain('SELECT f.kind, count(*) FROM lion_gf f JOIN lion_gd d ON f.fk = d.pk WHERE d.region = ''eu'' GROUP BY f.kind');
-- a filter per kind, ORed, beside a range, over the keys of a dimension's
-- filtered rows (an IN list of them), grouped by kind and ordered by count
SELECT * FROM lion_xj_explain('SELECT f.kind, count(*) FROM lion_gf f WHERE ((f.kind = ''a'' AND f.tags && ''{t1}'' AND f.tags && ''{u2}'') OR (f.kind = ''b'' AND f.x = 3) OR f.kind = ''c'' OR f.kind = ''d'') AND f.ts >= ''2025-06-01'' AND f.fk IN (SELECT d.pk FROM lion_gd d WHERE d.region = ''eu'' AND d.grp IN (''g1'', ''g2'')) GROUP BY f.kind ORDER BY 2 DESC');
-- a column no bound gives a value, over partitions and over a plain table
SELECT * FROM lion_xj_explain('SELECT f.x, count(*) FROM lion_gf f JOIN lion_gd d ON f.fk = d.pk WHERE d.region = ''us'' AND f.kind IN (''a'', ''b'') GROUP BY f.x');
SELECT * FROM lion_xj_explain('SELECT f.x, count(*) FROM lion_gp f JOIN lion_gd d ON f.fk = d.pk WHERE d.region = ''us'' AND f.tags && ''{t2}'' GROUP BY f.x');

-- ---- 2. the answers ------------------------------------------------------------
-- the partition key: every kind, the default partition's and the NULL group
SELECT lion_xj('SELECT f.kind, count(*) FROM lion_gf f JOIN lion_gd d ON f.fk = d.pk WHERE d.region = ''eu'' GROUP BY f.kind');
SELECT lion_xj('SELECT f.kind, count(*) FROM lion_gf f JOIN lion_gd d ON f.fk = d.pk GROUP BY f.kind');
SELECT lion_xj('SELECT f.kind, count(*) FROM lion_gf f JOIN lion_gd d ON f.fk = d.pk WHERE f.tags && ''{t3,u4}'' AND f.ts < ''2026-02-01'' GROUP BY f.kind');
SELECT lion_xj('SELECT f.kind, count(*) FROM lion_gf f WHERE ((f.kind = ''a'' AND f.tags && ''{t1}'' AND f.tags && ''{u2}'') OR (f.kind = ''b'' AND f.x = 3) OR f.kind = ''c'' OR f.kind = ''d'') AND f.ts >= ''2025-06-01'' AND f.fk IN (SELECT d.pk FROM lion_gd d WHERE d.region = ''eu'' AND d.grp IN (''g1'', ''g2'')) GROUP BY f.kind ORDER BY 2 DESC');
-- beside dimension columns, with HAVING, an expression of it, ORDER BY, LIMIT
SELECT lion_xj('SELECT f.kind, d.region, count(*) FROM lion_gf f JOIN lion_gd d ON f.fk = d.pk WHERE f.x IN (1, 5, 6) GROUP BY f.kind, d.region');
SELECT lion_xj('SELECT d.grp, f.kind, count(1) FROM lion_gf f JOIN lion_gd d ON f.fk = d.pk WHERE f.tags && ''{u1}'' GROUP BY d.grp, f.kind HAVING count(*) > 50');
SELECT lion_xj('SELECT upper(f.kind), count(f.fk) FROM lion_gf f JOIN lion_gd d ON f.fk = d.pk WHERE d.attr = 2 GROUP BY f.kind');
SELECT lion_xj('SELECT f.kind, count(*) FROM lion_gf f JOIN lion_gd d ON f.fk = d.pk WHERE d.grp = ''g3'' GROUP BY f.kind ORDER BY 2 DESC, 1 LIMIT 3');
-- beside the join key, which stands for the dimension's; and pinned by a
-- filter, which leaves the Agg no column to group by
SELECT lion_xj('SELECT f.fk, f.kind, count(*) FROM lion_gf f JOIN lion_gd d ON f.fk = d.pk WHERE d.grp = ''g1'' AND f.x = 4 GROUP BY f.fk, f.kind');
SELECT lion_xj('SELECT f.kind, count(*) FROM lion_gf f JOIN lion_gd d ON f.fk = d.pk WHERE f.kind = ''b'' AND d.region = ''eu'' GROUP BY f.kind');
SELECT lion_xj('SELECT f.x, count(*) FROM lion_gp f JOIN lion_gd d ON f.fk = d.pk WHERE f.x = 6 AND d.region = ''us'' GROUP BY f.x');
-- other columns, over partitions and over the plain table: x, and lvl, whose
-- hundred values and NULL are more groups than one chunk
SELECT lion_xj('SELECT f.x, count(*) FROM lion_gf f JOIN lion_gd d ON f.fk = d.pk WHERE d.region = ''us'' AND f.kind IN (''a'', ''b'') GROUP BY f.x');
SELECT lion_xj('SELECT f.x, count(*) FROM lion_gp f JOIN lion_gd d ON f.fk = d.pk WHERE d.region = ''us'' AND f.tags && ''{t2}'' GROUP BY f.x');
SELECT lion_xj('SELECT f.lvl, count(*) FROM lion_gf f JOIN lion_gd d ON f.fk = d.pk WHERE d.grp = ''g1'' GROUP BY f.lvl');
SELECT lion_xj('SELECT f.lvl, d.region, count(*) FROM lion_gp f JOIN lion_gd d ON f.fk = d.pk WHERE f.x < 4 GROUP BY f.lvl, d.region');
SELECT lion_xj('SELECT f.kind, count(*) FROM lion_gp f JOIN lion_gd d ON f.fk = d.pk WHERE d.region = ''ap'' GROUP BY f.kind');
-- the forward semi join over a non-unique key, counted over its distinct keys
SELECT lion_xj('SELECT f.kind, count(*) FROM lion_gf f WHERE f.x IN (1, 2, 7) AND f.fk IN (SELECT n.k FROM lion_gdn n WHERE n.region = ''eu'') GROUP BY f.kind');
SELECT lion_xj('SELECT f.x, count(*) FROM lion_gp f WHERE f.fk IN (SELECT n.k FROM lion_gdn n WHERE n.region = ''us'') GROUP BY f.x');
-- a filter that selects nothing in some partitions, and in all of them
SELECT lion_xj('SELECT f.kind, count(*) FROM lion_gf f JOIN lion_gd d ON f.fk = d.pk WHERE f.kind IN (''a'', ''c'') AND f.x = 3 GROUP BY f.kind');
SELECT lion_xj('SELECT f.kind, count(*) FROM lion_gf f JOIN lion_gd d ON f.fk = d.pk WHERE f.x = 99 GROUP BY f.kind');

-- ---- 3. the counters -----------------------------------------------------------
/*
 * A partition whose bounds give the column its value counts a key once, as
 * any partition does: over the partitions of 'a' and 'b' alone no count is
 * made per group.  Over the others each key found is counted once per entry
 * of the index on kind there.
 */
SET max_parallel_workers_per_gather = 0;
SELECT lion_xj_counter('SELECT f.kind, count(*) FROM lion_gf f JOIN lion_gd d ON f.fk = d.pk WHERE f.kind IN (''a'', ''b'') GROUP BY f.kind', 'Fact Group Counts') AS bound_only,
	   lion_xj_counter('SELECT f.kind, count(*) FROM lion_gf f JOIN lion_gd d ON f.fk = d.pk WHERE f.kind IN (''c'', ''d'') GROUP BY f.kind', 'Fact Group Counts') > 0 AS by_index;
/*
 * Batches: at a work_mem of 64 kB the keys are read in several, each taken to
 * every partition in turn, and the answers do not change.
 */
SET work_mem = '64kB';
SELECT lion_xj_counter('SELECT f.kind, count(*) FROM lion_gf f JOIN lion_gd d ON f.fk = d.pk GROUP BY f.kind', 'Join Key Batches') > 1 AS batches;
SET max_parallel_workers_per_gather = 2;
SELECT lion_xj('SELECT f.kind, count(*) FROM lion_gf f JOIN lion_gd d ON f.fk = d.pk GROUP BY f.kind');
SELECT lion_xj('SELECT f.lvl, count(*) FROM lion_gp f JOIN lion_gd d ON f.fk = d.pk WHERE d.region = ''eu'' GROUP BY f.lvl');
RESET work_mem;

-- ---- 4. rescans and a dirty heap -----------------------------------------------
-- a correlated subquery that rescans the node with a new fact filter
SELECT lion_xj('SELECT v.x, (SELECT string_agg(k || '':'' || c, '','' ORDER BY k) FROM (SELECT f.kind AS k, count(*) AS c FROM lion_gf f JOIN lion_gd d ON f.fk = d.pk WHERE d.region = ''us'' AND f.x = v.x GROUP BY f.kind) s) FROM (VALUES (1), (4), (8)) v(x)');
DELETE FROM lion_gf WHERE kind = 'c' AND id % 7 = 0;
UPDATE lion_gf SET kind = 'd' WHERE kind = 'c' AND id % 11 = 0;
UPDATE lion_gf SET kind = 'b' WHERE kind = 'a' AND id % 13 = 0;
UPDATE lion_gp SET x = 3 WHERE id % 17 = 0;
SELECT lion_xj('SELECT f.kind, count(*) FROM lion_gf f JOIN lion_gd d ON f.fk = d.pk WHERE d.region = ''eu'' GROUP BY f.kind');
SELECT lion_xj('SELECT f.x, count(*) FROM lion_gp f JOIN lion_gd d ON f.fk = d.pk WHERE d.region = ''us'' GROUP BY f.x');
VACUUM (FREEZE) lion_gf;
VACUUM (FREEZE) lion_gp;
SELECT lion_xj('SELECT f.kind, count(*) FROM lion_gf f JOIN lion_gd d ON f.fk = d.pk WHERE d.region = ''eu'' GROUP BY f.kind');
SELECT lion_xj('SELECT f.x, count(*) FROM lion_gp f JOIN lion_gd d ON f.fk = d.pk WHERE d.region = ''us'' GROUP BY f.x');

-- ---- 5. declined ---------------------------------------------------------------
/*
 * An expression of a fact column, which no index's entries are the groups
 * of; two fact columns; an aggregate other than a count beside the fact
 * column; a numeric column, whose equal values print differently (DESIGN.md
 * §10); and a partition whose bounds do not give the column its value, with
 * no index on it: the ordinary plan.
 */
SELECT lion_xj('SELECT f.id % 3, count(*) FROM lion_gf f JOIN lion_gd d ON f.fk = d.pk WHERE d.region = ''eu'' GROUP BY f.id % 3');
SELECT lion_xj('SELECT f.kind, f.x, count(*) FROM lion_gf f JOIN lion_gd d ON f.fk = d.pk WHERE d.region = ''eu'' GROUP BY f.kind, f.x');
SELECT lion_xj('SELECT f.kind, count(DISTINCT d.attr) FROM lion_gf f JOIN lion_gd d ON f.fk = d.pk GROUP BY f.kind');
SELECT lion_xj('SELECT f.amt, count(*) FROM lion_gf f JOIN lion_gd d ON f.fk = d.pk WHERE d.region = ''eu'' GROUP BY f.amt');
DROP INDEX lion_gf_kind;
SELECT lion_xj('SELECT f.kind, count(*) FROM lion_gf f JOIN lion_gd d ON f.fk = d.pk WHERE d.region = ''eu'' GROUP BY f.kind');
-- ... but over the partitions whose bounds give it, it needs none
SELECT lion_xj('SELECT f.kind, count(*) FROM lion_gf f JOIN lion_gd d ON f.fk = d.pk WHERE d.region = ''eu'' AND f.kind IN (''a'', ''b'') GROUP BY f.kind');

DROP TABLE lion_gf, lion_gp, lion_gd, lion_gdn;
DROP FUNCTION lion_xj(text);
DROP FUNCTION lion_xj_explain(text);
DROP FUNCTION lion_xj_plans(text);
DROP FUNCTION lion_xj_counter(text, text);
