-- ORDER BY lion_bm25_score(...) DESC from the index: the LionBm25 scan
-- (DESIGN.md §17, "Ranking") against the same queries sorted row by row.

\set VERBOSITY terse
SET client_min_messages = warning;
SET synchronous_commit = on;

CREATE EXTENSION IF NOT EXISTS pg_lion;

CREATE TABLE bs_docs (id int, d tsvector) WITH (autovacuum_enabled = off);
INSERT INTO bs_docs
SELECT i, setweight(to_tsvector('simple', 'w' || (i % 3) || ' t' || (i % 11)), 'A') ||
	   to_tsvector('simple', (SELECT string_agg('w' || ((i * 7 + g * g * 13) % (5 + g * 3)), ' ')
							  FROM generate_series(1, 1 + i % 40) g))
FROM generate_series(1, 3000) i;
INSERT INTO bs_docs VALUES (3001, strip(to_tsvector('simple', 'w1 w2 w2 w3'))),
						   (3002, ''), (3003, NULL);
CREATE INDEX bs_docs_d ON bs_docs USING lion (d) WITH (store_positions = true);
VACUUM ANALYZE bs_docs;

-- the ranked query: score q, keep rows matching w and extra
CREATE FUNCTION bs_sql(q tsquery, w tsquery, k int, extra text, k1 float8, b float8,
					   tiebreak text) RETURNS text LANGUAGE sql AS $$
SELECT format($q$SELECT row_number() OVER () AS r, id, round(s::numeric, 9) AS sc FROM
	(SELECT id, lion_bm25_score(d, %L, 'bs_docs_d', %s, %s) AS s FROM bs_docs
	  WHERE d @@ %L AND %s ORDER BY lion_bm25_score(d, %L, 'bs_docs_d', %s, %s) DESC%s %s) x$q$,
	q, k1, b, w, extra, q, k1, b, tiebreak,
	CASE WHEN k > 0 THEN 'LIMIT ' || k ELSE '' END) $$;

-- does the plan of sql use the scan?
CREATE FUNCTION bs_scanned(sql text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || sql LOOP
		IF l LIKE '%LionBm25%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

-- the scan's rows against the same query sorted with the scan off, ties
-- by TID in both; the scan is made the only way to the rows, since the
-- planner rightly prefers sorting a few
CREATE FUNCTION bs_same(q tsquery, w tsquery, k int, extra text DEFAULT 'true',
						k1 float8 DEFAULT 1.2, b float8 DEFAULT 0.75)
RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	sql text := bs_sql(q, w, k, extra, k1, b, '');
	result text;
BEGIN
	SET LOCAL enable_seqscan = off;
	SET LOCAL enable_bitmapscan = off;
	SET LOCAL enable_indexscan = off;
	IF NOT bs_scanned(sql) THEN RETURN 'not scanned'; END IF;
	EXECUTE 'CREATE TEMP TABLE bs_got AS ' || sql;
	RESET enable_seqscan;
	RESET enable_bitmapscan;
	RESET enable_indexscan;
	SET LOCAL pg_lion.enable_bm25_scan = off;
	EXECUTE 'CREATE TEMP TABLE bs_ref AS ' || bs_sql(q, w, k, extra, k1, b, ', ctid');
	RESET pg_lion.enable_bm25_scan;
	SELECT count(*) || ' rows, ' ||
		   count(*) FILTER (WHERE g.id IS DISTINCT FROM f.id OR g.sc IS DISTINCT FROM f.sc) || ' differ'
	  INTO result FROM bs_got g FULL JOIN bs_ref f USING (r);
	DROP TABLE bs_got, bs_ref;
	RETURN result;
END $$;

EXPLAIN (COSTS OFF)
SELECT id FROM bs_docs WHERE d @@ 'w1 | w7'
 ORDER BY lion_bm25_score(d, 'w1 | w7', 'bs_docs_d') DESC LIMIT 10;

SELECT bs_same('w1 | w7', 'w1 | w7', 10);
SELECT bs_same('w1 & w7', 'w1 & w7', 50);			-- the WHERE drops candidates
SELECT bs_same('w3 | t4 | w12', 'w3 | t4 | w12', 100);
SELECT bs_same('w2 <-> w5 & !w9', 'w2 <-> w5 & !w9', 30);
SELECT bs_same('w1 | w2 | t7 | w30', 'w30', 5);	-- a WHERE on one of the lexemes
SELECT bs_same('w4 | w8', 'w4 | w8', 40, 'true', 2.0, 0.3);
SELECT bs_same('w1 | w5', 'w1 | w5', 20, 'id % 7 = 0');	-- more walks for the filter
SELECT bs_same('w6 & t0', 'w6 & t0', 5000);		-- more than there are
SELECT bs_same('w2', 'w2', 3000);					-- the stripped row too
SET enable_sort = off;
SELECT bs_same('w1 | t3', 'w1 | t3', 0);			-- no LIMIT: every row
RESET enable_sort;

-- not offered: ascending, no WHERE, a WHERE lexeme the score lacks, a WHERE
-- an empty document matches, a prefix
SELECT bs_scanned($$SELECT id FROM bs_docs WHERE d @@ 'w1'
	ORDER BY lion_bm25_score(d, 'w1', 'bs_docs_d') LIMIT 10$$) AS asc;
SELECT bs_scanned($$SELECT id FROM bs_docs
	ORDER BY lion_bm25_score(d, 'w1', 'bs_docs_d') DESC LIMIT 10$$) AS no_where;
SELECT bs_scanned($$SELECT id FROM bs_docs WHERE d @@ 'w1 | w9'
	ORDER BY lion_bm25_score(d, 'w1', 'bs_docs_d') DESC LIMIT 10$$) AS other_lexeme;
SELECT bs_scanned($$SELECT id FROM bs_docs WHERE d @@ '!w9'
	ORDER BY lion_bm25_score(d, 'w9', 'bs_docs_d') DESC LIMIT 10$$) AS empty_matches;
SELECT bs_scanned($$SELECT id FROM bs_docs WHERE d @@ 'w1:*'
	ORDER BY lion_bm25_score(d, 'w1', 'bs_docs_d') DESC LIMIT 10$$) AS prefix;
SET pg_lion.enable_bm25_scan = off;
SELECT bs_scanned($$SELECT id FROM bs_docs WHERE d @@ 'w1'
	ORDER BY lion_bm25_score(d, 'w1', 'bs_docs_d') DESC LIMIT 10$$) AS off;
RESET pg_lion.enable_bm25_scan;

-- a second sort key: an incremental sort over the scan's order
EXPLAIN (COSTS OFF)
SELECT id FROM bs_docs WHERE d @@ 'w1'
 ORDER BY lion_bm25_score(d, 'w1', 'bs_docs_d') DESC, id LIMIT 10;
CREATE TEMP TABLE bs_got AS SELECT row_number() OVER () AS r, id FROM
	(SELECT id FROM bs_docs WHERE d @@ 'w1'
	  ORDER BY lion_bm25_score(d, 'w1', 'bs_docs_d') DESC, id LIMIT 10) x;
SET pg_lion.enable_bm25_scan = off;
SELECT count(*) AS differ FROM bs_got g FULL JOIN
	(SELECT row_number() OVER () AS r, id FROM
		(SELECT id FROM bs_docs WHERE d @@ 'w1'
		  ORDER BY lion_bm25_score(d, 'w1', 'bs_docs_d') DESC, id LIMIT 10) x) f USING (r)
 WHERE g.id IS DISTINCT FROM f.id;
RESET pg_lion.enable_bm25_scan;
DROP TABLE bs_got;

-- the score agrees with lion_bm25(), and is NULL for a NULL document
SELECT count(*) FILTER (WHERE round(s.score::numeric, 9) =
						round(lion_bm25_score(t.d, 'w1 | w7', 'bs_docs_d')::numeric, 9)) AS same
  FROM lion_bm25('bs_docs_d', 'w1 | w7', 100) s JOIN bs_docs t ON t.ctid = s.ctid;
SELECT lion_bm25_score(d, 'w1', 'bs_docs_d') FROM bs_docs WHERE id IN (3002, 3003) ORDER BY id;

-- dead rows are skipped; rows inserted since are found
DELETE FROM bs_docs WHERE id IN (
	SELECT id FROM bs_docs WHERE d @@ 'w5'
	 ORDER BY lion_bm25_score(d, 'w5', 'bs_docs_d') DESC LIMIT 40);
INSERT INTO bs_docs SELECT i, to_tsvector('simple', 'w5 w5 w5 w5') FROM generate_series(4001, 4003) i;
SELECT bs_same('w5', 'w5', 10);

-- once per outer row
SELECT v.n, x.id FROM (VALUES (1), (2)) v(n),
	LATERAL (SELECT id FROM bs_docs WHERE d @@ 'w5' AND id > v.n
			  ORDER BY lion_bm25_score(d, 'w5', 'bs_docs_d') DESC LIMIT 2) x;

-- rows inserted while a cursor is open: in the index, not in the snapshot.
-- After the first walk of 64, a row inserted among the rest must not cost
-- one of them (the walks stop only when one comes up short), and the scores
-- shown are the ones the rows were ranked by, read when the scan started
CREATE TABLE bs_cur (id int, d tsvector) WITH (autovacuum_enabled = off);
INSERT INTO bs_cur SELECT i, to_tsvector('simple', 'w ' || repeat('x ', i))
  FROM generate_series(1, 100) i;
CREATE INDEX bs_cur_d ON bs_cur USING lion (d) WITH (store_positions = true);
VACUUM ANALYZE bs_cur;
CREATE TEMP TABLE bs_shown (n serial, id int, s float8);
-- what is left of cursor bs_c, into bs_shown
CREATE FUNCTION bs_drain() RETURNS void LANGUAGE plpgsql AS $$
DECLARE c refcursor := 'bs_c'; r record;
BEGIN
	LOOP
		FETCH c INTO r;
		EXIT WHEN NOT FOUND;
		INSERT INTO bs_shown (id, s) VALUES (r.id, r.s);
	END LOOP;
END $$;
BEGIN;
SET LOCAL enable_seqscan = off;
SET LOCAL enable_bitmapscan = off;
SET LOCAL enable_sort = off;
DECLARE bs_c CURSOR FOR SELECT id, 0::float8 AS s FROM bs_cur WHERE d @@ 'w'
	ORDER BY lion_bm25_score(d, 'w', 'bs_cur_d') DESC;
MOVE 64 IN bs_c;
INSERT INTO bs_cur VALUES (1000, to_tsvector('simple', 'w ' || repeat('x ', 70)));
SELECT bs_drain();
SELECT count(*) AS rest, min(id), max(id) FROM bs_shown;
ROLLBACK;
BEGIN;
SET LOCAL enable_seqscan = off;
SET LOCAL enable_bitmapscan = off;
SET LOCAL enable_sort = off;
DECLARE bs_c CURSOR FOR SELECT id, lion_bm25_score(d, 'w | x', 'bs_cur_d') AS s
	FROM bs_cur WHERE d @@ 'w | x'
	ORDER BY lion_bm25_score(d, 'w | x', 'bs_cur_d') DESC;
INSERT INTO bs_cur SELECT 2000 + i, to_tsvector('simple', 'q') FROM generate_series(1, 3000) i;
SELECT bs_drain();
SELECT count(*) AS shown, count(*) FILTER (WHERE s > prev) AS out_of_order
  FROM (SELECT s, lag(s) OVER (ORDER BY n) AS prev FROM bs_shown) x;
ROLLBACK;
DROP FUNCTION bs_drain();

-- the scan tuple carries the columns the plan uses: system columns and a
-- whole row too
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_sort = off;
SELECT ctid = (SELECT ctid FROM bs_cur WHERE id = 1) AS ctid,
	   tableoid::regclass, (c).id
  FROM bs_cur c WHERE d @@ 'w'
 ORDER BY lion_bm25_score(d, 'w', 'bs_cur_d') DESC LIMIT 1;
EXPLAIN (COSTS OFF, VERBOSE)
SELECT id, lion_bm25_score(d, 'w', 'bs_cur_d') FROM bs_cur WHERE d @@ 'w'
 ORDER BY lion_bm25_score(d, 'w', 'bs_cur_d') DESC LIMIT 1;
-- a row locked for update is left to the ordinary plan
SELECT bs_scanned($$SELECT id FROM bs_cur WHERE d @@ 'w'
	ORDER BY lion_bm25_score(d, 'w', 'bs_cur_d') DESC LIMIT 1 FOR UPDATE$$) AS for_update;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_sort;
DROP TABLE bs_cur;

-- errors
SELECT lion_bm25_score(d, 'w1:*', 'bs_docs_d') FROM bs_docs LIMIT 1;
SELECT lion_bm25_score(d, 'w1', 'bs_docs_d', -1) FROM bs_docs LIMIT 1;
CREATE INDEX bs_docs_plain ON bs_docs USING lion (d);
SELECT lion_bm25_score(d, 'w1', 'bs_docs_plain') FROM bs_docs LIMIT 1;

DROP TABLE bs_docs;
DROP FUNCTION bs_same(tsquery, tsquery, int, text, float8, float8);
DROP FUNCTION bs_scanned(text);
DROP FUNCTION bs_sql(tsquery, tsquery, int, text, float8, float8, text);
