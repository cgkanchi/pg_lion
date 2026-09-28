-- The two-column GROUP BY of DESIGN.md §20 when the inner index's keys do not
-- fit work_mem (lion_load_inner_keys()): the inner index's entry scan is then
-- walked once per outer group instead.  That walk outlives every pair of its
-- outer group, and it used to be begun inside the per-pair memory context,
-- whose reset before the next pair freed the walk's position and batch under
-- it - a leaf copied into freed memory, and the contexts deleted twice at the
-- end (the 2026-09-27 review).  b's keys are some 300 bytes wide and 700 of
-- them, so at work_mem = 64kB the first 256 already outgrow the budget.  Each
-- query is answered through the pushdown, with every other scan disabled, and
-- through a sequential scan with the pushdown off, and the two answers must be
-- equal as multisets.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

/*
 * lion_g2() runs q through the pushdown with every other scan disabled, then
 * as a sequential scan with the pushdown off, and says whether the node ran
 * and how many rows came out - after proving the two answers equal.
 */
CREATE FUNCTION lion_g2(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	pushed boolean := false;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			pushed := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lion_g2_on AS %s', q);

	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	EXECUTE format('CREATE TEMP TABLE lion_g2_off AS %s', q);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM lion_g2_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_g2_on EXCEPT ALL SELECT * FROM lion_g2_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_g2_off EXCEPT ALL SELECT * FROM lion_g2_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lion_g2_on, lion_g2_off';

	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN pushed THEN 'pushed down' ELSE 'not pushed down' END,
				  nrows);
END $$;

/*
 * a has 3 values and drives the loop (the column with fewer groups is the
 * outer one); b has 700, each pair (a, b) is two rows and there are 2100 of
 * them.  c = 1 selects the pairs whose b ends in 1 or 6: 420.
 */
CREATE TABLE lion_g2 (a int NOT NULL, b text NOT NULL, c int NOT NULL);
INSERT INTO lion_g2
SELECT i % 3, repeat('x', 300) || (i % 700), i % 5
  FROM generate_series(1, 4200) i;
CREATE INDEX lion_g2_a ON lion_g2 USING lion (a);
CREATE INDEX lion_g2_b ON lion_g2 USING lion (b);
CREATE INDEX lion_g2_c ON lion_g2 USING lion (c);
VACUUM ANALYZE lion_g2;

SET work_mem = '64kB';
SELECT lion_g2('SELECT a, b, count(*) FROM lion_g2 GROUP BY a, b');
SELECT lion_g2('SELECT b, a, count(*) FROM lion_g2 GROUP BY b, a');
SELECT lion_g2('SELECT a, b, count(*) FROM lion_g2 WHERE c = 1 GROUP BY a, b');
SELECT lion_g2('SELECT a, b, count(b) FROM lion_g2 WHERE c IN (1, 2) GROUP BY a, b');

-- ... and each partition of a partitioned table walks its own inner index so
-- (DESIGN.md §16): the same 2100 pairs, once in each partition
CREATE TABLE lion_g2p (a int NOT NULL, b text NOT NULL, k int NOT NULL)
	PARTITION BY LIST (k);
CREATE TABLE lion_g2p_0 PARTITION OF lion_g2p FOR VALUES IN (0);
CREATE TABLE lion_g2p_1 PARTITION OF lion_g2p FOR VALUES IN (1);
INSERT INTO lion_g2p
SELECT i % 3, repeat('x', 300) || (i % 700), i / 2100
  FROM generate_series(0, 4199) i;
CREATE INDEX lion_g2p_a ON lion_g2p USING lion (a);
CREATE INDEX lion_g2p_b ON lion_g2p USING lion (b);
VACUUM ANALYZE lion_g2p;
SELECT lion_g2('SELECT a, b, count(*) FROM lion_g2p GROUP BY a, b');
RESET work_mem;

DROP TABLE lion_g2, lion_g2p;
DROP FUNCTION lion_g2(text);
