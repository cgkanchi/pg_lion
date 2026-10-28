-- Aggregates of bit-sliced columns over a partitioned table that plan-time
-- pruning leaves ONE leaf of (DESIGN.md §52.13, §16, "A parent pruned to one
-- leaf"): the cases of slices_partitioned.sql that prune to a single leaf,
-- through a sub-partitioned parent, to an empty leaf, to the NULL partition
-- of a LIST and to one leaf of a HASH.
--
-- PostgreSQL 18.0 to 18.6 lose a disabled path's disabled_nodes in an
-- Append of one child (fixed upstream in 18.7, "Propagate disabled_nodes to
-- single-child Append paths"), so the ordinary plan the checks below
-- disable is not disabled there, and it wins on cost against the slices on
-- data this small.  The test stops at once there, and on builds of the 18
-- branch stamped 18.6 that have the fix, which the version cannot tell
-- apart (slices_partitioned_leaf_1.out).
SELECT current_setting('server_version_num')::int BETWEEN 180000 AND 180006
	AS append_drops_disabled \gset
\if :append_drops_disabled
\quit
\endif
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;
SET TimeZone = 'UTC';
SET DateStyle = 'ISO, YMD';

/*
 * spt_q() runs q with the lion paths preferred - every other scan disabled -
 * then with the pushdown off, and compares the two as multisets of rows as
 * text, so that a sum's numeric, an average's scale and a timestamp's
 * format have to be core's to the character.  It says what the node did:
 * answered aggregates from slices, counts only, or nothing.
 */
CREATE FUNCTION spt_q(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	used boolean := false;
	slices boolean := false;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(LionCount\)' THEN
			used := true;
		END IF;
		IF ln ~ 'Aggregates From Slices:' THEN
			slices := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE spt_on AS SELECT s::text AS r FROM (%s) s', q);

	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE format('CREATE TEMP TABLE spt_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM spt_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM spt_on EXCEPT ALL SELECT * FROM spt_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM spt_off EXCEPT ALL SELECT * FROM spt_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE spt_on, spt_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN slices THEN 'from slices'
					   WHEN used THEN 'counts only' ELSE 'no pushdown' END,
				  nrows);
END $$;

-- The plan of q with the lion paths preferred, as EXPLAIN (COSTS OFF)
-- prints it, or EXPLAIN ANALYZE's lines of the node that depend on nothing
-- but the data.
CREATE FUNCTION spt_plan(q text, ana bool DEFAULT false) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	FOR ln IN EXECUTE CASE WHEN ana
					  THEN 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) '
					  ELSE 'EXPLAIN (COSTS OFF) ' END || q LOOP
		IF NOT ana OR
		   ln ~ '(LionCount|Partitions|Lion Indexes|Implied by|Aggregates From Slices|Range From Slices|Slices Located|Slices Absent|Slice Counts|Slice WHERE Copies|Slice Descent Steps|Slice Range|Slice Heap)' THEN
			RETURN NEXT regexp_replace(regexp_replace(ln, '\s+\(actual.*$', ''),
									   '(Slice Heap (TIDs|Blocks) Rechecked): [1-9][0-9]*$', '\1: >0');
		END IF;
	END LOOP;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
END $$;

-- ---------- 1. RANGE partitions, every type of the allowlist ----------
-- Four leaves of the range on k: two of rows, one with no row, and one
-- sub-partitioned by LIST on g (with a default); and a fifth attached with
-- two columns dropped and the rest in another order, whose own sliced index
-- puts the columns at other key positions than its siblings' - which the
-- parent's index does not match, so the leaf gets the parent's too, after
-- its own.  NULLs, negatives, the types' extremes and infinities, and int8
-- values whose sum is far past the int8 range.
CREATE TABLE spr (k int, g int, h int, a int2, b int4, c int8, d date,
				  ts timestamp, tz timestamptz, pad text)
	PARTITION BY RANGE (k);
CREATE TABLE spr_1 PARTITION OF spr FOR VALUES FROM (0) TO (100)
	WITH (autovacuum_enabled = off);
CREATE TABLE spr_2 PARTITION OF spr FOR VALUES FROM (100) TO (200)
	WITH (autovacuum_enabled = off);
CREATE TABLE spr_empty PARTITION OF spr FOR VALUES FROM (200) TO (300)
	WITH (autovacuum_enabled = off);
CREATE TABLE spr_4 PARTITION OF spr FOR VALUES FROM (300) TO (400)
	PARTITION BY LIST (g);
CREATE TABLE spr_4a PARTITION OF spr_4 FOR VALUES IN (0, 1, 2)
	WITH (autovacuum_enabled = off);
CREATE TABLE spr_4b PARTITION OF spr_4 DEFAULT
	WITH (autovacuum_enabled = off);
CREATE TABLE spr_5 (x1 int, tz timestamptz, ts timestamp, x2 text, d date,
					c int8, b int4, a int2, h int, g int, k int, pad text)
	WITH (autovacuum_enabled = off);
ALTER TABLE spr_5 DROP COLUMN x1, DROP COLUMN x2;
CREATE INDEX spr_5_own ON spr_5 USING lion (tz, d, c, a, b, ts) WITH (slices = on);
ALTER TABLE spr ATTACH PARTITION spr_5 FOR VALUES FROM (400) TO (500);
CREATE INDEX spr_g ON spr USING lion (g);
CREATE INDEX spr_h ON spr USING lion (h);
CREATE INDEX spr_v ON spr USING lion (a, b, c, d, ts, tz) WITH (slices = on);
INSERT INTO spr
SELECT CASE WHEN i % 4 = 0 THEN i % 100
			WHEN i % 4 = 1 THEN 100 + i % 100
			WHEN i % 4 = 2 THEN 300 + i % 100
			ELSE 400 + i % 100 END,
	   i % 7, i % 3,
	   CASE WHEN i % 17 = 0 THEN NULL ELSE (i * 13) % 2001 - 1000 END,
	   CASE WHEN i % 19 = 0 THEN NULL ELSE (i * 7919) % 200001 - 100000 END,
	   CASE WHEN i % 23 = 0 THEN NULL
			ELSE 4000000000000000000 + (i::int8 * 1000003) % 1000000000 END,
	   CASE WHEN i % 29 = 0 THEN NULL ELSE date '2000-01-01' + (i * 7) % 20000 - 10000 END,
	   CASE WHEN i % 37 = 0 THEN NULL
			ELSE timestamp '2000-01-01' + (i * 7777) % 1000000 * interval '1 min' END,
	   CASE WHEN i % 41 = 0 THEN NULL
			ELSE timestamptz '2020-01-01 00:00+00' + (i * 3331) % 100000 * interval '1 s' END,
	   repeat('x', 40)
  FROM generate_series(1, 6000) i;
-- the extremes, in two partitions; a row of NULLs in a third
INSERT INTO spr VALUES
	(5, 3, 0, -32768, -2147483648, -9223372036854775808, '-infinity',
	 '-infinity', '-infinity', 'least'),
	(450, 3, 1, 32767, 2147483647, 9223372036854775807, 'infinity',
	 'infinity', 'infinity', 'greatest'),
	(150, 3, 2, NULL, NULL, NULL, NULL, NULL, NULL, 'nulls');
VACUUM (FREEZE, ANALYZE) spr;

-- ---------- 2. pruning to one leaf ----------
-- one leaf, one leaf of the sub-partitioned one, and the empty one
SELECT * FROM spt_plan('SELECT sum(b), max(c) FROM spr WHERE k >= 100 AND k < 200 AND g = 3');
SELECT spt_q('SELECT sum(b), max(c), count(*) FROM spr WHERE k >= 100 AND k < 200 AND g = 3');
SELECT spt_q('SELECT sum(b), avg(a), min(d) FROM spr WHERE k >= 300 AND k < 400 AND g = 1');
SELECT spt_q('SELECT sum(b), avg(a), min(d) FROM spr WHERE k >= 300 AND k < 400 AND g = 5');
SELECT spt_q('SELECT count(b), sum(b), avg(b), min(b), max(b) FROM spr WHERE k >= 200 AND k < 300');

-- ---------- 3. LIST and HASH partitions, and two levels ----------
-- LIST by a text key with a NULL partition and a default, and the leaves of
-- a HASH partitioning under one of them
CREATE TABLE spl (kind text, g int, v int4, w int8, d date)
	PARTITION BY LIST (kind);
CREATE TABLE spl_a PARTITION OF spl FOR VALUES IN ('a')
	WITH (autovacuum_enabled = off);
CREATE TABLE spl_bc PARTITION OF spl FOR VALUES IN ('b', 'c')
	WITH (autovacuum_enabled = off);
CREATE TABLE spl_null PARTITION OF spl FOR VALUES IN (NULL)
	WITH (autovacuum_enabled = off);
CREATE TABLE spl_h PARTITION OF spl FOR VALUES IN ('h') PARTITION BY HASH (g);
CREATE TABLE spl_h0 PARTITION OF spl_h FOR VALUES WITH (MODULUS 3, REMAINDER 0)
	WITH (autovacuum_enabled = off);
CREATE TABLE spl_h1 PARTITION OF spl_h FOR VALUES WITH (MODULUS 3, REMAINDER 1)
	WITH (autovacuum_enabled = off);
CREATE TABLE spl_h2 PARTITION OF spl_h FOR VALUES WITH (MODULUS 3, REMAINDER 2)
	WITH (autovacuum_enabled = off);
CREATE TABLE spl_def PARTITION OF spl DEFAULT WITH (autovacuum_enabled = off);
CREATE INDEX spl_g ON spl USING lion (g);
CREATE INDEX spl_v ON spl USING lion (v, w, d) WITH (slices = on);
INSERT INTO spl
SELECT (ARRAY['a', 'b', 'c', NULL, 'h', 'h', 'z'])[1 + i % 7], i % 11,
	   CASE WHEN i % 5 = 0 THEN NULL ELSE i * 101 - 200000 END,
	   CASE WHEN i % 9 = 0 THEN NULL ELSE i::int8 * 2000000000000000 END,
	   CASE WHEN i % 6 = 0 THEN NULL ELSE date '2010-01-01' + i END
  FROM generate_series(1, 4000) i;
VACUUM (FREEZE, ANALYZE) spl;
-- the NULL partition, and one leaf of the hashed ones through the hash key
SELECT spt_q('SELECT sum(v), max(w), count(*) FROM spl WHERE kind IS NULL AND g = 4');
SELECT spt_q('SELECT sum(v), min(w), count(*) FROM spl WHERE kind = ''h'' AND g = 4');
SELECT * FROM spt_plan('SELECT sum(v), min(w), count(*) FROM spl WHERE kind = ''h'' AND g = 4');

DROP TABLE spr, spl;
DROP FUNCTION spt_q(text), spt_plan(text, bool);
