# The visibility-map interlock of DESIGN.md section 9, and the cancellability
# of the wait it imposes on VACUUM (DESIGN.md section 11), shown to be real.
#
# lion_count.c copies containers out of an index page, keeps the PIN on that
# page, and only then asks the visibility map about the heap blocks those
# containers cover.  The injection point "lion-count-containers-pinned"
# fires exactly in that window.  ambulkdelete takes LockBufferForCleanup() on
# the index pages it rewrites (DESIGN.md section 11), and a cleanup lock waits
# for every pin to go away, so a VACUUM that runs while a counting backend is
# parked at the injection point MUST block.
#
# cnt_race_wait_for_vacuum() watches pg_stat_activity until the VACUUM is seen
# parked on wait_event 'BufferCleanup' ('BufferPin' before PostgreSQL 19).
# It returns false if the VACUUM finishes without ever being seen there, and
# a false in the expected output means the interlock is gone -- a count could then be told a heap page is
# all-visible after VACUUM removed the dead TIDs it had already read out of
# the index, which is exactly how this would produce a wrong answer.
#
# The two permutations are:
#
#  * "waits for the pin": VACUUM blocks, the reader finishes, VACUUM then
#    completes by itself.
#  * "cancelled while waiting": the same VACUUM runs under a one second
#    statement_timeout and dies of it *while it is still waiting*, before
#    anything wakes the reader.  ambulkdelete never waits for a cleanup lock
#    while holding another buffer's content lock (an LWLock, which would imply
#    HOLD_INTERRUPTS and swallow the cancel), so the timeout is delivered in
#    the wait itself; the reader then finishes normally and the index is still
#    consistent afterwards.
#
# isolationtester only recognises *heavyweight* lock waits by itself, and a
# cleanup lock is not one, hence the (*) marker on the VACUUM steps: it makes
# the tester report the step as waiting as soon as it is launched instead of
# sitting on it.  The "s1_count" marker in the first permutation keeps the
# report of VACUUM's completion after the reader's, which is the whole point
# of that permutation.
#
# Why s1's count is the pre-delete value (950), worked through per section 9:
#
#  * 1000 rows have k = 1.  s2_predel deletes 50 of them (id > 3800) and
#    commits before s1 takes its snapshot, so they are dead to everyone.
#  * s2_delete then removes 500 more (id <= 2000) and commits.  s1's snapshot
#    is older, so those rows are still visible to s1.
#  * Both deletes cleared the all-visible bit on the heap pages they touched,
#    so every block holding a deleted row goes through the heap recheck, where
#    table_fetch_tid() applies s1's snapshot: the 500 rows s2 deleted are
#    counted, the 50 dead ones are not.
#  * The heap blocks neither delete touched are still all-visible, and every
#    TID the index lists for them was alive at s1's snapshot, so their members
#    are counted straight from the map without a heap visit.
#  * 950 either way -- and the interlock is what stops VACUUM from flipping a
#    block to all-visible between the moment we read the container and the
#    moment we probe the map.
#
# The table is sized so that k = 1's posting set is a single container (4000
# rows occupy far fewer than the 64 heap blocks a container covers), so the
# injection point is reached exactly once per count.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE TABLE cnt_race (id int, k int NOT NULL);
	INSERT INTO cnt_race SELECT i, i % 4 FROM generate_series(1, 4000) i;
	CREATE INDEX cnt_race_k ON cnt_race USING lion (k);

	CREATE FUNCTION cnt_race_wait_for_vacuum() RETURNS boolean
	LANGUAGE plpgsql AS $fn$
	DECLARE
		waited boolean := false;
		active boolean;
		seen boolean := false;
	BEGIN
		FOR i IN 1 .. 3000 LOOP
			/* pg_stat_activity is otherwise read once per transaction */
			PERFORM pg_stat_clear_snapshot();
			SELECT count(*) FILTER (WHERE wait_event IN ('BufferCleanup', 'BufferPin')) > 0,
				   count(*) > 0
			  INTO waited, active
			  FROM pg_stat_activity
			 WHERE datname = current_database()
			   AND pid <> pg_backend_pid()
			   AND state = 'active'
			   AND query LIKE 'VACUUM%';
			EXIT WHEN waited;
			seen := seen OR active;
			/* the VACUUM came and went without ever waiting for the pin */
			EXIT WHEN seen AND NOT active;
			PERFORM pg_sleep(0.01);
		END LOOP;
		RETURN waited;
	END $fn$;
}

teardown
{
	DROP FUNCTION cnt_race_wait_for_vacuum();
	DROP TABLE cnt_race;
}

# The counting session.  The pushdown is off so that its plain count(*) does
# not run into the injection point; only lion_index_count() does.
session s1
setup
{
	SET pg_lion.enable_count_pushdown = off;
	SELECT injection_points_set_local();
	SELECT injection_points_attach('lion-count-containers-pinned', 'wait');
}
step s1_begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1_plain	{ SELECT count(*) FROM cnt_race WHERE k = 1; }
step s1_count	{ SELECT lion_index_count('cnt_race_k', 1); }
step s1_commit	{ COMMIT; }

# The writer, which also releases s1 from the injection point.
session s2
setup			{ SET pg_lion.enable_count_pushdown = off; }
# Rows that are dead to everybody, so the VACUUM below really has index
# entries to remove and really has to take cleanup locks.
step s2_predel	{ DELETE FROM cnt_race WHERE id > 3800; }
step s2_delete	{ DELETE FROM cnt_race WHERE k = 1 AND id <= 2000; }
# Assert the wait, then release s1.
step s2_wakeup	{
	SELECT cnt_race_wait_for_vacuum() AS vacuum_waited_for_the_pin;
	SELECT injection_points_wakeup('lion-count-containers-pinned');
}
# The same assertion on its own, for the run where the VACUUM is cancelled
# before anything releases s1.
step s2_watch	{ SELECT cnt_race_wait_for_vacuum() AS vacuum_waited_for_the_pin; }
step s2_release	{ SELECT injection_points_wakeup('lion-count-containers-pinned'); }
step s2_detach	{ SELECT injection_points_detach('lion-count-containers-pinned'); }

# The vacuuming session.
session s3
setup			{ SET pg_lion.enable_count_pushdown = off; }
step s3_prep	{ VACUUM cnt_race; }
step s3_limit	{ SET statement_timeout = '1s'; }
step s3_vacuum	{ VACUUM cnt_race; }
step s3_reset	{ RESET statement_timeout; }
step s3_count	{ SELECT count(*) FROM cnt_race WHERE k = 1; }
step s3_verify	{ SELECT lion_index_verify('cnt_race_k', true); }

# VACUUM waits for the pin and finishes once the reader has let go of it.
permutation
	s3_prep					# make the heap all-visible
	s2_predel				# rows nobody can see any more
	s1_begin s1_plain		# s1's snapshot: 950 rows with k = 1
	s2_delete				# 500 more, committed after s1's snapshot
	s1_count				# blocks with the container page pinned
	s3_vacuum(*, s1_count)	# blocks for the cleanup lock; cannot finish
							# before s1_count lets go of the pin
	s2_wakeup				# asserts the wait, then releases s1
	s1_commit
	s3_count				# 450
	s3_verify
	s2_detach

# The same wait, cancelled by statement_timeout while VACUUM is still waiting
# -- s1 is only woken afterwards, and still gets the right answer.
permutation
	s3_prep
	s2_predel
	s1_begin s1_plain
	s2_delete
	s1_count				# blocks with the container page pinned
	s3_limit
	s3_vacuum(*)			# blocks for the cleanup lock, then dies of the
							# one second timeout while it is still waiting
	s2_watch				# asserts the wait; does NOT release s1
	s3_reset
	s2_release				# only now is s1 let go
	s1_commit
	s3_vacuum				# nothing in the way any more
	s3_count				# 450
	s3_verify				# the cancelled VACUUM left the index consistent
	s2_detach
