# lion_index_verify() beside concurrent writers (DESIGN.md §7).
#
# verify() compares whole levels of the directory and of every posting tree
# with each other and proves every block reachable, which a writer splitting
# pages between two of its reads can make fail on a sound index.  Under
# AccessShareLock and nothing else, a loop of inserts that split the directory
# against a loop of verify() calls made it report
#
#	the directory points at block 733, but the index has only 732 blocks
#
# about an index that was fine (2026-09-25 review).  It then took ShareLock,
# which kept every writer out while it ran.  It takes ShareUpdateExclusiveLock
# now, as CREATE INDEX CONCURRENTLY does: INSERTs go on, VACUUM waits, and
# every check a concurrent INSERT can make fail is settled on the spot, walked
# again, or recorded as a candidate that is checked again once the writers
# that were in flight are done.  Every permutation below parks verify() at an
# injection point with nothing held, makes a writer change the index under it
# in one of the ways an INSERT can, and requires the check to come back clean
# - and the index to be what the writer made it, which the observer shows.
#
# The directory (vc: 3000 keys of about 110 bytes, three levels):
#  * split_right: parked after the leaves are walked, and again after level
#    1, new keys split the last leaves and then the last page of level 1.
#    The level above then holds downlinks to pages the walk of the level
#    below never reached, and nothing reached the new pages: candidates,
#    settled after the wait.
#  * split_left: parked on the FIRST leaf, new keys split it.  The walk goes
#    on to the page that was its right sibling, whose left link now names the
#    split's new page: settled by walking the level again, left to right.
#  * split_root (vr: ten keys, one leaf that is the root): parked with the
#    root and the height read from the meta page, inserts split the root.  The
#    old root is a page without the root flag at the height the check expects:
#    settled by reading the meta page again.
# Posting trees:
#  * posting_split (vp: two keys on alternate rows, bitset leaves under a
#    root): parked after one set's leaves are walked, an insert splits its last
#    leaf.  The level above holds a downlink the leaf walk never saw, and the
#    entry's counters moved: the set is walked again.
#  * pushdown (vq: four one-page sets): parked after one of them is walked, an
#    insert pushes its root down (the root keeps its block) and adds a key
#    whose set spills and grows at once.  The first set is walked again; the
#    new key's pages are candidates, and its root is found through its entry.
#  * spill: parked after the leaves of vc, an existing INLINE entry spills to
#    a posting tree.  The new root is a candidate, found through the entry.
#  * held: a set a writer changes under every walk is walked
#    LION_VERIFY_SET_ATTEMPTS times with nothing held and then once with its
#    entry's directory leaf held SHARE; the 'notice' on
#    'lion-verify-set-held' shows that walk happening.
#  * fsm_reuse: a key's rows are deleted and VACUUM frees its posting tree.
#    Parked after the leaves, an insert splits the other key's leaves into
#    those freed pages (the relation does not grow, deleted_pages falls): live
#    pages the reachability pass finds unreached, settled through their root.
#  * fsm_new_set: the same, with a key that did not exist when the leaves were
#    walked.  Its root comes from the end of the relation and its leaves from
#    the freed pages, so the unreached pages name a root nothing reached
#    either: it is settled through the entry that names it.  (A first version
#    of the recheck reported "block 7 is not reachable from the meta page".)
# Locks:
#  * vacuum_waits: VACUUM needs the lock verify() holds and waits for it.
#  * writer_waited: parked after the leaves, one insert splits leaves and
#    another is stopped half way (on an advisory lock) after its first row, so
#    that it holds the index open for writing.  verify() has candidates, and
#    waits for that statement's transaction before checking them again.
#  * writer_not_waited: the same splits made by a transaction that stays open
#    once its INSERT is over.  A statement lets its lock on the index go when
#    it ends, having finished every split it began, so verify() has nothing
#    to wait for.
#  * lock_drop: `LOCK TABLE; DROP INDEX` in another transaction goes through
#    while verify() waits for the table: it takes the table first, holding
#    nothing on the index (the deadlock the old lock order had).
#
# A parked step carries no (*) marker: isolationtester sees a session waiting
# on an injection point as blocked, so it moves on only once verify() has
# really parked.  The markers in parentheses pin the order completions are
# reported in.

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;

	CREATE TABLE vc (k text NOT NULL) WITH (autovacuum_enabled = off);
	INSERT INTO vc SELECT 'key' || g || repeat('x', 100) FROM generate_series(1, 3000) g;
	CREATE INDEX vc_k ON vc USING lion (k) WITH (inline_limit = 64);

	CREATE TABLE vr (k text NOT NULL) WITH (autovacuum_enabled = off);
	INSERT INTO vr SELECT 'key' || g FROM generate_series(1, 10) g;
	CREATE INDEX vr_k ON vr USING lion (k) WITH (inline_limit = 64);

	CREATE TABLE vp (id int, k int NOT NULL) WITH (autovacuum_enabled = off);
	INSERT INTO vp SELECT i, i % 2 FROM generate_series(1, 60000) i;
	CREATE INDEX vp_k ON vp USING lion (k) WITH (inline_limit = 64);

	CREATE TABLE vq (id int, k int NOT NULL) WITH (autovacuum_enabled = off);
	INSERT INTO vq SELECT i, i % 4 FROM generate_series(1, 4000) i;
	CREATE INDEX vq_k ON vq USING lion (k) WITH (inline_limit = 64);

	CREATE TABLE v_before AS
	SELECT (SELECT leaf_pages FROM lion_index_stats('vc_k')) AS vc_leaves,
		   (SELECT internal_pages FROM lion_index_stats('vc_k')) AS vc_internal,
		   (SELECT inline_entries FROM lion_index_stats('vc_k')) AS vc_inline,
		   (SELECT directory_height FROM lion_index_stats('vr_k')) AS vr_height,
		   (SELECT container_pages FROM lion_index_stats('vp_k')) AS vp_leaves,
		   (SELECT max_posting_height FROM lion_index_stats('vq_k')) AS vq_height,
		   lion_index_posting_root('vq_k', 0) AS vq_root;
}

teardown
{
	DROP TABLE vc, vr, vp, vq, v_before;
	DROP TABLE IF EXISTS v_free;
	/* Injection points are cluster-wide: never leave one attached, and
	 * never fail the teardown because the permutation already did. */
	DO $$ BEGIN PERFORM injection_points_detach('lion-verify-meta-read');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
	DO $$ BEGIN PERFORM injection_points_detach('lion-verify-dir-level-walked');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
	DO $$ BEGIN PERFORM injection_points_detach('lion-verify-dir-page-read');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
	DO $$ BEGIN PERFORM injection_points_detach('lion-verify-set-leaves-walked');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
	DO $$ BEGIN PERFORM injection_points_detach('lion-verify-set-held');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

# The checker.
session s1
setup			{ SELECT injection_points_set_local(); }
step s1_park_meta	{ SELECT injection_points_attach('lion-verify-meta-read', 'wait'); }
step s1_park_level	{ SELECT injection_points_attach('lion-verify-dir-level-walked', 'wait'); }
step s1_park_page	{ SELECT injection_points_attach('lion-verify-dir-page-read', 'wait'); }
step s1_park_set	{ SELECT injection_points_attach('lion-verify-set-leaves-walked', 'wait'); }
step s1_note_held	{ SELECT injection_points_attach('lion-verify-set-held', 'notice'); }
step s1_verify_c	{ SELECT lion_index_verify('vc_k', true); }
step s1_verify_r	{ SELECT lion_index_verify('vr_k', true); }
step s1_verify_p	{ SELECT lion_index_verify('vp_k', true); }
step s1_verify_q	{ SELECT lion_index_verify('vq_k', true); }

# The writer.
session s2
step s2_begin		{ BEGIN; }
step s2_commit		{ COMMIT; }
step s2_split_right	{
	INSERT INTO vc SELECT 'new' || g || '-' || repeat('y', 120)
	  FROM generate_series(1, 1000) g;
}
step s2_split_right2	{
	INSERT INTO vc SELECT 'new' || g || '-' || repeat('y', 120)
	  FROM generate_series(1001, 2000) g;
}
# Inserts one row, and waits for s3's advisory lock before the second.
step s2_late		{
	INSERT INTO vc SELECT 'late' || g ||
		   CASE WHEN g > 1 THEN pg_advisory_xact_lock_shared(4711)::text ELSE '' END
	  FROM generate_series(1, 3) g;
}
step s2_split_left	{
	INSERT INTO vc SELECT 'key0-' || g || repeat('z', 100)
	  FROM generate_series(1, 100) g;
}
step s2_split_root	{
	INSERT INTO vr SELECT 'root' || g || repeat('r', 200)
	  FROM generate_series(1, 300) g;
}
step s2_spill		{
	INSERT INTO vc SELECT 'key1' || repeat('x', 100) FROM generate_series(1, 200);
}
step s2_grow_p		{ INSERT INTO vp SELECT 100000 + i, 0 FROM generate_series(1, 30000) i; }
step s2_touch_p		{ INSERT INTO vp SELECT 200000 + i, 0 FROM generate_series(1, 100) i; }
step s2_grow_new	{ INSERT INTO vp SELECT 100000 + i, 2 FROM generate_series(1, 30000) i; }
step s2_push_q		{
	INSERT INTO vq SELECT 4000 + i, CASE WHEN i % 2 = 0 THEN 0 ELSE 5 END
	  FROM generate_series(1, 40000) i;
}
step s2_free_p		{ DELETE FROM vp WHERE k = 1; }
step s2_vacuum_p	{ VACUUM vp; }
step s2_xid			{ SELECT pg_current_xact_id() IS NOT NULL AS took_an_xid; }
step s2_vacuum_c	{ VACUUM vc; }
step s2_lock		{ BEGIN; LOCK TABLE vc IN ACCESS EXCLUSIVE MODE; }
step s2_drop		{ DROP INDEX vc_k; }
step s2_rollback	{ ROLLBACK; }

# The observer.
session s3
step s3_hold		{ SELECT pg_advisory_lock(4711); }
step s3_release		{ SELECT pg_advisory_unlock(4711); }
step s3_wake_meta	{
	SELECT injection_points_detach('lion-verify-meta-read');
	SELECT injection_points_wakeup('lion-verify-meta-read');
}
step s3_wake_level	{
	SELECT injection_points_detach('lion-verify-dir-level-walked');
	SELECT injection_points_wakeup('lion-verify-dir-level-walked');
}
step s3_wake_page	{
	SELECT injection_points_detach('lion-verify-dir-page-read');
	SELECT injection_points_wakeup('lion-verify-dir-page-read');
}
# ... and let it park again, after the next level.
step s3_wake_level_again	{ SELECT injection_points_wakeup('lion-verify-dir-level-walked'); }
step s3_wake_set	{
	SELECT injection_points_detach('lion-verify-set-leaves-walked');
	SELECT injection_points_wakeup('lion-verify-set-leaves-walked');
}
# ... and let it park at the same point again, in the next walk of the set.
step s3_wake_set_again	{ SELECT injection_points_wakeup('lion-verify-set-leaves-walked'); }
step s3_note_free	{
	CREATE TABLE v_free AS
	SELECT deleted_pages, pg_relation_size('vp_k') AS size
	  FROM lion_index_stats('vp_k');
	SELECT deleted_pages > 0 AS vacuum_freed_pages FROM v_free;
}
step s3_dir	{
	SELECT s.leaf_pages > b.vc_leaves AS the_leaves_split,
		   s.internal_pages > b.vc_internal AS the_internal_pages_split,
		   (SELECT count(*) FROM vc) AS rows
	  FROM lion_index_stats('vc_k') s, v_before b;
	SELECT lion_index_verify('vc_k', true);
}
step s3_root	{
	SELECT s.directory_height > b.vr_height AS the_root_split
	  FROM lion_index_stats('vr_k') s, v_before b;
	SELECT lion_index_verify('vr_k', true);
}
step s3_spilled	{
	SELECT s.inline_entries = b.vc_inline - 1 AS the_entry_spilled,
		   lion_index_posting_root('vc_k', 'key1' || repeat('x', 100)) IS NOT NULL
		   AS it_has_a_posting_tree
	  FROM lion_index_stats('vc_k') s, v_before b;
	SELECT lion_index_verify('vc_k', true);
}
step s3_posting	{
	SELECT s.container_pages > b.vp_leaves AS the_leaves_split,
		   (SELECT count(*) FROM vp WHERE k = 0) AS k0_rows
	  FROM lion_index_stats('vp_k') s, v_before b;
	SELECT lion_index_verify('vp_k', true);
}
step s3_pushdown	{
	SELECT s.max_posting_height > b.vq_height AS the_root_was_pushed_down,
		   lion_index_posting_root('vq_k', 0) = b.vq_root AS the_root_kept_its_block,
		   lion_index_posting_root('vq_k', 5) IS NOT NULL AS the_new_key_spilled
	  FROM lion_index_stats('vq_k') s, v_before b;
	SELECT lion_index_verify('vq_k', true);
}
step s3_touched	{
	SELECT (SELECT count(*) FROM vp WHERE k = 0) AS k0_rows;
	SELECT lion_index_verify('vp_k', true);
}
step s3_reused	{
	SELECT s.deleted_pages < f.deleted_pages AS freed_pages_were_taken,
		   pg_relation_size('vp_k') = f.size AS the_index_did_not_grow
	  FROM lion_index_stats('vp_k') s, v_free f;
	SELECT lion_index_verify('vp_k', true);
}
step s3_new_set	{
	SELECT s.deleted_pages < f.deleted_pages AS freed_pages_were_taken,
		   lion_index_posting_root('vp_k', 2) IS NOT NULL AS the_new_key_has_a_tree
	  FROM lion_index_stats('vp_k') s, v_free f;
	SELECT lion_index_verify('vp_k', true);
}

permutation
	s1_park_level
	s1_verify_c(s3_wake_level)	# parks with the leaves walked
	s2_split_right				# goes through: nothing waits for the check
	s3_wake_level_again			# parks again with level 1 walked
	s2_split_right2				# splits leaves, and a page of level 1
	s3_wake_level				# the check settles what the splits left
	s3_dir

permutation
	s1_park_page
	s1_verify_c(s3_wake_page)	# parks on the first leaf
	s2_split_left				# splits it
	s3_wake_page				# the next leaf's left link is the new page
	s3_dir

permutation
	s1_park_meta
	s1_verify_r(s3_wake_meta)	# parks with the root and height read
	s2_split_root				# splits the root
	s3_wake_meta
	s3_root

permutation
	s1_park_set
	s1_verify_p(s3_wake_set)	# parks with the first set's leaves walked
	s2_grow_p					# splits its last leaf
	s3_wake_set					# the walk sees a downlink it did not expect
	s3_posting

permutation
	s1_park_set
	s1_verify_q(s3_wake_set)	# parks with a one-page set walked
	s2_push_q					# pushes its root down; a new key spills
	s3_wake_set
	s3_pushdown

permutation
	s1_park_level
	s1_verify_c(s3_wake_level)
	s2_spill					# an INLINE entry the walk checked spills
	s3_wake_level
	s3_spilled

permutation
	s1_park_set
	s1_note_held
	s1_verify_p(s3_wake_set)	# attempt 1 parks
	s2_touch_p
	s3_wake_set_again			# attempt 1 is thrown away; attempt 2 parks
	s2_touch_p
	s3_wake_set_again			# attempt 3 parks
	s2_touch_p
	s3_wake_set					# thrown away too: the leaf is held (NOTICE)
	s3_touched

permutation
	s2_free_p
	s2_vacuum_p					# k = 1's posting tree goes to the free space map
	s2_xid						# ... behind every snapshot from here on
	s3_note_free
	s1_park_level
	s1_verify_p(s3_wake_level)	# parks with the leaves walked
	s2_grow_p					# splits k = 0's leaves into the freed pages
	s3_wake_level
	s3_reused

permutation
	s2_free_p
	s2_vacuum_p
	s2_xid
	s3_note_free
	s1_park_level
	s1_verify_p(s3_wake_level)	# parks with the leaves walked
	s2_grow_new					# a new key: its root from the end of the
								# relation, its leaves from the freed pages
	s3_wake_level
	s3_new_set

permutation
	s1_park_meta
	s1_verify_c(s3_wake_meta)
	s2_vacuum_c(s1_verify_c)	# waits for the check's lock
	s3_wake_meta
	s3_dir

permutation
	s3_hold
	s1_park_level
	s1_verify_c(s2_late)		# parks with the leaves walked
	s2_split_right
	s2_late						# one row in, then stopped: holds the index
	s3_wake_level				# the check has candidates, and waits for it
	s3_release					# the insert ends, and then the check
	s3_dir

permutation
	s2_begin
	s1_park_level
	s1_verify_c					# parks with the leaves walked
	s2_split_right				# in a transaction that stays open
	s3_wake_level				# candidates, but no statement to wait for
	s2_commit
	s3_dir

permutation
	s2_lock
	s1_verify_c(s2_rollback)	# waits for the table, holding nothing on the index
	s2_drop						# ... so this does not wait for it
	s2_rollback
