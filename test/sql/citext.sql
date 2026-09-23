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

/*
 * The sorted directory of DESIGN.md §21 orders entries by support proc 4, and
 * citext's is citext_cmp(), which returns 0 for two spellings that differ only
 * in case.  So 'Alice' and 'alice' TIE in the order and the descent's run scan
 * has to find the one entry they share by the opclass EQUALITY - if it stopped
 * at the bytewise tail of the comparison instead, each spelling would get an
 * entry of its own and the counts would split.  Four spellings of one name,
 * inserted in an order that makes each of them the one the entry might have
 * been created with: still ONE entry, and one group.
 */
CREATE TABLE lion_cimerge (name citext NOT NULL);
CREATE INDEX lion_cimerge_name ON lion_cimerge USING lion (name);
INSERT INTO lion_cimerge
	SELECT (ARRAY['Alice','alice','ALICE','aLiCe'])[1 + g % 4]
	  FROM generate_series(1, 400) g;
SELECT ordered FROM lion_index_stats('lion_cimerge_name');
SELECT entries AS one_entry_for_four_spellings, ntids
  FROM lion_index_stats('lion_cimerge_name');
SELECT count(*) AS one_group FROM (SELECT name FROM lion_cimerge GROUP BY name) g;
SELECT count(*) FROM lion_cimerge WHERE name = 'ALICE';
SELECT count(*) FROM lion_cimerge WHERE name = 'alice';
SELECT lion_index_verify('lion_cimerge_name', true);
-- the same through a bulk build, which groups by the same equality
REINDEX INDEX lion_cimerge_name;
SELECT entries AS one_entry_after_reindex FROM lion_index_stats('lion_cimerge_name');
SELECT lion_index_verify('lion_cimerge_name', true);
DROP TABLE lion_cimerge;

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
-- counting needs no representative: still the custom node
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM lion_cirep WHERE name = 'SECRETOLDSPELLING';
SELECT count(*) FROM lion_cirep WHERE name = 'SECRETOLDSPELLING';
SELECT * FROM lion_explain_norm('SELECT count(*) FROM lion_cirep GROUP BY name') AS p("QUERY PLAN");
SELECT count(*) FROM lion_cirep GROUP BY name;
RESET enable_seqscan;
DROP TABLE lion_cirep;
DROP EXTENSION pg_lion_citext;
DROP EXTENSION citext;
