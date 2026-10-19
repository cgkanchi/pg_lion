-- Page deletion in the entry directory (DESIGN.md §21, "Page deletion"):
-- VACUUM takes the leaves it empties out of the tree, with the internal pages
-- above them that have no other child, and the pages are used again.
\set VERBOSITY terse
SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS pg_lion;

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
DELETE FROM dd WHERE id BETWEEN 4000 AND 4999;
DELETE FROM dd_ref WHERE id BETWEEN 4000 AND 4999;
VACUUM dd;
SELECT dd_note('middle');
SELECT s.leaves < o.leaves AS leaves_went, s.deleted > o.deleted AS pages_deleted
  FROM dd_size s, dd_size o WHERE s.step = 'middle' AND o.step = 'oldest half';
SELECT * FROM lion_index_verify('dd_k');
SELECT * FROM dd_check();

-- ---------- 3. new keys use the pages again ----------
-- A transaction id is spent first, so that the deleted pages' safexid is
-- behind every snapshot when the inserts look for pages (nbtree's rule): the
-- index does not grow, and fewer of its pages are free.  Keys 4999 down to
-- 4900 go first, to the first leaf of a page whose range now begins further
-- left than its first downlink's key: that leaf splits again and again, and
-- each separator goes right after that downlink, also once the page is full
-- and splits itself.
SELECT txid_current() IS NOT NULL AS spent;
INSERT INTO dd SELECT i, dd_key(i) FROM generate_series(4999, 4900, -1) i;
INSERT INTO dd SELECT i, dd_key(i) FROM generate_series(6001, 8000) i;
INSERT INTO dd SELECT i, dd_key(i) FROM generate_series(4000, 4499) i;
INSERT INTO dd_ref SELECT i, dd_key(i) FROM generate_series(4999, 4900, -1) i;
INSERT INTO dd_ref SELECT i, dd_key(i) FROM generate_series(6001, 8000) i;
INSERT INTO dd_ref SELECT i, dd_key(i) FROM generate_series(4000, 4499) i;
SELECT dd_note('reinserted');
SELECT s.pages = m.pages AS no_growth, s.deleted < m.deleted AS reused
  FROM dd_size s, dd_size m WHERE s.step = 'reinserted' AND m.step = 'middle';
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
INSERT INTO dd SELECT i, dd_key(i) FROM generate_series(1, 3000) i;
INSERT INTO dd_ref SELECT i, dd_key(i) FROM generate_series(1, 3000) i;
SELECT dd_note('refilled');
SELECT s.pages = e.pages AS no_growth
  FROM dd_size s, dd_size e WHERE s.step = 'refilled' AND e.step = 'empty';
SELECT * FROM lion_index_verify('dd_k');
SELECT * FROM dd_check();

DROP TABLE dd, dd_ref, dd_size;
DROP FUNCTION dd_check(), dd_same(text), dd_key(int), dd_note(text);
