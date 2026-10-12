-- Facet counts: lion_index_facets(idx, k, filter) (DESIGN.md §7, "Facet
-- counts").
--
-- The k keys of a key column with the most rows the snapshot sees, under a
-- filter made of what lion indexes answer: for a multi-key column a key is a
-- tag, a lexeme or a jsonb path key and its count the rows whose value holds
-- it, which is
--   SELECT e, count(DISTINCT ctid) FROM t, unnest(col) e WHERE <filter>
--    GROUP BY e ORDER BY 2 DESC, e LIMIT k
-- and for a scalar column `GROUP BY col`.  Every answer is checked against
-- that query, in order: the top k with its ties broken by the key, every key
-- when k is NULL.  A filter that keeps few rows is answered from those rows
-- (the plain-column indexes below); an expression index and a partial one
-- always count by the keys, so each answer is made both ways.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET synchronous_commit = on;

-- tags repeat an element and hold NULL elements; NULL and empty arrays and
-- tsvectors; a NULL group
CREATE TABLE lfac (id int, grp int, tags text[], tsv tsvector, nums int[],
				   doc jsonb, pad text)
	WITH (autovacuum_enabled = off);
INSERT INTO lfac
SELECT i, CASE WHEN i % 17 = 0 THEN NULL ELSE i % 6 END,
	CASE WHEN i % 23 = 0 THEN NULL
		 WHEN i % 19 = 0 THEN '{}'::text[]
		 ELSE ARRAY['a' || (i % 7), 'b' || (i * 7 % 40), 'c' || (i * 13 % 300),
					'a' || (i % 7), CASE WHEN i % 11 = 0 THEN NULL ELSE 'd' END] END,
	CASE WHEN i % 29 = 0 THEN NULL
		 WHEN i % 31 = 0 THEN ''::tsvector
		 ELSE to_tsvector('simple', 'w' || (i % 5) || ' x' || (i * 3 % 70) ||
						  ' y' || (i * 11 % 250)) END,
	ARRAY[i % 9, i % 4, i * 7 % 120],
	jsonb_build_object('cat', 'c' || (i % 3), 'n', i % 4,
					   'tags', jsonb_build_array('t' || (i % 5), 't' || (i % 2)))
		|| CASE WHEN i % 2 = 1 THEN '{"odd": true}'::jsonb ELSE '{}'::jsonb END,
	'p'
FROM generate_series(1, 3000) i;
CREATE INDEX lfac_grp ON lfac USING lion (grp);
CREATE INDEX lfac_tags ON lfac USING lion (tags);
-- one that stores positions files every row under an empty length key too,
-- which is no lexeme
CREATE INDEX lfac_tsv ON lfac USING lion (tsv) WITH (store_positions = true);
CREATE INDEX lfac_nums ON lfac USING lion (nums);
CREATE INDEX lfac_doc ON lfac USING lion (doc jsonb_contains_ops);
-- the same keys through an expression and through a partial index, which
-- never read the filter's rows from the heap
CREATE INDEX lfac_tags_x ON lfac USING lion ((tags || '{}'::text[]));
CREATE INDEX lfac_tsv_p ON lfac USING lion (tsv) WHERE id > 0;
CREATE INDEX lfac_grp_x ON lfac USING lion ((grp + 0));
VACUUM ANALYZE lfac;

/*
 * lfac_check(idx, k, filter, expected) compares lion_index_facets(idx, k,
 * filter) with expected, a query of (k, n) rows over every key, ordered by n
 * descending and then k and cut at k.  It says 'same, N rows', or both
 * answers.
 */
CREATE FUNCTION lfac_check(idx text, k int, filter text, expected text)
RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	a text;
	b text;
	n int;
BEGIN
	SELECT string_agg(coalesce(f.key, '<null>') || ':' || f.count, ' ' ORDER BY f.r), count(*)
	  INTO a, n
	  FROM lion_index_facets(idx::regclass, k, filter) WITH ORDINALITY AS f(key, count, r);
	EXECUTE format($q$SELECT string_agg(coalesce(k::text, '<null>') || ':' || n, ' ' ORDER BY n DESC, k NULLS LAST)
	  FROM (SELECT * FROM (%s) e ORDER BY n DESC, k NULLS LAST LIMIT %s) e$q$,
	  expected, coalesce(k::text, 'ALL')) INTO b;
	IF a IS NOT DISTINCT FROM b THEN
		RETURN 'same, ' || n || ' rows';
	END IF;
	RETURN 'lion: ' || coalesce(a, '(none)') || ' | query: ' || coalesce(b, '(none)');
END $$;

-- the queries the facets stand for, with a WHERE
CREATE FUNCTION lfac_tags_q(w text) RETURNS text LANGUAGE sql AS $$
	SELECT format('SELECT e AS k, count(DISTINCT id) AS n FROM lfac, unnest(tags) e WHERE e IS NOT NULL AND (%s) GROUP BY e', w) $$;
CREATE FUNCTION lfac_tsv_q(w text) RETURNS text LANGUAGE sql AS $$
	SELECT format('SELECT word COLLATE "C" AS k, ndoc AS n FROM ts_stat(%L)', 'SELECT tsv FROM lfac WHERE ' || w) $$;
CREATE FUNCTION lfac_nums_q(w text) RETURNS text LANGUAGE sql AS $$
	SELECT format('SELECT e AS k, count(DISTINCT id) AS n FROM lfac, unnest(nums) e WHERE (%s) GROUP BY e', w) $$;
CREATE FUNCTION lfac_grp_q(w text) RETURNS text LANGUAGE sql AS $$
	SELECT format('SELECT grp AS k, count(*) AS n FROM lfac WHERE (%s) GROUP BY grp', w) $$;

/*
 * Each filter against each column, through the plain index and the one that
 * always counts by the keys, for every key and for top k of a few sizes.
 */
CREATE TABLE lfac_filters (f text);
INSERT INTO lfac_filters VALUES
	('true'),
	('grp = 3'),
	('grp IN (1, 2)'),
	('grp IS NULL'),
	('grp = 3::bigint'),
	('3 = grp'),
	($$tsv @@ 'w1 & x3'$$),
	($$tsv @@ 'w1 | x3'$$),
	($$tsv @@ 'y7'$$),
	($$tsv @@ 'x1:*'$$),
	($$tags @> '{a1}'$$),
	($$tags @> '{a1}' OR grp = 4$$),
	($$tags && '{a1,b2}' AND (grp = 1 OR grp = 2)$$),
	($$tags @> '{c7}'$$),
	($$nums @> '{3}' AND tsv @@ 'w4'$$),
	($$doc @> '{"cat": "c1"}'$$),
	($$doc ? 'odd' AND grp = 5$$),
	($$doc @> '{"tags": ["t3"]}' AND tags @> '{d}'$$),
	('grp = 99'),
	('grp = NULL'),
	('false');

CREATE FUNCTION lfac_all(tag text) RETURNS TABLE (filter text, idx text, k int, result text)
LANGUAGE plpgsql AS $$
DECLARE
	f text;
	kk int;
BEGIN
	FOR f IN SELECT lfac_filters.f FROM lfac_filters LOOP
		FOREACH kk IN ARRAY ARRAY[NULL, 1, 3, 10, 1000]::int[] LOOP
			filter := f; k := kk;
			idx := 'lfac_tags';   result := lfac_check(idx, kk, f, lfac_tags_q(f)); RETURN NEXT;
			idx := 'lfac_tags_x'; result := lfac_check(idx, kk, f, lfac_tags_q(f)); RETURN NEXT;
			idx := 'lfac_tsv';    result := lfac_check(idx, kk, f, lfac_tsv_q(f)); RETURN NEXT;
			idx := 'lfac_tsv_p';  result := lfac_check(idx, kk, f, lfac_tsv_q(f)); RETURN NEXT;
			idx := 'lfac_nums';   result := lfac_check(idx, kk, f, lfac_nums_q(f)); RETURN NEXT;
			idx := 'lfac_grp';    result := lfac_check(idx, kk, f, lfac_grp_q(f)); RETURN NEXT;
			idx := 'lfac_grp_x';  result := lfac_check(idx, kk, f, lfac_grp_q(f)); RETURN NEXT;
		END LOOP;
	END LOOP;
END $$;

-- every answer the same as the query's; the differences, if any
SELECT count(*) AS checks, count(*) FILTER (WHERE result LIKE 'same%') AS same
  FROM lfac_all('fresh');
SELECT filter, idx, k, result FROM lfac_all('fresh') WHERE result NOT LIKE 'same%';

-- a few answers in full
SELECT * FROM lion_index_facets('lfac_tags', 5);
SELECT * FROM lion_index_facets('lfac_tags', 4, $$tsv @@ 'w1 & x3' AND grp = 1$$);
SELECT * FROM lion_index_facets('lfac_tsv', 3, 'grp IN (2, 4)');
SELECT * FROM lion_index_facets('lfac_grp', NULL, $$tags @> '{a2}'$$);
-- tags repeat 'aN' in a row: the row is counted once, as count(DISTINCT) does
SELECT key, count, (SELECT count(*) FROM lfac, unnest(tags) e WHERE e = key) AS occurrences
  FROM lion_index_facets('lfac_tags', 2);

-- jsonb_contains_ops keys as jsonpath; each count is that of its `@>` or `?`
SELECT * FROM lion_index_facets('lfac_doc', NULL, 'grp IN (1, 2)');
SELECT f.key, f.count,
	   (SELECT count(*) FROM lfac WHERE grp IN (1, 2) AND doc @> jsonb_build_object('cat', substr(f.key, 13, 2))) AS query
  FROM lion_index_facets('lfac_doc', NULL, 'grp IN (1, 2)') f WHERE f.key LIKE '$."cat" == %';
SELECT count(*) FROM lfac WHERE grp IN (1, 2) AND doc ? 'odd';

/*
 * Deleted and updated rows, without VACUUM: the entries still hold their
 * TIDs, and their ntids with them, but the counts are of what the snapshot
 * sees.  An update of an indexed column moves the row to new keys; one of
 * the pad column alone is a HOT update and moves nothing.
 */
DELETE FROM lfac WHERE id % 10 = 3;
UPDATE lfac SET tags = ARRAY['a1', 'new' || (id % 4)], tsv = to_tsvector('simple', 'w9 fresh'),
				grp = 5 WHERE id % 10 = 5;
UPDATE lfac SET pad = 'q' WHERE id % 10 = 7;
INSERT INTO lfac SELECT 5000 + i, i % 3, ARRAY['a0', 'late'], to_tsvector('simple', 'w0 late'),
	ARRAY[1], '{"cat": "c9"}', 'p' FROM generate_series(1, 40) i;
SELECT count(*) AS checks, count(*) FILTER (WHERE result LIKE 'same%') AS same
  FROM lfac_all('changed');
SELECT filter, idx, k, result FROM lfac_all('changed') WHERE result NOT LIKE 'same%';
-- a key whose every row is gone has no facet, though its entry has TIDs
SELECT count(*) FROM lion_index_facets('lfac_tags') WHERE key = 'c9';
SELECT count(*) FROM lion_index_facets('lfac_tags', NULL, 'grp = 3') WHERE key = 'c9';
SELECT count(*) FROM lfac WHERE tags @> '{c9}';

-- the transaction's own changes, and nothing of one rolled back
BEGIN;
DELETE FROM lfac WHERE grp = 0;
INSERT INTO lfac VALUES (9001, 0, '{a1,mine}', 'w1 mine', '{7}', '{}', 'p');
SELECT lfac_check('lfac_tags', NULL, 'grp = 0', lfac_tags_q('grp = 0'));
SELECT lfac_check('lfac_tags_x', 10, 'true', lfac_tags_q('true'));
SELECT lfac_check('lfac_tsv', NULL, $$tsv @@ 'mine'$$, lfac_tsv_q($$tsv @@ 'mine'$$));
ROLLBACK;
SELECT * FROM lion_index_facets('lfac_tags', NULL, 'grp = 0') WHERE key = 'mine';

-- after VACUUM, the same
VACUUM lfac;
SELECT count(*) AS checks, count(*) FILTER (WHERE result LIKE 'same%') AS same
  FROM lfac_all('vacuumed');

-- k = 0, a NULL index, a NULL k (every key), a NULL filter (none)
SELECT count(*) FROM lion_index_facets('lfac_tags', 0);
SELECT count(*) FROM lion_index_facets(NULL);
SELECT count(*) = (SELECT count(DISTINCT e) FROM lfac, unnest(tags) e) AS every_key
  FROM lion_index_facets('lfac_tags', NULL, NULL);

-- what a filter cannot be: each is refused, never answered by a scan
SELECT * FROM lion_index_facets('lfac_tags', 3, 'grp > 2');
SELECT * FROM lion_index_facets('lfac_tags', 3, 'NOT (grp = 1)');
SELECT * FROM lion_index_facets('lfac_tags', 3, 'id = 1');
SELECT * FROM lion_index_facets('lfac_tags', 3, $$tsv @@ '!w1'$$);
SELECT * FROM lion_index_facets('lfac_tags', 3, $$tsv @@ 'w1 <-> x3'$$);
SELECT * FROM lion_index_facets('lfac_tags', 3, $$tags <@ '{a1,b2}'$$);
SELECT * FROM lion_index_facets('lfac_tags', 3, 'grp IS NOT NULL');
SELECT * FROM lion_index_facets('lfac_tags', 3, 'grp = ALL (ARRAY[1, 2])');
SELECT * FROM lion_index_facets('lfac_tags', 3, 'grp = id');
SELECT * FROM lion_index_facets('lfac_tags', 3, 'grp = (random() * 3)::int');
SELECT * FROM lion_index_facets('lfac_tags', 3, 'grp = (SELECT 1)');
SELECT * FROM lion_index_facets('lfac_tags', 3, 'grp');
SELECT * FROM lion_index_facets('lfac_tags', 3, 'grp = 1; SELECT 1');
SELECT * FROM lion_index_facets('lfac_tags', 3, 'grp = 1 ORDER BY 1');
SELECT * FROM lion_index_facets('lfac_tags', 3, 'grp = 1 UNION SELECT');
SELECT * FROM lion_index_facets('lfac_tags', 3, 'nosuchcolumn = 1');
SELECT * FROM lion_index_facets('lfac_tags', -1);
SELECT * FROM lion_index_facets('lfac_tags', 3, NULL, 2::int2);
SELECT * FROM lion_index_facets('lfac_tags', 3, NULL, 0::int2);
SELECT * FROM lion_index_facets('lfac', 3);
CREATE INDEX lfac_btree ON lfac (grp);
SELECT * FROM lion_index_facets('lfac_btree', 3);
DROP INDEX lfac_btree;
-- the count functions still refuse a multi-key column
SELECT * FROM lion_index_count_group_stats('lfac_tags');

-- a multicolumn index: each key column by its number
CREATE INDEX lfac_mc ON lfac USING lion (grp, tags);
SELECT lfac_check('lfac_mc', 4, $$tsv @@ 'w1'$$, lfac_grp_q($$tsv @@ 'w1'$$));
SELECT (SELECT string_agg(key || ':' || count, ' ' ORDER BY r)
		  FROM lion_index_facets('lfac_mc', 4, $$tsv @@ 'w1'$$, 2::int2)
			   WITH ORDINALITY AS f(key, count, r)) =
	   (SELECT string_agg(e || ':' || n, ' ' ORDER BY n DESC, e)
		  FROM (SELECT e, count(DISTINCT id) AS n FROM lfac, unnest(tags) e
				 WHERE e IS NOT NULL AND tsv @@ 'w1' GROUP BY e
				 ORDER BY 2 DESC, 1 LIMIT 4) x) AS second_column;
DROP INDEX lfac_mc;

-- privileges: SELECT on the facet column and on every column the filter
-- reads, as the query needs them
CREATE ROLE lfac_user;
GRANT SELECT (tags) ON lfac TO lfac_user;
SET ROLE lfac_user;
SELECT count(*) FROM lion_index_facets('lfac_tags', 3);
SELECT count(*) FROM lion_index_facets('lfac_tags', 3, 'grp = 1');
SELECT count(*) FROM lion_index_facets('lfac_grp', 3);
RESET ROLE;
GRANT SELECT (grp) ON lfac TO lfac_user;
SET ROLE lfac_user;
SELECT count(*) FROM lion_index_facets('lfac_tags', 3, 'grp = 1');
RESET ROLE;
-- EXECUTE on what the query calls: the filter's operator
GRANT SELECT (tsv) ON lfac TO lfac_user;
REVOKE EXECUTE ON FUNCTION ts_match_vq(tsvector, tsquery) FROM PUBLIC;
SET ROLE lfac_user;
SELECT count(*) FROM lion_index_facets('lfac_tags', 3, $$tsv @@ 'w1'$$);
RESET ROLE;
GRANT EXECUTE ON FUNCTION ts_match_vq(tsvector, tsquery) TO PUBLIC;
SET ROLE lfac_user;
SELECT count(*) FROM lion_index_facets('lfac_tags', 3, $$tsv @@ 'w1'$$);
RESET ROLE;
REVOKE ALL ON lfac FROM lfac_user;
DROP ROLE lfac_user;

/*
 * Many distinct keys tied at a row each: a top k cannot settle on its
 * candidates, walks every key and keeps only what the top k needs, the keys
 * it drops - past twice k, counted as zero, or never counted - freed as it
 * goes.  Long keys, ties broken by the key, a few keys with two rows, and
 * deleted rows (no VACUUM) that leave keys with a bound but no row.
 */
CREATE TABLE lfac_wide (id int, k text, g int) WITH (autovacuum_enabled = off);
INSERT INTO lfac_wide
SELECT i, repeat(md5(i::text), 8) || i, i % 4 FROM generate_series(1, 5000) i;
INSERT INTO lfac_wide
SELECT 5000 + i, repeat(md5(i::text), 8) || i, i % 4 FROM generate_series(1, 5000, 1250) i;
CREATE INDEX lfac_wide_k ON lfac_wide USING lion (k);
CREATE INDEX lfac_wide_kx ON lfac_wide USING lion ((k || ''));
CREATE INDEX lfac_wide_g ON lfac_wide USING lion (g);
VACUUM ANALYZE lfac_wide;
CREATE FUNCTION lfac_wide_all() RETURNS TABLE (filter text, idx text, k int, result text)
LANGUAGE plpgsql AS $$
DECLARE
	f text;
	kk int;
BEGIN
	FOREACH f IN ARRAY ARRAY['true', 'g = 1', 'g IN (1, 2)'] LOOP
		FOREACH kk IN ARRAY ARRAY[1, 3, 70, 200]::int[] LOOP
			filter := f; k := kk;
			FOREACH idx IN ARRAY ARRAY['lfac_wide_k', 'lfac_wide_kx'] LOOP
				result := lfac_check(idx, kk, f,
					format('SELECT k, count(*) AS n FROM lfac_wide WHERE (%s) GROUP BY k', f));
				RETURN NEXT;
			END LOOP;
		END LOOP;
	END LOOP;
END $$;
SELECT count(*) AS checks, count(*) FILTER (WHERE result LIKE 'same%') AS same
  FROM lfac_wide_all();
SELECT * FROM lfac_wide_all() WHERE result NOT LIKE 'same%';
SELECT left(key, 12) AS key, length(key), count FROM lion_index_facets('lfac_wide_k', 6);
SELECT left(key, 12) AS key, length(key), count FROM lion_index_facets('lfac_wide_k', 3, 'g = 2');
-- a two-row key with no row left, and a seventh of the rows gone
DELETE FROM lfac_wide WHERE id IN (1251, 6251) OR id % 7 = 0;
SELECT count(*) AS checks, count(*) FILTER (WHERE result LIKE 'same%') AS same
  FROM lfac_wide_all();
SELECT * FROM lfac_wide_all() WHERE result NOT LIKE 'same%';
SELECT left(key, 12) AS key, length(key), count FROM lion_index_facets('lfac_wide_kx', 4);
DROP FUNCTION lfac_wide_all();
DROP TABLE lfac_wide;

DROP TABLE lfac, lfac_filters;
DROP FUNCTION lfac_all(text);
DROP FUNCTION lfac_check(text, int, text, text);
DROP FUNCTION lfac_tags_q(text);
DROP FUNCTION lfac_tsv_q(text);
DROP FUNCTION lfac_nums_q(text);
DROP FUNCTION lfac_grp_q(text);
