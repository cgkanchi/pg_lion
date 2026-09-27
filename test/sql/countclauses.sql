-- Clause shapes the count pushdown recognises (DESIGN.md §10).  Each of these
-- was once declined before any cost was asked, so the LionCount node never
-- saw the commonest filters of a real workload:
--
--	- an enum key, `status = 'live'` or `status IN ('live', 'dead')`;
--	- a boolean column tested by itself: `flag`, which is what `flag = true` is
--	  folded into, `NOT flag`, `flag IS TRUE` and the rest;
--	- a stable expression as a clause value, `ts >= now() - interval '90 days'`.
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
 * lion_cc_prep() does the same for a prepared statement under
 * force_generic_plan, which is what keeps `$1::date + 1` an expression over a
 * Param rather than the literal a custom plan folds it into.
 */
CREATE FUNCTION lion_cc_prep(q text, args text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	pushed boolean := false;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('plan_cache_mode', 'force_generic_plan', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE 'PREPARE lion_ccp_on AS ' || q;
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) EXECUTE lion_ccp_on(' || args || ')' LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			pushed := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lion_cc_on AS EXECUTE lion_ccp_on(%s)', args);

	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE 'PREPARE lion_ccp_off AS ' || q;
	EXECUTE format('CREATE TEMP TABLE lion_cc_off AS EXECUTE lion_ccp_off(%s)', args);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE 'DEALLOCATE lion_ccp_on';
	EXECUTE 'DEALLOCATE lion_ccp_off';

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
 * handed.  A window relative to now() or current_date says only whether it
 * found rows, not how many: the table's times are relative to when it was
 * built, and a run that straddles midnight would move the count.
 */
CREATE FUNCTION lion_cc_scan(how text, q text, exact boolean DEFAULT true)
RETURNS text
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
	RETURN format('%s, %s', coalesce(used, 'no lion scan'),
				  CASE WHEN exact THEN nrows || ' rows'
					   WHEN nrows > 0 THEN 'some rows' ELSE 'no rows' END);
END $$;

-- A STABLE function the planner cannot inline or fold: its value is a setting.
CREATE FUNCTION lion_cc_pick() RETURNS int
STABLE LANGUAGE plpgsql AS $$
BEGIN
	RETURN current_setting('lion_cc.pick')::int;
END $$;
SET lion_cc.pick = '5';
SET lion_cc.picks = '2,5,9';

/*
 * The table.  status is a nullable enum and dstatus the same values through a
 * domain over it; flag is a nullable boolean; pc has a different number of
 * rows for every value (2k + 1 of every 400 for pc = k), so that a count says
 * which value it was made for; d and ts lie in the recent past, relative to
 * the time the table is built, so that a window over current_date or now()
 * selects some of them and not all.
 */
CREATE TYPE lion_cc_ws AS ENUM ('live', 'parked', 'dead', 'unknown');
CREATE TYPE lion_cc_other AS ENUM ('live', 'parked', 'dead', 'unknown');
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
			ELSE (ARRAY['live', 'parked', 'dead']::lion_cc_ws[])[1 + (g % 7) % 3] END,
	   CASE WHEN g % 50 = 0 THEN NULL
			ELSE (ARRAY['live', 'parked', 'dead']::lion_cc_ws[])[1 + (g % 7) % 3] END,
	   CASE WHEN g % 23 = 0 THEN NULL ELSE g % 5 <> 0 END,
	   floor(sqrt(g % 400))::int,
	   current_date - (g % 90),
	   now() - (g % 2000) * interval '1 hour',
	   'c' || (g % 6)
  FROM generate_series(1, 12000) g;
CREATE TABLE lion_cc_dim (status lion_cc_ws PRIMARY KEY, label text NOT NULL);
INSERT INTO lion_cc_dim VALUES ('live', 'up'), ('parked', 'idle'), ('dead', 'down'),
	('unknown', 'up');

/*
 * The queries, run twice below: once over one single-column index per column
 * and once over a single multicolumn index (DESIGN.md §24).
 */
CREATE TABLE lion_cc_q (n int PRIMARY KEY, q text NOT NULL);

-- ---- 1. enum equality and IN lists ----
-- enum_ops is FOR TYPE anyenum; the constant is of the column's own enum
INSERT INTO lion_cc_q VALUES
(101, $$SELECT count(*) FROM lion_cc_t WHERE status = 'live'$$),
(102, $$SELECT count(*) FROM lion_cc_t WHERE 'dead' = status$$),
(103, $$SELECT count(*) FROM lion_cc_t WHERE status = 'unknown'$$),
(104, $$SELECT count(*) FROM lion_cc_t WHERE status IN ('live', 'dead')$$),
(105, $$SELECT count(*) FROM lion_cc_t WHERE status = ANY ('{parked}'::lion_cc_ws[])$$),
(106, $$SELECT count(*) FROM lion_cc_t WHERE status = 'live' AND country = 'c3'$$),
(107, $$SELECT count(*) FROM lion_cc_t WHERE status IN ('parked', 'dead') AND country IN ('c1', 'c2')$$),
(108, $$SELECT count(status) FROM lion_cc_t WHERE status = 'live'$$),
(109, $$SELECT count(*) FROM lion_cc_t WHERE status = 'live' OR status IS NULL$$),
(110, $$SELECT count(*) FROM lion_cc_t WHERE status < 'dead'$$),
-- GROUP BY an enum column, and a grouping the planner folds to the constant
(111, $$SELECT status, count(*) FROM lion_cc_t GROUP BY status$$),
(112, $$SELECT status, count(*) FROM lion_cc_t WHERE status IN ('live', 'dead') GROUP BY status$$),
(113, $$SELECT status, count(*) FROM lion_cc_t WHERE status = 'live' GROUP BY status$$),
(114, $$SELECT country, count(*) FROM lion_cc_t WHERE status = 'parked' GROUP BY country$$),
(115, $$SELECT status, count(*) FROM lion_cc_t WHERE country = 'c4' GROUP BY status$$),
-- a domain over the enum: the parser only takes it cast to its enum
(116, $$SELECT count(*) FROM lion_cc_t WHERE dstatus::lion_cc_ws = 'live'$$),
(117, $$SELECT count(*) FROM lion_cc_t WHERE dstatus::lion_cc_ws IN ('live', 'parked')$$),
(118, $$SELECT dstatus, count(*) FROM lion_cc_t GROUP BY dstatus$$),
-- `<>` and NOT IN are no strategy of the index: the ordinary plan
(119, $$SELECT count(*) FROM lion_cc_t WHERE status <> 'live'$$),
(120, $$SELECT count(*) FROM lion_cc_t WHERE status NOT IN ('live', 'dead')$$),
-- the FK-side join (DESIGN.md §27) on an enum key
(121, $$SELECT d.label, count(*) FROM lion_cc_t f JOIN lion_cc_dim d ON f.status = d.status GROUP BY d.label$$),
(122, $$SELECT d.status, count(*) FROM lion_cc_t f JOIN lion_cc_dim d ON f.status = d.status WHERE f.country = 'c1' AND f.pc = 3 GROUP BY d.status$$);

-- ---- 2. a boolean column tested by itself ----
-- `flag = true` is folded into `flag`, and `flag = false` into NOT flag
INSERT INTO lion_cc_q VALUES
(201, $$SELECT count(*) FROM lion_cc_t WHERE flag$$),
(202, $$SELECT count(*) FROM lion_cc_t WHERE flag = true$$),
(203, $$SELECT count(*) FROM lion_cc_t WHERE NOT flag$$),
(204, $$SELECT count(*) FROM lion_cc_t WHERE flag = false$$),
(205, $$SELECT count(*) FROM lion_cc_t WHERE flag <> true$$),
(206, $$SELECT count(*) FROM lion_cc_t WHERE flag IS TRUE$$),
(207, $$SELECT count(*) FROM lion_cc_t WHERE flag IS FALSE$$),
-- the two that hold for NULL too: an OR of the other value and the NULL entry
(208, $$SELECT count(*) FROM lion_cc_t WHERE flag IS NOT TRUE$$),
(209, $$SELECT count(*) FROM lion_cc_t WHERE flag IS NOT FALSE$$),
(210, $$SELECT count(*) FROM lion_cc_t WHERE NOT (flag IS TRUE) AND country = 'c2'$$),
(211, $$SELECT count(*) FROM lion_cc_t WHERE flag IS UNKNOWN$$),
(212, $$SELECT count(*) FROM lion_cc_t WHERE flag IS NOT UNKNOWN$$),
-- beside other columns, under an OR, and in its arms
(213, $$SELECT count(*) FROM lion_cc_t WHERE flag AND status = 'live'$$),
(214, $$SELECT count(*) FROM lion_cc_t WHERE NOT flag AND country = 'c1'$$),
(215, $$SELECT count(*) FROM lion_cc_t WHERE flag OR country = 'c1'$$),
(216, $$SELECT count(*) FROM lion_cc_t WHERE status = 'dead' OR flag IS NOT TRUE$$),
(217, $$SELECT count(*) FROM lion_cc_t WHERE (flag AND status = 'live') OR (NOT flag AND country = 'c2')$$),
(218, $$SELECT count(*) FROM lion_cc_t WHERE flag AND NOT flag$$),
(219, $$SELECT count(*) FROM lion_cc_t WHERE flag AND flag IS TRUE$$),
(220, $$SELECT count(flag) FROM lion_cc_t WHERE flag IS FALSE$$),
-- GROUP BY the boolean, and a boolean clause beside another GROUP BY
(221, $$SELECT flag, count(*) FROM lion_cc_t GROUP BY flag$$),
(222, $$SELECT flag, count(*) FROM lion_cc_t WHERE flag GROUP BY flag$$),
(223, $$SELECT flag, count(*) FROM lion_cc_t WHERE flag IS NOT FALSE GROUP BY flag$$),
(224, $$SELECT status, count(*) FROM lion_cc_t WHERE NOT flag GROUP BY status$$),
(225, $$SELECT count(DISTINCT status) FROM lion_cc_t WHERE flag$$),
-- a fact filter of the FK-side join
(226, $$SELECT d.status, count(*) FROM lion_cc_t f JOIN lion_cc_dim d ON f.status = d.status WHERE f.flag AND f.country = 'c1' GROUP BY d.status$$);

-- ---- 3. stable expressions as clause values ----
INSERT INTO lion_cc_q VALUES
(301, $$SELECT count(*) FROM lion_cc_t WHERE ts >= now() - interval '30 days'$$),
(302, $$SELECT count(*) FROM lion_cc_t WHERE ts >= now() - interval '30 days' AND country = 'c1'$$),
(303, $$SELECT count(*) FROM lion_cc_t WHERE ts >= now() - interval '30 days' AND ts < now() - interval '10 days'$$),
(304, $$SELECT count(*) FROM lion_cc_t WHERE now() - interval '5 days' > ts$$),
(305, $$SELECT count(*) FROM lion_cc_t WHERE d >= current_date - 30$$),
(306, $$SELECT count(*) FROM lion_cc_t WHERE d = current_date - 3$$),
(307, $$SELECT count(*) FROM lion_cc_t WHERE d IN (current_date, current_date - 1) AND flag$$),
(308, $$SELECT count(*) FROM lion_cc_t WHERE d = current_date - 3 OR status = 'dead'$$),
(309, $$SELECT pc, count(*) FROM lion_cc_t WHERE pc >= lion_cc_pick() GROUP BY pc$$),
(310, $$SELECT count(*) FROM lion_cc_t WHERE pc = lion_cc_pick()$$),
(311, $$SELECT count(*) FROM lion_cc_t WHERE pc IN (lion_cc_pick(), lion_cc_pick() + 2)$$),
(312, $$SELECT count(*) FROM lion_cc_t WHERE pc = ANY (string_to_array(current_setting('lion_cc.picks'), ',')::int[])$$),
(313, $$SELECT count(*) FROM lion_cc_t WHERE pc = (SELECT max(k) FROM (VALUES (3), (4)) v(k))$$),
(314, $$SELECT d.label, count(*) FROM lion_cc_t f JOIN lion_cc_dim d ON f.status = d.status WHERE f.d = current_date - 3 GROUP BY d.label$$),
-- not values: a volatile function, one in an IN list, a column of the relation
(315, $$SELECT count(*) FROM lion_cc_t WHERE pc = (random() * 0)::int$$),
(316, $$SELECT count(*) FROM lion_cc_t WHERE pc IN (3, (random() * 0)::int)$$),
(317, $$SELECT count(*) FROM lion_cc_t WHERE pc = id$$),
(318, $$SELECT count(*) FROM lion_cc_t WHERE ts >= now() - (random() * 0) * interval '1 day'$$);

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
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_cc_t WHERE status = 'live';
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_cc_t WHERE status IN ('live', 'dead');
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_cc_t WHERE dstatus::lion_cc_ws = 'live';
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_cc_t WHERE flag = true;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_cc_t WHERE NOT flag;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_cc_t WHERE flag IS TRUE AND status = 'parked';
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_cc_t WHERE flag IS NOT TRUE;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_cc_t WHERE ts >= now() - interval '30 days';
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_cc_t WHERE d >= current_date - 30;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_cc_t WHERE pc IN (lion_cc_pick(), 3);
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
RESET enable_indexonlyscan;

/*
 * A generic plan keeps an expression over a Param as it is, and the node
 * evaluates it at every execution: the same cached plan answers for each new
 * parameter, and for each new value of a stable function.
 */
SELECT lion_cc_prep('SELECT count(*) FROM lion_cc_t WHERE d >= $1::date + 1', 'current_date - 20');
SELECT lion_cc_prep('SELECT count(*) FROM lion_cc_t WHERE d = $1::date + 1', 'current_date - 20');
SELECT lion_cc_prep('SELECT count(*) FROM lion_cc_t WHERE ts >= $1::timestamptz - interval ''1 day''', 'now()');
SELECT lion_cc_prep('SELECT count(*) FROM lion_cc_t WHERE pc IN ($1 + 1, $1 + 2)', '3');
SELECT lion_cc_prep('SELECT count(*) FROM lion_cc_t WHERE flag = $1', 'false');
SELECT lion_cc_prep('SELECT count(*) FROM lion_cc_t WHERE status = $1', '''dead''');
SELECT lion_cc_prep('SELECT count(*) FROM lion_cc_t WHERE pc = $1 + 1', 'NULL');

SET plan_cache_mode = force_generic_plan;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
PREPARE lion_ccq(int) AS SELECT count(*) FROM lion_cc_t WHERE pc = $1 + 1;
EXPLAIN (COSTS OFF) EXECUTE lion_ccq(4);
EXECUTE lion_ccq(4);
EXECUTE lion_ccq(6);
PREPARE lion_ccf AS SELECT count(*) FROM lion_cc_t WHERE pc = lion_cc_pick();
EXPLAIN (COSTS OFF) EXECUTE lion_ccf;
EXECUTE lion_ccf;
SET lion_cc.pick = '7';
EXECUTE lion_ccf;
SET pg_lion.enable_count_pushdown = off;
SELECT count(*) FROM lion_cc_t WHERE pc = 5;
SELECT count(*) FROM lion_cc_t WHERE pc = 7;
RESET pg_lion.enable_count_pushdown;
DEALLOCATE lion_ccq;
DEALLOCATE lion_ccf;
RESET plan_cache_mode;
SET lion_cc.pick = '5';

/*
 * An exec Param inside an expression, which a nested loop changes between
 * rescans: the node evaluates `v.k + 1` again for every outer row.
 */
EXPLAIN (COSTS OFF)
SELECT v.k, s.c FROM (VALUES (1), (4), (6)) v(k),
	 LATERAL (SELECT count(*) AS c FROM lion_cc_t WHERE pc = v.k + 1) s;
SELECT v.k, s.c FROM (VALUES (1), (4), (6)) v(k),
	 LATERAL (SELECT count(*) AS c FROM lion_cc_t WHERE pc = v.k + 1) s ORDER BY v.k;
SET pg_lion.enable_count_pushdown = off;
SELECT v.k, s.c FROM (VALUES (1), (4), (6)) v(k),
	 LATERAL (SELECT count(*) AS c FROM lion_cc_t WHERE pc = v.k + 1) s ORDER BY v.k;
RESET pg_lion.enable_count_pushdown;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
RESET enable_indexonlyscan;

-- The same clauses through the access method's own scans.
SELECT lion_cc_scan('index', $$SELECT id FROM lion_cc_t WHERE status = 'parked'$$);
SELECT lion_cc_scan('bitmap', $$SELECT id FROM lion_cc_t WHERE dstatus::lion_cc_ws IN ('live', 'dead')$$);
SELECT lion_cc_scan('index', $$SELECT id FROM lion_cc_t WHERE flag$$);
SELECT lion_cc_scan('bitmap', $$SELECT id FROM lion_cc_t WHERE NOT flag$$);
SELECT lion_cc_scan('bitmap', $$SELECT id FROM lion_cc_t WHERE flag IS TRUE$$);
SELECT lion_cc_scan('index', $$SELECT id FROM lion_cc_t WHERE ts >= now() - interval '30 days'$$, false);
SELECT lion_cc_scan('bitmap', $$SELECT id FROM lion_cc_t WHERE d >= current_date - 30$$, false);
SELECT lion_cc_scan('index', $$SELECT id FROM lion_cc_t WHERE pc = lion_cc_pick()$$);
SELECT lion_cc_scan('bitmap', $$SELECT id FROM lion_cc_t WHERE pc IN (lion_cc_pick(), lion_cc_pick() + 2)$$);

/*
 * lion_index_posting_root() takes a key of the column's own type, which for
 * enum_ops is the column's enum (or a domain over it) and not anyenum; a
 * different enum is still no key of this column.  'unknown' has no entry.
 */
SELECT lion_index_posting_root('lion_cc_status', 'unknown'::lion_cc_ws);
SELECT lion_index_posting_root('lion_cc_status', 'unknown'::lion_cc_wsd);
SELECT lion_index_posting_root('lion_cc_status', 'unknown'::lion_cc_other);

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
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_cc_t WHERE status = 'live' AND flag;
EXPLAIN (COSTS OFF) SELECT flag, count(*) FROM lion_cc_t WHERE status IN ('live', 'dead') GROUP BY flag;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_cc_t WHERE ts >= now() - interval '30 days' AND NOT flag;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
RESET enable_indexonlyscan;

SELECT lion_cc_prep('SELECT count(*) FROM lion_cc_t WHERE d >= $1::date + 1 AND status = $2', 'current_date - 20, ''live''');

-- ... and a dirty heap, whose candidates the node rechecks
UPDATE lion_cc_t SET flag = NOT flag, status = 'dead' WHERE id % 11 = 0;
SELECT n, lion_cc(q) FROM lion_cc_q WHERE n IN (101, 104, 111, 113, 121, 201, 203, 208, 213, 301, 305, 310) ORDER BY n;

DROP TABLE lion_cc_q, lion_cc_t, lion_cc_dim;
DROP DOMAIN lion_cc_wsd;
DROP TYPE lion_cc_ws, lion_cc_other;
DROP FUNCTION lion_cc(text), lion_cc_prep(text, text),
	lion_cc_scan(text, text, boolean), lion_cc_pick();
RESET lion_cc.pick;
RESET lion_cc.picks;
