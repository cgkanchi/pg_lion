# A reader of a store in key-ordered windows (DESIGN.md §41) against the
# insert that splits a bucket of its window, moving its rows.
#
# lion_store_gather() reads an ordered window in two walks, the
# permutation's chain and then the column's, each from a head it read from
# the map with no lock or pin held; the injection point
# "lion-store-gather-head" parks it after each of those reads.  Between the
# two walks it holds nothing either, and joins what the permutation said
# with what the column's pages hold only if the window's generation, read
# from the window header with the permutation, is the same after the
# column's walk: a split that moves rows writes them at their new positions,
# then the permutation, then the header with the next generation, and only
# then clears their old positions (§41, "Why it is safe").
#
# The window is sorted at build into 64 buckets of about 47 rows; 260 rows
# of one key in the middle of a bucket fill it, and it splits.  The reader
# is lion_index_stored() of a row of that key, one the split moves, checked
# against the heap after:
#
#  * before the permutation: parked with the permutation's head in hand;
#  * between the walks: parked with the permutation's answer in hand - the
#    position it names is cleared by the split, the generation has moved,
#    and the window is read again.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE TABLE stsp (k int, t text) WITH (autovacuum_enabled = off);
	INSERT INTO stsp SELECT i, 'v' || i FROM generate_series(1, 3000) i;
	SET pg_lion.store_order_min_pages = 0;
	CREATE INDEX stsp_i ON stsp USING lion (k) INCLUDE (t)
		WITH (store_values = on, cluster_column = k);
}

teardown
{
	DROP TABLE stsp;
	DO $$ BEGIN PERFORM injection_points_detach('lion-store-gather-head');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

session s1
setup
{
	SET enable_indexscan = off;
	SET enable_indexonlyscan = off;
	SET enable_bitmapscan = off;
	SELECT injection_points_set_local();
	SELECT injection_points_attach('lion-store-gather-head', 'wait');
}
step s1_read	{
	SELECT lion_index_stored('stsp_i', (SELECT ctid FROM stsp WHERE t = 'v1500'))
		AS stored;
}
step s1_check	{
	SELECT k, t, lion_index_stored('stsp_i', ctid) AS stored
	  FROM stsp WHERE t = 'v1500';
	SELECT count(*) AS mismatched FROM stsp
	 WHERE lion_index_stored('stsp_i', ctid) IS DISTINCT FROM ARRAY[k::text, t];
	SELECT lion_index_verify('stsp_i', true);
}

session s2
step s2_split	{
	INSERT INTO stsp SELECT 1500, 'n' || i FROM generate_series(1, 260) i;
}
step s2_window	{
	SELECT generation, buckets, slots, entries
	  FROM lion_index_store_window('stsp_i', 0);
}
# the reader is parked again, at the column's head
step s2_parked	{
	DO $$
	BEGIN
		FOR i IN 1 .. 3000 LOOP
			EXIT WHEN EXISTS (SELECT 1 FROM pg_stat_activity
							   WHERE wait_event = 'lion-store-gather-head');
			PERFORM pg_sleep(0.01);
			PERFORM pg_stat_clear_snapshot();
		END LOOP;
	END $$;
}
step s2_wakeup_once	{ SELECT injection_points_wakeup('lion-store-gather-head'); }
step s2_detach		{ SELECT injection_points_detach('lion-store-gather-head'); }
# (*, s1_read): the wakeup is reported as waiting, and complete once the
# reader it woke is, as in store_vacuum_reader.spec; s2_window_after, a step
# of its own session, makes the tester print that before s1_check on every
# major.
step s2_wakeup		{ SELECT injection_points_wakeup('lion-store-gather-head'); }
step s2_window_after	{
	SELECT generation, buckets, slots, entries
	  FROM lion_index_store_window('stsp_i', 0);
}

# the split under the permutation's head
permutation
	s2_window
	s1_read				# parks with the permutation's head in hand
	s2_split
	s2_window
	s2_detach
	s2_wakeup(*, s1_read)
	s2_window_after
	s1_check

# the split between the permutation's walk and the column's
permutation
	s1_read
	s2_wakeup_once		# on to the column's head, where it parks again
	s2_parked
	s2_split
	s2_window
	s2_detach
	s2_wakeup(*, s1_read)
	s2_window_after
	s1_check
