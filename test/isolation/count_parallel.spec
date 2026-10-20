# A parallel count (DESIGN.md §10, "A count in parallel") beside writers.
#
# The count's participants claim the heap's container keys a range at a time
# and count each claimed range as the serial node counts the whole, every
# worker under the leader's snapshot.  So a REPEATABLE READ reader's parallel
# count must stay the count of its snapshot while other sessions update,
# delete, insert and VACUUM, range by range: each step prints the parallel
# count and the same count with the pushdown off (cpar_plain(), whose SET
# clause turns it off for its own query alone), in the same transaction.
#
# The table spans a dozen container keys, cut into ranges of one key
# (pg_lion.parallel_range_keys), and parallel plans cost nothing to start,
# so the count below is the parallel node's (s1_plan shows it).  The writes
# leave pages the visibility map no longer vouches for in several ranges.
#
# Then SERIALIZABLE: two transactions each count rows through the parallel
# node and each insert a row the other counted.  The count takes the
# relation-level predicate lock the serial count takes, in whichever
# participant, so the second commit fails as it does for the serial node.

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE TABLE cpar (id int NOT NULL, a int NOT NULL, b boolean NOT NULL,
					   k int NOT NULL, pad text NOT NULL)
		WITH (autovacuum_enabled = off);
	INSERT INTO cpar
	SELECT i, h % 4, (h / 7) % 2 = 0, i % 50, repeat('x', 100)
	  FROM (SELECT i, abs(hashint4(i)) AS h
			  FROM generate_series(1, 40000) i) r;
	CREATE INDEX cpar_abk ON cpar USING lion (a, b, k);
	CREATE FUNCTION cpar_plain() RETURNS bigint LANGUAGE sql
		SET pg_lion.enable_count_pushdown = off
		AS $$ SELECT count(*) FROM cpar WHERE a = 1 AND b $$;
}
setup { VACUUM (FREEZE, ANALYZE) cpar; }

teardown
{
	DROP TABLE cpar;
	DROP FUNCTION cpar_plain();
}

# The REPEATABLE READ reader, and the first SERIALIZABLE transaction.
session s1
setup
{
	SET max_parallel_workers_per_gather = 2;
	SET parallel_setup_cost = 0;
	SET parallel_tuple_cost = 0;
	SET min_parallel_index_scan_size = 0;
	SET min_parallel_table_scan_size = 0;
	SET pg_lion.parallel_range_keys = 1;
	SET enable_seqscan = off;
	SET enable_bitmapscan = off;
}
step s1_rr		{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1_ser		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1_plan	{ EXPLAIN (COSTS OFF) SELECT count(*) FROM cpar WHERE a = 1 AND b; }
step s1_count	{ SELECT count(*) AS lion FROM cpar WHERE a = 1 AND b; }
step s1_plain	{ SELECT cpar_plain() AS plain; }
step s1_insert	{ INSERT INTO cpar VALUES (100001, 1, true, 7, 'y'); }
step s1_commit	{ COMMIT; }

# The writer: autocommitted changes spread over the table's keys.
session s2
setup
{
	SET max_parallel_workers_per_gather = 2;
	SET parallel_setup_cost = 0;
	SET parallel_tuple_cost = 0;
	SET min_parallel_index_scan_size = 0;
	SET min_parallel_table_scan_size = 0;
	SET pg_lion.parallel_range_keys = 1;
	SET enable_seqscan = off;
	SET enable_bitmapscan = off;
}
step s2_update	{ UPDATE cpar SET b = NOT b WHERE id % 9 = 0; }
step s2_delete	{ DELETE FROM cpar WHERE a = 1 AND id % 13 = 0; }
step s2_insert	{ INSERT INTO cpar SELECT 50000 + i, 1, true, i % 50, 'z'
					FROM generate_series(1, 3000) i; }
step s2_ser		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2_count	{ SELECT count(*) AS lion FROM cpar WHERE a = 1 AND b; }
step s2_insert1	{ INSERT INTO cpar VALUES (100002, 1, true, 8, 'y'); }
step s2_commit	{ COMMIT; }

# A reader with a fresh snapshot for every statement, and VACUUM.
session s3
setup
{
	SET max_parallel_workers_per_gather = 2;
	SET parallel_setup_cost = 0;
	SET parallel_tuple_cost = 0;
	SET min_parallel_index_scan_size = 0;
	SET min_parallel_table_scan_size = 0;
	SET pg_lion.parallel_range_keys = 1;
	SET enable_seqscan = off;
	SET enable_bitmapscan = off;
}
step s3_count	{ SELECT count(*) AS lion FROM cpar WHERE a = 1 AND b; }
step s3_plain	{ SELECT cpar_plain() AS plain; }
step s3_vacuum	{ VACUUM cpar; }

# The snapshot's count, range by range, while the table changes under it -
# and a fresh snapshot's, which sees every change.
permutation s1_plan s1_rr s1_count s1_plain s2_update s2_delete s2_insert
			s1_count s1_plain s3_count s3_plain s3_vacuum s1_plan s1_count
			s1_plain s3_count s3_plain s1_commit s1_count s1_plain

# Two SERIALIZABLE parallel counts, each of a row the other inserts.
permutation s1_ser s2_ser s1_plan s1_count s2_count s1_insert s2_insert1
			s1_commit s2_commit
