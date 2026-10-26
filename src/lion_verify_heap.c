/*-------------------------------------------------------------------------
 *
 * lion_verify_heap.c
 *		lion_index_verify(): the recheck of the candidates a concurrent writer
 *		may explain (DESIGN.md §7: suspect, wait, recheck), and heapallindexed.
 *
 * Part of the SQL-callable helpers of the lion index; lion_funcs.h
 * describes them and declares what their files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_funcs.h"

/* ---------------------------------------------------------------------
 * The recheck (DESIGN.md §7: suspect, wait, recheck)
 * --------------------------------------------------------------------- */

/*
 * Wait for the statements that are writing the index.
 *
 * An INSERT, an UPDATE or a COPY takes RowExclusiveLock on each index of its
 * table when it puts its first row into it (ExecOpenIndices()) and lets it go
 * when the statement ends (ExecCloseIndices()) - unlike its lock on the
 * table, which lasts until the transaction ends.  Every change a writer makes
 * to this index is made under that lock, and a statement that has ended has
 * finished every split it began, or left it flagged for good, and linked
 * every page it took.  So waiting - in ShareLock, the weakest mode that
 * conflicts with RowExclusiveLock, and CREATE INDEX CONCURRENTLY's own - for
 * everyone who holds a conflicting lock on the INDEX is waiting for exactly
 * the writes that may have been in flight while the walk ran.  WaitForLockers()
 * waits for each holder's whole transaction, which is more than needed and the
 * only unit it offers.  It leaves this backend out, and nobody else can hold a
 * stronger lock: the ShareUpdateExclusiveLock held here conflicts with all of
 * them.
 *
 * CIC waits on the TABLE's lock instead, because it has to outwait every
 * transaction that might still insert without knowing the new index.  This
 * check need not: a transaction that wrote the table and sits idle, or
 * prepared, holds no lock on the index any more and is not waited for.
 *
 * The wait is a lock wait on each writer's virtual transaction id, so
 * lock_timeout ends it as well as statement_timeout.
 */
void
lion_verify_wait_for_writers(LionVerifyState *vs)
{
	LOCKTAG		tag;

	SET_LOCKTAG_RELATION(tag, vs->index->rd_lockInfo.lockRelId.dbId,
						 vs->index->rd_lockInfo.lockRelId.relId);
	WaitForLockers(tag, ShareLock, false);
}

/*
 * A downlink of level cand->level + 1 names a page the walk of cand->level did
 * not reach (lion_verify_filter_downlinks()).  On a sound index that page is
 * a split's new sibling, and a split links its new page immediately right of
 * the page it splits: so it lies in the level's right-link chain between the
 * reached page whose downlink came before it and the reached page whose
 * downlink came after it.  Walk there, each page held until its right sibling
 * is locked, which also proves every left link on the way (see
 * lion_verify_leftlink()).  Pages only ever enter the chain, and the walk
 * starts at a page the level walk reached, so anything a writer does now
 * cannot hide the page from it.
 */
static void
lion_verify_recheck_downlink(LionVerifyState *vs, const LionVerifyCand *cand)
{
	uint16		kind = (cand->level == 0) ? LION_PAGE_BUCKET : LION_PAGE_DIR;
	BlockNumber cur = cand->left;
	BlockNumber steps = 0;
	Buffer		buf;
	Page		page;

	Assert(BlockNumberIsValid(cand->left));

	page = lion_verify_read_page(vs, cur, kind, &buf);
	for (;;)
	{
		BlockNumber next = LionPageGetOpaque(page)->rightlink;
		Buffer		nbuf;
		Page		npage;

		if (!BlockNumberIsValid(next) || next == cand->right ||
			++steps > vs->nblocks)
		{
			if (BlockNumberIsValid(cand->right))
				lion_corrupt("lion index \"%s\": downlink %d of level %u points at block %u, which is not a page of level %u between blocks %u and %u",
							RelationGetRelationName(vs->index), cand->downlink,
							cand->level + 1, cand->blk, cand->level, cand->left,
							cand->right);
			lion_corrupt("lion index \"%s\": downlink %d of level %u points at block %u, which is not a page of level %u after block %u",
						RelationGetRelationName(vs->index), cand->downlink,
						cand->level + 1, cand->blk, cand->level, cand->left);
		}

		npage = lion_verify_read_page(vs, next, kind, &nbuf);
		if (LionPageGetOpaque(npage)->level != cand->level)
			lion_corrupt("lion index \"%s\": directory page %u is at level %u, expected %u",
						RelationGetRelationName(vs->index), next,
						LionPageGetOpaque(npage)->level, cand->level);
		if (LionPageGetOpaque(npage)->leftlink != cur)
			lion_corrupt("lion index \"%s\": directory page %u has left link %u, expected %u",
						RelationGetRelationName(vs->index), next,
						LionPageGetOpaque(npage)->leftlink, cur);
		UnlockReleaseBuffer(buf);
		buf = nbuf;
		page = npage;
		cur = next;
		if (cur == cand->blk)
			break;
		CHECK_FOR_INTERRUPTS();
	}
	UnlockReleaseBuffer(buf);
}

/*
 * Mark in found[] which of the n blocks in roots[] (sorted) an entry of the
 * directory names as the root of one of its trees: its posting set's, or -
 * in a column that stores positions - its position tree's (DESIGN.md §17).
 * Both come from the end of the relation, and both can grow out of the free
 * space map before the walk is over.  One walk of the leaves, with nothing
 * but the entries' headers (and position extensions) read: an entry the walk
 * has not reached yet may move right in a split, but only onto a page the
 * walk still comes to, so it is seen.
 *
 * A key inserted during the walk has both roots out of the end of the
 * relation, and its position tree can take leaves from the free space map:
 * those leaves are settled only if its position root is found here.
 */
static void
lion_verify_find_heads(LionVerifyState *vs, const BlockNumber *roots, int n,
					   bool *found)
{
	BlockNumber blk = lion_dir_leftmost_leaf(vs->index, vs->ix);
	BlockNumber steps = 0;

	while (BlockNumberIsValid(blk))
	{
		Buffer		buf;
		Page		page;
		OffsetNumber maxoff;
		OffsetNumber off;
		BlockNumber next;

		page = lion_verify_read_page(vs, blk, LION_PAGE_BUCKET, &buf);
		maxoff = PageGetMaxOffsetNumber(page);
		for (off = lion_page_first_data(page); off <= maxoff; off++)
		{
			ItemId		iid = lion_verify_itemid(vs, blk, page, off);
			LionEntryTuple *e;
			BlockNumber *hit;
			BlockNumber ehead;

			if (!ItemIdIsUsed(iid) || ItemIdGetLength(iid) < LION_ENTRY_SHORT_HDRSZ)
				continue;
			e = (LionEntryTuple *) PageGetItem(page, iid);
			if ((e->flags & LION_ENTRY_CHAIN) == 0 ||
				ItemIdGetLength(iid) < LION_ENTRY_CHAIN_HDRSZ)
				continue;
			ehead = lion_entry_head(e);
			hit = (BlockNumber *) bsearch(&ehead, roots, n, sizeof(BlockNumber),
										  lion_verify_blkcmp);
			if (hit != NULL)
				found[hit - roots] = true;

			/* the extension lies inside the item, before its payload */
			if ((e->flags & LION_ENTRY_POSITIONS) != 0 &&
				ItemIdGetLength(iid) >= LionEntryPayloadOffset(e))
			{
				ehead = lion_entry_posext(e)->pos_root;
				hit = (BlockNumber *) bsearch(&ehead, roots, n,
											  sizeof(BlockNumber),
											  lion_verify_blkcmp);
				if (hit != NULL)
					found[hit - roots] = true;
			}
		}
		next = LionPageGetOpaque(page)->rightlink;
		UnlockReleaseBuffer(buf);

		if (++steps > vs->nblocks)
			lion_corrupt("lion index \"%s\": the right links of the directory leaves go round in a cycle",
						RelationGetRelationName(vs->index));
		blk = next;
		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * The first key a search for a page of a posting set or a position tree
 * uses: a leaf's minckey, an internal page's first separator.  False when it
 * has none - an empty leaf, an internal page without a downlink.  The page's
 * header bounds have been checked.
 */
static bool
lion_verify_tree_page_key(LionVerifyState *vs, BlockNumber blk, Page page,
						  uint32 *ckey)
{
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	OffsetNumber first = lion_posting_first_data(page);
	ItemId		iid;

	if (maxoff < FirstOffsetNumber)
		return false;
	if (LionPageGetOpaque(page)->level == 0)
	{
		*ckey = LionPageGetOpaque(page)->minckey;
		return true;
	}
	if (first > maxoff)
		return false;

	/*
	 * The line pointer is vouched for before the pivot is read, as every
	 * other item this file reads is: its length alone says nothing about
	 * WHERE it points, and a damaged offset put the "pivot" past the end of
	 * the page.
	 */
	iid = lion_verify_itemid(vs, blk, page, first);
	if (!ItemIdIsUsed(iid) || ItemIdGetLength(iid) != LION_POSTING_PIVOT_SIZE)
		return false;
	*ckey = lion_posting_pivot(page, first)->ckey;
	return true;
}

/*
 * What a page no walk reached is, read again after the wait: its kind, its
 * level, and the first key a search for it would use.  False when it has no
 * first key to search for.
 */
typedef struct LionVerifyUnreached
{
	bool		isdir;
	uint16		level;
	BlockNumber ohead;			/* a posting or position page's root */
	uint32		ckey;			/* ... and its first key */
	LionEntryTuple *first;		/* a directory page's first item (a copy) */
} LionVerifyUnreached;

static bool
lion_verify_read_unreached(LionVerifyState *vs, BlockNumber blk,
						   LionVerifyUnreached *u)
{
	Buffer		buf;
	Page		page;
	bool		ok = false;

	memset(u, 0, sizeof(*u));
	buf = ReadBuffer(vs->index, blk);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);

	if (!PageIsNew(page) && PageGetSpecialSize(page) == LION_SPECIAL_SIZE &&
		LionPageGetOpaque(page)->page_id == LION_PAGE_ID &&
		!LionPageIsDeleted(page))
	{
		LionPageOpaque opaque = LionPageGetOpaque(page);
		OffsetNumber maxoff;

		/* a page nothing has checked yet: its header and flags first */
		lion_verify_page_bounds(vs, blk, page);
		(void) lion_verify_page_flags(vs, blk, page);
		maxoff = PageGetMaxOffsetNumber(page);

		u->level = opaque->level;
		if (LionPageIsBucket(page) || LionPageIsDir(page))
		{
			OffsetNumber first = lion_page_first_data(page);

			u->isdir = true;
			if (first <= maxoff)
			{
				int			i;

				/* a page nothing has checked yet: vouch for its items first */
				for (i = FirstOffsetNumber; i <= maxoff; i++)
					(void) lion_verify_dir_item(vs, blk, page, (OffsetNumber) i);

				/*
				 * Its first KEY: an internal page's first downlink is minus
				 * infinity, which a search lands on the leftmost page with.
				 */
				first = lion_dir_first_keyed(page);
				if (first <= maxoff && ItemIdIsUsed(PageGetItemId(page, first)))
				{
					u->first = lion_verify_copy_item(page, first);
					ok = true;
				}
			}
		}
		else if (LionPageIsContainer(page))
		{
			u->ohead = opaque->owner_head;
			ok = lion_verify_tree_page_key(vs, blk, page, &u->ckey);
		}
	}

	UnlockReleaseBuffer(buf);
	return ok;
}

/*
 * Is blk on `level` of the tree - a posting set, or a position tree - rooted
 * at root, where a search for its first key ckey goes?  The search takes the
 * route of what sorts just BELOW ckey (lion_posting_search_level() with
 * before), which lands at or left of every page of the level whose first key
 * is ckey, and the level is then walked right to blk, or to the first page
 * whose own first key is above ckey, past which blk cannot be: the first
 * keys of a level do not descend from left to right.  Each step lets go of
 * the page before it takes the next, as a reader's does; pages only ever
 * enter a level while VACUUM is locked out, and a split puts its new page
 * right of the one it splits, so a page right of the one let go is still
 * right of it.
 *
 * A search for ckey itself does not land on blk in a position tree whenever
 * ckey repeats.  Its keys are heap blocks, and one block's members can fill
 * more than one leaf (lion_postree.c): a leaf's first key is then its left
 * neighbour's last, where a walk right along the leaves while their last key
 * is below ckey stops - one leaf left of blk - and leaves can share a
 * separator, where the descent goes to the last of them, right of blk.  Such
 * a leaf is what an insert splits off an existing key's position tree.  A
 * posting set's container keys do not repeat, but its separators can (a
 * split whose halves are both empty by the time it is repaired gives the
 * right one the left one's, DESIGN.md §22), and the walk costs it nothing:
 * blk is the page the search lands on, or the one after it.
 */
static bool
lion_verify_on_level(LionVerifyState *vs, BlockNumber root, BlockNumber blk,
					 uint32 ckey, uint16 level)
{
	Buffer		buf = lion_posting_search_level(vs->index, root, ckey, level,
												true);
	BlockNumber steps = 0;

	while (BufferIsValid(buf))
	{
		Page		page = BufferGetPage(buf);
		BlockNumber cur = BufferGetBlockNumber(buf);
		BlockNumber next = LionPageGetOpaque(page)->rightlink;
		uint32		first;

		if (cur == blk)
		{
			UnlockReleaseBuffer(buf);
			return true;
		}

		/* a page of the tree, perhaps one nothing has checked yet */
		lion_verify_page_bounds(vs, cur, page);
		if (LionPageIsRightmost(page) || ++steps > vs->nblocks ||
			(lion_verify_tree_page_key(vs, cur, page, &first) && first > ckey))
			break;

		UnlockReleaseBuffer(buf);
		buf = InvalidBuffer;
		CHECK_FOR_INTERRUPTS();
		if (!lion_verify_block_exists(vs, next))
			break;
		buf = ReadBuffer(vs->index, next);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (!lion_page_owns_tree(page, root) ||
			LionPageGetOpaque(page)->level != level)
			break;
	}

	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
	return false;
}

/*
 * Is blk, a page no walk reached, part of its tree now that the writers that
 * were in flight are done?  A search for the page's own first key has to land
 * on it: from the directory's root for a directory page; from its tree's root
 * for a page of a posting set or a position tree - a root that is itself
 * reached, or named by an entry (rootfound) - where the search lands at or
 * left of it and its level is walked to it (lion_verify_on_level()).  A root
 * is part of the index when an entry names it.  A search races with the
 * writers that started since the wait, and the page's first key can move
 * right in a split of the page, so a miss is tried again, with the key read
 * again, a few times.
 */
static bool
lion_verify_confirm(LionVerifyState *vs, BlockNumber blk,
					const BlockNumber *roots, int nroots, const bool *rootfound)
{
	int			attempt;

	for (attempt = 0; attempt < LION_VERIFY_SET_ATTEMPTS; attempt++)
	{
		LionVerifyUnreached u;
		Buffer		buf;
		bool		hit;

		if (!lion_verify_read_unreached(vs, blk, &u))
			return false;

		if (u.isdir)
		{
			LionSearchKey sk;

			lion_search_key_exact(vs->ix, &sk, u.first);
			buf = lion_dir_search_level(vs->index, vs->ix, &sk, u.level);
			pfree(u.first);
		}
		else
		{
			BlockNumber *r;

			if (u.ohead == blk)
			{
				r = (BlockNumber *) bsearch(&blk, roots, nroots, sizeof(BlockNumber),
											lion_verify_blkcmp);
				return r != NULL && rootfound[r - roots];
			}

			/* its root has to be part of the index first */
			if (!(u.ohead < vs->nblocks && vs->refs[u.ohead] != 0))
			{
				r = (BlockNumber *) bsearch(&u.ohead, roots, nroots,
											sizeof(BlockNumber),
											lion_verify_blkcmp);
				if (r == NULL || !rootfound[r - roots])
					return false;
			}
			if (lion_verify_on_level(vs, u.ohead, blk, u.ckey, u.level))
				return true;
			CHECK_FOR_INTERRUPTS();
			continue;
		}

		if (!BufferIsValid(buf))
			continue;
		hit = (BufferGetBlockNumber(buf) == blk);
		UnlockReleaseBuffer(buf);
		if (hit)
			return true;
		CHECK_FOR_INTERRUPTS();
	}

	return false;
}

/*
 * Settle the candidates the walk recorded, and report what is still wrong.
 *
 * Only when there are candidates does the check wait for the writers that
 * were in flight (lion_verify_wait_for_writers()); an index nobody wrote to
 * while it was walked has none.  After the wait every change a writer made
 * behind the walk is complete - its pages linked, its splits finished or
 * flagged - and each candidate is checked again by itself.  A page that is
 * still unreachable is reported as the walk would have reported it: a leak
 * with a WARNING when it is a kind VACUUM's sweep frees, and corruption
 * otherwise.
 */
void
lion_verify_recheck(LionVerifyState *vs)
{
	BlockNumber *roots;
	bool	   *rootfound;
	int			nroots = 0;
	int			i;

	if (vs->ncands == 0)
		return;

	vs->nwaits++;
	lion_verify_wait_for_writers(vs);

	for (i = 0; i < vs->ncands; i++)
	{
		if (vs->cands[i].kind == LION_VCAND_DOWNLINK)
			lion_verify_recheck_downlink(vs, &vs->cands[i]);
	}

	/*
	 * The roots no walk reached are found through the entries that name them,
	 * all in one walk of the leaves: an unreached page that is a root itself,
	 * and the root an unreached posting or position page is stamped with.
	 * The second is not a candidate in its own right when a writer made it:
	 * the root of a posting set or of a position tree always comes from the
	 * end of the relation (§18, §17), past the blocks the reachability pass
	 * looks at, while the pages the tree grows by later may come out of the
	 * free space map, well inside them.  A new key, or an entry the walk read
	 * INLINE and a writer spilled, is exactly that - twice over in a column
	 * that stores positions.
	 */
	roots = (BlockNumber *) palloc(sizeof(BlockNumber) * vs->ncands);
	for (i = 0; i < vs->ncands; i++)
	{
		LionVerifyUnreached u;

		if (vs->cands[i].kind != LION_VCAND_UNREACHED)
			continue;
		if (lion_verify_read_unreached(vs, vs->cands[i].blk, &u))
		{
			if (u.isdir)
				pfree(u.first);
			else if (u.ohead == vs->cands[i].blk ||
					 !(u.ohead < vs->nblocks && vs->refs[u.ohead] != 0))
				roots[nroots++] = u.ohead;
		}
	}
	if (nroots > 0)
	{
		int			k = 0;

		qsort(roots, nroots, sizeof(BlockNumber), lion_verify_blkcmp);
		for (i = 0; i < nroots; i++)
		{
			if (k == 0 || roots[i] != roots[k - 1])
				roots[k++] = roots[i];
		}
		nroots = k;
	}
	rootfound = (bool *) palloc0(sizeof(bool) * Max(nroots, 1));
	if (nroots > 0)
		lion_verify_find_heads(vs, roots, nroots, rootfound);

	for (i = 0; i < vs->ncands; i++)
	{
		BlockNumber blk = vs->cands[i].blk;

		if (vs->cands[i].kind != LION_VCAND_UNREACHED)
			continue;
		if (lion_verify_confirm(vs, blk, roots, nroots, rootfound))
			continue;

		switch (lion_verify_classify(vs, blk, NULL))
		{
			case LION_UNREF_FREE:
				break;
			case LION_UNREF_LEAK:
			case LION_UNREF_INTERNAL:
				lion_verify_warn_leak(vs, blk);
				break;
			case LION_UNREF_LIVE:
			case LION_UNREF_FOREIGN:
				lion_verify_unreachable(vs, blk);
				break;
		}
	}

	pfree(rootfound);
	pfree(roots);
}

/* ---------------------------------------------------------------------
 * heapallindexed
 * --------------------------------------------------------------------- */

/* What a lookup of a heap tuple under one of its keys found. */
typedef enum LionVerifyFound
{
	LION_VFOUND_ABSENT,			/* the TID is not under the key */
	LION_VFOUND_PRESENT,		/* it is, with the positions it has to have */
	LION_VFOUND_NOPOS,			/* it is, and the key has no positions for it */
	LION_VFOUND_OTHERPOS		/* it is, with positions that are not its own */
} LionVerifyFound;

/*
 * What a TID found under its key comes to, given expect, the member its row
 * gives the key in a column that stores positions, and stored, the member
 * the key holds for it when `have` says it holds one.  Both are in
 * lion_posmember_from_key()'s form - positions ascending, no repeats - so
 * the same positions with the same weights are the same bytes.
 */
static LionVerifyFound
lion_verify_found_positions(const LionPosMember *expect, bool have,
							const LionPosMember *stored)
{
	if (expect == NULL)
		return LION_VFOUND_PRESENT;
	if (!have)
		return LION_VFOUND_NOPOS;
	if (expect->npos != stored->npos ||
		memcmp(expect->pos, stored->pos, sizeof(uint16) * expect->npos) != 0)
		return LION_VFOUND_OTHERPOS;
	return LION_VFOUND_PRESENT;
}

/*
 * Is (ckey, lo) present in the posting set of key, or of the reserved entry
 * named by reservedflag when there is one (DESIGN.md §14 and §17)?  With
 * expect, the member the tuple's value gives the key in a column that stores
 * positions (lion_posmember_from_key(), as the build and the insert make it),
 * the positions the key stores for the TID are compared with it too: the
 * index has to hold the row's own positions under each of its keys, as the
 * position filter decides a phrase from them alone (§17).
 *
 * A reader's lookup (DESIGN.md §5, SCAN): the entry is read under its leaf's
 * SHARE lock, and a CHAIN entry's leaf is let go before the posting tree is
 * descended.  The descent hands back, locked, the leaf that holds ckey if any
 * leaf does - moving right past a split, which is the only way an item moves
 * - so the answer is the one a held leaf would give.  Holding it is what
 * this used to do, and on a hot standby that deadlocked with replay, which
 * locks a record's posting pages BEFORE the entry's leaf (§25): the startup
 * process held the container page and waited for the leaf this held while
 * this waited for the container page, for good.  A set can go away only once
 * it holds nothing any snapshot sees (§18), and VACUUM is locked out on a
 * primary anyway, so a set found gone holds no row this snapshot sees.
 *
 * The positions are looked up the same way: an INLINE entry's chunk under
 * the leaf's lock, with the payload; a CHAIN entry's position tree once the
 * leaf is let go, by a reader's lookup that moves right past a split too
 * (lion_postree_fetch()).  A row this snapshot sees has its positions under
 * every key for as long as the check runs: the insert wrote them before its
 * TID (W1), and only VACUUM takes them out, after the TID (V1).  The lookup
 * keeps one member and one chunk, in buffers made once for the whole scan,
 * so the comparison costs a row nothing that grows with the index.
 */
static LionVerifyFound
lion_verify_tid_present(LionVerifyState *vs, LionState *state, Datum key,
					   uint16 reservedflag, int slicepos, uint32 hash,
					   uint32 ckey, uint16 lo, const LionPosMember *expect)
{
	Relation	index = vs->index;
	Buffer		entrybuf;
	OffsetNumber entryoff;
	BlockNumber head = InvalidBlockNumber;
	BlockNumber posroot = InvalidBlockNumber;
	uint32		ehash = 0;
	bool		present = false;
	LionVerifyFound found = LION_VFOUND_ABSENT;

	if ((slicepos >= 0) ?
		lion_find_slice_entry(index, state, BUFFER_LOCK_SHARE, slicepos,
							  &entrybuf, &entryoff) :
		(reservedflag != 0) ?
		lion_find_reserved_entry(index, state, BUFFER_LOCK_SHARE,
								reservedflag, &entrybuf, &entryoff) :
		lion_find_entry(index, state, BUFFER_LOCK_SHARE, key, hash,
					   &entrybuf, &entryoff))
	{
		Page		page = BufferGetPage(entrybuf);
		ItemId		iid = PageGetItemId(page, entryoff);
		LionEntryTuple *entry = (LionEntryTuple *) PageGetItem(page, iid);

		if ((entry->flags & LION_ENTRY_INLINE) != 0)
		{
			Size		paylen = LION_ENTRY_PAYLOAD_LEN(entry, ItemIdGetLength(iid));
			Size		cur = 0;

			while (lion_inline_fetch(index, BufferGetBlockNumber(entrybuf),
									 entryoff, LionEntryGetPayload(entry),
									 paylen, &cur, vs->cbuf) > 0)
			{
				if (lion_item_covers(vs->cbuf, ckey))
				{
					present = lion_item_contains(vs->cbuf, ckey, lo);
					break;
				}
				if (lion_item_first_ckey(vs->cbuf) > ckey)
					break;
			}

			if (present)
			{
				bool		have = false;

				/* ... and the entry's one chunk, copied out whole and checked */
				if (expect != NULL &&
					lion_entry_inline_poschunk(index, entry, ItemIdGetLength(iid),
											   BufferGetBlockNumber(entrybuf),
											   entryoff, vs->posbuf))
					have = lion_poschunk_find(vs->posbuf, expect->code,
											  vs->posm);
				found = lion_verify_found_positions(expect, have, vs->posm);
			}
		}
		else
		{
			const LionEntryPosExt *x = lion_entry_posext(entry);

			head = lion_entry_head(entry);
			ehash = lion_entry_owner(entry);
			if (x != NULL)
				posroot = x->pos_root;
		}

		UnlockReleaseBuffer(entrybuf);
	}
	else if (BufferIsValid(entrybuf))
		UnlockReleaseBuffer(entrybuf);

	if (BlockNumberIsValid(head))
	{
		Buffer		cbuf = lion_posting_search(index, NULL, ehash, head, ckey,
											   BUFFER_LOCK_SHARE, false);

		if (BufferIsValid(cbuf))
		{
			Page		cpage = BufferGetPage(cbuf);
			BlockNumber blk = BufferGetBlockNumber(cbuf);
			OffsetNumber off;
			bool		hit;

			/*
			 * The descent checks each page with the tree code's test, which
			 * serves position trees too and so does not look at the
			 * LION_PAGE_POSITIONS bit (lion_page_owns_tree()).  The leaf it
			 * lands on is held to the test the readers of the set apply
			 * (lion_page_owns_entry()), so that a TID counts as present only
			 * where a count or a scan would find it.
			 */
			if (lion_page_owns_entry(cpage, ehash, head))
			{
				off = lion_page_find_item(index, cpage, blk, ckey, &hit);
				if (hit)
					present = lion_item_contains(lion_page_item_fetch(index, cpage,
																	  blk, off),
												ckey, lo);
			}
			UnlockReleaseBuffer(cbuf);
		}

		if (present)
		{
			bool		have = false;

			if (expect != NULL && BlockNumberIsValid(posroot))
				have = lion_postree_fetch(index, ehash, posroot, expect->code,
										  vs->posm);
			found = lion_verify_found_positions(expect, have, vs->posm);
		}
	}

	return found;
}

/*
 * Is the row at code in the SLICE of column state at pos (DESIGN.md §50), as
 * the index is now?  A slice is found as a reserved entry is, and its set
 * searched as any entry's (lion_verify_tid_present()).
 */
bool
lion_verify_in_slice(LionVerifyState *vs, LionState *state, int pos,
					 uint64 code)
{
	return lion_verify_tid_present(vs, state, (Datum) 0, 0, pos, 0,
								   lion_code_ckey(code), lion_code_lo(code),
								   NULL) == LION_VFOUND_PRESENT;
}

/* ... and under the VALUE entry of key, a value of column state? */
bool
lion_verify_under_key(LionVerifyState *vs, LionState *state, Datum key,
					  uint64 code)
{
	return lion_verify_tid_present(vs, state, key, 0, -1,
								   lion_hash_key(state, key),
								   lion_code_ckey(code), lion_code_lo(code),
								   NULL) == LION_VFOUND_PRESENT;
}

/*
 * A member's positions as a tsvector prints them - "3,7A" - for a message,
 * or "none" for a member of a stripped tsvector's key.
 */
static char *
lion_verify_format_positions(const LionPosMember *m)
{
	StringInfoData buf;
	int			i;

	if (m->npos == 0)
		return pstrdup("none");
	initStringInfo(&buf);
	for (i = 0; i < m->npos; i++)
	{
		uint16		w = LION_POS_WEIGHT(m->pos[i]);

		if (i > 0)
			appendStringInfoChar(&buf, ',');
		appendStringInfo(&buf, "%u", (unsigned) LION_POS_POS(m->pos[i]));
		if (w != 0)
			appendStringInfoChar(&buf, "DCBA"[w]);
	}
	return buf.data;
}

/*
 * table_index_build_scan() callback: every heap tuple visible to our snapshot
 * has to be indexed under its key.
 */
static void
lion_verify_one_key(LionVerifyState *vs, LionState *state, ItemPointer tid,
				   Datum key, uint16 reservedflag, uint64 code,
				   const LionPosMember *expect)
{
	uint32		hash = (reservedflag != 0) ? LION_NULLKEY_HASH :
		lion_hash_key(state, key);
	bool		typisvarlena;
	LionVerifyFound found;

	found = lion_verify_tid_present(vs, state, key, reservedflag, -1, hash,
									lion_code_ckey(code), lion_code_lo(code),
									expect);
	if (found == LION_VFOUND_PRESENT)
		return;
	if (found != LION_VFOUND_ABSENT)
	{
		const char *what = (found == LION_VFOUND_NOPOS) ? "no positions" :
			"positions that are not its own";
		const char *keystr;

		/*
		 * The positions are the row's text as much as the key is - a
		 * lexeme's places in the document - so they go into the message on
		 * the key value's terms (below).
		 */
		if (!vs->showvalues)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("heap tuple (%u,%u) from table \"%s\" has %s under one of its keys in \"%s\"",
							ItemPointerGetBlockNumber(tid),
							ItemPointerGetOffsetNumber(tid),
							RelationGetRelationName(vs->heap), what,
							RelationGetRelationName(vs->index)),
					 errdetail("Key column %u of the tuple is not NULL.",
							   state->attno)));

		getTypeOutputInfo(state->typid, &vs->keyoutfunc, &typisvarlena);
		keystr = lion_is_length_key(state, key) ? "the length key" :
			psprintf("key \"%s\"", OidOutputFunctionCall(vs->keyoutfunc, key));
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("heap tuple (%u,%u) from table \"%s\" has %s under one of its keys in \"%s\"",
						ItemPointerGetBlockNumber(tid),
						ItemPointerGetOffsetNumber(tid),
						RelationGetRelationName(vs->heap), what,
						RelationGetRelationName(vs->index)),
				 errdetail("Key column %u of the tuple has %s at positions %s, and the index stores %s for the tuple under it.",
						   state->attno, keystr,
						   lion_verify_format_positions(expect),
						   (found == LION_VFOUND_NOPOS) ? "none" :
						   lion_verify_format_positions(vs->posm))));
	}

	/*
	 * The key's value goes into the message only for a superuser.  The
	 * function may be granted to roles without SELECT on the table, and it
	 * reads past column privileges and row-level security; the message would
	 * hand them the value, and put it in the server log.  The TID is enough
	 * to find the row.
	 *
	 * "A superuser" means the CALLER, which lion_index_verify() asked before
	 * it became the table owner: superuser() here would ask about the owner,
	 * and a table a superuser owns would then show its values to whoever the
	 * function was granted to.
	 */
	if (reservedflag == 0 && !vs->showvalues)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("heap tuple (%u,%u) from table \"%s\" is not indexed in \"%s\"",
						ItemPointerGetBlockNumber(tid),
						ItemPointerGetOffsetNumber(tid),
						RelationGetRelationName(vs->heap),
						RelationGetRelationName(vs->index)),
				 errdetail("Key column %u of the tuple is not NULL.", state->attno)));

	getTypeOutputInfo(state->typid, &vs->keyoutfunc, &typisvarlena);

	ereport(ERROR,
			(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg("heap tuple (%u,%u) from table \"%s\" is not indexed in \"%s\"",
					ItemPointerGetBlockNumber(tid),
					ItemPointerGetOffsetNumber(tid),
					RelationGetRelationName(vs->heap),
					RelationGetRelationName(vs->index)),
			 errdetail("Key column %u of the tuple is %s.", state->attno,
					   (reservedflag == LION_ENTRY_NULLKEY) ? "NULL" :
					   (reservedflag == LION_ENTRY_EMPTYKEY) ? "absent" :
					   (reservedflag == LION_ENTRY_EMPTYDIM) ? "absent, in an array with an empty first dimension" :
					   (reservedflag == LION_ENTRY_EMPTYINNER) ? "absent, in an array with an empty inner dimension" :
					   (reservedflag == LION_ENTRY_NULLELEMS) ? "only NULL elements" :
					   OidOutputFunctionCall(vs->keyoutfunc, key))));
}

/*
 * table_index_build_scan() callback: every heap tuple visible to our snapshot
 * has to be indexed under its key, in EVERY key column (DESIGN.md §24).
 */
static void
lion_verify_heap_callback(Relation index, ItemPointer tid, Datum *values,
						 bool *isnull, bool tupleIsAlive, void *arg)
{
	LionVerifyState *vs = (LionVerifyState *) arg;
	MemoryContext oldcxt;
	uint64		code;
	int			c;

	oldcxt = MemoryContextSwitchTo(vs->heapcxt);

	lion_check_key_offset(tid);
	code = lion_tid_to_code(tid);

	for (c = 0; c < vs->ix->ncolumns; c++)
	{
		LionState  *state = &vs->ix->cols[c];
		Datum		key;

		if (isnull[c])
		{
			/* NULL values live in the column's reserved entry (§14). */
			lion_verify_one_key(vs, state, tid, (Datum) 0,
							   LION_ENTRY_NULLKEY, code, NULL);
		}
		else if (state->multikey)
		{
			/*
			 * DESIGN.md §17: the row has to be present under EVERY key its
			 * value extracts to, and in the reserved entry the extraction
			 * names when it extracts to none - the EMPTY one, EMPTYDIM or
			 * EMPTYINNER for an empty array with dimensions, or the
			 * NULLELEMS one for keys that are all NULL.  Extracting here
			 * rather than trusting the index is the whole point of the check:
			 * it is the same call the build and the insert make, so a row
			 * that is missing under one of several keys is found.
			 */
			Datum	   *keys;
			LionKeyPositions *kpos = NULL;
			LionPosMember *pm = NULL;
			uint16		nokey;
			int			nkeys;
			int			i;

			/*
			 * ... and in a column that stores positions, with the positions
			 * the value gives each key, which the same call yields: the
			 * insert's and the build's (lion_insert.c, lion_build.c).  A
			 * reserved entry stores none (§17).
			 */
			if (state->positions)
			{
				nkeys = lion_extract_value_pos(state, values[c], &keys, &kpos,
											   &nokey);
				pm = (LionPosMember *) palloc(sizeof(LionPosMember));
			}
			else
				nkeys = lion_extract_value(state, values[c], &keys, &nokey);

			if (nkeys == 0)
				lion_verify_one_key(vs, state, tid, (Datum) 0, nokey, code,
								   NULL);
			for (i = 0; i < nkeys; i++)
			{
				if (pm != NULL)
					lion_posmember_from_key(pm, code, &kpos[i]);
				lion_verify_one_key(vs, state, tid, keys[i], 0, code, pm);
			}
		}
		else
		{
			key = values[c];
			if (!state->typbyval && state->typlen == -1)
				key = PointerGetDatum(PG_DETOAST_DATUM(key));

			lion_verify_one_key(vs, state, tid, key, 0, code, NULL);

			/* ... and in exactly the slices its value's bits name (§50) */
			if (state->sliced)
				lion_verify_row_slices(vs, state, tid, key, code);
		}

		CHECK_FOR_INTERRUPTS();
	}

	vs->nheaptuples++;

	MemoryContextSwitchTo(oldcxt);
	MemoryContextReset(vs->heapcxt);
}

/*
 * Scan the heap with a fresh MVCC snapshot and check that every visible tuple
 * is in the index, the way contrib/amcheck does for its heapallindexed check
 * - with its own positions under each of its keys, in a column that stores
 * them (lion_verify_tid_present()).
 */
void
lion_verify_heapallindexed(LionVerifyState *vs)
{
	IndexInfo  *indexinfo = BuildIndexInfo(vs->index);
	TableScanDesc scan;
	Snapshot	snapshot;

	vs->heapcxt = AllocSetContextCreate(CurrentMemoryContext,
										"lion index verify heap tuple",
										ALLOCSET_DEFAULT_SIZES);

	/* What the comparison of positions keeps, for the whole scan. */
	vs->posbuf = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	vs->posm = (LionPosMember *) palloc(sizeof(LionPosMember));

	snapshot = RegisterSnapshot(GetTransactionSnapshot());

	/*
	 * A new snapshot is guaranteed to have every entry the index needs, but
	 * an old transaction snapshot may predate the index's indcheckxmin
	 * horizon, in which case it is not safe to use.  That test - and the
	 * validity/readiness tests next to it - are the planner's, shared with
	 * the SQL count functions in lion_index_usable().
	 */
	{
		const char *why;

		if (!lion_index_usable(vs->index, snapshot, &why))
		{
			UnregisterSnapshot(snapshot);
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot verify index \"%s\" against the heap because %s",
							RelationGetRelationName(vs->index), why)));
		}
	}

	/*
	 * The scan is created here rather than by table_index_build_scan() so
	 * that it really uses the snapshot registered above; the scan behaves
	 * like the first heap scan of a CREATE INDEX CONCURRENTLY, which maps
	 * heap-only tuples back to the TID of their HOT chain root - the TID the
	 * index actually holds.  table_index_build_scan() ends the scan for us.
	 */
	indexinfo->ii_Concurrent = true;
	indexinfo->ii_Unique = false;
	indexinfo->ii_ExclusionOps = NULL;
	indexinfo->ii_ExclusionProcs = NULL;
	indexinfo->ii_ExclusionStrats = NULL;

	scan = table_beginscan_strat(vs->heap, snapshot, 0, NULL, true, true);

	table_index_build_scan(vs->heap, vs->index, indexinfo, true, false,
						   lion_verify_heap_callback, (void *) vs, scan);

	UnregisterSnapshot(snapshot);
	MemoryContextDelete(vs->heapcxt);
	vs->heapcxt = NULL;
	pfree(vs->posbuf);
	pfree(vs->posm);
	vs->posbuf = NULL;
	vs->posm = NULL;
}
