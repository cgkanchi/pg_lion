/*-------------------------------------------------------------------------
 *
 * lion_scan.c
 *		Bitmap scans (DESIGN.md section 5, SCAN) and plain index scans
 *		(DESIGN.md §29) of the lion index.
 *
 * The plain scan, liongettuple() at the end of this file, answers the same
 * quals with the same choice of which to answer (lion_scan_choose()) and the
 * same set trees, but pulls them as a stream of containers - one container's
 * TIDs at a time - from a SOURCE that lion_source_open() also offers to other
 * callers.  Between two calls it holds no pin under an MVCC snapshot and the
 * page of its current batch under any other (§29.5).  The rest of this
 * comment is about the bitmap scan, whose walk and trees the source shares.
 *
 * A lion index answers an equality to a value, `= ANY (array)` (DESIGN.md
 * §15, amsearcharray), a range - every `<`, `<=`, `>=` and `>` on the column
 * at once, as one bounded walk of the sorted directory (DESIGN.md §28) - and
 * `IS NULL` or `IS NOT NULL` (DESIGN.md §14, amsearchnulls).  The scan finds
 * the entries a qual selects and emits their posting sets into the caller's
 * TIDBitmap.
 *
 * Several quals are INTERSECTED, whether they are on one key column (`b =
 * ANY (x) AND b = ANY (y)`, lion_scan_choose()) or on the several columns of
 * a MULTICOLUMN index (DESIGN.md §24), which holds each column's keys as an
 * independent set of entries, so that a scan key resolves to its column by
 * sk_attno.  The AND goes through the very expression evaluator a multi-key
 * query already uses, because "the sets of a and the sets of b" is an AND of
 * set trees whatever produced them.  A qual
 * the sets cannot express (`IS NOT NULL`, a multi-key query that needs the
 * whole index) is dropped and the TIDs are marked for recheck, which is
 * always correct: the bitmap heap scan re-applies the original quals.  A
 * range column is a walk and not a set; its entries are ANDed with the other
 * columns' answer a container at a time (lion_emit_intersect()), and on a
 * column with summaries (DESIGN.md §32) the walk reads the summaries of the
 * buckets the range covers whole.
 *
 * A multi-key opclass (DESIGN.md §17) answers `@>`, `&&`, `<@` and `@@`
 * instead.  The query is handed to the opclass's extractQuery function, which
 * yields keys and a mode; an exact mode gives a boolean tree over those keys,
 * whose posting sets are combined by the very evaluator the count pushdown
 * uses (lion_sets_iterate(), DESIGN.md §9 cursors), and anything else falls
 * back to emitting every indexed row with recheck set.
 *
 * Only one page lock is held at a time, and never across the TIDBitmap calls:
 * every page is copied into backend-local memory before its containers are
 * emitted.  The bucket page is always released before any container page is
 * pinned, which is the reader side of the deadlock rule in DESIGN.md §11.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/itup.h"
#include "access/relscan.h"
#include "access/xlog.h"
#if PG_VERSION_NUM >= 190000
#include "executor/instrument_node.h"
#endif
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "lion.h"
#include "lion_count.h"

/*
 * Per key column: the probe resolved for the last scan key type seen on that
 * column - lion_probe_init(), the same resolution the count path makes
 * (DESIGN.md §21), so the two can never descend differently for one value.
 * Cached per column because a multicolumn scan has one scan key per column
 * and they would otherwise evict each other on every rescan (DESIGN.md §24).
 */
typedef struct LionScanCol
{
	Oid			subtype;		/* type of the scan key argument */
	bool		probevalid;
	LionProbe	probe;
} LionScanCol;

typedef struct LionScanOpaqueData
{
	LionIndexState *ix;			/* cached relation state */
	LionScanCol *cols;			/* [ix->ncolumns] */
	bool		recheck;		/* the emitted TIDs are a superset */

	/* The keys being answered: the scan's own, or a lion_source_open()'s. */
	Relation	index;
	ScanKey		keys;
	int			nkeys;

	/*
	 * The plain scan (liongettuple(), DESIGN.md §29).  The source is opened
	 * by the first call after a rescan and lives in gtcxt, which amrescan
	 * resets; the batch is the members of the container it last handed out,
	 * as lo values of that container (lion_container_to_array()), turned into
	 * a TID one at a time.
	 */
	MemoryContext gtcxt;
	struct LionSource *src;
	bool		srcdone;		/* the source is exhausted */
	uint16	   *lo;				/* [LION_CONTAINER_RANGE], allocated once */
	int			nlo;
	int			pos;
	BlockNumber firstblk;		/* first heap block of the batch's container */
	IndexTuple	nullitup;		/* what an index-only scan is handed (§29.9) */
	bool		heapcheck;		/* ... after its TID is looked up in the heap */
} LionScanOpaqueData;

typedef LionScanOpaqueData *LionScanOpaque;

static void lion_source_release(struct LionSource *src);
static void lion_source_entry_set(Relation index, LionEntryTuple *entry,
								  Size itemlen, MemoryContext cxt,
								  LionPostingSet *ps);
static bool lion_scan_col_ranges(LionScanOpaque so, LionState *col,
								 ScanKey best, LionRange **rangesp,
								 int *nrangesp);
static double lion_scan_window_bytes(void);

/* State threaded through lion_container_iterate() by lion_container_to_tbm() */
typedef struct LionTbmState
{
	TIDBitmap  *tbm;
	BlockNumber firstblk;		/* first heap block of the container */
	BlockNumber curblk;			/* heap block the buffered TIDs belong to */
	int			ntids;
	int64		total;
	bool		recheck;		/* passed on to tbm_add_tuples() */
	ItemPointerData tids[LION_MAX_OFFSET + 1];
} LionTbmState;

static bool
lion_tbm_callback(uint16 lo, void *arg)
{
	LionTbmState *st = (LionTbmState *) arg;
	uint16		blkinc;
	OffsetNumber off;
	BlockNumber blk;

	lion_lo_split(lo, &blkinc, &off);
	blk = st->firstblk + (BlockNumber) blkinc;

	/*
	 * Members arrive in ascending lo order, hence in heap block order, so one
	 * tbm_add_tuples() call per heap block is enough.  The length test is
	 * belt and braces: a corrupt container must not overrun tids[].
	 */
	if (st->ntids > 0 &&
		(blk != st->curblk || st->ntids > LION_MAX_OFFSET))
	{
		tbm_add_tuples(st->tbm, st->tids, st->ntids, st->recheck);
		st->ntids = 0;
	}
	st->curblk = blk;

	Assert(st->ntids <= LION_MAX_OFFSET);
	ItemPointerSet(&st->tids[st->ntids], blk, off);
	st->ntids++;
	st->total++;
	return true;
}

/*
 * The same for a sparse segment (DESIGN.md §13): its pairs are sorted by
 * (ckey, lo), hence by heap block, so the very same buffering emits one
 * tbm_add_tuples() call per heap block.  Only the first block of the pair's
 * own container key has to be recomputed as the ckey changes.
 */
static bool
lion_tbm_pair_callback(uint32 ckey, uint16 lo, void *arg)
{
	LionTbmState *st = (LionTbmState *) arg;

	st->firstblk = lion_ckey_first_block(ckey);
	return lion_tbm_callback(lo, arg);
}

/*
 * Emit one item (container or sparse segment) into a TIDBitmap, one
 * tbm_add_tuples() call per heap block.  Returns the number of TIDs emitted.
 */
int64
lion_container_to_tbm(const LionContainer *c, TIDBitmap *tbm, bool recheck)
{
	LionTbmState st;

	st.tbm = tbm;
	st.firstblk = lion_ckey_first_block(c->ckey);
	st.curblk = InvalidBlockNumber;
	st.ntids = 0;
	st.total = 0;
	st.recheck = recheck;

	if (c->type == LION_CT_SPARSE)
		lion_sparse_iterate(c, lion_tbm_pair_callback, &st);
	else
		lion_container_iterate(c, lion_tbm_callback, &st);

	if (st.ntids > 0)
		tbm_add_tuples(tbm, st.tids, st.ntids, st.recheck);

	return st.total;
}

/*
 * Emit every item of an INLINE entry payload.  Those items are packed
 * without padding, so each one is copied into an aligned buffer first.
 */
static int64
lion_emit_inline(const char *payload, Size paylen, TIDBitmap *tbm, bool recheck)
{
	int64		ntids = 0;
	Size		off = 0;
	LionContainer *cbuf = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);

	while (lion_inline_fetch(payload, paylen, &off, cbuf) > 0)
		ntids += lion_container_to_tbm(cbuf, tbm, recheck);

	pfree(cbuf);
	return ntids;
}

IndexScanDesc
lionbeginscan(Relation r, int nkeys, int norderbys)
{
	IndexScanDesc scan;
	LionScanOpaque so;

	scan = RelationGetIndexScan(r, nkeys, norderbys);

	so = (LionScanOpaque) palloc0(sizeof(LionScanOpaqueData));
	so->ix = lion_get_index_state(r);
	so->cols = (LionScanCol *) palloc0(sizeof(LionScanCol) * so->ix->ncolumns);
	so->index = r;

	scan->opaque = so;
	scan->xs_itupdesc = RelationGetDescr(r);

	return scan;
}

/*
 * Close the plain scan's source, if one is open: every pin it holds goes
 * (DESIGN.md §29.7), then the memory.  The probe cache stays.
 */
static void
lion_scan_reset(LionScanOpaque so)
{
	if (so->src != NULL)
		lion_source_release(so->src);
	so->src = NULL;
	so->srcdone = false;
	so->nlo = 0;
	so->pos = 0;
	if (so->gtcxt != NULL)
		MemoryContextReset(so->gtcxt);
}

void
lionrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
		  ScanKey orderbys, int norderbys)
{
	LionScanOpaque so = (LionScanOpaque) scan->opaque;

	lion_scan_reset(so);

	if (scankey && scan->numberOfKeys > 0)
		memcpy(scan->keyData, scankey,
			   scan->numberOfKeys * sizeof(ScanKeyData));
}

void
lionendscan(IndexScanDesc scan)
{
	LionScanOpaque so = (LionScanOpaque) scan->opaque;

	if (so != NULL)
	{
		lion_scan_reset(so);
		if (so->gtcxt != NULL)
			MemoryContextDelete(so->gtcxt);
		if (so->lo != NULL)
			pfree(so->lo);
		if (so->cols != NULL)
			pfree(so->cols);
		pfree(so);
		scan->opaque = NULL;
	}
}

/*
 * Emit every container of a CHAIN entry, walking the chain one shared lock at
 * a time.  The page image (items and rightlink) is read atomically under the
 * lock, so a concurrent split - which only ever moves items rightwards onto a
 * brand new page - cannot make a reader miss or repeat a container.
 *
 * No bucket page may be held here: DESIGN.md §11 forbids taking a bucket lock
 * while a container page is pinned, and this function pins them.
 */
static int64
lion_emit_chain(Relation index, uint32 hash, BlockNumber head, TIDBitmap *tbm,
			   bool recheck)
{
	PGAlignedBlock *copy;
	BlockNumber blkno;
	Buffer		buf;
	int64		ntids = 0;

	if (!BlockNumberIsValid(head))
		return 0;

	/*
	 * DESIGN.md §22: `head` is the ROOT of the posting tree, which is the
	 * single leaf only while the set fits one page.  The leaves are still one
	 * rightlinked list in ckey order, so the walk below is unchanged once it
	 * has descended to the leftmost one - and the descent hands that leaf back
	 * LOCKED, which is what keeps a root push-down from slipping in between
	 * (the block would then be an internal page and this walk would find no
	 * containers on it at all).
	 */
	buf = lion_posting_search(index, NULL, hash, head, 0, BUFFER_LOCK_SHARE,
							  false);
	if (!BufferIsValid(buf))
		return 0;

	copy = (PGAlignedBlock *) palloc(sizeof(PGAlignedBlock));

	for (;;)
	{
		Page		page;
		Page		cpage = (Page) copy->data;
		BlockNumber cblkno = BufferGetBlockNumber(buf);
		OffsetNumber maxoff;
		OffsetNumber off;

		page = BufferGetPage(buf);
		memcpy(cpage, page, BLCKSZ);
		UnlockReleaseBuffer(buf);

		blkno = LionPageGetOpaque(cpage)->rightlink;

		maxoff = PageGetMaxOffsetNumber(cpage);
		/* each item checked before the container code reads it (§3) */
		for (off = FirstOffsetNumber; off <= maxoff; off++)
			ntids += lion_container_to_tbm(lion_page_item_fetch(index, cpage,
																 cblkno, off),
										   tbm, recheck);

		CHECK_FOR_INTERRUPTS();

		if (!BlockNumberIsValid(blkno))
			break;

		buf = ReadBuffer(index, blkno);
		LockBuffer(buf, BUFFER_LOCK_SHARE);

		/*
		 * DESIGN.md §18: a page that no longer claims this posting set means
		 * the set was freed after the entry was copied out, which can only
		 * happen once it held nothing visible to anyone.  The scan stops
		 * there; outside verify() this is never an error.  A leaf's rightlink
		 * always names another leaf, so a page at a level above zero is the
		 * same kind of accident and is treated the same way.
		 */
		if (!lion_page_owns_entry(BufferGetPage(buf), hash, head) ||
			!LionPageIsPostingLeaf(BufferGetPage(buf)))
		{
			UnlockReleaseBuffer(buf);
			break;
		}
	}

	pfree(copy);
	return ntids;
}

/*
 * Emit the posting set of the entry at (entrybuf, entryoff), a directory leaf
 * the caller holds SHARE-locked.  It is released here, before any container
 * page is touched (the reader side of the DESIGN.md §11 deadlock rule).
 */
static int64
lion_emit_entry(Relation index, Buffer entrybuf,
			   OffsetNumber entryoff, TIDBitmap *tbm, bool recheck)
{
	Page		page = BufferGetPage(entrybuf);
	ItemId		iid = PageGetItemId(page, entryoff);
	LionEntryTuple *entry = (LionEntryTuple *) PageGetItem(page, iid);
	char	   *payload = NULL;
	Size		paylen = 0;
	BlockNumber blkno = InvalidBlockNumber;
	uint32		hash = entry->hash;
	int64		ntids;

	if (entry->flags & LION_ENTRY_INLINE)
	{
		paylen = LION_ENTRY_PAYLOAD_LEN(entry, ItemIdGetLength(iid));
		if (paylen > 0)
		{
			payload = (char *) palloc(paylen);
			memcpy(payload, LionEntryGetPayload(entry), paylen);
		}
	}
	else
	{
		Assert(entry->flags & LION_ENTRY_CHAIN);
		blkno = entry->head;
	}

	UnlockReleaseBuffer(entrybuf);

	if (payload != NULL)
	{
		ntids = lion_emit_inline(payload, paylen, tbm, recheck);
		pfree(payload);
		return ntids;
	}

	/*
	 * Test hook: the entry has been copied and its bucket page released, so
	 * all this scan holds of the posting set is a head block - no pin, no
	 * lock, nothing that stops a VACUUM from deleting the entry and freeing
	 * the chain, or an insert from taking those pages back.  What keeps the
	 * walk below correct is the owner check in each page's special area
	 * (DESIGN.md §18); test/isolation/vacuum_chain_reuse.spec parks a reader
	 * here and does exactly that to it.  Compiles to nothing without
	 * --enable-injection-points.
	 */
	LION_INJECTION_POINT("lion-scan-chain-entered");

	return lion_emit_chain(index, hash, blkno, tbm, recheck);
}

/*
 * Emit the posting set of one search value on one key column.
 */
static int64
lion_emit_value(LionScanOpaque so, LionState *col,
			   ScanKey skey, Datum value, TIDBitmap *tbm)
{
	Relation	index = so->index;
	LionScanCol *sc = &so->cols[col->attno - 1];
	Buffer		entrybuf;
	OffsetNumber entryoff;

	if (!sc->probevalid || sc->subtype != skey->sk_subtype)
	{
		/* the probe's FmgrInfos live as long as the scan */
		MemoryContext oldcxt = MemoryContextSwitchTo(GetMemoryChunkContext(so));

		lion_probe_init(index, col, skey->sk_subtype, &sc->probe);
		MemoryContextSwitchTo(oldcxt);
		sc->subtype = skey->sk_subtype;
		sc->probevalid = true;
	}

	if (!lion_probe_find(index, col, &sc->probe, value, BUFFER_LOCK_SHARE,
						 &entrybuf, &entryoff))
	{
		if (BufferIsValid(entrybuf))
			UnlockReleaseBuffer(entrybuf);
		return 0;
	}

	return lion_emit_entry(index, entrybuf, entryoff, tbm, so->recheck);
}

/*
 * `col = ANY (array)` (DESIGN.md §15).  The elements are looked up one at a
 * time and their sets emitted into the same bitmap; NULL elements select
 * nothing, and repeated elements are harmless because a TIDBitmap is a set.
 */
static int64
lion_emit_array(LionScanOpaque so, LionState *col,
			   ScanKey skey, TIDBitmap *tbm)
{
	ArrayType  *arr = DatumGetArrayTypeP(skey->sk_argument);
	Oid			elemtype = ARR_ELEMTYPE(arr);
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	Datum	   *elems;
	bool	   *nulls;
	int			nelems;
	int64		ntids = 0;
	int			i;

	get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
	deconstruct_array(arr, elemtype, elmlen, elmbyval, elmalign,
					  &elems, &nulls, &nelems);

	for (i = 0; i < nelems; i++)
	{
		if (nulls[i])
			continue;			/* `x = NULL` is never true */
		ntids += lion_emit_value(so, col, skey, elems[i], tbm);
		CHECK_FOR_INTERRUPTS();
	}

	pfree(elems);
	pfree(nulls);
	if ((Pointer) arr != DatumGetPointer(skey->sk_argument))
		pfree(arr);

	return ntids;
}

/*
 * `col IS NULL` (DESIGN.md §14): that column's reserved NULL entry.
 */
static int64
lion_emit_null(Relation index, LionState *col, TIDBitmap *tbm, bool recheck)
{
	Buffer		entrybuf;
	OffsetNumber entryoff;

	if (!lion_find_null_entry(index, col, BUFFER_LOCK_SHARE,
							 &entrybuf, &entryoff))
	{
		if (BufferIsValid(entrybuf))
			UnlockReleaseBuffer(entrybuf);
		return 0;
	}

	return lion_emit_entry(index, entrybuf, entryoff, tbm, recheck);
}

/*
 * Every row indexed under one key column: the union of every entry of that
 * column but its NULL one, which means walking the column's whole run of
 * entries.  Correct, and expensive in proportion to the number of distinct
 * keys - the planner's cost estimate for such a scan is the whole index, so
 * it only happens when that is cheap or when nothing else can answer the
 * query.
 *
 * Two quals need it:
 *
 *	- `col IS NOT NULL` (DESIGN.md §14), which is exactly "every row whose
 *	  value is not NULL" and needs no recheck of its own;
 *	- the ALL fallback of a multi-key query (DESIGN.md §17): `tags @> '{}'`,
 *	  `<@`, a tsquery with NOT/phrase/prefix/weights.  Those ask for a
 *	  superset and the caller passes recheck = true.
 *
 * The reserved EMPTY entry is emitted like any other, which is what it exists
 * for: a row an opclass extracted no key from is under no key, and only a
 * walk like this one can find it.  The NULL entry is left out, because a NULL
 * value satisfies neither kind of qual (the multi-key operators are strict).
 *
 * The walk is the column's first leaf and then the right links (DESIGN.md §21
 * and §24), so for an ordered opclass the entries come out in key order, and
 * it stops at the first entry of the NEXT column.  Each leaf is copied into
 * backend-local memory and released before its entries are emitted, so no
 * directory page is held while container pages are pinned (DESIGN.md §11).
 * Working from the copy can miss an entry added after the copy was taken,
 * which is exactly as acceptable as it is for a single key: such an entry can
 * only hold TIDs that no snapshot older than this scan can see.  A concurrent
 * split moves entries only to a page further right, which this walk has not
 * passed yet, so nothing is seen twice either.
 */
/*
 * The leaf walk itself, as an iterator, shared by the bitmap scan (below) and
 * the plain scan's WALK (DESIGN.md §29.3): each directory leaf is copied into
 * backend-local memory and released - or, for a plain scan under a non-MVCC
 * snapshot, released but kept PINNED until the walk moves on (§29.5) - and
 * lion_walk_next() hands out the entries of the copy the walk selects, one at
 * a time.  An entry is valid until the next call: the next call may read the
 * next leaf into the same buffer.
 *
 * The entries selected are the column's (DESIGN.md §24), without its NULL
 * entry unless withnull, and - when ranges are given - those at least one of
 * the ranges selects (DESIGN.md §28).  Several ranges are the union a plain
 * scan needs for `op ANY (array)` on a column that cannot compare the
 * elements, walked ONCE rather than once per element, so that no entry comes
 * out twice; the walk ends when every range has ended.
 *
 * ONE range over a column with SUMMARY posting sets (DESIGN.md §32) is walked
 * the way a sum walks it instead (lion_entry_scan_begin_summed()): the keys of
 * the buckets at the range's ends one by one, and every bucket the range
 * covers whole as its summary, one entry of up to summary_tids rows.  They are
 * pairwise disjoint and together exactly the range's rows, as the keys are
 * (§32, "Readers"), so no caller can tell them apart - every one of them
 * takes an entry's posting set and nothing else of it - but a range over a
 * near-unique column is a few thousand entries instead of a million.  The
 * copies are the entry scan's batch, handed out with no pin, which is why a
 * walk that must keep its pins (§29.5) walks the keys: the only such walk is
 * a plain scan under a non-MVCC snapshot.  A walk with its NULL entry has no
 * range and never gets here.
 */
typedef struct LionLeafWalk
{
	Relation	index;
	LionState  *col;
	bool		withnull;
	LionRange  *ranges;			/* nranges of them, ORed; NULL: every entry */
	int			nranges;
	bool	   *ended;			/* [nranges]: no later entry is in range r */
	bool		keeppin;		/* keep the leaf the image came from pinned */
	Buffer		pinbuf;
	PGAlignedBlock *copy;
	bool		haspage;
	BlockNumber nextblk;
	OffsetNumber off;
	OffsetNumber maxoff;
	bool		done;
	LionEntryScan *sum;			/* the summed walk above, or NULL */
} LionLeafWalk;

static void
lion_walk_begin(LionLeafWalk *w, Relation index, LionState *col,
				bool withnull, LionRange *ranges, int nranges, bool keeppin)
{
	int			nlive = 0;
	int			i;

	memset(w, 0, sizeof(LionLeafWalk));
	w->index = index;
	w->col = col;
	w->withnull = withnull;
	w->ranges = ranges;
	w->nranges = (ranges != NULL) ? nranges : 0;
	w->keeppin = keeppin;
	w->pinbuf = InvalidBuffer;
	w->copy = (PGAlignedBlock *) palloc(sizeof(PGAlignedBlock));
	w->nextblk = InvalidBlockNumber;

	if (w->nranges > 0)
	{
		w->ended = (bool *) palloc(sizeof(bool) * w->nranges);
		for (i = 0; i < w->nranges; i++)
		{
			w->ended[i] = ranges[i].empty;
			if (!ranges[i].empty)
				nlive++;
		}
		if (nlive == 0)
		{
			w->done = true;
			return;
		}
	}

	/* One range over a summarized column: its summaries (above). */
	if (w->nranges == 1 && col->summarized && !withnull && !keeppin)
	{
		LionEntryScan *es = (LionEntryScan *) palloc(sizeof(LionEntryScan));

		if (lion_entry_scan_begin_summed(es, index, (AttrNumber) col->attno,
										 &ranges[0], LION_WALK_INSIDE))
		{
			w->sum = es;
			return;
		}
		pfree(es);
	}

	/*
	 * A range starts at the leaf its lower bound lives on (DESIGN.md §28);
	 * the entries of an earlier column and those below the bound that share
	 * the leaf are passed over below, like everything else the walk skips.
	 */
	w->nextblk = (w->nranges == 1) ? lion_range_first_leaf(index, &ranges[0]) :
		lion_dir_column_first(index, col, NULL);
}

static LionEntryTuple *
lion_walk_next(LionLeafWalk *w, Size *itemlen)
{
	Page		cpage = (Page) w->copy->data;

	if (w->sum != NULL)
	{
		LionEntryTuple *entry = NULL;

		if (!w->done)
			entry = lion_entry_scan_next_copy(w->sum, itemlen);
		if (entry == NULL)
			w->done = true;
		return entry;
	}

	for (;;)
	{
		if (w->done)
			return NULL;

		if (!w->haspage)
		{
			Buffer		buf;
			Page		page;

			if (!BlockNumberIsValid(w->nextblk))
			{
				w->done = true;
				return NULL;
			}

			/* The entries of the leaf before this one have all been used. */
			if (BufferIsValid(w->pinbuf))
			{
				ReleaseBuffer(w->pinbuf);
				w->pinbuf = InvalidBuffer;
			}

			buf = ReadBuffer(w->index, w->nextblk);
			lion_dir_pages_read++;
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buf);
			if (!LionPageIsLeaf(page))
			{
				UnlockReleaseBuffer(buf);
				elog(ERROR, "lion index: block %u is not a directory leaf",
					 w->nextblk);
			}
			memcpy(cpage, page, BLCKSZ);
			if (w->keeppin)
			{
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);
				w->pinbuf = buf;
			}
			else
				UnlockReleaseBuffer(buf);

			w->nextblk = LionPageGetOpaque(cpage)->rightlink;
			w->off = lion_page_first_data(cpage);
			w->maxoff = PageGetMaxOffsetNumber(cpage);
			w->haspage = true;
		}

		while (w->off <= w->maxoff)
		{
			ItemId		iid = PageGetItemId(cpage, w->off);
			LionEntryTuple *entry;

			w->off = OffsetNumberNext(w->off);
			if (!ItemIdIsUsed(iid))
				continue;
			entry = (LionEntryTuple *) PageGetItem(cpage, iid);

			/*
			 * Bounded to one key column (DESIGN.md §24), and to its values:
			 * the column's summary entries (§32) come after its last value.
			 */
			if (entry->attno < w->col->attno)
				continue;
			if (entry->attno > w->col->attno || LionEntryIsSummary(entry))
			{
				w->done = true;
				return NULL;
			}

			if (LionEntryIsNullKey(entry) && !w->withnull)
				continue;

			/* Only the entries a range selects, up to its end (§28). */
			if (w->nranges > 0)
			{
				bool		match = false;
				bool		allended = true;
				int			r;

				for (r = 0; r < w->nranges; r++)
				{
					int			t;

					if (w->ended[r])
						continue;
					t = lion_range_test(&w->ranges[r], entry);
					if (t == LION_RANGE_MATCH)
						match = true;
					else if (t == LION_RANGE_END)
						w->ended[r] = true;
					if (!w->ended[r])
						allended = false;
				}
				if (!match)
				{
					if (allended)
					{
						w->done = true;
						return NULL;
					}
					continue;
				}
			}

			*itemlen = ItemIdGetLength(iid);
			return entry;
		}

		w->haspage = false;
	}
}

static void
lion_walk_end(LionLeafWalk *w)
{
	if (BufferIsValid(w->pinbuf))
		ReleaseBuffer(w->pinbuf);
	w->pinbuf = InvalidBuffer;
	if (w->sum != NULL)
	{
		lion_entry_scan_end(w->sum);
		pfree(w->sum);
		w->sum = NULL;
	}
	w->done = true;
	w->haspage = false;
}

static int64
lion_emit_all_keys_ext(Relation index, LionState *col, TIDBitmap *tbm,
					  bool recheck, bool withnull, LionRange *range)
{
	LionLeafWalk w;
	LionEntryTuple *entry;
	Size		itemlen;
	int64		ntids = 0;

	lion_walk_begin(&w, index, col, withnull, range, 1, false);

	while ((entry = lion_walk_next(&w, &itemlen)) != NULL)
	{
		if ((entry->flags & LION_ENTRY_INLINE) != 0)
			ntids += lion_emit_inline(LionEntryGetPayload(entry),
									 LION_ENTRY_PAYLOAD_LEN(entry, itemlen),
									 tbm, recheck);
		else
			ntids += lion_emit_chain(index, entry->hash, entry->head,
									tbm, recheck);

		CHECK_FOR_INTERRUPTS();
	}

	lion_walk_end(&w);
	pfree(w.copy);
	return ntids;
}

static int64
lion_emit_all_keys(Relation index, LionState *col, TIDBitmap *tbm,
				  bool recheck)
{
	return lion_emit_all_keys_ext(index, col, tbm, recheck, false, NULL);
}

/*
 * Is this scan key one of the range comparisons of DESIGN.md §28?  Only a
 * SCALAR column has them: a multi-key class's strategies are §17's, and its
 * validator refuses 6 .. 9 anyway.
 */
static bool
lion_scankey_is_range(LionState *col, ScanKey skey)
{
	return !col->multikey &&
		(skey->sk_flags & (SK_SEARCHNULL | SK_SEARCHNOTNULL |
						   SK_SEARCHARRAY)) == 0 &&
		LION_STRAT_IS_RANGE(skey->sk_strategy);
}

/*
 * ... and is it `col < ANY (array)` or a sibling: an array key with a range
 * strategy on a scalar column, one walk to the widest element (below).  It
 * arrives as an array key, as a list does, but it is a walk and not a set
 * tree, so every caller that sorts quals into trees and walks asks this first.
 * A NULL array (SK_ISNULL) is still one: it selects nothing.
 */
static bool
lion_scankey_is_array_range(LionState *col, ScanKey skey)
{
	return !col->multikey &&
		(skey->sk_flags & (SK_SEARCHNULL | SK_SEARCHNOTNULL |
						   SK_SEARCHARRAY)) == SK_SEARCHARRAY &&
		LION_STRAT_IS_RANGE(skey->sk_strategy);
}

/*
 * Every `<`, `<=`, `>=` and `>` of the scan on one key column, as ONE bounded
 * walk of that column's entries (DESIGN.md §28): `BETWEEN a AND b` descends
 * to `a` once and stops past `b`, and needs no recheck of its own.
 *
 * The walk is lion_emit_all_keys()'s: each leaf is copied and let go before
 * its entries are emitted, an INLINE posting set from the copy and a posting
 * tree one page at a time, so nothing is pinned between two entries however
 * wide the range is.  No pin budget is drawn on - a bitmap scan needs no
 * visibility-map interlock, since the executor visits every TID it emits -
 * and the memory is the TIDBitmap's own, which goes lossy under work_mem
 * rather than growing.  On a column with summaries (DESIGN.md §32) the walk
 * reads the summary of every bucket the range covers whole instead of its
 * keys (lion_walk_begin()): the same TIDs, from far fewer entries.
 */
static int64
lion_emit_range(LionScanOpaque so, LionState *col,
			   TIDBitmap *tbm)
{
	Relation	index = so->index;
	LionRange	range;
	int64		ntids;
	int			i;

	lion_range_init(&range, index, (AttrNumber) col->attno);
	for (i = 0; i < so->nkeys; i++)
	{
		ScanKey		k = &so->keys[i];

		if (k->sk_attno != col->attno || !lion_scankey_is_range(col, k))
			continue;
		lion_range_add(&range, index, k->sk_strategy, k->sk_func.fn_oid,
					   k->sk_subtype, k->sk_argument,
					   (k->sk_flags & SK_ISNULL) != 0, k->sk_collation);
	}

	ntids = lion_emit_all_keys_ext(index, col, tbm, so->recheck, false,
								   &range);
	if (range.bounds != NULL)
		pfree(range.bounds);
	return ntids;
}

/*
 * `col < ANY (array)` and its siblings: amsearcharray hands the planner's
 * ScalarArrayOpExpr to us for every strategy of the family, so the range
 * comparisons of DESIGN.md §28 arrive here as arrays too.  (Only `ANY` is ever
 * an index qual; `ALL` is not.)
 *
 * `ANY` is the union of the elements' answers, and the union of `k < e` over
 * the elements is `k < max(e)` - the walk of the WIDEST bound, the largest
 * element for `<` and `<=` and the smallest for `>=` and `>` - so the scan
 * makes ONE walk, whatever the array holds.  Walking every element's range
 * into the same bitmap gives the same answer but emits the overlap again for
 * each element: two hundred near-equal bounds over a million-row unique
 * column emitted ten million TIDs and took 1.4 s against 20 ms.  The widest
 * element is found with the comparison §21's probe resolution gives values of
 * the array's type among themselves (the probe's sortproc: the column's own
 * proc 4, or the family's proc 4 for that type); a column that has none - an
 * unordered one - walks element by element as before.  A NULL element makes
 * no row true (a strict comparison with NULL is NULL, and `ANY` of NULLs and
 * falses is not true), so it is skipped; an array of nothing else, or an
 * empty one, selects nothing.
 */
static int64
lion_emit_array_range(LionScanOpaque so, LionState *col,
					 ScanKey skey, TIDBitmap *tbm)
{
	Relation	index = so->index;
	LionScanCol *sc = &so->cols[col->attno - 1];
	ArrayType  *arr = DatumGetArrayTypeP(skey->sk_argument);
	Oid			elemtype = ARR_ELEMTYPE(arr);
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	Datum	   *elems;
	bool	   *nulls;
	int			nelems;
	int64		ntids = 0;
	int			widest = -1;
	int			i;

	get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
	deconstruct_array(arr, elemtype, elmlen, elmbyval, elmalign,
					  &elems, &nulls, &nelems);

	if (!sc->probevalid || sc->subtype != skey->sk_subtype)
	{
		/* the probe's FmgrInfos live as long as the scan */
		MemoryContext oldcxt = MemoryContextSwitchTo(GetMemoryChunkContext(so));

		lion_probe_init(index, col, skey->sk_subtype, &sc->probe);
		MemoryContextSwitchTo(oldcxt);
		sc->subtype = skey->sk_subtype;
		sc->probevalid = true;
	}

	for (i = 0; i < nelems; i++)
	{
		LionRange	range;

		if (nulls[i])
			continue;			/* a strict comparison with NULL is not true */

		if (sc->probe.hassort)
		{
			int32		c;

			if (widest < 0)
			{
				widest = i;
				continue;
			}
			c = DatumGetInt32(FunctionCall2Coll(&sc->probe.sortproc,
												col->collation,
												elems[i], elems[widest]));
			if (LION_STRAT_IS_LOWER(skey->sk_strategy) ? (c < 0) : (c > 0))
				widest = i;
			continue;
		}

		/* No order among the elements: one walk each. */
		lion_range_init(&range, index, (AttrNumber) col->attno);
		lion_range_add(&range, index, skey->sk_strategy, skey->sk_func.fn_oid,
					   skey->sk_subtype, elems[i], false, skey->sk_collation);
		ntids += lion_emit_all_keys_ext(index, col, tbm, so->recheck, false,
									   &range);
		pfree(range.bounds);
		CHECK_FOR_INTERRUPTS();
	}

	if (widest >= 0)
	{
		LionRange	range;

		lion_range_init(&range, index, (AttrNumber) col->attno);
		lion_range_add(&range, index, skey->sk_strategy, skey->sk_func.fn_oid,
					   skey->sk_subtype, elems[widest], false,
					   skey->sk_collation);
		ntids = lion_emit_all_keys_ext(index, col, tbm, so->recheck, false,
									  &range);
		pfree(range.bounds);
	}

	pfree(elems);
	pfree(nulls);
	if ((Pointer) arr != DatumGetPointer(skey->sk_argument))
		pfree(arr);

	return ntids;
}

/* ---------------------------------------------------------------------
 * Multi-key opclasses (DESIGN.md §17)
 * --------------------------------------------------------------------- */

/* State threaded through lion_sets_iterate() by lion_emit_query(). */
typedef struct LionQueryEmitState
{
	TIDBitmap  *tbm;
	bool		recheck;
	int64		ntids;
} LionQueryEmitState;

static bool
lion_query_emit_cb(const LionContainer *c, void *arg)
{
	LionQueryEmitState *es = (LionQueryEmitState *) arg;

	es->ntids += lion_container_to_tbm(c, es->tbm, es->recheck);
	return true;
}

/*
 * Answer one multi-key query (`tags @> '{a,b}'`, `tsv @@ 'a & b'`, ...).
 *
 * The opclass's extractQuery function says which keys the query needs and how
 * exactly they answer it; lion_extract_query() turns that into one of three
 * shapes (DESIGN.md §17):
 *
 *	NONE	nothing can match - `tags && '{}'`, an empty tsquery;
 *	KEYS	the rows are exactly the ones a boolean tree over the keys
 *			selects, so the posting sets are located and combined and the
 *			TIDs go out without a recheck of their own;
 *	ALL		the index cannot decide, so every indexed row goes out with
 *			recheck set and the bitmap heap scan re-applies the operator.
 *
 * The posting sets are ALL located before any of them is walked, which keeps
 * the reader side of the DESIGN.md §11 deadlock rule: every directory-page
 * lock this function takes is taken before the first container page is
 * pinned.
 */
static int64
lion_emit_query(Relation index, LionState *col, StrategyNumber strategy,
			   Datum query, TIDBitmap *tbm, bool recheck)
{
	LionQuery	q;
	LionPostingSet *sets;
	LionQueryEmitState es;
	MemoryContext cxt;
	MemoryContext oldcxt;
	int			i;

	/*
	 * The extracted keys, the tree and the located payloads all live and die
	 * with one query.  A scan of `col op ANY (...)` calls this once per
	 * element, and a tsquery can carry hundreds of lexemes, so they get a
	 * context of their own rather than the executor's.
	 */
	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"lion index multikey scan",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	lion_extract_query(col, query, strategy, &q);

	if (q.mode != LION_QMODE_KEYS)
	{
		int64		ntids = 0;

		MemoryContextSwitchTo(oldcxt);
		if (q.mode == LION_QMODE_ALL)
			ntids = lion_emit_all_keys(index, col, tbm, true);
		MemoryContextDelete(cxt);
		return ntids;
	}

	Assert(q.nkeys > 0 && q.tree != NULL);

	/*
	 * A query may carry any number of keys, and every INLINE set would keep
	 * a pin on its leaf until the end (2026-09-23 review).  Those pins are
	 * the count's visibility-map interlock (DESIGN.md §9) and protect nothing
	 * here - every TID goes to the bitmap heap scan, which visits the heap
	 * for it - so each one is dropped as soon as its payload is copied.
	 */
	sets = (LionPostingSet *) palloc0(sizeof(LionPostingSet) * q.nkeys);
	for (i = 0; i < q.nkeys; i++)
	{
		(void) lion_posting_set_lookup_col(index, (AttrNumber) col->attno,
										  q.keys[i], InvalidOid, &sets[i]);
		lion_posting_set_unpin(&sets[i]);
		CHECK_FOR_INTERRUPTS();
	}

	es.tbm = tbm;
	es.recheck = recheck;
	es.ntids = 0;

	(void) lion_sets_iterate(q.nkeys, sets, q.tree, lion_query_emit_cb, &es);

	/* Every pin goes before the memory the sets live in does. */
	for (i = 0; i < q.nkeys; i++)
		lion_posting_set_release(&sets[i]);

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);

	return es.ntids;
}

/*
 * The multi-key entry point of the scan.
 *
 * `col op ANY (const array)` reaches a multi-key index too (amsearcharray is
 * on for the scalar classes of DESIGN.md §15), and there the array holds one
 * QUERY per element - an array of arrays, or of tsqueries.  Each is answered
 * separately and the results go into the same bitmap, which is their union
 * and exactly what `ANY` means; a TIDBitmap is a set, so overlapping elements
 * cost nothing but the second lookup.
 */
static int64
lion_emit_multikey(LionScanOpaque so, LionState *col,
				  ScanKey skey, TIDBitmap *tbm)
{
	Relation	index = so->index;

	/*
	 * A strategy this file does not know is an ERROR and not an empty result:
	 * the opclass would be claiming an operator the scan cannot answer, and
	 * answering "no rows" would be a wrong answer rather than a missing
	 * optimisation.  lion_gin_strategy() raises it.
	 */
	if ((skey->sk_flags & SK_SEARCHARRAY) != 0)
	{
		ArrayType  *arr = DatumGetArrayTypeP(skey->sk_argument);
		Oid			elemtype = ARR_ELEMTYPE(arr);
		int16		elmlen;
		bool		elmbyval;
		char		elmalign;
		Datum	   *elems;
		bool	   *nulls;
		int			nelems;
		int64		ntids = 0;
		int			i;

		get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
		deconstruct_array(arr, elemtype, elmlen, elmbyval, elmalign,
						  &elems, &nulls, &nelems);

		for (i = 0; i < nelems; i++)
		{
			if (nulls[i])
				continue;		/* a strict operator with a NULL is not true */
			ntids += lion_emit_query(index, col, skey->sk_strategy,
									elems[i], tbm, so->recheck);
			CHECK_FOR_INTERRUPTS();
		}

		pfree(elems);
		pfree(nulls);
		if ((Pointer) arr != DatumGetPointer(skey->sk_argument))
			pfree(arr);

		return ntids;
	}

	return lion_emit_query(index, col, skey->sk_strategy, skey->sk_argument,
						  tbm, so->recheck);
}

/* ---------------------------------------------------------------------
 * Several quals at once, of several key columns (DESIGN.md §24) or of one
 *
 * Each qual becomes a boolean TREE over posting sets, all of them located
 * into one array, and the trees are ANDed together.  That is the very shape
 * a multi-key query already produces (§17), so the evaluator of lion_count.c
 * answers both without knowing which is which.
 *
 * Every set is located BEFORE the merge starts, which is the reader side of
 * the §11 rule: the directory locks are all taken and released before the
 * first container page is pinned.
 * --------------------------------------------------------------------- */

typedef struct LionScanSets
{
	LionPostingSet *sets;
	int			nsets;
	int			maxsets;
} LionScanSets;

static int
lion_sets_reserve(LionScanSets *acc, int n)
{
	int			first = acc->nsets;

	if (acc->nsets + n > acc->maxsets)
	{
		acc->maxsets = Max(acc->maxsets * 2, acc->nsets + n);
		acc->sets = (LionPostingSet *)
			repalloc(acc->sets, sizeof(LionPostingSet) * acc->maxsets);
	}
	memset(&acc->sets[first], 0, sizeof(LionPostingSet) * n);
	acc->nsets += n;

	return first;
}

/* A leaf naming the set at absolute position keyno. */
static LionKeyNode *
lion_scan_leaf(int keyno)
{
	LionKeyNode *n = (LionKeyNode *) palloc0(sizeof(LionKeyNode));

	n->kind = LION_KN_KEY;
	n->keyno = keyno;
	return n;
}

static LionKeyNode *
lion_scan_op(LionKeyNodeKind kind, LionKeyNode **args, int nargs)
{
	LionKeyNode *n;

	if (nargs == 1)
		return args[0];

	n = (LionKeyNode *) palloc0(sizeof(LionKeyNode));
	n->kind = kind;
	n->nargs = nargs;
	n->args = args;
	return n;
}

/*
 * Renumber a tree lion_extract_query() built over keys 0 .. n-1 so that its
 * leaves name sets at base .. base + n - 1 of the shared array.
 */
static void
lion_scan_shift(LionKeyNode *node, int base)
{
	int			i;

	if (node == NULL)
		return;
	if (node->kind == LION_KN_KEY)
	{
		node->keyno += base;
		return;
	}
	for (i = 0; i < node->nargs; i++)
		lion_scan_shift(node->args[i], base);
}

/*
 * The set tree of ONE column's scan key, with its sets appended to *acc.
 *
 * Returns NULL and leaves *ok false when the qual cannot be expressed as a
 * tree over posting sets at all (`IS NOT NULL`, a multi-key query that needs
 * every row): the caller then drops the qual and rechecks.  *nomatch means
 * the qual selects nothing whatsoever, which settles the entire scan.
 */
static LionKeyNode *
lion_scan_col_tree(Relation index, LionState *col,
				  ScanKey skey, LionScanSets *acc, bool *ok, bool *nomatch)
{
	AttrNumber	attno = (AttrNumber) col->attno;

	*ok = true;
	*nomatch = false;

	/* The null tests carry no strategy number and must be tested first. */
	if ((skey->sk_flags & SK_SEARCHNULL) != 0)
	{
		int			base = lion_sets_reserve(acc, 1);

		if (!lion_posting_set_lookup_null_col(index, attno, &acc->sets[base]))
			*nomatch = true;
		return lion_scan_leaf(base);
	}
	if ((skey->sk_flags & SK_SEARCHNOTNULL) != 0)
	{
		/* The complement of a set is not a set the evaluator can build. */
		*ok = false;
		return NULL;
	}

	/* `col = NULL` (or a NULL array) is never true. */
	if ((skey->sk_flags & SK_ISNULL) != 0)
	{
		*nomatch = true;
		return NULL;
	}

	if (col->multikey)
	{
		Datum	   *queries;
		bool	   *qnulls = NULL;
		int			nqueries = 1;
		LionKeyNode **args;
		int			nargs = 0;
		int			i;

		if ((skey->sk_flags & SK_SEARCHARRAY) != 0)
		{
			ArrayType  *arr = DatumGetArrayTypeP(skey->sk_argument);
			Oid			elemtype = ARR_ELEMTYPE(arr);
			int16		elmlen;
			bool		elmbyval;
			char		elmalign;

			get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
			deconstruct_array(arr, elemtype, elmlen, elmbyval, elmalign,
							  &queries, &qnulls, &nqueries);
		}
		else
		{
			queries = (Datum *) palloc(sizeof(Datum));
			queries[0] = skey->sk_argument;
		}

		args = (LionKeyNode **) palloc(sizeof(LionKeyNode *) *
									   Max(nqueries, 1));

		for (i = 0; i < nqueries; i++)
		{
			LionQuery	q;
			int			base;
			int			k;

			if (qnulls != NULL && qnulls[i])
				continue;		/* a strict operator with a NULL is not true */

			lion_extract_query(col, queries[i], skey->sk_strategy, &q);

			if (q.mode == LION_QMODE_NONE)
				continue;		/* this element selects nothing */
			if (q.mode != LION_QMODE_KEYS)
			{
				*ok = false;	/* needs every indexed row */
				return NULL;
			}

			/* No pins to keep: see lion_emit_query(). */
			base = lion_sets_reserve(acc, q.nkeys);
			for (k = 0; k < q.nkeys; k++)
			{
				(void) lion_posting_set_lookup_col(index, attno, q.keys[k],
												  InvalidOid,
												  &acc->sets[base + k]);
				lion_posting_set_unpin(&acc->sets[base + k]);
				CHECK_FOR_INTERRUPTS();
			}
			lion_scan_shift(q.tree, base);
			args[nargs++] = q.tree;
		}

		if (nargs == 0)
		{
			*nomatch = true;	/* every element selects nothing */
			return NULL;
		}

		/* `op ANY (array)` is the UNION of the elements' answers. */
		return lion_scan_op(LION_KN_OR, args, nargs);
	}

	if (skey->sk_strategy != LION_STRAT_EQUAL)
	{
		*ok = false;
		return NULL;
	}

	if ((skey->sk_flags & SK_SEARCHARRAY) != 0)
	{
		ArrayType  *arr = DatumGetArrayTypeP(skey->sk_argument);
		Oid			elemtype = ARR_ELEMTYPE(arr);
		int16		elmlen;
		bool		elmbyval;
		char		elmalign;
		Datum	   *elems;
		bool	   *nulls;
		int			nelems;
		int			nsets;
		int			nfound;
		int			base;
		LionKeyNode **args;
		int			i;

		get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
		deconstruct_array(arr, elemtype, elmlen, elmbyval, elmalign,
						  &elems, &nulls, &nelems);

		if (nelems == 0)
		{
			*nomatch = true;
			return NULL;
		}

		/* The lookup keeps its pins within a budget (DESIGN.md §15). */
		base = lion_sets_reserve(acc, nelems);
		nsets = lion_posting_set_lookup_many_col(index, attno, elemtype,
												nelems, elems, nulls,
												&acc->sets[base], &nfound);
		/* The lookup packs what it located at the front; drop the rest. */
		acc->nsets = base + nsets;
		if (nfound == 0)
		{
			*nomatch = true;
			return NULL;
		}

		args = (LionKeyNode **) palloc(sizeof(LionKeyNode *) * Max(nsets, 1));
		for (i = 0; i < nsets; i++)
			args[i] = lion_scan_leaf(base + i);

		return lion_scan_op(LION_KN_OR, args, nsets);
	}

	{
		int			base = lion_sets_reserve(acc, 1);

		if (!lion_posting_set_lookup_col(index, attno, skey->sk_argument,
										skey->sk_subtype, &acc->sets[base]))
			*nomatch = true;
		return lion_scan_leaf(base);
	}
}

/*
 * The posting sets ONE scan key selects, located as a scan locates them
 * (lion_scan_col_tree()), and the tree that combines them, for a caller
 * outside a scan: the planner's intersection probe (DESIGN.md §29.11,
 * "Correlated sets").  False when the key is not a tree over sets (`IS NOT
 * NULL`, a range, a multi-key query that needs every row); *nomatch when it
 * selects nothing at all.  The sets are allocated in the current memory
 * context and the caller releases them (lion_posting_set_release()); a set
 * may keep its INLINE leaf's pin until then.
 */
bool
lion_scankey_sets(Relation index, ScanKey skey, int *nsets,
				  LionPostingSet **sets, LionKeyNode **tree, bool *nomatch)
{
	LionState  *col = lion_index_column_state(index, skey->sk_attno);
	LionScanSets acc;
	LionKeyNode *node;
	bool		ok;

	acc.maxsets = 4;
	acc.nsets = 0;
	acc.sets = (LionPostingSet *) palloc(sizeof(LionPostingSet) * acc.maxsets);

	node = lion_scan_col_tree(index, col, skey, &acc, &ok, nomatch);

	*nsets = acc.nsets;
	*sets = acc.sets;
	*tree = ok ? node : NULL;
	return ok || *nomatch;
}

/*
 * Answer a scan of SEVERAL set quals, of one key column or of several:
 * intersect their set trees (DESIGN.md §24).  Any qual that cannot be
 * expressed is dropped and *recheck is set, which is correct because the
 * bitmap heap scan re-applies the original quals.  Returns -1 when NOTHING
 * could be expressed, so the caller falls back to a single-qual answer.
 */
static int64
lion_emit_columns(LionScanOpaque so, ScanKey *keys,
				 int nkeys, TIDBitmap *tbm)
{
	LionScanSets acc;
	LionKeyNode **args;
	LionKeyNode *tree;
	LionQueryEmitState es;
	MemoryContext cxt;
	MemoryContext oldcxt;
	int			nargs = 0;
	int			i;
	int			j;

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"lion index multicolumn scan",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	acc.maxsets = 8;
	acc.nsets = 0;
	acc.sets = (LionPostingSet *) palloc0(sizeof(LionPostingSet) * acc.maxsets);
	args = (LionKeyNode **) palloc(sizeof(LionKeyNode *) * nkeys);

	for (i = 0; i < nkeys; i++)
	{
		LionState  *col = lion_column(so->ix, keys[i]->sk_attno);
		LionKeyNode *node;
		bool		ok;
		bool		nomatch;

		node = lion_scan_col_tree(so->index, col, keys[i], &acc, &ok, &nomatch);

		if (nomatch)
		{
			/* One column selects nothing, so the intersection does too. */
			for (j = 0; j < acc.nsets; j++)
				lion_posting_set_release(&acc.sets[j]);
			MemoryContextSwitchTo(oldcxt);
			MemoryContextDelete(cxt);
			return 0;
		}
		if (!ok || node == NULL)
		{
			so->recheck = true;	/* the heap scan re-applies this qual */
			continue;
		}
		args[nargs++] = node;
	}

	if (nargs == 0)
	{
		for (i = 0; i < acc.nsets; i++)
			lion_posting_set_release(&acc.sets[i]);
		MemoryContextSwitchTo(oldcxt);
		MemoryContextDelete(cxt);
		return -1;
	}

	tree = lion_scan_op(LION_KN_AND, args, nargs);

	/*
	 * Every set is located; the leaves the INLINE ones pinned protect
	 * nothing here (the bitmap heap scan visits every TID), and a list's could
	 * be up to the whole list pin budget (DESIGN.md §15).  The walk below
	 * keeps no pin either.
	 */
	for (i = 0; i < acc.nsets; i++)
		lion_posting_set_unpin(&acc.sets[i]);

	es.tbm = tbm;
	es.recheck = so->recheck;
	es.ntids = 0;

	(void) lion_sets_iterate(acc.nsets, acc.sets, tree, lion_query_emit_cb,
							 &es);

	for (i = 0; i < acc.nsets; i++)
		lion_posting_set_release(&acc.sets[i]);

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);

	return es.ntids;
}

/* ---------------------------------------------------------------------
 * A range beside other key columns (DESIGN.md §28, "Bitmap scans")
 * --------------------------------------------------------------------- */

/* One range column of an intersection: the ranges its walk tests. */
typedef struct LionScanRangeCol
{
	AttrNumber	attno;
	LionRange  *ranges;
	int			nranges;
} LionScanRangeCol;

/*
 * A WINDOW of the other columns' answer: its next containers, in ascending
 * container key, copied out of their stream.
 *
 * The walk of the LAST range column ANDs every container of every entry it
 * hands out with the window's container at that key, and adds what is left to
 * the caller's bitmap at once: a bitmap is a set, so it does not matter which
 * entry a TID came from.  A range column before the last has another range to
 * be ANDed with afterwards, so what its walk finds is ORed into one bitset
 * image per window container instead (hit[]), and the window's containers
 * are cut down to it (lion_rwin_apply()) before the next column's walk.
 */
typedef struct LionRangeWindow
{
	int			n;				/* containers in the window */
	int			max;			/* room for, in the three arrays */
	uint32	   *keys;			/* [max] their container keys, ascending */
	LionContainer **f;			/* [max] the containers, in cxt */
	uint64	  **hit;			/* [max] a walk's images, NULL until used */
	bool		last;			/* the walk is the last range column's */
	int			p;				/* the slot the current entry has reached */
	TIDBitmap  *tbm;
	bool		recheck;
	int64		ntids;			/* TIDs added from containers */
	LionTbmState tst;			/* ... and from sparse segments' pairs */
	LionContainer *tmp;			/* LION_CONTAINER_MAX_SIZE bytes each */
	LionContainer *ibuf;
	MemoryContext cxt;			/* the window's containers and images */
	MemoryContext entrycxt;		/* one posting tree's stream */
} LionRangeWindow;

/*
 * Move the window's position forward to the first container whose key is not
 * below ckey, and say which slot holds ckey itself (-1 for none).  The items
 * of one entry come in ascending container key, so the position never moves
 * back within an entry, and a binary search from it finds the next one in a
 * window of any size.  False once ckey is past the window's last key, which
 * ends the entry.
 */
static inline bool
lion_rwin_slot(LionRangeWindow *w, uint32 ckey, int *slot)
{
	*slot = -1;
	if (ckey > w->keys[w->n - 1])
		return false;
	if (w->keys[w->p] < ckey)
	{
		int			lo = w->p + 1;
		int			hi = w->n - 1;	/* keys[hi] >= ckey */

		while (lo < hi)
		{
			int			mid = lo + (hi - lo) / 2;

			if (w->keys[mid] < ckey)
				lo = mid + 1;
			else
				hi = mid;
		}
		w->p = lo;
	}
	if (w->keys[w->p] == ckey)
		*slot = w->p;
	return true;
}

static inline uint64 *
lion_rwin_image(LionRangeWindow *w, int slot)
{
	if (w->hit[slot] == NULL)
		w->hit[slot] = (uint64 *) MemoryContextAllocZero(w->cxt,
														 LION_BITSET_BYTES);
	return w->hit[slot];
}

static bool
lion_rwin_pair_cb(uint32 ckey, uint16 lo, void *arg)
{
	LionRangeWindow *w = (LionRangeWindow *) arg;
	int			slot;

	if (!lion_rwin_slot(w, ckey, &slot))
		return false;
	if (slot < 0)
		return true;
	if (!w->last)
		lion_rwin_image(w, slot)[lo >> 6] |= UINT64CONST(1) << (lo & 63);
	else if (lion_container_contains(w->f[slot], lo))
		(void) lion_tbm_pair_callback(ckey, lo, &w->tst);
	return true;
}

/*
 * One item of a walked entry against the window: a container, or a sparse
 * segment of an INLINE payload (DESIGN.md §13).  Anything at a key the window
 * has no container at is none of its business.  Returns false once the item
 * is past the window, which ends the entry.
 */
static bool
lion_rwin_item(LionRangeWindow *w, const LionContainer *c)
{
	int			slot;

	/* A segment's header carries its first container key (§13). */
	if (c->ckey > w->keys[w->n - 1])
		return false;
	if (c->type == LION_CT_SPARSE)
	{
		lion_sparse_iterate(c, lion_rwin_pair_cb, w);
		if (w->tst.ntids > 0)
		{
			tbm_add_tuples(w->tbm, w->tst.tids, w->tst.ntids, w->recheck);
			w->tst.ntids = 0;
		}
		return true;			/* the next item says whether it is past */
	}
	if (!lion_rwin_slot(w, c->ckey, &slot))
		return false;
	if (slot < 0)
		return true;
	if (!w->last)
		lion_container_or_into_bitset(c, lion_rwin_image(w, slot));
	else if (lion_container_and(c, w->f[slot], w->tmp) > 0)
		w->ntids += lion_container_to_tbm(w->tmp, w->tbm, w->recheck);
	return true;
}

/*
 * One walked entry against the window: an INLINE one straight from the walk's
 * copy, item by item, and a posting tree sought to the window's first key and
 * read up to its last - as the plain scan's WINDOW reads its entries
 * (lion_source_window()).
 */
static void
lion_rwin_entry(LionRangeWindow *w, Relation index, LionEntryTuple *entry,
				Size itemlen)
{
	w->p = 0;
	if ((entry->flags & LION_ENTRY_INLINE) != 0)
	{
		const char *payload = LionEntryGetPayload(entry);
		Size		paylen = LION_ENTRY_PAYLOAD_LEN(entry, itemlen);
		Size		off = 0;

		while (lion_inline_fetch(payload, paylen, &off, w->ibuf) > 0)
		{
			if (!lion_rwin_item(w, w->ibuf))
				break;
		}
	}
	else
	{
		LionPostingSet ps;
		LionSetStream *st;
		const LionContainer *c;
		MemoryContext oldcxt = MemoryContextSwitchTo(w->entrycxt);

		lion_source_entry_set(index, entry, itemlen, w->entrycxt, &ps);
		st = lion_stream_begin(1, &ps, NULL, false);
		lion_stream_seek(st, w->keys[0]);
		while ((c = lion_stream_next(st)) != NULL)
		{
			if (!lion_rwin_item(w, c))
				break;
		}
		lion_stream_end(st);
		MemoryContextSwitchTo(oldcxt);
		MemoryContextReset(w->entrycxt);
	}
}

/*
 * One walk of a range column for the window.  lion_walk_begin() reads the
 * column's summaries when it has them (DESIGN.md §32), so a range that covers
 * whole buckets is a handful of entries, however many keys it holds.
 */
static void
lion_rwin_walk(LionRangeWindow *w, LionScanOpaque so, LionScanRangeCol *rc)
{
	LionLeafWalk lw;
	LionEntryTuple *entry;
	Size		itemlen;

	lion_walk_begin(&lw, so->index, lion_column(so->ix, rc->attno), false,
					rc->ranges, rc->nranges, false);
	while ((entry = lion_walk_next(&lw, &itemlen)) != NULL)
	{
		lion_rwin_entry(w, so->index, entry, itemlen);
		CHECK_FOR_INTERRUPTS();
	}
	lion_walk_end(&lw);
	pfree(lw.copy);
}

/*
 * After the walk of a range column that is not the last: each container of
 * the window keeps only what the walk found at its key, and one left with
 * nothing leaves the window.
 */
static void
lion_rwin_apply(LionRangeWindow *w)
{
	int			i;
	int			k = 0;

	for (i = 0; i < w->n; i++)
	{
		uint64	   *img = w->hit[i];
		LionContainer *f = w->f[i];
		Size		size;

		w->hit[i] = NULL;
		if (img == NULL)
		{
			pfree(f);
			continue;
		}
		lion_bits_to_container(img, w->keys[i], w->ibuf);
		pfree(img);
		if (lion_container_and(w->ibuf, f, w->tmp) == 0)
		{
			pfree(f);
			continue;
		}
		pfree(f);
		size = lion_container_size(w->tmp);
		w->f[k] = (LionContainer *) MemoryContextAlloc(w->cxt, size);
		memcpy(w->f[k], w->tmp, size);
		w->keys[k] = w->keys[i];
		k++;
	}
	w->n = k;
}

/*
 * The next window: the other columns' next containers until they take the
 * window's memory - their own size, and a bitset image's more each when a
 * walk will fill images - and at least one.  False when the stream has none
 * left.
 */
static bool
lion_rwin_fill(LionRangeWindow *w, LionSetStream *st, bool *done, Size budget,
			   bool images)
{
	Size		held = 0;

	MemoryContextReset(w->cxt);
	w->n = 0;
	while (!*done && held < budget)
	{
		const LionContainer *c = lion_stream_next(st);
		Size		size;

		if (c == NULL)
		{
			*done = true;
			break;
		}
		if (c->cardinality == 0)
			continue;
		if (w->n >= w->max)
		{
			w->max *= 2;
			w->keys = (uint32 *) repalloc(w->keys, sizeof(uint32) * w->max);
			w->f = (LionContainer **)
				repalloc(w->f, sizeof(LionContainer *) * w->max);
			w->hit = (uint64 **) repalloc(w->hit, sizeof(uint64 *) * w->max);
		}
		size = lion_container_size(c);
		w->f[w->n] = (LionContainer *) MemoryContextAlloc(w->cxt, size);
		memcpy(w->f[w->n], c, size);
		w->keys[w->n] = c->ckey;
		w->hit[w->n] = NULL;
		held += GetMemoryChunkSpace(w->f[w->n]) + sizeof(uint32) +
			sizeof(LionContainer *) + sizeof(uint64 *);
		if (images)
			held += LION_BITSET_BYTES;
		w->n++;
	}
	return w->n > 0;
}

/*
 * Answer a scan whose keys span several key columns when at least one of them
 * is answered by a RANGE (DESIGN.md §28): the other columns' set trees ANDed
 * into ONE stream of containers in ascending container key, and each range
 * column's walk ANDed with it a container at a time - exactly - into the
 * caller's bitmap, which a BitmapOr above may be sharing with its other arms.
 *
 * The stream is taken a WINDOW at a time, as much of it as the window's
 * memory holds (lion_scan_window_bytes(): work_mem, and never less than
 * pg_lion.scan_window_floor), and every range column is walked once per
 * window, each entry sought to the window and read up to its end.  A
 * selective answer of the other columns is one window, and each range one
 * walk; a range over a summarized column (§32) walks the summaries of the
 * buckets it covers rather than their keys, so a wide one is short.  When
 * nothing beside the ranges could be expressed, the first range column,
 * collected into a set of its own (lion_range_collect(), summaries and all),
 * stands for the other columns, and a range left on its own is simply walked
 * into the bitmap, rechecked.
 *
 * rangekeys[] names each range column by its chosen key: a plain range key,
 * whose column's range keys are then walked together, or `col < ANY (array)`,
 * one walk to the widest element.
 *
 * (Deviation from the version before it, which walked each range into a
 * TIDBitmap of its own, of work_mem, and intersected the bitmaps - core's
 * BitmapAnd inside one scan.  A range over a large heap does not fit in
 * work_mem when its rows are spread over the heap: its bitmap went LOSSY, and
 * tbm_intersect() keeps every exact page of the other side that falls on a
 * lossy page of it, only marked for recheck.  A time window over rows stored
 * in no time order has a row on nearly every page, so the range removed
 * almost nothing and the heap recheck threw most of the rows away - with the
 * range walked key by key into its bitmap all the same, and the scan
 * reporting the smaller input's count as its own, which looked as if the
 * range had not been answered at all (a benchmark, 2026-09-28).
 * test/sql/rangebitmap.sql shows no row removed by the recheck now.)
 */
static int64
lion_emit_intersect(LionScanOpaque so, ScanKey *keys,
				   int nkeys, ScanKey *rangekeys, int nrange,
				   TIDBitmap *tbm)
{
	Relation	index = so->index;
	MemoryContext cxt;
	MemoryContext oldcxt;
	LionScanSets acc;
	LionKeyNode **args;
	LionScanRangeCol *rc;
	LionRangeWindow w;
	LionSetStream *st;
	Size		budget = (Size) lion_scan_window_bytes();
	bool		done = false;
	bool		nomatch = false;
	int			nargs = 0;
	int			first = 0;
	int64		ntids;
	int			i;
	int			r;

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"lion index range intersection",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	acc.maxsets = 8;
	acc.nsets = 0;
	acc.sets = (LionPostingSet *) palloc0(sizeof(LionPostingSet) * acc.maxsets);
	args = (LionKeyNode **) palloc(sizeof(LionKeyNode *) * (nkeys + 1));

	/*
	 * Each range column's ranges.  One that can hold nothing - a NULL bound,
	 * an empty array or one of NULLs - settles the whole scan.
	 */
	rc = (LionScanRangeCol *) palloc0(sizeof(LionScanRangeCol) * nrange);
	for (i = 0; i < nrange && !nomatch; i++)
	{
		LionState  *col = lion_column(so->ix, rangekeys[i]->sk_attno);
		bool		any = false;

		rc[i].attno = (AttrNumber) col->attno;
		if (lion_scan_col_ranges(so, col, rangekeys[i], &rc[i].ranges,
								 &rc[i].nranges))
		{
			for (r = 0; r < rc[i].nranges; r++)
				any |= !rc[i].ranges[r].empty;
		}
		nomatch = !any;
	}

	/* The other columns' set trees, as lion_emit_columns() builds them. */
	for (i = 0; i < nkeys && !nomatch; i++)
	{
		LionKeyNode *node;
		bool		ok;
		bool		none;

		node = lion_scan_col_tree(so->index,
								  lion_column(so->ix, keys[i]->sk_attno),
								  keys[i], &acc, &ok, &none);
		if (none)
			nomatch = true;
		else if (!ok || node == NULL)
			so->recheck = true; /* the heap scan re-applies this qual */
		else
			args[nargs++] = node;
	}

	if (!nomatch && nargs == 0 && nrange == 1)
	{
		/* Nothing beside the range could be expressed: the range alone. */
		ScanKey		skey = rangekeys[0];
		LionState  *col = lion_column(so->ix, skey->sk_attno);

		for (i = 0; i < acc.nsets; i++)
			lion_posting_set_release(&acc.sets[i]);
		MemoryContextSwitchTo(oldcxt);
		MemoryContextDelete(cxt);
		Assert(so->recheck);
		if ((skey->sk_flags & SK_SEARCHARRAY) == 0)
			return lion_emit_range(so, col, tbm);
		return lion_emit_array_range(so, col, skey, tbm);
	}

	if (!nomatch && nargs == 0)
	{
		/*
		 * Nothing but ranges: the first one, collected, stands for the other
		 * columns' sets.  Past the window's memory the collection goes on in
		 * a temporary file, a window of container keys at a time.
		 */
		LionKeyNode **leaves = (LionKeyNode **)
			palloc(sizeof(LionKeyNode *) * Max(rc[0].nranges, 1));
		int			nleaves = 0;

		for (r = 0; r < rc[0].nranges; r++)
		{
			int			base;
			Size		held;
			bool		spilled;
			int64		nsets;
			int64		nsums;

			if (rc[0].ranges[r].empty)
				continue;
			base = lion_sets_reserve(&acc, 1);
			(void) lion_range_collect(index, rc[0].attno, &rc[0].ranges[r],
									  budget, true, &acc.sets[base], &held,
									  &spilled, &nsets, &nsums);
			if (acc.sets[base].found)
				leaves[nleaves++] = lion_scan_leaf(base);
			else
				acc.nsets--;	/* the range holds no row */
		}
		if (nleaves == 0)
			nomatch = true;
		else
			args[nargs++] = lion_scan_op(LION_KN_OR, leaves, nleaves);
		first = 1;
	}

	if (nomatch)
	{
		for (i = 0; i < acc.nsets; i++)
			lion_posting_set_release(&acc.sets[i]);
		MemoryContextSwitchTo(oldcxt);
		MemoryContextDelete(cxt);
		return 0;
	}

	/*
	 * Every set is located, and every directory lock taken and let go, before
	 * the stream reads a posting page (the §11 rule).  None of their pins
	 * protects anything here - the bitmap heap scan visits every TID - and
	 * the stream keeps none between two containers, so the walks below lock
	 * directory leaves with nothing pinned.
	 */
	for (i = 0; i < acc.nsets; i++)
		lion_posting_set_unpin(&acc.sets[i]);
	st = lion_stream_begin_bounded(acc.nsets, acc.sets,
								   lion_scan_op(LION_KN_AND, args, nargs));

	memset(&w, 0, sizeof(w));
	w.max = 64;
	w.keys = (uint32 *) palloc(sizeof(uint32) * w.max);
	w.f = (LionContainer **) palloc(sizeof(LionContainer *) * w.max);
	w.hit = (uint64 **) palloc(sizeof(uint64 *) * w.max);
	w.tbm = tbm;
	w.recheck = so->recheck;
	w.tst.tbm = tbm;
	w.tst.recheck = so->recheck;
	w.tst.curblk = InvalidBlockNumber;
	w.tmp = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	w.ibuf = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);

	/*
	 * Anything over a kilobyte - a bitset, an image - gets a block of its own
	 * at its own size, so that what the window holds is about what it counts
	 * (lion_range_collect() does the same).
	 */
	w.cxt = AllocSetContextCreate(cxt, "lion index range window",
								  ALLOCSET_SMALL_SIZES);
	w.entrycxt = AllocSetContextCreate(cxt, "lion index range entry",
									   ALLOCSET_DEFAULT_SIZES);

	while (lion_rwin_fill(&w, st, &done, budget, nrange - first > 1))
	{
		for (i = first; i < nrange && w.n > 0; i++)
		{
			w.last = (i == nrange - 1);
			lion_rwin_walk(&w, so, &rc[i]);
			if (!w.last)
				lion_rwin_apply(&w);
		}
		CHECK_FOR_INTERRUPTS();
	}
	ntids = w.ntids + w.tst.total;

	lion_stream_end(st);
	for (i = 0; i < acc.nsets; i++)
		lion_posting_set_release(&acc.sets[i]);
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);

	return ntids;
}

/*
 * The choice both kinds of scan make of the quals they answer (DESIGN.md §5
 * SCAN step 5, §24, §28, §29.2).  The planner may hand one key column several
 * quals - `tags && '{a}' AND tags && '{b}'`, `b = ANY (x) AND b = ANY (y)`,
 * an equality next to a range or a null test - and every one of them that is
 * a SET TREE is answered: an equality, a list, `IS NULL` and a multi-key
 * query.  A column's trees are ANDed with each other exactly as they are with
 * the other columns' (lion_emit_columns(), lion_emit_intersect(), the plain
 * scan's streams), which only ever shrinks the answer: it is the rows the
 * quals select together, with nothing left for the heap to throw away.
 *
 * What is not a set tree is a WALK of the column's entries, one per column:
 *
 *	- `op ANY (array)` with a range strategy, one walk to the widest element
 *	  (§28), beside which a second one and the column's plain range keys are
 *	  left to the recheck, as they were when it outranked them;
 *	- else a range, every `<`, `<=`, `>=` and `>` of the column at once;
 *	- else `IS NOT NULL`, every entry but the NULL one (§14).
 *
 * A column with a set tree is not walked at all: its range keys are left to
 * the recheck, which tests one bound per row of the sets, where the walk
 * would read every entry the range holds to remove whole entries from them.
 * `IS NOT NULL` beside a strict qual of its column - anything but a null test
 * - is IMPLIED by it and neither answered nor rechecked: the strict qual
 * never selects a NULL, whether the index answers it or the recheck does
 * (and a second `IS NOT NULL` is implied by the first).  A qual that can
 * select nothing settles the whole scan (*nomatch): a comparison with NULL,
 * and `IS NULL` beside a strict qual or `IS NOT NULL` of its column.  Two
 * set trees that contradict each other (`k = 5 AND k = 6`) need no such
 * test: their AND is empty.
 *
 * ndropped counts the quals left to the recheck here; a set tree the scan
 * cannot build after all - a multi-key query in mode ALL (§17) - is dropped
 * where it is built, and sets the recheck there.  The same classification is
 * mirrored in lion_am.c (lion_cost_col_quals()) so that the path is priced as
 * the scan it takes.
 */
typedef struct LionScanChoice
{
	ScanKey    *sets;			/* the set quals answered, in key order */
	int			nsets;
	ScanKey		walk[INDEX_MAX_KEYS];	/* per column: its walk, if any */
	ScanKey		first[INDEX_MAX_KEYS];	/* per column: its first answered
										 * qual, set or walk */
	int			nwalks;
	int			ndropped;		/* quals left to the recheck */
	bool		nomatch;		/* the quals can select nothing */
} LionScanChoice;

static void
lion_scan_choose(LionScanOpaque so, LionScanChoice *ch)
{
	int			nrange[INDEX_MAX_KEYS];
	int			narray[INDEX_MAX_KEYS];
	int			nisnull[INDEX_MAX_KEYS];
	int			nstrict[INDEX_MAX_KEYS];
	ScanKey		firstset[INDEX_MAX_KEYS];
	ScanKey		firstrange[INDEX_MAX_KEYS];
	ScanKey		firstarray[INDEX_MAX_KEYS];
	ScanKey		firstnotnull[INDEX_MAX_KEYS];
	int			ncols = so->ix->ncolumns;
	int			i;

	ch->sets = (ScanKey *) palloc(sizeof(ScanKey) * Max(so->nkeys, 1));
	ch->nsets = 0;
	ch->nwalks = 0;
	ch->ndropped = 0;
	ch->nomatch = false;
	for (i = 0; i < ncols; i++)
	{
		ch->walk[i] = NULL;
		ch->first[i] = NULL;
		nrange[i] = 0;
		narray[i] = 0;
		nisnull[i] = 0;
		nstrict[i] = 0;
		firstset[i] = NULL;
		firstrange[i] = NULL;
		firstarray[i] = NULL;
		firstnotnull[i] = NULL;
	}

	/* One pass in key order: the set quals go straight into ch->sets. */
	for (i = 0; i < so->nkeys; i++)
	{
		ScanKey		k = &so->keys[i];
		int			ci = k->sk_attno - 1;
		LionState  *col;

		if (k->sk_attno < 1 || k->sk_attno > ncols)
			continue;			/* not a key column of this index */
		col = lion_column(so->ix, k->sk_attno);

		/* The null tests carry no strategy number and must be tested first. */
		if ((k->sk_flags & SK_SEARCHNOTNULL) != 0)
		{
			if (firstnotnull[ci] == NULL)
				firstnotnull[ci] = k;
			continue;
		}
		if ((k->sk_flags & SK_SEARCHNULL) != 0)
			nisnull[ci]++;
		else
		{
			/*
			 * A strict qual, which with a NULL value - `col = NULL`, a NULL
			 * array, a NULL bound - is never true.
			 */
			nstrict[ci]++;
			if ((k->sk_flags & SK_ISNULL) != 0)
				ch->nomatch = true;

			if (lion_scankey_is_range(col, k))
			{
				if (firstrange[ci] == NULL)
					firstrange[ci] = k;
				nrange[ci]++;
				continue;
			}
			if (lion_scankey_is_array_range(col, k))
			{
				if (firstarray[ci] == NULL)
					firstarray[ci] = k;
				narray[ci]++;
				continue;
			}
		}

		/* An equality, a list, `IS NULL` or a multi-key query: a set tree. */
		ch->sets[ch->nsets++] = k;
		if (firstset[ci] == NULL)
			firstset[ci] = k;
	}

	for (i = 0; i < ncols; i++)
	{
		/* `IS NULL` beside a strict qual or `IS NOT NULL` holds of no row */
		if (nisnull[i] > 0 && (nstrict[i] > 0 || firstnotnull[i] != NULL))
			ch->nomatch = true;

		if (firstset[i] != NULL)
			ch->ndropped += nrange[i] + narray[i];	/* the recheck bounds the sets */
		else if (firstarray[i] != NULL)
		{
			ch->walk[i] = firstarray[i];
			ch->ndropped += narray[i] - 1 + nrange[i];
		}
		else if (firstrange[i] != NULL)
			ch->walk[i] = firstrange[i];	/* every range key, one walk */
		else if (firstnotnull[i] != NULL)
			ch->walk[i] = firstnotnull[i];
		if (ch->walk[i] != NULL)
			ch->nwalks++;
		ch->first[i] = (firstset[i] != NULL) ? firstset[i] : ch->walk[i];
	}
}

/*
 * The bitmap scan of the quals *ch answers (lion_scan_choose()).
 */
static int64
lion_getbitmap_choice(LionScanOpaque so, LionScanChoice *ch, TIDBitmap *tbm)
{
	Relation	index = so->index;
	int			ncols = so->ix->ncolumns;
	int			i;
	LionState  *col;
	ScanKey		skey;

	if (ch->nomatch || ch->nsets + ch->nwalks == 0)
		return 0;

	so->recheck = (ch->ndropped > 0);

	/*
	 * Several quals, of one column or of several: intersect their set trees.
	 * A qual the sets cannot express is dropped there and rechecked; when NONE
	 * of them can be expressed the answer falls through to the single-qual
	 * path below, which knows how to walk a whole column.  A RANGE column is
	 * not a set tree at all - its answer is a union of however many entries
	 * the range holds (DESIGN.md §28), `col < ANY (array)` included - so its
	 * walk is ANDed with the others' stream a container at a time
	 * (lion_emit_intersect()).  `IS NOT NULL` goes with the sets, which drop
	 * it: beside anything that answers, it is only rechecked.
	 */
	if (ch->nsets + ch->nwalks > 1)
	{
		ScanKey    *keys;
		ScanKey		rangekeys[INDEX_MAX_KEYS];
		int			n = 0;
		int			nrange = 0;
		int64		ntids;

		keys = (ScanKey *) palloc(sizeof(ScanKey) * (ch->nsets + ncols));
		for (i = 0; i < ch->nsets; i++)
			keys[n++] = ch->sets[i];
		for (i = 0; i < ncols; i++)
		{
			if (ch->walk[i] == NULL)
				continue;
			if ((ch->walk[i]->sk_flags & SK_SEARCHNOTNULL) != 0)
				keys[n++] = ch->walk[i];
			else
				rangekeys[nrange++] = ch->walk[i];
		}

		if (nrange > 0)
			ntids = lion_emit_intersect(so, keys, n, rangekeys, nrange, tbm);
		else
			ntids = lion_emit_columns(so, keys, n, tbm);
		pfree(keys);
		if (ntids >= 0)
			return ntids;

		/* Nothing was expressible: answer the first column and recheck. */
		so->recheck = true;
	}

	for (i = 0; i < ncols; i++)
	{
		if (ch->first[i] != NULL)
			break;
	}
	Assert(i < ncols);
	skey = ch->first[i];
	col = lion_column(so->ix, skey->sk_attno);

	/* The null tests carry no strategy number and must be tested first. */
	if ((skey->sk_flags & SK_SEARCHNULL) != 0)
		return lion_emit_null(index, col, tbm, so->recheck);
	if ((skey->sk_flags & SK_SEARCHNOTNULL) != 0)
		return lion_emit_all_keys(index, col, tbm, so->recheck);

	/* `col = NULL` (or a NULL array) is never true. */
	if ((skey->sk_flags & SK_ISNULL) != 0)
		return 0;

	/* A multi-key opclass answers the strategies of DESIGN.md §17. */
	if (col->multikey)
		return lion_emit_multikey(so, col, skey, tbm);

	/* Every range key of the column, as one walk (DESIGN.md §28). */
	if (lion_scankey_is_range(col, skey))
		return lion_emit_range(so, col, tbm);

	/* ... and `col < ANY (array)`, one walk per element. */
	if ((skey->sk_flags & SK_SEARCHARRAY) != 0 &&
		LION_STRAT_IS_RANGE(skey->sk_strategy))
		return lion_emit_array_range(so, col, skey, tbm);

	/*
	 * A strategy this file does not know is an ERROR and not an empty result:
	 * the opclass would be claiming an operator the scan cannot answer, and
	 * "no rows" would be a wrong answer rather than a missing optimisation.
	 */
	if (skey->sk_strategy != LION_STRAT_EQUAL)
		elog(ERROR, "lion index \"%s\": unsupported strategy %d",
			 RelationGetRelationName(index), (int) skey->sk_strategy);

	if ((skey->sk_flags & SK_SEARCHARRAY) != 0)
		return lion_emit_array(so, col, skey, tbm);

	return lion_emit_value(so, col, skey, skey->sk_argument, tbm);
}

/*
 * The bitmap scan proper, on the keys so->keys[0 .. so->nkeys - 1].
 */
static int64
lion_getbitmap_so(LionScanOpaque so, TIDBitmap *tbm)
{
	LionScanChoice ch;
	int64		ntids;

	/*
	 * No scan key at all: a PARTIAL index whose predicate the query implies
	 * (amoptionalkey, DESIGN.md §24).  The answer is every row the index
	 * holds, which is the union of every entry of ANY ONE key column - the
	 * NULL entry included this time, because a row whose value is NULL is
	 * still an indexed row.  No recheck: these TIDs are exactly the rows the
	 * scan selects.
	 */
	if (so->nkeys < 1)
	{
		so->recheck = false;
		return lion_emit_all_keys_ext(so->index, lion_column(so->ix, 1), tbm,
									 false, true, NULL);
	}

	lion_scan_choose(so, &ch);
	ntids = lion_getbitmap_choice(so, &ch, tbm);
	pfree(ch.sets);

	return ntids;
}

int64
liongetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	LionScanOpaque so = (LionScanOpaque) scan->opaque;

	/* Not under old_snapshot_threshold (DESIGN.md §9). */
	lion_check_old_snapshot(scan->indexRelation, scan->xs_snapshot);

	/*
	 * Re-fetch the cached state: a relcache invalidation since ambeginscan
	 * would have thrown the copy in rd_amcache away.
	 */
	so->ix = lion_get_index_state(scan->indexRelation);
	so->index = scan->indexRelation;
	so->keys = scan->keyData;
	so->nkeys = scan->numberOfKeys;

	pgstat_count_index_scan(scan->indexRelation);
#if PG_VERSION_NUM >= 180000
	if (scan->instrument)
		scan->instrument->nsearches++;
#endif

	return lion_getbitmap_so(so, tbm);
}

/* ---------------------------------------------------------------------
 * Plain index scans (DESIGN.md §29): the TID source and amgettuple
 * --------------------------------------------------------------------- */

/*
 * The shapes of §29.3.  NONE selects nothing; SETS is one stream over the
 * columns' set trees; WALK streams the entries of one column's walk, each
 * ANDed with the other columns' trees; WINDOW is the same answer for a walk
 * too long to restart the other columns' stream for every entry: the walked
 * entries ORed together a window of the other columns' containers at a time
 * and ANDed with ONE stream of those; LIST streams a long IN list a batch of
 * its sets at a time (§29.4); UNION streams every entry of a multi-key column
 * as their exact union, a window of container keys at a time.
 *
 * No shape ever returns a TID the index does not hold (§29.6): an index-only
 * scan evaluates no recheck qual, and a plain scan of a partial index has the
 * quals its predicate implies taken out of its recheck, so a superset TID -
 * a lossy bitmap page's every offset, say - would come back as a row.
 */
typedef enum LionSourceShape
{
	LION_SRC_NONE,
	LION_SRC_SETS,
	LION_SRC_WALK,
	LION_SRC_WINDOW,
	LION_SRC_LIST,
	LION_SRC_UNION
} LionSourceShape;

struct LionSource
{
	LionScanOpaque so;			/* the keys and the probe cache */
	Relation	index;
	MemoryContext cxt;			/* everything below, but what entrycxt holds */
	MemoryContext entrycxt;		/* one entry's or one batch's stream */
	bool		keeppins;		/* §29.5: a non-MVCC scan keeps its pins */
	bool		recheck;		/* §29.6 */
	LionSourceShape shape;

	/* the located sets of the SETS tree, or of the other columns' trees */
	LionPostingSet *sets;		/* nsets of them, + 1 slot for a WALK entry */
	int			nsets;
	LionKeyNode *tree;			/* SETS, WALK: over sets[] (WALK's names
								 * sets[nsets] for the entry) */
	LionKeyNode **restargs;		/* LIST: the other columns' trees */
	int			nrestargs;
	LionSetStream *stream;		/* the current one */

	/* WALK, and WINDOW's walk of each window */
	LionLeafWalk walk;
	LionRange  *ranges;
	int			nranges;
	LionIndexState *ix;			/* the state walk.col and ranges[] point into */
	AttrNumber	walkattno;		/* ... and the walked column's number */

	/*
	 * WINDOW: the other columns' stream (over sets[] and tree), and a window
	 * of its next containers - each as a bitset image, beside the image the
	 * walked entries are ORed into at the same container key - made on first
	 * use and reused by every later window.
	 */
	LionSetStream *reststream;
	bool		restdone;		/* reststream has returned NULL */
	int			wwidth;			/* slots per window */
	int			wn;				/* slots the current window fills */
	int			wemit;			/* the next slot to hand out */
	uint32	   *wkeys;			/* [wwidth] the slots' container keys */
	uint64	  **wrest;			/* [wwidth] the other columns' images */
	uint64	  **wwalk;			/* [wwidth] the walked entries' images */
	bool	   *wtouched;		/* [wwidth] an entry had a container there */
	MemoryContext walkcxt;		/* one window's walk */
	LionContainer *ibuf;		/* an INLINE entry's item, aligned */

	/* LIST: the list's values in lookup order, located a batch at a time */
	AttrNumber	listattno;
	Oid			listtype;
	Datum	   *lvalues;
	uint32	   *lhashes;
	int			nlvalues;
	int			lpos;			/* the first value not located yet */
	LionPostingSet *bsets;		/* the batch's sets, in entrycxt */
	int			nbsets;

	/* UNION: a window of container keys, ORed from every entry */
	AttrNumber	unionattno;
	bool		withnull;
	int			uwidth;			/* container keys per window */
	uint64	  **uimg;			/* [uwidth] bitset images, made on first use */
	bool	   *utouched;		/* [uwidth] */
	uint64		ustart;			/* the window's first container key */
	uint64		unext;			/* the smallest key seen past the window */
	int			uemit;			/* the next slot to hand out */
	bool		ufirst;
	bool		udone;
	LionContainer *cbuf;		/* the container handed out */
};

/*
 * pg_lion.scan_window_floor, in kB: the least memory a window below may take,
 * whatever work_mem says, because every window walks the entries again - a
 * window never shrinks with work_mem past it.  4 MB by default; the
 * regression suite lowers it to cross window boundaries on a small table.
 */
int			lion_scan_window_floor = LION_SCAN_WINDOW_FLOOR;

static double
lion_scan_window_bytes(void)
{
	return Max((double) lion_scan_window_floor, (double) work_mem) * 1024.0;
}

/*
 * How many container keys one UNION window gathers (§29.3): a 4 kB bitset
 * image each, so the window memory over 4 kB - 1024 at the default floor
 * whatever work_mem says, and more when work_mem allows, up to 65536.  An
 * image is made only when an entry has a container there, so a window costs
 * what it holds.  lioncostestimate() prices the windows with the same number.
 */
int
lion_union_window(void)
{
	return (int) Max(1.0, Min(lion_scan_window_bytes() / LION_BITSET_BYTES,
							  65536.0));
}

/*
 * How many of the other columns' containers one WINDOW gathers (§29.3): two
 * bitset images each, the other columns' and the walked entries', so the
 * window memory over 8 kB - 512 at the default floor, for the UNION's reason:
 * every window walks the range again.  At most 4 MB at the floor, work_mem
 * above it.  lioncostestimate() prices the windows with the same number.
 */
int
lion_walk_window(void)
{
	return (int) Max(1.0, Min(lion_scan_window_bytes() / (2 * LION_BITSET_BYTES),
							  65536.0));
}

/*
 * The blocks of a source's entry context, which holds one walked entry's
 * stream or one LIST batch at a time: all of one size, so that what the
 * context takes is what it holds, give or take one block.  With the default
 * doubling blocks a batch took twice what it used, the last block half empty
 * - 1073 kB at 1 MB of work_mem, 4180 kB at 4 MB, 16.9 MB at 16 MB for 17.6
 * kB a set (2026-09-25 review).  A walk's entry stream fits one block, so it
 * resets to that one block from entry to entry, with no malloc between.
 */
#define LION_SCAN_ENTRY_BLOCK	(64 * 1024)

/*
 * How many sets of one IN list a plain scan holds cursors for at once (§29.4).
 * lioncostestimate() tells a LIST from a set tree with the same number.
 */
int
lion_scan_list_batch(void)
{
	/*
	 * A located set and the cursor that reads it take up to ~19 kB in the
	 * entry context: two allocator chunks of 8 kB - the cursor's staging
	 * container and, for a set with a sparse segment (§13), the segment's
	 * buffer, 4104 bytes each, or a posting-tree cursor's page image - and
	 * the set itself, its OR node share and the blocks' slack.  32 kB of
	 * work_mem per set keeps a batch near 60% of work_mem, which leaves the
	 * rest for the list itself (12 bytes a value, in the source's own
	 * context) and the fixed parts.  At least 32 sets, ~620 kB, whatever
	 * work_mem says: below 1 MB that floor is the bound.
	 */
	return Max(32, work_mem / 32);
}

/* The probe of a scan key's type on one column, cached per column. */
static LionProbe *
lion_scan_probe(LionScanOpaque so, LionState *col, Oid subtype)
{
	LionScanCol *sc = &so->cols[col->attno - 1];

	if (!sc->probevalid || sc->subtype != subtype)
	{
		/* the probe's FmgrInfos live as long as the scan */
		MemoryContext oldcxt = MemoryContextSwitchTo(GetMemoryChunkContext(so));

		lion_probe_init(so->index, col, subtype, &sc->probe);
		MemoryContextSwitchTo(oldcxt);
		sc->subtype = subtype;
		sc->probevalid = true;
	}
	return &sc->probe;
}

/*
 * The ranges a walk of a scalar column tests its entries with: every range key
 * of the column as ONE range, or, for `op ANY (array)`, the range of the
 * widest element when the column orders the elements (§28) and one range per
 * element otherwise, all of them walked at once.  Returns false when nothing
 * can be in range: an empty array, or one of NULLs only.  The plain scan's
 * WALK (§29.3) and the bitmap scan's intersection (lion_emit_intersect())
 * both walk these; they are allocated in the current memory context, with
 * the element values they point at.
 */
static bool
lion_scan_col_ranges(LionScanOpaque so, LionState *col, ScanKey best,
					 LionRange **rangesp, int *nrangesp)
{
	Relation	index = so->index;
	LionRange  *ranges;
	int			nranges;
	int			i;

	if ((best->sk_flags & SK_SEARCHARRAY) == 0)
	{
		ranges = (LionRange *) palloc(sizeof(LionRange));
		lion_range_init(&ranges[0], index, (AttrNumber) col->attno);
		for (i = 0; i < so->nkeys; i++)
		{
			ScanKey		k = &so->keys[i];

			if (k->sk_attno != col->attno || !lion_scankey_is_range(col, k))
				continue;
			lion_range_add(&ranges[0], index, k->sk_strategy,
						   k->sk_func.fn_oid, k->sk_subtype, k->sk_argument,
						   (k->sk_flags & SK_ISNULL) != 0, k->sk_collation);
		}
		*rangesp = ranges;
		*nrangesp = 1;
		return true;
	}

	*rangesp = NULL;
	*nrangesp = 0;
	if ((best->sk_flags & SK_ISNULL) != 0)
		return false;			/* `k < ANY (NULL)` */

	{
		ArrayType  *arr = DatumGetArrayTypeP(best->sk_argument);
		Oid			elemtype = ARR_ELEMTYPE(arr);
		LionProbe  *probe = lion_scan_probe(so, col, best->sk_subtype);
		int16		elmlen;
		bool		elmbyval;
		char		elmalign;
		Datum	   *elems;
		bool	   *nulls;
		int			nelems;
		int			widest = -1;

		get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
		deconstruct_array(arr, elemtype, elmlen, elmbyval, elmalign,
						  &elems, &nulls, &nelems);

		ranges = (LionRange *) palloc(sizeof(LionRange) * Max(nelems, 1));
		nranges = 0;

		for (i = 0; i < nelems; i++)
		{
			if (nulls[i])
				continue;		/* a strict comparison with NULL is not true */

			if (probe->hassort)
			{
				int32		c;

				if (widest < 0)
				{
					widest = i;
					continue;
				}
				c = DatumGetInt32(FunctionCall2Coll(&probe->sortproc,
													col->collation,
													elems[i], elems[widest]));
				if (LION_STRAT_IS_LOWER(best->sk_strategy) ? (c < 0) : (c > 0))
					widest = i;
				continue;
			}

			/* No order among the elements: one range each, in ONE walk. */
			lion_range_init(&ranges[nranges], index, (AttrNumber) col->attno);
			lion_range_add(&ranges[nranges], index,
						   best->sk_strategy, best->sk_func.fn_oid,
						   best->sk_subtype, elems[i], false,
						   best->sk_collation);
			nranges++;
		}

		if (widest >= 0)
		{
			lion_range_init(&ranges[0], index, (AttrNumber) col->attno);
			lion_range_add(&ranges[0], index, best->sk_strategy,
						   best->sk_func.fn_oid, best->sk_subtype,
						   elems[widest], false, best->sk_collation);
			nranges = 1;
		}

		/* The element values stay with the ranges that use them. */
		*rangesp = ranges;
		*nrangesp = nranges;
		return nranges > 0;
	}
}

/* ... as the source of a plain scan's WALK keeps them (§29.3). */
static bool
lion_source_ranges(LionSource *src, LionState *col, ScanKey best)
{
	return lion_scan_col_ranges(src->so, col, best, &src->ranges,
								&src->nranges);
}

/*
 * A scalar `col = ANY (array)` long enough to be located a batch at a time
 * (§29.4): how many non-NULL-or-not elements the array has, or -1 when the key
 * is not such a list at all.
 */
static int
lion_scankey_list_length(LionState *col, ScanKey skey)
{
	if (col->multikey ||
		(skey->sk_flags & (SK_SEARCHARRAY | SK_SEARCHNULL | SK_SEARCHNOTNULL |
						   SK_ISNULL)) != SK_SEARCHARRAY ||
		skey->sk_strategy != LION_STRAT_EQUAL)
		return -1;
	return ArrayGetNItems(ARR_NDIM(DatumGetArrayTypeP(skey->sk_argument)),
						  ARR_DIMS(DatumGetArrayTypeP(skey->sk_argument)));
}

/*
 * About how many posting pages one located set fills.  Its ntids and
 * ncontainers are its entry's own hints; a container's members take two
 * bytes each up to LION_BITSET_BYTES.  An INLINE set is inside its entry and
 * reads no page of its own.
 */
static double
lion_source_set_pages(LionPostingSet *ps)
{
	double		bytes;

	if (!ps->found || ps->is_inline)
		return 0.0;
	bytes = Min(2.0 * (double) ps->ntids,
				(double) LION_BITSET_BYTES * ps->ncontainers) +
		(double) LION_CONTAINER_HDRSZ * ps->ncontainers;
	return Max(1.0, ceil(bytes / BLCKSZ));
}

/*
 * About how many posting pages the located sets fill - what one stream over
 * them reads at most (an AND leapfrogs past some of it).
 */
static double
lion_source_rest_pages(LionSource *src)
{
	double		pages = 0.0;
	int			i;

	for (i = 0; i < src->nsets; i++)
		pages += lion_source_set_pages(&src->sets[i]);
	return pages;
}

/*
 * What restarting the located sets' stream for one walked entry of ntids
 * TIDs reads: of each set, a descent, and then the posting pages that hold
 * its containers at the entry's container keys - at most one page per TID of
 * the entry, and at most the set's own pages.  An entry of a few rows costs a
 * descent of each posting tree; an entry spread over the heap, which is
 * every entry of a low-cardinality column, costs reading the sets whole.  At
 * least 1, so that a walk beside INLINE sets alone is counted in entries.
 */
static double
lion_source_restart_pages(LionSource *src, double ntids)
{
	double		pages = 0.0;
	int			i;

	for (i = 0; i < src->nsets; i++)
	{
		double		setpages = lion_source_set_pages(&src->sets[i]);

		if (setpages > 0.0)
			pages += Max(1.0, Min(setpages, ntids));
	}
	return Max(1.0, pages);
}

/*
 * Is the walk of col, beside the other columns' sets, too long to restart
 * their stream for every entry (§29.3)?  The WINDOW shape reads their posting
 * pages once and walks the range once per window; the entry-by-entry WALK
 * restarts their stream for every entry, which reads what
 * lion_source_restart_pages() says.  So a walk is long when its restarts
 * would read more than those sets' posting pages, and than LION_WALK_SHORT:
 * counted here, from the directory leaves alone - each entry's ntids is in
 * its header - and never further than that.
 *
 * The first version counted ENTRIES against the posting pages, a restart
 * being a descent of each posting tree.  That holds for a column of a few
 * rows per key; each key of a low-cardinality column has rows at nearly every
 * container key, so each restart streamed the other columns' sets over the
 * whole heap.  `pc < 100` (100 keys of 4000 rows each) beside four such sets
 * over 8M rows was walked entry by entry and took 10.8 s where the bitmap
 * scan took 0.77 s; as a WINDOW it takes 0.39 s (2026-09-27; on a much
 * larger table an always-true range of a few thousand keys beside four such
 * sets did not finish in 60 s).
 */
#define LION_WALK_SHORT		16

static bool
lion_source_walk_is_long(LionSource *src, LionState *col)
{
	LionLeafWalk w;
	Size		itemlen;
	LionEntryTuple *entry;
	double		limit = Max((double) LION_WALK_SHORT, lion_source_rest_pages(src));
	double		restarts = 0.0;
	MemoryContext oldcxt = MemoryContextSwitchTo(src->entrycxt);

	lion_walk_begin(&w, src->index, col, false, src->ranges, src->nranges,
					false);
	while (restarts <= limit && (entry = lion_walk_next(&w, &itemlen)) != NULL)
		restarts += lion_source_restart_pages(src, (double) entry->ntids);
	lion_walk_end(&w);
	MemoryContextSwitchTo(oldcxt);
	MemoryContextReset(src->entrycxt);

	return restarts > limit;
}

/*
 * Build the source for so->keys (DESIGN.md §29.3).  Every set is located -
 * every directory lock taken - before any posting page is pinned, which is
 * the reader side of the §11 deadlock rule; a LIST locates each batch before
 * it pins a posting page for it, with the previous batch's pins gone.
 */
static LionSource *
lion_source_build(LionScanOpaque so, bool keeppins, MemoryContext parent)
{
	MemoryContext cxt;
	MemoryContext oldcxt;
	LionSource *src;
	LionScanChoice ch;
	LionScanSets acc;
	LionKeyNode **args;
	int			nargs = 0;
	int			walkcol = -1;
	int			notnullcol = -1;
	ScanKey		listkey = NULL;
	int			unioncol = -1;
	bool		withnull = false;
	bool		nomatch = false;
	int			ncols = so->ix->ncolumns;
	int			batch = lion_scan_list_batch();
	int			i;

	cxt = AllocSetContextCreate(parent, "lion index scan",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	src = (LionSource *) palloc0(sizeof(LionSource));
	src->so = so;
	src->index = so->index;
	src->cxt = cxt;
	src->keeppins = keeppins;
	src->shape = LION_SRC_NONE;
	src->entrycxt = AllocSetContextCreate(cxt, "lion index scan entry",
										  0,
										  LION_SCAN_ENTRY_BLOCK,
										  LION_SCAN_ENTRY_BLOCK);

	memset(&ch, 0, sizeof(ch));
	acc.maxsets = 8;
	acc.nsets = 0;
	acc.sets = (LionPostingSet *) palloc0(sizeof(LionPostingSet) * acc.maxsets);

	if (so->nkeys < 1)
	{
		/*
		 * No key at all: a partial index whose predicate the query implies
		 * (§24).  Every row the index holds is under column 1, NULL entry
		 * included - streamed by a walk, or as a union when that column is
		 * multi-key and a walk would repeat rows.
		 */
		args = (LionKeyNode **) palloc(sizeof(LionKeyNode *));
		withnull = true;
		if (lion_column(so->ix, 1)->multikey)
			unioncol = 0;
		else
			walkcol = 0;
	}
	else
	{
		lion_scan_choose(so, &ch);
		if (ch.nomatch || ch.nsets + ch.nwalks == 0)
			nomatch = true;
		src->recheck = (ch.ndropped > 0);

		/* one tree per set qual, and one more slot for a WALK's entry */
		args = (LionKeyNode **) palloc(sizeof(LionKeyNode *) * (ch.nsets + 1));

		/*
		 * The set quals, every one of every column: a set tree each, ANDed
		 * below, or a long list that drives the scan a batch at a time.
		 */
		for (i = 0; i < ch.nsets && !nomatch; i++)
		{
			ScanKey		skey = ch.sets[i];
			LionState  *col = lion_column(so->ix, skey->sk_attno);
			LionKeyNode *node;
			bool		ok;
			bool		none;

			if (col->multikey)
			{
				/* Answered, and rechecked all the same (§29.6). */
				src->recheck = true;
				node = lion_scan_col_tree(so->index, col, skey, &acc, &ok, &none);
				if (none)
					nomatch = true;
				else if (!ok)
				{
					/* mode ALL: every row; the first such column, as below */
					if (unioncol < 0 || skey->sk_attno - 1 < unioncol)
						unioncol = skey->sk_attno - 1;
				}
				else
					args[nargs++] = node;
				continue;
			}

			if (lion_scankey_list_length(col, skey) > batch)
			{
				if (listkey == NULL)
					listkey = skey;
				else
					src->recheck = true;	/* a second long list is not answered */
				continue;
			}

			if ((skey->sk_flags & (SK_SEARCHNULL | SK_ISNULL)) == 0 &&
				skey->sk_strategy != LION_STRAT_EQUAL)
				elog(ERROR, "lion index \"%s\": unsupported strategy %d",
					 RelationGetRelationName(so->index),
					 (int) skey->sk_strategy);

			node = lion_scan_col_tree(so->index, col, skey, &acc, &ok, &none);
			if (none)
				nomatch = true;
			else
			{
				Assert(ok && node != NULL);
				args[nargs++] = node;
			}
		}

		/*
		 * The walks, one per column at most: a range, or `IS NOT NULL`, which
		 * is every entry of the column but its NULL one - a walk of the whole
		 * column - and only drives the scan when nothing else answers it
		 * (below).  A multi-key column's is a union: a walk would repeat rows.
		 */
		for (i = 0; i < ncols && !nomatch; i++)
		{
			ScanKey		skey = ch.walk[i];

			if (skey == NULL)
				continue;

			if (lion_column(so->ix, (AttrNumber) (i + 1))->multikey)
			{
				src->recheck = true;
				if (unioncol < 0 || i < unioncol)
					unioncol = i;
				continue;
			}
			if ((skey->sk_flags & SK_SEARCHNOTNULL) != 0)
			{
				if (notnullcol < 0)
					notnullcol = i;
				else
					src->recheck = true;	/* a second one is not answered */
				continue;
			}
			if (walkcol < 0)
				walkcol = i;
			else
				src->recheck = true;	/* a second walk is not answered */
		}

		/*
		 * One driver: a long list outranks a walk, which is then left to the
		 * recheck.  A multi-key column that needs every row, next to a qual
		 * that does answer, is dropped and rechecked, as the bitmap path does;
		 * only when nothing else answers is it streamed as a union.
		 */
		if (listkey != NULL && walkcol >= 0)
		{
			walkcol = -1;
			src->recheck = true;
		}

		/*
		 * `IS NOT NULL` the same way: dropped and rechecked next to anything
		 * that answers - the bitmap path drops it too, and the cost model
		 * prices such a scan by the other columns' selectivity - and walked
		 * only when nothing does.  It used to drive a WALK whenever it was
		 * there, every entry of its column ANDed with the other columns'
		 * stream, restarted per entry: `a IS NOT NULL AND b = 5` over a
		 * million unique a took 4.37M buffer hits and 1.8 s against the
		 * bitmap's 19 ms (2026-09-25 review).
		 */
		if (notnullcol >= 0)
		{
			if (nargs > 0 || walkcol >= 0 || listkey != NULL)
				src->recheck = true;
			else
				walkcol = notnullcol;
		}
		if (unioncol >= 0 && (nargs > 0 || walkcol >= 0 || listkey != NULL))
			unioncol = -1;
	}

	if (!nomatch && walkcol >= 0 && !withnull)
	{
		LionState  *col = lion_column(so->ix, (AttrNumber) (walkcol + 1));
		ScanKey		walk = ch.walk[walkcol];

		if ((walk->sk_flags & SK_SEARCHNOTNULL) == 0 &&
			!lion_source_ranges(src, col, walk))
			nomatch = true;
	}

	if (!nomatch && listkey != NULL)
	{
		ArrayType  *arr = DatumGetArrayTypeP(listkey->sk_argument);
		int16		elmlen;
		bool		elmbyval;
		char		elmalign;
		Datum	   *elems;
		bool	   *nulls;
		int			nelems;

		src->listattno = listkey->sk_attno;
		src->listtype = ARR_ELEMTYPE(arr);
		get_typlenbyvalalign(src->listtype, &elmlen, &elmbyval, &elmalign);
		deconstruct_array(arr, src->listtype, elmlen, elmbyval, elmalign,
						  &elems, &nulls, &nelems);
		src->lvalues = (Datum *) palloc(sizeof(Datum) * Max(nelems, 1));
		src->lhashes = (uint32 *) palloc(sizeof(uint32) * Max(nelems, 1));
		src->nlvalues = lion_probe_sort(so->index, src->listattno,
										src->listtype, nelems, elems, nulls,
										src->lvalues, src->lhashes);
		pfree(elems);
		pfree(nulls);
		if (src->nlvalues == 0)
			nomatch = true;
	}

	if (nomatch || unioncol >= 0)
	{
		for (i = 0; i < acc.nsets; i++)
			lion_posting_set_release(&acc.sets[i]);
	}

	if (nomatch)
	{
		src->shape = LION_SRC_NONE;
		MemoryContextSwitchTo(oldcxt);
		return src;
	}

	if (unioncol >= 0)
	{
		src->shape = LION_SRC_UNION;
		src->recheck = true;
		src->unionattno = (AttrNumber) (unioncol + 1);
		src->withnull = withnull;
		src->uwidth = lion_union_window();
		src->uimg = (uint64 **) palloc0(sizeof(uint64 *) * src->uwidth);
		src->utouched = (bool *) palloc0(sizeof(bool) * src->uwidth);
		src->uemit = src->uwidth;
		src->ufirst = true;
		src->cbuf = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
		MemoryContextSwitchTo(oldcxt);
		return src;
	}

	/*
	 * The located sets: one more slot for a WALK's entry.  Under an MVCC
	 * snapshot no pin is kept at all (§29.5); otherwise a set located past
	 * the list pin budget carries no interlock, and the TIDs are rechecked.
	 */
	src->nsets = acc.nsets;
	src->sets = (LionPostingSet *) palloc0(sizeof(LionPostingSet) * (acc.nsets + 1));
	for (i = 0; i < acc.nsets; i++)
	{
		src->sets[i] = acc.sets[i];
		if (!keeppins)
			lion_posting_set_unpin(&src->sets[i]);
		else if (src->sets[i].found && src->sets[i].is_inline &&
				 !BufferIsValid(src->sets[i].pinbuf))
			src->recheck = true;
	}

	if (listkey != NULL)
	{
		src->shape = LION_SRC_LIST;
		src->restargs = args;
		src->nrestargs = nargs;
	}
	else if (walkcol < 0)
	{
		src->shape = LION_SRC_SETS;
		src->tree = lion_scan_op(LION_KN_AND, args, nargs);
		src->stream = lion_stream_begin(src->nsets, src->sets, src->tree,
										keeppins);
	}
	else if (nargs > 0 && !keeppins &&
			 lion_source_walk_is_long(src,
									  lion_column(so->ix, (AttrNumber) (walkcol + 1))))
	{
		/*
		 * A long walk beside the other columns' sets: their ONE stream, and
		 * the walk ORed into windows of it (§29.3).  Only without pins - a
		 * window copies containers out of many pages, as the UNION does, so
		 * a scan that must hold the page of each batch (§29.5) keeps the
		 * entry-by-entry WALK, whose pins are the ones it needs.
		 */
		src->shape = LION_SRC_WINDOW;
		src->tree = lion_scan_op(LION_KN_AND, args, nargs);
		src->ix = so->ix;
		src->walkattno = (AttrNumber) (walkcol + 1);
		src->reststream = lion_stream_begin(src->nsets, src->sets, src->tree,
											false);
		src->wwidth = lion_walk_window();
		src->wkeys = (uint32 *) palloc(sizeof(uint32) * src->wwidth);
		src->wrest = (uint64 **) palloc0(sizeof(uint64 *) * src->wwidth);
		src->wwalk = (uint64 **) palloc0(sizeof(uint64 *) * src->wwidth);
		src->wtouched = (bool *) palloc0(sizeof(bool) * src->wwidth);
		src->cbuf = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
		src->ibuf = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
		src->walkcxt = AllocSetContextCreate(cxt, "lion index scan window",
											 ALLOCSET_SMALL_SIZES);
	}
	else
	{
		LionState  *col = lion_column(so->ix, (AttrNumber) (walkcol + 1));

		src->shape = LION_SRC_WALK;
		args[nargs++] = lion_scan_leaf(src->nsets);
		src->tree = lion_scan_op(LION_KN_AND, args, nargs);
		src->ix = so->ix;
		src->walkattno = (AttrNumber) col->attno;
		lion_walk_begin(&src->walk, so->index, col, withnull, src->ranges,
						src->nranges, keeppins);
	}

	MemoryContextSwitchTo(oldcxt);
	return src;
}

/* A walked entry as a located set, without a lookup (§29.3). */
static void
lion_source_entry_set(Relation index, LionEntryTuple *entry, Size itemlen,
					  MemoryContext cxt, LionPostingSet *ps)
{
	memset(ps, 0, sizeof(LionPostingSet));
	ps->index = index;
	ps->attno = entry->attno;
	ps->found = true;
	ps->pinbuf = InvalidBuffer;
	ps->ntids = entry->ntids;
	ps->ncontainers = entry->ncontainers;
	ps->entryblk = InvalidBlockNumber;
	ps->entryoff = InvalidOffsetNumber;
	ps->keyisnull = LionEntryIsNullKey(entry);
	ps->cxt = cxt;
	if ((entry->flags & LION_ENTRY_INLINE) != 0)
	{
		ps->is_inline = true;
		ps->head = InvalidBlockNumber;
		ps->payload = LionEntryGetPayload(entry);
		ps->paylen = LION_ENTRY_PAYLOAD_LEN(entry, itemlen);
	}
	else
		ps->head = entry->head;
}

/* Let go of a LIST batch's sets: their pins, then their memory. */
static void
lion_source_end_batch(LionSource *src)
{
	int			i;

	if (src->stream != NULL)
		lion_stream_end(src->stream);
	src->stream = NULL;
	for (i = 0; i < src->nbsets; i++)
		lion_posting_set_release(&src->bsets[i]);
	src->bsets = NULL;
	src->nbsets = 0;
	MemoryContextReset(src->entrycxt);
}

/*
 * The next batch of a LIST (§29.4): at most lion_scan_list_batch() values, and
 * more only to keep the values of one hash - which include every value of one
 * equality class - together, so that no entry is located by two batches and
 * returned twice.  Returns false at the end of the list.
 */
static bool
lion_source_list_batch(LionSource *src)
{
	MemoryContext oldcxt;
	LionPostingSet *all;
	LionKeyNode **leaves;
	LionKeyNode **args;
	int			batch = lion_scan_list_batch();
	int			start;
	int			end;
	int			nb;
	int			nfound;
	int			i;

	for (;;)
	{
		if (src->lpos >= src->nlvalues)
			return false;

		start = src->lpos;
		end = Min(start + batch, src->nlvalues);
		while (end < src->nlvalues && src->lhashes[end] == src->lhashes[end - 1])
			end++;
		src->lpos = end;

		oldcxt = MemoryContextSwitchTo(src->entrycxt);
		all = (LionPostingSet *) palloc0(sizeof(LionPostingSet) *
										 (src->nsets + (end - start)));
		if (src->nsets > 0)
			memcpy(all, src->sets, sizeof(LionPostingSet) * src->nsets);
		nb = lion_posting_set_lookup_many_col(src->index, src->listattno,
											 src->listtype, end - start,
											 &src->lvalues[start], NULL,
											 &all[src->nsets], &nfound);
		src->bsets = &all[src->nsets];
		src->nbsets = nb;

		for (i = 0; i < nb; i++)
		{
			if (!src->keeppins)
				lion_posting_set_unpin(&src->bsets[i]);
			else if (src->bsets[i].found && src->bsets[i].is_inline &&
					 !BufferIsValid(src->bsets[i].pinbuf))
				src->recheck = true;
		}

		if (nfound == 0)
		{
			MemoryContextSwitchTo(oldcxt);
			lion_source_end_batch(src);
			CHECK_FOR_INTERRUPTS();
			continue;
		}

		leaves = (LionKeyNode **) palloc(sizeof(LionKeyNode *) * nb);
		for (i = 0; i < nb; i++)
			leaves[i] = lion_scan_leaf(src->nsets + i);
		args = (LionKeyNode **) palloc(sizeof(LionKeyNode *) * (src->nrestargs + 1));
		for (i = 0; i < src->nrestargs; i++)
			args[i] = src->restargs[i];
		args[src->nrestargs] = lion_scan_op(LION_KN_OR, leaves, nb);

		src->stream = lion_stream_begin(src->nsets + nb, all,
										lion_scan_op(LION_KN_AND, args,
													 src->nrestargs + 1),
										src->keeppins);
		MemoryContextSwitchTo(oldcxt);
		return true;
	}
}

/*
 * The next window of a UNION: every entry of the column, each sought to the
 * window's first container key and read up to its end, ORed into one bitset
 * image per container key.  Exact - every TID it holds is one the index holds
 * - and bounded: uwidth images, whatever the column holds.  Each window walks
 * the column's entries once; the next window starts at the smallest container
 * key any entry had past this one, so empty stretches of the heap cost
 * nothing.  Returns false when there is nothing past the last window.
 */
static bool
lion_source_union_window(LionSource *src)
{
	LionIndexState *ix;
	LionLeafWalk w;
	LionEntryTuple *entry;
	Size		itemlen;
	bool		havenext = false;
	uint64		next = PG_UINT64_MAX;
	uint64		end;
	MemoryContext oldcxt;

	if (src->udone)
		return false;
	if (!src->ufirst && src->unext == PG_UINT64_MAX)
	{
		src->udone = true;
		return false;
	}
	src->ustart = src->ufirst ? 0 : src->unext;
	src->ufirst = false;
	end = src->ustart + (uint64) src->uwidth;

	/* The state is looked up per window: see lion_source_next(). */
	ix = lion_get_index_state(src->index);
	oldcxt = MemoryContextSwitchTo(src->cxt);
	lion_walk_begin(&w, src->index, lion_column(ix, src->unionattno),
					src->withnull, NULL, 0, false);
	MemoryContextSwitchTo(oldcxt);

	while ((entry = lion_walk_next(&w, &itemlen)) != NULL)
	{
		LionPostingSet ps;
		LionSetStream *st;
		const LionContainer *c;

		oldcxt = MemoryContextSwitchTo(src->entrycxt);
		lion_source_entry_set(src->index, entry, itemlen, src->entrycxt, &ps);
		st = lion_stream_begin(1, &ps, NULL, false);
		lion_stream_seek(st, (uint32) Min(src->ustart, (uint64) PG_UINT32_MAX));
		while ((c = lion_stream_next(st)) != NULL)
		{
			uint64		k = c->ckey;

			if (k >= end)
			{
				if (k < next)
					next = k;
				havenext = true;
				break;
			}
			if (k < src->ustart)
				continue;		/* a sparse segment's keys below the seek */
			if (src->uimg[k - src->ustart] == NULL)
				src->uimg[k - src->ustart] = (uint64 *)
					MemoryContextAllocZero(src->cxt, LION_BITSET_BYTES);
			lion_container_or_into_bitset(c, src->uimg[k - src->ustart]);
			src->utouched[k - src->ustart] = true;
		}
		lion_stream_end(st);
		MemoryContextSwitchTo(oldcxt);
		MemoryContextReset(src->entrycxt);
		CHECK_FOR_INTERRUPTS();
	}
	lion_walk_end(&w);
	pfree(w.copy);

	src->unext = havenext ? next : PG_UINT64_MAX;
	src->uemit = 0;
	return true;
}

static const LionContainer *
lion_source_union_next(LionSource *src)
{
	for (;;)
	{
		while (src->uemit < src->uwidth)
		{
			int			k = src->uemit++;
			uint64	   *w = src->uimg[k];

			if (!src->utouched[k])
				continue;
			src->utouched[k] = false;
			lion_bits_to_container(w, (uint32) (src->ustart + k), src->cbuf);
			memset(w, 0, LION_BITSET_BYTES);
			if (src->cbuf->cardinality > 0)
				return src->cbuf;
		}
		if (!lion_source_union_window(src))
			return NULL;
	}
}

/*
 * One walked entry's position in a WINDOW, as its items come by in ascending
 * container key: the first slot whose key is not below the last item's.
 */
typedef struct LionWindowPos
{
	LionSource *src;
	int			p;
} LionWindowPos;

/* The slot of ckey in the window, or -1; false once ckey is past the window. */
static inline bool
lion_window_slot(LionWindowPos *pos, uint32 ckey, int *slot)
{
	LionSource *src = pos->src;

	*slot = -1;
	if (ckey > src->wkeys[src->wn - 1])
		return false;
	/* both ascend; this stops at the window's last slot at the latest */
	while (src->wkeys[pos->p] < ckey)
		pos->p++;
	if (src->wkeys[pos->p] == ckey)
	{
		*slot = pos->p;
		if (src->wwalk[*slot] == NULL)
			src->wwalk[*slot] = (uint64 *)
				MemoryContextAllocZero(src->cxt, LION_BITSET_BYTES);
		src->wtouched[*slot] = true;
	}
	return true;
}

static bool
lion_window_pair_cb(uint32 ckey, uint16 lo, void *arg)
{
	LionWindowPos *pos = (LionWindowPos *) arg;
	int			slot;

	if (!lion_window_slot(pos, ckey, &slot))
		return false;
	if (slot >= 0)
		pos->src->wwalk[slot][lo >> 6] |= UINT64CONST(1) << (lo & 63);
	return true;
}

/*
 * OR one item of a walked entry - a container, or a sparse segment of an
 * INLINE payload (DESIGN.md §13) - into the window's image at its container
 * key, where the other columns have one; anything else is none of the
 * window's business: below it (the seek's granularity, or a segment that
 * starts there) or at a key the other columns do not have.  Returns false
 * once the item is past the window, which ends the entry.
 */
static bool
lion_window_or_item(LionWindowPos *pos, const LionContainer *c)
{
	LionSource *src = pos->src;
	int			slot;

	/* A segment's header carries its first container key (§13). */
	if (c->ckey > src->wkeys[src->wn - 1])
		return false;
	if (c->type == LION_CT_SPARSE)
	{
		lion_sparse_iterate(c, lion_window_pair_cb, pos);
		return true;			/* the next item says whether it is past */
	}
	if (!lion_window_slot(pos, c->ckey, &slot))
		return false;
	if (slot >= 0)
		lion_container_or_into_bitset(c, pos->src->wwalk[slot]);
	return true;
}

/*
 * The next window of a WINDOW source (§29.3): the other columns' next wwidth
 * containers, each ORed into a bitset image of its own, and then ONE walk of
 * the range, every entry sought to the window's first container key and read
 * up to its last, ORed into a second image wherever the other columns have a
 * container.  Their AND is the answer for the window, handed out in container
 * key order by lion_source_window_next().  Returns false when the other
 * columns have no container left.
 *
 * The walk takes a directory leaf's lock only while it copies the leaf, and
 * with no posting page pinned: the other columns' stream copies every leaf it
 * reads and holds no pin between two calls (it is never opened with
 * keeppins), and each entry's stream is closed before the next leaf is read -
 * the reader side of the §11 rule, as for the UNION.
 */
static bool
lion_source_window(LionSource *src)
{
	LionIndexState *ix;
	LionState  *col;
	LionEntryTuple *entry;
	Size		itemlen;
	uint32		first;
	MemoryContext oldcxt;
	int			r;

	src->wn = 0;
	src->wemit = 0;

	/* What the rest's cursors allocate as they go lives with the source. */
	oldcxt = MemoryContextSwitchTo(src->cxt);
	while (!src->restdone && src->wn < src->wwidth)
	{
		const LionContainer *c = lion_stream_next(src->reststream);
		int			k = src->wn;

		if (c == NULL)
		{
			src->restdone = true;
			break;
		}
		if (c->cardinality == 0)
			continue;
		if (src->wrest[k] == NULL)
			src->wrest[k] = (uint64 *) palloc(LION_BITSET_BYTES);
		memset(src->wrest[k], 0, LION_BITSET_BYTES);
		lion_container_or_into_bitset(c, src->wrest[k]);
		src->wkeys[k] = c->ckey;
		src->wn++;
	}
	MemoryContextSwitchTo(oldcxt);
	if (src->wn == 0)
		return false;
	first = src->wkeys[0];

	/*
	 * The walked column's state is looked up per window, and the ranges
	 * re-pointed at it, for the reason lion_source_next() gives for a WALK.
	 */
	ix = lion_get_index_state(src->index);
	col = lion_column(ix, src->walkattno);
	for (r = 0; r < src->nranges; r++)
		src->ranges[r].state = col;
	src->ix = ix;
	src->so->ix = ix;

	MemoryContextReset(src->walkcxt);
	oldcxt = MemoryContextSwitchTo(src->walkcxt);
	lion_walk_begin(&src->walk, src->index, col, false, src->ranges,
					src->nranges, false);
	MemoryContextSwitchTo(oldcxt);

	while ((entry = lion_walk_next(&src->walk, &itemlen)) != NULL)
	{
		LionWindowPos pos;

		pos.src = src;
		pos.p = 0;

		if ((entry->flags & LION_ENTRY_INLINE) != 0)
		{
			/*
			 * Read straight out of the walk's copy of the leaf, item by item:
			 * a near-unique column's range is a run of one-row INLINE
			 * entries, and a stream set up and torn down for each of them
			 * cost more than the rest of the scan - 200k of them took 113 ms
			 * against the bitmap scan's 35, and 17 ms read this way.
			 */
			const char *payload = LionEntryGetPayload(entry);
			Size		paylen = LION_ENTRY_PAYLOAD_LEN(entry, itemlen);
			Size		off = 0;

			while (lion_inline_fetch(payload, paylen, &off, src->ibuf) > 0)
			{
				if (!lion_window_or_item(&pos, src->ibuf))
					break;
			}
		}
		else
		{
			/* A posting tree: sought to the window, one page at a time. */
			LionPostingSet ps;
			LionSetStream *st;
			const LionContainer *c;

			oldcxt = MemoryContextSwitchTo(src->entrycxt);
			lion_source_entry_set(src->index, entry, itemlen, src->entrycxt,
								  &ps);
			st = lion_stream_begin(1, &ps, NULL, false);
			lion_stream_seek(st, first);
			while ((c = lion_stream_next(st)) != NULL)
			{
				if (!lion_window_or_item(&pos, c))
					break;
			}
			lion_stream_end(st);
			MemoryContextSwitchTo(oldcxt);
			MemoryContextReset(src->entrycxt);
		}
		CHECK_FOR_INTERRUPTS();
	}
	lion_walk_end(&src->walk);
	return true;
}

static const LionContainer *
lion_source_window_next(LionSource *src)
{
	for (;;)
	{
		while (src->wemit < src->wn)
		{
			int			k = src->wemit++;
			uint64	   *walk = src->wwalk[k];
			const uint64 *rest = src->wrest[k];
			int			j;

			if (!src->wtouched[k])
				continue;
			src->wtouched[k] = false;
			for (j = 0; j < LION_BITSET_WORDS; j++)
				walk[j] &= rest[j];
			lion_bits_to_container(walk, src->wkeys[k], src->cbuf);
			memset(walk, 0, LION_BITSET_BYTES);
			if (src->cbuf->cardinality > 0)
				return src->cbuf;
		}
		if (!lion_source_window(src))
			return NULL;
	}
}

/*
 * The next container of the source, or NULL at the end.  Valid until the
 * next call, which is where the pins it was read under - if any - go.
 */
const LionContainer *
lion_source_next(LionSource *src)
{
	const LionContainer *c;

	switch (src->shape)
	{
		case LION_SRC_NONE:
			return NULL;

		case LION_SRC_SETS:
			return lion_stream_next(src->stream);

		case LION_SRC_UNION:
			return lion_source_union_next(src);

		case LION_SRC_WINDOW:
			return lion_source_window_next(src);

		case LION_SRC_LIST:
			for (;;)
			{
				if (src->stream != NULL)
				{
					c = lion_stream_next(src->stream);
					if (c != NULL)
						return c;

					/*
					 * The batch is done: its cursors' pins and its sets go,
					 * and only then is the next batch located (the §11 rule).
					 */
					lion_source_end_batch(src);
				}
				if (!lion_source_list_batch(src))
					return NULL;
				CHECK_FOR_INTERRUPTS();
			}

		case LION_SRC_WALK:

			/*
			 * The walk's column state belongs to the index's relcache entry,
			 * and a relcache invalidation replaces it - the executor may
			 * accept one between two calls, whenever it takes a lock for
			 * something else while the scan is paused.  The old state stays
			 * valid memory (LionAmCache, lion_pages.c) but is no longer the
			 * current one, so it is looked up again here, and the walk and its
			 * ranges re-pointed at the rebuilt one.  Nothing else the source holds points into it: the
			 * ranges' comparison functions are copies, and the posting-set
			 * cursors name the index and a block.
			 */
			{
				LionIndexState *ix = lion_get_index_state(src->index);

				if (ix != src->ix)
				{
					int			r;

					src->walk.col = lion_column(ix, src->walkattno);
					for (r = 0; r < src->nranges; r++)
						src->ranges[r].state = src->walk.col;
					if (src->walk.sum != NULL)
						src->walk.sum->state = src->walk.col;
					src->ix = ix;
					src->so->ix = ix;
				}
			}

			for (;;)
			{
				LionEntryTuple *entry;
				Size		itemlen;
				MemoryContext oldcxt;

				if (src->stream != NULL)
				{
					c = lion_stream_next(src->stream);
					if (c != NULL)
						return c;

					/*
					 * This entry is done: its cursors' pins go, and only then
					 * is the next directory leaf locked (the §11 rule).
					 */
					lion_stream_end(src->stream);
					src->stream = NULL;
					MemoryContextReset(src->entrycxt);
				}

				entry = lion_walk_next(&src->walk, &itemlen);
				if (entry == NULL)
					return NULL;

				/*
				 * An INLINE payload is read straight out of the walk's copy of
				 * the leaf, which stays put until this entry's stream is over;
				 * under a non-MVCC snapshot the walk keeps that leaf pinned
				 * (§29.5).
				 */
				lion_source_entry_set(src->index, entry, itemlen,
									  src->entrycxt, &src->sets[src->nsets]);

				oldcxt = MemoryContextSwitchTo(src->entrycxt);
				src->stream = lion_stream_begin(src->nsets + 1, src->sets,
												src->tree, src->keeppins);
				MemoryContextSwitchTo(oldcxt);

				CHECK_FOR_INTERRUPTS();
			}
	}
	return NULL;				/* keep compiler quiet */
}

/* Drop every pin the source holds; its memory goes with its context. */
static void
lion_source_release(LionSource *src)
{
	int			i;

	if (src->shape == LION_SRC_LIST)
		lion_source_end_batch(src);
	if (src->stream != NULL)
		lion_stream_end(src->stream);
	src->stream = NULL;
	if (src->reststream != NULL)
		lion_stream_end(src->reststream);
	src->reststream = NULL;
	if (src->shape == LION_SRC_WALK)
		lion_walk_end(&src->walk);
	for (i = 0; i < src->nsets; i++)
		lion_posting_set_release(&src->sets[i]);
	src->nsets = 0;
	src->shape = LION_SRC_NONE;
}

LionSource *
lion_source_open(Relation index, ScanKey keys, int nkeys, bool keeppins,
				 MemoryContext cxt)
{
	LionScanOpaque so;
	MemoryContext oldcxt = MemoryContextSwitchTo(cxt);

	so = (LionScanOpaque) palloc0(sizeof(LionScanOpaqueData));
	so->ix = lion_get_index_state(index);
	so->cols = (LionScanCol *) palloc0(sizeof(LionScanCol) * so->ix->ncolumns);
	so->index = index;
	so->keys = keys;
	so->nkeys = nkeys;
	MemoryContextSwitchTo(oldcxt);

	return lion_source_build(so, keeppins, cxt);
}

bool
lion_source_sorted(LionSource *src)
{
	return src->shape == LION_SRC_SETS || src->shape == LION_SRC_UNION ||
		src->shape == LION_SRC_WINDOW || src->shape == LION_SRC_NONE;
}

bool
lion_source_exact(LionSource *src)
{
	return !src->recheck;
}

void
lion_source_close(LionSource *src)
{
	LionScanOpaque so = src->so;

	lion_source_release(src);
	MemoryContextDelete(src->cxt);
	pfree(so->cols);
	pfree(so);
}

/*
 * amgettuple (DESIGN.md §29): the next TID of the source, one container's
 * members at a time.
 */
bool
liongettuple(IndexScanDesc scan, ScanDirection dir)
{
	LionScanOpaque so = (LionScanOpaque) scan->opaque;

	/* §29.1: lion is not ordered, so nothing asks for another direction. */
	if (!ScanDirectionIsForward(dir))
		elog(ERROR, "lion index \"%s\" cannot be scanned backward",
			 RelationGetRelationName(scan->indexRelation));

	if (so->src == NULL)
	{
		bool		droppin;

		if (so->srcdone)
			return false;

		/* Not under old_snapshot_threshold (DESIGN.md §9). */
		lion_check_old_snapshot(scan->indexRelation, scan->xs_snapshot);

		/* A relcache invalidation may have replaced the cached state. */
		so->ix = lion_get_index_state(scan->indexRelation);
		so->index = scan->indexRelation;
		so->keys = scan->keyData;
		so->nkeys = scan->numberOfKeys;

		pgstat_count_index_scan(scan->indexRelation);
#if PG_VERSION_NUM >= 180000
		if (scan->instrument)
			scan->instrument->nsearches++;
#endif

		if (so->gtcxt == NULL)
			so->gtcxt = AllocSetContextCreate(GetMemoryChunkContext(so),
											  "lion index scan state",
											  ALLOCSET_SMALL_SIZES);
		if (so->lo == NULL)
			so->lo = (uint16 *) MemoryContextAlloc(GetMemoryChunkContext(so),
												   sizeof(uint16) * LION_CONTAINER_RANGE);

		/*
		 * An index-only scan (DESIGN.md §29.9).  lion returns no column
		 * (amcanreturn is NULL), so the planner builds one only for a query
		 * that needs no column at all - `SELECT count(*) FROM t`, which
		 * amoptionalkey lets it answer from a lion index with no key - and
		 * nothing ever reads what the tuple holds.  It still has to BE one:
		 * a tuple of NULLs of the index's own shape.
		 */
		if (scan->xs_want_itup && so->nullitup == NULL)
		{
			TupleDesc	desc = RelationGetDescr(scan->indexRelation);
			Datum		values[INDEX_MAX_KEYS];
			bool		isnull[INDEX_MAX_KEYS];
			MemoryContext oldcxt;
			int			i;

			for (i = 0; i < desc->natts; i++)
			{
				values[i] = (Datum) 0;
				isnull[i] = true;
			}
			oldcxt = MemoryContextSwitchTo(GetMemoryChunkContext(so));
			so->nullitup = index_form_tuple(desc, values, isnull);
			MemoryContextSwitchTo(oldcxt);
		}

		/*
		 * nbtree's dropPin rule (nbtree.c, btrescan): an MVCC snapshot needs
		 * no pin, anything else keeps the page each batch came from pinned
		 * until the next batch (DESIGN.md §29.5).
		 */
		droppin = LION_IS_MVCC_LIKE(scan->xs_snapshot) &&
			!scan->xs_want_itup && scan->heapRelation != NULL;

		so->src = lion_source_build(so, !droppin, so->gtcxt);
		so->nlo = 0;
		so->pos = 0;

		/*
		 * Which TIDs an index-only scan has to look up in the heap before it
		 * hands them on (see below).  The UNION shape's, always.  And on a hot
		 * standby every shape's, when the index is in generic WAL mode
		 * (DESIGN.md §9, "Hot standby"): replay of a generic record takes no
		 * cleanup lock, so the pin a batch keeps does not stop the startup
		 * process from removing its TIDs and then replaying the heap records
		 * that set their pages all-visible - the executor would trust the
		 * visibility map for a row that is dead to this snapshot.  The count
		 * rechecks everything there for the same reason (cx.in_recovery in
		 * lion_count.c).  An rmgr-mode index keeps the interlock on the
		 * standby, because its removals replay under a cleanup lock behind
		 * the barrier of §25.
		 */
		so->heapcheck = scan->xs_want_itup &&
			(so->src->shape == LION_SRC_UNION ||
			 (RecoveryInProgress() &&
			  lion_wal_mode(scan->indexRelation) != LION_WAL_MODE_RMGR));
	}

	for (;;)
	{
		const LionContainer *c;

		if (so->pos < so->nlo)
		{
			uint16		blkinc;
			OffsetNumber off;

			lion_lo_split(so->lo[so->pos++], &blkinc, &off);
			ItemPointerSet(&scan->xs_heaptid, so->firstblk + blkinc, off);
			scan->xs_recheck = so->src->recheck;

			if (scan->xs_want_itup)
			{
				/*
				 * An index-only scan trusts the visibility map for every TID
				 * it is handed.  The SETS, LIST and WALK shapes keep the page
				 * each batch came from pinned until the next one
				 * (xs_want_itup turned dropPin off), which is the §9
				 * interlock the count relies on.  The UNION shape copies
				 * containers out of many pages into its window and pins none
				 * of them, and on a hot standby a generic-mode index's pins
				 * interlock nothing (so->heapcheck): those TIDs are checked
				 * in the heap here, and only a tuple visible to the snapshot
				 * is handed on - which stays visible to it, so neither VACUUM
				 * nor replay can take it away before the executor looks.
				 * Every TID is one the index holds (§29.6), so a visible one
				 * is a row the scan selects.  A WINDOW, which pins nothing
				 * either, is only ever built without keeppins, so it never
				 * gets here.
				 */
				Assert(so->src->shape != LION_SRC_WINDOW);
				if (so->heapcheck)
				{
					ItemPointerData tid = scan->xs_heaptid;

					if (!lion_table_fetch_tid(scan->heapRelation, &tid,
											  scan->xs_snapshot, NULL))
						continue;
				}
				scan->xs_itup = so->nullitup;
			}
			return true;
		}

		c = lion_source_next(so->src);
		if (c == NULL)
		{
			lion_source_release(so->src);
			so->src = NULL;
			so->srcdone = true;
			return false;
		}

		so->firstblk = lion_ckey_first_block(c->ckey);
		so->nlo = (int) lion_container_to_array(c, so->lo);
		so->pos = 0;

		/*
		 * Test hook: a batch has just been loaded and none of it returned.
		 * Under a non-MVCC snapshot the page it came from is pinned here;
		 * under an MVCC one nothing is.  test/isolation/gettuple_dirty_pin.spec
		 * parks scans of both kinds at this point.
		 */
		LION_INJECTION_POINT("lion-gettuple-batch");
	}
}
