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
 * MEMORY is a quarter of the build's (maintenance_work_mem): when the
 * streams outgrow it, every key's stream is written to one temporary file,
 * the key keeping where its part went, and the memory is reset.  The table
 * of keys stays, which costs a key's bytes and a few words for each of the
 * column's distinct keys.
 *
 * When the build writes a key's entry it takes the key's positions here
 * (lion_posbuild_take()), in whatever order it writes the entries in: the
 * parts on file, in the order they were written, then what is in memory.
 * The one disorder the scan has - a heap-only tuple reported under its HOT
 * chain's root offset, within one heap page (lion_spool.c) - is undone by
 * sorting each heap page's members as they are read.  The positions come
 * back
 *
 *	- as one chunk when they fit one, which the entry carries when it is
 *	  INLINE and which becomes a one-leaf position tree when it is a CHAIN;
 *	- as a position tree, written bottom up through the build's own page
 *	  writer as they are read, and the entry has to be a CHAIN.
 *
 * A key the build writes no entry for, or one whose bytes differ from the
 * entry's, is an ERROR when the column is done: equal keys whose bytes
 * differ (a nondeterministic collation) cannot store positions.
 *
 * A parallel build does not run the sink in its workers, so a positions
 * column builds serially (lion_build.c).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "miscadmin.h"
#include "storage/buffile.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "varatt.h"

#include "lion.h"
#include "lion_positions.h"
#include "lion_spool.h"

/* A stream's memory comes in blocks, the first this small, doubling up to
 * the largest. */
#define LION_POSB_FIRST_BLOCK	32
#define LION_POSB_MAX_BLOCK		8192

/* The streams' blocks are carved out of arena blocks this large. */
#define LION_POSB_ARENA			((Size) 64 * 1024)

/* The most members one key has on one heap page. */
#define LION_PAGE_CODES_MAX		(1 << LION_OFFSET_BITS)

typedef struct LionPosBlock
{
	struct LionPosBlock *next;
	uint32		used;
	uint32		cap;
	uint8		data[FLEXIBLE_ARRAY_MEMBER];
} LionPosBlock;

/* A part of a key's stream written to the file. */
typedef struct LionPosPart
{
	int			fileno;
	off_t		offset;
	Size		len;
} LionPosPart;

typedef struct LionPosKey
{
	struct LionPosKey *next;	/* in its hash bucket */
	uint32		hash;			/* hash_bytes() of the key's bytes */
	uint32		len;
	char	   *bytes;
	LionPosBlock *head;			/* in memory, oldest first */
	LionPosBlock *tail;
	LionPosPart *parts;			/* on file, oldest first */
	int			nparts;
	int			maxparts;
	bool		taken;
} LionPosKey;

typedef struct LionPosCol
{
	LionPosKey **buckets;
	uint32		nbuckets;		/* a power of two */
	LionPosKey **keys;			/* every key, in the order first seen */
	int			nkeys;
	int			maxkeys;
	int			ntaken;
} LionPosCol;

struct LionPosBuild
{
	Relation	index;
	int			ncols;
	LionPosCol **cols;			/* per key column; NULL when it stores none */
	MemoryContext cxt;			/* this, the keys and the tables */
	MemoryContext datacxt;		/* the streams in memory */
	Size		budget;
	Size		used;			/* the arena blocks in datacxt */
	char	   *arena;			/* where the next stream block is carved */
	Size		arenafree;
	BufFile    *file;			/* the streams written out, once there are */
	int			nspills;
	/* taking a key's positions */
	int			col;
	LionContainer *chunk;
	LionPosMember *page;		/* one heap page's members */
	int			npage;
	uint8	   *readbuf;
	Size		readcap;
};

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
	pb->datacxt = AllocSetContextCreate(cxt, "lion positions streams",
										ALLOCSET_DEFAULT_SIZES);
	/* a quarter of the build's memory: the spool has the rest */
	pb->budget = Max((Size) workmem * 1024 / 4, (Size) 64 * 1024);
	pb->col = -1;
	pb->chunk = (LionContainer *) palloc0(LION_CONTAINER_MAX_SIZE);
	pb->page = (LionPosMember *)
		palloc(sizeof(LionPosMember) * LION_PAGE_CODES_MAX);
	pb->cols = (LionPosCol **) palloc0(sizeof(LionPosCol *) * pb->ncols);
	for (c = 0; c < ix->ncolumns; c++)
	{
		LionState  *cs = &ix->cols[c];
		LionPosCol *pc;

		if (!cs->positions)
			continue;

		/* the keys are looked up by their bytes, which are text's */
		if (cs->typid != TEXTOID || !cs->ordered)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("lion index \"%s\": only a text key column ordered by its comparison can store positions",
							RelationGetRelationName(index))));
		pc = (LionPosCol *) palloc0(sizeof(LionPosCol));
		pc->nbuckets = 1024;
		pc->buckets = (LionPosKey **) palloc0(sizeof(LionPosKey *) * pc->nbuckets);
		pc->maxkeys = 1024;
		pc->keys = (LionPosKey **) palloc(sizeof(LionPosKey *) * pc->maxkeys);
		pb->cols[c] = pc;
	}
	MemoryContextSwitchTo(old);
	return pb;
}

static LionPosKey *
lion_posbuild_lookup(LionPosCol *pc, const char *bytes, uint32 len,
					 uint32 hash)
{
	LionPosKey *k;

	for (k = pc->buckets[hash & (pc->nbuckets - 1)]; k != NULL; k = k->next)
	{
		if (k->hash == hash && k->len == len && memcmp(k->bytes, bytes, len) == 0)
			return k;
	}
	return NULL;
}

/* A key seen for the first time, in pb->cxt. */
static LionPosKey *
lion_posbuild_new_key(LionPosBuild *pb, LionPosCol *pc, const char *bytes,
					  uint32 len, uint32 hash)
{
	MemoryContext old = MemoryContextSwitchTo(pb->cxt);
	LionPosKey *k = (LionPosKey *) palloc0(sizeof(LionPosKey));
	uint32		b;

	k->hash = hash;
	k->len = len;
	k->bytes = (char *) palloc(Max(len, 1));
	memcpy(k->bytes, bytes, len);

	if (pc->nkeys >= pc->maxkeys)
	{
		pc->maxkeys *= 2;
		pc->keys = (LionPosKey **) repalloc_huge(pc->keys,
												 sizeof(LionPosKey *) * pc->maxkeys);
	}
	pc->keys[pc->nkeys++] = k;

	/* twice the keys' buckets, rehashed */
	if ((uint32) pc->nkeys > pc->nbuckets && pc->nbuckets < (PG_UINT32_MAX >> 2))
	{
		uint32		nb = pc->nbuckets * 2;
		LionPosKey **buckets = (LionPosKey **)
			palloc_extended(sizeof(LionPosKey *) * nb,
							MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);
		int			i;

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
	MemoryContextSwitchTo(old);
	return k;
}

/*
 * size bytes of the streams' memory, carved out of arena blocks rather than
 * allocated one by one: most keys of a text column have a block of a few
 * members, and a palloc header for each would be a good part of it.
 */
static void *
lion_posbuild_alloc(LionPosBuild *pb, Size size)
{
	void	   *p;

	size = MAXALIGN(size);
	if (pb->arenafree < size)
	{
		Size		block = Max(size, (Size) LION_POSB_ARENA);

		pb->arena = (char *) MemoryContextAlloc(pb->datacxt, block);
		pb->arenafree = block;
		pb->used += block;
	}
	p = pb->arena;
	pb->arena += size;
	pb->arenafree -= size;
	return p;
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
 * Write every key's stream in memory to the file, the key keeping where its
 * part went, and let the memory go.
 */
static void
lion_posbuild_spill(LionPosBuild *pb)
{
	MemoryContext old = MemoryContextSwitchTo(pb->cxt);
	int			c;

	if (pb->file == NULL)
		pb->file = BufFileCreateTemp(false);
	for (c = 0; c < pb->ncols; c++)
	{
		LionPosCol *pc = pb->cols[c];
		int			i;

		if (pc == NULL)
			continue;
		for (i = 0; i < pc->nkeys; i++)
		{
			LionPosKey *k = pc->keys[i];
			LionPosPart *part;
			LionPosBlock *b;

			if (k->head == NULL)
				continue;
			if (k->nparts >= k->maxparts)
			{
				k->maxparts = Max(k->maxparts * 2, 2);
				k->parts = (k->parts == NULL) ?
					(LionPosPart *) palloc(sizeof(LionPosPart) * k->maxparts) :
					(LionPosPart *) repalloc(k->parts,
											 sizeof(LionPosPart) * k->maxparts);
			}
			part = &k->parts[k->nparts++];
			BufFileTell(pb->file, &part->fileno, &part->offset);
			part->len = 0;
			for (b = k->head; b != NULL; b = b->next)
			{
				BufFileWrite(pb->file, b->data, b->used);
				part->len += b->used;
			}
			k->head = k->tail = NULL;
		}
		CHECK_FOR_INTERRUPTS();
	}
	MemoryContextSwitchTo(old);
	MemoryContextReset(pb->datacxt);
	pb->used = 0;
	pb->arena = NULL;
	pb->arenafree = 0;
	pb->nspills++;
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
	uint32		hash = hash_bytes((const unsigned char *) bytes, (int) len);
	uint32		npos = Min((uint32) kp->npos, (uint32) LION_POS_MAX_NPOS);
	LionPosKey *k;
	uint8	   *start;
	uint8	   *p;

	Assert(pc != NULL);
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

	if (pb->used > pb->budget)
		lion_posbuild_spill(pb);
}

/* Is the build writing positions for this key column? */
bool
lion_posbuild_column(LionPosBuild *pb, int col)
{
	if (pb == NULL || pb->cols[col] == NULL)
		return false;
	pb->col = col;
	return true;
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

/* Write the leaf being filled; next is its right sibling, if any. */
static void
lion_posload_putleaf(LionPosLoader *ld, BlockNumber next)
{
	BlockNumber blk = BlockNumberIsValid(ld->leafblk) ? ld->leafblk :
		ld->w->alloc(ld->w->arg);

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
	ld->w->put(ld->w->arg, blk, ld->leafhandle);
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

	if (ld->nleaves == 0)
	{
		/* one leaf: it is the root */
		ld->leafblk = ld->root;
		ld->nleaves = 0;
		lion_posload_putleaf(ld, InvalidBlockNumber);
		return;
	}

	lion_posload_putleaf(ld, InvalidBlockNumber);
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

/* The heap page's members read so far, in code order, emitted. */
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
		if (i > 0 && pb->page[i].code == pb->page[i - 1].code)
			elog(ERROR, "lion index \"%s\": the build has a row's positions twice for one key",
				 RelationGetRelationName(pb->index));
		lion_posbuild_emit(t, &pb->page[i]);
	}
	pb->npage = 0;
}

/* Decode the members of len bytes of a key's stream. */
static void
lion_posbuild_read(LionPosTake *t, const uint8 *p, Size len)
{
	LionPosBuild *pb = t->pb;
	const uint8 *end = p + len;

	while (p < end)
	{
		uint64		code = lion_posbuild_get_varbyte(&p, end);
		uint64		npos = lion_posbuild_get_varbyte(&p, end);
		LionKeyPositions kp;
		uint16		posbuf[LION_POS_MAX_NPOS];

		if (npos > LION_POS_MAX_NPOS || (Size) (end - p) < sizeof(uint16) * npos)
			elog(ERROR, "lion index: a position stream of the build is damaged");
		memcpy(posbuf, p, sizeof(uint16) * npos);
		p += sizeof(uint16) * npos;

		if (pb->npage > 0 &&
			(lion_pos_block(code) != lion_pos_block(pb->page[0].code) ||
			 pb->npage >= LION_PAGE_CODES_MAX))
			lion_posbuild_flush_page(t);
		kp.npos = (uint16) npos;
		kp.pos = posbuf;
		lion_posmember_from_key(&pb->page[pb->npage++], code, &kp);
	}
}

/*
 * The positions of key, which the build is writing the entry of (see the
 * file header).  hash is the entry's, which the pages of a tree are stamped
 * with.  Exactly one of *chunk (positions that fit one chunk, valid until
 * the next call) and *root (a tree, written) is set; *nmembers says how many
 * rows they cover.
 */
void
lion_posbuild_take(LionPosBuild *pb, Datum key, uint32 hash,
				   const LionPageWriter *w, LionContainer **chunk,
				   BlockNumber *root, uint64 *nmembers)
{
	LionPosCol *pc = pb->cols[pb->col];
	text	   *kt = DatumGetTextPP(key);
	const char *bytes = VARDATA_ANY(kt);
	uint32		len = VARSIZE_ANY_EXHDR(kt);
	LionPosKey *k;
	LionPosTake t;
	LionPosBlock *b;
	int			i;

	*chunk = NULL;
	*root = InvalidBlockNumber;

	k = lion_posbuild_lookup(pc, bytes, len,
							 hash_bytes((const unsigned char *) bytes, (int) len));
	if (k == NULL || k->taken)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("lion index \"%s\": the build has no positions for a key it writes",
						RelationGetRelationName(pb->index)),
				 errdetail("A column that stores positions needs a deterministic collation.")));
	k->taken = true;
	pc->ntaken++;

	memset(&t, 0, sizeof(t));
	t.pb = pb;
	t.w = w;
	t.hash = hash;
	pb->npage = 0;

	for (i = 0; i < k->nparts; i++)
	{
		LionPosPart *part = &k->parts[i];

		if (part->len > pb->readcap)
		{
			if (pb->readbuf != NULL)
				pfree(pb->readbuf);
			pb->readcap = Max(part->len, (Size) BLCKSZ);
			pb->readbuf = (uint8 *) MemoryContextAllocHuge(pb->cxt, pb->readcap);
		}
		if (BufFileSeek(pb->file, part->fileno, part->offset, SEEK_SET) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not seek in the positions file of lion index \"%s\"",
							RelationGetRelationName(pb->index))));
		BufFileReadExact(pb->file, pb->readbuf, part->len);
		lion_posbuild_read(&t, pb->readbuf, part->len);
		CHECK_FOR_INTERRUPTS();
	}
	for (b = k->head; b != NULL; b = b->next)
		lion_posbuild_read(&t, b->data, b->used);
	if (pb->npage > 0)
		lion_posbuild_flush_page(&t);

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
	if (pb == NULL || pb->cols[col] == NULL)
		return;
	if (pb->cols[col]->ntaken != pb->cols[col]->nkeys)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("lion index \"%s\": the build has positions for a key it wrote no entry for",
						RelationGetRelationName(pb->index)),
				 errdetail("A column that stores positions needs a deterministic collation.")));
	pb->col = -1;
}

void
lion_posbuild_end(LionPosBuild *pb)
{
	if (pb == NULL)
		return;
	elog(DEBUG1, "lion index \"%s\": the build wrote its positions out %d times",
		 RelationGetRelationName(pb->index), pb->nspills);
	if (pb->file != NULL)
		BufFileClose(pb->file);
	MemoryContextDelete(pb->cxt);
}
