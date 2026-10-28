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
 * order, and says whether the first plan was the node, or its split
 * (section 13).  A prepared
 * statement's cached plan does not change with the settings, so for one the
 * ordinary answer comes from the query it stands for, given as `ordinary`.
 */
CREATE FUNCTION oj(q text, ordinary text DEFAULT NULL) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	r record;
	used boolean := false;
	split boolean := false;
	keys text := '';
	a text[] := '{}';
	b text[] := '{}';
BEGIN
	PERFORM set_config('pg_lion.pushdown_margin', '1000', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(LionOrderedJoin\)' THEN
			used := true;
		END IF;
		IF ln ~ 'Custom Scan \(LionOrderedSplit\)' THEN
			split := true;
		END IF;
		IF ln ~ '^\s*Key Sets: ' THEN
			keys := format(' (key sets: %s)', regexp_replace(ln, '^\s*Key Sets: ', ''));
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
				  CASE WHEN split THEN 'LionOrderedSplit' || keys
					   WHEN used THEN 'LionOrderedJoin' || keys
					   ELSE 'no LionOrderedJoin' END,
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
		IF ln !~ '^\s*(Index Searches|Disabled|Storage|Planning|Execution|Buffers|Full-sort Groups|Pre-sorted Groups|Sort Method|Fetch and Sort Space Used)' THEN
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

-- ... and a page core's nested loop takes while the tables fit in
--     effective_cache_size, the node's once they do not: the loop's fact heap
--     pages, and the dimension leaf and heap page of each key it looks up,
--     are then read cold (§39), as are the node's own reads, which are fewer.
SELECT * FROM oj_plan('SELECT f.id, f.score, d.pk, d.lvl FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl < 30
	ORDER BY f.score DESC NULLS LAST LIMIT 20');
SET effective_cache_size = '1MB';
SELECT * FROM oj_plan('SELECT f.id, f.score, d.pk, d.lvl FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl < 30
	ORDER BY f.score DESC NULLS LAST LIMIT 20');
RESET effective_cache_size;

-- ... and a page core's nested loop over a Memoize takes while the Memoize is
--     priced at core's hit ratio over the loop's whole walk, the node's once
--     it is priced at the hits the LIMIT's share of its calls meets (§39, "A
--     Memoize under a LIMIT").
SET pg_lion.enable_memoize_limit = off;
SELECT * FROM oj_plan('SELECT f.id, f.score, d.pk, d.lvl FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND d.lvl = 3
	ORDER BY f.score DESC NULLS LAST LIMIT 20');
RESET pg_lion.enable_memoize_limit;
SELECT * FROM oj_plan('SELECT f.id, f.score, d.pk, d.lvl FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND d.lvl = 3
	ORDER BY f.score DESC NULLS LAST LIMIT 20');

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
-- ... and with the left join above, the LIMIT's share of the walk still
--     decides the dimension's set: it starts lazy, its keys probed one by
--     one, and is built once those probes have cost what the build would.
SELECT * FROM oj_run('SELECT f.id, f.score, d.pk, x.x FROM fact f JOIN dim d ON f.fk = d.pk
	LEFT JOIN flt x ON x.pk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = 3 AND d.flag
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 5');

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

-- 9. Key sets: a table that only filters the join's key - by IN or EXISTS,
--    or by an inner join on a unique column of it with nothing of it wanted -
--    is read into a hash set of its keys before the walk, and each fact row's
--    key is tested against the sets before the dimension's btree is
--    descended.  fltn's keys repeat, each three times.
CREATE TABLE fltn (k int NOT NULL);
INSERT INTO fltn SELECT (i % 3000) * 3 + 1 FROM generate_series(1, 9000) i;
VACUUM (FREEZE, ANALYZE) fltn;
SELECT * FROM oj_run('SELECT f.id, f.score, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl < 10 AND d.flag
	  AND f.fk IN (SELECT pk FROM flt WHERE x < 3) AND d.pk IN (SELECT k FROM fltn)
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
SELECT oj('SELECT f.id, f.score, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = 3 AND d.flag
	  AND f.fk IN (SELECT pk FROM flt WHERE x < 3)
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
SELECT oj('SELECT f.id, f.score, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = 3 AND d.flag
	  AND EXISTS (SELECT 1 FROM flt x WHERE x.pk = d.pk AND x.x = 2)
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
SELECT oj('SELECT f.id, f.score, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
	JOIN flt x ON x.pk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = 3 AND d.flag AND x.x = 2
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
SELECT oj('SELECT f.id, f.score, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND d.lvl = 3 AND f.fk IN (SELECT pk FROM flt WHERE x = 1)
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');

-- ... an empty key set, which ends the scan before the walk starts.
SELECT oj('SELECT f.id, f.score, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = 3 AND d.flag
	  AND f.fk IN (SELECT pk FROM flt WHERE x = 99)
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');

-- ... a key set whose plan depends on an outer row's value: rebuilt for each
--     value, while the dimension's set, which does not depend on it, is
--     built once.
SELECT oj('SELECT v.l, s.* FROM (VALUES (1), (2), (2), (1)) v(l),
	LATERAL (SELECT f.id, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
			 WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = 3 AND d.flag
			   AND f.fk IN (SELECT pk FROM flt WHERE x = v.l)
			 ORDER BY f.score DESC NULLS LAST, f.id LIMIT 5) s');
SELECT * FROM oj_run('SELECT v.l, s.* FROM (VALUES (1), (2), (2), (1)) v(l),
	LATERAL (SELECT f.id, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
			 WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = 3 AND d.flag
			   AND f.fk IN (SELECT pk FROM flt WHERE x = v.l)
			 ORDER BY f.score DESC NULLS LAST, f.id LIMIT 5) s');

-- ... a semi join's keys repeated, held once each; and what is not a key
--     set: an inner join to a column that is not unique, which repeats the
--     rows; a table a column of which the query wants; a NOT EXISTS, which
--     stays an anti join above the node; and keys that would not fit in
--     work_mem.
SELECT oj('SELECT f.id, f.score, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl < 10 AND d.flag
	  AND f.fk IN (SELECT k FROM fltn)
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
SELECT oj('SELECT f.id, f.score, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
	JOIN fltn n ON n.k = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl < 10 AND d.flag
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
SELECT oj('SELECT f.id, f.score, d.pk, x.x FROM fact f JOIN dim d ON f.fk = d.pk
	JOIN flt x ON x.pk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = 3 AND d.flag AND x.x = 2
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
SELECT oj('SELECT f.id, f.score, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = 3 AND d.flag
	  AND NOT EXISTS (SELECT 1 FROM flt x WHERE x.pk = f.fk AND x.x = 2)
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
SET work_mem = '64kB';
SELECT oj('SELECT f.id, f.score, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = 3 AND d.flag
	  AND f.fk IN (SELECT pk FROM flt WHERE x < 9)
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
RESET work_mem;

-- 9b. A key set joined above the node (DESIGN.md §47.6, "A join above"):
--     past join_collapse_limit core plans the fact and the
--     dimension in a part of their own and joins the EXISTS or the IN above
--     it.  Its table is a key set all the same, and the semi join above
--     tests again the rows the node returns.  At a join_collapse_limit of 1
--     the explicit JOIN is such a part; at 8 a query of more tables than
--     that; at 16 the same query is planned whole and the table is a key set
--     inside the join, as in 9.  Every answer is the ordinary plan's.
SET join_collapse_limit = 1;
SELECT * FROM oj_run('SELECT f.id, f.score, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = 3 AND d.flag
	  AND EXISTS (SELECT 1 FROM flt x WHERE x.pk = d.pk AND x.x = 2)
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
SELECT oj('SELECT f.id, f.score, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = 3 AND d.flag
	  AND EXISTS (SELECT 1 FROM flt x WHERE x.pk = d.pk AND x.x = 2)
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
SELECT oj('SELECT f.id, f.score, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl < 10 AND d.flag
	  AND f.fk IN (SELECT pk FROM flt WHERE x < 3)
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
-- ... two of them, and an IN whose keys repeat
SELECT oj('SELECT f.id, f.score, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl < 10 AND d.flag
	  AND f.fk IN (SELECT pk FROM flt WHERE x < 3) AND d.pk IN (SELECT k FROM fltn)
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
-- ... an empty one
SELECT oj('SELECT f.id, f.score, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = 3 AND d.flag
	  AND EXISTS (SELECT 1 FROM flt x WHERE x.pk = f.fk AND x.x = 99)
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
-- ... and what is not one: a table whose clauses call a volatile function,
--     which the join above would call again, and a NOT EXISTS
SELECT oj('SELECT f.id, f.score, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = 3 AND d.flag
	  AND EXISTS (SELECT 1 FROM flt x WHERE x.pk = d.pk AND x.x = 2 AND random() < 2)
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
SELECT oj('SELECT f.id, f.score, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = 3 AND d.flag
	  AND NOT EXISTS (SELECT 1 FROM flt x WHERE x.pk = f.fk AND x.x = 2)
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
RESET join_collapse_limit;
-- ... nine tables: four left joins, an anti join and a LATERAL subquery
--     besides the fact, the dimension and the EXISTS, at the default limits,
--     at 16, and under GEQO.  flt's key is unique, and core makes the EXISTS
--     an inner join; fltn's repeats, and stays a semi join; and an EXISTS
--     that tests more than the key, which is no key set.
SELECT oj('SELECT f.id, f.score, d.pk, a.x, b.x, c.x, e.x, l.pk
	FROM fact f JOIN dim d ON f.fk = d.pk
	LEFT JOIN flt a ON a.pk = d.pk
	LEFT JOIN flt b ON b.pk = d.pk + 1
	LEFT JOIN flt c ON c.pk = d.pk + 2
	LEFT JOIN flt e ON e.pk = d.pk + 3
	LEFT JOIN LATERAL (SELECT pk FROM flt z WHERE z.pk >= d.pk ORDER BY z.pk LIMIT 1) l ON true
	LEFT JOIN fltn n ON n.k = d.pk
	WHERE f.grp = 2 AND d.lvl < 10 AND d.flag AND n.k IS NULL
	  AND EXISTS (SELECT 1 FROM flt x WHERE x.pk = d.pk AND x.x < 3)
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
-- (fltn read twice: the key set's plan under the node, and the semi join's
-- above it)
SELECT regexp_replace(l, '^\s*(->\s*)?', '') FROM oj_run('SELECT f.id, f.score, d.pk, a.x, b.x, c.x, e.x, l.pk
	FROM fact f JOIN dim d ON f.fk = d.pk
	LEFT JOIN flt a ON a.pk = d.pk
	LEFT JOIN flt b ON b.pk = d.pk + 1
	LEFT JOIN flt c ON c.pk = d.pk + 2
	LEFT JOIN flt e ON e.pk = d.pk + 3
	LEFT JOIN LATERAL (SELECT pk FROM flt z WHERE z.pk >= d.pk ORDER BY z.pk LIMIT 1) l ON true
	LEFT JOIN flt n ON n.pk = d.pk + 4
	WHERE f.grp = 2 AND d.lvl < 10 AND d.flag AND n.pk IS NULL
	  AND EXISTS (SELECT 1 FROM fltn y WHERE y.k = d.pk)
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20') l
	WHERE l ~ '^\s*(->\s*)?(Custom Scan \(LionOrderedJoin\)|Key Sets:|Seq Scan on fltn)';
SELECT oj('SELECT f.id, f.score, d.pk, a.x, b.x, c.x, e.x, l.pk
	FROM fact f JOIN dim d ON f.fk = d.pk
	LEFT JOIN flt a ON a.pk = d.pk
	LEFT JOIN flt b ON b.pk = d.pk + 1
	LEFT JOIN flt c ON c.pk = d.pk + 2
	LEFT JOIN flt e ON e.pk = d.pk + 3
	LEFT JOIN LATERAL (SELECT pk FROM flt z WHERE z.pk >= d.pk ORDER BY z.pk LIMIT 1) l ON true
	LEFT JOIN flt n ON n.pk = d.pk + 4
	WHERE f.grp = 2 AND d.lvl < 10 AND d.flag AND n.pk IS NULL
	  AND EXISTS (SELECT 1 FROM fltn y WHERE y.k = d.pk)
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
SELECT oj('SELECT f.id, f.score, d.pk, a.x, b.x, c.x, e.x, l.pk
	FROM fact f JOIN dim d ON f.fk = d.pk
	LEFT JOIN flt a ON a.pk = d.pk
	LEFT JOIN flt b ON b.pk = d.pk + 1
	LEFT JOIN flt c ON c.pk = d.pk + 2
	LEFT JOIN flt e ON e.pk = d.pk + 3
	LEFT JOIN LATERAL (SELECT pk FROM flt z WHERE z.pk >= d.pk ORDER BY z.pk LIMIT 1) l ON true
	LEFT JOIN fltn n ON n.k = d.pk
	WHERE f.grp = 2 AND d.lvl < 10 AND d.flag AND n.k IS NULL
	  AND EXISTS (SELECT 1 FROM flt x WHERE x.pk = d.pk AND x.x < d.kind)
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
SET join_collapse_limit = 16;
SET from_collapse_limit = 16;
SELECT oj('SELECT f.id, f.score, d.pk, a.x, b.x, c.x, e.x, l.pk
	FROM fact f JOIN dim d ON f.fk = d.pk
	LEFT JOIN flt a ON a.pk = d.pk
	LEFT JOIN flt b ON b.pk = d.pk + 1
	LEFT JOIN flt c ON c.pk = d.pk + 2
	LEFT JOIN flt e ON e.pk = d.pk + 3
	LEFT JOIN LATERAL (SELECT pk FROM flt z WHERE z.pk >= d.pk ORDER BY z.pk LIMIT 1) l ON true
	LEFT JOIN fltn n ON n.k = d.pk
	WHERE f.grp = 2 AND d.lvl < 10 AND d.flag AND n.k IS NULL
	  AND EXISTS (SELECT 1 FROM flt x WHERE x.pk = d.pk AND x.x < 3)
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
RESET join_collapse_limit;
RESET from_collapse_limit;
SET geqo_threshold = 2;
SELECT oj('SELECT f.id, f.score, d.pk, a.x, b.x, c.x, e.x, l.pk
	FROM fact f JOIN dim d ON f.fk = d.pk
	LEFT JOIN flt a ON a.pk = d.pk
	LEFT JOIN flt b ON b.pk = d.pk + 1
	LEFT JOIN flt c ON c.pk = d.pk + 2
	LEFT JOIN flt e ON e.pk = d.pk + 3
	LEFT JOIN LATERAL (SELECT pk FROM flt z WHERE z.pk >= d.pk ORDER BY z.pk LIMIT 1) l ON true
	LEFT JOIN fltn n ON n.k = d.pk
	WHERE f.grp = 2 AND d.lvl < 10 AND d.flag AND n.k IS NULL
	  AND EXISTS (SELECT 1 FROM flt x WHERE x.pk = d.pk AND x.x < 3)
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
RESET geqo_threshold;
-- ... the fact and the dimension on the nullable side of a left join: no
--     key set from above, whose drops a null-extended row would replace
SET join_collapse_limit = 1;
SELECT oj('SELECT y.pk, s.id, s.score FROM flt y LEFT JOIN
	(SELECT f.id, f.score, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
	 WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = 3 AND d.flag) s ON s.pk = y.pk
	WHERE y.x = 2 AND EXISTS (SELECT 1 FROM flt x WHERE x.pk = y.pk AND x.x = 2)
	ORDER BY y.pk, s.score DESC NULLS LAST, s.id LIMIT 20');
RESET join_collapse_limit;

-- 10. The fact's walk index-only, where its btree has every column of the
--     fact the join needs: no heap read for a member on an all-visible page;
--     with no set, every entry a member; and with an inexact set, each
--     member read for its recheck, the btree lacking the column.
SELECT oj('SELECT f.score, f.fk, d.pk, d.lvl FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = 3 AND d.flag
	ORDER BY f.score DESC NULLS LAST, d.pk LIMIT 20');
SELECT * FROM oj_run('SELECT f.score, f.fk, d.pk, d.lvl FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = 3 AND d.flag
	ORDER BY f.score DESC NULLS LAST, d.pk LIMIT 20');
SELECT oj('SELECT f.score, d.pk, d.kind FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 3 AND d.lvl = 7 AND d.flag
	ORDER BY f.score DESC NULLS LAST, d.pk LIMIT 20');
SELECT * FROM oj_run('SELECT f.score, d.pk, d.kind FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 3 AND d.lvl = 7 AND d.flag
	ORDER BY f.score DESC NULLS LAST, d.pk LIMIT 20');
SELECT oj('SELECT f.score, d.pk, d.kind FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags <@ ''{a1,b2,b5}'' AND d.lvl < 50 AND d.flag
	ORDER BY f.score DESC NULLS LAST, d.pk LIMIT 20');
SELECT * FROM oj_run('SELECT f.score, d.pk, d.kind FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags <@ ''{a1,b2,b5}'' AND d.lvl < 50 AND d.flag
	ORDER BY f.score DESC NULLS LAST, d.pk LIMIT 20');

-- ... and the fact's fetch-and-sort switch (§30.4), made due at once: the
--     rest of the walk's members fetched and sorted, the join reading them
--     as it read the walk.
SET pg_lion.ordered_switch_ratio = 1;
SELECT oj('SELECT f.id, f.score, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND f.tags @> ''{b2}'' AND d.lvl = 3 AND d.kind = 3
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
SELECT * FROM oj_run('SELECT f.id, f.score, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND f.tags @> ''{b2}'' AND d.lvl = 3 AND d.kind = 3
	ORDER BY f.score DESC NULLS LAST, f.id LIMIT 20');
RESET pg_lion.ordered_switch_ratio;

-- 11. What it declines, for core to plan: a join on more than the key, a
--     key that is not unique, rows locked, no lion clause on either side,
--     and the setting off.
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

-- 12. The rows out where the fact's key is not a foreign key (§47.3, "The
--     rows"): orph's 200,000 keys are twice the dimension's, so half of
--     them name no dimension row and cost a descent that returns nothing.
--     The node's estimate of the join is its descents' found share, as
--     core's is; under a key set, whose keys are the dimension's, that
--     share is taken once.  oj_rows() gives the node's estimate, lion's
--     paths priced at a thousandth, against the rows of the count: about
--     them (0.7 to 1.4 times), about twice them (1.6 to 2.4, as every
--     non-NULL key taken to name a row would make it), or the ratio.
CREATE FUNCTION oj_rows(q text, actual text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	p jsonb;
	n numeric;
	r numeric;
BEGIN
	PERFORM set_config('pg_lion.pushdown_margin', '1000', true);
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO p;
	PERFORM set_config('pg_lion.pushdown_margin', '1', true);
	p := p->0->'Plan'->'Plans'->0;
	IF p->>'Custom Plan Provider' IS DISTINCT FROM 'LionOrderedJoin' THEN
		RETURN 'no LionOrderedJoin';
	END IF;
	EXECUTE actual INTO n;
	r := (p->>'Plan Rows')::numeric / n;
	RETURN 'LionOrderedJoin, ' ||
		CASE WHEN r BETWEEN 0.7 AND 1.4 THEN 'about the rows'
			 WHEN r BETWEEN 1.6 AND 2.4 THEN 'about twice the rows'
			 ELSE format('%s times the rows', round(r, 1)) END;
END $$;
CREATE TABLE orph (id int PRIMARY KEY, fk int NOT NULL, score int NOT NULL)
	WITH (autovacuum_enabled = off);
INSERT INTO orph
SELECT i, i, ((hashint8extended(i::bigint, 5) & 9223372036854775807) % 1000000)::int
  FROM generate_series(1, 200000) i;
CREATE INDEX orph_score ON orph (score);
VACUUM (FREEZE, ANALYZE) orph;
SELECT oj_rows('SELECT o.id, d.pk FROM orph o JOIN dim d ON o.fk = d.pk
	WHERE d.lvl = 3 ORDER BY o.score LIMIT 20',
	'SELECT count(*) FROM orph o JOIN dim d ON o.fk = d.pk WHERE d.lvl = 3');
SELECT oj_rows('SELECT o.id, d.pk FROM orph o JOIN dim d ON o.fk = d.pk
	WHERE d.lvl = 3 AND o.fk IN (SELECT pk FROM flt WHERE x < 3)
	ORDER BY o.score LIMIT 20',
	'SELECT count(*) FROM orph o JOIN dim d ON o.fk = d.pk
	WHERE d.lvl = 3 AND o.fk IN (SELECT pk FROM flt WHERE x < 3)');
DROP FUNCTION oj_rows(text, text);
DROP TABLE orph;

-- 13. LionOrderedSplit (DESIGN.md §48): an ORDER BY that leads with a column
--     of the dimension, which a lion index of it walks in that order, and
--     goes on with the fact's walk.  The join runs an arm at a time: one for
--     each entry of the column, in the ORDER BY's order, the dimension's set
--     narrowed to the entry's rows and the fact walked from its start; an
--     Incremental Sort above sorts what the ORDER BY has past the walk.
--     d.flag leading, descending and ascending: a page the first arm fills
--     is the split's, nothing forced, and so are pages past it, OFFSET into
--     the second arm and the whole join without a LIMIT.
SELECT * FROM oj_plan('SELECT f.id, f.score, d.pk, d.flag FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl < 30
	ORDER BY d.flag DESC, f.score DESC NULLS LAST, f.id LIMIT 20');
SELECT oj('SELECT f.id, f.score, d.pk, d.flag FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl < 30
	ORDER BY d.flag DESC, f.score DESC NULLS LAST, f.id LIMIT 20');
SELECT oj('SELECT f.id, f.score, d.pk, d.flag FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl < 30
	ORDER BY d.flag, f.score DESC NULLS LAST, f.id LIMIT 20');
SELECT oj('SELECT f.id, f.score, d.pk, d.flag FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl < 30
	ORDER BY d.flag, f.score DESC NULLS LAST, f.id LIMIT 30 OFFSET 2500');
SELECT oj('SELECT f.id, f.score, d.pk, d.flag FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl < 30
	ORDER BY d.flag DESC, f.score DESC NULLS LAST, f.id');
SELECT * FROM oj_run('SELECT f.id, f.score, d.pk, d.flag FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl < 30
	ORDER BY d.flag, f.score DESC NULLS LAST, f.id LIMIT 30 OFFSET 2500');

-- ... a trailing d.lvl, sorted by the Incremental Sort above the split
SET pg_lion.pushdown_margin = 1000;
EXPLAIN (COSTS OFF) SELECT f.id, f.score, d.pk, d.flag, d.lvl FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> '{a1}' AND d.lvl < 30
	ORDER BY d.flag DESC, f.score DESC NULLS LAST, d.lvl, f.id LIMIT 20;
RESET pg_lion.pushdown_margin;
SELECT oj('SELECT f.id, f.score, d.pk, d.flag, d.lvl FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl < 30
	ORDER BY d.flag DESC, f.score DESC NULLS LAST, d.lvl, f.id LIMIT 20');

-- ... d.kind leading: ten values, more arms than pg_lion.ordered_split_max
--     plans for, until it is raised; an entry with no row of the dimension's
--     set - d.kind IN (2, 7) leaves eight - is passed over, its arm not run.
SELECT oj('SELECT f.id, f.score, d.pk, d.kind FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl < 30
	ORDER BY d.kind DESC, f.score DESC NULLS LAST, f.id LIMIT 20');
SET pg_lion.ordered_split_max = 10;
SELECT * FROM oj_plan('SELECT f.id, f.score, d.pk, d.kind FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl < 30
	ORDER BY d.kind DESC, f.score DESC NULLS LAST, f.id LIMIT 20');
SELECT oj('SELECT f.id, f.score, d.pk, d.kind FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl < 30
	ORDER BY d.kind DESC, f.score DESC NULLS LAST, f.id LIMIT 20');
SELECT oj('SELECT f.id, f.score, d.pk, d.kind FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl < 30
	ORDER BY d.kind, f.score DESC NULLS LAST, f.id LIMIT 200 OFFSET 900');
SELECT oj('SELECT f.id, f.score, d.pk, d.kind FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND d.kind IN (2, 7)
	ORDER BY d.kind DESC, f.score DESC NULLS LAST, f.id LIMIT 20 OFFSET 100');
SELECT * FROM oj_run('SELECT f.id, f.score, d.pk, d.kind FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND d.kind IN (2, 7)
	ORDER BY d.kind DESC, f.score DESC NULLS LAST, f.id LIMIT 20 OFFSET 100');
RESET pg_lion.ordered_split_max;

-- ... NULLs, on a nullable copy of the dimension: the NULL entry walked
--     first or last, as NULLS FIRST or LAST puts it, either way; not at all
--     where a clause on the column is strict.
CREATE TABLE dimn (pk int PRIMARY KEY, flag bool, kind int, lvl int NOT NULL,
	tier text) WITH (autovacuum_enabled = off);
INSERT INTO dimn
SELECT pk, CASE WHEN pk % 11 = 0 THEN NULL ELSE flag END,
	   CASE WHEN pk % 13 = 0 THEN NULL ELSE kind % 4 END, lvl,
	   CASE WHEN pk % 17 = 0 THEN NULL
			ELSE (ARRAY['gold', 'silver', 'bronze'])[1 + pk % 3] END
  FROM dim;
CREATE INDEX dimn_flag ON dimn USING lion (flag);
CREATE INDEX dimn_kind ON dimn USING lion (kind);
CREATE INDEX dimn_lvl ON dimn USING lion (lvl);
CREATE INDEX dimn_tier ON dimn USING lion (tier);
VACUUM (FREEZE, ANALYZE) dimn;
SELECT oj('SELECT f.id, f.score, d.pk, d.flag FROM fact f JOIN dimn d ON f.fk = d.pk
	WHERE f.grp = 2 AND d.lvl < 30
	ORDER BY d.flag DESC NULLS LAST, f.score DESC NULLS LAST, f.id LIMIT 20 OFFSET 3000');
SELECT oj('SELECT f.id, f.score, d.pk, d.flag FROM fact f JOIN dimn d ON f.fk = d.pk
	WHERE f.grp = 2 AND d.lvl < 30
	ORDER BY d.flag DESC NULLS FIRST, f.score DESC NULLS LAST, f.id LIMIT 20');
SELECT oj('SELECT f.id, f.score, d.pk, d.flag FROM fact f JOIN dimn d ON f.fk = d.pk
	WHERE f.grp = 2 AND d.lvl < 30
	ORDER BY d.flag NULLS FIRST, f.score DESC NULLS LAST, f.id LIMIT 20 OFFSET 1300');
SELECT oj('SELECT f.id, f.score, d.pk, d.flag FROM fact f JOIN dimn d ON f.fk = d.pk
	WHERE f.grp = 2 AND d.lvl < 30
	ORDER BY d.flag NULLS LAST, f.score DESC NULLS LAST, f.id LIMIT 20 OFFSET 7000');
SELECT oj('SELECT f.id, f.score, d.pk, d.tier FROM fact f JOIN dimn d ON f.fk = d.pk
	WHERE f.grp = 2 AND d.lvl < 30
	ORDER BY d.tier DESC, f.score DESC NULLS LAST, f.id LIMIT 20 OFFSET 1000');
SELECT * FROM oj_run('SELECT f.id, f.score, d.pk, d.flag FROM fact f JOIN dimn d ON f.fk = d.pk
	WHERE f.grp = 2 AND d.lvl < 30
	ORDER BY d.flag NULLS FIRST, f.score DESC NULLS LAST, f.id LIMIT 20 OFFSET 1300');
SET pg_lion.pushdown_margin = 1000;
EXPLAIN (COSTS OFF) SELECT f.id, f.score, d.pk, d.kind FROM fact f JOIN dimn d ON f.fk = d.pk
	WHERE f.grp = 2 AND d.kind IS NOT NULL
	ORDER BY d.kind NULLS FIRST, f.score DESC NULLS LAST, f.id LIMIT 20;
RESET pg_lion.pushdown_margin;
SELECT oj('SELECT f.id, f.score, d.pk, d.kind FROM fact f JOIN dimn d ON f.fk = d.pk
	WHERE f.grp = 2 AND d.kind IS NOT NULL
	ORDER BY d.kind NULLS FIRST, f.score DESC NULLS LAST, f.id LIMIT 20');

-- ... a value the statistics have never seen, inserted after ANALYZE, and
--     one inserted in the same transaction before the query: each is an
--     entry of the column, walked in its place; the planner's arms are only
--     its price.
INSERT INTO dimn SELECT 100000 + i, true, 9, i % 100, 'zinc' FROM generate_series(1, 50) i;
INSERT INTO fact SELECT 600000 + i, 100001 + i % 50, 2, 0, i, '{a1}', 'n'
  FROM generate_series(1, 30) i;
SELECT oj('SELECT f.id, f.score, d.pk, d.kind FROM fact f JOIN dimn d ON f.fk = d.pk
	WHERE f.grp = 2 AND d.lvl < 30
	ORDER BY d.kind DESC NULLS LAST, f.score DESC NULLS LAST, f.id LIMIT 20');
SELECT oj('SELECT f.id, f.score, d.pk, d.kind FROM fact f JOIN dimn d ON f.fk = d.pk
	WHERE f.grp = 2 AND d.lvl < 30
	ORDER BY d.kind NULLS LAST, f.score DESC NULLS LAST, f.id OFFSET 29600');
BEGIN;
INSERT INTO dimn SELECT 100100 + i, false, -1, i % 100, 'aluminium' FROM generate_series(1, 50) i;
INSERT INTO fact SELECT 600100 + i, 100101 + i % 50, 2, 0, 5000 + i, '{a1}', 'n'
  FROM generate_series(1, 30) i;
SELECT oj('SELECT f.id, f.score, d.pk, d.kind FROM fact f JOIN dimn d ON f.fk = d.pk
	WHERE f.grp = 2 AND d.lvl < 30
	ORDER BY d.kind NULLS FIRST, f.score DESC NULLS LAST, f.id LIMIT 20 OFFSET 2280');
SELECT oj('SELECT f.id, f.score, d.pk, d.tier FROM fact f JOIN dimn d ON f.fk = d.pk
	WHERE f.grp = 2 AND d.lvl < 30
	ORDER BY d.tier, f.score DESC NULLS LAST, f.id LIMIT 20');
SET LOCAL pg_lion.pushdown_margin = 1000;
SELECT f.id, f.score, d.pk, d.kind, d.tier FROM fact f JOIN dimn d ON f.fk = d.pk
	WHERE f.grp = 2 AND d.lvl < 30
	ORDER BY d.tier, f.score DESC NULLS LAST, f.id LIMIT 5;
ROLLBACK;

-- ... a key set (§47.6), built once for every arm
SELECT oj('SELECT f.id, f.score, d.pk, d.flag FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl < 30
	  AND f.fk IN (SELECT pk FROM flt WHERE x < 3)
	ORDER BY d.flag, f.score DESC NULLS LAST, f.id LIMIT 20 OFFSET 600');
SELECT * FROM oj_run('SELECT f.id, f.score, d.pk, d.flag FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl < 30
	  AND f.fk IN (SELECT pk FROM flt WHERE x < 3)
	ORDER BY d.flag, f.score DESC NULLS LAST, f.id LIMIT 20 OFFSET 600');

-- ... a rescan, the dimension's set made again for each outer row's value,
--     once however many arms the row runs (seven rows, two arms each: 7
--     builds), and entries passed over between rescans; and a generic
--     plan's Param
SELECT oj('SELECT x.pk, s.* FROM flt x, LATERAL (SELECT f.id, f.score, d.pk, d.flag
	FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = x.x + 3
	ORDER BY d.flag, f.score DESC NULLS LAST, f.id LIMIT 3) s
	WHERE x.pk < 20 ORDER BY x.pk, s.flag, s.score DESC NULLS LAST, s.id');
SELECT oj('SELECT x.pk, s.* FROM flt x, LATERAL (SELECT f.id, f.score, d.pk, d.flag
	FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = x.x + 3
	ORDER BY d.flag, f.score DESC NULLS LAST, f.id LIMIT 3 OFFSET 100) s
	WHERE x.pk < 20 ORDER BY x.pk, s.flag, s.score DESC NULLS LAST, s.id');
SELECT btrim(ln) AS counters FROM oj_run('SELECT x.pk, s.* FROM flt x, LATERAL (SELECT f.id, f.score, d.pk, d.flag
	FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl = x.x + 3
	ORDER BY d.flag, f.score DESC NULLS LAST, f.id LIMIT 3 OFFSET 100) s
	WHERE x.pk < 20 ORDER BY x.pk, s.flag, s.score DESC NULLS LAST, s.id') ln
 WHERE ln ~ '(LionOrderedSplit|Arms Run|Entries Skipped|Lion Set):';
SET pg_lion.ordered_split_max = 10;
SELECT oj('SELECT x.pk, s.* FROM flt x, LATERAL (SELECT f.id, f.score, d.pk, d.kind
	FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.kind IN (x.x, x.x + 4)
	ORDER BY d.kind DESC, f.score DESC NULLS LAST, f.id LIMIT 3 OFFSET 1300) s
	WHERE x.pk < 20 ORDER BY x.pk, s.kind DESC, s.score DESC NULLS LAST, s.id');
RESET pg_lion.ordered_split_max;
SET plan_cache_mode = force_generic_plan;
SET pg_lion.pushdown_margin = 1000;
PREPARE ps(int) AS SELECT f.id, f.score, d.pk, d.flag FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND d.lvl = $1 ORDER BY d.flag DESC, f.score DESC NULLS LAST, f.id LIMIT 5;
RESET pg_lion.pushdown_margin;
SELECT oj('EXECUTE ps(4)', 'SELECT f.id, f.score, d.pk, d.flag FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND d.lvl = 4 ORDER BY d.flag DESC, f.score DESC NULLS LAST, f.id LIMIT 5');
SELECT oj('EXECUTE ps(9)', 'SELECT f.id, f.score, d.pk, d.flag FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND d.lvl = 9 ORDER BY d.flag DESC, f.score DESC NULLS LAST, f.id LIMIT 5');
DEALLOCATE ps;
RESET plan_cache_mode;

-- ... an arm's set past hash_mem: the set's memory, charged with the
--     dimension's set and spilled to its arena past it (§30.4, "Spilling"),
--     given back as the next arm begins.  A dimension of 640,000 narrow rows,
--     55 containers: at a 64 kB hash_mem, an arm's set without a set of the
--     dimension - the entry whole - spills in each arm; with d.lvl < 4 the
--     set is in memory and the arm's past it; with d.lvl < 5 the set spills
--     too, and the arms' sets are in the arena whole; in a LATERAL subquery,
--     the set rebuilt for each outer row lets go of the arms' with its own.
CREATE TABLE dimbig (pk int PRIMARY KEY, flag bool NOT NULL, lvl int NOT NULL)
	WITH (autovacuum_enabled = off);
INSERT INTO dimbig
SELECT i, (hashint8extended(i::bigint, 5) & 9223372036854775807) % 3 <> 0,
	   ((hashint8extended(i::bigint, 6) & 9223372036854775807) % 100)::int
  FROM generate_series(1, 640000) i;
CREATE INDEX dimbig_flag ON dimbig USING lion (flag);
CREATE INDEX dimbig_lvl ON dimbig USING lion (lvl);
VACUUM (FREEZE, ANALYZE) dimbig;
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SELECT oj('SELECT f.id, f.score, d.pk, d.flag FROM fact f JOIN dimbig d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}''
	ORDER BY d.flag DESC, f.score DESC NULLS LAST, f.id LIMIT 20 OFFSET 17000');
SELECT btrim(ln) AS counters FROM oj_run('SELECT f.id, f.score, d.pk, d.flag FROM fact f JOIN dimbig d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}''
	ORDER BY d.flag DESC, f.score DESC NULLS LAST, f.id LIMIT 20 OFFSET 17000') ln
 WHERE ln ~ '(Arms Run|Arm Sets Spilled|Dimension Lion Set):';
SELECT oj('SELECT f.id, f.score, d.pk, d.flag FROM fact f JOIN dimbig d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl < 4
	ORDER BY d.flag DESC, f.score DESC NULLS LAST, f.id LIMIT 20 OFFSET 500');
SELECT btrim(ln) AS counters FROM oj_run('SELECT f.id, f.score, d.pk, d.flag FROM fact f JOIN dimbig d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl < 4
	ORDER BY d.flag DESC, f.score DESC NULLS LAST, f.id LIMIT 20 OFFSET 500') ln
 WHERE ln ~ '(Arms Run|Arm Sets Spilled|Dimension Lion Set):';
SELECT oj('SELECT f.id, f.score, d.pk, d.flag FROM fact f JOIN dimbig d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl < 5
	ORDER BY d.flag, f.score DESC NULLS LAST, f.id LIMIT 20 OFFSET 500');
SELECT btrim(ln) AS counters FROM oj_run('SELECT f.id, f.score, d.pk, d.flag FROM fact f JOIN dimbig d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl < 5
	ORDER BY d.flag, f.score DESC NULLS LAST, f.id LIMIT 20 OFFSET 500') ln
 WHERE ln ~ '(Arms Run|Arm Sets Spilled|Dimension Lion Set):';
SELECT oj('SELECT x.pk, s.* FROM flt x, LATERAL (SELECT f.id, f.score, d.pk, d.flag
	FROM fact f JOIN dimbig d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl < x.x + 3
	ORDER BY d.flag DESC, f.score DESC NULLS LAST, f.id LIMIT 3 OFFSET 700) s
	WHERE x.pk < 20 ORDER BY x.pk, s.flag DESC, s.score DESC NULLS LAST, s.id');
SELECT btrim(ln) AS counters FROM oj_run('SELECT x.pk, s.* FROM flt x, LATERAL (SELECT f.id, f.score, d.pk, d.flag
	FROM fact f JOIN dimbig d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl < x.x + 3
	ORDER BY d.flag DESC, f.score DESC NULLS LAST, f.id LIMIT 3 OFFSET 700) s
	WHERE x.pk < 20 ORDER BY x.pk, s.flag DESC, s.score DESC NULLS LAST, s.id') ln
 WHERE ln ~ '(Arms Run|Arm Sets Spilled|Dimension Lion Set):';
RESET hash_mem_multiplier;
RESET work_mem;
DROP TABLE dimbig;

-- ... and what it declines: a column whose lion order is not the ORDER BY's
--     - another operator family, another collation - an expression of the
--     column, and what the join itself declines (§47.1); and the settings.
SELECT oj('SELECT f.id, f.score, d.pk, d.tier FROM fact f JOIN dimn d ON f.fk = d.pk
	WHERE f.grp = 2 AND d.lvl < 30
	ORDER BY d.tier USING ~<~, f.score DESC NULLS LAST, f.id LIMIT 20');
SELECT oj('SELECT f.id, f.score, d.pk, d.tier FROM fact f JOIN dimn d ON f.fk = d.pk
	WHERE f.grp = 2 AND d.lvl < 30
	ORDER BY d.tier COLLATE "POSIX", f.score DESC NULLS LAST, f.id LIMIT 20');
SELECT oj('SELECT f.id, f.score, d.pk, d.kind FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND d.lvl < 30
	ORDER BY d.kind % 2, f.score DESC NULLS LAST, f.id LIMIT 20');
SELECT oj('SELECT f.id, f.score, d.pk FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND d.lvl < 30
	ORDER BY d.flag DESC, f.score DESC NULLS LAST, f.id LIMIT 20 FOR SHARE OF d');
SELECT oj('SELECT f.id, f.score, d.pk FROM fact f LEFT JOIN dim d ON f.fk = d.pk AND d.lvl < 30
	WHERE f.grp = 2
	ORDER BY d.flag DESC, f.score DESC NULLS LAST, f.id LIMIT 20');
SET pg_lion.ordered_split_max = 1;
SELECT oj('SELECT f.id, f.score, d.pk, d.flag FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl < 30
	ORDER BY d.flag DESC, f.score DESC NULLS LAST, f.id LIMIT 20');
SET pg_lion.ordered_split_max = 0;
SELECT oj('SELECT f.id, f.score, d.pk, d.flag FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl < 30
	ORDER BY d.flag DESC, f.score DESC NULLS LAST, f.id LIMIT 20');
RESET pg_lion.ordered_split_max;
SET pg_lion.enable_ordered_join = off;
SELECT * FROM oj_plan('SELECT f.id, f.score, d.pk, d.flag FROM fact f JOIN dim d ON f.fk = d.pk
	WHERE f.grp = 2 AND f.tags @> ''{a1}'' AND d.lvl < 30
	ORDER BY d.flag DESC, f.score DESC NULLS LAST, f.id LIMIT 20');
RESET pg_lion.enable_ordered_join;
DROP TABLE dimn;

DROP FUNCTION oj(text, text);
DROP FUNCTION oj_plan(text);
DROP FUNCTION oj_run(text);
DROP TABLE fact, dim, flt, fltn;
