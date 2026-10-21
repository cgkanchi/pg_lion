/*-------------------------------------------------------------------------
 *
 * lion_posting_delete.c
 *		Taking an emptied leaf out of a key's posting set (DESIGN.md §22,
 *		"Empty leaves").
 *
 * VACUUM empties the leaves of a posting set whose rows are gone - the
 * oldest rows of a key that keeps getting new ones are a run of leaves at the
 * set's low end - and a leaf it leaves empty is taken out of the set here,
 * while the key lives on, so that readers do not walk it and its block goes
 * back to the free space map.  The scheme is nbtree's page deletion cut down
 * to what a posting set needs:
 *
 *	One record takes the leaf L out of both of the set's structures: its left
 *	neighbour's right link skips it, and in the parent its downlink is pointed
 *	at its right sibling R, whose own downlink - the next one on the same
 *	parent page - goes.  R takes L's key range over, as nbtree's right
 *	sibling does.  L is flagged UNLINKED and keeps everything a reader needs
 *	to pass through it: it is still a container page of the set, owner stamps
 *	and all, and still links to R.  Its leftlink, which a posting leaf does
 *	not otherwise use, holds the next transaction id at the time.
 *
 *	A reader that held a link to L - a right link it read before, or a
 *	downlink it is about to follow - lands on an empty leaf of its set, and
 *	empty leaves were always walked past: maxckey 0 is below every key, so a
 *	descent moves right, and a walk finds no item and follows the right link
 *	to R, which is where L's range went.
 *
 *	L is handed out again only once no scan can still hold a link to it: the
 *	leak sweep of a later VACUUM records it in the free space map when its
 *	transaction id is behind every snapshot (GlobalVisCheckRemovableFullXid(),
 *	nbtree's rule), and lion_alloc_page() checks that again and announces the
 *	reuse to a standby (lion_wal_log_reuse()), whose queries may hold such a
 *	link for longer.  Until then the page is exactly what it was, an empty
 *	leaf, on a standby as on the primary.
 *
 * What is never taken out: the set's root (a flat set's first leaf is its
 * root); the rightmost leaf, whose range has no right sibling to go to and
 * which is the entry's `tail`; a leaf whose split, or whose left neighbour's
 * split, never finished; and in a set with an internal level, a leaf whose
 * downlink is the last on its parent page, whose range would have to go to a
 * page under another parent.  Internal pages are not deleted while the set
 * lives.  Such a leaf stays, empty, and the key's next rows in its range go
 * into it.
 *
 * LOCKING.  The caller holds the key's directory leaf EXCLUSIVE throughout,
 * which every writer of the set does too (lion_posting.c, rule 2), and only
 * VACUUM takes a leaf out, so the set cannot change shape meanwhile: the
 * pages involved are found with SHARE locks, one at a time, and then taken
 * EXCLUSIVE left to right and the parent last - a child before its parent,
 * as a split takes them - and checked again.  Readers take one page at a
 * time.  Anything unexpected ends the attempt without an ERROR, leaving the
 * leaf as it is; lion_index_verify() is what reports damage.
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
 * The page at blk locked in mode when it is a live leaf of the set (head,
 * hash) - not one already taken out - else InvalidBuffer, nothing held.
 */
static Buffer
lion_pdel_leaf(Relation index, BlockNumber blk, uint32 hash, BlockNumber head,
			   int mode)
{
	Buffer		buf;
	Page		page;

	if (!BlockNumberIsValid(blk) || blk == LION_METAPAGE_BLKNO ||
		blk >= RelationGetNumberOfBlocks(index))
		return InvalidBuffer;

	buf = ReadBuffer(index, blk);
	LockBuffer(buf, mode);
	page = BufferGetPage(buf);
	if (!lion_page_owns_entry(page, hash, head) ||
		!LionPageIsPostingLeaf(page) || LionPageIsUnlinked(page))
	{
		UnlockReleaseBuffer(buf);
		return InvalidBuffer;
	}
	return buf;
}

/*
 * The leaf whose right link is blk, found by walking right from start, or
 * InvalidBlockNumber when start is blk itself (blk is the leftmost leaf).
 * *found is false when the walk does not get to blk.
 */
static BlockNumber
lion_pdel_left_of(Relation index, uint32 hash, BlockNumber head,
				  BlockNumber start, BlockNumber blk, bool *found)
{
	BlockNumber cur = start;
	LionRightWalk walk;

	*found = false;
	if (cur == blk)
	{
		*found = true;
		return InvalidBlockNumber;
	}

	lion_rightwalk_init(&walk);
	for (;;)
	{
		Buffer		buf = lion_pdel_leaf(index, cur, hash, head,
										 BUFFER_LOCK_SHARE);
		BlockNumber next;

		if (!BufferIsValid(buf))
			return InvalidBlockNumber;
		next = LionPageGetOpaque(BufferGetPage(buf))->rightlink;
		UnlockReleaseBuffer(buf);

		if (next == blk)
		{
			*found = true;
			return cur;
		}
		if (!BlockNumberIsValid(next))
			return InvalidBlockNumber;
		lion_rightwalk_step(index, &walk, cur, next);
		cur = next;
		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * The level-1 page that holds blk's downlink, from where the separators
 * route routekey rightwards, and the downlink's offset: InvalidBlockNumber
 * when the level ends first.
 */
static BlockNumber
lion_pdel_find_parent(Relation index, uint32 hash, BlockNumber head,
					  uint32 routekey, BlockNumber blk, OffsetNumber *offp)
{
	Buffer		buf;
	LionRightWalk walk;

	buf = lion_posting_search_level(index, head, routekey, 1, false);
	if (!BufferIsValid(buf))
		return InvalidBlockNumber;

	lion_rightwalk_init(&walk);
	for (;;)
	{
		Page		page = BufferGetPage(buf);
		BlockNumber pblk = BufferGetBlockNumber(buf);
		OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
		OffsetNumber off;
		BlockNumber next;

		if (LionPageGetOpaque(page)->level != 1 ||
			LionPageGetOpaque(page)->owner_hash != hash)
			break;
		for (off = lion_posting_first_data(page); off <= maxoff; off++)
		{
			if (lion_posting_downlink(index, page, pblk, off) == blk)
			{
				UnlockReleaseBuffer(buf);
				*offp = off;
				return pblk;
			}
		}

		next = LionPageGetOpaque(page)->rightlink;
		if (!BlockNumberIsValid(next))
			break;
		lion_rightwalk_step(index, &walk, pblk, next);
		UnlockReleaseBuffer(buf);
		CHECK_FOR_INTERRUPTS();

		buf = ReadBuffer(index, next);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		if (!lion_page_owns_entry(BufferGetPage(buf), hash, head))
			break;
	}

	UnlockReleaseBuffer(buf);
	return InvalidBlockNumber;
}

bool
lion_posting_unlink_leaf(Relation index, uint32 hash, BlockNumber head,
						 BlockNumber hint, BlockNumber blk)
{
	Buffer		buf;
	bool		flat;
	bool		found;
	BlockNumber start = InvalidBlockNumber;
	BlockNumber lf;
	BlockNumber pblk = InvalidBlockNumber;
	OffsetNumber poff = InvalidOffsetNumber;
	uint32		routekey = 0;
	Buffer		lfbuf = InvalidBuffer;
	Buffer		lbuf;
	Buffer		pbuf = InvalidBuffer;
	Page		lpage;
	BlockNumber rblk;
	LionPostingPivot retarget = {0};
	FullTransactionId xid;
	LionWalState *xstate;
	Page		p;

	if (blk == head)
		return false;

	/* Is the set flat - its head its first leaf - or a tree? */
	buf = ReadBuffer(index, head);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	if (!lion_page_owns_entry(BufferGetPage(buf), hash, head))
	{
		UnlockReleaseBuffer(buf);
		return false;
	}
	flat = LionPageIsPostingLeaf(BufferGetPage(buf));
	UnlockReleaseBuffer(buf);

	/*
	 * The left neighbour, from the leaf the caller's walk saw before blk when
	 * that is still a leaf of the set - a root push-down turns a flat set's
	 * head into an internal page - else from the set's leftmost leaf.  Pages
	 * only ever enter the chain to the right of the page that splits, so the
	 * neighbour is that leaf or to its right.
	 */
	if (BlockNumberIsValid(hint))
	{
		buf = lion_pdel_leaf(index, hint, hash, head, BUFFER_LOCK_SHARE);
		if (BufferIsValid(buf))
		{
			start = hint;
			UnlockReleaseBuffer(buf);
		}
	}
	if (!BlockNumberIsValid(start))
		start = flat ? head : lion_posting_leftmost_leaf(index, hash, head);
	if (!BlockNumberIsValid(start))
		return false;
	lf = lion_pdel_left_of(index, hash, head, start, blk, &found);
	if (!found || (flat && !BlockNumberIsValid(lf)))
		return false;

	/*
	 * The parent, routed by the left neighbour's last key - the downlink is on
	 * the page that holds the neighbour's or one to its right - or from the
	 * level's leftmost page when the neighbour is empty or there is none.
	 */
	if (!flat)
	{
		if (BlockNumberIsValid(lf))
		{
			buf = lion_pdel_leaf(index, lf, hash, head, BUFFER_LOCK_SHARE);
			if (!BufferIsValid(buf))
				return false;
			if (PageGetMaxOffsetNumber(BufferGetPage(buf)) >= FirstOffsetNumber)
				routekey = LionPageGetOpaque(BufferGetPage(buf))->maxckey;
			UnlockReleaseBuffer(buf);
		}
		pblk = lion_pdel_find_parent(index, hash, head, routekey, blk, &poff);
		if (!BlockNumberIsValid(pblk))
			return false;
	}

	/* Left to right, then the parent; every check made again. */
	if (BlockNumberIsValid(lf))
	{
		lfbuf = lion_pdel_leaf(index, lf, hash, head, BUFFER_LOCK_EXCLUSIVE);
		if (!BufferIsValid(lfbuf))
			return false;
		if (LionPageGetOpaque(BufferGetPage(lfbuf))->rightlink != blk ||
			LionPageIncompleteSplit(BufferGetPage(lfbuf)))
		{
			UnlockReleaseBuffer(lfbuf);
			return false;
		}
	}

	lbuf = lion_pdel_leaf(index, blk, hash, head, BUFFER_LOCK_EXCLUSIVE);
	if (!BufferIsValid(lbuf))
	{
		if (BufferIsValid(lfbuf))
			UnlockReleaseBuffer(lfbuf);
		return false;
	}
	lpage = BufferGetPage(lbuf);
	rblk = LionPageGetOpaque(lpage)->rightlink;
	if (PageGetMaxOffsetNumber(lpage) != 0 || !BlockNumberIsValid(rblk) ||
		LionPageIncompleteSplit(lpage))
	{
		UnlockReleaseBuffer(lbuf);
		if (BufferIsValid(lfbuf))
			UnlockReleaseBuffer(lfbuf);
		return false;
	}

	if (!flat)
	{
		Page		ppage;
		OffsetNumber maxoff;

		pbuf = ReadBuffer(index, pblk);
		LockBuffer(pbuf, BUFFER_LOCK_EXCLUSIVE);
		ppage = BufferGetPage(pbuf);
		maxoff = PageGetMaxOffsetNumber(ppage);

		/*
		 * The downlink right of L's is R's, on the same page: R takes L's
		 * range over by L's downlink, whose separator is L's lower bound.
		 */
		if (!lion_page_owns_entry(ppage, hash, head) ||
			LionPageGetOpaque(ppage)->level != 1 ||
			poff < lion_posting_first_data(ppage) || poff >= maxoff ||
			lion_posting_downlink(index, ppage, pblk, poff) != blk ||
			lion_posting_downlink(index, ppage, pblk,
								  OffsetNumberNext(poff)) != rblk)
		{
			UnlockReleaseBuffer(pbuf);
			UnlockReleaseBuffer(lbuf);
			if (BufferIsValid(lfbuf))
				UnlockReleaseBuffer(lfbuf);
			return false;
		}
		lion_page_check_items(index, ppage, pblk);
		retarget = *lion_posting_pivot(ppage, poff);
		retarget.child = rblk;
	}

	/* Everything fallible is done; one record (DESIGN.md §25). */
	xid = ReadNextFullTransactionId();
	xstate = lion_wal_begin(index);
	if (BufferIsValid(lfbuf))
	{
		p = lion_wal_register_buffer(xstate, lfbuf, LION_WALBUF_STD);
		LionPageGetOpaque(p)->rightlink = rblk;
		lion_wal_log_special(xstate, p);
	}
	p = lion_wal_register_buffer(xstate, lbuf, LION_WALBUF_STD);
	LionPageGetOpaque(p)->flags |= LION_PAGE_UNLINKED;
	LionPageGetOpaque(p)->leftlink = (BlockNumber) XidFromFullTransactionId(xid);
	lion_wal_log_special(xstate, p);
	if (BufferIsValid(pbuf))
	{
		p = lion_wal_register_buffer(xstate, pbuf, LION_WALBUF_STD);
		lion_wal_save_item(xstate, p, poff);
		if (!PageIndexTupleOverwrite(p, poff, &retarget,
									 LION_POSTING_PIVOT_SIZE))
			elog(PANIC, "lion index: could not rewrite a posting downlink in place");
		lion_wal_op_replace(xstate, p, poff, &retarget, LION_POSTING_PIVOT_SIZE);
		PageIndexTupleDelete(p, OffsetNumberNext(poff));
		lion_wal_op(xstate, p, LION_OP_DELETE, OffsetNumberNext(poff), 0,
					NULL, 0);
	}
	lion_wal_finish(xstate, LION_XLOG_UNLINK);

	if (BufferIsValid(pbuf))
		UnlockReleaseBuffer(pbuf);
	UnlockReleaseBuffer(lbuf);
	if (BufferIsValid(lfbuf))
		UnlockReleaseBuffer(lfbuf);
	return true;
}
