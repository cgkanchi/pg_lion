-- LionOrdered's fetch-and-sort switch (DESIGN.md §30.4, "When the walk is
-- not paying") at a work_mem its rows do not fit in: its sort goes to disk,
-- as a Sort node's does, and never gives up and lets the walk go on to the
-- end of the index; under a constant LIMIT that nothing between the node and
-- it can drop a row from, it keeps only the rows the LIMIT can take, in a
-- heap.  Every query's rows are compared, in order, with the same query
-- with the node off; every plan below is the node's, with the switch.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

-- k = id / 3, three rows a value, with a btree on (k, id); c: the last
-- 10,000 ids, at the far end of k's order; e: the first 10,000, at the far
-- end of its order backward.  A row is some 150 bytes, so the members' rows
-- are 1.5 MB.
CREATE TABLE osw (id int, k int, c int, e int, pad text)
	WITH (autovacuum_enabled = off);
INSERT INTO osw SELECT i, i / 3, (i > 90000)::int, (i <= 10000)::int,
	   repeat('x', 100)
  FROM generate_series(1, 100000) i;
CREATE INDEX osw_l ON osw USING lion (c, e);
CREATE INDEX osw_k ON osw (k, id);
VACUUM (FREEZE, ANALYZE) osw;

-- the same split in two partitions, the last 5,000 ids of each a member
CREATE TABLE osw_p (id int, k int, c int, pad text) PARTITION BY RANGE (id);
CREATE TABLE osw_p1 PARTITION OF osw_p FOR VALUES FROM (1) TO (50001)
	WITH (autovacuum_enabled = off);
CREATE TABLE osw_p2 PARTITION OF osw_p FOR VALUES FROM (50001) TO (100001)
	WITH (autovacuum_enabled = off);
INSERT INTO osw_p SELECT i, i / 3, ((i - 1) % 50000 >= 45000)::int,
	   repeat('x', 100)
  FROM generate_series(1, 100000) i;
CREATE INDEX osw_p_l ON osw_p USING lion (c);
CREATE INDEX osw_p_k ON osw_p (k, id);
VACUUM (FREEZE, ANALYZE) osw_p;

-- the node's switches and how its sort went, for q, which must be a
-- LionOrdered plan; one external sort or another, as the major's tuplesort
-- runs it
CREATE FUNCTION osw_run(q text) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE ln text; used bool := false;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(LionOrdered\)' THEN
			used := true;
		ELSIF ln ~ '(Index Entries Walked|Switched|Fetch and Sort (Method|Space Type))' THEN
			RETURN NEXT btrim(regexp_replace(ln, 'external (sort|merge)', 'external'));
		END IF;
	END LOOP;
	IF NOT used THEN
		RETURN NEXT 'not LionOrdered';
	END IF;
END $$;
-- q's rows, in order, against its own with the node off
CREATE FUNCTION osw_rows(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r record; acc text := ''; n int := 0;
BEGIN
	FOR r IN EXECUTE q LOOP
		acc := md5(acc || r::text);
		n := n + 1;
	END LOOP;
	RETURN n || ' rows ' || acc;
END $$;
CREATE FUNCTION osw_same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text := osw_rows(q); b text;
BEGIN
	SET LOCAL pg_lion.enable_ordered_scan = off;
	SET LOCAL enable_sort = on;
	SET LOCAL enable_seqscan = on;
	SET LOCAL enable_bitmapscan = on;
	SET LOCAL enable_indexscan = on;
	b := osw_rows(q);
	RETURN CASE WHEN a = b THEN 'same, ' || split_part(a, ' ', 1) || ' rows'
		   ELSE 'differ: ' || a || ' against ' || b END;
END $$;

-- the node's plans, the switch at its first chance (10,000 entries in), and
-- 64kB of work_mem for 1.5 MB of rows
SET enable_sort = off;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SET pg_lion.ordered_switch_ratio = 1;
SET work_mem = '64kB';

-- 1. A LIMIT, and nothing between the node and it: the sort keeps the ten
--    rows it can take, in a heap, in memory
SELECT * FROM osw_run('SELECT id, k FROM osw WHERE c = 1 ORDER BY k, id LIMIT 10');
SELECT osw_same('SELECT id, k FROM osw WHERE c = 1 ORDER BY k, id LIMIT 10');
-- ... with an OFFSET, which the Limit reads too: thirty
SELECT * FROM osw_run('SELECT id, k FROM osw WHERE c = 1 ORDER BY k, id LIMIT 10 OFFSET 20');
SELECT osw_same('SELECT id, k FROM osw WHERE c = 1 ORDER BY k, id LIMIT 10 OFFSET 20');
-- ... backward
SELECT * FROM osw_run('SELECT id, k FROM osw WHERE e = 1 ORDER BY k DESC, id DESC LIMIT 10');
SELECT osw_same('SELECT id, k FROM osw WHERE e = 1 ORDER BY k DESC, id DESC LIMIT 10');
-- ... and the rows come out of the sort as they were fetched: their system
-- columns too
SELECT * FROM osw_run('SELECT ctid, xmin::text, id, pad FROM osw WHERE c = 1 ORDER BY k, id LIMIT 10');
SELECT osw_same('SELECT ctid, xmin::text, id, pad FROM osw WHERE c = 1 ORDER BY k, id LIMIT 10');

-- 2. No LIMIT: every member's row is sorted, on disk
SELECT * FROM osw_run('SELECT id, k, pad FROM osw WHERE c = 1 ORDER BY k, id');
SELECT osw_same('SELECT id, k, pad FROM osw WHERE c = 1 ORDER BY k, id');

-- 3. A LIMIT the sort may not keep to, which it sorts every row for, on
--    disk: a filter of the node's own, whose rejects the Limit does not
--    count ...
SELECT * FROM osw_run('SELECT ctid, id, k FROM osw WHERE c = 1 AND pad LIKE ''x%'' ORDER BY k, id LIMIT 10');
SELECT osw_same('SELECT ctid, id, k FROM osw WHERE c = 1 AND pad LIKE ''x%'' ORDER BY k, id LIMIT 10');
-- ... WITH TIES, which reads the rows tied with the tenth as well (two
-- more here) ...
SELECT * FROM osw_run('SELECT id, k FROM osw WHERE c = 1 ORDER BY k FETCH FIRST 10 ROWS WITH TIES');
SELECT osw_same('SELECT id, k FROM osw WHERE c = 1 ORDER BY k FETCH FIRST 10 ROWS WITH TIES');
-- ... FOR UPDATE, whose LockRows skips a row it cannot lock ...
SELECT * FROM osw_run('SELECT id, k FROM osw WHERE c = 1 ORDER BY k, id LIMIT 10 FOR UPDATE');
SELECT osw_same('SELECT id, k FROM osw WHERE c = 1 ORDER BY k, id LIMIT 10 FOR UPDATE');
-- ... an order the query's goes on from, sorted again above the node ...
SELECT * FROM osw_run('SELECT id, k FROM osw WHERE c = 1 ORDER BY k, id, pad LIMIT 10');
SELECT osw_same('SELECT id, k FROM osw WHERE c = 1 ORDER BY k, id, pad LIMIT 10');
-- ... and a LIMIT not known when the plan was made
SET plan_cache_mode = force_generic_plan;
PREPARE osw_lim(int) AS SELECT id, k FROM osw WHERE c = 1 ORDER BY k, id LIMIT $1;
SELECT * FROM osw_run('EXECUTE osw_lim(10)');
SELECT osw_same('EXECUTE osw_lim(10)');
DEALLOCATE osw_lim;
RESET plan_cache_mode;

-- 4. Rescanned for each outer row, a new set each time: the rows the LIMIT
--    takes at each scan, two of the three switching
SELECT * FROM osw_run('SELECT g.v, s.id FROM (VALUES (1), (0), (1)) g(v),
	LATERAL (SELECT id FROM osw WHERE c = g.v ORDER BY k, id LIMIT 5) s');
SELECT osw_same('SELECT g.v, s.id FROM (VALUES (1), (0), (1)) g(v),
	LATERAL (SELECT id FROM osw WHERE c = g.v ORDER BY k, id LIMIT 5) s');

-- 5. Partitions under a MergeAppend, which reads no more of one than the
--    LIMIT takes: each sort keeps ten
SELECT * FROM osw_run('SELECT id, k FROM osw_p WHERE c = 1 ORDER BY k, id LIMIT 10');
SELECT osw_same('SELECT id, k FROM osw_p WHERE c = 1 ORDER BY k, id LIMIT 10');

-- 6. The walk's rows first, then the sort's: e's 10,000 members are met at
--    the start of the walk, and c's are switched to after them, the sort
--    keeping the ten the LIMIT has left - read across through a cursor too.
--    (pad keeps the walk from the btree's values alone, §40, whose switch
--    waits for a walk that meets members more sparsely than this one.)
SELECT * FROM osw_run('SELECT id, k, pad FROM osw WHERE c = 1 OR e = 1 ORDER BY k, id LIMIT 10010');
SELECT osw_same('SELECT id, k, pad FROM osw WHERE c = 1 OR e = 1 ORDER BY k, id LIMIT 10010');
BEGIN;
DECLARE osw_c CURSOR FOR
	SELECT id, k, length(pad) FROM osw WHERE c = 1 OR e = 1 ORDER BY k, id LIMIT 10010;
MOVE 9995 IN osw_c;
FETCH 10 FROM osw_c;
FETCH 10 FROM osw_c;
COMMIT;

RESET work_mem;
RESET pg_lion.ordered_switch_ratio;
RESET enable_indexscan;
RESET enable_bitmapscan;
RESET enable_seqscan;
RESET enable_sort;
DROP FUNCTION osw_same(text);
DROP FUNCTION osw_rows(text);
DROP FUNCTION osw_run(text);
DROP TABLE osw_p;
DROP TABLE osw;
