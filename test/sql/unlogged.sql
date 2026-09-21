-- Unlogged relations exercise ambuildempty (init fork) and the
-- SET LOGGED / REINDEX rebuild path.
\set VERBOSITY terse
SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS roaring_index;

CREATE UNLOGGED TABLE rbi_unlogged AS
SELECT i, (i % 137)::int4 AS k, 'v' || (i % 137) AS t
  FROM generate_series(1, 50000) i;

CREATE INDEX rbi_unlogged_k ON rbi_unlogged USING roaring (k);
CREATE INDEX rbi_unlogged_t ON rbi_unlogged USING roaring (t)
	WITH (buckets = 16, inline_limit = 128);
ANALYZE rbi_unlogged;

SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM rbi_unlogged WHERE k = 42;
SELECT count(*), sum(i) FROM rbi_unlogged WHERE k = 42;
SELECT count(*), sum(i) FROM rbi_unlogged WHERE t = 'v42';
SET enable_bitmapscan = off; RESET enable_seqscan;
SELECT count(*), sum(i) FROM rbi_unlogged WHERE k = 42;
SELECT count(*), sum(i) FROM rbi_unlogged WHERE t = 'v42';
RESET enable_bitmapscan;

-- The init fork is written by ambuildempty; make the relation logged and
-- rebuild, then check the answers again.
ALTER TABLE rbi_unlogged SET LOGGED;
REINDEX TABLE rbi_unlogged;

SET enable_seqscan = off;
SELECT count(*), sum(i) FROM rbi_unlogged WHERE k = 42;
SELECT count(*), sum(i) FROM rbi_unlogged WHERE t = 'v42';
SET enable_bitmapscan = off; RESET enable_seqscan;
SELECT count(*), sum(i) FROM rbi_unlogged WHERE k = 42;
SELECT count(*), sum(i) FROM rbi_unlogged WHERE t = 'v42';
RESET enable_bitmapscan;

-- ... and back to unlogged, which runs ambuildempty again
ALTER TABLE rbi_unlogged SET UNLOGGED;
SET enable_seqscan = off;
SELECT count(*), sum(i) FROM rbi_unlogged WHERE k = 42;
RESET enable_seqscan;

DROP TABLE rbi_unlogged;
