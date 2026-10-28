/* pg_lion--0.1--0.1.0-dev1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_lion UPDATE" to load this file. \quit

/*
 * The catch-up from any '0.1' (DESIGN.md §23, "Extension versions").
 *
 * Version 0.1 was never released, and its install script changed many times
 * under that one name, so a database may hold any of those versions of it:
 * functions since removed or given other arguments or OUT columns, operator
 * classes without the strategies added since, support functions since
 * replaced.  This script brings any of them to 0.1.0-dev1 without touching an
 * index.
 *
 * It creates today's objects in a scratch schema first, as the install script
 * does, and then reconciles the installed ones with them:
 *
 *	- a function whose arguments and result are unchanged is replaced in place
 *	  (CREATE OR REPLACE), which keeps the grants and the objects that depend
 *	  on it; any other is dropped, and today's moved into the extension's
 *	  schema.  A view or function of the user's over a dropped one stops the
 *	  update, with core's error naming it.
 *	- an operator family no index uses is dropped and today's moved in; one an
 *	  index uses is brought level member by member: missing operators and
 *	  support functions added, a class's default flag set, and a support
 *	  function or operator a class holds that is now another one switched in
 *	  the catalogs, as no DDL can (an index on it was built with the old one,
 *	  and its format is refused until it is rebuilt with REINDEX anyway).
 *	- a class indexes use that moved to another family (date_ops, timestamp_ops
 *	  and timestamptz_ops, now in datetime_ops) joins today's family and takes
 *	  today's members for its type, in the catalogs; its old family goes.
 *	- a family that is gone but that indexes still use is detached from the
 *	  extension, with a WARNING, rather than dropped with them.
 *
 * test/upgrade/run.sh installs every version of the 0.1 script in the
 * repository's history, with a lion index on every operator class, updates
 * it with this script, and requires the catalogs to match a fresh install.
 */

CREATE SCHEMA lion_upgrade_new;
SELECT pg_catalog.set_config('lion_upgrade.schema', pg_catalog.current_schema(), true);
SELECT pg_catalog.set_config('search_path', 'lion_upgrade_new', true);

/* ---- today's functions, in the scratch schema ---- */

CREATE FUNCTION lion_handler(internal)
RETURNS index_am_handler
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

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

CREATE FUNCTION lion_array_extract_value(anyarray, internal, internal)
	RETURNS internal AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION lion_array_extract_query(anyarray, internal, int2, internal, internal, internal, internal)
	RETURNS internal AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION lion_tsvector_positions(tsvector, internal) RETURNS internal
	AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION lion_tsvector_extract_value(tsvector, internal, internal)
	RETURNS internal AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION lion_tsvector_extract_query(tsvector, internal, int2, internal, internal, internal, internal)
	RETURNS internal AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION lion_jsonb_extract_value(jsonb, internal, internal)
	RETURNS internal AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION lion_jsonb_extract_query(jsonb, internal, int2, internal, internal, internal, internal)
	RETURNS internal AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION lion_bm25(index regclass, query tsquery, k int,
						  k1 float8 DEFAULT 1.2, b float8 DEFAULT 0.75,
						  col name DEFAULT NULL,
						  OUT ctid tid, OUT score float8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT VOLATILE
	PARALLEL RESTRICTED ROWS 10;

CREATE FUNCTION lion_bm25_score(doc tsvector, query tsquery, index regclass,
								k1 float8 DEFAULT 1.2, b float8 DEFAULT 0.75,
								col name DEFAULT NULL)
	RETURNS float8
	AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT STABLE
	PARALLEL RESTRICTED COST 10;

CREATE FUNCTION lion_index_count(idx regclass, key "any")
RETURNS bigint
AS 'MODULE_PATHNAME', 'lion_index_count'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

COMMENT ON FUNCTION lion_index_count(regclass, "any") IS
	'count rows with key from a lion index, skipping all-visible heap pages';

CREATE FUNCTION lion_index_count(idx1 regclass, key1 "any",
									idx2 regclass, key2 "any")
RETURNS bigint
AS 'MODULE_PATHNAME', 'lion_index_count2'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

COMMENT ON FUNCTION lion_index_count(regclass, "any", regclass, "any") IS
	'count rows matching both keys from two lion indexes on the same table';

CREATE FUNCTION lion_index_count_any(idx regclass, keys "any")
RETURNS bigint
AS 'MODULE_PATHNAME', 'lion_index_count_any'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

COMMENT ON FUNCTION lion_index_count_any(regclass, "any") IS
	'count rows whose key is in the array, skipping all-visible heap pages';

CREATE FUNCTION lion_index_count_stats(idx regclass, key "any",
										  OUT count bigint,
										  OUT blocks_skipped bigint,
										  OUT tids_rechecked bigint,
										  OUT blocks_rechecked bigint,
										  OUT cache_hits bigint)
RETURNS record
AS 'MODULE_PATHNAME', 'lion_index_count_stats'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

COMMENT ON FUNCTION lion_index_count_stats(regclass, "any") IS
	'lion_index_count() plus how much of the heap it had to visit';

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

CREATE FUNCTION lion_index_facets(idx regclass,
								  k integer DEFAULT NULL,
								  filter text DEFAULT NULL,
								  attno int2 DEFAULT 1,
								  OUT key text,
								  OUT count bigint)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'lion_index_facets'
LANGUAGE C CALLED ON NULL INPUT VOLATILE PARALLEL UNSAFE ROWS 10;

COMMENT ON FUNCTION lion_index_facets(regclass, integer, text, int2) IS
	'the keys of a lion index column with the most visible rows under a filter of lion-indexed conditions, counted exactly';

CREATE FUNCTION lion_index_slice_agg(idx regclass,
									 filter text DEFAULT NULL,
									 attno int2 DEFAULT 1,
									 OUT count bigint,
									 OUT sum numeric,
									 OUT min text,
									 OUT max text)
RETURNS record
AS 'MODULE_PATHNAME', 'lion_index_slice_agg'
LANGUAGE C CALLED ON NULL INPUT VOLATILE PARALLEL UNSAFE;

COMMENT ON FUNCTION lion_index_slice_agg(regclass, text, int2) IS
	'count, sum, min and max of a bit-sliced lion index column over its visible rows under a filter of lion-indexed conditions, from the slices alone, exactly';

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
									OUT narrow_containers int8,
									OUT nullelem_tids int8,
									OUT position_pages int8,
									OUT position_internal_pages int8,
									OUT emptydim_tids int8,
									OUT emptyinner_tids int8,
									OUT sliced bool,
									OUT slice_entries int8,
									OUT slice_tids int8,
									OUT slice_bytes int8,
									OUT slice_pages int8)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'lion_index_stats'
LANGUAGE C STRICT VOLATILE PARALLEL RESTRICTED;

COMMENT ON FUNCTION lion_index_stats(regclass) IS
	'shape of a lion index, one row per key column: directory shape, entries, containers by kind (narrow_containers, DESIGN.md §38), sparse segments, posting trees, NULL keys and key-less rows (empty_tids, and emptydim_tids and emptyinner_tids for empty arrays with dimensions, nullelem_tids for arrays of NULLs, §17), summary posting sets, the distinct keys the planner is given, the pages of the position trees (§17), and whether the column is bit-sliced with what its slices hold (§51)';

CREATE FUNCTION lion_index_posting_root(idx regclass, key anyelement)
RETURNS int8
AS 'MODULE_PATHNAME', 'lion_index_posting_root'
LANGUAGE C STRICT VOLATILE PARALLEL RESTRICTED;

COMMENT ON FUNCTION lion_index_posting_root(regclass, anyelement) IS
	'root block of one key''s posting tree, or NULL when it is still inline';

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

REVOKE EXECUTE ON FUNCTION lion_index_posting_root(regclass, anyelement) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION lion_index_verify(regclass, bool) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION lion_index_wal_mode(regclass) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION lion_index_stats(regclass) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION lion_index_stats(regclass) TO pg_stat_scan_tables;

/* ---- reconcile the functions ---- */

DO $$
DECLARE
	ename	text := pg_catalog.current_setting('lion_upgrade.schema');
	e		oid := pg_catalog.to_regnamespace(pg_catalog.quote_ident(ename));
	s		oid := pg_catalog.to_regnamespace('lion_upgrade_new');
	ext		oid := (SELECT oid FROM pg_catalog.pg_extension WHERE extname = 'pg_lion');
	seen	text[] := '{}';
	r		record;
	o		record;
	def		text;
	what	text;
BEGIN
	FOR r IN
		SELECT p.oid, p.proname, p.prokind, p.proacl,
			   pg_catalog.oidvectortypes(p.proargtypes) AS idargs,
			   pg_catalog.pg_get_function_arguments(p.oid) AS args,
			   pg_catalog.pg_get_function_result(p.oid) AS res,
			   pg_catalog.obj_description(p.oid, 'pg_proc') AS descr
		FROM pg_catalog.pg_proc p
		WHERE p.pronamespace = s
		ORDER BY p.proname, 5
	LOOP
		seen := seen || (r.proname || '(' || r.idargs || ')');
		what := CASE r.prokind WHEN 'a' THEN 'AGGREGATE' ELSE 'FUNCTION' END;

		SELECT p.oid, p.prokind, p.proacl,
			   pg_catalog.pg_get_function_arguments(p.oid) AS args,
			   pg_catalog.pg_get_function_result(p.oid) AS res,
			   EXISTS (SELECT 1 FROM pg_catalog.pg_depend d
					   WHERE d.classid = 'pg_catalog.pg_proc'::pg_catalog.regclass
						 AND d.objid = p.oid AND d.refobjid = ext AND d.deptype = 'e') AS member
		INTO o
		FROM pg_catalog.pg_proc p
		WHERE p.pronamespace = e AND p.proname = r.proname
		  AND pg_catalog.oidvectortypes(p.proargtypes) = r.idargs;

		IF FOUND AND NOT o.member THEN
			RAISE EXCEPTION 'function %.%(%) is not part of pg_lion and is in the way',
				ename, r.proname, r.idargs;
		END IF;

		IF FOUND AND o.prokind = r.prokind AND o.args = r.args AND o.res = r.res
		   AND (r.prokind <> 'a' OR (SELECT (a.aggtransfn, a.aggtranstype, a.aggfinalfn, a.aggcombinefn, a.agginitval)
										IS NOT DISTINCT FROM
										(b.aggtransfn, b.aggtranstype, b.aggfinalfn, b.aggcombinefn, b.agginitval)
									 FROM pg_catalog.pg_aggregate a, pg_catalog.pg_aggregate b
									 WHERE a.aggfnoid = o.oid AND b.aggfnoid = r.oid)) THEN
			-- the same function: replace it in place
			IF r.prokind <> 'a' THEN
				def := pg_catalog.pg_get_functiondef(r.oid);
				def := pg_catalog.regexp_replace(def, '^CREATE OR REPLACE FUNCTION lion_upgrade_new\.',
												 'CREATE OR REPLACE FUNCTION ' || pg_catalog.quote_ident(ename) || '.');
				EXECUTE def;
			END IF;
			EXECUTE pg_catalog.format('COMMENT ON %s %s IS %L', what, o.oid::pg_catalog.regprocedure, r.descr);
			-- the grants today's script makes; the user's own are kept
			IF r.proacl IS NOT NULL THEN
				IF NOT EXISTS (SELECT 1 FROM pg_catalog.aclexplode(r.proacl) x WHERE x.grantee = 0) THEN
					EXECUTE pg_catalog.format('REVOKE EXECUTE ON FUNCTION %s FROM PUBLIC', o.oid::pg_catalog.regprocedure);
				END IF;
				FOR def IN
					SELECT pg_catalog.quote_ident(a.rolname)
					FROM pg_catalog.aclexplode(r.proacl) x JOIN pg_catalog.pg_roles a ON a.oid = x.grantee
					WHERE x.grantee <> x.grantor
				LOOP
					EXECUTE pg_catalog.format('GRANT EXECUTE ON FUNCTION %s TO %s', o.oid::pg_catalog.regprocedure, def);
				END LOOP;
			END IF;
			EXECUTE pg_catalog.format('DROP %s lion_upgrade_new.%I(%s)', what, r.proname, r.idargs);
		ELSE
			IF FOUND THEN
				-- another function under the same name and argument types
				EXECUTE pg_catalog.format('DROP %s %s',
					CASE o.prokind WHEN 'a' THEN 'AGGREGATE' ELSE 'FUNCTION' END, o.oid::pg_catalog.regprocedure);
			END IF;
			EXECUTE pg_catalog.format('ALTER %s lion_upgrade_new.%I(%s) SET SCHEMA %I', what, r.proname, r.idargs, ename);
		END IF;
	END LOOP;

	PERFORM pg_catalog.set_config('lion_upgrade.functions', seen::text, true);

	-- functions today's script no longer has, unless an operator class or the
	-- access method still names them (those go after the classes, below)
	FOR o IN
		SELECT p.oid, p.prokind
		FROM pg_catalog.pg_proc p
		JOIN pg_catalog.pg_depend d ON d.classid = 'pg_catalog.pg_proc'::pg_catalog.regclass
			AND d.objid = p.oid AND d.refobjid = ext AND d.deptype = 'e'
		WHERE NOT (p.proname || '(' || pg_catalog.oidvectortypes(p.proargtypes) || ')' = ANY (seen))
		  AND NOT EXISTS (SELECT 1 FROM pg_catalog.pg_amproc a WHERE a.amproc = p.oid)
		  AND NOT EXISTS (SELECT 1 FROM pg_catalog.pg_am a WHERE a.amhandler = p.oid)
	LOOP
		EXECUTE pg_catalog.format('DROP %s %s',
			CASE o.prokind WHEN 'a' THEN 'AGGREGATE' ELSE 'FUNCTION' END, o.oid::pg_catalog.regprocedure);
	END LOOP;

	-- clear the installed classes' default flags, so that today's classes can
	-- be created as the defaults they are; the reconciliation below sets the
	-- flag of every class that stays
	UPDATE pg_catalog.pg_opclass c SET opcdefault = false
	FROM pg_catalog.pg_depend d
	WHERE d.classid = 'pg_catalog.pg_opclass'::pg_catalog.regclass AND d.objid = c.oid
	  AND d.refobjid = ext AND d.deptype = 'e' AND c.opcdefault;
END
$$;

/* ---- today's operator classes, in the scratch schema ---- */

SELECT pg_catalog.set_config('search_path',
	'lion_upgrade_new, ' || pg_catalog.quote_ident(pg_catalog.current_setting('lion_upgrade.schema')), true);

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

CREATE OPERATOR CLASS oid_ops DEFAULT FOR TYPE oid USING lion AS
	OPERATOR	1	= (oid, oid),
	OPERATOR	6	< (oid, oid),
	OPERATOR	7	<= (oid, oid),
	OPERATOR	8	>= (oid, oid),
	OPERATOR	9	> (oid, oid),
	OPERATOR	10	<> (oid, oid),
	FUNCTION	1	hashoid(oid),
	FUNCTION	4	btoidcmp(oid, oid);

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

CREATE OPERATOR FAMILY datetime_ops USING lion;

SELECT lion_create_opclass_pre18('CREATE OPERATOR CLASS date_ops DEFAULT FOR TYPE date USING lion
	FAMILY datetime_ops AS
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

CREATE OPERATOR CLASS timestamp_ops DEFAULT FOR TYPE timestamp USING lion
	FAMILY datetime_ops AS
	OPERATOR	1	= (timestamp, timestamp),
	OPERATOR	6	< (timestamp, timestamp),
	OPERATOR	7	<= (timestamp, timestamp),
	OPERATOR	8	>= (timestamp, timestamp),
	OPERATOR	9	> (timestamp, timestamp),
	OPERATOR	10	<> (timestamp, timestamp),
	FUNCTION	1	timestamp_hash(timestamp),
	FUNCTION	4	timestamp_cmp(timestamp, timestamp);

SELECT lion_create_opclass_pre18('CREATE OPERATOR CLASS timestamptz_ops DEFAULT FOR TYPE timestamptz USING lion
	FAMILY datetime_ops AS
	OPERATOR	1	= (timestamptz, timestamptz),
	OPERATOR	6	< (timestamptz, timestamptz),
	OPERATOR	7	<= (timestamptz, timestamptz),
	OPERATOR	8	>= (timestamptz, timestamptz),
	OPERATOR	9	> (timestamptz, timestamptz),
	OPERATOR	10	<> (timestamptz, timestamptz),
	FUNCTION	1	%s,
	FUNCTION	4	timestamptz_cmp(timestamptz, timestamptz)', 'timestamptz_hash(timestamptz)', 'timestamp_hash(timestamp)');

ALTER OPERATOR FAMILY datetime_ops USING lion ADD
	OPERATOR	1	= (date, timestamp),
	OPERATOR	6	< (date, timestamp),
	OPERATOR	7	<= (date, timestamp),
	OPERATOR	8	>= (date, timestamp),
	OPERATOR	9	> (date, timestamp),
	OPERATOR	10	<> (date, timestamp),
	OPERATOR	1	= (date, timestamptz),
	OPERATOR	6	< (date, timestamptz),
	OPERATOR	7	<= (date, timestamptz),
	OPERATOR	8	>= (date, timestamptz),
	OPERATOR	9	> (date, timestamptz),
	OPERATOR	10	<> (date, timestamptz),
	OPERATOR	1	= (timestamp, date),
	OPERATOR	6	< (timestamp, date),
	OPERATOR	7	<= (timestamp, date),
	OPERATOR	8	>= (timestamp, date),
	OPERATOR	9	> (timestamp, date),
	OPERATOR	10	<> (timestamp, date),
	OPERATOR	1	= (timestamptz, date),
	OPERATOR	6	< (timestamptz, date),
	OPERATOR	7	<= (timestamptz, date),
	OPERATOR	8	>= (timestamptz, date),
	OPERATOR	9	> (timestamptz, date),
	OPERATOR	10	<> (timestamptz, date),
	OPERATOR	1	= (timestamptz, timestamp),
	OPERATOR	6	< (timestamptz, timestamp),
	OPERATOR	7	<= (timestamptz, timestamp),
	OPERATOR	8	>= (timestamptz, timestamp),
	OPERATOR	9	> (timestamptz, timestamp),
	OPERATOR	10	<> (timestamptz, timestamp),
	FUNCTION	4	(date, timestamp) date_cmp_timestamp(date, timestamp),
	FUNCTION	4	(date, timestamptz) date_cmp_timestamptz(date, timestamptz),
	FUNCTION	4	(timestamp, date) timestamp_cmp_date(timestamp, date),
	FUNCTION	4	(timestamptz, date) timestamptz_cmp_date(timestamptz, date),
	FUNCTION	4	(timestamptz, timestamp) timestamptz_cmp_timestamp(timestamptz, timestamp);

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

CREATE OPERATOR CLASS jsonb_ops FOR TYPE jsonb USING lion AS
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

CREATE OPERATOR CLASS array_ops DEFAULT FOR TYPE anyarray USING lion AS
	OPERATOR	2	@> (anyarray, anyarray),
	OPERATOR	3	&& (anyarray, anyarray),
	OPERATOR	4	<@ (anyarray, anyarray),
	OPERATOR	17	= (anyarray, anyarray),
	FUNCTION	2	lion_array_extract_value(anyarray, internal, internal),
	FUNCTION	3	lion_array_extract_query(anyarray, internal, int2, internal, internal, internal, internal),
	STORAGE		anyelement;

CREATE OPERATOR CLASS tsvector_ops DEFAULT FOR TYPE tsvector USING lion AS
	OPERATOR	5	@@ (tsvector, tsquery),
	OPERATOR	18	@@@ (tsvector, tsquery),
	FUNCTION	1	hashtext(text),
	FUNCTION	4	bttextcmp(text, text),
	FUNCTION	2	lion_tsvector_extract_value(tsvector, internal, internal),
	FUNCTION	3	lion_tsvector_extract_query(tsvector, internal, int2, internal, internal, internal, internal),
	FUNCTION	5	lion_tsvector_positions(tsvector, internal),
	STORAGE		text;

SELECT lion_create_opclass_pre18('CREATE OPERATOR CLASS jsonb_contains_ops DEFAULT FOR TYPE jsonb USING lion AS
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

/* ---- reconcile the operator families and classes ---- */

DO $$
DECLARE
	ename	text := pg_catalog.current_setting('lion_upgrade.schema');
	en		text := pg_catalog.quote_ident(ename);
	e		oid := pg_catalog.to_regnamespace(en);
	s		oid := pg_catalog.to_regnamespace('lion_upgrade_new');
	am		oid := (SELECT oid FROM pg_catalog.pg_am WHERE amname = 'lion');
	ext		oid := (SELECT oid FROM pg_catalog.pg_extension WHERE extname = 'pg_lion');
	seen	name[] := '{}';
	sf		record;
	ef		oid;
	c		record;
	m		record;
	ec		record;
	ops		oid[];
	procs	oid[];
BEGIN
	FOR sf IN
		SELECT f.oid, f.opfname FROM pg_catalog.pg_opfamily f
		WHERE f.opfnamespace = s AND f.opfmethod = am ORDER BY f.opfname
	LOOP
		seen := seen || sf.opfname;
		SELECT f.oid INTO ef FROM pg_catalog.pg_opfamily f
		WHERE f.opfnamespace = e AND f.opfmethod = am AND f.opfname = sf.opfname;

		-- a family no index uses is simply replaced
		IF ef IS NOT NULL AND NOT EXISTS (
			SELECT 1 FROM pg_catalog.pg_index i, pg_catalog.pg_opclass oc
			WHERE oc.opcfamily = ef AND oc.oid = ANY (i.indclass::oid[])) THEN
			EXECUTE pg_catalog.format('DROP OPERATOR FAMILY %s.%I USING lion', en, sf.opfname);
			ef := NULL;
		END IF;

		IF ef IS NULL THEN
			FOR c IN SELECT oc.* FROM pg_catalog.pg_opclass oc WHERE oc.opcfamily = sf.oid LOOP
				-- a class of this name in another family moves out of the way
				SELECT oc.* INTO ec FROM pg_catalog.pg_opclass oc
				WHERE oc.opcnamespace = e AND oc.opcmethod = am AND oc.opcname = c.opcname;
				CONTINUE WHEN NOT FOUND;
				IF NOT EXISTS (SELECT 1 FROM pg_catalog.pg_index i WHERE ec.oid = ANY (i.indclass::oid[])) THEN
					EXECUTE pg_catalog.format('DROP OPERATOR CLASS %s.%I USING lion', en, c.opcname);
					CONTINUE;
				END IF;

				-- Indexes use it (as date_ops, timestamp_ops and timestamptz_ops
				-- moved from families of their own into datetime_ops): the
				-- installed class, which the indexes name, joins today's family
				-- and takes over today's class's members, in the catalogs, as no
				-- DDL can.  Its own members, in the old family, go.
				IF ec.opcintype <> c.opcintype OR ec.opckeytype <> c.opckeytype THEN
					RAISE EXCEPTION 'operator class %.% changed in a way this update cannot make while indexes use it',
						ename, c.opcname
						USING HINT = 'Drop those indexes, update, and create them again.';
				END IF;
				ops := ARRAY(SELECT d.objid FROM pg_catalog.pg_depend d
							 WHERE d.classid = 'pg_catalog.pg_amop'::pg_catalog.regclass AND d.deptype = 'i'
							   AND d.refclassid = 'pg_catalog.pg_opclass'::pg_catalog.regclass AND d.refobjid = ec.oid);
				procs := ARRAY(SELECT d.objid FROM pg_catalog.pg_depend d
							   WHERE d.classid = 'pg_catalog.pg_amproc'::pg_catalog.regclass AND d.deptype = 'i'
								 AND d.refclassid = 'pg_catalog.pg_opclass'::pg_catalog.regclass AND d.refobjid = ec.oid);
				DELETE FROM pg_catalog.pg_amop WHERE oid = ANY (ops);
				DELETE FROM pg_catalog.pg_amproc WHERE oid = ANY (procs);
				DELETE FROM pg_catalog.pg_depend
				WHERE (classid = 'pg_catalog.pg_amop'::pg_catalog.regclass AND objid = ANY (ops))
				   OR (classid = 'pg_catalog.pg_amproc'::pg_catalog.regclass AND objid = ANY (procs));
				UPDATE pg_catalog.pg_depend SET refobjid = ec.oid
				WHERE refclassid = 'pg_catalog.pg_opclass'::pg_catalog.regclass AND refobjid = c.oid
				  AND classid IN ('pg_catalog.pg_amop'::pg_catalog.regclass, 'pg_catalog.pg_amproc'::pg_catalog.regclass);
				UPDATE pg_catalog.pg_depend SET refobjid = sf.oid
				WHERE classid = 'pg_catalog.pg_opclass'::pg_catalog.regclass AND objid = ec.oid
				  AND refclassid = 'pg_catalog.pg_opfamily'::pg_catalog.regclass AND refobjid = ec.opcfamily;
				UPDATE pg_catalog.pg_opclass SET opcfamily = sf.oid, opcdefault = c.opcdefault WHERE oid = ec.oid;
				EXECUTE pg_catalog.format('DROP OPERATOR CLASS lion_upgrade_new.%I USING lion', c.opcname);
				-- the old family, empty now unless other classes still hold it
				IF NOT EXISTS (SELECT 1 FROM pg_catalog.pg_opclass oc WHERE oc.opcfamily = ec.opcfamily)
				   AND NOT EXISTS (SELECT 1 FROM pg_catalog.pg_amop WHERE amopfamily = ec.opcfamily)
				   AND NOT EXISTS (SELECT 1 FROM pg_catalog.pg_amproc WHERE amprocfamily = ec.opcfamily) THEN
					EXECUTE pg_catalog.format('DROP OPERATOR FAMILY %s USING lion',
						(SELECT pg_catalog.quote_ident(ns.nspname) || '.' || pg_catalog.quote_ident(f.opfname)
						 FROM pg_catalog.pg_opfamily f JOIN pg_catalog.pg_namespace ns ON ns.oid = f.opfnamespace
						 WHERE f.oid = ec.opcfamily));
				END IF;
				-- other sessions must not keep the indexes' old family cached
				UPDATE pg_catalog.pg_class SET relname = relname
				WHERE oid IN (SELECT i.indexrelid FROM pg_catalog.pg_index i WHERE ec.oid = ANY (i.indclass::oid[]));
			END LOOP;
			EXECUTE pg_catalog.format('ALTER OPERATOR FAMILY lion_upgrade_new.%I USING lion SET SCHEMA %s', sf.opfname, en);
			FOR c IN SELECT oc.opcname FROM pg_catalog.pg_opclass oc WHERE oc.opcfamily = sf.oid AND oc.opcnamespace = s LOOP
				EXECUTE pg_catalog.format('ALTER OPERATOR CLASS lion_upgrade_new.%I USING lion SET SCHEMA %s', c.opcname, en);
			END LOOP;
			CONTINUE;
		END IF;

		-- indexes use the family: bring it level in place.  Its classes first.
		FOR c IN SELECT oc.* FROM pg_catalog.pg_opclass oc WHERE oc.opcfamily = sf.oid LOOP
			SELECT oc.* INTO ec FROM pg_catalog.pg_opclass oc
			WHERE oc.opcnamespace = e AND oc.opcmethod = am AND oc.opcname = c.opcname;
			IF NOT FOUND OR ec.opcfamily <> ef OR ec.opcintype <> c.opcintype OR ec.opckeytype <> c.opckeytype THEN
				RAISE EXCEPTION 'operator class %.% changed in a way this update cannot make while indexes use family %',
					ename, c.opcname, sf.opfname
					USING HINT = 'Drop those indexes, update, and create them again.';
			END IF;
			UPDATE pg_catalog.pg_opclass SET opcdefault = c.opcdefault WHERE oid = ec.oid;
		END LOOP;
		FOR c IN
			SELECT oc.oid, oc.opcname FROM pg_catalog.pg_opclass oc
			WHERE oc.opcfamily = ef
			  AND NOT EXISTS (SELECT 1 FROM pg_catalog.pg_opclass n WHERE n.opcfamily = sf.oid AND n.opcname = oc.opcname)
		LOOP
			IF EXISTS (SELECT 1 FROM pg_catalog.pg_index i WHERE c.oid = ANY (i.indclass::oid[])) THEN
				RAISE EXCEPTION 'operator class %.% is gone, and indexes use it', ename, c.opcname
					USING HINT = 'Drop those indexes, update, and create them again.';
			END IF;
			EXECUTE pg_catalog.format('DROP OPERATOR CLASS %s.%I USING lion', en, c.opcname);
		END LOOP;

		-- Operators, by (strategy, left type, right type).  A member a class
		-- holds cannot be dropped or replaced with ALTER OPERATOR FAMILY, so a
		-- held operator that changed is switched in the catalogs.
		FOR m IN
			SELECT coalesce(n.amopstrategy, o.amopstrategy) AS strategy,
				   coalesce(n.amoplefttype, o.amoplefttype) AS lt,
				   coalesce(n.amoprighttype, o.amoprighttype) AS rt,
				   n.oid AS noid, n.amopopr AS nopr, n.amoppurpose AS npurpose, n.amopsortfamily AS nsort,
				   o.oid AS ooid, o.amopopr AS oopr, o.amoppurpose AS opurpose, o.amopsortfamily AS osort,
				   EXISTS (SELECT 1 FROM pg_catalog.pg_depend d
						   WHERE d.classid = 'pg_catalog.pg_amop'::pg_catalog.regclass
							 AND d.objid = o.oid AND d.deptype = 'i') AS held
			FROM (SELECT * FROM pg_catalog.pg_amop WHERE amopfamily = sf.oid) n
			FULL JOIN (SELECT * FROM pg_catalog.pg_amop WHERE amopfamily = ef) o
				ON n.amopstrategy = o.amopstrategy AND n.amoplefttype = o.amoplefttype
				   AND n.amoprighttype = o.amoprighttype
		LOOP
			CONTINUE WHEN m.noid IS NOT NULL AND m.ooid IS NOT NULL AND m.nopr = m.oopr
				AND m.npurpose = m.opurpose AND m.nsort = m.osort;
			IF m.ooid IS NOT NULL AND m.held THEN
				IF m.noid IS NULL OR m.npurpose <> m.opurpose OR m.nsort <> m.osort THEN
					RAISE EXCEPTION 'operator % of family %.% changed in a way this update cannot make while indexes use it',
						m.strategy, ename, sf.opfname
						USING HINT = 'Drop those indexes, update, and create them again.';
				END IF;
				UPDATE pg_catalog.pg_amop SET amopopr = m.nopr WHERE oid = m.ooid;
				UPDATE pg_catalog.pg_depend SET refobjid = m.nopr
				WHERE classid = 'pg_catalog.pg_amop'::pg_catalog.regclass AND objid = m.ooid
				  AND refclassid = 'pg_catalog.pg_operator'::pg_catalog.regclass AND refobjid = m.oopr;
				CONTINUE;
			END IF;
			IF m.ooid IS NOT NULL THEN
				EXECUTE pg_catalog.format('ALTER OPERATOR FAMILY %s.%I USING lion DROP OPERATOR %s (%s, %s)',
					en, sf.opfname, m.strategy, pg_catalog.format_type(m.lt, NULL), pg_catalog.format_type(m.rt, NULL));
			END IF;
			IF m.noid IS NOT NULL THEN
				EXECUTE pg_catalog.format('ALTER OPERATOR FAMILY %s.%I USING lion ADD OPERATOR %s %s (%s, %s)%s',
					en, sf.opfname, m.strategy,
					(SELECT pg_catalog.quote_ident(ns.nspname) || '.' || op.oprname
					 FROM pg_catalog.pg_operator op JOIN pg_catalog.pg_namespace ns ON ns.oid = op.oprnamespace
					 WHERE op.oid = m.nopr),
					pg_catalog.format_type(m.lt, NULL), pg_catalog.format_type(m.rt, NULL),
					CASE WHEN m.npurpose = 'o' THEN
						(SELECT ' FOR ORDER BY ' || pg_catalog.quote_ident(ns.nspname) || '.' || pg_catalog.quote_ident(sf2.opfname)
						 FROM pg_catalog.pg_opfamily sf2 JOIN pg_catalog.pg_namespace ns ON ns.oid = sf2.opfnamespace
						 WHERE sf2.oid = m.nsort)
					ELSE '' END);
			END IF;
		END LOOP;

		-- support functions, likewise, by (number, left type, right type)
		FOR m IN
			SELECT coalesce(n.amprocnum, o.amprocnum) AS num,
				   coalesce(n.amproclefttype, o.amproclefttype) AS lt,
				   coalesce(n.amprocrighttype, o.amprocrighttype) AS rt,
				   n.oid AS noid, n.amproc AS nproc,
				   o.oid AS ooid, o.amproc AS oproc,
				   EXISTS (SELECT 1 FROM pg_catalog.pg_depend d
						   WHERE d.classid = 'pg_catalog.pg_amproc'::pg_catalog.regclass
							 AND d.objid = o.oid AND d.deptype = 'i') AS held
			FROM (SELECT * FROM pg_catalog.pg_amproc WHERE amprocfamily = sf.oid) n
			FULL JOIN (SELECT * FROM pg_catalog.pg_amproc WHERE amprocfamily = ef) o
				ON n.amprocnum = o.amprocnum AND n.amproclefttype = o.amproclefttype
				   AND n.amprocrighttype = o.amprocrighttype
		LOOP
			CONTINUE WHEN m.noid IS NOT NULL AND m.ooid IS NOT NULL AND m.nproc = m.oproc;
			IF m.ooid IS NOT NULL AND m.held THEN
				IF m.noid IS NULL THEN
					RAISE EXCEPTION 'support function % of family %.% is gone, and indexes use it',
						m.num, ename, sf.opfname
						USING HINT = 'Drop those indexes, update, and create them again.';
				END IF;
				UPDATE pg_catalog.pg_amproc SET amproc = m.nproc WHERE oid = m.ooid;
				UPDATE pg_catalog.pg_depend SET refobjid = m.nproc
				WHERE classid = 'pg_catalog.pg_amproc'::pg_catalog.regclass AND objid = m.ooid
				  AND refclassid = 'pg_catalog.pg_proc'::pg_catalog.regclass AND refobjid = m.oproc;
				CONTINUE;
			END IF;
			IF m.ooid IS NOT NULL THEN
				EXECUTE pg_catalog.format('ALTER OPERATOR FAMILY %s.%I USING lion DROP FUNCTION %s (%s, %s)',
					en, sf.opfname, m.num, pg_catalog.format_type(m.lt, NULL), pg_catalog.format_type(m.rt, NULL));
			END IF;
			IF m.noid IS NOT NULL THEN
				EXECUTE pg_catalog.format('ALTER OPERATOR FAMILY %s.%I USING lion ADD FUNCTION %s (%s, %s) %s',
					en, sf.opfname, m.num, pg_catalog.format_type(m.lt, NULL), pg_catalog.format_type(m.rt, NULL),
					m.nproc::pg_catalog.regprocedure);
			END IF;
		END LOOP;

		EXECUTE pg_catalog.format('DROP OPERATOR FAMILY lion_upgrade_new.%I USING lion', sf.opfname);
	END LOOP;

	-- families today's script no longer has: dropped, or detached from the
	-- extension where indexes still use them
	FOR c IN
		SELECT f.oid, f.opfname FROM pg_catalog.pg_opfamily f
		JOIN pg_catalog.pg_depend d ON d.classid = 'pg_catalog.pg_opfamily'::pg_catalog.regclass
			AND d.objid = f.oid AND d.refobjid = ext AND d.deptype = 'e'
		WHERE NOT (f.opfname = ANY (seen))
	LOOP
		IF NOT EXISTS (SELECT 1 FROM pg_catalog.pg_index i, pg_catalog.pg_opclass oc
					   WHERE oc.opcfamily = c.oid AND oc.oid = ANY (i.indclass::oid[])) THEN
			EXECUTE pg_catalog.format('DROP OPERATOR FAMILY %s.%I USING lion', en, c.opfname);
			CONTINUE;
		END IF;
		RAISE WARNING 'operator family %.% is no longer part of pg_lion, but indexes use it: it is left in place, detached from the extension',
			ename, c.opfname;
		FOR ec IN SELECT oc.opcname FROM pg_catalog.pg_opclass oc WHERE oc.opcfamily = c.oid LOOP
			EXECUTE pg_catalog.format('ALTER EXTENSION pg_lion DROP OPERATOR CLASS %s.%I USING lion', en, ec.opcname);
		END LOOP;
		EXECUTE pg_catalog.format('ALTER EXTENSION pg_lion DROP OPERATOR FAMILY %s.%I USING lion', en, c.opfname);
	END LOOP;
END
$$;

/* ---- the rest of today's script, and clearing up ---- */

DROP FUNCTION lion_create_opclass_pre18(text, text, text);

-- functions only the replaced families still named
DO $$
DECLARE
	o		record;
	ext		oid := (SELECT oid FROM pg_catalog.pg_extension WHERE extname = 'pg_lion');
BEGIN
	FOR o IN
		SELECT p.oid, p.prokind
		FROM pg_catalog.pg_proc p
		JOIN pg_catalog.pg_depend d ON d.classid = 'pg_catalog.pg_proc'::pg_catalog.regclass
			AND d.objid = p.oid AND d.refobjid = ext AND d.deptype = 'e'
		WHERE NOT (p.proname || '(' || pg_catalog.oidvectortypes(p.proargtypes) || ')'
				   = ANY (pg_catalog.current_setting('lion_upgrade.functions')::text[]))
	LOOP
		IF EXISTS (SELECT 1 FROM pg_catalog.pg_amproc a WHERE a.amproc = o.oid) THEN
			RAISE WARNING 'function % is no longer part of pg_lion, but a detached operator family uses it: it is left in place, detached from the extension',
				o.oid::pg_catalog.regprocedure;
			EXECUTE pg_catalog.format('ALTER EXTENSION pg_lion DROP FUNCTION %s', o.oid::pg_catalog.regprocedure);
		ELSE
			EXECUTE pg_catalog.format('DROP %s %s',
				CASE o.prokind WHEN 'a' THEN 'AGGREGATE' ELSE 'FUNCTION' END, o.oid::pg_catalog.regprocedure);
		END IF;
	END LOOP;
END
$$;

DROP SCHEMA lion_upgrade_new;

COMMENT ON ACCESS METHOD lion IS
	'roaring bitmap inverted index access method';
