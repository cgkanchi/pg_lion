# heapallindexed's report of a row its key holds without positions
# (DESIGN.md §7 and §17).
#
# In a column that stores positions, heapallindexed looks each row's TID up
# under each of its keys and compares the positions the key stores for it
# with the row's own.  A TID the key holds with no positions at all is a
# report of its own - "has no positions under one of its keys" - and the
# structural check, which runs first, reports every such TID itself
# (verify.sql's seventeenth case); nor can a writer leave one behind it, as
# a row's positions are written before its TID and only VACUUM, which the
# check locks out, takes them away after it (W1, V1).  So the scan meets
# one only when the index was damaged between the two passes, and that is
# what this does: the check parks between them, at lion-verify-heap-scan,
# holding nothing; the index's pages are evicted from shared buffers, and in
# the relation file the first member of 'alpha''s position tree goes one
# offset on, as in verify.sql's seventeenth case, which leaves row (0,1)
# without positions under 'alpha'; and the scan, reading the leaf back from
# the file, must report the row.
#
# The page's checksum is wrong after that, which ignore_checksum_failure
# lets through with a WARNING that client_min_messages hides.

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE EXTENSION IF NOT EXISTS pg_buffercache;

	/*
	 * 'alpha' and 'beta' are CHAIN entries (inline_limit = 64), each with a
	 * position tree of one leaf: one chunk, a 10-byte header and each row's
	 * three bytes - the delta of its code (1), its count of positions (1)
	 * and its position (20 for 'alpha': a position is stored shifted left by
	 * two, over its weight).
	 */
	CREATE TABLE vn (id int, d tsvector) WITH (autovacuum_enabled = off);
	INSERT INTO vn SELECT i, 'alpha:5 beta:6' FROM generate_series(1, 600) i;
	CREATE INDEX vn_d ON vn USING lion (d)
	  WITH (store_positions = true, inline_limit = 64);

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

	/* The check's report, with its DETAIL, which names the key. */
	CREATE FUNCTION v_verify(idx regclass) RETURNS SETOF text
	LANGUAGE plpgsql AS $f$
	DECLARE
		state text;
		msg text;
		detail text;
	BEGIN
		PERFORM lion_index_verify(idx, true);
		RETURN NEXT 'ok';
	EXCEPTION WHEN OTHERS THEN
		GET STACKED DIAGNOSTICS state = RETURNED_SQLSTATE, msg = MESSAGE_TEXT,
								detail = PG_EXCEPTION_DETAIL;
		RETURN NEXT state || ': ' || msg;
		RETURN NEXT detail;
	END $f$;

	/*
	 * The delta of the first member of the position leaf whose first chunk
	 * starts with 'alpha''s first member, at 10 of the chunk, from 1 to 2 in
	 * the relation file: every member moves one offset on, and the leaf - a
	 * page flagged LION_PAGE_POSITIONS (128, the low byte of the flags at
	 * 26 of the special area, the last 32 bytes) at level 0 (24) - holds
	 * nothing for (0,1).  The leaves damaged, which must be one.
	 */
	CREATE FUNCTION vn_damage() RETURNS int LANGUAGE plpgsql AS $f$
	DECLARE
		bs int8 := current_setting('block_size')::int8;
		path text := pg_relation_filepath('vn_d');
		blk int8;
		lp bytea;
		item int8;
		damaged int := 0;
	BEGIN
		FOR blk IN 1 .. pg_relation_size('vn_d') / bs - 1 LOOP
			CONTINUE WHEN get_byte(pg_read_binary_file(path, blk * bs + bs - 32 + 26, 1), 0) & 128 = 0
				OR pg_read_binary_file(path, blk * bs + bs - 32 + 24, 2) <> '\x0000';
			lp := pg_read_binary_file(path, blk * bs + 24, 4);
			item := get_byte(lp, 0) + (get_byte(lp, 1) & 127) * 256;
			CONTINUE WHEN pg_read_binary_file(path, blk * bs + item + 10, 3) <> '\x010114';
			EXECUTE format('COPY (SELECT 1) TO PROGRAM %L',
						   format('cat >/dev/null; printf ''\002'' | dd of=%s bs=1 seek=%s conv=notrunc 2>/dev/null',
								  path, blk * bs + item + 10));
			damaged := damaged + 1;
		END LOOP;
		RETURN damaged;
	END $f$;

	/*
	 * Every buffer of the index evicted, dirty ones written back first:
	 * pg_buffercache_evict() a buffer at a time, which PostgreSQL 17 has
	 * (pg_buffercache_evict_relation() is 18's), and then none left.
	 */
	CREATE FUNCTION vn_evict() RETURNS bool LANGUAGE plpgsql AS $f$
	BEGIN
		PERFORM pg_buffercache_evict(bufferid)
		   FROM pg_buffercache
		  WHERE relfilenode = pg_relation_filenode('vn_d')
			AND reldatabase = (SELECT oid FROM pg_database
								WHERE datname = current_database());
		RETURN NOT EXISTS (SELECT 1 FROM pg_buffercache
							WHERE relfilenode = pg_relation_filenode('vn_d')
							  AND reldatabase = (SELECT oid FROM pg_database
												  WHERE datname = current_database()));
	END $f$;
}

teardown
{
	DROP TABLE vn;
	DROP FUNCTION v_parked(text);
	DROP FUNCTION v_verify(regclass);
	DROP FUNCTION vn_damage();
	DROP FUNCTION vn_evict();
	/* Injection points are cluster-wide: never leave one attached. */
	DO $$ BEGIN PERFORM injection_points_detach('lion-verify-heap-scan');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

# The checker.
session s1
setup		{
	SELECT injection_points_set_local();
	SET ignore_checksum_failure = on;
	SET client_min_messages = error;
}
step s1_park	{ SELECT injection_points_attach('lion-verify-heap-scan', 'wait'); }
step s1_verify	{ SELECT v_verify('vn_d') AS verify; }

# The damage.
session s2
step s2_at_scan	{ SELECT v_parked('lion-verify-heap-scan'); }
# Pages a backend holds in a buffer are read from there, not from the file;
# the eviction writes back any that are dirty before the file is damaged.
step s2_evict	{ SELECT vn_evict() AS all_evicted; }
step s2_damage	{ SELECT vn_damage() AS leaves_damaged; }
step s2_wake	{
	SELECT injection_points_detach('lion-verify-heap-scan');
	SELECT injection_points_wakeup('lion-verify-heap-scan');
}

permutation
	s1_park
	s1_verify(s2_wake)		# the structure is sound: parks before the scan
	s2_at_scan				# ... and it really is parked there
	s2_evict
	s2_damage
	s2_wake					# the scan finds (0,1) without positions
