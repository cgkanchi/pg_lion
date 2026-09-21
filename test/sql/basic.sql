-- Basic functional test for the roaring index access method.
\set VERBOSITY terse
SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS roaring_index;

-- Deterministic data set with a mix of cardinalities.  Only rbi_r10 depends
-- on the seeded PRNG; every other column is a pure function of i.
SELECT setseed(0.42);

CREATE TABLE rbi_basic AS
SELECT i,
       (i % 2) = 0                                  AS b2,
       (i % 10)::int4                               AS c10,
       ((i::bigint * 7919) % 1000)::int4                    AS c1000,
       ((i::bigint * 104729) % 50000)::int8                 AS c50k,
       ('key ' || ((i::bigint * 31) % 1000))::text          AS t1000,
       (DATE '2020-01-01' + (i % 10))               AS d10,
       (((i % 1000)::numeric) / 8)                  AS n1000,
       md5((i % 1000)::text)::uuid                  AS u1000,
       (random() * 9)::int4                         AS r10,
       CASE WHEN i % 1000 = 0 THEN NULL ELSE (i % 5)::int4 END AS cnull
  FROM generate_series(1, 200000) i;

CREATE INDEX rbi_b2 ON rbi_basic USING roaring (b2);
CREATE INDEX rbi_c10 ON rbi_basic USING roaring (c10);
CREATE INDEX rbi_c1000 ON rbi_basic USING roaring (c1000);
CREATE INDEX rbi_c50k ON rbi_basic USING roaring (c50k);
CREATE INDEX rbi_t1000 ON rbi_basic USING roaring (t1000);
CREATE INDEX rbi_d10 ON rbi_basic USING roaring (d10);
CREATE INDEX rbi_n1000 ON rbi_basic USING roaring (n1000);
CREATE INDEX rbi_u1000 ON rbi_basic USING roaring (u1000);
CREATE INDEX rbi_r10 ON rbi_basic USING roaring (r10);
CREATE INDEX rbi_cnull ON rbi_basic USING roaring (cnull);

ANALYZE rbi_basic;

/*
 * rbi_cmp() runs the same predicate twice: once with only bitmap scans
 * enabled (and verifies from the plan that the roaring index really was
 * used) and once with a plain sequential scan.  It reports the row count so
 * that a wrong-but-consistent answer would still show up in the diff.
 */
CREATE OR REPLACE FUNCTION rbi_cmp(tbl text, pred text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	q text := format('SELECT count(*) AS c, coalesce(sum(i), 0) AS s FROM %s WHERE %s', tbl, pred);
	a record;
	b record;
	ln text;
	used boolean := false;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', false);
	PERFORM set_config('enable_bitmapscan', 'on', false);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Bitmap Index Scan%' THEN
			used := true;
		END IF;
	END LOOP;
	EXECUTE q INTO a;

	PERFORM set_config('enable_seqscan', 'on', false);
	PERFORM set_config('enable_bitmapscan', 'off', false);
	EXECUTE q INTO b;
	PERFORM set_config('enable_bitmapscan', 'on', false);

	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH index=%s seqscan=%s', a::text, b::text);
	END IF;
	IF NOT used THEN
		RETURN format('NO BITMAP INDEX SCAN (%s rows)', a.c);
	END IF;
	RETURN format('ok %s rows', a.c);
END $$;

-- The plan really is a bitmap index scan on the roaring index.
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM rbi_basic WHERE c1000 = 42;
EXPLAIN (COSTS OFF) SELECT count(*) FROM rbi_basic WHERE t1000 = 'key 7';
RESET enable_seqscan;

-- 2 distinct values: one bitset container per 64 heap blocks.
SELECT rbi_cmp('rbi_basic', 'b2');
SELECT rbi_cmp('rbi_basic', 'NOT b2');
-- 10 distinct values
SELECT rbi_cmp('rbi_basic', 'c10 = 0');
SELECT rbi_cmp('rbi_basic', 'c10 = 9');
-- 1000 distinct values
SELECT rbi_cmp('rbi_basic', 'c1000 = 0');
SELECT rbi_cmp('rbi_basic', 'c1000 = 42');
SELECT rbi_cmp('rbi_basic', 'c1000 = 999');
-- 50000 distinct values (int8)
SELECT rbi_cmp('rbi_basic', 'c50k = 29');
SELECT rbi_cmp('rbi_basic', 'c50k = 49729');
-- text, date, numeric, uuid
SELECT rbi_cmp('rbi_basic', 't1000 = ''key 0''');
SELECT rbi_cmp('rbi_basic', 't1000 = ''key 777''');
SELECT rbi_cmp('rbi_basic', 'd10 = DATE ''2020-01-05''');
SELECT rbi_cmp('rbi_basic', 'n1000 = 124.875');
SELECT rbi_cmp('rbi_basic', 'u1000 = md5(''500'')::uuid');
-- seeded-random column
SELECT rbi_cmp('rbi_basic', 'r10 = 4');

-- keys that match no row at all
SELECT rbi_cmp('rbi_basic', 'c1000 = -1');
SELECT rbi_cmp('rbi_basic', 'c50k = 1000000');
SELECT rbi_cmp('rbi_basic', 't1000 = ''no such key''');
SELECT rbi_cmp('rbi_basic', 'd10 = DATE ''1999-01-01''');

-- NULL keys are not indexed: the scan must return nothing and not fail.
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM rbi_basic WHERE cnull = (SELECT NULL::int4);
SELECT count(*) FROM rbi_basic WHERE cnull = (SELECT NULL::int4);
RESET enable_seqscan;
SELECT rbi_cmp('rbi_basic', 'cnull = 3');
-- IS NULL cannot use the index but must still be right
SELECT count(*) FROM rbi_basic WHERE cnull IS NULL;

-- BitmapAnd over two roaring indexes
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM rbi_basic WHERE c10 = 3 AND c1000 = 419;
RESET enable_seqscan;
SELECT rbi_cmp('rbi_basic', 'c10 = 3 AND c1000 = 419');

-- BitmapOr: the planner uses one index scan per array element
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM rbi_basic WHERE c1000 IN (11, 22, 33);
EXPLAIN (COSTS OFF) SELECT count(*) FROM rbi_basic WHERE c1000 = 11 OR t1000 = 'key 500';
RESET enable_seqscan;
SELECT rbi_cmp('rbi_basic', 'c1000 IN (11, 22, 33)');
SELECT rbi_cmp('rbi_basic', 'c1000 = 11 OR t1000 = ''key 500''');

-- Cross-type equality inside the shared integer operator family
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM rbi_basic WHERE c50k = 29::int4;
EXPLAIN (COSTS OFF) SELECT count(*) FROM rbi_basic WHERE c10 = 3::int8;
RESET enable_seqscan;
SELECT rbi_cmp('rbi_basic', 'c50k = 29::int4');
SELECT rbi_cmp('rbi_basic', 'c50k = 29::int2');
SELECT rbi_cmp('rbi_basic', 'c10 = 3::int8');
SELECT rbi_cmp('rbi_basic', 'c10 = 3::int2');

-- varchar reaches the text opclass by binary coercion
CREATE TABLE rbi_varchar (v varchar(32));
INSERT INTO rbi_varchar SELECT 'v' || (i % 100) FROM generate_series(1, 10000) i;
CREATE INDEX rbi_varchar_idx ON rbi_varchar USING roaring (v);
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM rbi_varchar WHERE v = 'v42';
SELECT count(*) FROM rbi_varchar WHERE v = 'v42';
RESET enable_seqscan;

-- An enum column uses the anyenum opclass
CREATE TYPE rbi_color AS ENUM ('red', 'green', 'blue');
CREATE TABLE rbi_enum AS
SELECT (ARRAY['red', 'green', 'blue']::rbi_color[])[1 + (i % 3)] AS c
  FROM generate_series(1, 5000) i;
CREATE INDEX rbi_enum_idx ON rbi_enum USING roaring (c);
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM rbi_enum WHERE c = 'green';
SELECT count(*) FROM rbi_enum WHERE c = 'green';
RESET enable_seqscan;

-- An empty table builds a valid (tiny) index
CREATE TABLE rbi_empty (k int4);
CREATE INDEX rbi_empty_idx ON rbi_empty USING roaring (k);
SET enable_seqscan = off;
SELECT count(*) FROM rbi_empty WHERE k = 1;
RESET enable_seqscan;

-- An insert into an index built on an empty table creates the first entry
INSERT INTO rbi_empty VALUES (1);

/*
 * VACUUM removes the deleted TIDs from the index; the answers must stay
 * correct both before and after it runs.
 */
CREATE TABLE rbi_vac AS
SELECT i, (i % 50)::int4 AS k FROM generate_series(1, 20000) i;
CREATE INDEX rbi_vac_idx ON rbi_vac USING roaring (k);
VACUUM rbi_vac;
SET enable_seqscan = off;
SELECT count(*), sum(i) FROM rbi_vac WHERE k = 7;
DELETE FROM rbi_vac WHERE i <= 10000;
VACUUM rbi_vac;
SELECT count(*), sum(i) FROM rbi_vac WHERE k = 7;
RESET enable_seqscan;
SELECT count(*), sum(i) FROM rbi_vac WHERE k + 0 = 7;
DROP TABLE rbi_vac;

DROP TABLE rbi_varchar, rbi_enum, rbi_empty;
DROP TYPE rbi_color;
