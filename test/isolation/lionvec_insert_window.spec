# A lionvec insert parked between its WAL records, against scans (DESIGN.md
# §44.5, "Insert").
#
# An insert writes three records under the meta page's exclusive lock: the
# row's REFINE item, its CODE item on its nearest partition's tail, and in an
# index that spills its copy on its second partition's.  Two injection points
# park it between them:
#
#  * lionvec-insert-refined: the REFINE item is in, no CODE item names it;
#  * lionvec-insert-coded: the row is in its nearest partition, and its copy
#    is not yet in its second.
#
# A scan reads the meta page once, as it starts, and then only the chains, a
# page at a time, so one that was open before the insert parked goes on: s1
# opens a cursor, takes a few rows, and drains it while s2 is parked.  Its
# rows must be exactly those of a sort under the same snapshot, each once -
# the parked insert's row, met in one chain and not yet the other, is not
# visible to it either way.  A scan that starts while the insert is parked
# waits for the meta page, which its planning reads too (a buffer lock,
# which isolationtester cannot see: hence the (*) markers), and runs once
# the insert is done.  Its transaction took its snapshot before the insert
# began (REPEATABLE READ: one taken as the query starts could be taken
# before the insert commits or after), so it sees none of the insert's
# rows.  Afterwards every row is returned once by a scan of the whole index,
# and the new rows come first for a query at their cluster.
#
# The markers naming s2_insert, and the empty steps, fix the order in which
# the steps a wakeup frees are reported, which would otherwise race the next
# step: the insert's end first, then a step that waited on it, then the
# next step.
#
# pg_lion.vector_margin and vector_patience make the scans read every
# partition that could hold a nearer row, so that the comparisons are exact
# (test/sql/vector.sql does the same).

setup
{
	SET synchronous_commit = on;
	SET client_min_messages = warning;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS vector;
	CREATE EXTENSION IF NOT EXISTS pg_lion_vector;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	RESET client_min_messages;

	/* a pseudo-random vector, the same on every platform */
	CREATE FUNCTION lvi_vec(seed bigint, scale float8 DEFAULT 1.0)
	RETURNS vector LANGUAGE sql IMMUTABLE SET search_path FROM CURRENT AS $$
		SELECT array_agg(((hashint8extended(seed * 1000 + j, 42) % 100000)::float8
						  / 100000.0 * scale)::real ORDER BY j)::vector
		  FROM generate_series(1, 16) j
	$$;
	CREATE TABLE lvi (id int, v vector(16)) WITH (autovacuum_enabled = off);
	INSERT INTO lvi SELECT i, lvi_vec(i) FROM generate_series(1, 2000) i;
	CREATE INDEX lvi_v ON lvi USING lionvec (v) WITH (partitions = 8);
	ANALYZE lvi;

	CREATE TABLE lvi_got (n serial, id int);
	/* Move up to n rows (all of them for NULL) of a cursor into lvi_got. */
	CREATE FUNCTION lvi_take(cname text, n int) RETURNS bigint
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
			INSERT INTO lvi_got (id) VALUES (r.id);
			got := got + 1;
		END LOOP;
		RETURN got;
	END $fn$;
}

teardown
{
	DROP TABLE lvi, lvi_got;
	DROP FUNCTION lvi_take(text, int);
	DROP FUNCTION lvi_vec(bigint, float8);
	/* Injection points are cluster-wide: never leave one attached, and
	 * never fail the teardown because the permutation already did. */
	DO $$ BEGIN PERFORM injection_points_detach('lionvec-insert-refined');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
	DO $$ BEGIN PERFORM injection_points_detach('lionvec-insert-coded');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

# The reader: a cursor over every row, nearest first.
session s1
setup
{
	SET pg_lion.vector_margin = 100;
	SET pg_lion.vector_patience = 0;
	SET pg_lion.vector_epsilon = 6;
	SET enable_seqscan = off;
	SET enable_sort = off;
}
step s1_open	{
	BEGIN ISOLATION LEVEL REPEATABLE READ;
	DECLARE c CURSOR FOR SELECT id FROM lvi ORDER BY v <-> lvi_vec(-7, 0.5);
	SELECT lvi_take('c', 5);
}
# The same, its snapshot taken first and the cursor opened later.
step s1_begin	{
	BEGIN ISOLATION LEVEL REPEATABLE READ;
	SELECT count(*) AS rows FROM lvi;
}
step s1_declare	{
	DECLARE c CURSOR FOR SELECT id FROM lvi ORDER BY v <-> lvi_vec(-7, 0.5);
	SELECT lvi_take('c', 5);
}
step s1_drain	{ SELECT lvi_take('c', NULL); }
# The rows the cursor returned against a sort under the same snapshot: as
# many, each once, in the sort's order.
step s1_check	{
	SET LOCAL enable_seqscan = on;
	SET LOCAL enable_sort = on;
	SET LOCAL enable_indexscan = off;
	SELECT count(*) AS rows, count(DISTINCT g.id) AS distinct_rows,
		   count(*) = count(*) FILTER (WHERE g.id = s.id) AS sort_order
	  FROM lvi_got g
	  JOIN (SELECT row_number() OVER (ORDER BY v <-> lvi_vec(-7, 0.5), id) AS n, id
			  FROM lvi) s USING (n);
	COMMIT;
}

# The writer: fifty rows of a far cluster, parked at its first.
session s2
setup
{
	SELECT injection_points_set_local();
}
step s2_park_refined	{ SELECT injection_points_attach('lionvec-insert-refined', 'wait'); }
step s2_park_coded		{ SELECT injection_points_attach('lionvec-insert-coded', 'wait'); }
step s2_insert	{
	INSERT INTO lvi
	SELECT 3000 + i, lvi_vec(-1, 0.1) + lvi_vec(9000 + i, 0.05)
	  FROM generate_series(1, 50) i;
}
# Nothing: run after a wakeup, it waits for the insert to end, so that the
# next step starts only then (the isolation README's empty step).
step s2_done	{ }

# Scans afresh.
session s3
setup
{
	SET pg_lion.vector_margin = 100;
	SET pg_lion.vector_patience = 0;
	SET pg_lion.vector_epsilon = 6;
	SET enable_seqscan = off;
	SET enable_sort = off;
}
# A snapshot from before the insert.
step s3_begin	{
	BEGIN ISOLATION LEVEL REPEATABLE READ;
	SELECT count(*) AS rows FROM lvi;
}
# Started while the insert holds the meta page, it waits for it, and in
# s3_begin's transaction finds none of the new rows; started after the
# insert, on its own, it finds them.
step s3_new	{
	SELECT count(*) FILTER (WHERE id > 3000) AS new_rows
	  FROM (SELECT id FROM lvi ORDER BY v <-> lvi_vec(-1, 0.1) LIMIT 60) s;
}
# Every row once, through a scan of the whole index.
step s3_whole	{
	SELECT count(*) AS rows, count(DISTINCT id) AS distinct_rows
	  FROM (SELECT id FROM lvi ORDER BY v <-> lvi_vec(5, 0.7)) s;
}
step s3_commit	{ COMMIT; }

# Wakes s2.
session s4
step s4_wake_refined	{
	SELECT injection_points_detach('lionvec-insert-refined');
	SELECT injection_points_wakeup('lionvec-insert-refined');
}
step s4_wake_coded	{
	SELECT injection_points_detach('lionvec-insert-coded');
	SELECT injection_points_wakeup('lionvec-insert-coded');
}

# Parked with the REFINE item in: the open cursor drains, a new scan waits.
permutation s1_open s2_park_refined s3_begin s2_insert s1_drain s3_new(*, s2_insert) s4_wake_refined s3_commit s1_check s3_whole

# Parked with the row in its nearest partition and not yet its second.
permutation s1_open s2_park_coded s2_insert s1_drain s4_wake_coded s2_done s1_check s3_new s3_whole

# The cursor opened while the insert is parked waits for it, reads the index
# as the insert left it, and returns no row of it.
permutation s1_begin s2_park_coded s2_insert s1_declare(*, s2_insert) s4_wake_coded s1_drain s1_check s3_whole
