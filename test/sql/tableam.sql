-- Table access methods (DESIGN.md sections 2 and 23).
--
-- A lion index may only sit on a heap table: its TIDs are encoded with the
-- heap's offset width, and the count reads the heap's visibility map.  The
-- test is the table AM's ROUTINE, not its name, so a table AM created with
-- core's own heap handler is the heap and everything works on it - which is
-- what this file checks, together with the two ways a table's AM changes
-- under an existing index: ALTER TABLE ... SET ACCESS METHOD, and partitions
-- of one parent with different AMs.
--
-- The refusals themselves need a table AM that is not the heap, and the
-- installation has none; test/modules/lion_hooktest provides one and its
-- tableam test covers them (make hookcheck).
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';

-- Flushed commits, so that VACUUM can set the visibility map (pushdown.sql).
SET synchronous_commit = on;

CREATE EXTENSION IF NOT EXISTS pg_lion;

CREATE ACCESS METHOD lion_tam_heap2 TYPE TABLE HANDLER heap_tableam_handler;

/*
 * Whether the count was pushed down, and the number of rows, after proving
 * the pushed-down answer equal to the plain one as a multiset.
 */
CREATE FUNCTION lion_tam_pp(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	pushed boolean := false;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			pushed := true;
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lion_tam_on AS %s', q);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE format('CREATE TEMP TABLE lion_tam_off AS %s', q);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM lion_tam_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_tam_on EXCEPT ALL SELECT * FROM lion_tam_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_tam_off EXCEPT ALL SELECT * FROM lion_tam_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lion_tam_on, lion_tam_off';

	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows',
				  CASE WHEN pushed THEN 'pushed down' ELSE 'not pushed down' END,
				  nrows);
END $$;

-- ---- the heap under another name -----------------------------------------
CREATE TABLE lion_tam_h2 (k int, g int, v int) USING lion_tam_heap2;
INSERT INTO lion_tam_h2 SELECT i % 50, i % 7, i FROM generate_series(1, 20000) i;
CREATE INDEX lion_tam_h2_k ON lion_tam_h2 USING lion (k);
CREATE INDEX lion_tam_h2_g ON lion_tam_h2 USING lion (g);
VACUUM ANALYZE lion_tam_h2;

SELECT lion_tam_pp('SELECT count(*) FROM lion_tam_h2 WHERE k = 3');
SELECT lion_tam_pp('SELECT g, count(*) FROM lion_tam_h2 WHERE k = 3 GROUP BY g');
SELECT lion_tam_pp('SELECT k, count(*) FROM lion_tam_h2 GROUP BY k');
SELECT lion_index_count('lion_tam_h2_k', 3) AS sql_count,
	   (SELECT count(*) FROM lion_tam_h2 WHERE k = 3) AS actual;

-- dirty pages: the recheck goes through the heap AM's per-block path
DELETE FROM lion_tam_h2 WHERE v % 3 = 0;
SELECT lion_tam_pp('SELECT count(*) FROM lion_tam_h2 WHERE k = 3');
SELECT lion_index_count('lion_tam_h2_k', 3) AS sql_count,
	   (SELECT count(*) FROM lion_tam_h2 WHERE k = 3) AS actual;
VACUUM lion_tam_h2;
SELECT lion_tam_pp('SELECT g, count(*) FROM lion_tam_h2 WHERE k = 3 GROUP BY g');
SELECT lion_index_verify('lion_tam_h2_k', true);

-- ---- ALTER TABLE ... SET ACCESS METHOD rewrites and rebuilds -------------
CREATE TABLE lion_tam_alt (k int, v int);
INSERT INTO lion_tam_alt SELECT i % 20, i FROM generate_series(1, 10000) i;
CREATE INDEX lion_tam_alt_k ON lion_tam_alt USING lion (k);
CREATE TEMP TABLE lion_tam_fn AS
	SELECT pg_relation_filenode('lion_tam_alt_k') AS before;

ALTER TABLE lion_tam_alt SET ACCESS METHOD lion_tam_heap2;
SELECT a.amname, pg_relation_filenode('lion_tam_alt_k') <> f.before AS index_rebuilt
  FROM pg_class c JOIN pg_am a ON a.oid = c.relam, lion_tam_fn f
 WHERE c.oid = 'lion_tam_alt'::regclass;
SELECT lion_index_verify('lion_tam_alt_k', true);
VACUUM lion_tam_alt;
SELECT lion_tam_pp('SELECT count(*) FROM lion_tam_alt WHERE k = 7');

ALTER TABLE lion_tam_alt SET ACCESS METHOD heap;
SELECT a.amname
  FROM pg_class c JOIN pg_am a ON a.oid = c.relam
 WHERE c.oid = 'lion_tam_alt'::regclass;
SELECT lion_index_verify('lion_tam_alt_k', true);
VACUUM lion_tam_alt;
SELECT lion_tam_pp('SELECT k, count(*) FROM lion_tam_alt GROUP BY k');

-- ---- partitions with different table AMs ----------------------------------
CREATE TABLE lion_tam_p (k int, v int) PARTITION BY RANGE (v);
CREATE TABLE lion_tam_p1 PARTITION OF lion_tam_p FOR VALUES FROM (0) TO (10000);
CREATE TABLE lion_tam_p2 PARTITION OF lion_tam_p FOR VALUES FROM (10000) TO (20000)
	USING lion_tam_heap2;
INSERT INTO lion_tam_p SELECT i % 30, i FROM generate_series(0, 19999) i;
CREATE INDEX lion_tam_p_k ON lion_tam_p USING lion (k);
-- attached later, of the other AM: its copy of the index is built on attach
CREATE TABLE lion_tam_p3 (k int, v int) USING lion_tam_heap2;
INSERT INTO lion_tam_p3 SELECT i % 30, i FROM generate_series(20000, 24999) i;
ALTER TABLE lion_tam_p ATTACH PARTITION lion_tam_p3 FOR VALUES FROM (20000) TO (30000);
VACUUM ANALYZE lion_tam_p;

SELECT c.relname, a.amname
  FROM pg_inherits i JOIN pg_class c ON c.oid = i.inhrelid
  JOIN pg_am a ON a.oid = c.relam
 WHERE i.inhparent = 'lion_tam_p'::regclass
 ORDER BY 1;
SELECT lion_tam_pp('SELECT count(*) FROM lion_tam_p WHERE k = 4');
SELECT lion_tam_pp('SELECT k, count(*) FROM lion_tam_p GROUP BY k');

-- ---- cleanup ----------------------------------------------------------------
DROP TABLE lion_tam_h2, lion_tam_alt, lion_tam_p, lion_tam_fn;
DROP FUNCTION lion_tam_pp(text);
DROP ACCESS METHOD lion_tam_heap2;
