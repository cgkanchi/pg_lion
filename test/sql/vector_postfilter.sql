-- pg_lion.vector_postfilter (DESIGN.md §44.6, "Post-filtering").
--
-- A lionvec scan ends a pause, and skips a row found too late, by the rows it
-- returns; a Filter above it rejects most of those afterwards, so the plain
-- scan with a Filter and LionOrdered's restricted scan answer the same query
-- with different rows at the same margin and patience.  The plan must not
-- decide the answer.  Under exact and under error, the default, a filter
-- lion indexes answer goes through LionOrdered; any other fails at plan time
-- under error, with a hint that names the subquery and the settings, and
-- under exact gets an exact plan (a sort of the filter's rows); allow lets
-- the plain scan with its Filter compete, as before.
--
-- The plans are shown with the sequential and bitmap scans and the sort
-- disabled, which leaves the plain scan with its Filter the cheapest plan
-- where it is a candidate (allow, at the end).  Each query is also run and
-- its rows compared with a sort of the filter's rows; pg_lion.vector_margin
-- is 100 and the pause off, as in vector.sql, so every plan is exact here and
-- any difference is a wrong answer.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_lion_vector;
RESET client_min_messages;
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;
SET pg_lion.vector_epsilon = 6;
SET pg_lion.vector_margin = 100;
SET pg_lion.vector_patience = 0;

CREATE FUNCTION pf_vec(seed bigint, n int, scale float8 DEFAULT 1.0)
RETURNS vector LANGUAGE sql IMMUTABLE AS $$
	SELECT array_agg(((hashint8extended(seed * 1000 + j, 43) % 100000)::float8
					  / 100000.0 * scale)::real ORDER BY j)::vector
	  FROM generate_series(1, n) j
$$;

/*
 * pf_plan() prints q's plan, each vector literal as '...', and without the
 * line PostgreSQL 18 adds under a node it had to take though disabled.
 */
CREATE FUNCTION pf_plan(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln !~ '^\s*Disabled' THEN
			RETURN NEXT regexp_replace(ln, '''\[[^]]*\]''::vector', '''...''', 'g');
		END IF;
	END LOOP;
END $$;

/*
 * pf_cmp() runs q - which returns columns id and d, ORDER BY d - as planned
 * under the settings in force, and as a sort of the filter's rows (no index
 * scan, LionOrdered off, and pg_lion.vector_postfilter exact, which error
 * would otherwise turn into an error), and compares the distances in order
 * (to 4 places) and the rows with a distance, sorted; and says what the
 * first plan scanned the vectors with: LionOrdered, the lionvec index
 * itself, or a sort.
 */
CREATE FUNCTION pf_cmp(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	r record;
	how text := 'a sort';
	ad text[] := '{}';
	bd text[] := '{}';
	ai int[] := '{}';
	bi int[] := '{}';
	saved text[] := ARRAY[current_setting('enable_seqscan'),
						  current_setting('enable_bitmapscan'),
						  current_setting('enable_indexscan'),
						  current_setting('enable_sort'),
						  current_setting('pg_lion.enable_ordered_scan'),
						  current_setting('pg_lion.vector_postfilter')];
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(LionOrdered\)' THEN
			how := 'LionOrdered';
		ELSIF ln ~ 'Index Scan using pf_(l2|n1)' AND how <> 'LionOrdered' THEN
			how := 'the lionvec index';
		END IF;
	END LOOP;
	FOR r IN EXECUTE q LOOP
		ad := ad || coalesce(round(r.d::numeric, 4)::text, 'NULL');
		IF r.d IS NOT NULL THEN
			ai := ai || r.id;
		END IF;
	END LOOP;

	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_sort', 'on', true);
	PERFORM set_config('pg_lion.enable_ordered_scan', 'off', true);
	PERFORM set_config('pg_lion.vector_postfilter', 'exact', true);
	FOR r IN EXECUTE q LOOP
		bd := bd || coalesce(round(r.d::numeric, 4)::text, 'NULL');
		IF r.d IS NOT NULL THEN
			bi := bi || r.id;
		END IF;
	END LOOP;
	PERFORM set_config('enable_seqscan', saved[1], true);
	PERFORM set_config('enable_bitmapscan', saved[2], true);
	PERFORM set_config('enable_indexscan', saved[3], true);
	PERFORM set_config('enable_sort', saved[4], true);
	PERFORM set_config('pg_lion.enable_ordered_scan', saved[5], true);
	PERFORM set_config('pg_lion.vector_postfilter', saved[6], true);

	IF ad IS DISTINCT FROM bd THEN
		RETURN format('MISMATCH: %s rows, %s by the sort; distances %s / %s',
					  coalesce(array_length(ad, 1), 0), coalesce(array_length(bd, 1), 0),
					  ad[1:3], bd[1:3]);
	END IF;
	IF (SELECT array_agg(x ORDER BY x) FROM unnest(ai) x) IS DISTINCT FROM
	   (SELECT array_agg(x ORDER BY x) FROM unnest(bi) x) THEN
		RETURN 'MISMATCH: other rows at the same distances';
	END IF;
	RETURN format('%s: the sort''s %s rows', how, coalesce(array_length(ad, 1), 0));
END $$;

-- 4,000 rows in ten clusters, indexed in 20 partitions; cat and tag have
-- lion indexes, note has none
CREATE TABLE pf (
	id int PRIMARY KEY,
	cat int,			-- 10 values, the cluster
	tag int,			-- 100 values, independent of cat
	note text,			-- 7 values
	v vector(24)
) WITH (autovacuum_enabled = off);
INSERT INTO pf
SELECT i, i % 10,
	   ((hashint8extended(i::bigint, 7) & 9223372036854775807) % 100)::int,
	   'n' || (i % 7),
	   pf_vec(-(i % 10) - 1, 24) + pf_vec(i, 24, 0.3)
  FROM generate_series(1, 4000) i;
UPDATE pf SET v = NULL WHERE id % 211 = 0;
CREATE INDEX pf_l2 ON pf USING lionvec (v) WITH (partitions = 20);
CREATE INDEX pf_cat ON pf USING lion (cat);
CREATE INDEX pf_tag ON pf USING lion (tag);
VACUUM (FREEZE, ANALYZE) pf;
-- the default, error, shown once an index has loaded the library (CREATE
-- EXTENSION IF NOT EXISTS does not, where an earlier test created it)
SHOW pg_lion.vector_postfilter;

SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_sort = off;

-- exact.  A filter lion indexes answer: LionOrdered; for two columns, over a
-- set of both, though a bitmap scan would filter the less selective one
-- instead (DESIGN.md §44.6, "Post-filtering")
SET pg_lion.vector_postfilter = exact;
SELECT * FROM pf_plan('SELECT id FROM pf WHERE cat = 3 ORDER BY v <-> pf_vec(4, 24) LIMIT 10');
SELECT * FROM pf_plan('SELECT id FROM pf WHERE cat IN (1, 2) AND tag < 50
					   ORDER BY v <-> pf_vec(4, 24) LIMIT 10');
-- a filter on a column no lion index has: a sort of its rows, never the
-- lionvec scan with a Filter, though the sort is disabled
SELECT * FROM pf_plan('SELECT id FROM pf WHERE note = ''n1'' ORDER BY v <-> pf_vec(4, 24) LIMIT 10');
-- ... and with a clause lion answers beside it: a sort of the lion scan's
-- rows, not LionOrdered with a Filter of its own
SELECT * FROM pf_plan('SELECT id FROM pf WHERE cat = 3 AND note = ''n1''
					   ORDER BY v <-> pf_vec(4, 24) LIMIT 10');
-- a key: the one row, sorted
SELECT * FROM pf_plan('SELECT id FROM pf WHERE id = 5 ORDER BY v <-> pf_vec(4, 24) LIMIT 1');
-- the vector column's own test is a filter like any other
SELECT * FROM pf_plan('SELECT id FROM pf WHERE v IS NOT NULL ORDER BY v <-> pf_vec(4, 24) LIMIT 10');
-- no filter, or one a gating Result tests once: the lionvec scan
SELECT * FROM pf_plan('SELECT id FROM pf ORDER BY v <-> pf_vec(4, 24) LIMIT 10');
SELECT * FROM pf_plan('SELECT id FROM pf WHERE current_user IS NOT NULL
					   ORDER BY v <-> pf_vec(4, 24) LIMIT 10');
-- with LionOrdered off, a lion filter is sorted too: the setting still
-- keeps the lionvec scan's rows from being filtered
SET pg_lion.enable_ordered_scan = off;
SELECT * FROM pf_plan('SELECT id FROM pf WHERE cat = 3 ORDER BY v <-> pf_vec(4, 24) LIMIT 10');
RESET pg_lion.enable_ordered_scan;

-- ... and their answers are the sort's
CREATE TABLE pf_cases (f text, k text);
INSERT INTO pf_cases VALUES
	('cat = 3', 'LIMIT 10'), ('cat = 3', ''), ('cat IN (1, 2) AND tag < 50', 'LIMIT 10'),
	('tag = 7 OR cat = 4', 'LIMIT 10'), ('note = ''n1''', 'LIMIT 10'),
	('cat = 3 AND note = ''n1''', 'LIMIT 10'), ('cat = 3 AND note = ''n1''', ''),
	('id = 5', 'LIMIT 1'), ('v IS NOT NULL', 'LIMIT 10'), ('true', 'LIMIT 10');
SELECT f, k, pf_cmp(format('SELECT id, v <-> pf_vec(4, 24) AS d FROM pf WHERE %s
							ORDER BY v <-> pf_vec(4, 24) %s', f, k)) AS result
  FROM pf_cases ORDER BY f, k;
SET pg_lion.enable_ordered_scan = off;
SELECT pf_cmp('SELECT id, v <-> pf_vec(4, 24) AS d FROM pf WHERE cat = 3
			   ORDER BY v <-> pf_vec(4, 24) LIMIT 10');
RESET pg_lion.enable_ordered_scan;

-- error, the default: a query whose lionvec order would be filtered fails at
-- plan time, EXPLAIN included, and says how to write it, and how to have
-- the exact plan or the filtered scan instead
RESET pg_lion.vector_postfilter;
\set VERBOSITY default
EXPLAIN (COSTS OFF)
SELECT id FROM pf WHERE note = 'n1' ORDER BY v <-> pf_vec(4, 24) LIMIT 10;
SELECT id FROM pf WHERE cat = 3 AND note = 'n1' ORDER BY v <-> pf_vec(4, 24) LIMIT 10;
SET pg_lion.enable_ordered_scan = off;
SELECT id FROM pf WHERE cat = 3 ORDER BY v <-> pf_vec(4, 24) LIMIT 10;
RESET pg_lion.enable_ordered_scan;
\set VERBOSITY terse
-- a key, and the vector column's own test, are filters like any other
SELECT id FROM pf WHERE id = 5 ORDER BY v <-> pf_vec(4, 24) LIMIT 1;
SELECT id FROM pf WHERE v IS NOT NULL ORDER BY v <-> pf_vec(4, 24) LIMIT 10;
-- a filter lion answers is LionOrdered's, and no filter is no error, nor
-- one the planner tests once - a generic plan's Param too
SELECT * FROM pf_plan('SELECT id FROM pf WHERE cat = 3 ORDER BY v <-> pf_vec(4, 24) LIMIT 10');
SELECT pf_cmp('SELECT id, v <-> pf_vec(4, 24) AS d FROM pf WHERE cat IN (1, 2) AND tag < 50
			   ORDER BY v <-> pf_vec(4, 24) LIMIT 10');
SELECT pf_cmp('SELECT id, v <-> pf_vec(4, 24) AS d FROM pf ORDER BY v <-> pf_vec(4, 24) LIMIT 10');
SELECT pf_cmp('SELECT id, v <-> pf_vec(4, 24) AS d FROM pf WHERE current_user IS NOT NULL
			   ORDER BY v <-> pf_vec(4, 24) LIMIT 10');
PREPARE pf_once(bool) AS SELECT id FROM pf WHERE $1 ORDER BY v <-> pf_vec(4, 24) LIMIT 10;
SET plan_cache_mode = force_generic_plan;
SELECT * FROM pf_plan('EXECUTE pf_once(true)');
RESET plan_cache_mode;
DEALLOCATE pf_once;
-- error is the default's value
SET pg_lion.vector_postfilter = error;
SELECT id FROM pf WHERE note = 'n1' ORDER BY v <-> pf_vec(4, 24) LIMIT 10;
RESET pg_lion.vector_postfilter;
-- the subquery the hint suggests: the nearest 1,000 rows of the scan
-- (restricted by the clauses lion answers, or not at all), then the other
-- clauses; the planner never pushes a clause below a LIMIT
SELECT * FROM pf_plan('SELECT id FROM (SELECT id, note, v <-> pf_vec(4, 24) AS d FROM pf WHERE cat = 3
										ORDER BY d LIMIT 1000) s
					   WHERE note = ''n1'' ORDER BY d LIMIT 10');
SELECT pf_cmp('SELECT id, d FROM (SELECT id, note, v <-> pf_vec(4, 24) AS d FROM pf WHERE cat = 3
								  ORDER BY d LIMIT 1000) s
			   WHERE note = ''n1'' ORDER BY d LIMIT 10');
SELECT * FROM pf_plan('SELECT id FROM (SELECT id, note, v <-> pf_vec(4, 24) AS d FROM pf
										ORDER BY d LIMIT 1000) s
					   WHERE note = ''n1'' ORDER BY d LIMIT 10');
SELECT pf_cmp('SELECT id, d FROM (SELECT id, note, v <-> pf_vec(4, 24) AS d FROM pf
								  ORDER BY d LIMIT 1000) s
			   WHERE note = ''n1'' ORDER BY d LIMIT 10');
-- a partial lionvec index whose predicate implies the filter is the
-- filter's order: no row it returns is filtered
CREATE INDEX pf_n1 ON pf USING lionvec (v) WITH (partitions = 4) WHERE note = 'n1';
SELECT * FROM pf_plan('SELECT id FROM pf WHERE note = ''n1'' ORDER BY v <-> pf_vec(4, 24) LIMIT 10');
SELECT pf_cmp('SELECT id, v <-> pf_vec(4, 24) AS d FROM pf WHERE note = ''n1''
			   ORDER BY v <-> pf_vec(4, 24) LIMIT 10');
-- with a clause more, which lion answers, the partial index's scan would be
-- filtered, and LionOrdered, whose set must answer every clause, the
-- predicate's too, is not offered: the error names that clause
\set VERBOSITY default
SELECT id FROM pf WHERE note = 'n1' AND tag < 50 ORDER BY v <-> pf_vec(4, 24) LIMIT 10;
\set VERBOSITY terse
DROP INDEX pf_n1;

-- row-level security: LionOrdered does not scan such a table (DESIGN.md
-- §30.1), and the policy's clause is a filter, so under the default even a
-- query with no WHERE fails, and the subquery the hint suggests elsewhere
-- does too: the policy filters its scan as well.  The table's owner, whom
-- the policy does not apply to, is not filtered.
CREATE ROLE pf_reader;
GRANT SELECT ON pf TO pf_reader;
ALTER TABLE pf ENABLE ROW LEVEL SECURITY;
CREATE POLICY pf_not5 ON pf FOR SELECT USING (cat <> 5);
SELECT * FROM pf_plan('SELECT id FROM pf ORDER BY v <-> pf_vec(4, 24) LIMIT 10');
SET ROLE pf_reader;
\set VERBOSITY default
SELECT id FROM pf ORDER BY v <-> pf_vec(4, 24) LIMIT 10;
\set VERBOSITY terse
SELECT id FROM (SELECT id, note, v <-> pf_vec(4, 24) AS d FROM pf ORDER BY d LIMIT 1000) s
 WHERE note = 'n1' ORDER BY d LIMIT 10;
SET pg_lion.vector_postfilter = exact;
SELECT * FROM pf_plan('SELECT id FROM pf ORDER BY v <-> pf_vec(4, 24) LIMIT 10');
SELECT count(*) AS rows, count(*) FILTER (WHERE cat = 5) AS cat5
  FROM (SELECT id, cat FROM pf ORDER BY v <-> pf_vec(4, 24) LIMIT 10) s;
RESET ROLE;
DROP POLICY pf_not5 ON pf;
ALTER TABLE pf DISABLE ROW LEVEL SECURITY;
REVOKE SELECT ON pf FROM pf_reader;
DROP ROLE pf_reader;

-- allow: the lionvec scan with a Filter is a plan again, the cheapest here,
-- and LionOrdered may filter its members
SET pg_lion.vector_postfilter = allow;
SELECT * FROM pf_plan('SELECT id FROM pf WHERE note = ''n1'' ORDER BY v <-> pf_vec(4, 24) LIMIT 10');
SELECT * FROM pf_plan('SELECT id FROM pf WHERE cat = 3 AND note = ''n1''
					   ORDER BY v <-> pf_vec(4, 24) LIMIT 10');
SET pg_lion.enable_ordered_scan = off;
SELECT * FROM pf_plan('SELECT id FROM pf WHERE cat = 3 ORDER BY v <-> pf_vec(4, 24) LIMIT 10');
RESET pg_lion.enable_ordered_scan;
SELECT f, k, pf_cmp(format('SELECT id, v <-> pf_vec(4, 24) AS d FROM pf WHERE %s
							ORDER BY v <-> pf_vec(4, 24) %s', f, k)) AS result
  FROM pf_cases WHERE f LIKE '%note%' ORDER BY f, k;
RESET pg_lion.vector_postfilter;

RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_sort;
DROP TABLE pf, pf_cases;
DROP FUNCTION pf_cmp(text);
DROP FUNCTION pf_plan(text);
DROP FUNCTION pf_vec(bigint, int, float8);
