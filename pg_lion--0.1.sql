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
 */

/* ---- integers: one family, so cross-type equality can use the index ---- */

CREATE OPERATOR FAMILY integer_ops USING lion;

CREATE OPERATOR CLASS int2_ops DEFAULT FOR TYPE int2 USING lion
	FAMILY integer_ops AS
	OPERATOR	1	= (int2, int2),
	FUNCTION	1	hashint2(int2),
	FUNCTION	4	btint2cmp(int2, int2);

CREATE OPERATOR CLASS int4_ops DEFAULT FOR TYPE int4 USING lion
	FAMILY integer_ops AS
	OPERATOR	1	= (int4, int4),
	FUNCTION	1	hashint4(int4),
	FUNCTION	4	btint4cmp(int4, int4);

CREATE OPERATOR CLASS int8_ops DEFAULT FOR TYPE int8 USING lion
	FAMILY integer_ops AS
	OPERATOR	1	= (int8, int8),
	FUNCTION	1	hashint8(int8),
	FUNCTION	4	btint8cmp(int8, int8);

/*
 * Cross-type equality, and the cross-type ORDERING that lets a search for a
 * value of one width descend a directory of another (DESIGN.md §21).  Without
 * support function 4 for the pair, such a search falls back to walking every
 * leaf - correct, but linear.
 */
ALTER OPERATOR FAMILY integer_ops USING lion ADD
	OPERATOR	1	= (int2, int4),
	OPERATOR	1	= (int2, int8),
	OPERATOR	1	= (int4, int2),
	OPERATOR	1	= (int4, int8),
	OPERATOR	1	= (int8, int2),
	OPERATOR	1	= (int8, int4),
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
	FUNCTION	1	hashfloat4(float4),
	FUNCTION	4	btfloat4cmp(float4, float4);

CREATE OPERATOR CLASS float8_ops DEFAULT FOR TYPE float8 USING lion
	FAMILY float_ops AS
	OPERATOR	1	= (float8, float8),
	FUNCTION	1	hashfloat8(float8),
	FUNCTION	4	btfloat8cmp(float8, float8);

ALTER OPERATOR FAMILY float_ops USING lion ADD
	OPERATOR	1	= (float4, float8),
	OPERATOR	1	= (float8, float4),
	FUNCTION	4	(float4, float8) btfloat48cmp(float4, float8),
	FUNCTION	4	(float8, float4) btfloat84cmp(float8, float4);

/* ---- one class per remaining type ---- */

CREATE OPERATOR CLASS oid_ops DEFAULT FOR TYPE oid USING lion AS
	OPERATOR	1	= (oid, oid),
	FUNCTION	1	hashoid(oid),
	FUNCTION	4	btoidcmp(oid, oid);

CREATE OPERATOR CLASS bool_ops DEFAULT FOR TYPE bool USING lion AS
	OPERATOR	1	= (bool, bool),
	FUNCTION	1	hashbool(bool),
	FUNCTION	4	btboolcmp(bool, bool);

CREATE OPERATOR CLASS char_ops DEFAULT FOR TYPE "char" USING lion AS
	OPERATOR	1	= ("char", "char"),
	FUNCTION	1	hashchar("char"),
	FUNCTION	4	btcharcmp("char", "char");

CREATE OPERATOR CLASS name_ops DEFAULT FOR TYPE name USING lion AS
	OPERATOR	1	= (name, name),
	FUNCTION	1	hashname(name),
	FUNCTION	4	btnamecmp(name, name);

/* varchar reaches this class through binary coercion, as it does for hash */
CREATE OPERATOR CLASS text_ops DEFAULT FOR TYPE text USING lion AS
	OPERATOR	1	= (text, text),
	FUNCTION	1	hashtext(text),
	FUNCTION	4	bttextcmp(text, text);

CREATE OPERATOR CLASS bpchar_ops DEFAULT FOR TYPE bpchar USING lion AS
	OPERATOR	1	= (bpchar, bpchar),
	FUNCTION	1	hashbpchar(bpchar),
	FUNCTION	4	bpcharcmp(bpchar, bpchar);

CREATE OPERATOR CLASS bytea_ops DEFAULT FOR TYPE bytea USING lion AS
	OPERATOR	1	= (bytea, bytea),
	FUNCTION	1	hashbytea(bytea),
	FUNCTION	4	byteacmp(bytea, bytea);

CREATE OPERATOR CLASS uuid_ops DEFAULT FOR TYPE uuid USING lion AS
	OPERATOR	1	= (uuid, uuid),
	FUNCTION	1	uuid_hash(uuid),
	FUNCTION	4	uuid_cmp(uuid, uuid);

CREATE OPERATOR CLASS date_ops DEFAULT FOR TYPE date USING lion AS
	OPERATOR	1	= (date, date),
	FUNCTION	1	hashdate(date),
	FUNCTION	4	date_cmp(date, date);

CREATE OPERATOR CLASS time_ops DEFAULT FOR TYPE time USING lion AS
	OPERATOR	1	= (time, time),
	FUNCTION	1	time_hash(time),
	FUNCTION	4	time_cmp(time, time);

CREATE OPERATOR CLASS timetz_ops DEFAULT FOR TYPE timetz USING lion AS
	OPERATOR	1	= (timetz, timetz),
	FUNCTION	1	timetz_hash(timetz),
	FUNCTION	4	timetz_cmp(timetz, timetz);

CREATE OPERATOR CLASS timestamp_ops DEFAULT FOR TYPE timestamp USING lion AS
	OPERATOR	1	= (timestamp, timestamp),
	FUNCTION	1	timestamp_hash(timestamp),
	FUNCTION	4	timestamp_cmp(timestamp, timestamp);

CREATE OPERATOR CLASS timestamptz_ops DEFAULT FOR TYPE timestamptz USING lion AS
	OPERATOR	1	= (timestamptz, timestamptz),
	FUNCTION	1	timestamptz_hash(timestamptz),
	FUNCTION	4	timestamptz_cmp(timestamptz, timestamptz);

CREATE OPERATOR CLASS interval_ops DEFAULT FOR TYPE interval USING lion AS
	OPERATOR	1	= (interval, interval),
	FUNCTION	1	interval_hash(interval),
	FUNCTION	4	interval_cmp(interval, interval);

CREATE OPERATOR CLASS numeric_ops DEFAULT FOR TYPE numeric USING lion AS
	OPERATOR	1	= (numeric, numeric),
	FUNCTION	1	hash_numeric(numeric),
	FUNCTION	4	numeric_cmp(numeric, numeric);

CREATE OPERATOR CLASS macaddr_ops DEFAULT FOR TYPE macaddr USING lion AS
	OPERATOR	1	= (macaddr, macaddr),
	FUNCTION	1	hashmacaddr(macaddr),
	FUNCTION	4	macaddr_cmp(macaddr, macaddr);

CREATE OPERATOR CLASS macaddr8_ops DEFAULT FOR TYPE macaddr8 USING lion AS
	OPERATOR	1	= (macaddr8, macaddr8),
	FUNCTION	1	hashmacaddr8(macaddr8),
	FUNCTION	4	macaddr8_cmp(macaddr8, macaddr8);

CREATE OPERATOR CLASS inet_ops DEFAULT FOR TYPE inet USING lion AS
	OPERATOR	1	= (inet, inet),
	FUNCTION	1	hashinet(inet),
	FUNCTION	4	network_cmp(inet, inet);

CREATE OPERATOR CLASS jsonb_ops DEFAULT FOR TYPE jsonb USING lion AS
	OPERATOR	1	= (jsonb, jsonb),
	FUNCTION	1	jsonb_hash(jsonb),
	FUNCTION	4	jsonb_cmp(jsonb, jsonb);

CREATE OPERATOR CLASS pg_lsn_ops DEFAULT FOR TYPE pg_lsn USING lion AS
	OPERATOR	1	= (pg_lsn, pg_lsn),
	FUNCTION	1	pg_lsn_hash(pg_lsn),
	FUNCTION	4	pg_lsn_cmp(pg_lsn, pg_lsn);

/*
 * xid and cid have no btree opclass, so they get no support function 4: their
 * directories are ordered by (hash, stored bytes) alone, which is a complete
 * order but not the type's, and lion_index_stats() reports ordered = false.
 */
CREATE OPERATOR CLASS xid_ops DEFAULT FOR TYPE xid USING lion AS
	OPERATOR	1	= (xid, xid),
	FUNCTION	1	hashxid(xid);

CREATE OPERATOR CLASS xid8_ops DEFAULT FOR TYPE xid8 USING lion AS
	OPERATOR	1	= (xid8, xid8),
	FUNCTION	1	hashxid8(xid8),
	FUNCTION	4	xid8cmp(xid8, xid8);

CREATE OPERATOR CLASS cid_ops DEFAULT FOR TYPE cid USING lion AS
	OPERATOR	1	= (cid, cid),
	FUNCTION	1	hashcid(cid);

CREATE OPERATOR CLASS tid_ops DEFAULT FOR TYPE tid USING lion AS
	OPERATOR	1	= (tid, tid),
	FUNCTION	1	hashtid(tid),
	FUNCTION	4	bttidcmp(tid, tid);

CREATE OPERATOR CLASS enum_ops DEFAULT FOR TYPE anyenum USING lion AS
	OPERATOR	1	= (anyenum, anyenum),
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

CREATE OPERATOR CLASS tsvector_ops DEFAULT FOR TYPE tsvector USING lion AS
	OPERATOR	5	@@ (tsvector, tsquery),
	FUNCTION	1	hashtext(text),
	FUNCTION	4	bttextcmp(text, text),
	FUNCTION	2	gin_extract_tsvector(tsvector, internal, internal),
	FUNCTION	3	gin_extract_tsquery(tsvector, internal, int2, internal, internal, internal, internal),
	STORAGE		text;

/* ---------------------------------------------------------------------
 * count functions (lion_count.c)
 *
 * Heap-skipping count(*) over the posting sets of one or two roaring
 * indexes, interlocked with the visibility map (DESIGN.md section 9).  The
 * result is always exactly count(*) of the equivalent SELECT under the same
 * snapshot, so these are VOLATILE.
 * --------------------------------------------------------------------- */

CREATE FUNCTION lion_index_count(idx regclass, key anyelement)
RETURNS bigint
AS 'MODULE_PATHNAME', 'lion_index_count'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

COMMENT ON FUNCTION lion_index_count(regclass, anyelement) IS
	'count rows with key from a lion index, skipping all-visible heap pages';

CREATE FUNCTION lion_index_count(idx1 regclass, key1 anyelement,
									idx2 regclass, key2 anyelement)
RETURNS bigint
AS 'MODULE_PATHNAME', 'lion_index_count2'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

COMMENT ON FUNCTION lion_index_count(regclass, anyelement, regclass, anyelement) IS
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
									OUT inline_slack_bytes int8)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'lion_index_stats'
LANGUAGE C STRICT VOLATILE PARALLEL RESTRICTED;

COMMENT ON FUNCTION lion_index_stats(regclass) IS
	'shape of a lion index, one row per key column: directory shape, entries, containers by kind, sparse segments, posting trees, NULL keys and key-less rows';

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
