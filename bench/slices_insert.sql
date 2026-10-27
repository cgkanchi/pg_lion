-- bench/slices_insert.sql - one run of the insert measurement of DESIGN.md
-- §51.11 and §51 "Insert cost, revisited": 100,000 rows of uniform int4 and
-- int8 values into a table of 1,000,000 rows with a lion index on each
-- column, by one INSERT ... SELECT and then by 10,000 single-row INSERTs, the
-- time and the WAL of each.
--
--   psql -X -q -v sl=on -f bench/slices_insert.sql     (sl: on, off or none)
--
-- prints "select|ms|wal bytes" and "single|ms|wal bytes".  The values come
-- from seeded random(), so every run inserts the same rows and the WAL bytes
-- of two runs of one build agree to a few hundred.  It CHECKPOINTs first, so
-- that no checkpoint falls inside a run on a cluster whose max_wal_size is
-- above about 1 GB.  For an A/B of two builds, alternate the runs - install
-- one build's pg_lion.so, restart the cluster (a preloaded library is only
-- read at start), run, and the same for the other - and compare medians: one
-- run is not a measurement on a shared machine.
\set ON_ERROR_STOP on
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET maintenance_work_mem = '1GB';
DROP TABLE IF EXISTS slc_bi;
CHECKPOINT;
SELECT setseed(0.42) \gset
CREATE TABLE slc_bi (v4 int4, v8 int8) WITH (autovacuum_enabled = off);
INSERT INTO slc_bi SELECT (floor(random() * 4294967296) - 2147483648)::int8::int4, (((floor(random() * 4294967296) - 2147483648)::int8 << 32) | floor(random() * 4294967296)::int8) FROM generate_series(1, 1000000);
SELECT :'sl' <> 'none' AS haveidx \gset
\if :haveidx
CREATE INDEX slc_bi_v4 ON slc_bi USING lion (v4) WITH (slices = :sl);
CREATE INDEX slc_bi_v8 ON slc_bi USING lion (v8) WITH (slices = :sl);
\endif
CREATE TEMP TABLE r (what text, ms numeric, wal int8);
DO $$
DECLARE
	t0 timestamptz;
	lsn pg_lsn;
BEGIN
	PERFORM setseed(0.17);
	lsn := pg_current_wal_insert_lsn();
	t0 := clock_timestamp();
	INSERT INTO slc_bi SELECT (floor(random() * 4294967296) - 2147483648)::int8::int4, (((floor(random() * 4294967296) - 2147483648)::int8 << 32) | floor(random() * 4294967296)::int8) FROM generate_series(1, 100000);
	INSERT INTO r VALUES ('select', round(extract(epoch FROM clock_timestamp() - t0) * 1000), pg_wal_lsn_diff(pg_current_wal_insert_lsn(), lsn));
END $$;
DO $$
DECLARE
	t0 timestamptz;
	lsn pg_lsn;
	i int;
BEGIN
	PERFORM setseed(0.29);
	lsn := pg_current_wal_insert_lsn();
	t0 := clock_timestamp();
	FOR i IN 1 .. 10000 LOOP
		EXECUTE 'INSERT INTO slc_bi VALUES ($1, $2)'
			USING (floor(random() * 4294967296) - 2147483648)::int8::int4,
				  (((floor(random() * 4294967296) - 2147483648)::int8 << 32) | floor(random() * 4294967296)::int8);
	END LOOP;
	INSERT INTO r VALUES ('single', round(extract(epoch FROM clock_timestamp() - t0) * 1000), pg_wal_lsn_diff(pg_current_wal_insert_lsn(), lsn));
END $$;
\pset format unaligned
\pset tuples_only on
SELECT what || '|' || ms || '|' || wal FROM r;
DROP TABLE slc_bi;
