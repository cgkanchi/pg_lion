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
 *	  the parent next, and a third, tiny record clears the flag.  A record
 *	  takes four buffers and a leaf split already needs (left, new right, a
 *	  second new page, the entry leaf), so the flag cannot ride along with the
 *	  parent insertion the way nbtree does it.  A crash anywhere in between
 *	  leaves the flag set, and so does an ERROR there - the parent insertion
 *	  may have to split the parent, which allocates a page, which can fail;
 *	  the next WRITE descent that meets it finishes the split before touching
 *	  the page, and the repair looks for the downlink before adding one, so
 *	  doing it twice is harmless.  Readers never notice: the right link takes
 *	  them to the items.
 *
 *	  Two writers reach a leaf WITHOUT descending - an insert through the
 *	  append hint (the entry's `tail`) and VACUUM, which walks the right links
 *	  - and a descent is the only thing that repairs as it goes, so more rules
 *	  close the gap.  The append hint is used only for changes that stay
 *	  inside the page; anything that may split the page descends
 *	  (lion_insert.c).  And a page that has NO downlink - the right half of an
 *	  unfinished split, reached through the hint or a right link - finds, when
 *	  its own split needs its parent, the flagged page to its left and
 *	  finishes that first (lion_posting_find_parent(), nbtree's
 *	  _bt_getstackbuf() doing _bt_finish_split()).
 *
 * Every page modification here goes through the WAL shim of DESIGN.md §25,
 * which writes either a GenericXLog record or one of our own, and every buffer is
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
									   Buffer childbuf,
									   uint32 routeckey, bool haveroute,
									   OffsetNumber *offp);
static void lion_posting_place_pivot(Relation index, Relation heaprel,
									 uint32 hash, BlockNumber head, Buffer buf,
									 OffsetNumber off,
									 const LionPostingPivot *pivot);
static void lion_posting_finish_split_ext(Relation index, Relation heaprel,
										  uint32 hash, BlockNumber head,
										  Buffer pbuf, const uint32 *knownsep,
										  Buffer heldright);

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
		 * insert next to.  (lion_posting_find_parent() copes with that too,
		 * by walking the level from where the route leads up to the page and
		 * finishing the splits it passes - but that is a second pass over the
		 * level, and only a writer that reached its leaf without a descent
		 * should ever need it.)  The repair moves items' ownership around, so
		 * the descent starts again rather than guessing where it now stands.
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
 *
 * The child is returned still EXCLUSIVE-locked.  Both callers go on to write
 * to it with the root unlocked, and the one that matters is VACUUM's regrow
 * (DESIGN.md §18): there the root was a leaf VACUUM holds a CLEANUP lock on,
 * the container being re-placed is the FILTERED one, and the copy the push-down
 * just moved onto the child still holds the dead TIDs.  Were the child
 * unlocked in between, a reader could descend to it, copy that stale
 * container and keep its pin, and the write that follows - under an ordinary
 * exclusive lock - would remove the dead TIDs from under it, breaking the §11
 * interlock on the primary.  A page nobody else has ever been able to lock
 * cannot be pinned-and-copied by anyone.
 */
static Buffer
lion_posting_pushdown(Relation index, Relation heaprel, Buffer buf,
					  Buffer entrybuf, OffsetNumber entryoff,
					  LionEntryTuple *entry)
{
	Page		page = BufferGetPage(buf);
	BlockNumber head = BufferGetBlockNumber(buf);
	uint32		hash = LionPageGetOpaque(page)->owner_hash;
	uint16		level = LionPageGetOpaque(page)->level;
	PGAlignedBlock *copy;
	Page		cpage;
	OffsetNumber maxoff;
	OffsetNumber off;
	LionWalState *xstate;
	Page		pR;
	Page		pC;
	Buffer		cbuf;
	BlockNumber cblk;
	LionPostingPivot pivot;
	int			nmoved;

	Assert(LionPageIsContainer(page));
	Assert(!LionPageIncompleteSplit(page));
	Assert(LionPageIsRightmost(page));	/* a root has no sibling */
	Assert(level < LION_POSTING_MAX_HEIGHT);

	/* Both pages are rebuilt, so work from a private copy of the root. */
	copy = (PGAlignedBlock *) palloc(sizeof(PGAlignedBlock));
	cpage = (Page) copy->data;
	memcpy(cpage, page, BLCKSZ);
	maxoff = PageGetMaxOffsetNumber(cpage);

	/* The child is allocated before the record opens (DESIGN.md §25). */
	cbuf = lion_alloc_page(index, heaprel, true);
	cblk = BufferGetBlockNumber(cbuf);

	xstate = lion_wal_begin(index);

	/*
	 * The root block is REBUILT as the level above, so it is registered as a
	 * page this record initialises: replay zeroes it and rebuilds it from the
	 * one downlink below, with no full-page image at all.
	 */
	pR = lion_wal_register_buffer(xstate, buf, LION_WALBUF_INIT);
	pC = lion_wal_init_buffer(xstate, cbuf, LION_PAGE_CONTAINER);

	/* The child takes the root's items, at the bytes they were allotted. */
	LionPageGetOpaque(pC)->level = level;
	LionPageGetOpaque(pC)->rightlink = InvalidBlockNumber;
	lion_page_set_owner(pC, hash, head);
	nmoved = 0;
	lion_wal_op(xstate, pC, LION_OP_ADDMANY, FirstOffsetNumber, 0, NULL, 0);
	for (off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId		iid = PageGetItemId(cpage, off);
		uint16		isz;

		if (!ItemIdIsUsed(iid))
			continue;
		isz = (uint16) ItemIdGetLength(iid);
		if (PageAddItemExtended(pC, PageGetItem(cpage, iid), isz,
								InvalidOffsetNumber, 0) == InvalidOffsetNumber)
			elog(ERROR, "lion index \"%s\": could not push the root of the posting set at %u down",
				 RelationGetRelationName(index), head);
		lion_wal_op_append(xstate, pC, &isz, sizeof(uint16));
		lion_wal_op_append(xstate, pC, PageGetItem(cpage, iid), isz);
		nmoved++;
	}
	lion_wal_op_count(xstate, pC, (uint16) nmoved);
	if (level == 0)
		lion_page_update_minmax(pC);
	lion_wal_log_special(xstate, pC);

	/* ... and the root block becomes the level above, with one downlink. */
	lion_init_page(pR, LION_PAGE_CONTAINER);
	lion_wal_op(xstate, pR, LION_OP_INIT, 0, LION_PAGE_CONTAINER, NULL, 0);
	LionPageGetOpaque(pR)->level = level + 1;
	lion_page_set_owner(pR, hash, head);
	pivot.ckey = 0;				/* minus infinity: it owns everything */
	pivot.child = cblk;
	{
		OffsetNumber noff = PageAddItemExtended(pR, (char *) &pivot,
												LION_POSTING_PIVOT_SIZE,
												InvalidOffsetNumber, 0);

		if (noff == InvalidOffsetNumber)
			elog(ERROR, "lion index \"%s\": could not build the new root of the posting set at %u",
				 RelationGetRelationName(index), head);
		lion_wal_op(xstate, pR, LION_OP_ADD, noff, 0, &pivot,
					LION_POSTING_PIVOT_SIZE);
	}
	lion_wal_log_special(xstate, pR);

	if (level == 0)
	{
		Assert(entry != NULL && BufferIsValid(entrybuf));
		entry->tail = cblk;
		if (!lion_replace_entry(index, xstate, entrybuf, entryoff, entry,
								LionEntryPayloadOffset(entry)))
			elog(ERROR, "lion index: could not update entry tuple at %u/%u",
				 BufferGetBlockNumber(entrybuf), entryoff);
	}

	lion_wal_finish(xstate, LION_XLOG_SPLIT);
	pfree(copy);

	return cbuf;
}

Buffer
lion_posting_root_pushdown(Relation index, Relation heaprel, Buffer buf,
						   Buffer entrybuf, OffsetNumber entryoff,
						   LionEntryTuple *entry)
{
	Assert(LionPageIsPostingLeaf(BufferGetPage(buf)));
	Assert(BufferGetBlockNumber(buf) == entry->head);

	return lion_posting_pushdown(index, heaprel, buf, entrybuf, entryoff,
								 entry);
}

/* ---------------------------------------------------------------------
 * Placing a downlink
 * --------------------------------------------------------------------- */

/*
 * Fill a freshly initialised INTERNAL posting page with hk (its high key, or
 * NULL when it is rightmost) followed by items[from .. to), logging the lot as
 * one ADDMANY operation (DESIGN.md §25).
 */
static void
lion_posting_fill(Relation index, LionWalState *xstate, Page page,
				  const LionPostingPivot *hk, LionPostingPivot *items,
				  int from, int to)
{
	OffsetNumber off = FirstOffsetNumber;
	uint16		isz = (uint16) LION_POSTING_PIVOT_SIZE;
	int			i;

	if (hk != NULL)
	{
		if (PageAddItemExtended(page, (char *) hk, LION_POSTING_PIVOT_SIZE,
								InvalidOffsetNumber, 0) == InvalidOffsetNumber)
			elog(ERROR, "lion index \"%s\": could not place a posting high key",
				 RelationGetRelationName(index));
		lion_wal_op(xstate, page, LION_OP_ADD, FirstOffsetNumber, 0, hk,
					LION_POSTING_PIVOT_SIZE);
		off = OffsetNumberNext(off);
	}

	lion_wal_op(xstate, page, LION_OP_ADDMANY, off, (uint16) (to - from),
				NULL, 0);
	for (i = from; i < to; i++)
	{
		if (PageAddItemExtended(page, (char *) &items[i],
								LION_POSTING_PIVOT_SIZE, InvalidOffsetNumber,
								0) == InvalidOffsetNumber)
			elog(ERROR, "lion index \"%s\": could not place a posting downlink",
				 RelationGetRelationName(index));
		lion_wal_op_append(xstate, page, &isz, sizeof(uint16));
		lion_wal_op_append(xstate, page, &items[i], LION_POSTING_PIVOT_SIZE);
	}
}

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
	LionWalState *xstate;
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

	/* The sibling is allocated before the record opens (DESIGN.md §25). */
	rbuf = lion_alloc_page(index, heaprel, true);
	rblk = BufferGetBlockNumber(rbuf);

	xstate = lion_wal_begin(index);

	/*
	 * Both halves are rebuilt from the pivots copied out above, so both are
	 * registered as pages this record initialises and neither needs a
	 * full-page image.
	 */
	pP = lion_wal_register_buffer(xstate, buf, LION_WALBUF_INIT);
	pR = lion_wal_init_buffer(xstate, rbuf, LION_PAGE_CONTAINER);

	lion_init_page(pP, LION_PAGE_CONTAINER | LION_PAGE_INCOMPLETE_SPLIT);
	lion_wal_op(xstate, pP, LION_OP_INIT, 0,
				LION_PAGE_CONTAINER | LION_PAGE_INCOMPLETE_SPLIT, NULL, 0);
	LionPageGetOpaque(pP)->level = level;
	LionPageGetOpaque(pP)->rightlink = rblk;
	lion_page_set_owner(pP, hash, head);
	lion_posting_fill(index, xstate, pP, &hk, items, 0, k);
	lion_wal_log_special(xstate, pP);

	LionPageGetOpaque(pR)->level = level;
	LionPageGetOpaque(pR)->rightlink = oldright;
	lion_page_set_owner(pR, hash, head);
	lion_posting_fill(index, xstate, pR, rightmost ? NULL : &oldhk, items, k,
					  nitems);
	lion_wal_log_special(xstate, pR);

	lion_wal_finish(xstate, LION_XLOG_SPLIT);
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
		LionWalState *xstate = lion_wal_begin(index);
		Page		p = lion_wal_register_buffer(xstate, buf, LION_WALBUF_STD);

		if (PageAddItemExtended(p, (char *) pivot, LION_POSTING_PIVOT_SIZE,
								off, 0) == InvalidOffsetNumber)
			elog(ERROR, "lion index \"%s\": could not add a downlink to block %u",
				 RelationGetRelationName(index), BufferGetBlockNumber(buf));
		lion_wal_op(xstate, p, LION_OP_ADD, off, 0, pivot,
					LION_POSTING_PIVOT_SIZE);
		lion_wal_finish(xstate, LION_XLOG_DOWNLINK);
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
		Buffer		cbuf;

		cbuf = lion_posting_pushdown(index, heaprel, buf, InvalidBuffer,
									 InvalidOffsetNumber, NULL);

		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
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
	lion_posting_finish_split_ext(index, heaprel, hash, head, pbuf, NULL,
								  InvalidBuffer);
}

void
lion_posting_finish_split_sep(Relation index, Relation heaprel, uint32 hash,
							  BlockNumber head, Buffer pbuf,
							  const uint32 *knownsep)
{
	lion_posting_finish_split_ext(index, heaprel, hash, head, pbuf, knownsep,
								  InvalidBuffer);
}

/*
 * The one implementation of both.  heldright, when valid, is a page this
 * backend already holds EXCLUSIVE, and is the reason this exists: when it is
 * pbuf's right sibling - lion_posting_adopt() finishing the split that left
 * heldright without a downlink - the separator is read off it directly
 * instead of by locking it, which would be a lock this backend already holds
 * and would wait for itself forever.
 */
static void
lion_posting_finish_split_ext(Relation index, Relation heaprel, uint32 hash,
							  BlockNumber head, Buffer pbuf,
							  const uint32 *knownsep, Buffer heldright)
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
	LionWalState *xstate;
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
		if (BufferIsValid(heldright) && BufferGetBlockNumber(heldright) == rblk)
		{
			Page		rp = BufferGetPage(heldright);

			if (PageGetMaxOffsetNumber(rp) >= FirstOffsetNumber)
			{
				sep = LionPageGetOpaque(rp)->minckey;
				havesep = true;
			}
		}
		else
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

	parent = lion_posting_find_parent(index, heaprel, hash, head, pbuf,
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

	xstate = lion_wal_begin(index);
	p = lion_wal_register_buffer(xstate, pbuf, LION_WALBUF_STD);
	LionPageGetOpaque(p)->flags &= ~(uint16) LION_PAGE_INCOMPLETE_SPLIT;
	lion_wal_op(xstate, p, LION_OP_FLAGS, 0, LionPageGetOpaque(p)->flags,
				NULL, 0);
	lion_wal_finish(xstate, LION_XLOG_SPLIT_CLEAR);
}

/*
 * The block of `level` that the separators route routeckey to - or, without a
 * route key, the leftmost block of that level: where a scan of that level
 * for something belonging to routeckey starts.  Reads only, SHARE one page at
 * a time, and holds nothing on return.  The root must be at `level` or above
 * it.
 */
static BlockNumber
lion_posting_level_start(Relation index, BlockNumber head, uint16 level,
						 uint32 routeckey, bool haveroute)
{
	Buffer		buf;
	BlockNumber blk;

	buf = lion_posting_getbuf(index, head, head, BUFFER_LOCK_SHARE, true);
	if (LionPageGetOpaque(BufferGetPage(buf))->level < level)
	{
		UnlockReleaseBuffer(buf);
		elog(ERROR, "lion index \"%s\": posting set at %u has no level %u",
			 RelationGetRelationName(index), head, level);
	}

	for (;;)
	{
		Page		page = BufferGetPage(buf);
		OffsetNumber off;
		BlockNumber child;

		if (LionPageGetOpaque(page)->level == level)
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

	blk = BufferGetBlockNumber(buf);
	UnlockReleaseBuffer(buf);

	return blk;
}

/*
 * Scan a level rightwards from blk for the downlink of childblk, EXCLUSIVE
 * one page at a time, and return the page that holds it still locked, with
 * *offp its offset - or InvalidBuffer when the level ends first.  The scan
 * holds one page at a time and only ever moves rightwards, so two repairers
 * cannot deadlock.
 */
static Buffer
lion_posting_scan_for_downlink(Relation index, Relation heaprel, uint32 hash,
							   BlockNumber head, BlockNumber blk,
							   BlockNumber childblk, OffsetNumber *offp)
{
	for (;;)
	{
		Buffer		buf;
		Page		page;
		OffsetNumber off;
		OffsetNumber maxoff;

		buf = lion_posting_getbuf(index, blk, head, BUFFER_LOCK_EXCLUSIVE, true);
		page = BufferGetPage(buf);

		/*
		 * This page's own split may never have finished.  Finish it first: the
		 * downlink we want may be on the page its sibling became, and nothing
		 * may add an item to a page whose flag a later split of it would
		 * overwrite.  The recursion goes one level up at every step, and
		 * sideways only through lion_posting_adopt(), which clears a flag each
		 * time, so it ends.
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
			return InvalidBuffer;
		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * childbuf, which the caller holds EXCLUSIVE, has no downlink anywhere in the
 * level above.  Only one thing leaves a page like that: a split to its LEFT
 * that wrote its first record and never got to the second (a crash, or an
 * ERROR in the parent insertion), whose flagged left half is somewhere
 * between the page the separators route childbuf's keys to and childbuf
 * itself.  A descent would have met that flag and finished it; childbuf was
 * reached WITHOUT one - through the append hint, or by VACUUM walking the
 * right links - so the finishing is done here: walk the level rightwards from
 * where the route key leads, finish every unfinished split on the way, stop
 * at childbuf.  nbtree's _bt_getstackbuf() finishes the incomplete splits it
 * meets in the same spirit; it only ever meets them one level up because
 * nbtree reaches every page it splits by a descent.
 *
 * The walk locks pages to the LEFT of one this backend holds, which the lock
 * order (DESIGN.md §5) otherwise forbids, and it is safe here for the reason
 * the whole posting tree rests on: every writer of this key holds the entry's
 * directory leaf, which this backend does, so nobody else can be holding a
 * page of this tree and waiting for one to its right.  Readers hold one page
 * at a time and wait for nothing while they do, and VACUUM, when it holds a
 * page of the tree without the directory leaf, only ever asks for the leaf
 * conditionally (lion_vacuum.c, rule 2).
 *
 * The pages it passes are taken SHARE and only a flagged one is taken again
 * EXCLUSIVE.  Returns whether it finished anything; false means the level
 * holds no reason for the missing downlink, which is corruption.
 */
static bool
lion_posting_adopt(Relation index, Relation heaprel, uint32 hash,
				   BlockNumber head, Buffer childbuf, uint32 routeckey,
				   bool haveroute)
{
	BlockNumber childblk = BufferGetBlockNumber(childbuf);
	uint16		level = LionPageGetOpaque(BufferGetPage(childbuf))->level;
	bool		finished = false;
	bool		route = haveroute;

	for (;;)
	{
		BlockNumber blk = lion_posting_level_start(index, head, level,
												   routeckey, route);

		while (BlockNumberIsValid(blk) && blk != childblk)
		{
			Buffer		buf;
			Page		page;

			buf = lion_posting_getbuf(index, blk, head, BUFFER_LOCK_SHARE, true);
			page = BufferGetPage(buf);
			if (LionPageGetOpaque(page)->level != level)
			{
				UnlockReleaseBuffer(buf);
				elog(ERROR, "lion index \"%s\": block %u of the posting set at %u is not on level %u with its left neighbour",
					 RelationGetRelationName(index), blk, head, level);
			}

			if (LionPageIncompleteSplit(page))
			{
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);
				LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
				page = BufferGetPage(buf);
				if (LionPageIncompleteSplit(page))
				{
					lion_posting_finish_split_ext(index, heaprel, hash, head,
												  buf, NULL, childbuf);
					finished = true;
					page = BufferGetPage(buf);
				}
			}

			blk = LionPageGetOpaque(page)->rightlink;
			UnlockReleaseBuffer(buf);
			CHECK_FOR_INTERRUPTS();
		}

		if (blk == childblk)
			return finished;

		/*
		 * The route led PAST childbuf.  Separators are only non-decreasing - a
		 * page both of whose halves were empty when its split was repaired
		 * shares its left neighbour's (lion_posting_finish_split_ext()) - so
		 * a route key can land right of a page that holds it; start again
		 * from the leftmost page of the level, which cannot.
		 */
		if (!route)
			return false;
		route = false;
	}
}

/*
 * The page one level above childbuf that holds its downlink, locked
 * EXCLUSIVE, with *offp its offset.  routeckey is a key childbuf holds;
 * without one the scan starts at the leftmost page of that level.  childbuf is
 * held EXCLUSIVE by the caller throughout.
 *
 * A child with no downlink at all is repaired rather than reported: see
 * lion_posting_adopt().  Before that repair existed, an append that took the
 * right half of an unfinished split through the append hint and then filled
 * it hit an ERROR here, and so did every later split of the tail, for good
 * (2026-09-25 review).
 */
static Buffer
lion_posting_find_parent(Relation index, Relation heaprel, uint32 hash,
						 BlockNumber head, Buffer childbuf,
						 uint32 routeckey, bool haveroute,
						 OffsetNumber *offp)
{
	BlockNumber childblk = BufferGetBlockNumber(childbuf);
	uint16		childlevel = LionPageGetOpaque(BufferGetPage(childbuf))->level;
	Buffer		buf;

	if (childblk == head)
		elog(ERROR, "lion index \"%s\": posting set at %u has no level above %u for block %u",
			 RelationGetRelationName(index), head, childlevel, childblk);

	buf = lion_posting_scan_for_downlink(index, heaprel, hash, head,
										 lion_posting_level_start(index, head,
																  childlevel + 1,
																  routeckey,
																  haveroute),
										 childblk, offp);
	if (BufferIsValid(buf))
		return buf;

	/*
	 * Not there.  Finish whatever left it out, then look again: from where
	 * the route leads, which is where the downlink now is, and failing that
	 * from the leftmost page of the level, because a route key can lead past
	 * a downlink whose separator an emptied neighbour shares (see
	 * lion_posting_adopt()).
	 */
	if (lion_posting_adopt(index, heaprel, hash, head, childbuf, routeckey,
						   haveroute))
	{
		buf = lion_posting_scan_for_downlink(index, heaprel, hash, head,
											 lion_posting_level_start(index, head,
																	  childlevel + 1,
																	  routeckey,
																	  haveroute),
											 childblk, offp);
		if (BufferIsValid(buf))
			return buf;
	}
	if (haveroute)
	{
		buf = lion_posting_scan_for_downlink(index, heaprel, hash, head,
											 lion_posting_level_start(index, head,
																	  childlevel + 1,
																	  0, false),
											 childblk, offp);
		if (BufferIsValid(buf))
			return buf;
	}

	elog(ERROR, "lion index \"%s\": no downlink for block %u at level %u of the posting set at %u",
		 RelationGetRelationName(index), childblk, childlevel + 1, head);
	return InvalidBuffer;		/* keep the compiler quiet */
}
