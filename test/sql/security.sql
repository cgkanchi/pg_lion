-- Access control, row-level security and collation rules of the count paths
-- (findings of the 2026-09-20 adversarial review).
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET synchronous_commit = on;
CREATE ROLE lion_sec_reader;
CREATE ROLE lion_sec_other;
CREATE TABLE lion_sec (id int, tenant text NOT NULL, secret text NOT NULL, k int NOT NULL);
INSERT INTO lion_sec SELECT g, 't' || (g % 3), 's' || (g % 7), g % 5 FROM generate_series(1, 3000) g;
CREATE INDEX lion_sec_secret ON lion_sec USING lion (secret);
CREATE INDEX lion_sec_k ON lion_sec USING lion (k);
CREATE INDEX lion_sec_tenant ON lion_sec USING lion (tenant);
VACUUM ANALYZE lion_sec;
-- ---------- privileges: the SQL count functions need SELECT like the query would ----------
SET ROLE lion_sec_reader;
SELECT lion_index_count('lion_sec_secret', 's1'::text);           -- no privilege at all
SELECT lion_index_count('lion_sec_tenant', 't1'::text, 'lion_sec_secret', 's1'::text);
SELECT * FROM lion_index_count_stats('lion_sec_secret', 's1'::text);
RESET ROLE;
GRANT SELECT (k) ON lion_sec TO lion_sec_reader;              -- column privilege on k only
SET ROLE lion_sec_reader;
SELECT lion_index_count('lion_sec_k', 1);                    -- allowed: only k is referenced
SELECT lion_index_count('lion_sec_secret', 's1'::text);           -- still denied: secret is not granted
SELECT lion_index_count('lion_sec_tenant', 't1'::text, 'lion_sec_secret', 's1'::text);   -- denied: tenant and secret are not granted
RESET ROLE;
GRANT SELECT ON lion_sec TO lion_sec_reader;
SET ROLE lion_sec_reader;
SELECT lion_index_count('lion_sec_secret', 's1'::text) = (SELECT count(*) FROM lion_sec WHERE secret = 's1') AS ok;
RESET ROLE;
-- ---------- row-level security: the functions refuse, the planner path stays correct ----------
GRANT SELECT ON lion_sec TO lion_sec_other;
ALTER TABLE lion_sec ENABLE ROW LEVEL SECURITY;
CREATE POLICY lion_sec_t1 ON lion_sec FOR SELECT TO lion_sec_other USING (tenant = 't1');
SET ROLE lion_sec_other;
SELECT lion_index_count('lion_sec_k', 1);                    -- refused: RLS applies to this role
SELECT count(*) FROM lion_sec WHERE k = 1;                      -- policy-filtered answer
SELECT count(*) = (SELECT count(*) FROM lion_sec WHERE k = 1 AND tenant = 't1') AS policy_applied
  FROM lion_sec WHERE k = 1;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_sec WHERE k = 1;  -- no LionCount under security quals
RESET ROLE;
SELECT lion_index_count('lion_sec_k', 1) = (SELECT count(*) FROM lion_sec WHERE k = 1) AS owner_bypasses_rls;
ALTER TABLE lion_sec DISABLE ROW LEVEL SECURITY;
-- ---------- collation: an index built under another collation is not used by the pushdown ----------
CREATE TABLE lion_coll (id int, name text NOT NULL, k int NOT NULL);
INSERT INTO lion_coll SELECT g, (ARRAY['a','B','c'])[1 + g % 3], g % 4 FROM generate_series(1, 3000) g;
CREATE INDEX lion_coll_name_c ON lion_coll USING lion (name COLLATE "C");
CREATE INDEX lion_coll_k ON lion_coll USING lion (k);
VACUUM ANALYZE lion_coll;
-- EXPLAIN in a form every supported release prints the same way: subplans
-- are named "expr_N" as in PostgreSQL 19 ("N" before), 18's "Disabled: true"
-- lines are dropped, actual row counts are integers (18 adds ".00"), and
-- sorting is off while it plans, so that a query the pushdown must refuse gets
-- the same core plan whether disabled paths are counted (18) or priced (16,
-- 17).  LionCount never sorts, so this cannot hide it.
CREATE OR REPLACE FUNCTION lion_explain_norm(q text, opts text DEFAULT 'COSTS OFF')
RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	l text;
BEGIN
	PERFORM set_config('enable_sort', 'off', true);
	FOR l IN EXECUTE 'EXPLAIN (' || opts || ') ' || q LOOP
		CONTINUE WHEN l ~ '^\s*Disabled: true$';
		l := regexp_replace(l, '(InitPlan|SubPlan) (\d+)', '\1 expr_\2', 'g');
		RETURN NEXT regexp_replace(l, 'rows=(\d+)\.00 ', 'rows=\1 ', 'g');
	END LOOP;
	PERFORM set_config('enable_sort', 'on', true);
END
$$;
SET enable_seqscan = off;
-- clause collation (default) differs from the index collation ("C"): no pushdown, no index use
SELECT * FROM lion_explain_norm($q$SELECT count(*) FROM lion_coll WHERE name = 'B'$q$) AS p("QUERY PLAN");
SELECT count(*) FROM lion_coll WHERE name = 'B';
-- matching collation: pushed down
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_coll WHERE name = 'B' COLLATE "C";
SELECT count(*) FROM lion_coll WHERE name = 'B' COLLATE "C";
-- GROUP BY under the column's collation cannot drive from the "C" index
SELECT * FROM lion_explain_norm('SELECT name, count(*) FROM lion_coll GROUP BY name') AS p("QUERY PLAN");
-- a collation-insensitive column is unaffected
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_coll WHERE k = 2;
RESET enable_seqscan;
DROP TABLE lion_coll;
DROP TABLE lion_sec;
DROP ROLE lion_sec_reader;
DROP ROLE lion_sec_other;
