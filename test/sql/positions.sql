-- Stored positions (DESIGN.md §17, "Stored positions"): insert, VACUUM,
-- CREATE INDEX, and the bitmap scans they answer.
--
-- A column whose multi-key opclass has support function 5 (tsvector_ops), in
-- an index built WITH (store_positions = true), stores for every key each
-- row's word positions and weights: in the entry while it is INLINE, in a
-- position tree once the entry spills.  The test creates a function that
-- lists what the index stores for one key, and compares that with unnest() of
-- the rows' tsvectors.
--
-- The table keeps autovacuum off so that the VACUUMs below are the only ones.

\set VERBOSITY terse
SET client_min_messages = warning;
-- a heap page becomes all-visible only once its rows' commits are flushed,
-- and the dev cluster commits asynchronously (count.sql says more)
SET synchronous_commit = on;

CREATE EXTENSION IF NOT EXISTS pg_lion;

CREATE FUNCTION lion_debug_key_positions(regclass, text,
	OUT tid tid, OUT positions int2[], OUT weights text[]) RETURNS SETOF record
	AS '$libdir/pg_lion' LANGUAGE C STRICT;

CREATE TABLE pos_docs (id int, d tsvector) WITH (autovacuum_enabled = off);
CREATE INDEX pos_docs_d ON pos_docs USING lion (d) WITH (store_positions = true);

-- What the index stores for every lexeme of the table, live rows only (an
-- UPDATE leaves the old row's positions behind, for VACUUM), against the
-- rows themselves.
CREATE VIEW pos_compare AS
WITH idx AS (
	SELECT l.lexeme, p.tid, p.positions, p.weights
	FROM (SELECT DISTINCT unnest(tsvector_to_array(d)) AS lexeme
		  FROM pos_docs) l,
		 lion_debug_key_positions('pos_docs_d', l.lexeme) p
	WHERE p.tid IN (SELECT ctid FROM pos_docs)),
heap AS (
	SELECT u.lexeme, pos_docs.ctid AS tid, u.positions, u.weights
	FROM pos_docs, unnest(d) u)
SELECT (SELECT count(*) FROM heap) AS heap_rows,
	   (SELECT count(*) FROM (SELECT * FROM idx EXCEPT ALL SELECT * FROM heap) x) AS only_index,
	   (SELECT count(*) FROM (SELECT * FROM heap EXCEPT ALL SELECT * FROM idx) x) AS only_heap;

-- one row, then a few: inline entries
INSERT INTO pos_docs VALUES (1, 'a:1A b:2,5 c:3B');
INSERT INTO pos_docs VALUES (2, 'a:7 c:1C,2,9D');
INSERT INTO pos_docs VALUES (3, strip('a:1 b:2'::tsvector));	-- no positions
INSERT INTO pos_docs VALUES (4, ''::tsvector);				-- no lexemes
SELECT * FROM lion_debug_key_positions('pos_docs_d', 'a') ORDER BY tid;
SELECT * FROM lion_debug_key_positions('pos_docs_d', 'b') ORDER BY tid;
SELECT * FROM pos_compare;

-- positions at the limits: a tsvector keeps at most 255 per lexeme, and the
-- largest position is 16383
SELECT array_length(positions, 1) AS npos, positions[256] AS last
FROM (SELECT string_agg('long', ' ') AS t FROM generate_series(1, 300)) s,
	 unnest(to_tsvector('simple', t));
INSERT INTO pos_docs
SELECT 5, to_tsvector('simple', string_agg('long', ' ')) FROM generate_series(1, 300);
INSERT INTO pos_docs VALUES (6, 'edge:16383A');
SELECT tid, array_length(positions, 1) AS npos, positions[array_length(positions, 1)] AS last
FROM lion_debug_key_positions('pos_docs_d', 'long');
SELECT * FROM lion_debug_key_positions('pos_docs_d', 'edge');

-- many rows: rare words stay inline, common ones spill to position trees,
-- and an UPDATE adds a second version of every third row
SELECT setseed(0.42);
INSERT INTO pos_docs
SELECT g, setweight((SELECT string_agg(w, ' ')
					 FROM (SELECT 'w' || floor(random() * random() * 300)::int AS w
						   FROM generate_series(1, 5 + g % 40)) s)::tsvector,
					(ARRAY['A', 'B', 'C', 'D'])[1 + g % 4]::"char")
FROM generate_series(10, 2500) g;
UPDATE pos_docs SET d = to_tsvector('simple', 'common rare' || id || ' common again common') || d
WHERE id % 3 = 0;
SELECT * FROM pos_compare;
SELECT lion_index_verify('pos_docs_d');
SELECT lion_index_verify('pos_docs_d', true);
SELECT count(*) FROM pos_docs WHERE d @@ 'common & again';

-- VACUUM takes the dead rows' positions out with their TIDs: inline ones in
-- the entry's rewrite, spilled ones from the position tree after the posting
-- set; what is left matches the rows exactly, dead versions and all gone
DELETE FROM pos_docs WHERE id % 7 = 0;
VACUUM pos_docs;
SELECT * FROM pos_compare;
SELECT count(*) AS dead_left
FROM (SELECT DISTINCT unnest(tsvector_to_array(d)) AS lexeme FROM pos_docs) l,
	 lion_debug_key_positions('pos_docs_d', l.lexeme) p
WHERE p.tid NOT IN (SELECT ctid FROM pos_docs);
SELECT lion_index_verify('pos_docs_d', true);

-- a key whose rows all go takes its position tree with it, and the next
-- rows of that key start a new one
DELETE FROM pos_docs WHERE d @@ 'common';
VACUUM pos_docs;
SELECT count(*) FROM lion_debug_key_positions('pos_docs_d', 'common');
INSERT INTO pos_docs
SELECT g, to_tsvector('simple', 'common again ' || g || ' common') FROM generate_series(1, 600) g;
SELECT * FROM pos_compare;
SELECT lion_index_verify('pos_docs_d', true);

-- A build over the rows there are (REINDEX): the same positions, with keys of
-- every shape - inline ones, a few rows with many positions each (a chunk
-- too large for the entry, or more than a chunk: a CHAIN entry with a small
-- posting set), and a key in every row whose tree is more than a leaf
INSERT INTO pos_docs
SELECT 3000 + g, to_tsvector('simple', repeat('dense ', 200) || 'few' || (g % 3))
FROM generate_series(1, 12) g;
INSERT INTO pos_docs
SELECT 4000 + g, to_tsvector('simple', 'everywhere ' || repeat('again ', 1 + g % 20))
FROM generate_series(1, 4000) g;
REINDEX INDEX pos_docs_d;
SELECT * FROM pos_compare;
SELECT lion_index_verify('pos_docs_d', true);

-- and it goes on from there: inserts, deletes, VACUUM
INSERT INTO pos_docs
SELECT 9000 + g, to_tsvector('simple', 'everywhere dense new' || g) FROM generate_series(1, 300) g;
DELETE FROM pos_docs WHERE id % 11 = 0;
VACUUM pos_docs;
SELECT * FROM pos_compare;
SELECT lion_index_verify('pos_docs_d', true);

-- A build in the least memory there is writes the keys' positions out to a
-- file more than once and reads each key's parts back in order; heap-only
-- tuples, which the scan reports under their chain's root offset, bring a
-- page's rows to it out of order
CREATE TABLE pos_hot (id int, d tsvector)
	WITH (fillfactor = 50, autovacuum_enabled = off);
INSERT INTO pos_hot
SELECT g, to_tsvector('simple', repeat('dense ', 100 + g % 3) || 'w' || (g % 50) ||
							   repeat(' pad', g % 2) || ' hot' || (g % 3))
FROM generate_series(1, 3000) g;
CREATE INDEX pos_hot_d ON pos_hot USING lion (d) WITH (store_positions = true);
UPDATE pos_hot SET id = -id WHERE id % 4 = 0;
SELECT count(*) AS moved FROM pos_hot WHERE id < 0;
-- the index keeps a chain's root offset, so its entries are compared with the
-- heap's by block; the phrases, whose answer differs between neighbours on a
-- page, check each row got its own positions
CREATE FUNCTION pos_hot_check(OUT heap_rows bigint, OUT only_index bigint,
							  OUT only_heap bigint, OUT phrases bigint,
							  OUT matched numeric, OUT differ bigint)
LANGUAGE plpgsql AS $$
BEGIN
	WITH idx AS (
		SELECT l.lexeme, (p.tid::text::point)[0] AS blk, p.positions, p.weights
		FROM (SELECT DISTINCT unnest(tsvector_to_array(d)) AS lexeme FROM pos_hot) l,
			 lion_debug_key_positions('pos_hot_d', l.lexeme) p),
	heap AS (
		SELECT u.lexeme, (pos_hot.ctid::text::point)[0] AS blk, u.positions, u.weights
		FROM pos_hot, unnest(d) u)
	SELECT (SELECT count(*) FROM heap),
		   (SELECT count(*) FROM (SELECT * FROM idx EXCEPT ALL SELECT * FROM heap) x),
		   (SELECT count(*) FROM (SELECT * FROM heap EXCEPT ALL SELECT * FROM idx) x)
	INTO heap_rows, only_index, only_heap;
	PERFORM set_config('enable_seqscan', 'off', true);
	CREATE TEMP TABLE pos_hot_idx AS
	SELECT format('w%s <-> hot%s', w, h)::tsquery AS q, 0::bigint AS n
	FROM generate_series(0, 49) w, generate_series(0, 2) h;
	UPDATE pos_hot_idx SET n = (SELECT count(*) FROM pos_hot WHERE d @@ q);
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	SELECT count(*), sum(i.n),
		   count(*) FILTER (WHERE i.n IS DISTINCT FROM
			   (SELECT count(*) FROM pos_hot WHERE d @@ i.q))
	INTO phrases, matched, differ
	FROM pos_hot_idx i;
	DROP TABLE pos_hot_idx;
END $$;
SET maintenance_work_mem = '1MB';
REINDEX INDEX pos_hot_d;
RESET maintenance_work_mem;
SELECT * FROM pos_hot_check();
SELECT lion_index_verify('pos_hot_d', true);
-- A parallel build: each participant keeps the positions of its share of the
-- heap, and the leader merges a key's by heap block (PostgreSQL 17 and later;
-- before, the same build runs serially)
ALTER TABLE pos_hot SET (parallel_workers = 2);
SET max_parallel_maintenance_workers = 2;
SET maintenance_work_mem = '96MB';
REINDEX INDEX pos_hot_d;
RESET maintenance_work_mem;
RESET max_parallel_maintenance_workers;
SELECT * FROM pos_hot_check();
SELECT lion_index_verify('pos_hot_d', true);
DROP FUNCTION pos_hot_check();
DROP TABLE pos_hot;

-- Queries answered from the positions (lion_posfilter.c): the posting sets
-- give the candidates, and each one is decided from its stored positions, so
-- a bitmap scan or an index scan returns exactly what the heap's @@ does and
-- has nothing to recheck.  Every shape against a seq scan, over inline keys
-- and position trees, with stripped rows and the last position there is.
INSERT INTO pos_docs VALUES
	(20001, strip(to_tsvector('simple', 'common again common'))),
	(20002, 'near:16382 far:16383'),
	(20003, 'far:1A near:3B,16383C');
CREATE TEMP TABLE pos_queries (q tsquery);
INSERT INTO pos_queries VALUES
	('common <-> again'), ('again <-> common'), ('again <2> common'),
	('common <-> again <-> common'), ('everywhere <-> again <-> again'),
	('dense <-> dense'), ('dense <-> few1'), ('dense <200> few2'),
	('w1:A'), ('w1:B & w2'), ('w1:AB <-> w2'), ('w2:D <-> w1:CD'),
	('common & !again'), ('everywhere & !again'), ('again & !(common <-> again)'),
	('common <-> !again'), ('!common <-> again'), ('common & !nosuch'),
	('(common <-> again) | (w1 <-> w2)'), ('(common | dense) <-> again'),
	('common <-> (again | new5)'), ('near <-> far'), ('far <-> near'),
	('near:C <2> far'), ('far:A <2> near:B'), ('nosuch <-> common'),
	('w5 <-> w5'), ('rare3 <-> common'), ('common <-> ag:*');
SET enable_seqscan = off;
SET enable_indexscan = off;
CREATE TEMP TABLE pos_by_index AS
SELECT q::text, (SELECT array_agg(id ORDER BY id) FROM pos_docs WHERE d @@ q) AS ids
FROM pos_queries;
RESET enable_indexscan;
SET enable_bitmapscan = off;
CREATE TEMP TABLE pos_by_iscan AS
SELECT q::text, (SELECT array_agg(id ORDER BY id) FROM pos_docs WHERE d @@ q) AS ids
FROM pos_queries;
RESET enable_seqscan;
SET enable_indexscan = off;
CREATE TEMP TABLE pos_by_heap AS
SELECT q::text, (SELECT array_agg(id ORDER BY id) FROM pos_docs WHERE d @@ q) AS ids
FROM pos_queries;
RESET enable_bitmapscan;
RESET enable_indexscan;
SELECT i.q, coalesce(cardinality(h.ids), 0) AS rows,
	   i.ids IS NOT DISTINCT FROM h.ids AS bitmap_same,
	   s.ids IS NOT DISTINCT FROM h.ids AS iscan_same
FROM pos_by_index i JOIN pos_by_heap h USING (q) JOIN pos_by_iscan s USING (q)
ORDER BY i.q;

-- what the bitmap heap scan still rechecks: nothing, but for a prefix
-- lexeme, which names keys the filter cannot follow
CREATE FUNCTION pos_rechecked(q tsquery) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE
	l text;
BEGIN
	SET LOCAL enable_seqscan = off;
	SET LOCAL enable_indexscan = off;
	FOR l IN EXECUTE format('EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) '
							'SELECT id FROM pos_docs WHERE d @@ %L', q) LOOP
		IF l LIKE '%Rows Removed by Index Recheck%' THEN
			RETURN true;
		END IF;
	END LOOP;
	RETURN false;
END $$;
SELECT q, pos_rechecked(q) FROM (VALUES
	('common <-> again'::tsquery), ('everywhere & !again'), ('w1:A'),
	('common <-> ag:*')) v(q);
DROP FUNCTION pos_rechecked(tsquery);

-- `@@ ANY (array)` in a plain index scan: the union of its elements, each
-- element's superset through a filter of its own (LION_KN_POSFILTER), so
-- the heap rechecks nothing unless an element has a prefix lexeme
CREATE TEMP TABLE pos_arrays AS
SELECT ARRAY[a.q, b.q] AS arr FROM pos_queries a, pos_queries b
WHERE a.q::text < b.q::text
UNION ALL SELECT ARRAY[q, NULL] FROM pos_queries;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
CREATE TEMP TABLE pos_any_iscan AS
SELECT arr::text AS a,
	   (SELECT array_agg(id ORDER BY id) FROM pos_docs WHERE d @@ ANY (arr)) AS ids
FROM pos_arrays;
RESET enable_seqscan;
SET enable_indexscan = off;
CREATE TEMP TABLE pos_any_heap AS
SELECT arr::text AS a,
	   (SELECT array_agg(id ORDER BY id) FROM pos_docs WHERE d @@ ANY (arr)) AS ids
FROM pos_arrays;
RESET enable_bitmapscan;
RESET enable_indexscan;
SELECT count(*) AS arrays,
	   count(*) FILTER (WHERE i.ids IS DISTINCT FROM h.ids) AS differ
FROM pos_any_iscan i JOIN pos_any_heap h USING (a);
CREATE FUNCTION pos_any_explain(arr tsquery[]) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	l text;
BEGIN
	SET LOCAL enable_seqscan = off;
	SET LOCAL enable_bitmapscan = off;
	FOR l IN EXECUTE format('EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) '
							'SELECT id FROM pos_docs WHERE d @@ ANY (%L)', arr) LOOP
		CONTINUE WHEN l LIKE '%Index Searches:%';
		RETURN NEXT regexp_replace(l, ' \(actual .*\)$', '');
	END LOOP;
END $$;
SELECT pos_any_explain('{"common <-> again", "far <-> near"}');
SELECT pos_any_explain('{"common <-> again", "common <-> ag:*"}');
DROP FUNCTION pos_any_explain(tsquery[]);

-- The count pushdown decides the same candidates from the positions before
-- it asks the visibility map, so a vacuumed table is counted without the
-- heap; a literal query and a generic plan's parameter alike
VACUUM pos_docs;
CREATE TEMP TABLE pos_counted AS
SELECT q::text, (SELECT count(*) FROM pos_docs WHERE d @@ q) AS n FROM pos_queries;
PREPARE pos_count(tsquery) AS SELECT count(*) FROM pos_docs WHERE d @@ $1;
SET plan_cache_mode = force_generic_plan;
CREATE TEMP TABLE pos_generic (q text, n bigint);
DO $$
DECLARE
	r record;
	n bigint;
BEGIN
	FOR r IN SELECT q FROM pos_queries LOOP
		EXECUTE format('EXECUTE pos_count(%L)', r.q) INTO n;
		INSERT INTO pos_generic VALUES (r.q::text, n);
	END LOOP;
END $$;
RESET plan_cache_mode;
DEALLOCATE pos_count;
SELECT c.q, c.n AS counted, g.n AS generic, coalesce(cardinality(h.ids), 0) AS heap
FROM pos_counted c JOIN pos_generic g USING (q) JOIN pos_by_heap h USING (q)
WHERE c.n <> coalesce(cardinality(h.ids), 0) OR g.n <> c.n;
CREATE FUNCTION pos_count_explain(q tsquery) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE
	l text;
BEGIN
	FOR l IN EXECUTE format('EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) '
							'SELECT count(*) FROM pos_docs WHERE d @@ %L', q) LOOP
		IF l ~ '(Custom Scan|Heap TIDs Rechecked|Position|Removed)' THEN
			RETURN NEXT regexp_replace(l, ' \(actual .*\)$', '');
		END IF;
	END LOOP;
END $$;
SELECT pos_count_explain('common <-> again');
SELECT pos_count_explain('common <-> ag:*');
DROP FUNCTION pos_count_explain(tsquery);

-- ... and per group: the walk that intersects a copy of the WHERE with every
-- group's set filters the copy once a key (lion_count_groups.c), the
-- distinct count and the join's per-key counts likewise
CREATE TABLE pos_grouped (id int, cat int, d tsvector) WITH (autovacuum_enabled = off);
INSERT INTO pos_grouped SELECT id, id % 7, d FROM pos_docs;
CREATE INDEX pos_grouped_cd ON pos_grouped USING lion (cat, d) WITH (store_positions = true);
CREATE TABLE pos_dim (id int PRIMARY KEY, name text);
INSERT INTO pos_dim SELECT g, 'n' || (g % 3) FROM generate_series(0, 6) g;
VACUUM ANALYZE pos_grouped, pos_dim;
EXPLAIN (COSTS OFF)
SELECT cat, count(*) FROM pos_grouped WHERE d @@ 'everywhere <-> again' GROUP BY cat;
EXPLAIN (COSTS OFF)
SELECT count(DISTINCT cat) FROM pos_grouped WHERE d @@ 'common <-> again & !new5';
-- PostgreSQL 19 may aggregate below the join instead (eager aggregation)
DO $$BEGIN IF EXISTS (SELECT 1 FROM pg_settings WHERE name = 'enable_eager_aggregate') THEN PERFORM set_config('enable_eager_aggregate', 'off', false); END IF; END$$;
EXPLAIN (COSTS OFF)
SELECT pos_dim.name, count(*)
FROM pos_grouped JOIN pos_dim ON pos_grouped.cat = pos_dim.id
WHERE pos_grouped.d @@ 'w1:A | (common <-> again)' GROUP BY pos_dim.name;
CREATE TEMP TABLE pos_pushed AS
SELECT 'group' AS what, cat::text AS k, count(*) AS n
FROM pos_grouped WHERE d @@ 'everywhere <-> again' GROUP BY cat
UNION ALL
SELECT 'distinct', NULL, count(DISTINCT cat)
FROM pos_grouped WHERE d @@ 'common <-> again & !new5'
UNION ALL
SELECT 'join', pos_dim.name, count(*)
FROM pos_grouped JOIN pos_dim ON pos_grouped.cat = pos_dim.id
WHERE pos_grouped.d @@ 'w1:A | (common <-> again)' GROUP BY pos_dim.name;
DO $$BEGIN IF EXISTS (SELECT 1 FROM pg_settings WHERE name = 'enable_eager_aggregate') THEN EXECUTE 'RESET enable_eager_aggregate'; END IF; END$$;
SET pg_lion.enable_count_pushdown = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SELECT what, count(*) AS rows,
	   count(*) FILTER (WHERE n IS DISTINCT FROM heap_n) AS differ
FROM (SELECT p.what, p.n,
			 CASE p.what
				 WHEN 'group' THEN (SELECT count(*) FROM pos_grouped
									WHERE d @@ 'everywhere <-> again' AND cat::text = p.k)
				 WHEN 'distinct' THEN (SELECT count(DISTINCT cat) FROM pos_grouped
									   WHERE d @@ 'common <-> again & !new5')
				 ELSE (SELECT count(*) FROM pos_grouped JOIN pos_dim
					   ON pos_grouped.cat = pos_dim.id
					   WHERE pos_grouped.d @@ 'w1:A | (common <-> again)'
					   AND pos_dim.name = p.k)
			 END AS heap_n
	  FROM pos_pushed p) x
GROUP BY what ORDER BY what;
RESET pg_lion.enable_count_pushdown;
RESET enable_bitmapscan;
RESET enable_indexscan;

-- Under an OR no recheck sees a leaf alone, so a leaf the sets only bound is
-- decided from the positions as the count walks it (LION_KN_POSFILTER): two
-- queries ORed, a query ORed with another column, the same with a GROUP BY,
-- and `@@ ANY (array)`, which is the OR of its elements' queries, each
-- counted by the pushdown and by the heap.  At the smallest
-- work_mem the union is read a window at a time, without its pins.
CREATE FUNCTION pos_or_count(sql text, pushed OUT bool, n OUT text)
LANGUAGE plpgsql AS $$
DECLARE
	l text;
BEGIN
	pushed := false;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || sql LOOP
		pushed := pushed OR l LIKE '%Custom Scan (LionCount)%';
	END LOOP;
	EXECUTE 'SELECT string_agg(x::text, '','' ORDER BY x::text) FROM (' ||
		sql || ') x' INTO n;
END $$;
CREATE TEMP TABLE pos_or_shapes AS
SELECT format('SELECT count(*) FROM pos_grouped WHERE d @@ %L OR d @@ %L',
			  a.q, b.q) AS s
FROM pos_queries a, pos_queries b WHERE a.q::text < b.q::text
UNION ALL
SELECT format('SELECT count(*) FROM pos_grouped WHERE d @@ %L OR cat = 3', q)
FROM pos_queries
UNION ALL
SELECT format('SELECT cat, count(*) FROM pos_grouped WHERE d @@ %L OR d @@ %L '
			  'GROUP BY cat', a.q, b.q)
FROM pos_queries a, pos_queries b
WHERE a.q::text < b.q::text AND b.q IN ('common <-> again', 'w1:A')
UNION ALL
SELECT format('SELECT count(*) FROM pos_grouped WHERE d @@ ANY (%L::tsquery[])',
			  ARRAY[a.q, b.q])
FROM pos_queries a, pos_queries b WHERE a.q::text < b.q::text
UNION ALL
SELECT format('SELECT cat, count(*) FROM pos_grouped '
			  'WHERE d @@ ANY (%L::tsquery[]) GROUP BY cat', ARRAY[q, NULL, q])
FROM pos_queries;
CREATE TEMP TABLE pos_or_pushed AS
SELECT s, (pos_or_count(s)).* FROM pos_or_shapes;
SET work_mem = '64kB';
CREATE TEMP TABLE pos_or_narrow AS
SELECT s, (pos_or_count(s)).* FROM pos_or_shapes;
RESET work_mem;
SET pg_lion.enable_count_pushdown = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
CREATE TEMP TABLE pos_or_heap AS
SELECT s, (pos_or_count(s)).* FROM pos_or_shapes;
RESET pg_lion.enable_count_pushdown;
RESET enable_bitmapscan;
RESET enable_indexscan;
-- every shape but those with a prefix lexeme is pushed down
SELECT count(*) AS shapes,
	   count(*) FILTER (WHERE p.pushed) AS pushed,
	   count(*) FILTER (WHERE NOT p.pushed AND p.s NOT LIKE '%:*%') AS not_pushed,
	   count(*) FILTER (WHERE p.n IS DISTINCT FROM h.n) AS differ,
	   count(*) FILTER (WHERE w.n IS DISTINCT FROM h.n) AS differ_narrow
FROM pos_or_pushed p JOIN pos_or_narrow w USING (s) JOIN pos_or_heap h USING (s);
DO $$
DECLARE
	l text;
BEGIN
	FOR l IN EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF)
		SELECT count(*) FROM pos_grouped
		WHERE d @@ 'common <-> again' OR d @@ 'w1:A' LOOP
		IF l ~ '(Custom Scan|Heap TIDs Rechecked|Position|Removed)' THEN
			RAISE WARNING '%', regexp_replace(l, ' \(actual .*\)$', '');
		END IF;
	END LOOP;
END $$;
DROP FUNCTION pos_or_count(text);
DROP TABLE pos_grouped, pos_dim;

-- store_positions is off by default, read at build time only and recorded
-- on the meta page: ALTER INDEX changes nothing until a REINDEX.
CREATE TABLE pos_opt (id int, d tsvector) WITH (autovacuum_enabled = off);
INSERT INTO pos_opt
SELECT i, to_tsvector('simple', 'a b c ' || (i % 7)) FROM generate_series(1, 500) i;
CREATE INDEX pos_opt_d ON pos_opt USING lion (d);
SELECT count(*) FROM lion_debug_key_positions('pos_opt_d', 'a');
CREATE INDEX pos_opt_id ON pos_opt USING lion (id) WITH (store_positions = true);
ALTER INDEX pos_opt_d SET (store_positions = true);
INSERT INTO pos_opt VALUES (501, to_tsvector('simple', 'a b'));
SELECT count(*) FROM lion_debug_key_positions('pos_opt_d', 'a');
REINDEX INDEX pos_opt_d;
SELECT count(*) FROM lion_debug_key_positions('pos_opt_d', 'a');
ALTER INDEX pos_opt_d SET (store_positions = false);
INSERT INTO pos_opt VALUES (502, to_tsvector('simple', 'a b'));
SELECT count(*) FROM lion_debug_key_positions('pos_opt_d', 'a');
-- an OR of phrases is pushed down only while the index stores positions
VACUUM pos_opt;
SET enable_seqscan = off;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM pos_opt WHERE d @@ 'a <-> b' OR d @@ 'b <-> c';
SELECT count(*) FROM pos_opt WHERE d @@ 'a <-> b' OR d @@ 'b <-> c';
REINDEX INDEX pos_opt_d;
SELECT count(*) FROM lion_debug_key_positions('pos_opt_d', 'a');
EXPLAIN (COSTS OFF)
SELECT count(*) FROM pos_opt WHERE d @@ 'a <-> b' OR d @@ 'b <-> c';
SELECT count(*) FROM pos_opt WHERE d @@ 'a <-> b' OR d @@ 'b <-> c';
RESET enable_seqscan;
DROP TABLE pos_opt;

DROP TABLE pos_docs CASCADE;
DROP FUNCTION lion_debug_key_positions(regclass, text);
