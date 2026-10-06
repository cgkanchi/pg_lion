-- The latest rows of a fact whose fk is in a filtered dimension: ORDER BY a
-- lion-indexed column [DESC] LIMIT n, over a partitioned fact and a plain one,
-- with and without a semi join to a dimension (DESIGN.md §30.11).
--
-- The fact has a lion index over (tags, ts, fk) and a btree on (fk, ts), and
-- no btree on ts: the only order of ts there is is lion's own directory, which
-- LionOrdered now walks, in either direction, and whose leaf partitions it now
-- scans.  Every answer is compared, IN ORDER, with the plan the planner makes
-- with pg_lion.enable_ordered_scan off; every ORDER BY ends in the unique id,
-- which core sorts incrementally above the walk, so that ties in ts come out
-- in one order.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;
SET work_mem = '4MB';
-- a Memoize over the dimension's probe is core's choice, and varies by major
SET enable_memoize = off;

-- The dimension: 3,000 keys; grp = 1 AND flag keeps the 1,200 whose pk % 5 is
-- 1 or 3, which the fact's rows favour.  grp = 3 AND lvl = 7 keeps a few.
CREATE TABLE osd (pk int PRIMARY KEY, grp int NOT NULL, flag bool NOT NULL,
				  lvl int NOT NULL) WITH (autovacuum_enabled = off);
INSERT INTO osd
SELECT pk, CASE WHEN pk % 5 IN (1, 3) THEN 1 ELSE 2 + pk % 3 END,
	   pk % 5 IN (1, 3), pk % 97
  FROM generate_series(1, 3000) pk;
CREATE INDEX osd_cov ON osd (grp, flag, lvl) INCLUDE (pk);

-- The fact: 60,000 rows LIST-partitioned by kind ('a' 36,000, 'b' 12,000,
-- the rest in the default partition), and a plain copy.  ts takes 20,000
-- values ten minutes apart, three rows each, in no heap order, and is NULL on
-- every 89th row; fk is NULL on every 97th, a heavy key on every other row, a
-- key of 1 .. 12,000 (most of them no dimension key) otherwise.
CREATE TABLE osf (id int NOT NULL, kind text NOT NULL, fk int, ts timestamptz,
				  tags text[], name text) PARTITION BY LIST (kind);
CREATE TABLE osf_a PARTITION OF osf FOR VALUES IN ('a') WITH (autovacuum_enabled = off);
CREATE TABLE osf_b PARTITION OF osf FOR VALUES IN ('b') WITH (autovacuum_enabled = off);
CREATE TABLE osf_o PARTITION OF osf DEFAULT WITH (autovacuum_enabled = off);
INSERT INTO osf
SELECT i, (ARRAY['a', 'a', 'a', 'b', 'c'])[1 + i % 5],
	   CASE WHEN i % 97 = 0 THEN NULL
			WHEN i % 2 = 0 THEN 5 * ((i * 7) % 600) + 1 + 2 * (i % 4 / 2)
			ELSE 1 + (i * 7919) % 12000 END,
	   CASE WHEN i % 89 = 0 THEN NULL
			ELSE timestamptz '2025-01-01 00:00+00' +
				 ((i::bigint * 104729) % 20000) * interval '10 minutes' END,
	   ARRAY['t' || i % 11, 'u' || i % 7] ||
	   CASE WHEN i % 1000 = 7 THEN ARRAY['rare'] ELSE '{}' END,
	   'n' || i
  FROM generate_series(1, 60000) i;
CREATE TABLE ost (LIKE osf) WITH (autovacuum_enabled = off);
INSERT INTO ost SELECT * FROM osf;

CREATE INDEX osf_l ON osf USING lion (tags, ts, fk) WITH (summaries = on, summary_tids = 64);
CREATE INDEX osf_fk ON osf (fk, ts);
CREATE INDEX ost_l ON ost USING lion (tags, ts, fk) WITH (summaries = on, summary_tids = 64);
CREATE INDEX ost_fk ON ost (fk, ts);
VACUUM (FREEZE, ANALYZE) osd;
VACUUM (FREEZE, ANALYZE) osf;
VACUUM (FREEZE, ANALYZE) ost;

/*
 * os_cmp() runs q with nothing disabled - or, with force, without core's
 * Sort, so that an ordered path is what is left - and says whether its plan
 * has a LionOrdered node; runs it again with the node off; and compares the
 * two answers in order.
 */
CREATE FUNCTION os_cmp(q text, force boolean DEFAULT false,
					   ordinary text DEFAULT NULL) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	r record;
	used boolean := false;
	a text[] := '{}';
	b text[] := '{}';
BEGIN
	IF force THEN
		PERFORM set_config('enable_sort', 'off', true);
	END IF;
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(LionOrdered\)' THEN
			used := true;
		END IF;
	END LOOP;
	FOR r IN EXECUTE q LOOP
		a := a || r::text;
	END LOOP;
	PERFORM set_config('enable_sort', 'on', true);

	PERFORM set_config('pg_lion.enable_ordered_scan', 'off', true);
	FOR r IN EXECUTE coalesce(ordinary, q) LOOP
		b := b || r::text;
	END LOOP;
	PERFORM set_config('pg_lion.enable_ordered_scan', 'on', true);

	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH: %s rows through the node, %s ordinary; first %s / %s',
					  cardinality(a), cardinality(b), a[1], b[1]);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN used THEN 'LionOrdered' ELSE 'no LionOrdered' END,
				  cardinality(a));
END $$;

-- os_plan() prints the plan with nothing disabled, less what varies by major.
CREATE FUNCTION os_plan(q text) RETURNS SETOF text
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

-- os_uses() says whether the plan with nothing disabled has the node.
CREATE FUNCTION os_uses(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(LionOrdered\)' THEN
			RETURN 'LionOrdered';
		END IF;
	END LOOP;
	RETURN 'no LionOrdered';
END $$;

-- os_run() prints the node's counters under EXPLAIN ANALYZE.
CREATE FUNCTION os_run(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF ln ~ '(LionOrdered|Ordered By|Index Entries Walked|Lion Keys|Lion Set|Heap Fetches|Rows Removed by|Switched)' THEN
			RETURN NEXT btrim(regexp_replace(ln, '\s*\(actual.*\)', ''));
		END IF;
	END LOOP;
END $$;

-- 1. The plans chosen with nothing disabled: the feed with its semi join is a
--    nested loop over the walk of ts backwards, over the partition the kind
--    prunes to and over the plain fact; the feed alone is the walk.
SELECT * FROM os_plan($$
	SELECT f.id, f.fk, f.ts FROM osf f
	 WHERE f.kind = 'a' AND f.tags && ARRAY['t1', 't2', 't3']
	   AND f.ts >= timestamptz '2025-03-01 00:00+00'
	   AND f.fk IN (SELECT d.pk FROM osd d WHERE d.grp = 1 AND d.flag)
	 ORDER BY f.ts DESC LIMIT 10$$);
SELECT * FROM os_plan($$
	SELECT f.id, f.fk, f.ts FROM ost f
	 WHERE f.tags && ARRAY['t1', 't2', 't3']
	   AND f.ts >= timestamptz '2025-03-01 00:00+00'
	   AND f.fk IN (SELECT d.pk FROM osd d WHERE d.grp = 1 AND d.flag)
	 ORDER BY f.ts DESC LIMIT 10$$);
SELECT * FROM os_plan($$
	SELECT f.id, f.ts FROM osf f
	 WHERE f.kind = 'a' AND f.tags && ARRAY['t1', 't2', 't3']
	   AND f.ts >= timestamptz '2025-03-01 00:00+00'
	 ORDER BY f.ts DESC LIMIT 10$$);
-- a filter a lion set answers: the walk tests each row's TID against it;
-- one of a few dozen rows is fetched and sorted instead
SELECT * FROM os_plan($$
	SELECT f.id, f.ts FROM osf f
	 WHERE f.kind = 'a' AND f.tags @> ARRAY['t3', 'u4']
	 ORDER BY f.ts DESC LIMIT 10$$);
SELECT os_uses($$
	SELECT f.id, f.ts FROM osf f
	 WHERE f.kind = 'a' AND f.tags @> ARRAY['rare']
	 ORDER BY f.ts DESC LIMIT 10$$);
-- a semi join that keeps few rows: core's plan, which reads only those
SELECT os_uses($$
	SELECT f.id, f.fk, f.ts FROM osf f
	 WHERE f.kind = 'a' AND f.tags && ARRAY['t1', 't2', 't3']
	   AND f.ts >= timestamptz '2025-03-01 00:00+00'
	   AND f.fk IN (SELECT d.pk FROM osd d WHERE d.grp = 3 AND d.lvl = 7)
	 ORDER BY f.ts DESC LIMIT 10$$);
-- two partitions: a MergeAppend of two walks
SELECT * FROM os_plan($$
	SELECT f.id, f.ts FROM osf f
	 WHERE f.kind IN ('a', 'b') AND f.tags && ARRAY['t1', 't2', 't3']
	 ORDER BY f.ts DESC LIMIT 10$$);

-- 2. The answers, over the partitioned fact and the plain one.
SELECT os_cmp($$
	SELECT f.id, f.fk, f.ts FROM osf f
	 WHERE f.kind = 'a' AND f.tags && ARRAY['t1', 't2', 't3']
	   AND f.ts >= timestamptz '2025-03-01 00:00+00'
	   AND f.fk IN (SELECT d.pk FROM osd d WHERE d.grp = 1 AND d.flag)
	 ORDER BY f.ts DESC, f.id LIMIT 25$$);
SELECT os_cmp($$
	SELECT f.id, f.fk, f.ts FROM ost f
	 WHERE f.kind = 'a' AND f.tags && ARRAY['t1', 't2', 't3']
	   AND f.ts >= timestamptz '2025-03-01 00:00+00'
	   AND f.fk IN (SELECT d.pk FROM osd d WHERE d.grp = 1 AND d.flag)
	 ORDER BY f.ts DESC, f.id LIMIT 25$$);
SELECT os_cmp($$
	SELECT f.id, f.fk, f.ts FROM osf f
	 WHERE f.kind = 'a' AND f.tags && ARRAY['t1', 't2', 't3']
	   AND f.ts >= timestamptz '2025-03-01 00:00+00'
	   AND EXISTS (SELECT 1 FROM osd d WHERE d.pk = f.fk AND d.grp = 1 AND d.flag)
	 ORDER BY f.ts DESC, f.id LIMIT 3$$);
SELECT os_cmp($$
	SELECT f.id, f.fk, f.ts FROM osf f
	 WHERE f.kind = 'a' AND f.tags && ARRAY['t1', 't2', 't3']
	   AND f.ts >= timestamptz '2025-03-01 00:00+00'
	   AND f.fk IN (SELECT d.pk FROM osd d WHERE d.grp = 1 AND d.flag)
	 ORDER BY f.ts DESC, f.id LIMIT 300$$);
-- LIMIT with OFFSET
SELECT os_cmp($$
	SELECT f.id, f.fk, f.ts FROM osf f
	 WHERE f.kind = 'a' AND f.tags && ARRAY['t1', 't2', 't3']
	   AND f.ts >= timestamptz '2025-03-01 00:00+00'
	   AND f.fk IN (SELECT d.pk FROM osd d WHERE d.grp = 1 AND d.flag)
	 ORDER BY f.ts DESC, f.id OFFSET 17 LIMIT 9$$);
-- the feed alone; with a range bounded on both sides; ascending
SELECT os_cmp($$
	SELECT f.id, f.ts FROM osf f
	 WHERE f.kind = 'a' AND f.tags && ARRAY['t1', 't2', 't3']
	   AND f.ts >= timestamptz '2025-03-01 00:00+00'
	 ORDER BY f.ts DESC, f.id LIMIT 40$$);
SELECT os_cmp($$
	SELECT f.id, f.ts FROM osf f
	 WHERE f.kind = 'a' AND f.tags && ARRAY['t4']
	   AND f.ts BETWEEN timestamptz '2025-02-01 00:00+00' AND timestamptz '2025-02-03 00:00+00'
	 ORDER BY f.ts DESC, f.id LIMIT 40$$, true);
SELECT os_cmp($$
	SELECT f.id, f.ts FROM osf f
	 WHERE f.kind = 'a' AND f.tags && ARRAY['t4']
	   AND f.ts > timestamptz '2025-02-01 00:00+00' AND f.ts < timestamptz '2025-02-03 00:00+00'
	 ORDER BY f.ts, f.id LIMIT 40$$, true);
SELECT os_cmp($$
	SELECT f.id, f.ts FROM ost f
	 WHERE f.tags && ARRAY['u3'] AND f.ts < timestamptz '2025-01-02 12:00+00'
	 ORDER BY f.ts, f.id LIMIT 40$$, true);
-- a stable bound, evaluated when the walk starts
SELECT os_cmp($$
	SELECT f.id, f.ts FROM osf f
	 WHERE f.kind = 'a' AND f.tags && ARRAY['t1']
	   AND f.ts >= ('2025-04-01'::date)::timestamptz
	 ORDER BY f.ts DESC, f.id LIMIT 20$$, true);
-- no range: the NULL entry first or last, as the ORDER BY puts NULLs
SELECT os_cmp($$
	SELECT f.id, f.ts FROM osf f WHERE f.kind = 'a' AND f.tags && ARRAY['t2']
	 ORDER BY f.ts DESC, f.id LIMIT 30$$, true);
SELECT os_cmp($$
	SELECT f.id, f.ts FROM osf f WHERE f.kind = 'a' AND f.tags && ARRAY['t2']
	 ORDER BY f.ts DESC NULLS LAST, f.id LIMIT 30$$, true);
SELECT os_cmp($$
	SELECT f.id, f.ts FROM ost f WHERE f.tags && ARRAY['t2']
	 ORDER BY f.ts NULLS FIRST, f.id LIMIT 30$$, true);
SELECT os_cmp($$
	SELECT f.id, f.ts FROM ost f WHERE f.tags && ARRAY['t2'] AND f.ts IS NOT NULL
	 ORDER BY f.ts DESC, f.id LIMIT 30$$, true);
SELECT os_cmp($$
	SELECT f.id, f.ts FROM ost f WHERE f.tags && ARRAY['t2']
	 ORDER BY f.ts DESC, f.id OFFSET 670 LIMIT 30$$, true);
-- NULL fks: in the feed, not in the semi join
SELECT os_cmp($$
	SELECT f.id, f.fk, f.ts FROM ost f WHERE f.fk IS NULL
	 ORDER BY f.ts DESC, f.id LIMIT 30$$, true);
SELECT os_cmp($$
	SELECT f.id, f.fk, f.ts FROM ost f
	 WHERE f.fk IN (SELECT d.pk FROM osd d WHERE d.lvl < 30) OR f.fk IS NULL
	 ORDER BY f.ts DESC, f.id LIMIT 30$$, true);
-- a set, a sparse one, and one that is empty
SELECT os_cmp($$
	SELECT f.id, f.ts FROM osf f WHERE f.kind = 'a' AND f.tags @> ARRAY['t3', 'u4']
	 ORDER BY f.ts DESC, f.id LIMIT 10$$);
SELECT os_cmp($$
	SELECT f.id, f.ts FROM ost f WHERE f.tags @> ARRAY['t3', 'u4']
	   AND f.ts < timestamptz '2025-04-01 00:00+00'
	 ORDER BY f.ts, f.id LIMIT 10$$);
SELECT os_cmp($$
	SELECT f.id, f.ts FROM osf f WHERE f.kind = 'a' AND f.tags @> ARRAY['rare']
	 ORDER BY f.ts DESC, f.id LIMIT 10$$, true);
SELECT os_cmp($$
	SELECT f.id, f.ts FROM osf f WHERE f.kind = 'a' AND f.tags @> ARRAY['none']
	 ORDER BY f.ts DESC, f.id LIMIT 10$$, true);
-- the selective semi join, forced through the walk
SELECT os_cmp($$
	SELECT f.id, f.fk, f.ts FROM osf f
	 WHERE f.kind = 'a' AND f.tags && ARRAY['t1', 't2', 't3']
	   AND f.ts >= timestamptz '2025-03-01 00:00+00'
	   AND f.fk IN (SELECT d.pk FROM osd d WHERE d.grp = 3 AND d.lvl = 7)
	 ORDER BY f.ts DESC, f.id LIMIT 10$$, true);
-- two partitions, and every partition
SELECT os_cmp($$
	SELECT f.id, f.kind, f.ts FROM osf f
	 WHERE f.kind IN ('a', 'b') AND f.tags && ARRAY['t1', 't2', 't3']
	   AND f.fk IN (SELECT d.pk FROM osd d WHERE d.grp = 1 AND d.flag)
	 ORDER BY f.ts DESC, f.id LIMIT 50$$, true);
SELECT os_cmp($$
	SELECT f.id, f.kind, f.ts FROM osf f WHERE f.tags && ARRAY['u1']
	 ORDER BY f.ts, f.id LIMIT 50$$, true);
-- the answers of the two facts agree too
SELECT (SELECT array_agg(id ORDER BY ts DESC, id) FROM
		(SELECT id, ts FROM osf WHERE kind = 'a' AND tags && ARRAY['t5']
		  ORDER BY ts DESC, id LIMIT 60) x) =
	   (SELECT array_agg(id ORDER BY ts DESC, id) FROM
		(SELECT id, ts FROM ost WHERE kind = 'a' AND tags && ARRAY['t5']
		  ORDER BY ts DESC, id LIMIT 60) y) AS same_answer;

-- row locks above the walk
SELECT os_cmp($$
	SELECT f.id, f.ts FROM ost f
	 WHERE f.tags && ARRAY['t2'] AND f.ts > timestamptz '2025-03-01 00:00+00'
	 ORDER BY f.ts DESC, f.id LIMIT 5 FOR UPDATE$$, true);

-- 3. What the walk did: the keys and rows it met, the rows it fetched; and
--    a walk that meets a set's few members too far apart, which fetches and
--    sorts the rest of them (§30.4).
SELECT * FROM os_run($$
	SELECT f.id FROM osf f
	 WHERE f.kind = 'a' AND f.tags && ARRAY['t1', 't2', 't3']
	   AND f.ts >= timestamptz '2025-03-01 00:00+00'
	 ORDER BY f.ts DESC LIMIT 10$$);
SELECT * FROM os_run($$
	SELECT f.id FROM osf f WHERE f.kind = 'a' AND f.tags @> ARRAY['t3', 'u4']
	 ORDER BY f.ts DESC LIMIT 10$$);
SET enable_sort = off;
SELECT * FROM os_run($$
	SELECT f.id FROM osf f WHERE f.kind = 'a' AND f.tags @> ARRAY['rare']
	 ORDER BY f.ts DESC LIMIT 20$$);
RESET enable_sort;
SELECT os_cmp($$
	SELECT f.id, f.ts FROM osf f WHERE f.kind = 'a' AND f.tags @> ARRAY['rare']
	 ORDER BY f.ts DESC, f.id LIMIT 20$$, true);

-- 4. Params: a generic plan, and LATERAL subqueries rescanned per outer row,
--    with the outer value in the walk's range and in the filter.
SET plan_cache_mode = force_generic_plan;
PREPARE os_p(text, text[], timestamptz, int) AS
	SELECT f.id, f.fk, f.ts FROM osf f
	 WHERE f.kind = $1 AND f.tags && $2 AND f.ts >= $3
	   AND f.fk IN (SELECT d.pk FROM osd d WHERE d.grp = 1 AND d.flag)
	 ORDER BY f.ts DESC, f.id LIMIT $4;
SELECT os_cmp($$EXECUTE os_p('a', ARRAY['t1', 't2'], '2025-03-01 00:00+00', 15)$$, true,
			  $$SELECT f.id, f.fk, f.ts FROM osf f
				 WHERE f.kind = 'a' AND f.tags && ARRAY['t1', 't2'] AND f.ts >= '2025-03-01 00:00+00'
				   AND f.fk IN (SELECT d.pk FROM osd d WHERE d.grp = 1 AND d.flag)
				 ORDER BY f.ts DESC, f.id LIMIT 15$$);
SELECT os_cmp($$EXECUTE os_p('b', ARRAY['u4'], '2025-04-20 00:00+00', 7)$$, true,
			  $$SELECT f.id, f.fk, f.ts FROM osf f
				 WHERE f.kind = 'b' AND f.tags && ARRAY['u4'] AND f.ts >= '2025-04-20 00:00+00'
				   AND f.fk IN (SELECT d.pk FROM osd d WHERE d.grp = 1 AND d.flag)
				 ORDER BY f.ts DESC, f.id LIMIT 7$$);
SELECT os_cmp($$EXECUTE os_p('a', ARRAY['t1'], NULL, 7)$$, true,
			  $$SELECT f.id, f.fk, f.ts FROM osf f
				 WHERE f.kind = 'a' AND f.tags && ARRAY['t1'] AND f.ts >= NULL
				   AND f.fk IN (SELECT d.pk FROM osd d WHERE d.grp = 1 AND d.flag)
				 ORDER BY f.ts DESC, f.id LIMIT 7$$);
DEALLOCATE os_p;
RESET plan_cache_mode;
SELECT os_cmp($$
	SELECT o.v, x.id, x.ts FROM (VALUES ('2025-02-01 00:00+00'::timestamptz),
										('2025-04-01 00:00+00'), ('2026-01-01 00:00+00')) o(v),
	  LATERAL (SELECT f.id, f.ts FROM osf f
				WHERE f.kind = 'a' AND f.tags && ARRAY['t6'] AND f.ts < o.v
				ORDER BY f.ts DESC, f.id LIMIT 4) x$$, true);
SELECT os_cmp($$
	SELECT o.v, x.id, x.ts FROM (VALUES ('t1'), ('u2'), ('rare')) o(v),
	  LATERAL (SELECT f.id, f.ts FROM ost f
				WHERE f.tags @> ARRAY[o.v] ORDER BY f.ts DESC, f.id LIMIT 4) x$$, true);

-- 5. A cursor fetched in pieces, against the whole answer.
SET enable_sort = off;
BEGIN;
DECLARE os_c CURSOR FOR
	SELECT f.id, f.ts FROM osf f
	 WHERE f.kind = 'a' AND f.tags && ARRAY['t1', 't2', 't3']
	   AND f.ts < timestamptz '2025-04-01 00:00+00'
	   AND f.fk IN (SELECT d.pk FROM osd d WHERE d.grp = 1 AND d.flag)
	 ORDER BY f.ts DESC, f.id LIMIT 12;
FETCH 4 FROM os_c;
FETCH 3 FROM os_c;
FETCH ALL FROM os_c;
ROLLBACK;
RESET enable_sort;
SET pg_lion.enable_ordered_scan = off;
SELECT f.id, f.ts FROM osf f
 WHERE f.kind = 'a' AND f.tags && ARRAY['t1', 't2', 't3']
   AND f.ts < timestamptz '2025-04-01 00:00+00'
   AND f.fk IN (SELECT d.pk FROM osd d WHERE d.grp = 1 AND d.flag)
 ORDER BY f.ts DESC, f.id LIMIT 12;
RESET pg_lion.enable_ordered_scan;

/*
 * ... and a cursor paused in the middle of its walk while 6,000 new keys go
 * into the fifty hours of the column it walks next - below the row it
 * stopped at for a descending walk, above it for an ascending one - and split
 * the leaf it holds a copy of and the leaves it reads next.  The rows are
 * inserted by the same transaction after the cursor's snapshot was taken, so
 * the cursor must return exactly what it would have returned without them.
 */
CREATE FUNCTION os_cursor_split(q text, backward boolean) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	c refcursor;
	r record;
	ln text;
	used boolean := false;
	a text[] := '{}';
	b text[] := '{}';
BEGIN
	PERFORM set_config('pg_lion.enable_ordered_scan', 'off', true);
	FOR r IN EXECUTE q LOOP
		b := b || r::text;
	END LOOP;
	PERFORM set_config('pg_lion.enable_ordered_scan', 'on', true);
	PERFORM set_config('enable_sort', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(LionOrdered\)' THEN
			used := true;
		END IF;
	END LOOP;
	OPEN c FOR EXECUTE q;
	LOOP
		FETCH c INTO r;
		EXIT WHEN NOT FOUND;
		a := a || r::text;
		IF cardinality(a) = 50 THEN
			INSERT INTO ost
			SELECT 100000 + i, 'a', 1,
				   CASE WHEN backward THEN r.ts - interval '20 minutes' - i * interval '30 seconds'
						ELSE r.ts + interval '20 minutes' + i * interval '30 seconds' END,
				   ARRAY['t1', 'u1'], 'new'
			  FROM generate_series(1, 6000) i;
		END IF;
	END LOOP;
	CLOSE c;
	PERFORM set_config('enable_sort', 'on', true);
	DELETE FROM ost WHERE id > 100000;
	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH: %s rows through the cursor, %s ordinary; first %s / %s',
					  cardinality(a), cardinality(b), a[1], b[1]);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN used THEN 'LionOrdered' ELSE 'no LionOrdered' END,
				  cardinality(a));
END $$;
SELECT os_cursor_split($$
	SELECT f.id, f.ts FROM ost f
	 WHERE f.tags && ARRAY['t1'] AND f.ts < timestamptz '2025-03-01 00:00+00'
	 ORDER BY f.ts DESC, f.id LIMIT 200$$, true);
SELECT os_cursor_split($$
	SELECT f.id, f.ts FROM ost f
	 WHERE f.tags && ARRAY['u1'] AND f.ts > timestamptz '2025-02-01 00:00+00'
	 ORDER BY f.ts, f.id LIMIT 200$$, false);
DROP FUNCTION os_cursor_split(text, boolean);
VACUUM ost;

-- 6. A dirty heap: new ts values (the newest ones among them), new tags, HOT
--    updates, deletes, NULLs made and cleared - then VACUUM.
UPDATE osf SET ts = ts + interval '400 days' WHERE id % 211 = 0;
UPDATE osf SET tags = ARRAY['t1'] WHERE id % 223 = 0;
UPDATE osf SET name = name || 'x' WHERE id % 7 = 0;
UPDATE osf SET ts = NULL WHERE id % 229 = 0;
UPDATE osf SET ts = timestamptz '2025-05-01 00:00+00' WHERE id % 89 = 0 AND id % 2 = 0;
DELETE FROM osf WHERE id % 13 = 0;
UPDATE ost SET ts = ts + interval '400 days' WHERE id % 211 = 0;
UPDATE ost SET fk = NULL WHERE id % 227 = 0;
DELETE FROM ost WHERE id % 13 = 0;
SELECT os_cmp($$
	SELECT f.id, f.fk, f.ts FROM osf f
	 WHERE f.kind = 'a' AND f.tags && ARRAY['t1', 't2', 't3']
	   AND f.ts >= timestamptz '2025-03-01 00:00+00'
	   AND f.fk IN (SELECT d.pk FROM osd d WHERE d.grp = 1 AND d.flag)
	 ORDER BY f.ts DESC, f.id LIMIT 25$$, true);
SELECT os_cmp($$
	SELECT f.id, f.ts FROM osf f WHERE f.kind = 'a' AND f.tags && ARRAY['t1']
	 ORDER BY f.ts DESC, f.id LIMIT 40$$, true);
SELECT os_cmp($$
	SELECT f.id, f.ts FROM ost f WHERE f.tags && ARRAY['t9'] AND f.ts > timestamptz '2025-02-15 00:00+00'
	 ORDER BY f.ts, f.id LIMIT 40$$, true);
SELECT os_cmp($$
	SELECT f.id, f.fk, f.ts FROM ost f
	 WHERE f.fk IN (SELECT d.pk FROM osd d WHERE d.grp = 1 AND d.flag)
	 ORDER BY f.ts DESC NULLS LAST, f.id LIMIT 40$$, true);
VACUUM osf;
VACUUM ost;
SELECT os_cmp($$
	SELECT f.id, f.fk, f.ts FROM osf f
	 WHERE f.kind = 'a' AND f.tags && ARRAY['t1', 't2', 't3']
	   AND f.ts >= timestamptz '2025-03-01 00:00+00'
	   AND f.fk IN (SELECT d.pk FROM osd d WHERE d.grp = 1 AND d.flag)
	 ORDER BY f.ts DESC, f.id LIMIT 25$$, true);
SELECT os_cmp($$
	SELECT f.id, f.ts FROM osf f WHERE f.kind = 'a' AND f.tags && ARRAY['t1']
	 ORDER BY f.ts DESC, f.id LIMIT 40$$, true);
SELECT os_cmp($$
	SELECT f.id, f.ts FROM ost f WHERE f.tags && ARRAY['t9'] AND f.ts > timestamptz '2025-02-15 00:00+00'
	 ORDER BY f.ts, f.id LIMIT 40$$, true);
SELECT os_cmp($$
	SELECT f.id, f.fk, f.ts FROM ost f
	 WHERE f.fk IN (SELECT d.pk FROM osd d WHERE d.grp = 1 AND d.flag)
	 ORDER BY f.ts DESC NULLS LAST, f.id LIMIT 40$$, true);
SELECT lion_index_verify('osf_a_tags_ts_fk_idx'), lion_index_verify('ost_l');

-- 7. Parallel: the walk is not parallel-safe, so a parallel plan keeps it
--    above the Gather's side, or does not use it; the answer is the same.
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SELECT * FROM os_plan($$
	SELECT f.id, f.fk, f.ts FROM osf f
	 WHERE f.kind = 'a' AND f.tags && ARRAY['t1', 't2', 't3']
	   AND f.ts >= timestamptz '2025-03-01 00:00+00'
	   AND f.fk IN (SELECT d.pk FROM osd d WHERE d.grp = 1 AND d.flag)
	 ORDER BY f.ts DESC LIMIT 10$$);
SELECT os_cmp($$
	SELECT f.id, f.fk, f.ts FROM osf f
	 WHERE f.kind = 'a' AND f.tags && ARRAY['t1', 't2', 't3']
	   AND f.ts >= timestamptz '2025-03-01 00:00+00'
	   AND f.fk IN (SELECT d.pk FROM osd d WHERE d.grp = 1 AND d.flag)
	 ORDER BY f.ts DESC, f.id LIMIT 25$$);
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;

-- 8. A btree's walk over partitions (DESIGN.md §30.8's v2): ORDER BY a btree
--    column of each partition under a lion filter.
CREATE INDEX osf_id ON osf (id);
ANALYZE osf;
SELECT * FROM os_plan($$
	SELECT f.id FROM osf f WHERE f.tags @> ARRAY['rare'] ORDER BY f.id LIMIT 5$$);
SELECT os_cmp($$
	SELECT f.id FROM osf f WHERE f.tags @> ARRAY['rare'] ORDER BY f.id LIMIT 5$$);
-- a five-row LIMIT over one partition of 11,000 rows goes to core's backward
-- scan with a filter: so short a walk does not pay the lion lookups' start-up
-- (DESIGN.md §40.3), and the two run in the same time
SELECT os_cmp($$
	SELECT f.id FROM osf f WHERE f.kind = 'b' AND f.tags @> ARRAY['t3', 'u4']
	 ORDER BY f.id DESC LIMIT 5$$, true);
DROP INDEX osf_id;

-- 9. Not offered: with the node off; for a GROUP BY of the column, which
--    wants every row.
SET pg_lion.enable_ordered_scan = off;
SELECT os_uses($$
	SELECT f.id FROM osf f WHERE f.kind = 'a' AND f.tags && ARRAY['t1']
	 ORDER BY f.ts DESC LIMIT 10$$);
RESET pg_lion.enable_ordered_scan;
SELECT os_uses($$
	SELECT f.ts, count(*) FROM ost f WHERE f.tags && ARRAY['t1']
	 GROUP BY f.ts$$);

DROP FUNCTION os_cmp(text, boolean, text);
DROP FUNCTION os_plan(text);
DROP FUNCTION os_uses(text);
DROP FUNCTION os_run(text);
DROP TABLE osf, ost, osd;
