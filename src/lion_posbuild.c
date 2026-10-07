/*-------------------------------------------------------------------------
 *
 * lion_posbuild.c
 *		The build's positions (DESIGN.md §17, "Stored positions"): what a
 *		CREATE INDEX writes for a column whose opclass stores them.
 *
 * The build's spool keeps only codes (lion_spool.c), so a column that stores
 * positions hands each key's positions in a row to the sink here, which
 * sorts them in a tuplesort of its own by (key, code), the key in the
 * column's DIRECTORY order: the order lion_build.c writes the column's
 * entries in (DESIGN.md §21).  So when the build writes a key's entry, the
 * key's positions are next in the sort, and lion_posbuild_take() reads them
 * there:
 *
 *	- positions that fit one chunk come back as the chunk, which the entry
 *	  carries when it is INLINE and which becomes a one-leaf position tree
 *	  when it is a CHAIN;
 *	- more come back as a position tree, written bottom up through the build's
 *	  own page writer as they are read, and the entry has to be a CHAIN.
 *
 * Only the order of keys whose comparison ties keys their bytes do not can
 * differ from the directory's - the directory breaks those ties by hash -
 * which for the one key type that stores positions, text lexemes, takes a
 * nondeterministic collation.  The build stops with an ERROR there rather
 * than lose a key's positions.
 *
 * A parallel build does not run the sink in its workers, so a positions
 * column builds serially (lion_build.c).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tupdesc.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tuplesort.h"
#include "varatt.h"

#include "lion.h"
#include "lion_positions.h"
#include "lion_spool.h"

struct LionPosBuild
{
	Relation	index;
	int			ncols;
	Tuplesortstate **sorts;		/* per key column; NULL when it stores none */
	TupleDesc	desc;			/* (key, code, positions): the raw positions
								 * as text, which nothing compares */
	TupleTableSlot *inslot;
	TupleTableSlot *outslot;
	MemoryContext cxt;
	/* the column being written */
	int			col;
	bool		have;			/* outslot holds the next tuple */
	LionContainer *chunk;
	LionPosMember *member;
	uint16	   *posbuf;
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
	pb->col = -1;
	pb->desc = CreateTemplateTupleDesc(3);
	TupleDescInitBuiltinEntry(pb->desc, (AttrNumber) 1, "key", TEXTOID, -1, 0);
	TupleDescInitBuiltinEntry(pb->desc, (AttrNumber) 2, "code", INT8OID, -1, 0);
	TupleDescInitBuiltinEntry(pb->desc, (AttrNumber) 3, "pos", TEXTOID, -1, 0);
	TupleDescFinalize(pb->desc);
	pb->inslot = MakeSingleTupleTableSlot(pb->desc, &TTSOpsVirtual);
	pb->outslot = MakeSingleTupleTableSlot(pb->desc, &TTSOpsMinimalTuple);
	pb->chunk = (LionContainer *) palloc0(LION_CONTAINER_MAX_SIZE);
	pb->member = (LionPosMember *) palloc(sizeof(LionPosMember));
	pb->posbuf = (uint16 *) palloc(sizeof(uint16) * LION_POS_MAX_NPOS);
	pb->sorts = (Tuplesortstate **) palloc0(sizeof(Tuplesortstate *) * pb->ncols);
	for (c = 0; c < ix->ncolumns; c++)
	{
		LionState  *cs = &ix->cols[c];
		AttrNumber	cols[2] = {1, 2};
		Oid			ops[2] = {TextLessOperator, Int8LessOperator};
		Oid			colls[2] = {cs->collation, InvalidOid};
		bool		nf[2] = {false, false};

		if (!cs->positions)
			continue;

		/*
		 * The sort compares keys as text under the column's collation, which
		 * is the directory's order only for a text key column ordered by
		 * the text comparison.
		 */
		if (cs->typid != TEXTOID || !cs->ordered)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("lion index \"%s\": only a text key column ordered by its comparison can store positions",
							RelationGetRelationName(index))));
		if (!OidIsValid(colls[0]))
			colls[0] = DEFAULT_COLLATION_OID;
		pb->sorts[c] = tuplesort_begin_heap(pb->desc, 2, cols, ops, colls, nf,
											workmem, NULL, TUPLESORT_NONE);
	}
	MemoryContextSwitchTo(old);
	return pb;
}

/* LionSpoolPosSink: one key's positions in one row. */
void
lion_posbuild_sink(void *arg, int col, Datum key, uint64 code,
				   const LionKeyPositions *kp)
{
	LionPosBuild *pb = (LionPosBuild *) arg;
	TupleTableSlot *slot = pb->inslot;
	text	   *pos;
	Size		len = sizeof(uint16) * kp->npos;

	pos = (text *) palloc(VARHDRSZ + len);
	SET_VARSIZE(pos, VARHDRSZ + len);
	if (len > 0)
		memcpy(VARDATA(pos), kp->pos, len);

	ExecClearTuple(slot);
	slot->tts_values[0] = key;
	slot->tts_isnull[0] = false;
	slot->tts_values[1] = Int64GetDatum((int64) code);
	slot->tts_isnull[1] = false;
	slot->tts_values[2] = PointerGetDatum(pos);
	slot->tts_isnull[2] = false;
	ExecStoreVirtualTuple(slot);
	tuplesort_puttupleslot(pb->sorts[col], slot);
	pfree(pos);
}

/* Is the build writing positions for this key column? */
bool
lion_posbuild_column(LionPosBuild *pb, int col)
{
	if (pb == NULL || pb->sorts[col] == NULL)
		return false;
	if (pb->col != col)
	{
		pb->col = col;
		tuplesort_performsort(pb->sorts[col]);
		pb->have = tuplesort_gettupleslot(pb->sorts[col], true, false,
										  pb->outslot, NULL);
	}
	return true;
}

/* Does the next tuple of the sort belong to key (the text bytes)? */
static bool
lion_posbuild_at_key(LionPosBuild *pb, const text *key)
{
	bool		isnull;
	text	   *k;

	if (!pb->have)
		return false;
	k = DatumGetTextPP(slot_getattr(pb->outslot, 1, &isnull));
	return VARSIZE_ANY_EXHDR(k) == VARSIZE_ANY_EXHDR(key) &&
		memcmp(VARDATA_ANY(k), VARDATA_ANY(key), VARSIZE_ANY_EXHDR(k)) == 0;
}

/* The next tuple of the sort as a member, and the sort moved past it. */
static void
lion_posbuild_next_member(LionPosBuild *pb)
{
	bool		isnull;
	uint64		code = (uint64) DatumGetInt64(slot_getattr(pb->outslot, 2,
														   &isnull));
	text	   *pos = DatumGetTextPP(slot_getattr(pb->outslot, 3, &isnull));
	Size		poslen = VARSIZE_ANY_EXHDR(pos);
	LionKeyPositions kp;

	if (poslen > sizeof(uint16) * LION_POS_MAX_NPOS || poslen % 2 != 0)
		elog(ERROR, "lion index: a row's positions came back from the sort damaged");
	memcpy(pb->posbuf, VARDATA_ANY(pos), poslen);
	kp.npos = (uint16) (poslen / sizeof(uint16));
	kp.pos = pb->posbuf;
	lion_posmember_from_key(pb->member, code, &kp);
	pb->have = tuplesort_gettupleslot(pb->sorts[pb->col], true, false,
									  pb->outslot, NULL);
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

	if (ld->chunkopen && !lion_poschunk_append(ld->chunk, cap, m))
		lion_posload_close(ld);
	if (!ld->chunkopen)
	{
		memset(ld->chunk, 0, LION_CONTAINER_MAX_SIZE);
		lion_poschunk_init(ld->chunk, lion_pos_block(m->code));
		ld->chunkopen = true;
		if (!lion_poschunk_append(ld->chunk, cap, m))
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

/*
 * The positions of key, which the build is writing the entry of: they are
 * next in the sort of its column (see the file header).  hash is the
 * entry's, which the pages of a tree are stamped with.  Exactly one of
 * *chunk (positions that fit one chunk, valid until the next call) and
 * *root (a tree, written) is set; *nmembers says how many rows they cover.
 */
void
lion_posbuild_take(LionPosBuild *pb, Datum key, uint32 hash,
				   const LionPageWriter *w, LionContainer **chunk,
				   BlockNumber *root, uint64 *nmembers)
{
	text	   *k = DatumGetTextPP(key);
	LionPosLoader *ld = NULL;
	uint64		n = 0;

	*chunk = NULL;
	*root = InvalidBlockNumber;

	if (!lion_posbuild_at_key(pb, k))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("lion index \"%s\": the keys' positions do not sort in the order of their entries",
						RelationGetRelationName(pb->index)),
				 errdetail("A column that stores positions needs a deterministic collation.")));

	memset(pb->chunk, 0, LION_CONTAINER_MAX_SIZE);
	do
	{
		lion_posbuild_next_member(pb);
		n++;
		if (ld != NULL)
			lion_posload_add(ld, pb->member);
		else
		{
			if (n == 1)
				lion_poschunk_init(pb->chunk, lion_pos_block(pb->member->code));
			if (!lion_poschunk_append(pb->chunk, LION_CONTAINER_MAX_SIZE,
									  pb->member))
			{
				/* more than a chunk: a tree, which starts with these */
				LionPosIter it;
				LionPosMember *m = (LionPosMember *) palloc(sizeof(LionPosMember));

				ld = (LionPosLoader *) palloc0(sizeof(LionPosLoader));
				ld->w = w;
				ld->hash = hash;
				ld->root = w->alloc(w->arg);
				ld->chunk = (LionContainer *) palloc0(LION_CONTAINER_MAX_SIZE);
				ld->leafblk = InvalidBlockNumber;
				ld->maxpivots = 64;
				ld->pivots = (LionPostingPivot *)
					palloc(sizeof(LionPostingPivot) * ld->maxpivots);
				lion_posload_newleaf(ld);
				lion_poschunk_iter_init(&it, pb->chunk);
				while (lion_poschunk_iter_next(&it, m))
					lion_posload_add(ld, m);
				lion_posload_add(ld, pb->member);
				pfree(m);
			}
		}
		if ((n & 0xFFFF) == 0)
			CHECK_FOR_INTERRUPTS();
	} while (lion_posbuild_at_key(pb, k));

	if (ld != NULL)
	{
		lion_posload_end(ld);
		*root = ld->root;
		pfree(ld->pivots);
		pfree(ld->chunk);
		pfree(ld);
	}
	else
		*chunk = pb->chunk;
	*nmembers = n;
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

/* The column is written: every position in its sort has found its key. */
void
lion_posbuild_column_done(LionPosBuild *pb, int col)
{
	if (pb == NULL || pb->sorts[col] == NULL)
		return;
	if (pb->have)
		elog(ERROR, "lion index \"%s\": the build has positions for a key it wrote no entry for",
			 RelationGetRelationName(pb->index));
	tuplesort_end(pb->sorts[col]);
	pb->sorts[col] = NULL;
}

void
lion_posbuild_end(LionPosBuild *pb)
{
	int			c;

	if (pb == NULL)
		return;
	for (c = 0; c < pb->ncols; c++)
		if (pb->sorts[c] != NULL)
			tuplesort_end(pb->sorts[c]);
	ExecDropSingleTupleTableSlot(pb->inslot);
	ExecDropSingleTupleTableSlot(pb->outslot);
	MemoryContextDelete(pb->cxt);
}
