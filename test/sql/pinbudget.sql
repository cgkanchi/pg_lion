-- The buffer pins an IN list holds (2026-09-23 review, third round).
--
-- An INLINE entry's posting set is copied out of its directory leaf and the
-- leaf stays PINNED until the count has asked the visibility map about the
-- set's heap blocks (DESIGN.md §9).  An IN list therefore held one pin per
-- distinct leaf its values' entries live on, with no bound: a list whose
-- length is not known at plan time - an array parameter, an InitPlan - over
-- an index with more leaves than shared_buffers failed with "no unpinned
-- buffers available", and one short of that starved every other backend.
-- The lookup now keeps pins on at most a budget of leaves - this backend's
-- share of shared_buffers, and never more than 1000 - and the sets beyond it
-- hold none (DESIGN.md §15).  Every answer below must still be exact.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
CREATE EXTENSION IF NOT EXISTS pg_buffercache;
RESET client_min_messages;
/* VACUUM can only set all-visible for commits that reached disk (citext.sql). */
SET synchronous_commit = on;
/*
 * Keys of about 1.9 kB put one or two entries on a leaf, so 3000 keys are
 * spread over some 1500 leaves; x is a second key column and a
 * second index for the merge and the GROUP BY driver.
 */
CREATE TABLE lion_pin (k text NOT NULL, x int NOT NULL, y int NOT NULL);
INSERT INTO lion_pin SELECT repeat(md5(i::text), 59) || i, i % 2, i % 1000 FROM generate_series(1, 3000) i;
CREATE INDEX lion_pin_k ON lion_pin USING lion (k);
CREATE INDEX lion_pin_x ON lion_pin USING lion (x);
CREATE INDEX lion_pin_y ON lion_pin USING lion (y);
CREATE INDEX lion_pin_kx ON lion_pin USING lion (k, x);
VACUUM ANALYZE lion_pin;
SELECT pg_relation_size('lion_pin_k') / current_setting('block_size')::int > 1200 AS many_leaves;
/*
 * How many of an index's pages some backend pins right now: this one's pins
 * included, which is the point - a GROUP BY count keeps its WHERE sets
 * located, and their pins held, from its first group to its last, so a
 * cursor stopped after the first group shows what the lookup kept.
 */
CREATE FUNCTION lion_pinned(idx regclass) RETURNS bigint
LANGUAGE sql AS $$
	SELECT count(*) FROM pg_buffercache
	 WHERE reldatabase = (SELECT oid FROM pg_database WHERE datname = current_database())
	   AND relfilenode = pg_relation_filenode(idx)
	   AND pinning_backends > 0
$$;
-- ---------- the pushdown's array-parameter path ----------
BEGIN;
DECLARE lion_pin_c CURSOR FOR
	SELECT x, count(*) FROM lion_pin
	 WHERE k = ANY ((SELECT array_agg(k) FROM lion_pin)::text[]) GROUP BY x ORDER BY x;
FETCH 1 FROM lion_pin_c;
/* the list's leaves, plus the one or two the GROUP BY's entry scan holds */
SELECT lion_pinned('lion_pin_k') + lion_pinned('lion_pin_kx') BETWEEN 1 AND 1002 AS pins_bounded;
FETCH 1 FROM lion_pin_c;
COMMIT;
SELECT lion_pinned('lion_pin_k') + lion_pinned('lion_pin_kx') AS pins_after;
-- ---------- every path over a list longer than the budget is still exact ----------
-- the SQL function (the disjoint sum: the sets beyond the budget are located
-- again one at a time, under a pin of their own)
SELECT lion_index_count_any('lion_pin_k', (SELECT array_agg(k) FROM lion_pin)) AS count_any;
SELECT lion_index_count_any('lion_pin_k', (SELECT array_agg(k) FROM lion_pin WHERE x = 1)) AS count_any_half;
-- the pushdown: the list alone, ANDed with another source, and as GROUP BY driver
SELECT count(*) FROM lion_pin WHERE k = ANY ((SELECT array_agg(k) FROM lion_pin)::text[]);
SELECT count(*) FROM lion_pin WHERE k = ANY ((SELECT array_agg(k) FROM lion_pin)::text[]) AND x = 1;
SELECT count(*), sum(n) FROM (
	SELECT k, count(*) AS n FROM lion_pin
	 WHERE k = ANY ((SELECT array_agg(k) FROM lion_pin)::text[]) GROUP BY k) s;
SELECT count(*) FROM lion_pin WHERE k = ANY ((SELECT array_agg(k) FROM lion_pin WHERE x = 0)::text[]) OR x = 1;
/*
 * ... and where the answer comes from.  The heap is all-visible, so a count
 * that keeps the interlock answers every row from the visibility map: the
 * sum does (every set past the budget is located again, pinned), the AND
 * does (x's set carries the interlock at every container key), the GROUP BY
 * does (each group is one set, located again).  The OR across columns has
 * nothing to carry it for the sets past the budget and rechecks every row.
 */
CREATE FUNCTION lion_pinsrc(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ 'Heap Blocks Skipped via VM|Heap TIDs Rechecked|Custom Scan' THEN
			RETURN NEXT regexp_replace(l, ' *\(actual.*$', '');
		END IF;
	END LOOP;
END
$$;
SELECT lion_pinsrc('SELECT count(*) FROM lion_pin WHERE k = ANY ((SELECT array_agg(k) FROM lion_pin)::text[])');
SELECT lion_pinsrc('SELECT count(*) FROM lion_pin WHERE k = ANY ((SELECT array_agg(k) FROM lion_pin)::text[]) AND x = 1');
SELECT lion_pinsrc('SELECT k, count(*) FROM lion_pin WHERE k = ANY ((SELECT array_agg(k) FROM lion_pin)::text[]) GROUP BY k');
SELECT lion_pinsrc('SELECT count(*) FROM lion_pin WHERE k = ANY ((SELECT array_agg(k) FROM lion_pin WHERE x = 0)::text[]) OR x = 1');
/*
 * ... while what fits in the budget keeps the map (the budget's first
 * version, the backend's fair share of the pool - 86 buffers on a stock
 * server - sent both of these to the heap, and single lookups too): a list of
 * a hundred values under the same OR, and a plain AND of two INLINE sets.
 */
SELECT lion_pinsrc('SELECT count(*) FROM lion_pin WHERE k = ANY ((SELECT array_agg(k) FROM (SELECT k FROM lion_pin ORDER BY k LIMIT 100) s)::text[]) OR x = 1');
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT lion_pinsrc($q$SELECT count(*) FROM lion_pin WHERE k = repeat(md5('7'), 59) || '7' AND y = 7$q$);
RESET enable_seqscan;
RESET enable_bitmapscan;
DROP FUNCTION lion_pinsrc(text);
-- the multicolumn bitmap scan
SET pg_lion.enable_count_pushdown = off;
SET enable_seqscan = off;
SELECT count(*) FROM lion_pin WHERE k = ANY ((SELECT array_agg(k) FROM lion_pin)::text[]) AND x = 1;
RESET enable_seqscan;
RESET pg_lion.enable_count_pushdown;
-- ... and with a dirty heap, where the answer has to come from the heap too
UPDATE lion_pin SET x = 1 - x WHERE right(k, 1) = '7';
DELETE FROM lion_pin WHERE right(k, 1) = '3';
SELECT lion_index_count_any('lion_pin_k', (SELECT array_agg(k) FROM lion_pin)) AS count_any_dirty,
	   (SELECT count(*) FROM lion_pin) AS expected;
SELECT count(*) FROM lion_pin WHERE k = ANY ((SELECT array_agg(k) FROM lion_pin)::text[]) AND x = 1;
SET pg_lion.enable_count_pushdown = off;
SELECT count(*) FROM lion_pin WHERE k = ANY ((SELECT array_agg(k) FROM lion_pin)::text[]) AND x = 1;
RESET pg_lion.enable_count_pushdown;
DROP TABLE lion_pin;
DROP FUNCTION lion_pinned(regclass);
