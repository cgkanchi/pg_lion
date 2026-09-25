# A LionOrdered scan (DESIGN.md §30) paused in a cursor while the table
# changes under it.
#
# The cursor's snapshot is taken at DECLARE; the node builds its lion TID set
# at the FIRST FETCH and walks the btree afterwards.  Rows committed between
# DECLARE and the first FETCH are in the set but invisible to the snapshot;
# rows committed while the cursor is paused are not in the set - unless one
# takes the heap slot of a dead member VACUUM removed, whose TID the set still
# holds (ordered_recycle.spec) - and are invisible either way; rows deleted,
# or updated out of the filter, while the
# cursor is paused are still visible to it and must still come back, at their
# old place in the order.  Every permutation drains the cursor into oc_got and
# compares it, row by row in order, with the answer the snapshot saw
# (oc_expect, taken in setup).  VACUUM runs while the cursor is paused and
# must complete: the paused node holds no lion pin (§29.5).

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;

	CREATE TABLE oc (id int PRIMARY KEY, k int, c int, pad text)
		WITH (autovacuum_enabled = off);
	INSERT INTO oc
	SELECT i, (i * 7919) % 10007, i % 25, repeat('p', 20)
	  FROM generate_series(1, 10000) i;
	CREATE INDEX oc_k ON oc (k, id);
	CREATE INDEX oc_c ON oc USING lion (c);
}
setup { VACUUM ANALYZE oc; }
setup
{
	CREATE TABLE oc_expect AS
	SELECT row_number() OVER (ORDER BY k, id) AS n, id, k
	  FROM oc WHERE c = 3;
	CREATE TABLE oc_got (n serial, id int, k int);

	/* Move up to n rows (all of them for NULL) of the cursor into oc_got. */
	CREATE FUNCTION oc_take(n int) RETURNS bigint
	LANGUAGE plpgsql AS $fn$
	DECLARE
		c refcursor := 'oc_cur';
		r record;
		got bigint := 0;
	BEGIN
		LOOP
			EXIT WHEN n IS NOT NULL AND got >= n;
			FETCH c INTO r;
			EXIT WHEN NOT FOUND;
			INSERT INTO oc_got (id, k) VALUES (r.id, r.k);
			got := got + 1;
		END LOOP;
		RETURN got;
	END $fn$;
}

teardown
{
	DROP FUNCTION oc_take(int);
	DROP TABLE oc, oc_expect, oc_got;
}

session s1
setup
{
	SET enable_seqscan = off; SET enable_bitmapscan = off;
	SET enable_indexscan = off; SET enable_indexonlyscan = off;
}
step s1_begin	{ BEGIN; }
step s1_plan	{ EXPLAIN (COSTS OFF) SELECT id, k FROM oc WHERE c = 3 ORDER BY k, id; }
step s1_declare	{ DECLARE oc_cur NO SCROLL CURSOR FOR SELECT id, k FROM oc WHERE c = 3 ORDER BY k, id; }
step s1_take5	{ SELECT oc_take(5); }
step s1_rest	{ SELECT oc_take(NULL); }
step s1_commit	{ COMMIT; }
step s1_check
{
	SELECT (SELECT count(*) FROM oc_got) AS got,
		   (SELECT count(*) FROM oc_expect) AS expected,
		   (SELECT count(*) FROM oc_got g FULL JOIN oc_expect e USING (n)
			 WHERE g.id IS DISTINCT FROM e.id OR g.k IS DISTINCT FROM e.k) AS differ;
}
step s1_now		{ SELECT count(*) FROM oc WHERE c = 3; }

session s2
# before the first FETCH: in the lion set, invisible to the cursor
step s2_early
{
	INSERT INTO oc SELECT 20000 + i, -i, 3, 'early' FROM generate_series(1, 20) i;
	UPDATE oc SET c = 3 WHERE c = 4 AND id % 3 = 0;
	UPDATE oc SET k = -k - 100 WHERE c = 3 AND id % 7 = 1 AND id < 10000;
}
# while the cursor is paused: not in the set, or still visible to it
step s2_change
{
	INSERT INTO oc SELECT 30000 + i, -500 - i, 3, 'late' FROM generate_series(1, 20) i;
	INSERT INTO oc SELECT 31000 + i, 20000 + i, 3, 'late' FROM generate_series(1, 20) i;
	DELETE FROM oc WHERE c = 3 AND id % 5 = 0 AND id < 10000;
	UPDATE oc SET c = 4 WHERE c = 3 AND id % 5 = 1 AND id < 10000;
	UPDATE oc SET c = 3 WHERE c = 5 AND id % 2 = 0;
	UPDATE oc SET pad = 'hot' WHERE c = 3 AND id % 5 = 2;
	UPDATE oc SET k = k + 5000 WHERE c = 3 AND id % 5 = 3 AND id < 10000;
}
step s2_vacuum	{ VACUUM oc; }

permutation s1_plan s1_begin s1_declare s1_take5 s2_change s2_vacuum s1_rest s1_commit s1_check s1_now
permutation s1_begin s1_declare s2_early s1_take5 s2_change s2_vacuum s1_rest s1_commit s1_check s1_now
