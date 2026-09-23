# A reader holding a chain's head block while VACUUM frees that chain and an
# insert takes the pages back (DESIGN.md §18).
#
# liongetbitmap() copies the entry tuple, releases the directory leaf, and only
# then walks the container chain.  Between those two it holds NOTHING of the
# posting set but the head block number: no pin, no lock.  In that window
# VACUUM may delete the entry, mark every page of the chain LION_PAGE_DELETED
# and record them in the free space map, and a later insert may take them for
# a different key's chain.  A reader that then simply followed its head block
# would emit another key's TIDs - and a plain equality scan sets no recheck
# flag, so the bitmap heap scan would hand those rows straight back: a WRONG
# ANSWER, not a slow one.
#
# What stops it is the owner check in the page's special area: every container
# page carries owner_hash and owner_head, and a head block is the one block
# lion_new_buffer() never takes from the free space map, so owner_head
# identifies a chain for the whole life of the index.  A page that does not
# claim this chain ends the walk, which is right: the chain was only freed
# because its posting set held nothing visible to anyone.
#
# The test:
#
#  * key 1 has 20000 rows and `inline_limit = 64`, so its posting set is a
#    CHAIN of several container pages.
#  * s2 deletes every row of key 1 and commits, BEFORE s1's snapshot: the
#    correct answer for s1 is 0, and VACUUM is free to remove them.
#  * s1 runs `count(*) WHERE k = 1` as a bitmap scan and parks at
#    'lion-scan-chain-entered', holding only the head block.
#  * s3 vacuums: the entry goes, the chain's pages become DELETED and go into
#    the free space map.
#  * s2 inserts 20000 rows of key 9, which spill and take those pages back -
#    `pages_reused` proves the reuse really happened.
#  * s2 releases s1, whose walk must stop at the first page that no longer
#    claims its chain and return 0.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE TABLE vcr (id int, k int NOT NULL);
	INSERT INTO vcr SELECT i, CASE WHEN i <= 20000 THEN 1 ELSE 2 END
	  FROM generate_series(1, 24000) i;
	CREATE INDEX vcr_k ON vcr USING lion (k) WITH (inline_limit = 64);
}

teardown
{
	DROP TABLE vcr;
	/* Injection points are cluster-wide: never leave one attached, and
	 * never fail the teardown because the permutation already did. */
	DO $$ BEGIN PERFORM injection_points_detach('lion-scan-chain-entered');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

# The reading session: a bitmap index scan, not the count pushdown.
session s1
setup
{
	SET pg_lion.enable_count_pushdown = off;
	SET enable_seqscan = off;
	SET enable_indexscan = off;
	SELECT injection_points_set_local();
	SELECT injection_points_attach('lion-scan-chain-entered', 'wait');
}
step s1_scan	{ SELECT count(*) AS rows_for_key_1 FROM vcr WHERE k = 1; }
step s1_again	{ SELECT count(*) AS rows_for_key_1 FROM vcr WHERE k = 1;
				  SELECT count(*) AS rows_for_key_9 FROM vcr WHERE k = 9; }

# The writer.
session s2
setup			{ SET pg_lion.enable_count_pushdown = off; }
step s2_delete	{ DELETE FROM vcr WHERE k = 1; }
step s2_freed	{
	SELECT entries, deleted_pages > 0 AS chain_was_freed
	  FROM lion_index_stats('vcr_k');
}
# Take the freed pages back for a new key.
step s2_insert	{
	INSERT INTO vcr SELECT i, 9 FROM generate_series(100001, 120000) i;
}
step s2_reused	{
	SELECT deleted_pages AS pages_left_free FROM lion_index_stats('vcr_k');
}
step s2_wakeup	{
	/*
	 * Detach first, so that nothing can park at the point again, then
	 * release whoever is parked now.  How MANY times the point is reached is
	 * not fixed - the two-column driver runs more than one entry scan, and a
	 * scan fires the point once per leaf it consumes an entry from -
	 * but after this step nobody is waiting on it, which is all the
	 * permutation needs.  injection_points_wakeup() searches the WAITERS, so
	 * it still works after the detach.
	 */
	SELECT injection_points_detach('lion-scan-chain-entered');
	DO $$
	DECLARE n int; zero int := 0;
	BEGIN
		FOR i IN 1 .. 6000 LOOP
			SELECT count(*) INTO n FROM pg_stat_activity
			 WHERE wait_event = 'lion-scan-chain-entered';
			IF n = 0 THEN
				zero := zero + 1;
				EXIT WHEN zero >= 10;
			ELSE
				zero := 0;
				/*
				 * The waiter clears its slot itself once it notices the
				 * counter, so it can be gone between the poll above and
				 * this call; that is a success, not a failure.
				 */
				BEGIN
					PERFORM injection_points_wakeup('lion-scan-chain-entered');
				EXCEPTION WHEN OTHERS THEN NULL;
				END;
			END IF;
			PERFORM pg_sleep(0.01);
		END LOOP;
	END $$;
}

# The vacuuming session.
session s3
setup			{ SET pg_lion.enable_count_pushdown = off; }
step s3_prep	{ VACUUM (FREEZE, ANALYZE) vcr; }
step s3_vacuum	{ VACUUM vcr; }
step s3_verify	{ SELECT lion_index_verify('vcr_k', true); }

# The parked step carries no (*) marker: isolationtester sees a session
# waiting on an injection point as blocked (pg_isolation_test_session_is_blocked
# checks for it), so it moves on only once the step has really parked.  (*)
# would move on at once, and the next step could then race the park - a slow
# backend reached the point only after VACUUM had already run.  The marker in
# parentheses pins the report of its completion after the step that releases
# it.
permutation
	s3_prep					# make the heap all-visible
	s2_delete				# key 1 is dead to everyone from here on
	s1_scan(s2_wakeup)	# parks holding nothing but the head block
	s3_vacuum				# entry deleted, chain freed, pages in the FSM
	s2_freed
	s2_insert				# key 9 spills into the pages key 1 gave up
	s2_reused
	s2_wakeup				# detaches the point, then releases s1, whose walk
							# resumes from the stale head block
	s1_again				# and the index still answers both keys correctly
	s3_verify
