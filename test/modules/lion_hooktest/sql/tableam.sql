-- A table access method that is not the heap (DESIGN.md sections 2 and 23).
--
-- lion_heapcopy's handler returns a COPY of the heap's routine: its tables
-- hold heap tuples, but to pg_lion it is another table AM, exactly as
-- citus_columnar or a compressed TimescaleDB chunk would be.  Every way a
-- lion index could come to sit on such a table goes through ambuild, which
-- refuses; the count paths refuse too, which is checked by moving a table
-- that already has an index to the other AM behind the catalog's back.
--
-- Core's own heap code refuses a copy of the heap routine in places
-- (heap_getnext(): "only heap AM is supported"), which is what any index
-- build on such a table used to fail with; ambuild now refuses first, with
-- its own error.  Only lion indexes are built on lion_heapcopy tables here.
\set VERBOSITY terse
SET client_min_messages = warning;
SET synchronous_commit = on;
CREATE EXTENSION IF NOT EXISTS pg_lion;
CREATE EXTENSION IF NOT EXISTS lion_hooktest;

CREATE FUNCTION tam_pushed(q text) RETURNS boolean
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			RETURN true;
		END IF;
	END LOOP;
	RETURN false;
END $$;

-- ---- CREATE INDEX, on an empty table and on one with rows ------------------
CREATE TABLE tam_copy (k int, v int) USING lion_heapcopy;
\set VERBOSITY default
CREATE INDEX tam_copy_k ON tam_copy USING lion (k);
\set VERBOSITY terse
INSERT INTO tam_copy SELECT i % 10, i FROM generate_series(1, 5000) i;
CREATE INDEX tam_copy_k ON tam_copy USING lion (k);
CREATE INDEX tam_copy_kv ON tam_copy USING lion (k, v);
SELECT count(*) AS lion_indexes
  FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
  JOIN pg_am a ON a.oid = c.relam
 WHERE i.indrelid = 'tam_copy'::regclass AND a.amname = 'lion';

-- ---- ALTER TABLE ... SET ACCESS METHOD: the rewrite rebuilds the index -----
CREATE TABLE tam_heap (k int, v int);
INSERT INTO tam_heap SELECT i % 10, i FROM generate_series(1, 5000) i;
CREATE INDEX tam_heap_k ON tam_heap USING lion (k);
ALTER TABLE tam_heap SET ACCESS METHOD lion_heapcopy;
SELECT a.amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam
 WHERE c.oid = 'tam_heap'::regclass;
SELECT lion_index_count('tam_heap_k', 3) AS sql_count,
	   (SELECT count(*) FROM tam_heap WHERE k = 3) AS actual;

-- and back to the heap, after which the index builds
ALTER TABLE tam_copy SET ACCESS METHOD heap;
CREATE INDEX tam_copy_k ON tam_copy USING lion (k);
SELECT lion_index_count('tam_copy_k', 3) AS sql_count,
	   (SELECT count(*) FROM tam_copy WHERE k = 3) AS actual;

-- ---- partitions: each has its own table AM ---------------------------------
CREATE TABLE tam_p (k int, v int) PARTITION BY RANGE (v);
CREATE TABLE tam_p1 PARTITION OF tam_p FOR VALUES FROM (0) TO (1000);
CREATE TABLE tam_p2 PARTITION OF tam_p FOR VALUES FROM (1000) TO (2000)
	USING lion_heapcopy;
CREATE INDEX tam_p_k ON tam_p USING lion (k);			-- tam_p2 refuses
DROP TABLE tam_p2;
CREATE INDEX tam_p_k ON tam_p USING lion (k);
CREATE TABLE tam_p2 PARTITION OF tam_p FOR VALUES FROM (1000) TO (2000)
	USING lion_heapcopy;								-- the new partition's copy refuses
CREATE TABLE tam_p3 (k int, v int) USING lion_heapcopy;
ALTER TABLE tam_p ATTACH PARTITION tam_p3 FOR VALUES FROM (2000) TO (3000);
-- an index on the partition itself is refused as well, so nothing can be
-- attached to the parent's index either
CREATE INDEX tam_p3_k ON tam_p3 USING lion (k);
SELECT c.relname FROM pg_inherits i JOIN pg_class c ON c.oid = i.inhrelid
 WHERE i.inhparent = 'tam_p'::regclass ORDER BY 1;

-- ---- the count paths, for an index ambuild never saw on this AM ------------
CREATE TABLE tam_hack (k int, g int, v int);
INSERT INTO tam_hack SELECT i % 10, i % 3, i FROM generate_series(1, 20000) i;
CREATE INDEX tam_hack_k ON tam_hack USING lion (k);
CREATE INDEX tam_hack_g ON tam_hack USING lion (g);
VACUUM ANALYZE tam_hack;
SELECT tam_pushed('SELECT count(*) FROM tam_hack WHERE k = 3') AS pushed_on_heap;

UPDATE pg_class SET relam = (SELECT oid FROM pg_am WHERE amname = 'lion_heapcopy')
 WHERE oid = 'tam_hack'::regclass;
SELECT tam_pushed('SELECT count(*) FROM tam_hack WHERE k = 3') AS pushed;
SELECT tam_pushed('SELECT g, count(*) FROM tam_hack WHERE k = 3 GROUP BY g') AS pushed;
SELECT tam_pushed('SELECT k, count(*) FROM tam_hack GROUP BY k') AS pushed;
SELECT count(*) FROM tam_hack WHERE k = 3;
SELECT g, count(*) FROM tam_hack WHERE k = 3 GROUP BY g ORDER BY g;
SELECT lion_index_count('tam_hack_k', 3);
SELECT lion_index_count('tam_hack_k', 3, 'tam_hack_g', 1);
SELECT lion_index_count_any('tam_hack_k', ARRAY[1, 2]);
SELECT * FROM lion_index_count_stats('tam_hack_k', 3);
SELECT * FROM lion_index_count_group_stats('tam_hack_g');
UPDATE pg_class SET relam = (SELECT oid FROM pg_am WHERE amname = 'heap')
 WHERE oid = 'tam_hack'::regclass;
SELECT tam_pushed('SELECT count(*) FROM tam_hack WHERE k = 3') AS pushed_on_heap;
SELECT lion_index_count('tam_hack_k', 3);

-- a partitioned parent with one such leaf is declined as a whole
INSERT INTO tam_p SELECT i % 10, i FROM generate_series(0, 999) i;
CREATE TABLE tam_p4 PARTITION OF tam_p FOR VALUES FROM (3000) TO (4000);
INSERT INTO tam_p SELECT i % 10, i FROM generate_series(3000, 3999) i;
VACUUM ANALYZE tam_p;
SELECT tam_pushed('SELECT count(*) FROM tam_p WHERE k = 3') AS pushed_on_heap;
UPDATE pg_class SET relam = (SELECT oid FROM pg_am WHERE amname = 'lion_heapcopy')
 WHERE oid = 'tam_p4'::regclass;
SELECT tam_pushed('SELECT count(*) FROM tam_p WHERE k = 3') AS pushed;
SELECT tam_pushed('SELECT k, count(*) FROM tam_p GROUP BY k') AS pushed;
SELECT count(*) FROM tam_p WHERE k = 3;
UPDATE pg_class SET relam = (SELECT oid FROM pg_am WHERE amname = 'heap')
 WHERE oid = 'tam_p4'::regclass;
SELECT tam_pushed('SELECT count(*) FROM tam_p WHERE k = 3') AS pushed_on_heap;

DROP TABLE tam_copy, tam_heap, tam_p, tam_p3, tam_hack;
DROP FUNCTION tam_pushed(text);
