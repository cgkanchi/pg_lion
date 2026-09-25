# Two SERIALIZABLE transactions each read through a LionOrdered scan
# (DESIGN.md §30) - the first rows, in btree order, of the rows lion says
# have c = 1 (or c = 2) - and then each insert a row the OTHER one's read
# would have returned first.  The node takes a relation predicate lock on the
# lion index it builds its set from, the btree walk takes its page locks, and
# every fetched row its tuple lock, so this write skew must end in a
# serialization failure, as it does for the ordinary plan.

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE TABLE os (id int PRIMARY KEY, k int, c int);
	INSERT INTO os SELECT i, (i * 37) % 1009, i % 10 FROM generate_series(1, 1000) i;
	CREATE INDEX os_k ON os (k, id);
	CREATE INDEX os_c ON os USING lion (c);
	ANALYZE os;
}
teardown
{
	DROP TABLE os;
}

session s1
setup
{
	SET enable_seqscan = off; SET enable_bitmapscan = off;
	SET enable_indexscan = off; SET enable_indexonlyscan = off;
}
step s1_begin	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1_plan	{ EXPLAIN (COSTS OFF) SELECT id FROM os WHERE c = 1 ORDER BY k, id LIMIT 3; }
step s1_read	{ SELECT id, k FROM os WHERE c = 1 ORDER BY k, id LIMIT 3; }
step s1_insert	{ INSERT INTO os VALUES (2001, -1, 2); }
step s1_commit	{ COMMIT; }

session s2
setup
{
	SET enable_seqscan = off; SET enable_bitmapscan = off;
	SET enable_indexscan = off; SET enable_indexonlyscan = off;
}
step s2_begin	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2_read	{ SELECT id, k FROM os WHERE c = 2 ORDER BY k, id LIMIT 3; }
step s2_insert	{ INSERT INTO os VALUES (2002, -2, 1); }
step s2_commit	{ COMMIT; }

permutation s1_plan s1_begin s2_begin s1_read s2_read s1_insert s2_insert s1_commit s2_commit
permutation s1_begin s2_begin s1_read s2_read s1_insert s1_commit s2_insert s2_commit
