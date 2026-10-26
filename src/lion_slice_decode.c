/*-------------------------------------------------------------------------
 *
 * lion_slice_decode.c
 *		A sliced column's aggregates for a batch of groups, decoded key by
 *		key (DESIGN.md §52, "The decode route").
 *
 * Part of the count engine: lion_count.h is its interface, and
 * lion_count_int.h declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "access/xlog.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "storage/predicate.h"
#include "utils/memutils.h"

#include "lion_count_int.h"
#include "lion_slice_enc.h"

/*
 * The fewest rows of a 64-row word whose values are gathered by a transpose
 * of the word's 64 x nbits bits (lion_planes_gather_strided(), §50) rather
 * than read bit by bit: a transpose takes about what four rows read bit by
 * bit do, whatever the width (both grow with it).
 */
#define LION_SLICE_GATHER_MIN	4

/* What the walk keeps beside the recheck list: each TID's group and value. */
typedef struct LionSliceRecheck
{
	LionCountCtx *cx;			/* its tids, and the heap's answers */
	uint32	   *groups;
	uint64	   *values;
	bool	   *hasval;
	bool	   *vis;
	int			cap;
} LionSliceRecheck;

/* A visible row of group g with encoding enc, into the group's answers. */
static inline void
lion_slice_decode_add(LionSliceGroupsOut *out, uint32 g, uint64 enc)
{
	int64		n = ++out->count[g];

#ifdef HAVE_INT128
	if (out->sumenc != NULL)
		out->sumenc[g] += (int128) enc;
#endif
	if (out->minenc != NULL && (n == 1 || enc < out->minenc[g]))
		out->minenc[g] = enc;
	if (out->maxenc != NULL && (n == 1 || enc > out->maxenc[g]))
		out->maxenc[g] = enc;
}

/* The rows of the list the heap shows, into the answers; the list emptied. */
static void
lion_slice_recheck_flush(LionSliceRecheck *rc, LionSliceGroupsOut *out,
						 LionDecodeStats *dstats)
{
	LionCountCtx *cx = rc->cx;
	int			i;

	if (cx->ntids == 0)
		return;
	memset(rc->vis, 0, sizeof(bool) * cx->ntids);
	(void) lion_recheck_visible(cx, rc->vis);
	for (i = 0; i < cx->ntids; i++)
	{
		if (rc->vis[i])
		{
			if (out->allcount != NULL)
				out->allcount[rc->groups[i]]++;
			if (rc->hasval[i])
			{
				lion_slice_decode_add(out, rc->groups[i], rc->values[i]);
				dstats->rows++;
			}
		}
	}
	dstats->rechecked += cx->ntids;
	cx->ntids = 0;
}

#define LION_SLICE_RECHECK_ENTRY \
	(sizeof(ItemPointerData) + sizeof(uint32) + sizeof(uint64) + \
	 2 * sizeof(bool))

/*
 * One row for the heap to decide on, with its group and value - in TID
 * order, as the rows come key by key and in position order inside a key:
 * lion_decode_recheck_add()'s list, which the argument there covers.
 */
static void
lion_slice_recheck_add(LionSliceRecheck *rc, LionSliceGroupsOut *out,
					   LionDecodeStats *dstats, uint32 ckey, uint32 lo,
					   uint32 g, uint64 enc, bool hasval)
{
	LionCountCtx *cx = rc->cx;
	ItemPointerData tid;

	lion_code_to_tid(((uint64) ckey << LION_CONTAINER_BITS) | lo, &tid);
	if (cx->ntids > 0 &&
		cx->ntids + LION_RECHECK_BLOCK_TIDS > cx->batchmax &&
		ItemPointerGetBlockNumber(&cx->tids[cx->ntids - 1]) !=
		ItemPointerGetBlockNumber(&tid))
		lion_slice_recheck_flush(rc, out, dstats);

	if (cx->ntids >= rc->cap)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(cx->cxt);
		int			newcap = (rc->cap == 0) ? Min(1024, cx->batchmax) :
			Min(rc->cap * 2, cx->batchmax);

		if (newcap <= rc->cap)
			newcap = rc->cap + LION_RECHECK_BLOCK_TIDS;
		if (rc->cap == 0)
		{
			cx->tids = (ItemPointerData *) palloc(sizeof(ItemPointerData) * newcap);
			rc->groups = (uint32 *) palloc(sizeof(uint32) * newcap);
			rc->values = (uint64 *) palloc(sizeof(uint64) * newcap);
			rc->hasval = (bool *) palloc(sizeof(bool) * newcap);
			rc->vis = (bool *) palloc(sizeof(bool) * newcap);
		}
		else
		{
			cx->tids = (ItemPointerData *)
				repalloc(cx->tids, sizeof(ItemPointerData) * newcap);
			rc->groups = (uint32 *) repalloc(rc->groups, sizeof(uint32) * newcap);
			rc->values = (uint64 *) repalloc(rc->values, sizeof(uint64) * newcap);
			rc->hasval = (bool *) repalloc(rc->hasval, sizeof(bool) * newcap);
			rc->vis = (bool *) repalloc(rc->vis, sizeof(bool) * newcap);
		}
		rc->cap = newcap;
		cx->maxtids = newcap;
		MemoryContextSwitchTo(oldcxt);
	}
	Assert(cx->ntids == 0 ||
		   ItemPointerCompare(&cx->tids[cx->ntids - 1], &tid) < 0);
	cx->tids[cx->ntids] = tid;
	rc->groups[cx->ntids] = g;
	rc->values[cx->ntids] = enc;
	rc->hasval[cx->ntids] = hasval;
	cx->ntids++;
}

/*
 * A slice's container at the key, as the walk reads it: a bitmap of `width`
 * words a heap block (lion_container.h, "widths") - a BITSET's full width, a
 * NARROW's own, read in place - or an image of the key's positions the walk
 * made of an ARRAY or a RUN, or none.  Word i of the key's full layout, the
 * 64 positions from 64 i, is word b * width + s of it for block b = i /
 * LION_BITSET_WIDTH and s = i % LION_BITSET_WIDTH, when s < width; a NARROW
 * has no member past offset 64 * width of a block, and the word is 0.
 */
typedef struct LionSlicePlane
{
	const uint64 *data;
	uint32		width;
} LionSlicePlane;

static const uint64 lion_slice_zero_word = 0;

static inline const uint64 *
lion_slice_plane_word(const LionSlicePlane *pl, uint32 i)
{
	uint32		b = i / LION_BITSET_WIDTH;
	uint32		sub = i % LION_BITSET_WIDTH;

	if (sub >= pl->width)
		return &lion_slice_zero_word;
	return pl->data + (Size) b * pl->width + sub;
}

/*
 * A SLICED COLUMN'S AGGREGATES FOR A BATCH OF GROUPS, DECODED KEY BY KEY
 * (DESIGN.md §52, "The decode route").  For each of ngroups located sets of
 * one grouping column - the groups of a batch of the GROUP BY's walk - the
 * rows of the group that have a value, that `where` holds (the collected
 * WHERE, or NULL for every row) and that the snapshot sees, out->count[g],
 * and the sum of their values' encodings, their least and their greatest,
 * read off the column's slices: slices[i], i < nbits, the rows with bit i
 * of the encoding set (not found, no row has it), and slices[nbits] its
 * existence slice E, the rows with a value.
 *
 * Counted pair by pair, each group was a copy of its rows and nbits counts
 * against it, each over every key the group spans: a pass over the slices a
 * group, which past a few dozen groups is what the GROUP BY costs.  Here the
 * slices are read once for the batch: the walk is lion_count_groups_decode()'s
 * with one column, the groups, and at each key the WHERE leaves rows at, each
 * slice's container there made a plane of the key's 32768 positions, and
 * each row's encoding read off the planes - 64 rows at a time by
 * lion_planes_gather_strided() (§50) where a word holds enough of them, a
 * row's nbits bits one by one where it holds a few - and added to its
 * group's answers.
 *
 * WHY IT IS EXACT is the decoded walk's argument (lion_count_groups_decode()),
 * column 0 being the groups: every row counted is in exactly one group's set,
 * located with its pin (the interlock), the map asked at a key after every
 * group standing there has copied its container and before any moves past
 * it, and a row on a block the map does not vouch for goes to the heap with
 * its group and value (lion_recheck_visible()).  `where` and the slices are
 * read with no pin, and only label a row of a pinned set: a row the snapshot
 * sees with a value is in E and in exactly the slices of its value's bits,
 * and one with none in no slice at all, which its
 * insert wrote before its transaction committed and only VACUUM removes, for
 * rows no snapshot sees - and the slices are read after the snapshot was
 * taken.  A TID a slice lists for a row the snapshot does not see labels a row
 * the heap or the pinned group set rejects.
 */
void
lion_count_groups_slices(Relation heap, Snapshot snapshot, int ngroups,
						 LionPostingSet *groups, PGAlignedBlock *images,
						 const LionPostingSet *where, int nbits,
						 LionPostingSet *slices, LionSliceGroupsOut *out,
						 LionCountStats *stats, LionDecodeStats *dstats,
						 LionVisCache *cache, bool rel_read_only)
{
	const LionMatSet *mat = NULL;
	MemoryContext cxt;
	MemoryContext oldcxt;
	LionCountCtx base;
	LionCountCtx gcx;
	LionCountCtx scx;
	LionSliceRecheck rc;
	LionSetCursor *cur;
	LionSetCursor *scur;
	LionGroupEnt *heap0;
	int			nheap0 = 0;
	int		   *hot;
	bool	   *trust;
	uint32	   *val0;
	uint64	   *present;
	uint64	   *rows;
	uint64	   *wimg;
	uint16	   *members;
	uint64	   *planebuf;
	LionSlicePlane planes[LION_SLICE_MAX_BITS + 1];
	const uint64 *wplanes[LION_SLICE_MAX_BITS + 1];
	uint64		gathered[64];
	LionContainer *buf = NULL;
	uint32		firstkey = 0;
	int			nplanes = nbits + 1;
	int			idx = 0;
	int			g;
	int			i;
	int			j;

	for (g = 0; g < ngroups; g++)
	{
		if (out->allcount != NULL)
			out->allcount[g] = 0;
		out->count[g] = 0;
#ifdef HAVE_INT128
		if (out->sumenc != NULL)
			out->sumenc[g] = 0;
#endif
		if (out->minenc != NULL)
			out->minenc[g] = 0;
		if (out->maxenc != NULL)
			out->maxenc[g] = 0;
	}
	Assert(nbits >= 1 && nbits <= LION_SLICE_MAX_BITS);
	if (ngroups == 0)
		return;
	if (where != NULL)
	{
		if (!where->found || where->mat == NULL ||
			where->mat->ncontainers == 0)
			return;				/* the WHERE selects nothing */
		mat = where->mat;
		Assert(mat->collected);
		firstkey = lion_mat_key(mat, 0);
	}
	if (!slices[nbits].found && out->allcount == NULL)
		return;					/* no row has a value */

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"lion index slice decoded walk",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	if (cache != NULL && cache->windowed)
		elog(ERROR, "lion index count: a decoded walk confined to a window of keys");

	/* lion_count_groups_decode()'s set-up, for one column and the slices */
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
	base.in_recovery = RecoveryInProgress();
	base.rel_read_only = rel_read_only;
	base.raw = true;
	base.tids_sorted = true;
	lion_vis_cache_begin(cache, heap, snapshot);
	base.cache = cache;
	base.filter = (cache != NULL) ? cache->filter : NULL;
	if (base.filter != NULL && base.filter->heap != heap)
		elog(ERROR, "lion index count: a row filter for another relation");
	base.posdone = (mat != NULL && mat->posfiltered);
	base.batchmax = lion_recheck_budget_for(LION_SLICE_RECHECK_ENTRY);

	memset(&rc, 0, sizeof(rc));
	rc.cx = &base;

	cur = (LionSetCursor *) palloc(sizeof(LionSetCursor) * ngroups);
	scur = (LionSetCursor *) palloc0(sizeof(LionSetCursor) * nplanes);
	heap0 = (LionGroupEnt *) palloc(sizeof(LionGroupEnt) * ngroups);
	hot = (int *) palloc(sizeof(int) * ngroups);
	trust = (bool *) palloc(sizeof(bool) * ngroups);
	val0 = (uint32 *) palloc(sizeof(uint32) * LION_CONTAINER_RANGE);
	present = (uint64 *) palloc(LION_BITSET_BYTES);
	rows = (uint64 *) palloc(LION_BITSET_BYTES);
	wimg = (uint64 *) palloc(LION_BITSET_BYTES);
	members = (uint16 *) palloc(sizeof(uint16) * LION_CONTAINER_RANGE);
	planebuf = (uint64 *) palloc((Size) LION_BITSET_BYTES * nplanes);
	if (mat != NULL && lion_mat_spills(mat))
		buf = (LionContainer *) palloc(MAXALIGN(LION_CONTAINER_MAX_SIZE));

	/*
	 * The groups' cursors keep their pins; the slices' let go of each page
	 * once it is copied, as a decoded walk's other columns do.  Nothing below
	 * the copy's first key can be counted: every cursor begins there.
	 */
	gcx = base;
	gcx.vmbuf = InvalidBuffer;
	gcx.droppins = false;
	scx = base;
	scx.vmbuf = InvalidBuffer;
	scx.droppins = true;
	for (g = 0; g < ngroups; g++)
	{
		LionPostingSet *ps = &groups[g];

		trust[g] = ps->found && !ps->nopin && ps->mat == NULL;
		lion_cursor_init_at(&cur[g], ps, &gcx, false, firstkey,
							images != NULL ? &images[g] : NULL);
		if (cur[g].valid)
			lion_group_heap_push(heap0, &nheap0, cur[g].cur->ckey, g);
	}
	for (j = 0; j < nplanes; j++)
		lion_cursor_init_at(&scur[j], &slices[j], &scx, true, firstkey, NULL);

	while (nheap0 > 0)
	{
		uint32		key = heap0[0].ckey;
		BlockNumber firstblk = lion_ckey_first_block(key);
		const uint64 *wbits;
		const LionContainer *w;
		int			nhot = 0;
		uint64		wanted = 0;
		uint64		allvis;
		uint64		counted = 0;
		int			b;

		/* the copy at or past the smallest key a group stands at */
		if (mat != NULL)
		{
			base.stats.copy_seeks++;
			idx = lion_mat_seek(mat, idx, key);
			if (idx >= mat->ncontainers)
				break;
			if (lion_mat_key(mat, idx) != key)
			{
				uint32		target = lion_mat_key(mat, idx);

				while (nheap0 > 0 && heap0[0].ckey < target)
				{
					LionGroupEnt e = lion_group_heap_pop(heap0, &nheap0);

					lion_cursor_seek(&cur[e.g], target);
					if (cur[e.g].valid)
						lion_group_heap_push(heap0, &nheap0,
											 cur[e.g].cur->ckey, e.g);
				}
				CHECK_FOR_INTERRUPTS();
				continue;
			}
		}

		/* the groups standing at the key: each row's group, and the rows */
		while (nheap0 > 0 && heap0[0].ckey == key)
			hot[nhot++] = lion_group_heap_pop(heap0, &nheap0).g;
		memset(present, 0, LION_BITSET_BYTES);
		for (i = 0; i < nhot; i++)
		{
			int			k = hot[i];
			uint32		n = lion_container_to_array(cur[k].cur, members);
			uint32		m;

			for (m = 0; m < n; m++)
			{
				uint16		lo = members[m];

				val0[lo] = (uint32) k;
				present[lo >> 6] |= UINT64CONST(1) << (lo & 63);
			}
		}

		/* ... of them, the ones the copy keeps */
		if (mat != NULL)
		{
			w = lion_count_posfilter(&base,
									 lion_mat_container(&base, mat, idx, buf));
			if (w->type == LION_CT_BITSET)
				wbits = LION_BITSET_DATA(w);
			else
			{
				memset(wimg, 0, LION_BITSET_BYTES);
				lion_container_or_into_bitset(w, wimg);
				wbits = wimg;
			}
			for (i = 0; i < LION_BITSET_WORDS; i++)
				rows[i] = present[i] & wbits[i];
		}
		else
			memcpy(rows, present, LION_BITSET_BYTES);

		/* the heap blocks the rows are on */
		for (b = 0; b < LION_BLOCKS_PER_CONTAINER; b++)
		{
			const int	wpb = (1 << LION_OFFSET_BITS) / 64;
			uint64		any = 0;
			int			ww;

			for (ww = b * wpb; ww < (b + 1) * wpb; ww++)
				any |= rows[ww];
			if (any != 0)
				wanted |= UINT64CONST(1) << b;
		}

		if (wanted != 0)
		{
			/*
			 * The map, once for the key, every group's container here copied
			 * under its pin and none moved past it.
			 */
			LION_INJECTION_POINT("lion-count-containers-pinned");
			if (base.in_recovery || lion_row_filter_heap(base.filter))
				allvis = 0;
			else
			{
				allvis = lion_vm_allvisible_mask(heap, firstblk, wanted,
												 &base.vmbuf,
												 &base.stats.vm_pins);
				base.stats.vm_checks++;
#ifdef LION_VM_MASK_CHECK
				lion_vm_mask_check(heap, firstblk, wanted, allvis,
								   &base.vmbuf);
#endif
			}

			/*
			 * Each slice's container here, a plane of the key's positions: a
			 * bitmap read where it lies, an ARRAY or a RUN made an image.
			 */
			for (j = 0; j < nplanes; j++)
			{
				LionSetCursor *sc = &scur[j];
				const LionContainer *c;

				if (sc->valid && sc->cur->ckey < key)
				{
					if (sc->cur->ckey + 1 == key)
						lion_cursor_next(sc);
					else
						lion_cursor_seek(sc, key);
				}
				if (!sc->valid || sc->cur->ckey != key)
				{
					planes[j].data = NULL;
					planes[j].width = 0;
					continue;
				}
				c = sc->cur;
				if (c->type == LION_CT_BITSET || c->type == LION_CT_NARROW)
				{
					planes[j].data = LION_BITSET_DATA(c);
					planes[j].width = lion_container_width(c);
				}
				else
				{
					uint64	   *p = planebuf + (Size) j * LION_BITSET_WORDS;

					memset(p, 0, LION_BITSET_BYTES);
					lion_container_or_into_bitset(c, p);
					planes[j].data = p;
					planes[j].width = LION_BITSET_WIDTH;
				}
			}

			/*
			 * Each row's value - the rows of E - counted or sent to the heap
			 * with it; and, asked for the groups' counts, every row.
			 */
			for (i = 0; i < LION_BITSET_WORDS; i++)
			{
				uint64		bits = rows[i];
				uint64		vbits;
				bool		gather;

				if (bits == 0)
					continue;
				wplanes[nbits] = lion_slice_plane_word(&planes[nbits],
													   (uint32) i);
				vbits = bits & *wplanes[nbits];
				if (out->allcount == NULL)
					bits = vbits;
				if (bits == 0)
					continue;
				for (j = 0; j < nbits; j++)
					wplanes[j] = lion_slice_plane_word(&planes[j], (uint32) i);
				gather = (pg_popcount64(vbits) >= LION_SLICE_GATHER_MIN);
				if (gather)
					lion_planes_gather_strided(wplanes, 64, nbits, gathered);
				while (bits != 0)
				{
					int			bit = pg_rightmost_one_pos64(bits);
					uint32		lo = (uint32) (i * 64 + bit);
					uint32		g0 = val0[lo];
					bool		hasval = ((vbits >> bit) & 1) != 0;
					uint64		enc = 0;
					uint64		blkbit;

					bits &= bits - 1;
					if (hasval && gather)
						enc = gathered[bit];
					else if (hasval)
					{
						for (j = 0; j < nbits; j++)
							enc |= ((*wplanes[j] >> bit) & 1) << j;
					}
					blkbit = UINT64CONST(1) << (lo >> LION_OFFSET_BITS);
					if ((allvis & blkbit) != 0 && trust[g0])
					{
						if (out->allcount != NULL)
							out->allcount[g0]++;
						if (hasval)
						{
							lion_slice_decode_add(out, g0, enc);
							dstats->rows++;
						}
						counted |= blkbit;
					}
					else
						lion_slice_recheck_add(&rc, out, dstats, key, lo, g0,
											   enc, hasval);
				}
			}

			/* the blocks counted from the map, locked as an index-only scan */
			if (base.serializable)
			{
				uint64		m = counted;

				while (m != 0)
				{
					int			bb = pg_rightmost_one_pos64(m);

					m &= m - 1;
					PredicateLockPage(heap, firstblk + (BlockNumber) bb,
									  snapshot);
				}
			}
			base.stats.blocks_skipped_via_vm += pg_popcount64(counted);
		}
		dstats->keys++;

		/*
		 * Only now may the groups here let go of their pages (§9), each to
		 * the copy's next key - a step where that is the next key, a seek
		 * past the keys between otherwise (DESIGN.md §22) - or, with no
		 * WHERE, the next it has.
		 */
		if (mat != NULL && idx + 1 >= mat->ncontainers)
			break;
		for (i = 0; i < nhot; i++)
		{
			int			k = hot[i];

			if (mat != NULL && lion_mat_key(mat, idx + 1) != key + 1)
				lion_cursor_seek(&cur[k], lion_mat_key(mat, idx + 1));
			else
				lion_cursor_next(&cur[k]);
			if (cur[k].valid)
				lion_group_heap_push(heap0, &nheap0, cur[k].cur->ckey, k);
		}
		CHECK_FOR_INTERRUPTS();
	}

	for (g = 0; g < ngroups; g++)
		lion_cursor_close(&cur[g]);
	for (j = 0; j < nplanes; j++)
		lion_cursor_close(&scur[j]);

	/* no index page is pinned any more: the heap answers the rest */
	lion_slice_recheck_flush(&rc, out, dstats);

	if (stats != NULL)
	{
		lion_count_stats_add(stats, &base.stats);
		lion_count_stats_add(stats, &gcx.stats);
		lion_count_stats_add(stats, &scx.stats);
	}
	if (BufferIsValid(gcx.vmbuf))
		ReleaseBuffer(gcx.vmbuf);
	if (BufferIsValid(scx.vmbuf))
		ReleaseBuffer(scx.vmbuf);
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
