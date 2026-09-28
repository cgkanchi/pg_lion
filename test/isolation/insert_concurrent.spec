# Inserts from two open transactions into one directory leaf of a lion index,
# and scans between them: the MVCC interleavings.
#
# The counting queries carry a second aggregate on purpose: it keeps the
# plans bitmap heap scans, which is what this spec is about.
#
# Every key here shares one directory leaf.  Two transactions insert rows
# with the same key, and then with a key that did not exist, while a third
# session scans; the index holds both transactions' TIDs from the moment
# each INSERT ends, committed or not, and the scan must see exactly the
# committed rows - none of an open transaction's, none of a rolled-back
# one's - and the index must still be structurally sound afterwards.
#
# Every step runs to completion before the next one starts, so the two
# INSERTs never hold the leaf at the same time and neither ever waits for the
# other's page lock: the second session to insert a key finds the entry the
# first one made.  This spec does not exercise two writers racing on a page
# or on the creation of one entry; dir_insert_race does that, with an
# injection point.

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE TABLE lion_conc (i int4, k int4);
	INSERT INTO lion_conc SELECT i, i % 4 FROM generate_series(1, 20000) i;
	CREATE INDEX lion_conc_k ON lion_conc USING lion (k)
		WITH (inline_limit = 64);
	ANALYZE lion_conc;
}

teardown
{
	DROP TABLE lion_conc;
}

session s1
setup		{ BEGIN; }
step s1_ins	{ INSERT INTO lion_conc SELECT i, 1 FROM generate_series(20001, 20500) i; }
step s1_ins2	{ INSERT INTO lion_conc SELECT i, 5 FROM generate_series(21001, 21100) i; }
step s1_commit	{ COMMIT; }
step s1_rollback { ROLLBACK; }

session s2
setup		{ BEGIN; }
step s2_ins	{ INSERT INTO lion_conc SELECT i, 1 FROM generate_series(30001, 30500) i; }
step s2_ins2	{ INSERT INTO lion_conc SELECT i, 5 FROM generate_series(31001, 31100) i; }
step s2_commit	{ COMMIT; }

session s3
setup		{ SET enable_seqscan = off; }
step s3_count	{ SELECT count(*), max(i) FROM lion_conc WHERE k = 1; }
step s3_check	{ SELECT (SELECT count(*) + 0 * coalesce(max(i), 0) FROM lion_conc WHERE k = 1) =
					 (SELECT count(*) FROM lion_conc WHERE k + 0 = 1) AS k1_matches_seqscan,
					 (SELECT count(*) + 0 * coalesce(max(i), 0) FROM lion_conc WHERE k = 5) =
					 (SELECT count(*) FROM lion_conc WHERE k + 0 = 5) AS k5_matches_seqscan,
					 (SELECT count(*) + 0 * coalesce(max(i), 0) FROM lion_conc WHERE k = 1) AS k1,
					 (SELECT count(*) + 0 * coalesce(max(i), 0) FROM lion_conc WHERE k = 5) AS k5; }
step s3_verify	{ SELECT lion_index_verify('lion_conc_k', true); }
step s3_stats	{ SELECT entries, ntids FROM lion_index_stats('lion_conc_k'); }

# Both sessions insert into the same key, then into a key that does not exist
# yet (the first INSERT creates its entry, the second finds it), with a scan
# in between.
permutation s1_ins s2_ins s3_count s1_ins2 s2_ins2 s3_count s1_commit s2_commit s3_check s3_stats s3_verify

# The same, but one of the two transactions rolls back: its TIDs stay in the
# index (no index AM undoes an insert) and the scan must filter them out.
permutation s1_ins s1_ins2 s2_ins s2_ins2 s1_rollback s2_commit s3_check s3_verify
