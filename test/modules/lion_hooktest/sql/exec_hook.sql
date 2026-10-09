-- The function-execute hook (InvokeFunctionExecuteHook) a security module
-- such as sepgsql sees, through every custom node of pg_lion
-- (test/hook-check.sh): a node that stands in for an expression - a clause's
-- operator it answers from a posting set, an aggregate it computes, a score
-- it returns - calls the hook for it as ExecInitFunc() would have, so a
-- function the module refuses is refused through the node exactly as through
-- the ordinary plan: when the plan starts, plain EXPLAIN and a cached plan
-- included.  lion_hooktest.deny_execute names the refused function.  The
-- node list is pg_lion's test/sql/node_contracts.sql's, which checks the
-- EXECUTE privileges themselves: a new node goes in both.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
CREATE EXTENSION IF NOT EXISTS lion_hooktest;
LOAD 'lion_hooktest';
RESET client_min_messages;
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE nc (id int, k int, j int, o int, d tsvector);
INSERT INTO nc SELECT i, i % 10, i % 7, (i * 7919) % 3001,
	   to_tsvector('simple', 'w' || (i % 5) || ' w' || (i % 7) || ' w' || (i % 11))
  FROM generate_series(1, 3000) i;
CREATE INDEX nc_k ON nc USING lion (k);
CREATE INDEX nc_j ON nc USING lion (j);
CREATE INDEX nc_d ON nc USING lion (d) WITH (store_positions = true);
CREATE INDEX nc_o ON nc (o) INCLUDE (id);
CREATE FUNCTION nc_bucket(int) RETURNS int IMMUTABLE LANGUAGE plpgsql
	AS 'BEGIN RETURN $1 % 4; END';
CREATE INDEX nc_b ON nc USING lion (nc_bucket(j));
CREATE TABLE ncd (pk int PRIMARY KEY, name text NOT NULL);
INSERT INTO ncd SELECT i, 'n' || (i % 2) FROM generate_series(0, 12) i;
VACUUM (FREEZE, ANALYZE) nc;
VACUUM (FREEZE, ANALYZE) ncd;

CREATE TABLE nc_nodes (node text PRIMARY KEY, q text, fn regprocedure, force text);
INSERT INTO nc_nodes VALUES
	('LionCount',
	 'SELECT count(*) FROM nc WHERE k = 3', 'int4eq(int4,int4)', ''),
	('LionCount, its aggregate',
	 'SELECT count(*) FROM nc WHERE k = 3', 'count()', ''),
	('LionCount, an expression column filtered',
	 'SELECT count(*) FROM nc WHERE nc_bucket(j) = 1', 'nc_bucket(int4)', ''),
	('LionCount, an expression column grouped',
	 'SELECT nc_bucket(j), count(*) FROM nc GROUP BY 1', 'nc_bucket(int4)', ''),
	('LionJoinAgg',
	 'SELECT count(DISTINCT d.name), count(*) FROM nc JOIN ncd d ON d.pk = nc.k WHERE nc.j = 3',
	 'int4eq(int4,int4)', ''),
	('LionSemiJoin',
	 'SELECT d.pk FROM ncd d WHERE EXISTS (SELECT 1 FROM nc WHERE nc.k = d.pk AND nc.j = 3)',
	 'int4eq(int4,int4)', 'enable_hashjoin enable_mergejoin enable_nestloop'),
	('LionAntiJoin',
	 'SELECT d.pk FROM ncd d WHERE NOT EXISTS (SELECT 1 FROM nc WHERE nc.k = d.pk AND nc.j = 3)',
	 'int4eq(int4,int4)', 'enable_hashjoin enable_mergejoin enable_nestloop'),
	('LionOrdered',
	 'SELECT id FROM nc WHERE k = 3 ORDER BY o LIMIT 5', 'int4eq(int4,int4)',
	 'enable_sort enable_seqscan enable_bitmapscan enable_indexscan'),
	('LionBtreeScan',
	 'SELECT id, o FROM nc WHERE o BETWEEN 100 AND 400 AND k = 3', 'int4eq(int4,int4)',
	 'enable_seqscan enable_bitmapscan enable_indexscan'),
	('LionBm25',
	 'SELECT id, lion_bm25_score(d, ''w1 & w3'', ''nc_d'') FROM nc WHERE d @@ ''w1 & w3''
	   ORDER BY lion_bm25_score(d, ''w1 & w3'', ''nc_d'') DESC LIMIT 5',
	 'lion_bm25_score(tsvector,tsquery,regclass,float8,float8)',
	 'enable_seqscan enable_bitmapscan enable_indexscan');

-- lion's paths on and the core settings in force off, or the other way round
CREATE FUNCTION nc_paths(force text, lion boolean) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE
	g text;
BEGIN
	FOREACH g IN ARRAY ARRAY['pg_lion.enable_count_pushdown', 'pg_lion.enable_semijoin',
							 'pg_lion.enable_ordered_scan', 'pg_lion.enable_btree_scan',
							 'pg_lion.enable_bm25_scan'] LOOP
		PERFORM set_config(g, CASE WHEN lion THEN 'on' ELSE 'off' END, true);
	END LOOP;
	FOREACH g IN ARRAY string_to_array(force, ' ') LOOP
		PERFORM set_config(g, CASE WHEN lion THEN 'off' ELSE 'on' END, true);
	END LOOP;
END $$;

CREATE FUNCTION nc_rows(q text) RETURNS text
LANGUAGE sql AS $$
	SELECT format('SELECT coalesce(string_agg(s::text, '' '' ORDER BY s::text), ''(none)'') FROM (%s) s', q)
$$;

CREATE FUNCTION nc_try(stmt text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	r text;
BEGIN
	EXECUTE stmt INTO r;
	RETURN r;
EXCEPTION WHEN insufficient_privilege THEN
	RETURN 'ERROR: ' || SQLERRM;
END $$;

CREATE FUNCTION nc_explain(q text, node text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	r text := 'NOT PLANNED';
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (' || split_part(node, ',', 1) || ')%' THEN
			r := 'planned';
		END IF;
	END LOOP;
	RETURN r;
EXCEPTION WHEN insufficient_privilege THEN
	RETURN 'ERROR: ' || SQLERRM;
END $$;

CREATE FUNCTION nc_vs(lion text, ordinary text) RETURNS text
LANGUAGE sql AS $$
	SELECT CASE WHEN lion = ordinary THEN 'as the ordinary plan'
				ELSE lion || ' | the ordinary plan: ' || ordinary END
$$;

-- one node's query through the node and the ordinary plan, with its function
-- refused by the hook: run, plain EXPLAIN, and a generic plan made before
CREATE FUNCTION nc_hook(name text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	n nc_nodes;
	answer text;
	a text;
BEGIN
	SELECT * INTO STRICT n FROM nc_nodes WHERE node = name;
	PERFORM nc_paths(n.force, true);
	answer := nc_try(nc_rows(n.q));
	RETURN NEXT 'plan: ' || nc_explain(n.q, n.node);
	PERFORM set_config('plan_cache_mode', 'force_generic_plan', true);
	EXECUTE 'PREPARE nc_p AS ' || nc_rows(n.q);
	a := nc_try('EXECUTE nc_p');

	PERFORM set_config('lion_hooktest.deny_execute', n.fn::oid::text, true);
	PERFORM nc_paths(n.force, true);
	a := nc_try(nc_rows(n.q));
	PERFORM nc_paths(n.force, false);
	RETURN NEXT 'refused: ' || a || ', ' || nc_vs(a, nc_try(nc_rows(n.q)));
	PERFORM nc_paths(n.force, true);
	a := nc_explain(n.q, n.node);
	PERFORM nc_paths(n.force, false);
	RETURN NEXT 'plain EXPLAIN: ' || nc_vs(a, nc_explain(n.q, n.node));
	RETURN NEXT 'a cached plan: ' || nc_try('EXECUTE nc_p');

	PERFORM set_config('lion_hooktest.deny_execute', '', true);
	RETURN NEXT 'allowed again: ' ||
		CASE WHEN nc_try('EXECUTE nc_p') = answer THEN 'the answer'
			 ELSE nc_try('EXECUTE nc_p') END;
	DEALLOCATE nc_p;
END $$;

SELECT node, nc_hook(node) FROM nc_nodes ORDER BY node;
SHOW lion_hooktest.deny_execute;

DROP FUNCTION nc_hook(text);
DROP FUNCTION nc_vs(text, text);
DROP FUNCTION nc_explain(text, text);
DROP FUNCTION nc_try(text);
DROP FUNCTION nc_rows(text);
DROP FUNCTION nc_paths(text, boolean);
DROP TABLE nc_nodes, nc, ncd;
DROP FUNCTION nc_bucket(int);
