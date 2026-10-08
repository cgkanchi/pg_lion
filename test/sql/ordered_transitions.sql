-- Every transition of a LionOrdered scan (DESIGN.md §30.4), forced and
-- named: EXPLAIN ANALYZE says why a lazy set was built, when the switch to
-- fetch-and-sort gave up, when a scan stopped counting the members it met,
-- and when the early stop ended a walk.  Each run's rows are compared, in
-- order, with the same query with the node off.
--
-- Not forced here: building a lazy set because the walk is not in heap
-- order.  It needs a walk of 64 keys within the probe budget, which a
-- random-order walk spends at about 8 a key per set, while the budget is
-- the sets' containers: sets of more than about 256 containers, 16,000
-- heap pages each.  Nor building one because the switch could be due by
-- the set's recorded member count: ordered.sql forces that (case 15).
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

-- a: 7 values in heap order's period, b: 10 by hash; k: a permutation of
-- the ids with a btree, in no heap order; the primary key in heap order
CREATE TABLE otr (id int PRIMARY KEY, k int, a int, b int, pad text)
	WITH (autovacuum_enabled = off);
INSERT INTO otr SELECT i, (i::bigint * 7919 % 200003)::int, i % 7,
	   (hashint4(i) & 1023) % 10, repeat('x', 40)
  FROM generate_series(1, 200000) i;
CREATE INDEX otr_l ON otr USING lion (a, b);
CREATE INDEX otr_k ON otr (k, id);
-- the heap in eight interleaved sweeps: a walk of one meets a new container
-- key every 700 entries or so, where one in heap order meets one every 5,500
CREATE INDEX otr_m ON otr ((id % 8), id);
VACUUM (FREEZE, ANALYZE) otr;

-- the node's counters for q, which must be a LionOrdered plan
CREATE FUNCTION otr_run(q text) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE ln text; used bool := false;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(LionOrdered\)' THEN
			used := true;
		ELSIF ln ~ '(Index Entries Walked|Lion Set:|Switched|Given Up|Not Counting|Stopped Early)' THEN
			RETURN NEXT btrim(ln);
		END IF;
	END LOOP;
	IF NOT used THEN
		RETURN NEXT 'not LionOrdered';
	END IF;
END $$;

-- q's rows in order against the same query with the node off and sorting on
CREATE FUNCTION otr_rows(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r record; acc text := '';
BEGIN
	FOR r IN EXECUTE q LOOP
		acc := acc || r::text || ',';
	END LOOP;
	RETURN md5(acc);
END $$;
CREATE FUNCTION otr_same(q text, ordinary text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE a text := otr_rows(q); b text;
BEGIN
	SET LOCAL pg_lion.enable_ordered_scan = off;
	SET LOCAL enable_sort = on;
	SET LOCAL enable_seqscan = on;
	b := otr_rows(ordinary);
	RETURN CASE WHEN a = b THEN 'same' ELSE 'differ' END;
END $$;

-- only the node's plans, made at the default work_mem: generic plans keep
-- them when work_mem is lowered below.  A LionOrdered set starts lazily only
-- where the planner expects the walk to stop short (DESIGN.md §30.4): those
-- below whose walks go far bound the walked column by parameters, whose
-- default estimate (0.5% of the rows) has the planner expect a short walk,
-- and are run over the whole table
SET enable_sort = off;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SET plan_cache_mode = force_generic_plan;
PREPARE otr_k(int) AS
	SELECT id, k FROM otr WHERE a = $1 AND b = 4 ORDER BY k, id LIMIT 5;
PREPARE otr_id(int, int, int) AS
	SELECT id FROM otr WHERE a = $1 AND b = 4 AND id % 8 BETWEEN $2 AND $3
	 ORDER BY id % 8, id LIMIT 3000;
PREPARE otr_all(int, int, int) AS
	SELECT id FROM otr WHERE a = $1 AND b = 4 AND id BETWEEN $2 AND $3
	 ORDER BY id;
PREPARE otr_pad(int, int, int) AS
	SELECT id, k, pad FROM otr WHERE a = $1 AND b = 4 AND k BETWEEN $2 AND $3
	 ORDER BY k, id;
PREPARE otr_or(int, int, int) AS
	SELECT id FROM otr WHERE (b = $1 OR a = 2) AND id % 8 BETWEEN $2 AND $3
	 ORDER BY id % 8, id LIMIT 90000;
PREPARE otr_far(int) AS
	SELECT id FROM otr WHERE a = $1 AND b = 4 ORDER BY id;
EXPLAIN (COSTS OFF) EXECUTE otr_k(3);
EXPLAIN (COSTS OFF) EXECUTE otr_id(3, 0, 7);
EXPLAIN (COSTS OFF) EXECUTE otr_all(3, 1, 200000);
EXPLAIN (COSTS OFF) EXECUTE otr_pad(3, 0, 200003);
EXPLAIN (COSTS OFF) EXECUTE otr_or(1, 0, 7);

-- 1. The lazy set built because its probes cost what the build would: the
--    walk in k's order starts its streams again at nearly every key
SELECT * FROM otr_run('EXECUTE otr_k(3)');
SELECT otr_same('EXECUTE otr_k(3)',
	'SELECT id, k FROM otr WHERE a = 3 AND b = 4 ORDER BY k, id LIMIT 5');

-- 2. ... because the walk has met 10,000 entries, at least eight for each
--    unit of the probe budget, in heap order and with the switch kept away;
--    the early stop then ends the walk at the last member
SET pg_lion.ordered_switch_ratio = 1000000;
SELECT * FROM otr_run('EXECUTE otr_all(3, 1, 200000)');
SELECT otr_same('EXECUTE otr_all(3, 1, 200000)',
	'SELECT id FROM otr WHERE a = 3 AND b = 4 ORDER BY id');
RESET pg_lion.ordered_switch_ratio;

-- 3. ... because the memo, a member bitmap per key met, outgrew half of
--    hash_mem; built in 64kB, the set's visited bitmaps do not fit either,
--    so the scan stops counting members: no early stop, no switch
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SELECT * FROM otr_run('EXECUTE otr_id(3, 0, 7)');
SELECT otr_same('EXECUTE otr_id(3, 0, 7)',
	'SELECT id FROM otr WHERE a = 3 AND b = 4 ORDER BY id % 8, id LIMIT 3000');

-- 4. ... and the set built then degrades to its container keys, every
--    member rechecked
SELECT * FROM otr_run('EXECUTE otr_or(1, 0, 7)');
SELECT otr_same('EXECUTE otr_or(1, 0, 7)',
	'SELECT id FROM otr WHERE (b = 1 OR a = 2) ORDER BY id % 8, id LIMIT 90000');

-- 5. Built from the start, the set's visited bitmaps over hash_mem: the
--    scan stops counting members as the walk meets them
SET pg_lion.enable_lazy_set = off;
SELECT * FROM otr_run('EXECUTE otr_id(3, 0, 7)');
SELECT otr_same('EXECUTE otr_id(3, 0, 7)',
	'SELECT id FROM otr WHERE a = 3 AND b = 4 ORDER BY id % 8, id LIMIT 3000');
RESET pg_lion.enable_lazy_set;
RESET hash_mem_multiplier;
RESET work_mem;

-- 6. The switch to fetch and sort: with the ratio at 1 it comes at the
--    first chance, 10,000 entries in
SET pg_lion.ordered_switch_ratio = 1;
SELECT * FROM otr_run('EXECUTE otr_pad(3, 0, 200003)');
SELECT otr_same('EXECUTE otr_pad(3, 0, 200003)',
	'SELECT id, k, pad FROM otr WHERE a = 3 AND b = 4 ORDER BY k, id');

-- 7. ... and gives up when the members' rows do not fit in work_mem, while
--    hash_mem still holds the visited bitmaps: the walk goes on to the end.
--    In 64kB the rows kept pass work_mem before 1,024 members are fetched;
--    in 256kB the first 1,024 foretell that the 2,738 would not fit, and the
--    rest are not fetched
SET work_mem = '64kB';
SET hash_mem_multiplier = 8;
SELECT * FROM otr_run('EXECUTE otr_pad(3, 0, 200003)');
SET work_mem = '256kB';
SET hash_mem_multiplier = 2;
SELECT * FROM otr_run('EXECUTE otr_pad(3, 0, 200003)');
SELECT otr_same('EXECUTE otr_pad(3, 0, 200003)',
	'SELECT id, k, pad FROM otr WHERE a = 3 AND b = 4 ORDER BY k, id');
RESET hash_mem_multiplier;
RESET work_mem;
RESET pg_lion.ordered_switch_ratio;

-- 8. A walk the planner expects to go far, with no LIMIT: the set built at
--    the start, never lazily
SELECT * FROM otr_run('EXECUTE otr_far(3)');
SELECT otr_same('EXECUTE otr_far(3)',
	'SELECT id FROM otr WHERE a = 3 AND b = 4 ORDER BY id');

RESET plan_cache_mode;
RESET enable_sort;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
DEALLOCATE ALL;
DROP FUNCTION otr_same(text, text);
DROP FUNCTION otr_rows(text);
DROP FUNCTION otr_run(text);
DROP TABLE otr;
