-- pgbench script: phase 5's write load (run.sh) on lv_rec, a table whose
-- vectors lie in TOAST under a lionvec index that keeps a locator (DESIGN.md
-- §44.13).  One statement a transaction, so no two clients deadlock: a new
-- row; an UPDATE of another column (HOT where the page has room) or of the
-- lion-indexed one (not HOT); the vector set anew, or to itself; a DELETE;
-- an insert rolled back; and INSERT ... SELECT of a row, which the heap
-- stores anew under another value id.  Each locates or leaves its rows'
-- entries, and the crash takes the server away in the middle of it.

\set r random(1, 10)
\set id random(1, 4000)
\if :r <= 3
INSERT INTO lv_rec SELECT n, n % 10, 'w', lv_rec_vec(n) FROM (SELECT nextval('lv_rec_seq') AS n) s;
\elif :r = 4
UPDATE lv_rec SET note = 'h' WHERE id = :id;
\elif :r = 5
UPDATE lv_rec SET cat = (cat + 1) % 10 WHERE id = :id;
\elif :r = 6
UPDATE lv_rec SET v = lv_rec_vec(:id + 1000000) WHERE id = :id;
\elif :r = 7
DELETE FROM lv_rec WHERE id = :id;
\elif :r = 8
BEGIN;
INSERT INTO lv_rec SELECT n, n % 10, 'r', lv_rec_vec(n) FROM (SELECT nextval('lv_rec_seq') AS n) s;
ROLLBACK;
\elif :r = 9
INSERT INTO lv_rec SELECT nextval('lv_rec_seq'), cat, 'c', v FROM lv_rec WHERE id = :id;
\else
UPDATE lv_rec SET v = v WHERE id = :id;
\endif
