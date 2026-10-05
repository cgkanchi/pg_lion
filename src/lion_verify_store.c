/*-------------------------------------------------------------------------
 *
 * lion_verify_store.c
 *		lion_index_verify()'s checks of the window store (DESIGN.md §40):
 *		the meta page's record, the map, every chain and every page on it,
 *		an ordered index's windows whole (§41), the store pages no walk
 *		reached, and - with heapallindexed - the value of every stored
 *		column of every row the check's snapshot sees.
 *
 * Part of the SQL-callable helpers of the lion index; lion_funcs.h
 * describes them and declares what their files share.
 *
 * WRITERS BESIDE THE CHECK.  On a primary the check holds
 * ShareUpdateExclusiveLock, so VACUUM - the only thing that clears a map
 * slot or frees a store page - is locked out, and the only changes the walk
 * can meet are an insert's: a slot set, a map page added, a store page
 * rewritten, split, or appended to its chain.  A page's range never moves
 * left (a split keeps the lower part, a new page takes a range past every
 * other), so a chain walked one page at a time still shows ranges that
 * follow on from each other, and a page a writer made behind the walk is
 * linked by the time the walk is over: the reachability pass asks its chain
 * (lion_verify_store_classify()).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_funcs.h"
#include "lion_store.h"

/*
 * The meta page's store record (DESIGN.md §40, "Format"): present exactly on
 * a version 9 meta page, and naming the columns the relation cache's state
 * was built from.  metapage is held SHARE by lion_verify_meta().
 */
void
lion_verify_store_meta(LionVerifyState *vs, Page metapage)
{
	LionMetaPageData *meta = LionPageGetMeta(metapage);
	LionMetaStore *ms;

	memset(&vs->store, 0, sizeof(LionMetaStore));
	if (!LION_META_HAS_STORE(meta))
	{
		if (vs->ix->nstored != 0)
			lion_corrupt("lion index \"%s\": meta page version %u has no window store, but the index stores %d columns",
						RelationGetRelationName(vs->index), meta->version,
						vs->ix->nstored);
		return;
	}

	if (!LionMetaHasStoreArea(metapage))
		lion_corrupt("lion index \"%s\": meta page version %u ends at byte %u, before its store record",
					RelationGetRelationName(vs->index), meta->version,
					((PageHeader) metapage)->pd_lower);
	ms = LionPageGetMetaStore(metapage);
	memcpy(&vs->store, ms, sizeof(LionMetaStore));

	if (vs->store.store_cols == 0 ||
		vs->store.store_cols != vs->ix->store.store_cols)
		lion_corrupt("lion index \"%s\": meta page names stored columns %08X, expected %08X",
					RelationGetRelationName(vs->index), vs->store.store_cols,
					vs->ix->store.store_cols);
	if (vs->store.store_max_len > LION_MAX_STORE_MAX_LEN)
		lion_corrupt("lion index \"%s\": meta page has store_max_len %u, at most %d",
					RelationGetRelationName(vs->index),
					vs->store.store_max_len, LION_MAX_STORE_MAX_LEN);
	if (vs->store.store_root == LION_METAPAGE_BLKNO ||
		!lion_verify_block_exists(vs, vs->store.store_root) ||
		vs->store.store_root != vs->ix->store.store_root)
		lion_corrupt("lion index \"%s\": meta page names window map root %u",
					RelationGetRelationName(vs->index), vs->store.store_root);

	/* the order of a version 10 index (§41), and of no other */
	if (LION_META_HAS_ORDER(meta))
	{
		LionMetaStoreOrder *mo;

		if (!LionMetaHasOrderArea(metapage))
			lion_corrupt("lion index \"%s\": meta page version %u ends at byte %u, before its order record",
						RelationGetRelationName(vs->index), meta->version,
						((PageHeader) metapage)->pd_lower);
		mo = LionPageGetMetaStoreOrder(metapage);
		if (mo->order_ord < 0 || mo->order_ord >= vs->ix->nstored ||
			mo->order_ord != vs->ix->store_order ||
			(mo->order_flags & ~LION_STORE_ORDER_CLUSTER) != 0 ||
			mo->order_cols != lion_store_order_cols(vs->ix) ||
			mo->reserved[0] != 0 || mo->reserved[1] != 0)
			lion_corrupt("lion index \"%s\": meta page names order column ordinal %d with flags %u and columns 0x%x, expected ordinal %d",
						RelationGetRelationName(vs->index), (int) mo->order_ord,
						(unsigned) mo->order_flags, mo->order_cols,
						vs->ix->store_order);
	}
	else if (vs->ix->store_order >= 0)
		lion_corrupt("lion index \"%s\": meta page version %u has no order, but the index is ordered by ordinal %d",
					RelationGetRelationName(vs->index), meta->version,
					vs->ix->store_order);
}

/* Read and check map page blk, (level, number), and copy its entries. */
static void
lion_verify_store_map_page(LionVerifyState *vs, BlockNumber blk, uint16 level,
						   uint64 number, BlockNumber *entries)
{
	Buffer		buf;
	Page		page;
	char	   *msg;
	uint32		i;

	lion_verify_visit(vs, blk, "the window map");
	page = lion_verify_read_page(vs, blk, LION_PAGE_STOREMAP, &buf);
	if ((msg = lion_storemap_page_check(page, level, number)) != NULL)
		lion_corrupt("lion index \"%s\": block %u, page " UINT64_FORMAT " of level %u of the window map, %s",
					RelationGetRelationName(vs->index), blk, number,
					(unsigned) level, msg);
	memcpy(entries, lion_storemap_page_entries(page), LION_STOREMAP_ITEM_SIZE);
	UnlockReleaseBuffer(buf);
	vs->nmappages++;

	for (i = 0; i < LION_STOREMAP_FANOUT; i++)
		if (entries[i] != 0 && !lion_verify_block_exists(vs, entries[i]))
			lion_corrupt("lion index \"%s\": entry %u of window map page %u points at block %u, but the index has only %u blocks",
						RelationGetRelationName(vs->index), i, blk, entries[i],
						vs->nblocks);
}

/*
 * The chain of map slot `slot`: every page a live store page of its window
 * and column, sound (lion_store_page_check() and its codes), the head's range
 * starting at the window's first heap page and every next one where the last
 * ended.
 */
static void
lion_verify_store_chain(LionVerifyState *vs, uint64 slot, BlockNumber head)
{
	LionIndexState *ix = vs->ix;
	int			stride = lion_store_stride(ix);
	uint32		ckey = (uint32) (slot / (uint64) stride);
	int			ord = (int) (slot % (uint64) stride);
	const LionStoreCol *col = (ord < ix->nstored) ? &ix->stored[ord] : ix->storeperm;
	int			npagesmax = lion_store_col_pages(col);
	BlockNumber blk = head;
	int			nexthi = 0;
	int			npages = 0;

	while (BlockNumberIsValid(blk))
	{
		Buffer		buf;
		Page		page;
		LionStoreHeader *h;
		char	   *msg;
		BlockNumber next;

		lion_verify_visit(vs, blk, "a store chain");
		page = lion_verify_read_page(vs, blk, LION_PAGE_STORE, &buf);
		if (LionPageIsDeleted(page))
			lion_corrupt("lion index \"%s\": block %u on the store chain of window %u column %d is a freed page",
						RelationGetRelationName(vs->index), blk, ckey,
						col->attno);
		if (!lion_store_page_owned(page, ckey, (uint16) ord))
			lion_corrupt("lion index \"%s\": block %u on the store chain of window %u column %d belongs to window %u ordinal %u",
						RelationGetRelationName(vs->index), blk, ckey,
						col->attno, LionPageGetOpaque(page)->owner_head,
						LionPageGetOpaque(page)->owner_hash);
		if ((msg = lion_store_page_check(page, col)) != NULL ||
			(msg = lion_store_page_check_codes(page)) != NULL)
			lion_corrupt("lion index \"%s\": store page %u %s",
						RelationGetRelationName(vs->index), blk, msg);

		h = lion_store_page_header(page);
		if ((int) h->lo != nexthi)
			lion_corrupt("lion index \"%s\": store page %u of window %u column %d covers heap pages %u to %u, expected its range to start at %d",
						RelationGetRelationName(vs->index), blk, ckey,
						col->attno, (unsigned) h->lo, (unsigned) h->hi,
						nexthi);
		nexthi = (int) h->hi + 1;
		next = LionPageGetOpaque(page)->rightlink;
		UnlockReleaseBuffer(buf);

		vs->nstorepages++;
		if (++npages > npagesmax ||
			(BlockNumberIsValid(next) && nexthi >= npagesmax))
			lion_corrupt("lion index \"%s\": the store chain of window %u column %d goes on past heap page %d at block %u",
						RelationGetRelationName(vs->index), ckey, col->attno,
						nexthi - 1, blk);
		blk = next;
		CHECK_FOR_INTERRUPTS();
	}
}

void
lion_verify_store(LionVerifyState *vs)
{
	BlockNumber *rootents;
	BlockNumber *innerents;
	BlockNumber *leafents;
	uint32		a;
	int			stride = lion_store_stride(vs->ix);
	int64		lastwin = -1;

	vs->storecxt = CurrentMemoryContext;
	if (vs->store.store_cols == 0)
		return;

	rootents = (BlockNumber *) palloc(LION_STOREMAP_ITEM_SIZE);
	innerents = (BlockNumber *) palloc(LION_STOREMAP_ITEM_SIZE);
	leafents = (BlockNumber *) palloc(LION_STOREMAP_ITEM_SIZE);

	/*
	 * The map is walked from copies of its pages: a slot an insert sets after
	 * the copy is a window whose whole chain is new, and its pages are
	 * settled with the other unreached ones.
	 */
	lion_verify_store_map_page(vs, vs->store.store_root, LION_STOREMAP_ROOT, 0,
							   rootents);
	for (a = 0; a < LION_STOREMAP_FANOUT; a++)
	{
		uint32		b;

		if (rootents[a] == 0)
			continue;
		lion_verify_store_map_page(vs, rootents[a], LION_STOREMAP_INNER, a,
								   innerents);
		for (b = 0; b < LION_STOREMAP_FANOUT; b++)
		{
			uint64		leafno = (uint64) a * LION_STOREMAP_FANOUT + b;
			uint32		c;

			if (innerents[b] == 0)
				continue;
			lion_verify_store_map_page(vs, innerents[b], LION_STOREMAP_LEAF,
									   leafno, leafents);
			for (c = 0; c < LION_STOREMAP_FANOUT; c++)
			{
				uint64		slot = leafno * LION_STOREMAP_FANOUT + c;

				if (leafents[c] == 0)
					continue;

				/*
				 * An ordered index's windows (§41) are checked whole once
				 * each of their chains has been on its own.
				 */
				if (vs->ix->store_order >= 0 && lastwin >= 0 &&
					(int64) (slot / (uint64) stride) != lastwin)
					lion_store_verify_window(vs->index, vs->ix, (uint32) lastwin);
				lastwin = (int64) (slot / (uint64) stride);
				lion_verify_store_chain(vs, slot, leafents[c]);
			}
		}
	}

	if (vs->ix->store_order >= 0 && lastwin >= 0)
		lion_store_verify_window(vs->index, vs->ix, (uint32) lastwin);

	pfree(leafents);
	pfree(innerents);
	pfree(rootents);
}

/*
 * A store or map page no walk reached: one an insert linked in behind the
 * walk, which its chain or the map says now; for a store page that neither
 * does, an orphan of a cleanup that stopped between clearing a window's map
 * slot and freeing the rest of its chain, which the next VACUUM's sweep
 * frees (§40, "VACUUM").  A map page is never freed, so one the map does
 * not link is damage.  buf is held SHARE, and released.
 */
LionVerifyUnref
lion_verify_store_classify(LionVerifyState *vs, BlockNumber blk, Page page,
						   Buffer buf)
{
	LionPageOpaque opaque = LionPageGetOpaque(page);
	uint32		owner = opaque->owner_head;
	uint32		ord = opaque->owner_hash;
	uint16		level = opaque->level;
	bool		store = LionPageIsStore(page);

	UnlockReleaseBuffer(buf);

	if (vs->store.store_cols == 0)
		return LION_UNREF_LIVE;	/* a store page in an index without one */

	if (!store)
	{
		if (lion_storemap_linked(vs->index, vs->ix, blk, level, owner))
			return LION_UNREF_LINKED;
		return LION_UNREF_LIVE;
	}

	if (ord >= (uint32) lion_store_stride(vs->ix))
		return LION_UNREF_LIVE;
	if (lion_store_page_linked(vs->index, vs->ix, blk, owner, (uint16) ord))
		return LION_UNREF_LINKED;
	return LION_UNREF_LEAK;
}

/*
 * heapallindexed: the stored values of a row the snapshot sees are its own
 * (§40, "Why it is safe": the insert wrote them before the TID reached any
 * posting set, and nothing but a later insert of the same TID - which needs
 * the row dead and VACUUM, locked out, to have run - writes the slot again).
 * The message says which column and not what either value is, for the
 * reason lion_verify_one_key() gives.
 */
void
lion_verify_store_heap(LionVerifyState *vs, ItemPointer tid, Datum *values,
					   bool *isnull)
{
	LionIndexState *ix = vs->ix;
	int			ord;

	if (vs->readers == NULL)
		vs->readers = (struct LionStoreReader **)
			MemoryContextAllocZero(vs->storecxt,
								   sizeof(LionStoreReader *) * ix->nstored);

	for (ord = 0; ord < ix->nstored; ord++)
	{
		const LionStoreCol *col = &ix->stored[ord];
		int			r;

		if (vs->readers[ord] == NULL)
			vs->readers[ord] = lion_store_open(vs->index, ix, ord, vs->storecxt);
		r = lion_store_compare(vs->readers[ord], tid, values[col->attno - 1],
							   isnull[col->attno - 1]);
		if (r == LION_STORE_CMP_DIFFERENT)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("heap tuple (%u,%u) from table \"%s\" has another value of column %d in the store of \"%s\"",
							ItemPointerGetBlockNumber(tid),
							ItemPointerGetOffsetNumber(tid),
							RelationGetRelationName(vs->heap), col->attno,
							RelationGetRelationName(vs->index))));
		if (r == LION_STORE_CMP_MISSING)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("heap tuple (%u,%u) from table \"%s\" has no value of column %d in the store of \"%s\"",
							ItemPointerGetBlockNumber(tid),
							ItemPointerGetOffsetNumber(tid),
							RelationGetRelationName(vs->heap), col->attno,
							RelationGetRelationName(vs->index))));
	}
}

/* Close the readers, and say what the store walk saw. */
void
lion_verify_store_end(LionVerifyState *vs)
{
	int			ord;

	if (vs->readers != NULL)
	{
		for (ord = 0; ord < vs->ix->nstored; ord++)
			if (vs->readers[ord] != NULL)
				lion_store_close(vs->readers[ord]);
		pfree(vs->readers);
		vs->readers = NULL;
	}
	if (vs->store.store_cols != 0)
		elog(DEBUG1, "lion index \"%s\": window store of %d columns, %lld map pages and %lld store pages reached, %u recorded",
			 RelationGetRelationName(vs->index), vs->ix->nstored,
			 (long long) vs->nmappages, (long long) vs->nstorepages,
			 vs->store.store_pages);
}
