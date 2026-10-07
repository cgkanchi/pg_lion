-- Stored positions (DESIGN.md §17, "Stored positions"): insert, VACUUM,
-- CREATE INDEX, and the bitmap scans they answer.
--
-- A column whose multi-key opclass has support function 5 stores, for every
-- key, each row's word positions and weights: in the entry while it is
-- INLINE, in a position tree once the entry spills.  The opclass is not part
-- of the extension yet: this test creates it, and a test function that lists
-- what the index stores for one key, and compares that with unnest() of the
-- rows' tsvectors.
--
-- The table keeps autovacuum off so that the VACUUMs below are the only ones.

\set VERBOSITY terse
SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS pg_lion;

CREATE FUNCTION lion_tsvector_positions(tsvector, internal) RETURNS internal
	AS '$libdir/pg_lion' LANGUAGE C STRICT;
CREATE FUNCTION lion_debug_key_positions(regclass, text,
	OUT tid tid, OUT positions int2[], OUT weights text[]) RETURNS SETOF record
	AS '$libdir/pg_lion' LANGUAGE C STRICT;
CREATE OPERATOR CLASS tsvector_pos_ops FOR TYPE tsvector USING lion AS
	OPERATOR	5	@@ (tsvector, tsquery),
	FUNCTION	1	hashtext(text),
	FUNCTION	4	bttextcmp(text, text),
	FUNCTION	2	gin_extract_tsvector(tsvector, internal, internal),
	FUNCTION	3	gin_extract_tsquery(tsvector, internal, int2, internal, internal, internal, internal),
	FUNCTION	5	lion_tsvector_positions(tsvector, internal),
	STORAGE		text;

CREATE TABLE pos_docs (id int, d tsvector) WITH (autovacuum_enabled = off);
CREATE INDEX pos_docs_d ON pos_docs USING lion (d tsvector_pos_ops);

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
			RETURN NEXT l;
		END IF;
	END LOOP;
END $$;
SELECT pos_count_explain('common <-> again');
SELECT pos_count_explain('common <-> ag:*');
DROP FUNCTION pos_count_explain(tsquery);

DROP TABLE pos_docs CASCADE;
DROP OPERATOR CLASS tsvector_pos_ops USING lion;
DROP FUNCTION lion_debug_key_positions(regclass, text);
DROP FUNCTION lion_tsvector_positions(tsvector, internal);
