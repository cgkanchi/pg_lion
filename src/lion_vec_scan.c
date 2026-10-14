/*-------------------------------------------------------------------------
 *
 * lion_vec_scan.c
 *		The lionvec scan: rows in order of their distance to a query
 *		(DESIGN.md §44.4).
 *
 * A pass reads plane 1 of every row and keeps, as candidates, the rows
 * whose lower bound on the key (lion_vec_math.h) is no more than tau, the
 * L-th smallest upper bound: at least L rows lie within tau, so no row
 * beyond it can be among the next L.  The candidates then go into a
 * priority queue by their lower bounds, beside one entry, REST, that stands
 * for every row the pass left out, at tau.  The queue is worked lazily:
 *
 *	the least entry is a row known from plane 1	- read its REFINE item, and
 *		put it back with the tighter lower bound of its 8-bit codes
 *	... a row known from its codes				- fetch it from the heap,
 *		compute its distance exactly with the operator's own function, and
 *		put it back with that
 *	... a row whose distance is exact			- no row left can be nearer:
 *		return it
 *	... REST									- make another pass, for four
 *		times as many rows, past the rows already returned
 *
 * so a LIMIT k query refines about the rows plane 1 cannot tell from the
 * k-th, and computes the distance of about k + a few.  The bounds hold
 * with the probability pg_lion.vector_epsilon sets; a row whose bound fails
 * and turns up after rows farther than it is skipped, so the scan never
 * returns rows out of order.
 *
 * NULL rows come last, with NULL distances; before them, in a cosine index,
 * the zero vectors, whose distance is NaN.  A restricted scan (LionOrdered,
 * §44.6) does all of this over the members of lion's TID set alone, and of
 * the CODE pages the build wrote reads only those the directory says can
 * hold one (lv_restrict_pages()).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/relscan.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "utils/float.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "lion_compat.h"
#include "lion_vec.h"

/* what an entry of the queue knows of its row */
#define LV_REST		0			/* not a row: every row the pass left out */
#define LV_PLANE1	1
#define LV_CODES8	2
#define LV_EXACT	3

typedef struct LvItem
{
	double		key;			/* a lower bound on the key, or the key */
	double		dist;			/* LV_EXACT: the distance */
	ItemPointerData tid;
	uint8		level;
	OffsetNumber roff;
	BlockNumber rblk;
	float4		norm;
	float4		xm;
} LvItem;

/*
 * What the scan has learnt of a row, kept from pass to pass: the next pass
 * starts a refined row from its codes' bounds and a fetched one from its
 * key, not from plane 1 again.  Without that, a pass cut short by work_mem
 * (lv_cut()) whose rows all refine past the cut would be made again, the
 * same, for ever: the rows would come back from plane 1 below the cut, be
 * refined past it, and the cut's REST come out first.  With it, every pass
 * moves at least one row up a level, or out.
 */
#define LV_GONE		0			/* returned already, or not there */

typedef struct LvSeen
{
	uint64		key;
	char		status;			/* simplehash's */
	uint8		level;			/* LV_CODES8, LV_EXACT, or LV_GONE */
	double		lo;				/* the key's bounds; LV_EXACT: the key */
	double		hi;
	double		dist;			/* LV_EXACT: the distance */
} LvSeen;

#define SH_PREFIX lvseen
#define SH_ELEMENT_TYPE LvSeen
#define SH_KEY_TYPE uint64
#define SH_KEY key
#define SH_HASH_KEY(tb, key) murmurhash32((uint32) (key) ^ (uint32) ((key) >> 32) * 0x9E3779B1u)
#define SH_EQUAL(tb, a, b) ((a) == (b))
#define SH_SCOPE static inline
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

/*
 * Where a pass or a walk is in the CODE pages: the chain from code_head, or,
 * restricted, the listed pages of the build's run (so->rpages) and then the
 * chain from the run's last page, which inserts append to.
 */
typedef struct LvPages
{
	bool		listed;			/* the page last returned came from the list */
	int			pos;			/* the next listed page */
} LvPages;

typedef enum LvPhase
{
	LV_START,
	LV_NORMAL,					/* the queue */
	LV_WALK,					/* the chain, by flags */
	LV_DONE
} LvPhase;

/* which rows a walk of the chain returns, and with what distance */
typedef enum LvWalk
{
	LV_WALK_ZERO,				/* cosine zero vectors: NaN */
	LV_WALK_NONNULL,			/* every row not NULL: NaN (a zero query) */
	LV_WALK_NULL,				/* NULL rows: NULL */
	LV_WALK_ALL					/* every row: NULL (a NULL query, or none) */
} LvWalk;

typedef struct LvScanOpaque
{
	MemoryContext cxt;			/* the scan's */
	MemoryContext qcxt;			/* a rescan's, reset by the next */
	LvPhase		phase;

	/* the query */
	LionVecInfo *info;
	int			metric;
	Datum		query;			/* detoasted, in qcxt */
	LionVecQuery q;

	/* the restriction (lion_vec_restrict()) */
	bool		restricted;
	int			rn;
	const uint32 *rkeys;
	LionContainer *const *rconts;
	int			rpos;			/* the last key found */
	bool		rlisted;		/* the pages below are in use */
	BlockNumber *rpages;		/* the build's pages that can hold a member */
	int			nrpages;
	BlockNumber rtail;			/* the build's last page, then the chain */
	LionVecDirEntry *dir;		/* the directory (lion_vec.h), for checking */
	uint32		nbuilt;

	/* the queue, and a pass's bookkeeping */
	LvItem	   *pq;
	int			npq;
	int			maxpq;
	double	   *hi;				/* max-heap of the L least upper bounds */
	int			maxhi;
	int			batch;			/* L of the next pass */
	lvseen_hash *seen;
	double		lastdist;		/* of the row returned last */
	BlockNumber code_head;

	/* the walks */
	LvWalk		walk;
	LvPages		wpages;
	BlockNumber wblk;
	uint64		wvisited;		/* lionvec_chain_step()'s */
	BlockNumber wnblocks;
	ItemPointerData *wtids;		/* [MaxOffsetNumber] */
	int			wn;
	int			wpos;

	/* the heap side of an exact distance */
	TupleTableSlot *slot;
#if PG_VERSION_NUM < 200000
	struct IndexFetchTableData *fetch;
#endif
	IndexInfo  *indexInfo;
	EState	   *estate;

	/* for DEBUG1 */
	int64		npasses;
	int64		ncands;
	int64		nrefined;
	int64		nfetched;
	int64		nreturned;
	int64		nskipped;
	int64		nmissed;		/* exact keys below the bound they had */
	int64		npages;			/* CODE pages the passes read */
} LvScanOpaque;

/* ----------------------------------------------------------------
 * The queue: a binary min-heap of LvItems
 * ----------------------------------------------------------------
 */

/*
 * a comes out before b: by key, and at equal keys the better-known first, so
 * that a row known exactly is returned before REST or a bound at its key
 * (neither can hold a nearer row); then by distance and TID, which makes the
 * order of ties the same from one run to the next.
 */
static inline bool
lv_before(const LvItem *a, const LvItem *b)
{
	if (a->key != b->key)
		return a->key < b->key;
	if (a->level != b->level)
		return a->level > b->level;
	if (a->level == LV_EXACT && a->dist != b->dist)
		return a->dist < b->dist;
	return ItemPointerCompare((ItemPointer) &a->tid, (ItemPointer) &b->tid) < 0;
}

static void
lv_sift_down(LvItem *pq, int n, int i)
{
	LvItem		x = pq[i];

	for (;;)
	{
		int			c = 2 * i + 1;

		if (c >= n)
			break;
		if (c + 1 < n && lv_before(&pq[c + 1], &pq[c]))
			c++;
		if (!lv_before(&pq[c], &x))
			break;
		pq[i] = pq[c];
		i = c;
	}
	pq[i] = x;
}

static void
lv_pq_grow(LvScanOpaque *so, int need)
{
	if (need <= so->maxpq)
		return;
	if ((Size) need > MaxAllocSize / sizeof(LvItem) / 2)
		elog(ERROR, "lionvec: too many candidates");
	so->maxpq = Max(need, so->maxpq * 2);
	so->pq = repalloc(so->pq, sizeof(LvItem) * so->maxpq);
}

static void
lv_pq_push(LvScanOpaque *so, const LvItem *item)
{
	int			i;

	lv_pq_grow(so, so->npq + 1);
	i = so->npq++;
	while (i > 0)
	{
		int			p = (i - 1) / 2;

		if (!lv_before(item, &so->pq[p]))
			break;
		so->pq[i] = so->pq[p];
		i = p;
	}
	so->pq[i] = *item;
}

static LvItem
lv_pq_pop(LvScanOpaque *so)
{
	LvItem		top = so->pq[0];

	Assert(so->npq > 0);
	so->npq--;
	if (so->npq > 0)
	{
		so->pq[0] = so->pq[so->npq];
		lv_sift_down(so->pq, so->npq, 0);
	}
	return top;
}

/* ----------------------------------------------------------------
 * The max-heap of upper bounds a pass keeps
 * ----------------------------------------------------------------
 */

static void
lv_hi_push(double *h, int *n, int max, double v)
{
	int			i;

	if (*n < max)
	{
		i = (*n)++;
		while (i > 0 && h[(i - 1) / 2] < v)
		{
			h[i] = h[(i - 1) / 2];
			i = (i - 1) / 2;
		}
		h[i] = v;
		return;
	}
	if (v >= h[0])
		return;
	i = 0;
	for (;;)
	{
		int			c = 2 * i + 1;

		if (c >= *n)
			break;
		if (c + 1 < *n && h[c + 1] > h[c])
			c++;
		if (h[c] <= v)
			break;
		h[i] = h[c];
		i = c;
	}
	h[i] = v;
}

/* ----------------------------------------------------------------
 * The restriction
 * ----------------------------------------------------------------
 */

void
lion_vec_restrict(IndexScanDesc scan, int n, const uint32 *keys,
				  LionContainer *const *conts)
{
	LvScanOpaque *so;
	int			i;

	if (!lion_vec_relation(scan->indexRelation))
		elog(ERROR, "lionvec: \"%s\" is not a lionvec index",
			 RelationGetRelationName(scan->indexRelation));
	so = (LvScanOpaque *) scan->opaque;
	if (so->phase != LV_START)
		elog(ERROR, "lionvec: a scan is restricted after it has started");
	if (n < 0 || (n > 0 && (keys == NULL || conts == NULL)))
		elog(ERROR, "lionvec: malformed restriction");
	/* lv_key_search() bisects: one pass over the keys is cheap next to a scan */
	for (i = 1; i < n; i++)
	{
		if (keys[i] <= keys[i - 1])
			elog(ERROR, "lionvec: restriction keys out of order");
	}
	so->restricted = true;
	so->rn = n;
	so->rkeys = keys;
	so->rconts = conts;
	so->rpos = 0;
}

/* The first key at or after ckey, or rn. */
static int
lv_key_search(const LvScanOpaque *so, uint32 ckey)
{
	int			lo = 0;
	int			hi = so->rn;

	while (lo < hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (so->rkeys[mid] < ckey)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* Does a CODE page with these container keys hold any member? */
static bool
lv_page_has_member(const LvScanOpaque *so, uint32 minkey, uint32 maxkey)
{
	int			i;

	if (minkey > maxkey)
		return false;			/* nothing was ever put on it */
	i = lv_key_search(so, minkey);
	return i < so->rn && so->rkeys[i] <= maxkey;
}

static bool
lv_member(LvScanOpaque *so, ItemPointer tid)
{
	uint64		code;
	uint32		ckey;

	if (ItemPointerGetOffsetNumber(tid) > LION_MAX_OFFSET)
		return false;			/* not a heap TID lion can hold */
	code = lion_tid_to_code(tid);
	ckey = lion_code_ckey(code);
	/* rows come in runs of one key, so the last one found is tried first */
	if (so->rpos >= so->rn || so->rkeys[so->rpos] != ckey)
	{
		int			i = lv_key_search(so, ckey);

		if (i >= so->rn || so->rkeys[i] != ckey)
			return false;
		so->rpos = i;
	}
	return so->rconts[so->rpos] == NULL ||
		lion_container_contains(so->rconts[so->rpos], lion_code_lo(code));
}

/*
 * The pages of the build's run a restricted scan reads (DESIGN.md §44.6):
 * those the directory says can hold a member.  The members go to
 * lionvec_dir_mark() a container at a time, as runs of TID keys; a NULL
 * container stands for every TID of its key's blocks.  Without a
 * directory, or with a run of one page, the scan walks the chain as an
 * unrestricted one does.
 */
static void
lv_restrict_pages(IndexScanDesc scan, LvScanOpaque *so,
				  const LionVecMetaData *meta)
{
	uint16	   *los;
	uint64	   *lo;
	uint64	   *hi;
	uint64	   *sel;
	int			nwords;
	int			from = 0;
	int			n = 0;
	int			k;
	int			i;

	so->rlisted = false;
	if (meta->nbuilt < 2)
		return;
	so->dir = lionvec_read_dir(scan->indexRelation, meta);
	so->nbuilt = meta->nbuilt;

	/* a container's members, and as many runs as it can make */
	los = palloc(sizeof(uint16) * LION_CONTAINER_RANGE);
	lo = palloc(sizeof(uint64) * LION_CONTAINER_RANGE);
	hi = palloc(sizeof(uint64) * LION_CONTAINER_RANGE);
	nwords = (int) ((meta->nbuilt - 1 + 63) / 64);
	sel = palloc0(sizeof(uint64) * Max(nwords, 1));
	for (k = 0; k < so->rn; k++)
	{
		uint64		base = (uint64) so->rkeys[k] * LION_BLOCKS_PER_CONTAINER;
		int			nr = 0;

		CHECK_FOR_INTERRUPTS();
		if (base > MaxBlockNumber)
			break;
		if (so->rconts[k] == NULL)
		{
			uint64		last = Min(base + LION_BLOCKS_PER_CONTAINER - 1,
								   (uint64) MaxBlockNumber);

			lo[0] = LIONVEC_TID_KEY(base, 0);
			hi[0] = LIONVEC_TID_KEY(last, PG_UINT16_MAX);
			nr = 1;
		}
		else
		{
			uint32		m = lion_container_to_array(so->rconts[k], los);
			uint32		j;

			for (j = 0; j < m; j++)
			{
				uint64		blk = base + (los[j] >> LION_OFFSET_BITS);
				uint64		key;

				if (blk > MaxBlockNumber)
					break;
				key = LIONVEC_TID_KEY(blk, los[j] & LION_MAX_OFFSET);
				if (nr > 0 && hi[nr - 1] + 1 == key)
					hi[nr - 1] = key;
				else if (nr == 0 || hi[nr - 1] < key)
				{
					lo[nr] = hi[nr] = key;
					nr++;
				}
				/* else out of order: not a well-formed container */
				else
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("lionvec: a restriction's members are out of order")));
			}
		}
		n += lionvec_dir_mark(so->dir, (int) meta->nbuilt, lo, hi, nr, &from, sel);
	}

	so->rpages = palloc(sizeof(BlockNumber) * Max(n, 1));
	so->nrpages = 0;
	for (i = 0; i < (int) meta->nbuilt - 1; i++)
	{
		if (sel[i / 64] & (UINT64CONST(1) << (i % 64)))
			so->rpages[so->nrpages++] = meta->code_head + (BlockNumber) i;
	}
	Assert(so->nrpages == n);
	so->rtail = meta->code_head + meta->nbuilt - 1;
	so->rlisted = true;
	pfree(sel);
	pfree(hi);
	pfree(lo);
	pfree(los);
}

/* The first CODE page to read. */
static BlockNumber
lv_pages_first(LvScanOpaque *so, LvPages *it)
{
	it->pos = 0;
	it->listed = so->rlisted;
	if (!it->listed)
		return so->code_head;
	if (it->pos < so->nrpages)
		return so->rpages[it->pos++];
	it->listed = false;
	return so->rtail;
}

/* The next, after one whose chain goes on to next. */
static BlockNumber
lv_pages_next(LvScanOpaque *so, LvPages *it, BlockNumber next)
{
	if (!it->listed)
		return next;
	if (it->pos < so->nrpages)
		return so->rpages[it->pos++];
	it->listed = false;
	return so->rtail;
}

/*
 * A row of a listed page must lie where the directory says, or a page the
 * scan skipped may hold a member too.
 */
static inline void
lv_check_listed(IndexScanDesc scan, LvScanOpaque *so, BlockNumber blk,
				ItemPointer tid)
{
	uint32		i = blk - so->code_head;
	uint64		key = LIONVEC_TID_KEY(ItemPointerGetBlockNumber(tid),
									  ItemPointerGetOffsetNumber(tid));

	Assert(i + 1 < so->nbuilt);
	if (key < so->dir[i].lo || key > so->dir[i].hi)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has a row of heap block %u at block %u, outside the range its directory gives",
						RelationGetRelationName(scan->indexRelation),
						ItemPointerGetBlockNumber(tid), blk)));
}

/* ----------------------------------------------------------------
 * Begin, rescan, end
 * ----------------------------------------------------------------
 */

IndexScanDesc
lionvecbeginscan(Relation index, int nkeys, int norderbys)
{
	IndexScanDesc scan = RelationGetIndexScan(index, nkeys, norderbys);
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext,
											  "lionvec scan",
											  ALLOCSET_DEFAULT_SIZES);
	LvScanOpaque *so = MemoryContextAllocZero(cxt, sizeof(LvScanOpaque));

	so->cxt = cxt;
	so->qcxt = AllocSetContextCreate(cxt, "lionvec query",
									 ALLOCSET_DEFAULT_SIZES);
	so->phase = LV_START;
	so->wtids = MemoryContextAlloc(cxt, sizeof(ItemPointerData) * MaxOffsetNumber);
	scan->opaque = so;
	if (norderbys > 0)
	{
		scan->xs_orderbyvals = MemoryContextAllocZero(cxt, sizeof(Datum) * norderbys);
		scan->xs_orderbynulls = MemoryContextAlloc(cxt, sizeof(bool) * norderbys);
		memset(scan->xs_orderbynulls, true, sizeof(bool) * norderbys);
	}
	return scan;
}

static void
lv_debug_stats(IndexScanDesc scan, LvScanOpaque *so)
{
	if (so->npasses > 0)
		elog(DEBUG1, "lionvec scan of \"%s\": " INT64_FORMAT " passes, "
			 INT64_FORMAT " candidates, " INT64_FORMAT " refined, "
			 INT64_FORMAT " fetched, " INT64_FORMAT " returned, "
			 INT64_FORMAT " skipped out of order, " INT64_FORMAT " bounds missed, "
			 INT64_FORMAT " pages read",
			 RelationGetRelationName(scan->indexRelation), so->npasses,
			 so->ncands, so->nrefined, so->nfetched, so->nreturned,
			 so->nskipped, so->nmissed, so->npages);
	so->npasses = so->ncands = so->nrefined = so->nfetched = 0;
	so->nreturned = so->nskipped = so->nmissed = so->npages = 0;
}

void
lionvecrescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys,
			  int norderbys)
{
	LvScanOpaque *so = (LvScanOpaque *) scan->opaque;

	lv_debug_stats(scan, so);
	if (keys && scan->numberOfKeys > 0)
		memmove(scan->keyData, keys, scan->numberOfKeys * sizeof(ScanKeyData));
	if (orderbys && scan->numberOfOrderBys > 0)
		memmove(scan->orderByData, orderbys,
				scan->numberOfOrderBys * sizeof(ScanKeyData));

	MemoryContextReset(so->qcxt);
	so->phase = LV_START;
	so->info = NULL;
	so->restricted = false;
	so->rn = 0;
	so->rkeys = NULL;
	so->rconts = NULL;
	so->rlisted = false;
	so->rpages = NULL;
	so->nrpages = 0;
	so->dir = NULL;
	so->nbuilt = 0;
	so->pq = NULL;
	so->npq = so->maxpq = 0;
	so->hi = NULL;
	so->maxhi = 0;
	so->seen = NULL;
	so->wn = so->wpos = 0;
}

void
lionvecendscan(IndexScanDesc scan)
{
	LvScanOpaque *so = (LvScanOpaque *) scan->opaque;

	lv_debug_stats(scan, so);
	if (so->slot != NULL)
		ExecDropSingleTupleTableSlot(so->slot);
#if PG_VERSION_NUM < 200000
	if (so->fetch != NULL)
		table_index_fetch_end(so->fetch);
#endif
	if (so->estate != NULL)
		FreeExecutorState(so->estate);
	MemoryContextDelete(so->cxt);
	scan->opaque = NULL;
}

/* ----------------------------------------------------------------
 * Starting: the query
 * ----------------------------------------------------------------
 */

static void
lv_start_walk(LvScanOpaque *so, LvWalk walk)
{
	so->phase = LV_WALK;
	so->walk = walk;
	so->wblk = lv_pages_first(so, &so->wpages);
	so->wvisited = 0;
	so->wnblocks = 0;
	so->wn = so->wpos = 0;
}

/* What follows the queue, or a walk: the zero vectors, the NULLs, the end. */
static void
lv_next_phase(LvScanOpaque *so)
{
	if (so->phase == LV_NORMAL && so->metric == LIONVEC_COSINE)
		lv_start_walk(so, LV_WALK_ZERO);
	else if (so->phase == LV_NORMAL ||
			 (so->phase == LV_WALK &&
			  (so->walk == LV_WALK_ZERO || so->walk == LV_WALK_NONNULL)))
		lv_start_walk(so, LV_WALK_NULL);
	else
		so->phase = LV_DONE;
}

static void
lv_start(IndexScanDesc scan, LvScanOpaque *so)
{
	Relation	index = scan->indexRelation;
	MemoryContext old = MemoryContextSwitchTo(so->qcxt);
	LionVecMetaData meta;
	ScanKey		ob;
	Pointer		tofree;
	const float *v;
	int			dim;
	float	   *x;

	if (scan->xs_snapshot == InvalidSnapshot ||
		!LION_IS_MVCC_LIKE(scan->xs_snapshot))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("lionvec index \"%s\" can only be scanned with an MVCC snapshot",
						RelationGetRelationName(index))));

	so->info = lionvec_get_info(index);
	so->metric = so->info->metric;
	lionvec_read_meta(index, &meta);
	so->code_head = meta.code_head;
	so->lastdist = -INFINITY;
	if (so->restricted)
		lv_restrict_pages(scan, so, &meta);

	/* no order to give: every row, in the chain's order */
	if (scan->numberOfOrderBys == 0)
	{
		lv_start_walk(so, LV_WALK_ALL);
		MemoryContextSwitchTo(old);
		return;
	}

	ob = &scan->orderByData[0];
	if (ob->sk_strategy != so->metric)
		elog(ERROR, "lionvec: index \"%s\" orders by strategy %d, not %d",
			 RelationGetRelationName(index), so->metric, ob->sk_strategy);
	if (ob->sk_flags & SK_ISNULL)
	{
		lv_start_walk(so, LV_WALK_ALL);
		MemoryContextSwitchTo(old);
		return;
	}

	v = lionvec_datum_floats(ob->sk_argument, &dim, &tofree);
	so->query = tofree != NULL ? PointerGetDatum(tofree) : ob->sk_argument;
	if (so->info->dims == 0)
	{
		/* no vector indexed yet: only NULLs to return */
		lv_start_walk(so, LV_WALK_NULL);
		MemoryContextSwitchTo(old);
		return;
	}
	/* pgvector's error for a query of the wrong size */
	if (dim != so->info->dims)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("different vector dimensions %d and %d",
						so->info->dims, dim)));

	x = palloc(sizeof(float) * so->info->dimsp);
	if (!lionvec_prepare(&so->info->rot, so->metric, v, dim, x))
	{
		/* cosine of a zero query: NaN to every vector, NULL to NULLs */
		lv_start_walk(so, LV_WALK_NONNULL);
		MemoryContextSwitchTo(old);
		return;
	}
	so->q.dimsp = so->info->dimsp;
	so->q.qc = palloc(sizeof(float) * so->info->dimsp);
	so->q.planes = palloc0(sizeof(uint64) * (so->info->dimsp / 64) * LIONVEC_QBITS);
	lionvec_query_init(&so->q, so->metric, so->info->mean, x, lionvec_epsilon);

	so->seen = lvseen_create(so->qcxt, 256, NULL);
	so->batch = lionvec_batch;
	so->maxpq = 1024;
	so->pq = palloc(sizeof(LvItem) * so->maxpq);
	so->npq = 0;
	{
		/* the first pass is made when this comes out of the queue */
		LvItem		rest;

		memset(&rest, 0, sizeof(rest));
		rest.key = -INFINITY;
		rest.level = LV_REST;
		lv_pq_push(so, &rest);
	}
	so->phase = LV_NORMAL;
	MemoryContextSwitchTo(old);
}

/* ----------------------------------------------------------------
 * A pass over plane 1
 * ----------------------------------------------------------------
 */

static inline uint64
lv_tidkey(ItemPointer tid)
{
	return ((uint64) ItemPointerGetBlockNumber(tid) << 16) |
		ItemPointerGetOffsetNumber(tid);
}

/*
 * Keep the candidates under cap by cutting at a key: the cap / 2 with the
 * least lower bounds stay, the cut is the least bound of those that went,
 * and the rest of the pass takes only rows below it; REST then stands for
 * everything else, at the cut.  Rows kept AT the cut stay - REST comes out
 * after every row at its key - which is what makes a pass over a million
 * equal vectors return cap / 2 of them, rather than none.
 */
static double
lv_cut(LvScanOpaque *so, int cap)
{
	int			keep = cap / 2;
	int			i;
	double		cut;

	/* the least lower bounds to the front: a heap of all, popped keep times */
	for (i = so->npq / 2 - 1; i >= 0; i--)
		lv_sift_down(so->pq, so->npq, i);
	{
		LvItem	   *sorted = palloc(sizeof(LvItem) * keep);
		int			n = so->npq;

		for (i = 0; i < keep; i++)
		{
			sorted[i] = so->pq[0];
			so->pq[0] = so->pq[--n];
			lv_sift_down(so->pq, n, 0);
		}
		cut = so->pq[0].key;
		memcpy(so->pq, sorted, sizeof(LvItem) * keep);
		so->npq = keep;
		pfree(sorted);
	}
	return cut;
}

static void
lv_pass(IndexScanDesc scan, LvScanOpaque *so)
{
	Relation	index = scan->indexRelation;
	int			dimsp = so->info->dimsp;
	int			L = so->batch;
	int			nhi = 0;
	double		tau = INFINITY;
	double		cut = INFINITY;
	int			cap;
	BlockNumber blk;
	LvPages		pages;
	BlockNumber nblocks = RelationGetNumberOfBlocks(index);
	uint64		visited = 0;
	bool		anyseen = so->seen->members > 0;
	int64		cands = 0;

	/* what work_mem holds of candidates, and never fewer than four batches */
	cap = (int) Min((double) work_mem * 1024.0 / sizeof(LvItem),
					(double) (MaxAllocSize / sizeof(LvItem) / 2));
	cap = Max(cap, 4 * L + 64);

	so->npasses++;
	so->npq = 0;
	if (so->maxhi < L)
	{
		so->hi = so->hi == NULL ?
			MemoryContextAlloc(so->qcxt, sizeof(double) * L) :
			repalloc(so->hi, sizeof(double) * L);
		so->maxhi = L;
	}

	for (blk = lv_pages_first(so, &pages); BlockNumberIsValid(blk);)
	{
		Buffer		buf;
		Page		page;
		LionVecPageOpaque op;
		OffsetNumber off;
		OffsetNumber maxoff;
		BlockNumber next;
		bool		listed = pages.listed;

		CHECK_FOR_INTERRUPTS();
		lionvec_chain_step(index, &visited, &nblocks);
		buf = ReadBuffer(index, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		lionvec_check_page(index, page, blk, LIONVEC_PAGE_CODE);
		op = LionVecPageGetOpaque(page);
		next = op->next;
		so->npages++;
		if (so->restricted && !lv_page_has_member(so, op->minkey, op->maxkey))
		{
			UnlockReleaseBuffer(buf);
			blk = lv_pages_next(so, &pages, next);
			continue;
		}

		maxoff = PageGetMaxOffsetNumber(page);
		for (off = FirstOffsetNumber; off <= maxoff; off = OffsetNumberNext(off))
		{
			LionVecCode *code = lionvec_code_at(index, page, blk, off, dimsp);
			LvItem		item;
			double		lo;
			double		hi;

			if (listed)
				lv_check_listed(scan, so, blk, &code->tid);
			if (code->flags & (LIONVEC_NULL | LIONVEC_ZERO))
				continue;
			if (so->restricted && !lv_member(so, &code->tid))
				continue;

			item.tid = code->tid;
			item.rblk = code->rblk;
			item.roff = code->roff;
			item.norm = code->norm;
			item.xm = code->xm;
			item.dist = 0.0;
			item.level = LV_PLANE1;
			if (anyseen)
			{
				LvSeen	   *s = lvseen_lookup(so->seen, lv_tidkey(&code->tid));

				if (s != NULL && s->level == LV_GONE)
					continue;
				if (s != NULL)
				{
					item.level = s->level;
					item.dist = s->dist;
					lo = s->lo;
					hi = s->hi;
				}
			}
			if (item.level == LV_PLANE1)
			{
				double		est;
				double		err;

				if (!BlockNumberIsValid(code->rblk))
					ereport(ERROR,
							(errcode(ERRCODE_INDEX_CORRUPTED),
							 errmsg("lionvec index \"%s\" has a row with no refine item at (%u,%u)",
									RelationGetRelationName(index), blk, off)));
				lionvec_estimate1(&so->q, code->bits, code->popcount,
								  code->norm, code->alpha1, &est, &err);
				lionvec_key_bounds(so->metric, code->norm, code->xm, est, err,
								   &lo, &hi);
			}

			if (lo > tau || lo >= cut)
				continue;
			lv_hi_push(so->hi, &nhi, L, hi);
			if (nhi == L)
				tau = so->hi[0];
			if (lo > tau)
				continue;
			item.key = lo;
			if (so->npq >= cap)
			{
				int			i;
				int			n = 0;

				/* drop what tau has passed by since, and cut if that is not enough */
				for (i = 0; i < so->npq; i++)
					if (so->pq[i].key <= tau && so->pq[i].key <= cut)
						so->pq[n++] = so->pq[i];
				so->npq = n;
				if (so->npq > cap / 2)
					cut = Min(cut, lv_cut(so, cap));
				if (lo >= cut)
					continue;
			}
			lv_pq_grow(so, so->npq + 1);
			so->pq[so->npq++] = item;
			cands++;
		}
		UnlockReleaseBuffer(buf);
		blk = lv_pages_next(so, &pages, next);
	}

	/* the candidates tau has passed by since go, and the rest are a heap */
	{
		int			i;
		int			n = 0;

		for (i = 0; i < so->npq; i++)
			if (so->pq[i].key <= tau && so->pq[i].key <= cut)
				so->pq[n++] = so->pq[i];
		so->npq = n;
		for (i = so->npq / 2 - 1; i >= 0; i--)
			lv_sift_down(so->pq, so->npq, i);
	}
	so->ncands += cands;

	/* every row this pass left out has a lower bound past REST's */
	if (nhi == L || cut < INFINITY)
	{
		LvItem		rest;

		memset(&rest, 0, sizeof(rest));
		rest.key = Min(tau, cut);
		rest.level = LV_REST;
		lv_pq_push(so, &rest);
	}

	/* the next pass, if there is one, for four times as many rows */
	so->batch = (int) Min((double) L * 4.0,
						  Max((double) L, (double) cap / 8.0));
}

/* ----------------------------------------------------------------
 * Refining, and the exact distance
 * ----------------------------------------------------------------
 */

static void
lv_remember(LvScanOpaque *so, ItemPointer tid, uint8 level, double lo,
			double hi, double dist)
{
	bool		found;
	LvSeen	   *s = lvseen_insert(so->seen, lv_tidkey(tid), &found);

	s->level = level;
	s->lo = lo;
	s->hi = hi;
	s->dist = dist;
}

static void
lv_mark_done(LvScanOpaque *so, ItemPointer tid)
{
	lv_remember(so, tid, LV_GONE, 0.0, 0.0, 0.0);
}

static void
lv_refine(IndexScanDesc scan, LvScanOpaque *so, LvItem *item)
{
	Relation	index = scan->indexRelation;
	Buffer		buf = ReadBuffer(index, item->rblk);
	Page		page;
	LionVecRefine *ref;
	double		est;
	double		err;
	double		lo;
	double		hi;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	lionvec_check_page(index, page, item->rblk, LIONVEC_PAGE_REFINE);
	ref = lionvec_refine_at(index, page, item->rblk, item->roff,
							so->info->dimsp, &item->tid);
	if (ref == NULL)
	{
		/* VACUUM has removed the row since the pass read it (§44.5) */
		UnlockReleaseBuffer(buf);
		lv_mark_done(so, &item->tid);
		return;
	}
	lionvec_estimate8(&so->q, ref->codes, item->norm, ref->alpha8,
					  ref->rnorm8, &est, &err);
	UnlockReleaseBuffer(buf);
	so->nrefined++;

	/*
	 * The codes' bound replaces plane 1's rather than tightening it: it is
	 * the better by two orders of magnitude, and keeping the larger of the
	 * two would keep plane 1's failures too.
	 */
	lionvec_key_bounds(so->metric, item->norm, item->xm, est, err, &lo, &hi);
	item->key = lo;
	item->level = LV_CODES8;
	lv_remember(so, &item->tid, LV_CODES8, lo, hi, 0.0);
	lv_pq_push(so, item);
}

/*
 * The distance of the row from the heap, with the operator's own function,
 * so that it is exactly the distance the query's ORDER BY computes; false if
 * no version of the row is visible to the scan's snapshot.  Every version of
 * a HOT chain has the same vector, as the index's column is not one a HOT
 * update can change.
 */
static bool
lv_fetch_distance(IndexScanDesc scan, LvScanOpaque *so, ItemPointer tid,
				  double *dist)
{
	Relation	heap = scan->heapRelation;
	ItemPointerData htid = *tid;
	bool		all_dead = false;
	bool		found;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	ScanKey		ob = &scan->orderByData[0];
	ExprContext *econtext;
	MemoryContext old;

	if (heap == NULL)
		elog(ERROR, "lionvec: an ordered scan needs its table");
	if (so->slot == NULL)
	{
		old = MemoryContextSwitchTo(so->cxt);
		so->slot = table_slot_create(heap, NULL);
#if PG_VERSION_NUM >= 200000
#elif PG_VERSION_NUM >= 190000
		so->fetch = table_index_fetch_begin(heap, SO_NONE);
#else
		so->fetch = table_index_fetch_begin(heap);
#endif
		so->indexInfo = BuildIndexInfo(scan->indexRelation);
		so->estate = CreateExecutorState();
		MemoryContextSwitchTo(old);
	}

#if PG_VERSION_NUM >= 200000
	found = table_fetch_tid(heap, &htid, scan->xs_snapshot, &all_dead) &&
		table_tuple_fetch_row_version(heap, &htid, scan->xs_snapshot, so->slot);
#else
	{
		bool		call_again = false;

		found = table_index_fetch_tuple(so->fetch, &htid, scan->xs_snapshot,
										so->slot, &call_again, &all_dead);
	}
#endif
	so->nfetched++;
	if (!found)
		return false;

	econtext = GetPerTupleExprContext(so->estate);
	ResetExprContext(econtext);
	econtext->ecxt_scantuple = so->slot;
	old = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
	FormIndexDatum(so->indexInfo, so->slot, so->estate, values, isnull);
	if (isnull[0])
	{
		/* not the row the index has: there is none to order */
		MemoryContextSwitchTo(old);
		ExecClearTuple(so->slot);
		return false;
	}
	*dist = DatumGetFloat8(FunctionCall2Coll(&ob->sk_func, ob->sk_collation,
											 values[0], so->query));
	MemoryContextSwitchTo(old);
	ExecClearTuple(so->slot);
	return true;
}

static void
lv_rerank(IndexScanDesc scan, LvScanOpaque *so, LvItem *item)
{
	double		dist;

	if (!lv_fetch_distance(scan, so, &item->tid, &dist))
	{
		lv_mark_done(so, &item->tid);
		return;
	}
	item->level = LV_EXACT;
	item->dist = dist;
	{
		double		bound = item->key;

		item->key = lionvec_key_exact(so->metric, dist, so->q.offset);
		if (isnan(item->key))
			item->key = INFINITY;
		if (item->key < bound)
			so->nmissed++;
	}
	lv_remember(so, &item->tid, LV_EXACT, item->key, item->key, dist);
	lv_pq_push(so, item);
}

/* ----------------------------------------------------------------
 * Walks of the chain: zero vectors, NULLs, everything
 * ----------------------------------------------------------------
 */

static bool
lv_walk_wants(LvWalk walk, uint16 flags)
{
	switch (walk)
	{
		case LV_WALK_ZERO:
			return (flags & LIONVEC_ZERO) != 0;
		case LV_WALK_NONNULL:
			return (flags & LIONVEC_NULL) == 0;
		case LV_WALK_NULL:
			return (flags & LIONVEC_NULL) != 0;
		case LV_WALK_ALL:
			return true;
	}
	return false;
}

/*
 * The next row of the walk.  A page's rows are copied out under its lock and
 * returned from the copy, so that a VACUUM between two calls, which moves the
 * items of a page it changes, cannot make one be returned twice.
 */
static bool
lv_walk_next(IndexScanDesc scan, LvScanOpaque *so, ItemPointer tid)
{
	Relation	index = scan->indexRelation;

	while (so->wpos >= so->wn)
	{
		Buffer		buf;
		Page		page;
		OffsetNumber off;
		OffsetNumber maxoff;
		LionVecPageOpaque op;
		bool		listed = so->wpages.listed;

		if (!BlockNumberIsValid(so->wblk))
			return false;
		CHECK_FOR_INTERRUPTS();
		lionvec_chain_step(index, &so->wvisited, &so->wnblocks);
		buf = ReadBuffer(index, so->wblk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		lionvec_check_page(index, page, so->wblk, LIONVEC_PAGE_CODE);
		op = LionVecPageGetOpaque(page);
		so->wn = so->wpos = 0;
		if (!so->restricted || lv_page_has_member(so, op->minkey, op->maxkey))
		{
			maxoff = PageGetMaxOffsetNumber(page);
			for (off = FirstOffsetNumber; off <= maxoff; off = OffsetNumberNext(off))
			{
				LionVecCode *code = lionvec_code_at(index, page, so->wblk, off,
													so->info->dimsp);

				if (listed)
					lv_check_listed(scan, so, so->wblk, &code->tid);
				if (!lv_walk_wants(so->walk, code->flags))
					continue;
				if (so->restricted && !lv_member(so, &code->tid))
					continue;
				Assert(so->wn < MaxOffsetNumber);
				so->wtids[so->wn++] = code->tid;
			}
		}
		so->wblk = lv_pages_next(so, &so->wpages, op->next);
		UnlockReleaseBuffer(buf);
	}
	*tid = so->wtids[so->wpos++];
	return true;
}

/* ----------------------------------------------------------------
 * amgettuple
 * ----------------------------------------------------------------
 */

bool
lionvecgettuple(IndexScanDesc scan, ScanDirection dir)
{
	LvScanOpaque *so = (LvScanOpaque *) scan->opaque;

	if (dir != ForwardScanDirection)
		elog(ERROR, "lionvec only scans forward");
	if (so->phase == LV_START)
		lv_start(scan, so);

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();
		switch (so->phase)
		{
			case LV_START:
				Assert(false);
				so->phase = LV_DONE;
				break;

			case LV_NORMAL:
				{
					MemoryContext old;
					LvItem		item;

					if (so->npq == 0)
					{
						lv_next_phase(so);
						break;
					}
					old = MemoryContextSwitchTo(so->qcxt);
					item = lv_pq_pop(so);
					switch (item.level)
					{
						case LV_REST:
							lv_pass(scan, so);
							break;
						case LV_PLANE1:
							lv_refine(scan, so, &item);
							break;
						case LV_CODES8:
							lv_rerank(scan, so, &item);
							break;
						case LV_EXACT:
							MemoryContextSwitchTo(old);
							lv_mark_done(so, &item.tid);
							if (item.dist < so->lastdist)
							{
								/* found too late: see the file's header */
								so->nskipped++;
								continue;
							}
							so->lastdist = item.dist;
							so->nreturned++;
							scan->xs_heaptid = item.tid;
							scan->xs_recheck = false;
							scan->xs_recheckorderby = false;
							scan->xs_orderbyvals[0] = Float8GetDatum(item.dist);
							scan->xs_orderbynulls[0] = false;
							return true;
					}
					MemoryContextSwitchTo(old);
				}
				break;

			case LV_WALK:
				{
					ItemPointerData tid;

					if (!lv_walk_next(scan, so, &tid))
					{
						lv_next_phase(so);
						break;
					}
					scan->xs_heaptid = tid;
					scan->xs_recheck = false;
					scan->xs_recheckorderby = false;
					if (scan->numberOfOrderBys > 0)
					{
						bool		isnan_dist = so->walk == LV_WALK_ZERO ||
							so->walk == LV_WALK_NONNULL;

						scan->xs_orderbyvals[0] = isnan_dist ?
							Float8GetDatum(get_float8_nan()) : (Datum) 0;
						scan->xs_orderbynulls[0] = !isnan_dist;
					}
					so->nreturned++;
					return true;
				}

			case LV_DONE:
				return false;
		}
	}
}
