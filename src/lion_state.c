/*-------------------------------------------------------------------------
 *
 * lion_state.c
 *		The per-relation state of a lion index - its columns, their opclass
 *		procedures and order - with the checks that it is usable, and the keys
 *		of its entries.
 *
 * Part of the page layer, which lion.h declares.  See DESIGN.md sections 4
 * and 5.
 *
 * Every page modification in the page layer goes through the WAL shim of
 * DESIGN.md §25 (lion_wal_begin/register_buffer/op/finish), which writes
 * either a GenericXLog record or one of the extension's own: the buffer is
 * registered before it is touched and lion_wal_finish() runs before any
 * lock is dropped.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/generic_xlog.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/table.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "parser/parse_coerce.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/freespace.h"
#include "storage/indexfsm.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/typcache.h"
#include "varatt.h"

#include "lion.h"

/*
 * May this transaction answer a query from this index at all?
 *
 * Every caller that opens a lion index by name - the SQL count functions,
 * the verifier - has to make the decision the planner makes for a query that
 * mentions the table, because the index it was handed was not approved by
 * anyone.  There are three ways an index can exist and still be unusable:
 *
 *	- indisvalid is false: the index is not complete (a failed CREATE INDEX
 *	  CONCURRENTLY, a failed REINDEX CONCURRENTLY, or an index still being
 *	  built).  get_relation_info() ignores such an index entirely.
 *	- indisready is false: it is not even receiving inserts yet, so it is
 *	  missing rows by construction.
 *	- indcheckxmin is true and the index tuple's xmin is not yet old enough:
 *	  the index build found a broken HOT chain and indexed only the LATEST
 *	  version of it, so a snapshot that can still see an older version of
 *	  that row cannot use the index (src/backend/access/heap/README.HOT).
 *	  This is the case that produces a WRONG ANSWER rather than a missing
 *	  optimisation: the row is not in the posting set the count selects, and
 *	  no amount of heap rechecking puts back a TID that is not there.
 *
 * The indcheckxmin test is the one get_relation_info() applies
 * (src/backend/optimizer/util/plancat.c): compare against TransactionXmin,
 * the xmin of the OLDEST snapshot this transaction has taken, which is the
 * conservative bound for every snapshot it can still use.  A snapshot handed
 * in explicitly is honoured too when it is somehow older than that.
 *
 * Returns true when the index may be used.  Otherwise *why (which may not be
 * NULL) is set to a short phrase that reads after "because".
 */
bool
lion_index_usable(Relation index, Snapshot snapshot, const char **why)
{
	Form_pg_index idx = index->rd_index;
	TransactionId limit = TransactionXmin;

	Assert(why != NULL);
	*why = NULL;

	if (!idx->indisvalid)
	{
		*why = "the index is not valid";
		return false;
	}
	if (!idx->indisready)
	{
		*why = "the index is not ready for queries";
		return false;
	}

	if (!idx->indcheckxmin)
		return true;

	/*
	 * TransactionXmin is set from the transaction's first snapshot and never
	 * moves, so it already covers the caller's snapshot; be conservative
	 * anyway if the caller brought an older one.
	 */
	if (snapshot != NULL && TransactionIdIsValid(snapshot->xmin) &&
		TransactionIdPrecedes(snapshot->xmin, limit))
		limit = snapshot->xmin;

	if (!TransactionIdPrecedes(HeapTupleHeaderGetXmin(index->rd_indextuple->t_data),
							   limit))
	{
		*why = "it was built from a broken HOT chain (indcheckxmin) and cannot be used by this transaction's snapshot";
		return false;
	}

	return true;
}

/*
 * Refuse to read index under snapshot while old_snapshot_threshold is set
 * (PostgreSQL 16 only; DESIGN.md §9, "old_snapshot_threshold").
 *
 * With the threshold set, VACUUM prunes with a horizon that may have passed
 * an old snapshot's xmin: it removes TIDs that snapshot still sees and marks
 * their heap pages all-visible.  Core's access methods notice by comparing
 * the LSN of every page they read with the snapshot's (TestForOldSnapshot())
 * and raise "snapshot too old"; lion does not, so a count, a bitmap scan or
 * an index-only scan would give a silently different answer - a row missing
 * whose TID is gone, a dead row counted from the visibility map.  Rather than
 * test every page of every path, lion is not read at all under an MVCC
 * snapshot on such a server: the planner never picks it (lioncostestimate()
 * and the CustomScan hooks), and whatever reaches it anyway - a direct SQL
 * count, a plan with every other path disabled - stops here.  Other snapshots
 * are not affected by early pruning, and neither are inserts or VACUUM.
 */
void
lion_check_old_snapshot(Relation index, Snapshot snapshot)
{
	if (!lion_old_snapshot_threshold_active())
		return;
	if (snapshot != NULL && !IsMVCCSnapshot(snapshot))
		return;

	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("lion index \"%s\" cannot be read while old_snapshot_threshold is set",
					RelationGetRelationName(index)),
			 errdetail("A lion index does not detect \"snapshot too old\", so it could answer rows an old snapshot must not see, or miss rows it must."),
			 errhint("Set old_snapshot_threshold to -1 and restart the server.")));
}

/*
 * Does ltopr sort with cmpfunc?
 *
 * ambuild's tuplesort is driven by an OPERATOR and the directory by a
 * FUNCTION, and DESIGN.md §21 requires them to be the same order exactly.
 * PrepareSortSupportFromOrderingOp() resolves the operator to a btree
 * opfamily and takes that family's comparison support function, so the
 * question "will the sort use cmpfunc?" is answered by asking the same
 * catalogue the same way.
 */
static bool
lion_ltopr_sorts_with(Oid ltopr, Oid cmpfunc)
{
	Oid			opfamily;
	Oid			opcintype;

	if (!OidIsValid(ltopr) || !OidIsValid(cmpfunc))
		return false;
	if (!lion_ordering_op_is_lt(ltopr, &opfamily, &opcintype))
		return false;

	return get_opfamily_proc(opfamily, opcintype, opcintype,
							 BTORDER_PROC) == cmpfunc;
}

/*
 * A `<` operator that sorts with cmpfunc, for a comparison function that is
 * NOT the key type's default one: find the btree operator family that uses it
 * as its comparison support function and take that family's `<`.
 *
 * A comparison that belongs to no btree family at all cannot be handed to a
 * tuplesort, and an ordering the BUILD cannot reproduce is no ordering
 * (DESIGN.md §21): the caller then leaves the index unordered, which is still
 * a complete directory order - (kind, hash, bytes) - just not the opclass's.
 *
 * pg_amproc is scanned rather than looked up because the family is what is
 * being searched for.  It happens once per relcache build of an index whose
 * opclass names a comparison of its own, which no built-in opclass does.
 */
static Oid
lion_find_sort_operator(Oid cmpfunc, Oid typid)
{
	Relation	rel;
	ScanKeyData skey;
	SysScanDesc scan;
	HeapTuple	tup;
	Oid			result = InvalidOid;

	rel = table_open(AccessMethodProcedureRelationId, AccessShareLock);
	ScanKeyInit(&skey, Anum_pg_amproc_amproc, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(cmpfunc));
	scan = systable_beginscan(rel, InvalidOid, false, NULL, 1, &skey);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_amproc amp = (Form_pg_amproc) GETSTRUCT(tup);
		Oid			ltopr;

		if (amp->amprocnum != BTORDER_PROC ||
			amp->amproclefttype != amp->amprocrighttype)
			continue;
		if (amp->amproclefttype != typid &&
			!IsBinaryCoercible(typid, amp->amproclefttype))
			continue;

		ltopr = get_opfamily_member(amp->amprocfamily, amp->amproclefttype,
									amp->amprocrighttype,
									BTLessStrategyNumber);
		if (lion_ltopr_sorts_with(ltopr, cmpfunc))
		{
			result = ltopr;
			break;
		}
	}

	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	return result;
}

/*
 * Does this function have a SQL-standard (`RETURN`) body?  See
 * lion_proc_ident() for why a comparison with one is not ordered by.
 */
static bool
lion_proc_has_sqlbody(Oid procoid)
{
	HeapTuple	tup;
	bool		isnull;

	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(procoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for function %u", procoid);
	(void) SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_prosqlbody, &isnull);
	ReleaseSysCache(tup);

	return !isnull;
}

/*
 * What rd_amcache points to: a HANDLE on the index's state, not the state.
 *
 * A relcache flush of an index entry - RelationReloadIndexInfo(), which any
 * catalog read may trigger under debug_discard_caches and ordinary
 * invalidation traffic can trigger at any lock acquisition - pfree()s
 * rd_amcache and nothing else of what it points into.  With the state itself
 * there, every caller that had taken a LionIndexState and read the catalog
 * before it was done with it - a scan resolving a cross-type probe, verify()
 * comparing text keys under a collation, the order check below - held a
 * pointer into freed memory: under debug_discard_caches the order check read
 * a garbage identity and refused every index, and verify() crashed.  With a
 * handle, the flush frees only the handle.  The state stays where it is, in
 * rd_indexcxt, so a pointer taken earlier stays valid - it is simply no
 * longer the entry's current state, and the next lion_get_index_state()
 * builds a new one, as it always did after a flush.
 *
 * WHY rd_indexcxt OUTLIVES THE FLUSH, on every supported release: a flush of
 * an index entry that is OPEN (rd_refcnt > 0) and has its index support
 * loaded (rd_indexcxt != NULL) is always the in-place reload - 16's and
 * 17's RelationClearRelation() take the index branch and return before the
 * destroy/full-rebuild code, and 18-20's RelationRebuildRelation() calls
 * RelationReloadIndexInfo() for exactly that case - and the reload frees
 * rd_amcache and keeps rd_indexcxt.  The context is deleted only when the
 * entry itself is destroyed (RelationDestroyRelation(), which asserts a
 * reference count of zero on every one of those releases).  So
 * the rule is: a state, or a column state, is valid for as long as the
 * caller holds the index open, and every caller in this extension does - a
 * scan through its IndexScanDesc, VACUUM and insert through the executor's
 * open relation, the count node until lion_close_relation(), the SQL
 * functions until they close it.  Nothing keeps one across an index_close().
 *
 * The cost: each flush of an open index leaves one state behind in
 * rd_indexcxt until the relcache entry is destroyed.  That was already true
 * of the column states and their FmgrInfos, which were never freed with the
 * rd_amcache they hung off; the handle adds the header's few dozen bytes.
 */
typedef struct LionAmCache
{
	LionIndexState *ix;
} LionAmCache;

/*
 * The WAL mode of every index this backend has read a meta page of, keyed by
 * its relfilenode (DESIGN.md §25).
 *
 * lion_wal_mode() is asked for when a record is begun, which is in the middle
 * of a page change - a directory split holds the meta page itself EXCLUSIVE -
 * and the mode used to come out of lion_get_index_state().  After a relcache
 * flush that means building the state again, which reads the meta page: a
 * second lock on a buffer this backend already holds, which a cassert build
 * traps on and a production build would wait on for ever (found under
 * debug_discard_caches, where every catalog read flushes; ordinary
 * invalidation traffic can do the same between an insert's start and its
 * split).  The mode never changes for a relfilenode - REINDEX and TRUNCATE
 * give the index a new one - so it is remembered here the first time a meta
 * page is read, and the write path never has to read one.
 */
typedef struct LionWalModeEnt
{
	RelFileLocator locator;
	uint32		wal_mode;
} LionWalModeEnt;

static HTAB *lion_wal_modes = NULL;

static void
lion_remember_wal_mode(Relation index, uint32 wal_mode)
{
	LionWalModeEnt *ent;
	bool		found;

	if (lion_wal_modes == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(RelFileLocator);
		ctl.entrysize = sizeof(LionWalModeEnt);
		ctl.hcxt = TopMemoryContext;
		lion_wal_modes = hash_create("lion index WAL modes", 64, &ctl,
									 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	ent = (LionWalModeEnt *) hash_search(lion_wal_modes, &index->rd_locator,
										 HASH_ENTER, &found);
	ent->wal_mode = wal_mode;
}

/*
 * The meta page's wal_mode of index, without reading anything in the common
 * case: remembered per relfilenode by whoever read its meta page first.
 */
uint32
lion_index_meta_wal_mode(Relation index)
{
	if (lion_wal_modes != NULL)
	{
		LionWalModeEnt *ent = (LionWalModeEnt *)
			hash_search(lion_wal_modes, &index->rd_locator, HASH_FIND, NULL);

		if (ent != NULL)
			return ent->wal_mode;
	}
	return lion_get_index_state(index)->meta.wal_mode;
}

/*
 * How many pg_proc invalidations this backend has seen (DESIGN.md §21).  A
 * cached index state is kept in the index's relcache entry, and replacing
 * the comparison it was built with invalidates pg_proc and NOT the index, so
 * lion_get_index_state() checks the recorded order's comparisons again
 * whenever this has moved since the state last passed the check.  The
 * callback only counts: it runs while invalidations are being processed,
 * where no catalog may be read.
 */
static uint64 lion_proc_generation = 1;
static bool lion_proc_callback_registered = false;

static void
lion_proc_inval_callback(Datum arg, LionSysCacheId cacheid, uint32 hashvalue)
{
	lion_proc_generation++;
}

/*
 * Fill in the state of ONE key column of index (DESIGN.md §24).  attno is
 * 1-based and names an index column, not a heap attribute.
 */
static void
lion_fill_column_state(Relation index, LionState *state, AttrNumber attno,
					   MemoryContext cxt)
{
	Form_pg_attribute att;
	int			i = attno - 1;
	Oid			eqopr;
	Oid			eqfunc;
	Oid			eqoprused = InvalidOid;		/* the equality the entries use */

	state->attno = (uint16) attno;

	/*
	 * The index's own tuple descriptor already carries the KEY type, opclass
	 * STORAGE and polymorphism resolved (see the comment on LionState.typid),
	 * so a multi-key class needs no extra type lookup here.
	 */
	att = TupleDescAttr(RelationGetDescr(index), i);
	state->typid = att->atttypid;
	get_typlenbyvalalign(state->typid, &state->typlen, &state->typbyval,
						 &state->typalign);
	state->collation = index->rd_indcollation[i];

	/*
	 * A key type that cares about collations must have one: hashtext() and
	 * the text equality operator both refuse to work without.  The index
	 * column of a tsvector_ops index is text while the tsvector it is
	 * extracted from is not collatable at all, so the index has no collation
	 * to offer; lexemes are byte strings, and C is the collation that hashes
	 * and compares them bytewise - which is exactly what GIN's
	 * gin_cmp_tslexeme() does.
	 */
	if (!OidIsValid(state->collation) && type_is_collatable(state->typid))
		state->collation = C_COLLATION_OID;

	/* Multi-key opclass?  Support proc 2 is what says so (DESIGN.md §17). */
	state->multikey =
		OidIsValid(index_getprocid(index, attno, LION_EXTRACTVALUE_PROC));

	if (state->multikey)
	{
		if (!OidIsValid(index_getprocid(index, attno, LION_EXTRACTQUERY_PROC)))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("operator class of column %d of index \"%s\" has support function %d but not %d",
							attno, RelationGetRelationName(index),
							LION_EXTRACTVALUE_PROC, LION_EXTRACTQUERY_PROC)));

		fmgr_info_copy(&state->extractvalue,
					   index_getprocinfo(index, attno, LION_EXTRACTVALUE_PROC),
					   cxt);
		fmgr_info_copy(&state->extractquery,
					   index_getprocinfo(index, attno, LION_EXTRACTQUERY_PROC),
					   cxt);
	}

	/*
	 * Hashing: the opclass's support function 1 when it has one.  A
	 * polymorphic multi-key class cannot name a single function for the key
	 * type (array_ops indexes anyelement), so its hash comes from the key
	 * type's default hash opclass, the way GIN resolves its comparison
	 * function in initGinState().
	 */
	if (OidIsValid(index_getprocid(index, attno, LION_HASH_PROC)))
		fmgr_info_copy(&state->hashproc,
					   index_getprocinfo(index, attno, LION_HASH_PROC), cxt);
	else
	{
		TypeCacheEntry *typentry;

		Assert(state->multikey);
		typentry = lookup_type_cache(state->typid, TYPECACHE_HASH_PROC_FINFO);
		if (!OidIsValid(typentry->hash_proc_finfo.fn_oid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("could not identify a hash function for type %s",
							format_type_be(state->typid)),
					 errdetail("Index \"%s\" extracts keys of that type.",
							   RelationGetRelationName(index))));
		fmgr_info_copy(&state->hashproc, &typentry->hash_proc_finfo, cxt);
	}

	/*
	 * Equality: strategy 1 of the opfamily for a scalar opclass.  A multi-key
	 * opclass's strategies are about the indexed VALUE (@>, &&, @@), not
	 * about two keys, so its keys are compared with the key type's default
	 * equality operator - which is the one its hash opclass agrees with.
	 */
	eqopr = state->multikey ? InvalidOid :
		get_opfamily_member(index->rd_opfamily[i],
							index->rd_opcintype[i],
							index->rd_opcintype[i],
							LION_STRAT_EQUAL);
	if (OidIsValid(eqopr))
	{
		eqfunc = get_opcode(eqopr);
		if (!OidIsValid(eqfunc))
			elog(ERROR, "could not find function for operator %u", eqopr);
		fmgr_info_cxt(eqfunc, &state->eqproc, cxt);
		eqoprused = eqopr;
	}
	else
	{
		TypeCacheEntry *typentry;

		if (!state->multikey)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("operator class of column %d of index \"%s\" has no equality operator",
							attno, RelationGetRelationName(index))));

		typentry = lookup_type_cache(state->typid, TYPECACHE_EQ_OPR_FINFO);
		if (!OidIsValid(typentry->eq_opr_finfo.fn_oid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("could not identify an equality operator for type %s",
							format_type_be(state->typid)),
					 errdetail("Index \"%s\" extracts keys of that type.",
							   RelationGetRelationName(index))));
		fmgr_info_copy(&state->eqproc, &typentry->eq_opr_finfo, cxt);
		eqoprused = typentry->eq_opr;
	}

	/*
	 * ORDERING (DESIGN.md §21).  Support proc 4 is the opclass's own btree
	 * comparison of the KEY type.  Three rules decide what the directory is
	 * ordered by, and all three are about the same thing: the order the
	 * BUILD lays entries out in and the order a SEARCH descends must be one
	 * and the same, and both must agree with the opclass EQUALITY, because
	 * the directory holds one entry per equality class.
	 *
	 *	1. An opclass WITH proc 4 is ordered by it.
	 *
	 *	2. An opclass WITHOUT proc 4 - a polymorphic multi-key class, or one
	 *	   that simply does not name one - may borrow the key type's default
	 *	   btree comparison ONLY when its own equality operator IS the key
	 *	   type's default btree equality.  Otherwise the borrowed comparison
	 *	   can be FINER than the opclass equality, which would put the two
	 *	   members of one equality class at two different positions and
	 *	   therefore in two entries: a case-insensitive text class stores
	 *	   'A' and 'a' in ONE entry, and bttextcmp separates them, so a
	 *	   lookup for the spelling that is not the stored one would descend
	 *	   past the entry and miss.  Such an opclass is UNORDERED: the
	 *	   directory order is (kind, hash, bytes), which ties exactly where
	 *	   the hash ties and lets the run scan apply the opclass equality.
	 *
	 *	3. Either way the ordering needs a `<` OPERATOR THAT SORTS WITH THE
	 *	   SAME FUNCTION, because ambuild's tuplesort is driven by an
	 *	   operator.  The key type's default `<` qualifies only when the
	 *	   comparison is the key type's default one; a comparison of the
	 *	   opclass's own is looked for in the btree family that uses it.  An
	 *	   ordering the build cannot reproduce is no ordering.
	 *
	 * An unordered index is not a broken one: (kind, hash, bytes) is a
	 * complete directory order, just not the type's, so lion_index_stats()
	 * reports ordered = false and the count pushdown claims no pathkeys.
	 */
	state->ordered = false;
	state->ltopr = InvalidOid;
	{
		Oid			cmpfunc = index_getprocid(index, attno, LION_CMP_PROC);
		TypeCacheEntry *typentry =
			lookup_type_cache(state->typid,
							  TYPECACHE_CMP_PROC_FINFO | TYPECACHE_LT_OPR |
							  TYPECACHE_BTREE_OPFAMILY);
		bool		haveproc = false;

		if (OidIsValid(cmpfunc))
		{
			fmgr_info_copy(&state->cmpproc,
						   index_getprocinfo(index, attno, LION_CMP_PROC), cxt);
			haveproc = true;
		}
		else if (OidIsValid(typentry->cmp_proc_finfo.fn_oid))
		{
			/* Rule 2: borrow only when the equalities are provably the same. */
			Oid			defaulteq =
				OidIsValid(typentry->btree_opf) ?
				get_opfamily_member(typentry->btree_opf,
									typentry->btree_opintype,
									typentry->btree_opintype,
									BTEqualStrategyNumber) : InvalidOid;

			if (OidIsValid(defaulteq) && eqoprused == defaulteq)
			{
				cmpfunc = typentry->cmp_proc_finfo.fn_oid;
				fmgr_info_copy(&state->cmpproc, &typentry->cmp_proc_finfo, cxt);
				haveproc = true;
			}
		}

		/*
		 * A comparison with a SQL-standard body keeps it parsed, in
		 * prosqlbody, with no prosrc: nothing the recorded order could
		 * recognise it by (lion_proc_ident() - the tree embeds Oids that
		 * pg_upgrade does not keep, and its deparse depends on the session's
		 * search_path).  So a build does not order by it: the column is laid
		 * out in hash order, which no later change of the comparison can
		 * make wrong, and lion_meta_record_order() says so.  Ranges on it
		 * then test every entry of the column (§28), and the count pushdown
		 * claims no pathkeys; both are correct.
		 */
		if (haveproc && lion_proc_has_sqlbody(cmpfunc))
		{
			state->sqlbodycmp = true;
			haveproc = false;
		}

		if (haveproc)
		{
			/* Rule 3: the operator has to sort with that very function. */
			if (lion_ltopr_sorts_with(typentry->lt_opr, cmpfunc))
				state->ltopr = typentry->lt_opr;
			else
				state->ltopr = lion_find_sort_operator(cmpfunc, state->typid);

			state->ordered = OidIsValid(state->ltopr);
		}
	}

	/*
	 * ... and all of that is only how a BUILD decides it.  The directory an
	 * existing index has is in the order its build chose, whatever the
	 * catalog would choose today (§21, "The order is the index's"): a btree
	 * opclass created since can make proc 4 sortable and so "ordered" (rule
	 * 3), dropping it can do the reverse, and reading a hash-ordered
	 * directory in value order - or the other way round - finds keys where
	 * they are not.  So once the meta page records the order, it decides.
	 * An ordered column needs only its comparison for that, never the sort
	 * operator, which only the build's tuplesort uses; whether the
	 * comparison is still the one the build used is checked for the whole
	 * index in lion_fill_index_state().
	 */
	if ((state->ix->meta.order_flags & LION_META_ORDER_RECORDED) != 0)
	{
		bool		stored = (state->ix->meta.ordered_cols &
							  (((uint32) 1) << i)) != 0;

		if (stored && !OidIsValid(state->cmpproc.fn_oid))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("index \"%s\" was built in the order of a comparison function that key column %d no longer has",
							RelationGetRelationName(index), attno),
					 errhint("REINDEX the index.")));
		state->ordered = stored;
	}
}

/*
 * What a comparison function DOES, for LionMetaPageData.order_ident: the
 * source it runs, not the name it is called by.  That is prosrc - the C
 * symbol of an internal or C-language function (`btint4cmp`, `citext_cmp`),
 * the body of a SQL or PL one given as a string - and probin, the library a
 * C function lives in ('$libdir/citext').
 *
 * Neither a name nor an Oid would do.  Oids do not survive pg_upgrade, which
 * keeps an index's files but recreates its user functions.  A name keyed by
 * its schema turned `ALTER EXTENSION citext SET SCHEMA` - citext is
 * relocatable, and so are its type and citext_cmp - into an index that
 * refuses to open, and an unqualified one would do the same to `ALTER
 * FUNCTION ... RENAME`, although nothing about the order changed.  The source
 * survives all three and still tells a DIFFERENT comparison apart in the
 * common cases: a proc 4 swapped for another function, a string body replaced
 * with CREATE OR REPLACE, a borrowed comparison (rule 2) that now comes from
 * another default btree class.  The argument types are left out on purpose:
 * they are the key type, which the index fixes, and a relocated or renamed
 * type is the same type.
 *
 * WHAT IT CANNOT SEE, and it is a best-effort guard for that reason, not a
 * proof.  A function's source does not include what it calls: a string body
 * that calls another user function which is replaced later compares
 * differently with the same source, and so does a C function whose library
 * is swapped under the same symbol.  Core's btree has the same exposure - its
 * rule is that changing what an opclass function does requires a REINDEX -
 * and so does this index.  A SQL-standard (`RETURN`) body has no prosrc at
 * all; a build never orders by one (lion_fill_column_state()), and a body
 * that becomes one later changes prosrc to the empty string, which this sees.
 */
static uint32
lion_proc_ident(Oid procoid)
{
	HeapTuple	tup;
	Datum		d;
	bool		isnull;
	uint32		h = 0;

	/*
	 * A function that no longer exists at all (a loose family member dropped,
	 * and then the function) is simply a comparison that is not the recorded
	 * one: the caller's REINDEX error, not an internal one.
	 */
	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(procoid));
	if (!HeapTupleIsValid(tup))
		return hash_bytes_uint32(procoid) ^ 0x6c696f6e;

	d = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_prosrc, &isnull);
	if (!isnull)
	{
		char	   *src = TextDatumGetCString(d);

		h = hash_combine(h, hash_bytes((const unsigned char *) src,
									   (int) strlen(src)));
		pfree(src);
	}
	d = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_probin, &isnull);
	if (!isnull)
	{
		char	   *bin = TextDatumGetCString(d);

		h = hash_combine(h, hash_bytes((const unsigned char *) bin,
									   (int) strlen(bin)));
		pfree(bin);
	}
	(void) SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_prosqlbody, &isnull);
	if (!isnull)
		h = hash_combine(h, hash_bytes_uint32(0x5153424f));	/* a RETURN body */
	ReleaseSysCache(tup);

	return h;
}

/*
 * Which comparison each ORDERED key column of ix is read by, as one hash
 * (LionMetaPageData.order_ident, lion_proc_ident() for what "which" means).
 * Zero when no column is ordered.
 */
uint32
lion_order_ident(LionIndexState *ix)
{
	uint32		h = 0;
	int			i;

	for (i = 0; i < ix->ncolumns; i++)
	{
		LionState  *col = &ix->cols[i];

		if (!col->ordered)
			continue;
		h = hash_combine(h, hash_bytes_uint32((uint32) col->attno));
		h = hash_combine(h, lion_proc_ident(col->cmpproc.fn_oid));
	}

	return h;
}

/*
 * Fill in *ix for index: the meta page image the caller supplies (ambuild
 * needs a state before the meta page exists) and every key column.
 */
void
lion_fill_index_state(Relation index, LionIndexState *ix,
					  const LionMetaPageData *meta, MemoryContext cxt)
{
	int			ncols = lion_index_ncolumns(index);
	int			i;

	if (ncols < 1 || ncols > INDEX_MAX_KEYS)
		elog(ERROR, "lion index \"%s\" has %d key columns",
			 RelationGetRelationName(index), ncols);

	memset(ix, 0, sizeof(LionIndexState));
	ix->meta = *meta;
	ix->ncolumns = ncols;
	ix->cols = (LionState *) MemoryContextAllocZero(cxt,
													sizeof(LionState) * ncols);

	for (i = 0; i < ncols; i++)
	{
		ix->cols[i].ix = ix;
		lion_fill_column_state(index, &ix->cols[i], (AttrNumber) (i + 1), cxt);

		/*
		 * Summaries (DESIGN.md §32) are recorded per column by the build, which
		 * gives them only to an ordered scalar column; a meta page that names
		 * another is not one a build wrote.
		 */
		if ((meta->summary_cols & (((uint32) 1) << i)) != 0)
		{
			if (!ix->cols[i].ordered || ix->cols[i].multikey)
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("index \"%s\" records summaries for key column %d, which is not an ordered scalar column",
								RelationGetRelationName(index), i + 1),
						 errhint("REINDEX the index.")));
			ix->cols[i].summarized = true;
		}
	}

	/*
	 * The recorded order names its comparisons as well (§21): a proc 4 that
	 * was added to the family on its own can be dropped and another function
	 * added in its place, and a borrowed comparison follows the key type's
	 * default btree class, which can change too.  The directory is in the
	 * order of the one it was built with, so a different one - as far as
	 * lion_proc_ident() can tell, which is a best-effort answer - is an ERROR
	 * and not a quietly different order.  lion_get_index_state() makes the
	 * same check again for a cached state once a function has changed.
	 */
	if ((meta->order_flags & LION_META_ORDER_RECORDED) != 0 &&
		lion_order_ident(ix) != meta->order_ident)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" was built in the order of a comparison function its operator class no longer uses",
						RelationGetRelationName(index)),
				 errhint("REINDEX the index.")));
}

/*
 * Get the cached per-relation state, building it on first use.
 */
LionIndexState *
lion_get_index_state(Relation index)
{
	LionIndexState *ix;
	LionMetaPageData meta;
	uint64		gen;

	if (!lion_proc_callback_registered)
	{
		CacheRegisterSyscacheCallback(PROCOID, lion_proc_inval_callback,
									  (Datum) 0);
		lion_proc_callback_registered = true;
	}

	/*
	 * A cached state is good until a function has changed since it was last
	 * checked (lion_proc_generation).  Then the recorded order's comparisons
	 * are resolved and compared again - in a throw-away state, which is all
	 * lion_fill_index_state() needs to raise its ERROR - and a state that
	 * passes is good until the next change.  The count is read BEFORE the
	 * check, whose own catalog reads may process more invalidations: one that
	 * arrives meanwhile makes the next call check again.
	 *
	 * THOSE SAME CATALOG READS MAY FLUSH THIS INDEX'S RELCACHE ENTRY, which
	 * pfree()s rd_amcache (see LionAmCache): the handle may be gone by the
	 * time the check returns.  The state it pointed to is not, but it is no
	 * longer the entry's, so the handle is looked up again afterwards and a
	 * state built afresh when the flush left none.  The check reads a COPY of
	 * the meta page image for the same reason.
	 */
	while (index->rd_amcache != NULL)
	{
		LionAmCache *cache = (LionAmCache *) index->rd_amcache;

		ix = cache->ix;
		gen = lion_proc_generation;
		if (ix->procgen == gen)
			return ix;

		/*
		 * Not with a buffer lock held, or inside a critical section: the
		 * check reads the catalog, which is no business of a caller that is
		 * half-way through a page change (lion_wal_mode() is asked for inside
		 * a split, with the meta page locked).  Every LWLock holds off
		 * interrupts, so a positive InterruptHoldoffCount is exactly "some
		 * lock is held"; the state is returned unchecked, and the next call
		 * made with nothing held checks it.
		 */
		if (InterruptHoldoffCount > 0 || CritSectionCount > 0)
			return ix;

		/*
		 * What the cached state CALLS is its own FmgrInfos, the functions it
		 * was filled with - not whatever the catalog would resolve today - so
		 * the question is only whether one of those has changed what it runs
		 * since the index was built: lion_order_ident() over the state's own
		 * comparisons, one syscache lookup per ordered column.  A comparison
		 * the catalog would resolve differently now but this state does not
		 * call cannot hurt it; the next state built answers for that, through
		 * lion_fill_index_state().  (A full fill here cost a hundred syscache
		 * lookups per call under debug_discard_caches, where every call finds
		 * the count moved.)
		 */
		if ((ix->meta.order_flags & LION_META_ORDER_RECORDED) != 0 &&
			ix->meta.ordered_cols != 0)
		{
			uint32		recorded = ix->meta.order_ident;

			if (lion_order_ident(ix) != recorded)
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("index \"%s\" was built in the order of a comparison function its operator class no longer uses",
								RelationGetRelationName(index)),
						 errhint("REINDEX the index.")));

			/* The handle may have been freed by now: look again. */
			if ((LionAmCache *) index->rd_amcache != cache)
				continue;
		}
		ix->procgen = gen;
		return ix;
	}

	gen = lion_proc_generation;
	lion_read_meta(index, &meta);
	lion_remember_wal_mode(index, meta.wal_mode);

	ix = (LionIndexState *) MemoryContextAlloc(index->rd_indexcxt,
											   sizeof(LionIndexState));
	lion_fill_index_state(index, ix, &meta, index->rd_indexcxt);
	ix->procgen = gen;

	/*
	 * Installed only now, so that a flush during the fill above - which reads
	 * the catalog - has no handle to free and leaves this state alone.
	 */
	{
		LionAmCache *cache = (LionAmCache *)
			MemoryContextAlloc(index->rd_indexcxt, sizeof(LionAmCache));

		cache->ix = ix;
		index->rd_amcache = (void *) cache;
	}
	return ix;
}

/*
 * The state of one key column.  attno is an INDEX column number (DESIGN.md
 * §24), which is what a ScanKey's sk_attno and an entry tuple's attno are.
 */
LionState *
lion_index_column_state(Relation index, AttrNumber attno)
{
	LionIndexState *ix = lion_get_index_state(index);

	if (attno < 1 || attno > ix->ncolumns)
		elog(ERROR, "lion index \"%s\" has no key column %d",
			 RelationGetRelationName(index), attno);

	return &ix->cols[attno - 1];
}

/* Column 1, which is all a single-column index has. */
LionState *
lion_get_state(Relation index)
{
	return &lion_get_index_state(index)->cols[0];
}

/*
 * Heap TIDs whose offset does not fit in LION_OFFSET_BITS cannot be encoded.
 */
void
lion_check_key_offset(ItemPointer tid)
{
	if (ItemPointerGetOffsetNumber(tid) > LION_MAX_OFFSET)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("lion index: table access method is not supported"),
				 errdetail("Item pointer offset %u exceeds the maximum of %u supported by this index.",
						   ItemPointerGetOffsetNumber(tid),
						   (unsigned) LION_MAX_OFFSET)));
}

/* ---------------------------------------------------------------------
 * Key handling
 * --------------------------------------------------------------------- */

uint32
lion_hash_key(LionState *state, Datum key)
{
	return DatumGetUInt32(FunctionCall1Coll(&state->hashproc,
											state->collation, key));
}

/*
 * Number of bytes needed to store key, with datumCopy semantics
 * (DESIGN.md section 4).
 */
Size
lion_key_datum_size(LionState *state, Datum key)
{
	Size		size;

	if (state->typbyval)
		size = sizeof(Datum);
	else if (state->typlen > 0)
		size = (Size) state->typlen;
	else if (state->typlen == -1)
	{
		struct varlena *v = PG_DETOAST_DATUM(key);

		size = VARSIZE(v);
		if ((Pointer) v != DatumGetPointer(key))
			pfree(v);
	}
	else
	{
		Assert(state->typlen == -2);
		size = strlen(DatumGetCString(key)) + 1;
	}

	if (size > LION_MAX_KEY_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("lion index key size %zu exceeds maximum %d",
						size, LION_MAX_KEY_SIZE)));

	return size;
}

/*
 * Store key at dest, which must have lion_key_datum_size() bytes.
 */
void
lion_store_key(LionState *state, Datum key, char *dest)
{
	if (state->typbyval)
		memcpy(dest, &key, sizeof(Datum));
	else if (state->typlen > 0)
		memcpy(dest, DatumGetPointer(key), (Size) state->typlen);
	else if (state->typlen == -1)
	{
		struct varlena *v = PG_DETOAST_DATUM(key);

		memcpy(dest, v, VARSIZE(v));
		if ((Pointer) v != DatumGetPointer(key))
			pfree(v);
	}
	else
	{
		char	   *s = DatumGetCString(key);

		memcpy(dest, s, strlen(s) + 1);
	}
}

/*
 * Reconstruct a key Datum from stored bytes.  For by-reference types the
 * result points into src, so it is only valid while the caller keeps the
 * page pinned.
 */
Datum
lion_fetch_key(LionState *state, const char *src)
{
	if (state->typbyval)
	{
		Datum		d;

		memcpy(&d, src, sizeof(Datum));
		return d;
	}

	return PointerGetDatum(src);
}

bool
lion_keys_equal(LionState *state, Datum a, Datum b)
{
	return DatumGetBool(FunctionCall2Coll(&state->eqproc, state->collation,
										  a, b));
}
