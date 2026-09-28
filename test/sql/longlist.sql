-- An IN list too long to locate at once (2026-09-28 review; DESIGN.md §15,
-- "A list too long to locate at once").
--
-- A parameter's array has no length cap, and the pushdown held every
-- value's located posting set at once, work_mem or not - past some nine
-- million values more than an allocation may have.  A count of the relation
-- as one row now locates such a list a batch at a time and adds the batches'
-- counts up, which is exact because the entries of one scalar index are
-- disjoint and a batch never splits the values of one entry.  Every other
-- shape holds the list whole.  Each query below is answered with the
-- pushdown and with a sequential scan, and the two must agree.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
/* VACUUM can only set all-visible for commits that reached disk (citext.sql). */
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE lion_ll (k int NOT NULL, j int, x int NOT NULL, t text NOT NULL);
INSERT INTO lion_ll
SELECT i % 5000, CASE WHEN i % 11 = 0 THEN NULL ELSE i % 13 END, i % 3, 'v' || (i % 4000)
  FROM generate_series(1, 60000) i;
CREATE INDEX lion_ll_k ON lion_ll USING lion (k);
CREATE INDEX lion_ll_j ON lion_ll USING lion (j);
CREATE INDEX lion_ll_x ON lion_ll USING lion (x);
CREATE INDEX lion_ll_t ON lion_ll USING lion (t);
VACUUM ANALYZE lion_ll;

CREATE TABLE lion_llp (k int NOT NULL, x int NOT NULL) PARTITION BY RANGE (x);
CREATE TABLE lion_llp_0 PARTITION OF lion_llp FOR VALUES FROM (0) TO (2);
CREATE TABLE lion_llp_1 PARTITION OF lion_llp FOR VALUES FROM (2) TO (10);
INSERT INTO lion_llp SELECT i % 4000, i % 5 FROM generate_series(1, 40000) i;
CREATE INDEX lion_llp_k ON lion_llp USING lion (k);
VACUUM ANALYZE lion_llp;

/*
 * lion_llcmp() runs q with the pushdown (every other scan disabled) and with
 * a sequential scan, compares the answers as multisets, and says whether the
 * node took the list a batch at a time or whole.
 */
CREATE FUNCTION lion_llcmp(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	l text;
	pushed boolean := false;
	batches bigint := 0;
	a text;
	b text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ 'Custom Scan \(LionCount\)' THEN
			pushed := true;
		END IF;
		IF l ~ 'List Batches: ' THEN
			batches := batches + substring(l from 'List Batches: ([0-9]+)')::bigint;
		END IF;
	END LOOP;
	EXECUTE 'SELECT array_agg(r ORDER BY r)::text FROM (' || q || ') r' INTO a;
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	EXECUTE 'SELECT array_agg(r ORDER BY r)::text FROM (' || q || ') r' INTO b;
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH: %s against %s', left(a, 120), left(b, 120));
	END IF;
	RETURN CASE WHEN NOT pushed THEN 'not pushed down'
				WHEN batches > 1 THEN 'pushed down, in batches'
				ELSE 'pushed down, whole' END || ', same answer';
END $$;

-- a batch is a work_mem of located sets, never fewer than a thousand values
SET work_mem = '64kB';
-- the list alone (the disjoint sum, batch by batch), and with repeats that
-- straddle every batch boundary, and NULLs
SELECT lion_llcmp('SELECT count(*) FROM lion_ll WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g))');
SELECT lion_llcmp('SELECT count(*) FROM lion_ll WHERE k = ANY (array(SELECT g % 1500 FROM generate_series(0, 5999) g))');
SELECT lion_llcmp('SELECT count(*) FROM lion_ll WHERE k = ANY (array(SELECT (g * 7) % 2600 FROM generate_series(0, 4999) g))');
SELECT lion_llcmp('SELECT count(*) FROM lion_ll WHERE k = ANY (array(SELECT CASE WHEN g % 7 = 0 THEN NULL ELSE g END FROM generate_series(0, 3499) g))');
-- ANDed with another clause, a negated one, and another list
SELECT lion_llcmp('SELECT count(*) FROM lion_ll WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) AND x = 1');
SELECT lion_llcmp('SELECT count(*) FROM lion_ll WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) AND j IS NOT NULL');
SELECT lion_llcmp('SELECT count(*) FROM lion_ll WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) AND j = ANY (array(SELECT g FROM generate_series(1, 4) g))');
-- values with no entry: all of them, and all but a few
SELECT lion_llcmp('SELECT count(*) FROM lion_ll WHERE k = ANY (array(SELECT g FROM generate_series(100000, 103000) g))');
SELECT lion_llcmp('SELECT count(*) FROM lion_ll WHERE k = ANY (array(SELECT g FROM generate_series(4990, 7990) g))');
-- another element type than the key's, and text
SELECT lion_llcmp('SELECT count(*) FROM lion_ll WHERE k = ANY (array(SELECT g::int8 FROM generate_series(0, 2999) g))');
SELECT lion_llcmp($$SELECT count(*) FROM lion_ll WHERE t = ANY (array(SELECT 'v' || g FROM generate_series(0, 3999) g)) AND x = 2$$);
-- a partitioned table: each partition's list in batches of its own
SELECT lion_llcmp('SELECT count(*) FROM lion_llp WHERE k = ANY (array(SELECT g FROM generate_series(0, 2499) g))');
-- the shapes that count the list more than once hold it whole
SELECT lion_llcmp('SELECT x, count(*) FROM lion_ll WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) GROUP BY x');
SELECT lion_llcmp('SELECT count(DISTINCT x) FROM lion_ll WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g))');
SELECT lion_llcmp('SELECT count(*) FROM lion_ll WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g)) OR x = 1');
-- a dirty heap
UPDATE lion_ll SET x = (x + 1) % 3 WHERE k % 17 = 0;
DELETE FROM lion_ll WHERE k % 19 = 0;
SELECT lion_llcmp('SELECT count(*) FROM lion_ll WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g))');
SELECT lion_llcmp('SELECT count(*) FROM lion_ll WHERE k = ANY (array(SELECT g % 1500 FROM generate_series(0, 5999) g)) AND x = 1');
-- with the default work_mem the same list is one batch: located whole
RESET work_mem;
SELECT lion_llcmp('SELECT count(*) FROM lion_ll WHERE k = ANY (array(SELECT g FROM generate_series(0, 2999) g))');

DROP FUNCTION lion_llcmp(text);
DROP TABLE lion_ll;
DROP TABLE lion_llp;
