/*-------------------------------------------------------------------------
 *
 * rbi_insert.c
 *		aminsert for the roaring index (DESIGN.md section 5, INSERT).
 *
 * One TID is added to the posting set of one key.  The bucket head page is
 * held EXCLUSIVE for the whole operation, so all writers of a key (inserts
 * and VACUUM) are serialised against each other while readers of other
 * buckets are unaffected.
 *
 * There are three cases:
 *
 *	- the key has no entry yet: add an INLINE entry holding one one-pair
 *	  sparse segment;
 *	- the key has an INLINE entry: rebuild its payload with the member added
 *	  and overwrite the entry; if the payload outgrows inline_limit or the
 *	  bucket page, spill the posting set onto container pages and fall through
 *	  to the CHAIN case;
 *	- the key has a CHAIN entry: find the container page that owns the ckey,
 *	  copy the item out, add the member and write it back, splitting the
 *	  page if it no longer fits.  A BITSET container is the common case for a
 *	  low-cardinality key and never changes size, so it takes a fast path that
 *	  sets the bit in the page image instead of copying 4104 bytes out and
 *	  back (rbi_insert_bitset_inplace()).
 *
 * An item is a container or a sparse segment (DESIGN.md §13), and which one
 * owns a ckey decides what the insert does:
 *
 *	- a container with that ckey: as before.
 *	- a sparse segment whose range covers the ckey: the pair goes in, and if
 *	  that brings the ckey to RBI_SPARSE_THRESHOLD members it is promoted to
 *	  a container of its own and the segment is split around it.  A segment
 *	  that is already at RBI_SPARSE_MAX_PAIRS splits in half first.
 *	- nothing owns the ckey: the pair joins the segment immediately before it
 *	  (preferred, because a segment grows to the right cheaply), else the one
 *	  immediately after it, else it becomes a new one-pair segment.  Nothing
 *	  lies between two adjacent items, so joining either of them cannot make
 *	  item ranges overlap or interleave.
 *
 * A NULL key is no different once its entry has been located: it lives in the
 * reserved NULL entry of bucket 0, which has no key bytes and is recognised
 * by RBI_ENTRY_NULLKEY (DESIGN.md §14), and its payload is an ordinary
 * INLINE payload that spills to a chain like any other.  So is the reserved
 * EMPTY entry a multi-key opclass uses for rows it extracts no key from
 * (DESIGN.md §17); a row with several keys is simply several of these
 * inserts, one per key.
 *
 * Splitting a segment produces up to three items where there was one, and
 * they are placed in a single WAL record (rbi_chain_put_items_locked): a
 * crash must never leave the same ckey in two items.
 *
 * Every page modification goes through GenericXLog, and the entry tuple is
 * always updated in the same record as the items it describes.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/generic_xlog.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "varatt.h"

#include "rbi.h"

static void rbi_insert_new_entry(Relation index, RBIState *state, Buffer headbuf,
								 Datum key, uint16 reservedflag, uint32 hash,
								 uint32 ckey, uint16 lo);
static void rbi_insert_inline(Relation index, RBIState *state, Buffer entrybuf,
							  OffsetNumber entryoff, uint32 ckey, uint16 lo);
static void rbi_insert_chain(Relation index, Buffer entrybuf,
							 OffsetNumber entryoff, uint32 ckey, uint16 lo);
static bool rbi_insert_bitset_inplace(Relation index, Buffer buf, OffsetNumber off,
									  Buffer entrybuf, OffsetNumber entryoff,
									  RBIEntryTuple *entry, Size entrysize,
									  uint16 lo, bool *done);
static void rbi_insert_segment(Relation index, Buffer buf, OffsetNumber off,
							   bool replace, Buffer entrybuf,
							   OffsetNumber entryoff, RBIEntryTuple *entry,
							   uint32 ckey, uint16 lo);

/*
 * Scratch buffers and result of adding one pair to a sparse segment.
 *
 * seg holds the segment being changed; the other three are where the items
 * that replace it are built.  items[] points into these buffers and is what
 * goes on the page, in ascending ckey order.
 */
typedef struct RBISegWork
{
	RBIContainer *seg;			/* the segment, mutated in place */
	RBIContainer *cont;			/* container promoted out of it */
	RBIContainer *left;
	RBIContainer *right;
	RBIContainer *items[RBI_MAX_PUT_ITEMS];
	int			nitems;
} RBISegWork;

/*
 * The buffers are allocated on demand: most inserts never touch a segment at
 * all, and the ones that do usually only need seg.  Four 4104-byte buffers
 * per insert would be four separate allocations on the hot path for nothing.
 */
static void
rbi_segwork_init(RBISegWork *w)
{
	w->seg = NULL;
	w->cont = NULL;
	w->left = NULL;
	w->right = NULL;
	w->nitems = 0;
}

static RBIContainer *
rbi_segwork_buf(RBIContainer **p)
{
	if (*p == NULL)
		*p = (RBIContainer *) palloc(RBI_CONTAINER_MAX_SIZE);
	return *p;
}

/* Does this item accept one more pair? */
static bool
rbi_segment_has_room(const RBIContainer *item)
{
	return item->type == RBI_CT_SPARSE &&
		item->cardinality < RBI_SPARSE_MAX_PAIRS;
}

/*
 * Add (ckey, lo) to the segment in w->seg and build the items that must take
 * its place, leaving them in w->items[0 .. w->nitems).  Returns false when
 * the pair was already there, in which case nothing has to be written.
 *
 * The threshold test comes first, on purpose: promoting the ckey to a
 * container *shrinks* the segment, so a segment that is at its maximum needs
 * splitting only when the pair really stays in it, and one insert can never
 * need more than RBI_MAX_PUT_ITEMS items.
 */
static bool
rbi_segment_add(RBISegWork *w, uint32 ckey, uint16 lo)
{
	uint32		cnt;

	Assert(w->seg->type == RBI_CT_SPARSE);
	w->nitems = 0;

	if (rbi_sparse_contains(w->seg, ckey, lo))
		return false;			/* already indexed */

	cnt = rbi_sparse_count(w->seg, ckey);

	if (cnt + 1 >= RBI_SPARSE_THRESHOLD)
	{
		/* Dense enough: the ckey becomes a container of its own. */
		(void) rbi_segwork_buf(&w->cont);
		(void) rbi_segwork_buf(&w->left);
		(void) rbi_segwork_buf(&w->right);

		if (rbi_sparse_extract(w->seg, ckey, w->cont) != cnt)
			elog(ERROR, "roaring index: sparse segment lost container key %u",
				 ckey);
		if (!rbi_container_add(w->cont, lo))
			elog(ERROR, "roaring index: duplicate member in sparse segment for container key %u",
				 ckey);
		rbi_container_optimize(w->cont);

		rbi_sparse_split_at(w->seg, ckey, w->left, w->right);

		if (w->left->cardinality > 0)
			w->items[w->nitems++] = w->left;
		w->items[w->nitems++] = w->cont;
		if (w->right->cardinality > 0)
			w->items[w->nitems++] = w->right;
	}
	else if (rbi_sparse_insert(w->seg, ckey, lo, NULL))
	{
		w->items[w->nitems++] = w->seg;
	}
	else if (rbi_sparse_split_half(w->seg, rbi_segwork_buf(&w->left),
								   rbi_segwork_buf(&w->right)))
	{
		/* Full: split at a container key boundary, then insert in one half. */
		RBIContainer *half = (ckey <= rbi_item_last_ckey(w->left)) ?
			w->left : w->right;

		if (!rbi_sparse_insert(half, ckey, lo, NULL))
			elog(ERROR, "roaring index: half of a split sparse segment is full");

		w->items[w->nitems++] = w->left;
		w->items[w->nitems++] = w->right;
	}
	else
	{
		/*
		 * A full segment whose pairs all have one container key, which the
		 * threshold policy should have turned into a container long ago.
		 * Do it now rather than fail: the ckey becomes a container and the
		 * new pair a segment of its own, on the correct side of it.
		 */
		uint32		only = w->seg->ckey;

		(void) rbi_segwork_buf(&w->cont);
		(void) rbi_sparse_extract(w->seg, only, w->cont);
		rbi_container_optimize(w->cont);
		Assert(w->seg->cardinality == 0);

		rbi_sparse_init(w->left, ckey);
		if (!rbi_sparse_insert(w->left, ckey, lo, NULL))
			elog(ERROR, "roaring index: empty sparse segment rejected a pair");

		if (ckey < only)
		{
			w->items[w->nitems++] = w->left;
			w->items[w->nitems++] = w->cont;
		}
		else
		{
			w->items[w->nitems++] = w->cont;
			w->items[w->nitems++] = w->left;
		}
	}

	Assert(w->nitems >= 1 && w->nitems <= RBI_MAX_PUT_ITEMS);
	return true;
}

/*
 * Add (ckey, lo) through the sparse segment at (buf, off), or -- with replace
 * false -- as a brand new one-pair segment inserted at off.
 *
 * buf is the container page and entrybuf the entry's bucket page, both held
 * EXCLUSIVE by the caller and still locked on return; entry is the caller's
 * private copy of the entry tuple, whose counters are updated here and
 * written in the same WAL record as the items.
 */
static void
rbi_insert_segment(Relation index, Buffer buf, OffsetNumber off, bool replace,
				   Buffer entrybuf, OffsetNumber entryoff,
				   RBIEntryTuple *entry, uint32 ckey, uint16 lo)
{
	Page		page = BufferGetPage(buf);
	RBISegWork	w;

	rbi_segwork_init(&w);

	if (replace)
	{
		ItemId		iid = PageGetItemId(page, off);
		Size		isize = ItemIdGetLength(iid);

		if (isize > RBI_CONTAINER_MAX_SIZE)
			elog(ERROR, "roaring index: item of %zu bytes at %u/%u", isize,
				 BufferGetBlockNumber(buf), off);
		memcpy(rbi_segwork_buf(&w.seg), PageGetItem(page, iid), isize);

		if (w.seg->type != RBI_CT_SPARSE)
			elog(ERROR, "roaring index: item %u on block %u is not a sparse segment",
				 off, BufferGetBlockNumber(buf));

		if (!rbi_segment_add(&w, ckey, lo))
			return;				/* already indexed: nothing changes */
	}
	else
	{
		rbi_sparse_init(rbi_segwork_buf(&w.seg), ckey);
		if (!rbi_sparse_insert(w.seg, ckey, lo, NULL))
			elog(ERROR, "roaring index: empty sparse segment rejected a pair");
		w.items[0] = w.seg;
		w.nitems = 1;
	}

	entry->ntids += 1;
	entry->ncontainers += (uint32) (w.nitems - (replace ? 1 : 0));

	rbi_chain_put_items_locked(index, buf, entrybuf, entryoff, entry, off,
							   replace, w.items, w.nitems);
}

/*
 * Write a one-pair sparse segment for (ckey, lo) at dst, which need not be
 * aligned.  Returns the number of bytes written.
 */
static Size
rbi_put_singleton(char *dst, uint32 ckey, uint16 lo)
{
	union
	{
		RBIContainer c;
		uint64		force_align;
		char		data[RBI_CONTAINER_HDRSZ + sizeof(uint32) + sizeof(uint16)];
	}			buf;
	Size		csize;

	rbi_sparse_init(&buf.c, ckey);
	if (!rbi_sparse_insert(&buf.c, ckey, lo, NULL))
		elog(ERROR, "roaring index: empty sparse segment rejected a pair");
	csize = rbi_sparse_size(&buf.c);

	Assert(csize <= sizeof(buf.data));
	memcpy(dst, &buf.c, csize);

	return csize;
}

/*
 * Rebuild an INLINE payload with (ckey, lo) added, writing it to out, which
 * must have room for paylen + RBI_CONTAINER_MAX_SIZE bytes (a container grows
 * by at most that much; splitting a segment into three items grows the
 * payload by at most 26 bytes, and a new segment adds 14).
 *
 * Returns the length of the new payload, or 0 if the member was already
 * there, in which case out is undefined.  *ndelta receives the change in the
 * number of items, which is -0 .. +2.
 */
static Size
rbi_inline_add(const char *payload, Size paylen, uint32 ckey, uint16 lo,
			   char *out, int *ndelta)
{
	RBIContainer *cbuf = (RBIContainer *) palloc(RBI_CONTAINER_MAX_SIZE);
	RBISegWork	w;
	Size		off;
	Size		used = 0;
	Size		csize;
	int			n = 0;
	int			target = -1;	/* item to replace */
	int			insertpos = -1; /* item to insert a new segment before */
	int			previdx = -1;
	bool		prev_ok = false;

	*ndelta = 0;
	rbi_segwork_init(&w);

	/*
	 * Pass 1: decide which item takes the pair.  The items are ordered by
	 * first ckey and their ranges do not overlap, so the walk stops either on
	 * the item that covers the ckey or on the first item beyond it.
	 */
	off = 0;
	while (rbi_inline_fetch(payload, paylen, &off, cbuf) > 0)
	{
		if (rbi_item_covers(cbuf, ckey))
		{
			target = n;
			break;
		}
		if (rbi_item_first_ckey(cbuf) > ckey)
		{
			/* In the gap: the segment before it, else the one after it. */
			if (prev_ok)
				target = previdx;
			else if (rbi_segment_has_room(cbuf))
				target = n;
			else
				insertpos = n;
			break;
		}
		prev_ok = rbi_segment_has_room(cbuf);
		previdx = n;
		n++;
	}

	if (target < 0 && insertpos < 0)
	{
		/* Past the last item. */
		if (prev_ok)
			target = previdx;
		else
			insertpos = n;
	}

	/* Pass 2: copy the payload, replacing or inserting where pass 1 said. */
	off = 0;
	n = 0;
	while ((csize = rbi_inline_fetch(payload, paylen, &off, cbuf)) > 0)
	{
		if (n == insertpos)
		{
			used += rbi_put_singleton(out + used, ckey, lo);
			*ndelta = 1;
			insertpos = -1;
		}

		if (n == target)
		{
			if (cbuf->type == RBI_CT_SPARSE)
			{
				int			i;

				memcpy(rbi_segwork_buf(&w.seg), cbuf, csize);
				if (!rbi_segment_add(&w, ckey, lo))
				{
					pfree(cbuf);
					return 0;	/* already indexed */
				}

				for (i = 0; i < w.nitems; i++)
				{
					Size		isz = rbi_item_size(w.items[i]);

					memcpy(out + used, w.items[i], isz);
					used += isz;
				}
				*ndelta = w.nitems - 1;
				n++;
				continue;
			}

			/* A container with this very ckey. */
			Assert(cbuf->ckey == ckey);
			if (!rbi_container_add(cbuf, lo))
			{
				pfree(cbuf);
				return 0;		/* already indexed */
			}
			csize = rbi_container_size(cbuf);
		}

		memcpy(out + used, cbuf, csize);
		used += csize;
		n++;
	}

	if (insertpos >= 0)
	{
		/* The payload was empty, or the new segment goes at the end. */
		used += rbi_put_singleton(out + used, ckey, lo);
		*ndelta = 1;
	}

	pfree(cbuf);
	return used;
}

/*
 * The key is not in the index yet: add an INLINE entry with a single
 * one-pair sparse segment.  headbuf is held EXCLUSIVE.  With isnull the entry
 * is the reserved NULL-key entry of bucket 0 (DESIGN.md §14).
 */
static void
rbi_insert_new_entry(Relation index, RBIState *state, Buffer headbuf,
					 Datum key, uint16 reservedflag, uint32 hash, uint32 ckey,
					 uint16 lo)
{
	char		payload[RBI_CONTAINER_HDRSZ + sizeof(uint32) + sizeof(uint16)];
	RBIEntryTuple *entry;
	Size		paylen;
	Size		size;
	int			max_entries = rbi_max_entries(index);

	paylen = rbi_put_singleton(payload, ckey, lo);

	entry = (reservedflag != 0) ?
		rbi_make_reserved_entry(reservedflag, RBI_ENTRY_INLINE, payload,
								paylen, &size) :
		rbi_make_entry(state, key, hash, RBI_ENTRY_INLINE,
					   payload, paylen, &size);
	entry->ncontainers = 1;
	entry->ntids = 1;

	/*
	 * Cardinality guard (DESIGN.md §17), on the one path that can make the
	 * index grow a key: a brand new entry.
	 *
	 * The estimate is deliberately crude - the entries of THIS bucket times
	 * the bucket count - because counting the whole index would mean reading
	 * every bucket page for every new key.  Hash values spread the keys
	 * evenly enough for the product to have the right order of magnitude, and
	 * an advisory warning is all it feeds.  The bucket is already locked
	 * EXCLUSIVE by the caller, so the walk sees a consistent chain and costs
	 * nothing but the pages the insert is about to touch anyway.
	 */
	if (max_entries > 0)
	{
		int64		nbucket = rbi_bucket_nentries(index, headbuf) + 1;
		int64		estimate = nbucket * (int64) state->meta.nbuckets;

		if (estimate > (int64) max_entries)
			rbi_warn_max_entries(index, estimate);
	}

	rbi_add_entry(index, headbuf, entry, size);

	pfree(entry);
}

/*
 * Add (ckey, lo) to the INLINE entry at (entrybuf, entryoff), spilling the
 * posting set onto container pages if it no longer fits.  entrybuf is held
 * EXCLUSIVE.
 */
static void
rbi_insert_inline(Relation index, RBIState *state, Buffer entrybuf,
				  OffsetNumber entryoff, uint32 ckey, uint16 lo)
{
	Page		page = BufferGetPage(entrybuf);
	ItemId		iid = PageGetItemId(page, entryoff);
	RBIEntryTuple *entry = (RBIEntryTuple *) PageGetItem(page, iid);
	Size		paylen = RBI_ENTRY_PAYLOAD_LEN(entry, ItemIdGetLength(iid));
	uint32		ncontainers = entry->ncontainers;
	uint64		ntids = entry->ntids;
	char	   *oldpay;
	char	   *newpay;
	Size		newlen;
	int			ndelta;

	/*
	 * The payload is copied out of the page: a spill rewrites the entry while
	 * the old items are still being read.
	 */
	oldpay = (char *) palloc(paylen);
	memcpy(oldpay, RBIEntryGetPayload(entry), paylen);

	newpay = (char *) palloc(paylen + RBI_CONTAINER_MAX_SIZE);
	newlen = rbi_inline_add(oldpay, paylen, ckey, lo, newpay, &ndelta);

	if (newlen == 0)
	{
		/* The TID is already in this posting set; nothing to do. */
		pfree(newpay);
		pfree(oldpay);
		return;
	}

	if (newlen <= (Size) state->meta.inline_limit)
	{
		RBIEntryTuple *newentry;
		Size		newsize;
		bool		ok;

		newentry = rbi_entry_rebuild(entry, newpay, newlen, &newsize);
		newentry->ncontainers = (uint32) ((int) ncontainers + ndelta);
		newentry->ntids = ntids + 1;

		ok = rbi_replace_entry(index, NULL, entrybuf, entryoff, newentry,
							   newsize);
		pfree(newentry);

		if (ok)
		{
			pfree(newpay);
			pfree(oldpay);
			return;
		}
	}

	/*
	 * The posting set has outgrown the entry tuple (or the bucket page).
	 * Move the containers it had onto a chain, then add the new member
	 * through the chain path.
	 */
	{
		RBIEntryTuple *chain;
		Size		chainsize;

		chain = rbi_entry_rebuild(entry, NULL, 0, &chainsize);
		chain->ncontainers = ncontainers;
		chain->ntids = ntids;

		rbi_entry_spill(index, entrybuf, entryoff, chain, oldpay, paylen);
		pfree(chain);
	}

	pfree(newpay);
	pfree(oldpay);

	rbi_insert_chain(index, entrybuf, entryoff, ckey, lo);
}

/*
 * Fast path for adding a member to a BITSET container that is already on a
 * page.  A bitset is always RBI_CONTAINER_MAX_SIZE bytes whatever its
 * cardinality, so the bit can be set directly in the page image instead of
 * copying 4104 bytes into a work buffer and the same 4104 bytes back through
 * PageIndexTupleOverwrite.  Nothing else about the page changes: the item
 * keeps its size and its offset, and minckey/maxckey keep their values
 * because the container keeps its ckey.
 *
 * Returns false if the container at (buf, off) is not a bitset; then nothing
 * has been done and the caller takes the general path.  Returns true with
 * *done set when the change (or the discovery that the TID was already
 * indexed) is complete.  buf and entrybuf are held EXCLUSIVE throughout and
 * stay locked; entry is the caller's private copy of the entry tuple, which
 * is written in the same WAL record as the container.
 */
static bool
rbi_insert_bitset_inplace(Relation index, Buffer buf, OffsetNumber off,
						  Buffer entrybuf, OffsetNumber entryoff,
						  RBIEntryTuple *entry, Size entrysize, uint16 lo,
						  bool *done)
{
	Page		page = BufferGetPage(buf);
	ItemId		iid = PageGetItemId(page, off);
	RBIContainer *onpage = (RBIContainer *) PageGetItem(page, iid);
	GenericXLogState *xstate;
	Page		p;
	RBIContainer *c;

	if (onpage->type != RBI_CT_BITSET)
		return false;

	Assert(ItemIdGetLength(iid) == RBI_CONTAINER_MAX_SIZE);
	*done = true;

	/* Already indexed: do not start a record at all. */
	if (rbi_container_contains(onpage, lo))
		return true;

	xstate = GenericXLogStart(index);
	p = GenericXLogRegisterBuffer(xstate, buf, 0);
	c = (RBIContainer *) PageGetItem(p, PageGetItemId(p, off));

	if (!rbi_container_add(c, lo))
		elog(ERROR, "roaring index: bitset container %u on block %u changed under an exclusive lock",
			 c->ckey, BufferGetBlockNumber(buf));

	entry->ntids += 1;

	/* The entry always travels with the container change. */
	if (!rbi_replace_entry(index, xstate, entrybuf, entryoff, entry, entrysize))
		elog(ERROR, "roaring index: could not update entry %u on block %u",
			 entryoff, BufferGetBlockNumber(entrybuf));

	GenericXLogFinish(xstate);

	return true;
}

/*
 * Add (ckey, lo) to the CHAIN entry at (entrybuf, entryoff), which is held
 * EXCLUSIVE.
 */
static void
rbi_insert_chain(Relation index, Buffer entrybuf, OffsetNumber entryoff,
				 uint32 ckey, uint16 lo)
{
	Page		page = BufferGetPage(entrybuf);
	ItemId		iid = PageGetItemId(page, entryoff);
	RBIEntryTuple *entry = (RBIEntryTuple *) PageGetItem(page, iid);
	RBIEntryTuple *ecopy;
	RBIContainer *cbuf;
	Size		esize;
	BlockNumber blk;
	Buffer		buf;
	Page		cpage;
	OffsetNumber off;
	bool		found;
	int			delta;

	ecopy = rbi_entry_rebuild(entry, NULL, 0, &esize);
	Assert(esize == ItemIdGetLength(iid));
	Assert((ecopy->flags & RBI_ENTRY_CHAIN) != 0);

	cbuf = (RBIContainer *) palloc(RBI_CONTAINER_MAX_SIZE);

	blk = rbi_chain_find_page(index, ecopy->head, ecopy->tail, ckey);
	buf = ReadBuffer(index, blk);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	cpage = BufferGetPage(buf);

	if (!RBIPageIsContainer(cpage))
	{
		UnlockReleaseBuffer(buf);
		elog(ERROR, "roaring index: block %u is not a container page", blk);
	}

	/* Which item owns this ckey: a container, a segment, or nothing yet? */
	off = rbi_page_find_item(cpage, ckey, &found);

	if (found &&
		((RBIContainer *) PageGetItem(cpage, PageGetItemId(cpage, off)))->type
		== RBI_CT_SPARSE)
	{
		rbi_insert_segment(index, buf, off, true, entrybuf, entryoff, ecopy,
						   ckey, lo);
		UnlockReleaseBuffer(buf);
		pfree(cbuf);
		pfree(ecopy);
		return;
	}

	if (!found)
	{
		/*
		 * Nothing covers the ckey.  Prefer the segment immediately before the
		 * gap, then the one immediately after it; both are adjacent items, so
		 * nothing can end up between the pair and the segment it joins.
		 */
		OffsetNumber maxoff = PageGetMaxOffsetNumber(cpage);
		OffsetNumber target = InvalidOffsetNumber;

		if (off > FirstOffsetNumber &&
			rbi_segment_has_room((RBIContainer *)
								 PageGetItem(cpage,
											 PageGetItemId(cpage, OffsetNumberPrev(off)))))
			target = OffsetNumberPrev(off);
		else if (off <= maxoff &&
				 rbi_segment_has_room((RBIContainer *)
									  PageGetItem(cpage, PageGetItemId(cpage, off))))
			target = off;

		if (target != InvalidOffsetNumber)
		{
			rbi_insert_segment(index, buf, target, true, entrybuf, entryoff,
							   ecopy, ckey, lo);
			UnlockReleaseBuffer(buf);
			pfree(cbuf);
			pfree(ecopy);
			return;
		}

		/* No segment to join: the pair becomes a new one-pair segment. */
		rbi_insert_segment(index, buf, off, false, entrybuf, entryoff, ecopy,
						   ckey, lo);
		UnlockReleaseBuffer(buf);
		pfree(cbuf);
		pfree(ecopy);
		return;
	}

	/* A container with this ckey. */
	{
		ItemId		ciid = PageGetItemId(cpage, off);
		bool		done = false;

		Assert(((RBIContainer *) PageGetItem(cpage, ciid))->ckey == ckey);

		if (rbi_insert_bitset_inplace(index, buf, off, entrybuf, entryoff,
									  ecopy, esize, lo, &done))
		{
			Assert(done);
			UnlockReleaseBuffer(buf);
			pfree(cbuf);
			pfree(ecopy);
			return;
		}

		memcpy(cbuf, PageGetItem(cpage, ciid), ItemIdGetLength(ciid));
		if (!rbi_container_add(cbuf, lo))
		{
			/* already indexed */
			UnlockReleaseBuffer(buf);
			pfree(cbuf);
			pfree(ecopy);
			return;
		}
	}

	ecopy->ntids += 1;

	rbi_chain_put_container_locked(index, buf, entrybuf, entryoff, ecopy, cbuf,
								   &delta);
	UnlockReleaseBuffer(buf);

	Assert(delta == 0);
	(void) delta;

	pfree(cbuf);
	pfree(ecopy);
}

/*
 * Add one (key, code) pair to the index: locate or create the key's entry and
 * put the member in it.  reservedflag is 0 for a real key, or the flag of the
 * reserved entry the pair belongs to (DESIGN.md §14 and §17), in which case
 * key is meaningless and the bucket is 0.
 */
static void
rbi_insert_one(Relation index, RBIState *state, Datum key, uint16 reservedflag,
			   uint32 ckey, uint16 lo)
{
	uint32		hash;
	Buffer		headbuf;
	Buffer		entrybuf;
	OffsetNumber entryoff;
	bool		found;

	hash = (reservedflag != 0) ? RBI_NULLKEY_HASH : rbi_hash_key(state, key);

	headbuf = ReadBuffer(index,
						 RBI_BUCKET_BLKNO(rbi_bucket_of(hash,
														state->meta.nbuckets)));
	LockBuffer(headbuf, BUFFER_LOCK_EXCLUSIVE);

	found = (reservedflag != 0) ?
		rbi_find_reserved_entry(index, headbuf, BUFFER_LOCK_EXCLUSIVE,
								reservedflag, &entrybuf, &entryoff) :
		rbi_find_entry(index, state, headbuf, BUFFER_LOCK_EXCLUSIVE,
					   key, hash, &entrybuf, &entryoff);

	if (!found)
	{
		rbi_insert_new_entry(index, state, headbuf, key, reservedflag, hash,
							 ckey, lo);
	}
	else
	{
		RBIEntryTuple *entry;

		entry = (RBIEntryTuple *) PageGetItem(BufferGetPage(entrybuf),
											  PageGetItemId(BufferGetPage(entrybuf),
															entryoff));

		if ((entry->flags & RBI_ENTRY_INLINE) != 0)
			rbi_insert_inline(index, state, entrybuf, entryoff, ckey, lo);
		else
			rbi_insert_chain(index, entrybuf, entryoff, ckey, lo);

		/* rbi_find_entry() does not pin the head page twice. */
		if (entrybuf != headbuf)
			UnlockReleaseBuffer(entrybuf);
	}

	UnlockReleaseBuffer(headbuf);
}

/*
 * aminsert
 *
 * checkUnique is irrelevant (amcanunique is false) and indexUnchanged is
 * ignored: a roaring index has no way of knowing whether it has seen this
 * TID before without looking, and the lookup is the bulk of the work anyway.
 *
 * A multi-key opclass (DESIGN.md §17) turns one row into several independent
 * single-key inserts, each taking and releasing its own bucket lock.  They are
 * not atomic with respect to a concurrent reader - a scan may see the row
 * under some of its keys and not yet under the others - which is exactly the
 * visibility the heap gives anyway: the inserting transaction has not
 * committed, so no snapshot that can see the row can run before the last of
 * them has been written.
 */
bool
rbiinsert(Relation index, Datum *values, bool *isnull, ItemPointer ht_ctid,
		  Relation heapRel, IndexUniqueCheck checkUnique, bool indexUnchanged,
		  IndexInfo *indexInfo)
{
	RBIState   *state;
	MemoryContext insertcxt;
	MemoryContext oldcxt;
	Datum		key;
	uint64		code;
	uint32		ckey;
	uint16		lo;

	rbi_check_key_offset(ht_ctid);

	state = rbi_get_state(index);

	insertcxt = AllocSetContextCreate(CurrentMemoryContext,
									  "roaring index insert",
									  ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(insertcxt);

	code = rbi_tid_to_code(ht_ctid);
	ckey = rbi_code_ckey(code);
	lo = rbi_code_lo(code);

	/*
	 * A NULL value belongs to the reserved NULL entry, which lives in bucket 0
	 * and is found by its flag rather than by its (absent) key (DESIGN.md
	 * §14).  A multi-key opclass that extracts nothing from a non-NULL value
	 * puts the row in the reserved EMPTY entry the same way (DESIGN.md §17).
	 */
	if (isnull[0])
		rbi_insert_one(index, state, (Datum) 0, RBI_ENTRY_NULLKEY, ckey, lo);
	else if (state->multikey)
	{
		Datum	   *keys;
		int			nkeys = rbi_extract_value(state, values[0], &keys);
		int			i;

		if (nkeys == 0)
			rbi_insert_one(index, state, (Datum) 0, RBI_ENTRY_EMPTYKEY,
						   ckey, lo);
		for (i = 0; i < nkeys; i++)
		{
			rbi_insert_one(index, state, keys[i], 0, ckey, lo);
			CHECK_FOR_INTERRUPTS();
		}
	}
	else
	{
		key = values[0];
		if (!state->typbyval && state->typlen == -1)
			key = PointerGetDatum(PG_DETOAST_DATUM(key));

		rbi_insert_one(index, state, key, 0, ckey, lo);
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(insertcxt);

	/* No uniqueness check was performed. */
	return false;
}
