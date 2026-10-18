-- lionvec under PostgreSQL 16's old_snapshot_threshold (DESIGN.md §9,
-- "old_snapshot_threshold"; §44.5), the companion of oldsnapshot.sql:
-- test/hook-check.sh runs it on the same 16 server, started with
-- old_snapshot_threshold = 60, where pgvector is installed.
--
-- With the threshold set, VACUUM may remove the items of rows an old
-- snapshot still sees.  Core's access methods turn that into "snapshot too
-- old" by testing every page they read; lionvec tests none, and neither
-- does 16's heap fetch under an index scan (heap_hot_search_buffer()), so
-- such a row would be missing from the answer without a word.  So no
-- lionvec index is read while the threshold is set: lionveccostestimate()
-- prices every lionvec order out with disable_cost, no LionOrdered is
-- offered over one, and a filtered order is planned exactly rather than
-- refused under pg_lion.vector_postfilter = error (until 2026-10-18 it
-- failed at plan time, LionOrdered being unavailable); and a scan that
-- reaches a lionvec index anyway, in a plan with every other path
-- disabled, its vectors inline or in TOAST, is an ERROR before it reads a
-- page.  Inserts, updates, deletes, VACUUM and REINDEX are not refused.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_lion_vector;
RESET client_min_messages;
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SHOW old_snapshot_threshold;

-- a pseudo-random vector of n coordinates in (-scale, scale), the same on
-- every platform
CREATE FUNCTION ov_vec(seed bigint, n int, scale float8 DEFAULT 1.0)
RETURNS vector LANGUAGE sql IMMUTABLE PARALLEL SAFE AS $$
	SELECT array_agg(((hashint8extended(seed * 10000 + j, 46) % 100000)::float8
					  / 100000.0 * scale)::real ORDER BY j)::vector
	  FROM generate_series(1, n) j
$$;

-- q's plan, each vector literal as '...'
CREATE FUNCTION ov_plan(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		RETURN NEXT regexp_replace(ln, '''\[[^]]*\]''::vector', '''...''', 'g');
	END LOOP;
END $$;

-- how q's plan, a Limit over a scan, prices the scan: with disable_cost
-- (1.0e10, 16's price of a disabled path) or without
CREATE FUNCTION ov_price(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	p json;
BEGIN
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO p;
	p := p->0->'Plan'->'Plans'->0;
	RETURN format('%s using %s: %s', p->>'Node Type', p->>'Index Name',
				  CASE WHEN (p->>'Startup Cost')::float8 >= 1.0e10
					   THEN 'disable_cost' ELSE 'its own cost' END);
END $$;

-- 2,000 rows of 16 dimensions, their vectors inline, in four partitions;
-- cat, ten values, has a lion index
CREATE TABLE ov (id int NOT NULL, cat int NOT NULL, v vector(16))
	WITH (autovacuum_enabled = off);
INSERT INTO ov SELECT i, i % 10, ov_vec(i, 16) FROM generate_series(1, 2000) i;
CREATE INDEX ov_v ON ov USING lionvec (v) WITH (partitions = 4);
CREATE INDEX ov_cat ON ov USING lion (cat);
-- 200 rows of 1,536 dimensions, their vectors in TOAST: an index that keeps
-- a locator, which a scan would read them by (§44.13)
CREATE TABLE ovt (id int NOT NULL, cat int NOT NULL, v vector(1536))
	WITH (autovacuum_enabled = off);
INSERT INTO ovt SELECT i, i % 10, ov_vec(i, 1536) FROM generate_series(1, 200) i;
CREATE INDEX ovt_v ON ovt USING lionvec (v) WITH (partitions = 2);
VACUUM (FREEZE, ANALYZE) ov, ovt;
SELECT pg_relation_size(reltoastrelid) > 100 * 8192 AS in_toast
  FROM pg_class WHERE relname = 'ovt';
SHOW pg_lion.vector_postfilter;

-- The planner reads no lionvec index: a sort of the rows, for the plain
-- order and for the filtered one, which under error is not refused, and
-- with a sort's rows.
SELECT * FROM ov_plan('SELECT id FROM ov ORDER BY v <-> ov_vec(-1, 16) LIMIT 5');
SELECT * FROM ov_plan('SELECT id FROM ov WHERE cat = 3 ORDER BY v <-> ov_vec(-1, 16) LIMIT 5');
SELECT id, round((v <-> ov_vec(-1, 16))::numeric, 4) AS d
  FROM ov ORDER BY v <-> ov_vec(-1, 16) LIMIT 5;
SELECT id, round((v <-> ov_vec(-1, 16))::numeric, 4) AS d
  FROM ov WHERE cat = 3 ORDER BY v <-> ov_vec(-1, 16) LIMIT 5;
SELECT id, round((v <-> ov_vec(-1, 1536))::numeric, 4) AS d
  FROM ovt ORDER BY v <-> ov_vec(-1, 1536) LIMIT 5;

-- Every other path disabled: a lionvec scan is all that is left, priced
-- with disable_cost as the sequential scan and the sort are, and it refuses
-- to read - over vectors inline; under a filter it would apply after its
-- scan, which allow lets the planner choose; and over vectors in TOAST.  (A
-- parallel scan cannot be had here: its path and the serial one both carry
-- disable_cost, which leaves their costs within add_path()'s fuzz of each
-- other, and the serial one is kept.  Each of its processes would refuse
-- in the same place, lv_start().)
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_sort = off;
SELECT * FROM ov_plan('SELECT id FROM ov ORDER BY v <-> ov_vec(-1, 16) LIMIT 5');
SELECT ov_price('SELECT id FROM ov ORDER BY v <-> ov_vec(-1, 16) LIMIT 5');
SELECT id FROM ov ORDER BY v <-> ov_vec(-1, 16) LIMIT 5;
SET pg_lion.vector_postfilter = allow;
SELECT * FROM ov_plan('SELECT id FROM ov WHERE cat = 3 ORDER BY v <-> ov_vec(-1, 16) LIMIT 5');
\set VERBOSITY terse
SELECT id FROM ov WHERE cat = 3 ORDER BY v <-> ov_vec(-1, 16) LIMIT 5;
RESET pg_lion.vector_postfilter;
SELECT * FROM ov_plan('SELECT id FROM ovt ORDER BY v <-> ov_vec(-1, 1536) LIMIT 5');
SELECT ov_price('SELECT id FROM ovt ORDER BY v <-> ov_vec(-1, 1536) LIMIT 5');
SELECT id FROM ovt ORDER BY v <-> ov_vec(-1, 1536) LIMIT 5;
\set VERBOSITY default
RESET enable_sort;
RESET enable_bitmapscan;
RESET enable_seqscan;

-- Writes, VACUUM and REINDEX are not refused: rows in, a vector changed (a
-- non-HOT update, an insert into the index), rows out, the dead rows'
-- items removed - after which the index counts the table's rows - and the
-- index rebuilt.
INSERT INTO ov SELECT i, i % 10, ov_vec(i, 16) FROM generate_series(2001, 2100) i;
UPDATE ov SET v = ov_vec(-id, 16) WHERE id BETWEEN 1 AND 50;
DELETE FROM ov WHERE id BETWEEN 51 AND 100;
INSERT INTO ovt SELECT i, i % 10, ov_vec(i, 1536) FROM generate_series(201, 220) i;
UPDATE ovt SET v = ov_vec(-id, 1536) WHERE id BETWEEN 1 AND 10;
DELETE FROM ovt WHERE id BETWEEN 11 AND 20;
VACUUM (FREEZE, INDEX_CLEANUP ON) ov, ovt;
SELECT relname, reltuples = CASE relname WHEN 'ov_v' THEN (SELECT count(*) FROM ov)
										 ELSE (SELECT count(*) FROM ovt) END AS the_tables_rows
  FROM pg_class WHERE relname IN ('ov_v', 'ovt_v') ORDER BY relname;
REINDEX INDEX ov_v;
REINDEX INDEX ovt_v;
SELECT id, round((v <-> ov_vec(-1, 16))::numeric, 4) AS d
  FROM ov ORDER BY v <-> ov_vec(-1, 16) LIMIT 5;

DROP TABLE ov, ovt;
DROP FUNCTION ov_price(text);
DROP FUNCTION ov_plan(text);
DROP FUNCTION ov_vec(bigint, int, float8);
