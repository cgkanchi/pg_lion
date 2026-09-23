# On-access pruning sets the visibility map (PostgreSQL 19+), and the §9
# interlock still holds (DESIGN.md §11, "On-access pruning sets the visibility
# map too").
#
# §9 counts a container's members straight from the visibility map for every
# heap block that is all-visible when it asks, as long as it still pins the
# index page the container came from.  It was argued when only VACUUM set the
# map.  On 19 every read-only core scan may set it while pruning, and so does
# the count's own heap recheck; the claim that keeps counts exact is that a
# page holding a dead ROOT line pointer (LP_DEAD) is never made all-visible,
# and that only VACUUM, after ambulkdelete, frees one.  This spec puts all
# three writers of the map around a count that is parked with its container
# pinned and checks the answers:
#
#  * s2_predel: 50 rows of k = 1 dead to everybody, so VACUUM has index
#    entries to remove and must wait for the count's pin.
#  * s2_hot: HOT updates of rows of k = 1, committed BEFORE s1's snapshot, on
#    pages nothing else touches: once pruned they are all-visible, even to s1.
#  * s2_delete / s2_hot2: 250 more rows of k = 1 deleted, and more HOT
#    updates, both AFTER s1's snapshot: s1 still sees those rows and their old
#    versions, so their pages must stay off the map while s1 is open.
#  * s1 counts k = 1 through the pushdown (rel_read_only: the count prunes and
#    may set the map) and parks at lion-count-containers-pinned.
#  * Permutation 1: s4 runs a plain sequential scan - core's on-access pruning,
#    which sets the map on 19 - and s3's VACUUM prunes its first pass and then
#    blocks on the pin.  s1 is woken and must count 950: every TID of the
#    predeleted rows is LP_DEAD by then, which keeps its page off the map.
#  * Permutation 2: no core scan; s1's own recheck prunes (and on 19 marks the
#    HOT-only pages all-visible), then s1 counts AGAIN in the same transaction,
#    parks again with the pin held while VACUUM waits for it, and must still
#    count 950 - partly from pages its own first count put on the map.
#
# The answers do not depend on the version: 16-18 prune without setting the
# map (and the count does not call it there), 19 and later set it.  Injection
# points need 17; the Makefile leaves this spec out where they are missing.
#
# Page geometry, as in test/sql/prune.sql: fixed-width rows and fillfactor 90
# leave a little under 10% of each page free, so a few HOT updates per page
# take it below the threshold at which heap_page_prune_opt() prunes.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE TABLE cpr (id int NOT NULL, k int NOT NULL, v int NOT NULL,
					  pad text NOT NULL)
		WITH (fillfactor = 90, autovacuum_enabled = off);
	INSERT INTO cpr SELECT i, i % 4, 0, repeat('x', 60)
	  FROM generate_series(1, 4000) i;
	CREATE INDEX cpr_k ON cpr USING lion (k);

	CREATE FUNCTION cpr_wait_for_vacuum() RETURNS boolean
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
	DROP FUNCTION cpr_wait_for_vacuum();
	DROP TABLE cpr;
}

# The counting session: the pushdown, and no other plan for its count.
session s1
setup
{
	SET enable_seqscan = off;
	SET enable_bitmapscan = off;
	SET enable_indexscan = off;
	SELECT injection_points_set_local();
	SELECT injection_points_attach('lion-count-containers-pinned', 'wait');
}
step s1_begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1 AS snapshot; }
step s1_count	{ SELECT count(*) FROM cpr WHERE k = 1; }
step s1_count2	{ SELECT count(*) FROM cpr WHERE k = 1; }
step s1_commit	{ COMMIT; }

# The writer, which also releases s1 from the injection point.
session s2
setup			{ SET synchronous_commit = on; SET pg_lion.enable_count_pushdown = off; }
step s2_predel	{ DELETE FROM cpr WHERE k = 1 AND id > 3800; }
step s2_hot		{ UPDATE cpr SET v = 1 WHERE k = 1 AND id BETWEEN 1500 AND 3000 AND id % 40 IN (1, 21); }
step s2_delete	{ DELETE FROM cpr WHERE k = 1 AND id <= 1000; }
step s2_hot2	{ UPDATE cpr SET v = 2 WHERE k = 1 AND id BETWEEN 1001 AND 1400 AND id % 40 IN (1, 21); }
step s2_wakeup	{
	SELECT cpr_wait_for_vacuum() AS vacuum_waited_for_the_pin;
	SELECT injection_points_wakeup('lion-count-containers-pinned');
}
step s2_release	{ SELECT injection_points_wakeup('lion-count-containers-pinned'); }
step s2_detach	{ SELECT injection_points_detach('lion-count-containers-pinned'); }

# The vacuuming session, and the reference answers afterwards.
session s3
setup			{ SET synchronous_commit = on; SET pg_lion.enable_count_pushdown = off; }
step s3_prep	{ VACUUM (FREEZE) cpr; }
step s3_vacuum	{ VACUUM cpr; }
step s3_count	{
	SELECT count(*) AS seq_k1 FROM cpr WHERE k = 1;
	SELECT lion_index_count('cpr_k', 1) AS index_k1;
}
step s3_verify	{ SELECT lion_index_verify('cpr_k', true); }

# Core's on-access pruning: a sequential scan of a relation the query does not
# modify, which on 19 marks every page it cleans all-visible if it can.
session s4
setup			{ SET pg_lion.enable_count_pushdown = off; SET enable_indexscan = off; SET enable_bitmapscan = off; }
step s4_scan	{ SELECT count(*) AS seq_all FROM cpr; }

permutation
	s3_prep					# every page all-visible and frozen
	s2_predel s2_hot		# before s1's snapshot
	s1_begin				# s1's snapshot: 950 rows with k = 1
	s2_delete s2_hot2		# after it
	s1_count				# parks with the container page pinned
	s4_scan					# core prunes, and sets the map where it may
	s3_vacuum(*, s1_count)	# prunes, then blocks on the pin
	s2_wakeup				# asserts the wait, then releases s1: 950
	s1_commit
	s3_count				# 700
	s3_verify
	s2_detach

permutation
	s3_prep
	s2_predel s2_hot
	s1_begin
	s2_delete s2_hot2
	s1_count				# parks with the container page pinned
	s2_release				# the recheck prunes (19: HOT-only pages go on the map): 950
	s1_count2				# parks again
	s3_vacuum(*, s1_count2)	# blocks on the pin
	s2_wakeup				# releases s1: 950 again
	s1_commit
	s3_count				# 700
	s3_verify
	s2_detach
