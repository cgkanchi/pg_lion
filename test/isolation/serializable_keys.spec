# The granularity of lion's SERIALIZABLE predicate locks (DESIGN.md §9,
# "SERIALIZABLE"): a read locks the pages that stand for the keys it read -
# the posting root of a CHAIN entry, the directory leaf of an INLINE one or of
# a key that is absent - and an insert checks the page that stands for its
# key.  Each reader below runs twice, two transactions each reading one key
# (or a few) and inserting a row:
#
#   - "other": each inserts keys neither read.  Both commit, as with a B-tree.
#     Column c's keys are CHAIN entries all on one directory leaf, so only the
#     roots tell them apart; column k's are INLINE, on leaves of their own.
#     A key's rows lie on a few heap pages: the heap locks of a read of rows
#     on more pages than max_pred_locks_per_relation allows become one on the
#     whole table, which conflicts with every insert, through any index.
#   - "skew": each inserts the key the OTHER read, the classic write skew.
#     The second commit must fail with a serialization failure.
#
# One pair of permutations per reader: the count pushdown (LionCount), the
# SQL count function, a plain index scan, a bitmap scan, LionOrdered, the semi
# join path (LionSemiJoin), a range and an IN list.  reader_serializable.spec
# has every reader's absent-key case.

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE TABLE sk (id int, c int NOT NULL, k int NOT NULL);
	INSERT INTO sk SELECT g, (g / 4000) * 10 + g % 10, g % 5000
	  FROM generate_series(0, 99999) g;
	CREATE INDEX sk_c ON sk USING lion (c) WITH (inline_limit = 64);
	CREATE INDEX sk_k ON sk USING lion (k);
	CREATE INDEX sk_id ON sk (id);
	CREATE TABLE skd (pk int PRIMARY KEY) WITH (autovacuum_enabled = off);
	INSERT INTO skd SELECT g FROM generate_series(0, 249) g;
}
setup { VACUUM ANALYZE sk; }
# Never VACUUMed, so its visibility map stays empty and the semijoin's outer
# side plans as a bitmap scan whatever snapshots other backends hold: a
# VACUUM marks its pages all-visible only when none is older than the insert,
# and the plan would turn to an index-only scan when it can.
setup { ANALYZE skd; }
teardown
{
	DROP TABLE sk, skd;
}

session s1
setup			{ SET enable_seqscan = off; }
step s1_begin	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1_count	{ SELECT count(*) FROM sk WHERE c = 1; }
step s1_func	{ SELECT lion_index_count('sk_c', 1); }
step s1_idx		{ SET LOCAL enable_bitmapscan = off; SELECT sum(id) FROM sk WHERE c = 1; }
step s1_bmp		{ SET LOCAL enable_indexscan = off; SELECT sum(id) FROM sk WHERE c = 1; }
step s1_ord		{ SET LOCAL enable_bitmapscan = off; SET LOCAL pg_lion.enable_plain_scan = off;
				  SET LOCAL pg_lion.pushdown_margin = 1000; SET LOCAL enable_indexonlyscan = off;
				  SELECT id FROM sk WHERE c = 1 ORDER BY id LIMIT 3; }
step s1_semi	{ SET LOCAL enable_hashjoin = off; SET LOCAL enable_mergejoin = off;
				  SET LOCAL enable_nestloop = off;
				  SELECT count(*) FROM skd WHERE pk BETWEEN 1 AND 2
				    AND EXISTS (SELECT 1 FROM sk WHERE sk.c = skd.pk); }
step s1_range	{ SELECT count(*) FROM sk WHERE c BETWEEN 1 AND 3; }
step s1_in		{ SELECT count(*) FROM sk WHERE c IN (1, 3); }
step s1_kcount	{ SELECT count(*) FROM sk WHERE k = 1; }
step s1_kidx	{ SET LOCAL enable_bitmapscan = off; SELECT sum(id) FROM sk WHERE k = 1; }
step s1_kbmp	{ SET LOCAL enable_indexscan = off; SELECT sum(id) FROM sk WHERE k = 1; }
step s1_krange	{ SELECT count(*) FROM sk WHERE k BETWEEN 1 AND 3; }
step s1_other	{ INSERT INTO sk VALUES (200001, 200, 4000); }
step s1_skew	{ INSERT INTO sk VALUES (200001, 125, 2500); }
step s1_commit	{ COMMIT; }
step s1_plans
{
	EXPLAIN (COSTS OFF) SELECT count(*) FROM sk WHERE c = 1;
	SET enable_bitmapscan = off;
	EXPLAIN (COSTS OFF) SELECT sum(id) FROM sk WHERE c = 1;
	EXPLAIN (COSTS OFF) SELECT sum(id) FROM sk WHERE k = 1;
	SET pg_lion.enable_plain_scan = off;
	SET pg_lion.pushdown_margin = 1000;
	SET enable_indexonlyscan = off;
	EXPLAIN (COSTS OFF) SELECT id FROM sk WHERE c = 1 ORDER BY id LIMIT 3;
	RESET pg_lion.enable_plain_scan;
	RESET pg_lion.pushdown_margin;
	RESET enable_bitmapscan;
	RESET enable_indexonlyscan;
	SET enable_indexscan = off;
	EXPLAIN (COSTS OFF) SELECT sum(id) FROM sk WHERE c = 1;
	EXPLAIN (COSTS OFF) SELECT sum(id) FROM sk WHERE k = 1;
	RESET enable_indexscan;
	SET enable_hashjoin = off;
	SET enable_mergejoin = off;
	SET enable_nestloop = off;
	EXPLAIN (COSTS OFF) SELECT count(*) FROM skd WHERE pk BETWEEN 1 AND 2
	  AND EXISTS (SELECT 1 FROM sk WHERE sk.c = skd.pk);
	RESET enable_hashjoin;
	RESET enable_mergejoin;
	RESET enable_nestloop;
	EXPLAIN (COSTS OFF) SELECT count(*) FROM sk WHERE c BETWEEN 1 AND 3;
	EXPLAIN (COSTS OFF) SELECT count(*) FROM sk WHERE c IN (1, 3);
	EXPLAIN (COSTS OFF) SELECT count(*) FROM sk WHERE k = 1;
	EXPLAIN (COSTS OFF) SELECT count(*) FROM sk WHERE k BETWEEN 1 AND 3;
}

session s2
setup			{ SET enable_seqscan = off; }
step s2_begin	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2_count	{ SELECT count(*) FROM sk WHERE c = 125; }
step s2_func	{ SELECT lion_index_count('sk_c', 125); }
step s2_idx		{ SET LOCAL enable_bitmapscan = off; SELECT sum(id) FROM sk WHERE c = 125; }
step s2_bmp		{ SET LOCAL enable_indexscan = off; SELECT sum(id) FROM sk WHERE c = 125; }
step s2_ord		{ SET LOCAL enable_bitmapscan = off; SET LOCAL pg_lion.enable_plain_scan = off;
				  SET LOCAL pg_lion.pushdown_margin = 1000; SET LOCAL enable_indexonlyscan = off;
				  SELECT id FROM sk WHERE c = 125 ORDER BY id LIMIT 3; }
step s2_semi	{ SET LOCAL enable_hashjoin = off; SET LOCAL enable_mergejoin = off;
				  SET LOCAL enable_nestloop = off;
				  SELECT count(*) FROM skd WHERE pk BETWEEN 125 AND 126
				    AND EXISTS (SELECT 1 FROM sk WHERE sk.c = skd.pk); }
step s2_range	{ SELECT count(*) FROM sk WHERE c BETWEEN 125 AND 127; }
step s2_in		{ SELECT count(*) FROM sk WHERE c IN (125, 127); }
step s2_kcount	{ SELECT count(*) FROM sk WHERE k = 2500; }
step s2_kidx	{ SET LOCAL enable_bitmapscan = off; SELECT sum(id) FROM sk WHERE k = 2500; }
step s2_kbmp	{ SET LOCAL enable_indexscan = off; SELECT sum(id) FROM sk WHERE k = 2500; }
step s2_krange	{ SELECT count(*) FROM sk WHERE k BETWEEN 2500 AND 2502; }
step s2_other	{ INSERT INTO sk VALUES (200002, 60, 1000); }
step s2_skew	{ INSERT INTO sk VALUES (200002, 1, 1); }
step s2_commit	{ COMMIT; }

# the plans each reader below is
permutation s1_plans

permutation s1_begin s2_begin s1_count s2_count s1_other s2_other s1_commit s2_commit
permutation s1_begin s2_begin s1_count s2_count s1_skew s2_skew s1_commit s2_commit
permutation s1_begin s2_begin s1_func s2_func s1_other s2_other s1_commit s2_commit
permutation s1_begin s2_begin s1_func s2_func s1_skew s2_skew s1_commit s2_commit
permutation s1_begin s2_begin s1_idx s2_idx s1_other s2_other s1_commit s2_commit
permutation s1_begin s2_begin s1_idx s2_idx s1_skew s2_skew s1_commit s2_commit
permutation s1_begin s2_begin s1_bmp s2_bmp s1_other s2_other s1_commit s2_commit
permutation s1_begin s2_begin s1_bmp s2_bmp s1_skew s2_skew s1_commit s2_commit
permutation s1_begin s2_begin s1_ord s2_ord s1_other s2_other s1_commit s2_commit
permutation s1_begin s2_begin s1_ord s2_ord s1_skew s2_skew s1_commit s2_commit
permutation s1_begin s2_begin s1_semi s2_semi s1_other s2_other s1_commit s2_commit
permutation s1_begin s2_begin s1_semi s2_semi s1_skew s2_skew s1_commit s2_commit
permutation s1_begin s2_begin s1_range s2_range s1_other s2_other s1_commit s2_commit
permutation s1_begin s2_begin s1_range s2_range s1_skew s2_skew s1_commit s2_commit
permutation s1_begin s2_begin s1_in s2_in s1_other s2_other s1_commit s2_commit
permutation s1_begin s2_begin s1_in s2_in s1_skew s2_skew s1_commit s2_commit
permutation s1_begin s2_begin s1_kcount s2_kcount s1_other s2_other s1_commit s2_commit
permutation s1_begin s2_begin s1_kcount s2_kcount s1_skew s2_skew s1_commit s2_commit
permutation s1_begin s2_begin s1_kidx s2_kidx s1_other s2_other s1_commit s2_commit
permutation s1_begin s2_begin s1_kidx s2_kidx s1_skew s2_skew s1_commit s2_commit
permutation s1_begin s2_begin s1_kbmp s2_kbmp s1_other s2_other s1_commit s2_commit
permutation s1_begin s2_begin s1_kbmp s2_kbmp s1_skew s2_skew s1_commit s2_commit
permutation s1_begin s2_begin s1_krange s2_krange s1_other s2_other s1_commit s2_commit
permutation s1_begin s2_begin s1_krange s2_krange s1_skew s2_skew s1_commit s2_commit
