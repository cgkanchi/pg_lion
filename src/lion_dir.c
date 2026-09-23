/*-------------------------------------------------------------------------
 *
 * lion_dir.c
 *		The sorted entry directory of the lion index (DESIGN.md §21).
 *
 * Entries live on the leaves of a B-tree keyed by the index key, in the
 * order described on LION_KIND_* in lion.h.  The tree protocol is Lehman and
 * Yao's, as PostgreSQL's own nbtree implements it
 * (src/backend/access/nbtree/README):
 *
 *	- Every non-rightmost page's FIRST item is its high key, a key-only pivot
 *	  tuple that is strictly greater than every key on the page and at most
 *	  every key to its right.  Internal pages hold downlink pivots whose
 *	  `head` is the child block; the leftmost downlink of the leftmost page of
 *	  each level is flagged MINUSINF and compares below everything.
 *
 *	- A descent NEVER holds a parent lock while acquiring a child lock.  This
 *	  is not an optimisation but the reason the protocol has no deadlock: a
 *	  split runs the other way round, holding the child while it inserts the
 *	  downlink into the parent, and coupling downwards would close the cycle.
 *	  The right links are what make the un-coupled descent safe: a searcher
 *	  that lands on a page whose high key is no longer above its key simply
 *	  moves right, which is where a concurrent split put the items.
 *	  (Deviation from the §21 sketch, which said "lock coupling"; noted there.)
 *
 *	- A split writes ONE generic record for (left, right, old right sibling),
 *	  leaving the left page flagged LION_PAGE_INCOMPLETE_SPLIT, a second one
 *	  for the downlink in the parent, and a third, tiny one that clears the
 *	  flag.  A record takes at most four buffers, which is why the flag
 *	  cannot be cleared in the same record as the parent insertion the way
 *	  nbtree does it: a parent split already needs four.  A crash anywhere in
 *	  between leaves the flag set, and the next writer that lands on the page
 *	  finishes the split; the repair checks whether the downlink is already
 *	  there, so it is idempotent.  The left page is held EXCLUSIVE from the
 *	  first record to the last, so no concurrent writer ever observes the flag
 *	  of a split that is still in progress - which nbtree's README explains is
 *	  the one thing that must not happen (two writers would each insert the
 *	  downlink).
 *
 *	- A ROOT split is atomic instead (left, right, new root, meta page = four
 *	  buffers), so the meta page never names a root that does not exist and a
 *	  root is never flagged.
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

/* DESIGN.md §21: EXPLAIN ANALYZE's "Directory Pages Read". */
int64		lion_dir_pages_read = 0;

static Buffer lion_dir_find_parent(Relation index, Relation heaprel,
								  LionIndexState *ix, const LionSearchKey *sk,
								  BlockNumber childblk, uint16 childlevel,
								  OffsetNumber *offp);
static void lion_dir_split(Relation index, Relation heaprel, LionIndexState *ix,
						  Buffer buf, OffsetNumber off, bool replace,
						  LionEntryTuple *newitem, Size newsize);
static void lion_dir_place_again(Relation index, Relation heaprel,
								 LionIndexState *ix, Buffer buf, bool replace,
								 LionEntryTuple *item, Size size);
static void lion_dir_finish_split(Relation index, Relation heaprel,
								 LionIndexState *ix, Buffer pbuf);

/* ---------------------------------------------------------------------
 * Small page helpers
 * --------------------------------------------------------------------- */

static inline Buffer
lion_dir_readbuf(Relation index, BlockNumber blk)
{
	lion_dir_pages_read++;
	return ReadBuffer(index, blk);
}

static inline LionEntryTuple *
lion_page_item(Page page, OffsetNumber off)
{
	return (LionEntryTuple *) PageGetItem(page, PageGetItemId(page, off));
}

static inline Size
lion_page_itemsz(Page page, OffsetNumber off)
{
	return (Size) ItemIdGetLength(PageGetItemId(page, off));
}

/* The high key of a non-rightmost directory page. */
LionEntryTuple *
lion_dir_highkey(Page page)
{
	Assert(!LionPageIsRightmost(page));
	return lion_page_item(page, FirstOffsetNumber);
}

#define lion_page_highkey(page)	lion_dir_highkey(page)

static void
lion_dir_check_page(Relation index, Page page, BlockNumber blk)
{
	if (PageIsNew(page) || PageGetSpecialSize(page) != LION_SPECIAL_SIZE ||
		LionPageGetOpaque(page)->page_id != LION_PAGE_ID ||
		(LionPageGetOpaque(page)->flags & (LION_PAGE_BUCKET | LION_PAGE_DIR)) == 0)
		elog(ERROR, "lion index \"%s\": block %u is not a directory page",
			 RelationGetRelationName(index), blk);
}

/*
 * Move to the right sibling, releasing the page we came from first.  Moving
 * right needs no coupling: items only ever move rightwards, so a page that
 * splits under us puts what we are looking for on a page we have not passed.
 */
Buffer
lion_dir_step_right(Relation index, Buffer buf, int lockmode)
{
	BlockNumber next = LionPageGetOpaque(BufferGetPage(buf))->rightlink;
	Buffer		nbuf;

	Assert(BlockNumberIsValid(next));
	UnlockReleaseBuffer(buf);
	nbuf = lion_dir_readbuf(index, next);
	LockBuffer(nbuf, lockmode);
	lion_dir_check_page(index, BufferGetPage(nbuf), next);

	CHECK_FOR_INTERRUPTS();
	return nbuf;
}

/* ---------------------------------------------------------------------
 * The order (DESIGN.md §21)
 * --------------------------------------------------------------------- */

int
lion_cmp_prefix(const LionEntryTuple *item, const LionSearchKey *sk)
{
	int			ikind;

	/*
	 * The KEY COLUMN leads (DESIGN.md §24).  An item of another column is
	 * settled here, before any opclass function is called - which is what
	 * makes it safe for one descent to cross the entries of columns whose key
	 * types it knows nothing about.  A minus-infinity downlink carries column
	 * 0 and therefore sorts below every entry of every column.
	 */
	if (item->attno != sk->attno)
		return item->attno < sk->attno ? -1 : 1;

	ikind = lion_entry_kind(item);
	if (ikind != sk->kind)
		return ikind < sk->kind ? -1 : 1;
	if (ikind != LION_KIND_VALUE)
		return 0;					/* one NULL and one EMPTY entry per column */

	if (sk->cmpproc != NULL)
	{
		Datum		stored = lion_fetch_key(sk->col, LionEntryGetKey(item));
		int32		c = DatumGetInt32(FunctionCall2Coll(sk->cmpproc,
														sk->collation,
														stored, sk->key));

		if (c != 0)
			return c < 0 ? -1 : 1;
	}

	if (item->hash != sk->hash)
		return item->hash < sk->hash ? -1 : 1;

	return 0;
}

int
lion_cmp_entry(const LionEntryTuple *item, const LionSearchKey *sk)
{
	int			c = lion_cmp_prefix(item, sk);
	Size		n;

	if (c != 0)
		return c;

	/*
	 * The prefixes tie.  Without the search value's stored form the key is
	 * taken to be the smallest member of its own run, which is what puts a
	 * lookup at the start of the run it is about to scan; with it, the
	 * bytewise tail makes the order total.
	 */
	if (sk->raw == NULL)
		return 1;

	n = Min((Size) item->keylen, sk->rawlen);
	if (n > 0)
	{
		c = memcmp(LionEntryGetKey(item), sk->raw, n);
		if (c != 0)
			return c < 0 ? -1 : 1;
	}
	if ((Size) item->keylen != sk->rawlen)
		return (Size) item->keylen < sk->rawlen ? -1 : 1;
	return 0;
}

int
lion_cmp_entries(LionIndexState *ix, const LionEntryTuple *a,
				 const LionEntryTuple *b)
{
	LionSearchKey sk;

	lion_search_key_exact(ix, &sk, b);
	return lion_cmp_entry(a, &sk);
}

void
lion_search_key_init(LionState *state, LionSearchKey *sk, int kind, Datum key,
					 uint32 hash)
{
	sk->attno = state->attno;
	sk->col = state;
	sk->kind = kind;
	sk->key = key;
	sk->hash = hash;
	sk->cmpproc = state->ordered ? &state->cmpproc : NULL;
	sk->eqproc = &state->eqproc;
	sk->collation = state->collation;
	sk->raw = NULL;
	sk->rawlen = 0;
}

void
lion_search_key_exact(LionIndexState *ix, LionSearchKey *sk,
					  const LionEntryTuple *entry)
{
	int			kind = lion_entry_kind(entry);

	/*
	 * A minus-infinity downlink has no column and no key at all; it compares
	 * below everything on the attno term alone, so the column state it is
	 * given is never consulted (DESIGN.md §24).
	 */
	if (entry->attno == 0 || kind == LION_KIND_MINF)
	{
		lion_search_key_init(lion_column(ix, 1), sk, LION_KIND_MINF, (Datum) 0,
							 entry->hash);
		sk->attno = 0;
		sk->cmpproc = NULL;
	}
	else
	{
		LionState  *col = lion_column(ix, (AttrNumber) entry->attno);

		lion_search_key_init(col, sk, kind,
							 kind == LION_KIND_VALUE ?
							 lion_fetch_key(col, LionEntryGetKey(entry)) :
							 (Datum) 0,
							 entry->hash);
	}

	sk->raw = LionEntryGetKey(entry);
	sk->rawlen = entry->keylen;
}

/*
 * The first offset in [first data item, maxoff + 1] whose item does not sort
 * before sk (strict: does not sort before or equal to it).
 */
static OffsetNumber
lion_page_binsrch_ext(Page page, const LionSearchKey *sk, bool strict)
{
	OffsetNumber lo = lion_page_first_data(page);
	OffsetNumber hi = OffsetNumberNext(PageGetMaxOffsetNumber(page));

	while (lo < hi)
	{
		OffsetNumber mid = lo + (hi - lo) / 2;
		int			c = lion_cmp_entry(lion_page_item(page, mid), sk);

		if (c > 0 || (!strict && c == 0))
			hi = mid;
		else
			lo = OffsetNumberNext(mid);
	}

	return lo;
}

/* The first item that does not sort before sk, for a leaf search. */
OffsetNumber
lion_dir_binsrch(Page page, const LionSearchKey *sk)
{
	return lion_page_binsrch_ext(page, sk, false);
}

/* The downlink of the child sk belongs to, on an internal page. */
static OffsetNumber
lion_page_downlink(Page page, const LionSearchKey *sk)
{
	OffsetNumber first = lion_page_first_data(page);
	OffsetNumber off = lion_page_binsrch_ext(page, sk, true);

	/*
	 * Normally the leftmost downlink of a page compares below every key - the
	 * leftmost page of a level carries a MINUSINF one, and every other page's
	 * first separator is its own lower bound, so a descent that reached it has
	 * a key at or above that separator.  Taking the leftmost child when that
	 * somehow does not hold is conservative rather than wrong: the search then
	 * lands left of its key and the move-right rule walks to it.
	 */
	if (off <= first)
		return first;
	return OffsetNumberPrev(off);
}

/* ---------------------------------------------------------------------
 * The root
 * --------------------------------------------------------------------- */

BlockNumber
lion_dir_root(Relation index, LionIndexState *ix, uint32 *height)
{
	Buffer		buf;
	Page		page;
	LionMetaPageData *meta;
	BlockNumber root;
	uint32		h;

	buf = ReadBuffer(index, LION_METAPAGE_BLKNO);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	if (PageIsNew(page) || !LionPageIsMeta(page))
	{
		UnlockReleaseBuffer(buf);
		elog(ERROR, "lion index \"%s\": block 0 is not the meta page",
			 RelationGetRelationName(index));
	}
	meta = LionPageGetMeta(page);
	root = meta->root;
	h = meta->height;
	UnlockReleaseBuffer(buf);

	if (!BlockNumberIsValid(root))
		elog(ERROR, "lion index \"%s\": the meta page has no root",
			 RelationGetRelationName(index));

	ix->meta.root = root;
	ix->meta.height = h;
	if (height != NULL)
		*height = h;

	return root;
}

/*
 * The current root page, locked SHARE.  The cached root block is trusted and
 * validated by the LION_PAGE_ROOT flag, which a root split clears on the page
 * it demotes - exactly nbtree's BTP_ROOT trick, so the common case costs no
 * meta-page visit at all.
 */
static Buffer
lion_dir_get_root(Relation index, LionIndexState *ix)
{
	bool		refreshed = false;
	BlockNumber blk;

	blk = BlockNumberIsValid(ix->meta.root) ? ix->meta.root :
		lion_dir_root(index, ix, NULL);

	for (;;)
	{
		Buffer		buf = lion_dir_readbuf(index, blk);
		Page		page;

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		lion_dir_check_page(index, page, blk);

		if (LionPageIsRoot(page))
			return buf;

		UnlockReleaseBuffer(buf);
		if (refreshed)
			elog(ERROR, "lion index \"%s\": block %u is not the root page",
				 RelationGetRelationName(index), blk);
		blk = lion_dir_root(index, ix, NULL);
		refreshed = true;
	}
}

BlockNumber
lion_dir_leftmost_leaf(Relation index, LionIndexState *ix)
{
	Buffer		buf = lion_dir_get_root(index, ix);

	for (;;)
	{
		Page		page = BufferGetPage(buf);
		BlockNumber blk = BufferGetBlockNumber(buf);
		BlockNumber child;

		if (LionPageIsLeaf(page))
		{
			UnlockReleaseBuffer(buf);
			return blk;
		}

		if (PageGetMaxOffsetNumber(page) < lion_page_first_data(page))
		{
			UnlockReleaseBuffer(buf);
			elog(ERROR, "lion index \"%s\": internal page %u has no downlink",
				 RelationGetRelationName(index), blk);
		}

		child = lion_page_item(page, lion_page_first_data(page))->head;
		UnlockReleaseBuffer(buf);

		buf = lion_dir_readbuf(index, child);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		lion_dir_check_page(index, BufferGetPage(buf), child);
		CHECK_FOR_INTERRUPTS();
	}
}

/* ---------------------------------------------------------------------
 * Descent
 * --------------------------------------------------------------------- */

Buffer
lion_dir_search(Relation index, Relation heaprel, LionIndexState *ix,
				const LionSearchKey *sk, int lockmode, bool forwrite,
				OffsetNumber *offp)
{
	Buffer		buf;

	Assert(!forwrite || lockmode == BUFFER_LOCK_EXCLUSIVE);

restart:
	buf = lion_dir_get_root(index, ix);

	if (LionPageIsLeaf(BufferGetPage(buf)) && lockmode != BUFFER_LOCK_SHARE)
	{
		/*
		 * A one-page tree: the root is the leaf and has to be relocked in the
		 * caller's mode.  Anything may have happened in between, so start over
		 * if it is no longer both.
		 */
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		LockBuffer(buf, lockmode);
		if (!LionPageIsRoot(BufferGetPage(buf)) ||
			!LionPageIsLeaf(BufferGetPage(buf)))
		{
			UnlockReleaseBuffer(buf);
			goto restart;
		}
	}

	for (;;)
	{
		Page		page = BufferGetPage(buf);
		int			pagelock = LionPageIsLeaf(page) ? lockmode :
			BUFFER_LOCK_SHARE;
		OffsetNumber off;
		BlockNumber child;
		int			childlock;

		/*
		 * A concurrent split may have moved our key to the right - and a
		 * writer finishes EVERY unfinished split it meets on the way there,
		 * not only one it ends up standing on.  That is not tidiness: the page
		 * to the right of a flagged one is the page with no downlink, and a
		 * writer that stepped over the flag and then split THAT page would
		 * have no parent item to insert next to.  nbtree's _bt_moveright does
		 * exactly this, lock upgrade and all (DESIGN.md §21).
		 */
		for (;;)
		{
			page = BufferGetPage(buf);

			if (LionPageIsRightmost(page))
				break;

			if (forwrite && LionPageIncompleteSplit(page))
			{
				if (pagelock != BUFFER_LOCK_EXCLUSIVE)
				{
					LockBuffer(buf, BUFFER_LOCK_UNLOCK);
					LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
				}
				if (LionPageIncompleteSplit(BufferGetPage(buf)))
					lion_dir_finish_split(index, heaprel, ix, buf);
				if (pagelock != BUFFER_LOCK_EXCLUSIVE)
				{
					LockBuffer(buf, BUFFER_LOCK_UNLOCK);
					LockBuffer(buf, pagelock);
				}
				continue;
			}

			if (lion_cmp_entry(lion_page_highkey(page), sk) > 0)
				break;

			buf = lion_dir_step_right(index, buf, pagelock);
		}
		page = BufferGetPage(buf);

		if (LionPageIsLeaf(page))
		{
			*offp = lion_page_binsrch_ext(page, sk, false);
			return buf;
		}

		off = lion_page_downlink(page, sk);
		child = lion_page_item(page, off)->head;
		childlock = (LionPageGetOpaque(page)->level == 1) ? lockmode :
			BUFFER_LOCK_SHARE;

		/* Release the parent BEFORE locking the child; see the file header. */
		UnlockReleaseBuffer(buf);
		buf = lion_dir_readbuf(index, child);
		LockBuffer(buf, childlock);
		lion_dir_check_page(index, BufferGetPage(buf), child);

		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * The cross-type fallback of DESIGN.md §21: an opfamily that offers
 * cross-type equality but no cross-type ordering cannot be descended for such
 * a value, so every leaf is walked instead.  Correct, and linear in the
 * directory; none of pg_lion's own opclasses take this path.
 */
static bool
lion_dir_find_by_scan(Relation index, LionIndexState *ix,
					 const LionSearchKey *sk, int lockmode, Buffer *bufp,
					 OffsetNumber *offnum)
{
	BlockNumber blk = lion_dir_leftmost_leaf(index, ix);

	while (BlockNumberIsValid(blk))
	{
		Buffer		buf = lion_dir_readbuf(index, blk);
		Page		page;
		OffsetNumber off;
		OffsetNumber maxoff;

		LockBuffer(buf, lockmode);
		page = BufferGetPage(buf);
		lion_dir_check_page(index, page, blk);
		maxoff = PageGetMaxOffsetNumber(page);

		for (off = lion_page_first_data(page); off <= maxoff; off++)
		{
			ItemId		iid = PageGetItemId(page, off);
			LionEntryTuple *e;

			if (!ItemIdIsUsed(iid))
				continue;
			e = (LionEntryTuple *) PageGetItem(page, iid);
			if (e->attno != sk->attno)
				continue;		/* another key column (DESIGN.md §24) */
			if (lion_entry_kind(e) != sk->kind)
				continue;
			if (sk->kind == LION_KIND_VALUE &&
				!DatumGetBool(FunctionCall2Coll(sk->eqproc, sk->collation,
												lion_fetch_key(sk->col,
															   LionEntryGetKey(e)),
												sk->key)))
				continue;

			*bufp = buf;
			*offnum = off;
			return true;
		}

		blk = LionPageGetOpaque(page)->rightlink;
		UnlockReleaseBuffer(buf);
		CHECK_FOR_INTERRUPTS();
	}

	*bufp = InvalidBuffer;
	*offnum = InvalidOffsetNumber;
	return false;
}

/*
 * Step from buf to its right sibling holding buf until the sibling is locked
 * (lock coupling, left to right), and then release buf unless keep says the
 * caller still needs it.
 */
static Buffer
lion_dir_step_right_coupled(Relation index, Buffer buf, int lockmode, bool keep)
{
	BlockNumber next = LionPageGetOpaque(BufferGetPage(buf))->rightlink;
	Buffer		nbuf;

	Assert(BlockNumberIsValid(next));
	nbuf = lion_dir_readbuf(index, next);
	LockBuffer(nbuf, lockmode);
	lion_dir_check_page(index, BufferGetPage(nbuf), next);
	if (!keep)
		UnlockReleaseBuffer(buf);

	CHECK_FOR_INTERRUPTS();
	return nbuf;
}

/*
 * Scan the run of prefix-equal keys that starts at (*bufp, *offp) for the one
 * sk names, following right links while the run continues.  *bufp is a leaf
 * locked in lockmode and stays locked, though the scan may replace it with a
 * page further right.  On false, *offp is where the scan stopped.
 *
 * guardp is for the insert path's find-or-create (DESIGN.md §21, "Insert of
 * a new entry"): when it is given and the run crosses a page boundary, the
 * page the scan STARTED on is not released but handed back in *guardp, still
 * locked, and the scan couples its steps right.  Every writer of a key of
 * this prefix starts on that same page, so holding it serialises them.
 * *guardp is InvalidBuffer when the scan never left its first page.
 */
static bool
lion_dir_scan_run_ext(Relation index, const LionSearchKey *sk,
					  int lockmode, Buffer *bufp, OffsetNumber *offp,
					  bool *movedright, Buffer *guardp)
{
	Buffer		buf = *bufp;
	OffsetNumber off = *offp;
	bool		moved = false;

	if (guardp != NULL)
		*guardp = InvalidBuffer;

	for (;;)
	{
		Page		page = BufferGetPage(buf);
		OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

		for (; off <= maxoff; off++)
		{
			ItemId		iid = PageGetItemId(page, off);
			LionEntryTuple *e;
			int			c;

			if (!ItemIdIsUsed(iid))
				continue;
			e = (LionEntryTuple *) PageGetItem(page, iid);

			c = lion_cmp_prefix(e, sk);
			if (c > 0)
				goto notfound;
			if (c < 0)
				continue;

			/*
			 * The prefixes tie, so this is a candidate; only the opclass
			 * equality decides, which is what makes citext's 'Alice' and
			 * 'alice' one entry (DESIGN.md §21).
			 */
			if (sk->kind != LION_KIND_VALUE ||
				DatumGetBool(FunctionCall2Coll(sk->eqproc, sk->collation,
											   lion_fetch_key(sk->col,
															  LionEntryGetKey(e)),
											   sk->key)))
			{
				*bufp = buf;
				*offp = off;
				if (movedright != NULL)
					*movedright = moved;
				return true;
			}
		}

		/* The run of prefix-equal keys may continue on the right sibling. */
		if (LionPageIsRightmost(page))
			break;
		if (lion_cmp_prefix(lion_page_highkey(page), sk) > 0)
			break;

		/*
		 * A writer must not step over an unfinished split either: the page it
		 * would land on has no downlink yet, and inserting into it could split
		 * it (see lion_dir_search()).  Only a write scan holds the lock this
		 * needs; a reader simply follows the link.
		 */
		if (lockmode == BUFFER_LOCK_EXCLUSIVE && LionPageIncompleteSplit(page))
		{
			lion_dir_finish_split(index, NULL, sk->col->ix, buf);
			continue;
		}

		if (guardp != NULL)
		{
			bool		keep = !BufferIsValid(*guardp);

			if (keep)
				*guardp = buf;
			buf = lion_dir_step_right_coupled(index, buf, lockmode, keep);
		}
		else
			buf = lion_dir_step_right(index, buf, lockmode);
		off = lion_page_first_data(BufferGetPage(buf));
		moved = true;
	}

notfound:
	*bufp = buf;
	*offp = off;
	if (movedright != NULL)
		*movedright = moved;
	return false;
}

bool
lion_dir_scan_run(Relation index, const LionSearchKey *sk,
				  int lockmode, Buffer *bufp, OffsetNumber *offp,
				  bool *movedright)
{
	return lion_dir_scan_run_ext(index, sk, lockmode, bufp, offp, movedright,
								 NULL);
}

bool
lion_dir_find(Relation index, Relation heaprel, LionIndexState *ix,
			  const LionSearchKey *sk, int lockmode, bool forwrite,
			  Buffer *bufp, OffsetNumber *offnum, bool *movedright)
{
	Buffer		buf;
	OffsetNumber off;

	if (movedright != NULL)
		*movedright = false;

	if (sk->col->ordered && sk->cmpproc == NULL && sk->kind == LION_KIND_VALUE)
	{
		Assert(!forwrite);
		return lion_dir_find_by_scan(index, ix, sk, lockmode, bufp, offnum);
	}

	buf = lion_dir_search(index, heaprel, ix, sk, lockmode, forwrite, &off);

	*bufp = buf;
	*offnum = off;

	if (forwrite)
	{
		Buffer		guard;
		bool		found;
		bool		moved;

		/*
		 * The insert path's find-or-create (DESIGN.md §21).  When the lookup
		 * stays on one leaf, that leaf is held from here to the insert and
		 * nothing more is needed.  When the prefix run crosses a page
		 * boundary, the leaf the run is entered from - the one every writer
		 * of a key of this prefix descends to - is kept locked (the guard) and
		 * handed back INSTEAD of the leaf the scan ended on, so that no other
		 * writer can run the same lookup, miss as well, and create the key a
		 * second time before this one has placed it.
		 */
		found = lion_dir_scan_run_ext(index, sk, lockmode, bufp, offnum,
									  &moved, &guard);
		if (movedright != NULL)
			*movedright = moved;
		if (!BufferIsValid(guard))
			return found;
		if (found)
		{
			UnlockReleaseBuffer(guard);
			return true;
		}
		UnlockReleaseBuffer(*bufp);
		*bufp = guard;
		*offnum = InvalidOffsetNumber;
		Assert(moved);
		return false;
	}

	return lion_dir_scan_run(index, sk, lockmode, bufp, offnum, movedright);
}

/*
 * The leaf where one key column's run of entries begins (DESIGN.md §24).
 *
 * Every entry of that column sorts at or above (attno, MINF), which is a
 * position no stored item can occupy - MINF belongs to downlinks - so the
 * descent lands exactly on the first entry of the column, or on the first
 * entry of a LATER column when this one has none at all.  The caller walks
 * right from there and stops at the first entry whose attno is not its own.
 */
BlockNumber
lion_dir_column_first(Relation index, LionState *col, OffsetNumber *offp)
{
	LionSearchKey sk;
	Buffer		buf;
	OffsetNumber off;
	BlockNumber blk;

	lion_search_key_init(col, &sk, LION_KIND_MINF, (Datum) 0, 0);
	sk.cmpproc = NULL;			/* MINF never reaches the key comparison */

	buf = lion_dir_search(index, NULL, col->ix, &sk, BUFFER_LOCK_SHARE, false,
						  &off);
	blk = BufferGetBlockNumber(buf);
	UnlockReleaseBuffer(buf);

	if (offp != NULL)
		*offp = off;
	return blk;
}

/* ---------------------------------------------------------------------
 * Placing items
 * --------------------------------------------------------------------- */

/*
 * A pivot tuple - a high key or a downlink - carrying src's key and kind.
 * child is the downlink target, or InvalidBlockNumber for a high key; a NULL
 * src makes the minus-infinity downlink.
 */
static LionEntryTuple *
lion_make_pivot(const LionEntryTuple *src, uint16 pivotflag, BlockNumber child,
				Size *size)
{
	Size		keylen = (src != NULL) ? src->keylen : 0;
	Size		total = MAXALIGN(LION_ENTRY_HDRSZ + keylen);
	LionEntryTuple *p = (LionEntryTuple *) palloc0(total);

	p->hash = (src != NULL) ? src->hash : 0;
	p->flags = pivotflag |
		(uint16) ((src != NULL) ?
				  (src->flags & (LION_ENTRY_RESERVED | LION_ENTRY_MINUSINF)) :
				  LION_ENTRY_MINUSINF);
	p->keylen = (uint16) keylen;
	p->head = child;
	p->tail = InvalidBlockNumber;
	p->ncontainers = 0;
	/* A pivot routes to the column its source key belongs to (§24). */
	p->attno = (src != NULL) ? src->attno : 0;
	p->ntids = 0;
	if (keylen > 0)
		memcpy(LionEntryGetKey(p), LionEntryGetKey(src), keylen);

	*size = total;
	return p;
}

void
lion_dir_delete(Relation index, Buffer buf, OffsetNumber *offs, int noffs)
{
	LionWalState *xstate;
	Page		p;

	Assert(noffs > 0);

	/*
	 * This is VACUUM deleting emptied entries, so replay takes a CLEANUP lock
	 * on the leaf (DESIGN.md §25): it deletes items, and a standby reader
	 * parked between two entries of the page has to be waited for exactly as
	 * a reader on the primary is.
	 */
	xstate = lion_wal_begin(index);
	p = lion_wal_register_buffer(xstate, buf, LION_WALBUF_CLEANUP);
	PageIndexMultiDelete(p, offs, noffs);
	lion_wal_op(xstate, p, LION_OP_MULTIDEL, 0, (uint16) noffs, offs,
				sizeof(OffsetNumber) * noffs);
	lion_wal_finish(xstate, LION_XLOG_ENTRY);
}

void
lion_dir_place(Relation index, Relation heaprel, LionIndexState *ix, Buffer buf,
			   OffsetNumber off, bool replace, LionEntryTuple *item, Size size)
{
	Page		page = BufferGetPage(buf);
	LionWalState *xstate;
	Page		p;

	/*
	 * Never touch a page whose own split never finished: a second split of it
	 * would overwrite the flag and lose the first one's downlink.  Every
	 * caller has come through a descent that repairs what it meets, so this is
	 * the last line of the same rule rather than the first.
	 */
	if (LionPageIncompleteSplit(page))
	{
		lion_dir_finish_split(index, heaprel, ix, buf);
		page = BufferGetPage(buf);
	}

	Assert(off >= lion_page_first_data(page));
	Assert(size <= LION_MAX_ITEM_SIZE);

	if (replace)
	{
		xstate = lion_wal_begin(index);
		p = lion_wal_register_buffer(xstate, buf, LION_WALBUF_STD);
		lion_wal_save_item(xstate, p, off);
		if (PageIndexTupleOverwrite(p, off, (char *) item, size))
		{
			/*
			 * An INLINE entry that gains one member differs from its
			 * predecessor in its counters and the bytes from the insertion
			 * point up, so the record carries a DELTA rather than the whole
			 * payload (DESIGN.md §25).
			 */
			lion_wal_op_replace(xstate, p, off, item, size);
			lion_wal_finish(xstate, LION_XLOG_ENTRY);
			return;
		}

		/*
		 * Nothing was written - PageIndexTupleOverwrite() tests first - so
		 * the record can still be abandoned, which DESIGN.md §25 requires of
		 * every abort now that an rmgr-mode record is inside a critical
		 * section.
		 */
		lion_wal_abort(xstate);
	}
	else if (PageGetFreeSpace(page) >= MAXALIGN(size))
	{
		xstate = lion_wal_begin(index);
		p = lion_wal_register_buffer(xstate, buf, LION_WALBUF_STD);
		if (PageAddItemExtended(p, (char *) item, size, off, 0) ==
			InvalidOffsetNumber)
			elog(ERROR, "lion index \"%s\": could not add an item to block %u",
				 RelationGetRelationName(index), BufferGetBlockNumber(buf));
		lion_wal_op(xstate, p, LION_OP_ADD, off, 0, item, size);
		lion_wal_finish(xstate, LION_XLOG_ENTRY);
		return;
	}

	lion_dir_split(index, heaprel, ix, buf, off, replace, item, size);
}

/* ---------------------------------------------------------------------
 * Splits
 * --------------------------------------------------------------------- */

typedef struct LionDirItem
{
	LionEntryTuple *item;
	Size		size;			/* its exact length */
	Size		need;			/* MAXALIGN(size) plus a line pointer */
	Size		pivot;			/* what a pivot copy of its key would cost */
} LionDirItem;

/*
 * Where to cut the item list.  Aim at half the bytes, then pull the cut
 * towards whichever side does not fit: the left half has to hold its new high
 * key (a copy of items[firstright]'s key) and the right half the old one.
 *
 * The two walk-backs can pull in OPPOSITE directions, and then there is no cut
 * at all: an 8 KiB page holding two ~4000-byte entries under a short high key
 * has room for neither half once a 6000-byte entry is inserted between them
 * (the largest entry is 6088 bytes and the largest pivot 2036, so one item
 * plus one pivot is 8128 and two items plus a pivot is not).  That is reported
 * with *ok = false rather than as an error: the caller then splits the page as
 * it STANDS - which always has a cut, because k = 1 puts one item plus its
 * high key on the left and a suffix of what the page already held plus its old
 * high key on the right - and places the item again afterwards, which
 * terminates because every split moves at least one item off the page and a
 * page holding one item plus the new one always splits.
 *
 * *(This replaces an error branch, reported by the 2026-09-22 review of §21.
 * With today's size caps the shape is in fact unreachable - a fresh entry is
 * at most 2052 bytes on a page and a replacement grows by at most a few dozen,
 * and either way a cut exists - but that rests on three independent constants
 * (LION_MAX_ENTRY_SIZE, LION_MAX_PIVOT_SIZE and how much one insert can add),
 * and none of them should be load-bearing for whether an INSERT errors out.
 * DESIGN.md §21 "Split" carries the arithmetic.)*
 */
static int
lion_dir_choose_split(LionDirItem *items, int nitems, Size oldhk, bool append,
					  bool *ok)
{
	Size		budget = LION_PAGE_CAPACITY;
	Size	   *prefix;			/* prefix[i] = bytes of items[0 .. i) */
	Size		total;
	int			k;
	int			i;

	Assert(nitems >= 2);
	*ok = true;

	prefix = (Size *) palloc(sizeof(Size) * (nitems + 1));
	prefix[0] = 0;
	for (i = 0; i < nitems; i++)
		prefix[i + 1] = prefix[i] + items[i].need;
	total = prefix[nitems];

	/*
	 * Ascending keys - the shape of a growing index - put only the NEW item on
	 * the right page, so the left one stays as full as it can instead of half
	 * empty.  The cut then walks back just far enough for the high key the
	 * left page now needs, which is a handful of items and not half of them:
	 * without that walk-back the cut is rejected outright (the page is full,
	 * which is why it is being split) and an ascending build lands at 50%.
	 */
	if (append)
		k = nitems - 1;
	else
	{
		for (k = 1; k < nitems; k++)
		{
			if (prefix[k] * 2 >= total)
				break;
		}
		if (k >= nitems)
			k = nitems - 1;
	}

	while (k > 1 && prefix[k] + items[k].pivot > budget)
		k--;
	while (k < nitems - 1 && (total - prefix[k]) + oldhk > budget)
		k++;

	if (prefix[k] + items[k].pivot > budget ||
		(total - prefix[k]) + oldhk > budget)
		*ok = false;

	pfree(prefix);
	Assert(k >= 1 && k < nitems);
	return k;
}

/* Fill a freshly initialised directory page image with items[from .. to). */
static void
lion_dir_fill_page(Relation index, LionWalState *xstate, Page page,
				   LionEntryTuple *hk, Size hksz,
				   LionDirItem *items, int from, int to)
{
	OffsetNumber off = FirstOffsetNumber;
	int			i;

	if (hk != NULL)
	{
		if (PageAddItemExtended(page, (char *) hk, hksz, FirstOffsetNumber,
								0) == InvalidOffsetNumber)
			elog(ERROR, "lion index \"%s\": could not place a high key",
				 RelationGetRelationName(index));
		lion_wal_op(xstate, page, LION_OP_ADD, FirstOffsetNumber, 0, hk, hksz);
		off = OffsetNumberNext(off);
	}

	/*
	 * Both halves of a directory split are rebuilt from scratch, so both are
	 * logged in full: one operation naming the first offset and the number of
	 * items, followed by the items themselves.  DESIGN.md §25 preferred
	 * logging only the moved half - which is what a CONTAINER split does - and
	 * a directory split cannot, because the only page it could derive the
	 * other half from is the very page it re-initialises (§25 records the
	 * deviation).
	 */
	lion_wal_op(xstate, page, LION_OP_ADDMANY, off, (uint16) (to - from),
				NULL, 0);

	for (i = from; i < to; i++)
	{
		uint16		isz = (uint16) items[i].size;

		if (PageAddItemExtended(page, (char *) items[i].item, items[i].size,
								InvalidOffsetNumber, 0) == InvalidOffsetNumber)
			elog(ERROR, "lion index \"%s\": could not place a directory item",
				 RelationGetRelationName(index));
		lion_wal_op_append(xstate, page, &isz, sizeof(uint16));
		lion_wal_op_append(xstate, page, items[i].item, isz);
	}
}

static void
lion_dir_split(Relation index, Relation heaprel, LionIndexState *ix, Buffer buf,
			   OffsetNumber off, bool replace, LionEntryTuple *newitem,
			   Size newsize)
{
	Page		page = BufferGetPage(buf);
	BlockNumber pblk = BufferGetBlockNumber(buf);
	bool		isleaf = LionPageIsLeaf(page);
	bool		isroot = LionPageIsRoot(page);
	uint16		level = LionPageGetOpaque(page)->level;
	BlockNumber oldright = LionPageGetOpaque(page)->rightlink;
	BlockNumber leftlink = LionPageGetOpaque(page)->leftlink;
	OffsetNumber first = lion_page_first_data(page);
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	OffsetNumber o;
	PGAlignedBlock *copy;
	Page		cpage;
	LionDirItem *items;
	int			nitems = 0;
	int			firstright;
	int			i;
	LionEntryTuple *oldhk = NULL;
	Size		oldhksz = 0;		/* its exact length */
	Size		oldhkneed = 0;		/* what it costs on a page */
	LionEntryTuple *lefthk;
	Size		lefthksz;
	LionEntryTuple *sep;
	Size		sepsz;
	LionWalState *xstate;
	Page		pP;
	Page		pR;
	Buffer		rbuf;
	Buffer		qbuf = InvalidBuffer;
	Buffer		metabuf;
	Buffer		newroot = InvalidBuffer;
	LionEntryTuple *minf = NULL;
	Size		minfsz = 0;
	BlockNumber rblk;
	bool		append;
	bool		cutok;

	Assert(!LionPageIncompleteSplit(page));
	if (isroot && BlockNumberIsValid(oldright))
		elog(ERROR, "lion index \"%s\": root page %u has a right sibling",
			 RelationGetRelationName(index), pblk);

	/* Work from a private copy: both pages are rebuilt from scratch. */
	copy = (PGAlignedBlock *) palloc(sizeof(PGAlignedBlock));
	cpage = (Page) copy->data;
	memcpy(cpage, page, BLCKSZ);

	items = (LionDirItem *) palloc(sizeof(LionDirItem) * (maxoff + 2));

	if (!LionPageIsRightmost(cpage))
	{
		oldhk = lion_page_highkey(cpage);
		oldhksz = lion_page_itemsz(cpage, FirstOffsetNumber);
		oldhkneed = MAXALIGN(oldhksz) + sizeof(ItemIdData);
	}

	for (o = first; o <= maxoff + 1; o++)
	{
		if (newitem != NULL && o == off)
		{
			items[nitems].item = newitem;
			items[nitems].size = newsize;
			nitems++;
		}
		if (o > maxoff)
			break;
		if (newitem != NULL && replace && o == off)
			continue;				/* the stale version goes away */
		if (!ItemIdIsUsed(PageGetItemId(cpage, o)))
			continue;
		items[nitems].item = lion_page_item(cpage, o);
		items[nitems].size = lion_page_itemsz(cpage, o);
		nitems++;
	}

	for (i = 0; i < nitems; i++)
	{
		items[i].need = MAXALIGN(items[i].size) + sizeof(ItemIdData);
		items[i].pivot = MAXALIGN(LION_ENTRY_HDRSZ + items[i].item->keylen) +
			sizeof(ItemIdData);
	}

	if (nitems < 2)
		elog(ERROR, "lion index \"%s\": block %u cannot hold a single item of %zu bytes",
			 RelationGetRelationName(index), pblk, newsize);

	append = newitem != NULL && !BlockNumberIsValid(oldright) && !replace &&
		off > maxoff;
	firstright = lion_dir_choose_split(items, nitems, oldhkneed, append, &cutok);

	if (!cutok)
	{
		/*
		 * No cut can hold both halves WITH the new item (see
		 * lion_dir_choose_split()).  Split the page as it stands, which always
		 * has one, and then place the item again on whichever half now owns
		 * it.  Every split moves at least one item off this page, so the retry
		 * terminates: a page that holds one item always takes a second.
		 */
		Assert(newitem != NULL);
		pfree(items);
		pfree(copy);
		lion_dir_split(index, heaprel, ix, buf, InvalidOffsetNumber, false,
					   NULL, 0);
		lion_dir_place_again(index, heaprel, ix, buf, replace, newitem,
							 newsize);
		return;
	}

	lefthk = lion_make_pivot(items[firstright].item, LION_ENTRY_HIGHKEY,
							 InvalidBlockNumber, &lefthksz);
	sep = lion_make_pivot(items[firstright].item, LION_ENTRY_DOWNLINK,
						  InvalidBlockNumber, &sepsz);

	/*
	 * The minus-infinity downlink of a new root is built HERE rather than
	 * where it is placed, because it is a palloc and the record it goes into
	 * is a critical section (DESIGN.md §25).
	 */
	if (isroot)
		minf = lion_make_pivot(NULL, LION_ENTRY_DOWNLINK, pblk, &minfsz);

	/*
	 * Everything fallible happens HERE, before the record opens: the sibling
	 * (and, for a root split, the new root) are taken from the free space map
	 * or the end of the relation, and the two further buffers the record
	 * needs are read and locked.  DESIGN.md §25 requires it, because an
	 * rmgr-mode record is written inside a critical section, where extending
	 * a relation would turn a full disk into a PANIC.
	 *
	 * The order is the one the old code took inside the record - sibling,
	 * then the old right sibling, then the meta page - so the lock ordering
	 * of §21 is unchanged.
	 */
	rbuf = lion_alloc_page(index, heaprel, true);
	rblk = BufferGetBlockNumber(rbuf);
	sep->head = rblk;

	if (isroot)
		newroot = lion_alloc_page(index, heaprel, true);

	if (BlockNumberIsValid(oldright))
	{
		qbuf = lion_dir_readbuf(index, oldright);
		LockBuffer(qbuf, BUFFER_LOCK_EXCLUSIVE);
	}

	metabuf = ReadBuffer(index, LION_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

	xstate = lion_wal_begin(index);

	/*
	 * The left page is REBUILT from scratch, so it is registered as a page
	 * this record initialises: replay zeroes it and fills it from the log,
	 * and no full-page image is needed for it at all.
	 */
	pP = lion_wal_register_buffer(xstate, buf, LION_WALBUF_INIT);
	lion_init_page(pP, (uint16) ((isleaf ? LION_PAGE_BUCKET : LION_PAGE_DIR) |
								 (isroot ? 0 : LION_PAGE_INCOMPLETE_SPLIT)));
	lion_wal_op(xstate, pP, LION_OP_INIT, 0, LionPageGetOpaque(pP)->flags,
				NULL, 0);
	pR = lion_wal_init_buffer(xstate, rbuf,
							  isleaf ? LION_PAGE_BUCKET : LION_PAGE_DIR);

	/* Rebuild the left page. */
	LionPageGetOpaque(pP)->level = level;
	LionPageGetOpaque(pP)->leftlink = leftlink;
	LionPageGetOpaque(pP)->rightlink = rblk;
	lion_dir_fill_page(index, xstate, pP, lefthk, lefthksz, items, 0,
					   firstright);
	lion_wal_log_special(xstate, pP);

	/* ... and build the right one. */
	LionPageGetOpaque(pR)->level = level;
	LionPageGetOpaque(pR)->leftlink = pblk;
	LionPageGetOpaque(pR)->rightlink = oldright;
	lion_dir_fill_page(index, xstate, pR, oldhk, oldhksz, items, firstright,
					   nitems);
	lion_wal_log_special(xstate, pR);

	if (BufferIsValid(qbuf))
	{
		Page		pQ = lion_wal_register_buffer(xstate, qbuf,
												  LION_WALBUF_STD);

		LionPageGetOpaque(pQ)->leftlink = rblk;
		lion_wal_log_special(xstate, pQ);
	}

	if (isroot)
	{
		/*
		 * A root split is atomic: left, right, the new root and the meta page
		 * are four buffers, exactly what GenericXLog allows, so the tree never
		 * has a root the meta page does not name.  The old root loses its
		 * ROOT flag in the same record, which is what makes every other
		 * backend's cached root block detect the change.
		 */
		Page		pN;
		Page		pM;
		BlockNumber nblk;
		OffsetNumber noff;

		pN = lion_wal_init_buffer(xstate, newroot,
								  LION_PAGE_DIR | LION_PAGE_ROOT);
		nblk = BufferGetBlockNumber(newroot);
		LionPageGetOpaque(pN)->level = level + 1;

		noff = PageAddItemExtended(pN, (char *) minf, minfsz,
								   InvalidOffsetNumber, 0);
		if (noff == InvalidOffsetNumber)
			elog(ERROR, "lion index \"%s\": could not build a new root",
				 RelationGetRelationName(index));
		lion_wal_op(xstate, pN, LION_OP_ADD, noff, 0, minf, minfsz);

		noff = PageAddItemExtended(pN, (char *) sep, sepsz,
								   InvalidOffsetNumber, 0);
		if (noff == InvalidOffsetNumber)
			elog(ERROR, "lion index \"%s\": could not build a new root",
				 RelationGetRelationName(index));
		lion_wal_op(xstate, pN, LION_OP_ADD, noff, 0, sep, sepsz);
		lion_wal_log_special(xstate, pN);

		pM = lion_wal_register_buffer(xstate, metabuf, LION_WALBUF_STD);
		LionPageGetMeta(pM)->root = nblk;
		LionPageGetMeta(pM)->height = level + 1;
		LionPageGetMeta(pM)->dirpages += 2;		/* the sibling and the root */
		lion_wal_op(xstate, pM, LION_OP_META, 0, 0, LionPageGetMeta(pM),
					sizeof(LionMetaPageData));

		lion_wal_finish(xstate, LION_XLOG_SPLIT);

		ix->meta.root = nblk;
		ix->meta.height = level + 1;

		UnlockReleaseBuffer(metabuf);
		UnlockReleaseBuffer(newroot);
		UnlockReleaseBuffer(rbuf);
		pfree(minf);
		pfree(sep);
		pfree(lefthk);
		pfree(items);
		pfree(copy);
		return;
	}

	/*
	 * The meta page counts the directory's pages, which is what the cost model
	 * needs in order to tell a directory page from a container page without
	 * reading the index (lion_index_dir_pages(), DESIGN.md §21).  It is the
	 * fourth and last buffer of this record.
	 */
	{
		Page		pM = lion_wal_register_buffer(xstate, metabuf,
												  LION_WALBUF_STD);

		LionPageGetMeta(pM)->dirpages++;
		lion_wal_op(xstate, pM, LION_OP_META, 0, 0, LionPageGetMeta(pM),
					sizeof(LionMetaPageData));
		lion_wal_finish(xstate, LION_XLOG_SPLIT);
		UnlockReleaseBuffer(metabuf);
	}

	if (BufferIsValid(qbuf))
		UnlockReleaseBuffer(qbuf);
	UnlockReleaseBuffer(rbuf);
	pfree(sep);
	pfree(lefthk);
	pfree(items);
	pfree(copy);

	/*
	 * Test hook: the split is on disk and the left page says so, but its right
	 * sibling has no downlink yet.  test/recovery/run.sh crashes the server
	 * here and proves that the next writer's descent repairs it.  Compiles to
	 * nothing without --enable-injection-points.
	 */
	LION_INJECTION_POINT("lion-dir-split-incomplete");

	lion_dir_finish_split(index, heaprel, ix, buf);
}

/*
 * Place an item again after the page it belonged on was split without it.
 *
 * buf is the LEFT half, held EXCLUSIVE, and stays so; the item goes there or
 * on the brand new right sibling, which this decides with the high key the
 * split just gave buf.  Locking the sibling while holding buf is the allowed
 * direction (left to right, DESIGN.md §5), and nothing ever holds a parent
 * while waiting for a child, so the sibling's own split - which goes upwards -
 * cannot close a cycle with it.
 */
static void
lion_dir_place_again(Relation index, Relation heaprel, LionIndexState *ix,
					 Buffer buf, bool replace, LionEntryTuple *item, Size size)
{
	LionSearchKey sk;
	Page		page = BufferGetPage(buf);

	lion_search_key_exact(ix, &sk, item);

	if (!LionPageIsRightmost(page) &&
		lion_cmp_entry(lion_page_highkey(page), &sk) <= 0)
	{
		BlockNumber next = LionPageGetOpaque(page)->rightlink;
		Buffer		rbuf = lion_dir_readbuf(index, next);
		OffsetNumber roff;

		LockBuffer(rbuf, BUFFER_LOCK_EXCLUSIVE);
		lion_dir_check_page(index, BufferGetPage(rbuf), next);
		roff = lion_dir_binsrch(BufferGetPage(rbuf), &sk);
		lion_dir_place(index, heaprel, ix, rbuf, roff, replace, item, size);
		UnlockReleaseBuffer(rbuf);
		return;
	}

	lion_dir_place(index, heaprel, ix, buf,
				   lion_dir_binsrch(page, &sk), replace, item, size);
}

/*
 * Is the downlink for rblk already in the parent level, immediately after the
 * one at (pbuf, off)?  A crash between the two records of a split leaves the
 * flag set with the downlink already there, and the repair must not add a
 * second one.
 */
static bool
lion_dir_downlink_present(Relation index, Buffer pbuf, OffsetNumber off,
						 BlockNumber rblk)
{
	Page		page = BufferGetPage(pbuf);
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	BlockNumber next;
	Buffer		nbuf;
	bool		found;

	if (off < maxoff)
		return lion_page_item(page, OffsetNumberNext(off))->head == rblk;

	/* It would be the first item of the parent's right sibling. */
	next = LionPageGetOpaque(page)->rightlink;
	if (!BlockNumberIsValid(next))
		return false;

	nbuf = ReadBuffer(index, next);
	LockBuffer(nbuf, BUFFER_LOCK_SHARE);
	{
		Page		np = BufferGetPage(nbuf);
		OffsetNumber nfirst = lion_page_first_data(np);

		found = nfirst <= PageGetMaxOffsetNumber(np) &&
			lion_page_item(np, nfirst)->head == rblk;
	}
	UnlockReleaseBuffer(nbuf);

	return found;
}

/*
 * Finish the split of pbuf, which the caller holds EXCLUSIVE and keeps: put
 * the downlink of its right sibling into the parent, then clear the flag.
 *
 * heaprel may be NULL; a page taken from the free space map then simply is
 * not one, and the relation is extended instead.
 */
static void
lion_dir_finish_split(Relation index, Relation heaprel, LionIndexState *ix,
					  Buffer pbuf)
{
	Page		ppage = BufferGetPage(pbuf);
	BlockNumber pblk = BufferGetBlockNumber(pbuf);
	BlockNumber rblk = LionPageGetOpaque(ppage)->rightlink;
	uint16		level = LionPageGetOpaque(ppage)->level;
	OffsetNumber first = lion_page_first_data(ppage);
	LionSearchKey sk;
	LionSearchKey *skp = NULL;
	LionEntryTuple *sep;
	Size		sepsz;
	Buffer		parent;
	OffsetNumber off;
	LionWalState *xstate;
	Page		p;

	Assert(LionPageIncompleteSplit(ppage));

	if (LionPageIsRoot(ppage) || !BlockNumberIsValid(rblk))
		elog(ERROR, "lion index \"%s\": block %u is flagged as an incomplete split but has no right sibling to link",
			 RelationGetRelationName(index), pblk);

	/* The separator is this page's high key, a copy of its sibling's first. */
	sep = lion_make_pivot(lion_page_highkey(ppage), LION_ENTRY_DOWNLINK, rblk,
						  &sepsz);

	/*
	 * A key that routes to THIS page, for finding its downlink: any key it
	 * holds will do, because an exact search key routes to the page that holds
	 * it.  An empty page (VACUUM removed everything) has none, and then the
	 * parent level is scanned from its leftmost page instead.
	 */
	if (first <= PageGetMaxOffsetNumber(ppage))
	{
		lion_search_key_exact(ix, &sk, lion_page_item(ppage, first));
		skp = &sk;
	}

	parent = lion_dir_find_parent(index, heaprel, ix, skp, pblk, level, &off);

	if (!lion_dir_downlink_present(index, parent, off, rblk))
		lion_dir_place(index, heaprel, ix, parent, OffsetNumberNext(off),
					   false, sep, sepsz);

	UnlockReleaseBuffer(parent);
	pfree(sep);

	xstate = lion_wal_begin(index);
	p = lion_wal_register_buffer(xstate, pbuf, LION_WALBUF_STD);
	LionPageGetOpaque(p)->flags &= ~(uint16) LION_PAGE_INCOMPLETE_SPLIT;
	lion_wal_op(xstate, p, LION_OP_FLAGS, 0, LionPageGetOpaque(p)->flags,
				NULL, 0);
	lion_wal_finish(xstate, LION_XLOG_SPLIT_CLEAR);
}

/*
 * The page at childlevel + 1 that holds the downlink of childblk, locked
 * EXCLUSIVE, with *offp its offset.  sk may be NULL, which starts the scan at
 * the leftmost page of that level.
 */
static Buffer
lion_dir_find_parent(Relation index, Relation heaprel, LionIndexState *ix,
					 const LionSearchKey *sk, BlockNumber childblk,
					 uint16 childlevel, OffsetNumber *offp)
{
	Buffer		buf;
	BlockNumber blk;

	/* Descend to the level above the child, reading only. */
	buf = lion_dir_get_root(index, ix);
	if (LionPageGetOpaque(BufferGetPage(buf))->level <= childlevel)
	{
		UnlockReleaseBuffer(buf);
		elog(ERROR, "lion index \"%s\": no level above %u for block %u",
			 RelationGetRelationName(index), childlevel, childblk);
	}

	for (;;)
	{
		Page		page = BufferGetPage(buf);
		OffsetNumber off;
		BlockNumber child;

		if (LionPageGetOpaque(page)->level == childlevel + 1)
			break;

		if (sk != NULL)
		{
			while (!LionPageIsRightmost(page) &&
				   lion_cmp_entry(lion_page_highkey(page), sk) <= 0)
			{
				buf = lion_dir_step_right(index, buf, BUFFER_LOCK_SHARE);
				page = BufferGetPage(buf);
			}
			off = lion_page_downlink(page, sk);
		}
		else
			off = lion_page_first_data(page);

		child = lion_page_item(page, off)->head;
		UnlockReleaseBuffer(buf);
		buf = lion_dir_readbuf(index, child);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		lion_dir_check_page(index, BufferGetPage(buf), child);
		CHECK_FOR_INTERRUPTS();
	}

	/*
	 * At the right level.  Take the page EXCLUSIVE (the descent had it SHARE)
	 * and scan right until the downlink turns up.  The scan holds one page at
	 * a time and only ever moves rightwards, so two repairers cannot deadlock.
	 */
	blk = BufferGetBlockNumber(buf);
	UnlockReleaseBuffer(buf);

	for (;;)
	{
		Page		page;
		OffsetNumber off;
		OffsetNumber maxoff;

		buf = lion_dir_readbuf(index, blk);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
		lion_dir_check_page(index, page, blk);

		/*
		 * This page's own split may never have finished.  Finish it first: the
		 * downlink we want may be on the page its sibling became, and in any
		 * case nothing may add an item to a page whose flag a later split of
		 * it would overwrite.  The recursion is bounded by the height, because
		 * every step of it goes one level up.
		 */
		if (LionPageIncompleteSplit(page))
		{
			lion_dir_finish_split(index, heaprel, ix, buf);
			page = BufferGetPage(buf);
		}

		maxoff = PageGetMaxOffsetNumber(page);
		for (off = lion_page_first_data(page); off <= maxoff; off++)
		{
			if (lion_page_item(page, off)->head == childblk)
			{
				*offp = off;
				return buf;
			}
		}

		blk = LionPageGetOpaque(page)->rightlink;
		UnlockReleaseBuffer(buf);
		if (!BlockNumberIsValid(blk))
			elog(ERROR, "lion index \"%s\": no downlink for block %u at level %u",
				 RelationGetRelationName(index), childblk, childlevel + 1);
		CHECK_FOR_INTERRUPTS();
	}
}

/* ---------------------------------------------------------------------
 * Adding an entry
 * --------------------------------------------------------------------- */

/*
 * Insert a brand new entry.  (*bufp, off, movedright) are what lion_dir_find()
 * with forwrite left behind when it did not find the key; *bufp is held
 * EXCLUSIVE and stays so - it is the caller's to release - and a page further
 * right the entry ends up on is released here.
 *
 * Find-or-create is ONE serialised operation (DESIGN.md §21): whatever leaf
 * *bufp is, it has been held since the unsuccessful lookup, and every writer
 * of a key of this prefix has to lock it first, so no one can have created
 * the key in the meantime and the insert needs no second existence check.
 */
void
lion_dir_add_entry(Relation index, Relation heaprel, LionIndexState *ix,
				   Buffer *bufp, OffsetNumber off, bool movedright,
				   LionEntryTuple *entry, Size size)
{
	LionSearchKey exact;
	Buffer		guard = *bufp;
	Buffer		buf = guard;

	lion_search_key_exact(ix, &exact, entry);

	if (movedright)
	{
		/*
		 * The lookup crossed a page boundary inside one prefix run, so *bufp
		 * is the GUARD - the leaf the run is entered from, still locked - and
		 * the key's exact bytewise position may be on any page of the run.
		 * Walk right to it with the guard held, coupling each step (left to
		 * right, the lock order of DESIGN.md §5), and finishing any split
		 * whose right half the walk is about to enter, as every writer must.
		 * Runs longer than one item need an opclass whose comparison ties for
		 * distinct entries, so this is rare; the guard makes it correct.
		 *
		 * Test hook: the window between the unsuccessful lookup and the
		 * insert, which test/isolation/dir_insert_race.spec parks a writer in
		 * while a second one inserts the same key.
		 */
		LION_INJECTION_POINT("lion-dir-add-entry-spanning");

		for (;;)
		{
			Page		page = BufferGetPage(buf);

			if (LionPageIsRightmost(page) ||
				lion_cmp_entry(lion_page_highkey(page), &exact) > 0)
				break;
			if (LionPageIncompleteSplit(page))
			{
				lion_dir_finish_split(index, heaprel, ix, buf);
				continue;
			}
			buf = lion_dir_step_right_coupled(index, buf,
											  BUFFER_LOCK_EXCLUSIVE,
											  buf == guard);
		}
		off = lion_dir_binsrch(BufferGetPage(buf), &exact);
	}
	else
	{
		Page		page = BufferGetPage(buf);
		OffsetNumber first = lion_page_first_data(page);

		while (off > first &&
			   lion_cmp_entry(lion_page_item(page, OffsetNumberPrev(off)),
							  &exact) > 0)
			off = OffsetNumberPrev(off);
	}

	/*
	 * A split of buf takes its right sibling, the meta page and then the
	 * parent; nothing to the right of buf is held, and the guard (if it is not
	 * buf itself) is to its LEFT, so the split's lock order is the ordinary
	 * one with one more page held on the left - the same shape as
	 * lion_dir_place_again().
	 */
	lion_dir_place(index, heaprel, ix, buf, off, false, entry, size);

	if (buf != guard)
		UnlockReleaseBuffer(buf);
}
