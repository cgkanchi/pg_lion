-- bench/slices.sql - the measurements of DESIGN.md §51, "Measured": what bit
-- slices cost (index size, build, insert) and what lion_index_slice_agg()
-- answers in, against the ordinary plan of the query it stands for.
--
--   psql -X -v rows=5000000 -f bench/slices.sql   (into a scratch database)
--
-- Run against a release build, on a cluster with nothing else on it; every
-- timing is the best of :reps runs of a statement, clock_timestamp() around
-- it in a DO block, warm.  Results go to slc_bench and are printed at the end.
\set ON_ERROR_STOP on
\if :{?rows}
\else
\set rows 5000000
\endif
\if :{?reps}
\else
\set reps 3
\endif
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET maintenance_work_mem = '1GB';
SET work_mem = '64MB';

DROP TABLE IF EXISTS slc_b, slc_bench, slc_bi_none, slc_bi_off, slc_bi_on;
CREATE TABLE slc_bench (what text, col text, slices bool, ms numeric, bytes int8,
						note text);

-- Uniform (u: every value of the type equally likely, an int8's two halves
-- drawn apart since random() has 52 bits), sequential (s: the row number, so
-- in heap order) and skewed (k: random()^4 of a range, most rows near 0)
-- columns of int4 and int8, and filters of 1%, 10% and 50%.
SELECT setseed(0.42);
CREATE TABLE slc_b (id int, g100 int, g10 int, g2 int,
					u4 int4, s4 int4, k4 int4, u8 int8, s8 int8, k8 int8)
	WITH (autovacuum_enabled = off);
INSERT INTO slc_b
SELECT i, floor(random() * 100), floor(random() * 10), floor(random() * 2),
	   (floor(random() * 4294967296) - 2147483648)::int8::int4,
	   i,
	   floor(power(random(), 4) * 1000000)::int4,
	   (((floor(random() * 4294967296) - 2147483648)::int8 << 32) | floor(random() * 4294967296)::int8),
	   i::int8 * 1000003,
	   floor(power(random(), 4) * 1e15)::int8
  FROM generate_series(1, :rows) i;
CREATE INDEX slc_b_g ON slc_b USING lion (g100, g10, g2);
VACUUM (FREEZE, ANALYZE) slc_b;

-- Build time and size of a one-column index, slices off and on.
CREATE PROCEDURE slc_build(col text, reps int) LANGUAGE plpgsql AS $$
DECLARE
	s bool;
	t0 timestamptz;
	best numeric;
	r int;
BEGIN
	FOREACH s IN ARRAY ARRAY[false, true]
	LOOP
		best := NULL;
		FOR r IN 1 .. reps LOOP
			EXECUTE 'DROP INDEX IF EXISTS slc_b_x';
			t0 := clock_timestamp();
			EXECUTE format('CREATE INDEX slc_b_x ON slc_b USING lion (%I) WITH (slices = %s)',
						   col, s);
			best := least(best, extract(epoch FROM clock_timestamp() - t0) * 1000);
		END LOOP;
		INSERT INTO slc_bench VALUES ('build', col, s, round(best),
			pg_relation_size('slc_b_x'), NULL);
		EXECUTE 'DROP INDEX slc_b_x';
	END LOOP;
END $$;
CALL slc_build('u4', :reps);
CALL slc_build('s4', :reps);
CALL slc_build('k4', :reps);
CALL slc_build('u8', :reps);
CALL slc_build('s8', :reps);
CALL slc_build('k8', :reps);

-- sum, min and max under a filter: the function against the query's plan.
CREATE INDEX slc_b_u4 ON slc_b USING lion (u4) WITH (slices = on);
CREATE INDEX slc_b_u8 ON slc_b USING lion (u8) WITH (slices = on);
CREATE INDEX slc_b_k4 ON slc_b USING lion (k4) WITH (slices = on);
VACUUM (FREEZE, ANALYZE) slc_b;
CREATE PROCEDURE slc_agg(col text, filter text, reps int) LANGUAGE plpgsql AS $$
DECLARE
	t0 timestamptz;
	best1 numeric;
	best2 numeric;
	r int;
	a text;
	b text;
BEGIN
	FOR r IN 1 .. reps LOOP
		t0 := clock_timestamp();
		EXECUTE format('SELECT format(''%%s %%s %%s %%s'', count, sum, min, max) FROM lion_index_slice_agg(%L, %L)',
					   'slc_b_' || col, filter) INTO a;
		best1 := least(best1, extract(epoch FROM clock_timestamp() - t0) * 1000);
		t0 := clock_timestamp();
		EXECUTE format('SELECT format(''%%s %%s %%s %%s'', count(%1$I), sum(%1$I), min(%1$I), max(%1$I)) FROM slc_b WHERE %2$s',
					   col, filter) INTO b;
		best2 := least(best2, extract(epoch FROM clock_timestamp() - t0) * 1000);
	END LOOP;
	IF a IS DISTINCT FROM b THEN
		RAISE EXCEPTION 'different: % / %', a, b;
	END IF;
	INSERT INTO slc_bench VALUES ('agg', col, true, round(best1, 1), NULL, filter),
		('agg', col, false, round(best2, 1), NULL, filter);
END $$;
CALL slc_agg('u4', 'g100 = 7', :reps);
CALL slc_agg('u4', 'g10 = 3', :reps);
CALL slc_agg('u4', 'g2 = 1', :reps);
CALL slc_agg('u8', 'g100 = 7', :reps);
CALL slc_agg('u8', 'g10 = 3', :reps);
CALL slc_agg('u8', 'g2 = 1', :reps);
CALL slc_agg('k4', 'g100 = 7', :reps);
CALL slc_agg('k4', 'g10 = 3', :reps);
CALL slc_agg('k4', 'g2 = 1', :reps);

-- Insert cost: 100,000 rows of random values into a table of 1,000,000, by
-- one INSERT ... SELECT and by single-row INSERTs, with no index, a lion
-- index without slices and one with; and the WAL the INSERT ... SELECT wrote
-- (in bytes), which depends on the cluster's WAL mode (DESIGN.md §25).
CREATE PROCEDURE slc_ins(reps int) LANGUAGE plpgsql AS $$
DECLARE
	t text;
	t0 timestamptz;
	best numeric;
	r int;
	i int;
	lsn pg_lsn;
	wal int8;
BEGIN
	FOREACH t IN ARRAY ARRAY['none', 'off', 'on']
	LOOP
		FOR r IN 1 .. reps LOOP
			EXECUTE format('DROP TABLE IF EXISTS slc_bi_%s', t);
			EXECUTE format('CREATE TABLE slc_bi_%s (v4 int4, v8 int8) WITH (autovacuum_enabled = off)', t);
			EXECUTE format('INSERT INTO slc_bi_%s SELECT (floor(random() * 4294967296) - 2147483648)::int8::int4, (((floor(random() * 4294967296) - 2147483648)::int8 << 32) | floor(random() * 4294967296)::int8) FROM generate_series(1, 1000000)', t);
			IF t <> 'none' THEN
				EXECUTE format('CREATE INDEX ON slc_bi_%s USING lion (v4) WITH (slices = %s)', t, t);
				EXECUTE format('CREATE INDEX ON slc_bi_%s USING lion (v8) WITH (slices = %s)', t, t);
			END IF;
			lsn := pg_current_wal_insert_lsn();
			t0 := clock_timestamp();
			EXECUTE format('INSERT INTO slc_bi_%s SELECT (floor(random() * 4294967296) - 2147483648)::int8::int4, (((floor(random() * 4294967296) - 2147483648)::int8 << 32) | floor(random() * 4294967296)::int8) FROM generate_series(1, 100000)', t);
			best := least(best, extract(epoch FROM clock_timestamp() - t0) * 1000);
			wal := pg_wal_lsn_diff(pg_current_wal_insert_lsn(), lsn);
		END LOOP;
		INSERT INTO slc_bench VALUES ('insert select 100k', 'v4+v8', t = 'on', round(best), wal, t);
		best := NULL;
		FOR r IN 1 .. reps LOOP
			t0 := clock_timestamp();
			FOR i IN 1 .. 10000 LOOP
				EXECUTE format('INSERT INTO slc_bi_%s VALUES ($1, $2)', t)
					USING (floor(random() * 4294967296) - 2147483648)::int8::int4,
						  (((floor(random() * 4294967296) - 2147483648)::int8 << 32) | floor(random() * 4294967296)::int8);
			END LOOP;
			best := least(best, extract(epoch FROM clock_timestamp() - t0) * 1000);
		END LOOP;
		INSERT INTO slc_bench VALUES ('insert 10k single rows', 'v4+v8', t = 'on', round(best), NULL, t);
		EXECUTE format('DROP TABLE slc_bi_%s', t);
		best := NULL;
	END LOOP;
END $$;
CALL slc_ins(:reps);

SELECT * FROM slc_bench ORDER BY what, col, note, slices;
DROP PROCEDURE slc_build(text, int);
DROP PROCEDURE slc_agg(text, text, int);
DROP PROCEDURE slc_ins(int);
