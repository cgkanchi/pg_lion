-- Whose the list pins are (2026-09-28 review; DESIGN.md §15, "The pin budget").
--
-- The leaves an IN list keeps pinned are charged to a budget per backend,
-- and a set abandoned by an error is never released.  Its pin is: the
-- resource owner it was taken under releases it with the subtransaction the
-- error ends.  The charge used to stay until the top-level transaction
-- ended, and every list after a failed subtransaction came out NOPIN - which
-- under an OR means the heap for every candidate.  The charges now go with
-- the owner's pins.  And the keys of a multi-key clause, which a parameter
-- can make a thousand per clause, are located under the same budget.  Every
-- answer below must still be exact.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
/*
 * A temporary table's buffer pool is temp_buffers, which can only be set
 * before the session touches one.  At 100 buffers the budget is an eighth of
 * them, 12 leaves, whatever the server's shared_buffers.  Keys of some 200
 * bytes put a few dozen entries on a leaf, and 2000 of them fill some fifty.
 */
\connect -
SET temp_buffers = 100;
SET synchronous_commit = on;
CREATE TEMP TABLE lion_po (k text NOT NULL, tags text[] NOT NULL, x int NOT NULL);
INSERT INTO lion_po
SELECT repeat(md5(i::text), 6) || i, ARRAY[repeat(md5(i::text), 6) || i, 't' || i % 7], i % 2
  FROM generate_series(1, 2000) i;
CREATE INDEX lion_po_k ON lion_po USING lion (k);
CREATE INDEX lion_po_tags ON lion_po USING lion (tags);
CREATE INDEX lion_po_x ON lion_po USING lion (x);
VACUUM ANALYZE lion_po;
SELECT pg_relation_size('lion_po_k') / current_setting('block_size')::int > 24 AS many_leaves;

/* Does the pushdown answer q? */
CREATE FUNCTION lion_po_pushed(q text) RETURNS boolean
LANGUAGE plpgsql AS $$
DECLARE
	l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Custom Scan \(LionCount\)' THEN
			RETURN true;
		END IF;
	END LOOP;
	RETURN false;
END $$;

/*
 * Where a count's answer came from: the visibility map, or the heap for
 * every candidate - which is what a union of sets located past the budget
 * has to do, with nothing left to carry the §9 interlock.
 */
CREATE FUNCTION lion_po_src(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	l text;
	pushed boolean := false;
	rechecked bigint := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ 'Custom Scan \(LionCount\)' THEN
			pushed := true;
		END IF;
		IF l ~ 'Heap TIDs Rechecked: ' THEN
			rechecked := rechecked + substring(l from 'Heap TIDs Rechecked: ([0-9]+)')::bigint;
		END IF;
	END LOOP;
	RETURN CASE WHEN NOT pushed THEN 'not pushed down'
				WHEN rechecked > 0 THEN 'rechecked in the heap'
				ELSE 'from the map' END;
END $$;

/* A count with the pushdown against the same with a sequential scan. */
CREATE FUNCTION lion_po_cmp(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	a text;
	b text;
	pushed boolean;
BEGIN
	pushed := lion_po_pushed(q);
	EXECUTE 'SELECT array_agg(r ORDER BY r)::text FROM (' || q || ') r' INTO a;
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	EXECUTE 'SELECT array_agg(r ORDER BY r)::text FROM (' || q || ') r' INTO b;
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH: %s against %s', left(a, 120), left(b, 120));
	END IF;
	RETURN CASE WHEN pushed THEN 'pushed down, ' ELSE 'NOT pushed down, ' END ||
		   'same answer';
END $$;

SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;

-- ---------- a failed subtransaction gives its charges back ----------
-- thirty neighbouring keys under an OR: a leaf or two, within the budget, so
-- every set of the union is pinned and the map answers
SELECT lion_po_src('SELECT count(*) FROM lion_po WHERE k = ANY ((SELECT array_agg(k) FROM (SELECT k FROM lion_po ORDER BY k LIMIT 30) s)::text[]) OR x = 1');
-- a count that locates every key - the whole budget - and fails at its first
-- group, while its sets are located
SELECT lion_po_pushed('SELECT x FROM lion_po WHERE k = ANY ((SELECT array_agg(k) FROM lion_po)::text[]) GROUP BY x HAVING 1 / (count(*) - 1000) > 0') AS pushed;
BEGIN;
SAVEPOINT lion_po_s;
SELECT x FROM lion_po WHERE k = ANY ((SELECT array_agg(k) FROM lion_po)::text[]) GROUP BY x HAVING 1 / (count(*) - 1000) > 0;
ROLLBACK TO SAVEPOINT lion_po_s;
-- the same transaction: the budget is whole again, and the map answers
SELECT lion_po_src('SELECT count(*) FROM lion_po WHERE k = ANY ((SELECT array_agg(k) FROM (SELECT k FROM lion_po ORDER BY k LIMIT 30) s)::text[]) OR x = 1');
COMMIT;
-- ... and a PL/pgSQL EXCEPTION block that fails like that in a loop
CREATE FUNCTION lion_po_fail(n int) RETURNS int
LANGUAGE plpgsql AS $$
DECLARE
	failed int := 0;
BEGIN
	FOR i IN 1 .. n LOOP
		BEGIN
			PERFORM x FROM lion_po
			  WHERE k = ANY ((SELECT array_agg(k) FROM lion_po)::text[])
			  GROUP BY x HAVING 1 / (count(*) - 1000) > 0;
		EXCEPTION WHEN division_by_zero THEN
			failed := failed + 1;
		END;
	END LOOP;
	RETURN failed;
END $$;
BEGIN;
SELECT lion_po_fail(5) AS failed;
SELECT lion_po_src('SELECT count(*) FROM lion_po WHERE k = ANY ((SELECT array_agg(k) FROM (SELECT k FROM lion_po ORDER BY k LIMIT 30) s)::text[]) OR x = 1');
SELECT lion_po_cmp('SELECT count(*) FROM lion_po WHERE k = ANY ((SELECT array_agg(k) FROM (SELECT k FROM lion_po ORDER BY k LIMIT 30) s)::text[]) OR x = 1');
COMMIT;
-- a cursor opened outside a subtransaction and fetched inside it keeps its
-- sets, and their charges, when the subtransaction is rolled back
BEGIN;
DECLARE lion_po_c CURSOR FOR
	SELECT x, count(*) FROM lion_po
	 WHERE k = ANY ((SELECT array_agg(k) FROM lion_po)::text[]) GROUP BY x ORDER BY x;
SAVEPOINT lion_po_s;
FETCH 1 FROM lion_po_c;
ROLLBACK TO SAVEPOINT lion_po_s;
FETCH 1 FROM lion_po_c;
CLOSE lion_po_c;
SELECT lion_po_src('SELECT count(*) FROM lion_po WHERE k = ANY ((SELECT array_agg(k) FROM (SELECT k FROM lion_po ORDER BY k LIMIT 30) s)::text[]) OR x = 1');
COMMIT;

-- ---------- a multi-key clause's keys, under the budget ----------
-- a thousand keys of a parameter over some fifty leaves: the sets past the
-- first twelve leaves come out NOPIN, and every shape is still exact
SELECT lion_po_cmp('SELECT count(*) FROM lion_po WHERE tags && (SELECT array_agg(k) FROM lion_po WHERE x = 0)');
SELECT lion_po_cmp($$SELECT count(*) FROM lion_po WHERE tags && (SELECT array_agg(k) FROM lion_po WHERE right(k, 1) IN ('1', '2', '3')) AND x = 1$$);
SELECT lion_po_cmp($$SELECT x, count(*) FROM lion_po WHERE tags && (SELECT array_agg(k) FROM lion_po WHERE right(k, 1) IN ('1', '2', '3')) GROUP BY x$$);
SELECT lion_po_cmp($$SELECT count(*) FROM lion_po WHERE tags && (SELECT array_agg(k) FROM lion_po WHERE x = 0) AND tags && (SELECT array_agg(k) FROM lion_po WHERE right(k, 1) IN ('2', '4'))$$);
-- a single key found once the list beside it has spent the budget
SELECT lion_po_cmp($$SELECT count(*) FROM lion_po WHERE k = ANY ((SELECT array_agg(k) FROM lion_po)::text[]) AND tags @> ARRAY['t3']$$);
SELECT lion_po_cmp($$SELECT count(*) FROM lion_po WHERE tags && (SELECT array_agg(k) FROM lion_po) AND tags @> ARRAY['t3']$$);
-- ... and on a dirty heap
UPDATE lion_po SET x = 1 - x WHERE right(k, 1) = '7';
DELETE FROM lion_po WHERE right(k, 1) = '9';
SELECT lion_po_cmp('SELECT count(*) FROM lion_po WHERE tags && (SELECT array_agg(k) FROM lion_po WHERE x = 0)');
SELECT lion_po_cmp($$SELECT x, count(*) FROM lion_po WHERE tags && (SELECT array_agg(k) FROM lion_po WHERE right(k, 1) IN ('1', '7')) GROUP BY x$$);
SELECT lion_po_cmp($$SELECT count(*) FROM lion_po WHERE tags && (SELECT array_agg(k) FROM lion_po WHERE right(k, 1) IN ('1', '7')) AND x = 1$$);

RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
DROP FUNCTION lion_po_fail(int);
DROP FUNCTION lion_po_cmp(text);
DROP FUNCTION lion_po_src(text);
DROP FUNCTION lion_po_pushed(text);
DROP TABLE lion_po;
