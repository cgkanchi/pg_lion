-- EXECUTE privileges on the functions lion's paths stand in for.  LionCount
-- replaces an Agg and a scan's quals, the SQL count functions replace a whole
-- query, and LionBm25 replaces the lion_bm25_score() calls above it, so each
-- must ask for the EXECUTE privileges the executor would have asked for on
-- the plan it replaces - the aggregate, the operators of the WHERE clause,
-- the grouping equality, the join operator, the score - no more and no less
-- (DESIGN.md §9, "Privileges").
--
-- Revoking a function from PUBLIC affects the whole database, so
-- every REVOKE here is paired with a GRANT a few lines below it, and the file
-- ends by checking that nothing is left revoked.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET synchronous_commit = on;
CREATE ROLE lion_exec_user;
CREATE ROLE lion_exec_other;

-- nullable columns (holding no NULL), so that PostgreSQL 19+ does not turn
-- count(k) into count(*) before either plan sees it
CREATE TABLE lion_ex (k int, g int, t text);
INSERT INTO lion_ex SELECT i % 10, i % 3, 't' || (i % 4) FROM generate_series(1, 3000) i;
CREATE INDEX lion_ex_k ON lion_ex USING lion (k);
CREATE INDEX lion_ex_g ON lion_ex USING lion (g);
CREATE INDEX lion_ex_t ON lion_ex USING lion (t);

-- the dimension of an FK-side join (DESIGN.md §27) on lion_ex.k
CREATE TABLE lion_exd (pk int PRIMARY KEY, name text NOT NULL);
INSERT INTO lion_exd SELECT i, 'n' || (i % 2) FROM generate_series(0, 9) i;

-- a partitioned table, partitioned on a column no query below constrains
CREATE TABLE lion_exp (p int NOT NULL, k int NOT NULL, g int NOT NULL) PARTITION BY RANGE (p);
CREATE TABLE lion_exp_1 PARTITION OF lion_exp FOR VALUES FROM (0) TO (2);
CREATE TABLE lion_exp_2 PARTITION OF lion_exp FOR VALUES FROM (2) TO (4);
INSERT INTO lion_exp SELECT i % 4, i % 10, i % 3 FROM generate_series(1, 3000) i;
CREATE INDEX lion_exp_k ON lion_exp USING lion (k);
CREATE INDEX lion_exp_g ON lion_exp USING lion (g);

VACUUM (FREEZE, ANALYZE) lion_ex;
VACUUM (FREEZE, ANALYZE) lion_exd;
VACUUM (FREEZE, ANALYZE) lion_exp;
GRANT SELECT ON lion_ex, lion_exd, lion_exp TO lion_exec_user, lion_exec_other;

/*
 * lion_ex_try() runs a query and returns its rows as one sorted string, or
 * the error it raised.  lion_ex() says whether the pushdown plans the query
 * (asked as the caller, who holds every privilege), then runs it as role r
 * through the pushdown and through the ordinary plan, and prints the first
 * answer and whether the second is the same.
 */
CREATE FUNCTION lion_ex_try(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	r text;
BEGIN
	EXECUTE format('SELECT coalesce(string_agg(s::text, '' '' ORDER BY s::text), ''(none)'') FROM (%s) s', q)
		INTO r;
	RETURN r;
EXCEPTION WHEN insufficient_privilege THEN
	RETURN 'ERROR: ' || SQLERRM;
END $$;

CREATE FUNCTION lion_ex(q text, r text DEFAULT 'lion_exec_user') RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	pushed boolean := false;
	ron text;
	roff text;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			pushed := true;
		END IF;
	END LOOP;
	PERFORM set_config('role', r, true);
	ron := lion_ex_try(q);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	/* the ordinary plan gets every join method back */
	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_nestloop', 'on', true);
	roff := lion_ex_try(q);
	RETURN CASE WHEN pushed THEN 'pushed: ' ELSE 'NOT PUSHED: ' END || ron ||
		CASE WHEN ron = roff THEN ' (as the ordinary plan)'
			 ELSE ' | the ordinary plan: ' || roff END;
END $$;

-- ---------- the WHERE clause's operator: int4eq ----------
REVOKE EXECUTE ON FUNCTION int4eq(int4, int4) FROM PUBLIC;
SELECT lion_ex('SELECT count(*) FROM lion_ex WHERE k = 1');
SELECT lion_ex('SELECT count(*) FROM lion_ex WHERE k = ANY (''{1,2}'')');
SELECT lion_ex('SELECT count(*) FROM lion_ex WHERE k IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10)');
SELECT lion_ex('SELECT count(*) FROM lion_ex WHERE k = 1 OR t = ''t1''');
SELECT lion_ex('SELECT count(*) FROM lion_ex WHERE t = ''t1'' OR (t = ''t2'' AND g = 2)');
SELECT lion_ex('SELECT count(*) FROM lion_ex WHERE t = ''t1''');      -- texteq only: allowed
SELECT lion_ex('SELECT count(*) FROM lion_ex WHERE k = 1::int8');     -- int48eq: allowed
-- the grouping equality, as HashAggregate and GroupAggregate check it
SELECT lion_ex('SELECT g, count(*) FROM lion_ex GROUP BY g');
SELECT lion_ex('SELECT t, count(*) FROM lion_ex GROUP BY t');         -- allowed
SELECT lion_ex('SELECT t, g, count(*) FROM lion_ex GROUP BY t, g');
SELECT lion_ex('SELECT t, count(*) FROM lion_ex WHERE g = 1 GROUP BY t');
-- count(DISTINCT k) compares k with nodeAgg's unchecked equalfnOne, so core
-- runs it without int4eq; so does the pushdown, and with a GROUP BY neither
SELECT lion_ex('SELECT count(DISTINCT k) FROM lion_ex');
SELECT lion_ex('SELECT t, count(DISTINCT k) FROM lion_ex GROUP BY t');
SELECT lion_ex('SELECT g, count(DISTINCT k) FROM lion_ex GROUP BY g');
SELECT lion_ex('SELECT count(DISTINCT t) FROM lion_ex WHERE k = 2');
-- a partitioned table: the clause runs in every partition
SELECT lion_ex('SELECT count(*) FROM lion_exp WHERE k = 1');
SELECT lion_ex('SELECT g, count(*) FROM lion_exp GROUP BY g');
-- the FK-side join eliminates the join clause's operator; forced, so that the
-- node is what runs, against the ordinary join
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
SELECT lion_ex('SELECT count(*) FROM lion_ex f JOIN lion_exd d ON f.k = d.pk');
SELECT lion_ex('SELECT d.name, count(*) FROM lion_ex f JOIN lion_exd d ON f.k = d.pk GROUP BY d.name');
SELECT lion_ex('SELECT d.name, count(*) FROM lion_ex f JOIN lion_exd d ON f.k = d.pk WHERE f.t = ''t1'' GROUP BY d.name');
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_nestloop;
-- plain EXPLAIN initialises the plan, which asks for the privilege too
SET ROLE lion_exec_user;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_ex WHERE k = 1;
-- ... except the grouping equality, which HashAggregate only checks when it runs
EXPLAIN (COSTS OFF) SELECT g, count(*) FROM lion_ex GROUP BY g;
-- the SQL count functions ask what `SELECT count(*) FROM t WHERE col = key` does
SELECT lion_index_count('lion_ex_k', 1);
SELECT lion_index_count('lion_ex_g', 1, 'lion_ex_k', 1);
SELECT count FROM lion_index_count_stats('lion_ex_k', 1);
SELECT lion_index_count_any('lion_ex_k', ARRAY[1, 2]);
SELECT groups, count FROM lion_index_count_group_stats('lion_ex_g');
SELECT lion_index_count('lion_ex_k', 1::int8);                         -- int48eq: allowed
SELECT groups, count FROM lion_index_count_group_stats('lion_ex_t');   -- allowed
RESET ROLE;
GRANT EXECUTE ON FUNCTION int4eq(int4, int4) TO PUBLIC;

-- ---------- the equality of a key the count resolves: enum_eq, texteq ----------
-- An enum key on enum_ops is looked up with the class's (anyenum, anyenum)
-- member, and a varchar key on a text_ops column with (text, text), which the
-- parser reaches by relabelling the key: the functions `col = key` calls, so
-- the ones the count asks EXECUTE on (test/sql/keytypes.sql, 2026-09-25).
CREATE TYPE lion_ex_mood AS ENUM ('sad', 'ok', 'happy');
CREATE TABLE lion_exe (e lion_ex_mood, v varchar);
INSERT INTO lion_exe
SELECT (enum_range(NULL::lion_ex_mood))[1 + i % 3], 'v' || (i % 4) FROM generate_series(1, 300) i;
CREATE INDEX lion_exe_e ON lion_exe USING lion (e);
CREATE INDEX lion_exe_v ON lion_exe USING lion (v);
VACUUM (FREEZE, ANALYZE) lion_exe;
GRANT SELECT ON lion_exe TO lion_exec_user;
REVOKE EXECUTE ON FUNCTION enum_eq(anyenum, anyenum) FROM PUBLIC;
SELECT lion_ex('SELECT count(*) FROM lion_exe WHERE e = ''ok''');
SET ROLE lion_exec_user;
SELECT lion_index_count('lion_exe_e', 'ok'::lion_ex_mood);
SELECT lion_index_count_any('lion_exe_e', '{ok, sad}'::lion_ex_mood[]);
SELECT lion_index_count('lion_exe_v', 'v1'::varchar);                  -- texteq: allowed
RESET ROLE;
GRANT EXECUTE ON FUNCTION enum_eq(anyenum, anyenum) TO PUBLIC;
-- lion_ex() itself compares text, so these run by hand
REVOKE EXECUTE ON FUNCTION texteq(text, text) FROM PUBLIC;
SET ROLE lion_exec_user;
SELECT count(*) FROM lion_exe WHERE v = 'v1'::varchar;
SELECT lion_index_count('lion_exe_v', 'v1'::varchar);
SELECT lion_index_count_any('lion_exe_v', '{v1, v2}'::varchar[]);
SELECT lion_index_count('lion_exe_e', 'ok'::lion_ex_mood);             -- enum_eq: allowed
RESET ROLE;
GRANT EXECUTE ON FUNCTION texteq(text, text) TO PUBLIC;
SET ROLE lion_exec_user;
SELECT lion_index_count('lion_exe_v', 'v1'::varchar) = (SELECT count(*) FROM lion_exe WHERE v = 'v1') AS ok;
SELECT lion_index_count('lion_exe_e', 'ok'::lion_ex_mood) = (SELECT count(*) FROM lion_exe WHERE e = 'ok') AS ok;
RESET ROLE;
DROP TABLE lion_exe;
DROP TYPE lion_ex_mood;

-- ---------- a range's operators (DESIGN.md §28): int4lt ----------
REVOKE EXECUTE ON FUNCTION int4lt(int4, int4) FROM PUBLIC;
SELECT lion_ex('SELECT count(*) FROM lion_ex WHERE k < 5');
SELECT lion_ex('SELECT count(*) FROM lion_ex WHERE 5 > k');                -- int4gt, as written: allowed
SELECT lion_ex('SELECT count(*) FROM lion_ex WHERE k >= 2 AND k < 5 AND g = 1');
SELECT lion_ex('SELECT k, count(*) FROM lion_ex WHERE k < 5 GROUP BY k');
SELECT lion_ex('SELECT count(DISTINCT k) FROM lion_ex WHERE k < 5');
SELECT lion_ex('SELECT count(*) FROM lion_exp WHERE k < 5');
SELECT lion_ex('SELECT count(*) FROM lion_ex WHERE k BETWEEN 2 AND 5');  -- int4ge, int4le: allowed
SELECT lion_ex('SELECT count(*) FROM lion_ex WHERE k < 5::int8');        -- int48lt: allowed
SET ROLE lion_exec_user;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_ex WHERE k < 5;
RESET ROLE;
GRANT EXECUTE ON FUNCTION int4lt(int4, int4) TO PUBLIC;

-- ---------- a hashed IN list is checked for its hash function as well ----------
REVOKE EXECUTE ON FUNCTION hashint4(int4) FROM PUBLIC;
SELECT lion_ex('SELECT count(*) FROM lion_ex WHERE k IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10)');
SELECT lion_ex('SELECT count(*) FROM lion_ex WHERE k IN (1, 2)');      -- not hashed: allowed
GRANT EXECUTE ON FUNCTION hashint4(int4) TO PUBLIC;

-- ---------- the aggregates: count() and count("any") ----------
REVOKE EXECUTE ON FUNCTION count() FROM PUBLIC;
SELECT lion_ex('SELECT count(*) FROM lion_ex WHERE k = 1');
SELECT lion_ex('SELECT count(k) FROM lion_ex WHERE k = 1');             -- count("any"): allowed
SELECT lion_ex('SELECT g, count(*) FROM lion_ex GROUP BY g');
SELECT lion_ex('SELECT g, count(g) FROM lion_ex GROUP BY g HAVING count(*) > 0');
SELECT lion_ex('SELECT count(*) FROM lion_exp WHERE k = 1');
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
SELECT lion_ex('SELECT d.name, count(*) FROM lion_ex f JOIN lion_exd d ON f.k = d.pk GROUP BY d.name');
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_nestloop;
SET ROLE lion_exec_user;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_ex WHERE k = 1;
SELECT lion_index_count('lion_ex_k', 1);
SELECT lion_index_count_any('lion_ex_k', ARRAY[1, 2]);
SELECT groups, count FROM lion_index_count_group_stats('lion_ex_g');
RESET ROLE;
GRANT EXECUTE ON FUNCTION count() TO PUBLIC;

REVOKE EXECUTE ON FUNCTION count("any") FROM PUBLIC;
SELECT lion_ex('SELECT count(DISTINCT k) FROM lion_ex');
SELECT lion_ex('SELECT t, count(DISTINCT k) FROM lion_ex GROUP BY t');
SELECT lion_ex('SELECT count(k) FROM lion_ex WHERE k = 1');
SELECT lion_ex('SELECT count(*) FROM lion_ex WHERE k = 1');             -- count(): allowed
GRANT EXECUTE ON FUNCTION count("any") TO PUBLIC;

-- ---------- cached plans and role changes: checked when the plan starts ----------
SET plan_cache_mode = force_generic_plan;
SET ROLE lion_exec_user;
PREPARE lion_ex_q(int) AS SELECT count(*) FROM lion_ex WHERE k = $1;
PREPARE lion_ex_qg AS SELECT g, count(*) FROM lion_ex GROUP BY g ORDER BY g;
EXPLAIN (COSTS OFF) EXECUTE lion_ex_q(1);
EXPLAIN (COSTS OFF) EXECUTE lion_ex_qg;
EXECUTE lion_ex_q(1);
EXECUTE lion_ex_qg;
RESET ROLE;
REVOKE EXECUTE ON FUNCTION int4eq(int4, int4) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION int4eq(int4, int4) TO lion_exec_other;
SET ROLE lion_exec_user;
EXECUTE lion_ex_q(1);                   -- the plan prepared before the REVOKE
EXECUTE lion_ex_qg;
SET ROLE lion_exec_other;
EXECUTE lion_ex_q(1);                   -- same statement, a role that holds it
EXECUTE lion_ex_qg;
SET ROLE lion_exec_user;
EXECUTE lion_ex_q(1);
SET ROLE lion_exec_other;
EXECUTE lion_ex_q(1);
RESET ROLE;
-- a SECURITY DEFINER function runs with its owner's privileges
CREATE FUNCTION lion_ex_def_other(int) RETURNS bigint SECURITY DEFINER
LANGUAGE sql AS 'SELECT count(*) FROM lion_ex WHERE k = $1';
ALTER FUNCTION lion_ex_def_other(int) OWNER TO lion_exec_other;
CREATE FUNCTION lion_ex_def_user(int) RETURNS bigint SECURITY DEFINER
LANGUAGE sql AS 'SELECT count(*) FROM lion_ex WHERE k = $1';
ALTER FUNCTION lion_ex_def_user(int) OWNER TO lion_exec_user;
SET ROLE lion_exec_user;
SELECT lion_ex_def_other(1);            -- owner holds int4eq
SELECT lion_ex_def_user(1);
SET ROLE lion_exec_other;
SELECT lion_ex_def_user(1);             -- owner does not
SELECT lion_ex_def_other(1);
RESET ROLE;
REVOKE EXECUTE ON FUNCTION int4eq(int4, int4) FROM lion_exec_other;
GRANT EXECUTE ON FUNCTION int4eq(int4, int4) TO PUBLIC;
-- granted again: the same prepared plans answer
SET ROLE lion_exec_user;
EXECUTE lion_ex_q(1);
EXECUTE lion_ex_qg;
RESET ROLE;
DEALLOCATE lion_ex_q;
DEALLOCATE lion_ex_qg;
RESET plan_cache_mode;

-- ---------- the LionBm25 scan's score: lion_bm25_score() ----------
-- The scan returns the score as a column of its tuple, which setrefs.c puts
-- in place of every copy of the call above it, so ExecInitFunc() never sees
-- the call: the scan asks EXECUTE on it itself, every time it is initialised.
CREATE TABLE lion_exb (id int, d tsvector);
INSERT INTO lion_exb
SELECT i, to_tsvector('simple', 'w' || (i % 5) || ' w' || (i % 7) || ' w' || (i % 11))
  FROM generate_series(1, 2000) i;
CREATE INDEX lion_exb_d ON lion_exb USING lion (d) WITH (store_positions = true);
VACUUM (FREEZE, ANALYZE) lion_exb;
GRANT SELECT ON lion_exb TO lion_exec_user, lion_exec_other;

-- lion_ex() for the scan: is it planned (the other scans off, since the
-- planner rightly sorts a table this small), then role r's answer through it
-- and through the ordinary plan
CREATE FUNCTION lion_exb(q text, r text DEFAULT 'lion_exec_user') RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	scanned boolean := false;
	ron text;
	roff text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (LionBm25)%' THEN
			scanned := true;
		END IF;
	END LOOP;
	PERFORM set_config('role', r, true);
	ron := lion_ex_try(q);
	PERFORM set_config('pg_lion.enable_bm25_scan', 'off', true);
	roff := lion_ex_try(q);
	RETURN CASE WHEN scanned THEN 'scanned: ' ELSE 'NOT SCANNED: ' END || ron ||
		CASE WHEN ron = roff THEN ' (as the ordinary plan)'
			 ELSE ' | the ordinary plan: ' || roff END;
END $$;

REVOKE EXECUTE ON FUNCTION lion_bm25_score(tsvector, tsquery, regclass, float8, float8) FROM PUBLIC;
SELECT lion_exb('SELECT id FROM lion_exb WHERE d @@ ''w1 & w3 & w4''
	ORDER BY lion_bm25_score(d, ''w1 & w3 & w4'', ''lion_exb_d'') DESC');
-- the score shown as well, which the scan's column stands in for too
SELECT lion_exb('SELECT id, lion_bm25_score(d, ''w1 & w3 & w4'', ''lion_exb_d'') FROM lion_exb
	WHERE d @@ ''w1 & w3 & w4'' ORDER BY lion_bm25_score(d, ''w1 & w3 & w4'', ''lion_exb_d'') DESC');
-- granted to one role: that one may, the other may not
GRANT EXECUTE ON FUNCTION lion_bm25_score(tsvector, tsquery, regclass, float8, float8) TO lion_exec_user;
SELECT lion_exb('SELECT id FROM lion_exb WHERE d @@ ''w1 & w3 & w4''
	ORDER BY lion_bm25_score(d, ''w1 & w3 & w4'', ''lion_exb_d'') DESC');
SELECT lion_exb('SELECT id FROM lion_exb WHERE d @@ ''w1 & w3 & w4''
	ORDER BY lion_bm25_score(d, ''w1 & w3 & w4'', ''lion_exb_d'') DESC', 'lion_exec_other');
-- plain EXPLAIN initialises the plan, which asks for the privilege too
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SET ROLE lion_exec_other;
EXPLAIN (COSTS OFF) SELECT id FROM lion_exb WHERE d @@ 'w1 & w3 & w4'
	ORDER BY lion_bm25_score(d, 'w1 & w3 & w4', 'lion_exb_d') DESC LIMIT 5;
RESET ROLE;
-- a cached plan, made by a role that may call the function, run by one that
-- may not: the privilege is asked when the plan runs, not when it was made
SET plan_cache_mode = force_generic_plan;
PREPARE lion_exb_q AS SELECT count(*) FROM (SELECT id FROM lion_exb WHERE d @@ 'w1 & w3 & w4'
	ORDER BY lion_bm25_score(d, 'w1 & w3 & w4', 'lion_exb_d') DESC LIMIT 5) s;
EXPLAIN (COSTS OFF) EXECUTE lion_exb_q;
EXECUTE lion_exb_q;
SET ROLE lion_exec_other;
EXECUTE lion_exb_q;
SET ROLE lion_exec_user;
EXECUTE lion_exb_q;
RESET ROLE;
DEALLOCATE lion_exb_q;
RESET plan_cache_mode;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
REVOKE EXECUTE ON FUNCTION lion_bm25_score(tsvector, tsquery, regclass, float8, float8) FROM lion_exec_user;
GRANT EXECUTE ON FUNCTION lion_bm25_score(tsvector, tsquery, regclass, float8, float8) TO PUBLIC;
-- granted again: every role, and the same rows as the ordinary plan
SELECT lion_exb('SELECT id FROM lion_exb WHERE d @@ ''w1 & w3 & w4''
	ORDER BY lion_bm25_score(d, ''w1 & w3 & w4'', ''lion_exb_d'') DESC', 'lion_exec_other');

-- ---------- positive controls: every privilege back, every answer exact ----------
SELECT lion_ex('SELECT count(*) FROM lion_ex WHERE k = 1');
SELECT lion_ex('SELECT count(*) FROM lion_ex WHERE k IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10)');
SELECT lion_ex('SELECT count(*) FROM lion_ex WHERE k = 1 OR t = ''t1''');
SELECT lion_ex('SELECT g, count(*) FROM lion_ex GROUP BY g');
SELECT lion_ex('SELECT t, g, count(*) FROM lion_ex GROUP BY t, g');
SELECT lion_ex('SELECT g, count(DISTINCT k) FROM lion_ex GROUP BY g');
SELECT lion_ex('SELECT g, count(*) FROM lion_exp WHERE k = 1 GROUP BY g');
SELECT lion_ex('SELECT count(*) FROM lion_ex WHERE k < 5 AND g = 1');
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
SELECT lion_ex('SELECT d.name, count(*) FROM lion_ex f JOIN lion_exd d ON f.k = d.pk GROUP BY d.name');
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_nestloop;
SET ROLE lion_exec_user;
SELECT lion_index_count('lion_ex_k', 1) = (SELECT count(*) FROM lion_ex WHERE k = 1) AS ok;
SELECT lion_index_count('lion_ex_g', 1, 'lion_ex_k', 1) = (SELECT count(*) FROM lion_ex WHERE g = 1 AND k = 1) AS ok;
SELECT lion_index_count_any('lion_ex_k', ARRAY[1, 2]) = (SELECT count(*) FROM lion_ex WHERE k IN (1, 2)) AS ok;
SELECT groups, count FROM lion_index_count_group_stats('lion_ex_g');
RESET ROLE;

-- nothing is left revoked from PUBLIC by this file
SELECT p.oid::regprocedure
  FROM pg_proc p
 WHERE p.oid IN ('int4eq(int4,int4)'::regprocedure, 'int4lt(int4,int4)'::regprocedure,
                 'hashint4(int4)'::regprocedure,
                 'enum_eq(anyenum,anyenum)'::regprocedure, 'texteq(text,text)'::regprocedure,
                 'count()'::regprocedure, 'count("any")'::regprocedure,
                 'lion_bm25_score(tsvector,tsquery,regclass,float8,float8)'::regprocedure)
   AND NOT has_function_privilege('public', p.oid, 'EXECUTE');

DROP FUNCTION lion_ex_def_other(int);
DROP FUNCTION lion_ex_def_user(int);
DROP FUNCTION lion_exb(text, text);
DROP FUNCTION lion_ex(text, text);
DROP FUNCTION lion_ex_try(text);
DROP TABLE lion_ex, lion_exd, lion_exp, lion_exb;
DROP ROLE lion_exec_user;
DROP ROLE lion_exec_other;
