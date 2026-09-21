/*-------------------------------------------------------------------------
 *
 * lion_funcs.c
 *		SQL-callable helpers for the lion index (DESIGN.md section 7):
 *		lion_index_stats() and lion_index_verify().
 *
 * Both open the index with AccessShareLock and read one page at a time under
 * a SHARE lock, so they run concurrently with inserts; lion_index_verify()
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

#include "lion.h"

PG_FUNCTION_INFO_V1(lion_index_stats);
PG_FUNCTION_INFO_V1(lion_index_verify);

#define LION_STATS_NCOLS		19

typedef struct LionVerifyState
{
	Relation	index;
	Relation	heap;
	LionState   *state;
	BlockNumber nblocks;
	uint8	   *refs;			/* how often each block is referenced */
	LionContainer *cbuf;			/* aligned container work buffer */
	int64		nnullentries;	/* reserved NULL-key entries seen (at most 1) */
	int64		nemptyentries;	/* reserved no-key entries seen (at most 1) */
	Oid			keyoutfunc;		/* output function of the indexed type */
	MemoryContext heapcxt;		/* per heap tuple, heapallindexed only */
	int64		nheaptuples;
} LionVerifyState;

/*
 * Report structural damage.  Every message names the block (and item) the
 * problem was found in, as the caller has no other way of locating it.
 */
#define lion_corrupt(...) \
	ereport(ERROR, \
			(errcode(ERRCODE_INDEX_CORRUPTED), \
			 errmsg(__VA_ARGS__)))

/*
 * Open relid as a lion index.
 */
static Relation
lion_open_index(Oid relid, LOCKMODE lockmode)
{
	Relation	index = index_open(relid, lockmode);

	if (index->rd_rel->relkind != RELKIND_INDEX ||
		index->rd_indam == NULL ||
		index->rd_indam->ambuild != lionbuild)
	{
		char	   *name = pstrdup(RelationGetRelationName(index));

		index_close(index, lockmode);
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a lion index", name)));
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
 * lion_index_stats()
 * --------------------------------------------------------------------- */

typedef struct LionStats
{
	int64		bucket_pages;
	int64		entries;
	int64		inline_entries;
	int64		container_pages;
	int64		containers;
	int64		by_type[4];		/* indexed by LionContainerType */
	int64		sparse_segments;	/* items of type LION_CT_SPARSE */
	int64		sparse_members;	/* (ckey, lo) pairs inside them */
	int64		ntids;
	int64		null_tids;		/* members of the reserved NULL entry (§14) */
	int64		empty_tids;		/* members of the reserved EMPTY entry (§17) */
	int64		max_bucket_pages;	/* longest bucket chain (DESIGN.md §5) */
	int64		container_bytes;	/* logical bytes of every item */
	int64		slack_bytes;	/* free bytes INSIDE items (DESIGN.md §4) */
	int64		free_bytes;
	int64		deleted_pages;	/* freed pages awaiting reuse (DESIGN.md §18) */
} LionStats;

/*
 * Account for one item of a posting set.  The container counters count real
 * containers only; a sparse segment (DESIGN.md §13) is reported by
 * sparse_segments/sparse_members instead.  container_bytes is the bytes of
 * every item, whatever its kind.
 */
static void
lion_stats_item(LionStats *st, const LionContainer *c, Size itemlen)
{
	Size		size = lion_item_size(c);

	if (c->type == LION_CT_SPARSE)
	{
		st->sparse_segments++;
		st->sparse_members += (int64) c->cardinality;
	}
	else
	{
		st->containers++;
		if (c->type >= LION_CT_ARRAY && c->type <= LION_CT_RUN)
			st->by_type[c->type]++;
	}

	/*
	 * container_bytes counts what the items really hold; an item on a
	 * container page may have been allotted more than that, and those spare
	 * bytes - growth slack an insert can add a member into without moving
	 * anything else (DESIGN.md §4) - are reported separately.  An item inside
	 * an INLINE payload never has any.
	 */
	st->container_bytes += (int64) size;
	if (itemlen > size)
		st->slack_bytes += (int64) (itemlen - size);
}

Datum
lion_index_stats(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	index;
	LionState   *state;
	LionStats	st;
	LionContainer *cbuf;
	BlockNumber nblocks;
	BlockNumber blk;
	TupleDesc	tupdesc;
	Datum		values[LION_STATS_NCOLS];
	bool		nulls[LION_STATS_NCOLS];
	HeapTuple	tuple;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	index = lion_open_index(relid, AccessShareLock);
	state = lion_get_state(index);

	memset(&st, 0, sizeof(st));
	cbuf = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);

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

		if (PageIsNew(page) || PageGetSpecialSize(page) != LION_SPECIAL_SIZE)
		{
			UnlockReleaseBuffer(buf);
			continue;
		}

		maxoff = PageGetMaxOffsetNumber(page);

		if (LionPageIsBucket(page))
		{
			st.bucket_pages++;
			st.free_bytes += (int64) PageGetFreeSpace(page);

			for (off = FirstOffsetNumber; off <= maxoff; off++)
			{
				ItemId		iid = PageGetItemId(page, off);
				LionEntryTuple *entry;

				if (!ItemIdIsUsed(iid))
					continue;

				entry = (LionEntryTuple *) PageGetItem(page, iid);
				st.entries++;
				st.ntids += (int64) entry->ntids;
				if (LionEntryIsNullKey(entry))
					st.null_tids += (int64) entry->ntids;
				if (LionEntryIsEmptyKey(entry))
					st.empty_tids += (int64) entry->ntids;

				if ((entry->flags & LION_ENTRY_INLINE) != 0)
				{
					Size		paylen = LION_ENTRY_PAYLOAD_LEN(entry,
															   ItemIdGetLength(iid));
					Size		cur = 0;
					Size		csize;

					st.inline_entries++;
					while ((csize = lion_inline_fetch(LionEntryGetPayload(entry),
													 paylen, &cur, cbuf)) > 0)
						lion_stats_item(&st, cbuf, csize);
				}
			}
		}
		else if (LionPageIsContainer(page))
		{
			/*
			 * A DELETED page (DESIGN.md §18) holds nothing and is waiting in
			 * the free space map to be handed out again; it is neither a
			 * container page nor free space of one.
			 */
			if (LionPageIsDeleted(page))
			{
				st.deleted_pages++;
				UnlockReleaseBuffer(buf);
				continue;
			}

			st.container_pages++;
			st.free_bytes += (int64) PageGetFreeSpace(page);

			for (off = FirstOffsetNumber; off <= maxoff; off++)
			{
				ItemId		iid = PageGetItemId(page, off);

				if (!ItemIdIsUsed(iid))
					continue;

				lion_stats_item(&st,
							   (LionContainer *) PageGetItem(page, iid),
							   ItemIdGetLength(iid));
			}
		}

		UnlockReleaseBuffer(buf);
		CHECK_FOR_INTERRUPTS();
	}

	pfree(cbuf);

	/*
	 * The longest bucket chain, which is what a lookup of a key in the worst
	 * bucket has to walk.  ambuild aims at one page per bucket, so a number
	 * well above 1 means the index has outgrown the directory it was built
	 * with and wants a REINDEX (DESIGN.md §5); inserts warn about the same
	 * thing.  It needs a walk of its own: the page loop above visits blocks in
	 * block order and cannot tell which bucket an overflow page belongs to.
	 */
	{
		uint32		b;

		for (b = 0; b < state->meta.nbuckets; b++)
		{
			Buffer		headbuf = ReadBuffer(index, LION_BUCKET_BLKNO(b));
			int			n;

			LockBuffer(headbuf, BUFFER_LOCK_SHARE);
			n = lion_bucket_npages(index, headbuf);
			UnlockReleaseBuffer(headbuf);

			st.max_bucket_pages = Max(st.max_bucket_pages, (int64) n);
			CHECK_FOR_INTERRUPTS();
		}
	}

	memset(nulls, 0, sizeof(nulls));
	values[0] = Int32GetDatum((int32) state->meta.nbuckets);
	values[1] = Int64GetDatum(st.bucket_pages);
	values[2] = Int64GetDatum(st.entries);
	values[3] = Int64GetDatum(st.inline_entries);
	values[4] = Int64GetDatum(st.container_pages);
	values[5] = Int64GetDatum(st.containers);
	values[6] = Int64GetDatum(st.by_type[LION_CT_ARRAY]);
	values[7] = Int64GetDatum(st.by_type[LION_CT_BITSET]);
	values[8] = Int64GetDatum(st.by_type[LION_CT_RUN]);
	values[9] = Int64GetDatum(st.ntids);
	values[10] = Int64GetDatum(st.container_bytes);
	values[11] = Int64GetDatum(st.free_bytes);
	values[12] = Int64GetDatum(st.sparse_segments);
	values[13] = Int64GetDatum(st.sparse_members);
	values[14] = Int64GetDatum(st.null_tids);
	values[15] = Int64GetDatum(st.empty_tids);
	values[16] = Int64GetDatum(st.slack_bytes);
	values[17] = Int64GetDatum(st.max_bucket_pages);
	values[18] = Int64GetDatum(st.deleted_pages);

	tuple = heap_form_tuple(tupdesc, values, nulls);

	index_close(index, AccessShareLock);

	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/* ---------------------------------------------------------------------
 * lion_index_verify()
 * --------------------------------------------------------------------- */

/*
 * Record that blk is referenced by something, and refuse to look at it twice
 * (which also stops a corrupt rightlink cycle from looping forever).
 */
static void
lion_verify_visit(LionVerifyState *vs, BlockNumber blk, const char *what)
{
	if (blk >= vs->nblocks)
		lion_corrupt("lion index \"%s\": %s points at block %u, but the index has only %u blocks",
					RelationGetRelationName(vs->index), what, blk, vs->nblocks);

	if (vs->refs[blk] != 0)
		lion_corrupt("lion index \"%s\": block %u is referenced more than once (reached again as %s)",
					RelationGetRelationName(vs->index), blk, what);

	vs->refs[blk]++;
}

/*
 * Read blk with a SHARE lock and check its page header and kind.
 */
static Page
lion_verify_read_page(LionVerifyState *vs, BlockNumber blk, uint16 kind,
					 Buffer *bufp)
{
	Buffer		buf;
	Page		page;
	LionPageOpaque opaque;
	uint16		flags;

	buf = ReadBuffer(vs->index, blk);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	*bufp = buf;

	if (PageIsNew(page))
		lion_corrupt("lion index \"%s\": block %u has never been initialised",
					RelationGetRelationName(vs->index), blk);

	if (PageGetSpecialSize(page) != LION_SPECIAL_SIZE)
		lion_corrupt("lion index \"%s\": block %u has a special area of %u bytes, expected %zu",
					RelationGetRelationName(vs->index), blk,
					(unsigned) PageGetSpecialSize(page),
					(Size) LION_SPECIAL_SIZE);

	opaque = LionPageGetOpaque(page);

	if (opaque->page_id != LION_PAGE_ID)
		lion_corrupt("lion index \"%s\": block %u has page id 0x%04X, expected 0x%04X",
					RelationGetRelationName(vs->index), blk,
					opaque->page_id, LION_PAGE_ID);

	flags = opaque->flags & (LION_PAGE_META | LION_PAGE_BUCKET | LION_PAGE_CONTAINER);
	if (flags != LION_PAGE_META && flags != LION_PAGE_BUCKET &&
		flags != LION_PAGE_CONTAINER)
		lion_corrupt("lion index \"%s\": block %u has flags 0x%04X, expected exactly one page kind",
					RelationGetRelationName(vs->index), blk, opaque->flags);

	/* Only a container page may ever carry the DELETED bit (DESIGN.md §18). */
	if ((opaque->flags & LION_PAGE_DELETED) != 0 && flags != LION_PAGE_CONTAINER)
		lion_corrupt("lion index \"%s\": block %u is marked deleted but is a %s page",
					RelationGetRelationName(vs->index), blk,
					flags == LION_PAGE_META ? "meta" : "bucket");

	if (flags != kind)
		lion_corrupt("lion index \"%s\": block %u is a %s page, expected a %s page",
					RelationGetRelationName(vs->index), blk,
					flags == LION_PAGE_META ? "meta" :
					flags == LION_PAGE_BUCKET ? "bucket" : "container",
					kind == LION_PAGE_META ? "meta" :
					kind == LION_PAGE_BUCKET ? "bucket" : "container");

	return page;
}

/*
 * Check that the stored key of an entry has the length its type calls for,
 * so that hashing it cannot run off the end of the item.
 */
static void
lion_verify_keylen(LionVerifyState *vs, BlockNumber blk, OffsetNumber off,
				  const LionEntryTuple *entry)
{
	LionState   *state = vs->state;
	Size		keylen = entry->keylen;
	const char *key = LionEntryGetKey(entry);

	if (keylen == 0 || keylen > LION_MAX_KEY_SIZE)
		lion_corrupt("lion index \"%s\": entry %u on block %u has key length %zu, expected 1 .. %d",
					RelationGetRelationName(vs->index), off, blk, keylen,
					LION_MAX_KEY_SIZE);

	if (state->typbyval)
	{
		if (keylen != sizeof(Datum))
			lion_corrupt("lion index \"%s\": entry %u on block %u has key length %zu, expected %zu for a by-value type",
						RelationGetRelationName(vs->index), off, blk, keylen,
						sizeof(Datum));
	}
	else if (state->typlen > 0)
	{
		if (keylen != (Size) state->typlen)
			lion_corrupt("lion index \"%s\": entry %u on block %u has key length %zu, expected %d",
						RelationGetRelationName(vs->index), off, blk, keylen,
						state->typlen);
	}
	else if (state->typlen == -1)
	{
		if (VARATT_IS_EXTERNAL(key) || VARATT_IS_COMPRESSED(key) ||
			VARSIZE_ANY(key) != keylen)
			lion_corrupt("lion index \"%s\": entry %u on block %u has a malformed varlena key of %zu bytes",
						RelationGetRelationName(vs->index), off, blk, keylen);
	}
	else
	{
		if (strnlen(key, keylen) != keylen - 1)
			lion_corrupt("lion index \"%s\": entry %u on block %u has a malformed cstring key of %zu bytes",
						RelationGetRelationName(vs->index), off, blk, keylen);
	}
}

/*
 * An item may be allotted MORE bytes on a container page than its header
 * needs: growth slack the insert path adds a member into without moving
 * anything else on the page (DESIGN.md §4).  So the rule is not "the item
 * fills its space exactly" any more, but "it fills it to within one slack
 * allowance": avail is the allocated length (ItemIdGetLength, or the exact
 * size of an item inside an INLINE payload, which never has slack).
 */
static void
lion_verify_item_slack(LionVerifyState *vs, BlockNumber blk, OffsetNumber off,
					  const LionContainer *item, Size avail, const char *what)
{
	Size		size = lion_item_size(item);

	if (avail < size)
		lion_corrupt("lion index \"%s\": %s %u on block %u occupies %zu bytes but needs %zu",
					RelationGetRelationName(vs->index), what, off, blk, avail,
					size);

	if (avail > (Size) LION_CONTAINER_MAX_SIZE)
		lion_corrupt("lion index \"%s\": %s %u on block %u occupies %zu bytes, more than an item may ever take",
					RelationGetRelationName(vs->index), what, off, blk, avail);

	if (avail - size > LION_ITEM_SLACK_BOUND)
		lion_corrupt("lion index \"%s\": %s %u on block %u occupies %zu bytes, %zu more than its %zu bytes need (at most %d bytes of slack)",
					RelationGetRelationName(vs->index), what, off, blk, avail,
					avail - size, size, LION_ITEM_SLACK_BOUND);
}

/*
 * Check one sparse segment (DESIGN.md §13).
 *
 * Besides its own structure, a segment has to respect the two rules that
 * make the rest of the index able to ignore it: its range must start above
 * everything before it (the caller keeps the last ckey of the previous item
 * in *prevckey, so this also proves the ranges do not overlap or interleave,
 * within a page and across a chain), and no container key inside it may have
 * reached LION_SPARSE_THRESHOLD members, because such a key belongs in a
 * container of its own.
 */
static void
lion_verify_segment(LionVerifyState *vs, BlockNumber blk, OffsetNumber off,
				   const LionContainer *c, Size avail, bool *haveprev,
				   uint32 *prevckey)
{
	const char *detail = NULL;
	const uint32 *ckeys;
	uint32		n = c->cardinality;
	uint32		run = 1;
	uint32		i;

	if (!lion_sparse_check(c, avail, &detail))
		lion_corrupt("lion index \"%s\": sparse segment %u on block %u is corrupt: %s",
					RelationGetRelationName(vs->index), off, blk, detail);

	lion_verify_item_slack(vs, blk, off, c, avail, "sparse segment");

	if (n == 0)
		lion_corrupt("lion index \"%s\": sparse segment %u on block %u is empty",
					RelationGetRelationName(vs->index), off, blk);

	if (*haveprev && c->ckey <= *prevckey)
		lion_corrupt("lion index \"%s\": sparse segment %u on block %u starts at container key %u, not above the previous key %u",
					RelationGetRelationName(vs->index), off, blk, c->ckey,
					*prevckey);

	ckeys = LION_SPARSE_CKEYS_CONST(c);
	for (i = 1; i <= n; i++)
	{
		if (i < n && ckeys[i] == ckeys[i - 1])
		{
			run++;
			continue;
		}
		if (run >= LION_SPARSE_THRESHOLD)
			lion_corrupt("lion index \"%s\": sparse segment %u on block %u holds %u members of container key %u, which needs a container of its own",
						RelationGetRelationName(vs->index), off, blk, run,
						ckeys[i - 1]);
		run = 1;
	}

	*prevckey = lion_item_last_ckey(c);
	*haveprev = true;
}

/*
 * Check one item that is on a container page or inside an INLINE payload:
 * a container, or a sparse segment.
 */
static void
lion_verify_container(LionVerifyState *vs, BlockNumber blk, OffsetNumber off,
					 const LionContainer *c, Size avail, bool *haveprev,
					 uint32 *prevckey)
{
	const char *detail = NULL;

	if (c->type == LION_CT_SPARSE)
	{
		lion_verify_segment(vs, blk, off, c, avail, haveprev, prevckey);
		return;
	}

	if (!lion_container_check(c, avail, &detail))
		lion_corrupt("lion index \"%s\": container %u on block %u is corrupt: %s",
					RelationGetRelationName(vs->index), off, blk, detail);

	lion_verify_item_slack(vs, blk, off, c, avail, "container");

	if (c->cardinality == 0)
		lion_corrupt("lion index \"%s\": container %u on block %u is empty",
					RelationGetRelationName(vs->index), off, blk);

	if (*haveprev && c->ckey <= *prevckey)
		lion_corrupt("lion index \"%s\": container %u on block %u has container key %u, not above the previous key %u",
					RelationGetRelationName(vs->index), off, blk, c->ckey,
					*prevckey);

	*prevckey = c->ckey;
	*haveprev = true;
}

/*
 * Walk and check the container chain of a CHAIN entry.
 */
static void
lion_verify_chain(LionVerifyState *vs, BlockNumber eblk, OffsetNumber eoff,
				 const LionEntryTuple *entry)
{
	BlockNumber blk = entry->head;
	BlockNumber last = InvalidBlockNumber;
	bool		haveprev = false;
	uint32		prevckey = 0;
	uint64		card = 0;
	uint32		ncontainers = 0;

	if (!BlockNumberIsValid(entry->head) || !BlockNumberIsValid(entry->tail))
		lion_corrupt("lion index \"%s\": chain entry %u on block %u has head %u and tail %u",
					RelationGetRelationName(vs->index), eoff, eblk,
					entry->head, entry->tail);

	while (BlockNumberIsValid(blk))
	{
		Buffer		buf;
		Page		page;
		LionPageOpaque opaque;
		OffsetNumber maxoff;
		OffsetNumber off;
		OffsetNumber firstused = InvalidOffsetNumber;
		OffsetNumber lastused = InvalidOffsetNumber;

		lion_verify_visit(vs, blk, "a container chain");
		page = lion_verify_read_page(vs, blk, LION_PAGE_CONTAINER, &buf);
		opaque = LionPageGetOpaque(page);
		maxoff = PageGetMaxOffsetNumber(page);

		/*
		 * DESIGN.md §18.  A live entry must not reach a freed page at all,
		 * and every page of a chain has to name that chain: readers rely on
		 * both to tell a chain they still hold a link to from one whose pages
		 * have been handed to somebody else.
		 */
		if (LionPageIsDeleted(page))
			lion_corrupt("lion index \"%s\": block %u is reachable from chain entry %u on block %u but is marked deleted",
						RelationGetRelationName(vs->index), blk, eoff, eblk);

		if (opaque->owner_head != entry->head ||
			opaque->owner_hash != entry->hash)
			lion_corrupt("lion index \"%s\": block %u of chain entry %u on block %u is owned by hash %u at head %u, expected hash %u at head %u",
						RelationGetRelationName(vs->index), blk, eoff, eblk,
						opaque->owner_hash, opaque->owner_head,
						entry->hash, entry->head);

		for (off = FirstOffsetNumber; off <= maxoff; off++)
		{
			ItemId		iid = PageGetItemId(page, off);
			LionContainer *c;

			if (!ItemIdIsUsed(iid))
				continue;

			c = (LionContainer *) PageGetItem(page, iid);
			lion_verify_container(vs, blk, off, c, ItemIdGetLength(iid),
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
				lion_corrupt("lion index \"%s\": empty container page %u has minckey %u and maxckey %u, expected 0 and 0",
							RelationGetRelationName(vs->index), blk,
							opaque->minckey, opaque->maxckey);
		}
		else
		{
			uint32		minckey =
				lion_item_first_ckey((LionContainer *)
									PageGetItem(page, PageGetItemId(page, firstused)));
			uint32		maxckey =
				lion_item_last_ckey((LionContainer *)
								   PageGetItem(page, PageGetItemId(page, lastused)));

			if (opaque->minckey != minckey || opaque->maxckey != maxckey)
				lion_corrupt("lion index \"%s\": container page %u has minckey %u and maxckey %u, but holds %u .. %u",
							RelationGetRelationName(vs->index), blk,
							opaque->minckey, opaque->maxckey, minckey, maxckey);
		}

		last = blk;
		blk = opaque->rightlink;
		UnlockReleaseBuffer(buf);

		CHECK_FOR_INTERRUPTS();
	}

	if (last != entry->tail)
		lion_corrupt("lion index \"%s\": chain entry %u on block %u ends at block %u, but its tail is block %u",
					RelationGetRelationName(vs->index), eoff, eblk, last,
					entry->tail);

	if (card != entry->ntids)
		lion_corrupt("lion index \"%s\": chain entry %u on block %u claims " UINT64_FORMAT " TIDs, but its containers hold " UINT64_FORMAT,
					RelationGetRelationName(vs->index), eoff, eblk,
					entry->ntids, card);

	if (ncontainers != entry->ncontainers)
		lion_corrupt("lion index \"%s\": chain entry %u on block %u claims %u containers, but its chain holds %u",
					RelationGetRelationName(vs->index), eoff, eblk,
					entry->ncontainers, ncontainers);
}

/*
 * Check one entry tuple.
 */
static void
lion_verify_entry(LionVerifyState *vs, uint32 bucket, BlockNumber blk,
				 OffsetNumber off, ItemId iid, Page page)
{
	LionEntryTuple *entry = (LionEntryTuple *) PageGetItem(page, iid);
	Size		itemsz = ItemIdGetLength(iid);
	uint16		kind = entry->flags & (LION_ENTRY_INLINE | LION_ENTRY_CHAIN);
	uint32		nbuckets = vs->state->meta.nbuckets;

	if (itemsz < LION_ENTRY_HDRSZ)
		lion_corrupt("lion index \"%s\": entry %u on block %u is only %zu bytes",
					RelationGetRelationName(vs->index), off, blk, itemsz);

	if (kind != LION_ENTRY_INLINE && kind != LION_ENTRY_CHAIN)
		lion_corrupt("lion index \"%s\": entry %u on block %u has flags 0x%04X, expected exactly one of INLINE and CHAIN",
					RelationGetRelationName(vs->index), off, blk, entry->flags);

	if ((entry->flags & ~(uint16) (LION_ENTRY_INLINE | LION_ENTRY_CHAIN |
								   LION_ENTRY_RESERVED)) != 0)
		lion_corrupt("lion index \"%s\": entry %u on block %u has unknown flag bits in 0x%04X",
					RelationGetRelationName(vs->index), off, blk, entry->flags);

	if ((entry->flags & LION_ENTRY_RESERVED) == LION_ENTRY_RESERVED)
		lion_corrupt("lion index \"%s\": entry %u on block %u is both the null and the empty entry",
					RelationGetRelationName(vs->index), off, blk);

	if (LionEntryIsReserved(entry))
	{
		/*
		 * A reserved entry (DESIGN.md §14 and §17): no key bytes, hash 0,
		 * bucket 0, and one of each per index at most - a second one would
		 * split its rows between two entries that no reader looks for twice.
		 */
		const char *what = LionEntryIsNullKey(entry) ? "null" : "empty";

		if (entry->keylen != 0)
			lion_corrupt("lion index \"%s\": %s entry %u on block %u has a key of %u bytes",
						RelationGetRelationName(vs->index), what, off, blk,
						entry->keylen);
		if (entry->hash != LION_NULLKEY_HASH)
			lion_corrupt("lion index \"%s\": %s entry %u on block %u stores hash %u, expected %d",
						RelationGetRelationName(vs->index), what, off, blk,
						entry->hash, LION_NULLKEY_HASH);
		if (bucket != LION_NULLKEY_BUCKET)
			lion_corrupt("lion index \"%s\": %s entry %u on block %u is in bucket %u, expected bucket %d",
						RelationGetRelationName(vs->index), what, off, blk,
						bucket, LION_NULLKEY_BUCKET);
		if (LionEntryIsNullKey(entry) ? (++vs->nnullentries > 1) :
			(++vs->nemptyentries > 1))
			lion_corrupt("lion index \"%s\": entry %u on block %u is a second %s entry",
						RelationGetRelationName(vs->index), off, blk, what);

		/*
		 * Only a multi-key opclass ever writes an empty entry; finding one in
		 * a scalar index means the two flag bits have been confused
		 * somewhere.
		 */
		if (LionEntryIsEmptyKey(entry) && !vs->state->multikey)
			lion_corrupt("lion index \"%s\": entry %u on block %u is an empty-key entry, but the operator class extracts no keys",
						RelationGetRelationName(vs->index), off, blk);
	}
	else
	{
		uint32		hash;

		lion_verify_keylen(vs, blk, off, entry);

		hash = lion_hash_key(vs->state,
							lion_fetch_key(vs->state, LionEntryGetKey(entry)));
		if (hash != entry->hash)
			lion_corrupt("lion index \"%s\": entry %u on block %u stores hash %u, but its key hashes to %u",
						RelationGetRelationName(vs->index), off, blk,
						entry->hash, hash);

		if (lion_bucket_of(hash, nbuckets) != bucket)
			lion_corrupt("lion index \"%s\": entry %u on block %u belongs to bucket %u, but was found in bucket %u",
						RelationGetRelationName(vs->index), off, blk,
						lion_bucket_of(hash, nbuckets), bucket);
	}

	if (itemsz < LionEntryPayloadOffset(entry))
		lion_corrupt("lion index \"%s\": entry %u on block %u is %zu bytes, too small for its %u byte key",
					RelationGetRelationName(vs->index), off, blk, itemsz,
					entry->keylen);

	if (kind == LION_ENTRY_INLINE)
	{
		Size		paylen = LION_ENTRY_PAYLOAD_LEN(entry, itemsz);
		Size		cur = 0;
		Size		csize;
		bool		haveprev = false;
		uint32		prevckey = 0;
		uint64		card = 0;
		uint32		ncontainers = 0;

		if (BlockNumberIsValid(entry->head) || BlockNumberIsValid(entry->tail))
			lion_corrupt("lion index \"%s\": inline entry %u on block %u has head %u and tail %u, expected none",
						RelationGetRelationName(vs->index), off, blk,
						entry->head, entry->tail);

		while ((csize = lion_inline_fetch(LionEntryGetPayload(entry), paylen,
										 &cur, vs->cbuf)) > 0)
		{
			lion_verify_container(vs, blk, off, vs->cbuf, csize,
								 &haveprev, &prevckey);
			card += vs->cbuf->cardinality;
			ncontainers++;
		}

		/*
		 * Whatever is left is the zeroed slack VACUUM leaves when it writes a
		 * shrunken payload back into the bytes the entry already had
		 * (DESIGN.md §18).  It is bounded so that it cannot hide a malformed
		 * payload, and every byte of it has to really be zero.
		 */
		if (paylen - cur > LION_ENTRY_SLACK_BOUND)
			lion_corrupt("lion index \"%s\": entry %u on block %u has %zu bytes of payload slack, at most %d allowed",
						RelationGetRelationName(vs->index), off, blk,
						paylen - cur, LION_ENTRY_SLACK_BOUND);
		{
			const char *pay = LionEntryGetPayload(entry);
			Size		i;

			for (i = cur; i < paylen; i++)
			{
				if (pay[i] != 0)
					lion_corrupt("lion index \"%s\": entry %u on block %u has a non-zero byte at payload offset %zu, past its last item",
								RelationGetRelationName(vs->index), off, blk, i);
			}
		}

		if (card != entry->ntids)
			lion_corrupt("lion index \"%s\": inline entry %u on block %u claims " UINT64_FORMAT " TIDs, but its payload holds " UINT64_FORMAT,
						RelationGetRelationName(vs->index), off, blk,
						entry->ntids, card);

		if (ncontainers != entry->ncontainers)
			lion_corrupt("lion index \"%s\": inline entry %u on block %u claims %u containers, but its payload holds %u",
						RelationGetRelationName(vs->index), off, blk,
						entry->ncontainers, ncontainers);
	}
	else
	{
		if (itemsz != LionEntryPayloadOffset(entry))
			lion_corrupt("lion index \"%s\": chain entry %u on block %u is %zu bytes, expected %zu",
						RelationGetRelationName(vs->index), off, blk, itemsz,
						LionEntryPayloadOffset(entry));

		lion_verify_chain(vs, blk, off, entry);
	}
}

/*
 * Check one bucket: its chain of bucket pages and every entry on them.
 */
static void
lion_verify_bucket(LionVerifyState *vs, uint32 bucket)
{
	BlockNumber headblk = LION_BUCKET_BLKNO(bucket);
	Buffer		headbuf;
	Buffer		curbuf;
	Page		page;
	BlockNumber blk = headblk;

	lion_verify_visit(vs, headblk, "a bucket head");
	page = lion_verify_read_page(vs, headblk, LION_PAGE_BUCKET, &headbuf);
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

			lion_verify_entry(vs, bucket, blk, off, iid, page);
			CHECK_FOR_INTERRUPTS();
		}

		next = LionPageGetOpaque(page)->rightlink;
		if (!BlockNumberIsValid(next))
			break;

		lion_verify_visit(vs, next, "a bucket chain");
		{
			Buffer		nextbuf;

			page = lion_verify_read_page(vs, next, LION_PAGE_BUCKET, &nextbuf);
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
lion_verify_meta(LionVerifyState *vs)
{
	Buffer		buf;
	Page		page;
	LionMetaPageData *meta;

	lion_verify_visit(vs, LION_METAPAGE_BLKNO, "the meta page");
	page = lion_verify_read_page(vs, LION_METAPAGE_BLKNO, LION_PAGE_META, &buf);
	meta = LionPageGetMeta(page);

	if (meta->magic != LION_MAGIC || meta->version != LION_VERSION)
		lion_corrupt("lion index \"%s\": meta page has magic %08X version %u, expected %08X version %u",
					RelationGetRelationName(vs->index), meta->magic,
					meta->version, LION_MAGIC, LION_VERSION);

	if (meta->offset_bits != LION_OFFSET_BITS ||
		meta->container_bits != LION_CONTAINER_BITS)
		lion_corrupt("lion index \"%s\": meta page has offset_bits %u and container_bits %u, expected %d and %d",
					RelationGetRelationName(vs->index), meta->offset_bits,
					meta->container_bits, LION_OFFSET_BITS, LION_CONTAINER_BITS);

	if (meta->nbuckets == 0 || meta->nbuckets > LION_MAX_BUCKETS)
		lion_corrupt("lion index \"%s\": meta page has %u buckets, expected 1 .. %d",
					RelationGetRelationName(vs->index), meta->nbuckets,
					LION_MAX_BUCKETS);

	if (meta->inline_limit < LION_MIN_INLINE_LIMIT ||
		meta->inline_limit > LION_MAX_INLINE_LIMIT)
		lion_corrupt("lion index \"%s\": meta page has inline_limit %u, expected %d .. %d",
					RelationGetRelationName(vs->index), meta->inline_limit,
					LION_MIN_INLINE_LIMIT, LION_MAX_INLINE_LIMIT);

	if (meta->nbuckets != vs->state->meta.nbuckets)
		lion_corrupt("lion index \"%s\": meta page has %u buckets, but the cached state has %u",
					RelationGetRelationName(vs->index), meta->nbuckets,
					vs->state->meta.nbuckets);

	if (vs->nblocks < 1 + meta->nbuckets)
		lion_corrupt("lion index \"%s\": %u blocks are too few for a meta page and %u buckets",
					RelationGetRelationName(vs->index), vs->nblocks,
					meta->nbuckets);

	if (LionPageGetOpaque(page)->rightlink != InvalidBlockNumber)
		lion_corrupt("lion index \"%s\": the meta page has a right link to block %u",
					RelationGetRelationName(vs->index),
					LionPageGetOpaque(page)->rightlink);

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
lion_verify_reachable(LionVerifyState *vs)
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

		/*
		 * A DELETED page is the normal state of a freed one (DESIGN.md §18):
		 * it is unreferenced on purpose, it is in the free space map, and the
		 * next allocation whose safexid test it passes takes it.  Nothing to
		 * report.
		 */
		if (!PageIsNew(page) &&
			PageGetSpecialSize(page) == LION_SPECIAL_SIZE &&
			LionPageGetOpaque(page)->page_id == LION_PAGE_ID &&
			LionPageIsDeleted(page))
		{
			UnlockReleaseBuffer(buf);
			continue;
		}

		leaked = PageIsNew(page) ||
			(PageGetSpecialSize(page) == LION_SPECIAL_SIZE &&
			 LionPageGetOpaque(page)->page_id == LION_PAGE_ID &&
			 LionPageIsContainer(page) &&
			 PageGetMaxOffsetNumber(page) == 0);

		UnlockReleaseBuffer(buf);

		if (!leaked)
			lion_corrupt("lion index \"%s\": block %u is not reachable from the meta page",
						RelationGetRelationName(vs->index), blk);

		ereport(WARNING,
				(errmsg("lion index \"%s\": block %u is unused and unreachable",
						RelationGetRelationName(vs->index), blk),
				 errdetail("An interrupted page allocation leaks blocks; the next VACUUM turns them into free pages.")));
	}
}

/* ---------------------------------------------------------------------
 * heapallindexed
 * --------------------------------------------------------------------- */

/*
 * Is (ckey, lo) present in the posting set of key, or of the reserved entry
 * named by reservedflag when there is one (DESIGN.md §14 and §17)?
 */
static bool
lion_verify_tid_present(LionVerifyState *vs, Datum key, uint16 reservedflag,
					   uint32 hash, uint32 ckey, uint16 lo)
{
	Relation	index = vs->index;
	LionState   *state = vs->state;
	Buffer		headbuf;
	Buffer		entrybuf;
	OffsetNumber entryoff;
	bool		present = false;

	headbuf = ReadBuffer(index,
						 LION_BUCKET_BLKNO(lion_bucket_of(hash,
														state->meta.nbuckets)));
	LockBuffer(headbuf, BUFFER_LOCK_SHARE);

	if ((reservedflag != 0) ?
		lion_find_reserved_entry(index, headbuf, BUFFER_LOCK_SHARE,
								reservedflag, &entrybuf, &entryoff) :
		lion_find_entry(index, state, headbuf, BUFFER_LOCK_SHARE, key, hash,
					   &entrybuf, &entryoff))
	{
		Page		page = BufferGetPage(entrybuf);
		ItemId		iid = PageGetItemId(page, entryoff);
		LionEntryTuple *entry = (LionEntryTuple *) PageGetItem(page, iid);

		if ((entry->flags & LION_ENTRY_INLINE) != 0)
		{
			Size		paylen = LION_ENTRY_PAYLOAD_LEN(entry, ItemIdGetLength(iid));
			Size		cur = 0;

			while (lion_inline_fetch(LionEntryGetPayload(entry), paylen, &cur,
									vs->cbuf) > 0)
			{
				if (lion_item_covers(vs->cbuf, ckey))
				{
					present = lion_item_contains(vs->cbuf, ckey, lo);
					break;
				}
				if (lion_item_first_ckey(vs->cbuf) > ckey)
					break;
			}
		}
		else
		{
			BlockNumber blk = lion_chain_find_page(index, entry->hash,
												  entry->head, entry->tail,
												  ckey);
			Buffer		cbuf;
			Page		cpage;
			OffsetNumber off;
			bool		found;

			cbuf = ReadBuffer(index, blk);
			LockBuffer(cbuf, BUFFER_LOCK_SHARE);
			cpage = BufferGetPage(cbuf);
			off = lion_page_find_item(cpage, ckey, &found);
			if (found)
				present = lion_item_contains((LionContainer *)
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
lion_verify_one_key(LionVerifyState *vs, ItemPointer tid, Datum key,
				   uint16 reservedflag, uint64 code)
{
	uint32		hash = (reservedflag != 0) ? LION_NULLKEY_HASH :
		lion_hash_key(vs->state, key);

	if (lion_verify_tid_present(vs, key, reservedflag, hash,
							   lion_code_ckey(code), lion_code_lo(code)))
		return;

	ereport(ERROR,
			(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg("heap tuple (%u,%u) from table \"%s\" is not indexed in \"%s\"",
					ItemPointerGetBlockNumber(tid),
					ItemPointerGetOffsetNumber(tid),
					RelationGetRelationName(vs->heap),
					RelationGetRelationName(vs->index)),
			 errdetail("The tuple's key is %s.",
					   (reservedflag == LION_ENTRY_NULLKEY) ? "NULL" :
					   (reservedflag == LION_ENTRY_EMPTYKEY) ? "absent" :
					   OidOutputFunctionCall(vs->keyoutfunc, key))));
}

static void
lion_verify_heap_callback(Relation index, ItemPointer tid, Datum *values,
						 bool *isnull, bool tupleIsAlive, void *arg)
{
	LionVerifyState *vs = (LionVerifyState *) arg;
	LionState   *state = vs->state;
	MemoryContext oldcxt;
	Datum		key;
	uint64		code;

	oldcxt = MemoryContextSwitchTo(vs->heapcxt);

	lion_check_key_offset(tid);
	code = lion_tid_to_code(tid);

	if (isnull[0])
	{
		/* NULL values live in the reserved entry of bucket 0 (DESIGN.md §14). */
		lion_verify_one_key(vs, tid, (Datum) 0, LION_ENTRY_NULLKEY, code);
	}
	else if (state->multikey)
	{
		/*
		 * DESIGN.md §17: the row has to be present under EVERY key its value
		 * extracts to, and in the reserved EMPTY entry when it extracts to
		 * none.  Extracting here rather than trusting the index is the whole
		 * point of the check: it is the same call the build and the insert
		 * make, so a row that is missing under one of several keys is found.
		 */
		Datum	   *keys;
		int			nkeys = lion_extract_value(state, values[0], &keys);
		int			i;

		if (nkeys == 0)
			lion_verify_one_key(vs, tid, (Datum) 0, LION_ENTRY_EMPTYKEY, code);
		for (i = 0; i < nkeys; i++)
			lion_verify_one_key(vs, tid, keys[i], 0, code);
	}
	else
	{
		key = values[0];
		if (!state->typbyval && state->typlen == -1)
			key = PointerGetDatum(PG_DETOAST_DATUM(key));

		lion_verify_one_key(vs, tid, key, 0, code);
	}

	vs->nheaptuples++;

	MemoryContextSwitchTo(oldcxt);
	MemoryContextReset(vs->heapcxt);
}

/*
 * Scan the heap with a fresh MVCC snapshot and check that every visible tuple
 * is in the index, the way contrib/amcheck does for its heapallindexed check.
 */
static void
lion_verify_heapallindexed(LionVerifyState *vs)
{
	IndexInfo  *indexinfo = BuildIndexInfo(vs->index);
	TableScanDesc scan;
	Snapshot	snapshot;
	bool		typisvarlena;

	getTypeOutputInfo(vs->state->typid, &vs->keyoutfunc, &typisvarlena);

	vs->heapcxt = AllocSetContextCreate(CurrentMemoryContext,
										"lion index verify heap tuple",
										ALLOCSET_DEFAULT_SIZES);

	snapshot = RegisterSnapshot(GetTransactionSnapshot());

	/*
	 * A new snapshot is guaranteed to have every entry the index needs, but
	 * an old transaction snapshot may predate the index's indcheckxmin
	 * horizon, in which case it is not safe to use.  That test - and the
	 * validity/readiness tests next to it - are the planner's, shared with
	 * the SQL count functions in lion_index_usable().
	 */
	{
		const char *why;

		if (!lion_index_usable(vs->index, snapshot, &why))
		{
			UnregisterSnapshot(snapshot);
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot verify index \"%s\" against the heap because %s",
							RelationGetRelationName(vs->index), why)));
		}
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
						   lion_verify_heap_callback, (void *) vs, scan);

	UnregisterSnapshot(snapshot);
	MemoryContextDelete(vs->heapcxt);
	vs->heapcxt = NULL;
}

Datum
lion_index_verify(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	bool		heapallindexed = PG_GETARG_BOOL(1);
	LionVerifyState vs;
	Oid			heapoid;
	uint32		b;

	memset(&vs, 0, sizeof(vs));

	vs.index = lion_open_index(relid, AccessShareLock);
	heapoid = IndexGetRelation(relid, false);
	vs.heap = table_open(heapoid, AccessShareLock);

	vs.state = lion_get_state(vs.index);
	vs.nblocks = RelationGetNumberOfBlocks(vs.index);
	vs.cbuf = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	vs.refs = (uint8 *) palloc0(sizeof(uint8) * Max(vs.nblocks, 1));

	lion_verify_meta(&vs);

	for (b = 0; b < vs.state->meta.nbuckets; b++)
	{
		lion_verify_bucket(&vs, b);
		CHECK_FOR_INTERRUPTS();
	}

	lion_verify_reachable(&vs);

	if (heapallindexed)
		lion_verify_heapallindexed(&vs);

	pfree(vs.refs);
	pfree(vs.cbuf);

	table_close(vs.heap, AccessShareLock);
	index_close(vs.index, AccessShareLock);

	PG_RETURN_VOID();
}
