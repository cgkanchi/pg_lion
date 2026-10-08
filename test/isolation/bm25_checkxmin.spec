# Snapshot eligibility of lion_bm25(), as count_checkxmin.spec tests it for
# the count functions.
#
# There is no index on the table when s1 updates the row, so the update is
# HOT; s2 already holds a REPEATABLE READ snapshot, so the CREATE INDEX that
# follows finds a broken HOT chain, indexes only the new version and sets
# indcheckxmin.  s2's snapshot still sees the old version, which is not in
# the index: before the fix, ranking 'old' returned nothing for it while an
# ordinary query found the row, and ranking 'new' returned the row the
# snapshot sees as 'old'.  Now lion_bm25() refuses, and a snapshot taken
# after the build ranks normally.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE TABLE bm_cx (id int, d tsvector);
	INSERT INTO bm_cx VALUES (1, to_tsvector('simple', 'old doc'));
}

teardown
{
	DROP TABLE bm_cx;
}

session s1
step s1_update	{ UPDATE bm_cx SET d = to_tsvector('simple', 'new doc'); }
step s1_index	{ CREATE INDEX bm_cx_d ON bm_cx USING lion (d) WITH (store_positions = true); }
step s1_check	{ SELECT indisvalid, indisready, indcheckxmin
					FROM pg_index WHERE indexrelid = 'bm_cx_d'::regclass; }

session s2
step s2_begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s2_snap	{ SELECT id, d FROM bm_cx; }
step s2_sp		{ SAVEPOINT p; }
step s2_back	{ ROLLBACK TO p; }
step s2_plain	{ SELECT id FROM bm_cx WHERE d @@ 'old'; }
step s2_old		{ SELECT t.id FROM lion_bm25('bm_cx_d', 'old', 10) s JOIN bm_cx t ON t.ctid = s.ctid; }
step s2_new		{ SELECT t.id FROM lion_bm25('bm_cx_d', 'new', 10) s JOIN bm_cx t ON t.ctid = s.ctid; }
step s2_commit	{ COMMIT; }

session s3
step s3_new		{ SELECT t.id, t.d FROM lion_bm25('bm_cx_d', 'new', 10) s JOIN bm_cx t ON t.ctid = s.ctid; }

permutation s2_begin s2_snap s1_update s1_index s1_check s2_plain
			s2_sp s2_old s2_back s2_new s2_back s2_commit s3_new
