-- BM25 ranking from stored positions (DESIGN.md §17, "Ranking"):
-- lion_bm25() against BM25 computed in SQL from the rows themselves.

\set VERBOSITY terse
SET client_min_messages = warning;
SET synchronous_commit = on;

CREATE EXTENSION IF NOT EXISTS pg_lion;

CREATE TABLE bm_docs (id int, note text, d tsvector)
	WITH (autovacuum_enabled = off, fillfactor = 60);
-- lengths 3..42, a skewed vocabulary, a few title (A) words
INSERT INTO bm_docs
SELECT i, 'n', setweight(to_tsvector('simple', 'w' || (i % 3) || ' t' || (i % 11)), 'A') ||
	   to_tsvector('simple', (SELECT string_agg('w' || ((i * 7 + g * g * 13) % (5 + g * 3)), ' ')
							  FROM generate_series(1, 1 + i % 40) g))
FROM generate_series(1, 3000) i;
-- a row stripped of positions, and one with no lexemes
INSERT INTO bm_docs VALUES (3001, 'n', strip(to_tsvector('simple', 'w1 w2 w2 w3'))),
						   (3002, 'n', '');
CREATE INDEX bm_docs_d ON bm_docs USING lion (d) WITH (store_positions = true);
VACUUM bm_docs;

-- the reference: a row's length is its lexeme occurrences, a stripped
-- lexeme counted once; N and avgdl over the rows with a lexeme
CREATE FUNCTION bm_ref(q text[], k int, k1 float8 DEFAULT 1.2, b float8 DEFAULT 0.75)
RETURNS TABLE (id int, score float8) LANGUAGE sql AS $$
WITH occ AS (SELECT t.id, t.ctid AS tid, u.lexeme,
					greatest(coalesce(array_length(u.positions, 1), 0), 1)::float8 AS tf
			   FROM bm_docs t, unnest(t.d) u),
len AS (SELECT occ.id, sum(tf) AS dl FROM occ GROUP BY occ.id),
st AS (SELECT count(*)::float8 AS n, avg(dl) AS avgdl FROM len),
df AS (SELECT lexeme, count(*)::float8 AS df FROM occ WHERE lexeme = ANY (q) GROUP BY lexeme)
SELECT occ.id, sum(ln(1 + (st.n - df.df + 0.5) / (df.df + 0.5)) * occ.tf * (k1 + 1) /
				   (occ.tf + k1 * (1 - b + b * len.dl / st.avgdl))) AS s
  FROM occ JOIN df USING (lexeme) JOIN len USING (id), st
 GROUP BY occ.id, occ.tid ORDER BY s DESC, occ.tid LIMIT k $$;

CREATE FUNCTION bm_lion(q tsquery, k int, k1 float8 DEFAULT 1.2, b float8 DEFAULT 0.75)
RETURNS TABLE (id int, score float8) LANGUAGE sql AS $$
SELECT t.id, s.score FROM lion_bm25('bm_docs_d', q, k, k1, b) s
  JOIN bm_docs t ON t.ctid = s.ctid ORDER BY s.score DESC, t.ctid $$;

-- same rows, same scores, same order (ties are broken by TID in both)
CREATE FUNCTION bm_same(q tsquery, lex text[], k int, k1 float8 DEFAULT 1.2,
						b float8 DEFAULT 0.75) RETURNS text LANGUAGE sql AS $$
WITH l AS (SELECT row_number() OVER () AS r, id, round(score::numeric, 9) AS sc
			 FROM bm_lion(q, k, k1, b)),
	 f AS (SELECT row_number() OVER () AS r, id, round(score::numeric, 9) AS sc
			 FROM bm_ref(lex, k, k1, b))
SELECT count(*) || ' rows, ' ||
	   count(*) FILTER (WHERE l.id IS DISTINCT FROM f.id OR l.sc IS DISTINCT FROM f.sc) || ' differ'
  FROM l FULL JOIN f USING (r) $$;

SELECT bm_same('w1', '{w1}', 20);
SELECT bm_same('w1 & w7', '{w1,w7}', 50);
SELECT bm_same('w3 | t4 | w12', '{w3,t4,w12}', 100);
SELECT bm_same('w2 <-> w5 & !w9', '{w2,w5}', 30);	-- NOT terms do not score
SELECT bm_same('w1:A & w2', '{w1,w2}', 30);			-- weights do not either
SELECT bm_same('w4 | w8', '{w4,w8}', 40, 2.0, 0.3);
SELECT bm_same('w4 | w8', '{w4,w8}', 40, 0.5, 1.0);
SELECT bm_same('w6 & t0', '{w6,t0}', 5000);			-- every candidate
SELECT bm_same('w3', '{w3}', 10, 1.2, 0);			-- no length normalisation
-- the stripped row counts each lexeme once
SELECT id, round(score::numeric, 6) FROM bm_lion('w2', 3000) WHERE id = 3001;
-- a lexeme no row has, an empty query, a NOT alone, k = 0
SELECT count(*) FROM lion_bm25('bm_docs_d', 'nosuchword', 10);
SELECT count(*) FROM lion_bm25('bm_docs_d', '', 10);
SELECT count(*) FROM lion_bm25('bm_docs_d', '!w1', 10);
SELECT count(*) FROM lion_bm25('bm_docs_d', 'w1', 0);

-- dead rows are skipped and k is still filled; a HOT update's row is found
-- through its new TID
DELETE FROM bm_docs WHERE id IN (SELECT id FROM bm_ref('{w1}', 3));
UPDATE bm_docs SET note = 'hot' WHERE id IN (SELECT id FROM bm_ref('{w1}', 6));
SELECT count(*) AS returned, count(t.id) AS joined, count(*) FILTER (WHERE t.note = 'hot') AS hot
  FROM lion_bm25('bm_docs_d', 'w1', 10) s LEFT JOIN bm_docs t ON t.ctid = s.ctid;
-- after VACUUM the statistics no longer count the deleted rows, once a
-- backend reads them again: it keeps N and avgdl until the row count moves by
-- more than 1/64
VACUUM bm_docs;
SELECT bm_same('w1 | w5', '{w1,w5}', 40) LIKE '40 rows, 0 differ' AS exact;
\c
SET client_min_messages = warning;
SELECT bm_same('w1 | w5', '{w1,w5}', 40);

-- inserts and a rebuild
INSERT INTO bm_docs SELECT i, 'n', to_tsvector('simple', 'w1 w1 w1 zz' || i) FROM generate_series(3100, 3200) i;
SELECT bm_same('w1 | zz3150', '{w1,zz3150}', 40);
REINDEX INDEX bm_docs_d;
SELECT bm_same('w1 | zz3150', '{w1,zz3150}', 40);

-- errors
SELECT * FROM lion_bm25('bm_docs_d', 'w1:*', 10);
SELECT * FROM lion_bm25('bm_docs_d', 'w1', -1);
SELECT * FROM lion_bm25('bm_docs_d', 'w1', 10, -1);
CREATE INDEX bm_docs_plain ON bm_docs USING lion (d);
SELECT * FROM lion_bm25('bm_docs_plain', 'w1', 10);
CREATE INDEX bm_docs_btree ON bm_docs (id);
SELECT * FROM lion_bm25('bm_docs_btree', 'w1', 10);
SELECT * FROM lion_bm25('bm_docs', 'w1', 10);
CREATE ROLE bm_nobody;
SET ROLE bm_nobody;
SELECT * FROM lion_bm25('bm_docs_d', 'w1', 10);
RESET ROLE;
DROP ROLE bm_nobody;

DROP TABLE bm_docs;
DROP FUNCTION bm_same(tsquery, text[], int, float8, float8);
DROP FUNCTION bm_lion(tsquery, int, float8, float8);
DROP FUNCTION bm_ref(text[], int, float8, float8);
