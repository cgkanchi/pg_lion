-- `col IS NOT NULL` answered exactly by every lion scan (DESIGN.md §14,
-- §29.2): beside quals that answer, the column's NULL entry is taken away
-- from their answer - every row the index holds is under exactly one of a
-- column's NULL entry and its other entries, a multi-key column's EMPTY entry
-- among them - or, where the column is mostly NULL, its other entries are
-- walked beside them; alone, it is a walk of the column (a union of a
-- multi-key one) with any other `IS NOT NULL` column's NULL entry taken away.
-- It used to be dropped beside anything that answered and left to the heap
-- recheck, which fetched every row of the other quals' answer.
--
-- Every query below is run as a bitmap scan, a plain index scan, and through
-- LionOrdered / LionBtreeScan, each answer checked against a sequential scan,
-- and each scan's recheck reported: the rows it threw away, and LionOrdered's
-- set, exact or rechecked.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;
SET default_statistics_target = 1000;

CREATE TABLE snn (
	id int PRIMARY KEY,
	a int,			-- 40 values, a third NULL
	b int,			-- 20 values, never NULL
	c int,			-- three values, nearly every row NULL, scattered
	d int,			-- near-unique, a tenth NULL
	tags text[],	-- NULL, empty, or two tags
	tv tsvector,	-- NULL, empty, or two words
	j jsonb,		-- NULL, empty, or two keys
	g int,			-- 10 values, never NULL: beside the multi-key columns
	pad text
) WITH (autovacuum_enabled = off);
INSERT INTO snn
SELECT i,
	   CASE WHEN i % 3 = 0 THEN NULL ELSE i % 40 END,
	   i % 20,
	   CASE WHEN (hashint4(i) & 2147483647) % 30 = 0 THEN i % 3 END,
	   CASE WHEN i % 10 = 7 THEN NULL ELSE i END,
	   CASE i % 10 WHEN 0 THEN NULL WHEN 1 THEN '{}'::text[]
			ELSE ARRAY['t' || (i % 8), 'u' || (i % 5)] END,
	   CASE WHEN i % 4 = 1 THEN NULL WHEN i % 8 = 2 THEN ''::tsvector
			ELSE to_tsvector('simple', 'w' || (i % 9) || ' x' || (i % 6)) END,
	   CASE WHEN i % 5 = 2 THEN NULL WHEN i % 10 = 3 THEN '{}'::jsonb
			ELSE jsonb_build_object('k', i % 5, 'm', i % 7) END,
	   i % 10,
	   repeat('p', 40)
  FROM generate_series(1, 60000) i;
CREATE INDEX snn_l ON snn USING lion (a, b, c, d);
CREATE INDEX snn_m ON snn USING lion (tags, tv, j, g);
-- covering: LionOrdered and LionBtreeScan walk it index-only (DESIGN.md §40)
CREATE INDEX snn_bt ON snn (id) INCLUDE (a, b, c, d, g, tags, tv, j);
VACUUM (FREEZE, ANALYZE) snn;

-- every one of these is a row with a value, empty or not
SELECT count(*) FILTER (WHERE tags = '{}') AS empty_tags,
	   count(*) FILTER (WHERE tv = ''::tsvector) AS empty_tv,
	   count(*) FILTER (WHERE j = '{}'::jsonb) AS empty_j
  FROM snn;

/*
 * snn_run() runs q one way - 'bitmap', 'plain' (an Index Scan), or 'node'
 * (LionOrdered or LionBtreeScan, every core scan but the btree's index-only
 * one disabled) - and through a sequential scan, compares the answers, in
 * order when `ordered` and as multisets otherwise, and says which scan ran and
 * what its recheck did: the rows the heap recheck of the index quals threw
 * away (only ever there when the scan left a qual to it), and for the nodes
 * whether their set was exact.  The answer is aggregated by the very query
 * EXPLAIN ANALYZE reports on: q is its subquery, planned as it is alone, and
 * one with an ORDER BY is never pulled up, so it is aggregated in its order.
 */
CREATE FUNCTION snn_run(q text, how text, ordered boolean DEFAULT false)
RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	node text := NULL;
	notes text := '';
	wq text := CASE WHEN ordered
		THEN format('SELECT array_agg(r::text) FROM (%s) r', q)
		ELSE format('SELECT array_agg(r::text ORDER BY r::text) FROM (%s) r', q) END;
	a text[];
	b text[];
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_tidscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', (how = 'bitmap')::text, true);
	PERFORM set_config('enable_indexscan', (how = 'plain')::text, true);
	PERFORM set_config('enable_indexonlyscan', (how = 'node')::text, true);
	PERFORM set_config('pg_lion.enable_ordered_scan', (how = 'node')::text, true);
	PERFORM set_config('pg_lion.enable_btree_scan', (how = 'node')::text, true);
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || wq LOOP
		IF node IS NULL AND ln ~ '(Bitmap Index Scan on|Index Scan using|Index Only Scan using|Custom Scan \(Lion\w+\)|Seq Scan on)' THEN
			node := substring(ln from '(Bitmap Index Scan on \S+|Index Scan using \S+|Index Only Scan using \S+|Custom Scan \(Lion\w+\)|Seq Scan on \S+)');
		ELSIF ln ~ 'Rows Removed by (Index|Lion) Recheck' THEN
			notes := notes || ', ' || substring(ln from 'Rows Removed by \w+ Recheck: \d+');
		ELSIF ln ~ 'Lion Set: ' THEN
			notes := notes || ', set ' || substring(ln from '(exact|rechecked)');
		END IF;
	END LOOP;
	EXECUTE wq INTO a;

	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('pg_lion.enable_ordered_scan', 'off', true);
	PERFORM set_config('pg_lion.enable_btree_scan', 'off', true);
	EXECUTE wq INTO b;
	PERFORM set_config('enable_tidscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_ordered_scan', 'on', true);
	PERFORM set_config('pg_lion.enable_btree_scan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	IF a IS DISTINCT FROM b THEN
		RETURN format('%s: MISMATCH: %s rows through %s, %s by the sequential scan',
					  how, coalesce(array_length(a, 1), 0), node,
					  coalesce(array_length(b, 1), 0));
	END IF;
	RETURN format('%s: %s, %s rows%s', how, coalesce(node, 'no scan'),
				  coalesce(array_length(a, 1), 0), notes);
END $$;

-- ... all three ways
CREATE FUNCTION snn_all(q text, ordered boolean DEFAULT false)
RETURNS SETOF text
LANGUAGE sql AS $$
	SELECT snn_run(q, how, ordered)
	  FROM unnest(ARRAY['bitmap', 'plain', 'node']) how
$$;

/*
 * snn_explain() prints what EXPLAIN ANALYZE says of the node's lion side.
 */
CREATE FUNCTION snn_explain(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF ln ~ '(Custom Scan|Ordered By|Index:|Lion Cond|Heap Fetches|Lion Set:|Recheck)' THEN
			RETURN NEXT regexp_replace(ln, '\d+ containers', 'N containers');
		END IF;
	END LOOP;
END $$;

/*
 * snn_plan() prints the plan's nodes with nothing disabled - the plan choice
 * is the test - without the lines whose contents vary between majors.
 */
CREATE FUNCTION snn_plan(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln !~ '^\s*(Index Searches|Disabled|Storage|Planning|Execution|Buffers)' THEN
			RETURN NEXT ln;
		END IF;
	END LOOP;
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
END $$;

-- the queries: each one's columns are in the covering btree, so that the
-- nodes walk it index-only
CREATE TABLE snn_q (n int, q text);
INSERT INTO snn_q VALUES
	-- beside an equality, an IN list, a range: a's NULL entry taken away
	(1, 'SELECT id, a FROM snn WHERE a IS NOT NULL AND b = 5'),
	(2, 'SELECT id, a FROM snn WHERE a IS NOT NULL AND b IN (1, 2, 17)'),
	(3, 'SELECT id, a FROM snn WHERE a IS NOT NULL AND d BETWEEN 1000 AND 30000'),
	(4, 'SELECT id, a FROM snn WHERE a IS NOT NULL AND b = 5 AND d < 20000'),
	(5, 'SELECT id, a FROM snn WHERE a IS NOT NULL AND d IN (3, 6, 9, 12, 7007, 30001)'),
	-- two of them: beside a set, both taken away; alone, one walks
	(6, 'SELECT id FROM snn WHERE a IS NOT NULL AND d IS NOT NULL AND b = 3'),
	(7, 'SELECT id FROM snn WHERE a IS NOT NULL AND d IS NOT NULL'),
	(8, 'SELECT id FROM snn WHERE a IS NOT NULL AND c IS NOT NULL AND d IS NOT NULL'),
	-- alone: a walk of the column
	(9, 'SELECT id FROM snn WHERE a IS NOT NULL'),
	-- a column with no NULL: nothing to take away
	(10, 'SELECT id FROM snn WHERE b IS NOT NULL AND a = 3'),
	-- implied by a strict qual of its column, and contradicted by IS NULL
	(11, 'SELECT id FROM snn WHERE a IS NOT NULL AND a IN (3, 4) AND b = 3'),
	(12, 'SELECT id FROM snn WHERE a IS NOT NULL AND a IS NULL AND b = 3'),
	-- `<> ANY` of two values is `IS NOT NULL` (DESIGN.md §35)
	(13, 'SELECT id FROM snn WHERE a <> ANY (''{1, 2}'') AND b = 6'),
	-- the large NULL entry: c is NULL on 29 rows in 30
	(14, 'SELECT id, c FROM snn WHERE c IS NOT NULL AND b = 5'),
	(15, 'SELECT id, c FROM snn WHERE c IS NOT NULL AND b IN (5, 6) AND a IS NOT NULL'),
	(16, 'SELECT id, c FROM snn WHERE c IS NOT NULL AND d BETWEEN 100 AND 40000'),
	-- multi-key columns: the EMPTY entry's rows have a value
	(17, 'SELECT id FROM snn WHERE tags IS NOT NULL AND g = 1'),
	(18, 'SELECT id FROM snn WHERE tv IS NOT NULL AND g IN (2, 6)'),
	(19, 'SELECT id FROM snn WHERE j IS NOT NULL AND g = 3'),
	(20, 'SELECT id FROM snn WHERE tags && ''{t1}'' AND tv IS NOT NULL AND j IS NOT NULL'),
	(21, 'SELECT id FROM snn WHERE j @> ''{"k": 1}'' AND tags IS NOT NULL'),
	(22, 'SELECT id FROM snn WHERE tv @@ ''w2''::tsquery AND tags IS NOT NULL AND g = 2'),
	(23, 'SELECT id FROM snn WHERE tags IS NOT NULL'),
	(24, 'SELECT id FROM snn WHERE tags IS NOT NULL AND tv IS NOT NULL'),
	(25, 'SELECT id FROM snn WHERE tags IS NOT NULL AND j IS NOT NULL AND tv IS NOT NULL'),
	-- beside a multi-key query that needs every row (mode ALL, §17), which
	-- is left to the recheck
	(26, 'SELECT id FROM snn WHERE tags <@ ''{t1, u1}'' AND tv IS NOT NULL');

SELECT n, snn_all(q) FROM snn_q ORDER BY n;

-- in the btree's order: LionOrdered, its set built or probed lazily
SELECT snn_all('SELECT id, a FROM snn WHERE a IS NOT NULL AND b = 5 ORDER BY id', true);
SELECT snn_all('SELECT id, a FROM snn WHERE a IS NOT NULL AND b = 5 ORDER BY id LIMIT 7', true);
SELECT snn_all('SELECT id FROM snn WHERE tags IS NOT NULL AND g = 1 ORDER BY id LIMIT 7', true);
SELECT snn_all('SELECT id, c FROM snn WHERE c IS NOT NULL AND b = 5 ORDER BY id LIMIT 3', true);
-- heap mode: pad is in no index
SELECT snn_all('SELECT id, pad FROM snn WHERE a IS NOT NULL AND b IN (4, 9) ORDER BY id', true);

-- a list longer than a batch: the plain scan's LIST and the bitmap scan's
-- batches, each batch's union less a's NULL entry
SET work_mem = '64kB';
SELECT array_agg(v)::text AS snn_long FROM generate_series(1, 9000, 3) v \gset
SELECT snn_all(format('SELECT id FROM snn WHERE a IS NOT NULL AND d = ANY (%L)', :'snn_long'));
SELECT snn_all(format('SELECT id FROM snn WHERE a IS NOT NULL AND d = ANY (%L) AND b = 4', :'snn_long'));
RESET work_mem;
-- windows of the other quals' containers past one (§29.3); the bitmap is
-- lossy at this work_mem, and what its recheck throws away is the bitmap's,
-- so not shown
SET pg_lion.scan_window_floor = '64kB';
SET work_mem = '64kB';
SELECT n, split_part(snn_run(q, how), ', Rows Removed', 1)
  FROM snn_q, unnest(ARRAY['bitmap', 'plain', 'node']) WITH ORDINALITY u(how, o)
 WHERE n IN (3, 4, 7, 14, 16, 24) ORDER BY n, o;
RESET work_mem;
RESET pg_lion.scan_window_floor;

-- walk the column, or take its NULL entry away (lion_scan_notnull_walks()):
-- c's NULL entry holds 29 rows in 30, but packed (DESIGN.md §38) over a heap
-- of 22 container keys it is two pages, which a walk of c's three values
-- would not save; a's NULL entry beside its 40 values the same
SET enable_seqscan = off;
SET enable_indexscan = off;
SET pg_lion.enable_count_pushdown = off;
SET client_min_messages = debug1;
SELECT count(*) FROM snn WHERE c IS NOT NULL AND b = 5;
SELECT count(*) FROM snn WHERE a IS NOT NULL AND b IN (5, 6);
-- not beside a range: the range has the walk
SELECT count(*) FROM snn WHERE c IS NOT NULL AND b = 5 AND d > 100;
RESET client_min_messages;
RESET enable_seqscan;
RESET enable_indexscan;
RESET pg_lion.enable_count_pushdown;

-- the plans, nothing disabled: the IS NOT NULL is in the index condition
-- and nothing is rechecked
SELECT snn_plan('SELECT sum(id) FROM snn WHERE a IS NOT NULL AND b = 5');
SELECT snn_plan('SELECT id, a FROM snn WHERE a IS NOT NULL AND b IN (4, 9) ORDER BY id');
SELECT snn_plan('SELECT count(*) FROM snn WHERE c IS NOT NULL AND b = 5 AND g = 5');

-- the large NULL entry: a heap of 72 container keys at fillfactor 10, w NULL
-- on 399 rows in 400, its two values INLINE in the directory - their leaf is
-- cheaper walked than the NULL entry's three pages of runs are read
CREATE TABLE snw (id int, b int, w int)
	WITH (autovacuum_enabled = off, fillfactor = 10);
INSERT INTO snw
SELECT i, i % 20, CASE WHEN (hashint4(i) & 2147483647) % 400 = 0 THEN i % 2 END
  FROM generate_series(1, 100000) i;
CREATE INDEX snw_l ON snw USING lion (b, w);
CREATE INDEX snw_bt ON snw (id) INCLUDE (b, w);
VACUUM (FREEZE, ANALYZE) snw;
SELECT entries, inline_entries, null_tids
  FROM lion_index_stats('snw_l') WHERE attno = 2;
SELECT pg_relation_size('snw') / current_setting('block_size')::int / 64 > 64
	AS past_64_keys;
SET enable_seqscan = off;
SET enable_indexscan = off;
SET pg_lion.enable_count_pushdown = off;
SET client_min_messages = debug1;
SELECT count(*) FROM snw WHERE w IS NOT NULL AND b = 5;
SELECT count(*) FROM snw WHERE w IS NOT NULL AND b IN (3, 5, 9);
RESET client_min_messages;
RESET enable_seqscan;
RESET enable_indexscan;
RESET pg_lion.enable_count_pushdown;
SELECT snn_all('SELECT id, w FROM snw WHERE w IS NOT NULL AND b = 5');
SELECT snn_all('SELECT id, w FROM snw WHERE w IS NOT NULL AND b IN (3, 5, 9)');
SELECT snn_all('SELECT id FROM snw WHERE w IS NOT NULL AND b IN (3, 5, 9) AND id > 50000');
-- moved into and out of the NULL entry, and some deleted
UPDATE snw SET w = 1 WHERE id % 997 = 0;
UPDATE snw SET w = NULL WHERE w IS NOT NULL AND id % 3 = 0;
DELETE FROM snw WHERE id % 1009 = 0;
SELECT snn_all('SELECT id, w FROM snw WHERE w IS NOT NULL AND b = 5');
SELECT snn_all('SELECT id, w FROM snw WHERE w IS NOT NULL AND b IN (3, 5, 9)');
DROP TABLE snw;

-- a dirty heap: rows moved into and out of a's NULL entry, others deleted;
-- the old versions are still in the entries they were in
UPDATE snn SET a = NULL WHERE id % 29 = 0;
UPDATE snn SET a = 1 WHERE a IS NULL AND id % 31 = 0;
UPDATE snn SET c = 1 WHERE c IS NULL AND id % 37 = 0;
UPDATE snn SET tags = NULL WHERE id % 41 = 0;
UPDATE snn SET tags = '{}' WHERE tags IS NULL AND id % 43 = 0;
DELETE FROM snn WHERE id % 47 = 0;
SELECT n, snn_all(q) FROM snn_q ORDER BY n;
SELECT snn_all('SELECT id, a FROM snn WHERE a IS NOT NULL AND b = 5 ORDER BY id', true);
SELECT snn_all('SELECT id, a FROM snn WHERE a IS NOT NULL AND b = 5 ORDER BY id LIMIT 7', true);
SELECT snn_all('SELECT id FROM snn WHERE tags IS NOT NULL AND g = 1 ORDER BY id LIMIT 7', true);

-- ... and vacuumed
VACUUM (FREEZE, ANALYZE) snn;
SELECT n, snn_all(q) FROM snn_q ORDER BY n;
SELECT snn_all('SELECT id, a FROM snn WHERE a IS NOT NULL AND b = 5 ORDER BY id', true);

-- the nodes' sets are exact, so no lion qual is left for the heap to check
-- (DESIGN.md §40.3): a heap walk fetches the rows it returns and no other,
-- and the index-only walk fetches none
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SET pg_lion.enable_count_pushdown = off;
SELECT snn_explain('SELECT id, a FROM snn WHERE a IS NOT NULL AND b = 5 AND c IS NOT NULL ORDER BY id');
SELECT snn_explain('SELECT id, a FROM snn WHERE a IS NOT NULL AND b IN (2, 3)');
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
RESET pg_lion.enable_count_pushdown;

DROP TABLE snn, snn_q;
DROP FUNCTION snn_run(text, text, boolean), snn_all(text, boolean),
	snn_explain(text), snn_plan(text);
