-- Reloptions and the limitations the handler advertises.
\set VERBOSITY terse
SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS pg_lion;

CREATE TABLE lion_opt AS
SELECT i, (i % 97)::int4 AS k, 'v' || (i % 97) AS t
  FROM generate_series(1, 20000) i;

-- buckets: accepted and ignored since format 4 (DESIGN.md §21), still
-- validated exactly as it was
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
 * buckets changes nothing at all since format 4: the entry directory is a
 * B-tree that grows by splitting, so two indexes that differ only in that
 * option are the same index (DESIGN.md §21).
 */
CREATE INDEX lion_opt_b7 ON lion_opt USING lion (k) WITH (buckets = 7);
CREATE INDEX lion_opt_b1000 ON lion_opt USING lion (k) WITH (buckets = 1000);
SELECT directory_height, leaf_pages, entries FROM lion_index_stats('lion_opt_b7');
SELECT directory_height, leaf_pages, entries FROM lion_index_stats('lion_opt_b1000');

-- fillfactor: 10 .. 100, default 90 (DESIGN.md §21)
CREATE INDEX lion_opt_ff ON lion_opt USING lion (k) WITH (fillfactor = 50);
CREATE INDEX lion_opt_ffbad1 ON lion_opt USING lion (k) WITH (fillfactor = 9);
CREATE INDEX lion_opt_ffbad2 ON lion_opt USING lion (k) WITH (fillfactor = 101);
SELECT reloptions FROM pg_class WHERE relname = 'lion_opt_ff';
ALTER INDEX lion_opt_ff SET (fillfactor = 100);
ALTER INDEX lion_opt_ff RESET (fillfactor);
DROP INDEX lion_opt_ff;
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

-- Multicolumn indexes are supported since DESIGN.md §24; test/sql/multicolumn.sql
-- is where they are exercised.  Here just prove the DDL is accepted.
CREATE INDEX ON lion_opt USING lion (k, t);
DROP INDEX lion_opt_k_t_idx;

-- Things a lion index cannot do.
CREATE INDEX ON lion_opt USING lion (k) INCLUDE (t);
-- (as WARNINGs: PostgreSQL 19 reports the ERROR's position and 18 does not)
DO $$ BEGIN CREATE INDEX ON lion_opt USING lion (k DESC);
EXCEPTION WHEN feature_not_supported THEN RAISE WARNING '%', SQLERRM; END $$;
DO $$ BEGIN CREATE INDEX ON lion_opt USING lion (k NULLS FIRST);
EXCEPTION WHEN feature_not_supported THEN RAISE WARNING '%', SQLERRM; END $$;
CREATE UNIQUE INDEX ON lion_opt USING lion (k);
CREATE INDEX ON lion_opt USING lion (i) WHERE k = 1;   -- partial: allowed
DROP INDEX lion_opt_i_idx;

-- No opclass for a type without a hash function
CREATE TABLE lion_noop (x point);
CREATE INDEX ON lion_noop USING lion (x);
DROP TABLE lion_noop;

-- An index carried by pg_upgrade from 16 or 17 onto a later server keeps the
-- pre-18 hash substitute its opclass was created with (DESIGN.md §23
-- addendum); it must validate on every major, as it did before the upgrade.
CREATE OPERATOR FAMILY lion_pre18_bool USING lion;
CREATE OPERATOR CLASS lion_pre18_bool_ops FOR TYPE bool USING lion
	FAMILY lion_pre18_bool AS
	OPERATOR	1	= (bool, bool),
	FUNCTION	1	hashchar("char"),
	FUNCTION	4	btboolcmp(bool, bool);
CREATE OPERATOR CLASS lion_pre18_date_ops FOR TYPE date USING lion AS
	OPERATOR	1	= (date, date),
	FUNCTION	1	hashint4(int4),
	FUNCTION	4	date_cmp(date, date);
SELECT opcname, amvalidate(oid) FROM pg_opclass
 WHERE opcname IN ('lion_pre18_bool_ops', 'lion_pre18_date_ops') ORDER BY opcname;
DROP OPERATOR FAMILY lion_pre18_bool USING lion;
DROP OPERATOR CLASS lion_pre18_date_ops USING lion;
DROP OPERATOR FAMILY lion_pre18_date_ops USING lion;

DROP TABLE lion_opt;
