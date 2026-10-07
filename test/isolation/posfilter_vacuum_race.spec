# The count pushdown deciding a phrase from stored positions (DESIGN.md §17,
# "Stored positions") under the visibility-map interlock of DESIGN.md
# section 9: the design note's posfilter_vacuum_race.
#
# The count copies a container, keeps its page pinned, asks the map about its
# heap blocks and only then lets the pin go; the position filter runs in that
# window, after the injection point "lion-count-containers-pinned" and before
# the map is asked (lion_count_container_masks()).  A count parked there while
# VACUUM removes dead rows must hold VACUUM off at the pinned page, and its
# answer must be the one a sequential scan under its snapshot gives.
#
# 4000 rows: id % 4 = 1 is 'alpha beta', which the phrase alpha <-> beta
# matches; id % 4 = 2 is 'beta alpha', which has both lexemes and is a
# candidate the positions turn away; the rest is 'gamma'.  The heap is 34
# pages, so each posting set is one container and the injection point is
# reached once.  s2_predel kills 50 matching rows before s1's snapshot,
# s2_delete 500 more after it: s1 counts 950, and a count after both, 450.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE TABLE pv_race (id int, d tsvector);
	INSERT INTO pv_race
	SELECT i, CASE i % 4 WHEN 1 THEN 'alpha:1 beta:2 gamma:3'
						 WHEN 2 THEN 'beta:1 alpha:2 gamma:3'
						 ELSE 'gamma:1' END::tsvector
	FROM generate_series(1, 4000) i;
	CREATE INDEX pv_race_d ON pv_race USING lion (d tsvector_pos_ops);

	CREATE FUNCTION pv_race_pushed() RETURNS boolean
	LANGUAGE plpgsql AS $fn$
	DECLARE
		l text;
	BEGIN
		FOR l IN EXPLAIN (COSTS OFF)
			SELECT count(*) FROM pv_race WHERE d @@ 'alpha <-> beta' LOOP
			RETURN l LIKE 'Custom Scan (LionCount)%';
		END LOOP;
		RETURN false;
	END $fn$;

	CREATE FUNCTION pv_race_wait_for_vacuum() RETURNS boolean
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
	DROP FUNCTION pv_race_wait_for_vacuum();
	DROP FUNCTION pv_race_pushed();
	DROP TABLE pv_race;
}

# The counting session: the phrase count goes through the count pushdown,
# which pv_race_pushed() checks of the plan without running it.
session s1
setup
{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('lion-count-containers-pinned', 'wait');
}
step s1_begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1_snap	{ SELECT pv_race_pushed() AS pushed; }
step s1_count	{ SELECT count(*) FROM pv_race WHERE d @@ 'alpha <-> beta'; }
step s1_commit	{ COMMIT; }

session s2
step s2_predel	{ DELETE FROM pv_race WHERE id > 3800; }
step s2_delete	{ DELETE FROM pv_race WHERE id % 4 = 1 AND id <= 2000; }
step s2_wakeup	{
	SELECT pv_race_wait_for_vacuum() AS vacuum_waited_for_the_pin;
	SELECT injection_points_wakeup('lion-count-containers-pinned');
}
step s2_detach	{ SELECT injection_points_detach('lion-count-containers-pinned'); }

session s3
setup			{ SET pg_lion.enable_count_pushdown = off; SET enable_bitmapscan = off; SET enable_indexscan = off; }
step s3_prep	{ VACUUM pv_race; }
step s3_vacuum	{ VACUUM pv_race; }
step s3_count	{ RESET enable_bitmapscan; RESET enable_indexscan; SET pg_lion.enable_count_pushdown = on; SELECT count(*) FROM pv_race WHERE d @@ 'alpha <-> beta'; }
step s3_seq		{ SELECT count(*) FROM pv_race WHERE d @@ 'alpha <-> beta'; }
step s3_verify	{ SELECT lion_index_verify('pv_race_d', true); }

permutation
	s3_prep					# make the heap all-visible
	s2_predel				# 50 matching rows nobody can see any more
	s1_begin s1_snap		# s1's snapshot
	s2_delete				# 500 more, committed after s1's snapshot
	s1_count				# parks with the container pinned, positions unread
	s3_vacuum(*, s1_count)	# waits for the cleanup lock
	s2_wakeup				# asserts the wait, then releases s1: 950
	s1_commit
	s3_seq					# 450, by the heap
	s3_count				# 450, from the positions
	s3_verify
	s2_detach
