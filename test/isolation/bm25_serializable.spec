# Two SERIALIZABLE transactions each rank an absent term, then each insert a
# row with it.  Ranking reads the index under the relation-level predicate
# lock index_beginscan() takes for an AM without ampredlocks, so the second
# commit must fail with a serialization failure, as it does for an ordinary
# index scan - both through lion_bm25() and through the LionBm25 scan.

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE TABLE bm_ser (id int, d tsvector);
	INSERT INTO bm_ser SELECT g, to_tsvector('simple', 'common w' || (g % 10))
	  FROM generate_series(1, 1000) g;
	CREATE INDEX bm_ser_d ON bm_ser USING lion (d) WITH (store_positions = true);
}
teardown
{
	DROP TABLE bm_ser;
}

session s1
setup			{ SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_indexscan = off; }
step s1_begin	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1_func	{ SELECT count(*) FROM lion_bm25('bm_ser_d', 'secret', 10); }
step s1_plan	{ EXPLAIN (COSTS OFF) SELECT id FROM bm_ser WHERE d @@ 'secret'
					ORDER BY lion_bm25_score(d, 'secret', 'bm_ser_d') DESC LIMIT 10; }
step s1_scan	{ SELECT id FROM bm_ser WHERE d @@ 'secret'
					ORDER BY lion_bm25_score(d, 'secret', 'bm_ser_d') DESC LIMIT 10; }
step s1_insert	{ INSERT INTO bm_ser VALUES (-1, to_tsvector('simple', 'secret one')); }
step s1_commit	{ COMMIT; }

session s2
setup			{ SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_indexscan = off; }
step s2_begin	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2_func	{ SELECT count(*) FROM lion_bm25('bm_ser_d', 'secret', 10); }
step s2_scan	{ SELECT id FROM bm_ser WHERE d @@ 'secret'
					ORDER BY lion_bm25_score(d, 'secret', 'bm_ser_d') DESC LIMIT 10; }
step s2_insert	{ INSERT INTO bm_ser VALUES (-2, to_tsvector('simple', 'secret two')); }
step s2_commit	{ COMMIT; }

# lion_bm25()
permutation s1_begin s2_begin s1_func s2_func s1_insert s2_insert s1_commit s2_commit
# the LionBm25 scan
permutation s1_begin s2_begin s1_plan s1_scan s2_scan s1_insert s2_insert s1_commit s2_commit
