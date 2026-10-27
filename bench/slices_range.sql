-- bench/slices_range.sql - the measurements of DESIGN.md §53, "Measured":
-- ranges on a bit-sliced column answered from its slices, against the same
-- ranges walked over the column's entries (§28), summed from its summaries
-- (§32) and answered by the ordinary plan (a btree beside), at 0.1% to 90%
-- of the rows; and sum(x) under a range on x, end to end (§52).
--
--   psql -X -v rows=5000000 -f bench/slices_range.sql      (a scratch database)
--   psql -X -v setup=off -f bench/slices_range.sql          (again, same data)
--
-- Every timing is the median of :reps runs of the statement, the shapes taken
-- in turn so that a burst of load on a shared machine falls on all of them
-- alike; clock_timestamp() around each in a DO block, warm.  Every answer is
-- compared with the ordinary plan's.  Results go to slr_res, printed at the
-- end.
\set ON_ERROR_STOP on
\if :{?rows}
\else
\set rows 5000000
\endif
\if :{?reps}
\else
\set reps 5
\endif
\if :{?setup}
\else
\set setup on
\endif
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET maintenance_work_mem = '1GB';
SET work_mem = '64MB';

\if :setup
DROP TABLE IF EXISTS slr_b, slr_res, slr_q;
-- u4, u8: uniform over the type (an int8's two halves drawn apart, random()
-- having 52 bits); s4, s8: the row number (times 1,000,003), in heap order;
-- t8: a timestamp's microseconds, a row a second with a second's jitter, in
-- heap order; g100 a filter of 1%.
SELECT setseed(0.53);
CREATE TABLE slr_b (id int, g100 int, u4 int4, s4 int4, u8 int8, s8 int8,
					t8 int8)
	WITH (autovacuum_enabled = off);
INSERT INTO slr_b
SELECT i, floor(random() * 100),
	   (floor(random() * 4294967296) - 2147483648)::int8::int4,
	   i,
	   (((floor(random() * 4294967296) - 2147483648)::int8 << 32) | floor(random() * 4294967296)::int8),
	   i::int8 * 1000003,
	   1700000000000000 + i::int8 * 1000000 + floor(random() * 1000000)::int8
  FROM generate_series(1, :rows) i;
CREATE INDEX slr_b_g ON slr_b USING lion (g100);
VACUUM (FREEZE, ANALYZE) slr_b;

CREATE TABLE slr_res (phase text, col text, pct numeric, shape text,
					  ms numeric, cost numeric, plan text, ok bool);

-- The range of each fraction, as BETWEEN lo AND hi centred in the column's
-- values: the type's range for a uniform column, the rows' for the others.
CREATE TABLE slr_q (col text, pct numeric, lo numeric, hi numeric);
INSERT INTO slr_q
SELECT c.col, p.pct,
	   c.lo + (c.hi - c.lo) * (1 - p.pct / 100) / 2,
	   c.lo + (c.hi - c.lo) * (1 + p.pct / 100) / 2
  FROM (VALUES ('u4', -2147483648::numeric, 2147483647::numeric),
			   ('s4', 1, :rows),
			   ('u8', -9223372036854775808::numeric, 9223372036854775807::numeric),
			   ('s8', 1000003, :rows * 1000003::numeric),
			   ('t8', 1700000000000000 + 1000000::numeric,
				1700000000000000 + :rows * 1000000::numeric)) c(col, lo, hi),
	   (VALUES (0.1), (1), (10), (50), (90)) p(pct);
UPDATE slr_q SET lo = floor(lo), hi = floor(hi);
\endif

/*
 * One statement's time, in ms, and its plan's total cost and shape, under the
 * settings given (name=value, ...).
 */
CREATE OR REPLACE FUNCTION slr_time(q text, sets text[], OUT ms numeric,
									OUT cost numeric, OUT plan text,
									OUT answer text)
LANGUAGE plpgsql AS $$
DECLARE
	s text;
	t0 timestamptz;
	j json;
	olds text[] := '{}';
BEGIN
	FOREACH s IN ARRAY sets LOOP
		olds := olds || (split_part(s, '=', 1) || '=' ||
						 current_setting(split_part(s, '=', 1)));
		PERFORM set_config(split_part(s, '=', 1), split_part(s, '=', 2), true);
	END LOOP;
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	cost := round((j -> 0 -> 'Plan' ->> 'Total Cost')::numeric);
	plan := j -> 0 -> 'Plan' ->> 'Node Type';
	IF plan = 'Custom Scan' THEN
		plan := (j -> 0 -> 'Plan' ->> 'Custom Plan Provider') ||
			CASE WHEN (j -> 0 -> 'Plan')::jsonb ? 'Range From Slices' THEN ' (slices)'
				 ELSE '' END;
	ELSE
		plan := plan || coalesce(' > ' || (j -> 0 -> 'Plan' -> 'Plans' -> 0 ->> 'Node Type'), '');
	END IF;
	t0 := clock_timestamp();
	EXECUTE 'SELECT (' || q || ')::text' INTO answer;
	ms := round(extract(epoch FROM clock_timestamp() - t0) * 1000, 2);
	FOREACH s IN ARRAY olds LOOP
		PERFORM set_config(split_part(s, '=', 1), split_part(s, '=', 2), true);
	END LOOP;
END $$;

/*
 * Each shape of each query, :reps times, the shapes in turn, the median of
 * each kept: names[i]
 * run under the settings sets[i][...].  The answer is compared with the
 * ordinary plan's (the pushdown off).
 */
CREATE TEMP TABLE slr_one (sh int, ms numeric, cost numeric, plan text,
						   ok bool);
CREATE OR REPLACE PROCEDURE slr_run(phase text, agg text, reps int,
									names text[], sets text[])
LANGUAGE plpgsql AS $$
DECLARE
	r record;
	q text;
	k int;
	sh int;
	times numeric[];
	t record;
	ref text;
BEGIN
	FOR r IN SELECT * FROM slr_q ORDER BY col, pct LOOP
		q := format('SELECT %s FROM slr_b WHERE %I BETWEEN %s AND %s',
					replace(agg, 'X', r.col), r.col, r.lo, r.hi);
		SELECT answer INTO ref
		  FROM slr_time(q, ARRAY['pg_lion.enable_count_pushdown=off']);
		DELETE FROM slr_one;
		FOR k IN 1 .. reps LOOP
			FOR sh IN 1 .. array_length(names, 1) LOOP
				SELECT * INTO t FROM slr_time(q, sets[sh:sh]);
				INSERT INTO slr_one VALUES (sh, t.ms, t.cost, t.plan,
											t.answer = ref);
			END LOOP;
		END LOOP;
		INSERT INTO slr_res
		SELECT phase, r.col, r.pct, names[o.sh],
			   percentile_disc(0.5) WITHIN GROUP (ORDER BY o.ms),
			   max(o.cost), max(o.plan), bool_and(o.ok)
		  FROM slr_one o GROUP BY o.sh;
	END LOOP;
END $$;

/* the shapes: the ranges from slices, and the index's other way */
\set lionsets 'ARRAY[ARRAY[''pg_lion.prefer_slice_ranges=on'', ''enable_seqscan=off'', ''enable_bitmapscan=off'', ''pg_lion.enable_plain_scan=off''], ARRAY[''pg_lion.enable_slice_ranges=off'', ''enable_seqscan=off'', ''enable_bitmapscan=off'', ''pg_lion.enable_plain_scan=off''], ARRAY[''pg_lion.enable_count_pushdown=on'', ''enable_seqscan=on'', ''enable_bitmapscan=on'', ''pg_lion.enable_plain_scan=on''], ARRAY[''pg_lion.enable_count_pushdown=off'', ''enable_seqscan=on'', ''enable_bitmapscan=on'', ''pg_lion.enable_plain_scan=on'']]'

-- Phase 1: one-column lion indexes with slices and no summaries - the
-- ranges from slices against the walk of their entries.
DROP INDEX IF EXISTS slr_b_u4, slr_b_s4, slr_b_u8, slr_b_s8, slr_b_t8;
CREATE INDEX slr_b_u4 ON slr_b USING lion (u4) WITH (slices = on);
CREATE INDEX slr_b_s4 ON slr_b USING lion (s4) WITH (slices = on);
CREATE INDEX slr_b_u8 ON slr_b USING lion (u8) WITH (slices = on);
CREATE INDEX slr_b_s8 ON slr_b USING lion (s8) WITH (slices = on);
CREATE INDEX slr_b_t8 ON slr_b USING lion (t8) WITH (slices = on);
VACUUM ANALYZE slr_b;
DELETE FROM slr_res WHERE phase IN ('walk', 'walk sum');
CALL slr_run('walk', 'count(*)', :reps,
			 ARRAY['slices', 'walk', 'plan', 'ordinary'], :lionsets);

-- Phase 2: the same with summaries too - the ranges from slices against the
-- summaries; and sum(x) under the range, end to end.
DROP INDEX slr_b_u4, slr_b_s4, slr_b_u8, slr_b_s8, slr_b_t8;
CREATE INDEX slr_b_u4 ON slr_b USING lion (u4) WITH (slices = on, summaries = on);
CREATE INDEX slr_b_s4 ON slr_b USING lion (s4) WITH (slices = on, summaries = on);
CREATE INDEX slr_b_u8 ON slr_b USING lion (u8) WITH (slices = on, summaries = on);
CREATE INDEX slr_b_s8 ON slr_b USING lion (s8) WITH (slices = on, summaries = on);
CREATE INDEX slr_b_t8 ON slr_b USING lion (t8) WITH (slices = on, summaries = on);
VACUUM ANALYZE slr_b;
DELETE FROM slr_res WHERE phase IN ('summaries', 'summaries sum');
CALL slr_run('summaries', 'count(*)', :reps,
			 ARRAY['slices', 'summaries', 'plan', 'ordinary'], :lionsets);
CALL slr_run('summaries sum', 'sum(X)', :reps,
			 ARRAY['slices', 'summaries', 'plan', 'ordinary'], :lionsets);

-- Phase 3: a btree on each column beside - core's index-only scan - and the
-- planner's choice between it and the lion paths.
CREATE INDEX slr_b_bt_u4 ON slr_b (u4);
CREATE INDEX slr_b_bt_s4 ON slr_b (s4);
CREATE INDEX slr_b_bt_u8 ON slr_b (u8);
CREATE INDEX slr_b_bt_s8 ON slr_b (s8);
CREATE INDEX slr_b_bt_t8 ON slr_b (t8);
VACUUM ANALYZE slr_b;
DELETE FROM slr_res WHERE phase IN ('btree', 'btree sum');
CALL slr_run('btree', 'count(*)', :reps, ARRAY['btree', 'plan'],
			 ARRAY[ARRAY['pg_lion.enable_count_pushdown=off', 'enable_seqscan=on'],
				   ARRAY['pg_lion.enable_count_pushdown=on', 'enable_seqscan=on']]);
CALL slr_run('btree sum', 'sum(X)', :reps, ARRAY['btree', 'plan'],
			 ARRAY[ARRAY['pg_lion.enable_count_pushdown=off', 'enable_seqscan=on'],
				   ARRAY['pg_lion.enable_count_pushdown=on', 'enable_seqscan=on']]);
DROP INDEX slr_b_bt_u4, slr_b_bt_s4, slr_b_bt_u8, slr_b_bt_s8, slr_b_bt_t8;

SELECT phase, col, pct, shape, ms, cost, plan, ok
  FROM slr_res ORDER BY phase, col, pct, shape;
