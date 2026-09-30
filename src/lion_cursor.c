/*-------------------------------------------------------------------------
 *
 * lion_cursor.c
 *		Container cursors: one located posting set, a container at a time.
 *
 * Part of the count engine: lion_count.h is its interface, and
 * lion_count_int.h declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_count_int.h"

static void lion_inline_skip(const char *payload, Size paylen, Size *off,
							 uint32 target, LionContainer *buf);

/* ---------------------------------------------------------------------
 * Container cursors
 * --------------------------------------------------------------------- */

/*
 * Release the pin the cursor owns, if any.  The only two callers are
 * lion_cursor_next() (page exhausted) and lion_cursor_close().
 */
static void
lion_cursor_unpin(LionSetCursor *cur)
{
	if (cur->ownpin && BufferIsValid(cur->pinbuf))
		ReleaseBuffer(cur->pinbuf);
	cur->pinbuf = InvalidBuffer;
	cur->ownpin = false;
	cur->haspage = false;
}



/*
 * A cursor over `set`, standing at its first container whose key is at or
 * above `target` - lion_cursor_seek() from nothing in hand: a CHAIN set's
 * first leaf comes from a descent for that key rather than for key 0 and the
 * leaves between.  Nothing below the target is read, so nothing of it is
 * counted or pinned (§9 asks nothing of it).  `image` is where a CHAIN set's
 * leaves are copied to, the caller's to keep - a walk that sets up many
 * cursors again and again gives each the same one every time - or NULL for
 * one of the cursor's own.
 */
void
lion_cursor_init_at(LionSetCursor *cur, const LionPostingSet *set,
					LionCountCtx *cx, bool droppins, uint32 target,
					PGAlignedBlock *image)
{
	memset(cur, 0, sizeof(LionSetCursor));
	cur->set = set;
	cur->cx = cx;
	cur->pinbuf = InvalidBuffer;
	cur->nextblk = InvalidBlockNumber;
	cur->droppins = droppins || cx->droppins;
	cur->mintarget = target;

	if (!set->found)
		return;

	if (set->mat != NULL)
	{
		/* a private copy: nothing to pin, nothing to walk */
		cur->matidx = 0;
		if (target > 0)
		{
			cx->stats.copy_seeks++;
			cur->matidx = lion_mat_seek(set->mat, 0, target);
		}

		/* ... and when it is spilled, a container at a time is read back */
		if (lion_mat_spills(set->mat))
			cur->cbuf = (LionContainer *)
				palloc(MAXALIGN(LION_CONTAINER_MAX_SIZE));
	}
	else if (set->is_inline)
	{
		cur->cbuf = (LionContainer *) palloc(lion_inline_stage_size(set->paylen));
		cur->payoff = 0;
		if (target > 0)
			lion_inline_skip(set->payload, set->paylen, &cur->payoff, target,
							 cur->cbuf);
		/* borrowed, not owned: the LionPostingSet releases it */
		cur->pinbuf = set->pinbuf;
		cur->ownpin = false;
	}
	else
	{
		/*
		 * The entry's head block is the ROOT of the posting tree (DESIGN.md
		 * §22), so the first leaf comes from a descent for the first key
		 * wanted - which is also how a root push-down cannot be raced: the
		 * descent hands back a page that WAS a leaf under the lock it read it
		 * with.
		 */
		cur->imgbuf = (image != NULL) ? image :
			(PGAlignedBlock *) palloc(sizeof(PGAlignedBlock));
		cur->img = (Page) cur->imgbuf->data;
		cur->nextblk = InvalidBlockNumber;
		cur->descend = true;
		cur->seekckey = target;

		/*
		 * Test hook: the entry has been copied and its leaf released, and
		 * nothing of the posting set is pinned yet - all this cursor holds is
		 * the root's block number.  test/isolation/vacuum_regrow_pushdown.spec
		 * sends the descent into a VACUUM that is in the middle of pushing
		 * that root down.  Compiles to nothing without injection points.
		 */
		LION_INJECTION_POINT("lion-count-chain-entered");
	}

	lion_cursor_next(cur);
}

/* A cursor over `set`, standing at its first container. */
void
lion_cursor_init(LionSetCursor *cur, const LionPostingSet *set, LionCountCtx *cx,
				 bool droppins)
{
	lion_cursor_init_at(cur, set, cx, droppins, 0, NULL);
}

/*
 * Take over a leaf the caller holds SHARE-locked: copy the page - items and
 * rightlink together, as lion_scan.c does - then drop the content lock but
 * keep the pin (DESIGN.md §9).
 */
static void
lion_cursor_take_page(LionSetCursor *cur, Buffer buf)
{
	memcpy(cur->img, BufferGetPage(buf), BLCKSZ);
	cur->imgblk = BufferGetBlockNumber(buf);
	cur->haspage = true;

	/*
	 * A caller that needs no interlock (DESIGN.md §29.5: a plain index scan
	 * under an MVCC snapshot, which visits the heap for every TID) keeps the
	 * image and nothing else.  Everything the cursor does next - the items,
	 * the right link, maxckey for a seek - is read from the image, so the pin
	 * was only ever the §9 interlock, and a pin held across a paused scan
	 * would make every VACUUM of the table wait for it.  The same holds for
	 * a cursor the expression above it has told to carry no interlock
	 * (cur->droppins, see LionSetCursor).
	 */
	if (cur->droppins)
	{
		UnlockReleaseBuffer(buf);
		cur->pinbuf = InvalidBuffer;
		cur->ownpin = false;
	}
	else
	{
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		cur->pinbuf = buf;
		cur->ownpin = true;
	}

	cur->nextblk = LionPageGetOpaque(cur->img)->rightlink;
	cur->off = FirstOffsetNumber;
	cur->maxoff = PageGetMaxOffsetNumber(cur->img);
}

/*
 * Advance to the next ITEM of the set: a container or a sparse segment, in
 * ascending ckey order, or NULL at the end.
 *
 * DESIGN.md section 9: for a CHAIN set this is the one place a source page
 * pin is dropped, and it happens only once the caller has finished with every
 * item of that page - see lion_count_container(), which calls
 * lion_cursor_next() immediately after the visibility-map checks and nowhere
 * else.
 */
static const LionContainer *
lion_cursor_next_item(LionSetCursor *cur)
{
	if (cur->set->mat != NULL)
	{
		const LionMatSet *mat = cur->set->mat;

		if (cur->matidx >= mat->ncontainers)
			return NULL;
		if (!lion_mat_inmem(mat, cur->matidx))
		{
			if (cur->matidx == 0 && mat->first != NULL)
			{
				cur->matidx++;
				return mat->first;
			}
			lion_spill_read(mat, cur->matidx++, cur->cbuf);
			cur->cx->stats.copy_file_reads++;
			return cur->cbuf;
		}
		return mat->containers[cur->matidx++];
	}

	if (cur->set->is_inline)
	{
		if (lion_inline_fetch(cur->set->payload, cur->set->paylen,
							 &cur->payoff, cur->cbuf) == 0)
			return NULL;		/* payload exhausted; the set keeps its pin */
		return cur->cbuf;
	}

	for (;;)
	{
		Buffer		buf;
		Page		page;

		/*
		 * Finish the page we already have in hand.  An item goes to the
		 * container code only once lion_page_item_fetch() has made sure it
		 * holds what its header says (DESIGN.md §3, "untrusted containers").
		 */
		if (cur->haspage && cur->off <= cur->maxoff)
		{
			OffsetNumber off = cur->off;

			cur->off = OffsetNumberNext(off);
			return lion_page_item_fetch(cur->set->index, cur->img, cur->imgblk,
										off);
		}

		/* Its items have all been consumed: the pin may go. */
		lion_cursor_unpin(cur);

		if (cur->descend)
		{
			bool		found;

			/*
			 * A descent for seekckey: the first leaf of the set, or the leaf a
			 * seek jumped to (DESIGN.md §22).  It comes back locked, so no
			 * root push-down and no split can slip in between.
			 */
			cur->descend = false;
			buf = lion_posting_search(cur->set->index, NULL, 0, cur->set->head,
									  cur->seekckey, BUFFER_LOCK_SHARE, false);
			if (!BufferIsValid(buf))
			{
				cur->nextblk = InvalidBlockNumber;
				return NULL;
			}
			lion_cursor_take_page(cur, buf);
			cur->off = lion_page_find_item(cur->set->index, cur->img,
										   cur->imgblk, cur->seekckey, &found);
			CHECK_FOR_INTERRUPTS();
			continue;
		}

		if (!BlockNumberIsValid(cur->nextblk))
			return NULL;

		lion_posting_pages_read++;
		buf = ReadBuffer(cur->set->index, cur->nextblk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);

		/*
		 * DESIGN.md §18: every page reached through an entry's head or a
		 * rightlink has to still claim that posting set.  A root block is the
		 * one block this index never recycles, so owner_head identifies the
		 * set for the whole life of the index and a mismatch can only mean
		 * that the set was freed after it was located - which VACUUM does only
		 * once the set held nothing visible to anyone.  The cursor then simply
		 * ends here and contributes nothing further; it is never an error
		 * outside verify().  This is also what keeps a standby reader safe,
		 * where replay can reuse a page under a held pin because generic WAL
		 * cannot raise a recovery conflict.
		 *
		 * A leaf's rightlink always names another leaf, so a page above level
		 * zero is the same kind of accident and ends the walk the same way.
		 */
		if (!lion_page_owns(page, cur->set->head) ||
			!LionPageIsPostingLeaf(page))
		{
			UnlockReleaseBuffer(buf);
			cur->nextblk = InvalidBlockNumber;
			return NULL;
		}

		lion_cursor_take_page(cur, buf);

		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * The cursor stands at a container it has read: what EXPLAIN ANALYZE counts
 * of it (LionCountStats) - every container, and whether it came from the
 * count's own key set or from a private copy.
 */
static inline void
lion_cursor_counted(LionSetCursor *cur)
{
	LionCountCtx *cx = cur->cx;

	cx->stats.containers_visited++;
	if (cur->set->mat != NULL)
		cx->stats.copy_containers++;
	if (cur->set == cx->keyset)
		cx->stats.key_containers++;
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
lion_cursor_emit_segment(LionSetCursor *cur)
{
	const LionContainer *seg = cur->seg;
	const uint32 *ckeys = LION_SPARSE_CKEYS_CONST(seg);
	/* the pairs a reader may look at, and los[] where that many ckeys end */
	uint32		n = lion_sparse_npairs(seg);
	const uint16 *los = LION_SPARSE_LOS_CONST_AT(seg, n);
	uint32		ckey;
	uint32		end;
	Size		need;

	/*
	 * A seek may have asked for a container key inside this segment's range
	 * (DESIGN.md §22).  Its pairs are sorted, so the ones below the target are
	 * skipped here rather than presented and thrown away by the merge; when no
	 * seek has happened mintarget is 0 or already behind us and this is a
	 * no-op.
	 */
	while (cur->segpos < n && ckeys[cur->segpos] < cur->mintarget)
		cur->segpos++;

	if (cur->segpos >= n)
		return false;

	ckey = ckeys[cur->segpos];

	/*
	 * Allocated on first use, not per cursor: an IN list of a thousand values
	 * (DESIGN.md §15) is a thousand cursors, and most posting sets hold no
	 * sparse segment at all.  And sized for what one container key of a
	 * segment holds, fewer than LION_SPARSE_THRESHOLD members, rather than
	 * for the largest container: a sparse column's IN list used to cost a
	 * second 8 kB per value here.  A segment is not trusted to keep that
	 * promise, though - the buffer grows to whatever this key's pairs need,
	 * which an ARRAY container of up to LION_ARRAY_MAX_CARD members and
	 * LION_CONTAINER_MAX_SIZE beyond that (lion_container_append_sorted()
	 * turns it into a BITSET) always covers.
	 */
	for (end = cur->segpos; end < n && ckeys[end] == ckey; end++)
		;
	if (end - cur->segpos > LION_ARRAY_MAX_CARD)
		need = LION_CONTAINER_MAX_SIZE;
	else
		need = LION_CONTAINER_HDRSZ +
			Max(end - cur->segpos, LION_SPARSE_THRESHOLD) * sizeof(uint16);
	if (cur->segbuf == NULL || cur->segcap < need)
	{
		/* (the old one, if any, goes with the context: only a malformed
		 * segment can get here twice) */
		cur->segcap = MAXALIGN(need);
		cur->segbuf = (LionContainer *) palloc(cur->segcap);
	}

	/*
	 * A segment's pairs are data (DESIGN.md §3): a lo is masked into range
	 * before the builder sees it, and the builder copes with pairs that are
	 * out of order or repeated.
	 */
	lion_container_init(cur->segbuf, ckey);
	do
	{
		lion_container_append_sorted(cur->segbuf,
									 (uint16) (los[cur->segpos] & LION_LO_MASK));
		cur->segpos++;
	} while (cur->segpos < n && ckeys[cur->segpos] == ckey);

	cur->cur = cur->segbuf;
	cur->valid = true;
	lion_cursor_counted(cur);
	return true;
}

/* Pull items until one of them yields a container. */
static void
lion_cursor_advance(LionSetCursor *cur)
{
	for (;;)
	{
		const LionContainer *item = lion_cursor_next_item(cur);

		if (item == NULL)
			return;

		if (item->type != LION_CT_SPARSE)
		{
			cur->cur = item;
			cur->valid = true;
			lion_cursor_counted(cur);
			return;
		}

		cur->seg = item;
		cur->segpos = 0;
		if (lion_cursor_emit_segment(cur))
			return;
		cur->seg = NULL;		/* an empty segment: nothing to present */
	}
}

/*
 * Advance to the next container of the set, expanding sparse segments one
 * container key at a time (see the comment on LionSetCursor).
 */
void
lion_cursor_next(LionSetCursor *cur)
{
	cur->valid = false;
	cur->cur = NULL;

	if (cur->set == NULL || !cur->set->found)
		return;

	/* Still inside a segment?  Its next container key is the next container. */
	if (cur->seg != NULL)
	{
		if (lion_cursor_emit_segment(cur))
			return;
		cur->seg = NULL;
	}

	lion_cursor_advance(cur);
}

/*
 * Skip a packed INLINE payload forward to the first item that can hold
 * `target`, leaving *off in front of it.
 *
 * DEVIATION from DESIGN.md §22, which said an INLINE set would seek by binary
 * search: an inline payload is a sequence of items packed without padding and
 * carries no offsets to search, so this walks the item headers instead.  It
 * costs nothing over the sequential walk it replaces - one forward pass over
 * the payload either way - and an offset index built to allow a binary search
 * would have to make that very pass to build itself.
 */
static void
lion_inline_skip(const char *payload, Size paylen, Size *off, uint32 target,
				 LionContainer *buf)
{
	Size		prev = *off;

	while (lion_inline_fetch(payload, paylen, off, buf) > 0)
	{
		if (lion_item_last_ckey(buf) >= target)
		{
			*off = prev;		/* leave it for the walk to pick up */
			return;
		}
		prev = *off;
	}
}

/*
 * Position a CHAIN cursor's leaf on `target`: step right while the current
 * leaf cannot hold it and the walk is short, else descend (DESIGN.md §22).
 *
 * THE §9 PIN RULE.  This drops the pin on the leaf the cursor is standing on,
 * so it may only be called where lion_cursor_next() may: at a container key
 * whose container has already been through the visibility-map check, or whose
 * container carried no visibility-map obligation at all because nothing of it
 * reached the count.  All three callers are of the second kind, and all three
 * are the very places that used to call lion_ecursor_next() for the same
 * reason: lion_run_merge()'s `!alleq` branch, the AND node's wind-forward
 * loop, and the merge winding a NEGATED source up to the key it is about to
 * subtract at - everything such a source passes over is below that key and
 * contributes nothing to it.
 */
static void
lion_cursor_seek_leaf(LionSetCursor *cur, uint32 target)
{
	int			steps = 0;

	for (;;)
	{
		BlockNumber blk;
		Buffer		buf;
		Page		page;
		bool		found;

		if (!cur->haspage)
		{
			/* nothing in hand: let the next page come from a descent */
			cur->descend = true;
			cur->seekckey = target;
			return;
		}

		if (LionPageGetOpaque(cur->img)->maxckey >= target ||
			!BlockNumberIsValid(cur->nextblk))
		{
			/*
			 * The target is at or before the end of this page, or there is no
			 * page after it.  Either way this is where the walk resumes; an
			 * offset past the last item simply ends the cursor.
			 */
			cur->off = lion_page_find_item(cur->set->index, cur->img,
										   cur->imgblk, target, &found);
			return;
		}

		if (steps >= LION_POSTING_SEEK_STEPS || cur->cx->farseeks)
		{
			cur->descend = true;
			cur->seekckey = target;
			lion_cursor_unpin(cur);
			cur->off = OffsetNumberNext(cur->maxoff);
			return;
		}

		blk = cur->nextblk;
		lion_cursor_unpin(cur);
		cur->off = OffsetNumberNext(cur->maxoff);

		lion_posting_pages_read++;
		buf = ReadBuffer(cur->set->index, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (!lion_page_owns(page, cur->set->head) ||
			!LionPageIsPostingLeaf(page))
		{
			UnlockReleaseBuffer(buf);
			cur->nextblk = InvalidBlockNumber;
			return;
		}
		lion_cursor_take_page(cur, buf);
		steps++;

		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * Move the cursor to the first container whose key is at or above `target`
 * (DESIGN.md §22).  A no-op when it is already there or past it, and when the
 * set is exhausted.
 *
 * This is what turns an intersection from a stream into a set of probes: the
 * merge advances whichever source has the largest container key and seeks the
 * others to it, so the work tracks the most selective side instead of the sum
 * of all of them.  See lion_cursor_seek_leaf() for the §9 pin rule it obeys.
 */
void
lion_cursor_seek(LionSetCursor *cur, uint32 target)
{
	if (cur->set == NULL || !cur->set->found || !cur->valid)
		return;
	if (cur->cur->ckey >= target)
		return;

	cur->mintarget = target;
	cur->valid = false;
	cur->cur = NULL;

	/* Still inside a segment?  emit_segment() skips its pairs for us. */
	if (cur->seg != NULL)
	{
		if (lion_cursor_emit_segment(cur))
			return;
		cur->seg = NULL;
	}

	if (cur->set->mat != NULL)
	{
		/*
		 * A private copy is an array: its index answers where the target is
		 * (lion_mat_seek()) - or a binary search does, a spilled one by the
		 * keys it keeps in memory, one per container.
		 */
		cur->cx->stats.copy_seeks++;
		cur->matidx = lion_mat_seek(cur->set->mat, cur->matidx, target);
	}
	else if (cur->set->is_inline)
		lion_inline_skip(cur->set->payload, cur->set->paylen, &cur->payoff,
						 target, cur->cbuf);
	else
		lion_cursor_seek_leaf(cur, target);

	lion_cursor_advance(cur);
}

void
lion_cursor_close(LionSetCursor *cur)
{
	lion_cursor_unpin(cur);
	cur->seg = NULL;
	cur->valid = false;
	cur->cur = NULL;
}
