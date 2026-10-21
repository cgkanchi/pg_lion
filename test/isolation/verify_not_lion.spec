# lion_index_verify() of a relation that is not a lion index says so without
# locking its table (DESIGN.md §7).
#
# The check of a lion index locks the table, ShareUpdateExclusiveLock, and
# runs as the table's owner.  Whether the relation is a lion index at all is
# asked of its catalog row before either: s1 holds the lock VACUUM and DDL
# take on the table, which conflicts with the check's, and the check of the
# table's btree index - and of the table itself - must be refused at once
# rather than wait for s1.  The check of the table's lion index waits, as it
# has to.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE TABLE vnl (k int NOT NULL);
	INSERT INTO vnl SELECT g % 5 FROM generate_series(1, 100) g;
	CREATE INDEX vnl_bt ON vnl (k);
	CREATE INDEX vnl_lion ON vnl USING lion (k);
}

teardown
{
	DROP TABLE vnl;
}

session s1
step s1_lock	{ BEGIN; LOCK TABLE vnl IN SHARE UPDATE EXCLUSIVE MODE; }
step s1_commit	{ COMMIT; }

session s2
setup			{ SET lock_timeout = '10s'; }
step s2_btree	{ SELECT lion_index_verify('vnl_bt'); }
step s2_table	{ SELECT lion_index_verify('vnl'); }
step s2_lion	{ SELECT lion_index_verify('vnl_lion'); }

permutation s1_lock s2_btree s2_table s2_lion s1_commit
