# A statement's batch of a summarized column's rows (DESIGN.md §55, "A
# column with summaries") beside another writer of the same buckets and
# VACUUM.
#
# The table: t = i for i in 1 .. 4000, summary_tids = 64, so the column's
# buckets are closed up to the open one, which holds the highest keys.  The
# rows with t <= 400 are deleted and committed first.
#
# s1 inserts 2000 rows with t = 4000, the open bucket's key.  They join it
# without raising it, past what it may hold - only a key above it would
# close it - and go in in holds, the INLINE summary spilling to a CHAIN one,
# 256 rows a hold; s1 parks on 'lion-insert-batch-summary' after the first,
# holding nothing.  While it is parked:
#  * s2 inserts rows above every key, and commits: the open bucket is full,
#    so s2 closes it - at 4000, with s1's first rows in it - and opens the
#    next; s1's run now belongs to a closed bucket;
#  * VACUUM empties the buckets of the deleted rows;
#  * s3's counts - a range count, a bitmap scan and a sequential scan -
#    agree, and see none of s1's rows.
# s1 is then released, finds its bucket again, closed now, writes the rest
# of its rows into it and commits; every count then has them, and
# lion_index_verify() - which compares every summary with the keys of its
# bucket - passes.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE TABLE sbr (i int4, t int8) WITH (autovacuum_enabled = off);
	CREATE INDEX sbr_t ON sbr USING lion (t)
		WITH (summaries = on, summary_tids = 64, inline_limit = 256);
	INSERT INTO sbr SELECT i, i FROM generate_series(1, 4000) i;
	ANALYZE sbr;

	/* Detach a point, then release whoever is parked on it until no one is. */
	CREATE FUNCTION sbr_release(point text) RETURNS void
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

	/* The rows in a range of t three ways, and whether they agree. */
	CREATE FUNCTION sbr_counts(lo int8, hi int8) RETURNS text
	LANGUAGE plpgsql AS $fn$
	DECLARE
		pushed int8;
		bitmap int8;
		seq int8;
	BEGIN
		/* every call from the same settings: a step may call it twice */
		PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
		PERFORM set_config('enable_indexscan', 'on', true);
		PERFORM set_config('enable_bitmapscan', 'on', true);
		PERFORM set_config('enable_seqscan', 'off', true);
		PERFORM set_config('pg_lion.pushdown_margin', '1000', true);
		SELECT count(*) INTO pushed FROM sbr WHERE t BETWEEN lo AND hi;
		PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
		PERFORM set_config('enable_indexscan', 'off', true);
		SELECT count(*) INTO bitmap FROM sbr WHERE t BETWEEN lo AND hi;
		PERFORM set_config('enable_seqscan', 'on', true);
		PERFORM set_config('enable_bitmapscan', 'off', true);
		SELECT count(*) INTO seq FROM sbr WHERE t BETWEEN lo AND hi;
		RETURN format('%s %s', seq,
					  CASE WHEN pushed = seq AND bitmap = seq THEN 'agree'
						   ELSE format('DISAGREE pushdown %s bitmap %s', pushed, bitmap) END);
	END $fn$;

	DELETE FROM sbr WHERE t <= 400;
}

teardown
{
	DROP FUNCTION sbr_release(text);
	DROP FUNCTION sbr_counts(int8, int8);
	DROP TABLE sbr;
	/* Injection points are cluster-wide: never leave one attached, and
	 * never fail the teardown because the permutation already did. */
	DO $$ BEGIN PERFORM injection_points_detach('lion-insert-batch-summary');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

# The batch that parks.  Each writer loads the library first: the
# transaction that loads it buffers nothing (lion_defer_init()).
session s1
setup			{ SELECT injection_points_set_local(); LOAD 'pg_lion'; }
step s1_attach	{ SELECT injection_points_attach('lion-insert-batch-summary', 'wait'); }
step s1_insert	{ INSERT INTO sbr SELECT 10000 + i, 4000 FROM generate_series(1, 2000) i; }

# The other writer, which also releases s1.
session s2
setup			{ LOAD 'pg_lion'; }
step s2_insert	{ INSERT INTO sbr SELECT 20000 + i, 4000 + i FROM generate_series(1, 300) i; }
step s2_release	{ SELECT sbr_release('lion-insert-batch-summary'); }

# VACUUM and the reader.
session s3
step s3_vacuum	{ VACUUM sbr; }
step s3_counts	{ SELECT sbr_counts(1, 4000) AS low, sbr_counts(3900, 5000) AS top; }
step s3_verify	{ SELECT lion_index_verify('sbr_t', true); }

permutation s1_attach s1_insert s2_insert s3_vacuum s3_counts s2_release s3_counts s3_verify
