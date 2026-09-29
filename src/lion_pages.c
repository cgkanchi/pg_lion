/*-------------------------------------------------------------------------
 *
 * lion_pages.c
 *		Index pages: initializing, deleting and recycling them, the WAL-logged
 *		new and reinitialized buffer, and the items of container pages.
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
 * The item at off of a container page - a posting-tree leaf - for a caller
 * that is about to hand it to the container or segment code.
 *
 * AN ITEM ON A PAGE IS DATA (lion_container.c, "untrusted containers").  The
 * container and segment code is memory-safe for any payload behind a header
 * of a valid type, provided the item really holds the lion_item_size() its
 * header claims: that is the one thing a caller has to know, and this is
 * where it is made sure of for a page item, as lion_inline_fetch() makes sure
 * of it for an INLINE payload.  The line pointer has to be a normal one - a
 * container page never has any other kind, because every delete there
 * compacts - that lies inside the page's item space at a MAXALIGNed offset,
 * the test lion_verify_itemid() makes, which is amcheck's; the type has to be
 * one of the four item kinds; the size the header gives has to fit both the
 * line pointer's length and the largest legal item; and a sparse segment has
 * to hold a pair, because an empty one is never stored and has no last
 * container key.  Anything else is an ERROR naming the index and the block.
 * Before this, a damaged item was read wherever its line pointer and its
 * header said, up to 256 KiB past the page image (2026-09-27 review).  What
 * the payload itself holds is lion_index_verify()'s business.
 *
 * It is a dozen comparisons per item, against the hundreds to thousands of
 * instructions the container code then spends on it.  page may be a private
 * copy of the block - the readers copy a leaf under its lock and read the
 * copy - which is why the block number is passed in, for the message.
 */
LionContainer *
lion_page_item_fetch(Relation index, Page page, BlockNumber blkno,
					 OffsetNumber off)
{
	PageHeader	phdr = (PageHeader) page;
	ItemId		iid;
	LionContainer *item;
	unsigned	lpoff;
	unsigned	lplen;
	Size		size;

	/* the line pointer array and the item space are on the page */
	if (unlikely(phdr->pd_lower > phdr->pd_upper ||
				 phdr->pd_upper > phdr->pd_special ||
				 phdr->pd_special > BLCKSZ))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": container page %u has a corrupt page header (lower %u, upper %u, special %u)",
						RelationGetRelationName(index), blkno,
						phdr->pd_lower, phdr->pd_upper, phdr->pd_special)));
	if (unlikely(off < FirstOffsetNumber || off > PageGetMaxOffsetNumber(page)))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": container page %u has no item %u",
						RelationGetRelationName(index), blkno, off)));

	iid = PageGetItemId(page, off);
	if (unlikely(!ItemIdIsNormal(iid)))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": line pointer %u on container page %u is %s, which a container page never has",
						RelationGetRelationName(index), off, blkno,
						!ItemIdIsUsed(iid) ? "unused" :
						ItemIdIsDead(iid) ? "dead" : "a redirect")));

	lpoff = ItemIdGetOffset(iid);
	lplen = ItemIdGetLength(iid);
	if (unlikely(lplen < LION_CONTAINER_HDRSZ ||
				 lpoff < phdr->pd_upper || lpoff + lplen > phdr->pd_special ||
				 lpoff != MAXALIGN(lpoff)))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": line pointer %u on container page %u points at %u bytes at offset %u, outside the item space %u .. %u",
						RelationGetRelationName(index), off, blkno, lplen,
						lpoff, phdr->pd_upper, phdr->pd_special)));

	item = (LionContainer *) PageGetItem(page, iid);
	if (unlikely(item->type != LION_CT_ARRAY && item->type != LION_CT_BITSET &&
				 item->type != LION_CT_RUN && item->type != LION_CT_SPARSE))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": item %u on container page %u has type %u, which is no item kind",
						RelationGetRelationName(index), off, blkno,
						item->type)));

	/* a RUN is sized by its run count, which follows the header */
	if (item->type == LION_CT_RUN && lplen < LION_CONTAINER_HDRSZ + sizeof(uint16))
		size = LION_CONTAINER_HDRSZ + sizeof(uint16);
	else
		size = lion_item_size(item);
	if (unlikely(size > lplen || size > LION_CONTAINER_MAX_SIZE))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": item %u on container page %u needs %zu bytes, but has %u",
						RelationGetRelationName(index), off, blkno, size,
						lplen)));

	if (unlikely(item->type == LION_CT_SPARSE && item->cardinality == 0))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": sparse segment %u on container page %u is empty",
						RelationGetRelationName(index), off, blkno)));

	return item;
}

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
lion_page_find_item(Relation index, Page page, BlockNumber blkno, uint32 ckey,
					bool *found)
{
	OffsetNumber low = FirstOffsetNumber;
	OffsetNumber high = PageGetMaxOffsetNumber(page);
	OffsetNumber cand = InvalidOffsetNumber;

	*found = false;

	while (low <= high)
	{
		OffsetNumber mid = low + (high - low) / 2;
		LionContainer *c = lion_page_item_fetch(index, page, blkno, mid);

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
		LionContainer *c = lion_page_item_fetch(index, page, blkno, cand);

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
lion_page_find_container(Relation index, Page page, BlockNumber blkno,
						 uint32 ckey, bool *found)
{
	OffsetNumber low = FirstOffsetNumber;
	OffsetNumber high = PageGetMaxOffsetNumber(page);

	*found = false;

	/* Invariant: everything below low is < ckey, everything above high is > ckey */
	while (low <= high)
	{
		OffsetNumber mid = low + (high - low) / 2;
		LionContainer *c = lion_page_item_fetch(index, page, blkno, mid);

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
void
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
void
lion_item_zero_slack(LionContainer *item, Size size, Size alloc)
{
	Assert(alloc >= size);
	if (alloc > size)
		memset((char *) item + size, 0, alloc - size);
}
