# FINDING (2026-10-01 soak): the aggregates over keys of DESIGN.md §37 count
# the rows of an ABORTED insert.
#
# lion_wagg_run() reads each entry's ntids as its rows when the visibility
# map calls every heap page all-visible before the walk and again after it,
# with the same page count.  Its argument is that a change made in between
# takes the mark off its page and "that page cannot be marked again while
# this snapshot's xmin holds VACUUM back".  That is true of a COMMITTED
# change, but an aborted one is dead to every snapshot whatever the horizon:
# VACUUM prunes it, ambulkdelete removes its TID, and the second heap pass
# marks the page all-visible again - all while the walk, which holds nothing
# between two leaves, has already read the entry's header with the aborted
# TID counted in ntids.
#
#  * wa: 9800 keys k in [0, 10000) with two rows each, none with
#    k % 50 = 49, on a heap
#    loaded at fillfactor 50 and then set to 100, so inserts fit on the
#    existing all-visible pages; VACUUM FREEZE marks every page.
#  * s1 runs sum(k), max(k % 50) through LionCount and parks at
#    'lion-entry-scan-leaf' before the directory's second leaf.
#  * s2 inserts one row for every existing key and one for every k % 50 = 49
#    key in [0, 10000), and ROLLS BACK.  Every entry's ntids now counts one
#    TID more; every touched page lost its mark.
#  * s3 wakes s1 ONCE: it reads one leaf (ntids one too high) and parks again
#    before the next.
#  * s3 runs VACUUM (INDEX_CLEANUP ON): the aborted rows are pruned, their
#    TIDs leave the index, and the pages are marked all-visible again.
#  * s3 detaches the point and wakes s1, which finishes the walk, finds every
#    page all-visible with the same page count, and keeps the headers' sums.
#
# Expected (correct) result: s1's answer equals s3_reference's.  On main at
# dc7cd29 sum(k) is too high by the keys of the leaf read in between, and
# max(k % 50) is 49, a value no row has, when that leaf held a k % 50 = 49
# key.  The second permutation aborts a subtransaction instead (ROLLBACK TO
# SAVEPOINT in a transaction that commits): the same.  (With a count(*) in
# the same target list the count of every row walks first and the park lands
# in it; the aggregates' own walk then starts after the VACUUM.)
#
# Run with test/soak/findings/run-spec.sh wagg_aborted_insert; the expected
# file holds the CORRECT answers, so the run fails while the bug is there.

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
}

teardown
{
	DROP TABLE wa;
	DO $$ BEGIN PERFORM injection_points_detach('lion-entry-scan-leaf');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

session s1
setup
{
	SET max_parallel_workers_per_gather = 0;
	SET enable_seqscan = off;
	SELECT injection_points_set_local();
	SELECT injection_points_attach('lion-entry-scan-leaf', 'wait');
}
step s1_aggs	{ SELECT sum(k), max(k % 50) FROM wa; }

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
	CREATE TEMP TABLE wa_pages AS SELECT pg_relation_size('wa') AS before;
	DO $$
	DECLARE ln text; found bool := false;
	BEGIN
		FOR ln IN EXPLAIN (COSTS OFF) SELECT sum(k), max(k % 50) FROM wa LOOP
			IF ln ~ 'Aggregates Over Keys' THEN found := true; END IF;
		END LOOP;
		RAISE NOTICE 'aggregates over keys planned: %', found;
	END $$;
	SELECT leaf_pages >= 3 AS three_leaves_or_more FROM lion_index_stats('wa_k');
}
step s3_parked	{ SELECT count(*) = 1 AS parked FROM pg_stat_activity WHERE wait_event_type = 'InjectionPoint' AND wait_event = 'lion-entry-scan-leaf'; }
step s3_wake_once	{ SELECT injection_points_wakeup('lion-entry-scan-leaf'); }
step s3_vacuum	{ VACUUM (INDEX_CLEANUP ON) wa; }
step s3_allvis	{
	SELECT pg_relation_size('wa') = (SELECT before FROM wa_pages) AS same_pages,
		   (SELECT relallvisible FROM pg_class WHERE relname = 'wa') = pg_relation_size('wa') / 8192 AS all_visible_again;
}
step s3_release	{
	SELECT injection_points_detach('lion-entry-scan-leaf');
	DO $$
	DECLARE n int; zero int := 0;
	BEGIN
		FOR i IN 1 .. 6000 LOOP
			SELECT count(*) INTO n FROM pg_stat_activity
			 WHERE wait_event = 'lion-entry-scan-leaf';
			IF n = 0 THEN
				zero := zero + 1;
				EXIT WHEN zero >= 10;
			ELSE
				zero := 0;
				BEGIN
					PERFORM injection_points_wakeup('lion-entry-scan-leaf');
				EXCEPTION WHEN OTHERS THEN NULL;
				END;
			END IF;
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
	s3_wake_once		# s1 reads one leaf, parks before the next
	s3_parked
	s3_vacuum			# the aborted TIDs go, the pages are marked again
	s3_allvis
	s3_release			# s1 finishes and keeps the headers' sums
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
	s3_wake_once
	s3_parked
	s3_vacuum
	s3_allvis
	s3_release
	s3_reference
	s3_after
	s3_verify
