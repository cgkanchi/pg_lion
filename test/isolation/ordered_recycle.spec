# A TID the paused walk has already met is recycled and met again (2026-09-25
# review; DESIGN.md §30.4, "Stopping early").
#
# c = 1 holds A (k = 2), X (k = 1, deleted before the cursor starts) and B
# (k = 100), with 3,000 filler rows between them in btree order.  The cursor
# returns A; VACUUM then removes X - the paused scan holds no pin (§29.5) - and
# an insert takes X's heap slot with k = 50, so the btree walk meets X's TID a
# second time, now as a tuple the cursor's snapshot cannot see.  The scan used
# to count that as one more member found, reach the set's cardinality and stop
# before B.  A member now counts once, however often the walk meets its TID,
# and FETCH ALL must return B.
setup
{
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE TABLE r (id int, k int, c int, pad text) WITH (autovacuum_enabled = off);
	ALTER TABLE r ALTER pad SET STORAGE PLAIN;
	INSERT INTO r SELECT 100+i, 3 + i % 47, 0, 'f' FROM generate_series(1,3000) i;
	INSERT INTO r VALUES (1, 2, 1, repeat('a',3000)), (2, 1000, 0, repeat('a',3000)),
	                     (3, 1, 1, repeat('a',3000)), (4, 1001, 0, repeat('a',3000)),
	                     (5, 100, 1, repeat('a',3000)), (6, 1002, 0, repeat('a',3000));
	CREATE INDEX r_k ON r (k);
	CREATE INDEX r_c ON r USING lion (c);
	DELETE FROM r WHERE id = 3;
}
teardown { DROP TABLE r; }

session s1
setup { SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_indexscan = off; SET enable_indexonlyscan = off; SET enable_sort = off; }
step s1_tids { SELECT ctid, id, k, c FROM r WHERE id < 100 ORDER BY id; }
step s1_plan { EXPLAIN (COSTS OFF) SELECT id, k FROM r WHERE c = 1 ORDER BY k; }
step s1_begin { BEGIN; }
step s1_declare { DECLARE cur NO SCROLL CURSOR FOR SELECT id, k, ctid FROM r WHERE c = 1 ORDER BY k; }
step s1_f1 { FETCH 1 FROM cur; }
step s1_rest { FETCH ALL FROM cur; }
step s1_commit { COMMIT; }
step s1_after { SELECT ctid, id, k, c FROM r WHERE id < 100 ORDER BY id; }

session s2
step s2_vac { VACUUM r; }

session s3
step s3_ins { INSERT INTO r VALUES (7, 50, 0, repeat('b',3000)) RETURNING ctid; }

permutation s1_tids s1_plan s1_begin s1_declare s1_f1 s2_vac s3_ins s1_rest s1_commit s1_after
