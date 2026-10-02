-- BITSET containers (DESIGN.md §3), on rows narrow enough to keep them.
--
-- A container of more than 512 members, all at heap offsets below 128, is
-- a NARROW where that is its smallest form (DESIGN.md §38), and on rows of
-- about 60 bytes or more, where a page holds fewer than 128, that is what a
-- dense set of scattered rows becomes.  Many of the other tests' tables have
-- such rows, so the BITSET arms of the set algebra and of the readers,
-- VACUUM and the inserts run only where a page holds more rows than that.
-- The table here has seven int columns, 136 rows to a page, and each
-- section says which paths it is for; lion_index_stats() shows that its
-- containers are BITSETs, RUNs and ARRAYs and no NARROW.
-- Every answer is checked against a sequential scan, as a multiset in both
-- directions.  range_cost.sql §2 has the union of a range collected on such
-- rows, where a widening is refused.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

/*
 * lbs_check() runs q with one kind of scan - 'bitmap', 'plain' (an Index
 * Scan) or 'count' (the LionCount pushdown, every other scan disabled) - and
 * again as a sequential scan with the pushdown off, and compares the two as
 * multisets.  It says whether the scan asked for was really used, and the
 * answer: its rows when there are four or fewer, else how many.
 */
CREATE FUNCTION lbs_check(q text, how text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	used boolean := false;
	want text;
	nrows bigint;
	ndiff bigint;
	answer text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', (how = 'bitmap')::text, true);
	PERFORM set_config('enable_indexscan', (how = 'plain')::text, true);
	PERFORM set_config('enable_indexonlyscan', (how = 'plain')::text, true);
	PERFORM set_config('pg_lion.enable_count_pushdown', (how = 'count')::text, true);
	want := CASE how WHEN 'bitmap' THEN 'Bitmap Index Scan'
					 WHEN 'plain' THEN 'Index (Only )?Scan using'
					 ELSE 'Custom Scan \(LionCount\)' END;
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ want THEN
			used := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lbs_on AS SELECT s::text AS r FROM (%s) s', q);

	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE format('CREATE TEMP TABLE lbs_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*), string_agg(r, '' '' ORDER BY r) FROM lbs_on'
		INTO nrows, answer;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lbs_on EXCEPT ALL SELECT * FROM lbs_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lbs_off EXCEPT ALL SELECT * FROM lbs_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lbs_on, lbs_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s%s: %s', CASE WHEN used THEN '' ELSE 'no ' END, how,
				  CASE WHEN nrows <= 4 THEN answer ELSE nrows || ' rows' END);
END $$;

-- An index's containers by kind, per key column.
CREATE FUNCTION lbs_mix(idx regclass)
RETURNS TABLE (attno int2, arrays int8, bitsets int8, runs int8, narrows int8)
LANGUAGE sql AS $$
	SELECT attno, array_containers, bitset_containers, run_containers,
		   narrow_containers
	  FROM lion_index_stats(idx)
$$;

-- ---------- 1. the table ----------
/*
 * 60,000 rows of seven int columns, 136 to a page: 442 pages, seven
 * container keys, every block holding offsets past 127.  a is every other
 * row and b every third: BITSETs at every key.  r is runs of 40 rows, four
 * values: RUNs.  s has 23 values, some 440 members a key: ARRAYs.  u has 5000
 * values of twelve rows, a member or two at a key: sparse segments, whose
 * members an AND looks up one at a time.  z is 0 in 150 rows of every 160,
 * in runs that fill a block's offsets, and one of five other values in the
 * rest: 0 a RUN, the others ARRAYs.
 */
CREATE TABLE lion_bs (id int NOT NULL, a int NOT NULL, b int NOT NULL,
					  r int NOT NULL, s int NOT NULL, u int NOT NULL,
					  z int NOT NULL)
	WITH (autovacuum_enabled = off);
INSERT INTO lion_bs
SELECT i, i % 2, i % 3, (i / 40) % 4, i % 23, i % 5000,
	   CASE WHEN i % 160 < 150 THEN 0 ELSE 1 + i % 5 END
  FROM generate_series(1, 60000) i;
CREATE INDEX lion_bs_a ON lion_bs USING lion (a);
CREATE INDEX lion_bs_b ON lion_bs USING lion (b);
CREATE INDEX lion_bs_r ON lion_bs USING lion (r);
CREATE INDEX lion_bs_s ON lion_bs USING lion (s);
CREATE INDEX lion_bs_u ON lion_bs USING lion (u);
CREATE INDEX lion_bs_z ON lion_bs USING lion (z);
VACUUM (FREEZE, ANALYZE) lion_bs;
SELECT count(*) AS rows_on_page_0 FROM lion_bs WHERE (ctid::text::point)[0] = 0;
SELECT 'a' AS col, * FROM lbs_mix('lion_bs_a') UNION ALL
SELECT 'b', * FROM lbs_mix('lion_bs_b') UNION ALL
SELECT 'r', * FROM lbs_mix('lion_bs_r') UNION ALL
SELECT 's', * FROM lbs_mix('lion_bs_s') UNION ALL
SELECT 'u', * FROM lbs_mix('lion_bs_u') UNION ALL
SELECT 'z', * FROM lbs_mix('lion_bs_z');

-- ---------- 2. the count: ANDs ----------
-- two BITSETs, in one kernel pass; a RUN and a BITSET, both ways round, the
-- RUN's gaps cleared from the BITSET's image; an ARRAY tested against a
-- BITSET; a member or two looked up in one; and chains of them
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE a = 1 AND b = 2', 'count');
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE a = 0 AND r = 2', 'count');
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE r = 1 AND b = 0', 'count');
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE a = 0 AND b = 1 AND z = 0', 'count');
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE s = 5 AND a = 1', 'count');
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE u = 77 AND b = 1', 'count');
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE s = 5 AND a = 1 AND b = 1 AND r = 3', 'count');

-- ---------- 3. the count: ORs ----------
-- IN lists of BITSETs and of RUNs, z = 0's runs set across whole words of
-- the image, and the arms of an OR, a BITSET's and a RUN's in either order
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE b IN (0, 2)', 'count');
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE r IN (0, 3) AND a = 1', 'count');
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE z IN (0, 2) AND b = 2', 'count');
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE (z = 0 OR a = 1) AND b = 2', 'count');
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE (a = 1 OR r = 2) AND b = 0', 'count');
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE (r = 2 OR a = 1) AND b = 0', 'count');
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE (a = 1 OR r = 2) AND s = 4', 'count');
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE (b = 1 OR r = 0) AND (a = 0 OR z = 3)', 'count');
-- an IN list or a nested OR beside an AND of BITSETs, which probes it for
-- the AND's members, or builds it; and BITSETs probed for the members of an
-- ARRAY and of a sparse segment
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE a = 1 AND b = 0 AND z IN (1, 2, 3)', 'count');
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE a = 1 AND b = 0 AND r = 1 AND s IN (1, 2, 3)', 'count');
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE a = 1 AND b = 0 AND (s = 1 OR (r = 2 AND z = 0))', 'count');
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE s = 9 AND b IN (0, 1)', 'count');
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE u = 4321 AND a IN (0, 1) AND b IN (1, 2)', 'count');

-- ---------- 4. the count: `<>` ----------
-- With nothing else to select rows, every row less z = 0's entry, a RUN
-- cleared from the image of b's union, or less a BITSET; an ARRAY, a RUN and
-- a BITSET less a BITSET; a BITSET less a RUN
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE z <> 0', 'count');
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE b <> 1', 'count');
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE s = 11 AND a <> 0', 'count');
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE r = 1 AND a <> 1', 'count');
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE z = 0 AND a <> 1', 'count');
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE b = 2 AND z <> 0', 'count');
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE a <> 1 AND r <> 0', 'count');

-- ---------- 5. the count: GROUP BY ----------
-- groups of BITSETs, RUNs and ARRAYs, counted against a WHERE collected
-- into BITSETs, which they are tested against as they are; two columns
-- decoded against it; and count(DISTINCT)
SELECT lbs_check('SELECT b, count(*) FROM lion_bs WHERE a = 1 GROUP BY b', 'count');
SELECT lbs_check('SELECT r, count(*) FROM lion_bs WHERE a = 0 AND b <> 2 GROUP BY r', 'count');
SELECT lbs_check('SELECT s, count(*) FROM lion_bs WHERE b IN (0, 2) GROUP BY s', 'count');
SELECT lbs_check('SELECT b, r, count(*) FROM lion_bs WHERE a = 1 GROUP BY b, r', 'count');
SELECT lbs_check('SELECT count(DISTINCT s) FROM lion_bs WHERE a = 1 AND b = 0', 'count');

-- ---------- 6. bitmap and plain scans ----------
-- a BITSET's members handed out; ANDs, ORs and `<>` of BITSETs and RUNs;
-- an IN list of RUNs, and of BITSETs, unioned for a plain scan
SELECT lbs_check('SELECT id FROM lion_bs WHERE a = 1', 'bitmap');
SELECT lbs_check('SELECT id FROM lion_bs WHERE b = 2', 'plain');
SELECT lbs_check('SELECT id FROM lion_bs WHERE a = 0 AND b = 1', 'bitmap');
SELECT lbs_check('SELECT id FROM lion_bs WHERE a = 1 AND r = 3', 'bitmap');
SELECT lbs_check('SELECT id FROM lion_bs WHERE b IN (1, 2) AND s = 7', 'bitmap');
SELECT lbs_check('SELECT id FROM lion_bs WHERE s BETWEEN 3 AND 5 AND a = 1', 'bitmap');
SELECT lbs_check('SELECT id FROM lion_bs WHERE a = 1 OR r = 1', 'bitmap');
SELECT lbs_check('SELECT id FROM lion_bs WHERE z <> 0', 'bitmap');
SELECT lbs_check('SELECT id FROM lion_bs WHERE b <> 0 AND a = 1', 'plain');
SELECT lbs_check('SELECT id FROM lion_bs WHERE r IN (1, 2)', 'plain');
SELECT lbs_check('SELECT id FROM lion_bs WHERE b IN (0, 2) AND z IN (0, 4)', 'plain');

-- ---------- 7. inserts ----------
-- 10,000 rows more: the last container key's BITSETs take their members in
-- place, and at the key after it s = 0, every other row, is an ARRAY that
-- the inserts take past 2048 members, into a BITSET.
INSERT INTO lion_bs
SELECT i, i % 2, i % 3, (i / 40) % 4, CASE WHEN i % 2 = 0 THEN 0 ELSE i % 23 END,
	   i % 5000, CASE WHEN i % 160 < 150 THEN 0 ELSE 1 + i % 5 END
  FROM generate_series(60001, 70000) i;
SELECT 'a' AS col, * FROM lbs_mix('lion_bs_a') UNION ALL
SELECT 's', * FROM lbs_mix('lion_bs_s');
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE a = 1 AND b = 2', 'count');
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE s = 0 AND z <> 0', 'count');
SELECT lbs_check('SELECT id FROM lion_bs WHERE s = 0 AND a = 0', 'bitmap');

-- ---------- 8. a dirty heap, VACUUM and verify() ----------
-- The counts recheck the rows of the pages the map cannot vouch for.
-- VACUUM takes the dead rows out of the BITSETs, and one thinned to 2048
-- members or fewer is an ARRAY again: a = 1 keeps a fifth of its rows.
UPDATE lion_bs SET z = 1 WHERE id % 7 = 0;
DELETE FROM lion_bs WHERE id % 10 IN (1, 3, 5, 7);
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE a = 1 AND b = 2', 'count');
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE z <> 0', 'count');
SELECT lbs_check('SELECT b, count(*) FROM lion_bs WHERE a = 0 GROUP BY b', 'count');
VACUUM (FREEZE, ANALYZE) lion_bs;
SELECT 'a' AS col, * FROM lbs_mix('lion_bs_a') UNION ALL
SELECT 'b', * FROM lbs_mix('lion_bs_b');
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE a = 1 AND b = 2', 'count');
SELECT lbs_check('SELECT count(*) FROM lion_bs WHERE z <> 0', 'count');
SELECT lbs_check('SELECT id FROM lion_bs WHERE a = 0 AND b = 1', 'bitmap');
SELECT lion_index_verify('lion_bs_a', true);
SELECT lion_index_verify('lion_bs_b', true);
SELECT lion_index_verify('lion_bs_z', true);

DROP TABLE lion_bs;
DROP FUNCTION lbs_check(text, text), lbs_mix(regclass);
