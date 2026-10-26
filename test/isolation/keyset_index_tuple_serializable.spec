# Two SERIALIZABLE transactions, one reading through a LionOrderedJoin whose
# key set is tested on the fact walk's index tuples (DESIGN.md §47.6, "Tested
# on the index tuple") - the three top-ranked fact rows whose key is in the
# key set; the top-ranked row's key is not, and the walk turns its entry away
# without reading the heap.  The other moves that row's key into the key set
# - an update of an INCLUDE column of the walked btree, never HOT, so a new
# index entry - after reading the key table's rows the first then inserts
# one of.  The first's read missed the moved row and the second's the
# inserted one: write skew, which must end in a serialization failure, as it
# does for core's plan.  The walk read the btree leaf the new entry goes to
# and holds its page lock (core's btree takes it, as for an index-only scan),
# though it took no tuple lock on the row it did not fetch.

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE TABLE kco (pk int PRIMARY KEY, tier int NOT NULL);
	INSERT INTO kco SELECT i, i % 3 FROM generate_series(1, 200) i;
	CREATE INDEX kco_tier ON kco USING lion (tier);
	CREATE TABLE kcpc (id int PRIMARY KEY, fk int, rank int NOT NULL);
	INSERT INTO kcpc
	SELECT i, CASE WHEN i = 2000 THEN 150 ELSE 1 + (i * 7) % 200 END, i
	  FROM generate_series(1, 2000) i;
	CREATE INDEX kcpc_rank ON kcpc (rank DESC) INCLUDE (fk);
	CREATE TABLE kib (fk int NOT NULL, prod int NOT NULL, PRIMARY KEY (fk, prod));
	INSERT INTO kib SELECT k, 1 FROM generate_series(1, 10) k;
	ANALYZE kco;
	ANALYZE kcpc;
	ANALYZE kib;
}
teardown
{
	DROP TABLE kco, kcpc, kib;
}

session s1
setup
{
	SET pg_lion.pushdown_margin = 1000;
	SET enable_hashjoin = off; SET enable_mergejoin = off;
}
step s1_plan	{ EXPLAIN (COSTS OFF) SELECT f.id, f.fk FROM kcpc f JOIN kco d ON f.fk = d.pk WHERE f.fk IN (SELECT fk FROM kib WHERE prod = 1) ORDER BY f.rank DESC LIMIT 3; }
step s1_begin	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1_read	{ SELECT f.id, f.fk FROM kcpc f JOIN kco d ON f.fk = d.pk WHERE f.fk IN (SELECT fk FROM kib WHERE prod = 1) ORDER BY f.rank DESC LIMIT 3; }
step s1_insert	{ INSERT INTO kib VALUES (5, 9); }
step s1_commit	{ COMMIT; }

session s2
step s2_begin	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2_read	{ SELECT count(*) FROM kib WHERE prod = 9; }
step s2_update	{ UPDATE kcpc SET fk = 1 WHERE id = 2000; }
step s2_commit	{ COMMIT; }

permutation s1_plan s1_begin s2_begin s1_read s2_read s2_update s1_insert s1_commit s2_commit
permutation s1_begin s2_begin s1_read s2_read s2_update s2_commit s1_insert s1_commit
