-- lionvec's parallel build (DESIGN.md §44.12).  Every index here is built
-- serially and in parallel, and the two are read off their pages and
-- compared: a parallel build makes a serial build's index but for where the
-- rows' REFINE items lie, which only their CODE items point at.  The DEBUG1
-- lines show which way a build went; the first of each pair is core's, which
-- before PostgreSQL 17 builds only btree indexes in parallel and so says
-- "serially" of every lionvec build (vector_parallel_1.out), and from 17 on
-- sizes a build by the heap's main fork, where pgvector's vectors are not.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_lion_vector;
CREATE EXTENSION IF NOT EXISTS pageinspect;
RESET client_min_messages;
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

/*
 * pv_vec() is a pseudo-random vector of n coordinates, the same on every
 * platform (hashint8extended(), not random()), near one of twelve centres.
 */
CREATE FUNCTION pv_vec(i bigint, n int) RETURNS vector LANGUAGE sql IMMUTABLE
-- an index expression below calls it, and CREATE INDEX runs it under a safe
-- search_path from PostgreSQL 17 on, where vector would not be found
SET search_path FROM CURRENT AS $$
	SELECT array_agg(((hashint8extended(i * 1000 + j, 7) % 100000)::float8 / 400000.0
					  + (hashint8extended((i % 12) * 1000 + j, 9) % 100000)::float8 / 100000.0
					  )::real ORDER BY j)::vector
	  FROM generate_series(1, n) j
$$;

/*
 * pv_index() reads a lionvec index off its pages (pageinspect) into what a
 * serial and a parallel build of it must agree on, as digests: the meta page,
 * but for where the chains after the REFINE pages are and the REFINE page
 * inserts append to (a parallel build's processes each leave one partly
 * filled); the PART items in order, but for
 * where their CODE chains start and end; the REGION items in order, but for
 * the block of their first PART item; each chain's CODE items in order -
 * every partition's, then the rows' in no partition, then the NULL rows' -
 * but for where their REFINE items are; and the REFINE items, as a set.  The
 * offsets assume a little-endian server.
 */
CREATE FUNCTION pv_u16(p bytea, o int) RETURNS int LANGUAGE sql IMMUTABLE AS
$$ SELECT get_byte(p, o) | (get_byte(p, o + 1) << 8) $$;
CREATE FUNCTION pv_u32(p bytea, o int) RETURNS bigint LANGUAGE sql IMMUTABLE AS
$$ SELECT pv_u16(p, o)::bigint | (pv_u16(p, o + 2)::bigint << 16) $$;
CREATE FUNCTION pv_items(p bytea) RETURNS SETOF bytea LANGUAGE plpgsql AS $$
DECLARE
	i int;
	lp bigint;
BEGIN
	FOR i IN 1 .. (pv_u16(p, 12) - 24) / 4 LOOP
		lp := pv_u32(p, 20 + 4 * i);
		CONTINUE WHEN (lp >> 15) & 3 <> 1;		-- not LP_NORMAL
		RETURN NEXT substr(p, (lp & 32767)::int + 1, (lp >> 17)::int);
	END LOOP;
END $$;
CREATE FUNCTION pv_index(idx regclass)
RETURNS TABLE (meta text, parts text, regions text, codes text, refines text)
LANGUAGE plpgsql AS $$
DECLARE
	bs int := current_setting('block_size')::int;
	nb int := pg_relation_size(idx) / current_setting('block_size')::int;
	m bytea := get_raw_page(idx::text, 0);
	p bytea;
	it bytea;
	heads bigint[] := '{}';
	acc bytea[] := '{}';
	pb bigint;
	h bigint;
BEGIN
	-- the meta page's contents, from 24, but for the blocks of the PART,
	-- REST and NULL chains (28, 36 to 51), refine_tail (52) and the REGION
	-- chain's (72), which follow the REFINE pages; nregions (76) is kept
	meta := md5(substr(m, 25, 28) || substr(m, 57, 4) || substr(m, 81, 16) ||
				substr(m, 101, 4));
	-- the PART chain (its head at 28): partno, then head and tail, skipped
	pb := pv_u32(m, 52);
	WHILE pb <> 4294967295 LOOP
		p := get_raw_page(idx::text, pb::int);
		FOR it IN SELECT pv_items(p) LOOP
			heads := heads || pv_u32(it, 4);
			acc := acc || (substr(it, 1, 4) || substr(it, 13));
		END LOOP;
		pb := pv_u32(p, bs - 16);
	END LOOP;
	parts := md5(array_to_string(array(SELECT encode(x, 'hex') FROM unnest(acc) x), '/'));
	-- the REGION chain (its head at 72): pblk, at 12 to 15 in an item, skipped
	acc := '{}';
	pb := pv_u32(m, 96);
	WHILE pb <> 4294967295 LOOP
		p := get_raw_page(idx::text, pb::int);
		FOR it IN SELECT pv_items(p) LOOP
			acc := acc || (substr(it, 1, 12) || substr(it, 17));
		END LOOP;
		pb := pv_u32(p, bs - 16);
	END LOOP;
	regions := md5(array_to_string(array(SELECT encode(x, 'hex') FROM unnest(acc) x), '/'));
	-- the CODE chains, then the REST and NULL chains' (36, 44): rblk and roff,
	-- at 20 to 25 in an item, skipped
	acc := '{}';
	heads := heads || pv_u32(m, 60) || pv_u32(m, 68);
	FOREACH h IN ARRAY heads LOOP
		acc := acc || '\x'::bytea;
		WHILE h <> 4294967295 LOOP
			p := get_raw_page(idx::text, h::int);
			FOR it IN SELECT pv_items(p) LOOP
				acc := acc || (substr(it, 1, 20) || substr(it, 27));
			END LOOP;
			h := pv_u32(p, bs - 16);
		END LOOP;
	END LOOP;
	codes := md5(array_to_string(array(SELECT encode(x, 'hex') FROM unnest(acc) x), '/'));
	-- every REFINE page's (type 3) items, sorted
	SELECT md5(string_agg(encode(x, 'hex'), '/' ORDER BY x)) INTO refines
	  FROM generate_series(1, nb - 1) b,
		   LATERAL (SELECT get_raw_page(idx::text, b) AS pg) g,
		   LATERAL pv_items(g.pg) x
	 WHERE pv_u16(g.pg, bs - 4) = 3;
	RETURN NEXT;
END $$;

-- REINDEX idx with workers allowed, and read it
CREATE FUNCTION pv_build(idx regclass, workers int)
RETURNS TABLE (meta text, parts text, regions text, codes text, refines text)
LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('max_parallel_maintenance_workers', workers::text, true);
	EXECUTE format('REINDEX INDEX %s', idx);
	RETURN QUERY SELECT * FROM pv_index(idx);
END $$;

-- the ten nearest rows to a few vectors, through the index
CREATE FUNCTION pv_nearest(op text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	r text := '';
	q int;
	ids int[];
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_sort', 'off', true);
	FOR q IN 1 .. 5 LOOP
		EXECUTE format('SELECT array_agg(id) FROM (SELECT id FROM pv ORDER BY v %s pv_vec(%s, 24) LIMIT 10) s',
					   op, 100000 + q) INTO ids;
		r := r || ids::text;
	END LOOP;
	RETURN md5(r);
END $$;

-- ---------------------------------------------------------------------
-- 1. Every kind of row - partitioned, NULL, a cosine zero, deleted, and
--    HOT-updated (indexed under its chain's root TID) - and every kind of
--    index: each metric, spilling or not, one partition, a halfvec
--    expression, and 100 partitions in ten regions, whose k-means is routed
--    through regions remade at each iteration by the leader, and whose
--    second pass is too (DESIGN.md §44.3).  No sample here is cut: k-means
--    is given every row of a table below LIONVEC_SAMPLE_MIN rows.
--    parallel_workers fixes the worker count, whatever the table's size
--    (lionvec_build_workers()).
-- ---------------------------------------------------------------------

CREATE TABLE pv (id int, v vector(24), w text) WITH (parallel_workers = 2);
INSERT INTO pv SELECT i, pv_vec(i, 24), 'x' FROM generate_series(1, 6000) i;
INSERT INTO pv SELECT i, NULL, 'x' FROM generate_series(6001, 6003) i;
INSERT INTO pv VALUES (6004, array_fill(0, ARRAY[24])::real[]::vector, 'x');
DELETE FROM pv WHERE id % 97 = 0;
UPDATE pv SET w = 'y' WHERE id % 89 = 0;

CREATE INDEX pv_l2 ON pv USING lionvec (v) WITH (partitions = 40);
CREATE INDEX pv_ip ON pv USING lionvec (v vector_ip_ops) WITH (partitions = 40);
CREATE INDEX pv_cos ON pv USING lionvec (v vector_cosine_ops) WITH (partitions = 40);
CREATE INDEX pv_nospill ON pv USING lionvec (v) WITH (partitions = 40, spill = off);
CREATE INDEX pv_one ON pv USING lionvec (v) WITH (partitions = 1);
CREATE INDEX pv_half ON pv USING lionvec ((v::halfvec(24)) halfvec_l2_ops) WITH (partitions = 40);
CREATE INDEX pv_reg ON pv USING lionvec (v) WITH (partitions = 100);
CREATE INDEX pv_regcos ON pv USING lionvec (v vector_cosine_ops) WITH (partitions = 100);

CREATE TABLE pv_serial AS
SELECT i AS idx, b.* FROM unnest('{pv_l2,pv_ip,pv_cos,pv_nospill,pv_one,pv_half,pv_reg,pv_regcos}'::regclass[]) i,
	   LATERAL pv_build(i, 0) b;
CREATE TABLE pv_near AS
SELECT op, pv_nearest(op) AS ids FROM unnest('{<->,<#>,<=>}'::text[]) op;
CREATE TABLE pv_parallel AS
SELECT i AS idx, b.* FROM unnest('{pv_l2,pv_ip,pv_cos,pv_nospill,pv_one,pv_half,pv_reg,pv_regcos}'::regclass[]) i,
	   LATERAL pv_build(i, 2) b;
SELECT s.idx AS index, s.meta = p.meta AS meta, s.parts = p.parts AS parts,
	   s.regions = p.regions AS regions, s.codes = p.codes AS codes,
	   s.refines = p.refines AS refines
  FROM pv_serial s JOIN pv_parallel p ON p.idx = s.idx ORDER BY s.idx::text;
-- the regions are there, and only where there are 64 partitions or more
SELECT i AS index, pv_u32(get_raw_page(i::text, 0), 100) AS regions
  FROM unnest('{pv_l2,pv_one,pv_reg,pv_regcos}'::regclass[]) i;
-- and the indexes answer as they did
DROP INDEX pv_half;
SELECT n.op, pv_nearest(n.op) = n.ids AS same FROM pv_near n ORDER BY n.op;

-- which way each build went
SET client_min_messages = debug1;
SET max_parallel_maintenance_workers = 2;
REINDEX INDEX pv_l2;
REINDEX INDEX pv_reg;
SET max_parallel_maintenance_workers = 0;
REINDEX INDEX pv_l2;
REINDEX INDEX pv_reg;
RESET max_parallel_maintenance_workers;
SET client_min_messages = warning;

-- CREATE INDEX CONCURRENTLY, which reads an MVCC snapshot, in parallel
SET max_parallel_maintenance_workers = 2;
CREATE INDEX CONCURRENTLY pv_cic ON pv USING lionvec (v) WITH (partitions = 40);
RESET max_parallel_maintenance_workers;
CREATE TABLE pv_cic_parallel AS SELECT * FROM pv_index('pv_cic');
SELECT p.meta = s.meta AS meta, p.parts = s.parts AS parts, p.regions = s.regions AS regions,
	   p.codes = s.codes AS codes, p.refines = s.refines AS refines
  FROM pv_cic_parallel p, pv_build('pv_cic', 0) s;

-- ---------------------------------------------------------------------
-- 2. Without parallel_workers: a table of 10,000 rows with a vector or
--    more asks for max_parallel_maintenance_workers, whatever
--    maintenance_work_mem, and so does an expression or partial index
--    whose expressions and predicate are parallel safe; a smaller table,
--    a temporary table, a table of NULL rows and an index whose predicate
--    is parallel unsafe build serially.
-- ---------------------------------------------------------------------

CREATE TABLE pa (id int, v vector(8));
INSERT INTO pa SELECT i, pv_vec(i, 8) FROM generate_series(1, 10000) i;
CREATE INDEX pa_l2 ON pa USING lionvec (v) WITH (partitions = 20);
SET max_parallel_maintenance_workers = 2;
SET client_min_messages = debug1;
REINDEX INDEX pa_l2;
-- the least maintenance_work_mem, where each process's sort spills to
-- disk: still in parallel, and the same index as a serial build's
SET maintenance_work_mem = '1MB';
REINDEX INDEX pa_l2;
SET client_min_messages = warning;
CREATE TABLE pa_serial AS SELECT * FROM pv_build('pa_l2', 0);
SELECT p.meta = s.meta AS meta, p.parts = s.parts AS parts, p.regions = s.regions AS regions,
	   p.codes = s.codes AS codes, p.refines = s.refines AS refines
  FROM pv_build('pa_l2', 2) p, pa_serial s;
RESET maintenance_work_mem;
DELETE FROM pa WHERE id = 1;
VACUUM pa;
SET client_min_messages = debug1;
REINDEX INDEX pa_l2;
SET client_min_messages = warning;
INSERT INTO pa VALUES (1, pv_vec(1, 8));

-- expressions and predicates, parallel safe and not
CREATE FUNCTION pv_unsafe(i int) RETURNS bool LANGUAGE plpgsql IMMUTABLE PARALLEL UNSAFE
AS $$ BEGIN RETURN i > 0; END $$;
SET client_min_messages = debug1;
CREATE INDEX pa_half ON pa USING lionvec ((v::halfvec(8)) halfvec_l2_ops) WITH (partitions = 20);
CREATE INDEX pa_safe ON pa USING lionvec (v) WITH (partitions = 20) WHERE id > 0;
CREATE INDEX pa_unsafe ON pa USING lionvec (v) WITH (partitions = 20) WHERE pv_unsafe(id);
SET client_min_messages = warning;

CREATE TEMP TABLE pt (id int, v vector(8)) WITH (parallel_workers = 2);
INSERT INTO pt SELECT i, pv_vec(i, 8) FROM generate_series(1, 2000) i;
CREATE TABLE pn (id int, v vector(8)) WITH (parallel_workers = 2);
INSERT INTO pn SELECT i, NULL FROM generate_series(1, 2000) i;
SET client_min_messages = debug1;
CREATE INDEX pt_l2 ON pt USING lionvec (v) WITH (partitions = 20);
CREATE INDEX pn_l2 ON pn USING lionvec (v) WITH (partitions = 20);
SET client_min_messages = warning;
RESET max_parallel_maintenance_workers;

-- ---------------------------------------------------------------------
-- 3. The sample's cut (DESIGN.md §44.3): a table of more than
--    LIONVEC_SAMPLE_MIN rows in few partitions has its k-means given 65,536
--    of the rows it sampled, cut before a parallel build shares the sample,
--    and the same 65,536 either way: the meta page and the PART items - the
--    centroids, and each partition's count, spread and longest row - are a
--    serial build's.  (pv_index()'s CODE digest would take minutes over
--    140,000 items, so pv_parts() stops at the PART chain.)
-- ---------------------------------------------------------------------

CREATE FUNCTION pv_parts(idx regclass) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	bs int := current_setting('block_size')::int;
	m bytea := get_raw_page(idx::text, 0);
	p bytea;
	it bytea;
	acc bytea[] := '{}';
	pb bigint := pv_u32(m, 52);
BEGIN
	WHILE pb <> 4294967295 LOOP
		p := get_raw_page(idx::text, pb::int);
		FOR it IN SELECT pv_items(p) LOOP
			acc := acc || (substr(it, 1, 4) || substr(it, 13));
		END LOOP;
		pb := pv_u32(p, bs - 16);
	END LOOP;
	RETURN md5(substr(m, 25, 28) || substr(m, 57, 4) || substr(m, 81, 16) || substr(m, 101, 4)) ||
		   md5(array_to_string(array(SELECT encode(x, 'hex') FROM unnest(acc) x), '/'));
END $$;
CREATE TABLE pc (id int, v vector(8)) WITH (parallel_workers = 2);
INSERT INTO pc SELECT i, pv_vec(i, 8) FROM generate_series(1, 70000) i;
SET max_parallel_maintenance_workers = 2;
SET client_min_messages = debug1;
CREATE INDEX pc_l2 ON pc USING lionvec (v) WITH (partitions = 20);
SET client_min_messages = warning;
CREATE TABLE pc_parallel AS SELECT pv_parts('pc_l2') AS parts;
SET max_parallel_maintenance_workers = 0;
SET client_min_messages = debug1;
REINDEX INDEX pc_l2;
SET client_min_messages = warning;
RESET max_parallel_maintenance_workers;
SELECT p.parts = pv_parts('pc_l2') AS "meta page and partitions the same" FROM pc_parallel p;

DROP TABLE pv, pv_serial, pv_parallel, pv_near, pv_cic_parallel, pa, pa_serial, pt, pn,
	pc, pc_parallel;
DROP FUNCTION pv_unsafe(int), pv_vec(bigint, int), pv_u16(bytea, int), pv_u32(bytea, int),
	pv_items(bytea), pv_index(regclass), pv_build(regclass, int), pv_nearest(text),
	pv_parts(regclass);
