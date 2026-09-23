# VACUUM's entry-page retry must not mistake a pushed-down root for a leaf
# (DESIGN.md §11, §22).
#
# Pass 2 cleanup-locks a container page, filters it, then needs the entry page
# for the counters.  If an insert holds that page, VACUUM lets go of the
# container page (waiting for a directory leaf while holding a container page
# would invert the lock order), takes the entry page blocking, and retries the
# cleanup lock.  While the container page was unlocked anything may have
# happened to it: in particular the insert that held the entry page may have
# overflowed this one-page set and pushed its root down, leaving an INTERNAL
# page of downlinks at the block VACUUM is about to re-filter.  Filtering that
# page as a leaf reads pivots as containers: an assertion on a cassert build,
# skipped postings and dead TIDs left behind on a release build.  The rule that
# closes it: after the retry, re-check the page type and restart the descent,
# exactly as the first acquisition does.
#
# Two injection points make the window deterministic:
#   lion-vacuum-page-filtered  VACUUM has filtered the page and still holds
#                              its cleanup lock; the entry page is not tried yet
#   lion-vacuum-entry-busy     the entry page was busy; VACUUM let go of the
#                              container page and holds nothing but its pin
#
# Sequence: VACUUM parks at the first point on k = 1's page (the only entry
# with dead TIDs).  s2's INSERT takes the directory leaf, descends to that same
# page and blocks on VACUUM's cleanup lock.  s3 waits until s2 is waiting,
# then wakes VACUUM: the entry page is busy (s2 holds it), so VACUUM unlocks
# the container page and parks at the second point.  s2 now runs to completion
# and pushes the root down (height 0 -> 1, root block kept).  s3 wakes VACUUM,
# which retries the cleanup lock on the old root - now internal - and must
# restart its descent.  Afterwards every dead TID is gone and verify() is
# clean.
setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE TABLE vrp (id int, k int NOT NULL);
	INSERT INTO vrp SELECT i, i % 4 FROM generate_series(1, 4000) i;
	CREATE INDEX vrp_k ON vrp USING lion (k) WITH (inline_limit = 64);
	CREATE FUNCTION vrp_wait_for_insert() RETURNS boolean
	LANGUAGE plpgsql AS $fn$
	DECLARE
		waited boolean := false;
	BEGIN
		FOR i IN 1 .. 3000 LOOP
			PERFORM pg_stat_clear_snapshot();
			SELECT count(*) > 0 INTO waited
			  FROM pg_stat_activity
			 WHERE datname = current_database()
			   AND pid <> pg_backend_pid()
			   AND state = 'active'
			   AND query ~ '^\s*INSERT'
			   /* a buffer content lock: 'Buffer'/'BufferExclusive' on this
			    * server, 'LWLock'/'BufferContent' on older ones */
			   AND wait_event_type IN ('Buffer', 'LWLock');
			EXIT WHEN waited;
			PERFORM pg_sleep(0.01);
		END LOOP;
		RETURN waited;
	END $fn$;
}
teardown
{
	DROP TABLE vrp;
	DROP FUNCTION vrp_wait_for_insert();
	DO $$ BEGIN PERFORM injection_points_detach('lion-vacuum-page-filtered');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
	DO $$ BEGIN PERFORM injection_points_detach('lion-vacuum-entry-busy');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

# The vacuuming session.
session s1
setup
{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('lion-vacuum-page-filtered', 'wait');
	SELECT injection_points_attach('lion-vacuum-entry-busy', 'wait');
}
step s1_vacuum	{ VACUUM (INDEX_CLEANUP ON) vrp; }
# In the VACUUM's own session, so that it cannot run before the VACUUM is done:
# every dead TID must be gone (ntids = rows), and the index must agree with
# the heap.
step s1_check	{
	SELECT (SELECT sum(ntids) FROM lion_index_stats('vrp_k')) AS ntids,
		   (SELECT count(*) FROM vrp) AS rows,
		   (SELECT count(*) FROM vrp WHERE k = 1) AS k1_seq,
		   lion_index_count('vrp_k', 1) AS k1_index;
	SELECT lion_index_verify('vrp_k', true);
	DROP TABLE vrp_root;
}

# The writer.
session s2
setup			{ SET synchronous_commit = on; }
step s2_predel	{ DELETE FROM vrp WHERE k = 1 AND id <= 2000; }
# Every other row, so the new members are ARRAY and BITSET containers that
# overflow the page (consecutive offsets would compress into a short RUN).
step s2_grow	{
	INSERT INTO vrp SELECT 100000 + i, CASE WHEN i % 2 = 0 THEN 1 ELSE 5 END
	  FROM generate_series(1, 40000) i;
}

# The bookkeeping session.
session s3
setup			{ SET synchronous_commit = on; }
step s3_prep	{ VACUUM (FREEZE) vrp; }
step s3_root	{
	CREATE TABLE vrp_root AS SELECT lion_index_posting_root('vrp_k', 1) AS root;
}
step s3_before	{
	SELECT max_posting_height AS height_before FROM lion_index_stats('vrp_k');
}
step s3_wake1	{
	SELECT vrp_wait_for_insert() AS insert_blocked_on_vacuum;
	SELECT injection_points_detach('lion-vacuum-page-filtered');
	SELECT injection_points_wakeup('lion-vacuum-page-filtered');
}
step s3_after	{
	SELECT max_posting_height AS height_after,
		   lion_index_posting_root('vrp_k', 1) = (SELECT root FROM vrp_root)
			 AS root_block_kept
	  FROM lion_index_stats('vrp_k');
}
step s3_wake2	{
	SELECT injection_points_detach('lion-vacuum-entry-busy');
	SELECT injection_points_wakeup('lion-vacuum-entry-busy');
}


permutation
	s3_prep					# all-visible heap
	s3_root					# the root's block number
	s2_predel				# 500 dead rows under k = 1, heap not vacuumed
	s3_before				# a one-page set: the root is the leaf
	s1_vacuum(*, s3_wake2)	# parks holding the cleanup lock on that page
	s2_grow(*, s3_wake1)	# takes the entry page, blocks on the cleanup lock
	s3_wake1				# VACUUM finds the entry page busy, parks unlocked
	s3_after				# s2 has pushed the root down: internal, same block
	s3_wake2				# VACUUM retries on the old root
	s1_check
