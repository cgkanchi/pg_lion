-- Page deletion in the entry directory (DESIGN.md §21, "Page deletion"):
-- VACUUM takes the leaves it empties out of the tree, with the internal pages
-- above them that have no other child, and the pages are used again.
\set VERBOSITY terse
SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS pg_lion;
CREATE EXTENSION IF NOT EXISTS pageinspect;

/*
 * Keys of 600 bytes: a dozen to a leaf and a dozen downlinks to an internal
 * page, so 6,000 of them make a directory of four levels.  The key right
 * after key 3700, with 'hot' appended, has 5,000 rows and a posting tree of
 * its own.  dd_ref holds the same rows and no index: it is what every answer
 * is compared with.
 */
CREATE FUNCTION dd_key(i int) RETURNS text
LANGUAGE sql IMMUTABLE AS $$ SELECT 'k' || lpad(i::text, 599, '0') $$;
CREATE TABLE dd (id int, k text) WITH (autovacuum_enabled = off);
ALTER TABLE dd ALTER k SET STORAGE PLAIN;
CREATE TABLE dd_ref (id int, k text) WITH (autovacuum_enabled = off);
INSERT INTO dd SELECT i, dd_key(i) FROM generate_series(1, 6000) i;
INSERT INTO dd SELECT 100000 + i, dd_key(3700) || 'hot' FROM generate_series(1, 5000) i;
INSERT INTO dd_ref SELECT * FROM dd;
CREATE INDEX dd_k ON dd USING lion (k);
ANALYZE dd;

/* q's answer on dd through the index, and on dd_ref, and whether they agree. */
CREATE FUNCTION dd_same(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	a text;
	b text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_sort', 'off', true);
	EXECUTE format('SELECT string_agg(x::text, %L) FROM (%s) s(x)', ',',
				   format(q, 'dd')) INTO a;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_sort', 'on', true);
	EXECUTE format('SELECT string_agg(x::text, %L) FROM (%s) s(x)', ',',
				   format(q, 'dd_ref')) INTO b;
	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH index=%s reference=%s', a, b);
	END IF;
	RETURN 'ok ' || coalesce(left(a, 24), '<none>');
END $$;

-- The queries every step checks: the ends, both directions of an ordered
-- walk, counts over ranges, lookups of keys that are gone and keys that are
-- not, and the key with a posting tree.
CREATE FUNCTION dd_check() RETURNS SETOF text
LANGUAGE sql AS $$
	SELECT dd_same('SELECT min(k) FROM %I')
	UNION ALL SELECT dd_same('SELECT max(k) FROM %I')
	UNION ALL SELECT dd_same('SELECT id FROM (SELECT id FROM %I ORDER BY k LIMIT 3) t')
	UNION ALL SELECT dd_same('SELECT id FROM (SELECT id FROM %I ORDER BY k DESC LIMIT 3) t')
	UNION ALL SELECT dd_same('SELECT count(*) FROM %I WHERE k < ' || quote_literal(dd_key(2500)))
	UNION ALL SELECT dd_same('SELECT count(*) FROM %I WHERE k >= ' || quote_literal(dd_key(3500)) ||
							 ' AND k < ' || quote_literal(dd_key(5200)))
	UNION ALL SELECT dd_same('SELECT count(*) FROM %I WHERE k = ' || quote_literal(dd_key(10)))
	UNION ALL SELECT dd_same('SELECT count(*) FROM %I WHERE k = ' || quote_literal(dd_key(4321)))
	UNION ALL SELECT dd_same('SELECT count(*) FROM %I WHERE k = ' || quote_literal(dd_key(5999)))
	UNION ALL SELECT dd_same('SELECT count(*) FROM %I WHERE k = ' || quote_literal(dd_key(4903)))
	UNION ALL SELECT dd_same('SELECT count(*) FROM %I WHERE k = ' || quote_literal(dd_key(4998)))
	UNION ALL SELECT dd_same('SELECT count(*) FROM %I WHERE k = ' || quote_literal(dd_key(3700) || 'hot'))
$$;

CREATE TABLE dd_size (step text, pages bigint, leaves bigint, internal bigint,
					  deleted bigint);
CREATE FUNCTION dd_note(step text) RETURNS void
LANGUAGE sql AS $$
	INSERT INTO dd_size
	SELECT step, pg_relation_size('dd_k') / current_setting('block_size')::int,
		   leaf_pages, internal_pages, deleted_pages
	  FROM lion_index_stats('dd_k')
$$;
/*
 * The internal pages of the directory that are not the rightmost of their
 * level, by how many downlinks they hold: their items but the high key.
 */
CREATE FUNCTION dd_downlinks(OUT level int, OUT downlinks int, OUT pages bigint)
RETURNS SETOF record LANGUAGE sql AS $$
	SELECT get_byte(r, s + 24) + 256 * get_byte(r, s + 25), (h.lower - 24) / 4 - 1,
		   count(*)
	  FROM generate_series(1, (pg_relation_size('dd_k') / 8192)::int - 1) b,
		   LATERAL get_raw_page('dd_k', b) r,
		   LATERAL (SELECT special AS s, lower FROM page_header(r)) h
	 -- DIR and not DELETED, with a right link
	 WHERE (get_byte(r, s + 26) + 256 * get_byte(r, s + 27)) & (16 | 8) = 16
	   AND get_byte(r, s) + get_byte(r, s + 1) + get_byte(r, s + 2) + get_byte(r, s + 3) < 4 * 255
	 GROUP BY 1, 2
$$;

SELECT dd_note('built');
SELECT directory_height, leaf_pages, internal_pages, deleted_pages
  FROM lion_index_stats('dd_k');
SELECT * FROM dd_check();

-- ---------- 1. the oldest half goes ----------
-- Every leaf of keys 1 .. 3000 empties, the leftmost among them, and so do
-- the internal pages above them that have no other key left under them.
-- What stays is the last child of a parent that keeps others, and the
-- rightmost page of each level.
DELETE FROM dd WHERE id <= 3000;
DELETE FROM dd_ref WHERE id <= 3000;
VACUUM dd;
SELECT dd_note('oldest half');
SELECT s.leaves < b.leaves / 2 + 20 AS leaves_went,
	   s.internal < b.internal AS internal_went,
	   s.deleted > b.leaves / 3 AS pages_deleted,
	   s.pages = b.pages AS same_size
  FROM dd_size s, dd_size b WHERE s.step = 'oldest half' AND b.step = 'built';
SELECT * FROM lion_index_verify('dd_k');
SELECT * FROM dd_check();

-- ---------- 2. a run in the middle goes ----------
-- A parent whose children all empty keeps its last one, empty: that
-- downlink is the parent's only one, and the parent's own downlink is the
-- last on the page above it, so neither goes.  An internal page of one
-- downlink is what a deletion may leave (DESIGN.md §21, "Height").
DELETE FROM dd WHERE id BETWEEN 4000 AND 4999;
DELETE FROM dd_ref WHERE id BETWEEN 4000 AND 4999;
VACUUM dd;
SELECT dd_note('middle');
SELECT s.leaves < o.leaves AS leaves_went, s.deleted > o.deleted AS pages_deleted
  FROM dd_size s, dd_size o WHERE s.step = 'middle' AND o.step = 'oldest half';
SELECT pages AS one_downlink FROM dd_downlinks() WHERE level > 0 AND downlinks = 1;
SELECT * FROM lion_index_verify('dd_k');
SELECT * FROM dd_check();

-- ---------- 3. new keys use the pages again ----------
-- A transaction id is spent first, so that the deleted pages' safexid is
-- behind every snapshot, and the VACUUM after it, which has no row to remove,
-- hands them to the free space map (nbtree's rule, DESIGN.md §18, "Reuse"):
-- the index does not grow, and fewer of its pages are free.  Keys 4999 down
-- to 4900 go first, into the range the run left: they go under the page of
-- one downlink, whose leaf splits again and again, and the page takes their
-- separators after its minus-infinity downlink and splits itself.
SELECT txid_current() IS NOT NULL AS spent;
VACUUM dd;
INSERT INTO dd SELECT i, dd_key(i) FROM generate_series(4999, 4900, -1) i;
INSERT INTO dd SELECT i, dd_key(i) FROM generate_series(6001, 8000) i;
INSERT INTO dd SELECT i, dd_key(i) FROM generate_series(4000, 4499) i;
INSERT INTO dd_ref SELECT i, dd_key(i) FROM generate_series(4999, 4900, -1) i;
INSERT INTO dd_ref SELECT i, dd_key(i) FROM generate_series(6001, 8000) i;
INSERT INTO dd_ref SELECT i, dd_key(i) FROM generate_series(4000, 4499) i;
SELECT dd_note('reinserted');
SELECT s.pages = m.pages AS no_growth, s.deleted < m.deleted AS reused
  FROM dd_size s, dd_size m WHERE s.step = 'reinserted' AND m.step = 'middle';
SELECT min(downlinks) AS fewest_downlinks FROM dd_downlinks() WHERE level > 0;
SELECT * FROM lion_index_verify('dd_k');
SELECT * FROM dd_check();

-- ---------- 4. the posting tree's key goes, then everything ----------
DELETE FROM dd WHERE k = dd_key(3700) || 'hot';
DELETE FROM dd_ref WHERE k = dd_key(3700) || 'hot';
VACUUM dd;
SELECT * FROM lion_index_verify('dd_k');
SELECT * FROM dd_check();
DELETE FROM dd;
DELETE FROM dd_ref;
VACUUM dd;
SELECT dd_note('empty');
-- what is left is the root and the rightmost page of each level below it
SELECT directory_height, leaf_pages, internal_pages, entries
  FROM lion_index_stats('dd_k');
SELECT * FROM lion_index_verify('dd_k');
SELECT * FROM dd_check();

-- ---------- 5. and grows again ----------
SELECT txid_current() IS NOT NULL AS spent;
VACUUM dd;
INSERT INTO dd SELECT i, dd_key(i) FROM generate_series(1, 3000) i;
INSERT INTO dd_ref SELECT i, dd_key(i) FROM generate_series(1, 3000) i;
SELECT dd_note('refilled');
SELECT s.pages = e.pages AS no_growth
  FROM dd_size s, dd_size e WHERE s.step = 'refilled' AND e.step = 'empty';
SELECT * FROM lion_index_verify('dd_k');
SELECT * FROM dd_check();

DROP TABLE dd, dd_ref, dd_size;
DROP FUNCTION dd_check(), dd_same(text), dd_key(int), dd_note(text),
	dd_downlinks();

-- ---------- 6. the free space map holds the pages it may hand out ----------
-- VACUUM records a DELETED page in the map once no scan can still hold a
-- link to it, and not before, and an allocation takes the pages the map
-- offers until one may be taken (DESIGN.md §18, "Reuse").  Two VACUUMs, the
-- first of which takes out leaves of the upper keys and the second leaves of
-- the lower ones, below them in the index, leave free pages of two ages: the
-- map holds the old ones only, the inserts after the second VACUUM take them
-- and the index does not grow, and the young ones join the map with the next
-- VACUUM.  A map that listed the young pages as well would offer them first,
-- by block, to inserts that may not take them yet.
CREATE EXTENSION IF NOT EXISTS pg_freespacemap;

/*
 * The blocks idx's free space map lists, by what they are: free pages the
 * next allocation may take - all-zero, or DELETED or an empty UNLINKED
 * posting leaf with a safexid behind every snapshot (its transaction id
 * before the oldest one this snapshot sees running) - free pages it may not
 * take yet, and live pages.  A DELETED directory page keeps its safexid in
 * its special area's minckey and maxckey, a DELETED container page in the
 * eight bytes after the page header, and an UNLINKED leaf the low half in
 * its leftlink (lion.h).
 */
CREATE FUNCTION dd_fsm(idx regclass, OUT reusable bigint, OUT waiting bigint,
	OUT live bigint)
LANGUAGE sql AS $$
	WITH p AS (
		SELECT h.lower = 0 AS new, fl, r, h.special AS s
		  FROM pg_freespace(idx) f,
			   LATERAL get_raw_page(idx::text, f.blkno::int) r,
			   LATERAL page_header(r) h,
			   LATERAL (SELECT CASE WHEN h.lower = 0 THEN 0
								ELSE get_byte(r, h.special + 26) +
									 256 * get_byte(r, h.special + 27) END) x(fl)
		 WHERE f.avail > 0 AND f.blkno > 0),
	x AS (
		SELECT new, fl,
			   CASE WHEN fl & 8 <> 0 AND fl & (2 | 16) <> 0 THEN
						(SELECT sum(get_byte(r, s + 8 + i)::numeric * 256 ^ i)
						   FROM generate_series(0, 7) i)
					WHEN fl & 8 <> 0 THEN
						(SELECT sum(get_byte(r, 24 + i)::numeric * 256 ^ i)
						   FROM generate_series(0, 7) i)
					WHEN fl & 4 <> 0 AND fl & 512 <> 0 THEN
						(SELECT sum(get_byte(r, s + 4 + i)::numeric * 256 ^ i)
						   FROM generate_series(0, 3) i)
			   END AS safexid,
			   (pg_snapshot_xmin(pg_current_snapshot())::text)::numeric AS xmin
		  FROM p)
	SELECT count(*) FILTER (WHERE new OR safexid < CASE WHEN fl & 8 <> 0 THEN xmin
														ELSE xmin % 4294967296 END),
		   count(*) FILTER (WHERE NOT new AND safexid >= CASE WHEN fl & 8 <> 0 THEN xmin
															 ELSE xmin % 4294967296 END),
		   count(*) FILTER (WHERE NOT new AND safexid IS NULL)
	  FROM x
$$;

CREATE TABLE dd_fo (id int, k int) WITH (autovacuum_enabled = off);
INSERT INTO dd_fo SELECT i, i FROM generate_series(1, 100000) i;
CREATE INDEX dd_fo_k ON dd_fo USING lion (k);
SELECT pg_relation_size('dd_fo_k') / current_setting('block_size')::int AS blocks \gset

-- The upper keys go, then a transaction id is spent: their leaves are behind
-- every snapshot.  Then the lower keys go: theirs are not.
DELETE FROM dd_fo WHERE id BETWEEN 60001 AND 90000;
VACUUM dd_fo;
SELECT txid_current() IS NOT NULL AS spent;
DELETE FROM dd_fo WHERE id BETWEEN 10001 AND 40000;
VACUUM dd_fo;
SELECT deleted_pages > 250 AS pages_deleted FROM lion_index_stats('dd_fo_k');
-- the map holds the upper leaves and none of the lower ones
SELECT reusable > 100 AS holds_reusable, waiting, live FROM dd_fsm('dd_fo_k');

-- As many new keys as the upper half had: they take its leaves, and the
-- index does not grow.
INSERT INTO dd_fo SELECT i, i FROM generate_series(200001, 230000) i;
SELECT pg_relation_size('dd_fo_k') / current_setting('block_size')::int = :blocks
		   AS no_growth,
	   deleted_pages < 200 AS reused
  FROM lion_index_stats('dd_fo_k');
SELECT waiting, live FROM dd_fsm('dd_fo_k');

-- The lower leaves go to the map with the next VACUUM, once a transaction has
-- started since they were deleted, and new keys take them.
SELECT txid_current() IS NOT NULL AS spent;
VACUUM dd_fo;
SELECT reusable > 100 AS holds_reusable, waiting, live FROM dd_fsm('dd_fo_k');
INSERT INTO dd_fo SELECT i, i FROM generate_series(300001, 330000) i;
SELECT pg_relation_size('dd_fo_k') / current_setting('block_size')::int = :blocks
		   AS no_growth,
	   deleted_pages < 100 AS reused
  FROM lion_index_stats('dd_fo_k');
SELECT * FROM lion_index_verify('dd_fo_k', true);
SELECT count(*) FROM dd_fo WHERE k BETWEEN 1 AND 400000;

DROP TABLE dd_fo;
DROP FUNCTION dd_fsm(regclass);
