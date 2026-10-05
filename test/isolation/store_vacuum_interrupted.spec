# A VACUUM of an ordered window store (DESIGN.md §41) cut short between the
# permutation and the columns.
#
# VACUUM's bulk delete finds an ordered window's dead rows through its
# permutation and clears them from the permutation and from every column.
# A crash, or an ERROR, between the two must not leave an entry naming a row
# whose values are gone: verify() checks every entry's order value against
# its bucket's range, and a split sorts a bucket's rows by the values its
# entries lead to.  So the permutation is cleared first, and a value no entry
# names is what an interrupted VACUUM may leave (as an interrupted insert
# does).  The injection point "lion-store-vacuum-chain-cleared", after each
# chain VACUUM clears, ERRORs here after the first chain of the window; the
# index pages it wrote stay written, as after a crash.  Two indexes: lion_vi_h
# keeps its short order column in heap layout and its text column in key
# order; lion_vi_o has every column in key order.  Each is then verified, its
# window shown, VACUUMed again, and filled - into the dead rows' heap
# positions, and into a second window whose one bucket splits - and its
# values checked against the heap.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE TABLE stvi_h (k int, t text) WITH (autovacuum_enabled = off);
	INSERT INTO stvi_h SELECT i % 50, repeat(md5(i::text), 3) FROM generate_series(1, 3000) i;
	CREATE INDEX lion_vi_h ON stvi_h USING lion (k) INCLUDE (t)
		WITH (store_values = on, cluster_column = k);
	CREATE TABLE stvi_o (k int, t text) WITH (autovacuum_enabled = off);
	INSERT INTO stvi_o SELECT * FROM stvi_h;
	SET pg_lion.store_order_min_pages = 0;
	CREATE INDEX lion_vi_o ON stvi_o USING lion (k) INCLUDE (t)
		WITH (store_values = on, cluster_column = k);
	RESET pg_lion.store_order_min_pages;
}

teardown
{
	DROP TABLE stvi_h, stvi_o;
	DO $$ BEGIN PERFORM injection_points_detach('lion-store-vacuum-chain-cleared');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

session s1
setup
{
	SET client_min_messages = warning;
	SELECT injection_points_set_local();
}
step s1_delete		{ DELETE FROM stvi_h WHERE k % 4 <> 1; DELETE FROM stvi_o WHERE k % 4 <> 1; }
step s1_attach		{ SELECT injection_points_attach('lion-store-vacuum-chain-cleared', 'error'); }
step s1_vacuum_h	{ VACUUM stvi_h; }
step s1_vacuum_o	{ VACUUM stvi_o; }
step s1_detach		{ SELECT injection_points_detach('lion-store-vacuum-chain-cleared'); }
step s1_window		{
	SELECT i AS index, w.order_attno, w.entries, w.orphaned, w.heap_pages > 0 AS heap_layout
	  FROM unnest(ARRAY['lion_vi_h', 'lion_vi_o']::regclass[]) i,
		   LATERAL lion_index_store_window(i, 0) w ORDER BY 1;
}
step s1_verify_h	{ SELECT lion_index_verify('lion_vi_h', true) AS verify_h; }
step s1_verify_o	{ SELECT lion_index_verify('lion_vi_o', true) AS verify_o; }
step s1_vacuum_h2	{ VACUUM stvi_h; }
step s1_vacuum_o2	{ VACUUM stvi_o; }
step s1_fill		{
	INSERT INTO stvi_h SELECT i % 50, repeat(md5(i::text), 3) FROM generate_series(1, 9000) i;
	INSERT INTO stvi_o SELECT i % 50, repeat(md5(i::text), 3) FROM generate_series(1, 9000) i;
}
step s1_check		{
	SELECT (SELECT count(*) FROM stvi_h
			 WHERE lion_index_stored('lion_vi_h', ctid) IS DISTINCT FROM ARRAY[k::text, t]) AS mismatch_h,
		   (SELECT count(*) FROM stvi_o
			 WHERE lion_index_stored('lion_vi_o', ctid) IS DISTINCT FROM ARRAY[k::text, t]) AS mismatch_o,
		   (lion_index_store_window('lion_vi_h', 1)).buckets > 1 AS split_h,
		   (lion_index_store_window('lion_vi_o', 1)).buckets > 1 AS split_o;
}

permutation
	s1_delete
	s1_attach
	s1_vacuum_h
	s1_vacuum_o
	s1_detach
	s1_window
	s1_verify_h
	s1_verify_o
	s1_vacuum_h2
	s1_vacuum_o2
	s1_window
	s1_verify_h
	s1_verify_o
	s1_fill
	s1_check
	s1_verify_h
	s1_verify_o
