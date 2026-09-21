/*-------------------------------------------------------------------------
 *
 * rbi_am.c
 *		Access method handler, reloptions, opclass validation, cost estimate
 *		and ambuildempty for the roaring index.  See DESIGN.md section 6.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/amvalidate.h"
#include "access/generic_xlog.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/xloginsert.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_type.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/indexfsm.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"

#include "rbi.h"
#include "rbi_count.h"

PG_MODULE_MAGIC_EXT(
					.name = "roaring_index",
					.version = PG_VERSION
);

PG_FUNCTION_INFO_V1(roaring_handler);

/*
 * Strategies and support procedure numbers live in rbi.h, because build,
 * insert, scan and count all need them.  The multi-key strategies of
 * DESIGN.md §17 are 2 .. 5; a scalar opclass has only strategy 1.
 */
#define RBI_MULTI_STRATEGY_MASK \
	((1 << RBI_STRAT_CONTAINS) | (1 << RBI_STRAT_OVERLAP) | \
	 (1 << RBI_STRAT_CONTAINED) | (1 << RBI_STRAT_MATCH))

/* Kind of relation options for roaring indexes */
static relopt_kind rbi_relopt_kind;

static const relopt_parse_elt rbi_relopt_tab[] = {
	{"buckets", RELOPT_TYPE_INT, offsetof(RBIOptions, buckets)},
	{"inline_limit", RELOPT_TYPE_INT, offsetof(RBIOptions, inline_limit)},
	{"max_entries", RELOPT_TYPE_INT, offsetof(RBIOptions, max_entries)}
};

/*
 * Module initialisation: register the reloptions of the roaring AM, the
 * count-pushdown GUC and the planner hook that plants the RoaringCount
 * CustomScan (DESIGN.md section 10, implemented in rbi_customscan.c).
 *
 * This runs the first time the library is loaded into a backend, which for
 * any query over a roaring index happens in get_relation_info() when the
 * planner opens the index and fetches its handler - well before
 * create_upper_paths_hook is consulted for that same query.
 */
void
_PG_init(void)
{
	rbi_relopt_kind = add_reloption_kind();

	add_int_reloption(rbi_relopt_kind, "buckets",
					  "Number of hash buckets (0 selects it from the data)",
					  0, 0, RBI_MAX_BUCKETS,
					  AccessExclusiveLock);
	add_int_reloption(rbi_relopt_kind, "inline_limit",
					  "Maximum size in bytes of a posting set kept inside its entry tuple",
					  RBI_DEFAULT_INLINE_LIMIT, RBI_MIN_INLINE_LIMIT,
					  RBI_MAX_INLINE_LIMIT,
					  AccessExclusiveLock);
	add_int_reloption(rbi_relopt_kind, "max_entries",
					  "Distinct keys above which the index warns once per backend (0 disables)",
					  RBI_DEFAULT_MAX_ENTRIES, 0, INT_MAX,
					  ShareUpdateExclusiveLock);

	DefineCustomBoolVariable("roaring_index.enable_count_pushdown",
							 "Answer count(*) over roaring indexes from the index and the visibility map.",
							 NULL,
							 &rbi_enable_count_pushdown,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	MarkGUCPrefixReserved("roaring_index");

	rbi_prev_create_upper_paths_hook = create_upper_paths_hook;
	create_upper_paths_hook = rbi_create_upper_paths;
}

/*
 * Handler function: return the IndexAmRoutine of the roaring AM.
 */
Datum
roaring_handler(PG_FUNCTION_ARGS)
{
	static const IndexAmRoutine amroutine = {
		.type = T_IndexAmRoutine,
		.amstrategies = RBI_NSTRATEGIES,
		.amsupport = RBI_NPROC,
		.amoptsprocnum = 0,
		.amcanorder = false,
		.amcanorderbyop = false,
		.amcanhash = false,

		/*
		 * Every operator of a roaring opfamily agrees on one equivalence
		 * relation: a scalar family holds nothing but equality operators
		 * (cross-type ones included), and a multi-key family holds nothing
		 * but containment/match operators and no equality at all, so
		 * equality_ops_are_compatible() is never asked about two of those.
		 */
		.amconsistentequality = true,
		.amconsistentordering = false,
		.amcanbackward = false,
		.amcanunique = false,
		.amcanmulticol = false,
		.amoptionalkey = false,
		.amsearcharray = true,
		.amsearchnulls = true,
		/* multi-key opclasses store the KEY type, not the column type (§17) */
		.amstorage = true,
		.amclusterable = false,
		.ampredlocks = false,
		.amcanparallel = false,
		.amcanbuildparallel = false,
		.amcaninclude = false,
		.amusemaintenanceworkmem = true,
		.amsummarizing = false,
		.amparallelvacuumoptions = VACUUM_OPTION_NO_PARALLEL,
		.amkeytype = InvalidOid,

		.ambuild = rbibuild,
		.ambuildempty = rbibuildempty,
		.aminsert = rbiinsert,
		.aminsertcleanup = NULL,
		.ambulkdelete = rbibulkdelete,
		.amvacuumcleanup = rbivacuumcleanup,
		.amcanreturn = NULL,
		.amcostestimate = rbicostestimate,
		.amgettreeheight = NULL,
		.amoptions = rbioptions,
		.amproperty = NULL,
		.ambuildphasename = NULL,
		.amvalidate = rbivalidate,
		.amadjustmembers = NULL,
		.ambeginscan = rbibeginscan,
		.amrescan = rbirescan,
		.amgettuple = NULL,
		.amgetbitmap = rbigetbitmap,
		.amendscan = rbiendscan,
		.ammarkpos = NULL,
		.amrestrpos = NULL,
		.amestimateparallelscan = NULL,
		.aminitparallelscan = NULL,
		.amparallelrescan = NULL,
		.amtranslatestrategy = NULL,
		.amtranslatecmptype = NULL,
	};

	PG_RETURN_POINTER(&amroutine);
}

/*
 * Parse reloptions.
 */
bytea *
rbioptions(Datum reloptions, bool validate)
{
	return (bytea *) build_reloptions(reloptions, validate,
									  rbi_relopt_kind,
									  sizeof(RBIOptions),
									  rbi_relopt_tab,
									  lengthof(rbi_relopt_tab));
}

/*
 * Cost estimate: exactly the generic estimate, but a roaring index has no
 * correlation with the heap order (contrib/bloom does the same).
 */
void
rbicostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
				Cost *indexStartupCost, Cost *indexTotalCost,
				Selectivity *indexSelectivity, double *indexCorrelation,
				double *indexPages)
{
	GenericCosts costs = {0};

	genericcostestimate(root, path, loop_count, &costs);

	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = 0.0;
	*indexPages = costs.numIndexPages;
}

/*
 * Validator for a roaring opclass.  Modelled on hashvalidate()/ginvalidate().
 *
 * There are two shapes of roaring opclass and they are validated differently
 * (DESIGN.md §6 and §17):
 *
 *	scalar		support proc 1 is a hash function returning int4 and strategy
 *				1 is equality, exactly as a hash opclass.  Its support
 *				function's argument type only has to be binary-coercible from
 *				the opclass input type, so that e.g. a varchar column can use
 *				text_ops, and every type an operator mentions must have a hash
 *				function in the family, which is what makes cross-type
 *				equality usable.
 *
 *	multi-key	support procs 2 and 3 are GIN's extractValue and extractQuery
 *				and the strategies are 2 .. 5.  The keys are of the STORAGE
 *				type, so proc 1 - when there is one - hashes THAT and not the
 *				opclass input type; a polymorphic STORAGE type (array_ops
 *				stores anyelement) cannot name one function at all and the key
 *				type's default hash opclass is used instead.
 */
bool
rbivalidate(Oid opclassoid)
{
	bool		result = true;
	HeapTuple	classtup;
	Form_pg_opclass classform;
	Oid			opfamilyoid;
	Oid			opcintype;
	Oid			opckeytype;
	char	   *opclassname;
	char	   *opfamilyname;
	CatCList   *proclist,
			   *oprlist;
	List	   *grouplist;
	OpFamilyOpFuncGroup *opclassgroup;
	List	   *hashabletypes = NIL;
	bool		multikey = false;
	bool		haveop = false;
	int			i;
	ListCell   *lc;

	classtup = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclassoid));
	if (!HeapTupleIsValid(classtup))
		elog(ERROR, "cache lookup failed for operator class %u", opclassoid);
	classform = (Form_pg_opclass) GETSTRUCT(classtup);

	opfamilyoid = classform->opcfamily;
	opcintype = classform->opcintype;
	opckeytype = classform->opckeytype;
	if (!OidIsValid(opckeytype))
		opckeytype = opcintype;
	opclassname = NameStr(classform->opcname);

	opfamilyname = get_opfamily_name(opfamilyoid, false);

	oprlist = SearchSysCacheList1(AMOPSTRATEGY, ObjectIdGetDatum(opfamilyoid));
	proclist = SearchSysCacheList1(AMPROCNUM, ObjectIdGetDatum(opfamilyoid));

	/*
	 * Which shape is this?  An extractValue procedure for the opclass's own
	 * type is what makes an opclass multi-key, and it is the same test
	 * rbi_fill_state() makes at run time (through index_getprocid()).
	 */
	for (i = 0; i < proclist->n_members; i++)
	{
		Form_pg_amproc procform =
			(Form_pg_amproc) GETSTRUCT(&proclist->members[i]->tuple);

		if (procform->amprocnum == RBI_EXTRACTVALUE_PROC &&
			procform->amproclefttype == opcintype &&
			procform->amprocrighttype == opcintype)
			multikey = true;
	}

	/* Check individual support functions */
	for (i = 0; i < proclist->n_members; i++)
	{
		HeapTuple	proctup = &proclist->members[i]->tuple;
		Form_pg_amproc procform = (Form_pg_amproc) GETSTRUCT(proctup);
		bool		ok;

		if (procform->amproclefttype != procform->amprocrighttype)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains support function %s with different left and right input types",
							opfamilyname, "roaring",
							format_procedure(procform->amproc))));
			result = false;
		}

		switch (procform->amprocnum)
		{
			case RBI_HASH_PROC:
				if (multikey)
					ok = check_amproc_signature(procform->amproc, INT4OID,
												false, 1, 1, opckeytype);
				else
					ok = check_amproc_signature(procform->amproc, INT4OID,
												false, 1, 1,
												procform->amproclefttype);
				break;
			case RBI_EXTRACTVALUE_PROC:
				/* GIN's extractValue; some opclasses omit nullFlags */
				ok = check_amproc_signature(procform->amproc, INTERNALOID,
											false, 2, 3,
											procform->amproclefttype,
											INTERNALOID, INTERNALOID);
				break;
			case RBI_EXTRACTQUERY_PROC:
				/* GIN's extractQuery; some omit nullFlags and searchMode */
				ok = check_amproc_signature(procform->amproc, INTERNALOID,
											false, 5, 7,
											procform->amproclefttype,
											INTERNALOID, INT2OID, INTERNALOID,
											INTERNALOID, INTERNALOID,
											INTERNALOID);
				break;
			default:
				ereport(INFO,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("operator family \"%s\" of access method %s contains function %s with invalid support number %d",
								opfamilyname, "roaring",
								format_procedure(procform->amproc),
								procform->amprocnum)));
				result = false;
				continue;		/* don't want additional message */
		}

		/*
		 * The two extraction procedures only make sense together: an opclass
		 * with extractQuery but no extractValue would search for keys it
		 * never stored.  Only the opclass's own type pair can be judged here,
		 * for the same reason ginvalidate() gives.
		 */
		if (procform->amprocnum != RBI_HASH_PROC && !multikey &&
			procform->amproclefttype == opcintype)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator class \"%s\" of access method %s has support function %s but no support function %d",
							opclassname, "roaring",
							format_procedure(procform->amproc),
							RBI_EXTRACTVALUE_PROC)));
			result = false;
		}

		if (!ok)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains function %s with wrong signature for support number %d",
							opfamilyname, "roaring",
							format_procedure(procform->amproc),
							procform->amprocnum)));
			result = false;
		}
		else if (procform->amprocnum == RBI_HASH_PROC && !multikey)
			hashabletypes = list_append_unique_oid(hashabletypes,
												   procform->amproclefttype);
	}

	/* Check individual operators */
	for (i = 0; i < oprlist->n_members; i++)
	{
		HeapTuple	oprtup = &oprlist->members[i]->tuple;
		Form_pg_amop oprform = (Form_pg_amop) GETSTRUCT(oprtup);
		bool		stratok;

		haveop = true;

		/*
		 * A scalar family answers equality and nothing else; a multi-key one
		 * answers the containment/match strategies and never equality (the
		 * keys of one row are not the row's value, so `=` could not be
		 * answered from them).
		 */
		stratok = multikey ?
			(oprform->amopstrategy >= 1 &&
			 oprform->amopstrategy <= RBI_NSTRATEGIES &&
			 (RBI_MULTI_STRATEGY_MASK & (1 << oprform->amopstrategy)) != 0) :
			(oprform->amopstrategy == RBI_STRAT_EQUAL);

		if (!stratok)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains operator %s with invalid strategy number %d",
							opfamilyname, "roaring",
							format_operator(oprform->amopopr),
							oprform->amopstrategy)));
			result = false;
		}

		if (oprform->amoppurpose != AMOP_SEARCH ||
			OidIsValid(oprform->amopsortfamily))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains invalid ORDER BY specification for operator %s",
							opfamilyname, "roaring",
							format_operator(oprform->amopopr))));
			result = false;
		}

		if (!check_amop_signature(oprform->amopopr, BOOLOID,
								  oprform->amoplefttype,
								  oprform->amoprighttype))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains operator %s with wrong signature",
							opfamilyname, "roaring",
							format_operator(oprform->amopopr))));
			result = false;
		}

		/*
		 * Every type used by an operator must be hashable by this family:
		 * that is what lets a cross-type equality use the index, because the
		 * search value is hashed with its OWN type's function.  A multi-key
		 * family hashes keys and not the types its operators mention, so the
		 * rule does not apply to it.
		 */
		if (!multikey &&
			(!list_member_oid(hashabletypes, oprform->amoplefttype) ||
			 !list_member_oid(hashabletypes, oprform->amoprighttype)))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s lacks support function for operator %s",
							opfamilyname, "roaring",
							format_operator(oprform->amopopr))));
			result = false;
		}
	}

	/* Check for inconsistent groups of operators/functions */
	grouplist = identify_opfamily_groups(oprlist, proclist);
	opclassgroup = NULL;
	foreach(lc, grouplist)
	{
		OpFamilyOpFuncGroup *thisgroup = (OpFamilyOpFuncGroup *) lfirst(lc);

		if (thisgroup->lefttype == opcintype &&
			thisgroup->righttype == opcintype)
			opclassgroup = thisgroup;

		/*
		 * A scalar family must offer equality for every type pair it knows
		 * about.  A multi-key family cannot be checked that way: its
		 * operators and its support functions do not even share a type pair
		 * (tsvector_ops has @@(tsvector,tsquery) and procs on
		 * (tsvector,tsvector)), so the per-operator and per-class checks
		 * above and below are all there is.
		 */
		if (!multikey && thisgroup->operatorset != (1 << RBI_STRAT_EQUAL))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s is missing operator(s) for types %s and %s",
							opfamilyname, "roaring",
							format_type_be(thisgroup->lefttype),
							format_type_be(thisgroup->righttype))));
			result = false;
		}
	}

	/* The opclass itself must be complete */
	if (multikey)
	{
		uint64		want = (((uint64) 1) << RBI_EXTRACTVALUE_PROC) |
			(((uint64) 1) << RBI_EXTRACTQUERY_PROC);

		/*
		 * A hash function for the key type is required unless the key type is
		 * polymorphic, in which case there is no single function to name and
		 * the key type's default hash opclass answers instead
		 * (rbi_fill_state()).
		 */
		if (!IsPolymorphicType(opckeytype))
			want |= ((uint64) 1) << RBI_HASH_PROC;

		if (!opclassgroup || (opclassgroup->functionset & want) != want)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator class \"%s\" of access method %s is missing support function(s)",
							opclassname, "roaring")));
			result = false;
		}

		if (!haveop)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator class \"%s\" of access method %s is missing operator(s)",
							opclassname, "roaring")));
			result = false;
		}
	}
	else
	{
		if (!opclassgroup ||
			(opclassgroup->functionset & (((uint64) 1) << RBI_HASH_PROC)) == 0)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator class \"%s\" of access method %s is missing support function %d",
							opclassname, "roaring", RBI_HASH_PROC)));
			result = false;
		}
		if (!opclassgroup)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator class \"%s\" of access method %s is missing operator(s)",
							opclassname, "roaring")));
			result = false;
		}
	}

	ReleaseCatCacheList(proclist);
	ReleaseCatCacheList(oprlist);
	ReleaseSysCache(classtup);

	return result;
}

/*
 * Build an empty index in the init fork (used for unlogged relations).
 */
void
rbibuildempty(Relation index)
{
	RBIOptions *opts = (RBIOptions *) index->rd_options;
	uint32		nbuckets;
	uint32		inline_limit;
	uint32		b;
	Buffer		buf;

	nbuckets = rbi_clamp_buckets((opts && opts->buckets > 0) ?
								 opts->buckets : RBI_DEFAULT_BUCKETS);
	inline_limit = opts ? (uint32) opts->inline_limit : RBI_DEFAULT_INLINE_LIMIT;

	/* Meta page */
	buf = ExtendBufferedRel(BMR_REL(index), INIT_FORKNUM, NULL,
							EB_LOCK_FIRST | EB_SKIP_EXTENSION_LOCK);
	Assert(BufferGetBlockNumber(buf) == RBI_METAPAGE_BLKNO);
	START_CRIT_SECTION();
	rbi_init_metapage(BufferGetPage(buf), nbuckets, inline_limit);
	MarkBufferDirty(buf);
	log_newpage_buffer(buf, true);
	END_CRIT_SECTION();
	UnlockReleaseBuffer(buf);

	/* Bucket head pages */
	for (b = 0; b < nbuckets; b++)
	{
		buf = ExtendBufferedRel(BMR_REL(index), INIT_FORKNUM, NULL,
								EB_LOCK_FIRST | EB_SKIP_EXTENSION_LOCK);
		Assert(BufferGetBlockNumber(buf) == RBI_BUCKET_BLKNO(b));
		START_CRIT_SECTION();
		rbi_init_page(BufferGetPage(buf), RBI_PAGE_BUCKET);
		MarkBufferDirty(buf);
		log_newpage_buffer(buf, true);
		END_CRIT_SECTION();
		UnlockReleaseBuffer(buf);

		CHECK_FOR_INTERRUPTS();
	}
}
