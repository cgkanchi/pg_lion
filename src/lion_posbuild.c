/*-------------------------------------------------------------------------
 *
 * lion_posbuild.c
 *		The build's positions (DESIGN.md §17, "Stored positions"): what a
 *		CREATE INDEX writes for a column whose opclass stores them.
 *
 * The build's spool keeps only codes (lion_spool.c), so a column that stores
 * positions hands each key's positions in a row to the sink here, which
 * APPENDS them to a stream of the key's own, found through a hash table of
 * the column's keys by their bytes: a heap scan delivers a key's rows in
 * ascending TID order, so a key's stream needs no sorting, as the spool's
 * posting sets need none.  A member in a stream is its code, its number of
 * positions (varbyte) and the positions as the opclass gave them.
 *
 * MEMORY is a share of the build's (lion_posbuild_budget()), which the spool
 * does without, and it counts everything the scan keeps here: the streams,
 * the keys and the table that finds them.  When they outgrow it, the keys
 * are sorted into the order the build writes its entries in - the column,
 * then the key's comparison - and written with their streams to a logical
 * tape as one RUN, and all of it is forgotten: a key the scan meets again
 * starts a new stream.  So what the scan holds depends on the budget alone,
 * not on the keys the column has nor on the runs it has written.
 *
 * When the build writes a key's entry it takes the key's positions here
 * (lion_posbuild_take()), and as it writes the entries in that same order,
 * the runs are MERGED as it goes, k-way, each read front to back: a key's
 * positions are its records in the runs, oldest first, which is the order
 * the scan delivered them in.  One merge reads at most maxorder runs, so the
 * scan merges its runs into fewer as it writes them, always runs that follow
 * one another, so that the order holds (lion_posbuild_cascade()); the runs
 * it keeps are few however many it writes.  Without a run, the keys in
 * memory are sorted and read where they are.  The one disorder the scan
 * has - a heap-only tuple reported under its HOT chain's root offset, within
 * one heap page (lion_spool.c) - is undone by sorting each heap page's
 * members as they are read, which also covers a page whose members a spill
 * split between two runs.  The positions come back
 *
 *	- as one chunk when they fit one, which the entry carries when it is
 *	  INLINE and which becomes a one-leaf position tree when it is a CHAIN;
 *	- as a position tree, written bottom up through the build's own page
 *	  writer as they are read, and the entry has to be a CHAIN.
 *
 * A key the build writes no entry for, or one whose bytes differ from the
 * entry's, is an ERROR: the merge meets it before the key the build asks
 * for, or at the column's end.  Equal keys whose bytes differ (a
 * nondeterministic collation) cannot store positions.
 *
 * A parallel build's participants each run the sink on their share of the
 * heap, and the leader takes a key's positions from all of them (see "A
 * parallel build" below).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/tablespace.h"
#include "common/hashfn.h"
#include "lib/binaryheap.h"
#include "miscadmin.h"
#include "storage/buffile.h"
#include "storage/shmem.h"
#include "utils/logtape.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/sortsupport.h"
#include "varatt.h"

#include "lion.h"
#include "lion_positions.h"
#include "lion_spool.h"

/* A stream's memory comes in blocks, the first this small, doubling up to
 * the largest. */
#define LION_POSB_FIRST_BLOCK	32
#define LION_POSB_MAX_BLOCK		8192

/*
 * Keys, their streams' blocks and the like are carved out of arena blocks of
 * at most this size; the arena block scales with the budget, so that a small
 * one is not gone in one block.
 */
#define LION_POSB_ARENA			((Size) 64 * 1024)

/* A column's table of keys starts with this many buckets. */
#define LION_POSB_FIRST_BUCKETS	256

/*
 * A run holds a key's stream in chunks of whole members, at most this many
 * bytes each: no block of a stream in memory is larger, and a member always
 * fits one.
 */
#define LION_POSRUN_CHUNK		LION_POSB_MAX_BLOCK

StaticAssertDecl(LION_POS_VARINT_MAX * 2 + sizeof(uint16) * LION_POS_MAX_NPOS <=
				 LION_POSB_MAX_BLOCK,
				 "a stream's member fits a block");

/* Where a run's record would have its column: the end of the run. */
#define LION_POSRUN_END			(-1)

/* The most members one key has on one heap page. */
#define LION_PAGE_CODES_MAX		(1 << LION_OFFSET_BITS)

typedef struct LionPosBlock
{
	struct LionPosBlock *next;
	uint32		used;
	uint32		cap;
	uint8		data[FLEXIBLE_ARRAY_MEMBER];
} LionPosBlock;

/* A key the scan has met since the last spill, and its stream. */
typedef struct LionPosKey
{
	struct LionPosKey *next;	/* in its hash bucket */
	uint32		hash;			/* hash_bytes() of the key's bytes */
	text	   *key;			/* the bytes, behind a 4-byte header */
	LionPosBlock *head;			/* oldest first */
	LionPosBlock *tail;
} LionPosKey;

typedef struct LionPosCol
{
	SortSupportData ssup;		/* the comparison the build orders keys by */
	LionPosKey **buckets;		/* in datacxt, as everything below */
	uint32		nbuckets;		/* a power of two */
	LionPosKey **keys;			/* in the order first seen; sorted at a spill */
	int64		nkeys;
	int64		maxkeys;
} LionPosCol;

/*
 * A run is a sequence of records in the order the build takes keys in, each
 * a header, the key's bytes, and its stream in chunks: a uint32 length, then
 * that many bytes of whole members; a zero length ends the record.  A header
 * whose column is LION_POSRUN_END ends the run.
 */
typedef struct LionPosRecord
{
	int32		col;
	uint32		keylen;
} LionPosRecord;

/* Where a run goes: a tape of the build's set, or a participant's file. */
typedef struct LionPosWriter
{
	LogicalTape *tape;
	BufFile    *file;
	uint32		len;			/* the bytes of the chunk being filled */
	uint8		buf[LION_POSRUN_CHUNK];
} LionPosWriter;

/*
 * One input of a merge: a run on a tape, a parallel participant's file, or
 * the keys in memory, sorted.  It is always on a record - one key - or at
 * its end (eos), and within the record on a chunk of members, which p..end
 * is what is left of.
 */
typedef struct LionPosCursor
{
	int			index;			/* among the merge's inputs: older first */
	MemoryContext cxt;			/* what the cursor allocates lives here */
	LogicalTape *tape;
	BufFile    *file;
	/* the keys in memory, when neither: where the cursor is in them */
	int			mcol;
	int64		mkey;
	LionPosBlock *mblock;		/* the record's next block */

	/* the current record */
	bool		eos;
	int			col;
	text	   *key;			/* with a 4-byte header */
	bool		more;			/* the record has chunks left */
	char	   *keybuf;			/* a run's key */
	Size		keycap;

	/* the current chunk */
	uint8	   *chunk;			/* a run's: LION_POSRUN_CHUNK bytes */
	const uint8 *p;
	const uint8 *end;

	/* a parallel leader's merge by heap block: the member at the head */
	bool		has;
	uint64		code;
	uint32		npos;
	const uint8 *pos;
} LionPosCursor;

/* The merge the build takes keys from. */
typedef struct LionPosMerge
{
	LionPosCursor **in;			/* older first */
	int			nin;
	binaryheap *heap;			/* those not at their end, the smallest first */
	LionPosCursor **grp;		/* those holding the key being taken */
	bool		byblock;		/* a parallel leader's: a key's members are
								 * merged by heap block */
} LionPosMerge;

struct LionPosBuild
{
	Relation	index;
	int			ncols;
	LionPosCol **cols;			/* per key column; NULL when it stores none */
	MemoryContext cxt;			/* this, the runs and the merge */
	MemoryContext datacxt;		/* the keys, their tables, their streams */
	MemoryContext mergecxt;		/* a merge into a run, reset after it */
	Size		budget;
	Size		emptysize;		/* what datacxt holds with nothing in it */
	char	   *arena;			/* where the next small allocation is carved */
	Size		arenafree;
	Size		arenablock;
	/* the runs, in a tape set of their own */
	LogicalTapeSet *tapes;		/* created at the first spill */
	LogicalTape **runs;			/* oldest first */
	int		   *levels;			/* each run's level: never up, oldest first */
	int			nruns;
	int			maxruns;
	int			nspills;
	int			nmerges;		/* runs merged into one before the last merge */
	Size		readbuf;		/* the read buffer of one run in a merge */
	int			maxorder;		/* the most runs one merge reads at once */
	/* a parallel build's participant: its output is the build's shared file */
	SharedFileSet *fileset;
	int			filenum;
	/* a parallel build's leader: every participant's */
	int			nsrcs;
	BufFile   **srcfiles;
	/* taking a key's positions */
	LionPosMerge *merge;		/* set up when the first column is written */
	int			col;
	LionContainer *chunk;
	LionPosMember *page;		/* one heap page's members */
	int			npage;
	int			maxpage;
};

static void lion_posbuild_reset(LionPosBuild *pb);

/*
 * What the positions take of the build's workmem kB, which the spool does
 * without (lion_build.c): half, and never less than 64kB.  A row's positions
 * for a key are several times the bytes its code takes in the spool, so this
 * is where memory saves the most spilling.
 */
static Size
lion_posbuild_share(int workmem)
{
	return Max((Size) workmem * 1024 / 2, (Size) 64 * 1024);
}

LionPosBuild *
lion_posbuild_begin(Relation index, LionIndexState *ix, int workmem)
{
	LionPosBuild *pb;
	MemoryContext cxt;
	MemoryContext old;
	int			c;
	bool		any = false;

	for (c = 0; c < ix->ncolumns; c++)
		any |= ix->cols[c].positions;
	if (!any)
		return NULL;

	cxt = AllocSetContextCreate(CurrentMemoryContext, "lion positions build",
								ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cxt);
	pb = (LionPosBuild *) palloc0(sizeof(LionPosBuild));
	pb->index = index;
	pb->ncols = ix->ncolumns;
	pb->cxt = cxt;
	pb->budget = lion_posbuild_share(workmem);

	/*
	 * Blocks sized to the budget, as the spool's: a context's blocks double
	 * up to its largest size, and a small budget would otherwise be gone in
	 * one block.
	 */
	pb->datacxt = AllocSetContextCreate(cxt, "lion positions streams",
										ALLOCSET_DEFAULT_MINSIZE,
										ALLOCSET_DEFAULT_INITSIZE,
										Max(ALLOCSET_DEFAULT_INITSIZE,
											Min(ALLOCSET_DEFAULT_MAXSIZE,
												pb->budget / 16)));
	pb->mergecxt = AllocSetContextCreate(cxt, "lion positions merge",
										 ALLOCSET_DEFAULT_SIZES);
	pb->arenablock = Max((Size) 1024, Min(LION_POSB_ARENA, pb->budget / 16));

	/*
	 * A merge reads each run through a read buffer and a chunk, and reads at
	 * most maxorder of them at once, so that one merge's buffers take no more
	 * than half the budget, as the spool's do; the runs are merged into fewer
	 * as the scan writes them (lion_posbuild_cascade()) and before the last
	 * merge (lion_posbuild_reduce_runs()).
	 */
	pb->readbuf = Max((Size) BLCKSZ, Min((Size) BLCKSZ * 8, pb->budget / 16));
	pb->maxorder = (int) Max(6, Min(512, pb->budget / 2 /
									(pb->readbuf + LION_POSRUN_CHUNK)));
	pb->maxruns = 8;
	pb->runs = (LogicalTape **) palloc(sizeof(LogicalTape *) * pb->maxruns);
	pb->levels = (int *) palloc(sizeof(int) * pb->maxruns);
	pb->col = -1;
	pb->cols = (LionPosCol **) palloc0(sizeof(LionPosCol *) * pb->ncols);
	for (c = 0; c < ix->ncolumns; c++)
	{
		LionState  *cs = &ix->cols[c];
		LionPosCol *pc;

		if (!cs->positions)
			continue;

		/*
		 * The keys are looked up by their bytes, which are text's, and put
		 * in the order of the column's comparison, which is the order the
		 * build writes them in (lion_spool.c).
		 */
		if (cs->typid != TEXTOID || !cs->ordered)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("lion index \"%s\": only a text key column ordered by its comparison can store positions",
							RelationGetRelationName(index))));
		pc = (LionPosCol *) palloc0(sizeof(LionPosCol));
		pc->ssup.ssup_cxt = cxt;
		pc->ssup.ssup_collation = cs->collation;
		pc->ssup.ssup_nulls_first = false;
		pc->ssup.abbreviate = false;
		PrepareSortSupportFromOrderingOp(cs->ltopr, &pc->ssup);
		pb->cols[c] = pc;
	}
	lion_posbuild_reset(pb);
	MemoryContextSwitchTo(old);
	return pb;
}

/* The share of the build's memory the positions take: 0 without any. */
Size
lion_posbuild_budget(const LionPosBuild *pb)
{
	return (pb == NULL) ? 0 : pb->budget;
}

/* Forget the keys and their streams: after a spill, and to begin with. */
static void
lion_posbuild_reset(LionPosBuild *pb)
{
	MemoryContext old;
	int			c;

	MemoryContextReset(pb->datacxt);
	pb->arena = NULL;
	pb->arenafree = 0;
	old = MemoryContextSwitchTo(pb->datacxt);
	for (c = 0; c < pb->ncols; c++)
	{
		LionPosCol *pc = pb->cols[c];

		if (pc == NULL)
			continue;
		pc->nbuckets = LION_POSB_FIRST_BUCKETS;
		pc->buckets = (LionPosKey **)
			palloc0(sizeof(LionPosKey *) * pc->nbuckets);
		pc->maxkeys = LION_POSB_FIRST_BUCKETS;
		pc->keys = (LionPosKey **) palloc(sizeof(LionPosKey *) * pc->maxkeys);
		pc->nkeys = 0;
	}
	MemoryContextSwitchTo(old);
	pb->emptysize = MemoryContextMemAllocated(pb->datacxt, false);
}

/*
 * What the keys and their streams take: the empty tables and the context's
 * first block are not counted, as the spool does not count its own.
 */
static inline Size
lion_posbuild_used(LionPosBuild *pb)
{
	Size		size = MemoryContextMemAllocated(pb->datacxt, false);

	return (size > pb->emptysize) ? size - pb->emptysize : 0;
}

/* The keys in memory, of every column. */
static int64
lion_posbuild_nkeys(LionPosBuild *pb)
{
	int64		n = 0;
	int			c;

	for (c = 0; c < pb->ncols; c++)
	{
		if (pb->cols[c] != NULL)
			n += pb->cols[c]->nkeys;
	}
	return n;
}

/*
 * size bytes in datacxt, carved out of arena blocks when small rather than
 * allocated one by one: most keys of a text column have a block of a few
 * members, and a palloc header for each would be a good part of it.
 */
static void *
lion_posbuild_alloc(LionPosBuild *pb, Size size)
{
	void	   *p;

	size = MAXALIGN(size);
	if (size > pb->arenablock / 4)
		return MemoryContextAlloc(pb->datacxt, size);
	if (pb->arenafree < size)
	{
		pb->arena = (char *) MemoryContextAlloc(pb->datacxt, pb->arenablock);
		pb->arenafree = pb->arenablock;
	}
	p = pb->arena;
	pb->arena += size;
	pb->arenafree -= size;
	return p;
}

static LionPosKey *
lion_posbuild_lookup(LionPosCol *pc, const char *bytes, uint32 len,
					 uint32 hash)
{
	LionPosKey *k;

	for (k = pc->buckets[hash & (pc->nbuckets - 1)]; k != NULL; k = k->next)
	{
		if (k->hash == hash && VARSIZE(k->key) - VARHDRSZ == len &&
			memcmp(VARDATA(k->key), bytes, len) == 0)
			return k;
	}
	return NULL;
}

/* A key met for the first time since the last spill. */
static LionPosKey *
lion_posbuild_new_key(LionPosBuild *pb, LionPosCol *pc, const char *bytes,
					  uint32 len, uint32 hash)
{
	LionPosKey *k = (LionPosKey *) lion_posbuild_alloc(pb, sizeof(LionPosKey));
	uint32		b;

	k->hash = hash;
	k->key = (text *) lion_posbuild_alloc(pb, VARHDRSZ + (Size) len);
	SET_VARSIZE(k->key, VARHDRSZ + len);
	memcpy(VARDATA(k->key), bytes, len);
	k->head = k->tail = NULL;

	if (pc->nkeys >= pc->maxkeys)
	{
		pc->maxkeys *= 2;
		pc->keys = (LionPosKey **)
			repalloc_huge(pc->keys, mul_size(sizeof(LionPosKey *),
											 (Size) pc->maxkeys));
	}
	pc->keys[pc->nkeys++] = k;

	/* twice the keys' buckets, rehashed */
	if ((uint64) pc->nkeys > pc->nbuckets && pc->nbuckets < (PG_UINT32_MAX >> 2))
	{
		uint32		nb = pc->nbuckets * 2;
		LionPosKey **buckets = (LionPosKey **)
			MemoryContextAllocExtended(pb->datacxt, sizeof(LionPosKey *) * nb,
									   MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);
		int64		i;

		for (i = 0; i < pc->nkeys - 1; i++)
		{
			LionPosKey *o = pc->keys[i];

			o->next = buckets[o->hash & (nb - 1)];
			buckets[o->hash & (nb - 1)] = o;
		}
		pfree(pc->buckets);
		pc->buckets = buckets;
		pc->nbuckets = nb;
	}
	b = hash & (pc->nbuckets - 1);
	k->next = pc->buckets[b];
	pc->buckets[b] = k;
	return k;
}

/* Room for need more bytes at the end of k's stream in memory. */
static uint8 *
lion_posbuild_room(LionPosBuild *pb, LionPosKey *k, Size need)
{
	LionPosBlock *t = k->tail;

	if (t == NULL || t->cap - t->used < need)
	{
		Size		cap = (t == NULL) ? LION_POSB_FIRST_BLOCK :
			Min((Size) t->cap * 2, (Size) LION_POSB_MAX_BLOCK);
		LionPosBlock *nb;

		cap = Max(cap, need);
		Assert(cap <= LION_POSB_MAX_BLOCK);
		nb = (LionPosBlock *) lion_posbuild_alloc(pb,
												  offsetof(LionPosBlock, data) + cap);
		nb->next = NULL;
		nb->used = 0;
		nb->cap = (uint32) cap;
		if (t == NULL)
			k->head = nb;
		else
			t->next = nb;
		k->tail = nb;
		t = nb;
	}
	return t->data + t->used;
}

static inline uint8 *
lion_posbuild_put_varbyte(uint8 *p, uint64 v)
{
	while (v >= 0x80)
	{
		*p++ = (uint8) (v | 0x80);
		v >>= 7;
	}
	*p++ = (uint8) v;
	return p;
}

static inline Size
lion_posbuild_varbyte_len(uint64 v)
{
	Size		n = 1;

	while (v >= 0x80)
	{
		v >>= 7;
		n++;
	}
	return n;
}

static inline uint64
lion_posbuild_get_varbyte(const uint8 **pp, const uint8 *end)
{
	const uint8 *p = *pp;
	uint64		v = 0;
	int			shift = 0;

	while (p < end && shift < 64)
	{
		uint8		b = *p++;

		v |= (uint64) (b & 0x7F) << shift;
		if ((b & 0x80) == 0)
		{
			*pp = p;
			return v;
		}
		shift += 7;
	}
	elog(ERROR, "lion index: a position stream of the build is damaged");
	return 0;					/* keep compiler quiet */
}

/*
 * The order the build writes a column's entries in (lion_spool.c): the
 * column's comparison.  Two keys it ties are equal keys whose bytes differ,
 * which only a nondeterministic collation has and which cannot store
 * positions (see the file header); the bytes put them in some fixed order
 * all the same, which is all a merge needs.
 */
static int
lion_posbuild_key_cmp(LionPosCol *pc, const text *a, const text *b)
{
	Size		la = VARSIZE_ANY_EXHDR(a);
	Size		lb = VARSIZE_ANY_EXHDR(b);
	int			c;

	c = ApplySortComparator(PointerGetDatum(a), false,
							PointerGetDatum(b), false, &pc->ssup);
	if (c != 0)
		return c;
	c = memcmp(VARDATA_ANY(a), VARDATA_ANY(b), Min(la, lb));
	if (c != 0)
		return c < 0 ? -1 : 1;
	return (la > lb) - (la < lb);
}

static int
lion_posbuild_sort_cmp(const void *a, const void *b, void *arg)
{
	return lion_posbuild_key_cmp((LionPosCol *) arg,
								 (*(LionPosKey *const *) a)->key,
								 (*(LionPosKey *const *) b)->key);
}

/* A column's keys in memory, in the order the build takes them. */
static void
lion_posbuild_sort(LionPosCol *pc)
{
	if (pc->nkeys > 1)
		qsort_arg(pc->keys, (size_t) pc->nkeys, sizeof(LionPosKey *),
				  lion_posbuild_sort_cmp, pc);
}

/* ---------------------------------------------------------------------
 * Runs
 * --------------------------------------------------------------------- */

static void
lion_posrun_write(LionPosWriter *w, const void *p, Size len)
{
	if (w->tape != NULL)
		LogicalTapeWrite(w->tape, p, len);
	else
		BufFileWrite(w->file, p, len);
}

static void
lion_posrun_begin_record(LionPosWriter *w, int col, const text *key)
{
	LionPosRecord r;

	r.col = col;
	r.keylen = (uint32) VARSIZE_ANY_EXHDR(key);
	lion_posrun_write(w, &r, sizeof(r));
	lion_posrun_write(w, VARDATA_ANY(key), r.keylen);
	w->len = 0;
}

static void
lion_posrun_flush(LionPosWriter *w)
{
	uint32		n = w->len;

	if (n == 0)
		return;
	lion_posrun_write(w, &n, sizeof(n));
	lion_posrun_write(w, w->buf, n);
	w->len = 0;
}

/* len bytes of whole members: onto the chunk being filled, or the next. */
static void
lion_posrun_put(LionPosWriter *w, const uint8 *p, Size len)
{
	if (len > LION_POSRUN_CHUNK)
		elog(ERROR, "lion index: a position stream of the build is damaged");
	if (w->len + len > LION_POSRUN_CHUNK)
		lion_posrun_flush(w);
	memcpy(w->buf + w->len, p, len);
	w->len += (uint32) len;
}

static void
lion_posrun_end_record(LionPosWriter *w)
{
	uint32		zero = 0;

	lion_posrun_flush(w);
	lion_posrun_write(w, &zero, sizeof(zero));
}

static void
lion_posrun_end(LionPosWriter *w)
{
	LionPosRecord r;

	r.col = LION_POSRUN_END;
	r.keylen = 0;
	lion_posrun_write(w, &r, sizeof(r));
}

static LionPosWriter *
lion_posrun_writer(LogicalTape *tape, BufFile *file)
{
	LionPosWriter *w = (LionPosWriter *) palloc(sizeof(LionPosWriter));

	w->tape = tape;
	w->file = file;
	w->len = 0;
	return w;
}

/* Every key in memory, with its stream, in the order the build takes them. */
static void
lion_posbuild_write_memory(LionPosBuild *pb, LionPosWriter *w)
{
	int			c;

	for (c = 0; c < pb->ncols; c++)
	{
		LionPosCol *pc = pb->cols[c];
		int64		i;

		if (pc == NULL)
			continue;
		lion_posbuild_sort(pc);
		for (i = 0; i < pc->nkeys; i++)
		{
			LionPosKey *k = pc->keys[i];
			LionPosBlock *b;

			lion_posrun_begin_record(w, c, k->key);
			for (b = k->head; b != NULL; b = b->next)
				lion_posrun_put(w, b->data, b->used);
			lion_posrun_end_record(w);
			if ((i & 1023) == 1023)
				CHECK_FOR_INTERRUPTS();
		}
	}
	lion_posrun_end(w);
}

static void
lion_posbuild_tapes(LionPosBuild *pb)
{
	if (pb->tapes == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(pb->cxt);

		PrepareTempTablespaces();
		pb->tapes = LogicalTapeSetCreate(false, NULL, -1);
		MemoryContextSwitchTo(old);
	}
}

/*
 * A new tape in the set.  Its struct, and the write buffer the first write
 * allocates, belong to pb->cxt: a tape outlives the merge that writes it.
 * (The read buffer a tape allocates at its first read is freed by
 * LogicalTapeClose(), which every merge into a run does before its context
 * goes.)
 */
static LogicalTape *
lion_posbuild_new_tape(LionPosBuild *pb)
{
	MemoryContext old = MemoryContextSwitchTo(pb->cxt);
	LogicalTape *tape;

	lion_posbuild_tapes(pb);
	tape = LogicalTapeCreate(pb->tapes);
	LogicalTapeWrite(tape, NULL, 0);
	MemoryContextSwitchTo(old);
	return tape;
}

static void
lion_posbuild_add_run(LionPosBuild *pb, LogicalTape *tape, int level)
{
	Assert(pb->nruns == 0 || pb->levels[pb->nruns - 1] >= level);
	if (pb->nruns >= pb->maxruns)
	{
		pb->maxruns *= 2;
		pb->runs = (LogicalTape **)
			repalloc(pb->runs, mul_size(sizeof(LogicalTape *),
										(Size) pb->maxruns));
		pb->levels = (int *)
			repalloc(pb->levels, mul_size(sizeof(int), (Size) pb->maxruns));
	}
	pb->runs[pb->nruns] = tape;
	pb->levels[pb->nruns] = level;
	pb->nruns++;
}

static LogicalTape *lion_posbuild_merge_runs(LionPosBuild *pb, int first,
											 int k);

/*
 * The runs are in LEVELS, as an LSM tree's are: a spill adds a run of level
 * 0, and maxorder runs of one level are merged into one of the next.  The
 * levels never go up from the oldest run to the newest, so the runs a merge
 * reads follow one another and the order holds; every byte is copied once a
 * level, and fewer than maxorder runs a level are held, however many spills
 * there are.
 */
static void
lion_posbuild_cascade(LionPosBuild *pb)
{
	while (pb->nruns >= pb->maxorder)
	{
		int			first = pb->nruns - pb->maxorder;
		int			level = pb->levels[pb->nruns - 1];
		LogicalTape *tape;

		if (pb->levels[first] != level)
			break;
		tape = lion_posbuild_merge_runs(pb, first, pb->maxorder);
		pb->nruns = first;
		lion_posbuild_add_run(pb, tape, level + 1);
	}
}

/* Write every key in memory to a new run, and forget them all. */
static void
lion_posbuild_spill(LionPosBuild *pb)
{
	MemoryContext old;
	LogicalTape *tape;

	tape = lion_posbuild_new_tape(pb);
	old = MemoryContextSwitchTo(pb->mergecxt);
	lion_posbuild_write_memory(pb, lion_posrun_writer(tape, NULL));
	/* the write buffer goes now, the read buffer comes with the first read */
	LogicalTapeRewindForRead(tape, pb->readbuf);
	MemoryContextSwitchTo(old);
	MemoryContextReset(pb->mergecxt);

	lion_posbuild_reset(pb);
	pb->nspills++;
	lion_posbuild_add_run(pb, tape, 0);
	lion_posbuild_cascade(pb);
}

/* LionSpoolPosSink: one key's positions in one row. */
void
lion_posbuild_sink(void *arg, int col, Datum key, uint64 code,
				   const LionKeyPositions *kp)
{
	LionPosBuild *pb = (LionPosBuild *) arg;
	LionPosCol *pc = pb->cols[col];
	text	   *t = DatumGetTextPP(key);
	const char *bytes = VARDATA_ANY(t);
	uint32		len = VARSIZE_ANY_EXHDR(t);
	uint32		hash;
	uint32		npos = Min((uint32) kp->npos, (uint32) LION_POS_MAX_NPOS);
	LionPosKey *k;
	uint8	   *start;
	uint8	   *p;

	Assert(pc != NULL);

	/* what the spool refuses next anyway, before a run has to hold it */
	if ((Size) len + VARHDRSZ > LION_MAX_KEY_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("lion index key size %zu exceeds maximum %d",
						(Size) len + VARHDRSZ, LION_MAX_KEY_SIZE)));

	hash = hash_bytes((const unsigned char *) bytes, (int) len);
	k = lion_posbuild_lookup(pc, bytes, len, hash);
	if (k == NULL)
		k = lion_posbuild_new_key(pb, pc, bytes, len, hash);

	start = p = lion_posbuild_room(pb, k,
								   lion_posbuild_varbyte_len(code) +
								   lion_posbuild_varbyte_len(npos) +
								   sizeof(uint16) * npos);
	p = lion_posbuild_put_varbyte(p, code);
	p = lion_posbuild_put_varbyte(p, npos);
	if (npos > 0)
	{
		memcpy(p, kp->pos, sizeof(uint16) * npos);
		p += sizeof(uint16) * npos;
	}
	k->tail->used += (uint32) (p - start);

	if (lion_posbuild_used(pb) > pb->budget)
		lion_posbuild_spill(pb);
}

/* ---------------------------------------------------------------------
 * Cursors and merges
 * --------------------------------------------------------------------- */

static void
lion_poscur_read(LionPosCursor *c, void *p, Size len)
{
	if (c->tape != NULL)
	{
		if (LogicalTapeRead(c->tape, p, len) != len)
			elog(ERROR, "lion index: a positions run of the build ends early");
	}
	else
		BufFileReadExact(c->file, p, len);
}

/* The record's next chunk into p..end, or false when it has no more. */
static bool
lion_poscur_next_chunk(LionPosCursor *c)
{
	uint32		n;

	if (!c->more)
		return false;
	if (c->tape == NULL && c->file == NULL)
	{
		LionPosBlock *b = c->mblock;

		if (b == NULL)
		{
			c->more = false;
			return false;
		}
		c->p = b->data;
		c->end = b->data + b->used;
		c->mblock = b->next;
		return true;
	}
	lion_poscur_read(c, &n, sizeof(n));
	if (n == 0)
	{
		c->more = false;
		return false;
	}
	if (n > LION_POSRUN_CHUNK)
		elog(ERROR, "lion index: a positions run of the build is damaged");
	lion_poscur_read(c, c->chunk, n);
	c->p = c->chunk;
	c->end = c->chunk + n;
	return true;
}

/* Move to the next record, or to the end. */
static void
lion_poscur_next_record(LionPosBuild *pb, LionPosCursor *c)
{
	LionPosRecord r;

	/* whatever was not read of the record before */
	while (lion_poscur_next_chunk(c))
		;
	c->p = c->end = NULL;
	c->has = false;

	if (c->tape == NULL && c->file == NULL)
	{
		LionPosKey *k;

		while (c->mcol < pb->ncols &&
			   (pb->cols[c->mcol] == NULL ||
				c->mkey >= pb->cols[c->mcol]->nkeys))
		{
			c->mcol++;
			c->mkey = 0;
		}
		if (c->mcol >= pb->ncols)
		{
			c->eos = true;
			return;
		}
		k = pb->cols[c->mcol]->keys[c->mkey++];
		c->col = c->mcol;
		c->key = k->key;
		c->mblock = k->head;
		c->more = true;
		return;
	}

	lion_poscur_read(c, &r, sizeof(r));
	if (r.col == LION_POSRUN_END)
	{
		c->eos = true;
		return;
	}
	if (r.col < 0 || r.col >= pb->ncols || pb->cols[r.col] == NULL ||
		(Size) r.keylen + VARHDRSZ > LION_MAX_KEY_SIZE)
		elog(ERROR, "lion index \"%s\": a positions run of the build is damaged",
			 RelationGetRelationName(pb->index));
	if (c->keycap < VARHDRSZ + (Size) r.keylen)
	{
		if (c->keybuf != NULL)
			pfree(c->keybuf);
		c->keycap = Max(MAXALIGN(VARHDRSZ + (Size) r.keylen), 64);
		c->keybuf = MemoryContextAlloc(c->cxt, c->keycap);
	}
	SET_VARSIZE(c->keybuf, VARHDRSZ + r.keylen);
	lion_poscur_read(c, VARDATA(c->keybuf), r.keylen);
	c->col = r.col;
	c->key = (text *) c->keybuf;
	c->more = true;
}

/* A cursor on the first record of a run, a participant's file or memory. */
static LionPosCursor *
lion_poscur_open(LionPosBuild *pb, int index, LogicalTape *tape,
				 BufFile *file)
{
	LionPosCursor *c = (LionPosCursor *) palloc0(sizeof(LionPosCursor));

	c->index = index;
	c->cxt = CurrentMemoryContext;
	c->tape = tape;
	c->file = file;
	if (tape != NULL || file != NULL)
		c->chunk = (uint8 *) palloc(LION_POSRUN_CHUNK);
	lion_poscur_next_record(pb, c);
	return c;
}

/* Where a cursor's record is against (col, key) in the build's order. */
static inline int
lion_poscur_cmp_key(LionPosBuild *pb, const LionPosCursor *c, int col,
					const text *key)
{
	if (c->col != col)
		return c->col < col ? -1 : 1;
	return lion_posbuild_key_cmp(pb->cols[col], c->key, key);
}

/*
 * binaryheap keeps its LARGEST element first, so this is backwards; between
 * inputs on the same key the older comes first.
 */
static int
lion_posbuild_heap_cmp(Datum a, Datum b, void *arg)
{
	LionPosBuild *pb = (LionPosBuild *) arg;
	const LionPosCursor *x = (const LionPosCursor *) DatumGetPointer(a);
	const LionPosCursor *y = (const LionPosCursor *) DatumGetPointer(b);
	int			c = lion_poscur_cmp_key(pb, x, y->col, y->key);

	if (c == 0)
		c = (x->index > y->index) - (x->index < y->index);
	return -c;
}

static binaryheap *
lion_posbuild_heap(LionPosBuild *pb, LionPosCursor **in, int nin)
{
	binaryheap *heap = binaryheap_allocate(Max(nin, 1),
										   lion_posbuild_heap_cmp, pb);
	int			i;

	for (i = 0; i < nin; i++)
	{
		if (!in[i]->eos)
			binaryheap_add_unordered(heap, PointerGetDatum(in[i]));
	}
	binaryheap_build(heap);
	return heap;
}

/*
 * Merge the inputs into w: each key once, its chunks those of its records
 * in the order of the inputs, which is the order the scan delivered them in.
 */
static void
lion_posbuild_merge_into(LionPosBuild *pb, LionPosCursor **in, int nin,
						 LionPosWriter *w)
{
	binaryheap *heap = lion_posbuild_heap(pb, in, nin);
	LionPosCursor **grp = (LionPosCursor **)
		palloc(sizeof(LionPosCursor *) * Max(nin, 1));

	while (!binaryheap_empty(heap))
	{
		int			ngrp = 0;
		int			i;

		grp[ngrp++] = (LionPosCursor *)
			DatumGetPointer(binaryheap_remove_first(heap));
		while (!binaryheap_empty(heap) &&
			   lion_poscur_cmp_key(pb, (LionPosCursor *)
								   DatumGetPointer(binaryheap_first(heap)),
								   grp[0]->col, grp[0]->key) == 0)
			grp[ngrp++] = (LionPosCursor *)
				DatumGetPointer(binaryheap_remove_first(heap));

		lion_posrun_begin_record(w, grp[0]->col, grp[0]->key);
		for (i = 0; i < ngrp; i++)
		{
			while (lion_poscur_next_chunk(grp[i]))
				lion_posrun_put(w, grp[i]->p, grp[i]->end - grp[i]->p);
		}
		lion_posrun_end_record(w);

		for (i = 0; i < ngrp; i++)
		{
			lion_poscur_next_record(pb, grp[i]);
			if (!grp[i]->eos)
				binaryheap_add(heap, PointerGetDatum(grp[i]));
		}
		CHECK_FOR_INTERRUPTS();
	}
	lion_posrun_end(w);
	binaryheap_free(heap);
	pfree(grp);
}

/* k runs from first on merged into one new run, and closed. */
static LogicalTape *
lion_posbuild_merge_runs(LionPosBuild *pb, int first, int k)
{
	LogicalTape *tape = lion_posbuild_new_tape(pb);
	MemoryContext old = MemoryContextSwitchTo(pb->mergecxt);
	LionPosCursor **in = (LionPosCursor **) palloc(sizeof(LionPosCursor *) * k);
	int			i;

	Assert(k >= 2 && first >= 0 && first + k <= pb->nruns);
	for (i = 0; i < k; i++)
		in[i] = lion_poscur_open(pb, i, pb->runs[first + i], NULL);
	lion_posbuild_merge_into(pb, in, k, lion_posrun_writer(tape, NULL));
	LogicalTapeRewindForRead(tape, pb->readbuf);
	for (i = 0; i < k; i++)
		LogicalTapeClose(pb->runs[first + i]);
	MemoryContextSwitchTo(old);
	MemoryContextReset(pb->mergecxt);
	pb->nmerges++;
	return tape;
}

/*
 * Leave at most maxorder runs for the last merge: the newest runs, which the
 * levels make the smallest, merged into one as often as it takes.
 */
static void
lion_posbuild_reduce_runs(LionPosBuild *pb)
{
	while (pb->nruns > pb->maxorder)
	{
		int			k = Min(pb->maxorder, pb->nruns - pb->maxorder + 1);
		int			first = pb->nruns - k;
		int			level = pb->levels[first];
		LogicalTape *tape = lion_posbuild_merge_runs(pb, first, k);

		pb->nruns = first;
		lion_posbuild_add_run(pb, tape, level);
	}
}

/* The merge the build takes keys from, over in[0 .. nin-1], older first. */
static LionPosMerge *
lion_posbuild_merge_begin(LionPosBuild *pb, LionPosCursor **in, int nin,
						  bool byblock)
{
	LionPosMerge *m = (LionPosMerge *) palloc0(sizeof(LionPosMerge));

	m->in = in;
	m->nin = nin;
	m->byblock = byblock;
	m->heap = lion_posbuild_heap(pb, in, nin);
	m->grp = (LionPosCursor **) palloc(sizeof(LionPosCursor *) * Max(nin, 1));
	pb->chunk = (LionContainer *) palloc0(LION_CONTAINER_MAX_SIZE);
	pb->maxpage = 16;
	pb->page = (LionPosMember *) palloc(sizeof(LionPosMember) * pb->maxpage);
	return m;
}

/*
 * The scan is over: the merge the build takes keys from.  With runs, what is
 * in memory becomes one too, and the runs are reduced to what one merge
 * reads; without, the keys in memory are sorted and read where they are.
 */
static void
lion_posbuild_finish(LionPosBuild *pb)
{
	MemoryContext old;
	LionPosCursor **in;
	int			nin;
	int			c;
	int			i;

	Assert(pb->merge == NULL && pb->fileset == NULL);
	if (pb->nruns > 0 && lion_posbuild_nkeys(pb) > 0)
		lion_posbuild_spill(pb);
	lion_posbuild_reduce_runs(pb);

	old = MemoryContextSwitchTo(pb->cxt);
	nin = Max(pb->nruns, 1);
	in = (LionPosCursor **) palloc(sizeof(LionPosCursor *) * nin);
	if (pb->nruns > 0)
	{
		for (i = 0; i < pb->nruns; i++)
			in[i] = lion_poscur_open(pb, i, pb->runs[i], NULL);
	}
	else
	{
		for (c = 0; c < pb->ncols; c++)
		{
			if (pb->cols[c] != NULL)
				lion_posbuild_sort(pb->cols[c]);
		}
		in[0] = lion_poscur_open(pb, 0, NULL, NULL);
	}
	pb->merge = lion_posbuild_merge_begin(pb, in, nin, false);
	MemoryContextSwitchTo(old);
}

/* Is the build writing positions for this key column? */
bool
lion_posbuild_column(LionPosBuild *pb, int col)
{
	if (pb == NULL || pb->cols[col] == NULL)
		return false;
	if (pb->merge == NULL)
		lion_posbuild_finish(pb);
	pb->col = col;
	return true;
}

/* ---------------------------------------------------------------------
 * A parallel build
 *
 * A participant's sink is the serial one, with the participant's share of
 * the memory.  When its scan is done it merges its runs and what is still in
 * memory into one, which it writes to a file of the build's shared fileset
 * under its file number.  The leader merges every participant's file as a
 * serial build merges its runs.  A parallel heap scan hands each participant
 * whole block ranges in ascending order, so a key's record in one
 * participant's file holds its rows in TID order but for the disorder within
 * a heap page (see the file header), and no heap page is in two
 * participants' records: taking a key merges its participants' records by
 * heap block.
 * --------------------------------------------------------------------- */

/* The name of a parallel build participant's positions file. */
static void
lion_posbuild_filename(char *name, int filenum)
{
	snprintf(name, MAXPGPATH, "lionpos.%d", filenum);
}

LionPosBuild *
lion_posbuild_begin_shared(Relation index, LionIndexState *ix, int workmem,
						   SharedFileSet *fileset, int filenum)
{
	LionPosBuild *pb = lion_posbuild_begin(index, ix, workmem);

	if (pb != NULL)
	{
		pb->fileset = fileset;
		pb->filenum = filenum;
	}
	return pb;
}

void
lion_posbuild_export(LionPosBuild *pb)
{
	char		name[MAXPGPATH];
	BufFile    *file;
	MemoryContext old;

	Assert(pb->fileset != NULL);
	if (pb->nruns > 0 && lion_posbuild_nkeys(pb) > 0)
		lion_posbuild_spill(pb);
	lion_posbuild_reduce_runs(pb);

	/* the file exists even when it holds nothing: the leader opens it */
	lion_posbuild_filename(name, pb->filenum);
	file = BufFileCreateFileSet(&pb->fileset->fs, name);
	old = MemoryContextSwitchTo(pb->mergecxt);
	if (pb->nruns == 0)
		lion_posbuild_write_memory(pb, lion_posrun_writer(NULL, file));
	else
	{
		LionPosCursor **in = (LionPosCursor **)
			palloc(sizeof(LionPosCursor *) * pb->nruns);
		int			i;

		for (i = 0; i < pb->nruns; i++)
			in[i] = lion_poscur_open(pb, i, pb->runs[i], NULL);
		lion_posbuild_merge_into(pb, in, pb->nruns,
								 lion_posrun_writer(NULL, file));
		for (i = 0; i < pb->nruns; i++)
			LogicalTapeClose(pb->runs[i]);
		pb->nruns = 0;
	}
	MemoryContextSwitchTo(old);
	MemoryContextReset(pb->mergecxt);
	BufFileClose(file);
	lion_posbuild_reset(pb);
}

void
lion_posbuild_import(LionPosBuild *pb, SharedFileSet *fileset,
					 int nparticipants, const int *filenums)
{
	MemoryContext old = MemoryContextSwitchTo(pb->cxt);
	char		name[MAXPGPATH];
	LionPosCursor **in;
	int			i;

	Assert(pb->fileset == NULL && pb->nsrcs == 0 && pb->merge == NULL);
	Assert(pb->nruns == 0 && lion_posbuild_nkeys(pb) == 0);
	pb->nsrcs = nparticipants;
	pb->srcfiles = (BufFile **) palloc0(sizeof(BufFile *) * nparticipants);
	in = (LionPosCursor **) palloc(sizeof(LionPosCursor *) *
								   Max(nparticipants, 1));
	for (i = 0; i < nparticipants; i++)
	{
		lion_posbuild_filename(name, filenums[i]);
		pb->srcfiles[i] = BufFileOpenFileSet(&fileset->fs, name, O_RDONLY,
											 false);
		in[i] = lion_poscur_open(pb, i, NULL, pb->srcfiles[i]);
	}
	pb->merge = lion_posbuild_merge_begin(pb, in, nparticipants, true);
	MemoryContextSwitchTo(old);
}

/* ---------------------------------------------------------------------
 * A position tree written bottom up through the build's page writer
 * --------------------------------------------------------------------- */

/*
 * The member bytes a written chunk holds at most: half a page, so that a
 * leaf takes two.  An insert later appends to the last chunk and splits it
 * when it is full, as for any chunk.
 */
#define LION_POS_LOAD_BYTES \
	Min((Size) LION_POS_MAX_BYTES, \
		(Size) MAXALIGN_DOWN(LION_PAGE_CAPACITY / 2 - sizeof(ItemIdData)) - \
		LION_POS_HDRSZ)

typedef struct LionPosLoader
{
	const LionPageWriter *w;
	uint32		hash;
	BlockNumber root;			/* reserved first, written last */
	LionContainer *chunk;		/* the chunk being filled */

	/*
	 * Up to LION_POSTING_FLAT_LEAVES leaves make a tree without an internal
	 * level, whose first leaf is at the root block (DESIGN.md §17, §22).
	 * Until the tree is known to need more, the finished leaves are held
	 * here, the first one without a block; `tree` once it has more, and the
	 * first leaf then gets a block of its own.
	 */
	void	   *held[LION_POSTING_FLAT_LEAVES];
	int			nheld;
	bool		tree;
	LionPosTail tail;			/* where its last member ends */
	bool		chunkopen;
	/* the leaf being filled */
	void	   *leafhandle;
	Page		leaf;
	BlockNumber leafblk;		/* InvalidBlockNumber: the first, maybe root */
	int			nleaves;		/* leaves written */
	LionPostingPivot *pivots;	/* their downlinks */
	int			maxpivots;
} LionPosLoader;

static void
lion_posload_newleaf(LionPosLoader *ld)
{
	ld->leafhandle = ld->w->get(ld->w->arg, &ld->leaf,
								LION_PAGE_CONTAINER | LION_PAGE_POSITIONS);
	lion_page_set_owner(ld->leaf, ld->hash, ld->root);
}

/*
 * Write the held leaves, the first one at firstblk: the root block when the
 * tree stays without an internal level, a block of its own when it does not.
 */
static void
lion_posload_put_held(LionPosLoader *ld, BlockNumber firstblk)
{
	int			i;

	Assert(ld->nheld == ld->nleaves);
	if (ld->nheld > 0)
		ld->pivots[0].child = firstblk;
	for (i = 0; i < ld->nheld; i++)
		ld->w->put(ld->w->arg, ld->pivots[i].child, ld->held[i]);
	ld->nheld = 0;
}

/* Finish the leaf being filled; next is its right sibling, if any. */
static void
lion_posload_putleaf(LionPosLoader *ld, BlockNumber next)
{
	BlockNumber blk = ld->leafblk;

	Assert(BlockNumberIsValid(blk) || ld->nleaves == 0);
	lion_page_update_minmax(ld->leaf);
	LionPageGetOpaque(ld->leaf)->rightlink = next;
	if (ld->nleaves == ld->maxpivots)
	{
		ld->maxpivots *= 2;
		ld->pivots = (LionPostingPivot *) repalloc(ld->pivots,
												   sizeof(LionPostingPivot) *
												   ld->maxpivots);
	}
	/* (P3) of lion_postree.c: a leaf's separator is its first chunk's header */
	ld->pivots[ld->nleaves].ckey = (ld->nleaves == 0) ? 0 :
		LionPageGetOpaque(ld->leaf)->minckey;
	ld->pivots[ld->nleaves].child = blk;
	ld->nleaves++;
	if (ld->tree)
		ld->w->put(ld->w->arg, blk, ld->leafhandle);
	else
		ld->held[ld->nheld++] = ld->leafhandle;
	ld->leafhandle = NULL;
	ld->leaf = NULL;
}

/* The chunk being filled goes onto the leaf, or onto the next one. */
static void
lion_posload_close(LionPosLoader *ld)
{
	Size		sz = lion_poschunk_size(ld->chunk);

	if (PageGetFreeSpace(ld->leaf) < MAXALIGN(sz))
	{
		BlockNumber next = ld->w->alloc(ld->w->arg);

		lion_posload_putleaf(ld, next);

		/*
		 * One leaf more than a tree without an internal level has: the
		 * root block is the internal root's, and the first leaf moves off it.
		 */
		if (!ld->tree && ld->nleaves + 1 > LION_POSTING_FLAT_LEAVES)
		{
			ld->tree = true;
			lion_posload_put_held(ld, ld->w->alloc(ld->w->arg));
		}
		lion_posload_newleaf(ld);
		ld->leafblk = next;
	}
	if (PageAddItemExtended(ld->leaf, ld->chunk, sz, InvalidOffsetNumber, 0) ==
		InvalidOffsetNumber)
		elog(ERROR, "lion index: failed to add a position chunk to a build page");
	ld->chunkopen = false;
}

static void
lion_posload_add(LionPosLoader *ld, const LionPosMember *m)
{
	Size		cap = LION_POS_HDRSZ + LION_POS_LOAD_BYTES;

	if (ld->chunkopen && !lion_poschunk_append_tail(ld->chunk, cap, m,
													&ld->tail))
		lion_posload_close(ld);
	if (!ld->chunkopen)
	{
		memset(ld->chunk, 0, LION_CONTAINER_MAX_SIZE);
		lion_poschunk_init(ld->chunk, lion_pos_block(m->code));
		ld->chunkopen = true;
		ld->tail.valid = false;
		if (!lion_poschunk_append_tail(ld->chunk, cap, m, &ld->tail))
			elog(ERROR, "lion index: a position member does not fit an empty chunk");
	}
}

/*
 * One level of internal pages over the downlinks of the level below; the
 * downlinks of the pages written come back, or nothing when they all fit
 * one page, which is then the root.
 */
static int
lion_posload_level(LionPosLoader *ld, uint16 level, LionPostingPivot *pivots,
				   int npivots, LionPostingPivot **uppers)
{
	Size		per = MAXALIGN(LION_POSTING_PIVOT_SIZE) + sizeof(ItemIdData);
	int			fit = (int) (LION_PAGE_CAPACITY / per);
	int			perpage = fit - 1;	/* a non-rightmost page has a high key */
	bool		isroot = (npivots <= fit);
	int			npages = isroot ? 1 : (npivots + perpage - 1) / perpage;
	LionPostingPivot *up = isroot ? NULL :
		(LionPostingPivot *) palloc(sizeof(LionPostingPivot) * npages);
	BlockNumber blk = isroot ? ld->root : ld->w->alloc(ld->w->arg);
	int			done = 0;
	int			p;

	for (p = 0; p < npages; p++)
	{
		int			n = isroot ? npivots : Min(perpage, npivots - done);
		bool		last = (p == npages - 1);
		BlockNumber next = last ? InvalidBlockNumber : ld->w->alloc(ld->w->arg);
		Page		page;
		void	   *h = ld->w->get(ld->w->arg, &page,
								   LION_PAGE_CONTAINER | LION_PAGE_POSITIONS);
		int			i;

		lion_page_set_owner(page, ld->hash, ld->root);
		LionPageGetOpaque(page)->level = level;
		LionPageGetOpaque(page)->rightlink = next;
		for (i = last ? 0 : -1; i < n; i++)
		{
			LionPostingPivot hk;
			LionPostingPivot *item = &pivots[done + i];

			if (i < 0)
			{
				/* the high key: the separator of the right sibling */
				hk.ckey = pivots[done + n].ckey;
				hk.child = InvalidBlockNumber;
				item = &hk;
			}
			if (PageAddItemExtended(page, item, LION_POSTING_PIVOT_SIZE,
									InvalidOffsetNumber, 0) == InvalidOffsetNumber)
				elog(ERROR, "lion index: failed to add a downlink to a position build page");
		}
		if (!isroot)
		{
			up[p].ckey = (p == 0) ? 0 : pivots[done].ckey;
			up[p].child = blk;
		}
		ld->w->put(ld->w->arg, blk, h);
		blk = next;
		done += n;
	}
	*uppers = up;
	return isroot ? 0 : npages;
}

/* Close the tree: the last leaf, then the internal levels up to the root. */
static void
lion_posload_end(LionPosLoader *ld)
{
	LionPostingPivot *pivots;
	LionPostingPivot *uppers;
	int			npivots;
	uint16		level = 1;

	if (ld->chunkopen)
		lion_posload_close(ld);

	lion_posload_putleaf(ld, InvalidBlockNumber);
	if (!ld->tree)
	{
		/* no internal level: the first leaf is the root */
		lion_posload_put_held(ld, ld->root);
		return;
	}

	pivots = ld->pivots;
	npivots = ld->nleaves;
	while ((npivots = lion_posload_level(ld, level, pivots, npivots,
										 &uppers)) > 0)
	{
		if (pivots != ld->pivots)
			pfree(pivots);
		pivots = uppers;
		if (++level >= LION_POSTING_MAX_HEIGHT)
			elog(ERROR, "lion index: a position tree the build writes is too tall");
	}
	if (pivots != ld->pivots)
		pfree(pivots);
}

/* Where take() puts the members it reads: a chunk, or a tree once they
 * do not fit one. */
typedef struct LionPosTake
{
	LionPosBuild *pb;
	const LionPageWriter *w;
	uint32		hash;
	LionPosLoader *ld;
	LionPosTail tail;
	uint64		n;
	uint64		last;			/* the code of the member emitted last */
} LionPosTake;

static void
lion_posbuild_emit(LionPosTake *t, const LionPosMember *m)
{
	LionPosBuild *pb = t->pb;

	t->n++;
	if (t->ld != NULL)
	{
		lion_posload_add(t->ld, m);
		return;
	}
	if (t->n == 1)
	{
		memset(pb->chunk, 0, LION_CONTAINER_MAX_SIZE);
		lion_poschunk_init(pb->chunk, lion_pos_block(m->code));
		t->tail.valid = false;
	}
	if (!lion_poschunk_append_tail(pb->chunk, LION_CONTAINER_MAX_SIZE, m,
								   &t->tail))
	{
		/* more than a chunk: a tree, which starts with these */
		const LionPageWriter *w = t->w;
		LionPosLoader *ld = (LionPosLoader *) palloc0(sizeof(LionPosLoader));
		LionPosMember *cm = (LionPosMember *) palloc(sizeof(LionPosMember));
		LionPosIter it;

		ld->w = w;
		ld->hash = t->hash;
		ld->root = w->alloc(w->arg);
		ld->chunk = (LionContainer *) palloc0(LION_CONTAINER_MAX_SIZE);
		ld->leafblk = InvalidBlockNumber;
		ld->maxpivots = 64;
		ld->pivots = (LionPostingPivot *)
			palloc(sizeof(LionPostingPivot) * ld->maxpivots);
		lion_posload_newleaf(ld);
		lion_poschunk_iter_init(&it, pb->chunk);
		while (lion_poschunk_iter_next(&it, cm))
			lion_posload_add(ld, cm);
		lion_posload_add(ld, m);
		pfree(cm);
		t->ld = ld;
	}
}

static int
lion_posmember_code_cmp(const void *a, const void *b)
{
	uint64		x = ((const LionPosMember *) a)->code;
	uint64		y = ((const LionPosMember *) b)->code;

	return (x > y) - (x < y);
}

/*
 * The heap page's members read so far, in code order, emitted: after every
 * member emitted before, which the order of the runs (see the file header)
 * is what makes so.
 */
static void
lion_posbuild_flush_page(LionPosTake *t)
{
	LionPosBuild *pb = t->pb;
	int			i;

	if (pb->npage > 1)
		qsort(pb->page, pb->npage, sizeof(LionPosMember),
			  lion_posmember_code_cmp);
	for (i = 0; i < pb->npage; i++)
	{
		uint64		code = pb->page[i].code;

		if (t->n > 0 && code == t->last)
			elog(ERROR, "lion index \"%s\": the build has a row's positions twice for one key",
				 RelationGetRelationName(pb->index));
		if (t->n > 0 && code < t->last)
			elog(ERROR, "lion index \"%s\": the build has a key's positions out of order",
				 RelationGetRelationName(pb->index));
		t->last = code;
		lion_posbuild_emit(t, &pb->page[i]);
	}
	pb->npage = 0;
}

/*
 * One member of a key's stream: onto the heap page's, which it may close.
 * The page's array grows as a page brings more members, up to as many as a
 * page has offsets.
 */
static void
lion_posbuild_member(LionPosTake *t, uint64 code, uint32 npos,
					 const uint8 *pos)
{
	LionPosBuild *pb = t->pb;
	LionKeyPositions kp;
	uint16		posbuf[LION_POS_MAX_NPOS];

	Assert(npos <= LION_POS_MAX_NPOS);
	memcpy(posbuf, pos, sizeof(uint16) * npos);
	if (pb->npage > 0 &&
		(lion_pos_block(code) != lion_pos_block(pb->page[0].code) ||
		 pb->npage >= LION_PAGE_CODES_MAX))
		lion_posbuild_flush_page(t);
	if (pb->npage >= pb->maxpage)
	{
		Assert(pb->maxpage < LION_PAGE_CODES_MAX);
		pb->maxpage = Min(pb->maxpage * 2, LION_PAGE_CODES_MAX);
		pb->page = (LionPosMember *)
			repalloc(pb->page, sizeof(LionPosMember) * pb->maxpage);
	}
	kp.npos = (uint16) npos;
	kp.pos = posbuf;
	lion_posmember_from_key(&pb->page[pb->npage++], code, &kp);
}

/* The next member of a stream at *pp, false at end. */
static bool
lion_posbuild_decode(const uint8 **pp, const uint8 *end, uint64 *code,
					 uint32 *npos, const uint8 **pos)
{
	const uint8 *p = *pp;
	uint64		n;

	if (p >= end)
		return false;
	*code = lion_posbuild_get_varbyte(&p, end);
	n = lion_posbuild_get_varbyte(&p, end);
	if (n > LION_POS_MAX_NPOS || (Size) (end - p) < sizeof(uint16) * n)
		elog(ERROR, "lion index: a position stream of the build is damaged");
	*npos = (uint32) n;
	*pos = p;
	*pp = p + sizeof(uint16) * n;
	return true;
}

/* Decode the members of len bytes of a key's stream. */
static void
lion_posbuild_read(LionPosTake *t, const uint8 *p, Size len)
{
	const uint8 *end = p + len;
	uint64		code;
	uint32		npos;
	const uint8 *pos;

	while (lion_posbuild_decode(&p, end, &code, &npos, &pos))
		lion_posbuild_member(t, code, npos, pos);
}

/* The next member of a cursor's record at its head; has is false at its end. */
static void
lion_poscur_member(LionPosCursor *c)
{
	while (!lion_posbuild_decode(&c->p, c->end, &c->code, &c->npos, &c->pos))
	{
		if (!lion_poscur_next_chunk(c))
		{
			c->has = false;
			return;
		}
	}
	c->has = true;
}

/*
 * A key's records from more than one participant of a parallel build,
 * merged by heap block (see "A parallel build" above).
 */
static void
lion_posbuild_merge_members(LionPosTake *t, LionPosCursor **grp, int ngrp)
{
	int			i;

	for (i = 0; i < ngrp; i++)
		lion_poscur_member(grp[i]);
	for (;;)
	{
		LionPosCursor *best = NULL;

		for (i = 0; i < ngrp; i++)
		{
			if (grp[i]->has &&
				(best == NULL ||
				 lion_pos_block(grp[i]->code) < lion_pos_block(best->code)))
				best = grp[i];
		}
		if (best == NULL)
			break;
		lion_posbuild_member(t, best->code, best->npos, best->pos);
		lion_poscur_member(best);
	}
}

/* A key the merge holds that the build wrote no entry for. */
static void
lion_posbuild_unwritten(LionPosBuild *pb)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("lion index \"%s\": the build has positions for a key it wrote no entry for",
					RelationGetRelationName(pb->index)),
			 errdetail("A column that stores positions needs a deterministic collation.")));
}

/*
 * The positions of key, which the build is writing the entry of (see the
 * file header).  hash is the entry's owner stamp (lion_entry_owner()),
 * which the pages of a tree are stamped with.  Exactly one of *chunk
 * (positions that fit one chunk, valid until the next call) and *root (a
 * tree, written) is set; *nmembers says how many rows they cover.
 */
void
lion_posbuild_take(LionPosBuild *pb, Datum key, uint32 hash,
				   const LionPageWriter *w, LionContainer **chunk,
				   BlockNumber *root, uint64 *nmembers)
{
	LionPosMerge *m = pb->merge;
	text	   *kt = DatumGetTextPP(key);
	LionPosTake t;
	int			ngrp = 0;
	int			i;

	*chunk = NULL;
	*root = InvalidBlockNumber;
	Assert(m != NULL && pb->col >= 0 && pb->cols[pb->col] != NULL);

	/*
	 * The inputs whose record is this key, older first; one whose record is
	 * a key before it holds a key the build passed by.
	 */
	while (!binaryheap_empty(m->heap))
	{
		LionPosCursor *c = (LionPosCursor *)
			DatumGetPointer(binaryheap_first(m->heap));
		int			r = lion_poscur_cmp_key(pb, c, pb->col, kt);

		if (r > 0)
			break;
		if (r < 0)
			lion_posbuild_unwritten(pb);
		m->grp[ngrp++] = (LionPosCursor *)
			DatumGetPointer(binaryheap_remove_first(m->heap));
	}
	if (ngrp == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("lion index \"%s\": the build has no positions for a key it writes",
						RelationGetRelationName(pb->index)),
				 errdetail("A column that stores positions needs a deterministic collation.")));

	memset(&t, 0, sizeof(t));
	t.pb = pb;
	t.w = w;
	t.hash = hash;
	pb->npage = 0;

	if (m->byblock && ngrp > 1)
		lion_posbuild_merge_members(&t, m->grp, ngrp);
	else
	{
		/* one scan's records, in the order it wrote them */
		for (i = 0; i < ngrp; i++)
		{
			LionPosCursor *c = m->grp[i];

			while (lion_poscur_next_chunk(c))
				lion_posbuild_read(&t, c->p, c->end - c->p);
		}
	}
	if (pb->npage > 0)
		lion_posbuild_flush_page(&t);

	for (i = 0; i < ngrp; i++)
	{
		lion_poscur_next_record(pb, m->grp[i]);
		if (!m->grp[i]->eos)
			binaryheap_add(m->heap, PointerGetDatum(m->grp[i]));
	}

	if (t.n == 0)
		elog(ERROR, "lion index \"%s\": the build has no positions for a key it writes",
			 RelationGetRelationName(pb->index));
	if (t.ld != NULL)
	{
		lion_posload_end(t.ld);
		*root = t.ld->root;
		pfree(t.ld->pivots);
		pfree(t.ld->chunk);
		pfree(t.ld);
	}
	else
		*chunk = pb->chunk;
	*nmembers = t.n;
}

/*
 * A one-leaf position tree holding chunk, for a CHAIN entry whose positions
 * fit one: its root.
 */
BlockNumber
lion_posbuild_chunk_tree(const LionPageWriter *w, uint32 hash,
						 const LionContainer *chunk)
{
	BlockNumber root = w->alloc(w->arg);
	Page		page;
	void	   *h = w->get(w->arg, &page,
						   LION_PAGE_CONTAINER | LION_PAGE_POSITIONS);

	lion_page_set_owner(page, hash, root);
	if (PageAddItemExtended(page, chunk, lion_poschunk_size(chunk),
							InvalidOffsetNumber, 0) == InvalidOffsetNumber)
		elog(ERROR, "lion index: failed to add a position chunk to a build page");
	lion_page_update_minmax(page);
	w->put(w->arg, root, h);
	return root;
}

/* The column is written: every key with positions has had them taken. */
void
lion_posbuild_column_done(LionPosBuild *pb, int col)
{
	LionPosMerge *m;

	if (pb == NULL || pb->cols[col] == NULL)
		return;
	m = pb->merge;
	Assert(m != NULL);
	if (!binaryheap_empty(m->heap) &&
		((LionPosCursor *) DatumGetPointer(binaryheap_first(m->heap)))->col <= col)
		lion_posbuild_unwritten(pb);
	pb->col = -1;
}

void
lion_posbuild_end(LionPosBuild *pb)
{
	if (pb == NULL)
		return;
	elog(DEBUG1, "lion index \"%s\": the build spilled its positions %d times and merged runs into one %d times",
		 RelationGetRelationName(pb->index), pb->nspills, pb->nmerges);
	if (pb->tapes != NULL)
		LogicalTapeSetClose(pb->tapes);
	for (int i = 0; i < pb->nsrcs; i++)
		BufFileClose(pb->srcfiles[i]);
	MemoryContextDelete(pb->cxt);
}
