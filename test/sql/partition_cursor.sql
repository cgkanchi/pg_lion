-- Same-session DDL under an open cursor over a partitioned count (DESIGN.md
-- section 16, "Executor").
--
-- Core refuses a TRUNCATE, DROP, REINDEX or ALTER of a relation a query of
-- the same session still uses - a cursor between two FETCHes, say - and it
-- knows which ones are in use from the relcache references the query's scans
-- hold from ExecInitNode() to ExecEndNode() (CheckTableNotInUse()).  The
-- count node over a partitioned table used to open each leaf partition only
-- for its own turn, so all of these went through: a TRUNCATE of a leaf made
-- the cursor's later counts drop, and a DROP made its next FETCH fail with
-- "could not open relation with OID".  The node now opens every leaf's heap
-- and indexes when it is initialised and holds them until it ends, and each
-- statement below must be refused with the error core gives with the pushdown
-- off.  Each one runs in a savepoint, so it errors and is rolled back, and the
-- cursor must go on to count what it would have counted.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
CREATE EXTENSION IF NOT EXISTS pg_lion;

/*
 * EXPLAIN in a form every supported release prints the same way (see
 * pushdown.sql): subplans are named "expr_N" as in PostgreSQL 19 ("N"
 * before), and 18's "Disabled: true" lines are left out.
 */
CREATE FUNCTION lion_pc_explain(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		CONTINUE WHEN ln ~ '^\s*Disabled: true$';
		RETURN NEXT regexp_replace(ln, '(InitPlan|SubPlan) (\d+)', '\1 expr_\2', 'g');
	END LOOP;
END $$;

/* Whether a query's plan has the count node. */
CREATE FUNCTION lion_pc_pushed(q text) RETURNS boolean
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

/*
 * Three leaves of 1000 rows, g = 0 .. 4, each with a lion index of its own
 * rather than one of a partitioned index, which a DROP INDEX can name.
 */
CREATE TABLE lion_pc (k int, g int) PARTITION BY RANGE (k);
CREATE TABLE lion_pc1 PARTITION OF lion_pc FOR VALUES FROM (0) TO (1000);
CREATE TABLE lion_pc2 PARTITION OF lion_pc FOR VALUES FROM (1000) TO (2000);
CREATE TABLE lion_pc3 PARTITION OF lion_pc FOR VALUES FROM (2000) TO (3000);
INSERT INTO lion_pc SELECT i, i % 5 FROM generate_series(0, 2999) i;
CREATE INDEX lion_pc1_g ON lion_pc1 USING lion (g);
CREATE INDEX lion_pc2_g ON lion_pc2 USING lion (g);
CREATE INDEX lion_pc3_g ON lion_pc3 USING lion (g);
VACUUM (FREEZE, ANALYZE) lion_pc;
SET enable_seqscan = off;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SET pg_lion.enable_ordered_scan = off;

-- ---- 1. a correlated count, rescanned for each x ---------------------------------
SELECT * FROM lion_pc_explain('SELECT x, (SELECT count(*) FROM lion_pc WHERE g = v.x) FROM generate_series(0, 4) v(x)');
BEGIN;
DECLARE c CURSOR FOR
SELECT x, (SELECT count(*) FROM lion_pc WHERE g = v.x) FROM generate_series(0, 4) v(x);
FETCH 1 FROM c;
-- a leaf emptied, or dropped (its index goes first)
SAVEPOINT s;
TRUNCATE lion_pc3;
FETCH 1 FROM c;
ROLLBACK TO s;
SAVEPOINT s;
DROP TABLE lion_pc2;
FETCH 1 FROM c;
ROLLBACK TO s;
-- a leaf's index dropped, rebuilt or altered
SAVEPOINT s;
DROP INDEX lion_pc3_g;
ROLLBACK TO s;
SAVEPOINT s;
REINDEX INDEX lion_pc3_g;
ROLLBACK TO s;
SAVEPOINT s;
REINDEX TABLE lion_pc1;
ROLLBACK TO s;
SAVEPOINT s;
ALTER INDEX lion_pc3_g SET (fillfactor = 50);
ROLLBACK TO s;
-- a leaf altered, or given another index
SAVEPOINT s;
ALTER TABLE lion_pc3 ALTER COLUMN g SET STATISTICS 50;
ROLLBACK TO s;
SAVEPOINT s;
CREATE INDEX ON lion_pc3 (k);
ROLLBACK TO s;
/*
 * DETACH PARTITION is an ALTER TABLE of the parent, which no scan of the
 * leaves has open, so core accepts it as well.  The cursor goes on counting
 * the detached table, which it has open; and a DROP of it is refused.
 */
SAVEPOINT s;
ALTER TABLE lion_pc DETACH PARTITION lion_pc3;
FETCH 1 FROM c;
DROP TABLE lion_pc3;
ROLLBACK TO s;
FETCH ALL FROM c;
COMMIT;

-- ... and the same with the pushdown off, through the same lion indexes
SET pg_lion.enable_count_pushdown = off;
SET enable_bitmapscan = on;
SELECT * FROM lion_pc_explain('SELECT x, (SELECT count(*) FROM lion_pc WHERE g = v.x) FROM generate_series(0, 4) v(x)');
BEGIN;
DECLARE c CURSOR FOR
SELECT x, (SELECT count(*) FROM lion_pc WHERE g = v.x) FROM generate_series(0, 4) v(x);
FETCH 1 FROM c;
SAVEPOINT s;
TRUNCATE lion_pc3;
FETCH 1 FROM c;
ROLLBACK TO s;
SAVEPOINT s;
DROP TABLE lion_pc2;
FETCH 1 FROM c;
ROLLBACK TO s;
SAVEPOINT s;
DROP INDEX lion_pc3_g;
ROLLBACK TO s;
SAVEPOINT s;
REINDEX INDEX lion_pc3_g;
ROLLBACK TO s;
SAVEPOINT s;
REINDEX TABLE lion_pc1;
ROLLBACK TO s;
SAVEPOINT s;
ALTER INDEX lion_pc3_g SET (fillfactor = 50);
ROLLBACK TO s;
SAVEPOINT s;
ALTER TABLE lion_pc3 ALTER COLUMN g SET STATISTICS 50;
ROLLBACK TO s;
SAVEPOINT s;
CREATE INDEX ON lion_pc3 (k);
ROLLBACK TO s;
SAVEPOINT s;
ALTER TABLE lion_pc DETACH PARTITION lion_pc3;
FETCH 1 FROM c;
DROP TABLE lion_pc3;
ROLLBACK TO s;
FETCH ALL FROM c;
COMMIT;
RESET pg_lion.enable_count_pushdown;
SET enable_bitmapscan = off;

-- ---- 2. a GROUP BY: every partition counted by the first FETCH ------------------
/*
 * The Finalize HashAggregate reads all of the node's partial rows before it
 * returns its first, so the node has counted every partition and is waiting
 * for its end: its relations are still in use until then, as a finished
 * scan's are.
 */
SELECT * FROM lion_pc_explain('SELECT g, count(*) FROM lion_pc GROUP BY g ORDER BY g');
BEGIN;
DECLARE c CURSOR FOR SELECT g, count(*) FROM lion_pc GROUP BY g ORDER BY g;
FETCH 1 FROM c;
SAVEPOINT s;
TRUNCATE lion_pc1;
ROLLBACK TO s;
SAVEPOINT s;
REINDEX INDEX lion_pc2_g;
ROLLBACK TO s;
FETCH ALL FROM c;
COMMIT;

-- ---- 3. the FK-side join -------------------------------------------------------
/*
 * Over a partitioned fact table (DESIGN.md section 27, "A partitioned fact
 * table") each batch of keys is taken to every leaf in turn, and the leaves'
 * relations are held from one turn to the next.  And grouped by a fact
 * column ("Grouped by a fact column"), the index of the column's groups is
 * read at every turn - over a plain table too - and held for the life of
 * the node with the others.
 */
SET pg_lion.enable_semijoin = off;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
SET enable_seqscan = on;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE lion_pcd (pk int8 PRIMARY KEY, region text NOT NULL);
INSERT INTO lion_pcd
SELECT i, (ARRAY['eu', 'us', 'ap'])[1 + i % 3] FROM generate_series(1, 300) i;
CREATE TABLE lion_pcf (id int, fk int8, x int, lvl int) PARTITION BY RANGE (id);
CREATE TABLE lion_pcf1 PARTITION OF lion_pcf FOR VALUES FROM (0) TO (3000);
CREATE TABLE lion_pcf2 PARTITION OF lion_pcf FOR VALUES FROM (3000) TO (6000);
INSERT INTO lion_pcf
SELECT i, 1 + i % 300, i % 4, i % 7 FROM generate_series(0, 5999) i;
CREATE INDEX lion_pcf_fk ON lion_pcf USING lion (fk);
CREATE INDEX lion_pcf_x ON lion_pcf USING lion (x);
CREATE TABLE lion_pcg (LIKE lion_pcf);
INSERT INTO lion_pcg SELECT * FROM lion_pcf;
CREATE INDEX lion_pcg_fk ON lion_pcg USING lion (fk);
CREATE INDEX lion_pcg_x ON lion_pcg USING lion (x);
CREATE INDEX lion_pcg_lvl ON lion_pcg USING lion (lvl);
VACUUM (FREEZE, ANALYZE) lion_pcd;
VACUUM (FREEZE, ANALYZE) lion_pcf;
VACUUM (FREEZE, ANALYZE) lion_pcg;

SELECT lion_pc_pushed('SELECT v.x, (SELECT count(*) FROM lion_pcf f JOIN lion_pcd d ON f.fk = d.pk WHERE d.region = ''us'' AND f.x = v.x) FROM generate_series(0, 3) v(x)') AS pushed;
BEGIN;
DECLARE c CURSOR FOR
SELECT v.x, (SELECT count(*) FROM lion_pcf f JOIN lion_pcd d ON f.fk = d.pk
			 WHERE d.region = 'us' AND f.x = v.x)
  FROM generate_series(0, 3) v(x);
FETCH 1 FROM c;
SAVEPOINT s;
TRUNCATE lion_pcf2;
FETCH 1 FROM c;
ROLLBACK TO s;
SAVEPOINT s;
REINDEX INDEX lion_pcf2_fk_idx;
ROLLBACK TO s;
FETCH ALL FROM c;
COMMIT;

SELECT lion_pc_pushed('SELECT v.x, (SELECT string_agg(l || '':'' || c, '','' ORDER BY l) FROM (SELECT f.lvl AS l, count(*) AS c FROM lion_pcg f JOIN lion_pcd d ON f.fk = d.pk WHERE d.region = ''us'' AND f.x = v.x GROUP BY f.lvl) s) FROM generate_series(0, 3) v(x)') AS pushed;
BEGIN;
DECLARE c CURSOR FOR
SELECT v.x, (SELECT string_agg(l || ':' || c, ',' ORDER BY l)
			   FROM (SELECT f.lvl AS l, count(*) AS c
					   FROM lion_pcg f JOIN lion_pcd d ON f.fk = d.pk
					  WHERE d.region = 'us' AND f.x = v.x GROUP BY f.lvl) s)
  FROM generate_series(0, 3) v(x);
FETCH 1 FROM c;
SAVEPOINT s;
DROP INDEX lion_pcg_lvl;
FETCH 1 FROM c;
ROLLBACK TO s;
SAVEPOINT s;
REINDEX INDEX lion_pcg_lvl;
ROLLBACK TO s;
FETCH ALL FROM c;
COMMIT;

DROP TABLE lion_pc, lion_pcd, lion_pcf, lion_pcg;
DROP FUNCTION lion_pc_explain(text);
DROP FUNCTION lion_pc_pushed(text);
