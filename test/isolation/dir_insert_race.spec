# Two sessions creating the SAME new key at once, when the run of entries
# whose prefix ties with it spans more than one directory leaf (DESIGN.md §21,
# "Insert of a new entry").
#
# The directory holds exactly one entry per equality class, and the insert
# path's find-or-create is what keeps it that way: look the key up, and if it
# is absent, insert it.  When the lookup stays on one leaf that leaf is held
# EXCLUSIVE from the unsuccessful lookup to the insert, so a second writer of
# the same key waits for it and then finds the entry.  When the prefix run
# crosses a page boundary the lookup ends on a leaf further right than the
# one the key belongs on, and the insert used to give that leaf up and descend
# again - with nothing held in between, so a second writer could run its own
# lookup there, miss as well, and both would insert: two entries for one key,
# one of which no lookup ever returns.
#
# The fixture makes every entry share one prefix run: an UNORDERED text class
# (its equality is not text's default one, so §21 rule 2 borrows no ordering)
# whose hash is a constant, so every key collides with every other and the run
# is the whole column, several leaves long.
#
# The test:
#
#  * s1 inserts a brand new key and parks at 'lion-dir-add-entry-spanning',
#    which fires between its unsuccessful lookup and its insert.
#  * s2 inserts a row with the same key.  With the fix it waits for s1 (a
#    buffer lock, which isolationtester cannot see - hence the (*) marker);
#    without it, it inserts the entry itself.
#  * s3 releases s1.
#
# One entry for the key, both rows counted, verify() clean.

setup
{
	SET synchronous_commit = on;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE FUNCTION dir_race_eq(text, text) RETURNS boolean
		LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE AS 'texteq';
	CREATE FUNCTION dir_race_hash(text) RETURNS integer
		LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE AS $$ SELECT 42 $$;
	CREATE OPERATOR === (LEFTARG = text, RIGHTARG = text,
						 FUNCTION = dir_race_eq, COMMUTATOR = ===);
	CREATE OPERATOR CLASS dir_race_ops FOR TYPE text USING lion AS
		OPERATOR 1 === (text, text),
		FUNCTION 1 dir_race_hash(text);
	CREATE TABLE dir_race (id int, k text NOT NULL);
	INSERT INTO dir_race
	SELECT i, 'key-' || lpad((2 * i)::text, 120, '0')
	  FROM generate_series(1, 300) i;
	CREATE INDEX dir_race_k ON dir_race USING lion (k dir_race_ops);
}

teardown
{
	DROP TABLE dir_race;
	DROP OPERATOR CLASS dir_race_ops USING lion;
	DROP OPERATOR === (text, text);
	DROP FUNCTION dir_race_eq(text, text);
	DROP FUNCTION dir_race_hash(text);
	/* Injection points are cluster-wide: never leave one attached, and
	 * never fail the teardown because the permutation already did. */
	DO $$ BEGIN PERFORM injection_points_detach('lion-dir-add-entry-spanning');
	   EXCEPTION WHEN OTHERS THEN NULL; END $$;
}

session s1
setup
{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('lion-dir-add-entry-spanning', 'wait');
}
step s1_ins	{ INSERT INTO dir_race VALUES (1001, 'key-' || lpad('301', 120, '0')); }
# Runs once s1_ins is done (a session runs one step at a time), and first waits
# for s2's insert to be done as well, so that both rows are in.
step s1_check	{
	DO $$
	BEGIN
		FOR i IN 1 .. 3000 LOOP
			EXIT WHEN NOT EXISTS (SELECT FROM pg_stat_activity
								   WHERE pid <> pg_backend_pid()
									 AND datname = current_database()
									 AND state <> 'idle'
									 AND query LIKE 'INSERT INTO dir_race VALUES (1002%');
			PERFORM pg_sleep(0.01);
		END LOOP;
	END $$;
	SELECT entries,
		   lion_index_count('dir_race_k', 'key-' || lpad('301', 120, '0')) AS new_key_rows,
		   lion_index_count('dir_race_k', 'key-' || lpad('300', 120, '0')) AS old_key_rows
	  FROM lion_index_stats('dir_race_k');
}

session s2
step s2_ins	{ INSERT INTO dir_race VALUES (1002, 'key-' || lpad('301', 120, '0')); }

session s3
step s3_shape	{
	SELECT ordered, leaf_pages > 2 AS run_spans_leaves, entries
	  FROM lion_index_stats('dir_race_k');
}
# Report what s2 is doing while s1 is parked - waiting for s1's leaf, or
# already done - then detach the point and release s1.
step s3_wakeup	{
	DO $$
	DECLARE st text;
	BEGIN
		FOR i IN 1 .. 3000 LOOP
			SELECT CASE WHEN state = 'idle' THEN 'finished'
						WHEN wait_event_type IN ('Buffer', 'LWLock')
						THEN 'waiting for a directory leaf'
						ELSE NULL END INTO st
			  FROM pg_stat_activity
			 WHERE pid <> pg_backend_pid() AND datname = current_database()
			   AND query LIKE 'INSERT INTO dir_race VALUES (1002%';
			EXIT WHEN st IS NOT NULL;
			PERFORM pg_sleep(0.01);
		END LOOP;
		RAISE NOTICE 's2 is %', coalesce(st, 'unknown');
	END $$;
	SELECT injection_points_detach('lion-dir-add-entry-spanning');
	SELECT injection_points_wakeup('lion-dir-add-entry-spanning');
}
step s3_verify	{ SELECT lion_index_verify('dir_race_k', true); }

permutation
	s3_shape				# one prefix run, several leaves long
	s1_ins					# misses, parks before its insert
	s2_ins(*, s1_ins)		# the same key: must not create a second entry
	s3_wakeup				# s2 waits for s1's leaf; release s1
	s1_check(s2_ins)		# one entry, both rows
	s3_verify
