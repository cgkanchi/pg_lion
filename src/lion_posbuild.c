/*-------------------------------------------------------------------------
 *
 * lion_posbuild.c
 *		The build's positions (DESIGN.md §17, "Stored positions"): what a
 *		CREATE INDEX writes for a column whose opclass stores them.
 *
 * The positions travel through the build's spool with the codes
 * (lion_spool.c): each row's positions for a key stay with the row's code
 * through every spill, merge and parallel participant's export, under the
 * spool's one memory budget, and come out of the merge with the key's codes,
 * in code order.  The build hands them here member by member as it writes the
 * key's posting set (lion_build.c), and they come back
 *
 *	- as one chunk when they fit one, which the entry carries when it is
 *	  INLINE and which becomes a one-leaf position tree when it is a CHAIN;
 *	- as a position tree, written bottom up through the build's own page
 *	  writer as the members come, and the entry has to be a CHAIN.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/rel.h"

#include "lion.h"
#include "lion_positions.h"
#include "lion_spool.h"

/* One key's positions being written (see the file header). */
struct LionPosBuild
{
	MemoryContext cxt;
	const LionPageWriter *w;
	uint32		hash;			/* the entry's owner stamp */
	LionContainer *chunk;		/* the members, while they fit one chunk */
	LionPosTail tail;
	struct LionPosLoader *ld;	/* ... else the tree they make */
	uint64		n;				/* members so far */
	uint64		last;			/* the code of the last one */
	LionPosMember m;
};

LionPosBuild *
lion_posbuild_begin(Relation index, LionIndexState *ix)
{
	LionPosBuild *pb;
	MemoryContext cxt;
	int			c;
	bool		any = false;

	for (c = 0; c < ix->ncolumns; c++)
	{
		LionState  *cs = &ix->cols[c];

		if (!cs->positions)
			continue;
		any = true;

		/* the members come in the column's directory order, by key */
		if (cs->typid != TEXTOID || !cs->ordered)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("lion index \"%s\": only a text key column ordered by its comparison can store positions",
							RelationGetRelationName(index))));
	}
	if (!any)
		return NULL;

	cxt = AllocSetContextCreate(CurrentMemoryContext, "lion positions build",
								ALLOCSET_DEFAULT_SIZES);
	pb = (LionPosBuild *) MemoryContextAllocZero(cxt, sizeof(LionPosBuild));
	pb->cxt = cxt;
	pb->chunk = (LionContainer *) MemoryContextAllocZero(cxt,
														 LION_CONTAINER_MAX_SIZE);
	return pb;
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


/* ---------------------------------------------------------------------
 * One key's positions
 * --------------------------------------------------------------------- */

void
lion_posbuild_key_begin(LionPosBuild *pb, const LionPageWriter *w,
						uint32 hash)
{
	Assert(pb->ld == NULL);
	pb->w = w;
	pb->hash = hash;
	pb->n = 0;
	pb->last = 0;
	pb->tail.valid = false;
}

/* The key's next member, its code above every one before it. */
void
lion_posbuild_key_add(LionPosBuild *pb, uint64 code,
					  const LionKeyPositions *kp)
{
	LionPosMember *m = &pb->m;

	if (pb->n > 0 && code <= pb->last)
		elog(ERROR, "lion index: the build has a row's positions out of order for one key");
	lion_posmember_from_key(m, code, kp);
	pb->n++;
	pb->last = code;

	if (pb->ld != NULL)
	{
		lion_posload_add(pb->ld, m);
		return;
	}
	if (pb->n == 1)
	{
		memset(pb->chunk, 0, LION_CONTAINER_MAX_SIZE);
		lion_poschunk_init(pb->chunk, lion_pos_block(code));
		pb->tail.valid = false;
	}
	if (!lion_poschunk_append_tail(pb->chunk, LION_CONTAINER_MAX_SIZE, m,
								   &pb->tail))
	{
		/* more than a chunk: a tree, which starts with these */
		MemoryContext old = MemoryContextSwitchTo(pb->cxt);
		const LionPageWriter *w = pb->w;
		LionPosLoader *ld = (LionPosLoader *) palloc0(sizeof(LionPosLoader));
		LionPosMember *cm = (LionPosMember *) palloc(sizeof(LionPosMember));
		LionPosIter it;

		ld->w = w;
		ld->hash = pb->hash;
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
		pb->ld = ld;
		MemoryContextSwitchTo(old);
	}
}

/*
 * The key's positions, all of them added: exactly one of *chunk (positions
 * that fit one chunk, valid until the next key) and *root (a tree, written)
 * is set; *nmembers says how many rows they cover.
 */
void
lion_posbuild_key_end(LionPosBuild *pb, LionContainer **chunk,
					  BlockNumber *root, uint64 *nmembers)
{
	*chunk = NULL;
	*root = InvalidBlockNumber;
	*nmembers = pb->n;
	if (pb->n == 0)
		elog(ERROR, "lion index: the build has no positions for a key it writes");
	if (pb->ld != NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(pb->cxt);

		lion_posload_end(pb->ld);
		*root = pb->ld->root;
		pfree(pb->ld->pivots);
		pfree(pb->ld->chunk);
		pfree(pb->ld);
		pb->ld = NULL;
		MemoryContextSwitchTo(old);
	}
	else
		*chunk = pb->chunk;
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

void
lion_posbuild_end(LionPosBuild *pb)
{
	if (pb == NULL)
		return;
	MemoryContextDelete(pb->cxt);
}
