-- the abort phase: writes that all roll back, so the heap's pages lose their
-- all-visible mark only until VACUUM prunes the dead rows and marks them again
\set id random(1, :maxid)
\set n random(1, 30)
BEGIN;
INSERT INTO @T@ (c2, c20, c200, c20k, c1m, cn, tags, pad) SELECT * FROM soak.gen(:n);
UPDATE @T@ SET c1m = c1m + 1, c20k = c20k + 1 WHERE id BETWEEN :id AND :id + 3;
ROLLBACK;
