# Concurrent inserts into one hash bucket of a roaring index.
#
# The counting queries carry a second aggregate on purpose: it keeps the
# plans bitmap heap scans, which is what this spec is about.
#
# The index has a single bucket, so every key shares one bucket head page and
# all writers serialise on it.  Two sessions insert rows with the same key
# while a third scans; the scan must see exactly the committed rows, and the
# index must still be structurally sound afterwards.

setup
{
	CREATE EXTENSION IF NOT EXISTS roaring_index;
	CREATE TABLE rbi_conc (i int4, k int4);
	INSERT INTO rbi_conc SELECT i, i % 4 FROM generate_series(1, 20000) i;
	CREATE INDEX rbi_conc_k ON rbi_conc USING roaring (k)
		WITH (buckets = 1, inline_limit = 64);
	ANALYZE rbi_conc;
}

teardown
{
	DROP TABLE rbi_conc;
}

session s1
setup		{ BEGIN; }
step s1_ins	{ INSERT INTO rbi_conc SELECT i, 1 FROM generate_series(20001, 20500) i; }
step s1_ins2	{ INSERT INTO rbi_conc SELECT i, 5 FROM generate_series(21001, 21100) i; }
step s1_commit	{ COMMIT; }
step s1_rollback { ROLLBACK; }

session s2
setup		{ BEGIN; }
step s2_ins	{ INSERT INTO rbi_conc SELECT i, 1 FROM generate_series(30001, 30500) i; }
step s2_ins2	{ INSERT INTO rbi_conc SELECT i, 5 FROM generate_series(31001, 31100) i; }
step s2_commit	{ COMMIT; }

session s3
setup		{ SET enable_seqscan = off; }
step s3_count	{ SELECT count(*), max(i) FROM rbi_conc WHERE k = 1; }
step s3_check	{ SELECT (SELECT count(*) + 0 * coalesce(max(i), 0) FROM rbi_conc WHERE k = 1) =
					 (SELECT count(*) FROM rbi_conc WHERE k + 0 = 1) AS k1_matches_seqscan,
					 (SELECT count(*) + 0 * coalesce(max(i), 0) FROM rbi_conc WHERE k = 5) =
					 (SELECT count(*) FROM rbi_conc WHERE k + 0 = 5) AS k5_matches_seqscan,
					 (SELECT count(*) + 0 * coalesce(max(i), 0) FROM rbi_conc WHERE k = 1) AS k1,
					 (SELECT count(*) + 0 * coalesce(max(i), 0) FROM rbi_conc WHERE k = 5) AS k5; }
step s3_verify	{ SELECT roaring_index_verify('rbi_conc_k', true); }
step s3_stats	{ SELECT entries, ntids FROM roaring_index_stats('rbi_conc_k'); }

# Both sessions insert into the same key, then into a key that does not exist
# yet (so both try to create the same entry), with a scan in between.
permutation s1_ins s2_ins s3_count s1_ins2 s2_ins2 s3_count s1_commit s2_commit s3_check s3_stats s3_verify

# The same, but one of the two transactions rolls back: its TIDs stay in the
# index (no index AM undoes an insert) and the scan must filter them out.
permutation s1_ins s1_ins2 s2_ins s2_ins2 s1_rollback s2_commit s3_check s3_verify
