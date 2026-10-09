-- The contracts every custom node of pg_lion keeps with PostgreSQL, run
-- against each node from one list, so a new node gets a line here (as
-- reader_contracts.sql does for the direct readers).  A node stands in for
-- expressions the executor would have initialised - a clause's operator it
-- answers from a posting set, an aggregate it computes, a score it returns -
-- and so asks EXECUTE on each of them itself (DESIGN.md §9, "Privileges"):
--   - of the current user, when the plan starts, plain EXPLAIN included;
--   - whichever role made the plan: a cached plan answers a REVOKE and a SET
--     ROLE as the ordinary plan does;
--   - of the owner inside a SECURITY DEFINER function;
-- and with the privilege its answer is the ordinary plan's.  The hook a
-- security module sees for the same check (InvokeFunctionExecuteHook) needs a
-- module that installs one: test/modules/lion_hooktest/sql/exec_hook.sql runs
-- this list against it.  security_exec.sql covers each node's own functions
-- in more detail.
--
-- Revoking a function from PUBLIC affects the whole database, so nc_check()
-- grants it back before it returns, and the file ends by checking that
-- nothing is left revoked.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;
CREATE ROLE lion_nc_yes;
CREATE ROLE lion_nc_no;

-- o: a permutation of the ids, with a covering btree
CREATE TABLE nc (id int, k int, j int, o int, d tsvector);
INSERT INTO nc SELECT i, i % 10, i % 7, (i * 7919) % 3001,
	   to_tsvector('simple', 'w' || (i % 5) || ' w' || (i % 7) || ' w' || (i % 11))
  FROM generate_series(1, 3000) i;
CREATE INDEX nc_k ON nc USING lion (k);
CREATE INDEX nc_j ON nc USING lion (j);
CREATE INDEX nc_d ON nc USING lion (d) WITH (store_positions = true);
CREATE INDEX nc_o ON nc (o) INCLUDE (id);
-- a dimension on nc.k, three of whose keys have no fact row
CREATE TABLE ncd (pk int PRIMARY KEY, name text NOT NULL);
INSERT INTO ncd SELECT i, 'n' || (i % 2) FROM generate_series(0, 12) i;
VACUUM (FREEZE, ANALYZE) nc;
VACUUM (FREEZE, ANALYZE) ncd;
GRANT SELECT ON nc, ncd TO lion_nc_yes, lion_nc_no;

-- every custom node, with a query it answers, a function it stands in for,
-- and the settings that make the planner choose it; a new node goes here
CREATE TABLE nc_nodes (node text PRIMARY KEY, q text, fn regprocedure, force text);
INSERT INTO nc_nodes VALUES
	('LionCount',
	 'SELECT count(*) FROM nc WHERE k = 3', 'int4eq(int4,int4)', ''),
	('LionCount, its aggregate',
	 'SELECT count(*) FROM nc WHERE k = 3', 'count()', ''),
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

-- lion's paths on, and the core settings in force turned off (for the
-- transaction); or every lion path off and core's settings back
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

-- q's rows as one sorted string
CREATE FUNCTION nc_rows(q text) RETURNS text
LANGUAGE sql AS $$
	SELECT format('SELECT coalesce(string_agg(s::text, '' '' ORDER BY s::text), ''(none)'') FROM (%s) s', q)
$$;

-- what statement stmt returns, or the privilege error it raised
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

-- plain EXPLAIN of q: the node it planned, or the privilege error it raised
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

-- q run as its owner
CREATE FUNCTION nc_def_yes(q text) RETURNS text SECURITY DEFINER
LANGUAGE plpgsql AS 'BEGIN RETURN nc_try(nc_rows(q)); END';
CREATE FUNCTION nc_def_no(q text) RETURNS text SECURITY DEFINER
LANGUAGE plpgsql AS 'BEGIN RETURN nc_try(nc_rows(q)); END';
ALTER FUNCTION nc_def_yes(text) OWNER TO lion_nc_yes;
ALTER FUNCTION nc_def_no(text) OWNER TO lion_nc_no;

-- what the node's plan says, against the ordinary plan's: "as the ordinary
-- plan" when they agree
CREATE FUNCTION nc_vs(lion text, ordinary text) RETURNS text
LANGUAGE sql AS $$
	SELECT CASE WHEN lion = ordinary THEN 'as the ordinary plan'
				ELSE lion || ' | the ordinary plan: ' || ordinary END
$$;

/*
 * nc_check() runs one node's query through the node and through the ordinary
 * plan: with every privilege, then with the function revoked from PUBLIC and
 * granted to lion_nc_yes alone - as lion_nc_no, as lion_nc_yes, through a
 * generic plan the superuser made, and through SECURITY DEFINER functions -
 * and grants it back.
 */
CREATE FUNCTION nc_check(name text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	n nc_nodes;
	answer text;
	a text;
	b text;
BEGIN
	SELECT * INTO STRICT n FROM nc_nodes WHERE node = name;
	PERFORM nc_paths(n.force, true);
	answer := nc_try(nc_rows(n.q));
	RETURN NEXT 'plan: ' || nc_explain(n.q, n.node);
	PERFORM nc_paths(n.force, false);
	RETURN NEXT 'every privilege: ' ||
		CASE WHEN answer = nc_try(nc_rows(n.q)) THEN 'the ordinary plan''s answer'
			 ELSE 'MISMATCH: ' || answer || ' | ' || nc_try(nc_rows(n.q)) END;

	EXECUTE format('REVOKE EXECUTE ON FUNCTION %s FROM PUBLIC', n.fn);
	EXECUTE format('GRANT EXECUTE ON FUNCTION %s TO lion_nc_yes', n.fn);

	-- a role without the privilege, and plain EXPLAIN
	PERFORM set_config('role', 'lion_nc_no', true);
	PERFORM nc_paths(n.force, true);
	a := nc_try(nc_rows(n.q));
	PERFORM nc_paths(n.force, false);
	RETURN NEXT 'without EXECUTE on ' || n.fn || ': ' || a || ', ' || nc_vs(a, nc_try(nc_rows(n.q)));
	PERFORM nc_paths(n.force, true);
	a := nc_explain(n.q, n.node);
	PERFORM nc_paths(n.force, false);
	RETURN NEXT 'plain EXPLAIN: ' || nc_vs(a, nc_explain(n.q, n.node));

	-- a role with it
	PERFORM set_config('role', 'lion_nc_yes', true);
	PERFORM nc_paths(n.force, true);
	a := nc_try(nc_rows(n.q));
	RETURN NEXT 'granted to the role: ' ||
		CASE WHEN a = answer THEN 'the answer' ELSE a END;

	-- a generic plan made by the superuser, run by each role
	PERFORM set_config('role', 'none', true);
	PERFORM set_config('plan_cache_mode', 'force_generic_plan', true);
	EXECUTE 'PREPARE nc_p AS ' || nc_rows(n.q);
	a := nc_try('EXECUTE nc_p');
	b := nc_explain('EXECUTE nc_p', n.node);
	PERFORM set_config('role', 'lion_nc_no', true);
	a := nc_try('EXECUTE nc_p');
	PERFORM set_config('role', 'lion_nc_yes', true);
	b := b || ', ' || CASE WHEN nc_try('EXECUTE nc_p') = answer THEN 'the answer'
						   ELSE nc_try('EXECUTE nc_p') END;
	RETURN NEXT 'a cached plan (' || b || ') without it: ' || a;
	PERFORM set_config('role', 'none', true);
	DEALLOCATE nc_p;

	-- SECURITY DEFINER: the owner's privileges, whoever calls
	PERFORM set_config('role', 'lion_nc_no', true);
	a := nc_def_yes(n.q);
	PERFORM set_config('role', 'lion_nc_yes', true);
	b := nc_def_no(n.q);
	RETURN NEXT 'SECURITY DEFINER, owned by the role with it: ' ||
		CASE WHEN a = answer THEN 'the answer' ELSE a END ||
		'; by the role without it: ' || b;

	PERFORM set_config('role', 'none', true);
	EXECUTE format('REVOKE EXECUTE ON FUNCTION %s FROM lion_nc_yes', n.fn);
	EXECUTE format('GRANT EXECUTE ON FUNCTION %s TO PUBLIC', n.fn);
END $$;

SELECT node, nc_check(node) FROM nc_nodes ORDER BY node;

-- nothing is left revoked from PUBLIC by this file
SELECT fn FROM nc_nodes WHERE NOT has_function_privilege('public', fn, 'EXECUTE');

DROP FUNCTION nc_check(text);
DROP FUNCTION nc_vs(text, text);
DROP FUNCTION nc_def_yes(text);
DROP FUNCTION nc_def_no(text);
DROP FUNCTION nc_explain(text, text);
DROP FUNCTION nc_try(text);
DROP FUNCTION nc_rows(text);
DROP FUNCTION nc_paths(text, boolean);
DROP TABLE nc_nodes, nc, ncd;
DROP ROLE lion_nc_yes;
DROP ROLE lion_nc_no;
