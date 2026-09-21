-- case-insensitive keys through the roaring_index_citext extension
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS roaring_index;
RESET client_min_messages;
CREATE EXTENSION citext;
CREATE EXTENSION roaring_index_citext;
-- async commits leave pages non-all-visible for VACUUM; the cost model then prefers the seqscan
SET synchronous_commit = on;
CREATE TABLE rbi_ci (id int, name citext NOT NULL);
INSERT INTO rbi_ci SELECT g, (ARRAY['Alice','BOB','carol','Alice ','bob'])[1 + g % 5] FROM generate_series(1, 5000) g;
CREATE INDEX rbi_ci_name ON rbi_ci USING roaring (name);
VACUUM ANALYZE rbi_ci;
SET enable_seqscan = off;
-- 'alice' matches 'Alice' (case-insensitive) but not 'Alice ' (trailing space)
SELECT count(*) FROM rbi_ci WHERE name = 'alice';
SELECT count(*) FROM rbi_ci WHERE name = 'BoB';
SELECT count(*) FROM rbi_ci WHERE name = 'nobody';
EXPLAIN (COSTS OFF) SELECT count(*) FROM rbi_ci WHERE name = 'CAROL';
SET roaring_index.enable_count_pushdown = off;
SELECT count(*) FROM rbi_ci WHERE name = 'CAROL';
EXPLAIN (COSTS OFF) SELECT count(*) FROM rbi_ci WHERE name = 'CAROL';
RESET roaring_index.enable_count_pushdown;
-- distinct keys: 'Alice' and 'Alice ' are different, 'BOB'/'bob' are one key
SELECT name, count(*) FROM rbi_ci GROUP BY name ORDER BY name;
SELECT entries FROM roaring_index_stats('rbi_ci_name');
SELECT roaring_index_verify('rbi_ci_name', true);
INSERT INTO rbi_ci VALUES (0, 'aLiCe');
SELECT count(*) FROM rbi_ci WHERE name = 'ALICE';
DROP TABLE rbi_ci;
DROP EXTENSION roaring_index_citext;
DROP EXTENSION citext;
