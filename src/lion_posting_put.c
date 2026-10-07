/*-------------------------------------------------------------------------
 *
 * lion_posting_put.c
 *		Writing container chains: putting containers and items on a posting
 *		set's pages, spilling an INLINE entry to a chain, and splitting a page.
 *
 * Part of the page layer, which lion.h declares.  See DESIGN.md sections 4
 * and 5.
 *
 * Every page modification in the page layer goes through the WAL shim of
 * DESIGN.md §25 (lion_wal_begin/register_buffer/op/finish), which writes
 * either a GenericXLog record or one of the extension's own: the buffer is
 * registered before it is touched and lion_wal_finish() runs before any
 * lock is dropped.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/generic_xlog.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/table.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "parser/parse_coerce.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/freespace.h"
#include "storage/indexfsm.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/typcache.h"
#include "varatt.h"

#include "lion.h"

static void lion_split_and_place(Relation index, Relation heaprel, Buffer buf,
								OffsetNumber off, bool replace,
								const LionTreeRef *tree,
								LionContainer **items, int nitems);

/*
 * An item of a leaf of the tree, checked as a reader checks it: a posting
 * set's container or segment, or a position chunk.
 */
static LionContainer *
lion_tree_item_fetch(Relation index, const LionTreeRef *tree, Page page,
					 BlockNumber blk, OffsetNumber off)
{
	if (tree->kind != 0)
		return lion_page_poschunk_fetch(index, page, blk, off);
	return lion_page_item_fetch(index, page, blk, off);
}

/* The entry travels with every change to a posting tree; a position tree has none. */
static void
lion_tree_put_entry(Relation index, LionWalState *xstate, const LionTreeRef *tree)
{
	if (tree->entry != NULL)
		lion_put_entry(index, xstate, tree->entrybuf, tree->entryoff,
					   tree->entry);
}

/* ---------------------------------------------------------------------
 * Container chains
 * --------------------------------------------------------------------- */

/*
 * Insert or replace a container on the container page buf, which the caller
 * holds EXCLUSIVE (or with a cleanup lock) and which owns c->ckey, splitting
 * the page when the container does not fit.  The buffer stays locked.
 *
 * entrybuf/entryoff identify the entry tuple, which the caller holds
 * EXCLUSIVE; entry must be a private (palloc'd) copy with ntids and
 * ncontainers already updated for this change.  It is written back in the
 * same WAL record as the container, so that a crash can never desynchronise
 * the counters from the containers.
 */
void
lion_chain_put_container_locked_ext(Relation index, Relation heaprel, Buffer buf,
								   Buffer entrybuf,
								   OffsetNumber entryoff, LionEntryTuple *entry,
								   LionContainer *c, int *ncontainers_delta,
								   bool slack)
{
	Page		page = BufferGetPage(buf);
	OffsetNumber off;
	bool		found;

	Assert(c->type != LION_CT_SPARSE);
	Assert(LionPageIsContainer(page));

	off = lion_page_find_container(index, page, BufferGetBlockNumber(buf),
								   c->ckey, &found);
	*ncontainers_delta = found ? 0 : 1;

	lion_chain_put_items_locked_ext(index, heaprel, buf, entrybuf, entryoff,
								   entry, off, found, &c, 1, slack);
}

void
lion_chain_put_container_locked(Relation index, Relation heaprel, Buffer buf,
							   Buffer entrybuf,
							   OffsetNumber entryoff, LionEntryTuple *entry,
							   LionContainer *c, int *ncontainers_delta)
{
	lion_chain_put_container_locked_ext(index, heaprel, buf, entrybuf, entryoff,
									   entry, c, ncontainers_delta, false);
}

/*
 * Replace the item at off, or insert at off, with nitems items.
 *
 * The items must be in ascending ckey order and their ranges must fit in the
 * gap the replaced item (or the insert position) leaves, so that the page
 * stays ordered by first ckey with non-overlapping item ranges (DESIGN.md
 * §13).  Everything happens in one WAL record together with the entry tuple,
 * which is what keeps a segment that is being split around a promoted
 * container from being visible as two items holding the same ckey.
 */
void
lion_chain_put_items_locked_ext(Relation index, Relation heaprel, Buffer buf,
							   Buffer entrybuf,
							   OffsetNumber entryoff, LionEntryTuple *entry,
							   OffsetNumber off, bool replace,
							   LionContainer **items, int nitems, bool slack)
{
	LionTreeRef tree;

	Assert((entry->flags & LION_ENTRY_CHAIN) != 0);
	Assert(BlockNumberIsValid(entry->head) && BlockNumberIsValid(entry->tail));

	tree.hash = entry->hash;
	tree.root = entry->head;
	tree.kind = 0;
	tree.entrybuf = entrybuf;
	tree.entryoff = entryoff;
	tree.entry = entry;
	lion_tree_put_items_locked(index, heaprel, buf, &tree, off, replace, items,
							   nitems, slack);
}

/*
 * The leaf write of either kind of tree (lion.h).  A position tree's items
 * are ordered by header ckey like a posting tree's, but two chunks may share
 * one: the members of one ckey can run across chunks (lion_positions.h).
 */
void
lion_tree_put_items_locked(Relation index, Relation heaprel, Buffer buf,
						   const LionTreeRef *tree, OffsetNumber off,
						   bool replace, LionContainer **items, int nitems,
						   bool slack)
{
	Page		page = BufferGetPage(buf);
	Size		sizes[LION_MAX_PUT_ITEMS];
	Size		allocs[LION_MAX_PUT_ITEMS];
	Size		need = 0;
	Size		want = 0;
	Size		have;
	int			i;

	Assert((tree->entry == NULL) == (tree->kind != 0));
	Assert(LionPageIsContainer(page));
	Assert((LionPageGetOpaque(page)->flags & LION_PAGE_POSITIONS) == tree->kind);
	Assert(nitems >= 1 && nitems <= LION_MAX_PUT_ITEMS);

	for (i = 0; i < nitems; i++)
	{
		sizes[i] = lion_item_size(items[i]);
		Assert(sizes[i] <= LION_CONTAINER_MAX_SIZE);
		Assert(lion_item_is_positions(items[i]) == (tree->kind != 0));
		Assert(i == 0 ||
			   (tree->kind != 0 ?
				items[i]->ckey >= items[i - 1]->ckey :
				lion_item_first_ckey(items[i]) > lion_item_last_ckey(items[i - 1])));
		allocs[i] = slack ? lion_item_alloc_size(items[i], sizes[i]) : sizes[i];
		need += MAXALIGN(sizes[i]) + sizeof(ItemIdData);
		want += MAXALIGN(allocs[i]) + sizeof(ItemIdData);
	}
	Assert(need <= LION_MAX_ITEM_SIZE + sizeof(ItemIdData));

	/*
	 * The item that goes away is overwritten or deleted by its line pointer,
	 * inside the record: it has to be one a reader would take, alone in the
	 * bytes the page allots it (lion_page_check_alone()).
	 */
	if (replace)
	{
		(void) lion_tree_item_fetch(index, tree, page,
									BufferGetBlockNumber(buf), off);
		lion_page_check_alone(index, page, BufferGetBlockNumber(buf), off);
	}

	/* One item taking another one's place: overwrite it where it is. */
	if (replace && nitems == 1)
	{
		Size		cur = ItemIdGetLength(PageGetItemId(page, off));
		Size		writesz = sizes[0];

		/*
		 * Prefer to leave the item's allocated length exactly as it is: then
		 * PageIndexTupleOverwrite() moves no other item on the page and the
		 * WAL delta covers the item alone.  That is the whole of "shrink in
		 * place" (DESIGN.md §18) as well as the growth case: an item that has
		 * lost members keeps its slot and the freed bytes become slack.  An
		 * item that has far more room than it can use - a segment replaced by
		 * the container one of its container keys was promoted to, or one
		 * that lost most of its members - gives the excess back.
		 *
		 * cur comes off the page, and the item is copied out of a work buffer
		 * of LION_CONTAINER_MAX_SIZE bytes, so a page that claims more than
		 * that (only a corrupt one can) gets the exact size.
		 */
		if (cur >= sizes[0] && cur <= (Size) LION_CONTAINER_MAX_SIZE &&
			cur - sizes[0] <= LION_ITEM_SLACK_BOUND)
			writesz = cur;
		else if (slack &&
				 MAXALIGN(allocs[0]) <=
				 MAXALIGN(cur) + PageGetExactFreeSpace(page))
			writesz = allocs[0];
		lion_item_zero_slack(items[0], sizes[0], writesz);

		{
			LionWalState *xstate = lion_wal_begin(index);
			Page		p = lion_wal_register_buffer(xstate, buf, LION_WALBUF_STD);

			lion_wal_save_item(xstate, p, off);
			if (PageIndexTupleOverwrite(p, off, items[0], writesz))
			{
				/*
				 * An item that is rewritten at (or near) the length it already
				 * has differs from its predecessor in a handful of bytes - one
				 * more member of an ARRAY, the two-byte cardinality - so the
				 * record carries those bytes and not the 1.6 KB item
				 * (DESIGN.md §25, LION_OP_DELTA).
				 */
				lion_wal_op_replace(xstate, p, off, items[0], writesz);
				lion_page_update_minmax(p);
				lion_wal_op(xstate, p, LION_OP_MINMAX, 0, 0, NULL, 0);
				lion_tree_put_entry(index, xstate, tree);
				lion_wal_finish(xstate, LION_XLOG_ITEM_REPLACE);
				return;
			}
			lion_wal_abort(xstate);
		}
	}

	/*
	 * Does everything fit as it stands?  PageIndexTupleDelete() compacts, so
	 * the space of the item that goes away is available to the new ones.
	 */
	have = PageGetExactFreeSpace(page);
	if (replace)
		have += MAXALIGN(ItemIdGetLength(PageGetItemId(page, off))) +
			sizeof(ItemIdData);

	/* Slack is a luxury: drop all of it rather than split the page for it. */
	if (want > have)
	{
		for (i = 0; i < nitems; i++)
			allocs[i] = sizes[i];
		want = need;
	}

	if (have >= want)
	{
		LionWalState *xstate = lion_wal_begin(index);
		Page		p = lion_wal_register_buffer(xstate, buf, LION_WALBUF_STD);

		if (replace)
		{
			PageIndexTupleDelete(p, off);
			lion_wal_op(xstate, p, LION_OP_DELETE, off, 0, NULL, 0);
		}

		for (i = 0; i < nitems; i++)
		{
			lion_item_zero_slack(items[i], sizes[i], allocs[i]);
			if (PageAddItemExtended(p, items[i], allocs[i],
									off + (OffsetNumber) i,
									0) == InvalidOffsetNumber)
				elog(ERROR, "lion index: failed to add item to page %u",
					 BufferGetBlockNumber(buf));
			lion_wal_op(xstate, p, LION_OP_ADD, off + (OffsetNumber) i, 0,
						items[i], allocs[i]);
		}

		lion_page_update_minmax(p);
		lion_wal_op(xstate, p, LION_OP_MINMAX, 0, 0, NULL, 0);
		lion_tree_put_entry(index, xstate, tree);
		lion_wal_finish(xstate, LION_XLOG_ITEM_ADD);
		return;
	}

	/*
	 * Not enough room: split the page and place the items.  The split places
	 * them at their exact size - a page that has just been split has room to
	 * spare, and the items get their slack back the next time they grow.
	 */
	lion_split_and_place(index, heaprel, buf, off, replace, tree, items, nitems);
}

void
lion_chain_put_items_locked(Relation index, Relation heaprel, Buffer buf,
						   Buffer entrybuf,
						   OffsetNumber entryoff, LionEntryTuple *entry,
						   OffsetNumber off, bool replace,
						   LionContainer **items, int nitems)
{
	lion_chain_put_items_locked_ext(index, heaprel, buf, entrybuf, entryoff,
								   entry, off, replace, items, nitems, false);
}

/*
 * Insert or replace a container in the chain of entry, splitting pages when
 * necessary.  Finds and locks the owning container page itself; see
 * lion_chain_put_container_locked() for the contract on entry.
 */
void
lion_chain_put_container(Relation index, Relation heaprel, Buffer entrybuf,
						OffsetNumber entryoff,
						LionEntryTuple *entry, LionContainer *c,
						int *ncontainers_delta)
{
	Buffer		buf;

	Assert((entry->flags & LION_ENTRY_CHAIN) != 0);
	Assert(BlockNumberIsValid(entry->head) && BlockNumberIsValid(entry->tail));

	/*
	 * A write descent, which takes the leaf EXCLUSIVE straight away and
	 * repairs any unfinished split on the way (DESIGN.md §22).
	 */
	buf = lion_posting_search(index, heaprel, entry->hash, entry->head,
							  c->ckey, BUFFER_LOCK_EXCLUSIVE, true);

	lion_chain_put_container_locked(index, heaprel, buf, entrybuf, entryoff,
								   entry, c, ncontainers_delta);

	UnlockReleaseBuffer(buf);
}

/*
 * The INLINE -> CHAIN spill (DESIGN.md §4, §5 INSERT step 3 and VACUUM step
 * 2, §18, §22).
 *
 * An INLINE posting set moves onto container pages when it outgrows its
 * entry, and two callers do that with payloads of very different sizes:
 *
 *	- an INSERT spills the payload it FOUND, which is at most inline_limit
 *	  bytes and therefore always fits one leaf;
 *	- VACUUM spills the payload it has just FILTERED, and removing members
 *	  can make a payload grow by an order of magnitude.  A clustered key's
 *	  RUN container is a few hundred bytes for 64 heap pages of rows; the
 *	  same container with a tenth of them gone at random is a 4104-byte
 *	  BITSET, and two of those do not fit one page.  A 4 KB payload of such
 *	  containers needs a leaf per container: seven for a 300,000-row table of
 *	  three keys, eleven for 150,000 rows of one key with every other row
 *	  deleted.
 *
 * This code used to allocate every leaf before its first record and refused
 * anything past four as "unreachable" - which it is for an INSERT - so every
 * VACUUM of such a table failed, and failed again the next time, autovacuum
 * and the anti-wraparound VACUUM included, until the failsafe gave up on
 * index vacuuming (2026-09-25 review).  The number of leaves is bounded by
 * the root now and by nothing else:
 *
 *	1. The ROOT is allocated first, because every page of a posting set is
 *	   stamped with the root's block (§18) - and never from the free space
 *	   map, which is what keeps owner_head an identity.  It stays pinned and
 *	   EXCLUSIVE until the end.
 *	2. Each LEAF is filled, linked to the next one and logged in a record of
 *	   its own, which registers that one buffer.  The next leaf is allocated
 *	   just before the record of the one in front of it - that record has to
 *	   carry the rightlink, and an rmgr-mode record cannot allocate (§25) - so
 *	   no more than two leaves are ever pinned.
 *	3. The root's downlinks, ONE internal level (LION_SPILL_MAX_LEAVES says
 *	   why one is always enough), go in with the rewritten entry in the LAST
 *	   record, which registers two buffers.
 *
 * The entry changes in that last record and in no other, so it is INLINE
 * until the whole posting set is on disk and CHAIN from the moment it is,
 * never half of each.  A crash or an ERROR before the last record leaves
 * behind leaves that nothing references, stamped with a root that was never
 * written, and loses nothing: the entry still holds the payload it always
 * held (for VACUUM, dead TIDs included, which the next VACUUM removes).
 * Those leaves are NOT empty, which is how every other leak looks, so the
 * leak sweep recognises them by their root instead: it is not a live root of
 * their key (lion_posting_root_live(), lion_vacuum_sweep()).  An INSERT's
 * spill is one record and cannot leave anything behind.
 */

/*
 * The most leaves a spill may write: as many downlinks as one page of pivots,
 * the root, holds.  It is never the limit that binds.  A leaf takes at least
 * one item, a spilled payload has no more items than the INLINE payload it
 * came from (filtering drops items and never splits one), and that payload is
 * at most LION_MAX_INLINE_LIMIT bytes of items that are each at least a
 * header long: 512 items against 678 downlinks on an 8 KB page, and the
 * assertion keeps it so for every block size the index supports.  The run-time
 * check in lion_entry_spill() is for a payload that is corrupt.
 */
#define LION_SPILL_MAX_LEAVES \
	((int) (LION_PAGE_CAPACITY / \
			(MAXALIGN(LION_POSTING_PIVOT_SIZE) + sizeof(ItemIdData))))

StaticAssertDecl(Min(LION_MAX_INLINE_LIMIT, LION_MAX_ENTRY_SIZE) / LION_CONTAINER_HDRSZ <=
				 LION_PAGE_CAPACITY / (MAXALIGN(LION_POSTING_PIVOT_SIZE) + sizeof(ItemIdData)),
				 "pg_lion: the downlinks of a spilled INLINE payload must fit one root page");

/*
 * Where the leaf that starts at byte `off` of an INLINE payload ends: the
 * offset just past the last item an empty leaf takes, counted exactly as
 * PageAddItemExtended() packs them (MAXALIGNed, one line pointer each).
 * *more says whether an item follows, i.e. whether another leaf is needed.
 *
 * A leaf always takes its first item - none is larger than
 * LION_CONTAINER_MAX_SIZE, which an empty page holds - so every call makes
 * progress.
 */
static Size
lion_spill_leaf_end(const char *payload, Size paylen, Size off,
					LionContainer *cbuf, bool *more)
{
	Size		used = 0;
	Size		end = off;
	Size		csize;

	*more = false;
	while ((csize = lion_inline_fetch(payload, paylen, &off, cbuf)) > 0)
	{
		Size		need = MAXALIGN(csize) + sizeof(ItemIdData);

		if (used > 0 && used + need > (Size) LION_PAGE_CAPACITY)
		{
			*more = true;
			break;
		}
		used += need;
		end = off;
	}

	return end;
}

/*
 * How many LEAVES an INLINE payload needs once it is on container pages.
 *
 * It decides whether the page the entry's `head` names is the single leaf or
 * the ROOT above several of them, and a root has to be allocated before the
 * first leaf is written, since every page of a posting set carries the root's
 * block (DESIGN.md §18, §22).  It is lion_spill_leaf_end() run to the end of
 * the payload, which is also what the fill loop in lion_entry_spill() runs,
 * so the two agree by construction.
 */
static int
lion_spill_count_leaves(const char *payload, Size paylen, LionContainer *cbuf)
{
	Size		off = 0;
	bool		more = true;
	int			n = 0;

	while (more)
	{
		off = lion_spill_leaf_end(payload, paylen, off, cbuf, &more);
		n++;
	}

	return n;
}

/*
 * Put the items of payload[off, end) on a leaf the open record has just
 * initialised, log them, and set the leaf's minckey/maxckey.
 * lion_spill_leaf_end() chose `end` so that they fit, so a failure here is a
 * bug - a PANIC in rmgr mode, where the record is a critical section.
 */
static void
lion_spill_fill_leaf(LionWalState *xstate, Page page, const char *payload,
					 Size off, Size end, LionContainer *cbuf)
{
	Size		csize;

	while ((csize = lion_inline_fetch(payload, end, &off, cbuf)) > 0)
	{
		OffsetNumber noff = PageAddItemExtended(page, cbuf, csize,
												InvalidOffsetNumber, 0);

		if (noff == InvalidOffsetNumber)
			elog(ERROR, "lion index: failed to spill a container onto a new leaf");
		lion_wal_op(xstate, page, LION_OP_ADD, noff, 0, cbuf, csize);
	}
	lion_page_update_minmax(page);
}

/*
 * Turn the caller's private copy of the entry into its CHAIN shape, for the
 * record that writes it.
 *
 * Only the INLINE/CHAIN half of the flags changes: a reserved entry (NULL-key,
 * §14, or empty-key, §17) stays the reserved entry it was once its payload
 * moves to a chain.  Losing a reserved bit here would leave a key-less entry
 * that lion_find_reserved_entry() no longer finds and that every other reader
 * takes for an ordinary entry with a zero-length key, so the next row of that
 * kind would start a second entry.
 */
static void
lion_spill_set_chain(LionEntryTuple *entry, BlockNumber root, BlockNumber tail)
{
	entry->flags = (entry->flags & (LION_ENTRY_KINDFLAGS | LION_ENTRY_POSITIONS)) |
		LION_ENTRY_CHAIN;
	entry->head = root;
	entry->tail = tail;
}

/*
 * Move an INLINE entry's payload onto container pages and turn it into a
 * CHAIN entry (see the comment above LION_SPILL_MAX_LEAVES).
 *
 * entry is a private copy of the entry tuple in its CHAIN shape (no payload,
 * with ncontainers and ntids already set by the caller); payload holds the
 * containers to write out, packed as in an INLINE payload.  The entry page is
 * held EXCLUSIVE by the caller (a cleanup lock, when VACUUM calls) and is
 * rewritten here, in the last record.
 */
/*
 * The root of a spilled entry's position tree, in the spill's last record:
 * one leaf holding the entry's inline positions chunk (none when poschunk is
 * NULL or empty).  It goes into the same record as the entry that starts
 * pointing at it, so that it is never a root nothing references; its block,
 * like the posting root's, comes from extending the relation.
 */
static void
lion_spill_posroot(LionWalState *xstate, Buffer posbuf, uint32 hash,
				   const LionContainer *poschunk, LionEntryTuple *entry)
{
	Page		page;
	BlockNumber posroot = BufferGetBlockNumber(posbuf);

	page = lion_wal_init_buffer(xstate, posbuf,
								LION_PAGE_CONTAINER | LION_PAGE_POSITIONS);
	lion_page_set_owner(page, hash, posroot);
	if (poschunk != NULL && poschunk->cardinality > 0)
	{
		Size		sz = lion_poschunk_size(poschunk);

		if (PageAddItemExtended(page, poschunk, sz, FirstOffsetNumber,
								0) == InvalidOffsetNumber)
			elog(ERROR, "lion index: failed to place a position chunk on a new root");
		lion_wal_op(xstate, page, LION_OP_ADD, FirstOffsetNumber, 0, poschunk,
					sz);
		lion_page_update_minmax(page);
		lion_wal_op(xstate, page, LION_OP_MINMAX, 0, 0, NULL, 0);
	}
	lion_wal_log_special(xstate, page);
	lion_entry_posext(entry)->pos_root = posroot;
}

void
lion_entry_spill(Relation index, Relation heaprel, Buffer entrybuf,
				OffsetNumber entryoff, LionEntryTuple *entry,
				const char *payload, Size paylen)
{
	if ((entry->flags & LION_ENTRY_POSITIONS) != 0)
		elog(ERROR, "lion index \"%s\": an entry with positions spills with them",
			 RelationGetRelationName(index));
	lion_entry_spill_pos(index, heaprel, entrybuf, entryoff, entry, payload,
						 paylen, NULL);
}

/*
 * lion_entry_spill() for an entry that stores positions: its inline chunk
 * (poschunk, an aligned copy, or NULL for none) becomes the first leaf of a
 * new position tree, whose root goes into the last record with the entry.
 * That record registers one buffer more than it did, three at most.
 */
void
lion_entry_spill_pos(Relation index, Relation heaprel, Buffer entrybuf,
					 OffsetNumber entryoff, LionEntryTuple *entry,
					 const char *payload, Size paylen,
					 const LionContainer *poschunk)
{
	LionWalState *xstate;
	Buffer		posbuf = InvalidBuffer;
	Buffer		rootbuf;
	Page		rootpage;
	BlockNumber root;
	LionContainer *cbuf;
	LionPostingPivot *pivots;
	Buffer		leafbuf;
	BlockNumber tail = InvalidBlockNumber;
	Size		off = 0;
	int			nleaves;
	int			i;

	/*
	 * The entry is rewritten in the last record, by its line pointer, at a
	 * smaller size, which moves the items below it on the leaf: it has to be
	 * a whole entry alone in its bytes (lion_page_check_alone()).
	 */
	(void) lion_page_entry_fetch(index, BufferGetPage(entrybuf),
								 BufferGetBlockNumber(entrybuf), entryoff);
	lion_page_check_alone(index, BufferGetPage(entrybuf),
						  BufferGetBlockNumber(entrybuf), entryoff);

	cbuf = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	nleaves = lion_spill_count_leaves(payload, paylen, cbuf);

	if (nleaves > LION_SPILL_MAX_LEAVES)
		elog(ERROR, "lion index \"%s\": an inline payload of %zu bytes needs %d container pages, more than one root can link",
			 RelationGetRelationName(index), paylen, nleaves);

	/*
	 * The ROOT of a posting set is the one page this index never recycles, so
	 * that root blocks come only from extending the relation and no two sets
	 * can ever share one - which is what makes owner_head an identity a
	 * reader can trust with nothing else in hand, exactly the situation the
	 * count cursor is in (DESIGN.md §18).  It is taken before any record
	 * opens (§25) and held EXCLUSIVE to the end, which is also what tells the
	 * leak sweep that the leaves written below are not orphans while this
	 * runs: it cannot lock their root.
	 */
	rootbuf = lion_alloc_page(index, heaprel, false);
	root = BufferGetBlockNumber(rootbuf);
	if ((entry->flags & LION_ENTRY_POSITIONS) != 0)
		posbuf = lion_alloc_page(index, heaprel, false);

	if (nleaves == 1)
	{
		/*
		 * One leaf, which IS the root: the whole spill is one record holding
		 * the new page and the entry that starts pointing at it.  Every spill
		 * an INSERT makes is this one.
		 */
		xstate = lion_wal_begin(index);
		rootpage = lion_wal_init_buffer(xstate, rootbuf, LION_PAGE_CONTAINER);
		lion_page_set_owner(rootpage, entry->hash, root);
		lion_spill_fill_leaf(xstate, rootpage, payload, 0, paylen, cbuf);
		lion_wal_log_special(xstate, rootpage);

		lion_spill_set_chain(entry, root, root);
		if (BufferIsValid(posbuf))
			lion_spill_posroot(xstate, posbuf, entry->hash, poschunk, entry);
		lion_put_entry(index, xstate, entrybuf, entryoff, entry);
		lion_wal_finish(xstate, LION_XLOG_ITEM_ADD);

		UnlockReleaseBuffer(rootbuf);
		if (BufferIsValid(posbuf))
			UnlockReleaseBuffer(posbuf);
		pfree(cbuf);
		return;
	}

	/*
	 * Several leaves.  The ROOT is written first, in a record of its own: an
	 * internal page of the set with no downlink yet.  Every leaf is stamped
	 * with its block, and "a block is a root once in the life of the index"
	 * rests on the block having been handed out by an extension that stays
	 * handed out - which an extension that no record ever touched does not,
	 * when an OS crash loses it: the next extension hands the same block out
	 * again, as the root of another set, and the leaves a crashed spill had
	 * stamped with it then looked live to the leak sweep and to verify() for
	 * as long as that set lived (lion_posting_root_live(); every summary's
	 * and every NULL entry's hash is the same 0, so the stamp could not tell
	 * them apart).  Replaying this record extends the relation past the root
	 * whatever the leaves' blocks, so the block is never a root again; a
	 * crash after it leaves an internal page without downlinks, which the
	 * sweep frees (lion_vac_children_deleted()) and after it the leaves.
	 */
	xstate = lion_wal_begin(index);
	rootpage = lion_wal_init_buffer(xstate, rootbuf, LION_PAGE_CONTAINER);
	LionPageGetOpaque(rootpage)->level = 1;
	lion_page_set_owner(rootpage, entry->hash, root);
	lion_wal_log_special(xstate, rootpage);
	lion_wal_finish(xstate, LION_XLOG_PAGE_INIT);

	/*
	 * Then the leaves, left to right, one record each.  Where each one ends
	 * is decided before its record opens, and so is its right sibling, which
	 * is allocated then: nothing fallible happens inside a record (§25), and
	 * a leaf is linked to a page that exists the moment it is written.
	 */
	pivots = (LionPostingPivot *) palloc(sizeof(LionPostingPivot) * nleaves);
	leafbuf = lion_alloc_page(index, heaprel, true);

	for (i = 0; i < nleaves; i++)
	{
		Buffer		nextbuf = InvalidBuffer;
		BlockNumber leafblk = BufferGetBlockNumber(leafbuf);
		Page		leafpage;
		Size		end;
		bool		more;

		end = lion_spill_leaf_end(payload, paylen, off, cbuf, &more);
		if (more != (i + 1 < nleaves))
			elog(ERROR, "lion index \"%s\": a spilled payload of %zu bytes does not pack the way it was counted",
				 RelationGetRelationName(index), paylen);
		if (more)
			nextbuf = lion_alloc_page(index, heaprel, true);

		xstate = lion_wal_begin(index);
		leafpage = lion_wal_init_buffer(xstate, leafbuf, LION_PAGE_CONTAINER);
		lion_page_set_owner(leafpage, entry->hash, root);
		lion_spill_fill_leaf(xstate, leafpage, payload, off, end, cbuf);
		if (more)
			LionPageGetOpaque(leafpage)->rightlink = BufferGetBlockNumber(nextbuf);
		lion_wal_log_special(xstate, leafpage);

		/* The leftmost downlink is minus infinity (DESIGN.md §22). */
		pivots[i].ckey = (i == 0) ? 0 : LionPageGetOpaque(leafpage)->minckey;
		pivots[i].child = leafblk;
		tail = leafblk;

		lion_wal_finish(xstate, LION_XLOG_ITEM_ADD);

		UnlockReleaseBuffer(leafbuf);
		leafbuf = nextbuf;
		off = end;
	}
	Assert(!BufferIsValid(leafbuf));

	/*
	 * Test hook: every leaf is written and logged, nothing references any of
	 * them, and the entry is still the INLINE entry it was.  An ERROR or a
	 * crash here is the leak the sweep recovers
	 * (test/isolation/vacuum_spill_interrupted.spec).  Compiles to nothing
	 * without --enable-injection-points.
	 */
	LION_INJECTION_POINT("lion-spill-leaves-written");

	/* The root's downlinks and the entry, in one last record. */
	lion_spill_set_chain(entry, root, tail);
	xstate = lion_wal_begin(index);
	rootpage = lion_wal_init_buffer(xstate, rootbuf, LION_PAGE_CONTAINER);
	LionPageGetOpaque(rootpage)->level = 1;
	lion_page_set_owner(rootpage, entry->hash, root);
	for (i = 0; i < nleaves; i++)
	{
		OffsetNumber noff = PageAddItemExtended(rootpage, &pivots[i],
												LION_POSTING_PIVOT_SIZE,
												InvalidOffsetNumber, 0);

		if (noff == InvalidOffsetNumber)
			elog(ERROR, "lion index: failed to build the root of a spilled posting set");
		lion_wal_op(xstate, rootpage, LION_OP_ADD, noff, 0, &pivots[i],
					LION_POSTING_PIVOT_SIZE);
	}
	lion_wal_log_special(xstate, rootpage);
	if (BufferIsValid(posbuf))
		lion_spill_posroot(xstate, posbuf, entry->hash, poschunk, entry);
	lion_put_entry(index, xstate, entrybuf, entryoff, entry);
	lion_wal_finish(xstate, LION_XLOG_ITEM_ADD);
	UnlockReleaseBuffer(rootbuf);
	if (BufferIsValid(posbuf))
		UnlockReleaseBuffer(posbuf);

	pfree(pivots);
	pfree(cbuf);
}

/*
 * Does `head` name the LIVE root of a posting set whose key hashes to `hash`?
 *
 * The leak sweep asks this about the owner stamp of a non-empty leaf that no
 * entry references (DESIGN.md §18) and frees the leaf when the answer is no,
 * so "no" must never be said of a page of a set that exists - while it is the
 * right answer for the leaves an interrupted spill leaves behind, whose root
 * was never written.
 *
 * Live means what the readers' owner check means - the page at `head` is a
 * container page, not DELETED, and stamped with (hash, head) itself - and a
 * downlink on it when it is an internal page.  Only a ROOT carries its own
 * block as owner_head, and a block is a root at most once in the life of the
 * index (lion_entry_spill() takes roots with reuse = false, and logs a
 * multi-leaf spill's root before its first leaf, so that not even an OS
 * crash hands the block out again), so:
 *
 *	- every page of a set that exists names a root that passes: its own set's,
 *	  which stays live until the entry is gone and the set is freed, and has a
 *	  downlink from the moment it is internal (a push-down makes it one with
 *	  one, in one record);
 *	- a spill that is still writing holds its root EXCLUSIVE from before it
 *	  stamps its first leaf until the root is complete, so the lock below
 *	  waits for it (wait = true) or fails and answers "live" (wait = false);
 *	- a root seen under that lock unwritten, with no downlink, DELETED, or
 *	  stamped for another set (its block recycled) stays that way for every
 *	  leaf stamped with it: the spill that stamped the leaf ended without
 *	  completing the root, and nothing will make that block a root again.
 *
 * With wait = false the only lock taken is a conditional one, so the caller
 * may hold other buffer locks (the sweep holds the leaf's cleanup lock); with
 * wait = true it must hold none.  A block past the end of the relation - a
 * root whose extension a crash undid - is not live.
 */
bool
lion_posting_root_live(Relation index, uint32 hash, BlockNumber head, bool wait)
{
	Buffer		buf;
	Page		page;
	bool		live;

	if (!BlockNumberIsValid(head) || head == LION_METAPAGE_BLKNO ||
		head >= RelationGetNumberOfBlocks(index))
		return false;

	buf = ReadBuffer(index, head);
	if (wait)
		LockBuffer(buf, BUFFER_LOCK_SHARE);
	else if (!ConditionalLockBuffer(buf))
	{
		ReleaseBuffer(buf);
		return true;			/* busy: somebody is writing it, keep it */
	}

	page = BufferGetPage(buf);
	live = lion_page_owns_entry(page, hash, head) &&
		!(LionPageIsPostingInternal(page) &&
		  PageGetMaxOffsetNumber(page) < lion_posting_first_data(page));
	UnlockReleaseBuffer(buf);

	return live;
}

/*
 * Split leaf page P (buf) and place the caller's items, all in one WAL record.
 *
 * Items at offsets off..maxoff (excluding the stale item at off when replace
 * is true, which is dropped) move to a brand new page N linked immediately
 * after P.  If the new items still do not fit on P, a second new page M
 * holding them is linked between P and N.  This always makes progress because
 * the caller's items together fit on an empty page (they are at most
 * LION_MAX_PUT_ITEMS items of at most LION_CONTAINER_MAX_SIZE bytes, and a
 * segment split only ever adds a few bytes to what was one item).  Items
 * never move left and never move to an existing page.
 *
 * DESIGN.md §22 adds the tree above them.  The new page - or, when there are
 * two, each of them - has no downlink in the parent when the record lands, so
 * the page to its LEFT is flagged LION_PAGE_INCOMPLETE_SPLIT and is held
 * EXCLUSIVE until its downlink has been inserted and the flag cleared.  A
 * crash in between costs nothing but the next write descent's repair.
 *
 * The ROOT is split by pushing it down instead, so that the root block - the
 * entry's `head`, and the owner stamp of every page of the set - never moves.
 *
 * P itself may be the left half of a split that never finished - a crash or
 * an ERROR between that split's two records (DESIGN.md §22) - when the caller
 * reached it without a descent, which is what VACUUM's regrow does: it walks
 * the right links.  That split is finished FIRST.  Splitting P again as it
 * stands would link the new page between P and the right half R of the old
 * split, and finishing THIS split would then give the new page its downlink
 * and clear the one flag that remembered R: R would have no downlink for
 * good, and every reader that seeks by container key - a descent, which is
 * how an intersection probes a set - would miss what is on it.  nbtree's
 * _bt_insertonpg() refuses to touch such a page at all, and lion_dir_place()
 * finishes the split first for the directory, as this does.
 *
 * Finishing it here, inside VACUUM's removal window (lion_wal_removal_begin()),
 * makes replay take a cleanup lock on the parent and on P for those records as
 * well, which is more than they need and harmless: the downlinks of a split
 * this function goes on to make are written in that window already.
 */
static void
lion_split_and_place(Relation index, Relation heaprel, Buffer buf,
					OffsetNumber off, bool replace, const LionTreeRef *tree,
					LionContainer **items, int nitems)
{
	Page		page;
	BlockNumber blk = BufferGetBlockNumber(buf);
	OffsetNumber maxoff;
	OffsetNumber firstright = replace ? OffsetNumberNext(off) : off;
	OffsetNumber delfirst = off;
	int			ndel;
	int			nmove;
	Size		sizes[LION_MAX_PUT_ITEMS];
	Size		need = 0;
	char	   *movebuf = NULL;
	Size	   *movelen = NULL;
	char	  **moveptr = NULL;
	OffsetNumber *delofs = NULL;
	LionWalState *xstate;
	Page		pP;
	Page		pN = NULL;
	Page		pM = NULL;
	Buffer		nbuf = InvalidBuffer;
	Buffer		mbuf = InvalidBuffer;
	BlockNumber oldright;
	BlockNumber nblk = InvalidBlockNumber;
	BlockNumber mblk = InvalidBlockNumber;
	uint32		firstmoved = 0;	/* first ckey of the items that move right */
	PGAlignedBlock *trial = NULL;
	bool		needm;
	int			i;

	/*
	 * An unfinished split of P comes first (see above).  Finishing it touches
	 * the parent and P's flag and nothing else: P's items and right link stay
	 * as they are, so off and everything below still describe the page.  The
	 * root is never flagged - its split is a push-down - so this cannot meet
	 * the push-down path below.
	 */
	if (LionPageIncompleteSplit(BufferGetPage(buf)))
		lion_posting_finish_split(index, heaprel, tree->hash, tree->root, buf);

	page = BufferGetPage(buf);
	maxoff = PageGetMaxOffsetNumber(page);
	ndel = (int) (maxoff + 1 - delfirst);
	nmove = (int) (maxoff + 1 - firstright);
	oldright = LionPageGetOpaque(page)->rightlink;

	Assert(ndel >= 0 && nmove >= 0 && nmove <= ndel);
	Assert(nitems >= 1 && nitems <= LION_MAX_PUT_ITEMS);
	Assert(LionPageIsPostingLeaf(page));
	Assert(!LionPageIncompleteSplit(page));

	if (blk == tree->root)
	{
		/*
		 * The whole set is this one page, so there is no parent to take a
		 * downlink.  Push the root down - its items go to a new child, the
		 * root block becomes the level above - and place the items on the
		 * child, which is a verbatim copy and therefore wants them at the
		 * very same offset.
		 *
		 * The root's lock is dropped while that happens, because the child's
		 * own split will have to take it to insert ITS downlink and buffer
		 * locks are not reentrant.  Writers of one key serialise on the
		 * entry's directory leaf (DESIGN.md §22), so nothing else can be in
		 * this tree; a reader that looks in between sees the push-down, which
		 * is already on disk, and descends.  The caller gets its buffer back
		 * locked as it handed it over - with an EXCLUSIVE lock, which is what
		 * a cleanup lock decays to here: the page it holds is the new ROOT,
		 * an internal page that holds no TIDs at all.
		 *
		 * The CHILD, on the other hand, comes back from the push-down still
		 * locked, and stays locked until the items are placed: when this is
		 * VACUUM re-placing a container it filtered (lion_vacuum_regrow()),
		 * the push-down has just copied the UNFILTERED container onto the
		 * child, and a reader that could lock the child in between would copy
		 * the dead TIDs, keep its pin, and have them removed under it by the
		 * write below, which takes no cleanup lock (DESIGN.md §11).  Nobody
		 * but this backend has ever locked the child, so nobody holds a copy
		 * of what is on it.
		 */
		Buffer		cbuf;

		cbuf = lion_posting_root_pushdown(index, heaprel, buf, tree->entrybuf,
										  tree->entryoff, tree->entry);

		LockBuffer(buf, BUFFER_LOCK_UNLOCK);

		/*
		 * Test hook: the push-down is on disk and the root is unlocked; the
		 * child is not (see above).  test/isolation/vacuum_regrow_pushdown.spec
		 * parks VACUUM's regrow here and sends a count's descent at the child.
		 */
		LION_INJECTION_POINT("lion-posting-pushdown-child");

		lion_tree_put_items_locked(index, heaprel, cbuf, tree, off, replace,
								   items, nitems, false);

		UnlockReleaseBuffer(cbuf);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		return;
	}

	for (i = 0; i < nitems; i++)
	{
		sizes[i] = lion_item_size(items[i]);
		need += MAXALIGN(sizes[i]) + sizeof(ItemIdData);
	}

	/*
	 * The record deletes items by their line pointers and moves them to N by
	 * them, so no two of them may overlap (lion_page_check_items()): one that
	 * reached into another would have that one overwritten when it goes, or
	 * the items would not fit N inside the record - a PANIC in rmgr mode.
	 */
	lion_page_check_items(index, page, blk);

	/* Copy out the items that are going to move, before touching the page. */
	if (nmove > 0)
	{
		Size		used = 0;
		Size		budget = 0;

		movebuf = (char *) palloc(BLCKSZ);
		movelen = (Size *) palloc(sizeof(Size) * nmove);
		moveptr = (char **) palloc(sizeof(char *) * nmove);

		for (i = 0; i < nmove; i++)
		{
			ItemId		iid = PageGetItemId(page, firstright + i);
			Size		sz = ItemIdGetLength(iid);
			const LionContainer *item;

			/*
			 * Items are data (lion_page_item_fetch()), and so are the line
			 * pointers' lengths.  What moves has to fit N, an empty page,
			 * line pointers and all, or the record fails half way through
			 * - which the check above already makes sure of; this one keeps
			 * movebuf, a page, from being overrun whatever the page holds.
			 */
			item = lion_tree_item_fetch(index, tree, page, blk, firstright + i);
			budget += MAXALIGN(sz) + sizeof(ItemIdData);
			if (unlikely(budget > (Size) LION_PAGE_CAPACITY))
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("lion index \"%s\": the items of container page %u add up to more than a page",
								RelationGetRelationName(index), blk)));
			memcpy(movebuf + used, item, sz);
			moveptr[i] = movebuf + used;
			movelen[i] = sz;
			used += MAXALIGN(sz);
		}

		/* the separator of the page those items go to */
		firstmoved = lion_item_first_ckey((const LionContainer *) moveptr[0]);
	}

	if (ndel > 0)
	{
		delofs = (OffsetNumber *) palloc(sizeof(OffsetNumber) * ndel);
		for (i = 0; i < ndel; i++)
			delofs[i] = delfirst + i;
	}

	/*
	 * Does P still hold the new items once the moved ones are gone?
	 *
	 * The question used to be asked of the page itself, in the middle of the
	 * record, and the second new page was allocated there if the answer was
	 * no.  DESIGN.md §25 does not allow that any more - an rmgr-mode record
	 * runs in a critical section, where extending the relation would turn a
	 * full disk into a PANIC - so it is asked HERE, of a private copy that
	 * the deletion is rehearsed on.  A copy rather than arithmetic because
	 * PageIndexMultiDelete() compacts, and what a compaction recovers is
	 * exactly what the arithmetic would have to guess at.  A split copies a
	 * page once; the hot path (an in-place member insert) copies nothing at
	 * all, which is the whole point of the section.
	 */
	trial = (PGAlignedBlock *) palloc(sizeof(PGAlignedBlock));
	memcpy(trial->data, page, BLCKSZ);
	if (ndel > 0)
		PageIndexMultiDelete((Page) trial->data, delofs, ndel);
	needm = PageGetExactFreeSpace((Page) trial->data) < need;

	/* Every page this record needs, taken before the record opens (§25). */
	if (nmove > 0)
	{
		nbuf = lion_alloc_page(index, heaprel, true);
		nblk = BufferGetBlockNumber(nbuf);
	}
	if (needm)
	{
		mbuf = lion_alloc_page(index, heaprel, true);
		mblk = BufferGetBlockNumber(mbuf);
	}

	xstate = lion_wal_begin(index);
	pP = lion_wal_register_buffer(xstate, buf, LION_WALBUF_STD);

	if (BufferIsValid(nbuf))
	{
		pN = lion_wal_init_buffer(xstate, nbuf, LION_PAGE_CONTAINER | tree->kind);
		lion_page_set_owner(pN, tree->hash, tree->root);
	}
	if (BufferIsValid(mbuf))
	{
		pM = lion_wal_init_buffer(xstate, mbuf, LION_PAGE_CONTAINER | tree->kind);
		lion_page_set_owner(pM, tree->hash, tree->root);
	}

	if (ndel > 0)
	{
		PageIndexMultiDelete(pP, delofs, ndel);
		lion_wal_op(xstate, pP, LION_OP_MULTIDEL, 0, (uint16) ndel, delofs,
					sizeof(OffsetNumber) * ndel);
	}

	/* pM is the page needm asked for, and pN the one the moved items need */
	if (pM == NULL)
	{
		Assert(PageGetExactFreeSpace(pP) >= need);
		for (i = 0; i < nitems; i++)
		{
			OffsetNumber noff = PageAddItemExtended(pP, items[i], sizes[i],
													InvalidOffsetNumber, 0);

			if (noff == InvalidOffsetNumber)
				elog(ERROR, "lion index: failed to place item after split");
			lion_wal_op(xstate, pP, LION_OP_ADD, noff, 0, items[i], sizes[i]);
		}
	}
	else
	{
		/* The items get a page of their own, linked immediately after P. */
		for (i = 0; i < nitems; i++)
		{
			OffsetNumber noff = PageAddItemExtended(pM, items[i], sizes[i],
													InvalidOffsetNumber, 0);

			if (noff == InvalidOffsetNumber)
				elog(ERROR, "lion index: failed to place item on new page");
			lion_wal_op(xstate, pM, LION_OP_ADD, noff, 0, items[i], sizes[i]);
		}
	}

	if (pN != NULL)
	{
		Assert(nmove > 0);
		for (i = 0; i < nmove; i++)
		{
			OffsetNumber noff = PageAddItemExtended(pN, moveptr[i], movelen[i],
													InvalidOffsetNumber, 0);

			if (noff == InvalidOffsetNumber)
				elog(ERROR, "lion index: failed to move container during split");
			lion_wal_op(xstate, pN, LION_OP_ADD, noff, 0, moveptr[i],
						movelen[i]);
		}
	}

	lion_page_update_minmax(pP);
	if (pN != NULL)
		lion_page_update_minmax(pN);
	if (pM != NULL)
		lion_page_update_minmax(pM);

	/*
	 * Relink: P -> [M] -> [N] -> oldright.  Each brand new page is one the
	 * parent has no downlink for yet, so the page to its LEFT is flagged and
	 * stays EXCLUSIVE until that downlink is in (DESIGN.md §22).
	 */
	{
		BlockNumber after_m = BlockNumberIsValid(nblk) ? nblk : oldright;

		if (pM != NULL)
		{
			LionPageGetOpaque(pM)->rightlink = after_m;
			LionPageGetOpaque(pP)->rightlink = mblk;
			LionPageGetOpaque(pP)->flags |= LION_PAGE_INCOMPLETE_SPLIT;
			if (pN != NULL)
				LionPageGetOpaque(pM)->flags |= LION_PAGE_INCOMPLETE_SPLIT;
		}
		else
		{
			LionPageGetOpaque(pP)->rightlink = after_m;
			if (pN != NULL)
				LionPageGetOpaque(pP)->flags |= LION_PAGE_INCOMPLETE_SPLIT;
		}

		if (pN != NULL)
			LionPageGetOpaque(pN)->rightlink = oldright;
	}

	lion_wal_log_special(xstate, pP);
	if (pN != NULL)
		lion_wal_log_special(xstate, pN);
	if (pM != NULL)
		lion_wal_log_special(xstate, pM);

	/* If P was the tail, the chain has a new last page. */
	if (tree->entry != NULL && tree->entry->tail == blk)
	{
		BlockNumber newtail = BlockNumberIsValid(nblk) ? nblk : mblk;

		Assert(!BlockNumberIsValid(oldright));
		if (BlockNumberIsValid(newtail))
			tree->entry->tail = newtail;
	}

	/* The entry always travels with the container change. */
	lion_tree_put_entry(index, xstate, tree);

	lion_wal_finish(xstate, LION_XLOG_SPLIT);

	if (BufferIsValid(nbuf))
		UnlockReleaseBuffer(nbuf);

	if (movebuf)
	{
		pfree(movebuf);
		pfree(movelen);
		pfree(moveptr);
	}
	if (delofs)
		pfree(delofs);
	pfree(trial);

	/*
	 * Test hook: the split is on disk and the left page says so, but its right
	 * sibling has no downlink yet.  test/recovery/run.sh crashes the server
	 * here and proves that the next writer's descent repairs it.  Compiles to
	 * nothing without --enable-injection-points.
	 */
	if (LionPageIncompleteSplit(BufferGetPage(buf)))
		LION_INJECTION_POINT("lion-posting-split-incomplete");

	/*
	 * The downlinks, left to right: M's first, because a descent cannot reach
	 * M before M has one and therefore cannot repair M before P.  Each
	 * separator is the first container key the split put on the sibling, and
	 * is handed over rather than read back off the page, because M is still
	 * held EXCLUSIVE here.
	 */
	if (LionPageIncompleteSplit(BufferGetPage(buf)))
	{
		uint32		sep = BlockNumberIsValid(mblk) ?
			lion_item_first_ckey(items[0]) : firstmoved;

		lion_posting_finish_split_sep(index, heaprel, tree->hash, tree->root,
									  buf, &sep);
	}

	if (BufferIsValid(mbuf))
	{
		if (LionPageIncompleteSplit(BufferGetPage(mbuf)))
			lion_posting_finish_split_sep(index, heaprel, tree->hash,
										  tree->root, mbuf, &firstmoved);
		UnlockReleaseBuffer(mbuf);
	}
}
