-- bench/slices_pushdown.sql - the measurements of DESIGN.md §52: the
-- aggregates of a bit-sliced column answered from its slices by the
-- LionCount node, against the ordinary plan and against the other answers
-- lion has for them (the aggregates over keys of §37), and the plane kernel
-- against lion_container_and_cardinality().
--
--   psql -X -v rows=5000000 -f bench/slices_pushdown.sql            (setup, then all)
--   psql -X -v setup=off -f bench/slices_pushdown.sql               (the table kept)
--
-- Into a scratch database, on a release build, with nothing else running;
-- every timing is the best of :reps runs of a statement, clock_timestamp()
-- around it in a DO block, warm; every answer is compared with the ordinary
-- plan's (pg_lion.enable_count_pushdown off).  Results go to slp_bench and
-- are printed at the end; the table is left for further runs.
\set ON_ERROR_STOP on
\if :{?rows}
\else
\set rows 5000000
\endif
\if :{?reps}
\else
\set reps 3
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
-- The columns of bench/slices.sql - u4 and u8 uniform over the type, k4
-- skewed (power(random(), 4) times 10^6), s4 the row number - and filters of
-- 1%, 10% and 50% (g100, g10, g2), grouping columns of 2, 20 and 200 values
-- (h2, h20, h200), and r, uniform over 0 .. 10^6, with summaries, for
-- ranges.
DROP TABLE IF EXISTS slp;
SELECT setseed(0.42);
CREATE TABLE slp (id int, g100 int, g10 int, g2 int, h2 int, h20 int,
				  h200 int, r int4, u4 int4, k4 int4, s4 int4, u8 int8)
	WITH (autovacuum_enabled = off);
INSERT INTO slp
SELECT i, floor(random() * 100), floor(random() * 10), floor(random() * 2),
	   floor(random() * 2), floor(random() * 20), floor(random() * 200),
	   floor(random() * 1000000),
	   (floor(random() * 4294967296) - 2147483648)::int8::int4,
	   floor(power(random(), 4) * 1000000)::int4,
	   i,
	   (((floor(random() * 4294967296) - 2147483648)::int8 << 32) | floor(random() * 4294967296)::int8)
  FROM generate_series(1, :rows) i;
CREATE INDEX slp_g ON slp USING lion (g100, g10, g2);
CREATE INDEX slp_h ON slp USING lion (h2, h20, h200);
CREATE INDEX slp_r ON slp USING lion (r) WITH (summaries = auto);
CREATE INDEX slp_u4 ON slp USING lion (u4) WITH (slices = on);
CREATE INDEX slp_k4 ON slp USING lion (k4) WITH (slices = on);
CREATE INDEX slp_u8 ON slp USING lion (u8) WITH (slices = on);
VACUUM (FREEZE, ANALYZE) slp;
VACUUM (FREEZE, ANALYZE) slp;
\endif

DROP TABLE IF EXISTS slp_bench;
CREATE TABLE slp_bench (shape text, query text, plan text, ms numeric,
						lion_plan text, lion_ms numeric, nokernel_ms numeric,
						lion_cost numeric, ordinary_plan text,
						ordinary_ms numeric, ordinary_cost numeric);

-- The plan's top node and the node below an Aggregate, marked with what the
-- LionCount node answers the aggregates by, and its total cost.
CREATE OR REPLACE FUNCTION slp_plan(q text, OUT plan text, OUT cost numeric)
LANGUAGE plpgsql AS $$
DECLARE
	j json;
	p json;
	ln text;
BEGIN
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	p := j -> 0 -> 'Plan';
	cost := (p ->> 'Total Cost')::numeric;
	plan := coalesce(p ->> 'Custom Plan Provider', p ->> 'Node Type');
	WHILE p -> 'Plans' IS NOT NULL AND plan ~ '(Aggregate|Gather|Sort|Limit)$'
	LOOP
		p := p -> 'Plans' -> 0;
		plan := plan || ' > ' || coalesce(p ->> 'Custom Plan Provider', p ->> 'Node Type');
	END LOOP;
	FOR ln IN EXECUTE 'EXPLAIN ' || q LOOP
		IF ln ~ 'Aggregates From Slices' THEN
			plan := plan || ' (slices)';
		ELSIF ln ~ 'Aggregates Over Keys' THEN
			plan := plan || ' (keys)';
		END IF;
	END LOOP;
END $$;

-- The best of reps runs of q, ms, and its rows as text.
CREATE OR REPLACE FUNCTION slp_time(q text, reps int, OUT ms numeric,
									OUT answer text)
LANGUAGE plpgsql AS $$
DECLARE
	t0 timestamptz;
	r int;
BEGIN
	FOR r IN 1 .. reps LOOP
		t0 := clock_timestamp();
		EXECUTE format('SELECT string_agg(x::text, '' '' ORDER BY x::text) FROM (%s) x', q)
			INTO answer;
		ms := least(ms, extract(epoch FROM clock_timestamp() - t0) * 1000);
	END LOOP;
	ms := round(ms, 1);
END $$;

-- One shape: the plan the planner picks; the LionCount node, preferred over
-- every other scan, with the plane kernel and without; the ordinary plan
-- (the pushdown off) - timed, their answers compared.
CREATE OR REPLACE PROCEDURE slp_run(shape text, q text, reps int)
LANGUAGE plpgsql AS $$
DECLARE
	p record;
	a record;
	lp record;
	l record;
	c record;
	o record;
	b record;
BEGIN
	SELECT * INTO p FROM slp_plan(q);
	SELECT * INTO a FROM slp_time(q, reps);
	SET LOCAL enable_seqscan = off;
	SET LOCAL enable_bitmapscan = off;
	SET LOCAL pg_lion.enable_plain_scan = off;
	SELECT * INTO lp FROM slp_plan(q);
	SELECT * INTO l FROM slp_time(q, reps);
	SET LOCAL pg_lion.enable_slice_kernel = off;
	SELECT * INTO c FROM slp_time(q, reps);
	RESET pg_lion.enable_slice_kernel;
	RESET enable_seqscan;
	RESET enable_bitmapscan;
	RESET pg_lion.enable_plain_scan;
	SET LOCAL pg_lion.enable_count_pushdown = off;
	SELECT * INTO o FROM slp_plan(q);
	SELECT * INTO b FROM slp_time(q, reps);
	RESET pg_lion.enable_count_pushdown;
	IF a.answer IS DISTINCT FROM b.answer OR c.answer IS DISTINCT FROM b.answer OR
	   l.answer IS DISTINCT FROM b.answer THEN
		RAISE EXCEPTION 'different answers for %: % / % / % / %', q, a.answer,
			l.answer, c.answer, b.answer;
	END IF;
	INSERT INTO slp_bench VALUES (shape, q, p.plan, a.ms, lp.plan, l.ms, c.ms,
								  round(lp.cost), o.plan, b.ms, round(o.cost));
END $$;

\if :{?only}
\else
\set only all
\endif
SELECT :'only' IN ('all', '1') AS do1, :'only' IN ('all', '2') AS do2 \gset

\if :do1
-- Level 1: the aggregates under a WHERE of 1%, 10% and 50%, and none.
CALL slp_run('1 u4 sum,count 1%', 'SELECT sum(u4), count(u4) FROM slp WHERE g100 = 7', :reps);
CALL slp_run('1 u4 sum,count 10%', 'SELECT sum(u4), count(u4) FROM slp WHERE g10 = 3', :reps);
CALL slp_run('1 u4 sum,count 50%', 'SELECT sum(u4), count(u4) FROM slp WHERE g2 = 1', :reps);
CALL slp_run('1 u4 all 1%', 'SELECT sum(u4), avg(u4), min(u4), max(u4), count(*) FROM slp WHERE g100 = 7', :reps);
CALL slp_run('1 u4 all 10%', 'SELECT sum(u4), avg(u4), min(u4), max(u4), count(*) FROM slp WHERE g10 = 3', :reps);
CALL slp_run('1 u4 all 50%', 'SELECT sum(u4), avg(u4), min(u4), max(u4), count(*) FROM slp WHERE g2 = 1', :reps);
CALL slp_run('1 u4 max 10%', 'SELECT max(u4) FROM slp WHERE g10 = 3', :reps);
CALL slp_run('1 u8 all 1%', 'SELECT sum(u8), avg(u8), min(u8), max(u8) FROM slp WHERE g100 = 7', :reps);
CALL slp_run('1 u8 all 10%', 'SELECT sum(u8), avg(u8), min(u8), max(u8) FROM slp WHERE g10 = 3', :reps);
CALL slp_run('1 u8 all 50%', 'SELECT sum(u8), avg(u8), min(u8), max(u8) FROM slp WHERE g2 = 1', :reps);
CALL slp_run('1 k4 all 1%', 'SELECT sum(k4), avg(k4), min(k4), max(k4) FROM slp WHERE g100 = 7', :reps);
CALL slp_run('1 k4 all 10%', 'SELECT sum(k4), avg(k4), min(k4), max(k4) FROM slp WHERE g10 = 3', :reps);
CALL slp_run('1 k4 all 50%', 'SELECT sum(k4), avg(k4), min(k4), max(k4) FROM slp WHERE g2 = 1', :reps);
CALL slp_run('1 u4 sum range 10%', 'SELECT sum(u4), count(*) FROM slp WHERE r < 100000', :reps);
CALL slp_run('1 u4 sum IN 2%', 'SELECT sum(u4) FROM slp WHERE g100 IN (3, 4)', :reps);
CALL slp_run('1 u4 sum no WHERE', 'SELECT sum(u4), max(u4) FROM slp', :reps);
CALL slp_run('1 k4 sum no WHERE', 'SELECT sum(k4), max(k4) FROM slp', :reps);
\endif

\if :do2
-- Level 2: GROUP BY 2, 20 and 200 groups, under a WHERE of 10% and none,
-- and the pairs of two columns.
CALL slp_run('2 u4 all 10% by 2', 'SELECT h2, sum(u4), min(u4), max(u4), count(*) FROM slp WHERE g10 = 3 GROUP BY h2', :reps);
CALL slp_run('2 u4 all 10% by 20', 'SELECT h20, sum(u4), min(u4), max(u4), count(*) FROM slp WHERE g10 = 3 GROUP BY h20', :reps);
CALL slp_run('2 u4 all 10% by 200', 'SELECT h200, sum(u4), min(u4), max(u4), count(*) FROM slp WHERE g10 = 3 GROUP BY h200', :reps);
CALL slp_run('2 u4 sum 10% by 2', 'SELECT h2, sum(u4), count(*) FROM slp WHERE g10 = 3 GROUP BY h2', :reps);
CALL slp_run('2 u4 sum 10% by 20', 'SELECT h20, sum(u4), count(*) FROM slp WHERE g10 = 3 GROUP BY h20', :reps);
CALL slp_run('2 u4 sum 10% by 200', 'SELECT h200, sum(u4), count(*) FROM slp WHERE g10 = 3 GROUP BY h200', :reps);
CALL slp_run('2 u4 max 1% by 20', 'SELECT h20, max(u4) FROM slp WHERE g100 = 7 GROUP BY h20', :reps);
CALL slp_run('2 u4 sum all by 2', 'SELECT h2, sum(u4) FROM slp GROUP BY h2', :reps);
CALL slp_run('2 u4 sum all by 20', 'SELECT h20, sum(u4) FROM slp GROUP BY h20', :reps);
CALL slp_run('2 u4 sum all by 200', 'SELECT h200, sum(u4) FROM slp GROUP BY h200', :reps);
CALL slp_run('2 k4 all 10% by 20', 'SELECT h20, sum(k4), min(k4), max(k4) FROM slp WHERE g10 = 3 GROUP BY h20', :reps);
CALL slp_run('2 u4 sum 10% pairs 2x20', 'SELECT h2, h20, sum(u4), count(*) FROM slp WHERE g10 = 3 GROUP BY h2, h20', :reps);
CALL slp_run('2 u4 sum 50% pairs 10x20', 'SELECT g10, h20, sum(u4) FROM slp WHERE g2 = 1 GROUP BY g10, h20', :reps);
\endif

SELECT shape, plan, ms, lion_plan, lion_ms, nokernel_ms, lion_cost,
	   ordinary_plan, ordinary_ms, ordinary_cost
  FROM slp_bench ORDER BY shape;
