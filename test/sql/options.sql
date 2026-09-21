-- Reloptions and the limitations the handler advertises.
\set VERBOSITY terse
SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS pg_lion;

CREATE TABLE lion_opt AS
SELECT i, (i % 97)::int4 AS k, 'v' || (i % 97) AS t
  FROM generate_series(1, 20000) i;

-- buckets: 0 (auto) up to 65536
CREATE INDEX lion_opt_b0 ON lion_opt USING lion (k) WITH (buckets = 0);
CREATE INDEX lion_opt_b1 ON lion_opt USING lion (k) WITH (buckets = 1);
CREATE INDEX lion_opt_b4096 ON lion_opt USING lion (k) WITH (buckets = 4096);
CREATE INDEX lion_opt_bad1 ON lion_opt USING lion (k) WITH (buckets = -1);
CREATE INDEX lion_opt_bad2 ON lion_opt USING lion (k) WITH (buckets = 65537);

-- inline_limit: 64 .. 4096
CREATE INDEX lion_opt_i64 ON lion_opt USING lion (k) WITH (inline_limit = 64);
CREATE INDEX lion_opt_i4096 ON lion_opt USING lion (k) WITH (inline_limit = 4096);
CREATE INDEX lion_opt_bad3 ON lion_opt USING lion (k) WITH (inline_limit = 63);
CREATE INDEX lion_opt_bad4 ON lion_opt USING lion (k) WITH (inline_limit = 4097);
CREATE INDEX lion_opt_bad5 ON lion_opt USING lion (k) WITH (nosuchoption = 1);

-- reloptions are recorded on the index
SELECT c.relname, c.reloptions
  FROM pg_class c
 WHERE c.relname IN ('lion_opt_b0', 'lion_opt_b1', 'lion_opt_b4096', 'lion_opt_i64')
 ORDER BY 1;

-- the upper boundary is accepted by the parser (building it would allocate
-- 65537 pages, so only validate the option here)
ALTER INDEX lion_opt_b0 SET (buckets = 65536);
ALTER INDEX lion_opt_b0 SET (buckets = 65537);
ALTER INDEX lion_opt_b0 RESET (buckets);

-- All of these must find the same rows.
SET enable_seqscan = off;
SELECT count(*) FROM lion_opt WHERE k = 42;
DROP INDEX lion_opt_b0;
SELECT count(*) FROM lion_opt WHERE k = 42;
DROP INDEX lion_opt_b1;
SELECT count(*) FROM lion_opt WHERE k = 42;
DROP INDEX lion_opt_b4096;
SELECT count(*) FROM lion_opt WHERE k = 42;
DROP INDEX lion_opt_i64;
SELECT count(*) FROM lion_opt WHERE k = 42;
DROP INDEX lion_opt_i4096;
RESET enable_seqscan;
SELECT count(*) FROM lion_opt WHERE k = 42;

/*
 * buckets is an exact count now, not rounded up to a power of two: a hash is
 * mapped to its bucket with a modulo (DESIGN.md section 4).
 */
CREATE INDEX lion_opt_b7 ON lion_opt USING lion (k) WITH (buckets = 7);
CREATE INDEX lion_opt_b1000 ON lion_opt USING lion (k) WITH (buckets = 1000);
SELECT nbuckets, bucket_pages, entries FROM lion_index_stats('lion_opt_b7');
SELECT nbuckets, bucket_pages, entries FROM lion_index_stats('lion_opt_b1000');
SELECT lion_index_verify('lion_opt_b7', true);
SELECT lion_index_verify('lion_opt_b1000', true);
SET enable_seqscan = off;
SELECT count(*) FROM lion_opt WHERE k = 42;
RESET enable_seqscan;
DROP INDEX lion_opt_b7, lion_opt_b1000;

/*
 * inline_limit defaults to the maximum (4096), so a posting set of one 1408
 * byte array container stays in its entry tuple; the old default of 1024
 * would have given it a container page of its own.
 */
CREATE TABLE lion_opt_inl AS
SELECT i, (i % 20)::int4 AS k FROM generate_series(1, 14000) i;
CREATE INDEX lion_opt_inl_d ON lion_opt_inl USING lion (k);
CREATE INDEX lion_opt_inl_s ON lion_opt_inl USING lion (k)
	WITH (inline_limit = 1024);
SELECT entries, inline_entries, container_pages, containers, container_bytes
  FROM lion_index_stats('lion_opt_inl_d');
SELECT entries, inline_entries, container_pages, containers, container_bytes
  FROM lion_index_stats('lion_opt_inl_s');
SELECT lion_index_verify('lion_opt_inl_d', true);
SELECT lion_index_verify('lion_opt_inl_s', true);
SELECT pg_relation_size('lion_opt_inl_d') < pg_relation_size('lion_opt_inl_s')
	   AS inline_is_smaller;
DROP TABLE lion_opt_inl;

-- Things a lion index cannot do.
CREATE INDEX ON lion_opt USING lion (k, t);
CREATE INDEX ON lion_opt USING lion (k) INCLUDE (t);
CREATE INDEX ON lion_opt USING lion (k DESC);
CREATE INDEX ON lion_opt USING lion (k NULLS FIRST);
CREATE UNIQUE INDEX ON lion_opt USING lion (k);
CREATE INDEX ON lion_opt USING lion (i) WHERE k = 1;   -- partial: allowed
DROP INDEX lion_opt_i_idx;

-- No opclass for a type without a hash function
CREATE TABLE lion_noop (x point);
CREATE INDEX ON lion_noop USING lion (x);
DROP TABLE lion_noop;

DROP TABLE lion_opt;
