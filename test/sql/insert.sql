-- Inserts into a roaring index (DESIGN.md section 5, INSERT).
--
-- Every phase compares index scans with sequential scans and runs
-- roaring_index_verify(idx, true), which also checks that every heap tuple
-- visible to a fresh snapshot is indexed under its key.
\set VERBOSITY terse
SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS roaring_index;

SELECT setseed(0.42);

CREATE OR REPLACE FUNCTION rbi_cmp(tbl text, pred text) RETURNS text
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
CREATE TABLE rbi_ins (i int4, k int4, t text, b bool);
CREATE INDEX rbi_ins_k ON rbi_ins USING roaring (k);
CREATE INDEX rbi_ins_t ON rbi_ins USING roaring (t);
CREATE INDEX rbi_ins_b ON rbi_ins USING roaring (b);

CREATE OR REPLACE FUNCTION rbi_ins_check(tag text) RETURNS text
LANGUAGE plpgsql AS $$
BEGIN
	PERFORM roaring_index_verify('rbi_ins_k', true);
	PERFORM roaring_index_verify('rbi_ins_t', true);
	PERFORM roaring_index_verify('rbi_ins_b', true);
	RETURN format('%s | k=3 %s | t=v3 %s | t=v7 %s | b %s | k=-1 %s', tag,
				  rbi_cmp('rbi_ins', 'k = 3'),
				  rbi_cmp('rbi_ins', 't = ''v3'''),
				  rbi_cmp('rbi_ins', 't = ''v7'''),
				  rbi_cmp('rbi_ins', 'b'),
				  rbi_cmp('rbi_ins', 'k = -1'));
END $$;

SELECT rbi_ins_check('empty');

-- Phase 1: single-row inserts, each its own statement.
INSERT INTO rbi_ins VALUES (1, 1, 'v1', true);
INSERT INTO rbi_ins VALUES (2, 2, 'v2', false);
INSERT INTO rbi_ins VALUES (3, 3, 'v3', true);
INSERT INTO rbi_ins VALUES (4, 3, 'v3', false);
INSERT INTO rbi_ins VALUES (5, 7, 'v7', true);
SELECT rbi_ins_check('single rows');
SELECT * FROM roaring_index_stats('rbi_ins_k');

-- Phase 2: multi-row VALUES.
INSERT INTO rbi_ins VALUES
	(6, 3, 'v3', false), (7, 7, 'v7', true), (8, 11, 'v11', false),
	(9, 3, 'v7', true), (10, 7, 'v3', false);
SELECT rbi_ins_check('multi-row values');

-- Phase 3: a bulk INSERT ... SELECT.  100k rows over 13 int keys, 7 text
-- keys and 2 booleans.
INSERT INTO rbi_ins
SELECT i, i % 13, 'v' || (i % 7), (i % 3) = 0
  FROM generate_series(11, 100010) i;
SELECT rbi_ins_check('insert select 100k');
SELECT entries, inline_entries, ntids FROM roaring_index_stats('rbi_ins_k');
SELECT entries, inline_entries, ntids FROM roaring_index_stats('rbi_ins_t');
SELECT container_pages > 0 AS has_container_pages,
	   containers > entries AS several_containers_per_key
  FROM roaring_index_stats('rbi_ins_k');

-- Phase 4: UPDATE of an indexed column.  The new tuple cannot be HOT, so it
-- gets a fresh TID in every index; the old TIDs stay until VACUUM.
UPDATE rbi_ins SET k = 5 WHERE i BETWEEN 100 AND 5000;
SELECT rbi_ins_check('update indexed column');
SELECT rbi_cmp('rbi_ins', 'k = 5');

-- An update of a non-indexed column is HOT and leaves the index alone.
UPDATE rbi_ins SET i = i WHERE i BETWEEN 200 AND 300;
SELECT rbi_ins_check('hot update');

-- Phase 5: DELETE without VACUUM.  The index still points at the dead rows;
-- the bitmap heap scan has to filter them.
DELETE FROM rbi_ins WHERE i % 7 = 0;
SELECT rbi_ins_check('delete without vacuum');

-- Phase 6: a key that appears for the first time long after the build.
INSERT INTO rbi_ins VALUES (200001, 424242, 'brand new key', true);
SELECT rbi_ins_check('brand new key');
SELECT rbi_cmp('rbi_ins', 'k = 424242');
SELECT rbi_cmp('rbi_ins', 't = ''brand new key''');

-- Phase 7: cross-type insert values and cross-type scan keys.
INSERT INTO rbi_ins VALUES (200002, 3::int8, 'v7', true);
INSERT INTO rbi_ins VALUES (200003, 3::int2, 'v7'::varchar, false);
INSERT INTO rbi_ins VALUES (200004, 3::numeric, 'v7', true);
SELECT rbi_ins_check('cross-type values');
SELECT rbi_cmp('rbi_ins', 'k = 3::int8');
SELECT rbi_cmp('rbi_ins', 'k = 3::int2');

-- NULL keys are not indexed, and must not upset the entry that has the same
-- hash bucket.
INSERT INTO rbi_ins VALUES (200005, NULL, NULL, NULL);
SELECT rbi_ins_check('null keys');
SELECT count(*) FROM rbi_ins WHERE k IS NULL;

/*
 * An INLINE entry that spills.  inline_limit = 64 bytes holds four one-pair
 * sparse segments, so the first inserts of a key stay inside the entry tuple
 * and a later one moves the posting set onto container pages.
 *
 * buckets = 1 on purpose: the 97 keys inserted below then all land in the
 * same bucket, which is the only way aminsert reaches the long entry list
 * and the bucket page chain (auto-sizing gives an index built on an empty
 * table RBI_DEFAULT_BUCKETS buckets, so nothing else here would).
 */
CREATE TABLE rbi_spill (i int4, k int4);
CREATE INDEX rbi_spill_k ON rbi_spill USING roaring (k)
	WITH (buckets = 1, inline_limit = 64);
INSERT INTO rbi_spill VALUES (1, 1);
-- the first TID of a key is one pair in one sparse segment (DESIGN.md 13)
SELECT entries, inline_entries, containers, sparse_segments, sparse_members,
	   ntids
  FROM roaring_index_stats('rbi_spill_k');
INSERT INTO rbi_spill SELECT i, 1 FROM generate_series(2, 200) i;
/*
 * These 200 rows are consecutive, so they share one container key: the
 * segment reaches RBI_SPARSE_THRESHOLD members, the key is promoted to a
 * container of its own, and that container is a single run of 14 bytes --
 * which still fits inline.
 */
SELECT entries, inline_entries, containers, sparse_segments, run_containers,
	   container_pages > 0 AS spilled, ntids
  FROM roaring_index_stats('rbi_spill_k');
SELECT roaring_index_verify('rbi_spill_k', true);

-- Appends at the tail of the chain, then many more keys.
INSERT INTO rbi_spill SELECT i, i % 97 FROM generate_series(201, 60000) i;
SELECT entries, inline_entries, container_pages > 0 AS has_container_pages, ntids
  FROM roaring_index_stats('rbi_spill_k');
SELECT roaring_index_verify('rbi_spill_k', true);
SELECT rbi_cmp('rbi_spill', 'k = 1');
SELECT rbi_cmp('rbi_spill', 'k = 42');

/*
 * ARRAY to BITSET growth: 40000 rows of two alternating keys cover about 178
 * heap blocks, so each key gets three container keys, the first with far
 * more than the 2048 members an array container can hold.  The keys have to
 * alternate: a key whose TIDs are consecutive ends up as a run container of
 * a handful of bytes and never reaches the array bound at all.
 */
CREATE TABLE rbi_dense (i int4, k int4);
CREATE INDEX rbi_dense_k ON rbi_dense USING roaring (k);
INSERT INTO rbi_dense SELECT i, i % 2 FROM generate_series(1, 40000) i;
SELECT bitset_containers > 0 AS has_bitset, sparse_segments, ntids
  FROM roaring_index_stats('rbi_dense_k');
SELECT roaring_index_verify('rbi_dense_k', true);
SELECT rbi_cmp('rbi_dense', 'k = 1');

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
CREATE TABLE rbi_mid (i int4, k int4);
CREATE INDEX rbi_mid_k ON rbi_mid USING roaring (k) WITH (inline_limit = 64);
INSERT INTO rbi_mid
SELECT i, CASE WHEN i <= 100000 THEN 1 ELSE 2 - (i % 2) END
  FROM generate_series(1, 300000) i;
CREATE TEMP TABLE rbi_mid_before AS
	SELECT container_pages FROM roaring_index_stats('rbi_mid_k');
SELECT bitset_containers > 0 AS key2_has_bitsets
  FROM roaring_index_stats('rbi_mid_k');
DELETE FROM rbi_mid WHERE k = 1;
VACUUM rbi_mid;
INSERT INTO rbi_mid SELECT i, 2 + (i % 2) FROM generate_series(300001, 400000) i;
-- the new containers do not fit where they belong, so pages were split
SELECT (SELECT container_pages FROM roaring_index_stats('rbi_mid_k')) >
	   (SELECT container_pages FROM rbi_mid_before) AS pages_split;
SELECT roaring_index_verify('rbi_mid_k', true);
SELECT rbi_cmp('rbi_mid', 'k = 2');
SELECT rbi_cmp('rbi_mid', 'k = 3');
SELECT ntids FROM roaring_index_stats('rbi_mid_k');

/*
 * A new container key in the middle of an INLINE payload: key 1 has its TIDs
 * spread over the whole heap, so its inline payload is a list of containers
 * with rising container keys; after the low heap pages are freed and reused,
 * a new insert belongs in front of the ones that are left.
 */
CREATE TABLE rbi_inlmid (i int4, k int4);
CREATE INDEX rbi_inlmid_k ON rbi_inlmid USING roaring (k);
INSERT INTO rbi_inlmid
SELECT i, CASE WHEN i % 20000 = 0 THEN 1 ELSE 2 END
  FROM generate_series(1, 300000) i;
SELECT entries, inline_entries FROM roaring_index_stats('rbi_inlmid_k');
DELETE FROM rbi_inlmid WHERE i <= 100000;
VACUUM rbi_inlmid;
INSERT INTO rbi_inlmid VALUES (300001, 1);
SELECT inline_entries, ntids FROM roaring_index_stats('rbi_inlmid_k');
SELECT roaring_index_verify('rbi_inlmid_k', true);
SELECT rbi_cmp('rbi_inlmid', 'k = 1');

-- Inserts inside an aborted transaction leave the index consistent (the TIDs
-- stay behind, exactly as in every other index AM, and VACUUM removes them).
BEGIN;
INSERT INTO rbi_dense SELECT i, 2 FROM generate_series(20001, 25000) i;
ROLLBACK;
SELECT roaring_index_verify('rbi_dense_k', true);
SELECT rbi_cmp('rbi_dense', 'k = 2');
VACUUM rbi_dense;
SELECT roaring_index_verify('rbi_dense_k', true);
SELECT rbi_cmp('rbi_dense', 'k = 2');

DROP TABLE rbi_ins, rbi_spill, rbi_dense, rbi_mid, rbi_inlmid;
DROP FUNCTION rbi_ins_check(text);
