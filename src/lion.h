/*-------------------------------------------------------------------------
 * lion.h
 *	  On-disk structures and internal API of the lion index AM.
 *	  See DESIGN.md §4-§7.  The structs in this file are the on-disk format
 *	  and are fixed; the prototypes are the contract between modules.
 *-------------------------------------------------------------------------
 */
#ifndef LION_H
#define LION_H

#include "postgres.h"

#include "lion_compat.h"
#include "lion_slice_enc.h"

#include "access/amapi.h"
#include "access/genam.h"
#include "access/generic_xlog.h"
#include "access/itup.h"
#include "access/transam.h"
#include "common/int.h"
#include "fmgr.h"
#include "nodes/tidbitmap.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "utils/relcache.h"

#include "lion_tid.h"
#include "lion_container.h"
#include "lion_sparse.h"
#include "lion_wal.h"

/* ---------- page special area ---------- */

#define LION_PAGE_ID			0xFF87

#define LION_PAGE_META		0x0001
#define LION_PAGE_BUCKET		0x0002	/* directory LEAF page (DESIGN.md §21) */
#define LION_PAGE_CONTAINER	0x0004
#define LION_PAGE_DELETED	0x0008	/* freed page, DESIGN.md §18 and §21 */
#define LION_PAGE_DIR		0x0010	/* directory INTERNAL page (§21) */
#define LION_PAGE_ROOT		0x0020	/* ... and it is the current root (§21) */
#define LION_PAGE_INCOMPLETE_SPLIT 0x0040	/* its right sibling has no
											 * downlink yet (§21) */
#define LION_PAGE_POSITIONS	0x0080	/* a CONTAINER page of a key's position
									 * tree, holding position chunks
									 * (lion_positions.h), never containers */
#define LION_PAGE_HALF_DEAD	0x0100	/* an emptied directory LEAF whose
									 * downlink is gone and which is being
									 * unlinked (§21, "Page deletion") */
#define LION_PAGE_UNLINKED	0x0200	/* an emptied posting LEAF VACUUM took
									 * out of its set: still the set's page
									 * and linked right, nothing links to it
									 * (§22, "Empty leaves") */

/* The three page KINDS; exactly one of them is set on every page. */
#define LION_PAGE_KINDS \
	(LION_PAGE_META | LION_PAGE_BUCKET | LION_PAGE_CONTAINER | LION_PAGE_DIR)

/*
 * The special area of every page (format version 4, DESIGN.md §18 and §21).
 *
 * owner_hash/owner_head name the posting set a CONTAINER page belongs to, and
 * every reader that reaches a container page through an entry's head or a
 * rightlink checks them.  They exist because a freed page can be reused, and
 * generic WAL cannot raise a recovery conflict: on a standby a reader holding
 * a stale chain link could otherwise land on a page that replay has already
 * given to another key.
 *
 * owner_head is the chain's head block, and it is a chain identity that is
 * unique for the whole life of the index: a chain's head page is the only
 * page lion_alloc_page() is told not to take from the free space map (see
 * lion_entry_spill()), so a head block always comes from extending the
 * relation and no two chains, at any two times, can share one.  That is what
 * lets a reader that knows only the head block - the count cursor, whose
 * LionPostingSet does not carry the entry's hash - validate a page on its own.
 * owner_hash is the second half of the check for the readers that do have the
 * entry in hand, and what verify() uses to prove a chain belongs to its key.
 *
 * Directory and meta pages leave both at their empty values (0 and
 * InvalidBlockNumber), with two exceptions that page deletion makes
 * (DESIGN.md §21, "Page deletion"): a HALF_DEAD leaf keeps in owner_head the
 * next page of its subtree to unlink (InvalidBlockNumber: the leaf itself),
 * and a DELETED directory page keeps its safexid in minckey (the low half)
 * and maxckey (the high half), since its body still holds the high key every
 * reader that lands on it reads (lion_dir_page_safexid()).
 *
 * The meta page keeps in maxckey the pages out of the index's trees that the
 * last ambulkdelete could not hand out yet, nbtree's
 * btm_last_cleanup_num_delpages (lionvacuumcleanup(), DESIGN.md §18,
 * "Reuse"); an index written before it reads 0 there, which means none.
 *
 * An UNLINKED posting leaf (DESIGN.md §22, "Empty leaves") keeps in leftlink
 * the low half of the transaction id from which on no scan can still hold a
 * link to it (lion_page_get_safexid()); a posting leaf leaves leftlink unused
 * otherwise.
 *
 * `level` and `leftlink` belong to the entry directory (DESIGN.md §21): the
 * leaves are level 0 and the root is level `height`, and every directory page
 * is linked to both of its siblings so that a reader that had to move right
 * can tell a concurrent split from corruption and so that VACUUM can walk the
 * leaves in key order.
 *
 * A CONTAINER page uses `level` too, since DESIGN.md §22: a posting set is a
 * B-tree over container keys whose LEAVES are the container pages of §4
 * (level 0, still rightlinked in ckey order) and whose INTERNAL pages hold
 * LionPostingPivot downlinks (level > 0).  Container pages leave `leftlink`
 * at InvalidBlockNumber: a posting page's left sibling is never followed, and
 * leaving it out is what keeps a posting-page split inside the four buffers
 * GenericXLog allows.
 */
typedef struct LionPageOpaqueData
{
	BlockNumber rightlink;		/* next page in chain or InvalidBlockNumber */
	BlockNumber leftlink;		/* previous page, directory pages (§21) */
	uint32		minckey;		/* container pages only; 0 when empty */
	uint32		maxckey;		/* container pages only; 0 when empty */
	uint32		owner_hash;		/* hash of the entry that owns this chain */
	BlockNumber owner_head;		/* head block of that chain */
	uint16		level;			/* directory pages: 0 = leaf (§21) */
	uint16		flags;			/* LION_PAGE_* */
	uint16		page_id;		/* LION_PAGE_ID */
	uint16		unused;			/* pad to a multiple of MAXIMUM_ALIGNOF */
} LionPageOpaqueData;

typedef LionPageOpaqueData *LionPageOpaque;

#define LionPageGetOpaque(page)	((LionPageOpaque) PageGetSpecialPointer(page))
#define LION_SPECIAL_SIZE		MAXALIGN(sizeof(LionPageOpaqueData))
#define LionPageIsMeta(page)		((LionPageGetOpaque(page)->flags & LION_PAGE_META) != 0)
#define LionPageIsBucket(page)	((LionPageGetOpaque(page)->flags & LION_PAGE_BUCKET) != 0)
#define LionPageIsContainer(page) ((LionPageGetOpaque(page)->flags & LION_PAGE_CONTAINER) != 0)
#define LionPageIsDeleted(page)	((LionPageGetOpaque(page)->flags & LION_PAGE_DELETED) != 0)
#define LionPageIsDir(page)		((LionPageGetOpaque(page)->flags & LION_PAGE_DIR) != 0)
#define LionPageIsLeaf(page)		LionPageIsBucket(page)
#define LionPageIsRoot(page)		((LionPageGetOpaque(page)->flags & LION_PAGE_ROOT) != 0)
#define LionPageIsHalfDead(page)	((LionPageGetOpaque(page)->flags & LION_PAGE_HALF_DEAD) != 0)
#define LionPageIsUnlinked(page)	((LionPageGetOpaque(page)->flags & LION_PAGE_UNLINKED) != 0)
#define LionPageIncompleteSplit(page) \
	((LionPageGetOpaque(page)->flags & LION_PAGE_INCOMPLETE_SPLIT) != 0)
#define LionPageIsRightmost(page) \
	(!BlockNumberIsValid(LionPageGetOpaque(page)->rightlink))

/* Total bytes a page can hold in items and line pointers. */
#define LION_PAGE_CAPACITY \
	((Size) (BLCKSZ - SizeOfPageHeaderData - LION_SPECIAL_SIZE))

/*
 * DESIGN.md §2: every container fits on a page - a BITSET is the largest,
 * and it must be an item on an otherwise empty container page, line pointer
 * and all.  That holds from BLCKSZ 8192 up (8136 bytes of capacity for a
 * 4108-byte item) and not below it: at 4096 the capacity is 4040 bytes, and a
 * build or an insert would fail on the first BITSET.  lion_tid.h refuses
 * such a BLCKSZ outright, with a clearer message; this is the property that
 * refusal stands for.
 */
StaticAssertDecl(MAXALIGN(LION_CONTAINER_MAX_SIZE) + sizeof(ItemIdData) <= LION_PAGE_CAPACITY,
				 "pg_lion: a BITSET container does not fit on a page of this BLCKSZ");

/*
 * The body of a DELETED container page: the transaction id from which on no
 * scan can still hold a link to it, exactly as nbtree's BTDeletedPageData (see
 * BTPageIsRecyclable() in access/nbtree.h).  Nothing else is left on the page.
 * A DELETED directory page keeps its safexid in the special area instead
 * (above).
 */
typedef struct LionDeletedPageData
{
	FullTransactionId safexid;
} LionDeletedPageData;

/*
 * Is this page a live container page of the chain whose head block is head?
 *
 * This is the check DESIGN.md §18 requires of every reader that follows an
 * entry's head or a rightlink.  A mismatch is not an error outside verify():
 * it means the chain was freed after the reader copied the entry, which can
 * only happen once the posting set held nothing visible to anyone, so the
 * reader simply stops there.
 */
static inline bool
lion_page_owns(Page page, BlockNumber head)
{
	LionPageOpaque opaque;

	if (PageIsNew(page) || PageGetSpecialSize(page) != LION_SPECIAL_SIZE)
		return false;
	opaque = LionPageGetOpaque(page);

	return opaque->page_id == LION_PAGE_ID &&
		(opaque->flags & (LION_PAGE_CONTAINER | LION_PAGE_DELETED |
						  LION_PAGE_POSITIONS)) == LION_PAGE_CONTAINER &&
		opaque->owner_head == head;
}

/*
 * Is this page a live page of the tree - posting or position - whose root
 * block is root?  The tree code (lion_posting.c) serves both kinds and asks
 * this: a root block is a tree's identity for the whole life of the index
 * (a head is never recycled as a head, lion_alloc_page()), so the owner
 * stamp alone tells the trees apart, and a page of one can never pass for a
 * page of the other.  The kind of a page is in its LION_PAGE_POSITIONS bit,
 * which every page of a tree shares with its root.
 */
static inline bool
lion_page_owns_tree(Page page, BlockNumber root)
{
	LionPageOpaque opaque;

	if (PageIsNew(page) || PageGetSpecialSize(page) != LION_SPECIAL_SIZE)
		return false;
	opaque = LionPageGetOpaque(page);

	return opaque->page_id == LION_PAGE_ID &&
		(opaque->flags & (LION_PAGE_CONTAINER | LION_PAGE_DELETED)) ==
		LION_PAGE_CONTAINER &&
		opaque->owner_head == root;
}

/*
 * Is this page a live page of the position tree whose root block is root?
 * Position pages are container pages with LION_PAGE_POSITIONS set, stamped
 * with their own tree's root rather than the posting set's head, so that
 * neither tree's readers, its sweep nor verify can take a page of the other
 * for one of its own.
 */
static inline bool
lion_page_owns_positions(Page page, BlockNumber root)
{
	LionPageOpaque opaque;

	if (PageIsNew(page) || PageGetSpecialSize(page) != LION_SPECIAL_SIZE)
		return false;
	opaque = LionPageGetOpaque(page);

	return opaque->page_id == LION_PAGE_ID &&
		(opaque->flags & (LION_PAGE_CONTAINER | LION_PAGE_DELETED |
						  LION_PAGE_POSITIONS)) ==
		(LION_PAGE_CONTAINER | LION_PAGE_POSITIONS) &&
		opaque->owner_head == root;
}

/* The same with the entry's hash checked as well, for readers that have it. */
static inline bool
lion_page_owns_entry(Page page, uint32 hash, BlockNumber head)
{
	return lion_page_owns(page, head) &&
		LionPageGetOpaque(page)->owner_hash == hash;
}

/* ---------- the per-key posting tree (DESIGN.md §22) ---------- */

/*
 * A CHAIN entry's posting set is a B-tree over container keys, GIN's
 * posting-tree shape.  Its LEAVES are the container pages of §4 - items sorted
 * by first ckey, minckey/maxckey, owner stamps, growth slack, DELETED marking,
 * and still linked left to right in ckey order, so every sequential walk
 * (scans, counts, VACUUM) is what it always was.  Its INTERNAL pages hold
 * these pivots and let an intersection SEEK instead of stream.
 *
 * `ckey` is a SEPARATOR: every container key in the child's subtree is at or
 * above it.  `child` is InvalidBlockNumber on the one pivot that is not a
 * downlink, the HIGH KEY: the first item of every non-rightmost internal page
 * is a strict upper bound on the container keys of its whole subtree, which is
 * what lets a reader tell "my key moved right in a split that has not finished"
 * from "my key is simply not here" (nbtree's rule, and §21's).  Leaves carry
 * no high key - they hold containers, not pivots - and use `maxckey` in the
 * special area instead; that is enough for them because every WRITER of a key
 * holds that key's directory leaf EXCLUSIVE (§5, §21) and therefore no leaf
 * split can be in flight while another writer descends.
 */
typedef struct LionPostingPivot
{
	uint32		ckey;
	BlockNumber child;
} LionPostingPivot;

#define LION_POSTING_PIVOT_SIZE		((Size) sizeof(LionPostingPivot))

/* The greatest level a posting tree can reach; verify() and the descent bound
 * themselves by it so that a corrupt `level` cannot loop for ever. */
#define LION_POSTING_MAX_HEIGHT		16

/*
 * The most leaves a posting or position tree has with no internal level
 * (DESIGN.md §22, "Small sets have no root page").  Up to this many, the
 * entry's head is the FIRST leaf and the leaves are only a rightlinked chain:
 * a reader walks right from the head, a writer walks right to the leaf that
 * owns its key, and a split links its new pages in without a downlink.  The
 * split that would make the set one leaf longer than this pushes the head
 * down first: its items go to a brand new leaf and the head becomes a level-1
 * page with a downlink to every leaf, so the head block still never moves.
 * A root page of a set this small is a page that is 99% empty; a walk of
 * sixteen leaves is what a descent saves only once the set is larger.
 */
#define LION_POSTING_FLAT_LEAVES	16

static inline bool
LionPageIsPostingLeaf(Page page)
{
	return LionPageIsContainer(page) && LionPageGetOpaque(page)->level == 0;
}

static inline bool
LionPageIsPostingInternal(Page page)
{
	return LionPageIsContainer(page) && LionPageGetOpaque(page)->level > 0;
}

/*
 * The first item of a posting page that is not its high key.  Leaves have no
 * high key at all, so only a non-rightmost INTERNAL page skips one.
 */
static inline OffsetNumber
lion_posting_first_data(Page page)
{
	return (LionPageGetOpaque(page)->level > 0 && !LionPageIsRightmost(page)) ?
		OffsetNumberNext(FirstOffsetNumber) : FirstOffsetNumber;
}

static inline LionPostingPivot *
lion_posting_pivot(Page page, OffsetNumber off)
{
	return (LionPostingPivot *) PageGetItem(page, PageGetItemId(page, off));
}

/*
 * The high key of a non-rightmost internal posting page, UNCHECKED: nothing
 * here looks at maxoff or at the line pointer.  lion_posting.c reads a page's
 * high key and pivots through checking versions of these two, which make a
 * damaged page an ERROR instead of a read past it (DESIGN.md §22).
 */
static inline LionPostingPivot *
lion_posting_highkey(Page page)
{
	Assert(LionPageGetOpaque(page)->level > 0 && !LionPageIsRightmost(page));
	return lion_posting_pivot(page, FirstOffsetNumber);
}

/*
 * A WALK RIGHT ALONG ONE LEVEL IS BOUNDED BY THE SIZE OF THE INDEX (DESIGN.md
 * §21, §22).  A walk that only moves right never passes a page twice: no
 * page is ever taken out of a level of a live tree, and a split puts its new
 * page to the right of the page it splits, where the walk has not been yet.
 * So a walk that has taken more steps than the index has blocks is going
 * round a cycle of damaged right links.  EVERY walk along a level of the
 * directory or of a posting set steps through this - the readers', the
 * writers' and VACUUM's alike; only lion_index_verify() keeps counts and a
 * map of the blocks it has seen of its own.  Without the bound the scans, the
 * counts and VACUUM would go round such a cycle until they were cancelled,
 * and autovacuum, which nothing cancels, for ever - and some walks cannot be
 * cancelled at all: a posting-tree writer holds its key's directory leaf the
 * whole time, the directory's find-or-create holds its guard, and a content
 * lock holds interrupts off, so CHECK_FOR_INTERRUPTS() does nothing there and
 * statement_timeout never fires.  (That holds on every major: LockBuffer()
 * takes an LWLock through LWLockAcquire() up to 18 and its own lock through
 * BufferLockAcquire() from 19, and both HOLD_INTERRUPTS() until the release.
 * An ERROR raised with such a lock held does not leak it either: the abort
 * releases it, LWLockReleaseAll() up to 18 and the resource owner's
 * ResOwnerReleaseBuffer() from 19.  A temporary index's local buffers are not
 * locked at all.)  The size is asked for only once a walk is
 * LION_RIGHTWALK_CHEAP pages long, which almost no walk ever is, and again
 * each time the walk outgrows it, because the index grows meanwhile.
 *
 * A step also refuses the two links no page ever has, at once and for the
 * price of two comparisons: one to the page itself, the shortest cycle, and
 * one to block 0, the meta page, which a posting-set reader would otherwise
 * take for a freed page and end the set at without a word.  A link past the
 * end of the relation fails in ReadBuffer(); InvalidBlockNumber, which
 * ReadBuffer() would take for P_NEW, is the end of the level, and no walk
 * steps there.
 */
typedef struct LionRightWalk
{
	uint32		steps;
	uint32		limit;
} LionRightWalk;

#define LION_RIGHTWALK_CHEAP	1024

/* Block 0 of every lion index, which no link ever names. */
#define LION_METAPAGE_BLKNO	0

extern void lion_rightwalk_exceeded(Relation index, LionRightWalk *walk,
									BlockNumber blk);
extern void lion_rightwalk_badlink(Relation index, BlockNumber blk,
								   BlockNumber next);

static inline void
lion_rightwalk_init(LionRightWalk *walk)
{
	walk->steps = 0;
	walk->limit = LION_RIGHTWALK_CHEAP;
}

/*
 * One step right from blk to next, blk's right link, which is a valid block
 * number; an ERROR when the link is damaged or the walk has gone round a
 * cycle.
 */
static inline void
lion_rightwalk_step(Relation index, LionRightWalk *walk, BlockNumber blk,
					BlockNumber next)
{
	Assert(BlockNumberIsValid(next));
	if (unlikely(next == blk || next == LION_METAPAGE_BLKNO))
		lion_rightwalk_badlink(index, blk, next);
	if (unlikely(++walk->steps > walk->limit))
		lion_rightwalk_exceeded(index, walk, blk);
}

/* ---------- meta page ---------- */

#define LION_MAGIC			0x52424931	/* 'RBI1' */
/*
 * Version 2 indexes NULL keys (DESIGN.md §14).  The page format did not
 * change - a version 1 index is structurally valid - but it holds no NULL
 * entry, so `IS NULL` would answer "no rows" instead of the truth.  Refusing
 * to open it is the only safe reading: REINDEX turns it into a version 2 one.
 *
 * Version 3 (DESIGN.md §18) adds owner_hash/owner_head to the page special
 * area, which makes the special area 24 bytes instead of 16 and therefore
 * moves every item on every page.  A version 2 index cannot be read at all by
 * this code, so opening one is the same ERROR with the same REINDEX hint.
 *
 * Version 4 (DESIGN.md §21) replaces the hash-bucket entry directory with a
 * B-tree keyed by the index key: the special area grows again (leftlink and
 * level), the meta page points at a root instead of counting buckets, and
 * entry tuples live on directory leaves in key order.  Nothing of a version 3
 * index is readable, so opening one is the same ERROR with the same hint.
 *
 * Version 5 (DESIGN.md §22) makes each key's posting set a B-tree over
 * container keys: the entry's `head` is its ROOT, which is an internal page
 * of LionPostingPivot downlinks as soon as the set outgrows one page.  The
 * page HEADER did not have to change (§21 had already given container pages a
 * `level`), but a version 4 posting set of more than one page is a flat
 * rightlinked chain with no root above it, which this code cannot descend and
 * therefore cannot write to.  §22 planned to share §21's version number
 * because the two were meant to land together; §21 shipped first, so the bump
 * is separate.  Opening a version 4 index is the same ERROR with the same
 * REINDEX hint.
 *
 * Version 6 (DESIGN.md §24) gives every entry tuple the KEY COLUMN it belongs
 * to, in four bytes the header had been padding out, and makes that column the
 * leading term of the directory order.  A version 5 index has zeros there, so
 * every one of its entries would read as column 0 and no descent would find
 * anything; reading such an index by TREATING attno 0 as 1 was considered and
 * rejected in §24, because the two formats would then be told apart by a field
 * that means "column one" in one of them and "no column at all" in the other.
 * Opening a version 5 index is the same ERROR with the same REINDEX hint.
 *
 * Version 7 (DESIGN.md §32) is version 6 plus SUMMARY POSTING SETS: entries of
 * two new kinds, after a column's VALUE entries, each holding the union of the
 * posting sets of a run of that column's keys.  It is an ADDITION: every page
 * and every item of a version 6 index reads exactly as it did, and a version 6
 * index is simply one without summaries, which this code reads and writes as it
 * always has.  The number moves only so that a build of pg_lion that predates
 * summaries refuses an index that has them - it would take a summary entry for
 * a VALUE entry, whose kind is what an unknown flag reads as, and count its
 * rows twice.  A build writes version 7 only when at least one key column has
 * summaries (summary_cols below), so an index built with `summaries = off`
 * stays readable by the builds before this one.
 *
 * Version 8 (DESIGN.md §38) is version 7 plus NARROW CONTAINERS: items of a
 * fifth type, a bitset of the offsets below 64 * k of a container's heap
 * blocks, k being the item's width, 1 .. LION_NARROW_MAX_WIDTH, in its
 * header's flags byte.  It is an addition too - summary_cols says as before
 * whether there are summaries, and every page and item of a version 6 or 7
 * index reads as it did - and the number moves so that a build that
 * predates NARROW refuses an index that has one rather than reporting it
 * corrupt, or worse.
 *
 * Version 9 (DESIGN.md §13) stores a sparse segment's pairs as a stream of
 * delta varints instead of uint32 ckeys[] and uint16 los[], which
 * reinterprets every segment of every older index, so versions 6, 7 and 8
 * are no longer read: opening one is an ERROR whose hint asks for REINDEX.
 * Every index this build writes is version 9, whatever it holds: whether a
 * key column has summaries is what summary_cols says (§32).  The leaf-root
 * posting trees share the number.
 *
 * Version 10 (DESIGN.md §21, "Entry tuples") lays out every directory item
 * anew - a 12-byte header for an INLINE entry or a pivot and a 28-byte one
 * for a CHAIN entry, where every item had the same 32 bytes; no stored hash;
 * a by-value key at its type's width rather than as a whole Datum; the
 * payload packed right after the key (lion_entryfmt.h) - and it stamps the
 * pages of a posting tree with a hash of the key's bytes instead of the
 * key's hash.  So version 9 is no longer read either: opening one is an
 * ERROR whose hint asks for REINDEX.  Whether an index has summaries is said
 * as version 9 said it.
 *
 * Version 11 raises LION_MAX_KEY_SIZE from 2000 to 2047 bytes.  A version 10
 * index of jsonb_contains_ops (§42) holds as hashes the keys of 2001 to 2047
 * bytes that this build stores as they are, and so would not be found under
 * them; and LION_MAX_ENTRY_SIZE, which leaves room for the largest pivot,
 * shrank, so an INLINE entry a version 10 build wrote may be larger than a
 * page now leaves room for.  Opening a version 10 index is an ERROR whose
 * hint asks for REINDEX.
 *
 * Version 12 begins EVERY internal directory page with a minus-infinity
 * downlink, where version 11 did so only on the leftmost page of a level
 * (DESIGN.md §21, "Height").  A version 11 directory may have keyed first
 * downlinks and any height, which is what version 12 rules out, so opening
 * one is an ERROR whose hint asks for REINDEX.  Version 12 also has the pages
 * VACUUM takes out of the tree and hands out again - HALF_DEAD and DELETED
 * directory pages (§21, "Page deletion"), UNLINKED posting leaves (§22,
 * "Empty leaves") - and the count of them still waiting on the meta page
 * (§18, "Reuse").
 *
 * Version 13 files a multi-key row whose extracted keys are all NULL - an
 * array of NULLs - in a reserved entry of its own, LION_ENTRY_NULLELEMS,
 * where version 12 filed it in the EMPTY entry (DESIGN.md §17, "The reserved
 * EMPTY entry").  The EMPTY entry of a version 12 index may hold such rows,
 * which the count of a non-empty array (`array_length(a, 1) IS NOT NULL`)
 * would take away with the empty ones, so opening one is an ERROR whose hint
 * asks for REINDEX.
 *
 * Version 14 files an array with no element that has dimensions - `[0:-1]`,
 * an empty int2vector's or oidvector's cast to int2[] or oid[] - in reserved
 * entries of their own, LION_ENTRY_EMPTYDIM and LION_ENTRY_EMPTYINNER, where
 * version 13 filed it in the EMPTY entry with the arrays of no dimension
 * (§17).  `a = '{}'`, `a <> '{}'`, array_length() and array_ndims() tell the
 * two apart, so the EMPTY entry of a version 13 index answers them wrongly.
 * Version 14 also lets every index hold NARROW items (§38) wherever a
 * container goes, whoever writes them - the build, an INSERT or VACUUM - and
 * its order_flags have the bits defined below and no others, where version
 * 13 said with the flag 0x0004 that the build had written a NARROW, and
 * VACUUM made them only in such an index: a version 13 index may carry that
 * bit, which is no flag of this format.  So opening a version 13 index is an
 * ERROR whose hint asks for REINDEX.
 *
 * Version 15 (DESIGN.md §51) is version 14 plus SLICE ENTRIES: after a
 * bit-sliced column's VALUE entries and summaries, one entry per bit of its
 * values' encoding and one of the rows not NULL, of a kind no build before
 * knows - it would read a slice as a VALUE entry with a one-byte key, and fail
 * on it or count it.  An ADDITION, as version 7 was (§32): every page and item
 * of a version 14 index reads as it did, and a build writes version 15 only
 * when at least one key column is sliced (slice_cols below), so an index
 * built with `slices = off` is version 14 byte for byte and stays readable by
 * the builds before this one.  This build reads both.
 */
#define LION_VERSION			6	/* the base format of versions 6 .. 8 */
#define LION_VERSION_SUMMARIES	7	/* ... plus the summaries of §32 */
#define LION_VERSION_NARROW		8	/* ... plus the NARROW items of §38 */
#define LION_VERSION_V9			9	/* varbyte sparse segments (§13) */
#define LION_VERSION_V10		10	/* compact entry tuples (§21) */
#define LION_VERSION_V11		11	/* keys of up to 2047 bytes (§4) */
#define LION_VERSION_V12		12	/* key-less first downlinks (§21) */
#define LION_VERSION_V13		13	/* the NULLELEMS entry (§17) */
#define LION_VERSION_V14		14	/* EMPTYDIM and EMPTYINNER (§17), NARROW
								 * items anywhere, no flag (§38) */
#define LION_VERSION_V15		15	/* ... plus the SLICE entries of §51 */

typedef struct LionMetaPageData
{
	uint32		magic;
	uint32		version;
	uint16		offset_bits;	/* LION_OFFSET_BITS at build time */
	uint16		container_bits; /* LION_CONTAINER_BITS at build time */

	/*
	 * BIT-SLICED KEY COLUMNS (DESIGN.md §51): bit i - 1, key column i keeps
	 * SLICE entries, decided when the index was built and fixed for its life.
	 * Nonzero only on a version 15 meta page.  The word was nbuckets, the
	 * hash directory's bucket count, which is 0 on every meta page written
	 * since version 4 (§21) - exactly "no column is sliced" - so the struct
	 * keeps its 56 bytes, as §25, §21's order record and §32 kept them.
	 */
	uint32		slice_cols;
	uint32		inline_limit;	/* max inline payload bytes in an entry tuple */
	BlockNumber root;			/* root of the entry directory (§21) */
	uint32		height;			/* level of the root; 0 = the root is a leaf */
	uint32		dirpages;		/* directory pages, leaves and internal both */

	/*
	 * How this index is WAL-logged (DESIGN.md §25): LION_WAL_MODE_GENERIC or
	 * LION_WAL_MODE_RMGR.  It lives in what was reserved space, so the format
	 * version did not have to move: a version 6 index written before §25 has
	 * a zero here, which is exactly "generic", and reads and writes as it
	 * always did.  REINDEX is what changes an index's mode.
	 */
	uint32		wal_mode;

	/*
	 * THE ORDER OF THE DIRECTORY, as the build laid it out (DESIGN.md §21,
	 * "The order is the index's").  Whether a key column's entries are in its
	 * comparison's order is a property of the directory, fixed when the index
	 * is built; recomputing it from the catalog at every relcache build let a
	 * btree opclass created (or dropped) later flip an existing directory
	 * between hash order and value order, and every descent then read it in
	 * an order it is not in.
	 *
	 *	order_flags		LION_META_ORDER_RECORDED once the two words below
	 *					mean something.  Zero on an index built before they
	 *					existed, which is read the old way.
	 *	ordered_cols	bit i - 1: key column i is in its comparison's order.
	 *	order_ident		a hash of WHICH comparison each ordered column was
	 *					built with - the source it runs (lion_proc_ident()),
	 *					which survives pg_upgrade, a schema move and a rename
	 *					where an Oid or a name does not - so that the common
	 *					ways of replacing a comparison since are noticed
	 *					rather than used.  A best-effort guard, not a proof:
	 *					what the function CALLS is not in its source (§21).
	 *
	 * They live in what was reserved space, zero on every index written
	 * before, so the format version stays at 6, as it did for wal_mode.
	 */
	uint32		order_flags;
	uint32		ordered_cols;
	uint32		order_ident;

	/*
	 * SUMMARY POSTING SETS (DESIGN.md §32), in the last two reserved words -
	 * zero on every index written before them, which is exactly "no column
	 * has summaries".
	 *
	 *	summary_cols	bit i - 1: key column i has summaries, decided when the
	 *					index was built and fixed for its life.  Nonzero only
	 *					on a version 7 meta page.
	 *	summary_tids	how many TIDs a summary bucket takes before the next
	 *					key above every key it holds starts a new one: the
	 *					`summary_tids` reloption at build time.  Inserts read
	 *					it from here, so ALTER INDEX changes nothing until a
	 *					REINDEX, as for wal_mode.
	 */
	uint32		summary_cols;
	uint32		summary_tids;
} LionMetaPageData;

#define LION_META_ORDER_RECORDED	0x0001

/*
 * The build stored word positions (DESIGN.md §17, "Stored positions") for
 * every key column whose opclass can extract them (support function 5): the
 * `store_positions` reloption at build time, with "auto" settled by the
 * columns (LION_POSOPT_AUTO), so that the bit is set only on an index that
 * has such a column.  It shares order_flags because the reserved words are
 * spent; an index written before it has the bit clear, which is exactly "no
 * positions".  ALTER INDEX changes nothing until a REINDEX.
 */
#define LION_META_POSITIONS			0x0002

/* ordered_cols has one bit per key column. */
StaticAssertDecl(INDEX_MAX_KEYS <= 32,
				 "lion's meta page records the order of at most 32 key columns");

/*
 * §25 and §21's order record spent four of the reserved words and §32's
 * summaries the last two, so the struct is exactly the size it has been since
 * version 4 and a meta page written by any of them reads as the others - which
 * is what let the format version stay at 6 until §32 needed it to move for a
 * reason of its own.
 */
StaticAssertDecl(sizeof(LionMetaPageData) == 56,
				 "the lion meta page payload must not change size");

#define LionPageGetMeta(page)	((LionMetaPageData *) PageGetContents(page))

/*
 * THE DISTINCT KEYS OF EACH KEY COLUMN (DESIGN.md §33), for the planner's
 * n_distinct.  The reserved words of LionMetaPageData are all spent, so they
 * follow it on the meta page, at LION_META_NDISTINCT_OFFSET, and are there
 * only when pd_lower covers them.  A meta page written before §33 ends at
 * LionMetaPageData, which is what pd_lower says, and reads as one without
 * counts - the zero that "no count" is everywhere else.  A build writes the
 * area, and a meta page without one gets it from the first VACUUM or ANALYZE
 * that counts; nothing before §33 reads past LionMetaPageData, which is why
 * the format version does not move (§33, "Format").
 *
 *	valid_cols	bit i - 1: ndistinct[i - 1] is key column i's count.  A
 *				multi-key column's keys are elements, not values of the
 *				column, and it never has one.
 *	rows		the rows the index held when it was counted: the TIDs under
 *				its first scalar key column's entries, NULL entry included.
 *				ANALYZE's convention divides by it (§33, "The value"), and
 *				the planner compares it with the table's rows to tell a
 *				count the table has outgrown (§33, "Stale counts").
 *	ndistinct	the VALUE entries of each key column - neither its NULL nor
 *				its EMPTY entry, nor a summary (§32).  A key whose rows are
 *				all dead is counted until VACUUM deletes its entry: an
 *				estimate, as ANALYZE's own is.
 *	bulkdeletes	how many ambulkdelete calls have finished on the index,
 *				modulo 2^32 - not a count for the planner but the one word
 *				of the area that was spare (§37, "The rows of an entry"):
 *				the end of every call adds one, in the record that writes
 *				the counts it took, and the aggregates over keys read it
 *				before their first look at the visibility map and after
 *				their second.  ANALYZE, and a VACUUM that calls no
 *				ambulkdelete, keep it.  Zero where a build wrote the area,
 *				and on a meta page that has none.
 *	sumsq		the sum over the same VALUE entries of the square of each
 *				one's TIDs, Σn², counted in the same pass as ndistinct
 *				(DESIGN.md §33, "The rows of a probed key"): the rows a
 *				key drawn by row meets, Σn²/Σn, are what an equality to a
 *				Param meets on a skewed column.  Saturates at
 *				PG_UINT64_MAX, which the planner takes for unknown, as it
 *				takes 0 (LionMetaSumsqKnown()).
 *
 * Nothing in it says when it was counted: a build writes exactly the bytes
 * every other build of the same rows writes, whatever the memory, the workers
 * or the snapshot (test/sql/build.sql compares them), and a time would differ.
 *
 * The array has room for 32 columns whatever INDEX_MAX_KEYS this server was
 * built with, so that the layout does not depend on it (ordered_cols above
 * already caps a lion index at 32 key columns).
 */
#define LION_META_MAX_COLS		32

typedef struct LionMetaNdistinct
{
	uint32		valid_cols;
	uint32		bulkdeletes;
	uint64		rows;
	uint64		ndistinct[LION_META_MAX_COLS];
	uint64		sumsq[LION_META_MAX_COLS];
} LionMetaNdistinct;

StaticAssertDecl(INDEX_MAX_KEYS <= LION_META_MAX_COLS,
				 "lion's meta page counts the keys of at most 32 key columns");
StaticAssertDecl(sizeof(LionMetaNdistinct) == 528,
				 "the lion meta page's key counts must not change size");

/* Key column i's (from 0) Σn² of nd, when it is known. */
#define LionMetaSumsqKnown(nd, i) \
	((nd)->rows > 0 && (nd)->sumsq[i] != 0 && \
	 (nd)->sumsq[i] != PG_UINT64_MAX)

#define LION_META_NDISTINCT_OFFSET \
	(MAXALIGN(SizeOfPageHeaderData) + MAXALIGN(sizeof(LionMetaPageData)))
#define LION_META_NDISTINCT_END \
	(LION_META_NDISTINCT_OFFSET + sizeof(LionMetaNdistinct))
#define LionPageGetMetaNdistinct(page) \
	((LionMetaNdistinct *) ((char *) (page) + LION_META_NDISTINCT_OFFSET))
/* Does this meta page carry the counts at all? */
#define LionMetaHasNdistinct(page) \
	(((PageHeader) (page))->pd_lower >= LION_META_NDISTINCT_END)

/*
 * Add an entry of n TIDs to a column's Σn², saturating at PG_UINT64_MAX
 * (which the planner reads as unknown).
 */
static inline void
lion_sumsq_add(uint64 *sum, uint64 n)
{
	uint64		sq;

	if (*sum == PG_UINT64_MAX)
		return;
	if (pg_mul_u64_overflow(n, n, &sq) || pg_add_u64_overflow(*sum, sq, sum))
		*sum = PG_UINT64_MAX;
}

/* The first block after the meta page; where ambuild puts the first leaf. */
#define LION_FIRST_BLKNO		((BlockNumber) 1)

/* ---------- entry tuples (items on directory pages) ---------- */

/*
 * The entry tuple's layout - its two header forms, its flags, where its key,
 * positions extension and payload are, and the accessors of its counts and
 * links - is in lion_entryfmt.h (DESIGN.md §21, "Entry tuples"), which the
 * unit tests build without a server.
 */
#include "lion_entryfmt.h"

#define LionEntryIsPivot(e)		(((e)->flags & LION_ENTRY_PIVOT) != 0)
#define LionEntryIsHighKey(e)	(((e)->flags & LION_ENTRY_HIGHKEY) != 0)
#define LionEntryIsDownlink(e)	(((e)->flags & LION_ENTRY_DOWNLINK) != 0)
#define LionEntryIsMinusInf(e)	(((e)->flags & LION_ENTRY_MINUSINF) != 0)

/*
 * Five entries per key column have no key at all: keylen 0, hash 0, first in
 * the column's run, and recognised by a flag rather than by comparing keys.
 *
 *	- LION_ENTRY_NULLKEY holds the rows whose indexed VALUE is NULL
 *	  (DESIGN.md §14).
 *	- LION_ENTRY_EMPTYKEY holds the rows a multi-key opclass extracted
 *	  nothing from at all - an array with no element and no dimension, a
 *	  tsvector with no lexemes (DESIGN.md §17).  A scalar opclass never
 *	  creates it.
 *	- LION_ENTRY_EMPTYDIM and LION_ENTRY_EMPTYINNER hold the arrays with no
 *	  element that have dimensions, an array extraction's alone (§17): the
 *	  first those whose first dimension has length 0 (`[0:-1]`), the second
 *	  those whose first dimension has a length and an inner one none
 *	  (`[1:2][0:-1]`).
 *	- LION_ENTRY_NULLELEMS holds the rows it extracted keys from that are
 *	  all NULL - an array of NULLs (§17).  Only array_ops' extraction
 *	  returns NULL keys.
 *
 * Everything that searches a bucket for a key must skip them, and everything
 * that reads a stored key must ask first.  They are reached only through
 * lion_find_reserved_entry() and its wrappers.  A row is in exactly one of
 * them or under its own keys, never in two.
 */
#define LionEntryIsNullKey(e)	(((e)->flags & LION_ENTRY_NULLKEY) != 0)
#define LionEntryIsEmptyKey(e)	(((e)->flags & LION_ENTRY_EMPTYKEY) != 0)
#define LionEntryIsNullElems(e)	(((e)->flags & LION_ENTRY_NULLELEMS) != 0)
#define LionEntryIsReserved(e)	(((e)->flags & LION_ENTRY_RESERVED) != 0)
/* ... and the reserved entries of rows under no key that are not NULL */
#define LionEntryIsKeyless(e)	(((e)->flags & LION_ENTRY_KEYLESS) != 0)
#define LionEntryIsSummary(e)	(((e)->flags & LION_ENTRY_SUMKINDS) != 0)
#define LionEntryIsSumLast(e)	(((e)->flags & LION_ENTRY_SUMLAST) != 0)
/* A bit slice or the existence slice of a sliced column (DESIGN.md §51). */
#define LionEntryIsSlice(e)		(((e)->flags & LION_ENTRY_SLICE) != 0)
/*
 * A summary or a slice: an entry whose posting set is derived from its
 * column's rows and that sorts after every VALUE entry of the column.  Every
 * walk that returns keys stops at the first one, as it stops at the next
 * column's first entry (DESIGN.md §32, §51).
 */
#define LionEntryIsDerived(e)	(((e)->flags & LION_ENTRY_DERIVED) != 0)
#define LION_NULLKEY_HASH		0

/*
 * The hash every SUMMARY entry carries (DESIGN.md §32), whatever its key.
 * A summary is ordered by its key alone - the keys of a column's summaries
 * are distinct under proc 4, and the open one sorts by its kind - so the hash
 * has nothing to order; and it is what the owner stamp of every page of a
 * CHAINED summary's posting tree says (§18), which must not change when the
 * open bucket's key is raised.  With the key's hash there, raising the key of
 * a chained open bucket left its pages claiming another entry.
 */
#define LION_SUMMARY_HASH		0

/*
 * THE DIRECTORY ORDER (DESIGN.md §21, §24).
 *
 * Entries sort by (attno, kind, key, hash, stored bytes).  The KEY COLUMN
 * leads (§24): one relation holds every key column's entries, as independent
 * runs laid end to end, so a lookup is an (attno, key) descent and an entry
 * scan bounded to one column is the leaf walk from its first entry to the
 * first entry of the next column.  The leftmost downlink of an internal page
 * carries attno 0 and therefore sorts below every real entry whatever its
 * kind, which is what keeps the MINF rule of §21 intact.
 *
 * Within one column the kind puts the five reserved entries first - they
 * have no key to compare - and gives the leftmost downlink of an internal page
 * a value below every real one:
 *
 *		MINF < NULL < EMPTY < EMPTYDIM < EMPTYINNER < NULLELEMS < VALUE
 *			 < SUMMARY < SUMLAST < SLICE
 *
 * The two summary kinds of DESIGN.md §32 come after every VALUE entry, so a
 * column's values are one run and its summaries the run right after it: a walk
 * of a column's values ends at its first summary exactly as it ends at the
 * next column's first entry.  SUMMARY entries are ordered by their keys like
 * values; the one SUMLAST entry a column may have is ordered by its kind
 * alone.  The SLICE entries of §51 come last, after the summaries, each
 * ordered by its one-byte key - the bit position - which the prefix compares
 * bytewise (lion_cmp_prefix()): one entry per position, found as a reserved
 * entry is found, and the first of a column's derived kinds that a walk of
 * values meets when the column has no summaries.
 *
 * Within VALUE the order is the opclass's ordering support function (proc 4)
 * under the index collation, then the hash, then a bytewise comparison of the
 * stored datum.  The first three of those form the "prefix" comparison: two
 * entries whose prefixes tie are candidates for being the same key, and only
 * the opclass equality decides (citext's 'Alice' and 'alice' tie and ARE one
 * entry).  The bytewise tail exists so that the order is total even for a
 * type with no btree opclass at all, where the prefix is just (kind, hash).
 */
#define LION_KIND_MINF		0
#define LION_KIND_NULL		1
#define LION_KIND_EMPTY		2
#define LION_KIND_EMPTYDIM	3	/* DESIGN.md §17, "The reserved EMPTY entry" */
#define LION_KIND_EMPTYINNER 4	/* ... likewise */
#define LION_KIND_NULLELEMS	5	/* ... likewise */
#define LION_KIND_VALUE		6
#define LION_KIND_SUMMARY	7	/* DESIGN.md §32 */
#define LION_KIND_SUMLAST	8
#define LION_KIND_SLICE		9	/* DESIGN.md §51 */

/*
 * The kinds of the reserved entries, which have no key and one per column:
 * NULL to NULLELEMS, consecutive.
 */
#define LION_KIND_IS_RESERVED(k) \
	((k) >= LION_KIND_NULL && (k) <= LION_KIND_NULLELEMS)
#define LION_NRESERVED	(LION_KIND_NULLELEMS - LION_KIND_NULL + 1)
StaticAssertDecl(LION_KIND_EMPTY == LION_KIND_NULL + 1 &&
				 LION_KIND_EMPTYDIM == LION_KIND_NULL + 2 &&
				 LION_KIND_EMPTYINNER == LION_KIND_NULL + 3 &&
				 LION_KIND_NULLELEMS == LION_KIND_NULL + 4 &&
				 LION_KIND_VALUE == LION_KIND_NULLELEMS + 1,
				 "the reserved kinds are consecutive, in directory order");

/*
 * Kinds whose order compares keys OF THE COLUMN'S TYPE through its opclass
 * (the others are one entry per column, or - a SLICE - one per position of a
 * key that is not the column's, compared bytewise).
 */
#define LION_KIND_HAS_KEY(k)	((k) == LION_KIND_VALUE || (k) == LION_KIND_SUMMARY)
/* The kinds after a column's VALUE entries: its summaries and its slices. */
#define LION_KIND_IS_DERIVED(k)	((k) > LION_KIND_VALUE)

/*
 * The largest key, in bytes as stored (a varlena with its 4-byte header).  A
 * tsvector lexeme is at most 2047 bytes (ts_type.h's MAXSTRLEN), so a key of
 * this size holds every lexeme of up to 2043; the longer ones are hashed by
 * tsvector_ops (lion_lexeme_key(), DESIGN.md §17 "Long lexemes"), as are the
 * longer text and bytea elements of array_ops, and keys of
 * jsonb_contains_ops past it by that class (§42.1).  Any other varlena key
 * past it is stored compressed, as a btree index tuple stores a long value
 * (lion_key_prepare(), §4 "Long keys"), and is an ERROR only when even that
 * does not fit.
 */
#define LION_MAX_KEY_SIZE		2047
#define LION_DEFAULT_INLINE_LIMIT 4096
#define LION_MIN_INLINE_LIMIT	64
#define LION_MAX_INLINE_LIMIT	4096

/* The kind of an entry tuple, for the directory order above. */
static inline int
lion_entry_kind(const LionEntryTuple *e)
{
	if ((e->flags & LION_ENTRY_MINUSINF) != 0)
		return LION_KIND_MINF;
	if ((e->flags & LION_ENTRY_NULLKEY) != 0)
		return LION_KIND_NULL;
	if ((e->flags & LION_ENTRY_EMPTYKEY) != 0)
		return LION_KIND_EMPTY;
	if ((e->flags & LION_ENTRY_EMPTYDIM) != 0)
		return LION_KIND_EMPTYDIM;
	if ((e->flags & LION_ENTRY_EMPTYINNER) != 0)
		return LION_KIND_EMPTYINNER;
	if ((e->flags & LION_ENTRY_NULLELEMS) != 0)
		return LION_KIND_NULLELEMS;
	if ((e->flags & LION_ENTRY_SLICE) != 0)
		return LION_KIND_SLICE;
	if ((e->flags & LION_ENTRY_SUMLAST) != 0)
		return LION_KIND_SUMLAST;
	if ((e->flags & LION_ENTRY_SUMMARY) != 0)
		return LION_KIND_SUMMARY;
	return LION_KIND_VALUE;
}

/*
 * The kind of the reserved entry a reserved flag names, and the flag of a
 * reserved kind: LION_ENTRY_NULLKEY is LION_KIND_NULL, LION_ENTRY_EMPTYKEY
 * LION_KIND_EMPTY, LION_ENTRY_EMPTYDIM LION_KIND_EMPTYDIM,
 * LION_ENTRY_EMPTYINNER LION_KIND_EMPTYINNER and LION_ENTRY_NULLELEMS
 * LION_KIND_NULLELEMS.
 */
static inline int
lion_reserved_kind(uint16 reservedflag)
{
	switch (reservedflag)
	{
		case LION_ENTRY_NULLKEY:
			return LION_KIND_NULL;
		case LION_ENTRY_EMPTYKEY:
			return LION_KIND_EMPTY;
		case LION_ENTRY_EMPTYDIM:
			return LION_KIND_EMPTYDIM;
		case LION_ENTRY_EMPTYINNER:
			return LION_KIND_EMPTYINNER;
		default:
			Assert(reservedflag == LION_ENTRY_NULLELEMS);
			return LION_KIND_NULLELEMS;
	}
}

static inline uint16
lion_reserved_flag(int kind)
{
	switch (kind)
	{
		case LION_KIND_NULL:
			return LION_ENTRY_NULLKEY;
		case LION_KIND_EMPTY:
			return LION_ENTRY_EMPTYKEY;
		case LION_KIND_EMPTYDIM:
			return LION_ENTRY_EMPTYDIM;
		case LION_KIND_EMPTYINNER:
			return LION_ENTRY_EMPTYINNER;
		default:
			Assert(kind == LION_KIND_NULLELEMS);
			return LION_ENTRY_NULLELEMS;
	}
}

/*
 * The kind flags of a PIVOT (a high key or a downlink) made from an item with
 * `flags` (DESIGN.md §21, §32): the item's own, except that a pivot is never
 * of the SUMLAST kind.  The open bucket's entry sorts after every summary of
 * its column by its kind alone, and closing the bucket drops the flag IN
 * PLACE - so a SUMLAST pivot above it would then sort after the SUMMARY it
 * routes to, the left page's high key above the right page's first key, and
 * every later descent to a summary would land left of it.  A SUMMARY pivot
 * with the open bucket's key at the time is a lower bound for the entry
 * whatever becomes of it, because its key only ever rises, and it is above
 * every other summary of the column, whose keys are below the open one's.
 * The pivot keeps LION_ENTRY_ALIGNKEY too, because it carries the item's key
 * where the item has it.
 */
static inline uint16
lion_pivot_kindflags(uint16 flags)
{
	uint16		k = flags & (LION_ENTRY_KINDFLAGS | LION_ENTRY_MINUSINF |
							 LION_ENTRY_ALIGNKEY);

	if ((k & LION_ENTRY_SUMLAST) != 0)
		k = (uint16) ((k & ~LION_ENTRY_SUMLAST) | LION_ENTRY_SUMMARY);
	return k;
}

/*
 * The largest PIVOT tuple (a high key or a downlink) the directory can hold,
 * and therefore what every split has to keep room for on both halves.
 */
#define LION_MAX_PIVOT_TUPLE \
	MAXALIGN(DOUBLEALIGN(LION_ENTRY_SHORT_HDRSZ) + LION_MAX_KEY_SIZE)
#define LION_MAX_PIVOT_SIZE		(LION_MAX_PIVOT_TUPLE + sizeof(ItemIdData))

/*
 * The largest ENTRY tuple, which is smaller than the largest item a page can
 * hold: a leaf that holds one entry must still have room for the high key a
 * split would give it (DESIGN.md §21).  A payload that would exceed this
 * spills onto container pages instead, exactly as one that exceeds
 * inline_limit does.
 */
#define LION_MAX_ENTRY_SIZE \
	MAXALIGN_DOWN(LION_MAX_ITEM_SIZE - LION_MAX_PIVOT_SIZE - sizeof(ItemIdData))

/* ---------- reloptions ---------- */

#define LION_DEFAULT_FILLFACTOR	90
#define LION_MIN_FILLFACTOR		10

/*
 * How full ambuild packs a POSTING-TREE LEAF, the leaves of a set without an
 * internal level included (DESIGN.md §22, "Bulk build").  100 packs a leaf
 * until the next item does not fit, which is what every build did before the
 * option existed; less leaves the room the first inserts after the build need
 * to rewrite a container in place, where a full leaf has to split: an UPDATE
 * whose new version stays on the old row's page - a heap below fillfactor
 * 100, or a page VACUUM made room on - inserts inside a container the build
 * wrote.  The default is nbtree's leaf default, 90, and the directory's.  A
 * leaf always takes its first item, so the fill is a target the build stays
 * under, never a reason to leave a page empty.
 */
#define LION_DEFAULT_POSTING_FILLFACTOR	90
#define LION_MIN_POSTING_FILLFACTOR		10

/*
 * How full ambuild packs an INTERNAL directory page, and the fewest downlinks
 * one may carry (DESIGN.md §21).
 *
 * `fillfactor` is about the LEAVES - it leaves room for the entries a later
 * INSERT grows or adds - and must not reach the internal levels: an internal
 * page that holds one downlink makes every level as large as the one below
 * it, so the bottom-up build never reaches a root at all.  nbtree keeps a
 * separate BTREE_NONLEAF_FILLFACTOR of 70 for exactly this reason, and a hard
 * minimum on top of it, because a page's fill target says nothing about how
 * large one item is: three 2047-byte pivots plus a high key do not fit a
 * block, so the floor that can always be honoured is two.
 */
#define LION_NONLEAF_FILLFACTOR	70
#define LION_MIN_DOWNLINKS		3
#define LION_ABS_MIN_DOWNLINKS	2

/*
 * The tallest directory a meta page may record (its `height`, the root's
 * level, 0 for a root that is a leaf); a taller one is damage.
 *
 * No INSERT reaches it short of more than 2^31 leaves made over the index's
 * life, whatever the keys and their order (DESIGN.md §21, "Height").  Every
 * internal page's FIRST downlink is minus infinity - a header and no key, its
 * lower bound being its left sibling's high key - so an internal page split
 * has the room for two downlinks on each half even when every key is
 * LION_MAX_KEY_SIZE (lion_dir_choose_split()), and so does the build
 * (lion_build_level_add()), which may leave ONE on the rightmost page of a
 * level; and a root has two when it is made, by a root split or as the one
 * page of the build's top level over a level of at least two.  A page with
 * two downlinks takes a third without splitting, so a split cuts four or more.
 * VACUUM takes downlinks out (lion_dir_delete.c) and may leave a page with
 * one, so what is counted is the pages MADE at a level: with Φ the sum over
 * the level's pages of max(downlinks - 2, 0), a downlink added raises Φ by at
 * most one, a split lowers it by two, a downlink or page taken out never
 * raises it, a root split adds a term of 0 and the build terms of
 * downlinks - 2 but for one page.  Φ >= 0 then gives 2M <= A + 1 for the M
 * pages made at a level and the A downlinks ever added to it, and each of
 * those names a page made at the level below.  A directory of height h has
 * therefore had at least 2^(k-1) + 1 pages made at level h - k for k >= 1,
 * 2^(h-1) + 1 leaves and 2^h + h pages in all.  A relation has fewer than
 * 2^32 blocks, so without page deletion h <= 31; with it the pages are used
 * again, and 32 levels take more than 2^31 leaves made, a leaf split for
 * every half page of entries added.  lion_dir_split() refuses a root split
 * that would make the directory 32 levels tall before it writes anything.
 */
#define LION_MAX_DIR_HEIGHT		31

typedef struct LionOptions
{
	int32		vl_len_;		/* varlena header (do not touch directly) */
	int			buckets;		/* accepted and ignored since format 4 (§21) */
	int			inline_limit;
	int			max_entries;	/* 0 = unlimited (DESIGN.md §17) */
	int			fillfactor;		/* directory leaf fill at build time (§21) */
	int			posting_fillfactor;	/* posting leaf fill at build time (§22) */
	int			build_slack;	/* LION_SLACKOPT_*, DESIGN.md §4 */
	int			wal_mode;		/* LION_WALOPT_*, DESIGN.md §25 */
	int			summaries;		/* LION_SUMOPT_*, DESIGN.md §32 */
	int			summary_tids;	/* TIDs a summary bucket takes (§32) */
	int			store_positions;	/* LION_POSOPT_*, DESIGN.md §17 */
	bool		slices;			/* bit-sliced columns, DESIGN.md §51 */
} LionOptions;

/*
 * The `summaries` reloption (DESIGN.md §32): which ORDERED SCALAR key columns
 * a build gives summary posting sets.  OFF is the default and builds none, so
 * an index is written as format 6 exactly as before; ON gives every such
 * column summaries; AUTO decides per column from the data the build sees - a
 * column with many keys and few rows per key, where a range walks many small
 * entries - and gives an index built on an empty table none.  Read at build
 * time only: the answer is recorded on the meta page (summary_cols).
 */
#define LION_SUMOPT_OFF			0
#define LION_SUMOPT_ON			1
#define LION_SUMOPT_AUTO		2

/*
 * The `summary_tids` reloption: how many TIDs a summary bucket holds before the
 * next insert of a key above every key it holds starts a new one, and how many
 * a build puts in one before it starts the next.  It trades the two costs of a
 * summed range (DESIGN.md §32, "Bucket size"): the summaries a range covers,
 * about its rows over this, against the keys of the two buckets at its edges,
 * which are walked one by one.
 */
#define LION_DEFAULT_SUMMARY_TIDS	4096
#define LION_MIN_SUMMARY_TIDS		16
#define LION_MAX_SUMMARY_TIDS		(1 << 24)

/*
 * AUTO's rule (DESIGN.md §32): a column gets summaries when it has at least
 * LION_SUMMARY_AUTO_MIN_BUCKETS buckets' worth of TIDs and its keys hold at
 * most summary_tids / LION_SUMMARY_AUTO_MIN_KEYS TIDs each on average, so that
 * a bucket stands for at least that many keys.  Below either there is little
 * per-key work for a summary to save: a range over a few keys, or over keys
 * each so large that a summary is barely larger than one of them.
 */
#define LION_SUMMARY_AUTO_MIN_BUCKETS	4
#define LION_SUMMARY_AUTO_MIN_KEYS		16

/*
 * The `wal_mode` reloption (DESIGN.md §25).  AUTO is the default and means
 * "rmgr when this server registered the resource manager, generic when it did
 * not", which is what lets the same CREATE INDEX script work on a cluster
 * that preloads the library and on one that does not.  Asking for RMGR
 * explicitly on a server without it is an ERROR with the preload hint.
 */
#define LION_WALOPT_AUTO		0
#define LION_WALOPT_GENERIC		1
#define LION_WALOPT_RMGR		2

/*
 * The `build_slack` reloption (DESIGN.md §4, "Growth slack"): whether ambuild
 * allots every INLINE entry and every posting-tree item the growth slack an
 * INSERT would give it, so that the first member an insert adds lands inside
 * the bytes the item already has and no other item on its page moves.  AUTO
 * gives it to an index built for generic WAL, where a moved item makes the
 * record a diff of everything that moved, and not to one built for the
 * resource manager, whose record is the item's own change either way.  OFF,
 * the default, writes every item at its exact size, as every build did before
 * the option.  Read at build time only.
 */
#define LION_SLACKOPT_AUTO		0
#define LION_SLACKOPT_ON		1
#define LION_SLACKOPT_OFF		2
#define LION_DEFAULT_BUILD_SLACK	LION_SLACKOPT_OFF

/*
 * The `store_positions` reloption (DESIGN.md §17, "Stored positions").  AUTO
 * is the default and stores them for every key column whose opclass can
 * (support function 5: tsvector_ops), and builds an index with no such column
 * without them, as GIN keeps a tsvector's positions without being asked.  ON
 * asks for them, and is an ERROR on an index none of whose columns can store
 * them; OFF builds without them.  "true" and "false" are ON and OFF, so that
 * the spellings of the boolean the option once was - a bare WITH
 * (store_positions) among them, which core hands over as "true" - still
 * work.  Read at build time only: what the build did is recorded on the meta
 * page (LION_META_POSITIONS).
 */
#define LION_POSOPT_AUTO		0
#define LION_POSOPT_ON			1
#define LION_POSOPT_OFF			2

#define LION_DEFAULT_MAX_ENTRIES		0

/* ---------- per-relation cached state (rd_amcache) ---------- */

/*
 * DESIGN.md §24 made the cached state TWO structs, because an index now has
 * one key column per entry run and each of them has an opclass of its own:
 *
 *	LionState		everything about ONE key column - its key type, hash,
 *					equality, ordering and multi-key extraction.  Every
 *					function that looks a key up, builds an entry or compares
 *					two of them takes the state of the column it is working
 *					on, so the great majority of this extension did not have
 *					to change at all.
 *	LionIndexState	what belongs to the relation: the meta page (and with it
 *					the cached directory root) and the array of columns.
 *
 * `ix` is the column's way back to the relation, and `attno` is the column
 * number every entry tuple of that column carries.  lion_get_state() hands
 * back column 1, which is what a single-column index has and all a caller
 * that predates §24 ever means.
 */
typedef struct LionIndexState LionIndexState;

typedef struct LionState
{
	LionIndexState *ix;			/* the index this column belongs to; NULL in
								 * the throw-away states the cost model and
								 * the planner build for one extractQuery
								 * call */
	uint16		attno;			/* 1-based key column number (DESIGN.md §24) */

	/*
	 * The KEY type: what an entry tuple stores, hashes and compares.  It is
	 * the type of the index's own tuple descriptor column, which core has
	 * already resolved from the opclass: the indexed column's type for a
	 * scalar opclass, and the opclass STORAGE type - with a polymorphic
	 * anyelement replaced by the column's element type - for a multi-key one
	 * (ConstructTupleDescriptor() in catalog/index.c does that, so a
	 * roaring array_ops index on text[] has a text key column).
	 */
	Oid			typid;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	Oid			collation;
	FmgrInfo	hashproc;		/* opclass support proc 1, or the key type's
								 * default hash opclass proc */
	bool		imagehash;		/* hashproc is lion_image_hash(): the key
								 * type has a btree class and no hash one */
	FmgrInfo	eqproc;			/* strategy-1 operator's function, or the key
								 * type's default equality */

	/*
	 * ORDERING (DESIGN.md §21).  Support proc 4 is a btree comparison
	 * function for the KEY type; when the opclass has none - a polymorphic
	 * multi-key class - the key type's own default btree comparison is used
	 * instead.  `ordered` is false when the type has no btree opclass at all,
	 * and then the directory is ordered by (kind, hash) alone: still a
	 * well-defined tree, but its entry order is not the type's order, so
	 * lion_index_stats() reports ordered = false and the count pushdown
	 * claims no pathkeys for it.
	 *
	 * ltopr is the `<` operator of the same ordering, which ambuild's
	 * tuplesort needs; an ordering without one is treated as no ordering,
	 * because the build and the search must agree on the order exactly.
	 */
	bool		ordered;
	FmgrInfo	cmpproc;		/* valid iff ordered */
	Oid			ltopr;			/* valid iff ordered */

	/*
	 * The comparison has a SQL-standard (`RETURN`) body, which has no source
	 * the recorded order could recognise it by, so a BUILD lays this column
	 * out in hash order instead and says so (DESIGN.md §21).
	 */
	bool		sqlbodycmp;

	/*
	 * Multi-key opclasses (DESIGN.md §17): one indexed value yields many
	 * keys through GIN's extractValue, and a query yields keys and a mode
	 * through GIN's extractQuery.  multikey is false for every scalar
	 * opclass, and then neither FmgrInfo is valid.
	 */
	bool		multikey;
	FmgrInfo	extractvalue;	/* support proc 2 */
	FmgrInfo	extractquery;	/* support proc 3 */

	/*
	 * The column STORES POSITIONS (DESIGN.md §17, "Stored positions"): its
	 * multi-key opclass has support proc 5, which hands back the positions of
	 * every key proc 2 extracts, in the same order.  Every VALUE entry of the
	 * column then carries LION_ENTRY_POSITIONS.
	 */
	bool		positions;
	FmgrInfo	positionsproc;	/* support proc 5 */

	/*
	 * The column has SUMMARY POSTING SETS (DESIGN.md §32): its bit in the meta
	 * page's summary_cols.  Only ever set on an ordered scalar column, and only
	 * on a version 7 index; inserts keep the summaries current and ranges read
	 * them.
	 */
	bool		summarized;

	/*
	 * The column is BIT-SLICED (DESIGN.md §51): its bit in the meta page's
	 * slice_cols.  Only ever set on a scalar column whose key type
	 * lion_slice_type_nbits() takes, and only on a version 15 index; slicebits
	 * is that width, 16, 32 or 64.  Inserts keep the slices current;
	 * lion_index_slice_agg() reads them.
	 */
	bool		sliced;
	int			slicebits;
} LionState;

struct LionIndexState
{
	LionMetaPageData meta;		/* copy of the meta page */
	Oid			indexrelid;		/* the index, for its name in an ERROR */
	int			ncolumns;		/* key columns, 1 .. INDEX_MAX_KEYS (§24) */
	LionState  *cols;			/* [ncolumns]; cols[i] is key column i + 1 */

	/*
	 * The backend's pg_proc invalidation count when the recorded order's
	 * comparisons were last checked against the catalog (§21): a cached
	 * state is checked again once a function has changed since.
	 */
	uint64		procgen;
};

/* The state of one key column of an index whose state is already in hand. */
static inline LionState *
lion_column(LionIndexState *ix, AttrNumber attno)
{
	Assert(attno >= 1 && attno <= ix->ncolumns);
	return &ix->cols[attno - 1];
}

/*
 * The key column whose postings count the index's ROWS (DESIGN.md §18,
 * "Statistics"): its first scalar one.  A scalar column files every row the
 * index holds under exactly one of its entries - the NULL entry for a NULL -
 * so the ntids of its entries add up to the rows, where the postings of the
 * whole index count a row once per key column, once per element of a
 * multi-key column, and again in a SUMMARY of its bucket.  0 when every key
 * column is multi-key: a row may be filed under any number of their entries,
 * and nothing short of the union of them all says how many rows there are.
 */
static inline AttrNumber
lion_index_row_column(const LionIndexState *ix)
{
	int			i;

	for (i = 0; i < ix->ncolumns; i++)
	{
		if (!ix->cols[i].multikey)
			return (AttrNumber) (i + 1);
	}
	return InvalidAttrNumber;
}

/* ---------- multi-key extraction (DESIGN.md §17, lion_multikey.c) ---------- */

/*
 * Strategy numbers of the roaring AM.  1 is equality, which every scalar
 * opclass has; 2 .. 5 belong to the multi-key classes; 6 .. 9 are the range
 * comparisons of DESIGN.md §28, in btree's order, which an ORDERED scalar
 * class has beside its proc 4, and 10 is `<>` (§35), which it has beside
 * them: every entry but one, a range with a hole in it.  11 .. 16 are
 * jsonb_contains_ops' (DESIGN.md §42, §43), which are multi-key too.  17 is
 * array `=`, which the keys only bound - the rows holding every element of
 * the query, rechecked - and 18 tsvector's `@@@`, `@@` under its deprecated
 * name, which a scan takes as 5 (LION_STRAT_CANONICAL()); a bitmap scan is
 * all that answers either, as for GIN.
 */
#define LION_STRAT_EQUAL			1
#define LION_STRAT_CONTAINS		2	/* anyarray @> anyarray */
#define LION_STRAT_OVERLAP		3	/* anyarray && anyarray */
#define LION_STRAT_CONTAINED		4	/* anyarray <@ anyarray */
#define LION_STRAT_MATCH			5	/* tsvector @@ tsquery */
#define LION_STRAT_LT			6	/* key < value (§28) */
#define LION_STRAT_LE			7	/* key <= value */
#define LION_STRAT_GE			8	/* key >= value */
#define LION_STRAT_GT			9	/* key > value */
#define LION_STRAT_NE			10	/* key <> value (§35) */
#define LION_STRAT_JSONB_CONTAINS	11	/* jsonb @> jsonb (§42) */
#define LION_STRAT_JSONB_EXISTS	12	/* jsonb ? text */
#define LION_STRAT_JSONB_EXISTS_ANY	13	/* jsonb ?| text[] */
#define LION_STRAT_JSONB_EXISTS_ALL	14	/* jsonb ?& text[] */
#define LION_STRAT_JSONPATH_EXISTS	15	/* jsonb @? jsonpath (§43) */
#define LION_STRAT_JSONPATH_MATCH	16	/* jsonb @@ jsonpath */
#define LION_STRAT_ARRAY_EQUAL	17	/* anyarray = anyarray (§17) */
#define LION_STRAT_MATCH_OLD		18	/* tsvector @@@ tsquery, which is @@ */
#define LION_NSTRATEGIES			18

/* a strategy as the scan answers it: `@@@` is `@@` */
#define LION_STRAT_CANONICAL(s) \
	((s) == LION_STRAT_MATCH_OLD ? LION_STRAT_MATCH : (s))

/* a multi-key strategy (§17) */
#define LION_STRAT_IS_MULTI(s) \
	(((s) >= LION_STRAT_CONTAINS && (s) <= LION_STRAT_MATCH) || \
	 ((s) >= LION_STRAT_JSONB_CONTAINS && (s) <= LION_STRAT_JSONPATH_MATCH))
/* a jsonpath one, which the keys only ever bound (§43) */
#define LION_STRAT_IS_JSONPATH(s) \
	((s) == LION_STRAT_JSONPATH_EXISTS || (s) == LION_STRAT_JSONPATH_MATCH)
/* ... and one whose rows the posting sets can select: all but `<@` */
#define LION_STRAT_IS_MULTI_SEARCH(s) \
	(LION_STRAT_IS_MULTI(s) && (s) != LION_STRAT_CONTAINED)

#define LION_STRAT_IS_RANGE(s) \
	((s) >= LION_STRAT_LT && (s) <= LION_STRAT_GT)
/* a bound of a walk (LionRange): a range comparison, or the hole `<>` makes */
#define LION_STRAT_IS_WALK(s) \
	(LION_STRAT_IS_RANGE(s) || (s) == LION_STRAT_NE)
/* a LOWER bound (>=, >) as opposed to an upper one (<, <=) */
#define LION_STRAT_IS_LOWER(s) \
	((s) == LION_STRAT_GE || (s) == LION_STRAT_GT)

/* Support procedure numbers. */
#define LION_HASH_PROC			1
#define LION_EXTRACTVALUE_PROC	2
#define LION_EXTRACTQUERY_PROC	3
#define LION_CMP_PROC			4	/* btree comparison of the KEY type (§21) */
#define LION_POSITIONS_PROC		5	/* word positions per extracted key (§17) */
#define LION_NPROC				5

/*
 * A search mode of lion's own, beyond GIN's (access/gin.h), that a multi-key
 * extractQuery may answer: the AND/OR its strategy makes of the keys selects
 * a SUPERSET of the rows, which the caller rechecks (jsonb_contains_ops'
 * `@>` with a multi-leaf array element, DESIGN.md §42).
 */
#define LION_SEARCH_MODE_LOSSY	16

/*
 * What a query over a multi-key index selects.
 *
 *	LION_QMODE_NONE	nothing at all (`tags && '{}'`)
 *	LION_QMODE_KEYS	exactly the rows the key tree selects; no recheck
 *	LION_QMODE_LOSSY	at least the rows the query selects, and perhaps
 *					more: the key tree selects a SUPERSET, which the caller
 *					rechecks (lion_extract_query_superset() only: a phrase
 *					as the AND of its lexemes, `a & !b` as `a`)
 *	LION_QMODE_ALL	every indexed row, with recheck (`tags @> '{}'`, `<@`,
 *					a tsquery with NOT/phrase/prefix/weights, a NULL key)
 */
typedef enum LionQueryMode
{
	LION_QMODE_NONE = 0,
	LION_QMODE_KEYS,
	LION_QMODE_LOSSY,
	LION_QMODE_ALL
} LionQueryMode;

/*
 * A boolean tree over extracted keys.  A leaf names a key by its position in
 * the array the extraction produced; the count code reuses the very same
 * structure with keyno naming a located posting set instead, which is what
 * lets one evaluator serve both the bitmap scan and the count pushdown.
 */
typedef enum LionKeyNodeKind
{
	LION_KN_KEY = 0,
	LION_KN_AND,
	LION_KN_OR,
	LION_KN_POSFILTER,			/* its one child's members, those of them a
								 * tsquery matches by the stored positions
								 * (lion_posfilter.c); the count's only */
	LION_KN_ANDNOT				/* its first child's members less every other
								 * child's: `col IS NOT NULL` beside other
								 * quals, the column's NULL entry subtracted
								 * (DESIGN.md §14, §29.2); the scans' only */
} LionKeyNodeKind;

/*
 * What a LION_KN_POSFILTER node decides its child's members with: the
 * arguments of lion_posfilter_begin(), which every cursor of the node calls
 * for a filter of its own.
 */
typedef struct LionPosSpec
{
	Relation	index;
	LionState  *col;
	Datum		query;
	StrategyNumber strategy;
	Size		mem;			/* what each such filter holds, at most
								 * (lion_posfilter_mem()) */
} LionPosSpec;

typedef struct LionKeyNode
{
	LionKeyNodeKind kind;
	int			keyno;			/* LION_KN_KEY only */
	int			nargs;
	struct LionKeyNode **args;
	LionPosSpec *pos;			/* LION_KN_POSFILTER only */
} LionKeyNode;

typedef struct LionQuery
{
	LionQueryMode mode;
	int			nkeys;			/* keys the extraction produced */
	Datum	   *keys;
	LionKeyNode *tree;			/* LION_QMODE_KEYS and LOSSY; over keys[] */
} LionQuery;

/*
 * Extract the keys of one indexed value with support proc 2, drop the NULL
 * ones and the duplicates, and return them in *keys (palloc'd in the current
 * context) with the count as the return value.  Zero keys means the row
 * belongs in a reserved entry, and *nokey (when nokey is not NULL) says
 * which: LION_ENTRY_EMPTYKEY when nothing was extracted at all - or
 * LION_ENTRY_EMPTYDIM or LION_ENTRY_EMPTYINNER for an array with no element
 * that has dimensions - and LION_ENTRY_NULLELEMS when every key extracted
 * was NULL (DESIGN.md §17).
 */
extern int lion_extract_value(LionState *state, Datum value, Datum **keys,
							  uint16 *nokey);

/*
 * Is fn the extraction of an array's elements, lion's array_ops' or GIN's
 * (DESIGN.md §17)?  And the reserved entry of an array with no element by
 * its dimensions - LION_ENTRY_EMPTYKEY, LION_ENTRY_EMPTYDIM or
 * LION_ENTRY_EMPTYINNER - or 0 for one with an element.
 */
extern bool lion_extracts_array_elements(PGFunction fn);
extern uint16 lion_array_empty_flag(Datum value);

/*
 * The positions of one extracted key, as support proc 5 returns them: npos
 * WordEntryPos values (weight << 14 | position), ascending by position.
 * npos 0 is a key of a tsvector stripped of positions.
 */
typedef struct LionKeyPositions
{
	uint16		npos;
	const uint16 *pos;
} LionKeyPositions;

/*
 * lion_extract_value() for a column that stores positions: (*pos)[i] are the
 * positions of (*keys)[i].
 */
extern bool lion_is_length_key(const LionState *state, Datum key);
extern int lion_extract_value_pos(LionState *state, Datum value, Datum **keys,
								  LionKeyPositions **pos, uint16 *nokey);
extern void lion_posmember_from_key(LionPosMember *m, uint64 code,
									const LionKeyPositions *kp);

/*
 * An aligned copy of the INLINE entry's positions chunk into buf (capacity
 * LION_CONTAINER_MAX_SIZE): false when it has no positions section.  A
 * section that is not one well-formed chunk is an ERROR.
 */
extern bool lion_entry_inline_poschunk(Relation index, const LionEntryTuple *e,
									   Size itemsz, BlockNumber blkno,
									   OffsetNumber off, LionContainer *buf);

/*
 * Extract a query with support proc 3 and work out how its keys combine
 * (DESIGN.md §17).  Everything is palloc'd in the current context.
 */
extern void lion_extract_query(LionState *state, Datum query,
							  StrategyNumber strategy, LionQuery *q);

/*
 * The same, for a caller that rechecks what it is handed (the count pushdown's
 * heap recheck of a query it only sees at run time, DESIGN.md §17): a query
 * the key sets cannot answer exactly comes back LION_QMODE_LOSSY with a tree
 * that selects a superset of its rows, where one narrower than every row
 * exists, and LION_QMODE_ALL where none does.  keys[] then holds only the
 * keys the tree names.  A query lion_extract_query() answers exactly comes
 * back the same here.
 *
 * A jsonpath query (§43) names PREFIXES of keys.  With index, the executor's
 * call, each is replaced by the OR of the index's keys that start with it.
 * Without one, at plan time, each stands for that OR as one key and must
 * never be looked up.
 */
extern void lion_extract_query_superset(LionState *state, Relation index,
									   Datum query, StrategyNumber strategy,
									   LionQuery *q);

/* Key tree nodes, for an extractQuery of lion's own that builds its tree. */
extern LionKeyNode *lion_keynode_leaf(int keyno);
extern LionKeyNode *lion_keynode_op(LionKeyNodeKind kind, LionKeyNode **args,
									int nargs);

/*
 * tsvector_ops' keys (DESIGN.md §17, "Long lexemes"): a lexeme of up to
 * LION_LEXEME_KEY_MAX bytes is its own key, and a longer one is filed under
 * a key of exactly that many - its first bytes, up to LION_LEXEME_KEY_KEEP of
 * them, then '#', then its SHA-256 in hex.  A key of that length is
 * ambiguous, and a query that names one is rechecked.
 * lion_lexeme_prefix_max() is the longest prefix every hashed key keeps.
 */
#define LION_LEXEME_KEY_MAX		(LION_MAX_KEY_SIZE - VARHDRSZ)	/* 2043 */
#define LION_LEXEME_KEY_HASH	64	/* SHA-256 in hex */
#define LION_LEXEME_KEY_KEEP	(LION_LEXEME_KEY_MAX - LION_LEXEME_KEY_HASH - 1)

extern text *lion_lexeme_key(const char *lex, int len, bool *ambiguous);
extern int	lion_lexeme_prefix_max(void);
extern Datum lion_tsvector_extract_value(PG_FUNCTION_ARGS);
extern Datum lion_tsvector_extract_query(PG_FUNCTION_ARGS);

/*
 * array_ops' keys (DESIGN.md §17, "Long elements"): a text, varchar or bytea
 * element longer than LION_LEXEME_KEY_MAX bytes is filed under a hashed key
 * as a long lexeme is, and a key of LION_LEXEME_KEY_MAX bytes is ambiguous.
 */
extern Datum lion_array_extract_value(PG_FUNCTION_ARGS);
extern Datum lion_array_extract_query(PG_FUNCTION_ARGS);

/*
 * Whether a key of the column may stand for values other than itself - a
 * hashed key of a long lexeme or array element, or a value spelled like one -
 * and the values of a heap row's value that are filed under such a key
 * (lion_index_facets(), which shows them in its place).
 */
extern bool lion_key_is_ambiguous(const LionState *state, Datum key);
extern int	lion_ambiguous_values(LionState *state, Datum value, Datum key,
								  Datum **values);

/* jsonb_contains_ops' extractQuery (lion_jsonb.c), which hands over a tree */
extern Datum lion_jsonb_extract_query(PG_FUNCTION_ARGS);
/* ... its extractValue, and one of its keys as text (lion_index_facets()) */
extern Datum lion_jsonb_extract_value(PG_FUNCTION_ARGS);
extern char *lion_jsonb_key_out(const bytea *key);
extern bool lion_tsquery_item_keys(LionState *state, Datum query,
								   StrategyNumber strategy, Datum **itemkeys);

/*
 * Prefix lexemes (DESIGN.md §17, "Prefix lexemes").  lion_tsquery_expand_prefixes()
 * rewrites each `foo:*` of a tsquery into the OR of the lexemes the index
 * holds that start with foo, which every caller then answers like any other
 * query; *expanded says whether it could (false: the query comes back as it
 * was).  lion_tsquery_strip_prefixes() is the planner's stand-in, before any
 * index is open: `foo:*` becomes the lexeme `foo`, which has the expansion's
 * shape.  lion_tsquery_has_prefix() says whether a query has one at all.
 */
extern Datum lion_tsquery_expand_prefixes(Relation index, LionState *col,
										  Datum query, bool *expanded);
extern Datum lion_tsquery_strip_prefixes(Datum query);
extern bool lion_tsquery_has_prefix(Datum query);


/* ---------- the page layer (lion_pages.c, lion_meta.c, lion_state.c, lion_entry.c,
 * lion_posting_put.c): primitives shared by build/insert/scan/vacuum ---------- */

/* The whole cached state of an index, built on first use (DESIGN.md §24). */
extern LionIndexState *lion_get_index_state(Relation index);

/*
 * The state of one key column.  attno is the INDEX column number, 1-based, as
 * it appears in a ScanKey's sk_attno and in IndexOptInfo->indexkeys[i] + 1 -
 * never a heap attribute number.  ERRORs when the index has no such column.
 */
extern LionState *lion_index_column_state(Relation index, AttrNumber attno);

/* Column 1, which is all a single-column index has. */
extern LionState *lion_get_state(Relation index);

/* Key columns of an index, without building its state. */
#define lion_index_ncolumns(index) \
	((int) IndexRelationGetNumberOfKeyAttributes(index))

extern void lion_init_page(Page page, uint16 flags);

/*
 * Take a block for the index WITHOUT starting a record: a recycled one from
 * the free space map when one is safe to take, else a fresh one from
 * extending the relation (DESIGN.md §18).  The buffer comes back pinned and
 * EXCLUSIVE with an uninitialised page, which the caller initialises in its
 * record with lion_wal_init_buffer().
 *
 * heaprel is what GlobalVisCheckRemovableFullXid() needs in order to decide
 * whether a DELETED page's safexid is old enough for the page to be reused;
 * NULL means "never recycle", which is what ambuildempty wants.  reuse =
 * false is the same thing for one call, and is how a chain's HEAD page is
 * allocated: head blocks must be unique for the life of the index, because
 * that is what makes owner_head a chain identity a reader can trust.
 *
 * This is the fallible half of allocating a page, and DESIGN.md §25 requires
 * it to happen BEFORE lion_wal_begin(): in rmgr mode the record is written
 * inside a critical section, where extending a relation would turn a full
 * disk into a PANIC.  Every split therefore works out how many pages it needs
 * from the page it already holds, takes them here, and only then opens its
 * record.
 */
extern Buffer lion_alloc_page(Relation index, Relation heaprel, bool reuse);

extern void lion_init_metapage(Page page, uint32 inline_limit,
							  BlockNumber root, uint32 height,
							  uint32 dirpages, uint32 wal_mode);

/* Stamp a container page with the chain it belongs to (DESIGN.md §18). */
static inline void
lion_page_set_owner(Page page, uint32 hash, BlockNumber head)
{
	LionPageOpaque opaque = LionPageGetOpaque(page);

	opaque->owner_hash = hash;
	opaque->owner_head = head;
}

/*
 * Mark a container page free.  The page keeps its owner (verify() and the
 * leak sweep read it) but loses everything else; safexid is the transaction
 * id from which on nothing can still hold a link to it.
 */
extern void lion_page_set_deleted(Page page, FullTransactionId safexid);
extern FullTransactionId lion_page_get_safexid(Page page);
extern void lion_check_key_offset(ItemPointer tid);	/* ERROR if offset > LION_MAX_OFFSET */
extern bool lion_table_am_supported(Relation heap);	/* the heap AM only (lion_am.c) */
extern void lion_check_table_am(Relation heap);	/* ERROR if not supported */

/* Key handling */
extern uint32 lion_hash_key(LionState *state, Datum key);
extern Size lion_key_datum_size(LionState *state, Datum key);	/* bytes needed to store the key */
extern Datum lion_key_prepare(LionState *state, Datum key, Relation heap,
							  ItemPointer tid);	/* the key as stored; ERROR if too long */
extern void lion_store_key(LionState *state, Datum key, char *dest);
extern Datum lion_fetch_key(LionState *state, const char *src);

/*
 * The layout flags an entry of this column's keys carries: LION_ENTRY_ALIGNKEY
 * for a key type passed by reference whose alignment is double
 * (lion_entryfmt.h), none otherwise.
 */
static inline uint16
lion_key_layout_flags(const LionState *state)
{
	return (!state->typbyval && state->typalign == 'd') ?
		LION_ENTRY_ALIGNKEY : 0;
}

/*
 * A key read off a page, checked for the form lion_store_key() gives it
 * before any opclass function sees it (lion_state.c): lion_key_is_valid()
 * says whether it has it, lion_check_key() raises ERRCODE_INDEX_CORRUPTED
 * when it has not, and lion_entry_key() is lion_fetch_key() of an entry's
 * key once lion_check_key() has passed it.
 */
extern bool lion_key_is_valid(LionState *state, const char *key, Size keylen);
extern void lion_check_key(LionState *state, const char *key, Size keylen);
extern Datum lion_entry_key(LionState *state, const LionEntryTuple *entry);

/*
 * THE HASH OF AN ENTRY (DESIGN.md §21), which the directory order compares
 * after proc 4 - or first, in a column without one - and which no entry
 * stores: lion_hash_key() of the stored key for a VALUE entry, and of a
 * pivot made of one, and 0 (LION_NULLKEY_HASH, LION_SUMMARY_HASH) for every
 * other kind.  `state` is the column of the entry's attno.
 */
static inline uint32
lion_entry_hash(LionState *state, const LionEntryTuple *entry)
{
	if (lion_entry_kind(entry) != LION_KIND_VALUE)
		return 0;
	return lion_hash_key(state, lion_entry_key(state, entry));
}

/*
 * THE OWNER STAMP of the pages of a CHAIN entry's posting tree and position
 * tree (DESIGN.md §18): what LionPageOpaqueData.owner_hash holds beside the
 * root.  A hash of the stored key's bytes for a VALUE entry, and 0 for every
 * other kind - for a summary because the open bucket's key is raised in
 * place, and its pages must not then claim another entry.  It needs no
 * opclass function, so every reader that holds the entry can check a page.
 */
extern uint32 lion_owner_stamp(int kind, const char *key, Size keylen);

static inline uint32
lion_entry_owner(const LionEntryTuple *entry)
{
	return lion_owner_stamp(lion_entry_kind(entry), LionEntryGetKey(entry),
							entry->keylen);
}
extern bool lion_keys_equal(LionState *state, Datum a, Datum b);

/* ---------- lion_dir.c: the sorted entry directory (DESIGN.md §21) ---------- */

/*
 * What a descent compares stored entries against.
 *
 *	attno		the KEY COLUMN being searched (DESIGN.md §24), which is the
 *				leading term of the order
 *	col			that column's state: what fetches a stored key, and where a
 *				caller that did not override them took cmpproc/eqproc from.
 *				It may be the state of ANOTHER index's column only in the
 *				sense that the caller owns it; nothing here reads the meta
 *				page through it
 *	kind		LION_KIND_NULL / EMPTY / VALUE
 *	key, hash	the value being looked for, and its hash (the hash must be the
 *				one the index would have stored for it, which for a cross-type
 *				search is the SEARCH type's own hash function: a roaring
 *				opfamily's types hash compatibly)
 *	hashlazy	hash is not filled in: it is lion_hash_key(col, key), and the
 *				comparison computes it the first time the order needs it,
 *				which in an ordered column is only on a proc-4 tie (a key
 *				made from a stored entry, lion_search_key_exact())
 *	cmpproc		comparison of a STORED key against this one, stored key on the
 *				LEFT; NULL means the index has no ordering and the prefix is
 *				(kind, hash) alone
 *	eqproc		equality of a stored key against this one, stored key on the
 *				left; this and not the comparison is what decides that two
 *				keys are ONE entry
 *	raw/rawlen	the search value in its STORED form, when the caller has it.
 *				NULL makes the key compare as the smallest member of its own
 *				prefix run, which is what positions a lookup at the start of
 *				the run it then scans; a caller that wants the exact insert
 *				position supplies it.
 */
typedef struct LionSearchKey
{
	uint16		attno;
	LionState  *col;
	int			kind;
	Datum		key;
	uint32		hash;
	bool		hashlazy;
	FmgrInfo   *cmpproc;
	FmgrInfo   *eqproc;
	Oid			collation;
	const char *raw;
	Size		rawlen;
} LionSearchKey;

/* A search key for a value of that column's own key type. */
extern void lion_search_key_init(LionState *state, LionSearchKey *sk,
								int kind, Datum key, uint32 hash);

/*
 * ... and one that positions exactly, from a built entry tuple.  The COLUMN
 * comes from the entry, which is why this takes the index state and not one
 * column's: a directory page holds entries of every column, and the repair of
 * an unfinished split builds this from whatever item the page happens to
 * carry (DESIGN.md §24).
 */
extern void lion_search_key_exact(LionIndexState *ix, LionSearchKey *sk,
								 const LionEntryTuple *entry);

/*
 * Compare a stored entry tuple with a search key.  <0: the item sorts first.
 * Everything the comparison needs is in the search key, the column state it
 * carries included, so an item of ANOTHER column is answered by the leading
 * attno term without touching a single opclass function.
 */
extern int	lion_cmp_entry(const LionEntryTuple *item, const LionSearchKey *sk);
/* The same, stopping before the bytewise tail (DESIGN.md §21). */
extern int	lion_cmp_prefix(const LionEntryTuple *item, const LionSearchKey *sk);
/* Compare two stored entry tuples in full; used by verify() and the build. */
extern int	lion_cmp_entries(LionIndexState *ix, const LionEntryTuple *a,
							 const LionEntryTuple *b);

/* The root block of the directory, refreshing the cached copy if need be. */
extern BlockNumber lion_dir_root(Relation index, LionIndexState *ix,
								uint32 *height);
/*
 * The leftmost leaf, where an ordered walk of every entry starts.  _ext reads
 * every page but the meta page through strategy: VACUUM's (info->strategy),
 * whose walk starts there (DESIGN.md §18).
 */
extern BlockNumber lion_dir_leftmost_leaf(Relation index, LionIndexState *ix);
extern BlockNumber lion_dir_leftmost_leaf_ext(Relation index, LionIndexState *ix,
											  BufferAccessStrategy strategy);

/*
 * The leaf where key column `col`'s run of entries begins, and the offset of
 * its first entry on that leaf (DESIGN.md §24).  The run ends at the first
 * entry of the next column, which a walk recognises by its attno.
 */
extern BlockNumber lion_dir_column_first(Relation index, LionState *col,
										 OffsetNumber *offp);

/*
 * For the planner's endpoint probe (DESIGN.md §28): the leaf where the
 * column's VALUE entries begin, share-locked, with *offp the first of them,
 * and the leaf where they end, with *offp the first item past them; and the
 * left sibling of a directory page, found within maxsteps pages or given up
 * on (InvalidBuffer), releasing the page it comes from.
 */
extern Buffer lion_dir_value_start(Relation index, LionState *col,
								   OffsetNumber *offp);
extern Buffer lion_dir_value_end(Relation index, LionState *col,
								 OffsetNumber *offp);
extern Buffer lion_dir_step_left(Relation index, Buffer buf, int maxsteps);

/* The first data item of a directory page (offset 1, or 2 under a high key). */
static inline OffsetNumber
lion_page_first_data(Page page)
{
	return LionPageIsRightmost(page) ? FirstOffsetNumber :
		OffsetNumberNext(FirstOffsetNumber);
}

/*
 * The first data item of a directory page that carries a key: past the
 * minus-infinity downlink every internal page begins with (DESIGN.md §21).
 * One past the last item when there is none.
 */
extern OffsetNumber lion_dir_first_keyed(Page page);

extern void lion_entry_size_error(Page page, ItemId iid);

/*
 * The entry a directory page's item holds, its size checked against the
 * header and the key it claims: an ERROR (ERRCODE_INDEX_CORRUPTED) when the
 * item does not hold them whole, since every reader compares or decodes the
 * keylen bytes after the header its flags give it, and a damaged keylen or
 * flags word would send it past the item - and long enough for the
 * positions extension its flags claim, which is where LION_ENTRY_PAYLOAD_LEN() starts counting: an
 * item shorter than that would give a payload length wrapped around zero.  A
 * few compares per item, so readers can afford it on every entry they look
 * at; writers have lion_page_entry_fetch(), which checks more.
 */
static inline LionEntryTuple *
lion_entry_at(Page page, ItemId iid)
{
	LionEntryTuple *e = (LionEntryTuple *) PageGetItem(page, iid);
	Size		len = ItemIdGetLength(iid);

	if (unlikely((Size) ItemIdGetOffset(iid) + len > BLCKSZ ||
				 len < LION_ENTRY_SHORT_HDRSZ ||
				 len < LionEntryPayloadOffset(e)))
		lion_entry_size_error(page, iid);
	return e;
}

static inline LionEntryTuple *
lion_page_entry(Page page, OffsetNumber off)
{
	return lion_entry_at(page, PageGetItemId(page, off));
}

/*
 * Descend to the leaf that owns sk and return it locked in lockmode, with
 * *offp the offset of the first item that does not sort before sk.  With
 * forwrite, internal pages whose split never finished are repaired on the way
 * (DESIGN.md §21) and lockmode must be BUFFER_LOCK_EXCLUSIVE.
 */
extern Buffer lion_dir_search(Relation index, Relation heaprel,
							 LionIndexState *ix, const LionSearchKey *sk,
							 int lockmode, bool forwrite, OffsetNumber *offp);
/*
 * The same with every page but the meta page read through strategy: VACUUM's
 * (info->strategy), which finds a summary again this way after an insert
 * moved it (DESIGN.md §18).  A split it finishes on the way reads without it.
 */
extern Buffer lion_dir_search_ext(Relation index, Relation heaprel,
								  LionIndexState *ix, const LionSearchKey *sk,
								  int lockmode, bool forwrite,
								  OffsetNumber *offp,
								  BufferAccessStrategy strategy);

/*
 * The level-1 page a read-only descent took its leaf's downlink from, for a
 * caller that looks keys up in their order (lion_lookup_walk_find()): its
 * block, and a copy of its high key as it was then - in hikey, which the
 * caller allocates at LION_MAX_PIVOT_TUPLE bytes - or
 * that it was the rightmost page of its level.  blk is InvalidBlockNumber
 * when there is none: a directory of one level, or a high key that did not
 * look like one.
 */
typedef struct LionDirParent
{
	BlockNumber blk;
	bool		rightmost;
	Size		hikeylen;
	LionEntryTuple *hikey;
} LionDirParent;

/*
 * lion_dir_search() for a reader, BUFFER_LOCK_SHARE, that records the level-1
 * page it went through in *parent - and starts there, instead of at the root,
 * when parent->blk is valid.  The caller vouches that sk does not sort below
 * any key that page had when it was recorded: a page's lower bound never
 * rises, and a page deleted since passes its range to the right (DESIGN.md
 * §21, "Page deletion"), so sk's leaf is below it or below a page to its
 * right, which the descent moves right to as it always does.
 */
extern Buffer lion_dir_search_from(Relation index, LionIndexState *ix,
								   const LionSearchKey *sk,
								   LionDirParent *parent, OffsetNumber *offp);

/*
 * The same, handing back the first ITEM at or after sk instead: the leaf is
 * the one that item is on, stepping right from the landing leaf when sk sorts
 * after everything on it.
 */
extern Buffer lion_dir_search_first(Relation index, Relation heaprel,
									LionIndexState *ix, const LionSearchKey *sk,
									int lockmode, bool forwrite,
									OffsetNumber *offp);

/*
 * The same read-only descent stopped at `level`: the page of that level whose
 * key range holds sk, locked SHARE, or InvalidBuffer when the directory is not
 * that tall.  For lion_index_verify(), which proves with it that a page a
 * concurrent split made is where a search for its keys lands (DESIGN.md §7).
 */
extern Buffer lion_dir_search_level(Relation index, LionIndexState *ix,
									const LionSearchKey *sk, uint16 level);

/*
 * Locate the entry for sk.  On true *buf is a leaf locked in lockmode and
 * *offnum its offset.  On false *buf is still a locked leaf and *offnum is
 * the offset the entry would be inserted at, EXCEPT when *movedright says the
 * scan had to follow a right link.  With forwrite that leaf is then the one
 * the prefix run is entered from, held since the lookup so that no other
 * writer can create the key meanwhile, and *offnum is invalid:
 * lion_dir_add_entry() walks right from it to the exact position (DESIGN.md
 * §21).  movedright may be NULL.
 */
extern bool lion_dir_find(Relation index, Relation heaprel, LionIndexState *ix,
						 const LionSearchKey *sk, int lockmode, bool forwrite,
						 Buffer *buf, OffsetNumber *offnum, bool *movedright);
/* ... reading through strategy, as lion_dir_search_ext() does */
extern bool lion_dir_find_ext(Relation index, Relation heaprel,
							 LionIndexState *ix, const LionSearchKey *sk,
							 int lockmode, bool forwrite, Buffer *buf,
							 OffsetNumber *offnum, bool *movedright,
							 BufferAccessStrategy strategy);

/*
 * lion_dir_find() with forwrite for the next SLICE key of a walk, in
 * ascending order, from the leaf *buf the caller holds EXCLUSIVE since it
 * found an earlier key of the walk: moves right as a descent would (and may
 * leave a leaf further right in *buf), never needs a guard (DESIGN.md §51,
 * "Insert cost, revisited").  hint, when valid, is the offset of the earlier
 * key on that leaf, where the search may begin.
 */
extern bool lion_dir_find_next(Relation index, Relation heaprel,
							   LionIndexState *ix, const LionSearchKey *sk,
							   OffsetNumber hint, Buffer *buf,
							   OffsetNumber *offnum);
/* ... and whether it would look on the held leaf buf itself */
extern bool lion_dir_leaf_holds(Buffer buf, const LionSearchKey *sk);

/*
 * The pieces of the above, for a caller that walks the leaves itself; _ext
 * reads the sibling through strategy.
 */
extern LionEntryTuple *lion_dir_highkey(Page page);
extern Buffer lion_dir_step_right(Relation index, Buffer buf, int lockmode,
								  LionRightWalk *walk);
extern Buffer lion_dir_step_right_ext(Relation index, Buffer buf, int lockmode,
									  LionRightWalk *walk,
									  BufferAccessStrategy strategy);
extern Buffer lion_dir_read_leaf(Relation index, BlockNumber blk);
extern OffsetNumber lion_dir_binsrch(Page page, const LionSearchKey *sk);
extern bool lion_dir_scan_run(Relation index, const LionSearchKey *sk,
							 int lockmode, Buffer *bufp, OffsetNumber *offp,
							 bool *movedright);

/*
 * Insert a brand new entry where lion_dir_find() with forwrite said it
 * belongs.  *bufp is held EXCLUSIVE and stays so; it is the caller's to
 * release.
 */
extern void lion_dir_add_entry(Relation index, Relation heaprel,
							  LionIndexState *ix, Buffer *bufp,
							  OffsetNumber off, bool movedright,
							  LionEntryTuple *entry, Size size);

/*
 * Put item at (buf, off) on a directory page the caller holds EXCLUSIVE,
 * replacing what is there or inserting before it, and splitting the page when
 * it does not fit.  buf stays locked and pinned; after a split the item may
 * live on the new right sibling instead.
 */
extern void lion_dir_place(Relation index, Relation heaprel, LionIndexState *ix,
						  Buffer buf, OffsetNumber off, bool replace,
						  LionEntryTuple *item, Size size);

/* Delete the items at the given ascending offsets in one record. */
extern void lion_dir_delete(Relation index, Buffer buf, OffsetNumber *offs,
						   int noffs);

/*
 * Page deletion (lion_dir_delete.c, DESIGN.md §21, "Page deletion").
 *
 * lion_dir_delete_page() is VACUUM's last step on the directory leaf leafblk:
 * when the leaf is empty it takes it out of the tree - with the internal pages
 * above it that have no other child - or it finishes doing so for a leaf an
 * earlier VACUUM left HALF_DEAD.  Nothing may be held on entry, and the
 * return value is the number of pages it marked DELETED.  strategy is
 * VACUUM's (info->strategy), which every page it reads but the meta page goes
 * through.
 *
 * lion_dir_past_deleted() hands back buf, a directory page locked in
 * lockmode, or - when that page is DELETED - the first page to its right that
 * is not, for the walks that take a downlink without comparing a key with the
 * page's high key; the pages it steps to are read through strategy (NULL, or
 * VACUUM's).
 */
extern int	lion_dir_delete_page(Relation index, LionIndexState *ix,
								 BlockNumber leafblk,
								 BufferAccessStrategy strategy);
extern Buffer lion_dir_past_deleted(Relation index, Buffer buf, int lockmode,
									BufferAccessStrategy strategy);

/* The safexid a DELETED directory page keeps in its special area. */
static inline FullTransactionId
lion_dir_page_safexid(Page page)
{
	LionPageOpaque opaque = LionPageGetOpaque(page);

	return FullTransactionIdFromU64(((uint64) opaque->maxckey << 32) |
									(uint64) opaque->minckey);
}

static inline void
lion_dir_page_set_safexid(Page page, FullTransactionId safexid)
{
	LionPageOpaque opaque = LionPageGetOpaque(page);
	uint64		x = U64FromFullTransactionId(safexid);

	opaque->minckey = (uint32) x;
	opaque->maxckey = (uint32) (x >> 32);
}

/*
 * Build an entry tuple in palloc'd memory; *size receives its total length.
 * It has no hash (lion_entryfmt.h): lion_entry_hash() recomputes it.
 */
extern LionEntryTuple *lion_make_entry(LionState *state, Datum key, uint16 flags,
									   const char *payload, Size payloadlen,
									   Size *size);

/*
 * A pivot - pivotflag is LION_ENTRY_HIGHKEY or LION_ENTRY_DOWNLINK - with
 * src's key and kind (a NULL src: the minus-infinity downlink), and child as
 * its downlink target (InvalidBlockNumber for a high key); and the size of
 * one made from src.
 */
extern LionEntryTuple *lion_make_pivot(const LionEntryTuple *src,
									   uint16 pivotflag, BlockNumber child,
									   Size *size);

static inline Size
lion_pivot_size(const LionEntryTuple *src)
{
	return lion_entry_payoff_for(LION_ENTRY_HIGHKEY |
								 (src->flags & LION_ENTRY_ALIGNKEY),
								 src->keylen);
}

/*
 * A private copy of an entry in the CHAIN header form, with its counts and
 * no payload: what a spill turns an INLINE entry into (lion_entry.c).
 */
extern LionEntryTuple *lion_entry_chain_shape(const LionEntryTuple *entry,
											  Size *size);

/*
 * The same for a reserved entry (DESIGN.md §14 and §17): no key, hash 0, and
 * reservedflag - exactly one of the flags of LION_ENTRY_RESERVED - set on
 * top of the caller's INLINE/CHAIN flag.  There is one of each PER KEY
 * COLUMN (DESIGN.md §24), which is why the column has to be named here.
 */
extern LionEntryTuple *lion_make_reserved_entry(AttrNumber attno,
											  uint16 reservedflag, uint16 flags,
											  const char *payload,
											  Size payloadlen, Size *size);

/*
 * The same for a SLICE entry (DESIGN.md §51): its one-byte key is pos, a bit
 * position or LION_SLICE_EXISTS, and LION_ENTRY_SLICE is set on top of the
 * caller's INLINE/CHAIN flag.
 */
extern LionEntryTuple *lion_make_slice_entry(AttrNumber attno, int pos,
										   uint16 flags, const char *payload,
										   Size payloadlen, Size *size);

/* The bit position a SLICE entry's key names (lion_slice_enc.h). */
static inline int
lion_entry_slice_pos(const LionEntryTuple *e)
{
	Assert(LionEntryIsSlice(e) && e->keylen == LION_SLICE_KEYLEN);
	return (int) *(const uint8 *) LionEntryGetKey(e);
}

/*
 * A search key for the SLICE entry of column state at pos (DESIGN.md §51):
 * the kind and the one-byte key, which the prefix compares (lion_dir.c).
 * posbyte is the caller's, and has to live as long as sk.
 */
extern void lion_search_key_slice(LionState *state, LionSearchKey *sk,
								  int pos, uint8 *posbyte);

/*
 * Locate the entry for a real key (the caller supplies its hash), or one of
 * the reserved key-less entries of DESIGN.md §14 and §17.  Both descend
 * the directory and hand back the leaf locked in lockmode; the caller
 * releases it.
 */
extern bool lion_find_entry(Relation index, LionState *state, int lockmode,
						   Datum key, uint32 hash, Buffer *buf,
						   OffsetNumber *offnum);

extern bool lion_find_reserved_entry(Relation index, LionState *state,
									int lockmode, uint16 reservedflag,
									Buffer *buf, OffsetNumber *offnum);

/* ... and a SLICE entry of a bit-sliced column (DESIGN.md §51). */
extern bool lion_find_slice_entry(Relation index, LionState *state,
								  int lockmode, int pos, Buffer *buf,
								  OffsetNumber *offnum);

/*
 * Add the row at (ckey, lo) to the SLICE of column state at pos, creating
 * the entry when there is none (lion_insert.c): one step of an insert into a
 * sliced column, and what lion_debug_slice_insert() damages a slice with.
 */
extern void lion_slice_insert_one(Relation index, Relation heaprel,
								  LionState *state, int pos, uint32 ckey,
								  uint16 lo);

static inline bool
lion_find_null_entry(Relation index, LionState *state, int lockmode,
					Buffer *buf, OffsetNumber *offnum)
{
	return lion_find_reserved_entry(index, state, lockmode,
								   LION_ENTRY_NULLKEY, buf, offnum);
}

/*
 * Warn once per backend per index when an index has grown past its
 * max_entries reloption (DESIGN.md §17).  nentries is an estimate; the
 * warning never rejects a row.
 */
extern void lion_warn_max_entries(Relation index, int64 nentries);

/* The max_entries reloption of an index, 0 when unlimited. */
extern int lion_max_entries(Relation index);

/*
 * The options of an index as a build reads them: copied out of the relcache
 * entry at once, or the defaults where the index has none.
 */
extern void lion_build_options(Relation index, LionOptions *opts);

/*
 * Replace the entry at (buf, offnum) with a new version (buf held EXCLUSIVE).
 * Handles size changes; if the new tuple cannot fit even after defragmenting
 * the page, returns false and changes nothing (caller must spill).
 * Caller supplies the open record if it wants the change batched with other
 * buffers (state may be NULL: then this function logs by itself).
 */
extern bool lion_replace_entry(Relation index, LionWalState *state, Buffer buf,
							  OffsetNumber offnum, LionEntryTuple *entry, Size size);

/* ---------- lion_posting.c: the per-key posting tree (DESIGN.md §22) ------- */

/*
 * Posting-tree navigation.  hash/head identify the set - head is its ROOT,
 * and the root block never changes for the life of the set, which is what
 * makes owner_head the identity DESIGN.md §18 relies on - and are checked
 * against every page the descent touches; a page that does not belong ends
 * the walk (the set was deleted after the caller copied the entry).
 *
 * lion_posting_search() hands back the LEAF that owns ckey, locked in
 * lockmode, or InvalidBuffer when the set is gone.  Internal pages are always
 * taken SHARE and released before the child is locked (no coupling: a split
 * holds the child while it locks the parent, so coupling downwards would close
 * a cycle buffer locks have no detector for - §21 makes the same argument for
 * the directory).  With forwrite the descent finishes every unfinished split
 * it meets and starts again, and lockmode must be BUFFER_LOCK_EXCLUSIVE.
 */
extern Buffer lion_posting_search(Relation index, Relation heaprel,
								  uint32 hash, BlockNumber head, uint32 ckey,
								  int lockmode, bool forwrite);

/*
 * lion_posting_search() that also says whether the head was a LEAF when the
 * descent read it: a set of at most LION_POSTING_FLAT_LEAVES leaves and no
 * internal level (DESIGN.md §22).  A cursor over such a set never descends
 * again to seek: a descent would start at the head and walk right from
 * there, and stepping right from where the cursor stands reads less.  The
 * answer stays usable after a push-down: the leaves are still the chain they
 * were, and stepping right along it is still correct.
 */
extern Buffer lion_posting_search_ext(Relation index, Relation heaprel,
									  uint32 hash, BlockNumber head,
									  uint32 ckey, int lockmode, bool forwrite,
									  bool before, bool *flat);

/*
 * lion_posting_search() routed by the last separator strictly below ckey
 * rather than at or below it: the leaf that holds what sorts just before
 * ckey (the leftmost leaf for ckey 0).  A position tree's separators repeat
 * when one ckey's members fill several leaves, and every one of those leaves
 * is to the right of this one (DESIGN.md §17).
 */
extern Buffer lion_posting_search_before(Relation index, Relation heaprel,
										 uint32 hash, BlockNumber root,
										 uint32 ckey, int lockmode,
										 bool forwrite);

/*
 * A reader's descent of the set rooted at head, stopped at `level`: the page
 * of that level the separators route ckey to, locked SHARE, or InvalidBuffer.
 * With `before`, the route of what sorts just below ckey, as
 * lion_posting_search_before() takes it.  For lion_index_verify() (DESIGN.md
 * §7), as lion_dir_search_level() is, and for VACUUM, which reads through its
 * ring: _ext's strategy, info->strategy.
 */
extern Buffer lion_posting_search_level(Relation index, BlockNumber head,
										uint32 ckey, uint16 level,
										bool before);
extern Buffer lion_posting_search_level_ext(Relation index, BlockNumber head,
											uint32 ckey, uint16 level,
											bool before,
											BufferAccessStrategy strategy);

/*
 * Take the empty leaf blk out of the posting set (hash, head) while the key
 * lives on (lion_posting_delete.c, DESIGN.md §22, "Empty leaves"): its left
 * neighbour's right link and its parent's downlink go to its right sibling,
 * and it is flagged LION_PAGE_UNLINKED, for a later VACUUM to recycle.  The
 * caller holds the key's directory leaf EXCLUSIVE; hint is a leaf it saw left
 * of blk, or InvalidBlockNumber.  False when the leaf may not go now.  Every
 * page is read through strategy, VACUUM's (info->strategy).
 */
extern bool lion_posting_unlink_leaf(Relation index, uint32 hash,
									 BlockNumber head, BlockNumber hint,
									 BlockNumber blk,
									 BufferAccessStrategy strategy);

/*
 * The SEPARATORS of the posting set rooted at head - the first container key
 * of every child of its root, in key order, and of every child of those
 * children for as long as there are fewer than `want` and the level below
 * can be read in at most maxpages pages in all - for the intersection
 * probe's sample by rank (DESIGN.md §29.11, "A sample of the driver's own
 * rows").  The leaves of a posting tree hold about equal bytes of it, so
 * the separators of one level are its quantiles by bytes.  palloc'd, *nsep
 * of them; *level the level whose pages they are the first keys of (1: the
 * leaves; 0, and NULL returned, for a tree with no internal level, whose
 * head is its only leaf or the first of a few), and *pages the pages read.
 * NULL as well when the set is gone (a page that does not belong to it).
 * A reader's SHARE locks, one page at a time, every pivot and downlink
 * checked as a descent checks them: a damaged one is an ERROR.
 */
extern uint32 *lion_posting_separators(Relation index, BlockNumber head,
									   int want, int maxpages, int *nsep,
									   int *level, int *pages);

/*
 * The leftmost leaf of the set rooted at head, read through strategy, or
 * InvalidBlockNumber when the set is gone: for VACUUM taking a leaf out of
 * the set (lion_posting_delete.c).
 */
extern BlockNumber lion_posting_leftmost_leaf(Relation index, BlockNumber head,
											  BufferAccessStrategy strategy);

/*
 * The container page that owns ckey in the posting set at head (DESIGN.md
 * §4), found by a descent (lion_posting_search()).
 */
extern BlockNumber lion_chain_find_page(Relation index, uint32 hash,
									   BlockNumber head, BlockNumber tail,
									   uint32 ckey);

/*
 * The child block of the downlink at off on internal posting page blk,
 * checked: off names a downlink the page has, its line pointer is one whole
 * pivot inside the page, and the child is a valid block number (which
 * ReadBuffer() would otherwise take for P_NEW and extend the index).  Anything
 * else is an ERROR, ERRCODE_INDEX_CORRUPTED.  For every descent of a posting
 * tree, lion_vacuum_descend() included.
 */
extern BlockNumber lion_posting_downlink(Relation index, Page page,
										 BlockNumber blk, OffsetNumber off);

/*
 * Push the head of a FLAT tree - a set of at most LION_POSTING_FLAT_LEAVES
 * leaves and no internal level, whose head is its first leaf (DESIGN.md §22)
 * - down WITHOUT moving it: its items go to a brand new leaf linked where the
 * head was in the chain, and the head block - the entry's `head` and the
 * owner stamp of every page of the set - becomes a level-1 page with a
 * downlink to that leaf and to every other leaf of the chain, in one record.
 * headbuf is the head, held EXCLUSIVE; held is another leaf of the set the
 * caller holds locked (it is read from the buffer, not locked again), or
 * InvalidBuffer.  The new leaf comes back pinned and still EXCLUSIVE-locked -
 * it has never been unlocked since it was allocated, so no reader can have
 * copied the items it now holds before the caller has finished with them
 * (DESIGN.md §11, §22).
 *
 * When the head was the only leaf the set's last leaf changes, and the
 * entry's `tail` is updated in the same record - only `tail`: the record logs
 * the entry as it is on entrybuf, because the counters in the caller's copy
 * describe items that a LATER record places, and that record may never be
 * written.  The caller's copy gets the new `tail` as well.  A position tree
 * has no entry and passes InvalidBuffer and NULL.
 *
 * nbtree and GIN both keep the root in place on a root split, and §22 needs
 * it for a second reason: `head` is the set's identity, so an entry never has
 * to be rewritten for a root split and the four-buffer budget of a
 * GenericXLog record is never the binding constraint.
 */
extern Buffer lion_posting_promote(Relation index, Relation heaprel,
								   Buffer headbuf, Buffer held,
								   Buffer entrybuf, OffsetNumber entryoff,
								   LionEntryTuple *entry);

/*
 * Is the head of the tree rooted at root a LEAF, i.e. is the tree flat?  For
 * a writer of the tree, which holds a leaf of it (held, not locked again when
 * it is the head) - and whose key's writers serialise, so the answer holds
 * for as long as it keeps writing.
 */
extern bool lion_posting_root_is_leaf(Relation index, BlockNumber root,
									  Buffer held);

/*
 * How many leaves the flat tree rooted at root has, counted along its right
 * links and no further than limit + 1.  held as above.
 */
extern int	lion_posting_flat_count(Relation index, BlockNumber root,
									Buffer held, int limit);

/*
 * Finish the split of the posting page pbuf, which the caller holds EXCLUSIVE
 * and keeps: put the downlink of its right sibling into the parent and clear
 * LION_PAGE_INCOMPLETE_SPLIT.  Idempotent, so a crash between the two records
 * costs nothing but the next writer's repair.
 */
extern void lion_posting_finish_split(Relation index, Relation heaprel,
									  uint32 hash, BlockNumber head,
									  Buffer pbuf);

/*
 * The same with the separator supplied.  A split knows it - it is the first
 * container key it put on the new sibling - and saying so spares a read of a
 * page the caller may still be holding EXCLUSIVE.  NULL is the repair case,
 * which has to work it out from the pages.
 */
extern void lion_posting_finish_split_sep(Relation index, Relation heaprel,
										  uint32 hash, BlockNumber head,
										  Buffer pbuf, const uint32 *knownsep);

/*
 * The item at off of a container page (block blkno of index; page may be a
 * private copy of it), checked for what the container and segment code needs
 * of an item it is handed: a normal line pointer inside the page, a known
 * item type, a size that fits the item, and no empty segment.  Any damage is
 * an ERROR (ERRCODE_INDEX_CORRUPTED).  Every reader of a container page takes
 * its items through this.
 */
extern LionContainer *lion_page_item_fetch(Relation index, Page page,
										   BlockNumber blkno, OffsetNumber off);

/*
 * What a WRITER checks of a page before the WAL record that changes it opens
 * (an rmgr-mode record is a critical section, DESIGN.md §25), each an ERROR
 * (ERRCODE_INDEX_CORRUPTED) on damage: every item of the page inside its item
 * space and no two overlapping (lion_page_check_items(), before items are
 * deleted, moved to another page or rewritten); the item at off alone in the
 * bytes the page allots it (lion_page_check_alone(), before it is overwritten
 * or deleted in place); the directory entry at off a whole one, exactly one
 * of INLINE and CHAIN, and of the size a CHAIN entry has to have
 * (lion_page_entry_fetch()); and the page header within the bounds
 * PageAddItemExtended() PANICs outside of (lion_page_check_header(), before
 * an item is added to a page nothing else has checked).
 */
extern void lion_page_check_header(Relation index, Page page, BlockNumber blkno);
extern void lion_page_check_items(Relation index, Page page, BlockNumber blkno);
extern void lion_page_check_alone(Relation index, Page page, BlockNumber blkno,
								  OffsetNumber off);
extern LionEntryTuple *lion_page_entry_fetch(Relation index, Page page,
											 BlockNumber blkno,
											 OffsetNumber off);

/*
 * Locate a container by ckey on a container page.  Returns the offset of the
 * item whose own ckey is exactly ckey, or the offset where it should be
 * inserted with *found=false.  Only for callers that know they are looking
 * for a regular container (a segment's own ckey is its first one).  Every
 * item the search looks at goes through lion_page_item_fetch(), so the one
 * found is fit to be handed on.
 */
extern OffsetNumber lion_page_find_container(Relation index, Page page,
											 BlockNumber blkno, uint32 ckey,
											 bool *found);

/*
 * Locate the item that covers ckey on a container page: the container with
 * that ckey, or the sparse segment whose range [first ckey, last ckey]
 * contains it.  With *found=false the return value is the offset at which an
 * item for ckey would have to be inserted to keep the items ordered by first
 * ckey (DESIGN.md §13; item ranges never overlap or interleave).  Items are
 * checked as lion_page_find_container() checks them.
 */
extern OffsetNumber lion_page_find_item(Relation index, Page page,
										BlockNumber blkno, uint32 ckey,
										bool *found);

/*
 * Insert-or-replace a container in the chain of an entry, splitting pages as
 * needed (DESIGN.md §4).  The entry's bucket buffer (entrybuf, held EXCLUSIVE)
 * is updated when tail changes.  ncontainers_delta receives +1 for a new
 * container, 0 for a replacement.  All changes WAL-logged.
 */
extern void lion_chain_put_container(Relation index, Relation heaprel,
									Buffer entrybuf, OffsetNumber entryoff,
									LionEntryTuple *entry, LionContainer *c,
									int *ncontainers_delta);

/* Page-level min/max maintenance after any change of a container page's items. */
extern void lion_page_update_minmax(Page page);
extern LionContainer *lion_page_poschunk_fetch(Relation index, Page page,
											   BlockNumber blkno, OffsetNumber off);

/* Convenience: read the meta page once and validate it (used by lion_get_state). */
extern void lion_read_meta(Relation index, LionMetaPageData *meta);
extern void lion_meta_check_version(Relation index,
									const LionMetaPageData *meta);
extern bool lion_index_other_format(Relation index);

/* ---------- AM entry points ---------- */

extern IndexBuildResult *lionbuild(Relation heap, Relation index, struct IndexInfo *indexInfo);
extern void lionbuildempty(Relation index);
extern bool lioninsert(Relation index, Datum *values, bool *isnull, ItemPointer ht_ctid,
					  Relation heapRel, IndexUniqueCheck checkUnique, bool indexUnchanged,
					  struct IndexInfo *indexInfo);
extern IndexBulkDeleteResult *lionbulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
											IndexBulkDeleteCallback callback, void *callback_state);
extern IndexBulkDeleteResult *lionvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats);
extern IndexScanDesc lionbeginscan(Relation r, int nkeys, int norderbys);
extern void lionrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys, ScanKey orderbys, int norderbys);
extern void lionendscan(IndexScanDesc scan);
extern int64 liongetbitmap(IndexScanDesc scan, TIDBitmap *tbm);
extern bool liongettuple(IndexScanDesc scan, ScanDirection dir);	/* DESIGN.md §29 */

/*
 * The windows of a plain scan (DESIGN.md §29.3): container keys per window of
 * one that reads a multi-key column whole, and the other columns' containers
 * per window of a long range beside them.  Each is sized by work_mem, and by
 * pg_lion.scan_window_floor at least - 4 MB by default: 1024 container keys,
 * 65536 heap blocks, for the first, 512 containers for the second.
 */
#define LION_SCAN_WINDOW_FLOOR	4096	/* kB */
extern PGDLLIMPORT int lion_scan_window_floor;
extern int	lion_union_window(void);
extern int	lion_walk_window(void);
extern int	lion_scan_list_batch(void);
extern bytea *lionoptions(Datum reloptions, bool validate);
extern bool lionvalidate(Oid opclassoid);
/* a parent queried without its children: no lion path of its own (lion_plan_hooks.c) */
extern bool lion_rel_is_unexpanded_parent(struct RangeTblEntry *rte);
extern void lioncostestimate(struct PlannerInfo *root, struct IndexPath *path, double loop_count,
							Cost *indexStartupCost, Cost *indexTotalCost, Selectivity *indexSelectivity,
							double *indexCorrelation, double *indexPages);
/* the entries a lion path's lookups hand their answer out by (lion_amcost.c) */
extern double lion_cost_path_entries(struct PlannerInfo *root,
									 struct IndexPath *path);
extern bool lion_enable_plain_scan;	/* GUC pg_lion.enable_plain_scan */

/*
 * Emit one item's members into a TIDBitmap (lion_scan.c), one tbm_add_tuples()
 * call per heap block; works for containers and sparse segments alike.
 * recheck is passed straight to tbm_add_tuples(): true when the scan is
 * emitting a superset of the matching TIDs and the heap scan has to apply the
 * original quals again.  Returns the number of members emitted.
 */
extern int64 lion_container_to_tbm(const LionContainer *c, TIDBitmap *tbm,
								  bool recheck);


/* ---------- additive helpers (wave 1, agent "am-core") ---------- */

/*
 * Largest item that can be placed on an otherwise empty lion index page.
 */
#define LION_MAX_ITEM_SIZE \
	MAXALIGN_DOWN(BLCKSZ - SizeOfPageHeaderData - LION_SPECIAL_SIZE - sizeof(ItemIdData))

/*
 * An INLINE entry payload holds containers back to back with no padding
 * (DESIGN.md section 4), so a container inside it is generally not aligned.
 * lion_inline_fetch() copies the container at byte offset *off into the
 * caller's aligned buffer (capacity LION_CONTAINER_MAX_SIZE), advances *off
 * past it, and returns its size; it returns 0 when the payload is exhausted.
 * Containers stored as page items are always MAXALIGNed by PageAddItem and
 * can be used in place.
 *
 * Damage in the payload is an ERROR naming the index and the entry - its
 * block and offset, which a caller that copied the payload out of the page
 * keeps for this - with ERRCODE_INDEX_CORRUPTED, as damage in a page item is
 * (lion_page_item_fetch()).  lion_inline_next() is the same walk returning
 * the damage instead, for lion_index_stats(), which skips what it cannot read
 * (DESIGN.md §7), and for a payload the caller built itself.
 */
extern Size lion_inline_next(const char *payload, Size paylen, Size *off,
							 LionContainer *buf, const char **damage);
pg_noreturn extern void lion_inline_damaged(Relation index, BlockNumber blk,
											OffsetNumber eoff,
											const char *damage,
											const LionContainer *hdr,
											Size off, Size avail);

static inline Size
lion_inline_fetch(Relation index, BlockNumber blk, OffsetNumber eoff,
				  const char *payload, Size paylen, Size *off,
				  LionContainer *buf)
{
	const char *damage;
	Size		csize = lion_inline_next(payload, paylen, off, buf, &damage);

	if (unlikely(damage != NULL))
		lion_inline_damaged(index, blk, eoff, damage, buf, *off,
							paylen - *off);
	return csize;
}

/*
 * The positions extension of e, or NULL for an entry without one.  Entries
 * sit MAXALIGNed on their pages, and so does the extension.
 */
static inline LionEntryPosExt *
lion_entry_posext(const LionEntryTuple *e)
{
	if ((e->flags & LION_ENTRY_POSITIONS) == 0)
		return NULL;
	return (LionEntryPosExt *) ((char *) e + LionEntryExtOffset(e));
}

/*
 * Bytes of an INLINE entry's positions section, given the page item size:
 * never more than what follows the payload offset, whatever pos_len claims
 * (lion_page_entry_fetch() and verify report a pos_len that overruns).
 */
static inline Size
lion_entry_pos_len(const LionEntryTuple *e, Size itemsz)
{
	const LionEntryPosExt *x = lion_entry_posext(e);
	Size		off = LionEntryPayloadOffset(e);

	if (x == NULL || itemsz <= off)
		return 0;
	return Min((Size) x->pos_len, itemsz - off);
}

/*
 * Length in bytes of an INLINE entry's payload - its container items and
 * their slack, not its positions - given the page item size.
 */
#define LION_ENTRY_PAYLOAD_LEN(e, itemsz) \
	((Size) (itemsz) - LionEntryPayloadOffset(e) - lion_entry_pos_len((e), (itemsz)))

/*
 * The short header's counts (lion_entryfmt.h) hold every INLINE posting set:
 * its payload is at most LION_MAX_ENTRY_SIZE bytes, of items of at least
 * LION_CONTAINER_HDRSZ bytes and at most LION_CONTAINER_RANGE members each.
 */
StaticAssertDecl(LION_MAX_ENTRY_SIZE / LION_CONTAINER_HDRSZ <= PG_UINT16_MAX,
				 "an INLINE payload's item count fits the short entry header");
StaticAssertDecl((uint64) (LION_MAX_ENTRY_SIZE / LION_CONTAINER_HDRSZ) *
				 LION_CONTAINER_RANGE <= PG_UINT32_MAX,
				 "an INLINE payload's TID count fits the short entry header");

/*
 * The setters of an entry's counts, which store into the entry's own header
 * form; a count its form cannot hold is damage the caller read from a page,
 * and an ERROR (ERRCODE_INDEX_CORRUPTED).
 */
extern void lion_entry_count_error(const LionEntryTuple *e, const char *what,
								   uint64 n);

static inline void
lion_entry_set_ntids(LionEntryTuple *e, uint64 n)
{
	if (unlikely(!lion_entry_try_set_ntids(e, n)))
		lion_entry_count_error(e, "TIDs", n);
}

static inline void
lion_entry_set_ncontainers(LionEntryTuple *e, uint64 n)
{
	if (unlikely(n > PG_UINT32_MAX ||
				 !lion_entry_try_set_ncontainers(e, (uint32) n)))
		lion_entry_count_error(e, "items", n);
}

/*
 * Fill *ix - every key column of it - using the supplied meta page image.
 * ambuild uses this before the meta page exists; lion_get_index_state() uses
 * it afterwards.  The column array and the FmgrInfos are allocated in cxt.
 */
extern void lion_fill_index_state(Relation index, LionIndexState *ix,
								 const LionMetaPageData *meta,
								 MemoryContext cxt);
/* The meta page's wal_mode, remembered per relfilenode (§25; lion_state.c). */
extern uint32 lion_index_meta_wal_mode(Relation index);

/* Record on a meta page image the order a build laid the directory out in (§21). */
extern void lion_meta_record_order(LionMetaPageData *meta, LionIndexState *ix);

/*
 * ... and which key columns got summary posting sets, with their bucket size
 * (DESIGN.md §32).
 */
extern void lion_meta_record_summaries(LionMetaPageData *meta, uint32 cols,
									   uint32 bucket_tids);

/*
 * ... and which are bit-sliced (DESIGN.md §51), which decides the version:
 * 15 when any is, 14 - byte for byte what it was - when none is.  After
 * lion_meta_record_summaries(), which sets 14.
 */
extern void lion_meta_record_slices(LionMetaPageData *meta, uint32 cols);

/*
 * The distinct keys of each key column (DESIGN.md §33).  Fill in a count of
 * ix's columns from the VALUE entries of each (nvalues[], one per key column),
 * the sum of the squares of their TIDs (sumsq[], one per key column) and the
 * rows under its first scalar column, taken now; put it on a meta page
 * image nothing else can see yet (ambuild); write it on the index's meta page
 * in a WAL record of its own (VACUUM and ANALYZE); read it back, false when the
 * meta page carries none.
 */
extern void lion_meta_fill_ndistinct(LionMetaNdistinct *nd,
									 const LionIndexState *ix,
									 const uint64 *nvalues,
									 const uint64 *sumsq, uint64 rows);
extern void lion_meta_record_ndistinct(Page metapage,
									   const LionMetaNdistinct *nd);
extern void lion_meta_write_ndistinct(Relation index,
									  const LionMetaNdistinct *nd);
extern bool lion_read_meta_ndistinct(Relation index, LionMetaNdistinct *nd);

/*
 * The end of an ambulkdelete call (DESIGN.md §37): add one to the meta page's
 * bulkdeletes, in the record that writes nd beside it - or the counts the page
 * has, when nd is NULL.  lion_meta_write_ndistinct() keeps the number as it
 * is, and lion_read_meta_ndistinct() reads it back.
 */
extern void lion_meta_count_bulkdelete(Relation index,
									   const LionMetaNdistinct *nd);

/*
 * Like lion_find_entry(), but with the comparison functions the caller wants:
 * used by scans and by the count pushdown, where the search value may be of a
 * type that is only cross-type comparable with the indexed one.  Both procs
 * take the STORED key on the left; NULL means the index's own.  A cross-type
 * search whose opfamily has no cross-type ordering function (support proc 4)
 * cannot descend at all and falls back to a walk of every leaf, which is
 * correct and slow; the default opclasses of pg_lion all carry one.
 * *movedright, when given, is lion_dir_find()'s.
 */
extern bool lion_find_entry_ext(Relation index, LionState *state, int lockmode,
							   Datum key, uint32 hash,
							   FmgrInfo *eqproc, FmgrInfo *cmpproc,
							   Oid collation,
							   Buffer *buf, OffsetNumber *offnum,
							   bool *movedright);

/*
 * May this transaction use index for a query under snapshot?  Requires
 * indisvalid and indisready, and honours indcheckxmin exactly as the planner
 * does in get_relation_info().  Callers that open an index by name (the SQL
 * count functions, the verifier) must ask, because nothing else checked for
 * them; *why receives the reason when the answer is false.
 */
extern bool lion_index_usable(Relation index, Snapshot snapshot,
							 const char **why);

/*
 * ERROR unless index may be read under snapshot: never under an MVCC one
 * while PostgreSQL 16's old_snapshot_threshold is set (DESIGN.md §9).
 */
extern void lion_check_old_snapshot(Relation index, Snapshot snapshot);

/*
 * Direct readers of a lion index (lion_reader.c): what a function handed an
 * index by name, or a node reading one without index_beginscan(), owes the
 * caller.  LION_READ_ROWS for anything that returns or counts rows, which
 * vets the index for the snapshot and refuses row-level security;
 * LION_READ_STATS for index-wide statistics only, which refuses row-level
 * security too.  Predicate locks are not the policy's: every lookup and walk
 * takes them on what it reads (below).
 */
typedef enum LionReadPolicy
{
	LION_READ_ROWS,
	LION_READ_STATS,
} LionReadPolicy;

extern Relation lion_reader_open(Oid indexoid, LionReadPolicy policy,
								 Snapshot snapshot, const char *action,
								 Relation *heap);
extern bool lion_is_partitioned_index(Oid relid);
extern bool lion_is_lion_index(Oid relid);
extern void lion_refuse_partitioned_index(Oid relid);
extern void lion_reader_vet(Relation heap, Relation index, Snapshot snapshot,
							const char *action);

/*
 * SERIALIZABLE predicate locks (lion_reader.c, DESIGN.md §9): what a reader
 * takes on the pages it reads, and what an insert checks before it changes
 * an entry.  All of them do nothing outside a SERIALIZABLE transaction.
 */
extern void lion_reader_lock_page(Relation index, BlockNumber blkno);
extern void lion_reader_lock_entry(Relation index, BlockNumber leaf,
								   const LionEntryTuple *entry);
extern void lion_reader_lock_absent(Relation index, Buffer buf,
									bool movedright);
extern void lion_reader_lock_leaf(Relation index, Buffer buf,
								  AttrNumber attno);
extern void lion_writer_check(Relation index, Buffer leafbuf,
							  OffsetNumber off, bool found);
extern void lion_predlock_copy(Relation index, BlockNumber from,
							   BlockNumber to);
extern void lion_predlock_move(Relation index, BlockNumber from,
							   BlockNumber to);


/* ---------- additive helpers (wave 2, insert/vacuum/verify) ---------- */

/*
 * Bring a page that lion_alloc_page() took into the caller's open record: it
 * is registered as a page this record INITIALISES (a generic record logs a
 * full image, an rmgr record a PAGE_INIT operation and no image at all) and
 * initialised with `flags`.  Registering the new page in the record that
 * links it into a chain is what keeps a crash from leaving an initialised
 * page nothing points at.
 */
extern Page lion_wal_init_buffer(LionWalState *state, Buffer buf, uint16 flags);

/*
 * Log the page's special area as it now stands.  A caller that changes
 * rightlink, level, owner stamps, min/max or the page flags calls this once,
 * when they are final; in generic mode it does nothing, because the diff has
 * them already.
 */
extern void lion_wal_log_special(LionWalState *state, Page page);

/*
 * Private copy of an entry tuple (header, key and positions extension taken
 * from entry, which may point into a page, in its own header form) with a
 * new payload.  The caller updates the counts; lion_entry_chain_shape() is
 * what changes the form.
 */
extern LionEntryTuple *lion_entry_rebuild(const LionEntryTuple *entry,
										const char *payload, Size payloadlen,
										Size *size);

/*
 * The same with the result ALLOCATED at allocsz bytes and the tail past the
 * payload zeroed: an INLINE entry's growth slack (DESIGN.md §4).  The zeroes
 * terminate the payload - lion_inline_fetch() stops at the first zero item
 * header - so nothing else has to know the slack is there.
 */
extern LionEntryTuple *lion_entry_rebuild_slack(const LionEntryTuple *entry,
												const char *payload,
												Size payloadlen, Size allocsz,
												Size *size);
extern LionEntryTuple *lion_entry_rebuild_pos(const LionEntryTuple *entry,
											  const char *payload,
											  Size payloadlen, Size payarea,
											  const char *positions,
											  Size poslen, Size *size);
extern LionEntryTuple *lion_entry_add_posext(const LionEntryTuple *shape,
											 Size *size);

/*
 * How many bytes to give an INLINE entry an INSERT is rewriting: its payload
 * plus growth slack by the rule of DESIGN.md §4, capped by inline_limit (so
 * that slack can never make a key spill) and by maxsize (so that it can never
 * be worth splitting a leaf for).
 */
extern Size lion_entry_alloc_size(Size payoff, Size paylen, Size inline_limit,
								  Size maxsize);

/*
 * The largest INLINE payload an entry whose payload starts at `payoff` may
 * carry: inline_limit, unless the key leaves less than that of
 * LION_MAX_ENTRY_SIZE.  A posting set past it spills onto container pages.
 * ambuild and INSERT both decide with this, so that every set an INSERT
 * could produce, a rebuild can write again.
 */
static inline Size
lion_inline_max(Size payoff, Size inline_limit)
{
	Assert(payoff < (Size) LION_MAX_ENTRY_SIZE);
	return Min(inline_limit, (Size) LION_MAX_ENTRY_SIZE - payoff);
}

/*
 * Like lion_chain_put_container(), but for a container page the caller has
 * already located and locked EXCLUSIVE (a cleanup lock counts); the buffer
 * stays locked.  The page must be the one that owns c->ckey.
 */
extern void lion_chain_put_container_locked(Relation index, Relation heaprel,
										   Buffer buf,
										   Buffer entrybuf, OffsetNumber entryoff,
										   LionEntryTuple *entry, LionContainer *c,
										   int *ncontainers_delta);

/*
 * The general form of the above: replace the item at off (replace = true) or
 * insert at off (replace = false) with nitems items, which must be in
 * ascending, non-overlapping ckey order and belong exactly where off points.
 * This is what lets an insert turn one sparse segment into (left segment,
 * container, right segment) atomically, in one WAL record, without the page
 * ever holding a ckey twice (DESIGN.md §13).
 *
 * buf is held EXCLUSIVE (a cleanup lock counts) and stays locked; entry is
 * the caller's private copy with its counters already updated, and travels in
 * the same record.  The page is split as in DESIGN.md §4 when the items do
 * not fit; the caller's items must together fit on an empty page.
 */
#define LION_MAX_PUT_ITEMS	3

extern void lion_chain_put_items_locked(Relation index, Relation heaprel,
									   Buffer buf,
									   Buffer entrybuf, OffsetNumber entryoff,
									   LionEntryTuple *entry,
									   OffsetNumber off, bool replace,
									   LionContainer **items, int nitems);

/*
 * Move an INLINE payload onto a chain of freshly allocated container pages
 * and rewrite the entry (a private copy in CHAIN shape, ncontainers/ntids
 * already set) as a CHAIN entry.  The entry page is held EXCLUSIVE by the
 * caller.
 */
extern void lion_entry_spill(Relation index, Relation heaprel, Buffer entrybuf,
							OffsetNumber entryoff, LionEntryTuple *entry,
							const char *payload, Size paylen);
extern void lion_entry_spill_pos(Relation index, Relation heaprel,
								 Buffer entrybuf, OffsetNumber entryoff,
								 LionEntryTuple *entry, const char *payload,
								 Size paylen, const LionContainer *poschunk);

/*
 * Does `head` name the live root of a posting set whose key hashes to `hash`?
 * What tells a leaf that an interrupted multi-leaf spill left behind - its
 * root was never written - from a leaf of a set that exists (DESIGN.md §18).
 * wait = false never blocks and answers true when the root is busy.  The root
 * is read through strategy: VACUUM's sweep passes its own (info->strategy),
 * lion_index_verify() NULL.
 */
extern bool lion_posting_root_live(Relation index, uint32 hash,
								   BlockNumber head, bool wait,
								   BufferAccessStrategy strategy);


/* ---------- additive helpers (wave 3, write path) ---------- */

/*
 * GROWTH SLACK INSIDE AN ITEM (DESIGN.md §4).
 *
 * An item on a container page may be allocated LARGER than the bytes its
 * header says it needs.  The spare bytes at its end are slack that the insert
 * path grows into: adding a member to an ARRAY or RUN container, or a pair to
 * a sparse segment, then happens by memmove INSIDE the item - under the same
 * lock, in the same WAL record as the entry tuple, without changing the
 * item's offset or its ItemIdGetLength() - so no other item on the page
 * moves and the GenericXLog delta is a handful of bytes instead of a page.
 *
 * The two sizes of an item are therefore different things, and both are
 * needed:
 *
 *	 lion_item_size(item)		its LOGICAL size, derived from the header;
 *								what every reader uses, and what the item
 *								would be written back as.
 *	 ItemIdGetLength(iid)		its ALLOCATED length on the page, which is
 *								what the item may grow to in place.
 *
 * Readers never look at the allocated length (they walk items by their
 * headers), so slack is invisible to them; code that copies an item out of a
 * page by ItemIdGetLength() - VACUUM does - still works because the allocated
 * length is capped at LION_CONTAINER_MAX_SIZE, the size of every item work
 * buffer.  Slack bytes are always zeroed when an item is written, so that
 * the page image stays deterministic and the WAL delta stays small.
 *
 * A bulk-built index gets no slack at all: it is read-mostly, and the space
 * would be pure loss.  Slack appears when an insert first grows an item.
 */
#define LION_ITEM_SLACK_MIN		8	/* smallest slack worth having */
#define LION_ITEM_SLACK_MAX		64	/* and the most, per item */
#define LION_ITEM_SLACK_FRACTION 8	/* size/8, clamped into the above */

/* The most bytes of slack an INSERT may leave (alignment padding included). */
#define LION_ITEM_SLACK_LIMIT	(LION_ITEM_SLACK_MAX + MAXIMUM_ALIGNOF - 1)

/*
 * SHRINKING AN ITEM IN PLACE (DESIGN.md §18).
 *
 * VACUUM removes members from an item and writes it back.  Writing it back at
 * its new, smaller size would make PageIndexTupleOverwrite() move every item
 * after it on the page, and a GenericXLog delta is a byte-wise diff of the
 * page image: the record would then carry the whole tail of the page instead
 * of the handful of bytes the removal really changed.  So a filtered item
 * KEEPS the number of bytes the page has allotted it and the freed bytes
 * become slack, exactly like the growth slack an insert leaves behind - which
 * the next insert into that item then grows into for free.
 *
 * The bound is larger than an insert's because a filter can free more than a
 * growth adds (a 1% deletion takes ~40 bytes out of a full ARRAY container),
 * and it is finite because unbounded slack would be indistinguishable from
 * corruption and would waste the page: an item that has lost more than this
 * is compacted, paying the page-tail delta once.
 */
#define LION_ITEM_SLACK_VACUUM	256

/* The most slack any item may hold, and what verify() accepts. */
#define LION_ITEM_SLACK_BOUND \
	((LION_ITEM_SLACK_LIMIT > LION_ITEM_SLACK_VACUUM) ? \
	 LION_ITEM_SLACK_LIMIT : LION_ITEM_SLACK_VACUUM)

/*
 * The same for the INLINE payload of an entry tuple on a bucket page: a
 * filtered payload is written back into the bytes the entry already has, with
 * the remainder zeroed.  lion_inline_fetch() stops at the first zero item
 * header, which no real item can have (type 0 is not a container kind), so
 * the trailing zeroes are self-describing and no length field is needed.
 */
#define LION_ENTRY_SLACK_BOUND	256

/*
 * Allocated length to write an item of logical size `size` with, so that it
 * has room to grow in place.  Always >= size, always MAXALIGNed and never
 * above LION_CONTAINER_MAX_SIZE; a BITSET container, which is already the
 * largest an item can be, gets its exact size.  The caller checks that the
 * result fits on the page and falls back to `size` if it does not.
 */
extern Size lion_item_alloc_size(const LionContainer *item, Size size);

/* Free bytes inside an item of allocated length itemlen. */
static inline Size
lion_item_slack(const LionContainer *item, Size itemlen)
{
	Size		size = lion_item_size(item);

	return (itemlen > size) ? itemlen - size : 0;
}

/*
 * lion_chain_put_items_locked() and lion_chain_put_container_locked() with
 * control over slack: with slack = true the items are written with room to
 * grow (and the items[] buffers must have LION_CONTAINER_MAX_SIZE bytes of
 * capacity, because their slack is zeroed in place before the copy).  The
 * two original functions are these with slack = false, which is what VACUUM
 * and the build want: they write items at their exact size.
 */
extern void lion_chain_put_items_locked_ext(Relation index, Relation heaprel,
										   Buffer buf,
										   Buffer entrybuf, OffsetNumber entryoff,
										   LionEntryTuple *entry,
										   OffsetNumber off, bool replace,
										   LionContainer **items, int nitems,
										   bool slack);

extern void lion_chain_put_container_locked_ext(Relation index, Relation heaprel,
											   Buffer buf,
											   Buffer entrybuf, OffsetNumber entryoff,
											   LionEntryTuple *entry, LionContainer *c,
											   int *ncontainers_delta, bool slack);

/*
 * The tree a leaf write goes to: a key's posting tree, whose entry travels in
 * every record that changes it, or one of its position trees (DESIGN.md §17),
 * which has no entry to keep in step.  root is the owner stamp of every page
 * of the tree, kind the page kind its pages carry (0 or LION_PAGE_POSITIONS).
 * entry is NULL exactly when kind is LION_PAGE_POSITIONS.
 */
typedef struct LionTreeRef
{
	uint32		hash;
	BlockNumber root;
	uint16		kind;
	Buffer		entrybuf;
	OffsetNumber entryoff;
	LionEntryTuple *entry;
} LionTreeRef;

/*
 * lion_chain_put_items_locked_ext() for either kind of tree.  For a position
 * tree the items are position chunks, ordered by header ckey (which, unlike
 * the posting tree's, may repeat), and buf is a leaf of that tree.
 */
extern void lion_tree_put_items_locked(Relation index, Relation heaprel,
									   Buffer buf, const LionTreeRef *tree,
									   OffsetNumber off, bool replace,
									   LionContainer **items, int nitems,
									   bool slack);

/*
 * Position trees (lion_postree.c): a key's position chunks once they outgrow
 * its entry.  put replaces a member of the same code and says so; the caller
 * holds the key's directory leaf EXCLUSIVE.  fetch is a reader's lookup.
 */
extern BlockNumber lion_postree_create(Relation index, Relation heaprel,
									   uint32 hash);
extern bool lion_postree_put(Relation index, Relation heaprel, uint32 hash,
							 BlockNumber root, const LionPosMember *m);
extern bool lion_postree_fetch(Relation index, uint32 hash, BlockNumber root,
							   uint64 code, LionPosMember *m);

/*
 * The position filter (lion_posfilter.c): a tsquery decided exactly, member
 * by member, from the positions a column stores.  begin returns NULL when the
 * query has an operand it cannot follow (a prefix lexeme).
 */
typedef struct LionPosFilter LionPosFilter;

/* A forward reader over one key's members (lion_posfilter.c). */
typedef struct LionPosCursor LionPosCursor;
extern LionPosCursor *lion_poscursor_open(Relation index, LionState *col,
										  Datum key, bool countsonly);
extern const LionPosMember *lion_poscursor_next(Relation index,
												LionPosCursor *cur,
												uint64 code);
extern void lion_poscursor_close(LionPosCursor *cur);

extern LionPosFilter *lion_posfilter_begin(Relation index, LionState *col,
										   Datum query, StrategyNumber strategy);
extern Size lion_posfilter_mem(Datum query);
extern int	lion_posfilter_container(LionPosFilter *pf, const LionContainer *c,
									 uint16 *keep);
extern const LionContainer *lion_posfilter_apply(LionPosFilter *pf,
												 const LionContainer *c);
extern void lion_posfilter_counts(const LionPosFilter *pf, int64 *nchecked,
								  int64 *nremoved);
extern void lion_posfilter_work(const LionPosFilter *pf, int64 *nseeks,
							   int64 *ndecoded);
extern void lion_posfilter_end(LionPosFilter *pf);
extern LionKeyNode *lion_posfilter_keynode(Relation index, LionState *col,
										   Datum query, StrategyNumber strategy,
										   LionKeyNode *child);
extern uint32 lion_settle_slices(LionIndexState *ix, const LionOptions *opts);
extern void lion_settle_store_positions(Relation index, LionIndexState *ix,
										const LionOptions *opts);
extern bool lion_query_posexact(Oid opfamily, Oid lefttype, Oid opno,
								Datum query, Oid collation);

/*
 * The number of DIRECTORY PAGES the current backend has read: the counter
 * behind EXPLAIN ANALYZE's "Directory Pages Read" (DESIGN.md §21).  It is a
 * plain process-local counter because every read of a leaf or an internal
 * page goes through one of a handful of places, and the count is only ever
 * used as a difference over a node's execution, or as a clock.
 */
extern PGDLLIMPORT int64 lion_dir_pages_read;

/*
 * Every one of those reads goes through lion_dir_count_read(), which also
 * marks the index read; lion_dir_read_since() says whether an index was read
 * since the counter stood at a value - the index-usage statistics of a count
 * (pg_stat_user_indexes), which report a scan of the indexes it read only.
 */
extern void lion_dir_count_read(Relation index);
extern bool lion_dir_read_since(Oid indexoid, int64 since);

/*
 * The same for the pages of POSTING TREES (DESIGN.md §22) the current backend
 * has read: every page a descent of one reads and every leaf a cursor steps
 * or seeks to.  The FK-side join reports the difference over its counts as
 * "Join Key Posting Pages Read" (DESIGN.md §27, "Where a key's time goes").
 */
extern PGDLLIMPORT int64 lion_posting_pages_read;


/*
 * Functions of the page layer that one of its files (lion_pages.c,
 * lion_meta.c, lion_state.c, lion_entry.c, lion_posting_put.c) uses in
 * another.
 */

/* lion_pages.c */
extern void lion_put_entry(Relation index, LionWalState *xstate,
						   Buffer entrybuf, OffsetNumber entryoff,
						   LionEntryTuple *entry);
extern void lion_item_zero_slack(LionContainer *item, Size size, Size alloc);

/* lion_state.c */
extern uint32 lion_order_ident(LionIndexState *ix);

/*
 * Begin a full iteration of a simplehash table of prefix pfx, as
 * pfx_start_iterate() does: at the table's first unused slot, which there
 * always is, since simplehash grows before a table fills.  The same walk in
 * the same order; what differs is that the slot is found here, where it
 * starts at 0 rather than at simplehash's PG_UINT64_MAX, which a static
 * analyser takes for a value the 32-bit iterator cannot hold.
 */
#define LION_SH_START_ITERATE(pfx, tb, it) \
	do { \
		uint32		lion_sh_at_ = 0; \
		uint32		lion_sh_i_; \
		\
		for (lion_sh_i_ = 0; lion_sh_i_ < (tb)->size; lion_sh_i_++) \
		{ \
			if ((tb)->data[lion_sh_i_].status != pfx##_SH_IN_USE) \
			{ \
				lion_sh_at_ = lion_sh_i_; \
				break; \
			} \
		} \
		pfx##_start_iterate_at((tb), (it), lion_sh_at_); \
	} while (0)

#endif							/* LION_H */
