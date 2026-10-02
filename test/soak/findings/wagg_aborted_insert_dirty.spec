# FINDING (2026-10-01 soak), the same as wagg_aborted_insert.spec but with
# one page NOT all-visible when the aggregates begin.
#
# On main (dc7cd29) the first look at the visibility map then fails and every
# entry is counted, which is right: this spec passes there.  It is here for
# the unmerged branch claude/lion-walkcost-wagg (4139040), whose
# LION_WALK_DIRTY walk takes an entry's ntids whenever none of its members
# lies under a container key that held a page not all-visible at the first
# look, and keeps the walk if no other key turned dirty by the second.  An
# aborted insert onto a clean key's page, pruned and re-marked by VACUUM
# between the two looks, passes both tests, so the phantom row is counted
# there too.
#
#  * s2_dirty: a HOT update of the row on the heap's last page, committed
#    before s1's statement: one page (and one container key) not all-visible.
#  * the rest as in wagg_aborted_insert.spec.
#
# Expected (correct) result: s1's answer equals s3_reference's.
#
# PR #15 (claude/lion-wagg-aborted) fixes main's walk: a VACUUM's bulk delete
# between the two looks at the map, counted in the meta page, sends the walk
# to exact counting.  A branch with the dirty-key walk needs the same check
# on that walk; this spec is the test of it.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE TABLE wa (id int, k int, pad text)
		WITH (fillfactor = 50, autovacuum_enabled = off);
	INSERT INTO wa SELECT r, k, 'x'
	  FROM generate_series(0, 9999) k, generate_series(1, 2) r
	 WHERE k % 50 <> 49;
	ALTER TABLE wa SET (fillfactor = 100);
	CREATE INDEX wa_k ON wa USING lion (k);
}

teardown
{
	DROP TABLE wa;
	DO $$ BEGIN PERFORM injection_points_detach('lion-entry-scan-leaf');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

session s1
setup
{
	SET max_parallel_workers_per_gather = 0;
	SET enable_seqscan = off;
	SELECT injection_points_set_local();
	SELECT injection_points_attach('lion-entry-scan-leaf', 'wait');
}
step s1_aggs	{ SELECT sum(k), max(k % 50) FROM wa; }

session s2
setup			{ SET pg_lion.enable_count_pushdown = off; }
step s2_abort	{
	BEGIN;
	INSERT INTO wa SELECT -1, k, 'y' FROM (SELECT DISTINCT k FROM wa) d;
	INSERT INTO wa SELECT -2, k, 'y' FROM generate_series(49, 9999, 50) k;
	ROLLBACK;
}
step s2_dirty	{ UPDATE wa SET pad = 'z' WHERE k = 9998 AND id = 2; }

session s3
setup			{ SET max_parallel_workers_per_gather = 0; SET enable_seqscan = off; }
step s3_prep	{ VACUUM (FREEZE, ANALYZE) wa; }
step s3_how		{
	CREATE TEMP TABLE wa_pages AS SELECT pg_relation_size('wa') AS before;
	DO $$
	DECLARE ln text; found bool := false;
	BEGIN
		FOR ln IN EXPLAIN (COSTS OFF) SELECT sum(k), max(k % 50) FROM wa LOOP
			IF ln ~ 'Aggregates Over Keys' THEN found := true; END IF;
		END LOOP;
		RAISE NOTICE 'aggregates over keys planned: %', found;
	END $$;
	SELECT leaf_pages >= 3 AS three_leaves_or_more FROM lion_index_stats('wa_k');
}
step s3_parked	{ SELECT count(*) = 1 AS parked FROM pg_stat_activity WHERE wait_event_type = 'InjectionPoint' AND wait_event = 'lion-entry-scan-leaf'; }
step s3_wake_once	{ SELECT injection_points_wakeup('lion-entry-scan-leaf'); }
step s3_vacuum	{ VACUUM (INDEX_CLEANUP ON) wa; }
step s3_release	{
	SELECT injection_points_detach('lion-entry-scan-leaf');
	DO $$
	DECLARE n int; zero int := 0;
	BEGIN
		FOR i IN 1 .. 6000 LOOP
			SELECT count(*) INTO n FROM pg_stat_activity
			 WHERE wait_event = 'lion-entry-scan-leaf';
			IF n = 0 THEN
				zero := zero + 1;
				EXIT WHEN zero >= 10;
			ELSE
				zero := 0;
				BEGIN
					PERFORM injection_points_wakeup('lion-entry-scan-leaf');
				EXCEPTION WHEN OTHERS THEN NULL;
				END;
			END IF;
			PERFORM pg_sleep(0.01);
		END LOOP;
	END $$;
}
step s3_reference	{
	SET pg_lion.enable_count_pushdown = off;
	SET enable_seqscan = on;
	SET enable_bitmapscan = off;
	SET enable_indexscan = off;
	SELECT sum(k), max(k % 50) FROM wa;
	RESET pg_lion.enable_count_pushdown;
	SET enable_seqscan = off;
	RESET enable_bitmapscan;
	RESET enable_indexscan;
}
step s3_after	{ SELECT sum(k), max(k % 50) FROM wa; }
step s3_verify	{ SELECT lion_index_verify('wa_k', true); }

permutation
	s3_prep				# every page all-visible
	s3_how				# the plan takes the aggregates over keys
	s2_dirty			# ... but one page is not, before s1 begins
	s1_aggs(s3_release)	# parks before the second leaf
	s3_parked
	s2_abort			# one aborted TID in every entry
	s3_wake_once		# s1 reads one leaf, parks before the next
	s3_parked
	s3_vacuum			# the aborted TIDs go, the pages are marked again
	s3_release			# s1 finishes
	s3_reference		# the answer from the heap
	s3_after			# and LionCount again, after the VACUUM
	s3_verify
