# Range counts that sum SUMMARY posting sets, and lion_index_verify()'s check
# of them, beside the writers that change them (DESIGN.md §31).
#
# The table: 4000 rows, k = 1 .. 4000 in heap order, and a lion index on k
# with buckets of 1000 rows - summaries keyed 1000, 2000, 3000 and the open
# one, 4000 - so that each bucket's rows have heap pages of their own.
#
#  * vacuum: `count(*) WHERE k <= 3500` sums the summaries of the first three
#    buckets and walks the keys 3001 .. 3500.  Every row of the first bucket is
#    deleted and committed before s1 takes its snapshot, so VACUUM may remove
#    them - from the first summary as from their keys - and then mark their
#    pages all-visible.  s1 parks on 'lion-count-containers-pinned' at the
#    first container it counts, which is the first summary's, with the
#    directory leaf that holds it pinned; a VACUUM started now has to wait for
#    that pin (sr_wait_for_vacuum() asserts it), or s1 would count a thousand
#    dead rows.  Rows of the second bucket deleted AFTER s1's snapshot are
#    still counted by s1, through the heap recheck.  2500.
#  * phases: `count(*) WHERE k >= 500` walks the keys 500 .. 1000 (the first
#    bucket straddles the bound), then the summaries of every later bucket,
#    the open one included.  s1 parks on 'lion-entry-scan-phase' between the
#    two, holding nothing of the directory, while s2 appends 3000 rows above
#    every key - closing the open bucket and opening three more, raising the
#    open one's key as it goes - and adds rows to the keys of the middle
#    buckets, splitting the leaves the next phase descends into.  s2 commits
#    after s1's snapshot, so s1 sees none of it: 3501, as its own count with
#    the pushdown off says.
#  * bounded: the same with `k BETWEEN 500 AND 3500`, whose last bucket is not
#    whole: the walk goes on past the summaries to the keys 3001 .. 3500, and
#    parks at each phase in turn.
#  * verify: an insert is parked on 'lion-insert-before-summary', with its row
#    under its key and in no summary yet.  lion_index_verify() finds the row in
#    the key and not in the bucket's summary, waits for the inserting
#    statement, which s2 then releases, and finds it in the summary on its
#    second look: no error.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE TABLE sr (id int, k int NOT NULL, g int NOT NULL);
	INSERT INTO sr SELECT i, i, i % 3 FROM generate_series(1, 4000) i;
	CREATE INDEX sr_k ON sr USING lion (k) WITH (summaries = on, summary_tids = 1000);
	CREATE INDEX sr_g ON sr USING lion (g);

	CREATE FUNCTION sr_wait_for_vacuum() RETURNS boolean
	LANGUAGE plpgsql AS $fn$
	DECLARE
		waited boolean := false;
		active boolean;
		seen boolean := false;
	BEGIN
		FOR i IN 1 .. 3000 LOOP
			/* pg_stat_activity is otherwise read once per transaction */
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
			/* the VACUUM came and went without ever waiting for the pin */
			EXIT WHEN seen AND NOT active;
			PERFORM pg_sleep(0.01);
		END LOOP;
		RETURN waited;
	END $fn$;

	/* Detach a point, then release whoever is parked on it until no one is. */
	CREATE FUNCTION sr_release(point text) RETURNS void
	LANGUAGE plpgsql AS $fn$
	DECLARE
		n int;
		zero int := 0;
	BEGIN
		PERFORM injection_points_detach(point);
		FOR i IN 1 .. 6000 LOOP
			PERFORM pg_stat_clear_snapshot();
			SELECT count(*) INTO n FROM pg_stat_activity WHERE wait_event = point;
			IF n = 0 THEN
				zero := zero + 1;
				EXIT WHEN zero >= 10;
			ELSE
				zero := 0;
				BEGIN
					PERFORM injection_points_wakeup(point);
				EXCEPTION WHEN OTHERS THEN NULL;
				END;
			END IF;
			PERFORM pg_sleep(0.01);
		END LOOP;
	END $fn$;
}

teardown
{
	DROP FUNCTION sr_wait_for_vacuum();
	DROP FUNCTION sr_release(text);
	DROP TABLE sr;
	/* Injection points are cluster-wide: never leave one attached, and
	 * never fail the teardown because the permutation already did. */
	DO $$ BEGIN PERFORM injection_points_detach('lion-count-containers-pinned');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
	DO $$ BEGIN PERFORM injection_points_detach('lion-entry-scan-phase');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
	DO $$ BEGIN PERFORM injection_points_detach('lion-insert-before-summary');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

# The counting session.  Its reference counts turn the pushdown off, so only
# the pushdown's steps run into a point.
session s1
setup
{
	SET pg_lion.enable_count_pushdown = on;
	SET enable_seqscan = off;
	SET enable_bitmapscan = off;
	SET enable_indexscan = off;
	SET enable_indexonlyscan = off;
	SELECT injection_points_set_local();
}
step s1_attach_pinned	{ SELECT injection_points_attach('lion-count-containers-pinned', 'wait'); }
step s1_attach_phase	{ SELECT injection_points_attach('lion-entry-scan-phase', 'wait'); }
step s1_begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1_plain	{
	SET LOCAL pg_lion.enable_count_pushdown = off;
	SELECT count(*) AS upto_3500 FROM sr WHERE k <= 3500;
	SELECT count(*) AS from_500 FROM sr WHERE k >= 500;
	SELECT count(*) AS between_500_3500 FROM sr WHERE k BETWEEN 500 AND 3500;
	SET LOCAL pg_lion.enable_count_pushdown = on;
}
step s1_upto	{ SELECT count(*) AS upto_3500 FROM sr WHERE k <= 3500; }
step s1_from	{ SELECT count(*) AS from_500 FROM sr WHERE k >= 500; }
step s1_between	{ SELECT count(*) AS between_500_3500 FROM sr WHERE k BETWEEN 500 AND 3500; }
step s1_commit	{ COMMIT; }

# The writer, which also releases whoever is parked.
session s2
setup			{ SET pg_lion.enable_count_pushdown = off; }
step s2_predel	{ DELETE FROM sr WHERE k <= 1000; }
step s2_delete	{ DELETE FROM sr WHERE k BETWEEN 1001 AND 1100; }
step s2_append	{
	INSERT INTO sr SELECT 10000 + i, 4000 + i, i % 3 FROM generate_series(1, 3000) i;
	INSERT INTO sr SELECT 20000 + i, 1000 + i, i % 3 FROM generate_series(1, 2500) i;
}
step s2_wakeup_pinned	{
	SELECT sr_wait_for_vacuum() AS vacuum_waited_for_the_pin;
	SELECT sr_release('lion-count-containers-pinned');
}
step s2_wakeup_phase	{ SELECT sr_release('lion-entry-scan-phase'); }
step s2_wakeup_insert	{ SELECT sr_release('lion-insert-before-summary'); }

# The session whose insert parks between its key and its summary.
session s4
setup			{ SELECT injection_points_set_local(); }
step s4_attach	{ SELECT injection_points_attach('lion-insert-before-summary', 'wait'); }
step s4_insert	{ INSERT INTO sr VALUES (99999, 2500, 1); }

# The observer.
session s3
setup
{
	SET enable_seqscan = off;
	SET enable_bitmapscan = off;
	SET enable_indexscan = off;
	SET enable_indexonlyscan = off;
}
step s3_prep	{ VACUUM (FREEZE, ANALYZE) sr; }
step s3_how		{
	CREATE FUNCTION sr_how(q text) RETURNS text LANGUAGE plpgsql AS $fn$
	DECLARE j jsonb;
	BEGIN
		EXECUTE 'EXPLAIN (ANALYZE, TIMING OFF, COSTS OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) ' || q INTO j;
		RETURN (jsonb_path_query_first(j, 'strict $.** ? (@."Custom Plan Provider" == "LionCount")') ->> 'Summaries Summed');
	END $fn$;
	SELECT sr_how('SELECT count(*) FROM sr WHERE k <= 3500') AS upto_3500_summaries,
		   sr_how('SELECT count(*) FROM sr WHERE k >= 500') AS from_500_summaries,
		   sr_how('SELECT count(*) FROM sr WHERE k BETWEEN 500 AND 3500') AS between_summaries;
	DROP FUNCTION sr_how(text);
}
step s3_vacuum	{ VACUUM sr; }
step s3_counts	{
	SET pg_lion.enable_count_pushdown = off;
	RESET enable_seqscan;
	SELECT count(*) AS upto_3500 FROM sr WHERE k <= 3500;
	SELECT count(*) AS from_500 FROM sr WHERE k >= 500;
	SELECT count(*) AS between_500_3500 FROM sr WHERE k BETWEEN 500 AND 3500;
	SET pg_lion.enable_count_pushdown = on;
	SET enable_seqscan = off;
	SELECT count(*) AS upto_3500 FROM sr WHERE k <= 3500;
	SELECT count(*) AS from_500 FROM sr WHERE k >= 500;
	SELECT count(*) AS between_500_3500 FROM sr WHERE k BETWEEN 500 AND 3500;
}
step s3_shape	{
	SELECT summary_entries, summary_tids = ntids AS every_row_in_a_summary
	  FROM lion_index_stats('sr_k');
}
step s3_verify	{ SELECT lion_index_verify('sr_k', true); }

# VACUUM waits for the pin on the first summary's leaf; the count is 2500.
permutation
	s3_prep					# make the heap all-visible
	s3_how					# the counts sum summaries
	s1_attach_pinned
	s2_predel				# the first bucket: rows nobody can see
	s1_begin s1_plain		# s1's snapshot
	s2_delete				# committed after s1's snapshot
	s1_upto					# parks with the first summary's container pinned
	s3_vacuum(*, s1_upto)	# blocks for the cleanup lock
	s2_wakeup_pinned		# asserts the wait, then releases s1
	s1_commit
	s3_counts
	s3_verify

# Buckets closed, opened and rekeyed, and leaves split, between two phases.
permutation
	s3_prep
	s1_attach_phase
	s1_begin s1_plain
	s1_from					# parks after the keys of the first bucket
	s2_append				# closes and opens buckets, splits leaves
	s2_wakeup_phase			# releases s1: 3501
	s1_commit
	s3_shape
	s3_counts
	s3_verify

permutation
	s3_prep
	s1_attach_phase
	s1_begin s1_plain
	s1_between				# parks before the summaries, and before the keys after them
	s2_append
	s2_wakeup_phase			# releases s1: 3001
	s1_commit
	s3_counts
	s3_verify

# verify() beside an insert that is between its key and its summary.
permutation
	s3_prep
	s4_attach
	s4_insert				# parks: the row is under its key only
	s3_verify(s2_wakeup_insert)	# a candidate; waits for the inserter
	s2_wakeup_insert		# releases the insert, which commits
	s3_shape
