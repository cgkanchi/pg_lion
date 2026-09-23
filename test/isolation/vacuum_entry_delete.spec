# A GROUP BY count running while VACUUM deletes entries (DESIGN.md §18).
#
# VACUUM removes an entry whose posting set has become empty, and the GROUP BY
# driver walks the entries of a directory leaf giving up its lock on the page
# between two of them.  So the two have to agree on where the walk resumes, or
# a group would be skipped (every entry after a deleted one shifts down) or
# returned twice.
#
# What makes them agree since DESIGN.md §21 is that the scan resumes at a KEY
# and not at an offset: "the first key above the last one I returned".  A
# deleted entry is simply gone, and only an EMPTY posting set is ever deleted,
# so its group had nothing this scan's snapshot could have counted.
#
# The test:
#
#  * 40 keys, small enough that every entry is on ONE directory leaf and a
#    deletion really would shift the ones after it; the rows carry a 400-byte
#    padding column so that each key's TIDs spread over enough heap blocks to
#    need several container keys, and `inline_limit = 64` so that every entry
#    is a CHAIN entry - a posting set the scan holds nothing of but a head
#    block, which is what lets VACUUM run at all while the scan is parked (an
#    INLINE payload keeps its leaf pinned, and VACUUM would simply wait for
#    that pin).
#  * s2 deletes every even key's rows and commits BEFORE s1 takes its
#    snapshot, so they are dead to everyone: s1 must not see them, and VACUUM
#    is free to remove them.
#  * s1 runs the grouped count and parks at 'lion-entry-scan-resumed', which
#    fires once per leaf, between the first entry and the second.
#  * s3 vacuums: 20 entries lose their last TID and are deleted, and their
#    chains are freed.
#  * s2 releases s1, which finishes its walk of the same leaf.
#
# The expected output is the 20 odd keys, each exactly once, with 300 rows
# each - and `groups` and `distinct_groups` being equal is the assertion that
# nothing came back twice.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE TABLE vgd (id int, k int NOT NULL, g int NOT NULL, pad text);
	INSERT INTO vgd SELECT i, 1 + (i % 40), i % 3, repeat('x', 400)
	  FROM generate_series(1, 12000) i;
	CREATE INDEX vgd_g ON vgd USING lion (g);
	CREATE INDEX vgd_k ON vgd USING lion (k) WITH (inline_limit = 64);
}

teardown
{
	DROP TABLE vgd;
	/* Injection points are cluster-wide: never leave one attached, and
	 * never fail the teardown because the permutation already did. */
	DO $$ BEGIN PERFORM injection_points_detach('lion-entry-scan-resumed');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

# The counting session.  The pushdown is what walks the entries.
session s1
setup
{
	SET pg_lion.enable_count_pushdown = on;
	SET enable_seqscan = off;
	SELECT injection_points_set_local();
	SELECT injection_points_attach('lion-entry-scan-resumed', 'wait');
}
step s1_group	{
	SELECT count(*) AS groups, count(DISTINCT k) AS distinct_groups,
		   sum(c) AS rows_counted, min(c) AS min_rows, max(c) AS max_rows
	  FROM (SELECT k, count(*) AS c FROM vgd GROUP BY k) g;
}
# The two-column GROUP BY driver walks the same entries, one outer entry scan
# with the inner sets re-located per pair, so it meets the same deletions.
step s1_group2	{
	SELECT count(*) AS groups, count(DISTINCT (k, g)) AS distinct_groups,
		   sum(c) AS rows_counted
	  FROM (SELECT k, g, count(*) AS c FROM vgd GROUP BY k, g) x;
}
# The same answers with the pushdown off, as the reference.
step s1_plain	{
	SET pg_lion.enable_count_pushdown = off;
	SELECT count(*) AS groups, count(DISTINCT k) AS distinct_groups,
		   sum(c) AS rows_counted
	  FROM (SELECT k, count(*) AS c FROM vgd GROUP BY k) g;
	SELECT count(*) AS groups2, count(DISTINCT (k, g)) AS distinct_groups2,
		   sum(c) AS rows_counted2
	  FROM (SELECT k, g, count(*) AS c FROM vgd GROUP BY k, g) x;
	SET pg_lion.enable_count_pushdown = on;
}

# The writer.
session s2
setup			{ SET pg_lion.enable_count_pushdown = off; }
step s2_delete	{ DELETE FROM vgd WHERE k % 2 = 0; }
step s2_wakeup	{
	/*
	 * Detach first, so that nothing can park at the point again, then
	 * release whoever is parked now.  How MANY times the point is reached is
	 * not fixed - the two-column driver runs more than one entry scan, and a
	 * scan fires the point once per leaf it consumes an entry from -
	 * but after this step nobody is waiting on it, which is all the
	 * permutation needs.  injection_points_wakeup() searches the WAITERS, so
	 * it still works after the detach.
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
				/*
				 * The waiter clears its slot itself once it notices the
				 * counter, so it can be gone between the poll above and
				 * this call; that is a success, not a failure.
				 */
				BEGIN
					PERFORM injection_points_wakeup('lion-entry-scan-resumed');
				EXCEPTION WHEN OTHERS THEN NULL;
				END;
			END IF;
			PERFORM pg_sleep(0.01);
		END LOOP;
	END $$;
}

# The vacuuming session.
session s3
setup			{ SET pg_lion.enable_count_pushdown = off; }
step s3_prep	{ VACUUM (FREEZE, ANALYZE) vgd; }
step s3_vacuum	{ VACUUM vgd; }
# While s1 is parked it pins a heap page (its recheck path), and a dead row of
# an even key that happens to sit on that page cannot be pruned by the first
# VACUUM (no cleanup lock), so that key's entry survives with one TID until
# the next VACUUM.  That is heap VACUUM behaving as designed, not the index:
# the interlock assertion is s1's output above.  So the first VACUUM must have
# freed AT LEAST one chain and removed the bulk of the dead TIDs; a second
# VACUUM after s1 has finished must reach exactly 20 entries / 6000 TIDs.
step s3_stats	{
	SELECT entries BETWEEN 20 AND 40 AS entries_in_range,
		   ntids BETWEEN 6000 AND 6020 AS ntids_in_range,
		   deleted_pages > 0 AS chains_were_freed
	  FROM lion_index_stats('vgd_k');
}
step s3_vacuum2	{ VACUUM vgd; }
step s3_stats2	{ SELECT entries, ntids FROM lion_index_stats('vgd_k'); }
step s3_verify	{ SELECT lion_index_verify('vgd_k', true);
				  SELECT lion_index_verify('vgd_g', true); }

# The (*) marker tells isolationtester that the step blocks on something it
# cannot see (an injection point is not a heavyweight lock), and the second
# marker pins the report of its completion after the step that releases it.
permutation
	s3_prep					# make the heap all-visible
	s2_delete				# 6000 rows, dead to everyone from here on
	s1_group(*, s2_wakeup)	# parks between the first and second entry
	s3_vacuum				# deletes 20 entries and frees their chains
	s2_wakeup				# detaches the point and releases s1
	s1_plain				# the same numbers without the pushdown
	s3_stats
	s3_vacuum2
	s3_stats2
	s3_verify

# The same race against the two-column GROUP BY driver, which runs an outer
# entry scan and re-locates the inner sets for every pair.
permutation
	s3_prep
	s2_delete
	s1_group2(*, s2_wakeup)
	s3_vacuum
	s2_wakeup
	s1_plain
	s3_stats
	s3_vacuum2
	s3_stats2
	s3_verify
