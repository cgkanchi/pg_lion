# A range count through the pushdown while an inserter SPLITS the directory
# leaves it walks (DESIGN.md §28, "One read per leaf" and "The complement").
#
# A walk reads each directory leaf ONCE: it copies the entries it wants out of
# the leaf under one share lock, gives the lock up, counts the copies, and goes
# on at the right sibling the leaf had when it was read.  So between two of its
# entries, and between two of its leaves, it holds no lock at all and a
# concurrent insert is free to split the leaf it is standing on and the ones
# it has yet to read.  What makes that safe is §21's: a split moves entries
# only rightwards, the entries of the leaf the walk read are in its copy, and
# the next leaf read resumes at "the first key above the last one examined".
#
# The test:
#
#  * 2000 even keys, three rows each, INLINE and small: some twenty leaves.
#  * s1 counts a range that is best walked from the inside, and parks at
#    'lion-entry-scan-resumed', between the first and second entry of the
#    first leaf it read - holding that leaf's copies, and its pin, since the
#    copies are INLINE.
#  * s4 counts a range that is best taken as its complement, and parks at
#    'lion-entry-scan-leaf', between two leaves of the race that decides it
#    (lion_range_choose()): the walks inside and below the range are stepped a
#    leaf at a time in turn.
#  * s2 inserts 2000 odd keys - one between every two existing ones, so every
#    leaf splits - and ROLLS BACK.  The rollback is the point: the ENTRIES stay
#    (an aborted transaction does not undo index insertions), so the leaves
#    really split, while not one row becomes visible - so the counts stay
#    comparable with the reference computed afterwards.
#  * s2 releases the parked session, whose count must be exact.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE TABLE rsr (id int, k int NOT NULL, g int NOT NULL);
	INSERT INTO rsr SELECT i, 2 * (i % 2000), i % 3 FROM generate_series(1, 6000) i;
	CREATE INDEX rsr_k ON rsr USING lion (k);
	CREATE INDEX rsr_g ON rsr USING lion (g);
	ANALYZE rsr;
}

teardown
{
	DROP TABLE rsr;
	/* Injection points are cluster-wide: never leave one attached, and
	 * never fail the teardown because the permutation already did. */
	DO $$ BEGIN PERFORM injection_points_detach('lion-entry-scan-resumed');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
	DO $$ BEGIN PERFORM injection_points_detach('lion-entry-scan-leaf');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

# The session that walks the inside of a range, parked between two entries.
session s1
setup
{
	SET pg_lion.enable_count_pushdown = on;
	SET enable_seqscan = off;
	SET enable_bitmapscan = off;
	SET enable_indexscan = off;
	SELECT injection_points_set_local();
	SELECT injection_points_attach('lion-entry-scan-resumed', 'wait');
}
step s1_inside	{ SELECT count(*) FROM rsr WHERE k BETWEEN 1000 AND 1800 AND g = 1; }

# The session that takes the complement, parked between two leaves.
session s4
setup
{
	SET pg_lion.enable_count_pushdown = on;
	SET enable_seqscan = off;
	SET enable_bitmapscan = off;
	SET enable_indexscan = off;
	SELECT injection_points_set_local();
	SELECT injection_points_attach('lion-entry-scan-leaf', 'wait');
}
step s4_complement	{ SELECT count(*) FROM rsr WHERE k >= 800 AND g = 1; }

# The splitting session.
session s2
setup			{ SET pg_lion.enable_count_pushdown = off; }
step s2_split	{
	BEGIN;
	INSERT INTO rsr SELECT 100000 + i, 2 * i + 1, 1 FROM generate_series(0, 1999) i;
	ROLLBACK;
}
step s2_wakeup_resumed	{
	/*
	 * Detach first, so that nothing can park at the point again, then
	 * release whoever is parked now: the walk reaches it once per leaf.
	 */
	SELECT injection_points_detach('lion-entry-scan-resumed');
	DO $$
	DECLARE n int; zero int := 0;
	BEGIN
		FOR i IN 1 .. 6000 LOOP
			SELECT count(*) INTO n FROM pg_stat_activity
			 WHERE wait_event = 'lion-entry-scan-resumed';
			IF n = 0 THEN
				zero := zero + 1;
				EXIT WHEN zero >= 10;
			ELSE
				zero := 0;
				BEGIN
					PERFORM injection_points_wakeup('lion-entry-scan-resumed');
				EXCEPTION WHEN OTHERS THEN NULL;
				END;
			END IF;
			PERFORM pg_sleep(0.01);
		END LOOP;
	END $$;
}
step s2_wakeup_leaf	{
	SELECT injection_points_detach('lion-entry-scan-leaf');
	DO $$
	DECLARE n int; zero int := 0;
	BEGIN
		FOR i IN 1 .. 6000 LOOP
			SELECT count(*) INTO n FROM pg_stat_activity
			 WHERE wait_event = 'lion-entry-scan-leaf';
			IF n = 0 THEN
				zero := zero + 1;
				EXIT WHEN zero >= 10;
			ELSE
				zero := 0;
				BEGIN
					PERFORM injection_points_wakeup('lion-entry-scan-leaf');
				EXCEPTION WHEN OTHERS THEN NULL;
				END;
			END IF;
			PERFORM pg_sleep(0.01);
		END LOOP;
	END $$;
}

# The observer: the way each count is evaluated, the references, and proof
# that the leaves really split.
session s3
setup
{
	SET enable_seqscan = off;
	SET enable_bitmapscan = off;
	SET enable_indexscan = off;
}
step s3_prep	{ VACUUM (FREEZE, ANALYZE) rsr; }
step s3_how		{
	CREATE FUNCTION rsr_how(q text) RETURNS text LANGUAGE plpgsql AS $fn$
	DECLARE j jsonb;
	BEGIN
		EXECUTE 'EXPLAIN (ANALYZE, TIMING OFF, COSTS OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) ' || q INTO j;
		RETURN jsonb_path_query_first(j, 'strict $.** ? (@."Custom Plan Provider" == "LionCount")') ->> 'Range Evaluation';
	END $fn$;
	SELECT rsr_how('SELECT count(*) FROM rsr WHERE k BETWEEN 1000 AND 1800 AND g = 1') AS inside,
		   rsr_how('SELECT count(*) FROM rsr WHERE k >= 800 AND g = 1') AS complement,
		   leaf_pages < 30 AS one_leaf_per_hundred_keys
	  FROM lion_index_stats('rsr_k');
	DROP FUNCTION rsr_how(text);
}
step s3_plain	{
	SET pg_lion.enable_count_pushdown = off;
	RESET enable_seqscan;
	SELECT count(*) AS inside FROM rsr WHERE k BETWEEN 1000 AND 1800 AND g = 1;
	SELECT count(*) AS complement FROM rsr WHERE k >= 800 AND g = 1;
	SET pg_lion.enable_count_pushdown = on;
	SET enable_seqscan = off;
}
step s3_shape	{
	SELECT leaf_pages > 30 AS the_leaves_really_split,
		   entries = 4000 AS aborted_entries_are_still_there
	  FROM lion_index_stats('rsr_k');
}
step s3_verify	{ SELECT lion_index_verify('rsr_k', true); }

# The parked steps carry no (*) marker: isolationtester sees a session waiting
# on an injection point as blocked, so it moves on only once the step has
# really parked.  The marker in parentheses pins the report of its completion
# after the step that releases it.
permutation
	s3_prep					# make the heap all-visible
	s3_how					# inside and complement, some twenty leaves
	s1_inside(s2_wakeup_resumed)	# parks between two entries of a leaf
	s2_split				# splits every leaf, the one it stands on too
	s2_wakeup_resumed		# detaches the point and releases s1
	s3_plain				# the same numbers without the pushdown
	s3_shape
	s3_verify

permutation
	s3_prep
	s3_how
	s4_complement(s2_wakeup_leaf)	# parks between two leaves of the race
	s2_split
	s2_wakeup_leaf
	s3_plain
	s3_shape
	s3_verify
