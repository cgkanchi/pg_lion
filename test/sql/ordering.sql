-- The directory order is the INDEX's, not the catalog's (DESIGN.md §21).
--
-- Whether a key column's directory is in its comparison's order, and which
-- comparison that is, is decided when the index is BUILT and recorded on its
-- meta page.  A catalog change that would decide it differently today - a
-- btree opclass that makes a lion opclass's proc 4 sortable, the same one
-- dropped again, a family's proc 4 swapped for another function - must not
-- change how an existing directory is read or written: the order is the one
-- its entries are in.  Every answer is checked against a sequential scan.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
RESET client_min_messages;
SET synchronous_commit = on;

CREATE FUNCTION lion_ocheck(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	viaidx bigint;
	viaseq bigint;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	EXECUTE q INTO viaidx;
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	EXECUTE q INTO viaseq;			-- the pushdown, where it applies
	IF viaseq <> viaidx THEN
		RETURN format('MISMATCH: bitmap %s, pushdown %s', viaidx, viaseq);
	END IF;
	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	EXECUTE q INTO viaseq;
	PERFORM set_config('enable_bitmapscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_indexonlyscan', 'on', true);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	IF viaseq <> viaidx THEN
		RETURN format('MISMATCH: index %s, seqscan %s', viaidx, viaseq);
	END IF;
	RETURN format('%s rows, as the sequential scan', viaidx);
END $$;

/*
 * A lion opclass whose proc 4 is a function no btree family sorts with: the
 * directory of an index built with it is in HASH order (§21 rule 3).
 */
CREATE FUNCTION lion_ocmp(int4, int4) RETURNS int4 LANGUAGE sql IMMUTABLE STRICT
	AS 'SELECT btint4cmp($1, $2)';
CREATE OPERATOR FAMILY lion_ofam USING lion;
CREATE OPERATOR CLASS lion_oops FOR TYPE int4 USING lion FAMILY lion_ofam AS
	OPERATOR 1 = (int4, int4),
	OPERATOR 6 < (int4, int4),
	OPERATOR 7 <= (int4, int4),
	OPERATOR 8 >= (int4, int4),
	OPERATOR 9 > (int4, int4),
	FUNCTION 1 hashint4(int4),
	FUNCTION 4 lion_ocmp(int4, int4);

-- operators of our own, which a btree opclass over lion_ocmp can name
CREATE FUNCTION lion_olt(int4, int4) RETURNS bool LANGUAGE sql IMMUTABLE STRICT AS 'SELECT $1 < $2';
CREATE FUNCTION lion_ole(int4, int4) RETURNS bool LANGUAGE sql IMMUTABLE STRICT AS 'SELECT $1 <= $2';
CREATE FUNCTION lion_oeq(int4, int4) RETURNS bool LANGUAGE sql IMMUTABLE STRICT AS 'SELECT $1 = $2';
CREATE FUNCTION lion_oge(int4, int4) RETURNS bool LANGUAGE sql IMMUTABLE STRICT AS 'SELECT $1 >= $2';
CREATE FUNCTION lion_ogt(int4, int4) RETURNS bool LANGUAGE sql IMMUTABLE STRICT AS 'SELECT $1 > $2';
CREATE OPERATOR <<< (LEFTARG = int4, RIGHTARG = int4, FUNCTION = lion_olt);
CREATE OPERATOR <<= (LEFTARG = int4, RIGHTARG = int4, FUNCTION = lion_ole);
CREATE OPERATOR === (LEFTARG = int4, RIGHTARG = int4, FUNCTION = lion_oeq);
CREATE OPERATOR >>= (LEFTARG = int4, RIGHTARG = int4, FUNCTION = lion_oge);
CREATE OPERATOR >>> (LEFTARG = int4, RIGHTARG = int4, FUNCTION = lion_ogt);

CREATE TABLE lion_ord (id int, a int, b int);
INSERT INTO lion_ord SELECT i, i % 5000, i % 5000 FROM generate_series(1, 50000) i;

-- ---------- built unordered, then the catalog would call it ordered ----------
CREATE INDEX lion_ord_a ON lion_ord USING lion (a lion_oops);
VACUUM ANALYZE lion_ord;
SELECT ordered FROM lion_index_stats('lion_ord_a');
SELECT lion_ocheck('SELECT count(*) FROM lion_ord WHERE a BETWEEN 100 AND 2000');
SELECT lion_ocheck('SELECT count(*) FROM lion_ord WHERE a = 1234');

CREATE OPERATOR CLASS lion_obt FOR TYPE int4 USING btree AS
	OPERATOR 1 <<<,
	OPERATOR 2 <<=,
	OPERATOR 3 ===,
	OPERATOR 4 >>=,
	OPERATOR 5 >>>,
	FUNCTION 1 lion_ocmp(int4, int4);

-- a new backend builds the index's relcache entry against the new catalog
\c
\set VERBOSITY terse
SET synchronous_commit = on;
SELECT ordered FROM lion_index_stats('lion_ord_a');
SELECT lion_ocheck('SELECT count(*) FROM lion_ord WHERE a BETWEEN 100 AND 2000');
SELECT lion_ocheck('SELECT count(*) FROM lion_ord WHERE a = 1234');
SELECT lion_ocheck('SELECT count(*) FROM lion_ord WHERE a IN (7, 1234, 4999)');
-- the write path places new keys in the directory's own order too
INSERT INTO lion_ord SELECT i, 5000 + i % 700, 0 FROM generate_series(1, 7000) i;
SELECT lion_ocheck('SELECT count(*) FROM lion_ord WHERE a >= 4990');
SELECT lion_ocheck('SELECT count(*) FROM lion_ord WHERE a = 5555');
SELECT lion_index_verify('lion_ord_a', true);

-- ---------- built ordered, then the catalog would call it unordered ----------
CREATE INDEX lion_ord_b ON lion_ord USING lion (b lion_oops);
SELECT ordered FROM lion_index_stats('lion_ord_b');
DROP OPERATOR CLASS lion_obt USING btree;
\c
\set VERBOSITY terse
SET synchronous_commit = on;
SELECT ordered FROM lion_index_stats('lion_ord_a');
SELECT ordered FROM lion_index_stats('lion_ord_b');
SELECT lion_ocheck('SELECT count(*) FROM lion_ord WHERE b BETWEEN 100 AND 2000');
SELECT lion_ocheck('SELECT count(*) FROM lion_ord WHERE b = 1234');
INSERT INTO lion_ord SELECT i, 0, 6000 + i % 300 FROM generate_series(1, 3000) i;
SELECT lion_ocheck('SELECT count(*) FROM lion_ord WHERE b > 5990');
SELECT lion_ocheck('SELECT count(*) FROM lion_ord WHERE b = 6123');
SELECT lion_index_verify('lion_ord_b', true);
-- the comparison is known by what it runs, not by its name: a rename is the
-- same comparison, a new body is not
ALTER FUNCTION lion_ocmp(int4, int4) RENAME TO lion_ocmp_renamed;
\c
\set VERBOSITY terse
SELECT lion_ocheck('SELECT count(*) FROM lion_ord WHERE b BETWEEN 100 AND 2000');
ALTER FUNCTION lion_ocmp_renamed(int4, int4) RENAME TO lion_ocmp;
CREATE OR REPLACE FUNCTION lion_ocmp(int4, int4) RETURNS int4 LANGUAGE sql IMMUTABLE STRICT
	AS 'SELECT btint4cmp($2, $1)';
\c
\set VERBOSITY terse
SELECT count(*) FROM lion_ord WHERE b = 7;
CREATE OR REPLACE FUNCTION lion_ocmp(int4, int4) RETURNS int4 LANGUAGE sql IMMUTABLE STRICT
	AS 'SELECT btint4cmp($1, $2)';
\c
\set VERBOSITY terse
SET synchronous_commit = on;
SELECT lion_ocheck('SELECT count(*) FROM lion_ord WHERE b = 7');
-- REINDEX asks the catalog again
REINDEX INDEX lion_ord_b;
SELECT ordered FROM lion_index_stats('lion_ord_b');
SELECT lion_ocheck('SELECT count(*) FROM lion_ord WHERE b BETWEEN 100 AND 2000');
DROP TABLE lion_ord;

-- ---------- the comparison itself swapped under an ordered index ----------
-- A proc 4 added to the FAMILY after the class is a loose member, which can be
-- dropped and replaced by another function; the index was built in the old
-- one's order, and saying so is the only right answer.
CREATE FUNCTION lion_ocmp_rev(int4, int4) RETURNS int4 LANGUAGE sql IMMUTABLE STRICT
	AS 'SELECT btint4cmp($2, $1)';
CREATE OPERATOR FAMILY lion_ofam2 USING lion;
CREATE OPERATOR CLASS lion_oops2 FOR TYPE int4 USING lion FAMILY lion_ofam2 AS
	OPERATOR 1 = (int4, int4),
	FUNCTION 1 hashint4(int4);
ALTER OPERATOR FAMILY lion_ofam2 USING lion ADD FUNCTION 4 (int4, int4) btint4cmp(int4, int4);
CREATE TABLE lion_ord2 (id int, a int);
INSERT INTO lion_ord2 SELECT i, i % 500 FROM generate_series(1, 5000) i;
CREATE INDEX lion_ord2_a ON lion_ord2 USING lion (a lion_oops2);
SELECT ordered FROM lion_index_stats('lion_ord2_a');
ALTER OPERATOR FAMILY lion_ofam2 USING lion DROP FUNCTION 4 (int4, int4);
\c
\set VERBOSITY terse
-- without it the class borrows int4's own btint4cmp (§21 rule 2): the very
-- comparison the build used, so the directory reads as it always did
SELECT count(*) FROM lion_ord2 WHERE a = 7;
ALTER OPERATOR FAMILY lion_ofam2 USING lion ADD FUNCTION 4 (int4, int4) lion_ocmp_rev(int4, int4);
\c
\set VERBOSITY terse
-- another one in its place: the directory is not in ITS order
SELECT count(*) FROM lion_ord2 WHERE a = 7;
REINDEX INDEX lion_ord2_a;
SELECT ordered FROM lion_index_stats('lion_ord2_a');
SET enable_seqscan = off;
SELECT count(*) FROM lion_ord2 WHERE a = 7;
RESET enable_seqscan;
DROP TABLE lion_ord2;

-- ---------- a relocatable extension moves its comparison ----------
-- citext_cmp moves with citext; the citext index was built in its order and
-- is still in it, wherever the function lives now
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS citext;
CREATE EXTENSION IF NOT EXISTS pg_lion_citext;
RESET client_min_messages;
SELECT n.nspname AS citext_schema FROM pg_extension e JOIN pg_namespace n ON n.oid = e.extnamespace
 WHERE e.extname = 'citext' \gset
CREATE TABLE lion_ordci (id int, n citext);
INSERT INTO lion_ordci SELECT i, CASE WHEN i % 2 = 0 THEN upper(chr(97 + i % 26)) ELSE chr(97 + i % 26) END || (i % 50)
  FROM generate_series(1, 5000) i;
CREATE INDEX lion_ordci_n ON lion_ordci USING lion (n);
SELECT ordered FROM lion_index_stats('lion_ordci_n');
CREATE SCHEMA lion_ordmoved;
ALTER EXTENSION citext SET SCHEMA lion_ordmoved;
\c
\set VERBOSITY terse
SET synchronous_commit = on;
SET search_path = public, lion_ordmoved;
SELECT ordered FROM lion_index_stats('lion_ordci_n');
SELECT lion_ocheck('SELECT count(*) FROM lion_ordci WHERE n = ''m12''');
SELECT lion_ocheck('SELECT count(*) FROM lion_ordci WHERE n BETWEEN ''c'' AND ''K3''');
SELECT lion_ocheck('SELECT count(*) FROM lion_ordci WHERE n > ''x''');
INSERT INTO lion_ordci SELECT i, 'Q' || i FROM generate_series(1, 300) i;
SELECT lion_ocheck('SELECT count(*) FROM lion_ordci WHERE n >= ''q''');
SELECT lion_index_verify('lion_ordci_n', true);
DROP TABLE lion_ordci;
ALTER EXTENSION citext SET SCHEMA :"citext_schema";
RESET search_path;
DROP SCHEMA lion_ordmoved;

DROP OPERATOR FAMILY lion_ofam2 USING lion;
DROP OPERATOR FAMILY lion_ofam USING lion;
DROP OPERATOR <<< (int4, int4);
DROP OPERATOR <<= (int4, int4);
DROP OPERATOR === (int4, int4);
DROP OPERATOR >>= (int4, int4);
DROP OPERATOR >>> (int4, int4);
DROP FUNCTION lion_olt(int4, int4), lion_ole(int4, int4), lion_oeq(int4, int4),
	lion_oge(int4, int4), lion_ogt(int4, int4);
DROP FUNCTION lion_ocmp(int4, int4), lion_ocmp_rev(int4, int4);
DROP FUNCTION lion_ocheck(text);
