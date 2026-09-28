# Bitmap scans of a lion index under snapshots taken before and after a
# DELETE and a VACUUM: the MVCC interleavings.
#
# The counting queries carry a second aggregate on purpose: it keeps the
# plans bitmap heap scans, which is what this spec is about.
#
# s1 holds a REPEATABLE READ snapshot taken before the delete, so its bitmap
# heap scan must keep returning the old rows even after s2 has deleted them
# and s3 has vacuumed - a VACUUM that, with that snapshot open, may remove
# none of them - and a new snapshot must see the new count.  Once no old
# snapshot is left, the VACUUM does remove the TIDs from the index.
#
# Every step runs to completion before the next one starts, so no scan is
# ever in progress - holding a pin - while the VACUUM runs, and this spec does
# not exercise the pin / cleanup-lock interlock between ambulkdelete and a
# scan (DESIGN.md §9, §11).  The specs that park a scan on an injection point
# with its page pinned do: count_vacuum_race, count_prune_race,
# vacuum_entry_delete, gettuple_dirty_pin and their neighbours.

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE TABLE lion_vsc (i int4, k int4);
	INSERT INTO lion_vsc SELECT i, i % 4 FROM generate_series(1, 40000) i;
	CREATE INDEX lion_vsc_k ON lion_vsc USING lion (k)
		WITH (inline_limit = 64);
	ANALYZE lion_vsc;
}

teardown
{
	DROP TABLE lion_vsc;
}

session s1
setup		{ SET enable_seqscan = off; }
step s1_begin	{ BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ; }
step s1_count	{ SELECT count(*), max(i) FROM lion_vsc WHERE k = 1; }
step s1_commit	{ COMMIT; }

session s2
setup		{ BEGIN; }
step s2_delete	{ DELETE FROM lion_vsc WHERE k = 1 AND i % 8 = 1; }
step s2_commit	{ COMMIT; }

session s3
setup		{ SET enable_seqscan = off; }
step s3_vacuum	{ VACUUM lion_vsc; }
step s3_count	{ SELECT count(*), max(i) FROM lion_vsc WHERE k = 1; }
step s3_check	{ SELECT (SELECT count(*) + 0 * coalesce(max(i), 0) FROM lion_vsc WHERE k = 1) =
					 (SELECT count(*) FROM lion_vsc WHERE k + 0 = 1) AS matches_seqscan,
					 (SELECT count(*) + 0 * coalesce(max(i), 0) FROM lion_vsc WHERE k = 1) AS k1; }
step s3_verify	{ SELECT lion_index_verify('lion_vsc_k', true); }
step s3_stats	{ SELECT entries, ntids FROM lion_index_stats('lion_vsc_k'); }

# s1 keeps its old snapshot across the delete and the vacuum, which can
# remove none of the deleted rows while it is open; a fresh snapshot in s3
# sees the new rows.
permutation s1_begin s1_count s2_delete s2_commit s3_count s3_vacuum s1_count s1_commit s1_count s3_check s3_verify

# Once no old snapshot is left, VACUUM really removes the TIDs from the
# index, and the counts follow.
permutation s2_delete s2_commit s3_stats s3_vacuum s3_stats s3_check s3_verify
