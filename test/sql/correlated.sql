-- A correlated subquery's lion scan, priced over its calls (DESIGN.md
-- §40.11).
--
-- A scalar sub-select or an EXISTS that refers to the query around it is
-- planned with no loop count and run again for every outer row, the outer
-- row's value arriving as an exec Param.  Core prices its scans for one
-- call, and so did lion: the pages its sets are sought in, its directory
-- leaves and a set built once and kept were charged at every call as at the
-- first.  A lion path whose quals hold such a Param is now priced as the
-- inner side of a nested loop over the outer relation's rows: its index pages
-- spread over the calls as a parameterized scan's are, a set no Param reaches
-- built once for all of them.  A Param that is not an outer row's - a
-- prepared statement's own, an InitPlan's - is not a loop and keeps the
-- price of one call.  Every answer is compared with core's.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

-- A dimension of 100,208 keys, and a fact of 300,000 rows on them: keys 1
-- to 8 have 5,000 rows each, keys 9 to 208 300 each, and the other 100,000
-- one to three; the key's column analyzed at a statistics target of 8, so
-- that its most common values are the eight largest keys.  doc holds three
-- words, a of three values, b of four and c of two.  The fact's lion index
-- holds both, and a B-tree on the key covers a count and max(id).
CREATE TABLE lsq_dim (id int PRIMARY KEY, grp int NOT NULL);
INSERT INTO lsq_dim SELECT i, i % 100 FROM generate_series(1, 100208) i;
CREATE TABLE lsq_fact (id int NOT NULL, fk int NOT NULL, doc tsvector NOT NULL,
					   pad text) WITH (autovacuum_enabled = off);
INSERT INTO lsq_fact
SELECT i,
	   CASE WHEN i <= 40000 THEN 1 + i % 8
			WHEN i <= 100000 THEN 9 + i % 200
			ELSE 209 + (hashint4(i) & 2147483647) % 100000 END,
	   to_tsvector('simple',
				   'a' || (hashint4(i + 1) & 2147483647) % 3 ||
				   ' b' || (hashint4(i + 2) & 2147483647) % 4 ||
				   ' c' || (hashint4(i + 3) & 2147483647) % 2),
	   repeat('p', 60)
  FROM generate_series(1, 300000) i
 ORDER BY hashint4(i + 9);
ALTER TABLE lsq_fact ALTER COLUMN fk SET STATISTICS 8;
CREATE INDEX lsq_fact_lion ON lsq_fact USING lion (fk, doc);
CREATE INDEX lsq_fact_fk ON lsq_fact (fk) INCLUDE (id);
VACUUM (FREEZE, ANALYZE) lsq_dim;
VACUUM (FREEZE, ANALYZE) lsq_fact;

/*
 * lsq_plan() is q's plan as its nodes' names, a node's children in brackets
 * after it: a custom scan by its name, an index scan with its index's, a
 * SubPlan's nodes among its parent's children.  lsq_cost() is the total cost
 * (or another of its costs) of the first node of q's plan, top down, whose
 * name is `node`.
 */
CREATE FUNCTION lsq_names(p json) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	r text := coalesce(p->>'Custom Plan Provider', p->>'Node Type') ||
		coalesce(' ' || (p->>'Index Name'), '');
	c json;
	kids text[] := '{}';
BEGIN
	FOR c IN SELECT * FROM json_array_elements(coalesce(p->'Plans', '[]'::json))
	LOOP
		kids := kids || lsq_names(c);
	END LOOP;
	IF cardinality(kids) > 0 THEN
		r := r || '[' || array_to_string(kids, ', ') || ']';
	END IF;
	RETURN r;
END $$;
CREATE FUNCTION lsq_plan(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	p json;
BEGIN
	EXECUTE 'EXPLAIN (COSTS OFF, FORMAT JSON) ' || q INTO p;
	RETURN lsq_names(p->0->'Plan');
END $$;
CREATE FUNCTION lsq_find(p json, node text, field text) RETURNS float8
LANGUAGE plpgsql AS $$
DECLARE
	c json;
	r float8;
BEGIN
	IF coalesce(p->>'Custom Plan Provider', p->>'Node Type') = node THEN
		RETURN (p->>field)::float8;
	END IF;
	FOR c IN SELECT * FROM json_array_elements(coalesce(p->'Plans', '[]'::json))
	LOOP
		r := lsq_find(c, node, field);
		IF r IS NOT NULL THEN
			RETURN r;
		END IF;
	END LOOP;
	RETURN NULL;
END $$;
CREATE FUNCTION lsq_cost(q text, node text, field text DEFAULT 'Total Cost')
RETURNS float8
LANGUAGE plpgsql AS $$
DECLARE
	p json;
BEGIN
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO p;
	RETURN lsq_find(p->0->'Plan', node, field);
END $$;

-- The count, a call for each key of one group of a thousand (one key in a
-- hundred is of the mid-sized, the rest light); and the same count as a
-- prepared statement's generic plan, whose $1 is no outer row's, prepared
-- afresh under each section's settings.
CREATE VIEW lsq_q (q) AS VALUES
	($q$SELECT d.id, (SELECT count(*) FROM lsq_fact f WHERE f.fk = d.id AND f.doc @@ 'a1 & b3 & c1'::tsquery) AS n FROM lsq_dim d WHERE d.grp = 7$q$);

-- ---------- 1. a correlated count ----------
-- the walk off: its set of the tsquery's rows, which no Param reaches, is
-- built once for every call (section 3), and the count is the one weighed
-- here
SET pg_lion.enable_btree_scan = off;
SELECT lsq_plan(q) FROM lsq_q;
PREPARE lsq_generic(int) AS
	SELECT count(*) FROM lsq_fact f WHERE f.fk = $1 AND f.doc @@ 'a1 & b3 & c1'::tsquery;
SET plan_cache_mode = force_generic_plan;
SELECT lsq_plan('EXECUTE lsq_generic(107)');
-- the count's price at a call is a fraction of the generic plan's, which
-- pays every page of its lookups and seeks
SELECT lsq_cost(q, 'LionCount') < lsq_cost('EXECUTE lsq_generic(107)', 'LionCount') / 2
	   AS spread
  FROM lsq_q;
-- an InitPlan's Param is no loop either: the count it keys pays what the
-- generic plan's does (and the InitPlan's own price on top)
SELECT lsq_cost($$SELECT count(*) FROM lsq_fact f WHERE f.fk = (SELECT max(id) FROM lsq_dim WHERE id < 108) AND f.doc @@ 'a1 & b3 & c1'::tsquery$$, 'LionCount') >=
	   lsq_cost('EXECUTE lsq_generic(107)', 'LionCount') AS one_call;
RESET plan_cache_mode;
DEALLOCATE lsq_generic;
DO $$ BEGIN EXECUTE 'CREATE TABLE lsq_got1 AS ' || (SELECT q FROM lsq_q); END $$;
RESET pg_lion.enable_btree_scan;

-- ---------- 2. the lion index's own scan ----------
-- the count pushdown and the walk off: a call's scan of the lion index on
-- both columns, its directory leaves spread over the calls as a
-- parameterized scan's are, against the generic plan's bitmap of the key's
-- B-tree, priced for one call
SET pg_lion.enable_count_pushdown = off;
SET pg_lion.enable_btree_scan = off;
SELECT lsq_plan(q) FROM lsq_q;
PREPARE lsq_generic(int) AS
	SELECT count(*) FROM lsq_fact f WHERE f.fk = $1 AND f.doc @@ 'a1 & b3 & c1'::tsquery;
SET plan_cache_mode = force_generic_plan;
SELECT lsq_plan('EXECUTE lsq_generic(107)');
RESET plan_cache_mode;
DEALLOCATE lsq_generic;
DO $$ BEGIN EXECUTE 'CREATE TABLE lsq_got2 AS ' || (SELECT q FROM lsq_q); END $$;
RESET pg_lion.enable_count_pushdown;
RESET pg_lion.enable_btree_scan;

-- ---------- 3. the walk of the covering B-tree ----------
-- the count and the lion index's own scans off: the call's scan is the
-- walk, its set the tsquery's alone - no Param reaches it - built once for
-- every call, and its pages spread over the calls; the generic plan's is
-- core's index scan with the tsquery as its filter
SET pg_lion.enable_count_pushdown = off;
SET pg_lion.enable_plain_scan = off;
SET enable_bitmapscan = off;
SELECT lsq_plan(q) FROM lsq_q;
PREPARE lsq_generic(int) AS
	SELECT count(*) FROM lsq_fact f WHERE f.fk = $1 AND f.doc @@ 'a1 & b3 & c1'::tsquery;
SET plan_cache_mode = force_generic_plan;
SELECT lsq_plan('EXECUTE lsq_generic(107)');
RESET plan_cache_mode;
DEALLOCATE lsq_generic;
DO $$ BEGIN EXECUTE 'CREATE TABLE lsq_got3 AS ' || (SELECT q FROM lsq_q); END $$;
-- a scalar sub-select of the B-tree's included column
SELECT lsq_plan($$SELECT d.id, (SELECT max(f.id) FROM lsq_fact f WHERE f.fk = d.id AND f.doc @@ 'a1 & b3'::tsquery) FROM lsq_dim d WHERE d.grp = 7$$);
CREATE TABLE lsq_max AS
SELECT d.id, (SELECT max(f.id) FROM lsq_fact f WHERE f.fk = d.id AND f.doc @@ 'a1 & b3'::tsquery) AS m
  FROM lsq_dim d WHERE d.grp = 7;
RESET pg_lion.enable_count_pushdown;
RESET pg_lion.enable_plain_scan;
RESET enable_bitmapscan;

-- ---------- 4. EXISTS ----------
-- the outer rows a few, so that core runs the subquery for each rather than
-- hashing the whole of its answer
SELECT lsq_plan($$SELECT d.id, EXISTS (SELECT 1 FROM lsq_fact f WHERE f.fk = d.id AND f.doc @@ 'a1 & b3 & c1'::tsquery) FROM lsq_dim d WHERE d.grp = 7 AND d.id < 2000$$);
CREATE TABLE lsq_exists AS
SELECT d.id, EXISTS (SELECT 1 FROM lsq_fact f WHERE f.fk = d.id AND f.doc @@ 'a1 & b3 & c1'::tsquery) AS e
  FROM lsq_dim d WHERE d.grp = 7 AND d.id < 2000;

-- ---------- 5. the answers, against core's ----------
-- every lion scan off: core's index scan of the key's B-tree, the tsquery
-- its filter
SET pg_lion.enable_count_pushdown = off;
SET pg_lion.enable_btree_scan = off;
SET pg_lion.enable_plain_scan = off;
SET enable_bitmapscan = off;
SELECT lsq_plan(q) FROM lsq_q;
DO $$ BEGIN EXECUTE 'CREATE TABLE lsq_want AS ' || (SELECT q FROM lsq_q); END $$;
SELECT g.form, count(*) AS rows, sum(w.n) AS counted,
	   count(*) FILTER (WHERE (w.id, w.n) IS DISTINCT FROM (g.id, g.n)) AS differ
  FROM lsq_want w
  FULL JOIN (SELECT 1 AS form, * FROM lsq_got1
			 UNION ALL SELECT 2, * FROM lsq_got2
			 UNION ALL SELECT 3, * FROM lsq_got3) g
	ON g.id = w.id
 GROUP BY g.form ORDER BY g.form;
SELECT count(*) AS rows, count(*) FILTER (WHERE m IS NOT NULL) AS found,
	   count(*) FILTER (WHERE m IS DISTINCT FROM
						(SELECT max(f.id) FROM lsq_fact f
						  WHERE f.fk = x.id AND f.doc @@ 'a1 & b3'::tsquery)) AS differ
  FROM lsq_max x;
SELECT count(*) AS rows, count(*) FILTER (WHERE e) AS found,
	   count(*) FILTER (WHERE e IS DISTINCT FROM
						EXISTS (SELECT 1 FROM lsq_fact f
								 WHERE f.fk = x.id AND f.doc @@ 'a1 & b3 & c1'::tsquery)) AS differ
  FROM lsq_exists x;
RESET pg_lion.enable_count_pushdown;
RESET pg_lion.enable_btree_scan;
RESET pg_lion.enable_plain_scan;
RESET enable_bitmapscan;

-- ---------- 6. the join, where the planner sees the loop ----------
-- the same count as a join's: its paths carry no exec Param, and are
-- priced as they were
SELECT lsq_plan($$SELECT d.id, count(*) FROM lsq_dim d JOIN lsq_fact f ON f.fk = d.id WHERE d.grp = 7 AND f.doc @@ 'a1 & b3 & c1'::tsquery GROUP BY d.id$$);
SET pg_lion.enable_count_pushdown = off;
SET pg_lion.enable_plain_scan = off;
SET enable_bitmapscan = off;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
-- at core's rows a key: the key is skewed, and the rows a probed key meets
-- (skew.sql) would price the loop the other way round
SET pg_lion.enable_skew_estimate = off;
SELECT lsq_plan($$SELECT d.id, count(*) FROM lsq_dim d JOIN lsq_fact f ON f.fk = d.id WHERE d.grp = 7 AND f.doc @@ 'a1 & b3 & c1'::tsquery GROUP BY d.id$$);
SELECT count(*) AS groups, sum(n) AS counted
  FROM (SELECT d.id, count(*) AS n FROM lsq_dim d JOIN lsq_fact f ON f.fk = d.id
		 WHERE d.grp = 7 AND f.doc @@ 'a1 & b3 & c1'::tsquery GROUP BY d.id) j;
RESET pg_lion.enable_count_pushdown;
RESET pg_lion.enable_plain_scan;
RESET enable_bitmapscan;
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET pg_lion.enable_skew_estimate;

-- ---------- 7. an InitPlan's Param in the Lion Cond ----------
-- the lion index on the tsvector alone, so that the outer row's key is the
-- B-tree's alone and the Lion Cond holds the tsquery an InitPlan computes:
-- set once for every call, it rebuilds nothing, and the set's build is
-- spread over the calls - a call's share of it next to nothing beside the
-- call's walk.  The margin keeps the walk in the plan, a choice close to
-- core's index scan on these tables, and divides both costs alike.
CREATE INDEX lsq_fact_doc ON lsq_fact USING lion (doc);
DROP INDEX lsq_fact_lion;
SET pg_lion.enable_count_pushdown = off;
SET pg_lion.enable_plain_scan = off;
SET enable_bitmapscan = off;
SET pg_lion.pushdown_margin = 4;
SELECT lsq_cost(q, 'LionBtreeScan', 'Startup Cost') * 100 <
	   lsq_cost(q, 'LionBtreeScan') AS built_once
  FROM (VALUES ($q$SELECT d.id, (SELECT count(*) FROM lsq_fact f WHERE f.fk = d.id AND f.doc @@ (SELECT 'a1 & b3 & c1'::tsquery)) FROM lsq_dim d WHERE d.grp = 7$q$)) v (q);
SELECT count(*) AS rows, sum(n) AS counted
  FROM (SELECT d.id, (SELECT count(*) FROM lsq_fact f WHERE f.fk = d.id AND f.doc @@ (SELECT 'a1 & b3 & c1'::tsquery)) AS n
		  FROM lsq_dim d WHERE d.grp = 7) g;
RESET pg_lion.enable_count_pushdown;
RESET pg_lion.enable_plain_scan;
RESET enable_bitmapscan;
RESET pg_lion.pushdown_margin;

DROP TABLE lsq_want, lsq_got1, lsq_got2, lsq_got3, lsq_max, lsq_exists;
DROP VIEW lsq_q;
DROP TABLE lsq_fact, lsq_dim;
DROP FUNCTION lsq_plan(text), lsq_cost(text, text, text), lsq_names(json),
	lsq_find(json, text, text);
