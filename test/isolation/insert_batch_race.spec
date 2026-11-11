# A statement's batch of inserts (DESIGN.md §55) beside other writers of the
# same key, VACUUM and readers.
#
# The table: 20000 rows, k = i % 2, so both keys are CHAIN entries on the one
# directory leaf.  The rows with i <= 4000 are deleted and committed first.
#
# s1 inserts 2000 rows with k = 1.  Its batch goes into the key's posting set
# in holds of the leaf, LION_APPLY_HOLD rows at a time, and s1 parks on
# 'lion-insert-batch-hold' after the first, holding nothing.  While it is
# parked:
#  * s2 inserts 500 rows into each key, a batch of its own, and commits: the
#    leaf is free between s1's holds;
#  * VACUUM removes the deleted rows - from the posting set s1 is halfway
#    through - which s1's snapshot cannot see, and s2 inserts again, into the
#    space VACUUM freed;
#  * s3's counts - the count pushdown, a bitmap scan and a sequential scan -
#    agree, and see none of s1's rows.
# s1 is then released, writes the rest of its batch and commits; every count
# then has its 2000 rows, and lion_index_verify() with the heap check passes.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE TABLE ibr (i int4, k int4) WITH (autovacuum_enabled = off);
	INSERT INTO ibr SELECT i, i % 2 FROM generate_series(1, 20000) i;
	CREATE INDEX ibr_k ON ibr USING lion (k);
	ANALYZE ibr;

	/* Detach a point, then release whoever is parked on it until no one is. */
	CREATE FUNCTION ibr_release(point text) RETURNS void
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

	/* The counts of k = 1 three ways, and whether they agree. */
	CREATE FUNCTION ibr_counts() RETURNS text
	LANGUAGE plpgsql AS $fn$
	DECLARE
		pushed int8;
		bitmap int8;
		seq int8;
	BEGIN
		PERFORM set_config('enable_seqscan', 'off', true);
		PERFORM set_config('pg_lion.pushdown_margin', '1000', true);
		SELECT count(*) INTO pushed FROM ibr WHERE k = 1;
		PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
		PERFORM set_config('enable_indexscan', 'off', true);
		SELECT count(*) INTO bitmap FROM ibr WHERE k = 1;
		PERFORM set_config('enable_seqscan', 'on', true);
		PERFORM set_config('enable_bitmapscan', 'off', true);
		SELECT count(*) INTO seq FROM ibr WHERE k = 1;
		RETURN format('%s %s', seq,
					  CASE WHEN pushed = seq AND bitmap = seq THEN 'agree'
						   ELSE format('DISAGREE pushdown %s bitmap %s', pushed, bitmap) END);
	END $fn$;

	DELETE FROM ibr WHERE i <= 4000;
}

teardown
{
	DROP FUNCTION ibr_release(text);
	DROP FUNCTION ibr_counts();
	DROP TABLE ibr;
	/* Injection points are cluster-wide: never leave one attached, and
	 * never fail the teardown because the permutation already did. */
	DO $$ BEGIN PERFORM injection_points_detach('lion-insert-batch-hold');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

# The batch that parks.  Each writer loads the library first: the
# transaction that loads it buffers nothing (lion_defer_init()).
session s1
setup			{ SELECT injection_points_set_local(); LOAD 'pg_lion'; }
step s1_attach	{ SELECT injection_points_attach('lion-insert-batch-hold', 'wait'); }
step s1_insert	{ INSERT INTO ibr SELECT i, 1 FROM generate_series(100001, 102000) i; }

# The other writer, which also releases s1.
session s2
setup			{ LOAD 'pg_lion'; }
step s2_insert	{
	INSERT INTO ibr SELECT 200000 + i, i % 2 FROM generate_series(1, 1000) i;
}
step s2_insert2	{ INSERT INTO ibr SELECT i, 1 FROM generate_series(300001, 300300) i; }
step s2_release	{ SELECT ibr_release('lion-insert-batch-hold'); }

# VACUUM and the reader.
session s3
step s3_vacuum	{ VACUUM ibr; }
step s3_counts	{ SELECT ibr_counts() AS k1; }
step s3_verify	{ SELECT lion_index_verify('ibr_k', true); }

permutation s1_attach s1_insert s2_insert s3_vacuum s2_insert2 s3_counts s2_release s3_counts s3_verify
