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
 * always qualifies (4104 bytes whatever its cardinality), and so does a
 * NARROW for a member within its width (DESIGN.md §38); an ARRAY or RUN
 * container and a sparse segment qualify when the item has growth slack
 * inside it (DESIGN.md §4), which the general path leaves behind whenever it
 * writes an item on behalf of an insert.
 *
 * An item is a container or a sparse segment (DESIGN.md §13), and which one
 * owns a ckey decides what the insert does:
 *
 *	- a container with that ckey: the member goes into it.
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
 * INLINE payload that spills to a chain like any other.  So are the reserved
 * entries a multi-key opclass uses for rows it extracts no key from
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
#include "lion_compat.h"
#include "lion_defer.h"

static void lion_insert_new_entry(Relation index, Relation heaprel,
								 LionState *state, Buffer *leafbuf,
								 OffsetNumber off, bool movedright,
								 Datum key, uint16 reservedflag, uint32 hash,
								 uint32 ckey, uint16 lo,
								 const LionPosMember *pm);
static void lion_insert_inline(Relation index, Relation heaprel,
							  LionState *state, Buffer entrybuf,
							  OffsetNumber entryoff, uint32 ckey, uint16 lo,
							  const LionPosMember *pm);
static void lion_insert_place_new(Relation index, Relation heaprel,
								  LionState *state, Buffer *leafbuf,
								  OffsetNumber off, bool movedright,
								  LionEntryTuple *entry, Size size);
static bool lion_insert_inline_ext(Relation index, Relation heaprel,
								   LionState *state, Buffer entrybuf,
								   OffsetNumber entryoff, uint32 ckey,
								   uint16 lo, const LionPosMember *pm,
								   bool nosplit);
static void lion_insert_chain(Relation index, Relation heaprel, Buffer entrybuf,
							 OffsetNumber entryoff, uint32 ckey, uint16 lo,
							 const LionPosMember *pm);
static bool lion_insert_container_inplace(Relation index, Buffer buf, OffsetNumber off,
										 Buffer entrybuf, OffsetNumber entryoff,
										 LionEntryTuple *entry, Size entrysize,
										 uint16 lo, bool *done);

/* What the first half of an in-place insert decides (lion_*_inplace_check()). */
#define LION_INPLACE_NO		0	/* the member needs the general path */
#define LION_INPLACE_YES	1	/* it goes in in place */
#define LION_INPLACE_DUP	2	/* it is there already */

static int	lion_container_inplace_check(Relation index, Buffer buf,
										 OffsetNumber off, uint16 lo);
static int	lion_container_inplace_decide(const LionContainer *onpage,
										  Size alloc, uint16 lo);
static void lion_container_inplace_apply(LionWalState *xstate, Buffer buf,
										 OffsetNumber off, uint16 lo);
static int	lion_segment_inplace_check(Relation index, Buffer buf,
									   OffsetNumber off,
									   const LionSparseProbe *pr);
static void lion_segment_inplace_apply(LionWalState *xstate, Buffer buf,
									   OffsetNumber off, uint32 ckey,
									   uint16 lo, const LionSparseProbe *pr);
static Buffer lion_insert_lock_chain_page(Relation index, Relation heaprel,
										  BlockNumber entryblk, uint32 hash,
										  BlockNumber head, BlockNumber tail,
										  uint32 ckey, bool *ontail);
static bool lion_insert_chain_leaf(Relation index, Relation heaprel, Buffer buf,
								   Buffer entrybuf, OffsetNumber entryoff,
								   LionEntryTuple *ecopy, Size esize,
								   LionContainer *cbuf, uint32 ckey, uint16 lo,
								   bool mayput);
static bool lion_insert_segment_inplace(Relation index, Buffer buf, OffsetNumber off,
									   Buffer entrybuf, OffsetNumber entryoff,
									   LionEntryTuple *entry, Size entrysize,
									   uint32 ckey, uint16 lo,
									   const LionSparseProbe *pr, bool *done);
static void lion_insert_segment(Relation index, Relation heaprel, Buffer buf,
							   OffsetNumber off,
							   bool replace, Buffer entrybuf,
							   OffsetNumber entryoff, LionEntryTuple *entry,
							   uint32 ckey, uint16 lo,
							   const LionSparseProbe *pr);

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

/*
 * Is this item a segment that has room for (ckey, lo)?  If so *pr is its
 * probe for the pair (lion_sparse_probe()), which the insert that follows
 * takes over instead of decoding the segment again.
 */
static bool
lion_segment_has_room(const LionContainer *item, uint32 ckey, uint16 lo,
					  LionSparseProbe *pr)
{
	if (item->type != LION_CT_SPARSE)
		return false;
	lion_sparse_probe(item, ckey, lo, pr);
	return pr->found || lion_sparse_probe_fits(pr);
}

/*
 * Add (ckey, lo) to the segment in w->seg and build the items that must take
 * its place, leaving them in w->items[0 .. w->nitems).  Returns false when
 * the pair was already there, in which case nothing has to be written.  pr is
 * the probe of w->seg for the pair (lion_sparse_probe()): all this asks of
 * the segment - is the pair there, how many pairs has its ckey, does it fit -
 * comes from that one decode, and so does the insert itself.
 *
 * The threshold test comes first, on purpose: promoting the ckey to a
 * container *shrinks* the segment, so a segment that is at its maximum needs
 * splitting only when the pair really stays in it, and one insert can never
 * need more than LION_MAX_PUT_ITEMS items.
 */
static bool
lion_segment_add(LionSegWork *w, uint32 ckey, uint16 lo,
				 const LionSparseProbe *pr)
{
	uint32		cnt;

	Assert(w->seg->type == LION_CT_SPARSE);
	Assert(pr->code == lion_make_code(ckey, lo));
	w->nitems = 0;

	if (pr->found)
		return false;			/* already indexed */

	cnt = pr->ckey_pairs;

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
		/* the smallest that holds it, as the build's (DESIGN.md §38) */
		lion_container_optimize(w->cont);

		lion_sparse_split_at(w->seg, ckey, w->left, w->right);

		if (w->left->cardinality > 0)
			w->items[w->nitems++] = w->left;
		w->items[w->nitems++] = w->cont;
		if (w->right->cardinality > 0)
			w->items[w->nitems++] = w->right;
	}
	else if (lion_sparse_probe_fits(pr))
	{
		if (!lion_sparse_insert_probed(w->seg, pr, NULL))
			elog(ERROR, "lion index: sparse segment refused a pair its probe said fits");
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
 * false -- as a brand new one-pair segment inserted at off.  pr, if not NULL,
 * is the probe of that segment for the pair (lion_sparse_probe()), taken
 * under the same lock.
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
				   LionEntryTuple *entry, uint32 ckey, uint16 lo,
				   const LionSparseProbe *pr)
{
	Page		page = BufferGetPage(buf);
	LionSegWork	w;
	LionSparseProbe probe;

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

		/* the probe of the page item is the probe of its copy */
		if (pr == NULL)
		{
			lion_sparse_probe(w.seg, ckey, lo, &probe);
			pr = &probe;
		}
		if (!lion_segment_add(&w, ckey, lo, pr))
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

	lion_entry_set_ntids(entry, lion_entry_ntids(entry) + 1);
	lion_entry_set_ncontainers(entry, lion_entry_ncontainers(entry) + (uint32) (w.nitems - (replace ? 1 : 0)));

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
		char		data[LION_SPARSE_SINGLETON_MAX];
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
 * Rebuild the INLINE payload of entry eoff on block blk with (ckey, lo)
 * added, writing it to out, which must have room for paylen +
 * LION_CONTAINER_MAX_SIZE bytes (a container grows
 * by at most that much; a segment that takes the pair grows by at most two
 * varints, splitting one into three items adds two headers and a container,
 * and a new segment adds at most LION_SPARSE_SINGLETON_MAX bytes).
 *
 * Returns the length of the new payload, or 0 if the member was already
 * there, in which case out is undefined.  *ndelta receives the change in the
 * number of items, which is -0 .. +2.
 *
 * Which item takes the pair is decided from the items' first ckeys, which
 * are in their headers: the items are ordered by first ckey and their ranges
 * do not overlap, so the only item that can cover the ckey is the last one
 * whose first ckey is at or below it - the candidate - and the gap the ckey
 * falls in otherwise lies between the candidate and the item after it.  Only
 * those two can be the target, and a segment among them is decoded once
 * (lion_sparse_probe()), which answers whether it covers the ckey, whether it
 * has room and where the pair goes; the items around the target are copied
 * as bytes, back to back as they were.
 */
static Size
lion_inline_add(Relation index, BlockNumber blk, OffsetNumber eoff,
				const char *payload, Size paylen, uint32 ckey, uint16 lo,
				char *out, int *ndelta)
{
	union
	{
		LionContainer c;
		uint64		force_align;
		char		data[LION_CONTAINER_MAX_SIZE];
	}			walkbuf;
	LionContainer *cbuf = &walkbuf.c;
	LionContainer *item;
	LionSegWork	w;
	LionSparseProbe probe;
	Size		off;
	Size		ioff;			/* byte offset of the item being walked */
	Size		csize;
	Size		end;			/* the end of the last item: slack follows */
	Size		candoff = 0;
	Size		nextoff = 0;
	Size		toff = 0;		/* the target's bytes, or the new segment's place */
	Size		tend = 0;
	Size		used;
	bool		cand = false;	/* the last item with first ckey <= ckey */
	bool		next = false;	/* the item after it */
	bool		target = false;
	int			i;

	*ndelta = 0;
	lion_segwork_init(&w);

	/*
	 * Walk the items once, reading their headers: every item is fetched, as
	 * the copy of the payload needs every one of them sound
	 * (lion_inline_fetch()), and the walk finds where they end.
	 */
	off = 0;
	for (;;)
	{
		ioff = off;
		if (lion_inline_fetch(index, blk, eoff, payload, paylen, &off,
							  cbuf) == 0)
			break;
		if (next)
			continue;
		if (lion_item_first_ckey(cbuf) <= ckey)
		{
			cand = true;
			candoff = ioff;
		}
		else
		{
			next = true;
			nextoff = ioff;
		}
	}
	end = off;

	/* The candidate takes the pair if it covers the ckey, or has room. */
	item = lion_segwork_buf(&w.seg);
	if (cand)
	{
		off = candoff;
		csize = lion_inline_fetch(index, blk, eoff, payload, paylen, &off,
								  item);
		Assert(csize > 0);
		if (item->type == LION_CT_SPARSE)
		{
			lion_sparse_probe(item, ckey, lo, &probe);
			target = lion_sparse_probe_covers(item, &probe) ||
				lion_sparse_probe_fits(&probe);
		}
		else
			target = (item->ckey == ckey);
		toff = candoff;
		tend = off;
	}

	/* In the gap after it: the segment after the gap, else a new segment. */
	if (!target && next)
	{
		off = nextoff;
		csize = lion_inline_fetch(index, blk, eoff, payload, paylen, &off,
								  item);
		Assert(csize > 0);
		target = lion_segment_has_room(item, ckey, lo, &probe);
		toff = nextoff;
		tend = target ? off : nextoff;
	}
	else if (!target)
		toff = tend = end;		/* past the last item, or the payload is empty */

	/* The items before the target, as they are. */
	memcpy(out, payload, toff);
	used = toff;

	if (!target)
	{
		used += lion_put_singleton(out + used, ckey, lo);
		*ndelta = 1;
	}
	else if (item->type == LION_CT_SPARSE)
	{
		if (!lion_segment_add(&w, ckey, lo, &probe))
			return 0;			/* already indexed */

		for (i = 0; i < w.nitems; i++)
		{
			Size		isz = lion_item_size(w.items[i]);

			memcpy(out + used, w.items[i], isz);
			used += isz;
		}
		*ndelta = w.nitems - 1;
	}
	else
	{
		/*
		 * A container with this very ckey, written back as the smallest that
		 * holds it, as the build writes it (DESIGN.md §38).
		 */
		Assert(item->ckey == ckey);
		if (!lion_container_add(item, lo))
			return 0;			/* already indexed */
		lion_container_optimize(item);
		csize = lion_container_size(item);
		memcpy(out + used, item, csize);
		used += csize;
	}

	/* The items after it, as they are. */
	memcpy(out + used, payload + tend, end - tend);
	used += end - tend;

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
					 uint16 lo, const LionPosMember *pm)
{
	char		payload[LION_SPARSE_SINGLETON_MAX];
	LionEntryTuple *entry;
	Size		paylen;
	Size		size;

	paylen = lion_put_singleton(payload, ckey, lo);

	if (pm != NULL)
	{
		/* A key of a column that stores positions: its one chunk, inline. */
		LionEntryTuple *shape;
		LionEntryTuple *px;
		LionContainer *chunk = (LionContainer *) palloc0(LION_CONTAINER_MAX_SIZE);
		Size		ssize;

		Assert(reservedflag == 0);
		lion_poschunk_init(chunk, lion_pos_block(pm->code));
		if (!lion_poschunk_append(chunk, LION_CONTAINER_MAX_SIZE, pm))
			elog(ERROR, "lion index \"%s\": a row's positions do not make a position chunk",
				 RelationGetRelationName(index));
		shape = lion_make_entry(state, key, LION_ENTRY_INLINE, NULL, 0,
								&ssize);
		px = lion_entry_add_posext(shape, &ssize);
		entry = lion_entry_rebuild_pos(px, payload, paylen, paylen,
									   (const char *) chunk,
									   lion_poschunk_size(chunk), &size);
		pfree(shape);
		pfree(px);
		pfree(chunk);
	}
	else
		entry = (reservedflag != 0) ?
			lion_make_reserved_entry((AttrNumber) state->attno, reservedflag,
									LION_ENTRY_INLINE, payload, paylen, &size) :
			lion_make_entry(state, key, LION_ENTRY_INLINE,
							payload, paylen, &size);
	lion_entry_set_ncontainers(entry, 1);
	lion_entry_set_ntids(entry, 1);

	lion_insert_place_new(index, heaprel, state, leafbuf, off, movedright,
						  entry, size);
	pfree(entry);
}

/*
 * Put the new entry `entry` (size bytes) at the place the find-or-create
 * found for it, *leafbuf held EXCLUSIVE since: lion_insert_new_entry()'s
 * second half.
 */
static void
lion_insert_place_new(Relation index, Relation heaprel, LionState *state,
					  Buffer *leafbuf, OffsetNumber off, bool movedright,
					  LionEntryTuple *entry, Size size)
{
	int			max_entries = lion_max_entries(index);

	/*
	 * Cardinality guard (DESIGN.md §17), on the one path that can make the
	 * index grow a key: a brand new entry.
	 *
	 * An order-of-magnitude estimate from the shape of the directory (§21):
	 * the entries on THIS leaf times the number of leaves.  The meta page
	 * counts the directory's pages, and a directory of height h has at least
	 * h internal ones, so the leaves are at most that count less h, and one at
	 * least.  The index's other blocks are not leaves: they are the posting
	 * trees' and position trees' pages and the free ones, and a column of a
	 * few keys with many rows each is mostly those.  Crude by design:
	 * counting the whole index for every new key would read every leaf, and
	 * all this feeds is a warning.  The leaf is held: it comes before the meta
	 * page in a split's lock order, which takes the meta page last.
	 */
	if (max_entries > 0)
	{
		Page		page = BufferGetPage(*leafbuf);
		LionMetaPageData meta;
		int64		perleaf = (int64) PageGetMaxOffsetNumber(page) + 1;
		int64		nleaves;
		int64		estimate;

		lion_read_meta(index, &meta);
		nleaves = Max((int64) meta.dirpages - (int64) meta.height, 1);
		estimate = perleaf * nleaves;
		if (estimate > (int64) max_entries)
			lion_warn_max_entries(index, estimate);
	}

	lion_dir_add_entry(index, heaprel, state->ix, leafbuf, off, movedright,
					  entry, size);
}

/*
 * A new INLINE entry for the key of the batch row `row` (lion_insert_apply()),
 * with the payload given: a key's entry, a reserved one, or a bit slice.
 */
static LionEntryTuple *
lion_insert_row_entry(LionState *state, const LionInsertRow *row,
					  const char *payload, Size paylen, Size *size)
{
	if (row->flag == LION_ENTRY_SLICE)
		return lion_make_slice_entry((AttrNumber) state->attno,
									 (int) DatumGetInt32(row->key),
									 LION_ENTRY_INLINE, payload, paylen, size);
	if (row->flag != 0)
		return lion_make_reserved_entry((AttrNumber) state->attno, row->flag,
										LION_ENTRY_INLINE, payload, paylen,
										size);
	return lion_make_entry(state, row->key, LION_ENTRY_INLINE, payload, paylen,
						   size);
}

/*
 * lion_insert_new_entry() for the rows rows[0 .. n) of one key of a batch
 * (lion_insert_apply()), n at least one: the new INLINE entry takes as many
 * of them as its payload holds under the limit, all in its one record.
 * Returns how many it took, one at least.
 */
static int
lion_insert_new_entry_rows(Relation index, Relation heaprel, LionState *state,
						   Buffer *leafbuf, OffsetNumber off, bool movedright,
						   const LionInsertRow *rows, int n)
{
	char		first[LION_SPARSE_SINGLETON_MAX];
	LionEntryTuple *entry;
	Size		size;
	Size		paylen;
	Size		limit;
	char	   *cur;
	char	   *next;
	uint32		ncontainers = 1;
	int			taken = 1;
	int			added = 1;

	paylen = lion_put_singleton(first, lion_code_ckey(rows[0].code),
								lion_code_lo(rows[0].code));
	entry = lion_insert_row_entry(state, &rows[0], first, paylen, &size);

	limit = lion_inline_max(LionEntryPayloadOffset(entry),
							(Size) state->ix->meta.inline_limit);
	if (n > 1 && paylen < limit)
	{
		cur = (char *) palloc(limit + LION_CONTAINER_MAX_SIZE);
		next = (char *) palloc(limit + LION_CONTAINER_MAX_SIZE);
		memcpy(cur, first, paylen);
		for (; taken < n; taken++)
		{
			int			ndelta;
			Size		len;
			char	   *swap;

			len = lion_inline_add(index, BufferGetBlockNumber(*leafbuf), off,
								  cur, paylen,
								  lion_code_ckey(rows[taken].code),
								  lion_code_lo(rows[taken].code), next, &ndelta);
			if (len == 0)
				continue;		/* the same TID twice */
			if (len > limit)
				break;
			swap = cur;
			cur = next;
			next = swap;
			paylen = len;
			ncontainers += ndelta;
			added++;
		}
		if (added > 1)
		{
			LionEntryTuple *exact;
			Size		payoff;

			exact = lion_insert_row_entry(state, &rows[0], cur, paylen, &size);
			pfree(entry);
			entry = exact;

			/*
			 * Growth slack, as an insert's rewrite of an INLINE entry gives
			 * it (lion_inline_rebuilt()), where the leaf the entry goes on
			 * has the room for it: the key's next rows then go inside it.
			 */
			payoff = LionEntryPayloadOffset(exact);
			if (OffsetNumberIsValid(off))
			{
				Size		maxsize = MAXALIGN_DOWN(PageGetFreeSpace(BufferGetPage(*leafbuf)));
				Size		writesz = lion_entry_alloc_size(payoff, paylen,
															(Size) state->ix->meta.inline_limit,
															Max(maxsize, size));

				if (writesz > size)
				{
					entry = lion_entry_rebuild_slack(exact, cur, paylen,
													 writesz, &size);
					pfree(exact);
				}
			}
		}
		pfree(cur);
		pfree(next);
	}
	lion_entry_set_ncontainers(entry, ncontainers);
	lion_entry_set_ntids(entry, (uint64) added);

	lion_insert_place_new(index, heaprel, state, leafbuf, off, movedright,
						  entry, size);
	pfree(entry);
	return taken;
}

/*
 * The INLINE entry with positions `entry` (itemsz bytes on page, a payload
 * area of paylen bytes) rewritten around newpay (newlen bytes, ndelta more
 * items) and the inline chunk `chunk` (poslen bytes), counting nadded more
 * TIDs - the two fit under the limit together, which the caller checked - in
 * one record (DESIGN.md §17, W1).
 */
static void
lion_inline_pos_place(Relation index, Relation heaprel, LionState *state,
					  Buffer entrybuf, OffsetNumber entryoff,
					  LionEntryTuple *entry, Size itemsz, Size paylen,
					  const char *newpay, Size newlen, int ndelta, uint64 nadded,
					  const LionContainer *chunk, Size poslen)
{
	Page		page = BufferGetPage(entrybuf);
	Size		payoff = LionEntryPayloadOffset(entry);
	Size		limit = lion_inline_max(payoff,
										(Size) state->ix->meta.inline_limit);
	uint32		ncontainers = lion_entry_ncontainers(entry);
	uint64		ntids = lion_entry_ntids(entry);
	LionEntryTuple *newentry;
	Size		newsize;
	Size		payarea = paylen;	/* the payload area the entry has */

	/*
	 * Growth slack, as lion_insert_inline() gives it, for the payload area:
	 * keep the area the entry has while the payload fits it, and grow it with
	 * fresh slack - never past what leaves the positions room under the
	 * limit - when it does not.
	 */
	if (newlen > payarea)
	{
		Size		room = limit - poslen;
		Size		maxsize = MAXALIGN_DOWN(MAXALIGN(itemsz) +
											PageGetExactFreeSpace(page));
		Size		alloc;

		maxsize = (maxsize > MAXALIGN(poslen)) ? maxsize - MAXALIGN(poslen) : 0;
		alloc = lion_entry_alloc_size(payoff, newlen,
									  Min((Size) state->ix->meta.inline_limit, room),
									  Max(maxsize, payoff + newlen));
		payarea = alloc - payoff;
	}

	/*
	 * The caller's test is the payload's length; the entry is built at its
	 * AREA, slack included.  An area the payload still fits, kept from the
	 * last rewrite, plus positions that grew since can be past the limit -
	 * next to a long key that limit is LION_MAX_ENTRY_SIZE, and the rebuild
	 * then refuses the entry instead of anything spilling, for this row and
	 * for every later one of the key.  The slack goes first: the payload at
	 * its own length fits, which is what the caller checked.
	 */
	if ((MAXALIGN(payoff + payarea) - payoff) + poslen > limit)
		payarea = newlen;

	newentry = lion_entry_rebuild_pos(entry, newpay, newlen, payarea,
									  (const char *) chunk, poslen, &newsize);
	lion_entry_set_ncontainers(newentry, (uint32) ((int) ncontainers + ndelta));
	lion_entry_set_ntids(newentry, ntids + nadded);
	lion_dir_place(index, heaprel, state->ix, entrybuf, entryoff, true,
				   newentry, newsize);
	pfree(newentry);
}

/*
 * lion_insert_inline() for an entry that stores positions: newpay is its
 * payload with the row added (newlen bytes, ndelta more items), and pm is the
 * row's positions, which go into the entry's inline chunk.  Both land in one
 * record (DESIGN.md §17, W1), or, when the two together outgrow the entry,
 * both spill - the containers onto a posting tree and the chunk onto a new
 * position tree, the entry pointing at both in the spill's last record - and
 * the row goes in through the chain path.
 */
static void
lion_insert_inline_pos(Relation index, Relation heaprel, LionState *state,
					   Buffer entrybuf, OffsetNumber entryoff,
					   LionEntryTuple *entry, Size itemsz,
					   const char *oldpay, Size paylen,
					   const char *newpay, Size newlen, int ndelta,
					   const LionPosMember *pm)
{
	BlockNumber blk = BufferGetBlockNumber(entrybuf);
	Size		payoff = LionEntryPayloadOffset(entry);
	Size		limit = lion_inline_max(payoff,
										(Size) state->ix->meta.inline_limit);
	LionContainer *oldchunk = (LionContainer *) palloc0(LION_CONTAINER_MAX_SIZE);
	LionContainer *chunk = (LionContainer *) palloc0(LION_CONTAINER_MAX_SIZE);
	bool		hadchunk;
	bool		fits;
	Size		poslen = 0;

	hadchunk = lion_entry_inline_poschunk(index, entry, itemsz, blk, entryoff,
										  oldchunk);
	if (hadchunk)
		memcpy(chunk, oldchunk, lion_poschunk_size(oldchunk));
	else
		lion_poschunk_init(chunk, lion_pos_block(pm->code));

	fits = lion_poschunk_insert(chunk, LION_CONTAINER_MAX_SIZE, pm, NULL);
	if (fits)
	{
		poslen = lion_poschunk_size(chunk);
		fits = (MAXALIGN(payoff + newlen) - payoff) + poslen <= limit;
	}

	if (fits)
		lion_inline_pos_place(index, heaprel, state, entrybuf, entryoff, entry,
							  itemsz, paylen, newpay, newlen, ndelta, 1, chunk,
							  poslen);
	else
	{
		LionEntryTuple *chain;
		Size		chainsize;

		chain = lion_entry_chain_shape(entry, &chainsize);
		lion_entry_spill_pos(index, heaprel, entrybuf, entryoff, chain, oldpay,
							 paylen, hadchunk ? oldchunk : NULL);
		pfree(chain);

		lion_insert_chain(index, heaprel, entrybuf, entryoff,
						  lion_code_ckey(pm->code), lion_code_lo(pm->code), pm);
	}

	pfree(oldchunk);
	pfree(chunk);
}

/*
 * The INLINE entry `entry`, itemsz bytes on page, rebuilt around newpay
 * (newlen bytes, ndelta more items than it had) and counting nadded more
 * TIDs, at the length the growth slack gives it - palloc'd, *newsize bytes.
 *
 * GROWTH SLACK, the INLINE half of DESIGN.md §4.
 *
 * The payload this key had may already have room for the member that was
 * just added to it, because the last insert-driven rewrite left some: then
 * the entry KEEPS the bytes the page has allotted it (*newsize == itemsz)
 * and only its own bytes change.  PageIndexTupleOverwrite() then moves no
 * other entry on the leaf, as a change of length would (1.5-2.9 KB of record
 * in generic mode), and the record is a delta of the counters and the tail
 * of the payload.
 *
 * Otherwise the entry is written at its new length PLUS fresh slack, which is
 * what puts the next few inserts on the cheap path.  Only what fits on the
 * leaf as it stands: slack is never worth a split, and never worth compacting
 * an entry that has too much of it either.
 */
static LionEntryTuple *
lion_inline_rebuilt(LionState *state, Page page, LionEntryTuple *entry,
					Size itemsz, const char *newpay, Size newlen, int ndelta,
					uint64 nadded, Size *newsize)
{
	Size		payoff = LionEntryPayloadOffset(entry);
	Size		need = payoff + newlen;
	uint32		ncontainers = lion_entry_ncontainers(entry);
	uint64		ntids = lion_entry_ntids(entry);
	Size		writesz;
	LionEntryTuple *newentry;

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
										newsize);
	lion_entry_set_ncontainers(newentry, (uint32) ((int) ncontainers + ndelta));
	lion_entry_set_ntids(newentry, ntids + nadded);

	return newentry;
}

/*
 * Add (ckey, lo) to the INLINE entry at (entrybuf, entryoff), spilling the
 * posting set onto container pages if it no longer fits.  entrybuf is held
 * EXCLUSIVE.
 */
static void
lion_insert_inline(Relation index, Relation heaprel, LionState *state,
				  Buffer entrybuf,
				  OffsetNumber entryoff, uint32 ckey, uint16 lo,
				  const LionPosMember *pm)
{
	(void) lion_insert_inline_ext(index, heaprel, state, entrybuf, entryoff,
								  ckey, lo, pm, false);
}

/*
 * Would an entry of newsize bytes take the place of the one at off without
 * the leaf splitting - would PageIndexTupleOverwrite() take it?
 */
static inline bool
lion_page_overwrite_fits(Page page, OffsetNumber off, Size newsize)
{
	PageHeader	phdr = (PageHeader) page;

	return MAXALIGN(newsize) <=
		MAXALIGN(ItemIdGetLength(PageGetItemId(page, off))) +
		(Size) (phdr->pd_upper - phdr->pd_lower);
}

/*
 * lion_insert_inline(), and with nosplit for a leaf a writer took without a
 * descent (lion_dir_find_on_leaf()): returns false, having written nothing,
 * where the entry would have to grow past what the leaf has room for, which
 * would split it.  An entry with positions is not taken there at all.  A
 * spill is: it rewrites the entry smaller, where it stands.
 */
static bool
lion_insert_inline_ext(Relation index, Relation heaprel, LionState *state,
					   Buffer entrybuf, OffsetNumber entryoff, uint32 ckey,
					   uint16 lo, const LionPosMember *pm, bool nosplit)
{
	Page		page = BufferGetPage(entrybuf);
	ItemId		iid = PageGetItemId(page, entryoff);
	LionEntryTuple *entry = lion_page_entry_fetch(index, page,
												  BufferGetBlockNumber(entrybuf),
												  entryoff);
	Size		itemsz = ItemIdGetLength(iid);
	Size		payoff = LionEntryPayloadOffset(entry);
	Size		paylen = LION_ENTRY_PAYLOAD_LEN(entry, itemsz);
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
	newlen = lion_inline_add(index, BufferGetBlockNumber(entrybuf), entryoff,
							 oldpay, paylen, ckey, lo, newpay, &ndelta);

	if (newlen == 0)
	{
		/* The TID is already in this posting set; nothing to do. */
		pfree(newpay);
		pfree(oldpay);
		return true;
	}

	if (pm != NULL)
	{
		Assert(!nosplit);
		lion_insert_inline_pos(index, heaprel, state, entrybuf, entryoff,
							   entry, itemsz, oldpay, paylen, newpay, newlen,
							   ndelta, pm);
		pfree(newpay);
		pfree(oldpay);
		return true;
	}

	if (newlen <= lion_inline_max(payoff,
								  (Size) state->ix->meta.inline_limit))
	{
		LionEntryTuple *newentry;
		Size		newsize;

		newentry = lion_inline_rebuilt(state, page, entry, itemsz, newpay,
									   newlen, ndelta, 1, &newsize);

		if (nosplit && !lion_page_overwrite_fits(page, entryoff, newsize))
		{
			pfree(newentry);
			pfree(newpay);
			pfree(oldpay);
			return false;
		}

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
		return true;
	}

	/*
	 * The posting set has outgrown the entry tuple (or the bucket page).
	 * Move the containers it had onto a chain, then add the new member
	 * through the chain path.
	 */
	{
		LionEntryTuple *chain;
		Size		chainsize;

		chain = lion_entry_chain_shape(entry, &chainsize);

		lion_entry_spill(index, heaprel, entrybuf, entryoff, chain, oldpay,
						paylen);
		pfree(chain);
	}

	pfree(newpay);
	pfree(oldpay);

	lion_insert_chain(index, heaprel, entrybuf, entryoff, ckey, lo, NULL);
	return true;
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
 *	NARROW	when the member's offset is below 64 times its width, the ones
 *			it has bits for (DESIGN.md §38); one past them widens it, to
 *			the narrowest NARROW that holds the member.
 *
 * which is lion_container_inplace_need(), the table the redo of the record
 * written here checks too.
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
	LionWalState *xstate;

	switch (lion_container_inplace_check(index, buf, off, lo))
	{
		case LION_INPLACE_NO:
			return false;
		case LION_INPLACE_DUP:
			*done = true;
			return true;		/* already indexed: no record at all */
		default:
			break;
	}
	*done = true;

	xstate = lion_wal_begin(index);
	lion_container_inplace_apply(xstate, buf, off, lo);

	lion_entry_set_ntids(entry, lion_entry_ntids(entry) + 1);

	/* The entry always travels with the container change. */
	if (!lion_replace_entry(index, xstate, entrybuf, entryoff, entry, entrysize))
		elog(ERROR, "lion index: could not update entry %u on block %u",
			 entryoff, BufferGetBlockNumber(entrybuf));

	lion_wal_finish(xstate, LION_XLOG_ITEM_SET);

	return true;
}

/*
 * The first half of lion_insert_container_inplace(), which decides before any
 * record opens: can the container at (buf, off) - held EXCLUSIVE, and come
 * through lion_page_item_fetch() - take lo in place (LION_INPLACE_YES), does
 * it hold lo already (LION_INPLACE_DUP), or does the member need the general
 * path (LION_INPLACE_NO)?  The ERROR of lion_page_check_alone() is raised
 * here, outside the record, for a YES.
 */
static int
lion_container_inplace_check(Relation index, Buffer buf, OffsetNumber off,
							 uint16 lo)
{
	Page		page = BufferGetPage(buf);
	ItemId		iid = PageGetItemId(page, off);
	int			r;

	r = lion_container_inplace_decide((LionContainer *) PageGetItem(page, iid),
									  ItemIdGetLength(iid), lo);

	/*
	 * The member goes into the bytes the line pointer allots the item, which
	 * lion_page_item_fetch() has bounded - and which must not reach into the
	 * next item: this wrote into that one's header before, and logged it.
	 */
	if (r == LION_INPLACE_YES)
		lion_page_check_alone(index, page, BufferGetBlockNumber(buf), off);

	return r;
}

/*
 * lion_container_inplace_check()'s decision for the container c, allotted
 * alloc bytes: the page's, or a copy that earlier members of a batch went
 * into (lion_insert_chain_rows()).
 */
static int
lion_container_inplace_decide(const LionContainer *onpage, Size alloc,
							  uint16 lo)
{
	Size		need;

	need = lion_container_inplace_need(onpage, lo);
	if (need == 0)
		return LION_INPLACE_NO;	/* would change its representation */

	/*
	 * A member that closes the one-value gap between two runs MERGES them,
	 * and the container comes out a run SHORTER while the item keeps its
	 * allotment.  In place, a stream of such inserts would grow the unused
	 * tail of the item without bound; the slack an item may carry is
	 * LION_ITEM_SLACK_BOUND, the rule VACUUM's shrink-in-place already
	 * follows (DESIGN.md §4, §18).  So a merge that would take the item past
	 * it is left to the general path, which rewrites the item at its logical
	 * size plus normal growth slack.  Nothing else shrinks a container in
	 * place: an ARRAY and a new run only grow, an extended run, a BITSET and
	 * a NARROW keep their size.
	 */
	if (onpage->type == LION_CT_RUN &&
		lo > 0 && lo < LION_LO_MASK &&
		lion_container_contains(onpage, (uint16) (lo - 1)) &&
		lion_container_contains(onpage, (uint16) (lo + 1)) &&
		!lion_container_contains(onpage, lo) &&
		alloc > lion_container_size(onpage) - sizeof(LionRun) +
		LION_ITEM_SLACK_BOUND)
		return LION_INPLACE_NO;

	if (alloc < need)
		return LION_INPLACE_NO;

	/* Already indexed: do not start a record at all. */
	if (lion_container_contains(onpage, lo))
		return LION_INPLACE_DUP;

	return LION_INPLACE_YES;
}

/*
 * The second half, inside the record xstate: add lo to the container at
 * (buf, off), which lion_container_inplace_check() said takes it in place,
 * and log the operation.  Registers buf; the caller logs the entry.
 */
static void
lion_container_inplace_apply(LionWalState *xstate, Buffer buf,
							 OffsetNumber off, uint16 lo)
{
	Page		p = lion_wal_register_buffer(xstate, buf, LION_WALBUF_STD);
	ItemId		iid = PageGetItemId(p, off);
	LionContainer *c = (LionContainer *) PageGetItem(p, iid);

	if (!lion_container_add(c, lo))
		elog(ERROR, "lion index: container %u on block %u changed under an exclusive lock",
			 c->ckey, BufferGetBlockNumber(buf));

	/* The whole point: the item still ends where it ended. */
	Assert(lion_container_size(c) <= ItemIdGetLength(iid));

	/*
	 * This is the hot-key insert record of DESIGN.md §25, and it logs the
	 * CALL rather than the bytes: adding a member to a sorted ARRAY shifts
	 * every element above it, so the bytes that change run to the end of the
	 * item, while "add member lo to the item at offset off" is four.  Replay
	 * calls the same library function on the same item, which is what makes
	 * the two pages identical to the byte.
	 */
	lion_wal_op(xstate, p, LION_OP_CONTAINER_ADD, off, lo, NULL, 0);
}

/*
 * The same for a sparse segment that is already on a page: insert the pair in
 * its sorted place inside the item, which rewrites the stream from the
 * insertion point on (nothing but the header and the new bytes at the end,
 * for a pair above every other) and leaves the item's allocated length and
 * offset untouched.
 *
 * Only for a pair that really stays in the segment: a container key that
 * reaches LION_SPARSE_THRESHOLD members has to be promoted to a container of
 * its own, which turns one item into up to three and belongs to
 * lion_insert_segment().  Unlike a container, a segment covers a RANGE of
 * container keys, and the pair may extend it, so the page bounds are
 * recomputed.  pr is the segment's probe for the pair (lion_chain_target()).
 */
static bool
lion_insert_segment_inplace(Relation index, Buffer buf, OffsetNumber off,
						   Buffer entrybuf, OffsetNumber entryoff,
						   LionEntryTuple *entry, Size entrysize,
						   uint32 ckey, uint16 lo,
						   const LionSparseProbe *pr, bool *done)
{
	LionWalState *xstate;

	switch (lion_segment_inplace_check(index, buf, off, pr))
	{
		case LION_INPLACE_NO:
			return false;
		case LION_INPLACE_DUP:
			*done = true;
			return true;
		default:
			break;
	}
	*done = true;

	xstate = lion_wal_begin(index);
	lion_segment_inplace_apply(xstate, buf, off, ckey, lo, pr);

	lion_entry_set_ntids(entry, lion_entry_ntids(entry) + 1);

	if (!lion_replace_entry(index, xstate, entrybuf, entryoff, entry, entrysize))
		elog(ERROR, "lion index: could not update entry %u on block %u",
			 entryoff, BufferGetBlockNumber(entrybuf));

	lion_wal_finish(xstate, LION_XLOG_ITEM_SET);

	return true;
}

/*
 * The first half of lion_insert_segment_inplace(), as for a container, from
 * the segment's probe for the pair (lion_sparse_probe()).
 */
static int
lion_segment_inplace_check(Relation index, Buffer buf, OffsetNumber off,
						   const LionSparseProbe *pr)
{
	Page		page = BufferGetPage(buf);
	ItemId		iid = PageGetItemId(page, off);
	LionContainer *onpage = (LionContainer *) PageGetItem(page, iid);
	Size		alloc = ItemIdGetLength(iid);
	Size		need;

	Assert(onpage->type == LION_CT_SPARSE);

	need = lion_sparse_probe_insert_size(onpage, pr);
	if (need == 0)
		return LION_INPLACE_NO;	/* would have to split */
	if (pr->ckey_pairs + 1 >= LION_SPARSE_THRESHOLD)
		return LION_INPLACE_NO;	/* would have to promote the container key */
	if (alloc < need)
		return LION_INPLACE_NO;	/* no slack inside the item */

	if (pr->found)
		return LION_INPLACE_DUP;

	/* As for a container (lion_container_inplace_check()). */
	lion_page_check_alone(index, page, BufferGetBlockNumber(buf), off);

	return LION_INPLACE_YES;
}

/*
 * The second half: the pair in, inside the record, and the page's bounds.
 * pr is the segment's probe for the pair from the first half, taken under
 * the same lock, whose bytes the page image the record registers has too.
 */
static void
lion_segment_inplace_apply(LionWalState *xstate, Buffer buf, OffsetNumber off,
						   uint32 ckey, uint16 lo, const LionSparseProbe *pr)
{
	Page		p = lion_wal_register_buffer(xstate, buf, LION_WALBUF_STD);
	ItemId		iid = PageGetItemId(p, off);
	LionContainer *s = (LionContainer *) PageGetItem(p, iid);
	bool		dup = false;
	bool		ok;

	ok = lion_sparse_insert_probed(s, pr, &dup);
	if (!ok || dup)
		elog(ERROR, "lion index: sparse segment %u on block %u changed under an exclusive lock",
			 off, BufferGetBlockNumber(buf));

	Assert(lion_sparse_size(s) <= ItemIdGetLength(iid));

	/*
	 * The call, not the bytes: a pair inside the segment rewrites the stream
	 * from where it goes in.
	 */
	lion_wal_op(xstate, p, LION_OP_SPARSE_INS, off, lo, &ckey, sizeof(uint32));

	/* A segment's range can grow in either direction. */
	lion_page_update_minmax(p);
	lion_wal_op(xstate, p, LION_OP_MINMAX, 0, 0, NULL, 0);
}

/*
 * Lock the posting-tree leaf that owns ckey EXCLUSIVE, for the CHAIN insert
 * path, and say in *ontail whether it was the append hint that found it.
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
 * What the hint skips is not only the descent's locks but its REPAIRS: a
 * write descent finishes every unfinished split it meets (DESIGN.md §22), and
 * the tail may be the right half of one - the split that made it the tail
 * wrote its first record and then crashed, or failed to put the downlink in
 * - so that it has no downlink of its own.  A leaf taken through the hint is
 * therefore good only for a change that STAYS on it; lion_insert_chain()
 * descends for anything that may split it.
 *
 * Nothing is held while descending: the tail lock is dropped first, so no page
 * is ever locked before a page to its left (DESIGN.md §22 rule 3).
 *
 * entryblk is the directory leaf the caller holds EXCLUSIVE.  The entry's
 * head and tail are links read off it, and data like any other (DESIGN.md
 * §21, "Readers"): InvalidBlockNumber is P_NEW, which ReadBuffer() answers by
 * EXTENDING the index, once per insert, and a link to the leaf itself would
 * lock it a second time and wait for this backend for ever.  Both are refused
 * before anything is read; a block past the end fails in ReadBuffer().
 */
static Buffer
lion_insert_lock_chain_page(Relation index, Relation heaprel,
						   BlockNumber entryblk, uint32 hash,
						   BlockNumber head, BlockNumber tail, uint32 ckey,
						   bool *ontail)
{
	Buffer		buf;
	Page		page;

	if (unlikely(!BlockNumberIsValid(head) || !BlockNumberIsValid(tail) ||
				 head == entryblk || tail == entryblk))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": a chain entry on block %u links to head %u and tail %u, and one of them cannot be a page of its posting set",
						RelationGetRelationName(index), entryblk, head, tail)));

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
	{
		*ontail = true;
		return buf;
	}

	UnlockReleaseBuffer(buf);

	*ontail = false;
	return lion_posting_search(index, heaprel, hash, head, ckey,
							   BUFFER_LOCK_EXCLUSIVE, true);
}

/*
 * Which item of the posting leaf buf, held EXCLUSIVE and owning ckey, takes
 * (ckey, lo), and at what offset (*off):
 *
 *	LION_TARGET_CONTAINER	the container with that very ckey;
 *	LION_TARGET_SEGMENT		the sparse segment that covers the ckey, or else
 *							the one immediately before the gap the ckey falls
 *							in, or else the one immediately after it, when it
 *							has room for the pair - both are adjacent items,
 *							so nothing can end up between the pair and the
 *							segment it joins;
 *	LION_TARGET_NEW			none: the pair becomes a new one-pair segment,
 *							inserted at *off.
 *
 * For LION_TARGET_SEGMENT *pr is the segment's probe for the pair
 * (lion_sparse_probe()), the one decode of it the insert needs.
 *
 * Every item looked at here has been through lion_page_item_fetch(), so the
 * in-place paths may take the item at *off as it stands.  One decision for
 * lion_insert_chain_leaf() and for the slices' batched in-place adds
 * (lion_slice_batch_add()), which therefore put a member where the other
 * would.
 */
#define LION_TARGET_CONTAINER	0
#define LION_TARGET_SEGMENT		1
#define LION_TARGET_NEW			2

static int
lion_chain_target(Relation index, Buffer buf, uint32 ckey, uint16 lo,
				  OffsetNumber *offp, LionSparseProbe *pr)
{
	Page		cpage = BufferGetPage(buf);
	BlockNumber blk = BufferGetBlockNumber(buf);
	OffsetNumber off;
	OffsetNumber maxoff;
	bool		found;

	off = lion_page_find_item(index, cpage, blk, ckey, &found);
	*offp = off;

	if (found)
	{
		LionContainer *item = lion_page_item_fetch(index, cpage, blk, off);

		if (item->type != LION_CT_SPARSE)
			return LION_TARGET_CONTAINER;
		lion_sparse_probe(item, ckey, lo, pr);
		return LION_TARGET_SEGMENT;
	}

	maxoff = PageGetMaxOffsetNumber(cpage);
	if (off > FirstOffsetNumber &&
		lion_segment_has_room(lion_page_item_fetch(index, cpage, blk,
												   OffsetNumberPrev(off)),
							  ckey, lo, pr))
	{
		*offp = OffsetNumberPrev(off);
		return LION_TARGET_SEGMENT;
	}
	if (off <= maxoff &&
		lion_segment_has_room(lion_page_item_fetch(index, cpage, blk, off),
							  ckey, lo, pr))
		return LION_TARGET_SEGMENT;

	return LION_TARGET_NEW;
}

/*
 * Add (ckey, lo) on the leaf buf, which owns ckey and is held EXCLUSIVE, for
 * the CHAIN entry at (entrybuf, entryoff), also held EXCLUSIVE.  ecopy is the
 * caller's private copy of that entry (esize bytes) and cbuf a work buffer of
 * LION_CONTAINER_MAX_SIZE bytes.
 *
 * With mayput false only the in-place paths may run - the ones that change a
 * member inside an item and nothing else on the page.  When the member needs
 * the general path instead, which rewrites the item and may split the page,
 * nothing at all is done (ecopy included) and the answer is false.  Otherwise
 * the member is in (or was already) and the answer is true.
 */
static bool
lion_insert_chain_leaf(Relation index, Relation heaprel, Buffer buf,
					   Buffer entrybuf, OffsetNumber entryoff,
					   LionEntryTuple *ecopy, Size esize, LionContainer *cbuf,
					   uint32 ckey, uint16 lo, bool mayput)
{
	OffsetNumber off;
	int			delta;
	LionSparseProbe probe;

	switch (lion_chain_target(index, buf, ckey, lo, &off, &probe))
	{
		case LION_TARGET_SEGMENT:
			{
				bool		done = false;

				if (lion_insert_segment_inplace(index, buf, off, entrybuf,
												entryoff, ecopy, esize, ckey,
												lo, &probe, &done))
					return true;
				if (!mayput)
					return false;
				lion_insert_segment(index, heaprel, buf, off, true, entrybuf,
								   entryoff, ecopy, ckey, lo, &probe);
				return true;
			}
		case LION_TARGET_NEW:
			if (!mayput)
				return false;
			lion_insert_segment(index, heaprel, buf, off, false, entrybuf,
							   entryoff, ecopy, ckey, lo, NULL);
			return true;
		default:
			break;
	}

	/* A container with this ckey. */
	{
		Page		cpage = BufferGetPage(buf);
		BlockNumber blk = BufferGetBlockNumber(buf);
		ItemId		ciid = PageGetItemId(cpage, off);
		bool		done = false;

		Assert(((LionContainer *) PageGetItem(cpage, ciid))->ckey == ckey);

		if (lion_insert_container_inplace(index, buf, off, entrybuf, entryoff,
										 ecopy, esize, lo, &done))
		{
			Assert(done);
			return true;
		}
		if (!mayput)
			return false;

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
			return true;		/* already indexed */

		/*
		 * The item is written anew, so as the smallest that holds its
		 * members, as the build and VACUUM write theirs (DESIGN.md §38): an
		 * ARRAY that a NARROW would be smaller than becomes one here, and
		 * not a BITSET at 2048 members.  In place, between two of these, it
		 * grows by the slack it was given (lion_item_alloc_size()).
		 */
		lion_container_optimize(cbuf);
	}

	lion_entry_set_ntids(ecopy, lion_entry_ntids(ecopy) + 1);

	lion_chain_put_container_locked_ext(index, heaprel, buf, entrybuf, entryoff,
									   ecopy, cbuf, &delta, true);

	Assert(delta == 0);
	(void) delta;

	return true;
}

/*
 * Add (ckey, lo) to the CHAIN entry at (entrybuf, entryoff), which is held
 * EXCLUSIVE.
 *
 * The leaf comes from the append hint when the hint qualifies, and then it is
 * used only for the in-place paths, which are what an appending key takes
 * nearly every time (a BITSET always, a NARROW for a member within its
 * width, anything else while its item has growth slack).  A member that needs the general path lets the tail go and DESCENDS
 * to it, forwrite: the general path can split the page, and a page that is the
 * right half of an unfinished split must not be split before the split that
 * made it is finished (DESIGN.md §22), which is what the descent does on its
 * way down - it routes to the flagged left half, because the right half has
 * no downlink.  Were every append to go to the tail, the flagged page would
 * never be descended to, and the first split of the tail would find no
 * downlink for it - and so would every later one.  lion_posting_find_parent()
 * repairs that case as well; the descent is what makes it rare, and puts the
 * repair on the first general-path insert instead of on the tail's next
 * split.
 *
 * The descent after a declined hint almost always lands on the tail again,
 * so the cost is the descent's own locks - one per level - on the general
 * path only.
 */
static void
lion_insert_chain(Relation index, Relation heaprel, Buffer entrybuf,
				 OffsetNumber entryoff,
				 uint32 ckey, uint16 lo, const LionPosMember *pm)
{
	Page		page = BufferGetPage(entrybuf);
	LionEntryTuple *entry;
	LionEntryTuple *ecopy;
	LionContainer *cbuf;
	Size		esize;
	Buffer		buf;
	bool		ontail;

	/*
	 * Every record below rewrites the entry in place, at the size it has,
	 * inside the record (lion_put_entry()): it has to have the size a CHAIN
	 * entry has, or that write fails there - a PANIC in rmgr mode - and be
	 * alone in its bytes (lion_page_check_alone(), below), or the write
	 * overwrites the item that shares them, and logs it.
	 */
	entry = lion_page_entry_fetch(index, page, BufferGetBlockNumber(entrybuf),
								  entryoff);

	/*
	 * The row's positions go in first (DESIGN.md §17, W1): a reader that
	 * finds the TID in the posting set finds its positions too.
	 */
	if (pm != NULL)
	{
		const LionEntryPosExt *x = lion_entry_posext(entry);

		if (x == NULL || !BlockNumberIsValid(x->pos_root))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("lion index \"%s\": chain entry %u on block %u stores no position tree",
							RelationGetRelationName(index), entryoff,
							BufferGetBlockNumber(entrybuf))));
		(void) lion_postree_put(index, heaprel, lion_entry_owner(entry), x->pos_root, pm);
		entry = lion_page_entry_fetch(index, page,
									  BufferGetBlockNumber(entrybuf), entryoff);
	}
	lion_page_check_alone(index, page, BufferGetBlockNumber(entrybuf), entryoff);
	ecopy = lion_entry_rebuild(entry, NULL, 0, &esize);
	Assert(esize == ItemIdGetLength(PageGetItemId(page, entryoff)));
	Assert((ecopy->flags & LION_ENTRY_CHAIN) != 0);

	cbuf = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);

	buf = lion_insert_lock_chain_page(index, heaprel,
									 BufferGetBlockNumber(entrybuf),
									 lion_entry_owner(ecopy), lion_entry_head(ecopy),
									 lion_entry_tail(ecopy),
									 ckey, &ontail);

	if (!lion_insert_chain_leaf(index, heaprel, buf, entrybuf, entryoff, ecopy,
								esize, cbuf, ckey, lo, !ontail))
	{
		bool		done;

		Assert(ontail);
		UnlockReleaseBuffer(buf);
		buf = lion_posting_search(index, heaprel, lion_entry_owner(ecopy),
								  lion_entry_head(ecopy),
								  ckey, BUFFER_LOCK_EXCLUSIVE, true);
		done = lion_insert_chain_leaf(index, heaprel, buf, entrybuf, entryoff,
									  ecopy, esize, cbuf, ckey, lo, true);
		Assert(done);
		(void) done;
	}

	UnlockReleaseBuffer(buf);
	pfree(cbuf);
	pfree(ecopy);
}

/*
 * The rows rows[0 .. n) of one key of a batch (lion_insert_apply()) into its
 * CHAIN entry at (entrybuf, entryoff), held EXCLUSIVE, from the first: the
 * rows of the first one's container key that its container takes in place,
 * in ONE record - a CONTAINER_ADD for each member, and the entry's counter
 * once - where lion_insert_chain() writes a record a row; or, when that is
 * not two rows or more (the key has a sparse segment there, or none, or the
 * container must be rewritten), the first row alone through
 * lion_insert_chain().  Returns how many rows it took, one at least.  A TID
 * the set holds already counts as taken.
 */
static int
lion_insert_chain_rows(Relation index, Relation heaprel, Buffer entrybuf,
					   OffsetNumber entryoff, const LionInsertRow *rows, int n)
{
	Page		page = BufferGetPage(entrybuf);
	BlockNumber entryblk = BufferGetBlockNumber(entrybuf);
	uint32		ckey = lion_code_ckey(rows[0].code);
	LionEntryTuple *entry;
	LionEntryTuple *ecopy;
	LionContainer *copy;
	uint16	   *los;
	Size		esize;
	Size		alloc;
	Buffer		buf;
	Page		cpage;
	OffsetNumber off;
	LionWalState *xstate;
	bool		ontail;
	bool		found;
	int			run;
	int			taken = 0;
	int			nlos = 0;
	int			i;

	for (run = 1; run < n && lion_code_ckey(rows[run].code) == ckey; run++)
		;
	if (run < 2)
		goto one;

	entry = lion_page_entry_fetch(index, page, entryblk, entryoff);
	lion_page_check_alone(index, page, entryblk, entryoff);
	ecopy = lion_entry_rebuild(entry, NULL, 0, &esize);
	Assert(esize == ItemIdGetLength(PageGetItemId(page, entryoff)));
	Assert((ecopy->flags & LION_ENTRY_CHAIN) != 0);

	buf = lion_insert_lock_chain_page(index, heaprel, entryblk,
									  lion_entry_owner(ecopy),
									  lion_entry_head(ecopy),
									  lion_entry_tail(ecopy), ckey, &ontail);
	cpage = BufferGetPage(buf);
	off = lion_page_find_item(index, cpage, BufferGetBlockNumber(buf), ckey,
							  &found);
	if (!found ||
		lion_page_item_fetch(index, cpage, BufferGetBlockNumber(buf),
							 off)->type == LION_CT_SPARSE)
	{
		UnlockReleaseBuffer(buf);
		pfree(ecopy);
		goto one;
	}

	/*
	 * Which members go in in place, decided on a copy of the container that
	 * takes each in turn, as the page will - before the record opens.
	 */
	alloc = ItemIdGetLength(PageGetItemId(cpage, off));
	if (alloc > LION_CONTAINER_MAX_SIZE)
		elog(ERROR, "lion index: container of %zu bytes at %u/%u",
			 alloc, BufferGetBlockNumber(buf), off);
	copy = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	memcpy(copy, PageGetItem(cpage, PageGetItemId(cpage, off)), alloc);
	los = (uint16 *) palloc(sizeof(uint16) * run);
	for (; taken < run; taken++)
	{
		uint16		lo = lion_code_lo(rows[taken].code);
		int			r = lion_container_inplace_decide(copy, alloc, lo);

		if (r == LION_INPLACE_NO)
			break;
		if (r == LION_INPLACE_DUP)
			continue;
		if (!lion_container_add(copy, lo))
			elog(ERROR, "lion index: could not add a member to a copy of container %u",
				 ckey);
		los[nlos++] = lo;
	}
	pfree(copy);

	if (nlos > 0 && (nlos > 1 || taken > 1))
	{
		lion_page_check_alone(index, cpage, BufferGetBlockNumber(buf), off);
		xstate = lion_wal_begin(index);
		for (i = 0; i < nlos; i++)
			lion_container_inplace_apply(xstate, buf, off, los[i]);
		lion_entry_set_ntids(ecopy, lion_entry_ntids(ecopy) + nlos);
		if (!lion_replace_entry(index, xstate, entrybuf, entryoff, ecopy,
								esize))
			elog(ERROR, "lion index: could not update entry %u on block %u",
				 entryoff, entryblk);
		lion_wal_finish(xstate, LION_XLOG_ITEM_SET);
	}
	else if (nlos > 0)
		taken = 0;				/* one member: the ordinary path's */
	UnlockReleaseBuffer(buf);
	pfree(los);
	pfree(ecopy);
	if (taken > 0)
		return taken;

one:
	lion_insert_chain(index, heaprel, entrybuf, entryoff, ckey,
					  lion_code_lo(rows[0].code), NULL);
	return 1;
}

/*
 * The row of a batch `row` of a column that stores positions, as the member
 * lion_insert_one() would put in for it (m holds LION_POS_MAX_NPOS).
 */
static void
lion_insert_row_member(const LionState *state, const LionInsertRow *row,
					   LionPosMember *m)
{
	const LionDeferPos *dp = lion_defer_row_pos(state, row);

	m->code = row->code;
	m->npos = dp->npos;
	m->dl = dp->dl;
	memcpy(m->pos, dp->pos, sizeof(uint16) * dp->npos);
}

/*
 * The rows rows[0 .. n) of one key of a column that stores positions
 * (DESIGN.md §17) into its CHAIN entry at (entrybuf, entryoff), held
 * EXCLUSIVE, as lion_insert_chain() puts in one: their positions first, into
 * the entry's position tree - a run appended to the tree's last chunk in one
 * rewrite of it (lion_postree_put_rows()) - so that a reader that finds a TID
 * in the posting set finds its positions too; then the TIDs, a container's
 * in one record (lion_insert_chain_rows()).
 */
static void
lion_insert_chain_pos_rows(Relation index, Relation heaprel, LionState *state,
						   Buffer entrybuf, OffsetNumber entryoff,
						   const LionInsertRow *rows, int n)
{
	Page		page = BufferGetPage(entrybuf);
	LionEntryTuple *entry;
	const LionEntryPosExt *x;
	LionPosMember **ms;
	int			i;

	entry = lion_page_entry_fetch(index, page, BufferGetBlockNumber(entrybuf),
								  entryoff);
	x = lion_entry_posext(entry);
	if (x == NULL || !BlockNumberIsValid(x->pos_root))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": chain entry %u on block %u stores no position tree",
						RelationGetRelationName(index), entryoff,
						BufferGetBlockNumber(entrybuf))));

	ms = (LionPosMember **) palloc(sizeof(LionPosMember *) * n);
	for (i = 0; i < n; i++)
	{
		ms[i] = (LionPosMember *) palloc(sizeof(LionPosMember));
		lion_insert_row_member(state, &rows[i], ms[i]);
	}
	lion_postree_put_rows(index, heaprel, lion_entry_owner(entry),
						  x->pos_root, ms, n);
	for (i = 0; i < n; i++)
		pfree(ms[i]);
	pfree(ms);

	for (i = 0; i < n;)
		i += lion_insert_chain_rows(index, heaprel, entrybuf, entryoff,
									&rows[i], n - i);
}

/* ---------------------------------------------------------------------
 * Summary posting sets (DESIGN.md §32)
 * --------------------------------------------------------------------- */

/* A new SUMLAST entry: the open bucket of a column, holding one row. */
static LionEntryTuple *
lion_summary_make_last(LionState *state, Datum key, uint32 hash, uint32 ckey,
					   uint16 lo, Size *size)
{
	char		payload[LION_CONTAINER_HDRSZ + sizeof(uint32) + sizeof(uint16)];
	Size		paylen = lion_put_singleton(payload, ckey, lo);
	LionEntryTuple *entry;

	entry = lion_make_entry(state, key,
							LION_ENTRY_INLINE | LION_ENTRY_SUMMARY |
							LION_ENTRY_SUMLAST,
							payload, paylen, size);
	lion_entry_set_ncontainers(entry, 1);
	lion_entry_set_ntids(entry, 1);
	return entry;
}

/*
 * The column's SUMLAST entry at (buf, off), held EXCLUSIVE, holds fewer than
 * summary_tids rows and a key below `key`: raise its key to `key` - the
 * largest key its bucket holds once the caller's row is in it - and put the
 * row in, under the lock the lookup took.  Returns false when the caller has
 * to look the entry up again and add the row there (nothing is lost: the key
 * is raised, and the next lookup finds the entry with a key at or above the
 * row's).
 *
 * An INLINE entry (the open bucket of an appending column nearly always is)
 * is rebuilt around the new key with the row already in its payload and put
 * back through lion_dir_place() - ONE record, which splits the leaf when the
 * entry no longer fits it; the SUMLAST kind sorts after every other summary
 * whatever its key, so its position does not change.  So an append to a
 * summarized column costs one descent and one record, not one for the key
 * and another for the row.  A payload that the row and a longer key push
 * past what an entry may inline is spilled first (lion_entry_spill(), as an
 * insert's would be), with the old key, and the entry is then a CHAIN.
 *
 * A CHAIN entry's key is raised in a record of its own, and the row goes into
 * its posting tree through the chain insert straight after, still under the
 * leaf's lock - unless the rewrite split the leaf and moved the entry off it,
 * which is what the false return is for.
 */
static bool
lion_summary_rekey_insert(Relation index, Relation heaprel, LionState *state,
						  Buffer buf, OffsetNumber off, Datum key,
						  uint32 hash, uint32 ckey, uint16 lo)
{
	Page		page = BufferGetPage(buf);
	ItemId		iid = PageGetItemId(page, off);
	LionEntryTuple *e = lion_page_entry_fetch(index, page,
											  BufferGetBlockNumber(buf), off);
	LionEntryTuple *newentry;
	Size		newsize;
	Size		newpayoff = lion_entry_payoff_for(e->flags,
												  lion_key_datum_size(state, key));
	Size		inline_limit = (Size) state->ix->meta.inline_limit;

	if ((e->flags & LION_ENTRY_INLINE) != 0)
	{
		Size		itemsz = ItemIdGetLength(iid);
		Size		payoff = LionEntryPayloadOffset(e);
		Size		paylen = LION_ENTRY_PAYLOAD_LEN(e, itemsz);
		char	   *oldpay = (char *) palloc(Max(paylen, 1));
		char	   *newpay = (char *) palloc(paylen + LION_CONTAINER_MAX_SIZE);
		Size		newlen;
		int			ndelta = 0;
		uint64		added = 1;

		memcpy(oldpay, (char *) e + payoff, paylen);
		newlen = lion_inline_add(index, BufferGetBlockNumber(buf), off,
								 oldpay, paylen, ckey, lo, newpay, &ndelta);
		if (newlen == 0)
		{
			/* The row is in already; only the key is raised. */
			memcpy(newpay, oldpay, paylen);
			newlen = paylen;
			ndelta = 0;
			added = 0;
		}

		if (newpayoff >= (Size) LION_MAX_ENTRY_SIZE ||
			newlen > lion_inline_max(newpayoff, inline_limit))
		{
			LionEntryTuple *chain;
			Size		chainsize;

			chain = lion_entry_chain_shape(e, &chainsize);
			lion_entry_spill(index, heaprel, buf, off, chain, oldpay, paylen);
			pfree(chain);
			pfree(newpay);
			pfree(oldpay);

			/* The CHAIN entry took the INLINE one's place, and is smaller. */
			e = lion_page_entry(BufferGetPage(buf), off);
			Assert(LionEntryIsSumLast(e) && (e->flags & LION_ENTRY_CHAIN) != 0);
		}
		else
		{
			LionEntryTuple *shape;
			Size		shapesize;
			Size		need = newpayoff + newlen;
			Size		writesz;

			/*
			 * Growth slack as lion_insert_inline() gives it (DESIGN.md §4):
			 * the bytes the page has allotted the entry when they still fit
			 * the new payload, else its new length and fresh slack.
			 */
			if (itemsz >= need && itemsz - need <= LION_ENTRY_SLACK_BOUND)
				writesz = itemsz;
			else
			{
				Size		maxsize = MAXALIGN_DOWN(MAXALIGN(itemsz) +
													PageGetExactFreeSpace(page));

				writesz = lion_entry_alloc_size(newpayoff, newlen, inline_limit,
												Max(maxsize, need));
			}

			shape = lion_make_entry(state, key, e->flags, NULL, 0,
									&shapesize);
			newentry = lion_entry_rebuild_slack(shape, newpay, newlen, writesz,
												&newsize);
			lion_entry_set_ncontainers(newentry, (uint32) ((int) lion_entry_ncontainers(e) + ndelta));
			lion_entry_set_ntids(newentry, lion_entry_ntids(e) + added);
			pfree(shape);
			pfree(newpay);
			pfree(oldpay);

			/* `e` points into the page, and is not valid after this. */
			lion_dir_place(index, heaprel, state->ix, buf, off, true,
						   newentry, newsize);
			pfree(newentry);
			return true;
		}
	}

	/* A CHAIN entry: its key, then the row. */
	newentry = lion_make_entry(state, key, e->flags, NULL, 0, &newsize);
	lion_entry_set_head(newentry, lion_entry_head(e));
	lion_entry_set_tail(newentry, lion_entry_tail(e));
	lion_entry_set_ncontainers(newentry, lion_entry_ncontainers(e));
	lion_entry_set_ntids(newentry, lion_entry_ntids(e));
	lion_dir_place(index, heaprel, state->ix, buf, off, true, newentry,
				   newsize);
	pfree(newentry);

	/*
	 * A column has one SUMLAST entry, so one of that kind and column at off
	 * is this one.  Anywhere else, the rewrite split the leaf under it.
	 */
	page = BufferGetPage(buf);
	if (off < lion_page_first_data(page) || off > PageGetMaxOffsetNumber(page))
		return false;
	e = lion_page_entry(page, off);
	if (LionEntryIsPivot(e) || !LionEntryIsSumLast(e) ||
		e->attno != state->attno || (e->flags & LION_ENTRY_CHAIN) == 0)
		return false;
	lion_insert_chain(index, heaprel, buf, off, ckey, lo, NULL);
	return true;
}

/*
 * The column's open bucket at (buf, off), held EXCLUSIVE, is full and `key`
 * is above every key it holds: close it where it stands and open the next one
 * with the caller's row.  Closing is dropping LION_ENTRY_SUMLAST - the entry's
 * key, the largest of its bucket, becomes the bucket's upper bound, and a
 * summary with that key sorts exactly where the open one did - in a record of
 * its own; the new SUMLAST entry is then added right after it.  A crash
 * between the two leaves a column with no open bucket, which is a state an
 * insert already handles (lion_summary_insert() creates one).
 */
static void
lion_summary_close_last(Relation index, Relation heaprel, LionState *state,
						Buffer buf, OffsetNumber off, Datum key, uint32 hash,
						uint32 ckey, uint16 lo)
{
	Page		page = BufferGetPage(buf);
	ItemId		iid = PageGetItemId(page, off);
	LionEntryTuple *onpage;
	LionEntryTuple *closed;
	LionEntryTuple *last;
	Size		size;
	Buffer		lbuf = buf;

	onpage = lion_page_entry_fetch(index, page, BufferGetBlockNumber(buf), off);
	size = ItemIdGetLength(iid);
	closed = (LionEntryTuple *) palloc(size);
	memcpy(closed, onpage, size);
	closed->flags &= ~LION_ENTRY_SUMLAST;
	lion_dir_place(index, heaprel, state->ix, buf, off, true, closed, size);
	pfree(closed);

	/* An entry of the same size stays where it was: add after it. */
	Assert(LionEntryIsSummary(lion_page_entry(BufferGetPage(buf), off)) &&
		   !LionEntryIsSumLast(lion_page_entry(BufferGetPage(buf), off)));
	last = lion_summary_make_last(state, key, hash, ckey, lo, &size);
	lion_dir_add_entry(index, heaprel, state->ix, &lbuf,
					   OffsetNumberNext(off), false, last, size);
	pfree(last);
}

/*
 * SUMMARY POSTING SETS (DESIGN.md §32): add (ckey, lo), a row whose key in a
 * summarized column is `key`, to the summary of the bucket the key belongs
 * to.  The caller has put the row under the key's own entry first and holds
 * nothing.
 *
 * The buckets of a column are (previous summary's key, this summary's key], in
 * key order, and the last one is open above: its SUMLAST entry sorts after
 * every other summary whatever its key, and its key is the largest key the
 * bucket holds so far.  So the bucket of `key` is the first summary of the
 * column whose key is at or above it, which a descent to (SUMMARY, key) lands
 * on - the column's SUMLAST entry when every other summary's key is below it.
 * lion_dir_search_first() compares what it lands on with the key, and steps
 * over a summary whose key is below it, which only an index damaged by an
 * earlier version has in that place (see there).  Then:
 *
 *	- a summary whose key is at or above `key`: the row joins it;
 *	- the SUMLAST entry, with a key at or above `key`: the same;
 *	- the SUMLAST entry with a smaller key, while it holds fewer than
 *	  summary_tids TIDs: its key is raised to `key` and the row joins it, in
 *	  one step under the lock the lookup took (lion_summary_rekey_insert());
 *	- the SUMLAST entry with a smaller key once it is FULL: the bucket is
 *	  closed where it stands - the entry loses LION_ENTRY_SUMLAST and becomes
 *	  an ordinary summary whose key, the largest it holds, is its upper bound -
 *	  and a new SUMLAST entry holding the row alone is added right after it.
 *	  No TID moves: every key the old bucket holds is at or below its key,
 *	  which is what a key above all of them is the moment to say;
 *	- no summary at all at or above `key`: a SUMLAST entry holding the row is
 *	  added where the column's summaries end.
 *
 * The last case is an index built on an empty table, whose first insert opens
 * the first bucket, or a crash between the two records of a close.  It is no
 * longer VACUUM: an emptied SUMLAST entry stays (lion_vacuum.c), because a
 * new one opened after it was deleted was keyed by the row that opened it -
 * possibly below the key of a pivot that the deleted buckets' leaves left in
 * the directory - and once that bucket closed it sorted below the separator
 * that routes to it.  After such a crash the new entry is above every summary
 * the column has (the lookup found none at or above the row's key), and the
 * item in front of the place it goes is compared with the key as well, when
 * it is on the same leaf, to be sure.  (What is left is that closed bucket
 * emptied and deleted by VACUUM before any row above it arrives, and then a
 * row below its key: DESIGN.md §32, "Open for the owner".)
 *
 * Every one of those changes the summary run under the EXCLUSIVE lock of the
 * leaf the summary lives on, which every writer of that summary holds - the
 * serialisation every entry has (§5) - so no two inserts can race to raise,
 * close or create one.  The row's key entry and its summary are two steps, not
 * one: an ERROR or a crash between them leaves a TID in the key's set and not
 * in the summary, and it is the TID of a row whose transaction never
 * committed, which no count can see and the next VACUUM removes from the key's
 * set.  Every row a snapshot CAN see went through both steps before its
 * transaction committed.
 */
static void
lion_summary_insert(Relation index, Relation heaprel, LionState *state,
					Datum key, uint32 ckey, uint16 lo)
{
	uint32		hash = LION_SUMMARY_HASH;	/* whatever the key: see lion.h */
	uint32		bucket_tids = state->ix->meta.summary_tids;
	bool		guarded = false;

	/*
	 * Each turn either places the row or changes the summary run so that the
	 * next one gets further: an open bucket made by this insert or by another,
	 * or an entry the rewrite of its key moved off its leaf.
	 */
	for (;;)
	{
		LionIndexState *ix = state->ix;
		LionSearchKey sk;
		Buffer		buf;
		Page		page;
		OffsetNumber off;
		LionEntryTuple *e = NULL;
		int			kind = -1;

		CHECK_FOR_INTERRUPTS();

		lion_search_key_init(state, &sk, LION_KIND_SUMMARY, key, 0);
		buf = lion_dir_search_first(index, heaprel, ix, &sk,
									BUFFER_LOCK_EXCLUSIVE, true, &off);
		page = BufferGetPage(buf);
		if (off <= PageGetMaxOffsetNumber(page))
		{
			e = lion_page_entry(page, off);
			if (e->attno == state->attno && LionEntryIsSummary(e) &&
				!LionEntryIsPivot(e))
				kind = lion_entry_kind(e);
		}

		/*
		 * SERIALIZABLE (DESIGN.md §9): the summary the row goes into, and the
		 * leaf as well when its key or the run of summaries changes - or a
		 * new open bucket is made - below.
		 */
		if (kind >= 0)
			lion_writer_check(index, buf, off, true);

		if (kind == LION_KIND_SUMMARY)
		{
			/* lion_dir_search_first() found its key at or above ours. */
			if ((e->flags & LION_ENTRY_INLINE) != 0)
				lion_insert_inline(index, heaprel, state, buf, off, ckey, lo, NULL);
			else
				lion_insert_chain(index, heaprel, buf, off, ckey, lo, NULL);
			UnlockReleaseBuffer(buf);
			return;
		}

		if (kind == LION_KIND_SUMLAST)
		{
			Datum		lastkey = lion_entry_key(state, e);
			int32		c = DatumGetInt32(FunctionCall2Coll(&state->cmpproc,
															state->collation,
															key, lastkey));
			bool		done = true;

			if (c <= 0)
			{
				if ((e->flags & LION_ENTRY_INLINE) != 0)
					lion_insert_inline(index, heaprel, state, buf, off, ckey,
									  lo, NULL);
				else
					lion_insert_chain(index, heaprel, buf, off, ckey, lo, NULL);
			}
			else
			{
				lion_writer_check(index, buf, off, false);
				if (lion_entry_ntids(e) >= (uint64) bucket_tids)
					lion_summary_close_last(index, heaprel, state, buf, off,
											key, hash, ckey, lo);
				else
					done = lion_summary_rekey_insert(index, heaprel, state,
													 buf, off, key, hash,
													 ckey, lo);
			}
			UnlockReleaseBuffer(buf);
			if (done)
				return;
			continue;
		}

		/*
		 * No summary of the column is at or above the key, and it has no open
		 * bucket either: an index built on an empty table, or a crash between
		 * the two records of a close (see above).  The new SUMLAST entry goes
		 * where the column's summaries end, through the find-or-create every
		 * new entry takes (lion_dir_find()), which serialises it against
		 * another insert doing the same.
		 */
		UnlockReleaseBuffer(buf);
		{
			bool		movedright;
			bool		found;
			LionEntryTuple *last;
			Size		size;

			lion_search_key_init(state, &sk, LION_KIND_SUMLAST, (Datum) 0, 0);
			found = lion_dir_find(index, heaprel, ix, &sk,
								  BUFFER_LOCK_EXCLUSIVE, true, &buf, &off,
								  &movedright);
			if (found)
			{
				/* Another insert made one meanwhile: use it. */
				UnlockReleaseBuffer(buf);
				continue;
			}

			/*
			 * The summary in front of the new entry, when it is on this leaf,
			 * must be below the key: the new bucket is above every other.
			 * One that is not came after the lookup - which nothing but a
			 * close can make, and a close makes its SUMLAST under the same
			 * lock - or was missed by it, which only damage explains.  Look
			 * again once, and then say so.
			 */
			page = BufferGetPage(buf);
			if (OffsetNumberIsValid(off) &&
				off > lion_page_first_data(page) &&
				off <= OffsetNumberNext(PageGetMaxOffsetNumber(page)))
			{
				LionEntryTuple *prev = lion_page_entry(page,
													   OffsetNumberPrev(off));

				if (!LionEntryIsPivot(prev) && prev->attno == state->attno &&
					lion_entry_kind(prev) == LION_KIND_SUMMARY &&
					DatumGetInt32(FunctionCall2Coll(&state->cmpproc,
													state->collation,
													lion_entry_key(state, prev),
													key)) >= 0)
				{
					UnlockReleaseBuffer(buf);
					if (guarded)
						ereport(ERROR,
								(errcode(ERRCODE_INDEX_CORRUPTED),
								 errmsg("lion index \"%s\": the summaries of key column %d are out of order",
										RelationGetRelationName(index),
										state->attno),
								 errhint("REINDEX the index.")));
					guarded = true;
					continue;
				}
			}

			lion_writer_check(index, buf, off, false);
			last = lion_summary_make_last(state, key, hash, ckey, lo, &size);
			lion_dir_add_entry(index, heaprel, ix, &buf, off, movedright,
							   last, size);
			pfree(last);
			UnlockReleaseBuffer(buf);
			return;
		}
	}
}

/*
 * The rows rows[0 .. n) of one key of a batch (lion_insert_apply()) into the
 * INLINE entry at (entrybuf, entryoff), held EXCLUSIVE: as many as the
 * payload holds under the limit, in one rewrite of the entry and one record.
 * Returns how many it took - a TID the set holds already counts as taken -
 * or 0, having written nothing, when the first row would take the payload
 * past the limit (it spills, which lion_insert_inline() does) or, with
 * nosplit, when the rewritten entry does not fit where it stands
 * (lion_insert_inline_ext()).  No entry with positions comes here.
 */
static int
lion_insert_inline_rows(Relation index, Relation heaprel, LionState *state,
						Buffer entrybuf, OffsetNumber entryoff,
						const LionInsertRow *rows, int n, bool nosplit)
{
	Page		page = BufferGetPage(entrybuf);
	BlockNumber blk = BufferGetBlockNumber(entrybuf);
	LionEntryTuple *entry = lion_page_entry_fetch(index, page, blk, entryoff);
	Size		itemsz = ItemIdGetLength(PageGetItemId(page, entryoff));
	Size		payoff = LionEntryPayloadOffset(entry);
	Size		paylen = LION_ENTRY_PAYLOAD_LEN(entry, itemsz);
	Size		limit = lion_inline_max(payoff,
										(Size) state->ix->meta.inline_limit);
	Size		bufsz = Max(paylen, limit) + LION_CONTAINER_MAX_SIZE;
	LionEntryTuple *newentry;
	Size		newsize;
	char	   *cur;
	char	   *next;
	int			ndelta = 0;
	int			taken;
	int			added = 0;

	Assert(lion_entry_posext(entry) == NULL);

	cur = (char *) palloc(bufsz);
	next = (char *) palloc(bufsz);
	memcpy(cur, LionEntryGetPayload(entry), paylen);
	for (taken = 0; taken < n; taken++)
	{
		int			nd;
		Size		len;
		char	   *swap;

		len = lion_inline_add(index, blk, entryoff, cur, paylen,
							  lion_code_ckey(rows[taken].code),
							  lion_code_lo(rows[taken].code), next, &nd);
		if (len == 0)
			continue;			/* already indexed */
		if (len > limit)
			break;
		swap = cur;
		cur = next;
		next = swap;
		paylen = len;
		ndelta += nd;
		added++;
	}
	pfree(next);

	if (added == 0)
	{
		pfree(cur);
		return taken;			/* every row taken was there already, or 0 */
	}

	newentry = lion_inline_rebuilt(state, page, entry, itemsz, cur, paylen,
								   ndelta, (uint64) added, &newsize);
	pfree(cur);
	if (nosplit && !lion_page_overwrite_fits(page, entryoff, newsize))
	{
		pfree(newentry);
		return 0;
	}

	/* as lion_insert_inline_ext(): a leaf that has no room splits */
	lion_dir_place(index, heaprel, state->ix, entrybuf, entryoff, true,
				   newentry, newsize);
	pfree(newentry);
	return taken;
}

/*
 * lion_insert_inline_rows() for an INLINE entry that stores positions
 * (DESIGN.md §17): the rows rows[0 .. n) of one key, as many as the payload
 * and the inline chunk hold together under the limit, in one rewrite of the
 * entry and one record.  The chunk is read and checked once, and a run of
 * rows above its last member is appended to it with the place its members
 * end kept (lion_poschunk_append_tail()) rather than merged in row by row;
 * a row below goes in by lion_poschunk_insert().  pm is a member to build
 * each row's in.  Returns how many it took, or 0, having written nothing,
 * when the first row does not fit - it spills, which lion_insert_inline()
 * does.
 */
static int
lion_insert_inline_pos_rows(Relation index, Relation heaprel, LionState *state,
							Buffer entrybuf, OffsetNumber entryoff,
							const LionInsertRow *rows, int n,
							LionPosMember *pm)
{
	Page		page = BufferGetPage(entrybuf);
	BlockNumber blk = BufferGetBlockNumber(entrybuf);
	LionEntryTuple *entry = lion_page_entry_fetch(index, page, blk, entryoff);
	Size		itemsz = ItemIdGetLength(PageGetItemId(page, entryoff));
	Size		payoff = LionEntryPayloadOffset(entry);
	Size		paylen = LION_ENTRY_PAYLOAD_LEN(entry, itemsz);
	Size		area = paylen;	/* the payload area the entry has */
	Size		limit = lion_inline_max(payoff,
										(Size) state->ix->meta.inline_limit);
	Size		bufsz = Max(paylen, limit) + LION_CONTAINER_MAX_SIZE;
	LionContainer *chunk = (LionContainer *) palloc0(LION_CONTAINER_MAX_SIZE);
	LionContainer *nextchunk = (LionContainer *) palloc0(LION_CONTAINER_MAX_SIZE);
	LionPosTail tail;
	char	   *cur;
	char	   *next;
	Size		poslen;
	int			ndelta = 0;
	int			taken;
	int			added = 0;

	if (!lion_entry_inline_poschunk(index, entry, itemsz, blk, entryoff, chunk))
		lion_poschunk_init(chunk, lion_pos_block(rows[0].code));
	poslen = lion_poschunk_size(chunk);
	tail.valid = false;

	cur = (char *) palloc(bufsz);
	next = (char *) palloc(bufsz);
	memcpy(cur, LionEntryGetPayload(entry), paylen);
	for (taken = 0; taken < n; taken++)
	{
		LionPosTail ntail = tail;
		int			nd;
		Size		len;
		Size		nposlen;
		bool		ok;
		void	   *swap;

		len = lion_inline_add(index, blk, entryoff, cur, paylen,
							  lion_code_ckey(rows[taken].code),
							  lion_code_lo(rows[taken].code), next, &nd);
		if (len == 0)
			continue;			/* already indexed, positions and all */

		lion_insert_row_member(state, &rows[taken], pm);
		memcpy(nextchunk, chunk, poslen);
		ok = lion_poschunk_append_tail(nextchunk, LION_CONTAINER_MAX_SIZE, pm,
									   &ntail);
		if (!ok)
		{
			/* below the last member, or no room: merged in, as for one row */
			ok = lion_poschunk_insert(nextchunk, LION_CONTAINER_MAX_SIZE, pm,
									  NULL);
			ntail.valid = false;
		}
		if (!ok)
			break;
		nposlen = lion_poschunk_size(nextchunk);
		if ((MAXALIGN(payoff + len) - payoff) + nposlen > limit)
			break;

		swap = cur;
		cur = next;
		next = swap;
		swap = chunk;
		chunk = nextchunk;
		nextchunk = swap;
		paylen = len;
		poslen = nposlen;
		tail = ntail;
		ndelta += nd;
		added++;
	}
	pfree(next);
	pfree(nextchunk);

	if (added > 0)
		lion_inline_pos_place(index, heaprel, state, entrybuf, entryoff, entry,
							  itemsz, area, cur, paylen, ndelta, (uint64) added,
							  chunk, poslen);
	pfree(cur);
	pfree(chunk);
	return taken;				/* the rows written, and any the set held */
}

/* ---------------------------------------------------------------------
 * Leaf hints (DESIGN.md §21, "A leaf already known")
 * --------------------------------------------------------------------- */

static LionLeafHint *
lion_leaf_hints(LionIndexState *ix)
{
	if (ix->leafhint == NULL)
	{
		LionLeafHint *h = (LionLeafHint *)
			MemoryContextAlloc(GetMemoryChunkContext(ix->cols),
							   sizeof(LionLeafHint) * LION_LEAF_HINTS);
		int			i;

		for (i = 0; i < LION_LEAF_HINTS; i++)
		{
			h[i].blk = InvalidBlockNumber;
			h[i].hash = 0;
			h[i].attno = InvalidAttrNumber;
			h[i].kind = 0;
		}
		ix->leafhint = h;
	}
	return ix->leafhint;
}

/* The leaf this backend last found sk's entry on, or InvalidBlockNumber */
static inline BlockNumber
lion_leaf_hint_get(LionIndexState *ix, const LionSearchKey *sk)
{
	LionLeafHint *h = &lion_leaf_hints(ix)[lion_leaf_hint_slot(sk->attno,
																sk->hash)];

	if (h->attno == sk->attno && h->hash == sk->hash && h->kind == sk->kind)
		return h->blk;
	return InvalidBlockNumber;
}

/* ... and remember blk as that leaf */
static inline void
lion_leaf_hint_set(LionIndexState *ix, const LionSearchKey *sk,
				   BlockNumber blk)
{
	LionLeafHint *h = &lion_leaf_hints(ix)[lion_leaf_hint_slot(sk->attno,
																sk->hash)];

	h->blk = blk;
	h->hash = sk->hash;
	h->attno = sk->attno;
	h->kind = (int16) sk->kind;
}

/*
 * Put the row (ckey, lo) into the entry at (leafbuf, entryoff), a leaf the
 * writer took without a descent (lion_dir_find_on_leaf()) and holds
 * EXCLUSIVE: true when it is in, false - with nothing written - when the
 * insert could split the leaf, which only a writer that came through a
 * descent may do.  A CHAIN entry's insert rewrites the entry where it stands
 * at its size; an INLINE entry's does when the leaf has the room
 * (lion_insert_inline_ext()), or when it spills; an INLINE entry with
 * positions is left to the descent.
 */
static bool
lion_insert_known(Relation index, Relation heaprel, LionState *state,
				  Buffer leafbuf, OffsetNumber entryoff, uint32 ckey, uint16 lo,
				  const LionPosMember *pm)
{
	LionEntryTuple *entry = lion_page_entry(BufferGetPage(leafbuf), entryoff);

	if ((entry->flags & LION_ENTRY_INLINE) != 0)
	{
		if (pm != NULL)
			return false;
		lion_writer_check(index, leafbuf, entryoff, true);
		return lion_insert_inline_ext(index, heaprel, state, leafbuf, entryoff,
									  ckey, lo, NULL, true);
	}
	lion_writer_check(index, leafbuf, entryoff, true);
	lion_insert_chain(index, heaprel, leafbuf, entryoff, ckey, lo, pm);
	return true;
}

/*
 * Add one (key, code) pair to the index: locate or create the key's entry and
 * put the member in it.  reservedflag is 0 for a real key, or the flag of the
 * reserved entry the pair belongs to (DESIGN.md §14 and §17), in which case
 * key is meaningless and the bucket is 0.
 */
static void
lion_insert_one(Relation index, Relation heaprel, LionState *state, Datum key,
			   uint16 reservedflag, uint32 ckey, uint16 lo,
			   const LionPosMember *pm)
{
	uint32		hash;
	LionSearchKey sk;
	Buffer		leafbuf;
	OffsetNumber entryoff;
	bool		movedright;
	bool		found;
	BlockNumber hint;

	hash = (reservedflag != 0) ? LION_NULLKEY_HASH : lion_hash_key(state, key);

	if (reservedflag != 0)
		lion_search_key_init(state, &sk, lion_reserved_kind(reservedflag),
							 (Datum) 0, hash);
	else
		lion_search_key_init(state, &sk, LION_KIND_VALUE, key, hash);

	/*
	 * The leaf this backend last wrote the key on, when it still holds the
	 * entry and the insert leaves it unsplit: no descent at all (DESIGN.md
	 * §21, "A leaf already known").
	 */
	hint = lion_leaf_hint_get(state->ix, &sk);
	if (BlockNumberIsValid(hint) &&
		lion_dir_find_on_leaf(index, &sk, hint, &leafbuf, &entryoff))
	{
		bool		done = lion_insert_known(index, heaprel, state, leafbuf,
											 entryoff, ckey, lo, pm);

		UnlockReleaseBuffer(leafbuf);
		if (done)
			return;
	}

	/*
	 * One descent, with the leaf taken EXCLUSIVE, and it is held for the whole
	 * insert: every writer of a key (inserts and VACUUM) serialises on the
	 * leaf that holds its entry (DESIGN.md §5, §21).
	 */
	found = lion_dir_find(index, heaprel, state->ix, &sk, BUFFER_LOCK_EXCLUSIVE,
						  true, &leafbuf, &entryoff, &movedright);

	/* SERIALIZABLE: the page that stands for the key (DESIGN.md §9) */
	lion_writer_check(index, leafbuf, entryoff, found);

	if (!found)
		lion_insert_new_entry(index, heaprel, state, &leafbuf, entryoff,
							 movedright, key, reservedflag, hash, ckey, lo, pm);
	else
	{
		/*
		 * The flags only pick the path: each of the two takes the entry
		 * through lion_page_entry_fetch() before it reads or writes a byte
		 * of it, which refuses one that is not exactly one of INLINE and
		 * CHAIN.
		 */
		LionEntryTuple *entry = lion_page_entry(BufferGetPage(leafbuf),
												entryoff);

		if ((entry->flags & LION_ENTRY_INLINE) != 0)
			lion_insert_inline(index, heaprel, state, leafbuf, entryoff, ckey,
							  lo, pm);
		else
			lion_insert_chain(index, heaprel, leafbuf, entryoff, ckey, lo, pm);
	}

	/*
	 * The leaf held now: the entry's, or one to its left when a split moved
	 * the entry on - a guess either way, which the next insert checks.
	 */
	lion_leaf_hint_set(state->ix, &sk, BufferGetBlockNumber(leafbuf));
	UnlockReleaseBuffer(leafbuf);
}

/* ---------------------------------------------------------------------
 * Bit slices (DESIGN.md §51)
 * --------------------------------------------------------------------- */

/*
 * Put the row at (ckey, lo) into the SLICE entry of column state at pos,
 * which a find-or-create on the leaf *leafbuf, held EXCLUSIVE, found at
 * entryoff (found) or says belongs there: a new INLINE entry, or the member
 * added by the same INLINE and CHAIN paths a key's entry takes, the in-place
 * fast paths for a BITSET or a slack item and their records included - a
 * slice of a dense column is bitsets, so most of its inserts are a bit set
 * in place.  *leafbuf stays held, though a split of it may have moved the
 * entries above its cut to a new right sibling.  The caller has made the
 * writer's SERIALIZABLE check (lion_writer_check()).
 */
static void
lion_slice_put(Relation index, Relation heaprel, LionState *state,
			   Buffer *leafbuf, OffsetNumber entryoff, bool movedright,
			   bool found, int pos, uint32 ckey, uint16 lo)
{
	if (!found)
	{
		char		payload[LION_SPARSE_SINGLETON_MAX];
		Size		paylen = lion_put_singleton(payload, ckey, lo);
		LionEntryTuple *entry;
		Size		size;

		entry = lion_make_slice_entry((AttrNumber) state->attno, pos,
									  LION_ENTRY_INLINE, payload, paylen,
									  &size);
		lion_entry_set_ncontainers(entry, 1);
		lion_entry_set_ntids(entry, 1);
		lion_dir_add_entry(index, heaprel, state->ix, leafbuf, entryoff,
						   movedright, entry, size);
		pfree(entry);
	}
	else if ((lion_page_entry(BufferGetPage(*leafbuf), entryoff)->flags &
			  LION_ENTRY_INLINE) != 0)
		lion_insert_inline(index, heaprel, state, *leafbuf, entryoff, ckey, lo,
						   NULL);
	else
		lion_insert_chain(index, heaprel, *leafbuf, entryoff, ckey, lo, NULL);
}

/*
 * Add one row to the SLICE entry of column state at pos: the entry located
 * or created by the find-or-create every new entry takes (lion_dir_find(),
 * which with the position in the prefix finds exactly that slice), and the
 * member put in by lion_slice_put().  What lion_debug_slice_insert() damages
 * a slice with; a row's inserts go through lion_slice_insert().
 */
void
lion_slice_insert_one(Relation index, Relation heaprel, LionState *state,
					  int pos, uint32 ckey, uint16 lo)
{
	LionSearchKey sk;
	uint8		posbyte;
	Buffer		leafbuf;
	OffsetNumber entryoff;
	bool		movedright;
	bool		found;

	lion_search_key_slice(state, &sk, pos, &posbyte);
	found = lion_dir_find(index, heaprel, state->ix, &sk, BUFFER_LOCK_EXCLUSIVE,
						  true, &leafbuf, &entryoff, &movedright);

	/* SERIALIZABLE: the page that stands for the slice (DESIGN.md §9) */
	lion_writer_check(index, leafbuf, entryoff, found);

	lion_slice_put(index, heaprel, state, &leafbuf, entryoff, movedright,
				   found, pos, ckey, lo);
	UnlockReleaseBuffer(leafbuf);
}

/*
 * SEVERAL SLICES TO A RECORD: a row's in-place adds into the slices of a
 * column that are on one directory leaf (DESIGN.md §51, "Insert cost,
 * revisited").
 *
 * A dense slice is a posting tree, and a row appended to the table lands in
 * the last container of each of its slices' trees, on the tree's tail, where
 * the hot-key insert sets the bit in place: a record of the container page
 * and the entry's leaf (lion_insert_container_inplace()).  The leaf is the
 * same for every slice of the run, so a row of an int8 column wrote it into
 * 33 records on average.  Here the adds are gathered first - each slice's
 * tail locked, its item found and checked for room, everything fallible done
 * - and then made in one ITEM_SET record of the posting leaves and the leaf:
 * up to three posting leaves in generic mode, GenericXLog's four buffers in
 * all, and up to seven with the resource manager, which takes a wider record
 * when asked (lion_wal_begin_ext(), LION_WAL_MAX_BLOCKS_WIDE).  On the leaf
 * each CHAIN entry's counter is raised in place and logged as the bytes that
 * changed (lion_slice_count_one()), and an INLINE slice whose grown payload
 * fits the bytes its entry has joins the record as a rewrite of the entry at
 * its length (lion_slice_batch_inline()).  The record is ITEM_SET as before
 * - ENTRY when every slice in it was INLINE - with more blocks and more
 * operations per block, which its redo has always taken (lion_redo_apply()
 * runs any stream on any block), so the WAL format is unchanged.
 *
 * LOCKS.  The leaf is held throughout, so no other writer of these slices
 * runs, and each tail is locked after it, as a single insert does.  The one
 * thing new is holding several posting leaves, of different sets, at once;
 * so a tail after the first is only ever TRIED (ConditionalLockBuffer()), and
 * when it is busy the batch is written and let go, and the slice starts a new
 * one, waiting for its tail with nothing but the leaf held.  So the batch
 * waits for exactly what a single insert waits for, in the same order, and
 * adds no edge to any wait-for graph on the primary.  On a standby, redo
 * takes the record's posting leaves in order and then the leaf (DESIGN.md
 * §25), holding each to the end of the record; no reader holds a content
 * lock on a posting page while it waits for another (a cursor copies the
 * page and lets the lock go, a descent lets the parent go before it locks
 * the child), so the several leaves close no cycle there either.
 *
 * What is not batched goes the ordinary way, with the batch written first:
 * a slice that needs the general path (an item without room, a new segment,
 * a member past a NARROW's width), a tail that is not the set's rightmost
 * leaf or does not own the ckey (lion_insert_lock_chain_page()'s hint), an
 * INLINE slice that has to grow or spill, and a new one - each of which may
 * split the leaf, which would move the batch's entries - and the batch is
 * also written before the walk leaves its leaf.
 */
typedef struct LionSliceAdd
{
	Buffer		buf;			/* the slice's tail, held EXCLUSIVE; or
								 * InvalidBuffer, an INLINE slice */
	OffsetNumber off;			/* the item that takes the member */
	bool		segment;		/* a sparse segment, else a container */
	LionSparseProbe probe;		/* a segment: its probe for the pair, taken
								 * under the lock buf stays in */
	OffsetNumber entryoff;		/* the slice's entry on the batch's leaf */
	LionEntryTuple *ecopy;		/* an INLINE slice: the entry as it is to be,
								 * its payload grown and its count raised;
								 * NULL for a CHAIN one, whose count is
								 * raised in place */
	Size		esize;			/* its length, which is the old one's */
} LionSliceAdd;

typedef struct LionSliceBatch
{
	uint32		ckey;			/* the row, the same in every slice */
	uint16		lo;
	int			n;				/* adds */
	int			nbufs;			/* of them, the ones with a posting leaf */
	int			maxbufs;		/* posting leaves a record takes beside the
								 * leaf: lion_wal_max_blocks() - 1 */
	bool		checked;		/* the leaf's items checked since it last
								 * changed but by the batch (see
								 * lion_slice_batch_check_leaf()) */
	LionSliceAdd adds[LION_SLICE_MAX_POS];
} LionSliceBatch;

/*
 * Each entry the batch rewrites is overwritten in place, at its length,
 * inside the record, so it has to be alone in its bytes, which a single
 * insert checks of the one entry by a pass over the leaf's line pointers
 * (lion_page_check_alone()).  The walk holds the leaf, and the batch's
 * records change it by nothing but such rewrites, which keep every item
 * where it is; so the batch checks every item of the leaf once instead, in
 * one pass over the page (lion_page_check_items()), until the walk moves to
 * another leaf or changes this one the ordinary way (b->checked).  For a
 * column of 17 or 33 slices on a leaf of a few hundred entries that is a
 * quarter to an eighth of the work - a pass that marks the few 8-byte units
 * of each item, against 17 or 33 passes over every line pointer - which the
 * profile of the batched inserts had put at some 13% of their time.
 */
static void
lion_slice_batch_check_leaf(Relation index, LionSliceBatch *b, Buffer leafbuf)
{
	if (b->checked)
		return;
	lion_page_check_items(index, BufferGetPage(leafbuf),
						  BufferGetBlockNumber(leafbuf));
	b->checked = true;
}

/* What lion_slice_batch_add() did with a slice. */
#define LION_SLICE_TAKEN	0	/* in the batch, or there already */
#define LION_SLICE_ORDINARY	1	/* it needs lion_slice_put() */
#define LION_SLICE_BUSY		2	/* its tail is locked: write the batch first */

/*
 * Take the batch's row into the CHAIN slice entry at (leafbuf, entryoff), if
 * it goes in in place on its set's tail.  leafbuf is held EXCLUSIVE.  On
 * LION_SLICE_ORDINARY and LION_SLICE_BUSY nothing of the attempt is held; the
 * tail is waited for only when the batch is empty.
 */
static int
lion_slice_batch_add(Relation index, LionSliceBatch *b, Buffer leafbuf,
					 OffsetNumber entryoff)
{
	Page		page = BufferGetPage(leafbuf);
	BlockNumber leafblk = BufferGetBlockNumber(leafbuf);
	uint32		ckey = b->ckey;
	uint16		lo = b->lo;
	LionEntryTuple *entry;
	LionSliceAdd *a;
	BlockNumber head;
	BlockNumber tail;
	Buffer		buf;
	Page		cpage;
	OffsetNumber off;
	LionSparseProbe probe;
	int			target;
	int			verdict;
	int			i;

	Assert(b->nbufs < b->maxbufs && b->n < LION_SLICE_MAX_POS);

	entry = lion_page_entry_fetch(index, page, leafblk, entryoff);
	Assert((entry->flags & LION_ENTRY_CHAIN) != 0);
	head = lion_entry_head(entry);
	tail = lion_entry_tail(entry);

	/*
	 * Links that cannot be a page of the set, and a tail another slice of
	 * the batch claims, are lion_insert_lock_chain_page()'s ERROR to raise,
	 * on the ordinary path.
	 */
	if (!BlockNumberIsValid(head) || !BlockNumberIsValid(tail) ||
		head == leafblk || tail == leafblk)
		return LION_SLICE_ORDINARY;
	for (i = 0; i < b->n; i++)
		if (BufferIsValid(b->adds[i].buf) &&
			BufferGetBlockNumber(b->adds[i].buf) == tail)
			return LION_SLICE_ORDINARY;

	buf = ReadBuffer(index, tail);
	if (b->nbufs == 0)
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	else if (!ConditionalLockBuffer(buf))
	{
		ReleaseBuffer(buf);
		return LION_SLICE_BUSY;
	}
	cpage = BufferGetPage(buf);

	/* lion_insert_lock_chain_page()'s hint, and its owner check */
	if (!lion_page_owns_entry(cpage, lion_entry_owner(entry), head) ||
		!LionPageIsPostingLeaf(cpage) || !LionPageIsRightmost(cpage) ||
		LionPageIncompleteSplit(cpage) ||
		PageGetMaxOffsetNumber(cpage) < FirstOffsetNumber ||
		ckey < LionPageGetOpaque(cpage)->minckey)
	{
		UnlockReleaseBuffer(buf);
		return LION_SLICE_ORDINARY;
	}

	target = lion_chain_target(index, buf, ckey, lo, &off, &probe);
	if (target == LION_TARGET_CONTAINER)
		verdict = lion_container_inplace_check(index, buf, off, lo);
	else if (target == LION_TARGET_SEGMENT)
		verdict = lion_segment_inplace_check(index, buf, off, &probe);
	else
		verdict = LION_INPLACE_NO;

	if (verdict != LION_INPLACE_YES)
	{
		UnlockReleaseBuffer(buf);
		return verdict == LION_INPLACE_DUP ? LION_SLICE_TAKEN :
			LION_SLICE_ORDINARY;
	}

	/* The entry is rewritten in place at its size, inside the record. */
	lion_slice_batch_check_leaf(index, b, leafbuf);

	/* its counter is raised in place (lion_slice_count_one()) */
	a = &b->adds[b->n++];
	b->nbufs++;
	a->buf = buf;
	a->off = off;
	a->segment = (target == LION_TARGET_SEGMENT);
	if (a->segment)
		a->probe = probe;
	a->entryoff = entryoff;
	a->ecopy = NULL;
	a->esize = 0;

	return LION_SLICE_TAKEN;
}

/*
 * The same for the INLINE slice entry at (leafbuf, entryoff): taken when the
 * entry holds the grown payload within the bytes it has - its growth slack
 * (lion_inline_rebuilt()) - so that its overwrite moves nothing on the leaf
 * and cannot fail, and can share a record.  The new entry is the one
 * lion_insert_inline() would write, built the same way.  An entry that has
 * to grow on the leaf - which may split it - or to spill is left to the
 * ordinary path, which computes the same thing again: the batch's other
 * entries keep their lengths, so the leaf's free space it reads is the same.
 * Takes no buffer.
 */
static int
lion_slice_batch_inline(Relation index, LionState *state, LionSliceBatch *b,
						Buffer leafbuf, OffsetNumber entryoff)
{
	Page		page = BufferGetPage(leafbuf);
	BlockNumber leafblk = BufferGetBlockNumber(leafbuf);
	LionEntryTuple *entry;
	LionEntryTuple *newentry;
	LionSliceAdd *a;
	Size		itemsz;
	Size		paylen;
	Size		newlen;
	Size		newsize;
	char	   *newpay;
	int			ndelta;

	Assert(b->n < LION_SLICE_MAX_POS);

	entry = lion_page_entry_fetch(index, page, leafblk, entryoff);
	Assert((entry->flags & LION_ENTRY_INLINE) != 0 &&
		   lion_entry_posext(entry) == NULL);
	itemsz = ItemIdGetLength(PageGetItemId(page, entryoff));
	paylen = LION_ENTRY_PAYLOAD_LEN(entry, itemsz);

	newpay = (char *) palloc(paylen + LION_CONTAINER_MAX_SIZE);
	newlen = lion_inline_add(index, leafblk, entryoff,
							 LionEntryGetPayload(entry), paylen, b->ckey,
							 b->lo, newpay, &ndelta);
	if (newlen == 0)
	{
		pfree(newpay);
		return LION_SLICE_TAKEN;	/* already indexed */
	}
	if (newlen > lion_inline_max(LionEntryPayloadOffset(entry),
								 (Size) state->ix->meta.inline_limit))
	{
		pfree(newpay);
		return LION_SLICE_ORDINARY; /* it spills */
	}

	newentry = lion_inline_rebuilt(state, page, entry, itemsz, newpay, newlen,
								   ndelta, 1, &newsize);
	pfree(newpay);
	if (newsize != itemsz)
	{
		pfree(newentry);
		return LION_SLICE_ORDINARY;
	}

	/* lion_dir_place()'s check of the item it overwrites */
	lion_slice_batch_check_leaf(index, b, leafbuf);

	a = &b->adds[b->n++];
	a->buf = InvalidBuffer;
	a->off = InvalidOffsetNumber;
	a->segment = false;
	a->entryoff = entryoff;
	a->ecopy = newentry;
	a->esize = newsize;

	return LION_SLICE_TAKEN;
}

/*
 * One more TID in the CHAIN slice entry at (leafbuf, entryoff), inside the
 * record xstate.  An in-place add changes nothing of a CHAIN entry but its
 * ntids counter, so the counter is raised where it is, and logged as the
 * bytes of it that changed - one, but on a carry - in a SETBYTES, which
 * replay copies over the same bytes of the same entry (lion_redo_apply()):
 * nine bytes of record, where the DELTA of a rewritten entry
 * (lion_replace_entry()) is some fourteen, and costs a copy of the entry and
 * a comparison besides, which the profile of the batched inserts put at an
 * eighth of their time.
 */
static void
lion_slice_count_one(LionWalState *xstate, Buffer leafbuf,
					 OffsetNumber entryoff)
{
	Page		p = lion_wal_register_buffer(xstate, leafbuf, LION_WALBUF_STD);
	LionEntryTuple *e = (LionEntryTuple *) PageGetItem(p,
													   PageGetItemId(p, entryoff));
	char	   *ctr = (char *) e + offsetof(LionEntryChainHdr, ntids);
	char		old[sizeof(uint64)];
	int			first;
	int			last;

	Assert((e->flags & LION_ENTRY_CHAIN) != 0);
	memcpy(old, ctr, sizeof(old));
	lion_entry_set_ntids(e, lion_entry_ntids(e) + 1);

	for (first = 0; first < (int) sizeof(old) - 1 && old[first] == ctr[first];
		 first++)
		;
	for (last = (int) sizeof(old) - 1; last > first && old[last] == ctr[last];
		 last--)
		;
	lion_wal_op(xstate, p, LION_OP_SETBYTES, entryoff,
				(uint16) (offsetof(LionEntryChainHdr, ntids) + first),
				ctr + first, (Size) (last - first + 1));
}

/*
 * Write the batch b in one record - the posting leaves first, each with its
 * CONTAINER_ADD or SPARSE_INS (and MINMAX), then the leaf with each CHAIN
 * entry's counter (lion_slice_count_one()) and each INLINE entry's DELTA, the
 * order redo takes them in (DESIGN.md §25): an ITEM_SET, or an ENTRY when
 * every slice of it was INLINE - and let the posting leaves go.  leafbuf
 * stays held.
 */
static void
lion_slice_batch_write(Relation index, Buffer leafbuf, LionSliceBatch *b)
{
	LionWalState *xstate;
	int			i;

	if (b->n == 0)
		return;
	xstate = lion_wal_begin_ext(index, b->nbufs + 1);
	for (i = 0; i < b->n; i++)
	{
		LionSliceAdd *a = &b->adds[i];

		if (!BufferIsValid(a->buf))
			continue;
		if (a->segment)
			lion_segment_inplace_apply(xstate, a->buf, a->off, b->ckey, b->lo,
									   &a->probe);
		else
			lion_container_inplace_apply(xstate, a->buf, a->off, b->lo);
	}

	/*
	 * Every entry keeps its length, so no overwrite moves another item or
	 * fails (lion_replace_entry() tests before it writes, and has nothing to
	 * refuse here).
	 */
	for (i = 0; i < b->n; i++)
	{
		LionSliceAdd *a = &b->adds[i];

		if (BufferIsValid(a->buf))
			lion_slice_count_one(xstate, leafbuf, a->entryoff);
		else if (!lion_replace_entry(index, xstate, leafbuf, a->entryoff,
									 a->ecopy, a->esize))
			elog(ERROR, "lion index: could not update entry %u on block %u",
				 a->entryoff, BufferGetBlockNumber(leafbuf));
	}
	lion_wal_finish(xstate, b->nbufs > 0 ? LION_XLOG_ITEM_SET :
					LION_XLOG_ENTRY);

	for (i = 0; i < b->n; i++)
	{
		if (BufferIsValid(b->adds[i].buf))
			UnlockReleaseBuffer(b->adds[i].buf);
		if (b->adds[i].ecopy != NULL)
			pfree(b->adds[i].ecopy);
	}
	b->n = 0;
	b->nbufs = 0;
}

/*
 * The slices a sliced column's row with value key goes into, in ascending
 * position: the bits its encoding has set (DESIGN.md §51), then the existence
 * slice.  Returns how many, LION_SLICE_MAX_POS at most.
 */
static int
lion_slice_positions(LionState *state, Datum key, int *pos)
{
	int			nbits = state->slicebits;
	int			npos = 0;
	int64		v;
	uint64		enc;

	switch (nbits)
	{
		case 16:
			v = DatumGetInt16(key);
			break;
		case 32:
			v = DatumGetInt32(key);
			break;
		default:
			v = DatumGetInt64(key);
			break;
	}
	enc = lion_slice_encode(v, nbits);
	while (enc != 0)
	{
		pos[npos++] = pg_rightmost_one_pos64(enc);
		enc &= enc - 1;
	}
	pos[npos++] = LION_SLICE_EXISTS;
	return npos;
}

/*
 * A row of a sliced column: into the slice of every bit its value's encoding
 * has set, in ascending position, and then into the existence slice, AFTER
 * the row went under its key.  Readers see no half-done row: its transaction
 * has not committed (lioninsert()).  A failure between the steps leaves a
 * dead row in some of the sets and not the others, which no snapshot sees and
 * the next VACUUM takes out of all of them; lion_index_verify() knows such a
 * row by its being dead (lion_verify_slices()).
 *
 * ONE DESCENT for the column (DESIGN.md §51, "Insert cost, revisited"): a
 * column's slices sort together, by position, after its summaries, so they
 * are one run on one leaf or a few adjacent ones, and the positions a row
 * needs are visited in that order.  The first is found by a descent; each
 * next one from the leaf the walk holds (lion_dir_find_next()), which moves
 * right as a descent would when the slices spill onto the next leaf or a
 * split of the held leaf moved them there.  The leaf stays held from the
 * first slice to the last.  Each slice has its writer check; the in-place
 * adds go into a batch of several slices to a record (lion_slice_batch_add()
 * above), and anything else is put in as by an insert of its own
 * (lion_slice_put()), its posting pages locked after the leaf and let go
 * before the next, after the batch is written - so the lock order and the
 * split protocol are those of one insert, repeated.  Holding the leaf across
 * the steps keeps out the other writers of these slices for the row's whole
 * walk, rather than for each step; they would queue on the same leaf for
 * every step anyway.
 *
 * THE SEAM for the plane kernel (DESIGN.md §50, §51 "Not done"): one row at
 * a time.  An insert of many rows - a COPY, an INSERT ... SELECT - could turn
 * them into planes (lion_planes_scatter_strided()) and add each plane's word
 * to its slice once; aminsert is handed one row at a time, so that is a
 * bulk-insert path of its own.
 */
static void
lion_slice_insert(Relation index, Relation heaprel, LionState *state,
				  Datum key, uint32 ckey, uint16 lo)
{
	int			pos[LION_SLICE_MAX_POS];
	int			npos = lion_slice_positions(state, key, pos);
	Buffer		leafbuf = InvalidBuffer;
	OffsetNumber entryoff = InvalidOffsetNumber;
	LionSliceBatch batch;
	int			i;

	batch.ckey = ckey;
	batch.lo = lo;
	batch.n = 0;
	batch.nbufs = 0;
	batch.maxbufs = lion_wal_max_blocks(index) - 1;
	batch.checked = false;

	for (i = 0; i < npos; i++)
	{
		LionSearchKey sk;
		uint8		posbyte;
		bool		movedright = false;
		bool		found;

		lion_search_key_slice(state, &sk, pos[i], &posbyte);
		if (i == 0)
			found = lion_dir_find(index, heaprel, state->ix, &sk,
								  BUFFER_LOCK_EXCLUSIVE, true, &leafbuf,
								  &entryoff, &movedright);
		else
		{
			/* the batch's entries are on the leaf: written before it goes */
			if (!lion_dir_leaf_holds(leafbuf, &sk))
			{
				lion_slice_batch_write(index, leafbuf, &batch);
				batch.checked = false;
			}
			found = lion_dir_find_next(index, heaprel, state->ix, &sk,
									   entryoff, &leafbuf, &entryoff);
		}

		/* SERIALIZABLE: the page that stands for the slice (DESIGN.md §9) */
		lion_writer_check(index, leafbuf, entryoff, found);

		if (found)
		{
			int			r;

			if ((lion_page_entry(BufferGetPage(leafbuf), entryoff)->flags &
				 LION_ENTRY_CHAIN) != 0)
			{
				r = lion_slice_batch_add(index, &batch, leafbuf, entryoff);
				if (r == LION_SLICE_BUSY)
				{
					lion_slice_batch_write(index, leafbuf, &batch);
					r = lion_slice_batch_add(index, &batch, leafbuf, entryoff);
					Assert(r != LION_SLICE_BUSY);
				}
			}
			else
				r = lion_slice_batch_inline(index, state, &batch, leafbuf,
											entryoff);
			if (r == LION_SLICE_TAKEN)
			{
				if (batch.nbufs == batch.maxbufs)
					lion_slice_batch_write(index, leafbuf, &batch);
				continue;
			}
		}

		/* the ordinary path, which may split the leaf: the batch goes first */
		lion_slice_batch_write(index, leafbuf, &batch);
		lion_slice_put(index, heaprel, state, &leafbuf, entryoff, movedright,
					   found, pos[i], ckey, lo);
		batch.checked = false;
	}

	lion_slice_batch_write(index, leafbuf, &batch);
	UnlockReleaseBuffer(leafbuf);
}

/*
 * The first block of the statement's insert context (lioninsert()): what a
 * row allocates, with room to spare, so that no row needs a second block.
 */
#define LION_INSERT_CONTEXT_SIZE	(64 * 1024)

/* ---------------------------------------------------------------------
 * Batches (DESIGN.md §55)
 * --------------------------------------------------------------------- */

/*
 * Rows of one key, held to under one hold of its directory leaf: enough that
 * the handoffs of the leaf stop mattering, few enough that a writer of
 * another key on the leaf - or VACUUM - waits for well under a millisecond.
 */
#define LION_APPLY_HOLD		256

/* The directory's order of entry kinds (DESIGN.md §21): reserved, then values */
static inline int
lion_insert_row_kind(const LionInsertRow *row)
{
	if (row->flag == LION_ENTRY_SLICE)
		return LION_KIND_SLICE;
	if (row->flag == LION_ENTRY_SUMMARY)
		return LION_KIND_SUMMARY;
	return (row->flag != 0) ? lion_reserved_kind(row->flag) : LION_KIND_VALUE;
}

/*
 * Do a and b go to the same entry, by the bytes of their keys?  Two keys
 * that are equal but not byte for byte - which a collation or a type may
 * make - only make two groups, each of which finds the entry the other
 * found.  The order is a cheap one, for grouping: columns, kinds, bucket
 * hashes, key bytes.
 */
static int
lion_insert_row_keycmp(LionIndexState *ix, const LionInsertRow *a,
					   const LionInsertRow *b)
{
	LionState  *state;

	if (a->col != b->col)
		return (a->col < b->col) ? -1 : 1;
	if (a->flag != b->flag)
		return (lion_insert_row_kind(a) < lion_insert_row_kind(b)) ? -1 : 1;
	if (a->hash != b->hash)
		return (a->hash < b->hash) ? -1 : 1;
	if (a->flag == LION_ENTRY_SLICE)	/* by position, as the directory */
		return (a->key < b->key) ? -1 : (a->key > b->key) ? 1 : 0;
	if (a->flag != 0)
		return 0;

	state = &ix->cols[a->col];
	if (state->typbyval)
		return (a->key < b->key) ? -1 : (a->key > b->key) ? 1 : 0;
	if (state->typlen > 0)
		return memcmp(DatumGetPointer(a->key), DatumGetPointer(b->key),
					  state->typlen);
	if (state->typlen == -1)
	{
		Size		la = VARSIZE_ANY(DatumGetPointer(a->key));
		Size		lb = VARSIZE_ANY(DatumGetPointer(b->key));

		if (la != lb)
			return (la < lb) ? -1 : 1;
		return memcmp(DatumGetPointer(a->key), DatumGetPointer(b->key), la);
	}
	return strcmp(DatumGetCString(a->key), DatumGetCString(b->key));
}

/* ... and then by TID, so a key's posting set is walked once, in order */
static int
lion_insert_row_cmp(const void *pa, const void *pb, void *arg)
{
	const LionInsertRow *a = (const LionInsertRow *) pa;
	const LionInsertRow *b = (const LionInsertRow *) pb;
	int			c = lion_insert_row_keycmp((LionIndexState *) arg, a, b);

	if (c != 0)
		return c;
	return (a->code < b->code) ? -1 : (a->code > b->code) ? 1 : 0;
}

/* The TIDs of two batch rows, for a slice's rows that came out of order */
static int
lion_insert_code_cmp(const void *pa, const void *pb)
{
	uint64		a = ((const LionInsertRow *) pa)->code;
	uint64		b = ((const LionInsertRow *) pb)->code;

	return (a < b) ? -1 : (a > b) ? 1 : 0;
}

/* One key's rows of a batch: rows[start .. start + n) */
typedef struct LionInsertGroup
{
	int			start;
	int			n;
} LionInsertGroup;

typedef struct LionInsertGroupSort
{
	LionIndexState *ix;
	LionInsertRow *rows;
} LionInsertGroupSort;

/*
 * The groups in the directory's own order (lion_cmp_prefix()): column, kind,
 * an ordered column's comparison, bucket hash, then the bytes.  Keys that
 * go into the directory in its order land on the leaf the one before them
 * left off at, and fill leaves the way an ascending run of inserts does,
 * rather than splitting them all down the middle.
 *
 * The comparison is the column's proc 4, as the descent's is - never a sort
 * operator, which an index whose catalog has changed since its build may no
 * longer have although its directory is ordered (lion_fill_column_state()).
 */
static int
lion_insert_group_cmp(const void *pa, const void *pb, void *arg)
{
	LionInsertGroupSort *gs = (LionInsertGroupSort *) arg;
	const LionInsertRow *a = &gs->rows[((const LionInsertGroup *) pa)->start];
	const LionInsertRow *b = &gs->rows[((const LionInsertGroup *) pb)->start];
	LionState  *state = &gs->ix->cols[a->col];

	if (a->col == b->col && a->flag == 0 && b->flag == 0 && state->ordered)
	{
		int32		c = DatumGetInt32(FunctionCall2Coll(&state->cmpproc,
														state->collation,
														a->key, b->key));

		if (c != 0)
			return c < 0 ? -1 : 1;
	}
	return lion_insert_row_keycmp(gs->ix, a, b);
}

/*
 * Does a's prefix (lion_cmp_prefix()) sort strictly before b's?  a is the key
 * of an earlier hold of the same batch, so it sorts at or before b (the
 * groups are in the directory's order): the question is only whether the two
 * prefixes tie - the same column and kind, and for a value the same position
 * under proc 4 in an ordered column, the same hash in any other; for a bit
 * slice the same slice.  Only when they do not is the leaf that held a's
 * entry one whose range begins before every entry of b's prefix run, which
 * lion_dir_find_from() needs.
 */
static bool
lion_insert_prefix_before(const LionSearchKey *a, const LionSearchKey *b)
{
	if (a->attno != b->attno || a->kind != b->kind)
		return true;
	if (a->kind == LION_KIND_SLICE)
		return *(const uint8 *) a->raw != *(const uint8 *) b->raw;
	if (a->kind != LION_KIND_VALUE)
		return false;
	if (a->cmpproc != NULL)
		return DatumGetInt32(FunctionCall2Coll(a->cmpproc, a->collation,
											   a->key, b->key)) != 0;
	return a->hash != b->hash;
}

/* A summarized column's batch rows by key, then TID (lion_summary_insert_rows()) */
static int
lion_insert_summary_cmp(const void *pa, const void *pb, void *arg)
{
	const LionInsertRow *a = (const LionInsertRow *) pa;
	const LionInsertRow *b = (const LionInsertRow *) pb;
	LionState  *state = (LionState *) arg;
	int32		c = DatumGetInt32(FunctionCall2Coll(&state->cmpproc,
													state->collation,
													a->key, b->key));

	if (c != 0)
		return c < 0 ? -1 : 1;
	return (a->code < b->code) ? -1 : (a->code > b->code) ? 1 : 0;
}

/*
 * The rows rows[0 .. n) of a batch, all of whose keys are at or below segkey,
 * into the summary that a descent to (SUMMARY, segkey) lands on - the bucket
 * of every one of them (lion_summary_insert_rows()).  They go in in TID
 * order, an INLINE summary's as many as its payload holds under the limit in
 * one rewrite (lion_insert_inline_rows()), a CHAIN one's up to
 * LION_APPLY_HOLD a hold, a record to a container (lion_insert_chain_rows()),
 * and the summary is looked up again for the next hold.  buf, when valid,
 * holds that summary at off EXCLUSIVE already, for the first hold.  A lookup
 * that lands anywhere but on a summary of the column at or above segkey - its
 * bucket emptied and removed meanwhile, say - leaves the rest to
 * lion_summary_insert(), a row at a time.
 */
static void
lion_summary_put_rows(Relation index, Relation heaprel, LionState *state,
					  MemoryContext holdcxt, LionInsertRow *rows, int n,
					  Datum segkey, Buffer buf, OffsetNumber off)
{
	int			i;

	for (i = 1; i < n; i++)
		if (rows[i - 1].code > rows[i].code)
			break;
	if (i < n)
		qsort(rows, n, sizeof(LionInsertRow), lion_insert_code_cmp);

	i = 0;
	while (i < n)
	{
		LionEntryTuple *e;
		int			taken;

		CHECK_FOR_INTERRUPTS();
		if (!BufferIsValid(buf))
		{
			LionSearchKey sk;
			Page		page;

			lion_search_key_init(state, &sk, LION_KIND_SUMMARY, segkey, 0);
			buf = lion_dir_search_first(index, heaprel, state->ix, &sk,
										BUFFER_LOCK_EXCLUSIVE, true, &off);
			page = BufferGetPage(buf);
			e = (off <= PageGetMaxOffsetNumber(page)) ?
				lion_page_entry(page, off) : NULL;
			if (e == NULL || LionEntryIsPivot(e) ||
				e->attno != state->attno || !LionEntryIsSummary(e) ||
				DatumGetInt32(FunctionCall2Coll(&state->cmpproc,
												state->collation, segkey,
												lion_entry_key(state, e))) > 0)
			{
				UnlockReleaseBuffer(buf);
				for (; i < n; i++)
					lion_summary_insert(index, heaprel, state, rows[i].key,
										lion_code_ckey(rows[i].code),
										lion_code_lo(rows[i].code));
				return;
			}
		}

		/* SERIALIZABLE (DESIGN.md §9): the summary the rows go into */
		lion_writer_check(index, buf, off, true);
		e = lion_page_entry(BufferGetPage(buf), off);
		if ((e->flags & LION_ENTRY_INLINE) != 0)
		{
			taken = (n - i == 1) ? 0 :
				lion_insert_inline_rows(index, heaprel, state, buf, off,
										&rows[i], Min(n - i, LION_APPLY_HOLD),
										false);
			if (taken == 0)
			{
				/* a row alone, or a first row that spills the summary */
				lion_insert_inline(index, heaprel, state, buf, off,
								   lion_code_ckey(rows[i].code),
								   lion_code_lo(rows[i].code), NULL);
				taken = 1;
			}
		}
		else
		{
			int			stop = Min(n - i, LION_APPLY_HOLD);

			for (taken = 0; taken < stop;)
				taken += lion_insert_chain_rows(index, heaprel, buf, off,
												&rows[i + taken], stop - taken);
		}
		UnlockReleaseBuffer(buf);
		buf = InvalidBuffer;
		MemoryContextReset(holdcxt);
		i += taken;
		LION_INJECTION_POINT("lion-insert-batch-summary");
	}
}

/*
 * lion_summary_insert() for a batch (DESIGN.md §55, "A column with
 * summaries"): rows[0 .. n) are the summary rows of one column, which the
 * batch writes after the column's keys, so each row is under its key's entry
 * first, as the row's own steps go.  They come sorted by key, then TID, and
 * go into their buckets a run at a time:
 *
 *	- a closed summary takes every row up to its key in one run;
 *	- the open one (SUMLAST) takes the rows up to its key, and the rows above
 *	  it while it holds fewer than summary_tids - its key raised once, to the
 *	  largest of them, as the row that raises it goes in
 *	  (lion_summary_rekey_insert()) - and then the rest of the run;
 *	- a row above a full open bucket closes it and opens the next
 *	  (lion_summary_close_last()), and the rows after it go on from there.
 *
 * Which bucket a row lands in can differ from where row-by-row inserts in
 * TID order would have put it - a key that a later row would have raised the
 * open bucket past may close it here - but every bucket still holds just the
 * keys in (the key before it, its own key], which is all a count asks.  A
 * column with no summary at or above a row's key goes through
 * lion_summary_insert(), which makes one.
 */
static void
lion_summary_insert_rows(Relation index, Relation heaprel, LionState *state,
						 MemoryContext holdcxt, LionInsertRow *rows, int n)
{
	uint32		bucket_tids = state->ix->meta.summary_tids;
	int			i;

	for (i = 1; i < n; i++)
		if (lion_insert_summary_cmp(&rows[i - 1], &rows[i], state) > 0)
			break;
	if (i < n)
		qsort_arg(rows, n, sizeof(LionInsertRow), lion_insert_summary_cmp,
				  state);

	i = 0;
	while (i < n)
	{
		LionSearchKey sk;
		Buffer		buf;
		Page		page;
		OffsetNumber off;
		LionEntryTuple *e = NULL;
		int			kind = -1;
		int			j;

		CHECK_FOR_INTERRUPTS();
		lion_search_key_init(state, &sk, LION_KIND_SUMMARY, rows[i].key, 0);
		buf = lion_dir_search_first(index, heaprel, state->ix, &sk,
									BUFFER_LOCK_EXCLUSIVE, true, &off);
		page = BufferGetPage(buf);
		if (off <= PageGetMaxOffsetNumber(page))
		{
			e = lion_page_entry(page, off);
			if (e->attno == state->attno && LionEntryIsSummary(e) &&
				!LionEntryIsPivot(e))
				kind = lion_entry_kind(e);
		}

		if (kind == LION_KIND_SUMMARY)
		{
			/* lion_dir_search_first() found its key at or above the row's */
			Datum		key = lion_entry_key(state, e);

			for (j = i + 1; j < n; j++)
				if (DatumGetInt32(FunctionCall2Coll(&state->cmpproc,
													state->collation,
													rows[j].key, key)) > 0)
					break;
			lion_summary_put_rows(index, heaprel, state, holdcxt, &rows[i],
								  j - i, rows[j - 1].key, buf, off);
			i = j;
			continue;
		}

		if (kind == LION_KIND_SUMLAST)
		{
			Datum		top = lion_entry_key(state, e);
			uint64		ntids = lion_entry_ntids(e);
			int			raise = -1; /* the row that raises its key, if any */

			/* SERIALIZABLE (DESIGN.md §9), as lion_summary_insert() checks */
			lion_writer_check(index, buf, off, true);

			/* the run it takes, its rows counted as they come */
			for (j = i; j < n; j++, ntids++)
			{
				if (DatumGetInt32(FunctionCall2Coll(&state->cmpproc,
													state->collation,
													rows[j].key, top)) <= 0)
					continue;
				if (ntids >= (uint64) bucket_tids)
					break;
				top = rows[j].key;
				raise = j;
			}

			if (j == i)
			{
				/* full, and the row is above it: the next bucket */
				lion_writer_check(index, buf, off, false);
				lion_summary_close_last(index, heaprel, state, buf, off,
										rows[i].key, LION_SUMMARY_HASH,
										lion_code_ckey(rows[i].code),
										lion_code_lo(rows[i].code));
				UnlockReleaseBuffer(buf);
				MemoryContextReset(holdcxt);
				i++;
				continue;
			}

			if (raise < 0)
			{
				lion_summary_put_rows(index, heaprel, state, holdcxt, &rows[i],
									  j - i, rows[j - 1].key, buf, off);
				i = j;
				continue;
			}

			/*
			 * Its key raised to the largest of the run's, with that row in -
			 * the last of the run, the rows after it being its key's - and
			 * then the rest.  An entry the rewrite moved off the leaf is
			 * looked up again with the rows as they stand (nothing went in
			 * but its key).
			 */
			{
				LionInsertRow last = rows[raise];
				bool		done;

				lion_writer_check(index, buf, off, false);
				done = lion_summary_rekey_insert(index, heaprel, state, buf, off,
												 last.key, LION_SUMMARY_HASH,
												 lion_code_ckey(last.code),
												 lion_code_lo(last.code));
				UnlockReleaseBuffer(buf);
				MemoryContextReset(holdcxt);
				if (!done)
					continue;
				memmove(&rows[raise], &rows[raise + 1],
						sizeof(LionInsertRow) * (j - raise - 1));
				rows[j - 1] = last;
				if (j - 1 > i)
					lion_summary_put_rows(index, heaprel, state, holdcxt,
										  &rows[i], j - 1 - i, last.key,
										  InvalidBuffer, InvalidOffsetNumber);
				i = j;
				continue;
			}
		}

		/* no summary at or above the key: a row's own path makes one */
		UnlockReleaseBuffer(buf);
		lion_summary_insert(index, heaprel, state, rows[i].key,
							lion_code_ckey(rows[i].code),
							lion_code_lo(rows[i].code));
		MemoryContextReset(holdcxt);
		i++;
	}
}

/*
 * The rows rows[0 .. n) of one key, whose entry the writer found on a leaf it
 * took without a descent (lion_dir_find_on_leaf()) and holds EXCLUSIVE: as
 * many as one hold takes - a CHAIN entry's up to LION_APPLY_HOLD, an INLINE
 * entry's as many as its payload holds under the limit - or none, with
 * nothing written, when the INLINE entry would spill or grow past what the
 * leaf has room for (lion_insert_known()).
 */
static int
lion_insert_apply_known(Relation index, Relation heaprel, LionState *state,
						Buffer leafbuf, OffsetNumber entryoff,
						const LionInsertRow *rows, int n)
{
	bool		haspos = state->positions && rows[0].flag == 0;
	int			i;
	int			stop;

	if ((lion_page_entry(BufferGetPage(leafbuf), entryoff)->flags &
		 LION_ENTRY_INLINE) != 0)
	{
		/* positions inline go in by a descent (lion_insert_known()) */
		if (haspos)
			return 0;
		/* a key of one row: the single insert's path, which copies less */
		if (n == 1)
			return lion_insert_known(index, heaprel, state, leafbuf, entryoff,
									 lion_code_ckey(rows[0].code),
									 lion_code_lo(rows[0].code), NULL) ? 1 : 0;
		lion_writer_check(index, leafbuf, entryoff, true);
		return lion_insert_inline_rows(index, heaprel, state, leafbuf,
									   entryoff, rows,
									   Min(n, LION_APPLY_HOLD), true);
	}

	lion_writer_check(index, leafbuf, entryoff, true);
	stop = Min(n, LION_APPLY_HOLD);
	if (haspos)
		lion_insert_chain_pos_rows(index, heaprel, state, leafbuf, entryoff,
								   rows, stop);
	else
		for (i = 0; i < stop;)
			i += lion_insert_chain_rows(index, heaprel, leafbuf, entryoff,
										&rows[i], stop - i);
	return stop;
}

/*
 * Write a batch of buffered rows (lion_defer.c), reordering rows[] in place.
 *
 * Each key's rows are what lion_insert_one() would do for them one at a
 * time, with the descents and the records shared: a CHAIN entry - the
 * posting set of a key of many rows - takes up to LION_APPLY_HOLD of them
 * under one hold of its leaf, the entry rewritten in place at its size, so
 * its offset stays good, and the rows one container takes in place go in in
 * one record (lion_insert_chain_rows()).  A new entry, or an INLINE one,
 * takes as many as its payload holds under the limit in one rewrite of the
 * entry, which may move it (lion_insert_new_entry_rows(),
 * lion_insert_inline_rows()); an INLINE entry that spills becomes a CHAIN one
 * and takes the rest in holds.  The keys go in directory order,
 * their rows in TID order.
 *
 * The rows are untouched but for their order, so a write that fails can be
 * redone whole: a row already in is a no-op the second time.
 */
void
lion_insert_apply(Relation index, Relation heaprel, LionInsertRow *rows,
				  int nrows)
{
	LionIndexState *ix = lion_get_index_state(index);
	LionInsertGroup *groups;
	LionInsertGroupSort gs;
	MemoryContext cxt;
	MemoryContext holdcxt;
	MemoryContext oldcxt;
	int			ngroups = 0;
	int			nother;
	int			nslice = 0;
	int			nslicebuckets = 0;
	int		   *sliceat = NULL;
	int			g;
	int			i;
	LionLastLeaf last;
	LionSearchKey lastsk;
	uint8		lastposbyte;	/* lastsk's, for a slice */
	LionPosMember *pm = NULL;	/* a row's positions, for one insert */

	if (nrows == 0)
		return;

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"lion index insert batch",
								ALLOCSET_DEFAULT_SIZES);
	holdcxt = AllocSetContextCreate(cxt,
									"lion index insert batch hold",
									LION_INSERT_CONTEXT_SIZE,
									LION_INSERT_CONTEXT_SIZE,
									ALLOCSET_DEFAULT_MAXSIZE);
	oldcxt = MemoryContextSwitchTo(cxt);
	pm = (LionPosMember *) palloc(sizeof(LionPosMember));
	memset(&last, 0, sizeof(last));
	memset(&lastsk, 0, sizeof(lastsk));
	last.blk = InvalidBlockNumber;
	last.cxt = cxt;

	/*
	 * A sliced column's rows go into a dozen or more bit slices each
	 * (DESIGN.md §51), so most of a batch of one can be slice rows: they go
	 * to the end, each slice's together, by a counting pass rather than the
	 * sort.  A slice's rows are its column's, which come in the order the
	 * heap took them - TID order nearly always, which is checked.
	 */
	nother = nrows;
	for (i = 0; i < nrows; i++)
		if (rows[i].flag == LION_ENTRY_SLICE)
			nslice++;
	if (nslice > 0)
	{
		int			nbuckets = ix->ncolumns * LION_SLICE_MAX_POS;
		int		   *fill = (int *) palloc0(sizeof(int) * (nbuckets + 1));
		LionInsertRow *slices = (LionInsertRow *)
			palloc(sizeof(LionInsertRow) * nslice);
		int			b;

		/* a slice's bucket: its column's, then its position's index */
#define LION_SLICE_BUCKET(r) \
	((r)->col * LION_SLICE_MAX_POS + \
	 (DatumGetInt32((r)->key) == LION_SLICE_EXISTS ? LION_SLICE_MAX_BITS : \
	  DatumGetInt32((r)->key)))

		sliceat = (int *) palloc(sizeof(int) * (nbuckets + 1));
		for (i = 0; i < nrows; i++)
			if (rows[i].flag == LION_ENTRY_SLICE)
				fill[LION_SLICE_BUCKET(&rows[i]) + 1]++;
		for (b = 0; b < nbuckets; b++)
			fill[b + 1] += fill[b];
		memcpy(sliceat, fill, sizeof(int) * (nbuckets + 1));
		nother = 0;
		for (i = 0; i < nrows; i++)
		{
			if (rows[i].flag == LION_ENTRY_SLICE)
				slices[fill[LION_SLICE_BUCKET(&rows[i])]++] = rows[i];
			else if (nother++ != i)
				rows[nother - 1] = rows[i];
		}
#undef LION_SLICE_BUCKET
		Assert(nother + nslice == nrows);
		memcpy(&rows[nother], slices, sizeof(LionInsertRow) * nslice);
		pfree(slices);
		pfree(fill);
		nslicebuckets = nbuckets;
	}

	/* the other keys' rows together, in TID order, by a cheap comparison ... */
	qsort_arg(rows, nother, sizeof(LionInsertRow), lion_insert_row_cmp, ix);

	groups = (LionInsertGroup *) palloc(sizeof(LionInsertGroup) * nrows);
	for (i = 0; i < nother; i++)
	{
		if (i == 0 || lion_insert_row_keycmp(ix, &rows[i - 1], &rows[i]) != 0)
		{
			groups[ngroups].start = i;
			groups[ngroups++].n = 0;
		}
		groups[ngroups - 1].n++;
	}
	for (i = 0; i < nslicebuckets; i++)
	{
		LionInsertRow *srows = &rows[nother + sliceat[i]];
		int			n = sliceat[i + 1] - sliceat[i];
		int			j;

		if (n == 0)
			continue;
		for (j = 1; j < n; j++)
			if (srows[j - 1].code > srows[j].code)
				break;
		if (j < n)
			qsort(srows, n, sizeof(LionInsertRow), lion_insert_code_cmp);
		groups[ngroups].start = nother + sliceat[i];
		groups[ngroups++].n = n;
	}

	/* ... and the keys in the directory's order, by its own */
	gs.ix = ix;
	gs.rows = rows;
	if (ngroups > 1)
		qsort_arg(groups, ngroups, sizeof(LionInsertGroup),
				  lion_insert_group_cmp, &gs);

	MemoryContextSwitchTo(holdcxt);

	for (g = 0; g < ngroups; g++)
	{
		LionInsertRow *grows = &rows[groups[g].start];
		LionState  *state = &ix->cols[grows[0].col];
		uint16		flag = grows[0].flag;
		int			end = groups[g].n;
		uint8		posbyte;	/* a slice's search key points at it */
		bool		haspos = state->positions && flag == 0;

		if (flag == LION_ENTRY_SUMMARY)
		{
			/* a summarized column's buckets, after its keys */
			lion_summary_insert_rows(index, heaprel, state, holdcxt, grows,
									 end);
			MemoryContextReset(holdcxt);
			continue;
		}

		i = 0;
		while (i < end)
		{
			LionSearchKey sk;
			Buffer		leafbuf;
			OffsetNumber entryoff;
			bool		movedright = false;
			bool		found;
			BlockNumber hint;
			int			r = LION_FIND_NOT_HERE;

			if (flag == LION_ENTRY_SLICE)
				lion_search_key_slice(state, &sk,
									  (int) DatumGetInt32(grows[i].key),
									  &posbyte);
			else if (flag != 0)
				lion_search_key_init(state, &sk, lion_reserved_kind(flag),
									 (Datum) 0, grows[i].hash);
			else
				lion_search_key_init(state, &sk, LION_KIND_VALUE,
									 grows[i].key, grows[i].hash);

			/*
			 * The leaf this backend last wrote the key on, when it still
			 * holds the entry: a CHAIN entry's next rows, or an INLINE
			 * entry's next row where the leaf has the room, with no descent
			 * (DESIGN.md §21, "A leaf already known").
			 */
			hint = lion_leaf_hint_get(ix, &sk);
			if (BlockNumberIsValid(hint) &&
				lion_dir_find_on_leaf(index, &sk, hint, &leafbuf, &entryoff))
			{
				int			n = lion_insert_apply_known(index, heaprel, state,
														leafbuf, entryoff,
														&grows[i], end - i);

				/*
				 * Not noted as the leaf the next key starts from: a leaf
				 * reached without a descent may be a split's right half with
				 * no downlink yet, where a new entry must not go.
				 */
				UnlockReleaseBuffer(leafbuf);
				if (n > 0)
				{
					i += n;
					MemoryContextReset(holdcxt);
					LION_INJECTION_POINT("lion-insert-batch-hold");
					CHECK_FOR_INTERRUPTS();
					continue;
				}
			}

			/*
			 * Else the find-or-create, from the leaf the last one ended on
			 * when that key's prefix sorts before this one's - the keys go
			 * in the directory's order, so a run of them that share a leaf
			 * shares its descent - or from the root.
			 */
			if (BlockNumberIsValid(last.blk) &&
				lion_insert_prefix_before(&lastsk, &sk))
				r = lion_dir_find_from(index, heaprel, ix, &sk, &last,
									   &leafbuf, &entryoff, &movedright);
			if (r == LION_FIND_NOT_HERE)
				found = lion_dir_find(index, heaprel, ix, &sk,
									  BUFFER_LOCK_EXCLUSIVE, true, &leafbuf,
									  &entryoff, &movedright);
			else
				found = (r == LION_FIND_FOUND);
			lion_writer_check(index, leafbuf, entryoff, found);

			if (haspos && !found)
			{
				/* a new entry with positions: its first row, as one insert */
				lion_insert_row_member(state, &grows[i], pm);
				lion_insert_new_entry(index, heaprel, state, &leafbuf,
									  entryoff, movedright, grows[i].key, 0,
									  grows[i].hash,
									  lion_code_ckey(grows[i].code),
									  lion_code_lo(grows[i].code), pm);
				i++;
			}
			else if (haspos && (lion_page_entry(BufferGetPage(leafbuf),
												 entryoff)->flags &
								 LION_ENTRY_INLINE) != 0)
			{
				/* as for an INLINE entry without positions, below */
				int			n = (end - i == 1) ? 0 :
					lion_insert_inline_pos_rows(index, heaprel, state, leafbuf,
												entryoff, &grows[i],
												Min(end - i, LION_APPLY_HOLD),
												pm);

				if (n == 0)
				{
					lion_insert_row_member(state, &grows[i], pm);
					lion_insert_inline(index, heaprel, state, leafbuf, entryoff,
									   lion_code_ckey(grows[i].code),
									   lion_code_lo(grows[i].code), pm);
					n = 1;
				}
				i += n;
			}
			else if (haspos)
			{
				int			n = Min(end - i, LION_APPLY_HOLD);

				lion_insert_chain_pos_rows(index, heaprel, state, leafbuf,
										   entryoff, &grows[i], n);
				i += n;
			}
			else if (!found)
				i += lion_insert_new_entry_rows(index, heaprel, state,
												&leafbuf, entryoff, movedright,
												&grows[i],
												Min(end - i, LION_APPLY_HOLD));
			else if ((lion_page_entry(BufferGetPage(leafbuf),
									  entryoff)->flags & LION_ENTRY_INLINE) != 0)
			{
				/* a key of one row goes the single insert's way, below */
				int			n = (end - i == 1) ? 0 :
					lion_insert_inline_rows(index, heaprel, state, leafbuf,
											entryoff, &grows[i],
											Min(end - i, LION_APPLY_HOLD),
											false);

				if (n == 0)
				{
					/* its only row, or a first row that spills it */
					lion_insert_inline(index, heaprel, state, leafbuf, entryoff,
									   lion_code_ckey(grows[i].code),
									   lion_code_lo(grows[i].code), NULL);
					n = 1;
				}
				i += n;
			}
			else
			{
				int			stop = Min(end, i + LION_APPLY_HOLD);

				while (i < stop)
					i += lion_insert_chain_rows(index, heaprel, leafbuf,
												entryoff, &grows[i], stop - i);
			}

			lion_dir_note_leaf(&last, leafbuf);
			lastsk = sk;
			if (sk.kind == LION_KIND_SLICE)
			{
				lastposbyte = posbyte;
				lastsk.raw = (const char *) &lastposbyte;
			}
			lion_leaf_hint_set(ix, &sk, last.blk);
			UnlockReleaseBuffer(leafbuf);
			MemoryContextReset(holdcxt);

			/*
			 * Test hook: between two holds, with nothing held;
			 * test/isolation/insert_batch_race.spec has other writers and
			 * VACUUM take the leaf here.
			 */
			LION_INJECTION_POINT("lion-insert-batch-hold");
			CHECK_FOR_INTERRUPTS();
		}
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);
}


/*
 * What lioninsert() keeps for a statement, in the IndexInfo's ii_AmCache:
 * the context its rows are inserted in, and how many rows it has had.
 */
typedef struct LionInsertCache
{
	MemoryContext cxt;
	uint64		nrows;
} LionInsertCache;

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
 *
 * After the statement's first row, a column's (key, TID) pairs - and its
 * summary's and slices' - are buffered rather than written, when
 * lion_defer_allowed() says the statement may: the buffer is written sorted
 * by key at the end of the statement, or before anything reads the index
 * (DESIGN.md §55, lion_defer.c).  The visibility argument is the same once more: no snapshot
 * that can see the row exists before the statement is done.
 */
bool
lioninsert(Relation index, Datum *values, bool *isnull, ItemPointer ht_ctid,
		  Relation heapRel, IndexUniqueCheck checkUnique, bool indexUnchanged,
		  IndexInfo *indexInfo)
{
	LionIndexState *ix;
	LionInsertCache *cache = NULL;
	MemoryContext insertcxt;
	MemoryContext oldcxt;
	LionDefer  *defer = NULL;
	bool		batch = false;
	uint64		code;
	uint32		ckey;
	uint16		lo;
	int			c;

	lion_check_key_offset(ht_ctid);

	ix = lion_get_index_state(index);

	/*
	 * The row is inserted in a context of its own, reset when it is done.
	 * The context is the statement's: it is kept in the IndexInfo the
	 * executor holds for the index (ii_AmCache, under ii_Context), so a
	 * statement of many rows makes it once.  Its first block takes what one
	 * row allocates - a few work buffers of LION_CONTAINER_MAX_SIZE, which
	 * the allocator rounds up to 8 kB each - and is kept by every reset, so
	 * a row allocates nothing from malloc.  A caller without an IndexInfo
	 * gets a context for the row alone, and no batch.
	 */
	if (indexInfo != NULL && indexInfo->ii_Context != NULL)
	{
		if (indexInfo->ii_AmCache == NULL)
		{
			cache = (LionInsertCache *)
				MemoryContextAllocZero(indexInfo->ii_Context,
									   sizeof(LionInsertCache));
			cache->cxt = AllocSetContextCreate(indexInfo->ii_Context,
											   "lion index insert",
											   LION_INSERT_CONTEXT_SIZE,
											   LION_INSERT_CONTEXT_SIZE,
											   ALLOCSET_DEFAULT_MAXSIZE);
			indexInfo->ii_AmCache = cache;
		}
		cache = (LionInsertCache *) indexInfo->ii_AmCache;
		insertcxt = cache->cxt;

		/* one row is no batch: the statement's first goes straight in */
		batch = (cache->nrows++ > 0) && lion_defer_allowed(indexInfo);
	}
	else
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

		/* buffered, or written now */
		if (batch && defer == NULL)
			defer = lion_defer_get(index, heapRel);

		/*
		 * A NULL value belongs to that column's reserved NULL entry, which is
		 * found by its flag rather than by its (absent) key (DESIGN.md §14).
		 * A multi-key opclass that extracts nothing from a non-NULL value
		 * puts the row in the reserved entry the extraction names the same
		 * way (DESIGN.md §17).
		 */
		if (isnull[c])
		{
			if (batch)
				lion_defer_add(defer, index, heapRel, state, c, (Datum) 0,
							   LION_ENTRY_NULLKEY, code, NULL);
			else
				lion_insert_one(index, heapRel, state, (Datum) 0,
							   LION_ENTRY_NULLKEY, ckey, lo, NULL);
		}
		else if (state->multikey)
		{
			Datum	   *keys;
			LionKeyPositions *kpos = NULL;
			LionPosMember *pm = NULL;
			uint16		nokey;
			int			nkeys;
			int			i;

			if (state->positions)
			{
				nkeys = lion_extract_value_pos(state, values[c], &keys, &kpos,
											   &nokey);
				pm = (LionPosMember *) palloc(sizeof(LionPosMember));
			}
			else
				nkeys = lion_extract_value(state, values[c], &keys, &nokey);

			/* every key checked before any is written (DESIGN.md §4) */
			for (i = 0; i < nkeys; i++)
				keys[i] = lion_key_prepare(state, keys[i], heapRel, ht_ctid);

			if (batch)
			{
				if (nkeys == 0)
					lion_defer_add(defer, index, heapRel, state, c, (Datum) 0,
								   nokey, code, NULL);
				for (i = 0; i < nkeys; i++)
				{
					if (pm != NULL)
						lion_posmember_from_key(pm, code, &kpos[i]);
					lion_defer_add(defer, index, heapRel, state, c, keys[i], 0,
								   code, pm);
				}
			}
			else
			{
				if (nkeys == 0)
					lion_insert_one(index, heapRel, state, (Datum) 0, nokey,
								   ckey, lo, NULL);
				for (i = 0; i < nkeys; i++)
				{
					if (pm != NULL)
						lion_posmember_from_key(pm, code, &kpos[i]);
					lion_insert_one(index, heapRel, state, keys[i], 0, ckey,
								   lo, pm);
					CHECK_FOR_INTERRUPTS();
				}
			}
		}
		else
		{
			/* stored as it is, or compressed when long (DESIGN.md §4) */
			key = lion_key_prepare(state, values[c], heapRel, ht_ctid);

			if (batch)
			{
				lion_defer_add(defer, index, heapRel, state, c, key, 0, code,
							   NULL);

				/*
				 * ... and its bucket's summary (DESIGN.md §32), and its bit
				 * slices (§51), a pair a slice, which the batch writes after
				 * the column's keys, as the row's own steps below go.
				 */
				if (state->summarized)
					lion_defer_add(defer, index, heapRel, state, c, key,
								   LION_ENTRY_SUMMARY, code, NULL);
				if (state->sliced)
				{
					int			pos[LION_SLICE_MAX_POS];
					int			npos = lion_slice_positions(state, key, pos);
					int			i;

					for (i = 0; i < npos; i++)
						lion_defer_add(defer, index, heapRel, state, c,
									   Int32GetDatum(pos[i]),
									   LION_ENTRY_SLICE, code, NULL);
				}
				CHECK_FOR_INTERRUPTS();
				continue;
			}
			lion_insert_one(index, heapRel, state, key, 0, ckey, lo, NULL);

			/*
			 * ... and in its bucket's summary (DESIGN.md §32), which is a
			 * second step with nothing held in between: see
			 * lion_summary_insert() for why that is safe.  Test hook: the row
			 * is under its key and in no summary yet;
			 * test/isolation/summary_race.spec runs lion_index_verify() in
			 * this window.
			 */
			if (state->summarized)
			{
				LION_INJECTION_POINT("lion-insert-before-summary");
				lion_summary_insert(index, heapRel, state, key, ckey, lo);
			}

			/*
			 * ... and in its bit slices (DESIGN.md §51), a step after that
			 * with nothing held in between, as for the summary.
			 */
			if (state->sliced)
				lion_slice_insert(index, heapRel, state, key, ckey, lo);
		}

		CHECK_FOR_INTERRUPTS();
	}

	MemoryContextSwitchTo(oldcxt);
	if (cache != NULL)
		MemoryContextReset(insertcxt);
	else
		MemoryContextDelete(insertcxt);

	/* No uniqueness check was performed. */
	return false;
}
