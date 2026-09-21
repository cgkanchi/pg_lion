# Snapshot eligibility of the SQL count functions (DESIGN.md §9; the
# 2026-09-20 review, finding 2).
#
# An index whose build found a broken HOT chain carries indcheckxmin: it holds
# only the LATEST version of that chain, so a transaction whose snapshot can
# still see an older version must not use it.  The planner enforces that in
# get_relation_info(); lion_index_count() opens an index it was handed by
# name, so it has to make the same decision itself - and heap rechecking
# cannot repair the damage, because the TID the old snapshot needs is not in
# the posting set at all.
#
# The recipe builds exactly that index.  There is no index on the table when
# s1 updates the row, so the update is HOT; s2 already holds a REPEATABLE READ
# snapshot, which holds back OldestXmin, so the CREATE INDEX that follows finds
# the chain root (k = 1) recently dead and HOT-updated - a broken HOT chain -
# indexes only k = 2, and sets indcheckxmin.  s1_check prints the catalog flags
# so that a future PostgreSQL that stops setting indcheckxmin here fails this
# test loudly instead of silently testing nothing.
#
# Before the fix, s2's ordinary count(*) WHERE k = 1 returned 1 while
# lion_index_count() returned 0.  Now it refuses.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE TABLE cnt_cx (k int);
	INSERT INTO cnt_cx VALUES (1);
}

teardown
{
	DROP TABLE cnt_cx;
}

# The writer: HOT-update first, build the index second.
session s1
step s1_update	{ UPDATE cnt_cx SET k = 2; }
step s1_index	{ CREATE INDEX cnt_cx_k ON cnt_cx USING lion (k); }
step s1_check	{ SELECT indisvalid, indisready, indcheckxmin
					FROM pg_index WHERE indexrelid = 'cnt_cx_k'::regclass; }

# The old reader, whose snapshot predates the index.
session s2
setup			{ SET pg_lion.enable_count_pushdown = off; }
step s2_begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s2_snap	{ SELECT count(*) AS visible_rows FROM cnt_cx; }
# each refusal aborts the transaction, so every one of them gets a savepoint
step s2_sp		{ SAVEPOINT p; }
step s2_back	{ ROLLBACK TO p; }
step s2_plain	{ SELECT count(*) AS plain FROM cnt_cx WHERE k = 1; }
step s2_count	{ SELECT lion_index_count('cnt_cx_k', 1); }
step s2_stats	{ SELECT count FROM lion_index_count_stats('cnt_cx_k', 1); }
step s2_count2	{ SELECT lion_index_count('cnt_cx_k', 1, 'cnt_cx_k', 1); }
step s2_verify	{ SELECT lion_index_verify('cnt_cx_k', true); }
step s2_commit	{ COMMIT; }

# A reader that takes its snapshot after the build: for it the index is fine.
session s3
setup			{ SET pg_lion.enable_count_pushdown = off; }
step s3_count	{ SELECT lion_index_count('cnt_cx_k', 2) AS roaring,
						 (SELECT count(*) FROM cnt_cx WHERE k = 2) AS plain; }
step s3_count1	{ SELECT lion_index_count('cnt_cx_k', 1) AS roaring,
						 (SELECT count(*) FROM cnt_cx WHERE k = 1) AS plain; }

permutation s2_begin s2_snap s1_update s1_index s1_check s2_plain
			s2_sp s2_count s2_back s2_stats s2_back s2_count2 s2_back
			s2_verify s2_back s2_commit
			s2_count s3_count s3_count1
