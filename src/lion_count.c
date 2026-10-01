/*-------------------------------------------------------------------------
 *
 * lion_count.c
 *		Counting: one container against the visibility map and the heap, the
 *		merge of the sources, and running it over a batch of sets.
 *
 * Part of the count engine: lion_count.h is its interface, and
 * lion_count_int.h declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_count_int.h"

static bool lion_exists_settled(LionCountCtx *cx);
static bool lion_sources_collect_keys(Relation heap, Snapshot snapshot,
									  int nsources, LionCountSource *sources,
									  Size maxbytes, bool spill, bool ranged,
									  uint32 lo, uint64 hi,
									  LionPostingSet *out, bool *spilled,
									  LionCountStats *stats);

/* ---------------------------------------------------------------------
 * Counting one container
 * --------------------------------------------------------------------- */

typedef struct LionRecheckCollector
{
	LionCountCtx *cx;
	uint32		ckey;
	const bool *needrecheck;	/* LION_BLOCKS_PER_CONTAINER entries */
} LionRecheckCollector;

/*
 * How many TIDs one recheck batch may hold (DESIGN.md §9).
 *
 * The list used to grow by doubling until the whole merge was over: 20
 * million candidates - which a hot standby produces for any 20-million-row
 * posting set, because it never trusts the visibility map - meant a 192 MiB
 * array, and past 134 million candidates an allocation request larger than
 * palloc will serve.  work_mem is the executor's own answer to "how much may
 * one node keep", so it is the budget here too.
 */
int
lion_recheck_budget(void)
{
	int64		budget = ((int64) work_mem * INT64CONST(1024)) /
		(int64) sizeof(ItemPointerData);

	if (budget < LION_RECHECK_MIN_BATCH)
		budget = LION_RECHECK_MIN_BATCH;
	if (budget > LION_RECHECK_MAX_BATCH)
		budget = LION_RECHECK_MAX_BATCH;
	return (int) budget;
}

static void
lion_recheck_add(LionCountCtx *cx, uint64 code)
{
	ItemPointerData tid;

	lion_code_to_tid(code, &tid);

	/*
	 * Bounded batches.  When the list is full, recheck what it holds right
	 * now, add the visible rows to the running total and start over - rather
	 * than keeping every candidate TID until the merge ends.
	 *
	 * Why flushing here, in the middle of the merge and with the source pages
	 * still pinned, is safe (this is the §9 interlock, so it has to be
	 * argued): the ordering rule is that the VISIBILITY MAP question about a
	 * container's heap blocks must be asked before the pin on the page that
	 * container came from is released, and nothing here touches that - the VM
	 * checks for this container have already happened (they are what put
	 * these TIDs on the list) and the pins are still held.  Rechecking a TID
	 * in the heap under our snapshot needs no index pin at all:
	 *
	 *	- if VACUUM has removed that TID from the index and from the heap page
	 *	  meanwhile, the tuple was dead to every snapshot including ours, so
	 *	  not counting it is right - and table_fetch_tid()/
	 *	  heap_hot_search_buffer() find nothing there;
	 *	- if the line pointer has since been reused by a brand-new tuple, that
	 *	  tuple's xmin is later than our snapshot, so it is invisible to it
	 *	  and is not counted either.
	 *
	 * The only cost of an early flush is that a heap block whose TIDs
	 * straddle two batches is pinned twice, so the flush waits for a block
	 * boundary; that also keeps blocks_rechecked exact.
	 */
	if (cx->ntids >= cx->batchmax &&
		(!cx->tids_sorted ||
		 ItemPointerGetBlockNumber(&cx->tids[cx->ntids - 1]) !=
		 ItemPointerGetBlockNumber(&tid)))
		lion_recheck_flush(cx);

	if (cx->ntids >= cx->maxtids)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(cx->cxt);
		int			newmax;

		if (cx->maxtids == 0)
			newmax = Min(LION_RECHECK_INIT_TIDS, cx->batchmax);
		else if (cx->maxtids < cx->batchmax)
			newmax = Min(cx->maxtids * 2, cx->batchmax);
		else
			newmax = cx->maxtids + LION_RECHECK_GROW_TIDS;	/* a long block */

		if (cx->tids == NULL)
			cx->tids = (ItemPointerData *)
				palloc(sizeof(ItemPointerData) * newmax);
		else
			cx->tids = (ItemPointerData *)
				repalloc(cx->tids, sizeof(ItemPointerData) * newmax);
		cx->maxtids = newmax;
		MemoryContextSwitchTo(oldcxt);
	}

	cx->tids[cx->ntids] = tid;

	/*
	 * The list is built in ckey order, and inside a container in ascending lo
	 * order, so it comes out sorted by (block, offset) - which is what lets
	 * lion_recheck_heap() process it one heap block at a time.  Verify rather
	 * than assume: one comparison per TID buys the right to skip the sort.
	 */
	if (cx->ntids > 0 &&
		ItemPointerCompare(&cx->tids[cx->ntids - 1], &cx->tids[cx->ntids]) >= 0)
		cx->tids_sorted = false;

	cx->ntids++;
}

static bool
lion_recheck_cb(uint16 lo, void *arg)
{
	LionRecheckCollector *rc = (LionRecheckCollector *) arg;
	uint16		blkinc;
	OffsetNumber off;

	lion_lo_split(lo, &blkinc, &off);
	Assert(blkinc < LION_BLOCKS_PER_CONTAINER);
	if (rc->needrecheck[blkinc])
		lion_recheck_add(rc->cx, lion_make_code(rc->ckey, lo));

	return true;
}

/*
 * Copy one container of a collected intersection (DESIGN.md §27).  The merge
 * hands them over in ascending container key order, one per key, which is the
 * order a LionMatSet keeps.  A copy that would outgrow its budget is given up
 * on: failed is set, the merge stops at the next container boundary
 * (lion_exists_settled()), and the caller counts the ordinary way instead.
 *
 * The merge's results come unoptimized (LionCountCtx.raw): what is kept is
 * the smallest representation, as the optimizing set algebra used to leave
 * it, so that a copy takes what it always took.
 */
static void
lion_collect_container(LionCollect *col, const LionContainer *c)
{
	union
	{
		LionContainer hdr;
		uint64		align;
		char		data[LION_CONTAINER_MAX_SIZE];
	}			opt;
	Size		sz;
	MemoryContext oldcxt;
	int			i;

	if (col->failed)
		return;
	if (lion_container_cardinality(c) == 0)
		return;

	memcpy(opt.data, c, lion_container_size(c));
	lion_container_optimize(&opt.hdr);
	c = &opt.hdr;
	sz = lion_item_size(c);
	if (!col->spilled &&
		sizeof(LionMatSet) + MAXALIGN(col->used + sz) +
		(sizeof(LionContainer *) + sizeof(uint32)) * (col->noffs + 1) >
		col->maxbytes)
	{
		if (!col->spill)
		{
			col->failed = true;
			return;
		}

		/*
		 * Past the memory, and allowed to SPILL: what the copy holds so far
		 * goes to a temporary file, and so does everything after it.
		 */
		lion_spill_begin(&col->sp, col->cxt);
		for (i = 0; i < col->noffs; i++)
			lion_spill_add(&col->sp,
						   (const LionContainer *) (col->buf + col->offs[i]));
		pfree(col->buf);
		pfree(col->offs);
		col->buf = NULL;
		col->offs = NULL;
		col->spilled = true;
	}
	if (col->spilled)
	{
		lion_spill_add(&col->sp, c);
		col->members += lion_container_cardinality(c);
		return;
	}

	oldcxt = MemoryContextSwitchTo(col->cxt);
	while (col->used + sz > col->cap)
	{
		col->cap *= 2;
		col->buf = (char *) repalloc(col->buf, col->cap);
	}
	if (col->noffs >= col->offcap)
	{
		col->offcap *= 2;
		col->offs = (Size *) repalloc(col->offs, sizeof(Size) * col->offcap);
	}
	MemoryContextSwitchTo(oldcxt);

	memcpy(col->buf + col->used, c, sz);
	col->offs[col->noffs++] = col->used;
	col->used += MAXALIGN(sz);
	col->members += lion_container_cardinality(c);
}

/*
 * Step 1 of counting a container: ask the visibility map about every heap
 * block that has members, and either count the members outright (plus a
 * predicate lock, as an index-only scan would take) or queue the block's TIDs
 * for a heap recheck under the caller's snapshot.
 *
 * THIS IS THE HEART OF DESIGN.md SECTION 9, and its contract is an ordering
 * one: on entry the page every source container was copied from is still
 * PINNED, and the caller may only let those pins go - which means advancing a
 * cursor - after this function has returned.  Doing it the other way round
 * would let a concurrent VACUUM finish ambulkdelete on the page just read,
 * prune the heap and set all-visible, after which this would count dead
 * tuples.
 *
 * Two callers, and both are written so that the order can be checked by
 * reading them: lion_count_container() below, which advances the merge's
 * cursors straight afterwards, and lion_count_one_set(), the inner loop of
 * the disjoint sum of DESIGN.md §15, which advances its single cursor.
 */
static void
lion_count_container_vm(LionCountCtx *cx, const LionContainer *c)
{
	BlockNumber firstblk = lion_ckey_first_block(c->ckey);
	uint64		members;		/* blocks of this container that have members */
	uint64		allvis;			/* blocks marked all-visible in the VM */

	/* A collection copies the container and asks nothing (DESIGN.md §27). */
	if (cx->collect != NULL)
	{
		lion_collect_container(cx->collect, c);
		return;
	}

	/*
	 * Test hook: the containers have been copied out, the source pages are
	 * still pinned, and the visibility map has not been consulted yet.  A
	 * VACUUM that reaches ambulkdelete while a backend waits here must block
	 * on the cleanup lock; test/isolation/count_vacuum_race.spec proves it.
	 * Compiles to nothing without --enable-injection-points.
	 */
	LION_INJECTION_POINT("lion-count-containers-pinned");

	/*
	 * One pass over the container and one read of the visibility map for all
	 * of its heap blocks, instead of a VM probe and a binary search per block.
	 * VM_ALL_VISIBLE only, never VM_ALL_FROZEN: freezing says nothing about a
	 * tuple being visible to *this* snapshot.  The map read is unlocked and
	 * may be slightly stale in the "bit was just cleared" direction, which is
	 * harmless for the same reason it is harmless for index-only scans (see
	 * visibilitymap_get_status and lion_vm_allvisible_mask).
	 */
	members = lion_container_block_mask(c);
	if (cx->in_recovery || cx->novm)
		allvis = 0;				/* see lion_count_sources(): no interlock */
	else if (cx->filter != NULL)
		allvis = 0;				/* the map vouches for visibility, and every
								 * row has to be tested as well */
	else
	{
		allvis = lion_vm_allvisible_mask(cx->heap, firstblk, members,
										&cx->vmbuf, &cx->stats.vm_pins);
		cx->stats.vm_checks++;
#ifdef LION_VM_MASK_CHECK
		lion_vm_mask_check(cx->heap, firstblk, members, allvis, &cx->vmbuf);
#endif
	}
	lion_count_container_masks(cx, c, members, allvis);
}

/*
 * Step 2 of counting a container, once the visibility map has been asked
 * about its blocks under the pins of DESIGN.md section 9: `members` has the
 * bit of every heap block c has a member on (lion_container_block_mask()),
 * `allvis` those the map marks all-visible.  Count the members of those
 * blocks outright, and queue the others' TIDs for the heap recheck.  The
 * grouped walk (lion_count_groups_copy()) comes here too, with one answer of
 * the map for all the groups' containers at a key.
 */
void
lion_count_container_masks(LionCountCtx *cx, const LionContainer *c,
						   uint64 members, uint64 allvis)
{
	BlockNumber firstblk = lion_ckey_first_block(c->ckey);
	uint64		dirty;			/* blocks with members that need a heap recheck */

	dirty = members & ~allvis;

	if (dirty == 0)
	{
		/* The common case on a vacuumed table: O(1) per container. */
		cx->count += lion_container_cardinality(c);
		cx->stats.blocks_skipped_via_vm += pg_popcount64(members);
	}
	else
	{
		bool		needrecheck[LION_BLOCKS_PER_CONTAINER];
		uint32		dirty_members = 0;
		uint64		m = dirty;
		LionRecheckCollector rc;

		memset(needrecheck, 0, sizeof(needrecheck));
		while (m != 0)
		{
			int			b = pg_rightmost_one_pos64(m);
			uint16		lo_start = (uint16) (b << LION_OFFSET_BITS);
			uint16		lo_end = (uint16) (lo_start + LION_MAX_OFFSET);

			m &= m - 1;
			needrecheck[b] = true;
			dirty_members += lion_container_range_cardinality(c, lo_start, lo_end);
		}

		/*
		 * Only a damaged container can hold more members in some blocks than
		 * its header says it holds in all of them (DESIGN.md §3: the header's
		 * cardinality is a claim).  Its count is then wrong either way, but
		 * not by four billion, and not by an assertion failure.
		 */
		if (unlikely(dirty_members > lion_container_cardinality(c)))
			dirty_members = lion_container_cardinality(c);

		cx->count += lion_container_cardinality(c) - dirty_members;
		cx->stats.blocks_skipped_via_vm += pg_popcount64(members & allvis);

		/* Queue the members of the non-all-visible blocks for a heap recheck. */
		rc.cx = cx;
		rc.ckey = c->ckey;
		rc.needrecheck = needrecheck;
		lion_container_iterate(c, lion_recheck_cb, &rc);
	}

	/*
	 * We are not visiting the heap for the blocks counted above, so lock
	 * those pages as an index-only scan would (heapam_indexscan.c).  Only a
	 * serializable transaction can need them.
	 */
	if (cx->serializable)
	{
		uint64		m = members & allvis;

		while (m != 0)
		{
			int			b = pg_rightmost_one_pos64(m);

			m &= m - 1;
			PredicateLockPage(cx->heap, firstblk + (BlockNumber) b, cx->snapshot);
		}
	}

}

/*
 * The merge's form: count the container, then let the cursors move on.
 *
 * DESIGN.md section 9: every heap block of *c has been checked against the
 * visibility map by the time the loop below runs, so - and only now - the pins
 * on the pages the source containers came from may be released.
 * lion_ecursor_next() is what releases them.
 *
 * Only the DRIVER is flagged to move on (DESIGN.md §22).  The others stay
 * where they are and are SOUGHT to wherever the driver gets to on the next
 * round of the merge, which is what makes their work one probe per container
 * key of the driver rather than a walk; a cursor that keeps its place also
 * keeps its pin, which is a pin too many and never a wrong answer.
 */
static void
lion_count_container(LionCountCtx *cx, const LionContainer *c,
					LionExprCursor *cursors, int nsources)
{
	int			i;

	lion_count_container_vm(cx, c);

	for (i = 0; i < nsources; i++)
	{
		if (cursors[i].advance)
			lion_ecursor_next(&cursors[i]);
	}
}

/*
 * Is ckey past the chunk a collection is making (LionCollect.ranged)?  Never
 * for a count, or a collection of the whole intersection.
 */
static inline bool
lion_collect_past(const LionCountCtx *cx, uint32 ckey)
{
	return cx->collect != NULL && cx->collect->ranged &&
		(uint64) ckey >= cx->collect->hi;
}

/*
 * Count ONE posting set on its own.
 *
 * Three callers, all of them counts that have nothing to merge: each entry of
 * the disjoint sum of DESIGN.md §15, each entry of the sum-over-all of §14, and
 * any count that comes down to a single set - a plain `WHERE k = 5`, or one
 * group of a GROUP BY with no other clause.  It is lion_run_merge() with every
 * source but one removed, written out because that takes an expression cursor,
 * a tree node and a per-container pass over the source array out of a loop that
 * runs tens of thousands of times for one IN list.
 *
 * The DESIGN.md §9 ordering is the same and is visible in the same two adjacent
 * statements: consult the visibility map, and only then advance the cursor,
 * which is the one thing that unpins the page the container was copied from.
 * The set's OWN pin (an inline entry's) is the caller's and is not touched
 * here: lion_cursor_init() borrows it and lion_cursor_close() leaves it.
 *
 * A NOPIN set has no such pin, and nothing else is counted with it to carry
 * the interlock, so its entry is located again here and counted from that
 * copy, under the pin the new lookup keeps until the count of it is done
 * (DESIGN.md §15).  One set at a time: the pass holds that one pin.
 *
 * A MATERIALIZED set has none either: it is a private copy, which is only
 * ever counted while another source of the same intersection carries the
 * interlock (lion_posting_set_materialize()).  No caller hands one over on
 * its own today - the copies are made for the WHERE sets of a GROUP BY, and a
 * lone set is never copied - but nothing would stop one (2026-09-25 review),
 * and counting it from the visibility map would be exactly the stale read §9
 * forbids.  So it is counted from its CHAIN instead, walked page by page
 * under the cursor's own pins as any located CHAIN set is: the head is the
 * posting tree's root, which a set keeps for the life of the index (§18),
 * and a page that no longer belongs to the set ends the walk.
 *
 * An existence test (DESIGN.md §26) stops after the first container that
 * settles it; the cursor has moved on by then, which is where the §9 ordering
 * says it may.
 */
static void
lion_count_one_set(LionCountCtx *cx, LionPostingSet *ps)
{
	LionPostingSet fresh;
	LionSetCursor cur;
	bool		relocated = false;

	if (ps->nopin)
	{
		if (!lion_posting_set_relocate(ps, &fresh))
			return;
		ps = &fresh;
		relocated = true;
	}
	else if (ps->mat != NULL)
	{
		Assert(ps->found && !ps->is_inline);

		/*
		 * A COLLECTED intersection (lion_sources_collect()) has no chain to
		 * fall back on, and is only ever COUNTED beside a located set that
		 * carries the interlock (DESIGN.md §27).  A collection is no count:
		 * it asks the visibility map nothing and needs no pin
		 * (lion_count_container_vm()), so it copies such a set as it stands.
		 * That is what the FK-side join does when its one fact filter is a
		 * range already collected into memory (§32): one source of one set,
		 * the shape this function is the shortcut for.  It used to be refused
		 * here, and the join's first row failed.
		 */
		if (!BlockNumberIsValid(ps->head))
		{
			if (cx->collect == NULL)
				elog(ERROR, "lion index: a collected posting set counted on its own");
		}
		else
		{
			fresh = *ps;
			fresh.mat = NULL;	/* the chain itself, not the copy */
			fresh.budgeted = false; /* nothing of it is the list's to return */
			ps = &fresh;
		}
	}

	/* a chunk of a collection (ranged): from its first key, up to its end */
	lion_cursor_init_at(&cur, ps, cx, false,
						(cx->collect != NULL && cx->collect->ranged) ?
						cx->collect->lo : 0, NULL);
	while (cur.valid)
	{
		if (lion_collect_past(cx, cur.cur->ckey))
			break;
		lion_count_container_vm(cx, cur.cur);
		lion_cursor_next(&cur);
		if (lion_exists_settled(cx))
			break;
		CHECK_FOR_INTERRUPTS();
	}
	lion_cursor_close(&cur);

	if (relocated)
		lion_posting_set_release(&fresh);
}

/*
 * Is an EXISTENCE test answered yet (DESIGN.md §26)?  Always false for a
 * count.  Called by the merge after each container it has put through the
 * visibility map - so after lion_count_container_vm() has asked its question
 * under the source pins, which is the whole of the DESIGN.md §9 obligation -
 * and never anywhere else.
 *
 *	- A member on an all-visible block (cx->count > 0) is a visible row, by
 *	  exactly the argument that lets a count add it: the settle is immediate,
 *	  and the recheck TIDs the same container may have queued are dropped
 *	  unread.  They could only have added to an answer already known.
 *	- Otherwise whatever this container queued is rechecked now instead of at
 *	  the end of the merge.  The flush is the ordinary one (its argument is on
 *	  lion_recheck_add(): a recheck needs no index pin), and a container
 *	  boundary is a heap-block boundary, so no block is split across batches.
 *	  One visible row settles the test.
 *
 * So a test on an all-visible heap reads the first container of the
 * intersection and stops, and one on a dirty heap rechecks container by
 * container until its first visible row - never more than one container past
 * it.
 */
static bool
lion_exists_settled(LionCountCtx *cx)
{
	/* A collection that outgrew its budget stops the same way (§27). */
	if (cx->collect != NULL)
		return cx->collect->failed;

	if (!cx->exists)
		return false;

	if (cx->count > 0)
	{
		cx->ntids = 0;
		cx->tids_sorted = true;
		return true;
	}

	lion_recheck_flush(cx);
	return cx->recheck_count > 0;
}

static int
lion_tid_cmp(const void *a, const void *b)
{
	return ItemPointerCompare((ItemPointer) a, (ItemPointer) b);
}

/*
 * Recheck through the table AM, one TID at a time.  This is the portable
 * path; table_fetch_tid() pins, share-locks and unpins the block for every
 * TID, which is what lion_recheck_heap_heap() avoids for the heap AM.
 *
 * The TIDs come from an index, so each one is the root of a HOT chain, which
 * is exactly what table_fetch_tid() expects: it walks the chain and reports
 * whether any version of the row satisfies the snapshot.  Under an MVCC
 * snapshot at most one version can, so a visible chain counts as one row.
 *
 * (DESIGN.md section 9 step 4 describes this in terms of
 * table_index_fetch_tuple()/call_again; that API no longer exists in
 * PostgreSQL 20devel, where the index-scan callbacks moved into the table AM.
 * table_fetch_tid() is its direct replacement for TID-at-a-time lookups.)
 */
static int64
lion_recheck_heap_am(LionCountCtx *cx)
{
	int64		visible = 0;
	BlockNumber lastblk = InvalidBlockNumber;
	int			i;

	for (i = 0; i < cx->ntids; i++)
	{
		ItemPointerData tid = cx->tids[i];	/* mutable copy: callee updates it */
		BlockNumber blk = ItemPointerGetBlockNumber(&tid);

		if (blk != lastblk)
		{
			cx->stats.blocks_rechecked++;
			lastblk = blk;
		}

		if (lion_table_fetch_tid(cx->heap, &tid, cx->snapshot, NULL))
		{
			visible++;
			if (cx->visout != NULL)
				cx->visout[i] = true;
		}

		if ((i & 0x3ff) == 0)
			CHECK_FOR_INTERRUPTS();
	}

	return visible;
}

/*
 * The same thing for the heap AM, one heap BLOCK at a time.
 *
 * table_fetch_tid() is heapam_fetch_tid(), which is ReadBuffer + share lock +
 * heap_hot_search_buffer() + unlock + unpin.  Our TID list is sorted, and
 * after a plain DELETE every heap block of the posting set is dirty, so the
 * per-TID version pins, locks, unlocks and unpins the same buffer once for
 * every member of the set that lives on the block - a million buffer lookups
 * for a million-row table, which measured slower than the sequential scan the
 * pushdown is supposed to beat.  Reading the buffer once per block and
 * calling heap_hot_search_buffer() under one share lock is the same work
 * without the per-TID buffer manager traffic.
 *
 * heap_hot_search_buffer() is the very function the AM callback uses, so the
 * semantics are unchanged, including the ones that are easy to lose:
 *
 *	- it starts at the root of the HOT chain, which is what an index stores,
 *	  and follows redirects and HOT updates to the one version our snapshot
 *	  can see (first_call = true, one call per TID: with an MVCC snapshot at
 *	  most one chain member is visible, and heapam_fetch_tid() looks no
 *	  further either);
 *	- it calls HeapCheckForSerializableConflictOut() on every chain member it
 *	  tests and PredicateLockTID() on the one it returns, so a SERIALIZABLE
 *	  transaction takes exactly the tuple-level predicate locks it would have
 *	  taken through the table AM.  (The all-visible blocks we never look at
 *	  are predicate-locked page-wise in lion_count_container(), as an
 *	  index-only scan does.)
 *
 * all_dead is passed as NULL, as the table_fetch_tid() call it replaces did:
 * we have no index tuple to mark killed, and asking for it would cost a
 * GlobalVisTest per invisible chain for nothing.
 *
 * The per-query visibility cache sits here and nowhere else: a block whose
 * answer is already known is served from the bitmap with no buffer access at
 * all, and the blocks that are left are fetched once each, as they always
 * were.  lion_vis_cache_lookup() carries the argument for why a remembered
 * answer is still the right one.
 *
 * PRUNING ON ACCESS (PostgreSQL 19 and later; DESIGN.md §11, "On-access
 * pruning sets the visibility map too").  Every block that is read is first
 * offered to heap_page_prune_opt(), exactly as a bitmap heap scan offers it
 * (BitmapHeapScanNextBlock()): pinned, not locked, with the count's own
 * visibility map pin to reuse.  If the page qualifies - something prunable,
 * little free space, the cleanup lock free right now - it is pruned, and when
 * the statement does not modify the relation (cx->rel_read_only) and what is
 * left is visible to every snapshot, it is marked all-visible, so that the
 * next count, or the next group of this one, reads it from the map instead of
 * rechecking it.  That a map bit set this way is as good as one VACUUM set -
 * a pinned container's TID on an all-visible page is exactly one visible row -
 * is the argument of that DESIGN.md subsection, checked against pruneheap.c.
 * Here it only has to be safe to call:
 *
 *	- no lock is held that it could wait behind: the index pages of the merge
 *	  are pinned, never locked, at a flush, the heap cleanup lock is only ever
 *	  tried, and the VM page is pinned before that and locked only briefly,
 *	  as by every core scan;
 *	- the answers below are taken after it, from the pruned page, and pruning
 *	  removes only versions no snapshot can see and never moves a root line
 *	  pointer, which is all the per-TID recheck and the cache rely on (point 2
 *	  of the argument on lion_vis_cache_lookup());
 *	- it does nothing in recovery (its own first test), and on 16-18 the call
 *	  compiles to nothing (lion_compat.h), where core's scans still prune.
 */
static int64
lion_recheck_heap_heap(LionCountCtx *cx)
{
	int64		visible = 0;
	int			i = 0;

	while (i < cx->ntids)
	{
		BlockNumber blk = ItemPointerGetBlockNumber(&cx->tids[i]);
		LionVisEntry *e = lion_vis_cache_lookup(cx, blk);
		Buffer		buf;

		/* Already resolved: no ReadBuffer, no content lock, no heap at all. */
		if (e != NULL && e->filled)
		{
			cx->stats.cache_hits++;
			do
			{
				if (lion_vis_entry_visible(e, ItemPointerGetOffsetNumber(&cx->tids[i])))
				{
					visible++;
					if (cx->visout != NULL)
						cx->visout[i] = true;
				}
				i++;
			} while (i < cx->ntids &&
					 ItemPointerGetBlockNumber(&cx->tids[i]) == blk);

			CHECK_FOR_INTERRUPTS();
			continue;
		}

		/* Decide (and allocate) before the page is locked. */
		e = lion_vis_cache_prepare(cx, blk, e);

		buf = ReadBuffer(cx->heap, blk);
		lion_heap_page_prune_opt(cx->heap, buf, &cx->vmbuf, cx->rel_read_only);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		cx->stats.blocks_rechecked++;

		if (e != NULL)
		{
			/* Resolve the whole page once, then read this count's TIDs off. */
			lion_vis_fill_page(cx, buf, blk, e);
			do
			{
				if (lion_vis_entry_visible(e, ItemPointerGetOffsetNumber(&cx->tids[i])))
				{
					visible++;
					if (cx->visout != NULL)
						cx->visout[i] = true;
				}
				i++;
			} while (i < cx->ntids &&
					 ItemPointerGetBlockNumber(&cx->tids[i]) == blk);
		}
		else
		{
			do
			{
				ItemPointerData tid = cx->tids[i];	/* callee updates it */
				HeapTupleData heapTuple;

				if (heap_hot_search_buffer(&tid, cx->heap, buf, cx->snapshot,
										   &heapTuple, NULL, true))
				{
					visible++;
					if (cx->visout != NULL)
						cx->visout[i] = true;
				}
				i++;
			} while (i < cx->ntids &&
					 ItemPointerGetBlockNumber(&cx->tids[i]) == blk);
		}

		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buf);

		/* Only now: an interrupt cannot be serviced under a buffer lock. */
		CHECK_FOR_INTERRUPTS();
	}

	return visible;
}

bool
lion_row_filter_test(LionRowFilter *filter, HeapTuple tuple)
{
	TupleDesc	desc = RelationGetDescr(filter->heap);
	MemoryContext oldcxt = MemoryContextSwitchTo(filter->tmpcxt);
	bool		pass = true;
	int			i;

	for (i = 0; i < filter->nclauses && pass; i++)
	{
		LionRowFilterClause *c = &filter->clauses[i];
		Datum		d;
		bool		isnull;

		d = heap_getattr(tuple, c->attno, desc, &isnull);
		if (isnull)
			pass = false;		/* a strict operator, or IS NOT NULL */
		else if (!c->notnull)
		{
			LOCAL_FCINFO(fcinfo, 2);
			Datum		result;

			InitFunctionCallInfoData(*fcinfo, &c->flinfo, 2, c->collation,
									 NULL, NULL);
			fcinfo->args[0].value = d;
			fcinfo->args[0].isnull = false;
			fcinfo->args[1].value = c->value;
			fcinfo->args[1].isnull = false;
			result = FunctionCallInvoke(fcinfo);
			pass = (!fcinfo->isnull && DatumGetBool(result));
		}
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextReset(filter->tmpcxt);
	return pass;
}

/*
 * lion_recheck_heap_heap() for a count with a row filter (DESIGN.md §17, "A
 * query known only at run time"): the row the snapshot sees is not only
 * found, it is tested, and counted when it passes.
 *
 * The chain of each candidate is resolved under the share lock exactly as
 * there - heap_hot_search_buffer(), its serializable conflict checks and
 * tuple predicate locks included - and the offsets of the visible members
 * are noted.  The filter runs after the lock is released, on the tuples the
 * pin alone keeps in place, which is what core's page-at-a-time heap scans do
 * (heap_prepare_pagescan()): nothing moves a tuple while another backend holds
 * a pin, since pruning and defragmentation need the cleanup lock, and the
 * operator it calls may read TOAST or run for a while, neither of which may
 * happen under a buffer content lock.
 *
 * The visibility cache is neither read nor filled: it knows which offsets
 * are visible, but the filter needs their tuples.
 */
static int64
lion_recheck_heap_filtered(LionCountCtx *cx)
{
	LionRowFilter *filter = cx->filter;
	OffsetNumber vis[MaxHeapTuplesPerPage];
	int			visidx[MaxHeapTuplesPerPage];	/* ... and its place in tids */
	int64		passed = 0;
	int			i = 0;

	while (i < cx->ntids)
	{
		BlockNumber blk = ItemPointerGetBlockNumber(&cx->tids[i]);
		Buffer		buf;
		Page		page;
		int			nvis = 0;
		int			j;

		buf = ReadBuffer(cx->heap, blk);
		lion_heap_page_prune_opt(cx->heap, buf, &cx->vmbuf, cx->rel_read_only);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		cx->stats.blocks_rechecked++;

		do
		{
			ItemPointerData tid = cx->tids[i];	/* callee updates it */
			HeapTupleData heapTuple;

			if (heap_hot_search_buffer(&tid, cx->heap, buf, cx->snapshot,
									   &heapTuple, NULL, true))
			{
				/* one visible member per chain, one chain per candidate */
				if (nvis >= MaxHeapTuplesPerPage)
					elog(ERROR, "lion index count: more visible tuples than a heap page holds");
				visidx[nvis] = i;
				vis[nvis++] = ItemPointerGetOffsetNumber(&tid);
			}
			i++;
		} while (i < cx->ntids &&
				 ItemPointerGetBlockNumber(&cx->tids[i]) == blk);

		LockBuffer(buf, BUFFER_LOCK_UNLOCK);

		page = BufferGetPage(buf);
		for (j = 0; j < nvis; j++)
		{
			ItemId		lp = PageGetItemId(page, vis[j]);
			HeapTupleData tuple;

			tuple.t_data = (HeapTupleHeader) PageGetItem(page, lp);
			tuple.t_len = ItemIdGetLength(lp);
			tuple.t_tableOid = RelationGetRelid(cx->heap);
			ItemPointerSet(&tuple.t_self, blk, vis[j]);

			if (lion_row_filter_test(filter, &tuple))
			{
				passed++;
				if (cx->visout != NULL)
					cx->visout[visidx[j]] = true;
			}
			else
				cx->stats.rows_removed++;
		}

		ReleaseBuffer(buf);
		CHECK_FOR_INTERRUPTS();
	}

	return passed;
}

int64
lion_count_heap_filtered(Relation heap, Snapshot snapshot,
						 LionRowFilter *filter, LionCountStats *stats)
{
	TableScanDesc scan;
	HeapTuple	tuple;
	BlockNumber lastblk = InvalidBlockNumber;
	int64		count = 0;

	if (filter == NULL || filter->heap != heap)
		elog(ERROR, "lion index count: a heap scan without its row filter");

	/*
	 * An ordinary sequential scan under the count's snapshot: it takes the
	 * relation's predicate lock under SERIALIZABLE and prunes on access as
	 * any scan does.  table_beginscan_strat() rather than table_beginscan(),
	 * whose arguments changed in PostgreSQL 19; these are its defaults.
	 */
	scan = table_beginscan_strat(heap, snapshot, 0, NULL, true, true);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		BlockNumber blk = ItemPointerGetBlockNumber(&tuple->t_self);

		if (blk != lastblk)
		{
			stats->blocks_rechecked++;
			lastblk = blk;
		}
		stats->tids_rechecked++;
		if (lion_row_filter_test(filter, tuple))
			count++;
		else
			stats->rows_removed++;
		CHECK_FOR_INTERRUPTS();
	}
	table_endscan(scan);

	return count;
}

/*
 * Visit the heap for the TIDs of blocks that were not all-visible.
 */
static int64
lion_recheck_heap(LionCountCtx *cx)
{
	int64		visible;

	if (cx->ntids == 0)
		return 0;

	/*
	 * Both paths below want the list in (block, offset) order; it is produced
	 * that way, and lion_recheck_add() checks that it was.
	 */
	if (!cx->tids_sorted)
		qsort(cx->tids, cx->ntids, sizeof(ItemPointerData), lion_tid_cmp);

	/*
	 * A filtered count reads the tuples themselves, which only the heap AM
	 * gives it that way; the count pushdown refuses every other table AM
	 * before it gets here (DESIGN.md §10).
	 */
	if (cx->filter != NULL)
	{
		if (cx->heap->rd_tableam != GetHeapamTableAmRoutine())
			elog(ERROR, "lion index count: a row filter over a table that is not a heap");
		visible = lion_recheck_heap_filtered(cx);
	}
	else if (cx->heap->rd_tableam == GetHeapamTableAmRoutine())
		visible = lion_recheck_heap_heap(cx);
	else
		visible = lion_recheck_heap_am(cx);

	cx->stats.tids_rechecked += cx->ntids;
	return visible;
}

/*
 * The heap's answer for each TID of cx->tids rather than their number: vis[i]
 * is set when the i'th is a row the snapshot sees (and, under a row filter,
 * one that passes it), for a caller that has to know WHICH rows - the decoded
 * walk (DESIGN.md §34), whose rows each belong to a combination of groups it
 * keeps beside them.  The list must be in (block, offset) order already, as
 * every list built key by key is, since sorting it here would part it from
 * what the caller keeps beside it; it is left as it is, and so is vis for
 * every TID the heap does not show.  Returns how many were visible.
 */
int64
lion_recheck_visible(LionCountCtx *cx, bool *vis)
{
	int64		visible;

	if (!cx->tids_sorted)
		elog(ERROR, "lion index count: a recheck by row of TIDs out of order");
	cx->visout = vis;
	visible = lion_recheck_heap(cx);
	cx->visout = NULL;
	return visible;
}

/*
 * Recheck the batch accumulated so far and empty the list.  Called from
 * lion_recheck_add() whenever the budget is reached (the safety argument is
 * there) and once more when the merge is over.
 */
void
lion_recheck_flush(LionCountCtx *cx)
{
	if (cx->ntids == 0)
		return;

	cx->recheck_count += lion_recheck_heap(cx);
	cx->ntids = 0;
	cx->tids_sorted = true;
}


/* ---------------------------------------------------------------------
 * The merge
 * --------------------------------------------------------------------- */

/*
 * The SQL-callable counts come through here and cannot see the statement that
 * called them, which may be modifying the relation: they prune on access but
 * never ask pruning to set the visibility map (rel_read_only = false, as core
 * passes for a scan of a result relation; DESIGN.md §11).
 */
int64
lion_count_sources(Relation heap, Snapshot snapshot, int nsources,
				  LionCountSource *sources, LionCountStats *stats)
{
	return lion_count_sources_cached(heap, snapshot, nsources, sources, stats,
									NULL, false);
}

/*
 * The collected copy a count's other source is, when it is one: a source of
 * one set, a copy lion_sources_collect() made (LionMatSet.collected).  NULL
 * for anything else.
 */
static const LionPostingSet *
lion_source_collected(const LionCountSource *src, const LionNodePlan *plan)
{
	const LionPostingSet *ps;

	if (src->negated || src->nsets != 1 || plan->node == NULL ||
		plan->node->kind != LION_KN_KEY)
		return NULL;
	ps = &src->sets[plan->node->keyno];
	if (!ps->found || ps->mat == NULL || !ps->mat->collected)
		return NULL;
	return ps;
}


/*
 * THE MERGE OF A SET AND A COLLECTED COPY (DESIGN.md §27, "The copy, looked
 * up by key"): lion_run_merge() for its commonest pair, the key's set of an
 * FK-side join - or a group's set, §10 - driving, and the copy of the WHERE
 * sources that every one of those counts is ANDed with.  The copy is an array
 * of containers in key order, never changed once made, so it needs no cursor:
 * each of the driver's containers looks its key up in it (lion_mat_seek(),
 * direct where the copy's keys are dense) and is ANDed with the container
 * found there, and where the copy has none at that key the driver is sought
 * to the next key it has, as the leapfrog of the general merge would seek it.
 * What that saves is the general merge's bookkeeping round every container
 * of the key: a cursor over the copy sought, advanced and rebuilt, the
 * sources' keys compared, their order walked.
 *
 * The §9 rule reads as in lion_count_container(): a container of the result
 * is put to the visibility map before the driver - the one source that
 * carries a pin - moves past it, and a key the copy has nothing at, or
 * whose AND is empty, asks the map nothing and lets the driver go on.  The
 * copy holds no pin and is only ever counted beside the driver's pinned
 * containers, which is lion_sources_collect()'s argument.
 */
static void
lion_run_merge_copy(LionCountCtx *cx, LionCountSource *drvsrc,
					const LionNodePlan *drvplan, const LionPostingSet *copy)
{
	const LionMatSet *mat = copy->mat;
	LionExprCursor drv;
	LionContainer *work;
	LionContainer *buf = NULL;
	int			idx = 0;

	work = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	if (lion_mat_spills(mat))
		buf = (LionContainer *) palloc(MAXALIGN(LION_CONTAINER_MAX_SIZE));

	lion_ecursor_init(&drv, drvplan, drvsrc->sets, drvsrc->nsets, cx, false);
	while (drv.valid)
	{
		uint32		key;

		cx->stats.copy_seeks++;
		idx = lion_mat_seek(mat, idx, drv.ckey);
		if (idx >= mat->ncontainers)
			break;				/* nothing of the copy at or past the key */
		key = lion_mat_key(mat, idx);
		if (key != drv.ckey)
		{
			/* the copy has nothing here: on to the next key it has (§22) */
			lion_ecursor_seek(&drv, key);
			CHECK_FOR_INTERRUPTS();
			continue;
		}

		if (lion_container_and_raw(drv.cur,
								   lion_mat_container(cx, mat, idx, buf),
								   work) > 0)
		{
			/* the map is asked before the driver lets its page go */
			lion_count_container_vm(cx, work);
			lion_ecursor_next(&drv);
			if (lion_exists_settled(cx))
				break;
		}
		else
			lion_ecursor_next(&drv);
		CHECK_FOR_INTERRUPTS();
	}
	lion_ecursor_close(&drv);

	pfree(work);
	if (buf != NULL)
		pfree(buf);
}

/*
 * One pass of the merge: intersect the positive sources container key by
 * container key, subtract the negated ones, and count what is left against the
 * visibility map.  Everything the caller set up in *cx - the recheck batch, the
 * visibility map pin, the visibility cache, the statistics - is accumulated
 * into, so this may be called more than once for one count: the batches of a
 * disjoint list below are the caller that does (lion_run_batches()).  Each
 * source's cursor is built from its plan (lion_plan_node()); cx->novm must
 * already say whether any positive source's plan is pinned.
 *
 * This is where DESIGN.md §9 lives; nothing about the interlock changes with
 * how often it runs, because each pass takes its pins, asks the visibility map
 * and drops them again inside lion_count_container().
 */
static void
lion_run_merge(LionCountCtx *cx, int nsources, LionCountSource *sources,
			  LionNodePlan **plans)
{
	LionExprCursor *cursors;
	LionContainer *work[2];
	double	   *est;			/* members of each positive source */
	int		   *probeord;		/* the positive sources, least first */
	int			nprobe = 0;
	int			driver;
	uint64		hi;				/* the merge ends before this key */
	int			i;

	cursors = (LionExprCursor *) palloc0(sizeof(LionExprCursor) * nsources);

	/*
	 * WHICH SOURCE DRIVES, AND IN WHICH ORDER THE OTHERS ARE PROBED
	 * (DESIGN.md §22, §25).  The merge is a leapfrog join: one source is
	 * walked sequentially and the others are PROBED at the container keys it
	 * produces.  The driver has to be the most selective one, because the work
	 * is its container count times the number of sources; a dense driver would
	 * make every other source probe at every container key of the heap, which
	 * is the walk this replaces.
	 *
	 * The others are probed in ASCENDING SELECTIVITY, which is what lets the
	 * loop below stop at the first one that empties the intersection: the
	 * fewer members a source has, the likelier it is to be the one that kills
	 * the container key, and everything after it in this order is then never
	 * sought at all.  Ordering by anything else would still be correct and
	 * would only read more pages.
	 *
	 * The estimate is the sum of the entries' own row counts, which the
	 * located posting sets carry already (`ntids`), so no page is read to
	 * decide it.  It is exact for the ordinary one-set source and an upper
	 * bound for a union; a wrong guess costs performance and never an answer.
	 * The sort is an insertion sort because a query has a handful of sources
	 * and this runs once per count; it is STABLE, so probeord[0] is the first
	 * source with the fewest members - the same one this used to pick with a
	 * single `est < bestest` pass, and the same one the cost model picks.
	 */
	est = (double *) palloc0(sizeof(double) * nsources);
	probeord = (int *) palloc(sizeof(int) * nsources);
	for (i = 0; i < nsources; i++)
	{
		int			j;

		if (sources[i].negated)
			continue;
		for (j = 0; j < sources[i].nsets; j++)
			est[i] += (double) sources[i].sets[j].ntids;

		for (j = nprobe++; j > 0 && est[probeord[j - 1]] > est[i]; j--)
			probeord[j] = probeord[j - 1];
		probeord[j] = i;
	}
	Assert(nprobe > 0);
	driver = probeord[0];

	/*
	 * A set against a collected copy - what an FK-side join's every count
	 * is, and a GROUP BY's once its WHERE is collected - with the set
	 * driving: the copy is looked up at the set's keys, and needs no cursor
	 * (lion_run_merge_copy()).  A collection is a merge of the sources
	 * themselves, never of a copy.
	 */
	if (nsources == 2 && nprobe == 2 && cx->collect == NULL)
	{
		const LionPostingSet *copy = lion_source_collected(&sources[probeord[1]],
														   plans[probeord[1]]);

		if (copy != NULL)
		{
			lion_run_merge_copy(cx, &sources[driver], plans[driver], copy);
			pfree(cursors);
			pfree(est);
			pfree(probeord);
			return;
		}
	}

	/*
	 * Only an intersection needs a place to put one: a single source hands its
	 * own container straight to lion_count_container(), and the disjoint sum
	 * of DESIGN.md §15 runs this once per entry, where two 4 KiB buffers per
	 * pass are the bulk of the work.
	 */
	work[0] = work[1] = NULL;
	if (nsources > 1)
	{
		work[0] = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
		work[1] = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	}

	/*
	 * The positive sources are lazy: a union or a tree among them is built
	 * only when the leapfrog finds that cheaper than probing its members
	 * (DESIGN.md §29.11, "Unions probed", "Trees probed") - but for a tree
	 * that drives, which is built as it is sought (lion_leapfrog_lazy()).  A
	 * negated one is subtracted whole.
	 */
	for (i = 0; i < nsources; i++)
		lion_ecursor_init_ex(&cursors[i], plans[i], sources[i].sets,
							 sources[i].nsets, cx, false, cx->raw,
							 !sources[i].negated &&
							 (i != driver ||
							  lion_leapfrog_lazy(plans[i]->node)));

	/*
	 * A chunk of a shared copy (LionCollect.ranged) begins at its first key:
	 * the positive sources are sought there - a collection holds no pin, so
	 * nothing they pass over carries an answer - and the merge ends where the
	 * chunk does (`hi`, below).  The negated ones are sought when they are
	 * needed, as always.
	 */
	if (cx->collect != NULL && cx->collect->ranged && cx->collect->lo > 0)
	{
		for (i = 0; i < nsources; i++)
		{
			if (!sources[i].negated)
				lion_ecursor_seek(&cursors[i], cx->collect->lo);
		}
	}

	/*
	 * Past a container key only the DRIVER steps; the others are left standing
	 * where they are and are sought to wherever the driver has got to
	 * (DESIGN.md §22).  A cursor that keeps its place also keeps its pin,
	 * which is a pin too many and never a wrong answer (see
	 * lion_ecursor_next()).
	 */
	for (i = 0; i < nsources; i++)
		cursors[i].advance = false;
	cursors[driver].advance = true;
	hi = (cx->collect != NULL && cx->collect->ranged) ?
		cx->collect->hi : LION_WIDE_END;

	/*
	 * Merge the sources by container key.  Containers are stored in ascending
	 * ckey order both inline and along a chain, and an expression cursor
	 * preserves that, so a single forward pass over all of them is enough.
	 */
	for (;;)
	{
		uint32		maxckey;
		const LionContainer *acc;
		int			w;

		/*
		 * THE INTERSECTION OF THE POSITIVE SOURCES, BUILT AS THEY ARE SOUGHT
		 * (DESIGN.md §25): the leapfrog every AND of posting sets is made by,
		 * a lion index scan's included (lion_leapfrog(), which carries the §9
		 * argument).  The sources are taken in probe order, each sought to the
		 * driver's key and folded into the accumulator before the next one is
		 * touched at all, and a key is abandoned - the sources after it in the
		 * order not sought, the pages their probes would have read not read -
		 * the moment the accumulator is empty.  Before §25 the whole round of
		 * seeks was made first and the intersection looked at afterwards,
		 * which read a dense source's leaf at every container key the driver
		 * produced, including the ones where the selective sources had
		 * already ruled the key out.  A chunk of a collection ends at its last
		 * key (lion_collect_past()).
		 */
		acc = lion_leapfrog(cursors, probeord, nprobe, work, &w, hi,
							&cx->stats, &maxckey);
		if (acc == NULL)
			goto merge_done;

		/* ... minus the negated ones (DESIGN.md §14, `col IS NOT NULL`). */
		for (i = 0; i < nsources; i++)
		{
			if (!sources[i].negated)
				continue;
			if (cursors[i].valid && cursors[i].ckey < maxckey)
				lion_ecursor_seek(&cursors[i], maxckey);	/* nothing to subtract there */
			if (!cursors[i].valid || cursors[i].ckey != maxckey)
				continue;

			if (lion_container_cardinality(acc) > 0)
			{
				lion_container_andnot_raw(acc, cursors[i].cur, work[w]);
				acc = work[w];
				w ^= 1;
			}
		}

		if (lion_container_cardinality(acc) > 0)
		{
			lion_count_container(cx, acc, cursors, nsources);

			/*
			 * An existence test (DESIGN.md §26) is done at the first container
			 * that shows a visible row.  The VM question about this one has
			 * been asked under its pins, and the cursors have moved on; what
			 * they still pin is released below, as at the end of any merge.
			 */
			if (lion_exists_settled(cx))
				goto merge_done;
		}
		else
		{
			/*
			 * Nothing of this container key survives, so no visibility-map
			 * question is asked about it and the source pages may go.
			 */
			for (i = 0; i < nsources; i++)
			{
				if (cursors[i].advance)
					lion_ecursor_next(&cursors[i]);
			}
		}
		CHECK_FOR_INTERRUPTS();
	}

merge_done:
	for (i = 0; i < nsources; i++)
		lion_ecursor_close(&cursors[i]);

	pfree(cursors);
	pfree(est);
	pfree(probeord);
	if (work[0] != NULL)
	{
		pfree(work[0]);
		pfree(work[1]);
	}
}

/*
 * Is this tree the plain union of every one of the source's sets, each set
 * appearing exactly once?  That is the shape lion_locate_leaf() gives an IN
 * list and lion_source_tree() gives a source with no tree of its own, and it
 * is the only shape the disjoint-sum short-circuit below may be applied to: a
 * set that appears twice in the tree, or an AND anywhere in it, would make the
 * union something other than the sum.
 */
static bool
lion_tree_is_flat_union(const LionKeyNode *node, int nsets)
{
	int			i;

	if (node == NULL)
		return true;			/* the implicit union of all the sets */
	if (node->kind == LION_KN_KEY)
		return nsets == 1 && node->keyno == 0;
	if (node->kind != LION_KN_OR || node->nargs != nsets)
		return false;
	for (i = 0; i < nsets; i++)
	{
		if (node->args[i]->kind != LION_KN_KEY || node->args[i]->keyno != i)
			return false;
	}
	return true;
}

/*
 * THE DISJOINT-SUM SHORT-CIRCUIT (DESIGN.md §15)
 * ==============================================
 * The count of a union is not in general the sum of the counts - a row in two
 * of the sets would be counted twice - but for the posting sets of DIFFERENT
 * entries of one SCALAR lion index it is, because those sets are disjoint by
 * construction:
 *
 *	- a scalar opclass extracts exactly ONE key from a row's column value, so
 *	  the build and the insert path put that row's TID under exactly one entry:
 *	  the entry of its value, or the reserved NULL entry of DESIGN.md §14 when
 *	  the value is NULL, or (for a multi-key opclass only) the reserved EMPTY
 *	  entry of §17;
 *	- the reserved entries are therefore disjoint from every value entry as
 *	  well, and from each other;
 *	- so no TID is in two entries, and the union of any set of entries has
 *	  exactly as many members as their counts add up to.
 *
 * DESIGN.md §14 already relies on exactly this: `col IS NOT NULL` with nothing
 * else to drive the merge is answered by summing the counts of every entry of
 * the column's index, and that is only the count of their union because the
 * entries are disjoint.  This is the same argument applied to the entries an
 * IN list names instead of to all of them.
 *
 * A MULTI-KEY opclass (DESIGN.md §17) is excluded, and this is the whole
 * reason the test below asks the index rather than trusting the caller: one
 * row yields many keys there, so it appears under several entries and the sum
 * over them is not a row count.  §17 refuses such an index as a GROUP BY or
 * sum-over-all driver for the same reason.
 *
 * What the caller has to promise, because this code cannot see it, is that no
 * two of the sets are the SAME entry: `src->disjoint`.
 * lion_posting_set_lookup_many() keeps that promise by dropping duplicates by
 * entry identity and not merely by value.
 *
 * Finally, the short-circuit only applies while this source is the ONLY
 * positive one.  An intersection has to be evaluated container key by
 * container key, and for that the union has to be materialised per key, which
 * is precisely the merge - but the same disjointness still lets the merge take
 * the list a BATCH of entries at a time and add the passes up
 * (lion_run_batches()).
 */
static bool
lion_source_disjoint_list(const LionCountSource *src)
{
	Relation	index = NULL;
	AttrNumber	attno = 0;
	int			i;

	if (src->negated || !src->disjoint)
		return false;
	if (src->nsets < 2)
		return false;			/* one set is its own union already */
	if (!lion_tree_is_flat_union(src->tree, src->nsets))
		return false;

	for (i = 0; i < src->nsets; i++)
	{
		if (!src->sets[i].found)
			continue;
		if (index == NULL)
		{
			index = src->sets[i].index;
			attno = (AttrNumber) src->sets[i].attno;
		}
		else if (src->sets[i].index != index)
			return false;		/* entries of two indexes are not disjoint */
		else if ((AttrNumber) src->sets[i].attno != attno)
			return false;		/* nor are two COLUMNS of one index (§24) */
	}
	if (index == NULL)
		return false;

	return !lion_index_column_state(index, attno)->multikey;
}

static bool
lion_sources_disjoint_sum(int nsources, const LionCountSource *sources)
{
	return nsources == 1 && lion_source_disjoint_list(&sources[0]);
}

/*
 * ... and is it FASTER?  The short-circuit above is about whether summing is
 * the RIGHT answer; this is about whether it is the cheap one, and the two are
 * independent.
 *
 * Both paths read every container of every set exactly once and count the same
 * members.  What they do not share is where the per-container overheads land:
 *
 *	- the SUM asks the visibility map once per (set, container key), because
 *	  each set is counted on its own;
 *	- the MERGE asks it once per container key, for the union, and pays instead
 *	  a heap sift and a union step per container.
 *
 * So the sum wins exactly when there is little to amortize.  Two things say
 * there is, and either one is enough to send the count back to the merge:
 *
 *	- DENSE sets.  If an entry has many rows in the SAME container key, the
 *	  merge folds all the sets' members at that key into one container and asks
 *	  the visibility map about it once.  Measured at 1M rows (15385 heap pages,
 *	  241 container keys), `k IN (1000 values)` with the sum against the merge:
 *	  20000 distinct keys (1.0 rows per key per container key) 3.6 ms against
 *	  9.9, 5000 keys (1.0) 11.5 against 21.5, 2000 keys (2.1) 25.1 against 32.2,
 *	  500 keys (8.3) 21.2 against 9.0, and 200 keys (20.7) 14.0 against 6.5.
 *	  The crossover is between two and eight rows per container key, so the
 *	  test is four.
 *	- and MANY of them, because the merge only becomes good at dense sets once
 *	  it reaches its bitset image (LION_OR_BITSET_MIN containers at one key);
 *	  below that it folds them pairwise, which is quadratic in the sets.  On the
 *	  200-key column above, sum against merge at 5 values is 0.49 ms against
 *	  0.64, at 15 values 1.14 against 2.37, at 30 values 2.15 against 7.6 - and
 *	  at 50, where the image takes over, 3.5 against 2.5.
 *
 * The density is read off the entries' own row counts, which the posting sets
 * carry already (`ntids`), against the number of container keys the heap has;
 * no extra page is touched to decide this.  lion_cost_count_rel() makes the
 * same test from the planner's estimates, so that the price the node is chosen
 * on is the price of the path it will take; LION_SUM_MAX_DENSITY and
 * LION_OR_BITSET_MIN live in lion_count.h for that reason.
 */
static bool
lion_sum_is_cheaper(Relation heap, const LionCountSource *src)
{
	double		ckeys;
	double		density;
	int64		ntids = 0;
	int			nfound = 0;
	int			i;

	/* Too few sets for the merge's bitset image: it would fold them pairwise. */
	if (src->nsets < LION_OR_BITSET_MIN)
		return true;

	for (i = 0; i < src->nsets; i++)
	{
		if (!src->sets[i].found)
			continue;
		ntids += src->sets[i].ntids;
		nfound++;
	}
	if (nfound < LION_OR_BITSET_MIN)
		return true;

	ckeys = (double) RelationGetNumberOfBlocks(heap) / LION_BLOCKS_PER_CONTAINER;
	if (ckeys < 1.0)
		ckeys = 1.0;

	/* rows per container key in the average entry */
	density = ((double) ntids / nfound) / ckeys;

	return density <= LION_SUM_MAX_DENSITY;
}

/*
 * Is every posting set of every source held in an index whose records replay
 * under a cleanup lock (DESIGN.md §25)?  That is the condition for trusting
 * the visibility map in recovery; see the comment at cx.in_recovery below.
 */
static bool
lion_sources_all_rmgr(int nsources, LionCountSource *sources)
{
	int			i,
				j;

	for (i = 0; i < nsources; i++)
	{
		for (j = 0; j < sources[i].nsets; j++)
		{
			LionPostingSet *ps = &sources[i].sets[j];

			if (!ps->found)
				continue;		/* an absent key contributes no TID */
			if (ps->index == NULL ||
				lion_wal_mode(ps->index) != LION_WAL_MODE_RMGR)
				return false;
		}
	}

	return true;
}

/*
 * Does this source hold a set that was located without its pin (DESIGN.md
 * §15)?
 */
static bool
lion_source_has_nopin(const LionCountSource *src)
{
	int			j;

	for (j = 0; j < src->nsets; j++)
	{
		if (src->sets[j].found && src->sets[j].nopin)
			return true;
	}
	return false;
}

/*
 * Does the whole count come down to ONE posting set?
 *
 * That is not the short-circuit above and needs none of its argument: there is
 * no union to take apart, so nothing about disjointness, about the opclass or
 * about how the caller found the set comes into it.  It is only worth asking
 * because it is the shape of every group of a GROUP BY, of every entry of the
 * sum-over-all of DESIGN.md §14, and of a plain `WHERE k = 5` - and because
 * lion_count_one_set() answers it without an expression cursor, a tree node or
 * a per-container pass over a one-element source array.
 */
static bool
lion_sources_one_set(int nsources, const LionCountSource *sources)
{
	return (nsources == 1 &&
			!sources[0].negated &&
			sources[0].nsets == 1 &&
			lion_tree_is_flat_union(sources[0].tree, 1));
}

/*
 * THE BATCHED DISJOINT LIST (DESIGN.md §15, "Bounded cursors").
 *
 * The disjoint sum counts a list's entries one at a time, and only when the
 * list is the only positive source.  Anywhere else - `k = ANY ($1) AND x = 1`,
 * a dense list the sum would be slow for, a GROUP BY on another column - the
 * list is a source of the merge, and the merge used to build a cursor for
 * every entry at once: 17 kB each (8 since the staging buffers were sized)
 * and, for a CHAIN entry, a buffer pin from the moment it was built.  A list
 * of 50000 values held 880 MB whatever work_mem said, and a temporary table's
 * list of 1100 CHAIN entries ran out of local buffers.
 *
 * The disjointness that makes the sum exact makes batches exact too.  With
 * the list's sets S_1 .. S_n pairwise disjoint and cut into batches whose
 * unions are U_1 .. U_m, R the intersection of the other positive sources and
 * N the union of the negated ones,
 *
 *		|((U_1 ∪ ... ∪ U_m) ∩ R) \ N|  =  Σ_j |(U_j ∩ R) \ N|
 *
 * because the (U_j ∩ R) \ N are pairwise disjoint and together make up the
 * left side.  So the ordinary merge runs once per batch, with the list
 * replaced by that batch's union, and the passes add up - into cx, which
 * every pass accumulates into anyway.
 *
 * DESIGN.md §9 needs no new argument.  Each pass is an ordinary merge: it
 * builds its cursors, asks the visibility map under their pins and closes
 * them before the next pass builds any.  A batch is sized to fit the open
 * budget, so it is never planned wide, and its union is pinned exactly when
 * each of its sets is; whether a pass may trust the map is decided per pass,
 * from that pass's plans.  Across passes the count shares only what the
 * disjoint sum already shares - the recheck queue, the visibility-map pin,
 * the visibility cache - and no TID is in two batches, a scalar index holding
 * a row under exactly one entry.  (A line pointer VACUUM frees between two
 * passes and an insert reuses under another entry is a row inserted after our
 * snapshot: invisible to the heap recheck, and its page cannot be all-visible
 * while our snapshot is registered.)
 *
 * The price is that every other source is read once per pass.  A batch is as
 * large as the budget allows, so an ordinary list is one pass - no batching at
 * all, lion_batch_source() says -1 - and nothing about it changes.
 */

/*
 * The source, if any, that is a disjoint list too large to open at once: the
 * one whose cursors would hold the most memory, opened whole.  -1 if none.
 */
static int
lion_batch_source(int nsources, const LionCountSource *sources,
				  LionKeyNode **trees, const LionOpenBudget *budget)
{
	int			best = -1;
	Size		bestmem = 0;
	int			i;

	for (i = 0; i < nsources; i++)
	{
		LionNodePlan p;

		if (!lion_source_disjoint_list(&sources[i]))
			continue;
		lion_plan_node(&p, trees[i], sources[i].sets, NULL, false, false);
		if (p.mem <= budget->mem && p.pins <= budget->pins)
			continue;
		if (best < 0 || p.mem > bestmem)
		{
			best = i;
			bestmem = p.mem;
		}
	}
	return best;
}

/*
 * Where the batch that starts at sets[start] ends: as many sets as the open
 * budget allows, and at least LION_BATCH_MIN_SETS that have an entry - or,
 * with fewer pins than that (lion_open_budget_init()), as many as the pins
 * allow and at least one, which is never planned wide either.  The sets
 * without an entry are left out of the batch's union and cost nothing.  The
 * price is what lion_plan_node() charges for an OR over the found ones, plus
 * the accumulators it charges only from two or three children on, so that a
 * batch that fits is never planned wide.
 */
static int
lion_batch_end(const LionCountSource *src, int start,
			   const LionOpenBudget *budget)
{
	Size		mem = lion_node_overhead(LION_KN_OR, 3) -
		3 * LION_OR_CHILD_OVERHEAD;
	int			pins = 0;
	int			n = 0;
	int			end;

	for (end = start; end < src->nsets; end++)
	{
		Size		m;
		int			p;

		if (!src->sets[end].found)
			continue;
		lion_leaf_cost(&src->sets[end], false, &m, &p);
		m += LION_OR_CHILD_OVERHEAD;
		if (n >= 1 && pins + p > budget->pins)
			break;
		if (n >= LION_BATCH_MIN_SETS && mem + m > budget->mem)
			break;
		mem += m;
		pins += p;
		n++;
	}
	return end;
}

/*
 * Count the merge of sources[] with sources[b], a disjoint list, taken a
 * batch at a time (see above).  trees[] are the sources' own; the list's is
 * rebuilt per batch over the batch's found sets.
 */
static void
lion_run_batches(LionCountCtx *cx, int nsources, LionCountSource *sources,
				 LionKeyNode **trees, int b, const LionOpenBudget *budget)
{
	LionCountSource *list = &sources[b];
	LionCountSource *pass;
	LionNodePlan **plans;
	MemoryContext passcxt;
	int			start = 0;
	int			i;

	/* The other sources are planned once: their plans do not change. */
	plans = (LionNodePlan **) palloc0(sizeof(LionNodePlan *) * nsources);
	for (i = 0; i < nsources; i++)
	{
		if (i != b)
			plans[i] = lion_plan_build(trees[i], sources[i].sets, budget, false);
	}
	pass = (LionCountSource *) palloc(sizeof(LionCountSource) * nsources);
	memcpy(pass, sources, sizeof(LionCountSource) * nsources);

	/*
	 * Everything one pass builds - the batch's tree and plan, and every
	 * cursor of the merge - lives here and is gone before the next pass.
	 */
	passcxt = AllocSetContextCreate(CurrentMemoryContext,
									"lion index count batch",
									ALLOCSET_DEFAULT_SIZES);

	while (start < list->nsets)
	{
		int			end = lion_batch_end(list, start, budget);
		MemoryContext oldcxt;
		LionKeyNode **args;
		int			nargs = 0;
		int			ncarry = 0;

		oldcxt = MemoryContextSwitchTo(passcxt);

		/* the batch's union, over the sets that have an entry */
		args = (LionKeyNode **) palloc(sizeof(LionKeyNode *) * (end - start));
		for (i = start; i < end; i++)
		{
			if (!list->sets[i].found)
				continue;
			args[nargs] = (LionKeyNode *) palloc0(sizeof(LionKeyNode));
			args[nargs]->kind = LION_KN_KEY;
			args[nargs]->keyno = i - start;
			nargs++;
		}

		if (nargs > 0)
		{
			LionKeyNode *tree = args[0];

			if (nargs > 1)
			{
				tree = (LionKeyNode *) palloc0(sizeof(LionKeyNode));
				tree->kind = LION_KN_OR;
				tree->nargs = nargs;
				tree->args = args;
			}
			pass[b].sets = &list->sets[start];
			pass[b].nsets = end - start;
			pass[b].tree = tree;
			plans[b] = lion_plan_build(tree, pass[b].sets, budget, false);
			Assert(!plans[b]->wide);

			/* §9: does this pass have a positive source that carries it? */
			for (i = 0; i < nsources; i++)
			{
				if (!pass[i].negated && plans[i]->pinned)
					ncarry++;
			}
			cx->novm = (ncarry == 0);

			lion_run_merge(cx, nsources, pass, plans);
		}

		MemoryContextSwitchTo(oldcxt);
		MemoryContextReset(passcxt);
		start = end;

		/* An existence test needs one batch with a visible row (§26). */
		if (lion_exists_settled(cx))
			break;
		CHECK_FOR_INTERRUPTS();
	}

	MemoryContextDelete(passcxt);
	pfree(pass);
	pfree(plans);
}

/*
 * The count - or, with exists set, the existence test of DESIGN.md §26 - of
 * (intersection of the positive sources) minus (the negated ones).  Both
 * public forms below are this one function, so that an existence test reads
 * containers, asks the visibility map and rechecks the heap exactly the way a
 * count does, and differs only in where it stops (lion_exists_settled()).
 */

int64
lion_count_sources_cached(Relation heap, Snapshot snapshot, int nsources,
						 LionCountSource *sources, LionCountStats *stats,
						 LionVisCache *cache, bool rel_read_only)
{
	return lion_count_sources_run(heap, snapshot, nsources, sources, stats,
								  cache, rel_read_only, false, NULL);
}

bool
lion_exists_sources_cached(Relation heap, Snapshot snapshot, int nsources,
						  LionCountSource *sources, LionCountStats *stats,
						  LionVisCache *cache, bool rel_read_only)
{
	return lion_count_sources_run(heap, snapshot, nsources, sources, stats,
								  cache, rel_read_only, true, NULL) > 0;
}

/*
 * The intersection of the positive sources minus the negated ones, as a
 * private posting set (DESIGN.md §27): the containers the merge of a count
 * would have put through the visibility map, copied into memory instead, and
 * nothing asked of the map or the heap.  *out comes back a found, pinless,
 * materialized set of no index entry of its own - head is invalid - and holds
 * every row of the intersection, visible to the caller's snapshot or not.  It
 * is what the FK-side join intersects with each dimension row's fk set, where
 * the merge of the fact filters would otherwise be built again at every one
 * of that set's container keys, per dimension row.
 *
 * A copy made this way is a stale copy by the time it is counted, exactly as
 * a materialized set is, and it is safe on exactly the same terms (the
 * argument is on lion_posting_set_materialize()): it may only ever be counted
 * beside a located set that carries the DESIGN.md §9 interlock - a dead TID
 * it still lists is then either gone from that set's container, or on a heap
 * block whose all-visible bit VACUUM cannot have set yet - and it cannot be
 * missing a row the snapshot sees, because it is read after the snapshot was
 * taken and a visible row was in every index before its transaction
 * committed.  lion_count_one_set() refuses to count one on its own.
 *
 * Returns false, with *out not found and nothing allocated, when the copy
 * would take more than maxbytes.  The caller then counts the ordinary way.  A
 * list too long to open at once is read as a windowed union, not in the
 * batches a count takes it in (lion_count_sources_run()).  The copy is
 * allocated in the current memory context.
 *
 * With spill, a copy past maxbytes goes on in a temporary file instead
 * (LionSpill, 2026-09-28 review): what memory then keeps is sixteen bytes a
 * container, and a count reads the file a container at a time.  The ordinary
 * way it would otherwise fall back to seeks every source at every container
 * key of every located set it is counted beside - for a long IN list among
 * the sources, a union of all its sets built again for each of them.
 *
 * It returns false as well where no source has a found set to copy from.
 */

bool
lion_sources_collect(Relation heap, Snapshot snapshot, int nsources,
					 LionCountSource *sources, Size maxbytes, bool spill,
					 LionPostingSet *out, bool *spilled, LionCountStats *stats)
{
	return lion_sources_collect_keys(heap, snapshot, nsources, sources,
									 maxbytes, spill, false, 0, 0,
									 out, spilled, stats);
}

/*
 * The same copy of the container keys from lo up to, not including, hi alone
 * (LION_KEYS_END: every key from lo on) - one range of lion_key_ranges()'s
 * cut, which one participant of a parallel GROUP BY counts its groups against
 * (DESIGN.md §10, "A GROUP BY in parallel").  The sources are sought to lo and
 * the merge stops at hi, as for a chunk of a shared copy (LionCollect.ranged);
 * what it holds is the whole copy's containers at those keys, on the same
 * terms.
 */
bool
lion_sources_collect_range(Relation heap, Snapshot snapshot, int nsources,
						   LionCountSource *sources, Size maxbytes, bool spill,
						   uint32 lo, uint64 hi, LionPostingSet *out,
						   bool *spilled, LionCountStats *stats)
{
	return lion_sources_collect_keys(heap, snapshot, nsources, sources,
									 maxbytes, spill, true, lo, hi,
									 out, spilled, stats);
}

static bool
lion_sources_collect_keys(Relation heap, Snapshot snapshot, int nsources,
						  LionCountSource *sources, Size maxbytes, bool spill,
						  bool ranged, uint32 lo, uint64 hi,
						  LionPostingSet *out, bool *spilled,
						  LionCountStats *stats)
{
	LionCollect col;
	LionMatSet *mat;
	Relation	index = NULL;
	int			i;
	int			j;

	memset(out, 0, sizeof(LionPostingSet));
	out->pinbuf = InvalidBuffer;
	out->head = InvalidBlockNumber;
	*spilled = false;

	for (i = 0; i < nsources && index == NULL; i++)
	{
		for (j = 0; j < sources[i].nsets; j++)
		{
			if (sources[i].sets[j].found)
			{
				index = sources[i].sets[j].index;
				break;
			}
		}
	}
	if (index == NULL)
		return false;

	/* the catalogs a spill's file needs, looked up before the merge starts */
	if (spill)
		PrepareTempTablespaces();

	memset(&col, 0, sizeof(col));
	col.cxt = CurrentMemoryContext;
	col.maxbytes = maxbytes;
	col.spill = spill;
	col.ranged = ranged;
	col.lo = lo;
	col.hi = hi;
	col.cap = 8192;
	col.buf = (char *) palloc(col.cap);
	col.offcap = 256;
	col.offs = (Size *) palloc(sizeof(Size) * col.offcap);

	(void) lion_count_sources_run(heap, snapshot, nsources, sources, stats,
								  NULL, false, false, &col);

	if (col.failed)
	{
		pfree(col.buf);
		pfree(col.offs);
		return false;
	}

	/*
	 * An empty intersection is a set that selects nothing, which is how a key
	 * with no entry reads: not found.
	 */
	out->index = index;
	out->attno = 1;
	out->cxt = CurrentMemoryContext;
	out->entryblk = InvalidBlockNumber;
	out->entryoff = InvalidOffsetNumber;
	if (col.spilled)
	{
		mat = lion_spill_finish(&col.sp);

		/*
		 * Every count against the copy - the FK-side join's per dimension
		 * key, a GROUP BY's per group - builds a cursor over it, and a cursor
		 * starts at the first container: keep that one in memory, so that a
		 * count reads the file once, for the container it is sought to, and
		 * not twice.
		 */
		lion_spill_keep_first(mat, CurrentMemoryContext);

		/* ... and seek it by its keys, directly where they are dense */
		lion_mat_index(mat, CurrentMemoryContext, 0);
		mat->collected = true;
		out->found = true;
		out->mat = mat;
		out->ntids = col.members;
		out->ncontainers = (uint32) mat->ncontainers;
		*spilled = true;
		return true;
	}
	if (col.noffs == 0)
	{
		pfree(col.buf);
		pfree(col.offs);
		return true;
	}

	mat = (LionMatSet *) palloc0(sizeof(LionMatSet));
	mat->ncontainers = col.noffs;
	mat->bytes = col.used;
	mat->held = sizeof(LionMatSet) + MAXALIGN(Max(col.used, (Size) 1)) +
		sizeof(LionContainer *) * Max(col.noffs, 1);
	mat->buf = col.buf;
	mat->containers = (LionContainer **)
		palloc(sizeof(LionContainer *) * Max(col.noffs, 1));
	for (i = 0; i < col.noffs; i++)
		mat->containers[i] = (LionContainer *) (col.buf + col.offs[i]);
	pfree(col.offs);

	/*
	 * The keys, which the budget above made room for, and the direct index
	 * where they are dense and it fits what is left: every count against the
	 * copy seeks it at each container of its own key's set.
	 */
	lion_mat_index(mat, CurrentMemoryContext,
				   maxbytes > mat->held ? maxbytes - mat->held : 0);
	mat->collected = true;

	out->found = true;
	out->mat = mat;
	out->ntids = col.members;
	out->ncontainers = (uint32) col.noffs;

	return true;
}


int64
lion_count_sources_run(Relation heap, Snapshot snapshot, int nsources,
					   LionCountSource *sources, LionCountStats *stats,
					   LionVisCache *cache, bool rel_read_only, bool exists,
					   LionCollect *collect)
{
	MemoryContext cxt;
	MemoryContext oldcxt;
	LionCountCtx cx;
	LionKeyNode **trees;
	LionOpenBudget budget;
	int64		result;
	int			ncarry;
	bool	   *carry;
	bool		summed;
	bool		oneset;
	bool		scratch = false;
	int			batchsrc;
	Size		matheld;
	Size		matbudget = (Size) work_mem * 1024;
	int			npositive = 0;
	int			i;
	int			j;

	Assert(nsources >= 1);

	/*
	 * The shape of each source, and whether it can select anything at all: a
	 * positive source that cannot makes the whole intersection empty, and a
	 * negated one that cannot simply subtracts nothing.  A range source still
	 * to be walked is no set at all, and is the caller's to expand
	 * (LionCountSource.rangewalk).
	 */
	trees = (LionKeyNode **) palloc0(sizeof(LionKeyNode *) * nsources);
	for (i = 0; i < nsources; i++)
	{
		if (sources[i].rangewalk != NULL)
			elog(ERROR, "lion index: a range source to be walked handed to a count");
		trees[i] = lion_source_tree(&sources[i]);

		if (sources[i].negated)
			continue;
		npositive++;
		if (!lion_source_satisfiable(trees[i], sources[i].sets))
			return 0;
	}
	if (npositive == 0)
		elog(ERROR, "lion index count needs at least one positive source");

	/*
	 * The disjoint-sum short-circuit: count each entry's posting set on its own
	 * and add the results up, instead of merging a thousand sub-cursors into
	 * one union.  Nothing else about the count changes - each set goes through
	 * the same single-set machinery below, with the same visibility-map check
	 * per container and the same recheck queue - so DESIGN.md §9 reads exactly
	 * as it does for a one-key count, one set at a time.
	 *
	 * Two questions, and both have to be yes: is the sum the same answer as the
	 * union (lion_sources_disjoint_sum(), which carries the argument) and is it
	 * the cheaper way to get it (lion_sum_is_cheaper(), which is where the
	 * measurements are).
	 *
	 * Or the list was longer than the lookup's pin budget and some of its sets
	 * are NOPIN (DESIGN.md §15).  The sum is then taken however the costs
	 * compare, because it is the one way to count them that keeps the §9
	 * interlock: one set at a time, each located again under a pin of its own
	 * (lion_count_one_set()).  The union would have nothing to carry it - the
	 * list is the only positive source - and could only recheck every TID.
	 */
	summed = (lion_sources_disjoint_sum(nsources, sources) &&
			  (lion_source_has_nopin(&sources[0]) ||
			   lion_sum_is_cheaper(heap, &sources[0])));

	/*
	 * A collection (DESIGN.md §27) wants the union itself, not the sum of its
	 * counts, and needs no interlock to build it: a NOPIN set is read from
	 * its own copy like any other.
	 */
	if (collect != NULL)
		summed = false;
	oneset = !summed && lion_sources_one_set(nsources, sources);

	/*
	 * Everything else is a merge, and a merge builds every cursor of every
	 * source at once.  What they may hold open is budgeted (DESIGN.md §15,
	 * "Bounded cursors"): a disjoint list too big for the budget is taken a
	 * batch at a time (lion_run_batches()), and any other union too big for
	 * it is read as a windowed union, pinless (lion_plan_node()).  Both only
	 * happen past the budget, so an ordinary query is planned exactly as it
	 * always was.
	 */
	lion_open_budget_init(&budget, heap);

	/*
	 * A collection is never batched.  The passes of a batched list each
	 * yield every container key of their own, so a copy of them would come
	 * out of order; and the reason a COUNT batches a disjoint list rather than
	 * read it as a windowed union - that a batch keeps its pins, and so the
	 * DESIGN.md §9 interlock - is nothing to a collection, whose cursors drop
	 * every pin anyway (DESIGN.md §27).  A list too wide to open at once is
	 * therefore read the way any other too-wide union is (lion_plan_node()):
	 * a window of container keys at a time, its sets opened one after the
	 * other, every key in order.  That is what lets an IN list whose length
	 * the planner could not see - a parameter, an expression (§27) - be
	 * collected once however long it turns out, instead of being merged
	 * again by every count; such a list used to be refused a copy here.
	 */
	batchsrc = (summed || oneset || collect != NULL) ? -1 :
		lion_batch_source(nsources, sources, trees, &budget);

	/*
	 * Decide which sets to serve from a private copy this time (DESIGN.md
	 * section 9; the argument is on lion_posting_set_materialize()).
	 *
	 * Two rules, and the safety of the whole thing rests on the second:
	 *
	 *	1. only a set that has been counted before, which in practice means
	 *	   the WHERE sets of the GROUP BY path in lion_exec_count.c, where the
	 *	   same sets are intersected with every group in turn and walking
	 *	   their chains again per group is the dominant cost.  A one-shot
	 *	   count never pays for a copy it would use once.
	 *
	 *	2. never the last POSITIVE source that still carries the interlock.
	 *	   A set that is INLINE, or that is walked page by page, holds a pin
	 *	   while its containers are counted against the visibility map, and
	 *	   that pin is what keeps VACUUM from having finished ambulkdelete() -
	 *	   on this index, and therefore from having set all-visible on any
	 *	   heap page at all.  One source is enough, but there must be one, and
	 *	   it has to be a positive one that holds a pin at EVERY container key
	 *	   it yields, which is what lion_source_pinned() decides - under the
	 *	   open budget the cursors will be built with, which makes a union too
	 *	   wide for it no carrier at all.  A list taken in batches is priced
	 *	   whole instead: each batch of it is pinned exactly when all of its
	 *	   sets are.
	 *
	 * A negated set may always be copied: a stale copy can only hold TIDs
	 * whose rows are dead (a live row's key cannot change without the row
	 * getting a new TID), and subtracting a dead TID cannot take a live row
	 * out of the count.
	 *
	 * And the copies are BUDGETED (DESIGN.md §15, "Bounded cursors").  They
	 * live as long as the sets do - for a GROUP BY, the whole of a relation's
	 * turn - and a list on another column made every one of its CHAIN sets a
	 * copy, up to 256 kB each, with nothing bounding the total.  So all the
	 * copies this count's sources hold, those made by earlier counts of the
	 * same sets included, stay within work_mem; a set that does not fit is
	 * walked page by page, as a set too big to copy always was.
	 */
	carry = (bool *) palloc0(sizeof(bool) * nsources);
	ncarry = 0;
	matheld = 0;
	for (i = 0; i < nsources; i++)
	{
		for (j = 0; j < sources[i].nsets; j++)
		{
			if (sources[i].sets[j].found)
				sources[i].sets[j].nuses++;
			if (sources[i].sets[j].mat != NULL)
				matheld += sources[i].sets[j].mat->held;
		}

		if (sources[i].negated)
			continue;
		carry[i] = lion_source_pinned(trees[i], sources[i].sets,
									  i == batchsrc ? NULL : &budget);
		if (carry[i])
			ncarry++;
	}

	/*
	 * A summed source counts every set exactly once, so there is nothing a
	 * private copy could save; skipping the decision also keeps the one
	 * positive source of each pass on the pinned path by construction.  A
	 * collection reads each set once too.
	 */
	for (i = 0; !summed && collect == NULL && i < nsources; i++)
	{
		/*
		 * Rule 3 (DESIGN.md §19): a source may forbid it outright.  An OR
		 * across columns does, because a dead TID may be contributed by any
		 * single leaf of the union and the interlock the source carries is
		 * that EVERY leaf holds a pin (lion_source_pinned()); keeping that
		 * property is simpler than reasoning about which other source
		 * happened to carry the interlock at the container key in question.
		 */
		if (sources[i].nomaterialize)
			continue;

		for (j = 0; j < sources[i].nsets; j++)
		{
			LionPostingSet *ps = &sources[i].sets[j];

			if (!ps->found)
				continue;
			if (ps->is_inline || ps->mat != NULL)
				continue;		/* nothing to gain: already a private copy */
			if (ps->nuses < 2)
				continue;		/* rule 1 */
			if (!sources[i].negated && carry[i] && ncarry <= 1)
				continue;		/* rule 2: this is the last interlock */
			if (ps->ncontainers > LION_MATERIALIZE_MAX_CONTAINERS &&
				ps->ntids > LION_MATERIALIZE_MAX_BYTES / sizeof(uint16))
				continue;		/* hopeless even as an ARRAY of members */
			if (ps->matfailed)
				continue;		/* tried, and too big for what was left */
			if (matheld >= matbudget)
				continue;		/* the budget is spent */

			if (!lion_posting_set_materialize(ps, matbudget - matheld))
				continue;
			matheld += ps->mat->held;

			if (!sources[i].negated && carry[i] &&
				!lion_source_pinned(trees[i], sources[i].sets,
									i == batchsrc ? NULL : &budget))
			{
				carry[i] = false;
				ncarry--;
			}
		}
	}

	/*
	 * A collection counts nothing, keeps no answer and asks the map nothing
	 * (below): it has no use for the node's cache.
	 */
	if (collect != NULL)
		cache = NULL;

	/*
	 * The memory the count works in: the node's scratch context, reset when
	 * the count is over, where the caller keeps a cache for its execution
	 * (LionVisCache.scratch) - one AllocSet made per node instead of one per
	 * count, whose first block, and the two blocks past it that a merge's 8 kB
	 * work containers used to take, were allocated and freed again for every
	 * key of an FK-side join.
	 */
	if (cache != NULL && !cache->scratchbusy)
	{
		if (cache->scratch == NULL)
			cache->scratch = AllocSetContextCreate(cache->parent,
												   "lion index count",
												   ALLOCSET_DEFAULT_MINSIZE,
												   LION_COUNT_SCRATCH_BLOCK,
												   ALLOCSET_DEFAULT_MAXSIZE);
		cxt = cache->scratch;
		cache->scratchbusy = true;
		scratch = true;
	}
	else
		cxt = AllocSetContextCreate(CurrentMemoryContext,
									"lion index count",
									ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	memset(&cx, 0, sizeof(cx));
	cx.cxt = cxt;
	cx.heap = heap;
	cx.snapshot = snapshot;

	/*
	 * The visibility-map page the last count of this relation left pinned
	 * (LionVisCache.vmbuf): the map of a large heap is a few pages, and
	 * every count of the node asks the same one or two, so a count takes the
	 * pin over rather than pinning the page again - a buffer lookup and two
	 * atomic operations on a buffer header that every participant of a
	 * parallel plan shares.  A pin of another relation's map is let go.
	 */
	cx.vmbuf = InvalidBuffer;
	if (cache != NULL && BufferIsValid(cache->vmbuf))
	{
		if (cache->vmrelid == RelationGetRelid(heap))
		{
			cx.vmbuf = cache->vmbuf;
			cache->vmbuf = InvalidBuffer;
			cache->vmrelid = InvalidOid;
		}
		else
			lion_vis_cache_release_vm(cache);
	}
	/* SerializationNeededForRead() begins with exactly this test; hoisting it
	 * lets non-serializable counts skip the per-block PredicateLockPage loop. */
	cx.serializable = IsolationIsSerializable();
	/*
	 * HOT STANDBY (DESIGN.md §9, §25).
	 *
	 * The §9 interlock is "a reader that holds a pin on the page a container
	 * came from cannot be overtaken by whatever removes that container's dead
	 * TIDs", and on the primary it holds because ambulkdelete takes a CLEANUP
	 * lock on every page it removes a TID from (§11), and a cleanup lock waits
	 * for pins.
	 *
	 * Replay of a GENERIC record takes an ordinary exclusive lock, which does
	 * not wait for pins, so on a standby a reader's pin does not stop
	 * ambulkdelete's records from being replayed and the heap records that
	 * follow can set all-visible while this backend still holds a copy of the
	 * old containers.  A generic-mode index therefore rechecks every candidate
	 * TID in the heap on a standby and never trusts the visibility map.
	 *
	 * Replay of an RMGR-mode record does take the cleanup lock: every record
	 * that removes a TID or deletes an item (VACUUM_PAGE, ITEM_DELETE,
	 * PAGE_DELETED, and the ENTRY record VACUUM deletes entries with) declares
	 * the page it removes them from in its cleanup mask, and lion_redo() takes
	 * that block with XLogReadBufferForRedoExtended(..., get_cleanup_lock =
	 * true).  §9's argument then reads on the standby exactly as it reads on
	 * the primary, with "VACUUM" replaced by "the startup process": while this
	 * backend holds a pin on the page it took a container from, replay cannot
	 * have removed a TID from that page, so it cannot have replayed the heap
	 * record that set any of that container's heap pages all-visible.
	 *
	 * That alone is NOT enough, and the reason is the page-split hole of
	 * §11: the TIDs this backend copied may since have MOVED - a split of the
	 * leaf to its right sibling, a root push-down (§22) to a new child, an
	 * INLINE payload spilled off the directory leaf - and be removed from the
	 * page they moved to, which this backend does not pin.  On the primary
	 * the removal cannot happen before VACUUM has held a cleanup lock on the
	 * page they came from, because VACUUM visits every page (and every page
	 * of a posting tree's descent) in chain order; but a page VACUUM visits
	 * without changing writes no record of its own.  So VACUUM carries those
	 * visits as a BARRIER on its next removal record (or in a VACUUM_VISIT
	 * record of their own), and redo cleanup-locks every page of it, one at a
	 * time with nothing else held, before it applies the removal (§25,
	 * xl_lion_visit in lion_wal.h).  A page is only ever linked in to the
	 * right of the page whose split created it and replay applies records in
	 * the order the primary wrote them, so replay meets the barrier for the
	 * page this backend pins before the removal that could hurt it -
	 * test/recovery/run.sh phase 3 parks a standby reader in exactly that
	 * window for the split, the push-down and the spill.  The reader pays for
	 * it the way a primary reader does: the standby's replay waits, which is
	 * a recovery conflict resolved by max_standby_streaming_delay rather than
	 * a wrong answer.
	 *
	 * ONE generic-mode source is enough to lose it, because a container of the
	 * intersection carries the dead TIDs of every source it came from, and the
	 * interlock has to hold for all of them.  So the map is trusted only when
	 * every index this count reads is in rmgr mode.
	 */
	cx.in_recovery = RecoveryInProgress() &&
		!lion_sources_all_rmgr(nsources, sources);

	/*
	 * NOPIN sets (DESIGN.md §15).  Materializing never takes the last carrier
	 * away (rule 2), so a merge with no positive source that holds a pin at
	 * every container key is one whose sets were located over the pin budget:
	 * an IN list under an OR across columns, say, or intersected only with
	 * sets that are themselves NOPIN.  Nothing then stops VACUUM from having
	 * removed a TID the copies still list and set its heap page all-visible,
	 * so the map is not asked at all and every candidate goes to the heap,
	 * as on a standby.  The sum and the one-set count do not need this: they
	 * locate a NOPIN set again, pinned, before they count it.
	 *
	 * The same goes for a union too wide for the open budget, which is read
	 * without pins (lion_wide_fill()).  Whether any positive source carries
	 * the interlock is therefore asked of the plans the cursors are built
	 * from, after the materialization above, by the merge path below - once,
	 * or once per batch of a list (lion_run_batches()).
	 */
	cx.novm = false;
	cx.rel_read_only = rel_read_only;
	cx.raw = true;
	cx.tids_sorted = true;
	cx.batchmax = lion_recheck_budget();
	cx.exists = exists;

	/*
	 * A collection counts nothing, so nothing it reads needs a pin once it
	 * has been copied: the cursors let go of every leaf as they go, as a
	 * bitmap walk's do (DESIGN.md §27).
	 */
	cx.collect = collect;
	if (collect != NULL)
		cx.droppins = true;

	/*
	 * The count's own key set, when source slot 0 is one (a group's set, an
	 * FK-side join's fk set): what stats.key_containers counts the reads of.
	 */
	cx.keyset = (collect == NULL && !sources[0].negated &&
				 sources[0].nsets == 1) ? &sources[0].sets[0] : NULL;

	/*
	 * The visibility cache, if the caller keeps one for this node execution.
	 * It is emptied here if it holds answers for another relation or another
	 * snapshot, so a partitioned count may hand the same handle to every
	 * partition (DESIGN.md §9 and §16).
	 */
	lion_vis_cache_begin(cache, heap, snapshot);
	cx.cache = cache;

	/*
	 * ... and the row filter the node asks of every count of this execution
	 * (DESIGN.md §17, "A query known only at run time").  A collection has no
	 * cache and is not filtered: it copies the superset, and the counts of
	 * that copy are.
	 */
	cx.filter = (cache != NULL) ? cache->filter : NULL;
	if (cx.filter != NULL && cx.filter->heap != heap)
		elog(ERROR, "lion index count: a row filter for another relation");

	if (summed)
	{
		/*
		 * One pass per entry, each with the source reduced to that one set.
		 * The passes share the recheck queue, the visibility-map pin and the
		 * visibility cache, so a dirty heap page is still visited once for the
		 * whole list and the batching of DESIGN.md §9 still bounds the memory.
		 */
		MemoryContext setcxt;

		/*
		 * One pass allocates a cursor, a staging buffer for the inline
		 * payload or a whole page image for a chain, and a buffer for the
		 * sparse segment it expands: up to ten kilobytes or so, a thousand
		 * times over for the longest list the planner allows.  A context that
		 * is RESET after each pass hands the same memory out again, so the
		 * pass runs in cache instead of walking a dozen megabytes of fresh
		 * memory.  The keeper block is sized to hold all of it, which is what
		 * makes the reset free.
		 */
		setcxt = AllocSetContextCreate(cxt, "lion index count entry",
									   32 * 1024, 32 * 1024,
									   ALLOCSET_DEFAULT_MAXSIZE);

		for (j = 0; j < sources[0].nsets; j++)
		{
			if (!sources[0].sets[j].found)
				continue;

			MemoryContextSwitchTo(setcxt);
			lion_count_one_set(&cx, &sources[0].sets[j]);
			MemoryContextSwitchTo(cxt);
			MemoryContextReset(setcxt);

			cx.stats.sets_summed++;

			/* An existence test needs one entry with a visible row (§26). */
			if (lion_exists_settled(&cx))
				break;
			CHECK_FOR_INTERRUPTS();
		}
		MemoryContextDelete(setcxt);
	}
	else if (oneset)
		lion_count_one_set(&cx, &sources[0].sets[0]);
	else if (batchsrc >= 0)
		lion_run_batches(&cx, nsources, sources, trees, batchsrc, &budget);
	else
	{
		LionNodePlan **plans;
		int			ncarried = 0;

		plans = (LionNodePlan **) palloc(sizeof(LionNodePlan *) * nsources);
		for (i = 0; i < nsources; i++)
		{
			plans[i] = lion_plan_build(trees[i], sources[i].sets, &budget,
									   false);
			if (!sources[i].negated && plans[i]->pinned)
				ncarried++;
		}
		cx.novm = (ncarried == 0);
		lion_run_merge(&cx, nsources, sources, plans);
	}

	/*
	 * Everything that could be answered from the visibility map has been;
	 * what is left of the last batch needs the heap and the snapshot.  No
	 * index page is pinned any more, which is fine: the decisions that needed
	 * a pin were all made above.
	 */
	lion_recheck_flush(&cx);
	result = cx.count + cx.recheck_count;

	/*
	 * The map page stays pinned for the next count, when there is a cache to
	 * keep it in (and nothing kept there since, which no count does).
	 */
	if (BufferIsValid(cx.vmbuf))
	{
		if (cache != NULL && !BufferIsValid(cache->vmbuf))
		{
			cache->vmbuf = cx.vmbuf;
			cache->vmrelid = RelationGetRelid(heap);
		}
		else
			ReleaseBuffer(cx.vmbuf);
	}

	MemoryContextSwitchTo(oldcxt);
	if (scratch)
	{
		MemoryContextReset(cxt);
		cache->scratchbusy = false;
	}
	else
		MemoryContextDelete(cxt);

	if (stats != NULL)
		lion_count_stats_add(stats, &cx.stats);

	return result;
}

void
lion_count_stats_add(LionCountStats *dst, const LionCountStats *src)
{
	dst->blocks_skipped_via_vm += src->blocks_skipped_via_vm;
	dst->tids_rechecked += src->tids_rechecked;
	dst->blocks_rechecked += src->blocks_rechecked;
	dst->containers_visited += src->containers_visited;
	dst->probes_avoided += src->probes_avoided;
	dst->unions_built += src->unions_built;
	dst->unions_probed += src->unions_probed;
	dst->trees_built += src->trees_built;
	dst->trees_probed += src->trees_probed;
	dst->cache_hits += src->cache_hits;
	dst->cache_full += src->cache_full;
	dst->sets_summed += src->sets_summed;
	dst->rows_removed += src->rows_removed;
	dst->key_containers += src->key_containers;
	dst->copy_containers += src->copy_containers;
	dst->copy_seeks += src->copy_seeks;
	dst->copy_file_reads += src->copy_file_reads;
	dst->vm_checks += src->vm_checks;
	dst->vm_pins += src->vm_pins;
}

bool
lion_sets_satisfiable(int nsets, LionPostingSet *sets, LionKeyNode *tree)
{
	LionCountSource src;

	memset(&src, 0, sizeof(src));
	src.nsets = nsets;
	src.sets = sets;
	src.tree = tree;

	return lion_source_satisfiable(lion_source_tree(&src), sets);
}

/*
 * Is every member of an INLINE payload on a heap block the visibility map
 * calls all-visible (DESIGN.md §37, "The rows of an entry")?  Then each is a
 * row every snapshot sees, and the entry's rows are its ntids.  The caller
 * holds a pin of the leaf the payload was copied from, which is the
 * interlock every count rests on (§9): VACUUM cannot take a TID out of the
 * entry and mark its page while the leaf is pinned, and a TID put in after
 * the snapshot was taken came with its page's mark taken off, which this
 * snapshot's xmin keeps off.  stage is a LION_CONTAINER_MAX_SIZE buffer the
 * items are read into.
 */
bool
lion_payload_all_visible(Relation heap, const char *payload, Size paylen,
						 Buffer *vmbuf, LionContainer *stage)
{
	Size		off = 0;

	while (lion_inline_fetch(payload, paylen, &off, stage) > 0)
	{
		if (stage->type == LION_CT_SPARSE)
		{
			uint32		n = lion_sparse_npairs(stage);
			const uint32 *ckeys = LION_SPARSE_CKEYS_CONST(stage);
			const uint16 *los = LION_SPARSE_LOS_CONST_AT(stage, n);
			uint32		i;

			for (i = 0; i < n; i++)
			{
				uint64		wanted = UINT64CONST(1) <<
					(los[i] >> LION_OFFSET_BITS);

				if ((lion_vm_allvisible_mask(heap,
											 lion_ckey_first_block(ckeys[i]),
											 wanted, vmbuf, NULL) &
					 wanted) != wanted)
					return false;
			}
		}
		else
		{
			uint64		wanted = lion_container_block_mask(stage);

			if ((lion_vm_allvisible_mask(heap,
										 lion_ckey_first_block(stage->ckey),
										 wanted, vmbuf, NULL) &
				 wanted) != wanted)
				return false;
		}
	}
	return true;
}

/*
 * The container keys of heap - one for every LION_BLOCKS_PER_CONTAINER of its
 * blocks - that hold a block the visibility map does not call all-visible, as
 * a byte each in a palloc'd array of *nkeys, with *nblocks the heap's size
 * (DESIGN.md §37).  The last key's blocks past the end count as all-visible.
 */
uint8 *
lion_heap_dirty_keys(Relation heap, BlockNumber *nblocks, uint32 *nkeys)
{
	Buffer		vmbuf = InvalidBuffer;
	uint8	   *dirty;
	uint32		k;

	*nblocks = RelationGetNumberOfBlocks(heap);
	*nkeys = (uint32) ((*nblocks + LION_BLOCKS_PER_CONTAINER - 1) /
					   LION_BLOCKS_PER_CONTAINER);
	dirty = (uint8 *) palloc0(Max(*nkeys, 1));
	for (k = 0; k < *nkeys; k++)
	{
		BlockNumber first = (BlockNumber) k * LION_BLOCKS_PER_CONTAINER;
		uint32		n = Min(*nblocks - first, LION_BLOCKS_PER_CONTAINER);
		uint64		wanted = (n == 64) ? ~UINT64CONST(0) :
			(UINT64CONST(1) << n) - 1;

		if ((lion_vm_allvisible_mask(heap, first, wanted, &vmbuf, NULL) &
			 wanted) != wanted)
			dirty[k] = 1;
	}
	if (BufferIsValid(vmbuf))
		ReleaseBuffer(vmbuf);
	return dirty;
}

/*
 * Does an INLINE payload have a member under one of the nkeys container keys
 * dirty marks, or past them?
 */
bool
lion_payload_touches(const char *payload, Size paylen, const uint8 *dirty,
					 uint32 nkeys, LionContainer *stage)
{
	Size		off = 0;

	while (lion_inline_fetch(payload, paylen, &off, stage) > 0)
	{
		if (stage->type == LION_CT_SPARSE)
		{
			uint32		n = lion_sparse_npairs(stage);
			const uint32 *ckeys = LION_SPARSE_CKEYS_CONST(stage);
			uint32		i;

			for (i = 0; i < n; i++)
			{
				if (ckeys[i] >= nkeys || dirty[ckeys[i]])
					return true;
			}
		}
		else if (stage->ckey >= nkeys || dirty[stage->ckey])
			return true;
	}
	return false;
}
