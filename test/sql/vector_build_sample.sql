-- pg_lion.vector_build_sample: the build's partitions and maintenance_work_mem
-- (DESIGN.md §44.3, §44.8).
--
-- A lionvec build chooses its partitions from a reservoir sample of as many
-- rows as half of maintenance_work_mem holds, and makes no more than the
-- sample has rows for: 16 a partition it chooses by itself, one a partition
-- the partitions option asks for.  Where that is fewer than a sample of every
-- row would make, the build fails, in its first pass, at the first row that
-- makes it so, saying the partitions it wants, those the sample has rows for
-- and the maintenance_work_mem that would hold enough, rounded up to a
-- megabyte; the rows it names are then reckoned from the heap's blocks read
-- so far ("About"), the table's own when it gets that far.  allow builds the
-- partitions the sample has rows for, with a NOTICE that counts them.  A
-- table of fewer than 10,000 rows has one partition by itself, whatever the
-- sample holds of it, and an option of more partitions than the table's
-- rows wants one a row, which a sample of every row makes: neither fails.
--
-- At 8 dimensions a sample row is 68 bytes (64 padded dimensions and a
-- scale), so maintenance_work_mem's least, 64kB, holds 481 rows: 16 each for
-- 30 partitions.  A table of 10,000 rows wants 625 (a sixteenth of them, 8
-- sqrt(n) being 800), 10,000 sample rows, 1,329kB.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_lion_vector;
CREATE EXTENSION IF NOT EXISTS pageinspect;
RESET client_min_messages;
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;

CREATE FUNCTION bs_vec(i bigint) RETURNS vector LANGUAGE sql IMMUTABLE AS $$
	SELECT array_agg(((hashint8extended(i * 100 + j, 11) % 100000)::float8
					  / 100000.0)::real ORDER BY j)::vector
	  FROM generate_series(1, 8) j
$$;
-- the partitions on an index's meta page (a little-endian server's)
CREATE FUNCTION bs_parts(idx regclass) RETURNS bigint LANGUAGE sql AS $$
	SELECT get_byte(m, 56)::bigint | (get_byte(m, 57)::bigint << 8) |
		   (get_byte(m, 58)::bigint << 16) | (get_byte(m, 59)::bigint << 24)
	  FROM get_raw_page(idx::text, 0) m
$$;
-- an index expression that counts the rows a build reads, through a
-- sequence, whose count a failed build leaves; CREATE INDEX runs it under a
-- safe search_path from PostgreSQL 17 on
CREATE SEQUENCE bs_read;
CREATE FUNCTION bs_counted(v vector) RETURNS vector LANGUAGE plpgsql IMMUTABLE
SET search_path FROM CURRENT AS $$
BEGIN
	PERFORM nextval('bs_read');
	RETURN v;
END $$;

CREATE TABLE bs10 (id int, v vector(8)) WITH (autovacuum_enabled = off);
INSERT INTO bs10 SELECT i, bs_vec(i) FROM generate_series(1, 10000) i;
CREATE TABLE bs20 (id int, v vector(8)) WITH (autovacuum_enabled = off);
INSERT INTO bs20 SELECT i, bs_vec(i) FROM generate_series(1, 20000) i;
CREATE TABLE bs1k (id int, v vector(8)) WITH (autovacuum_enabled = off);
INSERT INTO bs1k SELECT i, bs_vec(i) FROM generate_series(1, 1000) i;

-- the default, error: the sample of 481 rows has them for 30 partitions of
-- the 625 that 10,000 rows want; the 10,000th row, the last, makes it so
SET maintenance_work_mem = '64kB';
\set VERBOSITY default
CREATE INDEX bs10_v ON bs10 USING lionvec (v);
\set VERBOSITY terse
SELECT current_setting('pg_lion.vector_build_sample') AS vector_build_sample;
-- what the hint says, and the least it rounds up from, make them all, and a
-- kilobyte less does not
SET maintenance_work_mem = '2MB';
CREATE INDEX bs10_v ON bs10 USING lionvec (v);
SELECT bs_parts('bs10_v') AS partitions;
SET maintenance_work_mem = '1329kB';
REINDEX INDEX bs10_v;
SELECT bs_parts('bs10_v') AS partitions;
SET maintenance_work_mem = '1328kB';
REINDEX INDEX bs10_v;
SELECT bs_parts('bs10_v') AS partitions;
DROP INDEX bs10_v;

-- 20,000 rows want 1,131 partitions: the build fails at the 10,000th row of
-- 20,000, the first with which the rows so far want more than 30, before
-- the rest of the first pass, k-means and the second, and reckons the rest
SET maintenance_work_mem = '64kB';
\set VERBOSITY default
CREATE INDEX bs20_v ON bs20 USING lionvec (bs_counted(v));
\set VERBOSITY terse
SELECT last_value AS rows_read FROM bs_read;

-- allow: the 30 partitions, with a NOTICE of the table's own count; at
-- the default again a REINDEX fails, and leaves the index as it was
SET pg_lion.vector_build_sample = allow;
\set VERBOSITY default
CREATE INDEX bs20_v ON bs20 USING lionvec (v);
\set VERBOSITY terse
SELECT bs_parts('bs20_v') AS partitions;
RESET pg_lion.vector_build_sample;
REINDEX INDEX bs20_v;
SELECT bs_parts('bs20_v') AS partitions;
SELECT count(*) FROM (SELECT id FROM bs20 ORDER BY v <-> bs_vec(7) LIMIT 10) s;
DROP INDEX bs20_v;

-- a parallel build reads the first pass in the leader alone, so it fails
-- there too, before a worker starts; under allow it makes the serial
-- build's 30
ALTER TABLE bs20 SET (parallel_workers = 2);
SET max_parallel_maintenance_workers = 2;
CREATE INDEX bs20_v ON bs20 USING lionvec (v);
SET pg_lion.vector_build_sample = allow;
CREATE INDEX bs20_v ON bs20 USING lionvec (v);
SELECT bs_parts('bs20_v') AS partitions;
RESET pg_lion.vector_build_sample;
RESET max_parallel_maintenance_workers;
ALTER TABLE bs20 RESET (parallel_workers);
DROP INDEX bs20_v;

-- the partitions option: 600 of 1,000 rows, one sample row each, of which
-- the reservoir holds 481; the 482nd row makes it short
\set VERBOSITY default
CREATE INDEX bs1k_v ON bs1k USING lionvec (v) WITH (partitions = 600);
\set VERBOSITY terse
SET pg_lion.vector_build_sample = allow;
CREATE INDEX bs1k_v ON bs1k USING lionvec (v) WITH (partitions = 600);
SELECT bs_parts('bs1k_v') AS partitions;
RESET pg_lion.vector_build_sample;
SET maintenance_work_mem = '1MB';
REINDEX INDEX bs1k_v;
SELECT bs_parts('bs1k_v') AS partitions;
DROP INDEX bs1k_v;
-- more partitions than the table has rows, from a sample of every row: as
-- many as the rows, which no memory setting changes, and no error
CREATE INDEX bs1k_v ON bs1k USING lionvec (v) WITH (partitions = 2000);
SELECT bs_parts('bs1k_v') AS partitions;
DROP INDEX bs1k_v;

-- fewer than 10,000 rows: one partition, whatever the sample holds of them
SET maintenance_work_mem = '64kB';
CREATE INDEX bs10_v ON bs10 USING lionvec (v) WHERE id < 10000;
SELECT bs_parts('bs10_v') AS partitions;
DROP INDEX bs10_v;

SET pg_lion.vector_build_sample = maybe;
RESET maintenance_work_mem;
DROP TABLE bs10, bs20, bs1k;
DROP FUNCTION bs_counted(vector);
DROP SEQUENCE bs_read;
DROP FUNCTION bs_parts(regclass);
DROP FUNCTION bs_vec(bigint);
