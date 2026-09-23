/*-------------------------------------------------------------------------
 *
 * lion_pages.c
 *		Storage primitives for the lion index access method: meta page,
 *		bucket pages and entry tuples, container chains and their splits,
 *		page allocation.  See DESIGN.md sections 4 and 5.
 *
 * Every page modification in this file goes through the WAL shim of
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
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "parser/parse_coerce.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/freespace.h"
#include "storage/indexfsm.h"
#include "utils/hsearch.h"
#include "utils/injection_point.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/typcache.h"
#include "varatt.h"

#include "lion.h"

static void lion_split_and_place(Relation index, Relation heaprel, Buffer buf,
								OffsetNumber off,
								bool replace, Buffer entrybuf,
								OffsetNumber entryoff, LionEntryTuple *entry,
								LionContainer **items, int nitems);

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
 * Initialise a page of the lion index.  Sets up the special area.
 */
void
lion_init_page(Page page, uint16 flags)
{
	LionPageOpaque opaque;

	PageInit(page, BLCKSZ, LION_SPECIAL_SIZE);

	opaque = LionPageGetOpaque(page);
	opaque->rightlink = InvalidBlockNumber;
	opaque->leftlink = InvalidBlockNumber;
	opaque->minckey = 0;
	opaque->maxckey = 0;
	opaque->owner_hash = 0;
	opaque->owner_head = InvalidBlockNumber;
	opaque->level = 0;
	opaque->flags = flags;
	opaque->page_id = LION_PAGE_ID;
	opaque->unused = 0;
}

/*
 * Mark a container page free (DESIGN.md §18).
 *
 * Everything on the page goes except the special area, whose owner_hash and
 * owner_head are deliberately kept: verify() uses them to say which chain a
 * leaked page came from, and the leak sweep uses the flags.  The body holds
 * the safexid, the transaction id from which on no scan can still be holding
 * a link to this page - which is what lion_new_buffer() waits for before
 * handing the block to somebody else.  This mirrors BTPageSetDeleted().
 */
void
lion_page_set_deleted(Page page, FullTransactionId safexid)
{
	LionPageOpaque opaque = LionPageGetOpaque(page);
	LionDeletedPageData *contents;

	Assert((opaque->flags & LION_PAGE_CONTAINER) != 0);

	/* Drop every item and the chain link; keep the owner. */
	((PageHeader) page)->pd_lower = SizeOfPageHeaderData;
	((PageHeader) page)->pd_upper = ((PageHeader) page)->pd_special;
	opaque->flags |= LION_PAGE_DELETED;
	opaque->rightlink = InvalidBlockNumber;
	opaque->minckey = 0;
	opaque->maxckey = 0;

	contents = (LionDeletedPageData *) PageGetContents(page);
	contents->safexid = safexid;
	((PageHeader) page)->pd_lower += sizeof(LionDeletedPageData);
	Assert(((PageHeader) page)->pd_lower <= ((PageHeader) page)->pd_upper);
}

FullTransactionId
lion_page_get_safexid(Page page)
{
	Assert(LionPageIsDeleted(page));

	if (((PageHeader) page)->pd_lower <
		SizeOfPageHeaderData + (int) sizeof(LionDeletedPageData))
		return FirstNormalFullTransactionId;	/* corrupt: never recyclable */

	return ((LionDeletedPageData *) PageGetContents(page))->safexid;
}

/*
 * Is the page at hand one the index may hand out again?
 *
 * The nbtree rule (BTPageIsRecyclable): the page must be DELETED and its
 * safexid must be old enough that no transaction which could still hold a
 * link to it is running.  heaprel is what GlobalVisCheckRemovableFullXid()
 * needs to compute that horizon.
 */
static bool
lion_page_recyclable(Page page, Relation heaprel)
{
	Assert(heaprel != NULL);

	if (PageIsNew(page))
		return true;
	if (PageGetSpecialSize(page) != LION_SPECIAL_SIZE ||
		LionPageGetOpaque(page)->page_id != LION_PAGE_ID)
		return false;
	if (!LionPageIsDeleted(page))
		return false;

	return GlobalVisCheckRemovableFullXid(heaprel, lion_page_get_safexid(page));
}

/*
 * A block for the index: a recycled one when the free space map offers one
 * that is safe to take, else a fresh one from extending the relation.
 *
 * The buffer comes back pinned and EXCLUSIVE, with an uninitialised page that
 * the caller must lion_init_page() inside its own WAL record.
 *
 * Only a CONDITIONAL lock is ever taken on a recycled page, exactly as
 * _bt_allocbuf() does and for the same reason: this is called with other
 * pages of this index already locked (a split holds P, the bucket page, ...),
 * and buffer content locks have no deadlock detection.  A page that cannot be
 * locked, or that turns out not to be recyclable, is put straight back in the
 * free space map and the relation is extended instead - which also keeps this
 * loop from spinning on a block the map keeps offering.
 *
 * Note what is NOT here: nbtree writes an XLOG_BTREE_REUSE_PAGE record so
 * that replay can cancel a standby query that might still hold a link to the
 * block.  Generic WAL cannot raise a recovery conflict, so a standby reader
 * is protected by the owner check in the page's special area instead
 * (DESIGN.md §18).
 */
Buffer
lion_alloc_page(Relation index, Relation heaprel, bool reuse)
{
	if (reuse && heaprel != NULL)
	{
		BlockNumber blkno = GetFreeIndexPage(index);

		if (BlockNumberIsValid(blkno))
		{
			Buffer		buf = ReadBuffer(index, blkno);

			if (ConditionalLockBuffer(buf))
			{
				if (lion_page_recyclable(BufferGetPage(buf), heaprel))
					return buf;
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			}

			/* Not ours to take now; leave it for the next allocation. */
			RecordFreeIndexPage(index, blkno);
			ReleaseBuffer(buf);
		}
	}

	return ExtendBufferedRel(BMR_REL(index), MAIN_FORKNUM, NULL, EB_LOCK_FIRST);
}

/*
 * Give a page back that a caller took and did not use.
 *
 * Nothing was written to it, so there is nothing to log and nothing to undo:
 * the block is either all-zero (it came from extending the relation) or still
 * the DELETED page the free space map offered, and both are exactly what
 * lion_alloc_page() accepts.  Recording it in the map hands it to the next
 * allocation, so a split that turns out to need one page instead of two costs
 * a page once and never again.
 *
 * DESIGN.md §25: the alternative - deciding inside the record - is not open
 * any more, because an rmgr-mode record runs in a critical section.
 */
void
lion_release_unused_page(Relation index, Buffer buf)
{
	BlockNumber blk = BufferGetBlockNumber(buf);

	UnlockReleaseBuffer(buf);
	RecordFreeIndexPage(index, blk);
}

/*
 * Fill in a meta page image.
 */
void
lion_init_metapage(Page page, uint32 inline_limit, BlockNumber root,
				  uint32 height, uint32 dirpages, uint32 wal_mode)
{
	LionMetaPageData *meta;

	lion_init_page(page, LION_PAGE_META);

	meta = LionPageGetMeta(page);
	memset(meta, 0, sizeof(LionMetaPageData));
	meta->magic = LION_MAGIC;
	meta->version = LION_VERSION;
	meta->offset_bits = LION_OFFSET_BITS;
	meta->container_bits = LION_CONTAINER_BITS;
	meta->unused_nbuckets = 0;
	meta->inline_limit = inline_limit;
	meta->root = root;
	meta->height = height;
	meta->dirpages = dirpages;
	meta->wal_mode = wal_mode;

	((PageHeader) page)->pd_lower += sizeof(LionMetaPageData);
	Assert(((PageHeader) page)->pd_lower <= ((PageHeader) page)->pd_upper);
}

/*
 * Bring a page the caller already took into its open record.
 *
 * The buffer must come from lion_alloc_page(), which is the fallible half and
 * has to happen before the record opens (DESIGN.md §25).  Registering it here
 * as a page this record INITIALISES is what keeps a crash from leaving an
 * initialised page that nothing points at: a generic record logs the
 * initialisation as a full image, an rmgr record as a PAGE_INIT operation and
 * no image at all.
 */
Page
lion_wal_init_buffer(LionWalState *state, Buffer buf, uint16 flags)
{
	Page		page;

	page = lion_wal_register_buffer(state, buf, LION_WALBUF_INIT);
	lion_init_page(page, flags);
	lion_wal_op(state, page, LION_OP_INIT, 0, flags, NULL, 0);

	return page;
}

/*
 * Log the special area of a page this record registered, as it now stands.
 */
void
lion_wal_log_special(LionWalState *state, Page page)
{
	lion_wal_op(state, page, LION_OP_SPECIAL, 0, 0, LionPageGetOpaque(page),
				sizeof(LionPageOpaqueData));
}

/*
 * A fresh page in a record of its own, for a caller that has no record open.
 *
 * reuse is implied here; a chain's HEAD page, which must never come from the
 * free space map (DESIGN.md §18), is allocated with lion_alloc_page(...,
 * false) by the caller that links it in.
 */
Buffer
lion_new_buffer(Relation index, Relation heaprel, uint16 flags)
{
	Buffer		buffer;
	LionWalState *state;

	buffer = lion_alloc_page(index, heaprel, true);

	state = lion_wal_begin(index);
	lion_wal_init_buffer(state, buffer, flags);
	lion_wal_finish(state, LION_XLOG_PAGE_INIT);

	return buffer;
}

/*
 * Read and validate the meta page.
 */
void
lion_read_meta(Relation index, LionMetaPageData *meta)
{
	Buffer		buf;
	Page		page;
	LionMetaPageData *ondisk;

	buf = ReadBuffer(index, LION_METAPAGE_BLKNO);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);

	if (PageIsNew(page) || !LionPageIsMeta(page) ||
		LionPageGetOpaque(page)->page_id != LION_PAGE_ID)
	{
		UnlockReleaseBuffer(buf);
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" is not a valid lion index",
						RelationGetRelationName(index))));
	}

	ondisk = LionPageGetMeta(page);
	*meta = *ondisk;
	UnlockReleaseBuffer(buf);

	if (meta->magic != LION_MAGIC || meta->version != LION_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" is not a valid lion index",
						RelationGetRelationName(index)),
				 errdetail("Meta page magic %08X version %u, expected %08X version %u.",
						   meta->magic, meta->version, LION_MAGIC, LION_VERSION),
				 errhint("REINDEX the index: its on-disk format predates this build of pg_lion.")));

	if (meta->offset_bits != LION_OFFSET_BITS ||
		meta->container_bits != LION_CONTAINER_BITS)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" was built for a different block size",
						RelationGetRelationName(index)),
				 errdetail("Index has offset_bits %u, container_bits %u; this build uses %d and %d.",
						   meta->offset_bits, meta->container_bits,
						   LION_OFFSET_BITS, LION_CONTAINER_BITS)));

	if (!BlockNumberIsValid(meta->root))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" has no directory root",
						RelationGetRelationName(index))));
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
	CompareType cmptype;

	if (!OidIsValid(ltopr) || !OidIsValid(cmpfunc))
		return false;
	if (!get_ordering_op_properties(ltopr, &opfamily, &opcintype, &cmptype))
		return false;
	if (cmptype != COMPARE_LT)
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
	}
}

/*
 * Get the cached per-relation state, building it on first use.
 */
LionIndexState *
lion_get_index_state(Relation index)
{
	LionIndexState *ix;
	LionMetaPageData meta;

	if (index->rd_amcache != NULL)
		return (LionIndexState *) index->rd_amcache;

	lion_read_meta(index, &meta);

	ix = (LionIndexState *) MemoryContextAlloc(index->rd_indexcxt,
											   sizeof(LionIndexState));
	lion_fill_index_state(index, ix, &meta, index->rd_indexcxt);

	index->rd_amcache = (void *) ix;
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

/* ---------------------------------------------------------------------
 * Entry tuples
 * --------------------------------------------------------------------- */

/*
 * Fetch the item (container or sparse segment) at *off of an INLINE payload
 * into the aligned buffer buf and advance *off past it.  Returns the item
 * size, or 0 when the payload has been consumed.
 *
 * The payload may be followed by ZEROED SLACK: VACUUM writes a filtered
 * payload back into the bytes the entry already has rather than shrinking the
 * entry tuple, so that no other entry on the bucket page moves and the WAL
 * delta is the handful of bytes that changed (DESIGN.md §18).  No real item
 * has type 0 - the four item kinds are 1 .. 4 - so a zero item header is an
 * unambiguous end marker and the payload needs no length of its own.  The
 * same goes for a tail shorter than one header.
 */
Size
lion_inline_fetch(const char *payload, Size paylen, Size *off, LionContainer *buf)
{
	Size		avail;
	Size		peek;
	Size		csize;

	Assert(*off <= paylen);
	avail = paylen - *off;
	if (avail < LION_CONTAINER_HDRSZ)
		return 0;				/* end of the payload, or its zeroed slack */

	/*
	 * The item may be unaligned, so everything is read through the caller's
	 * buffer.  lion_container_size() of a RUN container needs the nruns field,
	 * which is the first uint16 of the payload, so peek that far before asking
	 * for the size.  Every other item kind, sparse segments included, is sized
	 * from the header alone.
	 */
	peek = Min(avail, LION_CONTAINER_HDRSZ + sizeof(uint16));
	memcpy(buf, payload + *off, peek);
	if (buf->type == 0)
		return 0;				/* the slack VACUUM left behind */
	if (buf->type != LION_CT_ARRAY && buf->type != LION_CT_BITSET &&
		buf->type != LION_CT_RUN && buf->type != LION_CT_SPARSE)
		elog(ERROR, "lion index: malformed inline item of type %u",
			 buf->type);
	csize = lion_item_size(buf);
	if (csize < LION_CONTAINER_HDRSZ || csize > LION_CONTAINER_MAX_SIZE ||
		csize > avail)
		elog(ERROR, "lion index: malformed inline item");

	memcpy(buf, payload + *off, csize);
	*off += csize;

	return csize;
}

/*
 * Build an entry tuple in palloc'd memory.
 *
 * The layout is: header, key bytes, MAXALIGN padding, payload.  For INLINE
 * entries the payload is a sequence of containers in ascending ckey order,
 * packed back to back with no padding, so a container inside it is generally
 * unaligned; read them with lion_inline_fetch().  head/tail/ncontainers/ntids
 * are left at their empty values; the caller fills them in.
 */
LionEntryTuple *
lion_make_entry(LionState *state, Datum key, uint32 hash, uint16 flags,
			   const char *payload, Size payloadlen, Size *size)
{
	LionEntryTuple *entry;
	Size		keylen = lion_key_datum_size(state, key);
	Size		payoff = MAXALIGN((LION_ENTRY_HDRSZ) + keylen);
	Size		total = payoff + payloadlen;

	if (total > LION_MAX_ENTRY_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("lion index entry of %zu bytes is too large for a directory leaf",
						total)));

	entry = (LionEntryTuple *) palloc0(total);
	entry->hash = hash;
	entry->flags = flags;
	entry->keylen = (uint16) keylen;
	entry->head = InvalidBlockNumber;
	entry->tail = InvalidBlockNumber;
	entry->ncontainers = 0;
	entry->attno = state->attno;
	entry->ntids = 0;

	lion_store_key(state, key, LionEntryGetKey(entry));

	if (payloadlen > 0)
	{
		Assert(payload != NULL);
		memcpy(((char *) entry) + payoff, payload, payloadlen);
	}

	*size = total;
	return entry;
}

/*
 * Build a reserved entry tuple: the NULL-key one (DESIGN.md §14) or the
 * no-key one a multi-key opclass needs for rows it extracts nothing from
 * (DESIGN.md §17).
 *
 * Neither has a key at all: keylen 0, hash 0, and the reservedflag bit, which
 * is how every reader recognises them.  The payload is an ordinary INLINE
 * payload, so once such an entry exists the insert, VACUUM and count paths
 * treat it exactly like any other.
 */
LionEntryTuple *
lion_make_reserved_entry(AttrNumber attno, uint16 reservedflag, uint16 flags,
						const char *payload, Size payloadlen, Size *size)
{
	LionEntryTuple *entry;
	Size		payoff = MAXALIGN(LION_ENTRY_HDRSZ);
	Size		total = payoff + payloadlen;

	Assert(reservedflag == LION_ENTRY_NULLKEY ||
		   reservedflag == LION_ENTRY_EMPTYKEY);
	Assert(attno >= 1);

	if (total > LION_MAX_ENTRY_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("lion index entry of %zu bytes is too large for a directory leaf",
						total)));

	entry = (LionEntryTuple *) palloc0(total);
	entry->hash = LION_NULLKEY_HASH;
	entry->flags = flags | reservedflag;
	entry->keylen = 0;
	entry->head = InvalidBlockNumber;
	entry->tail = InvalidBlockNumber;
	entry->ncontainers = 0;
	entry->attno = (uint16) attno;
	entry->ntids = 0;

	if (payloadlen > 0)
	{
		Assert(payload != NULL);
		memcpy(((char *) entry) + payoff, payload, payloadlen);
	}

	*size = total;
	return entry;
}

/*
 * Private copy of an entry tuple with a new payload.
 *
 * The header, the key bytes and their alignment padding are taken from entry
 * (which usually points into a page); payload replaces whatever payload the
 * original had.  A zero-length payload produces the shape of a CHAIN entry.
 * The caller owns the result and fills in flags, head, tail, ncontainers and
 * ntids as needed.
 */
LionEntryTuple *
lion_entry_rebuild(const LionEntryTuple *entry, const char *payload,
				  Size payloadlen, Size *size)
{
	Size		payoff = LionEntryPayloadOffset(entry);
	Size		total = payoff + payloadlen;
	LionEntryTuple *copy;

	if (total > LION_MAX_ENTRY_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("lion index entry of %zu bytes is too large for a directory leaf",
						total)));

	copy = (LionEntryTuple *) palloc(total);
	memcpy(copy, entry, payoff);
	if (payloadlen > 0)
	{
		Assert(payload != NULL);
		memcpy(((char *) copy) + payoff, payload, payloadlen);
	}

	*size = total;
	return copy;
}

/*
 * The same, but with the entry ALLOCATED at `allocsz` bytes and everything
 * past the payload zeroed: growth slack for the next insert into this key
 * (DESIGN.md §4, applied to an INLINE payload).
 *
 * The slack needs no length field, because lion_inline_fetch() stops at the
 * first zero item header - no item kind is 0 - which is the convention
 * VACUUM's shrink-in-place already relies on (DESIGN.md §18).  So the payload
 * is self-terminating and every reader of it, the spill included, sees exactly
 * the items that are there.
 */
LionEntryTuple *
lion_entry_rebuild_slack(const LionEntryTuple *entry, const char *payload,
						 Size payloadlen, Size allocsz, Size *size)
{
	Size		payoff = LionEntryPayloadOffset(entry);
	LionEntryTuple *copy;

	Assert(allocsz >= payoff + payloadlen);

	if (allocsz > LION_MAX_ENTRY_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("lion index entry of %zu bytes is too large for a directory leaf",
						allocsz)));

	copy = (LionEntryTuple *) palloc0(allocsz);
	memcpy(copy, entry, payoff);
	if (payloadlen > 0)
	{
		Assert(payload != NULL);
		memcpy(((char *) copy) + payoff, payload, payloadlen);
	}

	*size = allocsz;
	return copy;
}

/*
 * Allocated length for an INLINE entry an INSERT is rewriting, so that the
 * next few members land inside the bytes it already has (DESIGN.md §4).
 *
 * The rule is the one items use - payload/8, clamped into
 * [LION_ITEM_SLACK_MIN, LION_ITEM_SLACK_MAX] - with two caps of its own.  The
 * slack is payload BYTES THAT HOLD NO PAYLOAD, so it may never push the stored
 * payload past `inline_limit`, and it may never be worth splitting the leaf
 * for: `maxsize` is the largest the entry may become without one, and the
 * caller works it out from the free space on the page.
 *
 * What this must NOT do is make a key spill early.  The spill test is made on
 * the REAL payload length before this is called, so an entry whose payload is
 * still inside inline_limit stays INLINE however much slack it carries; and an
 * entry whose payload has reached inline_limit gets none, because it is about
 * to spill anyway.  Only inserts add it: ambuild and VACUUM write entries at
 * the length they need (VACUUM keeps the length it finds, which is the same
 * slack seen from the other side).
 */
Size
lion_entry_alloc_size(Size payoff, Size paylen, Size inline_limit, Size maxsize)
{
	Size		extra;
	Size		alloc;

	extra = paylen / LION_ITEM_SLACK_FRACTION;
	extra = Max(extra, (Size) LION_ITEM_SLACK_MIN);
	extra = Min(extra, (Size) LION_ITEM_SLACK_MAX);
	extra = MAXALIGN(extra);

	if (paylen + extra > inline_limit)
		extra = (inline_limit > paylen) ? inline_limit - paylen : 0;

	alloc = payoff + paylen + extra;
	if (alloc > (Size) LION_MAX_ENTRY_SIZE)
		alloc = (Size) LION_MAX_ENTRY_SIZE;
	if (alloc > maxsize)
		alloc = maxsize;
	if (alloc < payoff + paylen)
		alloc = payoff + paylen;	/* no room for slack, or none wanted */

	Assert(alloc - (payoff + paylen) <= LION_ENTRY_SLACK_BOUND);
	return alloc;
}

/*
 * Find the entry for (hash, key) in the directory, comparing stored keys with
 * eqproc and ordering them with cmpproc (NULL for either means the index's
 * own).  DESIGN.md §21 replaced the bucket chain this used to walk with a
 * descent: on success *buf is a directory LEAF locked in lockmode, which the
 * caller releases, and *offnum is the entry's offset on it.
 *
 * On failure *buf is normally still a locked leaf - the one the key would go
 * on, which is what the insert path wants - but it is InvalidBuffer when the
 * cross-type fallback walk of lion_dir_find() ran, so a caller that uses it
 * must be one that never searches cross-type.
 */
bool
lion_find_entry_ext(Relation index, LionState *state, int lockmode, Datum key,
				   uint32 hash, FmgrInfo *eqproc, FmgrInfo *cmpproc,
				   Oid collation, Buffer *buf, OffsetNumber *offnum)
{
	LionSearchKey sk;

	lion_search_key_init(state, &sk, LION_KIND_VALUE, key, hash);
	if (eqproc != NULL)
	{
		sk.eqproc = eqproc;
		sk.cmpproc = cmpproc;
		sk.collation = collation;
	}

	return lion_dir_find(index, NULL, state->ix, &sk, lockmode, false,
						 buf, offnum, NULL);
}

bool
lion_find_entry(Relation index, LionState *state, int lockmode, Datum key,
			   uint32 hash, Buffer *buf, OffsetNumber *offnum)
{
	return lion_find_entry_ext(index, state, lockmode, key, hash, NULL, NULL,
							  InvalidOid, buf, offnum);
}

/*
 * Find a reserved entry (DESIGN.md §14 and §17).  Both sort before every real
 * key (LION_KIND_NULL and LION_KIND_EMPTY), so they live on the leftmost leaf
 * and are reached by the same descent as anything else.
 */
bool
lion_find_reserved_entry(Relation index, LionState *state, int lockmode,
						uint16 reservedflag, Buffer *buf, OffsetNumber *offnum)
{
	LionSearchKey sk;

	Assert(reservedflag == LION_ENTRY_NULLKEY ||
		   reservedflag == LION_ENTRY_EMPTYKEY);

	lion_search_key_init(state, &sk,
						 (reservedflag == LION_ENTRY_NULLKEY) ?
						 LION_KIND_NULL : LION_KIND_EMPTY,
						 (Datum) 0, LION_NULLKEY_HASH);

	return lion_dir_find(index, NULL, state->ix, &sk, lockmode, false,
						 buf, offnum, NULL);
}

/*
 * Replace the entry at (buf, offnum).  Returns false and changes nothing if
 * the new version does not fit.
 */
bool
lion_replace_entry(Relation index, LionWalState *state, Buffer buf,
				  OffsetNumber offnum, LionEntryTuple *entry, Size size)
{
	LionWalState *wal = state;
	Page		page;
	bool		ok;

	if (wal == NULL)
		wal = lion_wal_begin(index);

	page = lion_wal_register_buffer(wal, buf, LION_WALBUF_STD);

	/*
	 * PageIndexTupleOverwrite() tests before it writes, so a failure here has
	 * changed nothing - which is what lets an rmgr-mode record be abandoned
	 * at this point even though it is inside a critical section (DESIGN.md
	 * §25).
	 */
	lion_wal_save_item(wal, page, offnum);
	ok = PageIndexTupleOverwrite(page, offnum, entry, size);
	if (ok)
		lion_wal_op_replace(wal, page, offnum, entry, size);

	if (state == NULL)
	{
		if (ok)
			lion_wal_finish(wal, LION_XLOG_ENTRY);
		else
			lion_wal_abort(wal);
	}

	return ok;
}

/* ---------------------------------------------------------------------
 * Container chains
 * --------------------------------------------------------------------- */

/*
 * Recompute minckey/maxckey of a container page from its items.
 *
 * The bounds are the first ckey of the first item and the LAST ckey of the
 * last item: a sparse segment covers a range, and the whole point of
 * minckey/maxckey is to say which ckeys this page owns (DESIGN.md §13).
 */
void
lion_page_update_minmax(Page page)
{
	LionPageOpaque opaque = LionPageGetOpaque(page);
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

	if (maxoff < FirstOffsetNumber)
	{
		opaque->minckey = 0;
		opaque->maxckey = 0;
		return;
	}

	opaque->minckey = lion_item_first_ckey((LionContainer *)
										  PageGetItem(page,
													  PageGetItemId(page, FirstOffsetNumber)));
	opaque->maxckey = lion_item_last_ckey((LionContainer *)
										 PageGetItem(page,
													 PageGetItemId(page, maxoff)));
}

/*
 * Binary search a container page for the item that covers ckey: a container
 * whose ckey it is, or a sparse segment whose range it falls in.
 *
 * With *found = false the result is the offset an item for ckey would have to
 * take: the first item whose first ckey is above ckey (one past the last item
 * when there is none).  Items are ordered by first ckey and their ranges
 * never overlap, so "the last item whose first ckey is <= ckey" is the only
 * item that can possibly cover it.
 */
OffsetNumber
lion_page_find_item(Page page, uint32 ckey, bool *found)
{
	OffsetNumber low = FirstOffsetNumber;
	OffsetNumber high = PageGetMaxOffsetNumber(page);
	OffsetNumber cand = InvalidOffsetNumber;

	*found = false;

	while (low <= high)
	{
		OffsetNumber mid = low + (high - low) / 2;
		LionContainer *c = (LionContainer *) PageGetItem(page, PageGetItemId(page, mid));

		if (lion_item_first_ckey(c) <= ckey)
		{
			cand = mid;
			low = mid + 1;
		}
		else
		{
			if (mid == FirstOffsetNumber)
				break;
			high = mid - 1;
		}
	}

	if (cand == InvalidOffsetNumber)
		return FirstOffsetNumber;	/* ckey belongs before every item */

	{
		LionContainer *c = (LionContainer *) PageGetItem(page, PageGetItemId(page, cand));

		if (lion_item_last_ckey(c) >= ckey)
		{
			*found = true;
			return cand;
		}
	}

	return OffsetNumberNext(cand);
}

/*
 * Binary search a container page for ckey.  When not found, the returned
 * offset is the position the container should be inserted at.
 */
OffsetNumber
lion_page_find_container(Page page, uint32 ckey, bool *found)
{
	OffsetNumber low = FirstOffsetNumber;
	OffsetNumber high = PageGetMaxOffsetNumber(page);

	*found = false;

	/* Invariant: everything below low is < ckey, everything above high is > ckey */
	while (low <= high)
	{
		OffsetNumber mid = low + (high - low) / 2;
		LionContainer *c = (LionContainer *) PageGetItem(page, PageGetItemId(page, mid));

		if (c->ckey == ckey)
		{
			*found = true;
			return mid;
		}
		if (c->ckey < ckey)
			low = mid + 1;
		else
		{
			if (mid == FirstOffsetNumber)
				return FirstOffsetNumber;
			high = mid - 1;
		}
	}

	return low;
}

/*
 * Write the caller's private copy of the entry tuple back into the record
 * that is changing the entry's containers.  An entry's size never changes
 * once it is a CHAIN entry, so this cannot fail.
 */
static void
lion_put_entry(Relation index, LionWalState *xstate, Buffer entrybuf,
			  OffsetNumber entryoff, LionEntryTuple *entry)
{
	if (!lion_replace_entry(index, xstate, entrybuf, entryoff, entry,
						   LionEntryPayloadOffset(entry)))
		elog(ERROR, "lion index: could not update entry tuple at %u/%u",
			 BufferGetBlockNumber(entrybuf), entryoff);
}

/*
 * Allocated length for an item that is about to be written to a container
 * page, with room to grow in place (DESIGN.md §4, "growth slack").
 *
 * The slack is a fraction of the item rather than a fixed number of bytes,
 * because both extremes of item size are common: a key with many container
 * keys owns dozens of ~50-byte items per page, where 64 bytes of slack each
 * would nearly halve the page's capacity, while a key with one big ARRAY per
 * container key wants as much room as it can get.  MAXALIGN padding, which
 * the page spends on the item either way, is part of the slack and therefore
 * free.
 */
Size
lion_item_alloc_size(const LionContainer *item, Size size)
{
	Size		extra;
	Size		alloc;

	Assert(size == lion_item_size(item));

	/* A bitset is already the largest an item can be. */
	if (item->type == LION_CT_BITSET)
		return size;

	extra = size / LION_ITEM_SLACK_FRACTION;
	extra = Max(extra, (Size) LION_ITEM_SLACK_MIN);
	extra = Min(extra, (Size) LION_ITEM_SLACK_MAX);

	alloc = MAXALIGN(size) + MAXALIGN(extra);
	if (alloc > (Size) LION_CONTAINER_MAX_SIZE)
		alloc = Min(MAXALIGN(size), (Size) LION_CONTAINER_MAX_SIZE);

	Assert(alloc >= size && alloc - size <= LION_ITEM_SLACK_LIMIT);
	return alloc;
}

/*
 * Zero the slack of an item in the caller's work buffer, so that the bytes
 * that land on the page are the same every time the item is written: a
 * GenericXLog delta is a byte-wise diff of the page image, and garbage in the
 * slack would put the whole item in every record.
 */
static void
lion_item_zero_slack(LionContainer *item, Size size, Size alloc)
{
	Assert(alloc >= size);
	if (alloc > size)
		memset((char *) item + size, 0, alloc - size);
}

/*
 * Insert or replace a container on the container page buf, which the caller
 * holds EXCLUSIVE (or with a cleanup lock) and which owns c->ckey, splitting
 * the page when the container does not fit.  The buffer stays locked.
 *
 * entrybuf/entryoff identify the entry tuple, which the caller holds
 * EXCLUSIVE; entry must be a private (palloc'd) copy with ntids and
 * ncontainers already updated for this change.  It is written back in the
 * same WAL record as the container, so that a crash can never desynchronise
 * the counters from the containers.
 */
void
lion_chain_put_container_locked_ext(Relation index, Relation heaprel, Buffer buf,
								   Buffer entrybuf,
								   OffsetNumber entryoff, LionEntryTuple *entry,
								   LionContainer *c, int *ncontainers_delta,
								   bool slack)
{
	Page		page = BufferGetPage(buf);
	OffsetNumber off;
	bool		found;

	Assert(c->type != LION_CT_SPARSE);
	Assert(LionPageIsContainer(page));

	off = lion_page_find_container(page, c->ckey, &found);
	*ncontainers_delta = found ? 0 : 1;

	lion_chain_put_items_locked_ext(index, heaprel, buf, entrybuf, entryoff,
								   entry, off, found, &c, 1, slack);
}

void
lion_chain_put_container_locked(Relation index, Relation heaprel, Buffer buf,
							   Buffer entrybuf,
							   OffsetNumber entryoff, LionEntryTuple *entry,
							   LionContainer *c, int *ncontainers_delta)
{
	lion_chain_put_container_locked_ext(index, heaprel, buf, entrybuf, entryoff,
									   entry, c, ncontainers_delta, false);
}

/*
 * Replace the item at off, or insert at off, with nitems items.
 *
 * The items must be in ascending ckey order and their ranges must fit in the
 * gap the replaced item (or the insert position) leaves, so that the page
 * stays ordered by first ckey with non-overlapping item ranges (DESIGN.md
 * §13).  Everything happens in one WAL record together with the entry tuple,
 * which is what keeps a segment that is being split around a promoted
 * container from being visible as two items holding the same ckey.
 */
void
lion_chain_put_items_locked_ext(Relation index, Relation heaprel, Buffer buf,
							   Buffer entrybuf,
							   OffsetNumber entryoff, LionEntryTuple *entry,
							   OffsetNumber off, bool replace,
							   LionContainer **items, int nitems, bool slack)
{
	Page		page = BufferGetPage(buf);
	Size		sizes[LION_MAX_PUT_ITEMS];
	Size		allocs[LION_MAX_PUT_ITEMS];
	Size		need = 0;
	Size		want = 0;
	Size		have;
	int			i;

	Assert((entry->flags & LION_ENTRY_CHAIN) != 0);
	Assert(BlockNumberIsValid(entry->head) && BlockNumberIsValid(entry->tail));
	Assert(LionPageIsContainer(page));
	Assert(nitems >= 1 && nitems <= LION_MAX_PUT_ITEMS);

	for (i = 0; i < nitems; i++)
	{
		sizes[i] = lion_item_size(items[i]);
		Assert(sizes[i] <= LION_CONTAINER_MAX_SIZE);
		Assert(i == 0 ||
			   lion_item_first_ckey(items[i]) > lion_item_last_ckey(items[i - 1]));
		allocs[i] = slack ? lion_item_alloc_size(items[i], sizes[i]) : sizes[i];
		need += MAXALIGN(sizes[i]) + sizeof(ItemIdData);
		want += MAXALIGN(allocs[i]) + sizeof(ItemIdData);
	}
	Assert(need <= LION_MAX_ITEM_SIZE + sizeof(ItemIdData));

	/* One item taking another one's place: overwrite it where it is. */
	if (replace && nitems == 1)
	{
		Size		cur = ItemIdGetLength(PageGetItemId(page, off));
		Size		writesz = sizes[0];

		/*
		 * Prefer to leave the item's allocated length exactly as it is: then
		 * PageIndexTupleOverwrite() moves no other item on the page and the
		 * WAL delta covers the item alone.  That is the whole of "shrink in
		 * place" (DESIGN.md §18) as well as the growth case: an item that has
		 * lost members keeps its slot and the freed bytes become slack.  An
		 * item that has far more room than it can use - a segment replaced by
		 * the container one of its container keys was promoted to, or one
		 * that lost most of its members - gives the excess back.
		 *
		 * cur comes off the page, and the item is copied out of a work buffer
		 * of LION_CONTAINER_MAX_SIZE bytes, so a page that claims more than
		 * that (only a corrupt one can) gets the exact size.
		 */
		if (cur >= sizes[0] && cur <= (Size) LION_CONTAINER_MAX_SIZE &&
			cur - sizes[0] <= LION_ITEM_SLACK_BOUND)
			writesz = cur;
		else if (slack &&
				 MAXALIGN(allocs[0]) <=
				 MAXALIGN(cur) + PageGetExactFreeSpace(page))
			writesz = allocs[0];
		lion_item_zero_slack(items[0], sizes[0], writesz);

		{
			LionWalState *xstate = lion_wal_begin(index);
			Page		p = lion_wal_register_buffer(xstate, buf, LION_WALBUF_STD);

			lion_wal_save_item(xstate, p, off);
			if (PageIndexTupleOverwrite(p, off, items[0], writesz))
			{
				/*
				 * An item that is rewritten at (or near) the length it already
				 * has differs from its predecessor in a handful of bytes - one
				 * more member of an ARRAY, the two-byte cardinality - so the
				 * record carries those bytes and not the 1.6 KB item
				 * (DESIGN.md §25, LION_OP_DELTA).
				 */
				lion_wal_op_replace(xstate, p, off, items[0], writesz);
				lion_page_update_minmax(p);
				lion_wal_op(xstate, p, LION_OP_MINMAX, 0, 0, NULL, 0);
				lion_put_entry(index, xstate, entrybuf, entryoff, entry);
				lion_wal_finish(xstate, LION_XLOG_ITEM_REPLACE);
				return;
			}
			lion_wal_abort(xstate);
		}
	}

	/*
	 * Does everything fit as it stands?  PageIndexTupleDelete() compacts, so
	 * the space of the item that goes away is available to the new ones.
	 */
	have = PageGetExactFreeSpace(page);
	if (replace)
		have += MAXALIGN(ItemIdGetLength(PageGetItemId(page, off))) +
			sizeof(ItemIdData);

	/* Slack is a luxury: drop all of it rather than split the page for it. */
	if (want > have)
	{
		for (i = 0; i < nitems; i++)
			allocs[i] = sizes[i];
		want = need;
	}

	if (have >= want)
	{
		LionWalState *xstate = lion_wal_begin(index);
		Page		p = lion_wal_register_buffer(xstate, buf, LION_WALBUF_STD);

		if (replace)
		{
			PageIndexTupleDelete(p, off);
			lion_wal_op(xstate, p, LION_OP_DELETE, off, 0, NULL, 0);
		}

		for (i = 0; i < nitems; i++)
		{
			lion_item_zero_slack(items[i], sizes[i], allocs[i]);
			if (PageAddItemExtended(p, items[i], allocs[i],
									off + (OffsetNumber) i,
									0) == InvalidOffsetNumber)
				elog(ERROR, "lion index: failed to add item to page %u",
					 BufferGetBlockNumber(buf));
			lion_wal_op(xstate, p, LION_OP_ADD, off + (OffsetNumber) i, 0,
						items[i], allocs[i]);
		}

		lion_page_update_minmax(p);
		lion_wal_op(xstate, p, LION_OP_MINMAX, 0, 0, NULL, 0);
		lion_put_entry(index, xstate, entrybuf, entryoff, entry);
		lion_wal_finish(xstate, LION_XLOG_ITEM_ADD);
		return;
	}

	/*
	 * Not enough room: split the page and place the items.  The split places
	 * them at their exact size - a page that has just been split has room to
	 * spare, and the items get their slack back the next time they grow.
	 */
	lion_split_and_place(index, heaprel, buf, off, replace, entrybuf, entryoff,
						entry, items, nitems);
}

void
lion_chain_put_items_locked(Relation index, Relation heaprel, Buffer buf,
						   Buffer entrybuf,
						   OffsetNumber entryoff, LionEntryTuple *entry,
						   OffsetNumber off, bool replace,
						   LionContainer **items, int nitems)
{
	lion_chain_put_items_locked_ext(index, heaprel, buf, entrybuf, entryoff,
								   entry, off, replace, items, nitems, false);
}

/*
 * Insert or replace a container in the chain of entry, splitting pages when
 * necessary.  Finds and locks the owning container page itself; see
 * lion_chain_put_container_locked() for the contract on entry.
 */
void
lion_chain_put_container(Relation index, Relation heaprel, Buffer entrybuf,
						OffsetNumber entryoff,
						LionEntryTuple *entry, LionContainer *c,
						int *ncontainers_delta)
{
	Buffer		buf;

	Assert((entry->flags & LION_ENTRY_CHAIN) != 0);
	Assert(BlockNumberIsValid(entry->head) && BlockNumberIsValid(entry->tail));

	/*
	 * A write descent, which takes the leaf EXCLUSIVE straight away and
	 * repairs any unfinished split on the way (DESIGN.md §22).
	 */
	buf = lion_posting_search(index, heaprel, entry->hash, entry->head,
							  c->ckey, BUFFER_LOCK_EXCLUSIVE, true);

	lion_chain_put_container_locked(index, heaprel, buf, entrybuf, entryoff,
								   entry, c, ncontainers_delta);

	UnlockReleaseBuffer(buf);
}

/*
 * Move an INLINE entry's payload onto a chain of container pages and turn it
 * into a CHAIN entry.
 *
 * entry is a private copy of the entry tuple in its CHAIN shape (no payload,
 * with ncontainers and ntids already set by the caller); payload holds the
 * containers to write out, packed as in an INLINE payload.  The entry page is
 * held EXCLUSIVE by the caller and is rewritten here, in the same WAL record
 * as the last container page.
 */
/*
 * How many LEAVES an INLINE payload needs once it is on container pages.
 *
 * This has to agree exactly with the packing loop below, because the answer
 * decides whether the page the entry's `head` names is the single leaf or the
 * ROOT above several of them - and a root has to be allocated BEFORE the
 * leaves, since every page of a posting set is stamped with the root's block
 * (DESIGN.md §18, §22).  An inline payload is at most `inline_limit` bytes and
 * the smallest item is ten, so this is at most two in practice; the loop is
 * written for any number anyway.
 */
static int
lion_spill_count_leaves(const char *payload, Size paylen, LionContainer *cbuf)
{
	Size		off = 0;
	Size		csize;
	Size		used = 0;
	int			n = 1;

	while ((csize = lion_inline_fetch(payload, paylen, &off, cbuf)) > 0)
	{
		Size		need = MAXALIGN(csize) + sizeof(ItemIdData);

		if (used > 0 && used + need > (Size) LION_PAGE_CAPACITY)
		{
			n++;
			used = 0;
		}
		used += need;
	}

	return n;
}

/*
 * The most leaves a spill can need.  An INLINE payload is at most
 * LION_MAX_ENTRY_SIZE bytes and a leaf holds LION_PAGE_CAPACITY, so two is
 * already unreachable; the limit exists because DESIGN.md §25 requires every
 * page to be ALLOCATED before the first record opens (an rmgr-mode record runs
 * in a critical section), and an unbounded number of pinned buffers is not
 * something to discover at run time.
 */
#define LION_SPILL_MAX_LEAVES	4

void
lion_entry_spill(Relation index, Relation heaprel, Buffer entrybuf,
				OffsetNumber entryoff, LionEntryTuple *entry,
				const char *payload, Size paylen)
{
	LionWalState *xstate;
	Buffer		headbuf;
	Page		headpage;
	Buffer		leafbuf[LION_SPILL_MAX_LEAVES];
	BlockNumber leafblk[LION_SPILL_MAX_LEAVES];
	LionContainer *cbuf;
	LionPostingPivot pivots[LION_SPILL_MAX_LEAVES];
	BlockNumber head;
	Size		off = 0;
	Size		csize;
	int			nleaves;
	int			cur;
	bool		multi;
	int			i;

	cbuf = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	nleaves = lion_spill_count_leaves(payload, paylen, cbuf);
	multi = (nleaves > 1);

	if (nleaves > LION_SPILL_MAX_LEAVES)
		elog(ERROR, "lion index \"%s\": an inline payload of %zu bytes needs %d container pages",
			 RelationGetRelationName(index), paylen, nleaves);

	/*
	 * Every page this spill needs is taken HERE, before any record opens
	 * (DESIGN.md §25).  The ROOT of a posting set is the one page this index
	 * never recycles, so that root blocks come only from extending the
	 * relation and no two sets can ever share one - which is what makes
	 * owner_head an identity a reader can trust with nothing else in hand,
	 * exactly the situation the count cursor is in (DESIGN.md §18).
	 */
	headbuf = lion_alloc_page(index, heaprel, false);
	head = BufferGetBlockNumber(headbuf);

	for (i = 0; i < (multi ? nleaves : 0); i++)
	{
		leafbuf[i] = lion_alloc_page(index, heaprel, true);
		leafblk[i] = BufferGetBlockNumber(leafbuf[i]);
	}

	if (!multi)
	{
		/*
		 * One leaf, which IS the head: the whole spill is one record holding
		 * the new page and the entry that starts pointing at it.
		 */
		xstate = lion_wal_begin(index);
		headpage = lion_wal_init_buffer(xstate, headbuf, LION_PAGE_CONTAINER);
		lion_page_set_owner(headpage, entry->hash, head);

		while ((csize = lion_inline_fetch(payload, paylen, &off, cbuf)) > 0)
		{
			OffsetNumber noff = PageAddItemExtended(headpage, cbuf, csize,
													InvalidOffsetNumber, 0);

			if (noff == InvalidOffsetNumber)
				elog(ERROR, "lion index: failed to spill container to page %u",
					 head);
			lion_wal_op(xstate, headpage, LION_OP_ADD, noff, 0, cbuf, csize);
		}
		lion_page_update_minmax(headpage);
		lion_wal_log_special(xstate, headpage);

		entry->flags = (entry->flags & LION_ENTRY_RESERVED) | LION_ENTRY_CHAIN;
		entry->head = head;
		entry->tail = head;
		lion_put_entry(index, xstate, entrybuf, entryoff, entry);
		lion_wal_finish(xstate, LION_XLOG_ITEM_ADD);

		UnlockReleaseBuffer(headbuf);
		pfree(cbuf);
		return;
	}

	/*
	 * Several leaves.  Each one is filled and linked to its successor in a
	 * record of its own - the block numbers are all known, because the pages
	 * were allocated above - and the ROOT that names them goes in with the
	 * entry, last.  A crash before that last record leaks the leaves, which
	 * is what §18 already says about a crash in the middle of a whole-chain
	 * free: they are unreferenced, the entry still describes the INLINE
	 * payload it always did, and the next VACUUM's sweep collects them.
	 */
	cur = 0;
	xstate = lion_wal_begin(index);
	{
		Page		curpage = lion_wal_init_buffer(xstate, leafbuf[0],
												   LION_PAGE_CONTAINER);

		lion_page_set_owner(curpage, entry->hash, head);

		while ((csize = lion_inline_fetch(payload, paylen, &off, cbuf)) > 0)
		{
			OffsetNumber noff;

			if (PageGetFreeSpace(curpage) < MAXALIGN(csize))
			{
				Assert(cur + 1 < nleaves);
				lion_page_update_minmax(curpage);
				LionPageGetOpaque(curpage)->rightlink = leafblk[cur + 1];
				pivots[cur].ckey = LionPageGetOpaque(curpage)->minckey;
				pivots[cur].child = leafblk[cur];
				lion_wal_log_special(xstate, curpage);
				lion_wal_finish(xstate, LION_XLOG_ITEM_ADD);
				UnlockReleaseBuffer(leafbuf[cur]);

				cur++;
				xstate = lion_wal_begin(index);
				curpage = lion_wal_init_buffer(xstate, leafbuf[cur],
											   LION_PAGE_CONTAINER);
				lion_page_set_owner(curpage, entry->hash, head);
			}

			noff = PageAddItemExtended(curpage, cbuf, csize,
									   InvalidOffsetNumber, 0);
			if (noff == InvalidOffsetNumber)
				elog(ERROR, "lion index: failed to spill container to page %u",
					 leafblk[cur]);
			lion_wal_op(xstate, curpage, LION_OP_ADD, noff, 0, cbuf, csize);
		}

		lion_page_update_minmax(curpage);
		pivots[cur].ckey = LionPageGetOpaque(curpage)->minckey;
		pivots[cur].child = leafblk[cur];
		lion_wal_log_special(xstate, curpage);
		lion_wal_finish(xstate, LION_XLOG_ITEM_ADD);
		UnlockReleaseBuffer(leafbuf[cur]);
	}

	/*
	 * A leaf the packing did not need after all goes straight back: nothing
	 * was written to it, so there is nothing to log and nothing to undo.
	 * lion_spill_count_leaves() agrees with the loop above by construction,
	 * so this is the belt to its braces - and it is the shape DESIGN.md §25
	 * requires, since a page cannot be allocated inside the record any more.
	 */
	for (i = cur + 1; i < nleaves; i++)
		lion_release_unused_page(index, leafbuf[i]);
	nleaves = cur + 1;

	/*
	 * Only the INLINE/CHAIN half of the flags changes: a reserved entry
	 * (NULL-key, §14, or empty-key, §17) stays the reserved entry it was once
	 * its payload moves to a chain.  Losing a reserved bit here would leave a
	 * key-less entry that lion_find_reserved_entry() no longer finds and that
	 * every other reader takes for an ordinary entry with a zero-length key,
	 * so the next row of that kind would start a second entry.
	 */
	entry->flags = (entry->flags & LION_ENTRY_RESERVED) | LION_ENTRY_CHAIN;
	entry->head = head;
	entry->tail = leafblk[nleaves - 1];

	/* The root's downlinks and the entry, in one last record. */
	xstate = lion_wal_begin(index);
	headpage = lion_wal_init_buffer(xstate, headbuf, LION_PAGE_CONTAINER);
	LionPageGetOpaque(headpage)->level = 1;
	lion_page_set_owner(headpage, entry->hash, head);
	pivots[0].ckey = 0;			/* the leftmost downlink is minus infinity */
	for (i = 0; i < nleaves; i++)
	{
		OffsetNumber noff = PageAddItemExtended(headpage, (char *) &pivots[i],
												LION_POSTING_PIVOT_SIZE,
												InvalidOffsetNumber, 0);

		if (noff == InvalidOffsetNumber)
			elog(ERROR, "lion index: failed to build the root of a spilled posting set");
		lion_wal_op(xstate, headpage, LION_OP_ADD, noff, 0, &pivots[i],
					LION_POSTING_PIVOT_SIZE);
	}
	lion_wal_log_special(xstate, headpage);
	lion_put_entry(index, xstate, entrybuf, entryoff, entry);
	lion_wal_finish(xstate, LION_XLOG_ITEM_ADD);
	UnlockReleaseBuffer(headbuf);

	pfree(cbuf);
}

/*
 * Split leaf page P (buf) and place the caller's items, all in one WAL record.
 *
 * Items at offsets off..maxoff (excluding the stale item at off when replace
 * is true, which is dropped) move to a brand new page N linked immediately
 * after P.  If the new items still do not fit on P, a second new page M
 * holding them is linked between P and N.  This always makes progress because
 * the caller's items together fit on an empty page (they are at most
 * LION_MAX_PUT_ITEMS items of at most LION_CONTAINER_MAX_SIZE bytes, and a
 * segment split only ever adds a few bytes to what was one item).  Items
 * never move left and never move to an existing page.
 *
 * DESIGN.md §22 adds the tree above them.  The new page - or, when there are
 * two, each of them - has no downlink in the parent when the record lands, so
 * the page to its LEFT is flagged LION_PAGE_INCOMPLETE_SPLIT and is held
 * EXCLUSIVE until its downlink has been inserted and the flag cleared.  A
 * crash in between costs nothing but the next write descent's repair.
 *
 * The ROOT is split by pushing it down instead, so that the root block - the
 * entry's `head`, and the owner stamp of every page of the set - never moves.
 */
static void
lion_split_and_place(Relation index, Relation heaprel, Buffer buf,
					OffsetNumber off, bool replace,
					Buffer entrybuf, OffsetNumber entryoff,
					LionEntryTuple *entry, LionContainer **items, int nitems)
{
	Page		page = BufferGetPage(buf);
	BlockNumber blk = BufferGetBlockNumber(buf);
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	OffsetNumber firstright = replace ? OffsetNumberNext(off) : off;
	OffsetNumber delfirst = off;
	int			ndel = (int) (maxoff + 1 - delfirst);
	int			nmove = (int) (maxoff + 1 - firstright);
	Size		sizes[LION_MAX_PUT_ITEMS];
	Size		need = 0;
	char	   *movebuf = NULL;
	Size	   *movelen = NULL;
	char	  **moveptr = NULL;
	OffsetNumber *delofs = NULL;
	LionWalState *xstate;
	Page		pP;
	Page		pN = NULL;
	Page		pM = NULL;
	Buffer		nbuf = InvalidBuffer;
	Buffer		mbuf = InvalidBuffer;
	BlockNumber oldright = LionPageGetOpaque(page)->rightlink;
	BlockNumber nblk = InvalidBlockNumber;
	BlockNumber mblk = InvalidBlockNumber;
	uint32		firstmoved = 0;	/* first ckey of the items that move right */
	PGAlignedBlock *trial = NULL;
	bool		needm;
	int			i;

	Assert(ndel >= 0 && nmove >= 0 && nmove <= ndel);
	Assert(nitems >= 1 && nitems <= LION_MAX_PUT_ITEMS);
	Assert(LionPageIsPostingLeaf(page));
	Assert(!LionPageIncompleteSplit(page));

	if (blk == entry->head)
	{
		/*
		 * The whole set is this one page, so there is no parent to take a
		 * downlink.  Push the root down - its items go to a new child, the
		 * root block becomes the level above - and place the items on the
		 * child, which is a verbatim copy and therefore wants them at the
		 * very same offset.
		 *
		 * The root's lock is dropped while that happens, because the child's
		 * own split will have to take it to insert ITS downlink and buffer
		 * locks are not reentrant.  Writers of one key serialise on the
		 * entry's directory leaf (DESIGN.md §22), so nothing else can be in
		 * this tree; a reader that looks in between sees the push-down, which
		 * is already on disk, and descends.  The caller gets its buffer back
		 * locked as it handed it over - with an EXCLUSIVE lock, which is what
		 * a cleanup lock decays to here: the page it holds is the new ROOT,
		 * an internal page that holds no TIDs at all, and the TIDs this call
		 * writes go to a page nobody can have pinned because it did not exist
		 * a moment ago (DESIGN.md §11).
		 */
		BlockNumber cblk;
		Buffer		cbuf;

		lion_posting_root_pushdown(index, heaprel, buf, entrybuf, entryoff,
								   entry, &cblk);

		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		cbuf = ReadBuffer(index, cblk);
		LockBuffer(cbuf, BUFFER_LOCK_EXCLUSIVE);
		if (!lion_page_owns_entry(BufferGetPage(cbuf), entry->hash, entry->head))
			elog(ERROR, "lion index: block %u is not a page of the posting set at %u",
				 cblk, entry->head);

		lion_chain_put_items_locked_ext(index, heaprel, cbuf, entrybuf,
										entryoff, entry, off, replace, items,
										nitems, false);

		UnlockReleaseBuffer(cbuf);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		return;
	}

	for (i = 0; i < nitems; i++)
	{
		sizes[i] = lion_item_size(items[i]);
		need += MAXALIGN(sizes[i]) + sizeof(ItemIdData);
	}

	/* Copy out the items that are going to move, before touching the page. */
	if (nmove > 0)
	{
		Size		used = 0;

		movebuf = (char *) palloc(BLCKSZ);
		movelen = (Size *) palloc(sizeof(Size) * nmove);
		moveptr = (char **) palloc(sizeof(char *) * nmove);

		for (i = 0; i < nmove; i++)
		{
			ItemId		iid = PageGetItemId(page, firstright + i);
			Size		sz = ItemIdGetLength(iid);

			Assert(used + MAXALIGN(sz) <= BLCKSZ);
			memcpy(movebuf + used, PageGetItem(page, iid), sz);
			moveptr[i] = movebuf + used;
			movelen[i] = sz;
			used += MAXALIGN(sz);
		}

		/* the separator of the page those items go to */
		firstmoved = lion_item_first_ckey((const LionContainer *) moveptr[0]);
	}

	if (ndel > 0)
	{
		delofs = (OffsetNumber *) palloc(sizeof(OffsetNumber) * ndel);
		for (i = 0; i < ndel; i++)
			delofs[i] = delfirst + i;
	}

	/*
	 * Does P still hold the new items once the moved ones are gone?
	 *
	 * The question used to be asked of the page itself, in the middle of the
	 * record, and the second new page was allocated there if the answer was
	 * no.  DESIGN.md §25 does not allow that any more - an rmgr-mode record
	 * runs in a critical section, where extending the relation would turn a
	 * full disk into a PANIC - so it is asked HERE, of a private copy that
	 * the deletion is rehearsed on.  A copy rather than arithmetic because
	 * PageIndexMultiDelete() compacts, and what a compaction recovers is
	 * exactly what the arithmetic would have to guess at.  A split copies a
	 * page once; the hot path (an in-place member insert) copies nothing at
	 * all, which is the whole point of the section.
	 */
	trial = (PGAlignedBlock *) palloc(sizeof(PGAlignedBlock));
	memcpy(trial->data, page, BLCKSZ);
	if (ndel > 0)
		PageIndexMultiDelete((Page) trial->data, delofs, ndel);
	needm = PageGetExactFreeSpace((Page) trial->data) < need;

	/* Every page this record needs, taken before the record opens (§25). */
	if (nmove > 0)
	{
		nbuf = lion_alloc_page(index, heaprel, true);
		nblk = BufferGetBlockNumber(nbuf);
	}
	if (needm)
	{
		mbuf = lion_alloc_page(index, heaprel, true);
		mblk = BufferGetBlockNumber(mbuf);
	}

	xstate = lion_wal_begin(index);
	pP = lion_wal_register_buffer(xstate, buf, LION_WALBUF_STD);

	if (BufferIsValid(nbuf))
	{
		pN = lion_wal_init_buffer(xstate, nbuf, LION_PAGE_CONTAINER);
		lion_page_set_owner(pN, entry->hash, entry->head);
	}
	if (BufferIsValid(mbuf))
	{
		pM = lion_wal_init_buffer(xstate, mbuf, LION_PAGE_CONTAINER);
		lion_page_set_owner(pM, entry->hash, entry->head);
	}

	if (ndel > 0)
	{
		PageIndexMultiDelete(pP, delofs, ndel);
		lion_wal_op(xstate, pP, LION_OP_MULTIDEL, 0, (uint16) ndel, delofs,
					sizeof(OffsetNumber) * ndel);
	}

	if (!needm)
	{
		Assert(PageGetExactFreeSpace(pP) >= need);
		for (i = 0; i < nitems; i++)
		{
			OffsetNumber noff = PageAddItemExtended(pP, items[i], sizes[i],
													InvalidOffsetNumber, 0);

			if (noff == InvalidOffsetNumber)
				elog(ERROR, "lion index: failed to place item after split");
			lion_wal_op(xstate, pP, LION_OP_ADD, noff, 0, items[i], sizes[i]);
		}
	}
	else
	{
		/* The items get a page of their own, linked immediately after P. */
		for (i = 0; i < nitems; i++)
		{
			OffsetNumber noff = PageAddItemExtended(pM, items[i], sizes[i],
													InvalidOffsetNumber, 0);

			if (noff == InvalidOffsetNumber)
				elog(ERROR, "lion index: failed to place item on new page");
			lion_wal_op(xstate, pM, LION_OP_ADD, noff, 0, items[i], sizes[i]);
		}
	}

	if (nmove > 0)
	{
		for (i = 0; i < nmove; i++)
		{
			OffsetNumber noff = PageAddItemExtended(pN, moveptr[i], movelen[i],
													InvalidOffsetNumber, 0);

			if (noff == InvalidOffsetNumber)
				elog(ERROR, "lion index: failed to move container during split");
			lion_wal_op(xstate, pN, LION_OP_ADD, noff, 0, moveptr[i],
						movelen[i]);
		}
	}

	lion_page_update_minmax(pP);
	if (pN != NULL)
		lion_page_update_minmax(pN);
	if (pM != NULL)
		lion_page_update_minmax(pM);

	/*
	 * Relink: P -> [M] -> [N] -> oldright.  Each brand new page is one the
	 * parent has no downlink for yet, so the page to its LEFT is flagged and
	 * stays EXCLUSIVE until that downlink is in (DESIGN.md §22).
	 */
	{
		BlockNumber after_m = BlockNumberIsValid(nblk) ? nblk : oldright;

		if (BlockNumberIsValid(mblk))
		{
			LionPageGetOpaque(pM)->rightlink = after_m;
			LionPageGetOpaque(pP)->rightlink = mblk;
			LionPageGetOpaque(pP)->flags |= LION_PAGE_INCOMPLETE_SPLIT;
			if (BlockNumberIsValid(nblk))
				LionPageGetOpaque(pM)->flags |= LION_PAGE_INCOMPLETE_SPLIT;
		}
		else
		{
			LionPageGetOpaque(pP)->rightlink = after_m;
			if (BlockNumberIsValid(nblk))
				LionPageGetOpaque(pP)->flags |= LION_PAGE_INCOMPLETE_SPLIT;
		}

		if (BlockNumberIsValid(nblk))
			LionPageGetOpaque(pN)->rightlink = oldright;
	}

	lion_wal_log_special(xstate, pP);
	if (pN != NULL)
		lion_wal_log_special(xstate, pN);
	if (pM != NULL)
		lion_wal_log_special(xstate, pM);

	/* If P was the tail, the chain has a new last page. */
	if (entry->tail == blk)
	{
		BlockNumber newtail = BlockNumberIsValid(nblk) ? nblk : mblk;

		Assert(!BlockNumberIsValid(oldright));
		if (BlockNumberIsValid(newtail))
			entry->tail = newtail;
	}

	/* The entry always travels with the container change. */
	lion_put_entry(index, xstate, entrybuf, entryoff, entry);

	lion_wal_finish(xstate, LION_XLOG_SPLIT);

	if (BufferIsValid(nbuf))
		UnlockReleaseBuffer(nbuf);

	if (movebuf)
	{
		pfree(movebuf);
		pfree(movelen);
		pfree(moveptr);
	}
	if (delofs)
		pfree(delofs);
	pfree(trial);

	/*
	 * Test hook: the split is on disk and the left page says so, but its right
	 * sibling has no downlink yet.  test/recovery/run.sh crashes the server
	 * here and proves that the next writer's descent repairs it.  Compiles to
	 * nothing without --enable-injection-points.
	 */
	if (LionPageIncompleteSplit(BufferGetPage(buf)))
		INJECTION_POINT("lion-posting-split-incomplete", NULL);

	/*
	 * The downlinks, left to right: M's first, because a descent cannot reach
	 * M before M has one and therefore cannot repair M before P.  Each
	 * separator is the first container key the split put on the sibling, and
	 * is handed over rather than read back off the page, because M is still
	 * held EXCLUSIVE here.
	 */
	if (LionPageIncompleteSplit(BufferGetPage(buf)))
	{
		uint32		sep = BlockNumberIsValid(mblk) ?
			lion_item_first_ckey(items[0]) : firstmoved;

		lion_posting_finish_split_sep(index, heaprel, entry->hash, entry->head,
									  buf, &sep);
	}

	if (BufferIsValid(mbuf))
	{
		if (LionPageIncompleteSplit(BufferGetPage(mbuf)))
			lion_posting_finish_split_sep(index, heaprel, entry->hash,
										  entry->head, mbuf, &firstmoved);
		UnlockReleaseBuffer(mbuf);
	}
}

/* ---------------------------------------------------------------------
 * Cardinality guard (DESIGN.md §17)
 *
 * The `max_entries` reloption is advisory: an index that grows past it keeps
 * working and keeps accepting rows, but says so once.  It exists because a
 * multi-key opclass makes it easy to index a column with millions of distinct
 * keys by accident (a tsvector of a whole document collection, an array of
 * UUIDs), and an inverted index with one entry per row is a slow way of
 * storing a table.
 * --------------------------------------------------------------------- */

/* Indexes this backend has already complained about, keyed by relation Oid. */
static HTAB *lion_warned_indexes = NULL;

int
lion_max_entries(Relation index)
{
	LionOptions *opts = (LionOptions *) index->rd_options;

	return opts ? opts->max_entries : LION_DEFAULT_MAX_ENTRIES;
}

void
lion_warn_max_entries(Relation index, int64 nentries)
{
	Oid			relid = RelationGetRelid(index);
	bool		found;

	if (lion_warned_indexes == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(Oid);
		ctl.hcxt = TopMemoryContext;
		lion_warned_indexes = hash_create("lion index max_entries warnings",
										 16, &ctl,
										 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	(void) hash_search(lion_warned_indexes, &relid, HASH_ENTER, &found);
	if (found)
		return;					/* this backend has said it once already */

	ereport(WARNING,
			(errmsg("lion index \"%s\" has more than %d distinct keys",
					RelationGetRelationName(index), lion_max_entries(index)),
			 errdetail("The index holds about " INT64_FORMAT " entries; its max_entries option is %d.",
					   nentries, lion_max_entries(index)),
			 errhint("Raise max_entries, or index a column with fewer distinct keys.")));
}
