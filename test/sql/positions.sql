-- Stored positions (DESIGN.md §17, "Stored positions"): insert and VACUUM.
--
-- A column whose multi-key opclass has support function 5 stores, for every
-- key, each row's word positions and weights: in the entry while it is
-- INLINE, in a position tree once the entry spills.  Nothing reads them yet,
-- so the opclass is not part of the extension: this test creates it, and a
-- test function that lists what the index stores for one key, and compares
-- that with unnest() of the rows' tsvectors.
--
-- Not done yet: CREATE INDEX over existing rows (an error below).  The table
-- keeps autovacuum off so that the VACUUMs below are the only ones.

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

-- the build does not write positions yet
CREATE INDEX pos_docs_d2 ON pos_docs USING lion (d tsvector_pos_ops);

DROP TABLE pos_docs CASCADE;
DROP OPERATOR CLASS tsvector_pos_ops USING lion;
DROP FUNCTION lion_debug_key_positions(regclass, text);
DROP FUNCTION lion_tsvector_positions(tsvector, internal);
