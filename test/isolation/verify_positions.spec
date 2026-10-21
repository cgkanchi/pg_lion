# lion_index_verify() beside writers of a column that stores positions
# (DESIGN.md §7 and §17).
#
# A key of such a column has two trees: its posting set and its position
# tree, whose keys are heap blocks.  Both roots come from the end of the
# relation, and both trees grow out of the free space map.  A page a writer
# makes behind the walk is a live page nothing reached, which the recheck
# settles once the writers are done (verify_concurrent.spec has the rest of
# that story) by finding its root and searching the tree for the page's
# first key.  Two kinds of such page are particular to position trees, and
# for each the check must come back clean rather than report "block N is not
# reachable from the meta page":
#
#  * new_key: a key that did not exist when the walk passed.  The root of its
#    position tree, like that of its posting set, is nothing the walk reached
#    and has to be found through the entry that names it, as a position root.
#  * grown_key: an existing key's position tree, with no internal level,
#    grows by leaves out of the free space map.  A position leaf starts at the
#    heap block its left neighbour ends with when one block's rows straddle
#    the two, so a search for the new leaf's first key that walks right while
#    a leaf's last key is below it stops at that neighbour, one leaf left.
#
# The table: three keys of 4,000 rows each, with three position leaves apiece,
# and a fourth of 20,000 rows whose two trees are freed by a DELETE and
# VACUUM.  Each permutation parks the check with the directory's leaves (and
# every tree under them) walked, has an INSERT take those free pages, and
# requires the check to come back clean.  As in verify_concurrent.spec, a
# parked step carries no (*) marker, and every parked check with a marker is
# followed by s3_at_level, which fails if the check never parks there.

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;

	/* Made before the rows, so that the inserts make the trees. */
	CREATE TABLE vt (id int, d tsvector) WITH (autovacuum_enabled = off);
	CREATE INDEX vt_d ON vt USING lion (d) WITH (store_positions = true);
	INSERT INTO vt SELECT i, to_tsvector('simple', 'kept' || (i % 3))
	  FROM generate_series(1, 12000) i;
	INSERT INTO vt SELECT 100000 + i, to_tsvector('simple', 'doomed')
	  FROM generate_series(1, 20000) i;

	/* Returns once the check is parked at `point` (verify_concurrent.spec). */
	CREATE FUNCTION v_parked(point text) RETURNS void LANGUAGE plpgsql AS $f$
	DECLARE
		t0 timestamptz := clock_timestamp();
	BEGIN
		LOOP
			PERFORM pg_stat_clear_snapshot();
			EXIT WHEN EXISTS (SELECT 1 FROM pg_stat_activity
							   WHERE wait_event_type = 'InjectionPoint'
								 AND wait_event = point);
			IF clock_timestamp() - t0 > interval '120 s' THEN
				RAISE EXCEPTION 'the check did not park at %', point;
			END IF;
			PERFORM pg_sleep(0.001);
		END LOOP;
	END $f$;
}

teardown
{
	DROP TABLE vt;
	DROP TABLE IF EXISTS v_free;
	DROP FUNCTION v_parked(text);
	/* Injection points are cluster-wide: never leave one attached. */
	DO $$ BEGIN PERFORM injection_points_detach('lion-verify-dir-level-walked');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

# The checker.
session s1
setup			{ SELECT injection_points_set_local(); }
step s1_park_level	{ SELECT injection_points_attach('lion-verify-dir-level-walked', 'wait'); }
step s1_verify	{ SELECT lion_index_verify('vt_d', true); }
# A step of this session cannot start until the check it parked has ended.
step s1_done	{ SELECT 'checked' AS s1; }

# The writer.
session s2
step s2_free	{ DELETE FROM vt WHERE id > 100000; }
# FREEZE, so that VACUUM waits for a heap page someone else has pinned rather
# than leave its dead rows, and with them the trees (verify_concurrent.spec).
step s2_vacuum	{ VACUUM (FREEZE) vt; }
step s2_xid		{ SELECT pg_current_xact_id() IS NOT NULL AS took_an_xid; }
# The pages a VACUUM deletes go to the free space map only once no snapshot
# can still reach them, which the next VACUUM settles (DESIGN.md §18).
step s2_recycle	{ VACUUM vt; }
step s2_new_key	{
	INSERT INTO vt SELECT 200000 + i, to_tsvector('simple', 'fresh')
	  FROM generate_series(1, 20000) i;
}
step s2_grow_key	{
	INSERT INTO vt SELECT 200000 + i, to_tsvector('simple', 'kept0')
	  FROM generate_series(1, 8000) i;
}

# The observer.
session s3
step s3_note_free	{
	CREATE TABLE v_free AS
	SELECT deleted_pages FROM lion_index_stats('vt_d');
	SELECT deleted_pages > 0 AS vacuum_freed_pages FROM v_free;
}
step s3_at_level	{ SELECT v_parked('lion-verify-dir-level-walked'); }
step s3_wake_level	{
	SELECT injection_points_detach('lion-verify-dir-level-walked');
	SELECT injection_points_wakeup('lion-verify-dir-level-walked');
}
step s3_new_key	{
	SELECT s.deleted_pages < f.deleted_pages AS freed_pages_were_taken,
		   (SELECT count(*) FROM vt WHERE d @@ 'fresh'::tsquery) AS fresh_rows
	  FROM lion_index_stats('vt_d') s, v_free f;
	SELECT lion_index_verify('vt_d', true);
}
step s3_grown_key	{
	SELECT s.deleted_pages < f.deleted_pages AS freed_pages_were_taken,
		   (SELECT count(*) FROM vt WHERE d @@ 'kept0'::tsquery) AS kept0_rows
	  FROM lion_index_stats('vt_d') s, v_free f;
	SELECT lion_index_verify('vt_d', true);
}

permutation
	s2_free
	s2_vacuum					# the fourth key's trees go to the free space map
	s2_xid						# ... behind every snapshot from here on
	s2_recycle					# ... and handed out by the next VACUUM
	s3_note_free
	s1_park_level
	s1_verify(s3_wake_level)	# parks with the leaves walked
	s3_at_level				# ... and it really is parked there
	s2_new_key					# a new key: both roots from the end of the
								# relation, its leaves from the freed pages
	s3_wake_level
	s1_done						# the check has ended
	s3_new_key

permutation
	s2_free
	s2_vacuum
	s2_xid
	s2_recycle
	s3_note_free
	s1_park_level
	s1_verify(s3_wake_level)	# parks with the leaves walked
	s3_at_level				# ... and it really is parked there
	s2_grow_key					# an existing key's position tree splits its
								# last leaf into the freed pages
	s3_wake_level
	s1_done						# the check has ended
	s3_grown_key
