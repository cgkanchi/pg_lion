-- a multi-row insert, one statement
\set n random(2, 12)
INSERT INTO @T@ (c2, c20, c200, c20k, c1m, cn, tags, pad) SELECT * FROM soak.gen(:n);
