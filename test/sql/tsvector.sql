-- Multi-key operator classes: tsvector (DESIGN.md section 17).
--
-- `tsvector_ops` indexes one row under every lexeme of its tsvector, with
-- text keys.  A tsquery of AND and OR over plain lexemes becomes a boolean
-- tree over those keys and is answered exactly; NOT, phrase search, prefixes
-- and weights need positions or ranges the index does not have, and fall back
-- to scanning every indexed row with a recheck.
--
-- Every query is run through the index and through a forced sequential scan
-- and the two results compared as multisets.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'roaring_index';

/* VACUUM can only set all-visible for commits that reached disk (citext.sql). */
SET synchronous_commit = on;
SET default_statistics_target = 1000;

CREATE EXTENSION IF NOT EXISTS roaring_index;

CREATE FUNCTION rbi_tscmp(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	node text := 'seq scan';
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('roaring_index.enable_count_pushdown', 'on', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (RoaringCount)%' THEN
			node := 'pushed down';
		ELSIF ln LIKE '%Bitmap Index Scan%' THEN
			node := 'index scan';
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE rbi_t_idx AS %s', q);

	PERFORM set_config('roaring_index.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	EXECUTE format('CREATE TEMP TABLE rbi_t_seq AS %s', q);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);

	EXECUTE 'SELECT count(*) FROM rbi_t_idx' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM rbi_t_idx EXCEPT ALL SELECT * FROM rbi_t_seq) a)'
			' + (SELECT count(*) FROM (SELECT * FROM rbi_t_seq EXCEPT ALL SELECT * FROM rbi_t_idx) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE rbi_t_idx, rbi_t_seq';

	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows', node, nrows);
END $$;

CREATE FUNCTION rbi_tsplan(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		RETURN NEXT ln;
	END LOOP;
	PERFORM set_config('enable_seqscan', 'on', true);
END $$;

-- ---- the table ---------------------------------------------------------
--
-- A few hundred generated sentences over a vocabulary of ~60 words, so that
-- the posting sets are big enough to be interesting and the answers small
-- enough to compare.  Row 0's text is empty, so its tsvector has no lexeme at
-- all and the row lands in the reserved EMPTY entry; row -1's is NULL.
CREATE TABLE rbi_ts (
	id		int		NOT NULL,
	grp		int		NOT NULL,	-- 6 groups, scalar roaring index
	body	text,
	tsv		tsvector
);

INSERT INTO rbi_ts (id, grp, body)
SELECT i, i % 6,
	   'alpha w' || (i % 17) || ' w' || (i % 23) || ' w' || (i % 29) ||
	   ' bravo charlie delta w' || (i % 31)
  FROM generate_series(1, 600) i;
INSERT INTO rbi_ts (id, grp, body) VALUES (0, 0, '');
INSERT INTO rbi_ts (id, grp, body) VALUES (-1, 1, NULL);
UPDATE rbi_ts SET tsv = to_tsvector('english', coalesce(body, ''))
 WHERE body IS NOT NULL;

CREATE INDEX rbi_ts_tsv ON rbi_ts USING roaring (tsv);
CREATE INDEX rbi_ts_grp ON rbi_ts USING roaring (grp);
VACUUM ANALYZE rbi_ts;

-- The key column of a tsvector_ops index is text: the lexeme.
SELECT a.attname, a.atttypid::regtype
  FROM pg_attribute a
 WHERE a.attrelid = 'rbi_ts_tsv'::regclass AND a.attnum > 0;

SELECT roaring_index_verify('rbi_ts_tsv', true);
SELECT entries, null_tids, empty_tids FROM roaring_index_stats('rbi_ts_tsv');

-- ---- plans -------------------------------------------------------------
SELECT rbi_tsplan($$SELECT id FROM rbi_ts
					WHERE tsv @@ to_tsquery('english', 'w1 & w2')$$);
SELECT rbi_tsplan($$SELECT count(*) FROM rbi_ts
					WHERE tsv @@ to_tsquery('english', 'w1 & w2')$$);
SELECT rbi_tsplan($$SELECT count(*) FROM rbi_ts
					WHERE tsv @@ to_tsquery('english', 'w1 | w2')$$);
-- NOT, prefix, phrase and weights are never pushed down
SELECT rbi_tsplan($$SELECT count(*) FROM rbi_ts
					WHERE tsv @@ to_tsquery('english', 'alpha & !w1')$$);
SELECT rbi_tsplan($$SELECT count(*) FROM rbi_ts
					WHERE tsv @@ to_tsquery('english', 'w1:*')$$);
SELECT rbi_tsplan($$SELECT count(*) FROM rbi_ts
					WHERE tsv @@ phraseto_tsquery('english', 'bravo charlie')$$);
SELECT rbi_tsplan($$SELECT count(*) FROM rbi_ts
					WHERE tsv @@ to_tsquery('english', 'alpha:A')$$);
SELECT rbi_tsplan($$SELECT grp, count(*) FROM rbi_ts
					WHERE tsv @@ to_tsquery('english', 'w1 & w2') GROUP BY grp$$);

-- ---- AND, OR, nested parentheses ---------------------------------------
SELECT rbi_tscmp($$SELECT id FROM rbi_ts WHERE tsv @@ to_tsquery('english', 'w1')$$);
SELECT rbi_tscmp($$SELECT id FROM rbi_ts WHERE tsv @@ to_tsquery('english', 'w1 & w2')$$);
SELECT rbi_tscmp($$SELECT id FROM rbi_ts WHERE tsv @@ to_tsquery('english', 'w1 | w2')$$);
SELECT rbi_tscmp($$SELECT id FROM rbi_ts
				  WHERE tsv @@ to_tsquery('english', 'alpha & (w1 | w2)')$$);
SELECT rbi_tscmp($$SELECT id FROM rbi_ts
				  WHERE tsv @@ to_tsquery('english', '(w1 | w2) & (w3 | w4)')$$);
SELECT rbi_tscmp($$SELECT id FROM rbi_ts
				  WHERE tsv @@ to_tsquery('english', 'alpha & bravo & charlie & delta')$$);
SELECT rbi_tscmp($$SELECT id FROM rbi_ts
				  WHERE tsv @@ to_tsquery('english', '((w1 | w2) & w3) | (w4 & w5)')$$);
-- a lexeme that matches nothing, alone and inside a tree
SELECT rbi_tscmp($$SELECT id FROM rbi_ts WHERE tsv @@ to_tsquery('english', 'nosuchword')$$);
SELECT rbi_tscmp($$SELECT id FROM rbi_ts
				  WHERE tsv @@ to_tsquery('english', 'alpha & nosuchword')$$);
SELECT rbi_tscmp($$SELECT id FROM rbi_ts
				  WHERE tsv @@ to_tsquery('english', 'alpha | nosuchword')$$);

-- ---- two index quals, and `@@ ANY (...)` --------------------------------
-- The access method answers ONE qual per scan and marks every TID for
-- recheck, so the bitmap heap scan applies the rest (DESIGN.md §5).
SELECT rbi_tscmp($$SELECT id FROM rbi_ts
				  WHERE tsv @@ to_tsquery('english', 'w1')
					AND tsv @@ to_tsquery('english', 'w2')$$);
-- `@@ ANY (array of tsqueries)` is the union of the queries' answers, which
-- an array_ops index cannot be given (an array of arrays does not exist).
SELECT rbi_tscmp($$SELECT id FROM rbi_ts
				  WHERE tsv @@ ANY (ARRAY['w1 & w2', 'w3 & w4']::tsquery[])$$);
-- one element of the list needs the ALL fallback: the union still holds
SELECT rbi_tscmp($$SELECT id FROM rbi_ts
				  WHERE tsv @@ ANY (ARRAY['w1 & w2', '!w3']::tsquery[])$$);

-- ---- the fallbacks -----------------------------------------------------
SELECT rbi_tscmp($$SELECT id FROM rbi_ts
				  WHERE tsv @@ to_tsquery('english', 'alpha & !w1')$$);
SELECT rbi_tscmp($$SELECT id FROM rbi_ts
				  WHERE tsv @@ to_tsquery('english', '!w1')$$);
SELECT rbi_tscmp($$SELECT id FROM rbi_ts WHERE tsv @@ to_tsquery('english', 'w1:*')$$);
SELECT rbi_tscmp($$SELECT id FROM rbi_ts
				  WHERE tsv @@ to_tsquery('english', 'alpha & w1:*')$$);
SELECT rbi_tscmp($$SELECT id FROM rbi_ts
				  WHERE tsv @@ phraseto_tsquery('english', 'bravo charlie')$$);
SELECT rbi_tscmp($$SELECT id FROM rbi_ts
				  WHERE tsv @@ to_tsquery('english', 'bravo <-> charlie')$$);
SELECT rbi_tscmp($$SELECT id FROM rbi_ts
				  WHERE tsv @@ to_tsquery('english', 'bravo <2> delta')$$);
SELECT rbi_tscmp($$SELECT id FROM rbi_ts WHERE tsv @@ to_tsquery('english', 'alpha:A')$$);
SELECT rbi_tscmp($$SELECT id FROM rbi_ts
				  WHERE tsv @@ to_tsquery('english', 'alpha:* & bravo:B')$$);

-- plainto_tsquery is a plain AND, so it is answered exactly
SELECT rbi_tscmp($$SELECT id FROM rbi_ts
				  WHERE tsv @@ plainto_tsquery('english', 'bravo charlie')$$);
SELECT rbi_tscmp($$SELECT id FROM rbi_ts
				  WHERE tsv @@ plainto_tsquery('english', 'alpha w1')$$);

-- NULL and empty tsvectors
SELECT rbi_tscmp($$SELECT id FROM rbi_ts WHERE tsv IS NULL$$);
SELECT rbi_tscmp($$SELECT id FROM rbi_ts WHERE tsv IS NOT NULL$$);

-- ---- the count pushdown ------------------------------------------------
SELECT rbi_tscmp($$SELECT count(*) FROM rbi_ts
				  WHERE tsv @@ to_tsquery('english', 'w1 & w2')$$);
SELECT rbi_tscmp($$SELECT count(*) FROM rbi_ts
				  WHERE tsv @@ to_tsquery('english', 'w1 | w2')$$);
SELECT rbi_tscmp($$SELECT count(*) FROM rbi_ts
				  WHERE tsv @@ to_tsquery('english', 'alpha & (w1 | w2)')$$);
SELECT rbi_tscmp($$SELECT count(*) FROM rbi_ts
				  WHERE tsv @@ to_tsquery('english', 'alpha & !w1')$$);
SELECT rbi_tscmp($$SELECT count(*) FROM rbi_ts
				  WHERE tsv @@ to_tsquery('english', 'w1 & w2') AND grp = 3$$);

-- the counts themselves
SELECT count(*) FROM rbi_ts WHERE tsv @@ to_tsquery('english', 'w1 & w2');
SELECT count(*) FROM rbi_ts WHERE tsv @@ to_tsquery('english', 'w1 | w2');
SELECT count(*) FROM rbi_ts WHERE tsv @@ to_tsquery('english', 'alpha & (w1 | w2)');
SELECT count(*) FROM rbi_ts WHERE tsv @@ to_tsquery('english', 'nosuchword');

-- ---- GROUP BY a scalar column with a tsquery WHERE clause --------------
SELECT rbi_tscmp($$SELECT grp, count(*) FROM rbi_ts
				  WHERE tsv @@ to_tsquery('english', 'w1 & w2') GROUP BY grp$$);
SELECT rbi_tscmp($$SELECT grp, count(*) FROM rbi_ts
				  WHERE tsv @@ to_tsquery('english', 'w1 | w2') GROUP BY grp$$);
SELECT grp, count(*) FROM rbi_ts
 WHERE tsv @@ to_tsquery('english', 'w1 & w2') GROUP BY grp ORDER BY grp;

-- ---- writes ------------------------------------------------------------
INSERT INTO rbi_ts (id, grp, body, tsv)
SELECT i, i % 6, 'echo foxtrot w' || (i % 17),
	   to_tsvector('english', 'echo foxtrot w' || (i % 17))
  FROM generate_series(601, 900) i;
INSERT INTO rbi_ts (id, grp, body, tsv) VALUES (1000, 2, '', to_tsvector('english', ''));
INSERT INTO rbi_ts (id, grp, body, tsv) VALUES (1001, 2, NULL, NULL);

SELECT roaring_index_verify('rbi_ts_tsv', true);
SELECT entries, null_tids, empty_tids FROM roaring_index_stats('rbi_ts_tsv');

-- a dirty heap
SELECT rbi_tscmp($$SELECT id FROM rbi_ts
				  WHERE tsv @@ to_tsquery('english', 'echo & foxtrot')$$);
SELECT rbi_tscmp($$SELECT count(*) FROM rbi_ts
				  WHERE tsv @@ to_tsquery('english', 'echo & w3')$$);

UPDATE rbi_ts SET tsv = to_tsvector('english', 'golf hotel')
 WHERE id BETWEEN 100 AND 150;
DELETE FROM rbi_ts WHERE id BETWEEN 200 AND 250;

SELECT roaring_index_verify('rbi_ts_tsv', true);
SELECT rbi_tscmp($$SELECT id FROM rbi_ts
				  WHERE tsv @@ to_tsquery('english', 'golf & hotel')$$);
SELECT rbi_tscmp($$SELECT count(*) FROM rbi_ts
				  WHERE tsv @@ to_tsquery('english', 'alpha & bravo')$$);

VACUUM rbi_ts;
SELECT roaring_index_verify('rbi_ts_tsv', true);

VACUUM ANALYZE rbi_ts;
-- an all-visible heap: the counts come out of the visibility map
SELECT rbi_tscmp($$SELECT count(*) FROM rbi_ts
				  WHERE tsv @@ to_tsquery('english', 'alpha & bravo')$$);
SELECT rbi_tscmp($$SELECT count(*) FROM rbi_ts
				  WHERE tsv @@ to_tsquery('english', 'golf | echo')$$);
SELECT rbi_tscmp($$SELECT grp, count(*) FROM rbi_ts
				  WHERE tsv @@ to_tsquery('english', 'w1 & w2') GROUP BY grp$$);
SELECT roaring_index_verify('rbi_ts_tsv', true);

-- ---- the EMPTY entry spilling (2026-09-20 review, finding 1) ----------
/*
 * A tsvector with no lexemes belongs to the reserved EMPTY entry, which must
 * stay the EMPTY entry when its posting set moves onto container pages - the
 * array case is argued in array.sql; here the rows are empty documents.
 */
CREATE TABLE rbi_ts_empty (id int NOT NULL, tsv tsvector);
CREATE INDEX rbi_ts_empty_tsv ON rbi_ts_empty USING roaring (tsv)
	WITH (inline_limit = 64);
INSERT INTO rbi_ts_empty
SELECT i, CASE WHEN i % 2 = 0 THEN to_tsvector('simple', '')
			   ELSE to_tsvector('simple', 'onlyword') END
  FROM generate_series(1, 6000) i;
-- one EMPTY entry with 3000 rows and one lexeme entry, both spilled
SELECT entries, inline_entries, null_tids, empty_tids,
	   container_pages > 0 AS spilled
  FROM roaring_index_stats('rbi_ts_empty_tsv');
SELECT roaring_index_verify('rbi_ts_empty_tsv', true);
-- these rows must join the EMPTY entry, not start a second one
INSERT INTO rbi_ts_empty VALUES (10001, to_tsvector('simple', '')), (10002, NULL);
SELECT entries, null_tids, empty_tids
  FROM roaring_index_stats('rbi_ts_empty_tsv');
SELECT roaring_index_verify('rbi_ts_empty_tsv', true);
-- and once more after a VACUUM has rewritten the entries
DELETE FROM rbi_ts_empty WHERE id % 4 = 0;
VACUUM rbi_ts_empty;
SELECT entries, null_tids, empty_tids
  FROM roaring_index_stats('rbi_ts_empty_tsv');
SELECT roaring_index_verify('rbi_ts_empty_tsv', true);
DROP TABLE rbi_ts_empty;

-- ---- max_entries on a lexeme index -------------------------------------
CREATE TABLE rbi_ts_many (tsv tsvector);
INSERT INTO rbi_ts_many
SELECT to_tsvector('simple', 'lex' || i || ' lex' || (i + 1))
  FROM generate_series(1, 3000) i;
CREATE INDEX rbi_ts_many_tsv ON rbi_ts_many USING roaring (tsv)
	WITH (max_entries = 500);
SELECT entries > 500 FROM roaring_index_stats('rbi_ts_many_tsv');
SELECT roaring_index_verify('rbi_ts_many_tsv', true);

DROP TABLE rbi_ts_many;
DROP TABLE rbi_ts;
DROP FUNCTION rbi_tscmp(text);
DROP FUNCTION rbi_tsplan(text);
