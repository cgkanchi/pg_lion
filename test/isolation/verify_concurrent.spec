# lion_index_verify() against concurrent writers (DESIGN.md §7).
#
# verify() compares whole levels of the directory with each other and proves
# every block reachable, which is only true of an index nobody changes while
# it is walked.  Under AccessShareLock, a loop of inserts that split the
# directory against a loop of verify() calls made it report
#
#	the directory points at block 733, but the index has only 732 blocks
#
# about an index that was fine (2026-09-25 review): the block count was taken
# before a split extended the relation.  It takes ShareLock on the table and on
# the index now, as bt_index_parent_check() does.
#
#  * The first permutation parks verify() at 'lion-verify-meta-read', right
#    after it has taken the block count and the root, and has s2 insert keys
#    that split directory leaves.  The insert WAITS for the check, the check
#    comes back clean, and the insert then really splits the directory.
#  * The second is a writer already in flight: verify() waits for its commit.
#  * The third is the deadlock the old lock order had.  A transaction that
#    holds the table and goes on to drop the index used to wait for verify()'s
#    index lock while verify() waited for its table lock.  verify() now waits
#    for the table without holding anything on the index, the DROP INDEX goes
#    through, and verify() runs once that transaction rolls back.

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE TABLE vc (k text NOT NULL);
	INSERT INTO vc SELECT 'key' || g || repeat('x', 100) FROM generate_series(1, 3000) g;
	CREATE INDEX vc_k ON vc USING lion (k) WITH (inline_limit = 64);
	CREATE TABLE vc_before AS SELECT leaf_pages FROM lion_index_stats('vc_k');
}

teardown
{
	DROP TABLE vc, vc_before;
	/* Injection points are cluster-wide: never leave one attached, and
	 * never fail the teardown because the permutation already did. */
	DO $$ BEGIN PERFORM injection_points_detach('lion-verify-meta-read');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

# The checker.
session s1
step s1_attach	{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('lion-verify-meta-read', 'wait');
}
step s1_verify	{ SELECT lion_index_verify('vc_k', true); }

# The writer.
session s2
step s2_begin	{ BEGIN; }
step s2_insert	{
	INSERT INTO vc SELECT 'new' || g || '-' || repeat('y', 120)
	  FROM generate_series(1, 600) g;
}
step s2_commit	{ COMMIT; }
step s2_lock	{ BEGIN; LOCK TABLE vc IN ACCESS EXCLUSIVE MODE; }
step s2_drop	{ DROP INDEX vc_k; }
step s2_rollback	{ ROLLBACK; }

# The observer.
session s3
step s3_wakeup	{
	SELECT injection_points_detach('lion-verify-meta-read');
	SELECT injection_points_wakeup('lion-verify-meta-read');
}
step s3_after	{
	SELECT s.leaf_pages > b.leaf_pages AS the_insert_split_the_directory,
		   (SELECT count(*) FROM vc WHERE k LIKE 'new%') AS new_rows
	  FROM lion_index_stats('vc_k') s, vc_before b;
	SELECT lion_index_verify('vc_k', true);
}

# The parked step carries no (*) marker: isolationtester sees a session
# waiting on an injection point as blocked, so it moves on only once verify()
# has really parked.  s2_insert then blocks on s1's ShareLock; the markers in
# parentheses pin the order the two completions are reported in.
permutation
	s1_attach
	s1_verify(s3_wakeup)		# parks with the block count taken
	s2_insert(s1_verify)		# waits for the check instead of splitting under it
	s3_wakeup					# the check finishes clean, then the insert runs
	s3_after

# A writer in flight: the check waits for it and then sees its rows.
permutation
	s2_begin
	s2_insert
	s1_verify(s2_commit)
	s2_commit
	s3_after

# LOCK TABLE, then DROP INDEX, in another transaction: no deadlock.
permutation
	s2_lock
	s1_verify(s2_rollback)		# waits for the table, holding nothing on the index
	s2_drop						# ... so this does not wait for it
	s2_rollback
