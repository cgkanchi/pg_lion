/*-------------------------------------------------------------------------
 *
 * lion_vacuum.c
 *		ambulkdelete / amvacuumcleanup for the lion index
 *		(DESIGN.md sections 5 and 11).
 *
 * Three rules shape everything in this file.
 *
 * 1. Cleanup lock on every page that is visited, in chain order.
 *
 *	  ambulkdelete takes LockBufferForCleanup() on every directory LEAF and on
 *	  every container page of every chain it walks, whether or not that page
 *	  has anything to remove, and it takes them in chain order (the leaves
 *	  left to right from the leftmost one, then each container chain left to
 *	  right).  Internal directory pages are NOT cleanup-locked, and need not
 *	  be: they hold no TIDs, so no reader can be holding a container copy that
 *	  came from one (DESIGN.md §11, §21).  This is the rule
 *	  nbtree's btvacuumscan follows, and it is what makes the interlock of
 *	  DESIGN.md section 9 airtight.  A reader pins page P, copies container C
 *	  out and drops the content lock; a concurrent insert may then split P and
 *	  move C onto a brand new page N linked immediately right of P.  Because
 *	  VACUUM cannot reach N without first holding a cleanup lock on P, and
 *	  that lock waits for the reader's pin, it cannot clean C on N while the
 *	  reader is still consulting the visibility map with its stale copy.  (If
 *	  VACUUM had already passed P when the reader took C, then C had already
 *	  been cleaned of this cycle's dead TIDs.)  Two properties make this
 *	  sufficient: pages are never recycled, so a new page is always right of
 *	  its split origin and is visited after it, and the set of dead TIDs is
 *	  fixed before index cleanup starts.  The same reason is why the
 *	  re-placement of a container that grew (lion_vacuum_regrow) walks right
 *	  from the page the container was filtered on, cleanup-locking each page
 *	  on the way, instead of jumping straight to the page that owns its ckey.
 *
 * 2. Never wait for a cleanup lock while holding another LWLock.
 *
 *	  A buffer content lock is an LWLock and holding one implies
 *	  HOLD_INTERRUPTS(), so such a wait would be uncancellable: a cursor that
 *	  keeps one container page pinned could freeze VACUUM -- deaf to
 *	  statement_timeout and pg_cancel_backend -- until the session owning the
 *	  cursor went away.  The rule also removes a deadlock, because buffer
 *	  LWLocks have no deadlock detector: a reader that pins a bucket page (an
 *	  INLINE entry) and then locks a container page closes a cycle with a
 *	  VACUUM that holds that container page and waits for the bucket head.
 *
 * 3. A TID leaves a page only under a cleanup lock on that page, and the
 *	  entry tuple's counters travel in the same WAL record as the containers
 *	  they count, so a crash can never desynchronise them.
 *
 * Hence each bucket is processed in two passes.
 *
 *	Pass 1 walks the directory leaves.  Each leaf is cleanup-locked with
 *	nothing else held; its INLINE entries are filtered in place (a filtered
 *	payload can *grow* -- a run that loses every other member becomes a
 *	bitset -- so it may spill onto container pages exactly as an insert
 *	would); the offset, chain head and KEY of every CHAIN entry are noted;
 *	then the content lock is dropped while the pin is kept, so that pass 2 can
 *	re-lock the page cheaply.
 *
 *	The key is noted because an offset is no longer a stable name for an entry
 *	(DESIGN.md §21): a sorted directory inserts in the middle of a leaf, which
 *	shifts everything after it, and splits move the upper half away.  So every
 *	time pass 2 re-locks the leaf it checks that the entry at the offset it
 *	remembers is still the same one, by kind and stored key - one entry per
 *	key, so equal bytes mean the same entry - and descends again for it when
 *	it is not.  That re-descent happens with NOTHING else held, because taking
 *	a directory lock while holding a container page would close exactly the
 *	cycle rule 2 exists to avoid.
 *
 *	Pass 2 walks each CHAIN entry's container chain, holding nothing between
 *	steps.  For every page X of the chain, including pages with nothing to
 *	remove:
 *	  1. LockBufferForCleanup(X) with nothing held, so the wait is
 *		 interruptible, and filter the page without changing it.
 *	  2. Nothing to remove: release X and move on; the entry page is not
 *		 needed and inserts are not disturbed.
 *	  3. Otherwise ConditionalLockBuffer(entry page) and, if that succeeds,
 *		 write the change and the entry in one record and release both.
 *	  4. If the entry page is busy, release X, take the entry page EXCLUSIVE
 *		 (blocking is fine with nothing else held), then
 *		 ConditionalLockBufferForCleanup(X); on success filter X again -- it
 *		 was unlocked in between -- and write; on failure release the entry
 *		 page and start over at 1.
 *
 * Inserts therefore interleave with pass 2, and that shapes the rest:
 *
 *	- An insert modifies an entry and its containers only while holding that
 *	  entry's bucket page EXCLUSIVE.  So everything VACUUM reads out of a
 *	  container page and everything it derives from that read must happen
 *	  inside one window in which both that page and the entry page are held:
 *	  containers are filtered and written back without releasing either lock
 *	  in between.
 *	- The entry is re-read from its page every time the entry page is locked,
 *	  and its counters are updated by applying deltas, never by writing back
 *	  absolute values computed in an earlier window.
 *	- A container that grew out of its slot has to be re-placed through the
 *	  chain machinery, which may split its page.  That happens in a window of
 *	  its own (lion_vacuum_regrow), which finds the page the container is on by
 *	  walking right under cleanup locks (rule 1) -- a concurrent insert, or the
 *	  re-placement of an earlier container, may have split the page and moved
 *	  this container right -- and re-reads and re-filters the container it
 *	  finds there.  Filtering is idempotent, so re-filtering costs nothing but
 *	  a callback call; reusing a copy taken in an earlier window would be
 *	  wrong, because an insert may have added a TID to it in the meantime.
 *	  Until that write happens the container still holds its dead TIDs, which
 *	  is exactly what rule 3 allows for: they leave the page under its cleanup
 *	  lock.
 *	- A split moves items only to brand new pages immediately to the right of
 *	  the page they came from, and only items this pass has already filtered,
 *	  so continuing the walk at the rightlink read at the start of the window
 *	  still visits everything that needs work, at most re-filtering containers
 *	  that have already been filtered.
 *	- Entries appended to a bucket while it is being vacuumed may be missed.
 *	  That is safe: they can only hold TIDs inserted after the heap scan that
 *	  produced the dead TID list, and a dead TID's line pointer cannot be
 *	  reused before every index has stopped pointing at it.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/generic_xlog.h"
#include "access/transam.h"
#include "commands/vacuum.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "portability/instr_time.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "storage/indexfsm.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "lion.h"

/*
 * Where ambulkdelete's time goes (DESIGN.md §18, "Measure first").
 *
 * The counters are accumulated unconditionally: a handful of clock reads per
 * PAGE (never per item) is far below what visiting the page costs, and having
 * the breakdown available in any build is what makes a regression in VACUUM
 * cost attributable.  They are reported at DEBUG1 at the end of ambulkdelete,
 * which is what bench/vacuum_micro.sh captures.
 */
typedef struct LionVacProfile
{
	instr_time	total;
	instr_time	cleanup_wait;	/* blocked waiting for a cleanup lock */
	instr_time	filter;			/* filtering items, i.e. the dead-TID callback */
	instr_time	apply;			/* GenericXLog: image copy, delta, WAL insert */
	instr_time	inlinework;		/* INLINE entries: filter and rewrite */
	int64		pages_visited;	/* container pages cleanup-locked */
	int64		pages_changed;	/* ... of which something was written to */
	int64		records;		/* GenericXLog records written */
	int64		items_rewritten;
	int64		items_deleted;
	int64		regrows;
	int64		entries_rewritten;
	int64		entries_deleted;
} LionVacProfile;

static inline void
lion_vac_tick(instr_time *acc, instr_time start)
{
	instr_time	now;

	INSTR_TIME_SET_CURRENT(now);
	INSTR_TIME_ACCUM_DIFF(*acc, now, start);
}

typedef struct LionVacState
{
	Relation	index;
	Relation	heaprel;		/* what page reuse needs, may be NULL */
	LionIndexState *ix;
	IndexBulkDeleteCallback callback;
	void	   *callback_state;
	IndexBulkDeleteResult *stats;
	double		numtids;		/* sum of ntids over every entry */
	LionContainer *cbuf;			/* aligned container work buffer */
	MemoryContext pagecxt;		/* reset per container page */
	MemoryContext bucketcxt;	/* reset per bucket page: pass 1 keeps its
								 * findings across pass 2, which resets
								 * pagecxt under it */

	/*
	 * The blocks this ambulkdelete accounted for: the meta page, every bucket
	 * page it walked and every container page it reached from a live entry.
	 * A page it FREED is cleared again.  What is left unset when the walk is
	 * over is either a block a concurrent insert created (which this VACUUM
	 * must not touch) or a leak - a page an interrupted allocation or a crash
	 * between the two steps of a whole-chain free left unreferenced - and the
	 * sweep at the end of ambulkdelete is what recovers those (DESIGN.md §18).
	 *
	 * Blocks at or above nblocks did not exist when this VACUUM started and
	 * are never looked at.
	 */
	uint8	   *visited;
	BlockNumber nblocks;

	int64		pages_newly_deleted;	/* pages this VACUUM freed */
	int64		pages_deleted;	/* DELETED pages the index holds now */
	LionVacProfile prof;
} LionVacState;

static inline void
lion_vac_visit(LionVacState *vs, BlockNumber blk)
{
	if (blk < vs->nblocks)
		vs->visited[blk / 8] |= (uint8) (1 << (blk % 8));
}

static inline void
lion_vac_unvisit(LionVacState *vs, BlockNumber blk)
{
	if (blk < vs->nblocks)
		vs->visited[blk / 8] &= (uint8) ~(1 << (blk % 8));
}

static inline bool
lion_vac_visited(LionVacState *vs, BlockNumber blk)
{
	return blk < vs->nblocks &&
		(vs->visited[blk / 8] & (uint8) (1 << (blk % 8))) != 0;
}

/*
 * One entry as pass 1 found it on a directory leaf.
 *
 * off is a HINT: it is where the entry was when pass 1 looked, and every
 * later use of it is checked against (kind, key) first, because an insert
 * into the middle of the leaf or a split of it moves entries (DESIGN.md §21).
 * The key is a private copy in vs->bucketcxt, which lives as long as this
 * leaf is being worked on.
 */
typedef struct LionVacEntry
{
	OffsetNumber off;
	bool		ischain;
	bool		maydelete;		/* its posting set was emptied by pass 1/2 */
	uint32		hash;
	BlockNumber head;
	uint16		attno;			/* its key column (DESIGN.md §24) */
	int			kind;			/* LION_KIND_* */
	uint16		keylen;
	char	   *keydata;
} LionVacEntry;

/*
 * Where an entry is right now: a pinned (not locked) directory leaf and the
 * offset it was last seen at.  Pass 2 carries one of these per entry and
 * re-validates it every time it locks the leaf.
 */
typedef struct LionVacEntryRef
{
	Buffer		buf;
	OffsetNumber off;
} LionVacEntryRef;

/* Argument of the lion_container_remove_if() predicate. */
typedef struct LionVacPred
{
	uint32		ckey;
	IndexBulkDeleteCallback callback;
	void	   *callback_state;
} LionVacPred;

/* One item (container or sparse segment) to be written back to its page. */
typedef struct LionVacItem
{
	OffsetNumber off;			/* where it sits on the page */
	Size		size;			/* logical size of the filtered item */
	Size		writesz;		/* bytes to write it as: its old allotment
								 * when it shrank by little, so that nothing
								 * else on the page moves (DESIGN.md §18) */
	uint32		removed;		/* TIDs it lost */
	LionContainer *c;			/* the filtered item, padded to writesz */
} LionVacItem;

/* What one filtering pass over a container page found. */
typedef struct LionVacWork
{
	LionVacItem *work;			/* containers to write back */
	OffsetNumber *delofs;		/* containers that lost every member */
	int			nwork;
	int			ndel;
	uint64		delremoved;		/* TIDs the deleted containers held */
} LionVacWork;

/*
 * One INLINE entry that pass 1 has to write back, held until the whole bucket
 * page can be written in a single WAL record.
 *
 * tuple is the new entry tuple, already padded to writesz: whenever the
 * filtered payload fits inside the bytes the entry already occupies, the
 * entry KEEPS them and the remainder is zeroed, so PageIndexTupleOverwrite()
 * moves no other entry on the page and the GenericXLog delta is this entry
 * alone (DESIGN.md §18).  lion_inline_fetch() stops at the first zero item
 * header, which is what makes the padding invisible to every reader.
 */
typedef struct LionVacInline
{
	OffsetNumber off;
	Size		writesz;
	LionEntryTuple *tuple;
	char	   *payload;		/* the filtered payload, for a spill */
	Size		paylen;
	bool		spill;			/* it no longer fits: move it to a chain */
} LionVacInline;

static void lion_vacuum_leaf_page(LionVacState *vs, BlockNumber blk,
								 BlockNumber *nextp);
static void lion_vacuum_delete_entries(LionVacState *vs, Buffer buf,
									  LionVacEntry *ents, int nents);
static void lion_vacuum_chain(LionVacState *vs, Buffer entrybuf,
							 LionVacEntry *ent);
static BlockNumber lion_vacuum_container_page(LionVacState *vs,
											 LionVacEntryRef *ref,
											 const LionVacEntry *ent,
											 BlockNumber blk);
static BlockNumber lion_vacuum_filter_page(LionVacState *vs, Buffer buf,
										  LionVacWork *w,
										  const LionVacEntry *ent);
static void lion_vacuum_apply_page(LionVacState *vs, LionVacEntryRef *ref,
								  Buffer buf, LionVacWork *w, uint32 **grownp,
								  int *ngrownp);
static void lion_vacuum_regrow(LionVacState *vs, LionVacEntryRef *ref,
							  const LionVacEntry *ent, BlockNumber startblk,
							  uint32 ckey);
static LionEntryTuple *lion_vacuum_entry_copy(Buffer entrybuf,
											OffsetNumber entryoff, Size *size);
static void lion_vacuum_free_chain(LionVacState *vs, uint32 hash,
								  BlockNumber head);
static void lion_vacuum_sweep(LionVacState *vs);

/*
 * The predicate handed to lion_container_remove_if(): map the container
 * coordinates back to a heap TID and ask the caller of ambulkdelete.
 */
static bool
lion_vac_is_dead(uint16 lo, void *arg)
{
	LionVacPred *pred = (LionVacPred *) arg;
	ItemPointerData tid;

	lion_code_to_tid(lion_make_code(pred->ckey, lo), &tid);

	return pred->callback(&tid, pred->callback_state);
}

/*
 * The same predicate for lion_sparse_remove_if(), whose pairs carry their own
 * container key (DESIGN.md §13).
 */
static bool
lion_vac_is_dead_pair(uint32 ckey, uint16 lo, void *arg)
{
	LionVacPred *pred = (LionVacPred *) arg;
	ItemPointerData tid;

	lion_code_to_tid(lion_make_code(ckey, lo), &tid);

	return pred->callback(&tid, pred->callback_state);
}

/*
 * Filter one item of a posting set in place and return how many TIDs it lost.
 *
 * A container may GROW while losing members (a run that loses every other
 * member becomes a bitset), which is what the regrow path exists for; a
 * sparse segment is a plain sorted list and can only shrink.
 */
static uint32
lion_vac_filter_item(LionContainer *item, LionVacPred *pred)
{
	if (item->type == LION_CT_SPARSE)
		return lion_sparse_remove_if(item, lion_vac_is_dead_pair, pred);

	pred->ckey = item->ckey;
	return lion_container_remove_if(item, lion_vac_is_dead, pred);
}

/* Pick the smallest representation of a filtered item. */
static void
lion_vac_optimize_item(LionContainer *item)
{
	if (item->type != LION_CT_SPARSE)
		lion_container_optimize(item);
}

/*
 * ambulkdelete
 */
IndexBulkDeleteResult *
lionbulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			  IndexBulkDeleteCallback callback, void *callback_state)
{
	Relation	index = info->index;
	LionVacState vs;
	MemoryContext vaccxt;
	MemoryContext oldcxt;
	instr_time	started;
	BlockNumber blk;

	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

	INSTR_TIME_SET_CURRENT(started);

	memset(&vs, 0, sizeof(vs));
	vs.index = index;
	vs.heaprel = info->heaprel;
	vs.ix = lion_get_index_state(index);
	vs.callback = callback;
	vs.callback_state = callback_state;
	vs.stats = stats;
	vs.numtids = 0;
	vs.cbuf = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	vs.nblocks = RelationGetNumberOfBlocks(index);
	vs.visited = (uint8 *) palloc0((vs.nblocks + 7) / 8 + 1);
	lion_vac_visit(&vs, LION_METAPAGE_BLKNO);

	/*
	 * VACUUM does not run in a short-lived context, so all per-bucket work
	 * goes into a context that is reset between buckets, and the (possibly
	 * long) walk of a container chain into one that is reset per page.
	 */
	vaccxt = AllocSetContextCreate(CurrentMemoryContext,
								   "lion index vacuum",
								   ALLOCSET_DEFAULT_SIZES);
	vs.pagecxt = AllocSetContextCreate(CurrentMemoryContext,
									   "lion index vacuum page",
									   ALLOCSET_DEFAULT_SIZES);
	vs.bucketcxt = AllocSetContextCreate(CurrentMemoryContext,
										 "lion index vacuum bucket page",
										 ALLOCSET_DEFAULT_SIZES);

	/*
	 * The leaves, left to right (DESIGN.md §21).  Chain order and visit order
	 * still agree, which is what the §11 interlock needs: a split of a leaf
	 * moves entries only onto a brand new page immediately to its right, and
	 * the walk has not passed that one yet.
	 */
	blk = lion_dir_leftmost_leaf(index, vs.ix);
	while (BlockNumberIsValid(blk))
	{
		BlockNumber next;

		oldcxt = MemoryContextSwitchTo(vaccxt);
		lion_vacuum_leaf_page(&vs, blk, &next);
		MemoryContextSwitchTo(oldcxt);
		MemoryContextReset(vaccxt);

		blk = next;
		vacuum_delay_point(false);
	}

	/*
	 * Everything reachable has been walked, so whatever is left unvisited is
	 * a leak: recover it before the FSM is vacuumed (DESIGN.md §18).  This
	 * costs a buffer read per unvisited block and nothing at all for the
	 * blocks the walk accounted for, which in a healthy index is all of them.
	 */
	lion_vacuum_sweep(&vs);

	MemoryContextDelete(vs.bucketcxt);
	MemoryContextDelete(vs.pagecxt);
	MemoryContextDelete(vaccxt);
	pfree(vs.visited);
	pfree(vs.cbuf);

	stats->num_pages = RelationGetNumberOfBlocks(index);
	stats->num_index_tuples = vs.numtids;
	stats->estimated_count = false;
	stats->pages_newly_deleted = (BlockNumber) vs.pages_newly_deleted;
	stats->pages_deleted = (BlockNumber) vs.pages_deleted;
	stats->pages_free = (BlockNumber) vs.pages_deleted;

	lion_vac_tick(&vs.prof.total, started);
	elog(DEBUG1, "lion vacuum \"%s\": total %.1f ms (cleanup-lock wait %.1f, filter %.1f, apply %.1f, inline %.1f); "
		 "pages %ld visited / %ld changed / %ld freed, %ld records, items %ld rewritten / %ld deleted, %ld regrown, entries %ld rewritten / %ld deleted",
		 RelationGetRelationName(index),
		 INSTR_TIME_GET_MILLISEC(vs.prof.total),
		 INSTR_TIME_GET_MILLISEC(vs.prof.cleanup_wait),
		 INSTR_TIME_GET_MILLISEC(vs.prof.filter),
		 INSTR_TIME_GET_MILLISEC(vs.prof.apply),
		 INSTR_TIME_GET_MILLISEC(vs.prof.inlinework),
		 (long) vs.prof.pages_visited, (long) vs.prof.pages_changed,
		 (long) vs.pages_newly_deleted,
		 (long) vs.prof.records, (long) vs.prof.items_rewritten,
		 (long) vs.prof.items_deleted, (long) vs.prof.regrows,
		 (long) vs.prof.entries_rewritten, (long) vs.prof.entries_deleted);

	return stats;
}

/*
 * amvacuumcleanup
 */
IndexBulkDeleteResult *
lionvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	if (info->analyze_only)
		return stats;

	/*
	 * Make the pages ambulkdelete recorded as free visible to the upper
	 * levels of the free space map, so that lion_new_buffer() finds them
	 * (DESIGN.md §18).  This is what nbtree and every other index AM does
	 * here, and it is cheap for an index that freed nothing.
	 */
	IndexFreeSpaceMapVacuum(info->index);

	if (stats == NULL)
	{
		/*
		 * No ambulkdelete call was needed, so nothing has been counted: hand
		 * back the heap's estimate rather than zero, which would tell the
		 * planner the index is empty.
		 */
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
		stats->num_pages = RelationGetNumberOfBlocks(info->index);
		stats->estimated_count = true;
		stats->num_index_tuples = info->num_heap_tuples >= 0 ?
			info->num_heap_tuples : 0;
	}

	return stats;
}

/*
 * Filter the payload of one INLINE entry, which pass 1 has under a cleanup
 * lock, and say what has to happen to it.  Nothing is written here.
 *
 * The cheap answer comes first and is the one that matters: a VACUUM that
 * removes 1% of the rows leaves 99% of the entries untouched, and finding
 * that out must not cost a single allocation.  So the payload is filtered
 * straight out of the page (the cleanup lock makes it stable) into the shared
 * work buffer, and only an entry that really loses a member pays for the
 * second pass that builds its new payload.  Before this split, a 632k-entry
 * index spent 278 ms of a 378 ms ambulkdelete here, nearly all of it on
 * palloc/StringInfo work for entries that then turned out to be unchanged.
 *
 * Returns false when nothing changes.  Otherwise *res describes the write,
 * with everything allocated in vs->pagecxt, which lives until this bucket
 * page is done.
 */
static bool
lion_vacuum_inline_filter(LionVacState *vs, Page page, OffsetNumber off,
						 LionVacInline *res)
{
	ItemId		iid = PageGetItemId(page, off);
	LionEntryTuple *entry = (LionEntryTuple *) PageGetItem(page, iid);
	Size		itemsz = ItemIdGetLength(iid);
	Size		paylen = LION_ENTRY_PAYLOAD_LEN(entry, itemsz);
	const char *onpage = LionEntryGetPayload(entry);
	LionVacPred	pred;
	StringInfoData newpay;
	Size		cur = 0;
	Size		payoff;
	Size		need;
	uint64		removed = 0;
	uint64		ntids = 0;
	uint32		ncontainers = 0;

	pred.callback = vs->callback;
	pred.callback_state = vs->callback_state;

	/* Probe: does this entry lose anything at all? */
	while (lion_inline_fetch(onpage, paylen, &cur, vs->cbuf) > 0)
		removed += lion_vac_filter_item(vs->cbuf, &pred);

	if (removed == 0)
	{
		vs->numtids += (double) entry->ntids;
		return false;
	}

	/* It does, so build the payload that replaces it. */
	initStringInfo(&newpay);
	cur = 0;
	while (lion_inline_fetch(onpage, paylen, &cur, vs->cbuf) > 0)
	{
		(void) lion_vac_filter_item(vs->cbuf, &pred);

		if (vs->cbuf->cardinality == 0)
			continue;			/* drop empty containers and segments */

		lion_vac_optimize_item(vs->cbuf);
		appendBinaryStringInfo(&newpay, (char *) vs->cbuf,
							   lion_item_size(vs->cbuf));
		ncontainers++;
		ntids += vs->cbuf->cardinality;
	}

	vs->stats->tuples_removed += (double) removed;
	vs->numtids += (double) ntids;

	res->off = off;
	res->spill = false;
	res->payload = newpay.data;
	res->paylen = (Size) newpay.len;

	payoff = LionEntryPayloadOffset(entry);
	need = payoff + res->paylen;

	/*
	 * A filtered payload that still fits inside the bytes this entry already
	 * occupies is written there, padded with zeroes (DESIGN.md §18): no other
	 * entry on the bucket page moves, so the record carries this entry alone
	 * instead of everything after it.  The padding is bounded so that it
	 * cannot be mistaken for corruption and cannot waste a page; an entry
	 * that has lost more than that is compacted, paying the page-tail delta
	 * once.  An entry that ended up EMPTY is written all the same - its dead
	 * TIDs have to leave the page under this cleanup lock - and the final
	 * step for this page then deletes it.
	 */
	if (res->paylen <= (Size) vs->ix->meta.inline_limit && need <= itemsz)
	{
		res->writesz = (itemsz - need <= LION_ENTRY_SLACK_BOUND) ? itemsz : need;
		res->tuple = (LionEntryTuple *) palloc0(res->writesz);
		memcpy(res->tuple, entry, payoff);
		if (res->paylen > 0)
			memcpy(((char *) res->tuple) + payoff, res->payload, res->paylen);
		res->tuple->ncontainers = ncontainers;
		res->tuple->ntids = ntids;
		return true;
	}

	/*
	 * It grew.  Removal can do that - a RUN that loses every other member
	 * becomes a BITSET - so the entry may no longer fit the page, or may have
	 * outgrown inline_limit, and then its posting set spills onto container
	 * pages exactly as an insert would.  Try the plain overwrite first; the
	 * spill is decided when that fails.
	 */
	if (res->paylen <= (Size) vs->ix->meta.inline_limit)
	{
		res->tuple = lion_entry_rebuild(entry, res->payload, res->paylen,
									   &res->writesz);
		res->tuple->ncontainers = ncontainers;
		res->tuple->ntids = ntids;
		return true;
	}

	res->spill = true;
	res->tuple = lion_entry_rebuild(entry, NULL, 0, &res->writesz);
	res->tuple->ncontainers = ncontainers;
	res->tuple->ntids = ntids;
	return true;
}

/* Is the entry at (page, off) the one pass 1 recorded? */
static bool
lion_vac_entry_matches(Page page, OffsetNumber off, const LionVacEntry *ent)
{
	ItemId		iid;
	LionEntryTuple *e;

	if (off < lion_page_first_data(page) || off > PageGetMaxOffsetNumber(page))
		return false;
	iid = PageGetItemId(page, off);
	if (!ItemIdIsUsed(iid))
		return false;
	e = (LionEntryTuple *) PageGetItem(page, iid);
	if (LionEntryIsPivot(e))
		return false;
	if (e->attno != ent->attno)
		return false;			/* another key column (DESIGN.md §24) */
	if (lion_entry_kind(e) != ent->kind || e->keylen != ent->keylen)
		return false;

	/*
	 * One entry per key, so equal stored bytes mean the same entry.  That is
	 * the whole identity check: offsets move, keys do not (DESIGN.md §21).
	 */
	return ent->keylen == 0 ||
		memcmp(LionEntryGetKey(e), ent->keydata, ent->keylen) == 0;
}

/*
 * Is ref still pointing at ent?  The caller holds ref->buf locked.  A cheap
 * hit at the remembered offset is the common case; a scan of the page catches
 * an insert that shifted it; anything else means it moved to another leaf.
 */
static bool
lion_vac_ref_valid(LionVacEntryRef *ref, const LionVacEntry *ent)
{
	Page		page = BufferGetPage(ref->buf);
	OffsetNumber off;
	OffsetNumber maxoff;

	if (!LionPageIsLeaf(page))
		return false;
	if (lion_vac_entry_matches(page, ref->off, ent))
		return true;

	maxoff = PageGetMaxOffsetNumber(page);
	for (off = lion_page_first_data(page); off <= maxoff; off++)
	{
		if (lion_vac_entry_matches(page, off, ent))
		{
			ref->off = off;
			return true;
		}
	}
	return false;
}

/*
 * Find ent again after a split moved it off the leaf ref pointed at.  NOTHING
 * may be held: this descends the directory, and taking a directory lock while
 * holding a container page would close the very cycle rule 2 avoids.
 */
static void
lion_vac_ref_relocate(LionVacState *vs, LionVacEntryRef *ref,
					 const LionVacEntry *ent)
{
	LionSearchKey sk;
	LionEntryTuple *probe;
	Size		probesz = MAXALIGN(LION_ENTRY_HDRSZ + ent->keylen);
	Buffer		buf;
	OffsetNumber off;

	if (BufferIsValid(ref->buf))
	{
		ReleaseBuffer(ref->buf);
		ref->buf = InvalidBuffer;
	}

	probe = (LionEntryTuple *) palloc0(probesz);
	probe->hash = ent->hash;
	probe->flags = (uint16) ((ent->kind == LION_KIND_NULL) ? LION_ENTRY_NULLKEY :
							 (ent->kind == LION_KIND_EMPTY) ? LION_ENTRY_EMPTYKEY :
							 0);
	probe->keylen = ent->keylen;
	probe->attno = ent->attno;
	if (ent->keylen > 0)
		memcpy(LionEntryGetKey(probe), ent->keydata, ent->keylen);
	lion_search_key_exact(vs->ix, &sk, probe);

	if (!lion_dir_find(vs->index, vs->heaprel, vs->ix, &sk,
					   BUFFER_LOCK_SHARE, false, &buf, &off, NULL))
	{
		if (BufferIsValid(buf))
			UnlockReleaseBuffer(buf);
		pfree(probe);
		elog(ERROR, "lion index \"%s\": an entry VACUUM is working on has disappeared",
			 RelationGetRelationName(vs->index));
	}

	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	ref->buf = buf;
	ref->off = off;
	pfree(probe);
}

/* Lock ref's leaf in mode, relocating the entry when it is no longer there. */
static void
lion_vac_ref_lock(LionVacState *vs, LionVacEntryRef *ref,
				 const LionVacEntry *ent, int mode)
{
	for (;;)
	{
		LockBuffer(ref->buf, mode);
		if (lion_vac_ref_valid(ref, ent))
			return;
		LockBuffer(ref->buf, BUFFER_LOCK_UNLOCK);
		lion_vac_ref_relocate(vs, ref, ent);
		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * Vacuum one directory leaf: pass 1 over its entries, pass 2 over the
 * container chains of the CHAIN entries it holds, then the final step that
 * deletes the entries whose posting sets are now empty (see the file header
 * and DESIGN.md §18).
 */
static void
lion_vacuum_leaf_page(LionVacState *vs, BlockNumber blk, BlockNumber *nextp)
{
	Buffer		buf;
	Page		page;
	OffsetNumber maxoff;
	OffsetNumber off;
	LionVacEntry *ents;
	LionVacInline *inl;
	instr_time	t0;
	int			nents = 0;
	int			ninl = 0;
	int			nmaydelete = 0;
	int			i;
	MemoryContext oldcxt;

	oldcxt = MemoryContextSwitchTo(vs->bucketcxt);

	/*
	 * Pass 1.  Every leaf is cleanup-locked, whether or not it holds an INLINE
	 * entry that changes, and the lock is taken with nothing else held, so the
	 * wait for concurrent readers' pins is interruptible.
	 */
	buf = ReadBuffer(vs->index, blk);
	INSTR_TIME_SET_CURRENT(t0);
	LockBufferForCleanup(buf);
	lion_vac_tick(&vs->prof.cleanup_wait, t0);
	lion_vac_visit(vs, blk);
	page = BufferGetPage(buf);

	if (!LionPageIsLeaf(page))
	{
		UnlockReleaseBuffer(buf);
		elog(ERROR, "lion index: block %u is not a directory leaf", blk);
	}

	*nextp = LionPageGetOpaque(page)->rightlink;
	maxoff = PageGetMaxOffsetNumber(page);
	ents = (LionVacEntry *) palloc(sizeof(LionVacEntry) * (maxoff + 1));
	inl = (LionVacInline *) palloc(sizeof(LionVacInline) * (maxoff + 1));

	INSTR_TIME_SET_CURRENT(t0);
	for (off = lion_page_first_data(page); off <= maxoff; off++)
	{
		ItemId		iid = PageGetItemId(page, off);
		LionEntryTuple *entry;
		LionVacEntry *ent;

		if (!ItemIdIsUsed(iid))
			continue;

		entry = (LionEntryTuple *) PageGetItem(page, iid);
		ent = &ents[nents++];
		ent->off = off;
		ent->hash = entry->hash;
		ent->head = entry->head;
		ent->maydelete = false;
		ent->attno = entry->attno;
		ent->kind = lion_entry_kind(entry);
		ent->keylen = entry->keylen;
		ent->keydata = NULL;
		if (entry->keylen > 0)
		{
			ent->keydata = (char *) palloc(entry->keylen);
			memcpy(ent->keydata, LionEntryGetKey(entry), entry->keylen);
		}

		if ((entry->flags & LION_ENTRY_INLINE) != 0)
		{
			ent->ischain = false;
			if (lion_vacuum_inline_filter(vs, page, off, &inl[ninl]))
			{
				ent->maydelete = (inl[ninl].tuple->ntids == 0);
				ninl++;
			}
			else
				ent->maydelete = (entry->ntids == 0);
		}
		else if ((entry->flags & LION_ENTRY_CHAIN) != 0)
		{
			ent->ischain = true;
			if (!BlockNumberIsValid(entry->head) ||
				!BlockNumberIsValid(entry->tail))
			{
				UnlockReleaseBuffer(buf);
				elog(ERROR, "lion index: chain entry %u on block %u has no container pages",
					 off, blk);
			}
		}
		else
		{
			uint16		flags = entry->flags;

			UnlockReleaseBuffer(buf);
			elog(ERROR, "lion index: entry %u on block %u has invalid flags %u",
				 off, blk, flags);
		}

		CHECK_FOR_INTERRUPTS();
	}

	/*
	 * Every INLINE entry that changed goes back in ONE record.  Entry offsets
	 * survive PageIndexTupleOverwrite(), so the writes do not disturb each
	 * other, and one record per PAGE instead of one per ENTRY is what keeps a
	 * leaf full of one-TID entries from costing thousands of records.
	 */
	if (ninl > 0)
	{
		GenericXLogState *xstate = GenericXLogStart(vs->index);
		Page		p = GenericXLogRegisterBuffer(xstate, buf, 0);
		int			nwritten = 0;

		for (i = 0; i < ninl; i++)
		{
			if (inl[i].spill)
				continue;
			if (PageIndexTupleOverwrite(p, inl[i].off, inl[i].tuple,
										inl[i].writesz))
				nwritten++;
			else
				inl[i].spill = true;	/* no room: it has to move to a chain */
		}

		if (nwritten > 0)
		{
			GenericXLogFinish(xstate);
			vs->prof.records++;
			vs->prof.entries_rewritten += nwritten;
		}
		else
			GenericXLogAbort(xstate);

		/*
		 * A payload that outgrew its entry moves onto container pages, one
		 * record of its own each.  This is rare (it needs a RUN container to
		 * turn into a BITSET while losing members), so it is not worth
		 * batching, and lion_entry_spill() rewrites the entry itself.
		 */
		for (i = 0; i < ninl; i++)
		{
			if (!inl[i].spill)
				continue;
			lion_entry_spill(vs->index, vs->heaprel, buf, inl[i].off,
							inl[i].tuple, inl[i].payload, inl[i].paylen);
			vs->prof.records++;
			vs->prof.entries_rewritten++;
		}
	}
	lion_vac_tick(&vs->prof.inlinework, t0);

	/* The page keeps its pin: pass 2 locks it again for every window. */
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);

	vacuum_delay_point(false);

	/* Pass 2, holding no page lock between its steps. */
	for (i = 0; i < nents; i++)
	{
		if (ents[i].ischain)
			lion_vacuum_chain(vs, buf, &ents[i]);
	}

	for (i = 0; i < nents; i++)
		nmaydelete += ents[i].maydelete ? 1 : 0;

	/*
	 * The final step for this page: delete the entries whose posting sets are
	 * empty, and free the chains they owned.  It comes last because
	 * PageIndexMultiDelete() renumbers everything after a deleted entry, so
	 * from here on no offset into this page means anything (DESIGN.md §18).
	 */
	if (nmaydelete > 0)
		lion_vacuum_delete_entries(vs, buf, ents, nents);

	ReleaseBuffer(buf);

	MemoryContextSwitchTo(oldcxt);
	MemoryContextReset(vs->bucketcxt);
}

/*
 * Pass 2 for one CHAIN entry: walk its container chain from head, left to
 * right, holding no page lock between pages.  entrybuf is the directory leaf
 * pass 1 found the entry on, pinned and not locked; the entry may MOVE while
 * this runs (an insert splitting the leaf), which is what the reference and
 * its validation are for (DESIGN.md §21).
 */
static void
lion_vacuum_chain(LionVacState *vs, Buffer entrybuf, LionVacEntry *ent)
{
	BlockNumber blk;
	LionVacEntryRef ref;
	LionEntryTuple *entry;

	/*
	 * DESIGN.md §22: the entry's head block is the ROOT of the posting tree,
	 * and the leaves - the only pages that hold TIDs - are still one
	 * rightlinked list in ckey order, so the walk below is what it always was
	 * once it has descended to the leftmost of them.  Internal pages are not
	 * cleanup-locked and need not be (§11: they hold no TIDs), and they are
	 * never empty while the set lives, so the leak sweep leaves them alone.
	 */
	blk = lion_posting_leftmost_leaf(vs->index, ent->hash, ent->head);
	lion_vac_visit(vs, ent->head);

	ref.buf = entrybuf;
	ref.off = ent->off;
	IncrBufferRefCount(ref.buf);	/* the walk owns a pin of its own */

	while (BlockNumberIsValid(blk))
	{
		blk = lion_vacuum_container_page(vs, &ref, ent, blk);

		vacuum_delay_point(false);
	}

	/* Report what the entry holds now that every page has been visited. */
	lion_vac_ref_lock(vs, &ref, ent, BUFFER_LOCK_SHARE);
	entry = lion_page_entry(BufferGetPage(ref.buf), ref.off);
	vs->numtids += (double) entry->ntids;
	ent->maydelete = (entry->ntids == 0 && entry->ncontainers == 0);
	LockBuffer(ref.buf, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(ref.buf);
}

/*
 * The final step for one directory leaf: delete every entry whose posting set is
 * empty, and free the container chains those entries owned (DESIGN.md §18).
 *
 * The emptiness is decided HERE, under the cleanup lock, and not from what
 * pass 1 or pass 2 saw: an insert may have put a TID back into the entry in
 * between, and then the entry (and its chain) has to stay.  ntids == 0 with
 * ncontainers == 0 is exactly "this posting set holds nothing anywhere",
 * because every item that reaches cardinality 0 is deleted from its page in
 * the same record that decrements the counters.
 *
 * The order is the one §18 prescribes: the entries go first, in one record,
 * and only then are the chain pages marked free.  From the moment the entry
 * is gone the chain is unreachable for any new reader or insert, and a crash
 * in between merely leaks pages the sweep picks up.
 */
static int
lion_vac_cmp_offset(const void *a, const void *b)
{
	OffsetNumber x = *(const OffsetNumber *) a;
	OffsetNumber y = *(const OffsetNumber *) b;

	return (x < y) ? -1 : ((x > y) ? 1 : 0);
}

static void
lion_vacuum_delete_entries(LionVacState *vs, Buffer buf, LionVacEntry *ents,
						  int nents)
{
	Page		page;
	OffsetNumber *delofs;
	BlockNumber *heads;
	uint32	   *hashes;
	instr_time	t0;
	int			ndel = 0;
	int			nfree = 0;
	int			i;

	if (nents == 0)
		return;

	delofs = (OffsetNumber *) palloc(sizeof(OffsetNumber) * nents);
	heads = (BlockNumber *) palloc(sizeof(BlockNumber) * nents);
	hashes = (uint32 *) palloc(sizeof(uint32) * nents);

	INSTR_TIME_SET_CURRENT(t0);
	LockBufferForCleanup(buf);
	lion_vac_tick(&vs->prof.cleanup_wait, t0);
	page = BufferGetPage(buf);

	for (i = 0; i < nents; i++)
	{
		LionVacEntryRef ref;
		LionEntryTuple *entry;

		if (!ents[i].maydelete)
			continue;

		/*
		 * The offset pass 1 recorded is only a hint (DESIGN.md §21).  An entry
		 * that is no longer on this leaf at all has moved to a leaf further
		 * right, which this VACUUM has not reached yet or will not reach; it
		 * simply stays until the next one, and deleting the WRONG entry is
		 * what the check prevents.
		 */
		ref.buf = buf;
		ref.off = ents[i].off;
		if (!lion_vac_ref_valid(&ref, &ents[i]))
			continue;

		entry = lion_page_entry(page, ref.off);
		if (entry->ntids != 0 || entry->ncontainers != 0)
			continue;

		delofs[ndel++] = ref.off;

		if ((entry->flags & LION_ENTRY_CHAIN) != 0)
		{
			heads[nfree] = entry->head;
			hashes[nfree] = entry->hash;
			nfree++;
		}
	}

	if (ndel > 0)
	{
		if (ndel > 1)
			qsort(delofs, ndel, sizeof(OffsetNumber), lion_vac_cmp_offset);
		lion_dir_delete(vs->index, buf, delofs, ndel);
		vs->prof.records++;
		vs->prof.entries_deleted += ndel;
	}

	LockBuffer(buf, BUFFER_LOCK_UNLOCK);

	/*
	 * Test hook: the entries are gone and WAL-logged, and their chains are
	 * unreachable but not yet marked free.  A crash here is the documented
	 * leak of DESIGN.md §18 - harmless, because the pages are unreferenced
	 * and the sweep at the end of the next ambulkdelete finds them.
	 * test/recovery/run.sh parks a VACUUM here and pulls the server out from
	 * under it.  Compiles to nothing without --enable-injection-points.
	 */
	if (nfree > 0)
		INJECTION_POINT("lion-vacuum-entries-deleted", NULL);

	/*
	 * Now that nothing points at them, the pages of the freed chains can go.
	 * This takes cleanup locks, so it happens with the leaf unlocked:
	 * VACUUM never waits for a page while holding another LWLock (§11).
	 */
	for (i = 0; i < nfree; i++)
		lion_vacuum_free_chain(vs, hashes[i], heads[i]);

	pfree(hashes);
	pfree(heads);
	pfree(delofs);
}

/*
 * Mark every page of a freed chain LION_PAGE_DELETED and hand it to the free
 * space map (DESIGN.md §18).  The entry that pointed at this chain is already
 * gone, so nothing can reach these pages any more except a reader that copied
 * the entry before it was deleted - which is what safexid and the owner check
 * in the page's special area are for.
 *
 * ConditionalLockBufferForCleanup, never a wait: this runs with nothing else
 * held, but a reader that still has one of these pages pinned would otherwise
 * stall VACUUM for no gain at all.  A page that cannot be taken is simply
 * left behind; it is unreferenced, and the sweep at the end of ambulkdelete
 * (or the next VACUUM) finds it.  Giving up also ends the walk, because the
 * rightlink cannot be read without the lock.
 */
static void
lion_vacuum_free_level(LionVacState *vs, uint32 hash, BlockNumber head,
					   BlockNumber first)
{
	BlockNumber blk = first;

	while (BlockNumberIsValid(blk))
	{
		Buffer		buf;
		Page		page;
		BlockNumber next;
		GenericXLogState *xstate;
		Page		p;

		buf = ReadBuffer(vs->index, blk);
		if (!ConditionalLockBufferForCleanup(buf))
		{
			ReleaseBuffer(buf);
			return;
		}

		page = BufferGetPage(buf);

		/*
		 * Only ever free a page that still says it belongs to this set and, if
		 * it is a LEAF, that really is empty.  Neither can fail as things
		 * stand - the entry was deleted with ncontainers == 0 - but freeing a
		 * page that is in use would be unrecoverable, so it is checked rather
		 * than asserted.  An INTERNAL page holds downlinks and no TIDs, so
		 * "empty" says nothing about it; it goes when its whole subtree does.
		 */
		if (!lion_page_owns_entry(page, hash, head) ||
			(LionPageIsPostingLeaf(page) && PageGetMaxOffsetNumber(page) != 0))
		{
			UnlockReleaseBuffer(buf);
			return;
		}

		next = LionPageGetOpaque(page)->rightlink;

		xstate = GenericXLogStart(vs->index);
		p = GenericXLogRegisterBuffer(xstate, buf, GENERIC_XLOG_FULL_IMAGE);
		lion_page_set_deleted(p, ReadNextFullTransactionId());
		GenericXLogFinish(xstate);
		UnlockReleaseBuffer(buf);

		RecordFreeIndexPage(vs->index, blk);
		lion_vac_unvisit(vs, blk);
		vs->pages_newly_deleted++;
		vs->pages_deleted++;
		vs->prof.records++;

		blk = next;
		CHECK_FOR_INTERRUPTS();
	}
}

static void
lion_vacuum_free_chain(LionVacState *vs, uint32 hash, BlockNumber head)
{
	BlockNumber levelfirst[LION_POSTING_MAX_HEIGHT + 1];
	BlockNumber blk = head;
	int			nlevels = 0;
	int			i;

	/*
	 * DESIGN.md §22: the leftmost page of every level, from the root down.
	 * The entry is already gone, so nothing can reach these pages except a
	 * reader that copied the entry before the delete - which the owner stamp
	 * and safexid handle - and a SHARE lock is enough to read the shape.
	 */
	while (BlockNumberIsValid(blk) && nlevels <= LION_POSTING_MAX_HEIGHT)
	{
		Buffer		buf = ReadBuffer(vs->index, blk);
		Page		page;
		BlockNumber child = InvalidBlockNumber;
		uint16		level;

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (!lion_page_owns_entry(page, hash, head))
		{
			UnlockReleaseBuffer(buf);
			break;
		}
		level = LionPageGetOpaque(page)->level;
		if (level > 0)
		{
			OffsetNumber f = lion_posting_first_data(page);

			if (f <= PageGetMaxOffsetNumber(page))
				child = lion_posting_pivot(page, f)->child;
		}
		UnlockReleaseBuffer(buf);

		levelfirst[nlevels++] = blk;
		if (level == 0)
			break;
		blk = child;
		CHECK_FOR_INTERRUPTS();
	}

	/* Bottom up, so that a level is only unreachable once it is empty. */
	for (i = nlevels - 1; i >= 0; i--)
		lion_vacuum_free_level(vs, hash, head, levelfirst[i]);
}

/*
 * Leak recovery (DESIGN.md §18).
 *
 * Every block the walk accounted for is marked in vs->visited, so what is
 * left is either a page a concurrent insert created - which this VACUUM knows
 * nothing about and must not touch - or a leak.  Two kinds of leak are
 * recoverable and this is where they come back:
 *
 *	- a page that is already DELETED, from a VACUUM that crashed between
 *	  recording it and vacuuming the free space map, or one this VACUUM did
 *	  not free itself.  It goes back into the map.
 *	- an EMPTY container page nothing references, which is what a crash
 *	  between the two steps of a whole-chain free leaves, and what an
 *	  interrupted multi-page spill leaves after its first page.  It becomes a
 *	  DELETED page and goes into the map.
 *
 * An empty container page of a LIVE chain is never mistaken for one of these,
 * because pass 2 walks every page of every chain it found and marks it
 * visited; a chain created after pass 1 read its bucket page has no empty
 * page in it (a spill and a split both fill every page they allocate inside
 * the record that allocates it, under the lock they hold throughout).
 *
 * Nothing here ever waits: an unreferenced page nobody can reach should not
 * be locked by anyone, and if it somehow is, the next VACUUM will find it.
 */
/*
 * Are all of this internal posting page's children already DELETED pages?
 *
 * That is what makes an unreferenced INTERNAL page provably dead (DESIGN.md
 * §22).  A LIVE internal page cannot pass it: every leaf of a live posting set
 * was visited by pass 2 and is therefore never swept, and a page a concurrent
 * insert created since then is full of items a split just put there - either
 * way its children are not DELETED.  The children are only ever
 * CONDITIONALLY locked, so this never waits and cannot invert a lock order.
 */
static bool
lion_vac_children_deleted(LionVacState *vs, Page page)
{
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	OffsetNumber off;

	for (off = lion_posting_first_data(page); off <= maxoff; off++)
	{
		BlockNumber child = lion_posting_pivot(page, off)->child;
		Buffer		cbuf;
		bool		deleted;

		if (!BlockNumberIsValid(child) || child >= vs->nblocks)
			return false;

		cbuf = ReadBuffer(vs->index, child);
		if (!ConditionalLockBuffer(cbuf))
		{
			ReleaseBuffer(cbuf);
			return false;
		}
		deleted = !PageIsNew(BufferGetPage(cbuf)) &&
			PageGetSpecialSize(BufferGetPage(cbuf)) == LION_SPECIAL_SIZE &&
			LionPageGetOpaque(BufferGetPage(cbuf))->page_id == LION_PAGE_ID &&
			LionPageIsDeleted(BufferGetPage(cbuf));
		UnlockReleaseBuffer(cbuf);

		if (!deleted)
			return false;
	}

	return true;
}

/* Mark blk DELETED and hand it to the free space map.  buf is cleanup-locked. */
static void
lion_vac_free_page(LionVacState *vs, Buffer buf, BlockNumber blk)
{
	GenericXLogState *xstate = GenericXLogStart(vs->index);
	Page		p = GenericXLogRegisterBuffer(xstate, buf,
											  GENERIC_XLOG_FULL_IMAGE);

	lion_page_set_deleted(p, ReadNextFullTransactionId());
	GenericXLogFinish(xstate);
	UnlockReleaseBuffer(buf);

	RecordFreeIndexPage(vs->index, blk);
	lion_vac_visit(vs, blk);
	vs->pages_newly_deleted++;
	vs->pages_deleted++;
	vs->prof.records++;
}

static void
lion_vacuum_sweep(LionVacState *vs)
{
	int			round;

	/*
	 * Rounds, because a leaked posting TREE has to go bottom up (§22): the
	 * leaves are empty and go in the first round, and an internal page becomes
	 * provably dead only once its own children have.  The loop is bounded by
	 * the tallest tree there can be and stops as soon as a round frees
	 * nothing, so a healthy index - where nothing is unaccounted for - reads
	 * no pages at all and runs the outer loop twice.
	 */
	for (round = 0; round <= LION_POSTING_MAX_HEIGHT; round++)
	{
		BlockNumber blk;
		bool		progress = false;

		for (blk = 1; blk < vs->nblocks; blk++)
		{
			Buffer		buf;
			Page		page;

			if (lion_vac_visited(vs, blk))
				continue;

			buf = ReadBuffer(vs->index, blk);
			if (!ConditionalLockBufferForCleanup(buf))
			{
				ReleaseBuffer(buf);
				continue;
			}
			page = BufferGetPage(buf);

			if (PageIsNew(page))
			{
				/* An extension whose WAL record never happened. */
				UnlockReleaseBuffer(buf);
				RecordFreeIndexPage(vs->index, blk);
				lion_vac_visit(vs, blk);
				vs->pages_deleted++;
				progress = true;
				continue;
			}

			if (PageGetSpecialSize(page) != LION_SPECIAL_SIZE ||
				LionPageGetOpaque(page)->page_id != LION_PAGE_ID ||
				!LionPageIsContainer(page))
			{
				UnlockReleaseBuffer(buf);
				continue;		/* not ours to reason about */
			}

			if (LionPageIsDeleted(page))
			{
				UnlockReleaseBuffer(buf);
				RecordFreeIndexPage(vs->index, blk);
				lion_vac_visit(vs, blk);
				vs->pages_deleted++;
				progress = true;
				continue;
			}

			if (LionPageIsPostingInternal(page))
			{
				if (!lion_vac_children_deleted(vs, page))
				{
					UnlockReleaseBuffer(buf);
					continue;
				}
				lion_vac_free_page(vs, buf, blk);
				progress = true;
				continue;
			}

			if (PageGetMaxOffsetNumber(page) == 0)
			{
				lion_vac_free_page(vs, buf, blk);
				progress = true;
				continue;
			}

			UnlockReleaseBuffer(buf);
			CHECK_FOR_INTERRUPTS();
		}

		if (!progress)
			break;
	}
}

/*
 * Private copy of the CHAIN entry at (entrybuf, entryoff), which the caller
 * holds locked.  Concurrent inserts change the entry, so it is re-read from
 * the page inside every window and its counters are only ever updated with
 * deltas.
 */
static LionEntryTuple *
lion_vacuum_entry_copy(Buffer entrybuf, OffsetNumber entryoff, Size *size)
{
	Page		page = BufferGetPage(entrybuf);
	ItemId		iid = PageGetItemId(page, entryoff);
	LionEntryTuple *entry;

	if (!ItemIdIsUsed(iid))
		elog(ERROR, "lion index: entry %u on block %u is gone",
			 entryoff, BufferGetBlockNumber(entrybuf));

	entry = (LionEntryTuple *) PageGetItem(page, iid);
	if ((entry->flags & LION_ENTRY_CHAIN) == 0)
		elog(ERROR, "lion index: entry %u on block %u is no longer a chain entry",
			 entryoff, BufferGetBlockNumber(entrybuf));

	return lion_entry_rebuild(entry, NULL, 0, size);
}

/*
 * Vacuum one container page of a chain and return the block to continue at.
 * Nothing is held on entry or on return; see the file header for the lock
 * protocol this implements.
 */
static BlockNumber
lion_vacuum_container_page(LionVacState *vs, LionVacEntryRef *ref,
						  const LionVacEntry *ent, BlockNumber blk)
{
	Buffer		buf;
	BlockNumber next;
	LionVacWork	w;
	uint32	   *grown = NULL;
	int			ngrown = 0;
	int			i;
	instr_time	t0;
	MemoryContext oldcxt;

	oldcxt = MemoryContextSwitchTo(vs->pagecxt);

	buf = ReadBuffer(vs->index, blk);

	for (;;)
	{
		/*
		 * Every page of the chain is visited under a cleanup lock, whether or
		 * not it turns out to have anything to remove.  Nothing else is held,
		 * so the wait for a reader's pin is interruptible.
		 */
		INSTR_TIME_SET_CURRENT(t0);
		LockBufferForCleanup(buf);
		lion_vac_tick(&vs->prof.cleanup_wait, t0);
		lion_vac_visit(vs, blk);
		vs->prof.pages_visited++;

		/*
		 * DESIGN.md §22: the only page that can stop being a leaf is the ROOT,
		 * and only by being pushed down - an insert moving its containers onto
		 * a brand new child.  The walk then restarts at the new leftmost leaf,
		 * which is safe for exactly the reason the split hole of §11 is safe:
		 * this cleanup lock waited for every pin on the page those containers
		 * came from, so no reader can be holding a stale copy of them, and the
		 * page they moved to did not exist a moment ago.
		 */
		if (!LionPageIsPostingLeaf(BufferGetPage(buf)))
		{
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			ReleaseBuffer(buf);
			MemoryContextSwitchTo(oldcxt);
			MemoryContextReset(vs->pagecxt);
			return lion_posting_leftmost_leaf(vs->index, ent->hash, ent->head);
		}

		INSTR_TIME_SET_CURRENT(t0);
		next = lion_vacuum_filter_page(vs, buf, &w, ent);
		lion_vac_tick(&vs->prof.filter, t0);

		if (w.nwork == 0 && w.ndel == 0)
		{
			/* Nothing to remove: no WAL record, and no entry page needed. */
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			break;
		}

		if (ConditionalLockBuffer(ref->buf))
		{
			if (!lion_vac_ref_valid(ref, ent))
			{
				/*
				 * A concurrent insert split the leaf and took the entry with
				 * it.  Let go of everything - a descent may not be made while
				 * a container page is held - find it again, and start over.
				 */
				LockBuffer(ref->buf, BUFFER_LOCK_UNLOCK);
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);
				lion_vac_ref_relocate(vs, ref, ent);
				CHECK_FOR_INTERRUPTS();
				continue;
			}

			INSTR_TIME_SET_CURRENT(t0);
			lion_vacuum_apply_page(vs, ref, buf, &w, &grown, &ngrown);
			lion_vac_tick(&vs->prof.apply, t0);
			LockBuffer(ref->buf, BUFFER_LOCK_UNLOCK);
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			break;
		}

		/*
		 * An insert holds the entry page.  Waiting for it here would be a
		 * lock-order inversion, so let go of the page, take the entry page
		 * with nothing held and try the cleanup lock again.
		 */
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		LockBuffer(ref->buf, BUFFER_LOCK_EXCLUSIVE);

		if (!lion_vac_ref_valid(ref, ent))
		{
			LockBuffer(ref->buf, BUFFER_LOCK_UNLOCK);
			lion_vac_ref_relocate(vs, ref, ent);
			CHECK_FOR_INTERRUPTS();
			continue;
		}

		if (ConditionalLockBufferForCleanup(buf))
		{
			/* The page was unlocked in between, so filter it again. */
			INSTR_TIME_SET_CURRENT(t0);
			next = lion_vacuum_filter_page(vs, buf, &w, ent);
			lion_vac_tick(&vs->prof.filter, t0);
			if (w.nwork > 0 || w.ndel > 0)
			{
				INSTR_TIME_SET_CURRENT(t0);
				lion_vacuum_apply_page(vs, ref, buf, &w, &grown, &ngrown);
				lion_vac_tick(&vs->prof.apply, t0);
			}
			LockBuffer(ref->buf, BUFFER_LOCK_UNLOCK);
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			break;
		}

		LockBuffer(ref->buf, BUFFER_LOCK_UNLOCK);
		CHECK_FOR_INTERRUPTS();
	}

	ReleaseBuffer(buf);

	/*
	 * Containers that grew out of their slot are re-placed one at a time,
	 * each in a window of its own, because placing one may split its page and
	 * move the others to the right.
	 */
	for (i = 0; i < ngrown; i++)
	{
		vs->prof.regrows++;
		lion_vacuum_regrow(vs, ref, ent, blk, grown[i]);
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextReset(vs->pagecxt);

	return next;
}

/*
 * Filter every container of the cleanup-locked page buf into *w, without
 * changing the page.  Returns the block to continue the walk at.
 *
 * The rightlink is read here, before anything on this page can move: a split
 * only ever inserts brand new pages between this one and the old right
 * sibling, and everything that moves onto them has already been filtered
 * (except the containers that grew, which lion_vacuum_regrow() follows), so
 * continuing at the old rightlink visits every container that still needs
 * work.
 */
static BlockNumber
lion_vacuum_filter_page(LionVacState *vs, Buffer buf, LionVacWork *w,
					   const LionVacEntry *ent)
{
	Page		page = BufferGetPage(buf);
	BlockNumber blk = BufferGetBlockNumber(buf);
	OffsetNumber maxoff;
	OffsetNumber off;
	LionVacPred	pred;

	/*
	 * DESIGN.md §18: the page has to claim this chain.  VACUUM is the only
	 * thing that frees a chain and it does not free the one it is walking, so
	 * a mismatch here is corruption rather than a race.
	 */
	if (!lion_page_owns_entry(page, ent->hash, ent->head))
		elog(ERROR, "lion index: block %u is not a container page of the chain at %u",
			 blk, ent->head);

	maxoff = PageGetMaxOffsetNumber(page);

	w->nwork = 0;
	w->ndel = 0;
	w->delremoved = 0;
	w->work = (LionVacItem *) palloc(sizeof(LionVacItem) * (maxoff + 1));
	w->delofs = (OffsetNumber *) palloc(sizeof(OffsetNumber) * (maxoff + 1));

	pred.callback = vs->callback;
	pred.callback_state = vs->callback_state;

	for (off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId		iid = PageGetItemId(page, off);
		LionVacItem *item;
		Size		isize;
		uint32		nremoved;

		if (!ItemIdIsUsed(iid))
			continue;

		isize = ItemIdGetLength(iid);
		if (isize > LION_CONTAINER_MAX_SIZE)
			elog(ERROR, "lion index: item of %zu bytes at %u/%u",
				 isize, blk, off);
		memcpy(vs->cbuf, PageGetItem(page, iid), isize);

		nremoved = lion_vac_filter_item(vs->cbuf, &pred);
		if (nremoved == 0)
			continue;

		if (vs->cbuf->cardinality == 0)
		{
			/* Empty containers and empty segments both go away. */
			w->delofs[w->ndel++] = off;
			w->delremoved += nremoved;
			continue;
		}

		lion_vac_optimize_item(vs->cbuf);
		item = &w->work[w->nwork++];
		item->off = off;
		item->size = lion_item_size(vs->cbuf);
		item->removed = nremoved;

		/*
		 * SHRINK IN PLACE (DESIGN.md §18).  A filtered item is written back
		 * into the bytes the page has already allotted it whenever the bytes
		 * it gives up are within the slack bound: PageIndexTupleOverwrite()
		 * then moves no other item, so the GenericXLog delta is this item and
		 * nothing else instead of the whole tail of the page.  An item that
		 * has lost far more than that gives the space back, paying the
		 * page-tail delta once.
		 */
		item->writesz = item->size;
		if (isize >= item->size && isize - item->size <= LION_ITEM_SLACK_VACUUM)
			item->writesz = isize;

		item->c = (LionContainer *) palloc0(item->writesz);
		memcpy(item->c, vs->cbuf, item->size);
	}

	return LionPageGetOpaque(page)->rightlink;
}

/*
 * Apply the result of lion_vacuum_filter_page() to the page: everything that
 * fits in place goes into one WAL record that also carries the entry tuple
 * with its new counters.  The caller holds a cleanup lock on buf and the
 * entry page EXCLUSIVE, and has not released either since the filtering.
 *
 * *grownp receives the ckeys of the containers that no longer fit their slot;
 * they are dealt with by lion_vacuum_regrow() once both locks are gone.
 */
static void
lion_vacuum_apply_page(LionVacState *vs, LionVacEntryRef *ref, Buffer buf,
					  LionVacWork *w, uint32 **grownp, int *ngrownp)
{
	Relation	index = vs->index;
	Buffer		entrybuf = ref->buf;
	OffsetNumber entryoff = ref->off;
	LionEntryTuple *ecopy;
	Size		esize;
	GenericXLogState *xstate;
	Page		p;
	uint32	   *grown;
	int			ngrown = 0;
	int			i;
	uint64		removed = w->delremoved;

	Assert(w->nwork > 0 || w->ndel > 0);

	vs->prof.pages_changed++;
	vs->prof.records++;
	vs->prof.items_rewritten += w->nwork;
	vs->prof.items_deleted += w->ndel;

	/* The entry may have been changed by inserts since the last window. */
	ecopy = lion_vacuum_entry_copy(entrybuf, entryoff, &esize);
	grown = (uint32 *) palloc(sizeof(uint32) * (w->nwork + 1));

	xstate = GenericXLogStart(index);
	p = GenericXLogRegisterBuffer(xstate, buf, 0);

	/*
	 * Empty containers go first, because the space they free may be what a
	 * container that grew needs.  PageIndexMultiDelete compacts the page
	 * itself (no PageRepairFragmentation needed) and renumbers the items
	 * after the deleted ones, so the offsets of what is left have to be
	 * adjusted.
	 */
	if (w->ndel > 0)
	{
		PageIndexMultiDelete(p, w->delofs, w->ndel);

		for (i = 0; i < w->nwork; i++)
		{
			int			shift = 0;
			int			j;

			for (j = 0; j < w->ndel; j++)
			{
				if (w->delofs[j] < w->work[i].off)
					shift++;
			}
			w->work[i].off -= shift;
		}
	}

	for (i = 0; i < w->nwork; i++)
	{
		/*
		 * After a PageIndexMultiDelete() the page has been compacted, so
		 * keeping the item's old allotment buys nothing any more and only
		 * wastes space; write it at its real size then.
		 */
		Size		writesz = (w->ndel > 0) ? w->work[i].size : w->work[i].writesz;

		if (PageIndexTupleOverwrite(p, w->work[i].off, w->work[i].c, writesz))
			removed += w->work[i].removed;
		else if (w->work[i].c->type == LION_CT_SPARSE)
		{
			/* A segment only ever shrinks, so its slot always holds it. */
			GenericXLogAbort(xstate);
			elog(ERROR, "lion index: filtered sparse segment %u on block %u no longer fits",
				 w->work[i].c->ckey, BufferGetBlockNumber(buf));
		}
		else
			grown[ngrown++] = w->work[i].c->ckey;
	}

	lion_page_update_minmax(p);

	Assert(ecopy->ntids >= removed);
	ecopy->ntids -= removed;
	Assert(ecopy->ncontainers >= (uint32) w->ndel);
	ecopy->ncontainers -= (uint32) w->ndel;

	if (!lion_replace_entry(index, xstate, entrybuf, entryoff, ecopy, esize))
		elog(ERROR, "lion index: could not update entry %u on block %u",
			 entryoff, BufferGetBlockNumber(entrybuf));

	GenericXLogFinish(xstate);

	vs->stats->tuples_removed += (double) removed;

	*grownp = grown;
	*ngrownp = ngrown;
}

/*
 * Re-place the container with this ckey, which no longer fits the slot it had
 * on page startblk.  Nothing is held on entry or on return (ref keeps its pin
 * on the leaf, which it owns for the length of pass 2).
 *
 * The page the container lives on now is found by walking right from startblk
 * under a cleanup lock on each page, never by jumping to it: a concurrent
 * insert (or the re-placement of an earlier container of the same page) may
 * have split a page and moved this container right, and cleaning it on the
 * new page without first holding the cleanup lock of every page before it
 * would let VACUUM slip past a reader that is holding a pin and a stale copy
 * of this very container (DESIGN.md section 11).
 *
 * The container is re-read and re-filtered under the locks, because an insert
 * may have added a TID to it since it was last read.
 */
static void
lion_vacuum_regrow(LionVacState *vs, LionVacEntryRef *ref,
				  const LionVacEntry *ent, BlockNumber startblk, uint32 ckey)
{
	Relation	index = vs->index;
	Buffer		entrybuf = InvalidBuffer;
	OffsetNumber entryoff = InvalidOffsetNumber;
	BlockNumber blk = startblk;

	for (;;)
	{
		LionEntryTuple *ecopy;
		Size		esize;
		Buffer		buf;
		Page		page;
		ItemId		iid;
		OffsetNumber off;
		BlockNumber next;
		bool		found;
		uint32		nremoved;
		LionVacPred	pred;

		buf = ReadBuffer(index, blk);

		/* Rule 1: a cleanup lock on every page on the way, in chain order. */
		LockBufferForCleanup(buf);
		lion_vac_visit(vs, blk);
		page = BufferGetPage(buf);
		if (!lion_page_owns_entry(page, ent->hash, ent->head))
		{
			UnlockReleaseBuffer(buf);
			elog(ERROR, "lion index: block %u is not a container page of the chain at %u",
				 blk, ent->head);
		}

		/* A root pushed down under us; start again at the leftmost leaf. */
		if (!LionPageIsPostingLeaf(page))
		{
			UnlockReleaseBuffer(buf);
			blk = lion_posting_leftmost_leaf(index, ent->hash, ent->head);
			if (!BlockNumberIsValid(blk))
				elog(ERROR, "lion index: container %u of a posting set at %u vanished from it",
					 ckey, ent->head);
			CHECK_FOR_INTERRUPTS();
			continue;
		}

		off = lion_page_find_container(page, ckey, &found);
		next = LionPageGetOpaque(page)->rightlink;

		if (!found)
		{
			/*
			 * A split moved the container to the right.  Containers are only
			 * ever removed by this vacuum, so it is still in the chain.
			 */
			UnlockReleaseBuffer(buf);
			if (!BlockNumberIsValid(next))
				elog(ERROR, "lion index: container %u of a chain at %u vanished from it",
					 ckey, ent->head);
			blk = next;
			CHECK_FOR_INTERRUPTS();
			continue;
		}

		/* Changing it needs the entry page as well, and never a wait here. */
		entrybuf = ref->buf;
		if (!ConditionalLockBuffer(entrybuf))
		{
			/*
			 * An insert holds the leaf.  Let go of the container page, take
			 * the leaf with nothing else held so that somebody makes progress,
			 * and try this page again.
			 */
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			ReleaseBuffer(buf);
			lion_vac_ref_lock(vs, ref, ent, BUFFER_LOCK_EXCLUSIVE);
			LockBuffer(ref->buf, BUFFER_LOCK_UNLOCK);
			CHECK_FOR_INTERRUPTS();
			continue;			/* start again at this same page */
		}
		if (!lion_vac_ref_valid(ref, ent))
		{
			/* A split took the entry to another leaf; find it again. */
			LockBuffer(entrybuf, BUFFER_LOCK_UNLOCK);
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			ReleaseBuffer(buf);
			lion_vac_ref_relocate(vs, ref, ent);
			CHECK_FOR_INTERRUPTS();
			continue;
		}
		entryoff = ref->off;

		/* Everything read from here on is used inside this window only. */
		ecopy = lion_vacuum_entry_copy(entrybuf, entryoff, &esize);

		iid = PageGetItemId(page, off);
		if (ItemIdGetLength(iid) > LION_CONTAINER_MAX_SIZE)
			elog(ERROR, "lion index: container of %zu bytes at %u/%u",
				 (Size) ItemIdGetLength(iid), blk, off);
		memcpy(vs->cbuf, PageGetItem(page, iid), ItemIdGetLength(iid));

		/*
		 * Only containers can grow while being filtered, and a container's
		 * ckey is never the first ckey of a segment (one ckey lives in one
		 * item), so the item found here must be the container itself.
		 */
		if (vs->cbuf->type == LION_CT_SPARSE)
		{
			LockBuffer(entrybuf, BUFFER_LOCK_UNLOCK);
			UnlockReleaseBuffer(buf);
			elog(ERROR, "lion index: container key %u of entry %u is now a sparse segment",
				 ckey, entryoff);
		}

		pred.ckey = ckey;
		pred.callback = vs->callback;
		pred.callback_state = vs->callback_state;
		nremoved = lion_container_remove_if(vs->cbuf, lion_vac_is_dead, &pred);

		if (nremoved > 0)
		{
			Assert(ecopy->ntids >= nremoved);
			ecopy->ntids -= nremoved;

			if (vs->cbuf->cardinality == 0)
			{
				GenericXLogState *xstate = GenericXLogStart(index);
				Page		p = GenericXLogRegisterBuffer(xstate, buf, 0);
				OffsetNumber delof = off;

				PageIndexMultiDelete(p, &delof, 1);
				lion_page_update_minmax(p);

				Assert(ecopy->ncontainers >= 1);
				ecopy->ncontainers -= 1;

				if (!lion_replace_entry(index, xstate, entrybuf, entryoff,
									   ecopy, esize))
					elog(ERROR, "lion index: could not update entry %u on block %u",
						 entryoff, BufferGetBlockNumber(entrybuf));

				GenericXLogFinish(xstate);
			}
			else
			{
				int			delta;

				lion_container_optimize(vs->cbuf);

				/*
				 * This writes the container and the entry in one record, and
				 * splits the page if the container still does not fit.
				 */
				lion_chain_put_container_locked(index, vs->heaprel, buf,
											   entrybuf, entryoff,
											   ecopy, vs->cbuf, &delta);
				if (delta != 0)
					elog(ERROR, "lion index: container %u of a chain at %u vanished from it",
						 ckey, ent->head);
			}

			vs->stats->tuples_removed += (double) nremoved;
		}

		LockBuffer(entrybuf, BUFFER_LOCK_UNLOCK);
		UnlockReleaseBuffer(buf);
		pfree(ecopy);
		return;
	}
}
