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
	lion_count_groups_copy_ext(heap, snapshot, ngroups, groups, copy, counts,
							   stats, cache, rel_read_only, images, NULL);
}

/*
 * A width's image of the copy's container at a key, for the plane kernel
 * (DESIGN.md §52, "The kernel"): the words of a bitmap of `width` words a
 * block (lion_container.h, "widths") whose members are those of the full
 * image `full` (a BITSET's LION_BITSET_WORDS words) at offsets below 64 *
 * width - the only ones a NARROW of that width has, so that its AND with
 * the result counts exactly what its AND with the full image counts.  The
 * full image itself at the full width; `out` holds LION_WIDTH_WORDS(width).
 */
static const uint64 *
lion_groups_width_image(const uint64 *full, uint32 width, uint64 *out)
{
	uint32		b;

	if (width == LION_BITSET_WIDTH)
		return full;
	for (b = 0; b < LION_BLOCKS_PER_CONTAINER; b++)
		memcpy(out + b * width, full + b * LION_BITSET_WIDTH,
			   sizeof(uint64) * width);
	return out;
}

/*
 * The groups standing at a key whose every block the map vouches for, in a
 * walk asked for the kernel (lion_count_groups_copy_ext(), DESIGN.md §52):
 * each counted without its intersection being built.  The BITSETs and
 * NARROWs of one width - where the copy's container has an image, wc - all
 * together, by one strided pass of lion_planes_count_strided() over their
 * payloads against the image at that width; with the kernel off each of
 * them, and every other kind always, by lion_container_and_cardinality(),
 * an ARRAY against the image by a bit test a member.  w is the copy's
 * container, wc its image or NULL, and `wanted` its blocks, every one of
 * them all-visible: a group's count lies on those blocks, which is what
 * blocks_skipped_via_vm takes for a group with a row there - no fewer than
 * its rows lie on.
 */
static void
lion_groups_count_clean(LionGroupsKernel *opts, int nhot, const int *hot,
						LionSetCursor *cur, LionCountCtx *gcx,
						const LionContainer *w, const LionContainer *wc,
						uint64 wanted)
{
	const uint64 **planes;
	int		   *who;
	int			nin[LION_BITSET_WIDTH + 1];
	int			first[LION_BITSET_WIDTH + 1];
	uint32	   *kcounts;
	uint64		nimg[LION_WIDTH_WORDS(LION_NARROW_MAX_WIDTH)];
	int			nblocks = pg_popcount64(wanted);
	bool		passed = false;
	uint32		width;
	int			at;
	int			i;

	planes = (const uint64 **) palloc(sizeof(uint64 *) * nhot);
	who = (int *) palloc(sizeof(int) * nhot);
	kcounts = (uint32 *) palloc(sizeof(uint32) * nhot);

	/* the kernel's groups by width, each width's together in planes[] */
	memset(nin, 0, sizeof(nin));
	for (i = 0; i < nhot; i++)
	{
		width = lion_container_width(cur[hot[i]].cur);
		if (opts->kernel && wc != NULL && width > 0 &&
			width <= LION_BITSET_WIDTH)
			nin[width]++;
	}
	at = 0;
	for (width = 0; width <= LION_BITSET_WIDTH; width++)
	{
		first[width] = at;
		at += nin[width];
		nin[width] = 0;
	}

	for (i = 0; i < nhot; i++)
	{
		int			g = hot[i];
		const LionContainer *c = cur[g].cur;
		uint32		n;
		uint64		blocks;

		opts->containers++;
		width = lion_container_width(c);
		if (opts->kernel && wc != NULL && width > 0 &&
			width <= LION_BITSET_WIDTH)
		{
			planes[first[width] + nin[width]] = LION_BITSET_DATA(c);
			who[first[width] + nin[width]] = g;
			nin[width]++;
			continue;
		}
		if (wc != NULL && c->type == LION_CT_ARRAY)
			n = lion_container_and_image_count(c, LION_BITSET_DATA(wc),
											   &blocks);
		else
			n = lion_container_and_cardinality(c, wc != NULL ? wc : w);
		if (n == 0)
			continue;
		gcx[g].count += n;
		gcx[g].stats.blocks_skipped_via_vm += nblocks;
	}

	for (width = 1; width <= LION_BITSET_WIDTH; width++)
	{
		const uint64 *img;
		int			j;

		if (nin[width] == 0)
			continue;
		if (w->type == LION_CT_NARROW && lion_container_width(w) == width)
			img = LION_BITSET_DATA(w);	/* the copy's own words */
		else
			img = lion_groups_width_image(LION_BITSET_DATA(wc), width, nimg);
		lion_planes_count_strided(img, &planes[first[width]],
								  LION_WIDTH_WORDS(width),
								  (uint32) nin[width], kcounts);
		opts->kernelplanes += nin[width];
		passed = true;
		for (j = 0; j < nin[width]; j++)
		{
			int			g = who[first[width] + j];

			if (kcounts[j] == 0)
				continue;
			gcx[g].count += kcounts[j];
			gcx[g].stats.blocks_skipped_via_vm += nblocks;
		}
	}
	if (passed)
		opts->kernelkeys++;
	pfree(planes);
	pfree(who);
	pfree(kcounts);
}

/*
 * lion_count_groups_copy(), and with opts not NULL the groups at a key whose
 * every block is all-visible counted without their intersections being
 * built - by the plane kernel where opts->kernel says so - and what it did
 * added to opts (lion_groups_count_clean(); DESIGN.md §52, "The kernel").
 * A bit-sliced column's slices are the groups it is for: some dozens of
 * BITSETs or NARROWs of one width at most keys of a dense column.
 */
void
lion_count_groups_copy_ext(Relation heap, Snapshot snapshot, int ngroups,
						   LionPostingSet *groups, const LionPostingSet *copy,
						   int64 *counts, LionCountStats *stats,
						   LionVisCache *cache, bool rel_read_only,
						   PGAlignedBlock *images, LionGroupsKernel *opts)
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
	LionRowFilter *gfilter;
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

	/*
	 * A window of keys (lion_vis_cache_set_window()) confines the counts of
	 * a parallel count alone, never this walk's: refused rather than
	 * counted past.
	 */
	if (cache != NULL && cache->windowed)
		elog(ERROR, "lion index count: a grouped walk confined to a window of keys");

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

	/* a copy decided from the positions as it was made decides nothing here */
	base.posdone = mat->posfiltered;

	/*
	 * Each group's recheck queue takes its share of one count's budget, and
	 * the shares add up to that budget.  The floor of one count's,
	 * LION_RECHECK_MIN_BATCH, is no floor of a share: 256 groups took 4096
	 * TIDs each, 6 MB of queues at the default work_mem of 4 MB (2026-10
	 * review).  A share holds a heap block's TIDs at the least, the fewest a
	 * flush takes anyway, since it waits for a block boundary.  That floor
	 * binds only where work_mem is below the open budget's floor
	 * (LION_OPEN_MIN_BYTES), which then sizes the batch: above it,
	 * lion_count_groups_batch() gives each group a page image out of
	 * work_mem, and a page image is more bytes than a block's TIDs.
	 */
	batchmax = Max(lion_recheck_budget() / ngroups, LION_RECHECK_BLOCK_TIDS);

	gcx = (LionCountCtx *) palloc(sizeof(LionCountCtx) * ngroups);
	cur = (LionSetCursor *) palloc(sizeof(LionSetCursor) * ngroups);
	trust = (bool *) palloc(sizeof(bool) * ngroups);
	gheap = (LionGroupEnt *) palloc(sizeof(LionGroupEnt) * ngroups);
	hot = (int *) palloc(sizeof(int) * ngroups);
	img = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	work = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	if (lion_mat_spills(mat))
		buf = (LionContainer *) palloc(MAXALIGN(LION_CONTAINER_MAX_SIZE));

	/*
	 * The copy's containers go through the position filters once a key,
	 * below, and so the groups' counts need only the heap's part of the row
	 * filter.
	 */
	gfilter = lion_row_filter_nopos(base.filter);

	for (g = 0; g < ngroups; g++)
	{
		LionPostingSet *ps = &groups[g];

		gcx[g] = base;
		gcx[g].filter = gfilter;
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
		int			ndone = 0;
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
		w = lion_count_posfilter(&base, lion_mat_container(&base, mat, idx, buf));
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
		if (base.in_recovery || lion_row_filter_heap(base.filter))
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

		/*
		 * Asked for the kernel (DESIGN.md §52), at a key whose every block
		 * is all-visible: every group here that trusts the map is counted
		 * without its intersection - all of them, or none and each below as
		 * always.  Not under SERIALIZABLE, whose blocks counted from the map
		 * take their predicate locks a block at a time below.
		 */
		if (opts != NULL && !base.serializable && (wanted & ~allvis) == 0)
		{
			for (i = 0; i < nhot; i++)
			{
				if (!trust[hot[i]])
					break;
			}
			if (i == nhot)
			{
				lion_groups_count_clean(opts, nhot, hot, cur, gcx, w, wc,
										wanted);
				ndone = nhot;
			}
		}

		for (i = ndone; i < nhot; i++)
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
			if (opts != NULL)
				opts->containers++;
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
