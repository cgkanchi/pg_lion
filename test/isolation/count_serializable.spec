# Two SERIALIZABLE transactions each count an absent key through the roaring
# count paths, then each insert that key.  Reading the index takes a
# relation-level predicate lock (as index_beginscan() would for an AM without
# ampredlocks), so the second commit must fail with a serialization failure,
# exactly as it does for the ordinary plan.
setup
{
    CREATE EXTENSION IF NOT EXISTS pg_lion;
    CREATE TABLE ser (k int NOT NULL);
    INSERT INTO ser SELECT g % 10 FROM generate_series(1, 1000) g;
    CREATE INDEX ser_k ON ser USING lion (k);
}
teardown
{
    DROP TABLE ser;
}

session s1
setup           { SET enable_seqscan = off; SET enable_bitmapscan = off; }
step s1_begin   { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1_func    { SELECT lion_index_count('ser_k', 99); }
step s1_plan    { EXPLAIN (COSTS OFF) SELECT count(*) FROM ser WHERE k = 99; }
step s1_push    { SELECT count(*) FROM ser WHERE k = 99; }
step s1_insert  { INSERT INTO ser VALUES (99); }
step s1_commit  { COMMIT; }

session s2
setup           { SET enable_seqscan = off; SET enable_bitmapscan = off; }
step s2_begin   { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2_func    { SELECT lion_index_count('ser_k', 99); }
step s2_push    { SELECT count(*) FROM ser WHERE k = 99; }
step s2_insert  { INSERT INTO ser VALUES (99); }
step s2_commit  { COMMIT; }

# SQL function path
permutation s1_begin s2_begin s1_func s2_func s1_insert s2_insert s1_commit s2_commit
# LionCount pushdown path
permutation s1_begin s2_begin s1_plan s1_push s2_push s1_insert s2_insert s1_commit s2_commit
