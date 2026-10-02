# The aggregates over keys of DESIGN.md §37 and the key of a row the query
# cannot see: an expression of the key is evaluated on it only once the walk
# is known to count the snapshot's rows.
#
# lion_wagg_run() reads each entry's ntids as its rows when the visibility map
# calls every heap page all-visible before the walk and again after it.  A
# row inserted in between - by a transaction that rolls back, or that commits
# after the query's snapshot - has its TID in the entry the walk reads, and
# the second look sends the walk to counting each entry, where the row's
# entry has no rows.  But the walk gave each entry to the aggregates as it
# read it, evaluating the argument on its key: sum(100 / k) on an inserted
# k = 0 raised "division by zero" before the second look could say that no
# row the query sees has that key, where the sequential scan answers.  The
# walk now keeps such an entry's key and evaluates the argument on it only
# after a look at the map has said the walk's counts are the snapshot's -
# the look after the walk, or one it takes when the keys waiting fill
# work_mem.
#
#  * wz: k in [1, 5000], two rows each, no k = 0; VACUUM FREEZE marks every
#    page all-visible.
#  * s1 runs sum(100 / k), min(1000 / k), max(k) through LionCount and parks
#    at 'lion-wagg-looked', after the first look and before the first
#    header.
#  * s2 inserts k = 0 and rolls back; or inserts it and commits, after s1's
#    snapshot.
#  * s3 releases s1, which reads the entry of k = 0 with one TID.
#
# s1's answer must be s3_reference's, the heap's, taken before the insert:
# 964 | 0 | 5000.  Before the fix the first two permutations failed with
# "division by zero".  The third runs the first with work_mem at its least,
# 64kB, which about a thousand keys fill: the walk looks at the map during the
# walk, finds the aborted row's page unmarked, and stops there.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE TABLE wz (k int) WITH (autovacuum_enabled = off);
	INSERT INTO wz SELECT k FROM generate_series(1, 5000) k,
								 generate_series(1, 2) r;
	CREATE INDEX wz_k ON wz USING lion (k);
}
setup { VACUUM (FREEZE, ANALYZE) wz; }

teardown
{
	DROP TABLE wz;
	/* Injection points are cluster-wide: never leave one attached, and
	 * never fail the teardown because the permutation already did. */
	DO $$ BEGIN PERFORM injection_points_detach('lion-wagg-looked');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

# The aggregates over keys, parked after the first look.
session s1
setup
{
	SET max_parallel_workers_per_gather = 0;
	SET enable_seqscan = off;
	RESET work_mem;
	SELECT injection_points_set_local();
	SELECT injection_points_attach('lion-wagg-looked', 'wait');
}
step s1_small	{ SET work_mem = 64; }
step s1_aggs	{ SELECT sum(100 / k), min(1000 / k), max(k) FROM wz; }

# The row of k = 0, rolled back or committed.
session s2
step s2_abort	{ BEGIN; INSERT INTO wz VALUES (0); ROLLBACK; }
step s2_commit	{ INSERT INTO wz VALUES (0); }

session s3
setup			{ SET max_parallel_workers_per_gather = 0; }
step s3_reference	{
	DO $$
	DECLARE ln text; found bool := false;
	BEGIN
		SET LOCAL enable_seqscan = off;
		FOR ln IN EXPLAIN (COSTS OFF)
				  SELECT sum(100 / k), min(1000 / k), max(k) FROM wz LOOP
			IF ln ~ 'Aggregates Over Keys' THEN found := true; END IF;
		END LOOP;
		RAISE NOTICE 'aggregates over keys planned: %', found;
	END $$;
	SET pg_lion.enable_count_pushdown = off;
	SELECT sum(100 / k), min(1000 / k), max(k) FROM wz;
	RESET pg_lion.enable_count_pushdown;
}
step s3_parked	{
	SELECT wait_event FROM pg_stat_activity
	 WHERE datname = current_database() AND wait_event_type = 'InjectionPoint';
}
step s3_release	{
	SELECT injection_points_detach('lion-wagg-looked');
	SELECT injection_points_wakeup('lion-wagg-looked');
	/*
	 * Wait until s1 has left the point.  The tester deems a session that
	 * waits at one blocked, and checks the walk again as soon as this step
	 * is done: seen there still, the walk's answer would be reported after
	 * the next step's instead of before it.
	 */
	DO $$
	BEGIN
		FOR i IN 1 .. 6000 LOOP
			PERFORM pg_stat_clear_snapshot();
			EXIT WHEN NOT EXISTS (SELECT FROM pg_stat_activity
								   WHERE datname = current_database()
									 AND wait_event_type = 'InjectionPoint'
									 AND wait_event = 'lion-wagg-looked');
			PERFORM pg_sleep(0.01);
		END LOOP;
	END $$;
}

permutation
	s3_reference		# the answer from the heap, and the plan
	s1_aggs(s3_release)	# parks after the first look
	s3_parked
	s2_abort			# k = 0, rolled back
	s3_release			# s1 reads its entry

# The same with k = 0 committed after s1's snapshot.
permutation
	s3_reference
	s1_aggs(s3_release)
	s3_parked
	s2_commit
	s3_release

# The first again, with the keys waiting for a look filling work_mem.
permutation
	s3_reference
	s1_small
	s1_aggs(s3_release)
	s3_parked
	s2_abort
	s3_release
