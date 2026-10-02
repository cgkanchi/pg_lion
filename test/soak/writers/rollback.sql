-- a transaction that rolls back: its rows are dead to every snapshot
\set id random(1, :maxid)
BEGIN;
INSERT INTO @T@ (c2, c20, c200, c20k, c1m, cn, tags, pad) SELECT * FROM soak.gen(8);
UPDATE @T@ SET c20 = (c20 + 1) % 20, c1m = c1m + 1 WHERE id BETWEEN :id AND :id + 5;
DELETE FROM @T@ WHERE id = :id + 9;
ROLLBACK;
