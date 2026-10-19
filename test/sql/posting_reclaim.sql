-- Empty leaves of a live key's posting set (DESIGN.md §22, "Empty leaves"):
-- VACUUM takes the leaves it empties out of the set while the key keeps rows,
-- and the pages are used again.
\set VERBOSITY terse
SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS pg_lion;

/*
 * Four keys, every row of the table in one of them.  Inserted after the
 * index exists, 60,000 rows to a key fill some seventeen leaves of it, half
 * full as appends leave them, and each key's set is a tree of one internal
 * level.  pr_ref holds the same rows and no index: it is what every answer is
 * compared with.
 */
CREATE TABLE pr (id int, k int) WITH (autovacuum_enabled = off);
CREATE TABLE pr_ref (id int, k int) WITH (autovacuum_enabled = off);
CREATE INDEX pr_k ON pr USING lion (k);
INSERT INTO pr SELECT g, g % 4 FROM generate_series(1, 240000) g;
INSERT INTO pr_ref SELECT * FROM pr;

/* q's answer on pr through the index, and on pr_ref, and whether they agree. */
CREATE FUNCTION pr_same(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	a text;
	b text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE format('SELECT string_agg(x::text, %L) FROM (%s) s(x)', ',',
				   format(q, 'pr')) INTO a;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE format('SELECT string_agg(x::text, %L) FROM (%s) s(x)', ',',
				   format(q, 'pr_ref')) INTO b;
	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH index=%s reference=%s', a, b);
	END IF;
	RETURN 'ok ' || coalesce(left(a, 40), '<none>');
END $$;

-- Every key's count through a bitmap scan and through an index scan, and the
-- first and last rows of one key in TID order.
CREATE FUNCTION pr_check() RETURNS SETOF text
LANGUAGE sql AS $$
	SELECT pr_same('SELECT k || '':'' || count(*) FROM %I WHERE k IN (0, 1, 2, 3) GROUP BY k ORDER BY k')
	UNION ALL SELECT pr_same('SELECT count(*) FROM %I WHERE k = 1')
	UNION ALL SELECT pr_same('SELECT count(*) FROM %I WHERE k = 2 AND id > 0')
	UNION ALL SELECT pr_same('SELECT min(id) FROM %I WHERE k = 3')
	UNION ALL SELECT pr_same('SELECT max(id) FROM %I WHERE k = 3')
$$;

SELECT container_pages AS leaves, posting_internal_pages, max_posting_height,
	   deleted_pages
  FROM lion_index_stats('pr_k');
SELECT pg_relation_size('pr_k') / 8192 AS blocks \gset
SELECT * FROM pr_check();

-- 1. The oldest three quarters of every key go.  VACUUM empties the leaves
--    that held them, which are each set's lowest, and takes every one of them
--    out: each has a right sibling whose downlink is the next one on the
--    root, the set's only internal page.
DELETE FROM pr WHERE id <= 180000;
DELETE FROM pr_ref WHERE id <= 180000;
VACUUM pr;
SELECT container_pages AS leaves, posting_internal_pages, max_posting_height,
	   deleted_pages
  FROM lion_index_stats('pr_k');
SELECT lion_index_verify('pr_k', true);
SELECT * FROM pr_check();

-- 2. New rows of every key, as many as went: the pages are used again and
--    the index does not grow.  The VACUUM that took them out could not hand
--    them out yet - no transaction had started since - and the next VACUUM
--    does, although it has no row to remove (DESIGN.md §18, "Reuse").
SELECT txid_current() > 0 AS xid;
VACUUM pr;
INSERT INTO pr SELECT g, g % 4 FROM generate_series(240001, 420000) g;
INSERT INTO pr_ref SELECT g, g % 4 FROM generate_series(240001, 420000) g;
SELECT pg_relation_size('pr_k') / 8192 <= :blocks AS not_grown,
	   deleted_pages < 40 AS reused
  FROM lion_index_stats('pr_k');
SELECT lion_index_verify('pr_k', true);
SELECT * FROM pr_check();

-- 3. A run in the middle of every key goes: its leaves leave the set between
--    two that stay, and their key range goes to the leaf right of them.  Rows
--    inserted into that range again are found.
DELETE FROM pr WHERE id BETWEEN 250000 AND 330000;
DELETE FROM pr_ref WHERE id BETWEEN 250000 AND 330000;
VACUUM pr;
SELECT deleted_pages >= 20 AS unlinked FROM lion_index_stats('pr_k');
SELECT lion_index_verify('pr_k', true);
SELECT * FROM pr_check();
-- The heap's free space puts these rows at the TIDs the deleted ones had.
INSERT INTO pr SELECT g, g % 4 FROM generate_series(500001, 510000) g;
INSERT INTO pr_ref SELECT g, g % 4 FROM generate_series(500001, 510000) g;
SELECT lion_index_verify('pr_k', true);
SELECT * FROM pr_check();

-- 4. A set without an internal level (DESIGN.md §22): its leaves are a
--    chain from the head, which stays, as does the last leaf.  The leaves in
--    between that VACUUM empties leave the chain.
CREATE TABLE pf (id int, k int) WITH (autovacuum_enabled = off);
CREATE INDEX pf_k ON pf USING lion (k);
INSERT INTO pf SELECT g, g % 4 FROM generate_series(1, 60000) g;
SELECT container_pages AS leaves, posting_internal_pages
  FROM lion_index_stats('pf_k');
DELETE FROM pf WHERE id BETWEEN 15000 AND 45000;
VACUUM pf;
SELECT container_pages AS leaves, posting_internal_pages,
	   deleted_pages > 0 AS unlinked
  FROM lion_index_stats('pf_k');
SELECT lion_index_verify('pf_k', true);
SET enable_seqscan = off;
SET pg_lion.enable_count_pushdown = off;
SELECT k, count(*) FROM pf WHERE k IN (0, 1, 2, 3) GROUP BY k ORDER BY k;
RESET enable_seqscan;
RESET pg_lion.enable_count_pushdown;

-- 5. Every row of a key goes: the whole set goes with its entry, and nothing
--    is taken out of it first.
DELETE FROM pr WHERE k = 0;
DELETE FROM pr_ref WHERE k = 0;
VACUUM pr;
SELECT lion_index_verify('pr_k', true);
SELECT * FROM pr_check();

DROP TABLE pr, pr_ref, pf;
DROP FUNCTION pr_check();
DROP FUNCTION pr_same(text);
