-- pg_lion_citext: the catch-up from any '0.1' (DESIGN.md §23, "Extension
-- versions").  Version 0.1 was never released, and its script gained support
-- function 4 and strategies 6 to 10 under that one name; this adds whichever
-- of them an installed citext_ops lacks, to its family, beside the indexes
-- that use it.
\echo Use "ALTER EXTENSION pg_lion_citext UPDATE" to load this file. \quit

DO $$
DECLARE
	fam		oid := (SELECT c.opcfamily FROM pg_catalog.pg_opclass c
					JOIN pg_catalog.pg_am a ON a.oid = c.opcmethod
					WHERE a.amname = 'lion' AND c.opcname = 'citext_ops'
					  AND c.opcnamespace = pg_catalog.to_regnamespace(pg_catalog.quote_ident(pg_catalog.current_schema())));
	cs		text := (SELECT pg_catalog.quote_ident(n.nspname)
					 FROM pg_catalog.pg_extension x JOIN pg_catalog.pg_namespace n ON n.oid = x.extnamespace
					 WHERE x.extname = 'citext');
	ct		oid := '@extschema:citext@.citext'::pg_catalog.regtype;
	m		record;
BEGIN
	FOR m IN
		SELECT * FROM (VALUES (6, '<'), (7, '<='), (8, '>='), (9, '>'), (10, '<>')) v(strategy, op)
		WHERE NOT EXISTS (SELECT 1 FROM pg_catalog.pg_amop WHERE amopfamily = fam AND amopstrategy = v.strategy
						  AND amoplefttype = ct AND amoprighttype = ct)
	LOOP
		EXECUTE pg_catalog.format('ALTER OPERATOR FAMILY citext_ops USING lion ADD OPERATOR %s %s.%s (%s.citext, %s.citext)',
								  m.strategy, cs, m.op, cs, cs);
	END LOOP;
	IF NOT EXISTS (SELECT 1 FROM pg_catalog.pg_amproc WHERE amprocfamily = fam AND amprocnum = 4
				   AND amproclefttype = ct AND amprocrighttype = ct) THEN
		ALTER OPERATOR FAMILY citext_ops USING lion ADD
			FUNCTION 4 (@extschema:citext@.citext, @extschema:citext@.citext)
				@extschema:citext@.citext_cmp(@extschema:citext@.citext, @extschema:citext@.citext);
	END IF;
END
$$;
