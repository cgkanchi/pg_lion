# A range count through the pushdown racing a VACUUM (DESIGN.md §28): the
# range bounds the entry walk that drives the count, and each entry of the
# walk is counted under the visibility-map interlock of §9 exactly as a
# single-key count is.  This is count_vacuum_race.spec for the two shapes a
# range drives: the sum over the range (`count(*) WHERE k BETWEEN ...`) and a
# GROUP BY over it.
#
# The table is laid out so that each key's rows have heap pages of their own:
# k = 0 is ids 1-1000, k = 1 ids 1001-2000, and so on.
#
#  * s2_predel deletes every row of k = 0 and commits before s1 takes its
#    snapshot, so they are dead to everyone and VACUUM may remove them - and,
#    having removed them, mark their pages all-visible.
#  * s2_delete then deletes every row of k = 2 and commits after s1's
#    snapshot, so s1 still sees them: their pages are dirty and go through the
#    heap recheck, under s1's snapshot.
#  * k = 1 is on all-visible pages and is settled from the map; k = 3 is
#    outside the range.
#
# The walk is in key order and starts at the range's lower bound, so the FIRST
# container s1's count puts through the visibility map is k = 0's, and it
# parks there, on the existing injection point 'lion-count-containers-pinned',
# with that container's page pinned.  A VACUUM started now must wait for the
# pin (cnt_r_wait_for_vacuum() asserts it); had it not, it could remove
# k = 0's TIDs and set their pages all-visible before s1 asks the map about
# the copy it holds, and s1 would count k = 0's thousand dead rows: 3000
# instead of 2000.
#
# s2_wakeup DETACHES the point before it wakes s1, because the walk reaches it
# once per container and only the first one is the interesting one; the rest
# of the walk then races the VACUUM freely, and the answer must be exact
# however that race goes.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE TABLE cnt_r (id int, k int NOT NULL);
	INSERT INTO cnt_r SELECT i, (i - 1) / 1000 FROM generate_series(1, 4000) i;
	CREATE INDEX cnt_r_k ON cnt_r USING lion (k);

	CREATE FUNCTION cnt_r_wait_for_vacuum() RETURNS boolean
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
	DROP FUNCTION cnt_r_wait_for_vacuum();
	DROP TABLE cnt_r;
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
	SET enable_indexscan = off;
	SELECT injection_points_set_local();
	SELECT injection_points_attach('lion-count-containers-pinned', 'wait');
}
step s1_plan	{
	EXPLAIN (COSTS OFF) SELECT count(*) FROM cnt_r WHERE k BETWEEN 0 AND 2;
	EXPLAIN (COSTS OFF) SELECT k, count(*) FROM cnt_r WHERE k < 3 GROUP BY k ORDER BY k;
}
step s1_begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1_plain	{
	SET LOCAL pg_lion.enable_count_pushdown = off;
	SELECT count(*) FROM cnt_r WHERE k BETWEEN 0 AND 2;
	SET LOCAL pg_lion.enable_count_pushdown = on;
}
step s1_count	{ SELECT count(*) FROM cnt_r WHERE k BETWEEN 0 AND 2; }
step s1_group	{ SELECT k, count(*) FROM cnt_r WHERE k < 3 GROUP BY k ORDER BY k; }
step s1_commit	{ COMMIT; }

# The writer, which also releases s1 from the injection point.
session s2
setup			{ SET pg_lion.enable_count_pushdown = off; }
step s2_predel	{ DELETE FROM cnt_r WHERE k = 0; }
step s2_delete	{ DELETE FROM cnt_r WHERE k = 2; }
# Assert the wait, then stop the point from firing again and release s1.
step s2_wakeup	{
	SELECT cnt_r_wait_for_vacuum() AS vacuum_waited_for_the_pin;
	SELECT injection_points_detach('lion-count-containers-pinned');
	SELECT injection_points_wakeup('lion-count-containers-pinned');
}

# The vacuuming session.
session s3
setup			{ SET pg_lion.enable_count_pushdown = off; }
step s3_prep	{ VACUUM cnt_r; }
step s3_vacuum	{ VACUUM cnt_r; }
step s3_count	{
	SELECT count(*) FROM cnt_r WHERE k BETWEEN 0 AND 2;
	SELECT k, count(*) FROM cnt_r WHERE k < 3 GROUP BY k ORDER BY k;
}
step s3_pushed	{
	SET pg_lion.enable_count_pushdown = on;
	SELECT count(*) FROM cnt_r WHERE k BETWEEN 0 AND 2;
	SELECT k, count(*) FROM cnt_r WHERE k < 3 GROUP BY k ORDER BY k;
	RESET pg_lion.enable_count_pushdown;
}
step s3_verify	{ SELECT lion_index_verify('cnt_r_k', true); }

# The sum over the range: VACUUM waits for the pin on k = 0's container, and
# the count is 2000.
permutation
	s3_prep					# make the heap all-visible
	s1_plan					# the plans are the pushdown's
	s2_predel				# k = 0: rows nobody can see any more
	s1_begin s1_plain		# s1's snapshot: 2000 rows in the range
	s2_delete				# k = 2, committed after s1's snapshot
	s1_count				# parks with k = 0's container pinned
	s3_vacuum(*, s1_count)	# blocks for the cleanup lock; cannot finish
							# before s1_count lets go of the pin
	s2_wakeup				# asserts the wait, then releases s1
	s1_commit
	s3_count				# 1000 now: k = 1
	s3_pushed				# and the same through the pushdown
	s3_verify

# The GROUP BY over the range: the same race, per group.
permutation
	s3_prep
	s2_predel
	s1_begin s1_plain
	s2_delete
	s1_group				# parks with k = 0's container pinned
	s3_vacuum(*, s1_group)
	s2_wakeup
	s1_commit
	s3_count
	s3_pushed
	s3_verify
