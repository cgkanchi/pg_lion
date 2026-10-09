/* pg_lion--0.1.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_lion" to load this file. \quit

CREATE FUNCTION lion_handler(internal)
RETURNS index_am_handler
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE ACCESS METHOD lion TYPE INDEX HANDLER lion_handler;

COMMENT ON ACCESS METHOD lion IS
	'roaring bitmap inverted index access method';

/*
 * Operator classes.  A roaring opclass needs exactly what a hash opclass
 * needs: support function 1 is the type's hash function and strategy 1 is
 * equality, so every class below reuses the hash access method's function.
 *
 * Support function 4 is the ORDERING of DESIGN.md §21: the btree comparison
 * function of the KEY type, which is what puts the entry directory in key
 * order.  It is the type's default btree opclass's support function 1, and it
 * is optional - `xid` and `cid` have no btree opclass at all, and an index on
 * one of those is simply not ordered (lion_index_stats().ordered is false and
 * the count pushdown claims no pathkeys for it).  The ordering must agree with
 * strategy 1: two keys the comparison calls equal must be equal to the
 * opclass, because the directory holds ONE entry per equality class and finds
 * it by scanning the run of keys the comparison ties.
 *
 * Strategies 6 .. 9 are the range comparisons of DESIGN.md §28 - <, <=, >=
 * and >, in btree's order - which a class may only have beside support
 * function 4 for the same type pair: a range is answered by walking the run of
 * entries the ordering puts between its bounds.  Every class below that has
 * support function 4 has them; xid and cid, and the multi-key classes further
 * down, do not.  The operators are core's and, like `=`, resolve to
 * pg_catalog's before anything in the target schema.
 *
 * Strategy 10 is `<>` (DESIGN.md §35): every entry of the column but the one
 * the value names - a range with a hole, walked as a range is - and every
 * class with the range comparisons has it, for the same type pairs, beside
 * the same support function 4.
 */

/* ---- integers: one family, so cross-type equality can use the index ---- */

CREATE OPERATOR FAMILY integer_ops USING lion;

CREATE OPERATOR CLASS int2_ops DEFAULT FOR TYPE int2 USING lion
	FAMILY integer_ops AS
	OPERATOR	1	= (int2, int2),
	OPERATOR	6	< (int2, int2),
	OPERATOR	7	<= (int2, int2),
	OPERATOR	8	>= (int2, int2),
	OPERATOR	9	> (int2, int2),
	OPERATOR	10	<> (int2, int2),
	FUNCTION	1	hashint2(int2),
	FUNCTION	4	btint2cmp(int2, int2);

CREATE OPERATOR CLASS int4_ops DEFAULT FOR TYPE int4 USING lion
	FAMILY integer_ops AS
	OPERATOR	1	= (int4, int4),
	OPERATOR	6	< (int4, int4),
	OPERATOR	7	<= (int4, int4),
	OPERATOR	8	>= (int4, int4),
	OPERATOR	9	> (int4, int4),
	OPERATOR	10	<> (int4, int4),
	FUNCTION	1	hashint4(int4),
	FUNCTION	4	btint4cmp(int4, int4);

CREATE OPERATOR CLASS int8_ops DEFAULT FOR TYPE int8 USING lion
	FAMILY integer_ops AS
	OPERATOR	1	= (int8, int8),
	OPERATOR	6	< (int8, int8),
	OPERATOR	7	<= (int8, int8),
	OPERATOR	8	>= (int8, int8),
	OPERATOR	9	> (int8, int8),
	OPERATOR	10	<> (int8, int8),
	FUNCTION	1	hashint8(int8),
	FUNCTION	4	btint8cmp(int8, int8);

/*
 * Cross-type equality, and the cross-type ORDERING that lets a search for a
 * value of one width descend a directory of another (DESIGN.md §21).  Without
 * support function 4 for the pair, such a search falls back to walking every
 * leaf - correct, but linear.  The cross-type range comparisons (§28) walk
 * the same way: `int4col < 5000000000::int8` descends with btint48cmp, which
 * compares the two widths exactly, as btree does.
 */
ALTER OPERATOR FAMILY integer_ops USING lion ADD
	OPERATOR	1	= (int2, int4),
	OPERATOR	1	= (int2, int8),
	OPERATOR	1	= (int4, int2),
	OPERATOR	1	= (int4, int8),
	OPERATOR	1	= (int8, int2),
	OPERATOR	1	= (int8, int4),
	OPERATOR	6	< (int2, int4),
	OPERATOR	7	<= (int2, int4),
	OPERATOR	8	>= (int2, int4),
	OPERATOR	9	> (int2, int4),
	OPERATOR	10	<> (int2, int4),
	OPERATOR	6	< (int2, int8),
	OPERATOR	7	<= (int2, int8),
	OPERATOR	8	>= (int2, int8),
	OPERATOR	9	> (int2, int8),
	OPERATOR	10	<> (int2, int8),
	OPERATOR	6	< (int4, int2),
	OPERATOR	7	<= (int4, int2),
	OPERATOR	8	>= (int4, int2),
	OPERATOR	9	> (int4, int2),
	OPERATOR	10	<> (int4, int2),
	OPERATOR	6	< (int4, int8),
	OPERATOR	7	<= (int4, int8),
	OPERATOR	8	>= (int4, int8),
	OPERATOR	9	> (int4, int8),
	OPERATOR	10	<> (int4, int8),
	OPERATOR	6	< (int8, int2),
	OPERATOR	7	<= (int8, int2),
	OPERATOR	8	>= (int8, int2),
	OPERATOR	9	> (int8, int2),
	OPERATOR	10	<> (int8, int2),
	OPERATOR	6	< (int8, int4),
	OPERATOR	7	<= (int8, int4),
	OPERATOR	8	>= (int8, int4),
	OPERATOR	9	> (int8, int4),
	OPERATOR	10	<> (int8, int4),
	FUNCTION	4	(int2, int4) btint24cmp(int2, int4),
	FUNCTION	4	(int2, int8) btint28cmp(int2, int8),
	FUNCTION	4	(int4, int2) btint42cmp(int4, int2),
	FUNCTION	4	(int4, int8) btint48cmp(int4, int8),
	FUNCTION	4	(int8, int2) btint82cmp(int8, int2),
	FUNCTION	4	(int8, int4) btint84cmp(int8, int4);

/* ---- floats: likewise ---- */

CREATE OPERATOR FAMILY float_ops USING lion;

CREATE OPERATOR CLASS float4_ops DEFAULT FOR TYPE float4 USING lion
	FAMILY float_ops AS
	OPERATOR	1	= (float4, float4),
	OPERATOR	6	< (float4, float4),
	OPERATOR	7	<= (float4, float4),
	OPERATOR	8	>= (float4, float4),
	OPERATOR	9	> (float4, float4),
	OPERATOR	10	<> (float4, float4),
	FUNCTION	1	hashfloat4(float4),
	FUNCTION	4	btfloat4cmp(float4, float4);

CREATE OPERATOR CLASS float8_ops DEFAULT FOR TYPE float8 USING lion
	FAMILY float_ops AS
	OPERATOR	1	= (float8, float8),
	OPERATOR	6	< (float8, float8),
	OPERATOR	7	<= (float8, float8),
	OPERATOR	8	>= (float8, float8),
	OPERATOR	9	> (float8, float8),
	OPERATOR	10	<> (float8, float8),
	FUNCTION	1	hashfloat8(float8),
	FUNCTION	4	btfloat8cmp(float8, float8);

ALTER OPERATOR FAMILY float_ops USING lion ADD
	OPERATOR	1	= (float4, float8),
	OPERATOR	1	= (float8, float4),
	OPERATOR	6	< (float4, float8),
	OPERATOR	7	<= (float4, float8),
	OPERATOR	8	>= (float4, float8),
	OPERATOR	9	> (float4, float8),
	OPERATOR	10	<> (float4, float8),
	OPERATOR	6	< (float8, float4),
	OPERATOR	7	<= (float8, float4),
	OPERATOR	8	>= (float8, float4),
	OPERATOR	9	> (float8, float4),
	OPERATOR	10	<> (float8, float4),
	FUNCTION	4	(float4, float8) btfloat48cmp(float4, float8),
	FUNCTION	4	(float8, float4) btfloat84cmp(float8, float4);

/* ---- one class per remaining type ---- */

CREATE OPERATOR CLASS oid_ops DEFAULT FOR TYPE oid USING lion AS
	OPERATOR	1	= (oid, oid),
	OPERATOR	6	< (oid, oid),
	OPERATOR	7	<= (oid, oid),
	OPERATOR	8	>= (oid, oid),
	OPERATOR	9	> (oid, oid),
	OPERATOR	10	<> (oid, oid),
	FUNCTION	1	hashoid(oid),
	FUNCTION	4	btoidcmp(oid, oid);

/*
 * PostgreSQL 18 gave seven types a hash function of their own (hashbool,
 * hashbytea, hashdate, hashxid, hashxid8, hashcid, timestamptz_hash).  Before
 * that their hash opclasses borrowed a physically compatible function -
 * hashchar, hashvarlena, hashint4, hashint8, timestamp_hash - which computes
 * the same value, and hashvalidate() kept a list of those substitutions;
 * lionvalidate() keeps the same list on those servers.  The classes of these
 * types are therefore created through this helper, which names the new
 * function where the server has it and the old one where it does not.
 *
 * Both names are looked up in pg_catalog ONLY.  Unqualified, a server without
 * the new function would find it in the target schema instead, and a role
 * with CREATE there could plant e.g. hashbool(bool) before CREATE EXTENSION:
 * it becomes support function 1, which then runs as whoever inserts into or
 * builds a lion index on that type - a superuser included.
 */
CREATE FUNCTION lion_create_opclass_pre18(stmt text, newfn text, oldfn text)
RETURNS void LANGUAGE plpgsql AS $f$
BEGIN
	IF pg_catalog.to_regprocedure('pg_catalog.' || newfn) IS NOT NULL THEN
		EXECUTE pg_catalog.format(stmt, 'pg_catalog.' || newfn);
	ELSE
		EXECUTE pg_catalog.format(stmt, 'pg_catalog.' || oldfn);
	END IF;
END
$f$;

SELECT lion_create_opclass_pre18('CREATE OPERATOR CLASS bool_ops DEFAULT FOR TYPE bool USING lion AS
	OPERATOR	1	= (bool, bool),
	OPERATOR	6	< (bool, bool),
	OPERATOR	7	<= (bool, bool),
	OPERATOR	8	>= (bool, bool),
	OPERATOR	9	> (bool, bool),
	OPERATOR	10	<> (bool, bool),
	FUNCTION	1	%s,
	FUNCTION	4	btboolcmp(bool, bool)', 'hashbool(bool)', 'hashchar("char")');

CREATE OPERATOR CLASS char_ops DEFAULT FOR TYPE "char" USING lion AS
	OPERATOR	1	= ("char", "char"),
	OPERATOR	6	< ("char", "char"),
	OPERATOR	7	<= ("char", "char"),
	OPERATOR	8	>= ("char", "char"),
	OPERATOR	9	> ("char", "char"),
	OPERATOR	10	<> ("char", "char"),
	FUNCTION	1	hashchar("char"),
	FUNCTION	4	btcharcmp("char", "char");

CREATE OPERATOR CLASS name_ops DEFAULT FOR TYPE name USING lion AS
	OPERATOR	1	= (name, name),
	OPERATOR	6	< (name, name),
	OPERATOR	7	<= (name, name),
	OPERATOR	8	>= (name, name),
	OPERATOR	9	> (name, name),
	OPERATOR	10	<> (name, name),
	FUNCTION	1	hashname(name),
	FUNCTION	4	btnamecmp(name, name);

/* varchar reaches this class through binary coercion, as it does for hash */
CREATE OPERATOR CLASS text_ops DEFAULT FOR TYPE text USING lion AS
	OPERATOR	1	= (text, text),
	OPERATOR	6	< (text, text),
	OPERATOR	7	<= (text, text),
	OPERATOR	8	>= (text, text),
	OPERATOR	9	> (text, text),
	OPERATOR	10	<> (text, text),
	FUNCTION	1	hashtext(text),
	FUNCTION	4	bttextcmp(text, text);

CREATE OPERATOR CLASS bpchar_ops DEFAULT FOR TYPE bpchar USING lion AS
	OPERATOR	1	= (bpchar, bpchar),
	OPERATOR	6	< (bpchar, bpchar),
	OPERATOR	7	<= (bpchar, bpchar),
	OPERATOR	8	>= (bpchar, bpchar),
	OPERATOR	9	> (bpchar, bpchar),
	OPERATOR	10	<> (bpchar, bpchar),
	FUNCTION	1	hashbpchar(bpchar),
	FUNCTION	4	bpcharcmp(bpchar, bpchar);

SELECT lion_create_opclass_pre18('CREATE OPERATOR CLASS bytea_ops DEFAULT FOR TYPE bytea USING lion AS
	OPERATOR	1	= (bytea, bytea),
	OPERATOR	6	< (bytea, bytea),
	OPERATOR	7	<= (bytea, bytea),
	OPERATOR	8	>= (bytea, bytea),
	OPERATOR	9	> (bytea, bytea),
	OPERATOR	10	<> (bytea, bytea),
	FUNCTION	1	%s,
	FUNCTION	4	byteacmp(bytea, bytea)', 'hashbytea(bytea)', 'hashvarlena(internal)');

CREATE OPERATOR CLASS uuid_ops DEFAULT FOR TYPE uuid USING lion AS
	OPERATOR	1	= (uuid, uuid),
	OPERATOR	6	< (uuid, uuid),
	OPERATOR	7	<= (uuid, uuid),
	OPERATOR	8	>= (uuid, uuid),
	OPERATOR	9	> (uuid, uuid),
	OPERATOR	10	<> (uuid, uuid),
	FUNCTION	1	uuid_hash(uuid),
	FUNCTION	4	uuid_cmp(uuid, uuid);

SELECT lion_create_opclass_pre18('CREATE OPERATOR CLASS date_ops DEFAULT FOR TYPE date USING lion AS
	OPERATOR	1	= (date, date),
	OPERATOR	6	< (date, date),
	OPERATOR	7	<= (date, date),
	OPERATOR	8	>= (date, date),
	OPERATOR	9	> (date, date),
	OPERATOR	10	<> (date, date),
	FUNCTION	1	%s,
	FUNCTION	4	date_cmp(date, date)', 'hashdate(date)', 'hashint4(int4)');

CREATE OPERATOR CLASS time_ops DEFAULT FOR TYPE time USING lion AS
	OPERATOR	1	= (time, time),
	OPERATOR	6	< (time, time),
	OPERATOR	7	<= (time, time),
	OPERATOR	8	>= (time, time),
	OPERATOR	9	> (time, time),
	OPERATOR	10	<> (time, time),
	FUNCTION	1	time_hash(time),
	FUNCTION	4	time_cmp(time, time);

CREATE OPERATOR CLASS timetz_ops DEFAULT FOR TYPE timetz USING lion AS
	OPERATOR	1	= (timetz, timetz),
	OPERATOR	6	< (timetz, timetz),
	OPERATOR	7	<= (timetz, timetz),
	OPERATOR	8	>= (timetz, timetz),
	OPERATOR	9	> (timetz, timetz),
	OPERATOR	10	<> (timetz, timetz),
	FUNCTION	1	timetz_hash(timetz),
	FUNCTION	4	timetz_cmp(timetz, timetz);

CREATE OPERATOR CLASS timestamp_ops DEFAULT FOR TYPE timestamp USING lion AS
	OPERATOR	1	= (timestamp, timestamp),
	OPERATOR	6	< (timestamp, timestamp),
	OPERATOR	7	<= (timestamp, timestamp),
	OPERATOR	8	>= (timestamp, timestamp),
	OPERATOR	9	> (timestamp, timestamp),
	OPERATOR	10	<> (timestamp, timestamp),
	FUNCTION	1	timestamp_hash(timestamp),
	FUNCTION	4	timestamp_cmp(timestamp, timestamp);

SELECT lion_create_opclass_pre18('CREATE OPERATOR CLASS timestamptz_ops DEFAULT FOR TYPE timestamptz USING lion AS
	OPERATOR	1	= (timestamptz, timestamptz),
	OPERATOR	6	< (timestamptz, timestamptz),
	OPERATOR	7	<= (timestamptz, timestamptz),
	OPERATOR	8	>= (timestamptz, timestamptz),
	OPERATOR	9	> (timestamptz, timestamptz),
	OPERATOR	10	<> (timestamptz, timestamptz),
	FUNCTION	1	%s,
	FUNCTION	4	timestamptz_cmp(timestamptz, timestamptz)', 'timestamptz_hash(timestamptz)', 'timestamp_hash(timestamp)');

CREATE OPERATOR CLASS interval_ops DEFAULT FOR TYPE interval USING lion AS
	OPERATOR	1	= (interval, interval),
	OPERATOR	6	< (interval, interval),
	OPERATOR	7	<= (interval, interval),
	OPERATOR	8	>= (interval, interval),
	OPERATOR	9	> (interval, interval),
	OPERATOR	10	<> (interval, interval),
	FUNCTION	1	interval_hash(interval),
	FUNCTION	4	interval_cmp(interval, interval);

CREATE OPERATOR CLASS numeric_ops DEFAULT FOR TYPE numeric USING lion AS
	OPERATOR	1	= (numeric, numeric),
	OPERATOR	6	< (numeric, numeric),
	OPERATOR	7	<= (numeric, numeric),
	OPERATOR	8	>= (numeric, numeric),
	OPERATOR	9	> (numeric, numeric),
	OPERATOR	10	<> (numeric, numeric),
	FUNCTION	1	hash_numeric(numeric),
	FUNCTION	4	numeric_cmp(numeric, numeric);

CREATE OPERATOR CLASS macaddr_ops DEFAULT FOR TYPE macaddr USING lion AS
	OPERATOR	1	= (macaddr, macaddr),
	OPERATOR	6	< (macaddr, macaddr),
	OPERATOR	7	<= (macaddr, macaddr),
	OPERATOR	8	>= (macaddr, macaddr),
	OPERATOR	9	> (macaddr, macaddr),
	OPERATOR	10	<> (macaddr, macaddr),
	FUNCTION	1	hashmacaddr(macaddr),
	FUNCTION	4	macaddr_cmp(macaddr, macaddr);

CREATE OPERATOR CLASS macaddr8_ops DEFAULT FOR TYPE macaddr8 USING lion AS
	OPERATOR	1	= (macaddr8, macaddr8),
	OPERATOR	6	< (macaddr8, macaddr8),
	OPERATOR	7	<= (macaddr8, macaddr8),
	OPERATOR	8	>= (macaddr8, macaddr8),
	OPERATOR	9	> (macaddr8, macaddr8),
	OPERATOR	10	<> (macaddr8, macaddr8),
	FUNCTION	1	hashmacaddr8(macaddr8),
	FUNCTION	4	macaddr8_cmp(macaddr8, macaddr8);

CREATE OPERATOR CLASS inet_ops DEFAULT FOR TYPE inet USING lion AS
	OPERATOR	1	= (inet, inet),
	OPERATOR	6	< (inet, inet),
	OPERATOR	7	<= (inet, inet),
	OPERATOR	8	>= (inet, inet),
	OPERATOR	9	> (inet, inet),
	OPERATOR	10	<> (inet, inet),
	FUNCTION	1	hashinet(inet),
	FUNCTION	4	network_cmp(inet, inet);

CREATE OPERATOR CLASS jsonb_ops DEFAULT FOR TYPE jsonb USING lion AS
	OPERATOR	1	= (jsonb, jsonb),
	OPERATOR	6	< (jsonb, jsonb),
	OPERATOR	7	<= (jsonb, jsonb),
	OPERATOR	8	>= (jsonb, jsonb),
	OPERATOR	9	> (jsonb, jsonb),
	OPERATOR	10	<> (jsonb, jsonb),
	FUNCTION	1	jsonb_hash(jsonb),
	FUNCTION	4	jsonb_cmp(jsonb, jsonb);

CREATE OPERATOR CLASS pg_lsn_ops DEFAULT FOR TYPE pg_lsn USING lion AS
	OPERATOR	1	= (pg_lsn, pg_lsn),
	OPERATOR	6	< (pg_lsn, pg_lsn),
	OPERATOR	7	<= (pg_lsn, pg_lsn),
	OPERATOR	8	>= (pg_lsn, pg_lsn),
	OPERATOR	9	> (pg_lsn, pg_lsn),
	OPERATOR	10	<> (pg_lsn, pg_lsn),
	FUNCTION	1	pg_lsn_hash(pg_lsn),
	FUNCTION	4	pg_lsn_cmp(pg_lsn, pg_lsn);

/*
 * xid and cid have no btree opclass, so they get no support function 4: their
 * directories are ordered by (hash, stored bytes) alone, which is a complete
 * order but not the type's, and lion_index_stats() reports ordered = false.
 */
SELECT lion_create_opclass_pre18('CREATE OPERATOR CLASS xid_ops DEFAULT FOR TYPE xid USING lion AS
	OPERATOR	1	= (xid, xid),
	FUNCTION	1	%s', 'hashxid(xid)', 'hashint4(int4)');

SELECT lion_create_opclass_pre18('CREATE OPERATOR CLASS xid8_ops DEFAULT FOR TYPE xid8 USING lion AS
	OPERATOR	1	= (xid8, xid8),
	OPERATOR	6	< (xid8, xid8),
	OPERATOR	7	<= (xid8, xid8),
	OPERATOR	8	>= (xid8, xid8),
	OPERATOR	9	> (xid8, xid8),
	OPERATOR	10	<> (xid8, xid8),
	FUNCTION	1	%s,
	FUNCTION	4	xid8cmp(xid8, xid8)', 'hashxid8(xid8)', 'hashint8(int8)');

SELECT lion_create_opclass_pre18('CREATE OPERATOR CLASS cid_ops DEFAULT FOR TYPE cid USING lion AS
	OPERATOR	1	= (cid, cid),
	FUNCTION	1	%s', 'hashcid(cid)', 'hashint4(int4)');

CREATE OPERATOR CLASS tid_ops DEFAULT FOR TYPE tid USING lion AS
	OPERATOR	1	= (tid, tid),
	OPERATOR	6	< (tid, tid),
	OPERATOR	7	<= (tid, tid),
	OPERATOR	8	>= (tid, tid),
	OPERATOR	9	> (tid, tid),
	OPERATOR	10	<> (tid, tid),
	FUNCTION	1	hashtid(tid),
	FUNCTION	4	bttidcmp(tid, tid);

CREATE OPERATOR CLASS enum_ops DEFAULT FOR TYPE anyenum USING lion AS
	OPERATOR	1	= (anyenum, anyenum),
	OPERATOR	6	< (anyenum, anyenum),
	OPERATOR	7	<= (anyenum, anyenum),
	OPERATOR	8	>= (anyenum, anyenum),
	OPERATOR	9	> (anyenum, anyenum),
	OPERATOR	10	<> (anyenum, anyenum),
	FUNCTION	1	hashenum(anyenum),
	FUNCTION	4	enum_cmp(anyenum, anyenum);

/* ---------------------------------------------------------------------
 * Multi-key operator classes (DESIGN.md §17)
 *
 * One indexed value contributes many keys.  The extraction is GIN's, reused
 * verbatim: support function 2 is extractValue and 3 is extractQuery, with
 * GIN's own signatures, so `ginarrayextract` and friends are named here
 * directly.  What is NOT reused is GIN's consistent function - a roaring scan
 * combines whole posting sets instead of testing one row at a time - so the
 * strategy numbers and what they mean are the roaring AM's own:
 *
 *		1 =		2 @>	3 &&	4 <@	5 @@
 *
 * The STORAGE type is the key type.  array_ops stores anyelement, which is
 * resolved to the column's element type when the index is created, and has
 * neither support function 1 nor 4 for that reason: the element type's own
 * default hash and btree opclasses are used instead (lion_fill_state()).
 * tsvector_ops stores text and can name hashtext() and bttextcmp() - lexemes
 * are byte strings compared under the C collation, which is what
 * gin_cmp_tslexeme() does too.
 * --------------------------------------------------------------------- */

CREATE OPERATOR CLASS array_ops DEFAULT FOR TYPE anyarray USING lion AS
	OPERATOR	2	@> (anyarray, anyarray),
	OPERATOR	3	&& (anyarray, anyarray),
	OPERATOR	4	<@ (anyarray, anyarray),
	FUNCTION	2	ginarrayextract(anyarray, internal, internal),
	FUNCTION	3	ginqueryarrayextract(anyarray, internal, int2, internal, internal, internal, internal),
	STORAGE		anyelement;

/*
 * Support function 5 hands the index every lexeme's positions and weights
 * beside its key.  An index stores them only when it is built WITH
 * (store_positions = true) (DESIGN.md §17, "Stored positions"): a phrase, a
 * weight and `a & !b` are then answered exactly from the index - no heap
 * recheck in a scan, and none on an all-visible page in a count - for about
 * three bytes per lexeme and row more.
 */
CREATE FUNCTION lion_tsvector_positions(tsvector, internal) RETURNS internal
	AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE OPERATOR CLASS tsvector_ops DEFAULT FOR TYPE tsvector USING lion AS
	OPERATOR	5	@@ (tsvector, tsquery),
	FUNCTION	1	hashtext(text),
	FUNCTION	4	bttextcmp(text, text),
	FUNCTION	2	gin_extract_tsvector(tsvector, internal, internal),
	FUNCTION	3	gin_extract_tsquery(tsvector, internal, int2, internal, internal, internal, internal),
	FUNCTION	5	lion_tsvector_positions(tsvector, internal),
	STORAGE		text;

/*
 * jsonb_contains_ops (lion_jsonb.c, DESIGN.md §42): a jsonb document under
 * one key per path to each of its scalars (with the scalar), per container,
 * and per top-level key, so that `@>`, `?`, `?|` and `?&` are answered from
 * the index, and jsonpath `@?` and `@@` (§43) narrowed by it and rechecked.
 * Keys are bytea and compare bytewise.  Not the default: the default
 * jsonb_ops indexes whole documents for `=`.
 *
 *   CREATE INDEX ON docs USING lion (doc jsonb_contains_ops);
 */
CREATE FUNCTION lion_jsonb_extract_value(jsonb, internal, internal)
	RETURNS internal AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION lion_jsonb_extract_query(jsonb, internal, int2, internal, internal, internal, internal)
	RETURNS internal AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

SELECT lion_create_opclass_pre18('CREATE OPERATOR CLASS jsonb_contains_ops FOR TYPE jsonb USING lion AS
	OPERATOR	11	@> (jsonb, jsonb),
	OPERATOR	12	? (jsonb, text),
	OPERATOR	13	?| (jsonb, text[]),
	OPERATOR	14	?& (jsonb, text[]),
	OPERATOR	15	@? (jsonb, jsonpath),
	OPERATOR	16	@@ (jsonb, jsonpath),
	FUNCTION	1	%s,
	FUNCTION	4	byteacmp(bytea, bytea),
	FUNCTION	2	lion_jsonb_extract_value(jsonb, internal, internal),
	FUNCTION	3	lion_jsonb_extract_query(jsonb, internal, int2, internal, internal, internal, internal),
	STORAGE		bytea', 'hashbytea(bytea)', 'hashvarlena(internal)');

/*
 * BM25 ranking (lion_bm25.c, DESIGN.md §17 "Ranking"): the k rows with the
 * highest Okapi BM25 score for the query's lexemes, best first, scored from
 * an index built WITH (store_positions = true).  Join on ctid for the rows:
 *
 *   SELECT d.*, s.score
 *     FROM lion_bm25('doc_tsv_idx', to_tsquery('english', 'cat & dog'), 10) s
 *     JOIN doc d ON d.ctid = s.ctid
 *    ORDER BY s.score DESC;
 */
CREATE FUNCTION lion_bm25(index regclass, query tsquery, k int,
						  k1 float8 DEFAULT 1.2, b float8 DEFAULT 0.75,
						  OUT ctid tid, OUT score float8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME' LANGUAGE C STRICT VOLATILE PARALLEL RESTRICTED ROWS 10;

/*
 * The score lion_bm25() would give one row, from its tsvector and the
 * index's statistics.  Ordered by it, with the rows matching a query on the
 * same column, a LionBm25 scan returns them best first from the index
 * (lion_bm25_scan.c):
 *
 *   SELECT d.*, lion_bm25_score(d.tsv, 'cat & dog', 'doc_tsv_idx') AS score
 *     FROM doc d
 *    WHERE d.tsv @@ 'cat & dog'
 *    ORDER BY lion_bm25_score(d.tsv, 'cat & dog', 'doc_tsv_idx') DESC
 *    LIMIT 10;
 *
 * PARALLEL RESTRICTED: each backend reads the index's statistics once per
 * call site, so two parallel workers that read them on either side of an
 * insert would score rows by different N, df and avgdl, and the merged order
 * would not be one ranking.  In the leader every row is scored by one read.
 */
CREATE FUNCTION lion_bm25_score(doc tsvector, query tsquery, index regclass,
								k1 float8 DEFAULT 1.2, b float8 DEFAULT 0.75)
	RETURNS float8
	AS 'MODULE_PATHNAME' LANGUAGE C STRICT STABLE PARALLEL RESTRICTED COST 10;

/* ---------------------------------------------------------------------
 * count functions (lion_count.c)
 *
 * Heap-skipping count(*) over the posting sets of one or two roaring
 * indexes, interlocked with the visibility map (DESIGN.md section 9).  The
 * result is always exactly count(*) of the equivalent SELECT under the same
 * snapshot, so these are VOLATILE.
 *
 * A key is taken as `col = key` would take it: the column's own type, a type
 * the column's operator family compares it with (an int8 key on an int4
 * column), or a binary coercion to the column's type (varchar on text); a
 * domain as its base type; and for enum_ops the column's own enum and no
 * other (DESIGN.md section 9, "SQL surface").  It is compared under the
 * collation `col = key` would use - the key's own when it brings one (an
 * explicit COLLATE), otherwise the column's - and where that is not the
 * index's collation and either of the two is nondeterministic, the count is
 * refused: the index's equality is not the query's then.  The grouped form
 * groups under the column's collation, as `GROUP BY col` does.
 *
 * They are STRICT: a NULL index or key answers NULL - no count is made -
 * where `count(*) WHERE col = NULL` answers 0 (section 9 says why).  A NULL
 * element of lion_index_count_any()'s array selects nothing and counts 0.
 * --------------------------------------------------------------------- */

CREATE FUNCTION lion_index_count(idx regclass, key anyelement)
RETURNS bigint
AS 'MODULE_PATHNAME', 'lion_index_count'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

COMMENT ON FUNCTION lion_index_count(regclass, anyelement) IS
	'count rows with key from a lion index, skipping all-visible heap pages';

/*
 * The two keys are of two unrelated polymorphic types: each is compared with
 * its own index's column, and nothing says the two columns share a type.
 * With one anyelement for both, `lion_index_count('i', 1, 't', 'x'::text)`
 * could not be called at all.
 */
CREATE FUNCTION lion_index_count(idx1 regclass, key1 anyelement,
									idx2 regclass, key2 anycompatible)
RETURNS bigint
AS 'MODULE_PATHNAME', 'lion_index_count2'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

COMMENT ON FUNCTION lion_index_count(regclass, anyelement, regclass, anycompatible) IS
	'count rows matching both keys from two lion indexes on the same table';

/*
 * count(*) WHERE col = ANY (keys): the union of the listed values' posting
 * sets (DESIGN.md section 15), located in bucket order and merged k-way.
 */
CREATE FUNCTION lion_index_count_any(idx regclass, keys anyarray)
RETURNS bigint
AS 'MODULE_PATHNAME', 'lion_index_count_any'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

COMMENT ON FUNCTION lion_index_count_any(regclass, anyarray) IS
	'count rows whose key is in the array, skipping all-visible heap pages';

CREATE FUNCTION lion_index_count_stats(idx regclass, key anyelement,
										  OUT count bigint,
										  OUT blocks_skipped bigint,
										  OUT tids_rechecked bigint,
										  OUT blocks_rechecked bigint,
										  OUT cache_hits bigint)
RETURNS record
AS 'MODULE_PATHNAME', 'lion_index_count_stats'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

COMMENT ON FUNCTION lion_index_count_stats(regclass, anyelement) IS
	'lion_index_count() plus how much of the heap it had to visit';

/*
 * Count every key of one index under one snapshot, sharing one per-query
 * visibility cache across the groups, exactly as the GROUP BY pushdown does
 * (DESIGN.md section 9).  cache_hits is the number of heap block visits the
 * cache answered without touching the buffer manager; cache_full counts the
 * visits that had to be fetched because the cache had used up work_mem.
 */
CREATE FUNCTION lion_index_count_group_stats(idx regclass,
												use_cache boolean DEFAULT true,
												attno int2 DEFAULT 1,
												OUT groups bigint,
												OUT count bigint,
												OUT blocks_skipped bigint,
												OUT tids_rechecked bigint,
												OUT blocks_rechecked bigint,
												OUT cache_hits bigint,
												OUT cache_full bigint)
RETURNS record
AS 'MODULE_PATHNAME', 'lion_index_count_group_stats'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

COMMENT ON FUNCTION lion_index_count_group_stats(regclass, boolean, int2) IS
	'count every key of one key column of a lion index, as the GROUP BY pushdown does, and report the heap visits';

/* ---------------------------------------------------------------------
 * aggregates of the FK-side join (lion_customscan.c, DESIGN.md §27,
 * "Every aggregate over the node's rows")
 *
 * When a join's aggregates need the dimension rows themselves -
 * count(DISTINCT), min, max - beside counts of join pairs, the node hands up
 * one row per dimension row that joins, carrying that row's count of fact
 * rows, and the core Agg above it adds those counts up with these in place of
 * count() and sum():
 *
 *	lion_join_count(n)	count's answer from counts: 0 over no rows, never
 *						NULL, a NULL n skipped (count(x) of a dimension column
 *						is lion_join_count(n) FILTER (WHERE x IS NOT NULL));
 *	lion_join_sum(n)	sum's: NULL over no rows, a NULL n skipped
 *						(sum(x) of an int2 or int4 column is lion_join_sum(x *
 *						n), whose answer is sum()'s int8).
 *
 * Both add in int8 with int8pl, which fails with "bigint out of range" where
 * count()'s own int8inc would (the counts are never negative, so no partial
 * sum passes the total), and combine with it, so they may be split into
 * partial aggregates.  They are core's sum(int8) minus what that one does
 * for a total past int8 (a numeric answer, from 128-bit or numeric state):
 * a count past int8 is an error for count() too.
 * --------------------------------------------------------------------- */

CREATE AGGREGATE lion_join_count(bigint) (
	SFUNC = pg_catalog.int8pl,
	STYPE = bigint,
	COMBINEFUNC = pg_catalog.int8pl,
	INITCOND = '0',
	PARALLEL = SAFE
);

COMMENT ON AGGREGATE lion_join_count(bigint) IS
	'the sum of the counts, 0 over no rows: count() of a join from its dimension rows'' counts';

CREATE AGGREGATE lion_join_sum(bigint) (
	SFUNC = pg_catalog.int8pl,
	STYPE = bigint,
	COMBINEFUNC = pg_catalog.int8pl,
	PARALLEL = SAFE
);

COMMENT ON AGGREGATE lion_join_sum(bigint) IS
	'the sum of the values in int8, NULL over no rows: sum() of a join from its dimension rows'' products';

/* ------------------------------------------------------------------ */
-- stats/verify functions (lion_funcs.c)
/* ------------------------------------------------------------------ */

CREATE FUNCTION lion_index_stats(idx regclass,
									OUT attno int2,
									OUT directory_height int4,
									OUT leaf_pages int8,
									OUT internal_pages int8,
									OUT ordered bool,
									OUT entries int8,
									OUT inline_entries int8,
									OUT container_pages int8,
									OUT containers int8,
									OUT array_containers int8,
									OUT bitset_containers int8,
									OUT run_containers int8,
									OUT ntids int8,
									OUT container_bytes int8,
									OUT free_bytes int8,
									OUT sparse_segments int8,
									OUT sparse_members int8,
									OUT null_tids int8,
									OUT empty_tids int8,
									OUT slack_bytes int8,
									OUT deleted_pages int8,
									OUT posting_internal_pages int8,
									OUT max_posting_height int4,
									OUT inline_slack_bytes int8,
									OUT summary_entries int8,
									OUT summary_tids int8,
									OUT summary_bytes int8,
									OUT summary_pages int8,
									OUT ndistinct int8,
									OUT narrow_containers int8)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'lion_index_stats'
LANGUAGE C STRICT VOLATILE PARALLEL RESTRICTED;

COMMENT ON FUNCTION lion_index_stats(regclass) IS
	'shape of a lion index, one row per key column: directory shape, entries, containers by kind (narrow_containers last, DESIGN.md §38), sparse segments, posting trees, NULL keys and key-less rows, summary posting sets, and the distinct keys the planner is given';

/*
 * The ROOT block of one key's posting tree (DESIGN.md §22), NULL when the key
 * has no entry or its posting set is still INLINE.  For tests: the root block
 * is the identity of a posting set and must never move, which is what a root
 * split's push-down buys.
 */
CREATE FUNCTION lion_index_posting_root(idx regclass, key anyelement)
RETURNS int8
AS 'MODULE_PATHNAME', 'lion_index_posting_root'
LANGUAGE C STRICT VOLATILE PARALLEL RESTRICTED;

COMMENT ON FUNCTION lion_index_posting_root(regclass, anyelement) IS
	'root block of one key''s posting tree, or NULL when it is still inline';

/*
 * How an index is WAL-logged: "generic" or "rmgr" (DESIGN.md §25).  The mode
 * is fixed at CREATE INDEX and recorded on the meta page; REINDEX changes it.
 */
CREATE FUNCTION lion_index_wal_mode(idx regclass)
RETURNS text
AS 'MODULE_PATHNAME', 'lion_index_wal_mode'
LANGUAGE C STRICT VOLATILE PARALLEL RESTRICTED;

COMMENT ON FUNCTION lion_index_wal_mode(regclass) IS
	'which WAL logger this index was built for: generic or rmgr';

CREATE FUNCTION lion_index_verify(idx regclass,
									 heapallindexed bool DEFAULT false)
RETURNS void
AS 'MODULE_PATHNAME', 'lion_index_verify'
LANGUAGE C STRICT VOLATILE PARALLEL RESTRICTED;

COMMENT ON FUNCTION lion_index_verify(regclass, bool) IS
	'check a lion index for structural damage, optionally also checking that every heap tuple is indexed';

/*
 * Who may call the diagnostic functions.  None of them checks table
 * privileges or row-level security: lion_index_posting_root() answers "is
 * this key in the index" for any key, lion_index_stats() gives row and key
 * counts, and lion_index_verify() reads the whole heap (2026-09-23 review).
 * lion_index_wal_mode() says little, but it too takes a lock on whatever
 * relation it is handed and checks nothing.  So, like pageinspect's and
 * amcheck's functions, they are not executable by PUBLIC, and as with
 * pgstattuple the statistics are granted to pg_stat_scan_tables.  A superuser
 * can GRANT the others where wanted.  The counting functions stay public:
 * they check what the equivalent query would.
 */
REVOKE EXECUTE ON FUNCTION lion_index_posting_root(regclass, anyelement) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION lion_index_verify(regclass, bool) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION lion_index_wal_mode(regclass) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION lion_index_stats(regclass) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION lion_index_stats(regclass) TO pg_stat_scan_tables;

DROP FUNCTION lion_create_opclass_pre18(text, text, text);
