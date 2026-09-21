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

-- ---- the stored key is one representative of an equality class ----------
/*
 * An entry keeps the spelling it was created with, for the whole class.  Once
 * every row of that spelling is gone the stored key is a value no visible row
 * holds, so a pushdown that would PRINT it - the GROUP BY column, or a column
 * a WHERE clause pins - has to step aside and let the ordinary aggregate read
 * the value off a live row (the 2026-09-20 review, finding 4).  Counting the
 * class is exact whatever it is spelled like, and is still pushed down.
 */
RESET enable_seqscan;
CREATE TABLE rbi_cirep (name citext NOT NULL);
INSERT INTO rbi_cirep VALUES ('SecretOldSpelling');
CREATE INDEX rbi_cirep_name ON rbi_cirep USING roaring (name);
DELETE FROM rbi_cirep;
INSERT INTO rbi_cirep SELECT 'secretoldspelling' FROM generate_series(1, 10000);
VACUUM ANALYZE rbi_cirep;
-- one entry, and the key in it is the spelling that was deleted
SELECT entries FROM roaring_index_stats('rbi_cirep_name');
EXPLAIN (COSTS OFF) SELECT name, count(*) FROM rbi_cirep GROUP BY name;
SELECT name, count(*) FROM rbi_cirep GROUP BY name;
SELECT left(name::text, 1) AS first_letter, count(*) FROM rbi_cirep GROUP BY name;
-- a column a clause pins is printed from the same key, and steps aside too
EXPLAIN (COSTS OFF)
SELECT name, count(*) FROM rbi_cirep WHERE name = 'SECRETOLDSPELLING' GROUP BY name;
SELECT name, count(*) FROM rbi_cirep WHERE name = 'SECRETOLDSPELLING' GROUP BY name;
-- counting needs no representative: still the custom node
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM rbi_cirep WHERE name = 'SECRETOLDSPELLING';
SELECT count(*) FROM rbi_cirep WHERE name = 'SECRETOLDSPELLING';
EXPLAIN (COSTS OFF) SELECT count(*) FROM rbi_cirep GROUP BY name;
SELECT count(*) FROM rbi_cirep GROUP BY name;
RESET enable_seqscan;
DROP TABLE rbi_cirep;
DROP EXTENSION roaring_index_citext;
DROP EXTENSION citext;
