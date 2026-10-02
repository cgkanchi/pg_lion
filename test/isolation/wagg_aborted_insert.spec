# The aggregates over keys of DESIGN.md §37 and the rows of an ABORTED insert
# (the 2026-10-01 soak's finding; §37, "The rows of an entry").
#
# lion_wagg_run() reads each entry's ntids as its rows when the visibility map
# calls every heap page all-visible before the walk and again after it, with
# as many pages.  A change made in between takes the mark off its page, and a
# COMMITTED one keeps it off while the walk's snapshot holds VACUUM back.  An
# aborted one does not: its tuple is dead to every snapshot, so VACUUM prunes
# it, its bulk delete takes the TID out of the index, and its second heap pass
# marks the page again - all after the walk, which holds nothing between two
# leaves, read a header whose ntids counted the TID.  The second look then
# finds every page marked, and the sums keep the phantom row.  What tells the
# walk is the meta page's count of finished bulk deletes, which the end of
# every ambulkdelete call moves before VACUUM may mark a page it emptied; read
# before the first look and after the second, it has moved, and the walk
# counts every entry instead.
#
#  * wa: 9800 keys k in [0, 10000) with two rows each, none with k % 50 = 49,
#    on a heap loaded at fillfactor 50 and then set to 100, so that the
#    aborted rows fit on the existing all-visible pages; VACUUM FREEZE marks
#    every page.
#  * s1 runs sum(k), max(k % 50) through LionCount and parks at
#    'lion-entry-scan-leaf', before the directory's second leaf, its first
#    look taken and one leaf read.
#  * s2 inserts one row for every existing key and one for every k % 50 = 49
#    key, and ROLLS BACK.  Every entry the walk has still to read counts one
#    TID more, and k % 50 = 49 has entries now; every touched page lost its
#    mark.
#  * s3 lets s1 read the rest of the leaves: it parks again at
#    'lion-wagg-walked', every header read and the map not looked at again.
#  * s3 runs VACUUM (INDEX_CLEANUP ON): the aborted rows are pruned, their
#    TIDs leave the index, the pages are marked all-visible again, and the heap
#    keeps its size (s3_allvis says both).
#  * s3 releases s1, whose second look finds every page all-visible.
#
# s1's answer must be s3_reference's, the heap's: 97980400 | 48.  Before the
# fix it was 147969514 | 49 - the keys of every leaf after the first, once
# more, and a k % 50 that no row has.  The second permutation aborts a
# subtransaction instead (ROLLBACK TO SAVEPOINT in a transaction that
# commits): the same.
#
# A speculative insert killed by INSERT ... ON CONFLICT leaves the same kind
# of TID - its tuple's xmin is cleared, so it is dead to every snapshot - but
# it cannot be shown here without an abort beside it: the kill needs a
# conflicting tuple inserted concurrently, which either commits, and then its
# page stays unmarked while s1's snapshot holds VACUUM back (s1 counts every
# entry anyway), or aborts, after which the killer's retry inserts its row and
# has to abort too.  Both permutations below are that abort.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE TABLE wa (id int, k int, pad text)
		WITH (fillfactor = 50, autovacuum_enabled = off);
	INSERT INTO wa SELECT r, k, 'x'
	  FROM generate_series(0, 9999) k, generate_series(1, 2) r
	 WHERE k % 50 <> 49;
	ALTER TABLE wa SET (fillfactor = 100);
	CREATE INDEX wa_k ON wa USING lion (k);
	CREATE TABLE wa_pages (before bigint);
}

teardown
{
	DROP TABLE wa, wa_pages;
	/* Injection points are cluster-wide: never leave one attached, and
	 * never fail the teardown because the permutation already did. */
	DO $$ BEGIN PERFORM injection_points_detach('lion-entry-scan-leaf');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
	DO $$ BEGIN PERFORM injection_points_detach('lion-wagg-walked');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

# The aggregates over keys, parked between two leaves and then after the walk.
session s1
setup
{
	SET max_parallel_workers_per_gather = 0;
	SET enable_seqscan = off;
	SELECT injection_points_set_local();
	SELECT injection_points_attach('lion-entry-scan-leaf', 'wait');
	SELECT injection_points_attach('lion-wagg-walked', 'wait');
}
step s1_aggs	{ SELECT sum(k), max(k % 50) FROM wa; }

# The aborted inserts: one row for every key, existing or k % 50 = 49.
session s2
setup			{ SET pg_lion.enable_count_pushdown = off; }
step s2_abort	{
	BEGIN;
	INSERT INTO wa SELECT -1, k, 'y' FROM (SELECT DISTINCT k FROM wa) d;
	INSERT INTO wa SELECT -2, k, 'y' FROM generate_series(49, 9999, 50) k;
	ROLLBACK;
}
step s2_savepoint	{
	BEGIN;
	SAVEPOINT a;
	INSERT INTO wa SELECT -1, k, 'y' FROM (SELECT DISTINCT k FROM wa) d;
	INSERT INTO wa SELECT -2, k, 'y' FROM generate_series(49, 9999, 50) k;
	ROLLBACK TO SAVEPOINT a;
	COMMIT;
}

session s3
setup			{ SET max_parallel_workers_per_gather = 0; SET enable_seqscan = off; }
step s3_prep	{ VACUUM (FREEZE, ANALYZE) wa; }
step s3_how		{
	INSERT INTO wa_pages SELECT pg_relation_size('wa');
	DO $$
	DECLARE ln text; found bool := false;
	BEGIN
		FOR ln IN EXPLAIN (COSTS OFF) SELECT sum(k), max(k % 50) FROM wa LOOP
			IF ln ~ 'Aggregates Over Keys' THEN found := true; END IF;
		END LOOP;
		RAISE NOTICE 'aggregates over keys planned: %', found;
	END $$;
	SELECT leaf_pages >= 2 AS two_leaves_or_more FROM lion_index_stats('wa_k');
}
step s3_parked	{
	SELECT wait_event FROM pg_stat_activity
	 WHERE datname = current_database() AND wait_event_type = 'InjectionPoint';
}
step s3_walk	{
	/*
	 * Detach first, so that the walk parks at no leaf again, then let it go
	 * on to the end of its walk, where it parks at the other point.
	 */
	SELECT injection_points_detach('lion-entry-scan-leaf');
	SELECT injection_points_wakeup('lion-entry-scan-leaf');
	DO $$
	BEGIN
		FOR i IN 1 .. 6000 LOOP
			/* pg_stat_activity is otherwise read once per transaction */
			PERFORM pg_stat_clear_snapshot();
			EXIT WHEN EXISTS (SELECT FROM pg_stat_activity
							   WHERE datname = current_database()
								 AND wait_event_type = 'InjectionPoint'
								 AND wait_event = 'lion-wagg-walked');
			PERFORM pg_sleep(0.01);
		END LOOP;
	END $$;
	SELECT wait_event FROM pg_stat_activity
	 WHERE datname = current_database() AND wait_event_type = 'InjectionPoint';
}
step s3_vacuum	{ VACUUM (INDEX_CLEANUP ON) wa; }
step s3_allvis	{
	SELECT pg_relation_size('wa') = (SELECT before FROM wa_pages) AS same_pages,
		   (SELECT relallvisible FROM pg_class WHERE relname = 'wa') =
		   pg_relation_size('wa') / current_setting('block_size')::int AS all_visible_again;
}
step s3_release	{
	SELECT injection_points_detach('lion-wagg-walked');
	SELECT injection_points_wakeup('lion-wagg-walked');
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
									 AND wait_event = 'lion-wagg-walked');
			PERFORM pg_sleep(0.01);
		END LOOP;
	END $$;
}
step s3_reference	{
	SET pg_lion.enable_count_pushdown = off;
	SET enable_seqscan = on;
	SET enable_bitmapscan = off;
	SET enable_indexscan = off;
	SELECT sum(k), max(k % 50) FROM wa;
	RESET pg_lion.enable_count_pushdown;
	SET enable_seqscan = off;
	RESET enable_bitmapscan;
	RESET enable_indexscan;
}
step s3_after	{ SELECT sum(k), max(k % 50) FROM wa; }
step s3_verify	{ SELECT lion_index_verify('wa_k', true); }

permutation
	s3_prep				# every page all-visible
	s3_how				# the plan takes the aggregates over keys
	s1_aggs(s3_release)	# parks before the second leaf
	s3_parked
	s2_abort			# one aborted TID in every entry
	s3_walk				# s1 reads the other leaves and parks after them
	s3_vacuum			# the aborted TIDs go, the pages are marked again
	s3_allvis
	s3_release			# s1 looks again, and counts every entry
	s3_reference		# the answer from the heap
	s3_after			# and LionCount again, after the VACUUM
	s3_verify

# The same with ROLLBACK TO SAVEPOINT in a transaction that commits.
permutation
	s3_prep
	s3_how
	s1_aggs(s3_release)
	s3_parked
	s2_savepoint
	s3_walk
	s3_vacuum
	s3_allvis
	s3_release
	s3_reference
	s3_after
	s3_verify
