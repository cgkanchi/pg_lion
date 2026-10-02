-- The decision matrix's synthetic tables (DESIGN.md §39, "The matrix"):
-- §31's shapes at :rows rows (2,000,000 by default).  Run by matrix.py
-- --setup, or by hand:
--
--   psql -X -v rows=2000000 -v dirty=0 -f bench/calib/setup.sql
--
-- `dirty` is the percentage of calib_s's rows updated after the VACUUM, so
-- that the counts' heap recheck has pages that are not all-visible.
\set ON_ERROR_STOP on
\if :{?rows}
\else
\set rows 2000000
\endif
\if :{?dirty}
\else
\set dirty 0
\endif
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
DROP TABLE IF EXISTS calib_s, calib_f, calib_d, calib_dk;
RESET client_min_messages;
SET maintenance_work_mem = '512MB';

-- §31's `s`: ints of 2, 20, 200, 20,000 and 1M values at random; 200 values
-- stored in heap order; a skewed one (90% one value); one NULL on 10% of
-- rows; k a random int, under a btree with id, for ORDER BY ... LIMIT
CREATE TABLE calib_s (
	id		int		NOT NULL,
	k		int		NOT NULL,
	c2		int		NOT NULL,
	c20		int		NOT NULL,
	c200	int		NOT NULL,
	c20k	int		NOT NULL,
	c1m		int		NOT NULL,
	cl200	int		NOT NULL,
	skew	int		NOT NULL,
	n10		int,
	pad		text	NOT NULL
) WITH (autovacuum_enabled = off);
INSERT INTO calib_s
SELECT i,
	   (hashint8extended(i::bigint, 10) & 2147483647)::int,
	   ((hashint8extended(i::bigint, 1) & 9223372036854775807) % 2)::int,
	   ((hashint8extended(i::bigint, 2) & 9223372036854775807) % 20)::int,
	   ((hashint8extended(i::bigint, 3) & 9223372036854775807) % 200)::int,
	   ((hashint8extended(i::bigint, 4) & 9223372036854775807) % 20000)::int,
	   ((hashint8extended(i::bigint, 5) & 9223372036854775807) % 1000000)::int,
	   ((i - 1)::bigint * 200 / :rows)::int,
	   CASE WHEN (hashint8extended(i::bigint, 6) & 9223372036854775807) % 10 < 9
			THEN 0
			ELSE 1 + ((hashint8extended(i::bigint, 7) & 9223372036854775807) % 99)::int END,
	   CASE WHEN (hashint8extended(i::bigint, 8) & 9223372036854775807) % 10 = 0
			THEN NULL
			ELSE ((hashint8extended(i::bigint, 9) & 9223372036854775807) % 50)::int END,
	   repeat('x', 40)
  FROM generate_series(1, :rows) i;
CREATE INDEX calib_s_c2 ON calib_s USING lion (c2);
CREATE INDEX calib_s_c20 ON calib_s USING lion (c20);
CREATE INDEX calib_s_c200 ON calib_s USING lion (c200);
CREATE INDEX calib_s_c20k ON calib_s USING lion (c20k);
CREATE INDEX calib_s_c1m ON calib_s USING lion (c1m);
CREATE INDEX calib_s_cl200 ON calib_s USING lion (cl200);
CREATE INDEX calib_s_skew ON calib_s USING lion (skew);
CREATE INDEX calib_s_n10 ON calib_s USING lion (n10);
-- btrees beside three of them, and the order
CREATE INDEX calib_s_c20k_bt ON calib_s (c20k);
CREATE INDEX calib_s_c1m_bt ON calib_s (c1m);
CREATE INDEX calib_s_cl200_bt ON calib_s (cl200);
CREATE INDEX calib_s_k_bt ON calib_s (k, id);

-- §27's shape: a fact of :rows rows over a dimension of :rows / 20 keys (20
-- rows a key, at random) and a small one of 1,000; kind of 20 values, x of
-- 10.  The dimension: a primary key, a status of 5 values and a country of
-- 50 under one lion index.
CREATE TABLE calib_d (
	pk		int8	PRIMARY KEY,
	status	int		NOT NULL,
	country	int		NOT NULL,
	attr	int		NOT NULL
);
INSERT INTO calib_d
SELECT i,
	   ((hashint8extended(i, 21) & 9223372036854775807) % 5)::int,
	   ((hashint8extended(i, 22) & 9223372036854775807) % 50)::int,
	   (i % 7)::int
  FROM generate_series(1, (:rows / 20)::bigint) i;
CREATE INDEX calib_d_sc ON calib_d USING lion (status, country);
CREATE TABLE calib_dk (pk int8 PRIMARY KEY, grp int NOT NULL);
INSERT INTO calib_dk SELECT i, (i % 20)::int FROM generate_series(1, 1000) i;
CREATE TABLE calib_f (
	id		int		NOT NULL,
	fk		int8,
	fk2		int8	NOT NULL,
	kind	int		NOT NULL,
	x		int		NOT NULL
) WITH (autovacuum_enabled = off);
INSERT INTO calib_f
SELECT i,
	   CASE WHEN i % 100 = 0 THEN NULL
			ELSE 1 + (hashint8extended(i::bigint, 31) & 9223372036854775807) % (:rows / 20)::bigint END,
	   1 + (hashint8extended(i::bigint, 32) & 9223372036854775807) % 1000,
	   ((hashint8extended(i::bigint, 33) & 9223372036854775807) % 20)::int,
	   i % 10
  FROM generate_series(1, :rows) i;
CREATE INDEX calib_f_fk ON calib_f USING lion (fk);
CREATE INDEX calib_f_fk2 ON calib_f USING lion (fk2);
CREATE INDEX calib_f_kind ON calib_f USING lion (kind);
CREATE INDEX calib_f_x ON calib_f USING lion (x);

VACUUM (FREEZE, ANALYZE) calib_s;
VACUUM (FREEZE, ANALYZE) calib_d;
VACUUM (FREEZE, ANALYZE) calib_dk;
VACUUM (FREEZE, ANALYZE) calib_f;

-- dirty a share of calib_s's pages: an update clears the all-visible bit of
-- the page it leaves and of the page it writes
UPDATE calib_s SET pad = repeat('y', 40) WHERE id % 100 < :dirty;
ANALYZE calib_s;
