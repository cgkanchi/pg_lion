# A LionStoreScan (DESIGN.md §40, "As built: the row gather") paused in a
# cursor while the table changes under it.
#
# The cursor's snapshot is taken at DECLARE; the node streams the lion set a
# window at a time from the first FETCH on, and decides for each window, when
# it gets there, which rows the store answers for: those on heap pages the
# visibility map calls all-visible, under the §9 pin of the container they
# came from.  Rows committed between DECLARE and the first FETCH are in the
# set but invisible to the snapshot, and their pages are no longer
# all-visible; rows deleted or updated while the cursor is paused are still
# visible to it, with their old values, whether the window they are in was
# gathered before the change or after it.  The changes are to runs of ids,
# so that most of the table's pages stay all-visible and their rows come
# from the store beside the changed pages' from the heap.  Every permutation
# drains the cursor into lss_got and compares it, as a multiset, with the
# answer the snapshot saw (lss_expect, taken in setup).
#
# No VACUUM runs while the cursor is paused: between two rows the node holds
# the pins of the piece it is returning, as an index-only scan does, and a
# VACUUM with index entries to remove would wait for it.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;

	CREATE TABLE lss (id int, a int, b int, x int, t text)
		WITH (autovacuum_enabled = off);
	INSERT INTO lss
	SELECT i, i % 7, i % 11, i % 1000, 'v' || (i % 97)
	  FROM generate_series(1, 40000) i;
	CREATE INDEX lss_a ON lss USING lion (a) INCLUDE (id, x) WITH (store_values = on);
	CREATE INDEX lss_b ON lss USING lion (b) INCLUDE (t);
}
setup { VACUUM (FREEZE, ANALYZE) lss; }
setup
{
	CREATE TABLE lss_expect AS
	SELECT id, a, x, t FROM lss WHERE a IN (1, 2) AND b IN (3, 4);
	CREATE TABLE lss_got (id int, a int, x int, t text);

	/* Move up to n rows (all of them for NULL) of the cursor into lss_got. */
	CREATE FUNCTION lss_take(n int) RETURNS bigint
	LANGUAGE plpgsql AS $fn$
	DECLARE
		c refcursor := 'lss_cur';
		r record;
		got bigint := 0;
	BEGIN
		LOOP
			EXIT WHEN n IS NOT NULL AND got >= n;
			FETCH c INTO r;
			EXIT WHEN NOT FOUND;
			INSERT INTO lss_got VALUES (r.id, r.a, r.x, r.t);
			got := got + 1;
		END LOOP;
		RETURN got;
	END $fn$;
}

teardown
{
	DROP FUNCTION lss_take(int);
	DROP TABLE lss, lss_expect, lss_got;
}

session s1
setup
{
	SET enable_seqscan = off; SET enable_bitmapscan = off;
	SET enable_indexscan = off; SET enable_indexonlyscan = off;
}
step s1_begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1_plan	{ EXPLAIN (COSTS OFF) SELECT id, a, x, t FROM lss WHERE a IN (1, 2) AND b IN (3, 4); }
step s1_declare	{ DECLARE lss_cur NO SCROLL CURSOR FOR SELECT id, a, x, t FROM lss WHERE a IN (1, 2) AND b IN (3, 4); }
step s1_take5	{ SELECT lss_take(5); }
step s1_rest	{ SELECT lss_take(NULL); }
step s1_commit	{ COMMIT; }
step s1_check
{
	SELECT (SELECT count(*) FROM lss_got) AS got,
		   (SELECT count(*) FROM lss_expect) AS expected,
		   (SELECT count(*) FROM (TABLE lss_got EXCEPT ALL TABLE lss_expect) x) +
		   (SELECT count(*) FROM (TABLE lss_expect EXCEPT ALL TABLE lss_got) y) AS differ;
}
step s1_now		{ SELECT count(*) FROM lss WHERE a IN (1, 2) AND b IN (3, 4); }

session s2
# before the first FETCH: in the lion set, invisible to the cursor
step s2_early
{
	INSERT INTO lss SELECT 50000 + i, 1, 3, -i, 'early' FROM generate_series(1, 20) i;
	UPDATE lss SET x = -x, t = 'moved' WHERE a = 2 AND b = 4 AND id BETWEEN 30001 AND 32000;
	UPDATE lss SET a = 1 WHERE a = 5 AND b = 3 AND id BETWEEN 1 AND 2000;
}
# while the cursor is paused: not visible, or still visible with old values
step s2_change
{
	INSERT INTO lss SELECT 60000 + i, 2, 4, -i, 'late' FROM generate_series(1, 20) i;
	DELETE FROM lss WHERE a = 1 AND b = 3 AND id BETWEEN 10001 AND 12000;
	UPDATE lss SET a = 6 WHERE a = 2 AND b = 3 AND id BETWEEN 36001 AND 38000;
	UPDATE lss SET x = x + 5000 WHERE a = 1 AND b = 4 AND id BETWEEN 20001 AND 22000;
	UPDATE lss SET b = 3 WHERE a = 1 AND b = 5 AND id BETWEEN 38001 AND 40000;
}

permutation s1_plan s1_begin s1_declare s1_take5 s2_change s1_rest s1_commit s1_check s1_now
permutation s1_begin s1_declare s2_early s1_take5 s2_change s1_rest s1_commit s1_check s1_now
