/*-------------------------------------------------------------------------
 *
 * lion_vec_loc.c
 *		lionvec's locator: where each row's vector lies in TOAST, so that a
 *		scan reads it from its chunks' pages directly, past the heap page
 *		and the TOAST index (DESIGN.md §44.13).
 *
 * The heap keeps a vector of more than 506 dimensions in TOAST unless told
 * otherwise, and a fetch of one reads the row's heap page for its TOAST
 * pointer, descends the TOAST index for its chunks, and reads their pages:
 * three reads, each of which only the one before it names.  An index that
 * keeps a locator knows the third without the first two: for each row whose
 * vector lies in TOAST it keeps the value's id and its chunks' places - a
 * block and line pointer of the TOAST relation and how many chunks follow
 * there, a RUN, one in the common case (lion_vec_math.h) - in an entry on a
 * LOC page, which the exact path reads in heap order beside its members,
 * and, in an index of vectors wide enough, in a slot of the row's REFINE
 * item too, which the probed path's rerank reads.  A place is only ever a
 * hint: every chunk read there is checked to be the chunk the value's id
 * and its sequence call for, as heap_fetch_toast_slice() checks the chunks
 * the TOAST index finds, and a row read so is returned only once its heap
 * tuple, fetched under the scan's snapshot, shows the same value.
 *
 * This file holds what the build, inserts, VACUUM and the scan share: the
 * TOAST lookups that make an entry, the reading of a run's chunks, the LOC
 * and LOCDIR pages' accessors, the build's writer of them, an insert's
 * append, and VACUUM's pass over them.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/detoast.h"
#include "access/generic_xlog.h"
#include "access/heapam.h"
#include "access/heaptoast.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/toast_internals.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "varatt.h"

#include "lion_compat.h"
#include "lion_vec.h"

StaticAssertDecl(sizeof(LionVecLocRun) == 8,
				 "lionvec: a run is two words");
StaticAssertDecl(offsetof(LionVecLoc, runs) == 12,
				 "lionvec: a LOC entry's header is three words");
StaticAssertDecl(sizeof(LionVecLocDir) == 12,
				 "lionvec: a LOCDIR item is three words");

/* ----------------------------------------------------------------
 * The heap's TOAST relation, and a value's chunks through its index
 * ----------------------------------------------------------------
 */

/*
 * Open the TOAST relation of heap and its indexes, for an index of dims
 * dimensions (half: of halfvec), whose values have a locator entry when
 * stored in TOAST as extsize bytes, uncompressed; false, with nothing
 * opened, if the heap has no TOAST relation, its TOAST relation's ids are
 * not OIDs, or its values would take more chunks than an entry holds.  Locked as a detoast locks them, until the
 * transaction ends; what it allocates is in the caller's memory context,
 * which must last until lionvec_toast_close().
 */
bool
lionvec_toast_open(LionVecToast *t, Relation heap, int dims, bool half)
{
	memset(t, 0, sizeof(*t));
	if (heap == NULL || dims <= 0 || !OidIsValid(heap->rd_rel->reltoastrelid))
		return false;
	t->extsize = lionvec_toast_extsize(dims, half);
	t->nchunks = lionvec_toast_nchunks(t->extsize, LION_TOAST_MAX_CHUNK_SIZE);
	if (t->nchunks < 1 || t->nchunks > LIONVEC_LOC_MAXRUNS)
		return false;
	t->dims = dims;
	t->half = half;
	t->toastrelid = heap->rd_rel->reltoastrelid;
	t->toastrel = table_open(t->toastrelid, AccessShareLock);
	/* PostgreSQL 20's TOAST relations of 8-byte ids have no entries */
	if (TupleDescAttr(RelationGetDescr(t->toastrel), 0)->atttypid != OIDOID)
	{
		table_close(t->toastrel, NoLock);
		memset(t, 0, sizeof(*t));
		return false;
	}
	t->valid = toast_open_indexes(t->toastrel, AccessShareLock, &t->idxs,
								  &t->nidx);
	return true;
}

void
lionvec_toast_close(LionVecToast *t)
{
	if (t->idxs != NULL)
		toast_close_indexes(t->idxs, t->nidx, AccessShareLock);
	if (t->toastrel != NULL)
		table_close(t->toastrel, NoLock);
	memset(t, 0, sizeof(*t));
}

/*
 * Whether d, a stored datum, is a value that can have an entry: on disk in
 * the heap's TOAST relation, uncompressed, of the dimensions' length; its
 * pointer into *ptr.  A NULL, a vector inline, compressed (EXTENDED
 * storage's pglz or lz4), or anything else has none, and is read as it
 * always was.
 */
bool
lionvec_toast_value(const LionVecToast *t, Datum d, LionToastPointer *ptr)
{
	struct varlena *v = (struct varlena *) DatumGetPointer(d);

	if (t->toastrel == NULL || !lion_toast_ondisk_oid(v))
		return false;
	VARATT_EXTERNAL_GET_POINTER(*ptr, v);
	return lionvec_toast_ptr_ok(t, ptr);
}

/*
 * ... and whether a TOAST pointer is one: its stored bytes the raw value's
 * less the header, which is VARATT_EXTERNAL_IS_COMPRESSED() false, and the
 * dimensions' length
 */
bool
lionvec_toast_ptr_ok(const LionVecToast *t, const LionToastPointer *ptr)
{
	return t->toastrel != NULL &&
		ptr->va_toastrelid == t->toastrelid &&
		(int64) LION_TOAST_EXTSIZE(*ptr) == t->extsize &&
		(int64) ptr->va_rawsize == t->extsize + VARHDRSZ;
}

/*
 * The chunk of a TOAST tuple: its sequence number and data, checked as
 * heap_fetch_toast_slice() checks them - a plain or short varlena, never
 * compressed or external - and, as a place read directly may hold any
 * tuple, within the tuple's own bytes.  False if not so.
 */
static bool
lionvec_chunk(HeapTuple tup, TupleDesc desc, Oid *valueid, int32 *seq,
			  char **data, int32 *size)
{
	bool		isnull;
	char	   *chunk;
	char	   *end = (char *) tup->t_data + tup->t_len;

	if (tup->t_len < SizeofHeapTupleHeader ||
		tup->t_data->t_hoff > tup->t_len ||
		HeapTupleHeaderGetNatts(tup->t_data) != 3 || HeapTupleHasNulls(tup) ||
		HeapTupleHasExternal(tup))
		return false;
	*valueid = DatumGetObjectId(fastgetattr(tup, 1, desc, &isnull));
	*seq = DatumGetInt32(fastgetattr(tup, 2, desc, &isnull));
	chunk = (char *) DatumGetPointer(fastgetattr(tup, 3, desc, &isnull));
	if (chunk < (char *) tup->t_data || chunk >= end)
		return false;
	if (!VARATT_IS_EXTENDED(chunk))
	{
		if (chunk + VARHDRSZ > end || VARSIZE(chunk) < VARHDRSZ ||
			chunk + VARSIZE(chunk) > end)
			return false;
		*size = VARSIZE(chunk) - VARHDRSZ;
		*data = VARDATA(chunk);
	}
	else if (VARATT_IS_SHORT(chunk))
	{
		if (VARSIZE_SHORT(chunk) < VARHDRSZ_SHORT ||
			chunk + VARSIZE_SHORT(chunk) > end)
			return false;
		*size = VARSIZE_SHORT(chunk) - VARHDRSZ_SHORT;
		*data = VARDATA_SHORT(chunk);
	}
	else
		return false;
	return true;
}

/*
 * A value's chunks, found through the TOAST index as heap_fetch_toast_slice()
 * finds them - under the TOAST snapshot, in chunk_seq order, every one of
 * 0 to nchunks - 1 once, each LION_TOAST_MAX_CHUNK_SIZE but the last - copied into
 * data (extsize bytes; NULL for none), and their places, as runs, into loc
 * with the value's id (loc->tid is the caller's).  False if anything is not
 * so, or the places take more runs than the value has chunks: the row then
 * has no entry, and is read as it always was, where core's own detoast
 * raises whatever error is due.
 */
bool
lionvec_toast_locate(LionVecToast *t, const LionToastPointer *ptr,
					 char *data, LionVecLoc *loc)
{
	TupleDesc	desc = RelationGetDescr(t->toastrel);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	ttup;
	SnapshotData tsnapdata;
	uint32		blks[LIONVEC_LOC_MAXRUNS];
	uint16		offs[LIONVEC_LOC_MAXRUNS];
	int			expected = 0;
	bool		ok = true;
	int			nruns;

	ScanKeyInit(&key, (AttrNumber) 1, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(ptr->va_valueid));
	scan = systable_beginscan_ordered(t->toastrel, t->idxs[t->valid],
									  lion_toast_snapshot(&tsnapdata), 1, &key);
	while ((ttup = systable_getnext_ordered(scan, ForwardScanDirection)) != NULL)
	{
		Oid			id;
		int32		seq;
		char	   *cdata;
		int32		csize;

		if (expected >= t->nchunks ||
			!lionvec_chunk(ttup, desc, &id, &seq, &cdata, &csize) ||
			id != ptr->va_valueid || seq != expected ||
			csize != lionvec_toast_chunk_size(t->extsize, LION_TOAST_MAX_CHUNK_SIZE, seq) ||
			!ItemPointerIsValid(&ttup->t_self))
		{
			ok = false;
			break;
		}
		blks[seq] = ItemPointerGetBlockNumber(&ttup->t_self);
		offs[seq] = ItemPointerGetOffsetNumber(&ttup->t_self);
		if (data != NULL)
			memcpy(data + (Size) seq * LION_TOAST_MAX_CHUNK_SIZE, cdata, csize);
		expected++;
	}
	systable_endscan_ordered(scan);
	if (!ok || expected != t->nchunks)
		return false;
	nruns = lionvec_loc_runs(blks, offs, t->nchunks, loc->runs, t->nchunks);
	if (nruns < 1)
		return false;
	loc->nruns = (uint8) nruns;
	loc->flags = 0;
	loc->valueid = ptr->va_valueid;
	return true;
}

/*
 * The TOAST pointer of the vector the heap stores at tid, an insert's row
 * (DESIGN.md §44.13, "Inserts"): the executor hands aminsert() the value it
 * inserted, which heap_insert() toasted a copy of - and from INSERT ...
 * SELECT, another row's pointer, which the heap detoasted and stored anew -
 * so the place is read off the tuple the heap wrote.  That tuple must be a
 * normal one and the end of its chain, as an insert's is; a HOT chain's
 * root that a later version follows, which CREATE INDEX CONCURRENTLY's
 * validate_index() passes, may hold another value, and has no entry.
 */
bool
lionvec_stored_pointer(Relation heap, ItemPointer tid, AttrNumber attno,
					   LionToastPointer *ptr)
{
	Buffer		buf;
	Page		page;
	OffsetNumber off = ItemPointerGetOffsetNumber(tid);
	bool		found = false;

	buf = ReadBuffer(heap, ItemPointerGetBlockNumber(tid));
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	if (!PageIsNew(page) && off >= FirstOffsetNumber &&
		off <= PageGetMaxOffsetNumber(page))
	{
		ItemId		lp = PageGetItemId(page, off);

		if (ItemIdIsNormal(lp))
		{
			HeapTupleData tup;

			tup.t_data = (HeapTupleHeader) PageGetItem(page, lp);
			tup.t_len = ItemIdGetLength(lp);
			tup.t_tableOid = RelationGetRelid(heap);
			tup.t_self = *tid;
			if (!HeapTupleHeaderIsHotUpdated(tup.t_data))
			{
				bool		isnull;
				Datum		d = heap_getattr(&tup, attno, RelationGetDescr(heap),
											 &isnull);

				if (!isnull && lion_toast_ondisk_oid(DatumGetPointer(d)))
				{
					VARATT_EXTERNAL_GET_POINTER(*ptr, DatumGetPointer(d));
					found = true;
				}
			}
		}
	}
	UnlockReleaseBuffer(buf);
	return found;
}

/* ----------------------------------------------------------------
 * Reading a value by its places
 * ----------------------------------------------------------------
 */

/*
 * The chunks of run, the value's chunks seq0 on, from buf - the TOAST
 * relation's block run->blk, pinned and share-locked by the caller - into
 * data, each checked: its line pointer on the page and normal, its tuple
 * visible to the TOAST snapshot tsnap (as the TOAST index's own fetch of it
 * would be), a TOAST tuple's three columns, its chunk_id the value's id,
 * its chunk_seq the one its place in the runs calls for, its data a plain
 * or short varlena of the length core's checks give that chunk.  False at
 * the first that fails.
 */
bool
lionvec_place_run(Relation toastrel, Snapshot tsnap, Buffer buf,
				  const LionVecLocRun *run, Oid valueid, int seq0,
				  int64 extsize, char *data)
{
	Page		page = BufferGetPage(buf);
	TupleDesc	desc = RelationGetDescr(toastrel);
	OffsetNumber maxoff;
	int			nchunks = lionvec_toast_nchunks(extsize, LION_TOAST_MAX_CHUNK_SIZE);
	int			j;

	if (PageIsNew(page) || PageGetSpecialSize(page) != 0)
		return false;
	maxoff = PageGetMaxOffsetNumber(page);
	for (j = 0; j < run->n; j++)
	{
		OffsetNumber off = run->off + j;
		int			seq = seq0 + j;
		ItemId		lp;
		HeapTupleData tup;
		Oid			id;
		int32		cseq;
		char	   *cdata;
		int32		csize;

		if (off < FirstOffsetNumber || off > maxoff || seq >= nchunks)
			return false;
		lp = PageGetItemId(page, off);
		if (!ItemIdIsNormal(lp))
			return false;
		tup.t_data = (HeapTupleHeader) PageGetItem(page, lp);
		tup.t_len = ItemIdGetLength(lp);
		tup.t_tableOid = RelationGetRelid(toastrel);
		ItemPointerSet(&tup.t_self, run->blk, off);
		if (tup.t_len < SizeofHeapTupleHeader ||
			!HeapTupleSatisfiesVisibility(&tup, tsnap, buf) ||
			!lionvec_chunk(&tup, desc, &id, &cseq, &cdata, &csize) ||
			id != valueid || cseq != seq ||
			csize != lionvec_toast_chunk_size(extsize, LION_TOAST_MAX_CHUNK_SIZE, seq))
			return false;
		memcpy(data + (Size) seq * LION_TOAST_MAX_CHUNK_SIZE, cdata, csize);
	}
	return true;
}

/*
 * A value read by its runs, a block at a time - the probed path's rerank,
 * whose blocks were asked for ahead (lion_vec_scan.c) - into data; false at
 * the first chunk that fails, or a block at or past nblocks, the TOAST
 * relation's size as counted under the lock that keeps it from shrinking.
 */
bool
lionvec_place_read(Relation toastrel, Snapshot tsnap, BlockNumber nblocks,
				   Oid valueid, const LionVecLocRun *runs, int nruns,
				   int64 extsize, char *data)
{
	int			nchunks = lionvec_toast_nchunks(extsize, LION_TOAST_MAX_CHUNK_SIZE);
	int			seq = 0;
	int			i;

	if (!lionvec_loc_runs_ok(runs, nruns, nchunks, MaxHeapTuplesPerPage))
		return false;
	for (i = 0; i < nruns; i++)
	{
		Buffer		buf;
		bool		ok;

		if (runs[i].blk >= nblocks)
			return false;
		buf = ReadBuffer(toastrel, runs[i].blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		ok = lionvec_place_run(toastrel, tsnap, buf, &runs[i], valueid, seq,
							   extsize, data);
		UnlockReleaseBuffer(buf);
		if (!ok)
			return false;
		seq += runs[i].n;
	}
	return seq == nchunks;
}

/* ----------------------------------------------------------------
 * LOC and LOCDIR pages
 * ----------------------------------------------------------------
 */

/*
 * A LOC entry, checked: its length against its runs, its runs against the
 * value's nchunks chunks and a heap page's line pointers, its TID and value
 * id valid, its flags 0.  ERRCODE_INDEX_CORRUPTED if not: the scan reads
 * nothing of a malformed entry.
 */
LionVecLoc *
lionvec_loc_at(Relation index, Page page, BlockNumber blk, OffsetNumber off,
			   int nchunks)
{
	ItemId		iid = PageGetItemId(page, off);
	LionVecLoc *loc;
	Size		len;

	if (!ItemIdIsNormal(iid) || ItemIdGetLength(iid) < LIONVEC_LOC_SIZE(1))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has a malformed locator entry at (%u,%u)",
						RelationGetRelationName(index), blk, off)));
	loc = (LionVecLoc *) PageGetItem(page, iid);
	len = ItemIdGetLength(iid);
	if (loc->flags != 0 || loc->nruns < 1 || len != LIONVEC_LOC_SIZE(loc->nruns) ||
		!ItemPointerIsValid(&loc->tid) || !OidIsValid(loc->valueid) ||
		!lionvec_loc_runs_ok(loc->runs, loc->nruns, nchunks, MaxHeapTuplesPerPage))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has a malformed locator entry of %zu bytes at (%u,%u)",
						RelationGetRelationName(index), len, blk, off)));
	return loc;
}

/* A LOCDIR item, checked: its length, a LOC page's block, and its range. */
static LionVecLocDir *
lionvec_locdir_at(Relation index, Page page, BlockNumber blk, OffsetNumber off)
{
	ItemId		iid = PageGetItemId(page, off);
	LionVecLocDir *d;

	if (!ItemIdIsNormal(iid) || ItemIdGetLength(iid) != sizeof(LionVecLocDir))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has a malformed locator directory item at (%u,%u)",
						RelationGetRelationName(index), blk, off)));
	d = (LionVecLocDir *) PageGetItem(page, iid);
	if (!BlockNumberIsValid(d->blk) || d->blk == LIONVEC_METAPAGE ||
		(d->lo > d->hi && !(d->lo == InvalidBlockNumber && d->hi == 0)))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has a malformed locator directory item at (%u,%u)",
						RelationGetRelationName(index), blk, off)));
	return d;
}

/* A page's range of heap blocks, in its special space, widened to blk. */
static void
lionvec_note_block(Page page, BlockNumber lo, BlockNumber hi)
{
	LionVecPageOpaque op = LionVecPageGetOpaque(page);

	if (lo > hi)
		return;
	op->minkey = Min(op->minkey, lo);
	op->maxkey = Max(op->maxkey, hi);
}

/*
 * Every LOCDIR item of the index, in the chain's order: the build's first,
 * in heap order, then the tail's and the inserts' pages'.  palloc'd; *n of
 * them.  None for an index without a locator.
 */
LionVecLocDir *
lionvec_locdir_read(Relation index, const LionVecMetaData *meta, int *n)
{
	BlockNumber blk = meta->loc_head;
	BlockNumber nblocks = RelationGetNumberOfBlocks(index);
	uint64		visited = 0;
	int			max = 64;
	LionVecLocDir *items = palloc(sizeof(LionVecLocDir) * max);

	*n = 0;
	if ((meta->flags & LIONVEC_META_LOCATOR) == 0)
		return items;
	while (BlockNumberIsValid(blk))
	{
		Buffer		buf;
		Page		page;
		OffsetNumber off;
		OffsetNumber maxoff;

		CHECK_FOR_INTERRUPTS();
		lionvec_chain_step(index, &visited, &nblocks);
		buf = ReadBuffer(index, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		lionvec_check_page(index, page, blk, LIONVEC_PAGE_LOCDIR);
		maxoff = PageGetMaxOffsetNumber(page);
		for (off = FirstOffsetNumber; off <= maxoff; off = OffsetNumberNext(off))
		{
			if (*n >= max)
			{
				if ((Size) max > MaxAllocSize / sizeof(LionVecLocDir) / 2)
					elog(ERROR, "lionvec: too many locator pages");
				max *= 2;
				items = repalloc(items, sizeof(LionVecLocDir) * max);
			}
			items[(*n)++] = *lionvec_locdir_at(index, page, blk, off);
		}
		blk = LionVecPageGetOpaque(page)->next;
		UnlockReleaseBuffer(buf);
	}
	return items;
}

/* ----------------------------------------------------------------
 * The build's LOC and LOCDIR pages
 * ----------------------------------------------------------------
 */

/* Start writing the build's entries, which come in heap order. */
void
lionvec_loc_writer_begin(LionVecLocWriter *w, Relation index)
{
	memset(w, 0, sizeof(*w));
	w->index = index;
	w->buf = InvalidBuffer;
	w->maxdir = 64;
	w->dir = palloc(sizeof(LionVecLocDir) * w->maxdir);
}

/* The LOC page being filled is done: its LOCDIR item, its buffer let go. */
static void
lionvec_loc_writer_page_done(LionVecLocWriter *w)
{
	Page		page;
	LionVecPageOpaque op;

	if (!BufferIsValid(w->buf))
		return;
	page = BufferGetPage(w->buf);
	op = LionVecPageGetOpaque(page);
	if (w->ndir >= w->maxdir)
	{
		if ((Size) w->maxdir > MaxAllocSize / sizeof(LionVecLocDir) / 2)
			elog(ERROR, "lionvec: too many locator pages");
		w->maxdir *= 2;
		w->dir = repalloc(w->dir, sizeof(LionVecLocDir) * w->maxdir);
	}
	w->dir[w->ndir].blk = BufferGetBlockNumber(w->buf);
	w->dir[w->ndir].lo = op->minkey <= op->maxkey ? op->minkey : InvalidBlockNumber;
	w->dir[w->ndir].hi = op->minkey <= op->maxkey ? op->maxkey : 0;
	w->ndir++;
	MarkBufferDirty(w->buf);
	UnlockReleaseBuffer(w->buf);
	w->buf = InvalidBuffer;
}

static void
lionvec_loc_writer_new_page(LionVecLocWriter *w)
{
	lionvec_loc_writer_page_done(w);
	w->buf = lionvec_new_buffer(w->index, MAIN_FORKNUM);
	lionvec_init_page(BufferGetPage(w->buf), LIONVEC_PAGE_LOC);
}

/* An entry, after those of lesser TIDs. */
void
lionvec_loc_writer_put(LionVecLocWriter *w, const LionVecLoc *loc)
{
	Size		len = LIONVEC_LOC_SIZE(loc->nruns);
	BlockNumber hblk = ItemPointerGetBlockNumber(&loc->tid);
	Page		page;

	if (ItemPointerIsValid(&w->last) &&
		ItemPointerCompare(&w->last, (ItemPointer) &loc->tid) >= 0)
		elog(ERROR, "lionvec: the build's locator entries are out of order");
	w->last = loc->tid;
	if (!BufferIsValid(w->buf) ||
		PageGetFreeSpace(BufferGetPage(w->buf)) < MAXALIGN(len))
		lionvec_loc_writer_new_page(w);
	page = BufferGetPage(w->buf);
	if (PageAddItemExtended(page, loc, len, InvalidOffsetNumber, 0) ==
		InvalidOffsetNumber)
		elog(ERROR, "lionvec: could not add a locator entry of %zu bytes", len);
	lionvec_note_block(page, hblk, hblk);
	w->nentries++;
}

/*
 * The last LOC page done, then the page inserts append to, empty, then the
 * LOCDIR chain: its items in the order of their pages, filling its pages,
 * each page's range of heap blocks in its special space.  Fills in meta's
 * locator fields.  The build's items, in heap order with disjoint ranges,
 * are the first loc_built, and fill loc_bpages pages from loc_head, the
 * static level a later version may search instead of reading it whole
 * (DESIGN.md §44.13, "A second level").
 */
void
lionvec_loc_writer_end(LionVecLocWriter *w, LionVecMetaData *meta)
{
	Buffer		dbuf = InvalidBuffer;
	BlockNumber head = InvalidBlockNumber;
	BlockNumber tail;
	int			built;
	int			i;

	lionvec_loc_writer_page_done(w);
	built = w->ndir;
	/* the inserts' first page */
	lionvec_loc_writer_new_page(w);
	tail = BufferGetBlockNumber(w->buf);
	lionvec_loc_writer_page_done(w);

	meta->loc_bpages = 0;
	for (i = 0; i < w->ndir; i++)
	{
		Page		page;

		if (!BufferIsValid(dbuf) ||
			PageGetFreeSpace(BufferGetPage(dbuf)) < MAXALIGN(sizeof(LionVecLocDir)))
		{
			Buffer		next = lionvec_new_buffer(w->index, MAIN_FORKNUM);

			lionvec_init_page(BufferGetPage(next), LIONVEC_PAGE_LOCDIR);
			if (BufferIsValid(dbuf))
			{
				LionVecPageGetOpaque(BufferGetPage(dbuf))->next =
					BufferGetBlockNumber(next);
				MarkBufferDirty(dbuf);
				UnlockReleaseBuffer(dbuf);
			}
			else
				head = BufferGetBlockNumber(next);
			dbuf = next;
			if (i < built)
				meta->loc_bpages++;
		}
		page = BufferGetPage(dbuf);
		if (PageAddItemExtended(page, &w->dir[i], sizeof(LionVecLocDir),
								InvalidOffsetNumber, 0) == InvalidOffsetNumber)
			elog(ERROR, "lionvec: could not add a locator directory item");
		lionvec_note_block(page, w->dir[i].lo, w->dir[i].hi);
	}
	Assert(BufferIsValid(dbuf));
	meta->loc_head = head;
	meta->loc_dtail = BufferGetBlockNumber(dbuf);
	MarkBufferDirty(dbuf);
	UnlockReleaseBuffer(dbuf);
	meta->loc_tail = tail;
	meta->loc_built = (uint32) built;
	meta->loc_root = InvalidBlockNumber;
	pfree(w->dir);
	w->dir = NULL;
}

/* ----------------------------------------------------------------
 * An insert's entry
 * ----------------------------------------------------------------
 */

/*
 * Append an insert's entry to the LOC tail, and widen the tail's LOCDIR
 * item - the last of the LOCDIR chain - to its heap block: one generic WAL
 * record of the two pages.  When the tail is full, the entry starts a new
 * LOC page instead, whose item goes on the LOCDIR chain's last page, and
 * the meta page names it the tail: a record of the new page, the LOCDIR
 * page and the meta page; and when that LOCDIR page is full too, a new one
 * after it, in the same record, four pages.  metabuf is the meta page,
 * exclusively locked by the caller throughout, which serializes inserts;
 * meta its contents.  The tail and the LOCDIR page are locked in that
 * order, after the meta page; VACUUM and scans lock one page at a time.
 */
void
lionvec_loc_append(Relation index, Buffer metabuf, const LionVecMetaData *meta,
				   const LionVecLoc *loc)
{
	Size		len = LIONVEC_LOC_SIZE(loc->nruns);
	BlockNumber hblk = ItemPointerGetBlockNumber(&loc->tid);
	Buffer		lbuf;
	Buffer		dbuf;
	Buffer		nbuf = InvalidBuffer;
	Buffer		ndbuf = InvalidBuffer;
	Page		page;
	GenericXLogState *state;
	OffsetNumber doff = InvalidOffsetNumber;

	Assert((meta->flags & LIONVEC_META_LOCATOR) != 0);
	lbuf = ReadBuffer(index, meta->loc_tail);
	LockBuffer(lbuf, BUFFER_LOCK_EXCLUSIVE);
	lionvec_check_page(index, BufferGetPage(lbuf), meta->loc_tail, LIONVEC_PAGE_LOC);
	dbuf = ReadBuffer(index, meta->loc_dtail);
	LockBuffer(dbuf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(dbuf);
	lionvec_check_page(index, page, meta->loc_dtail, LIONVEC_PAGE_LOCDIR);
	if (BlockNumberIsValid(LionVecPageGetOpaque(page)->next))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has a locator directory whose tail at block %u goes on",
						RelationGetRelationName(index), meta->loc_dtail)));

	if (PageGetFreeSpace(BufferGetPage(lbuf)) < MAXALIGN(len))
	{
		nbuf = lionvec_new_buffer(index, MAIN_FORKNUM);
		if (PageGetFreeSpace(page) < MAXALIGN(sizeof(LionVecLocDir)))
			ndbuf = lionvec_new_buffer(index, MAIN_FORKNUM);
	}
	else
	{
		/* the tail's item: the last of the chain */
		doff = PageGetMaxOffsetNumber(page);
		if (doff < FirstOffsetNumber ||
			lionvec_locdir_at(index, page, meta->loc_dtail, doff)->blk != meta->loc_tail)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("lionvec index \"%s\" has no locator directory item for its tail at block %u",
							RelationGetRelationName(index), meta->loc_tail)));
	}

	state = GenericXLogStart(index);
	if (!BufferIsValid(nbuf))
	{
		LionVecLocDir *d;

		page = GenericXLogRegisterBuffer(state, lbuf, 0);
		if (PageAddItemExtended(page, loc, len, InvalidOffsetNumber, 0) ==
			InvalidOffsetNumber)
			elog(ERROR, "lionvec: could not add a locator entry of %zu bytes", len);
		lionvec_note_block(page, hblk, hblk);
		page = GenericXLogRegisterBuffer(state, dbuf, 0);
		d = (LionVecLocDir *) PageGetItem(page, PageGetItemId(page, doff));
		if (d->lo == InvalidBlockNumber && d->hi == 0)
			d->lo = d->hi = hblk;
		else
		{
			d->lo = Min(d->lo, hblk);
			d->hi = Max(d->hi, hblk);
		}
		lionvec_note_block(page, hblk, hblk);
	}
	else
	{
		LionVecLocDir d;
		LionVecMeta m;

		page = GenericXLogRegisterBuffer(state, nbuf, GENERIC_XLOG_FULL_IMAGE);
		lionvec_init_page(page, LIONVEC_PAGE_LOC);
		if (PageAddItemExtended(page, loc, len, InvalidOffsetNumber, 0) ==
			InvalidOffsetNumber)
			elog(ERROR, "lionvec: could not add a locator entry of %zu bytes", len);
		lionvec_note_block(page, hblk, hblk);

		d.blk = BufferGetBlockNumber(nbuf);
		d.lo = d.hi = hblk;
		page = GenericXLogRegisterBuffer(state, dbuf, 0);
		if (BufferIsValid(ndbuf))
		{
			LionVecPageGetOpaque(page)->next = BufferGetBlockNumber(ndbuf);
			page = GenericXLogRegisterBuffer(state, ndbuf, GENERIC_XLOG_FULL_IMAGE);
			lionvec_init_page(page, LIONVEC_PAGE_LOCDIR);
		}
		if (PageAddItemExtended(page, &d, sizeof(d), InvalidOffsetNumber, 0) ==
			InvalidOffsetNumber)
			elog(ERROR, "lionvec: could not add a locator directory item");
		lionvec_note_block(page, hblk, hblk);

		page = GenericXLogRegisterBuffer(state, metabuf, 0);
		m = LionVecPageGetMeta(page);
		m->loc_tail = BufferGetBlockNumber(nbuf);
		if (BufferIsValid(ndbuf))
			m->loc_dtail = BufferGetBlockNumber(ndbuf);
	}
	GenericXLogFinish(state);

	if (BufferIsValid(ndbuf))
		UnlockReleaseBuffer(ndbuf);
	if (BufferIsValid(nbuf))
		UnlockReleaseBuffer(nbuf);
	UnlockReleaseBuffer(dbuf);
	UnlockReleaseBuffer(lbuf);
}

/* ----------------------------------------------------------------
 * VACUUM
 * ----------------------------------------------------------------
 */

static int
lionvec_locdir_blk_cmp(const void *a, const void *b)
{
	BlockNumber x = ((const LionVecLocDir *) a)->blk;
	BlockNumber y = ((const LionVecLocDir *) b)->blk;

	return x < y ? -1 : (x > y ? 1 : 0);
}

/*
 * Remove the entries of dead rows from every LOC page, in block order, in
 * the same ambulkdelete() as their CODE items, so that an entry leaves with
 * its TID before the heap gives the TID to another row: a generic WAL
 * record a page that loses any.  No range is narrowed: a reader that passes
 * a page over by a range too wide loses nothing, one too narrow would miss
 * the entries an insert adds meanwhile.  Pages are never recycled; an
 * emptied one stays where its LOCDIR item names it.
 */
void
lionvec_vacuum_locs(Relation index, const LionVecMetaData *meta, int nchunks,
					IndexBulkDeleteCallback callback, void *callback_state,
					BufferAccessStrategy strategy)
{
	int			n;
	LionVecLocDir *dir = lionvec_locdir_read(index, meta, &n);
	int			i;

	qsort(dir, n, sizeof(LionVecLocDir), lionvec_locdir_blk_cmp);
	for (i = 0; i < n; i++)
	{
		BlockNumber blk = dir[i].blk;
		Buffer		buf;
		Page		page;
		OffsetNumber dels[MaxOffsetNumber];
		int			ndel = 0;
		OffsetNumber off;
		OffsetNumber maxoff;

		if (i > 0 && dir[i - 1].blk == blk)
			continue;
		lion_vacuum_delay_point();
		buf = ReadBufferExtended(index, MAIN_FORKNUM, blk, RBM_NORMAL, strategy);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
		lionvec_check_page(index, page, blk, LIONVEC_PAGE_LOC);
		maxoff = PageGetMaxOffsetNumber(page);
		for (off = FirstOffsetNumber; off <= maxoff; off = OffsetNumberNext(off))
		{
			LionVecLoc *loc = lionvec_loc_at(index, page, blk, off, nchunks);

			if (callback(&loc->tid, callback_state))
				dels[ndel++] = off;
		}
		if (ndel > 0)
		{
			GenericXLogState *state = GenericXLogStart(index);

			page = GenericXLogRegisterBuffer(state, buf, 0);
			PageIndexMultiDelete(page, dels, ndel);
			GenericXLogFinish(state);
		}
		UnlockReleaseBuffer(buf);
	}
	pfree(dir);
}
