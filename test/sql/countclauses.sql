-- Clause shapes the count pushdown recognises (DESIGN.md §10).  Each of these
-- was once declined before any cost was asked, so the LionCount node never
-- saw some of the commonest filter shapes:
--
--	- an enum key, `status = 'val1'` or `status IN ('val1', 'val3')`;
--
-- Every query here runs with the other scans disabled, so that a LionCount
-- path that is built at all is the plan whatever the cost model says, and its
-- rows are compared with the same query's under
-- pg_lion.enable_count_pushdown = off, as a multiset both ways round.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

/*
 * lion_cc() runs a query with every other scan and join method disabled,
 * notes whether the plan is the LionCount node, and compares its rows with
 * the ordinary plan's (the pushdown off, the planner left alone).
 */
CREATE FUNCTION lion_cc(q text) RETURNS text
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
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			pushed := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lion_cc_on AS SELECT s::text AS r FROM (%s) s', q);

	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_nestloop', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE format('CREATE TEMP TABLE lion_cc_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM lion_cc_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_cc_on EXCEPT ALL SELECT * FROM lion_cc_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_cc_off EXCEPT ALL SELECT * FROM lion_cc_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lion_cc_on, lion_cc_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN pushed THEN 'LionCount' ELSE 'not pushed' END, nrows);
END $$;

/*
 * lion_cc_scan() runs a query as a plain INDEX scan or a BITMAP scan over a
 * lion index - the other scans and the count pushdown off - and again as a
 * sequential scan, and compares the two: core matches these clauses for an
 * index scan itself, and the access method has to agree with what it is
 * handed.
 */
CREATE FUNCTION lion_cc_scan(how text, q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	used text := NULL;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('enable_indexscan', (how = 'index')::text, true);
	PERFORM set_config('enable_bitmapscan', (how = 'bitmap')::text, true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ '(Index Scan using|Bitmap Index Scan on) lion_cc' AND used IS NULL THEN
			used := substring(ln from '((Index Scan using|Bitmap Index Scan on) \S+)');
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lion_cc_on AS SELECT s::text AS r FROM (%s) s', q);

	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	EXECUTE format('CREATE TEMP TABLE lion_cc_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM lion_cc_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_cc_on EXCEPT ALL SELECT * FROM lion_cc_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_cc_off EXCEPT ALL SELECT * FROM lion_cc_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lion_cc_on, lion_cc_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows', coalesce(used, 'no lion scan'), nrows);
END $$;

/*
 * The table.  status is a nullable enum and dstatus the same values through a
 * domain over it; flag is a nullable boolean; pc has a different number of
 * rows for every value (2k + 1 of every 400 for pc = k), so that a count says
 * which value it was made for; d and ts lie in the recent past, relative to
 * the time the table is built, so that a window over current_date or now()
 * selects some of them and not all.
 */
CREATE TYPE lion_cc_ws AS ENUM ('val1', 'val2', 'val3', 'val4');
CREATE TYPE lion_cc_other AS ENUM ('val1', 'val2', 'val3', 'val4');
CREATE DOMAIN lion_cc_wsd AS lion_cc_ws;
CREATE TABLE lion_cc_t (
	id		int NOT NULL,
	status	lion_cc_ws,
	dstatus	lion_cc_wsd,
	flag	bool,
	pc		int NOT NULL,
	d		date NOT NULL,
	ts		timestamptz NOT NULL,
	country	text NOT NULL
);
INSERT INTO lion_cc_t
SELECT g,
	   CASE WHEN g % 50 = 0 THEN NULL
			ELSE (ARRAY['val1', 'val2', 'val3']::lion_cc_ws[])[1 + (g % 7) % 3] END,
	   CASE WHEN g % 50 = 0 THEN NULL
			ELSE (ARRAY['val1', 'val2', 'val3']::lion_cc_ws[])[1 + (g % 7) % 3] END,
	   CASE WHEN g % 23 = 0 THEN NULL ELSE g % 5 <> 0 END,
	   floor(sqrt(g % 400))::int,
	   current_date - (g % 90),
	   now() - (g % 2000) * interval '1 hour',
	   'c' || (g % 6)
  FROM generate_series(1, 12000) g;
CREATE TABLE lion_cc_dim (status lion_cc_ws PRIMARY KEY, label text NOT NULL);
INSERT INTO lion_cc_dim VALUES ('val1', 'up'), ('val2', 'idle'), ('val3', 'down'),
	('val4', 'up');

/*
 * The queries, run twice below: once over one single-column index per column
 * and once over a single multicolumn index (DESIGN.md §24).
 */
CREATE TABLE lion_cc_q (n int PRIMARY KEY, q text NOT NULL);

-- ---- 1. enum equality and IN lists ----
-- enum_ops is FOR TYPE anyenum; the constant is of the column's own enum
INSERT INTO lion_cc_q VALUES
(101, $$SELECT count(*) FROM lion_cc_t WHERE status = 'val1'$$),
(102, $$SELECT count(*) FROM lion_cc_t WHERE 'val3' = status$$),
(103, $$SELECT count(*) FROM lion_cc_t WHERE status = 'val4'$$),
(104, $$SELECT count(*) FROM lion_cc_t WHERE status IN ('val1', 'val3')$$),
(105, $$SELECT count(*) FROM lion_cc_t WHERE status = ANY ('{val2}'::lion_cc_ws[])$$),
(106, $$SELECT count(*) FROM lion_cc_t WHERE status = 'val1' AND country = 'c3'$$),
(107, $$SELECT count(*) FROM lion_cc_t WHERE status IN ('val2', 'val3') AND country IN ('c1', 'c2')$$),
(108, $$SELECT count(status) FROM lion_cc_t WHERE status = 'val1'$$),
(109, $$SELECT count(*) FROM lion_cc_t WHERE status = 'val1' OR status IS NULL$$),
(110, $$SELECT count(*) FROM lion_cc_t WHERE status < 'val3'$$),
-- GROUP BY an enum column, and a grouping the planner folds to the constant
(111, $$SELECT status, count(*) FROM lion_cc_t GROUP BY status$$),
(112, $$SELECT status, count(*) FROM lion_cc_t WHERE status IN ('val1', 'val3') GROUP BY status$$),
(113, $$SELECT status, count(*) FROM lion_cc_t WHERE status = 'val1' GROUP BY status$$),
(114, $$SELECT country, count(*) FROM lion_cc_t WHERE status = 'val2' GROUP BY country$$),
(115, $$SELECT status, count(*) FROM lion_cc_t WHERE country = 'c4' GROUP BY status$$),
-- a domain over the enum: the parser only takes it cast to its enum
(116, $$SELECT count(*) FROM lion_cc_t WHERE dstatus::lion_cc_ws = 'val1'$$),
(117, $$SELECT count(*) FROM lion_cc_t WHERE dstatus::lion_cc_ws IN ('val1', 'val2')$$),
(118, $$SELECT dstatus, count(*) FROM lion_cc_t GROUP BY dstatus$$),
-- `<>` and NOT IN are no strategy of the index: the ordinary plan
(119, $$SELECT count(*) FROM lion_cc_t WHERE status <> 'val1'$$),
(120, $$SELECT count(*) FROM lion_cc_t WHERE status NOT IN ('val1', 'val3')$$),
-- the FK-side join (DESIGN.md §27) on an enum key
(121, $$SELECT d.label, count(*) FROM lion_cc_t f JOIN lion_cc_dim d ON f.status = d.status GROUP BY d.label$$),
(122, $$SELECT d.status, count(*) FROM lion_cc_t f JOIN lion_cc_dim d ON f.status = d.status WHERE f.country = 'c1' AND f.pc = 3 GROUP BY d.status$$);

-- ---------- over one single-column index per column ----------
CREATE INDEX lion_cc_status ON lion_cc_t USING lion (status);
CREATE INDEX lion_cc_dstatus ON lion_cc_t USING lion (dstatus);
CREATE INDEX lion_cc_flag ON lion_cc_t USING lion (flag);
CREATE INDEX lion_cc_pc ON lion_cc_t USING lion (pc);
CREATE INDEX lion_cc_d ON lion_cc_t USING lion (d);
CREATE INDEX lion_cc_ts ON lion_cc_t USING lion (ts);
CREATE INDEX lion_cc_country ON lion_cc_t USING lion (country);
VACUUM ANALYZE lion_cc_t, lion_cc_dim;

SELECT n, lion_cc(q) FROM lion_cc_q ORDER BY n;

-- What EXPLAIN prints: each clause as core prints its index condition.
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_cc_t WHERE status = 'val1';
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_cc_t WHERE status IN ('val1', 'val3');
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_cc_t WHERE dstatus::lion_cc_ws = 'val1';
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
RESET enable_indexonlyscan;

-- The same clauses through the access method's own scans.
SELECT lion_cc_scan('index', $$SELECT id FROM lion_cc_t WHERE status = 'val2'$$);
SELECT lion_cc_scan('bitmap', $$SELECT id FROM lion_cc_t WHERE dstatus::lion_cc_ws IN ('val1', 'val3')$$);

/*
 * lion_index_posting_root() takes a key of the column's own type, which for
 * enum_ops is the column's enum (or a domain over it) and not anyenum; a
 * different enum is still no key of this column.  'val4' has no entry.
 */
SELECT lion_index_posting_root('lion_cc_status', 'val4'::lion_cc_ws);
SELECT lion_index_posting_root('lion_cc_status', 'val4'::lion_cc_wsd);
SELECT lion_index_posting_root('lion_cc_status', 'val4'::lion_cc_other);

-- ---------- over one multicolumn index (DESIGN.md §24) ----------
DROP INDEX lion_cc_status, lion_cc_dstatus, lion_cc_flag, lion_cc_pc, lion_cc_d,
	lion_cc_ts, lion_cc_country;
CREATE INDEX lion_cc_m ON lion_cc_t USING lion (status, flag, pc, d, ts, country, dstatus);
VACUUM ANALYZE lion_cc_t;

SELECT n, lion_cc(q) FROM lion_cc_q ORDER BY n;

SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_cc_t WHERE status = 'val1' AND country = 'c1';
EXPLAIN (COSTS OFF) SELECT flag, count(*) FROM lion_cc_t WHERE status IN ('val1', 'val3') GROUP BY flag;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
RESET enable_indexonlyscan;

-- ... and a dirty heap, whose candidates the node rechecks
UPDATE lion_cc_t SET flag = NOT flag, status = 'val3' WHERE id % 11 = 0;
SELECT n, lion_cc(q) FROM lion_cc_q WHERE n IN (101, 104, 111, 113, 121) ORDER BY n;

DROP TABLE lion_cc_q, lion_cc_t, lion_cc_dim;
DROP DOMAIN lion_cc_wsd;
DROP TYPE lion_cc_ws, lion_cc_other;
DROP FUNCTION lion_cc(text), lion_cc_scan(text, text);
