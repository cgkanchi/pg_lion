# The LionBm25 scan goes on past its first walk with a stable continuation.
#
# s1 ranks 100 rows through a cursor, with no LIMIT, so the first walk
# ranks 64 of them; s2 then commits a row that outscores all of them, which
# s1's REPEATABLE READ snapshot does not see but the next walk does.  Before
# the fix the scan resumed at offset 64 in the bigger walk, which the new
# row had shifted down a place: it returned row 64 twice and never row 100.
# Every row must come back once, in the order the scan with the new row
# left out would give.
#
# A row committed below where the scan has got to is in the next walk too:
# the walks used to stop once they had given as many rows as the lexeme had
# when the scan started, and so lost the last visible row to it (99 of 100).
#
# The scores shown are the ones the rows were ranked by: a cursor opened
# before s2 commits many rows ranks by the statistics as they were then, and
# shows those, rather than ones read at the first FETCH.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE TABLE bm_cont (id int, d tsvector) WITH (autovacuum_enabled = off);
	-- 100 rows, each with 'hit' a different number of times, all with
	-- distinct scores
	INSERT INTO bm_cont
	SELECT g, to_tsvector('simple', repeat('hit ', g) || 'pad pad pad pad pad')
	  FROM generate_series(1, 100) g;
	INSERT INTO bm_cont SELECT 1000 + g, to_tsvector('simple', 'other')
	  FROM generate_series(1, 200) g;
	CREATE INDEX bm_cont_d ON bm_cont USING lion (d) WITH (store_positions = true);
	ANALYZE bm_cont;
	CREATE TABLE bm_cont_got (n serial, id int, s float8);
}
teardown
{
	DROP TABLE bm_cont, bm_cont_got;
}

session s1
setup			{ SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_indexscan = off; }
step s1_begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1_plan	{ EXPLAIN (COSTS OFF) SELECT id FROM bm_cont WHERE d @@ 'hit'
					ORDER BY lion_bm25_score(d, 'hit', 'bm_cont_d') DESC; }
step s1_open	{ DECLARE c NO SCROLL CURSOR FOR SELECT id FROM bm_cont WHERE d @@ 'hit'
					ORDER BY lion_bm25_score(d, 'hit', 'bm_cont_d') DESC; }
# each fetched row is kept in bm_cont_got, numbered in the order it came
step s1_fetch64	{ DO $$ DECLARE c refcursor := 'c'; r record;
					BEGIN FOR i IN 1..64 LOOP FETCH c INTO r;
					  INSERT INTO bm_cont_got (id) VALUES (r.id); END LOOP; END $$; }
step s1_rest	{ DO $$ DECLARE c refcursor := 'c'; r record;
					BEGIN LOOP FETCH c INTO r; EXIT WHEN NOT FOUND;
					  INSERT INTO bm_cont_got (id) VALUES (r.id); END LOOP; END $$; }
step s1_check	{ SELECT count(*) AS fetched, count(DISTINCT id) AS distinct_rows,
						 bool_and(id = 101 - n) AS best_first
					FROM bm_cont_got; }
step s1_open_scores { DECLARE c NO SCROLL CURSOR FOR
					SELECT id, lion_bm25_score(d, 'hit | pad', 'bm_cont_d') AS s
					  FROM bm_cont WHERE d @@ 'hit | pad'
					 ORDER BY lion_bm25_score(d, 'hit | pad', 'bm_cont_d') DESC; }
step s1_scores	{ DO $$ DECLARE c refcursor := 'c'; r record;
					BEGIN LOOP FETCH c INTO r; EXIT WHEN NOT FOUND;
					  INSERT INTO bm_cont_got (id, s) VALUES (r.id, r.s); END LOOP; END $$; }
step s1_check_scores { SELECT count(*) AS fetched,
						 count(*) FILTER (WHERE s > prev) AS out_of_order
					FROM (SELECT s, lag(s) OVER (ORDER BY n) AS prev FROM bm_cont_got) x; }
step s1_commit	{ COMMIT; }

session s2
step s2_insert	{ INSERT INTO bm_cont VALUES (-1, to_tsvector('simple', repeat('hit ', 300))); }
step s2_insert_below { INSERT INTO bm_cont VALUES (-2, to_tsvector('simple', repeat('hit ', 20) || 'pad pad pad pad pad')); }
step s2_insert_many { INSERT INTO bm_cont SELECT 5000 + g, to_tsvector('simple', 'other')
					FROM generate_series(1, 3000) g; }

permutation s1_begin s1_plan s1_open s1_fetch64 s2_insert s1_rest s1_check s1_commit
permutation s1_begin s1_plan s1_open s1_fetch64 s2_insert_below s1_rest s1_check s1_commit
permutation s1_begin s1_open_scores s2_insert_many s1_scores s1_check_scores s1_commit
