# Posting-tree splits that never finished, met by the writers that do NOT
# descend (DESIGN.md §22, addendum of 2026-09-25).
#
# A leaf split is two records: the first splits the page, flags its left half
# LION_PAGE_INCOMPLETE_SPLIT and - when the page was the set's last - makes the
# new right half the entry's `tail`; the second puts the right half's downlink
# into the parent.  A crash between them, or an ERROR (the parent insertion
# may split the parent, which allocates a page, which can run out of disk),
# leaves the flag set and the right half without a downlink.  A writer's
# DESCENT finishes such a split when it meets it, and that used to be the only
# repair - but two writers reach a leaf WITHOUT descending:
#
#  A. an APPEND, through the entry's `tail`.  Every later append went to the
#     right half; when that filled, its own split looked for its downlink in
#     the parent, found none, and failed with "no downlink for block N" - and
#     so did every later split of the tail, because each one left the next
#     tail without a downlink too.  An append-only table never took another
#     row into that key.
#  B. VACUUM's REGROW, which walks the right links to the page a container
#     that grew on filtering lives on.  On the right half, which has no
#     downlink, its split failed the same way, and VACUUM never completed.
#
# The splits are cut short with lion-posting-split-incomplete set to 'error'
# inside a subtransaction, which leaves on disk exactly what a crash between
# the two records leaves (test/recovery/run.sh phase 1d does it with a real
# crash).  After every scenario verify() must be clean - no split left
# unfinished - and the index must answer what the heap answers.
#
# The fixture of B: key 1 holds a RUN of five rows in every thirty, so each
# container key (64 heap blocks of 226 rows) is a ~1980-byte RUN and a
# bulk-built leaf holds four of them with 208 bytes to spare; key 3 is the row
# in the middle of each gap.  Deleting key 3's rows in ONE container key's heap
# blocks and vacuuming leaves free line pointers there and nowhere else, so
# the rows inserted next all land in that container key and grow exactly the
# RUN it owns - one new run of one member each - until its leaf splits.
# Deleting the 2nd and 4th row of every run later turns a RUN into an ARRAY
# half again as large (~2970 bytes), which is what sends VACUUM to regrow.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;

	-- A: created empty, so every split comes from aminsert.
	CREATE TABLE psr_app (id int, k int NOT NULL) WITH (autovacuum_enabled = off);
	CREATE INDEX psr_app_k ON psr_app USING lion (k);

	-- B: eight container keys, two leaves of four RUNs for key 1.
	CREATE TABLE psr_right (id int, k int NOT NULL) WITH (autovacuum_enabled = off);
	INSERT INTO psr_right SELECT i,
		CASE WHEN i % 30 < 5 THEN 1 WHEN i % 30 = 15 THEN 3 ELSE 2 END
	  FROM generate_series(1, 14464 * 8) i;
	CREATE INDEX psr_right_k ON psr_right USING lion (k);

	/*
	 * Insert key-1 rows one at a time, each in a subtransaction, until one of
	 * them fails - which, with an injection point set to 'error', is the one
	 * whose split it interrupted.  The rows before it commit.
	 */
	CREATE FUNCTION psr_insert_until_error(tbl text) RETURNS text
	LANGUAGE plpgsql AS $fn$
	BEGIN
		FOR n IN 1 .. 5000 LOOP
			BEGIN
				EXECUTE format('INSERT INTO %I VALUES (%s, 1)', tbl, 1000000 + n);
			EXCEPTION WHEN OTHERS THEN
				RETURN SQLERRM;
			END;
		END LOOP;
		RETURN 'never interrupted';
	END $fn$;

	/* Which container keys (64 heap blocks each) the inserted rows landed in. */
	CREATE FUNCTION psr_landed(tbl text, OUT lo int, OUT hi int)
	LANGUAGE plpgsql AS $fn$
	BEGIN
		EXECUTE format('SELECT min(floor((ctid::text::point)[0] / 64)),
							   max(floor((ctid::text::point)[0] / 64))
						  FROM %I WHERE id > 1000000', tbl) INTO lo, hi;
	END $fn$;

	/*
	 * One key's rows counted through the index (whichever way the planner
	 * reads it: the count pushdown, a bitmap scan or an index scan) and
	 * through the heap alone.  Every knob is set for each query, because the
	 * settings are transaction-local and a step is one transaction.
	 */
	CREATE FUNCTION psr_counts(tbl text, key int,
							   OUT via_index bigint, OUT via_heap bigint)
	LANGUAGE plpgsql AS $fn$
	DECLARE
		q text := format('SELECT count(*) FROM %I WHERE k = %s', tbl, key);
	BEGIN
		PERFORM set_config('enable_seqscan', 'off', true);
		PERFORM set_config('enable_bitmapscan', 'on', true);
		PERFORM set_config('enable_indexscan', 'on', true);
		PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
		EXECUTE q INTO via_index;
		PERFORM set_config('enable_seqscan', 'on', true);
		PERFORM set_config('enable_bitmapscan', 'off', true);
		PERFORM set_config('enable_indexscan', 'off', true);
		PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
		EXECUTE q INTO via_heap;
		PERFORM set_config('enable_bitmapscan', 'on', true);
		PERFORM set_config('enable_indexscan', 'on', true);
		PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	END $fn$;
}

teardown
{
	DROP TABLE psr_app, psr_right;
	DROP FUNCTION psr_insert_until_error(text);
	DROP FUNCTION psr_landed(text);
	DROP FUNCTION psr_counts(text, int);
	/* Never leave an injection point attached, whatever the permutation did. */
	DO $$ BEGIN PERFORM injection_points_detach('lion-posting-split-incomplete');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

session s1
setup
{
	SET synchronous_commit = on;
	SELECT injection_points_set_local();
}

step split_breaks	{ SELECT injection_points_attach('lion-posting-split-incomplete', 'error'); }
step split_works	{ SELECT injection_points_detach('lion-posting-split-incomplete'); }

# A. Appends after an unfinished split.
step a_fill		{ INSERT INTO psr_app SELECT i, i % 2 FROM generate_series(1, 60000) i; }
step a_cut		{ INSERT INTO psr_app SELECT 100000 + i, i % 2 FROM generate_series(1, 30000) i; }
step a_append	{
	INSERT INTO psr_app SELECT 200000 + i, i % 2 FROM generate_series(1, 20000) i;
	INSERT INTO psr_app SELECT 300000 + i, i % 2 FROM generate_series(1, 20000) i;
}
step a_check	{
	SELECT lion_index_verify('psr_app_k', true);
	SELECT * FROM psr_counts('psr_app', 0);
	SELECT * FROM psr_counts('psr_app', 1);
}

# B. VACUUM's regrow on the RIGHT half, which has no downlink.  The rows go to
# container key 0, the FIRST RUN of the first leaf, so the split moves the
# RUNs of keys 1-3 onto the new page, and their growth overflows THAT page.
step b_holes	{ DELETE FROM psr_right WHERE k = 3 AND id <= 14464; }
step b_vacuum_holes	{ VACUUM (INDEX_CLEANUP ON) psr_right; }
step b_before	{ SELECT container_pages FROM lion_index_stats('psr_right_k'); }
step b_cut		{ SELECT psr_insert_until_error('psr_right'); }
step b_after_cut {
	SELECT * FROM psr_landed('psr_right');
	SELECT container_pages FROM lion_index_stats('psr_right_k');
}
step b_delete	{
	DELETE FROM psr_right WHERE k = 1 AND id % 30 IN (1, 3)
	   AND id BETWEEN 14464 + 1 AND 14464 * 4;
}
step b_vacuum	{ VACUUM (INDEX_CLEANUP ON) psr_right; }
step b_check	{
	SELECT container_pages FROM lion_index_stats('psr_right_k');
	SELECT lion_index_verify('psr_right_k', true);
	SELECT * FROM psr_counts('psr_right', 1);
	SELECT * FROM psr_counts('psr_right', 2);
}

permutation
	a_fill split_breaks a_cut split_works a_append a_check
	b_holes b_vacuum_holes b_before split_breaks b_cut split_works b_after_cut
	b_delete b_vacuum b_check
