# Snapshot semantics of lion_index_facets() (DESIGN.md §7, "Facet counts").
#
# Every step reports the facets next to the query they stand for, run in the
# same statement and so under the same snapshot: they must always be equal.
# The absolute values show the snapshot behaviour: the REPEATABLE READ reader
# s1 keeps seeing the table as it was when it took its snapshot, the writer
# s2 sees its own uncommitted changes, and the READ COMMITTED reader s3 sees
# each committed change.  Nothing is vacuumed, so the index still holds the
# TIDs of every version.
#
# The planner's lion paths are off in every session, so the reference
# queries are not answered by the code under test.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE TABLE fsnap (id int, grp int, tags text[], tsv tsvector)
		WITH (autovacuum_enabled = off);
	INSERT INTO fsnap SELECT i, i % 2, ARRAY['t' || (i % 5), 'u' || (i % 3)],
		to_tsvector('simple', 'w' || (i % 4) || ' v' || (i % 7))
	  FROM generate_series(1, 400) i;
	CREATE INDEX fsnap_grp ON fsnap USING lion (grp);
	CREATE INDEX fsnap_tags ON fsnap USING lion (tags);
	CREATE INDEX fsnap_tsv ON fsnap USING lion (tsv);
	CREATE FUNCTION fsnap_both(OUT tags text, OUT query text, OUT lexemes text, OUT ts_stat text)
	LANGUAGE sql AS $f$
	SELECT
	(SELECT string_agg(key || ':' || count, ' ' ORDER BY r)
	   FROM lion_index_facets('fsnap_tags', 4, 'grp = 1') WITH ORDINALITY f(key, count, r)),
	(SELECT string_agg(e || ':' || n, ' ' ORDER BY n DESC, e)
	   FROM (SELECT e, count(DISTINCT id) AS n FROM fsnap, unnest(tags) e WHERE grp = 1
			  GROUP BY e ORDER BY 2 DESC, 1 LIMIT 4) x),
	(SELECT string_agg(key || ':' || count, ' ' ORDER BY r)
	   FROM lion_index_facets('fsnap_tsv', NULL, $$tags @> ARRAY['t1']$$) WITH ORDINALITY f(key, count, r)),
	(SELECT string_agg(word || ':' || ndoc, ' ' ORDER BY ndoc DESC, word COLLATE "C")
	   FROM ts_stat($$SELECT tsv FROM fsnap WHERE tags @> ARRAY['t1']$$))
	$f$;
}

teardown
{
	DROP FUNCTION fsnap_both();
	DROP TABLE fsnap;
}

# The long-lived REPEATABLE READ reader.
session s1
setup			{ SET enable_indexscan = off; SET enable_bitmapscan = off;
				  SET pg_lion.enable_count_pushdown = off; }
step s1_begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1_facets	{ SELECT tags = query AS tags_same, lexemes = ts_stat AS lexemes_same, tags, lexemes FROM fsnap_both(); }
step s1_commit	{ COMMIT; }

# The writer: an insert, an update that moves rows to other keys, a delete.
session s2
setup			{ SET enable_indexscan = off; SET enable_bitmapscan = off;
				  SET pg_lion.enable_count_pushdown = off; }
step s2_begin	{ BEGIN; }
step s2_write	{ INSERT INTO fsnap SELECT 1000 + i, 1, ARRAY['t1', 'hot'], 'w9 v9'
					FROM generate_series(1, 60) i;
				  UPDATE fsnap SET tags = ARRAY['t1', 'moved'], tsv = 'w8', grp = 1 WHERE id % 10 = 0;
				  DELETE FROM fsnap WHERE id % 5 = 3; }
step s2_facets	{ SELECT tags = query AS tags_same, lexemes = ts_stat AS lexemes_same, tags, lexemes FROM fsnap_both(); }
step s2_commit	{ COMMIT; }
step s2_rollback	{ ROLLBACK; }

# A reader that takes a fresh snapshot for every statement.
session s3
setup			{ SET enable_indexscan = off; SET enable_bitmapscan = off;
				  SET pg_lion.enable_count_pushdown = off; }
step s3_facets	{ SELECT tags = query AS tags_same, lexemes = ts_stat AS lexemes_same, tags, lexemes FROM fsnap_both(); }

# Uncommitted changes are seen by their writer only; once committed, by a new
# snapshot and never by s1's.
permutation s1_begin s1_facets s2_begin s2_write s2_facets s1_facets s3_facets
			s2_commit s1_facets s3_facets s1_commit s1_facets

# Rolled back, they are seen by no one, though the index holds their TIDs.
permutation s2_begin s2_write s2_facets s3_facets s2_rollback s3_facets
