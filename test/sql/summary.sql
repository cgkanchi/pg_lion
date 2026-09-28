-- Summary posting sets (DESIGN.md §32).
--
-- A summarized key column keeps, after its values, one SUMMARY entry per
-- bucket of consecutive keys holding the union of their posting sets; a range
-- over the column is then the summaries of the buckets it covers whole and the
-- keys of the two buckets at its edges.  Every answer here is checked against
-- a sequential scan with the pushdown off, and lion_index_verify() - which
-- compares every summary with the keys of its bucket - runs after every kind
-- of change: build, appends that open new buckets, inserts in the middle and
-- below every key, updates, deletes and VACUUM.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
CREATE EXTENSION IF NOT EXISTS pageinspect;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

/*
 * lion_sm() runs q with the pushdown on and every other scan disabled, and
 * again as a sequential scan with the pushdown off, and compares the answers.
 * It reports how the node evaluated a range and how many summaries it summed.
 */
CREATE FUNCTION lion_sm(q text) RETURNS text
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
		how := coalesce(node ->> 'Range Evaluation', 'no range') ||
			', summaries ' || coalesce(node ->> 'Summaries Summed', '0');
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

/* The same answer through a bitmap scan of the index, against a seqscan. */
CREATE FUNCTION lion_sm_bitmap(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	got text;
	want text;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	EXECUTE format('SELECT coalesce(string_agg(s::text, '' '' ORDER BY s::text), ''(none)'') FROM (%s) s', q)
		INTO got;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	EXECUTE format('SELECT coalesce(string_agg(s::text, '' '' ORDER BY s::text), ''(none)'') FROM (%s) s', q)
		INTO want;
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	IF got IS DISTINCT FROM want THEN
		RETURN format('MISMATCH: bitmap %s, seqscan %s', got, want);
	END IF;
	RETURN got;
END $$;

-- ---------- the table ----------
-- u: unique and in heap order; r: 9000 values in no order; g: 7 values;
-- k: 800 values, NULL in every 11th row; t: 3000 text values of every length
CREATE TABLE lion_sm (id int NOT NULL, u int NOT NULL, r int NOT NULL,
					  g int NOT NULL, k int, t text, pad text);
INSERT INTO lion_sm
SELECT i, i, (i * 7919) % 9001, i % 7,
	   CASE WHEN i % 11 = 0 THEN NULL ELSE (i * 13) % 800 END,
	   'k' || repeat('x', i % 23) || ((i * 31) % 3000)::text,
	   repeat('p', 30)
  FROM generate_series(1, 20000) i;
-- small buckets, so that twenty thousand rows make hundreds of them
CREATE INDEX lion_sm_urg ON lion_sm USING lion (u, r, g)
	WITH (summaries = on, summary_tids = 32);
CREATE INDEX lion_sm_k ON lion_sm USING lion (k)
	WITH (summaries = on, summary_tids = 64);
CREATE INDEX lion_sm_t ON lion_sm USING lion (t)
	WITH (summaries = on, summary_tids = 48);
VACUUM (FREEZE, ANALYZE) lion_sm;

-- every ordered scalar column got summaries, and they hold every row with a
-- value; the NULL rows are in none
SELECT attno, entries, ntids, null_tids, summary_entries, summary_tids,
	   summary_bytes > 0 AS has_bytes
  FROM lion_index_stats('lion_sm_urg');
SELECT attno, entries, ntids, null_tids, summary_entries, summary_tids
  FROM lion_index_stats('lion_sm_k');
SELECT summary_tids = ntids AS covers FROM lion_index_stats('lion_sm_t');
SELECT lion_index_verify('lion_sm_urg', true);
SELECT lion_index_verify('lion_sm_k', true);
SELECT lion_index_verify('lion_sm_t', true);

-- ---------- 1. ranges alone: the sum over the range, from its summaries ----------
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u >= 100');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u > 100');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u < 19000');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u <= 19000');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u BETWEEN 5000 AND 15000');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u > 5000 AND u < 15000');
SELECT lion_sm('SELECT count(u) FROM lion_sm WHERE u >= 1 AND u <= 20000');
-- within one bucket, and empty or inverted
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u BETWEEN 40 AND 45');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u > 45 AND u < 46');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u BETWEEN 900 AND 300');
-- outside the keys, and cross-type bounds outside the int4 domain
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u > 20000');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u < 5000000000::int8');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u >= -5000000000::int8 AND u < 7000');
-- the column in no order in the heap
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE r >= 1000');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE r BETWEEN 2000 AND 6000');
-- a nullable column: its NULL rows are in no bucket and never counted
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE k >= 0');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE k < 400');
SELECT lion_sm('SELECT count(k) FROM lion_sm WHERE k BETWEEN 10 AND 700');
-- text keys of every length
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE t >= ''kx''');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE t BETWEEN ''k1'' AND ''kxxxxx''');

-- ---------- 2. beside other clauses: inside, complement, full domain ----------
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u >= 100 AND g = 3');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u < 300 AND g = 3');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u BETWEEN 3000 AND 11000 AND g IN (1, 2)');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u >= 0 AND g = 5');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE r >= 100 AND g = 3');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE r < 8900 AND (g = 1 OR k = 7)');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE k >= 3 AND g = 2');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE k >= 3 AND k < 790 AND u > 1000');

-- ---------- 3. two bounds on one side (DESIGN.md §32: the tightest one) ----------
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u < 19900 AND u <= 19950 AND g = 3');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u <= 19950 AND u < 19900 AND g = 3');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u > 50 AND u >= 20 AND g = 3');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u >= 20 AND u > 50 AND u < 19000 AND u <= 19990');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u < 19900 AND u < 19950::int8 AND g = 4');

-- ---------- 4. walks that must not use summaries: per-key groups ----------
SELECT lion_sm('SELECT u, count(*) FROM lion_sm WHERE u BETWEEN 100 AND 140 GROUP BY u');
SELECT lion_sm('SELECT count(DISTINCT r) FROM lion_sm WHERE r BETWEEN 100 AND 400');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE k IS NOT NULL');
SELECT lion_sm('SELECT g, count(*) FROM lion_sm GROUP BY g');
-- and the bitmap and plain scans of the same index, which read no summary
SELECT lion_sm_bitmap('SELECT count(*) FROM lion_sm WHERE u BETWEEN 5000 AND 5100');
SELECT lion_sm_bitmap('SELECT count(*) FROM lion_sm WHERE k IS NOT NULL');
SELECT lion_sm_bitmap('SELECT count(*) FROM lion_sm WHERE t >= ''kxxxxxxxxxxxxxxxxxxxxx''');
SELECT lion_sm_bitmap('SELECT count(*) FROM lion_sm WHERE g = 3 AND r > 8000');

-- ---------- 5. inserts: new buckets above every key, keys between and below ----------
-- appends above every key open a new bucket each time the last one is full
INSERT INTO lion_sm SELECT i, i, (i * 7919) % 9001, i % 7, NULL, 'kz' || i::text, 'q'
  FROM generate_series(20001, 21500) i;
-- keys between existing ones join the bucket that covers them
INSERT INTO lion_sm SELECT 30000 + i, 2 * i, 9001 + i, i % 7, i % 800, 'kb' || i::text, 'q'
  FROM generate_series(1, 700) i;
-- and keys below every key join the first bucket
INSERT INTO lion_sm SELECT 40000 + i, -i, -i, i % 7, -i, '' || i::text, 'q'
  FROM generate_series(1, 300) i;
SELECT attno, summary_entries > 600 AS many, summary_tids = ntids AS covers
  FROM lion_index_stats('lion_sm_urg');
SELECT lion_index_verify('lion_sm_urg', true);
SELECT lion_index_verify('lion_sm_k', true);
SELECT lion_index_verify('lion_sm_t', true);
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u >= 100');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u > 20500');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u BETWEEN 1 AND 20700 AND g = 2');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u < 0');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE r < 100 AND g = 1');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE k < 5');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE t < ''kc''');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE t > ''kz1''');

-- ---------- 5b. an open bucket that is a posting tree, its key raised ----------
-- inline_limit = 64 spills a summary to a chain of its own after a few rows,
-- and the appends after a VACUUM land in the free space it left all over the
-- heap: the open bucket is a chain whose key every append raises.  Its pages
-- are stamped with its entry's hash (§18), which is why a summary's hash is a
-- constant and not its key's: the key changes, the stamp may not.
CREATE TABLE lion_sm_chain (k int NOT NULL, pad text);
INSERT INTO lion_sm_chain SELECT i, repeat('p', 200) FROM generate_series(1, 4000) i;
CREATE INDEX lion_sm_chain_k ON lion_sm_chain USING lion (k)
	WITH (summaries = on, summary_tids = 64, inline_limit = 64);
DELETE FROM lion_sm_chain WHERE k % 3 <> 0;
VACUUM (FREEZE) lion_sm_chain;
INSERT INTO lion_sm_chain SELECT 4000 + i, repeat('p', 200) FROM generate_series(1, 3000) i;
SELECT summary_entries > 60 AS many, summary_tids = ntids AS covers,
	   summary_pages > 0 AS chained
  FROM lion_index_stats('lion_sm_chain_k');
SELECT lion_index_verify('lion_sm_chain_k', true);
VACUUM (FREEZE, ANALYZE) lion_sm_chain;
SELECT lion_sm('SELECT count(*) FROM lion_sm_chain WHERE k > 3000');
SELECT lion_sm('SELECT count(*) FROM lion_sm_chain WHERE k BETWEEN 2000 AND 6500');
DROP TABLE lion_sm_chain;

-- ---------- 6. a dirty heap, and VACUUM ----------
UPDATE lion_sm SET pad = 'dirty' WHERE id % 5 = 0;
UPDATE lion_sm SET u = u + 100000 WHERE id % 97 = 0;
DELETE FROM lion_sm WHERE id % 13 = 0;
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u >= 100');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u BETWEEN 5000 AND 15000 AND g = 3');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u > 100000');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE r > 50 AND r < 8000');
SELECT lion_index_verify('lion_sm_urg', true);
VACUUM (FREEZE) lion_sm;
SELECT lion_index_verify('lion_sm_urg', true);
SELECT lion_index_verify('lion_sm_k', true);
SELECT lion_index_verify('lion_sm_t', true);
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u >= 100');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u BETWEEN 5000 AND 15000 AND g = 3');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u > 100000');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE r > 50 AND r < 8000');
-- a run of whole buckets deleted: their summaries are emptied and removed
DELETE FROM lion_sm WHERE u BETWEEN 2000 AND 9000;
VACUUM (FREEZE) lion_sm;
SELECT lion_index_verify('lion_sm_urg', true);
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u >= 100');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u BETWEEN 1000 AND 10000');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE u BETWEEN 1990 AND 9010 AND g = 3');
-- ... and every row of a column: every summary goes, and the next insert
-- opens a bucket again
DELETE FROM lion_sm WHERE k IS NOT NULL;
VACUUM (FREEZE) lion_sm;
SELECT summary_entries, summary_tids FROM lion_index_stats('lion_sm_k');
INSERT INTO lion_sm VALUES (50001, 50001, 1, 1, 42, 'x', 'q'), (50002, 50002, 2, 2, 41, 'y', 'q');
SELECT summary_entries, summary_tids FROM lion_index_stats('lion_sm_k');
SELECT lion_index_verify('lion_sm_k', true);
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE k >= 0');
SELECT lion_sm('SELECT count(*) FROM lion_sm WHERE k > 41');

-- ---------- 7. old format: an index without summaries, and REINDEX ----------
-- The format version, read off the meta page (block 0: LionMetaPageData
-- follows the 24-byte page header, with its version at +4), and how many key
-- columns have summaries.
CREATE FUNCTION lion_sm_meta(idx regclass, OUT version int, OUT summary_cols int)
LANGUAGE sql AS $$
	SELECT get_byte(p, 28) | (get_byte(p, 29) << 8),
		   (SELECT count(*)::int FROM lion_index_stats(idx) s WHERE s.summary_entries > 0)
	  FROM get_raw_page(idx::text, 0) p
$$;
CREATE TABLE lion_sm_old (u int NOT NULL, g int NOT NULL);
INSERT INTO lion_sm_old SELECT i, i % 5 FROM generate_series(1, 5000) i;
-- summaries = off is the default, and writes format 6, as every index before
CREATE INDEX lion_sm_old_u ON lion_sm_old USING lion (u, g);
VACUUM (FREEZE, ANALYZE) lion_sm_old;
SELECT * FROM lion_sm_meta('lion_sm_old_u');
SELECT attno, summary_entries FROM lion_index_stats('lion_sm_old_u');
SELECT lion_sm('SELECT count(*) FROM lion_sm_old WHERE u >= 100 AND g = 2');
SELECT lion_sm('SELECT count(*) FROM lion_sm_old WHERE u BETWEEN 100 AND 4000');
-- setting the option changes nothing until the index is rebuilt: inserts
-- never make summaries for a column the build did not give them
ALTER INDEX lion_sm_old_u SET (summaries = on, summary_tids = 64);
INSERT INTO lion_sm_old SELECT i, i % 5 FROM generate_series(5001, 5100) i;
SELECT attno, summary_entries FROM lion_index_stats('lion_sm_old_u');
SELECT lion_sm('SELECT count(*) FROM lion_sm_old WHERE u >= 100 AND g = 2');
REINDEX INDEX lion_sm_old_u;
SELECT * FROM lion_sm_meta('lion_sm_old_u');
SELECT attno, summary_entries > 0 AS has FROM lion_index_stats('lion_sm_old_u');
SELECT lion_index_verify('lion_sm_old_u', true);
SELECT lion_sm('SELECT count(*) FROM lion_sm_old WHERE u >= 100 AND g = 2');
SELECT lion_sm('SELECT count(*) FROM lion_sm_old WHERE u BETWEEN 100 AND 4000');
-- ... and off again
ALTER INDEX lion_sm_old_u SET (summaries = off);
REINDEX INDEX lion_sm_old_u;
SELECT * FROM lion_sm_meta('lion_sm_old_u');
SELECT attno, summary_entries FROM lion_index_stats('lion_sm_old_u');
DROP FUNCTION lion_sm_meta(regclass);

-- ---------- 8. auto: the build decides per column from the data ----------
CREATE TABLE lion_sm_auto (u int NOT NULL, g int NOT NULL, b bool, e int[]);
INSERT INTO lion_sm_auto SELECT i, i % 4, i % 2 = 0, ARRAY[i % 10] FROM generate_series(1, 8000) i;
-- u has many keys of one row each; g and b have a few keys of many rows; an
-- array column extracts keys and never has summaries
CREATE INDEX lion_sm_auto_i ON lion_sm_auto USING lion (u, g, b)
	WITH (summaries = auto, summary_tids = 64);
SELECT attno, summary_entries > 0 AS has FROM lion_index_stats('lion_sm_auto_i');
CREATE INDEX lion_sm_auto_e ON lion_sm_auto USING lion (e)
	WITH (summaries = on);
SELECT attno, summary_entries FROM lion_index_stats('lion_sm_auto_e');
-- an empty table gives auto nothing to decide from, and on its every column.
-- u arrives in ascending order and fills bucket after bucket; v arrives in no
-- text order ('v10' sorts below 'v9'), and once 'v99' is in, every later key
-- sorts below the open bucket's key and joins it: one bucket, which REINDEX
-- would split
CREATE TABLE lion_sm_empty (u int, v text);
CREATE INDEX lion_sm_empty_auto ON lion_sm_empty USING lion (u) WITH (summaries = auto);
CREATE INDEX lion_sm_empty_on ON lion_sm_empty USING lion (u, v) WITH (summaries = on, summary_tids = 16);
INSERT INTO lion_sm_empty SELECT i, 'v' || i FROM generate_series(1, 300) i;
SELECT attno, summary_entries FROM lion_index_stats('lion_sm_empty_auto');
SELECT attno, summary_entries > 5 AS many, summary_tids FROM lion_index_stats('lion_sm_empty_on');
SELECT lion_index_verify('lion_sm_empty_on', true);
SELECT lion_sm('SELECT count(*) FROM lion_sm_empty WHERE u >= 20');
SELECT lion_sm('SELECT count(*) FROM lion_sm_empty WHERE v < ''v5''');

-- ---------- 9. a partitioned table, summaries in some partitions only ----------
CREATE TABLE lion_sm_p (u int NOT NULL, g int NOT NULL) PARTITION BY RANGE (u);
CREATE TABLE lion_sm_p1 PARTITION OF lion_sm_p FOR VALUES FROM (0) TO (5000);
CREATE TABLE lion_sm_p2 PARTITION OF lion_sm_p FOR VALUES FROM (5000) TO (10000);
CREATE TABLE lion_sm_p3 PARTITION OF lion_sm_p FOR VALUES FROM (10000) TO (20000);
INSERT INTO lion_sm_p SELECT i, i % 3 FROM generate_series(0, 19999) i;
CREATE INDEX lion_sm_p1_i ON lion_sm_p1 USING lion (u, g) WITH (summaries = on, summary_tids = 32);
CREATE INDEX lion_sm_p2_i ON lion_sm_p2 USING lion (u, g);
CREATE INDEX lion_sm_p3_i ON lion_sm_p3 USING lion (u, g) WITH (summaries = on, summary_tids = 100);
VACUUM (FREEZE, ANALYZE) lion_sm_p;
SELECT lion_sm('SELECT count(*) FROM lion_sm_p WHERE u >= 100');
SELECT lion_sm('SELECT count(*) FROM lion_sm_p WHERE u BETWEEN 3000 AND 17000 AND g = 1');
SELECT lion_sm('SELECT count(*) FROM lion_sm_p WHERE u < 12000 AND g = 2');

-- ---------- 10. a unique timestamp, the shape summaries are for ----------
CREATE TABLE lion_sm_ts (ts timestamptz NOT NULL, c int NOT NULL);
INSERT INTO lion_sm_ts
SELECT '2025-01-01 00:00:00+00'::timestamptz + i * interval '1 minute', i % 50
  FROM generate_series(1, 30000) i;
CREATE INDEX lion_sm_ts_i ON lion_sm_ts USING lion (ts, c) WITH (summaries = auto);
VACUUM (FREEZE, ANALYZE) lion_sm_ts;
SELECT attno, summary_entries > 0 AS has FROM lion_index_stats('lion_sm_ts_i');
SELECT lion_sm('SELECT count(*) FROM lion_sm_ts WHERE ts >= ''2025-01-02 00:00:00+00''');
SELECT lion_sm('SELECT count(*) FROM lion_sm_ts WHERE ts >= ''2025-01-02 00:00:00+00'' AND c = 7');
SELECT lion_sm('SELECT count(*) FROM lion_sm_ts WHERE ts BETWEEN ''2025-01-05'' AND ''2025-01-12'' AND c = 7');
SELECT lion_sm('SELECT count(*) FROM lion_sm_ts WHERE ts < ''2025-01-03 00:00:00+00'' AND c = 9');
SELECT lion_index_verify('lion_sm_ts_i', true);

DROP TABLE lion_sm, lion_sm_old, lion_sm_auto, lion_sm_empty, lion_sm_p, lion_sm_ts;
DROP FUNCTION lion_sm(text);
DROP FUNCTION lion_sm_bitmap(text);
