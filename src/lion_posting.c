/*-------------------------------------------------------------------------
 *
 * lion_posting.c
 *		The per-key posting tree of the lion index (DESIGN.md §22).
 *
 * A CHAIN entry's posting set used to be a rightlinked chain of container
 * pages, which can only be walked.  It is now a B-tree over container keys -
 * GIN's posting-tree shape - whose LEAVES are those very container pages,
 * still rightlinked in ckey order, so every sequential reader (the bitmap
 * scan, the single-set count, VACUUM) does exactly what it did before, while
 * an intersection can SEEK to a container key instead of streaming everything
 * below it.
 *
 * The shape, and the four rules that make it work:
 *
 * 1. THE ROOT NEVER MOVES.  The entry's `head` is the root block, and it is
 *	  also the owner stamp of every page of the set (DESIGN.md §18), which is
 *	  the identity a reader that holds nothing but a head block validates
 *	  against.  So a root split does not allocate a new root: it PUSHES THE
 *	  ROOT DOWN - the root's items go to a brand new child and the root block
 *	  becomes an internal page with one downlink to it - exactly as nbtree and
 *	  GIN keep their root in place.  The entry is therefore never rewritten for
 *	  a root split, "a head block is never recycled as a head" stays true, and
 *	  no record ever has to carry both the entry leaf and four tree pages.
 *
 * 2. WRITERS OF ONE KEY SERIALISE ON THAT KEY'S DIRECTORY LEAF.  Every path
 *	  that changes a posting tree - aminsert, an INLINE spill, VACUUM's regrow
 *	  and its apply step - holds the directory leaf that carries the entry
 *	  EXCLUSIVE while it does (DESIGN.md §5, §11, §21).  So no split of a
 *	  posting tree can be in flight while another writer descends it, and a
 *	  write descent needs no move-right at the leaf level at all: the
 *	  separators say exactly which leaf owns a container key.  Only READERS
 *	  race with splits, and a reader that lands on a leaf whose maxckey no
 *	  longer reaches its key simply moves right, which is where a split put the
 *	  items.
 *
 * 3. NO COUPLING DOWNWARDS.  A descent releases a page before it locks the
 *	  child, and a split holds the child while it locks the parent.  That is
 *	  nbtree's rule and §21's, and the reason is not thrift: coupling downwards
 *	  would close a cycle that buffer content locks have no detector for.
 *
 * 4. A SPLIT IS TWO RECORDS AND AN IDEMPOTENT REPAIR.  The first record splits
 *	  the page and flags it LION_PAGE_INCOMPLETE_SPLIT; the downlink goes into
 *	  the parent next, and a third, tiny record clears the flag.  GenericXLog
 *	  takes four buffers and a leaf split already needs (left, new right, a
 *	  second new page, the entry leaf), so the flag cannot ride along with the
 *	  parent insertion the way nbtree does it.  A crash anywhere in between
 *	  leaves the flag set; the next WRITE descent that meets it finishes the
 *	  split before touching the page, and the repair looks for the downlink
 *	  before adding one, so doing it twice is harmless.  Readers never notice:
 *	  the right link takes them to the items.
 *
 * Every page modification here goes through GenericXLog, and every buffer is
 * registered before it is touched.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/generic_xlog.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "utils/rel.h"

#include "lion.h"

static Buffer lion_posting_find_parent(Relation index, Relation heaprel,
									   uint32 hash, BlockNumber head,
									   BlockNumber childblk, uint16 childlevel,
									   uint32 routeckey, bool haveroute,
									   OffsetNumber *offp);
static void lion_posting_place_pivot(Relation index, Relation heaprel,
									 uint32 hash, BlockNumber head, Buffer buf,
									 OffsetNumber off,
									 const LionPostingPivot *pivot);

/* ---------------------------------------------------------------------
 * Page helpers
 * --------------------------------------------------------------------- */

/*
 * Read blk, lock it in lockmode and check that it is still a page of the
 * posting set rooted at head (DESIGN.md §18).
 *
 * A mismatch means the set was deleted after the caller copied the entry,
 * which VACUUM only does once the set held nothing visible to anyone: a
 * READER treats it as the end of the set and gets InvalidBuffer back.  Every
 * WRITER holds the entry's directory leaf, so the set cannot go away under it
 * and a mismatch is corruption - those callers pass strict.
 */
static Buffer
lion_posting_getbuf(Relation index, BlockNumber blk, BlockNumber head,
					int lockmode, bool strict)
{
	Buffer		buf;
	Page		page;

	if (!BlockNumberIsValid(blk))
	{
		if (strict)
			elog(ERROR, "lion index \"%s\": posting set at %u has no page there",
				 RelationGetRelationName(index), head);
		return InvalidBuffer;
	}

	buf = ReadBuffer(index, blk);
	LockBuffer(buf, lockmode);
	page = BufferGetPage(buf);

	if (!lion_page_owns(page, head))
	{
		UnlockReleaseBuffer(buf);
		if (strict)
			elog(ERROR, "lion index \"%s\": block %u is not a page of the posting set at %u",
				 RelationGetRelationName(index), blk, head);
		return InvalidBuffer;
	}

	return buf;
}

/*
 * The downlink of the child that owns ckey: the LAST separator at or below
 * it.  Separators are non-decreasing (a page both of whose sides are empty
 * can be given the separator of its left neighbour, see
 * lion_posting_finish_split()), so "the last one" and not "any one" is what
 * routes a key past an emptied page.
 *
 * A key below every separator on the page takes the leftmost child, which is
 * conservative rather than wrong: the search then lands left of its key and
 * the move-right rule walks to it.
 */
static OffsetNumber
lion_posting_downlink_off(Page page, uint32 ckey)
{
	OffsetNumber first = lion_posting_first_data(page);
	OffsetNumber lo = first;
	OffsetNumber hi = OffsetNumberNext(PageGetMaxOffsetNumber(page));

	/* first offset whose separator is strictly above ckey */
	while (lo < hi)
	{
		OffsetNumber mid = lo + (hi - lo) / 2;

		if (lion_posting_pivot(page, mid)->ckey > ckey)
			hi = mid;
		else
			lo = OffsetNumberNext(mid);
	}

	if (lo <= first)
		return first;
	return OffsetNumberPrev(lo);
}

/* Move to the right sibling, releasing the page we came from first. */
static Buffer
lion_posting_step_right(Relation index, Buffer buf, BlockNumber head,
						int lockmode, bool strict)
{
	BlockNumber next = LionPageGetOpaque(BufferGetPage(buf))->rightlink;

	Assert(BlockNumberIsValid(next));
	UnlockReleaseBuffer(buf);

	CHECK_FOR_INTERRUPTS();
	return lion_posting_getbuf(index, next, head, lockmode, strict);
}

/* ---------------------------------------------------------------------
 * The descent
 * --------------------------------------------------------------------- */

Buffer
lion_posting_search(Relation index, Relation heaprel, uint32 hash,
					BlockNumber head, uint32 ckey, int lockmode, bool forwrite)
{
	Buffer		buf;
	Page		page;

	Assert(!forwrite || lockmode == BUFFER_LOCK_EXCLUSIVE);

restart:
	buf = lion_posting_getbuf(index, head, head, BUFFER_LOCK_SHARE, forwrite);
	if (!BufferIsValid(buf))
		return InvalidBuffer;

	if (LionPageIsPostingLeaf(BufferGetPage(buf)) &&
		lockmode != BUFFER_LOCK_SHARE)
	{
		/*
		 * A one-page set: the root IS the leaf and has to be relocked in the
		 * caller's mode.  A root push-down may have happened in between, and
		 * then the page is no longer a leaf and the descent starts over.
		 */
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		LockBuffer(buf, lockmode);
		page = BufferGetPage(buf);
		if (!lion_page_owns(page, head))
		{
			UnlockReleaseBuffer(buf);
			if (forwrite)
				elog(ERROR, "lion index \"%s\": the posting set at %u went away under a writer",
					 RelationGetRelationName(index), head);
			return InvalidBuffer;
		}
		if (!LionPageIsPostingLeaf(page))
		{
			UnlockReleaseBuffer(buf);
			goto restart;
		}
	}

	for (;;)
	{
		uint16		level;
		OffsetNumber off;
		BlockNumber child;
		int			childlock;

		page = BufferGetPage(buf);
		level = LionPageGetOpaque(page)->level;

		/*
		 * A writer finishes EVERY unfinished split it meets on the way down,
		 * not only one it ends up standing on: the page to the right of a
		 * flagged one is the page with no downlink, and a writer that stepped
		 * over the flag and then split THAT page would have no parent item to
		 * insert next to.  The repair moves items' ownership around, so the
		 * descent starts again rather than guessing where it now stands.
		 */
		if (forwrite && LionPageIncompleteSplit(page))
		{
			if (level > 0)
			{
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);
				LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
				if (!lion_page_owns(BufferGetPage(buf), head))
					elog(ERROR, "lion index \"%s\": the posting set at %u went away under a writer",
						 RelationGetRelationName(index), head);
			}
			if (LionPageIncompleteSplit(BufferGetPage(buf)))
				lion_posting_finish_split(index, heaprel, hash, head, buf);
			UnlockReleaseBuffer(buf);
			CHECK_FOR_INTERRUPTS();
			goto restart;
		}

		/* An internal page whose high key no longer exceeds ckey: move right. */
		if (level > 0 && !LionPageIsRightmost(page) &&
			lion_posting_highkey(page)->ckey <= ckey)
		{
			buf = lion_posting_step_right(index, buf, head, BUFFER_LOCK_SHARE,
										  forwrite);
			if (!BufferIsValid(buf))
				return InvalidBuffer;
			if (LionPageGetOpaque(BufferGetPage(buf))->level != level)
			{
				UnlockReleaseBuffer(buf);	/* a root push-down raced us */
				goto restart;
			}
			continue;
		}

		if (level == 0)
		{
			/*
			 * A READER may be standing on a leaf a concurrent split has moved
			 * its container key off; the items only ever go to a page
			 * immediately to the right, so walking right until the page can
			 * hold ckey finds them.  Empty leaves - VACUUM leaves them in the
			 * chain - have maxckey 0 and are walked past for the same reason.
			 *
			 * A WRITER never does this: writers of one key serialise on the
			 * entry's directory leaf (see the file header), so no split can be
			 * in flight and the separators route it to the leaf that OWNS
			 * ckey, which is where the item has to go even when the leaf's own
			 * keys are all below it.
			 */
			if (!forwrite)
			{
				while (LionPageGetOpaque(page)->maxckey < ckey &&
					   !LionPageIsRightmost(page))
				{
					buf = lion_posting_step_right(index, buf, head, lockmode,
												  false);
					if (!BufferIsValid(buf))
						return InvalidBuffer;
					page = BufferGetPage(buf);
					if (!LionPageIsPostingLeaf(page))
					{
						UnlockReleaseBuffer(buf);
						goto restart;
					}
				}
			}
			return buf;
		}

		off = lion_posting_downlink_off(page, ckey);
		if (off > PageGetMaxOffsetNumber(page))
			elog(ERROR, "lion index \"%s\": internal posting page %u has no downlink",
				 RelationGetRelationName(index), BufferGetBlockNumber(buf));

		child = lion_posting_pivot(page, off)->child;
		childlock = (level == 1) ? lockmode : BUFFER_LOCK_SHARE;

		/* Release the parent BEFORE locking the child; see the file header. */
		UnlockReleaseBuffer(buf);
		buf = lion_posting_getbuf(index, child, head, childlock, forwrite);
		if (!BufferIsValid(buf))
			return InvalidBuffer;
		if (LionPageGetOpaque(BufferGetPage(buf))->level != level - 1)
		{
			UnlockReleaseBuffer(buf);
			goto restart;
		}

		CHECK_FOR_INTERRUPTS();
	}
}

BlockNumber
lion_posting_leftmost_leaf(Relation index, uint32 hash, BlockNumber head)
{
	Buffer		buf;
	BlockNumber blk;

	/*
	 * Container key 0 is the smallest there is, so an ordinary descent for it
	 * takes the leftmost downlink of every level.
	 */
	buf = lion_posting_search(index, NULL, hash, head, 0, BUFFER_LOCK_SHARE,
							  false);
	if (!BufferIsValid(buf))
		return InvalidBlockNumber;

	blk = BufferGetBlockNumber(buf);
	UnlockReleaseBuffer(buf);

	return blk;
}

/*
 * DESIGN.md §4 called this "the container page that owns ckey", and it was a
 * walk from the head; it is a descent now, and it is still the only place a
 * container key is turned into a block.  The caller holds the entry, so the
 * set cannot go away and anything unexpected is corruption.  `tail` is
 * accepted and unused: the append hint lives in the insert path, which takes
 * the tail page EXCLUSIVE directly (lion_insert_lock_chain_page()).
 */
BlockNumber
lion_chain_find_page(Relation index, uint32 hash, BlockNumber head,
					 BlockNumber tail, uint32 ckey)
{
	Buffer		buf;
	BlockNumber blk;

	Assert(BlockNumberIsValid(head) && BlockNumberIsValid(tail));

	buf = lion_posting_search(index, NULL, hash, head, ckey, BUFFER_LOCK_SHARE,
							  false);
	if (!BufferIsValid(buf))
		elog(ERROR, "lion index \"%s\": the posting set at %u is gone",
			 RelationGetRelationName(index), head);

	blk = BufferGetBlockNumber(buf);
	UnlockReleaseBuffer(buf);

	return blk;
}

/* ---------------------------------------------------------------------
 * Root push-down
 * --------------------------------------------------------------------- */

/*
 * Move everything the root holds onto a brand new child and turn the root
 * block itself into the level above, holding one downlink.  The root keeps
 * its block, so the entry's `head` - and with it the owner stamp of every
 * page of the set - is unchanged (see rule 1 in the file header).
 *
 * entrybuf/entry are only needed when the root is a LEAF, because then the
 * set's last leaf changes and `tail` travels in the same record; an internal
 * root's push-down touches no entry field at all and passes InvalidBuffer.
 */
static void
lion_posting_pushdown(Relation index, Relation heaprel, Buffer buf,
					  Buffer entrybuf, OffsetNumber entryoff,
					  LionEntryTuple *entry, BlockNumber *childp)
{
	Page		page = BufferGetPage(buf);
	BlockNumber head = BufferGetBlockNumber(buf);
	uint32		hash = LionPageGetOpaque(page)->owner_hash;
	uint16		level = LionPageGetOpaque(page)->level;
	PGAlignedBlock *copy;
	Page		cpage;
	OffsetNumber maxoff;
	OffsetNumber off;
	GenericXLogState *xstate;
	Page		pR;
	Page		pC;
	Buffer		cbuf;
	BlockNumber cblk;
	LionPostingPivot pivot;

	Assert(LionPageIsContainer(page));
	Assert(!LionPageIncompleteSplit(page));
	Assert(LionPageIsRightmost(page));	/* a root has no sibling */
	Assert(level < LION_POSTING_MAX_HEIGHT);

	/* Both pages are rebuilt, so work from a private copy of the root. */
	copy = (PGAlignedBlock *) palloc(sizeof(PGAlignedBlock));
	cpage = (Page) copy->data;
	memcpy(cpage, page, BLCKSZ);
	maxoff = PageGetMaxOffsetNumber(cpage);

	xstate = GenericXLogStart(index);
	pR = GenericXLogRegisterBuffer(xstate, buf, 0);
	cbuf = lion_new_buffer_xl(index, heaprel, xstate, LION_PAGE_CONTAINER,
							  true, &pC);
	cblk = BufferGetBlockNumber(cbuf);

	/* The child takes the root's items, at the bytes they were allotted. */
	LionPageGetOpaque(pC)->level = level;
	LionPageGetOpaque(pC)->rightlink = InvalidBlockNumber;
	lion_page_set_owner(pC, hash, head);
	for (off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId		iid = PageGetItemId(cpage, off);

		if (!ItemIdIsUsed(iid))
			continue;
		if (PageAddItemExtended(pC, PageGetItem(cpage, iid),
								ItemIdGetLength(iid), InvalidOffsetNumber,
								0) == InvalidOffsetNumber)
			elog(ERROR, "lion index \"%s\": could not push the root of the posting set at %u down",
				 RelationGetRelationName(index), head);
	}
	if (level == 0)
		lion_page_update_minmax(pC);

	/* ... and the root block becomes the level above, with one downlink. */
	lion_init_page(pR, LION_PAGE_CONTAINER);
	LionPageGetOpaque(pR)->level = level + 1;
	lion_page_set_owner(pR, hash, head);
	pivot.ckey = 0;				/* minus infinity: it owns everything */
	pivot.child = cblk;
	if (PageAddItemExtended(pR, (char *) &pivot, LION_POSTING_PIVOT_SIZE,
							InvalidOffsetNumber, 0) == InvalidOffsetNumber)
		elog(ERROR, "lion index \"%s\": could not build the new root of the posting set at %u",
			 RelationGetRelationName(index), head);

	if (level == 0)
	{
		Assert(entry != NULL && BufferIsValid(entrybuf));
		entry->tail = cblk;
		if (!lion_replace_entry(index, xstate, entrybuf, entryoff, entry,
								LionEntryPayloadOffset(entry)))
			elog(ERROR, "lion index: could not update entry tuple at %u/%u",
				 BufferGetBlockNumber(entrybuf), entryoff);
	}

	GenericXLogFinish(xstate);
	UnlockReleaseBuffer(cbuf);
	pfree(copy);

	*childp = cblk;
}

void
lion_posting_root_pushdown(Relation index, Relation heaprel, Buffer buf,
						   Buffer entrybuf, OffsetNumber entryoff,
						   LionEntryTuple *entry, BlockNumber *childp)
{
	Assert(LionPageIsPostingLeaf(BufferGetPage(buf)));
	Assert(BufferGetBlockNumber(buf) == entry->head);

	lion_posting_pushdown(index, heaprel, buf, entrybuf, entryoff, entry,
						  childp);
}

/* ---------------------------------------------------------------------
 * Placing a downlink
 * --------------------------------------------------------------------- */

/*
 * Split the internal page buf, which has no room for `newitem` at off.
 *
 * Pivots are fixed size, so the cut is simply the middle: the left half keeps
 * a fresh high key (a copy of the first separator of the right half) and the
 * right half inherits the old one.  The page is flagged and the downlink for
 * the new sibling goes into the parent next, as rule 4 of the file header
 * describes.  buf is held EXCLUSIVE throughout by the caller.
 */
static void
lion_posting_split_internal(Relation index, Relation heaprel, uint32 hash,
							BlockNumber head, Buffer buf, OffsetNumber off,
							const LionPostingPivot *newitem)
{
	Page		page = BufferGetPage(buf);
	BlockNumber pblk = BufferGetBlockNumber(buf);
	uint16		level = LionPageGetOpaque(page)->level;
	BlockNumber oldright = LionPageGetOpaque(page)->rightlink;
	bool		rightmost = LionPageIsRightmost(page);
	OffsetNumber first = lion_posting_first_data(page);
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	OffsetNumber o;
	LionPostingPivot *items;
	LionPostingPivot oldhk;
	int			nitems = 0;
	int			k;
	int			i;
	GenericXLogState *xstate;
	Page		pP;
	Page		pR;
	Buffer		rbuf;
	BlockNumber rblk;
	LionPostingPivot hk;

	Assert(level > 0);
	Assert(pblk != head);
	Assert(!LionPageIncompleteSplit(page));

	if (!rightmost)
		oldhk = *lion_posting_highkey(page);

	items = (LionPostingPivot *)
		palloc(sizeof(LionPostingPivot) * (maxoff - first + 3));
	for (o = first; o <= maxoff + 1; o++)
	{
		if (o == off)
			items[nitems++] = *newitem;
		if (o > maxoff)
			break;
		if (!ItemIdIsUsed(PageGetItemId(page, o)))
			continue;
		items[nitems++] = *lion_posting_pivot(page, o);
	}

	if (nitems < 2)
		elog(ERROR, "lion index \"%s\": internal posting page %u cannot be split",
			 RelationGetRelationName(index), pblk);

	/*
	 * The middle cut always fits, which is why this needs none of the
	 * walk-backs lion_dir_choose_split() does: pivots are FIXED size, so a
	 * page holds at most LION_PAGE_CAPACITY / (MAXALIGN(8) + 4) = 679 of them
	 * and each half of a cut at nitems/2 is at most 341 pivots plus a high
	 * key - a little over half a page.  A page with at least two items can
	 * therefore always be split (DESIGN.md §22).
	 */
	k = nitems / 2;
	Assert((Size) (k + 1) * (MAXALIGN(LION_POSTING_PIVOT_SIZE) +
						 sizeof(ItemIdData)) <= (Size) LION_PAGE_CAPACITY);
	Assert((Size) (nitems - k + 1) * (MAXALIGN(LION_POSTING_PIVOT_SIZE) +
								  sizeof(ItemIdData)) <= (Size) LION_PAGE_CAPACITY);
	hk.ckey = items[k].ckey;
	hk.child = InvalidBlockNumber;

	xstate = GenericXLogStart(index);
	pP = GenericXLogRegisterBuffer(xstate, buf, 0);
	rbuf = lion_new_buffer_xl(index, heaprel, xstate, LION_PAGE_CONTAINER,
							  true, &pR);
	rblk = BufferGetBlockNumber(rbuf);

	lion_init_page(pP, LION_PAGE_CONTAINER | LION_PAGE_INCOMPLETE_SPLIT);
	LionPageGetOpaque(pP)->level = level;
	LionPageGetOpaque(pP)->rightlink = rblk;
	lion_page_set_owner(pP, hash, head);
	if (PageAddItemExtended(pP, (char *) &hk, LION_POSTING_PIVOT_SIZE,
							InvalidOffsetNumber, 0) == InvalidOffsetNumber)
		elog(ERROR, "lion index \"%s\": could not place a posting high key",
			 RelationGetRelationName(index));
	for (i = 0; i < k; i++)
	{
		if (PageAddItemExtended(pP, (char *) &items[i], LION_POSTING_PIVOT_SIZE,
								InvalidOffsetNumber, 0) == InvalidOffsetNumber)
			elog(ERROR, "lion index \"%s\": could not place a posting downlink",
				 RelationGetRelationName(index));
	}

	LionPageGetOpaque(pR)->level = level;
	LionPageGetOpaque(pR)->rightlink = oldright;
	lion_page_set_owner(pR, hash, head);
	if (!rightmost &&
		PageAddItemExtended(pR, (char *) &oldhk, LION_POSTING_PIVOT_SIZE,
							InvalidOffsetNumber, 0) == InvalidOffsetNumber)
		elog(ERROR, "lion index \"%s\": could not place a posting high key",
			 RelationGetRelationName(index));
	for (i = k; i < nitems; i++)
	{
		if (PageAddItemExtended(pR, (char *) &items[i], LION_POSTING_PIVOT_SIZE,
								InvalidOffsetNumber, 0) == InvalidOffsetNumber)
			elog(ERROR, "lion index \"%s\": could not place a posting downlink",
				 RelationGetRelationName(index));
	}

	GenericXLogFinish(xstate);
	UnlockReleaseBuffer(rbuf);
	pfree(items);

	lion_posting_finish_split(index, heaprel, hash, head, buf);
}

/*
 * Put `pivot` at off on the internal posting page buf, which the caller holds
 * EXCLUSIVE and which stays locked (the page it ends up on may be a sibling).
 */
static void
lion_posting_place_pivot(Relation index, Relation heaprel, uint32 hash,
						 BlockNumber head, Buffer buf, OffsetNumber off,
						 const LionPostingPivot *pivot)
{
	Page		page = BufferGetPage(buf);

	Assert(LionPageIsPostingInternal(page));

	/*
	 * Never touch a page whose own split never finished: a second split of it
	 * would overwrite the flag and lose the first one's downlink.
	 */
	if (LionPageIncompleteSplit(page))
	{
		lion_posting_finish_split(index, heaprel, hash, head, buf);
		page = BufferGetPage(buf);
	}

	if (PageGetFreeSpace(page) >= MAXALIGN(LION_POSTING_PIVOT_SIZE))
	{
		GenericXLogState *xstate = GenericXLogStart(index);
		Page		p = GenericXLogRegisterBuffer(xstate, buf, 0);

		if (PageAddItemExtended(p, (char *) pivot, LION_POSTING_PIVOT_SIZE,
								off, 0) == InvalidOffsetNumber)
		{
			GenericXLogAbort(xstate);
			elog(ERROR, "lion index \"%s\": could not add a downlink to block %u",
				 RelationGetRelationName(index), BufferGetBlockNumber(buf));
		}
		GenericXLogFinish(xstate);
		return;
	}

	if (BufferGetBlockNumber(buf) == head)
	{
		/*
		 * The root is full.  Push it down - which keeps its block, rule 1 -
		 * and place the pivot on the child, which is a verbatim copy of the
		 * old root and therefore wants it at the very same offset.  The root
		 * lock is dropped while that happens, because the child's own split
		 * will have to take it to insert ITS downlink, and buffer locks are
		 * not reentrant.  Nothing else can be modifying this tree (rule 2), so
		 * letting go of the root for that window changes nothing a reader can
		 * see beyond the push-down itself, which is already on disk.
		 */
		BlockNumber cblk;
		Buffer		cbuf;

		lion_posting_pushdown(index, heaprel, buf, InvalidBuffer,
							  InvalidOffsetNumber, NULL, &cblk);

		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		cbuf = lion_posting_getbuf(index, cblk, head, BUFFER_LOCK_EXCLUSIVE,
								   true);
		lion_posting_place_pivot(index, heaprel, hash, head, cbuf, off, pivot);
		UnlockReleaseBuffer(cbuf);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		return;
	}

	lion_posting_split_internal(index, heaprel, hash, head, buf, off, pivot);
}

/*
 * Is the downlink for rblk already somewhere in the parent level?  A crash
 * between the two records of a split leaves the flag set with the downlink
 * already there, and the repair must not add a second one.
 */
static bool
lion_posting_downlink_present(Relation index, Buffer pbuf, BlockNumber head,
							  BlockNumber rblk)
{
	Page		page = BufferGetPage(pbuf);
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	OffsetNumber off;
	BlockNumber next;
	Buffer		nbuf;
	bool		found = false;

	for (off = lion_posting_first_data(page); off <= maxoff; off++)
	{
		if (lion_posting_pivot(page, off)->child == rblk)
			return true;
	}

	/* It could be the first item of the parent's right sibling. */
	next = LionPageGetOpaque(page)->rightlink;
	if (!BlockNumberIsValid(next))
		return false;

	nbuf = lion_posting_getbuf(index, next, head, BUFFER_LOCK_SHARE, false);
	if (!BufferIsValid(nbuf))
		return false;
	{
		Page		np = BufferGetPage(nbuf);
		OffsetNumber nfirst = lion_posting_first_data(np);

		found = nfirst <= PageGetMaxOffsetNumber(np) &&
			lion_posting_pivot(np, nfirst)->child == rblk;
	}
	UnlockReleaseBuffer(nbuf);

	return found;
}

void
lion_posting_finish_split(Relation index, Relation heaprel, uint32 hash,
						  BlockNumber head, Buffer pbuf)
{
	lion_posting_finish_split_sep(index, heaprel, hash, head, pbuf, NULL);
}

void
lion_posting_finish_split_sep(Relation index, Relation heaprel, uint32 hash,
							  BlockNumber head, Buffer pbuf,
							  const uint32 *knownsep)
{
	Page		ppage = BufferGetPage(pbuf);
	BlockNumber pblk = BufferGetBlockNumber(pbuf);
	BlockNumber rblk = LionPageGetOpaque(ppage)->rightlink;
	uint16		level = LionPageGetOpaque(ppage)->level;
	OffsetNumber maxoff = PageGetMaxOffsetNumber(ppage);
	OffsetNumber first = lion_posting_first_data(ppage);
	uint32		routeckey = 0;
	bool		haveroute = false;
	bool		havesep = false;
	uint32		sep = 0;
	uint32		leftsep;
	Buffer		parent;
	OffsetNumber off;
	GenericXLogState *xstate;
	Page		p;

	Assert(LionPageIncompleteSplit(ppage));

	if (pblk == head || !BlockNumberIsValid(rblk))
		elog(ERROR, "lion index \"%s\": block %u is flagged as an incomplete split but has no right sibling to link",
			 RelationGetRelationName(index), pblk);

	/*
	 * A key that routes to THIS page, for finding its downlink.  A page VACUUM
	 * has emptied has none, and then the parent level is scanned from its
	 * leftmost page instead.
	 */
	if (level > 0)
	{
		if (first <= maxoff)
		{
			routeckey = lion_posting_pivot(ppage, first)->ckey;
			haveroute = true;
		}
		/* An internal split wrote the separator as this page's high key. */
		sep = lion_posting_highkey(ppage)->ckey;
		havesep = true;
	}
	else if (maxoff >= FirstOffsetNumber)
	{
		routeckey = LionPageGetOpaque(ppage)->minckey;
		haveroute = true;
	}

	/*
	 * A leaf split knows the separator - it is the first container key it put
	 * on the new sibling - and says so, which spares this a read of a page the
	 * caller may still be holding EXCLUSIVE (P -> M -> N leaves two of them in
	 * one hand).  Only the crash repair arrives without one.
	 */
	if (!havesep && knownsep != NULL)
	{
		sep = *knownsep;
		havesep = true;
	}

	/*
	 * A LEAF carries no high key, so the separator is the right sibling's own
	 * first container key.  Two degenerate cases have to be covered because a
	 * flag only outlives its split after a crash, and a VACUUM may have run in
	 * between and emptied either page: then any value that keeps the parent's
	 * separators non-decreasing and routes ckeys at or above this page's last
	 * one to the right sibling will do, because the two pages between them own
	 * exactly the range the parent gave the left one.
	 */
	if (!havesep)
	{
		Buffer		rbuf = lion_posting_getbuf(index, rblk, head,
											   BUFFER_LOCK_SHARE, true);
		Page		rp = BufferGetPage(rbuf);

		if (PageGetMaxOffsetNumber(rp) >= FirstOffsetNumber)
		{
			sep = LionPageGetOpaque(rp)->minckey;
			havesep = true;
		}
		UnlockReleaseBuffer(rbuf);
	}
	if (!havesep && maxoff >= FirstOffsetNumber)
	{
		uint32		mx = LionPageGetOpaque(ppage)->maxckey;

		if (mx < PG_UINT32_MAX)
		{
			sep = mx + 1;
			havesep = true;
		}
	}

	parent = lion_posting_find_parent(index, heaprel, hash, head, pblk, level,
									  routeckey, haveroute, &off);
	leftsep = lion_posting_pivot(BufferGetPage(parent), off)->ckey;
	if (!havesep || sep < leftsep)
		sep = leftsep;			/* both halves are empty: they own one range */

	if (!lion_posting_downlink_present(index, parent, head, rblk))
	{
		LionPostingPivot pivot;

		pivot.ckey = sep;
		pivot.child = rblk;
		lion_posting_place_pivot(index, heaprel, hash, head, parent,
								 OffsetNumberNext(off), &pivot);
	}

	UnlockReleaseBuffer(parent);

	xstate = GenericXLogStart(index);
	p = GenericXLogRegisterBuffer(xstate, pbuf, 0);
	LionPageGetOpaque(p)->flags &= ~(uint16) LION_PAGE_INCOMPLETE_SPLIT;
	GenericXLogFinish(xstate);
}

/*
 * The page at childlevel + 1 that holds the downlink of childblk, locked
 * EXCLUSIVE, with *offp its offset.  Without a route key the scan starts at
 * the leftmost page of that level.
 */
static Buffer
lion_posting_find_parent(Relation index, Relation heaprel, uint32 hash,
						 BlockNumber head, BlockNumber childblk,
						 uint16 childlevel, uint32 routeckey, bool haveroute,
						 OffsetNumber *offp)
{
	Buffer		buf;
	BlockNumber blk;

	buf = lion_posting_getbuf(index, head, head, BUFFER_LOCK_SHARE, true);
	if (LionPageGetOpaque(BufferGetPage(buf))->level <= childlevel)
	{
		UnlockReleaseBuffer(buf);
		elog(ERROR, "lion index \"%s\": posting set at %u has no level above %u for block %u",
			 RelationGetRelationName(index), head, childlevel, childblk);
	}

	/* Down to the level above the child, reading only. */
	for (;;)
	{
		Page		page = BufferGetPage(buf);
		OffsetNumber off;
		BlockNumber child;

		if (LionPageGetOpaque(page)->level == childlevel + 1)
			break;

		if (haveroute)
		{
			while (!LionPageIsRightmost(page) &&
				   lion_posting_highkey(page)->ckey <= routeckey)
			{
				buf = lion_posting_step_right(index, buf, head,
											  BUFFER_LOCK_SHARE, true);
				page = BufferGetPage(buf);
			}
			off = lion_posting_downlink_off(page, routeckey);
		}
		else
			off = lion_posting_first_data(page);

		if (off > PageGetMaxOffsetNumber(page))
		{
			UnlockReleaseBuffer(buf);
			elog(ERROR, "lion index \"%s\": internal posting page has no downlink",
				 RelationGetRelationName(index));
		}
		child = lion_posting_pivot(page, off)->child;
		UnlockReleaseBuffer(buf);
		buf = lion_posting_getbuf(index, child, head, BUFFER_LOCK_SHARE, true);
		CHECK_FOR_INTERRUPTS();
	}

	/*
	 * At the right level.  Take the page EXCLUSIVE (the descent had it SHARE)
	 * and scan right until the downlink turns up.  The scan holds one page at
	 * a time and only ever moves rightwards, so two repairers cannot deadlock.
	 */
	blk = BufferGetBlockNumber(buf);
	UnlockReleaseBuffer(buf);

	for (;;)
	{
		Page		page;
		OffsetNumber off;
		OffsetNumber maxoff;

		buf = lion_posting_getbuf(index, blk, head, BUFFER_LOCK_EXCLUSIVE, true);
		page = BufferGetPage(buf);

		/*
		 * This page's own split may never have finished.  Finish it first: the
		 * downlink we want may be on the page its sibling became, and nothing
		 * may add an item to a page whose flag a later split of it would
		 * overwrite.  The recursion is bounded by the height, because every
		 * step of it goes one level up.
		 */
		if (LionPageIncompleteSplit(page))
		{
			lion_posting_finish_split(index, heaprel, hash, head, buf);
			page = BufferGetPage(buf);
		}

		maxoff = PageGetMaxOffsetNumber(page);
		for (off = lion_posting_first_data(page); off <= maxoff; off++)
		{
			if (lion_posting_pivot(page, off)->child == childblk)
			{
				*offp = off;
				return buf;
			}
		}

		blk = LionPageGetOpaque(page)->rightlink;
		UnlockReleaseBuffer(buf);
		if (!BlockNumberIsValid(blk))
			elog(ERROR, "lion index \"%s\": no downlink for block %u at level %u of the posting set at %u",
				 RelationGetRelationName(index), childblk, childlevel + 1, head);
		CHECK_FOR_INTERRUPTS();
	}
}
