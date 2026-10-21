# Posting-tree splits that never finished, met by the writers that do NOT
# descend, and a root push-down cut short (DESIGN.md §22, the addenda "an
# unfinished split and the writers that do not descend" and "a root
# push-down logs the counters its page holds").
#
# A leaf split is two records: the first splits the page, flags its left half
# LION_PAGE_INCOMPLETE_SPLIT and - when the page was the set's last - makes the
# new right half the entry's `tail`; the second puts the right half's downlink
# into the parent.  A crash between them, or an ERROR (the parent insertion
# may split the parent, which allocates a page, which can run out of disk),
# leaves the flag set and the right half without a downlink.  A writer's
# DESCENT finishes such a split when it meets it, and two writers reach a leaf
# WITHOUT descending:
#
#  A. an APPEND, through the entry's `tail`, which names the right half.  When
#     that fills, its own split finds no downlink for it in the parent, and
#     neither would any later split of the tail, each of which leaves the next
#     tail without a downlink too: an append-only table would never take
#     another row into that key.  So an insert that may split the page takes
#     the descent, which finishes the split on its way.
#  B. VACUUM's REGROW, which walks the right links to the page a container
#     that grew on filtering lives on.  On the right half, which has no
#     downlink, its split looks for the missing downlink to its left and
#     finishes the split it finds there (lion_posting_adopt()), where it would
#     otherwise fail the same way and VACUUM would never complete.
#  C. the same on the flagged LEFT half, which it splits again: the split
#     finishes the page's own unfinished split first, where it would
#     otherwise put a new page between the two halves of the old split, take
#     the downlink the old right half is waiting for and orphan that half -
#     and the containers on it, for every reader that seeks by container key.
#
# And a push-down cut short.  A set of up to 16 leaves has no internal level
# (DESIGN.md §22); the split that would make a 17th leaf first PROMOTES the
# set - pushes its head down under a new root that links every leaf, in a
# record of its own - and then places its member in the next:
#
#  D. when the head is the page that overflows, the head's items go to the new
#     child and the member is placed on that child, which is the child's own
#     split, and can fail.  The push-down logs the entry as its page holds
#     it, with only `tail` moved, and the counters travel with the split that
#     places the items, so an ERROR in between (injection point
#     lion-posting-pushdown-child) leaves the entry's ntids what its
#     containers hold.
#  E. the same in VACUUM's regrow, whose counters are LOWERED by the TIDs
#     filtered out of a container it has not written yet: logged with the
#     push-down, they would be too low by those TIDs, and every later VACUUM
#     would subtract them again.
#  F. when another leaf of the chain overflows - an append to the last one -
#     the promotion leaves that leaf where it is, and its split follows as a
#     split of a tree leaf.  An ERROR in between (lion-posting-promoted)
#     leaves a root that links every leaf and the leaf full, which the next
#     insert splits (test/recovery/run.sh phase 1g does it with a crash).
#
# The splits are cut short with lion-posting-split-incomplete set to 'error'
# inside a subtransaction, which leaves on disk exactly what a crash between
# the two records leaves (test/recovery/run.sh phase 1d does it with a real
# crash).  After every scenario verify() must be clean - no split left
# unfinished, counters equal to the containers - and the index must answer
# what the heap answers.
#
# The fixture of B, C and D: in heap blocks 0..511 (226 rows a block, 64
# blocks a container key), key 1 holds a RUN of five rows in every sixty, so
# each container key is a ~1 KB RUN of 241 runs; key 3 is the row in the
# middle of each gap, and the other rows take 97 keys from 100 on.  Wide rows
# after block 511, keys 1 and 2 on alternate rows, give key 1 a NARROW
# (DESIGN.md §38) a container key, which the build at posting_fillfactor 10
# makes a leaf each: 18 of them in B and C, a tree, and 14 in D.  At the build
# key 1 has its RUN in container key 0 only - key 9 has its rows in keys 1..7
# - so its first leaf takes every container key below 8; key 9's rows then go
# and key 1's take their slots, by the same ids, and their RUNs go onto that
# leaf: seven fill it, with some 900 bytes to spare, and the eighth gets a
# leaf of its own.  In D that is 16 leaves and no internal level, and the head
# is the first leaf, of RUNs.  Deleting key 3's rows in ONE container key's
# heap blocks and vacuuming leaves free line pointers there and nowhere else,
# so the rows inserted next all land in that container key and grow exactly
# the RUN it owns - one new run of one member each - until its leaf splits.
# Deleting the 2nd and 4th row of every run later turns a RUN into an ARRAY
# of some 1,450 bytes, which is what sends VACUUM to regrow.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;

	-- A: wide rows, 65 to a heap page, on 22 container keys, which a build
	-- at posting_fillfactor 10 makes a leaf each: two trees.  The inserts'
	-- NARROWs fill the last leaf, and its splits are aminsert's.
	CREATE TABLE psr_app (id int, k int NOT NULL, pad char(80) NOT NULL DEFAULT 'x')
	  WITH (autovacuum_enabled = off);
	INSERT INTO psr_app SELECT i, i % 2 FROM generate_series(1, 90000) i;
	CREATE INDEX psr_app_k ON psr_app USING lion (k) WITH (posting_fillfactor = 10);

	-- B and C: eight container keys of RUNs, and 18 of wide rows after them.
	CREATE TABLE psr_right (id int, k int NOT NULL, pad text)
	  WITH (autovacuum_enabled = off);
	INSERT INTO psr_right SELECT i,
		CASE WHEN i % 60 < 5 THEN CASE WHEN i <= 14464 THEN 1 ELSE 9 END
			 WHEN i % 60 = 30 THEN 3 ELSE 100 + i % 97 END
	  FROM generate_series(1, 14464 * 8) i;
	INSERT INTO psr_right SELECT i, 1 + i % 2, repeat('x', 70)
	  FROM generate_series(14464 * 8 + 1, 14464 * 8 + 86400) i;
	CREATE INDEX psr_right_k ON psr_right USING lion (k) WITH (posting_fillfactor = 10);
	DELETE FROM psr_right WHERE k = 9;
	CREATE TABLE psr_left (id int, k int NOT NULL, pad text)
	  WITH (autovacuum_enabled = off);
	INSERT INTO psr_left SELECT i,
		CASE WHEN i % 60 < 5 THEN CASE WHEN i <= 14464 THEN 1 ELSE 9 END
			 WHEN i % 60 = 30 THEN 3 ELSE 100 + i % 97 END
	  FROM generate_series(1, 14464 * 8) i;
	INSERT INTO psr_left SELECT i, 1 + i % 2, repeat('x', 70)
	  FROM generate_series(14464 * 8 + 1, 14464 * 8 + 86400) i;
	CREATE INDEX psr_left_k ON psr_left USING lion (k) WITH (posting_fillfactor = 10);
	DELETE FROM psr_left WHERE k = 9;

	-- D: the same with 14 container keys of wide rows: key 1 has 16 leaves
	-- and no internal level, and its head is the first leaf of RUNs.
	CREATE TABLE psr_push (id int, k int NOT NULL, pad text)
	  WITH (autovacuum_enabled = off);
	INSERT INTO psr_push SELECT i,
		CASE WHEN i % 60 < 5 THEN CASE WHEN i <= 14464 THEN 1 ELSE 9 END
			 WHEN i % 60 = 30 THEN 3 ELSE 100 + i % 97 END
	  FROM generate_series(1, 14464 * 8) i;
	INSERT INTO psr_push SELECT i, 1 + i % 2, repeat('x', 70)
	  FROM generate_series(14464 * 8 + 1, 14464 * 8 + 67200) i;
	CREATE INDEX psr_push_k ON psr_push USING lion (k) WITH (posting_fillfactor = 10);
	DELETE FROM psr_push WHERE k = 9;

	-- E: test/isolation/vacuum_regrow_pushdown.spec's fixture.  k = 1's
	-- head holds a RUN in heap blocks 0..63 and the three NARROWs that the
	-- rows of k = 1 put into k = 3's slots after the build, and 15 more
	-- leaves follow it.
	CREATE TABLE psr_vpush (id int, k int NOT NULL, pad text)
	  WITH (autovacuum_enabled = off);
	INSERT INTO psr_vpush SELECT i, 1 FROM generate_series(1, 14464) i;
	INSERT INTO psr_vpush SELECT i, 2 + i % 2 FROM generate_series(14465, 57856) i;
	INSERT INTO psr_vpush SELECT i, 1 + i % 2, repeat('x', 70)
	  FROM generate_series(57857, 129856) i;
	CREATE INDEX psr_vpush_k ON psr_vpush USING lion (k)
		WITH (inline_limit = 64, posting_fillfactor = 10);
	DELETE FROM psr_vpush WHERE k = 3;

	-- F: wide rows on 16 container keys, which a build at posting_fillfactor
	-- 10 makes 16 leaves of each key.
	CREATE TABLE psr_prom (id int, k int NOT NULL, pad char(80) NOT NULL DEFAULT 'x')
	  WITH (autovacuum_enabled = off);
	INSERT INTO psr_prom SELECT i, i % 2 FROM generate_series(1, 66000) i;
	CREATE INDEX psr_prom_k ON psr_prom USING lion (k) WITH (posting_fillfactor = 10);

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

# The slots key 9 (key 3 in E) left, which the rows of key 1 then take, by
# the ids key 9 had.  VACUUM runs outside the transaction the first setup
# block is.
setup { VACUUM psr_right; }
setup { VACUUM psr_left; }
setup { VACUUM psr_push; }
setup { VACUUM psr_vpush; }
setup
{
	INSERT INTO psr_right SELECT i, 1 FROM generate_series(14465, 14464 * 8) i WHERE i % 60 < 5;
	INSERT INTO psr_left SELECT i, 1 FROM generate_series(14465, 14464 * 8) i WHERE i % 60 < 5;
	INSERT INTO psr_push SELECT i, 1 FROM generate_series(14465, 14464 * 8) i WHERE i % 60 < 5;
	INSERT INTO psr_vpush SELECT 300000 + i, 1 FROM generate_series(1, 21696) i;
}

teardown
{
	DROP TABLE psr_app, psr_right, psr_left, psr_push, psr_vpush, psr_prom;
	DROP FUNCTION psr_insert_until_error(text);
	DROP FUNCTION psr_landed(text);
	DROP FUNCTION psr_counts(text, int);
	/* Never leave an injection point attached, whatever the permutation did. */
	DO $$ BEGIN PERFORM injection_points_detach('lion-posting-split-incomplete');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
	DO $$ BEGIN PERFORM injection_points_detach('lion-posting-pushdown-child');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
	DO $$ BEGIN PERFORM injection_points_detach('lion-posting-promoted');
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
step push_breaks	{ SELECT injection_points_attach('lion-posting-pushdown-child', 'error'); }
step push_works		{ SELECT injection_points_detach('lion-posting-pushdown-child'); }
step promote_breaks	{ SELECT injection_points_attach('lion-posting-promoted', 'error'); }
step promote_works	{ SELECT injection_points_detach('lion-posting-promoted'); }

# A. Appends after an unfinished split.
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
# RUNs of keys 1-6 onto the new page, and their growth overflows THAT page.
step b_holes	{ DELETE FROM psr_right WHERE k = 3 AND id <= 14464; }
step b_vacuum_holes	{ VACUUM (INDEX_CLEANUP ON) psr_right; }
step b_before	{ SELECT container_pages FROM lion_index_stats('psr_right_k'); }
step b_cut		{ SELECT psr_insert_until_error('psr_right'); }
step b_after_cut {
	SELECT * FROM psr_landed('psr_right');
	SELECT container_pages FROM lion_index_stats('psr_right_k');
}
step b_delete	{
	DELETE FROM psr_right WHERE k = 1 AND id % 60 IN (1, 3)
	   AND id BETWEEN 14464 + 1 AND 14464 * 7;
}
step b_vacuum	{ VACUUM (INDEX_CLEANUP ON) psr_right; }
step b_check	{
	SELECT container_pages FROM lion_index_stats('psr_right_k');
	SELECT lion_index_verify('psr_right_k', true);
	SELECT * FROM psr_counts('psr_right', 1);
	SELECT * FROM psr_counts('psr_right', 100);
}

# C. VACUUM's regrow on the flagged LEFT half.  The rows go to container key
# 6, the LAST RUN of the first leaf, so the split moves nothing right: the
# grown RUN gets a page of its own and the leaf keeps the RUNs of keys 0-5,
# whose growth on filtering then overflows it.
step c_holes	{ DELETE FROM psr_left WHERE k = 3 AND id BETWEEN 14464 * 6 + 1 AND 14464 * 7; }
step c_vacuum_holes	{ VACUUM (INDEX_CLEANUP ON) psr_left; }
step c_before	{ SELECT container_pages FROM lion_index_stats('psr_left_k'); }
step c_cut		{ SELECT psr_insert_until_error('psr_left'); }
step c_after_cut {
	SELECT * FROM psr_landed('psr_left');
	SELECT container_pages FROM lion_index_stats('psr_left_k');
}
step c_delete	{ DELETE FROM psr_left WHERE k = 1 AND id % 60 IN (1, 3) AND id <= 14464 * 6; }
step c_vacuum	{ VACUUM (INDEX_CLEANUP ON) psr_left; }
step c_check	{
	SELECT container_pages FROM lion_index_stats('psr_left_k');
	SELECT lion_index_verify('psr_left_k', true);
	SELECT * FROM psr_counts('psr_left', 1);
	SELECT * FROM psr_counts('psr_left', 100);
}

# D. An insert's push-down of the head cut short.  The rows go to container
# key 0, the first RUN of the head, until the head splits - a 17th leaf.
# Right after it the head is the root, over a new child that holds what the
# head held and the 15 other leaves, and the entry must count exactly that.
step d_holes	{ DELETE FROM psr_push WHERE k = 3 AND id <= 14464; }
step d_vacuum_holes	{ VACUUM (INDEX_CLEANUP ON) psr_push; }
step d_cut		{ SELECT psr_insert_until_error('psr_push'); }
step d_after_cut {
	SELECT * FROM psr_landed('psr_push');
	SELECT posting_internal_pages FROM lion_index_stats('psr_push_k');
	SELECT lion_index_verify('psr_push_k');
}
step d_more		{ INSERT INTO psr_push SELECT 2000000 + i, 1 FROM generate_series(1, 200) i; }
step d_check	{
	SELECT lion_index_verify('psr_push_k', true);
	SELECT * FROM psr_counts('psr_push', 1);
	SELECT * FROM psr_counts('psr_push', 3);
}

# E. VACUUM's regrow push-down cut short: the child still holds the RUN with
# its dead TIDs, and the entry must still count them.  The next VACUUM
# removes them, and ntids then equals the heap's rows.
step e_delete	{ DELETE FROM psr_vpush WHERE k = 1 AND id <= 14464 AND id % 2 = 0; }
step e_cut		{ VACUUM (INDEX_CLEANUP ON) psr_vpush; }
step e_after_cut {
	SELECT posting_internal_pages FROM lion_index_stats('psr_vpush_k');
	SELECT lion_index_verify('psr_vpush_k');
}
step e_vacuum	{ VACUUM (INDEX_CLEANUP ON) psr_vpush; }
step e_check	{
	SELECT lion_index_verify('psr_vpush_k', true);
	SELECT ntids = (SELECT count(*) FROM psr_vpush) AS ntids_agrees
	  FROM lion_index_stats('psr_vpush_k');
	SELECT * FROM psr_counts('psr_vpush', 1);
}

# F. A promotion cut short before the leaf that needed it is split.  Right
# after it the head is a root over all 16 leaves, the last of them is full,
# and nothing is flagged: the next insert splits that leaf as a tree's.
step f_cut		{ INSERT INTO psr_prom SELECT 100000 + i, i % 2 FROM generate_series(1, 30000) i; }
step f_after_cut {
	SELECT posting_internal_pages FROM lion_index_stats('psr_prom_k');
	SELECT lion_index_verify('psr_prom_k');
}
step f_more		{ INSERT INTO psr_prom SELECT 200000 + i, i % 2 FROM generate_series(1, 20000) i; }
step f_check	{
	SELECT lion_index_verify('psr_prom_k', true);
	SELECT * FROM psr_counts('psr_prom', 0);
	SELECT * FROM psr_counts('psr_prom', 1);
}

permutation
	split_breaks a_cut split_works a_append a_check
	b_holes b_vacuum_holes b_before split_breaks b_cut split_works b_after_cut
	b_delete b_vacuum b_check
	c_holes c_vacuum_holes c_before split_breaks c_cut split_works c_after_cut
	c_delete c_vacuum c_check
	d_holes d_vacuum_holes push_breaks d_cut push_works d_after_cut d_more d_check
	e_delete push_breaks e_cut push_works e_after_cut e_vacuum e_check
	promote_breaks f_cut promote_works f_after_cut f_more f_check
