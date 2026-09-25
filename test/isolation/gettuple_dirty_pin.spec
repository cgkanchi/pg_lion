# The pin a plain index scan keeps - or does not keep - between two amgettuple
# calls (DESIGN.md §29.5).
#
# Under an MVCC snapshot the scan drops every index pin as soon as it has
# copied what it needs, as nbtree does (so->dropPin): the heap fetch applies
# the snapshot, so a TID that VACUUM removes and the heap recycles meanwhile
# cannot come back as a visible row.  Under any other snapshot that argument
# is gone - a dirty snapshot sees the tuple a recycled slot holds now - so
# the scan keeps the page each batch came from pinned until the next batch,
# and VACUUM, which takes a cleanup lock on every page of the index (§11),
# cannot remove any TID of that batch, and the heap cannot recycle its slot,
# until the scan has moved on.
#
# The non-MVCC scan here is the one core runs for an exclusion constraint:
# check_exclusion_or_unique_constraint() scans with SnapshotDirty, and
# `EXCLUDE USING lion (k WITH =)` is possible since lion has amgettuple.  The
# injection point "lion-gettuple-batch" fires after a batch is loaded and
# before its first TID is returned.
#
#  * pinned: the INSERT's constraint check parks there with its own new
#    entry's directory leaf pinned; a VACUUM that has dead TIDs to remove
#    must wait for that pin (dpin_wait_for_vacuum() sees it parked on
#    BufferCleanup, or BufferPin before 19) and finishes once the check has
#    moved on.
#  * unpinned: a plain SELECT parks at the same point under its MVCC
#    snapshot, and the same VACUUM runs to completion while it is parked -
#    the step carries no (*) marker, so a VACUUM that waited would hang the
#    permutation - and the SELECT's answer is still exact.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE TABLE dpin (k int, pad text, EXCLUDE USING lion (k WITH =))
		WITH (autovacuum_enabled = off);
	INSERT INTO dpin SELECT i, repeat('d', 20) FROM generate_series(1, 2000) i;

	CREATE FUNCTION dpin_wait_for_vacuum() RETURNS boolean
	LANGUAGE plpgsql AS $fn$
	DECLARE
		waited boolean := false;
		active boolean;
		seen boolean := false;
	BEGIN
		FOR i IN 1 .. 3000 LOOP
			PERFORM pg_stat_clear_snapshot();
			SELECT count(*) FILTER (WHERE wait_event IN ('BufferCleanup', 'BufferPin')) > 0,
				   count(*) > 0
			  INTO waited, active
			  FROM pg_stat_activity
			 WHERE datname = current_database()
			   AND pid <> pg_backend_pid()
			   AND state = 'active'
			   AND query LIKE 'VACUUM%';
			EXIT WHEN waited;
			seen := seen OR active;
			EXIT WHEN seen AND NOT active;
			PERFORM pg_sleep(0.01);
		END LOOP;
		RETURN waited;
	END $fn$;
}

teardown
{
	DROP FUNCTION dpin_wait_for_vacuum();
	DROP TABLE dpin;
	DO $$ BEGIN PERFORM injection_points_detach('lion-gettuple-batch');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

session s1
setup
{
	SET pg_lion.enable_count_pushdown = off;
	SET enable_seqscan = off;
	SET enable_bitmapscan = off;
	SELECT injection_points_set_local();
	SELECT injection_points_attach('lion-gettuple-batch', 'wait');
}
# SnapshotDirty: the exclusion check finds the row's own new entry.
step s1_insert	{ INSERT INTO dpin VALUES (3000, 'new'); }
# An MVCC snapshot: a forced plain index scan.
step s1_select	{ SELECT count(*) AS rows_with_k_5 FROM dpin WHERE k = 5; }
# Only runs once s1's parked step has finished, which pins where that is reported.
step s1_after	{ SELECT 'released' AS s1; }

session s2
step s2_delete	{ DELETE FROM dpin WHERE k > 1500; }
step s2_wakeup	{
	SELECT dpin_wait_for_vacuum() AS vacuum_waited_for_the_pin;
	SELECT injection_points_wakeup('lion-gettuple-batch');
}
step s2_release	{ SELECT injection_points_wakeup('lion-gettuple-batch'); }
step s2_detach	{ SELECT injection_points_detach('lion-gettuple-batch'); }

session s3
step s3_vacuum	{ VACUUM (INDEX_CLEANUP ON) dpin; }
step s3_check	{
	SELECT count(*) AS rows, count(DISTINCT k) AS keys FROM dpin;
	SELECT lion_index_verify('dpin_k_excl', true);
}

# The dirty scan holds its batch's page: VACUUM waits for it.
permutation
	s2_delete
	s1_insert					# parks with the leaf of entry 3000 pinned
	s3_vacuum(*, s1_insert)		# waits for the cleanup lock on that leaf
	s2_wakeup					# asserts the wait, then releases s1
	s1_after
	s3_check
	s2_detach

# The MVCC scan holds nothing: VACUUM runs to completion under it.
permutation
	s2_delete
	s1_select					# parks with a batch loaded and no pin
	s3_vacuum					# completes while s1 is parked
	s2_release
	s1_after
	s3_check
	s2_detach
