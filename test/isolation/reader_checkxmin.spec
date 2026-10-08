# The contract every reader handed an index by name keeps on snapshot
# eligibility (DESIGN.md §9, "Direct readers"; lion_reader_vet()).  The
# indexes are built over a broken HOT chain while s2 holds an older snapshot,
# so they carry indcheckxmin and hold only the new version of the row: every
# such reader must refuse them for s2, as the planner would, and serve a
# snapshot taken after the build.  A new reader gets a step here.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE TABLE rx (k int, d tsvector);
	INSERT INTO rx VALUES (1, to_tsvector('simple', 'old'));
}
teardown
{
	DROP TABLE rx;
}

session s1
step s1_update	{ UPDATE rx SET k = 2, d = to_tsvector('simple', 'new'); }
step s1_index	{ CREATE INDEX rx_k ON rx USING lion (k);
				  CREATE INDEX rx_d ON rx USING lion (d) WITH (store_positions = true); }
step s1_check	{ SELECT indexrelid::regclass, indcheckxmin FROM pg_index
					WHERE indrelid = 'rx'::regclass ORDER BY 1::text; }

session s2
step s2_begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s2_snap	{ SELECT k FROM rx; }
step s2_sp		{ SAVEPOINT p; }
step s2_back	{ ROLLBACK TO p; }
step s2_count	{ SELECT lion_index_count('rx_k', 1); }
step s2_count2	{ SELECT lion_index_count('rx_k', 1, 'rx_k', 1); }
step s2_any	{ SELECT lion_index_count_any('rx_k', ARRAY[1]); }
step s2_stats	{ SELECT count FROM lion_index_count_stats('rx_k', 1); }
step s2_groups	{ SELECT count(*) FROM lion_index_count_group_stats('rx_k'); }
step s2_bm25	{ SELECT count(*) FROM lion_bm25('rx_d', 'old', 10); }
step s2_commit	{ COMMIT; }

session s3
step s3_count	{ SELECT lion_index_count('rx_k', 2); }
step s3_count2	{ SELECT lion_index_count('rx_k', 2, 'rx_k', 2); }
step s3_any	{ SELECT lion_index_count_any('rx_k', ARRAY[2]); }
step s3_stats	{ SELECT count FROM lion_index_count_stats('rx_k', 2); }
step s3_groups	{ SELECT count(*) FROM lion_index_count_group_stats('rx_k'); }
step s3_bm25	{ SELECT count(*) FROM lion_bm25('rx_d', 'new', 10); }

permutation s2_begin s2_snap s1_update s1_index s1_check
			s2_sp s2_count s2_back s2_sp s2_count2 s2_back s2_sp s2_any s2_back s2_sp s2_stats s2_back s2_sp s2_groups s2_back s2_sp s2_bm25 s2_back
			s2_commit s3_count s3_count2 s3_any s3_stats s3_groups s3_bm25
