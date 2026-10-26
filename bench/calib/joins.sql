-- Tables for the joins of DESIGN.md §40.10, §46 to §49 at a scale where the
-- dimension's heap is larger than shared_buffers, for matrix.py
-- --setup-file bench/calib/joins.sql --queries bench/calib/joins.py.  :rows
-- rows in the dimension (4,000,000 by default: a heap of about 1.6 GB),
-- twice that in the fact and in competitor_cold.sql's dimension.
--
--   jn_dim     the dimension: eight lion indexes (probejoin.sql's,
--              ordjoin.sql's and heapprobe.sql's columns), a primary key that
--              covers d.pk alone, `name` and a wide `pad` in no index;
--              vacuumed, all-visible
--   jn_fact    the fact: 2 rows a key, no lion index on the key; B-trees on
--              `g` (2,000 groups spread over the heap: a nested loop's outer
--              rows), `(score DESC) INCLUDE (fk, id)` (§47.9's walked B-tree,
--              which holds the key) and `(rank DESC)` (one that does not);
--              lion indexes on `chan` and `tags`
--   jn_watch   key sets: product 7 :rows / 4,000 keys, product 3 :rows / 50
--   jn_blocked the NOT EXISTS's keys of §46, :rows / 1,000
--   jn_ccd     competitor_cold.sql's dimension, 2 * :rows rows, NOT vacuumed:
--              no page all-visible, as on a table under writes
--   jn_cc*     competitor_cold.sql's facts and anti join tables, ten times
\set ON_ERROR_STOP on
\if :{?rows}
\else
\set rows 4000000
\endif
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
DROP TABLE IF EXISTS jn_dim, jn_fact, jn_watch, jn_blocked,
	jn_ccd, jn_ccf, jn_ccm, jn_ccg, jn_ccs, jn_cct;
DROP TYPE IF EXISTS jn_grade;
RESET client_min_messages;
SET maintenance_work_mem = '1GB';
SET synchronous_commit = on;
SET default_statistics_target = 1000;

CREATE TYPE jn_grade AS ENUM ('delta', 'gamma', 'alpha', 'beta');
CREATE TABLE jn_dim (
	pk		int			NOT NULL,
	region	text		NOT NULL,	-- lion: five values
	tier	int			NOT NULL,	-- lion: 1 to 5
	active	bool		NOT NULL,	-- lion: two thirds true
	grade	jn_grade	NOT NULL,	-- lion: alpha 10%, beta 10%, gamma 70%, delta 10%
	tags	text[]		NOT NULL,	-- lion: one of ten t's
	kind	int			NOT NULL,	-- lion: ten values
	lvl		int			NOT NULL,	-- lion: a hundred values
	flag	bool		NOT NULL,	-- lion: two thirds true
	name	text		NOT NULL,	-- read by the heap probe's queries, in no index
	pad		text		NOT NULL	-- wide: the heap is many times the primary key
) WITH (autovacuum_enabled = off);
INSERT INTO jn_dim
SELECT i,
	   (ARRAY['north', 'east', 'south', 'west', 'central'])[1 + h0 % 5],
	   1 + h1 % 5,
	   h2 % 3 <> 0,
	   CASE WHEN h3 < 10 THEN 'alpha' WHEN h3 < 20 THEN 'beta'
			WHEN h3 < 90 THEN 'gamma' ELSE 'delta' END::jn_grade,
	   ARRAY['t' || (h4 % 10)],
	   h5 % 10,
	   h6 % 100,
	   h7 % 3 <> 0,
	   'd' || i,
	   repeat(md5(i::text), 9)
  FROM (SELECT i,
			   ((hashint8extended(i::bigint, 10) & 9223372036854775807) % 1000003)::int AS h0,
			   ((hashint8extended(i::bigint, 11) & 9223372036854775807) % 1000003)::int AS h1,
			   ((hashint8extended(i::bigint, 12) & 9223372036854775807) % 1000003)::int AS h2,
			   ((hashint8extended(i::bigint, 13) & 9223372036854775807) % 100)::int AS h3,
			   ((hashint8extended(i::bigint, 14) & 9223372036854775807) % 1000003)::int AS h4,
			   ((hashint8extended(i::bigint, 15) & 9223372036854775807) % 1000003)::int AS h5,
			   ((hashint8extended(i::bigint, 16) & 9223372036854775807) % 1000003)::int AS h6,
			   ((hashint8extended(i::bigint, 17) & 9223372036854775807) % 1000003)::int AS h7
		  FROM generate_series(1, :rows) i) s;
ALTER TABLE jn_dim ADD PRIMARY KEY (pk);
CREATE INDEX jn_dim_region ON jn_dim USING lion (region);
CREATE INDEX jn_dim_tier ON jn_dim USING lion (tier);
CREATE INDEX jn_dim_active ON jn_dim USING lion (active);
CREATE INDEX jn_dim_grade ON jn_dim USING lion (grade);
CREATE INDEX jn_dim_tags ON jn_dim USING lion (tags);
CREATE INDEX jn_dim_kind ON jn_dim USING lion (kind);
CREATE INDEX jn_dim_lvl ON jn_dim USING lion (lvl);
CREATE INDEX jn_dim_flag ON jn_dim USING lion (flag);

CREATE TABLE jn_fact (
	id		int		NOT NULL,
	fk		int,				-- a jn_dim key, uniform; NULL in every 101st row
	g		int		NOT NULL,	-- 2,000 groups, each spread over the heap
	chan	int		NOT NULL,	-- lion: eight values
	tags	text[]	NOT NULL,	-- lion: one of four a's and one of nine b's
	channel	int		NOT NULL,	-- five values, in no index: §46's group key
	score	float8	NOT NULL,	-- jn_fact_score's order
	rank	int		NOT NULL,	-- jn_fact_rank's order
	amount	int		NOT NULL	-- in no index: reading it puts a walk in heap mode
) WITH (autovacuum_enabled = off);
INSERT INTO jn_fact
SELECT i,
	   CASE WHEN i % 101 = 0 THEN NULL
			ELSE 1 + ((hashint8extended(i::bigint, 20) & 9223372036854775807) % :rows)::int END,
	   i % 2000,
	   ((hashint8extended(i::bigint, 21) & 9223372036854775807) % 8)::int,
	   ARRAY['a' || ((hashint8extended(i::bigint, 22) & 9223372036854775807) % 4),
			 'b' || ((hashint8extended(i::bigint, 23) & 9223372036854775807) % 9)],
	   ((hashint8extended(i::bigint, 24) & 9223372036854775807) % 5)::int,
	   ((hashint8extended(i::bigint, 25) & 9223372036854775807) % 100000000)::float8 / 100,
	   ((hashint8extended(i::bigint, 26) & 9223372036854775807) % 1000000000)::int,
	   i % 1000
  FROM generate_series(1, 2 * :rows) i;
ALTER TABLE jn_fact ADD PRIMARY KEY (id);
CREATE INDEX jn_fact_g ON jn_fact (g);
CREATE INDEX jn_fact_score ON jn_fact (score DESC) INCLUDE (fk, id);
CREATE INDEX jn_fact_rank ON jn_fact (rank DESC);
CREATE INDEX jn_fact_chan ON jn_fact USING lion (chan);
CREATE INDEX jn_fact_tags ON jn_fact USING lion (tags);

-- key sets: product 7 is 1,000 keys (0.025% of the dimension), product 3
-- 80,000 (2%); the multipliers are prime to :rows, so the keys are distinct
CREATE TABLE jn_watch (k int NOT NULL, prod int NOT NULL, PRIMARY KEY (k, prod))
	WITH (autovacuum_enabled = off);
INSERT INTO jn_watch SELECT 1 + (k::bigint * 5003 % :rows)::int, 7
  FROM generate_series(1, :rows / 4000) k;
INSERT INTO jn_watch SELECT 1 + (k::bigint * 7919 % :rows)::int, 3
  FROM generate_series(1, :rows / 50) k;
-- §46's NOT EXISTS: 4,000 keys of the dimension
CREATE TABLE jn_blocked (dim_id int NOT NULL) WITH (autovacuum_enabled = off);
INSERT INTO jn_blocked SELECT 1 + (k::bigint * 104729 % :rows)::int
  FROM generate_series(1, :rows / 1000) k;
CREATE INDEX jn_blocked_dim_id ON jn_blocked (dim_id);

VACUUM (FREEZE, ANALYZE) jn_dim;
VACUUM (FREEZE, ANALYZE) jn_fact;
VACUUM (FREEZE, ANALYZE) jn_watch;
VACUUM (FREEZE, ANALYZE) jn_blocked;

-- competitor_cold.sql's tables, ten times and more: a dimension of 2 * :rows
-- rows (about 1.2 GB), analyzed but not vacuumed - no page all-visible, so
-- LionProbeJoin reads the heap of every member it finds; facts of 30,000
-- scattered keys, 45,000 rows over 30,000 keys (core's Memoize), and
-- 2,000,000 rows whose scan is most of a plan; anti join tables of 3,000 and
-- 300,000 rows with a B-tree on the key.
CREATE TABLE jn_ccd (pk int8 NOT NULL, seg int NOT NULL, flag bool NOT NULL,
					 pad text NOT NULL)
	WITH (autovacuum_enabled = off);
INSERT INTO jn_ccd
SELECT i, i % 10, i % 4 = 0, repeat('p', 100) FROM generate_series(1, 2 * :rows) i;
ALTER TABLE jn_ccd ADD PRIMARY KEY (pk);
CREATE INDEX jn_ccd_seg ON jn_ccd USING lion (seg);
CREATE INDEX jn_ccd_flag ON jn_ccd USING lion (flag);
ANALYZE jn_ccd;
-- the hint bits, so that no timed run sets them
SELECT count(*) FROM jn_ccd WHERE pad IS NULL;

CREATE TABLE jn_ccf (id int NOT NULL, fk int8 NOT NULL, kind int NOT NULL)
	WITH (autovacuum_enabled = off);
INSERT INTO jn_ccf
SELECT i, 1 + abs(hashint4(i)) % (2 * :rows), i % 3 FROM generate_series(1, 30000) i;
CREATE TABLE jn_ccm (id int NOT NULL, fk int8 NOT NULL, kind int NOT NULL)
	WITH (autovacuum_enabled = off);
INSERT INTO jn_ccm
SELECT i, 1 + abs(hashint4(i % 30000)) % (2 * :rows), i % 3
  FROM generate_series(1, 45000) i;
CREATE TABLE jn_ccg (id int NOT NULL, fk int8 NOT NULL, kind int NOT NULL)
	WITH (autovacuum_enabled = off);
INSERT INTO jn_ccg
SELECT i, 1 + abs(hashint4(i)) % (2 * :rows), i % 3 FROM generate_series(1, 2000000) i;
CREATE TABLE jn_ccs (fk int8 NOT NULL, why int NOT NULL)
	WITH (autovacuum_enabled = off);
INSERT INTO jn_ccs
SELECT 1 + abs(hashint4(i + 7)) % (2 * :rows), i % 2 FROM generate_series(1, 3000) i;
CREATE INDEX jn_ccs_fk ON jn_ccs (fk);
CREATE TABLE jn_cct (fk int8 NOT NULL, why int NOT NULL)
	WITH (autovacuum_enabled = off);
INSERT INTO jn_cct
SELECT 1 + abs(hashint4(i + 11)) % (2 * :rows), i % 2 FROM generate_series(1, 300000) i;
CREATE INDEX jn_cct_fk ON jn_cct (fk);
VACUUM (FREEZE, ANALYZE) jn_ccf, jn_ccm, jn_ccg, jn_ccs, jn_cct;
CHECKPOINT;
