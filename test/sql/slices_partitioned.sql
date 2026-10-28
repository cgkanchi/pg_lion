-- Aggregates of bit-sliced columns over a PARTITIONED table, answered from
-- every partition's own slices by one LionCount node (DESIGN.md §52,
-- "Partitioned tables"; §16): count, sum, avg, min and max of a column each
-- leaf's lion index slices, the partitions' answers added up by the node.
-- Every answer is checked against the ordinary plan (the pushdown off), and
-- the plan that ran it is named.
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
		   ln ~ '(LionCount|Partitions|Lion Indexes|Aggregates From Slices|Slices Located|Slices Absent|Slice Counts|Slice WHERE Copies|Slice Descent Steps|Slice Heap)' THEN
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
-- spr_5 has the parent's index and its own, which it lists first
SELECT c.relname, i.indexrelid::regclass
  FROM pg_index i JOIN pg_class c ON c.oid = i.indrelid
 WHERE c.relname = 'spr_5' ORDER BY i.indexrelid;

-- the plans: the one row from every partition's slices, no index named
SELECT * FROM spt_plan('SELECT count(b), sum(b), avg(b), min(b), max(b) FROM spr WHERE g = 3');
SELECT * FROM spt_plan('SELECT sum(c), count(*) FROM spr');
SELECT * FROM spt_plan('SELECT max(tz), min(d), count(*) FROM spr WHERE h = 1 AND g IN (1, 2)');
SELECT * FROM spt_plan('SELECT count(b), sum(b), avg(b), min(b), max(b) FROM spr WHERE g = 3', true);

-- every type, under an equality on another indexed column
SELECT spt_q('SELECT count(a), sum(a), avg(a), min(a), max(a) FROM spr WHERE g = 3');
SELECT spt_q('SELECT count(b), sum(b), avg(b), min(b), max(b) FROM spr WHERE g = 3');
SELECT spt_q('SELECT count(c), sum(c), avg(c), min(c), max(c) FROM spr WHERE g = 3');
SELECT spt_q('SELECT count(d), min(d), max(d) FROM spr WHERE g = 3');
SELECT spt_q('SELECT count(ts), min(ts), max(ts) FROM spr WHERE g = 3');
SELECT spt_q('SELECT count(tz), min(tz), max(tz) FROM spr WHERE g = 3');
-- ... the answers themselves: the int8 sum is a numeric far past int8
SELECT count(c), sum(c), avg(c), min(a), max(b), min(d), max(ts), min(tz)
  FROM spr WHERE g = 3;
SET pg_lion.enable_count_pushdown = off;
SELECT count(c), sum(c), avg(c), min(a), max(b), min(d), max(ts), min(tz)
  FROM spr WHERE g = 3;
RESET pg_lion.enable_count_pushdown;
-- no WHERE: every row of every partition
SELECT spt_q('SELECT count(a), sum(a), avg(a), min(a), max(a), count(b), sum(b), avg(b), min(b), max(b) FROM spr');
SELECT spt_q('SELECT count(c), sum(c), avg(c), min(c), max(c), count(*) FROM spr');
SELECT spt_q('SELECT count(d), min(d), max(d), count(ts), min(ts), max(ts), count(tz), min(tz), max(tz) FROM spr');
-- several columns at once, beside count(*) and a filtered count
SELECT spt_q('SELECT sum(a), max(b), min(c), max(tz), count(*), count(*) FILTER (WHERE h = 1) FROM spr WHERE g <> 3');
-- WHEREs of other shapes: IN, OR, IS NULL, a range on another column, NOT IN
SELECT spt_q('SELECT sum(b), min(b), max(b) FROM spr WHERE g IN (1, 4, 6)');
SELECT spt_q('SELECT sum(c), avg(a) FROM spr WHERE g = 1 OR h = 2');
SELECT spt_q('SELECT count(b), sum(b), max(ts) FROM spr WHERE b IS NULL OR g = 0');
SELECT spt_q('SELECT sum(b), min(c) FROM spr WHERE a IS NOT NULL AND g = 5');
SELECT spt_q('SELECT sum(b), max(a), count(*) FROM spr WHERE g >= 2 AND g < 5 AND h = 0');
SELECT spt_q('SELECT sum(a), min(tz) FROM spr WHERE g NOT IN (1, 2) AND h = 1');
-- an equality on the sliced column itself, and a WHERE that selects nothing
SELECT spt_q('SELECT count(b), sum(b), min(b) FROM spr WHERE b = 2147483647');
SELECT spt_q('SELECT count(b), sum(b), avg(b), min(b), max(b), count(*) FROM spr WHERE g = 99');
-- a HAVING over the aggregates
SELECT spt_q('SELECT sum(b) FROM spr WHERE g = 2 HAVING sum(b) > 0');
SELECT spt_q('SELECT sum(b) FROM spr WHERE g = 2 HAVING sum(b) < 0');
-- expressions over the aggregates
SELECT spt_q('SELECT sum(a) + 1, max(c) - min(c), avg(b) * 2 FROM spr WHERE h = 2');

-- ---------- 2. pruning, and what the bounds imply ----------
-- a WHERE on the key that prunes to some partitions, to one leaf of the
-- sub-partitioned one, to the empty one, and to none at all
SELECT * FROM spt_plan('SELECT sum(b), max(c) FROM spr WHERE k >= 100 AND k < 200 AND g = 3');
SELECT spt_q('SELECT sum(b), max(c), count(*) FROM spr WHERE k >= 100 AND k < 200 AND g = 3');
SELECT spt_q('SELECT sum(b), max(c), count(*) FROM spr WHERE k < 200 AND g = 3');
SELECT spt_q('SELECT sum(b), avg(a), min(d) FROM spr WHERE k >= 300 AND k < 400 AND g = 1');
SELECT spt_q('SELECT sum(b), avg(a), min(d) FROM spr WHERE k >= 300 AND k < 400 AND g = 5');
SELECT spt_q('SELECT count(b), sum(b), avg(b), min(b), max(b) FROM spr WHERE k >= 200 AND k < 300');
SELECT spt_q('SELECT count(b), sum(b), avg(b), min(b), max(b), count(*) FROM spr WHERE k > 1000');
SELECT count(b), sum(b), avg(b), min(b), max(b), count(*) FROM spr WHERE k > 1000;
-- the key's clause implied by the partitions kept: they are counted whole
SELECT * FROM spt_plan('SELECT sum(b), min(tz) FROM spr WHERE k >= 300');
SELECT spt_q('SELECT sum(b), min(tz) FROM spr WHERE k >= 300');
-- with plan-time pruning off, the key's clause is a residual one (§16,
-- "Residual clauses over a partitioned table") - implied or refuted by
-- every partition's bounds, but residual in the plan - and the slices
-- decline a residual clause, as on one table
SET enable_partition_pruning = off;
SELECT spt_q('SELECT sum(b), max(c), count(*) FROM spr WHERE k < 200 AND g = 3');
RESET enable_partition_pruning;
-- a generic plan's parameter on another column, and on the key
PREPARE spr_p(int) AS SELECT count(b), sum(b), min(a), max(c) FROM spr WHERE g = $1;
SET plan_cache_mode = force_generic_plan;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET pg_lion.enable_plain_scan = off;
EXPLAIN (COSTS OFF) EXECUTE spr_p(3);
EXECUTE spr_p(3);
EXECUTE spr_p(99);
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET pg_lion.enable_plain_scan;
RESET plan_cache_mode;
SET pg_lion.enable_count_pushdown = off;
EXECUTE spr_p(3);
EXECUTE spr_p(99);
RESET pg_lion.enable_count_pushdown;
DEALLOCATE spr_p;
-- a LATERAL value: the node rescanned, every partition again
SELECT spt_q('SELECT v.g, s.* FROM (VALUES (1), (3), (99)) v(g),
			  LATERAL (SELECT sum(b), min(c), max(tz) FROM spr WHERE spr.g = v.g) s');

-- ---------- 3. a dirty heap, then VACUUM ----------
-- updates (some moving rows to another partition), deletes and inserts,
-- not vacuumed: the rows on pages the map does not vouch for are rechecked
-- in the heap, partition by partition
UPDATE spr SET b = -b, c = c - 1 WHERE k % 10 = 3;
UPDATE spr SET k = k + 100 WHERE k BETWEEN 10 AND 14;
DELETE FROM spr WHERE k % 10 = 7;
INSERT INTO spr (k, g, h, a, b, c, d, ts, tz)
SELECT 300 + i % 100, i % 7, i % 3, -i, i * 1000, 9000000000000000000 - i,
	   date '1990-01-01' + i, timestamp '1990-01-01' + i * interval '1 h',
	   timestamptz '1990-01-01 00:00+00' + i * interval '1 h'
  FROM generate_series(1, 300) i;
SELECT * FROM spt_plan('SELECT count(b), sum(b), avg(b), min(b), max(b) FROM spr WHERE g = 3', true);
SELECT spt_q('SELECT count(a), sum(a), avg(a), min(a), max(a) FROM spr WHERE g = 3');
SELECT spt_q('SELECT count(b), sum(b), avg(b), min(b), max(b) FROM spr WHERE g = 3');
SELECT spt_q('SELECT count(c), sum(c), avg(c), min(c), max(c) FROM spr');
SELECT spt_q('SELECT count(d), min(d), max(d), min(ts), max(tz) FROM spr WHERE h = 1');
-- this transaction's own rows at the extremes, and its deletes, rolled back
BEGIN;
INSERT INTO spr (k, g, h, a, b, c) VALUES
	(50, 3, 0, 32767, 2147483647, 9223372036854775807),
	(350, 3, 0, -32768, -2147483648, -9223372036854775808);
DELETE FROM spr WHERE g = 3 AND k >= 400;
SELECT spt_q('SELECT count(a), sum(a), min(a), max(a), count(b), sum(b), min(b), max(b), sum(c), min(c), max(c) FROM spr WHERE g = 3');
ROLLBACK;
SELECT spt_q('SELECT count(a), sum(a), min(a), max(a), count(b), sum(b), min(b), max(b), sum(c), min(c), max(c) FROM spr WHERE g = 3');
VACUUM (FREEZE, ANALYZE) spr;
SELECT * FROM spt_plan('SELECT count(b), sum(b), avg(b), min(b), max(b) FROM spr WHERE g = 3', true);
SELECT spt_q('SELECT count(b), sum(b), avg(b), min(b), max(b) FROM spr WHERE g = 3');
SELECT spt_q('SELECT count(c), sum(c), avg(c), min(c), max(c) FROM spr');
SELECT spt_q('SELECT count(d), min(d), max(d), min(ts), max(tz) FROM spr WHERE h = 1');

-- ---------- 4. LIST and HASH partitions, and two levels ----------
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
SELECT * FROM spt_plan('SELECT sum(v), max(w), min(d) FROM spl WHERE g = 4');
SELECT spt_q('SELECT count(v), sum(v), avg(v), min(v), max(v) FROM spl WHERE g = 4');
SELECT spt_q('SELECT count(w), sum(w), avg(w), min(w), max(w) FROM spl');
SELECT spt_q('SELECT count(d), min(d), max(d) FROM spl WHERE g IN (0, 10)');
-- pruned by the key, which the partitions kept imply: the NULL partition,
-- the hashed ones, one of them through the hash key, two lists
SELECT spt_q('SELECT sum(v), max(w), count(*) FROM spl WHERE kind IS NULL AND g = 4');
SELECT spt_q('SELECT sum(v), min(w), count(*) FROM spl WHERE kind = ''h''');
SELECT spt_q('SELECT sum(v), min(w), count(*) FROM spl WHERE kind = ''h'' AND g = 4');
SELECT spt_q('SELECT sum(v), avg(v), count(*) FROM spl WHERE kind IN (''a'', ''h'') AND g <> 4');
SELECT * FROM spt_plan('SELECT sum(v), min(w), count(*) FROM spl WHERE kind = ''h'' AND g = 4');
-- ... and where a partition kept does not imply it - 'b' of the list of two,
-- 'z' of the default - the clause on the unindexed key is tested on the
-- rows, a residual clause, which the slices decline
SELECT spt_q('SELECT sum(v), max(w) FROM spl WHERE kind = ''b'' AND g = 4');
SELECT spt_q('SELECT sum(v), avg(v), count(*) FROM spl WHERE kind = ''z''');

-- ---------- 5. declines ----------
-- a partition without a lion index slicing the column: the query keeps the
-- ordinary plan (or the counts alone), whatever the other partitions have
CREATE TABLE spd (k int, g int, v int4, w int8) PARTITION BY RANGE (k);
CREATE TABLE spd_1 PARTITION OF spd FOR VALUES FROM (0) TO (100);
CREATE TABLE spd_2 PARTITION OF spd FOR VALUES FROM (100) TO (200);
CREATE TABLE spd_3 PARTITION OF spd FOR VALUES FROM (200) TO (300);
INSERT INTO spd SELECT i % 300, i % 5, i, i::int8 * 7 FROM generate_series(1, 3000) i;
CREATE INDEX ON spd USING lion (g);
CREATE INDEX ON spd_1 USING lion (v, w) WITH (slices = on);
CREATE INDEX ON spd_2 USING lion (v, w) WITH (slices = on);
-- spd_3: v indexed, not sliced
CREATE INDEX ON spd_3 USING lion (v, w);
VACUUM (FREEZE, ANALYZE) spd;
SELECT spt_q('SELECT sum(v), max(w) FROM spd WHERE g = 2');
-- ... answered once pruning leaves only the partitions that slice it
SELECT spt_q('SELECT sum(v), max(w) FROM spd WHERE g = 2 AND k < 200');
-- a partial sliced index is no index for the slices, as on one table
CREATE INDEX ON spd_3 USING lion (v, w) WITH (slices = on) WHERE g < 3;
ANALYZE spd;
SELECT spt_q('SELECT sum(v), max(w) FROM spd WHERE g = 2');
-- nor one over an expression of the column
CREATE INDEX ON spd_3 USING lion ((v + 0), (w + 0)) WITH (slices = on);
ANALYZE spd;
SELECT spt_q('SELECT sum(v), max(w) FROM spd WHERE g = 2');
-- a whole one is, its columns in another order than its siblings'
CREATE INDEX ON spd_3 USING lion (w, v) WITH (slices = on);
ANALYZE spd;
SELECT spt_q('SELECT sum(v), max(w) FROM spd WHERE g = 2');
SELECT spt_q('SELECT sum(v), avg(w), min(v), max(w), count(*) FROM spd');
-- a GROUP BY over a partitioned table is not answered from slices (its
-- groups go up as partial aggregates; an average or an int8 sum would need
-- serialized partial states)
SELECT spt_q('SELECT g, sum(v), max(w) FROM spd GROUP BY g');
-- a residual clause, as on one table
SELECT spt_q('SELECT sum(v) FROM spd WHERE g = 2 AND w % 3 = 0');
-- the switch
SET pg_lion.enable_count_slices = off;
SELECT spt_q('SELECT sum(v), max(w) FROM spd WHERE g = 2');
RESET pg_lion.enable_count_slices;

-- a cursor over the one row: the partitions' indexes stay open, so a
-- REINDEX of one of them under it is refused as core refuses it
BEGIN;
SET LOCAL enable_seqscan = off;
SET LOCAL enable_bitmapscan = off;
SET LOCAL pg_lion.enable_plain_scan = off;
DECLARE spd_c CURSOR FOR SELECT sum(v), max(w) FROM spd WHERE g = 2;
FETCH spd_c;
REINDEX INDEX spd_1_v_w_idx;
ROLLBACK;

DROP TABLE spr, spl, spd;
DROP FUNCTION spt_q(text), spt_plan(text, bool);
