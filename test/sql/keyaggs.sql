-- Aggregates over the entries of lion columns, weighted by their rows
-- (DESIGN.md §37).
--
-- With no WHERE and no GROUP BY every row of the table is in one entry of a
-- scalar lion column - the NULL one for a NULL - and holds its key.  So
-- sum(f(x)) is the sum over x's entries of f(key) times the entry's rows,
-- avg that over their total, min and max the first f(key) in the aggregate's
-- order: one walk of each column's entries.  When every heap page is
-- all-visible before and after the walk, and no bulk delete of the index has
-- finished in between, an entry's rows are the count its header keeps;
-- otherwise each entry is counted.  Every answer is checked against a
-- SEQUENTIAL SCAN.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

/*
 * lion_kq() runs q with the LionCount pushdown and every other scan
 * disabled, then as a sequential scan with the pushdown off, and compares the
 * two.  It says whether the node took the aggregates over keys.
 */
CREATE FUNCTION lion_kq(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	used boolean := false;
	keys boolean := false;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'Custom Scan \(LionCount\)' THEN
			used := true;
		END IF;
		IF ln ~ 'Aggregates Over Keys:' THEN
			keys := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lion_kq_on AS SELECT s::text AS r FROM (%s) s', q);

	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE format('CREATE TEMP TABLE lion_kq_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM lion_kq_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_kq_on EXCEPT ALL SELECT * FROM lion_kq_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_kq_off EXCEPT ALL SELECT * FROM lion_kq_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lion_kq_on, lion_kq_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN keys THEN 'over keys' WHEN used THEN 'counts only'
					   ELSE 'no pushdown' END, nrows);
END $$;

/* The node's own lines of EXPLAIN ANALYZE, with every other scan disabled. */
CREATE FUNCTION lion_krun(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF ln ~ '(Aggregates Over Keys|Keys Aggregated|Key Walks|Lion Indexes|LionCount)' THEN
			RETURN NEXT regexp_replace(ln, '\s+\(actual.*$', '');
		END IF;
	END LOOP;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
END $$;

/*
 * Every scalar shape: int2, int4 with NULLs, int8 whose sum passes int8's
 * range, text, date and bool, each with a lion index, and a two-column
 * index whose second column is walked.
 */
CREATE TABLE ka (s int2, i int4, b int8, t text, d date, f bool, m int4, n numeric)
	WITH (autovacuum_enabled = off);
INSERT INTO ka
SELECT (g % 17 - 8)::int2,
	   CASE WHEN g % 11 = 0 THEN NULL ELSE g % 1000 - 300 END,
	   CASE WHEN g % 3 = 0 THEN 9000000000000000000 - g ELSE g::int8 * 1000003 END,
	   CASE WHEN g % 13 = 0 THEN NULL ELSE 'k' || (g % 37) END,
	   date '2020-01-01' + (g % 400),
	   g % 5 <> 0,
	   g % 7,
	   (g % 9) / 4.0
FROM generate_series(1, 30000) g;
CREATE INDEX ka_s ON ka USING lion (s);
CREATE INDEX ka_i ON ka USING lion (i);
CREATE INDEX ka_b ON ka USING lion (b);
CREATE INDEX ka_t ON ka USING lion (t);
CREATE INDEX ka_d ON ka USING lion (d);
CREATE INDEX ka_f ON ka USING lion (f);
CREATE INDEX ka_mn ON ka USING lion (n, m);
VACUUM ANALYZE ka;

-- 1. All-visible: the entries' own counts.
SELECT lion_kq('SELECT sum(s), count(*), avg(i) FROM ka');
SELECT lion_krun('SELECT sum(s), count(*), avg(i) FROM ka');
SELECT sum(s), count(*), avg(i) FROM ka;
SELECT lion_kq('SELECT sum(i), sum(i + 1), sum(i * 2 - 7), avg(s), avg(s * 3) FROM ka');
SELECT lion_kq('SELECT sum(b), avg(b), min(b), max(b) FROM ka');
SELECT sum(b), avg(b) FROM ka;
SELECT lion_kq('SELECT min(t), max(t), min(length(t)), max(t || ''!'') FROM ka');
SELECT lion_kq('SELECT min(d), max(d + 1), max(d - date ''2020-01-01'') FROM ka');
SELECT lion_kq('SELECT bool_and(f), bool_or(f), every(i > -1000), bool_and(i < 600) FROM ka');
SELECT lion_kq('SELECT sum(m), max(m), avg(m * m) FROM ka');
SELECT lion_krun('SELECT sum(m), max(m), avg(m * m) FROM ka');
-- ... a HAVING on the one row, counts of columns besides, and the same
-- aggregate twice.
SELECT lion_kq('SELECT sum(s) FROM ka HAVING sum(s) > 0');
SELECT lion_kq('SELECT sum(s) FROM ka HAVING sum(s) < 0');
SELECT lion_kq('SELECT sum(i), count(*), sum(i) FROM ka WHERE true');
-- ... an expression that is NULL for some keys, and a CASE.
SELECT lion_kq('SELECT sum(nullif(i, 7)), max(CASE WHEN s > 0 THEN s END) FROM ka');

-- 2. A heap the visibility map does not vouch for: each entry counted.
DELETE FROM ka WHERE s = 3;
SELECT lion_kq('SELECT sum(s), count(*), avg(i), min(t), max(b) FROM ka');
SELECT lion_krun('SELECT sum(s), count(*), avg(i), min(t), max(b) FROM ka');
-- ... rows this transaction added, and one it took away.
BEGIN;
INSERT INTO ka (s, i, b, t) VALUES (100, 5000, -1, 'zz');
DELETE FROM ka WHERE i = -300;
SELECT lion_kq('SELECT sum(s), max(s), avg(i), max(t), min(b) FROM ka');
ROLLBACK;
VACUUM ka;
SELECT lion_kq('SELECT sum(s), count(*), avg(i), min(t), max(b) FROM ka');
SELECT lion_krun('SELECT sum(s), avg(i) FROM ka');

-- 3. No rows: sums, averages and extremes are NULL, counts 0.
CREATE TABLE ka0 (x int4) WITH (autovacuum_enabled = off);
CREATE INDEX ka0_x ON ka0 USING lion (x);
VACUUM ANALYZE ka0;
SELECT lion_kq('SELECT sum(x), avg(x), min(x), count(*) FROM ka0');
SELECT sum(x), avg(x), min(x), count(*) FROM ka0;
INSERT INTO ka0 VALUES (NULL), (NULL);
VACUUM ka0;
SELECT lion_kq('SELECT sum(x), avg(x), max(x), count(*) FROM ka0');
SELECT sum(x), avg(x), max(x), count(*) FROM ka0;

-- 4. Declined: a WHERE, a GROUP BY, two columns in one argument, a volatile
--    argument, DISTINCT, FILTER, ORDER BY, a column whose keys need not be
--    its values (numeric), a partial index, and the aggregates no sum of
--    keys answers.
SELECT lion_kq('SELECT sum(s) FROM ka WHERE i > 0');
SELECT lion_kq('SELECT m, sum(s) FROM ka GROUP BY m');
SELECT lion_kq('SELECT sum(s + i) FROM ka');
SELECT lion_kq('SELECT sum(s + (random() * 0)::int) FROM ka');
SELECT lion_kq('SELECT sum(DISTINCT s) FROM ka');
SELECT lion_kq('SELECT sum(s) FILTER (WHERE s > 0) FROM ka');
SELECT lion_kq('SELECT min(t ORDER BY t) FROM ka');
SELECT lion_kq('SELECT sum(n), max(n) FROM ka');
SELECT lion_kq('SELECT string_agg(t, '','' ORDER BY t) IS NOT NULL FROM ka');
SELECT lion_kq('SELECT stddev(s) FROM ka');
CREATE TABLE kap (x int4) WITH (autovacuum_enabled = off);
INSERT INTO kap SELECT g % 50 FROM generate_series(1, 1000) g;
CREATE INDEX kap_x ON kap USING lion (x) WHERE x > 10;
VACUUM ANALYZE kap;
SELECT lion_kq('SELECT sum(x) FROM kap');

-- 5. What tells the walk that VACUUM took TIDs out under it: the meta page's
--    count of finished bulk deletes (DESIGN.md §37), at byte 84 of block 0 -
--    the second word of the key counts of §33, which follow the 24-byte page
--    header and the 56 bytes of meta data.  A build writes 0.  A VACUUM that
--    finds nothing dead calls no bulk delete, and ANALYZE counts the keys
--    without touching it.  Every bulk delete adds one - the one that took out
--    an aborted insert's rows too - and the walk, which compares it before
--    and after (test/isolation/wagg_aborted_insert.spec races it), takes the
--    entries' own counts again once VACUUM has finished.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pageinspect;
RESET client_min_messages;
CREATE FUNCTION lion_kbd(idx regclass) RETURNS bigint
LANGUAGE sql AS $$
	SELECT get_byte(p, 84)::bigint | (get_byte(p, 85)::bigint << 8) |
		   (get_byte(p, 86)::bigint << 16) | (get_byte(p, 87)::bigint << 24)
	  FROM get_raw_page(idx::text, 0) p
$$;
CREATE TABLE kbd (x int4) WITH (autovacuum_enabled = off);
INSERT INTO kbd SELECT g % 40 FROM generate_series(1, 4000) g;
CREATE INDEX kbd_x ON kbd USING lion (x);
SELECT lion_kbd('kbd_x') AS after_build;
VACUUM ANALYZE kbd;
SELECT lion_kbd('kbd_x') AS after_vacuum_of_nothing_dead;
BEGIN;
INSERT INTO kbd SELECT g FROM generate_series(1, 100) g;
ROLLBACK;
VACUUM (INDEX_CLEANUP ON) kbd;
SELECT lion_kbd('kbd_x') AS after_aborted_insert_vacuumed;
DELETE FROM kbd WHERE x = 7;
VACUUM (INDEX_CLEANUP ON) kbd;
ANALYZE kbd;
SELECT lion_kbd('kbd_x') AS after_delete_vacuumed_and_analyze;
SELECT lion_kq('SELECT sum(x), max(x), avg(x), count(*) FROM kbd');
SELECT lion_krun('SELECT sum(x), max(x) FROM kbd');
DROP FUNCTION lion_kbd(regclass);

DROP FUNCTION lion_krun(text);
DROP FUNCTION lion_kq(text);
DROP TABLE ka, ka0, kap, kbd;
