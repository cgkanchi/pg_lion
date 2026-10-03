-- Index-only scans that return the window store's columns (DESIGN.md §40,
-- "Index-only scans").
--
-- amcanreturn says true for a stored column whose stored type is the index
-- column's own (LionStoreCol.returnable): a key column under store_values,
-- an INCLUDE column always.  liongettuple() hands the executor each TID's
-- values - from the store for a batch the §9 interlock covers, from the heap
-- for a heap page the store leaves ABSENT and for a batch with no interlock
-- behind it - and core's IndexOnlyScan fetches the heap only for a page the
-- visibility map does not call all-visible.  lion's planner hook builds the
-- index-only paths core does not: a qual on a column the index cannot return
-- - a multi-key column, a key column without store_values - that the posting
-- sets answer exactly, with no recheck (`tags && '{a,b}'`, `doc @@ 'x & y'`).
--
-- Every answer is compared, as a multiset both ways round and spelling by
-- spelling, with the same query's under sequential scans only: over clean
-- pages, over pages with updates and deletes not yet vacuumed, and after the
-- VACUUM, with NULLs, a text column under a collation, a capped column and
-- an ABSENT heap page.  The sections:
--
--	1. a scalar key under store_values with INCLUDE columns
--	2. an array column: `&&`, `@>`, IS NULL, returning INCLUDE columns
--	3. a tsvector column: `@@`
--	4. store_max_len: a capped text column
--	5. an ABSENT heap page: its rows' values come from the heap
--	6. multi-key sets past the pin budget: their batches go to the heap
--	7. what is not answered from the index: SELECT * over the array column,
--	   a predicate the sets answer only with a recheck
--	8. citext: a key column's own spelling, and an INCLUDE citext column
--	9. the planner's choice: the heap for a few rows on every window, the
--	   store for a tenth of the table
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;
-- a temporary table's buffer pool is temp_buffers, settable only before the
-- session touches one: 100 buffers is a pin budget of 12 leaves (section 6)
SET temp_buffers = 100;

/*
 * lion_ios_check() names the scan the query's plan runs - the first of
 * Index Only Scan, Index Scan, Bitmap Heap Scan and Seq Scan in it - and
 * compares the query's rows with its rows under sequential scans only, each
 * row as its text spelling (citext's equality would take 'Alice' for
 * 'alice').  The settings are put back as they were.
 */
CREATE FUNCTION lion_ios_check(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	node text := 'other';
	n bigint;
	ndiff bigint;
	s_idx text := current_setting('enable_indexscan');
	s_ios text := current_setting('enable_indexonlyscan');
	s_bmp text := current_setting('enable_bitmapscan');
	s_seq text := current_setting('enable_seqscan');
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF node = 'other' AND
		   ln ~ '(Index Only Scan|Index Scan|Bitmap Heap Scan|Seq Scan)' THEN
			node := substring(ln from '(Index Only Scan|Index Scan|Bitmap Heap Scan|Seq Scan)');
		END IF;
	END LOOP;
	EXECUTE 'CREATE TEMP TABLE lion_ios_got AS SELECT x::text AS r FROM (' || q || ') x';
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	EXECUTE 'CREATE TEMP TABLE lion_ios_want AS SELECT x::text AS r FROM (' || q || ') x';
	PERFORM set_config('enable_indexscan', s_idx, true);
	PERFORM set_config('enable_indexonlyscan', s_ios, true);
	PERFORM set_config('enable_bitmapscan', s_bmp, true);
	PERFORM set_config('enable_seqscan', s_seq, true);
	SELECT count(*) INTO n FROM lion_ios_got;
	SELECT (SELECT count(*) FROM (SELECT r FROM lion_ios_got EXCEPT ALL
								  SELECT r FROM lion_ios_want) a) +
		   (SELECT count(*) FROM (SELECT r FROM lion_ios_want EXCEPT ALL
								  SELECT r FROM lion_ios_got) b)
	  INTO ndiff;
	DROP TABLE lion_ios_got, lion_ios_want;
	RETURN format('%s: %s, %s rows', node,
				  CASE WHEN ndiff = 0 THEN 'same' ELSE ndiff || ' differ' END, n);
END $$;

/*
 * EXPLAIN ANALYZE of q, the lines that say what the scan did: its node and
 * conditions, and core's own Heap Fetches - the rows it looked up in the heap
 * because the visibility map does not call their pages all-visible - as 0 or
 * "> 0".  The row counts are lion_ios_check()'s.
 */
CREATE FUNCTION lion_ios_explain(q text) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		CONTINUE WHEN ln ~ '^\s*(Index Searches|Disabled|Storage|Planning|Execution|Buffers|Memory)';
		ln := regexp_replace(ln, '\s*\(actual[^)]*\)', '');
		ln := regexp_replace(ln, 'Heap Fetches: [1-9][0-9]*', 'Heap Fetches: > 0');
		RETURN NEXT ln;
	END LOOP;
END $$;

/* Which columns of idx an index-only scan can return (amcanreturn). */
CREATE FUNCTION lion_ios_returnable(idx regclass)
RETURNS TABLE (col name, returnable boolean) LANGUAGE sql AS $$
	SELECT a.attname, pg_index_column_has_property(idx, a.attnum, 'returnable')
	  FROM pg_attribute a
	 WHERE a.attrelid = idx AND a.attnum > 0
	 ORDER BY a.attnum $$;

-- ---------------------------------------------------------------------
-- 1. A scalar key under store_values, INCLUDE columns: an int with NULLs,
--    a text column under the "C" collation, and a text column with one
--    value of 1,800 bytes in every thousand rows.  A NULL key now and then.
-- ---------------------------------------------------------------------

CREATE TABLE lion_ios_s (id int, k int, a int, t text COLLATE "C", w text);
INSERT INTO lion_ios_s
SELECT g, CASE WHEN g % 333 = 0 THEN NULL ELSE g % 200 END,
	   CASE WHEN g % 11 = 0 THEN NULL ELSE g * 3 END,
	   CASE WHEN g % 7 = 0 THEN NULL
			ELSE (ARRAY['apple', 'Banana', 'cherry', 'Date', 'eel'])[1 + g % 5] || (g % 3) END,
	   CASE WHEN g % 1000 = 1 THEN repeat('long', 450) ELSE 'w' || g END
  FROM generate_series(1, 40000) g;
CREATE INDEX lion_ios_s_k ON lion_ios_s USING lion (k) INCLUDE (a, t, w)
	WITH (store_values = true);
VACUUM ANALYZE lion_ios_s;
SELECT * FROM lion_ios_returnable('lion_ios_s_k');
-- the heap scans are kept out: on 287 heap pages the planner prices the
-- heap below the gathers of a WALK (k < 5 is five passes, each over the
-- five windows), and the choice between them is section 9's
SET enable_seqscan = off;
SET enable_bitmapscan = off;

EXPLAIN (COSTS OFF) SELECT k, a, t, w FROM lion_ios_s WHERE k = 17;
EXPLAIN (COSTS OFF) SELECT a, t FROM lion_ios_s WHERE k IN (3, 5, 150);
EXPLAIN (COSTS OFF) SELECT a, w FROM lion_ios_s WHERE k IS NULL;
-- a qual on a returned column is the scan's filter, evaluated on its values
EXPLAIN (COSTS OFF) SELECT k, length(w) FROM lion_ios_s WHERE k = 1 AND w LIKE 'long%';
SELECT k, count(*), min(length(w)) FROM lion_ios_s WHERE k = 1 AND w LIKE 'long%' GROUP BY k;
-- the collation is the column's: upper case before lower case under "C"
EXPLAIN (COSTS OFF) SELECT DISTINCT t FROM lion_ios_s WHERE k < 5 ORDER BY t;
SELECT DISTINCT t FROM lion_ios_s WHERE k < 5 ORDER BY t;

SELECT q, lion_ios_check(q) FROM (VALUES
	('SELECT k, a, t, w FROM lion_ios_s WHERE k = 17'),
	('SELECT a, t FROM lion_ios_s WHERE k IN (3, 5, 150)'),
	('SELECT a, w FROM lion_ios_s WHERE k IS NULL'),
	('SELECT k, t FROM lion_ios_s WHERE k < 5'),
	('SELECT k, length(w) FROM lion_ios_s WHERE k = 1 AND w LIKE ''long%''')) v(q);
SELECT * FROM lion_ios_explain('SELECT k, a, t, w FROM lion_ios_s WHERE k = 17');

-- updated and deleted rows, not vacuumed: the pages they are on are not
-- all-visible, and the executor takes those rows' visibility from the heap,
-- their values from the scan
UPDATE lion_ios_s SET a = a + 1, t = upper(t) WHERE id % 50 = 0;
DELETE FROM lion_ios_s WHERE id % 61 = 0;
INSERT INTO lion_ios_s VALUES (40001, 17, NULL, 'fig', NULL), (40002, 17, 5, NULL, 'new'),
	(40003, NULL, 7, 'Fig', repeat('long', 400));
SELECT q, lion_ios_check(q) FROM (VALUES
	('SELECT k, a, t, w FROM lion_ios_s WHERE k = 17'),
	('SELECT a, t FROM lion_ios_s WHERE k IN (3, 5, 150)'),
	('SELECT a, w FROM lion_ios_s WHERE k IS NULL'),
	('SELECT k, t FROM lion_ios_s WHERE k < 5'),
	('SELECT k, length(w) FROM lion_ios_s WHERE k = 1 AND w LIKE ''long%''')) v(q);
SELECT * FROM lion_ios_explain('SELECT k, a, t, w FROM lion_ios_s WHERE k = 17');

-- ... and vacuumed: every page all-visible again, no heap fetch at all
VACUUM lion_ios_s;
SELECT q, lion_ios_check(q) FROM (VALUES
	('SELECT k, a, t, w FROM lion_ios_s WHERE k = 17'),
	('SELECT a, w FROM lion_ios_s WHERE k IS NULL'),
	('SELECT k, t FROM lion_ios_s WHERE k < 5')) v(q);
SELECT * FROM lion_ios_explain('SELECT k, a, t, w FROM lion_ios_s WHERE k = 17');

-- an index without store_values returns its INCLUDE columns, not its key
CREATE INDEX lion_ios_s_nokey ON lion_ios_s USING lion (k) INCLUDE (a);
SELECT * FROM lion_ios_returnable('lion_ios_s_nokey');
DROP INDEX lion_ios_s_nokey;
RESET enable_seqscan;
RESET enable_bitmapscan;

-- ---------------------------------------------------------------------
-- 2. An array column, its INCLUDE columns returned under `&&` and `@>`:
--    the posting sets answer both exactly (DESIGN.md §15, §17), so the
--    plan never reads tags, which no index-only scan could return.  NULL
--    and empty arrays, a NULL INCLUDE value now and then.
-- ---------------------------------------------------------------------

CREATE TABLE lion_ios_a (id int, tags text[], inc int, note text COLLATE "C");
INSERT INTO lion_ios_a
SELECT g, CASE WHEN g % 97 = 0 THEN NULL WHEN g % 89 = 0 THEN '{}'
			   ELSE ARRAY['t' || (g % 40), 'u' || (g % 9)] END,
	   CASE WHEN g % 13 = 0 THEN NULL ELSE g END,
	   (ARRAY['Note', 'note', 'NOTE'])[1 + g % 3] || (g % 500)
  FROM generate_series(1, 40000) g;
CREATE INDEX lion_ios_a_tags ON lion_ios_a USING lion (tags) INCLUDE (inc, note);
VACUUM ANALYZE lion_ios_a;
SELECT * FROM lion_ios_returnable('lion_ios_a_tags');

EXPLAIN (COSTS OFF) SELECT inc, note FROM lion_ios_a WHERE tags && '{t3,t4}';
EXPLAIN (COSTS OFF) SELECT inc FROM lion_ios_a WHERE tags @> '{t3}';
EXPLAIN (COSTS OFF) SELECT inc, note FROM lion_ios_a WHERE tags @> '{t3,u2}';
EXPLAIN (COSTS OFF) SELECT inc FROM lion_ios_a WHERE tags @> '{t5}' AND inc > 20000;
EXPLAIN (COSTS OFF) SELECT inc, note FROM lion_ios_a WHERE tags IS NULL;

SELECT q, lion_ios_check(q) FROM (VALUES
	('SELECT inc, note FROM lion_ios_a WHERE tags && ''{t3,t4}'''),
	('SELECT inc FROM lion_ios_a WHERE tags @> ''{t3}'''),
	('SELECT inc, note FROM lion_ios_a WHERE tags @> ''{t3,u2}'''),
	('SELECT inc FROM lion_ios_a WHERE tags @> ''{t5}'' AND inc > 20000'),
	('SELECT inc, note FROM lion_ios_a WHERE tags IS NULL')) v(q);
SELECT * FROM lion_ios_explain('SELECT inc, note FROM lion_ios_a WHERE tags && ''{t3,t4}''');

UPDATE lion_ios_a SET inc = -inc, note = lower(note) WHERE id % 40 = 3;
UPDATE lion_ios_a SET tags = tags || '{t4}'::text[] WHERE id % 40 = 5;
DELETE FROM lion_ios_a WHERE id % 71 = 0;
INSERT INTO lion_ios_a VALUES (40001, '{t3,zz}', NULL, 'new'), (40002, '{t4}', 7, NULL);
SELECT q, lion_ios_check(q) FROM (VALUES
	('SELECT inc, note FROM lion_ios_a WHERE tags && ''{t3,t4}'''),
	('SELECT inc FROM lion_ios_a WHERE tags @> ''{t3}'''),
	('SELECT inc, note FROM lion_ios_a WHERE tags @> ''{t3,u2}'''),
	('SELECT inc FROM lion_ios_a WHERE tags @> ''{t5}'' AND inc > 20000'),
	('SELECT inc, note FROM lion_ios_a WHERE tags IS NULL')) v(q);
SELECT * FROM lion_ios_explain('SELECT inc, note FROM lion_ios_a WHERE tags && ''{t3,t4}''');

VACUUM lion_ios_a;
SELECT q, lion_ios_check(q) FROM (VALUES
	('SELECT inc, note FROM lion_ios_a WHERE tags && ''{t3,t4}'''),
	('SELECT inc, note FROM lion_ios_a WHERE tags @> ''{t3,u2}''')) v(q);
SELECT * FROM lion_ios_explain('SELECT inc, note FROM lion_ios_a WHERE tags && ''{t3,t4}''');

-- ---------------------------------------------------------------------
-- 3. A tsvector column: `@@` of an AND or an OR of lexemes is exact.
-- ---------------------------------------------------------------------

CREATE TABLE lion_ios_d (id int, doc tsvector, inc int, title text);
INSERT INTO lion_ios_d
SELECT g, CASE WHEN g % 101 = 0 THEN NULL
			   ELSE to_tsvector('simple', 'w' || (g % 30) || ' x' || (g % 11) || ' y' || (g % 7)) END,
	   CASE WHEN g % 17 = 0 THEN NULL ELSE g END, 'title ' || (g % 300)
  FROM generate_series(1, 40000) g;
CREATE INDEX lion_ios_d_doc ON lion_ios_d USING lion (doc) INCLUDE (inc, title);
VACUUM ANALYZE lion_ios_d;
SELECT * FROM lion_ios_returnable('lion_ios_d_doc');

EXPLAIN (COSTS OFF) SELECT inc, title FROM lion_ios_d WHERE doc @@ 'w3 & x2'::tsquery;
EXPLAIN (COSTS OFF) SELECT inc FROM lion_ios_d WHERE doc @@ 'w3 | w4'::tsquery;

SELECT q, lion_ios_check(q) FROM (VALUES
	('SELECT inc, title FROM lion_ios_d WHERE doc @@ ''w3 & x2''::tsquery'),
	('SELECT inc FROM lion_ios_d WHERE doc @@ ''w3 | w4''::tsquery')) v(q);
UPDATE lion_ios_d SET title = upper(title) WHERE id % 30 = 3;
DELETE FROM lion_ios_d WHERE id % 37 = 0;
SELECT q, lion_ios_check(q) FROM (VALUES
	('SELECT inc, title FROM lion_ios_d WHERE doc @@ ''w3 & x2''::tsquery'),
	('SELECT inc FROM lion_ios_d WHERE doc @@ ''w3 | w4''::tsquery')) v(q);
SELECT * FROM lion_ios_explain('SELECT inc, title FROM lion_ios_d WHERE doc @@ ''w3 & x2''::tsquery');
VACUUM lion_ios_d;
SELECT * FROM lion_ios_explain('SELECT inc, title FROM lion_ios_d WHERE doc @@ ''w3 & x2''::tsquery');

-- ---------------------------------------------------------------------
-- 4. store_max_len: a text INCLUDE column in capped slots
-- ---------------------------------------------------------------------

CREATE TABLE lion_ios_c (id int, k int, code text);
INSERT INTO lion_ios_c
SELECT g, g % 100, CASE WHEN g % 9 = 0 THEN NULL ELSE 'c-' || g END
  FROM generate_series(1, 30000) g;
CREATE INDEX lion_ios_c_k ON lion_ios_c USING lion (k) INCLUDE (code)
	WITH (store_values = true, store_max_len = 12);
VACUUM ANALYZE lion_ios_c;
EXPLAIN (COSTS OFF) SELECT k, code FROM lion_ios_c WHERE k = 42;
SELECT lion_ios_check('SELECT k, code FROM lion_ios_c WHERE k = 42');
UPDATE lion_ios_c SET code = 'u-' || id WHERE id % 25 = 0;
SELECT lion_ios_check('SELECT k, code FROM lion_ios_c WHERE k = 42');

-- ---------------------------------------------------------------------
-- 5. ABSENT: heap pages whose values no store page can hold (store.sql's
--    recipe: distinct strings of 1,984 bytes the heap compresses inline,
--    a hundred to a page).  The key is not stored - the qual is answered by
--    the posting sets, and the plan is the one lion's planner hook builds -
--    and the ABSENT pages' values come from the heap, the others' from the
--    store.  The other rows are short, and spread ten to a page by a column
--    the heap neither compresses nor toasts, so that the rows of a key, four
--    of them on the ABSENT pages, lie on as many heap pages as there are
--    rows: the store's few pages are the cheaper read, and the index-only
--    scan is the planner's own choice.
-- ---------------------------------------------------------------------

CREATE TABLE lion_ios_abs (id int, k int, s text, f text, w text) WITH (toast_tuple_target = 128);
ALTER TABLE lion_ios_abs ALTER COLUMN w SET STORAGE PLAIN;
INSERT INTO lion_ios_abs SELECT g, g % 100, repeat(md5(g::text), 62), repeat('f', 100), NULL FROM generate_series(1, 400) g;
INSERT INTO lion_ios_abs SELECT g, g % 100, 'short ' || g, NULL, repeat('w', 700) FROM generate_series(401, 5400) g;
CREATE INDEX lion_ios_abs_i ON lion_ios_abs USING lion (k) INCLUDE (s);
VACUUM ANALYZE lion_ios_abs;
SELECT attno, store_absent > 0 AS has_absent FROM lion_index_stats('lion_ios_abs_i') WHERE attno = 2;
SELECT * FROM lion_ios_returnable('lion_ios_abs_i');
-- rows on ABSENT pages and on stored ones, in one batch
EXPLAIN (COSTS OFF) SELECT s FROM lion_ios_abs WHERE k IN (7, 50);
SELECT count(*), count(*) FILTER (WHERE id <= 400) AS on_absent_pages FROM lion_ios_abs WHERE k IN (7, 50);
SELECT lion_ios_check('SELECT s FROM lion_ios_abs WHERE k IN (7, 50)');
SELECT lion_ios_check('SELECT length(s) FROM lion_ios_abs WHERE k = 7');
SELECT * FROM lion_ios_explain('SELECT s FROM lion_ios_abs WHERE k IN (7, 50)');
-- a walk of three entries gathers the windows once per entry
EXPLAIN (COSTS OFF) SELECT s FROM lion_ios_abs WHERE k BETWEEN 30 AND 32;
SELECT lion_ios_check('SELECT s FROM lion_ios_abs WHERE k BETWEEN 30 AND 32');
-- the key itself is not returned: a plain scan
EXPLAIN (COSTS OFF) SELECT k, s FROM lion_ios_abs WHERE k IN (7, 50);

-- ---------------------------------------------------------------------
-- 6. Past the pin budget.  A multi-key query's sets keep their leaves
--    pinned for an index-only scan as far as the list pin budget goes, and
--    a set past it holds none: the batches it contributes to have no §9
--    interlock, and their TIDs go to the heap, values and all.  A temporary
--    table's budget is an eighth of temp_buffers, 12 leaves here, and keys
--    of 1.9 kB put one or two on a leaf.
-- ---------------------------------------------------------------------

CREATE TEMP TABLE lion_ios_pin (id int, tags text[], inc int);
INSERT INTO lion_ios_pin SELECT g, ARRAY[repeat(md5(g::text), 59) || g], g * 7 FROM generate_series(1, 300) g;
CREATE INDEX lion_ios_pin_tags ON lion_ios_pin USING lion (tags) INCLUDE (inc);
VACUUM ANALYZE lion_ios_pin;
SELECT array_agg(repeat(md5(g::text), 59) || g) AS pin_keys FROM generate_series(1, 300, 4) g \gset
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT lion_ios_check(format('SELECT inc FROM lion_ios_pin WHERE tags && %L::text[]', :'pin_keys'));
UPDATE lion_ios_pin SET inc = -inc WHERE id % 8 = 1;
SELECT lion_ios_check(format('SELECT inc FROM lion_ios_pin WHERE tags && %L::text[]', :'pin_keys'));
RESET enable_seqscan;
RESET enable_bitmapscan;
DROP TABLE lion_ios_pin;

-- ---------------------------------------------------------------------
-- 7. What an index-only scan does not answer: the array column itself,
--    which no index can return (a row has many keys and the store one
--    slot), and the predicates the sets answer only with a recheck - `<@`,
--    `@> '{}'`, a tsquery with `!` or a prefix - whose recheck would read
--    the column.  The plans core takes instead.
-- ---------------------------------------------------------------------

SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT * FROM lion_ios_a WHERE tags @> '{t3}';
EXPLAIN (COSTS OFF) SELECT tags, inc FROM lion_ios_a WHERE tags && '{t3,t4}';
EXPLAIN (COSTS OFF) SELECT inc FROM lion_ios_a WHERE tags <@ '{t3,u2}';
EXPLAIN (COSTS OFF) SELECT inc FROM lion_ios_a WHERE tags @> '{}';
EXPLAIN (COSTS OFF) SELECT inc FROM lion_ios_d WHERE doc @@ '!w3'::tsquery;
EXPLAIN (COSTS OFF) SELECT inc FROM lion_ios_d WHERE doc @@ 'w3:*'::tsquery;
-- an INCLUDE column that is not returned: the plan needs id
EXPLAIN (COSTS OFF) SELECT id, inc FROM lion_ios_a WHERE tags && '{t3,t4}';
RESET enable_seqscan;
SELECT q, lion_ios_check(q) FROM (VALUES
	('SELECT * FROM lion_ios_a WHERE tags @> ''{t3}'''),
	('SELECT inc FROM lion_ios_a WHERE tags <@ ''{t3,u2}'''),
	('SELECT inc FROM lion_ios_d WHERE doc @@ ''!w3''::tsquery')) v(q);

-- ---------------------------------------------------------------------
-- 8. citext (the pg_lion_citext extension).  The key column's entry is one
--    spelling standing for all its case variants (citext.sql), but the store
--    holds each row's own datum (DESIGN.md §40, "Writes"): citext_ops has no
--    storage type, so the stored type is the index column's and the key is
--    returnable (LionStoreCol.returnable) - and what it returns is every
--    row's own spelling.  The INCLUDE citext column likewise.
-- ---------------------------------------------------------------------

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS citext;
CREATE EXTENSION IF NOT EXISTS pg_lion_citext;
RESET client_min_messages;
CREATE TABLE lion_ios_ci (id int, name citext, alias citext);
INSERT INTO lion_ios_ci
SELECT g, (ARRAY['Alice', 'ALICE', 'alice', 'Bob', 'BOB', 'carol'])[1 + g % 6],
	   CASE WHEN g % 10 = 0 THEN NULL ELSE (ARRAY['Al', 'AL', 'al'])[1 + g % 3] || (g % 4) END
  FROM generate_series(1, 30000) g;
CREATE INDEX lion_ios_ci_name ON lion_ios_ci USING lion (name) INCLUDE (alias)
	WITH (store_values = true);
VACUUM ANALYZE lion_ios_ci;
SELECT * FROM lion_ios_returnable('lion_ios_ci_name');
EXPLAIN (COSTS OFF) SELECT name, alias FROM lion_ios_ci WHERE name = 'alice';
SELECT name::text, count(*) FROM lion_ios_ci WHERE name = 'alice' GROUP BY name::text ORDER BY 1;
SELECT lion_ios_check('SELECT name, alias FROM lion_ios_ci WHERE name = ''alice''');
UPDATE lion_ios_ci SET name = upper(name::text), alias = lower(alias::text) WHERE id % 50 = 2;
SELECT name::text, count(*) FROM lion_ios_ci WHERE name = 'alice' GROUP BY name::text ORDER BY 1;
SELECT lion_ios_check('SELECT name, alias FROM lion_ios_ci WHERE name = ''alice''');
SELECT * FROM lion_ios_explain('SELECT name, alias FROM lion_ios_ci WHERE name = ''alice''');
DROP TABLE lion_ios_ci;
DROP EXTENSION pg_lion_citext;
DROP EXTENSION citext;

-- ---------------------------------------------------------------------
-- 9. The price of the gather (DESIGN.md §40, "As built: the price of the
--    index-only scan").  The scan gathers every returnable column of every
--    window its rows touch, a window's whole chain of each, priced in
--    core's page costs: a few hundred rows scattered over every window read
--    the whole store, and the heap's few hundred pages are cheaper; a tenth
--    of the rows read the same store once, and the heap is not.  200,000
--    rows of about 530 bytes (16 a page, 12,500 pages, 200 windows), a
--    stored text of 224 bytes beside an unstored filler of 300: the store
--    is about 5,500 pages.
-- ---------------------------------------------------------------------

CREATE TABLE lion_ios_p (id int, k int, g int, s text, filler text);
INSERT INTO lion_ios_p
SELECT g, g % 1000, g % 10, repeat(md5(g::text), 7), repeat('f', 300)
  FROM generate_series(1, 200000) g;
CREATE INDEX lion_ios_p_k ON lion_ios_p USING lion (k) INCLUDE (s);
CREATE INDEX lion_ios_p_g ON lion_ios_p USING lion (g) INCLUDE (s);
VACUUM ANALYZE lion_ios_p;
SELECT q, regexp_replace(lion_ios_check(q), '^(Index Scan|Bitmap Heap Scan|Seq Scan)', 'heap scan')
  FROM (VALUES
	('SELECT length(s) FROM lion_ios_p WHERE k = 123'),
	('SELECT length(s) FROM lion_ios_p WHERE g = 3')) v(q);
DROP TABLE lion_ios_p;

SELECT lion_index_verify('lion_ios_s_k', true), lion_index_verify('lion_ios_a_tags', true),
	   lion_index_verify('lion_ios_d_doc', true), lion_index_verify('lion_ios_c_k', true);

DROP TABLE lion_ios_s, lion_ios_a, lion_ios_d, lion_ios_c, lion_ios_abs;
DROP FUNCTION lion_ios_check(text);
DROP FUNCTION lion_ios_explain(text);
DROP FUNCTION lion_ios_returnable(regclass);
