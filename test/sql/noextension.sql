--
-- A database without the extension, in a server that has the library loaded.
--
-- With pg_lion in shared_preload_libraries (make installcheck-rmgr) its
-- planner hooks run in every database of the cluster, including those where
-- CREATE EXTENSION pg_lion never ran and the lion access method does not
-- exist.  Aggregates there must plan as usual: pg_upgrade's own checks run
-- count(*) in template1 and postgres, and failed with "access method "lion"
-- does not exist" before the hooks learned to find no lion index instead.
-- In generic mode (no preload) the library is not loaded in the new
-- backend and this passes trivially.
--
CREATE DATABASE lion_regress_noext;
\set regressdb :DBNAME
\c lion_regress_noext
SELECT count(*) > 0 AS some_rows FROM pg_class;
SELECT relkind, count(*) > 0 AS some_rows FROM pg_class
 WHERE relkind = 'r' GROUP BY relkind;
SELECT count(*) AS no_extension FROM pg_extension WHERE extname = 'pg_lion';
\c :regressdb
DROP DATABASE lion_regress_noext;
