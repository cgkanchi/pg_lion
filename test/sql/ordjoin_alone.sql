-- LionOrderedJoin and LionOrderedSplit alone under a LIMIT, and not
-- (DESIGN.md §47.10).
--
-- The LIMIT's share of the node's walk is taken only where nothing above the
-- node drops a row it returns: outer joins, anti joins, and the tables the
-- node folds into key sets.  Under an inner join that is no key set, or a
-- clause on a LATERAL subquery's columns, the node is priced as its whole
-- walk, all of it at start-up, which core's Limit cannot discount.  The
-- tables are made so that core's estimate of what those filters let through
-- is many times what does: the slice of the fact the pages walk holds half
-- the keys, the small key table's keys of kind 2 lie past them but for
-- forty, and the side table's 'high' rows of the slice are those of its
-- lowest scores, which the walk meets last.  Each query is run through the
-- node (lion's paths priced at a thousandth) and through core's plan with the
-- node off, and the two answers compared row by row in order; every ORDER BY
-- ends in a unique key.  What is chosen with nothing forced is printed as the
-- node's name, or core's, without the plan: core's plans differ between
-- majors.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;
SET default_statistics_target = 1000;
SET work_mem = '64MB';
CREATE SCHEMA ojalone;
SET search_path = ojalone, public;

CREATE TABLE dim (
	pk int PRIMARY KEY,
	flag bool NOT NULL,			-- lion: a third true
	lvl int NOT NULL,			-- lion: a hundred values
	rel int NOT NULL,			-- the ORDER BY's third key
	payload text NOT NULL		-- wide: the heap is many times the btree
) WITH (autovacuum_enabled = off);
INSERT INTO dim
SELECT i, i % 3 = 0,
	   ((hashint8extended(i::bigint, 2) & 9223372036854775807) % 100)::int,
	   ((hashint8extended(i::bigint, 7) & 9223372036854775807) % 1000)::int,
	   repeat(md5(i::text), 8)
  FROM generate_series(1, 100000) i;
CREATE INDEX dim_flag ON dim USING lion (flag);
CREATE INDEX dim_lvl ON dim USING lion (lvl);

-- four slices (grp, ver), a key at most once in each: (1, 2), the pages',
-- holds the keys up to 50,000, the three others every key
CREATE TABLE fact (
	id int PRIMARY KEY,
	grp int NOT NULL,
	ver int NOT NULL,
	fk int NOT NULL,
	score numeric,				-- NULL for every 97th key
	rec text NOT NULL,			-- lion: x, y or z
	note text NOT NULL
) WITH (autovacuum_enabled = off);
INSERT INTO fact
SELECT row_number() OVER (), g, v, k,
	   CASE WHEN k % 97 = 0 THEN NULL
			ELSE ((hashint8extended(k::bigint * 4 + g * 2 + v, 4) & 9223372036854775807) % 1000000)::numeric / 100 END,
	   (ARRAY['x', 'y', 'z'])[1 + ((hashint8extended(k::bigint * 4 + g * 2 + v, 5) & 9223372036854775807) % 3)::int],
	   'n'
  FROM (VALUES (1, 1), (1, 2), (2, 1), (2, 2)) s(g, v),
	   LATERAL generate_series(1, CASE WHEN (g, v) = (1, 2) THEN 50000 ELSE 100000 END) k;
CREATE INDEX fact_gvs ON fact (grp, ver, score DESC NULLS LAST) INCLUDE (fk);
CREATE UNIQUE INDEX fact_fgv ON fact (fk, grp, ver);
CREATE INDEX fact_rec ON fact USING lion (rec);

-- the small key table, unique by (fk, kind): kind 1's keys drawn 4,000 times
-- from every key, kind 2's 9,000 times from past the slice's, and forty in it
CREATE TABLE kt (fk int NOT NULL, kind int NOT NULL, since int NOT NULL,
	PRIMARY KEY (fk, kind)) WITH (autovacuum_enabled = off);
INSERT INTO kt SELECT DISTINCT k, kind, k % 7
  FROM generate_series(1, 3) kind,
	   LATERAL (SELECT CASE WHEN kind = 2 THEN 50001 ELSE 1 END +
					   ((hashint8extended(g::bigint * 10 + kind, 8) & 9223372036854775807) %
						CASE WHEN kind = 2 THEN 50000 ELSE 100000 END)::int k
				  FROM generate_series(1, CASE WHEN kind = 2 THEN 9000 ELSE 4000 END) g) s;
INSERT INTO kt SELECT 1000 * g + 7, 2, 0 FROM generate_series(1, 40) g;

-- a side table per key and day, joined to the fact's key: 'high' for every
-- key past the slice's, and in the slice for the fifth of its keys with the
-- lowest scores
CREATE TABLE sig (fk int NOT NULL, grp int NOT NULL, ver int NOT NULL, day int NOT NULL,
	level text NOT NULL, PRIMARY KEY (fk, grp, ver, day)) WITH (autovacuum_enabled = off);
INSERT INTO sig SELECT k, 1, 2, day,
	   CASE WHEN k > 50000 OR coalesce(f.score, 0) < 2000 THEN 'high' ELSE 'low' END
  FROM generate_series(1, 100000) k
  LEFT JOIN fact f ON f.fk = k AND f.grp = 1 AND f.ver = 2,
	   generate_series(1, 3) day;

-- the keys suppressed, one in fifty
CREATE TABLE sup (fk int NOT NULL, reason int NOT NULL, PRIMARY KEY (fk, reason))
	WITH (autovacuum_enabled = off);
INSERT INTO sup SELECT k, 1 FROM generate_series(1, 100000, 50) k;

VACUUM (FREEZE, ANALYZE) dim;
VACUUM (FREEZE, ANALYZE) fact;
VACUUM (FREEZE, ANALYZE) kt;
VACUUM (FREEZE, ANALYZE) sig;
VACUUM (FREEZE, ANALYZE) sup;

/*
 * oja() runs q through the node - lion's paths priced at a thousandth - and
 * through core's plan (the node off), compares the two answers in order, and
 * says which node the first plan was and with which key sets.  oja_plan()
 * says which the plan with nothing forced is: LionOrderedSplit,
 * LionOrderedJoin or core's.
 */
CREATE FUNCTION oja_kind(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	used boolean := false;
	split boolean := false;
	keys text := '';
BEGIN
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
	RETURN CASE WHEN split THEN 'LionOrderedSplit' || keys
				WHEN used THEN 'LionOrderedJoin' || keys
				ELSE 'core''s plan' END;
END $$;
CREATE FUNCTION oja_plan(q text) RETURNS text
LANGUAGE sql AS $$ SELECT ojalone.oja_kind(q) $$;
CREATE FUNCTION oja(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	r record;
	kind text;
	a text[] := '{}';
	b text[] := '{}';
BEGIN
	PERFORM set_config('pg_lion.pushdown_margin', '1000', true);
	kind := ojalone.oja_kind(q);
	FOR r IN EXECUTE q LOOP
		a := a || r::text;
	END LOOP;

	PERFORM set_config('pg_lion.pushdown_margin', '1', true);
	PERFORM set_config('pg_lion.enable_ordered_join', 'off', true);
	FOR r IN EXECUTE q LOOP
		b := b || r::text;
	END LOOP;
	PERFORM set_config('pg_lion.enable_ordered_join', 'on', true);

	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH: %s rows through the node, %s ordinary; first %s / %s',
					  coalesce(array_length(a, 1), 0), coalesce(array_length(b, 1), 0),
					  a[1], b[1]);
	END IF;
	RETURN format('%s, %s rows', kind, coalesce(array_length(a, 1), 0));
END $$;

/*
 * oja_run() runs q through the node under EXPLAIN ANALYZE and prints the
 * node's lines asked for, their counters; oja_lines() prints the lines asked
 * for of its plan, which core's nodes around it may differ in elsewhere.
 */
CREATE FUNCTION oja_lines(q text, pat text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('pg_lion.pushdown_margin', '1000', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ pat THEN
			RETURN NEXT btrim(ln);
		END IF;
	END LOOP;
	PERFORM set_config('pg_lion.pushdown_margin', '1', true);
END $$;
CREATE FUNCTION oja_run(q text, pat text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('pg_lion.pushdown_margin', '1000', true);
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF ln ~ pat THEN
			RETURN NEXT btrim(regexp_replace(ln, '\s*\(actual.*\)', ''));
		END IF;
	END LOOP;
	PERFORM set_config('pg_lion.pushdown_margin', '1', true);
END $$;

-- 1. The page: the dimension's flag leading the ORDER BY, the fact's score
--    and the dimension's rel after it; the side table LEFT JOINed LATERAL by
--    the fact's key, and a WHERE clause on its level, strict, which makes
--    that join an inner one; the small key table inner-joined to the key with
--    a constant kind; the suppressed keys anti-joined.  Core takes the
--    filters above the walk to let some three thousand of its rows through,
--    ten do, and the walk runs both arms to their ends: core's plan.  Forced,
--    the key table is a key set - unique by its key with the kind pinned to a
--    constant, its key in the join's target only because the side table's
--    key is of the key's class - but the side table's join still filters
--    above the node, and the counters show the whole walk.
SELECT oja_plan($$SELECT d.pk, d.flag, d.rel, f.score, f.rec, s.level
	FROM dim d
	JOIN fact f ON f.grp = 1 AND f.ver = 2 AND f.fk = d.pk
	LEFT JOIN LATERAL (SELECT s.* FROM sig s WHERE s.fk = f.fk AND s.grp = 1 AND s.ver = 2 AND s.day = 3) s ON true
	JOIN kt ON kt.fk = d.pk AND kt.kind = 2
	LEFT JOIN sup ON sup.fk = d.pk AND sup.reason = 1
	WHERE sup.fk IS NULL AND s.level = 'high'
	ORDER BY d.flag DESC NULLS LAST, f.score DESC NULLS LAST, d.rel DESC NULLS LAST, d.pk
	LIMIT 100$$);
SELECT oja($$SELECT d.pk, d.flag, d.rel, f.score, f.rec, s.level
	FROM dim d
	JOIN fact f ON f.grp = 1 AND f.ver = 2 AND f.fk = d.pk
	LEFT JOIN LATERAL (SELECT s.* FROM sig s WHERE s.fk = f.fk AND s.grp = 1 AND s.ver = 2 AND s.day = 3) s ON true
	JOIN kt ON kt.fk = d.pk AND kt.kind = 2
	LEFT JOIN sup ON sup.fk = d.pk AND sup.reason = 1
	WHERE sup.fk IS NULL AND s.level = 'high'
	ORDER BY d.flag DESC NULLS LAST, f.score DESC NULLS LAST, d.rel DESC NULLS LAST, d.pk
	LIMIT 100$$);
SELECT * FROM oja_run($$SELECT d.pk, d.flag, d.rel, f.score, f.rec, s.level
	FROM dim d
	JOIN fact f ON f.grp = 1 AND f.ver = 2 AND f.fk = d.pk
	LEFT JOIN LATERAL (SELECT s.* FROM sig s WHERE s.fk = f.fk AND s.grp = 1 AND s.ver = 2 AND s.day = 3) s ON true
	JOIN kt ON kt.fk = d.pk AND kt.kind = 2
	LEFT JOIN sup ON sup.fk = d.pk AND sup.reason = 1
	WHERE sup.fk IS NULL AND s.level = 'high'
	ORDER BY d.flag DESC NULLS LAST, f.score DESC NULLS LAST, d.rel DESC NULLS LAST, d.pk
	LIMIT 100$$, '(Arms Run|Key Sets|Key Set Keys|Index Entries Walked|Dimension Descents):');

-- ... the same page ordered by the fact's score, the join's order
SELECT oja_plan($$SELECT d.pk, d.flag, f.score, f.rec, s.level
	FROM dim d
	JOIN fact f ON f.grp = 1 AND f.ver = 2 AND f.fk = d.pk
	LEFT JOIN LATERAL (SELECT s.* FROM sig s WHERE s.fk = f.fk AND s.grp = 1 AND s.ver = 2 AND s.day = 3) s ON true
	JOIN kt ON kt.fk = d.pk AND kt.kind = 2
	LEFT JOIN sup ON sup.fk = d.pk AND sup.reason = 1
	WHERE sup.fk IS NULL AND s.level = 'high'
	ORDER BY f.score DESC NULLS LAST, d.pk LIMIT 100$$);
SELECT oja($$SELECT d.pk, d.flag, f.score, f.rec, s.level
	FROM dim d
	JOIN fact f ON f.grp = 1 AND f.ver = 2 AND f.fk = d.pk
	LEFT JOIN LATERAL (SELECT s.* FROM sig s WHERE s.fk = f.fk AND s.grp = 1 AND s.ver = 2 AND s.day = 3) s ON true
	JOIN kt ON kt.fk = d.pk AND kt.kind = 2
	LEFT JOIN sup ON sup.fk = d.pk AND sup.reason = 1
	WHERE sup.fk IS NULL AND s.level = 'high'
	ORDER BY f.score DESC NULLS LAST, d.pk LIMIT 100$$);

-- 2. Each filter above alone.  The side table's level, without the key
--    table: the side table an inner join above the node.
SELECT oja_plan($$SELECT d.pk, d.flag, d.rel, f.score, s.level
	FROM dim d
	JOIN fact f ON f.grp = 1 AND f.ver = 2 AND f.fk = d.pk
	LEFT JOIN LATERAL (SELECT s.* FROM sig s WHERE s.fk = f.fk AND s.grp = 1 AND s.ver = 2 AND s.day = 3) s ON true
	LEFT JOIN sup ON sup.fk = d.pk AND sup.reason = 1
	WHERE sup.fk IS NULL AND s.level = 'high'
	ORDER BY d.flag DESC NULLS LAST, f.score DESC NULLS LAST, d.rel DESC NULLS LAST, d.pk
	LIMIT 100$$);
SELECT oja($$SELECT d.pk, d.flag, d.rel, f.score, s.level
	FROM dim d
	JOIN fact f ON f.grp = 1 AND f.ver = 2 AND f.fk = d.pk
	LEFT JOIN LATERAL (SELECT s.* FROM sig s WHERE s.fk = f.fk AND s.grp = 1 AND s.ver = 2 AND s.day = 3) s ON true
	LEFT JOIN sup ON sup.fk = d.pk AND sup.reason = 1
	WHERE sup.fk IS NULL AND s.level = 'high'
	ORDER BY d.flag DESC NULLS LAST, f.score DESC NULLS LAST, d.rel DESC NULLS LAST, d.pk
	LIMIT 100$$);

-- ... a clause on the side table's level that is not strict, which leaves
--     the left join a left join and filters above it.
SELECT oja_plan($$SELECT d.pk, d.flag, d.rel, f.score, s.level
	FROM dim d
	JOIN fact f ON f.grp = 1 AND f.ver = 2 AND f.fk = d.pk
	LEFT JOIN LATERAL (SELECT s.* FROM sig s WHERE s.fk = f.fk AND s.grp = 1 AND s.ver = 2 AND s.day = 3) s ON true
	WHERE s.level IS DISTINCT FROM 'low'
	ORDER BY d.flag DESC NULLS LAST, f.score DESC NULLS LAST, d.rel DESC NULLS LAST, d.pk
	LIMIT 100$$);
SELECT oja($$SELECT d.pk, d.flag, d.rel, f.score, s.level
	FROM dim d
	JOIN fact f ON f.grp = 1 AND f.ver = 2 AND f.fk = d.pk
	LEFT JOIN LATERAL (SELECT s.* FROM sig s WHERE s.fk = f.fk AND s.grp = 1 AND s.ver = 2 AND s.day = 3) s ON true
	WHERE s.level IS DISTINCT FROM 'low'
	ORDER BY d.flag DESC NULLS LAST, f.score DESC NULLS LAST, d.rel DESC NULLS LAST, d.pk
	LIMIT 100$$);
-- ... and one on a LATERAL subquery that core cannot pull up, its LIMIT 1.
--     Not alone, the split is priced at its whole walk, all at start-up, and
--     is still offered (§47.10): here it is under core's plans' price, which
--     sort the whole of the flag's first value and run the subquery for each
--     of its rows.  The joins above the node are core's, priced by its own
--     estimate of what they let through.
SELECT oja_plan($$SELECT d.pk, d.flag, d.rel, f.score, s.level
	FROM dim d
	JOIN fact f ON f.grp = 1 AND f.ver = 2 AND f.fk = d.pk
	LEFT JOIN LATERAL (SELECT s.level FROM sig s WHERE s.fk = f.fk AND s.grp = 1 AND s.ver = 2
					   ORDER BY s.day DESC LIMIT 1) s ON true
	WHERE s.level = 'high'
	ORDER BY d.flag DESC NULLS LAST, f.score DESC NULLS LAST, d.rel DESC NULLS LAST, d.pk
	LIMIT 100$$);
SELECT oja($$SELECT d.pk, d.flag, d.rel, f.score, s.level
	FROM dim d
	JOIN fact f ON f.grp = 1 AND f.ver = 2 AND f.fk = d.pk
	LEFT JOIN LATERAL (SELECT s.level FROM sig s WHERE s.fk = f.fk AND s.grp = 1 AND s.ver = 2
					   ORDER BY s.day DESC LIMIT 1) s ON true
	WHERE s.level = 'high'
	ORDER BY d.flag DESC NULLS LAST, f.score DESC NULLS LAST, d.rel DESC NULLS LAST, d.pk
	LIMIT 100$$);

-- ... the key table, no key set: a column of it read, or its kind not pinned
--     to one value, which leaves a key in it twice.  (A key repeated comes
--     out repeated, the rows alike, so the ORDER BY needs no more keys.)
SELECT oja_plan($$SELECT d.pk, d.flag, d.rel, f.score, kt.since
	FROM dim d
	JOIN fact f ON f.grp = 1 AND f.ver = 2 AND f.fk = d.pk
	JOIN kt ON kt.fk = d.pk AND kt.kind = 2
	LEFT JOIN sup ON sup.fk = d.pk AND sup.reason = 1
	WHERE sup.fk IS NULL
	ORDER BY d.flag DESC NULLS LAST, f.score DESC NULLS LAST, d.rel DESC NULLS LAST, d.pk
	LIMIT 100$$);
SELECT oja($$SELECT d.pk, d.flag, d.rel, f.score, kt.since
	FROM dim d
	JOIN fact f ON f.grp = 1 AND f.ver = 2 AND f.fk = d.pk
	JOIN kt ON kt.fk = d.pk AND kt.kind = 2
	LEFT JOIN sup ON sup.fk = d.pk AND sup.reason = 1
	WHERE sup.fk IS NULL
	ORDER BY d.flag DESC NULLS LAST, f.score DESC NULLS LAST, d.rel DESC NULLS LAST, d.pk
	LIMIT 100$$);
SELECT oja_plan($$SELECT d.pk, d.flag, d.rel, f.score
	FROM dim d
	JOIN fact f ON f.grp = 1 AND f.ver = 2 AND f.fk = d.pk
	JOIN kt ON kt.fk = d.pk AND kt.kind IN (2, 3)
	ORDER BY d.flag DESC NULLS LAST, f.score DESC NULLS LAST, d.rel DESC NULLS LAST, d.pk
	LIMIT 100$$);
SELECT oja($$SELECT d.pk, d.flag, d.rel, f.score
	FROM dim d
	JOIN fact f ON f.grp = 1 AND f.ver = 2 AND f.fk = d.pk
	JOIN kt ON kt.fk = d.pk AND kt.kind IN (2, 3)
	ORDER BY d.flag DESC NULLS LAST, f.score DESC NULLS LAST, d.rel DESC NULLS LAST, d.pk
	LIMIT 100$$);
-- 3. Alone under the LIMIT, the split's: the page with only left joins and
--    anti joins above the node - the side table's columns read, its rows
--    filtering nothing, the suppressed keys a NOT EXISTS or a LEFT JOIN ...
--    IS NULL - and the first arm fills it.
SELECT oja_plan($$SELECT d.pk, d.flag, d.rel, f.score, f.rec, s.level
	FROM dim d
	JOIN fact f ON f.grp = 1 AND f.ver = 2 AND f.fk = d.pk
	LEFT JOIN LATERAL (SELECT s.* FROM sig s WHERE s.fk = f.fk AND s.grp = 1 AND s.ver = 2 AND s.day = 3) s ON true
	LEFT JOIN sup ON sup.fk = d.pk AND sup.reason = 1
	WHERE sup.fk IS NULL
	ORDER BY d.flag DESC NULLS LAST, f.score DESC NULLS LAST, d.rel DESC NULLS LAST, d.pk
	LIMIT 100$$);
SELECT oja($$SELECT d.pk, d.flag, d.rel, f.score, f.rec, s.level
	FROM dim d
	JOIN fact f ON f.grp = 1 AND f.ver = 2 AND f.fk = d.pk
	LEFT JOIN LATERAL (SELECT s.* FROM sig s WHERE s.fk = f.fk AND s.grp = 1 AND s.ver = 2 AND s.day = 3) s ON true
	LEFT JOIN sup ON sup.fk = d.pk AND sup.reason = 1
	WHERE sup.fk IS NULL
	ORDER BY d.flag DESC NULLS LAST, f.score DESC NULLS LAST, d.rel DESC NULLS LAST, d.pk
	LIMIT 100$$);
SELECT oja_plan($$SELECT d.pk, d.flag, d.rel, f.score, s.level
	FROM dim d
	JOIN fact f ON f.grp = 1 AND f.ver = 2 AND f.fk = d.pk AND f.rec = 'x'
	LEFT JOIN sig s ON s.fk = f.fk AND s.grp = 1 AND s.ver = 2 AND s.day = 3 AND s.level = 'high'
	WHERE NOT EXISTS (SELECT 1 FROM sup WHERE sup.fk = d.pk AND sup.reason = 1)
	ORDER BY d.flag DESC NULLS LAST, f.score DESC NULLS LAST, d.rel DESC NULLS LAST, d.pk
	LIMIT 100$$);
SELECT oja($$SELECT d.pk, d.flag, d.rel, f.score, s.level
	FROM dim d
	JOIN fact f ON f.grp = 1 AND f.ver = 2 AND f.fk = d.pk AND f.rec = 'x'
	LEFT JOIN sig s ON s.fk = f.fk AND s.grp = 1 AND s.ver = 2 AND s.day = 3 AND s.level = 'high'
	WHERE NOT EXISTS (SELECT 1 FROM sup WHERE sup.fk = d.pk AND sup.reason = 1)
	ORDER BY d.flag DESC NULLS LAST, f.score DESC NULLS LAST, d.rel DESC NULLS LAST, d.pk
	LIMIT 100$$);

-- ... the join's: ordered by the fact's score, a tenth of the dimension's
--     levels and a third of the fact's rec, the side table LEFT JOINed and
--     the suppressed keys a NOT EXISTS above it.
SELECT oja_plan($$SELECT d.pk, d.rel, f.score, s.level
	FROM dim d
	JOIN fact f ON f.grp = 1 AND f.ver = 2 AND f.fk = d.pk
	LEFT JOIN LATERAL (SELECT s.* FROM sig s WHERE s.fk = f.fk AND s.grp = 1 AND s.ver = 2 AND s.day = 3) s ON true
	WHERE d.lvl < 10 AND f.rec = 'x' AND NOT EXISTS (SELECT 1 FROM sup WHERE sup.fk = d.pk AND sup.reason = 1)
	ORDER BY f.score DESC NULLS LAST, d.pk LIMIT 100$$);
SELECT oja($$SELECT d.pk, d.rel, f.score, s.level
	FROM dim d
	JOIN fact f ON f.grp = 1 AND f.ver = 2 AND f.fk = d.pk
	LEFT JOIN LATERAL (SELECT s.* FROM sig s WHERE s.fk = f.fk AND s.grp = 1 AND s.ver = 2 AND s.day = 3) s ON true
	WHERE d.lvl < 10 AND f.rec = 'x' AND NOT EXISTS (SELECT 1 FROM sup WHERE sup.fk = d.pk AND sup.reason = 1)
	ORDER BY f.score DESC NULLS LAST, d.pk LIMIT 100$$);

-- ... and with the key table a key set (§47.6): kind 1's keys, among the
--     slice's as often as among the others, fill the first arm early; each
--     fact row's key is tested against them before the dimension is
--     descended.
SELECT oja_plan($$SELECT d.pk, d.flag, d.rel, f.score, s.level
	FROM dim d
	JOIN fact f ON f.grp = 1 AND f.ver = 2 AND f.fk = d.pk
	LEFT JOIN LATERAL (SELECT s.* FROM sig s WHERE s.fk = f.fk AND s.grp = 1 AND s.ver = 2 AND s.day = 3) s ON true
	JOIN kt ON kt.fk = d.pk AND kt.kind = 1
	LEFT JOIN sup ON sup.fk = d.pk AND sup.reason = 1
	WHERE sup.fk IS NULL
	ORDER BY d.flag DESC NULLS LAST, f.score DESC NULLS LAST, d.rel DESC NULLS LAST, d.pk
	LIMIT 100$$);
SELECT oja($$SELECT d.pk, d.flag, d.rel, f.score, s.level
	FROM dim d
	JOIN fact f ON f.grp = 1 AND f.ver = 2 AND f.fk = d.pk
	LEFT JOIN LATERAL (SELECT s.* FROM sig s WHERE s.fk = f.fk AND s.grp = 1 AND s.ver = 2 AND s.day = 3) s ON true
	JOIN kt ON kt.fk = d.pk AND kt.kind = 1
	LEFT JOIN sup ON sup.fk = d.pk AND sup.reason = 1
	WHERE sup.fk IS NULL
	ORDER BY d.flag DESC NULLS LAST, f.score DESC NULLS LAST, d.rel DESC NULLS LAST, d.pk
	LIMIT 100$$);
SELECT * FROM oja_run($$SELECT d.pk, d.flag, d.rel, f.score, s.level
	FROM dim d
	JOIN fact f ON f.grp = 1 AND f.ver = 2 AND f.fk = d.pk
	LEFT JOIN LATERAL (SELECT s.* FROM sig s WHERE s.fk = f.fk AND s.grp = 1 AND s.ver = 2 AND s.day = 3) s ON true
	JOIN kt ON kt.fk = d.pk AND kt.kind = 1
	LEFT JOIN sup ON sup.fk = d.pk AND sup.reason = 1
	WHERE sup.fk IS NULL
	ORDER BY d.flag DESC NULLS LAST, f.score DESC NULLS LAST, d.rel DESC NULLS LAST, d.pk
	LIMIT 100$$, '(Arms Run|Key Sets|Key Set Keys|Dimension Descents):');

-- ... and with kind 2's keys, of which the slice holds forty: alone too,
--     and priced by the key set's share of the dimension as if its keys
--     were spread over the walk as kind 1's are.  They are not, and the walk
--     runs both arms to their ends, each entry's key tested against the set
--     and no more (§47.10, "What a key set leaves").
SELECT oja_plan($$SELECT d.pk, d.flag, d.rel, f.score, s.level
	FROM dim d
	JOIN fact f ON f.grp = 1 AND f.ver = 2 AND f.fk = d.pk
	LEFT JOIN LATERAL (SELECT s.* FROM sig s WHERE s.fk = f.fk AND s.grp = 1 AND s.ver = 2 AND s.day = 3) s ON true
	JOIN kt ON kt.fk = d.pk AND kt.kind = 2
	LEFT JOIN sup ON sup.fk = d.pk AND sup.reason = 1
	WHERE sup.fk IS NULL
	ORDER BY d.flag DESC NULLS LAST, f.score DESC NULLS LAST, d.rel DESC NULLS LAST, d.pk
	LIMIT 100$$);
SELECT oja($$SELECT d.pk, d.flag, d.rel, f.score, s.level
	FROM dim d
	JOIN fact f ON f.grp = 1 AND f.ver = 2 AND f.fk = d.pk
	LEFT JOIN LATERAL (SELECT s.* FROM sig s WHERE s.fk = f.fk AND s.grp = 1 AND s.ver = 2 AND s.day = 3) s ON true
	JOIN kt ON kt.fk = d.pk AND kt.kind = 2
	LEFT JOIN sup ON sup.fk = d.pk AND sup.reason = 1
	WHERE sup.fk IS NULL
	ORDER BY d.flag DESC NULLS LAST, f.score DESC NULLS LAST, d.rel DESC NULLS LAST, d.pk
	LIMIT 100$$);

-- 4. A key set's key in the join's target (§47.6): the side table's join on
--    the key's class needs a member of it from the node, and with the
--    tables written key table first core takes the key table's - which the
--    node fills with the fact's key, equal to it and the same value - for its
--    hash join above.
SET enable_nestloop = off;
SET enable_mergejoin = off;
SELECT * FROM oja_lines($$SELECT d.pk, d.flag, f.score, s.level
	FROM kt JOIN dim d ON kt.fk = d.pk AND kt.kind = 2
	JOIN fact f ON f.fk = kt.fk AND f.grp = 1 AND f.ver = 2
	LEFT JOIN LATERAL (SELECT s.* FROM sig s WHERE s.fk = kt.fk AND s.grp = 1 AND s.ver = 2 AND s.day = 3) s ON true
	WHERE s.level = 'high'
	ORDER BY d.flag DESC NULLS LAST, f.score DESC NULLS LAST, d.pk LIMIT 100$$,
	'(Hash Cond:|Custom Scan|Key Sets:)');
SELECT oja($$SELECT d.pk, d.flag, f.score, s.level
	FROM kt JOIN dim d ON kt.fk = d.pk AND kt.kind = 2
	JOIN fact f ON f.fk = kt.fk AND f.grp = 1 AND f.ver = 2
	LEFT JOIN LATERAL (SELECT s.* FROM sig s WHERE s.fk = kt.fk AND s.grp = 1 AND s.ver = 2 AND s.day = 3) s ON true
	WHERE s.level = 'high'
	ORDER BY d.flag DESC NULLS LAST, f.score DESC NULLS LAST, d.pk LIMIT 100$$);
RESET enable_mergejoin;
RESET enable_nestloop;

SET client_min_messages = warning;
DROP SCHEMA ojalone CASCADE;
RESET client_min_messages;
RESET search_path;
