-- The soak's synthetic schema (test/soak/README.md).  psql variables:
--   rows    rows loaded into soak.t (plain table, indexes built after the load)
--   prows   rows loaded into soak.p (hash-partitioned, indexes built BEFORE
--           the load, so they are filled by the insert path)
-- Everything lives in schema soak, which is dropped and made again.
\set ON_ERROR_STOP 1
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
DROP SCHEMA IF EXISTS soak CASCADE;
CREATE SCHEMA soak;

-- One row's values.  Columns of 2, 20, 200, 20k and ~1M values, a nullable
-- one, a text[] multi-key one, and a pad that no index reads (its updates
-- are HOT where the page has room).  With corr, c20k follows the row number
-- (long runs of one value: run containers); without, it is random.
CREATE FUNCTION soak.gen(n int, corr bool DEFAULT false, base bigint DEFAULT 0)
RETURNS TABLE (c2 int, c20 int, c200 int2, c20k int8, c1m int, cn int,
			   tags text[], pad text)
LANGUAGE sql VOLATILE AS $$
	SELECT floor(random() * 2)::int,
		   -- skewed: value 0 holds about a fifth of the rows
		   CASE WHEN random() < 0.2 THEN 0 ELSE floor(random() * 20)::int END,
		   floor(random() * 200)::int2,
		   CASE WHEN corr THEN ((base + i) / 15) % 20000
				ELSE floor(random() * 20000)::int8 END,
		   floor(random() * 1000000)::int,
		   CASE WHEN random() < 0.2 THEN NULL ELSE floor(random() * 50)::int END,
		   CASE WHEN r < 0.02 THEN NULL
				WHEN r < 0.05 THEN '{}'::text[]
				ELSE ARRAY(SELECT DISTINCT 't' || CASE WHEN random() < 0.5
													   THEN floor(random() * 10)::int
													   ELSE floor(random() * 200)::int END
							 FROM generate_series(1, 1 + (i % 3)) WHERE r >= 0) END,
		   repeat('x', 10 + floor(random() * 50)::int)
	  FROM generate_series(1, n) i, LATERAL (SELECT random() AS r) rr
$$;

CREATE TABLE soak.t (
	id bigserial PRIMARY KEY,
	c2 int NOT NULL, c20 int NOT NULL, c200 int2 NOT NULL, c20k int8 NOT NULL,
	c1m int NOT NULL, cn int, tags text[], pad text
) WITH (fillfactor = 90, autovacuum_enabled = off);

CREATE TABLE soak.p (
	id bigserial,
	c2 int NOT NULL, c20 int NOT NULL, c200 int2 NOT NULL, c20k int8 NOT NULL,
	c1m int NOT NULL, cn int, tags text[], pad text,
	PRIMARY KEY (id)
) PARTITION BY HASH (id);
CREATE TABLE soak.p0 PARTITION OF soak.p FOR VALUES WITH (MODULUS 3, REMAINDER 0)
	WITH (fillfactor = 90, autovacuum_enabled = off);
CREATE TABLE soak.p1 PARTITION OF soak.p FOR VALUES WITH (MODULUS 3, REMAINDER 1)
	WITH (fillfactor = 90, autovacuum_enabled = off);
CREATE TABLE soak.p2 PARTITION OF soak.p FOR VALUES WITH (MODULUS 3, REMAINDER 2)
	WITH (fillfactor = 90, autovacuum_enabled = off);

-- A small dimension for the FK-side join and the semi/anti join (§27).
CREATE TABLE soak.d (k int2 PRIMARY KEY, g int NOT NULL);
INSERT INTO soak.d SELECT k, k % 7 FROM generate_series(0, 249) k;

-- p: the indexes first, so the load goes through the insert path.
CREATE INDEX p_l_c2 ON soak.p USING lion (c2);
CREATE INDEX p_l_c20 ON soak.p USING lion (c20);
CREATE INDEX p_l_c200 ON soak.p USING lion (c200);
CREATE INDEX p_l_c20k ON soak.p USING lion (c20k);
CREATE INDEX p_l_c1m ON soak.p USING lion (c1m);
CREATE INDEX p_l_cn ON soak.p USING lion (cn);
CREATE INDEX p_l_tags ON soak.p USING lion (tags);

INSERT INTO soak.t (c2, c20, c200, c20k, c1m, cn, tags, pad)
	SELECT * FROM soak.gen(:rows, true);
INSERT INTO soak.p (c2, c20, c200, c20k, c1m, cn, tags, pad)
	SELECT * FROM soak.gen(:prows, true);

-- t: the indexes after the load (the build path).
CREATE INDEX t_l_c2 ON soak.t USING lion (c2);
CREATE INDEX t_l_c20 ON soak.t USING lion (c20);
CREATE INDEX t_l_c200 ON soak.t USING lion (c200);
CREATE INDEX t_l_c20k ON soak.t USING lion (c20k);
CREATE INDEX t_l_c1m ON soak.t USING lion (c1m);
CREATE INDEX t_l_cn ON soak.t USING lion (cn);
CREATE INDEX t_l_tags ON soak.t USING lion (tags);
-- Two columns in one index: a second key column's entries and walks.
CREATE INDEX t_l_c20_c200 ON soak.t USING lion (c20, c200);

VACUUM (FREEZE, ANALYZE) soak.t, soak.p, soak.d;

-- The ids the writers draw from: the loaded ones and some headroom.
CREATE TABLE soak.meta AS
	SELECT (SELECT max(id) FROM soak.t) AS tmax, (SELECT max(id) FROM soak.p) AS pmax;

-- Every lion index of the soak, partitions' included, with its table and
-- whether it is scalar (one entry per row: ntids counts every TID).
CREATE VIEW soak.lion_indexes AS
	SELECT i.indexrelid::regclass AS idx, i.indrelid::regclass AS tbl,
		   NOT EXISTS (SELECT 1 FROM unnest(i.indkey) k
						 JOIN pg_attribute a ON a.attrelid = i.indrelid AND a.attnum = k
						WHERE a.atttypid = 'text[]'::regtype) AS scalar,
		   i.indnatts AS natts
	  FROM pg_index i
	  JOIN pg_class c ON c.oid = i.indexrelid
	  JOIN pg_am am ON am.oid = c.relam AND am.amname = 'lion'
	  JOIN pg_namespace n ON n.oid = c.relnamespace AND n.nspname = 'soak'
	 WHERE c.relkind = 'i';
