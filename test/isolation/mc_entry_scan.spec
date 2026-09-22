# An entry scan bounded to ONE key column of a multicolumn index, while
# another session fills a DIFFERENT column of the same index (DESIGN.md §24).
#
# The directory of a multicolumn index holds every column's entries in one
# tree, as runs laid end to end in attno order.  A GROUP BY over column 1 is
# therefore a leaf walk that must start at column 1's first entry and stop at
# column 2's - and it gives up its lock between two entries (§21), so column
# 2's entries may be inserted, and the leaf it is standing on split, while it
# is parked.
#
# What makes that safe is the same thing that makes §21's split safe: the walk
# resumes at "the first key above the last one I returned", and the key it
# compares against now leads with the COLUMN, so every column-2 entry sorts
# above every column-1 one whatever its value.  A split moves entries only
# RIGHT, onto a page the walk has not passed.
#
# The test:
#
#  * 30 groups in column `g` with ~90-byte keys and inline_limit = 64, plus 10
#    keys in column `o` of the same size: about five kilobytes of entries, so
#    both columns' runs share ONE leaf, which is also the root.
#  * s1 runs the column-1 group count and parks at 'lion-entry-scan-resumed',
#    which fires once per leaf, between the first entry and the second.
#  * s2 inserts 120 rows carrying 120 BRAND NEW `o` values and only EXISTING
#    `g` values, then ROLLS BACK.  The rollback is the point: the ENTRIES stay
#    (an aborted transaction does not undo index insertions), so the shared
#    leaf really splits and the root really moves, while not one row becomes
#    visible - so s1's answer stays comparable to the reference.
#  * s2 releases s1, which finishes its walk across the split.
#
# `groups = 30` is the assertion that the walk neither skipped a column-1 entry
# nor ran on into column 2, and `count` equal to the heap's row count is the
# assertion that no group lost rows.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE TABLE mces (id int, g text NOT NULL, o text NOT NULL, pad text);
	INSERT INTO mces
	SELECT i,
		   'g-' || lpad((i % 30)::text, 90, '0'),
		   'o-' || lpad((i % 10)::text, 90, '0'),
		   repeat('x', 200)
	  FROM generate_series(1, 6000) i;
	CREATE INDEX mces_go ON mces USING lion (g, o) WITH (inline_limit = 64);
	ANALYZE mces;
}

teardown
{
	DROP TABLE mces;
	/* Injection points are cluster-wide: never leave one attached, and
	 * never fail the teardown because the permutation already did. */
	DO $$ BEGIN PERFORM injection_points_detach('lion-entry-scan-resumed');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

# The counting session.  lion_index_count_group_stats() drives exactly the
# entry scan the GROUP BY pushdown drives, with the column named explicitly.
session s1
setup
{
	SET pg_lion.enable_count_pushdown = off;
	SELECT injection_points_set_local();
	SELECT injection_points_attach('lion-entry-scan-resumed', 'wait');
}
step s1_group	{
	SELECT groups, count FROM lion_index_count_group_stats('mces_go', true, 1::int2);
}
# The same walk over the OTHER column, as a second reading of the same rule.
step s1_group2	{
	SELECT groups, count FROM lion_index_count_group_stats('mces_go', true, 2::int2);
}
# The reference: the heap's own answer.
step s1_plain	{
	SELECT count(*) AS groups, sum(c) AS count
	  FROM (SELECT g, count(*) AS c FROM mces GROUP BY g) x;
}

# The session that fills the OTHER column.
session s2
setup			{ SET pg_lion.enable_count_pushdown = off; }
step s2_fill	{
	BEGIN;
	INSERT INTO mces
	SELECT 100000 + i,
		   'g-' || lpad((i % 30)::text, 90, '0'),		-- existing g keys
		   'o-new-' || lpad(i::text, 86, '0'),			-- brand new o keys
		   repeat('y', 200)
	  FROM generate_series(1, 120) i;
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
step s3_prep	{ VACUUM (FREEZE, ANALYZE) mces; }
step s3_shape	{
	SELECT leaf_pages > 1 AS the_shared_leaf_really_split,
		   directory_height > 0 AS the_root_really_moved
	  FROM lion_index_stats('mces_go') WHERE attno = 1;
	SELECT attno, entries FROM lion_index_stats('mces_go') ORDER BY attno;
}
step s3_verify	{ SELECT lion_index_verify('mces_go', true); }

# The (*) marker tells isolationtester that the step blocks on something it
# cannot see (an injection point is not a heavyweight lock), and the second
# marker pins the report of its completion after the step that releases it.
permutation
	s3_prep					# make the heap all-visible
	s1_group(*, s2_wakeup)	# parks between the first and second entry
	s2_fill					# fills column 2 and splits the shared leaf
	s2_wakeup				# detaches the point and releases s1
	s1_plain				# the heap's own answer
	s1_group2				# the same walk over the other column
	s3_shape
	s3_verify
