# The LionBm25 scan goes on past its first walk with a stable continuation.
#
# s1 ranks 100 rows through a cursor, with no LIMIT, so the first walk
# ranks 64 of them; s2 then commits a row that outscores all of them, which
# s1's REPEATABLE READ snapshot does not see but the next walk does.  Before
# the fix the scan resumed at offset 64 in the bigger walk, which the new
# row had shifted down a place: it returned row 64 twice and never row 100.
# Every row must come back once, in the order the scan with the new row
# left out would give.

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
	CREATE TABLE bm_cont_got (n serial, id int);
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
step s1_commit	{ COMMIT; }

session s2
step s2_insert	{ INSERT INTO bm_cont VALUES (-1, to_tsvector('simple', repeat('hit ', 300))); }

permutation s1_begin s1_plan s1_open s1_fetch64 s2_insert s1_rest s1_check s1_commit
