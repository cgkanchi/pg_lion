-- Plain index scans: amgettuple (DESIGN.md §29).
--
-- A lion index answers a plain Index Scan by streaming the bitmap path's own
-- set algebra one container at a time.  Every answer below is checked
-- against a SEQUENTIAL SCAN, as a multiset in both directions, and the plan
-- is checked to really be an Index Scan on a lion index.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
CREATE EXTENSION IF NOT EXISTS citext;
CREATE EXTENSION IF NOT EXISTS pg_lion_citext;
RESET client_min_messages;
SET synchronous_commit = on;
SET default_statistics_target = 1000;
SET max_parallel_workers_per_gather = 0;

/*
 * lion_iq() runs a query as a PLAIN INDEX SCAN - sequential, bitmap and
 * index-only scans disabled, the count pushdown off - and again as a
 * sequential scan, compares the two, and names the lion index the plan
 * scanned (or says there was none: a collation the index cannot answer).
 */
CREATE FUNCTION lion_iq(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	used text := NULL;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'Index Scan using lis' AND used IS NULL THEN
			used := substring(ln from 'Index Scan using (\S+)');
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lion_iq_on AS SELECT s::text AS r FROM (%s) s', q);

	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	EXECUTE format('CREATE TEMP TABLE lion_iq_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM lion_iq_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_iq_on EXCEPT ALL SELECT * FROM lion_iq_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_iq_off EXCEPT ALL SELECT * FROM lion_iq_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lion_iq_on, lion_iq_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN used IS NULL THEN 'no index scan'
					   ELSE 'index scan on ' || used END, nrows);
END $$;

/*
 * lion_ir() runs a plain index scan under EXPLAIN ANALYZE and reports the
 * index it used and how many rows the heap recheck of the index quals threw
 * away - which is only ever non-zero when the scan set xs_recheck.
 */
CREATE FUNCTION lion_ir(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF ln ~ 'Index Scan using' THEN
			RETURN NEXT 'Index Scan using ' || substring(ln from 'Index Scan using (\S+)');
		ELSIF ln ~ 'Rows Removed by Index Recheck' THEN
			RETURN NEXT btrim(ln);
		END IF;
	END LOOP;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
END $$;

/*
 * lion_ios() runs a query that needs no column as an INDEX-ONLY scan on a lion
 * index (DESIGN.md §29.9: lion returns no column, so this is the only kind the
 * planner can build) and compares it with a sequential scan.
 */
CREATE FUNCTION lion_ios(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	used text := NULL;
	a text;
	b text;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ 'Index Only Scan using lis' AND used IS NULL THEN
			used := substring(ln from 'Index Only Scan using (\S+)');
		END IF;
	END LOOP;
	EXECUTE q INTO a;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	EXECUTE q INTO b;
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	IF a IS DISTINCT FROM b THEN
		RETURN format('MISMATCH: %s against %s', a, b);
	END IF;
	RETURN format('%s: %s', coalesce('index-only scan on ' || used, 'no index-only scan'), a);
END $$;

/* The first line of a plan with nothing disabled. */
CREATE FUNCTION lion_top(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	res text := '';
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln ~ '(Scan|LionCount)' THEN
			res := res || CASE WHEN res = '' THEN '' ELSE ' / ' END ||
				   btrim(regexp_replace(ln, '->', ''));
		END IF;
	END LOOP;
	RETURN res;
END $$;

-- ---------- 1. the predicate kinds, each as a forced plain index scan ----------
CREATE TABLE lis (
	id		int NOT NULL,
	k		int NOT NULL,
	kk		int NOT NULL,		-- k again, for the multicolumn index
	u		int NOT NULL,
	g		int NOT NULL,
	n		int,
	t		text NOT NULL,
	ci		citext NOT NULL,
	tags	int[],
	tsv		tsvector,
	flag	bool NOT NULL,
	flag2	bool NOT NULL,
	pad		text
);
INSERT INTO lis
SELECT i, (i * 7919) % 200, (i * 7919) % 200, i, i % 10,
	   CASE WHEN i % 7 = 0 THEN NULL ELSE i % 50 END,
	   'v' || (i % 50),
	   CASE i % 3 WHEN 0 THEN 'alice' WHEN 1 THEN 'ALICE' ELSE 'Bob' || (i % 5) END,
	   CASE WHEN i % 11 = 0 THEN NULL
			WHEN i % 13 = 0 THEN '{}'::int[]
			ELSE ARRAY[i % 10, 10 + i % 17, 40 + i % 23] END,
	   CASE WHEN i % 11 = 0 THEN NULL
			ELSE to_tsvector('simple', 'w' || (i % 10) || ' x' || (i % 7)) END,
	   i % 97 = 0, i % 89 = 0,
	   repeat('p', 20)
  FROM generate_series(1, 30000) i;
CREATE INDEX lis_k ON lis USING lion (k);
CREATE INDEX lis_u ON lis USING lion (u);
CREATE INDEX lis_n ON lis USING lion (n);
CREATE INDEX lis_t ON lis USING lion (t);
CREATE INDEX lis_ci ON lis USING lion (ci);
CREATE INDEX lis_tags ON lis USING lion (tags);
CREATE INDEX lis_tsv ON lis USING lion (tsv);
CREATE INDEX lis_gk ON lis USING lion (g, kk);
CREATE INDEX lis_part ON lis USING lion (g) WHERE flag;
CREATE INDEX lis_tpart ON lis USING lion (tags) WHERE flag2;
ANALYZE lis;

-- equality, and a key that is not there
SELECT lion_iq('SELECT id FROM lis WHERE k = 17');
SELECT lion_iq('SELECT id FROM lis WHERE u = 12345');
SELECT lion_iq('SELECT id FROM lis WHERE u = -1');
-- IN lists and = ANY, NULL elements, duplicates, nothing left
SELECT lion_iq('SELECT id FROM lis WHERE k IN (1, 2, 3, 150)');
SELECT lion_iq('SELECT id FROM lis WHERE k = ANY (''{5, NULL, 7, 5}''::int[])');
SELECT lion_iq('SELECT id FROM lis WHERE k = ANY (''{NULL}''::int[])');
SELECT lion_iq('SELECT id FROM lis WHERE k = ANY (''{}''::int[])');
SELECT lion_iq('SELECT id FROM lis WHERE u IN (1, 29999, 30000, 30001)');
-- ranges: one walk, bounded both ways, open-ended, empty, and against an array
SELECT lion_iq('SELECT id FROM lis WHERE k BETWEEN 10 AND 20');
SELECT lion_iq('SELECT id FROM lis WHERE k > 190');
SELECT lion_iq('SELECT id FROM lis WHERE k < 3');
SELECT lion_iq('SELECT id FROM lis WHERE k > 5 AND k < 5');
SELECT lion_iq('SELECT id FROM lis WHERE u >= 29990');
SELECT lion_iq('SELECT id FROM lis WHERE k < ANY (''{5, 10, NULL}''::int[])');
SELECT lion_iq('SELECT id FROM lis WHERE k > ANY (''{195, 190}''::int[])');
-- NULL tests
SELECT lion_iq('SELECT id FROM lis WHERE n IS NULL');
SELECT lion_iq('SELECT id FROM lis WHERE n IS NOT NULL');
SELECT lion_iq('SELECT id FROM lis WHERE n = ANY (''{3, NULL}''::int[])');
-- two quals on one column: one answered, the other rechecked
SELECT lion_iq('SELECT id FROM lis WHERE k IN (17, 18) AND k IN (18, 19)');
SELECT lion_iq('SELECT id FROM lis WHERE k = 17 AND k > 10');
SELECT lion_ir('SELECT id FROM lis WHERE k IN (17, 18) AND k IN (18, 19)');
-- a multicolumn index: sets ANDed, a range walked with the other column's sets,
-- two ranges (one walked, one rechecked), and the second column alone
SELECT lion_iq('SELECT id FROM lis WHERE g = 3 AND kk = 17');
SELECT lion_iq('SELECT id FROM lis WHERE g IN (1, 2) AND kk IN (9, 18, 28, 19, 5)');
SELECT lion_iq('SELECT id FROM lis WHERE g = 3 AND kk BETWEEN 10 AND 60');
SELECT lion_iq('SELECT id FROM lis WHERE g BETWEEN 1 AND 3 AND kk BETWEEN 10 AND 60');
SELECT lion_ir('SELECT id FROM lis WHERE g BETWEEN 1 AND 3 AND kk BETWEEN 10 AND 60');
SELECT lion_iq('SELECT id FROM lis WHERE g IN (1, 2) AND kk BETWEEN 190 AND 200');
SELECT lion_iq('SELECT id FROM lis WHERE kk = 5');
-- cross-type keys (the family's int4/int8 members) and a binary-coerced varchar
SELECT lion_iq('SELECT id FROM lis WHERE k = 17::int8');
SELECT lion_iq('SELECT id FROM lis WHERE k = ANY (''{17, 18, 5000000000}''::int8[])');
SELECT lion_iq('SELECT id FROM lis WHERE k < 5000000000::int8');
SELECT lion_iq('SELECT id FROM lis WHERE k > 5000000000::int8');
SELECT lion_iq('SELECT id FROM lis WHERE t = ''v7''::varchar');
-- citext: one entry for both spellings
SELECT lion_iq('SELECT id FROM lis WHERE ci = ''Alice''');
SELECT lion_iq('SELECT id FROM lis WHERE ci IN (''BOB1'', ''bob2'')');
-- a collation the index was not built under is not answered by it
SELECT lion_iq('SELECT id FROM lis WHERE t = ''v7'' COLLATE "POSIX"');
-- arrays: exact queries (rechecked all the same) and ones that need every row
SELECT lion_iq('SELECT id FROM lis WHERE tags @> ''{3}''');
SELECT lion_iq('SELECT id FROM lis WHERE tags @> ''{3, 13}''');
SELECT lion_iq('SELECT id FROM lis WHERE tags && ''{3, 50}''');
SELECT lion_iq('SELECT id FROM lis WHERE tags <@ ''{1, 11, 41, 2, 12, 42}''');
SELECT lion_iq('SELECT id FROM lis WHERE tags @> ''{}''');
SELECT lion_iq('SELECT id FROM lis WHERE tags IS NULL');
SELECT lion_iq('SELECT id FROM lis WHERE tags IS NOT NULL');
SELECT lion_ir('SELECT id FROM lis WHERE tags <@ ''{1, 11, 41, 2, 12, 42}''');
-- tsvector: a tree of lexemes, and a NOT, which needs every row
SELECT lion_iq('SELECT id FROM lis WHERE tsv @@ ''w3 & x2''::tsquery');
SELECT lion_iq('SELECT id FROM lis WHERE tsv @@ ''w3 | x5''::tsquery');
SELECT lion_iq('SELECT id FROM lis WHERE tsv @@ ''w3 & !x2''::tsquery');
SELECT lion_ir('SELECT id FROM lis WHERE tsv @@ ''w3 & !x2''::tsquery');
-- partial indexes answered with no key at all: a scalar column and a multi-key one
SELECT lion_iq('SELECT id FROM lis WHERE flag');
SELECT lion_iq('SELECT id FROM lis WHERE flag2');
-- a query that needs no column at all can be an index-only scan: every row of
-- a column's walk (NULLs included), or of a multi-key column's bitmap
SELECT lion_ios('SELECT count(*) FROM lis');
SELECT lion_ios('SELECT count(*) FROM lis WHERE flag');
SELECT lion_ios('SELECT count(*) FROM lis WHERE flag2');

-- ---------- 2. nested-loop inner index scans, rescanned per outer row ----------
CREATE TABLE lis_outer (x int, arr int[]);
INSERT INTO lis_outer SELECT i * 97, ARRAY[i % 200, (i * 3) % 200] FROM generate_series(1, 60) i;
ANALYZE lis_outer;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_material = off;
SELECT lion_iq('SELECT o.x, l.id FROM lis_outer o JOIN lis l ON l.u = o.x');
SELECT lion_iq('SELECT o.x, l.id FROM lis_outer o JOIN lis l ON l.k = ANY (o.arr)');
SELECT lion_iq('SELECT o.x, l.id FROM lis_outer o JOIN lis l ON l.u BETWEEN o.x AND o.x + 5');
SELECT lion_iq('SELECT o.x, l.id FROM lis_outer o JOIN lis l ON l.g = o.x % 10 AND l.kk = o.x % 200');
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_material;

-- ---------- 3. cursors ----------
SET pg_lion.enable_count_pushdown = off;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
BEGIN;
EXPLAIN (COSTS OFF) DECLARE c CURSOR FOR SELECT id FROM lis WHERE k = 17;
DECLARE c CURSOR FOR SELECT id FROM lis WHERE k = 17;
FETCH 3 FROM c;
FETCH 2 FROM c;
MOVE FORWARD 100 IN c;
FETCH 2 FROM c;
FETCH BACKWARD 1 FROM c;		-- a NO SCROLL cursor refuses
ROLLBACK;
BEGIN;
-- a SCROLL cursor gets a Material node: lion cannot scan backward
EXPLAIN (COSTS OFF) DECLARE s SCROLL CURSOR FOR SELECT id FROM lis WHERE k = 17;
DECLARE s SCROLL CURSOR FOR SELECT id FROM lis WHERE k = 17;
FETCH 3 FROM s;
FETCH BACKWARD 2 FROM s;
FETCH ABSOLUTE 5 FROM s;
FETCH LAST FROM s;
FETCH PRIOR FROM s;
COMMIT;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET pg_lion.enable_count_pushdown;

-- ---------- 4. a dirty heap: updates and deletes, then VACUUM ----------
UPDATE lis SET k = (k + 1) % 200, kk = (kk + 1) % 200, tags = tags || 99 WHERE id % 10 = 0;
DELETE FROM lis WHERE id % 13 = 0;
UPDATE lis SET n = NULL WHERE id % 17 = 0;
SELECT lion_iq('SELECT id FROM lis WHERE k = 17');
SELECT lion_iq('SELECT id FROM lis WHERE k IN (17, 18, 19)');
SELECT lion_iq('SELECT id FROM lis WHERE k BETWEEN 10 AND 20');
SELECT lion_iq('SELECT id FROM lis WHERE n IS NULL');
SELECT lion_iq('SELECT id FROM lis WHERE g = 3 AND kk BETWEEN 10 AND 60');
SELECT lion_iq('SELECT id FROM lis WHERE tags @> ''{99}''');
SELECT lion_ios('SELECT count(*) FROM lis WHERE flag');
SELECT lion_ios('SELECT count(*) FROM lis WHERE flag2');
VACUUM lis;
SELECT lion_iq('SELECT id FROM lis WHERE k = 17');
SELECT lion_iq('SELECT id FROM lis WHERE k IN (17, 18, 19)');
SELECT lion_iq('SELECT id FROM lis WHERE k BETWEEN 10 AND 20');
SELECT lion_iq('SELECT id FROM lis WHERE n IS NULL');
SELECT lion_iq('SELECT id FROM lis WHERE g = 3 AND kk BETWEEN 10 AND 60');
SELECT lion_iq('SELECT id FROM lis WHERE tags @> ''{99}''');
SELECT lion_ios('SELECT count(*) FROM lis WHERE flag');
SELECT lion_ios('SELECT count(*) FROM lis WHERE flag2');
SELECT lion_index_verify('lis_k', true);

-- ---------- 5. a posting set far larger than one batch, streamed ----------
-- 150k narrow rows over three keys: every key's posting set is a posting
-- tree of many containers.  A cursor walks one of them; the scan's memory is
-- the same after a thousand rows as after forty-one thousand.
CREATE TABLE lis_big (id int, b int);
INSERT INTO lis_big SELECT i, i % 3 FROM generate_series(1, 150000) i;
CREATE INDEX lis_big_b ON lis_big USING lion (b);
ANALYZE lis_big;
SELECT container_pages > 3 AS is_a_posting_tree FROM lion_index_stats('lis_big_b');
BEGIN;
SET LOCAL pg_lion.enable_count_pushdown = off;
SET LOCAL enable_seqscan = off;
SET LOCAL enable_bitmapscan = off;
EXPLAIN (COSTS OFF) DECLARE big CURSOR FOR SELECT id FROM lis_big WHERE b = 1;
DECLARE big CURSOR FOR SELECT id FROM lis_big WHERE b = 1;
MOVE FORWARD 1000 IN big;
CREATE TEMP TABLE lis_mem AS
SELECT sum(total_bytes) AS bytes FROM pg_backend_memory_contexts
 WHERE name LIKE 'lion index scan%';
MOVE FORWARD 40000 IN big;
SELECT count(*) > 0 AS scan_contexts,
	   sum(total_bytes) <= (SELECT bytes FROM lis_mem) AS no_growth,
	   sum(total_bytes) < 1024 * 1024 AS under_1mb
  FROM pg_backend_memory_contexts WHERE name LIKE 'lion index scan%';
MOVE FORWARD ALL IN big;
COMMIT;
DROP TABLE lis_mem;
SELECT lion_iq('SELECT count(*), sum(id) FROM lis_big WHERE b = 1');

-- ---------- 6. plan choice with nothing disabled ----------
-- Narrow rows, many to a page: a one-row lookup goes to the plain scan; a
-- key of a few hundred scattered rows goes to the bitmap at the default
-- work_mem and to the plain scan when the bitmap would be lossy.
CREATE TABLE lis_f (id int, k int, u int);
INSERT INTO lis_f SELECT i, (i::int8 * 7919 % 300)::int, i FROM generate_series(1, 600000) i;
CREATE INDEX lis_f_k ON lis_f USING lion (k);
CREATE INDEX lis_f_u ON lis_f USING lion (u);
VACUUM ANALYZE lis_f;
SET pg_lion.enable_count_pushdown = off;
SELECT lion_top('SELECT id FROM lis_f WHERE u = 123456');
SELECT lion_top('SELECT id FROM lis_f WHERE u IN (1, 123456, 500000)');
SELECT lion_top('SELECT sum(id) FROM lis_f WHERE k = 17');
SET work_mem = '64kB';
SELECT lion_top('SELECT sum(id) FROM lis_f WHERE k = 17');
RESET work_mem;
RESET pg_lion.enable_count_pushdown;
SELECT id FROM lis_f WHERE u = 123456;
SET work_mem = '64kB';
SELECT count(*), sum(id) FROM lis_f WHERE k = 17;
RESET work_mem;
SELECT count(*), sum(id) FROM lis_f WHERE k = 17;
-- the count pushdown keeps its shapes
SELECT lion_top('SELECT count(*) FROM lis_f WHERE k = 17');
SELECT lion_top('SELECT count(*) FROM lis_f WHERE u = 123456');
SELECT lion_top('SELECT k, count(*) FROM lis_f GROUP BY k');
-- ... including a count over a clustered value after scattered updates, when
-- the visibility map pg_class remembers is stale: the rows of one value lie on
-- a few pages, which is what the node's recheck is priced by now as well as
-- the plain scan's heap side (DESIGN.md §29.11)
CREATE TABLE lis_cl (id int, c int, x int);
INSERT INTO lis_cl SELECT i, i / 2000, 0 FROM generate_series(1, 300000) i;
CREATE INDEX lis_cl_c ON lis_cl USING lion (c);
VACUUM ANALYZE lis_cl;
UPDATE lis_cl SET x = 1 WHERE id % 20 = 7;
ANALYZE lis_cl;
SELECT lion_top('SELECT count(*) FROM lis_cl WHERE c = 17');
SELECT count(*) FROM lis_cl WHERE c = 17;
DROP TABLE lis_cl;

-- ---------- 7. exclusion constraints: a non-MVCC (dirty snapshot) scan ----------
CREATE TABLE lis_ex (a int, b text, EXCLUDE USING lion (a WITH =));
INSERT INTO lis_ex VALUES (1, 'one'), (2, 'two'), (3, 'three');
INSERT INTO lis_ex VALUES (2, 'again');
INSERT INTO lis_ex VALUES (NULL, 'x'), (NULL, 'y');		-- NULLs never conflict
INSERT INTO lis_ex VALUES (4, 'four') ON CONFLICT DO NOTHING;
INSERT INTO lis_ex VALUES (4, 'four again') ON CONFLICT DO NOTHING;
UPDATE lis_ex SET a = 3 WHERE a = 1;
SELECT a, b FROM lis_ex ORDER BY a, b;
CREATE TABLE lis_ex2 (a int, b int, EXCLUDE USING lion (a WITH =, b WITH =));
INSERT INTO lis_ex2 VALUES (1, 1), (1, 2), (2, 1);
INSERT INTO lis_ex2 VALUES (1, 2);
INSERT INTO lis_ex2 SELECT i % 100, i / 100 FROM generate_series(1000, 5000) i;
INSERT INTO lis_ex2 VALUES (50, 30);
SELECT count(*) FROM lis_ex2;
-- building the constraint over existing duplicates fails
CREATE TABLE lis_ex3 (a int);
INSERT INTO lis_ex3 VALUES (1), (2), (1);
ALTER TABLE lis_ex3 ADD CONSTRAINT lis_ex3_a EXCLUDE USING lion (a WITH =);
DELETE FROM lis_ex3 WHERE ctid = (SELECT max(ctid) FROM lis_ex3);
ALTER TABLE lis_ex3 ADD CONSTRAINT lis_ex3_a EXCLUDE USING lion (a WITH =);
INSERT INTO lis_ex3 VALUES (2);
-- a multi-key exclusion: arrays that overlap conflict
CREATE TABLE lis_ex4 (id int, tags int[], EXCLUDE USING lion (tags WITH &&));
INSERT INTO lis_ex4 VALUES (1, '{1,2}'), (2, '{3}'), (3, '{}'), (4, NULL);
INSERT INTO lis_ex4 VALUES (5, '{2,5}');
INSERT INTO lis_ex4 VALUES (6, '{4,5}'), (7, '{}');
SELECT id, tags FROM lis_ex4 ORDER BY id;

-- ---------- 8. a partial multi-key index scanned whole, at a tiny work_mem ----------
-- A multi-key column that has to be read whole is streamed as the exact
-- union of its entries (DESIGN.md §29.3).  It used to be a private bitmap,
-- whose lossy pages named every offset - rows the partial index does not
-- hold, which neither a plain scan (the predicate is not rechecked) nor an
-- index-only scan (nothing is) could tell apart: 140216 rows with flag false
-- came back below, and a count of 175076 for 100000.
CREATE TABLE lis_pr (id int, tags int[], flag bool);
INSERT INTO lis_pr SELECT g, ARRAY[g % 10], g % 2 = 0 FROM generate_series(1, 400000) g;
CREATE INDEX lis_pr_tags ON lis_pr USING lion (tags) WHERE flag;
VACUUM ANALYZE lis_pr;
SET work_mem = '64kB';
SET pg_lion.enable_count_pushdown = off;
SET enable_bitmapscan = off;
SET enable_seqscan = off;
EXPLAIN (COSTS OFF)
SELECT flag, count(*) FROM (SELECT id, flag FROM lis_pr
	WHERE flag AND tags <@ '{0,1,2,3,4,5,6,7,8,9}') s GROUP BY flag;
SELECT flag, count(*) FROM (SELECT id, flag FROM lis_pr
	WHERE flag AND tags <@ '{0,1,2,3,4,5,6,7,8,9}') s GROUP BY flag ORDER BY flag;
SELECT flag, count(*) FROM (SELECT id, flag FROM lis_pr
	WHERE flag AND tags IS NOT NULL) s GROUP BY flag ORDER BY flag;
RESET enable_bitmapscan;
RESET enable_seqscan;
RESET pg_lion.enable_count_pushdown;
CREATE TABLE lis_pt (id int, tags int[], flag bool, pad text);
INSERT INTO lis_pt SELECT g, ARRAY[g % 10], g % 2 = 0, repeat('x', 200)
  FROM generate_series(1, 200000) g;
CREATE INDEX lis_pt_tags ON lis_pt USING lion (tags) WHERE flag;
VACUUM ANALYZE lis_pt;
-- with nothing disabled: an index-only scan of the partial multi-key index
SELECT lion_top('SELECT count(*) FROM lis_pt WHERE flag');
SELECT count(*) FROM lis_pt WHERE flag;
SELECT lion_ios('SELECT count(*) FROM lis_pt WHERE flag');
SELECT lion_ios('SELECT count(*) FROM lis_pr WHERE flag');
-- ... and after the heap is dirtied, so the index-only scan visits the heap
UPDATE lis_pt SET pad = 'y' WHERE id % 50 = 0;
DELETE FROM lis_pt WHERE id % 70 = 0;
SELECT lion_ios('SELECT count(*) FROM lis_pt WHERE flag');
RESET work_mem;
DROP TABLE lis_pr, lis_pt;

-- ---------- 9. a long IN list, located a batch at a time ----------
-- Every value of an IN list used to be located up front with a cursor of its
-- own, eleven kilobytes each: 20000 values held 220 MB.  Past a batch
-- (work_mem / 32 kB values, at least 32) the plain scan locates and streams
-- the list a batch at a time (DESIGN.md §29.4).  The scan, the sorted list
-- included, stays inside work_mem - set here, since a batch's memory follows
-- it: it used to take 1.05 times work_mem, which only a server default of
-- more than 16 MB hid.
BEGIN;
SET LOCAL pg_lion.enable_count_pushdown = off;
SET LOCAL enable_seqscan = off;
SET LOCAL enable_bitmapscan = off;
SET LOCAL work_mem = '4MB';
SELECT lion_top('SELECT id FROM lis_f WHERE u = ANY (array(SELECT g * 7 FROM generate_series(1, 20000) g))');
DECLARE lst CURSOR FOR
	SELECT id FROM lis_f WHERE u = ANY (array(SELECT g * 7 FROM generate_series(1, 20000) g));
FETCH 5 FROM lst;
SELECT current_setting('work_mem') AS work_mem, count(*) > 0 AS scan_contexts,
	   sum(total_bytes) < pg_size_bytes(current_setting('work_mem')) AS within_work_mem
  FROM pg_backend_memory_contexts WHERE name LIKE 'lion index scan%';
MOVE FORWARD ALL IN lst;
CLOSE lst;
SET LOCAL work_mem = '16MB';
DECLARE lst CURSOR FOR
	SELECT id FROM lis_f WHERE u = ANY (array(SELECT g * 7 FROM generate_series(1, 20000) g));
FETCH 5 FROM lst;
SELECT current_setting('work_mem') AS work_mem, count(*) > 0 AS scan_contexts,
	   sum(total_bytes) < pg_size_bytes(current_setting('work_mem')) AS within_work_mem
  FROM pg_backend_memory_contexts WHERE name LIKE 'lion index scan%';
MOVE FORWARD ALL IN lst;
COMMIT;
-- the answers: duplicates, NULLs, a spelling per batch boundary, other columns
SET work_mem = '64kB';
SELECT lion_iq('SELECT id FROM lis WHERE u = ANY (array(SELECT (g % 250) * 3 FROM generate_series(1, 600) g))');
SELECT lion_iq('SELECT id FROM lis WHERE u = ANY (array(SELECT CASE WHEN g % 9 = 0 THEN NULL ELSE g * 11 END FROM generate_series(1, 600) g)) AND u > 2000');
SELECT lion_iq('SELECT id FROM lis WHERE ci = ANY (array(SELECT CASE g % 4 WHEN 0 THEN ''alice'' WHEN 1 THEN ''ALICE'' WHEN 2 THEN ''Bob'' || (g % 5) ELSE ''bOB'' || (g % 5) END::citext FROM generate_series(1, 500) g))');
SELECT lion_iq('SELECT id FROM lis WHERE g = 3 AND kk = ANY (array(SELECT g % 200 FROM generate_series(1, 400) g))');
SELECT lion_iq('SELECT id FROM lis WHERE k = ANY (array(SELECT g % 250 FROM generate_series(1, 700) g)) AND n IS NULL');
RESET work_mem;

-- ---------- 10. the union of a multi-key column: one walk of its entries ----------
-- A multi-key column read whole is the union of its entries, gathered a
-- window of container keys at a time, and every window walks every entry
-- (DESIGN.md §29.3).  At 64 kB of work_mem a window used to be 16 container
-- keys, a thousand heap blocks, and the walk was repeated for each of them;
-- the window is at least 1024 container keys now, whatever work_mem says.
-- The index blocks one index-only count reads are the same at 64 kB and at
-- 64 MB of work_mem (pg_statio_user_indexes, flushed on demand).
CREATE TABLE lis_pw (id int, tags int[], flag bool);
INSERT INTO lis_pw SELECT g, ARRAY[g % 300], g % 2 = 0 FROM generate_series(1, 600000) g;
CREATE INDEX lis_pw_tags ON lis_pw USING lion (tags) WITH (inline_limit = 64) WHERE flag;
VACUUM ANALYZE lis_pw;
CREATE TABLE lis_pw_blks (wm text, blks bigint);
CREATE FUNCTION lis_pw_blks() RETURNS bigint LANGUAGE sql AS
$$ SELECT idx_blks_hit + idx_blks_read FROM pg_statio_user_indexes
	WHERE indexrelname = 'lis_pw_tags' $$;
SET pg_lion.enable_count_pushdown = off;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT lion_top('SELECT count(*) FROM lis_pw WHERE flag');
SELECT count(*) FROM lis_pw WHERE flag;		-- the relcache entry is built here
SET work_mem = '64kB';
SELECT pg_stat_force_next_flush();
SELECT pg_stat_clear_snapshot();
INSERT INTO lis_pw_blks SELECT 'before', lis_pw_blks();
SELECT count(*) FROM lis_pw WHERE flag;
SELECT pg_stat_force_next_flush();
SELECT pg_stat_clear_snapshot();
INSERT INTO lis_pw_blks SELECT '64kB', lis_pw_blks();
SET work_mem = '64MB';
SELECT count(*) FROM lis_pw WHERE flag;
SELECT pg_stat_force_next_flush();
SELECT pg_stat_clear_snapshot();
INSERT INTO lis_pw_blks SELECT '64MB', lis_pw_blks();
RESET work_mem;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET pg_lion.enable_count_pushdown;
SELECT (SELECT blks FROM lis_pw_blks WHERE wm = '64kB') - (SELECT blks FROM lis_pw_blks WHERE wm = 'before')
	 = (SELECT blks FROM lis_pw_blks WHERE wm = '64MB') - (SELECT blks FROM lis_pw_blks WHERE wm = '64kB')
	   AS same_index_reads_at_64kB_and_64MB;
-- ... and across windows: at the least pg_lion.scan_window_floor a window is
-- 16 container keys, a thousand heap blocks, and this union takes five
SET pg_lion.scan_window_floor = '64kB';
SET work_mem = '64kB';
SELECT lion_ios('SELECT count(*) FROM lis_pw WHERE flag');
SELECT lion_iq('SELECT id FROM lis_pw WHERE flag AND tags IS NOT NULL');
RESET work_mem;
RESET pg_lion.scan_window_floor;
DROP TABLE lis_pw, lis_pw_blks;
DROP FUNCTION lis_pw_blks();

-- ---------- 11. a range, or IS NOT NULL, beside another column's sets ----------
-- A range on one column of a multicolumn index beside an equality on another
-- was streamed entry by entry, each entry ANDed with the other column's
-- stream restarted for it - a descent of b's posting tree per entry of a:
-- `a BETWEEN 1 AND 400000 AND b = 5` over a million rows took 1.06M buffer
-- hits and 818 ms where the bitmap scan read 5481 buffers in 72 ms, and the
-- planner chose the plain scan for the 200000-row range (2026-09-25 review).
-- A walk longer than the other columns' posting pages is now ORed into
-- windows of their containers and ANDed with ONE stream of them, and
-- `a IS NOT NULL` beside `b = 5` - which walked every entry of a the same way,
-- 4.37M buffer hits - is dropped and rechecked, as the bitmap scan does
-- (DESIGN.md §29.3).  a is unique and in heap order, b has 50 values, d 20000
-- scattered ones (INLINE sparse segments spanning the heap), and a few rows
-- have a NULL a.
CREATE TABLE lis_w (a int, b int, d int, c int);
INSERT INTO lis_w SELECT g, g % 50, (g::int8 * 7919 % 20000)::int, g
  FROM generate_series(1, 200000) g;
INSERT INTO lis_w SELECT NULL, g % 50, g, -g FROM generate_series(1, 500) g;
CREATE INDEX lis_w_abd ON lis_w USING lion (a, b, d);
VACUUM ANALYZE lis_w;

/*
 * lion_bufs() runs a query as a plain index scan, or as a bitmap scan, under
 * EXPLAIN (ANALYZE, BUFFERS) and returns the shared buffers its top node
 * touched, hits and reads together.
 */
CREATE FUNCTION lion_bufs(q text, plain boolean) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	res bigint := NULL;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', CASE WHEN plain THEN 'off' ELSE 'on' END, true);
	PERFORM set_config('enable_indexscan', CASE WHEN plain THEN 'on' ELSE 'off' END, true);
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF res IS NULL AND ln ~ 'Buffers: shared' THEN
			res := coalesce(substring(ln from 'hit=(\d+)')::bigint, 0) +
				   coalesce(substring(ln from 'read=(\d+)')::bigint, 0);
		END IF;
	END LOOP;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	RETURN res;
END $$;

-- the answers, in one window of the other columns' containers and in several
CREATE TABLE lis_w_q (q text);
INSERT INTO lis_w_q VALUES
	('SELECT c FROM lis_w WHERE a BETWEEN 1000 AND 150000 AND b = 5'),	-- one-row INLINE entries
	('SELECT c FROM lis_w WHERE d BETWEEN 100 AND 15000 AND b = 7'),	-- sparse segments across windows
	('SELECT c FROM lis_w WHERE b BETWEEN 0 AND 30 AND d IN (1, 2, 3, 500, 7000)'),	-- posting trees walked
	('SELECT c FROM lis_w WHERE a < ANY (''{5000, 90000}'') AND b = 3'),	-- one walk to the widest
	('SELECT c FROM lis_w WHERE a > 100 AND a <= 180000 AND b IN (1, 2) AND d < 10000'),
	('SELECT c FROM lis_w WHERE a BETWEEN 1 AND 10 AND b = 5'),	-- short: entry by entry
	('SELECT c FROM lis_w WHERE a BETWEEN 1000 AND 150000 AND b = 999'),	-- b selects nothing
	('SELECT c FROM lis_w WHERE a > 5000000 AND b = 5'),	-- the range selects nothing
	('SELECT c FROM lis_w WHERE a BETWEEN 1 AND 150000 AND d BETWEEN 5 AND 50 AND b = 4'),	-- d left to the recheck
	('SELECT c FROM lis_w WHERE a IS NOT NULL AND b = 5'),	-- dropped, rechecked
	('SELECT c FROM lis_w WHERE a IS NOT NULL AND b > 45'),	-- the range walks
	('SELECT c FROM lis_w WHERE a IS NOT NULL AND d IS NOT NULL'),	-- one walks, one rechecked
	('SELECT c FROM lis_w WHERE a IS NOT NULL');
SELECT lion_iq(q) FROM lis_w_q;
SET pg_lion.scan_window_floor = '64kB';
SET work_mem = '64kB';
SELECT lion_iq(q) FROM lis_w_q;
RESET work_mem;
RESET pg_lion.scan_window_floor;
-- a NULL a is not a row of `a IS NOT NULL`: the heap recheck removes them
SELECT lion_ir('SELECT c FROM lis_w WHERE a IS NOT NULL AND b = 5');
SELECT lion_ir('SELECT c FROM lis_w WHERE a BETWEEN 1000 AND 150000 AND b = 5');
-- a plain scan reads about what the bitmap scan does, where it used to read
-- a descent of b's posting tree for every entry walked: 147, 36 and 390
-- times the bitmap scan's buffers here, 581, 128 and 807 ms against its 54,
-- 18 and 12
SELECT lion_bufs(q, true) <= 2 * lion_bufs(q, false) AS plain_reads_about_the_bitmaps
  FROM (VALUES ('SELECT sum(c) FROM lis_w WHERE a BETWEEN 1000 AND 150000 AND b = 5'),
			   ('SELECT sum(c) FROM lis_w WHERE d BETWEEN 100 AND 15000 AND b = 7'),
			   ('SELECT sum(c) FROM lis_w WHERE a IS NOT NULL AND b = 5')) v(q);
-- the window's memory is bounded: two 4 kB images per container of b's, at
-- most eight of them at the least floor and work_mem
BEGIN;
SET LOCAL pg_lion.enable_count_pushdown = off;
SET LOCAL enable_seqscan = off;
SET LOCAL enable_bitmapscan = off;
SET LOCAL pg_lion.scan_window_floor = '64kB';
SET LOCAL work_mem = '64kB';
DECLARE w CURSOR FOR SELECT c FROM lis_w WHERE a BETWEEN 1000 AND 150000 AND b = 5;
FETCH 3 FROM w;
SELECT count(*) > 0 AS scan_contexts,
	   sum(total_bytes) < 512 * 1024 AS under_512kB
  FROM pg_backend_memory_contexts WHERE name LIKE 'lion index scan%';
MOVE FORWARD ALL IN w;
COMMIT;
-- plan choice with nothing disabled: a short range beside b = 5 goes to the
-- plain scan, which now reads what the bitmap scan reads; a range of nearly
-- every a pays for its own walk (DESIGN.md §29.11) where it was prorated by
-- b's selectivity too, and goes to the sequential scan rather than to a
-- bitmap scan half as slow again; `a IS NOT NULL AND b = 5` is priced by b
-- alone and goes to the bitmap scan
SET pg_lion.enable_count_pushdown = off;
SELECT lion_top('SELECT sum(c) FROM lis_w WHERE a BETWEEN 1000 AND 5000 AND b = 5');
SELECT lion_top('SELECT sum(c) FROM lis_w WHERE a BETWEEN 1000 AND 190000 AND b = 5');
SELECT lion_top('SELECT sum(c) FROM lis_w WHERE a IS NOT NULL AND b = 5');
RESET pg_lion.enable_count_pushdown;
-- a dirty heap, then VACUUM
UPDATE lis_w SET b = (b + 1) % 50 WHERE c % 7 = 0;
DELETE FROM lis_w WHERE c % 11 = 0;
UPDATE lis_w SET a = NULL WHERE c % 13 = 0;
SELECT lion_iq(q) FROM lis_w_q;
VACUUM lis_w;
SET pg_lion.scan_window_floor = '64kB';
SET work_mem = '64kB';
SELECT lion_iq(q) FROM lis_w_q;
RESET work_mem;
RESET pg_lion.scan_window_floor;
DROP TABLE lis_w, lis_w_q;
DROP FUNCTION lion_bufs(text, boolean);

DROP TABLE lis, lis_outer, lis_big, lis_f, lis_ex, lis_ex2, lis_ex3, lis_ex4;
DROP FUNCTION lion_iq(text);
DROP FUNCTION lion_ir(text);
DROP FUNCTION lion_top(text);
DROP FUNCTION lion_ios(text);
