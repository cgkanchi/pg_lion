# VACUUM's REGROW pushing a posting set's root down while a count descends into
# it (DESIGN.md §11, §18, §22).
#
# A container can GROW while VACUUM filters it: a RUN that loses the 2nd and
# 4th member of every run of five turns into an ARRAY half again as large.
# When the page it lives on has no room for that, VACUUM re-places it through
# the ordinary placement code (lion_vacuum_regrow), and when the page is the
# one-page posting set's ROOT, placing it means a PUSH-DOWN: the root's items -
# the UNFILTERED container among them - are copied to a brand new child, the
# root becomes an internal page, and the filtered container is then written
# over the stale one on the child.
#
# The root is unlocked in between (the child's own split needs it), and the
# write to the child takes an ordinary exclusive lock, not a cleanup lock.  So
# if the child were unlocked in that window as well, a count could descend to
# it, copy the stale container with the dead TIDs, keep its pin - and have the
# write remove them under it, after which VACUUM finishes, marks the heap
# all-visible, and the count takes the dead rows straight from its copy.  The
# child therefore stays EXCLUSIVE from its allocation until the filtered
# container is on it: the count's descent waits on the child's content lock
# (reader_waits_on = a page lock) and copies the filtered container.  With
# the window open the count instead copies the stale one and parks
# (reader_waits_on = lion-count-containers-pinned), and answers 5,307 rows
# for a snapshot that sees 4,341.
#
# The fixture: k = 1 is one posting page of three containers - a RUN of ~480
# runs of five in heap blocks 0..63 and two ~2.9 KB ARRAYs in blocks 64..191 -
# with k = 2 everywhere else.  Deleting the 2nd and 4th row of every run
# (966 rows, dead to everyone before the count's snapshot) makes the RUN grow
# by ~900 bytes on filtering, which that page does not have.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE TABLE rgp (id int, k int NOT NULL);
	CREATE INDEX rgp_k ON rgp USING lion (k) WITH (inline_limit = 64);
	INSERT INTO rgp SELECT i, CASE
		WHEN i <= 14464 AND i % 30 < 5 THEN 1
		WHEN i > 14464 AND i % 10 = 0 THEN 1
		ELSE 2 END
	  FROM generate_series(1, 43392) i;

	/*
	 * Waits until the count is parked at the injection point `point`, or -
	 * when buffer_lock is true - waiting for a buffer content lock, which is
	 * reported as 'a page lock' whatever this server's name for that wait
	 * event is.
	 */
	CREATE FUNCTION rgp_reader_waits(point text, buffer_lock boolean) RETURNS text
	LANGUAGE plpgsql AS $fn$
	DECLARE ev text; evtype text;
	BEGIN
		FOR i IN 1 .. 3000 LOOP
			PERFORM pg_stat_clear_snapshot();
			SELECT wait_event, wait_event_type INTO ev, evtype
			  FROM pg_stat_activity
			 WHERE datname = current_database()
			   AND query LIKE 'SELECT lion_index_count%';
			IF ev = point THEN RETURN ev; END IF;
			IF buffer_lock AND (evtype = 'Buffer' OR ev = 'BufferContent') THEN
				RETURN 'a page lock';
			END IF;
			PERFORM pg_sleep(0.01);
		END LOOP;
		RETURN 'never';
	END $fn$;

	CREATE FUNCTION rgp_release(point text) RETURNS void
	LANGUAGE plpgsql AS $fn$
	DECLARE n int; zero int := 0;
	BEGIN
		PERFORM injection_points_detach(point);
		FOR i IN 1 .. 6000 LOOP
			PERFORM pg_stat_clear_snapshot();
			SELECT count(*) INTO n FROM pg_stat_activity WHERE wait_event = point;
			IF n = 0 THEN
				zero := zero + 1;
				EXIT WHEN zero >= 10;
			ELSE
				zero := 0;
				BEGIN
					PERFORM injection_points_wakeup(point);
				EXCEPTION WHEN OTHERS THEN NULL;
				END;
			END IF;
			PERFORM pg_sleep(0.01);
		END LOOP;
	END $fn$;
}

teardown
{
	DROP TABLE rgp;
	DROP FUNCTION rgp_reader_waits(text, boolean);
	DROP FUNCTION rgp_release(text);
	DO $$ BEGIN PERFORM injection_points_detach('lion-count-chain-entered');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
	DO $$ BEGIN PERFORM injection_points_detach('lion-count-containers-pinned');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
	DO $$ BEGIN PERFORM injection_points_detach('lion-posting-pushdown-child');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

# The count: it copies the entry, then parks holding nothing but the root's
# block number; released, it descends into whatever the VACUUM left.
session s1
setup
{
	SET pg_lion.enable_count_pushdown = off;
	SELECT injection_points_set_local();
	SELECT injection_points_attach('lion-count-chain-entered', 'wait');
	SELECT injection_points_attach('lion-count-containers-pinned', 'wait');
}
step s1_count	{ SELECT lion_index_count('rgp_k', 1); }

# The control session.
session s2
setup			{ SET synchronous_commit = on; }
step s2_delete	{ DELETE FROM rgp WHERE k = 1 AND id <= 14464 AND id % 30 IN (1, 3); }
# One step, because the count may block on a page lock in between, which
# isolationtester cannot see: release the count, wait until it has either
# copied from the child (and parked again) or is waiting for the child's lock,
# then release the VACUUM.
step s2_wake_both		{
	SELECT rgp_release('lion-count-chain-entered');
	SELECT rgp_reader_waits('lion-count-containers-pinned', true)
		AS reader_waits_on;
	SELECT rgp_release('lion-posting-pushdown-child');
}
step s2_wake_pinned		{
	SELECT rgp_reader_waits('lion-count-containers-pinned', false)
		AS reader_waits_on;
	SELECT rgp_release('lion-count-containers-pinned');
}

# The VACUUM, parked in the middle of its regrow's push-down.
session s3
setup
{
	SET synchronous_commit = on;
	SELECT injection_points_set_local();
	SELECT injection_points_attach('lion-posting-pushdown-child', 'wait');
}
step s3_vacuum	{ VACUUM (INDEX_CLEANUP ON) rgp; }

# Bookkeeping.
session s4
setup			{ SET pg_lion.enable_count_pushdown = off; SET synchronous_commit = on; }
step s4_prep	{ VACUUM (FREEZE) rgp; }
step s4_before	{
	SELECT container_pages, posting_internal_pages FROM lion_index_stats('rgp_k');
}
step s4_after	{
	SELECT container_pages, posting_internal_pages FROM lion_index_stats('rgp_k');
}
step s4_count	{
	SET enable_bitmapscan = off; SET enable_indexscan = off;
	SELECT count(*) FROM rgp WHERE k = 1;
}
step s4_verify	{ SELECT lion_index_verify('rgp_k', true); }

permutation
	s4_prep						# all-visible heap
	s2_delete					# 966 rows of the RUN container, dead to all
	s4_before					# k = 1 on one page, k = 2 on two leaves
	s1_count(s2_wake_pinned)	# copies the entry, parks before the descent
	s3_vacuum(s2_wake_both)		# regrow pushes k = 1's root down, parks with
								# the stale container on the child
	s2_wake_both				# the count descends to the child and waits
								# for its lock; the filtered container goes
								# on; VACUUM ends
	s2_wake_pinned				# the count consults the map: 4,341
	s4_after					# one more leaf and one more internal page
	s4_count
	s4_verify
