-- one row
INSERT INTO @T@ (c2, c20, c200, c20k, c1m, cn, tags, pad) SELECT * FROM soak.gen(1);
