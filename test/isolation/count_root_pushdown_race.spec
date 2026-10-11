# A count parked on the HEAD of a posting set without an internal level while
# an insert pushes that head down and a VACUUM then cleans the containers it
# moved (DESIGN.md §11, §22: "the barrier must follow the postings").
#
# A posting set of at most 16 leaves has no internal level: its head block is
# its first leaf.  The counting code copies that page, keeps the PIN, and only
# then asks the visibility map about the heap blocks the containers cover
# (§9).  An INSERT that needs a 17th leaf needs only exclusive content locks,
# so it may run in that window, and it PROMOTES the set (§22): the head's
# items go to a brand new child and the head block becomes an internal page
# holding a downlink to every leaf.  The reader's copy now describes
# containers that live on the child.
#
# A VACUUM that descended straight to the leftmost leaf would never ask for a
# cleanup lock on the (still pinned) root - it is not a leaf any more - so it
# would clean the child, finish ambulkdelete, and let the heap phase mark the
# deleted rows' pages all-visible; the reader, woken afterwards, would then
# count the dead rows straight from its copy.  The rule that closes it: VACUUM
# visits the posting tree TOP-DOWN and takes a cleanup lock on every page of
# its descent, internal ones included, so the reader's pin on the old root
# stops it before it can reach the page the containers moved to.
#
# The numbers: k = 1 has 1000 rows, all in heap blocks 0..57, the first
# container key, and then 30,000 more interleaved with 30,000 of k = 5, about
# 14 leaves a key - every other row, and wide rows, so that each container is
# a BITSET filling a leaf of its own (inline_limit = 64 keeps the sets off the
# directory leaf).  s2_predel deletes 500 of the first 1000 and commits before
# s1's snapshot, so the right answer for s1 is 30,500.  s2_grow adds 20,000
# more k = 1 rows (interleaved with 20,000 of k = 5), which need more than 16
# leaves and promote the set (max_posting_height goes from 0 to 1, the head
# block stays where it was); they commit after s1's snapshot and live on
# pages VACUUM cannot mark all-visible while s1's snapshot holds the horizon
# back, so they are rechecked and not counted.  A wrong interlock shows up as
# s1 answering 31,000 and vacuum_waited_for_the_pin = f.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE TABLE rpd (id int, k int NOT NULL, pad char(80) NOT NULL DEFAULT 'x');
	INSERT INTO rpd SELECT i, i % 4 FROM generate_series(1, 4000) i;
	CREATE INDEX rpd_k ON rpd USING lion (k) WITH (inline_limit = 64);
	INSERT INTO rpd SELECT 10000 + i, CASE WHEN i % 2 = 0 THEN 1 ELSE 5 END
	  FROM generate_series(1, 60000) i;

	CREATE FUNCTION rpd_wait_for_vacuum() RETURNS boolean
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
}

teardown
{
	DROP FUNCTION rpd_wait_for_vacuum();
	DROP TABLE rpd;
	DO $$ BEGIN PERFORM injection_points_detach('lion-count-containers-pinned');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

# The counting session.  lion_index_count() is what parks; the plain count is
# only there to take the snapshot's view for comparison.
session s1
setup
{
	SET pg_lion.enable_count_pushdown = off;
	SELECT injection_points_set_local();
	SELECT injection_points_attach('lion-count-containers-pinned', 'wait');
}
step s1_count	{ SELECT lion_index_count('rpd_k', 1); }

# The writer.
session s2
setup			{ SET pg_lion.enable_count_pushdown = off; SET synchronous_commit = on; }
step s2_predel	{ DELETE FROM rpd WHERE k = 1 AND id <= 2000; }
# Every other row, so that the new members are BITSET containers: consecutive
# offsets would compress into a RUN a few bytes long and never fill a leaf.
step s2_grow	{
	INSERT INTO rpd SELECT 100000 + i, CASE WHEN i % 2 = 0 THEN 1 ELSE 5 END
	  FROM generate_series(1, 40000) i;
}
step s2_wakeup	{
	SELECT rpd_wait_for_vacuum() AS vacuum_waited_for_the_pin;
	SELECT injection_points_detach('lion-count-containers-pinned');
	SELECT injection_points_wakeup('lion-count-containers-pinned');
}

# The bookkeeping and vacuuming session.
session s3
setup			{ SET pg_lion.enable_count_pushdown = off; SET synchronous_commit = on; }
step s3_prep	{ VACUUM (FREEZE) rpd; }
step s3_root	{
	CREATE TABLE rpd_root AS SELECT lion_index_posting_root('rpd_k', 1) AS root;
}
step s3_before	{
	SELECT max_posting_height AS height_before FROM lion_index_stats('rpd_k');
}
step s3_after	{
	SELECT max_posting_height AS height_after,
		   lion_index_posting_root('rpd_k', 1) = (SELECT root FROM rpd_root)
			 AS root_block_kept
	  FROM lion_index_stats('rpd_k');
}
step s3_vacuum	{ VACUUM (INDEX_CLEANUP ON) rpd; }
step s3_count	{
	SET enable_seqscan = on; SET enable_bitmapscan = off;
	SELECT count(*) FROM rpd WHERE k = 1;
}
step s3_verify	{ SELECT lion_index_verify('rpd_k', true); DROP TABLE rpd_root; }

permutation
	s3_prep					# all-visible heap
	s3_root					# the root's block number
	s2_predel				# 500 rows dead to everyone, heap not vacuumed
	s3_before				# no internal level: the head is the first leaf
	s1_count(s2_wakeup)	# parks holding a pin on that head, with a copy
							# of the container holding the 500 dead TIDs
	s2_grow					# a 17th leaf: the head is pushed down
	s3_after				# the head is internal now, at the same block
	s3_vacuum(*, s1_count)	# must wait for s1's pin on the old root
	s2_wakeup				# asserts the wait, then releases s1: 30,500
	s3_count				# 50,500 committed rows now
	s3_verify
