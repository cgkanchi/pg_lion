# The visibility-map interlock of DESIGN.md section 9 when a list is counted in
# BATCHES (DESIGN.md §15, "Bounded cursors"), and the pins it holds.
#
# A list too large for the count's open budget - here 400 CHAIN entries at
# work_mem 64kB, some thirty to a batch - is merged a batch of entries at a
# time, and the passes are added up.  Each pass is an ordinary merge: its
# cursors pin the posting page each current container came from, the
# visibility map is asked under those pins, and only then do they move on.
# The list is the ONLY source here and it is dense (a hundred rows per key
# over fourteen container keys), so neither the disjoint sum nor another
# clause carries the interlock: the batch's own pins have to.
#
# The injection point "lion-count-containers-pinned" parks the count at its
# first visibility-map question, inside the first batch.  There:
#
#  * s2_pins counts the posting pages some backend pins: the first batch's,
#    some thirty.  Before batching every cursor of the list was built at once
#    and this was 400, one pinned page per value, however long the list.
#  * a VACUUM of the table must BLOCK: ambulkdelete takes a cleanup lock on
#    every posting page in chain order (DESIGN.md §11) and the first batch's
#    pages are pinned.  batch_race_wait_for_vacuum() watches pg_stat_activity
#    for it, as count_vacuum_race.spec does.
#
# s1's count is 38000 either way, which is the point of the interlock:
#
#  * 200000 rows, k = id % 2000.  s2_predel deletes id > 190000 before s1's
#    snapshot, so those rows are dead to everybody: 95 of each key's 100 rows
#    are left, and 400 keys have 38000.
#  * s2_delete then deletes 2450 more (k < 50, id <= 100000) after s1's
#    snapshot: still visible to s1, and their heap pages are no longer
#    all-visible, so the recheck counts them.
#  * If VACUUM could finish while s1 still held copies of the dead TIDs, it
#    would set their heap pages all-visible and s1 would count them from the
#    map.  It cannot: it waits for the pins.
#
# The injection point fires at every container; s2 detaches it before waking
# s1, so the count then runs to the end without stopping again.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE EXTENSION IF NOT EXISTS pg_buffercache;
	CREATE TABLE batch_race (id int, k int NOT NULL);
	INSERT INTO batch_race SELECT i, i % 2000 FROM generate_series(1, 200000) i;
	CREATE INDEX batch_race_k ON batch_race USING lion (k) WITH (inline_limit = 64);

	CREATE FUNCTION batch_race_wait_for_vacuum() RETURNS boolean
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
	DROP FUNCTION batch_race_wait_for_vacuum();
	DROP TABLE batch_race;
}

# The counting session: the pushdown answers the list, a batch at a time.
session s1
setup
{
	SET work_mem = '64kB';
	SET enable_seqscan = off;
	SET enable_bitmapscan = off;
	SELECT injection_points_set_local();
	SELECT injection_points_attach('lion-count-containers-pinned', 'wait');
}
step s1_begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1_snap	{ SELECT count(*) FROM batch_race WHERE id < 0; }
step s1_count	{
	SELECT count(*) FROM batch_race
	 WHERE k = ANY (array(SELECT g FROM generate_series(1, 400) g));
}
step s1_commit	{ COMMIT; }

session s2
setup			{ SET pg_lion.enable_count_pushdown = off; }
step s2_predel	{ DELETE FROM batch_race WHERE id > 190000; }
step s2_delete	{ DELETE FROM batch_race WHERE k < 50 AND id <= 100000; }
# the posting pages pinned while s1 is parked: its first batch's
step s2_pins	{
	SELECT count(*) BETWEEN 1 AND 64 AS pins_bounded
	  FROM pg_buffercache
	 WHERE reldatabase = (SELECT oid FROM pg_database WHERE datname = current_database())
	   AND relfilenode = pg_relation_filenode('batch_race_k')
	   AND pinning_backends > 0;
}
# Assert the wait, then let s1 run to the end.
step s2_wakeup	{
	SELECT batch_race_wait_for_vacuum() AS vacuum_waited_for_the_pin;
	SELECT injection_points_detach('lion-count-containers-pinned');
	SELECT injection_points_wakeup('lion-count-containers-pinned');
}

session s3
setup			{ SET pg_lion.enable_count_pushdown = off; }
step s3_prep	{ VACUUM batch_race; }
step s3_vacuum	{ VACUUM (INDEX_CLEANUP ON) batch_race; }
step s3_count	{
	SELECT count(*) FROM batch_race
	 WHERE k = ANY (array(SELECT g FROM generate_series(1, 400) g));
}
step s3_verify	{ SELECT lion_index_verify('batch_race_k', true); }

permutation
	s3_prep					# make the heap all-visible
	s2_predel				# rows nobody can see any more
	s1_begin s1_snap		# s1's snapshot: 38000 rows in the list
	s2_delete				# 2450 more, committed after s1's snapshot
	s1_count				# parks inside the first batch, its pages pinned
	s2_pins					# one batch's pages, not the whole list's
	s3_vacuum(*, s1_count)	# blocks for the cleanup lock
	s2_wakeup				# asserts the wait, then releases s1
	s1_commit
	s3_count				# 35550
	s3_verify
