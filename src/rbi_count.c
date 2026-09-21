/*-------------------------------------------------------------------------
 *
 * rbi_count.c
 *		Heap-skipping count(*) over roaring posting sets, interlocked with
 *		the visibility map.  DESIGN.md section 9 is the specification and
 *		the safety argument; this file is deliberately shaped so that the
 *		argument can be checked by reading it.
 *
 * The rule the whole thing rests on:
 *
 *		A container may only be counted against the visibility map while the
 *		page it was copied out of is still PINNED.
 *
 * ambulkdelete() takes LockBufferForCleanup() on every page whose containers
 * or INLINE payloads it rewrites (DESIGN.md section 11), and a cleanup lock
 * waits for all pins to go away.  So while we pin the page a container came
 * from, VACUUM cannot have finished removing that container's dead TIDs from
 * this index, and therefore cannot yet have set all-visible on any heap page
 * those dead TIDs live on.  A heap block that the visibility map reports as
 * all-visible while we hold that pin can therefore not contain a dead tuple
 * of ours, and its members can be counted without looking at the heap.
 *
 * Content locks are a different matter: they are dropped as soon as the
 * containers have been copied into backend-local memory.  Only the pin is
 * load bearing.
 *
 * What the merge intersects are SOURCES, not single posting sets: a source is
 * the union of one or more sets (an IN list, DESIGN.md §15) and may be
 * NEGATED, in which case its members are subtracted from the result instead
 * of intersected into it (`col IS NOT NULL`, DESIGN.md §14).  Neither changes
 * the rule above - every sub-cursor that contributed a container still pins
 * the page it came from until the merged container has been through the
 * visibility map - but the pin that carries the interlock has to belong to a
 * POSITIVE source, because only those are guaranteed to hold a container at
 * every container key that gets counted.
 *
 * The rule says "the page a container was copied out of", not "every page
 * every container of the intersection came from", and that is deliberate:
 * VACUUM sets a heap page all-visible only after ambulkdelete() has finished
 * on EVERY index of the table, so one pin that blocks one index's
 * ambulkdelete blocks the all-visible bit for all of them.  That is what lets
 * rbi_posting_set_materialize() serve the WHERE sets of a GROUP BY from
 * pinless private copies while the group's own set is read the pinned way;
 * see the comment on that function.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/visibilitymap.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "storage/predicate.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/injection_point.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "rbi.h"
#include "rbi_count.h"

PG_FUNCTION_INFO_V1(roaring_index_count);
PG_FUNCTION_INFO_V1(roaring_index_count2);
PG_FUNCTION_INFO_V1(roaring_index_count_stats);

/* Initial and maximum growth step of the recheck TID array. */
#define RBI_RECHECK_INIT_TIDS	256

/*
 * A posting set is worth materializing (DESIGN.md section 9 and the comment
 * on rbi_posting_set_materialize()) when it is this small.  Either bound is
 * enough: 64 containers cover 4096 heap blocks however fat they are, and a
 * set of many thin containers is cheap to keep as long as it stays under the
 * byte budget.  A set that fails both bounds keeps being walked page by page.
 */
#define RBI_MATERIALIZE_MAX_CONTAINERS	64
#define RBI_MATERIALIZE_MAX_BYTES		(256 * 1024)

/*
 * Per-call state of one count.  The visibility map buffer is kept for the
 * whole call (one VM page covers ~32k heap blocks) and released at the end.
 */
typedef struct RBICountCtx
{
	Relation	heap;
	Snapshot	snapshot;
	Buffer		vmbuf;			/* pinned VM page, or InvalidBuffer */
	bool		serializable;	/* IsolationIsSerializable() at start: take page predicate locks */
	int64		count;			/* members counted straight from the VM */
	RBICountStats stats;

	/* TIDs on heap blocks that were not all-visible, in ascending order */
	ItemPointerData *tids;
	int			ntids;
	int			maxtids;
	bool		tids_sorted;	/* they came out in order (they always do) */
} RBICountCtx;

/*
 * A posting set's containers copied out of the index, private to the backend
 * and holding no pin.  Containers are MAXALIGNed inside buf so that a BITSET
 * payload keeps its uint64 alignment, and are in ascending ckey order, which
 * is what the merge in rbi_count_posting_sets() requires.
 */
typedef struct RBIMatSet
{
	int			ncontainers;
	Size		bytes;
	char	   *buf;
	RBIContainer **containers;
} RBIMatSet;

/*
 * A cursor over the containers of one posting set, in ascending ckey order.
 *
 * Invariant (DESIGN.md section 9): whenever cur is valid, pinbuf is a pin on
 * the page cur was copied out of.  The pin is dropped only by
 * rbi_cursor_next() / rbi_cursor_close(), never anywhere else, so every place
 * that lets go of a source page is visible in this file as a call to one of
 * those two functions.
 *
 * Sparse segments (DESIGN.md §13) are presented as containers, so that the
 * merge, the AND and the visibility-map mask below need not know they exist:
 * a segment is expanded one container key at a time into segbuf, each time
 * as a temporary ARRAY container holding that key's (at most
 * RBI_SPARSE_THRESHOLD - 1) members.  The keys come out in ascending order,
 * exactly like real containers, so the merge still sees one ascending run of
 * container keys per set, and rbi_count_container() still does one
 * visibility-map read per container key.
 *
 * The pin discipline is unchanged by that, and this is the reason it works:
 * a segment being expanded lives in the cursor's page image (or in the
 * INLINE payload copy), and the source page pin is only dropped when the
 * cursor moves past the LAST item of that page -- which cannot happen while
 * a segment of it still has container keys left, because only
 * rbi_cursor_next_item() advances the page and it is not called until then.
 */
typedef struct RBISetCursor
{
	const RBIPostingSet *set;
	bool		valid;			/* cur points at a container */
	const RBIContainer *cur;

	/* INLINE sets: offset into the payload copy */
	Size		payoff;
	RBIContainer *cbuf;			/* aligned staging buffer (payloads are packed) */

	/* materialized sets: index into set->mat->containers */
	int			matidx;

	/* CHAIN sets: the page image being consumed */
	BlockNumber nextblk;
	OffsetNumber off;
	OffsetNumber maxoff;
	PGAlignedBlock *imgbuf;		/* private copy of the current container page */
	Page		img;

	/* the sparse segment being expanded, and how far into its pairs */
	const RBIContainer *seg;
	uint32		segpos;
	RBIContainer *segbuf;		/* one container key's members, built here */

	/*
	 * Pin on the source page of the current container.  For an INLINE set
	 * this is the RBIPostingSet's own bucket-page pin, which the cursor
	 * borrows and must not release (ownpin is false).
	 */
	Buffer		pinbuf;
	bool		ownpin;

	RBICountCtx *cx;			/* for statistics */
} RBISetCursor;

static void rbi_cursor_next(RBISetCursor *cur);


/* ---------------------------------------------------------------------
 * Small helpers
 * --------------------------------------------------------------------- */

/*
 * Oid of the "roaring" access method.  Cached; relation-level caches already
 * invalidate on DDL, and an access method cannot be dropped and recreated
 * with a different Oid while any of its indexes exist.
 */
Oid
rbi_get_am_oid(void)
{
	static Oid	cached = InvalidOid;

	if (!OidIsValid(cached))
		cached = get_am_oid("roaring", false);

	return cached;
}

/*
 * Hash a search key and, when it is not of the index's own type, work out the
 * cross-type equality function to compare stored keys against it.  This is
 * the same dance rbi_scan.c does for scan keys: the hash must come from the
 * argument type's own support function, the comparison from the opfamily's
 * strategy-1 operator with the stored type on the left.
 */
static uint32
rbi_probe_prepare(Relation index, RBIState *state, Datum key, Oid keytype,
				  FmgrInfo *eqproc, bool *crosstype)
{
	Oid			opfamily = index->rd_opfamily[0];
	Oid			opcintype = index->rd_opcintype[0];
	Oid			eqopr;
	Oid			hashproc;
	FmgrInfo	hashinfo;

	*crosstype = false;

	if (!OidIsValid(keytype) || keytype == opcintype)
		return rbi_hash_key(state, key);

	eqopr = get_opfamily_member(opfamily, opcintype, keytype, 1);
	if (!OidIsValid(eqopr))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("type %s cannot be compared with index \"%s\"",
						format_type_be(keytype),
						RelationGetRelationName(index)),
				 errdetail("The index is on type %s.",
						   format_type_be(opcintype))));

	hashproc = get_opfamily_proc(opfamily, keytype, keytype, 1);
	if (!OidIsValid(hashproc))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("missing support function 1 for type %s in operator family \"%s\"",
						format_type_be(keytype),
						get_opfamily_name(opfamily, false))));

	fmgr_info(get_opcode(eqopr), eqproc);
	*crosstype = true;

	fmgr_info(hashproc, &hashinfo);
	return DatumGetUInt32(FunctionCall1Coll(&hashinfo, state->collation, key));
}


/* ---------------------------------------------------------------------
 * Locating posting sets
 * --------------------------------------------------------------------- */

/*
 * Fill *ps from the entry tuple at (buf, offnum), which the caller holds
 * locked SHARE.  Returns with the lock still held; the caller decides what to
 * do with the buffer.  When the entry is INLINE the payload is copied out and
 * *keeppin is set: the caller must keep a pin on buf and store it in
 * ps->pinbuf (DESIGN.md section 9).
 */
static void
rbi_fill_posting_set(Relation index, RBIState *state, Buffer buf,
					 OffsetNumber offnum, RBIPostingSet *ps, bool *keeppin)
{
	Page		page = BufferGetPage(buf);
	ItemId		iid = PageGetItemId(page, offnum);
	RBIEntryTuple *entry = (RBIEntryTuple *) PageGetItem(page, iid);

	memset(ps, 0, sizeof(RBIPostingSet));
	ps->index = index;
	ps->pinbuf = InvalidBuffer;
	ps->found = true;
	ps->ntids = entry->ntids;
	ps->ncontainers = entry->ncontainers;
	ps->cxt = CurrentMemoryContext;
	ps->nuses = 0;
	ps->mat = NULL;

	/*
	 * rbi_fetch_key() points into the page for by-reference types, so copy
	 * the key out while the buffer is still locked.  The reserved NULL entry
	 * (DESIGN.md §14) has no key bytes to copy.
	 */
	ps->keyisnull = RBIEntryIsNullKey(entry);
	if (ps->keyisnull)
	{
		ps->storedkey = (Datum) 0;
		ps->hasstoredkey = true;
	}
	else
	{
		ps->storedkey = datumCopy(rbi_fetch_key(state, RBIEntryGetKey(entry)),
								  state->typbyval, state->typlen);
		ps->hasstoredkey = true;
	}

	if ((entry->flags & RBI_ENTRY_INLINE) != 0)
	{
		ps->is_inline = true;
		ps->head = InvalidBlockNumber;
		ps->paylen = RBI_ENTRY_PAYLOAD_LEN(entry, ItemIdGetLength(iid));
		if (ps->paylen > 0)
		{
			ps->payload = (char *) palloc(ps->paylen);
			memcpy(ps->payload, RBIEntryGetPayload(entry), ps->paylen);
		}
		*keeppin = true;
	}
	else
	{
		Assert((entry->flags & RBI_ENTRY_CHAIN) != 0);
		ps->is_inline = false;
		ps->head = entry->head;
		*keeppin = false;
	}
}

bool
rbi_posting_set_lookup(Relation index, Datum key, Oid keytype,
					   RBIPostingSet *ps)
{
	RBIState   *state = rbi_get_state(index);
	FmgrInfo	eqproc;
	bool		crosstype;
	uint32		hash;
	uint32		bucket;
	Buffer		headbuf;
	Buffer		entrybuf;
	OffsetNumber entryoff;
	bool		keeppin;

	memset(ps, 0, sizeof(RBIPostingSet));
	ps->index = index;
	ps->pinbuf = InvalidBuffer;
	ps->head = InvalidBlockNumber;

	hash = rbi_probe_prepare(index, state, key, keytype, &eqproc, &crosstype);
	bucket = rbi_bucket_of(hash, state->meta.nbuckets);

	headbuf = ReadBuffer(index, RBI_BUCKET_BLKNO(bucket));
	LockBuffer(headbuf, BUFFER_LOCK_SHARE);

	if (!rbi_find_entry_ext(index, state, headbuf, BUFFER_LOCK_SHARE,
							key, hash,
							crosstype ? &eqproc : NULL, state->collation,
							&entrybuf, &entryoff))
	{
		UnlockReleaseBuffer(headbuf);
		return false;
	}

	rbi_fill_posting_set(index, state, entrybuf, entryoff, ps, &keeppin);

	if (keeppin)
	{
		/*
		 * DESIGN.md section 9: an INLINE payload's interlock is a pin on the
		 * bucket page it lives on, so drop the content lock but hold on to
		 * the pin until rbi_posting_set_release().
		 */
		LockBuffer(entrybuf, BUFFER_LOCK_UNLOCK);
		ps->pinbuf = entrybuf;
		if (entrybuf != headbuf)
			UnlockReleaseBuffer(headbuf);
	}
	else
	{
		if (entrybuf != headbuf)
			UnlockReleaseBuffer(entrybuf);
		UnlockReleaseBuffer(headbuf);
	}

	return true;
}

/*
 * The same for the rows whose key is NULL (DESIGN.md §14).  The entry is
 * found by its flag in bucket 0; everything after that - the pin discipline
 * of DESIGN.md §9 included - is identical to a real key's.
 */
bool
rbi_posting_set_lookup_null(Relation index, RBIPostingSet *ps)
{
	RBIState   *state = rbi_get_state(index);
	Buffer		headbuf;
	Buffer		entrybuf;
	OffsetNumber entryoff;
	bool		keeppin;

	memset(ps, 0, sizeof(RBIPostingSet));
	ps->index = index;
	ps->pinbuf = InvalidBuffer;
	ps->head = InvalidBlockNumber;

	headbuf = ReadBuffer(index, RBI_BUCKET_BLKNO(RBI_NULLKEY_BUCKET));
	LockBuffer(headbuf, BUFFER_LOCK_SHARE);

	if (!rbi_find_null_entry(index, headbuf, BUFFER_LOCK_SHARE,
							 &entrybuf, &entryoff))
	{
		UnlockReleaseBuffer(headbuf);
		return false;
	}

	rbi_fill_posting_set(index, state, entrybuf, entryoff, ps, &keeppin);

	if (keeppin)
	{
		LockBuffer(entrybuf, BUFFER_LOCK_UNLOCK);
		ps->pinbuf = entrybuf;
		if (entrybuf != headbuf)
			UnlockReleaseBuffer(headbuf);
	}
	else
	{
		if (entrybuf != headbuf)
			UnlockReleaseBuffer(entrybuf);
		UnlockReleaseBuffer(headbuf);
	}

	return true;
}

void
rbi_posting_set_release(RBIPostingSet *ps)
{
	if (BufferIsValid(ps->pinbuf))
		ReleaseBuffer(ps->pinbuf);
	ps->pinbuf = InvalidBuffer;
	ps->payload = NULL;			/* the memory belongs to the caller's context */
	ps->paylen = 0;
	ps->mat = NULL;				/* ... and so does the materialized copy */
	ps->nuses = 0;
	ps->hasstoredkey = false;
	ps->keyisnull = false;
	ps->found = false;
}


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
 * is being read the pinned way - rbi_count_posting_sets() enforces that.
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
 * The copy is walked exactly the way rbi_cursor_next() walks a chain - items
 * and rightlink read together under one SHARE lock - so a concurrent page
 * split (which only ever moves items to a new page to the right) cannot make
 * us miss or duplicate a container.
 */
static bool
rbi_posting_set_materialize(RBIPostingSet *ps)
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

	offcap = Min(RBI_MATERIALIZE_MAX_CONTAINERS * 2, 256);
	offs = (Size *) palloc(sizeof(Size) * offcap);
	cap = 8192;
	buf = (char *) palloc(cap);
	imgbuf = (PGAlignedBlock *) palloc(sizeof(PGAlignedBlock));
	img = (Page) imgbuf->data;

	for (blkno = ps->head; BlockNumberIsValid(blkno);)
	{
		Buffer		pagebuf;
		Page		page;
		OffsetNumber off;
		OffsetNumber maxoff;

		pagebuf = ReadBuffer(ps->index, blkno);
		LockBuffer(pagebuf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(pagebuf);
		if (!RBIPageIsContainer(page))
		{
			UnlockReleaseBuffer(pagebuf);
			elog(ERROR, "roaring index: block %u is not a container page",
				 blkno);
		}
		memcpy(img, page, BLCKSZ);
		UnlockReleaseBuffer(pagebuf);

		blkno = RBIPageGetOpaque(img)->rightlink;
		maxoff = PageGetMaxOffsetNumber(img);

		for (off = FirstOffsetNumber; off <= maxoff; off = OffsetNumberNext(off))
		{
			ItemId		iid = PageGetItemId(img, off);
			const RBIContainer *c;
			Size		sz;

			if (!ItemIdIsUsed(iid))
				continue;
			c = (const RBIContainer *) PageGetItem(img, iid);
			sz = rbi_item_size(c);
			Assert(sz <= RBI_CONTAINER_MAX_SIZE);

			/*
			 * Give up as soon as the set fails BOTH budgets: a wide set is
			 * cheaper to re-walk than to keep a copy of.  A sparse segment
			 * counts as the one item it is, which is also how the entry
			 * counts it in ncontainers.
			 */
			if (noffs >= RBI_MATERIALIZE_MAX_CONTAINERS &&
				used + sz > RBI_MATERIALIZE_MAX_BYTES)
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

		if (!ok)
			break;

		CHECK_FOR_INTERRUPTS();
	}

	pfree(imgbuf);

	if (ok)
	{
		RBIMatSet  *mat = (RBIMatSet *) palloc(sizeof(RBIMatSet));

		mat->ncontainers = noffs;
		mat->bytes = used;
		mat->buf = buf;
		mat->containers = (RBIContainer **)
			palloc(sizeof(RBIContainer *) * Max(noffs, 1));
		for (i = 0; i < noffs; i++)
		{
			mat->containers[i] = (RBIContainer *) (buf + offs[i]);
			if (i > 0 &&
				rbi_item_first_ckey(mat->containers[i]) <=
				rbi_item_last_ckey(mat->containers[i - 1]))
				sorted = false;
		}

		/*
		 * Chains are built and maintained in ascending ckey order, with item
		 * ranges that never overlap (DESIGN.md §13), so this never fires; the
		 * merge would silently under-count if it ever did, which is worth one
		 * comparison per item to rule out.
		 */
		if (!sorted)
			elog(ERROR, "roaring index: containers of \"%s\" are out of order",
				 RelationGetRelationName(ps->index));

		ps->mat = mat;
	}
	else
	{
		pfree(buf);
	}

	pfree(offs);
	MemoryContextSwitchTo(oldcxt);

	return ok;
}


/* ---------------------------------------------------------------------
 * Container cursors
 * --------------------------------------------------------------------- */

/*
 * Release the pin the cursor owns, if any.  The only two callers are
 * rbi_cursor_next() (page exhausted) and rbi_cursor_close().
 */
static void
rbi_cursor_unpin(RBISetCursor *cur)
{
	if (cur->ownpin && BufferIsValid(cur->pinbuf))
		ReleaseBuffer(cur->pinbuf);
	cur->pinbuf = InvalidBuffer;
	cur->ownpin = false;
}

static void
rbi_cursor_init(RBISetCursor *cur, const RBIPostingSet *set, RBICountCtx *cx)
{
	memset(cur, 0, sizeof(RBISetCursor));
	cur->set = set;
	cur->cx = cx;
	cur->pinbuf = InvalidBuffer;
	cur->nextblk = InvalidBlockNumber;

	if (!set->found)
		return;

	cur->segbuf = (RBIContainer *) palloc(RBI_CONTAINER_MAX_SIZE);

	if (set->mat != NULL)
	{
		/* a private copy: nothing to pin, nothing to walk */
		cur->matidx = 0;
	}
	else if (set->is_inline)
	{
		cur->cbuf = (RBIContainer *) palloc(RBI_CONTAINER_MAX_SIZE);
		cur->payoff = 0;
		/* borrowed, not owned: the RBIPostingSet releases it */
		cur->pinbuf = set->pinbuf;
		cur->ownpin = false;
	}
	else
	{
		cur->imgbuf = (PGAlignedBlock *) palloc(sizeof(PGAlignedBlock));
		cur->img = (Page) cur->imgbuf->data;
		cur->nextblk = set->head;
	}

	rbi_cursor_next(cur);
}

/*
 * Advance to the next ITEM of the set: a container or a sparse segment, in
 * ascending ckey order, or NULL at the end.
 *
 * DESIGN.md section 9: for a CHAIN set this is the one place a source page
 * pin is dropped, and it happens only once the caller has finished with every
 * item of that page - see rbi_count_container(), which calls
 * rbi_cursor_next() immediately after the visibility-map checks and nowhere
 * else.
 */
static const RBIContainer *
rbi_cursor_next_item(RBISetCursor *cur)
{
	if (cur->set->mat != NULL)
	{
		const RBIMatSet *mat = cur->set->mat;

		if (cur->matidx >= mat->ncontainers)
			return NULL;
		return mat->containers[cur->matidx++];
	}

	if (cur->set->is_inline)
	{
		if (rbi_inline_fetch(cur->set->payload, cur->set->paylen,
							 &cur->payoff, cur->cbuf) == 0)
			return NULL;		/* payload exhausted; the set keeps its pin */
		return cur->cbuf;
	}

	for (;;)
	{
		Buffer		buf;
		Page		page;

		/* Finish the page we already have in hand. */
		while (BufferIsValid(cur->pinbuf) && cur->off <= cur->maxoff)
		{
			ItemId		iid = PageGetItemId(cur->img, cur->off);

			cur->off = OffsetNumberNext(cur->off);
			if (!ItemIdIsUsed(iid))
				continue;
			return (const RBIContainer *) PageGetItem(cur->img, iid);
		}

		/* Its items have all been consumed: the pin may go. */
		rbi_cursor_unpin(cur);

		if (!BlockNumberIsValid(cur->nextblk))
			return NULL;

		buf = ReadBuffer(cur->set->index, cur->nextblk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (!RBIPageIsContainer(page))
		{
			UnlockReleaseBuffer(buf);
			elog(ERROR, "roaring index: block %u is not a container page",
				 cur->nextblk);
		}

		/*
		 * Copy the page - items and rightlink together, as rbi_scan.c does -
		 * then drop the content lock but keep the pin (DESIGN.md section 9).
		 */
		memcpy(cur->img, page, BLCKSZ);
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		cur->pinbuf = buf;
		cur->ownpin = true;

		cur->nextblk = RBIPageGetOpaque(cur->img)->rightlink;
		cur->off = FirstOffsetNumber;
		cur->maxoff = PageGetMaxOffsetNumber(cur->img);

		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * Build the next container key of the segment being expanded into segbuf and
 * point the cursor at it.  Returns false when the segment is used up.
 *
 * The result is a throw-away ARRAY container: the merge, the AND and the
 * visibility-map mask treat it exactly like one read from a page, and it
 * lives only until the next call.  The segment itself is left alone, and so
 * is the pin on the page it came from.
 */
static bool
rbi_cursor_emit_segment(RBISetCursor *cur)
{
	const RBIContainer *seg = cur->seg;
	const uint32 *ckeys = RBI_SPARSE_CKEYS_CONST(seg);
	const uint16 *los = RBI_SPARSE_LOS_CONST(seg);
	uint32		n = seg->cardinality;
	uint32		ckey;

	if (cur->segpos >= n)
		return false;

	ckey = ckeys[cur->segpos];
	rbi_container_init(cur->segbuf, ckey);
	do
	{
		rbi_container_append_sorted(cur->segbuf, los[cur->segpos]);
		cur->segpos++;
	} while (cur->segpos < n && ckeys[cur->segpos] == ckey);

	cur->cur = cur->segbuf;
	cur->valid = true;
	cur->cx->stats.containers_visited++;
	return true;
}

/*
 * Advance to the next container of the set, expanding sparse segments one
 * container key at a time (see the comment on RBISetCursor).
 */
static void
rbi_cursor_next(RBISetCursor *cur)
{
	cur->valid = false;
	cur->cur = NULL;

	if (cur->set == NULL || !cur->set->found)
		return;

	/* Still inside a segment?  Its next container key is the next container. */
	if (cur->seg != NULL)
	{
		if (rbi_cursor_emit_segment(cur))
			return;
		cur->seg = NULL;
	}

	for (;;)
	{
		const RBIContainer *item = rbi_cursor_next_item(cur);

		if (item == NULL)
			return;

		if (item->type != RBI_CT_SPARSE)
		{
			cur->cur = item;
			cur->valid = true;
			cur->cx->stats.containers_visited++;
			return;
		}

		cur->seg = item;
		cur->segpos = 0;
		if (rbi_cursor_emit_segment(cur))
			return;
		cur->seg = NULL;		/* an empty segment: nothing to present */
	}
}

static void
rbi_cursor_close(RBISetCursor *cur)
{
	rbi_cursor_unpin(cur);
	cur->seg = NULL;
	cur->valid = false;
	cur->cur = NULL;
}


/* ---------------------------------------------------------------------
 * Expression cursors: one source of the merge
 * --------------------------------------------------------------------- */

/*
 * A cursor over a boolean expression of posting sets, presenting one
 * ascending run of container keys just as a single set does, so that the
 * merge below need not know what is behind a source.
 *
 * Three node kinds, which are exactly the three RBIKeyNode kinds:
 *
 *	LEAF	one posting set, walked by an RBISetCursor.
 *	OR		the union (DESIGN.md §15's IN lists, and `tags && '{a,b}'`): the
 *			cursor stands at the SMALLEST container key any child has left,
 *			and its container is the OR of the containers of every child
 *			standing at that key.
 *	AND		the intersection (`tags @> '{a,b}'`, and the AND nodes of a
 *			tsquery, DESIGN.md §17): the children are wound forward until
 *			they all stand at one container key, and the container is the AND
 *			of theirs.  A container key whose intersection comes out empty is
 *			skipped here rather than handed up.
 *
 * THE PIN RULE (DESIGN.md §9) IS UNCHANGED BY EITHER OPERATOR.  Every leaf
 * that contributed a container to the result still pins the page that
 * container was copied from, because a leaf is only advanced by
 * rbi_ecursor_next(), and the merge only calls that from
 * rbi_count_container(), after the visibility map has been consulted for the
 * merged container.  Leaves that are ahead of the current key hold their own
 * pins as well, which is harmless: a pin too many never makes a count wrong,
 * it only makes VACUUM wait.
 *
 * Skipping is the one place a pin goes without anything having been counted
 * from it - an AND winding a lagging child forward, or dropping a container
 * key whose intersection is empty.  That is safe for the reason the merge's
 * own `!alleq` branch is safe: nothing of that container key reaches the
 * visibility map, so no answer rests on it.
 */
typedef struct RBIExprCursor
{
	const RBIKeyNode *node;		/* NULL: an empty source, never valid */
	RBIKeyNodeKind kind;

	/* RBI_KN_KEY */
	RBISetCursor leaf;

	/* RBI_KN_AND / RBI_KN_OR */
	int			nsub;
	struct RBIExprCursor *sub;
	RBIContainer *acc[2];		/* AND/OR accumulators, only when nsub > 1 */

	/* the container the cursor currently stands on */
	bool		valid;
	uint32		ckey;
	const RBIContainer *cur;

	bool		advance;		/* top level only: took part in this key */
} RBIExprCursor;

static void rbi_ecursor_build(RBIExprCursor *c);
static void rbi_ecursor_next(RBIExprCursor *c);

static void
rbi_ecursor_init(RBIExprCursor *c, const RBIKeyNode *node,
				 RBIPostingSet *sets, int nsets, RBICountCtx *cx)
{
	int			i;

	check_stack_depth();

	memset(c, 0, sizeof(RBIExprCursor));
	c->node = node;
	if (node == NULL)
		return;					/* a source with no sets at all */

	c->kind = node->kind;

	if (node->kind == RBI_KN_KEY)
	{
		Assert(node->keyno >= 0 && node->keyno < nsets);
		rbi_cursor_init(&c->leaf, &sets[node->keyno], cx);
	}
	else
	{
		Assert(node->nargs >= 1);
		c->nsub = node->nargs;
		c->sub = (RBIExprCursor *) palloc0(sizeof(RBIExprCursor) * c->nsub);
		for (i = 0; i < c->nsub; i++)
			rbi_ecursor_init(&c->sub[i], node->args[i], sets, nsets, cx);

		if (c->nsub > 1)
		{
			c->acc[0] = (RBIContainer *) palloc(RBI_CONTAINER_MAX_SIZE);
			c->acc[1] = (RBIContainer *) palloc(RBI_CONTAINER_MAX_SIZE);
		}
	}

	rbi_ecursor_build(c);
}

/*
 * Recompute the cursor's current container from its children.
 */
static void
rbi_ecursor_build(RBIExprCursor *c)
{
	const RBIContainer *acc;
	int			w = 0;
	int			i;

	c->valid = false;
	c->cur = NULL;

	if (c->node == NULL)
		return;

	if (c->kind == RBI_KN_KEY)
	{
		if (!c->leaf.valid)
			return;
		c->cur = c->leaf.cur;
		c->ckey = c->cur->ckey;
		c->valid = true;
		return;
	}

	if (c->kind == RBI_KN_OR)
	{
		uint32		minckey = 0;
		bool		havemin = false;

		for (i = 0; i < c->nsub; i++)
		{
			if (!c->sub[i].valid)
				continue;
			if (!havemin || c->sub[i].ckey < minckey)
			{
				minckey = c->sub[i].ckey;
				havemin = true;
			}
		}

		if (!havemin)
			return;				/* every child is exhausted */

		acc = NULL;
		for (i = 0; i < c->nsub; i++)
		{
			if (!c->sub[i].valid || c->sub[i].ckey != minckey)
				continue;

			if (acc == NULL)
				acc = c->sub[i].cur;
			else
			{
				/* Same container key on both sides, so this is a plain OR. */
				rbi_container_or(acc, c->sub[i].cur, c->acc[w]);
				acc = c->acc[w];
				w ^= 1;
			}
		}

		c->ckey = minckey;
		c->cur = acc;
		c->valid = true;
		return;
	}

	Assert(c->kind == RBI_KN_AND);

	for (;;)
	{
		uint32		maxckey;
		bool		alleq = true;

		for (i = 0; i < c->nsub; i++)
		{
			if (!c->sub[i].valid)
				return;			/* a child ran out: so has the intersection */
		}

		maxckey = c->sub[0].ckey;
		for (i = 1; i < c->nsub; i++)
		{
			if (c->sub[i].ckey > maxckey)
				maxckey = c->sub[i].ckey;
		}

		/* Wind the laggards forward; their containers cannot contribute. */
		for (i = 0; i < c->nsub; i++)
		{
			if (c->sub[i].ckey != maxckey)
			{
				alleq = false;
				rbi_ecursor_next(&c->sub[i]);
			}
		}
		if (!alleq)
		{
			CHECK_FOR_INTERRUPTS();
			continue;
		}

		acc = c->sub[0].cur;
		w = 0;
		for (i = 1; i < c->nsub; i++)
		{
			rbi_container_and(acc, c->sub[i].cur, c->acc[w]);
			acc = c->acc[w];
			w ^= 1;
		}

		if (rbi_container_cardinality(acc) > 0)
		{
			c->ckey = maxckey;
			c->cur = acc;
			c->valid = true;
			return;
		}

		/*
		 * Nothing of this container key survives the intersection, so nothing
		 * will ask the visibility map about it and every child may move on.
		 */
		for (i = 0; i < c->nsub; i++)
			rbi_ecursor_next(&c->sub[i]);

		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * Move past the current container key.  Only the children that stand at it
 * move; the ones that are ahead (an OR's) stay where they are.  This is the
 * only place a source lets go of a page pin that carried an answer.
 */
static void
rbi_ecursor_next(RBIExprCursor *c)
{
	int			i;

	if (c->node == NULL || !c->valid)
		return;

	switch (c->kind)
	{
		case RBI_KN_KEY:
			rbi_cursor_next(&c->leaf);
			break;

		case RBI_KN_OR:
			for (i = 0; i < c->nsub; i++)
			{
				if (c->sub[i].valid && c->sub[i].ckey == c->ckey)
					rbi_ecursor_next(&c->sub[i]);
			}
			break;

		case RBI_KN_AND:
			/* every child stands at c->ckey and contributed to the result */
			for (i = 0; i < c->nsub; i++)
				rbi_ecursor_next(&c->sub[i]);
			break;
	}

	rbi_ecursor_build(c);
}

static void
rbi_ecursor_close(RBIExprCursor *c)
{
	int			i;

	if (c->node == NULL)
		return;

	if (c->kind == RBI_KN_KEY)
		rbi_cursor_close(&c->leaf);
	else
	{
		for (i = 0; i < c->nsub; i++)
			rbi_ecursor_close(&c->sub[i]);
	}

	c->valid = false;
	c->cur = NULL;
}

/* ---------------------------------------------------------------------
 * Source expressions
 * --------------------------------------------------------------------- */

/*
 * The tree a source combines its sets with: its own, or the implicit union of
 * all of them.  NULL when the source has no sets at all, which only a negated
 * source can have (a `col IS NOT NULL` on a column with no NULLs).
 */
static RBIKeyNode *
rbi_source_tree(const RBICountSource *src)
{
	RBIKeyNode *node;
	RBIKeyNode **args;
	int			i;

	if (src->tree != NULL)
		return src->tree;
	if (src->nsets == 0)
		return NULL;

	args = (RBIKeyNode **) palloc(sizeof(RBIKeyNode *) * src->nsets);
	for (i = 0; i < src->nsets; i++)
	{
		args[i] = (RBIKeyNode *) palloc0(sizeof(RBIKeyNode));
		args[i]->kind = RBI_KN_KEY;
		args[i]->keyno = i;
	}
	if (src->nsets == 1)
		return args[0];

	node = (RBIKeyNode *) palloc0(sizeof(RBIKeyNode));
	node->kind = RBI_KN_OR;
	node->nargs = src->nsets;
	node->args = args;
	return node;
}

/*
 * Can this expression select anything at all?  A key with no entry in the
 * index selects nothing, and an AND of one such key selects nothing however
 * many other keys it has.  Answering that up front is what lets a count over
 * an impossible clause cost one bucket lookup per key and no merge.
 */
static bool
rbi_source_satisfiable(const RBIKeyNode *node, const RBIPostingSet *sets)
{
	int			i;

	if (node == NULL)
		return false;

	switch (node->kind)
	{
		case RBI_KN_KEY:
			return sets[node->keyno].found;

		case RBI_KN_AND:
			for (i = 0; i < node->nargs; i++)
			{
				if (!rbi_source_satisfiable(node->args[i], sets))
					return false;
			}
			return true;

		case RBI_KN_OR:
			for (i = 0; i < node->nargs; i++)
			{
				if (rbi_source_satisfiable(node->args[i], sets))
					return true;
			}
			return false;
	}

	return false;
}

/*
 * Does every container this expression can yield come with a live buffer pin
 * on the page it was read from?  That is the DESIGN.md §9 interlock, and
 * rbi_count_sources() has to keep at least one positive source that has it
 * (see the comment on rbi_posting_set_materialize()).
 *
 *	- a leaf has it unless its set has been materialized; a leaf whose key has
 *	  no entry yields nothing, so it has it vacuously;
 *	- an AND has it if ANY child has it, because every child stands at the
 *	  container key the result was built from and so every child's pin is
 *	  still held when the result is counted;
 *	- an OR has it only if EVERY child has it, because which children
 *	  contributed to a given container key is not known in advance.
 */
static bool
rbi_source_pinned(const RBIKeyNode *node, const RBIPostingSet *sets)
{
	int			i;

	if (node == NULL)
		return true;			/* yields nothing */

	switch (node->kind)
	{
		case RBI_KN_KEY:
			return !sets[node->keyno].found || sets[node->keyno].mat == NULL;

		case RBI_KN_AND:
			for (i = 0; i < node->nargs; i++)
			{
				if (rbi_source_pinned(node->args[i], sets))
					return true;
			}
			return false;

		case RBI_KN_OR:
			for (i = 0; i < node->nargs; i++)
			{
				if (!rbi_source_pinned(node->args[i], sets))
					return false;
			}
			return true;
	}

	return false;
}


/* ---------------------------------------------------------------------
 * The visibility map, a container at a time
 * --------------------------------------------------------------------- */

/*
 * The layout of a visibility map page.  These mirror the private macros of
 * the same name in src/backend/access/heap/visibilitymap.c, which is the only
 * place the format is written down; nothing outside that file exports them.
 * Keep them in step with it.  The static assertions below pin down the parts
 * of the format that visibilitymapdefs.h does export, which is what the bit
 * twiddling in rbi_vm_allvisible_mask() actually depends on.
 */
#define RBI_VM_MAPSIZE				(BLCKSZ - MAXALIGN(SizeOfPageHeaderData))
#define RBI_VM_HEAPBLOCKS_PER_BYTE	(BITS_PER_BYTE / BITS_PER_HEAPBLOCK)
#define RBI_VM_HEAPBLOCKS_PER_PAGE	(RBI_VM_MAPSIZE * RBI_VM_HEAPBLOCKS_PER_BYTE)
#define RBI_VM_HEAPBLK_TO_MAPBYTE(x) \
	(((x) % RBI_VM_HEAPBLOCKS_PER_PAGE) / RBI_VM_HEAPBLOCKS_PER_BYTE)

StaticAssertDecl(BITS_PER_HEAPBLOCK == 2,
				 "roaring_index: the visibility map is no longer two bits per heap block");
StaticAssertDecl(VISIBILITYMAP_VALID_BITS == 0x03,
				 "roaring_index: unexpected visibility map bit assignment");
StaticAssertDecl(VISIBILITYMAP_ALL_VISIBLE == 0x01,
				 "roaring_index: all-visible is no longer the low bit of each pair");
/* the masks below are uint64s, one bit per heap block a container covers */
StaticAssertDecl(RBI_BLOCKS_PER_CONTAINER <= 64,
				 "roaring_index: a container covers more heap blocks than a mask holds");
/*
 * A container's heap blocks start at a multiple of RBI_BLOCKS_PER_CONTAINER,
 * and both that and the number of blocks per map page are multiples of the
 * number of blocks per map byte, so every run of blocks rbi_vm_allvisible_mask()
 * reads begins and ends on a byte boundary of the map.
 */
StaticAssertDecl(RBI_BLOCKS_PER_CONTAINER % RBI_VM_HEAPBLOCKS_PER_BYTE == 0,
				 "roaring_index: container block range is not map-byte aligned");
StaticAssertDecl(RBI_VM_HEAPBLOCKS_PER_PAGE % RBI_VM_HEAPBLOCKS_PER_BYTE == 0,
				 "roaring_index: map page does not hold a whole number of map bytes");

/*
 * The all-visible bits of the RBI_BLOCKS_PER_CONTAINER consecutive heap blocks
 * a container covers, as one mask: bit i is set iff heap block firstblk + i is
 * marked all-visible.  *vmbuf is the caller's visibility map pin; it is moved
 * to whatever map page is needed and left pinned for the next call, exactly as
 * visibilitymap_get_status() leaves it.
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
 */
static uint64
rbi_vm_allvisible_mask(Relation heap, BlockNumber firstblk, Buffer *vmbuf)
{
	uint64		mask = 0;
	int			b = 0;

	Assert(firstblk % RBI_BLOCKS_PER_CONTAINER == 0);

	/*
	 * Usually one pass.  RBI_VM_HEAPBLOCKS_PER_PAGE (32672 at 8K) is NOT a
	 * multiple of RBI_BLOCKS_PER_CONTAINER, so roughly one container in five
	 * hundred straddles two map pages and needs two - which is why this is a
	 * loop and not sixteen bytes read in one go.
	 */
	while (b < RBI_BLOCKS_PER_CONTAINER)
	{
		BlockNumber blk = firstblk + (BlockNumber) b;
		int			n;

		/* how many of the blocks still wanted live on blk's map page */
		n = (int) (RBI_VM_HEAPBLOCKS_PER_PAGE -
				   (blk % RBI_VM_HEAPBLOCKS_PER_PAGE));
		n = Min(n, RBI_BLOCKS_PER_CONTAINER - b);
		Assert(n > 0 && n % RBI_VM_HEAPBLOCKS_PER_BYTE == 0);

		/*
		 * Take the pin the way visibilitymap_get_status() does: same buffer
		 * reuse, same refusal to extend the fork.  Its return value is the
		 * status of blk itself, which the byte read below repeats.
		 */
		if (!visibilitymap_pin_ok(blk, *vmbuf))
			(void) visibilitymap_get_status(heap, blk, vmbuf);

		if (BufferIsValid(*vmbuf))
		{
			const char *map = (const char *) PageGetContents(BufferGetPage(*vmbuf));
			uint32		mapbyte = RBI_VM_HEAPBLK_TO_MAPBYTE(blk);
			int			nbytes = n / RBI_VM_HEAPBLOCKS_PER_BYTE;
			int			j;

			for (j = 0; j < nbytes; j++)
			{
				/*
				 * Squeeze the four all-visible bits of the byte - the low bit
				 * of each pair - down into a nibble of the result.
				 */
				uint8		v = (uint8) (map[mapbyte + j] & 0x55);

				v = (uint8) ((v | (v >> 1)) & 0x33);
				v = (uint8) ((v | (v >> 2)) & 0x0f);
				mask |= ((uint64) v) << (b + j * RBI_VM_HEAPBLOCKS_PER_BYTE);
			}
		}
		/* else the fork stops short of these blocks: none of them is all-visible */

		b += n;
	}

	return mask;
}

/*
 * The other half of the pair: which of the heap blocks a container covers have
 * at least one member, in the same bit numbering.
 *
 * One pass over the container, whatever its representation.  The obvious
 * alternative - rbi_container_range_cardinality() once per block range - is
 * RBI_BLOCKS_PER_CONTAINER binary searches whether the container holds three
 * members or thirty-two thousand, and that cost is paid even when the answer
 * is going to be "all of it is all-visible, count the cardinality".
 */
#define RBI_BITSET_WORDS_PER_BLOCK	(RBI_BITSET_WORDS / RBI_BLOCKS_PER_CONTAINER)

StaticAssertDecl(RBI_BITSET_WORDS_PER_BLOCK * RBI_BLOCKS_PER_CONTAINER ==
				 RBI_BITSET_WORDS,
				 "roaring_index: bitset words do not divide evenly among heap blocks");

static uint64
rbi_container_block_mask(const RBIContainer *c)
{
	const char *payload = (const char *) c + RBI_CONTAINER_HDRSZ;
	uint64		mask = 0;
	uint32		i;

	switch (c->type)
	{
		case RBI_CT_ARRAY:
			{
				const uint16 *arr = (const uint16 *) payload;

				for (i = 0; i < c->cardinality; i++)
					mask |= UINT64CONST(1) << (arr[i] >> RBI_OFFSET_BITS);
				break;
			}
		case RBI_CT_BITSET:
			{
				const uint64 *w = (const uint64 *) payload;
				int			b;

				for (b = 0; b < RBI_BLOCKS_PER_CONTAINER; b++)
				{
					uint64		any = 0;
					int			k;

					for (k = 0; k < RBI_BITSET_WORDS_PER_BLOCK; k++)
						any |= w[b * RBI_BITSET_WORDS_PER_BLOCK + k];
					if (any != 0)
						mask |= UINT64CONST(1) << b;
				}
				break;
			}
		case RBI_CT_RUN:
			{
				uint32		nruns = *(const uint16 *) payload;
				const RBIRun *runs = (const RBIRun *) (payload + sizeof(uint16));

				for (i = 0; i < nruns; i++)
				{
					uint32		first = runs[i].start >> RBI_OFFSET_BITS;
					uint32		last = (((uint32) runs[i].start +
										 runs[i].len_minus_1) >> RBI_OFFSET_BITS);

					Assert(last < RBI_BLOCKS_PER_CONTAINER);
					mask |= (PG_UINT64_MAX >> (63 - last)) &
						(PG_UINT64_MAX << first);
				}
				break;
			}
		default:
			Assert(false);
			break;
	}

	return mask;
}

/*
 * Assert builds cross-check every mask against the function it replaces.  This
 * reinstates exactly the per-block cost the mask exists to avoid, so timings
 * taken on a cassert cluster want -DRBI_NO_VM_MASK_CHECK.
 */
#if defined(USE_ASSERT_CHECKING) && !defined(RBI_NO_VM_MASK_CHECK)
#define RBI_VM_MASK_CHECK 1
#endif

#ifdef RBI_VM_MASK_CHECK
/*
 * visibilitymap_get_status() for every block that has members - the only bits
 * of the mask the count looks at - must agree with the mask.
 *
 * It legitimately might not, if a concurrent VACUUM or DML changed a bit
 * between the two reads, so the comparison is only made when a fresh read of
 * the whole mask still matches the one under test.  That second read is what
 * keeps this assertion from being a race.
 */
static void
rbi_vm_mask_check(Relation heap, BlockNumber firstblk, uint64 members,
				  uint64 allvis, Buffer *vmbuf)
{
	uint64		expect = 0;
	uint64		m = members;

	while (m != 0)
	{
		int			b = pg_rightmost_one_pos64(m);

		m &= m - 1;
		if ((visibilitymap_get_status(heap, firstblk + (BlockNumber) b, vmbuf) &
			 VISIBILITYMAP_ALL_VISIBLE) != 0)
			expect |= UINT64CONST(1) << b;
	}

	if (rbi_vm_allvisible_mask(heap, firstblk, vmbuf) == allvis)
		Assert((allvis & members) == expect);
}
#endif

/* ---------------------------------------------------------------------
 * Counting one container
 * --------------------------------------------------------------------- */

typedef struct RBIRecheckCollector
{
	RBICountCtx *cx;
	uint32		ckey;
	const bool *needrecheck;	/* RBI_BLOCKS_PER_CONTAINER entries */
} RBIRecheckCollector;

static void
rbi_recheck_add(RBICountCtx *cx, uint64 code)
{
	if (cx->ntids >= cx->maxtids)
	{
		int			newmax = (cx->maxtids == 0) ?
			RBI_RECHECK_INIT_TIDS : cx->maxtids * 2;

		if (cx->tids == NULL)
			cx->tids = (ItemPointerData *)
				palloc(sizeof(ItemPointerData) * newmax);
		else
			cx->tids = (ItemPointerData *)
				repalloc(cx->tids, sizeof(ItemPointerData) * newmax);
		cx->maxtids = newmax;
	}

	rbi_code_to_tid(code, &cx->tids[cx->ntids]);

	/*
	 * The list is built in ckey order, and inside a container in ascending lo
	 * order, so it comes out sorted by (block, offset) - which is what lets
	 * rbi_recheck_heap() process it one heap block at a time.  Verify rather
	 * than assume: one comparison per TID buys the right to skip the sort.
	 */
	if (cx->ntids > 0 &&
		ItemPointerCompare(&cx->tids[cx->ntids - 1], &cx->tids[cx->ntids]) >= 0)
		cx->tids_sorted = false;

	cx->ntids++;
}

static bool
rbi_recheck_cb(uint16 lo, void *arg)
{
	RBIRecheckCollector *rc = (RBIRecheckCollector *) arg;
	uint16		blkinc;
	OffsetNumber off;

	rbi_lo_split(lo, &blkinc, &off);
	Assert(blkinc < RBI_BLOCKS_PER_CONTAINER);
	if (rc->needrecheck[blkinc])
		rbi_recheck_add(rc->cx, rbi_make_code(rc->ckey, lo));

	return true;
}

/*
 * Count one container of the intersection, then let the cursors move on.
 *
 * This function is the heart of DESIGN.md section 9.  On entry every cursor
 * still pins the page its current container was copied from; *c was computed
 * from exactly those containers.  The two things that must happen in this
 * order are both here, adjacent, and nowhere else:
 *
 *	1. ask the visibility map about every heap block that has members, and
 *	   either count the members outright (plus a predicate lock, as an
 *	   index-only scan would take) or queue the block's TIDs for a heap
 *	   recheck under the caller's snapshot;
 *	2. advance the cursors, which is what releases the source page pins.
 *
 * Doing (2) before (1) would let a concurrent VACUUM finish ambulkdelete on
 * the page we just read, prune the heap and set all-visible, after which
 * step (1) would count dead tuples.
 */
static void
rbi_count_container(RBICountCtx *cx, const RBIContainer *c,
					RBIExprCursor *cursors, int nsources)
{
	BlockNumber firstblk = rbi_ckey_first_block(c->ckey);
	uint64		members;		/* blocks of this container that have members */
	uint64		allvis;			/* blocks marked all-visible in the VM */
	uint64		dirty;			/* blocks with members that need a heap recheck */
	int			i;

	/*
	 * Test hook: the containers have been copied out, the source pages are
	 * still pinned, and the visibility map has not been consulted yet.  A
	 * VACUUM that reaches ambulkdelete while a backend waits here must block
	 * on the cleanup lock; test/isolation/count_vacuum_race.spec proves it.
	 * Compiles to nothing without --enable-injection-points.
	 */
	INJECTION_POINT("roaring-count-containers-pinned", NULL);

	/*
	 * One pass over the container and one read of the visibility map for all
	 * of its heap blocks, instead of a VM probe and a binary search per block.
	 * VM_ALL_VISIBLE only, never VM_ALL_FROZEN: freezing says nothing about a
	 * tuple being visible to *this* snapshot.  The map read is unlocked and
	 * may be slightly stale in the "bit was just cleared" direction, which is
	 * harmless for the same reason it is harmless for index-only scans (see
	 * visibilitymap_get_status and rbi_vm_allvisible_mask).
	 */
	members = rbi_container_block_mask(c);
	allvis = rbi_vm_allvisible_mask(cx->heap, firstblk, &cx->vmbuf);
#ifdef RBI_VM_MASK_CHECK
	rbi_vm_mask_check(cx->heap, firstblk, members, allvis, &cx->vmbuf);
#endif
	dirty = members & ~allvis;

	if (dirty == 0)
	{
		/* The common case on a vacuumed table: O(1) per container. */
		cx->count += rbi_container_cardinality(c);
		cx->stats.blocks_skipped_via_vm += pg_popcount64(members);
	}
	else
	{
		bool		needrecheck[RBI_BLOCKS_PER_CONTAINER];
		uint32		dirty_members = 0;
		uint64		m = dirty;
		RBIRecheckCollector rc;

		memset(needrecheck, 0, sizeof(needrecheck));
		while (m != 0)
		{
			int			b = pg_rightmost_one_pos64(m);
			uint16		lo_start = (uint16) (b << RBI_OFFSET_BITS);
			uint16		lo_end = (uint16) (lo_start + RBI_MAX_OFFSET);

			m &= m - 1;
			needrecheck[b] = true;
			dirty_members += rbi_container_range_cardinality(c, lo_start, lo_end);
		}
		Assert(dirty_members <= rbi_container_cardinality(c));

		cx->count += rbi_container_cardinality(c) - dirty_members;
		cx->stats.blocks_skipped_via_vm += pg_popcount64(members & allvis);

		/* Queue the members of the non-all-visible blocks for a heap recheck. */
		rc.cx = cx;
		rc.ckey = c->ckey;
		rc.needrecheck = needrecheck;
		rbi_container_iterate(c, rbi_recheck_cb, &rc);
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

	/*
	 * DESIGN.md section 9: every heap block of *c has now been checked
	 * against the visibility map, so - and only now - the pins on the pages
	 * the source containers came from may be released.  rbi_ecursor_next() is
	 * what releases them, and only the sources that contributed to *c (the
	 * ones the merge flagged) move on.
	 */
	for (i = 0; i < nsources; i++)
	{
		if (cursors[i].advance)
			rbi_ecursor_next(&cursors[i]);
	}
}

static int
rbi_tid_cmp(const void *a, const void *b)
{
	return ItemPointerCompare((ItemPointer) a, (ItemPointer) b);
}

/*
 * Recheck through the table AM, one TID at a time.  This is the portable
 * path; table_fetch_tid() pins, share-locks and unpins the block for every
 * TID, which is what rbi_recheck_heap_heap() avoids for the heap AM.
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
rbi_recheck_heap_am(RBICountCtx *cx)
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

		if (table_fetch_tid(cx->heap, &tid, cx->snapshot, NULL))
			visible++;

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
 *	  are predicate-locked page-wise in rbi_count_container(), as an
 *	  index-only scan does.)
 *
 * all_dead is passed as NULL, as the table_fetch_tid() call it replaces did:
 * we have no index tuple to mark killed, and asking for it would cost a
 * GlobalVisTest per invisible chain for nothing.
 */
static int64
rbi_recheck_heap_heap(RBICountCtx *cx)
{
	int64		visible = 0;
	int			i = 0;

	while (i < cx->ntids)
	{
		BlockNumber blk = ItemPointerGetBlockNumber(&cx->tids[i]);
		Buffer		buf;

		buf = ReadBuffer(cx->heap, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);

		do
		{
			ItemPointerData tid = cx->tids[i];	/* callee updates it */
			HeapTupleData heapTuple;

			if (heap_hot_search_buffer(&tid, cx->heap, buf, cx->snapshot,
									   &heapTuple, NULL, true))
				visible++;
			i++;
		} while (i < cx->ntids &&
				 ItemPointerGetBlockNumber(&cx->tids[i]) == blk);

		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buf);
		cx->stats.blocks_rechecked++;

		/* Only now: an interrupt cannot be serviced under a buffer lock. */
		CHECK_FOR_INTERRUPTS();
	}

	return visible;
}

/*
 * Visit the heap for the TIDs of blocks that were not all-visible.
 */
static int64
rbi_recheck_heap(RBICountCtx *cx)
{
	int64		visible;

	if (cx->ntids == 0)
		return 0;

	/*
	 * Both paths below want the list in (block, offset) order; it is produced
	 * that way, and rbi_recheck_add() checks that it was.
	 */
	if (!cx->tids_sorted)
		qsort(cx->tids, cx->ntids, sizeof(ItemPointerData), rbi_tid_cmp);

	if (cx->heap->rd_tableam == GetHeapamTableAmRoutine())
		visible = rbi_recheck_heap_heap(cx);
	else
		visible = rbi_recheck_heap_am(cx);

	cx->stats.tids_rechecked += cx->ntids;
	return visible;
}


/* ---------------------------------------------------------------------
 * The merge
 * --------------------------------------------------------------------- */

int64
rbi_count_sources(Relation heap, Snapshot snapshot, int nsources,
				  RBICountSource *sources, RBICountStats *stats)
{
	MemoryContext cxt;
	MemoryContext oldcxt;
	RBICountCtx cx;
	RBIExprCursor *cursors;
	RBIKeyNode **trees;
	RBIContainer *work[2];
	int64		result;
	int			ncarry;
	bool	   *carry;
	int			npositive = 0;
	int			i;
	int			j;

	Assert(nsources >= 1);

	/*
	 * The shape of each source, and whether it can select anything at all: a
	 * positive source that cannot makes the whole intersection empty, and a
	 * negated one that cannot simply subtracts nothing.
	 */
	trees = (RBIKeyNode **) palloc0(sizeof(RBIKeyNode *) * nsources);
	for (i = 0; i < nsources; i++)
	{
		trees[i] = rbi_source_tree(&sources[i]);

		if (sources[i].negated)
			continue;
		npositive++;
		if (!rbi_source_satisfiable(trees[i], sources[i].sets))
			return 0;
	}
	if (npositive == 0)
		elog(ERROR, "roaring index count needs at least one positive source");

	/*
	 * Decide which sets to serve from a private copy this time (DESIGN.md
	 * section 9; the argument is on rbi_posting_set_materialize()).
	 *
	 * Two rules, and the safety of the whole thing rests on the second:
	 *
	 *	1. only a set that has been counted before, which in practice means
	 *	   the WHERE sets of the GROUP BY path in rbi_customscan.c, where the
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
	 *	   it yields, which is what rbi_source_pinned() decides.
	 *
	 * A negated set may always be copied: a stale copy can only hold TIDs
	 * whose rows are dead (a live row's key cannot change without the row
	 * getting a new TID), and subtracting a dead TID cannot take a live row
	 * out of the count.
	 */
	carry = (bool *) palloc0(sizeof(bool) * nsources);
	ncarry = 0;
	for (i = 0; i < nsources; i++)
	{
		for (j = 0; j < sources[i].nsets; j++)
		{
			if (sources[i].sets[j].found)
				sources[i].sets[j].nuses++;
		}

		if (sources[i].negated)
			continue;
		carry[i] = rbi_source_pinned(trees[i], sources[i].sets);
		if (carry[i])
			ncarry++;
	}
	Assert(ncarry > 0);

	for (i = 0; i < nsources; i++)
	{
		for (j = 0; j < sources[i].nsets; j++)
		{
			RBIPostingSet *ps = &sources[i].sets[j];

			if (!ps->found)
				continue;
			if (ps->is_inline || ps->mat != NULL)
				continue;		/* nothing to gain: already a private copy */
			if (ps->nuses < 2)
				continue;		/* rule 1 */
			if (!sources[i].negated && carry[i] && ncarry <= 1)
				continue;		/* rule 2: this is the last interlock */
			if (ps->ncontainers > RBI_MATERIALIZE_MAX_CONTAINERS &&
				ps->ntids > RBI_MATERIALIZE_MAX_BYTES / sizeof(uint16))
				continue;		/* hopeless even as an ARRAY of members */

			if (rbi_posting_set_materialize(ps) && !sources[i].negated &&
				carry[i] && !rbi_source_pinned(trees[i], sources[i].sets))
			{
				carry[i] = false;
				ncarry--;
			}
		}
	}
	Assert(ncarry > 0);

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"roaring index count",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	memset(&cx, 0, sizeof(cx));
	cx.heap = heap;
	cx.snapshot = snapshot;
	cx.vmbuf = InvalidBuffer;
	/* SerializationNeededForRead() begins with exactly this test; hoisting it
	 * lets non-serializable counts skip the per-block PredicateLockPage loop. */
	cx.serializable = IsolationIsSerializable();
	cx.tids_sorted = true;

	cursors = (RBIExprCursor *) palloc0(sizeof(RBIExprCursor) * nsources);
	work[0] = (RBIContainer *) palloc(RBI_CONTAINER_MAX_SIZE);
	work[1] = (RBIContainer *) palloc(RBI_CONTAINER_MAX_SIZE);

	for (i = 0; i < nsources; i++)
		rbi_ecursor_init(&cursors[i], trees[i], sources[i].sets,
						 sources[i].nsets, &cx);

	/*
	 * Merge the sources by container key.  Containers are stored in ascending
	 * ckey order both inline and along a chain, and an expression cursor
	 * preserves that, so a single forward pass over all of them is enough.
	 */
	for (;;)
	{
		uint32		maxckey = 0;
		bool		havemax = false;
		bool		alleq = true;
		const RBIContainer *acc = NULL;
		int			w = 0;

		/* The positive sources drive the merge; all must still have data. */
		for (i = 0; i < nsources; i++)
		{
			if (sources[i].negated)
				continue;
			if (!cursors[i].valid)
				goto merge_done;
			if (!havemax || cursors[i].ckey > maxckey)
			{
				maxckey = cursors[i].ckey;
				havemax = true;
			}
		}

		for (i = 0; i < nsources; i++)
		{
			cursors[i].advance = false;
			if (!sources[i].negated && cursors[i].ckey != maxckey)
				alleq = false;
		}

		if (!alleq)
		{
			/*
			 * A ckey that is missing from some set contributes nothing, so
			 * the lagging containers carry no visibility-map obligation and
			 * their pages may be let go straight away.
			 */
			for (i = 0; i < nsources; i++)
			{
				if (!sources[i].negated && cursors[i].ckey < maxckey)
					rbi_ecursor_next(&cursors[i]);
			}
			CHECK_FOR_INTERRUPTS();
			continue;
		}

		/* The intersection of the positive sources ... */
		for (i = 0; i < nsources; i++)
		{
			if (sources[i].negated)
				continue;
			cursors[i].advance = true;
			if (acc == NULL)
				acc = cursors[i].cur;
			else
			{
				rbi_container_and(acc, cursors[i].cur, work[w]);
				acc = work[w];
				w ^= 1;
			}
		}

		/* ... minus the negated ones (DESIGN.md §14, `col IS NOT NULL`). */
		for (i = 0; i < nsources; i++)
		{
			if (!sources[i].negated)
				continue;
			while (cursors[i].valid && cursors[i].ckey < maxckey)
				rbi_ecursor_next(&cursors[i]);	/* nothing to subtract there */
			if (!cursors[i].valid || cursors[i].ckey != maxckey)
				continue;

			cursors[i].advance = true;
			if (rbi_container_cardinality(acc) > 0)
			{
				rbi_container_andnot(acc, cursors[i].cur, work[w]);
				acc = work[w];
				w ^= 1;
			}
		}

		if (rbi_container_cardinality(acc) > 0)
			rbi_count_container(&cx, acc, cursors, nsources);
		else
		{
			/*
			 * Nothing of this container key survives, so no visibility-map
			 * question is asked about it and the source pages may go.
			 */
			for (i = 0; i < nsources; i++)
			{
				if (cursors[i].advance)
					rbi_ecursor_next(&cursors[i]);
			}
		}
		CHECK_FOR_INTERRUPTS();
	}

merge_done:
	for (i = 0; i < nsources; i++)
		rbi_ecursor_close(&cursors[i]);

	/*
	 * Everything that could be answered from the visibility map has been;
	 * what is left needs the heap and the snapshot.  No index page is pinned
	 * any more, which is fine: the decisions that needed a pin were all made
	 * above.
	 */
	result = cx.count + rbi_recheck_heap(&cx);

	if (BufferIsValid(cx.vmbuf))
		ReleaseBuffer(cx.vmbuf);

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);

	if (stats != NULL)
	{
		stats->blocks_skipped_via_vm += cx.stats.blocks_skipped_via_vm;
		stats->tids_rechecked += cx.stats.tids_rechecked;
		stats->blocks_rechecked += cx.stats.blocks_rechecked;
		stats->containers_visited += cx.stats.containers_visited;
	}

	return result;
}

bool
rbi_sets_satisfiable(int nsets, RBIPostingSet *sets, RBIKeyNode *tree)
{
	RBICountSource src;

	memset(&src, 0, sizeof(src));
	src.nsets = nsets;
	src.sets = sets;
	src.tree = tree;

	return rbi_source_satisfiable(rbi_source_tree(&src), sets);
}

/*
 * Walk the containers of one expression over located posting sets, without
 * any visibility-map interlock: what a bitmap scan of a multi-key opclass
 * needs (DESIGN.md §17).  Every TID goes to the executor, which visits the
 * heap for all of them, so no pin has anything to protect here - but the
 * cursors take and drop their pins exactly as they do for a count, which is
 * why the very same evaluator serves both.
 */
int64
rbi_sets_iterate(int nsets, RBIPostingSet *sets, RBIKeyNode *tree,
				 rbi_container_callback cb, void *arg)
{
	RBICountSource src;
	RBIExprCursor cursor;
	RBICountCtx cx;
	RBIKeyNode *node;
	int64		total = 0;

	memset(&src, 0, sizeof(src));
	src.nsets = nsets;
	src.sets = sets;
	src.tree = tree;

	node = rbi_source_tree(&src);
	if (node == NULL)
		return 0;

	memset(&cx, 0, sizeof(cx));
	cx.vmbuf = InvalidBuffer;

	rbi_ecursor_init(&cursor, node, sets, nsets, &cx);

	while (cursor.valid)
	{
		total += (int64) rbi_container_cardinality(cursor.cur);
		if (!cb(cursor.cur, arg))
			break;
		rbi_ecursor_next(&cursor);
		CHECK_FOR_INTERRUPTS();
	}

	rbi_ecursor_close(&cursor);

	return total;
}

/*
 * The plain form: count the intersection of nsets posting sets.
 */
int64
rbi_count_posting_sets(Relation heap, Snapshot snapshot, int nsets,
					   RBIPostingSet *sets, RBICountStats *stats)
{
	RBICountSource *sources;
	int64		result;
	int			i;

	Assert(nsets >= 1);

	sources = (RBICountSource *) palloc0(sizeof(RBICountSource) * nsets);
	for (i = 0; i < nsets; i++)
	{
		sources[i].nsets = 1;
		sources[i].sets = &sets[i];
		sources[i].negated = false;
	}

	result = rbi_count_sources(heap, snapshot, nsets, sources, stats);

	pfree(sources);
	return result;
}

int64
rbi_count_keys(Relation heap, Snapshot snapshot, int nkeys, Relation *indexes,
			   Datum *keys, Oid *keytypes, RBICountStats *stats)
{
	RBIPostingSet *sets;
	int64		result;
	int			i;

	Assert(nkeys >= 1);

	sets = (RBIPostingSet *) palloc0(sizeof(RBIPostingSet) * nkeys);

	for (i = 0; i < nkeys; i++)
		rbi_posting_set_lookup(indexes[i], keys[i],
							   keytypes ? keytypes[i] : InvalidOid,
							   &sets[i]);

	result = rbi_count_posting_sets(heap, snapshot, nkeys, sets, stats);

	for (i = 0; i < nkeys; i++)
		rbi_posting_set_release(&sets[i]);

	pfree(sets);
	return result;
}


/* ---------------------------------------------------------------------
 * Iterating every entry of an index
 * --------------------------------------------------------------------- */

void
rbi_entry_scan_begin(RBIEntryScan *es, Relation index)
{
	es->index = index;
	es->state = rbi_get_state(index);
	es->bucket = 0;
	es->blkno = RBI_BUCKET_BLKNO(0);
	es->off = FirstOffsetNumber;
	es->done = false;
}

bool
rbi_entry_scan_next(RBIEntryScan *es, Datum *key, RBIPostingSet *ps)
{
	RBIState   *state = es->state;

	while (!es->done)
	{
		Buffer		buf;
		Page		page;
		OffsetNumber maxoff;
		BlockNumber next;
		bool		got = false;

		buf = ReadBuffer(es->index, es->blkno);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (!RBIPageIsBucket(page))
		{
			UnlockReleaseBuffer(buf);
			elog(ERROR, "roaring index: block %u is not a bucket page",
				 es->blkno);
		}
		maxoff = PageGetMaxOffsetNumber(page);

		while (es->off <= maxoff)
		{
			ItemId		iid = PageGetItemId(page, es->off);
			RBIEntryTuple *entry;
			OffsetNumber thisoff = es->off;
			bool		keeppin;

			es->off = OffsetNumberNext(es->off);

			if (!ItemIdIsUsed(iid))
				continue;
			entry = (RBIEntryTuple *) PageGetItem(page, iid);

			/*
			 * VACUUM keeps entries whose posting set has become empty
			 * (DESIGN.md section 5); they can never produce a group.
			 */
			if (entry->ntids == 0)
				continue;

			rbi_fill_posting_set(es->index, state, buf, thisoff, ps, &keeppin);
			*key = ps->storedkey;
			if (keeppin)
			{
				/*
				 * DESIGN.md section 9: the INLINE payload we just copied out
				 * needs a pin of its own on this bucket page, independent of
				 * the scan's position.
				 */
				IncrBufferRefCount(buf);
				ps->pinbuf = buf;
			}

			got = true;
			break;
		}

		next = RBIPageGetOpaque(page)->rightlink;
		UnlockReleaseBuffer(buf);

		if (got)
			return true;

		/* This bucket page is done: next page of the chain, or next bucket. */
		if (BlockNumberIsValid(next))
			es->blkno = next;
		else if (++es->bucket < state->meta.nbuckets)
			es->blkno = RBI_BUCKET_BLKNO(es->bucket);
		else
		{
			es->done = true;
			break;
		}
		es->off = FirstOffsetNumber;

		CHECK_FOR_INTERRUPTS();
	}

	return false;
}

void
rbi_entry_scan_end(RBIEntryScan *es)
{
	es->done = true;
}


/* ---------------------------------------------------------------------
 * SQL interface
 * --------------------------------------------------------------------- */

/*
 * Common argument validation for the roaring_index_count* functions.
 * Everything is opened here and closed by rbi_count_sql_close().
 */
typedef struct RBICountCall
{
	int			nkeys;
	Relation	heap;
	Relation	index[2];
	Datum		key[2];
	Oid			keytype[2];
} RBICountCall;

static void
rbi_count_sql_open(FunctionCallInfo fcinfo, int nkeys, RBICountCall *call)
{
	Oid			heapoid = InvalidOid;
	int			i;

	call->nkeys = nkeys;
	call->heap = NULL;
	for (i = 0; i < 2; i++)
		call->index[i] = NULL;

	for (i = 0; i < nkeys; i++)
	{
		Oid			idxoid = PG_GETARG_OID(2 * i);
		Oid			hoid;

		call->key[i] = PG_GETARG_DATUM(2 * i + 1);
		call->keytype[i] = get_fn_expr_argtype(fcinfo->flinfo, 2 * i + 1);
		if (!OidIsValid(call->keytype[i]))
			elog(ERROR, "could not determine the type of the search key");

		if (get_rel_relkind(idxoid) != RELKIND_INDEX)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is not an index", get_rel_name(idxoid))));

		hoid = IndexGetRelation(idxoid, false);
		if (i == 0)
			heapoid = hoid;
		else if (hoid != heapoid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("indexes \"%s\" and \"%s\" are not on the same table",
							get_rel_name(PG_GETARG_OID(0)),
							get_rel_name(idxoid))));
	}

	call->heap = table_open(heapoid, AccessShareLock);

	for (i = 0; i < nkeys; i++)
	{
		Relation	index = index_open(PG_GETARG_OID(2 * i), AccessShareLock);

		call->index[i] = index;

		if (index->rd_rel->relam != rbi_get_am_oid())
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("index \"%s\" is not a roaring index",
							RelationGetRelationName(index))));
		if (IndexRelationGetNumberOfKeyAttributes(index) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("roaring index \"%s\" must have exactly one key column",
							RelationGetRelationName(index))));

		/*
		 * The key must be the index's own type, or a type the opfamily can
		 * compare it with (integer cross-type equality, for instance).
		 * rbi_probe_prepare() would raise the same errors later; raising them
		 * here keeps them out of the middle of the count.
		 */
		if (call->keytype[i] != index->rd_opcintype[0] &&
			!OidIsValid(get_opfamily_member(index->rd_opfamily[0],
											index->rd_opcintype[0],
											call->keytype[i], 1)))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("type %s cannot be compared with index \"%s\"",
							format_type_be(call->keytype[i]),
							RelationGetRelationName(index)),
					 errdetail("The index is on type %s.",
							   format_type_be(index->rd_opcintype[0]))));
	}
}

static void
rbi_count_sql_close(RBICountCall *call)
{
	int			i;

	for (i = 0; i < call->nkeys; i++)
	{
		if (call->index[i] != NULL)
			index_close(call->index[i], AccessShareLock);
	}
	if (call->heap != NULL)
		table_close(call->heap, AccessShareLock);
}

static int64
rbi_count_sql(FunctionCallInfo fcinfo, int nkeys, RBICountStats *stats)
{
	RBICountCall call;
	Snapshot	snapshot;
	int64		result;

	rbi_count_sql_open(fcinfo, nkeys, &call);

	snapshot = GetActiveSnapshot();
	if (snapshot == NULL)
		elog(ERROR, "roaring index count requires an active snapshot");

	result = rbi_count_keys(call.heap, snapshot, nkeys, call.index,
							call.key, call.keytype, stats);

	rbi_count_sql_close(&call);
	return result;
}

Datum
roaring_index_count(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(rbi_count_sql(fcinfo, 1, NULL));
}

Datum
roaring_index_count2(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(rbi_count_sql(fcinfo, 2, NULL));
}

Datum
roaring_index_count_stats(PG_FUNCTION_ARGS)
{
	RBICountStats stats;
	TupleDesc	tupdesc;
	Datum		values[4];
	bool		nulls[4] = {false, false, false, false};
	int64		count;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	memset(&stats, 0, sizeof(stats));
	count = rbi_count_sql(fcinfo, 1, &stats);

	values[0] = Int64GetDatum(count);
	values[1] = Int64GetDatum(stats.blocks_skipped_via_vm);
	values[2] = Int64GetDatum(stats.tids_rechecked);
	values[3] = Int64GetDatum(stats.blocks_rechecked);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}
