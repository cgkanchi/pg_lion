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
 * directory names as its posting set's root.  One walk of the leaves, with
 * nothing but the entries' headers read: an entry the walk has not reached
 * yet may move right in a split, but only onto a page the walk still comes
 * to, so it is seen.
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

			if (!ItemIdIsUsed(iid) || ItemIdGetLength(iid) < LION_ENTRY_HDRSZ)
				continue;
			e = (LionEntryTuple *) PageGetItem(page, iid);
			if ((e->flags & LION_ENTRY_CHAIN) == 0)
				continue;
			hit = (BlockNumber *) bsearch(&e->head, roots, n, sizeof(BlockNumber),
										  lion_verify_blkcmp);
			if (hit != NULL)
				found[hit - roots] = true;
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
 * What a page no walk reached is, read again after the wait: its kind, its
 * level, and the first key a search for it would use.  False when it has no
 * first key to search for.
 */
typedef struct LionVerifyUnreached
{
	bool		isdir;
	uint16		level;
	BlockNumber ohead;			/* a posting page's root */
	uint32		ckey;			/* ... and its first container key */
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
		OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

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
				if (ItemIdIsUsed(PageGetItemId(page, first)))
				{
					u->first = lion_verify_copy_item(page, first);
					ok = true;
				}
			}
		}
		else if (LionPageIsContainer(page) && maxoff >= FirstOffsetNumber)
		{
			OffsetNumber first = lion_posting_first_data(page);

			u->ohead = opaque->owner_head;
			if (opaque->level == 0)
			{
				u->ckey = opaque->minckey;
				ok = true;
			}
			else if (first <= maxoff)
			{
				/*
				 * The line pointer is vouched for before the pivot is read,
				 * as every other item this file reads is: its length alone
				 * says nothing about WHERE it points, and a damaged offset
				 * put the "pivot" past the end of the page.
				 */
				ItemId		iid = lion_verify_itemid(vs, blk, page, first);

				if (ItemIdIsUsed(iid) &&
					ItemIdGetLength(iid) == LION_POSTING_PIVOT_SIZE)
				{
					u->ckey = lion_posting_pivot(page, first)->ckey;
					ok = true;
				}
			}
		}
	}

	UnlockReleaseBuffer(buf);
	return ok;
}

/*
 * Is blk, a page no walk reached, part of its tree now that the writers that
 * were in flight are done?  A search for the page's own first key has to land
 * on it: from the directory's root for a directory page, from its posting
 * set's root for a posting page - a root that is itself reached, or named by
 * an entry (rootfound).  A root of a posting set is part of the index when an
 * entry names it.  A search races with the writers that started since the
 * wait, and the page's first key can move right in a split of the page, so a
 * miss is tried again, with the key read again, a few times.
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
			buf = lion_posting_search_level(vs->index, u.ohead, u.ckey, u.level);
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
	 * and the root an unreached posting page is stamped with.  The second is
	 * not a candidate in its own right when a writer made it: a posting set's
	 * root always comes from the end of the relation (§18), past the blocks
	 * the reachability pass looks at, while the pages the set grows by later
	 * may come out of the free space map, well inside them.  A new key, or an
	 * entry the walk read INLINE and a writer spilled, is exactly that.
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

		switch (lion_verify_classify(vs, blk))
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

/*
 * Is (ckey, lo) present in the posting set of key, or of the reserved entry
 * named by reservedflag when there is one (DESIGN.md §14 and §17)?
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
 */
static bool
lion_verify_tid_present(LionVerifyState *vs, LionState *state, Datum key,
					   uint16 reservedflag, uint32 hash, uint32 ckey,
					   uint16 lo)
{
	Relation	index = vs->index;
	Buffer		entrybuf;
	OffsetNumber entryoff;
	BlockNumber head = InvalidBlockNumber;
	uint32		ehash = 0;
	bool		present = false;

	if ((reservedflag != 0) ?
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

			while (lion_inline_fetch(LionEntryGetPayload(entry), paylen, &cur,
									vs->cbuf) > 0)
			{
				if (lion_item_covers(vs->cbuf, ckey))
				{
					present = lion_item_contains(vs->cbuf, ckey, lo);
					break;
				}
				if (lion_item_first_ckey(vs->cbuf) > ckey)
					break;
			}
		}
		else
		{
			head = entry->head;
			ehash = entry->hash;
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
			bool		found;

			off = lion_page_find_item(index, cpage, blk, ckey, &found);
			if (found)
				present = lion_item_contains(lion_page_item_fetch(index, cpage,
																  blk, off),
											ckey, lo);
			UnlockReleaseBuffer(cbuf);
		}
	}

	return present;
}

/*
 * table_index_build_scan() callback: every heap tuple visible to our snapshot
 * has to be indexed under its key.
 */
static void
lion_verify_one_key(LionVerifyState *vs, LionState *state, ItemPointer tid,
				   Datum key, uint16 reservedflag, uint64 code)
{
	uint32		hash = (reservedflag != 0) ? LION_NULLKEY_HASH :
		lion_hash_key(state, key);
	bool		typisvarlena;

	if (lion_verify_tid_present(vs, state, key, reservedflag, hash,
							   lion_code_ckey(code), lion_code_lo(code)))
		return;

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
							   LION_ENTRY_NULLKEY, code);
		}
		else if (state->multikey)
		{
			/*
			 * DESIGN.md §17: the row has to be present under EVERY key its
			 * value extracts to, and in the reserved EMPTY entry when it
			 * extracts to none.  Extracting here rather than trusting the
			 * index is the whole point of the check: it is the same call the
			 * build and the insert make, so a row that is missing under one
			 * of several keys is found.
			 */
			Datum	   *keys;
			int			nkeys = lion_extract_value(state, values[c], &keys);
			int			i;

			if (nkeys == 0)
				lion_verify_one_key(vs, state, tid, (Datum) 0,
								   LION_ENTRY_EMPTYKEY, code);
			for (i = 0; i < nkeys; i++)
				lion_verify_one_key(vs, state, tid, keys[i], 0, code);
		}
		else
		{
			key = values[c];
			if (!state->typbyval && state->typlen == -1)
				key = PointerGetDatum(PG_DETOAST_DATUM(key));

			lion_verify_one_key(vs, state, tid, key, 0, code);
		}

		CHECK_FOR_INTERRUPTS();
	}

	vs->nheaptuples++;

	MemoryContextSwitchTo(oldcxt);
	MemoryContextReset(vs->heapcxt);
}

/*
 * Scan the heap with a fresh MVCC snapshot and check that every visible tuple
 * is in the index, the way contrib/amcheck does for its heapallindexed check.
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
}
