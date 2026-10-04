# A reader of a store in key-ordered windows (DESIGN.md §41) against the
# VACUUM that sorts its window again.
#
# lion_store_gather() reads an ordered window in two walks, the
# permutation's chain and then the column's, each from a head it read from
# the map with no lock or pin held; the injection point
# "lion-store-gather-head" parks it after each of those reads.  Between the
# two walks it holds nothing either, and joins what the permutation said
# with what the column's pages hold only if both are of one generation: a
# sort writes every chain of the window again under a new generation and
# frees the old pages, and a reader that finds a freed page, or one of
# another generation, reads the whole window again (§41, "Why it is safe").
#
# Three rows in four die, so VACUUM sorts the window again; the reader is
# lion_index_stored() of a row that lives, checked against the heap after:
#
#  * before the permutation: parked with the permutation's old head, which
#    the sort frees;
#  * between the walks: parked with the old permutation's answer in hand and
#    the column's old head, both freed by the sort - the new pages are of
#    the next generation, and the window is read again.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE TABLE stsr (k int, t text) WITH (autovacuum_enabled = off);
	INSERT INTO stsr SELECT i, 'v' || i FROM generate_series(1, 3000) i;
	CREATE INDEX stsr_i ON stsr USING lion (k) INCLUDE (t)
		WITH (store_values = on, cluster_column = k);
}

teardown
{
	DROP TABLE stsr;
	DO $$ BEGIN PERFORM injection_points_detach('lion-store-gather-head');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

session s1
setup
{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('lion-store-gather-head', 'wait');
}
step s1_read	{ SELECT lion_index_stored('stsr_i', '(0,1)') AS stored; }
step s1_check	{
	SELECT k, t, lion_index_stored('stsr_i', ctid) AS stored
	  FROM stsr WHERE ctid = '(0,1)';
	SELECT lion_index_verify('stsr_i', true);
}

session s2
step s2_delete_most	{ DELETE FROM stsr WHERE k % 4 <> 1; }
step s2_window		{
	SELECT generation, nsorted, entries, appended
	  FROM lion_index_store_window('stsr_i', 0);
}
step s2_vacuum		{ VACUUM stsr; }
# the reader is parked again, at the column's head
step s2_parked		{
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
# reader it woke is, as in store_vacuum_reader.spec
step s2_wakeup		{ SELECT injection_points_wakeup('lion-store-gather-head'); }

# the sort under the permutation's head
permutation
	s2_delete_most
	s2_window
	s1_read				# parks with the permutation's head in hand
	s2_vacuum
	s2_window
	s2_detach
	s2_wakeup(*, s1_read)
	s1_check

# the sort between the permutation's walk and the column's
permutation
	s2_delete_most
	s1_read
	s2_wakeup_once		# on to the column's head, where it parks again
	s2_parked
	s2_vacuum
	s2_window
	s2_detach
	s2_wakeup(*, s1_read)
	s1_check
