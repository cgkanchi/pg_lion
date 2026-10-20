-- BM25 ranking of one column of an index that stores positions for several
-- (DESIGN.md §17, "Ranking"): lion_bm25() and lion_bm25_score() take the
-- column to rank, and each column ranks as an index on it alone would.

\set VERBOSITY terse
SET client_min_messages = warning;
SET synchronous_commit = on;

CREATE EXTENSION IF NOT EXISTS pg_lion;

-- title and body share their vocabulary, at other frequencies and lengths,
-- so a lexeme's df and the mean length differ between them; every row has
-- both, so the two columns count the same N
CREATE TABLE bc_docs (id int, title tsvector, body tsvector, k int, txt text)
	WITH (autovacuum_enabled = off);
INSERT INTO bc_docs
SELECT i, to_tsvector('simple', 'w' || (i % 3) || ' w' || (i % 7) || ' t' || (i % 11)),
	   to_tsvector('simple', (SELECT string_agg('w' || ((i * 7 + g * g * 13) % (5 + g * 3)), ' ')
							  FROM generate_series(1, 1 + i % 40) g)),
	   i % 10,
	   'the cats ' || (CASE WHEN i % 4 = 0 THEN 'ran running ' ELSE '' END) || 'w' || (i % 5)
FROM generate_series(1, 3000) i;
-- two columns with positions and a scalar one in one index; each tsvector
-- column on its own for reference
CREATE INDEX bc_all ON bc_docs USING lion (title, body, k);
CREATE INDEX bc_title ON bc_docs USING lion (title);
CREATE INDEX bc_body ON bc_docs USING lion (body);
VACUUM ANALYZE bc_docs;

-- the index's columns, as col names them
SELECT attnum, attname FROM pg_attribute WHERE attrelid = 'bc_all'::regclass ORDER BY attnum;

-- lion_bm25() on one column of bc_all against the single-column index: the
-- same rows, in the same order, with the same scores to the bit
CREATE FUNCTION bc_same(q tsquery, k int, col name, single regclass,
						k1 float8 DEFAULT 1.2, b float8 DEFAULT 0.75)
RETURNS text LANGUAGE sql AS $$
WITH a AS (SELECT * FROM lion_bm25('bc_all', q, k, k1, b, col) WITH ORDINALITY AS s(ctid, score, r)),
	 s AS (SELECT * FROM lion_bm25(single, q, k, k1, b) WITH ORDINALITY AS s(ctid, score, r))
SELECT count(*) || ' rows, ' ||
	   count(*) FILTER (WHERE a.ctid IS DISTINCT FROM s.ctid OR a.score IS DISTINCT FROM s.score) || ' differ'
  FROM a FULL JOIN s USING (r) $$;

SELECT bc_same('w1', 20, 'title', 'bc_title');
SELECT bc_same('w1', 20, 'body', 'bc_body');
SELECT bc_same('w1 | w5 | t3', 100, 'title', 'bc_title');
SELECT bc_same('w1 | w5 | t3', 100, 'body', 'bc_body');
SELECT bc_same('w2 & w4', 3000, 'title', 'bc_title');
SELECT bc_same('w2 & w4', 3000, 'body', 'bc_body');
SELECT bc_same('w0 | w6', 50, 'title', 'bc_title', 2.0, 0.3);
SELECT bc_same('w0 | w6', 50, 'body', 'bc_body', 2.0, 0.3);
-- the two columns rank differently: their statistics are their own
SELECT count(*) FILTER (WHERE t.ctid IS DISTINCT FROM b.ctid OR t.score <> b.score) > 0 AS differ
  FROM lion_bm25('bc_all', 'w1 | w5', 50, col => 'title') WITH ORDINALITY AS t(ctid, score, r)
  FULL JOIN lion_bm25('bc_all', 'w1 | w5', 50, col => 'body') WITH ORDINALITY AS b(ctid, score, r)
	USING (r);

-- lion_bm25_score() likewise, for every row, and lion_bm25()'s scores
SELECT count(*) AS rows,
	   count(*) FILTER (WHERE lion_bm25_score(title, 'w1 | t3', 'bc_all', col => 'title') IS DISTINCT FROM
							  lion_bm25_score(title, 'w1 | t3', 'bc_title')) AS title_differ,
	   count(*) FILTER (WHERE lion_bm25_score(body, 'w1 | t3', 'bc_all', col => 'body') IS DISTINCT FROM
							  lion_bm25_score(body, 'w1 | t3', 'bc_body')) AS body_differ
  FROM bc_docs;
SELECT count(*) AS rows,
	   count(*) FILTER (WHERE s.score IS DISTINCT FROM
							  lion_bm25_score(d.body, 'w3 | w8', 'bc_all', 1.2, 0.75, 'body')) AS differ
  FROM lion_bm25('bc_all', 'w3 | w8', 200, col => 'body') s JOIN bc_docs d ON d.ctid = s.ctid;
-- another tsvector scored against a column's statistics, as the function
-- always could: body's tsvector by title's N, df and avgdl
SELECT count(*) FILTER (WHERE lion_bm25_score(body, 'w1 | t3', 'bc_all', col => 'title') IS DISTINCT FROM
							  lion_bm25_score(body, 'w1 | t3', 'bc_title')) AS differ
  FROM bc_docs;

-- an index with one column that stores positions needs no col, and takes
-- the name of that one; NULL is no col
SELECT count(*) FILTER (WHERE a.ctid IS DISTINCT FROM s.ctid OR a.score IS DISTINCT FROM s.score) AS differ
  FROM lion_bm25('bc_title', 'w1 | w5', 30, col => 'title') WITH ORDINALITY AS a(ctid, score, r)
  FULL JOIN lion_bm25('bc_title', 'w1 | w5', 30) WITH ORDINALITY AS s(ctid, score, r) USING (r);
SELECT count(*) FILTER (WHERE a.ctid IS DISTINCT FROM s.ctid OR a.score IS DISTINCT FROM s.score) AS differ
  FROM lion_bm25('bc_title', 'w1 | w5', 30, col => NULL) WITH ORDINALITY AS a(ctid, score, r)
  FULL JOIN lion_bm25('bc_title', 'w1 | w5', 30) WITH ORDINALITY AS s(ctid, score, r) USING (r);
SELECT lion_bm25_score(title, 'w1', 'bc_title', col => 'title') =
	   lion_bm25_score(title, 'w1', 'bc_title') AS same
  FROM bc_docs WHERE id = 1;

-- expression columns, named as CREATE INDEX named them: to_tsvector and
-- to_tsvector1 on PostgreSQL 19, other names on other versions, so taken
-- from the index into bc_e1 and bc_e2
CREATE INDEX bc_expr ON bc_docs USING lion (to_tsvector('simple', txt), to_tsvector('english', txt));
CREATE INDEX bc_english ON bc_docs USING lion (to_tsvector('english', txt));
SELECT max(attname::text) FILTER (WHERE attnum = 1) AS bc_e1,
	   max(attname::text) FILTER (WHERE attnum = 2) AS bc_e2
  FROM pg_attribute WHERE attrelid = 'bc_expr'::regclass \gset
SELECT :'bc_e1' <> :'bc_e2' AS distinct_names;
SELECT count(*) FILTER (WHERE a.ctid IS DISTINCT FROM s.ctid OR a.score IS DISTINCT FROM s.score) AS differ,
	   count(*) AS rows
  FROM lion_bm25('bc_expr', 'run & cat', 40, col => :'bc_e2') WITH ORDINALITY AS a(ctid, score, r)
  FULL JOIN lion_bm25('bc_english', 'run & cat', 40) WITH ORDINALITY AS s(ctid, score, r) USING (r);
SELECT count(*) FROM lion_bm25('bc_expr', 'run & cat', 40, col => :'bc_e1');
SELECT count(*) FILTER (WHERE lion_bm25_score(to_tsvector('english', txt), 'run', 'bc_expr', col => :'bc_e2')
							  IS DISTINCT FROM lion_bm25_score(to_tsvector('english', txt), 'run', 'bc_english')) AS differ
  FROM bc_docs;

-- NULLs: col NULL is the default; any other NULL is no rows, a NULL score
SELECT count(*) FROM lion_bm25('bc_all', NULL, 10, col => 'title');
SELECT count(*) FROM lion_bm25(NULL, 'w1', 10, col => 'title');
SELECT count(*) FROM lion_bm25('bc_all', 'w1', NULL, col => 'title');
SELECT count(*) FROM lion_bm25('bc_all', 'w1', 10, NULL, 0.75, 'title');
SELECT count(*) FROM lion_bm25('bc_title', 'w1', 10, 1.2, 0.75, NULL);
SELECT lion_bm25_score(NULL, 'w1', 'bc_all', col => 'title') AS doc,
	   lion_bm25_score(title, NULL, 'bc_all', col => 'title') AS query,
	   lion_bm25_score(title, 'w1', NULL, col => 'title') AS index,
	   lion_bm25_score(title, 'w1', 'bc_all', 1.2, NULL, 'title') AS b,
	   lion_bm25_score(title, 'w1', 'bc_title', 1.2, 0.75, NULL) > 0 AS col
  FROM bc_docs WHERE id = 1;

-- errors: no col on an index with several columns that store positions, a
-- column it does not have, one that does not store positions (its scalar
-- column), and an index that stores none
\set VERBOSITY default
SELECT * FROM lion_bm25('bc_all', 'w1', 10);
SELECT lion_bm25_score(title, 'w1', 'bc_all') FROM bc_docs WHERE id = 1;
SELECT * FROM lion_bm25('bc_all', 'w1', 10, col => 'nosuch');
SELECT * FROM lion_bm25('bc_all', 'w1', 10, col => 'Title');
SELECT * FROM lion_bm25('bc_all', 'w1', 10, col => 'k');
SELECT lion_bm25_score(title, 'w1', 'bc_all', col => 'k') FROM bc_docs WHERE id = 1;
-- (its hint names bc_e1 and bc_e2)
\set VERBOSITY terse
SELECT * FROM lion_bm25('bc_expr', 'w1', 10);
\set VERBOSITY default
CREATE INDEX bc_plain ON bc_docs USING lion (title, body) WITH (store_positions = off);
SELECT * FROM lion_bm25('bc_plain', 'w1', 10);
SELECT * FROM lion_bm25('bc_plain', 'w1', 10, col => 'title');
SELECT * FROM lion_bm25('bc_plain', 'w1', 10, col => 'nosuch');
DROP INDEX bc_plain;
\set VERBOSITY terse

-- ---------- the LionBm25 scan ----------

-- ORDER BY the score of col's column, a WHERE on it; ties by TID
CREATE FUNCTION bc_sql(doc text, q tsquery, idx text, col text, k int, tiebreak text)
RETURNS text LANGUAGE sql AS $$
SELECT format($q$SELECT row_number() OVER () AS r, id, s FROM
	(SELECT id, lion_bm25_score(%1$s, %2$L, %3$L, col => %4$s) AS s FROM bc_docs
	  WHERE %1$s @@ %2$L ORDER BY lion_bm25_score(%1$s, %2$L, %3$L, col => %4$s) DESC%6$s
	  LIMIT %5$s) x$q$, doc, q, idx, coalesce(quote_literal(col), 'NULL'), k, tiebreak) $$;

-- does the plan of sql use the scan?
CREATE FUNCTION bc_scanned(sql text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || sql LOOP
		IF l LIKE '%LionBm25%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

-- the scan's rows and scores against the same query sorted with the scan
-- off (the plain function, row by row), and against the scan of ref, the
-- single-column index: the same ids, in the same order, with the same
-- scores to the bit
CREATE FUNCTION bc_scan_same(doc text, q tsquery, col text, ref text, k int)
RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	sql text := bc_sql(doc, q, 'bc_all', col, k, '');
	result text;
BEGIN
	SET LOCAL enable_seqscan = off;
	SET LOCAL enable_bitmapscan = off;
	SET LOCAL pg_lion.enable_plain_scan = off;
	IF NOT bc_scanned(sql) THEN RETURN 'not scanned'; END IF;
	IF NOT bc_scanned(bc_sql(doc, q, ref, NULL, k, '')) THEN RETURN 'ref not scanned'; END IF;
	EXECUTE 'CREATE TEMP TABLE bc_got AS ' || sql;
	EXECUTE 'CREATE TEMP TABLE bc_one AS ' || bc_sql(doc, q, ref, NULL, k, '');
	RESET enable_seqscan;
	RESET enable_bitmapscan;
	RESET pg_lion.enable_plain_scan;
	SET LOCAL pg_lion.enable_bm25_scan = off;
	EXECUTE 'CREATE TEMP TABLE bc_ref AS ' || bc_sql(doc, q, 'bc_all', col, k, ', ctid');
	RESET pg_lion.enable_bm25_scan;
	SELECT count(*) || ' rows, ' ||
		   count(*) FILTER (WHERE g.id IS DISTINCT FROM f.id OR g.s IS DISTINCT FROM f.s) ||
		   ' differ from the function, ' ||
		   count(*) FILTER (WHERE g.id IS DISTINCT FROM o.id OR g.s IS DISTINCT FROM o.s) ||
		   ' from ' || ref
	  INTO result FROM bc_got g FULL JOIN bc_ref f USING (r) FULL JOIN bc_one o USING (r);
	DROP TABLE bc_got, bc_ref, bc_one;
	RETURN result;
END $$;

SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET pg_lion.enable_plain_scan = off;
EXPLAIN (COSTS OFF)
SELECT id FROM bc_docs WHERE title @@ 'w1 | w5'
 ORDER BY lion_bm25_score(title, 'w1 | w5', 'bc_all', col => 'title') DESC LIMIT 10;
EXPLAIN (COSTS OFF, VERBOSE)
SELECT id, lion_bm25_score(body, 'w1 | w5', 'bc_all', col => 'body') FROM bc_docs WHERE body @@ 'w1 | w5'
 ORDER BY lion_bm25_score(body, 'w1 | w5', 'bc_all', col => 'body') DESC LIMIT 10;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET pg_lion.enable_plain_scan;

SELECT bc_scan_same('title', 'w1 | w5', 'title', 'bc_title', 10);
SELECT bc_scan_same('body', 'w1 | w5', 'body', 'bc_body', 10);
SELECT bc_scan_same('title', 'w1 & t3', 'title', 'bc_title', 100);
SELECT bc_scan_same('body', 'w1 & w3', 'body', 'bc_body', 100);
SELECT bc_scan_same('title', 'w2 | w4 | t7', 'title', 'bc_title', 3000);
SELECT bc_scan_same('body', 'w2 | w4 | w20', 'body', 'bc_body', 3000);

-- col named on an index with one column that stores positions
SELECT bc_scanned(bc_sql('title', 'w1', 'bc_title', 'title', 10, '')) AS single_named;

-- not offered where the walk's scores would not be the function's: no col
-- on bc_all, or one it does not have, which the function refuses at run
-- time; a doc that is not the column col names, either way round; an
-- expression column
SELECT bc_scanned(bc_sql('title', 'w1', 'bc_all', NULL, 10, '')) AS no_col;
SELECT bc_scanned(bc_sql('title', 'w1', 'bc_all', 'nosuch', 10, '')) AS no_such_col;
SELECT bc_scanned(bc_sql('body', 'w1', 'bc_all', 'title', 10, '')) AS body_by_title;
SELECT bc_scanned(bc_sql('title', 'w1', 'bc_all', 'body', 10, '')) AS title_by_body;
SELECT bc_scanned(format($$SELECT id FROM bc_docs WHERE to_tsvector('english', txt) @@ 'run'
	ORDER BY lion_bm25_score(to_tsvector('english', txt), 'run', 'bc_expr', col => %L) DESC
	LIMIT 10$$, :'bc_e2')) AS expression;
SELECT bc_scanned(bc_sql('title', 'w1', 'bc_all', 'title', 10, '')) AS title_by_title;
-- col a parameter: a generic plan cannot tell the column, a custom one can
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET pg_lion.enable_plain_scan = off;
PREPARE bc_p(name) AS SELECT row_number() OVER () AS r, id, s FROM
	(SELECT id, lion_bm25_score(title, 'w1', 'bc_all', col => $1) AS s FROM bc_docs
	  WHERE title @@ 'w1'
	  ORDER BY lion_bm25_score(title, 'w1', 'bc_all', col => $1) DESC, id LIMIT 5) x;
SET plan_cache_mode = force_generic_plan;
SELECT bc_scanned('EXECUTE bc_p(''title'')') AS generic;
CREATE TEMP TABLE bc_generic AS EXECUTE bc_p('title');
SET plan_cache_mode = force_custom_plan;
SELECT bc_scanned('EXECUTE bc_p(''title'')') AS custom;
CREATE TEMP TABLE bc_custom AS EXECUTE bc_p('title');
SELECT count(*) AS rows,
	   count(*) FILTER (WHERE g.id IS DISTINCT FROM c.id OR g.s IS DISTINCT FROM c.s) AS differ
  FROM bc_generic g FULL JOIN bc_custom c USING (r);
RESET plan_cache_mode;
DEALLOCATE bc_p;
DROP TABLE bc_generic, bc_custom;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET pg_lion.enable_plain_scan;

-- and so the rows of each are the function's, row by row
SELECT * FROM (SELECT id FROM bc_docs WHERE title @@ 'w1'
	ORDER BY lion_bm25_score(title, 'w1', 'bc_all') DESC LIMIT 3) x;
CREATE TEMP TABLE bc_got AS SELECT row_number() OVER () AS r, id, s FROM
	(SELECT id, lion_bm25_score(body, 'w1 | w5', 'bc_all', col => 'title') AS s FROM bc_docs
	  WHERE body @@ 'w1 | w5'
	  ORDER BY lion_bm25_score(body, 'w1 | w5', 'bc_all', col => 'title') DESC, ctid LIMIT 40) x;
SELECT count(*) AS rows,
	   count(*) FILTER (WHERE g.s IS DISTINCT FROM lion_bm25_score(d.body, 'w1 | w5', 'bc_title')) AS differ,
	   count(*) FILTER (WHERE g.s > lag) AS out_of_order
  FROM (SELECT *, lag(s) OVER (ORDER BY r) FROM bc_got) g JOIN bc_docs d USING (id);
DROP TABLE bc_got;

-- the filters' rows through bc_all, the one lion index left, are seq scan's
DROP INDEX bc_title, bc_body, bc_expr, bc_english;
CREATE FUNCTION bc_count(w text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a int8; s int8;
BEGIN
	SET LOCAL enable_seqscan = off;
	EXECUTE 'SELECT count(*) FROM bc_docs WHERE ' || w INTO a;
	SET LOCAL enable_seqscan = on;
	SET LOCAL enable_bitmapscan = off;
	SET LOCAL enable_indexscan = off;
	SET LOCAL pg_lion.enable_count_pushdown = off;
	EXECUTE 'SELECT count(*) FROM bc_docs WHERE ' || w INTO s;
	RETURN CASE WHEN a = s THEN 'same' ELSE a || ' <> ' || s END;
END $$;
SELECT bc_count($$title @@ 'w1 & t3'$$), bc_count($$body @@ 'w1 <-> w3'$$),
	   bc_count($$title @@ 'w1' AND body @@ 'w5 & !w3'$$);

DROP TABLE bc_docs;
DROP FUNCTION bc_count(text);
DROP FUNCTION bc_scan_same(text, tsquery, text, text, int);
DROP FUNCTION bc_scanned(text);
DROP FUNCTION bc_sql(text, tsquery, text, text, int, text);
DROP FUNCTION bc_same(tsquery, int, name, regclass, float8, float8);
