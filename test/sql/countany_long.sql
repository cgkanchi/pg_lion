-- lion_index_count_any() over a list too long to locate at once (2026-09-29
-- review; DESIGN.md §15, "A list too long to locate at once").
--
-- The function located every value's posting set at once, whatever work_mem
-- said: an array of them some 120 bytes a value, which past nine million
-- values is more than an allocation may have ("invalid memory alloc request
-- size"), where the pushdown's `k = ANY (...)` answered - and before that a
-- gigabyte of memory.  A list longer than a work_mem of sets is now located a
-- batch at a time, as the pushdown's plain count locates one, and the
-- batches' counts are added up: exact, because the entries of one scalar
-- index are disjoint and a batch never splits the values of one entry.  Each
-- count below must equal the query's own, answered by a sequential scan.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
CREATE EXTENSION IF NOT EXISTS citext;
CREATE EXTENSION IF NOT EXISTS pg_lion_citext;
RESET client_min_messages;
/* VACUUM can only set all-visible for commits that reached disk (citext.sql). */
SET synchronous_commit = on;

CREATE TABLE lion_cal (k int, t text NOT NULL, c citext NOT NULL);
INSERT INTO lion_cal
SELECT CASE WHEN i % 23 = 0 THEN NULL ELSE i % 5000 END, 'v' || (i % 4000),
	   (ARRAY['Ab', 'cD', 'EF'])[1 + i % 3] || (i % 1500)
  FROM generate_series(1, 60000) i;
CREATE INDEX lion_cal_k ON lion_cal USING lion (k);
CREATE INDEX lion_cal_t ON lion_cal USING lion (t);
CREATE INDEX lion_cal_c ON lion_cal USING lion (c);
VACUUM ANALYZE lion_cal;

/*
 * lion_calcmp(idx, col, arr) counts the array expression arr through
 * lion_index_count_any(idx, arr), and again as `col = ANY (arr)` answered by a
 * sequential scan with the count pushdown off - the array made a constant
 * first, so that the scan hashes it - and reports the count and whether the
 * two agree.
 */
CREATE FUNCTION lion_calcmp(idx regclass, col text, arr text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	a bigint;
	b bigint;
	val text;
	typ text;
BEGIN
	EXECUTE format('SELECT lion_index_count_any(%L, %s)', idx, arr) INTO a;
	EXECUTE format('SELECT (%s)::text, pg_typeof(%s)::text', arr, arr) INTO val, typ;
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	EXECUTE format('SELECT count(*) FROM lion_cal WHERE %s = ANY (%L::%s)', col, val, typ)
		INTO b;
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH: %s against %s', a, b);
	END IF;
	RETURN format('%s, same answer', a);
END $$;

-- a batch is a work_mem of located sets, never fewer than a thousand values
SET work_mem = '64kB';
-- distinct values, three batches
SELECT lion_calcmp('lion_cal_k', 'k', 'array(SELECT g FROM generate_series(0, 2999) g)');
-- repeats, side by side and scattered, straddling every batch boundary
SELECT lion_calcmp('lion_cal_k', 'k', 'array(SELECT g % 1500 FROM generate_series(0, 5999) g)');
SELECT lion_calcmp('lion_cal_k', 'k', 'array(SELECT (g * 7) % 2600 FROM generate_series(0, 4999) g)');
SELECT lion_calcmp('lion_cal_k', 'k', 'array_fill(7, ARRAY[3000])');
-- NULL elements, which match nothing - not even the rows whose k is NULL
SELECT lion_calcmp('lion_cal_k', 'k', 'array(SELECT CASE WHEN g % 7 = 0 THEN NULL ELSE g END FROM generate_series(0, 3499) g)');
SELECT lion_calcmp('lion_cal_k', 'k', 'array_fill(NULL::int, ARRAY[3000])');
-- values with no entry: all of them, and all but a few
SELECT lion_calcmp('lion_cal_k', 'k', 'array(SELECT g FROM generate_series(100000, 103000) g)');
SELECT lion_calcmp('lion_cal_k', 'k', 'array(SELECT g FROM generate_series(4990, 7990) g)');
-- another element type than the key's: a cross-type probe, and a relabelled one
SELECT lion_calcmp('lion_cal_k', 'k', 'array(SELECT g::int8 FROM generate_series(0, 2999) g)');
SELECT lion_calcmp('lion_cal_t', 't', $$array(SELECT ('v' || (g * 3))::varchar FROM generate_series(0, 2999) g)$$);
-- another deterministic collation than the column's
SELECT lion_calcmp('lion_cal_t', 't', $$array(SELECT 'v' || (g * 3) FROM generate_series(0, 2999) g) COLLATE "C"$$);
-- values that are one entry without being byte-equal: 'ab7' and 'AB7' are one
-- citext key, hash alike and sort together, and a batch never splits them
SELECT lion_calcmp('lion_cal_c', 'c', $$array(SELECT (ARRAY['ab', 'CD', 'ef', 'AB', 'Cd'])[1 + g % 5] || (g % 1500) FROM generate_series(0, 5999) g)::citext[]$$);
/*
 * Nine million elements, which is where the array of sets passed the 1GB an
 * allocation may have: "invalid memory alloc request size 1080000000".  NULLs
 * and a few repeated values, so that it is quick.
 */
SELECT lion_index_count_any('lion_cal_k', array_fill(NULL::int, ARRAY[9000000]) || ARRAY[1, 2, 3, 3, 2]) AS nine_million,
	   (SELECT count(*) FROM lion_cal WHERE k IN (1, 2, 3)) AS expected;
-- a dirty heap
UPDATE lion_cal SET k = k + 1 WHERE k % 17 = 0;
DELETE FROM lion_cal WHERE k % 19 = 0;
SELECT lion_calcmp('lion_cal_k', 'k', 'array(SELECT g FROM generate_series(0, 2999) g)');
SELECT lion_calcmp('lion_cal_k', 'k', 'array(SELECT g % 1500 FROM generate_series(0, 5999) g)');
-- with the default work_mem the same list is one batch: located whole
RESET work_mem;
SELECT lion_calcmp('lion_cal_k', 'k', 'array(SELECT g FROM generate_series(0, 2999) g)');

DROP FUNCTION lion_calcmp(regclass, text, text);
DROP TABLE lion_cal;
DROP EXTENSION pg_lion_citext;
DROP EXTENSION citext;
