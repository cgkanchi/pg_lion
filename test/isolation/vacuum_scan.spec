# A bitmap scan of a roaring index against a concurrent DELETE and VACUUM.
#
# The counting queries carry a second aggregate on purpose: it keeps the
# plans bitmap heap scans, which is what this spec is about.
#
# s1 holds a REPEATABLE READ snapshot taken before the delete, so its bitmap
# heap scan must keep returning the old rows even after s2 has deleted them
# and s3 has vacuumed; a new snapshot must see the new count.  ambulkdelete
# takes cleanup locks on the pages it changes, so it also has to wait for the
# pins a concurrent scan holds.

setup
{
	CREATE EXTENSION IF NOT EXISTS roaring_index;
	CREATE TABLE rbi_vsc (i int4, k int4);
	INSERT INTO rbi_vsc SELECT i, i % 4 FROM generate_series(1, 40000) i;
	CREATE INDEX rbi_vsc_k ON rbi_vsc USING roaring (k)
		WITH (inline_limit = 64);
	ANALYZE rbi_vsc;
}

teardown
{
	DROP TABLE rbi_vsc;
}

session s1
setup		{ SET enable_seqscan = off; }
step s1_begin	{ BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ; }
step s1_count	{ SELECT count(*), max(i) FROM rbi_vsc WHERE k = 1; }
step s1_commit	{ COMMIT; }

session s2
setup		{ BEGIN; }
step s2_delete	{ DELETE FROM rbi_vsc WHERE k = 1 AND i % 8 = 1; }
step s2_commit	{ COMMIT; }

session s3
setup		{ SET enable_seqscan = off; }
step s3_vacuum	{ VACUUM rbi_vsc; }
step s3_count	{ SELECT count(*), max(i) FROM rbi_vsc WHERE k = 1; }
step s3_check	{ SELECT (SELECT count(*) + 0 * coalesce(max(i), 0) FROM rbi_vsc WHERE k = 1) =
					 (SELECT count(*) FROM rbi_vsc WHERE k + 0 = 1) AS matches_seqscan,
					 (SELECT count(*) + 0 * coalesce(max(i), 0) FROM rbi_vsc WHERE k = 1) AS k1; }
step s3_verify	{ SELECT roaring_index_verify('rbi_vsc_k', true); }
step s3_stats	{ SELECT entries, ntids FROM roaring_index_stats('rbi_vsc_k'); }

# s1 keeps its old snapshot across the delete and the vacuum; a fresh
# snapshot in s3 sees the new rows.
permutation s1_begin s1_count s2_delete s2_commit s3_count s3_vacuum s1_count s1_commit s1_count s3_check s3_verify

# Once no old snapshot is left, VACUUM really removes the TIDs from the
# index, and the counts follow.
permutation s2_delete s2_commit s3_stats s3_vacuum s3_stats s3_check s3_verify
