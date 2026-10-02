-- speculative inserts that race: a narrow band of ids far above the
-- sequence's, so that two writers often insert the same id at once and one
-- of the two heap tuples is killed
\set id random(2000000000, 2000000040)
INSERT INTO @T@ (id, c2, c20, c200, c20k, c1m, cn, tags, pad)
	SELECT :id, * FROM soak.gen(1) ON CONFLICT (id) DO NOTHING;
DELETE FROM @T@ WHERE id = :id AND random() < 0.5;
