-- One line per catalog fact about the objects of the extension named by
-- :'ext', sorted, so that two databases can be diffed.
\set QUIET on
\pset format unaligned
\pset tuples_only on
WITH ext AS (SELECT oid FROM pg_extension WHERE extname = :'ext'),
members AS (
	SELECT d.classid, d.objid, d.objsubid
	FROM pg_depend d, ext
	WHERE d.refclassid = 'pg_extension'::regclass AND d.refobjid = ext.oid AND d.deptype = 'e'
)
SELECT line FROM (
	-- every member, by identity
	SELECT 'member ' || pg_describe_object(classid, objid, objsubid) AS line FROM members
	UNION ALL
	-- comments on members
	SELECT 'comment ' || pg_describe_object(m.classid, m.objid, m.objsubid) || ': ' || d.description
	FROM members m JOIN pg_description d ON d.classoid = m.classid AND d.objoid = m.objid AND d.objsubid = m.objsubid
	UNION ALL
	-- the privileges pg_dump takes as the extension's own
	SELECT format('initprivs %s: %s', pg_describe_object(m.classid, m.objid, m.objsubid), i.initprivs)
	FROM members m JOIN pg_init_privs i ON i.classoid = m.classid AND i.objoid = m.objid AND i.objsubid = m.objsubid
	UNION ALL
	-- functions, aggregates and procedures
	SELECT format('function %s(%s) returns %s kind=%s lang=%s src=%s bin=%s vol=%s par=%s strict=%s secdef=%s leak=%s cost=%s rows=%s support=%s config=%s acl=%s',
		p.proname, pg_get_function_arguments(p.oid), pg_get_function_result(p.oid),
		p.prokind, l.lanname,
		CASE WHEN l.lanname IN ('c', 'internal') THEN p.prosrc ELSE md5(p.prosrc) END,
		p.probin,
		p.provolatile, p.proparallel, p.proisstrict, p.prosecdef, p.proleakproof,
		p.procost, p.prorows, p.prosupport, p.proconfig, p.proacl)
	FROM members m JOIN pg_proc p ON m.classid = 'pg_proc'::regclass AND p.oid = m.objid
	JOIN pg_language l ON l.oid = p.prolang
	UNION ALL
	SELECT format('aggregate %s: kind=%s sfunc=%s stype=%s final=%s combine=%s initval=%s msfunc=%s',
		a.aggfnoid::regprocedure, a.aggkind, a.aggtransfn, format_type(a.aggtranstype, NULL),
		a.aggfinalfn, a.aggcombinefn, a.agginitval, a.aggmtransfn)
	FROM members m JOIN pg_aggregate a ON m.classid = 'pg_proc'::regclass AND a.aggfnoid = m.objid
	UNION ALL
	SELECT format('am %s type=%s handler=%s', a.amname, a.amtype, a.amhandler)
	FROM members m JOIN pg_am a ON m.classid = 'pg_am'::regclass AND a.oid = m.objid
	UNION ALL
	SELECT format('opclass %s for %s default=%s family=%s keytype=%s',
		pg_describe_object('pg_opclass'::regclass, c.oid, 0), format_type(c.opcintype, NULL), c.opcdefault,
		pg_describe_object('pg_opfamily'::regclass, c.opcfamily, 0), format_type(c.opckeytype, NULL))
	FROM members m JOIN pg_opclass c ON m.classid = 'pg_opclass'::regclass AND c.oid = m.objid
	UNION ALL
	-- every operator and support function of the extension's families.  Not
	-- whether a class holds it or it is loose in the family: an upgrade adds
	-- members with ALTER OPERATOR FAMILY, loose, where an install creates
	-- them with their class, which only changes what DROP OPERATOR CLASS
	-- takes with it.
	SELECT format('amop %s: %s purpose=%s sortfamily=%s',
		pg_describe_object('pg_opfamily'::regclass, o.amopfamily, 0),
		pg_describe_object('pg_amop'::regclass, o.oid, 0), o.amoppurpose, o.amopsortfamily)
	FROM members m JOIN pg_amop o ON m.classid = 'pg_opfamily'::regclass AND o.amopfamily = m.objid
	UNION ALL
	SELECT format('amproc %s: %s',
		pg_describe_object('pg_opfamily'::regclass, p.amprocfamily, 0),
		pg_describe_object('pg_amproc'::regclass, p.oid, 0))
	FROM members m JOIN pg_amproc p ON m.classid = 'pg_opfamily'::regclass AND p.amprocfamily = m.objid
	UNION ALL
	SELECT format('operator %s returns %s code=%s com=%s neg=%s rest=%s join=%s hash=%s merge=%s',
		o.oid::regoperator, format_type(o.oprresult, NULL), o.oprcode, o.oprcom::regoperator,
		o.oprnegate::regoperator, o.oprrest, o.oprjoin, o.oprcanhash, o.oprcanmerge)
	FROM members m JOIN pg_operator o ON m.classid = 'pg_operator'::regclass AND o.oid = m.objid
	UNION ALL
	SELECT format('type %s', format_type(t.oid, NULL))
	FROM members m JOIN pg_type t ON m.classid = 'pg_type'::regclass AND t.oid = m.objid
	UNION ALL
	SELECT format('cast %s', pg_describe_object(m.classid, m.objid, 0))
	FROM members m WHERE m.classid = 'pg_cast'::regclass
	UNION ALL
	SELECT format('relation %s kind=%s column %s %s', c.oid::regclass, c.relkind, a.attname, format_type(a.atttypid, a.atttypmod))
	FROM members m JOIN pg_class c ON m.classid = 'pg_class'::regclass AND c.oid = m.objid
	JOIN pg_attribute a ON a.attrelid = c.oid AND a.attnum > 0 AND NOT a.attisdropped
	UNION ALL
	SELECT format('extension %s version=%s relocatable=%s config=%s', e.extname, e.extversion, e.extrelocatable, e.extconfig)
	FROM pg_extension e, ext WHERE e.oid = ext.oid
) facts
ORDER BY line COLLATE "C";
