-- The relation's rows (DESIGN.md §29.11, "The relation's rows"): what the
-- intersection probe measures of a relation's own restriction clauses
-- corrects rel->rows as well as lion's own estimates, so that a join above
-- the relation is sized for the rows its filters leave.
--
-- The filters below follow one hidden attribute, and their AND holds fifty
-- times the rows the product of their selectivities says.  Sized for that
-- product, a join over the relation is estimated at a few rows; sized for
-- what the probe measures, at about what it returns.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
-- a sample of every row: the statistics, and the plans, do not depend on it
SET default_statistics_target = 1000;
SET max_parallel_workers_per_gather = 0;

-- 50,000 rows in 50 hidden groups h = id % 50, each group on every page;
-- c5, c25 and c50 follow h, p (7 values) and q (11) do not.  lrc_f has
-- three rows for every row of lrc.
CREATE TABLE lrc (id int PRIMARY KEY, c5 int NOT NULL, c25 int NOT NULL,
				  c50 int NOT NULL, p int NOT NULL, q int NOT NULL, pad text);
INSERT INTO lrc
SELECT i, (i % 50) / 10, (i % 50) / 2, i % 50, i % 7, i % 11, repeat('x', 100)
  FROM generate_series(1, 50000) i;
CREATE INDEX lrc_i ON lrc USING lion (c5, c25, c50, p, q);
CREATE TABLE lrc_f (id int, rid int NOT NULL, v int NOT NULL);
INSERT INTO lrc_f SELECT i, (i % 50000) + 1, i % 13 FROM generate_series(1, 150000) i;
VACUUM (FREEZE, ANALYZE) lrc, lrc_f;

/*
 * lrc_est() is the planner's row estimate of q's top node, with the
 * correction on or off; lrc_scan() that of the scan of lrc in it.
 */
CREATE FUNCTION lrc_est(q text, fix boolean, node text DEFAULT NULL)
RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	j	json;
	r	bigint;
BEGIN
	PERFORM set_config('pg_lion.enable_rows_correction',
					   CASE WHEN fix THEN 'on' ELSE 'off' END, true);
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	IF node IS NULL THEN
		r := (j->0->'Plan'->>'Plan Rows')::bigint;
	ELSE
		r := (regexp_match(j::text, '"Relation Name": "' || node ||
						   '",[^}]*"Plan Rows": (\d+)'))[1];
	END IF;
	PERFORM set_config('pg_lion.enable_rows_correction', 'on', true);
	RETURN r;
END $$;

CREATE FUNCTION lrc_actual(q text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	n	bigint;
BEGIN
	EXECUTE 'SELECT count(*) FROM (' || q || ') s' INTO n;
	RETURN n;
END $$;

-- 1: three clauses that follow h, and hold exactly h = 11's 1,000 rows; the
--    product of their selectivities is about 20.
-- 2: p and q are independent: the probe finds what core estimated, and the
--    estimate stays exactly as it was.
-- 3: the join of 1 with three rows of lrc_f for each of its rows.
-- 4: the same join, whose other side's rows are what is filtered.
CREATE TABLE lrc_q (n int, q text, node text);
INSERT INTO lrc_q VALUES
	(1, $$SELECT id FROM lrc WHERE c5 = 1 AND c25 = 5 AND c50 = 11$$, 'lrc'),
	(2, $$SELECT id FROM lrc WHERE p = 3 AND q = 5$$, 'lrc'),
	(3, $$SELECT f.v FROM lrc_f f JOIN lrc r ON f.rid = r.id WHERE r.c5 = 1 AND r.c25 = 5 AND r.c50 = 11$$, NULL),
	(4, $$SELECT f.v FROM lrc_f f WHERE f.rid IN (SELECT id FROM lrc WHERE c5 = 1 AND c25 = 5 AND c50 = 11)$$, NULL);
SELECT n, lrc_actual(q) AS actual,
	   lrc_est(q, true, node) BETWEEN lrc_actual(q) / 2 AND lrc_actual(q) * 2 AS corrected_within_2x,
	   lrc_est(q, false, node) BETWEEN lrc_actual(q) / 2 AND lrc_actual(q) * 2 AS uncorrected_within_2x,
	   lrc_est(q, true, node) = lrc_est(q, false, node) AS unchanged
  FROM lrc_q ORDER BY n;

-- With the probe off there is nothing to correct with.
SET pg_lion.enable_intersection_probe = off;
SELECT lrc_est(q, true, node) = lrc_est(q, false, node) AS unchanged
  FROM lrc_q WHERE n = 1;
RESET pg_lion.enable_intersection_probe;

-- A parameterized scan of lrc, a nested loop's inner side: its rows are
-- the filters with the join clause ANDed in, and take the probe's factor
-- before they are clamped to one row and capped at the relation's rows.
-- 5: a lookup on lrc's primary key returns a row at most, and is estimated
--    at one with the correction as without it: core's product is a small
--    fraction of a row, and that fraction times the probe's factor is still
--    one.
-- 6: a lookup on p, which h does not decide, returns a seventh of the
--    filters' rows, and is estimated at about that with the correction and
--    at one row without it.
SET enable_hashjoin = off;
SET enable_mergejoin = off;
INSERT INTO lrc_q VALUES
	(5, $$SELECT r.id FROM lrc_f f JOIN lrc r ON r.id = f.rid WHERE f.id = 10 AND r.c5 = 1 AND r.c25 = 5 AND r.c50 = 11$$, 'lrc'),
	(6, $$SELECT r.id FROM lrc_f f JOIN lrc r ON r.p = f.v WHERE f.id = 1 AND r.c5 = 1 AND r.c25 = 5 AND r.c50 = 11$$, 'lrc');
EXPLAIN (COSTS OFF)
SELECT r.id FROM lrc_f f JOIN lrc r ON r.id = f.rid
 WHERE f.id = 10 AND r.c5 = 1 AND r.c25 = 5 AND r.c50 = 11;
EXPLAIN (COSTS OFF)
SELECT r.id FROM lrc_f f JOIN lrc r ON r.p = f.v
 WHERE f.id = 1 AND r.c5 = 1 AND r.c25 = 5 AND r.c50 = 11;
SELECT n, lrc_actual(q) AS actual,
	   CASE WHEN n = 5 THEN lrc_est(q, true, node) END AS corrected,
	   lrc_est(q, true, node) BETWEEN lrc_actual(q) / 2 AND lrc_actual(q) * 2 AS corrected_within_2x,
	   lrc_est(q, false, node) AS uncorrected
  FROM lrc_q WHERE n IN (5, 6) ORDER BY n;
RESET enable_hashjoin;
RESET enable_mergejoin;

-- The tests of whether a lion-indexed array is empty (DESIGN.md §17,
-- "Non-empty arrays") correct the rows with no factor of the probe's: core
-- takes `array_length(tags, 1) IS NOT NULL` at 0.995 and `cardinality(tags)
-- > 0` at a third, and lion the two together as the share of the rows whose
-- array has an element, nine in ten of lrc_a's.  A parameterized scan takes
-- the same clauses (lion_param_rows()).
-- 7: a lookup on g, a thirty-first of the rows, with that filter, as a nested
--    loop's inner side;
-- 8: the same with g a constant.  7 is estimated as 8 is, at about the rows
--    both return, and both at about a third of them without the correction;
--    with the probe off they are corrected all the same.
CREATE TABLE lrc_a (id int PRIMARY KEY, g int NOT NULL, tags int[]);
INSERT INTO lrc_a
SELECT i, i % 31,
	   CASE i % 20 WHEN 0 THEN NULL WHEN 1 THEN '{}' WHEN 2 THEN '{NULL}'
		   ELSE ARRAY[i % 50, i % 7] END
  FROM generate_series(1, 30000) i;
CREATE INDEX lrc_a_g ON lrc_a (g);
CREATE INDEX lrc_a_tags ON lrc_a USING lion (tags);
VACUUM (FREEZE, ANALYZE) lrc_a;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
-- an index scan, whose own rows lrc_est() reads, not a bitmap scan's
SET enable_bitmapscan = off;
INSERT INTO lrc_q VALUES
	(7, $$SELECT r.id FROM lrc_f f JOIN lrc_a r ON r.g = f.v WHERE f.id = 1 AND array_length(r.tags, 1) IS NOT NULL AND cardinality(r.tags) > 0$$, 'lrc_a'),
	(8, $$SELECT id FROM lrc_a WHERE g = 1 AND array_length(tags, 1) IS NOT NULL AND cardinality(tags) > 0$$, 'lrc_a');
EXPLAIN (COSTS OFF)
SELECT r.id FROM lrc_f f JOIN lrc_a r ON r.g = f.v
 WHERE f.id = 1 AND array_length(r.tags, 1) IS NOT NULL AND cardinality(r.tags) > 0;
SELECT n, lrc_actual(q) AS actual,
	   lrc_est(q, true, node) BETWEEN lrc_actual(q) * 0.9 AND lrc_actual(q) * 1.1 AS corrected_within_10pct,
	   lrc_est(q, false, node) BETWEEN lrc_actual(q) / 2 AND lrc_actual(q) * 2 AS uncorrected_within_2x
  FROM lrc_q WHERE n IN (7, 8) ORDER BY n;
SELECT abs(p.e - c.e) <= 0.02 * c.e AS parameterized_as_constant
  FROM (SELECT lrc_est(q, true, node) AS e FROM lrc_q WHERE n = 7) p,
	   (SELECT lrc_est(q, true, node) AS e FROM lrc_q WHERE n = 8) c;
SET pg_lion.enable_intersection_probe = off;
SELECT n, lrc_est(q, true, node) BETWEEN lrc_actual(q) * 0.9 AND lrc_actual(q) * 1.1 AS corrected_within_10pct
  FROM lrc_q WHERE n IN (7, 8) ORDER BY n;
RESET pg_lion.enable_intersection_probe;
RESET enable_bitmapscan;
RESET enable_hashjoin;
RESET enable_mergejoin;

-- The answers do not depend on it.
SELECT count(*), sum(f.v) FROM lrc_f f JOIN lrc r ON f.rid = r.id
 WHERE r.c5 = 1 AND r.c25 = 5 AND r.c50 = 11;
SET pg_lion.enable_rows_correction = off;
SELECT count(*), sum(f.v) FROM lrc_f f JOIN lrc r ON f.rid = r.id
 WHERE r.c5 = 1 AND r.c25 = 5 AND r.c50 = 11;
RESET pg_lion.enable_rows_correction;

DROP TABLE lrc, lrc_f, lrc_a, lrc_q;
DROP FUNCTION lrc_est(text, boolean, text), lrc_actual(text);
RESET default_statistics_target;
