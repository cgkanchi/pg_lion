/*-------------------------------------------------------------------------
 *
 * rbi_funcs.c
 *		SQL-callable helpers for the roaring index (DESIGN.md section 7):
 *		roaring_index_stats() and roaring_index_verify().
 *
 * Both open the index with AccessShareLock and read one page at a time under
 * a SHARE lock, so they run concurrently with inserts; roaring_index_verify()
 * additionally holds the SHARE lock of a bucket head page for as long as it
 * is checking that bucket, which pins down the whole bucket (every reader and
 * writer of a key enters through its bucket head) and gives it a stable view.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "varatt.h"

#include "rbi.h"

PG_FUNCTION_INFO_V1(roaring_index_stats);
PG_FUNCTION_INFO_V1(roaring_index_verify);

#define RBI_STATS_NCOLS		15

typedef struct RBIVerifyState
{
	Relation	index;
	Relation	heap;
	RBIState   *state;
	BlockNumber nblocks;
	uint8	   *refs;			/* how often each block is referenced */
	RBIContainer *cbuf;			/* aligned container work buffer */
	int64		nnullentries;	/* reserved NULL-key entries seen (at most 1) */
	Oid			keyoutfunc;		/* output function of the indexed type */
	MemoryContext heapcxt;		/* per heap tuple, heapallindexed only */
	int64		nheaptuples;
} RBIVerifyState;

/*
 * Report structural damage.  Every message names the block (and item) the
 * problem was found in, as the caller has no other way of locating it.
 */
#define rbi_corrupt(...) \
	ereport(ERROR, \
			(errcode(ERRCODE_INDEX_CORRUPTED), \
			 errmsg(__VA_ARGS__)))

/*
 * Open relid as a roaring index.
 */
static Relation
rbi_open_index(Oid relid, LOCKMODE lockmode)
{
	Relation	index = index_open(relid, lockmode);

	if (index->rd_rel->relkind != RELKIND_INDEX ||
		index->rd_indam == NULL ||
		index->rd_indam->ambuild != rbibuild)
	{
		char	   *name = pstrdup(RelationGetRelationName(index));

		index_close(index, lockmode);
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a roaring index", name)));
	}

	if (RELATION_IS_OTHER_TEMP(index))
	{
		char	   *name = pstrdup(RelationGetRelationName(index));

		index_close(index, lockmode);
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access temporary index \"%s\" of another session",
						name)));
	}

	return index;
}

/* ---------------------------------------------------------------------
 * roaring_index_stats()
 * --------------------------------------------------------------------- */

typedef struct RBIStats
{
	int64		bucket_pages;
	int64		entries;
	int64		inline_entries;
	int64		container_pages;
	int64		containers;
	int64		by_type[4];		/* indexed by RBIContainerType */
	int64		sparse_segments;	/* items of type RBI_CT_SPARSE */
	int64		sparse_members;	/* (ckey, lo) pairs inside them */
	int64		ntids;
	int64		null_tids;		/* members of the reserved NULL entry (§14) */
	int64		container_bytes;
	int64		free_bytes;
} RBIStats;

/*
 * Account for one item of a posting set.  The container counters count real
 * containers only; a sparse segment (DESIGN.md §13) is reported by
 * sparse_segments/sparse_members instead.  container_bytes is the bytes of
 * every item, whatever its kind.
 */
static void
rbi_stats_item(RBIStats *st, const RBIContainer *c, Size csize)
{
	if (c->type == RBI_CT_SPARSE)
	{
		st->sparse_segments++;
		st->sparse_members += (int64) c->cardinality;
	}
	else
	{
		st->containers++;
		if (c->type >= RBI_CT_ARRAY && c->type <= RBI_CT_RUN)
			st->by_type[c->type]++;
	}
	st->container_bytes += (int64) csize;
}

Datum
roaring_index_stats(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	index;
	RBIState   *state;
	RBIStats	st;
	RBIContainer *cbuf;
	BlockNumber nblocks;
	BlockNumber blk;
	TupleDesc	tupdesc;
	Datum		values[RBI_STATS_NCOLS];
	bool		nulls[RBI_STATS_NCOLS];
	HeapTuple	tuple;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	index = rbi_open_index(relid, AccessShareLock);
	state = rbi_get_state(index);

	memset(&st, 0, sizeof(st));
	cbuf = (RBIContainer *) palloc(RBI_CONTAINER_MAX_SIZE);

	nblocks = RelationGetNumberOfBlocks(index);
	for (blk = 1; blk < nblocks; blk++)
	{
		Buffer		buf;
		Page		page;
		OffsetNumber maxoff;
		OffsetNumber off;

		buf = ReadBuffer(index, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);

		if (PageIsNew(page) || PageGetSpecialSize(page) != RBI_SPECIAL_SIZE)
		{
			UnlockReleaseBuffer(buf);
			continue;
		}

		maxoff = PageGetMaxOffsetNumber(page);

		if (RBIPageIsBucket(page))
		{
			st.bucket_pages++;
			st.free_bytes += (int64) PageGetFreeSpace(page);

			for (off = FirstOffsetNumber; off <= maxoff; off++)
			{
				ItemId		iid = PageGetItemId(page, off);
				RBIEntryTuple *entry;

				if (!ItemIdIsUsed(iid))
					continue;

				entry = (RBIEntryTuple *) PageGetItem(page, iid);
				st.entries++;
				st.ntids += (int64) entry->ntids;
				if (RBIEntryIsNullKey(entry))
					st.null_tids += (int64) entry->ntids;

				if ((entry->flags & RBI_ENTRY_INLINE) != 0)
				{
					Size		paylen = RBI_ENTRY_PAYLOAD_LEN(entry,
															   ItemIdGetLength(iid));
					Size		cur = 0;
					Size		csize;

					st.inline_entries++;
					while ((csize = rbi_inline_fetch(RBIEntryGetPayload(entry),
													 paylen, &cur, cbuf)) > 0)
						rbi_stats_item(&st, cbuf, csize);
				}
			}
		}
		else if (RBIPageIsContainer(page))
		{
			st.container_pages++;
			st.free_bytes += (int64) PageGetFreeSpace(page);

			for (off = FirstOffsetNumber; off <= maxoff; off++)
			{
				ItemId		iid = PageGetItemId(page, off);

				if (!ItemIdIsUsed(iid))
					continue;

				rbi_stats_item(&st,
							   (RBIContainer *) PageGetItem(page, iid),
							   ItemIdGetLength(iid));
			}
		}

		UnlockReleaseBuffer(buf);
		CHECK_FOR_INTERRUPTS();
	}

	pfree(cbuf);

	memset(nulls, 0, sizeof(nulls));
	values[0] = Int32GetDatum((int32) state->meta.nbuckets);
	values[1] = Int64GetDatum(st.bucket_pages);
	values[2] = Int64GetDatum(st.entries);
	values[3] = Int64GetDatum(st.inline_entries);
	values[4] = Int64GetDatum(st.container_pages);
	values[5] = Int64GetDatum(st.containers);
	values[6] = Int64GetDatum(st.by_type[RBI_CT_ARRAY]);
	values[7] = Int64GetDatum(st.by_type[RBI_CT_BITSET]);
	values[8] = Int64GetDatum(st.by_type[RBI_CT_RUN]);
	values[9] = Int64GetDatum(st.ntids);
	values[10] = Int64GetDatum(st.container_bytes);
	values[11] = Int64GetDatum(st.free_bytes);
	values[12] = Int64GetDatum(st.sparse_segments);
	values[13] = Int64GetDatum(st.sparse_members);
	values[14] = Int64GetDatum(st.null_tids);

	tuple = heap_form_tuple(tupdesc, values, nulls);

	index_close(index, AccessShareLock);

	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/* ---------------------------------------------------------------------
 * roaring_index_verify()
 * --------------------------------------------------------------------- */

/*
 * Record that blk is referenced by something, and refuse to look at it twice
 * (which also stops a corrupt rightlink cycle from looping forever).
 */
static void
rbi_verify_visit(RBIVerifyState *vs, BlockNumber blk, const char *what)
{
	if (blk >= vs->nblocks)
		rbi_corrupt("roaring index \"%s\": %s points at block %u, but the index has only %u blocks",
					RelationGetRelationName(vs->index), what, blk, vs->nblocks);

	if (vs->refs[blk] != 0)
		rbi_corrupt("roaring index \"%s\": block %u is referenced more than once (reached again as %s)",
					RelationGetRelationName(vs->index), blk, what);

	vs->refs[blk]++;
}

/*
 * Read blk with a SHARE lock and check its page header and kind.
 */
static Page
rbi_verify_read_page(RBIVerifyState *vs, BlockNumber blk, uint16 kind,
					 Buffer *bufp)
{
	Buffer		buf;
	Page		page;
	RBIPageOpaque opaque;
	uint16		flags;

	buf = ReadBuffer(vs->index, blk);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	*bufp = buf;

	if (PageIsNew(page))
		rbi_corrupt("roaring index \"%s\": block %u has never been initialised",
					RelationGetRelationName(vs->index), blk);

	if (PageGetSpecialSize(page) != RBI_SPECIAL_SIZE)
		rbi_corrupt("roaring index \"%s\": block %u has a special area of %u bytes, expected %zu",
					RelationGetRelationName(vs->index), blk,
					(unsigned) PageGetSpecialSize(page),
					(Size) RBI_SPECIAL_SIZE);

	opaque = RBIPageGetOpaque(page);

	if (opaque->page_id != RBI_PAGE_ID)
		rbi_corrupt("roaring index \"%s\": block %u has page id 0x%04X, expected 0x%04X",
					RelationGetRelationName(vs->index), blk,
					opaque->page_id, RBI_PAGE_ID);

	flags = opaque->flags & (RBI_PAGE_META | RBI_PAGE_BUCKET | RBI_PAGE_CONTAINER);
	if (flags != RBI_PAGE_META && flags != RBI_PAGE_BUCKET &&
		flags != RBI_PAGE_CONTAINER)
		rbi_corrupt("roaring index \"%s\": block %u has flags 0x%04X, expected exactly one page kind",
					RelationGetRelationName(vs->index), blk, opaque->flags);

	if (flags != kind)
		rbi_corrupt("roaring index \"%s\": block %u is a %s page, expected a %s page",
					RelationGetRelationName(vs->index), blk,
					flags == RBI_PAGE_META ? "meta" :
					flags == RBI_PAGE_BUCKET ? "bucket" : "container",
					kind == RBI_PAGE_META ? "meta" :
					kind == RBI_PAGE_BUCKET ? "bucket" : "container");

	return page;
}

/*
 * Check that the stored key of an entry has the length its type calls for,
 * so that hashing it cannot run off the end of the item.
 */
static void
rbi_verify_keylen(RBIVerifyState *vs, BlockNumber blk, OffsetNumber off,
				  const RBIEntryTuple *entry)
{
	RBIState   *state = vs->state;
	Size		keylen = entry->keylen;
	const char *key = RBIEntryGetKey(entry);

	if (keylen == 0 || keylen > RBI_MAX_KEY_SIZE)
		rbi_corrupt("roaring index \"%s\": entry %u on block %u has key length %zu, expected 1 .. %d",
					RelationGetRelationName(vs->index), off, blk, keylen,
					RBI_MAX_KEY_SIZE);

	if (state->typbyval)
	{
		if (keylen != sizeof(Datum))
			rbi_corrupt("roaring index \"%s\": entry %u on block %u has key length %zu, expected %zu for a by-value type",
						RelationGetRelationName(vs->index), off, blk, keylen,
						sizeof(Datum));
	}
	else if (state->typlen > 0)
	{
		if (keylen != (Size) state->typlen)
			rbi_corrupt("roaring index \"%s\": entry %u on block %u has key length %zu, expected %d",
						RelationGetRelationName(vs->index), off, blk, keylen,
						state->typlen);
	}
	else if (state->typlen == -1)
	{
		if (VARATT_IS_EXTERNAL(key) || VARATT_IS_COMPRESSED(key) ||
			VARSIZE_ANY(key) != keylen)
			rbi_corrupt("roaring index \"%s\": entry %u on block %u has a malformed varlena key of %zu bytes",
						RelationGetRelationName(vs->index), off, blk, keylen);
	}
	else
	{
		if (strnlen(key, keylen) != keylen - 1)
			rbi_corrupt("roaring index \"%s\": entry %u on block %u has a malformed cstring key of %zu bytes",
						RelationGetRelationName(vs->index), off, blk, keylen);
	}
}

/*
 * Check one sparse segment (DESIGN.md §13).
 *
 * Besides its own structure, a segment has to respect the two rules that
 * make the rest of the index able to ignore it: its range must start above
 * everything before it (the caller keeps the last ckey of the previous item
 * in *prevckey, so this also proves the ranges do not overlap or interleave,
 * within a page and across a chain), and no container key inside it may have
 * reached RBI_SPARSE_THRESHOLD members, because such a key belongs in a
 * container of its own.
 */
static void
rbi_verify_segment(RBIVerifyState *vs, BlockNumber blk, OffsetNumber off,
				   const RBIContainer *c, Size avail, bool *haveprev,
				   uint32 *prevckey)
{
	const char *detail = NULL;
	const uint32 *ckeys;
	uint32		n = c->cardinality;
	uint32		run = 1;
	uint32		i;

	if (!rbi_sparse_check(c, avail, &detail))
		rbi_corrupt("roaring index \"%s\": sparse segment %u on block %u is corrupt: %s",
					RelationGetRelationName(vs->index), off, blk, detail);

	if (rbi_sparse_size(c) != avail)
		rbi_corrupt("roaring index \"%s\": sparse segment %u on block %u occupies %zu bytes but needs %zu",
					RelationGetRelationName(vs->index), off, blk, avail,
					rbi_sparse_size(c));

	if (n == 0)
		rbi_corrupt("roaring index \"%s\": sparse segment %u on block %u is empty",
					RelationGetRelationName(vs->index), off, blk);

	if (*haveprev && c->ckey <= *prevckey)
		rbi_corrupt("roaring index \"%s\": sparse segment %u on block %u starts at container key %u, not above the previous key %u",
					RelationGetRelationName(vs->index), off, blk, c->ckey,
					*prevckey);

	ckeys = RBI_SPARSE_CKEYS_CONST(c);
	for (i = 1; i <= n; i++)
	{
		if (i < n && ckeys[i] == ckeys[i - 1])
		{
			run++;
			continue;
		}
		if (run >= RBI_SPARSE_THRESHOLD)
			rbi_corrupt("roaring index \"%s\": sparse segment %u on block %u holds %u members of container key %u, which needs a container of its own",
						RelationGetRelationName(vs->index), off, blk, run,
						ckeys[i - 1]);
		run = 1;
	}

	*prevckey = rbi_item_last_ckey(c);
	*haveprev = true;
}

/*
 * Check one item that is on a container page or inside an INLINE payload:
 * a container, or a sparse segment.
 */
static void
rbi_verify_container(RBIVerifyState *vs, BlockNumber blk, OffsetNumber off,
					 const RBIContainer *c, Size avail, bool *haveprev,
					 uint32 *prevckey)
{
	const char *detail = NULL;

	if (c->type == RBI_CT_SPARSE)
	{
		rbi_verify_segment(vs, blk, off, c, avail, haveprev, prevckey);
		return;
	}

	if (!rbi_container_check(c, avail, &detail))
		rbi_corrupt("roaring index \"%s\": container %u on block %u is corrupt: %s",
					RelationGetRelationName(vs->index), off, blk, detail);

	if (rbi_container_size(c) != avail)
		rbi_corrupt("roaring index \"%s\": container %u on block %u occupies %zu bytes but needs %zu",
					RelationGetRelationName(vs->index), off, blk, avail,
					rbi_container_size(c));

	if (c->cardinality == 0)
		rbi_corrupt("roaring index \"%s\": container %u on block %u is empty",
					RelationGetRelationName(vs->index), off, blk);

	if (*haveprev && c->ckey <= *prevckey)
		rbi_corrupt("roaring index \"%s\": container %u on block %u has container key %u, not above the previous key %u",
					RelationGetRelationName(vs->index), off, blk, c->ckey,
					*prevckey);

	*prevckey = c->ckey;
	*haveprev = true;
}

/*
 * Walk and check the container chain of a CHAIN entry.
 */
static void
rbi_verify_chain(RBIVerifyState *vs, BlockNumber eblk, OffsetNumber eoff,
				 const RBIEntryTuple *entry)
{
	BlockNumber blk = entry->head;
	BlockNumber last = InvalidBlockNumber;
	bool		haveprev = false;
	uint32		prevckey = 0;
	uint64		card = 0;
	uint32		ncontainers = 0;

	if (!BlockNumberIsValid(entry->head) || !BlockNumberIsValid(entry->tail))
		rbi_corrupt("roaring index \"%s\": chain entry %u on block %u has head %u and tail %u",
					RelationGetRelationName(vs->index), eoff, eblk,
					entry->head, entry->tail);

	while (BlockNumberIsValid(blk))
	{
		Buffer		buf;
		Page		page;
		RBIPageOpaque opaque;
		OffsetNumber maxoff;
		OffsetNumber off;
		OffsetNumber firstused = InvalidOffsetNumber;
		OffsetNumber lastused = InvalidOffsetNumber;

		rbi_verify_visit(vs, blk, "a container chain");
		page = rbi_verify_read_page(vs, blk, RBI_PAGE_CONTAINER, &buf);
		opaque = RBIPageGetOpaque(page);
		maxoff = PageGetMaxOffsetNumber(page);

		for (off = FirstOffsetNumber; off <= maxoff; off++)
		{
			ItemId		iid = PageGetItemId(page, off);
			RBIContainer *c;

			if (!ItemIdIsUsed(iid))
				continue;

			c = (RBIContainer *) PageGetItem(page, iid);
			rbi_verify_container(vs, blk, off, c, ItemIdGetLength(iid),
								 &haveprev, &prevckey);

			card += c->cardinality;
			ncontainers++;

			if (firstused == InvalidOffsetNumber)
				firstused = off;
			lastused = off;
		}

		/* min/max in the special area must describe the items */
		if (firstused == InvalidOffsetNumber)
		{
			if (opaque->minckey != 0 || opaque->maxckey != 0)
				rbi_corrupt("roaring index \"%s\": empty container page %u has minckey %u and maxckey %u, expected 0 and 0",
							RelationGetRelationName(vs->index), blk,
							opaque->minckey, opaque->maxckey);
		}
		else
		{
			uint32		minckey =
				rbi_item_first_ckey((RBIContainer *)
									PageGetItem(page, PageGetItemId(page, firstused)));
			uint32		maxckey =
				rbi_item_last_ckey((RBIContainer *)
								   PageGetItem(page, PageGetItemId(page, lastused)));

			if (opaque->minckey != minckey || opaque->maxckey != maxckey)
				rbi_corrupt("roaring index \"%s\": container page %u has minckey %u and maxckey %u, but holds %u .. %u",
							RelationGetRelationName(vs->index), blk,
							opaque->minckey, opaque->maxckey, minckey, maxckey);
		}

		last = blk;
		blk = opaque->rightlink;
		UnlockReleaseBuffer(buf);

		CHECK_FOR_INTERRUPTS();
	}

	if (last != entry->tail)
		rbi_corrupt("roaring index \"%s\": chain entry %u on block %u ends at block %u, but its tail is block %u",
					RelationGetRelationName(vs->index), eoff, eblk, last,
					entry->tail);

	if (card != entry->ntids)
		rbi_corrupt("roaring index \"%s\": chain entry %u on block %u claims " UINT64_FORMAT " TIDs, but its containers hold " UINT64_FORMAT,
					RelationGetRelationName(vs->index), eoff, eblk,
					entry->ntids, card);

	if (ncontainers != entry->ncontainers)
		rbi_corrupt("roaring index \"%s\": chain entry %u on block %u claims %u containers, but its chain holds %u",
					RelationGetRelationName(vs->index), eoff, eblk,
					entry->ncontainers, ncontainers);
}

/*
 * Check one entry tuple.
 */
static void
rbi_verify_entry(RBIVerifyState *vs, uint32 bucket, BlockNumber blk,
				 OffsetNumber off, ItemId iid, Page page)
{
	RBIEntryTuple *entry = (RBIEntryTuple *) PageGetItem(page, iid);
	Size		itemsz = ItemIdGetLength(iid);
	uint16		kind = entry->flags & (RBI_ENTRY_INLINE | RBI_ENTRY_CHAIN);
	uint32		nbuckets = vs->state->meta.nbuckets;

	if (itemsz < RBI_ENTRY_HDRSZ)
		rbi_corrupt("roaring index \"%s\": entry %u on block %u is only %zu bytes",
					RelationGetRelationName(vs->index), off, blk, itemsz);

	if (kind != RBI_ENTRY_INLINE && kind != RBI_ENTRY_CHAIN)
		rbi_corrupt("roaring index \"%s\": entry %u on block %u has flags 0x%04X, expected exactly one of INLINE and CHAIN",
					RelationGetRelationName(vs->index), off, blk, entry->flags);

	if ((entry->flags & ~(uint16) (RBI_ENTRY_INLINE | RBI_ENTRY_CHAIN |
								   RBI_ENTRY_NULLKEY)) != 0)
		rbi_corrupt("roaring index \"%s\": entry %u on block %u has unknown flag bits in 0x%04X",
					RelationGetRelationName(vs->index), off, blk, entry->flags);

	if (RBIEntryIsNullKey(entry))
	{
		/*
		 * The reserved NULL-key entry (DESIGN.md §14): no key bytes, hash 0,
		 * bucket 0, and one per index at most - a second one would split the
		 * NULL rows between two entries that no reader looks for twice.
		 */
		if (entry->keylen != 0)
			rbi_corrupt("roaring index \"%s\": null entry %u on block %u has a key of %u bytes",
						RelationGetRelationName(vs->index), off, blk,
						entry->keylen);
		if (entry->hash != RBI_NULLKEY_HASH)
			rbi_corrupt("roaring index \"%s\": null entry %u on block %u stores hash %u, expected %d",
						RelationGetRelationName(vs->index), off, blk,
						entry->hash, RBI_NULLKEY_HASH);
		if (bucket != RBI_NULLKEY_BUCKET)
			rbi_corrupt("roaring index \"%s\": null entry %u on block %u is in bucket %u, expected bucket %d",
						RelationGetRelationName(vs->index), off, blk, bucket,
						RBI_NULLKEY_BUCKET);
		if (++vs->nnullentries > 1)
			rbi_corrupt("roaring index \"%s\": entry %u on block %u is a second null entry",
						RelationGetRelationName(vs->index), off, blk);
	}
	else
	{
		uint32		hash;

		rbi_verify_keylen(vs, blk, off, entry);

		hash = rbi_hash_key(vs->state,
							rbi_fetch_key(vs->state, RBIEntryGetKey(entry)));
		if (hash != entry->hash)
			rbi_corrupt("roaring index \"%s\": entry %u on block %u stores hash %u, but its key hashes to %u",
						RelationGetRelationName(vs->index), off, blk,
						entry->hash, hash);

		if (rbi_bucket_of(hash, nbuckets) != bucket)
			rbi_corrupt("roaring index \"%s\": entry %u on block %u belongs to bucket %u, but was found in bucket %u",
						RelationGetRelationName(vs->index), off, blk,
						rbi_bucket_of(hash, nbuckets), bucket);
	}

	if (itemsz < RBIEntryPayloadOffset(entry))
		rbi_corrupt("roaring index \"%s\": entry %u on block %u is %zu bytes, too small for its %u byte key",
					RelationGetRelationName(vs->index), off, blk, itemsz,
					entry->keylen);

	if (kind == RBI_ENTRY_INLINE)
	{
		Size		paylen = RBI_ENTRY_PAYLOAD_LEN(entry, itemsz);
		Size		cur = 0;
		Size		csize;
		bool		haveprev = false;
		uint32		prevckey = 0;
		uint64		card = 0;
		uint32		ncontainers = 0;

		if (BlockNumberIsValid(entry->head) || BlockNumberIsValid(entry->tail))
			rbi_corrupt("roaring index \"%s\": inline entry %u on block %u has head %u and tail %u, expected none",
						RelationGetRelationName(vs->index), off, blk,
						entry->head, entry->tail);

		while ((csize = rbi_inline_fetch(RBIEntryGetPayload(entry), paylen,
										 &cur, vs->cbuf)) > 0)
		{
			rbi_verify_container(vs, blk, off, vs->cbuf, csize,
								 &haveprev, &prevckey);
			card += vs->cbuf->cardinality;
			ncontainers++;
		}

		if (card != entry->ntids)
			rbi_corrupt("roaring index \"%s\": inline entry %u on block %u claims " UINT64_FORMAT " TIDs, but its payload holds " UINT64_FORMAT,
						RelationGetRelationName(vs->index), off, blk,
						entry->ntids, card);

		if (ncontainers != entry->ncontainers)
			rbi_corrupt("roaring index \"%s\": inline entry %u on block %u claims %u containers, but its payload holds %u",
						RelationGetRelationName(vs->index), off, blk,
						entry->ncontainers, ncontainers);
	}
	else
	{
		if (itemsz != RBIEntryPayloadOffset(entry))
			rbi_corrupt("roaring index \"%s\": chain entry %u on block %u is %zu bytes, expected %zu",
						RelationGetRelationName(vs->index), off, blk, itemsz,
						RBIEntryPayloadOffset(entry));

		rbi_verify_chain(vs, blk, off, entry);
	}
}

/*
 * Check one bucket: its chain of bucket pages and every entry on them.
 */
static void
rbi_verify_bucket(RBIVerifyState *vs, uint32 bucket)
{
	BlockNumber headblk = RBI_BUCKET_BLKNO(bucket);
	Buffer		headbuf;
	Buffer		curbuf;
	Page		page;
	BlockNumber blk = headblk;

	rbi_verify_visit(vs, headblk, "a bucket head");
	page = rbi_verify_read_page(vs, headblk, RBI_PAGE_BUCKET, &headbuf);
	curbuf = headbuf;

	for (;;)
	{
		OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
		OffsetNumber off;
		BlockNumber next;

		for (off = FirstOffsetNumber; off <= maxoff; off++)
		{
			ItemId		iid = PageGetItemId(page, off);

			if (!ItemIdIsUsed(iid))
				continue;

			rbi_verify_entry(vs, bucket, blk, off, iid, page);
			CHECK_FOR_INTERRUPTS();
		}

		next = RBIPageGetOpaque(page)->rightlink;
		if (!BlockNumberIsValid(next))
			break;

		rbi_verify_visit(vs, next, "a bucket chain");
		{
			Buffer		nextbuf;

			page = rbi_verify_read_page(vs, next, RBI_PAGE_BUCKET, &nextbuf);
			if (curbuf != headbuf)
				UnlockReleaseBuffer(curbuf);
			curbuf = nextbuf;
			blk = next;
		}
	}

	if (curbuf != headbuf)
		UnlockReleaseBuffer(curbuf);
	UnlockReleaseBuffer(headbuf);
}

/*
 * Check the meta page.
 */
static void
rbi_verify_meta(RBIVerifyState *vs)
{
	Buffer		buf;
	Page		page;
	RBIMetaPageData *meta;

	rbi_verify_visit(vs, RBI_METAPAGE_BLKNO, "the meta page");
	page = rbi_verify_read_page(vs, RBI_METAPAGE_BLKNO, RBI_PAGE_META, &buf);
	meta = RBIPageGetMeta(page);

	if (meta->magic != RBI_MAGIC || meta->version != RBI_VERSION)
		rbi_corrupt("roaring index \"%s\": meta page has magic %08X version %u, expected %08X version %u",
					RelationGetRelationName(vs->index), meta->magic,
					meta->version, RBI_MAGIC, RBI_VERSION);

	if (meta->offset_bits != RBI_OFFSET_BITS ||
		meta->container_bits != RBI_CONTAINER_BITS)
		rbi_corrupt("roaring index \"%s\": meta page has offset_bits %u and container_bits %u, expected %d and %d",
					RelationGetRelationName(vs->index), meta->offset_bits,
					meta->container_bits, RBI_OFFSET_BITS, RBI_CONTAINER_BITS);

	if (meta->nbuckets == 0 || meta->nbuckets > RBI_MAX_BUCKETS)
		rbi_corrupt("roaring index \"%s\": meta page has %u buckets, expected 1 .. %d",
					RelationGetRelationName(vs->index), meta->nbuckets,
					RBI_MAX_BUCKETS);

	if (meta->inline_limit < RBI_MIN_INLINE_LIMIT ||
		meta->inline_limit > RBI_MAX_INLINE_LIMIT)
		rbi_corrupt("roaring index \"%s\": meta page has inline_limit %u, expected %d .. %d",
					RelationGetRelationName(vs->index), meta->inline_limit,
					RBI_MIN_INLINE_LIMIT, RBI_MAX_INLINE_LIMIT);

	if (meta->nbuckets != vs->state->meta.nbuckets)
		rbi_corrupt("roaring index \"%s\": meta page has %u buckets, but the cached state has %u",
					RelationGetRelationName(vs->index), meta->nbuckets,
					vs->state->meta.nbuckets);

	if (vs->nblocks < 1 + meta->nbuckets)
		rbi_corrupt("roaring index \"%s\": %u blocks are too few for a meta page and %u buckets",
					RelationGetRelationName(vs->index), vs->nblocks,
					meta->nbuckets);

	if (RBIPageGetOpaque(page)->rightlink != InvalidBlockNumber)
		rbi_corrupt("roaring index \"%s\": the meta page has a right link to block %u",
					RelationGetRelationName(vs->index),
					RBIPageGetOpaque(page)->rightlink);

	UnlockReleaseBuffer(buf);
}

/*
 * Every block has to belong to the meta page, a bucket chain or exactly one
 * key's container chain.
 *
 * Two kinds of unreferenced block are tolerated (with a warning), because a
 * crash or an error can leave them behind and neither one makes the index
 * wrong: a block that was never initialised (the relation was extended and
 * the transaction did not get as far as its WAL record) and an empty
 * container page (a multi-page spill that did not reach its entry update).
 */
static void
rbi_verify_reachable(RBIVerifyState *vs)
{
	BlockNumber blk;

	for (blk = 0; blk < vs->nblocks; blk++)
	{
		Buffer		buf;
		Page		page;
		bool		leaked;

		if (vs->refs[blk] != 0)
			continue;

		buf = ReadBuffer(vs->index, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);

		leaked = PageIsNew(page) ||
			(PageGetSpecialSize(page) == RBI_SPECIAL_SIZE &&
			 RBIPageGetOpaque(page)->page_id == RBI_PAGE_ID &&
			 RBIPageIsContainer(page) &&
			 PageGetMaxOffsetNumber(page) == 0);

		UnlockReleaseBuffer(buf);

		if (!leaked)
			rbi_corrupt("roaring index \"%s\": block %u is not reachable from the meta page",
						RelationGetRelationName(vs->index), blk);

		ereport(WARNING,
				(errmsg("roaring index \"%s\": block %u is unused and unreachable",
						RelationGetRelationName(vs->index), blk),
				 errdetail("An interrupted page allocation leaks blocks; they are never reused in this version.")));
	}
}

/* ---------------------------------------------------------------------
 * heapallindexed
 * --------------------------------------------------------------------- */

/*
 * Is (ckey, lo) present in the posting set of key, or of the reserved NULL
 * entry when keyisnull (DESIGN.md §14)?
 */
static bool
rbi_verify_tid_present(RBIVerifyState *vs, Datum key, bool keyisnull,
					   uint32 hash, uint32 ckey, uint16 lo)
{
	Relation	index = vs->index;
	RBIState   *state = vs->state;
	Buffer		headbuf;
	Buffer		entrybuf;
	OffsetNumber entryoff;
	bool		present = false;

	headbuf = ReadBuffer(index,
						 RBI_BUCKET_BLKNO(rbi_bucket_of(hash,
														state->meta.nbuckets)));
	LockBuffer(headbuf, BUFFER_LOCK_SHARE);

	if (keyisnull ?
		rbi_find_null_entry(index, headbuf, BUFFER_LOCK_SHARE,
							&entrybuf, &entryoff) :
		rbi_find_entry(index, state, headbuf, BUFFER_LOCK_SHARE, key, hash,
					   &entrybuf, &entryoff))
	{
		Page		page = BufferGetPage(entrybuf);
		ItemId		iid = PageGetItemId(page, entryoff);
		RBIEntryTuple *entry = (RBIEntryTuple *) PageGetItem(page, iid);

		if ((entry->flags & RBI_ENTRY_INLINE) != 0)
		{
			Size		paylen = RBI_ENTRY_PAYLOAD_LEN(entry, ItemIdGetLength(iid));
			Size		cur = 0;

			while (rbi_inline_fetch(RBIEntryGetPayload(entry), paylen, &cur,
									vs->cbuf) > 0)
			{
				if (rbi_item_covers(vs->cbuf, ckey))
				{
					present = rbi_item_contains(vs->cbuf, ckey, lo);
					break;
				}
				if (rbi_item_first_ckey(vs->cbuf) > ckey)
					break;
			}
		}
		else
		{
			BlockNumber blk = rbi_chain_find_page(index, entry->head,
												  entry->tail, ckey);
			Buffer		cbuf;
			Page		cpage;
			OffsetNumber off;
			bool		found;

			cbuf = ReadBuffer(index, blk);
			LockBuffer(cbuf, BUFFER_LOCK_SHARE);
			cpage = BufferGetPage(cbuf);
			off = rbi_page_find_item(cpage, ckey, &found);
			if (found)
				present = rbi_item_contains((RBIContainer *)
											PageGetItem(cpage,
														PageGetItemId(cpage, off)),
											ckey, lo);
			UnlockReleaseBuffer(cbuf);
		}

		if (entrybuf != headbuf)
			UnlockReleaseBuffer(entrybuf);
	}

	UnlockReleaseBuffer(headbuf);

	return present;
}

/*
 * table_index_build_scan() callback: every heap tuple visible to our snapshot
 * has to be indexed under its key.
 */
static void
rbi_verify_heap_callback(Relation index, ItemPointer tid, Datum *values,
						 bool *isnull, bool tupleIsAlive, void *arg)
{
	RBIVerifyState *vs = (RBIVerifyState *) arg;
	RBIState   *state = vs->state;
	MemoryContext oldcxt;
	Datum		key;
	uint64		code;
	uint32		hash;
	bool		keyisnull = isnull[0];

	oldcxt = MemoryContextSwitchTo(vs->heapcxt);

	rbi_check_key_offset(tid);

	if (keyisnull)
	{
		/* NULL keys live in the reserved entry of bucket 0 (DESIGN.md §14). */
		key = (Datum) 0;
		hash = RBI_NULLKEY_HASH;
	}
	else
	{
		key = values[0];
		if (!state->typbyval && state->typlen == -1)
			key = PointerGetDatum(PG_DETOAST_DATUM(key));

		hash = rbi_hash_key(state, key);
	}
	code = rbi_tid_to_code(tid);

	if (!rbi_verify_tid_present(vs, key, keyisnull, hash, rbi_code_ckey(code),
								rbi_code_lo(code)))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("heap tuple (%u,%u) from table \"%s\" is not indexed in \"%s\"",
						ItemPointerGetBlockNumber(tid),
						ItemPointerGetOffsetNumber(tid),
						RelationGetRelationName(vs->heap),
						RelationGetRelationName(vs->index)),
				 errdetail("The tuple's key is %s.",
						   keyisnull ? "NULL" :
						   OidOutputFunctionCall(vs->keyoutfunc, key))));

	vs->nheaptuples++;

	MemoryContextSwitchTo(oldcxt);
	MemoryContextReset(vs->heapcxt);
}

/*
 * Scan the heap with a fresh MVCC snapshot and check that every visible tuple
 * is in the index, the way contrib/amcheck does for its heapallindexed check.
 */
static void
rbi_verify_heapallindexed(RBIVerifyState *vs)
{
	IndexInfo  *indexinfo = BuildIndexInfo(vs->index);
	TableScanDesc scan;
	Snapshot	snapshot;
	bool		typisvarlena;

	getTypeOutputInfo(vs->state->typid, &vs->keyoutfunc, &typisvarlena);

	vs->heapcxt = AllocSetContextCreate(CurrentMemoryContext,
										"roaring index verify heap tuple",
										ALLOCSET_DEFAULT_SIZES);

	snapshot = RegisterSnapshot(GetTransactionSnapshot());

	/*
	 * A new snapshot is guaranteed to have every entry the index needs, but
	 * at higher isolation levels an old transaction snapshot may predate the
	 * index's indcheckxmin horizon, in which case it is not safe to use.
	 */
	if (IsolationUsesXactSnapshot() && vs->index->rd_index->indcheckxmin &&
		!TransactionIdPrecedes(HeapTupleHeaderGetXmin(vs->index->rd_indextuple->t_data),
							   snapshot->xmin))
	{
		UnregisterSnapshot(snapshot);
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("index \"%s\" cannot be verified using transaction snapshot",
						RelationGetRelationName(vs->index))));
	}

	/*
	 * The scan is created here rather than by table_index_build_scan() so
	 * that it really uses the snapshot registered above; the scan behaves
	 * like the first heap scan of a CREATE INDEX CONCURRENTLY, which maps
	 * heap-only tuples back to the TID of their HOT chain root - the TID the
	 * index actually holds.  table_index_build_scan() ends the scan for us.
	 */
	indexinfo->ii_Concurrent = true;
	indexinfo->ii_Unique = false;
	indexinfo->ii_ExclusionOps = NULL;
	indexinfo->ii_ExclusionProcs = NULL;
	indexinfo->ii_ExclusionStrats = NULL;

	scan = table_beginscan_strat(vs->heap, snapshot, 0, NULL, true, true);

	table_index_build_scan(vs->heap, vs->index, indexinfo, true, false,
						   rbi_verify_heap_callback, (void *) vs, scan);

	UnregisterSnapshot(snapshot);
	MemoryContextDelete(vs->heapcxt);
	vs->heapcxt = NULL;
}

Datum
roaring_index_verify(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	bool		heapallindexed = PG_GETARG_BOOL(1);
	RBIVerifyState vs;
	Oid			heapoid;
	uint32		b;

	memset(&vs, 0, sizeof(vs));

	vs.index = rbi_open_index(relid, AccessShareLock);
	heapoid = IndexGetRelation(relid, false);
	vs.heap = table_open(heapoid, AccessShareLock);

	vs.state = rbi_get_state(vs.index);
	vs.nblocks = RelationGetNumberOfBlocks(vs.index);
	vs.cbuf = (RBIContainer *) palloc(RBI_CONTAINER_MAX_SIZE);
	vs.refs = (uint8 *) palloc0(sizeof(uint8) * Max(vs.nblocks, 1));

	rbi_verify_meta(&vs);

	for (b = 0; b < vs.state->meta.nbuckets; b++)
	{
		rbi_verify_bucket(&vs, b);
		CHECK_FOR_INTERRUPTS();
	}

	rbi_verify_reachable(&vs);

	if (heapallindexed)
		rbi_verify_heapallindexed(&vs);

	pfree(vs.refs);
	pfree(vs.cbuf);

	table_close(vs.heap, AccessShareLock);
	index_close(vs.index, AccessShareLock);

	PG_RETURN_VOID();
}
