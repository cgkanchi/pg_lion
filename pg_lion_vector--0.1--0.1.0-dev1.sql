-- pg_lion_vector: the catch-up from any '0.1' (DESIGN.md §23, "Extension
-- versions").  Version 0.1 was never released, and its script gained the
-- halfvec operator classes under that one name; this creates them where they
-- are missing.
\echo Use "ALTER EXTENSION pg_lion_vector UPDATE" to load this file. \quit

DO $$
BEGIN
	IF NOT EXISTS (
		SELECT 1 FROM pg_catalog.pg_opclass c JOIN pg_catalog.pg_am a ON a.oid = c.opcmethod
		WHERE a.amname = 'lionvec' AND c.opcname = 'halfvec_l2_ops'
		  AND c.opcnamespace = pg_catalog.to_regnamespace(pg_catalog.quote_ident(pg_catalog.current_schema()))) THEN
		CREATE OPERATOR CLASS halfvec_l2_ops
			DEFAULT FOR TYPE @extschema:vector@.halfvec USING lionvec AS
				OPERATOR 1 @extschema:vector@.<-> (@extschema:vector@.halfvec, @extschema:vector@.halfvec)
					FOR ORDER BY pg_catalog.float_ops;

		CREATE OPERATOR CLASS halfvec_ip_ops
			FOR TYPE @extschema:vector@.halfvec USING lionvec AS
				OPERATOR 2 @extschema:vector@.<#> (@extschema:vector@.halfvec, @extschema:vector@.halfvec)
					FOR ORDER BY pg_catalog.float_ops;

		CREATE OPERATOR CLASS halfvec_cosine_ops
			FOR TYPE @extschema:vector@.halfvec USING lionvec AS
				OPERATOR 3 @extschema:vector@.<=> (@extschema:vector@.halfvec, @extschema:vector@.halfvec)
					FOR ORDER BY pg_catalog.float_ops;
	END IF;
END
$$;
