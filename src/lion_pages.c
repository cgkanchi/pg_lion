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
 * line pointer's length and the largest legal item, and the line pointer may
 * claim no more beyond it than the growth slack an item can carry
 * (LION_ITEM_SLACK_BOUND, the rule lion_index_verify() applies); and a sparse
 * segment has to hold a pair, because an empty one is never stored and has no
 * last container key.  Anything else is an ERROR naming the index and the
 * block.  Before this, a damaged item was read wherever its line pointer and
 * its header said, up to 256 KiB past the page image (2026-09-27 review).
 * What the payload itself holds is lion_index_verify()'s business.
 *
 * The slack bound is not for the readers, which never look past the size the
 * header gives, but for the WRITERS, which copy, move and rewrite an item by
 * its line pointer, inside a WAL record: a length that reached from an item
 * into the next one made every root push-down of its set PANIC in rmgr mode,
 * again after every recovery (2026-09-29 review).  What a writer needs beyond
 * this - the item alone in its bytes, the items of the page apart - it checks
 * itself (lion_page_check_alone(), lion_page_check_items()).
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
	if (unlikely(!lion_container_type_valid(item->type) &&
				 item->type != LION_CT_SPARSE))
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
	if (unlikely(lplen > LION_CONTAINER_MAX_SIZE ||
				 lplen - size > LION_ITEM_SLACK_BOUND))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": item %u on container page %u occupies %u bytes, %zu more than its %zu bytes need (at most %d bytes of slack)",
						RelationGetRelationName(index), off, blkno, lplen,
						lplen - size, size, LION_ITEM_SLACK_BOUND)));

	if (unlikely(item->type == LION_CT_SPARSE && item->cardinality == 0))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": sparse segment %u on container page %u is empty",
						RelationGetRelationName(index), off, blkno)));

	return item;
}

/*
 * THE LINE POINTERS OF A PAGE A RECORD IS ABOUT TO CHANGE.
 *
 * bufpage.c overwrites, deletes and moves items by their line pointers, and
 * the push-down and the splits copy items out by them, all of it inside a WAL
 * record - which in rmgr mode is a critical section (DESIGN.md §25).  A line
 * pointer the writer has not looked at first is then either a PANIC -
 * bufpage.c's "corrupted line pointer" is an ERROR, and so is an item that
 * does not fit where the record puts it - or SILENT: an item written back at
 * the length its line pointer gives, when that length reaches into the next
 * item, overwrites the next item, and the record takes the damage to every
 * standby.  So a writer checks what its record is going to do before the
 * record opens, and refuses a damaged page with ERRCODE_INDEX_CORRUPTED:
 *
 *	- lion_page_check_items(), every item, before a record that deletes
 *	  items, moves them to a new page or rewrites several of them;
 *	- lion_page_check_alone(), the one item a record overwrites or deletes in
 *	  place;
 *	- lion_page_entry_fetch(), the directory entry a record rewrites.
 *
 * A container item has been through lion_page_item_fetch() as well, which
 * bounds what its line pointer may claim beyond its size.
 */

/* The header bounds, as bufpage.c tests them before it moves anything. */
static void
lion_page_check_header(Relation index, Page page, BlockNumber blkno)
{
	PageHeader	phdr = (PageHeader) page;

	if (unlikely(phdr->pd_lower < SizeOfPageHeaderData ||
				 phdr->pd_lower > phdr->pd_upper ||
				 phdr->pd_upper > phdr->pd_special ||
				 phdr->pd_special > BLCKSZ ||
				 phdr->pd_special != MAXALIGN(phdr->pd_special)))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": block %u has a corrupt page header (lower %u, upper %u, special %u)",
						RelationGetRelationName(index), blkno,
						phdr->pd_lower, phdr->pd_upper, phdr->pd_special)));
}

/*
 * Item off is on the page, and its line pointer is a normal one that lies
 * inside the item space at a MAXALIGNed offset: lion_verify_itemid()'s test,
 * and what bufpage.c requires of an item it overwrites or deletes.
 */
static void
lion_page_check_itemid(Relation index, Page page, BlockNumber blkno,
					   OffsetNumber off)
{
	PageHeader	phdr = (PageHeader) page;
	ItemId		iid;

	if (unlikely(off < FirstOffsetNumber || off > PageGetMaxOffsetNumber(page)))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": block %u has no item %u",
						RelationGetRelationName(index), blkno, off)));

	iid = PageGetItemId(page, off);
	if (unlikely(!ItemIdIsNormal(iid) || ItemIdGetLength(iid) == 0 ||
				 ItemIdGetOffset(iid) < phdr->pd_upper ||
				 ItemIdGetOffset(iid) + ItemIdGetLength(iid) > phdr->pd_special ||
				 ItemIdGetOffset(iid) != MAXALIGN(ItemIdGetOffset(iid))))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": line pointer %u on block %u points at %u bytes at offset %u, outside the item space %u .. %u",
						RelationGetRelationName(index), off, blkno,
						ItemIdGetLength(iid), ItemIdGetOffset(iid),
						phdr->pd_upper, phdr->pd_special)));
}

/*
 * Every used line pointer of block blkno lies inside the item space, and no
 * two items overlap.  Then neither PageIndexMultiDelete() nor
 * PageIndexTupleDelete() can fail on the page or move one item over another,
 * and the items - all of them, or any of them - fit an empty page of the same
 * kind, which is where a push-down or a split moves them: disjoint items at
 * MAXALIGNed offsets take no more of the item space than it has, and their n
 * line pointers take the pd_lower - SizeOfPageHeaderData bytes they take
 * here.  An unused line pointer is one VACUUM leaves on a directory leaf
 * (DESIGN.md §18); a container page has none (lion_page_item_fetch()).
 *
 * The items of a page this index wrote tile its item space, because every
 * delete compacts, so an overlap is damage - and the one damage nothing else
 * a writer checks would see: two line pointers can each be inside the page,
 * and an item's length within its slack, while one reaches into the other.
 * The test marks the MAXALIGN units each item covers, which is a pass over
 * the page's bytes an eighth at a time whatever the number of items.
 */
void
lion_page_check_items(Relation index, Page page, BlockNumber blkno)
{
	uint64		covered[BLCKSZ / MAXIMUM_ALIGNOF / 64];
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	OffsetNumber off;

	lion_page_check_header(index, page, blkno);
	memset(covered, 0, sizeof(covered));

	for (off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId		iid = PageGetItemId(page, off);
		unsigned	u;
		unsigned	end;

		if (!ItemIdIsUsed(iid))
			continue;
		lion_page_check_itemid(index, page, blkno, off);

		end = (ItemIdGetOffset(iid) + ItemIdGetLength(iid) +
			   MAXIMUM_ALIGNOF - 1) / MAXIMUM_ALIGNOF;
		for (u = ItemIdGetOffset(iid) / MAXIMUM_ALIGNOF; u < end; u++)
		{
			uint64		bit = UINT64CONST(1) << (u % 64);

			if (unlikely((covered[u / 64] & bit) != 0))
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("lion index \"%s\": item %u on block %u overlaps another item",
								RelationGetRelationName(index), off, blkno)));
			covered[u / 64] |= bit;
		}
	}
}

/*
 * Item off of block blkno is the only one in the bytes the page allots it -
 * from its offset to the MAXALIGNed end of its length, which is where
 * bufpage.c writes an item back and what it moves the items below it by.
 * It is lion_page_check_items() for the one item a record overwrites or
 * deletes in place, as one pass over the line pointers rather than over the
 * page, because the in-place insert of a member is the hot path of a hot
 * key; and it is what keeps a line
 * pointer whose length reaches into the next item from having that item
 * overwritten - which that insert did, growing a container into its
 * neighbour, and logged (2026-09-29 review).
 */
void
lion_page_check_alone(Relation index, Page page, BlockNumber blkno,
					  OffsetNumber off)
{
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	OffsetNumber i;
	unsigned	start;
	unsigned	end;

	lion_page_check_header(index, page, blkno);
	lion_page_check_itemid(index, page, blkno, off);
	start = ItemIdGetOffset(PageGetItemId(page, off));
	end = start + MAXALIGN(ItemIdGetLength(PageGetItemId(page, off)));

	for (i = FirstOffsetNumber; i <= maxoff; i++)
	{
		ItemId		iid = PageGetItemId(page, i);

		if (i == off || !ItemIdHasStorage(iid))
			continue;
		if (unlikely(ItemIdGetOffset(iid) < end &&
					 start < (unsigned) (ItemIdGetOffset(iid) +
										 ItemIdGetLength(iid))))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("lion index \"%s\": items %u and %u on block %u overlap",
							RelationGetRelationName(index), off, i, blkno)));
	}
}

/*
 * The entry at off of directory leaf blkno, for a writer that reads its
 * payload by its line pointer's length or rewrites it in a record: a line
 * pointer lion_page_check_itemid() accepts, long enough for the header and
 * for the key the header gives - and, for a CHAIN entry, exactly that long,
 * which is what lion_index_verify() requires of one and what every record
 * that rewrites one relies on (lion_put_entry(): the entry keeps its size, so
 * "this cannot fail", inside a critical section).
 */
LionEntryTuple *
lion_page_entry_fetch(Relation index, Page page, BlockNumber blkno,
					  OffsetNumber off)
{
	LionEntryTuple *entry;
	Size		len;

	lion_page_check_header(index, page, blkno);
	lion_page_check_itemid(index, page, blkno, off);
	entry = (LionEntryTuple *) PageGetItem(page, PageGetItemId(page, off));
	len = ItemIdGetLength(PageGetItemId(page, off));

	if (unlikely(len < LION_ENTRY_HDRSZ || len < LionEntryPayloadOffset(entry)))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": entry %u on block %u is %zu bytes, too small for its header and key",
						RelationGetRelationName(index), off, blkno, len)));
	if (unlikely((entry->flags & LION_ENTRY_CHAIN) != 0 &&
				 len != LionEntryPayloadOffset(entry)))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": chain entry %u on block %u is %zu bytes, expected %zu",
						RelationGetRelationName(index), off, blkno, len,
						LionEntryPayloadOffset(entry))));

	return entry;
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

	/*
	 * A bitset is already the largest an item can be, and a NARROW (DESIGN.md
	 * §38) is the size it is whatever add() does to it in place: a member it
	 * holds sets a bit, and one it does not makes it a BITSET, which no slack
	 * short of 3 KB would hold.
	 */
	if (item->type == LION_CT_BITSET || item->type == LION_CT_NARROW)
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
