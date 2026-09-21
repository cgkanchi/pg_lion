-- case-insensitive keys through the pg_lion_citext extension
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
CREATE EXTENSION citext;
CREATE EXTENSION pg_lion_citext;
-- async commits leave pages non-all-visible for VACUUM; the cost model then prefers the seqscan
SET synchronous_commit = on;
CREATE TABLE lion_ci (id int, name citext NOT NULL);
INSERT INTO lion_ci SELECT g, (ARRAY['Alice','BOB','carol','Alice ','bob'])[1 + g % 5] FROM generate_series(1, 5000) g;
CREATE INDEX lion_ci_name ON lion_ci USING lion (name);
VACUUM ANALYZE lion_ci;
SET enable_seqscan = off;
-- 'alice' matches 'Alice' (case-insensitive) but not 'Alice ' (trailing space)
SELECT count(*) FROM lion_ci WHERE name = 'alice';
SELECT count(*) FROM lion_ci WHERE name = 'BoB';
SELECT count(*) FROM lion_ci WHERE name = 'nobody';
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_ci WHERE name = 'CAROL';
SET pg_lion.enable_count_pushdown = off;
SELECT count(*) FROM lion_ci WHERE name = 'CAROL';
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_ci WHERE name = 'CAROL';
RESET pg_lion.enable_count_pushdown;
-- distinct keys: 'Alice' and 'Alice ' are different, 'BOB'/'bob' are one key
SELECT name, count(*) FROM lion_ci GROUP BY name ORDER BY name;
SELECT entries FROM lion_index_stats('lion_ci_name');
SELECT lion_index_verify('lion_ci_name', true);
INSERT INTO lion_ci VALUES (0, 'aLiCe');
SELECT count(*) FROM lion_ci WHERE name = 'ALICE';
DROP TABLE lion_ci;

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
CREATE TABLE lion_cirep (name citext NOT NULL);
INSERT INTO lion_cirep VALUES ('SecretOldSpelling');
CREATE INDEX lion_cirep_name ON lion_cirep USING lion (name);
DELETE FROM lion_cirep;
INSERT INTO lion_cirep SELECT 'secretoldspelling' FROM generate_series(1, 10000);
VACUUM ANALYZE lion_cirep;
-- one entry, and the key in it is the spelling that was deleted
SELECT entries FROM lion_index_stats('lion_cirep_name');
EXPLAIN (COSTS OFF) SELECT name, count(*) FROM lion_cirep GROUP BY name;
SELECT name, count(*) FROM lion_cirep GROUP BY name;
SELECT left(name::text, 1) AS first_letter, count(*) FROM lion_cirep GROUP BY name;
-- a column a clause pins is printed from the same key, and steps aside too
EXPLAIN (COSTS OFF)
SELECT name, count(*) FROM lion_cirep WHERE name = 'SECRETOLDSPELLING' GROUP BY name;
SELECT name, count(*) FROM lion_cirep WHERE name = 'SECRETOLDSPELLING' GROUP BY name;
-- counting needs no representative: still the custom node
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_cirep WHERE name = 'SECRETOLDSPELLING';
SELECT count(*) FROM lion_cirep WHERE name = 'SECRETOLDSPELLING';
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_cirep GROUP BY name;
SELECT count(*) FROM lion_cirep GROUP BY name;
RESET enable_seqscan;
DROP TABLE lion_cirep;
DROP EXTENSION pg_lion_citext;
DROP EXTENSION citext;
