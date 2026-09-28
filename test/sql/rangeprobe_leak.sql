-- The endpoint probe runs core's binary search over a column's histogram
-- with the range clause's own operator (DESIGN.md §28, "The endpoint
-- probe"), and so hands the histogram's values - rows of the table - to that
-- operator's function.  Core's own search asks statistic_proc_security_check()
-- first: where the user may not read every row of the column, only a
-- leakproof operator is given them.  The probe asked nothing, so a
-- non-leakproof operator saw the histogram of a table whose row-level
-- policy hid those rows from the user planning the query.
--
-- lpl_ge() is int4's >= that counts its calls in a setting.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET max_parallel_workers_per_gather = 0;

CREATE FUNCTION lpl_ge(a int4, b int4) RETURNS bool
LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('lpl.calls',
					   (current_setting('lpl.calls')::int + 1)::text, false);
	RETURN a >= b;
END $$;
CREATE OPERATOR >=% (LEFTARG = int4, RIGHTARG = int4, FUNCTION = lpl_ge);
-- in a btree family with int4's own <, which the histogram is sorted by, so
-- that the histogram may be searched with it (comparison_ops_are_compatible())
CREATE OPERATOR FAMILY lpl_bt USING btree;
ALTER OPERATOR FAMILY lpl_bt USING btree ADD
	OPERATOR 1 < (int4, int4),
	OPERATOR 4 >=% (int4, int4);
-- and lion's >= (strategy 8) of an operator class of its own
CREATE OPERATOR CLASS lpl_ops FOR TYPE int4 USING lion AS
	OPERATOR 1 = (int4, int4),
	OPERATOR 8 >=% (int4, int4),
	FUNCTION 1 hashint4(int4),
	FUNCTION 4 btint4cmp(int4, int4);

CREATE TABLE lpl (id int4) WITH (autovacuum_enabled = off);
INSERT INTO lpl SELECT g FROM generate_series(1, 1000) g;
CREATE INDEX lpl_i ON lpl USING lion (id lpl_ops);
ANALYZE lpl;
CREATE ROLE lion_lpl_reader;
GRANT SELECT ON lpl TO lion_lpl_reader;
ALTER TABLE lpl ENABLE ROW LEVEL SECURITY;
CREATE POLICY lpl_p ON lpl FOR SELECT TO lion_lpl_reader USING (id <= 100);

/* How many times planning q called lpl_ge(). */
CREATE FUNCTION lpl_calls(q text) RETURNS int
LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('lpl.calls', '0', false);
	EXECUTE 'EXPLAIN ' || q;
	RETURN current_setting('lpl.calls')::int;
END $$;

-- The table's owner reads every row: the probe searches the histogram, 5000
-- being past its end.
SELECT lpl_calls('SELECT count(*) FROM lpl WHERE id >=% 5000') > 0 AS searched;
-- The reader sees only the rows the policy lets through, and the operator is
-- not leakproof: the probe does not search the histogram, as core's own
-- search would not.
SET ROLE lion_lpl_reader;
SELECT lpl_calls('SELECT count(*) FROM lpl WHERE id >=% 5000') AS calls;
RESET ROLE;
-- A leakproof operator is given the histogram whoever plans the query, by
-- core and by the probe.
ALTER FUNCTION lpl_ge(int4, int4) LEAKPROOF;
SET ROLE lion_lpl_reader;
SELECT lpl_calls('SELECT count(*) FROM lpl WHERE id >=% 5000') > 0 AS searched;
RESET ROLE;

DROP TABLE lpl;
DROP ROLE lion_lpl_reader;
DROP FUNCTION lpl_calls(text);
DROP OPERATOR CLASS lpl_ops USING lion;
DROP OPERATOR FAMILY lpl_ops USING lion;
DROP OPERATOR FAMILY lpl_bt USING btree;
DROP OPERATOR >=% (int4, int4);
DROP FUNCTION lpl_ge(int4, int4);
