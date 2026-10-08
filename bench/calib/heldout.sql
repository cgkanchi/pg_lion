-- Tables the cost model's rates were NOT fitted on (DESIGN.md §39), for
-- matrix.py --setup-file bench/calib/heldout.sql --queries
-- bench/calib/heldout.py: other cardinalities, a text key, geometric skew,
-- a correlated column in runs of uneven length, 30% NULLs, and FK joins of
-- other sizes (10 rows a key, 2 rows a key, 64 keys).  :rows rows in the
-- scalar table and in the fact.
\set ON_ERROR_STOP on
\if :{?rows}
\else
\set rows 3000000
\endif
\if :{?dirty}
\else
\set dirty 10
\endif
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
DROP TABLE IF EXISTS ho_s, ho_f, ho_d10, ho_d2, ho_d64;
RESET client_min_messages;
SET maintenance_work_mem = '512MB';

CREATE TABLE ho_s (
	id		int		NOT NULL,
	k		int		NOT NULL,
	c5		int		NOT NULL,
	c50		int		NOT NULL,
	c1k		int		NOT NULL,
	c100k	int		NOT NULL,
	geo		int		NOT NULL,	-- value v with probability 2^-(v+1)
	run		int		NOT NULL,	-- 24 values in heap-order runs of uneven length
	tag		text	NOT NULL,	-- 300 strings
	n30		int,				-- NULL on 30% of rows, else 8 values
	pad		text	NOT NULL
) WITH (autovacuum_enabled = off);
INSERT INTO ho_s
SELECT i,
	   (hashint8extended(i::bigint, 110) & 2147483647)::int,
	   ((hashint8extended(i::bigint, 101) & 9223372036854775807) % 5)::int,
	   ((hashint8extended(i::bigint, 102) & 9223372036854775807) % 50)::int,
	   ((hashint8extended(i::bigint, 103) & 9223372036854775807) % 1000)::int,
	   ((hashint8extended(i::bigint, 104) & 9223372036854775807) % 100000)::int,
	   least(30, floor(-ln(((hashint8extended(i::bigint, 105) & 9223372036854775807) + 1)::float8
							 / 9223372036854775808.0) / ln(2.0::float8)))::int,
	   floor(24 * sqrt((i - 1)::float8 / :rows))::int,
	   'tag-' || ((hashint8extended(i::bigint, 106) & 9223372036854775807) % 300),
	   CASE WHEN (hashint8extended(i::bigint, 107) & 9223372036854775807) % 10 < 3
			THEN NULL
			ELSE ((hashint8extended(i::bigint, 108) & 9223372036854775807) % 8)::int END,
	   repeat('z', 60)
  FROM generate_series(1, :rows) i;
CREATE INDEX ho_s_c5 ON ho_s USING lion (c5);
CREATE INDEX ho_s_c50 ON ho_s USING lion (c50);
CREATE INDEX ho_s_c1k ON ho_s USING lion (c1k);
CREATE INDEX ho_s_c100k ON ho_s USING lion (c100k);
CREATE INDEX ho_s_geo ON ho_s USING lion (geo);
CREATE INDEX ho_s_run ON ho_s USING lion (run);
CREATE INDEX ho_s_tag ON ho_s USING lion (tag);
CREATE INDEX ho_s_n30 ON ho_s USING lion (n30);
CREATE INDEX ho_s_c100k_bt ON ho_s (c100k);
CREATE INDEX ho_s_run_bt ON ho_s (run);
CREATE INDEX ho_s_k_bt ON ho_s (k, id);

-- dimensions: :rows / 10 keys (10 fact rows a key), :rows / 2 keys (2 a
-- key, a hash join that spills) and 64 keys
CREATE TABLE ho_d10 (pk int8 PRIMARY KEY, region int NOT NULL, tier int NOT NULL);
INSERT INTO ho_d10
SELECT i, ((hashint8extended(i, 121) & 9223372036854775807) % 12)::int,
		  ((hashint8extended(i, 122) & 9223372036854775807) % 3)::int
  FROM generate_series(1, (:rows / 10)::bigint) i;
CREATE INDEX ho_d10_rt ON ho_d10 USING lion (region, tier);
CREATE TABLE ho_d2 (pk int8 PRIMARY KEY, flag int NOT NULL);
INSERT INTO ho_d2 SELECT i, ((hashint8extended(i, 123) & 9223372036854775807) % 40)::int
  FROM generate_series(1, (:rows / 2)::bigint) i;
CREATE INDEX ho_d2_flag ON ho_d2 USING lion (flag);
CREATE TABLE ho_d64 (pk int8 PRIMARY KEY, family int NOT NULL);
INSERT INTO ho_d64 SELECT i, (i % 8)::int FROM generate_series(1, 64) i;
CREATE TABLE ho_f (
	id		int		NOT NULL,
	f10		int8	NOT NULL,
	f2		int8	NOT NULL,
	f64		int8	NOT NULL,
	state	int		NOT NULL,
	y		int		NOT NULL
) WITH (autovacuum_enabled = off);
INSERT INTO ho_f
SELECT i,
	   1 + (hashint8extended(i::bigint, 131) & 9223372036854775807) % (:rows / 10)::bigint,
	   1 + (hashint8extended(i::bigint, 132) & 9223372036854775807) % (:rows / 2)::bigint,
	   1 + (hashint8extended(i::bigint, 133) & 9223372036854775807) % 64,
	   ((hashint8extended(i::bigint, 134) & 9223372036854775807) % 6)::int,
	   i % 25
  FROM generate_series(1, :rows) i;
CREATE INDEX ho_f_f10 ON ho_f USING lion (f10);
CREATE INDEX ho_f_f2 ON ho_f USING lion (f2);
CREATE INDEX ho_f_f64 ON ho_f USING lion (f64);
CREATE INDEX ho_f_state ON ho_f USING lion (state);
CREATE INDEX ho_f_y ON ho_f USING lion (y);

VACUUM (FREEZE, ANALYZE) ho_s;
VACUUM (FREEZE, ANALYZE) ho_d10;
VACUUM (FREEZE, ANALYZE) ho_d2;
VACUUM (FREEZE, ANALYZE) ho_d64;
VACUUM (FREEZE, ANALYZE) ho_f;

UPDATE ho_s SET pad = repeat('w', 60) WHERE id % 100 < :dirty;
ANALYZE ho_s;
