# A key's predicate locks follow it when the page that stands for it changes
# (DESIGN.md §9, "SERIALIZABLE").  s1 reads key 5000 and s2 key 15000, each on
# a directory leaf of its own; then s3, outside SERIALIZABLE, moves key 5000
# to another page; then each inserts the key the other read, the classic
# write skew, and the second commit must fail.  Had the lock stayed behind,
# s2's insert would check a page s1 never locked and both would commit.
#
#   - spill: s3 inserts enough rows of 5000 to spill its INLINE entry into a
#     posting tree, whose root an insert checks from then on.
#   - split: s3 inserts keys just below 5000 until its leaf splits, and 5000
#     goes to the new right sibling.
#   - vacuum: here the key is 3000, a CHAIN entry whose rows were deleted
#     before s1 read it; s3's VACUUM deletes the empty entry and frees its
#     root, and s2's insert then makes a new entry on the leaf.

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE TABLE sm (id int, k int NOT NULL);
	INSERT INTO sm SELECT g, g * 10 FROM generate_series(1, 2000) g;
	CREATE INDEX sm_k ON sm USING lion (k) WITH (inline_limit = 64);
	INSERT INTO sm SELECT g, 3000 FROM generate_series(2001, 2200) g;
}
setup { VACUUM ANALYZE sm; }
teardown
{
	DROP TABLE sm;
}

session s1
setup			{ SET enable_seqscan = off; }
step s1_begin	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1_read	{ SELECT count(*) FROM sm WHERE k = 5000; }
step s1_read3	{ SELECT count(*) FROM sm WHERE k = 3000; }
step s1_insert	{ INSERT INTO sm VALUES (-1, 15000); }
step s1_commit	{ COMMIT; }

session s2
setup			{ SET enable_seqscan = off; }
step s2_begin	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2_read	{ SELECT count(*) FROM sm WHERE k = 15000; }
step s2_insert	{ INSERT INTO sm VALUES (-2, 5000); }
step s2_insert3	{ INSERT INTO sm VALUES (-2, 3000); }
step s2_commit	{ COMMIT; }

session s3
step s3_spill	{ INSERT INTO sm SELECT -g, 5000 FROM generate_series(1, 200) g; }
step s3_split	{ INSERT INTO sm SELECT -g, 5000 - g FROM generate_series(1, 4999) g
				  WHERE g % 10 <> 0; }
step s3_delete	{ DELETE FROM sm WHERE k = 3000; }
step s3_vacuum	{ VACUUM sm; }
step s3_root	{ SELECT lion_index_posting_root('sm_k', 5000) IS NOT NULL AS chain; }
step s3_root3	{ SELECT lion_index_posting_root('sm_k', 3000) IS NOT NULL AS chain,
				    lion_index_count('sm_k', 3000) AS rows; }

permutation s1_begin s2_begin s1_read s2_read s3_root s3_spill s3_root s1_insert s2_insert s1_commit s2_commit
permutation s1_begin s2_begin s1_read s2_read s3_split s1_insert s2_insert s1_commit s2_commit
permutation s3_root3 s3_delete s1_begin s2_begin s1_read3 s2_read s3_vacuum s3_root3 s1_insert s2_insert3 s1_commit s2_commit
