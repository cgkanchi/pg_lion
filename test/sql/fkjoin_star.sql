-- FK-side join pushdown, a second dimension that only filters (DESIGN.md
-- §27, "A second dimension that only filters").
--
-- A table inner-joined to the fact on a unique key of its own, nothing of it
-- wanted above the join, keeps the fact rows whose fk is the key of one of
-- its rows and repeats none: the node folds it into an IN list on the fk,
-- whose values it collects at the start of each run from a child of its own
-- (NULL keys left out), and joins the remaining dimension as ever.  Every
-- answer is checked against the same query with the pushdown off and
-- sequential scans only; lion_st() says whether the node answered it, and
-- how many tables it folded.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;
-- VACUUM can only set all-visible once the commit record is on disk
SET synchronous_commit = on;

/*
 * lion_st() runs a query through the pushdown with every join method
 * disabled, so that the node is exercised, and with the pushdown off and
 * sequential scans only, compares the two as multisets both ways round and
 * says whether the node answered the first - with how many tables folded,
 * its children past the first.
 */
CREATE FUNCTION lion_st(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	pushed boolean := false;
	children int := 0;
	depth int;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			pushed := true;
			depth := length(substring(ln from '^\s*'));
		ELSIF pushed AND ln ~ '^\s*->' AND
			  length(substring(ln from '^\s*')) = depth + 6 THEN
			children := children + 1;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lion_st_on AS SELECT s::text AS r FROM (%s) s', q);

	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_hashjoin', 'on', true);
	PERFORM set_config('enable_mergejoin', 'on', true);
	PERFORM set_config('enable_nestloop', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	EXECUTE format('CREATE TEMP TABLE lion_st_off AS SELECT s::text AS r FROM (%s) s', q);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM lion_st_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_st_on EXCEPT ALL SELECT * FROM lion_st_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_st_off EXCEPT ALL SELECT * FROM lion_st_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lion_st_on, lion_st_off';
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	IF NOT pushed THEN
		RETURN format('not pushed down, %s rows', nrows);
	END IF;
	RETURN format('pushed down, %s folded, %s rows', children - 1, nrows);
END $$;

/*
 * The plan, joins disabled, with each child's plan as one line: that plan is
 * whatever each major makes of a small scan.
 */
CREATE FUNCTION lion_st_explain(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	seen boolean := false;
BEGIN
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		CONTINUE WHEN ln ~ '^\s*Disabled: true$';
		IF seen AND ln ~ '^\s*->' THEN
			RETURN NEXT regexp_replace(ln, '->.*$', '->  (a child''s plan)');
			CONTINUE;
		END IF;
		CONTINUE WHEN seen AND ln !~ '^\s*(Lion Indexes|Fact Filters|Join)';
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			seen := true;
		END IF;
		RETURN NEXT ln;
	END LOOP;
END $$;

/* One counter of the node's EXPLAIN ANALYZE output, joins disabled. */
CREATE FUNCTION lion_st_counter(q text, counter text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	val bigint;
BEGIN
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	PERFORM set_config('enable_nestloop', 'off', true);
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF btrim(split_part(ln, ':', 1)) = counter THEN
			val := btrim(split_part(ln, ':', 2))::bigint;
		END IF;
	END LOOP;
	RETURN val;
END $$;

/*
 * The dimension: 400 rows over a unique, nullable key - 1..300, which the
 * fact's fk covers, 100 keys past it, NULL in every 31st row - with a region
 * and a name.
 */
CREATE TABLE lion_sd (k int, region text NOT NULL, name text NOT NULL);
INSERT INTO lion_sd
SELECT CASE WHEN i % 31 = 0 THEN NULL ELSE i END,
	   (ARRAY['eu', 'us', 'ap'])[1 + i % 3], 'n' || (i % 7)
  FROM generate_series(1, 400) i;
CREATE UNIQUE INDEX lion_sd_k ON lion_sd (k);

/*
 * The tables that only filter: lion_se, 80 rows over a unique key 1..80 with
 * NULL in every 13th row, a flag and a tag; lion_sc, keyed by an int8 primary
 * key the fact's int4 column is compared with; lion_sn, a key with no unique
 * index, repeated; lion_sb, 2,000 keys of the fact's id.
 */
CREATE TABLE lion_se (k int, flag bool, tag text NOT NULL);
INSERT INTO lion_se
SELECT CASE WHEN i % 13 = 0 THEN NULL ELSE i END, i % 3 = 0, 't' || (i % 4)
  FROM generate_series(1, 80) i;
CREATE UNIQUE INDEX lion_se_k ON lion_se (k);
CREATE TABLE lion_sc (k int8 PRIMARY KEY, grp text NOT NULL);
INSERT INTO lion_sc SELECT i, 'g' || (i % 3) FROM generate_series(0, 19) i;
CREATE TABLE lion_sn (k int, flag bool);
INSERT INTO lion_sn SELECT i % 40, i % 3 = 0 FROM generate_series(1, 80) i;
CREATE TABLE lion_sb (k int PRIMARY KEY);
INSERT INTO lion_sb SELECT i * 7 FROM generate_series(1, 2000) i;

/*
 * The fact: 40,000 rows - fk over 1..300 (NULL in every 97th row), ek over
 * 1..90 so that some values have no lion_se row (NULL in every 89th), ck over
 * 0..24 against lion_sc's 0..19, x a filter of 7, id NOT NULL.
 */
CREATE TABLE lion_sf (id int NOT NULL, fk int, ek int, ck int, x int);
INSERT INTO lion_sf
SELECT i, CASE WHEN i % 97 = 0 THEN NULL ELSE 1 + (i * 7919) % 300 END,
	   CASE WHEN i % 89 = 0 THEN NULL ELSE 1 + (i / 300 + i % 11) % 90 END,
	   (i / 7) % 25, i % 7
  FROM generate_series(1, 40000) i;
CREATE INDEX lion_sf_fk ON lion_sf USING lion (fk);
CREATE INDEX lion_sf_ek ON lion_sf USING lion (ek);
CREATE INDEX lion_sf_ck ON lion_sf USING lion (ck);
CREATE INDEX lion_sf_x ON lion_sf USING lion (x);
CREATE INDEX lion_sf_id ON lion_sf USING lion (id);
VACUUM (FREEZE, ANALYZE) lion_sf;
ANALYZE lion_sd, lion_se, lion_sc, lion_sn, lion_sb;

-- ---- the plans ----

-- the folded table is a child of the node's, its key the IN list's values
SELECT * FROM lion_st_explain('SELECT d.region, count(*) FROM lion_sf f JOIN lion_sd d ON f.fk = d.k JOIN lion_se e ON f.ek = e.k WHERE e.flag GROUP BY d.region');
-- two of them, one compared across types
SELECT * FROM lion_st_explain('SELECT d.region, count(*) FROM lion_sf f JOIN lion_sd d ON f.fk = d.k JOIN lion_se e ON f.ek = e.k JOIN lion_sc c ON c.k = f.ck WHERE e.tag = ''t1'' AND c.grp = ''g2'' GROUP BY d.region');

-- ---- the answers, against the pushdown off ----

-- grouped by the dimension, the folded table's own filters applied, and none
SELECT lion_st('SELECT d.region, count(*) FROM lion_sf f JOIN lion_sd d ON f.fk = d.k JOIN lion_se e ON f.ek = e.k WHERE e.flag GROUP BY d.region');
SELECT lion_st('SELECT d.name, count(*), count(f.id) FROM lion_sf f JOIN lion_sd d ON f.fk = d.k JOIN lion_se e ON f.ek = e.k GROUP BY d.name');
SELECT lion_st('SELECT d.k, count(*) FROM lion_sf f JOIN lion_sd d ON f.fk = d.k JOIN lion_se e ON f.ek = e.k WHERE e.tag IN (''t0'', ''t3'') GROUP BY d.k');

-- two folded, one across types, and the fact's own filters beside them
SELECT lion_st('SELECT d.region, count(*) FROM lion_sf f JOIN lion_sd d ON f.fk = d.k JOIN lion_se e ON f.ek = e.k JOIN lion_sc c ON c.k = f.ck WHERE e.tag = ''t1'' AND c.grp = ''g2'' GROUP BY d.region');
SELECT lion_st('SELECT d.region, count(*) FROM lion_sf f JOIN lion_sd d ON f.fk = d.k JOIN lion_sc c ON c.k = f.ck WHERE f.x IN (2, 5) GROUP BY d.region');
SELECT lion_st('SELECT d.name, count(*) FROM lion_sf f JOIN lion_sd d ON f.fk = d.k JOIN lion_se e ON f.ek = e.k WHERE f.x = 3 AND d.region = ''us'' AND NOT e.flag GROUP BY d.name');

-- no GROUP BY, and every table but the fact only filtering: one of them is
-- the dimension
SELECT lion_st('SELECT count(*) FROM lion_sf f JOIN lion_sd d ON f.fk = d.k JOIN lion_se e ON f.ek = e.k WHERE e.flag');
SELECT lion_st('SELECT count(*) FROM lion_sf f JOIN lion_sd d ON f.fk = d.k JOIN lion_se e ON f.ek = e.k WHERE e.flag AND d.name = ''n2''');

-- the IN spelling, which core makes an inner join of a unique key
SELECT lion_st('SELECT d.region, count(*) FROM lion_sf f JOIN lion_sd d ON f.fk = d.k WHERE f.ek IN (SELECT k FROM lion_se WHERE flag) GROUP BY d.region');

-- a folded table no row of which passes its filters: no fact row at all
SELECT lion_st('SELECT d.region, count(*) FROM lion_sf f JOIN lion_sd d ON f.fk = d.k JOIN lion_se e ON f.ek = e.k WHERE e.tag = ''none'' GROUP BY d.region');
SELECT lion_st('SELECT count(*) FROM lion_sf f JOIN lion_sd d ON f.fk = d.k JOIN lion_se e ON f.ek = e.k WHERE e.tag = ''none'' AND d.region = ''eu''');

-- HAVING over the counts, and ORDER BY and LIMIT above them
SELECT lion_st('SELECT d.k, count(*) FROM lion_sf f JOIN lion_sd d ON f.fk = d.k JOIN lion_se e ON f.ek = e.k WHERE e.flag GROUP BY d.k HAVING count(*) > 36');
SELECT lion_st('SELECT d.name, count(*) FROM lion_sf f JOIN lion_sd d ON f.fk = d.k JOIN lion_sc c ON c.k = f.ck WHERE c.grp <> ''g0'' GROUP BY d.name ORDER BY count(*) DESC, d.name LIMIT 3');

-- ---- declined: a table that is not only a filter ----

-- a column of it grouped by or counted
SELECT lion_st('SELECT d.region, e.tag, count(*) FROM lion_sf f JOIN lion_sd d ON f.fk = d.k JOIN lion_se e ON f.ek = e.k GROUP BY d.region, e.tag');
SELECT lion_st('SELECT d.region, count(e.flag) FROM lion_sf f JOIN lion_sd d ON f.fk = d.k JOIN lion_se e ON f.ek = e.k GROUP BY d.region');
-- a key that is not unique: a fact row may join several of its rows
SELECT lion_st('SELECT d.region, count(*) FROM lion_sf f JOIN lion_sd d ON f.fk = d.k JOIN lion_sn n ON f.ek = n.k WHERE n.flag GROUP BY d.region');
-- joined to the dimension too, or to the fact by two clauses
SELECT lion_st('SELECT d.region, count(*) FROM lion_sf f JOIN lion_sd d ON f.fk = d.k JOIN lion_se e ON f.ek = e.k AND e.k < d.k GROUP BY d.region');
SELECT lion_st('SELECT d.region, count(*) FROM lion_sf f JOIN lion_sd d ON f.fk = d.k JOIN lion_se e ON f.ek = e.k AND e.k > f.x GROUP BY d.region');
-- a condition over it and the fact together
SELECT lion_st('SELECT d.region, count(*) FROM lion_sf f JOIN lion_sd d ON f.fk = d.k JOIN lion_se e ON f.ek = e.k WHERE e.flag OR f.x = 1 GROUP BY d.region');
-- an outer join of it, and an outer join beside it
SELECT lion_st('SELECT d.region, count(*) FROM lion_sf f JOIN lion_sd d ON f.fk = d.k LEFT JOIN lion_sn n ON f.ek = n.k GROUP BY d.region');
SELECT lion_st('SELECT d.name, count(f.id) FROM lion_sd d LEFT JOIN (lion_sf f JOIN lion_se e ON f.ek = e.k) ON f.fk = d.k GROUP BY d.name');
-- more keys than a count locates of one IN list at once, which at 64kB is a
-- thousand: the node would hold them all; 2,000 are taken at the default
SET work_mem = '64kB';
SELECT lion_st('SELECT d.region, count(*) FROM lion_sf f JOIN lion_sd d ON f.fk = d.k JOIN lion_sb b ON b.k = f.id GROUP BY d.region');
RESET work_mem;
SELECT lion_st('SELECT d.region, count(*) FROM lion_sf f JOIN lion_sd d ON f.fk = d.k JOIN lion_sb b ON b.k = f.id GROUP BY d.region');

-- ---- the counters ----

-- the keys collected are the folded table's rows that pass its filters,
-- less the NULL ones: 26 flagged, two of them with a NULL key
SELECT lion_st_counter('SELECT d.region, count(*) FROM lion_sf f JOIN lion_sd d ON f.fk = d.k JOIN lion_se e ON f.ek = e.k WHERE e.flag GROUP BY d.region', 'Join Filter Keys');
SELECT lion_st_counter('SELECT d.region, count(*) FROM lion_sf f JOIN lion_sd d ON f.fk = d.k JOIN lion_se e ON f.ek = e.k WHERE e.flag GROUP BY d.region', 'Join Keys Looked Up');

-- batches at a small work_mem
SET work_mem = '64kB';
SELECT lion_st('SELECT d.k, count(*) FROM lion_sf f JOIN lion_sd d ON f.fk = d.k JOIN lion_se e ON f.ek = e.k WHERE NOT e.flag GROUP BY d.k');
RESET work_mem;

-- a correlated subquery that rescans the node with a new filter of the
-- folded table's: its keys are collected again each time
SELECT lion_st('SELECT r.tag, (SELECT sum(c) FROM (SELECT d.region, count(*) AS c FROM lion_sf f JOIN lion_sd d ON f.fk = d.k JOIN lion_se e ON f.ek = e.k WHERE e.tag = r.tag GROUP BY d.region) s) FROM (VALUES (''t0''), (''t2''), (''none'')) r(tag)');

-- parallel plans on: with a folded table the node is offered serial only
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SELECT lion_st('SELECT d.region, count(*) FROM lion_sf f JOIN lion_sd d ON f.fk = d.k JOIN lion_se e ON f.ek = e.k WHERE e.flag GROUP BY d.region');
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET min_parallel_index_scan_size;

-- a partitioned fact: the IN list is each leaf's filter
CREATE TABLE lion_sp (id int NOT NULL, fk int, ek int) PARTITION BY RANGE (id);
CREATE TABLE lion_sp1 PARTITION OF lion_sp FOR VALUES FROM (MINVALUE) TO (15000);
CREATE TABLE lion_sp2 PARTITION OF lion_sp FOR VALUES FROM (15000) TO (MAXVALUE);
INSERT INTO lion_sp SELECT id, fk, ek FROM lion_sf;
CREATE INDEX lion_sp_fk ON lion_sp USING lion (fk);
CREATE INDEX lion_sp_ek ON lion_sp USING lion (ek);
VACUUM (FREEZE, ANALYZE) lion_sp;
SELECT lion_st('SELECT d.region, count(*) FROM lion_sp f JOIN lion_sd d ON f.fk = d.k JOIN lion_se e ON f.ek = e.k WHERE e.flag GROUP BY d.region');

-- a dirty heap, no VACUUM: deletes and updates in the fact, in the dimension
-- and in the folded table, whose updated rows move in and out of its filter
DELETE FROM lion_sf WHERE id % 11 = 0;
UPDATE lion_sf SET ek = 1 + (ek * 7) % 90 WHERE id % 13 = 0;
UPDATE lion_sf SET fk = 1 + (fk * 3) % 300 WHERE id % 17 = 0;
DELETE FROM lion_sd WHERE k % 19 = 0;
UPDATE lion_se SET flag = NOT flag WHERE k % 5 = 0;
DELETE FROM lion_se WHERE k % 7 = 0;
INSERT INTO lion_se VALUES (NULL, true, 't9'), (85, true, 't9');
SELECT lion_st('SELECT d.region, count(*) FROM lion_sf f JOIN lion_sd d ON f.fk = d.k JOIN lion_se e ON f.ek = e.k WHERE e.flag GROUP BY d.region');
SELECT lion_st('SELECT d.name, count(*) FROM lion_sf f JOIN lion_sd d ON f.fk = d.k JOIN lion_se e ON f.ek = e.k JOIN lion_sc c ON c.k = f.ck WHERE c.grp = ''g1'' GROUP BY d.name');
SELECT lion_st('SELECT count(*) FROM lion_sf f JOIN lion_sd d ON f.fk = d.k JOIN lion_se e ON f.ek = e.k WHERE e.tag = ''t9''');
SELECT lion_st_counter('SELECT d.region, count(*) FROM lion_sf f JOIN lion_sd d ON f.fk = d.k JOIN lion_se e ON f.ek = e.k WHERE e.flag GROUP BY d.region', 'Join Filter Keys');

DROP TABLE lion_sf, lion_sd, lion_se, lion_sc, lion_sn, lion_sb, lion_sp;
DROP FUNCTION lion_st(text), lion_st_explain(text), lion_st_counter(text, text);
