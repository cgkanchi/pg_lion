# A lion index vacuumed by a PARALLEL VACUUM WORKER (DESIGN.md §11, §18).
#
# Lion indexes declare VACUUM_OPTION_PARALLEL_BULKDEL, so a VACUUM of a table
# with two or more of them hands each index's ambulkdelete to whichever
# participant - the leader or a worker - claims it first.  Nothing in the
# protocol depends on the process: one index is still vacuumed start to
# finish by one participant, it still cleanup-locks every page in chain order
# and never waits while holding another lock, and the heap is still only
# marked all-visible after every index is done.  This spec pins the part that
# is new: that a worker really does run lion's ambulkdelete, and that what it
# leaves behind is right.
#
# The injection point is attached GLOBALLY rather than with
# injection_points_set_local(): a local attachment belongs to the attaching
# backend, and a worker is another backend.  s1 parks every participant that
# reaches a filtered chain page (both indexes have dead TIDs in chains, so each
# does), s2 waits until both have parked and one of them is a parallel
# worker, then lets them go.  Without the option the leader vacuums both
# indexes itself and no worker ever parks: s2 reports f.
setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE TABLE vpar (id int, a int NOT NULL, b int NOT NULL);
	INSERT INTO vpar SELECT i, i % 4, i % 5 FROM generate_series(1, 20000) i;
	CREATE INDEX vpar_a ON vpar USING lion (a) WITH (inline_limit = 64);
	CREATE INDEX vpar_b ON vpar USING lion (b) WITH (inline_limit = 64);
	/*
	 * Both participants parked, one of them a worker: then every participant
	 * that will ever park has (two indexes, one worker planned), and waking
	 * them cannot race one that is still on its way to the point.
	 */
	CREATE FUNCTION vpar_wait_for_worker() RETURNS boolean
	LANGUAGE plpgsql AS $fn$
	DECLARE
		nparked int := 0;
		nworkers int := 0;
	BEGIN
		FOR i IN 1 .. 3000 LOOP
			PERFORM pg_stat_clear_snapshot();
			SELECT count(*),
				   count(*) FILTER (WHERE backend_type = 'parallel worker')
			  INTO nparked, nworkers
			  FROM pg_stat_activity
			 WHERE wait_event = 'lion-vacuum-page-filtered';
			EXIT WHEN nparked = 2 AND nworkers > 0;
			PERFORM pg_sleep(0.01);
		END LOOP;
		RETURN nworkers > 0;
	END $fn$;
	/*
	 * One injection_points_wakeup() wakes ONE waiter, so wake until nobody
	 * is left; detaching first means nobody new arrives.
	 */
	CREATE FUNCTION vpar_wake_all() RETURNS void
	LANGUAGE plpgsql AS $fn$
	BEGIN
		PERFORM injection_points_detach('lion-vacuum-page-filtered');
		LOOP
			BEGIN
				PERFORM injection_points_wakeup('lion-vacuum-page-filtered');
			EXCEPTION WHEN OTHERS THEN
				EXIT;
			END;
			PERFORM pg_sleep(0.01);
		END LOOP;
	END $fn$;
}
teardown
{
	DROP TABLE vpar;
	DROP FUNCTION vpar_wait_for_worker();
	DROP FUNCTION vpar_wake_all();
	DO $$ BEGIN PERFORM injection_points_detach('lion-vacuum-page-filtered');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

# The vacuuming session.
session s1
setup
{
	SET synchronous_commit = on;
	SET min_parallel_index_scan_size = 0;
	SET max_parallel_maintenance_workers = 2;
}
step s1_attach	{ SELECT injection_points_attach('lion-vacuum-page-filtered', 'wait'); }
step s1_vacuum	{ VACUUM (PARALLEL 2, INDEX_CLEANUP ON) vpar; }
# Every dead TID is gone from both indexes, and both agree with the heap.
step s1_check	{
	SELECT (SELECT sum(ntids) FROM lion_index_stats('vpar_a')) AS a_ntids,
		   (SELECT sum(ntids) FROM lion_index_stats('vpar_b')) AS b_ntids,
		   (SELECT count(*) FROM vpar) AS rows,
		   (SELECT count(*) FROM vpar WHERE a = 1) AS a1_seq,
		   lion_index_count('vpar_a', 1) AS a1_index,
		   (SELECT count(*) FROM vpar WHERE b = 2) AS b2_seq,
		   lion_index_count('vpar_b', 2) AS b2_index;
	SELECT lion_index_verify('vpar_a', true);
	SELECT lion_index_verify('vpar_b', true);
}

session s2
setup			{ SET synchronous_commit = on; }
step s2_prep	{ VACUUM (FREEZE) vpar; }
step s2_delete	{ DELETE FROM vpar WHERE id % 3 = 0; }
step s2_wake	{
	SELECT vpar_wait_for_worker() AS worker_ran_lion_bulkdelete;
	SELECT vpar_wake_all();
}

permutation
	s2_prep					# all-visible heap
	s2_delete				# dead TIDs in every chain of both indexes
	s1_attach
	s1_vacuum(s2_wake)		# every participant parks on its first filtered page
	s2_wake					# a worker is among them; let everyone go
	s1_check
