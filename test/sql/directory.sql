-- The sorted key directory (DESIGN.md §21).
--
-- Everything here is about the entry directory being a B-tree keyed by the
-- index key instead of a fixed set of hash buckets: that it grows by
-- splitting (at every level, root splits included), that an ordered opclass
-- makes the entry scan - and therefore a GROUP BY - come out in key order,
-- that an opclass whose comparison is coarser than byte equality still keeps
-- one entry per equality class, and that a sorted IN list costs one pass over
-- the leaves its values live on.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
SET synchronous_commit = on;

CREATE EXTENSION IF NOT EXISTS pg_lion;

-- ---------------------------------------------------------------------
-- 1. An index created on an EMPTY table and grown by inserts.
--
-- This is the case the hash directory could not answer: it kept the 64
-- buckets it was built with for ever and every lookup walked a chain of a
-- dozen pages.  The tree splits instead, so the height grows and the lookups
-- stay logarithmic.
-- ---------------------------------------------------------------------

CREATE TABLE lion_dir (id int, k text NOT NULL);
CREATE INDEX lion_dir_k ON lion_dir USING lion (k);

-- An empty index is one leaf, which is also the root.
SELECT directory_height, leaf_pages, internal_pages, ordered, entries
	FROM lion_index_stats('lion_dir_k');

INSERT INTO lion_dir SELECT i, 'key' || lpad(i::text, 8, '0')
	FROM generate_series(1, 50000) i;
SELECT lion_index_verify('lion_dir_k');
SELECT directory_height > 0 AS has_internal_pages,
	   internal_pages > 0 AS internal_pages_exist,
	   entries = 50000 AS all_keys
	FROM lion_index_stats('lion_dir_k');

INSERT INTO lion_dir SELECT i, 'key' || lpad(i::text, 8, '0')
	FROM generate_series(50001, 100000) i;
SELECT lion_index_verify('lion_dir_k');

-- Descending keys as well, so that splits happen on the LEFT of the tree and
-- not only by appending to the rightmost page.
INSERT INTO lion_dir SELECT i, 'key' || lpad(i::text, 8, '0')
	FROM generate_series(150000, 100001, -1) i;
SELECT lion_index_verify('lion_dir_k');

-- ... and in random order, which is what really exercises the split point.
INSERT INTO lion_dir SELECT i, 'key' || lpad(i::text, 8, '0')
	FROM generate_series(150001, 200000) i ORDER BY hashint4(i);
SELECT lion_index_verify('lion_dir_k', true);

SELECT entries = 200000 AS all_keys,
	   directory_height >= 2 AS root_split_happened,
	   leaf_pages > 100 AS many_leaves,
	   internal_pages > 1 AS many_internal_pages
	FROM lion_index_stats('lion_dir_k');

-- Every key is findable, the first and the last included.
SELECT count(*) FROM lion_dir WHERE k = 'key00000001';
SELECT count(*) FROM lion_dir WHERE k = 'key00200000';
SELECT count(*) FROM lion_dir WHERE k = 'key00123456';
SELECT count(*) FROM lion_dir WHERE k = 'nosuchkey';

-- A REINDEX builds the same index from the same rows: same answers, and the
-- bulk-built tree is shallower than the one grown by inserts.
SELECT count(*) AS grown_rows FROM lion_dir;
REINDEX INDEX lion_dir_k;
SELECT lion_index_verify('lion_dir_k', true);
SELECT entries = 200000 AS all_keys FROM lion_index_stats('lion_dir_k');
SELECT count(*) FROM lion_dir WHERE k = 'key00123456';

DROP TABLE lion_dir;

-- ---------------------------------------------------------------------
-- 2. Ordered output: a GROUP BY driven by the index needs no Sort.
-- ---------------------------------------------------------------------

CREATE TABLE lion_ord (k int NOT NULL, nk int, v int);
INSERT INTO lion_ord
	SELECT i % 200, CASE WHEN i % 13 = 0 THEN NULL ELSE i % 200 END, i
	FROM generate_series(1, 100000) i;
CREATE INDEX lion_ord_k ON lion_ord USING lion (k);
CREATE INDEX lion_ord_nk ON lion_ord USING lion (nk);
VACUUM (ANALYZE) lion_ord;

SELECT ordered FROM lion_index_stats('lion_ord_k');

-- No Sort above the node: the groups already come out in key order.
EXPLAIN (COSTS OFF) SELECT k, count(*) FROM lion_ord GROUP BY k ORDER BY k;
SELECT k, count(*) FROM lion_ord GROUP BY k ORDER BY k LIMIT 3;

/*
 * ... and the node's own output really is in key order, with nothing above it
 * asking for one.  The check runs the query through EXECUTE so that it is
 * planned exactly as it is written - a GROUP BY buried in a subquery is
 * flattened into a HashAggregate over the heap, which would prove nothing at
 * all about the directory.
 */
CREATE FUNCTION lion_groups_monotonic(q text, rising bool) RETURNS boolean
	LANGUAGE plpgsql AS $$
DECLARE
	r record;
	prev bigint := NULL;
BEGIN
	FOR r IN EXECUTE q LOOP
		IF prev IS NOT NULL THEN
			IF rising AND r.k <= prev THEN
				RETURN false;
			END IF;
			IF NOT rising AND r.k >= prev THEN
				RETURN false;
			END IF;
		END IF;
		prev := r.k;
	END LOOP;
	RETURN true;
END $$;

EXPLAIN (COSTS OFF) SELECT k, count(*) FROM lion_ord GROUP BY k;
SELECT lion_groups_monotonic('SELECT k, count(*) FROM lion_ord GROUP BY k',
							 true) AS groups_in_key_order;

-- A NULLABLE group column keeps its Sort: the reserved NULL entry sorts
-- FIRST and `ORDER BY nk` means NULLS LAST.
EXPLAIN (COSTS OFF) SELECT nk, count(*) FROM lion_ord GROUP BY nk ORDER BY nk;

-- DESC needs a Sort too.
EXPLAIN (COSTS OFF) SELECT k, count(*) FROM lion_ord GROUP BY k ORDER BY k DESC;

DROP TABLE lion_ord;

-- ---------------------------------------------------------------------
-- 3. A type with no btree opclass at all: xid.  The directory is ordered by
-- (hash, stored bytes), which is a complete order but not the type's, so
-- ordered = false and nothing may claim the output is sorted.
-- ---------------------------------------------------------------------

CREATE TABLE lion_unord (x xid NOT NULL, v int);
INSERT INTO lion_unord SELECT (i % 50)::text::xid, i
	FROM generate_series(1, 20000) i;
CREATE INDEX lion_unord_x ON lion_unord USING lion (x);
VACUUM (ANALYZE) lion_unord;

SELECT ordered FROM lion_index_stats('lion_unord_x');
SELECT lion_index_verify('lion_unord_x', true);

-- The counts are right whatever the order.
SELECT count(*) FROM lion_unord WHERE x = '7'::xid;
SELECT sum(c) AS total, count(*) AS groups
	FROM (SELECT x, count(*) AS c FROM lion_unord GROUP BY x) g;

-- ... but the plan keeps its Sort, because the entry order is not xid's.
EXPLAIN (COSTS OFF)
	SELECT x, count(*) FROM lion_unord GROUP BY x ORDER BY x::text;

DROP TABLE lion_unord;

-- ---------------------------------------------------------------------
-- 4. An IN list of 1000 sorted values costs one pass over the leaves.
--
-- "Directory Pages Read" is the leaves and internal pages the node read.  A
-- thousand values located in one left-to-right walk may read each leaf they
-- live on once, plus the descents it takes to get there; a thousand separate
-- descents would be several times that.
-- ---------------------------------------------------------------------

CREATE TABLE lion_in (k int NOT NULL, v int);
INSERT INTO lion_in SELECT i % 20000, i FROM generate_series(1, 200000) i;
CREATE INDEX lion_in_k ON lion_in USING lion (k);
VACUUM (ANALYZE) lion_in;

CREATE FUNCTION lion_dirpages(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE
	j json;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, TIMING OFF, COSTS OFF, BUFFERS OFF, FORMAT JSON) '
		|| q INTO j;
	RETURN (j -> 0 -> 'Plan' ->> 'Directory Pages Read')::bigint;
END $$;

SELECT count(*) FROM lion_in WHERE k = ANY (ARRAY(SELECT generate_series(0, 999)));

SELECT lion_dirpages('SELECT count(*) FROM lion_in WHERE k = ANY (ARRAY(SELECT generate_series(0, 999)))')
	   <= (SELECT leaf_pages + directory_height + 2
		   FROM lion_index_stats('lion_in_k'))
	AS sorted_list_is_one_pass;

-- A single key is one descent: the root, its internal pages and the leaf.
SELECT lion_dirpages('SELECT count(*) FROM lion_in WHERE k = 1234')
	   <= (SELECT directory_height + 1 FROM lion_index_stats('lion_in_k'))
	AS single_key_is_one_descent;

DROP FUNCTION lion_dirpages(text);
DROP TABLE lion_in;

-- ---------------------------------------------------------------------
-- 5. fillfactor: 10 packs the leaves a tenth full, 100 completely.
-- ---------------------------------------------------------------------

CREATE TABLE lion_ff (k int NOT NULL);
INSERT INTO lion_ff SELECT i FROM generate_series(1, 50000) i;
CREATE INDEX lion_ff_10 ON lion_ff USING lion (k) WITH (fillfactor = 10);
CREATE INDEX lion_ff_100 ON lion_ff USING lion (k) WITH (fillfactor = 100);
SELECT lion_index_verify('lion_ff_10', true);
SELECT lion_index_verify('lion_ff_100', true);
SELECT (SELECT leaf_pages FROM lion_index_stats('lion_ff_10')) >
	   4 * (SELECT leaf_pages FROM lion_index_stats('lion_ff_100'))
	AS fillfactor_10_is_much_bigger;
SELECT count(*) FROM lion_ff WHERE k = 31337;
DROP TABLE lion_ff;

-- ---------------------------------------------------------------------
-- 6. `buckets` is accepted and ignored, with a NOTICE.
-- ---------------------------------------------------------------------

SET client_min_messages = notice;
CREATE TABLE lion_buckets (k int);
CREATE INDEX lion_buckets_k ON lion_buckets USING lion (k) WITH (buckets = 128);
SELECT reloptions FROM pg_class WHERE relname = 'lion_buckets_k';
-- and it changes nothing about the shape of the index
SELECT directory_height, leaf_pages, internal_pages
	FROM lion_index_stats('lion_buckets_k');
DROP TABLE lion_buckets;
SET client_min_messages = warning;

-- ---------------------------------------------------------------------
-- 7. The INTERNAL levels are packed independently of `fillfactor`.
--
-- fillfactor is about leaving room on the LEAVES for later inserts.  Applied
-- to an internal page it can leave one downlink on it, which makes every
-- level as large as the one below and the bottom-up build never reaches a
-- root at all (it ran until the statement timeout).  An internal page always
-- takes at least two downlinks, whatever the fill target says.
-- ---------------------------------------------------------------------

CREATE TABLE lion_ffi (k text NOT NULL);
INSERT INTO lion_ffi SELECT repeat('k', 500) || lpad(i::text, 6, '0')
	FROM generate_series(1, 3000) i;
CREATE INDEX lion_ffi_k ON lion_ffi USING lion (k) WITH (fillfactor = 10);
SELECT lion_index_verify('lion_ffi_k', true);
SELECT directory_height > 0 AS has_internal_levels,
	   internal_pages < leaf_pages AS levels_shrink,
	   entries = 3000 AS all_keys
	FROM lion_index_stats('lion_ffi_k');
SELECT count(*) FROM lion_ffi WHERE k = repeat('k', 500) || '001234';
DROP TABLE lion_ffi;

-- ---------------------------------------------------------------------
-- 8. A key much larger than the ones already on the page.
--
-- The high key a page is closed with is the first key of the page to its
-- right, and a LION_MAX_KEY_SIZE one arriving behind a page full of short
-- keys does not fit next to them.  nbtree's rule is that the item which
-- becomes the first of the next page supplies the high key, so trailing items
-- move right until the key fits; reserving for the incoming key alone
-- overflowed the page and the build failed outright.
-- ---------------------------------------------------------------------

CREATE TABLE lion_bigkey (k text NOT NULL);
INSERT INTO lion_bigkey SELECT 'k' || lpad(i::text, 7, '0')
	FROM generate_series(1, 12000) i;
-- one long key right behind a short one, at every distance from a page
-- boundary: the gaps between them are 1, 2, 3, ... keys
INSERT INTO lion_bigkey
	SELECT 'k' || lpad(i::text, 7, '0') || repeat('x', 1970)
	FROM generate_series(1, 150) t, LATERAL (SELECT (t * (t + 1) / 2) AS i) s;
CREATE INDEX lion_bigkey_k ON lion_bigkey USING lion (k);
SELECT lion_index_verify('lion_bigkey_k', true);
CREATE INDEX lion_bigkey_k100 ON lion_bigkey USING lion (k)
	WITH (fillfactor = 100);
SELECT lion_index_verify('lion_bigkey_k100', true);
SELECT count(*) FROM lion_bigkey WHERE k = 'k0000105' || repeat('x', 1970);
DROP TABLE lion_bigkey;

-- ---------------------------------------------------------------------
-- 9. An opclass whose equality is COARSER than the key type's.
--
-- Such a class holds one entry per equality class - 'A' and 'a' share one -
-- and the key type's own comparison separates the two spellings, so borrowing
-- it would put the entry where only one of them looks for it.  An opclass
-- without support proc 4 is therefore UNORDERED unless its equality IS the
-- key type's default btree equality, in which case borrowing is provably
-- consistent.
-- ---------------------------------------------------------------------

CREATE FUNCTION lion_dir_lower_eq(text, text) RETURNS boolean
	LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE
	AS $$ SELECT lower($1) = lower($2) $$;
CREATE OPERATOR ==== (LEFTARG = text, RIGHTARG = text,
					  FUNCTION = lion_dir_lower_eq, COMMUTATOR = ====);
CREATE FUNCTION lion_dir_lower_hash(text) RETURNS integer
	LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE
	AS $$ SELECT hashtext(lower($1)) $$;
CREATE OPERATOR CLASS lion_dir_lower_ops FOR TYPE text USING lion AS
	OPERATOR 1 ==== (text, text),
	FUNCTION 1 lion_dir_lower_hash(text);

CREATE TABLE lion_coarse (v text NOT NULL);
INSERT INTO lion_coarse SELECT 'A' FROM generate_series(1, 1000);
INSERT INTO lion_coarse SELECT 'a' FROM generate_series(1, 1000);
INSERT INTO lion_coarse SELECT 'B' || i FROM generate_series(1, 1000) i;
CREATE INDEX lion_coarse_v ON lion_coarse USING lion (v lion_dir_lower_ops);
-- one entry for the two spellings, and no ordering claimed for the class
SELECT ordered, entries = 1001 AS one_entry_per_class
	FROM lion_index_stats('lion_coarse_v');
-- BOTH spellings find that entry; the stored one used to be the only one that did
SELECT lion_index_count('lion_coarse_v', 'A'::text) AS stored_spelling,
	   lion_index_count('lion_coarse_v', 'a'::text) AS other_spelling;
SELECT lion_index_count_any('lion_coarse_v', ARRAY['a','A']::text[]) AS in_list;
SELECT count(*) FROM lion_coarse WHERE v ==== 'a';
SELECT lion_index_verify('lion_coarse_v', true);
DROP TABLE lion_coarse;

-- The same class WITHOUT the coarse equality: its strategy 1 is text's own
-- default btree equality, so borrowing bttextcmp is consistent and the index
-- is ordered even though the class names no support proc 4.
CREATE OPERATOR CLASS lion_dir_plain_ops FOR TYPE text USING lion AS
	OPERATOR 1 = (text, text),
	FUNCTION 1 hashtext(text);
CREATE TABLE lion_borrowed (v text NOT NULL);
INSERT INTO lion_borrowed SELECT 'v' || lpad(i::text, 6, '0')
	FROM generate_series(1, 2000) i;
CREATE INDEX lion_borrowed_v ON lion_borrowed
	USING lion (v lion_dir_plain_ops);
SELECT ordered, entries FROM lion_index_stats('lion_borrowed_v');
SELECT lion_index_verify('lion_borrowed_v', true);
DROP TABLE lion_borrowed;
DROP OPERATOR CLASS lion_dir_plain_ops USING lion;
DROP OPERATOR CLASS lion_dir_lower_ops USING lion;
DROP OPERATOR ==== (text, text);
DROP FUNCTION lion_dir_lower_eq(text, text);
DROP FUNCTION lion_dir_lower_hash(text);

-- ---------------------------------------------------------------------
-- 10. An opclass with a comparison of its OWN (support proc 4).
--
-- The build sorts with an OPERATOR and the directory descends with a
-- FUNCTION, and §21 needs them to be the same order exactly.  The operator is
-- therefore the `<` of the btree family that uses the very same comparison
-- support function, and not the key type's default `<`: sorting one way and
-- searching the other leaves every key where no search looks for it.
-- ---------------------------------------------------------------------

CREATE FUNCTION lion_rev_cmp(int4, int4) RETURNS int4
	LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE AS $$ SELECT btint4cmp($2, $1) $$;
CREATE FUNCTION lion_rev_lt(int4, int4) RETURNS boolean
	LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE AS $$ SELECT $1 > $2 $$;
CREATE FUNCTION lion_rev_le(int4, int4) RETURNS boolean
	LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE AS $$ SELECT $1 >= $2 $$;
CREATE FUNCTION lion_rev_eq(int4, int4) RETURNS boolean
	LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE AS $$ SELECT $1 = $2 $$;
CREATE FUNCTION lion_rev_ge(int4, int4) RETURNS boolean
	LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE AS $$ SELECT $1 <= $2 $$;
CREATE FUNCTION lion_rev_gt(int4, int4) RETURNS boolean
	LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE AS $$ SELECT $1 < $2 $$;
CREATE OPERATOR <# (LEFTARG = int4, RIGHTARG = int4, FUNCTION = lion_rev_lt,
					COMMUTATOR = >#, NEGATOR = >=#);
CREATE OPERATOR <=# (LEFTARG = int4, RIGHTARG = int4, FUNCTION = lion_rev_le,
					 COMMUTATOR = >=#, NEGATOR = >#);
CREATE OPERATOR =# (LEFTARG = int4, RIGHTARG = int4, FUNCTION = lion_rev_eq,
					COMMUTATOR = =#);
CREATE OPERATOR >=# (LEFTARG = int4, RIGHTARG = int4, FUNCTION = lion_rev_ge,
					 COMMUTATOR = <=#, NEGATOR = <#);
CREATE OPERATOR ># (LEFTARG = int4, RIGHTARG = int4, FUNCTION = lion_rev_gt,
					COMMUTATOR = <#, NEGATOR = <=#);
CREATE OPERATOR CLASS lion_rev_btree FOR TYPE int4 USING btree AS
	OPERATOR 1 <#, OPERATOR 2 <=#, OPERATOR 3 =#, OPERATOR 4 >=#, OPERATOR 5 >#,
	FUNCTION 1 lion_rev_cmp(int4, int4);
CREATE OPERATOR CLASS lion_rev_ops FOR TYPE int4 USING lion AS
	OPERATOR 1 = (int4, int4),
	FUNCTION 1 hashint4(int4),
	FUNCTION 4 lion_rev_cmp(int4, int4);

CREATE TABLE lion_rev (k int NOT NULL);
INSERT INTO lion_rev SELECT i % 500 FROM generate_series(1, 5000) i;
CREATE INDEX lion_rev_k ON lion_rev USING lion (k lion_rev_ops);
VACUUM (ANALYZE) lion_rev;
SELECT ordered, entries FROM lion_index_stats('lion_rev_k');
-- build order == search order: every key is where the descent looks for it
SELECT lion_index_verify('lion_rev_k', true);
SELECT count(*) AS keys_not_found FROM generate_series(0, 499) g
	WHERE lion_index_count('lion_rev_k', g) <> 10;
-- the groups come out in the OPCLASS's order, which is descending
EXPLAIN (COSTS OFF) SELECT k, count(*) FROM lion_rev GROUP BY k;
SELECT k, count(*) FROM lion_rev GROUP BY k LIMIT 3;
SELECT lion_groups_monotonic('SELECT k, count(*) FROM lion_rev GROUP BY k',
							 false) AS groups_in_opclass_order;
-- ... and that is not int4's own order, so an ORDER BY keeps its Sort
EXPLAIN (COSTS OFF) SELECT k, count(*) FROM lion_rev GROUP BY k ORDER BY k;
DROP TABLE lion_rev;

-- A comparison that belongs to no btree family at all cannot drive a
-- tuplesort, and an ordering the build cannot reproduce is no ordering: the
-- index is unordered instead, which is still correct.
CREATE FUNCTION lion_odd_cmp(int4, int4) RETURNS int4
	LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE AS $$ SELECT btint4cmp($2, $1) $$;
CREATE OPERATOR CLASS lion_odd_ops FOR TYPE int4 USING lion AS
	OPERATOR 1 = (int4, int4),
	FUNCTION 1 hashint4(int4),
	FUNCTION 4 lion_odd_cmp(int4, int4);
CREATE TABLE lion_odd (k int NOT NULL);
INSERT INTO lion_odd SELECT i % 500 FROM generate_series(1, 5000) i;
CREATE INDEX lion_odd_k ON lion_odd USING lion (k lion_odd_ops);
SELECT ordered, entries FROM lion_index_stats('lion_odd_k');
SELECT lion_index_verify('lion_odd_k', true);
SELECT count(*) AS keys_not_found FROM generate_series(0, 499) g
	WHERE lion_index_count('lion_odd_k', g) <> 10;
DROP TABLE lion_odd;
DROP OPERATOR CLASS lion_odd_ops USING lion;
DROP FUNCTION lion_odd_cmp(int4, int4);
DROP OPERATOR CLASS lion_rev_ops USING lion;
DROP OPERATOR CLASS lion_rev_btree USING btree;
DROP OPERATOR <# (int4, int4);
DROP OPERATOR <=# (int4, int4);
DROP OPERATOR =# (int4, int4);
DROP OPERATOR >=# (int4, int4);
DROP OPERATOR ># (int4, int4);
DROP FUNCTION lion_rev_cmp(int4, int4);
DROP FUNCTION lion_rev_lt(int4, int4);
DROP FUNCTION lion_rev_le(int4, int4);
DROP FUNCTION lion_rev_eq(int4, int4);
DROP FUNCTION lion_rev_ge(int4, int4);
DROP FUNCTION lion_rev_gt(int4, int4);

-- ---------------------------------------------------------------------
-- 11. Hash collisions under an UNORDERED opclass.
--
-- Without an ordering the entries of one hash are ordered by their stored
-- BYTES, and the sort only brings the hash together: the several distinct
-- keys inside one hash arrive interleaved and used to be written out in the
-- order their first TID happened to turn up.  The reserved NULL entry hashes
-- to 0 and shares its run with any key that hashes there, which used to split
-- it into several entries.
--
-- `xid` has no btree opclass at all, so a class over it is unordered whatever
-- its equality is; the hash here is deliberately coarse so that every value
-- collides with a great many others.
-- ---------------------------------------------------------------------

CREATE FUNCTION lion_coarse_xid_hash(xid) RETURNS integer
	LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE
	AS $$ SELECT (($1::text)::int8 % 7)::int4 $$;
CREATE OPERATOR CLASS lion_coarse_xid_ops FOR TYPE xid USING lion AS
	OPERATOR 1 = (xid, xid),
	FUNCTION 1 lion_coarse_xid_hash(xid);

CREATE TABLE lion_coll (x xid, v int);
INSERT INTO lion_coll
	SELECT CASE WHEN i % 11 = 0 THEN NULL ELSE (i % 300 + 1)::text::xid END, i
	FROM generate_series(1, 6000) i;
CREATE INDEX lion_coll_x ON lion_coll USING lion (x lion_coarse_xid_ops);
VACUUM (ANALYZE) lion_coll;
-- 300 keys and ONE reserved NULL entry, in the order the directory wants
SELECT ordered, entries FROM lion_index_stats('lion_coll_x');
SELECT lion_index_verify('lion_coll_x', true);
-- every key's posting set holds exactly the rows the heap has for it
SET pg_lion.enable_count_pushdown = off;
CREATE TEMP TABLE lion_coll_exp AS
	SELECT x, count(*) AS c FROM lion_coll WHERE x IS NOT NULL GROUP BY x;
RESET pg_lion.enable_count_pushdown;
SELECT count(*) AS keys, count(*) FILTER (
		WHERE lion_index_count('lion_coll_x', e.x) <> e.c) AS keys_wrong
	FROM lion_coll_exp e;
DROP TABLE lion_coll_exp;
-- the pushdown answers exactly what the heap does
SELECT count(*) FROM lion_coll WHERE x IS NULL;
SELECT count(*) FROM lion_coll WHERE x IS NOT NULL;
SELECT sum(c) AS total, count(*) AS groups
	FROM (SELECT x, count(*) AS c FROM lion_coll GROUP BY x) g;
SET pg_lion.enable_count_pushdown = off;
SELECT sum(c) AS total, count(*) AS groups
	FROM (SELECT x, count(*) AS c FROM lion_coll GROUP BY x) g;
RESET pg_lion.enable_count_pushdown;
DROP TABLE lion_coll;

-- The INSERT path places colliding keys by the same order.
CREATE TABLE lion_colli (x xid, v int);
CREATE INDEX lion_colli_x ON lion_colli USING lion (x lion_coarse_xid_ops);
INSERT INTO lion_colli
	SELECT CASE WHEN i % 11 = 0 THEN NULL ELSE (i % 300 + 1)::text::xid END, i
	FROM generate_series(1, 6000) i;
SELECT ordered, entries FROM lion_index_stats('lion_colli_x');
SELECT lion_index_verify('lion_colli_x', true);
SET pg_lion.enable_count_pushdown = off;
CREATE TEMP TABLE lion_colli_exp AS
	SELECT x, count(*) AS c FROM lion_colli WHERE x IS NOT NULL GROUP BY x;
RESET pg_lion.enable_count_pushdown;
SELECT count(*) AS keys, count(*) FILTER (
		WHERE lion_index_count('lion_colli_x', e.x) <> e.c) AS keys_wrong
	FROM lion_colli_exp e;
DROP TABLE lion_colli_exp;
SELECT count(*) FROM lion_colli WHERE x IS NULL;
DROP TABLE lion_colli;
DROP OPERATOR CLASS lion_coarse_xid_ops USING lion;
DROP FUNCTION lion_coarse_xid_hash(xid);

-- ---------------------------------------------------------------------
-- 12. Cross-type values on an ORDERED index.
--
-- A value of another type can be descended for when the family offers the
-- cross-type ordering (support proc 4), or when it casts to the key type;
-- otherwise the leaves are walked with the cross-type equality instead.  The
-- single and the batched lookup must make that decision the same way - the
-- batched one used to descend a key-ordered tree in HASH order and come back
-- with nothing.
-- ---------------------------------------------------------------------

-- The shipped integer family has the cross-type ordering.
CREATE TABLE lion_xt (k int NOT NULL, v int);
INSERT INTO lion_xt SELECT i % 2000, i FROM generate_series(1, 40000) i;
CREATE INDEX lion_xt_k ON lion_xt USING lion (k);
VACUUM (ANALYZE) lion_xt;
SELECT lion_index_count('lion_xt_k', 7::int8) AS one_int8,
	   lion_index_count('lion_xt_k', 7::int2) AS one_int2;
SELECT lion_index_count_any('lion_xt_k',
							ARRAY(SELECT (i * 7)::int8 FROM generate_series(1, 200) i)) AS many_int8,
	   lion_index_count_any('lion_xt_k',
							ARRAY(SELECT (i * 7)::int2 FROM generate_series(1, 200) i)) AS many_int2;
SELECT count(*) FROM lion_xt
	WHERE k = ANY (ARRAY(SELECT (i * 7)::int8 FROM generate_series(1, 200) i));
DROP TABLE lion_xt;

-- A family with cross-type equality and hash but NO cross-type ordering.
CREATE OPERATOR FAMILY lion_noxcmp_ops USING lion;
CREATE OPERATOR CLASS lion_noxcmp_int4_ops FOR TYPE int4 USING lion
	FAMILY lion_noxcmp_ops AS
	OPERATOR 1 = (int4, int4),
	FUNCTION 1 hashint4(int4),
	FUNCTION 4 btint4cmp(int4, int4);
ALTER OPERATOR FAMILY lion_noxcmp_ops USING lion ADD
	OPERATOR 1 = (int4, int8),
	OPERATOR 1 = (int4, int2),
	FUNCTION 1 (int8, int8) hashint8(int8),
	FUNCTION 1 (int2, int2) hashint2(int2);

CREATE TABLE lion_noxcmp (k int NOT NULL, v int);
INSERT INTO lion_noxcmp SELECT i % 2000, i FROM generate_series(1, 40000) i;
CREATE INDEX lion_noxcmp_k ON lion_noxcmp
	USING lion (k lion_noxcmp_int4_ops);
VACUUM (ANALYZE) lion_noxcmp;
SELECT ordered FROM lion_index_stats('lion_noxcmp_k');
-- one value at a time: the leaf walk of §21
SELECT lion_index_count('lion_noxcmp_k', 7::int8) AS one_int8,
	   lion_index_count('lion_noxcmp_k', 7::int2) AS one_int2;
-- a whole list: the same answers, which is the point
SELECT lion_index_count_any('lion_noxcmp_k', ARRAY[1,2,3,4,5]::int8[]) AS five_int8,
	   lion_index_count_any('lion_noxcmp_k', ARRAY[1,2,3,4,5]::int2[]) AS five_int2;
SELECT lion_index_count_any('lion_noxcmp_k',
							ARRAY(SELECT (i * 7)::int8 FROM generate_series(1, 200) i)) AS many_int8;
SELECT count(*) FROM lion_noxcmp
	WHERE k = ANY (ARRAY(SELECT (i * 7)::int8 FROM generate_series(1, 200) i));
SET pg_lion.enable_count_pushdown = off;
SELECT count(*) FROM lion_noxcmp
	WHERE k = ANY (ARRAY(SELECT (i * 7)::int8 FROM generate_series(1, 200) i));
RESET pg_lion.enable_count_pushdown;
DROP TABLE lion_noxcmp;
DROP OPERATOR FAMILY lion_noxcmp_ops USING lion CASCADE;

DROP FUNCTION lion_groups_monotonic(text, bool);
