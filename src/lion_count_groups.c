/*-------------------------------------------------------------------------
 *
 * lion_count_groups.c
 *		The groups of a walk, counted together (DESIGN.md §10).
 *
 * Part of the count engine: lion_count.h is its interface, and
 * lion_count_int.h declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_count_int.h"

/* ---------------------------------------------------------------------
 * The groups of a walk, counted together (DESIGN.md §10)
 * --------------------------------------------------------------------- */

/*
 * How many groups one lion_count_groups_copy() takes at most.  Each holds a
 * cursor - a page image for a CHAIN set - and a pin on the posting leaf it
 * stands on, so a batch takes what the cursors of one count may hold open
 * (LionOpenBudget, DESIGN.md §15 "Bounded cursors"): work_mem of cursors, and
 * the pins the lists have left.  Never fewer than LION_OPEN_MIN_PINS while
 * the pins allow that many, and one group at the least - a pool too small
 * for its connections allows none (2026-09-29 review) - nor more than
 * LION_GROUP_BATCH_MAX (lion_count.h): past a few hundred groups a batch
 * saves nothing more, each reading of the copy being shared by that many
 * already.
 */

/*
 * The members of the copy's container at a key, times the groups that stand
 * there, from which the grouped walk makes that container a bitset image to
 * test the groups against; below it each group ANDs its container with the
 * copy's directly.  An image costs a clear and a count of 4 kB, 0.3 to 0.4 us
 * shared by the groups, and a test against it a nanosecond a member of a
 * group's container; the direct AND looks the copy's few members up in the
 * group's container, or merges the two.  A WHERE of one or two rows a key
 * against twenty dense groups is the case below it; a hundred groups against
 * a WHERE of twenty rows a key, above.
 */
#define LION_GROUP_IMAGE_MIN	128

int
lion_count_groups_batch(Relation index)
{
	LionOpenBudget budget;
	Size		per = sizeof(LionSetCursor) + sizeof(LionCountCtx) +
		sizeof(PGAlignedBlock) + 4 * sizeof(int64);
	Size		n;

	lion_open_budget_init(&budget, index);
	n = Min(budget.mem / per, (Size) budget.pins);
	n = Min(n, (Size) LION_GROUP_BATCH_MAX);
	n = Max(n, (Size) Min(budget.pins, LION_OPEN_MIN_PINS));
	return (int) Max(n, (Size) 1);
}

/*
 * THE GROUPS OF A WALK, COUNTED TOGETHER (DESIGN.md §10, "The groups of a
 * walk, counted together").  counts[g] = the rows of groups[g] that the
 * collected copy `copy` (lion_sources_collect()) holds, visible to snapshot,
 * for each of ngroups located posting sets - what a count of each against the
 * copy (lion_run_merge_copy()) answers, made in ONE walk of container keys
 * for all of them.
 *
 * Counted one at a time, each group's containers looked the copy up at their
 * keys and were ANDed with the copy's container there: every container of
 * the copy was read, sought and merged again by every group that had a
 * container at its key - a GROUP BY of many groups over every page of the
 * heap read its collected WHERE once per group, each time an AND of a
 * group's few members with a dense ARRAY by galloping search.  Here the groups' cursors
 * stand on a heap ordered by container key, and at each key of the copy that
 * a group has a container at, the copy's container is set in a bitset image
 * ONCE and every group standing there is tested against it: a bit test per
 * member of a group's ARRAY, which gives the intersection's count and block
 * mask without building it (lion_container_and_image_count()).  The map is
 * asked once per key too, about every block the copy's container has a
 * member on - every group's intersection there lies on those.
 *
 * WHY IT IS EXACT, AND DESIGN.md §9.  Each group's set is located afresh by
 * the caller and read by a cursor of its own, which pins the page each of its
 * containers came from - the pinned source each of its counts had - and the
 * copy is the same pinless copy those counts were ANDed with, on the same
 * argument (lion_sources_collect()).  At a key, the map is asked after every
 * group standing there has copied its container under its pin, and before
 * any of them moves past it: lion_count_container()'s order, for all of them
 * at once.  A group whose set carries no pin of its own - located past the
 * pin budget, or a private copy - trusts no map (novm, per group), as its
 * count alone would; so does every group while a row filter applies
 * (DESIGN.md §17), whose rows all go to the heap.  A key the copy has nothing
 * at, and a group whose intersection there is empty, ask the map nothing, and
 * the groups sought past such keys let go of what they held there: no answer
 * rests on it.  Each group keeps its own recheck queue, as its count did, and
 * they share the visibility cache as the counts did.
 *
 * `images`, when not NULL, is ngroups page images of the caller's that the
 * groups' cursors copy their leaves to, so that a caller walking batch after
 * batch - a parallel GROUP BY's ranges above all - does not allocate and free
 * a page for every group every time.
 */
void
lion_count_groups_copy(Relation heap, Snapshot snapshot, int ngroups,
					   LionPostingSet *groups, const LionPostingSet *copy,
					   int64 *counts, LionCountStats *stats,
					   LionVisCache *cache, bool rel_read_only,
					   PGAlignedBlock *images)
{
	const LionMatSet *mat;
	MemoryContext cxt;
	MemoryContext oldcxt;
	LionCountCtx base;
	LionCountCtx *gcx;
	LionSetCursor *cur;
	bool	   *trust;
	LionGroupEnt *gheap;
	int		   *hot;
	int			nheap = 0;
	LionContainer *img;
	LionContainer *work;
	LionContainer *buf = NULL;
	int			batchmax;
	int			idx = 0;
	int			g;

	for (g = 0; g < ngroups; g++)
		counts[g] = 0;
	if (ngroups == 0 || !copy->found || copy->mat == NULL ||
		copy->mat->ncontainers == 0)
		return;
	mat = copy->mat;
	Assert(mat->collected);

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"lion index grouped walk",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	/* what lion_count_sources_run() sets up for one count, once for all */
	memset(&base, 0, sizeof(base));
	base.cxt = cxt;
	base.heap = heap;
	base.snapshot = snapshot;
	base.vmbuf = InvalidBuffer;
	if (cache != NULL && BufferIsValid(cache->vmbuf))
	{
		if (cache->vmrelid == RelationGetRelid(heap))
		{
			base.vmbuf = cache->vmbuf;
			cache->vmbuf = InvalidBuffer;
			cache->vmrelid = InvalidOid;
		}
		else
			lion_vis_cache_release_vm(cache);
	}
	base.serializable = IsolationIsSerializable();
	/* a copy is never made in recovery; were it, nothing would trust the map */
	base.in_recovery = RecoveryInProgress();
	base.rel_read_only = rel_read_only;
	base.raw = true;
	base.tids_sorted = true;

	/*
	 * The groups share the cache as their counts did, one after the other:
	 * each is a count begun, which is what lets the cache resolve a dirty
	 * block on its second visit rather than only record it.
	 */
	for (g = 0; g < ngroups; g++)
		lion_vis_cache_begin(cache, heap, snapshot);
	base.cache = cache;
	base.filter = (cache != NULL) ? cache->filter : NULL;
	if (base.filter != NULL && base.filter->heap != heap)
		elog(ERROR, "lion index count: a row filter for another relation");

	/* each group's recheck queue takes its share of one count's */
	batchmax = Max(lion_recheck_budget() / ngroups, LION_RECHECK_MIN_BATCH);

	gcx = (LionCountCtx *) palloc(sizeof(LionCountCtx) * ngroups);
	cur = (LionSetCursor *) palloc(sizeof(LionSetCursor) * ngroups);
	trust = (bool *) palloc(sizeof(bool) * ngroups);
	gheap = (LionGroupEnt *) palloc(sizeof(LionGroupEnt) * ngroups);
	hot = (int *) palloc(sizeof(int) * ngroups);
	img = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	work = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	if (lion_mat_spills(mat))
		buf = (LionContainer *) palloc(MAXALIGN(LION_CONTAINER_MAX_SIZE));

	for (g = 0; g < ngroups; g++)
	{
		LionPostingSet *ps = &groups[g];

		gcx[g] = base;
		gcx[g].vmbuf = InvalidBuffer;
		gcx[g].batchmax = batchmax;
		gcx[g].keyset = ps;
		trust[g] = ps->found && !ps->nopin && ps->mat == NULL;
		gcx[g].novm = !trust[g];

		/* nothing below the copy's first key can be counted: begin there */
		lion_cursor_init_at(&cur[g], ps, &gcx[g], false, lion_mat_key(mat, 0),
							images != NULL ? &images[g] : NULL);
		if (cur[g].valid)
			lion_group_heap_push(gheap, &nheap, cur[g].cur->ckey, g);
	}

	while (nheap > 0)
	{
		uint32		key = gheap[0].ckey;
		const LionContainer *w;
		const LionContainer *wc;
		uint64		wanted;
		uint64		allvis;
		int			nhot = 0;
		int			i;

		/* the copy at or past the smallest key a group stands at */
		base.stats.copy_seeks++;
		idx = lion_mat_seek(mat, idx, key);
		if (idx >= mat->ncontainers)
			break;
		if (lion_mat_key(mat, idx) != key)
		{
			uint32		target = lion_mat_key(mat, idx);

			/*
			 * The copy has nothing below target: every group below it is
			 * sought there (DESIGN.md §22), and nothing of the keys it passes
			 * is counted.
			 */
			while (nheap > 0 && gheap[0].ckey < target)
			{
				LionGroupEnt e = lion_group_heap_pop(gheap, &nheap);

				lion_cursor_seek(&cur[e.g], target);
				if (cur[e.g].valid)
					lion_group_heap_push(gheap, &nheap, cur[e.g].cur->ckey,
										 e.g);
			}
			CHECK_FOR_INTERRUPTS();
			continue;
		}

		/* every group standing at the key, its container read and pinned */
		while (nheap > 0 && gheap[0].ckey == key)
			hot[nhot++] = lion_group_heap_pop(gheap, &nheap).g;

		/*
		 * The copy's container there, once, as a bitset image - unless it is
		 * a few members, which each group's AND looks up in the group's
		 * container instead (lion_container_and_raw()): a sparse WHERE
		 * against dense groups would otherwise test every member of every
		 * group's container against an image of one or two.
		 */
		w = lion_mat_container(&base, mat, idx, buf);
		if (w->type == LION_CT_BITSET)
			wc = w;
		else if (lion_container_cardinality(w) * (uint32) nhot <
				 LION_GROUP_IMAGE_MIN)
			wc = NULL;
		else
		{
			lion_container_bitset_init(img, key);
			lion_container_or_into_bitset(w, LION_BITSET_DATA(img));
			(void) lion_container_bitset_recount(img);
			wc = img;
		}

		/*
		 * The map, once for every group here: every block any of their
		 * intersections has a member on is one of the copy's container's.
		 * The test hook of lion_count_container_vm(): the groups' containers
		 * copied, their pages pinned, the map not asked yet.
		 */
		LION_INJECTION_POINT("lion-count-containers-pinned");
		wanted = lion_container_block_mask(w);
		if (base.in_recovery || base.filter != NULL)
			allvis = 0;
		else
		{
			allvis = lion_vm_allvisible_mask(heap, lion_ckey_first_block(key),
											 wanted, &base.vmbuf,
											 &base.stats.vm_pins);
			base.stats.vm_checks++;
#ifdef LION_VM_MASK_CHECK
			lion_vm_mask_check(heap, lion_ckey_first_block(key), wanted,
							   allvis, &base.vmbuf);
#endif
		}

		for (i = 0; i < nhot; i++)
		{
			LionCountCtx *cx = &gcx[hot[i]];
			const LionContainer *c = cur[hot[i]].cur;
			uint64		av = trust[hot[i]] ? allvis : 0;
			uint64		blocks;
			uint32		n;
			bool		built = false;

			if (wc != NULL && c->type == LION_CT_ARRAY)
				n = lion_container_and_image_count(c, LION_BITSET_DATA(wc),
												   &blocks);
			else
			{
				n = lion_container_and_raw(c, wc != NULL ? wc : w, work);
				blocks = lion_container_block_mask(work);
				built = true;
			}
			if (n == 0)
				continue;

			if ((blocks & ~av) == 0 && !cx->serializable)
			{
				/* every row of it on an all-visible block: counted */
				cx->count += n;
				cx->stats.blocks_skipped_via_vm += pg_popcount64(blocks);
				continue;
			}

			/* the heap has to see some of it, or a predicate lock be taken */
			if (!built)
				(void) lion_container_and_raw(c, wc, work);
			lion_count_container_masks(cx, work, blocks, av);
		}

		/*
		 * Only now may the groups here let go of their pages (§9), and each
		 * goes straight to the copy's next key, the next one a count can be
		 * made at: a step where that is the next key, a seek past the keys
		 * between otherwise (DESIGN.md §22).  Past the copy's last key there
		 * is nothing left to count.
		 */
		if (idx + 1 >= mat->ncontainers)
			break;
		{
			uint32		next = lion_mat_key(mat, idx + 1);

			for (i = 0; i < nhot; i++)
			{
				int			hg = hot[i];

				if (next == key + 1)
					lion_cursor_next(&cur[hg]);
				else
					lion_cursor_seek(&cur[hg], next);
				if (cur[hg].valid)
					lion_group_heap_push(gheap, &nheap, cur[hg].cur->ckey, hg);
			}
		}
		CHECK_FOR_INTERRUPTS();
	}

	for (g = 0; g < ngroups; g++)
		lion_cursor_close(&cur[g]);

	/* no index page is pinned any more: the heap answers the rest */
	for (g = 0; g < ngroups; g++)
	{
		lion_recheck_flush(&gcx[g]);
		counts[g] = gcx[g].count + gcx[g].recheck_count;
		if (BufferIsValid(gcx[g].vmbuf))
			ReleaseBuffer(gcx[g].vmbuf);
		if (stats != NULL)
			lion_count_stats_add(stats, &gcx[g].stats);
	}
	if (stats != NULL)
		lion_count_stats_add(stats, &base.stats);

	if (BufferIsValid(base.vmbuf))
	{
		if (cache != NULL && !BufferIsValid(cache->vmbuf))
		{
			cache->vmbuf = base.vmbuf;
			cache->vmrelid = RelationGetRelid(heap);
		}
		else
			ReleaseBuffer(base.vmbuf);
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);
}
