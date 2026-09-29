/*-------------------------------------------------------------------------
 *
 * lion_vis.c
 *		The visibility map a container at a time, and the per-query
 *		visibility cache (DESIGN.md §9).
 *
 * Part of the count engine: lion_count.h is its interface, and
 * lion_count_int.h declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_count_int.h"

/* ---------------------------------------------------------------------
 * The visibility map, a container at a time
 * --------------------------------------------------------------------- */

/*
 * The all-visible bits of the LION_BLOCKS_PER_CONTAINER consecutive heap blocks
 * a container covers, as one mask: bit i is set iff heap block firstblk + i is
 * marked all-visible.  Only the bits of `wanted` are answered for - the caller
 * never looks at the others, and skipping them is what keeps a one-member
 * container to a single map byte.  *vmbuf is the caller's visibility map pin;
 * it is moved
 * to whatever map page is needed and left pinned for the next call, exactly as
 * visibilitymap_get_status() leaves it - and each move adds one to *pins, when
 * pins is not NULL (EXPLAIN ANALYZE's "Visibility Map Pages Pinned").
 *
 * This is visibilitymap_get_status() for a whole container at once.  It exists
 * because asking a block at a time costs a buffer-manager lookup per heap block
 * covered - 64 of them per container at 8K - and that was the entire cost of a
 * count over an all-visible table, where DESIGN.md section 9 wants O(1) work per
 * container.  The map bytes are read straight off the page with no lock, just as
 * visibilitymap_get_status() reads its one byte; its comment is the license:
 *
 *		"NOTE: This function is typically called without a lock on the heap
 *		 page, so somebody else could change the bit just after we look at it.
 *		 In fact, since we don't lock the visibility map page either, it's even
 *		 possible that someone else could have changed the bit just before we
 *		 look at it, but yet we might see the old value.  It is the caller's
 *		 responsibility to deal with all concurrency issues!"
 *
 * and DESIGN.md section 9 is where this file deals with them: a bit that is
 * stale in the "recently cleared" direction can only have been cleared by a
 * transaction whose tuples our snapshot cannot see (the index-only scan
 * argument, spelled out in that same comment), and a bit cannot become set
 * behind our back because the caller still pins the index page the container
 * was read from, which is what stops VACUUM finishing ambulkdelete().  Reading
 * sixteen adjacent bytes instead of one changes none of that: each byte is
 * still an independent unlocked read of the same page.
 *
 * The map fork is never extended.  visibilitymap_pin() would extend it - it
 * calls vm_readbuf() with extend = true, which writes - and a count has no
 * business growing the map, so the pin is taken the way
 * visibilitymap_get_status() takes it, and blocks the fork does not reach are
 * simply not all-visible.  That also covers the blocks past the end of the heap
 * that a container's range may include: they have no members, so nothing the
 * mask says about them is ever read.
 *
 * The work is split in three so that what the compiler sees at the call site is
 * the case that happens.  This runs once per container - tens of thousands of
 * times for one IN list - and all but one container in five hundred is answered
 * by reading a run of bytes off ONE map page; the loop that used to be here
 * spelled out the two-page case inline, and whether gcc inlined the whole thing
 * or none of it moved this query by a third in an -O1 build, in whichever
 * direction an unrelated edit to the file happened to push it.  Now the common
 * path is small and always inlined, and the straddling case is out of line.
 */


/*
 * The other half of the pair - which of the heap blocks a container covers
 * have at least one member, in the same bit numbering - is the container
 * library's lion_container_block_mask().  This file used to carry its own,
 * which took a damaged container's members and runs unmasked: past the range
 * they shifted by 64 or more, which is undefined and could drop a block with
 * members from the mask, and at BLCKSZ 16K and 32K they set bits past the
 * flags lion_count_container_vm() keeps per block (2026-09-27 review).
 */


#ifdef LION_VM_MASK_CHECK
/*
 * The comparison below on the blocks of ONE map page, with that page SHARE
 * locked.  `seg` are the wanted bits, shifted so that bit 0 is block blk.
 */
static void
lion_vm_mask_check_page(Relation heap, BlockNumber blk, uint64 seg)
{
	Buffer		vmbuf = InvalidBuffer;
	uint64		expect = 0;
	uint64		got;
	uint64		m = seg;

	if (seg == 0)
		return;

	/* pin the map page, the way visibilitymap_get_status() does */
	(void) visibilitymap_get_status(heap, blk, &vmbuf);
	if (!BufferIsValid(vmbuf))
		return;					/* the fork does not reach it: nothing set */

	LockBuffer(vmbuf, BUFFER_LOCK_SHARE);
	got = lion_vm_allvisible_page(heap, blk, seg, 0, &vmbuf, NULL);
	while (m != 0)
	{
		int			b = pg_rightmost_one_pos64(m);

		m &= m - 1;
		/* the same page: visibilitymap_get_status() keeps the pin, reads */
		if ((visibilitymap_get_status(heap, blk + (BlockNumber) b, &vmbuf) &
			 VISIBILITYMAP_ALL_VISIBLE) != 0)
			expect |= UINT64CONST(1) << b;
	}
	LockBuffer(vmbuf, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(vmbuf);

	/* the mask answers whole map bytes: only the wanted bits are compared */
	Assert((got & seg) == expect);
}

/*
 * visibilitymap_get_status() for every block that has members - the only bits
 * of the mask the count looks at - must agree with the mask.
 *
 * It legitimately might not: both read the map without a lock, and a
 * concurrent VACUUM or DML may change a bit between the two reads - even
 * twice, all-visible to not and back (A to B to A), which a second unlocked
 * read of the mask cannot tell from no change at all, so re-reading the mask
 * and comparing only when it came out the same was not enough (2026-09-25
 * review).  So a disagreement is settled by comparing the two again with the
 * map page SHARE locked: every writer of a map bit - visibilitymap_set(),
 * visibilitymap_clear(), 19's visibilitymap_set_vmbits() - holds the page
 * EXCLUSIVE, so under the share lock both read the same bytes and must agree,
 * and what is compared is what the check is for, the two functions.  The lock
 * is only ever taken in an assert build, on a disagreement, with no other
 * buffer locked; a pin on the heap's index pages is all the caller holds.
 */
void
lion_vm_mask_check(Relation heap, BlockNumber firstblk, uint64 members,
				  uint64 allvis, Buffer *vmbuf)
{
	uint64		expect = 0;
	uint64		m = members;
	int			n;

	while (m != 0)
	{
		int			b = pg_rightmost_one_pos64(m);

		m &= m - 1;
		if ((visibilitymap_get_status(heap, firstblk + (BlockNumber) b, vmbuf) &
			 VISIBILITYMAP_ALL_VISIBLE) != 0)
			expect |= UINT64CONST(1) << b;
	}

	if ((allvis & members) == expect)
		return;

	/* a container's blocks lie on one map page, or on two (see above) */
	n = (int) (LION_VM_HEAPBLOCKS_PER_PAGE -
			   (firstblk % LION_VM_HEAPBLOCKS_PER_PAGE));
	if (n >= LION_BLOCKS_PER_CONTAINER)
		lion_vm_mask_check_page(heap, firstblk, members);
	else
	{
		lion_vm_mask_check_page(heap, firstblk,
								members & ((UINT64CONST(1) << n) - 1));
		lion_vm_mask_check_page(heap, firstblk + (BlockNumber) n,
								members >> n);
	}
}
#endif

/* ---------------------------------------------------------------------
 * The per-query visibility cache
 * --------------------------------------------------------------------- */


/*
 * How many entries work_mem allows.
 *
 * An open-addressing table keeps more slots than members (simplehash grows at
 * a fill factor of 0.9) and allocates the bigger array before freeing the
 * smaller one while it grows, so the nominal entry budget is a third of what
 * work_mem would buy outright; that keeps the real high-water mark at or
 * below work_mem, which is the promise the GUC makes.
 */
static int
lion_vis_cache_budget(void)
{
	int64		budget = ((int64) work_mem * INT64CONST(1024)) /
		((int64) sizeof(LionVisEntry) * 3);

	if (budget < 64)
		budget = 64;			/* a tiny work_mem still caches something */
	if (budget > INT_MAX / 2)
		budget = INT_MAX / 2;
	return (int) budget;
}

LionVisCache *
lion_vis_cache_create(MemoryContext parent)
{
	LionVisCache *cache;

	cache = (LionVisCache *) MemoryContextAllocZero(parent, sizeof(LionVisCache));
	cache->cxt = AllocSetContextCreate(parent,
									   "LionCount visibility cache",
									   ALLOCSET_SMALL_SIZES);
	cache->parent = parent;
	cache->scratch = NULL;
	cache->scratchbusy = false;
	cache->vmbuf = InvalidBuffer;
	cache->vmrelid = InvalidOid;
	return cache;
}

/*
 * Let go of the visibility-map page the counts kept pinned from one to the
 * next (LionVisCache.vmbuf): when the node is done with the relation.
 */
void
lion_vis_cache_release_vm(LionVisCache *cache)
{
	if (cache == NULL || !BufferIsValid(cache->vmbuf))
		return;
	ReleaseBuffer(cache->vmbuf);
	cache->vmbuf = InvalidBuffer;
	cache->vmrelid = InvalidOid;
}

void
lion_vis_cache_reset(LionVisCache *cache)
{
	if (cache == NULL)
		return;
	lion_vis_cache_release_vm(cache);
	cache->ht = NULL;
	MemoryContextReset(cache->cxt);
	cache->relid = InvalidOid;
	cache->snapshot = NULL;
	cache->ncounts = 0;
	cache->full = false;
}

void
lion_vis_cache_destroy(LionVisCache *cache)
{
	if (cache == NULL)
		return;
	lion_vis_cache_release_vm(cache);
	cache->ht = NULL;
	cache->filter = NULL;
	if (cache->cxt != NULL)
		MemoryContextDelete(cache->cxt);
	cache->cxt = NULL;
	if (cache->scratch != NULL)
		MemoryContextDelete(cache->scratch);
	cache->scratch = NULL;
	cache->scratchbusy = false;
}

void
lion_vis_cache_set_filter(LionVisCache *cache, LionRowFilter *filter)
{
	if (cache == NULL)
	{
		if (filter != NULL)
			elog(ERROR, "lion index count: a row filter needs a visibility cache");
		return;
	}
	cache->filter = filter;
}

/*
 * Attach the cache to one count: empty it if it was filled for another
 * relation or under another snapshot, and note that another count has begun.
 *
 * The relation is the check that matters in practice - §16 walks the
 * partitions of one count one at a time, and block numbers mean different
 * things in each - and it is exact.  The snapshot check is a guard rather
 * than a proof: what makes reuse safe is that the driver holds one snapshot
 * for the whole node execution, so the object it hands us cannot be freed and
 * replaced underneath it; two distinct snapshots with the same pointer, xmin,
 * xmax and curcid could still differ in their in-progress list.  A caller
 * that wants a different snapshot must call lion_vis_cache_reset().
 */
void
lion_vis_cache_begin(LionVisCache *cache, Relation heap, Snapshot snapshot)
{
	if (cache == NULL || snapshot == NULL)
		return;

	if (cache->relid != RelationGetRelid(heap) ||
		cache->snapshot != snapshot ||
		cache->xmin != snapshot->xmin ||
		cache->xmax != snapshot->xmax ||
		cache->curcid != snapshot->curcid)
	{
		lion_vis_cache_reset(cache);
		cache->relid = RelationGetRelid(heap);
		cache->snapshot = snapshot;
		cache->xmin = snapshot->xmin;
		cache->xmax = snapshot->xmax;
		cache->curcid = snapshot->curcid;
	}

	cache->maxentries = lion_vis_cache_budget();
	if (cache->ncounts < INT_MAX)
		cache->ncounts++;
}


/*
 * The cached answer for a heap block, or NULL when there is none.
 *
 * WHY A CACHED ANSWER IS STILL THE RIGHT ANSWER (this is the whole point of
 * the cache, so it is argued here rather than in DESIGN.md alone)
 * ---------------------------------------------------------------------------
 * The cache is consulted in exactly one place, lion_recheck_heap_heap(), which
 * is where a TID that the visibility map could not answer for is resolved
 * against the snapshot.  The §9 pin rule is untouched: the visibility-map
 * question is still asked in lion_count_container() while the index page is
 * pinned, and only the TIDs it could not answer reach this code.  What is
 * claimed here is narrower: that
 *
 *		heap_hot_search_buffer(root TID, snapshot)
 *
 * is a function of the snapshot alone, so it may be evaluated once per (page,
 * snapshot) and reused for the rest of the query.  Four things could break
 * that, and none of them can happen:
 *
 *	1. The tuple we found could be removed.  It is visible to our snapshot,
 *	   which is registered, so it is not dead to all: neither HOT pruning nor
 *	   VACUUM may remove it or its root line pointer.
 *	2. HOT pruning could rewrite the chain under us - a core scan's, or on 19
 *	   and later the count's own, just before it reads the block (see
 *	   lion_recheck_heap_heap()).  It may: it can turn the
 *	   root line pointer into a redirect and drop intermediate versions.  But
 *	   it only removes versions that are dead to ALL snapshots - therefore
 *	   invisible to ours - and it keeps every surviving version reachable from
 *	   the root in chain order, which is precisely what
 *	   heap_hot_search_buffer() walks.  The version it finds from a given root
 *	   is unchanged.  (This is the same guarantee a bitmap heap scan relies on
 *	   between building its TID list and visiting the heap.)
 *	3. An answer of "not visible" could become "visible".  That needs a tuple
 *	   whose xmin our snapshot accepts to appear at that offset.  Every tuple
 *	   written after we looked belongs to a transaction that is either still
 *	   in progress at our snapshot, or began after it, or is our own with a
 *	   command id at or above the snapshot's curcid - invisible in all three
 *	   cases.  Line pointer numbers never move (page compaction moves tuple
 *	   data, not line pointers), so an existing visible tuple cannot arrive at
 *	   a different offset either.
 *	4. An answer of "visible" could become "not visible".  A delete by a
 *	   concurrent transaction leaves the tuple visible to our snapshot whether
 *	   it commits or not, and a delete by our own transaction is stamped with
 *	   a command id at or above curcid, which HeapTupleSatisfiesMVCC also
 *	   reports as still visible.
 *
 * Serializable isolation needs one addition, because a cache hit calls
 * neither HeapCheckForSerializableConflictOut() nor PredicateLockTID():
 * lion_vis_fill_page() takes PredicateLockPage() for the block it resolves.
 * That covers every later hit, and the two directions of rw-conflict
 * detection are then both closed: a write that happened BEFORE we resolved
 * the page is caught by the conflict-out check inside the sweep (which walks
 * every chain on the page, so it tests a superset of the tuples any single
 * recheck would have), and a write AFTER it is caught by the writer's own
 * CheckForSerializableConflictIn() against that page lock.
 */
LionVisEntry *
lion_vis_cache_lookup(LionCountCtx *cx, BlockNumber blkno)
{
	if (cx->cache == NULL || cx->cache->ht == NULL)
		return NULL;
	return lion_visht_lookup(cx->cache->ht, blkno);
}

/*
 * The entry whose bitmap the caller should resolve for blkno, or NULL when
 * there is nothing to resolve: no cache, a single count with nothing to
 * reuse, the block's first visit (which is only recorded), or a cache that
 * has spent its budget.
 *
 * The sweep costs one visibility test per line pointer of the page instead of
 * one per TID this count wants, so it is only worth doing once reuse is
 * evident: the first count records nothing, the second records the blocks it
 * visits, and a block is resolved on its second visit.  A one-shot count -
 * one call, every heap block visited once - therefore does exactly what it
 * did before the cache existed, down to the last buffer visit.  This mirrors
 * the `nuses >= 2` rule that decides when a posting set is worth
 * materializing.
 *
 * Called BEFORE the page is read, so that the hash table's allocations never
 * happen under a buffer content lock.
 */
LionVisEntry *
lion_vis_cache_prepare(LionCountCtx *cx, BlockNumber blkno, LionVisEntry *e)
{
	LionVisCache *cache = cx->cache;
	bool		found;

	if (cache == NULL || cache->ncounts < 2)
		return NULL;

	if (e != NULL)
		return e;				/* second visit: resolve the whole page */

	if (cache->ht == NULL)
		cache->ht = lion_visht_create(cache->cxt, 256, NULL);
	else if (cache->full ||
			 cache->ht->members >= (uint64) cache->maxentries)
	{
		/* The budget is spent: this block keeps being fetched per batch. */
		cache->full = true;
		cx->stats.cache_full++;
		return NULL;
	}

	/* First visit: remember only that it happened. */
	e = lion_visht_insert(cache->ht, blkno, &found);
	Assert(!found);
	e->filled = false;
	return NULL;
}

/*
 * Resolve every root line pointer of the page in buf, which the caller holds
 * share locked, into *e.
 */
void
lion_vis_fill_page(LionCountCtx *cx, Buffer buf, BlockNumber blkno,
				  LionVisEntry *e)
{
	Page		page = BufferGetPage(buf);
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	OffsetNumber off;

	Assert(!e->filled);
	if (maxoff > (OffsetNumber) MaxHeapTuplesPerPage)
		maxoff = (OffsetNumber) MaxHeapTuplesPerPage;	/* cannot happen */

	memset(e->vis, 0, sizeof(e->vis));
	for (off = FirstOffsetNumber; off <= maxoff; off = OffsetNumberNext(off))
	{
		ItemPointerData tid;
		HeapTupleData heapTuple;

		ItemPointerSet(&tid, blkno, off);

		/*
		 * The same call the per-TID recheck makes, for every line pointer
		 * instead of the ones this count happens to want.  An offset that is
		 * not the root of a chain (an unused or dead line pointer, or a
		 * heap-only tuple) yields false, which is exactly what a recheck of
		 * that TID would have returned - and no index TID points at one.
		 */
		if (heap_hot_search_buffer(&tid, cx->heap, buf, cx->snapshot,
								   &heapTuple, NULL, true))
			e->vis[(off - 1) / 64] |= UINT64CONST(1) << ((off - 1) % 64);
	}
	e->filled = true;

	/*
	 * Later hits on this block do no per-tuple predicate locking, so lock the
	 * page once now, the way an index-only scan does for a block it skips
	 * (see the argument on lion_vis_cache_lookup()).
	 */
	if (cx->serializable)
		PredicateLockPage(cx->heap, blkno, cx->snapshot);
}
