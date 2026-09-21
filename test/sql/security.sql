-- Access control, row-level security and collation rules of the count paths
-- (findings of the 2026-09-20 adversarial review).
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS roaring_index;
RESET client_min_messages;
SET synchronous_commit = on;
CREATE ROLE rbi_sec_reader;
CREATE ROLE rbi_sec_other;
CREATE TABLE rbi_sec (id int, tenant text NOT NULL, secret text NOT NULL, k int NOT NULL);
INSERT INTO rbi_sec SELECT g, 't' || (g % 3), 's' || (g % 7), g % 5 FROM generate_series(1, 3000) g;
CREATE INDEX rbi_sec_secret ON rbi_sec USING roaring (secret);
CREATE INDEX rbi_sec_k ON rbi_sec USING roaring (k);
CREATE INDEX rbi_sec_tenant ON rbi_sec USING roaring (tenant);
VACUUM ANALYZE rbi_sec;
-- ---------- privileges: the SQL count functions need SELECT like the query would ----------
SET ROLE rbi_sec_reader;
SELECT roaring_index_count('rbi_sec_secret', 's1'::text);           -- no privilege at all
SELECT roaring_index_count('rbi_sec_tenant', 't1'::text, 'rbi_sec_secret', 's1'::text);
SELECT * FROM roaring_index_count_stats('rbi_sec_secret', 's1'::text);
RESET ROLE;
GRANT SELECT (k) ON rbi_sec TO rbi_sec_reader;              -- column privilege on k only
SET ROLE rbi_sec_reader;
SELECT roaring_index_count('rbi_sec_k', 1);                    -- allowed: only k is referenced
SELECT roaring_index_count('rbi_sec_secret', 's1'::text);           -- still denied: secret is not granted
SELECT roaring_index_count('rbi_sec_tenant', 't1'::text, 'rbi_sec_secret', 's1'::text);   -- denied: tenant and secret are not granted
RESET ROLE;
GRANT SELECT ON rbi_sec TO rbi_sec_reader;
SET ROLE rbi_sec_reader;
SELECT roaring_index_count('rbi_sec_secret', 's1'::text) = (SELECT count(*) FROM rbi_sec WHERE secret = 's1') AS ok;
RESET ROLE;
-- ---------- row-level security: the functions refuse, the planner path stays correct ----------
GRANT SELECT ON rbi_sec TO rbi_sec_other;
ALTER TABLE rbi_sec ENABLE ROW LEVEL SECURITY;
CREATE POLICY rbi_sec_t1 ON rbi_sec FOR SELECT TO rbi_sec_other USING (tenant = 't1');
SET ROLE rbi_sec_other;
SELECT roaring_index_count('rbi_sec_k', 1);                    -- refused: RLS applies to this role
SELECT count(*) FROM rbi_sec WHERE k = 1;                      -- policy-filtered answer
SELECT count(*) = (SELECT count(*) FROM rbi_sec WHERE k = 1 AND tenant = 't1') AS policy_applied
  FROM rbi_sec WHERE k = 1;
EXPLAIN (COSTS OFF) SELECT count(*) FROM rbi_sec WHERE k = 1;  -- no RoaringCount under security quals
RESET ROLE;
SELECT roaring_index_count('rbi_sec_k', 1) = (SELECT count(*) FROM rbi_sec WHERE k = 1) AS owner_bypasses_rls;
ALTER TABLE rbi_sec DISABLE ROW LEVEL SECURITY;
-- ---------- collation: an index built under another collation is not used by the pushdown ----------
CREATE TABLE rbi_coll (id int, name text NOT NULL, k int NOT NULL);
INSERT INTO rbi_coll SELECT g, (ARRAY['a','B','c'])[1 + g % 3], g % 4 FROM generate_series(1, 3000) g;
CREATE INDEX rbi_coll_name_c ON rbi_coll USING roaring (name COLLATE "C");
CREATE INDEX rbi_coll_k ON rbi_coll USING roaring (k);
VACUUM ANALYZE rbi_coll;
SET enable_seqscan = off;
-- clause collation (default) differs from the index collation ("C"): no pushdown, no index use
EXPLAIN (COSTS OFF) SELECT count(*) FROM rbi_coll WHERE name = 'B';
SELECT count(*) FROM rbi_coll WHERE name = 'B';
-- matching collation: pushed down
EXPLAIN (COSTS OFF) SELECT count(*) FROM rbi_coll WHERE name = 'B' COLLATE "C";
SELECT count(*) FROM rbi_coll WHERE name = 'B' COLLATE "C";
-- GROUP BY under the column's collation cannot drive from the "C" index
EXPLAIN (COSTS OFF) SELECT name, count(*) FROM rbi_coll GROUP BY name;
-- a collation-insensitive column is unaffected
EXPLAIN (COSTS OFF) SELECT count(*) FROM rbi_coll WHERE k = 2;
RESET enable_seqscan;
DROP TABLE rbi_coll;
DROP TABLE rbi_sec;
DROP ROLE rbi_sec_reader;
DROP ROLE rbi_sec_other;
