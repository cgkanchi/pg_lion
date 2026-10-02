-- a transaction of several statements that commits
\set id random(1, :maxid)
\set c20 random(0, 19)
BEGIN;
INSERT INTO @T@ (c2, c20, c200, c20k, c1m, cn, tags, pad) SELECT * FROM soak.gen(5);
UPDATE @T@ SET c20 = :c20 WHERE id = :id;
DELETE FROM @T@ WHERE id = :id + 7;
UPDATE @T@ SET pad = 'z' WHERE id = :id + 3;
COMMIT;
