-- Bit-sliced key columns (DESIGN.md §51): the SLICE entries a column built
-- WITH (slices = on) keeps, lion_index_slice_agg() against the query it
-- stands for, verify, and the readers that must not see slices.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
CREATE EXTENSION IF NOT EXISTS pageinspect;
RESET client_min_messages;

-- The query lion_index_slice_agg(idx, filter, attno) stands for, with every
-- lion path off so that the code under test does not answer it:
--   SELECT count(x), sum(x), min(x), max(x) FROM t WHERE <pred> AND <filter>
-- x being key column attno, pred a partial index's predicate; sum only for
-- an integer column.
CREATE FUNCTION slc_ref(idx regclass, filter text, attno int2,
						OUT count bigint, OUT sum numeric, OUT min text,
						OUT max text)
LANGUAGE plpgsql
SET enable_indexscan = off SET enable_bitmapscan = off
SET pg_lion.enable_count_pushdown = off AS $$
DECLARE
	tbl regclass;
	col text;
	typ regtype;
	pred text;
	conds text[] := '{}';
BEGIN
	SELECT i.indrelid, pg_get_expr(i.indpred, i.indrelid)
	  INTO tbl, pred FROM pg_index i WHERE i.indexrelid = idx;
	col := pg_get_indexdef(idx, attno, true);
	SELECT a.atttypid INTO typ FROM pg_attribute a
	 WHERE a.attrelid = idx AND a.attnum = attno;
	IF pred IS NOT NULL THEN conds := conds || pred; END IF;
	IF filter IS NOT NULL THEN conds := conds || ('(' || filter || ')'); END IF;
	EXECUTE format('SELECT count(%1$s), %2$s, min(%1$s)::text, max(%1$s)::text FROM %3$s %4$s',
				   col,
				   CASE WHEN typ IN ('int2', 'int4', 'int8')
						THEN format('sum(%s)::numeric', col) ELSE 'NULL::numeric' END,
				   tbl,
				   CASE WHEN cardinality(conds) > 0
						THEN 'WHERE ' || array_to_string(conds, ' AND ') END)
	   INTO count, sum, min, max;
END $$;

-- The function's answer beside the query's: 'same' and the answer, or both.
CREATE FUNCTION slc_cmp(idx regclass, filter text DEFAULT NULL,
						attno int2 DEFAULT 1) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	l record;
	r record;
BEGIN
	SELECT * INTO l FROM lion_index_slice_agg(idx, filter, attno);
	SELECT * INTO r FROM slc_ref(idx, filter, attno);
	IF (l.count, l.sum, l.min, l.max) IS NOT DISTINCT FROM (r.count, r.sum, r.min, r.max) THEN
		RETURN format('same: %s %s %s %s', l.count, coalesce(l.sum::text, '-'),
					  coalesce(l.min, '-'), coalesce(l.max, '-'));
	END IF;
	RETURN format('DIFFERENT: lion %s %s %s %s, query %s %s %s %s',
				  l.count, l.sum, l.min, l.max, r.count, r.sum, r.min, r.max);
END $$;

-- A statement's SQLSTATE, message and detail, or 'ok'.
CREATE FUNCTION slc_err(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	detail text;
BEGIN
	EXECUTE q;
	RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
	GET STACKED DIAGNOSTICS detail = PG_EXCEPTION_DETAIL;
	RETURN SQLSTATE || ': ' || SQLERRM ||
		CASE WHEN detail <> '' THEN ' (' || detail || ')' ELSE '' END;
END $$;

-- The meta page's format version and sliced columns (DESIGN.md §51): the
-- little-endian words at 28 and 36 of block 0.
CREATE FUNCTION slc_meta(idx regclass, OUT version int8, OUT slice_cols int8)
LANGUAGE sql AS $$
	SELECT get_byte(p, 28)::int8 + get_byte(p, 29)::int8 * 256,
		   get_byte(p, 36)::int8 + get_byte(p, 37)::int8 * 256
	  FROM get_raw_page(idx::text, 0) p
$$;

-- ---------- 1. every type of the allowlist, its extremes, NULLs ----------
CREATE TABLE slc (id int, g int, tags text[], s text,
				  i2 int2, i4 int4, i8 int8, d date, tm time, ts timestamp,
				  tz timestamptz)
	WITH (autovacuum_enabled = off, fillfactor = 70);
INSERT INTO slc
SELECT i, i % 7, ARRAY['t' || (i % 5), 'u' || (i % 3)], 's' || (i % 4),
	   ((i * 37) % 2001 - 1000)::int2,
	   (i * 7919) % 100003 - 50000,
	   (i::int8 * 1000003) * (CASE WHEN i % 2 = 0 THEN 1 ELSE -1 END),
	   date '2000-01-01' + (i * 13) % 20000 - 10000,
	   time '00:00' + ((i * 61) % 86400) * interval '1 s',
	   timestamp '2020-01-01' + (i * 977) * interval '1 s' * (CASE WHEN i % 3 = 0 THEN -1 ELSE 1 END),
	   timestamptz '2020-01-01 00:00+00' + (i * 331) * interval '1 min'
  FROM generate_series(1, 6000) i;
-- NULLs in every sliced column, and each type's extremes
UPDATE slc SET i2 = NULL, i4 = NULL, i8 = NULL, d = NULL, tm = NULL, ts = NULL,
			   tz = NULL
 WHERE id % 50 = 0;
INSERT INTO slc VALUES
	(6001, 1, '{t1}', 's1', -32768, -2147483648, -9223372036854775808,
	 '-infinity', '00:00', '-infinity', '-infinity'),
	(6002, 1, '{t1}', 's1', 32767, 2147483647, 9223372036854775807,
	 'infinity', '24:00', 'infinity', 'infinity'),
	(6003, 2, '{t2}', 's2', 0, 0, 0, '2000-01-01', '12:00', '2000-01-01',
	 '2000-01-01 00:00+00'),
	(6004, 2, '{t2}', 's2', -1, -1, -1, '1999-12-31', '00:00:00.000001',
	 '1999-12-31 23:59:59.999999', '1999-12-31 23:59:59.999999+00');
CREATE INDEX slc_idx ON slc USING lion (i2, i4, i8, d, tm, ts, tz)
	WITH (slices = on);
CREATE INDEX slc_g ON slc USING lion (g);
CREATE INDEX slc_tags ON slc USING lion (tags);
VACUUM (FREEZE, ANALYZE) slc;

SELECT * FROM slc_meta('slc_idx');
SELECT attno, sliced, entries, ntids, null_tids, slice_entries, slice_tids
  FROM lion_index_stats('slc_idx');
SELECT lion_index_verify('slc_idx');
SELECT lion_index_verify('slc_idx', true);

SELECT a AS attno, f AS filter, slc_cmp('slc_idx', f, a::int2)
  FROM generate_series(1, 7) a,
	   unnest(ARRAY[NULL, 'g = 3', 'g IN (1, 4)', 'g = 2 OR tags @> ''{t1}''',
					'g = 5 AND tags @> ''{u2}''', 'g IS NULL', 'g = 99',
					'i4 = 0', 'i4 IN (-1, -2147483648, 2147483647)']) f
 ORDER BY a, f NULLS FIRST;

-- ---------- 2. what the function refuses ----------
CREATE INDEX slc_mc ON slc USING lion (i4, s) WITH (slices = on);
SELECT attno, sliced, slice_entries FROM lion_index_stats('slc_mc');
SELECT slc_cmp('slc_mc', 'g = 1');
SELECT slc_err($$SELECT * FROM lion_index_slice_agg('slc_mc', NULL, 2::int2)$$);
SELECT slc_err($$SELECT * FROM lion_index_slice_agg('slc_g')$$);
SELECT slc_err($$SELECT * FROM lion_index_slice_agg('slc_tags')$$);
SELECT slc_err($$SELECT * FROM lion_index_slice_agg('slc_idx', NULL, 9::int2)$$);
SELECT slc_err($$SELECT * FROM lion_index_slice_agg('slc_idx', NULL, 0::int2)$$);
SELECT slc_err($$SELECT * FROM lion_index_slice_agg('slc_idx', 'i4 > 5')$$);
SELECT slc_err($$SELECT * FROM lion_index_slice_agg('slc_idx', 'NOT g = 1')$$);
SELECT slc_err($$SELECT * FROM lion_index_slice_agg('slc_idx', 'g = 1; SELECT 1')$$);
SELECT slc_err($$SELECT * FROM lion_index_slice_agg('slc', NULL)$$);
SELECT * FROM lion_index_slice_agg(NULL);
-- a column of a type that cannot be sliced is simply not sliced
CREATE TABLE slc_types (o oid, f float8, n numeric, iv interval, k int4);
INSERT INTO slc_types SELECT i, i, i, i * interval '1 s', i FROM generate_series(1, 100) i;
CREATE INDEX slc_types_idx ON slc_types USING lion (o, f, n, iv, k)
	WITH (slices = on);
SELECT attno, sliced, slice_entries, slice_tids FROM lion_index_stats('slc_types_idx');
SELECT slc_err($$SELECT * FROM lion_index_slice_agg('slc_types_idx', NULL, 2::int2)$$);
SELECT slc_cmp('slc_types_idx', NULL, 5::int2);
DROP TABLE slc_types;

-- ---------- 3. an index built empty, then inserts ----------
CREATE TABLE slc_e (id int, g int, v int8) WITH (autovacuum_enabled = off);
CREATE INDEX slc_e_v ON slc_e USING lion (v, g) WITH (slices = on);
CREATE INDEX slc_e_g ON slc_e USING lion (g);
SELECT * FROM slc_meta('slc_e_v');
SELECT attno, sliced, slice_entries, slice_tids FROM lion_index_stats('slc_e_v');
SELECT slc_cmp('slc_e_v');
-- dense: a run of small keys, every row in the high slices
INSERT INTO slc_e SELECT i, i % 3, i % 8 FROM generate_series(1, 3000) i;
SELECT slc_cmp('slc_e_v'), slc_cmp('slc_e_v', 'g = 1');
-- sparse: a few rows with bits no other row has, and negative ones
INSERT INTO slc_e VALUES (3001, 0, 1099511627776), (3002, 1, -5), (3003, 2, NULL),
	(3004, 1, -9223372036854775807);
SELECT slc_cmp('slc_e_v'), slc_cmp('slc_e_v', 'g = 1'), slc_cmp('slc_e_v', 'g = 2');
-- into the containers the rows above made, one row at a time
INSERT INTO slc_e SELECT 4000 + i, 1, i * 1000 FROM generate_series(1, 40) i;
SELECT slc_cmp('slc_e_v', 'g = 1');
SELECT attno, sliced, slice_entries, slice_tids FROM lion_index_stats('slc_e_v');
SELECT lion_index_verify('slc_e_v', true);
-- an unlogged table's init fork records the sliced columns too (buildempty)
CREATE UNLOGGED TABLE slc_u (v int2);
CREATE INDEX slc_u_v ON slc_u USING lion (v) WITH (slices = on);
INSERT INTO slc_u SELECT (i % 200 - 100)::int2 FROM generate_series(1, 1000) i;
SELECT slc_cmp('slc_u_v');
SELECT lion_index_verify('slc_u_v', true);
DROP TABLE slc_u;

-- ---------- 4. updates and deletes, VACUUM, this transaction's rows ----------
UPDATE slc SET i4 = -i4, i8 = i8 / 3, tz = tz + interval '1 year'
 WHERE id % 11 = 0;
DELETE FROM slc WHERE id % 13 = 0;
INSERT INTO slc (id, g, i2, i4, i8, d, tm, ts, tz)
SELECT 7000 + i, i % 7, i::int2, i * 100, i * 1000, date '1970-01-01' + i,
	   time '06:00' + i * interval '1 min', timestamp '1990-01-01' + i * interval '1 day',
	   timestamptz '1990-01-01 00:00+00' + i * interval '1 h'
  FROM generate_series(1, 500) i;
-- a dirty heap: dead TIDs in the slices, rows the map does not vouch for
SELECT a AS attno, f AS filter, slc_cmp('slc_idx', f, a::int2)
  FROM generate_series(1, 7) a, unnest(ARRAY[NULL, 'g = 3', 'g IN (0, 6)']) f
 ORDER BY a, f NULLS FIRST;
SELECT attno, slice_tids FROM lion_index_stats('slc_idx') WHERE attno IN (2, 3);
SELECT lion_index_verify('slc_idx', true);
VACUUM (FREEZE) slc;
SELECT attno, slice_tids FROM lion_index_stats('slc_idx') WHERE attno IN (2, 3);
SELECT a AS attno, f AS filter, slc_cmp('slc_idx', f, a::int2)
  FROM generate_series(1, 7) a, unnest(ARRAY[NULL, 'g = 3', 'g IN (0, 6)']) f
 ORDER BY a, f NULLS FIRST;
SELECT lion_index_verify('slc_idx', true);
-- this transaction's own rows: inserted, deleted, updated, then rolled back
BEGIN;
INSERT INTO slc (id, g, i4, i8) VALUES (9003, 3, 2147483647, -9223372036854775808),
	(9007, 3, -2147483648, 9223372036854775807);
DELETE FROM slc WHERE g = 3 AND id % 4 = 1;
UPDATE slc SET i4 = 7, i8 = 7 WHERE g = 3 AND id % 4 = 2;
SELECT slc_cmp('slc_idx', 'g = 3', 2::int2), slc_cmp('slc_idx', 'g = 3', 3::int2),
	   slc_cmp('slc_idx', NULL, 2::int2);
ROLLBACK;
SELECT slc_cmp('slc_idx', 'g = 3', 2::int2), slc_cmp('slc_idx', 'g = 3', 3::int2),
	   slc_cmp('slc_idx', NULL, 2::int2);
-- REPEATABLE READ: one snapshot for both; test/isolation/slices_snapshot.spec
-- has another session write beside it
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT slc_cmp('slc_idx', 'g IN (1, 2)', 4::int2), slc_cmp('slc_idx', NULL, 7::int2);
COMMIT;
SELECT lion_index_verify('slc_idx', true);

-- ---------- 5. a partial index, and verify of a broken slice ----------
CREATE INDEX slc_p ON slc USING lion (i8) WITH (slices = on) WHERE g < 3;
SELECT slc_cmp('slc_p'), slc_cmp('slc_p', 'g = 1'), slc_cmp('slc_p', 'g = 5');
SELECT lion_index_verify('slc_p', true);
-- a row put into a slice its value does not name, by a test function: verify
-- finds it from the keys, and again from the heap
CREATE FUNCTION lion_debug_slice_insert(idx regclass, attno int2, pos int2, t tid)
	RETURNS void AS '$libdir/pg_lion', 'lion_debug_slice_insert' LANGUAGE C STRICT;
SELECT i8, ctid AS slc_t FROM slc WHERE id = 6003 \gset
SELECT lion_debug_slice_insert('slc_p', 1::int2, 5::int2, :'slc_t');
SELECT slc_err($$SELECT lion_index_verify('slc_p')$$);
-- the existence slice holding a row the column has under no key (a NULL)
REINDEX INDEX slc_p;
SELECT ctid AS slc_t FROM slc WHERE id = 50 \gset
SELECT lion_debug_slice_insert('slc_p', 1::int2, 64::int2, :'slc_t');
SELECT slc_err($$SELECT lion_index_verify('slc_p')$$);
SELECT slc_err($$SELECT lion_debug_slice_insert('slc_p', 1::int2, 65::int2, '(0,1)')$$);
SELECT slc_err($$SELECT lion_debug_slice_insert('slc_g', 1::int2, 0::int2, '(0,1)')$$);
DROP FUNCTION lion_debug_slice_insert(regclass, int2, int2, tid);
DROP INDEX slc_p;

-- ---------- 6. the readers that walk keys never meet a slice ----------
-- Two tables of the same rows, one index each, with summaries (§32) and
-- with and without slices: every walk of keys gives the same answer from
-- both, and the plan shows that the walk ran.
CREATE TABLE slc_w1 (id int, k int4, g int4, ts timestamp)
	WITH (autovacuum_enabled = off);
INSERT INTO slc_w1 SELECT i, CASE WHEN i % 97 = 0 THEN NULL ELSE (i * 31) % 400 - 200 END,
	   i % 9, timestamp '2020-01-01' + i * interval '1 h'
  FROM generate_series(1, 8000) i;
CREATE TABLE slc_w2 AS SELECT * FROM slc_w1;
ALTER TABLE slc_w2 SET (autovacuum_enabled = off);
CREATE INDEX slc_w1_k ON slc_w1 USING lion (k, g, ts)
	WITH (slices = on, summaries = on, summary_tids = 64);
CREATE INDEX slc_w2_k ON slc_w2 USING lion (k, g, ts)
	WITH (summaries = on, summary_tids = 64);
VACUUM (FREEZE, ANALYZE) slc_w1, slc_w2;
SELECT * FROM slc_meta('slc_w1_k');
SELECT * FROM slc_meta('slc_w2_k');

-- the rows of a query over slc_w1 and over slc_w2, as text
CREATE FUNCTION slc_both(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	EXECUTE format('SELECT string_agg(x::text, '' '') FROM (' || q || ') x', 'slc_w1') INTO a;
	EXECUTE format('SELECT string_agg(x::text, '' '') FROM (' || q || ') x', 'slc_w2') INTO b;
	RETURN CASE WHEN a IS NOT DISTINCT FROM b THEN 'same' ELSE a || ' <> ' || b END;
END $$;

-- the lion paths, whatever the tables' sizes would choose
SET enable_seqscan = off;
-- GROUP BY a sliced column, two columns, its top k, count(DISTINCT)
EXPLAIN (COSTS OFF) SELECT k, count(*) FROM slc_w1 GROUP BY k;
SELECT slc_both($$SELECT k, count(*) FROM %s GROUP BY k ORDER BY k$$);
SELECT slc_both($$SELECT k, g, count(*) FROM %s WHERE g < 4 GROUP BY k, g ORDER BY k, g$$);
SELECT slc_both($$SELECT k, count(*) FROM %s GROUP BY k ORDER BY 2 DESC, 1 LIMIT 3$$);
EXPLAIN (COSTS OFF) SELECT count(DISTINCT k) FROM slc_w1;
SELECT slc_both($$SELECT count(DISTINCT k) FROM %s$$);
SELECT slc_both($$SELECT count(DISTINCT k) FROM %s WHERE g = 2$$);
SELECT slc_both($$SELECT count(DISTINCT ts) FROM %s$$);
-- the aggregates over keys (§37), the slices' answer (§52) set aside
SET pg_lion.enable_count_slices = off;
EXPLAIN (COSTS OFF) SELECT sum(k), min(k), max(k), count(*) FROM slc_w1;
SELECT slc_both($$SELECT sum(k), min(k), max(k), count(*) FROM %s$$);
RESET pg_lion.enable_count_slices;
-- and from the slices, which this small table's costs choose (§52)
EXPLAIN (COSTS OFF) SELECT sum(k), min(k), max(k), count(*) FROM slc_w1;
SELECT slc_both($$SELECT sum(k), min(k), max(k), count(*) FROM %s$$);
-- ranges, summed from the summaries, whose walk ends where the slices begin
EXPLAIN (COSTS OFF) SELECT count(*) FROM slc_w1 WHERE k > -150;
SELECT slc_both($$SELECT count(*) FROM %s WHERE k > -150$$);
SELECT slc_both($$SELECT count(*) FROM %s WHERE k >= -200$$);
SELECT slc_both($$SELECT count(*) FROM %s WHERE k BETWEEN -50 AND 50 AND g = 1$$);
SELECT slc_both($$SELECT count(*) FROM %s WHERE ts >= '2020-03-01'$$);
SELECT slc_both($$SELECT count(*) FROM %s WHERE k <> 3$$);
-- IS NOT NULL and a range by bitmap scans: a walk of the whole column
SET pg_lion.enable_count_pushdown = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM slc_w1 WHERE k IS NOT NULL;
SELECT slc_both($$SELECT count(*) FROM %s WHERE k IS NOT NULL$$);
SELECT slc_both($$SELECT count(*) FROM %s WHERE k > 150$$);
SELECT slc_both($$SELECT count(*), min(id) FROM %s WHERE ts IS NOT NULL$$);
RESET enable_seqscan;
RESET pg_lion.enable_count_pushdown;
-- the facets
SELECT slc_both($$SELECT * FROM lion_index_facets('%s_k', 5)$$);
SELECT slc_both($$SELECT * FROM lion_index_facets('%s_k', NULL, 'g = 4')$$);
SELECT slc_both($$SELECT * FROM lion_index_facets('%s_k', 3, NULL, 2::int2)$$);
-- the index's statistics and the planner's distinct keys (§33)
SELECT slc_both($$SELECT attno, entries, ntids, null_tids, summary_entries, summary_tids, ndistinct
	FROM lion_index_stats('%s_k')$$);
SELECT attno, sliced, slice_entries FROM lion_index_stats('slc_w1_k');
-- a key of no row: its entry and slices emptied by VACUUM
DELETE FROM slc_w1 WHERE k = 0 OR id > 7990;
DELETE FROM slc_w2 WHERE k = 0 OR id > 7990;
VACUUM (FREEZE, ANALYZE) slc_w1, slc_w2;
SELECT slc_both($$SELECT attno, entries, ntids, null_tids, summary_entries, ndistinct
	FROM lion_index_stats('%s_k')$$);
SELECT slc_both($$SELECT k, count(*) FROM %s GROUP BY k ORDER BY k$$);
SELECT slc_cmp('slc_w1_k'), slc_cmp('slc_w1_k', 'g = 8'), slc_cmp('slc_w1_k', NULL, 3::int2);
SELECT lion_index_verify('slc_w1_k', true);

-- ---------- 7. the option, REINDEX, and the format ----------
-- read at build time only: ALTER INDEX changes the next REINDEX
ALTER INDEX slc_w1_k SET (slices = off);
SELECT attno, sliced FROM lion_index_stats('slc_w1_k');
REINDEX INDEX slc_w1_k;
SELECT * FROM slc_meta('slc_w1_k');
SELECT attno, sliced, slice_entries FROM lion_index_stats('slc_w1_k');
SELECT slc_err($$SELECT * FROM lion_index_slice_agg('slc_w1_k')$$);
ALTER INDEX slc_w1_k SET (slices = on);
REINDEX INDEX slc_w1_k;
SELECT * FROM slc_meta('slc_w1_k');
SELECT slc_cmp('slc_w1_k', 'g = 4');
SELECT lion_index_verify('slc_w1_k', true);
SELECT slc_err($$CREATE INDEX slc_bad ON slc_w2 USING lion (k) WITH (slices = maybe)$$);

-- A meta page whose version and sliced columns disagree, and one of a newer
-- format: refused by every reader.  Written on the file under the server,
-- before anything has read the new index's meta page into the buffers.
CREATE FUNCTION slc_poke(idx regclass, pos int8, val int8)
RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('COPY (SELECT 1) TO PROGRAM %L',
				   format('cat >/dev/null; printf ''\%s\%s'' | dd of=%s bs=1 seek=%s conv=notrunc 2>/dev/null',
						  lpad(to_oct(val & 255), 3, '0'),
						  lpad(to_oct((val >> 8) & 255), 3, '0'),
						  pg_relation_filepath(idx), pos));
END $$;
CREATE FUNCTION to_oct(x int8) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT ((x >> 6) & 7)::text || ((x >> 3) & 7)::text || (x & 7)::text
$$;
CREATE TABLE slc_f (k int);
INSERT INTO slc_f SELECT generate_series(1, 100);
CREATE INDEX slc_f_k ON slc_f USING lion (k) WITH (slices = on);
SELECT slc_poke('slc_f_k', 28, 14);
SELECT slc_err($$SELECT lion_index_verify('slc_f_k')$$);
DROP INDEX slc_f_k;
CREATE INDEX slc_f_k ON slc_f USING lion (k);
SELECT slc_poke('slc_f_k', 28, 15);
SELECT slc_err($$SELECT lion_index_verify('slc_f_k')$$);
DROP INDEX slc_f_k;
CREATE INDEX slc_f_k ON slc_f USING lion (k) WITH (slices = on);
SELECT slc_poke('slc_f_k', 28, 16);
SELECT slc_err($$SELECT lion_index_verify('slc_f_k')$$);
DROP TABLE slc_f;

DROP TABLE slc, slc_e, slc_w1, slc_w2;
DROP FUNCTION slc_both(text);
DROP FUNCTION slc_poke(regclass, int8, int8);
DROP FUNCTION to_oct(int8);
DROP FUNCTION slc_meta(regclass);
DROP FUNCTION slc_err(text);
DROP FUNCTION slc_cmp(regclass, text, int2);
DROP FUNCTION slc_ref(regclass, text, int2);
