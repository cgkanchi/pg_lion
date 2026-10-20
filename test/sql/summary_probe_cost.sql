-- A summed range priced as the executor runs it (DESIGN.md §32, "Summed
-- ranges: dense and probed", "The price of a probe that does not fit").
--
-- The shape: a window on a near-unique timestamp whose rows lie all over the
-- heap, with summaries, beside an equality and a multi-key overlap.  Each
-- bucket's summary then has a member or two at nearly every container key,
-- and counting it beside the other two sources is a merge of all of those
-- keys with them.  The walk turns to PROBING the rest of its sets at the
-- other sources' rows once its counts have visited twice as many containers
-- as the smallest of those sources has - when a probe of that source fits
-- work_mem.  When it does not, every summary is counted, several times the
-- work, and a plain scan of the same index is the cheaper plan.  The planner
-- asks the executor's question (lion_range_probe_fits()) of its estimate of
-- the same source, so the price of the count follows what will run: at the
-- floor of work_mem the plain scan is chosen, and at twice that the count,
-- which probes.  Before 2026-10-19 the planner asked whether F's rows at two
-- bytes each fit work_mem - F being the intersection, estimated as if the
-- two sources were independent - and priced the count probed at a work_mem
-- the executor then counted every summary at.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

/*
 * lion_spc() plans q as the planner likes and reports the plan's top scan
 * (chosen); then runs it with the count pushdown and every other scan
 * disabled - probed is EXPLAIN ANALYZE's "Range Walks Probed", visited its
 * "Containers Visited", cost the count's price - and again as a sequential
 * scan with the pushdown off.  same: the count was pushed down and the
 * answers agree.
 */
CREATE FUNCTION lion_spc(q text, OUT chosen text, OUT same boolean,
						 OUT probed boolean, OUT visited bigint,
						 OUT cost float8)
LANGUAGE plpgsql AS $$
DECLARE
	j jsonb;
	node jsonb;
	got text;
	want text;
BEGIN
	EXECUTE 'EXPLAIN (COSTS OFF, FORMAT JSON) ' || q INTO j;
	node := j -> 0 -> 'Plan';
	WHILE node -> 'Plans' IS NOT NULL AND node ->> 'Custom Plan Provider' IS NULL LOOP
		node := node -> 'Plans' -> 0;
	END LOOP;
	chosen := coalesce(node ->> 'Custom Plan Provider', node ->> 'Node Type');

	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	EXECUTE 'EXPLAIN (ANALYZE, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) '
		|| q INTO j;
	node := jsonb_path_query_first(j, 'strict $.** ? (@."Custom Plan Provider" == "LionCount")');
	probed := coalesce((node ->> 'Range Walks Probed')::int, 0) > 0;
	visited := (node ->> 'Containers Visited')::bigint;
	cost := (node ->> 'Total Cost')::float8;
	EXECUTE q INTO got;

	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	EXECUTE q INTO want;
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	same := node IS NOT NULL AND got IS NOT DISTINCT FROM want;
END $$;

-- ---------- the table ----------
-- ts: a permutation of the row number as minutes, in no heap order, so each
-- 128-row bucket's summary has rows at most of the heap's 112 container
-- keys, one or two at each; kind: two values of half the rows each; tags:
-- 't1' on a tenth of the rows, all of them of kind 'a' - so the two clauses
-- select what the smaller selects, twice what they would if they were
-- independent, which is what the planner's estimate of their intersection
-- assumes - 't2' on a fifth, 't3' on seven tenths.
CREATE TABLE lion_spc_t (id int NOT NULL, kind text NOT NULL,
						 tags text[] NOT NULL, ts timestamptz NOT NULL,
						 pad text NOT NULL)
	WITH (autovacuum_enabled = off);
INSERT INTO lion_spc_t
SELECT i, CASE WHEN i % 2 = 0 THEN 'a' ELSE 'b' END,
	   CASE WHEN i % 10 = 0 THEN ARRAY['t1', 't2']
			WHEN i % 5 = 0 THEN ARRAY['t2']
			ELSE ARRAY['x'] END ||
	   CASE WHEN i % 10 < 7 THEN ARRAY['t3'] ELSE ARRAY['y'] END,
	   '2025-01-01 00:00:00+00'::timestamptz +
	   ((i::bigint * 7919) % 200003) * interval '1 minute',
	   repeat('p', 200)
  FROM generate_series(1, 200003) i;
CREATE INDEX lion_spc_i ON lion_spc_t USING lion (kind, tags, ts)
	WITH (summaries = on, summary_tids = 128);
VACUUM (FREEZE, ANALYZE) lion_spc_t;
SELECT pg_relation_size('lion_spc_t') / 8192 / 64 + 1 AS container_keys;

CREATE TABLE lion_spc_r (wm text, chosen text, same boolean, probed boolean,
						 visited bigint, cost float8);

-- A heap held in memory, as this shape is run, at the random_page_cost such
-- servers set: the plain scan's heap fetches priced as cached reads.
SET random_page_cost = 1.1;
SET effective_cache_size = '4GB';

-- ---------- 1. a 42-day window at three work_mems ----------
-- The tag's 20,000 rows at 112 keys: a copy of 43 kB, past half of 64kB, and
-- the marks on top within 128kB.  At 64kB every summary is counted and the
-- plain scan of the index is chosen; from 128kB the count probes and is
-- chosen, at the same price past the fit.
SET work_mem = '64kB';
INSERT INTO lion_spc_r SELECT '64kB', s.*
  FROM lion_spc('SELECT count(*) FROM lion_spc_t WHERE kind = ''a'' AND tags && ARRAY[''t1''] AND ts >= ''2025-02-01'' AND ts < ''2025-03-15''') s;
SET work_mem = '128kB';
INSERT INTO lion_spc_r SELECT '128kB', s.*
  FROM lion_spc('SELECT count(*) FROM lion_spc_t WHERE kind = ''a'' AND tags && ARRAY[''t1''] AND ts >= ''2025-02-01'' AND ts < ''2025-03-15''') s;
SET work_mem = '4MB';
INSERT INTO lion_spc_r SELECT '4MB', s.*
  FROM lion_spc('SELECT count(*) FROM lion_spc_t WHERE kind = ''a'' AND tags && ARRAY[''t1''] AND ts >= ''2025-02-01'' AND ts < ''2025-03-15''') s;
RESET work_mem;
SELECT wm, chosen, same, probed FROM lion_spc_r ORDER BY cost DESC, wm;
SELECT p.cost < 0.5 * c.cost AS probed_under_half,
	   round(f.cost::numeric / p.cost::numeric, 2) AS past_the_fit
  FROM lion_spc_r c, lion_spc_r p, lion_spc_r f
 WHERE c.wm = '64kB' AND p.wm = '128kB' AND f.wm = '4MB';

-- ---------- 2. the planner and the executor agree on the fit ----------
-- A window short of a bucket walks its keys and never probes, whatever the
-- memory.  't2', of a fifth of the rows at 112 keys, is a NARROW at each,
-- which the marks take a bitset image of besides: some 1 MB with the copy,
-- past 128kB, so counted there and left to the plain scan; it fits 4MB.
TRUNCATE lion_spc_r;
SET work_mem = '128kB';
INSERT INTO lion_spc_r SELECT 'short', s.*
  FROM lion_spc('SELECT count(*) FROM lion_spc_t WHERE kind = ''a'' AND tags && ARRAY[''t1''] AND ts >= ''2025-02-01'' AND ts < ''2025-02-01 01:00''') s;
INSERT INTO lion_spc_r SELECT 't2, 128kB', s.*
  FROM lion_spc('SELECT count(*) FROM lion_spc_t WHERE kind = ''a'' AND tags && ARRAY[''t2''] AND ts >= ''2025-02-01'' AND ts < ''2025-03-15''') s;
SET work_mem = '4MB';
INSERT INTO lion_spc_r SELECT 't2, 4MB', s.*
  FROM lion_spc('SELECT count(*) FROM lion_spc_t WHERE kind = ''a'' AND tags && ARRAY[''t2''] AND ts >= ''2025-02-01'' AND ts < ''2025-03-15''') s;
RESET work_mem;
SELECT wm, chosen, same, probed FROM lion_spc_r ORDER BY wm;

-- ---------- 3. when the walk turns to probing ----------
-- Beside 't3', of 140,000 rows, the smaller of the two sources is kind =
-- 'a', of 100,000: a walk that waited until it had handed out twice that
-- many rows never got there in this window of 60,500.  It now probes once
-- its counts have visited twice as many containers as that source has, 112,
-- a few summaries in.
TRUNCATE lion_spc_r;
SET work_mem = '4MB';
INSERT INTO lion_spc_r SELECT '4MB', s.*
  FROM lion_spc('SELECT count(*) FROM lion_spc_t WHERE kind = ''a'' AND tags && ARRAY[''t3''] AND ts >= ''2025-02-01'' AND ts < ''2025-03-15''') s;
RESET work_mem;
SELECT wm, chosen, same, probed FROM lion_spc_r ORDER BY wm;
SELECT lion_index_verify('lion_spc_i', true);

-- ---------- 4. without summaries ----------
-- The window's 60,500 keys are walked and counted a leaf at a time, never
-- probed, whatever the memory, and priced so: the plain scan is chosen.
ALTER INDEX lion_spc_i SET (summaries = off);
REINDEX INDEX lion_spc_i;
VACUUM (FREEZE, ANALYZE) lion_spc_t;
TRUNCATE lion_spc_r;
SET work_mem = '4MB';
INSERT INTO lion_spc_r SELECT 'off, 4MB', s.*
  FROM lion_spc('SELECT count(*) FROM lion_spc_t WHERE kind = ''a'' AND tags && ARRAY[''t1''] AND ts >= ''2025-02-01'' AND ts < ''2025-03-15''') s;
RESET work_mem;
SELECT wm, chosen, same, probed FROM lion_spc_r ORDER BY wm;
RESET effective_cache_size;
RESET random_page_cost;

DROP TABLE lion_spc_r;
DROP TABLE lion_spc_t;
DROP FUNCTION lion_spc(text);
