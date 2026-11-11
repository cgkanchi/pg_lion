-- Statement-level insert batching (DESIGN.md §55): after a statement's first
-- row, the (key, TID) pairs of a column that stores nothing but its posting
-- sets and bit slices are buffered per index and written sorted by key at the
-- end of the statement, or before anything in the backend reads the index.
--
-- PostgreSQL 16 has no aminsertcleanup and buffers nothing, so every count
-- of buffered rows below goes through batched(n), which is n on 17 and later
-- and 0 on 16, and the output is the same on both.
--
-- lion_debug_defer_cleanup(false) makes the end of a statement leave its
-- rows buffered, as a caller of index_insert() that never calls
-- index_insert_cleanup() would; that is how the other ways in are reached.
\set VERBOSITY terse
SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS pg_lion;

CREATE FUNCTION lion_debug_defer_pending(idx regclass) RETURNS int8
	AS 'pg_lion', 'lion_debug_defer_pending' LANGUAGE C STRICT;
CREATE FUNCTION lion_debug_defer_cleanup(on_ bool) RETURNS bool
	AS 'pg_lion', 'lion_debug_defer_cleanup' LANGUAGE C STRICT;

CREATE FUNCTION batched(n int8) RETURNS int8 LANGUAGE sql AS $$
	SELECT CASE WHEN current_setting('server_version_num')::int >= 170000
				THEN n ELSE 0 END
$$;

-- a count by the count pushdown (a custom scan, which reads the index
-- without a scan of it - so it goes first), and an index (bitmap) scan, give
-- the count and sum a sequential scan gives
CREATE OR REPLACE FUNCTION lb_cmp(tbl text, pred text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	q text := format('SELECT count(*) AS c, coalesce(sum(i), 0) AS s FROM %s WHERE %s', tbl, pred);
	n int8;
	a record;
	b record;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	EXECUTE format('SELECT count(*) FROM %s WHERE %s', tbl, pred) INTO n;
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE q INTO a;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	EXECUTE q INTO b;
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	IF a IS DISTINCT FROM b OR n IS DISTINCT FROM b.c THEN
		RETURN format('MISMATCH count=%s index=%s seqscan=%s', n, a::text, b::text);
	END IF;
	RETURN format('ok %s rows', a.c);
END $$;

-- ---------------------------------------------------------------------
-- A batch of every kind of key: a few values (CHAIN entries), many
-- values (INLINE ones), NULLs, long text keys, multi-key arrays with
-- empty ones, in several columns of one index; and work_mem small
-- enough that the buffer is written several times within the statement.
-- ---------------------------------------------------------------------
CREATE TABLE lb (i int4, k int4, t text, a int4[]) WITH (autovacuum_enabled = off);
CREATE INDEX lb_kta ON lb USING lion (k, t, a);
CREATE INDEX lb_t ON lb USING lion (t);

SET work_mem = '64kB';
INSERT INTO lb
SELECT g,
	   CASE WHEN g % 50 = 0 THEN NULL ELSE g % 3 END,
	   CASE WHEN g % 7 = 0 THEN repeat('long-' || (g % 4), 600)
			ELSE 'v' || (g % 997) END,
	   CASE WHEN g % 13 = 0 THEN '{}'::int4[]
			WHEN g % 17 = 0 THEN NULL
			ELSE ARRAY[g % 5, g % 101] END
FROM generate_series(1, 30000) g;
RESET work_mem;

SELECT lion_index_verify('lb_kta', true), lion_index_verify('lb_t', true);
SELECT lb_cmp('lb', 'k = 1');
SELECT lb_cmp('lb', 'k IS NULL');
SELECT lb_cmp('lb', 't = ''v5''');
SELECT lb_cmp('lb', 't = repeat(''long-2'', 600)');
SELECT lb_cmp('lb', 'a @> ARRAY[3]');
SELECT lb_cmp('lb', 'a @> ARRAY[77]');
SELECT lb_cmp('lb', 'a = ''{}''');
SELECT lb_cmp('lb', 'a IS NULL');

-- a second batch into the same keys: the CHAIN entries take their rows
-- in holds, and rows already there stay once
INSERT INTO lb SELECT g, g % 3, 'v' || (g % 997), ARRAY[g % 5]
FROM generate_series(30001, 40000) g;
SELECT lion_index_verify('lb_kta', true), lion_index_verify('lb_t', true);
SELECT lb_cmp('lb', 'k = 2');
SELECT lb_cmp('lb', 't = ''v5''');
SELECT lb_cmp('lb', 'a @> ARRAY[4]');

-- the same through COPY
COPY lb FROM STDIN;
40001	1	v5	{4,9}
40002	1	v6	{4}
40003	2	\N	\N
40004	\N	v5	{}
\.
SELECT lion_index_verify('lb_kta', true);
SELECT lb_cmp('lb', 'k = 1');
SELECT lb_cmp('lb', 't = ''v5''');
SELECT lb_cmp('lb', 'a @> ARRAY[4]');

-- ---------------------------------------------------------------------
-- Readers in the middle of the statement see the rows it wrote before:
-- a volatile function in the INSERT's own SELECT list, and a row trigger.
-- ---------------------------------------------------------------------
CREATE TABLE lb_mid (i int4, k int4) WITH (autovacuum_enabled = off);
CREATE INDEX lb_mid_k ON lb_mid USING lion (k);
CREATE TABLE lb_log (tag text, pending int8, viaindex text);

CREATE FUNCTION lb_probe(g int4) RETURNS int4 LANGUAGE plpgsql VOLATILE AS $$
BEGIN
	IF g = 500 THEN
		-- names no table: buffered rows stay buffered
		INSERT INTO lb_log SELECT 'select list', lion_debug_defer_pending('lb_mid_k'), NULL;
		-- names lb_mid: they are written before it starts
		INSERT INTO lb_log SELECT 'select list, after', NULL, lb_cmp('lb_mid', 'k = 1');
	END IF;
	RETURN g % 2;
END $$;

INSERT INTO lb_mid SELECT g, lb_probe(g) FROM generate_series(1, 1000) g;
SELECT tag, pending = batched(498) AS pending_ok, viaindex FROM lb_log ORDER BY tag;

CREATE FUNCTION lb_trig() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
	IF NEW.i = 2500 THEN
		INSERT INTO lb_log SELECT 'trigger', lion_debug_defer_pending('lb_mid_k'), NULL;
		INSERT INTO lb_log SELECT 'trigger, after', NULL, lb_cmp('lb_mid', 'k = 0');
	END IF;
	RETURN NEW;
END $$;
CREATE TRIGGER lb_mid_t BEFORE INSERT ON lb_mid FOR EACH ROW EXECUTE FUNCTION lb_trig();

TRUNCATE lb_log;
INSERT INTO lb_mid SELECT g, g % 2 FROM generate_series(2001, 3000) g;
SELECT tag, pending = batched(498) AS pending_ok, viaindex FROM lb_log ORDER BY tag;
DROP TRIGGER lb_mid_t ON lb_mid;

-- an AFTER trigger's query runs before the statement's indexes are closed
CREATE FUNCTION lb_after() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
	INSERT INTO lb_log SELECT 'after statement', NULL, lb_cmp('lb_mid', 'k = 1');
	RETURN NULL;
END $$;
CREATE TRIGGER lb_mid_a AFTER INSERT ON lb_mid FOR EACH STATEMENT EXECUTE FUNCTION lb_after();
TRUNCATE lb_log;
INSERT INTO lb_mid SELECT g, g % 2 FROM generate_series(3001, 4000) g;
SELECT tag, viaindex FROM lb_log ORDER BY tag;
DROP TRIGGER lb_mid_a ON lb_mid;

SELECT lion_index_verify('lb_mid_k', true);

-- ---------------------------------------------------------------------
-- Rows left buffered past the end of their statement.
-- ---------------------------------------------------------------------
CREATE TABLE lb_left (i int4, k int4) WITH (autovacuum_enabled = off);
CREATE INDEX lb_left_k ON lb_left USING lion (k);
SELECT lion_debug_defer_cleanup(false);

-- (each statement of its own commits, which writes them: so these are
-- in transaction blocks)
BEGIN;
-- the statement's first row goes straight in
INSERT INTO lb_left VALUES (1, 1);
SELECT lion_debug_defer_pending('lb_left_k') AS pending;

-- a query on the table writes them first
INSERT INTO lb_left SELECT g, g % 4 FROM generate_series(2, 101) g;
SELECT lion_debug_defer_pending('lb_left_k') = batched(99) AS pending_ok;
SELECT lb_cmp('lb_left', 'k = 1');
SELECT lion_debug_defer_pending('lb_left_k') AS pending;

-- so does a direct reader that names the index
INSERT INTO lb_left SELECT g, g % 4 FROM generate_series(102, 201) g;
SELECT lion_debug_defer_pending('lb_left_k') = batched(99) AS pending_ok;
SELECT lion_index_verify('lb_left_k', true);
SELECT lion_debug_defer_pending('lb_left_k') AS pending;
COMMIT;

-- ... and the commit
BEGIN;
INSERT INTO lb_left SELECT g, g % 4 FROM generate_series(202, 301) g;
SELECT lion_debug_defer_pending('lb_left_k') = batched(99) AS pending_ok;
COMMIT;
SELECT lion_debug_defer_pending('lb_left_k') AS pending;
SELECT lion_index_verify('lb_left_k', true);
SELECT lb_cmp('lb_left', 'k = 2');

-- a subtransaction's rows: dropped with it when it aborts, the parent's
-- written first when it starts inserting, and the parent's when released
BEGIN;
INSERT INTO lb_left SELECT g, g % 4 FROM generate_series(302, 401) g;
SAVEPOINT s1;
INSERT INTO lb_left SELECT g, 9 FROM generate_series(402, 451) g;
SELECT lion_debug_defer_pending('lb_left_k') = batched(49) AS pending_ok;
ROLLBACK TO s1;
SELECT lion_debug_defer_pending('lb_left_k') AS pending;
SAVEPOINT s2;
INSERT INTO lb_left SELECT g, 5 FROM generate_series(452, 501) g;
RELEASE s2;
SELECT lion_debug_defer_pending('lb_left_k') = batched(49) AS pending_ok;
COMMIT;
SELECT lion_index_verify('lb_left_k', true);
SELECT lb_cmp('lb_left', 'k = 9');
SELECT lb_cmp('lb_left', 'k = 5');
SELECT lb_cmp('lb_left', 'k = 3');

-- an error caught in a PL/pgSQL block aborts its subtransaction
DO $$
BEGIN
	BEGIN
		INSERT INTO lb_left SELECT g, 7 FROM generate_series(502, 600) g;
		RAISE EXCEPTION 'undo';
	EXCEPTION WHEN raise_exception THEN
		NULL;
	END;
END $$;
SELECT lion_debug_defer_pending('lb_left_k') AS pending;
SELECT lion_index_verify('lb_left_k', true);
SELECT lb_cmp('lb_left', 'k = 7');

-- a utility command writes them first: here TRUNCATE, after which the
-- table's TIDs start over
BEGIN;
INSERT INTO lb_left SELECT g, g % 4 FROM generate_series(601, 700) g;
TRUNCATE lb_left;
SELECT lion_debug_defer_pending('lb_left_k') AS pending;
INSERT INTO lb_left SELECT g, 6 FROM generate_series(701, 800) g;
COMMIT;
SELECT lion_index_verify('lb_left_k', true);
SELECT lb_cmp('lb_left', 'k = 6');
SELECT lb_cmp('lb_left', 'k = 1');

-- an index dropped with rows waiting for it
BEGIN;
CREATE INDEX lb_left_k2 ON lb_left USING lion (k);
INSERT INTO lb_left SELECT g, 8 FROM generate_series(801, 900) g;
SELECT lion_debug_defer_pending('lb_left_k2') = batched(99) AS pending_ok;
DROP INDEX lb_left_k2;
COMMIT;
SELECT lion_index_verify('lb_left_k', true);
SELECT lb_cmp('lb_left', 'k = 8');

-- pg_lion.batch_inserts = off writes every row now
BEGIN;
SET LOCAL pg_lion.batch_inserts = off;
INSERT INTO lb_left SELECT g, 4 FROM generate_series(901, 1000) g;
SELECT lion_debug_defer_pending('lb_left_k') AS pending;
COMMIT;

-- nothing is buffered while an INSERT ... ON CONFLICT runs: a row that
-- loses its race is dead at once, and its TID may be reused before a batch
-- holding it is written
CREATE UNIQUE INDEX lb_left_i ON lb_left (i);
BEGIN;
INSERT INTO lb_left SELECT g, 3 FROM generate_series(951, 1100) g
	ON CONFLICT (i) DO NOTHING;
SELECT lion_debug_defer_pending('lb_left_k') AS pending;
WITH w AS (INSERT INTO lb_left SELECT g, 2 FROM generate_series(1051, 1200) g
		   ON CONFLICT (i) DO UPDATE SET k = 12 RETURNING i)
INSERT INTO lb_log SELECT 'cte', count(*), NULL FROM w;
SELECT lion_debug_defer_pending('lb_left_k') AS pending;
-- ... and once it is done, an ordinary INSERT buffers again
INSERT INTO lb_left SELECT g, 11 FROM generate_series(1201, 1300) g;
SELECT lion_debug_defer_pending('lb_left_k') = batched(99) AS pending_ok;
COMMIT;

SELECT lion_debug_defer_cleanup(true);
SELECT lion_index_verify('lb_left_k', true);
SELECT lb_cmp('lb_left', 'k = 3');
SELECT lb_cmp('lb_left', 'k = 12');
SELECT lb_cmp('lb_left', 'k = 2');
SELECT lb_cmp('lb_left', 'k = 11');

-- ---------------------------------------------------------------------
-- What is never buffered: an exclusion constraint's index, which checks
-- each row against the ones before it in the same statement, and columns
-- with summaries or positions beside plain ones in one index (a column
-- with slices is buffered, below).
-- ---------------------------------------------------------------------
CREATE TABLE lb_ex (a int, b text, EXCLUDE USING lion (a WITH =));
INSERT INTO lb_ex SELECT g, 'x' FROM generate_series(1, 100) g;
INSERT INTO lb_ex SELECT 1000 + g % 50, 'y' FROM generate_series(1, 100) g;	-- fails
SELECT count(*) FROM lb_ex;

CREATE TABLE lb_mix (i int4, s int4, a int4[], v int8, t text, d tsvector)
	WITH (autovacuum_enabled = off);
CREATE INDEX lb_mix_s ON lb_mix USING lion (s, a) WITH (summaries = on);
CREATE INDEX lb_mix_v ON lb_mix USING lion (v, t) WITH (slices = on);
CREATE INDEX lb_mix_d ON lb_mix USING lion (d, s) WITH (store_positions = on);
INSERT INTO lb_mix
SELECT g, g % 1000, ARRAY[g % 4], g * 7, 'w' || (g % 3),
	   to_tsvector('simple', 'w' || (g % 5) || ' x' || (g % 2) || ' w' || (g % 5))
FROM generate_series(1, 20000) g;
SELECT lion_index_verify('lb_mix_s', true), lion_index_verify('lb_mix_v', true),
	   lion_index_verify('lb_mix_d', true);
SELECT lb_cmp('lb_mix', 's BETWEEN 100 AND 300');
SELECT lb_cmp('lb_mix', 'a @> ARRAY[2]');
SELECT lb_cmp('lb_mix', 'v = 700');
SELECT lb_cmp('lb_mix', 't = ''w1''');
SELECT lb_cmp('lb_mix', 'd @@ ''w3 <-> x1''::tsquery');

-- ---------------------------------------------------------------------
-- Several rows of one key in one write: a new key's rows go into its
-- entry as it is made, an INLINE entry takes as many as its payload holds
-- under the limit in one rewrite (the rest after it spills), and a CHAIN
-- entry's rows for one container go in in one record.  inline_limit = 256
-- makes the three keys of 3,000 rows spill in the middle of the batch.
-- ---------------------------------------------------------------------
CREATE EXTENSION IF NOT EXISTS pg_walinspect;
CREATE TABLE lb_rows (i int4, k int4) WITH (autovacuum_enabled = off);
CREATE INDEX lb_rows_k ON lb_rows USING lion (k) WITH (inline_limit = 256);
INSERT INTO lb_rows
SELECT g, CASE WHEN g <= 9000 THEN 1000 + g % 3 ELSE g % 100 END
FROM generate_series(1, 12000) g;
SELECT entries, inline_entries, ntids FROM lion_index_stats('lb_rows_k');
SELECT lion_index_verify('lb_rows_k', true);
SELECT lb_cmp('lb_rows', 'k = 7');
SELECT lb_cmp('lb_rows', 'k = 1001');
SELECT lb_cmp('lb_rows', 'k BETWEEN 0 AND 99');

-- more rows under the same keys, and new ones
INSERT INTO lb_rows
SELECT g, CASE WHEN g % 2 = 0 THEN 1000 + g % 3 ELSE g % 150 END
FROM generate_series(12001, 14000) g;
SELECT entries, inline_entries, ntids FROM lion_index_stats('lb_rows_k');
SELECT lion_index_verify('lb_rows_k', true);
SELECT lb_cmp('lb_rows', 'k = 7');
SELECT lb_cmp('lb_rows', 'k = 121');
SELECT lb_cmp('lb_rows', 'k = 1002');

-- 400 rows of one CHAIN key, adjacent on the heap: a record for each
-- container they go into, not one for each row (on 16, which writes row
-- by row, the count is not asked).  The start is the insert position, not
-- the write position, which lags while earlier commits are unflushed.
SET synchronous_commit = on;	-- the commit flushes what is read back
SELECT pg_current_wal_insert_lsn() AS lb_lsn0 \gset
INSERT INTO lb_rows SELECT g, 1000 FROM generate_series(14001, 14400) g;
SELECT batched(1) = 0 OR count(*) < 40 AS a_record_a_container
  FROM pg_get_wal_records_info(:'lb_lsn0', pg_current_wal_lsn())
 WHERE resource_manager IN ('Generic', 'pg_lion');
RESET synchronous_commit;
SELECT lion_index_verify('lb_rows_k', true);
SELECT lb_cmp('lb_rows', 'k = 1000');

-- ---------------------------------------------------------------------
-- A sliced column (DESIGN.md §51): a row's bit slices are buffered with
-- its key, a pair a slice, and written after the column's keys, each
-- slice's rows together.  The statement's first row, of value 0, makes the
-- few slices its bits need, so the batch makes the rest, and
-- inline_limit = 256 spills them in the middle of it.  Then an UPDATE of
-- every row at fillfactor 70, whose new versions go partly on their own
-- page and partly at the end of the table, so that a slice's rows come in
-- out of TID order.
-- ---------------------------------------------------------------------
CREATE FUNCTION lb_slcmp(idx regclass, attno int2) RETURNS text
LANGUAGE plpgsql
SET enable_indexscan = off SET enable_bitmapscan = off
SET pg_lion.enable_count_pushdown = off AS $$
DECLARE
	l record;
	r record;
	col text := pg_get_indexdef(idx, attno, true);
BEGIN
	SELECT * INTO l FROM lion_index_slice_agg(idx, NULL, attno);
	EXECUTE format('SELECT count(%1$s) AS count, sum(%1$s)::numeric AS sum, min(%1$s)::text AS min, max(%1$s)::text AS max FROM lb_sl',
				   col) INTO r;
	IF (l.count, l.sum, l.min, l.max) IS NOT DISTINCT FROM (r.count, r.sum, r.min, r.max) THEN
		RETURN format('same: %s %s %s %s', l.count, l.sum, l.min, l.max);
	END IF;
	RETURN format('DIFFERENT: lion %s %s %s %s, query %s %s %s %s',
				  l.count, l.sum, l.min, l.max, r.count, r.sum, r.min, r.max);
END $$;

CREATE TABLE lb_sl (i int4, v int8, w int4)
	WITH (autovacuum_enabled = off, fillfactor = 70);
CREATE INDEX lb_sl_vw ON lb_sl USING lion (v, w)
	WITH (slices = on, inline_limit = 256);
INSERT INTO lb_sl
SELECT g, CASE WHEN g = 1 THEN 0 ELSE hashint4(g)::int8 * 1000003 END,
	   CASE WHEN g % 7 = 0 THEN NULL ELSE g % 50 - 25 END
FROM generate_series(1, 6000) g;
SELECT slice_entries, slice_tids FROM lion_index_stats('lb_sl_vw');
SELECT lion_index_verify('lb_sl_vw', true);
SELECT lb_slcmp('lb_sl_vw', 1::int2), lb_slcmp('lb_sl_vw', 2::int2);
SELECT lb_cmp('lb_sl', 'w = -3');
SELECT lb_cmp('lb_sl', 'w IS NULL');

UPDATE lb_sl SET v = -v, w = w + 1;
SELECT lion_index_verify('lb_sl_vw', true);
SELECT lb_slcmp('lb_sl_vw', 1::int2), lb_slcmp('lb_sl_vw', 2::int2);
SELECT lb_cmp('lb_sl', 'w = -3');

-- ---------------------------------------------------------------------
-- A column that stores positions (DESIGN.md §17).  A CHAIN key's rows go
-- into its position tree a run at a time, appended to the tree's last
-- chunk, which splits as it fills; an INLINE key's go into the entry's own
-- chunk with their TIDs in one rewrite, and inline_limit = 256 spills keys
-- in the middle of the batch; a key of one row is a new entry.  Then rows
-- that land below the ones the index has, in room a VACUUM freed, which are
-- merged in rather than appended - the m keys' into INLINE entries, the
-- common key's into its position tree.  What the index stores for every
-- key is compared with the rows' tsvectors.
-- ---------------------------------------------------------------------
CREATE FUNCTION lion_debug_key_positions(regclass, text,
	OUT tid tid, OUT positions int2[], OUT weights text[]) RETURNS SETOF record
	AS '$libdir/pg_lion' LANGUAGE C STRICT;
CREATE FUNCTION lb_doc(g int4) RETURNS tsvector LANGUAGE sql IMMUTABLE AS $$
	SELECT setweight(to_tsvector('simple',
		repeat('common ', 1 + g % 4) || 'w' || g % 40 || ' p' || g / 3 ||
		' r' || g || ' common m' || g % 400),
		(ARRAY['A', 'B', 'C', 'D'])[1 + g % 4]::"char")
$$;
CREATE TABLE lb_pos (i int4, d tsvector) WITH (autovacuum_enabled = off);
CREATE INDEX lb_pos_d ON lb_pos USING lion (d)
	WITH (store_positions = true, inline_limit = 256);
CREATE VIEW lb_pos_compare AS
WITH idx AS (
	SELECT l.lexeme, p.tid, p.positions, p.weights
	FROM (SELECT DISTINCT unnest(tsvector_to_array(d)) AS lexeme FROM lb_pos) l,
		 lion_debug_key_positions('lb_pos_d', l.lexeme) p),
heap AS (
	SELECT u.lexeme, lb_pos.ctid AS tid, u.positions, u.weights
	FROM lb_pos, unnest(d) u)
SELECT (SELECT count(*) FROM heap) AS heap_rows,
	   (SELECT count(*) FROM (SELECT * FROM idx EXCEPT ALL SELECT * FROM heap) x) AS only_index,
	   (SELECT count(*) FROM (SELECT * FROM heap EXCEPT ALL SELECT * FROM idx) x) AS only_heap;

INSERT INTO lb_pos SELECT g, lb_doc(g) FROM generate_series(1, 3000) g;
SELECT entries, inline_entries, ntids, position_pages > 2 AS position_pages
  FROM lion_index_stats('lb_pos_d');
SELECT lion_index_verify('lb_pos_d', true);
SELECT * FROM lb_pos_compare;
SELECT lb_cmp('lb_pos', $$d @@ 'common <-> w7'$$);
SELECT lb_cmp('lb_pos', $$d @@ 'common <2> common'$$);
SELECT lb_cmp('lb_pos', $$d @@ 'p100 <-> r301'$$);

-- room near the start of the table, which the next rows take first
DELETE FROM lb_pos WHERE i % 3 = 0 AND i <= 1500;
VACUUM lb_pos;
INSERT INTO lb_pos SELECT g, lb_doc(g) FROM generate_series(3001, 3800) g;
SELECT count(*) FILTER (WHERE ctid < '(5,0)') > 0 AS some_below,
	   count(*) FILTER (WHERE ctid > (SELECT max(ctid) FROM lb_pos WHERE i <= 3000)) > 0 AS some_above
  FROM lb_pos WHERE i > 3000;
SELECT lion_index_verify('lb_pos_d', true);
SELECT * FROM lb_pos_compare;
SELECT lb_cmp('lb_pos', $$d @@ 'common <-> w7'$$);
SELECT lb_cmp('lb_pos', $$d @@ 'w23 <-> p1101'$$);
SELECT lb_cmp('lb_pos', $$d @@ 'common <-> m5'$$);

-- ---------------------------------------------------------------------
-- A column with summaries (DESIGN.md §32): the batch writes its rows'
-- summaries after its keys, in key order, a bucket's run at a time.
-- Ascending keys raise the open bucket's key and close it once it is full,
-- bucket after bucket; a text column's keys come in no text order; then
-- rows between the keys there are and below all of them join closed
-- buckets, past what an INLINE summary holds.  lion_index_verify() compares
-- every summary with the keys of its bucket, and every TID is in one bucket
-- of each column (on 16, row by row, the text column makes fewer buckets,
-- which is not asked).
-- ---------------------------------------------------------------------
CREATE TABLE lb_sum (i int4, t int8, v text) WITH (autovacuum_enabled = off);
CREATE INDEX lb_sum_tv ON lb_sum USING lion (t, v)
	WITH (summaries = on, slices = on, summary_tids = 64, inline_limit = 256);
INSERT INTO lb_sum SELECT g, g * 10, 'v' || g FROM generate_series(1, 3000) g;
SELECT attno, summary_tids, summary_entries > 40 OR (attno = 2 AND batched(1) = 0) AS buckets
  FROM lion_index_stats('lb_sum_tv') ORDER BY attno;
SELECT lion_index_verify('lb_sum_tv', true);
SELECT lb_cmp('lb_sum', 't BETWEEN 1000 AND 20000');
SELECT lb_cmp('lb_sum', $$v < 'v5'$$);

-- more of them, and rows in the middle and below
INSERT INTO lb_sum SELECT g, g * 10, 'v' || g FROM generate_series(3001, 4000) g;
INSERT INTO lb_sum
SELECT 5000 + g, (g * 7919) % 40000, 'w' || (g * 31) % 997 FROM generate_series(1, 3000) g;
INSERT INTO lb_sum SELECT 9000 + g, -g, 'a' || g FROM generate_series(1, 500) g;
SELECT attno, summary_tids FROM lion_index_stats('lb_sum_tv') ORDER BY attno;
SELECT lion_index_verify('lb_sum_tv', true);
SELECT lb_cmp('lb_sum', 't BETWEEN 1000 AND 20000');
SELECT lb_cmp('lb_sum', 't < 500');
SELECT lb_cmp('lb_sum', 't >= 39990');
SELECT lb_cmp('lb_sum', $$v < 'v5'$$);
SELECT lb_cmp('lb_sum', $$v BETWEEN 'a' AND 'b'$$);

-- ---------------------------------------------------------------------
-- A partitioned table: one buffer per partition's index.
-- ---------------------------------------------------------------------
CREATE TABLE lb_part (i int4, k int4) PARTITION BY RANGE (i);
CREATE TABLE lb_part_1 PARTITION OF lb_part FOR VALUES FROM (0) TO (5000);
CREATE TABLE lb_part_2 PARTITION OF lb_part FOR VALUES FROM (5000) TO (10000);
CREATE INDEX lb_part_k ON lb_part USING lion (k);
INSERT INTO lb_part SELECT (g * 7919) % 10000, g % 5 FROM generate_series(1, 9999) g;
SELECT lion_index_verify('lb_part_1_k_idx', true), lion_index_verify('lb_part_2_k_idx', true);
SELECT lb_cmp('lb_part', 'k = 4');

DROP VIEW lb_pos_compare;
DROP TABLE lb, lb_mid, lb_log, lb_left, lb_ex, lb_mix, lb_rows, lb_sl, lb_pos,
	lb_sum, lb_part;
DROP FUNCTION lb_probe(int4), lb_trig(), lb_after(), lb_cmp(text, text), batched(int8),
	lb_slcmp(regclass, int2), lb_doc(int4),
	lion_debug_key_positions(regclass, text);
DROP FUNCTION lion_debug_defer_pending(regclass), lion_debug_defer_cleanup(bool);
