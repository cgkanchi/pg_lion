-- An ordered walk across a key join: the LionOrderedJoin CustomScan
-- (DESIGN.md §47).
--
-- SELECT f.<cols>, d.<cols> FROM fact f JOIN dim d ON f.fk = d.pk
-- WHERE <fact quals> AND <dim quals> ORDER BY <a btree of fact> LIMIT n: the
-- node walks fact's btree in order over fact's lion set, descends dim's
-- primary key for each member's key, tests the TID it finds against dim's
-- own lion set before it reads dim's heap, and returns the two rows joined,
-- in the walk's order.  Every query below is run through the node (lion's
-- paths priced at a thousandth, so that the join is the node wherever it may
-- be) and through the ordinary plan (the node off, nothing else changed), and
-- the two answers are compared row by row in order: every ORDER BY ends in a
-- unique key.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;
SET default_statistics_target = 1000;
SET work_mem = '64MB';

CREATE TABLE dim (
	pk int PRIMARY KEY,
	kind int NOT NULL,			-- lion: ten values
	flag bool NOT NULL,			-- lion: two thirds true
	tags text[] NOT NULL,		-- lion: one of ten t's and one of seven u's
	lvl int NOT NULL,			-- lion: a hundred values
	payload text				-- wide: the heap is many times the btree
) WITH (autovacuum_enabled = off);
INSERT INTO dim
SELECT i,
	   ((hashint8extended(i::bigint, 1) & 9223372036854775807) % 10)::int,
	   i % 3 <> 0,
	   ARRAY['t' || (i % 10), 'u' || (i % 7)],
	   ((hashint8extended(i::bigint, 2) & 9223372036854775807) % 100)::int,
	   repeat(md5(i::text), 4)
  FROM generate_series(1, 100000) i;
CREATE INDEX dim_kind ON dim USING lion (kind);
CREATE INDEX dim_flag ON dim USING lion (flag);
CREATE INDEX dim_tags ON dim USING lion (tags);
CREATE INDEX dim_lvl ON dim USING lion (lvl);

CREATE TABLE fact (
	id int PRIMARY KEY,
	fk int,						-- a dim row, NULL in every 101st row
	grp int NOT NULL,			-- five groups: the btree's leader
	ver int NOT NULL,			-- indexed by nothing: a filter
	score numeric,				-- the order; NULL in every 89th row
	tags text[] NOT NULL,		-- lion: one of four a's and one of nine b's
	note text
) WITH (autovacuum_enabled = off);
INSERT INTO fact
SELECT i,
	   CASE WHEN i % 101 = 0 THEN NULL
			ELSE 1 + ((hashint8extended(i::bigint, 3) & 9223372036854775807) % 100000)::int END,
	   i % 5,
	   i % 3,
	   CASE WHEN i % 89 = 0 THEN NULL
			ELSE ((hashint8extended(i::bigint, 4) & 9223372036854775807) % 1000000)::numeric / 100 END,
	   ARRAY['a' || (i % 4), 'b' || (i % 9)],
	   'n'
  FROM generate_series(1, 500000) i;
CREATE INDEX fact_grp_score ON fact (grp, score DESC NULLS LAST) INCLUDE (fk);
CREATE INDEX fact_tags ON fact USING lion (tags);
VACUUM (FREEZE, ANALYZE) dim;
VACUUM (FREEZE, ANALYZE) fact;

/*
 * oj() runs q through the node - lion's paths priced at a thousandth - and
 * through the ordinary plan (the node off), compares the two answers in
 * order, and says whether the first plan was the node.  A prepared
 * statement's cached plan does not change with the settings, so for one the
 * ordinary answer comes from the query it stands for, given as `ordinary`.
 */
CREATE FUNCTION oj(q text, ordinary text DEFAULT NULL) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	r record;
	used boolean := false;
	a text[] := '{}';
	b text[] := '{}';
BEGIN
	PERFORM set_config('pg_lion.pushdown_margin', '1000', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(LionOrderedJoin\)' THEN
			used := true;
		END IF;
	END LOOP;
	FOR r IN EXECUTE q LOOP
		a := a || r::text;
	END LOOP;

	PERFORM set_config('pg_lion.pushdown_margin', '1', true);
	PERFORM set_config('pg_lion.enable_ordered_join', 'off', true);
	FOR r IN EXECUTE coalesce(ordinary, q) LOOP
		b := b || r::text;
	END LOOP;
	PERFORM set_config('pg_lion.enable_ordered_join', 'on', true);

	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH: %s rows through the node, %s ordinary; first %s / %s',
					  coalesce(array_length(a, 1), 0), coalesce(array_length(b, 1), 0),
					  a[1], b[1]);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN used THEN 'LionOrderedJoin' ELSE 'no LionOrderedJoin' END,
				  coalesce(array_length(a, 1), 0));
END $$;

/*
 * oj_plan() prints the plan with nothing forced (the plan choice is the
 * test), without the lines whose contents vary between majors.
 */
CREATE FUNCTION oj_plan(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln !~ '^\s*(Index Searches|Disabled|Storage|Planning|Execution|Buffers)' THEN
			RETURN NEXT ln;
		END IF;
	END LOOP;
END $$;

/*
 * oj_run() runs q through the node under EXPLAIN ANALYZE and prints the
 * plan's lines without the timing, the memory and what varies between
 * majors.
 */
CREATE FUNCTION oj_run(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('pg_lion.pushdown_margin', '1000', true);
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF ln !~ '^\s*(Index Searches|Disabled|Storage|Planning|Execution|Buffers|Full-sort Groups|Pre-sorted Groups|Sort Method)' THEN
			RETURN NEXT regexp_replace(ln, '\s*\(actual.*\)', '');
		END IF;
	END LOOP;
	PERFORM set_config('pg_lion.pushdown_margin', '1', true);
END $$;

-- 1. The plan: a page the walk fills early is the node's - fact's btree
--    walked over fact's set, dim's key descended to and tested against dim's
--    set before its heap is read - and the rows are the ordinary plan's.
SELECT * FROM oj_plan('SELECT f.id, f.score, d.pk, d.lvl FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = 3 AND d.flag
	ORDER BY f.score DESC NULLS LAST LIMIT 20');
SELECT oj('SELECT f.id, f.score, d.pk, d.lvl, d.payload FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = 3 AND d.flag
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');

-- ... and a page whose rows are rare, the walk long and the dimension's
--    rows few, is core's: its rows found from both sets, joined and sorted.
SELECT * FROM oj_plan('SELECT f.id, f.score, d.pk, d.lvl FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = 3 AND d.kind = 3
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
SELECT oj('SELECT f.id, f.score, d.pk, d.lvl FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = 3 AND d.kind = 3
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');

-- 2. The walk either way, NULLs where the order puts them: forward for
--    DESC NULLS LAST, backward for ASC NULLS FIRST, which meets the NULL
--    scores first.
SELECT oj('SELECT f.id, f.score, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 3 AND f.tags @> ''{b4}'' AND d.lvl < 10 AND d.flag
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 30');
SELECT oj('SELECT f.id, f.score, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 3 AND f.tags @> ''{b4}'' AND d.lvl < 10 AND d.flag
	ORDER BY f.score NULLS FIRST, f.id DESC LIMIT 30');
SELECT * FROM oj_run('SELECT f.id, f.score, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 3 AND f.tags @> ''{b4}'' AND d.lvl < 10 AND d.flag
	ORDER BY f.score NULLS FIRST, f.id DESC LIMIT 30');

-- 3. OFFSET: the rows skipped are walked and joined as the rows returned.
SELECT oj('SELECT f.id, f.score, d.pk, d.kind FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 1 AND f.tags @> ''{a3}'' AND d.lvl IN (5, 6) AND d.flag
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 10 OFFSET 40');

-- 4. A side with no lion clause: the fact's walk then fetches every row it
--    meets, the dimension's probe every row it finds; the other side's set
--    does the filtering.
SELECT oj('SELECT f.id, f.score, d.pk, d.lvl FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND d.lvl = 3 AND d.flag
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
SELECT oj('SELECT f.id, f.score, d.pk, d.lvl FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND f.tags @> ''{b2}'' AND d.payload LIKE ''a%''
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
SELECT * FROM oj_run('SELECT f.id, f.score, d.pk, d.lvl FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND d.lvl = 3 AND d.flag
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');

-- 5. An inexact dimension set: its members' rows rechecked against its qual.
SELECT oj('SELECT f.id, f.score, d.pk, d.tags FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.tags <@ ''{t3,u5,t4}''
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
SELECT * FROM oj_run('SELECT f.id, f.score, d.pk, d.tags FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.tags <@ ''{t3,u5,t4}''
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');

-- 6. A filter on the fact: the walk's members' rows tested after the fetch.
SELECT oj('SELECT f.id, f.score, f.ver, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 4 AND f.tags @> ''{a0}'' AND f.ver = 1 AND d.kind = 7 AND NOT d.flag
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
SELECT * FROM oj_run('SELECT f.id, f.score, f.ver, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 4 AND f.tags @> ''{a0}'' AND f.ver = 1 AND d.kind = 7 AND NOT d.flag
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');

-- 7. Core's nodes above it: a trailing ORDER BY key of the dimension's, which
--    an Incremental Sort sorts the walk's groups by; a LEFT JOIN, and a LEFT
--    JOIN ... IS NULL, to a third table.
CREATE TABLE flt (pk int PRIMARY KEY, x int NOT NULL);
INSERT INTO flt SELECT i, i % 10 FROM generate_series(1, 100000, 3) i;
VACUUM (FREEZE, ANALYZE) flt;
SELECT oj('SELECT f.id, f.score, d.pk, d.lvl FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = 3 AND d.flag
	ORDER BY f.score DESC NULLS LAST, d.pk, f.id LIMIT 20');
SELECT oj('SELECT f.id, f.score, d.pk, x.x FROM fact f JOIN dim d ON f.fk = d.pk
	LEFT JOIN flt x ON x.pk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = 3 AND d.flag
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
SELECT oj('SELECT f.id, f.score, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
	LEFT JOIN flt x ON x.pk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = 3 AND d.flag AND x.pk IS NULL
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
SET pg_lion.pushdown_margin = 1000;
EXPLAIN (COSTS OFF) SELECT f.id, f.score, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
	LEFT JOIN flt x ON x.pk = d.pk
	WHERE f.grp = 2 AND f.tags @> '{a1}' AND d.lvl = 3 AND d.flag AND x.pk IS NULL
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20;
RESET pg_lion.pushdown_margin;

-- 8. A Param in a lion qual: a generic plan's, the set built from the value
--    each EXECUTE brings; and a rescan's, where the dimension's set is
--    rebuilt each time the outer row sets the value it depends on, and the
--    fact's, which depends on none, is built once.
SET plan_cache_mode = force_generic_plan;
SET pg_lion.pushdown_margin = 1000;
PREPARE oj_p(int) AS SELECT f.id, f.score, d.pk, d.lvl FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> '{a1}' AND d.lvl = $1 AND d.flag
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20;
EXPLAIN (COSTS OFF) EXECUTE oj_p(3);
SELECT oj('EXECUTE oj_p(3)', 'SELECT f.id, f.score, d.pk, d.lvl FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = 3 AND d.flag
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
SELECT oj('EXECUTE oj_p(40)', 'SELECT f.id, f.score, d.pk, d.lvl FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = 40 AND d.flag
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
DEALLOCATE oj_p;
RESET pg_lion.pushdown_margin;
RESET plan_cache_mode;
SELECT oj('SELECT v.l, s.* FROM (VALUES (3), (40), (40), (3)) v(l),
	LATERAL (SELECT f.id, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
			 WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = v.l AND d.flag
			 ORDER BY f.score DESC NULLS LAST, f.id LIMIT 5) s');
SELECT * FROM oj_run('SELECT v.l, s.* FROM (VALUES (3), (40), (40), (3)) v(l),
	LATERAL (SELECT f.id, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
			 WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = v.l AND d.flag
			 ORDER BY f.score DESC NULLS LAST, f.id LIMIT 5) s');

-- 9. What it declines, for core to plan: a join on more than the key, a
--    key that is not unique, rows locked, no lion clause on either side,
--    and the setting off.
SELECT oj('SELECT f.id, d.pk FROM fact f JOIN dim d ON f.fk = d.pk AND f.ver < d.kind
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = 3
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
SELECT oj('SELECT f.id, d.pk FROM fact f JOIN dim d ON f.fk = d.lvl
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.kind = 3
	ORDER BY f.score DESC NULLS LAST, f.id, d.pk LIMIT 20');
SELECT oj('SELECT f.id, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = 3
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20 FOR SHARE OF d');
SELECT oj('SELECT f.id, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND d.payload LIKE ''a%''
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
SET pg_lion.enable_ordered_join = off;
SELECT * FROM oj_plan('SELECT f.id, f.score, d.pk, d.lvl FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = 3 AND d.flag
	ORDER BY f.score DESC NULLS LAST LIMIT 20');
RESET pg_lion.enable_ordered_join;

DROP FUNCTION oj(text, text);
DROP FUNCTION oj_plan(text);
DROP FUNCTION oj_run(text);
DROP TABLE fact, dim, flt;
