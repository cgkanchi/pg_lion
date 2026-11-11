# Inserts that skip the directory descent (DESIGN.md §21, "A leaf already
# known") beside the other sessions that change the directory under them.
#
# s1 remembers the leaf it last wrote each key on, and goes there directly
# the next time; and a batch of its rows (§55) finds each key from the leaf
# the key before it ended on.  Both are checked against what the leaf is
# by the time s1 comes back to it, under its lock: s2 splits the leaf so
# that the key is no longer on it, or VACUUM takes the leaf out of the tree.
# Each insert of s1 reports the directory pages it read: one when the leaf
# it remembered served, that one and a descent (four levels here) when it
# did not.
#
# The keys are 600 bytes long, so a directory leaf holds eleven of them,
# and 2,000 of them make a directory of four levels (the fixture of
# dir_page_delete.spec).  The rows of the first 400 keys are deleted, so
# that VACUUM empties the leaves they were on, first to last; the
# injection point it parks on is attached locally with the 'error' action,
# so that VACUUM stops there with the leaf HALF_DEAD or DELETED and sends
# no invalidation, which would have s1 forget every leaf.
#
#  * moved: s1 writes key 1500 twice, s2 puts twelve keys just below it,
#    splitting its leaf so that it moves right, and s1 writes it again.
#  * halfdead, unlinked: s1 writes key 5 (in a transaction it rolls back,
#    so that its leaf still empties), VACUUM stops with that leaf HALF_DEAD
#    or DELETED, and s1 writes key 5 again.
#  * batch: s1's batch writes key 1500 (by a descent), then key 1505, from
#    the leaf 1500 ended on; it parks between the two, s2 splits that leaf
#    so that 1505 moves right, and s1 is released: it reads the leaf, finds
#    the high key below 1505 now, and descends.  Without the split, 1505 is
#    found on that leaf with no descent.
#
# Every key is then counted by a bitmap scan and a sequential scan, and
# lion_index_verify() with the heap check passes.

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE FUNCTION lion_debug_dir_pages_read() RETURNS int8
		AS 'pg_lion', 'lion_debug_dir_pages_read' LANGUAGE C STRICT;
	CREATE FUNCTION lhs_reads(q text) RETURNS int8 LANGUAGE plpgsql AS $fn$
	DECLARE
		r0 int8 := lion_debug_dir_pages_read();
	BEGIN
		EXECUTE q;
		RETURN lion_debug_dir_pages_read() - r0;
	END $fn$;
	CREATE FUNCTION lhs_key(i int) RETURNS text
	LANGUAGE sql IMMUTABLE AS $$ SELECT 'k' || lpad(i::text, 599, '0') $$;
	CREATE TABLE lhs (id int, k text) WITH (autovacuum_enabled = off);
	ALTER TABLE lhs ALTER k SET STORAGE PLAIN;
	INSERT INTO lhs SELECT i, lhs_key(i) FROM generate_series(1, 2000) i;
	CREATE INDEX lhs_k ON lhs USING lion (k);
	DELETE FROM lhs WHERE id <= 400;

	/* Detach a point, then release whoever is parked on it until no one is. */
	CREATE FUNCTION lhs_release(point text) RETURNS void
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

	/* Every key's rows by a bitmap scan and a sequential scan, and whether
	 * they agree. */
	CREATE FUNCTION lhs_counts() RETURNS text
	LANGUAGE plpgsql AS $fn$
	DECLARE
		bitmap text;
		seq text;
	BEGIN
		PERFORM set_config('enable_seqscan', 'off', true);
		PERFORM set_config('enable_indexscan', 'off', true);
		PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
		SELECT string_agg(format('%s:%s', right(k, 4), n), ',' ORDER BY k) INTO bitmap
		  FROM (SELECT k, (SELECT count(*) FROM lhs WHERE lhs.k = d.k) AS n
				  FROM (SELECT DISTINCT k FROM lhs) d) t;
		PERFORM set_config('enable_seqscan', 'on', true);
		PERFORM set_config('enable_bitmapscan', 'off', true);
		SELECT string_agg(format('%s:%s', right(k, 4), n), ',' ORDER BY k) INTO seq
		  FROM (SELECT k, count(*) AS n FROM lhs GROUP BY k) t;
		RETURN CASE WHEN bitmap = seq THEN 'agree' ELSE 'DISAGREE' END;
	END $fn$;
}

teardown
{
	DROP TABLE lhs;
	DROP FUNCTION lhs_key(int);
	DROP FUNCTION lhs_reads(text);
	DROP FUNCTION lhs_release(text);
	DROP FUNCTION lhs_counts();
	DROP FUNCTION lion_debug_dir_pages_read();
	DO $$ BEGIN PERFORM injection_points_detach('lion-insert-batch-hold');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

# The writer that remembers leaves.  It loads the library first: the
# transaction that loads it buffers nothing (lion_defer_init()).
session s1
setup			{ SELECT injection_points_set_local(); LOAD 'pg_lion'; }
step s1_1500	{
	SELECT lhs_reads($$INSERT INTO lhs VALUES (-1, lhs_key(1500))$$) AS first,
		   lhs_reads($$INSERT INTO lhs VALUES (-2, lhs_key(1500))$$) AS next;
}
step s1_1500_again	{
	SELECT lhs_reads($$INSERT INTO lhs VALUES (-3, lhs_key(1500))$$) AS moved,
		   lhs_reads($$INSERT INTO lhs VALUES (-4, lhs_key(1500))$$) AS next;
}
step s1_5		{
	BEGIN;
	SELECT lhs_reads($$INSERT INTO lhs VALUES (-5, lhs_key(5))$$) AS first,
		   lhs_reads($$INSERT INTO lhs VALUES (-6, lhs_key(5))$$) AS next;
	ROLLBACK;
}
step s1_5_again	{
	SELECT lhs_reads($$INSERT INTO lhs VALUES (-7, lhs_key(5))$$) AS gone,
		   lhs_reads($$INSERT INTO lhs VALUES (-8, lhs_key(5))$$) AS next;
}
step s1_attach	{ SELECT injection_points_attach('lion-insert-batch-hold', 'wait'); }
# the first row goes in at once; the other two are the batch, one hold a key
step s1_batch	{
	SELECT lhs_reads($$INSERT INTO lhs VALUES (-9, lhs_key(1800)),
							(-10, lhs_key(1500)), (-11, lhs_key(1505))$$) AS batch;
}

# The other writer, and VACUUM.
session s2
setup
{
	SET client_min_messages = error;
	SELECT injection_points_set_local();
}
step s2_split_1500	{
	INSERT INTO lhs SELECT -100 - i, lhs_key(1499) || chr(97 + i)
	  FROM generate_series(0, 11) i;
}
step s2_split_1505	{
	INSERT INTO lhs SELECT -200 - i, lhs_key(1500) || chr(97 + i)
	  FROM generate_series(0, 11) i;
}
step s2_halfdead	{ SELECT injection_points_attach('lion-dir-delete-halfdead', 'error'); }
step s2_unlinked	{ SELECT injection_points_attach('lion-dir-delete-unlinked', 'error'); }
step s2_vacuum		{ VACUUM lhs; }
step s2_detach_halfdead	{ SELECT injection_points_detach('lion-dir-delete-halfdead'); }
step s2_detach_unlinked	{ SELECT injection_points_detach('lion-dir-delete-unlinked'); }
step s2_release		{ SELECT lhs_release('lion-insert-batch-hold'); }

# The checks.
session s3
step s3_shape	{ SELECT deleted_pages > 0 AS some_deleted FROM lion_index_stats('lhs_k'); }
step s3_counts	{ SELECT lhs_counts() AS counts; }
step s3_verify	{ SELECT lion_index_verify('lhs_k', true) AS verify; }

permutation s1_1500 s2_split_1500 s1_1500_again s3_counts s3_verify
permutation s1_5 s2_halfdead s2_vacuum s2_detach_halfdead s3_shape s1_5_again s3_counts s3_verify
permutation s1_5 s2_unlinked s2_vacuum s2_detach_unlinked s3_shape s1_5_again s3_counts s3_verify
permutation s1_attach s1_batch s2_release s3_counts s3_verify
permutation s1_attach s1_batch s2_split_1505 s2_release s3_counts s3_verify
