# Two SERIALIZABLE transactions each read through LionOrdered in its
# index-only mode (DESIGN.md §40) - the first rows, in btree order, of the rows
# lion says have c = 1 (or c = 2), their values from the covering btree and the
# heap not read - and then each updates, HOT, a row the OTHER one returned.  A
# row returned without a heap visit has no tuple to predicate-lock (its xmin
# was never read), so the node locks its heap page as core's Index Only Scan
# does, and this write skew must end in a serialization failure, as it does
# for the ordinary plan.  Without the page lock neither update would conflict
# with anything the other read - a HOT update writes no index - and both would
# commit.  The table is vacuumed first, so that its pages are all-visible and
# the heap really is not read - s1_fetches shows the mode and `Heap Fetches:
# 0`, so that the conflict can only be the page lock's; the fillfactor keeps
# the updates HOT; the synchronous commit lets that VACUUM, the first after
# the rows' insertion, set the hint bits the all-visible flag needs.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE TABLE bss (id int PRIMARY KEY, k int, c int, note text)
		WITH (fillfactor = 70);
	INSERT INTO bss SELECT i, (i * 37) % 1009, i % 10, 'n' FROM generate_series(1, 1000) i;
	CREATE INDEX bss_k ON bss (k, id) INCLUDE (c);
	CREATE INDEX bss_c ON bss USING lion (c);
	ANALYZE bss;
	CREATE FUNCTION bss_fetches(q text) RETURNS text LANGUAGE plpgsql AS $$
	DECLARE
		ln text;
		r text := 'no node';
	BEGIN
		FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
			IF ln ~ 'Ordered By:' THEN r := btrim(ln); END IF;
			IF ln ~ 'Heap Fetches' THEN r := r || '; ' || btrim(ln); END IF;
		END LOOP;
		RETURN r;
	END $$;
}
teardown
{
	DROP TABLE bss;
	DROP FUNCTION bss_fetches(text);
}

session s0
step s0_vacuum	{ VACUUM bss; }

session s1
setup
{
	SET enable_seqscan = off; SET enable_bitmapscan = off;
	SET enable_indexscan = off;
}
step s1_begin	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1_plan	{ EXPLAIN (COSTS OFF) SELECT id, k FROM bss WHERE c = 1 ORDER BY k, id LIMIT 3; }
step s1_fetches	{ SELECT bss_fetches('SELECT id, k FROM bss WHERE c = 1 ORDER BY k, id LIMIT 3'); }
step s1_read	{ SELECT id, k FROM bss WHERE c = 1 ORDER BY k, id LIMIT 3; }
step s1_update	{ UPDATE bss SET note = 's1' WHERE id = 82; }
step s1_commit	{ COMMIT; }

session s2
setup
{
	SET enable_seqscan = off; SET enable_bitmapscan = off;
	SET enable_indexscan = off;
}
step s2_begin	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2_read	{ SELECT id, k FROM bss WHERE c = 2 ORDER BY k, id LIMIT 3; }
step s2_update	{ UPDATE bss SET note = 's2' WHERE id = 191; }
step s2_commit	{ COMMIT; }

permutation s0_vacuum s1_plan s1_fetches s1_begin s2_begin s1_read s2_read s1_update s2_update s1_commit s2_commit
permutation s0_vacuum s1_fetches s1_begin s2_begin s1_read s2_read s1_update s1_commit s2_update s2_commit
