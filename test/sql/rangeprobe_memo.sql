-- An end the endpoint probe found no live key at is not read again at every
-- plan (DESIGN.md §28, "An end found empty is not read again at every plan").
--
-- Core's probe marks the btree entries whose rows it finds dead, and the next
-- plan skips them; a posting set has nothing to mark, so an end whose newest
-- rows were deleted in bulk was walked to the probe's bounds - a hundred
-- directory leaves here - by every plan until the next VACUUM, and after it
-- too while the emptied leaves were more than the bound.  The probe now
-- remembers that it found nothing until the table or the index has changed.
--
-- The table's shape is rangeprobe.sql's: 1500 rows analyzed, then 28,500
-- newer ones, so that the index grew by inserts, about 34 keys to a leaf.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;
SET pg_lion.enable_count_pushdown = off;

/* The shared buffers planning q read, hits and reads together. */
CREATE FUNCTION lpm_bufs(q text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	j	json;
BEGIN
	EXECUTE 'EXPLAIN (BUFFERS, FORMAT JSON) ' || q INTO j;
	RETURN (j->0->'Planning'->>'Shared Hit Blocks')::bigint +
		   (j->0->'Planning'->>'Shared Read Blocks')::bigint;
END $$;

/* The row estimate of the bitmap index scan on lpm_lion: lion's own. */
CREATE FUNCTION lpm_rows(q text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	j	json;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	RETURN (regexp_match(j::text, '"Node Type": "Bitmap Index Scan",[^}]*"Index Name": "lpm_lion",[^}]*"Plan Rows": (\d+)'))[1];
END $$;

CREATE TABLE lpm (id int, ts timestamptz) WITH (autovacuum_enabled = off);
ALTER TABLE lpm ALTER ts SET STATISTICS 5;
INSERT INTO lpm SELECT g, '2026-01-01'::timestamptz + g * interval '1 minute'
  FROM generate_series(1, 1500) g;
CREATE INDEX lpm_lion ON lpm USING lion (ts);
VACUUM (ANALYZE) lpm;
INSERT INTO lpm SELECT g, '2026-01-01'::timestamptz + g * interval '1 minute'
  FROM generate_series(1501, 30000) g;
VACUUM (FREEZE) lpm;
CREATE TABLE lpm_est (step text, est bigint);
INSERT INTO lpm_est
	SELECT 'probed', lpm_rows($$SELECT * FROM lpm WHERE ts >= '2026-01-10'$$);

-- The newest 10,000 keys go, and VACUUM empties their leaves, some 300: more
-- than the probe reads.  The first plan reads a hundred of them and gives up;
-- the next one reads none.
DELETE FROM lpm WHERE id > 20000;
VACUUM (FREEZE) lpm;
SELECT 'DIAG shape' AS diag, leaf_pages, entries, ntids FROM lion_index_stats('lpm_lion');
CREATE TABLE lpm_io (step text, bufs bigint);
INSERT INTO lpm_io
	SELECT 'first', lpm_bufs($$SELECT * FROM lpm WHERE ts >= '2026-01-10'$$);
INSERT INTO lpm_io
	SELECT 'again', lpm_bufs($$SELECT * FROM lpm WHERE ts >= '2026-01-10'$$);
SELECT (SELECT bufs FROM lpm_io WHERE step = 'first') -
	   (SELECT bufs FROM lpm_io WHERE step = 'again') >= 100 AS walked_once;
INSERT INTO lpm_est
	SELECT 'given up', lpm_rows($$SELECT * FROM lpm WHERE ts >= '2026-01-10'$$);

-- Rows past the end, more than the deleted ones left room for, grow the
-- table and the index: the probe reads the end again and finds them.
INSERT INTO lpm SELECT g, '2026-01-01'::timestamptz + g * interval '1 minute'
  FROM generate_series(30001, 50000) g;
INSERT INTO lpm_est
	SELECT 'grown', lpm_rows($$SELECT * FROM lpm WHERE ts >= '2026-01-10'$$);
SELECT (SELECT est FROM lpm_est WHERE step = 'probed') >
	   10 * (SELECT est FROM lpm_est WHERE step = 'given up') AS gave_up,
	   (SELECT est FROM lpm_est WHERE step = 'grown') >
	   10 * (SELECT est FROM lpm_est WHERE step = 'given up') AS probed_again;
SELECT count(*) FROM lpm WHERE ts >= '2026-01-10';

SELECT 'DIAG io' AS diag, * FROM lpm_io ORDER BY step;
SELECT 'DIAG est' AS diag, * FROM lpm_est ORDER BY step;
-- DIAG twin with plain VACUUM
CREATE FUNCTION lpq_rows(q text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	j	json;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	RETURN (regexp_match(j::text, '"Node Type": "Bitmap Index Scan",[^}]*"Index Name": "lpq_lion",[^}]*"Plan Rows": (\d+)'))[1];
END $$;
CREATE TABLE lpq (id int, ts timestamptz) WITH (autovacuum_enabled = off);
ALTER TABLE lpq ALTER ts SET STATISTICS 5;
INSERT INTO lpq SELECT g, '2026-01-01'::timestamptz + g * interval '1 minute'
  FROM generate_series(1, 1500) g;
CREATE INDEX lpq_lion ON lpq USING lion (ts);
VACUUM (ANALYZE) lpq;
INSERT INTO lpq SELECT g, '2026-01-01'::timestamptz + g * interval '1 minute'
  FROM generate_series(1501, 30000) g;
VACUUM lpq;
SELECT 'DIAG q probed' AS diag, lpq_rows($$SELECT * FROM lpq WHERE ts >= '2026-01-10'$$) AS est;
DELETE FROM lpq WHERE id > 20000;
VACUUM lpq;
SELECT 'DIAG q shape' AS diag, leaf_pages, entries, ntids FROM lion_index_stats('lpq_lion');
SELECT 'DIAG q first' AS diag, lpm_bufs($$SELECT * FROM lpq WHERE ts >= '2026-01-10'$$);
SELECT 'DIAG q again' AS diag, lpm_bufs($$SELECT * FROM lpq WHERE ts >= '2026-01-10'$$);
SELECT 'DIAG q given up' AS diag, lpq_rows($$SELECT * FROM lpq WHERE ts >= '2026-01-10'$$) AS est;
DROP TABLE lpq;
DROP FUNCTION lpq_rows(text);
RESET pg_lion.enable_count_pushdown;
DROP TABLE lpm, lpm_est, lpm_io;
DROP FUNCTION lpm_bufs(text);
DROP FUNCTION lpm_rows(text);
