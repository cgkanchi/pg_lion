# Snapshot semantics of roaring_index_count() (DESIGN.md section 9).
#
# The count must always be exactly count(*) of the equivalent SELECT under
# the caller's snapshot, whatever other sessions are doing.  Every step here
# reports both numbers side by side, so the test fails if they ever diverge,
# and the absolute values show the snapshot behaviour: the REPEATABLE READ
# reader s1 keeps seeing the table as it was when it took its snapshot, while
# the READ COMMITTED reader s3 sees each committed change.
#
# The phase-2b pushdown is turned off in every session, otherwise the
# reference count(*) would be answered by the very code under test.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS roaring_index;
	CREATE TABLE cnt_snap (id int, k int NOT NULL);
	INSERT INTO cnt_snap SELECT i, i % 4 FROM generate_series(1, 4000) i;
	CREATE INDEX cnt_snap_k ON cnt_snap USING roaring (k)
		WITH (inline_limit = 64);
}

teardown
{
	DROP TABLE cnt_snap;
}

# The long-lived REPEATABLE READ reader.
session s1
setup			{ SET roaring_index.enable_count_pushdown = off; }
step s1_begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1_count	{ SELECT roaring_index_count('cnt_snap_k', 1) AS roaring,
						 (SELECT count(*) FROM cnt_snap WHERE k = 1) AS plain; }
step s1_commit	{ COMMIT; }

# The writer.
session s2
setup			{ SET roaring_index.enable_count_pushdown = off; }
step s2_begin	{ BEGIN; }
step s2_insert	{ INSERT INTO cnt_snap SELECT 10000 + i, 1
					FROM generate_series(1, 100) i; }
step s2_delete	{ DELETE FROM cnt_snap WHERE k = 1 AND id <= 400; }
step s2_commit	{ COMMIT; }

# A reader that takes a fresh snapshot for every statement.
session s3
setup			{ SET roaring_index.enable_count_pushdown = off; }
step s3_count	{ SELECT roaring_index_count('cnt_snap_k', 1) AS roaring,
						 (SELECT count(*) FROM cnt_snap WHERE k = 1) AS plain; }
step s3_vacuum	{ VACUUM cnt_snap; }

# An uncommitted insert is invisible to everyone else; once committed it is
# visible to a new snapshot but not to s1's.
permutation s1_begin s1_count s2_begin s2_insert s1_count s3_count
			s2_commit s1_count s3_count s1_commit s1_count

# The same for a delete.
permutation s1_begin s1_count s2_begin s2_delete s1_count s3_count
			s2_commit s1_count s3_count s1_commit s1_count

# And with a VACUUM in between, which removes the deleted TIDs from the index
# while s1 still has to count them.
permutation s1_begin s1_count s2_begin s2_delete s2_commit s1_count
			s3_vacuum s1_count s3_count s1_commit s1_count s3_count
