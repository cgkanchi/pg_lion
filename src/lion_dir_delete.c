/*-------------------------------------------------------------------------
 *
 * lion_dir_delete.c
 *		Deleting emptied pages of the entry directory (DESIGN.md §21,
 *		"Page deletion").
 *
 * VACUUM deletes the entries whose keys have no rows left (lion_vacuum.c),
 * and a leaf it leaves with no entry at all is taken out of the tree here, so
 * that a table that deletes its oldest keys and inserts new ones keeps a
 * directory of the size its live keys need, and readers do not walk long runs
 * of empty leaves.  The scheme is nbtree's (src/backend/access/nbtree/README,
 * "Page Deletion"), adapted to the lion directory:
 *
 *	Stage 1 makes the leaf HALF_DEAD.  The leaf's downlink leaves the parent -
 *	or, when the leaf is its parent's only child, the downlink of the highest
 *	page of the chain of only children above it (the "top parent") leaves the
 *	page above that - by pointing the downlink at the right sibling of the
 *	subtree instead and deleting that sibling's own downlink, so the sibling
 *	takes over the subtree's key range.  When the subtree's downlink is the
 *	parent's first, the key-less minus-infinity one every internal page
 *	begins with, it stays key-less, and the sibling's range begins at the
 *	parent's lower bound.  A parent of two downlinks is left with one, which
 *	the height bound allows for (LION_MAX_DIR_HEIGHT).  The leaf's high key
 *	becomes a MINUS INFINITY pivot, and its owner_head names the next page
 *	of the subtree to unlink.  One record: the parent and the leaf.
 *
 *	Stage 2 unlinks the subtree's pages from their levels one at a time, top
 *	parent first and the leaf last: each one's left and right siblings are
 *	linked to each other, and the page becomes DELETED - its kind, level and
 *	links kept, its items replaced by a MINUS INFINITY high key, its safexid
 *	in the special area - in one record of the left sibling, the page, the
 *	right sibling and the leaf.
 *
 * The MINUS INFINITY high key is what lets every reader stay as it is.  A
 * reader that stands on such a page, or lands on it through a link it read
 * before the page went, compares its key with the high key, finds the high key
 * below it, and moves right - which is where the page's key range went - as it
 * moves right past a concurrent split.  A walk along the leaves finds no item
 * on it and follows the right link, which a deleted page keeps.  Only the
 * walks that do not compare keys need to know: the descent to the leftmost
 * leaf and the walk left (lion_dir_past_deleted(), lion_dir_step_left()).
 *
 * A DELETED page is not reused until no scan can still hold a link to it:
 * its safexid is the next transaction id when it was unlinked, and
 * lion_alloc_page() takes it only once GlobalVisCheckRemovableFullXid() says
 * every snapshot is past it, nbtree's rule.  On a standby the reuse record
 * (lion_wal_log_reuse()) cancels the queries that may still hold one.
 *
 * What is never deleted: the root; the rightmost page of a level, whose key
 * range has no right sibling to go to; a page whose split never finished, or
 * whose right sibling has no downlink yet; and the LAST child of a parent
 * that has other children, whose range would have to go to a page under
 * another parent.  Such a leaf stays, empty, until its neighbours make it
 * deletable.
 *
 * LOCKING.  Pages are locked as every directory writer locks them: left to
 * right along a level, and a child before its parent (DESIGN.md §21).  Stage 1
 * holds the leaf EXCLUSIVE from its last check to its record, and with it
 * the parent it changes; the descents that find the parents hold one page at
 * a time.  An internal page of the subtree can gain an item only when a child
 * of it splits, and the only leaf below it is the empty one held, so the
 * chain of only children cannot change under stage 1.  Stage 2 locks the
 * leaf (when it is not the page being unlinked), then the left sibling, the
 * page and the right sibling, which is _bt_unlink_halfdead_page()'s order.
 * Only VACUUM deletes, and one VACUUM at a time runs on an index, so nothing
 * but splits moves under either stage, and a split only adds pages to the
 * right of the one it splits.
 *
 * Anything unexpected - a sibling link that does not agree, a parent that
 * cannot be found - ends the attempt without an ERROR: the leaf stays as it
 * is, a HALF_DEAD one is finished by the next VACUUM that walks it, and
 * lion_index_verify() is what reports damage.
 *
 * Every page a deletion reads but the meta page goes through the buffer
 * access strategy VACUUM gives it (info->strategy), as its walk reads the
 * leaves (lion_vacuum.c, lion_vac_read()).  A VACUUM that empties a run of
 * leaves reads each one again, with its siblings and its parents, and read
 * without the ring every one of them would stay in shared buffers.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/transam.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "utils/rel.h"

#include "lion.h"

/*
 * The page at blk, which a link says is a directory page at `level`, read
 * through strategy - VACUUM's (info->strategy), which every page a deletion
 * reads goes through - and locked in lockmode; an ERROR when it is not one.
 */
static Buffer
lion_dirdel_read(Relation index, BlockNumber blk, int lockmode, int level,
				 BufferAccessStrategy strategy)
{
	Buffer		buf;
	Page		page;

	if (!BlockNumberIsValid(blk) || blk == LION_METAPAGE_BLKNO)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\" links to an invalid block number",
						RelationGetRelationName(index))));

	buf = ReadBufferExtended(index, MAIN_FORKNUM, blk, RBM_NORMAL, strategy);
	LockBuffer(buf, lockmode);
	page = BufferGetPage(buf);
	if (PageIsNew(page) || PageGetSpecialSize(page) != LION_SPECIAL_SIZE ||
		LionPageGetOpaque(page)->page_id != LION_PAGE_ID ||
		(LionPageGetOpaque(page)->flags & (LION_PAGE_BUCKET | LION_PAGE_DIR)) == 0 ||
		(level >= 0 && LionPageGetOpaque(page)->level != (uint16) level) ||
		(!LionPageIsRightmost(page) &&
		 PageGetMaxOffsetNumber(page) < FirstOffsetNumber))
	{
		UnlockReleaseBuffer(buf);
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": block %u is not a directory page at level %d",
						RelationGetRelationName(index), blk, level)));
	}
	return buf;
}

/* Does the page hold no data item?  (Its high key does not count.) */
static bool
lion_dirdel_page_empty(Page page)
{
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	OffsetNumber off;

	for (off = lion_page_first_data(page); off <= maxoff; off++)
	{
		if (ItemIdIsUsed(PageGetItemId(page, off)))
			return false;
	}
	return true;
}

/* An empty leaf that stage 1 may take: see the file header for the rules. */
static bool
lion_dirdel_leaf_candidate(Page page)
{
	uint16		flags = LionPageGetOpaque(page)->flags;

	return (flags & LION_PAGE_BUCKET) != 0 &&
		(flags & (LION_PAGE_ROOT | LION_PAGE_INCOMPLETE_SPLIT |
				  LION_PAGE_DELETED | LION_PAGE_HALF_DEAD)) == 0 &&
		!LionPageIsRightmost(page) &&
		lion_dirdel_page_empty(page);
}

/* A private copy of the item at off, which the caller has vouched for. */
static LionEntryTuple *
lion_dirdel_copy_item(Page page, OffsetNumber off, Size *size)
{
	ItemId		iid = PageGetItemId(page, off);
	LionEntryTuple *copy;

	(void) lion_entry_at(page, iid);
	*size = ItemIdGetLength(iid);
	copy = (LionEntryTuple *) palloc(*size);
	memcpy(copy, PageGetItem(page, iid), *size);
	return copy;
}

/*
 * The page at `level` whose range holds the keys just BELOW sk, locked SHARE,
 * or InvalidBuffer when there is none.
 *
 * sk is the high key of the page whose parent is wanted, so that page's
 * downlink has a separator below sk, and the downlink after it - its right
 * sibling's - has sk itself.  A descent that moves right only past high keys
 * strictly below sk, and takes the last downlink whose separator is strictly
 * below it, lands on the page that holds the downlink, or - after a
 * concurrent split - on one to its left, from which the caller scans right.
 * (lion_dir_search() would land right of it: sk is where the right sibling's
 * range begins.)  Every page but the meta page is read through strategy.
 */
static Buffer
lion_dirdel_descend(Relation index, LionIndexState *ix,
					const LionSearchKey *sk, uint16 level,
					BufferAccessStrategy strategy)
{
	Buffer		buf = InvalidBuffer;
	uint32		height;
	int			tries;

	/*
	 * The root, as the meta page names it; a root split between the two reads
	 * makes the meta page name a taller directory, which is read again.
	 */
	for (tries = 0;; tries++)
	{
		BlockNumber root = lion_dir_root(index, ix, &height);

		if (height < level)
			return InvalidBuffer;
		buf = lion_dirdel_read(index, root, BUFFER_LOCK_SHARE, -1, strategy);
		if (LionPageIsRoot(BufferGetPage(buf)))
			break;
		UnlockReleaseBuffer(buf);
		if (tries >= 16)
			return InvalidBuffer;
		CHECK_FOR_INTERRUPTS();
	}

	for (;;)
	{
		Page		page = BufferGetPage(buf);
		uint16		plevel;
		OffsetNumber first;
		OffsetNumber off;
		BlockNumber child;
		LionRightWalk walk;

		/* right past a concurrent split: lion_dir_step_right(), but ringed */
		lion_rightwalk_init(&walk);
		while (!LionPageIsRightmost(page) &&
			   lion_cmp_entry(lion_dir_highkey(page), sk) < 0)
		{
			BlockNumber blk = BufferGetBlockNumber(buf);
			BlockNumber next = LionPageGetOpaque(page)->rightlink;
			int			slevel = LionPageGetOpaque(page)->level;

			lion_rightwalk_step(index, &walk, blk, next);
			UnlockReleaseBuffer(buf);
			CHECK_FOR_INTERRUPTS();
			buf = lion_dirdel_read(index, next, BUFFER_LOCK_SHARE, slevel,
								   strategy);
			page = BufferGetPage(buf);
		}

		plevel = LionPageGetOpaque(page)->level;
		if (plevel == level)
			return buf;
		if (plevel < level || LionPageIsLeaf(page))
		{
			UnlockReleaseBuffer(buf);
			return InvalidBuffer;
		}

		first = lion_page_first_data(page);
		if (first > PageGetMaxOffsetNumber(page))
		{
			UnlockReleaseBuffer(buf);
			return InvalidBuffer;
		}
		off = lion_dir_binsrch(page, sk);
		off = (off > first) ? OffsetNumberPrev(off) : first;
		child = lion_entry_head(lion_page_entry(page, off));

		/* the parent goes before the child is locked (lion_dir.c) */
		UnlockReleaseBuffer(buf);
		CHECK_FOR_INTERRUPTS();
		buf = lion_dirdel_read(index, child, BUFFER_LOCK_SHARE, plevel - 1,
							   strategy);
	}
}

/*
 * The page that holds the downlink of child, a page at childlevel whose high
 * key sk is, locked EXCLUSIVE, with *offp the downlink's offset; or
 * InvalidBuffer when it was not found, or is on a page whose own split never
 * finished, which stage 1 leaves alone.
 */
static Buffer
lion_dirdel_find_parent(Relation index, LionIndexState *ix,
						BlockNumber child, uint16 childlevel,
						const LionSearchKey *sk, OffsetNumber *offp,
						BufferAccessStrategy strategy)
{
	Buffer		buf;
	BlockNumber blk;
	LionRightWalk walk;

	buf = lion_dirdel_descend(index, ix, sk, childlevel + 1, strategy);
	if (!BufferIsValid(buf))
		return InvalidBuffer;
	blk = BufferGetBlockNumber(buf);
	UnlockReleaseBuffer(buf);

	/*
	 * Scan right, EXCLUSIVE, one page at a time.  The downlink is on a page
	 * whose range holds the child's lower bound, which is below sk, so a page
	 * whose high key is at or above sk is the last that can have it.
	 */
	lion_rightwalk_init(&walk);
	for (;;)
	{
		Page		page;
		OffsetNumber off;
		OffsetNumber maxoff;
		BlockNumber next;

		if (blk == child)
			return InvalidBuffer;	/* damage; verify() reports it */
		buf = lion_dirdel_read(index, blk, BUFFER_LOCK_EXCLUSIVE,
							   childlevel + 1, strategy);
		page = BufferGetPage(buf);

		if (!LionPageIsDeleted(page))
		{
			maxoff = PageGetMaxOffsetNumber(page);
			for (off = lion_page_first_data(page); off <= maxoff; off++)
			{
				if (lion_entry_head(lion_page_entry(page, off)) != child)
					continue;
				if (LionPageIncompleteSplit(page))
					break;
				*offp = off;
				return buf;
			}
			if (off <= maxoff)
				break;			/* found, on a flagged page */
		}

		if (LionPageIsRightmost(page) ||
			(!LionPageIsDeleted(page) &&
			 lion_cmp_entry(lion_dir_highkey(page), sk) >= 0))
			break;
		next = LionPageGetOpaque(page)->rightlink;
		UnlockReleaseBuffer(buf);
		lion_rightwalk_step(index, &walk, blk, next);
		blk = next;
		CHECK_FOR_INTERRUPTS();
	}

	UnlockReleaseBuffer(buf);
	return InvalidBuffer;
}

/*
 * Rewrite the page p of the open record as an empty page with flags and the
 * special area *opaque, holding nothing but a MINUS INFINITY high key: what a
 * HALF_DEAD leaf and a DELETED page look like.  hk is that key, built before
 * the record began.
 *
 * The caller registers p as initialised (LION_WALBUF_INIT): the record builds
 * the page from nothing, so redo zeroes it and replays the two operations,
 * and the record takes no full-page image of it.  Registered as an ordinary
 * change, it would take one at every rewrite: lion_init_page() zeroes its
 * LSN, which is then older than any checkpoint.
 */
static void
lion_dirdel_rewrite(LionWalState *xstate, Page p, const LionPageOpaqueData *opaque,
					const LionEntryTuple *hk, Size hksz)
{
	lion_init_page(p, opaque->flags);
	memcpy(LionPageGetOpaque(p), opaque, sizeof(LionPageOpaqueData));
	if (PageAddItemExtended(p, hk, hksz, FirstOffsetNumber, 0) !=
		FirstOffsetNumber)
		elog(PANIC, "lion index: could not put a high key on an empty page");
	lion_wal_op(xstate, p, LION_OP_INIT, 0, opaque->flags, opaque,
				sizeof(LionPageOpaqueData));
	lion_wal_op(xstate, p, LION_OP_ADD, FirstOffsetNumber, 0, hk, hksz);
}

/*
 * Take n pages off the meta page's count of directory pages, in a record of
 * its own with nothing else held: the meta page comes before the parent in a
 * split's lock order, so neither stage can take it.  The count is the
 * planner's (lion_plan_cost.c), and a crash between a deletion and this
 * record leaves it a few pages high.
 */
static void
lion_dirdel_count(Relation index, int n)
{
	Buffer		buf;
	Page		page;
	LionWalState *xstate;
	LionMetaPageData *meta;
	uint32		dirpages;

	if (n <= 0)
		return;

	buf = ReadBuffer(index, LION_METAPAGE_BLKNO);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buf);
	lion_meta_check_page(index, buf);
	dirpages = LionPageGetMeta(page)->dirpages;
	if (dirpages <= 1)
	{
		UnlockReleaseBuffer(buf);
		return;
	}
	dirpages = (dirpages > (uint32) n + 1) ? dirpages - (uint32) n : 1;

	xstate = lion_wal_begin(index);
	page = lion_wal_register_buffer(xstate, buf, LION_WALBUF_STD);
	meta = LionPageGetMeta(page);
	meta->dirpages = dirpages;
	lion_wal_op(xstate, page, LION_OP_META, 0, 0, meta,
				sizeof(LionMetaPageData));
	lion_wal_finish(xstate, LION_XLOG_META);
	UnlockReleaseBuffer(buf);
}

/*
 * Stage 1 for the empty leaf held EXCLUSIVE in leafbuf: find the subtree to
 * delete and its parent, take the subtree's downlink out, and mark the leaf
 * HALF_DEAD.  False when the leaf may not go now; leafbuf stays locked either
 * way.
 */
static bool
lion_dirdel_mark_halfdead(Relation index, LionIndexState *ix, Buffer leafbuf,
						  BufferAccessStrategy strategy)
{
	Page		leaf = BufferGetPage(leafbuf);
	BlockNumber leafblk = BufferGetBlockNumber(leafbuf);
	BlockNumber child = leafblk;
	uint16		childlevel = 0;
	BlockNumber top = leafblk;
	BlockNumber toprightsib = LionPageGetOpaque(leaf)->rightlink;
	LionEntryTuple *hkcopy;
	Size		hksz;
	LionSearchKey sk;
	Buffer		pbuf;
	Page		ppage;
	OffsetNumber poff;
	LionEntryTuple *retarget;
	Size		retargetsz;
	LionEntryTuple *minf;
	Size		minfsz;
	LionPageOpaqueData halfdead;
	LionWalState *xstate;
	Page		p;

	hkcopy = lion_dirdel_copy_item(leaf, FirstOffsetNumber, &hksz);
	lion_search_key_exact(ix, &sk, hkcopy);

	/*
	 * Up from the leaf for as long as the page whose parent is wanted is that
	 * parent's only child: the parent goes with it then.  Each parent is let
	 * go before the next one up is looked for, which nbtree's
	 * _bt_lock_subtree_parent() explains: an internal page gains an item only
	 * when a child of it splits, and the one leaf below all of them is held.
	 */
	for (;;)
	{
		OffsetNumber maxoff;

		pbuf = lion_dirdel_find_parent(index, ix, child, childlevel, &sk,
									   &poff, strategy);
		if (!BufferIsValid(pbuf))
			return false;
		ppage = BufferGetPage(pbuf);
		maxoff = PageGetMaxOffsetNumber(ppage);

		if (poff < maxoff)
			break;				/* the subtree's parent: it has a downlink right of it */

		/*
		 * The downlink is the parent's last.  Only when it is also its first,
		 * and the parent can go itself, can the subtree grow by the parent.
		 */
		if (poff != lion_page_first_data(ppage) ||
			LionPageIsRightmost(ppage) || LionPageIsRoot(ppage) ||
			LionPageIncompleteSplit(ppage) || LionPageIsDeleted(ppage))
		{
			UnlockReleaseBuffer(pbuf);
			return false;
		}

		top = BufferGetBlockNumber(pbuf);
		toprightsib = LionPageGetOpaque(ppage)->rightlink;
		child = top;
		childlevel++;
		pfree(hkcopy);
		hkcopy = lion_dirdel_copy_item(ppage, FirstOffsetNumber, &hksz);
		lion_search_key_exact(ix, &sk, hkcopy);
		UnlockReleaseBuffer(pbuf);
	}

	/*
	 * The downlink right of the subtree's has to be its right sibling's: that
	 * is the page that takes the subtree's range over.  It is not when the
	 * sibling's own split never finished, or when the sibling is a HALF_DEAD
	 * leaf whose downlink an earlier stage 1 took out already.
	 */
	if (lion_entry_head(lion_page_entry(ppage, OffsetNumberNext(poff))) !=
		toprightsib)
	{
		UnlockReleaseBuffer(pbuf);
		return false;
	}

	/* Everything the record writes is built before it begins (§25). */
	retarget = lion_dirdel_copy_item(ppage, poff, &retargetsz);
	lion_entry_set_head(retarget, toprightsib);
	minf = lion_make_pivot(NULL, LION_ENTRY_HIGHKEY, InvalidBlockNumber,
						   &minfsz);
	halfdead = *LionPageGetOpaque(leaf);
	halfdead.flags |= LION_PAGE_HALF_DEAD;
	halfdead.owner_head = (top == leafblk) ? InvalidBlockNumber : top;

	/*
	 * The leaf's range goes to its right sibling, and so do the predicate
	 * locks SERIALIZABLE readers took on it, a key there or a gap for one
	 * (DESIGN.md §9): a later insert in the range checks the sibling.  nbtree
	 * does this in _bt_mark_page_halfdead(), before its critical section, as
	 * here.
	 */
	lion_predlock_move(index, leafblk, LionPageGetOpaque(leaf)->rightlink);

	xstate = lion_wal_begin(index);
	p = lion_wal_register_buffer(xstate, pbuf, LION_WALBUF_STD);
	lion_wal_save_item(xstate, p, poff);
	if (!PageIndexTupleOverwrite(p, poff, retarget, retargetsz))
		elog(PANIC, "lion index: could not rewrite a downlink in place");
	lion_wal_op_replace(xstate, p, poff, retarget, retargetsz);
	PageIndexTupleDelete(p, OffsetNumberNext(poff));
	lion_wal_op(xstate, p, LION_OP_DELETE, OffsetNumberNext(poff), 0, NULL, 0);
	p = lion_wal_register_buffer(xstate, leafbuf, LION_WALBUF_INIT);
	lion_dirdel_rewrite(xstate, p, &halfdead, minf, minfsz);
	lion_wal_finish(xstate, LION_XLOG_UNLINK);

	UnlockReleaseBuffer(pbuf);
	pfree(retarget);
	pfree(minf);
	pfree(hkcopy);
	return true;
}

/*
 * Stage 2, one page: unlink the next page of the HALF_DEAD leaf's subtree -
 * its top parent, or the leaf itself when that is all that is left.  The leaf
 * is pinned in leafbuf and not locked, on entry and on return.  *done says the
 * leaf itself went; false when the page could not be unlinked, which leaves
 * the subtree for the next VACUUM.
 */
static bool
lion_dirdel_unlink(Relation index, Buffer leafbuf, bool *done,
				   BufferAccessStrategy strategy)
{
	BlockNumber leafblk = BufferGetBlockNumber(leafbuf);
	Page		leaf = BufferGetPage(leafbuf);
	BlockNumber target;
	BlockNumber leftsib;
	BlockNumber rightsib;
	BlockNumber nextchild = InvalidBlockNumber;
	uint16		level;
	bool		leaflocked = false;
	Buffer		lbuf = InvalidBuffer;
	Buffer		tbuf = InvalidBuffer;
	Buffer		rbuf = InvalidBuffer;
	Page		tpage;
	Page		rpage;
	LionEntryTuple *minf;
	Size		minfsz;
	LionPageOpaqueData deleted;
	FullTransactionId safexid;
	LionWalState *xstate;
	Page		p;
	bool		ok = false;

	*done = false;

	/* Which page goes next, and its left sibling as it says. */
	LockBuffer(leafbuf, BUFFER_LOCK_SHARE);
	if (!LionPageIsHalfDead(leaf) || LionPageIsDeleted(leaf))
	{
		LockBuffer(leafbuf, BUFFER_LOCK_UNLOCK);
		*done = true;
		return false;
	}
	target = LionPageGetOpaque(leaf)->owner_head;
	leftsib = LionPageGetOpaque(leaf)->leftlink;
	LockBuffer(leafbuf, BUFFER_LOCK_UNLOCK);

	if (!BlockNumberIsValid(target) || target == leafblk)
	{
		target = leafblk;
		level = 0;
	}
	else
	{
		tbuf = lion_dirdel_read(index, target, BUFFER_LOCK_SHARE, -1,
								strategy);
		tpage = BufferGetPage(tbuf);
		leftsib = LionPageGetOpaque(tpage)->leftlink;
		level = LionPageGetOpaque(tpage)->level;
		UnlockReleaseBuffer(tbuf);
		tbuf = InvalidBuffer;
		if (level == 0)
			return false;		/* damage; verify() reports it */

		/* the leaf first, then the target's level left to right */
		LockBuffer(leafbuf, BUFFER_LOCK_EXCLUSIVE);
		leaflocked = true;
	}

	/*
	 * The left sibling: the page the link names, or - after a split of it -
	 * the last page that split made, found by walking right until a page's
	 * right link is the target.
	 */
	if (BlockNumberIsValid(leftsib))
	{
		LionRightWalk walk;

		lion_rightwalk_init(&walk);
		lbuf = lion_dirdel_read(index, leftsib, BUFFER_LOCK_EXCLUSIVE, level,
								strategy);
		for (;;)
		{
			Page		lpage = BufferGetPage(lbuf);
			BlockNumber next = LionPageGetOpaque(lpage)->rightlink;

			if (!LionPageIsDeleted(lpage) && next == target)
				break;
			if (LionPageIsRightmost(lpage) || LionPageIsDeleted(lpage) ||
				next == leftsib || next == leafblk)
			{
				ereport(LOG,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("lion index \"%s\": no left sibling of directory block %u to unlink it from",
								RelationGetRelationName(index), target)));
				goto out;
			}
			UnlockReleaseBuffer(lbuf);
			lbuf = InvalidBuffer;
			lion_rightwalk_step(index, &walk, leftsib, next);
			leftsib = next;
			lbuf = lion_dirdel_read(index, leftsib, BUFFER_LOCK_EXCLUSIVE,
									level, strategy);
		}
	}

	/* The target itself. */
	if (target == leafblk)
	{
		LockBuffer(leafbuf, BUFFER_LOCK_EXCLUSIVE);
		leaflocked = true;
		tbuf = leafbuf;
	}
	else
		tbuf = lion_dirdel_read(index, target, BUFFER_LOCK_EXCLUSIVE, level,
								strategy);
	tpage = BufferGetPage(tbuf);

	if (LionPageIsRightmost(tpage) || LionPageIsRoot(tpage) ||
		LionPageIsDeleted(tpage) ||
		LionPageGetOpaque(tpage)->leftlink != leftsib)
	{
		ereport(LOG,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": directory block %u changed while it was being unlinked",
						RelationGetRelationName(index), target)));
		goto out;
	}
	if (target == leafblk)
	{
		if (!LionPageIsHalfDead(tpage) || !lion_dirdel_page_empty(tpage))
			goto out;
	}
	else
	{
		OffsetNumber first = lion_page_first_data(tpage);

		/* one downlink: the next page of the subtree */
		if (first != PageGetMaxOffsetNumber(tpage))
		{
			ereport(LOG,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("lion index \"%s\": directory block %u is not a page of one child, as the subtree being deleted needs",
							RelationGetRelationName(index), target)));
			goto out;
		}
		nextchild = lion_entry_head(lion_page_entry(tpage, first));
		if (nextchild == leafblk)
			nextchild = InvalidBlockNumber;
	}

	/* The right sibling, whose left link must be the target. */
	rightsib = LionPageGetOpaque(tpage)->rightlink;
	if (rightsib == leafblk || rightsib == target ||
		(BufferIsValid(lbuf) && rightsib == BufferGetBlockNumber(lbuf)))
		goto out;
	rbuf = lion_dirdel_read(index, rightsib, BUFFER_LOCK_EXCLUSIVE, level,
							strategy);
	rpage = BufferGetPage(rbuf);
	if (LionPageGetOpaque(rpage)->leftlink != target ||
		LionPageIsDeleted(rpage))
	{
		ereport(LOG,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": the right sibling %u of directory block %u does not link back to it",
						RelationGetRelationName(index), rightsib, target)));
		goto out;
	}

	minf = lion_make_pivot(NULL, LION_ENTRY_HIGHKEY, InvalidBlockNumber,
						   &minfsz);

	/*
	 * Any scan that still holds a link to the target has an xmin at or below
	 * this, which is nbtree's argument for ReadNextFullTransactionId(): the
	 * links to the target are gone once the record below is written.
	 */
	safexid = ReadNextFullTransactionId();
	deleted = *LionPageGetOpaque(tpage);
	deleted.flags = (uint16) ((deleted.flags & (LION_PAGE_BUCKET | LION_PAGE_DIR)) |
							  LION_PAGE_DELETED);
	deleted.owner_hash = 0;
	deleted.owner_head = InvalidBlockNumber;
	{
		uint64		x = U64FromFullTransactionId(safexid);

		deleted.minckey = (uint32) x;
		deleted.maxckey = (uint32) (x >> 32);
	}

	xstate = lion_wal_begin(index);
	if (BufferIsValid(lbuf))
	{
		p = lion_wal_register_buffer(xstate, lbuf, LION_WALBUF_STD);
		LionPageGetOpaque(p)->rightlink = rightsib;
		lion_wal_log_special(xstate, p);
	}
	p = lion_wal_register_buffer(xstate, tbuf, LION_WALBUF_INIT);
	lion_dirdel_rewrite(xstate, p, &deleted, minf, minfsz);
	p = lion_wal_register_buffer(xstate, rbuf, LION_WALBUF_STD);
	LionPageGetOpaque(p)->leftlink = leftsib;
	lion_wal_log_special(xstate, p);
	if (target != leafblk)
	{
		p = lion_wal_register_buffer(xstate, leafbuf, LION_WALBUF_STD);
		LionPageGetOpaque(p)->owner_head = nextchild;
		lion_wal_log_special(xstate, p);
	}
	lion_wal_finish(xstate, LION_XLOG_UNLINK);

	pfree(minf);
	ok = true;
	*done = (target == leafblk);

out:
	if (BufferIsValid(rbuf))
		UnlockReleaseBuffer(rbuf);
	if (BufferIsValid(tbuf) && tbuf != leafbuf)
		UnlockReleaseBuffer(tbuf);
	if (BufferIsValid(lbuf))
		UnlockReleaseBuffer(lbuf);
	if (leaflocked)
		LockBuffer(leafbuf, BUFFER_LOCK_UNLOCK);
	return ok;
}

int
lion_dir_delete_page(Relation index, LionIndexState *ix, BlockNumber leafblk,
					 BufferAccessStrategy strategy)
{
	Buffer		leafbuf;
	Page		leaf;
	int			ndeleted = 0;
	bool		halfdead;

	/* A look first, SHARE: nearly every leaf VACUUM walks is not empty. */
	leafbuf = lion_dirdel_read(index, leafblk, BUFFER_LOCK_SHARE, 0,
							   strategy);
	leaf = BufferGetPage(leafbuf);
	halfdead = LionPageIsHalfDead(leaf) && !LionPageIsDeleted(leaf);
	if (!halfdead && !lion_dirdel_leaf_candidate(leaf))
	{
		UnlockReleaseBuffer(leafbuf);
		return 0;
	}

	if (!halfdead)
	{
		bool		marked;

		LockBuffer(leafbuf, BUFFER_LOCK_UNLOCK);
		LockBuffer(leafbuf, BUFFER_LOCK_EXCLUSIVE);
		marked = lion_dirdel_leaf_candidate(leaf) &&
			lion_dirdel_mark_halfdead(index, ix, leafbuf, strategy);
		LockBuffer(leafbuf, BUFFER_LOCK_UNLOCK);
		if (!marked)
		{
			ReleaseBuffer(leafbuf);
			return 0;
		}

		/*
		 * Test hook: the leaf is HALF_DEAD and still linked, the subtree's
		 * downlink is gone, and nothing is held.
		 * test/isolation/dir_page_delete.spec parks here.
		 */
		LION_INJECTION_POINT("lion-dir-delete-halfdead");
	}
	else
		LockBuffer(leafbuf, BUFFER_LOCK_UNLOCK);

	/* Stage 2: the subtree's pages, top parent first, the leaf last. */
	for (;;)
	{
		bool		done;

		CHECK_FOR_INTERRUPTS();
		if (!lion_dirdel_unlink(index, leafbuf, &done, strategy))
			break;
		ndeleted++;
		if (done)
			break;

		/*
		 * Test hook: one page of the subtree is DELETED, the leaf and the
		 * rest of it are not yet, and nothing is held.
		 */
		LION_INJECTION_POINT("lion-dir-delete-unlinked");
	}

	ReleaseBuffer(leafbuf);
	lion_dirdel_count(index, ndeleted);
	return ndeleted;
}

/*
 * buf, or the first page right of it that is not DELETED: for a walk that
 * took a downlink without comparing a key with the high key of the page it
 * landed on - the descent to the leftmost page of a level - and so has to
 * move off a page deleted under it by hand.  A DELETED page keeps the right
 * link it had, and the page it names took its range over.  Each page right
 * of buf is read through strategy: NULL, or VACUUM's for the descent its walk
 * starts with (lion_dir_leftmost_leaf_ext()).
 */
Buffer
lion_dir_past_deleted(Relation index, Buffer buf, int lockmode,
					  BufferAccessStrategy strategy)
{
	LionRightWalk walk;

	lion_rightwalk_init(&walk);
	while (LionPageIsDeleted(BufferGetPage(buf)))
	{
		if (LionPageIsRightmost(BufferGetPage(buf)))
		{
			BlockNumber blk = BufferGetBlockNumber(buf);

			UnlockReleaseBuffer(buf);
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("lion index \"%s\": deleted directory page %u has no right sibling",
							RelationGetRelationName(index), blk)));
		}
		buf = lion_dir_step_right_ext(index, buf, lockmode, &walk, strategy);
	}
	return buf;
}
