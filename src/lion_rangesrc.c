/*-------------------------------------------------------------------------
 *
 * lion_rangesrc.c
 *		A range as a source: its union collected into a private copy, or
 *		probed at the rows of the other sources (DESIGN.md §32).
 *
 * Part of the count engine: lion_count.h is its interface, and
 * lion_count_int.h declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_count_int.h"

/*
 * The union lion_range_collect() builds: one container per container key,
 * each the OR of what every set of the range has at that key.  The sets come
 * in key order, which for a column stored in the heap's order is container
 * key order too, but in general is not - so the containers are kept by key
 * in a hash table and put in order at the end.
 *
 * Only the container keys in [lo, hi) are kept: all of them, until a
 * WINDOWED collection runs out of memory and lowers hi
 * (lion_range_union_evict()).
 *
 * DENSE ACCUMULATION (DESIGN.md §32, "Summed ranges: dense and probed").  A
 * range over a column in no heap order gives every container key a member or
 * two from each summary, and folding each of them in with a
 * lion_container_or() rebuilt the union so far every time: a merge of the
 * whole ARRAY while it was one, and past 2048 members a 4 KB image filled,
 * counted, optimized and copied back, per member.  So a union keeps the form
 * it grows in instead.  Once it is a BITSET the containers are ORed into it
 * in place (lion_container_or_inplace()) and it is optimized once, when the
 * walk is over (lion_range_union_finish()).  While it is an ARRAY the
 * members of incoming ARRAYs are only appended to `pend`, and folded in -
 * one pass through an image, lion_container_add_many() - once half as many
 * as the union holds have come: each member is then moved a bounded number
 * of times, and `pend` is at most half the union's own size.  An ARRAY that
 * a larger ARRAY comes to is merged with it, while the two fit an ARRAY.
 *
 * Anything else would go through an image - a RUN union, which is what a
 * dense union optimizes to (a heap block's rows are a run) and what every
 * union of a column in heap order is, or a RUN or a BITSET coming to an
 * ARRAY - and the union is WIDENED to a BITSET instead
 * (lion_range_union_widen()), ORed into in place from then on.  Folded, a
 * RUN union cost a 4 KB image per container that came to it however few
 * members it brought, which over a range of scattered rows is per member:
 * the collection went from 19 ms at 10,000 keys to 12.7 s at 190,000, 22
 * times core's plan (2026-09-29 review).  A widened union takes a BITSET's
 * memory where its optimized form may take a few bytes, so the unions
 * widened since the last time are optimized again - COMPACTED,
 * lion_range_union_compact() - whenever that memory runs short: before a
 * union is refused a widening, which then folds as before, and before the
 * collection gives up or a window gives back keys, which therefore happens
 * only where it did before.  A widening is made where a fold would have been
 * and compacted at most once, so a collection short of memory spends about
 * what it did, and one that is not a fold or two a union key.
 */
typedef struct LionRangeUnionEnt
{
	uint32		ckey;			/* hash key */
	uint32		size;
	LionContainer *c;
	uint16	   *pend;			/* members still to fold into c, any order */
	uint32		npend;
	uint32		pendcap;
} LionRangeUnionEnt;

typedef struct LionRangeUnion
{
	HTAB	   *byckey;
	MemoryContext cxt;			/* the containers */
	Size		held;			/* what they take, with overhead */
	Size		maxbytes;
	bool		failed;
	bool		window;			/* past maxbytes, lower hi rather than fail */
	uint64		lo;				/* the container keys kept: [lo, hi) */
	uint64		hi;
	LionContainer *tmp;			/* LION_CONTAINER_MAX_SIZE bytes */
	uint64	   *img;			/* LION_BITSET_BYTES: a fold's image */
	uint32	   *widened;		/* the keys widened since the last compaction */
	int			nwidened;
	int			widenedcap;
} LionRangeUnion;

/* No upper bound on the container keys of a window. */
#define LION_CKEY_END				((uint64) PG_UINT32_MAX + 1)

/* What one kept container costs beyond its chunk: its hash entry. */
#define LION_RANGE_UNION_OVERHEAD	(sizeof(LionRangeUnionEnt) + 16)

/*
 * The least memory a window has, whatever is left of the caller's: several
 * bitsets, so that a window always keeps some keys and gets on.
 */
#define LION_RANGE_WINDOW_MIN		(32 * 1024)

static int
lion_ckey_cmp(const void *a, const void *b)
{
	uint32		x = *(const uint32 *) a;
	uint32		y = *(const uint32 *) b;

	return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/*
 * A windowed collection past its memory: keep the lower half of the keys
 * the window holds, give the others back, and end the window where they
 * began - the next window starts there, and reads the range again for them.
 * With the window's memory at least LION_RANGE_WINDOW_MIN there are always
 * two keys or more to split.
 */
static void
lion_range_union_evict(LionRangeUnion *u)
{
	long		n = hash_get_num_entries(u->byckey);
	HASH_SEQ_STATUS seq;
	LionRangeUnionEnt *e;
	uint32	   *keys;
	uint32		cut;
	long		i = 0;

	if (n < 2)
		return;
	keys = (uint32 *) MemoryContextAlloc(u->cxt, sizeof(uint32) * n);
	hash_seq_init(&seq, u->byckey);
	while ((e = (LionRangeUnionEnt *) hash_seq_search(&seq)) != NULL)
		keys[i++] = e->ckey;
	Assert(i == n);
	qsort(keys, n, sizeof(uint32), lion_ckey_cmp);
	cut = keys[n / 2];
	pfree(keys);

	/* deleting the entry just returned is allowed during a hash_seq_search */
	hash_seq_init(&seq, u->byckey);
	while ((e = (LionRangeUnionEnt *) hash_seq_search(&seq)) != NULL)
	{
		uint32		ckey = e->ckey;

		if (ckey < cut)
			continue;
		u->held -= Min(u->held,
					   GetMemoryChunkSpace(e->c) + LION_RANGE_UNION_OVERHEAD);
		pfree(e->c);
		if (e->pend != NULL)
		{
			u->held -= Min(u->held, GetMemoryChunkSpace(e->pend));
			pfree(e->pend);
		}
		(void) hash_search(u->byckey, &ckey, HASH_REMOVE, NULL);
	}
	u->hi = cut;
}

/* Make src, a container of e's key, e's union: at its own size. */
static void
lion_range_union_store(LionRangeUnion *u, LionRangeUnionEnt *e,
					   const LionContainer *src)
{
	Size		size = lion_container_size(src);

	if (size != e->size)
	{
		u->held -= Min(u->held, GetMemoryChunkSpace(e->c));
		pfree(e->c);
		e->c = (LionContainer *) MemoryContextAlloc(u->cxt, size);
		u->held += GetMemoryChunkSpace(e->c);
		e->size = (uint32) size;
	}
	memcpy(e->c, src, size);
}

/*
 * Fold e's pending members into its union, in one pass through an image
 * (lion_container_add_many()).  A union that is no ARRAY afterwards takes
 * no more pending members, and gives their buffer back.
 */
static void
lion_range_union_fold(LionRangeUnion *u, LionRangeUnionEnt *e)
{
	if (e->npend == 0)
		return;
	memcpy(u->tmp, e->c, e->size);
	(void) lion_container_add_many(u->tmp, e->pend, e->npend, u->img);
	e->npend = 0;
	lion_range_union_store(u, e, u->tmp);
	if (e->c->type != LION_CT_ARRAY)
	{
		u->held -= Min(u->held, GetMemoryChunkSpace(e->pend));
		pfree(e->pend);
		e->pend = NULL;
		e->pendcap = 0;
	}
}

/*
 * Optimize every union widened since the last compaction that is still there
 * - a windowed collection may have given it back since - and that is still a
 * BITSET, so that what the unions hold is what their optimized forms take
 * again (DENSE ACCUMULATION, above).  A union that comes out of it a BITSET
 * all the same is one past what an ARRAY or a RUN holds, and stays one.
 */
static void
lion_range_union_compact(LionRangeUnion *u)
{
	int			i;

	for (i = 0; i < u->nwidened; i++)
	{
		LionRangeUnionEnt *e = (LionRangeUnionEnt *)
			hash_search(u->byckey, &u->widened[i], HASH_FIND, NULL);

		if (e == NULL || e->c->type != LION_CT_BITSET)
			continue;
		memcpy(u->tmp, e->c, e->size);
		lion_container_optimize(u->tmp);
		lion_range_union_store(u, e, u->tmp);
	}
	u->nwidened = 0;
}

/*
 * Make e's union, which is no BITSET and has nothing pending, a BITSET to OR
 * into in place (DENSE ACCUMULATION, above) - if the memory has room for one,
 * after compacting the unions widened before it if it has not.  False, with
 * nothing changed, when it has no room even then: the caller folds.
 */
static bool
lion_range_union_widen(LionRangeUnion *u, LionRangeUnionEnt *e)
{
	Assert(e->c->type != LION_CT_BITSET && e->npend == 0);

	if (u->held + LION_CONTAINER_MAX_SIZE > u->maxbytes)
	{
		lion_range_union_compact(u);
		if (u->held + LION_CONTAINER_MAX_SIZE > u->maxbytes)
			return false;
	}
	if (u->nwidened == u->widenedcap)
	{
		Size		had = (u->widened != NULL) ?
			GetMemoryChunkSpace(u->widened) : 0;

		u->widenedcap = Max(64, u->widenedcap * 2);
		u->widened = (u->widened != NULL) ?
			(uint32 *) repalloc(u->widened, sizeof(uint32) * u->widenedcap) :
			(uint32 *) MemoryContextAlloc(u->cxt,
										  sizeof(uint32) * u->widenedcap);
		u->held += GetMemoryChunkSpace(u->widened);
		u->held -= Min(u->held, had);
	}
	u->widened[u->nwidened++] = e->ckey;

	/* a BITSET union takes no pending members */
	if (e->pend != NULL)
	{
		u->held -= Min(u->held, GetMemoryChunkSpace(e->pend));
		pfree(e->pend);
		e->pend = NULL;
		e->pendcap = 0;
	}
	memcpy(u->tmp, e->c, e->size);
	lion_container_to_bitset(u->tmp);
	lion_range_union_store(u, e, u->tmp);
	return true;
}

/*
 * The walk is over: every union folded and optimized, which is the form it
 * is handed out in.  A BITSET that was ORed into in place - widened, or past
 * what an ARRAY holds - may now be smaller as an ARRAY or a RUN.
 */
static void
lion_range_union_finish(LionRangeUnion *u)
{
	HASH_SEQ_STATUS seq;
	LionRangeUnionEnt *e;

	hash_seq_init(&seq, u->byckey);
	while ((e = (LionRangeUnionEnt *) hash_seq_search(&seq)) != NULL)
	{
		lion_range_union_fold(u, e);
		if (e->pend != NULL)
		{
			u->held -= Min(u->held, GetMemoryChunkSpace(e->pend));
			pfree(e->pend);
			e->pend = NULL;
			e->pendcap = 0;
		}
		if (e->c->type == LION_CT_BITSET)
		{
			memcpy(u->tmp, e->c, e->size);
			lion_container_optimize(u->tmp);
			lion_range_union_store(u, e, u->tmp);
		}
	}
}

static bool
lion_range_union_cb(const LionContainer *c, void *arg)
{
	LionRangeUnion *u = (LionRangeUnion *) arg;
	LionRangeUnionEnt *e;
	uint32		ckey = c->ckey;
	bool		found;
	Size		size;

	if (lion_container_cardinality(c) == 0)
		return true;
	if ((uint64) ckey < u->lo || (uint64) ckey >= u->hi)
		return true;			/* another window's */

	/*
	 * What a container takes is counted as the allocator hands it out
	 * (GetMemoryChunkSpace()): a BITSET's 4104 bytes rounded up to a chunk of
	 * 8 kB was counted as 4104, and a union held twice what it said.  The
	 * containers' context gives anything over a kilobyte a block of its own
	 * (lion_range_collect()), so that no longer happens either.
	 */
	e = (LionRangeUnionEnt *) hash_search(u->byckey, &ckey, HASH_ENTER, &found);
	if (!found)
	{
		size = lion_container_size(c);
		e->c = (LionContainer *) MemoryContextAlloc(u->cxt, size);
		memcpy(e->c, c, size);
		e->size = (uint32) size;
		e->pend = NULL;
		e->npend = 0;
		e->pendcap = 0;
		u->held += GetMemoryChunkSpace(e->c) + LION_RANGE_UNION_OVERHEAD;
	}
	else if (e->c->type == LION_CT_BITSET)
	{
		/* dense: ORed in place, optimized once the walk is over */
		(void) lion_container_or_inplace(e->c, c);
	}
	else if (e->c->type == LION_CT_ARRAY && c->type == LION_CT_ARRAY &&
			 c->cardinality <= LION_RANGE_UNION_PEND_MIN)
	{
		/*
		 * A few members: pending, until half as many as the union holds
		 * have come.  The buffer grows with the union, never past that.
		 */
		uint32		fold = Max((uint32) LION_RANGE_UNION_PEND_MIN,
							   (uint32) e->c->cardinality / 2);
		uint32		want = fold + LION_RANGE_UNION_PEND_MIN;

		if (e->pendcap < want)
		{
			Size		had = (e->pend != NULL) ?
				GetMemoryChunkSpace(e->pend) : 0;

			e->pend = (e->pend != NULL) ?
				(uint16 *) repalloc(e->pend, sizeof(uint16) * want) :
				(uint16 *) MemoryContextAlloc(u->cxt, sizeof(uint16) * want);
			e->pendcap = want;
			u->held += GetMemoryChunkSpace(e->pend);
			u->held -= Min(u->held, had);
		}
		memcpy(&e->pend[e->npend], LION_ARRAY_DATA((LionContainer *) c),
			   sizeof(uint16) * c->cardinality);
		e->npend += c->cardinality;
		if (e->npend >= fold)
			lion_range_union_fold(u, e);
	}
	else
	{
		lion_range_union_fold(u, e);
		if (e->c->type != LION_CT_BITSET &&
			!(e->c->type == LION_CT_ARRAY && c->type == LION_CT_ARRAY &&
			  (uint32) e->c->cardinality + c->cardinality <=
			  LION_ARRAY_MAX_CARD))
			(void) lion_range_union_widen(u, e);
		if (e->c->type == LION_CT_BITSET)
			(void) lion_container_or_inplace(e->c, c);
		else
		{
			/* two ARRAYs merged, or a union refused a widening folded */
			(void) lion_container_or(e->c, c, u->tmp);
			lion_range_union_store(u, e, u->tmp);
		}
	}

	/* the widened unions give back what they took first */
	if (u->held > u->maxbytes && u->nwidened > 0)
		lion_range_union_compact(u);
	if (u->held > u->maxbytes)
	{
		if (!u->window)
		{
			u->failed = true;
			return false;
		}
		while (u->held > u->maxbytes && hash_get_num_entries(u->byckey) > 1)
			lion_range_union_evict(u);
	}
	return true;
}

static int
lion_range_union_cmp(const void *a, const void *b)
{
	uint32		x = (*(LionRangeUnionEnt *const *) a)->ckey;
	uint32		y = (*(LionRangeUnionEnt *const *) b)->ckey;

	return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/*
 * One window of a range's union (above): walk the range and keep what its
 * sets hold at the window's keys.  nsets and nsummaries, when given, say what
 * the walk read.
 */
static void
lion_range_union_walk(Relation index, AttrNumber attno, LionRange *range,
					  LionRangeUnion *u, int64 *nsets, int64 *nsummaries)
{
	LionEntryScan es;
	LionPostingSet ps;
	Datum		key;

	lion_entry_scan_begin_sum(&es, index, attno, range, LION_WALK_INSIDE);
	while (!u->failed && lion_entry_scan_next(&es, &key, &ps))
	{
		if (ps.found)
			(void) lion_sets_iterate(1, &ps, NULL, lion_range_union_cb, u);
		lion_posting_set_release(&ps);
		if (nsets != NULL)
			(*nsets)++;
		CHECK_FOR_INTERRUPTS();
	}
	if (nsummaries != NULL)
		*nsummaries = es.nsummaries;
	lion_entry_scan_end(&es);
}

/* The union's entries, in key order, in the current memory context. */
static LionRangeUnionEnt **
lion_range_union_sorted(LionRangeUnion *u, long *n)
{
	HASH_SEQ_STATUS seq;
	LionRangeUnionEnt *e;
	LionRangeUnionEnt **ents;
	long		i = 0;

	*n = hash_get_num_entries(u->byckey);
	ents = (LionRangeUnionEnt **)
		palloc(sizeof(LionRangeUnionEnt *) * Max(*n, 1));
	hash_seq_init(&seq, u->byckey);
	while ((e = (LionRangeUnionEnt *) hash_seq_search(&seq)) != NULL)
		ents[i++] = e;
	Assert(i == *n);
	if (*n > 1)
		qsort(ents, *n, sizeof(LionRangeUnionEnt *), lion_range_union_cmp);
	return ents;
}

/*
 * The rows of one range, collected (DESIGN.md §32, "A range as a source").
 *
 * The walk is the one a summed range makes (lion_entry_scan_begin_sum()):
 * the entries of the range's partial buckets and the summaries of its whole
 * ones, disjoint sets of one scalar column whose union is exactly the rows
 * whose key lies in the range.  Each set's containers are ORed into the union
 * as they come; its pin, if it has one, is dropped once it has been read.
 *
 * What comes out is safe on lion_sources_collect()'s terms, for the same
 * reasons: a stale copy - a TID VACUUM has since removed - is only ever
 * counted beside a located set that holds the §9 pin, which settles it, and
 * no row visible to the caller's snapshot can be missing, because the copy is
 * read after the snapshot was taken and every such row was in the index
 * before its transaction committed.  The same goes for the summaries: an
 * insert puts its row under its key and then under its bucket's summary, both
 * before it commits, and the walk reads the keys of a bucket or its summary,
 * never both (DESIGN.md §32, "Readers").
 *
 * A union that fits in maxbytes IS the set: its containers stay where the
 * union built them, in a context that becomes the set's, and are handed out
 * in key order through containers[].  It used to be copied into one buffer
 * at the end, while the hash table still held it, which took twice what the
 * union did at the peak (2026-09-28 review).
 *
 * One that does not fit fails - the caller walks the range at every count
 * instead - unless spill says it may not: an OR's leaf, which cannot be taken
 * apart that way and used to be collected whatever it took.  It is then
 * collected a WINDOW of container keys at a time into a temporary file
 * (LionSpill): a window keeps the keys from where the last one ended; when
 * its memory - maxbytes, and never below LION_RANGE_WINDOW_MIN - runs out it
 * gives back the upper half of its keys and ends where they began
 * (lion_range_union_evict()); at the end of the walk its containers go to
 * the file in key order, and the next window reads the range again from its
 * end.  Every window is a walk of the range, so a union n times the memory
 * costs about 2n walks; each is read after the snapshot like the first, and
 * the windows are disjoint ranges of TIDs, so the file holds each row once.
 * The set reads the file a container at a time; what it keeps in memory is
 * sixteen bytes a container key.
 */
bool
lion_range_collect(Relation index, AttrNumber attno, LionRange *range,
				   Size maxbytes, bool spill, LionPostingSet *out, Size *held,
				   bool *spilled, int64 *nsets, int64 *nsummaries)
{
	LionRangeUnion u;
	LionSpill	sp;
	HASHCTL		ctl;
	LionRangeUnionEnt **ents;
	LionMatSet *mat;
	MemoryContext cxt;
	uint64		lo = 0;
	uint64		members = 0;
	bool		windowed = false;
	long		n;
	long		i;

	memset(out, 0, sizeof(LionPostingSet));
	out->pinbuf = InvalidBuffer;
	out->head = InvalidBlockNumber;
	out->index = index;
	out->attno = attno;
	out->cxt = CurrentMemoryContext;
	out->entryblk = InvalidBlockNumber;
	out->entryoff = InvalidOffsetNumber;
	*held = 0;
	*spilled = false;
	*nsets = 0;
	*nsummaries = 0;
	memset(&sp, 0, sizeof(sp));

	/* the catalogs a spill's file needs, looked up before the walk starts */
	if (spill)
		PrepareTempTablespaces();

	for (;;)
	{
		/*
		 * One window, in a context of its own.  Anything over a kilobyte - a
		 * BITSET, a long ARRAY - is a block of its own there, allocated at
		 * its size and given back to malloc when freed (ALLOCSET_SMALL_SIZES
		 * put the chunk limit at 1 kB), so the union takes about what its
		 * containers are.
		 */
		cxt = AllocSetContextCreate(CurrentMemoryContext, "lion range collect",
									ALLOCSET_SMALL_SIZES);
		memset(&u, 0, sizeof(u));
		u.cxt = cxt;
		u.maxbytes = spill ? Max(maxbytes, (Size) LION_RANGE_WINDOW_MIN) :
			maxbytes;
		u.window = spill;
		u.lo = lo;
		u.hi = LION_CKEY_END;
		u.tmp = (LionContainer *) MemoryContextAlloc(cxt,
													 LION_CONTAINER_MAX_SIZE);
		u.img = (uint64 *) MemoryContextAlloc(cxt, LION_BITSET_BYTES);
		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(uint32);
		ctl.entrysize = sizeof(LionRangeUnionEnt);
		ctl.hcxt = cxt;
		u.byckey = hash_create("lion range union", 256, &ctl,
							   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

		/* the first window's walk is the one that says what the range read */
		lion_range_union_walk(index, attno, range, &u,
							  windowed ? NULL : nsets,
							  windowed ? NULL : nsummaries);

		if (u.failed)
		{
			Assert(!windowed);
			MemoryContextDelete(cxt);
			return false;
		}

		lion_range_union_finish(&u);
		ents = lion_range_union_sorted(&u, &n);

		if (!windowed && u.hi == LION_CKEY_END)
		{
			/*
			 * All of it, in memory: the containers stay where they are, and
			 * cxt - with nothing else left in it - is the set's.
			 */
			if (n == 0)
			{
				pfree(ents);
				MemoryContextDelete(cxt);
				return true;	/* the range selects nothing: not found */
			}
			mat = (LionMatSet *) palloc0(sizeof(LionMatSet));
			mat->ncontainers = (int) n;
			mat->containers = (LionContainer **)
				palloc(sizeof(LionContainer *) * n);
			for (i = 0; i < n; i++)
			{
				mat->containers[i] = ents[i]->c;
				mat->bytes += ents[i]->size;
				members += lion_container_cardinality(ents[i]->c);
			}
			pfree(ents);
			pfree(u.tmp);
			pfree(u.img);
			if (u.widened != NULL)
				pfree(u.widened);
			hash_destroy(u.byckey);
			mat->held = sizeof(LionMatSet) + sizeof(LionContainer *) * n +
				MemoryContextMemAllocated(cxt, true);

			out->found = true;
			out->mat = mat;
			out->ntids = members;
			out->ncontainers = (uint32) n;
			*held = mat->held;
			return true;
		}

		/* A window of more: its containers, in key order, to the file. */
		if (!windowed)
		{
			lion_spill_begin(&sp, CurrentMemoryContext);
			windowed = true;
		}
		for (i = 0; i < n; i++)
		{
			lion_spill_add(&sp, ents[i]->c);
			members += lion_container_cardinality(ents[i]->c);
		}
		pfree(ents);
		MemoryContextDelete(cxt);

		if (u.hi == LION_CKEY_END)
			break;
		Assert(u.hi > lo);
		lo = u.hi;
	}

	if (sp.nents == 0)
	{
		lion_spill_abandon(&sp);
		return true;			/* the range selects nothing: not found */
	}
	mat = lion_spill_finish(&sp);
	out->found = true;
	out->mat = mat;
	out->ntids = members;
	out->ncontainers = (uint32) mat->ncontainers;
	*held = mat->held;
	*spilled = true;
	return true;
}

/* ---------------------------------------------------------------------
 * A summed range PROBED at the rows of the other sources (DESIGN.md §32,
 * "Summed ranges: dense and probed")
 * --------------------------------------------------------------------- */

/*
 * A sum over a range counts the sets its walk hands out - the keys of its
 * partial buckets and the summaries of its whole ones - one count each, ANDed
 * with the other sources F.  When the column's rows lie all over the heap
 * every summary has a container at nearly every container key it can, a few
 * members each, and every one of those counts is a merge of thousands of tiny
 * containers with F: the summary drives, F is sought at each of its keys, or
 * F drives and the summary is sought at each of F's.  Either way the work is
 * the number of summaries times the smaller side, with the set-up of a count
 * on top of each, however few rows F has.
 *
 * PROBED, the sum turns around.  F is collected once - the intersection of
 * its sources, pinless, lion_sources_collect() - and every set of the walk is
 * then read only at F's container keys: its cursor is sought from one of them
 * to the next, and what it holds there is marked against F's members.  An
 * ARRAY container of F keeps a bit per member (lion_container_mark_members()),
 * any other a bitset image the range's containers are ORed into.  What the
 * walk leaves is the range's rows among F's, at most F's size whatever the
 * range covers; ONE count of that set ANDed with F answers the sum.
 *
 * WHY IT IS EXACT.  The sets of a summed walk are disjoint (§32, "Readers")
 * and their union is the rows of the part, so the sum of their counts is the
 * count of their union ANDed with F, and so is the count of any set that
 * holds every visible row of that union that F holds and nothing outside the
 * union.  The marks are that set: a member is marked only when a set of the
 * walk holds it, and every visible row of the union that F holds is in the
 * collected copy of F, because the copy is read after the caller's snapshot
 * was taken and a visible row was in every index before its transaction
 * committed - the collected set's argument (lion_sources_collect()).
 *
 * WHY §9 STILL HOLDS.  The walk's sets are read without pins, as a collected
 * range's are (§32, "A range as a source"), and the marks are a copy.  So the
 * count of them is made beside F as the count reads it - the view below, in
 * which a set an earlier count copied into memory is walked from its chain
 * again - and only when a positive source of F carries the interlock there
 * (lion_source_pinned()); otherwise the caller sums the old way, where each
 * set of the walk carries it.  Every member the count takes from the map is
 * then in the container of that source it holds a pin on, which is all the
 * §9 argument asks of a candidate (lion_posting_set_materialize()).
 */
struct LionRangeProbe
{
	MemoryContext cxt;			/* everything below */
	MemoryContext setcxt;		/* one added set's cursor, reset after it */
	int			nsources;		/* F, as the final count reads it */
	LionCountSource *sources;
	LionPostingSet probe;		/* F's intersection, collected */
	int			n;				/* its containers */
	uint32	   *keys;			/* ... their keys, ascending */
	const LionContainer **conts;
	uint64	  **marks;			/* per container: a bit per ARRAY member, or
								 * an image (NULL until something lands) */
	Relation	index;			/* the range's, from the first set added */
	uint16		attno;
	LionCountCtx cx;			/* the cursors': no pins, statistics only */
};

/* First i >= from with keys[i] >= target, or n: a galloping search. */
static inline int
lion_ckey_gallop(const uint32 *keys, int n, int from, uint32 target)
{
	int			lo = from;
	int			step = 1;
	int			hi;

	if (from >= n || keys[from] >= target)
		return from;
	while (from + step < n && keys[from + step] < target)
	{
		lo = from + step;
		step *= 2;
	}
	hi = Min(from + step, n);
	lo++;
	while (lo < hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (keys[mid] < target)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/*
 * Begin a probed sum beside the sources F (sources[0 .. nsources - 1]): NULL
 * when it cannot be taken - F has no positive source, holds a range still to
 * be walked, has no positive source that would carry the §9 interlock, or its
 * collected copy and the marks would take more than maxbytes - and the
 * caller then sums as it always has.  The probe is allocated in a context of
 * its own under the current one; lion_range_probe_end() frees it.  F must
 * stay located until then.
 */
LionRangeProbe *
lion_range_probe_begin(Relation heap, Snapshot snapshot, int nsources,
					   LionCountSource *sources, Size maxbytes,
					   LionCountStats *stats)
{
	LionRangeProbe *rp;
	MemoryContext cxt;
	MemoryContext oldcxt;
	LionOpenBudget budget;
	LionMatSet *mat;
	uint64	   *words;
	Size		need;
	Size		nwords = 0;
	bool		positive = false;
	bool		carried = false;
	bool		spilled;
	int			i;
	int			j;

	for (i = 0; i < nsources; i++)
	{
		if (sources[i].rangewalk != NULL)
			return NULL;
		if (!sources[i].negated)
			positive = true;
	}
	if (!positive)
		return NULL;

	cxt = AllocSetContextCreate(CurrentMemoryContext, "lion range probe",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);
	rp = (LionRangeProbe *) palloc0(sizeof(LionRangeProbe));
	rp->cxt = cxt;

	/*
	 * F as the final count reads it: copies of its sets, of which a CHAIN set
	 * an earlier count copied into memory (lion_posting_set_materialize()) is
	 * walked from its root again, under the pins of its own cursor.  None is
	 * copied into memory by that count - a copy would carry no interlock,
	 * and it reads each set once - and the sets themselves are left as they
	 * are for the counts of the walk before and after this one.  The copies
	 * share the sets' payloads and pins, which stay the caller's.
	 */
	lion_open_budget_init(&budget, heap);
	rp->nsources = nsources;
	rp->sources = (LionCountSource *) palloc(sizeof(LionCountSource) * nsources);
	for (i = 0; i < nsources; i++)
	{
		LionCountSource *s = &rp->sources[i];

		*s = sources[i];
		s->sets = (LionPostingSet *)
			palloc(sizeof(LionPostingSet) * Max(s->nsets, 1));
		memcpy(s->sets, sources[i].sets, sizeof(LionPostingSet) * s->nsets);
		for (j = 0; j < s->nsets; j++)
		{
			LionPostingSet *ps = &s->sets[j];

			ps->matfailed = true;
			if (ps->found && ps->mat != NULL && !ps->is_inline &&
				BlockNumberIsValid(ps->head))
			{
				ps->mat = NULL;
				ps->budgeted = false;	/* the original returns its pin */
			}
		}
		if (!s->negated &&
			lion_source_pinned(lion_source_tree(s), s->sets, &budget))
			carried = true;
	}
	if (!carried)
	{
		MemoryContextSwitchTo(oldcxt);
		MemoryContextDelete(cxt);
		return NULL;
	}

	/* F's rows, copied: half of the memory, the marks and the count the rest */
	if (!lion_sources_collect(heap, snapshot, nsources, sources, maxbytes / 2,
							  false, &rp->probe, &spilled, stats))
	{
		MemoryContextSwitchTo(oldcxt);
		MemoryContextDelete(cxt);
		return NULL;
	}
	Assert(!spilled);

	mat = rp->probe.found ? rp->probe.mat : NULL;
	rp->n = (mat != NULL) ? mat->ncontainers : 0;
	rp->keys = (uint32 *) palloc(sizeof(uint32) * Max(rp->n, 1));
	rp->conts = (const LionContainer **)
		palloc(sizeof(LionContainer *) * Max(rp->n, 1));
	rp->marks = (uint64 **) palloc0(sizeof(uint64 *) * Max(rp->n, 1));

	/*
	 * What the marks and the count's set can take: a bit per member of an
	 * ARRAY and the ARRAY itself again, an image and a BITSET's worth for
	 * any other.  Checked before anything is walked, so that nothing has to
	 * be given up halfway.
	 */
	need = (mat != NULL) ? mat->held : 0;
	for (i = 0; i < rp->n; i++)
	{
		const LionContainer *c = mat->containers[i];

		rp->keys[i] = c->ckey;
		rp->conts[i] = c;
		if (c->type == LION_CT_ARRAY)
		{
			uint32		card = Min((uint32) c->cardinality,
								   (uint32) LION_ARRAY_MAX_CARD);

			nwords += (card + 63) / 64;
			need += lion_container_size_for(LION_CT_ARRAY, card, 0) + 16;
		}
		else
			need += 2 * (LION_CONTAINER_MAX_SIZE + 16);
	}
	need += nwords * sizeof(uint64) +
		(Size) rp->n * (sizeof(uint32) + 2 * sizeof(void *));
	if (need > maxbytes)
	{
		MemoryContextSwitchTo(oldcxt);
		MemoryContextDelete(cxt);
		return NULL;
	}

	words = (uint64 *) palloc0(sizeof(uint64) * Max(nwords, 1));
	for (i = 0; i < rp->n; i++)
	{
		const LionContainer *c = rp->conts[i];

		if (c->type != LION_CT_ARRAY)
			continue;
		rp->marks[i] = words;
		words += (Min((uint32) c->cardinality, (uint32) LION_ARRAY_MAX_CARD) +
				  63) / 64;
	}

	/* The cursors of the walk's sets: they carry nothing, they only read. */
	rp->cx.cxt = cxt;
	rp->cx.vmbuf = InvalidBuffer;
	rp->cx.droppins = true;
	rp->setcxt = AllocSetContextCreate(cxt, "lion range probe set",
									   ALLOCSET_DEFAULT_SIZES);

	MemoryContextSwitchTo(oldcxt);
	return rp;
}

/* What c, at the key of F's container i, holds of F's rows. */
static void
lion_range_probe_mark(LionRangeProbe *rp, int i, const LionContainer *c)
{
	const LionContainer *p = rp->conts[i];

	if (p->type == LION_CT_ARRAY)
	{
		(void) lion_container_mark_members(c,
										   LION_ARRAY_DATA((LionContainer *) p),
										   Min((uint32) p->cardinality,
											   (uint32) LION_ARRAY_MAX_CARD),
										   rp->marks[i]);
		return;
	}
	if (rp->marks[i] == NULL)
		rp->marks[i] = (uint64 *) MemoryContextAllocZero(rp->cxt,
														 LION_BITSET_BYTES);
	lion_container_or_into_bitset(c, rp->marks[i]);
}

/*
 * Add one set the walk handed out.  Its cursor holds no pin and is sought
 * from each of F's container keys to the next: a set with nothing at them
 * costs the skips of its cursor - over a sparse segment's pairs, across a
 * page by a descent - and not a container each.  The set stays the caller's
 * to release.
 */
void
lion_range_probe_add(LionRangeProbe *rp, LionPostingSet *set)
{
	LionSetCursor cur;
	MemoryContext oldcxt;
	int			i = 0;

	if (!set->found)
		return;
	if (rp->index == NULL)
	{
		rp->index = set->index;
		rp->attno = set->attno;
	}
	if (rp->n == 0)
		return;					/* F selects nothing */

	oldcxt = MemoryContextSwitchTo(rp->setcxt);
	lion_cursor_init(&cur, set, &rp->cx, true);
	while (cur.valid)
	{
		uint32		ckey = cur.cur->ckey;

		if (rp->keys[i] < ckey)
		{
			i = lion_ckey_gallop(rp->keys, rp->n, i, ckey);
			if (i >= rp->n)
				break;
		}
		if (rp->keys[i] == ckey)
		{
			lion_range_probe_mark(rp, i, cur.cur);
			lion_cursor_next(&cur);
		}
		else
			lion_cursor_seek(&cur, rp->keys[i]);
		CHECK_FOR_INTERRUPTS();
	}
	lion_cursor_close(&cur);
	MemoryContextSwitchTo(oldcxt);
	MemoryContextReset(rp->setcxt);
}

/*
 * The sum: the rows the added sets hold among F's, ANDed with F as the count
 * reads it, counted once - under the visibility map, the recheck and the
 * row filter of any count (lion_count_sources_cached()).
 */
int64
lion_range_probe_count(LionRangeProbe *rp, Relation heap, Snapshot snapshot,
					   LionCountStats *stats, LionVisCache *cache,
					   bool rel_read_only)
{
	MemoryContext oldcxt;
	LionMatSet *mat;
	LionPostingSet acc;
	LionCountSource *srcs;
	LionContainer *tmp;
	LionContainer *res;
	uint64		members = 0;
	int64		result = 0;
	int			k = 0;
	int			i;

	if (stats != NULL)
		stats->containers_visited += rp->cx.stats.containers_visited;
	rp->cx.stats.containers_visited = 0;
	if (rp->n == 0 || rp->index == NULL)
		return 0;

	oldcxt = MemoryContextSwitchTo(rp->cxt);
	tmp = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	res = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	mat = (LionMatSet *) palloc0(sizeof(LionMatSet));
	mat->containers = (LionContainer **)
		palloc(sizeof(LionContainer *) * rp->n);

	for (i = 0; i < rp->n; i++)
	{
		const LionContainer *p = rp->conts[i];
		const uint64 *m = rp->marks[i];
		Size		size;

		if (m == NULL)
			continue;
		if (p->type == LION_CT_ARRAY)
		{
			const uint16 *arr = LION_ARRAY_DATA((LionContainer *) p);
			uint32		card = Min((uint32) p->cardinality,
								   (uint32) LION_ARRAY_MAX_CARD);
			uint32		j;

			lion_container_init(res, p->ckey);
			for (j = 0; j < card; j++)
				if ((m[j >> 6] >> (j & 63)) & 1)
					lion_container_append_sorted(res, arr[j]);
			lion_container_optimize(res);
		}
		else
		{
			/* the image holds the range's rows at this key: F's among them */
			lion_bits_to_container(m, p->ckey, tmp);
			(void) lion_container_and(tmp, p, res);
		}
		if (lion_container_cardinality(res) == 0)
			continue;

		size = lion_container_size(res);
		mat->containers[k] = (LionContainer *) palloc(size);
		memcpy(mat->containers[k], res, size);
		mat->bytes += size;
		members += lion_container_cardinality(res);
		k++;
	}

	if (k > 0)
	{
		mat->ncontainers = k;
		mat->held = sizeof(LionMatSet) + sizeof(LionContainer *) * rp->n +
			mat->bytes + (Size) k * 16;

		/*
		 * A collected set of the range's index: in recovery the count trusts
		 * the map only when that index, like every other it reads, replays
		 * under cleanup locks (lion_sources_all_rmgr()), as when each of the
		 * walk's sets was counted on its own.
		 */
		memset(&acc, 0, sizeof(acc));
		acc.index = rp->index;
		acc.attno = rp->attno;
		acc.found = true;
		acc.head = InvalidBlockNumber;
		acc.pinbuf = InvalidBuffer;
		acc.cxt = rp->cxt;
		acc.mat = mat;
		acc.matfailed = true;
		acc.ntids = members;
		acc.ncontainers = (uint32) k;
		acc.entryblk = InvalidBlockNumber;
		acc.entryoff = InvalidOffsetNumber;

		srcs = (LionCountSource *)
			palloc0(sizeof(LionCountSource) * (rp->nsources + 1));
		srcs[0].nsets = 1;
		srcs[0].sets = &acc;
		memcpy(&srcs[1], rp->sources, sizeof(LionCountSource) * rp->nsources);

		result = lion_count_sources_run(heap, snapshot, rp->nsources + 1, srcs,
										stats, cache, rel_read_only, false,
										NULL);
	}

	MemoryContextSwitchTo(oldcxt);
	return result;
}

void
lion_range_probe_end(LionRangeProbe *rp)
{
	/* the collected copy is in memory, never a file: nothing to close */
	MemoryContextDelete(rp->cxt);
}
