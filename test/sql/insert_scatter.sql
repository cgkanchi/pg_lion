-- Inserts that land at every place of a posting set (DESIGN.md section 13,
-- Insert), INLINE and in a posting tree.
--
-- Each row is updated once, in a scrambled order, and its new version stays
-- on its own heap page, so a key's posting set grows at its front, between
-- its items, inside them and at its end - not only at its end, where the
-- rows the heap appends land.  One row a page (fillfactor 10 and a 2400-byte
-- uncompressed pad) over 2560 pages is 20 container keys, and 64 keys of
-- about 40 rows each are mostly sparse segments, with a few container keys
-- promoted to containers.  The same rows go into an index that keeps them
-- INLINE and into one whose posting sets spill to posting trees at once
-- (inline_limit = 64).
\set VERBOSITY terse
SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS pg_lion;

CREATE TABLE lion_scatter (i int4, k int4, pad text)
	WITH (fillfactor = 10, autovacuum_enabled = off);
ALTER TABLE lion_scatter ALTER pad SET STORAGE PLAIN;
INSERT INTO lion_scatter
SELECT i, NULL, repeat('x', 2400) FROM generate_series(1, 2560) i;
CREATE INDEX lion_scatter_inl ON lion_scatter USING lion (k);
CREATE INDEX lion_scatter_chn ON lion_scatter USING lion ((k + 0))
	WITH (inline_limit = 64);

DO $$
DECLARE
	r record;
BEGIN
	FOR r IN SELECT i FROM lion_scatter ORDER BY hashint4(i) LOOP
		UPDATE lion_scatter SET k = 1 + (hashint4(r.i * 31) & 63)
		 WHERE i = r.i;
	END LOOP;
END $$;

-- every new version is on its old one's page, row i's page being i - 1
SELECT count(*) AS rows,
	   count(*) FILTER (WHERE (ctid::text::point)[0] <> i - 1) AS moved
  FROM lion_scatter;

SELECT entries, inline_entries, sparse_segments > containers AS mostly_sparse,
	   containers > 0 AS some_promoted, ntids
  FROM lion_index_stats('lion_scatter_inl');
SELECT entries, inline_entries, sparse_segments > 0 AS has_segments, ntids
  FROM lion_index_stats('lion_scatter_chn');

SELECT lion_index_verify('lion_scatter_inl', true);
SELECT lion_index_verify('lion_scatter_chn', true);

-- every key, through a bitmap scan of each index and through a sequential scan
CREATE FUNCTION lion_scatter_sum(pred text, scan text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	q text := 'SELECT count(*) AS c, coalesce(sum(i), 0) AS s FROM lion_scatter WHERE ' || pred;
	a record;
	ln text;
	used boolean := false;
BEGIN
	PERFORM set_config('enable_seqscan', CASE WHEN scan = 'seq' THEN 'on' ELSE 'off' END, true);
	PERFORM set_config('enable_bitmapscan', CASE WHEN scan = 'seq' THEN 'off' ELSE 'on' END, true);
	PERFORM set_config('enable_indexscan', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Bitmap Index Scan on ' || scan || '%' OR
		   (scan = 'seq' AND ln LIKE '%Seq Scan%') THEN
			used := true;
		END IF;
	END LOOP;
	IF NOT used THEN
		RETURN 'not scanned by ' || scan;
	END IF;
	EXECUTE q INTO a;
	RETURN a::text;
END $$;

SELECT count(*) AS keys,
	   count(*) FILTER (WHERE inl = seq) AS inline_agrees,
	   count(*) FILTER (WHERE chn = seq) AS tree_agrees
  FROM (SELECT lion_scatter_sum(format('k = %s', k), 'lion_scatter_inl') AS inl,
			   lion_scatter_sum(format('k + 0 = %s', k), 'lion_scatter_chn') AS chn,
			   lion_scatter_sum(format('k = %s', k), 'seq') AS seq
		  FROM generate_series(1, 64) k) s;

DROP TABLE lion_scatter;
DROP FUNCTION lion_scatter_sum(text, text);
