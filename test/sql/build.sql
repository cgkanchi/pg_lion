-- The build (DESIGN.md §24, "Build"): every row's code appended to its key's
-- entry in memory, spilled in sorted runs when maintenance_work_mem runs out,
-- and merged back in directory order.  Whatever the route, the index is the
-- same one, page for page, so this builds each index every way and compares
-- the relation files byte for byte.
--
-- The tables are UNLOGGED because a logged build's pages carry the LSN of the
-- WAL record that logged them; an unlogged one's are written as they are.
\set VERBOSITY terse
SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS pg_lion;
CREATE EXTENSION IF NOT EXISTS pageinspect;

SELECT setseed(0.31);

CREATE FUNCTION lion_build_digest(idx regclass) RETURNS text
	LANGUAGE sql AS $$ SELECT md5(pg_read_binary_file(pg_relation_filepath(idx))) $$;

-- REINDEX under the given maintenance_work_mem, and digest the result
CREATE FUNCTION lion_build_as(idx regclass, mem text)
	RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('maintenance_work_mem', mem, true);
	EXECUTE format('REINDEX INDEX %s', idx);
	RETURN lion_build_digest(idx);
END $$;

-- ---------------------------------------------------------------------
-- 1. Every kind of key: enum, bool, int, text, timestamptz, NULLs, arrays
--    with empty arrays and NULL elements, tsvectors with no lexemes.
-- ---------------------------------------------------------------------

CREATE TYPE lion_build_mood AS ENUM ('sad', 'ok', 'happy', 'ecstatic');
CREATE UNLOGGED TABLE lion_build (
	id int, mood lion_build_mood, flag bool, n int, t text, ts timestamptz,
	ia int[], ta text[], tv tsvector, u int8, pad text)
	WITH (fillfactor = 60);
INSERT INTO lion_build
SELECT g,
	   CASE WHEN g % 13 = 0 THEN NULL
			ELSE (ARRAY['sad','ok','happy','ecstatic']::lion_build_mood[])[1 + g % 4] END,
	   CASE WHEN g % 17 = 0 THEN NULL ELSE g % 3 = 0 END,
	   CASE WHEN g % 19 = 0 THEN NULL ELSE (g * 7919) % 500 END,
	   CASE WHEN g % 23 = 0 THEN NULL ELSE 'text ' || ((g * 48271) % 2000) END,
	   '2025-01-01'::timestamptz + ((g::int8 * 104729) % 5000) * interval '1 minute',
	   CASE WHEN g % 10 = 0 THEN NULL
			WHEN g % 10 = 1 THEN '{}'::int4[]
			WHEN g % 10 = 2 THEN ARRAY[NULL, g % 5]::int4[]
			ELSE ARRAY[g % 5, g % 50, g % 300, g % 5] END,
	   CASE WHEN g % 9 = 0 THEN NULL
			WHEN g % 9 = 1 THEN '{}'::text[]
			ELSE ARRAY['tag' || (g % 30), 'tag' || (g % 7), 'u' || g] END,
	   CASE WHEN g % 8 = 0 THEN NULL
			WHEN g % 8 = 1 THEN ''::tsvector
			ELSE to_tsvector('simple', 'word' || (g % 40) || ' other' || (g % 400)) END,
	   g::int8 * 1000003,
	   repeat('p', 20)
  FROM generate_series(1, 40000) g;

-- 2. HOT chains, made while the table has no index: a heap-only tuple is
--    reported under its chain's root offset, so the codes of one heap page
--    reach the build out of order.
UPDATE lion_build SET pad = pad || 'q' WHERE id % 5 = 0;
UPDATE lion_build SET pad = pad || 'r' WHERE id % 7 = 0;
UPDATE lion_build SET pad = pad || 's' WHERE id % 5 = 0;
SELECT count(*) > 1000 AS has_heap_only_tuples
  FROM generate_series(0, pg_relation_size('lion_build') /
					   current_setting('block_size')::int - 1) b,
	   heap_page_items(get_raw_page('lion_build', b::int))
 WHERE (t_infomask2 & 32768) <> 0;		-- HEAP_ONLY_TUPLE

CREATE INDEX lion_build_all ON lion_build
	USING lion (mood, flag, n, t, ts, ia, ta, tv, u);
CREATE INDEX lion_build_tv ON lion_build USING lion (tv);
CREATE INDEX lion_build_part ON lion_build USING lion (n, t) WHERE flag;
CREATE INDEX lion_build_expr ON lion_build USING lion ((n % 10), lower(t));

-- 3. An unordered class whose hash collides all the time, so that the
--    distinct keys of one hash meet in memory, in one run and across runs;
--    and one whose equality is coarser than the bytes, so that which
--    spelling an entry stores depends on which row came first.
CREATE FUNCTION lion_build_xid_hash(xid) RETURNS integer
	LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE
	AS $$ SELECT (($1::text)::int8 % 3)::int4 $$;
CREATE OPERATOR CLASS lion_build_xid_ops FOR TYPE xid USING lion AS
	OPERATOR 1 = (xid, xid),
	FUNCTION 1 lion_build_xid_hash(xid);
CREATE FUNCTION lion_build_lower_eq(text, text) RETURNS boolean
	LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE
	AS $$ SELECT lower($1) = lower($2) $$;
CREATE OPERATOR ==== (LEFTARG = text, RIGHTARG = text,
					  FUNCTION = lion_build_lower_eq, COMMUTATOR = ====);
CREATE FUNCTION lion_build_lower_hash(text) RETURNS integer
	LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE
	AS $$ SELECT hashtext(lower($1)) $$;
CREATE OPERATOR CLASS lion_build_lower_ops FOR TYPE text USING lion AS
	OPERATOR 1 ==== (text, text),
	FUNCTION 1 lion_build_lower_hash(text);

CREATE UNLOGGED TABLE lion_build_coll (x xid, s text, pad text)
	WITH (fillfactor = 60);
INSERT INTO lion_build_coll
SELECT CASE WHEN g % 29 = 0 THEN NULL ELSE ((g * 7) % 12 + 1)::text::xid END,
	   CASE (g * 31) % 4 WHEN 0 THEN 'Key' WHEN 1 THEN 'kEY' WHEN 2 THEN 'KEY'
			ELSE 'key' END || (g % 50),
	   'p'
  FROM generate_series(1, 30000) g;
-- ... and HOT chains again, so that the first row of a spelling moves
UPDATE lion_build_coll SET pad = pad || 'q' WHERE x::text::int % 5 = 0;
CREATE INDEX lion_build_coll_x ON lion_build_coll
	USING lion (x lion_build_xid_ops, s lion_build_lower_ops);
CREATE INDEX lion_build_coll_s ON lion_build_coll USING lion (s lion_build_lower_ops);

-- ---------------------------------------------------------------------
-- 4. The same index every way: in memory and spilled (64kB is the least
--    maintenance_work_mem there is).
-- ---------------------------------------------------------------------

CREATE TEMP TABLE lion_build_ways AS
SELECT idx::text AS idx,
	   lion_build_as(idx, '64MB') AS in_memory,
	   lion_build_as(idx, '64kB') AS spilled
  FROM unnest(ARRAY['lion_build_all', 'lion_build_tv', 'lion_build_part',
					'lion_build_expr', 'lion_build_coll_x',
					'lion_build_coll_s']::regclass[]) idx;
SELECT idx, spilled = in_memory AS spilled_same
  FROM lion_build_ways ORDER BY idx;

-- REINDEX CONCURRENTLY builds from an MVCC snapshot, into a new index that
-- then takes the old one's name
RESET maintenance_work_mem;
REINDEX INDEX CONCURRENTLY lion_build_all;
SELECT lion_build_digest('lion_build_all') = in_memory AS concurrently_same
  FROM lion_build_ways WHERE idx = 'lion_build_all';
SET maintenance_work_mem = '64kB';
REINDEX INDEX CONCURRENTLY lion_build_all;
SELECT lion_build_digest('lion_build_all') = in_memory AS concurrently_spilled_same
  FROM lion_build_ways WHERE idx = 'lion_build_all';
RESET maintenance_work_mem;

-- Which way each build went.
SET client_min_messages = debug1;
REINDEX INDEX lion_build_tv;
SET maintenance_work_mem = '64kB';
REINDEX INDEX lion_build_tv;
SET client_min_messages = warning;
RESET maintenance_work_mem;

-- ---------------------------------------------------------------------
-- 5. And it is the right index: verify() finds every heap row under every
--    key, and each column holds exactly as many keys, TIDs, NULL rows and
--    key-less rows as the heap says - so nothing is missing and nothing is
--    extra.  A posting set holds a TID once, so the totals settle it.
-- ---------------------------------------------------------------------

SELECT idx, lion_index_verify(idx::regclass, true) AS verified
  FROM lion_build_ways ORDER BY idx;

-- keys_sql gives each row's distinct non-NULL keys as text[], NULL for NULL
CREATE FUNCTION lion_build_shape(keys_sql text, OUT keys int8, OUT pairs int8,
								 OUT nulls int8, OUT empties int8)
	LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format($q$
		WITH r AS (SELECT (%s) AS a FROM lion_build)
		SELECT (SELECT count(DISTINCT e) FROM r, unnest(a) e),
			   (SELECT coalesce(sum(cardinality(a)), 0) FROM r),
			   (SELECT count(*) FROM r WHERE a IS NULL),
			   (SELECT count(*) FROM r WHERE a = '{}')$q$, keys_sql)
		INTO keys, pairs, nulls, empties;
END $$;

SELECT s.attno, s.entries, s.ntids, s.null_tids, s.empty_tids,
	   s.entries = h.keys + (h.nulls > 0)::int + (h.empties > 0)::int
	   AND s.ntids = h.pairs + h.nulls + h.empties
	   AND s.null_tids = h.nulls AND s.empty_tids = h.empties AS exact
  FROM lion_index_stats('lion_build_all') s
  JOIN (VALUES (1, 'CASE WHEN mood IS NULL THEN NULL ELSE ARRAY[mood::text] END'),
			   (2, 'CASE WHEN flag IS NULL THEN NULL ELSE ARRAY[flag::text] END'),
			   (3, 'CASE WHEN n IS NULL THEN NULL ELSE ARRAY[n::text] END'),
			   (4, 'CASE WHEN t IS NULL THEN NULL ELSE ARRAY[t] END'),
			   (5, 'CASE WHEN ts IS NULL THEN NULL ELSE ARRAY[ts::text] END'),
			   (6, 'CASE WHEN ia IS NULL THEN NULL ELSE ARRAY(SELECT DISTINCT e::text FROM unnest(ia) e WHERE e IS NOT NULL) END'),
			   (7, 'CASE WHEN ta IS NULL THEN NULL ELSE ARRAY(SELECT DISTINCT e FROM unnest(ta) e WHERE e IS NOT NULL) END'),
			   (8, 'CASE WHEN tv IS NULL THEN NULL ELSE tsvector_to_array(tv) END'),
			   (9, 'CASE WHEN u IS NULL THEN NULL ELSE ARRAY[u::text] END')) c(attno, keys_sql)
	ON c.attno = s.attno,
	   lion_build_shape(c.keys_sql) h
 ORDER BY s.attno;

-- one entry per spelling class, which counts every spelling
SELECT lion_index_count_any('lion_build_coll_s', ARRAY['KEY7', 'key7']::text[]) =
	   (SELECT count(*) FROM lion_build_coll WHERE lower(s) = 'key7') AS class_count;
SELECT entries FROM lion_index_stats('lion_build_coll_s');

DROP TABLE lion_build, lion_build_coll;
DROP TYPE lion_build_mood;
DROP OPERATOR CLASS lion_build_xid_ops USING lion;
DROP OPERATOR CLASS lion_build_lower_ops USING lion;
DROP OPERATOR ==== (text, text);
DROP FUNCTION lion_build_xid_hash(xid), lion_build_lower_eq(text, text),
	lion_build_lower_hash(text), lion_build_shape(text),
	lion_build_digest(regclass), lion_build_as(regclass, text);
