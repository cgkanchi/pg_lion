# A GROUP BY count parked between two entries while an inserter SPLITS the
# directory leaf it is standing on (DESIGN.md §21).
#
# The hash directory could not do this to a reader: entries were only ever
# appended to a bucket page, so an offset meant the same thing for ever.  A
# sorted directory inserts in the MIDDLE of a leaf, which shifts every offset
# after it, and splits the leaf, which moves the upper half onto a brand new
# page to its right.  Either one would make an offset-based scan skip a group
# or hand one out twice.
#
# What makes them agree is that lion_entry_scan_next() resumes at a KEY and
# not at an offset: "the first key above the last one I returned".  Splits
# move entries only RIGHT, onto a page the walk has not passed, and their keys
# are still above the last one returned, so they come out exactly once.
#
# The test:
#
#  * 30 keys with ~100-byte names and `inline_limit = 64`, which is about six
#    kilobytes of entries: one leaf, which is also the root.
#  * s1 runs the grouped count and parks at 'lion-entry-scan-resumed', which
#    fires once per leaf, between the first entry and the second.
#  * s2 inserts 60 new keys that interleave with the old ones and ROLLS BACK.
#    The rollback is the point: the ENTRIES stay (an aborted transaction does
#    not undo index insertions), so the leaf really splits and the root really
#    is replaced, while not one row becomes visible to anybody - so s1's
#    answer and the pushdown-off answer are comparable to the last row.
#  * s2 releases s1, which finishes its walk across the split.
#
# `groups` equal to `distinct_groups` is the assertion that nothing came back
# twice, and both being 30 is the assertion that nothing was skipped.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE TABLE dsp (id int, k text NOT NULL, pad text);
	INSERT INTO dsp
	SELECT i, 'key-' || lpad((2 * (i % 30))::text, 90, '0'), repeat('x', 200)
	  FROM generate_series(1, 6000) i;
	CREATE INDEX dsp_k ON dsp USING lion (k) WITH (inline_limit = 64);
	ANALYZE dsp;
}

teardown
{
	DROP TABLE dsp;
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
	  FROM (SELECT k, count(*) AS c FROM dsp GROUP BY k) g;
}
# The same answer with the pushdown off, as the reference.
step s1_plain	{
	SET pg_lion.enable_count_pushdown = off;
	SELECT count(*) AS groups, count(DISTINCT k) AS distinct_groups,
		   sum(c) AS rows_counted
	  FROM (SELECT k, count(*) AS c FROM dsp GROUP BY k) g;
	SET pg_lion.enable_count_pushdown = on;
}

# The splitting session.
session s2
setup			{ SET pg_lion.enable_count_pushdown = off; }
step s2_split	{
	BEGIN;
	INSERT INTO dsp
	SELECT 100000 + i, 'key-' || lpad((2 * i + 1)::text, 90, '0'),
		   repeat('y', 200)
	  FROM generate_series(1, 60) i;
	ROLLBACK;
}
step s2_wakeup	{
	/*
	 * Detach first, so that nothing can park at the point again, then
	 * release whoever is parked now.  How MANY times the point is reached is
	 * not fixed - the walk fires it once per leaf it consumes an entry from,
	 * and the split makes more leaves - but after this step nobody is waiting
	 * on it, which is all the permutation needs.
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

# The observer.
session s3
setup			{ SET pg_lion.enable_count_pushdown = off; }
step s3_prep	{ VACUUM (FREEZE, ANALYZE) dsp; }
step s3_shape	{
	SELECT leaf_pages > 1 AS the_leaf_really_split,
		   directory_height > 0 AS the_root_really_moved,
		   entries = 90 AS aborted_entries_are_still_there
	  FROM lion_index_stats('dsp_k');
}
step s3_verify	{ SELECT lion_index_verify('dsp_k', true); }

# The parked step carries no (*) marker: isolationtester sees a session
# waiting on an injection point as blocked (pg_isolation_test_session_is_blocked
# checks for it), so it moves on only once the step has really parked.  (*)
# would move on at once, and the next step could then race the park - a slow
# backend reached the point only after VACUUM had already run.  The marker in
# parentheses pins the report of its completion after the step that releases
# it.
permutation
	s3_prep					# make the heap all-visible
	s1_group(s2_wakeup)	# parks between the first and second entry
	s2_split				# splits the leaf the scan is standing on
	s2_wakeup				# detaches the point and releases s1
	s1_plain				# the same numbers without the pushdown
	s3_shape
	s3_verify
