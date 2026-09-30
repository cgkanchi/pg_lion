-- A buffer pool of sixteen buffers (DESIGN.md §15, "The pin budget";
-- 2026-09-29 review).  test/hook-check.sh starts the server with
-- shared_buffers = 128kB for this file alone: installcheck's server has a
-- pool of gigabytes, where no budget of pg_lion's comes near the pool.
--
-- Each pin budget of a count - the leaves an IN list keeps, the pages its
-- cursors pin, the groups a GROUP BY counts together - had a floor of 16 or
-- 64 pins whatever the pool, and the list budget itself came to sixteen fair
-- shares of the pool.  On sixteen buffers the queries below failed with "no
-- unpinned buffers available", where the plan without the pushdown answers.
-- Every budget is now capped at a few fair shares, floors included - which on
-- this pool is none at all - and what lies past it is read without its pins.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
/* VACUUM can only set all-visible for commits that reached disk (citext.sql). */
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;
SHOW shared_buffers;

/* CHAIN entries (inline_limit = 64): a cursor pins the page it reads. */
CREATE TABLE lion_pp (a int NOT NULL, b int NOT NULL, g int NOT NULL, h int NOT NULL);
INSERT INTO lion_pp SELECT i % 100, i % 7, i % 50, i % 1000 FROM generate_series(1, 20000) i;
CREATE INDEX lion_pp_a ON lion_pp USING lion (a) WITH (inline_limit = 64);
CREATE INDEX lion_pp_b ON lion_pp USING lion (b) WITH (inline_limit = 64);
CREATE INDEX lion_pp_g ON lion_pp USING lion (g) WITH (inline_limit = 64);
CREATE INDEX lion_pp_h ON lion_pp USING lion (h) WITH (inline_limit = 64);
VACUUM (FREEZE, ANALYZE) lion_pp;

/*
 * lion_ppcmp() runs q with the count pushdown (every other scan disabled) and
 * again without it, and reports whether the pushdown answered it and whether
 * the two agree as multisets.
 */
CREATE FUNCTION lion_ppcmp(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	l text;
	pushed boolean := false;
	a text;
	b text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Custom Scan \(LionCount\)' THEN
			pushed := true;
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
	RETURN CASE WHEN pushed THEN 'pushed down' ELSE 'not pushed down' END ||
		', same answer';
END $$;

-- a list of CHAIN entries ANDed with another clause: a batch of its cursors
SELECT lion_ppcmp('SELECT count(*) FROM lion_pp WHERE a IN (1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30) AND b = 1');
SELECT lion_ppcmp('SELECT count(*) FROM lion_pp WHERE a = ANY (array(SELECT generate_series(1, 50))) AND b = 1');
-- ... under an OR, and alone
SELECT lion_ppcmp('SELECT count(*) FROM lion_pp WHERE a IN (1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30) OR b = 1');
SELECT lion_ppcmp('SELECT count(*) FROM lion_pp WHERE a = ANY (array(SELECT generate_series(1, 50)))');
-- a GROUP BY: the groups a walk counts together, each pinning its page
SELECT lion_ppcmp('SELECT g, count(*) FROM lion_pp WHERE h IN (1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30) GROUP BY g');
SELECT lion_ppcmp('SELECT a, count(*) FROM lion_pp WHERE b IN (1, 2) GROUP BY a');
SELECT lion_ppcmp('SELECT count(DISTINCT a) FROM lion_pp WHERE b = 1');
-- the SQL function
SELECT lion_index_count_any('lion_pp_a', array(SELECT generate_series(1, 50))) AS count_any,
	   (SELECT count(*) FROM lion_pp WHERE a BETWEEN 1 AND 50) AS expected;

DROP FUNCTION lion_ppcmp(text);
DROP TABLE lion_pp;
