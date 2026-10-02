-- the abort phase, by ROLLBACK TO SAVEPOINT in a transaction that commits
-- nothing else
\set n random(1, 30)
BEGIN;
SAVEPOINT a;
INSERT INTO @T@ (c2, c20, c200, c20k, c1m, cn, tags, pad) SELECT * FROM soak.gen(:n);
ROLLBACK TO SAVEPOINT a;
COMMIT;
