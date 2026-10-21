-- FK-side join with a fact filter decided from stored positions (DESIGN.md
-- §17, "Stored positions"; §27).  A phrase the posting sets only bound is
-- decided from the positions the index stores.  A collected copy of the fact
-- filters is decided as it is made: every row of the superset once, however
-- many keys count against it, and none of the counts decides a row again.  A
-- count that probes the filters decides its own key's rows, a few of every
-- part of the heap, each lexeme's cursor seeking past the chunks that cannot
-- hold them; what that costs goes on a probing run's account, which pays for
-- a copy once it reaches the copy's price (§27, "Probed, then collected").
--
-- Every answer is checked against the same query with the pushdown off and
-- sequential scans only; fp_run() says how the node had the filters, how
-- many candidates it decided from positions and how many of those it
-- turned away.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

/*
 * 20,000 rows over 200 keys, 100 rows a key spread over the whole heap.  A
 * key's rows cycle through four texts: `foo bar`, `bar foo`, `foo x bar` and
 * neither word, so `foo & bar` - the superset of every phrase below - holds
 * 75 of them and 15,000 in all, and each phrase 25 and 5,000.  Each lexeme
 * is in 15,000 rows: a position tree of a dozen chunks.
 */
CREATE TABLE fp_fact (id int, fk int, cat int, d tsvector)
	WITH (autovacuum_enabled = off);
INSERT INTO fp_fact
SELECT i, 1 + (i * 7919) % 200, i % 3,
	   to_tsvector('simple',
				   CASE (i / 200) % 4 WHEN 0 THEN 'foo bar w' || (i % 97)
									  WHEN 1 THEN 'bar foo w' || (i % 97)
									  WHEN 2 THEN 'foo x bar w' || (i % 97)
									  ELSE 'zzz w' || (i % 97) END)
FROM generate_series(1, 20000) i;
CREATE INDEX fp_fact_dfc ON fp_fact USING lion (d, fk, cat)
	WITH (store_positions = on);
CREATE TABLE fp_dim (pk int PRIMARY KEY, name text);
INSERT INTO fp_dim SELECT g, 'n' || (g % 4) FROM generate_series(1, 200) g;
VACUUM (FREEZE, ANALYZE) fp_fact, fp_dim;

CREATE FUNCTION fp_run(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	e jsonb;
	node jsonb;
	how text;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	EXECUTE 'EXPLAIN (ANALYZE, FORMAT JSON, COSTS OFF, SUMMARY OFF, TIMING OFF, BUFFERS OFF) ' || q
		INTO e;
	node := jsonb_path_query_first(e,
		'$.** ? (@."Custom Plan Provider" == "LionCount")');
	EXECUTE format('CREATE TEMP TABLE fp_on AS SELECT s::text AS r FROM (%s) s', q);

	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_nestloop', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	EXECUTE format('CREATE TEMP TABLE fp_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	SELECT count(*) INTO nrows FROM fp_on;
	SELECT (SELECT count(*) FROM (SELECT * FROM fp_on EXCEPT ALL SELECT * FROM fp_off) a) +
		   (SELECT count(*) FROM (SELECT * FROM fp_off EXCEPT ALL SELECT * FROM fp_on) b)
		INTO ndiff;
	DROP TABLE fp_on, fp_off;
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	IF node IS NULL THEN
		RETURN format('not pushed down, %s rows', nrows);
	END IF;
	IF node->>'Fact Groups Counted Against' IS NOT NULL THEN
		how := 'groups counted against ' || (node->>'Fact Groups Counted Against');
	ELSE
		how := coalesce(node->>'Fact Filters', 'probed');
	END IF;
	IF (node->>'Parallel Aware')::bool THEN
		how := how || ', parallel';
	END IF;
	RETURN format('%s; %s checked, %s removed; %s rows', how,
				  coalesce(node->>'Position Checks', '0'),
				  coalesce(node->>'Rows Removed by Positions', '0'), nrows);
END $$;

-- collected: the copy decides the superset's 15,000 rows once, for every key
SET pg_lion.fkjoin_count_cost = 1e6;
SELECT fp_run($q$SELECT count(*) FROM fp_fact f JOIN fp_dim d ON f.fk = d.pk
	WHERE d.pk <= 50 AND f.d @@ 'foo <-> bar'$q$);
SELECT fp_run($q$SELECT count(*) FROM fp_fact f JOIN fp_dim d ON f.fk = d.pk
	WHERE f.d @@ 'foo <2> bar'$q$);
SELECT fp_run($q$SELECT d.name, count(*) FROM fp_fact f JOIN fp_dim d ON f.fk = d.pk
	WHERE d.pk <= 120 AND f.d @@ 'foo <-> bar' GROUP BY d.name$q$);
-- ... and with a fact column grouped by, each key counted in every group of
-- it against the copy; or, with no dimension column in the rows, the keys
-- united with the filters, which decides the united keys' 3,750 once
SELECT fp_run($q$SELECT d.name, f.cat, count(*) FROM fp_fact f
	JOIN fp_dim d ON f.fk = d.pk
	WHERE d.pk <= 50 AND f.d @@ 'foo <-> bar' GROUP BY d.name, f.cat$q$);
SELECT fp_run($q$SELECT f.cat, count(*) FROM fp_fact f JOIN fp_dim d ON f.fk = d.pk
	WHERE d.pk <= 50 AND f.d @@ 'foo <-> bar' GROUP BY f.cat$q$);
-- ... and the copy a parallel plan's participants share, made a chunk at a
-- time by whichever of them takes it: its rows decided once all told
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SELECT fp_run($q$SELECT count(*) FROM fp_fact f JOIN fp_dim d ON f.fk = d.pk
	WHERE f.d @@ 'foo <-> bar'$q$);
SET max_parallel_workers_per_gather = 0;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET min_parallel_index_scan_size;
RESET pg_lion.fkjoin_count_cost;

-- probed: every count decides its own key's 75 candidates, 3,750 for 50 keys
SET pg_lion.fkjoin_collect_container_cost = 1e9;
SET pg_lion.enable_filter_switch = off;
SELECT fp_run($q$SELECT count(*) FROM fp_fact f JOIN fp_dim d ON f.fk = d.pk
	WHERE d.pk <= 50 AND f.d @@ 'foo <-> bar'$q$);
SELECT fp_run($q$SELECT d.pk, count(*) FROM fp_fact f JOIN fp_dim d ON f.fk = d.pk
	WHERE d.pk % 7 = 3 AND f.d @@ 'foo <2> bar' GROUP BY d.pk$q$);
SELECT fp_run($q$SELECT d.name, f.cat, count(*) FROM fp_fact f
	JOIN fp_dim d ON f.fk = d.pk
	WHERE d.pk <= 50 AND f.d @@ 'foo <-> bar' GROUP BY d.name, f.cat$q$);
RESET pg_lion.enable_filter_switch;

-- ... and the run's account: what its counts' cursors decode comes to the
-- price of a copy within a few of the 200 keys, and the run collects
SELECT fp_run($q$SELECT count(*) FROM fp_fact f JOIN fp_dim d ON f.fk = d.pk
	WHERE f.d @@ 'foo <-> bar'$q$);
RESET pg_lion.fkjoin_collect_container_cost;

-- An index scan decides its superset from positions too, its cursors begun
-- again at every scan: the inner scan of a key's rows, repeated for each
-- key, is priced with the seeks to them (lion_cost_scan_posfilter()) - both
-- lexemes' 15,000 members decoded, at 1.8 cpu_operator_cost, and 56
-- candidates estimated at 4.3 a lexeme - beyond the same scan of
-- `foo & bar`, which the posting sets answer alone.
CREATE FUNCTION fp_inner_cost(tq text) RETURNS float8
LANGUAGE plpgsql AS $$
DECLARE
	e jsonb;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE format('EXPLAIN (FORMAT JSON) SELECT sum(c) FROM fp_dim d, '
				   'LATERAL (SELECT count(*) AS c FROM fp_fact f '
				   'WHERE f.fk = d.pk AND f.d @@ %L::tsquery) s '
				   'WHERE d.pk <= 20', tq) INTO e;
	RETURN jsonb_path_query_first(e,
		'strict $.**?(@."Index Name" == "fp_fact_dfc")."Total Cost"')::float8;
END $$;
SELECT round((fp_inner_cost('foo <-> bar') -
			  fp_inner_cost('foo & bar'))::numeric) AS phrase_inner_more;

DROP FUNCTION fp_inner_cost(text);
DROP FUNCTION fp_run(text);
DROP TABLE fp_fact, fp_dim;
