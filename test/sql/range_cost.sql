-- What a count over two ranges is priced at (DESIGN.md §28, "The cost of a
-- summed range").
--
-- `count(*) WHERE <range on a> AND <range on b>` sums one range and collects
-- the other as a source.  A collected range holds no pin, so it cannot drive
-- the count of the other sources that the complement of the summed range
-- needs, and the executor walks the inside however wide it is.  The planner
-- priced the complement all the same - any positive source would do - and
-- took two ranges over nearly every row for a tenth of what their inside
-- walk costs: the node, 2.5 s, over a sequential scan of 0.33 s (2M rows).
-- Every answer is checked against the pushdown turned off; the plan choices
-- are the planner's own, nothing disabled.
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

-- 100,000 rows, 47 to a heap page, 34 container keys.  lo has four values;
-- w1 is the row number modulo 1000 and w2 a permutation of 20,000 keys of
-- five rows each, both spread over the whole heap.
CREATE TABLE rcost (id int, lo int, w1 int, w2 int, pad text);
INSERT INTO rcost
SELECT g, g % 4, g % 1000, ((g::bigint * 7919) % 20000)::int, repeat('x', 120)
  FROM generate_series(1, 100000) g;
CREATE INDEX rcost_lo ON rcost USING lion (lo);
CREATE INDEX rcost_w1 ON rcost USING lion (w1);
CREATE INDEX rcost_w2 ON rcost USING lion (w2);
VACUUM (FREEZE, ANALYZE) rcost;

/*
 * rc_check() runs q with the pushdown forced (every other scan disabled) and
 * again with the pushdown off and the planner left alone, and compares the
 * answers.  It reports how the node took its ranges: how many it collected,
 * walked and spilled, and how it evaluated the range it summed.
 */
CREATE FUNCTION rc_check(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	j jsonb;
	node jsonb;
	how text;
	got text;
	want text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE 'EXPLAIN (ANALYZE, TIMING OFF, COSTS OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) '
		|| q INTO j;
	node := jsonb_path_query_first(j, 'strict $.** ? (@."Custom Plan Provider" == "LionCount")');
	IF node IS NULL THEN
		how := 'not pushed down';
	ELSE
		how := 'collected ' || coalesce(node ->> 'Range Sources Collected', '0') ||
			', walked ' || coalesce(node ->> 'Range Sources Walked', '0') ||
			', spilled ' || coalesce(node ->> 'Range Sources Spilled', '0');
		IF node ->> 'Range Evaluation' IS NOT NULL THEN
			how := how || ', ' || (node ->> 'Range Evaluation');
		END IF;
	END IF;
	EXECUTE format('SELECT coalesce(string_agg(s::text, '' '' ORDER BY s::text), ''(none)'') FROM (%s) s', q)
		INTO got;

	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	EXECUTE format('SELECT coalesce(string_agg(s::text, '' '' ORDER BY s::text), ''(none)'') FROM (%s) s', q)
		INTO want;
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	IF got IS DISTINCT FROM want THEN
		RETURN format('MISMATCH: %s, pushed %s, seqscan %s', how, got, want);
	END IF;
	RETURN how || ': ' || got;
END $$;

-- ---------- 1. The complement of a summed range, and what drives it ----------

-- Two ranges over nearly every row: the widest is summed and the other
-- collected, which cannot drive the complement, so the node would walk the
-- 19,989 keys inside.  The sequential scan is the cheaper plan, and taken.
EXPLAIN (COSTS OFF) SELECT count(*) FROM rcost WHERE w1 > 10 AND w2 > 10;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM rcost WHERE w1 BETWEEN 10 AND 900 AND w2 BETWEEN 10 AND 19000;
-- ... and the node, forced, walks the inside
SELECT rc_check('SELECT count(*) FROM rcost WHERE w1 > 10 AND w2 > 10');
SELECT rc_check('SELECT count(*) FROM rcost WHERE w1 BETWEEN 10 AND 900 AND w2 BETWEEN 10 AND 19000');

-- An equality beside the range drives the complement: the eleven keys
-- outside it are walked, and the node is still the plan.
EXPLAIN (COSTS OFF) SELECT count(*) FROM rcost WHERE lo = 1 AND w2 > 10;
SELECT rc_check('SELECT count(*) FROM rcost WHERE lo = 1 AND w2 > 10');
SELECT rc_check('SELECT count(*) FROM rcost WHERE lo = 1 AND w1 > 10 AND w2 > 10');

DROP FUNCTION rc_check(text);
DROP TABLE rcost;
