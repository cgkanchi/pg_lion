# lionvec scans that read vectors by their places in TOAST (DESIGN.md
# §44.13), against deletes, VACUUM of the table or of its TOAST relation
# alone, and inserts that take again the space VACUUM frees.
#
# A place is only ever a hint: a scan reads a value's chunks where its
# entry says they are, and checks each - its line pointer, its visibility to
# the TOAST snapshot, its value's id and its sequence number - and reads the
# row from the heap when one fails.  What makes the hint safe to act on is
# that the chunks of a value some snapshot still sees stay where they are:
# VACUUM removes only those of values dead to every snapshot, and the
# scan's lock on the TOAST relation, held to the end of its transaction,
# keeps it from being rewritten (VACUUM FULL) or truncated under it.  The
# exact path's members read by place are then checked before each is
# returned (the heap row the snapshot sees stores that value), the probed
# path's after its heap row is fetched.
#
# A third of the rows are deleted before any scan's snapshot, dead to all,
# their entries left in the index: VACUUM removes their chunks, and new
# rows take the line pointers again.  Rows deleted after the scan's
# snapshot are still the scan's to return.  Two pauses:
#
#  * lionvec-exact-located: the exact path, between reading the locator's
#    entries of its members and reading their chunks.  The members of rows
#    dead to all are then read at places that hold other values' chunks,
#    or none, and fall back (the DEBUG1 line counts them, "not"); those of
#    the rows deleted after its snapshot are read by place, and returned.
#  * a cursor of the probed path, parked after five rows, with REFINE items
#    and their places queued; VACUUM of the whole table removes the dead
#    rows' items and chunks, and the inserts take them again.
#
# The rows a scan returns are compared with a sort under the same snapshot.
# pg_lion.vector_margin and vector_patience make the probed scans read
# every partition that could hold a nearer row, so that the comparison is
# exact.

setup
{
	SET synchronous_commit = on;
	SET client_min_messages = warning;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS vector;
	CREATE EXTENSION IF NOT EXISTS pg_lion_vector;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	RESET client_min_messages;

	/* a pseudo-random vector of 768 coordinates, in TOAST: two chunks */
	CREATE FUNCTION lvt_vec(seed bigint, scale float8 DEFAULT 1.0)
	RETURNS vector LANGUAGE sql IMMUTABLE SET search_path FROM CURRENT AS $$
		SELECT array_agg(((hashint8extended(seed * 1000 + j, 42) % 100000)::float8
						  / 100000.0 * scale)::real ORDER BY j)::vector
		  FROM generate_series(1, 768) j
	$$;
	CREATE TABLE lvt (id int, cat int, v vector(768)) WITH (autovacuum_enabled = off);
	INSERT INTO lvt SELECT i, i % 10, lvt_vec(i) FROM generate_series(1, 900) i;
	CREATE INDEX lvt_cat ON lvt USING lion (cat);
	CREATE INDEX lvt_v ON lvt USING lionvec (v) WITH (partitions = 4);
	ANALYZE lvt;
	DELETE FROM lvt WHERE id % 3 = 0;

	CREATE TABLE lvt_got (n serial, id int);
	CREATE FUNCTION lvt_take(cname text, n int) RETURNS bigint
	LANGUAGE plpgsql AS $fn$
	DECLARE
		c refcursor := cname;
		r record;
		got bigint := 0;
	BEGIN
		LOOP
			EXIT WHEN n IS NOT NULL AND got >= n;
			FETCH c INTO r;
			EXIT WHEN NOT FOUND;
			INSERT INTO lvt_got (id) VALUES (r.id);
			got := got + 1;
		END LOOP;
		RETURN got;
	END $fn$;
}

teardown
{
	DROP TABLE lvt, lvt_got;
	DROP FUNCTION lvt_take(text, int);
	DROP FUNCTION lvt_vec(bigint, float8);
	DO $$ BEGIN PERFORM injection_points_detach('lionvec-exact-located');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

# The reader.
session s1
setup
{
	SET pg_lion.vector_margin = 100;
	SET pg_lion.vector_patience = 0;
	SET pg_lion.vector_epsilon = 6;
	SET pg_lion.vector_prefetch = off;
	SET max_parallel_workers_per_gather = 0;
	SET enable_seqscan = off;
	SET enable_bitmapscan = off;
	SET enable_sort = off;
	SELECT injection_points_set_local();
}
step s1_park	{ SELECT injection_points_attach('lionvec-exact-located', 'wait'); }
# The exact path (LionOrdered over the lion index on cat), in one
# statement, which parks once it has its members' entries.
step s1_scan	{
	BEGIN ISOLATION LEVEL REPEATABLE READ;
	SET LOCAL pg_lion.enable_plain_scan = off;
	SET LOCAL pg_lion.pushdown_margin = 1000;
	SET LOCAL client_min_messages = debug1;
	INSERT INTO lvt_got (id)
	SELECT id FROM (SELECT id FROM lvt WHERE cat = 3 ORDER BY v <-> lvt_vec(-3, 0.2)) s;
	RESET client_min_messages;
}
# The scan's rows, once it has returned them all: a step of s1's, so that
# the steps after it start only once the scan is done (it touches no TOAST).
step s1_got	{ SELECT count(*) AS got FROM lvt_got; }
step s1_check_exact	{
	SET LOCAL enable_seqscan = on;
	SET LOCAL enable_sort = on;
	SET LOCAL enable_indexscan = off;
	SET LOCAL pg_lion.enable_ordered_scan = off;
	SET LOCAL pg_lion.vector_postfilter = exact;
	SELECT count(*) AS rows, count(DISTINCT g.id) AS distinct_rows,
		   count(*) = count(*) FILTER (WHERE g.id = s.id) AS sort_order,
		   count(*) FILTER (WHERE g.id % 7 = 0) AS deleted_since
	  FROM lvt_got g
	  JOIN (SELECT row_number() OVER (ORDER BY v <-> lvt_vec(-3, 0.2), id) AS n, id
			  FROM lvt WHERE cat = 3) s USING (n);
	COMMIT;
}
# The probed path: a cursor over every row, nearest first.
step s1_open	{
	BEGIN ISOLATION LEVEL REPEATABLE READ;
	SET LOCAL client_min_messages = debug1;
	DECLARE c CURSOR FOR SELECT id FROM lvt ORDER BY v <-> lvt_vec(-3, 0.2);
	SELECT lvt_take('c', 5);
}
step s1_drain	{ SELECT lvt_take('c', NULL); }
step s1_check	{
	SET LOCAL enable_seqscan = on;
	SET LOCAL enable_sort = on;
	SET LOCAL enable_indexscan = off;
	SELECT count(*) AS rows, count(DISTINCT g.id) AS distinct_rows,
		   count(*) = count(*) FILTER (WHERE g.id = s.id) AS sort_order,
		   count(*) FILTER (WHERE g.id % 7 = 0) AS deleted_since
	  FROM lvt_got g
	  JOIN (SELECT row_number() OVER (ORDER BY v <-> lvt_vec(-3, 0.2), id) AS n, id
			  FROM lvt) s USING (n);
}
# The cursor's scan ends, and says what it did, when the COMMIT drops it: a
# step of its own, so that its DEBUG lines come after s1_check's rows
# whenever they reach isolationtester, not before them when they share a
# read with the rows and after when they do not.
step s1_commit	{ COMMIT; }

# The writer: rows deleted after the scan's snapshot; the TOAST relation
# vacuumed alone, or the whole table; new rows near the query, into the
# space freed.
session s2
step s2_delete	{ DELETE FROM lvt WHERE id % 7 = 0; }
step s2_vacuum_toast	{ VACUUM (PROCESS_MAIN false) lvt; }
step s2_vacuum	{ VACUUM lvt; }
step s2_insert	{
	INSERT INTO lvt
	SELECT 5000 + i, 3, lvt_vec(-3, 0.2) + lvt_vec(7000 + i, 0.05)
	  FROM generate_series(1, 300) i;
}
# The scan's lock on the TOAST relation, held to the end of its
# transaction: the ACCESS EXCLUSIVE lock VACUUM FULL of that relation, or a
# VACUUM truncating it, would take waits for it.
step s2_toast_lock	{
	SELECT mode, granted FROM pg_locks
	 WHERE locktype = 'relation' AND pid <> pg_backend_pid()
	   AND relation = (SELECT reltoastrelid FROM pg_class WHERE oid = 'lvt'::regclass);
}

# Wakes s1; and afterwards, every row once through the whole index, the new
# ones nearest.
session s3
setup
{
	SET pg_lion.vector_margin = 100;
	SET pg_lion.vector_patience = 0;
	SET pg_lion.vector_epsilon = 6;
	SET enable_seqscan = off;
	SET enable_sort = off;
}
step s3_wake	{
	SELECT injection_points_detach('lionvec-exact-located');
	SELECT injection_points_wakeup('lionvec-exact-located');
}
step s3_whole	{
	SELECT count(*) AS rows, count(DISTINCT id) AS distinct_rows,
		   count(*) FILTER (WHERE id > 5000) AS new_rows
	  FROM (SELECT id FROM lvt ORDER BY v <-> lvt_vec(-3, 0.2)) s;
}

# The exact path parked between its entries and its chunks: rows deleted,
# the dead rows' chunks vacuumed away and their places taken by new rows;
# the scan holds the TOAST relation against a rewrite to its end, and lets
# it go then.
permutation s1_park s1_scan s2_delete s2_vacuum_toast s2_insert s3_wake s1_got s2_toast_lock s1_check_exact s2_toast_lock s3_whole

# A cursor of the probed path parked with places queued: rows deleted, the
# table vacuumed, its REFINE items, heap TIDs and TOAST space taken again.
permutation s1_open s2_delete s2_vacuum s2_insert s1_drain s1_check s1_commit s3_whole
