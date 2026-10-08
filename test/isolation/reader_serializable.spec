# The contract every reader of a lion index keeps under SERIALIZABLE (DESIGN.md
# §9, "Direct readers"): it takes the relation-level predicate lock
# index_beginscan() takes for an AM without ampredlocks, before it reads, so
# a read that finds nothing still conflicts with a later insert of what it
# looked for.  Two transactions each read an absent key through the same
# reader, each insert it, and the second commit must fail, as it does for an
# ordinary index scan.  One permutation per reader: the SQL functions
# (lion_reader_vet()) and the custom scans (lion_reader_lock()).  A new
# reader gets a step pair and a permutation here.

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE TABLE rc (id int, k int NOT NULL, j int NOT NULL, d tsvector);
	INSERT INTO rc SELECT g, g % 10, g % 7, to_tsvector('simple', 'common w' || (g % 10))
	  FROM generate_series(1, 1000) g;
	CREATE INDEX rc_k ON rc USING lion (k);
	CREATE INDEX rc_j ON rc USING lion (j);
	CREATE INDEX rc_d ON rc USING lion (d) WITH (store_positions = true);
	ANALYZE rc;
}
teardown
{
	DROP TABLE rc;
}
session s1
setup			{ SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_indexscan = off; }
step s1_begin	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1_count	{ SELECT lion_index_count('rc_k', 99); }
step s1_count2	{ SELECT lion_index_count('rc_k', 99, 'rc_j', 99); }
step s1_any	{ SELECT lion_index_count_any('rc_k', ARRAY[98, 99]); }
step s1_stats	{ SELECT count FROM lion_index_count_stats('rc_k', 99); }
step s1_groups	{ SELECT count(*) FROM lion_index_count_group_stats('rc_k'); }
step s1_bm25	{ SELECT count(*) FROM lion_bm25('rc_d', 'secret', 10); }
step s1_push	{ SELECT count(*) FROM rc WHERE k = 99; }
step s1_rank	{ SELECT id FROM rc WHERE d @@ 'secret' ORDER BY lion_bm25_score(d, 'secret', 'rc_d') DESC LIMIT 10; }
step s1_plans	{ EXPLAIN (COSTS OFF) SELECT count(*) FROM rc WHERE k = 99;
				  EXPLAIN (COSTS OFF) SELECT id FROM rc WHERE d @@ 'secret' ORDER BY lion_bm25_score(d, 'secret', 'rc_d') DESC LIMIT 10; }
step s1_insert	{ INSERT INTO rc VALUES (-1, 99, 99, to_tsvector('simple', 'secret')); }
step s1_commit	{ COMMIT; }

session s2
setup			{ SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_indexscan = off; }
step s2_begin	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2_count	{ SELECT lion_index_count('rc_k', 99); }
step s2_count2	{ SELECT lion_index_count('rc_k', 99, 'rc_j', 99); }
step s2_any	{ SELECT lion_index_count_any('rc_k', ARRAY[98, 99]); }
step s2_stats	{ SELECT count FROM lion_index_count_stats('rc_k', 99); }
step s2_groups	{ SELECT count(*) FROM lion_index_count_group_stats('rc_k'); }
step s2_bm25	{ SELECT count(*) FROM lion_bm25('rc_d', 'secret', 10); }
step s2_push	{ SELECT count(*) FROM rc WHERE k = 99; }
step s2_rank	{ SELECT id FROM rc WHERE d @@ 'secret' ORDER BY lion_bm25_score(d, 'secret', 'rc_d') DESC LIMIT 10; }
step s2_insert	{ INSERT INTO rc VALUES (-2, 99, 99, to_tsvector('simple', 'secret')); }
step s2_commit	{ COMMIT; }

permutation s1_begin s2_begin s1_count s2_count s1_insert s2_insert s1_commit s2_commit
permutation s1_begin s2_begin s1_count2 s2_count2 s1_insert s2_insert s1_commit s2_commit
permutation s1_begin s2_begin s1_any s2_any s1_insert s2_insert s1_commit s2_commit
permutation s1_begin s2_begin s1_stats s2_stats s1_insert s2_insert s1_commit s2_commit
permutation s1_begin s2_begin s1_groups s2_groups s1_insert s2_insert s1_commit s2_commit
permutation s1_begin s2_begin s1_bm25 s2_bm25 s1_insert s2_insert s1_commit s2_commit
permutation s1_begin s2_begin s1_push s2_push s1_insert s2_insert s1_commit s2_commit
permutation s1_begin s2_begin s1_rank s2_rank s1_insert s2_insert s1_commit s2_commit
# the custom scans above are the plans
permutation s1_plans
