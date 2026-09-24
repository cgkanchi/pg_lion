# The SQL count functions check privileges BEFORE they lock anything (the
# 2026-09-23 review, third round).
#
# lion_index_count() and its siblings opened the table and the index - taking,
# or queueing for, an AccessShareLock on each - and only then asked whether
# the caller may read them.  A role with no privilege at all could therefore
# queue behind any lock on any table that has a lion index, and everything
# queued behind it waited too.  The cheap half of the check (SELECT on the
# table or on at least one of its columns) now comes first, so such a role is
# refused at once however the table is locked; the exact check still follows
# under the lock, where the index definition can be trusted.
#
# Before the fix, s2_count queued behind s1's lock and failed with a lock
# timeout.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE TABLE cnt_lp (k int NOT NULL);
	INSERT INTO cnt_lp SELECT g % 5 FROM generate_series(1, 100) g;
	CREATE INDEX cnt_lp_k ON cnt_lp USING lion (k);
	DROP ROLE IF EXISTS cnt_lp_none;
	CREATE ROLE cnt_lp_none;
}

teardown
{
	DROP TABLE cnt_lp;
	DROP ROLE cnt_lp_none;
}

session s1
step s1_lock	{ BEGIN; LOCK TABLE cnt_lp IN ACCESS EXCLUSIVE MODE; }
step s1_commit	{ COMMIT; }

session s2
setup			{ SET ROLE cnt_lp_none; SET lock_timeout = '10s'; }
step s2_count	{ SELECT lion_index_count('cnt_lp_k', 1); }
step s2_any		{ SELECT lion_index_count_any('cnt_lp_k', ARRAY[1, 2]); }
step s2_group	{ SELECT groups FROM lion_index_count_group_stats('cnt_lp_k'); }

permutation s1_lock s2_count s2_any s2_group s1_commit
