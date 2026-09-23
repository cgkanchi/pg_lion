/*-------------------------------------------------------------------------
 *
 * lion_insert.c
 *		aminsert for the lion index (DESIGN.md section 5, INSERT).
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
 *	  page if it no longer fits.
 *
 * The last case has an in-place fast path, which is what makes repeated
 * inserts into one key cheap: when the item on the page can take the member
 * without changing the number of bytes the page has allotted it, the member
 * is added directly in the page
 * (lion_insert_container_inplace(), lion_insert_segment_inplace()) under the
 * same locks and in the same single record as the entry tuple.  Nothing else
 * on the page moves, so the WAL delta is a few bytes.  A BITSET container
 * always qualifies (4104 bytes whatever its cardinality); an ARRAY or RUN
 * container and a sparse segment qualify when the item has growth slack
 * inside it (DESIGN.md §4), which the general path leaves behind whenever it
 * writes an item on behalf of an insert.
 *
 * An item is a container or a sparse segment (DESIGN.md §13), and which one
 * owns a ckey decides what the insert does:
 *
 *	- a container with that ckey: as before.
 *	- a sparse segment whose range covers the ckey: the pair goes in, and if
 *	  that brings the ckey to LION_SPARSE_THRESHOLD members it is promoted to
 *	  a container of its own and the segment is split around it.  A segment
 *	  that is already at LION_SPARSE_MAX_PAIRS splits in half first.
 *	- nothing owns the ckey: the pair joins the segment immediately before it
 *	  (preferred, because a segment grows to the right cheaply), else the one
 *	  immediately after it, else it becomes a new one-pair segment.  Nothing
 *	  lies between two adjacent items, so joining either of them cannot make
 *	  item ranges overlap or interleave.
 *
 * A NULL key is no different once its entry has been located: it lives in the
 * reserved NULL entry of bucket 0, which has no key bytes and is recognised
 * by LION_ENTRY_NULLKEY (DESIGN.md §14), and its payload is an ordinary
 * INLINE payload that spills to a chain like any other.  So is the reserved
 * EMPTY entry a multi-key opclass uses for rows it extracts no key from
 * (DESIGN.md §17); a row with several keys is simply several of these
 * inserts, one per key.
 *
 * Splitting a segment produces up to three items where there was one, and
 * they are placed in a single WAL record (lion_chain_put_items_locked): a
 * crash must never leave the same ckey in two items.
 *
 * Every page modification goes through the WAL shim of DESIGN.md §25, and
 * the entry tuple is
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

#include "lion.h"

static void lion_insert_new_entry(Relation index, Relation heaprel,
								 LionState *state, Buffer *leafbuf,
								 OffsetNumber off, bool movedright,
								 Datum key, uint16 reservedflag, uint32 hash,
								 uint32 ckey, uint16 lo);
static void lion_insert_inline(Relation index, Relation heaprel,
							  LionState *state, Buffer entrybuf,
							  OffsetNumber entryoff, uint32 ckey, uint16 lo);
static void lion_insert_chain(Relation index, Relation heaprel, Buffer entrybuf,
							 OffsetNumber entryoff, uint32 ckey, uint16 lo);
static bool lion_insert_container_inplace(Relation index, Buffer buf, OffsetNumber off,
										 Buffer entrybuf, OffsetNumber entryoff,
										 LionEntryTuple *entry, Size entrysize,
										 uint16 lo, bool *done);
static Buffer lion_insert_lock_chain_page(Relation index, Relation heaprel,
										  uint32 hash, BlockNumber head,
										  BlockNumber tail, uint32 ckey);
static bool lion_insert_segment_inplace(Relation index, Buffer buf, OffsetNumber off,
									   Buffer entrybuf, OffsetNumber entryoff,
									   LionEntryTuple *entry, Size entrysize,
									   uint32 ckey, uint16 lo, bool *done);
static void lion_insert_segment(Relation index, Relation heaprel, Buffer buf,
							   OffsetNumber off,
							   bool replace, Buffer entrybuf,
							   OffsetNumber entryoff, LionEntryTuple *entry,
							   uint32 ckey, uint16 lo);

/*
 * Scratch buffers and result of adding one pair to a sparse segment.
 *
 * seg holds the segment being changed; the other three are where the items
 * that replace it are built.  items[] points into these buffers and is what
 * goes on the page, in ascending ckey order.
 */
typedef struct LionSegWork
{
	LionContainer *seg;			/* the segment, mutated in place */
	LionContainer *cont;			/* container promoted out of it */
	LionContainer *left;
	LionContainer *right;
	LionContainer *items[LION_MAX_PUT_ITEMS];
	int			nitems;
} LionSegWork;

/*
 * The buffers are allocated on demand: most inserts never touch a segment at
 * all, and the ones that do usually only need seg.  Four 4104-byte buffers
 * per insert would be four separate allocations on the hot path for nothing.
 */
static void
lion_segwork_init(LionSegWork *w)
{
	w->seg = NULL;
	w->cont = NULL;
	w->left = NULL;
	w->right = NULL;
	w->nitems = 0;
}

static LionContainer *
lion_segwork_buf(LionContainer **p)
{
	if (*p == NULL)
		*p = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	return *p;
}

/* Does this item accept one more pair? */
static bool
lion_segment_has_room(const LionContainer *item)
{
	return item->type == LION_CT_SPARSE &&
		item->cardinality < LION_SPARSE_MAX_PAIRS;
}

/*
 * Add (ckey, lo) to the segment in w->seg and build the items that must take
 * its place, leaving them in w->items[0 .. w->nitems).  Returns false when
 * the pair was already there, in which case nothing has to be written.
 *
 * The threshold test comes first, on purpose: promoting the ckey to a
 * container *shrinks* the segment, so a segment that is at its maximum needs
 * splitting only when the pair really stays in it, and one insert can never
 * need more than LION_MAX_PUT_ITEMS items.
 */
static bool
lion_segment_add(LionSegWork *w, uint32 ckey, uint16 lo)
{
	uint32		cnt;

	Assert(w->seg->type == LION_CT_SPARSE);
	w->nitems = 0;

	if (lion_sparse_contains(w->seg, ckey, lo))
		return false;			/* already indexed */

	cnt = lion_sparse_count(w->seg, ckey);

	if (cnt + 1 >= LION_SPARSE_THRESHOLD)
	{
		/* Dense enough: the ckey becomes a container of its own. */
		(void) lion_segwork_buf(&w->cont);
		(void) lion_segwork_buf(&w->left);
		(void) lion_segwork_buf(&w->right);

		if (lion_sparse_extract(w->seg, ckey, w->cont) != cnt)
			elog(ERROR, "lion index: sparse segment lost container key %u",
				 ckey);
		if (!lion_container_add(w->cont, lo))
			elog(ERROR, "lion index: duplicate member in sparse segment for container key %u",
				 ckey);
		lion_container_optimize(w->cont);

		lion_sparse_split_at(w->seg, ckey, w->left, w->right);

		if (w->left->cardinality > 0)
			w->items[w->nitems++] = w->left;
		w->items[w->nitems++] = w->cont;
		if (w->right->cardinality > 0)
			w->items[w->nitems++] = w->right;
	}
	else if (lion_sparse_insert(w->seg, ckey, lo, NULL))
	{
		w->items[w->nitems++] = w->seg;
	}
	else if (lion_sparse_split_half(w->seg, lion_segwork_buf(&w->left),
								   lion_segwork_buf(&w->right)))
	{
		/* Full: split at a container key boundary, then insert in one half. */
		LionContainer *half = (ckey <= lion_item_last_ckey(w->left)) ?
			w->left : w->right;

		if (!lion_sparse_insert(half, ckey, lo, NULL))
			elog(ERROR, "lion index: half of a split sparse segment is full");

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

		(void) lion_segwork_buf(&w->cont);
		(void) lion_sparse_extract(w->seg, only, w->cont);
		lion_container_optimize(w->cont);
		Assert(w->seg->cardinality == 0);

		lion_sparse_init(w->left, ckey);
		if (!lion_sparse_insert(w->left, ckey, lo, NULL))
			elog(ERROR, "lion index: empty sparse segment rejected a pair");

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

	Assert(w->nitems >= 1 && w->nitems <= LION_MAX_PUT_ITEMS);
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
lion_insert_segment(Relation index, Relation heaprel, Buffer buf,
				   OffsetNumber off, bool replace,
				   Buffer entrybuf, OffsetNumber entryoff,
				   LionEntryTuple *entry, uint32 ckey, uint16 lo)
{
	Page		page = BufferGetPage(buf);
	LionSegWork	w;

	lion_segwork_init(&w);

	if (replace)
	{
		ItemId		iid = PageGetItemId(page, off);
		Size		isize = ItemIdGetLength(iid);

		if (isize > LION_CONTAINER_MAX_SIZE)
			elog(ERROR, "lion index: item of %zu bytes at %u/%u", isize,
				 BufferGetBlockNumber(buf), off);
		memcpy(lion_segwork_buf(&w.seg), PageGetItem(page, iid), isize);

		if (w.seg->type != LION_CT_SPARSE)
			elog(ERROR, "lion index: item %u on block %u is not a sparse segment",
				 off, BufferGetBlockNumber(buf));

		if (!lion_segment_add(&w, ckey, lo))
			return;				/* already indexed: nothing changes */
	}
	else
	{
		lion_sparse_init(lion_segwork_buf(&w.seg), ckey);
		if (!lion_sparse_insert(w.seg, ckey, lo, NULL))
			elog(ERROR, "lion index: empty sparse segment rejected a pair");
		w.items[0] = w.seg;
		w.nitems = 1;
	}

	entry->ntids += 1;
	entry->ncontainers += (uint32) (w.nitems - (replace ? 1 : 0));

	lion_chain_put_items_locked_ext(index, heaprel, buf, entrybuf, entryoff,
								   entry, off, replace, w.items, w.nitems, true);
}

/*
 * Write a one-pair sparse segment for (ckey, lo) at dst, which need not be
 * aligned.  Returns the number of bytes written.
 */
static Size
lion_put_singleton(char *dst, uint32 ckey, uint16 lo)
{
	union
	{
		LionContainer c;
		uint64		force_align;
		char		data[LION_CONTAINER_HDRSZ + sizeof(uint32) + sizeof(uint16)];
	}			buf;
	Size		csize;

	lion_sparse_init(&buf.c, ckey);
	if (!lion_sparse_insert(&buf.c, ckey, lo, NULL))
		elog(ERROR, "lion index: empty sparse segment rejected a pair");
	csize = lion_sparse_size(&buf.c);

	Assert(csize <= sizeof(buf.data));
	memcpy(dst, &buf.c, csize);

	return csize;
}

/*
 * Rebuild an INLINE payload with (ckey, lo) added, writing it to out, which
 * must have room for paylen + LION_CONTAINER_MAX_SIZE bytes (a container grows
 * by at most that much; splitting a segment into three items grows the
 * payload by at most 26 bytes, and a new segment adds 14).
 *
 * Returns the length of the new payload, or 0 if the member was already
 * there, in which case out is undefined.  *ndelta receives the change in the
 * number of items, which is -0 .. +2.
 */
static Size
lion_inline_add(const char *payload, Size paylen, uint32 ckey, uint16 lo,
			   char *out, int *ndelta)
{
	LionContainer *cbuf = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	LionSegWork	w;
	Size		off;
	Size		used = 0;
	Size		csize;
	int			n = 0;
	int			target = -1;	/* item to replace */
	int			insertpos = -1; /* item to insert a new segment before */
	int			previdx = -1;
	bool		prev_ok = false;

	*ndelta = 0;
	lion_segwork_init(&w);

	/*
	 * Pass 1: decide which item takes the pair.  The items are ordered by
	 * first ckey and their ranges do not overlap, so the walk stops either on
	 * the item that covers the ckey or on the first item beyond it.
	 */
	off = 0;
	while (lion_inline_fetch(payload, paylen, &off, cbuf) > 0)
	{
		if (lion_item_covers(cbuf, ckey))
		{
			target = n;
			break;
		}
		if (lion_item_first_ckey(cbuf) > ckey)
		{
			/* In the gap: the segment before it, else the one after it. */
			if (prev_ok)
				target = previdx;
			else if (lion_segment_has_room(cbuf))
				target = n;
			else
				insertpos = n;
			break;
		}
		prev_ok = lion_segment_has_room(cbuf);
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
	while ((csize = lion_inline_fetch(payload, paylen, &off, cbuf)) > 0)
	{
		if (n == insertpos)
		{
			used += lion_put_singleton(out + used, ckey, lo);
			*ndelta = 1;
			insertpos = -1;
		}

		if (n == target)
		{
			if (cbuf->type == LION_CT_SPARSE)
			{
				int			i;

				memcpy(lion_segwork_buf(&w.seg), cbuf, csize);
				if (!lion_segment_add(&w, ckey, lo))
				{
					pfree(cbuf);
					return 0;	/* already indexed */
				}

				for (i = 0; i < w.nitems; i++)
				{
					Size		isz = lion_item_size(w.items[i]);

					memcpy(out + used, w.items[i], isz);
					used += isz;
				}
				*ndelta = w.nitems - 1;
				n++;
				continue;
			}

			/* A container with this very ckey. */
			Assert(cbuf->ckey == ckey);
			if (!lion_container_add(cbuf, lo))
			{
				pfree(cbuf);
				return 0;		/* already indexed */
			}
			csize = lion_container_size(cbuf);
		}

		memcpy(out + used, cbuf, csize);
		used += csize;
		n++;
	}

	if (insertpos >= 0)
	{
		/* The payload was empty, or the new segment goes at the end. */
		used += lion_put_singleton(out + used, ckey, lo);
		*ndelta = 1;
	}

	pfree(cbuf);
	return used;
}

/*
 * The key is not in the index yet: add an INLINE entry with a single one-pair
 * sparse segment at the place the descent said it belongs (DESIGN.md §21).
 * *leafbuf is held EXCLUSIVE and stays so - it has been held since the
 * unsuccessful lookup, which is what makes find-or-create one serialised
 * operation (lion_dir_add_entry()).  reservedflag makes
 * it one of the two key-less entries of §14 and §17.
 */
static void
lion_insert_new_entry(Relation index, Relation heaprel, LionState *state,
					 Buffer *leafbuf, OffsetNumber off, bool movedright,
					 Datum key, uint16 reservedflag, uint32 hash, uint32 ckey,
					 uint16 lo)
{
	char		payload[LION_CONTAINER_HDRSZ + sizeof(uint32) + sizeof(uint16)];
	LionEntryTuple *entry;
	Size		paylen;
	Size		size;
	int			max_entries = lion_max_entries(index);

	paylen = lion_put_singleton(payload, ckey, lo);

	entry = (reservedflag != 0) ?
		lion_make_reserved_entry((AttrNumber) state->attno, reservedflag,
								LION_ENTRY_INLINE, payload, paylen, &size) :
		lion_make_entry(state, key, hash, LION_ENTRY_INLINE,
					   payload, paylen, &size);
	entry->ncontainers = 1;
	entry->ntids = 1;

	/*
	 * Cardinality guard (DESIGN.md §17), on the one path that can make the
	 * index grow a key: a brand new entry.
	 *
	 * The estimate used to be "entries on this bucket's chain times the bucket
	 * count"; with the sorted directory of §21 there is no bucket to count and
	 * the same order-of-magnitude estimate comes from the shape of the tree
	 * instead - the entries on THIS leaf times the number of leaves, which is
	 * the number of blocks the index has minus its internal pages, bounded
	 * below by one.  Both are crude by design: counting the whole index for
	 * every new key would read every leaf, and all this feeds is a warning.
	 */
	if (max_entries > 0)
	{
		Page		page = BufferGetPage(*leafbuf);
		int64		perleaf = (int64) PageGetMaxOffsetNumber(page) + 1;
		int64		nleaves = Max((int64) RelationGetNumberOfBlocks(index) - 1,
								  1);
		int64		estimate = perleaf * nleaves;

		if (estimate > (int64) max_entries)
			lion_warn_max_entries(index, estimate);
	}

	lion_dir_add_entry(index, heaprel, state->ix, leafbuf, off, movedright,
					  entry, size);

	pfree(entry);
}

/*
 * Add (ckey, lo) to the INLINE entry at (entrybuf, entryoff), spilling the
 * posting set onto container pages if it no longer fits.  entrybuf is held
 * EXCLUSIVE.
 */
static void
lion_insert_inline(Relation index, Relation heaprel, LionState *state,
				  Buffer entrybuf,
				  OffsetNumber entryoff, uint32 ckey, uint16 lo)
{
	Page		page = BufferGetPage(entrybuf);
	ItemId		iid = PageGetItemId(page, entryoff);
	LionEntryTuple *entry = (LionEntryTuple *) PageGetItem(page, iid);
	Size		itemsz = ItemIdGetLength(iid);
	Size		payoff = LionEntryPayloadOffset(entry);
	Size		paylen = LION_ENTRY_PAYLOAD_LEN(entry, itemsz);
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
	memcpy(oldpay, LionEntryGetPayload(entry), paylen);

	newpay = (char *) palloc(paylen + LION_CONTAINER_MAX_SIZE);
	newlen = lion_inline_add(oldpay, paylen, ckey, lo, newpay, &ndelta);

	if (newlen == 0)
	{
		/* The TID is already in this posting set; nothing to do. */
		pfree(newpay);
		pfree(oldpay);
		return;
	}

	if (newlen <= (Size) state->ix->meta.inline_limit &&
		payoff + newlen <= (Size) LION_MAX_ENTRY_SIZE)
	{
		LionEntryTuple *newentry;
		Size		newsize;
		Size		need = payoff + newlen;
		Size		writesz;

		/*
		 * GROWTH SLACK, the INLINE half of DESIGN.md §4.
		 *
		 * The payload this key had may already have room for the member that
		 * was just added to it, because the last insert-driven rewrite left
		 * some: then the entry KEEPS the bytes the page has allotted it and
		 * only its own bytes change.  PageIndexTupleOverwrite() then moves no
		 * other entry on the leaf - which is what an INLINE insert used to pay
		 * for every time, 1.5-2.9 KB of it in generic mode - and the record is
		 * a delta of the counters and the tail of the payload.
		 *
		 * Otherwise the entry is written at its new length PLUS fresh slack,
		 * which is what puts the next few inserts on the cheap path.  Only
		 * what fits on the leaf as it stands: slack is never worth a split,
		 * and never worth compacting an entry that has too much of it either.
		 */
		if (itemsz >= need && itemsz - need <= LION_ENTRY_SLACK_BOUND)
			writesz = itemsz;
		else
		{
			Size		maxsize = MAXALIGN_DOWN(MAXALIGN(itemsz) +
												PageGetExactFreeSpace(page));

			writesz = lion_entry_alloc_size(payoff, newlen,
											(Size) state->ix->meta.inline_limit,
											Max(maxsize, need));
		}

		newentry = lion_entry_rebuild_slack(entry, newpay, newlen, writesz,
											&newsize);
		newentry->ncontainers = (uint32) ((int) ncontainers + ndelta);
		newentry->ntids = ntids + 1;

		/*
		 * DESIGN.md §21: a grown entry that no longer fits its leaf splits the
		 * leaf instead of spilling onto a container page, which is what the
		 * hash directory had to do.  Note that `entry` points into the page
		 * and is not valid after this call.
		 */
		lion_dir_place(index, heaprel, state->ix, entrybuf, entryoff, true,
					  newentry, newsize);
		pfree(newentry);
		pfree(newpay);
		pfree(oldpay);
		return;
	}

	/*
	 * The posting set has outgrown the entry tuple (or the bucket page).
	 * Move the containers it had onto a chain, then add the new member
	 * through the chain path.
	 */
	{
		LionEntryTuple *chain;
		Size		chainsize;

		chain = lion_entry_rebuild(entry, NULL, 0, &chainsize);
		chain->ncontainers = ncontainers;
		chain->ntids = ntids;

		lion_entry_spill(index, heaprel, entrybuf, entryoff, chain, oldpay,
						paylen);
		pfree(chain);
	}

	pfree(newpay);
	pfree(oldpay);

	lion_insert_chain(index, heaprel, entrybuf, entryoff, ckey, lo);
}

/*
 * Fast path for adding a member to a container that is already on a page:
 * mutate the container in the page image instead of copying it into a work
 * buffer, adding the member there and copying the whole thing back through
 * PageIndexTupleOverwrite.  Nothing else about the page changes - the item
 * keeps its ALLOCATED length (ItemIdGetLength) and its offset, so no other
 * item moves - and minckey/maxckey keep their values because the container
 * keeps its ckey.  The GenericXLog delta is then the handful of bytes the
 * member really changed.
 *
 * Which containers qualify:
 *
 *	BITSET	always: it is LION_CONTAINER_MAX_SIZE bytes whatever its
 *			cardinality, so adding a member cannot change its size.
 *	ARRAY	when the item has two spare bytes inside it (DESIGN.md §4,
 *			growth slack) and the array is not at LION_ARRAY_MAX_CARD, where
 *			lion_container_add() would turn it into a bitset.
 *	RUN		when the item has room for one more run and the run count is
 *			below LION_RUN_MAX_NRUNS, for the same reason.
 *
 * Returns false when the container cannot take the member without changing
 * its size on the page; then nothing has been done and the caller takes the
 * general path (which is also what gives the item its slack for next time).
 * Returns true with *done set when the change - or the discovery that the TID
 * was already indexed - is complete.  buf and entrybuf are held EXCLUSIVE
 * throughout and stay locked; entry is the caller's private copy of the entry
 * tuple, which is written in the same WAL record as the container.
 */
static bool
lion_insert_container_inplace(Relation index, Buffer buf, OffsetNumber off,
							 Buffer entrybuf, OffsetNumber entryoff,
							 LionEntryTuple *entry, Size entrysize, uint16 lo,
							 bool *done)
{
	Page		page = BufferGetPage(buf);
	ItemId		iid = PageGetItemId(page, off);
	LionContainer *onpage = (LionContainer *) PageGetItem(page, iid);
	Size		alloc = ItemIdGetLength(iid);
	Size		need;
	LionWalState *xstate;
	Page		p;
	LionContainer *c;

	switch (onpage->type)
	{
		case LION_CT_BITSET:
			need = LION_CONTAINER_MAX_SIZE;
			break;

		case LION_CT_ARRAY:
			if (onpage->cardinality >= LION_ARRAY_MAX_CARD)
				return false;	/* would become a bitset */
			need = lion_container_size(onpage) + sizeof(uint16);
			break;

		case LION_CT_RUN:
			if (LION_RUN_NRUNS(onpage) >= LION_RUN_MAX_NRUNS)
				return false;	/* would become a bitset */
			need = lion_container_size(onpage) + sizeof(LionRun);

			/*
			 * A member that closes the one-value gap between two runs MERGES
			 * them, and the container comes out a run SHORTER while the item
			 * keeps its allotment.  In place, a stream of such inserts would
			 * grow the unused tail of the item without bound; the slack an
			 * item may carry is LION_ITEM_SLACK_BOUND, the rule VACUUM's
			 * shrink-in-place already follows (DESIGN.md §4, §18).  So a
			 * merge that would take the item past it is left to the general
			 * path, which rewrites the item at its logical size plus normal
			 * growth slack.  Nothing else shrinks a container in place: an
			 * ARRAY and a new run only grow, an extended run and a BITSET
			 * keep their size.
			 */
			if (lo > 0 && lo < LION_LO_MASK &&
				lion_container_contains(onpage, (uint16) (lo - 1)) &&
				lion_container_contains(onpage, (uint16) (lo + 1)) &&
				!lion_container_contains(onpage, lo) &&
				alloc > lion_container_size(onpage) - sizeof(LionRun) +
				LION_ITEM_SLACK_BOUND)
				return false;
			break;

		default:
			return false;
	}

	if (alloc < need)
		return false;

	*done = true;

	/* Already indexed: do not start a record at all. */
	if (lion_container_contains(onpage, lo))
		return true;

	xstate = lion_wal_begin(index);
	p = lion_wal_register_buffer(xstate, buf, LION_WALBUF_STD);
	c = (LionContainer *) PageGetItem(p, PageGetItemId(p, off));

	if (!lion_container_add(c, lo))
		elog(ERROR, "lion index: container %u on block %u changed under an exclusive lock",
			 c->ckey, BufferGetBlockNumber(buf));

	/* The whole point: the item still ends where it ended. */
	Assert(lion_container_size(c) <= alloc);

	/*
	 * This is the hot-key insert record of DESIGN.md §25, and it logs the
	 * CALL rather than the bytes: adding a member to a sorted ARRAY shifts
	 * every element above it, so the bytes that change run to the end of the
	 * item, while "add member lo to the item at offset off" is four.  Replay
	 * calls the same library function on the same item, which is what makes
	 * the two pages identical to the byte.
	 */
	lion_wal_op(xstate, p, LION_OP_CONTAINER_ADD, off, lo, NULL, 0);

	entry->ntids += 1;

	/* The entry always travels with the container change. */
	if (!lion_replace_entry(index, xstate, entrybuf, entryoff, entry, entrysize))
		elog(ERROR, "lion index: could not update entry %u on block %u",
			 entryoff, BufferGetBlockNumber(entrybuf));

	lion_wal_finish(xstate, LION_XLOG_ITEM_SET);

	return true;
}

/*
 * The same for a sparse segment that is already on a page: insert the pair in
 * its sorted place inside the item, which is one memmove of at most 4 KB
 * (ckeys[] and los[] both shift) and leaves the item's allocated length and
 * offset untouched.
 *
 * Only for a pair that really stays in the segment: a container key that
 * reaches LION_SPARSE_THRESHOLD members has to be promoted to a container of
 * its own, which turns one item into up to three and belongs to
 * lion_insert_segment().  Unlike a container, a segment covers a RANGE of
 * container keys, and the pair may extend it, so the page bounds are
 * recomputed.
 */
static bool
lion_insert_segment_inplace(Relation index, Buffer buf, OffsetNumber off,
						   Buffer entrybuf, OffsetNumber entryoff,
						   LionEntryTuple *entry, Size entrysize,
						   uint32 ckey, uint16 lo, bool *done)
{
	Page		page = BufferGetPage(buf);
	ItemId		iid = PageGetItemId(page, off);
	LionContainer *onpage = (LionContainer *) PageGetItem(page, iid);
	Size		alloc = ItemIdGetLength(iid);
	LionWalState *xstate;
	Page		p;
	LionContainer *s;
	bool		dup = false;

	Assert(onpage->type == LION_CT_SPARSE);

	if (onpage->cardinality >= LION_SPARSE_MAX_PAIRS)
		return false;			/* would have to split */
	if (lion_sparse_count(onpage, ckey) + 1 >= LION_SPARSE_THRESHOLD)
		return false;			/* would have to promote the container key */
	if (alloc < lion_sparse_size(onpage) + LION_SPARSE_PAIR_SIZE)
		return false;			/* no slack inside the item */

	*done = true;

	if (lion_sparse_contains(onpage, ckey, lo))
		return true;

	xstate = lion_wal_begin(index);
	p = lion_wal_register_buffer(xstate, buf, LION_WALBUF_STD);
	s = (LionContainer *) PageGetItem(p, PageGetItemId(p, off));

	if (!lion_sparse_insert(s, ckey, lo, &dup) || dup)
		elog(ERROR, "lion index: sparse segment %u on block %u changed under an exclusive lock",
			 off, BufferGetBlockNumber(buf));

	Assert(lion_sparse_size(s) <= alloc);

	/* The call, not the bytes: a pair shifts both halves of the segment. */
	lion_wal_op(xstate, p, LION_OP_SPARSE_INS, off, lo, &ckey, sizeof(uint32));

	/* A segment's range can grow in either direction. */
	lion_page_update_minmax(p);
	lion_wal_op(xstate, p, LION_OP_MINMAX, 0, 0, NULL, 0);

	entry->ntids += 1;

	if (!lion_replace_entry(index, xstate, entrybuf, entryoff, entry, entrysize))
		elog(ERROR, "lion index: could not update entry %u on block %u",
			 entryoff, BufferGetBlockNumber(entrybuf));

	lion_wal_finish(xstate, LION_XLOG_ITEM_SET);

	return true;
}

/*
 * Lock the posting-tree leaf that owns ckey EXCLUSIVE, for the CHAIN insert
 * path.
 *
 * The append case - the ckey belongs on the set's LAST leaf, which is where
 * every insert into a growing posting set lands - is decided with the
 * exclusive lock the insert needs anyway, without descending at all.  The
 * descent would take SHARE locks on the way down and then a lock on that very
 * page, and on a hot key that is one more handoff of the page's lock between
 * the waiters for nothing: the measured ceiling of concurrent inserts into
 * one key is set by how often the page lock changes hands, not by how long any
 * one holder keeps it (DESIGN.md §5).
 *
 * The tail qualifies when it is still the RIGHTMOST leaf of the set and holds
 * a container key at or below ckey: then nothing to its right can own ckey and
 * nothing to its left can either.  An empty tail owns nothing as far as its
 * own bounds go (minckey is 0, which would swallow every ckey), so it falls
 * through to the descent, which routes by the parent's separators instead.
 *
 * Nothing is held while descending: the tail lock is dropped first, so no page
 * is ever locked before a page to its left (DESIGN.md §22 rule 3).
 */
static Buffer
lion_insert_lock_chain_page(Relation index, Relation heaprel, uint32 hash,
						   BlockNumber head, BlockNumber tail, uint32 ckey)
{
	Buffer		buf;
	Page		page;

	Assert(BlockNumberIsValid(head) && BlockNumberIsValid(tail));

	buf = ReadBuffer(index, tail);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buf);

	/*
	 * DESIGN.md §18: every page reached through an entry's head, tail or
	 * rightlink has to claim that posting set.  An insert holds the entry's
	 * directory leaf EXCLUSIVE and VACUUM only frees a set under a cleanup
	 * lock on that same page, so the set cannot go away underneath this and a
	 * page that does not belong to it is corruption, not a race.
	 */
	if (!lion_page_owns_entry(page, hash, head))
	{
		UnlockReleaseBuffer(buf);
		elog(ERROR, "lion index: block %u is not a container page of the posting set at %u",
			 tail, head);
	}

	if (LionPageIsPostingLeaf(page) && LionPageIsRightmost(page) &&
		!LionPageIncompleteSplit(page) &&
		PageGetMaxOffsetNumber(page) >= FirstOffsetNumber &&
		ckey >= LionPageGetOpaque(page)->minckey)
		return buf;

	UnlockReleaseBuffer(buf);

	return lion_posting_search(index, heaprel, hash, head, ckey,
							   BUFFER_LOCK_EXCLUSIVE, true);
}

/*
 * Add (ckey, lo) to the CHAIN entry at (entrybuf, entryoff), which is held
 * EXCLUSIVE.
 */
static void
lion_insert_chain(Relation index, Relation heaprel, Buffer entrybuf,
				 OffsetNumber entryoff,
				 uint32 ckey, uint16 lo)
{
	Page		page = BufferGetPage(entrybuf);
	ItemId		iid = PageGetItemId(page, entryoff);
	LionEntryTuple *entry = (LionEntryTuple *) PageGetItem(page, iid);
	LionEntryTuple *ecopy;
	LionContainer *cbuf;
	Size		esize;
	BlockNumber blk;
	Buffer		buf;
	Page		cpage;
	OffsetNumber off;
	bool		found;
	int			delta;

	ecopy = lion_entry_rebuild(entry, NULL, 0, &esize);
	Assert(esize == ItemIdGetLength(iid));
	Assert((ecopy->flags & LION_ENTRY_CHAIN) != 0);

	cbuf = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);

	buf = lion_insert_lock_chain_page(index, heaprel, ecopy->hash, ecopy->head,
									 ecopy->tail, ckey);
	blk = BufferGetBlockNumber(buf);
	cpage = BufferGetPage(buf);

	/* Which item owns this ckey: a container, a segment, or nothing yet? */
	off = lion_page_find_item(cpage, ckey, &found);

	if (found &&
		((LionContainer *) PageGetItem(cpage, PageGetItemId(cpage, off)))->type
		== LION_CT_SPARSE)
	{
		bool		done = false;

		if (!lion_insert_segment_inplace(index, buf, off, entrybuf, entryoff,
										ecopy, esize, ckey, lo, &done))
			lion_insert_segment(index, heaprel, buf, off, true, entrybuf,
							   entryoff, ecopy, ckey, lo);
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
			lion_segment_has_room((LionContainer *)
								 PageGetItem(cpage,
											 PageGetItemId(cpage, OffsetNumberPrev(off)))))
			target = OffsetNumberPrev(off);
		else if (off <= maxoff &&
				 lion_segment_has_room((LionContainer *)
									  PageGetItem(cpage, PageGetItemId(cpage, off))))
			target = off;

		if (target != InvalidOffsetNumber)
		{
			bool		done = false;

			if (!lion_insert_segment_inplace(index, buf, target, entrybuf,
											entryoff, ecopy, esize, ckey, lo,
											&done))
				lion_insert_segment(index, heaprel, buf, target, true, entrybuf,
								   entryoff, ecopy, ckey, lo);
			UnlockReleaseBuffer(buf);
			pfree(cbuf);
			pfree(ecopy);
			return;
		}

		/* No segment to join: the pair becomes a new one-pair segment. */
		lion_insert_segment(index, heaprel, buf, off, false, entrybuf, entryoff,
						   ecopy, ckey, lo);
		UnlockReleaseBuffer(buf);
		pfree(cbuf);
		pfree(ecopy);
		return;
	}

	/* A container with this ckey. */
	{
		ItemId		ciid = PageGetItemId(cpage, off);
		bool		done = false;

		Assert(((LionContainer *) PageGetItem(cpage, ciid))->ckey == ckey);

		if (lion_insert_container_inplace(index, buf, off, entrybuf, entryoff,
										 ecopy, esize, lo, &done))
		{
			Assert(done);
			UnlockReleaseBuffer(buf);
			pfree(cbuf);
			pfree(ecopy);
			return;
		}

		/*
		 * The item may be longer than the container needs (growth slack,
		 * DESIGN.md §4); copying the slack along is harmless, and the
		 * allocated length can never exceed a work buffer.
		 */
		if (ItemIdGetLength(ciid) > LION_CONTAINER_MAX_SIZE)
			elog(ERROR, "lion index: container of %zu bytes at %u/%u",
				 (Size) ItemIdGetLength(ciid), blk, off);
		memcpy(cbuf, PageGetItem(cpage, ciid), ItemIdGetLength(ciid));
		if (!lion_container_add(cbuf, lo))
		{
			/* already indexed */
			UnlockReleaseBuffer(buf);
			pfree(cbuf);
			pfree(ecopy);
			return;
		}
	}

	ecopy->ntids += 1;

	lion_chain_put_container_locked_ext(index, heaprel, buf, entrybuf, entryoff,
									   ecopy, cbuf, &delta, true);
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
lion_insert_one(Relation index, Relation heaprel, LionState *state, Datum key,
			   uint16 reservedflag, uint32 ckey, uint16 lo)
{
	uint32		hash;
	LionSearchKey sk;
	Buffer		leafbuf;
	OffsetNumber entryoff;
	bool		movedright;
	bool		found;

	hash = (reservedflag != 0) ? LION_NULLKEY_HASH : lion_hash_key(state, key);

	if (reservedflag == LION_ENTRY_NULLKEY)
		lion_search_key_init(state, &sk, LION_KIND_NULL, (Datum) 0, hash);
	else if (reservedflag == LION_ENTRY_EMPTYKEY)
		lion_search_key_init(state, &sk, LION_KIND_EMPTY, (Datum) 0, hash);
	else
		lion_search_key_init(state, &sk, LION_KIND_VALUE, key, hash);

	/*
	 * One descent, with the leaf taken EXCLUSIVE, and it is held for the whole
	 * insert: every writer of a key (inserts and VACUUM) serialises on the
	 * leaf that holds its entry, exactly as they used to serialise on the
	 * bucket head page (DESIGN.md §5, §21).
	 */
	found = lion_dir_find(index, heaprel, state->ix, &sk, BUFFER_LOCK_EXCLUSIVE,
						  true, &leafbuf, &entryoff, &movedright);

	if (!found)
		lion_insert_new_entry(index, heaprel, state, &leafbuf, entryoff,
							 movedright, key, reservedflag, hash, ckey, lo);
	else
	{
		LionEntryTuple *entry;

		entry = (LionEntryTuple *) PageGetItem(BufferGetPage(leafbuf),
											  PageGetItemId(BufferGetPage(leafbuf),
															entryoff));

		if ((entry->flags & LION_ENTRY_INLINE) != 0)
			lion_insert_inline(index, heaprel, state, leafbuf, entryoff, ckey,
							  lo);
		else
			lion_insert_chain(index, heaprel, leafbuf, entryoff, ckey, lo);
	}

	UnlockReleaseBuffer(leafbuf);
}

/*
 * aminsert
 *
 * checkUnique is irrelevant (amcanunique is false) and indexUnchanged is
 * ignored: a lion index has no way of knowing whether it has seen this
 * TID before without looking, and the lookup is the bulk of the work anyway.
 *
 * A multi-key opclass (DESIGN.md §17) turns one row into several independent
 * single-key inserts, each taking and releasing its own bucket lock.  They are
 * not atomic with respect to a concurrent reader - a scan may see the row
 * under some of its keys and not yet under the others - which is exactly the
 * visibility the heap gives anyway: the inserting transaction has not
 * committed, so no snapshot that can see the row can run before the last of
 * them has been written.
 *
 * A MULTICOLUMN index (DESIGN.md §24) is the same thing once more over: the
 * columns are independent key sets in one relation, so one row is one insert
 * per column - each under its own column's entry, on its own directory leaf -
 * and the same visibility argument covers them.  A NULL in one column says
 * nothing about the others.
 */
bool
lioninsert(Relation index, Datum *values, bool *isnull, ItemPointer ht_ctid,
		  Relation heapRel, IndexUniqueCheck checkUnique, bool indexUnchanged,
		  IndexInfo *indexInfo)
{
	LionIndexState *ix;
	MemoryContext insertcxt;
	MemoryContext oldcxt;
	uint64		code;
	uint32		ckey;
	uint16		lo;
	int			c;

	lion_check_key_offset(ht_ctid);

	ix = lion_get_index_state(index);

	insertcxt = AllocSetContextCreate(CurrentMemoryContext,
									  "lion index insert",
									  ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(insertcxt);

	code = lion_tid_to_code(ht_ctid);
	ckey = lion_code_ckey(code);
	lo = lion_code_lo(code);

	for (c = 0; c < ix->ncolumns; c++)
	{
		LionState  *state = &ix->cols[c];
		Datum		key;

		/*
		 * A NULL value belongs to that column's reserved NULL entry, which is
		 * found by its flag rather than by its (absent) key (DESIGN.md §14).
		 * A multi-key opclass that extracts nothing from a non-NULL value
		 * puts the row in the column's reserved EMPTY entry the same way
		 * (DESIGN.md §17).
		 */
		if (isnull[c])
			lion_insert_one(index, heapRel, state, (Datum) 0,
						   LION_ENTRY_NULLKEY, ckey, lo);
		else if (state->multikey)
		{
			Datum	   *keys;
			int			nkeys = lion_extract_value(state, values[c], &keys);
			int			i;

			if (nkeys == 0)
				lion_insert_one(index, heapRel, state, (Datum) 0,
							   LION_ENTRY_EMPTYKEY, ckey, lo);
			for (i = 0; i < nkeys; i++)
			{
				lion_insert_one(index, heapRel, state, keys[i], 0, ckey, lo);
				CHECK_FOR_INTERRUPTS();
			}
		}
		else
		{
			key = values[c];
			if (!state->typbyval && state->typlen == -1)
				key = PointerGetDatum(PG_DETOAST_DATUM(key));

			lion_insert_one(index, heapRel, state, key, 0, ckey, lo);
		}

		CHECK_FOR_INTERRUPTS();
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(insertcxt);

	/* No uniqueness check was performed. */
	return false;
}
