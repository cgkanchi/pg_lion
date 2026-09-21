/*-------------------------------------------------------------------------
 *
 * lion_vacuum.c
 *		ambulkdelete / amvacuumcleanup for the lion index
 *		(DESIGN.md sections 5 and 11).
 *
 * Three rules shape everything in this file.
 *
 * 1. Cleanup lock on every page that is visited, in chain order.
 *
 *	  ambulkdelete takes LockBufferForCleanup() on every bucket page and on
 *	  every container page of every chain it walks, whether or not that page
 *	  has anything to remove, and it takes them in chain order (bucket pages
 *	  first, then each container chain left to right).  This is the rule
 *	  nbtree's btvacuumscan follows, and it is what makes the interlock of
 *	  DESIGN.md section 9 airtight.  A reader pins page P, copies container C
 *	  out and drops the content lock; a concurrent insert may then split P and
 *	  move C onto a brand new page N linked immediately right of P.  Because
 *	  VACUUM cannot reach N without first holding a cleanup lock on P, and
 *	  that lock waits for the reader's pin, it cannot clean C on N while the
 *	  reader is still consulting the visibility map with its stale copy.  (If
 *	  VACUUM had already passed P when the reader took C, then C had already
 *	  been cleaned of this cycle's dead TIDs.)  Two properties make this
 *	  sufficient: pages are never recycled, so a new page is always right of
 *	  its split origin and is visited after it, and the set of dead TIDs is
 *	  fixed before index cleanup starts.  The same reason is why the
 *	  re-placement of a container that grew (lion_vacuum_regrow) walks right
 *	  from the page the container was filtered on, cleanup-locking each page
 *	  on the way, instead of jumping straight to the page that owns its ckey.
 *
 * 2. Never wait for a cleanup lock while holding another LWLock.
 *
 *	  A buffer content lock is an LWLock and holding one implies
 *	  HOLD_INTERRUPTS(), so such a wait would be uncancellable: a cursor that
 *	  keeps one container page pinned could freeze VACUUM -- deaf to
 *	  statement_timeout and pg_cancel_backend -- until the session owning the
 *	  cursor went away.  The rule also removes a deadlock, because buffer
 *	  LWLocks have no deadlock detector: a reader that pins a bucket page (an
 *	  INLINE entry) and then locks a container page closes a cycle with a
 *	  VACUUM that holds that container page and waits for the bucket head.
 *
 * 3. A TID leaves a page only under a cleanup lock on that page, and the
 *	  entry tuple's counters travel in the same WAL record as the containers
 *	  they count, so a crash can never desynchronise them.
 *
 * Hence each bucket is processed in two passes.
 *
 *	Pass 1 walks the bucket's pages.  Each page is cleanup-locked with nothing
 *	else held; its INLINE entries are filtered in place (a filtered payload
 *	can *grow* -- a run that loses every other member becomes a bitset -- so
 *	it may spill onto container pages exactly as an insert would); the offset
 *	and chain head of every CHAIN entry are noted; then the content lock is
 *	dropped while the pin is kept, so that pass 2 can re-lock the page
 *	cheaply.
 *
 *	Pass 2 walks each CHAIN entry's container chain, holding nothing between
 *	steps.  For every page X of the chain, including pages with nothing to
 *	remove:
 *	  1. LockBufferForCleanup(X) with nothing held, so the wait is
 *		 interruptible, and filter the page without changing it.
 *	  2. Nothing to remove: release X and move on; the entry page is not
 *		 needed and inserts are not disturbed.
 *	  3. Otherwise ConditionalLockBuffer(entry page) and, if that succeeds,
 *		 write the change and the entry in one record and release both.
 *	  4. If the entry page is busy, release X, take the entry page EXCLUSIVE
 *		 (blocking is fine with nothing else held), then
 *		 ConditionalLockBufferForCleanup(X); on success filter X again -- it
 *		 was unlocked in between -- and write; on failure release the entry
 *		 page and start over at 1.
 *
 * Inserts therefore interleave with pass 2, and that shapes the rest:
 *
 *	- An insert modifies an entry and its containers only while holding that
 *	  entry's bucket page EXCLUSIVE.  So everything VACUUM reads out of a
 *	  container page and everything it derives from that read must happen
 *	  inside one window in which both that page and the entry page are held:
 *	  containers are filtered and written back without releasing either lock
 *	  in between.
 *	- The entry is re-read from its page every time the entry page is locked,
 *	  and its counters are updated by applying deltas, never by writing back
 *	  absolute values computed in an earlier window.
 *	- A container that grew out of its slot has to be re-placed through the
 *	  chain machinery, which may split its page.  That happens in a window of
 *	  its own (lion_vacuum_regrow), which finds the page the container is on by
 *	  walking right under cleanup locks (rule 1) -- a concurrent insert, or the
 *	  re-placement of an earlier container, may have split the page and moved
 *	  this container right -- and re-reads and re-filters the container it
 *	  finds there.  Filtering is idempotent, so re-filtering costs nothing but
 *	  a callback call; reusing a copy taken in an earlier window would be
 *	  wrong, because an insert may have added a TID to it in the meantime.
 *	  Until that write happens the container still holds its dead TIDs, which
 *	  is exactly what rule 3 allows for: they leave the page under its cleanup
 *	  lock.
 *	- A split moves items only to brand new pages immediately to the right of
 *	  the page they came from, and only items this pass has already filtered,
 *	  so continuing the walk at the rightlink read at the start of the window
 *	  still visits everything that needs work, at most re-filtering containers
 *	  that have already been filtered.
 *	- Entries appended to a bucket while it is being vacuumed may be missed.
 *	  That is safe: they can only hold TIDs inserted after the heap scan that
 *	  produced the dead TID list, and a dead TID's line pointer cannot be
 *	  reused before every index has stopped pointing at it.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/generic_xlog.h"
#include "commands/vacuum.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "lion.h"

typedef struct LionVacState
{
	Relation	index;
	LionState   *state;
	IndexBulkDeleteCallback callback;
	void	   *callback_state;
	IndexBulkDeleteResult *stats;
	double		numtids;		/* sum of ntids over every entry */
	LionContainer *cbuf;			/* aligned container work buffer */
	MemoryContext pagecxt;		/* reset per container page and per entry */
} LionVacState;

/* Argument of the lion_container_remove_if() predicate. */
typedef struct LionVacPred
{
	uint32		ckey;
	IndexBulkDeleteCallback callback;
	void	   *callback_state;
} LionVacPred;

/* One item (container or sparse segment) to be written back to its page. */
typedef struct LionVacItem
{
	OffsetNumber off;			/* where it sits on the page */
	Size		size;			/* size of the filtered item */
	uint32		removed;		/* TIDs it lost */
	LionContainer *c;			/* the filtered item */
} LionVacItem;

/* What one filtering pass over a container page found. */
typedef struct LionVacWork
{
	LionVacItem *work;			/* containers to write back */
	OffsetNumber *delofs;		/* containers that lost every member */
	int			nwork;
	int			ndel;
	uint64		delremoved;		/* TIDs the deleted containers held */
} LionVacWork;

/* A CHAIN entry seen by pass 1, to be walked by pass 2. */
typedef struct LionVacChain
{
	OffsetNumber off;			/* offset of the entry on its bucket page */
	BlockNumber head;			/* first page of its container chain */
} LionVacChain;

static void lion_vacuum_bucket(LionVacState *vs, uint32 bucket);
static void lion_vacuum_inline(LionVacState *vs, Buffer buf, OffsetNumber off);
static void lion_vacuum_chain(LionVacState *vs, Buffer entrybuf,
							 OffsetNumber entryoff, BlockNumber head);
static BlockNumber lion_vacuum_container_page(LionVacState *vs, Buffer entrybuf,
											 OffsetNumber entryoff,
											 BlockNumber blk);
static BlockNumber lion_vacuum_filter_page(LionVacState *vs, Buffer buf,
										  LionVacWork *w);
static void lion_vacuum_apply_page(LionVacState *vs, Buffer entrybuf,
								  OffsetNumber entryoff, Buffer buf,
								  LionVacWork *w, uint32 **grownp,
								  int *ngrownp);
static void lion_vacuum_regrow(LionVacState *vs, Buffer entrybuf,
							  OffsetNumber entryoff, BlockNumber startblk,
							  uint32 ckey);
static LionEntryTuple *lion_vacuum_entry_copy(Buffer entrybuf,
											OffsetNumber entryoff, Size *size);

/*
 * The predicate handed to lion_container_remove_if(): map the container
 * coordinates back to a heap TID and ask the caller of ambulkdelete.
 */
static bool
lion_vac_is_dead(uint16 lo, void *arg)
{
	LionVacPred *pred = (LionVacPred *) arg;
	ItemPointerData tid;

	lion_code_to_tid(lion_make_code(pred->ckey, lo), &tid);

	return pred->callback(&tid, pred->callback_state);
}

/*
 * The same predicate for lion_sparse_remove_if(), whose pairs carry their own
 * container key (DESIGN.md §13).
 */
static bool
lion_vac_is_dead_pair(uint32 ckey, uint16 lo, void *arg)
{
	LionVacPred *pred = (LionVacPred *) arg;
	ItemPointerData tid;

	lion_code_to_tid(lion_make_code(ckey, lo), &tid);

	return pred->callback(&tid, pred->callback_state);
}

/*
 * Filter one item of a posting set in place and return how many TIDs it lost.
 *
 * A container may GROW while losing members (a run that loses every other
 * member becomes a bitset), which is what the regrow path exists for; a
 * sparse segment is a plain sorted list and can only shrink.
 */
static uint32
lion_vac_filter_item(LionContainer *item, LionVacPred *pred)
{
	if (item->type == LION_CT_SPARSE)
		return lion_sparse_remove_if(item, lion_vac_is_dead_pair, pred);

	pred->ckey = item->ckey;
	return lion_container_remove_if(item, lion_vac_is_dead, pred);
}

/* Pick the smallest representation of a filtered item. */
static void
lion_vac_optimize_item(LionContainer *item)
{
	if (item->type != LION_CT_SPARSE)
		lion_container_optimize(item);
}

/*
 * ambulkdelete
 */
IndexBulkDeleteResult *
lionbulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			  IndexBulkDeleteCallback callback, void *callback_state)
{
	Relation	index = info->index;
	LionVacState vs;
	MemoryContext vaccxt;
	MemoryContext oldcxt;
	uint32		b;

	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

	memset(&vs, 0, sizeof(vs));
	vs.index = index;
	vs.state = lion_get_state(index);
	vs.callback = callback;
	vs.callback_state = callback_state;
	vs.stats = stats;
	vs.numtids = 0;
	vs.cbuf = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);

	/*
	 * VACUUM does not run in a short-lived context, so all per-bucket work
	 * goes into a context that is reset between buckets, and the (possibly
	 * long) walk of a container chain into one that is reset per page.
	 */
	vaccxt = AllocSetContextCreate(CurrentMemoryContext,
								   "lion index vacuum",
								   ALLOCSET_DEFAULT_SIZES);
	vs.pagecxt = AllocSetContextCreate(CurrentMemoryContext,
									   "lion index vacuum page",
									   ALLOCSET_DEFAULT_SIZES);

	for (b = 0; b < vs.state->meta.nbuckets; b++)
	{
		oldcxt = MemoryContextSwitchTo(vaccxt);
		lion_vacuum_bucket(&vs, b);
		MemoryContextSwitchTo(oldcxt);
		MemoryContextReset(vaccxt);

		vacuum_delay_point(false);
	}

	MemoryContextDelete(vs.pagecxt);
	MemoryContextDelete(vaccxt);
	pfree(vs.cbuf);

	stats->num_pages = RelationGetNumberOfBlocks(index);
	stats->num_index_tuples = vs.numtids;
	stats->estimated_count = false;
	stats->pages_newly_deleted = 0;
	stats->pages_deleted = 0;
	stats->pages_free = 0;		/* v0 never recycles pages */

	return stats;
}

/*
 * amvacuumcleanup
 */
IndexBulkDeleteResult *
lionvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	if (info->analyze_only)
		return stats;

	if (stats == NULL)
	{
		/*
		 * No ambulkdelete call was needed, so nothing has been counted: hand
		 * back the heap's estimate rather than zero, which would tell the
		 * planner the index is empty.
		 */
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
		stats->num_pages = RelationGetNumberOfBlocks(info->index);
		stats->estimated_count = true;
		stats->num_index_tuples = info->num_heap_tuples >= 0 ?
			info->num_heap_tuples : 0;
	}

	return stats;
}

/*
 * Vacuum one bucket: pass 1 over its bucket pages, then pass 2 over the
 * container chains of the CHAIN entries pass 1 found (see the file header).
 */
static void
lion_vacuum_bucket(LionVacState *vs, uint32 bucket)
{
	BlockNumber blk = LION_BUCKET_BLKNO(bucket);

	while (BlockNumberIsValid(blk))
	{
		Buffer		buf;
		Page		page;
		BlockNumber next;
		OffsetNumber maxoff;
		OffsetNumber off;
		LionVacChain *chains;
		int			nchains = 0;
		int			i;

		/*
		 * Pass 1.  Every bucket page is cleanup-locked, whether or not it
		 * holds an INLINE entry that changes, and the lock is taken with
		 * nothing else held, so the wait for concurrent readers' pins is
		 * interruptible.
		 */
		buf = ReadBuffer(vs->index, blk);
		LockBufferForCleanup(buf);
		page = BufferGetPage(buf);

		if (!LionPageIsBucket(page))
		{
			UnlockReleaseBuffer(buf);
			elog(ERROR, "lion index: block %u is not a bucket page", blk);
		}

		next = LionPageGetOpaque(page)->rightlink;
		maxoff = PageGetMaxOffsetNumber(page);
		chains = (LionVacChain *) palloc(sizeof(LionVacChain) * (maxoff + 1));

		for (off = FirstOffsetNumber; off <= maxoff; off++)
		{
			ItemId		iid = PageGetItemId(page, off);
			LionEntryTuple *entry;

			if (!ItemIdIsUsed(iid))
				continue;

			entry = (LionEntryTuple *) PageGetItem(page, iid);

			if ((entry->flags & LION_ENTRY_INLINE) != 0)
				lion_vacuum_inline(vs, buf, off);
			else if ((entry->flags & LION_ENTRY_CHAIN) != 0)
			{
				if (!BlockNumberIsValid(entry->head) ||
					!BlockNumberIsValid(entry->tail))
				{
					UnlockReleaseBuffer(buf);
					elog(ERROR, "lion index: chain entry %u on block %u has no container pages",
						 off, blk);
				}
				chains[nchains].off = off;
				chains[nchains].head = entry->head;
				nchains++;
			}
			else
			{
				uint16		flags = entry->flags;

				UnlockReleaseBuffer(buf);
				elog(ERROR, "lion index: entry %u on block %u has invalid flags %u",
					 off, blk, flags);
			}

			CHECK_FOR_INTERRUPTS();
		}

		/* The page keeps its pin: pass 2 locks it again for every window. */
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);

		vacuum_delay_point(false);

		/* Pass 2, holding no page lock between its steps. */
		for (i = 0; i < nchains; i++)
			lion_vacuum_chain(vs, buf, chains[i].off, chains[i].head);

		ReleaseBuffer(buf);
		pfree(chains);

		blk = next;
	}
}

/*
 * Filter the payload of an INLINE entry.  The entry page is cleanup-locked
 * and nothing else is held (pass 1).
 *
 * The result usually shrinks, but a run container that loses every other
 * member becomes a bitset, so the payload can also grow past inline_limit or
 * past the free space of the page: then the posting set spills onto container
 * pages, exactly as an insert would.
 */
static void
lion_vacuum_inline(LionVacState *vs, Buffer buf, OffsetNumber off)
{
	Page		page = BufferGetPage(buf);
	ItemId		iid = PageGetItemId(page, off);
	LionEntryTuple *entry = (LionEntryTuple *) PageGetItem(page, iid);
	Size		paylen = LION_ENTRY_PAYLOAD_LEN(entry, ItemIdGetLength(iid));
	MemoryContext oldcxt;
	StringInfoData newpay;
	LionVacPred	pred;
	char	   *oldpay;
	Size		cur = 0;
	Size		csize;
	uint64		removed = 0;
	uint64		ntids = 0;
	uint32		ncontainers = 0;

	oldcxt = MemoryContextSwitchTo(vs->pagecxt);

	/* The payload is copied out: a spill rewrites the entry under our feet. */
	oldpay = (char *) palloc(paylen);
	memcpy(oldpay, LionEntryGetPayload(entry), paylen);

	pred.callback = vs->callback;
	pred.callback_state = vs->callback_state;

	initStringInfo(&newpay);
	while ((csize = lion_inline_fetch(oldpay, paylen, &cur, vs->cbuf)) > 0)
	{
		removed += lion_vac_filter_item(vs->cbuf, &pred);

		if (vs->cbuf->cardinality == 0)
			continue;			/* drop empty containers and segments */

		lion_vac_optimize_item(vs->cbuf);
		appendBinaryStringInfo(&newpay, (char *) vs->cbuf,
							   lion_item_size(vs->cbuf));
		ncontainers++;
		ntids += vs->cbuf->cardinality;
	}

	if (removed == 0)
	{
		/* Nothing to do: do not dirty the page. */
		vs->numtids += (double) entry->ntids;
		MemoryContextSwitchTo(oldcxt);
		MemoryContextReset(vs->pagecxt);
		return;
	}

	vs->stats->tuples_removed += (double) removed;
	vs->numtids += (double) ntids;

	if ((Size) newpay.len <= (Size) vs->state->meta.inline_limit)
	{
		LionEntryTuple *newentry;
		Size		newsize;
		bool		ok;

		newentry = lion_entry_rebuild(entry, newpay.data, (Size) newpay.len,
									 &newsize);
		newentry->ncontainers = ncontainers;
		newentry->ntids = ntids;

		ok = lion_replace_entry(vs->index, NULL, buf, off, newentry, newsize);
		if (ok)
		{
			MemoryContextSwitchTo(oldcxt);
			MemoryContextReset(vs->pagecxt);
			return;
		}
	}

	/* It no longer fits inline: move the posting set onto container pages. */
	{
		LionEntryTuple *chain;
		Size		chainsize;

		chain = lion_entry_rebuild(entry, NULL, 0, &chainsize);
		chain->ncontainers = ncontainers;
		chain->ntids = ntids;

		lion_entry_spill(vs->index, buf, off, chain, newpay.data,
						(Size) newpay.len);
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextReset(vs->pagecxt);
}

/*
 * Pass 2 for one CHAIN entry: walk its container chain from head, left to
 * right, holding no page lock between pages.  entrybuf is the entry's bucket
 * page, pinned by pass 1 and not locked; entryoff is stable because entries
 * are never deleted or moved.
 */
static void
lion_vacuum_chain(LionVacState *vs, Buffer entrybuf, OffsetNumber entryoff,
				 BlockNumber head)
{
	BlockNumber blk = head;
	LionEntryTuple *entry;

	while (BlockNumberIsValid(blk))
	{
		blk = lion_vacuum_container_page(vs, entrybuf, entryoff, blk);

		vacuum_delay_point(false);
	}

	/* Report what the entry holds now that every page has been visited. */
	LockBuffer(entrybuf, BUFFER_LOCK_SHARE);
	entry = (LionEntryTuple *) PageGetItem(BufferGetPage(entrybuf),
										  PageGetItemId(BufferGetPage(entrybuf),
														entryoff));
	vs->numtids += (double) entry->ntids;
	LockBuffer(entrybuf, BUFFER_LOCK_UNLOCK);
}

/*
 * Private copy of the CHAIN entry at (entrybuf, entryoff), which the caller
 * holds locked.  Concurrent inserts change the entry, so it is re-read from
 * the page inside every window and its counters are only ever updated with
 * deltas.
 */
static LionEntryTuple *
lion_vacuum_entry_copy(Buffer entrybuf, OffsetNumber entryoff, Size *size)
{
	Page		page = BufferGetPage(entrybuf);
	ItemId		iid = PageGetItemId(page, entryoff);
	LionEntryTuple *entry;

	if (!ItemIdIsUsed(iid))
		elog(ERROR, "lion index: entry %u on block %u is gone",
			 entryoff, BufferGetBlockNumber(entrybuf));

	entry = (LionEntryTuple *) PageGetItem(page, iid);
	if ((entry->flags & LION_ENTRY_CHAIN) == 0)
		elog(ERROR, "lion index: entry %u on block %u is no longer a chain entry",
			 entryoff, BufferGetBlockNumber(entrybuf));

	return lion_entry_rebuild(entry, NULL, 0, size);
}

/*
 * Vacuum one container page of a chain and return the block to continue at.
 * Nothing is held on entry or on return; see the file header for the lock
 * protocol this implements.
 */
static BlockNumber
lion_vacuum_container_page(LionVacState *vs, Buffer entrybuf,
						  OffsetNumber entryoff, BlockNumber blk)
{
	Buffer		buf;
	BlockNumber next;
	LionVacWork	w;
	uint32	   *grown = NULL;
	int			ngrown = 0;
	int			i;
	MemoryContext oldcxt;

	oldcxt = MemoryContextSwitchTo(vs->pagecxt);

	buf = ReadBuffer(vs->index, blk);

	for (;;)
	{
		/*
		 * Every page of the chain is visited under a cleanup lock, whether or
		 * not it turns out to have anything to remove.  Nothing else is held,
		 * so the wait for a reader's pin is interruptible.
		 */
		LockBufferForCleanup(buf);

		next = lion_vacuum_filter_page(vs, buf, &w);

		if (w.nwork == 0 && w.ndel == 0)
		{
			/* Nothing to remove: no WAL record, and no entry page needed. */
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			break;
		}

		if (ConditionalLockBuffer(entrybuf))
		{
			lion_vacuum_apply_page(vs, entrybuf, entryoff, buf, &w,
								  &grown, &ngrown);
			LockBuffer(entrybuf, BUFFER_LOCK_UNLOCK);
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			break;
		}

		/*
		 * An insert holds the entry page.  Waiting for it here would be a
		 * lock-order inversion, so let go of the page, take the entry page
		 * with nothing held and try the cleanup lock again.
		 */
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		LockBuffer(entrybuf, BUFFER_LOCK_EXCLUSIVE);

		if (ConditionalLockBufferForCleanup(buf))
		{
			/* The page was unlocked in between, so filter it again. */
			next = lion_vacuum_filter_page(vs, buf, &w);
			if (w.nwork > 0 || w.ndel > 0)
				lion_vacuum_apply_page(vs, entrybuf, entryoff, buf, &w,
									  &grown, &ngrown);
			LockBuffer(entrybuf, BUFFER_LOCK_UNLOCK);
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			break;
		}

		LockBuffer(entrybuf, BUFFER_LOCK_UNLOCK);
		CHECK_FOR_INTERRUPTS();
	}

	ReleaseBuffer(buf);

	/*
	 * Containers that grew out of their slot are re-placed one at a time,
	 * each in a window of its own, because placing one may split its page and
	 * move the others to the right.
	 */
	for (i = 0; i < ngrown; i++)
		lion_vacuum_regrow(vs, entrybuf, entryoff, blk, grown[i]);

	MemoryContextSwitchTo(oldcxt);
	MemoryContextReset(vs->pagecxt);

	return next;
}

/*
 * Filter every container of the cleanup-locked page buf into *w, without
 * changing the page.  Returns the block to continue the walk at.
 *
 * The rightlink is read here, before anything on this page can move: a split
 * only ever inserts brand new pages between this one and the old right
 * sibling, and everything that moves onto them has already been filtered
 * (except the containers that grew, which lion_vacuum_regrow() follows), so
 * continuing at the old rightlink visits every container that still needs
 * work.
 */
static BlockNumber
lion_vacuum_filter_page(LionVacState *vs, Buffer buf, LionVacWork *w)
{
	Page		page = BufferGetPage(buf);
	BlockNumber blk = BufferGetBlockNumber(buf);
	OffsetNumber maxoff;
	OffsetNumber off;
	LionVacPred	pred;

	if (!LionPageIsContainer(page))
		elog(ERROR, "lion index: block %u is not a container page", blk);

	maxoff = PageGetMaxOffsetNumber(page);

	w->nwork = 0;
	w->ndel = 0;
	w->delremoved = 0;
	w->work = (LionVacItem *) palloc(sizeof(LionVacItem) * (maxoff + 1));
	w->delofs = (OffsetNumber *) palloc(sizeof(OffsetNumber) * (maxoff + 1));

	pred.callback = vs->callback;
	pred.callback_state = vs->callback_state;

	for (off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId		iid = PageGetItemId(page, off);
		LionVacItem *item;
		Size		isize;
		uint32		nremoved;

		if (!ItemIdIsUsed(iid))
			continue;

		isize = ItemIdGetLength(iid);
		if (isize > LION_CONTAINER_MAX_SIZE)
			elog(ERROR, "lion index: item of %zu bytes at %u/%u",
				 isize, blk, off);
		memcpy(vs->cbuf, PageGetItem(page, iid), isize);

		nremoved = lion_vac_filter_item(vs->cbuf, &pred);
		if (nremoved == 0)
			continue;

		if (vs->cbuf->cardinality == 0)
		{
			/* Empty containers and empty segments both go away. */
			w->delofs[w->ndel++] = off;
			w->delremoved += nremoved;
			continue;
		}

		lion_vac_optimize_item(vs->cbuf);
		item = &w->work[w->nwork++];
		item->off = off;
		item->size = lion_item_size(vs->cbuf);
		item->removed = nremoved;
		item->c = (LionContainer *) palloc(item->size);
		memcpy(item->c, vs->cbuf, item->size);
	}

	return LionPageGetOpaque(page)->rightlink;
}

/*
 * Apply the result of lion_vacuum_filter_page() to the page: everything that
 * fits in place goes into one WAL record that also carries the entry tuple
 * with its new counters.  The caller holds a cleanup lock on buf and the
 * entry page EXCLUSIVE, and has not released either since the filtering.
 *
 * *grownp receives the ckeys of the containers that no longer fit their slot;
 * they are dealt with by lion_vacuum_regrow() once both locks are gone.
 */
static void
lion_vacuum_apply_page(LionVacState *vs, Buffer entrybuf, OffsetNumber entryoff,
					  Buffer buf, LionVacWork *w, uint32 **grownp, int *ngrownp)
{
	Relation	index = vs->index;
	LionEntryTuple *ecopy;
	Size		esize;
	GenericXLogState *xstate;
	Page		p;
	uint32	   *grown;
	int			ngrown = 0;
	int			i;
	uint64		removed = w->delremoved;

	Assert(w->nwork > 0 || w->ndel > 0);

	/* The entry may have been changed by inserts since the last window. */
	ecopy = lion_vacuum_entry_copy(entrybuf, entryoff, &esize);
	grown = (uint32 *) palloc(sizeof(uint32) * (w->nwork + 1));

	xstate = GenericXLogStart(index);
	p = GenericXLogRegisterBuffer(xstate, buf, 0);

	/*
	 * Empty containers go first, because the space they free may be what a
	 * container that grew needs.  PageIndexMultiDelete compacts the page
	 * itself (no PageRepairFragmentation needed) and renumbers the items
	 * after the deleted ones, so the offsets of what is left have to be
	 * adjusted.
	 */
	if (w->ndel > 0)
	{
		PageIndexMultiDelete(p, w->delofs, w->ndel);

		for (i = 0; i < w->nwork; i++)
		{
			int			shift = 0;
			int			j;

			for (j = 0; j < w->ndel; j++)
			{
				if (w->delofs[j] < w->work[i].off)
					shift++;
			}
			w->work[i].off -= shift;
		}
	}

	for (i = 0; i < w->nwork; i++)
	{
		if (PageIndexTupleOverwrite(p, w->work[i].off, w->work[i].c,
									w->work[i].size))
			removed += w->work[i].removed;
		else if (w->work[i].c->type == LION_CT_SPARSE)
		{
			/* A segment only ever shrinks, so its slot always holds it. */
			GenericXLogAbort(xstate);
			elog(ERROR, "lion index: filtered sparse segment %u on block %u no longer fits",
				 w->work[i].c->ckey, BufferGetBlockNumber(buf));
		}
		else
			grown[ngrown++] = w->work[i].c->ckey;
	}

	lion_page_update_minmax(p);

	Assert(ecopy->ntids >= removed);
	ecopy->ntids -= removed;
	Assert(ecopy->ncontainers >= (uint32) w->ndel);
	ecopy->ncontainers -= (uint32) w->ndel;

	if (!lion_replace_entry(index, xstate, entrybuf, entryoff, ecopy, esize))
		elog(ERROR, "lion index: could not update entry %u on block %u",
			 entryoff, BufferGetBlockNumber(entrybuf));

	GenericXLogFinish(xstate);

	vs->stats->tuples_removed += (double) removed;

	*grownp = grown;
	*ngrownp = ngrown;
}

/*
 * Re-place the container with this ckey, which no longer fits the slot it had
 * on page startblk.  Nothing is held on entry or on return.
 *
 * The page the container lives on now is found by walking right from startblk
 * under a cleanup lock on each page, never by jumping to it: a concurrent
 * insert (or the re-placement of an earlier container of the same page) may
 * have split a page and moved this container right, and cleaning it on the
 * new page without first holding the cleanup lock of every page before it
 * would let VACUUM slip past a reader that is holding a pin and a stale copy
 * of this very container (DESIGN.md section 11).
 *
 * The container is re-read and re-filtered under the locks, because an insert
 * may have added a TID to it since it was last read.
 */
static void
lion_vacuum_regrow(LionVacState *vs, Buffer entrybuf, OffsetNumber entryoff,
				  BlockNumber startblk, uint32 ckey)
{
	Relation	index = vs->index;
	BlockNumber blk = startblk;

	for (;;)
	{
		LionEntryTuple *ecopy;
		Size		esize;
		Buffer		buf;
		Page		page;
		ItemId		iid;
		OffsetNumber off;
		BlockNumber next;
		bool		found;
		uint32		nremoved;
		LionVacPred	pred;

		buf = ReadBuffer(index, blk);

		/* Rule 1: a cleanup lock on every page on the way, in chain order. */
		LockBufferForCleanup(buf);
		page = BufferGetPage(buf);
		if (!LionPageIsContainer(page))
		{
			UnlockReleaseBuffer(buf);
			elog(ERROR, "lion index: block %u is not a container page", blk);
		}

		off = lion_page_find_container(page, ckey, &found);
		next = LionPageGetOpaque(page)->rightlink;

		if (!found)
		{
			/*
			 * A split moved the container to the right.  Containers are only
			 * ever removed by this vacuum, so it is still in the chain.
			 */
			UnlockReleaseBuffer(buf);
			if (!BlockNumberIsValid(next))
				elog(ERROR, "lion index: container %u of entry %u on block %u vanished from its chain",
					 ckey, entryoff, BufferGetBlockNumber(entrybuf));
			blk = next;
			CHECK_FOR_INTERRUPTS();
			continue;
		}

		/* Changing it needs the entry page as well, and never a wait here. */
		if (!ConditionalLockBuffer(entrybuf))
		{
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			LockBuffer(entrybuf, BUFFER_LOCK_EXCLUSIVE);

			if (!ConditionalLockBufferForCleanup(buf))
			{
				LockBuffer(entrybuf, BUFFER_LOCK_UNLOCK);
				ReleaseBuffer(buf);
				CHECK_FOR_INTERRUPTS();
				continue;		/* start again at this same page */
			}

			/* The page was unlocked in between: locate the container again. */
			off = lion_page_find_container(page, ckey, &found);
			if (!found)
			{
				next = LionPageGetOpaque(page)->rightlink;
				LockBuffer(entrybuf, BUFFER_LOCK_UNLOCK);
				UnlockReleaseBuffer(buf);
				if (!BlockNumberIsValid(next))
					elog(ERROR, "lion index: container %u of entry %u on block %u vanished from its chain",
						 ckey, entryoff, BufferGetBlockNumber(entrybuf));
				blk = next;
				CHECK_FOR_INTERRUPTS();
				continue;
			}
		}

		/* Everything read from here on is used inside this window only. */
		ecopy = lion_vacuum_entry_copy(entrybuf, entryoff, &esize);

		iid = PageGetItemId(page, off);
		if (ItemIdGetLength(iid) > LION_CONTAINER_MAX_SIZE)
			elog(ERROR, "lion index: container of %zu bytes at %u/%u",
				 (Size) ItemIdGetLength(iid), blk, off);
		memcpy(vs->cbuf, PageGetItem(page, iid), ItemIdGetLength(iid));

		/*
		 * Only containers can grow while being filtered, and a container's
		 * ckey is never the first ckey of a segment (one ckey lives in one
		 * item), so the item found here must be the container itself.
		 */
		if (vs->cbuf->type == LION_CT_SPARSE)
		{
			LockBuffer(entrybuf, BUFFER_LOCK_UNLOCK);
			UnlockReleaseBuffer(buf);
			elog(ERROR, "lion index: container key %u of entry %u is now a sparse segment",
				 ckey, entryoff);
		}

		pred.ckey = ckey;
		pred.callback = vs->callback;
		pred.callback_state = vs->callback_state;
		nremoved = lion_container_remove_if(vs->cbuf, lion_vac_is_dead, &pred);

		if (nremoved > 0)
		{
			Assert(ecopy->ntids >= nremoved);
			ecopy->ntids -= nremoved;

			if (vs->cbuf->cardinality == 0)
			{
				GenericXLogState *xstate = GenericXLogStart(index);
				Page		p = GenericXLogRegisterBuffer(xstate, buf, 0);
				OffsetNumber delof = off;

				PageIndexMultiDelete(p, &delof, 1);
				lion_page_update_minmax(p);

				Assert(ecopy->ncontainers >= 1);
				ecopy->ncontainers -= 1;

				if (!lion_replace_entry(index, xstate, entrybuf, entryoff,
									   ecopy, esize))
					elog(ERROR, "lion index: could not update entry %u on block %u",
						 entryoff, BufferGetBlockNumber(entrybuf));

				GenericXLogFinish(xstate);
			}
			else
			{
				int			delta;

				lion_container_optimize(vs->cbuf);

				/*
				 * This writes the container and the entry in one record, and
				 * splits the page if the container still does not fit.
				 */
				lion_chain_put_container_locked(index, buf, entrybuf, entryoff,
											   ecopy, vs->cbuf, &delta);
				if (delta != 0)
					elog(ERROR, "lion index: container %u of entry %u on block %u vanished from its chain",
						 ckey, entryoff, BufferGetBlockNumber(entrybuf));
			}

			vs->stats->tuples_removed += (double) nremoved;
		}

		LockBuffer(entrybuf, BUFFER_LOCK_UNLOCK);
		UnlockReleaseBuffer(buf);
		pfree(ecopy);
		return;
	}
}
