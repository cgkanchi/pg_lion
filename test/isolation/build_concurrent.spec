# CREATE INDEX CONCURRENTLY and REINDEX CONCURRENTLY of a lion index while
# other transactions write the table.
#
# A concurrent build waits for the writers it started behind, builds from a
# snapshot, and then validates: core asks the new index for every TID it
# holds (ambulkdelete with validate_index's callback) and inserts each row
# the build's snapshot did not see, through aminsert.  So a lion index built
# this way has to report every TID it has to that callback, and take inserts
# into an index that is not yet valid.  The writers here are still open when
# the build starts (it waits for them), and they insert, update the indexed
# column, delete and roll back.  Afterwards the counts through the index must
# be the sequential scan's, and lion_index_verify() with heapallindexed must
# find every row in the index and nothing else.

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE TABLE lion_cic (i int4 PRIMARY KEY, k int4, g int4);
	INSERT INTO lion_cic SELECT i, i % 7, i % 3 FROM generate_series(1, 6000) i;
	CREATE INDEX lion_cic_g ON lion_cic USING lion (g);
}

teardown
{
	DROP TABLE lion_cic;
}

session s1
step s1_begin	{ BEGIN; }
step s1_write	{
	INSERT INTO lion_cic SELECT i, i % 11, i % 3 FROM generate_series(6001, 6500) i;
	UPDATE lion_cic SET k = 100 WHERE i BETWEEN 1 AND 200;
	DELETE FROM lion_cic WHERE i BETWEEN 201 AND 300;
}
step s1_commit	{ COMMIT; }
step s1_rollback { ROLLBACK; }

session s2
step s2_cic		{ CREATE INDEX CONCURRENTLY lion_cic_k ON lion_cic USING lion (k); }
step s2_ric		{ REINDEX INDEX CONCURRENTLY lion_cic_g; }

# An older snapshot, on no table: the build waits for it only after it has
# validated, with the index ready (written to) but not yet valid (used).
session s4
step s4_begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1 AS snap; }
step s4_commit	{ COMMIT; }

session s3
setup		{ SET enable_seqscan = off; SET enable_indexscan = off; SET pg_lion.enable_count_pushdown = off; }
step s3_write	{
	INSERT INTO lion_cic SELECT i, 100, i % 3 FROM generate_series(7001, 7100) i;
	DELETE FROM lion_cic WHERE i BETWEEN 301 AND 350;
}
step s3_check	{
	SELECT (SELECT count(*) FROM lion_cic WHERE k = 100) =
		   (SELECT count(*) FROM lion_cic WHERE k + 0 = 100) AS k100_ok,
		   (SELECT count(*) FROM lion_cic WHERE k IN (0, 3, 5)) =
		   (SELECT count(*) FROM lion_cic WHERE k + 0 IN (0, 3, 5)) AS kin_ok,
		   (SELECT count(*) FROM lion_cic WHERE g = 1) =
		   (SELECT count(*) FROM lion_cic WHERE g + 0 = 1) AS g1_ok,
		   (SELECT count(*) FROM lion_cic WHERE k = 100) AS k100;
}
step s3_valid	{
	SELECT c.relname, i.indisvalid, i.indisready
	  FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
	 WHERE i.indrelid = 'lion_cic'::regclass AND c.relname LIKE 'lion_cic_%'
	   AND c.relname <> 'lion_cic_pkey'
	 ORDER BY 1;
}
step s3_verify	{
	SELECT lion_index_verify('lion_cic_k', true) AS k, lion_index_verify('lion_cic_g', true) AS g;
}
step s3_verify_g	{ SELECT lion_index_verify('lion_cic_g', true) AS g; }

# The build waits for the open writer, which then commits; then more writes
# into the new index.
permutation s1_begin s1_write s2_cic s1_commit s3_write s3_check s3_valid s3_verify

# The same writer rolls back: its inserted rows and the new versions of the
# updated ones are dead, and neither the build nor the validation may count
# them.
permutation s1_begin s1_write s2_cic s1_rollback s3_check s3_valid s3_verify

# REINDEX CONCURRENTLY of an existing lion index under the same writes.
permutation s1_begin s1_write s2_ric s1_commit s3_write s3_check s3_valid s3_verify_g

# Writes while the new index is ready and not valid: they reach it only
# through aminsert, after the validation has read it.
permutation s4_begin s1_begin s1_write s2_cic s1_commit s3_write s3_valid s4_commit s3_check s3_valid s3_verify
permutation s4_begin s1_begin s1_write s2_ric s1_commit s3_write s3_valid s4_commit s3_check s3_valid s3_verify_g
