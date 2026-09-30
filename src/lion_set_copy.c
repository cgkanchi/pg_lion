/*-------------------------------------------------------------------------
 *
 * lion_set_copy.c
 *		Private copies of posting sets: materializing a set, spilling a copy
 *		too big for memory (LionSpill), and seeking in a copy (LionMatSet).
 *
 * Part of the count engine: lion_count.h is its interface, and
 * lion_count_int.h declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_count_int.h"

/* ---------------------------------------------------------------------
 * Materializing a posting set
 * --------------------------------------------------------------------- */

/*
 * Copy every container of a CHAIN posting set into private memory, so that
 * later counts can read it without touching the index again.  Returns false
 * (and leaves ps->mat NULL) when the set turns out to be too big to be worth
 * copying; the caller then keeps walking the chain page by page.
 *
 * Why this is safe, and why only *some* sets may be materialized
 * -------------------------------------------------------------
 * A materialized set holds no pin, so it gives up the DESIGN.md section 9
 * interlock for itself: between the copy and the visibility-map check a
 * VACUUM may have run, so the copy may still list a TID that has since been
 * removed from the index and pruned from the heap.
 *
 * That does not make a wrong count possible, because the number this file
 * produces is the count of the INTERSECTION, and the intersection is only
 * ever counted from the visibility map while at least one participating set
 * is being read the pinned way - lion_count_posting_sets() enforces that.
 * Take a TID t that the stale copy still contains and that is really dead:
 *
 *	- either t is no longer in the pinned set's container, and it is not in
 *	  the intersection at all, so it is not counted;
 *	- or it is, which means we read that container before VACUUM removed t
 *	  from that index.  VACUUM only removes a TID from a page under a cleanup
 *	  lock on it (section 11), so it cannot have got past the page we are
 *	  pinning; it has therefore not finished ambulkdelete() on that index, and
 *	  it only sets all-visible on a heap page after ambulkdelete() has
 *	  finished on EVERY index of the table.  The visibility map therefore
 *	  cannot say all-visible for t's heap block, t goes to the heap recheck,
 *	  and the snapshot decides - which is the right answer for a dead tuple.
 *
 * The other direction, a copy that is missing a TID, cannot happen: an index
 * entry is written before the inserting transaction commits, so every row
 * visible to our snapshot was already in the index when we took the copy.
 *
 * The copy is walked exactly the way lion_cursor_next() walks a chain - items
 * and rightlink read together under one SHARE lock - so a concurrent page
 * split (which only ever moves items to a new page to the right) cannot make
 * us miss or duplicate a container.
 *
 * maxbytes is what the copy may take at most: what is left of the budget of
 * all the copies one count's sources hold (lion_count_sources_run(); DESIGN.md
 * §15, "Bounded cursors").  A set that does not fit is given up on for good
 * (ps->matfailed) and keeps being walked page by page.
 */
bool
lion_posting_set_materialize(LionPostingSet *ps, Size maxbytes)
{
	MemoryContext oldcxt;
	PGAlignedBlock *imgbuf;
	Page		img;
	BlockNumber blkno;
	char	   *buf;
	Size		cap;
	Size		used = 0;
	Size	   *offs;
	int			noffs = 0;
	int			offcap;
	bool		ok = true;
	bool		sorted = true;
	int			i;

	Assert(ps->found && !ps->is_inline && ps->mat == NULL);

	oldcxt = MemoryContextSwitchTo(ps->cxt != NULL ? ps->cxt : CurrentMemoryContext);

	offcap = Min(LION_MATERIALIZE_MAX_CONTAINERS * 2, 256);
	offs = (Size *) palloc(sizeof(Size) * offcap);
	cap = 8192;
	buf = (char *) palloc(cap);
	imgbuf = (PGAlignedBlock *) palloc(sizeof(PGAlignedBlock));
	img = (Page) imgbuf->data;

	/*
	 * DESIGN.md §22: the walk starts at the leftmost LEAF, which the descent
	 * hands back locked - the entry's head block is the tree's root and is
	 * only a leaf while the set fits one page.
	 */
	{
		Buffer		firstbuf = lion_posting_search(ps->index, NULL, 0, ps->head,
												   0, BUFFER_LOCK_SHARE, false);

		if (!BufferIsValid(firstbuf))
			blkno = InvalidBlockNumber;
		else
		{
			blkno = BufferGetBlockNumber(firstbuf);
			memcpy(img, BufferGetPage(firstbuf), BLCKSZ);
			UnlockReleaseBuffer(firstbuf);
		}
	}

	while (BlockNumberIsValid(blkno))
	{
		BlockNumber imgblk = blkno;
		OffsetNumber off;
		OffsetNumber maxoff;

		blkno = LionPageGetOpaque(img)->rightlink;
		maxoff = PageGetMaxOffsetNumber(img);

		for (off = FirstOffsetNumber; off <= maxoff; off = OffsetNumberNext(off))
		{
			/* checked for what the copy and its readers need (§3) */
			const LionContainer *c = lion_page_item_fetch(ps->index, img,
														  imgblk, off);
			Size		sz = lion_item_size(c);

			/*
			 * Give up as soon as the set fails BOTH budgets: a wide set is
			 * cheaper to re-walk than to keep a copy of.  A sparse segment
			 * counts as the one item it is, which is also how the entry
			 * counts it in ncontainers.
			 */
			if (noffs >= LION_MATERIALIZE_MAX_CONTAINERS &&
				used + sz > LION_MATERIALIZE_MAX_BYTES)
			{
				ok = false;
				break;
			}

			/*
			 * ... and whatever it is, once the count's copies are spent.  The
			 * copy is kept at its exact size (below), so this is what it will
			 * hold.
			 */
			if (sizeof(LionMatSet) + MAXALIGN(used + sz) +
				sizeof(LionContainer *) * (noffs + 1) > maxbytes)
			{
				ok = false;
				break;
			}

			while (used + sz > cap)
			{
				cap *= 2;
				buf = (char *) repalloc(buf, cap);
			}
			if (noffs >= offcap)
			{
				offcap *= 2;
				offs = (Size *) repalloc(offs, sizeof(Size) * offcap);
			}

			memcpy(buf + used, c, sz);
			offs[noffs++] = used;
			used += MAXALIGN(sz);
		}

		if (!ok || !BlockNumberIsValid(blkno))
			break;

		CHECK_FOR_INTERRUPTS();

		{
			Buffer		pagebuf;
			Page		page;

			lion_posting_pages_read++;
			pagebuf = ReadBuffer(ps->index, blkno);
			LockBuffer(pagebuf, BUFFER_LOCK_SHARE);
			page = BufferGetPage(pagebuf);

			/* The ownership check of DESIGN.md §18; see lion_cursor_next_item(). */
			if (!lion_page_owns(page, ps->head) || !LionPageIsPostingLeaf(page))
			{
				UnlockReleaseBuffer(pagebuf);
				break;
			}
			memcpy(img, page, BLCKSZ);
			UnlockReleaseBuffer(pagebuf);
		}
	}

	pfree(imgbuf);

	if (ok)
	{
		LionMatSet  *mat = (LionMatSet *) palloc0(sizeof(LionMatSet));

		/*
		 * The buffer grew by doubling from a page; what is kept is the exact
		 * size, because a GROUP BY may keep hundreds of these for as long as
		 * the relation is counted, and a set of one small segment would
		 * otherwise hold eight kilobytes.
		 */
		if (cap > used)
		{
			char	   *exact = (char *) palloc(Max(used, (Size) 1));

			memcpy(exact, buf, used);
			pfree(buf);
			buf = exact;
		}

		mat->ncontainers = noffs;
		mat->bytes = used;
		mat->held = sizeof(LionMatSet) + MAXALIGN(Max(used, (Size) 1)) +
			sizeof(LionContainer *) * Max(noffs, 1);
		mat->buf = buf;
		mat->containers = (LionContainer **)
			palloc(sizeof(LionContainer *) * Max(noffs, 1));
		for (i = 0; i < noffs; i++)
		{
			mat->containers[i] = (LionContainer *) (buf + offs[i]);
			if (i > 0 &&
				lion_item_first_ckey(mat->containers[i]) <=
				lion_item_last_ckey(mat->containers[i - 1]))
				sorted = false;
		}

		/*
		 * Chains are built and maintained in ascending ckey order, with item
		 * ranges that never overlap (DESIGN.md §13), so this never fires; the
		 * merge would silently under-count if it ever did, which is worth one
		 * comparison per item to rule out.
		 */
		if (!sorted)
			elog(ERROR, "lion index: containers of \"%s\" are out of order",
				 RelationGetRelationName(ps->index));

		ps->mat = mat;
	}
	else
	{
		pfree(buf);
		ps->matfailed = true;
	}

	pfree(offs);
	MemoryContextSwitchTo(oldcxt);

	return ok;
}

/*
 * The sets lion_count_sources_run() gives up on copying: hopeless even as an
 * ARRAY of members - the test it makes before it tries - or tried and too big
 * for what was left of the budget.  Each count walks their pages again.
 */
bool
lion_posting_set_rewalked(const LionPostingSet *ps)
{
	if (!ps->found || ps->is_inline || ps->mat != NULL)
		return false;
	return ps->matfailed ||
		(ps->ncontainers > LION_MATERIALIZE_MAX_CONTAINERS &&
		 ps->ntids > LION_MATERIALIZE_MAX_BYTES / sizeof(uint16));
}


/* ---------------------------------------------------------------------
 * Spilled copies (LionSpill)
 * --------------------------------------------------------------------- */

/*
 * Start a spill whose entries - and file - live in cxt.  The file honours
 * temp_tablespaces, which the caller has had PrepareTempTablespaces() look up
 * BEFORE it started to read the index: the lookup reads catalogs, which may
 * process invalidations, and that must not happen here, with a walk of the
 * index under way.
 */
void
lion_spill_begin(LionSpill *sp, MemoryContext cxt)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(cxt);

	sp->cxt = cxt;
	sp->file = (sp->fileset != NULL) ?
		BufFileCreateFileSet(sp->fileset, sp->name) :
		BufFileCreateTemp(false);
	sp->cap = 256;
	sp->ents = (LionSpillEnt *) palloc(sizeof(LionSpillEnt) * sp->cap);
	sp->nents = 0;
	MemoryContextSwitchTo(oldcxt);
}

/* Append one container: the next in ascending key order, one per key. */
void
lion_spill_add(LionSpill *sp, const LionContainer *c)
{
	Size		sz = lion_item_size(c);
	LionSpillEnt *e;
	int			fileno;
	pgoff_t		off;

	Assert(sz <= LION_CONTAINER_MAX_SIZE);
	Assert(sp->nents == 0 || sp->ents[sp->nents - 1].ckey < c->ckey);

	if (sp->nents >= sp->cap)
	{
		sp->cap *= 2;
		sp->ents = (LionSpillEnt *)
			repalloc_huge(sp->ents, sizeof(LionSpillEnt) * sp->cap);
	}
	BufFileTell(sp->file, &fileno, &off);
	if (fileno > PG_INT16_MAX)
		elog(ERROR, "lion index: a spilled posting set of more than %d file segments",
			 PG_INT16_MAX);

	e = &sp->ents[sp->nents++];
	e->off = off;
	e->ckey = c->ckey;
	e->size = (uint16) sz;
	e->fileno = (int16) fileno;
	BufFileWrite(sp->file, c, sz);
}

/*
 * The set a finished spill makes.  What it holds in memory is its entries and
 * the file's one-block buffer; the entries are cut to their number.
 */
LionMatSet *
lion_spill_finish(LionSpill *sp)
{
	LionMatSet *mat;
	Size		bytes = 0;
	int			i;

	mat = (LionMatSet *) MemoryContextAllocZero(sp->cxt, sizeof(LionMatSet));
	if (sp->nents < sp->cap)
		sp->ents = (LionSpillEnt *)
			repalloc_huge(sp->ents, sizeof(LionSpillEnt) * Max(sp->nents, 1));
	for (i = 0; i < sp->nents; i++)
		bytes += sp->ents[i].size;

	mat->ncontainers = sp->nents;
	mat->bytes = bytes;
	mat->held = sizeof(LionMatSet) + sizeof(LionSpillEnt) * Max(sp->nents, 1) +
		BLCKSZ;
	mat->file = sp->file;
	mat->spill = sp->ents;
	sp->file = NULL;
	sp->ents = NULL;
	return mat;
}

/* Give up on a spill: close its file. */
void
lion_spill_abandon(LionSpill *sp)
{
	if (sp->file != NULL)
		BufFileClose(sp->file);
	sp->file = NULL;
	if (sp->ents != NULL)
		pfree(sp->ents);
	sp->ents = NULL;
}

/* Read container i of a spilled set into buf, LION_CONTAINER_MAX_SIZE long. */
void
lion_spill_read(const LionMatSet *mat, int i, LionContainer *buf)
{
	const LionSpillEnt *e = &mat->spill[i];
	BufFile    *file = mat->file;

	/* a view of a shared copy: the file of the chunk container i is of */
	if (file == NULL)
	{
		int			lo = 0;
		int			hi = mat->nchunks - 1;

		while (lo < hi)
		{
			int			mid = lo + (hi - lo + 1) / 2;

			if (mat->chunkfirst[mid] <= i)
				lo = mid;
			else
				hi = mid - 1;
		}
		file = mat->files[lo];
	}
	if (file == NULL || e->fileno < 0 || e->size > LION_CONTAINER_MAX_SIZE)
		elog(ERROR, "lion index: container %d of a spilled copy is in no file", i);

	if (BufFileSeek(file, e->fileno, e->off, SEEK_SET) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek in the temporary file of a spilled lion posting set")));
	BufFileReadExact(file, buf, e->size);
}

/*
 * Keep a finished spill's first container in memory (LionMatSet.first): what
 * every cursor built over the set reads first, whatever it is sought to next.
 */
void
lion_spill_keep_first(LionMatSet *mat, MemoryContext cxt)
{
	Size		size;

	if (mat->ncontainers == 0)
		return;
	size = MAXALIGN(Max((Size) mat->spill[0].size, LION_CONTAINER_HDRSZ));
	mat->first = (LionContainer *) MemoryContextAlloc(cxt, size);
	lion_spill_read(mat, 0, mat->first);
	mat->held += size;
}


/* ---------------------------------------------------------------------
 * Seeking a private copy (LionMatSet)
 * --------------------------------------------------------------------- */




/*
 * Index a copy by its container keys, for the seeks every count makes of it
 * (DESIGN.md §27, "The copy, looked up by key"): the keys in an array of
 * their own - what a binary search reads, four bytes a container, where it
 * used to follow a pointer to each container it compared - and, where they
 * are dense, the direct index, which answers a seek with one read.  A copy
 * is never changed once it is made, so neither goes stale.
 *
 * A key an item covers is its last (lion_item_last_ckey()): the seek looks
 * for the first item whose last key is at or above the target, as it always
 * has.  The items of a copy are in ascending key order and never overlap, so
 * those keys ascend strictly, which is what both rely on; a copy whose keys
 * would not is left as it is.
 *
 * `room` is what the index may take from the copy's memory; what it takes is
 * added to mat->held.  A spilled copy keeps its keys in its entries already,
 * and gets the direct index alone - sixteen bytes a container at most, the
 * size of those entries.
 */
void
lion_mat_index(LionMatSet *mat, MemoryContext cxt, Size room)
{
	uint32	   *keys;
	uint32		first;
	uint32		last;
	Size		dirbytes;
	int			n = mat->ncontainers;
	int			i;

	if (n < 2)
		return;

	keys = (uint32 *) MemoryContextAlloc(cxt, sizeof(uint32) * n);
	for (i = 0; i < n; i++)
	{
		keys[i] = lion_mat_key(mat, i);
		if (i > 0 && keys[i] <= keys[i - 1])
		{
			pfree(keys);
			return;
		}
	}

	first = keys[0];
	last = keys[n - 1];
	dirbytes = sizeof(uint32) * ((Size) (last - first) + 1);

	if (mat->file == NULL)
	{
		if (sizeof(uint32) * n > room)
		{
			pfree(keys);
			return;
		}
		mat->keys = keys;
		mat->held += sizeof(uint32) * n;
		room -= sizeof(uint32) * n;
	}
	else
		room = dirbytes;		/* bounded by the spread alone */

	if ((uint64) last - first + 1 <= (uint64) n * LION_MAT_DIR_SPREAD &&
		dirbytes <= room)
	{
		uint32	   *dir = (uint32 *) MemoryContextAlloc(cxt, dirbytes);
		uint64		k = 0;

		/* slot k: the first container whose key is at or above first + k */
		for (i = 0; i < n; i++)
		{
			while ((uint64) first + k <= keys[i])
				dir[k++] = (uint32) i;
		}
		Assert(k == (uint64) last - first + 1);
		mat->dir = dir;
		mat->dirbase = first;
		mat->dirlen = (uint32) k;
		mat->held += dirbytes;
	}

	if (mat->keys == NULL)
		pfree(keys);
}
