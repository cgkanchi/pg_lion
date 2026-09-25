-- Multicolumn indexes (DESIGN.md §24).
--
-- One relation holds each key column's keys as an independent set of entries,
-- GIN's multicolumn model: a query on any subset of the columns intersects the
-- matching columns' posting sets.  Every query below is run through the index
-- (bitmap path, count pushdown OFF, so the ACCESS METHOD answers) and through a
-- forced sequential scan, and the two results are compared as multisets in both
-- directions.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';

/* VACUUM can only set all-visible for commits that reached disk (citext.sql). */
SET synchronous_commit = on;
SET default_statistics_target = 1000;

CREATE EXTENSION IF NOT EXISTS pg_lion;

/*
 * Run one query through the ACCESS METHOD (count pushdown off, seqscan off) and
 * through a plain sequential scan, and prove the two results are equal as
 * multisets.  The pushdown is deliberately disabled for sections 1 to 10:
 * what they are about is the AM, and the node would answer the counts among
 * them without reading an index page the way a bitmap scan does.  Section 11
 * is the pushdown over the same kind of index.
 */
CREATE FUNCTION lion_mccmp(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	node text := 'seq scan';
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'off', true);	-- the bitmap path (§29)
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Bitmap Index Scan%' THEN
			node := 'index scan';
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lion_mc_idx AS %s', q);

	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	EXECUTE format('CREATE TEMP TABLE lion_mc_seq AS %s', q);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);

	EXECUTE 'SELECT count(*) FROM lion_mc_idx' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_mc_idx EXCEPT ALL SELECT * FROM lion_mc_seq) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_mc_seq EXCEPT ALL SELECT * FROM lion_mc_idx) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lion_mc_idx, lion_mc_seq';

	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows', node, nrows);
END $$;

-- ---- the fixture ----------------------------------------------------------
--
-- k: 40 distinct ints with a NULL group.  t: 300 distinct texts, so its
-- posting sets are thin and sparse segments appear.  a: an int4[] with NULL
-- rows, '{}' rows and all-NULL-element rows, so BOTH reserved entries of §14
-- and §17 exist for that column and for no other.

CREATE TABLE lion_mc (
	id	int PRIMARY KEY,
	k	int,
	t	text,
	a	int4[]
);

INSERT INTO lion_mc
SELECT i,
	   CASE WHEN i % 23 = 0 THEN NULL ELSE i % 40 END,
	   't' || (i % 300),
	   CASE WHEN i % 31 = 0 THEN NULL
			WHEN i % 31 = 1 THEN '{}'::int4[]
			WHEN i % 31 = 2 THEN ARRAY[NULL, NULL]::int4[]
			ELSE ARRAY[i % 7, i % 11,
					   CASE WHEN i % 5 = 0 THEN NULL ELSE i % 13 END]
	   END
FROM generate_series(1, 20000) i;

CREATE INDEX lion_mc_kta ON lion_mc USING lion (k, t, a);

VACUUM (ANALYZE) lion_mc;

-- ---- 1. the shape ---------------------------------------------------------

-- One row per key column.  ntids of the two scalar columns is the row count;
-- the array column stores one TID per (key, row) pair plus one per key-less
-- row, and only IT has an EMPTY entry.
SELECT attno, ordered, entries, ntids, null_tids, empty_tids
  FROM lion_index_stats('lion_mc_kta');

SELECT (SELECT ntids FROM lion_index_stats('lion_mc_kta') WHERE attno = 1)
	 = (SELECT count(*) FROM lion_mc) AS k_one_tid_per_row,
	   (SELECT null_tids FROM lion_index_stats('lion_mc_kta') WHERE attno = 1)
	 = (SELECT count(*) FROM lion_mc WHERE k IS NULL) AS k_nulls,
	   (SELECT null_tids FROM lion_index_stats('lion_mc_kta') WHERE attno = 3)
	 = (SELECT count(*) FROM lion_mc WHERE a IS NULL) AS a_nulls,
	   (SELECT empty_tids FROM lion_index_stats('lion_mc_kta') WHERE attno = 3)
	 = (SELECT count(*) FROM lion_mc WHERE a IS NOT NULL AND NOT EXISTS
		(SELECT 1 FROM unnest(a) e WHERE e IS NOT NULL)) AS a_empties;

-- Each column's entry count is that column's own distinct keys (plus its
-- reserved entries), and nothing about any other column.
SELECT (SELECT entries FROM lion_index_stats('lion_mc_kta') WHERE attno = 1)
	 = (SELECT count(DISTINCT k) + 1 FROM lion_mc) AS k_entries,
	   (SELECT entries FROM lion_index_stats('lion_mc_kta') WHERE attno = 2)
	 = (SELECT count(DISTINCT t) FROM lion_mc) AS t_entries,
	   (SELECT entries FROM lion_index_stats('lion_mc_kta') WHERE attno = 3)
	 = (SELECT count(DISTINCT e) + 2 FROM lion_mc, unnest(a) x(e)
		 WHERE e IS NOT NULL) AS a_entries;

SELECT lion_index_verify('lion_mc_kta', true);

-- ---- 2. each column alone -------------------------------------------------

SELECT lion_mccmp($$SELECT id FROM lion_mc WHERE k = 7$$);
SELECT lion_mccmp($$SELECT id FROM lion_mc WHERE t = 't42'$$);
SELECT lion_mccmp($$SELECT id FROM lion_mc WHERE a @> ARRAY[3]$$);
SELECT lion_mccmp($$SELECT id FROM lion_mc WHERE a && ARRAY[3,9]$$);

-- an absent key in each column
SELECT lion_mccmp($$SELECT id FROM lion_mc WHERE k = 12345$$);
SELECT lion_mccmp($$SELECT id FROM lion_mc WHERE t = 'nosuchvalue'$$);
SELECT lion_mccmp($$SELECT id FROM lion_mc WHERE a @> ARRAY[9999]$$);

-- ---- 3. pairs, and all three ----------------------------------------------

SELECT lion_mccmp($$SELECT id FROM lion_mc WHERE k = 7 AND t = 't47'$$);
SELECT lion_mccmp($$SELECT id FROM lion_mc WHERE k = 7 AND a @> ARRAY[3]$$);
SELECT lion_mccmp($$SELECT id FROM lion_mc WHERE t = 't42' AND a @> ARRAY[3]$$);
SELECT lion_mccmp($$SELECT id FROM lion_mc
					WHERE k = 7 AND t = 't47' AND a @> ARRAY[3]$$);

-- the column order in the query is irrelevant: the index is order-insensitive
SELECT lion_mccmp($$SELECT id FROM lion_mc
					WHERE a @> ARRAY[3] AND t = 't47' AND k = 7$$);

-- one column absent makes the whole intersection empty
SELECT lion_mccmp($$SELECT id FROM lion_mc WHERE k = 12345 AND t = 't42'$$);

-- the query plan really is one Bitmap Index Scan with a multi-column condition
-- (with plain index scans off: one row goes to one since DESIGN.md §29)
SET enable_indexscan = off;
EXPLAIN (COSTS OFF)
SELECT id FROM lion_mc WHERE k = 7 AND t = 't47' AND a @> ARRAY[3];
RESET enable_indexscan;

-- ---- 4. IN lists, per column and across columns ---------------------------

SELECT lion_mccmp($$SELECT id FROM lion_mc WHERE k IN (1,2,3)$$);
SELECT lion_mccmp($$SELECT id FROM lion_mc WHERE t IN ('t1','t2','t3')$$);
SELECT lion_mccmp($$SELECT id FROM lion_mc
					WHERE k IN (1,2,3) AND t IN ('t1','t41','t81')$$);
SELECT lion_mccmp($$SELECT id FROM lion_mc
					WHERE k IN (1,2,3) AND a @> ARRAY[1]$$);

-- cross-type equality still descends the right column's directory
SELECT lion_mccmp($$SELECT id FROM lion_mc WHERE k = 7::int8 AND t = 't47'$$);

-- ---- 5. NULLs, per column -------------------------------------------------

SELECT lion_mccmp($$SELECT id FROM lion_mc WHERE k IS NULL$$);
SELECT lion_mccmp($$SELECT id FROM lion_mc WHERE a IS NULL$$);
SELECT lion_mccmp($$SELECT id FROM lion_mc WHERE k IS NULL AND t = 't42'$$);
SELECT lion_mccmp($$SELECT id FROM lion_mc WHERE k IS NULL AND a IS NULL$$);

-- IS NOT NULL is the complement of a posting set, which the evaluator cannot
-- build: the clause is dropped and the heap rechecks it, so the answer is
-- still exact.
SELECT lion_mccmp($$SELECT id FROM lion_mc WHERE k IS NOT NULL AND t = 't42'$$);
SELECT lion_mccmp($$SELECT id FROM lion_mc WHERE k = 7 AND a IS NOT NULL$$);

-- `t IS NOT NULL` alone walks that one column and nothing else
SELECT lion_mccmp($$SELECT count(*) FROM lion_mc WHERE t IS NOT NULL$$);

-- a multi-key query the index cannot answer exactly (`<@`) next to one it can
SELECT lion_mccmp($$SELECT id FROM lion_mc
					WHERE k = 7 AND a <@ ARRAY[0,1,2,3,4,5,6,7,8,9,10,11,12]$$);

-- ---- 6. writes ------------------------------------------------------------

INSERT INTO lion_mc
SELECT i,
	   CASE WHEN i % 17 = 0 THEN NULL ELSE i % 40 END,
	   't' || (i % 300),
	   CASE WHEN i % 29 = 0 THEN NULL ELSE ARRAY[i % 7, i % 11] END
FROM generate_series(20001, 23000) i;

UPDATE lion_mc SET k = k + 1000 WHERE id % 97 = 0;
UPDATE lion_mc SET t = NULL WHERE id % 89 = 0;
UPDATE lion_mc SET a = '{}'::int4[] WHERE id % 83 = 0;
DELETE FROM lion_mc WHERE id % 71 = 0;

SELECT lion_index_verify('lion_mc_kta', true);
SELECT lion_mccmp($$SELECT id FROM lion_mc WHERE k = 1007 AND t = 't47'$$);
SELECT lion_mccmp($$SELECT id FROM lion_mc WHERE t IS NULL$$);
SELECT lion_mccmp($$SELECT id FROM lion_mc WHERE k = 7 AND a @> ARRAY[3]$$);

VACUUM (ANALYZE) lion_mc;

SELECT lion_index_verify('lion_mc_kta', true);

-- every column's ntids is back to what the heap holds
SELECT (SELECT ntids FROM lion_index_stats('lion_mc_kta') WHERE attno = 1)
	 = (SELECT count(*) FROM lion_mc) AS k_exact,
	   (SELECT ntids FROM lion_index_stats('lion_mc_kta') WHERE attno = 2)
	 = (SELECT count(*) FROM lion_mc) AS t_exact,
	   (SELECT null_tids FROM lion_index_stats('lion_mc_kta') WHERE attno = 2)
	 = (SELECT count(*) FROM lion_mc WHERE t IS NULL) AS t_nulls;

SELECT lion_mccmp($$SELECT id FROM lion_mc WHERE k = 7 AND t = 't47'$$);

-- ---- 7. REINDEX equivalence -----------------------------------------------
--
-- ambuild and aminsert must produce the same index: the same entries, the same
-- TID totals and the same answers.

CREATE TEMP TABLE lion_mc_before AS
	SELECT * FROM lion_index_stats('lion_mc_kta');

REINDEX INDEX lion_mc_kta;

SELECT lion_index_verify('lion_mc_kta', true);

SELECT b.attno, b.entries = a.entries AS entries, b.ntids = a.ntids AS ntids,
	   b.null_tids = a.null_tids AS null_tids,
	   b.empty_tids = a.empty_tids AS empty_tids
  FROM lion_mc_before b
  JOIN lion_index_stats('lion_mc_kta') a USING (attno)
 ORDER BY b.attno;

SELECT lion_mccmp($$SELECT id FROM lion_mc WHERE k = 7 AND t = 't47'$$);
SELECT lion_mccmp($$SELECT id FROM lion_mc WHERE k IS NULL AND a IS NULL$$);

-- ---- 8. built on an empty table, then filled -------------------------------

CREATE TABLE lion_mc_empty (k int, t text);
CREATE INDEX lion_mc_empty_i ON lion_mc_empty USING lion (k, t);

SELECT attno, entries, ntids FROM lion_index_stats('lion_mc_empty_i');
SELECT lion_index_verify('lion_mc_empty_i', true);

INSERT INTO lion_mc_empty
SELECT CASE WHEN i % 13 = 0 THEN NULL ELSE i % 50 END, 'e' || (i % 200)
FROM generate_series(1, 5000) i;

VACUUM (ANALYZE) lion_mc_empty;

SELECT attno, entries, ntids, null_tids FROM lion_index_stats('lion_mc_empty_i');
SELECT lion_index_verify('lion_mc_empty_i', true);

SELECT lion_mccmp($$SELECT k, t FROM lion_mc_empty WHERE k = 7 AND t = 'e57'$$);
SELECT lion_mccmp($$SELECT k, t FROM lion_mc_empty WHERE k IS NULL AND t = 'e13'$$);

-- ---- 9. a mixed scalar + tsvector index ------------------------------------

CREATE TABLE lion_mc_ts (id int, g int, tsv tsvector);
INSERT INTO lion_mc_ts
SELECT i, i % 9,
	   CASE WHEN i % 41 = 0 THEN NULL
			WHEN i % 41 = 1 THEN ''::tsvector
			ELSE to_tsvector('simple',
							 'w' || (i % 17) || ' w' || (i % 19) ||
							 ' w' || (i % 23))
	   END
FROM generate_series(1, 6000) i;

CREATE INDEX lion_mc_ts_i ON lion_mc_ts USING lion (g, tsv);
VACUUM (ANALYZE) lion_mc_ts;

SELECT attno, ordered, entries, null_tids, empty_tids
  FROM lion_index_stats('lion_mc_ts_i');
SELECT lion_index_verify('lion_mc_ts_i', true);

SELECT lion_mccmp($$SELECT id FROM lion_mc_ts WHERE tsv @@ 'w3'::tsquery$$);
SELECT lion_mccmp($$SELECT id FROM lion_mc_ts
					WHERE g = 4 AND tsv @@ 'w3'::tsquery$$);
SELECT lion_mccmp($$SELECT id FROM lion_mc_ts
					WHERE g = 4 AND tsv @@ 'w3 & w5'::tsquery$$);
SELECT lion_mccmp($$SELECT id FROM lion_mc_ts
					WHERE g = 4 AND tsv @@ 'w3 | w5'::tsquery$$);
-- a phrase query needs positions the index does not store: dropped, rechecked
SELECT lion_mccmp($$SELECT id FROM lion_mc_ts
					WHERE g = 4 AND tsv @@ 'w3 <-> w5'::tsquery$$);
SELECT lion_mccmp($$SELECT id FROM lion_mc_ts WHERE g = 4 AND tsv IS NULL$$);

-- ---- 10. what the DDL accepts and refuses ----------------------------------

-- INCLUDE columns are not supported (amcaninclude is false)
CREATE INDEX ON lion_mc USING lion (k) INCLUDE (t);

-- more than INDEX_MAX_KEYS columns is core's own limit
DO $$ BEGIN
	EXECUTE 'CREATE INDEX ON lion_mc USING lion (' ||
			(SELECT string_agg('k', ',') FROM generate_series(1, 33)) || ')';
	RAISE WARNING 'a 33-column index was accepted';
EXCEPTION WHEN OTHERS THEN RAISE WARNING 'refused: %', SQLERRM;
END $$;

/*
 * A DUPLICATE column and an EXPRESSION column both WORK, which is a deviation
 * from §24's "not in scope": the key column number an entry carries is the
 * INDEX column's, never a heap attribute's, so neither needed a line of code.
 * A duplicate column is two identical key sets - pointless, but not wrong -
 * and the planner picks whichever of them it likes.
 */
CREATE INDEX lion_mc_dup ON lion_mc USING lion (k, k);
SELECT attno, entries, ntids FROM lion_index_stats('lion_mc_dup');
SELECT lion_index_verify('lion_mc_dup', true);
DROP INDEX lion_mc_dup;

CREATE INDEX lion_mc_expr ON lion_mc USING lion ((k % 3), t);
SELECT attno, entries FROM lion_index_stats('lion_mc_expr');
SELECT lion_index_verify('lion_mc_expr', true);
SELECT lion_mccmp($$SELECT id FROM lion_mc WHERE (k % 3) = 1 AND t = 't42'$$);
DROP INDEX lion_mc_expr;

-- ---- 11. the count pushdown over a multicolumn index (§10, §24) -----------
/*
 * The LionCount node reads a multicolumn index exactly as it reads n
 * single-column ones: every rule of DESIGN.md §10 is applied per (index, KEY
 * COLUMN), and a query that constrains three columns of one index becomes
 * three posting-set sources located through one relcache entry.
 *
 * Two things are checked for every query below.  The ANSWER, by running it
 * with the pushdown on and off and comparing the two as multisets in both
 * directions (lion_mcpd(), which is test/sql/pushdown.sql's lion_pd()).  And
 * the PLAN CHOICE, by asking the same question of a twin table carrying one
 * single-column lion index per column (lion_mcsame(), '@' standing for the
 * table): the cost model prices an index per RELATION, so without the
 * per-column correction of §24 a column of a five-column index would be
 * charged five directories and the node would be refused where the same query
 * over separate indexes is accepted.  "multicolumn X, single-column X" with
 * the two equal is the pin.
 */

/* The answer, with the pushdown on and off.  force disables every other plan
 * so that a shape the model would not pick is still EXERCISED. */
CREATE FUNCTION lion_mcpd(q text, force boolean DEFAULT false) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	pushed boolean := false;
	nrows bigint;
	ndiff bigint;
BEGIN
	IF force THEN
		PERFORM set_config('enable_seqscan', 'off', true);
		PERFORM set_config('enable_bitmapscan', 'off', true);
		PERFORM set_config('enable_indexscan', 'off', true);
		PERFORM set_config('enable_indexonlyscan', 'off', true);
	END IF;
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			pushed := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lion_mcpd_on AS %s', q);

	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE format('CREATE TEMP TABLE lion_mcpd_off AS %s', q);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM lion_mcpd_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_mcpd_on EXCEPT ALL SELECT * FROM lion_mcpd_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_mcpd_off EXCEPT ALL SELECT * FROM lion_mcpd_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lion_mcpd_on, lion_mcpd_off';

	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN pushed THEN 'pushed down' ELSE 'not pushed down' END,
				  nrows);
END $$;

/* Is the same query pushed down over ONE multicolumn index and over n
 * single-column ones?  Nothing is disabled: the model's answer IS the test. */
CREATE FUNCTION lion_mcsame(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	mc boolean := false;
	sc boolean := false;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || replace(q, '@', 'lion_mcp') LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			mc := true;
		END IF;
	END LOOP;
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || replace(q, '@', 'lion_mcs') LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			sc := true;
		END IF;
	END LOOP;
	RETURN format('multicolumn %s, single-column %s%s',
				  CASE WHEN mc THEN 'pushed' ELSE 'not pushed' END,
				  CASE WHEN sc THEN 'pushed' ELSE 'not pushed' END,
				  CASE WHEN mc = sc THEN '' ELSE '   *** DIFFER ***' END);
END $$;

/* A prepared statement's parameters, kept parameters by a generic plan. */
CREATE FUNCTION lion_mcprep(q text, args text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	rec record;
	pushed boolean := false;
	onrows text[] := '{}';
	offrows text[] := '{}';
	ndiff bigint;
BEGIN
	PERFORM set_config('plan_cache_mode', 'force_generic_plan', true);

	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE 'PREPARE lion_mcpp_on AS ' || q;
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) EXECUTE lion_mcpp_on(' || args || ')' LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			pushed := true;
		END IF;
	END LOOP;
	FOR rec IN EXECUTE 'EXECUTE lion_mcpp_on(' || args || ')' LOOP
		onrows := onrows || rec::text;
	END LOOP;

	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE 'PREPARE lion_mcpp_off AS ' || q;
	FOR rec IN EXECUTE 'EXECUTE lion_mcpp_off(' || args || ')' LOOP
		offrows := offrows || rec::text;
	END LOOP;

	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE 'DEALLOCATE lion_mcpp_on';
	EXECUTE 'DEALLOCATE lion_mcpp_off';

	SELECT (SELECT count(*) FROM (SELECT unnest(onrows)
								  EXCEPT ALL SELECT unnest(offrows)) a)
		 + (SELECT count(*) FROM (SELECT unnest(offrows)
								  EXCEPT ALL SELECT unnest(onrows)) b)
	  INTO ndiff;
	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;

	RETURN format('%s, %s rows',
				  CASE WHEN pushed THEN 'pushed down' ELSE 'not pushed down' END,
				  coalesce(array_length(onrows, 1), 0));
END $$;

/* What EXPLAIN ANALYZE says about the disjoint-sum short-circuit (§15). */
CREATE FUNCTION lion_mcsummed(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	res text := 'NOT PUSHED DOWN';
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF,'
					  ' BUFFERS OFF) ' || q LOOP
		IF btrim(split_part(ln, ':', 1)) = 'Posting Sets Summed' THEN
			res := format('Posting Sets Summed %s',
						  CASE WHEN btrim(split_part(ln, ':', 2))::bigint = 0
							   THEN '= 0' ELSE '> 0' END);
		END IF;
	END LOOP;
	RETURN res;
END $$;

-- The fixture: 100k rows, one index over every column, and a twin table with
-- one index per column.  The pushdown is at its best on an all-visible heap
-- (DESIGN.md §9), which is what the VACUUM is for.
CREATE TABLE lion_mcp (
	id	int		NOT NULL,
	a	int		NOT NULL,
	b	int		NOT NULL,
	c	text	NOT NULL,
	n	int,
	m	int
);
INSERT INTO lion_mcp
SELECT i, i % 10, i % 7, 'c' || (i % 4),
	   CASE WHEN i % 101 = 0 THEN NULL ELSE i % 5 END,
	   CASE WHEN i % 97 = 0 THEN NULL ELSE i % 3 END
  FROM generate_series(1, 100000) i;

CREATE INDEX lion_mcp_i ON lion_mcp USING lion (a, b, c, n, m);

CREATE TABLE lion_mcs (LIKE lion_mcp);
INSERT INTO lion_mcs SELECT * FROM lion_mcp;
CREATE INDEX lion_mcs_a ON lion_mcs USING lion (a);
CREATE INDEX lion_mcs_b ON lion_mcs USING lion (b);
CREATE INDEX lion_mcs_c ON lion_mcs USING lion (c);
CREATE INDEX lion_mcs_n ON lion_mcs USING lion (n);
CREATE INDEX lion_mcs_m ON lion_mcs USING lion (m);

VACUUM (ANALYZE) lion_mcp;
VACUUM (ANALYZE) lion_mcs;

SELECT lion_index_verify('lion_mcp_i', true);

-- EXPLAIN names the KEY COLUMN of a multicolumn index, and only of one: a
-- single-column index prints what it always printed.
EXPLAIN (COSTS OFF)
SELECT count(*) FROM lion_mcp WHERE a = 3 AND c = 'c1';
EXPLAIN (COSTS OFF)
SELECT count(*) FROM lion_mcs WHERE a = 3 AND c = 'c1';
EXPLAIN (COSTS OFF)
SELECT a, count(*) FROM lion_mcp WHERE b = 2 GROUP BY a;
EXPLAIN (COSTS OFF)
SELECT a, b, count(*) FROM lion_mcp GROUP BY a, b;
-- an OR is one source over several columns of the one index (§19)
EXPLAIN (COSTS OFF)
SELECT count(*) FROM lion_mcp WHERE a = 3 OR b = 2;
/*
 * And the sum-over-all of §14, whose driving column is not a group column at
 * all but the one its `IS NOT NULL` names - which is what the node has to
 * derive the driving index's key column from.  Nothing else can answer this
 * shape cheaply, so the other plans are switched off to pin it.
 */
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM lion_mcp WHERE n IS NOT NULL AND m IS NOT NULL;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
RESET enable_indexonlyscan;

-- ---- 11.1 one column at a time -------------------------------------------

SELECT lion_mcpd($$SELECT count(*) FROM lion_mcp WHERE a = 3$$);
SELECT lion_mcpd($$SELECT count(*) FROM lion_mcp WHERE b = 2$$);
SELECT lion_mcpd($$SELECT count(*) FROM lion_mcp WHERE c = 'c1'$$);
SELECT lion_mcpd($$SELECT count(*) FROM lion_mcp WHERE n = 4$$);
SELECT lion_mcpd($$SELECT count(*) FROM lion_mcp WHERE m = 1$$);
-- a cross-type constant, and one that matches no key at all
SELECT lion_mcpd($$SELECT count(*) FROM lion_mcp WHERE b = 2::int8$$);
SELECT lion_mcpd($$SELECT count(*) FROM lion_mcp WHERE c = 'nosuch'$$);

SELECT lion_mcsame($$SELECT count(*) FROM @ WHERE a = 3$$);
SELECT lion_mcsame($$SELECT count(*) FROM @ WHERE b = 2$$);
SELECT lion_mcsame($$SELECT count(*) FROM @ WHERE c = 'c1'$$);
SELECT lion_mcsame($$SELECT count(*) FROM @ WHERE m = 1$$);

-- ---- 11.2 GROUP BY each column -------------------------------------------

SELECT lion_mcpd($$SELECT a, count(*) FROM lion_mcp GROUP BY a$$);
SELECT lion_mcpd($$SELECT b, count(*) FROM lion_mcp GROUP BY b$$);
SELECT lion_mcpd($$SELECT c, count(*) FROM lion_mcp GROUP BY c$$);
SELECT lion_mcpd($$SELECT n, count(*) FROM lion_mcp GROUP BY n$$);
SELECT lion_mcpd($$SELECT m, count(m) FROM lion_mcp GROUP BY m$$);
-- the group key really is that column's own keys
SELECT a, count(*) FROM lion_mcp GROUP BY a ORDER BY a;
SELECT n, count(*) FROM lion_mcp GROUP BY n ORDER BY n NULLS LAST;

/*
 * The entries of ONE key column come out of the directory in that column's
 * key order (§21, §24), so a GROUP BY on it needs no Sort above the node -
 * exactly as over a single-column index.  `a` is NOT NULL, which is what lets
 * the pathkeys be NULLS LAST like the ORDER BY.
 */
SELECT lion_mcpd($$SELECT a, count(*) FROM lion_mcp GROUP BY a ORDER BY a$$);
EXPLAIN (COSTS OFF) SELECT a, count(*) FROM lion_mcp GROUP BY a ORDER BY a;

SELECT lion_mcsame($$SELECT a, count(*) FROM @ GROUP BY a$$);
SELECT lion_mcsame($$SELECT b, count(*) FROM @ GROUP BY b$$);
SELECT lion_mcsame($$SELECT c, count(*) FROM @ GROUP BY c$$);
SELECT lion_mcsame($$SELECT n, count(*) FROM @ GROUP BY n$$);

-- ---- 11.3 several columns of ONE index -----------------------------------
--
-- Three clauses on three columns are three sources located through one
-- relcache entry, ANDed exactly as three indexes would be.

SELECT lion_mcpd($$SELECT count(*) FROM lion_mcp WHERE a = 3 AND b = 2$$);
SELECT lion_mcpd($$SELECT count(*) FROM lion_mcp
				   WHERE a = 3 AND b = 2 AND c = 'c1'$$);
SELECT lion_mcpd($$SELECT count(*) FROM lion_mcp
				   WHERE a = 3 AND b = 2 AND c = 'c1' AND n = 4 AND m = 1$$);
-- the column order in the query is irrelevant
SELECT lion_mcpd($$SELECT count(*) FROM lion_mcp
				   WHERE m = 1 AND c = 'c1' AND a = 3$$);
SELECT lion_mcpd($$SELECT a, count(*) FROM lion_mcp WHERE b = 2 GROUP BY a$$);
SELECT lion_mcpd($$SELECT a, count(*) FROM lion_mcp
				   WHERE b = 2 AND c = 'c1' GROUP BY a$$);
-- a column the WHERE clause pins, printed from the key the entry stored
SELECT lion_mcpd($$SELECT c, count(*) FROM lion_mcp WHERE c = 'c2' GROUP BY c$$);
SELECT lion_mcpd($$SELECT a, c, count(*) FROM lion_mcp
				   WHERE a = 3 AND c = 'c3' GROUP BY a, c$$);
SELECT count(*) FROM lion_mcp WHERE a = 3 AND b = 2 AND c = 'c1';

SELECT lion_mcsame($$SELECT count(*) FROM @ WHERE a = 3 AND b = 2$$);
SELECT lion_mcsame($$SELECT count(*) FROM @ WHERE a = 3 AND b = 2 AND c = 'c1'$$);
SELECT lion_mcsame($$SELECT a, count(*) FROM @ WHERE b = 2 GROUP BY a$$);
/*
 * The one the per-column correction of §24 decides, and the reason it is
 * there: five clauses over five columns.  Charged the whole relation each -
 * five directories and five times the container pages for the one index that
 * holds them all - the node is REFUSED here while the same query over five
 * single-column indexes is accepted, which is the bias this pin exists to
 * catch (measured by disabling the correction, 2026-09-22).
 */
SELECT lion_mcsame($$SELECT count(*) FROM @
					 WHERE a = 3 AND b = 2 AND c = 'c1' AND n = 4 AND m = 1$$);

-- ---- 11.4 a two-column GROUP BY out of ONE index (§20) --------------------
--
-- The natural use of a multicolumn index: both grouping columns come from the
-- same relation, the outer/inner nested loop opens it twice and each side
-- walks its OWN key column's entries.  `b` has fewer distinct values, so it is
-- the outer one and EXPLAIN names it first.

SELECT lion_mcpd($$SELECT a, b, count(*) FROM lion_mcp GROUP BY a, b$$);
SELECT lion_mcpd($$SELECT b, a, count(*) FROM lion_mcp GROUP BY b, a$$);
SELECT lion_mcpd($$SELECT a, b, count(*) FROM lion_mcp
				   WHERE c = 'c1' GROUP BY a, b$$);
-- a NULL group on either side is that column's own reserved entry (§14)
SELECT lion_mcpd($$SELECT a, n, count(*) FROM lion_mcp GROUP BY a, n$$);
SELECT lion_mcpd($$SELECT n, m, count(*) FROM lion_mcp GROUP BY n, m$$, true);
SELECT lion_mcpd($$SELECT a, n, count(n) FROM lion_mcp GROUP BY a, n$$);
SELECT a, b, count(*) FROM lion_mcp GROUP BY a, b ORDER BY a, b LIMIT 8;
SELECT n, m, count(*) FROM lion_mcp GROUP BY n, m
 ORDER BY n NULLS LAST, m NULLS LAST LIMIT 8;

SELECT lion_mcsame($$SELECT a, b, count(*) FROM @ GROUP BY a, b$$);
SELECT lion_mcsame($$SELECT a, n, count(*) FROM @ GROUP BY a, n$$);
-- and one the model refuses over either portfolio, for the same reason
SELECT lion_mcsame($$SELECT a, b, count(*) FROM @ WHERE c = 'c1' GROUP BY a, b$$);

-- ---- 11.5 IN lists (§15) --------------------------------------------------

SELECT lion_mcpd($$SELECT count(*) FROM lion_mcp WHERE a IN (1,3,5)$$);
SELECT lion_mcpd($$SELECT count(*) FROM lion_mcp WHERE c IN ('c1','c3')$$);
SELECT lion_mcpd($$SELECT count(*) FROM lion_mcp
				   WHERE a IN (1,3,5) AND b IN (2,4)$$);
SELECT lion_mcpd($$SELECT count(*) FROM lion_mcp
				   WHERE a IN (1,3,5) AND c = 'c1'$$);
/*
 * An IN list on the very column a GROUP BY drives IS the groups (§15).  On a
 * multicolumn index that is a question about the (index, column) pair and not
 * about the index: `b IN (...)` says nothing about the groups of `a`, though
 * both live in lion_mcp_i.
 */
SELECT lion_mcpd($$SELECT a, count(*) FROM lion_mcp
				   WHERE a IN (1,3,5) GROUP BY a$$);
SELECT lion_mcpd($$SELECT a, count(*) FROM lion_mcp
				   WHERE b IN (2,4) GROUP BY a$$);
SELECT a, count(*) FROM lion_mcp WHERE b IN (2,4) GROUP BY a ORDER BY a;

SELECT lion_mcsame($$SELECT count(*) FROM @ WHERE a IN (1,3,5)$$);
SELECT lion_mcsame($$SELECT a, count(*) FROM @ WHERE a IN (1,3,5) GROUP BY a$$);
SELECT lion_mcsame($$SELECT a, count(*) FROM @ WHERE b IN (2,4) GROUP BY a$$);

-- ---- 11.6 NULL keys (§14) -------------------------------------------------

SELECT lion_mcpd($$SELECT count(*) FROM lion_mcp WHERE n IS NULL$$);
SELECT lion_mcpd($$SELECT count(*) FROM lion_mcp WHERE m IS NULL$$);
SELECT lion_mcpd($$SELECT count(*) FROM lion_mcp WHERE n IS NULL AND m IS NULL$$);
SELECT lion_mcpd($$SELECT count(*) FROM lion_mcp WHERE n IS NULL AND a = 3$$);
SELECT lion_mcpd($$SELECT a, count(*) FROM lion_mcp WHERE n IS NULL GROUP BY a$$);
SELECT lion_mcpd($$SELECT n, count(n) FROM lion_mcp WHERE n IS NULL GROUP BY n$$);
/*
 * `IS NOT NULL` with nothing else sums every entry of ONE key column and
 * subtracts that column's NULL entry (§14).  With two such clauses over one
 * index the driver's own clause must be dropped and the OTHER column's NULL
 * set still subtracted - they are different sets in the same relation, which
 * before §24 the executor told apart by the index Oid alone.
 */
SELECT lion_mcpd($$SELECT count(*) FROM lion_mcp WHERE n IS NOT NULL$$, true);
SELECT lion_mcpd($$SELECT count(*) FROM lion_mcp WHERE m IS NOT NULL$$, true);
SELECT lion_mcpd($$SELECT count(*) FROM lion_mcp
				   WHERE n IS NOT NULL AND m IS NOT NULL$$, true);
SELECT lion_mcpd($$SELECT count(*) FROM lion_mcp
				   WHERE m IS NOT NULL AND n IS NOT NULL$$, true);
SELECT count(*) FROM lion_mcp WHERE n IS NOT NULL AND m IS NOT NULL;
SELECT lion_mcpd($$SELECT count(*) FROM lion_mcp
				   WHERE n IS NOT NULL AND a = 3$$);

-- ---- 11.7 OR across two columns of ONE index (§19) ------------------------
/*
 * The union of two of one index's key columns is ONE source.  It must not be
 * SUMMED: a row with a = 3 AND b = 2 is a member of both sets, and adding the
 * two counts would count it twice.  lion_sources_disjoint_sum() refuses sets
 * whose attno differs exactly for that reason (§24), and the counter EXPLAIN
 * ANALYZE prints says so - while a plain IN list on one column, whose entries
 * really are disjoint, still takes the short-circuit.
 */
SELECT lion_mcpd($$SELECT count(*) FROM lion_mcp WHERE a = 3 OR b = 2$$);
SELECT lion_mcpd($$SELECT count(*) FROM lion_mcp WHERE a = 3 OR c = 'c1'$$);
SELECT lion_mcpd($$SELECT count(*) FROM lion_mcp
				   WHERE (a = 3 AND b = 2) OR c = 'c1'$$);
SELECT lion_mcpd($$SELECT count(*) FROM lion_mcp WHERE n = 4 OR m IS NULL$$);
SELECT lion_mcpd($$SELECT a, count(*) FROM lion_mcp
				   WHERE b = 2 OR c = 'c1' GROUP BY a$$);

SELECT (SELECT count(*) FROM lion_mcp WHERE a = 3 OR b = 2)
	 < (SELECT count(*) FROM lion_mcp WHERE a = 3)
	 + (SELECT count(*) FROM lion_mcp WHERE b = 2) AS or_is_a_union_not_a_sum;
SELECT count(*) FROM lion_mcp WHERE a = 3 OR b = 2;

SELECT lion_mcsummed($$SELECT count(*) FROM lion_mcp WHERE a = 3 OR b = 2$$);
SELECT lion_mcsummed($$SELECT count(*) FROM lion_mcp WHERE a IN (1,3,5)$$);

SELECT lion_mcsame($$SELECT count(*) FROM @ WHERE a = 3 OR b = 2$$);

-- ---- 11.8 parameters (§10) ------------------------------------------------

SELECT lion_mcprep('SELECT count(*) FROM lion_mcp WHERE a = $1', '3');
SELECT lion_mcprep('SELECT count(*) FROM lion_mcp WHERE c = $1', $$'c1'$$);
SELECT lion_mcprep('SELECT count(*) FROM lion_mcp WHERE a = $1 AND c = $2',
				  $$3, 'c1'$$);
SELECT lion_mcprep('SELECT count(*) FROM lion_mcp WHERE m = $1', 'NULL::int');
SELECT lion_mcprep('SELECT count(*) FROM lion_mcp WHERE a = ANY ($1)',
				  'ARRAY[1,3]');
SELECT lion_mcprep('SELECT a, count(*) FROM lion_mcp WHERE b = $1 GROUP BY a',
				  '2');

-- ---- 11.9 a partitioned table whose partitions differ (§16) ---------------
/*
 * Every partition has a multicolumn lion index of its own and they do NOT
 * agree: one indexes (a, b) and the others (b, a), and the third numbers its
 * heap columns differently again because a column was dropped from it.  So
 * neither the key column nor the heap attnum the plan carries is the one that
 * partition uses, and the executor has to derive both from the index it
 * really opened.
 */
CREATE TABLE lion_mcpart (id int NOT NULL, a int NOT NULL, b int NOT NULL)
	PARTITION BY RANGE (id);
CREATE TABLE lion_mcpart1 PARTITION OF lion_mcpart
	FOR VALUES FROM (0) TO (40000);
CREATE TABLE lion_mcpart2 PARTITION OF lion_mcpart
	FOR VALUES FROM (40000) TO (80000);
CREATE TABLE lion_mcpart3 (junk text, id int NOT NULL, a int NOT NULL,
						   b int NOT NULL);
ALTER TABLE lion_mcpart3 DROP COLUMN junk;
ALTER TABLE lion_mcpart ATTACH PARTITION lion_mcpart3
	FOR VALUES FROM (80000) TO (120000);

INSERT INTO lion_mcpart SELECT i, i % 10, i % 7
  FROM generate_series(0, 119999) i;

CREATE INDEX ON lion_mcpart1 USING lion (a, b);
CREATE INDEX ON lion_mcpart2 USING lion (b, a);
CREATE INDEX ON lion_mcpart3 USING lion (b, a);
VACUUM (ANALYZE) lion_mcpart;

SELECT lion_mcpd($$SELECT count(*) FROM lion_mcpart WHERE a = 3$$);
SELECT lion_mcpd($$SELECT count(*) FROM lion_mcpart WHERE b = 2$$);
SELECT lion_mcpd($$SELECT count(*) FROM lion_mcpart WHERE a = 3 AND b = 2$$);
SELECT lion_mcpd($$SELECT a, count(*) FROM lion_mcpart GROUP BY a$$);
SELECT lion_mcpd($$SELECT b, count(*) FROM lion_mcpart WHERE a = 3 GROUP BY b$$);
SELECT lion_mcpd($$SELECT count(*) FROM lion_mcpart WHERE a = 3 OR b = 2$$);
SELECT lion_mcpd($$SELECT a, b, count(*) FROM lion_mcpart GROUP BY a, b$$, true);
SELECT a, count(*) FROM lion_mcpart GROUP BY a ORDER BY a;
SELECT count(*) FROM lion_mcpart WHERE a = 3 AND b = 2;

-- ---- 11.10 a heap that is not all-visible ---------------------------------
--
-- The recheck path over a multicolumn index: every TID whose heap block the
-- visibility map cannot vouch for is resolved against the snapshot.

DELETE FROM lion_mcp WHERE id % 37 = 0;
UPDATE lion_mcp SET n = NULL WHERE id % 53 = 0;
SELECT lion_mcpd($$SELECT count(*) FROM lion_mcp WHERE a = 3 AND b = 2$$);
SELECT lion_mcpd($$SELECT a, count(*) FROM lion_mcp GROUP BY a$$);
SELECT lion_mcpd($$SELECT count(*) FROM lion_mcp WHERE n IS NULL$$);
SELECT lion_mcpd($$SELECT count(*) FROM lion_mcp WHERE a = 3 OR b = 2$$);
VACUUM lion_mcp;
SELECT lion_mcpd($$SELECT count(*) FROM lion_mcp WHERE a = 3 AND b = 2$$);
SELECT lion_mcpd($$SELECT a, b, count(*) FROM lion_mcp GROUP BY a, b$$);
SELECT lion_index_verify('lion_mcp_i', true);

-- ---- 11.11 a multi-key column beside scalar ones (§17) --------------------
/*
 * `lion_mc_kta` is (k, t, a) with an int4[] in KEY COLUMN 3, and
 * `lion_mc_ts_i` is (g, tsv) with a tsvector in column 2.  A multi-key clause
 * re-extracts its query at run time with the KEY COLUMN's own opclass - that
 * column's extractQuery support function, and the strategy the operator has in
 * THAT column's opfamily - so this is the shape that would read another
 * column's opclass if the column were assumed to be the first.  Neither
 * fixture is shaped for the cost model to prefer the node, so the other plans
 * are switched off and the answers are still compared with the pushdown off.
 */
SELECT lion_mcpd($$SELECT count(*) FROM lion_mc WHERE a @> ARRAY[3]$$, true);
SELECT lion_mcpd($$SELECT count(*) FROM lion_mc WHERE a && ARRAY[3,9]$$, true);
SELECT lion_mcpd($$SELECT count(*) FROM lion_mc
				   WHERE k = 7 AND a @> ARRAY[3]$$, true);
SELECT lion_mcpd($$SELECT count(*) FROM lion_mc
				   WHERE t = 't47' AND a @> ARRAY[3]$$, true);
SELECT lion_mcpd($$SELECT k, count(*) FROM lion_mc
				   WHERE a @> ARRAY[3] GROUP BY k$$, true);
SELECT count(*) FROM lion_mc WHERE k = 7 AND a @> ARRAY[3];

SELECT lion_mcpd($$SELECT count(*) FROM lion_mc_ts
				   WHERE tsv @@ 'w3 & w5'::tsquery$$, true);
SELECT lion_mcpd($$SELECT count(*) FROM lion_mc_ts
				   WHERE g = 4 AND tsv @@ 'w3'::tsquery$$, true);
SELECT lion_mcpd($$SELECT g, count(*) FROM lion_mc_ts
				   WHERE tsv @@ 'w3'::tsquery GROUP BY g$$, true);
SELECT count(*) FROM lion_mc_ts WHERE g = 4 AND tsv @@ 'w3'::tsquery;

-- ---- 12. cleanup ----------------------------------------------------------

DROP TABLE lion_mcpart;
DROP TABLE lion_mcp;
DROP TABLE lion_mcs;
DROP FUNCTION lion_mcpd(text, boolean);
DROP FUNCTION lion_mcsame(text);
DROP FUNCTION lion_mcprep(text, text);
DROP FUNCTION lion_mcsummed(text);
DROP TABLE lion_mc_ts;
DROP TABLE lion_mc_empty;
DROP TABLE lion_mc;
DROP FUNCTION lion_mccmp(text);
