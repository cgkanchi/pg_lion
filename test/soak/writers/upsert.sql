-- INSERT ... ON CONFLICT: an update of an existing row, or a speculative
-- insert of a missing one (killed when two writers race to the same id).
-- Only ids of the initial load, which the sequence has passed.
\set id random(1, :loadmax)
INSERT INTO @T@ (id, c2, c20, c200, c20k, c1m, cn, tags, pad)
	SELECT :id, * FROM soak.gen(1)
	ON CONFLICT (id) DO UPDATE SET c20 = EXCLUDED.c20, c20k = EXCLUDED.c20k;
