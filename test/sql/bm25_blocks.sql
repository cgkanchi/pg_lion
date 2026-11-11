-- Block bounds (DESIGN.md §17, "Block bounds"): a ranking walk steps over
-- the chunks whose impacts say no row of them can get in.  Whatever it
-- skips, its rows and scores must be those of scoring every row with
-- lion_bm25_score() and sorting, bit for bit, through a build that spills,
-- inserts, updates, deletes, VACUUM and a parallel rebuild.

\set VERBOSITY terse
SET client_min_messages = warning;
SET synchronous_commit = on;

CREATE EXTENSION IF NOT EXISTS pg_lion;

-- a few common words in every row at uneven tf, lengths 1 to 61, every 97th
-- row far longer, every 13th with a rarer word
CREATE TABLE bb_docs (id int, d tsvector) WITH (autovacuum_enabled = off);
CREATE FUNCTION bb_doc(i int, extra int DEFAULT 0) RETURNS tsvector
LANGUAGE sql IMMUTABLE AS $$
SELECT to_tsvector('simple',
		 (SELECT string_agg('c' || (((i * 31 + g * g * 7) % 11) % 5), ' ')
			FROM generate_series(1, 1 + (i * 37 + extra) % 61) g)
		 || CASE WHEN i % 97 = 0 THEN repeat(' c' || (i % 5), 200 + i % 50) ELSE '' END
		 || CASE WHEN i % 13 = 0 THEN ' r' || (i % 7) ELSE '' END) $$;
INSERT INTO bb_docs SELECT i, bb_doc(i) FROM generate_series(1, 6000) i;

-- a build that spills its runs, so the lengths go through them
SET maintenance_work_mem = '1MB';
CREATE INDEX bb_docs_d ON bb_docs USING lion (d) WITH (store_positions = true);
RESET maintenance_work_mem;
VACUUM bb_docs;

-- lion_bm25()'s rows against every row with a scored lexeme, scored and
-- sorted by (score, TID); q is an OR of the lexemes, so that @@ is that row set
CREATE FUNCTION bb_same(q tsquery, k int, k1 float8 DEFAULT 1.2,
						b float8 DEFAULT 0.75) RETURNS text LANGUAGE sql AS $$
WITH l AS (SELECT row_number() OVER () AS r, s.ctid, s.score
			 FROM lion_bm25('bb_docs_d', q, k, k1, b) s),
	 f AS (SELECT row_number() OVER (ORDER BY sc DESC, ctid) AS r, ctid, sc AS score
			 FROM (SELECT ctid, lion_bm25_score(d, q, 'bb_docs_d', k1, b) AS sc
					 FROM bb_docs WHERE d @@ q) x
			ORDER BY sc DESC, ctid LIMIT k)
SELECT count(*) || ' rows, ' ||
	   count(*) FILTER (WHERE l.ctid IS DISTINCT FROM f.ctid OR
						l.score IS DISTINCT FROM f.score) || ' differ'
  FROM l FULL JOIN f USING (r) $$;

CREATE FUNCTION bb_all() RETURNS SETOF text LANGUAGE sql AS $$
SELECT q || ' k=' || k || ' k1=' || k1 || ' b=' || b || ': ' || bb_same(q::tsquery, k, k1, b)
  FROM unnest(ARRAY['c0', 'c0 | c1', 'c1 | c3 | c4', 'c0 | c1 | c2 | c3 | c4',
					'c2 | r3', 'r1 | r5 | c4']) WITH ORDINALITY AS u(q, qn),
	   unnest(ARRAY[1, 10, 100]) AS k,
	   (VALUES (1.2::float8, 0.75::float8), (0, 0.75), (1.2, 0), (2.0, 1.0),
			   (1e308, 0.5)) AS p(k1, b)
 ORDER BY qn, k, k1, b $$;

-- rows that differ, if any
SELECT * FROM bb_all() WHERE bb_all NOT LIKE '% 0 differ';
SELECT count(*) FROM bb_all();

-- inserts: members appended to the last chunks and split there
INSERT INTO bb_docs SELECT i, bb_doc(i) FROM generate_series(6001, 9000) i;
SELECT * FROM bb_all() WHERE bb_all NOT LIKE '% 0 differ';

-- updates: new versions, of other lengths, in chunks already written
UPDATE bb_docs SET d = bb_doc(id, 29) WHERE id % 5 = 0;
SELECT * FROM bb_all() WHERE bb_all NOT LIKE '% 0 differ';

-- the best rows deleted: walks go on past the dead ones
DELETE FROM bb_docs WHERE ctid IN (SELECT ctid FROM lion_bm25('bb_docs_d', 'c0 | c1', 200));
SELECT * FROM bb_all() WHERE bb_all NOT LIKE '% 0 differ';

-- VACUUM removes members; the impacts they leave still bound what stays
DELETE FROM bb_docs WHERE id % 3 = 0;
VACUUM bb_docs;
SELECT * FROM bb_all() WHERE bb_all NOT LIKE '% 0 differ';
SELECT lion_index_verify('bb_docs_d', true);

-- a parallel rebuild
ALTER TABLE bb_docs SET (parallel_workers = 2);
SET max_parallel_maintenance_workers = 2;
SET min_parallel_table_scan_size = 0;
REINDEX INDEX bb_docs_d;
RESET max_parallel_maintenance_workers;
RESET min_parallel_table_scan_size;
SELECT * FROM bb_all() WHERE bb_all NOT LIKE '% 0 differ';
SELECT lion_index_verify('bb_docs_d', true);

DROP TABLE bb_docs;
DROP FUNCTION bb_all();
DROP FUNCTION bb_same(tsquery, int, float8, float8);
DROP FUNCTION bb_doc(int, int);
