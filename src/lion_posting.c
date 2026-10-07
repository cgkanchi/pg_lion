/*-------------------------------------------------------------------------
 *
 * lion_posting.c
 *		The per-key posting tree of the lion index (DESIGN.md §22).
 *
 * A CHAIN entry's posting set used to be a rightlinked chain of container
 * pages, which can only be walked.  It is now a B-tree over container keys -
 * GIN's posting-tree shape - whose LEAVES are those very container pages,
 * still rightlinked in ckey order, so every sequential reader (the bitmap
 * scan, the single-set count, VACUUM) does exactly what it did before, while
 * an intersection can SEEK to a container key instead of streaming everything
 * below it.
 *
 * The shape, and the four rules that make it work:
 *
 * 1. THE ROOT NEVER MOVES.  The entry's `head` is the root block, and it is
 *	  also the owner stamp of every page of the set (DESIGN.md §18), which is
 *	  the identity a reader that holds nothing but a head block validates
 *	  against.  So a root split does not allocate a new root: it PUSHES THE
 *	  ROOT DOWN - the root's items go to a brand new child and the root block
 *	  becomes an internal page with one downlink to it - exactly as nbtree and
 *	  GIN keep their root in place.  The entry is therefore never rewritten for
 *	  a root split, "a head block is never recycled as a head" stays true, and
 *	  no record ever has to carry both the entry leaf and four tree pages.
 *
 * 2. WRITERS OF ONE KEY SERIALISE ON THAT KEY'S DIRECTORY LEAF.  Every path
 *	  that changes a posting tree - aminsert, an INLINE spill, VACUUM's regrow
 *	  and its apply step - holds the directory leaf that carries the entry
 *	  EXCLUSIVE while it does (DESIGN.md §5, §11, §21).  So no split of a
 *	  posting tree can be in flight while another writer descends it, and a
 *	  write descent needs no move-right at the leaf level at all: the
 *	  separators say exactly which leaf owns a container key.  Only READERS
 *	  race with splits, and a reader that lands on a leaf whose maxckey no
 *	  longer reaches its key simply moves right, which is where a split put the
 *	  items.
 *
 * 3. NO COUPLING DOWNWARDS.  A descent releases a page before it locks the
 *	  child, and a split holds the child while it locks the parent.  That is
 *	  nbtree's rule and §21's, and the reason is not thrift: coupling downwards
 *	  would close a cycle that buffer content locks have no detector for.
 *
 * 4. A SPLIT IS TWO RECORDS AND AN IDEMPOTENT REPAIR.  The first record splits
 *	  the page and flags it LION_PAGE_INCOMPLETE_SPLIT; the downlink goes into
 *	  the parent next, and a third, tiny record clears the flag.  A record
 *	  takes four buffers and a leaf split already needs (left, new right, a
 *	  second new page, the entry leaf), so the flag cannot ride along with the
 *	  parent insertion the way nbtree does it.  A crash anywhere in between
 *	  leaves the flag set, and so does an ERROR there - the parent insertion
 *	  may have to split the parent, which allocates a page, which can fail;
 *	  the next WRITE descent that meets it finishes the split before touching
 *	  the page, and the repair looks for the downlink before adding one, so
 *	  doing it twice is harmless.  Readers never notice: the right link takes
 *	  them to the items.
 *
 *	  Two writers reach a leaf WITHOUT descending - an insert through the
 *	  append hint (the entry's `tail`) and VACUUM, which walks the right links
 *	  - and a descent is the only thing that repairs as it goes, so more rules
 *	  close the gap.  The append hint is used only for changes that stay
 *	  inside the page; anything that may split the page descends
 *	  (lion_insert.c).  A page that has NO downlink - the right half of an
 *	  unfinished split, reached through the hint or a right link - finds, when
 *	  its own split needs its parent, the flagged page to its left and
 *	  finishes that first (lion_posting_find_parent(), nbtree's
 *	  _bt_getstackbuf() doing _bt_finish_split()).  And a page that is itself
 *	  flagged finishes that split before it is split again
 *	  (lion_split_and_place()): a second split would put its new page between
 *	  the two halves of the first, and the first one's right half would never
 *	  get a downlink.
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

/* DESIGN.md §27: EXPLAIN ANALYZE's "Join Key Posting Pages Read". */
int64		lion_posting_pages_read = 0;

/*
 * THE PAGES A SPLIT REPAIR HOLDS.  Finishing a split
 * (lion_posting_finish_split_ext()) recurses: UP, when the page that takes
 * the downlink is flagged itself or has to split, and SIDEWAYS, when the page
 * has no downlink and lion_posting_adopt() finishes the split to its left
 * first - whose right half may have none either.  Every frame keeps its page
 * EXCLUSIVE until it returns, so a walk deep inside holds one page per
 * enclosing repair, several on one level when adopt nests.  Each frame links
 * the page it finishes into this list, which lives on the frames' own stacks,
 * innermost first, and every page a repair walk is about to lock is looked up
 * in it first (lion_posting_held_buffer()).
 */
typedef struct LionPostingHeld
{
	Buffer		buf;			/* held EXCLUSIVE by an enclosing frame */
	const struct LionPostingHeld *next; /* the frame outside it, or NULL */
} LionPostingHeld;

static Buffer lion_posting_find_parent(Relation index, Relation heaprel,
									   uint32 hash, BlockNumber head,
									   Buffer childbuf,
									   uint32 routeckey, bool haveroute,
									   OffsetNumber *offp,
									   const LionPostingHeld *held);
static void lion_posting_place_pivot(Relation index, Relation heaprel,
									 uint32 hash, BlockNumber head, Buffer buf,
									 OffsetNumber off,
									 const LionPostingPivot *pivot,
									 const LionPostingHeld *held);
static void lion_posting_finish_split_ext(Relation index, Relation heaprel,
										  uint32 hash, BlockNumber head,
										  Buffer pbuf, const uint32 *knownsep,
										  const LionPostingHeld *held);

/* ---------------------------------------------------------------------
 * Page helpers
 * --------------------------------------------------------------------- */

/*
 * POSTING-TREE LINKS ARE DATA, as the directory's are (DESIGN.md §21,
 * "Readers", and §22).  Every block number a descent or a walk here follows
 * was read from a page, and every pivot it reads is wherever a line pointer
 * says, so a damaged page is checked for exactly what would otherwise be
 * followed blindly, and refused with ERRCODE_INDEX_CORRUPTED:
 *
 *	- a line pointer is one whole pivot inside the page before the pivot is
 *	  read, and a non-rightmost internal page has the high key every reader
 *	  takes as its first item (lion_posting_pivot_at(),
 *	  lion_posting_highkey_at());
 *	- a downlink names a valid block (lion_posting_downlink()):
 *	  InvalidBlockNumber is P_NEW, and ReadBuffer() EXTENDS the index when
 *	  asked for it.  A block past the end fails in ReadBuffer() by itself;
 *	- a child is one level below its parent and a right sibling at its page's
 *	  level.  Only the root ever changes level (a push-down, rule 1), and the
 *	  root is nobody's child and nobody's sibling, so this is exact - and it is
 *	  what makes a descent end: a downlink to the root or to the page itself
 *	  used to send lion_posting_search() round for ever;
 *	- a page reached through a link is not one this backend already holds,
 *	  checked BEFORE it is locked: a content lock is not reentrant, and taking
 *	  one twice waits for this very backend for ever (lion_posting_held()).
 *	  In the split repair that is every page any frame of its recursion
 *	  holds (LionPostingHeld);
 *	- and a walk right along a level ends (LionRightWalk, lion.h).
 *
 * "For ever" was UNCANCELLABLE on every writer's path: a writer holds its
 * key's directory leaf throughout (rule 2), and a held content lock holds
 * interrupts off, so neither statement_timeout nor pg_terminate_backend()
 * could stop it, and every other writer of the leaf, VACUUM and the
 * checkpointer queued behind it.  Readers now check for interrupts between
 * pages, where they hold nothing.
 */

/*
 * The pivot at off on internal posting page blk, once off is an item the page
 * has and its line pointer puts one whole pivot inside the item space - a
 * damaged one could otherwise put it anywhere up to 32 kB past the page
 * (lion_verify_itemid() makes the same test, as amcheck's
 * PageGetItemIdCareful() does).  A few comparisons per pivot read, about a
 * dozen per page a descent's binary search passes.
 */
static LionPostingPivot *
lion_posting_pivot_at(Relation index, Page page, BlockNumber blk,
					  OffsetNumber off)
{
	PageHeader	phdr = (PageHeader) page;
	ItemId		iid;

	if (unlikely(off < FirstOffsetNumber || off > PageGetMaxOffsetNumber(page)))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": internal posting page %u has no item %u",
						RelationGetRelationName(index), blk, off)));

	iid = PageGetItemId(page, off);
	if (unlikely(!ItemIdIsNormal(iid) ||
				 ItemIdGetLength(iid) != LION_POSTING_PIVOT_SIZE ||
				 ItemIdGetOffset(iid) < phdr->pd_upper ||
				 ItemIdGetOffset(iid) + LION_POSTING_PIVOT_SIZE > phdr->pd_special))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": item %u of internal posting page %u is %u bytes at offset %u, not a pivot inside the page",
						RelationGetRelationName(index), off, blk,
						ItemIdGetLength(iid), ItemIdGetOffset(iid))));

	return (LionPostingPivot *) PageGetItem(page, iid);
}

/*
 * The high key of internal posting page blk, which is not rightmost.  A split
 * writes one on every such page and nothing takes it away, so a page without
 * is damaged, and lion_posting_highkey() on it would take a line pointer past
 * pd_lower for one (the check lion_dir_check_page() makes for the directory).
 */
static LionPostingPivot *
lion_posting_highkey_at(Relation index, Page page, BlockNumber blk)
{
	Assert(LionPageGetOpaque(page)->level > 0 && !LionPageIsRightmost(page));

	if (unlikely(PageGetMaxOffsetNumber(page) < FirstOffsetNumber))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": internal posting page %u has a right sibling but no high key",
						RelationGetRelationName(index), blk)));

	return lion_posting_pivot_at(index, page, blk, FirstOffsetNumber);
}

BlockNumber
lion_posting_downlink(Relation index, Page page, BlockNumber blk,
					  OffsetNumber off)
{
	BlockNumber child;

	/*
	 * off comes from a binary search, which answers past the last item on a
	 * page with no downlink - and an internal page is never left without one.
	 */
	if (unlikely(off < lion_posting_first_data(page) ||
				 off > PageGetMaxOffsetNumber(page)))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": internal posting page %u has no downlink",
						RelationGetRelationName(index), blk)));

	child = lion_posting_pivot_at(index, page, blk, off)->child;
	if (unlikely(!BlockNumberIsValid(child)))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": internal posting page %u has a downlink to an invalid block number",
						RelationGetRelationName(index), blk)));

	return child;
}

/* The page blk of a posting set is not at the level the link to it promises. */
static void
lion_posting_check_level(Relation index, Page page, BlockNumber blk,
						 int expected)
{
	if (unlikely((int) LionPageGetOpaque(page)->level != expected))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": posting block %u is at level %u, but the link to it expects level %d",
						RelationGetRelationName(index), blk,
						LionPageGetOpaque(page)->level, expected)));
}

/*
 * A link of the posting set at head leads to blk, which this backend already
 * holds locked.  Only damage makes such a link, and following it would lock
 * blk a second time and wait for itself for ever, so it is refused before the
 * lock.  The pages checked are the page a walk steps from and every page the
 * enclosing split repairs hold (LionPostingHeld), however far up and sideways
 * the repair has recursed (DESIGN.md §22, 2026-09-28 addendum).
 */
static void
lion_posting_held(Relation index, BlockNumber head, BlockNumber blk)
{
	ereport(ERROR,
			(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg("lion index \"%s\": a link of the posting set at %u leads to block %u, which this backend already holds",
					RelationGetRelationName(index), head, blk)));
}

/*
 * blk's buffer when an enclosing split repair holds it, else InvalidBuffer.
 * The list is one link per frame of the recursion, so this is a handful of
 * comparisons, and it is asked only on the repair's own walks.
 */
static Buffer
lion_posting_held_buffer(const LionPostingHeld *held, BlockNumber blk)
{
	for (; held != NULL; held = held->next)
	{
		if (BufferGetBlockNumber(held->buf) == blk)
			return held->buf;
	}
	return InvalidBuffer;
}

void
lion_rightwalk_exceeded(Relation index, LionRightWalk *walk, BlockNumber blk)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(index);

	if (walk->steps > nblocks)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": the right links from block %u go round in a cycle",
						RelationGetRelationName(index), blk),
				 errdetail("A walk right along one level took %u steps in an index of %u blocks.",
						   walk->steps, nblocks)));

	/* Not yet: the walk may go as far as the index is long, as of now. */
	walk->limit = nblocks;
}

/* blk's right link names blk itself or the meta page (see lion.h). */
void
lion_rightwalk_badlink(Relation index, BlockNumber blk, BlockNumber next)
{
	if (next == blk)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": the right link of block %u is the block itself",
						RelationGetRelationName(index), blk)));
	ereport(ERROR,
			(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg("lion index \"%s\": the right link of block %u is the meta page",
					RelationGetRelationName(index), blk)));
}

/*
 * Read blk, lock it in lockmode and check that it is still a page of the
 * posting set rooted at head (DESIGN.md §18).
 *
 * A mismatch means the set was deleted after the caller copied the entry,
 * which VACUUM only does once the set held nothing visible to anyone: a
 * READER treats it as the end of the set and gets InvalidBuffer back.  Every
 * WRITER holds the entry's directory leaf, so the set cannot go away under it
 * and a mismatch is corruption - those callers pass strict.
 */
static Buffer
lion_posting_getbuf(Relation index, BlockNumber blk, BlockNumber head,
					int lockmode, bool strict)
{
	Buffer		buf;
	Page		page;

	if (!BlockNumberIsValid(blk))
	{
		if (strict)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("lion index \"%s\": posting set at %u has no page there",
							RelationGetRelationName(index), head)));
		return InvalidBuffer;
	}

	lion_posting_pages_read++;
	buf = ReadBuffer(index, blk);
	LockBuffer(buf, lockmode);
	page = BufferGetPage(buf);

	if (!lion_page_owns_tree(page, head))
	{
		UnlockReleaseBuffer(buf);
		if (strict)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("lion index \"%s\": block %u is not a page of the posting set at %u",
							RelationGetRelationName(index), blk, head)));
		return InvalidBuffer;
	}

	return buf;
}

/*
 * The downlink of the child that owns ckey: the LAST separator at or below
 * it.  Separators are non-decreasing (a page both of whose sides are empty
 * can be given the separator of its left neighbour, see
 * lion_posting_finish_split()), so "the last one" and not "any one" is what
 * routes a key past an emptied page.
 *
 * A key below every separator on the page takes the leftmost child, which is
 * conservative rather than wrong: the search then lands left of its key and
 * the move-right rule walks to it.  blk is the page's block, for the ERROR a
 * damaged pivot raises; an offset past the last item comes back as it is, for
 * lion_posting_downlink() to refuse.
 *
 * With `before` it is the last separator strictly BELOW ckey instead: the
 * child that holds what sorts just before ckey, which is where a position
 * tree's reader of ckey starts (lion_posting_search_before()).
 */
static OffsetNumber
lion_posting_downlink_off(Relation index, Page page, BlockNumber blk,
						  uint32 ckey, bool before)
{
	OffsetNumber first = lion_posting_first_data(page);
	OffsetNumber lo = first;
	OffsetNumber hi = OffsetNumberNext(PageGetMaxOffsetNumber(page));

	/* first offset whose separator is above ckey (at or above, before) */
	while (lo < hi)
	{
		OffsetNumber mid = lo + (hi - lo) / 2;
		uint32		sep = lion_posting_pivot_at(index, page, blk, mid)->ckey;

		if (before ? sep >= ckey : sep > ckey)
			hi = mid;
		else
			lo = OffsetNumberNext(mid);
	}

	if (lo <= first)
		return first;
	return OffsetNumberPrev(lo);
}

/*
 * Move to the right sibling, releasing the page we came from first.  walk is
 * the walk the step belongs to; the caller checks the sibling's level.
 */
static Buffer
lion_posting_step_right(Relation index, Buffer buf, BlockNumber head,
						int lockmode, bool strict, LionRightWalk *walk)
{
	BlockNumber next = LionPageGetOpaque(BufferGetPage(buf))->rightlink;

	lion_rightwalk_step(index, walk, BufferGetBlockNumber(buf), next);
	UnlockReleaseBuffer(buf);

	/* Between the pages, where no content lock holds interrupts off. */
	CHECK_FOR_INTERRUPTS();
	return lion_posting_getbuf(index, next, head, lockmode, strict);
}

/*
 * Count one more restart of the descent of the set at head, which met blk at
 * `level` where its link promised `expected`, and refuse it once there have
 * been more than root push-downs can explain (see lion_posting_search()).
 * Called with nothing of the descent locked.
 */
static void
lion_posting_restart(Relation index, BlockNumber head, int *restarts,
					 BlockNumber blk, uint16 level, int expected)
{
	if (++(*restarts) > LION_POSTING_MAX_HEIGHT)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": posting block %u is at level %u, but the link to it expects level %d",
						RelationGetRelationName(index), blk, level, expected),
				 errdetail("The descent of the posting set at %u started again %d times.",
						   head, *restarts - 1)));
	CHECK_FOR_INTERRUPTS();
}

/* ---------------------------------------------------------------------
 * The descent
 * --------------------------------------------------------------------- */

static Buffer lion_posting_descend(Relation index, Relation heaprel,
								   uint32 hash, BlockNumber head, uint32 ckey,
								   bool before, int lockmode, bool forwrite);

Buffer
lion_posting_search(Relation index, Relation heaprel, uint32 hash,
					BlockNumber head, uint32 ckey, int lockmode, bool forwrite)
{
	return lion_posting_descend(index, heaprel, hash, head, ckey, false,
								lockmode, forwrite);
}

Buffer
lion_posting_search_before(Relation index, Relation heaprel, uint32 hash,
						   BlockNumber root, uint32 ckey, int lockmode,
						   bool forwrite)
{
	return lion_posting_descend(index, heaprel, hash, root, ckey, true,
								lockmode, forwrite);
}

static Buffer
lion_posting_descend(Relation index, Relation heaprel, uint32 hash,
					 BlockNumber head, uint32 ckey, bool before, int lockmode,
					 bool forwrite)
{
	Buffer		buf;
	Page		page;
	LionRightWalk walk;
	int			restarts = 0;

	Assert(!forwrite || lockmode == BUFFER_LOCK_EXCLUSIVE);

	/*
	 * HOW OFTEN THE DESCENT STARTS AGAIN.  A writer that finishes an
	 * unfinished split starts again each time, and that ends by itself: each
	 * repair clears a flag for good, and nobody sets one meanwhile, because
	 * writers of one key serialise (rule 2).  Every other restart is counted
	 * (lion_posting_restart()).  A one-page root that is no longer a leaf
	 * once it is relocked is a root push-down, and a set's root is pushed
	 * down at most LION_POSTING_MAX_HEIGHT times in its whole life.  A page at
	 * a level its link does not promise is no race at all - only the root
	 * changes level, and the root is nobody's child or sibling - but it is
	 * retried like one, and refused once the restarts are more than
	 * push-downs can explain.  Without the bound a downlink to the root, or
	 * to the page itself, started the descent again for ever, and on the
	 * insert path, where the caller holds the key's directory leaf,
	 * uncancellably.
	 */
restart:
	buf = lion_posting_getbuf(index, head, head, BUFFER_LOCK_SHARE, forwrite);
	if (!BufferIsValid(buf))
		return InvalidBuffer;

	if (LionPageIsPostingLeaf(BufferGetPage(buf)) &&
		lockmode != BUFFER_LOCK_SHARE)
	{
		/*
		 * A one-page set: the root IS the leaf and has to be relocked in the
		 * caller's mode.  A root push-down may have happened in between, and
		 * then the page is no longer a leaf and the descent starts over.
		 */
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		LockBuffer(buf, lockmode);
		page = BufferGetPage(buf);
		if (!lion_page_owns_tree(page, head))
		{
			UnlockReleaseBuffer(buf);
			if (forwrite)
				elog(ERROR, "lion index \"%s\": the posting set at %u went away under a writer",
					 RelationGetRelationName(index), head);
			return InvalidBuffer;
		}
		if (!LionPageIsPostingLeaf(page))
		{
			uint16		rlevel = LionPageGetOpaque(page)->level;

			UnlockReleaseBuffer(buf);
			lion_posting_restart(index, head, &restarts, head, rlevel, 0);
			goto restart;
		}
	}

	lion_rightwalk_init(&walk);
	for (;;)
	{
		uint16		level;
		BlockNumber blk;
		BlockNumber child;
		int			childlock;

		page = BufferGetPage(buf);
		blk = BufferGetBlockNumber(buf);
		level = LionPageGetOpaque(page)->level;

		/*
		 * A writer finishes EVERY unfinished split it meets on the way down,
		 * not only one it ends up standing on: the page to the right of a
		 * flagged one is the page with no downlink, and a writer that stepped
		 * over the flag and then split THAT page would have no parent item to
		 * insert next to.  (lion_posting_find_parent() copes with that too,
		 * by walking the level from where the route leads up to the page and
		 * finishing the splits it passes - but that is a second pass over the
		 * level, and only a writer that reached its leaf without a descent
		 * should ever need it.)  The repair moves items' ownership around, so
		 * the descent starts again rather than guessing where it now stands.
		 */
		if (forwrite && LionPageIncompleteSplit(page))
		{
			if (level > 0)
			{
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);
				LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
				if (!lion_page_owns_tree(BufferGetPage(buf), head))
					elog(ERROR, "lion index \"%s\": the posting set at %u went away under a writer",
						 RelationGetRelationName(index), head);
			}
			if (LionPageIncompleteSplit(BufferGetPage(buf)))
				lion_posting_finish_split(index, heaprel, hash, head, buf);
			UnlockReleaseBuffer(buf);
			CHECK_FOR_INTERRUPTS();
			goto restart;
		}

		/*
		 * An internal page whose high key no longer exceeds ckey (no longer
		 * reaches it, before): move right.
		 */
		if (level > 0 && !LionPageIsRightmost(page) &&
			(before ?
			 lion_posting_highkey_at(index, page, blk)->ckey < ckey :
			 lion_posting_highkey_at(index, page, blk)->ckey <= ckey))
		{
			buf = lion_posting_step_right(index, buf, head, BUFFER_LOCK_SHARE,
										  forwrite, &walk);
			if (!BufferIsValid(buf))
				return InvalidBuffer;
			if (LionPageGetOpaque(BufferGetPage(buf))->level != level)
			{
				BlockNumber rblk = BufferGetBlockNumber(buf);
				uint16		rlevel = LionPageGetOpaque(BufferGetPage(buf))->level;

				UnlockReleaseBuffer(buf);
				lion_posting_restart(index, head, &restarts, rblk, rlevel,
									 level);
				goto restart;
			}
			continue;
		}

		if (level == 0)
		{
			/*
			 * A READER may be standing on a leaf a concurrent split has moved
			 * its container key off; the items only ever go to a page
			 * immediately to the right, so walking right until the page can
			 * hold ckey finds them.  Empty leaves - VACUUM leaves them in the
			 * chain - have maxckey 0 and are walked past for the same reason.
			 *
			 * A WRITER never does this: writers of one key serialise on the
			 * entry's directory leaf (see the file header), so no split can be
			 * in flight and the separators route it to the leaf that OWNS
			 * ckey, which is where the item has to go even when the leaf's own
			 * keys are all below it.
			 */
			if (!forwrite)
			{
				while (LionPageGetOpaque(page)->maxckey < ckey &&
					   !LionPageIsRightmost(page))
				{
					buf = lion_posting_step_right(index, buf, head, lockmode,
												  false, &walk);
					if (!BufferIsValid(buf))
						return InvalidBuffer;
					page = BufferGetPage(buf);

					/*
					 * A leaf's right link names a leaf, and a page above
					 * level 0 met through one is the end of the set, exactly
					 * as a page of another set is (DESIGN.md §22).  This used
					 * to start the descent again, and a damaged link then led
					 * back to the same page every time.
					 */
					if (!LionPageIsPostingLeaf(page))
					{
						UnlockReleaseBuffer(buf);
						return InvalidBuffer;
					}
				}
			}
			return buf;
		}

		child = lion_posting_downlink(index, page, blk,
									  lion_posting_downlink_off(index, page,
																blk, ckey,
																before));
		childlock = (level == 1) ? lockmode : BUFFER_LOCK_SHARE;

		/* Release the parent BEFORE locking the child; see the file header. */
		UnlockReleaseBuffer(buf);

		/*
		 * Here, with nothing of the descent locked, and not after locking the
		 * child, where the lock holds interrupts off (lion_dir_search() does
		 * the same).  A writer's caller still holds the directory leaf, and
		 * that is why the descent also has to end by itself.
		 */
		CHECK_FOR_INTERRUPTS();
		buf = lion_posting_getbuf(index, child, head, childlock, forwrite);
		if (!BufferIsValid(buf))
			return InvalidBuffer;
		if (LionPageGetOpaque(BufferGetPage(buf))->level != level - 1)
		{
			uint16		clevel = LionPageGetOpaque(BufferGetPage(buf))->level;

			UnlockReleaseBuffer(buf);
			lion_posting_restart(index, head, &restarts, child, clevel,
								 level - 1);
			goto restart;
		}
		lion_rightwalk_init(&walk);
	}
}

/*
 * The page at `level` of the posting set rooted at head that the separators
 * route ckey to, locked SHARE: a READER's descent - SHARE locks, the parent
 * released before the child is locked, a move right past a concurrent split
 * at every level, and at the leaves the reader's walk right while a leaf's
 * maxckey is below ckey - that stops at `level` instead of at the leaves.
 * InvalidBuffer when a page does not belong to the set (it was freed, which
 * only VACUUM does) or the set has no such level.
 *
 * lion_index_verify() is the caller (DESIGN.md §7).  A posting page its walk
 * did not reach may be one a writer of that key made after the walk of the
 * set was over, and a search for the page's own first container key landing
 * on it is the proof that it belongs to the tree.  A page at a level its link
 * does not promise starts the descent again, a bounded number of times, as in
 * lion_posting_search(), and then there is no answer; a leaf whose right link
 * leads above level 0 is the end of the set, as it is there.  The links and
 * pivots are checked as every descent checks them, so a damaged one is an
 * ERROR here too.
 */
Buffer
lion_posting_search_level(Relation index, BlockNumber head, uint32 ckey,
						  uint16 level)
{
	int			restarts = 0;
	Buffer		buf;
	Page		page;
	LionRightWalk walk;

restart:
	if (restarts++ > LION_POSTING_MAX_HEIGHT)
		return InvalidBuffer;

	buf = lion_posting_getbuf(index, head, head, BUFFER_LOCK_SHARE, false);
	if (!BufferIsValid(buf))
		return InvalidBuffer;
	if (LionPageGetOpaque(BufferGetPage(buf))->level < level)
	{
		UnlockReleaseBuffer(buf);
		return InvalidBuffer;
	}

	lion_rightwalk_init(&walk);
	for (;;)
	{
		uint16		plevel;
		BlockNumber blk;
		BlockNumber child;

		page = BufferGetPage(buf);
		blk = BufferGetBlockNumber(buf);
		plevel = LionPageGetOpaque(page)->level;

		if (plevel > 0 && !LionPageIsRightmost(page) &&
			lion_posting_highkey_at(index, page, blk)->ckey <= ckey)
		{
			buf = lion_posting_step_right(index, buf, head, BUFFER_LOCK_SHARE,
										  false, &walk);
			if (!BufferIsValid(buf))
				return InvalidBuffer;
			if (LionPageGetOpaque(BufferGetPage(buf))->level != plevel)
			{
				UnlockReleaseBuffer(buf);
				CHECK_FOR_INTERRUPTS();
				goto restart;
			}
			continue;
		}

		if (plevel == level)
		{
			while (plevel == 0 && LionPageGetOpaque(page)->maxckey < ckey &&
				   !LionPageIsRightmost(page))
			{
				buf = lion_posting_step_right(index, buf, head,
											  BUFFER_LOCK_SHARE, false, &walk);
				if (!BufferIsValid(buf))
					return InvalidBuffer;
				page = BufferGetPage(buf);
				if (!LionPageIsPostingLeaf(page))
				{
					UnlockReleaseBuffer(buf);
					return InvalidBuffer;
				}
			}
			return buf;
		}

		child = lion_posting_downlink(index, page, blk,
									  lion_posting_downlink_off(index, page,
																blk, ckey,
																false));

		/* Release the parent BEFORE locking the child; see the file header. */
		UnlockReleaseBuffer(buf);
		CHECK_FOR_INTERRUPTS();
		buf = lion_posting_getbuf(index, child, head, BUFFER_LOCK_SHARE, false);
		if (!BufferIsValid(buf))
			return InvalidBuffer;
		if (LionPageGetOpaque(BufferGetPage(buf))->level != plevel - 1)
		{
			UnlockReleaseBuffer(buf);
			CHECK_FOR_INTERRUPTS();
			goto restart;
		}
		lion_rightwalk_init(&walk);
	}
}

BlockNumber
lion_posting_leftmost_leaf(Relation index, uint32 hash, BlockNumber head)
{
	Buffer		buf;
	BlockNumber blk;

	/*
	 * Container key 0 is the smallest there is, so an ordinary descent for it
	 * takes the leftmost downlink of every level.
	 */
	buf = lion_posting_search(index, NULL, hash, head, 0, BUFFER_LOCK_SHARE,
							  false);
	if (!BufferIsValid(buf))
		return InvalidBlockNumber;

	blk = BufferGetBlockNumber(buf);
	UnlockReleaseBuffer(buf);

	return blk;
}

/*
 * DESIGN.md §4 called this "the container page that owns ckey", and it was a
 * walk from the head; it is a descent now, and it is still the only place a
 * container key is turned into a block.  The caller holds the entry, so the
 * set cannot go away and anything unexpected is corruption.  `tail` is
 * accepted and unused: the append hint lives in the insert path, which takes
 * the tail page EXCLUSIVE directly (lion_insert_lock_chain_page()).
 */
BlockNumber
lion_chain_find_page(Relation index, uint32 hash, BlockNumber head,
					 BlockNumber tail, uint32 ckey)
{
	Buffer		buf;
	BlockNumber blk;

	Assert(BlockNumberIsValid(head) && BlockNumberIsValid(tail));

	buf = lion_posting_search(index, NULL, hash, head, ckey, BUFFER_LOCK_SHARE,
							  false);
	if (!BufferIsValid(buf))
		elog(ERROR, "lion index \"%s\": the posting set at %u is gone",
			 RelationGetRelationName(index), head);

	blk = BufferGetBlockNumber(buf);
	UnlockReleaseBuffer(buf);

	return blk;
}

/* ---------------------------------------------------------------------
 * Root push-down
 * --------------------------------------------------------------------- */

/*
 * Move everything the root holds onto a brand new child and turn the root
 * block itself into the level above, holding one downlink.  The root keeps
 * its block, so the entry's `head` - and with it the owner stamp of every
 * page of the set - is unchanged (see rule 1 in the file header).
 *
 * entrybuf/entry are only needed when the root is a LEAF, because then the
 * set's last leaf changes and `tail` travels in the same record; an internal
 * root's push-down touches no entry field at all and passes InvalidBuffer.
 *
 * What the record writes is the entry AS IT IS ON THE PAGE with `tail`
 * changed, and not the caller's copy: that copy already counts the items the
 * caller is about to place (an insert adds its member to ntids before it
 * places it, VACUUM's regrow subtracts the TIDs it filtered out), and they go
 * onto the child in a LATER record - the child's own split, which allocates
 * pages and can fail, or be cut short by a crash.  Logging the caller's
 * counters here would leave them claiming items that no page holds (the
 * 2026-09-25 review measured "claims 9241 TIDs, but its containers hold 9240"
 * after an ERROR at lion-posting-pushdown-child).  The push-down moves items
 * without changing them, so the counters on the page are exactly right for
 * the tree it leaves behind; the caller's copy gets the new `tail` too, and
 * its counters travel with the record that really places the items.
 *
 * The child is returned still EXCLUSIVE-locked.  Both callers go on to write
 * to it with the root unlocked, and the one that matters is VACUUM's regrow
 * (DESIGN.md §18): there the root was a leaf VACUUM holds a CLEANUP lock on,
 * the container being re-placed is the FILTERED one, and the copy the push-down
 * just moved onto the child still holds the dead TIDs.  Were the child
 * unlocked in between, a reader could descend to it, copy that stale
 * container and keep its pin, and the write that follows - under an ordinary
 * exclusive lock - would remove the dead TIDs from under it, breaking the §11
 * interlock on the primary.  A page nobody else has ever been able to lock
 * cannot be pinned-and-copied by anyone.
 */
static Buffer
lion_posting_pushdown(Relation index, Relation heaprel, Buffer buf,
					  Buffer entrybuf, OffsetNumber entryoff,
					  LionEntryTuple *entry)
{
	Page		page = BufferGetPage(buf);
	BlockNumber head = BufferGetBlockNumber(buf);
	uint32		hash = LionPageGetOpaque(page)->owner_hash;
	uint16		level = LionPageGetOpaque(page)->level;
	PGAlignedBlock *copy;
	Page		cpage;
	OffsetNumber maxoff;
	OffsetNumber off;
	LionWalState *xstate;
	Page		pR;
	Page		pC;
	Buffer		cbuf;
	BlockNumber cblk;
	LionPostingPivot pivot;
	int			nmoved;
	LionEntryTuple *logged = NULL;
	Size		loggedsz = 0;
	uint16		kind = LionPageGetOpaque(page)->flags & LION_PAGE_POSITIONS;

	Assert(LionPageIsContainer(page));
	Assert(!LionPageIncompleteSplit(page));
	Assert(LionPageIsRightmost(page));	/* a root has no sibling */
	Assert(level < LION_POSTING_MAX_HEIGHT);

	/*
	 * The items move by their line pointers, inside the record (see
	 * lion_page_check_items()), so they are checked before anything is
	 * allocated: each as a reader checks it, and all of them for fitting the
	 * child, which the items of an undamaged root always do.  One line pointer
	 * that claimed 4000 bytes for an item of 1816 used to fail the push-down
	 * half way through its record - a PANIC in rmgr mode, and the same PANIC
	 * again at the first insert into the key after recovery (2026-09-29
	 * review).
	 */
	maxoff = PageGetMaxOffsetNumber(page);
	lion_page_check_items(index, page, head);
	for (off = FirstOffsetNumber; off <= maxoff; off++)
	{
		if (level == 0 && kind != 0)
			(void) lion_page_poschunk_fetch(index, page, head, off);
		else if (level == 0)
			(void) lion_page_item_fetch(index, page, head, off);
		else
			(void) lion_posting_pivot_at(index, page, head, off);
	}

	/* Both pages are rebuilt, so work from a private copy of the root. */
	copy = (PGAlignedBlock *) palloc(sizeof(PGAlignedBlock));
	cpage = (Page) copy->data;
	memcpy(cpage, page, BLCKSZ);

	/* The child is allocated before the record opens (DESIGN.md §25). */
	cbuf = lion_alloc_page(index, heaprel, true);
	cblk = BufferGetBlockNumber(cbuf);

	/*
	 * The entry this record writes: the one on the page, which the caller
	 * holds EXCLUSIVE, with only `tail` changed (see the header comment).  It
	 * is copied here because an rmgr-mode record is a critical section, where
	 * nothing may be palloc'd.
	 */
	if (level == 0 && kind == 0)
	{
		Page		epage;
		LionEntryTuple *onpage;

		Assert(entry != NULL && BufferIsValid(entrybuf));
		epage = BufferGetPage(entrybuf);
		onpage = lion_page_entry_fetch(index, epage,
									   BufferGetBlockNumber(entrybuf),
									   entryoff);
		loggedsz = ItemIdGetLength(PageGetItemId(epage, entryoff));
		logged = (LionEntryTuple *) palloc(loggedsz);
		memcpy(logged, onpage, loggedsz);
		if ((logged->flags & LION_ENTRY_CHAIN) == 0 || logged->head != head)
			elog(ERROR, "lion index \"%s\": entry %u on block %u does not own the posting set at %u",
				 RelationGetRelationName(index), entryoff,
				 BufferGetBlockNumber(entrybuf), head);
		logged->tail = cblk;
	}

	xstate = lion_wal_begin(index);

	/*
	 * The root block is REBUILT as the level above, so it is registered as a
	 * page this record initialises: replay zeroes it and rebuilds it from the
	 * one downlink below, with no full-page image at all.
	 */
	pR = lion_wal_register_buffer(xstate, buf, LION_WALBUF_INIT);
	pC = lion_wal_init_buffer(xstate, cbuf, LION_PAGE_CONTAINER | kind);

	/* The child takes the root's items, at the bytes they were allotted. */
	LionPageGetOpaque(pC)->level = level;
	LionPageGetOpaque(pC)->rightlink = InvalidBlockNumber;
	lion_page_set_owner(pC, hash, head);
	nmoved = 0;
	lion_wal_op(xstate, pC, LION_OP_ADDMANY, FirstOffsetNumber, 0, NULL, 0);
	for (off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId		iid = PageGetItemId(cpage, off);
		uint16		isz;

		if (!ItemIdIsUsed(iid))
			continue;
		isz = (uint16) ItemIdGetLength(iid);
		if (PageAddItemExtended(pC, PageGetItem(cpage, iid), isz,
								InvalidOffsetNumber, 0) == InvalidOffsetNumber)
			elog(ERROR, "lion index \"%s\": could not push the root of the posting set at %u down",
				 RelationGetRelationName(index), head);
		lion_wal_op_append(xstate, pC, &isz, sizeof(uint16));
		lion_wal_op_append(xstate, pC, PageGetItem(cpage, iid), isz);
		nmoved++;
	}
	lion_wal_op_count(xstate, pC, (uint16) nmoved);
	if (level == 0)
		lion_page_update_minmax(pC);
	lion_wal_log_special(xstate, pC);

	/* ... and the root block becomes the level above, with one downlink. */
	lion_init_page(pR, LION_PAGE_CONTAINER | kind);
	lion_wal_op(xstate, pR, LION_OP_INIT, 0, LION_PAGE_CONTAINER | kind, NULL, 0);
	LionPageGetOpaque(pR)->level = level + 1;
	lion_page_set_owner(pR, hash, head);
	pivot.ckey = 0;				/* minus infinity: it owns everything */
	pivot.child = cblk;
	{
		OffsetNumber noff = PageAddItemExtended(pR, (char *) &pivot,
												LION_POSTING_PIVOT_SIZE,
												InvalidOffsetNumber, 0);

		if (noff == InvalidOffsetNumber)
			elog(ERROR, "lion index \"%s\": could not build the new root of the posting set at %u",
				 RelationGetRelationName(index), head);
		lion_wal_op(xstate, pR, LION_OP_ADD, noff, 0, &pivot,
					LION_POSTING_PIVOT_SIZE);
	}
	lion_wal_log_special(xstate, pR);

	if (logged != NULL)
	{
		if (!lion_replace_entry(index, xstate, entrybuf, entryoff, logged,
								loggedsz))
			elog(ERROR, "lion index: could not update entry tuple at %u/%u",
				 BufferGetBlockNumber(entrybuf), entryoff);
		entry->tail = cblk;
	}

	lion_wal_finish(xstate, LION_XLOG_SPLIT);
	pfree(copy);
	if (logged != NULL)
		pfree(logged);

	return cbuf;
}

Buffer
lion_posting_root_pushdown(Relation index, Relation heaprel, Buffer buf,
						   Buffer entrybuf, OffsetNumber entryoff,
						   LionEntryTuple *entry)
{
	Assert(LionPageIsPostingLeaf(BufferGetPage(buf)));
	Assert(entry == NULL ?
		   (LionPageGetOpaque(BufferGetPage(buf))->flags & LION_PAGE_POSITIONS) != 0 :
		   BufferGetBlockNumber(buf) == entry->head);

	return lion_posting_pushdown(index, heaprel, buf, entrybuf, entryoff,
								 entry);
}

/* ---------------------------------------------------------------------
 * Placing a downlink
 * --------------------------------------------------------------------- */

/*
 * Fill a freshly initialised INTERNAL posting page with hk (its high key, or
 * NULL when it is rightmost) followed by items[from .. to), logging the lot as
 * one ADDMANY operation (DESIGN.md §25).
 */
static void
lion_posting_fill(Relation index, LionWalState *xstate, Page page,
				  const LionPostingPivot *hk, LionPostingPivot *items,
				  int from, int to)
{
	OffsetNumber off = FirstOffsetNumber;
	uint16		isz = (uint16) LION_POSTING_PIVOT_SIZE;
	int			i;

	if (hk != NULL)
	{
		if (PageAddItemExtended(page, (char *) hk, LION_POSTING_PIVOT_SIZE,
								InvalidOffsetNumber, 0) == InvalidOffsetNumber)
			elog(ERROR, "lion index \"%s\": could not place a posting high key",
				 RelationGetRelationName(index));
		lion_wal_op(xstate, page, LION_OP_ADD, FirstOffsetNumber, 0, hk,
					LION_POSTING_PIVOT_SIZE);
		off = OffsetNumberNext(off);
	}

	lion_wal_op(xstate, page, LION_OP_ADDMANY, off, (uint16) (to - from),
				NULL, 0);
	for (i = from; i < to; i++)
	{
		if (PageAddItemExtended(page, (char *) &items[i],
								LION_POSTING_PIVOT_SIZE, InvalidOffsetNumber,
								0) == InvalidOffsetNumber)
			elog(ERROR, "lion index \"%s\": could not place a posting downlink",
				 RelationGetRelationName(index));
		lion_wal_op_append(xstate, page, &isz, sizeof(uint16));
		lion_wal_op_append(xstate, page, &items[i], LION_POSTING_PIVOT_SIZE);
	}
}

/*
 * Split the internal page buf, which has no room for `newitem` at off.
 *
 * Pivots are fixed size, so the cut is simply the middle: the left half keeps
 * a fresh high key (a copy of the first separator of the right half) and the
 * right half inherits the old one.  The page is flagged and the downlink for
 * the new sibling goes into the parent next, as rule 4 of the file header
 * describes.  buf is held EXCLUSIVE throughout by the caller, and held is
 * what the enclosing repairs hold (LionPostingHeld; NULL outside one).
 */
static void
lion_posting_split_internal(Relation index, Relation heaprel, uint32 hash,
							BlockNumber head, Buffer buf, OffsetNumber off,
							const LionPostingPivot *newitem,
							const LionPostingHeld *held)
{
	Page		page = BufferGetPage(buf);
	uint16		kind = LionPageGetOpaque(page)->flags & LION_PAGE_POSITIONS;
	BlockNumber pblk = BufferGetBlockNumber(buf);
	uint16		level = LionPageGetOpaque(page)->level;
	BlockNumber oldright = LionPageGetOpaque(page)->rightlink;
	bool		rightmost = LionPageIsRightmost(page);
	OffsetNumber first = lion_posting_first_data(page);
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	OffsetNumber o;
	LionPostingPivot *items;
	LionPostingPivot oldhk;
	int			nitems = 0;
	int			k;
	LionWalState *xstate;
	Page		pP;
	Page		pR;
	Buffer		rbuf;
	BlockNumber rblk;
	LionPostingPivot hk;

	Assert(level > 0);
	Assert(pblk != head);
	Assert(!LionPageIncompleteSplit(page));

	/*
	 * The pivots are copied out below, each one checked; that they also add
	 * up to no more than a page is what makes both halves fit (see the cut),
	 * and a page whose line pointers overlapped would otherwise fail to fill
	 * them inside the record.
	 */
	lion_page_check_items(index, page, pblk);

	if (!rightmost)
		oldhk = *lion_posting_highkey_at(index, page, pblk);

	items = (LionPostingPivot *)
		palloc(sizeof(LionPostingPivot) * (maxoff - first + 3));
	for (o = first; o <= maxoff + 1; o++)
	{
		if (o == off)
			items[nitems++] = *newitem;
		if (o > maxoff)
			break;
		if (!ItemIdIsUsed(PageGetItemId(page, o)))
			continue;
		items[nitems++] = *lion_posting_pivot_at(index, page, pblk, o);
	}

	if (nitems < 2)
		elog(ERROR, "lion index \"%s\": internal posting page %u cannot be split",
			 RelationGetRelationName(index), pblk);

	/*
	 * The middle cut always fits, which is why this needs none of the
	 * walk-backs lion_dir_choose_split() does: pivots are FIXED size, so a
	 * page holds at most LION_PAGE_CAPACITY / (MAXALIGN(8) + 4) = 679 of them
	 * and each half of a cut at nitems/2 is at most 341 pivots plus a high
	 * key - a little over half a page.  A page with at least two items can
	 * therefore always be split (DESIGN.md §22).
	 */
	k = nitems / 2;
	Assert((Size) (k + 1) * (MAXALIGN(LION_POSTING_PIVOT_SIZE) +
						 sizeof(ItemIdData)) <= (Size) LION_PAGE_CAPACITY);
	Assert((Size) (nitems - k + 1) * (MAXALIGN(LION_POSTING_PIVOT_SIZE) +
								  sizeof(ItemIdData)) <= (Size) LION_PAGE_CAPACITY);
	hk.ckey = items[k].ckey;
	hk.child = InvalidBlockNumber;

	/* The sibling is allocated before the record opens (DESIGN.md §25). */
	rbuf = lion_alloc_page(index, heaprel, true);
	rblk = BufferGetBlockNumber(rbuf);

	xstate = lion_wal_begin(index);

	/*
	 * Both halves are rebuilt from the pivots copied out above, so both are
	 * registered as pages this record initialises and neither needs a
	 * full-page image.
	 */
	pP = lion_wal_register_buffer(xstate, buf, LION_WALBUF_INIT);
	pR = lion_wal_init_buffer(xstate, rbuf, LION_PAGE_CONTAINER | kind);

	lion_init_page(pP, LION_PAGE_CONTAINER | LION_PAGE_INCOMPLETE_SPLIT | kind);
	lion_wal_op(xstate, pP, LION_OP_INIT, 0,
				LION_PAGE_CONTAINER | LION_PAGE_INCOMPLETE_SPLIT | kind, NULL, 0);
	LionPageGetOpaque(pP)->level = level;
	LionPageGetOpaque(pP)->rightlink = rblk;
	lion_page_set_owner(pP, hash, head);
	lion_posting_fill(index, xstate, pP, &hk, items, 0, k);
	lion_wal_log_special(xstate, pP);

	LionPageGetOpaque(pR)->level = level;
	LionPageGetOpaque(pR)->rightlink = oldright;
	lion_page_set_owner(pR, hash, head);
	lion_posting_fill(index, xstate, pR, rightmost ? NULL : &oldhk, items, k,
					  nitems);
	lion_wal_log_special(xstate, pR);

	lion_wal_finish(xstate, LION_XLOG_SPLIT);
	UnlockReleaseBuffer(rbuf);
	pfree(items);

	lion_posting_finish_split_ext(index, heaprel, hash, head, buf, NULL, held);
}

/*
 * Put `pivot` at off on the internal posting page buf, which the caller holds
 * EXCLUSIVE and which stays locked (the page it ends up on may be a sibling).
 * held is what the enclosing repairs hold; buf is not in it, and goes into it
 * where a repair of buf's own split begins.
 */
static void
lion_posting_place_pivot(Relation index, Relation heaprel, uint32 hash,
						 BlockNumber head, Buffer buf, OffsetNumber off,
						 const LionPostingPivot *pivot,
						 const LionPostingHeld *held)
{
	Page		page = BufferGetPage(buf);

	Assert(LionPageIsPostingInternal(page));

	/*
	 * Never touch a page whose own split never finished: a second split of it
	 * would overwrite the flag and lose the first one's downlink.
	 */
	if (LionPageIncompleteSplit(page))
	{
		lion_posting_finish_split_ext(index, heaprel, hash, head, buf, NULL,
									  held);
		page = BufferGetPage(buf);
	}

	if (PageGetFreeSpace(page) >= MAXALIGN(LION_POSTING_PIVOT_SIZE))
	{
		LionWalState *xstate = lion_wal_begin(index);
		Page		p = lion_wal_register_buffer(xstate, buf, LION_WALBUF_STD);

		if (PageAddItemExtended(p, (char *) pivot, LION_POSTING_PIVOT_SIZE,
								off, 0) == InvalidOffsetNumber)
			elog(ERROR, "lion index \"%s\": could not add a downlink to block %u",
				 RelationGetRelationName(index), BufferGetBlockNumber(buf));
		lion_wal_op(xstate, p, LION_OP_ADD, off, 0, pivot,
					LION_POSTING_PIVOT_SIZE);
		lion_wal_finish(xstate, LION_XLOG_DOWNLINK);
		return;
	}

	if (BufferGetBlockNumber(buf) == head)
	{
		/*
		 * The root is full.  Push it down - which keeps its block, rule 1 -
		 * and place the pivot on the child, which is a verbatim copy of the
		 * old root and therefore wants it at the very same offset.  The root
		 * lock is dropped while that happens, because the child's own split
		 * will have to take it to insert ITS downlink, and buffer locks are
		 * not reentrant.  Nothing else can be modifying this tree (rule 2), so
		 * letting go of the root for that window changes nothing a reader can
		 * see beyond the push-down itself, which is already on disk.  The root
		 * is never in held, which would make the repair below refuse it: only
		 * a page whose split is being finished goes there, and the root's
		 * split is this push-down, which flags nothing.
		 */
		Buffer		cbuf;

		Assert(!BufferIsValid(lion_posting_held_buffer(held, head)));
		cbuf = lion_posting_pushdown(index, heaprel, buf, InvalidBuffer,
									 InvalidOffsetNumber, NULL);

		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		lion_posting_place_pivot(index, heaprel, hash, head, cbuf, off, pivot,
								 held);
		UnlockReleaseBuffer(cbuf);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		return;
	}

	lion_posting_split_internal(index, heaprel, hash, head, buf, off, pivot,
								held);
}

/*
 * Is the downlink for rblk already somewhere in the parent level?  A crash
 * between the two records of a split leaves the flag set with the downlink
 * already there, and the repair must not add a second one.  pbuf is the
 * parent, which the caller holds EXCLUSIVE, and held is every page the repair
 * holds besides it - the page whose split is being finished first.
 */
static bool
lion_posting_downlink_present(Relation index, Buffer pbuf, BlockNumber head,
							  BlockNumber rblk, const LionPostingHeld *held)
{
	Page		page = BufferGetPage(pbuf);
	BlockNumber pblk = BufferGetBlockNumber(pbuf);
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	OffsetNumber off;
	BlockNumber next;
	Buffer		nbuf;
	bool		found = false;

	for (off = lion_posting_first_data(page); off <= maxoff; off++)
	{
		if (lion_posting_pivot_at(index, page, pblk, off)->child == rblk)
			return true;
	}

	/* It could be the first item of the parent's right sibling. */
	next = LionPageGetOpaque(page)->rightlink;
	if (!BlockNumberIsValid(next))
		return false;

	/* ... which is no page the repair holds, unless it is damaged */
	if (next == pblk || BufferIsValid(lion_posting_held_buffer(held, next)))
		lion_posting_held(index, head, next);

	nbuf = lion_posting_getbuf(index, next, head, BUFFER_LOCK_SHARE, false);
	if (!BufferIsValid(nbuf))
		return false;
	{
		Page		np = BufferGetPage(nbuf);
		OffsetNumber nfirst;

		lion_posting_check_level(index, np, next,
								 LionPageGetOpaque(page)->level);
		nfirst = lion_posting_first_data(np);
		found = nfirst <= PageGetMaxOffsetNumber(np) &&
			lion_posting_pivot_at(index, np, next, nfirst)->child == rblk;
	}
	UnlockReleaseBuffer(nbuf);

	return found;
}

void
lion_posting_finish_split(Relation index, Relation heaprel, uint32 hash,
						  BlockNumber head, Buffer pbuf)
{
	lion_posting_finish_split_ext(index, heaprel, hash, head, pbuf, NULL,
								  NULL);
}

void
lion_posting_finish_split_sep(Relation index, Relation heaprel, uint32 hash,
							  BlockNumber head, Buffer pbuf,
							  const uint32 *knownsep)
{
	lion_posting_finish_split_ext(index, heaprel, hash, head, pbuf, knownsep,
								  NULL);
}

/*
 * The one implementation of both, and of the repair's recursion into itself.
 * held is what the enclosing repairs hold (NULL for the outermost), and this
 * frame adds pbuf to it for everything it calls: no walk inside may lock a
 * page any frame holds, because that lock would wait for this very backend
 * for ever (LionPostingHeld).  One page of it is read here as well: when
 * pbuf's right sibling is held - lion_posting_adopt() finishing the split
 * that left its child without a downlink - the separator is read off the
 * held page directly instead of by locking it.
 */
static void
lion_posting_finish_split_ext(Relation index, Relation heaprel, uint32 hash,
							  BlockNumber head, Buffer pbuf,
							  const uint32 *knownsep,
							  const LionPostingHeld *held)
{
	Page		ppage = BufferGetPage(pbuf);
	BlockNumber pblk = BufferGetBlockNumber(pbuf);
	BlockNumber rblk = LionPageGetOpaque(ppage)->rightlink;
	uint16		level = LionPageGetOpaque(ppage)->level;
	OffsetNumber maxoff = PageGetMaxOffsetNumber(ppage);
	OffsetNumber first = lion_posting_first_data(ppage);
	uint32		routeckey = 0;
	bool		haveroute = false;
	bool		havesep = false;
	uint32		sep = 0;
	uint32		leftsep;
	Buffer		parent;
	OffsetNumber off;
	LionWalState *xstate;
	Page		p;
	LionPostingHeld self;

	Assert(LionPageIncompleteSplit(ppage));

	if (pblk == head || !BlockNumberIsValid(rblk))
		elog(ERROR, "lion index \"%s\": block %u is flagged as an incomplete split but has no right sibling to link",
			 RelationGetRelationName(index), pblk);
	if (rblk == pblk)
		lion_posting_held(index, head, rblk);	/* its own right sibling */

	/* From here on pbuf is one of the pages the repair holds. */
	self.buf = pbuf;
	self.next = held;

	/*
	 * A key that routes to THIS page, for finding its downlink.  A page VACUUM
	 * has emptied has none, and then the parent level is scanned from its
	 * leftmost page instead.
	 */
	if (level > 0)
	{
		if (first <= maxoff)
		{
			routeckey = lion_posting_pivot_at(index, ppage, pblk, first)->ckey;
			haveroute = true;
		}
		/* An internal split wrote the separator as this page's high key. */
		sep = lion_posting_highkey_at(index, ppage, pblk)->ckey;
		havesep = true;
	}
	else if (maxoff >= FirstOffsetNumber)
	{
		routeckey = LionPageGetOpaque(ppage)->minckey;
		haveroute = true;
	}

	/*
	 * A leaf split knows the separator - it is the first container key it put
	 * on the new sibling - and says so, which spares this a read of a page the
	 * caller may still be holding EXCLUSIVE (P -> M -> N leaves two of them in
	 * one hand).  Only the crash repair arrives without one.
	 */
	if (!havesep && knownsep != NULL)
	{
		sep = *knownsep;
		havesep = true;
	}

	/*
	 * A LEAF carries no high key, so the separator is the right sibling's own
	 * first container key.  Two degenerate cases have to be covered because a
	 * flag only outlives its split after a crash, and a VACUUM may have run in
	 * between and emptied either page: then any value that keeps the parent's
	 * separators non-decreasing and routes ckeys at or above this page's last
	 * one to the right sibling will do, because the two pages between them own
	 * exactly the range the parent gave the left one.
	 */
	if (!havesep)
	{
		Buffer		heldright = lion_posting_held_buffer(held, rblk);

		if (BufferIsValid(heldright))
		{
			Page		rp = BufferGetPage(heldright);

			lion_posting_check_level(index, rp, rblk, level);
			if (PageGetMaxOffsetNumber(rp) >= FirstOffsetNumber)
			{
				sep = LionPageGetOpaque(rp)->minckey;
				havesep = true;
			}
		}
		else
		{
			Buffer		rbuf = lion_posting_getbuf(index, rblk, head,
												   BUFFER_LOCK_SHARE, true);
			Page		rp = BufferGetPage(rbuf);

			lion_posting_check_level(index, rp, rblk, level);
			if (PageGetMaxOffsetNumber(rp) >= FirstOffsetNumber)
			{
				sep = LionPageGetOpaque(rp)->minckey;
				havesep = true;
			}
			UnlockReleaseBuffer(rbuf);
		}
	}
	if (!havesep && maxoff >= FirstOffsetNumber)
	{
		uint32		mx = LionPageGetOpaque(ppage)->maxckey;

		if (mx < PG_UINT32_MAX)
		{
			sep = mx + 1;
			havesep = true;
		}
	}

	parent = lion_posting_find_parent(index, heaprel, hash, head, pbuf,
									  routeckey, haveroute, &off, &self);
	leftsep = lion_posting_pivot_at(index, BufferGetPage(parent),
									BufferGetBlockNumber(parent), off)->ckey;
	if (!havesep || sep < leftsep)
		sep = leftsep;			/* both halves are empty: they own one range */

	if (!lion_posting_downlink_present(index, parent, head, rblk, &self))
	{
		LionPostingPivot pivot;

		pivot.ckey = sep;
		pivot.child = rblk;
		lion_posting_place_pivot(index, heaprel, hash, head, parent,
								 OffsetNumberNext(off), &pivot, &self);
	}

	UnlockReleaseBuffer(parent);

	xstate = lion_wal_begin(index);
	p = lion_wal_register_buffer(xstate, pbuf, LION_WALBUF_STD);
	LionPageGetOpaque(p)->flags &= ~(uint16) LION_PAGE_INCOMPLETE_SPLIT;
	lion_wal_op(xstate, p, LION_OP_FLAGS, 0, LionPageGetOpaque(p)->flags,
				NULL, 0);
	lion_wal_finish(xstate, LION_XLOG_SPLIT_CLEAR);
}

/*
 * The block of `level` that the separators route routeckey to - or, without a
 * route key, the leftmost block of that level: where a scan of that level
 * for something belonging to routeckey starts.  Reads only, SHARE one page at
 * a time, and holds nothing on return.  The root must be at `level` or above
 * it.
 *
 * `held` is every page the repair that asks holds EXCLUSIVE (LionPostingHeld):
 * among them the child whose parent it looks for, one level below `level`,
 * or - for lion_posting_adopt() - the page of `level` its walk is heading
 * for, and the pages of `level` the adoptions around that one are heading
 * for.  A link to any of them is never followed into a lock, which would wait
 * for this very backend for ever: when the last downlink leads to one, that
 * is the answer without being read, and the caller's walk knows what to do
 * with it; anywhere above `level` it is damage, because a repair holds no
 * page above the level it walks.  Every other page is checked to be at the
 * level its link promises.  Only writers get here, and writers of one key
 * serialise (rule 2), so the shape cannot change under this and a mismatch
 * is corruption.
 */
static BlockNumber
lion_posting_level_start(Relation index, BlockNumber head, uint16 level,
						 uint32 routeckey, bool haveroute,
						 const LionPostingHeld *held)
{
	Buffer		buf;
	BlockNumber blk;
	LionRightWalk walk;

	/* The root is never held by a repair (lion_posting_place_pivot()). */
	if (BufferIsValid(lion_posting_held_buffer(held, head)))
		lion_posting_held(index, head, head);
	buf = lion_posting_getbuf(index, head, head, BUFFER_LOCK_SHARE, true);
	if (LionPageGetOpaque(BufferGetPage(buf))->level < level)
	{
		UnlockReleaseBuffer(buf);
		elog(ERROR, "lion index \"%s\": posting set at %u has no level %u",
			 RelationGetRelationName(index), head, level);
	}

	lion_rightwalk_init(&walk);
	for (;;)
	{
		Page		page = BufferGetPage(buf);
		uint16		plevel = LionPageGetOpaque(page)->level;
		OffsetNumber off;
		BlockNumber child;

		blk = BufferGetBlockNumber(buf);
		if (plevel == level)
			break;

		if (haveroute)
		{
			while (!LionPageIsRightmost(page) &&
				   lion_posting_highkey_at(index, page, blk)->ckey <= routeckey)
			{
				BlockNumber next = LionPageGetOpaque(page)->rightlink;

				if (BufferIsValid(lion_posting_held_buffer(held, next)))
					lion_posting_held(index, head, next);
				buf = lion_posting_step_right(index, buf, head,
											  BUFFER_LOCK_SHARE, true, &walk);
				page = BufferGetPage(buf);
				blk = BufferGetBlockNumber(buf);
				lion_posting_check_level(index, page, blk, plevel);
			}
			off = lion_posting_downlink_off(index, page, blk, routeckey,
											false);
		}
		else
			off = lion_posting_first_data(page);

		child = lion_posting_downlink(index, page, blk, off);
		UnlockReleaseBuffer(buf);
		if (BufferIsValid(lion_posting_held_buffer(held, child)))
		{
			if (plevel - 1 == level)
				return child;
			lion_posting_held(index, head, child);
		}

		/* Between the pages; see lion_posting_search() for what it is worth. */
		CHECK_FOR_INTERRUPTS();
		buf = lion_posting_getbuf(index, child, head, BUFFER_LOCK_SHARE, true);
		lion_posting_check_level(index, BufferGetPage(buf), child, plevel - 1);
		lion_rightwalk_init(&walk);
	}

	UnlockReleaseBuffer(buf);

	return blk;
}

/*
 * Scan `level` rightwards from blk for the downlink of childblk, EXCLUSIVE
 * one page at a time, and return the page that holds it still locked, with
 * *offp its offset - or InvalidBuffer when the level ends first.  The scan
 * holds one page at a time and only ever moves rightwards, so two repairers
 * cannot deadlock.
 *
 * It does hold childblk the whole time, EXCLUSIVE, one level down, and with
 * it every page the enclosing repairs hold (held, which childblk is the
 * first of), and it locks every page it reaches EXCLUSIVE.  None of those is
 * on `level`: a repair holds no page above the level of the child it finds
 * a parent for, because the recursion only ever climbs.  So a link from this
 * level to one of them is damage, and is refused before the lock, which
 * would wait for this backend for ever; every page is checked to be at
 * `level`.
 */
static Buffer
lion_posting_scan_for_downlink(Relation index, Relation heaprel, uint32 hash,
							   BlockNumber head, BlockNumber blk, uint16 level,
							   BlockNumber childblk, OffsetNumber *offp,
							   const LionPostingHeld *held)
{
	LionRightWalk walk;

	Assert(BufferIsValid(lion_posting_held_buffer(held, childblk)));

	lion_rightwalk_init(&walk);
	for (;;)
	{
		Buffer		buf;
		Page		page;
		OffsetNumber off;
		OffsetNumber maxoff;
		BlockNumber next;

		if (BufferIsValid(lion_posting_held_buffer(held, blk)))
			lion_posting_held(index, head, blk);
		buf = lion_posting_getbuf(index, blk, head, BUFFER_LOCK_EXCLUSIVE, true);
		page = BufferGetPage(buf);
		lion_posting_check_level(index, page, blk, level);

		/*
		 * This page's own split may never have finished.  Finish it first: the
		 * downlink we want may be on the page its sibling became, and nothing
		 * may add an item to a page whose flag a later split of it would
		 * overwrite.  The recursion goes one level up at every step, and
		 * sideways only through lion_posting_adopt(), which clears a flag each
		 * time, so it ends.
		 */
		if (LionPageIncompleteSplit(page))
		{
			lion_posting_finish_split_ext(index, heaprel, hash, head, buf, NULL,
										  held);
			page = BufferGetPage(buf);
		}

		maxoff = PageGetMaxOffsetNumber(page);
		for (off = lion_posting_first_data(page); off <= maxoff; off++)
		{
			if (lion_posting_pivot_at(index, page, blk, off)->child == childblk)
			{
				*offp = off;
				return buf;
			}
		}

		next = LionPageGetOpaque(page)->rightlink;
		if (!BlockNumberIsValid(next))
		{
			UnlockReleaseBuffer(buf);
			return InvalidBuffer;
		}
		lion_rightwalk_step(index, &walk, blk, next);
		UnlockReleaseBuffer(buf);
		CHECK_FOR_INTERRUPTS();
		blk = next;
	}
}

/*
 * childbuf, which the caller holds EXCLUSIVE, has no downlink anywhere in the
 * level above.  Only one thing leaves a page like that: a split to its LEFT
 * that wrote its first record and never got to the second (a crash, or an
 * ERROR in the parent insertion), whose flagged left half is somewhere
 * between the page the separators route childbuf's keys to and childbuf
 * itself.  A descent would have met that flag and finished it; childbuf was
 * reached WITHOUT one - through the append hint, or by VACUUM walking the
 * right links - so the finishing is done here: walk the level rightwards from
 * where the route key leads, finish every unfinished split on the way, stop
 * at childbuf.  nbtree's _bt_getstackbuf() finishes the incomplete splits it
 * meets in the same spirit; it only ever meets them one level up because
 * nbtree reaches every page it splits by a descent.
 *
 * The walk locks pages to the LEFT of one this backend holds, which the lock
 * order (DESIGN.md §5) otherwise forbids, and it is safe here for the reason
 * the whole posting tree rests on: every writer of this key holds the entry's
 * directory leaf, which this backend does, so nobody else can be holding a
 * page of this tree and waiting for one to its right.  Readers hold one page
 * at a time and wait for nothing while they do, and VACUUM, when it holds a
 * page of the tree without the directory leaf, only ever asks for the leaf
 * conditionally (lion_vacuum.c, rule 2).
 *
 * TWO WALKS, BECAUSE A REPAIR RUNS INSIDE A REPAIR.  Finishing a flagged page
 * F looks for F's own downlink, and when F has none either - two unfinished
 * splits on one level - this runs again for F, inside, with childbuf still
 * held; and so on, to the left, for as many as there are.  An inner walk
 * must never lock childbuf, and a route key can land RIGHT of the page that
 * holds it (separators are only non-decreasing, see below), so a walk heading
 * for F could pass F and go on to childbuf.  Hence the level is walked twice:
 * first SHARE, finishing nothing, to find out whether childbuf lies ahead of
 * the start at all, and only then again, from the first flagged page that
 * walk saw up to childbuf, finishing.  A split is finished here only for a
 * page LEFT of childbuf, so every page the adoptions around this one hold on
 * this level - each one's own child - lies RIGHT of childbuf, and a walk that
 * meets one of them has passed childbuf exactly as a walk that reaches the
 * end of the level has: it stops there, without locking it, and the next
 * attempt starts from the leftmost page, where childbuf comes before all of
 * them.  Nothing either walk passes changes in between: finishing a split
 * clears a flag on this level and writes only above it, and no other writer
 * is in this tree (rule 2).
 *
 * The pages it passes are taken SHARE and only a flagged one is taken again
 * EXCLUSIVE.  Returns whether it finished anything; false means the level
 * holds no reason for the missing downlink, which is corruption.  held is
 * every page the repair holds, childbuf first.
 */
static bool
lion_posting_adopt(Relation index, Relation heaprel, uint32 hash,
				   BlockNumber head, Buffer childbuf, uint32 routeckey,
				   bool haveroute, const LionPostingHeld *held)
{
	BlockNumber childblk = BufferGetBlockNumber(childbuf);
	uint16		level = LionPageGetOpaque(BufferGetPage(childbuf))->level;
	bool		route = haveroute;

	Assert(lion_posting_held_buffer(held, childblk) == childbuf);

	for (;;)
	{
		BlockNumber blk = lion_posting_level_start(index, head, level,
												   routeckey, route, held);
		BlockNumber firstflagged = InvalidBlockNumber;
		bool		finished = false;
		LionRightWalk walk;

		/*
		 * Does childbuf lie ahead?  The walk ends at childbuf, at the end of
		 * the level, or at a page the adoptions around this one hold, right
		 * of childbuf (see above) - none of which it locks.
		 */
		lion_rightwalk_init(&walk);
		while (BlockNumberIsValid(blk) && blk != childblk &&
			   !BufferIsValid(lion_posting_held_buffer(held, blk)))
		{
			Buffer		buf;
			Page		page;
			BlockNumber next;

			buf = lion_posting_getbuf(index, blk, head, BUFFER_LOCK_SHARE, true);
			page = BufferGetPage(buf);
			if (LionPageGetOpaque(page)->level != level)
			{
				UnlockReleaseBuffer(buf);
				elog(ERROR, "lion index \"%s\": block %u of the posting set at %u is not on level %u with its left neighbour",
					 RelationGetRelationName(index), blk, head, level);
			}
			if (LionPageIncompleteSplit(page) &&
				!BlockNumberIsValid(firstflagged))
				firstflagged = blk;

			next = LionPageGetOpaque(page)->rightlink;
			if (BlockNumberIsValid(next))
				lion_rightwalk_step(index, &walk, blk, next);
			blk = next;
			UnlockReleaseBuffer(buf);
			CHECK_FOR_INTERRUPTS();
		}

		if (blk == childblk)
		{
			/* It does: finish every split from the first flagged page on. */
			blk = firstflagged;
			lion_rightwalk_init(&walk);
			while (BlockNumberIsValid(blk) && blk != childblk)
			{
				Buffer		buf;
				Page		page;
				BlockNumber next;

				/* The walk above passed these pages and met no held one. */
				if (BufferIsValid(lion_posting_held_buffer(held, blk)))
					lion_posting_held(index, head, blk);
				buf = lion_posting_getbuf(index, blk, head, BUFFER_LOCK_SHARE,
										  true);
				page = BufferGetPage(buf);
				lion_posting_check_level(index, page, blk, level);

				if (LionPageIncompleteSplit(page))
				{
					LockBuffer(buf, BUFFER_LOCK_UNLOCK);
					LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
					page = BufferGetPage(buf);
					if (LionPageIncompleteSplit(page))
					{
						lion_posting_finish_split_ext(index, heaprel, hash, head,
													  buf, NULL, held);
						finished = true;
						page = BufferGetPage(buf);
					}
				}

				next = LionPageGetOpaque(page)->rightlink;
				if (BlockNumberIsValid(next))
					lion_rightwalk_step(index, &walk, blk, next);
				blk = next;
				UnlockReleaseBuffer(buf);
				CHECK_FOR_INTERRUPTS();
			}
			return finished;
		}

		/*
		 * The route led PAST childbuf: to the end of the level, or to a page
		 * held around this adoption.  Separators are only non-decreasing - a
		 * page both of whose halves were empty when its split was repaired
		 * shares its left neighbour's (lion_posting_finish_split_ext()), and
		 * an internal split can cut a run of equal separators - so a route
		 * key can land right of a page that holds it; start again from the
		 * leftmost page of the level, which cannot.
		 */
		if (!route)
			return false;
		route = false;
	}
}

/*
 * The page one level above childbuf that holds its downlink, locked
 * EXCLUSIVE, with *offp its offset.  routeckey is a key childbuf holds;
 * without one the scan starts at the leftmost page of that level.  childbuf is
 * held EXCLUSIVE by the caller throughout, and is the first page of held,
 * which is every page the repair holds (LionPostingHeld).
 *
 * A child with no downlink at all is repaired rather than reported: see
 * lion_posting_adopt().  Before that repair existed, an append that took the
 * right half of an unfinished split through the append hint and then filled
 * it hit an ERROR here, and so did every later split of the tail, for good
 * (2026-09-25 review).
 */
static Buffer
lion_posting_find_parent(Relation index, Relation heaprel, uint32 hash,
						 BlockNumber head, Buffer childbuf,
						 uint32 routeckey, bool haveroute,
						 OffsetNumber *offp, const LionPostingHeld *held)
{
	BlockNumber childblk = BufferGetBlockNumber(childbuf);
	uint16		childlevel = LionPageGetOpaque(BufferGetPage(childbuf))->level;
	Buffer		buf;

	Assert(held != NULL && held->buf == childbuf);

	if (childblk == head)
		elog(ERROR, "lion index \"%s\": posting set at %u has no level above %u for block %u",
			 RelationGetRelationName(index), head, childlevel, childblk);

	buf = lion_posting_scan_for_downlink(index, heaprel, hash, head,
										 lion_posting_level_start(index, head,
																  childlevel + 1,
																  routeckey,
																  haveroute,
																  held),
										 childlevel + 1, childblk, offp, held);
	if (BufferIsValid(buf))
		return buf;

	/*
	 * Not there.  Finish whatever left it out, then look again: from where
	 * the route leads, which is where the downlink now is, and failing that
	 * from the leftmost page of the level, because a route key can lead past
	 * a downlink whose separator an emptied neighbour shares (see
	 * lion_posting_adopt()).
	 */
	if (lion_posting_adopt(index, heaprel, hash, head, childbuf, routeckey,
						   haveroute, held))
	{
		buf = lion_posting_scan_for_downlink(index, heaprel, hash, head,
											 lion_posting_level_start(index, head,
																	  childlevel + 1,
																	  routeckey,
																	  haveroute,
																	  held),
											 childlevel + 1, childblk, offp,
											 held);
		if (BufferIsValid(buf))
			return buf;
	}
	if (haveroute)
	{
		buf = lion_posting_scan_for_downlink(index, heaprel, hash, head,
											 lion_posting_level_start(index, head,
																	  childlevel + 1,
																	  0, false,
																	  held),
											 childlevel + 1, childblk, offp,
											 held);
		if (BufferIsValid(buf))
			return buf;
	}

	elog(ERROR, "lion index \"%s\": no downlink for block %u at level %u of the posting set at %u",
		 RelationGetRelationName(index), childblk, childlevel + 1, head);
	return InvalidBuffer;		/* keep the compiler quiet */
}
