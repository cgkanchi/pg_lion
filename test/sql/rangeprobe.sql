-- The endpoint probe (DESIGN.md §28, "The endpoint probe").
--
-- A range bound past the ends of a column's histogram is estimated by core
-- at a hundredth of one bin, however many rows have been added past that end
-- since the last ANALYZE.  Core reads the column's actual first or last value
-- from a btree to correct that (get_actual_variable_range()), and a column
-- indexed by lion alone never got the correction: the count pushdown walked
-- tens of thousands of entries it had priced at sixty.  Lion's own estimates
-- now read the ends from lion's directory, where core would read them, and
-- core's estimates stay core's.
--
-- The tables are sampled whole by ANALYZE, so their statistics - and the
-- plans - are the same on every run: statistics target 5, 1500 rows, and a
-- histogram of five bins, whose last one holds a fifth of the rows ANALYZE saw.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

/*
 * lpr_rows() is the row estimate of the scan node of q that uses index ix, or
 * of the relation's scan when ix is NULL: a lion bitmap index scan's rows are
 * lion's own estimate (amcostestimate()'s selectivity), a sequential scan's
 * core's.
 */
CREATE FUNCTION lpr_rows(q text, ix text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	j	json;
	r	bigint;
BEGIN
	IF ix IS NULL THEN
		PERFORM set_config('enable_indexscan', 'off', true);
		PERFORM set_config('enable_bitmapscan', 'off', true);
	ELSE
		PERFORM set_config('enable_seqscan', 'off', true);
		PERFORM set_config('enable_indexscan', 'off', true);
	END IF;
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	IF ix IS NULL THEN
		r := (regexp_match(j::text, '"Node Type": "Seq Scan",[^}]*"Plan Rows": (\d+)'))[1];
	ELSE
		r := (regexp_match(j::text, '"Node Type": "Bitmap Index Scan",[^}]*"Index Name": "' ||
						   ix || '",[^}]*"Plan Rows": (\d+)'))[1];
	END IF;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	RETURN r;
END $$;

/* The scan nodes of a plan with nothing disabled, on one line. */
CREATE FUNCTION lpr_plan(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln	text;
	res	text := '';
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'Scan' THEN
			res := res || CASE WHEN res = '' THEN '' ELSE ' / ' END ||
				   btrim(regexp_replace(ln, '->', ''));
		END IF;
	END LOOP;
	RETURN res;
END $$;

-- 1500 rows, a minute apart from 2026-01-01, analyzed; then 28,500 newer
-- ones, a minute apart to 2026-01-21 20:00, and not analyzed.  v is the same
-- the other way round: 1..1500 analyzed, then -28500..-1.
CREATE TABLE lpr (id int, ts timestamptz, c int, v int)
	WITH (autovacuum_enabled = off);
ALTER TABLE lpr ALTER id SET STATISTICS 5, ALTER ts SET STATISTICS 5,
	ALTER c SET STATISTICS 5, ALTER v SET STATISTICS 5;
INSERT INTO lpr SELECT g, '2026-01-01'::timestamptz + g * interval '1 minute',
	   g % 10, g
  FROM generate_series(1, 1500) g;
CREATE INDEX lpr_lion ON lpr USING lion (ts, c, v);
VACUUM (ANALYZE) lpr;
INSERT INTO lpr SELECT g, '2026-01-01'::timestamptz + g * interval '1 minute',
	   g % 10, 1500 - g
  FROM generate_series(1501, 30000) g;
VACUUM lpr;

-- ---------- 1. lion's estimate reads the ends; core's does not ----------
-- past the last end, 28,441 rows
SELECT count(*) AS actual,
	   lpr_rows($$SELECT * FROM lpr WHERE ts >= '2026-01-02 02:00'$$, NULL) < 100 AS core_below_100,
	   lpr_rows($$SELECT * FROM lpr WHERE ts >= '2026-01-02 02:00'$$, 'lpr_lion') > 3000 AS lion_above_3000
  FROM lpr WHERE ts >= '2026-01-02 02:00';
-- before the first end, a column that is not the index's first, cross-type
SELECT count(*) AS actual,
	   lpr_rows($$SELECT * FROM lpr WHERE v < 0::bigint$$, NULL) < 100 AS core_below_100,
	   lpr_rows($$SELECT * FROM lpr WHERE v < 0::bigint$$, 'lpr_lion') > 3000 AS lion_above_3000
  FROM lpr WHERE v < 0::bigint;
-- a bound inside the histogram is left as it is: core would not probe
SELECT lpr_rows($$SELECT * FROM lpr WHERE ts >= '2026-01-01 12:00'$$, NULL) =
	   lpr_rows($$SELECT * FROM lpr WHERE ts >= '2026-01-01 12:00'$$, 'lpr_lion') AS same;

-- ---------- 2. the plans that follow ----------
-- The count pushdown walked every entry past the end, priced as sixty of
-- them; a sequential scan reads the 30,000 rows in a fraction of that time.
SELECT lpr_plan($$SELECT count(*) FROM lpr WHERE ts >= '2026-01-02 02:00'$$);
SELECT lpr_plan($$SELECT sum(id) FROM lpr WHERE ts >= '2026-01-02 02:00'$$);
SELECT lpr_plan($$SELECT count(*) FROM lpr WHERE v < 0$$);
-- a selective equality beside the range keeps lion's scan
SELECT lpr_plan($$SELECT sum(id) FROM lpr WHERE ts >= '2026-01-02 02:00' AND c = 0 AND v = -700$$);
-- and the answers are the sequential scan's
SELECT count(*), sum(id) FROM lpr WHERE ts >= '2026-01-02 02:00';
SELECT count(*), sum(id) FROM lpr WHERE ts >= '2026-01-02 02:00' AND c = 0 AND v = -700;
SET enable_seqscan = off;
SELECT count(*) FROM lpr WHERE ts >= '2026-01-02 02:00';
SET enable_indexscan = off;
SET pg_lion.enable_count_pushdown = off;
SELECT count(*), sum(id) FROM lpr WHERE ts >= '2026-01-02 02:00';
SELECT count(*), sum(id) FROM lpr WHERE ts >= '2026-01-02 02:00' AND c = 0 AND v = -700;
RESET enable_seqscan;
RESET enable_indexscan;
RESET pg_lion.enable_count_pushdown;

-- ---------- 3. an end whose rows are deleted is not an end ----------
-- The newest 1,000 rows go.  Their entries stay until VACUUM, and once it has
-- run their leaves stay, empty and linked (DESIGN.md §18).  Either way the
-- probe steps over them to the live end, 2026-01-21 03:20, which is where
-- core finds it with a btree: lion's estimate from the directory alone and
-- core's from the btree are the same number, before VACUUM and after.  Each
-- lion estimate is taken right after the btree's, with the btree dropped in
-- between: building it rewrites the table's reltuples on 16, and both numbers
-- have to be scaled by the same one.
CREATE TABLE lpr_est (step text, est bigint);
CREATE FUNCTION lpr_note(step text, ix text) RETURNS void
LANGUAGE sql AS $$
	INSERT INTO lpr_est
		SELECT step, lpr_rows('SELECT * FROM lpr WHERE ts >= ''2026-01-10''', ix)
$$;
SELECT lpr_note('histogram', NULL);
DELETE FROM lpr WHERE id > 29000;
CREATE INDEX lpr_bt ON lpr (ts);
SELECT lpr_note('deleted, btree', NULL);
DROP INDEX lpr_bt;
SELECT lpr_note('deleted, lion', 'lpr_lion');
VACUUM lpr;
CREATE INDEX lpr_bt ON lpr (ts);
SELECT lpr_note('vacuumed, btree', NULL);
DROP INDEX lpr_bt;
SELECT lpr_note('vacuumed, lion', 'lpr_lion');
-- 9,000 more, and their leaves - some 300 of this column's, an index that
-- grew by inserts holding about 34 of its keys to a leaf - are more than the
-- probe reads (LION_PROBE_LEAVES): it gives up, and the histogram's own end
-- stands, as it does for core's probe when that gives up.
DELETE FROM lpr WHERE id > 20000;
VACUUM lpr;
SELECT lpr_note('too many, lion', 'lpr_lion');
SELECT lpr_note('too many, core', NULL);
SELECT (SELECT est FROM lpr_est WHERE step = 'deleted, lion') =
	   (SELECT est FROM lpr_est WHERE step = 'deleted, btree') AS deleted_as_btree,
	   (SELECT est FROM lpr_est WHERE step = 'vacuumed, lion') =
	   (SELECT est FROM lpr_est WHERE step = 'vacuumed, btree') AS vacuumed_as_btree,
	   (SELECT est FROM lpr_est WHERE step = 'vacuumed, lion') >
	   10 * (SELECT est FROM lpr_est WHERE step = 'histogram') AS probed,
	   (SELECT est FROM lpr_est WHERE step = 'too many, lion') =
	   (SELECT est FROM lpr_est WHERE step = 'too many, core') AS given_up;
SELECT count(*) FROM lpr WHERE ts >= '2026-01-10';

-- ---------- 4. what is not probed ----------
-- A partial lion index holds only some of the column: its ends are not the
-- column's.
DROP INDEX lpr_lion;
CREATE INDEX lpr_part ON lpr USING lion (ts) WHERE c < 5;
SELECT lpr_rows($$SELECT * FROM lpr WHERE ts >= '2026-01-10' AND c < 5$$, NULL) =
	   lpr_rows($$SELECT * FROM lpr WHERE ts >= '2026-01-10' AND c < 5$$, 'lpr_part') AS same;
-- A Param has no value to compare with the histogram at plan time.
CREATE INDEX lpr_lion ON lpr USING lion (ts, c, v);
PREPARE lpr_p(timestamptz) AS SELECT count(*) FROM lpr WHERE ts >= $1;
SET plan_cache_mode = force_generic_plan;
EXECUTE lpr_p('2026-01-02 02:00');
RESET plan_cache_mode;
DEALLOCATE lpr_p;

DROP TABLE lpr, lpr_est;
DROP FUNCTION lpr_note(text, text);
DROP FUNCTION lpr_rows(text, text);
DROP FUNCTION lpr_plan(text);
