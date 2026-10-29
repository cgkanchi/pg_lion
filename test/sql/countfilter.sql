-- Filtered counts (DESIGN.md §10, "Filtered counts").
--
-- count(*) FILTER (WHERE c), count(CASE WHEN c THEN 1 END),
-- sum(CASE WHEN c THEN 1 ELSE 0 END) and sum(CASE WHEN c THEN 1 END) are the
-- rows of the WHERE - or of the group - for which c is true, each reported
-- as its aggregate reports it: a count 0 over no row, a sum NULL over no row
-- at all, and the sum without ELSE NULL where no row has c true.  The node
-- counts the WHERE alone and then the WHERE with each condition, as counts
-- of their own; a GROUP BY's groups come out once per condition, as partial
-- rows a Finalize Agg adds up.
--
-- Every answer is checked against a SEQUENTIAL SCAN, on a table with NULLs
-- and with rows deleted and updated since it was last vacuumed.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET max_parallel_workers_per_gather = 0;

/*
 * lion_cf() runs q with the LionCount pushdown and every other scan
 * disabled, then as a sequential scan with the pushdown off, and compares the
 * two, row for row.  It says whether the node answered q, and how many rows q
 * has.
 */
CREATE FUNCTION lion_cf(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	used boolean := false;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(LionCount\)' THEN
			used := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lion_cf_on AS SELECT s::text AS r FROM (%s) s', q);

	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE format('CREATE TEMP TABLE lion_cf_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM lion_cf_off' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_cf_on EXCEPT ALL SELECT * FROM lion_cf_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_cf_off EXCEPT ALL SELECT * FROM lion_cf_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lion_cf_on, lion_cf_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows', CASE WHEN used THEN 'pushdown' ELSE 'no pushdown' END,
				  nrows);
END $$;

/* EXPLAIN (COSTS OFF) of q with every other scan disabled */
CREATE FUNCTION lion_cf_plan(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		RETURN NEXT ln;
	END LOOP;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'on', true);
END $$;

/*
 * a, b and f have NULLs; g is grouped by and never NULL; r is ranged over;
 * tags is a multi-key column.  Every column has a lion index but n.
 */
CREATE TABLE lion_cft (
	id int NOT NULL,
	g int NOT NULL,
	a int,
	b int,
	c int,
	r int,
	f boolean,
	t text,
	tags int[],
	n int
);
INSERT INTO lion_cft
SELECT i, i % 6,
	   CASE WHEN i % 11 = 0 THEN NULL ELSE i % 5 END,
	   CASE WHEN i % 7 = 0 THEN NULL ELSE i % 3 END,
	   i % 13,
	   i % 100,
	   CASE WHEN i % 9 = 0 THEN NULL ELSE i % 2 = 0 END,
	   'v' || (i % 4),
	   ARRAY[i % 10, 10 + i % 7],
	   i % 3
FROM generate_series(1, 30000) i;
CREATE INDEX lion_cft_g ON lion_cft USING lion (g);
CREATE INDEX lion_cft_a ON lion_cft USING lion (a);
CREATE INDEX lion_cft_b ON lion_cft USING lion (b);
CREATE INDEX lion_cft_c ON lion_cft USING lion (c);
CREATE INDEX lion_cft_r ON lion_cft USING lion (r);
CREATE INDEX lion_cft_f ON lion_cft USING lion (f);
CREATE INDEX lion_cft_t ON lion_cft USING lion (t);
CREATE INDEX lion_cft_tags ON lion_cft USING lion (tags);
VACUUM ANALYZE lion_cft;

-- rows deleted, and updated into other groups and filters, not vacuumed
DELETE FROM lion_cft WHERE id % 10 = 3;
UPDATE lion_cft SET b = 2, a = NULL WHERE id % 10 = 4;
UPDATE lion_cft SET g = 5, c = 1 WHERE id % 17 = 0;

-- ---------- 1. without a GROUP BY ----------

SELECT lion_cf('SELECT count(*) FILTER (WHERE b = 1), count(*) FILTER (WHERE c = 3), count(*) FROM lion_cft');
SELECT lion_cf('SELECT count(*) FILTER (WHERE b = 1), count(*) FROM lion_cft WHERE a = 2');
SELECT lion_cf('SELECT sum(CASE WHEN b = 1 THEN 1 ELSE 0 END), sum(CASE WHEN b = 1 THEN 1 END), count(CASE WHEN b = 1 THEN 1 END), sum(1) FILTER (WHERE b = 1) FROM lion_cft WHERE a = 2');
-- the WHERE selects no row: a count 0, a sum NULL
SELECT lion_cf('SELECT count(*) FILTER (WHERE b = 1), sum(CASE WHEN b = 1 THEN 1 ELSE 0 END), sum(CASE WHEN b = 1 THEN 1 END), count(*) FROM lion_cft WHERE a = 99');
-- ... and rows, none with the condition true: a sum without ELSE NULL
SELECT lion_cf('SELECT count(*) FILTER (WHERE b = 7), sum(CASE WHEN b = 7 THEN 1 ELSE 0 END), sum(CASE WHEN b = 7 THEN 1 END) FROM lion_cft WHERE a = 2');
-- a condition NULL in some rows is not true there
SELECT lion_cf('SELECT count(*) FILTER (WHERE f), sum(CASE WHEN f THEN 1 ELSE 0 END), count(*) FILTER (WHERE f IS NOT TRUE), count(*) FILTER (WHERE NOT f) FROM lion_cft');
SELECT lion_cf('SELECT count(*) FILTER (WHERE a IS NULL), count(*) FILTER (WHERE a IS NOT NULL), count(*) FILTER (WHERE b <> 1) FROM lion_cft WHERE c = 4');
-- lists, ORs, NOT IN, ANDs of clauses, ranges and a multi-key clause
SELECT lion_cf('SELECT count(*) FILTER (WHERE b IN (0, 2)), count(*) FILTER (WHERE a = 1 OR b = 0), count(*) FILTER (WHERE c NOT IN (1, 2, 3)) FROM lion_cft');
SELECT lion_cf('SELECT count(*) FILTER (WHERE a = 1 AND b = 0), count(*) FILTER (WHERE t = ''v2'' AND c <> 5) FROM lion_cft WHERE g = 3');
SELECT lion_cf('SELECT count(*) FILTER (WHERE r < 10), count(*) FILTER (WHERE r >= 40 AND r < 45), count(*) FROM lion_cft WHERE a = 3');
SELECT lion_cf('SELECT count(*) FILTER (WHERE r BETWEEN 5 AND 9 OR b = 2) FROM lion_cft WHERE a = 3');
SELECT lion_cf('SELECT count(*) FILTER (WHERE tags @> ARRAY[3]), count(*) FILTER (WHERE tags && ARRAY[11, 12]) FROM lion_cft WHERE g = 1');
-- a range in the WHERE beside a filter's own
SELECT lion_cf('SELECT count(*) FILTER (WHERE r < 30), count(*) FILTER (WHERE b = 1) FROM lion_cft WHERE r >= 20 AND r < 60');
-- a count(col) of a column every row it counts has
SELECT lion_cf('SELECT count(id) FILTER (WHERE b = 1), count(c) FILTER (WHERE c = 4), count(a) FILTER (WHERE a = 2) FROM lion_cft');
-- one condition in several forms is one count
SELECT lion_cf('SELECT count(*) FILTER (WHERE b = 1), sum(CASE WHEN b = 1 THEN 1 ELSE 0 END), count(*) FILTER (WHERE b = 1) * 2 FROM lion_cft');
-- a GROUP BY the planner folded to its one group
SELECT lion_cf('SELECT g, count(*) FILTER (WHERE b = 1), sum(CASE WHEN c = 2 THEN 1 ELSE 0 END) FROM lion_cft WHERE g = 2 GROUP BY g');
SELECT lion_cf('SELECT g, count(*) FILTER (WHERE b = 1) FROM lion_cft WHERE g = 2 AND a = 99 GROUP BY g');
-- HAVING over a filtered count
SELECT lion_cf('SELECT count(*) FILTER (WHERE b = 1) FROM lion_cft WHERE a = 2 HAVING count(*) FILTER (WHERE b = 1) > 0');

-- ---------- 2. GROUP BY ----------

SELECT lion_cf('SELECT g, sum(CASE WHEN b = 1 THEN 1 ELSE 0 END), count(*) FROM lion_cft GROUP BY g');
SELECT lion_cf('SELECT a, count(*) FILTER (WHERE b = 1), count(*) FILTER (WHERE c = 3), sum(CASE WHEN f THEN 1 END), count(*), count(a) FROM lion_cft GROUP BY a');
SELECT lion_cf('SELECT g, count(*) FILTER (WHERE b IS NULL), sum(CASE WHEN t IN (''v1'', ''v3'') THEN 1 ELSE 0 END) FROM lion_cft WHERE c <> 0 GROUP BY g');
-- a group none of whose rows has the condition true
SELECT lion_cf('SELECT g, sum(CASE WHEN c = 1 THEN 1 ELSE 0 END), sum(CASE WHEN c = 1 THEN 1 END), count(*) FILTER (WHERE c = 1) FROM lion_cft WHERE g IN (0, 5) GROUP BY g');
SELECT lion_cf('SELECT g, count(*) FILTER (WHERE r < 3) FROM lion_cft WHERE a = 1 GROUP BY g HAVING count(*) FILTER (WHERE r < 3) > 2');
SELECT lion_cf('SELECT g, count(*) FILTER (WHERE b = 1) FROM lion_cft GROUP BY g ORDER BY 2 DESC, g');
-- two columns, the nested loop of their entries
SELECT lion_cf('SELECT g, b, count(*) FILTER (WHERE c = 3), count(*) FROM lion_cft GROUP BY g, b');
-- three columns: the decoded walk
SELECT lion_cf('SELECT g, a, b, sum(CASE WHEN c = 3 THEN 1 ELSE 0 END), count(*) FROM lion_cft GROUP BY g, a, b');
SELECT lion_cf('SELECT g, a, b, count(*) FILTER (WHERE c = 3), sum(CASE WHEN t = ''v1'' THEN 1 END) FROM lion_cft WHERE f GROUP BY g, a, b');

-- ---------- 3. what is left to the ordinary plan ----------

-- a condition on a column without an index makes the whole query core's
SELECT lion_cf('SELECT count(*) FILTER (WHERE n = 1), count(*) FILTER (WHERE b = 1) FROM lion_cft');
-- ... and so does one no posting set answers, or a constant one
SELECT lion_cf('SELECT count(*) FILTER (WHERE b + 1 = 2) FROM lion_cft');
SELECT lion_cf('SELECT count(*) FILTER (WHERE true), count(*) FROM lion_cft WHERE a = 1');
-- a count(col) of a column the rows may have NULL
SELECT lion_cf('SELECT count(b) FILTER (WHERE c = 4) FROM lion_cft');
-- count(DISTINCT) beside a filtered count, and a filtered count(DISTINCT)
SELECT lion_cf('SELECT count(DISTINCT c), count(*) FILTER (WHERE b = 1) FROM lion_cft');
SELECT lion_cf('SELECT count(DISTINCT c) FILTER (WHERE b = 1) FROM lion_cft');
-- other constants, and a CASE with a FILTER
SELECT lion_cf('SELECT sum(CASE WHEN b = 1 THEN 2 ELSE 0 END) FROM lion_cft WHERE a = 1');
SELECT lion_cf('SELECT sum(CASE WHEN b = 1 THEN 1 ELSE 0 END) FILTER (WHERE c = 1) FROM lion_cft');

-- ---------- 4. the plans ----------

SELECT lion_cf_plan('SELECT count(*) FILTER (WHERE b = 1), count(*) FILTER (WHERE c = 3), count(*) FROM lion_cft WHERE a = 2');
SELECT lion_cf_plan('SELECT g, sum(CASE WHEN b = 1 THEN 1 ELSE 0 END), count(*) FROM lion_cft GROUP BY g');
SELECT lion_cf_plan('SELECT g, a, b, count(*) FILTER (WHERE c = 3 OR t = ''v1'') FROM lion_cft GROUP BY g, a, b');
SELECT lion_cf_plan('SELECT count(*) FILTER (WHERE r < 10 AND r > 2) FROM lion_cft WHERE a = 3');

-- ---------- 5. a parameter, a cursor ----------

SET plan_cache_mode = force_generic_plan;
PREPARE lion_cfp(int, int) AS
	SELECT g, count(*) FILTER (WHERE b = $1), sum(CASE WHEN c = $2 THEN 1 ELSE 0 END)
	FROM lion_cft GROUP BY g;
-- a multi-key condition whose value only the run knows: with '{}' it narrows
-- nothing, every row's array containing the empty one and none overlapping it
PREPARE lion_cfq(int[]) AS
	SELECT g, a, b, count(*) FILTER (WHERE tags @> $1) AS has, sum(CASE WHEN tags && $1 THEN 1 ELSE 0 END) AS meets, count(*) AS n
	FROM lion_cft WHERE c = 2 GROUP BY g, a, b;
BEGIN;
SET LOCAL enable_seqscan = off;
SET LOCAL enable_bitmapscan = off;
SET LOCAL pg_lion.enable_plain_scan = off;
EXPLAIN (COSTS OFF) EXECUTE lion_cfp(1, 3);
CREATE TEMP TABLE lion_cfr1 AS EXECUTE lion_cfp(1, 3);
CREATE TEMP TABLE lion_cfr2 AS EXECUTE lion_cfp(2, 12);
EXPLAIN (COSTS OFF) EXECUTE lion_cfq('{}');
CREATE TEMP TABLE lion_cfr3 AS EXECUTE lion_cfq('{}');
CREATE TEMP TABLE lion_cfr4 AS EXECUTE lion_cfq('{4, 12}');
COMMIT;
SET pg_lion.enable_count_pushdown = off;
CREATE TEMP TABLE lion_cfs1 AS EXECUTE lion_cfp(1, 3);
CREATE TEMP TABLE lion_cfs2 AS EXECUTE lion_cfp(2, 12);
CREATE TEMP TABLE lion_cfs3 AS EXECUTE lion_cfq('{}');
CREATE TEMP TABLE lion_cfs4 AS EXECUTE lion_cfq('{4, 12}');
RESET pg_lion.enable_count_pushdown;
SELECT * FROM lion_cfr1 ORDER BY g;
SELECT * FROM lion_cfr2 ORDER BY g;
-- ... both as the sequential scan has them
(SELECT * FROM lion_cfr1 EXCEPT SELECT * FROM lion_cfs1)
UNION ALL (SELECT * FROM lion_cfs1 EXCEPT SELECT * FROM lion_cfr1)
UNION ALL (SELECT * FROM lion_cfr2 EXCEPT SELECT * FROM lion_cfs2)
UNION ALL (SELECT * FROM lion_cfs2 EXCEPT SELECT * FROM lion_cfr2);
SELECT count(*) AS rows, sum(has) AS has, sum(meets) AS meets, sum(n) AS n FROM lion_cfr3;
SELECT count(*) AS rows, sum(has) AS has, sum(meets) AS meets, sum(n) AS n FROM lion_cfr4;
SELECT count(*) AS differ FROM (
	(SELECT * FROM lion_cfr3 EXCEPT SELECT * FROM lion_cfs3)
	UNION ALL (SELECT * FROM lion_cfs3 EXCEPT SELECT * FROM lion_cfr3)
	UNION ALL (SELECT * FROM lion_cfr4 EXCEPT SELECT * FROM lion_cfs4)
	UNION ALL (SELECT * FROM lion_cfs4 EXCEPT SELECT * FROM lion_cfr4)) d;
DROP TABLE lion_cfr1, lion_cfr2, lion_cfr3, lion_cfr4, lion_cfs1, lion_cfs2, lion_cfs3, lion_cfs4;
DEALLOCATE lion_cfp;
DEALLOCATE lion_cfq;
RESET plan_cache_mode;

/*
 * A cursor read a row at a time, with rows deleted between its rows: what it
 * reads is what its snapshot sees - the sequential scan's answer from before
 * the delete - though the node pauses between the rows of one run and the
 * next, and locates the next run's sets after the delete.
 */
CREATE FUNCTION lion_cf_cursor(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	cur refcursor;
	rec record;
	got text[] := '{}';
	want text;
	n int := 0;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE format('SELECT string_agg(s::text, '' '' ORDER BY s::text) FROM (%s) s', q)
		INTO want;
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('pg_lion.enable_plain_scan', 'off', true);
	OPEN cur FOR EXECUTE q;
	LOOP
		FETCH cur INTO rec;
		EXIT WHEN NOT FOUND;
		got := got || rec::text;
		n := n + 1;
		IF n = 3 THEN
			DELETE FROM lion_cft WHERE id % 10 = 5;
		END IF;
	END LOOP;
	CLOSE cur;
	IF (SELECT string_agg(x, ' ' ORDER BY x) FROM unnest(got) x) IS DISTINCT FROM want THEN
		RETURN 'MISMATCH';
	END IF;
	RETURN format('%s rows, the snapshot''s', n);
END $$;

BEGIN;
SELECT lion_cf_cursor('SELECT g, count(*) FILTER (WHERE a = 1), count(*) FILTER (WHERE b = 2), count(*) FROM lion_cft WHERE c <> 7 GROUP BY g');
SELECT lion_cf_cursor('SELECT g, b, sum(CASE WHEN a = 1 THEN 1 END), count(*) FROM lion_cft GROUP BY g, b');
ROLLBACK;

-- ---------- 6. a partitioned table ----------

CREATE TABLE lion_cfpt (g int NOT NULL, a int, b int) PARTITION BY LIST (g);
CREATE TABLE lion_cfpt0 PARTITION OF lion_cfpt FOR VALUES IN (0, 1, 2);
CREATE TABLE lion_cfpt1 PARTITION OF lion_cfpt FOR VALUES IN (3, 4, 5);
INSERT INTO lion_cfpt SELECT i % 6, CASE WHEN i % 13 = 0 THEN NULL ELSE i % 4 END, i % 5
FROM generate_series(1, 12000) i;
CREATE INDEX lion_cfpt_g ON lion_cfpt USING lion (g);
CREATE INDEX lion_cfpt_a ON lion_cfpt USING lion (a);
CREATE INDEX lion_cfpt_b ON lion_cfpt USING lion (b);
VACUUM ANALYZE lion_cfpt;
DELETE FROM lion_cfpt WHERE b = 4 AND a = 1;
UPDATE lion_cfpt SET a = 0 WHERE b = 3 AND a = 2;

SELECT lion_cf('SELECT count(*) FILTER (WHERE a = 1), sum(CASE WHEN b = 2 THEN 1 END), count(*) FROM lion_cfpt');
-- a WHERE the one partition left implies, beside a clause it does not: on a
-- table this small one pass over the rows is the cheaper plan, unless the
-- margin says otherwise (DESIGN.md §39).  The bitmap scans and the AM's plain
-- scans are off, so that the pass is what the count competes with: the
-- margin does not apply against the AM's own scans, nor against a plan
-- every scan of which is disabled.
SET enable_bitmapscan = off;
SET pg_lion.enable_plain_scan = off;
SET pg_lion.pushdown_margin = 4;
EXPLAIN (COSTS OFF)
SELECT count(*) FILTER (WHERE b = 1), count(*) FROM lion_cfpt WHERE g IN (0, 1, 2) AND a = 1;
SELECT count(*) FILTER (WHERE b = 1), count(*) FILTER (WHERE b IN (2, 3)), count(*) FROM lion_cfpt WHERE g IN (0, 1, 2) AND a = 1;
RESET pg_lion.pushdown_margin;
RESET pg_lion.enable_plain_scan;
RESET enable_bitmapscan;
-- the same counts without the node
SET pg_lion.enable_count_pushdown = off;
SELECT count(*) FILTER (WHERE b = 1), count(*) FILTER (WHERE b IN (2, 3)), count(*) FROM lion_cfpt WHERE g IN (0, 1, 2) AND a = 1;
RESET pg_lion.enable_count_pushdown;
SELECT lion_cf('SELECT g, count(*) FILTER (WHERE a = 1), sum(CASE WHEN b = 2 THEN 1 ELSE 0 END), count(*) FROM lion_cfpt GROUP BY g');
SELECT lion_cf('SELECT a, count(*) FILTER (WHERE b IN (1, 2)) FROM lion_cfpt WHERE g <> 4 GROUP BY a');

DROP TABLE lion_cfpt;
DROP TABLE lion_cft;
DROP FUNCTION lion_cf(text);
DROP FUNCTION lion_cf_plan(text);
DROP FUNCTION lion_cf_cursor(text);
