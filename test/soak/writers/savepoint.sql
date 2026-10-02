-- a subtransaction rolled back inside a transaction that commits
\set id random(1, :maxid)
BEGIN;
INSERT INTO @T@ (c2, c20, c200, c20k, c1m, cn, tags, pad) SELECT * FROM soak.gen(1);
SAVEPOINT a;
INSERT INTO @T@ (c2, c20, c200, c20k, c1m, cn, tags, pad) SELECT * FROM soak.gen(6);
UPDATE @T@ SET c200 = (c200 + 1) % 200 WHERE id BETWEEN :id AND :id + 4;
ROLLBACK TO SAVEPOINT a;
UPDATE @T@ SET pad = 'w' WHERE id = :id;
COMMIT;
