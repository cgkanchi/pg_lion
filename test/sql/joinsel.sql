-- A join clause lion prices as a parameterized index clause
-- (lion_rel_clause()): `c.fk = i.fk` beside a filter on another column of the
-- same lion index.  Estimated with varRelid 0 and no join, the clause is a
-- restriction with no constant, DEFAULT_EQ_SEL (1/200), which core caches in
-- the query's RestrictInfo; the join's size estimate read that back, a join of
-- 40 rows to a unique key came out at 40 x 6,667 / 200 = 1,333 rows.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

-- jsl.fk is unique; flag has three values; 40 rows of jsl_i have grp = 7,
-- each fk of jsl once
CREATE TABLE jsl (fk int NOT NULL, a int NOT NULL, flag int NOT NULL);
INSERT INTO jsl SELECT i, 1, i % 3 FROM generate_series(1, 20000) i;
CREATE INDEX jsl_fk ON jsl (fk);
CREATE INDEX jsl_lion ON jsl USING lion (a, flag, fk);
CREATE TABLE jsl_i (fk int NOT NULL, grp int NOT NULL);
INSERT INTO jsl_i SELECT 1 + (i * 37) % 20000, i % 125 FROM generate_series(1, 5000) i;
CREATE TABLE jsl_d (id int PRIMARY KEY);
INSERT INTO jsl_d SELECT i FROM generate_series(1, 20000) i;
VACUUM (FREEZE, ANALYZE) jsl, jsl_i, jsl_d;

-- the planner's row estimate of q's top node, and its actual rows
CREATE FUNCTION jsl_est(q text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	j	json;
BEGIN
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	RETURN (j->0->'Plan'->>'Plan Rows')::bigint;
END $$;
CREATE FUNCTION jsl_actual(q text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	n	bigint;
BEGIN
	EXECUTE 'SELECT count(*) FROM (' || q || ') s' INTO n;
	RETURN n;
END $$;

-- two tables, and three where the clause is one of an equivalence class's
\set q2 'SELECT 1 FROM jsl_i i JOIN jsl c ON c.fk = i.fk WHERE i.grp = 7 AND c.flag = 1'
\set q3 'SELECT 1 FROM jsl_d d JOIN jsl c ON c.fk = d.id AND c.a = 1 JOIN jsl_i i ON i.fk = d.id AND i.grp = 7'
SELECT jsl_actual(:'q2') AS actual, jsl_est(:'q2') BETWEEN 1 AND 40 AS est_ok;
SELECT jsl_actual(:'q3') AS actual, jsl_est(:'q3') BETWEEN 20 AND 80 AS est_ok;

DROP TABLE jsl, jsl_i, jsl_d;
DROP FUNCTION jsl_est(text), jsl_actual(text);
