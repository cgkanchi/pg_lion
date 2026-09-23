# count(DISTINCT k) through the pushdown racing a VACUUM (DESIGN.md §26): the
# visibility-map interlock of §9 covers an EXISTENCE test exactly as it covers
# a count, because an existence test reads the same containers the same way
# and only stops earlier.  This is count_vacuum_race.spec for the distinct
# count, both shapes.
#
# The table is laid out so that each key's rows have heap pages of their own:
# k = 0 is ids 1-1000, k = 1 ids 1001-2000, and so on, and g alternates.
#
#  * s2_predel deletes every row of k = 0 and commits before s1 takes its
#    snapshot, so they are dead to everyone and VACUUM may remove them - and,
#    having removed them, mark their pages all-visible.
#  * s2_delete then deletes every row of k = 2 and commits after s1's
#    snapshot, so s1 still sees them: their pages are dirty and go through the
#    heap recheck, under s1's snapshot.
#  * k = 1 and k = 3 are on all-visible pages and are settled from the map.
#
# The entry walk is in key order, so the FIRST container s1's distinct count
# puts through the visibility map is k = 0's (shape 1) or the first group's,
# which holds k = 0's rows (shape 2), and it parks there, on the existing
# injection point 'lion-count-containers-pinned', with that container's page
# pinned.  A VACUUM started now must wait for the pin (cnt_d_wait_for_vacuum()
# asserts it); had it not, it could remove k = 0's TIDs and set their pages
# all-visible before s1 asks the map about the copy it holds, and s1 would
# count k = 0: 4 distinct keys instead of 3.
#
# s2_wakeup DETACHES the point before it wakes s1, because the walk reaches
# it once per container and only the first one is the interesting one; the
# rest of the walk then races the VACUUM freely, and the answer must be exact
# however that race goes.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE TABLE cnt_d (id int, g int NOT NULL, k int NOT NULL);
	INSERT INTO cnt_d SELECT i, i % 2, (i - 1) / 1000 FROM generate_series(1, 4000) i;
	CREATE INDEX cnt_d_k ON cnt_d USING lion (k);
	CREATE INDEX cnt_d_g ON cnt_d USING lion (g);

	CREATE FUNCTION cnt_d_wait_for_vacuum() RETURNS boolean
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
	DROP FUNCTION cnt_d_wait_for_vacuum();
	DROP TABLE cnt_d;
	/* Injection points are cluster-wide: never leave one attached, and
	 * never fail the teardown because the permutation already did. */
	DO $$ BEGIN PERFORM injection_points_detach('lion-count-containers-pinned');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

# The counting session.  Only the pushdown's steps run into the point: the
# reference counts turn it off.
session s1
setup
{
	SET pg_lion.enable_count_pushdown = on;
	SET enable_seqscan = off;
	SET enable_bitmapscan = off;
	SELECT injection_points_set_local();
	SELECT injection_points_attach('lion-count-containers-pinned', 'wait');
}
step s1_plan	{
	EXPLAIN (COSTS OFF) SELECT count(DISTINCT k) FROM cnt_d;
	EXPLAIN (COSTS OFF) SELECT g, count(DISTINCT k) FROM cnt_d GROUP BY g;
}
step s1_begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1_plain	{
	SET LOCAL pg_lion.enable_count_pushdown = off;
	SELECT count(DISTINCT k) FROM cnt_d;
	SET LOCAL pg_lion.enable_count_pushdown = on;
}
step s1_count	{ SELECT count(DISTINCT k) FROM cnt_d; }
step s1_group	{ SELECT g, count(DISTINCT k) FROM cnt_d GROUP BY g ORDER BY g; }
step s1_commit	{ COMMIT; }

# The writer, which also releases s1 from the injection point.
session s2
setup			{ SET pg_lion.enable_count_pushdown = off; }
step s2_predel	{ DELETE FROM cnt_d WHERE k = 0; }
step s2_delete	{ DELETE FROM cnt_d WHERE k = 2; }
# Assert the wait, then stop the point from firing again and release s1.
step s2_wakeup	{
	SELECT cnt_d_wait_for_vacuum() AS vacuum_waited_for_the_pin;
	SELECT injection_points_detach('lion-count-containers-pinned');
	SELECT injection_points_wakeup('lion-count-containers-pinned');
}

# The vacuuming session.
session s3
setup			{ SET pg_lion.enable_count_pushdown = off; }
step s3_prep	{ VACUUM cnt_d; }
step s3_vacuum	{ VACUUM cnt_d; }
step s3_count	{
	SELECT count(DISTINCT k) FROM cnt_d;
	SELECT g, count(DISTINCT k) FROM cnt_d GROUP BY g ORDER BY g;
}
step s3_pushed	{
	SET pg_lion.enable_count_pushdown = on;
	SELECT count(DISTINCT k) FROM cnt_d;
	SELECT g, count(DISTINCT k) FROM cnt_d GROUP BY g ORDER BY g;
	RESET pg_lion.enable_count_pushdown;
}
step s3_verify	{
	SELECT lion_index_verify('cnt_d_k', true);
	SELECT lion_index_verify('cnt_d_g', true);
}

# Shape 1: VACUUM waits for the pin on k = 0's container, and the count is 3.
permutation
	s3_prep					# make the heap all-visible
	s1_plan					# the plans are the pushdown's
	s2_predel				# k = 0: rows nobody can see any more
	s1_begin s1_plain		# s1's snapshot: 3 distinct keys
	s2_delete				# k = 2, committed after s1's snapshot
	s1_count				# parks with k = 0's container pinned
	s3_vacuum(*, s1_count)	# blocks for the cleanup lock; cannot finish
							# before s1_count lets go of the pin
	s2_wakeup				# asserts the wait, then releases s1
	s1_commit
	s3_count				# 2 now: k = 1 and k = 3
	s3_pushed				# and the same through the pushdown
	s3_verify

# Shape 2: the same race, per group.
permutation
	s3_prep
	s2_predel
	s1_begin s1_plain
	s2_delete
	s1_group				# parks with the first group's container pinned
	s3_vacuum(*, s1_group)
	s2_wakeup
	s1_commit
	s3_count
	s3_pushed
	s3_verify
