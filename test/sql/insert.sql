-- Inserts into a lion index (DESIGN.md section 5, INSERT).
--
-- Every phase compares index scans with sequential scans and runs
-- lion_index_verify(idx, true), which also checks that every heap tuple
-- visible to a fresh snapshot is indexed under its key.
\set VERBOSITY terse
SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS pg_lion;

SELECT setseed(0.42);

CREATE OR REPLACE FUNCTION lion_cmp(tbl text, pred text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	q text := format('SELECT count(*) AS c, coalesce(sum(i), 0) AS s FROM %s WHERE %s', tbl, pred);
	a record;
	b record;
	ln text;
	used boolean := false;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', false);
	PERFORM set_config('enable_bitmapscan', 'on', false);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Bitmap Index Scan%' THEN
			used := true;
		END IF;
	END LOOP;
	EXECUTE q INTO a;

	PERFORM set_config('enable_seqscan', 'on', false);
	PERFORM set_config('enable_bitmapscan', 'off', false);
	EXECUTE q INTO b;
	PERFORM set_config('enable_bitmapscan', 'on', false);

	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH index=%s seqscan=%s', a::text, b::text);
	END IF;
	IF NOT used THEN
		RETURN format('NO BITMAP INDEX SCAN (%s rows)', a.c);
	END IF;
	RETURN format('ok %s rows', a.c);
END $$;

-- The index is built on an empty table, so every TID below arrives through
-- aminsert.
CREATE TABLE lion_ins (i int4, k int4, t text, b bool);
CREATE INDEX lion_ins_k ON lion_ins USING lion (k);
CREATE INDEX lion_ins_t ON lion_ins USING lion (t);
CREATE INDEX lion_ins_b ON lion_ins USING lion (b);

CREATE OR REPLACE FUNCTION lion_ins_check(tag text) RETURNS text
LANGUAGE plpgsql AS $$
BEGIN
	PERFORM lion_index_verify('lion_ins_k', true);
	PERFORM lion_index_verify('lion_ins_t', true);
	PERFORM lion_index_verify('lion_ins_b', true);
	RETURN format('%s | k=3 %s | t=v3 %s | t=v7 %s | b %s | k=-1 %s', tag,
				  lion_cmp('lion_ins', 'k = 3'),
				  lion_cmp('lion_ins', 't = ''v3'''),
				  lion_cmp('lion_ins', 't = ''v7'''),
				  lion_cmp('lion_ins', 'b'),
				  lion_cmp('lion_ins', 'k = -1'));
END $$;

SELECT lion_ins_check('empty');

-- Phase 1: single-row inserts, each its own statement.
INSERT INTO lion_ins VALUES (1, 1, 'v1', true);
INSERT INTO lion_ins VALUES (2, 2, 'v2', false);
INSERT INTO lion_ins VALUES (3, 3, 'v3', true);
INSERT INTO lion_ins VALUES (4, 3, 'v3', false);
INSERT INTO lion_ins VALUES (5, 7, 'v7', true);
SELECT lion_ins_check('single rows');
SELECT * FROM lion_index_stats('lion_ins_k');

-- Phase 2: multi-row VALUES.
INSERT INTO lion_ins VALUES
	(6, 3, 'v3', false), (7, 7, 'v7', true), (8, 11, 'v11', false),
	(9, 3, 'v7', true), (10, 7, 'v3', false);
SELECT lion_ins_check('multi-row values');

-- Phase 3: a bulk INSERT ... SELECT.  100k rows over 13 int keys, 7 text
-- keys and 2 booleans.
INSERT INTO lion_ins
SELECT i, i % 13, 'v' || (i % 7), (i % 3) = 0
  FROM generate_series(11, 100010) i;
SELECT lion_ins_check('insert select 100k');
SELECT entries, inline_entries, ntids FROM lion_index_stats('lion_ins_k');
SELECT entries, inline_entries, ntids FROM lion_index_stats('lion_ins_t');
SELECT container_pages > 0 AS has_container_pages,
	   containers > entries AS several_containers_per_key
  FROM lion_index_stats('lion_ins_k');

-- Phase 4: UPDATE of an indexed column.  The new tuple cannot be HOT, so it
-- gets a fresh TID in every index; the old TIDs stay until VACUUM.
UPDATE lion_ins SET k = 5 WHERE i BETWEEN 100 AND 5000;
SELECT lion_ins_check('update indexed column');
SELECT lion_cmp('lion_ins', 'k = 5');

-- An update of a non-indexed column is HOT and leaves the index alone.
UPDATE lion_ins SET i = i WHERE i BETWEEN 200 AND 300;
SELECT lion_ins_check('hot update');

-- Phase 5: DELETE without VACUUM.  The index still points at the dead rows;
-- the bitmap heap scan has to filter them.
DELETE FROM lion_ins WHERE i % 7 = 0;
SELECT lion_ins_check('delete without vacuum');

-- Phase 6: a key that appears for the first time long after the build.
INSERT INTO lion_ins VALUES (200001, 424242, 'brand new key', true);
SELECT lion_ins_check('brand new key');
SELECT lion_cmp('lion_ins', 'k = 424242');
SELECT lion_cmp('lion_ins', 't = ''brand new key''');

-- Phase 7: cross-type insert values and cross-type scan keys.
INSERT INTO lion_ins VALUES (200002, 3::int8, 'v7', true);
INSERT INTO lion_ins VALUES (200003, 3::int2, 'v7'::varchar, false);
INSERT INTO lion_ins VALUES (200004, 3::numeric, 'v7', true);
SELECT lion_ins_check('cross-type values');
SELECT lion_cmp('lion_ins', 'k = 3::int8');
SELECT lion_cmp('lion_ins', 'k = 3::int2');

-- NULL keys live in a reserved entry that sorts before every value, and must
-- not upset the entry next to it.
INSERT INTO lion_ins VALUES (200005, NULL, NULL, NULL);
SELECT lion_ins_check('null keys');
SELECT count(*) FROM lion_ins WHERE k IS NULL;

/*
 * An INLINE entry that spills.  inline_limit = 64 bytes holds four one-pair
 * sparse segments, so the first inserts of a key stay inside the entry tuple
 * and a later one moves the posting set onto container pages.
 */
CREATE TABLE lion_spill (i int4, k int4);
CREATE INDEX lion_spill_k ON lion_spill USING lion (k)
	WITH (inline_limit = 64);
INSERT INTO lion_spill VALUES (1, 1);
-- the first TID of a key is one pair in one sparse segment (DESIGN.md 13)
SELECT entries, inline_entries, containers, sparse_segments, sparse_members,
	   ntids
  FROM lion_index_stats('lion_spill_k');
INSERT INTO lion_spill SELECT i, 1 FROM generate_series(2, 200) i;
/*
 * These 200 rows are consecutive, so they share one container key: the
 * segment reaches LION_SPARSE_THRESHOLD members, the key is promoted to a
 * container of its own, and that container is a single run of 14 bytes --
 * which still fits inline.
 */
SELECT entries, inline_entries, containers, sparse_segments, run_containers,
	   container_pages > 0 AS spilled, ntids
  FROM lion_index_stats('lion_spill_k');
SELECT lion_index_verify('lion_spill_k', true);

-- Appends at the tail of the chain, then many more keys.
INSERT INTO lion_spill SELECT i, i % 97 FROM generate_series(201, 60000) i;
SELECT entries, inline_entries, container_pages > 0 AS has_container_pages, ntids
  FROM lion_index_stats('lion_spill_k');
SELECT lion_index_verify('lion_spill_k', true);
SELECT lion_cmp('lion_spill', 'k = 1');
SELECT lion_cmp('lion_spill', 'k = 42');

/*
 * ARRAY to BITSET growth: 40000 rows of two alternating keys cover about 178
 * heap blocks, so each key gets three container keys, the first with far
 * more than the 2048 members an array container can hold.  The keys have to
 * alternate: a key whose TIDs are consecutive ends up as a run container of
 * a handful of bytes and never reaches the array bound at all.
 */
CREATE TABLE lion_dense (i int4, k int4);
CREATE INDEX lion_dense_k ON lion_dense USING lion (k);
INSERT INTO lion_dense SELECT i, i % 2 FROM generate_series(1, 40000) i;
SELECT bitset_containers > 0 AS has_bitset, sparse_segments, ntids
  FROM lion_index_stats('lion_dense_k');
SELECT lion_index_verify('lion_dense_k', true);
SELECT lion_cmp('lion_dense', 'k = 1');

/*
 * Inserts that land in the MIDDLE of a container chain, which is what makes
 * a container page split.
 *
 * Key 2 is spread over the whole heap and every one of its containers is a
 * bitset, so its chain is one container per page with no room to spare.  Key
 * 1 owns the low heap blocks alone; deleting it and vacuuming frees those
 * blocks without touching key 2's chain.  The rows inserted afterwards go
 * into the freed low blocks and belong before everything key 2 already has,
 * so their containers have to be placed at the head of a full chain -- and
 * they grow to bitsets there, because key 3 is interleaved with them and
 * keeps their members from forming runs.
 */
CREATE TABLE lion_mid (i int4, k int4);
CREATE INDEX lion_mid_k ON lion_mid USING lion (k) WITH (inline_limit = 64);
INSERT INTO lion_mid
SELECT i, CASE WHEN i <= 100000 THEN 1 ELSE 2 - (i % 2) END
  FROM generate_series(1, 300000) i;
CREATE TEMP TABLE lion_mid_before AS
	SELECT container_pages FROM lion_index_stats('lion_mid_k');
SELECT bitset_containers > 0 AS key2_has_bitsets
  FROM lion_index_stats('lion_mid_k');
DELETE FROM lion_mid WHERE k = 1;
VACUUM lion_mid;
INSERT INTO lion_mid SELECT i, 2 + (i % 2) FROM generate_series(300001, 400000) i;
/*
 * Key 1's posting set is empty after the VACUUM, so its entry is deleted and
 * its whole chain is freed (DESIGN.md §18).  The pages the new containers
 * need therefore come back out of the free space map instead of extending the
 * relation: the index holds no MORE container pages than it did before, even
 * though the containers that go in do not fit where they belong and split the
 * pages they land on.
 */
SELECT (SELECT container_pages FROM lion_index_stats('lion_mid_k')) <=
	   (SELECT container_pages FROM lion_mid_before) AS pages_reused;
/*
 * DESIGN.md §22: those posting sets are TREES - the entry's head is a root
 * with downlinks and the leaves are the container pages - so the inserts above
 * found their page by descending rather than by walking from the head, and the
 * whole shape survives verify().
 */
SELECT posting_internal_pages > 0 AS has_internal_pages,
	   max_posting_height = 1 AS two_levels
  FROM lion_index_stats('lion_mid_k');
SELECT lion_index_posting_root('lion_mid_k', 2) IS NOT NULL AS key2_has_a_root;
SELECT lion_index_verify('lion_mid_k', true);
SELECT lion_cmp('lion_mid', 'k = 2');
SELECT lion_cmp('lion_mid', 'k = 3');
SELECT ntids FROM lion_index_stats('lion_mid_k');

/*
 * A new container key in the middle of an INLINE payload: key 1 has its TIDs
 * spread over the whole heap, so its inline payload is a list of containers
 * with rising container keys; after the low heap pages are freed and reused,
 * a new insert belongs in front of the ones that are left.
 */
CREATE TABLE lion_inlmid (i int4, k int4);
CREATE INDEX lion_inlmid_k ON lion_inlmid USING lion (k);
INSERT INTO lion_inlmid
SELECT i, CASE WHEN i % 20000 = 0 THEN 1 ELSE 2 END
  FROM generate_series(1, 300000) i;
SELECT entries, inline_entries FROM lion_index_stats('lion_inlmid_k');
DELETE FROM lion_inlmid WHERE i <= 100000;
VACUUM lion_inlmid;
INSERT INTO lion_inlmid VALUES (300001, 1);
SELECT inline_entries, ntids FROM lion_index_stats('lion_inlmid_k');
SELECT lion_index_verify('lion_inlmid_k', true);
SELECT lion_cmp('lion_inlmid', 'k = 1');

-- Inserts inside an aborted transaction leave the index consistent (the TIDs
-- stay behind, exactly as in every other index AM, and VACUUM removes them).
BEGIN;
INSERT INTO lion_dense SELECT i, 2 FROM generate_series(20001, 25000) i;
ROLLBACK;
SELECT lion_index_verify('lion_dense_k', true);
SELECT lion_cmp('lion_dense', 'k = 2');
VACUUM lion_dense;
SELECT lion_index_verify('lion_dense_k', true);
SELECT lion_cmp('lion_dense', 'k = 2');

/*
 * GROWTH SLACK (DESIGN.md section 4).  An item an insert writes on a
 * container page is allotted a few bytes more than it needs, and the next
 * members go into that slack in place, without moving anything else on the
 * page.  20 keys over 20000 rows put about 700 members in each container key
 * of each key: too many to stay a sparse segment, too few for a bitset, and
 * not consecutive, so every container is an ARRAY that grows two bytes at a
 * time.  inline_limit = 64 keeps the posting sets off the directory leaves,
 * where items have no slack.
 */
CREATE TABLE lion_slack (i int4, k int4);
CREATE INDEX lion_slack_k ON lion_slack USING lion (k) WITH (inline_limit = 64);
INSERT INTO lion_slack SELECT i, i % 20 FROM generate_series(1, 20000) i;
SELECT array_containers > 0 AS has_arrays, slack_bytes > 0 AS has_slack,
	   slack_bytes <= 72 * (containers + sparse_segments) AS slack_within_bound
  FROM lion_index_stats('lion_slack_k');
SELECT lion_index_verify('lion_slack_k', true);
SELECT lion_cmp('lion_slack', 'k = 7');

-- More inserts into the same container keys: they go into the slack, and the
-- posting sets stay exactly as correct.
CREATE TEMP TABLE lion_slack_before AS
	SELECT container_bytes, container_pages FROM lion_index_stats('lion_slack_k');
INSERT INTO lion_slack SELECT i, i % 20 FROM generate_series(20001, 22000) i;
SELECT ntids, slack_bytes > 0 AS has_slack,
	   slack_bytes <= 72 * (containers + sparse_segments) AS slack_within_bound
  FROM lion_index_stats('lion_slack_k');
SELECT (SELECT container_bytes FROM lion_index_stats('lion_slack_k')) >
	   (SELECT container_bytes FROM lion_slack_before) AS items_grew;
SELECT lion_index_verify('lion_slack_k', true);
SELECT lion_cmp('lion_slack', 'k = 7');
SELECT lion_cmp('lion_slack', 'k = 0');

-- A bulk build writes items at their exact size: a bulk-built index of the
-- very same rows has no slack at all.
CREATE INDEX lion_slack_b ON lion_slack USING lion (k) WITH (inline_limit = 64);
SELECT array_containers > 0 AS has_arrays, slack_bytes AS bulk_slack_bytes
  FROM lion_index_stats('lion_slack_b');
SELECT lion_index_verify('lion_slack_b', true);

/*
 * GROWTH SLACK INSIDE AN INLINE ENTRY (DESIGN.md §4).
 *
 * An INLINE entry an insert rewrites is allotted a few bytes more than its
 * payload needs, zeroed - which is also what terminates the payload, so
 * nothing else has to know they are there.  The next insert into that key
 * then writes INSIDE the entry: no other entry on the leaf moves, and the WAL
 * record is the bytes that changed instead of the whole payload.
 */
CREATE TABLE lion_islack (i int4, k int4);
CREATE INDEX lion_islack_k ON lion_islack USING lion (k);
INSERT INTO lion_islack SELECT i, i % 50 FROM generate_series(1, 5000) i;
SELECT inline_entries, container_pages,
	   inline_slack_bytes > 0 AS has_inline_slack,
	   inline_slack_bytes <= 72 * inline_entries AS slack_within_bound
  FROM lion_index_stats('lion_islack_k');
SELECT lion_index_verify('lion_islack_k', true);
SELECT lion_cmp('lion_islack', 'k = 7');

-- More rows under the same keys: they go into the slack, and the index does
-- not have to grow a page for them.
CREATE TEMP TABLE lion_islack_size AS
	SELECT pg_relation_size('lion_islack_k') AS bytes;
INSERT INTO lion_islack SELECT i, i % 50 FROM generate_series(5001, 5200) i;
SELECT pg_relation_size('lion_islack_k') <= (SELECT bytes FROM lion_islack_size)
	   AS grew_into_the_slack;
SELECT ntids, inline_slack_bytes <= 72 * inline_entries AS slack_within_bound
  FROM lion_index_stats('lion_islack_k');
SELECT lion_index_verify('lion_islack_k', true);
SELECT lion_cmp('lion_islack', 'k = 7');

-- A bulk build adds none of it, exactly as for items.
CREATE INDEX lion_islack_b ON lion_islack USING lion (k);
SELECT inline_entries > 0 AS has_inline, inline_slack_bytes AS bulk_inline_slack
  FROM lion_index_stats('lion_islack_b');
SELECT lion_index_verify('lion_islack_b', true);

/*
 * ... and the slack is not what decides a spill: the payload test is made on
 * the bytes the payload really uses, so a key still spills onto container
 * pages at inline_limit and not before it.
 */
CREATE TABLE lion_ispill (i int4, k int4);
CREATE INDEX lion_ispill_k ON lion_ispill USING lion (k)
	WITH (inline_limit = 256);
INSERT INTO lion_ispill SELECT i, i % 3 FROM generate_series(1, 60) i;
SELECT inline_entries, container_pages,
	   inline_slack_bytes <= 72 * inline_entries AS slack_within_bound
  FROM lion_index_stats('lion_ispill_k');
INSERT INTO lion_ispill SELECT i, i % 3 FROM generate_series(61, 20000) i;
SELECT inline_entries, container_pages > 0 AS spilled, ntids
  FROM lion_index_stats('lion_ispill_k');
SELECT lion_index_verify('lion_ispill_k', true);
SELECT lion_cmp('lion_ispill', 'k = 1');
DROP TABLE lion_islack, lion_ispill;

/*
 * An index created empty and grown: with the sorted directory of DESIGN.md
 * §21 that is no longer a problem to be warned about.  The tree splits as the
 * keys arrive, so 80000 of them give it a height and a few hundred leaves,
 * and a lookup costs one descent whatever the index was built from.  What a
 * REINDEX buys is a shallower, denser tree, not a working one.
 */
CREATE TABLE lion_grow (i int4, k int4);
CREATE INDEX lion_grow_k ON lion_grow USING lion (k);
SELECT directory_height, leaf_pages, internal_pages, entries
  FROM lion_index_stats('lion_grow_k');
INSERT INTO lion_grow SELECT i, i FROM generate_series(1, 80000) i;
SELECT directory_height > 0 AS grew_a_height, leaf_pages > 100 AS many_leaves,
	   entries = 80000 AS all_keys
  FROM lion_index_stats('lion_grow_k');
SELECT lion_index_verify('lion_grow_k', true);
SELECT lion_cmp('lion_grow', 'k = 12345');
CREATE TEMP TABLE lion_grow_before AS
	SELECT directory_height, leaf_pages FROM lion_index_stats('lion_grow_k');
REINDEX INDEX lion_grow_k;
/*
 * Ascending keys fill their leaves COMPLETELY - a split of the rightmost page
 * puts only the new key on the right one (DESIGN.md §21) - so a rebuild at
 * the default fillfactor of 90 comes out slightly BIGGER, not smaller.  Both
 * are within a few percent of the bytes the entries need, which is the thing
 * the hash directory could not manage.
 */
SELECT (SELECT leaf_pages FROM lion_index_stats('lion_grow_k')) <
	   (SELECT leaf_pages * 5 / 4 FROM lion_grow_before) AS rebuild_is_about_the_same,
	   (SELECT entries FROM lion_index_stats('lion_grow_k')) = 80000 AS all_keys;
SELECT lion_index_verify('lion_grow_k', true);
SELECT lion_cmp('lion_grow', 'k = 12345');
-- `buckets` is accepted and ignored since format 4 (DESIGN.md §21).
CREATE INDEX lion_grow_x ON lion_grow USING lion (k) WITH (buckets = 4);
INSERT INTO lion_grow VALUES (80001, 80001);
SELECT entries = 80001 AS all_keys FROM lion_index_stats('lion_grow_x');
DROP INDEX lion_grow_x;

/*
 * A partial index holds the rows its predicate selects and no more, whatever
 * pg_class.reltuples says about the heap; since DESIGN.md §21 nothing is
 * sized from that estimate at all - the tree is built from the entries that
 * are really there - so the only thing to check is that both are correct.
 */
CREATE TABLE lion_relt (i int4, k int4);
INSERT INTO lion_relt SELECT i, i FROM generate_series(1, 40000) i;
ANALYZE lion_relt;
DELETE FROM lion_relt WHERE i > 4000;
CREATE INDEX lion_relt_k ON lion_relt USING lion (k);
CREATE INDEX lion_relt_p ON lion_relt USING lion (k) WHERE i <= 2000;
SELECT (SELECT entries FROM lion_index_stats('lion_relt_k')) >
	   (SELECT entries FROM lion_index_stats('lion_relt_p'))
	   AS whole_table_has_more_entries;
VACUUM (ANALYZE) lion_relt;
REINDEX INDEX lion_relt_k;
SELECT lion_index_verify('lion_relt_k', true);
SELECT lion_index_verify('lion_relt_p', true);
SELECT lion_cmp('lion_relt', 'k = 1234');

DROP TABLE lion_ins, lion_spill, lion_dense, lion_mid, lion_inlmid, lion_slack,
		   lion_grow, lion_relt;
DROP FUNCTION lion_ins_check(text);
