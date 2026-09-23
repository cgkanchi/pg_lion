# A count parked on a posting-tree LEAF while an inserter SPLITS that leaf
# (DESIGN.md §22, and the reader half of the §4/§5 split rule).
#
# The counting code copies a leaf - its items AND its right link, together,
# under one SHARE lock - keeps the PIN, and only then asks the visibility map
# about the heap blocks those containers cover (DESIGN.md §9).  An INSERT needs
# only an exclusive CONTENT lock on that leaf, not a cleanup lock, so it is
# free to run in that window and to SPLIT the page: items at and after the
# insert position move to a brand new page linked immediately to the right, and
# the left page's right link is repointed at it.
#
# That is exactly the interleaving the "items only ever move right, onto a page
# the reader has not passed" rule exists for, and the reader's answer must not
# change:
#
#  * the containers that moved are in the copy the reader already holds, so it
#    counts them once;
#  * its copy's right link is the OLD one, which now names the page AFTER the
#    new sibling - so it steps over the new page, which holds nothing but the
#    items it has already seen plus the inserter's own new TIDs;
#  * and those new TIDs belong to a transaction that commits after the reader's
#    snapshot was taken, so they are not its to count anyway.
#
# The fixture makes the split land on the very page the reader is parked on:
# the count stops at the FIRST container of the set, which is on the leftmost
# leaf of that key's posting tree.  The DELETE takes the OTHER key's rows out
# of the front of the heap, which leaves free line pointers on those pages
# without touching k = 1's containers at all; the INSERT then puts k = 1 rows
# into them, so exactly the container keys that leaf owns grow - here from 770
# members to about 1540 - until its five arrays no longer fit and it splits.
#
# k = 1 has 50,000 rows and loses none of them, so s1 must answer 50,000
# however the pages move under it; `container_pages` before and after shows
# that they really did move (26 leaves become 32), and the unpushed count
# afterwards is 60,000, which is those 50,000 plus the inserter's 10,000.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	/* The padding is what makes a container key hold few enough rows for a
	 * leaf to carry SEVERAL containers: five arrays of ~1550 bytes, so that
	 * growing them is what splits the page rather than one bitset per leaf. */
	CREATE TABLE psr (id int, k int NOT NULL, pad char(300));
	INSERT INTO psr SELECT i, i % 2, '' FROM generate_series(1, 100000) i;
	CREATE INDEX psr_k ON psr USING lion (k);
}

teardown
{
	DROP TABLE psr;
	/* Injection points are cluster-wide: never leave one attached, and
	 * never fail the teardown because the permutation already did. */
	DO $$ BEGIN PERFORM injection_points_detach('lion-count-containers-pinned');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

# The counting session.  The pushdown is what pins the leaf.
session s1
setup
{
	SET pg_lion.enable_count_pushdown = on;
	SET enable_seqscan = off;
	SELECT injection_points_set_local();
	SELECT injection_points_attach('lion-count-containers-pinned', 'wait');
}
step s1_count	{ SELECT count(*) FROM psr WHERE k = 1; }
step s1_plain	{
	SET pg_lion.enable_count_pushdown = off;
	SELECT count(*) FROM psr WHERE k = 1;
	SET pg_lion.enable_count_pushdown = on;
}

# The writer.
session s2
setup			{ SET pg_lion.enable_count_pushdown = off; }
step s2_delete	{ DELETE FROM psr WHERE k = 0 AND id <= 20000; }
step s2_insert	{
	INSERT INTO psr SELECT 1000000 + i, 1, '' FROM generate_series(1, 10000) i;
}
step s2_wakeup	{
	/*
	 * Detach first, so that nothing can park at the point again, then
	 * release whoever is parked now.  injection_points_wakeup() searches the
	 * WAITERS, so it still works after the detach.
	 */
	SELECT injection_points_detach('lion-count-containers-pinned');
	DO $$
	DECLARE n int; zero int := 0;
	BEGIN
		FOR i IN 1 .. 6000 LOOP
			SELECT count(*) INTO n FROM pg_stat_activity
			 WHERE wait_event = 'lion-count-containers-pinned';
			IF n = 0 THEN
				zero := zero + 1;
				EXIT WHEN zero >= 10;
			ELSE
				zero := 0;
				BEGIN
					PERFORM injection_points_wakeup('lion-count-containers-pinned');
				EXCEPTION WHEN OTHERS THEN NULL;
				END;
			END IF;
			PERFORM pg_sleep(0.01);
		END LOOP;
	END $$;
}

# The bookkeeping session.
session s3
setup			{ SET pg_lion.enable_count_pushdown = off; }
step s3_prep	{ VACUUM (FREEZE, ANALYZE) psr; }
step s3_vacuum	{ VACUUM psr; }
step s3_leaves	{
	SELECT container_pages > 1 AS several_leaves, max_posting_height AS height
	  FROM lion_index_stats('psr_k');
}
step s3_split	{
	SELECT container_pages > (SELECT pages FROM psr_leaves) AS leaf_was_split
	  FROM lion_index_stats('psr_k');
}
step s3_note	{
	DROP TABLE IF EXISTS psr_leaves;
	CREATE TABLE psr_leaves AS
		SELECT container_pages AS pages FROM lion_index_stats('psr_k');
}
step s3_verify	{ SELECT lion_index_verify('psr_k', true); }

# The parked step carries no (*) marker: isolationtester sees a session
# waiting on an injection point as blocked (pg_isolation_test_session_is_blocked
# checks for it), so it moves on only once the step has really parked.  (*)
# would move on at once, and the next step could then race the park - a slow
# backend reached the point only after VACUUM had already run.  The marker in
# parentheses pins the report of its completion after the step that releases
# it.
permutation
	s3_prep					# make the heap all-visible
	s2_delete				# k = 0's rows at the front of the heap
	s3_vacuum				# ... so those pages have room again
	s3_leaves
	s3_note					# how many leaves the set has before the split
	s1_count(s2_wakeup)	# parks holding a pin on the leftmost leaf
	s2_insert				# grows that leaf's containers until it splits
	s2_wakeup				# releases s1, which finishes its walk
	s1_plain				# the truth now: s1's 50,000 plus the new 10,000
	s3_split
	s3_verify
