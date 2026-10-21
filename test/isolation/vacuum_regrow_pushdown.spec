# VACUUM's REGROW pushing a posting set's head down while a count descends into
# it (DESIGN.md §11, §18, §22).
#
# A container can GROW while VACUUM filters it: a RUN that loses every other
# member of its runs turns into a NARROW (DESIGN.md §38) several times as
# large.  When the page it lives on has no room for that, VACUUM re-places it through
# the ordinary placement code (lion_vacuum_regrow), and when the page is the
# HEAD of a posting set of 16 leaves with no internal level, placing it means
# a 17th leaf and so a PUSH-DOWN (lion_posting_promote()): the head's items -
# the UNFILTERED container among them - are copied to a brand new child, the
# head becomes the root, an internal page over all the leaves, and the
# filtered container is then written over the stale one on the child.
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
# (reader_waits_on = lion-count-containers-pinned), and answers 72,160 rows
# for a snapshot that sees 64,928.
#
# The fixture: k = 1's first leaf holds four containers - a RUN of one run of
# 226 rows a heap block in blocks 0..63, 264 bytes, and three NARROWs of 2,056
# bytes in blocks 64..255 - and 1,512 bytes to spare.  Deleting every other
# row of the RUN (7,232 rows, dead to everyone before the count's snapshot)
# makes it a NARROW too, 1,792 bytes larger, which that page does not have.
# The index is built at posting_fillfactor 10, which makes every container a
# leaf of its own, with no rows of k = 1 in blocks 64..255: its first leaf
# then takes every container key below that of its second, and the three
# NARROWs go in after the build, into the slots k = 3 leaves there.  Wide rows
# after block 255, every other one k = 1, give each key a leaf a container
# key, and k = 1 its 16th leaf: the set has no internal level until the
# regrow needs one more.  k = 2, three leaves longer, has one already.
#
# The second permutation parks the regrow before it tries the directory leaf
# of both keys' entries, and an insert into k = 2 parks in a leaf split with
# that leaf held: the regrow must wait for the leaf with nothing held and
# then go on, with the push-down and the count as in the first.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE TABLE rgp (id int, k int NOT NULL, pad text)
	  WITH (autovacuum_enabled = off);
	-- heap blocks 0..63, 226 rows each: k = 1 alone
	INSERT INTO rgp SELECT i, 1 FROM generate_series(1, 14464) i;
	-- blocks 64..255: k = 2 and k = 3 on alternate rows
	INSERT INTO rgp SELECT i, 2 + i % 2 FROM generate_series(14465, 57856) i;
	-- blocks 256..1215, 75 wide rows each: k = 1 and k = 2 on alternate rows
	INSERT INTO rgp SELECT i, 1 + i % 2, repeat('x', 70)
	  FROM generate_series(57857, 129856) i;
	CREATE INDEX rgp_k ON rgp USING lion (k)
		WITH (inline_limit = 64, posting_fillfactor = 10);
	DELETE FROM rgp WHERE k = 3;

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

	/*
	 * Waits until the VACUUM waits for a buffer content lock, reported as
	 * 'a page lock' as above; and until a backend is parked at `point`.
	 */
	CREATE FUNCTION rgp_vacuum_waits() RETURNS text
	LANGUAGE plpgsql AS $fn$
	DECLARE ev text; evtype text;
	BEGIN
		FOR i IN 1 .. 3000 LOOP
			PERFORM pg_stat_clear_snapshot();
			SELECT wait_event, wait_event_type INTO ev, evtype
			  FROM pg_stat_activity
			 WHERE datname = current_database() AND query LIKE 'VACUUM%';
			IF evtype = 'Buffer' OR ev = 'BufferContent' THEN
				RETURN 'a page lock';
			END IF;
			PERFORM pg_sleep(0.01);
		END LOOP;
		RETURN 'never';
	END $fn$;
	CREATE FUNCTION rgp_parked(point text) RETURNS boolean
	LANGUAGE plpgsql AS $fn$
	BEGIN
		FOR i IN 1 .. 3000 LOOP
			PERFORM pg_stat_clear_snapshot();
			PERFORM 1 FROM pg_stat_activity WHERE wait_event = point;
			IF FOUND THEN RETURN true; END IF;
			PERFORM pg_sleep(0.01);
		END LOOP;
		RETURN false;
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

# The slots k = 3 left in blocks 64..255, which the rows of k = 1 below take:
# its first leaf's three NARROWs.  (VACUUM runs in a setup block of its own,
# outside the transaction the first one is.)
setup { VACUUM rgp; }
setup { INSERT INTO rgp SELECT 300000 + i, 1 FROM generate_series(1, 21696) i; }

teardown
{
	DROP TABLE rgp;
	DROP FUNCTION rgp_reader_waits(text, boolean);
	DROP FUNCTION rgp_vacuum_waits();
	DROP FUNCTION rgp_parked(text);
	DROP FUNCTION rgp_release(text);
	DO $$ BEGIN PERFORM injection_points_detach('lion-count-chain-entered');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
	DO $$ BEGIN PERFORM injection_points_detach('lion-count-containers-pinned');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
	DO $$ BEGIN PERFORM injection_points_detach('lion-posting-pushdown-child');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
	DO $$ BEGIN PERFORM injection_points_detach('lion-vacuum-regrow-locked');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
	DO $$ BEGIN PERFORM injection_points_detach('lion-posting-split-incomplete');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

# The count: it copies the entry, then parks holding nothing but the head's
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
step s2_delete	{ DELETE FROM rgp WHERE k = 1 AND id <= 14464 AND id % 2 = 0; }
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

# The second permutation's VACUUM, parked in its regrow with k = 1's head
# cleanup-locked and the leaf of both keys' entries not yet tried ...
session s5
setup
{
	SET synchronous_commit = on;
	SELECT injection_points_set_local();
	SELECT injection_points_attach('lion-vacuum-regrow-locked', 'wait');
}
step s5_vacuum	{ VACUUM (INDEX_CLEANUP ON) rgp; }

# ... and an insert into k = 2's set, parked in a split with that leaf held:
# its new NARROWs, of 1,032 bytes on alternate rows with k = 4, fill k = 2's
# last leaf and split it (consecutive rows would make RUNs of a few hundred
# bytes, which the leaf would take all of).
session s6
setup
{
	SET synchronous_commit = on;
	SELECT injection_points_set_local();
	SELECT injection_points_attach('lion-posting-split-incomplete', 'wait');
}
step s6_insert	{
	INSERT INTO rgp SELECT i, CASE WHEN i % 2 = 0 THEN 2 ELSE 4 END, repeat('x', 70)
	  FROM generate_series(200001, 240000) i;
}

# One step, because the VACUUM then waits for a buffer content lock, which
# isolationtester cannot see: release the VACUUM, wait until it waits for the
# leaf, then release the insert.
session s7
step s7_parked	{ SELECT rgp_parked('lion-posting-split-incomplete') AS insert_holds_the_leaf; }
step s7_wake_both	{
	SELECT rgp_release('lion-vacuum-regrow-locked');
	SELECT rgp_vacuum_waits() AS vacuum_waits_on;
	SELECT rgp_release('lion-posting-split-incomplete');
}

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
	s2_delete					# 7,232 rows of the RUN container, dead to all
	s4_before					# k = 1 on 16 leaves, k = 2 on 18 and a root
	s1_count(s2_wake_pinned)	# copies the entry, parks before the descent
	s3_vacuum(s2_wake_both)		# regrow pushes k = 1's head down, parks with
								# the stale container on the child
	s2_wake_both				# the count descends to the child and waits
								# for its lock; the filtered container goes
								# on; VACUUM ends
	s2_wake_pinned				# the count consults the map: 64,928
	s4_after					# one more leaf and one more internal page
	s4_count
	s4_verify

# The regrow finds the leaf busy: an insert into the other key holds it.  It
# lets go of k = 1's head, waits for the leaf with nothing else held, and
# then takes the head's cleanup lock under it (lion_vacuum_regrow()), as
# lion_vacuum_container_page() does: with the leaf held no insert can be on
# the head, so the try fails only while a reader pins it.
permutation
	s4_prep
	s2_delete
	s4_before
	s5_vacuum(s7_wake_both)		# parks in its regrow, the head held
	s6_insert(s7_wake_both)		# k = 2's leaf splits, the leaf of the
								# entries held
	s7_parked
	s7_wake_both				# the leaf is busy: VACUUM waits for it;
								# the insert ends, and the regrow goes on
	s4_after
	s4_count
	s4_verify
