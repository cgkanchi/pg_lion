# lionvec scans against VACUUM and the inserts that reuse what it frees
# (DESIGN.md §44.5, "A scan against VACUUM").
#
# A scan copies what it needs of a CODE item - the row's TID and where its
# REFINE item is - into its queue and holds no lock; at most the pins of
# pages its read streams read ahead (DESIGN.md §44.4, "Read streams"), the
# default from PostgreSQL 17, which VACUUM, taking no cleanup lock, does not
# wait for.  VACUUM removes dead rows' CODE items (PageIndexMultiDelete(),
# which moves the items left on a page) and then their REFINE items, whose
# slots an insert may take again, as the heap may the TIDs.  The scan must take a REFINE item that is gone,
# or is another row's, as its row gone, follow its chains as they stand, and
# return exactly the rows of its snapshot, each once.
#
# 2,500 of the 3,000 rows are deleted before any scan's snapshot, so they
# are dead to every one and VACUUM removes them, in two flushes of its
# REFINE locations (maintenance_work_mem of 64 kB holds 2,048 of them).
# Then 2,500 new rows near the query go in, after the scan's snapshot, into
# the freed REFINE slots and heap TIDs.  Two pauses:
#
#  * a cursor (no injection point: a cursor IS the pause), parked after five
#    rows with the rest of its queue holding REFINE locations;
#  * lionvec-probe-page, between two pages of a chain in the middle of a
#    probe: the pages ahead are compacted and the chain's tail extended
#    while the scan stands on it, its stream holding pins of the next ones.
#    The VACUUM step does not wait for the parked scan, which holds no
#    lock.
#
# The rows a scan returns are compared with a sort under the same snapshot.
# pg_lion.vector_margin and vector_patience make it read every partition
# that could hold a nearer row, so that the comparison is exact.

setup
{
	SET synchronous_commit = on;
	SET client_min_messages = warning;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS vector;
	CREATE EXTENSION IF NOT EXISTS pg_lion_vector;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	RESET client_min_messages;

	CREATE FUNCTION lvv_vec(seed bigint, scale float8 DEFAULT 1.0)
	RETURNS vector LANGUAGE sql IMMUTABLE SET search_path FROM CURRENT AS $$
		SELECT array_agg(((hashint8extended(seed * 1000 + j, 42) % 100000)::float8
						  / 100000.0 * scale)::real ORDER BY j)::vector
		  FROM generate_series(1, 16) j
	$$;
	CREATE TABLE lvv (id int, v vector(16)) WITH (autovacuum_enabled = off);
	INSERT INTO lvv SELECT i, lvv_vec(i) FROM generate_series(1, 3000) i;
	CREATE INDEX lvv_v ON lvv USING lionvec (v) WITH (partitions = 4);
	ANALYZE lvv;
	DELETE FROM lvv WHERE id % 6 <> 0;

	CREATE TABLE lvv_got (n serial, id int);
	CREATE FUNCTION lvv_take(cname text, n int) RETURNS bigint
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
			INSERT INTO lvv_got (id) VALUES (r.id);
			got := got + 1;
		END LOOP;
		RETURN got;
	END $fn$;
}

teardown
{
	DROP TABLE lvv, lvv_got;
	DROP FUNCTION lvv_take(text, int);
	DROP FUNCTION lvv_vec(bigint, float8);
	DO $$ BEGIN PERFORM injection_points_detach('lionvec-probe-page');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

# The reader.
session s1
setup
{
	SET pg_lion.vector_margin = 100;
	SET pg_lion.vector_patience = 0;
	SET pg_lion.vector_epsilon = 6;
	SET enable_seqscan = off;
	SET enable_sort = off;
	SELECT injection_points_set_local();
}
step s1_open	{
	BEGIN ISOLATION LEVEL REPEATABLE READ;
	DECLARE c CURSOR FOR SELECT id FROM lvv ORDER BY v <-> lvv_vec(-3, 0.2);
	SELECT lvv_take('c', 5);
}
step s1_drain	{ SELECT lvv_take('c', NULL); }
step s1_park	{ SELECT injection_points_attach('lionvec-probe-page', 'wait'); }
# The whole scan in one statement, which parks in its first probe.
step s1_scan	{
	BEGIN ISOLATION LEVEL REPEATABLE READ;
	INSERT INTO lvv_got (id)
	SELECT id FROM (SELECT id FROM lvv ORDER BY v <-> lvv_vec(-3, 0.2) LIMIT 400) s;
}
# What the scan returned against a sort under the same snapshot.
step s1_check	{
	SET LOCAL enable_seqscan = on;
	SET LOCAL enable_sort = on;
	SET LOCAL enable_indexscan = off;
	SELECT count(*) AS rows, count(DISTINCT g.id) AS distinct_rows,
		   count(*) = count(*) FILTER (WHERE g.id = s.id) AS sort_order
	  FROM lvv_got g
	  JOIN (SELECT row_number() OVER (ORDER BY v <-> lvv_vec(-3, 0.2), id) AS n, id
			  FROM lvv) s USING (n);
	COMMIT;
}

# VACUUM, then the new rows into what it freed.
session s2
setup			{ SET maintenance_work_mem = '64kB'; }
step s2_vacuum	{ VACUUM lvv; }
step s2_insert	{
	INSERT INTO lvv
	SELECT 5000 + i, lvv_vec(-3, 0.2) + lvv_vec(7000 + i, 0.05)
	  FROM generate_series(1, 2500) i;
}

# Wakes s1, and checks the index afterwards: each row once, the new ones
# nearest.
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
	SELECT injection_points_detach('lionvec-probe-page');
	SELECT injection_points_wakeup('lionvec-probe-page');
}
step s3_whole	{
	SELECT count(*) AS rows, count(DISTINCT id) AS distinct_rows,
		   count(*) FILTER (WHERE id > 5000) AS new_rows
	  FROM (SELECT id FROM lvv ORDER BY v <-> lvv_vec(5, 0.7)) s;
}

# A cursor parked with REFINE locations queued: VACUUM frees them, inserts
# take them again.
permutation s1_open s2_vacuum s2_insert s1_drain s1_check s3_whole

# A probe parked between two pages of its chain.
permutation s1_park s1_scan s2_vacuum s2_insert s3_wake s1_check s3_whole
