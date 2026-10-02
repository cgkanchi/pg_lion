# Two SERIALIZABLE transactions each read sum(k) through the aggregates over
# keys of DESIGN.md §37, and then each write a row the OTHER one's sum
# counted: a write skew, which PostgreSQL must refuse with a serialization
# failure, as it does for the sequential scan the node stands in for.
#
# The walk reads every row of the table: from the entries' own counts, when
# every heap page is all-visible, which visits no heap page; or by counting
# each entry, which locks the heap pages it counts.  The node's index, the
# one its sum over every row reads, has the relation lock of every index the
# node reads, and every insert into the table goes into it.  But a DELETE
# goes into no index, and the walk from the entries' counts took no lock on
# the heap: two deletes of rows the other's sum counted both committed.  The
# walk now takes the lock a sequential scan takes, on the whole heap, before
# it reads anything.
#
#  * ws: 1000 rows, k in [0, 10); VACUUM FREEZE marks every page all-visible.
#  * The writes read nothing a predicate lock covers but their own row: a
#    DELETE by ctid, or an INSERT.
#  * The second permutation deletes a row first and commits, so that a page
#    is not all-visible and both walks count each entry instead.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE TABLE ws (k int NOT NULL) WITH (autovacuum_enabled = off);
	INSERT INTO ws SELECT g % 10 FROM generate_series(0, 999) g;
	CREATE INDEX ws_k ON ws USING lion (k);

	/* sum(k) as the node computes it, and which walk it takes */
	CREATE FUNCTION ws_sum() RETURNS text LANGUAGE plpgsql AS $$
	DECLARE
		ln text;
		walk text := 'no aggregates over keys';
		s bigint;
	BEGIN
		FOR ln IN EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF)
				  SELECT sum(k) FROM ws LOOP
			IF ln ~ 'Key Walks Counted' THEN
				walk := 'each entry counted';
			ELSIF ln ~ 'Key Walks From Entry Counts' AND
				  walk = 'no aggregates over keys' THEN
				walk := 'entry counts';
			END IF;
		END LOOP;
		SELECT sum(k) INTO s FROM ws;
		RETURN format('%s, %s', s, walk);
	END $$;
}
setup { VACUUM (FREEZE, ANALYZE) ws; }

teardown
{
	DROP TABLE ws;
	DROP FUNCTION ws_sum();
}

session s1
setup
{
	SET max_parallel_workers_per_gather = 0;
	SET enable_seqscan = off; SET enable_bitmapscan = off;
	SET enable_indexscan = off; SET enable_indexonlyscan = off;
}
step s1_dirty	{ DELETE FROM ws WHERE ctid = '(4,1)'; }
step s1_begin	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1_sum		{ SELECT ws_sum(); }
step s1_delete	{ DELETE FROM ws WHERE ctid = '(0,1)'; }
step s1_insert	{ INSERT INTO ws VALUES (100); }
step s1_commit	{ COMMIT; }

session s2
setup
{
	SET max_parallel_workers_per_gather = 0;
	SET enable_seqscan = off; SET enable_bitmapscan = off;
	SET enable_indexscan = off; SET enable_indexonlyscan = off;
}
step s2_begin	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2_sum		{ SELECT ws_sum(); }
step s2_delete	{ DELETE FROM ws WHERE ctid = '(0,2)'; }
step s2_insert	{ INSERT INTO ws VALUES (200); }
step s2_commit	{ COMMIT; }

# Each deletes a row: the walks from the entries' counts.
permutation s1_begin s2_begin s1_sum s2_sum s1_delete s2_delete s1_commit s2_commit
# The same, with a page not all-visible: both walks count each entry.
permutation s1_dirty s1_begin s2_begin s1_sum s2_sum s1_delete s2_delete s1_commit s2_commit
# Each inserts a row, which the index's own lock always caught.
permutation s1_begin s2_begin s1_sum s2_sum s1_insert s2_insert s1_commit s2_commit
