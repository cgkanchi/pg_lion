/*-------------------------------------------------------------------------
 *
 * lion_vec_scan.c
 *		The lionvec scan: rows in order of their distance to a query
 *		(DESIGN.md §44.4).
 *
 * Two priority queues.  One holds the partitions, by their centroids' keys
 * less the margin (lv_margin()): how much nearer than its centroid a
 * partition's rows are taken to come.  The other holds rows, by a lower
 * bound on their keys (lion_vec_math.h).  Whichever queue's least entry is
 * less is worked, the row's at equal keys:
 *
 *	a partition known from its centroid's plane 1	- read the centroid's
 *		REFINE item, and put it back by its 8-bit codes
 *	... known from its centroid's codes				- probe it: read its CODE
 *		chain, and put its rows in the row queue by plane 1's bounds
 *	a row known from plane 1						- read its REFINE item, and
 *		put it back with the tighter lower bound of its 8-bit codes
 *	... a row known from its codes					- fetch it from the heap,
 *		compute its distance exactly with the operator's own function, and
 *		put it back with that
 *	... a row whose distance is exact				- no row left in the queue,
 *		nor in a partition not yet probed, is taken to be nearer: return it
 *	... REST										- probe every partition
 *		probed so far again, for four times as many rows
 *
 * A probe keeps, of the rows it reads, those whose lower bound is no more
 * than tau, the L-th least upper bound of the rows probed so far: at least
 * L rows lie within tau, so no row beyond it can be among the next L.  REST
 * stands for the rows left out, at the least of their lower bounds.
 *
 * So a LIMIT k query probes the partitions whose centroids, less the
 * margin, lie nearer than its k-th row, refines about the rows plane 1
 * cannot tell from the k-th, and computes the distance of about k + a few.
 * A row whose partition the margin leaves unprobed is missed.
 *
 * A query whose rows lie far from every centroid finds many centroids
 * within its k-th row less the margin.  So the scan also pauses.  It keeps,
 * besides the L least upper bounds, the K least for K = LV_TIER_MIN, twice
 * that and so on below L - the tiers - and the next row's tier is the least
 * K past the rows the pass has returned, or L.  Once that tier is full and
 * the scan has scored pg_lion.vector_patience units of rows
 * (lv_patience_unit(): the rows, or under a restriction twice the members,
 * a partition would hold on average in 2 sqrt(n) partitions; more past
 * twice the default margin, lv_patience_scale()) without one entering it,
 * it takes those K as found and probes no partition until it has returned
 * them or the next row lies past the K-th upper bound; a REST, which starts
 * a new batch, ends a pause too.  Then the next tier's count, kept all
 * along, decides the next pause.  A row nearer than those a pause returned,
 * in a partition not probed yet, is skipped when it is found, as is one the
 * margin leaves till too late.
 * Ended only past tau, an upper bound, a pause returned rows far beyond
 * the L nearest: 0.955 of a LIMIT 1000's true rows, against 0.983 so.
 * Counted against L alone, a LIMIT 10 went on probing while rows entered
 * the 100 nearest, which it would never return: on DBpedia's 115,186 rows
 * a margin of 8 probed 200 partitions a query at the median and 401 at the
 * 95th percentile for 0.986 of the ten nearest; by tiers, 159 and 254 for
 * 0.985 (DESIGN.md §44.4).
 *
 * The bounds hold with the probability pg_lion.vector_epsilon sets; a row
 * whose bound fails and turns up after rows farther than it is skipped, so
 * the scan never returns rows out of order.
 *
 * In an index that spills, every row with a vector has two items, in two
 * partitions' chains (DESIGN.md §44.3).  The first a pass meets enters it -
 * the row queue, and the L least upper bounds - and the progress hash
 * records the pass; the other, met later in the same pass, is skipped
 * (counted as a copy skipped).  A REST starts a new pass, which enters each
 * row once again.  A walk returns no copy.
 *
 * NULL rows come last, with NULL distances; before them, in a cosine index,
 * the zero vectors, whose distance is NaN.  A restricted scan (LionOrdered,
 * §44.6) of no more than pg_lion.vector_exact_rows members computes every member's
 * distance from the heap and returns them in order (lv_exact()); of more,
 * it does all of the above over the members alone.
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
#include "utils/builtins.h"
#include "utils/float.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "lion_compat.h"
#include "lion_vec.h"

/* what an entry of the row queue knows of its row */
#define LV_REST		0			/* not a row: every row the probes left out */
#define LV_PLANE1	1
#define LV_CODES8	2
#define LV_EXACT	3

/* ... and an entry of the partition queue of its centroid */
#define LV_CENT1	1
#define LV_CENT8	2

/*
 * An entry of either queue.  A partition's carries its centroid's TID
 * (LionVecCentTid()), REFINE item and numbers.
 */
typedef struct LvItem
{
	double		key;			/* a lower bound on the key, or the key */
	double		dist;			/* LV_EXACT: the distance */
	double		hi1;			/* a row's: plane 1's upper bound on its key */
	ItemPointerData tid;
	uint8		level;
	OffsetNumber roff;
	BlockNumber rblk;
	float4		norm;
	float4		xm;
} LvItem;

/* a binary min-heap of them */
typedef struct LvHeap
{
	LvItem	   *items;
	int			n;
	int			max;
} LvHeap;

/*
 * What the scan has learnt of a row, kept across REST: probing again
 * starts a refined row from its codes' bounds and a fetched one from its
 * key, not from plane 1 again.  Without that, a pass cut short by work_mem
 * (lv_cut()) whose rows all refine past the cut would be made again, the
 * same, for ever: the rows would come back from plane 1 below the cut, be
 * refined past it, and the cut's REST come out first.  With it, every pass
 * moves at least one row up a level, or out.  In an index that spills, it
 * also has every row a pass entered, at LV_PLANE1 if no more is known, and
 * the pass that last did, so that the row's other item is skipped in it.
 */
#define LV_GONE		0			/* returned already, or not there */

typedef struct LvSeen
{
	uint64		key;
	char		status;			/* simplehash's */
	uint8		level;			/* LV_PLANE1, LV_CODES8, LV_EXACT, or LV_GONE */
	uint32		pass;			/* the pass that last entered it (spilling) */
	double		lo;				/* the key's bounds; LV_EXACT: the key */
	double		hi;
	double		dist;			/* LV_EXACT: the distance */
	double		hi1;			/* plane 1's upper bound (lv_seen_evict()) */
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

/* a member of a small restriction, with its distance (lv_exact()) */
typedef struct LvExact
{
	double		dist;
	ItemPointerData tid;
} LvExact;

typedef enum LvPhase
{
	LV_START,
	LV_NORMAL,					/* the queues */
	LV_LIST,					/* a small restriction's members, in order */
	LV_WALK,					/* chains, by flags */
	LV_DONE
} LvPhase;

/* which rows a walk returns, and with what distance */
typedef enum LvWalk
{
	LV_WALK_ZERO,				/* cosine zero vectors: NaN */
	LV_WALK_NONNULL,			/* every row not NULL: NaN (a zero query) */
	LV_WALK_NULL,				/* NULL rows: NULL */
	LV_WALK_ALL					/* every row: NULL (a NULL query, or none) */
} LvWalk;

/*
 * A tier of the pause (the file's header): the K least upper bounds of the
 * rows a pass has probed, K = LV_TIER_MIN, twice that and so on below L,
 * and when the last row entered them.  L's own are hi and hiat.  The least
 * K is a LIMIT 10's, the commonest; a smaller LIMIT pauses where it does.
 */
#define LV_TIER_MIN		10
#define LV_MAX_TIERS	32				/* K doubles from 10 past any int L */

typedef struct LvTier
{
	int			k;
	int			n;
	double	   *h;				/* max-heap, [k] */
	double		at;				/* scored when one last entered */
} LvTier;

typedef struct LvScanOpaque
{
	MemoryContext cxt;			/* the scan's */
	MemoryContext qcxt;			/* a rescan's, reset by the next */
	LvPhase		phase;
	LionVecMetaData meta;		/* as the scan started */
	bool		spill;			/* LIONVEC_META_SPILL: rows have copies */
	BlockNumber nblocks;		/* the index's, for lionvec_chain_step() */

	/* the query */
	LionVecInfo *info;
	int			metric;
	bool		dist4;			/* the operator returns float4, not float8 */
	Datum		query;			/* detoasted, in qcxt */
	LionVecQuery q;
	double		qnorm;			/* |q| */
	double		margin;			/* lv_margin() */

	/* the restriction (lion_vec_restrict()) */
	bool		restricted;
	int			rn;
	const uint32 *rkeys;
	LionContainer *const *rconts;
	int			rpos;			/* the last key found */

	/* the queues, and the probes' bookkeeping */
	LvHeap		rows;
	LvHeap		parts;
	BlockNumber *phead;			/* [nparts]: each partition's CODE chain */
	double	   *plift;			/* IP: [nparts][2], each one's maxnorm^2 and
								 * spread (lv_part_key()) */
	BlockNumber *probed;		/* the chains probed, in order */
	int			nprobed;
	double	   *hi;				/* max-heap of the L least upper bounds */
	int			nhi;
	int			maxhi;
	int			batch;			/* L */
	double		tau;
	double		cut;			/* lv_cut()'s, or infinity */
	double		patience;		/* rows scored without one entering a tier */
	double		scored;			/* rows the pass has scored ... */
	double		hiat;			/* ... when one last entered hi */
	LvTier		tiers[LV_MAX_TIERS];	/* the smaller tiers (the header) */
	int			ntiers;			/* this batch's, below L */
	int			ntieralloc;		/* tiers with heaps */
	int			passret;		/* rows the pass has returned */
	bool		paused;			/* probing no partition (the header) */
	int			pausetier;		/* the tier that paused, -1 for L's */
	int			pauseleft;		/* rows the pause may still return */
	uint32		pass;			/* from 1; a REST starts the next */
	lvseen_hash *seen;
	double		seenmost;		/* its entries' bound (lv_seen_most()) */
	bool		full;			/* seen could not take a row returned */
	uint32		nplane1;		/* its LV_PLANE1 entries: the queue's rows */
	int64		ngone;			/* its LV_GONE entries */
	int64		ndone;			/* rows it has marked LV_GONE, ever */
	int64		passdone;		/* ... as this pass started */
	int64		nadded;			/* entries it has taken, besides LV_PLANE1 ones */
	int64		walkadded;		/* ... as lv_seen_evict() last walked it */
	bool		behind;			/* it has dropped rows behind lastkey */
	bool		evicted;		/* this pass has lost knowledge (lv_remember()) */
	int			lossy;			/* passes that did, since a row was done */
	int64		nwalks;			/* lv_seen_evict()'s walks of it ... */
	int64		nevictions;		/* ... and those that lost knowledge */
	bool		twice;			/* the queue may hold a row twice (spilling) */
	double		lastdist;		/* of the row returned last */
	double		lastkey;		/* ... and its key */

	/* a small restriction's members (lv_exact()) */
	LvExact    *exact;
	int			nexact;
	int			epos;

	/* the walks */
	LvWalk		walk;
	BlockNumber *wheads;		/* the chains it walks */
	int			nwheads;
	int			wchain;			/* the next of them */
	BlockNumber wblk;
	uint64		wvisited;		/* lionvec_chain_step()'s */
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
	int64		nprobes;
	int64		npasses;
	int64		ncands;
	int64		nrefined;
	int64		nfetched;
	int64		nreturned;
	int64		nskipped;
	int64		nmissed;		/* exact keys below the bound they had */
	int64		npages;			/* CODE pages the probes read */
	int64		npauses;
	int64		ncopies;		/* items skipped, their row entered this pass */
} LvScanOpaque;

/* ----------------------------------------------------------------
 * The queues: binary min-heaps of LvItems
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
lv_heapify(LvHeap *h)
{
	int			i;

	for (i = h->n / 2 - 1; i >= 0; i--)
		lv_sift_down(h->items, h->n, i);
}

static void
lv_heap_grow(LvHeap *h, int need)
{
	if (need <= h->max)
		return;
	if ((Size) need > MaxAllocSize / sizeof(LvItem) / 2)
		elog(ERROR, "lionvec: too many candidates");
	h->max = Max(need, Max(h->max * 2, 64));
	h->items = h->items == NULL ?
		palloc(sizeof(LvItem) * h->max) :
		repalloc(h->items, sizeof(LvItem) * h->max);
}

static void
lv_heap_push(LvHeap *h, const LvItem *item)
{
	int			i;

	lv_heap_grow(h, h->n + 1);
	i = h->n++;
	while (i > 0)
	{
		int			p = (i - 1) / 2;

		if (!lv_before(item, &h->items[p]))
			break;
		h->items[i] = h->items[p];
		i = p;
	}
	h->items[i] = *item;
}

static LvItem
lv_heap_pop(LvHeap *h)
{
	LvItem		top = h->items[0];

	Assert(h->n > 0);
	h->n--;
	if (h->n > 0)
	{
		h->items[0] = h->items[h->n];
		lv_sift_down(h->items, h->n, 0);
	}
	return top;
}

static void
lv_push_rest(LvScanOpaque *so, double key)
{
	LvItem		rest;

	memset(&rest, 0, sizeof(rest));
	rest.key = key;
	rest.level = LV_REST;
	lv_heap_push(&so->rows, &rest);
}

/* ----------------------------------------------------------------
 * The max-heap of upper bounds the probes keep
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
 * How many rows the restriction admits: its containers' members, a NULL
 * container counting as its blocks' share of the table's rows, perblock a
 * block.  *whole says whether any container was NULL.
 */
static double
lv_members(const LvScanOpaque *so, double perblock, bool *whole)
{
	double		n = 0.0;
	int			k;

	*whole = false;
	for (k = 0; k < so->rn; k++)
	{
		if (so->rconts[k] == NULL)
		{
			*whole = true;
			n += perblock * LION_BLOCKS_PER_CONTAINER;
		}
		else
			n += lion_container_cardinality(so->rconts[k]);
	}
	return n;
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
		elog(DEBUG1, "lionvec scan of \"%s\": " INT64_FORMAT " partitions probed, "
			 INT64_FORMAT " passes, " INT64_FORMAT " candidates, "
			 INT64_FORMAT " refined, " INT64_FORMAT " fetched, "
			 INT64_FORMAT " returned, " INT64_FORMAT " skipped out of order, "
			 INT64_FORMAT " bounds missed, " INT64_FORMAT " pages read, "
			 INT64_FORMAT " pauses, " INT64_FORMAT " copies skipped",
			 RelationGetRelationName(scan->indexRelation), so->nprobes,
			 so->npasses, so->ncands, so->nrefined, so->nfetched,
			 so->nreturned, so->nskipped, so->nmissed, so->npages,
			 so->npauses, so->ncopies);
	if (so->nwalks > 0)
		elog(DEBUG1, "lionvec scan of \"%s\": its progress hash full "
			 INT64_FORMAT " times, " INT64_FORMAT " of them losing knowledge",
			 RelationGetRelationName(scan->indexRelation), so->nwalks,
			 so->nevictions);
	else if (so->nfetched > 0)
		elog(DEBUG1, "lionvec scan of \"%s\": " INT64_FORMAT " members fetched, "
			 INT64_FORMAT " returned",
			 RelationGetRelationName(scan->indexRelation), so->nfetched,
			 so->nreturned);
	so->nprobes = so->npasses = so->ncands = so->nrefined = so->nfetched = 0;
	so->nreturned = so->nskipped = so->nmissed = so->npages = 0;
	so->npauses = so->ncopies = 0;
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
	memset(&so->rows, 0, sizeof(LvHeap));
	memset(&so->parts, 0, sizeof(LvHeap));
	so->phead = NULL;
	so->plift = NULL;
	so->probed = NULL;
	so->nprobed = 0;
	so->hi = NULL;
	so->nhi = so->maxhi = 0;
	so->scored = so->hiat = 0.0;
	so->ntiers = so->ntieralloc = 0;
	so->passret = 0;
	so->paused = false;
	so->pausetier = -1;
	so->pauseleft = 0;
	so->seen = NULL;
	so->seenmost = 0.0;
	so->full = false;
	so->nplane1 = 0;
	so->ngone = so->ndone = so->passdone = 0;
	so->nadded = so->walkadded = 0;
	so->behind = false;
	so->evicted = false;
	so->lossy = 0;
	so->nwalks = so->nevictions = 0;
	so->twice = false;
	so->exact = NULL;
	so->nexact = so->epos = 0;
	so->wheads = NULL;
	so->nwheads = so->wchain = 0;
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
 * The exact distance
 * ----------------------------------------------------------------
 */

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
	Datum		d;
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
	d = FunctionCall2Coll(&ob->sk_func, ob->sk_collation, values[0], so->query);
	*dist = so->dist4 ? (double) DatumGetFloat4(d) : DatumGetFloat8(d);
	MemoryContextSwitchTo(old);
	ExecClearTuple(so->slot);
	return true;
}

/* ----------------------------------------------------------------
 * A small restriction: every member's distance (DESIGN.md §44.6)
 * ----------------------------------------------------------------
 */

/* NaN last, as the sort it stands in for puts it; then TID, for ties */
static int
lv_exact_cmp(const void *a, const void *b)
{
	const LvExact *x = (const LvExact *) a;
	const LvExact *y = (const LvExact *) b;

	if (isnan(x->dist) != isnan(y->dist))
		return isnan(x->dist) ? 1 : -1;
	if (x->dist != y->dist && !isnan(x->dist))
		return x->dist < y->dist ? -1 : 1;
	return ItemPointerCompare((ItemPointer) &x->tid, (ItemPointer) &y->tid);
}

/*
 * The members' distances from the heap, sorted: what probing would cost,
 * and more, when a restriction admits so few rows that most partitions
 * hold none, and the nearest member lies past many.  A member that is not
 * visible, or whose vector is NULL, is left for the NULL walk; a cosine
 * zero vector, whose distance is NaN, for the zero walk.
 */
static void
lv_exact(IndexScanDesc scan, LvScanOpaque *so)
{
	uint16	   *los = palloc(sizeof(uint16) * LION_CONTAINER_RANGE);
	int			max = 64;
	int			k;

	so->exact = palloc(sizeof(LvExact) * max);
	so->nexact = 0;
	for (k = 0; k < so->rn; k++)
	{
		uint32		m;
		uint32		j;

		Assert(so->rconts[k] != NULL);
		m = lion_container_to_array(so->rconts[k], los);
		for (j = 0; j < m; j++)
		{
			uint64		code = lion_make_code(so->rkeys[k], los[j]);
			ItemPointerData tid;
			double		dist;

			CHECK_FOR_INTERRUPTS();
			if ((code >> LION_OFFSET_BITS) > MaxBlockNumber)
				break;
			lion_code_to_tid(code, &tid);
			if (!OffsetNumberIsValid(ItemPointerGetOffsetNumberNoCheck(&tid)))
				continue;
			if (!lv_fetch_distance(scan, so, &tid, &dist))
				continue;
			if (isnan(dist) && so->metric == LIONVEC_COSINE)
				continue;
			if (so->nexact >= max)
			{
				max *= 2;
				so->exact = repalloc(so->exact, sizeof(LvExact) * max);
			}
			so->exact[so->nexact].dist = dist;
			so->exact[so->nexact].tid = tid;
			so->nexact++;
		}
	}
	pfree(los);
	qsort(so->exact, so->nexact, sizeof(LvExact), lv_exact_cmp);
	so->epos = 0;
	so->phase = LV_LIST;
}

/* ----------------------------------------------------------------
 * Walks of the chains: zero vectors, NULLs, everything
 * ----------------------------------------------------------------
 */

static void
lv_start_walk(IndexScanDesc scan, LvScanOpaque *so, LvWalk walk)
{
	MemoryContext old = MemoryContextSwitchTo(so->qcxt);

	so->phase = LV_WALK;
	so->walk = walk;
	if (walk == LV_WALK_ZERO || walk == LV_WALK_NULL)
	{
		/* both live in the chain of NULL rows */
		so->wheads = palloc(sizeof(BlockNumber));
		so->wheads[0] = so->meta.null_head;
		so->nwheads = 1;
	}
	else
		so->wheads = lionvec_chain_heads(scan->indexRelation, &so->meta,
										 &so->nwheads);
	so->wchain = 0;
	so->wblk = InvalidBlockNumber;
	so->wvisited = 0;
	so->wn = so->wpos = 0;
	MemoryContextSwitchTo(old);
}

/* What follows the queue, or a walk: the zero vectors, the NULLs, the end. */
static void
lv_next_phase(IndexScanDesc scan, LvScanOpaque *so)
{
	bool		ordered = so->phase == LV_NORMAL || so->phase == LV_LIST;

	if (ordered && so->metric == LIONVEC_COSINE)
		lv_start_walk(scan, so, LV_WALK_ZERO);
	else if (ordered ||
			 (so->phase == LV_WALK &&
			  (so->walk == LV_WALK_ZERO || so->walk == LV_WALK_NONNULL)))
		lv_start_walk(scan, so, LV_WALK_NULL);
	else
		so->phase = LV_DONE;
}

static bool
lv_walk_wants(LvWalk walk, uint16 flags)
{
	/* a row's copy: the row comes from its own item */
	if (flags & LIONVEC_SPILL)
		return false;
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
 * items of a page it changes, cannot make one be returned twice.  The chains
 * share no page, so all of them together are no longer than the index.
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

		if (!BlockNumberIsValid(so->wblk))
		{
			if (so->wchain >= so->nwheads)
				return false;
			so->wblk = so->wheads[so->wchain++];
			continue;
		}
		CHECK_FOR_INTERRUPTS();
		lionvec_chain_step(index, &so->wvisited, &so->nblocks);
		buf = ReadBuffer(index, so->wblk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		lionvec_check_page(index, page, so->wblk, LIONVEC_PAGE_CODE);
		op = LionVecPageGetOpaque(page);
		so->wn = so->wpos = 0;
		if (!so->restricted || lv_page_has_member(so, op->minkey, op->maxkey))
		{
			/* copies are in partitions' chains, which lionvec_chain_heads() gives first */
			bool		copies = so->spill &&
				(so->walk == LV_WALK_ALL || so->walk == LV_WALK_NONNULL) &&
				so->wchain - 1 < (int) so->meta.nparts;

			maxoff = PageGetMaxOffsetNumber(page);
			for (off = FirstOffsetNumber; off <= maxoff; off = OffsetNumberNext(off))
			{
				LionVecCode *code = lionvec_code_at(index, page, so->wblk, off,
													so->meta.dimsp, copies);

				if (!lv_walk_wants(so->walk, code->flags))
					continue;
				if (so->restricted && !lv_member(so, &code->tid))
					continue;
				Assert(so->wn < MaxOffsetNumber);
				so->wtids[so->wn++] = code->tid;
			}
		}
		so->wblk = op->next;
		UnlockReleaseBuffer(buf);
	}
	*tid = so->wtids[so->wpos++];
	return true;
}

/* ----------------------------------------------------------------
 * The partitions
 * ----------------------------------------------------------------
 */

/*
 * How much nearer than its centroid a partition's rows are taken to come,
 * in the units of distance: pg_lion.vector_margin standard deviations of
 * the rows about their centroids along one direction, as the build
 * measured them (DESIGN.md §44.4).  A row nearer the query than its
 * centroid is so by the part of its offset from the centroid that points
 * at the query, whose spread that is; it shrinks with the dimensions, as a
 * partition's rows lie ever more nearly at one distance from the query.
 * A restriction does not change it: its nearest members lie farther out,
 * past more centroids within the margin, and the pause, counted in
 * members, ends those probes instead.  Narrowed by 1.8 for each tenfold
 * the members thinned the rows by, a 1% filter of 975,000 rows of 1536
 * dimensions read 220 partitions for 0.970 of the ten nearest; not
 * narrowed, 244 for 0.981 (DESIGN.md §44.6).
 */
static double
lv_margin(LvScanOpaque *so)
{
	return lionvec_margin * so->meta.scale;
}

/*
 * pg_lion.vector_patience's unit: the rows, or under a restriction the
 * members, of a scan of `members` that an average partition would hold
 * were there 2 sqrt(n) of them, n the build's rows - sqrt(n) / 2 rows
 * unfiltered - and at least one.  It was the average partition's own,
 * members / nparts, the same at the old default of 2 sqrt(n) partitions;
 * but then four times the partitions paused after a quarter of the rows,
 * and on DBpedia's 115,186 rows a patience of 30 in 2,715 partitions held
 * the recall to 0.966 at any margin, where in 678 it reached 0.988
 * (DESIGN.md §44.4).  Now the pause does not move with the partitions.
 * The unit is of items as the probes count them: in an index that spills
 * (§44.10), each row is in two partitions, so a partition holds twice the
 * items.  Under a restriction it is twice the members': with the tiers
 * (the file's header) a LIMIT 10 under a filter pauses at its ten, not at
 * L's 100, and members come a few to a partition, so the same patience
 * took DBpedia's 1% filter from 0.969 of the ten nearest to 0.939 at the
 * default margin; twice it, 0.961, with 368 partitions probed against 530.
 */
#define LV_PATIENCE_RESTRICTED	2.0

static double
lv_patience_unit(const LvScanOpaque *so, double members)
{
	double		parts = Max(2.0 * sqrt(Max(so->meta.ntuples, 0.0)), 1.0);

	if (so->spill)
		members *= 2.0;
	if (so->restricted)
		members *= LV_PATIENCE_RESTRICTED;
	return Max(members / parts, 1.0);
}

/*
 * The patience grows with pg_lion.vector_margin past LV_PATIENCE_MARGINS
 * times the default margin, in step with it: a larger margin asks for more
 * of the nearest rows, and a pause that stayed put held the recall to what
 * it had found by then - on DBpedia's 115,186 rows 0.9935 of the ten
 * nearest at any margin from 12 before the tiers, and 0.9865 with them,
 * where in step a margin of 24 finds 0.9975 (DESIGN.md §44.4).  Below that
 * margin the margin stops a scan first, and the patience its tail.
 */
#define LV_PATIENCE_MARGINS		2.0

static double
lv_patience_scale(void)
{
	return Max(lionvec_margin / (LV_PATIENCE_MARGINS * LIONVEC_MARGIN_DEFAULT), 1.0);
}

/*
 * A partition's place in its queue, from a lower bound lo on the key of its
 * centroid (partition partno's): the key of a row the margin nearer.  An L2
 * key is the squared distance less |qc|^2 (lion_vec_math.h), so the margin
 * comes off the distance.  An inner product's comes off the distance in
 * the space lionvec_lift_key() lifts the partition's rows into, where they
 * lie at one distance from the origin, so that a partition of unit vectors
 * is ranked as an L2 index ranks it.  Off <q, c> instead, it left the
 * answers of rows of mixed lengths - the rows that stick out farthest along
 * q - beyond the margin: an L2 or cosine row nearer the query than its
 * centroid pays |x - c|^2 for it, and an inner product's does not (found by
 * vector_filter.sql's recall test, 2026-10-14).
 */
static double
lv_part_key(const LvScanOpaque *so, double lo, BlockNumber partno)
{
	if (isinf(lo))
		return lo;
	if (so->metric == LIONVEC_L2)
	{
		double		d = sqrt(Max(lo + so->q.offset, 0.0)) - so->margin;

		return (d > 0.0 ? d * d : 0.0) - so->q.offset;
	}
	if (so->plift != NULL)
		return lionvec_lift_key(lo, so->q.offset, so->qnorm, so->plift[2 * partno],
								so->plift[2 * partno + 1], so->margin);
	return lo - so->margin;
}

/*
 * Every partition into its queue, by its centroid's plane 1 (DESIGN.md
 * §44.4), and each one's CODE chain into phead.
 */
static void
lv_read_parts(IndexScanDesc scan, LvScanOpaque *so)
{
	Relation	index = scan->indexRelation;
	int			dimsp = so->meta.dimsp;
	int			k = (int) so->meta.nparts;
	BlockNumber blk = so->meta.part_head;
	uint64		visited = 0;
	int			n = 0;

	so->phead = palloc(sizeof(BlockNumber) * Max(k, 1));
	if (so->metric == LIONVEC_IP)
		so->plift = palloc(sizeof(double) * 2 * Max(k, 1));
	lv_heap_grow(&so->parts, Max(k, 1));
	while (BlockNumberIsValid(blk))
	{
		Buffer		buf;
		Page		page;
		OffsetNumber off;
		OffsetNumber maxoff;

		CHECK_FOR_INTERRUPTS();
		lionvec_chain_step(index, &visited, &so->nblocks);
		buf = ReadBuffer(index, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		lionvec_check_page(index, page, blk, LIONVEC_PAGE_PART);
		maxoff = PageGetMaxOffsetNumber(page);
		for (off = FirstOffsetNumber; off <= maxoff; off = OffsetNumberNext(off))
		{
			LionVecPart *part = lionvec_part_at(index, page, blk, off, dimsp);
			LvItem		item;
			double		est;
			double		err;
			double		lo;
			double		hi;

			if (n >= k || part->partno != (uint32) n)
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("lionvec index \"%s\" has partition %u where %d belongs, at (%u,%u)",
								RelationGetRelationName(index), part->partno, n, blk, off)));
			so->phead[n] = part->head;
			if (so->plift != NULL)
			{
				so->plift[2 * n] = (double) part->maxnorm * part->maxnorm;
				so->plift[2 * n + 1] = part->spread;
			}
			lionvec_estimate1(&so->q, part->cent.bits, part->cent.popcount,
							  part->cent.norm, part->cent.alpha1, &est, &err);
			lionvec_key_bounds(so->metric, part->cent.norm, part->cent.xm,
							   est, err, &lo, &hi);
			memset(&item, 0, sizeof(item));
			item.key = lv_part_key(so, lo, n);
			LionVecCentTid(&item.tid, n);
			item.level = LV_CENT1;
			item.rblk = part->cent.rblk;
			item.roff = part->cent.roff;
			item.norm = part->cent.norm;
			item.xm = part->cent.xm;
			so->parts.items[so->parts.n++] = item;
			n++;
		}
		blk = LionVecPageGetOpaque(page)->next;
		UnlockReleaseBuffer(buf);
	}
	if (n != k)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has %d partitions, and its meta page says %d",
						RelationGetRelationName(index), n, k)));
	lv_heapify(&so->parts);
}

/* A partition's centroid by its 8-bit codes, back into the queue. */
static void
lv_refine_part(IndexScanDesc scan, LvScanOpaque *so, LvItem *item)
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
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has lost partition %u's centroid",
						RelationGetRelationName(index),
						LionVecCentPartno(&item->tid))));
	lionvec_estimate8(&so->q, ref->codes, item->norm, ref->alpha8,
					  ref->rnorm8, &est, &err);
	UnlockReleaseBuffer(buf);
	lionvec_key_bounds(so->metric, item->norm, item->xm, est, err, &lo, &hi);
	item->key = lv_part_key(so, lo, LionVecCentPartno(&item->tid));
	item->level = LV_CENT8;
	lv_heap_push(&so->parts, item);
}

/* ----------------------------------------------------------------
 * Probes: a chain's rows into the row queue
 * ----------------------------------------------------------------
 */

static inline uint64
lv_tidkey(const ItemPointerData *tid)
{
	return ((uint64) ItemPointerGetBlockNumber(tid) << 16) |
		ItemPointerGetOffsetNumber(tid);
}

/* The rows work_mem holds in the queue, and never fewer than four batches. */
static int
lv_cap(const LvScanOpaque *so)
{
	int			cap;

	cap = (int) Min((double) work_mem * 1024.0 / sizeof(LvItem),
					(double) (MaxAllocSize / sizeof(LvItem) / 2));
	return Max(cap, 4 * so->batch + 64);
}

/*
 * The progress hash's memory (the LvSeen comment).  It holds an entry for
 * every row the scan has refined, fetched or returned, which no cap on the
 * queue bounds: a scan with no LIMIT holds one for every row of the table,
 * 48 bytes a row and simplehash's slack, gigabytes at a hundred million
 * rows.  So those entries get what core gives a hash table, hash_mem
 * (work_mem times hash_mem_multiplier): as many as the largest of
 * simplehash's power-of-two arrays within it holds before it grows.  The
 * entries of the queue's rows in an index that spills, LV_PLANE1
 * (lv_forget()), come with the queue's cap and are counted apart (nplane1).
 *
 * When it is full, lv_seen_evict() first drops the entries a probe no
 * longer needs: those of rows whose plane-1 upper bound (hi1) lies below
 * the key of the row returned last.  The scan's order is strict (the
 * file's header), so no such row can be returned again, and from then on
 * (so->behind) a probe drops a row whose upper bound lies below that key,
 * as the hash would have; so too a refine or a fetch, whose finding the
 * hash keeps.  Nothing is lost: the probe tells those rows by plane 1
 * alone, as it read them first.  (A row whose plane-1 bound fails so, which
 * the scan has not returned, is lost, as one the margin leaves till too
 * late is.)  What is left are the rows plane 1 cannot tell from the last
 * one, returned or behind it, and what the hash knows of rows ahead
 * (LV_CODES8 and LV_EXACT at or past the key).
 *
 * If that frees less than a quarter of the hash, what it knows of rows
 * ahead goes too, if that is a quarter.  Forgetting a row ahead only costs
 * a later pass work: it meets the row again and starts it from plane 1.  In
 * an index that spills, the entry also marked the row entered in its pass,
 * so the row's other item may now enter too and the queue hold the row
 * twice: whichever comes out second finds it returned (LV_GONE), or behind,
 * and is dropped.  A REST after passes that lost such knowledge and returned
 * no row would be the livelock the knowledge prevents, so the scan ends
 * there instead (lv_may_probe()).  A walk over the hash is allowed once a
 * quarter of what it may hold has come in since the last, which pays for
 * it.  When none is, or a walk frees no room, a row's codes or distance
 * are not kept; and a row returned must be, so the scan ends after it:
 * what it returned is in order, each row once, and the nearest it found.
 * A sort would spill to disk instead: a scan whose rows plane 1 cannot
 * tell apart outgrow hash_mem is one a sort serves better, and the ERROR,
 * or the NOTICE (pg_lion.vector_overflow = stop), says so.
 */
#define LV_SEEN_FILLFACTOR	0.9		/* simplehash's SH_FILLFACTOR */

/* The entries the hash may hold besides the queue's (nplane1). */
static double
lv_seen_most(void)
{
	double		bytes = (double) get_hash_memory_limit();
	double		size = 256.0;

	/* the largest power of two of entries within bytes, from simplehash's 256 */
	while (size * 2.0 * sizeof(LvSeen) <= bytes && size < (double) PG_UINT32_MAX)
		size *= 2.0;
	return floor(size * LV_SEEN_FILLFACTOR);
}

/* Whether the hash has room for one more entry not the queue's. */
static bool
lv_seen_room(const LvScanOpaque *so)
{
	return (double) (so->seen->members - so->nplane1) + 1.0 <= so->seenmost;
}

/* Whether a probe drops an entry's row by itself (above). */
static bool
lv_seen_behind(const LvScanOpaque *so, const LvSeen *s)
{
	return s->level != LV_PLANE1 && s->hi1 < so->lastkey;
}

/*
 * Whether an entry says a row is behind the row returned last, which plane
 * 1 alone does not (above): returned before it at a lesser distance, or
 * known to lie nearer.
 */
static bool
lv_seen_near(const LvScanOpaque *so, const LvSeen *s)
{
	if (s->level == LV_GONE)
		return s->dist < so->lastdist;
	return (s->level == LV_CODES8 || s->level == LV_EXACT) &&
		s->hi < so->lastkey;
}

/* Whether an entry is what the hash knows of a row ahead (above). */
static bool
lv_seen_ahead(const LvScanOpaque *so, const LvSeen *s)
{
	return (s->level == LV_CODES8 || s->level == LV_EXACT) &&
		!(s->hi < so->lastkey);
}

/*
 * Make room (above): drop the entries a probe no longer needs; if that
 * frees less than a quarter of the hash, those of the rows behind that
 * plane 1 cannot tell, and if that is still too little, what it knows of
 * rows ahead.  Whether there is room now.
 */
static bool
lv_seen_evict(LvScanOpaque *so)
{
	double		most = so->seenmost;
	double		before = (double) so->seen->members;
	double		near = 0.0;
	lvseen_iterator iter;
	LvSeen	   *s;
	bool		ahead;

	if ((double) (so->nadded - so->walkadded) < most / 4.0)
		return false;
	so->walkadded = so->nadded;
	so->nwalks++;
	lvseen_start_iterate(so->seen, &iter);
	while ((s = lvseen_iterate(so->seen, &iter)) != NULL)
	{
		if (lv_seen_behind(so, s))
		{
			if (s->level == LV_GONE)
				so->ngone--;
			/* deleting the entry just returned is allowed (simplehash.h) */
			lvseen_delete_item(so->seen, s);
			so->behind = true;
		}
		else if (lv_seen_near(so, s))
			near += 1.0;
	}
	if (before - (double) so->seen->members >= most / 4.0)
		return lv_seen_room(so);

	/* knowledge goes: a later pass works for it again */
	ahead = before - (double) so->seen->members + near < most / 4.0;
	so->nevictions++;
	so->evicted = true;
	so->behind = true;
	so->twice = true;
	lvseen_start_iterate(so->seen, &iter);
	while ((s = lvseen_iterate(so->seen, &iter)) != NULL)
	{
		if (lv_seen_near(so, s) || (ahead && lv_seen_ahead(so, s)))
		{
			if (s->level == LV_GONE)
				so->ngone--;
			lvseen_delete_item(so->seen, s);
		}
	}
	return lv_seen_room(so);
}

/*
 * Whether the scan may probe: not once the hash could not take a row
 * returned, nor - a REST - after LV_LOSSY_PASSES passes that lost knowledge
 * since the scan was last done with a row.  Such a pass is not made again
 * the same: the knowledge it kept and a batch that grows make the next one
 * differ, so a few are allowed; the bound is what ends a livelock.  Passes
 * that lose nothing in between do not reset it: they may only win back
 * what the last one lost, for the next to lose again (found by vector.sql's
 * scan at a work_mem of 64 kB, 65,000 passes for 20 rows).
 */
#define LV_LOSSY_PASSES		4

static bool
lv_may_probe(const LvScanOpaque *so, bool rest)
{
	if (so->full)
		return false;
	if (!rest)
		return true;
	return so->lossy + (so->evicted && so->ndone == so->passdone ? 1 : 0) <
		LV_LOSSY_PASSES;
}

/*
 * The scan cannot go on: the hash could not take a row returned, or a REST
 * (above).  pg_lion.vector_overflow says whether that fails the query or
 * ends the scan.
 */
static void
lv_seen_stop(IndexScanDesc scan, LvScanOpaque *so)
{
	if (lionvec_overflow == LIONVEC_OVERFLOW_STOP)
		ereport(NOTICE,
				(errmsg("lionvec scan of \"%s\" stopped after " INT64_FORMAT " rows, what it knew of its rows having outgrown hash_mem",
						RelationGetRelationName(scan->indexRelation), so->nreturned),
				 errhint("Raise work_mem or hash_mem_multiplier, or let the query sort its rows (enable_indexscan = off) or LIMIT them.")));
	else
		ereport(ERROR,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("lionvec scan of \"%s\" outgrew hash_mem after " INT64_FORMAT " rows",
						RelationGetRelationName(scan->indexRelation), so->nreturned),
				 errdetail("What it knew of the rows it could not yet tell apart no longer fit."),
				 errhint("Raise work_mem or hash_mem_multiplier, let the query sort its rows (enable_indexscan = off) or LIMIT them, "
						 "or set pg_lion.vector_overflow = stop to have the scan end at the rows it returned.")));
	so->phase = LV_DONE;
}

/*
 * A row of an index that spills enters this pass (lv_probe()): its other
 * item, met later in the pass, is skipped.
 */
static void
lv_enter(LvScanOpaque *so, ItemPointer tid)
{
	bool		found;
	LvSeen	   *s = lvseen_insert(so->seen, lv_tidkey(tid), &found);

	if (!found)
	{
		s->level = LV_PLANE1;
		s->lo = s->hi = s->dist = s->hi1 = 0.0;
		so->nplane1++;
	}
	s->pass = so->pass;
}

/*
 * A row that entered this pass leaves it unworked (lv_shrink(), lv_cut(),
 * lv_probe()'s second look at tau, a REST): forget it, if all the hash
 * knows of it is that it entered, so that the LV_PLANE1 entries are the
 * row queue's rows and stay within its cap.  Its other item, if met later
 * in the pass, is left out as this one was - tau and the cut only fall
 * within a pass - and a REST's pass enters it afresh.
 */
static void
lv_forget(LvScanOpaque *so, const LvItem *it)
{
	LvSeen	   *s;

	if (!so->spill || it->level != LV_PLANE1)
		return;
	s = lvseen_lookup(so->seen, lv_tidkey(&it->tid));
	if (s != NULL && s->level == LV_PLANE1)
	{
		lvseen_delete_item(so->seen, s);
		so->nplane1--;
	}
}

/*
 * Keep the queue under cap by cutting at a key: the cap / 2 with the least
 * lower bounds stay, the cut is the least bound of those that went, and the
 * probes take only rows below it; a REST at the cut then stands for
 * everything else.  Rows kept AT the cut stay - REST comes out after every
 * row at its key - which is what makes a scan of a million equal vectors
 * return cap / 2 of them, rather than none.  Returns the cut; the queue is
 * left in order, which is a heap.
 */
static double
lv_cut(LvScanOpaque *so, int cap)
{
	int			keep = cap / 2;
	int			n = so->rows.n;
	LvItem	   *sorted = palloc(sizeof(LvItem) * keep);
	double		cut;
	int			i;

	Assert(n > keep);
	lv_heapify(&so->rows);
	for (i = 0; i < keep; i++)
	{
		sorted[i] = so->rows.items[0];
		so->rows.items[0] = so->rows.items[--n];
		lv_sift_down(so->rows.items, n, 0);
	}
	cut = so->rows.items[0].key;
	for (i = 0; i < n; i++)
		lv_forget(so, &so->rows.items[i]);
	memcpy(so->rows.items, sorted, sizeof(LvItem) * keep);
	so->rows.n = keep;
	pfree(sorted);
	return cut;
}

/*
 * The queue is full: drop what tau has passed by since it went in, and cut
 * if that is not enough.  *drop takes the least bound of what went.
 */
static void
lv_shrink(LvScanOpaque *so, int cap, double *drop)
{
	int			n = 0;
	int			i;

	for (i = 0; i < so->rows.n; i++)
	{
		LvItem	   *it = &so->rows.items[i];

		if (it->key <= so->tau && it->key <= so->cut)
			so->rows.items[n++] = *it;
		else
		{
			*drop = Min(*drop, it->key);
			lv_forget(so, it);
		}
	}
	so->rows.n = n;
	if (n > cap / 2)
	{
		/* not inside Min(), which would cut a second time */
		double		cut = lv_cut(so, cap);

		so->cut = Min(so->cut, cut);
		*drop = Min(*drop, so->cut);
	}
	else
		lv_heapify(&so->rows);
}

/*
 * Probe the CODE chain from head: each row by plane 1's bounds, or by what
 * the scan has learnt of it, into the row queue if its lower bound is
 * within tau and below the cut, and the least bound of those left out to
 * a REST.  copies says whether the chain may hold rows' copies (a
 * partition's, of an index that spills); a row this pass has entered
 * already, from its other item, is skipped.
 */
static void
lv_probe(IndexScanDesc scan, LvScanOpaque *so, BlockNumber head, bool copies)
{
	Relation	index = scan->indexRelation;
	int			dimsp = so->info->dimsp;
	int			L = so->batch;
	int			cap = lv_cap(so);
	double		drop = INFINITY;

	/*
	 * Nothing to look up in an empty hash - a chain holds a TID once - but
	 * in an index that spills the probe's own entries count, and they catch
	 * a TID a corrupted chain holds twice, too
	 */
	bool		anyseen = so->spill || so->seen->members > 0;
	uint64		visited = 0;
	BlockNumber blk = head;

	while (BlockNumberIsValid(blk))
	{
		Buffer		buf;
		Page		page;
		LionVecPageOpaque op;
		OffsetNumber off;
		OffsetNumber maxoff;

		CHECK_FOR_INTERRUPTS();
		lionvec_chain_step(index, &visited, &so->nblocks);
		buf = ReadBuffer(index, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		lionvec_check_page(index, page, blk, LIONVEC_PAGE_CODE);
		op = LionVecPageGetOpaque(page);
		so->npages++;
		if (so->restricted && !lv_page_has_member(so, op->minkey, op->maxkey))
		{
			blk = op->next;
			UnlockReleaseBuffer(buf);
			continue;
		}

		maxoff = PageGetMaxOffsetNumber(page);
		for (off = FirstOffsetNumber; off <= maxoff; off = OffsetNumberNext(off))
		{
			LionVecCode *code = lionvec_code_at(index, page, blk, off, dimsp,
												copies);
			LvItem		item;
			double		lo;
			double		hi;

			if (code->flags & (LIONVEC_NULL | LIONVEC_ZERO))
				continue;
			if (so->restricted && !lv_member(so, &code->tid))
				continue;
			so->scored += 1.0;

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
				if (s != NULL && so->spill && s->pass == so->pass)
				{
					/* its other item entered it: the first met wins */
					so->ncopies++;
					continue;
				}
				if (s != NULL && s->level != LV_PLANE1)
				{
					item.level = s->level;
					item.dist = s->dist;
					item.hi1 = s->hi1;
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
				item.hi1 = hi;
			}

			/*
			 * Behind the row returned last (lv_seen_evict()): returned, or
			 * never to be.
			 */
			if (so->behind && hi < so->lastkey)
				continue;
			if (lo > so->tau || lo >= so->cut)
			{
				drop = Min(drop, lo);
				continue;
			}

			/*
			 * It enters the pass.  Its other item, if met later in it, has
			 * the same bounds, and tau and the cut only fall within a pass,
			 * so that item could do no more than this one does now.
			 */
			if (so->spill)
				lv_enter(so, &code->tid);
			if (so->nhi < L || hi < so->hi[0])
			{
				int			t;

				/*
				 * It enters: patience starts over, for L and for each tier
				 * it enters - a tier full and no nearer than it leaves the
				 * smaller ones, full too, no nearer either
				 */
				so->hiat = so->scored;
				for (t = so->ntiers - 1; t >= 0; t--)
				{
					LvTier	   *tier = &so->tiers[t];

					if (tier->n == tier->k && hi >= tier->h[0])
						break;
					tier->at = so->scored;
					lv_hi_push(tier->h, &tier->n, tier->k, hi);
				}
			}
			lv_hi_push(so->hi, &so->nhi, L, hi);
			if (so->nhi == L)
				so->tau = so->hi[0];
			if (lo > so->tau)
			{
				drop = Min(drop, lo);
				lv_forget(so, &item);
				continue;
			}
			item.key = lo;
			if (so->rows.n >= cap)
			{
				lv_shrink(so, cap, &drop);
				if (lo >= so->cut)
				{
					drop = Min(drop, lo);
					lv_forget(so, &item);
					continue;
				}
			}
			lv_heap_push(&so->rows, &item);
			so->ncands++;
		}
		blk = op->next;
		UnlockReleaseBuffer(buf);
		/* between two pages of a chain, holding nothing (§44.5) */
		if (BlockNumberIsValid(blk))
			LION_INJECTION_POINT("lionvec-probe-page");
	}

	/* every row left out lies past this */
	if (drop < INFINITY)
		lv_push_rest(so, drop);
}

/*
 * Is a pause due (the file's header)?  The tier of the next row - the
 * least K past the rows the pass has returned, or L - is full and has
 * taken no row in pg_lion.vector_patience units of rows.  Sets pausetier.
 */
static bool
lv_pause_due(LvScanOpaque *so)
{
	int			t;

	for (t = 0; t < so->ntiers; t++)
	{
		LvTier	   *tier = &so->tiers[t];

		if (tier->k > so->passret)
		{
			so->pausetier = t;
			return tier->n == tier->k &&
				so->scored - tier->at >= so->patience;
		}
	}
	so->pausetier = -1;
	return so->nhi == so->batch && so->scored - so->hiat >= so->patience;
}

/* The upper bound past which the pause's rows are all returned. */
static double
lv_pause_tau(const LvScanOpaque *so)
{
	return so->pausetier >= 0 ? so->tiers[so->pausetier].h[0] : so->tau;
}

/* Probe a partition from the queue, and remember it for a REST's. */
static void
lv_probe_part(IndexScanDesc scan, LvScanOpaque *so, LvItem *item)
{
	BlockNumber partno = LionVecCentPartno(&item->tid);

	Assert(partno < so->meta.nparts);
	so->probed[so->nprobed++] = so->phead[partno];
	so->nprobes++;
	lv_probe(scan, so, so->phead[partno], so->spill);
	if (lv_pause_due(so))
	{
		int			k = so->pausetier >= 0 ? so->tiers[so->pausetier].k : so->batch;

		so->paused = true;
		so->pauseleft = k > so->passret ? k - so->passret : k;
		so->npauses++;
	}
}

/* Empty the row queue, for probes of L rows. */
static void
lv_reset_rows(LvScanOpaque *so)
{
	int64		k;

	so->rows.n = 0;
	so->nhi = 0;
	so->tau = INFINITY;
	so->cut = INFINITY;
	so->scored = so->hiat = 0.0;
	so->passret = 0;
	if (so->maxhi < so->batch)
	{
		so->hi = so->hi == NULL ?
			MemoryContextAlloc(so->qcxt, sizeof(double) * so->batch) :
			repalloc(so->hi, sizeof(double) * so->batch);
		so->maxhi = so->batch;
	}

	/* the tiers below L; a tier's K is the same in every batch */
	so->ntiers = 0;
	for (k = LV_TIER_MIN; k < so->batch && so->ntiers < LV_MAX_TIERS; k *= 2)
	{
		LvTier	   *tier = &so->tiers[so->ntiers];

		if (so->ntiers >= so->ntieralloc)
		{
			tier->h = MemoryContextAlloc(so->qcxt, sizeof(double) * k);
			so->ntieralloc++;
		}
		tier->k = (int) k;
		tier->n = 0;
		tier->at = 0.0;
		so->ntiers++;
	}
}

/*
 * REST: the probes left rows out that may now be the nearest left.  Probe
 * every chain probed so far again, for four times as many rows, past those
 * already returned.
 */
static void
lv_rest(IndexScanDesc scan, LvScanOpaque *so)
{
	int			cap = lv_cap(so);
	int			i;

	so->npasses++;
	so->pass++;
	so->lossy = so->ndone == so->passdone ? so->lossy + (so->evicted ? 1 : 0) : 0;
	so->passdone = so->ndone;
	so->evicted = false;
	so->batch = (int) Min((double) so->batch * 4.0,
						  Max((double) so->batch, (double) cap / 8.0));
	for (i = 0; i < so->rows.n; i++)
		lv_forget(so, &so->rows.items[i]);
#ifdef USE_ASSERT_CHECKING
	{
		/* every row the pass entered was worked, or left and was forgotten */
		lvseen_iterator iter;
		LvSeen	   *s;

		lvseen_start_iterate(so->seen, &iter);
		while ((s = lvseen_iterate(so->seen, &iter)) != NULL)
			Assert(s->level != LV_PLANE1);
	}
#endif
	lv_reset_rows(so);
	so->paused = false;
	/* the first chain probed is the REST chain (lv_start()), the rest partitions */
	for (i = 0; i < so->nprobed; i++)
		lv_probe(scan, so, so->probed[i], so->spill && i > 0);
}

/* ----------------------------------------------------------------
 * Refining a row, and its exact distance
 * ----------------------------------------------------------------
 */

static void
lv_remember(LvScanOpaque *so, ItemPointer tid, uint8 level, double lo,
			double hi, double dist, double hi1)
{
	bool		found;
	LvSeen	   *s;

	/*
	 * A row a probe entered has its entry already (an index that spills),
	 * at LV_PLANE1; any other row's, or a row's that moves up from there,
	 * needs room.  Without it, and none to be made, a row's codes or
	 * distance are not kept (nor is its LV_PLANE1 entry, which would no
	 * longer be the queue's); a row returned must be, so the scan ends
	 * (so->full).
	 */
	if (so->full)
		return;
	if (lv_seen_room(so))
		s = lvseen_insert(so->seen, lv_tidkey(tid), &found);
	else
	{
		s = lvseen_lookup(so->seen, lv_tidkey(tid));
		if ((s == NULL || s->level == LV_PLANE1) && !lv_seen_evict(so))
		{
			so->evicted = true;
			if (level == LV_GONE)
				so->full = true;
			else if (s != NULL)
			{
				lvseen_delete_item(so->seen, s);
				so->nplane1--;
				so->twice = true;	/* the row's other item may enter */
			}
			return;
		}
		/* a walk moves entries */
		s = lvseen_insert(so->seen, lv_tidkey(tid), &found);
	}
	if (found && s->level == LV_PLANE1)
		so->nplane1--;
	if (!found || s->level == LV_PLANE1)
		so->nadded++;
	if (!found)
		s->pass = so->pass;
	if (level == LV_GONE && (!found || s->level != LV_GONE))
	{
		so->ngone++;
		so->ndone++;
	}
	s->level = level;
	s->lo = lo;
	s->hi = hi;
	s->dist = dist;
	s->hi1 = hi1;
}

static bool
lv_seen_gone(LvScanOpaque *so, ItemPointer tid)
{
	LvSeen	   *s = lvseen_lookup(so->seen, lv_tidkey(tid));

	return s != NULL && s->level == LV_GONE;
}

/* A row is returned, or found gone: the scan is done with it. */
static void
lv_mark_done(LvScanOpaque *so, LvItem *item)
{
	lv_remember(so, &item->tid, LV_GONE, item->key, item->key, item->dist,
				item->hi1);
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
		/* VACUUM has removed the row since the probe read it (§44.5) */
		UnlockReleaseBuffer(buf);
		lv_mark_done(so, item);
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
	lv_remember(so, &item->tid, LV_CODES8, lo, hi, 0.0, item->hi1);
	if (so->behind && hi < so->lastkey)
		return;					/* behind the row returned last; the hash says so */
	lv_heap_push(&so->rows, item);
}

static void
lv_rerank(IndexScanDesc scan, LvScanOpaque *so, LvItem *item)
{
	double		dist;

	if (!lv_fetch_distance(scan, so, &item->tid, &dist))
	{
		lv_mark_done(so, item);
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
	lv_remember(so, &item->tid, LV_EXACT, item->key, item->key, dist,
				item->hi1);
	if (so->behind && dist < so->lastdist)
		return;					/* behind the row returned last; the hash says so */
	lv_heap_push(&so->rows, item);
}

/* ----------------------------------------------------------------
 * Starting: the query
 * ----------------------------------------------------------------
 */

static void
lv_start(IndexScanDesc scan, LvScanOpaque *so)
{
	Relation	index = scan->indexRelation;
	MemoryContext old = MemoryContextSwitchTo(so->qcxt);
	ScanKey		ob;
	Pointer		tofree;
	const float *v;
	int			dim;
	float	   *x;
	double		tuples;
	double		members;
	double		qnorm = 0.0;
	int			i;

	if (scan->xs_snapshot == InvalidSnapshot ||
		!LION_IS_MVCC_LIKE(scan->xs_snapshot))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("lionvec index \"%s\" can only be scanned with an MVCC snapshot",
						RelationGetRelationName(index))));

	so->info = lionvec_get_info(index);
	so->metric = so->info->metric;
	lionvec_read_meta(index, &so->meta);
	so->spill = (so->meta.flags & LIONVEC_META_SPILL) != 0;
	so->nblocks = RelationGetNumberOfBlocks(index);
	so->lastdist = so->lastkey = -INFINITY;

	/* no order to give: every row, chain by chain */
	if (scan->numberOfOrderBys == 0)
	{
		lv_start_walk(scan, so, LV_WALK_ALL);
		MemoryContextSwitchTo(old);
		return;
	}

	ob = &scan->orderByData[0];
	if (ob->sk_strategy != so->metric)
		elog(ERROR, "lionvec: index \"%s\" orders by strategy %d, not %d",
			 RelationGetRelationName(index), so->metric, ob->sk_strategy);
	{
		Oid			rettype = get_func_rettype(ob->sk_func.fn_oid);

		if (!lionvec_distance_type_ok(rettype))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("lionvec index \"%s\" is ordered by an operator returning %s, and lionvec orders by float8 or float4",
							RelationGetRelationName(index), format_type_be(rettype))));
		so->dist4 = rettype == FLOAT4OID;
	}
	if (ob->sk_flags & SK_ISNULL)
	{
		lv_start_walk(scan, so, LV_WALK_ALL);
		MemoryContextSwitchTo(old);
		return;
	}

	/*
	 * Detoasted once, for the operator's function at every exact distance;
	 * a halfvec's floats are converted into an array of their own.  Both
	 * are in qcxt, which the next rescan resets.
	 */
	so->query = PointerGetDatum(PG_DETOAST_DATUM(ob->sk_argument));
	v = lionvec_datum_floats(so->query, so->info->half, NULL, 0, &dim, &tofree);
	if (so->info->dims == 0)
	{
		/* no vector indexed yet: only NULLs to return */
		lv_start_walk(scan, so, LV_WALK_NULL);
		MemoryContextSwitchTo(old);
		return;
	}
	/* pgvector's error for a query of the wrong size */
	if (dim != so->info->dims)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("different %s dimensions %d and %d",
						lionvec_type_name(so->info->half), so->info->dims, dim)));

	x = palloc(sizeof(float) * so->info->dimsp);
	if (!lionvec_prepare(&so->info->rot, so->metric, v, dim, x))
	{
		/* cosine of a zero query: NaN to every vector, NULL to NULLs */
		lv_start_walk(scan, so, LV_WALK_NONNULL);
		MemoryContextSwitchTo(old);
		return;
	}
	for (i = 0; i < dim; i++)
		qnorm += (double) v[i] * (double) v[i];
	qnorm = sqrt(qnorm);
	so->q.dimsp = so->info->dimsp;
	so->q.qc = palloc(sizeof(float) * so->info->dimsp);
	so->q.planes = palloc0(sizeof(uint64) * (so->info->dimsp / 64) * LIONVEC_QBITS);
	lionvec_query_init(&so->q, so->metric, so->info->mean, x, lionvec_epsilon);

	/* how many rows the scan is of */
	tuples = index->rd_rel->reltuples > 0 ? index->rd_rel->reltuples :
		so->meta.ntuples;
	members = tuples;
	if (so->restricted)
	{
		BlockNumber hblocks = RelationGetNumberOfBlocks(scan->heapRelation);
		bool		whole;

		members = lv_members(so, hblocks > 0 ? tuples / hblocks : 0.0, &whole);
		if (!whole && members <= lionvec_exact_rows)
		{
			lv_exact(scan, so);
			MemoryContextSwitchTo(old);
			return;
		}
	}
	so->qnorm = qnorm;
	so->margin = lv_margin(so);

	so->patience = lionvec_patience > 0 ?
		(double) lionvec_patience * lv_patience_unit(so, members) *
		lv_patience_scale() : INFINITY;

	so->seen = lvseen_create(so->qcxt, 256, NULL);
	so->seenmost = lv_seen_most();
	so->batch = lionvec_batch;
	lv_reset_rows(so);
	lv_read_parts(scan, so);

	/* the rows in no partition first, then the partitions as they come */
	so->probed = palloc(sizeof(BlockNumber) * ((Size) so->meta.nparts + 1));
	so->probed[so->nprobed++] = so->meta.rest_head;
	so->npasses++;
	so->pass = 1;
	lv_probe(scan, so, so->meta.rest_head, false);
	so->phase = LV_NORMAL;
	MemoryContextSwitchTo(old);
}

/* ----------------------------------------------------------------
 * amgettuple
 * ----------------------------------------------------------------
 */

/* A distance as the ORDER BY operator returns it (lionvec_distance_type_ok()). */
static Datum
lv_distance_datum(const LvScanOpaque *so, double dist)
{
	return so->dist4 ? Float4GetDatum((float4) dist) : Float8GetDatum(dist);
}

static void
lv_return(IndexScanDesc scan, LvScanOpaque *so, ItemPointer tid, double dist)
{
	so->nreturned++;
	scan->xs_heaptid = *tid;
	scan->xs_recheck = false;
	scan->xs_recheckorderby = false;
	scan->xs_orderbyvals[0] = lv_distance_datum(so, dist);
	scan->xs_orderbynulls[0] = false;
}

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

					if (so->full)
					{
						lv_seen_stop(scan, so);
						break;
					}
					if (so->rows.n == 0 && so->parts.n == 0)
					{
						lv_next_phase(scan, so);
						break;
					}
					old = MemoryContextSwitchTo(so->qcxt);

					/* a pause ends after L rows or past tau (the file's header) */
					if (so->paused &&
						(so->rows.n == 0 || so->pauseleft <= 0 ||
						 so->rows.items[0].key > lv_pause_tau(so)))
					{
						/* patience starts over, for the tier that paused */
						so->paused = false;
						if (so->pausetier >= 0)
							so->tiers[so->pausetier].at = so->scored;
						else
							so->hiat = so->scored;
					}

					/* a partition, when it comes before every row */
					if (so->parts.n > 0 && !so->paused &&
						(so->rows.n == 0 ||
						 so->parts.items[0].key < so->rows.items[0].key))
					{
						if (!lv_may_probe(so, false))
						{
							MemoryContextSwitchTo(old);
							lv_seen_stop(scan, so);
							break;
						}
						item = lv_heap_pop(&so->parts);
						if (item.level == LV_CENT1)
							lv_refine_part(scan, so, &item);
						else
							lv_probe_part(scan, so, &item);
						MemoryContextSwitchTo(old);
						break;
					}

					item = lv_heap_pop(&so->rows);
					if (so->twice && item.level != LV_REST &&
						lv_seen_gone(so, &item.tid))
					{
						/* a row's second item, after an eviction: returned already */
						so->ncopies++;
						MemoryContextSwitchTo(old);
						break;
					}
					switch (item.level)
					{
						case LV_REST:
							if (!lv_may_probe(so, true))
							{
								MemoryContextSwitchTo(old);
								lv_seen_stop(scan, so);
								continue;
							}
							lv_rest(scan, so);
							break;
						case LV_PLANE1:
							lv_refine(scan, so, &item);
							break;
						case LV_CODES8:
							lv_rerank(scan, so, &item);
							break;
						case LV_EXACT:
							MemoryContextSwitchTo(old);
							lv_mark_done(so, &item);
							if (item.dist < so->lastdist)
							{
								/* found too late: see the file's header */
								so->nskipped++;
								continue;
							}
							so->lastdist = item.dist;
							so->lastkey = item.key;
							so->pauseleft--;
							so->passret++;
							lv_return(scan, so, &item.tid, item.dist);
							return true;
					}
					MemoryContextSwitchTo(old);
				}
				break;

			case LV_LIST:
				if (so->epos >= so->nexact)
				{
					lv_next_phase(scan, so);
					break;
				}
				lv_return(scan, so, &so->exact[so->epos].tid,
						  so->exact[so->epos].dist);
				so->epos++;
				return true;

			case LV_WALK:
				{
					ItemPointerData tid;

					if (!lv_walk_next(scan, so, &tid))
					{
						lv_next_phase(scan, so);
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
							lv_distance_datum(so, get_float8_nan()) : (Datum) 0;
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
