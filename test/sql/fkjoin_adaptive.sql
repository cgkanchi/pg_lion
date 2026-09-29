-- FK-side join pushdown: probed, then collected (DESIGN.md §27).  A plan
-- that probes the fact filters - because it expected few dimension rows,
-- or here because the copy is priced out of it - keeps an account of what
-- probing has cost, and collects the filters part way through once that
-- reaches what collecting them costs.  The dimension's two filter flags are
-- the same on every row, which core multiplies into an estimate of one row,
-- and they keep the fact's heavy keys.  Every answer is checked against the
-- same query with the pushdown off.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;
-- The plans below probe: the collection's pass over the filters is priced
-- out of the planner's choice.  The run's own price of collecting them is
-- made from the sets it located, and does not read that setting.
SET pg_lion.fkjoin_collect_container_cost = 1e9;
-- The upper node's switch is what this file is about: with core's joins
-- disabled, a count of an anti join could otherwise be core's Agg over the
-- semi/anti join path (DESIGN.md §27, "The semi and anti join as a join
-- path"), whose lookups switch the same way but are not what these show.
SET pg_lion.enable_semijoin = off;

/*
 * lion_aj() runs a query through the pushdown with every join method
 * disabled, so that the node is EXERCISED, and again with the pushdown off
 * and sequential scans only, and compares the two as multisets; it says
 * whether the node ran and, from its EXPLAIN ANALYZE, how many times it
 * switched to a copy of the fact filters.
 */
CREATE FUNCTION lion_aj_node(q text) RETURNS jsonb
LANGUAGE plpgsql AS $$
DECLARE
	e jsonb;
BEGIN
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	EXECUTE 'EXPLAIN (ANALYZE, FORMAT JSON, COSTS OFF, SUMMARY OFF, TIMING OFF, BUFFERS OFF) ' || q
		INTO e;
	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_nestloop', 'on', true);
	RETURN jsonb_path_query_first(e,
		'$.** ? (@."Custom Plan Provider" == "LionCount")');
END $$;

CREATE FUNCTION lion_aj(q text, par bool DEFAULT false) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	node jsonb;
	how text;
	sw bigint;
	nrows bigint;
	ndiff bigint;
BEGIN
	node := lion_aj_node(q);
	IF node IS NULL THEN
		how := 'not pushed down';
	ELSE
		sw := coalesce((node->>'Fact Filter Switches')::bigint, 0);
		how := CASE WHEN (node->>'Parallel Aware')::bool
					THEN 'pushed down, parallel' ELSE 'pushed down' END ||
			CASE WHEN node->>'Join Key Lookups' IS NOT NULL
				 THEN ', walked' ELSE '' END ||
			CASE WHEN par THEN (CASE WHEN sw > 0 THEN ', switched' ELSE ', probed' END)
				 WHEN sw > 0 THEN format(', switched %s after %s keys', sw,
										 node->>'Fact Filter Keys Probed')
				 ELSE ', probed' END;
	END IF;

	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	EXECUTE format('CREATE TEMP TABLE lion_aj_on AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_nestloop', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	EXECUTE format('CREATE TEMP TABLE lion_aj_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM lion_aj_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_aj_on EXCEPT ALL SELECT * FROM lion_aj_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_aj_off EXCEPT ALL SELECT * FROM lion_aj_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lion_aj_on, lion_aj_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows', how, nrows);
END $$;

/* EXPLAIN ANALYZE's lines about the fact filters, joins disabled. */
CREATE FUNCTION lion_aj_explain(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF, BUFFERS OFF) ' || q LOOP
		IF ln ~ 'LionCount|Fact Filter|Join Keys Looked Up|Join Key Lookups|Join Type' THEN
			RETURN NEXT regexp_replace(ln, '\s+\(actual rows=.*\)$', '');
		END IF;
	END LOOP;
	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_nestloop', 'on', true);
END $$;

/*
 * The fact: 120000 rows over some 1700 heap pages, 27 container keys.  Half
 * of the rows belong to 40 heavy keys, 1500 each, scattered over every
 * container key; the other half to 12000 light keys, five each.  doc holds
 * five dense terms, each on three rows in ten, and two that never meet
 * (ya on x = 1, yb on x = 2).  The fk index is built at fillfactor 20, so
 * that its directory is tall enough for the walk in key order to pay.
 */
CREATE TABLE lion_adf (id int NOT NULL, fk int NOT NULL, x int NOT NULL,
					   doc tsvector NOT NULL, pad text);
INSERT INTO lion_adf
SELECT i,
	   CASE WHEN i % 2 = 0 THEN 1 + (i / 2) % 40
			ELSE 41 + ((i / 2) * 7919) % 12000 END,
	   i % 10,
	   array_to_tsvector(array_remove(ARRAY[
		   CASE WHEN (i * 7) % 10 < 3 THEN 'da' END,
		   CASE WHEN (i * 13) % 10 < 3 THEN 'db' END,
		   CASE WHEN (i * 17) % 11 < 3 THEN 'dc' END,
		   CASE WHEN (i * 19) % 13 < 4 THEN 'dd' END,
		   CASE WHEN (i * 23) % 7 < 2 THEN 'de' END,
		   CASE WHEN i % 10 = 1 THEN 'ya' END,
		   CASE WHEN i % 10 = 2 THEN 'yb' END,
		   'w' || (i % 97)]::text[], NULL)),
	   repeat('p', 40)
FROM generate_series(1, 120000) i;
CREATE INDEX lion_adf_fk ON lion_adf USING lion (fk) WITH (fillfactor = 20);
CREATE INDEX lion_adf_doc ON lion_adf USING lion (doc);
CREATE INDEX lion_adf_x ON lion_adf USING lion (x);

/*
 * The dimension: a key for each fact key and 2000 without fact rows; h1 and
 * h2 are true on the heavy keys alone, attr takes four values over them.
 */
CREATE TABLE lion_add (pk int PRIMARY KEY, h1 bool NOT NULL, h2 bool NOT NULL,
					   attr int NOT NULL)
	WITH (parallel_workers = 2);
INSERT INTO lion_add
SELECT i, i <= 40, i <= 40, i % 4 FROM generate_series(1, 14040) i;
VACUUM (FREEZE, ANALYZE) lion_adf;
VACUUM (FREEZE, ANALYZE) lion_add;

-- 1. The heavy keys under the tsquery: core expects one dimension row, the
-- plan probes, and the run collects the filters after a few keys.
SELECT * FROM lion_aj_explain($$
	SELECT count(*) FROM lion_adf f JOIN lion_add d ON d.pk = f.fk
	WHERE d.h1 AND d.h2 AND f.doc @@ '(da | db | dc) & (dd | de)'$$);
SELECT lion_aj($$
	SELECT count(*) FROM lion_adf f JOIN lion_add d ON d.pk = f.fk
	WHERE d.h1 AND d.h2 AND f.doc @@ '(da | db | dc) & (dd | de)'$$);
-- ... grouped, each dimension row handed up
SELECT lion_aj($$
	SELECT d.attr, count(*) FROM lion_adf f JOIN lion_add d ON d.pk = f.fk
	WHERE d.h1 AND d.h2 AND f.doc @@ '(da | db | dc) & (dd | de)'
	GROUP BY d.attr$$);
-- ... an IN list, a semi join and an anti join
SELECT lion_aj($$
	SELECT count(*) FROM lion_adf f JOIN lion_add d ON d.pk = f.fk
	WHERE d.h1 AND d.h2 AND f.x IN (1, 3, 5, 7)$$);
SELECT lion_aj($$
	SELECT count(*) FROM lion_add d WHERE d.h1 AND d.h2
	AND EXISTS (SELECT 1 FROM lion_adf f WHERE f.fk = d.pk
				AND f.doc @@ '(da | db | dc) & (dd | de)' AND f.x IN (1, 3))$$);
SELECT lion_aj($$
	SELECT count(*) FROM lion_add d WHERE d.h1 AND d.h2
	AND NOT EXISTS (SELECT 1 FROM lion_adf f WHERE f.fk = d.pk
					AND f.doc @@ 'da & dd' AND f.x = 4)$$);

-- 2. Every dimension row, walked in key order: the heavy keys come first.
SELECT * FROM lion_aj_explain($$
	SELECT count(*) FROM lion_adf f JOIN lion_add d ON d.pk = f.fk
	WHERE f.doc @@ '(da | db | dc) & (dd | de)'$$);
SELECT lion_aj($$
	SELECT count(*) FROM lion_adf f JOIN lion_add d ON d.pk = f.fk
	WHERE f.doc @@ '(da | db | dc) & (dd | de)'$$);
SELECT lion_aj($$
	SELECT d.attr, count(*) FROM lion_adf f JOIN lion_add d ON d.pk = f.fk
	WHERE f.doc @@ '(da | db | dc) & (dd | de)' GROUP BY d.attr$$);

-- 3. Two terms that never meet: the copy made part way through selects
-- nothing, and the keys after it are looked up no more - an anti join's
-- rows are every one of them, row at a time and walked.
SELECT * FROM lion_aj_explain($$
	SELECT count(*) FROM lion_add d WHERE d.h1 AND d.h2
	AND NOT EXISTS (SELECT 1 FROM lion_adf f WHERE f.fk = d.pk
					AND f.doc @@ 'ya & yb')$$);
SELECT lion_aj($$
	SELECT count(*) FROM lion_add d WHERE d.h1 AND d.h2
	AND NOT EXISTS (SELECT 1 FROM lion_adf f WHERE f.fk = d.pk
					AND f.doc @@ 'ya & yb')$$);
SELECT lion_aj($$
	SELECT count(*) FROM lion_add d
	WHERE NOT EXISTS (SELECT 1 FROM lion_adf f WHERE f.fk = d.pk
					  AND f.doc @@ 'ya & yb')$$);
SELECT lion_aj($$
	SELECT count(*) FROM lion_adf f JOIN lion_add d ON d.pk = f.fk
	WHERE f.doc @@ 'ya & yb'$$);

-- 4. The switch off: the run probes to its end, with the same answers.
SET pg_lion.enable_filter_switch = off;
SELECT * FROM lion_aj_explain($$
	SELECT count(*) FROM lion_adf f JOIN lion_add d ON d.pk = f.fk
	WHERE d.h1 AND d.h2 AND f.doc @@ '(da | db | dc) & (dd | de)'$$);
SELECT lion_aj($$
	SELECT count(*) FROM lion_adf f JOIN lion_add d ON d.pk = f.fk
	WHERE d.h1 AND d.h2 AND f.doc @@ '(da | db | dc) & (dd | de)'$$);
RESET pg_lion.enable_filter_switch;

-- 5. A copy that would not fit a hash table's memory is not made: at 64 kB
-- the run probes to its end.
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SELECT lion_aj($$
	SELECT count(*) FROM lion_adf f JOIN lion_add d ON d.pk = f.fk
	WHERE d.h1 AND d.h2 AND f.doc @@ '(da | db | dc) & (dd | de)'$$);
RESET work_mem;
RESET hash_mem_multiplier;

-- 6. Rescans: the node below a correlated subquery, its dimension filtered
-- by the outer row, starts its account again at each run - two runs of
-- twenty heavy keys each, two switches.
SELECT (n->>'Actual Loops')::int AS loops,
	   (n->>'Fact Filter Switches')::int AS switches
FROM lion_aj_node($$
	SELECT g, (SELECT count(*) FROM lion_adf f JOIN lion_add d ON d.pk = f.fk
			   WHERE d.h1 AND d.h2 AND d.attr / 2 = g
			   AND f.doc @@ '(da | db | dc) & (dd | de)')
	FROM generate_series(0, 1) g$$) n;
SELECT lion_aj($$
	SELECT g, (SELECT count(*) FROM lion_adf f JOIN lion_add d ON d.pk = f.fk
			   WHERE d.h1 AND d.h2 AND d.attr / 2 = g
			   AND f.doc @@ '(da | db | dc) & (dd | de)')
	FROM generate_series(0, 1) g$$);

-- 7. A partitioned fact: each leaf keeps its own account and switches on
-- its own, and its copy serves its later turns.
CREATE TABLE lion_adp (id int NOT NULL, fk int NOT NULL, x int NOT NULL,
					   doc tsvector NOT NULL, part int NOT NULL)
	PARTITION BY LIST (part);
CREATE TABLE lion_adp0 PARTITION OF lion_adp FOR VALUES IN (0);
CREATE TABLE lion_adp1 PARTITION OF lion_adp FOR VALUES IN (1);
CREATE TABLE lion_adp2 PARTITION OF lion_adp FOR VALUES IN (2);
INSERT INTO lion_adp SELECT id, fk, x, doc, id % 3 FROM lion_adf;
CREATE INDEX lion_adp_fk ON lion_adp USING lion (fk);
CREATE INDEX lion_adp_doc ON lion_adp USING lion (doc);
CREATE INDEX lion_adp_x ON lion_adp USING lion (x);
VACUUM (FREEZE, ANALYZE) lion_adp;
SELECT * FROM lion_aj_explain($$
	SELECT count(*) FROM lion_adp f JOIN lion_add d ON d.pk = f.fk
	WHERE d.h1 AND d.h2 AND f.doc @@ '(da | db | dc) & (dd | de)'$$);
SELECT lion_aj($$
	SELECT count(*) FROM lion_adp f JOIN lion_add d ON d.pk = f.fk
	WHERE d.h1 AND d.h2 AND f.doc @@ '(da | db | dc) & (dd | de)'$$);
SELECT lion_aj($$
	SELECT d.attr, count(*) FROM lion_adp f JOIN lion_add d ON d.pk = f.fk
	WHERE f.doc @@ '(da | db | dc) & (dd | de)' GROUP BY d.attr$$);
SELECT lion_aj($$
	SELECT count(*) FROM lion_add d WHERE d.h1 AND d.h2
	AND EXISTS (SELECT 1 FROM lion_adp f WHERE f.fk = d.pk
				AND f.doc @@ '(da | db | dc) & (dd | de)' AND f.x IN (1, 3))$$);
SELECT lion_aj($$
	SELECT count(*) FROM lion_add d
	WHERE NOT EXISTS (SELECT 1 FROM lion_adp f WHERE f.fk = d.pk
					  AND f.doc @@ 'ya & yb')$$);

-- 8. In parallel each participant keeps its own account and makes its own
-- copy; which of them switch depends on the rows each is given.
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SELECT lion_aj($$
	SELECT count(*) FROM lion_adf f JOIN lion_add d ON d.pk = f.fk
	WHERE f.doc @@ '(da | db | dc) & (dd | de)'$$, true);
SELECT lion_aj($$
	SELECT d.attr, count(*) FROM lion_adf f JOIN lion_add d ON d.pk = f.fk
	WHERE f.doc @@ '(da | db | dc) & (dd | de)' GROUP BY d.attr$$, true);
SELECT lion_aj($$
	SELECT count(*) FROM lion_add d
	WHERE NOT EXISTS (SELECT 1 FROM lion_adf f WHERE f.fk = d.pk
					  AND f.doc @@ '(da | db | dc) & (dd | de)' AND f.x = 4)$$, true);
SELECT lion_aj($$
	SELECT count(*) FROM lion_adp f JOIN lion_add d ON d.pk = f.fk
	WHERE f.doc @@ '(da | db | dc) & (dd | de)'$$, true);
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET min_parallel_index_scan_size;
SET max_parallel_workers_per_gather = 0;

-- 9. A dirty heap: heavy keys' rows deleted and updated - some of them to
-- other keys, some to other terms - counted against the snapshot, then
-- again after VACUUM.
DELETE FROM lion_adf WHERE fk IN (1, 2, 3) AND id % 5 = 0;
UPDATE lion_adf SET fk = fk + 1 WHERE fk BETWEEN 4 AND 8 AND id % 7 = 0;
UPDATE lion_adf SET doc = 'da dd'::tsvector WHERE fk = 9 AND id % 3 = 0;
DELETE FROM lion_adp WHERE fk IN (1, 2, 3) AND id % 5 = 0;
UPDATE lion_adp SET doc = 'db de'::tsvector WHERE fk = 10 AND id % 4 = 0;
SELECT lion_aj($$
	SELECT count(*) FROM lion_adf f JOIN lion_add d ON d.pk = f.fk
	WHERE d.h1 AND d.h2 AND f.doc @@ '(da | db | dc) & (dd | de)'$$);
SELECT lion_aj($$
	SELECT d.attr, count(*) FROM lion_adf f JOIN lion_add d ON d.pk = f.fk
	WHERE f.doc @@ '(da | db | dc) & (dd | de)' GROUP BY d.attr$$);
SELECT lion_aj($$
	SELECT count(*) FROM lion_adp f JOIN lion_add d ON d.pk = f.fk
	WHERE d.h1 AND d.h2 AND f.doc @@ '(da | db | dc) & (dd | de)'$$);
VACUUM lion_adf;
VACUUM lion_adp;
SELECT lion_aj($$
	SELECT count(*) FROM lion_adf f JOIN lion_add d ON d.pk = f.fk
	WHERE d.h1 AND d.h2 AND f.doc @@ '(da | db | dc) & (dd | de)'$$);
SELECT lion_aj($$
	SELECT d.attr, count(*) FROM lion_adf f JOIN lion_add d ON d.pk = f.fk
	WHERE f.doc @@ '(da | db | dc) & (dd | de)' GROUP BY d.attr$$);
SELECT lion_aj($$
	SELECT count(*) FROM lion_adp f JOIN lion_add d ON d.pk = f.fk
	WHERE d.h1 AND d.h2 AND f.doc @@ '(da | db | dc) & (dd | de)'$$);

DROP TABLE lion_adp, lion_adf, lion_add;
DROP FUNCTION lion_aj(text, bool), lion_aj_node(text), lion_aj_explain(text);
