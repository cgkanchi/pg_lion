/* roaring_index--0.1.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION roaring_index" to load this file. \quit

CREATE FUNCTION roaring_handler(internal)
RETURNS index_am_handler
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE ACCESS METHOD roaring TYPE INDEX HANDLER roaring_handler;

COMMENT ON ACCESS METHOD roaring IS
	'roaring bitmap inverted index access method';

/*
 * Operator classes.  A roaring opclass needs exactly what a hash opclass
 * needs: support function 1 is the type's hash function and strategy 1 is
 * equality, so every class below reuses the hash access method's function.
 */

/* ---- integers: one family, so cross-type equality can use the index ---- */

CREATE OPERATOR FAMILY integer_ops USING roaring;

CREATE OPERATOR CLASS int2_ops DEFAULT FOR TYPE int2 USING roaring
	FAMILY integer_ops AS
	OPERATOR	1	= (int2, int2),
	FUNCTION	1	hashint2(int2);

CREATE OPERATOR CLASS int4_ops DEFAULT FOR TYPE int4 USING roaring
	FAMILY integer_ops AS
	OPERATOR	1	= (int4, int4),
	FUNCTION	1	hashint4(int4);

CREATE OPERATOR CLASS int8_ops DEFAULT FOR TYPE int8 USING roaring
	FAMILY integer_ops AS
	OPERATOR	1	= (int8, int8),
	FUNCTION	1	hashint8(int8);

ALTER OPERATOR FAMILY integer_ops USING roaring ADD
	OPERATOR	1	= (int2, int4),
	OPERATOR	1	= (int2, int8),
	OPERATOR	1	= (int4, int2),
	OPERATOR	1	= (int4, int8),
	OPERATOR	1	= (int8, int2),
	OPERATOR	1	= (int8, int4);

/* ---- floats: likewise ---- */

CREATE OPERATOR FAMILY float_ops USING roaring;

CREATE OPERATOR CLASS float4_ops DEFAULT FOR TYPE float4 USING roaring
	FAMILY float_ops AS
	OPERATOR	1	= (float4, float4),
	FUNCTION	1	hashfloat4(float4);

CREATE OPERATOR CLASS float8_ops DEFAULT FOR TYPE float8 USING roaring
	FAMILY float_ops AS
	OPERATOR	1	= (float8, float8),
	FUNCTION	1	hashfloat8(float8);

ALTER OPERATOR FAMILY float_ops USING roaring ADD
	OPERATOR	1	= (float4, float8),
	OPERATOR	1	= (float8, float4);

/* ---- one class per remaining type ---- */

CREATE OPERATOR CLASS oid_ops DEFAULT FOR TYPE oid USING roaring AS
	OPERATOR	1	= (oid, oid),
	FUNCTION	1	hashoid(oid);

CREATE OPERATOR CLASS bool_ops DEFAULT FOR TYPE bool USING roaring AS
	OPERATOR	1	= (bool, bool),
	FUNCTION	1	hashbool(bool);

CREATE OPERATOR CLASS char_ops DEFAULT FOR TYPE "char" USING roaring AS
	OPERATOR	1	= ("char", "char"),
	FUNCTION	1	hashchar("char");

CREATE OPERATOR CLASS name_ops DEFAULT FOR TYPE name USING roaring AS
	OPERATOR	1	= (name, name),
	FUNCTION	1	hashname(name);

/* varchar reaches this class through binary coercion, as it does for hash */
CREATE OPERATOR CLASS text_ops DEFAULT FOR TYPE text USING roaring AS
	OPERATOR	1	= (text, text),
	FUNCTION	1	hashtext(text);

CREATE OPERATOR CLASS bpchar_ops DEFAULT FOR TYPE bpchar USING roaring AS
	OPERATOR	1	= (bpchar, bpchar),
	FUNCTION	1	hashbpchar(bpchar);

CREATE OPERATOR CLASS bytea_ops DEFAULT FOR TYPE bytea USING roaring AS
	OPERATOR	1	= (bytea, bytea),
	FUNCTION	1	hashbytea(bytea);

CREATE OPERATOR CLASS uuid_ops DEFAULT FOR TYPE uuid USING roaring AS
	OPERATOR	1	= (uuid, uuid),
	FUNCTION	1	uuid_hash(uuid);

CREATE OPERATOR CLASS date_ops DEFAULT FOR TYPE date USING roaring AS
	OPERATOR	1	= (date, date),
	FUNCTION	1	hashdate(date);

CREATE OPERATOR CLASS time_ops DEFAULT FOR TYPE time USING roaring AS
	OPERATOR	1	= (time, time),
	FUNCTION	1	time_hash(time);

CREATE OPERATOR CLASS timetz_ops DEFAULT FOR TYPE timetz USING roaring AS
	OPERATOR	1	= (timetz, timetz),
	FUNCTION	1	timetz_hash(timetz);

CREATE OPERATOR CLASS timestamp_ops DEFAULT FOR TYPE timestamp USING roaring AS
	OPERATOR	1	= (timestamp, timestamp),
	FUNCTION	1	timestamp_hash(timestamp);

CREATE OPERATOR CLASS timestamptz_ops DEFAULT FOR TYPE timestamptz USING roaring AS
	OPERATOR	1	= (timestamptz, timestamptz),
	FUNCTION	1	timestamptz_hash(timestamptz);

CREATE OPERATOR CLASS interval_ops DEFAULT FOR TYPE interval USING roaring AS
	OPERATOR	1	= (interval, interval),
	FUNCTION	1	interval_hash(interval);

CREATE OPERATOR CLASS numeric_ops DEFAULT FOR TYPE numeric USING roaring AS
	OPERATOR	1	= (numeric, numeric),
	FUNCTION	1	hash_numeric(numeric);

CREATE OPERATOR CLASS macaddr_ops DEFAULT FOR TYPE macaddr USING roaring AS
	OPERATOR	1	= (macaddr, macaddr),
	FUNCTION	1	hashmacaddr(macaddr);

CREATE OPERATOR CLASS macaddr8_ops DEFAULT FOR TYPE macaddr8 USING roaring AS
	OPERATOR	1	= (macaddr8, macaddr8),
	FUNCTION	1	hashmacaddr8(macaddr8);

CREATE OPERATOR CLASS inet_ops DEFAULT FOR TYPE inet USING roaring AS
	OPERATOR	1	= (inet, inet),
	FUNCTION	1	hashinet(inet);

CREATE OPERATOR CLASS jsonb_ops DEFAULT FOR TYPE jsonb USING roaring AS
	OPERATOR	1	= (jsonb, jsonb),
	FUNCTION	1	jsonb_hash(jsonb);

CREATE OPERATOR CLASS pg_lsn_ops DEFAULT FOR TYPE pg_lsn USING roaring AS
	OPERATOR	1	= (pg_lsn, pg_lsn),
	FUNCTION	1	pg_lsn_hash(pg_lsn);

CREATE OPERATOR CLASS xid_ops DEFAULT FOR TYPE xid USING roaring AS
	OPERATOR	1	= (xid, xid),
	FUNCTION	1	hashxid(xid);

CREATE OPERATOR CLASS xid8_ops DEFAULT FOR TYPE xid8 USING roaring AS
	OPERATOR	1	= (xid8, xid8),
	FUNCTION	1	hashxid8(xid8);

CREATE OPERATOR CLASS cid_ops DEFAULT FOR TYPE cid USING roaring AS
	OPERATOR	1	= (cid, cid),
	FUNCTION	1	hashcid(cid);

CREATE OPERATOR CLASS tid_ops DEFAULT FOR TYPE tid USING roaring AS
	OPERATOR	1	= (tid, tid),
	FUNCTION	1	hashtid(tid);

CREATE OPERATOR CLASS enum_ops DEFAULT FOR TYPE anyenum USING roaring AS
	OPERATOR	1	= (anyenum, anyenum),
	FUNCTION	1	hashenum(anyenum);

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
 * no support function 1 for that reason: the element type's own default hash
 * opclass is used instead (rbi_fill_state()).  tsvector_ops stores text and
 * can name hashtext().
 * --------------------------------------------------------------------- */

CREATE OPERATOR CLASS array_ops DEFAULT FOR TYPE anyarray USING roaring AS
	OPERATOR	2	@> (anyarray, anyarray),
	OPERATOR	3	&& (anyarray, anyarray),
	OPERATOR	4	<@ (anyarray, anyarray),
	FUNCTION	2	ginarrayextract(anyarray, internal, internal),
	FUNCTION	3	ginqueryarrayextract(anyarray, internal, int2, internal, internal, internal, internal),
	STORAGE		anyelement;

CREATE OPERATOR CLASS tsvector_ops DEFAULT FOR TYPE tsvector USING roaring AS
	OPERATOR	5	@@ (tsvector, tsquery),
	FUNCTION	1	hashtext(text),
	FUNCTION	2	gin_extract_tsvector(tsvector, internal, internal),
	FUNCTION	3	gin_extract_tsquery(tsvector, internal, int2, internal, internal, internal, internal),
	STORAGE		text;

/* ---------------------------------------------------------------------
 * count functions (rbi_count.c)
 *
 * Heap-skipping count(*) over the posting sets of one or two roaring
 * indexes, interlocked with the visibility map (DESIGN.md section 9).  The
 * result is always exactly count(*) of the equivalent SELECT under the same
 * snapshot, so these are VOLATILE.
 * --------------------------------------------------------------------- */

CREATE FUNCTION roaring_index_count(idx regclass, key anyelement)
RETURNS bigint
AS 'MODULE_PATHNAME', 'roaring_index_count'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

COMMENT ON FUNCTION roaring_index_count(regclass, anyelement) IS
	'count rows with key from a roaring index, skipping all-visible heap pages';

CREATE FUNCTION roaring_index_count(idx1 regclass, key1 anyelement,
									idx2 regclass, key2 anyelement)
RETURNS bigint
AS 'MODULE_PATHNAME', 'roaring_index_count2'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

COMMENT ON FUNCTION roaring_index_count(regclass, anyelement, regclass, anyelement) IS
	'count rows matching both keys from two roaring indexes on the same table';

/*
 * count(*) WHERE col = ANY (keys): the union of the listed values' posting
 * sets (DESIGN.md section 15), located in bucket order and merged k-way.
 */
CREATE FUNCTION roaring_index_count_any(idx regclass, keys anyarray)
RETURNS bigint
AS 'MODULE_PATHNAME', 'roaring_index_count_any'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

COMMENT ON FUNCTION roaring_index_count_any(regclass, anyarray) IS
	'count rows whose key is in the array, skipping all-visible heap pages';

CREATE FUNCTION roaring_index_count_stats(idx regclass, key anyelement,
										  OUT count bigint,
										  OUT blocks_skipped bigint,
										  OUT tids_rechecked bigint,
										  OUT blocks_rechecked bigint,
										  OUT cache_hits bigint)
RETURNS record
AS 'MODULE_PATHNAME', 'roaring_index_count_stats'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

COMMENT ON FUNCTION roaring_index_count_stats(regclass, anyelement) IS
	'roaring_index_count() plus how much of the heap it had to visit';

/*
 * Count every key of one index under one snapshot, sharing one per-query
 * visibility cache across the groups, exactly as the GROUP BY pushdown does
 * (DESIGN.md section 9).  cache_hits is the number of heap block visits the
 * cache answered without touching the buffer manager; cache_full counts the
 * visits that had to be fetched because the cache had used up work_mem.
 */
CREATE FUNCTION roaring_index_count_group_stats(idx regclass,
												use_cache boolean DEFAULT true,
												OUT groups bigint,
												OUT count bigint,
												OUT blocks_skipped bigint,
												OUT tids_rechecked bigint,
												OUT blocks_rechecked bigint,
												OUT cache_hits bigint,
												OUT cache_full bigint)
RETURNS record
AS 'MODULE_PATHNAME', 'roaring_index_count_group_stats'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

COMMENT ON FUNCTION roaring_index_count_group_stats(regclass, boolean) IS
	'count every key of a roaring index, as the GROUP BY pushdown does, and report the heap visits';

/* ------------------------------------------------------------------ */
-- stats/verify functions (rbi_funcs.c)
/* ------------------------------------------------------------------ */

CREATE FUNCTION roaring_index_stats(idx regclass,
									OUT nbuckets int4,
									OUT bucket_pages int8,
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
									OUT max_bucket_pages int8)
RETURNS record
AS 'MODULE_PATHNAME', 'roaring_index_stats'
LANGUAGE C STRICT VOLATILE PARALLEL RESTRICTED;

COMMENT ON FUNCTION roaring_index_stats(regclass) IS
	'shape of a roaring index: pages, entries, containers by kind, sparse segments, NULL keys and key-less rows';

CREATE FUNCTION roaring_index_verify(idx regclass,
									 heapallindexed bool DEFAULT false)
RETURNS void
AS 'MODULE_PATHNAME', 'roaring_index_verify'
LANGUAGE C STRICT VOLATILE PARALLEL RESTRICTED;

COMMENT ON FUNCTION roaring_index_verify(regclass, bool) IS
	'check a roaring index for structural damage, optionally also checking that every heap tuple is indexed';
