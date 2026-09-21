-- Basic functional test for the lion index access method.
\set VERBOSITY terse
SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS pg_lion;
-- This file exercises the access method's bitmap-scan path; the count pushdown
-- (tested in pushdown.sql) would otherwise take over the count(*) demonstrations.
SET pg_lion.enable_count_pushdown = off;

-- Deterministic data set with a mix of cardinalities.  Only lion_r10 depends
-- on the seeded PRNG; every other column is a pure function of i.
SELECT setseed(0.42);

CREATE TABLE lion_basic AS
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

CREATE INDEX lion_b2 ON lion_basic USING lion (b2);
CREATE INDEX lion_c10 ON lion_basic USING lion (c10);
CREATE INDEX lion_c1000 ON lion_basic USING lion (c1000);
CREATE INDEX lion_c50k ON lion_basic USING lion (c50k);
CREATE INDEX lion_t1000 ON lion_basic USING lion (t1000);
CREATE INDEX lion_d10 ON lion_basic USING lion (d10);
CREATE INDEX lion_n1000 ON lion_basic USING lion (n1000);
CREATE INDEX lion_u1000 ON lion_basic USING lion (u1000);
CREATE INDEX lion_r10 ON lion_basic USING lion (r10);
CREATE INDEX lion_cnull ON lion_basic USING lion (cnull);

ANALYZE lion_basic;

/*
 * lion_cmp() runs the same predicate twice: once with only bitmap scans
 * enabled (and verifies from the plan that the lion index really was
 * used) and once with a plain sequential scan.  It reports the row count so
 * that a wrong-but-consistent answer would still show up in the diff.
 */
CREATE OR REPLACE FUNCTION lion_cmp(tbl text, pred text) RETURNS text
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

-- The plan really is a bitmap index scan on the lion index.
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_basic WHERE c1000 = 42;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_basic WHERE t1000 = 'key 7';
RESET enable_seqscan;

-- 2 distinct values: one bitset container per 64 heap blocks.
SELECT lion_cmp('lion_basic', 'b2');
SELECT lion_cmp('lion_basic', 'NOT b2');
-- 10 distinct values
SELECT lion_cmp('lion_basic', 'c10 = 0');
SELECT lion_cmp('lion_basic', 'c10 = 9');
-- 1000 distinct values
SELECT lion_cmp('lion_basic', 'c1000 = 0');
SELECT lion_cmp('lion_basic', 'c1000 = 42');
SELECT lion_cmp('lion_basic', 'c1000 = 999');
-- 50000 distinct values (int8)
SELECT lion_cmp('lion_basic', 'c50k = 29');
SELECT lion_cmp('lion_basic', 'c50k = 49729');
-- text, date, numeric, uuid
SELECT lion_cmp('lion_basic', 't1000 = ''key 0''');
SELECT lion_cmp('lion_basic', 't1000 = ''key 777''');
SELECT lion_cmp('lion_basic', 'd10 = DATE ''2020-01-05''');
SELECT lion_cmp('lion_basic', 'n1000 = 124.875');
SELECT lion_cmp('lion_basic', 'u1000 = md5(''500'')::uuid');
-- seeded-random column
SELECT lion_cmp('lion_basic', 'r10 = 4');

-- keys that match no row at all
SELECT lion_cmp('lion_basic', 'c1000 = -1');
SELECT lion_cmp('lion_basic', 'c50k = 1000000');
SELECT lion_cmp('lion_basic', 't1000 = ''no such key''');
SELECT lion_cmp('lion_basic', 'd10 = DATE ''1999-01-01''');

-- NULL keys are not indexed: the scan must return nothing and not fail.
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_basic WHERE cnull = (SELECT NULL::int4);
SELECT count(*) FROM lion_basic WHERE cnull = (SELECT NULL::int4);
RESET enable_seqscan;
SELECT lion_cmp('lion_basic', 'cnull = 3');
-- IS NULL cannot use the index but must still be right
SELECT count(*) FROM lion_basic WHERE cnull IS NULL;

-- BitmapAnd over two lion indexes
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_basic WHERE c10 = 3 AND c1000 = 419;
RESET enable_seqscan;
SELECT lion_cmp('lion_basic', 'c10 = 3 AND c1000 = 419');

-- BitmapOr: the planner uses one index scan per array element
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_basic WHERE c1000 IN (11, 22, 33);
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_basic WHERE c1000 = 11 OR t1000 = 'key 500';
RESET enable_seqscan;
SELECT lion_cmp('lion_basic', 'c1000 IN (11, 22, 33)');
SELECT lion_cmp('lion_basic', 'c1000 = 11 OR t1000 = ''key 500''');

-- Cross-type equality inside the shared integer operator family
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_basic WHERE c50k = 29::int4;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_basic WHERE c10 = 3::int8;
RESET enable_seqscan;
SELECT lion_cmp('lion_basic', 'c50k = 29::int4');
SELECT lion_cmp('lion_basic', 'c50k = 29::int2');
SELECT lion_cmp('lion_basic', 'c10 = 3::int8');
SELECT lion_cmp('lion_basic', 'c10 = 3::int2');

-- varchar reaches the text opclass by binary coercion
CREATE TABLE lion_varchar (v varchar(32));
INSERT INTO lion_varchar SELECT 'v' || (i % 100) FROM generate_series(1, 10000) i;
CREATE INDEX lion_varchar_idx ON lion_varchar USING lion (v);
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_varchar WHERE v = 'v42';
SELECT count(*) FROM lion_varchar WHERE v = 'v42';
RESET enable_seqscan;

-- An enum column uses the anyenum opclass
CREATE TYPE lion_color AS ENUM ('red', 'green', 'blue');
CREATE TABLE lion_enum AS
SELECT (ARRAY['red', 'green', 'blue']::lion_color[])[1 + (i % 3)] AS c
  FROM generate_series(1, 5000) i;
CREATE INDEX lion_enum_idx ON lion_enum USING lion (c);
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_enum WHERE c = 'green';
SELECT count(*) FROM lion_enum WHERE c = 'green';
RESET enable_seqscan;

-- An empty table builds a valid (tiny) index
CREATE TABLE lion_empty (k int4);
CREATE INDEX lion_empty_idx ON lion_empty USING lion (k);
SET enable_seqscan = off;
SELECT count(*) FROM lion_empty WHERE k = 1;
RESET enable_seqscan;

-- An insert into an index built on an empty table creates the first entry
INSERT INTO lion_empty VALUES (1);

/*
 * VACUUM removes the deleted TIDs from the index; the answers must stay
 * correct both before and after it runs.
 */
CREATE TABLE lion_vac AS
SELECT i, (i % 50)::int4 AS k FROM generate_series(1, 20000) i;
CREATE INDEX lion_vac_idx ON lion_vac USING lion (k);
VACUUM lion_vac;
SET enable_seqscan = off;
SELECT count(*), sum(i) FROM lion_vac WHERE k = 7;
DELETE FROM lion_vac WHERE i <= 10000;
VACUUM lion_vac;
SELECT count(*), sum(i) FROM lion_vac WHERE k = 7;
RESET enable_seqscan;
SELECT count(*), sum(i) FROM lion_vac WHERE k + 0 = 7;
DROP TABLE lion_vac;

DROP TABLE lion_varchar, lion_enum, lion_empty;
DROP TYPE lion_color;
