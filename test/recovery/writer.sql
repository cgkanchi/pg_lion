-- pgbench script: the mixed write load the crash phase of run.sh interrupts.
-- Run with several clients; each client c only touches rows with id % 8 = c,
-- so no two clients ever contend for a row (no deadlock, no serialization
-- failure, nothing that would abort a pgbench client and fail the run).
--
-- One transaction: three inserts of brand new rows, a non-HOT update of every
-- indexed column of the first of them (so every index gets a new TID and loses
-- an old one), an update of a small primary-key range of this client's rows,
-- and a delete of this client's three lowest-numbered rows.  Three in, three
-- out: the table stays the same size however long the writer runs, while the
-- live id range slides upwards - which is what makes VACUUM free low heap
-- pages that later inserts reuse, so container keys arrive out of order and
-- the rarer placement paths (a pair landing inside a full sparse segment, a
-- container inserted in the middle of a chain page) get exercised.
--
-- Note where the sequence is read: nextval() is VOLATILE, so in a WHERE clause
-- the planner cannot use it as an index qual and evaluates it once per scanned
-- row.  Writing "WHERE id = 8 * nextval(...)" therefore sequentially scanned
-- the table, called nextval 40000 times and deleted nothing - 265 ms per
-- statement and 12 transactions per iteration.  Hence \gset: every id below is
-- a literal by the time the server sees it.
--
-- synchronous_commit comes from PGOPTIONS in run.sh: the crash tests are about
-- durability, so every commit these transactions report must be on disk before
-- pg_ctl -m immediate or kill -9 takes the server away.

\set u random(1, 200000)

BEGIN;

SELECT nextval('rbi_rec_ins') AS i1,
	   nextval('rbi_rec_ins') AS i2,
	   nextval('rbi_rec_ins') AS i3 \gset

INSERT INTO rbi_rec SELECT g.* FROM rbi_rec_gen(:client_id + 8 * :i1) g;

-- Every indexed column of the new row changes, including the array and the
-- tsvector, so the multi-key entries and both reserved entries move too.
UPDATE rbi_rec
   SET k4  = (k4 + 7) % 200,
	   t   = 'v' || ((k4 * 7 + 11) % 3000),
	   ct  = ('Mix' || ((k4 + 3) % 40))::citext,
	   b   = NOT b,
	   nn  = CASE WHEN nn IS NULL THEN (k4 % 30) ELSE NULL END,
	   arr = CASE WHEN arr IS NULL THEN ARRAY[(k4 % 17)]
				  WHEN cardinality(arr) = 0 THEN NULL
				  ELSE arr || (k4 % 97) END,
	   tsv = CASE WHEN tsv IS NULL THEN to_tsvector('simple', 'w' || (k4 % 41))
				  WHEN tsv = ''::tsvector THEN NULL
				  ELSE tsv || to_tsvector('simple', 'w' || (k4 % 41)) END
 WHERE id = :client_id + 8 * :i1;

INSERT INTO rbi_rec SELECT g.* FROM rbi_rec_gen(:client_id + 8 * :i2) g;
INSERT INTO rbi_rec SELECT g.* FROM rbi_rec_gen(:client_id + 8 * :i3) g;

-- A short primary-key range, so the cost is bounded even when the range is
-- full of index entries for rows that have already been deleted.  Roughly half
-- the windows are empty; the update above is the one that churns every index
-- on every transaction.
UPDATE rbi_rec
   SET b = NOT b,
	   t = 'v' || ((k4 * 13 + 5) % 3000),
	   nn = CASE WHEN nn IS NULL THEN 0 ELSE (nn + 1) % 30 END
 WHERE id BETWEEN :u AND :u + 64 AND id % 8 = :client_id;

-- Exactly three rows, and always rows that exist: an ordered primary-key scan
-- from the low end, which needs no bookkeeping that a crash could desynchronise
-- from the table.
DELETE FROM rbi_rec
 WHERE id IN (SELECT id FROM rbi_rec
			   WHERE id % 8 = :client_id ORDER BY id LIMIT 3);

END;
