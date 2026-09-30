/*-------------------------------------------------------------------------
 *
 * lion_count_shared.c
 *		A copy shared by the participants of a parallel plan (LionSharedCopy).
 *
 * Part of the count engine: lion_count.h is its interface, and
 * lion_count_int.h declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_count_int.h"

/* ---------------------------------------------------------------------
 * A copy shared by the participants of a parallel plan (LionSharedCopy)
 * --------------------------------------------------------------------- */

/*
 * ONE COPY PER QUERY (DESIGN.md §27, "One copy per query").  A parallel
 * FK-side join used to have every participant collect the fact filters for
 * itself - the same merge, the same copy, as many times as there were
 * processes.  Now they collect it once, together, into the Gather's dynamic
 * shared memory, the way a Parallel Hash builds one table:
 *
 *	- COLLECTING.  The heap's container keys are cut into chunks of `width`
 *	  keys, the last one open-ended (the heap may have grown since the chunks
 *	  were cut), and each participant claims the next chunk nobody has from
 *	  an atomic counter and collects it: the merge of lion_sources_collect()
 *	  over its own located sources, sought to the chunk's first key and
 *	  stopped at its end (LionCollect.ranged).  A chunk goes into the query's
 *	  DSA when the copy's memory still has room for it, and into a file of the
 *	  plan's file set otherwise, named for the chunk, with where each
 *	  container is in it in the DSA - a Parallel Hash's batches spill the
 *	  same way.  Whoever arrives last is ELECTED;
 *	- INDEXING.  The elected participant indexes the chunks as one copy: where
 *	  every container is (LionSpillEnt, in memory or in its chunk's file), the
 *	  keys and, where they are dense, the direct index (lion_mat_index()'s),
 *	  all in the DSA.  The others wait;
 *	- DONE.  Every participant reads the copy through a view of its own
 *	  (lion_copy_view()): pointers to its containers in memory, the files of
 *	  the chunks that spilled opened read-only, and the index read where it
 *	  is.  Nothing of the copy is ever written again.
 *
 * A participant that attaches late joins whatever phase the copy is in: it
 * collects what chunks are left, waits for the index, or just reads.  One
 * that never attaches is waited for by nobody.  An error or a cancel in any
 * participant ends the query, and the leader's with it: a worker's error is
 * rethrown in the leader, whose abort terminates the other workers, and a
 * worker waiting at the barrier answers that (ConditionVariableSleep() checks
 * for interrupts); the DSA goes with the query and the files with its DSM.
 *
 * Why the copy is as safe as the serial node's (DESIGN.md §27, "Why a stale
 * copy is safe").  Every chunk is read after the query's snapshot was taken -
 * which is all the argument asks of a copy: it cannot lack a row the snapshot
 * sees - and each participant counts it only ever beside its own located fk
 * set, which carries the interlock, exactly as a copy of its own.  The chunks
 * together are the intersection: they cover every container key once, and a
 * key's containers are the merge's at that key whoever collected them.
 */
#define LION_COPY_MAX_CHUNKS		64
#define LION_COPY_CHUNKS_EACH		4	/* chunks a participant: the work evens out */

/* the phases of LionSharedCopy.barrier */
#define LION_COPY_COLLECTING		0
#define LION_COPY_INDEXING			1
#define LION_COPY_DONE				2

/*
 * The fewest container keys a range covers when the executor cuts them,
 * pg_lion.parallel_range_keys: a range seeks every source to its first key,
 * and a parallel GROUP BY walks every group's entry again for each.  A testing
 * knob rather than a tuning one - the regression suite lowers it to cut a
 * table of a few megabytes into several ranges - so the planner prices the
 * ranges of the default width, LION_PARALLEL_RANGE_KEYS, whatever it is set
 * to.
 */
int			lion_parallel_range_keys = LION_PARALLEL_RANGE_KEYS;

/*
 * Cut the heap's container keys into ranges for the participants of a
 * parallel plan: a few for each, so that one that is slow to start or has
 * denser keys is made up for by the others, and no fewer than minkeys keys
 * each.  Returns how many there are, and in *ckeys the container keys they
 * are cut from, which lion_key_range() divides among them as evenly as whole
 * keys go - no range is empty.  The last one is open-ended.  The shared
 * copy's chunks are these ranges, and so are the ranges a parallel GROUP BY
 * counts its groups over (DESIGN.md §10, "A GROUP BY in parallel").
 */
int
lion_key_ranges(BlockNumber heapblocks, int participants, int minkeys,
				uint32 *ckeys)
{
	uint64		keys = (uint64) heapblocks / LION_BLOCKS_PER_CONTAINER + 1;
	uint64		n;

	n = Min((uint64) LION_COPY_MAX_CHUNKS,
			(uint64) Max(participants, 1) * LION_COPY_CHUNKS_EACH);
	n = Min(n, Max(keys / (uint64) Max(minkeys, 1), (uint64) 1));
	*ckeys = (uint32) keys;
	return (int) n;
}

/*
 * The container keys of range r of lion_key_ranges()'s cut of ckeys keys
 * into nranges: lo up to, not including, hi.  The last range has no end - the
 * heap may have grown since the ranges were cut - and takes every key from
 * its first on (LION_KEYS_END).
 */
void
lion_key_range(int nranges, uint32 ckeys, int r, uint32 *lo, uint64 *hi)
{
	Assert(r >= 0 && r < nranges);
	*lo = (uint32) ((uint64) r * ckeys / (uint64) nranges);
	*hi = (r == nranges - 1) ? LION_KEYS_END :
		(uint64) (r + 1) * ckeys / (uint64) nranges;
}

typedef struct LionCopyChunk
{
	dsa_pointer buf;			/* in memory: its containers, back to back */
	dsa_pointer ents;			/* in its file: where each one is */
	Size		bytes;			/* its containers' bytes */
	int			ncontainers;
	uint64		members;
	bool		spilled;
} LionCopyChunk;

struct LionSharedCopy
{
	Barrier		barrier;
	pg_atomic_uint32 nextchunk;	/* the next chunk nobody has claimed */
	pg_atomic_uint64 held;		/* the chunks' bytes in shared memory */
	Size		memory;			/* what they may take */
	int			participants;
	dsm_handle	seg;			/* the plan's DSM, which a worker attaches
								 * the file set through */
	int			nchunks;
	uint32		ckeys;			/* the container keys they are cut from */

	/* the index, made by the participant elected once every chunk is in */
	int			ncontainers;
	uint64		members;
	Size		bytes;
	int			nspilled;		/* chunks in files */
	dsa_pointer ents;			/* LionSpillEnt[ncontainers] */
	dsa_pointer keys;			/* uint32[ncontainers] */
	dsa_pointer dir;			/* uint32[dirlen], or InvalidDsaPointer */
	uint32		dirbase;
	uint32		dirlen;
	int			chunkfirst[LION_COPY_MAX_CHUNKS + 1];

	SharedFileSet fileset;
	LionCopyChunk chunk[LION_COPY_MAX_CHUNKS];
};

Size
lion_shared_copy_size(void)
{
	return MAXALIGN(sizeof(LionSharedCopy));
}

/* The wait at the barrier, as pg_stat_activity names it. */
static uint32
lion_copy_wait_event(void)
{
#if PG_VERSION_NUM >= 170000
	static uint32 event = 0;

	if (event == 0)
		event = WaitEventExtensionNew("LionFactFilterCopy");
	return event;
#else
	return PG_WAIT_EXTENSION;
#endif
}

/* A chunk's file in the plan's file set. */
static void
lion_copy_chunk_name(char *name, Size len, int c)
{
	snprintf(name, len, "lioncopy.%d", c);
}

/* Cut the heap's container keys into chunks (lion_key_ranges()). */
static void
lion_copy_cut(LionSharedCopy *sc, BlockNumber heapblocks)
{
	sc->nchunks = lion_key_ranges(heapblocks, sc->participants,
								  lion_parallel_range_keys, &sc->ckeys);
}

/* The copy as it is before anyone collects it. */
static void
lion_copy_empty(LionSharedCopy *sc, BlockNumber heapblocks)
{
	BarrierInit(&sc->barrier, 0);
	pg_atomic_write_u32(&sc->nextchunk, 0);
	pg_atomic_write_u64(&sc->held, 0);
	sc->ncontainers = 0;
	sc->members = 0;
	sc->bytes = 0;
	sc->nspilled = 0;
	sc->ents = InvalidDsaPointer;
	sc->keys = InvalidDsaPointer;
	sc->dir = InvalidDsaPointer;
	sc->dirbase = 0;
	sc->dirlen = 0;
	memset(sc->chunkfirst, 0, sizeof(sc->chunkfirst));
	memset(sc->chunk, 0, sizeof(sc->chunk));
	lion_copy_cut(sc, heapblocks);
}

/*
 * In the leader, once its DSM is made.  The file set is the DSM's: its files
 * go when the last process detaches from it, whatever happened.
 */
void
lion_shared_copy_init(LionSharedCopy *sc, dsm_segment *seg, int participants,
					  Size memory, BlockNumber heapblocks)
{
	memset(sc, 0, sizeof(LionSharedCopy));
	pg_atomic_init_u32(&sc->nextchunk, 0);
	pg_atomic_init_u64(&sc->held, 0);
	sc->memory = memory;
	sc->participants = participants;
	sc->seg = dsm_segment_handle(seg);
	SharedFileSetInit(&sc->fileset, seg);
	lion_copy_empty(sc, heapblocks);
}

/* In a worker, before it runs: the file set, through the DSM it attached. */
void
lion_shared_copy_attach(LionSharedCopy *sc)
{
	dsm_segment *seg = dsm_find_mapping(sc->seg);

	if (seg == NULL)
		elog(ERROR, "lion index: a parallel worker has not attached its plan's shared memory");
	SharedFileSetAttach(&sc->fileset, seg);
}

/*
 * Between two runs, in the leader, with no participant running (the Gather
 * has ended its workers, and the leader's own view of the last copy is gone):
 * the last copy's memory and files are freed, and the next run collects
 * again - with the new parameters of a rescan, over the heap as it is now.
 */
void
lion_shared_copy_reinit(LionSharedCopy *sc, dsa_area *area,
						BlockNumber heapblocks)
{
	int			c;

	if (area != NULL)
	{
		for (c = 0; c < LION_COPY_MAX_CHUNKS; c++)
		{
			if (DsaPointerIsValid(sc->chunk[c].buf))
				dsa_free(area, sc->chunk[c].buf);
			if (DsaPointerIsValid(sc->chunk[c].ents))
				dsa_free(area, sc->chunk[c].ents);
		}
		if (DsaPointerIsValid(sc->ents))
			dsa_free(area, sc->ents);
		if (DsaPointerIsValid(sc->keys))
			dsa_free(area, sc->keys);
		if (DsaPointerIsValid(sc->dir))
			dsa_free(area, sc->dir);
	}
	SharedFileSetDeleteAll(&sc->fileset);
	lion_copy_empty(sc, heapblocks);
}

/*
 * Collect chunk c: the part of the intersection whose container keys are the
 * chunk's, into this participant's memory first - at most what the copy has
 * left of its memory - and from there into the DSA, or, past that, into the
 * chunk's file.  A chunk that holds all the copy has left of its memory is
 * held twice for a moment, here and in the DSA; a chunk is a small part of
 * the heap's keys whenever the heap is large (lion_copy_cut()).
 */
static void
lion_copy_chunk(LionSharedCopy *sc, dsa_area *area, Relation heap,
				Snapshot snapshot, int nsources, LionCountSource *sources,
				int c, LionCountStats *stats)
{
	LionCopyChunk *ch = &sc->chunk[c];
	MemoryContext cxt;
	MemoryContext oldcxt;
	LionCollect col;
	char		name[MAXPGPATH];
	uint64		held = pg_atomic_read_u64(&sc->held);
	Size		room = (held < sc->memory) ? sc->memory - (Size) held : 0;
	int			i;

	cxt = AllocSetContextCreate(CurrentMemoryContext, "lion shared copy chunk",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);
	lion_copy_chunk_name(name, sizeof(name), c);

	memset(&col, 0, sizeof(col));
	col.cxt = cxt;
	col.maxbytes = room;
	col.spill = true;
	col.sp.fileset = &sc->fileset.fs;
	col.sp.name = name;
	col.ranged = true;
	lion_key_range(sc->nchunks, sc->ckeys, c, &col.lo, &col.hi);
	col.cap = 8192;
	col.buf = (char *) palloc(col.cap);
	col.offcap = 256;
	col.offs = (Size *) palloc(sizeof(Size) * col.offcap);

	(void) lion_count_sources_run(heap, snapshot, nsources, sources, stats,
								  NULL, false, false, &col);
	ch->members = col.members;

	if (!col.spilled && col.noffs > 0)
	{
		/*
		 * Into the shared memory, when the copy's memory still has room: the
		 * room is reserved before the memory is taken, and given back if the
		 * DSA cannot give it.
		 */
		uint64		before = pg_atomic_fetch_add_u64(&sc->held, col.used);
		dsa_pointer dp = InvalidDsaPointer;

		if (before + col.used <= sc->memory)
			dp = dsa_allocate_extended(area, col.used,
									   DSA_ALLOC_HUGE | DSA_ALLOC_NO_OOM);
		if (DsaPointerIsValid(dp))
		{
			memcpy(dsa_get_address(area, dp), col.buf, col.used);
			ch->buf = dp;
			ch->bytes = col.used;
			ch->ncontainers = col.noffs;
		}
		else
		{
			/* ... else into the chunk's file after all */
			(void) pg_atomic_fetch_sub_u64(&sc->held, col.used);
			lion_spill_begin(&col.sp, cxt);
			for (i = 0; i < col.noffs; i++)
				lion_spill_add(&col.sp,
							   (const LionContainer *) (col.buf + col.offs[i]));
			col.spilled = true;
		}
	}

	if (col.spilled)
	{
		/*
		 * The file is closed, for the others to open by its name, and where
		 * each container is in it goes into the shared memory: sixteen bytes
		 * a container, which a spilled copy keeps in memory in any case.
		 */
		LionSpill  *sp = &col.sp;
		dsa_pointer dp;

		BufFileClose(sp->file);
		sp->file = NULL;
		dp = dsa_allocate_extended(area,
								   sizeof(LionSpillEnt) * Max(sp->nents, 1),
								   DSA_ALLOC_HUGE);
		memcpy(dsa_get_address(area, dp), sp->ents,
			   sizeof(LionSpillEnt) * sp->nents);
		ch->ents = dp;
		ch->ncontainers = sp->nents;
		ch->bytes = 0;
		for (i = 0; i < sp->nents; i++)
			ch->bytes += sp->ents[i].size;
		ch->spilled = true;
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);
}

/*
 * The elected participant's part, once every chunk is in: where every
 * container of the copy is, in memory (fileno -1, and its offset in its
 * chunk's memory) or in its chunk's file, its key, and the direct index where
 * the keys are dense - what lion_mat_index() makes of a copy of one process.
 * The chunks cover ascending ranges of keys and each is in key order, so the
 * keys ascend across them.
 */
static void
lion_copy_index(LionSharedCopy *sc, dsa_area *area)
{
	LionSpillEnt *ents;
	uint32	   *keys;
	int			n = 0;
	int			c;
	int			i;
	int			j;

	for (c = 0; c < sc->nchunks; c++)
	{
		sc->chunkfirst[c] = n;
		n += sc->chunk[c].ncontainers;
		sc->members += sc->chunk[c].members;
		sc->bytes += sc->chunk[c].bytes;
		if (sc->chunk[c].spilled)
			sc->nspilled++;
	}
	sc->chunkfirst[sc->nchunks] = n;
	sc->ncontainers = n;
	if (n == 0)
		return;

	sc->ents = dsa_allocate_extended(area, sizeof(LionSpillEnt) * n,
									 DSA_ALLOC_HUGE);
	sc->keys = dsa_allocate_extended(area, sizeof(uint32) * n, DSA_ALLOC_HUGE);
	ents = (LionSpillEnt *) dsa_get_address(area, sc->ents);
	keys = (uint32 *) dsa_get_address(area, sc->keys);

	i = 0;
	for (c = 0; c < sc->nchunks; c++)
	{
		LionCopyChunk *ch = &sc->chunk[c];

		if (ch->spilled)
		{
			const LionSpillEnt *src = (const LionSpillEnt *)
				dsa_get_address(area, ch->ents);

			for (j = 0; j < ch->ncontainers; j++, i++)
			{
				ents[i] = src[j];
				keys[i] = src[j].ckey;
			}
		}
		else if (ch->ncontainers > 0)
		{
			const char *base = (const char *) dsa_get_address(area, ch->buf);
			Size		off = 0;

			for (j = 0; j < ch->ncontainers; j++, i++)
			{
				const LionContainer *item = (const LionContainer *) (base + off);
				Size		size = lion_item_size(item);

				ents[i].off = (pgoff_t) off;
				ents[i].ckey = item->ckey;
				ents[i].size = (uint16) size;
				ents[i].fileno = -1;
				keys[i] = item->ckey;
				off += MAXALIGN(size);
			}
			Assert(off == ch->bytes);
		}
	}
	Assert(i == n);

	for (i = 1; i < n; i++)
	{
		if (keys[i] <= keys[i - 1])
			elog(ERROR, "lion index: the chunks of a shared copy are out of key order");
	}

	if (n >= 2 &&
		(uint64) keys[n - 1] - keys[0] + 1 <= (uint64) n * LION_MAT_DIR_SPREAD)
	{
		uint32		first = keys[0];
		uint32	   *dir;
		uint64		k = 0;

		sc->dirlen = keys[n - 1] - first + 1;
		sc->dir = dsa_allocate_extended(area, sizeof(uint32) * sc->dirlen,
										DSA_ALLOC_HUGE);
		dir = (uint32 *) dsa_get_address(area, sc->dir);
		for (i = 0; i < n; i++)
		{
			while ((uint64) first + k <= keys[i])
				dir[k++] = (uint32) i;
		}
		Assert(k == sc->dirlen);
		sc->dirbase = first;
	}
}

/*
 * A participant's view of the finished copy, in the current memory context:
 * a posting set as lion_sources_collect() makes one - found, pinless, of no
 * index entry - whose containers are read where they are.  An empty copy is a
 * set that selects nothing, not found.  The files of the chunks that spilled
 * are opened here and closed by lion_posting_set_release(); the copy's first
 * container, which every cursor over it reads when it is built, is kept in
 * this process's memory when it is in a file.
 */
static void
lion_copy_view(LionSharedCopy *sc, dsa_area *area, Relation index,
			   LionPostingSet *out)
{
	LionMatSet *mat;
	LionSpillEnt *ents;
	int			n = sc->ncontainers;
	int			c;
	int			i;

	memset(out, 0, sizeof(LionPostingSet));
	out->pinbuf = InvalidBuffer;
	out->head = InvalidBlockNumber;
	out->entryblk = InvalidBlockNumber;
	out->entryoff = InvalidOffsetNumber;
	out->index = index;
	out->attno = 1;
	out->cxt = CurrentMemoryContext;
	if (n == 0)
		return;

	ents = (LionSpillEnt *) dsa_get_address(area, sc->ents);
	mat = (LionMatSet *) palloc0(sizeof(LionMatSet));
	mat->ncontainers = n;
	mat->bytes = sc->bytes;
	mat->spill = ents;
	mat->keys = (uint32 *) dsa_get_address(area, sc->keys);
	if (DsaPointerIsValid(sc->dir))
	{
		mat->dir = (uint32 *) dsa_get_address(area, sc->dir);
		mat->dirbase = sc->dirbase;
		mat->dirlen = sc->dirlen;
	}
	mat->containers = (LionContainer **) palloc(sizeof(LionContainer *) * n);
	mat->nchunks = sc->nchunks;
	mat->chunkfirst = sc->chunkfirst;
	mat->held = sizeof(LionMatSet) + sizeof(LionContainer *) * n;
	if (sc->nspilled > 0)
	{
		mat->files = (BufFile **) palloc0(sizeof(BufFile *) * sc->nchunks);
		mat->held += sizeof(BufFile *) * sc->nchunks;
	}

	for (c = 0; c < sc->nchunks; c++)
	{
		LionCopyChunk *ch = &sc->chunk[c];

		if (!ch->spilled)
		{
			char	   *base = (ch->ncontainers > 0) ?
				(char *) dsa_get_address(area, ch->buf) : NULL;

			for (i = sc->chunkfirst[c]; i < sc->chunkfirst[c + 1]; i++)
				mat->containers[i] = (LionContainer *) (base + ents[i].off);
		}
		else
		{
			char		name[MAXPGPATH];

			for (i = sc->chunkfirst[c]; i < sc->chunkfirst[c + 1]; i++)
				mat->containers[i] = NULL;
			lion_copy_chunk_name(name, sizeof(name), c);
			mat->files[c] = BufFileOpenFileSet(&sc->fileset.fs, name,
											   O_RDONLY, false);
		}
	}
	if (mat->containers[0] == NULL)
	{
		Size		size = MAXALIGN(Max((Size) ents[0].size, LION_CONTAINER_HDRSZ));

		mat->first = (LionContainer *) palloc(size);
		lion_spill_read(mat, 0, mat->first);
		mat->held += size;
	}
	mat->collected = true;

	out->found = true;
	out->mat = mat;
	out->ntids = sc->members;
	out->ncontainers = (uint32) n;
}

void
lion_shared_copy_collect(LionSharedCopy *sc, dsa_area *area, Relation heap,
						 Snapshot snapshot, int nsources,
						 LionCountSource *sources, LionPostingSet *out,
						 int *chunks, bool *built, bool *spilled,
						 LionCountStats *stats)
{
	Relation	index = NULL;
	int			phase;
	int			i;
	int			j;

	*chunks = 0;
	*built = false;

	/* the index the copy is named after, as lion_sources_collect() names it */
	for (i = 0; i < nsources && index == NULL; i++)
	{
		for (j = 0; j < sources[i].nsets; j++)
		{
			if (sources[i].sets[j].found)
			{
				index = sources[i].sets[j].index;
				break;
			}
		}
	}

	phase = BarrierAttach(&sc->barrier);
	if (phase == LION_COPY_COLLECTING)
	{
		for (;;)
		{
			uint32		c = pg_atomic_fetch_add_u32(&sc->nextchunk, 1);

			if (c >= (uint32) sc->nchunks)
				break;
			lion_copy_chunk(sc, area, heap, snapshot, nsources, sources,
							(int) c, stats);
			(*chunks)++;
			CHECK_FOR_INTERRUPTS();
		}
		if (BarrierArriveAndWait(&sc->barrier, lion_copy_wait_event()))
		{
			lion_copy_index(sc, area);
			*built = true;
		}
		phase = LION_COPY_INDEXING;
	}
	if (phase == LION_COPY_INDEXING)
		(void) BarrierArriveAndWait(&sc->barrier, lion_copy_wait_event());
	BarrierDetach(&sc->barrier);

	*spilled = (sc->nspilled > 0);
	lion_copy_view(sc, area, index, out);
}
