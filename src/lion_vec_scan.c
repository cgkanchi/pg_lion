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
 * A pause on a tier below the query's LIMIT ends by count, and probing
 * resumes for the rest, to skip what it then finds nearer than the rows
 * returned: a LIMIT 100 paused at 10, 20, 40, 80 and 100, and under a 3%
 * filter at a margin of 8 got 0.9884 of its hundred nearest.  So a scan
 * told the rows its query takes (lion_vec_scan_limit(): LionOrdered's, from
 * the planner's LIMIT and OFFSET) keeps no tier below them; that LIMIT 100
 * pauses on L alone and gets 0.9940, probing the same partitions, and a
 * LIMIT 10 is as it was.  And L is no fewer than those rows, to an eighth
 * of work_mem's (lv_start()): a LIMIT 1000, which made two or three passes
 * of L = 100, makes one, reading a third to a half of the pages.  The
 * plain scan, which core's Index Scan runs and no plan reaches, keeps
 * every tier and L (DESIGN.md §44.4, "A known LIMIT").
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
 * §44.6) of no more than pg_lion.vector_exact_rows members, or
 * pg_lion.vector_exact_per_partition members a partition if that is more
 * (lionvec_exact_limit()), computes every member's distance from the heap
 * and returns them in order (lv_exact()); of more, it does all of the above
 * over the members alone.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/detoast.h"
#include "access/heaptoast.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "pgstat.h"
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/float.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/spccache.h"

#include "lion_compat.h"
#include "lion_readahead.h"
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
	bool		copy;			/* a row's: met by its copy (LIONVEC_SPILL) */
	bool		pf;				/* the page its level reads next asked for
								 * (lv_prefetch_queue()) */
	bool		tp;				/* LV_CODES8: its REFINE item has its vector's
								 * place in TOAST, whose first block is tblk
								 * (DESIGN.md §44.13) */
	OffsetNumber roff;
	BlockNumber rblk;
	float4		norm;
	union
	{
		float4		xm;			/* to LV_PLANE1 */
		BlockNumber tblk;		/* LV_CODES8, tp: as above */
	}			u;
} LvItem;

StaticAssertDecl(sizeof(LvItem) == 48, "lionvec: a queue entry is six words");

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

/*
 * a member of a small restriction, with its distance (lv_exact()): from the
 * heap, or read by its place in TOAST (DESIGN.md §44.13), its value's id
 * then kept for the check before it is returned (lv_check_placed())
 */
#define LV_X_HEAP		0		/* not read yet, or from the heap */
#define LV_X_PLACED		1		/* read by place, checked when returned */
#define LV_X_DROP		2		/* read by place, a cosine zero vector: the
								 * zero walk's, as the heap path leaves it */

typedef struct LvExact
{
	double		dist;
	ItemPointerData tid;
	uint8		how;			/* LV_X_* */
	Oid			valueid;		/* LV_X_PLACED */
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
 * A scan told its LIMIT (lion_vec_scan_limit()) keeps only the tiers of K
 * at or past it, and none under a LIMIT of L or more, which pauses on L
 * (and L is then the LIMIT, lv_start()).
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

/*
 * A parallel scan's shared state (DESIGN.md §44.4, "A parallel scan"), in
 * the area core gives the access method after its ParallelIndexScanDesc: one
 * claim bit a chain - partition p's bit p, then the REST chain's and the
 * NULL chain's - which the first process to probe or walk that chain sets,
 * and, for an index that spills, the rows the processes have returned, so
 * that a row whose two items two processes probed is returned once.  Of a
 * fixed size, as before PostgreSQL 18 the estimate cannot see the index:
 * 32 KB of claims, check_meta() holding the partitions to LIONVEC_MAX_PARTS,
 * and 256 KB of rows.
 */
#define LV_SHARED_MAGIC		0x4c565053	/* "LVPS" */
#define LV_CLAIM_BITS		(LIONVEC_MAX_PARTS + 2)
#define LV_CLAIM_WORDS		((LV_CLAIM_BITS + 31) / 32)

/*
 * The rows returned: an open-addressed set of their TIDs (lv_tidkey(), with
 * LV_TAKEN_USED, so that no key is zero, which an empty slot is), each slot
 * set once by a compare-and-swap from zero and never cleared but by
 * amparallelrescan, which core calls with no worker running.  A row looks
 * for its key, or an empty slot, among LV_TAKEN_PROBES slots from its hash;
 * finding neither it can never be recorded, those slots being taken for
 * good, and then its own item returns it and its copy does not
 * (lv_take_row()).
 */
#define LV_TAKEN_SLOTS		32768		/* a power of two */
#define LV_TAKEN_PROBES		32
#define LV_TAKEN_USED		(UINT64CONST(1) << 63)
/* pg_atomic_uint64's alignment, which MAXALIGN need not give */
#define LV_SHARED_ALIGN		8

typedef struct LvShared
{
	uint32		magic;			/* LV_SHARED_MAGIC */
	uint32		nwords;			/* LV_CLAIM_WORDS */
	pg_atomic_uint64 taken[LV_TAKEN_SLOTS];
	pg_atomic_uint32 claims[LV_CLAIM_WORDS];
} LvShared;

/* the REST chain in so->probed, beside the partitions' numbers */
#define LV_REST_CHAIN		PG_UINT32_MAX

/*
 * Reading ahead (DESIGN.md §44.4, "Prefetch" and "Read streams"): how much
 * of a queue's top a look ahead into it reads (lv_prefetch_queue()).  What a
 * scan knows of its asks of each relation is a LionAsk (lion_readahead.h):
 * the blocks it has asked for lately, so that the entries of a queue whose
 * items share a page - five centroids' REFINE items to a page at 1536
 * dimensions - ask for it once, and whether asking pays, an ask of a page
 * in shared buffers already being a lookup for nothing, 4 to 9% of a warm
 * DBpedia scan: after 32 asks in a row that found their page there the next
 * 256 pass unasked.  A stream rests likewise, seeing its own reads
 * (lion_readahead_dormant()).  The PART chain does not count
 * (lv_read_parts()): every scan reads it, so it is warm whatever else is.
 */
#define LV_PF_TOP(dist)		(4 * (dist) + 1)	/* the queue's top looked at */

/*
 * A scan's read stream holds in pins twice the pages it reads ahead
 * (lv_start_prefetch()), and no more than this part of its backend's fair
 * share of shared_buffers (lion_pin_fair_share()), its three streams of the
 * index three such parts: a pool that leaves a stream fewer than
 * LION_READAHEAD_MIN_PINS has the scan ask the kernel instead.
 */
#define LV_RA_SHARE			8

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
	int64		limit;			/* rows it takes, -1 if not known
								 * (lion_vec_scan_limit()) */

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
	uint32	   *pnrows;			/* [nparts]: ... and the items the build put
								 * in it (lionvec_chain_pages()) */
	double	   *plift;			/* IP: [nparts][2], each one's maxnorm^2 and
								 * spread (lv_part_key()) */
	uint32	   *probed;			/* the chains probed, in order: partitions'
								 * numbers, or LV_REST_CHAIN */
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

	/* prefetch (DESIGN.md §44.4, "Prefetch"): pages ahead, 0 for none */
	int			ipf;			/* ... of the index */
	int			hpf;			/* ... of the heap */
	BlockNumber hblocks;		/* the heap's, when hpf is set */
	int			perpage;		/* CODE items a page (lionvec_page_items()) */
	int			rpfdue;			/* rows to refine before the next look ahead */
	int			fpfdue;			/* ... to fetch */
	int			cpfdue;			/* centroids to refine */
	double	   *pfkey;			/* [LV_PF_TOP(LIONVEC_PREFETCH_MAX)] */
	int		   *pfat;			/* ... their places in the queue */
	int		   *pfsel;			/* [LIONVEC_PREFETCH_MAX]: lionvec_least()'s */
	uint64		wpfnext;		/* a walk's next page to ask for ... */
	uint64		wpfend;			/* ... and the block it stops at */
	int			rmode;			/* LION_READAHEAD_*, pg_lion.vector_prefetch
								 * as the scan started */
	LionAsk		iask;			/* the index's asks of the kernel */
	LionAsk		hask;			/* the heap's */
	LionReadahead *chains;		/* the index's reads of its chains ... */
	LionReadahead *crefs;		/* ... of centroids' REFINE pages ... */
	LionReadahead *rrefs;		/* ... and of rows', in so->cxt, kept from
								 * scan to scan while their mode and pins
								 * stay (lv_readahead_begin()) */
	int			rpins;			/* ... their pins */
	bool		inrest;			/* lv_rest() is probing so->probed ... */
	int			restpf;			/* ... and its next chain to read ahead */
	LionReadaheadCounts rcounts;	/* the streams', for DEBUG1 */

	/* a parallel scan (DESIGN.md §44.4, "A parallel scan"); NULL: serial */
	LvShared   *shared;
	bool		nullasked;		/* the NULL chain's claim was tried ... */
	bool		nullmine;		/* ... and this process has the chain */

	/* the heap side of an exact distance */
	TupleTableSlot *slot;
#if PG_VERSION_NUM < 200000
	struct IndexFetchTableData *fetch;
#endif
	IndexInfo  *indexInfo;
	EState	   *estate;

	/*
	 * Vectors in TOAST read by their places (DESIGN.md §44.13,
	 * lv_start_toast()): by the exact path from the locator, by the rerank
	 * from the REFINE items' slots, each checked as it is read and against
	 * the heap before it is returned
	 */
	bool		tdirect;		/* the exact path reads by place */
	int			tref;			/* the rerank reads by place: the REFINE
								 * items' runs, or 0 */
	Relation	toastrel;		/* the heap's TOAST relation, kept open from
								 * the first scan that reads it to the end */
	BlockNumber tnblocks;		/* ... its blocks, counted as each scan starts */
	Snapshot	tsnap;			/* the TOAST snapshot ... */
	SnapshotData tsnapdata;		/* ... on 16 and 17 this */
	int64		textsize;		/* a value's bytes in TOAST */
	int			tnchunks;		/* ... and its chunks */
	struct varlena *tvalue;		/* a value read so, VARHDRSZ + textsize */
	int			ttried;			/* reads by place this scan, and ... */
	int			tfailed;		/* ... of them those that fell back */
	int			tpf;			/* TOAST pages ahead (as hpf), 0 for none */
	LionAsk		task;			/* the TOAST relation's asks */

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
	int64		nclaimed;		/* partitions another process probed */
	int64		ntaken;			/* rows another process returned */
	int64		nplaced;		/* rows read by place (§44.13) ... */
	int64		nplacefail;		/* ... and reads by place that fell back */
	int64		nlocpages;		/* LOC pages read */
	int64		nchecked;		/* rows read by place checked at return */
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

/* A row's TID as a key: the progress hash's, and a parallel scan's rows' */
static inline uint64
lv_tidkey(const ItemPointerData *tid)
{
	return ((uint64) ItemPointerGetBlockNumber(tid) << 16) |
		ItemPointerGetOffsetNumber(tid);
}

/* The upper bound past which the pause's rows are all returned. */
static double
lv_pause_tau(const LvScanOpaque *so)
{
	Assert(so->pausetier < so->ntiers);
	Assert(so->pausetier < 0 || so->tiers[so->pausetier].n > 0);
	return so->pausetier >= 0 ? so->tiers[so->pausetier].h[0] : so->tau;
}

/* ----------------------------------------------------------------
 * Reading ahead (DESIGN.md §44.4, "Prefetch" and "Read streams")
 * ----------------------------------------------------------------
 */

static bool lv_claimed(const LvScanOpaque *so, uint32 bit);

/*
 * Where a run of a chain is read ahead from, its first page blk being read
 * now: through a stream every page of it comes from the stream, the first
 * too; asking the kernel, the first is read as it is asked for, and the
 * asks start after it.
 */
static inline uint64
lv_run_start(const LvScanOpaque *so, BlockNumber blk)
{
	return (uint64) blk +
		(lion_readahead_mode(so->chains) == LION_READAHEAD_STREAM ? 0 : 1);
}

/*
 * The pages of the index from *next up to end (lionvec_prefetch_end()),
 * none past the blocks it had when last counted: a run of a chain the build
 * wrote, ahead of its reader - urgent to a stream, which the reader is in
 * the middle of.
 */
static void
lv_prefetch_run(LvScanOpaque *so, uint64 *next, uint64 end)
{
	if (end > so->nblocks)
		end = so->nblocks;
	for (; *next < end; (*next)++)
		(void) lion_readahead_ask(so->chains, (BlockNumber) *next, true, false);
}

/*
 * The heap block of a row, as its exact distance will read it: asked of the
 * kernel whichever way the index is read, a stream's pins on heap pages not
 * being kept from one row returned to the next (DESIGN.md §44.4, "Read
 * streams").
 */
static void
lv_prefetch_heap(IndexScanDesc scan, LvScanOpaque *so, ItemPointer tid)
{
	BlockNumber blk = ItemPointerGetBlockNumberNoCheck(tid);

	/* a row inserted since the scan counted the blocks is left to its read */
	if (blk < so->hblocks)
		lion_ask(&so->hask, scan->heapRelation, blk, true);
}

/*
 * The key up to which the rows' queue is worked before the next partition
 * is probed - that partition's, or past the end of a pause (lv_pause_tau())
 * if that is later: a row beyond it waits for the probe, which looks into
 * the queue again (lv_probe()).  On DBpedia a look at the least 64 rows
 * after every probe asked for 2,941 REFINE pages of a scan that refined 97
 * rows.
 */
static double
lv_prefetch_bound(const LvScanOpaque *so)
{
	double		bound = so->parts.n > 0 ? so->parts.items[0].key : INFINITY;

	if (so->paused)
		bound = Max(bound, lv_pause_tau(so));
	return bound;
}

/*
 * Look ahead into a queue: of its top LV_PF_TOP(dist) entries, which hold
 * its least (a binary heap's first levels), the dist least of those at
 * level, whose pages ask to be read next - a REFINE item's, a row's or a
 * centroid's, or, fetch, a row's heap block, and the first TOAST block of a
 * row its REFINE item placed (tp, DESIGN.md §44.13) - and, of the rows'
 * queue, only those the scan works before its next probe
 * (lv_prefetch_bound()).  dist is the relation's (so->ipf, so->hpf, and
 * so->tpf for rows placed).  An entry asks once at its level (pf), however
 * many looks find it near the top.  A stream is given them in the order of
 * their keys, which is the order the scan reads them in unless a probe puts
 * rows before them; the kernel is asked in lionvec_least()'s.  Returns how
 * many it chose: the caller looks again after half as many such entries
 * have come out, so that what it asked for stays about dist ahead
 * (lv_prefetch_due()).
 */
static int
lv_prefetch_queue(IndexScanDesc scan, LvScanOpaque *so, LvHeap *h,
				  uint8 level, bool fetch)
{
	int			dist = fetch ? Max(so->hpf, so->tref > 0 ? so->tpf : 0) : so->ipf;
	double		bound = h == &so->rows ? lv_prefetch_bound(so) : INFINITY;
	LionReadahead *ra = h == &so->rows ? so->rrefs : so->crefs;
	int			top;
	int			n = 0;
	int			m;
	int			i;

	if (dist <= 0)
		return 0;
	/* a resting stream takes nothing: no look (lion_readahead_dormant()) */
	if (!fetch && lion_readahead_dormant(ra))
		return 0;
	Assert(dist <= LIONVEC_PREFETCH_MAX && so->pfkey != NULL);
	top = Min(h->n, LV_PF_TOP(dist));
	for (i = 0; i < top; i++)
	{
		if (h->items[i].level != level || !(h->items[i].key <= bound))
			continue;
		so->pfkey[n] = h->items[i].key;
		so->pfat[n++] = i;
	}
	m = lionvec_least(so->pfkey, n, dist, so->pfsel);
	/* to a stream by key, at equal keys by place: m is at most dist, 64 */
	if (!fetch && lion_readahead_mode(ra) == LION_READAHEAD_STREAM)
		lionvec_least_order(so->pfkey, so->pfsel, m);
	for (i = 0; i < m; i++)
	{
		LvItem	   *it = &h->items[so->pfat[so->pfsel[i]]];

		if (it->pf)
			continue;
		it->pf = true;
		if (fetch && it->tp && so->tpf > 0)
		{
			/* its heap block, for the check, and its vector's first page */
			lv_prefetch_heap(scan, so, (ItemPointer) &it->tid);
			if (it->u.tblk < so->tnblocks)
				lion_ask(&so->task, so->toastrel, it->u.tblk, true);
		}
		else if (fetch)
		{
			if (so->hpf > 0)
				lv_prefetch_heap(scan, so, (ItemPointer) &it->tid);
		}
		else if (it->rblk < so->nblocks)
			(void) lion_readahead_ask(ra, it->rblk, false, true);
	}
	return m;
}

/* ... when *due entries at that level have come out since the last look */
static void
lv_prefetch_due(IndexScanDesc scan, LvScanOpaque *so, int *due,
				LvHeap *h, uint8 level, bool fetch)
{
	if (--(*due) > 0)
		return;
	*due = Max(1, lv_prefetch_queue(scan, so, h, level, fetch) / 2);
}

/*
 * The REFINE pages' refills (LionReadaheadRefill): a stream that wants a
 * page and has none pending looks into its queue at once, rather than once
 * the look it was given is half read (lv_prefetch_due()), so that it does
 * not run dry and end - core's stream, once its callback names no block,
 * reads no further until it is reset, and then from one page ahead.  Inside
 * the stream's callback: they take entries from memory and read nothing.
 */
static bool
lv_refs_refill(IndexScanDesc scan, LionReadahead *ra, LvHeap *h, uint8 level)
{
	LvScanOpaque *so = (LvScanOpaque *) scan->opaque;
	int			before;

	if (so->phase != LV_NORMAL)
		return false;
	before = lion_readahead_depth(ra);
	(void) lv_prefetch_queue(scan, so, h, level, false);
	return lion_readahead_depth(ra) > before;
}

static bool
lv_crefs_refill(void *arg)
{
	IndexScanDesc scan = (IndexScanDesc) arg;
	LvScanOpaque *so = (LvScanOpaque *) scan->opaque;

	return lv_refs_refill(scan, so->crefs, &so->parts, LV_CENT1);
}

static bool
lv_rrefs_refill(void *arg)
{
	IndexScanDesc scan = (IndexScanDesc) arg;
	LvScanOpaque *so = (LvScanOpaque *) scan->opaque;

	return lv_refs_refill(scan, so->rrefs, &so->rows, LV_PLANE1);
}

/*
 * A chain's first pages, from head, into the chains' stream as predicted:
 * the run the build wrote (pages long, 0 when not known: then the head
 * alone), to depth pages ahead of the reads in all.  True if any went in.
 */
static bool
lv_predict_chain(LvScanOpaque *so, BlockNumber head, uint32 pages, int depth)
{
	uint64		end = (uint64) head + Max(pages, 1);
	uint64		b;
	bool		pushed = false;

	if (!BlockNumberIsValid(head))
		return false;
	end = Min(end, (uint64) so->nblocks);
	for (b = head; b < end && lion_readahead_depth(so->chains) < depth; b++)
		pushed |= lion_readahead_ask(so->chains, (BlockNumber) b, false, false);
	return pushed;
}

/*
 * The chains' refill (LionReadaheadRefill), when the stream wants a page and
 * the run being read is all given: the chains the scan will probe next.
 * Inside a REST (lv_rest()) those are known, so->probed in order; between
 * them, the next partition is the partition queue's least, which a centroid
 * refined later may still come before: of its top, the least centroid
 * known from its codes (LV_CENT8) whose chain is not asked for yet (pf),
 * one another process has claimed passed over.  Not while paused, nor
 * outside the queues' phase - the PART chain, the walks.  Up to so->ipf
 * pages ahead, a chain at a time: a chain the scan does not probe after
 * all, its LIMIT met or a centroid coming first, is read for nothing, and
 * released unread.  Inside the stream's callback: it reads nothing.
 */
static bool
lv_chains_refill(void *arg)
{
	LvScanOpaque *so = (LvScanOpaque *) arg;
	int			depth = so->ipf;

	if (so->phase != LV_NORMAL || depth <= 0)
		return false;
	if (so->inrest)
	{
		while (so->restpf < so->nprobed &&
			   lion_readahead_depth(so->chains) < depth)
		{
			uint32		p = so->probed[so->restpf++];
			bool		pushed;

			if (p == LV_REST_CHAIN)
				pushed = lv_predict_chain(so, so->meta.rest_head, 0, depth);
			else
			{
				Assert(p < so->meta.nparts);
				pushed = lv_predict_chain(so, so->phead[p],
										  lionvec_chain_pages(so->pnrows[p],
															  so->perpage),
										  depth);
			}
			if (pushed)
				return true;
		}
		return false;
	}
	while (!so->paused && lion_readahead_depth(so->chains) < depth)
	{
		int			top = Min(so->parts.n, LV_PF_TOP(depth));
		LvItem	   *best = NULL;
		BlockNumber partno;
		int			i;

		for (i = 0; i < top; i++)
		{
			LvItem	   *it = &so->parts.items[i];

			if (it->level == LV_CENT8 && !it->pf &&
				(best == NULL || lv_before(it, best)))
				best = it;
		}
		if (best == NULL)
			return false;
		best->pf = true;
		partno = LionVecCentPartno(&best->tid);
		Assert(partno < so->meta.nparts);
		if (lv_claimed(so, partno))
			continue;
		if (lv_predict_chain(so, so->phead[partno],
							 lionvec_chain_pages(so->pnrows[partno],
												 so->perpage),
							 depth))
			return true;
	}
	return false;
}

/* What the streams did, into the scan's counts. */
static void
lv_readahead_counts(LvScanOpaque *so)
{
	if (so->chains != NULL)
		lion_readahead_counts(so->chains, &so->rcounts);
	if (so->crefs != NULL)
		lion_readahead_counts(so->crefs, &so->rcounts);
	if (so->rrefs != NULL)
		lion_readahead_counts(so->rrefs, &so->rcounts);
}

/*
 * The scan is done with the index's pages for now - a rescan, a walk after
 * the queues, the end: what its streams read ahead is released (counted as
 * unread), and the pages pending forgotten.
 */
static void
lv_readahead_reset(LvScanOpaque *so)
{
	lv_readahead_counts(so);
	if (so->chains != NULL)
		lion_readahead_reset(so->chains);
	if (so->crefs != NULL)
		lion_readahead_reset(so->crefs);
	if (so->rrefs != NULL)
		lion_readahead_reset(so->rrefs);
	so->inrest = false;
}

/* ... and for good: endscan, or a scan that reads otherwise now */
static void
lv_readahead_end(LvScanOpaque *so)
{
	lv_readahead_reset(so);
	if (so->chains != NULL)
		lion_readahead_end(so->chains);
	if (so->crefs != NULL)
		lion_readahead_end(so->crefs);
	if (so->rrefs != NULL)
		lion_readahead_end(so->rrefs);
	so->chains = so->crefs = so->rrefs = NULL;
}

/* ----------------------------------------------------------------
 * A parallel scan (DESIGN.md §44.4, "A parallel scan")
 * ----------------------------------------------------------------
 */

/* The shared state, from the area core gives the access method. */
static LvShared *
lv_shared_at(void *target)
{
	return (LvShared *) TYPEALIGN(LV_SHARED_ALIGN, target);
}

/* ... of a scan's ParallelIndexScanDesc, checked; NULL for a serial scan */
static LvShared *
lv_shared(IndexScanDesc scan)
{
	ParallelIndexScanDesc ps = scan->parallel_scan;
	LvShared   *sh;

	if (ps == NULL)
		return NULL;
#if PG_VERSION_NUM >= 180000
	sh = lv_shared_at(OffsetToPointer(ps, ps->ps_offset_am));
#else
	sh = lv_shared_at(OffsetToPointer(ps, ps->ps_offset));
#endif
	Assert(sh->magic == LV_SHARED_MAGIC);
	if (sh->magic != LV_SHARED_MAGIC || sh->nwords != LV_CLAIM_WORDS)
		elog(ERROR, "lionvec: index \"%s\" has no parallel scan state",
			 RelationGetRelationName(scan->indexRelation));
	return sh;
}

#if PG_VERSION_NUM >= 180000
Size
lionvecestimateparallelscan(Relation index, int nkeys, int norderbys)
#elif PG_VERSION_NUM >= 170000
Size
lionvecestimateparallelscan(int nkeys, int norderbys)
#else
Size
lionvecestimateparallelscan(void)
#endif
{
	return add_size(sizeof(LvShared), LV_SHARED_ALIGN);
}

void
lionvecinitparallelscan(void *target)
{
	LvShared   *sh = lv_shared_at(target);
	int			i;

	sh->magic = LV_SHARED_MAGIC;
	sh->nwords = LV_CLAIM_WORDS;
	for (i = 0; i < LV_TAKEN_SLOTS; i++)
		pg_atomic_init_u64(&sh->taken[i], 0);
	for (i = 0; i < LV_CLAIM_WORDS; i++)
		pg_atomic_init_u32(&sh->claims[i], 0);
}

/* Core calls it with no worker running: every chain is free again. */
void
lionvecparallelrescan(IndexScanDesc scan)
{
	LvShared   *sh = lv_shared(scan);
	int			i;

	Assert(sh != NULL);
	if (sh == NULL)
		return;
	for (i = 0; i < LV_TAKEN_SLOTS; i++)
		pg_atomic_write_u64(&sh->taken[i], 0);
	for (i = 0; i < LV_CLAIM_WORDS; i++)
		pg_atomic_write_u32(&sh->claims[i], 0);
}

/*
 * Claim a chain by its bit (LvShared): whether this process is the first to,
 * and so probes or walks it.  A serial scan has every chain.
 */
static bool
lv_claim(LvScanOpaque *so, uint32 bit)
{
	uint32		mask = (uint32) 1 << (bit % 32);

	if (so->shared == NULL)
		return true;
	if (bit >= LV_CLAIM_BITS)
		elog(ERROR, "lionvec: chain %u of a parallel scan past the %d it has room for",
			 bit, LV_CLAIM_BITS);
	return (pg_atomic_fetch_or_u32(&so->shared->claims[bit / 32], mask) & mask) == 0;
}

/* ... and whether another process has, without claiming it */
static bool
lv_claimed(const LvScanOpaque *so, uint32 bit)
{
	uint32		mask = (uint32) 1 << (bit % 32);

	if (so->shared == NULL || bit >= LV_CLAIM_BITS)
		return false;
	return (pg_atomic_read_u32(&so->shared->claims[bit / 32]) & mask) != 0;
}

/*
 * Whether this process returns a row of a parallel scan of an index that
 * spills: its two items may lie in two processes' partitions, and each would
 * find it.  The first to record its TID (LvShared) returns it; a row that
 * finds no room is returned by the process that met its own item, and not
 * through its copy - each partition is one process's, and one partition
 * holds a row's own item.  Without spill a row is in one chain, which one
 * process probes.
 */
static bool
lv_take_row(LvScanOpaque *so, const LvItem *item)
{
	uint64		key = lv_tidkey(&item->tid) | LV_TAKEN_USED;
	uint32		h = murmurhash32((uint32) key ^ (uint32) (key >> 32) * 0x9E3779B1u);
	int			i;

	for (i = 0; i < LV_TAKEN_PROBES; i++)
	{
		pg_atomic_uint64 *slot = &so->shared->taken[(h + i) & (LV_TAKEN_SLOTS - 1)];
		uint64		v = pg_atomic_read_u64(slot);

		if (v == 0)
		{
			uint64		expected = 0;

			if (pg_atomic_compare_exchange_u64(slot, &expected, key))
				return true;
			v = expected;		/* another process filled it first */
		}
		if (v == key)
			return false;
	}
	return !item->copy;
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
	/* LionOrdered, which alone restricts a scan, runs it serially (§44.6) */
	Assert(scan->parallel_scan == NULL);
	if (scan->parallel_scan != NULL)
		elog(ERROR, "lionvec: a parallel scan is restricted");
	if (n < 0 || (n > 0 && (keys == NULL || conts == NULL)))
		elog(ERROR, "lionvec: malformed restriction");
	/* lv_key_search() bisects: one pass over the keys is cheap next to a scan */
	for (i = 0; i < n; i++)
	{
		if (i > 0 && keys[i] <= keys[i - 1])
			elog(ERROR, "lionvec: restriction keys out of order");
		if (conts[i] == NULL)
			elog(ERROR, "lionvec: a restriction key without its container");
	}
	so->restricted = true;
	so->rn = n;
	so->rkeys = keys;
	so->rconts = conts;
	so->rpos = 0;
}

/*
 * The rows the scan's query will take from it, when the planner knew them
 * (lion_vec.h): lv_reset_rows() makes no tier below them.  Like the
 * restriction, it is given after every rescan, which forgets it.
 */
void
lion_vec_scan_limit(IndexScanDesc scan, int64 rows)
{
	LvScanOpaque *so;

	if (!lion_vec_relation(scan->indexRelation))
		elog(ERROR, "lionvec: \"%s\" is not a lionvec index",
			 RelationGetRelationName(scan->indexRelation));
	so = (LvScanOpaque *) scan->opaque;
	if (so->phase != LV_START)
		elog(ERROR, "lionvec: a scan is given its LIMIT after it has started");
	if (rows != -1 && rows <= 0)
		elog(ERROR, "lionvec: malformed LIMIT " INT64_FORMAT, rows);
	so->limit = rows;
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
	return lion_container_contains(so->rconts[so->rpos], lion_code_lo(code));
}

/* How many rows the restriction admits: its containers' members. */
static double
lv_members(const LvScanOpaque *so)
{
	double		n = 0.0;
	int			k;

	for (k = 0; k < so->rn; k++)
		n += lion_container_cardinality(so->rconts[k]);
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
	so->limit = -1;
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

/*
 * The scan's counts, at DEBUG1, when a rescan or the end forgets them.  How
 * it read ahead is named when it did (DESIGN.md §44.4, "Prefetch" and "Read
 * streams"): asking the kernel, the pages it asked for and those of them
 * not in shared buffers; through streams, the index's pages the streams
 * served and those of them read in, from outside shared buffers, the pages
 * read apart, which no stream was given, and those the streams read ahead
 * and the scan never read - and the heap's pages asked for, which no stream
 * reads (lv_prefetch_heap()).  A parallel scan's process names the
 * partitions others probed and the rows others returned, each process
 * sending its own line ("A parallel scan").  A scan that read vectors by
 * their places in TOAST (DESIGN.md §44.13) names how many, the LOC pages it
 * read for them, the reads that fell back to the heap, and the TOAST pages
 * it asked for.
 */
static void
lv_debug_stats(IndexScanDesc scan, LvScanOpaque *so)
{
	char		more[640];
	bool		streamed = so->rmode == LION_READAHEAD_STREAM;

	more[0] = '\0';
	lv_readahead_counts(so);
	if (streamed && (so->ipf > 0 || so->hpf > 0))
	{
		snprintf(more, sizeof(more), ", " INT64_FORMAT " pages streamed ("
				 INT64_FORMAT " read in), " INT64_FORMAT " read apart, "
				 INT64_FORMAT " streamed unread, " INT64_FORMAT " restarts",
				 so->rcounts.nstreamed, so->rcounts.nread,
				 so->rcounts.napart, so->rcounts.nwasted,
				 so->rcounts.nrestarts);
		if (so->hask.nasked > 0)
			snprintf(more + strlen(more), sizeof(more) - strlen(more),
					 ", " INT64_FORMAT " heap pages prefetched ("
					 INT64_FORMAT " not in shared buffers)",
					 so->hask.nasked, so->hask.nmissing);
	}
	else if (so->ipf > 0 || so->hpf > 0)
		snprintf(more, sizeof(more), ", " INT64_FORMAT " pages prefetched ("
				 INT64_FORMAT " not in shared buffers)",
				 so->iask.nasked + so->hask.nasked,
				 so->iask.nmissing + so->hask.nmissing);
	if (so->shared != NULL)
		snprintf(more + strlen(more), sizeof(more) - strlen(more),
				 ", " INT64_FORMAT " partitions another process probed, "
				 INT64_FORMAT " rows another returned",
				 so->nclaimed, so->ntaken);
	if (so->nplaced > 0 || so->nplacefail > 0 || so->nlocpages > 0)
		snprintf(more + strlen(more), sizeof(more) - strlen(more),
				 ", " INT64_FORMAT " read by place (" INT64_FORMAT
				 " locator pages, " INT64_FORMAT " not), " INT64_FORMAT
				 " checked",
				 so->nplaced, so->nlocpages, so->nplacefail, so->nchecked);
	if (so->task.nasked > 0)
		snprintf(more + strlen(more), sizeof(more) - strlen(more),
				 ", " INT64_FORMAT " TOAST pages prefetched (" INT64_FORMAT
				 " not in shared buffers)",
				 so->task.nasked, so->task.nmissing);
	if (so->npasses > 0)
		elog(DEBUG1, "lionvec scan of \"%s\": " INT64_FORMAT " partitions probed, "
			 INT64_FORMAT " passes, " INT64_FORMAT " candidates, "
			 INT64_FORMAT " refined, " INT64_FORMAT " fetched, "
			 INT64_FORMAT " returned, " INT64_FORMAT " skipped out of order, "
			 INT64_FORMAT " bounds missed, " INT64_FORMAT " pages read, "
			 INT64_FORMAT " pauses, " INT64_FORMAT " copies skipped%s",
			 RelationGetRelationName(scan->indexRelation), so->nprobes,
			 so->npasses, so->ncands, so->nrefined, so->nfetched,
			 so->nreturned, so->nskipped, so->nmissed, so->npages,
			 so->npauses, so->ncopies, more);
	if (so->nwalks > 0)
		elog(DEBUG1, "lionvec scan of \"%s\": its progress hash full "
			 INT64_FORMAT " times, " INT64_FORMAT " of them losing knowledge",
			 RelationGetRelationName(scan->indexRelation), so->nwalks,
			 so->nevictions);
	else if (so->nfetched > 0 || so->nplaced > 0 || so->nplacefail > 0)
		elog(DEBUG1, "lionvec scan of \"%s\": " INT64_FORMAT " members fetched, "
			 INT64_FORMAT " returned%s",
			 RelationGetRelationName(scan->indexRelation), so->nfetched,
			 so->nreturned, more);
	so->nprobes = so->npasses = so->ncands = so->nrefined = so->nfetched = 0;
	so->nreturned = so->nskipped = so->nmissed = so->npages = 0;
	so->npauses = so->ncopies = 0;
	so->nclaimed = so->ntaken = 0;
	so->nplaced = so->nplacefail = so->nlocpages = so->nchecked = 0;
	so->iask.nasked = so->iask.nmissing = 0;
	so->hask.nasked = so->hask.nmissing = 0;
	so->task.nasked = so->task.nmissing = 0;
	memset(&so->rcounts, 0, sizeof(so->rcounts));
}

void
lionvecrescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys,
			  int norderbys)
{
	LvScanOpaque *so = (LvScanOpaque *) scan->opaque;

	/* what the streams read ahead for the last scan, released first */
	lv_readahead_reset(so);
	lv_debug_stats(scan, so);
	if (keys && scan->numberOfKeys > 0)
		memmove(scan->keyData, keys, scan->numberOfKeys * sizeof(ScanKeyData));
	if (orderbys && scan->numberOfOrderBys > 0)
		memmove(scan->orderByData, orderbys,
				scan->numberOfOrderBys * sizeof(ScanKeyData));

	MemoryContextReset(so->qcxt);
	so->phase = LV_START;
	so->info = NULL;
	so->limit = -1;
	so->restricted = false;
	so->rn = 0;
	so->rkeys = NULL;
	so->rconts = NULL;
	memset(&so->rows, 0, sizeof(LvHeap));
	memset(&so->parts, 0, sizeof(LvHeap));
	so->phead = NULL;
	so->pnrows = NULL;
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
	so->ipf = so->hpf = 0;
	so->pfkey = NULL;
	so->pfat = so->pfsel = NULL;
	so->restpf = 0;
	so->shared = NULL;
	so->nullasked = so->nullmine = false;
	so->tdirect = false;
	so->tref = 0;
	so->tpf = 0;
	so->ttried = so->tfailed = 0;
}

void
lionvecendscan(IndexScanDesc scan)
{
	LvScanOpaque *so = (LvScanOpaque *) scan->opaque;

	lv_readahead_reset(so);
	lv_debug_stats(scan, so);
	lv_readahead_end(so);
	if (so->slot != NULL)
		ExecDropSingleTupleTableSlot(so->slot);
#if PG_VERSION_NUM < 200000
	if (so->fetch != NULL)
		table_index_fetch_end(so->fetch);
#endif
	if (so->estate != NULL)
		FreeExecutorState(so->estate);
	if (so->toastrel != NULL)
		table_close(so->toastrel, NoLock);
	MemoryContextDelete(so->cxt);
	scan->opaque = NULL;
}

/* ----------------------------------------------------------------
 * The exact distance
 * ----------------------------------------------------------------
 */

/* What the heap side of an exact distance keeps from one row to the next. */
static void
lv_heap_setup(IndexScanDesc scan, LvScanOpaque *so)
{
	Relation	heap = scan->heapRelation;
	MemoryContext old;

	if (heap == NULL)
		elog(ERROR, "lionvec: an ordered scan needs its table");
	if (so->slot != NULL)
		return;
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

/*
 * The version of the row at tid visible to the scan's snapshot, into
 * so->slot; false if there is none.  The caller clears the slot.
 */
static bool
lv_heap_fetch(IndexScanDesc scan, LvScanOpaque *so, ItemPointer tid)
{
	Relation	heap = scan->heapRelation;
	ItemPointerData htid = *tid;
	bool		all_dead = false;

	lv_heap_setup(scan, so);
#if PG_VERSION_NUM >= 200000
	return table_fetch_tid(heap, &htid, scan->xs_snapshot, &all_dead) &&
		table_tuple_fetch_row_version(heap, &htid, scan->xs_snapshot, so->slot);
#else
	{
		bool		call_again = false;

		(void) heap;
		return table_index_fetch_tuple(so->fetch, &htid, scan->xs_snapshot,
									   so->slot, &call_again, &all_dead);
	}
#endif
}

/*
 * The distance of the row in so->slot, with the operator's own function;
 * false if its vector is NULL.  Clears the slot.
 */
static bool
lv_slot_distance(IndexScanDesc scan, LvScanOpaque *so, double *dist)
{
	Datum		values[INDEX_MAX_KEYS];
	Datum		d;
	bool		isnull[INDEX_MAX_KEYS];
	ScanKey		ob = &scan->orderByData[0];
	ExprContext *econtext;
	MemoryContext old;

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
	bool		found = lv_heap_fetch(scan, so, tid);

	so->nfetched++;
	if (!found)
		return false;
	return lv_slot_distance(scan, so, dist);
}

/* ----------------------------------------------------------------
 * Vectors in TOAST read by their places (DESIGN.md §44.13)
 * ----------------------------------------------------------------
 */

/*
 * Whether the scan reads by place still: not once more than half of 64
 * reads or more have failed - a TOAST relation rewritten on its own, every
 * place stale - so that such a scan pays for 64 at most.
 */
static inline bool
lv_place_ok(const LvScanOpaque *so)
{
	return so->ttried < 64 || 2 * so->tfailed <= so->ttried;
}

/*
 * The value read into so->tvalue has the header a vector of the index's
 * dimensions has (lionvec_datum_dim(), and its unused field 0): bytes of
 * another value, read where a dead row's value was, never reach the
 * operator's function as a vector of other dimensions, which would raise
 * an error.
 */
static bool
lv_value_ok(const LvScanOpaque *so)
{
	const LionVecPgVector *v = (const LionVecPgVector *) so->tvalue;

	return lionvec_datum_dim(so->tvalue, VARSIZE(so->tvalue),
							 so->info->half) == so->info->dims &&
		v->unused == 0;
}

/* ... and its distance, with the operator's own function */
static double
lv_value_distance(IndexScanDesc scan, LvScanOpaque *so)
{
	ScanKey		ob = &scan->orderByData[0];
	ExprContext *econtext = GetPerTupleExprContext(so->estate);
	MemoryContext old;
	Datum		d;
	double		dist;

	ResetExprContext(econtext);
	old = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
	d = FunctionCall2Coll(&ob->sk_func, ob->sk_collation,
						  PointerGetDatum(so->tvalue), so->query);
	dist = so->dist4 ? (double) DatumGetFloat4(d) : DatumGetFloat8(d);
	MemoryContextSwitchTo(old);
	return dist;
}

/*
 * The check of a row read by place, its version visible to the scan in
 * so->slot (lv_heap_fetch()): its vector must be the value read, valueid
 * in the heap's TOAST relation.  A row the scan sees keeps its value where
 * the index found it (DESIGN.md §44.13, "Why a place that checks is the
 * right chunk"), and every version of it the same value, so anything else
 * is an index that names another value for a live row:
 * ERRCODE_INDEX_CORRUPTED, never a wrong distance.
 */
static void
lv_check_pointer(IndexScanDesc scan, LvScanOpaque *so, Oid valueid)
{
	AttrNumber	attno = so->indexInfo->ii_IndexAttrNumbers[0];
	bool		isnull = true;
	Datum		d = (Datum) 0;

	if (attno > 0)
		d = slot_getattr(so->slot, attno, &isnull);
	if (!isnull && lion_toast_ondisk_oid(DatumGetPointer(d)))
	{
		LionToastPointer ptr;

		memset(&ptr, 0, sizeof(ptr));
		VARATT_EXTERNAL_GET_POINTER(ptr, DatumGetPointer(d));
		if (ptr.va_valueid == valueid &&
			ptr.va_toastrelid == RelationGetRelid(so->toastrel))
			return;
	}
	ereport(ERROR,
			(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg("lionvec index \"%s\" places the vector of the row at (%u,%u) in TOAST value %u, and the row stores another",
					RelationGetRelationName(scan->indexRelation),
					ItemPointerGetBlockNumber(&so->slot->tts_tid),
					ItemPointerGetOffsetNumber(&so->slot->tts_tid), valueid)));
}

/*
 * The return-time check of an exact path's member read by place: false if
 * no version of the row is visible to the scan, which passes it over as
 * the heap path would have; ERRCODE_INDEX_CORRUPTED if one is and stores
 * another value (lv_check_pointer()).
 */
static bool
lv_check_placed(IndexScanDesc scan, LvScanOpaque *so, ItemPointer tid,
				Oid valueid)
{
	so->nchecked++;
	if (!lv_heap_fetch(scan, so, tid))
		return false;
	lv_check_pointer(scan, so, valueid);
	ExecClearTuple(so->slot);
	return true;
}

/*
 * Whether this scan reads vectors by their places in TOAST (DESIGN.md
 * §44.13): lionvec_reads_placed() of its meta page - an index that keeps a
 * locator, pg_lion.vector_toast_direct on, a heap of the heap access method
 * whose TOAST relation is the one the index was built over - which the
 * planner asks of the same meta page, so that it prices the exact path's
 * reads as they will be made (lion_vec.c, lionvec_startup_cost()); and a
 * regular MVCC snapshot, which the planner takes every query's to be.  (No
 * scan gets here under 16's old_snapshot_threshold, which would let a value
 * an old snapshot sees be gone with no error from a read by place:
 * lv_start() refuses it first.)  Then the exact path reads its members so,
 * from the locator (lv_exact_places()), and the rerank, in an index whose
 * REFINE items hold places, the rows it reranks (lv_rerank_placed()).  The
 * TOAST relation is opened once, locked as a detoast locks it, which keeps
 * it from being truncated or rewritten while the transaction lasts, so its
 * blocks counted now bound every place read.
 */
static void
lv_start_toast(IndexScanDesc scan, LvScanOpaque *so)
{
	Relation	heap = scan->heapRelation;
	int64		extsize;
	int			nchunks;

	so->tdirect = false;
	so->tref = 0;
	so->tpf = 0;
	so->ttried = so->tfailed = 0;
	lion_ask_reset(&so->task);
	if (!lionvec_reads_placed(heap, &so->meta, so->info->dims,
							  so->info->half) ||
		!IsMVCCSnapshot(scan->xs_snapshot))
		return;
	extsize = lionvec_toast_extsize(so->info->dims, so->info->half);
	nchunks = lionvec_toast_nchunks(extsize, LION_TOAST_MAX_CHUNK_SIZE);
	/* lionvec_reads_placed() holds a value's chunks to a LOC entry's runs */
	Assert(nchunks >= 1 && nchunks <= LIONVEC_LOC_MAXRUNS);

	if (so->toastrel != NULL &&
		RelationGetRelid(so->toastrel) != so->meta.toastrelid)
	{
		table_close(so->toastrel, NoLock);
		so->toastrel = NULL;
	}
	if (so->toastrel == NULL)
		so->toastrel = table_open(so->meta.toastrelid, AccessShareLock);
	if (so->tvalue == NULL || so->textsize != extsize)
	{
		if (so->tvalue != NULL)
			pfree(so->tvalue);
		so->tvalue = MemoryContextAlloc(so->cxt, VARHDRSZ + extsize);
	}
	SET_VARSIZE(so->tvalue, VARHDRSZ + extsize);
	so->textsize = extsize;
	so->tnchunks = nchunks;
	so->tsnap = lion_toast_snapshot(&so->tsnapdata);
	so->tnblocks = RelationGetNumberOfBlocks(so->toastrel);
	lv_heap_setup(scan, so);
	so->tdirect = true;
	so->tref = so->meta.refloc_runs;

	/* its pages ahead, as the heap's (lv_start_prefetch()) */
	if (so->rmode != LION_READAHEAD_OFF)
		so->tpf = Max(Min(get_tablespace_io_concurrency(so->toastrel->rd_rel->reltablespace),
						  LIONVEC_PREFETCH_MAX), 0);
	if (so->tref > 0 && so->tpf > 0)
	{
		/* the rerank asks for the heap block of each row it places */
		if (so->hpf <= 0)
			so->hblocks = RelationGetNumberOfBlocks(heap);
		if (so->pfkey == NULL)
		{
			so->pfkey = palloc(sizeof(double) * LV_PF_TOP(LIONVEC_PREFETCH_MAX));
			so->pfat = palloc(sizeof(int) * LV_PF_TOP(LIONVEC_PREFETCH_MAX));
			so->pfsel = palloc(sizeof(int) * LIONVEC_PREFETCH_MAX);
		}
	}
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
 * A list of blocks the exact path reads in its order (lv_exact()): the
 * members' heap blocks, a block that members one after another share
 * once, and those the scan counted (so->hblocks) only, through a read
 * stream; and the LOC pages and TOAST blocks of the members read by place
 * (lv_exact_places()), through a stream, or asked of the kernel dist
 * ahead, or read as they come (lv_list_begin()).
 */
typedef struct LvExactRead
{
	LionReadahead *ra;
	BlockNumber *blks;
	int			n;
	int			next;			/* the next to name to the stream */
	Relation	rel;			/* lv_list_*(): the relation ... */
	LionAsk    *ask;			/* ... its asks, dist ahead, when not
								 * streamed; or NULL */
	int			dist;
	int			pos;			/* the next to read */
	int			asked;			/* the next to ask for */
} LvExactRead;

/* LionReadaheadNext; inside the stream's callback, it reads nothing */
static BlockNumber
lv_exact_next(void *arg)
{
	LvExactRead *x = (LvExactRead *) arg;

	Assert(x->next >= 0 && x->next <= x->n);
	return x->next < x->n ? x->blks[x->next++] : InvalidBlockNumber;
}

/*
 * Read the n blocks blks[] of rel in order: through a list stream when the
 * scan streams (mode) and reads dist > 0 ahead, else asking the kernel dist
 * ahead through ask, else as they come.
 */
static void
lv_list_begin(LvExactRead *x, Relation rel, BlockNumber *blks, int n,
			  int mode, int dist, LionAsk *ask)
{
	memset(x, 0, sizeof(*x));
	x->rel = rel;
	x->blks = blks;
	x->n = n;
	if (n <= 0 || dist <= 0 || mode == LION_READAHEAD_OFF)
		return;
	if (mode == LION_READAHEAD_STREAM)
		x->ra = lion_readahead_begin_list(rel, LION_READAHEAD_STREAM,
										  lv_exact_next, x);
	if (x->ra == NULL)
	{
		x->ask = ask;
		x->dist = dist;
	}
}

/* The list's next block, which must be blk: pinned, not locked. */
static Buffer
lv_list_read(LvExactRead *x, BlockNumber blk)
{
	Buffer		buf = InvalidBuffer;

	if (x->pos >= x->n || x->blks[x->pos] != blk)
		elog(ERROR, "lionvec: a list read of \"%s\" lost its place at block %u",
			 RelationGetRelationName(x->rel), blk);
	if (x->ra != NULL)
	{
		BlockNumber got;

		if (!lion_readahead_next(x->ra, &got, &buf) || got != blk ||
			(BufferIsValid(buf) && BufferGetBlockNumber(buf) != blk))
			elog(ERROR, "lionvec: the read stream of \"%s\" lost its place at block %u",
				 RelationGetRelationName(x->rel), blk);
	}
	else if (x->ask != NULL)
	{
		for (; x->asked < x->n && x->asked <= x->pos + x->dist; x->asked++)
			if (x->asked > x->pos)
				lion_ask(x->ask, x->rel, x->blks[x->asked], true);
	}
	x->pos++;
	if (!BufferIsValid(buf))
		buf = ReadBuffer(x->rel, blk);
	return buf;
}

/* Done with the list, read whole or not: its stream's pins released. */
static void
lv_list_end(LvExactRead *x, LionReadaheadCounts *counts)
{
	if (x->ra != NULL)
	{
		lion_readahead_counts(x->ra, counts);
		lion_readahead_end(x->ra);
		x->ra = NULL;
	}
}

/* how lv_exact_places() reads a LOC page's entries */
#define LV_LOC_SORTED	1		/* the build's, in TID order: searched */
#define LV_LOC_WALK		2		/* an insert's: read whole */

/* The first member of so->exact[0 .. n), in TID order, at tid or past it. */
static int
lv_exact_lower(const LvScanOpaque *so, int n, ItemPointer tid)
{
	int			a = 0;
	int			b = n;

	while (a < b)
	{
		int			m = a + (b - a) / 2;

		if (ItemPointerCompare((ItemPointer) &so->exact[m].tid, tid) < 0)
			a = m + 1;
		else
			b = m;
	}
	return a;
}

/* The member at tid; -1 if none. */
static int
lv_exact_find(const LvScanOpaque *so, int n, ItemPointer tid)
{
	int			m = lv_exact_lower(so, n, tid);

	return m < n && ItemPointerEquals(&so->exact[m].tid, tid) ? m : -1;
}

/*
 * A LOC entry for member m: the member's, copied, unless another entry
 * already named it differently - then neither.
 */
static void
lv_exact_note(const LionVecLoc **ent, bool *twice, int m,
			  const LionVecLoc *loc)
{
	Size		len = LIONVEC_LOC_SIZE(loc->nruns);

	if (twice[m])
		return;
	if (ent[m] == NULL)
	{
		LionVecLoc *copy = palloc(len);

		memcpy(copy, loc, len);
		ent[m] = copy;
	}
	else if (ent[m]->nruns != loc->nruns || memcmp(ent[m], loc, len) != 0)
	{
		ent[m] = NULL;
		twice[m] = true;
	}
}

/*
 * The entries of a LOC page the build wrote, in TID order: each member
 * between its first entry's TID and its last's looked up by binary search,
 * and the entries at its TID noted, so that a page is read for its few
 * members, not whole.  An entry out of order, which only corruption
 * leaves, can hide one; that member is read from the heap.
 */
static void
lv_exact_page_sorted(Relation index, const LvScanOpaque *so, int n,
					 Page page, BlockNumber blk, const LionVecLoc **ent,
					 bool *twice)
{
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	ItemPointerData last;
	int			m;

	if (maxoff < FirstOffsetNumber)
		return;
	last = lionvec_loc_at(index, page, blk, maxoff, so->tnchunks)->tid;
	m = lv_exact_lower(so, n,
					   &lionvec_loc_at(index, page, blk, FirstOffsetNumber,
									   so->tnchunks)->tid);
	for (; m < n && ItemPointerCompare(&so->exact[m].tid, &last) <= 0; m++)
	{
		OffsetNumber a = FirstOffsetNumber;
		OffsetNumber b = maxoff + 1;

		while (a < b)
		{
			OffsetNumber mid = a + (b - a) / 2;
			LionVecLoc *loc = lionvec_loc_at(index, page, blk, mid,
											 so->tnchunks);

			if (ItemPointerCompare(&loc->tid, &so->exact[m].tid) < 0)
				a = mid + 1;
			else
				b = mid;
		}
		for (; a <= maxoff; a++)
		{
			LionVecLoc *loc = lionvec_loc_at(index, page, blk, a,
											 so->tnchunks);

			if (!ItemPointerEquals(&loc->tid, &so->exact[m].tid))
				break;
			lv_exact_note(ent, twice, m, loc);
		}
	}
}

/* The place of blk in sel[0 .. n), sorted; -1 if none. */
static int
lv_block_find(const BlockNumber *sel, int n, BlockNumber blk)
{
	int			a = 0;
	int			b = n;

	while (a < b)
	{
		int			m = a + (b - a) / 2;

		if (sel[m] < blk)
			a = m + 1;
		else
			b = m;
	}
	return a < n && sel[a] == blk ? a : -1;
}

/* the members read by place, by their first chunk's place */
typedef struct LvPlaceOrd
{
	BlockNumber blk;
	OffsetNumber off;
	int			member;
	const LionVecLoc *loc;
} LvPlaceOrd;

static int
lv_place_ord_cmp(const void *a, const void *b)
{
	const LvPlaceOrd *x = (const LvPlaceOrd *) a;
	const LvPlaceOrd *y = (const LvPlaceOrd *) b;

	if (x->blk != y->blk)
		return x->blk < y->blk ? -1 : 1;
	if (x->off != y->off)
		return x->off < y->off ? -1 : 1;
	return x->member < y->member ? -1 : (x->member > y->member ? 1 : 0);
}

/*
 * The exact path's members read by their places in TOAST (DESIGN.md §44.13,
 * "The exact path"), so->exact[0 .. n) in TID order.  The LOCDIR whole,
 * and of its LOC pages those whose range of heap blocks holds a member's
 * (lionvec_loc_select()), in block order through a list; a page of the
 * build's searched for its members (lv_exact_page_sorted()), an insert's
 * read whole, each entry looked up among the members by TID; a member that
 * two entries name differently takes neither.  Then the members with an
 * entry, by their first chunk's place, each run's block named in turn to
 * one list over the TOAST relation (a block repeated in a row once), each
 * member's chunks read from their pages and checked (lionvec_place_run()), its
 * vector assembled and its header checked (lv_value_ok()), and its
 * distance computed: the member is then LV_X_PLACED, with its value's id
 * for the check before it is returned (lv_check_placed()), or LV_X_DROP, a
 * cosine zero vector, which the zero walk returns.  Any other member stays
 * LV_X_HEAP, for the heap; so does every one past the point where the scan
 * stops reading by place (lv_place_ok()).  The lists end before it
 * returns, so that no page stays pinned.
 */
static void
lv_exact_places(IndexScanDesc scan, LvScanOpaque *so, int n)
{
	Relation	index = scan->indexRelation;
	Relation	toastrel = so->toastrel;
	MemoryContext cxt;
	MemoryContext old;
	LionVecLocDir *dir;
	int			ndir;
	uint32	   *mblks;
	int			nm = 0;
	uint32	   *dpage;
	uint32	   *dlo;
	uint32	   *dhi;
	BlockNumber *sel;
	int			nsel;
	uint8	   *how;			/* [nsel]: LV_LOC_SORTED or LV_LOC_WALK */
	int			nbuilt;
	const LionVecLoc **ent;		/* [n]: a member's entry, or NULL */
	bool	   *twice;			/* [n]: named by two entries that differ */
	LvPlaceOrd *ord;
	int			nord = 0;
	BlockNumber *tblks;
	int			nt = 0;
	LvExactRead x;
	Buffer		cur = InvalidBuffer;
	BlockNumber curblk = InvalidBlockNumber;
	int			i;

	Assert(so->tdirect && toastrel != NULL);
	cxt = AllocSetContextCreate(CurrentMemoryContext, "lionvec places",
								ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cxt);

	/* the members' heap blocks, in order, each once */
	mblks = palloc(sizeof(uint32) * n);
	for (i = 0; i < n; i++)
	{
		BlockNumber blk = ItemPointerGetBlockNumberNoCheck(&so->exact[i].tid);

		Assert(i == 0 || ItemPointerCompare(&so->exact[i - 1].tid,
											&so->exact[i].tid) < 0);
		if (nm == 0 || mblks[nm - 1] != blk)
			mblks[nm++] = blk;
	}

	/* the LOC pages that may hold their entries */
	dir = lionvec_locdir_read(index, &so->meta, &ndir);
	dpage = palloc(sizeof(uint32) * Max(ndir, 1));
	dlo = palloc(sizeof(uint32) * Max(ndir, 1));
	dhi = palloc(sizeof(uint32) * Max(ndir, 1));
	sel = palloc(sizeof(BlockNumber) * Max(ndir, 1));
	for (i = 0; i < ndir; i++)
	{
		dpage[i] = dir[i].blk;
		dlo[i] = dir[i].lo;
		dhi[i] = dir[i].hi;
	}
	nsel = lionvec_loc_select(dpage, dlo, dhi, ndir, mblks, nm, sel);
	for (i = 0; i < nsel; i++)
	{
		if (sel[i] >= so->nblocks)
			so->nblocks = RelationGetNumberOfBlocks(index);
		if (sel[i] >= so->nblocks || sel[i] == LIONVEC_METAPAGE)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("lionvec index \"%s\" has a locator page at block %u, past its end",
							RelationGetRelationName(index), sel[i])));
	}

	/*
	 * Which of them only the build's LOCDIR items name, their entries in TID
	 * order: those the scan searches (LV_LOC_SORTED); a page an insert's
	 * item names too, it reads whole (LV_LOC_WALK).
	 */
	how = palloc0(sizeof(uint8) * Max(nsel, 1));
	nbuilt = (int) Min(so->meta.loc_built, (uint32) ndir);
	for (i = 0; i < ndir; i++)
	{
		int			k = lv_block_find(sel, nsel, dir[i].blk);

		if (k >= 0)
			how[k] = i < nbuilt && how[k] != LV_LOC_WALK ?
				LV_LOC_SORTED : LV_LOC_WALK;
	}

	/* their entries, by member */
	ent = palloc0(sizeof(LionVecLoc *) * n);
	twice = palloc0(sizeof(bool) * n);
	lv_list_begin(&x, index, sel, nsel,
				  so->ipf > 0 ? so->rmode : LION_READAHEAD_OFF, so->ipf,
				  &so->iask);
	for (i = 0; i < nsel; i++)
	{
		Buffer		buf;
		Page		page;
		OffsetNumber off;
		OffsetNumber maxoff;

		CHECK_FOR_INTERRUPTS();
		buf = lv_list_read(&x, sel[i]);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		lionvec_check_page(index, page, sel[i], LIONVEC_PAGE_LOC);
		so->nlocpages++;
		if (how[i] == LV_LOC_SORTED)
			lv_exact_page_sorted(index, so, n, page, sel[i], ent, twice);
		else
		{
			maxoff = PageGetMaxOffsetNumber(page);
			for (off = FirstOffsetNumber; off <= maxoff;
				 off = OffsetNumberNext(off))
			{
				LionVecLoc *loc = lionvec_loc_at(index, page, sel[i], off,
												 so->tnchunks);
				int			m = lv_exact_find(so, n, &loc->tid);

				if (m >= 0)
					lv_exact_note(ent, twice, m, loc);
			}
		}
		UnlockReleaseBuffer(buf);
	}
	lv_list_end(&x, &so->rcounts);
	/* the entries read, and no chunk yet: a test parks the scan here */
	LION_INJECTION_POINT("lionvec-exact-located");

	/* the members with an entry, by their first chunk's place */
	ord = palloc(sizeof(LvPlaceOrd) * Max(n, 1));
	for (i = 0; i < n; i++)
	{
		const LionVecLoc *loc = ent[i];
		bool		inside = loc != NULL;
		int			r;

		for (r = 0; inside && r < loc->nruns; r++)
			inside = loc->runs[r].blk < so->tnblocks;
		if (!inside)
			continue;			/* a place past the end: the heap's */
		ord[nord].blk = loc->runs[0].blk;
		ord[nord].off = loc->runs[0].off;
		ord[nord].member = i;
		ord[nord].loc = loc;
		nord++;
	}
	qsort(ord, nord, sizeof(LvPlaceOrd), lv_place_ord_cmp);
	tblks = palloc(sizeof(BlockNumber) * Max(nord * so->tnchunks, 1));
	for (i = 0; i < nord; i++)
	{
		int			r;

		for (r = 0; r < ord[i].loc->nruns; r++)
		{
			BlockNumber blk = ord[i].loc->runs[r].blk;

			if (nt == 0 || tblks[nt - 1] != blk)
				tblks[nt++] = blk;
		}
	}

	/* their chunks, from their pages */
	lv_list_begin(&x, toastrel, tblks, nt,
				  so->tpf > 0 ? so->rmode : LION_READAHEAD_OFF, so->tpf,
				  &so->task);
	for (i = 0; i < nord && lv_place_ok(so); i++)
	{
		const LionVecLoc *loc = ord[i].loc;
		LvExact    *e = &so->exact[ord[i].member];
		bool		ok = true;
		int			seq = 0;
		int			r;
		double		dist;

		CHECK_FOR_INTERRUPTS();
		for (r = 0; r < loc->nruns; r++)
		{
			const LionVecLocRun *run = &loc->runs[r];

			/* every block named is read, the member's checks failed or not */
			if (run->blk != curblk)
			{
				if (BufferIsValid(cur))
					ReleaseBuffer(cur);
				cur = lv_list_read(&x, run->blk);
				curblk = run->blk;
			}
			if (ok)
			{
				LockBuffer(cur, BUFFER_LOCK_SHARE);
				ok = lionvec_place_run(toastrel, so->tsnap, cur, run,
									   loc->valueid, seq, so->textsize,
									   VARDATA(so->tvalue));
				LockBuffer(cur, BUFFER_LOCK_UNLOCK);
			}
			seq += run->n;
		}
		so->ttried++;
		if (!ok || seq != so->tnchunks || !lv_value_ok(so))
		{
			so->tfailed++;
			so->nplacefail++;
			continue;
		}
		so->nplaced++;
		dist = lv_value_distance(scan, so);
		if (isnan(dist) && so->metric == LIONVEC_COSINE)
		{
			e->how = LV_X_DROP;
			continue;
		}
		e->how = LV_X_PLACED;
		e->dist = dist;
		e->valueid = loc->valueid;
	}
	if (BufferIsValid(cur))
		ReleaseBuffer(cur);
	lv_list_end(&x, &so->rcounts);

	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
}

/*
 * The members' distances, sorted: what probing would cost, and more, when
 * a restriction admits so few rows that most partitions hold none, and the
 * nearest member lies past many.  A member that is not visible, or whose
 * vector is NULL, is left for the NULL walk; a cosine zero vector, whose
 * distance is NaN, for the zero walk.  The members are listed first, in
 * TID order.  Those whose vectors lie in TOAST are read by their places
 * there when the scan does (lv_exact_places()), and the rest from the
 * heap, their heap blocks read ahead (DESIGN.md §44.4, "Prefetch" and
 * "Read streams"): asking the kernel, each one's so->hpf members before
 * its fetch, the members of a block asking once; through a stream over the
 * list of their blocks, each block taken from the stream at its first
 * member, pinned unless the list rests (lion_readahead_next()), and its
 * pin dropped at once - the fetch pins it again, and prunes it only with
 * no other pin of this backend on it (ConditionalLockBufferForCleanup()) -
 * and the stream ended before the scan returns, so that it keeps no heap
 * page pinned.  Then their distances, over the list.
 */
static void
lv_exact(IndexScanDesc scan, LvScanOpaque *so)
{
	uint16	   *los = palloc(sizeof(uint16) * LION_CONTAINER_RANGE);
	int			max = 64;
	int			n = 0;
	int			i;
	int			k;
	LvExactRead x;
	BlockNumber last = InvalidBlockNumber;
	int			xb = -1;		/* the member's block's place in x.blks */

	so->exact = palloc(sizeof(LvExact) * max);
	so->nexact = 0;
	for (k = 0; k < so->rn; k++)
	{
		uint32		m;
		uint32		j;

		Assert(so->rconts[k] != NULL);
		CHECK_FOR_INTERRUPTS();
		m = lion_container_to_array(so->rconts[k], los);
		for (j = 0; j < m; j++)
		{
			uint64		code = lion_make_code(so->rkeys[k], los[j]);
			ItemPointerData tid;

			if ((code >> LION_OFFSET_BITS) > MaxBlockNumber)
				break;
			lion_code_to_tid(code, &tid);
			if (!OffsetNumberIsValid(ItemPointerGetOffsetNumberNoCheck(&tid)))
				continue;
			if (n >= max)
			{
				if ((Size) max > MaxAllocSize / sizeof(LvExact) / 2)
					elog(ERROR, "lionvec: too many members");
				max *= 2;
				so->exact = repalloc(so->exact, sizeof(LvExact) * max);
			}
			so->exact[n].dist = 0.0;
			so->exact[n].tid = tid;
			so->exact[n].how = LV_X_HEAP;
			so->exact[n].valueid = InvalidOid;
			n++;
		}
	}
	pfree(los);

	if (so->tdirect && n > 0)
		lv_exact_places(scan, so, n);

	memset(&x, 0, sizeof(x));
	if (so->hpf > 0 && so->rmode == LION_READAHEAD_STREAM)
	{
		x.blks = palloc(sizeof(BlockNumber) * Max(n, 1));
		for (i = 0; i < n; i++)
		{
			BlockNumber blk = ItemPointerGetBlockNumberNoCheck(&so->exact[i].tid);

			if (so->exact[i].how != LV_X_HEAP)
				continue;
			if (blk != last && blk < so->hblocks)
			{
				/*
				 * in TID order, each block once: the stream never holds a
				 * second pin of the block the fetch prunes
				 */
				Assert(x.n == 0 || blk > x.blks[x.n - 1]);
				x.blks[x.n++] = blk;
			}
			last = blk;
		}
		x.ra = lion_readahead_begin_list(scan->heapRelation,
										 LION_READAHEAD_STREAM,
										 lv_exact_next, &x);
		Assert(x.ra != NULL);
		last = InvalidBlockNumber;
	}
	else
	{
		for (i = 0; i < Min(n, so->hpf); i++)
			if (so->exact[i].how == LV_X_HEAP)
				lv_prefetch_heap(scan, so, &so->exact[i].tid);
	}
	for (i = 0; i < n; i++)
	{
		ItemPointerData tid = so->exact[i].tid;
		double		dist;

		CHECK_FOR_INTERRUPTS();
		if (so->exact[i].how == LV_X_DROP)
			continue;
		if (so->exact[i].how == LV_X_PLACED)
		{
			/* kept in place, its check due when it is returned */
			so->exact[so->nexact++] = so->exact[i];
			continue;
		}
		if (x.ra != NULL)
		{
			BlockNumber blk = ItemPointerGetBlockNumberNoCheck(&tid);

			if (blk != last && blk < so->hblocks)
			{
				BlockNumber got;
				Buffer		buf;

				/*
				 * the list's next, which the stream returns in order, pinned
				 * unless the list rests
				 */
				xb++;
				if (!lion_readahead_next(x.ra, &got, &buf) || xb >= x.n ||
					x.blks[xb] != blk || got != blk ||
					(BufferIsValid(buf) && BufferGetBlockNumber(buf) != blk))
					elog(ERROR, "lionvec: the read stream of \"%s\" lost its place at block %u",
						 RelationGetRelationName(scan->heapRelation), blk);
				if (BufferIsValid(buf))
					ReleaseBuffer(buf);
			}
			last = blk;
		}
		else if (so->hpf > 0 && i + so->hpf < n &&
				 so->exact[i + so->hpf].how == LV_X_HEAP)
			lv_prefetch_heap(scan, so, &so->exact[i + so->hpf].tid);
		if (!lv_fetch_distance(scan, so, &tid, &dist))
			continue;
		if (isnan(dist) && so->metric == LIONVEC_COSINE)
			continue;
		/* kept in place, behind the members still to fetch */
		so->exact[so->nexact].dist = dist;
		so->exact[so->nexact].tid = tid;
		so->exact[so->nexact].how = LV_X_HEAP;
		so->exact[so->nexact].valueid = InvalidOid;
		so->nexact++;
	}
	if (x.ra != NULL)
	{
		/* every block read: the stream holds nothing it read ahead */
		Assert(xb == x.n - 1 && x.next == x.n);
		Assert(lion_readahead_depth(x.ra) == 0);
		lion_readahead_counts(x.ra, &so->rcounts);
		lion_readahead_end(x.ra);
		pfree(x.blks);
	}
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
	{
		so->wheads = lionvec_chain_heads(scan->indexRelation, &so->meta,
										 &so->nwheads);
		/* the partitions', then REST and NULL: the claims' bits (LvShared) */
		if (so->nwheads != (int) so->meta.nparts + 2)
			elog(ERROR, "lionvec: index \"%s\" has %d chains for %u partitions",
				 RelationGetRelationName(scan->indexRelation), so->nwheads,
				 so->meta.nparts);
	}
	so->wchain = 0;
	so->wblk = InvalidBlockNumber;
	so->wvisited = 0;
	so->wn = so->wpos = 0;
	MemoryContextSwitchTo(old);
}

/*
 * Whether this process walks chain c of the walk (a parallel scan's claim,
 * LvShared).  The NULL chain is claimed once a scan, by the first walk to
 * reach it - the zero walk, or one of every row - and the NULL walk after
 * it is that process's too, so that its zero vectors and its NULLs come
 * from one process, each once.
 */
static bool
lv_walk_claim(LvScanOpaque *so, int c)
{
	uint32		nullbit = so->meta.nparts + 1;

	if (so->shared == NULL)
		return true;
	if (so->walk != LV_WALK_ZERO && so->walk != LV_WALK_NULL &&
		(uint32) c != nullbit)
		return lv_claim(so, (uint32) c);
	if (!so->nullasked)
	{
		so->nullmine = lv_claim(so, nullbit);
		so->nullasked = true;
	}
	return so->nullmine;
}

/*
 * What follows the queue, or a walk: the zero vectors, the NULLs, the end.
 * What the streams read ahead for the phase that ends is released.
 */
static void
lv_next_phase(IndexScanDesc scan, LvScanOpaque *so)
{
	bool		ordered = so->phase == LV_NORMAL || so->phase == LV_LIST;

	lv_readahead_reset(so);

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
			int			c;

			if (so->wchain >= so->nwheads)
				return false;
			c = so->wchain++;
			if (!lv_walk_claim(so, c))
				continue;		/* another process walks it */
			so->wblk = so->wheads[c];
			/* a chain the build wrote runs on in consecutive blocks */
			so->wpfnext = BlockNumberIsValid(so->wblk) ?
				lv_run_start(so, so->wblk) : 0;
			so->wpfend = so->nblocks;
			continue;
		}
		CHECK_FOR_INTERRUPTS();
		lionvec_chain_step(index, &so->wvisited, &so->nblocks);
		if (so->ipf > 0)
			lv_prefetch_run(so, &so->wpfnext,
							lionvec_prefetch_end(so->wblk, so->ipf, so->wpfend));
		buf = lion_readahead_read(so->chains, so->wblk);
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
		/* off the run: what follows was appended, and lies anywhere */
		if (op->next != so->wblk + 1)
			so->wpfend = 0;
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
 * §44.4), and each one's CODE chain into phead, with the items the build
 * put in it into pnrows.  The PART chain is a run of blocks the build wrote
 * last, which is prefetched ahead of the read as a probe's chain is; and
 * then the REFINE pages of the centroids the queue will refine first.
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
	uint64		pfnext = lv_run_start(so, blk);
	uint64		pfend = (uint64) blk +
		lionvec_chain_pages((uint32) k, lionvec_page_items(LIONVEC_PART_SIZE(dimsp)));

	so->phead = palloc(sizeof(BlockNumber) * Max(k, 1));
	so->pnrows = palloc(sizeof(uint32) * Max(k, 1));
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
		if (so->ipf > 0)
			lv_prefetch_run(so, &pfnext,
							lionvec_prefetch_end(blk, so->ipf, pfend));
		buf = lion_readahead_read(so->chains, blk);
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
			so->pnrows[n] = part->nrows;
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
			item.u.xm = part->cent.xm;
			so->parts.items[so->parts.n++] = item;
			n++;
		}
		if (LionVecPageGetOpaque(page)->next != blk + 1)
			pfend = 0;			/* off the build's run */
		blk = LionVecPageGetOpaque(page)->next;
		UnlockReleaseBuffer(buf);
	}
	if (n != k)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has %d partitions, and its meta page says %d",
						RelationGetRelationName(index), n, k)));
	lv_heapify(&so->parts);
	/* the PART chain is warm whatever else is: it says nothing (LionAsk) */
	so->iask.warm = 0;
	so->iask.dormant = 0;
	lion_readahead_awaken(so->chains);
	so->cpfdue = 1;
	lv_prefetch_due(scan, so, &so->cpfdue, &so->parts, LV_CENT1, false);
}

/*
 * A partition's centroid by its 8-bit codes, back into the queue; first,
 * every so often, the REFINE pages of the centroids to refine next.
 */
static void
lv_refine_part(IndexScanDesc scan, LvScanOpaque *so, LvItem *item)
{
	Relation	index = scan->indexRelation;
	Buffer		buf;
	Page		page;
	LionVecRefine *ref;
	double		est;
	double		err;
	double		lo;
	double		hi;

	lv_prefetch_due(scan, so, &so->cpfdue, &so->parts, LV_CENT1, false);
	buf = lion_readahead_read(so->crefs, item->rblk);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	lionvec_check_page(index, page, item->rblk, LIONVEC_PAGE_REFINE);
	ref = lionvec_refine_at(index, page, item->rblk, item->roff,
							so->info->dimsp, so->meta.refloc_runs, &item->tid);
	if (ref == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has lost partition %u's centroid",
						RelationGetRelationName(index),
						LionVecCentPartno(&item->tid))));
	lionvec_estimate8(&so->q, ref->codes, item->norm, ref->alpha8,
					  ref->rnorm8, &est, &err);
	UnlockReleaseBuffer(buf);
	lionvec_key_bounds(so->metric, item->norm, item->u.xm, est, err, &lo, &hi);
	item->key = lv_part_key(so, lo, LionVecCentPartno(&item->tid));
	item->level = LV_CENT8;
	item->pf = false;
	lv_heap_push(&so->parts, item);
}

/* ----------------------------------------------------------------
 * Probes: a chain's rows into the row queue
 * ----------------------------------------------------------------
 */

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
	lv_readahead_reset(so);
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
 *
 * pages is how many blocks from head the build wrote the chain in, one
 * after another (lionvec_chain_pages() of its PART item's count), or 0 when
 * not known (the REST chain): each page read asks for the next so->ipf of
 * them first, until a page's next is not the block after it - the rest of
 * the chain was appended later, a page here and there (DESIGN.md §44.4,
 * "Prefetch").  And once the chain is read, the REFINE pages of the rows
 * the queue will refine first.
 */
static void
lv_probe(IndexScanDesc scan, LvScanOpaque *so, BlockNumber head, bool copies,
		 uint32 pages)
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
	uint64		pfnext = lv_run_start(so, head);
	uint64		pfend = BlockNumberIsValid(head) ? (uint64) head + pages : 0;

	while (BlockNumberIsValid(blk))
	{
		Buffer		buf;
		Page		page;
		LionVecPageOpaque op;
		OffsetNumber off;
		OffsetNumber maxoff;

		CHECK_FOR_INTERRUPTS();
		lionvec_chain_step(index, &visited, &so->nblocks);
		if (so->ipf > 0 && pfend > 0)
			lv_prefetch_run(so, &pfnext,
							lionvec_prefetch_end(blk, so->ipf, pfend));
		buf = lion_readahead_read(so->chains, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		lionvec_check_page(index, page, blk, LIONVEC_PAGE_CODE);
		op = LionVecPageGetOpaque(page);
		so->npages++;
		if (op->next != blk + 1)
			pfend = 0;			/* off the build's run */
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
			item.u.xm = code->xm;
			item.dist = 0.0;
			item.level = LV_PLANE1;
			item.copy = (code->flags & LIONVEC_SPILL) != 0;
			item.pf = false;
			item.tp = false;
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
					if (item.level == LV_CODES8 && so->tref > 0)
					{
						/* its REFINE item may place it; its block unknown */
						item.tp = true;
						item.u.tblk = InvalidBlockNumber;
					}
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
		/* between two pages of a chain, holding no lock (§44.5) */
		if (BlockNumberIsValid(blk))
			LION_INJECTION_POINT("lionvec-probe-page");
	}

	/* every row left out lies past this */
	if (drop < INFINITY)
		lv_push_rest(so, drop);
	so->rpfdue = 1;
	lv_prefetch_due(scan, so, &so->rpfdue, &so->rows, LV_PLANE1, false);
}

/*
 * Is a pause due (the file's header)?  The tier of the next row - the
 * least K past the rows the pass has returned, or L - is full and has
 * taken no row in pg_lion.vector_patience units of rows.  Sets pausetier.
 * The tiers are those lv_reset_rows() made, ascending: under a LIMIT the
 * least may be past LV_TIER_MIN, and under one of L or more there are
 * none, and the tier is L's.
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

/*
 * Probe a partition from the queue, and remember it for a REST's - in a
 * parallel scan only if this process claims it first, another's being
 * that one's to probe and return the rows of (DESIGN.md §44.4, "A parallel
 * scan").
 */
static void
lv_probe_part(IndexScanDesc scan, LvScanOpaque *so, LvItem *item)
{
	BlockNumber partno = LionVecCentPartno(&item->tid);

	Assert(partno < so->meta.nparts);
	if (!lv_claim(so, partno))
	{
		so->nclaimed++;
		return;
	}
	so->probed[so->nprobed++] = partno;
	so->nprobes++;
	lv_probe(scan, so, so->phead[partno], so->spill,
			 lionvec_chain_pages(so->pnrows[partno], so->perpage));
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

	/*
	 * The tiers below L, and none below the rows the query takes when the
	 * planner knew them (so->limit; the file's header): a pause on such a
	 * tier would end by count before the query had its rows.  The least
	 * stays LV_TIER_MIN, so a LIMIT of 1 to 10 keeps every tier.  The
	 * limit is the scan's, so the first tier's K, and with it every
	 * tier's, is the same in every batch, and a tier's heap, allocated by
	 * the first, holds its K in each.  (k stays below L, an int, so it
	 * cannot overflow on the way to a limit past every tier.)
	 */
	so->ntiers = 0;
	for (k = LV_TIER_MIN; k < so->batch && so->ntiers < LV_MAX_TIERS; k *= 2)
	{
		LvTier	   *tier = &so->tiers[so->ntiers];

		if (so->limit > 0 && k < so->limit)
			continue;
		if (so->ntiers >= so->ntieralloc)
		{
			tier->h = MemoryContextAlloc(so->qcxt, sizeof(double) * k);
			so->ntieralloc++;
		}
		else if (tier->k != (int) k)
			elog(ERROR, "lionvec: tier %d is of %d rows and its heap of %d",
				 so->ntiers, (int) k, tier->k);
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

	/*
	 * the chains this process probed: the REST chain's rows have no copies;
	 * a stream reads the chains after each one ahead (lv_chains_refill())
	 */
	so->inrest = true;
	so->restpf = 0;
	for (i = 0; i < so->nprobed; i++)
	{
		uint32		p = so->probed[i];

		so->restpf = Max(so->restpf, i + 1);
		if (p == LV_REST_CHAIN)
			lv_probe(scan, so, so->meta.rest_head, false, 0);
		else
			lv_probe(scan, so, so->phead[p], so->spill,
					 lionvec_chain_pages(so->pnrows[p], so->perpage));
	}
	so->inrest = false;
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

/*
 * A row by its 8-bit codes, back into the queue; first, every so often, the
 * REFINE pages of the rows to refine next (DESIGN.md §44.4, "Prefetch").
 */
static void
lv_refine(IndexScanDesc scan, LvScanOpaque *so, LvItem *item)
{
	Relation	index = scan->indexRelation;
	Buffer		buf;
	Page		page;
	LionVecRefine *ref;
	double		est;
	double		err;
	double		lo;
	double		hi;
	BlockNumber tblk = InvalidBlockNumber;
	bool		tp = false;

	lv_prefetch_due(scan, so, &so->rpfdue, &so->rows, LV_PLANE1, false);
	buf = lion_readahead_read(so->rrefs, item->rblk);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	lionvec_check_page(index, page, item->rblk, LIONVEC_PAGE_REFINE);
	ref = lionvec_refine_at(index, page, item->rblk, item->roff,
							so->info->dimsp, so->meta.refloc_runs, &item->tid);
	if (ref == NULL)
	{
		/* VACUUM has removed the row since the probe read it (§44.5) */
		UnlockReleaseBuffer(buf);
		lv_mark_done(so, item);
		return;
	}
	lionvec_estimate8(&so->q, ref->codes, item->norm, ref->alpha8,
					  ref->rnorm8, &est, &err);
	if (so->tref > 0 && ref->nloc > 0)
	{
		/* its vector's place in TOAST, whose first page the rerank asks for */
		tp = true;
		tblk = LionVecRefineGetLoc(ref, so->info->dimsp)->runs[0].blk;
	}
	UnlockReleaseBuffer(buf);
	so->nrefined++;

	/*
	 * The codes' bound replaces plane 1's rather than tightening it: it is
	 * the better by two orders of magnitude, and keeping the larger of the
	 * two would keep plane 1's failures too.
	 */
	lionvec_key_bounds(so->metric, item->norm, item->u.xm, est, err, &lo, &hi);
	item->key = lo;
	item->level = LV_CODES8;
	item->pf = false;
	item->tp = tp;
	item->u.tblk = tblk;		/* plane 1's xm is done with */
	lv_remember(so, &item->tid, LV_CODES8, lo, hi, 0.0, item->hi1);
	if (so->behind && hi < so->lastkey)
		return;					/* behind the row returned last; the hash says so */
	lv_heap_push(&so->rows, item);
}

/*
 * A row placed by its REFINE item (tp), its exact distance read by place
 * (DESIGN.md §44.13, "The REFINE item's place"): 1 with *dist, 0 if no
 * version of it is visible to the scan or its vector is NULL, -1 to read
 * it from the heap instead - no place in its REFINE item, the item gone
 * (VACUUM, §44.5), or the scan no longer reading by place.  Its REFINE
 * item, read a moment ago by lv_refine(), is read again for the place; the
 * row is fetched from the heap first, for the check that comes before an
 * exact path's member is returned (lv_check_placed()) - a row the scan
 * does not see costs no TOAST read - and then its vector read from its
 * chunks' pages, which lv_prefetch_queue() asked for with its heap block,
 * and checked, where a failure detoasts the fetched row's vector as the
 * heap path does.
 */
static int
lv_rerank_placed(IndexScanDesc scan, LvScanOpaque *so, LvItem *item,
				 double *dist)
{
	Relation	index = scan->indexRelation;
	int			dimsp = so->info->dimsp;
	Buffer		buf;
	Page		page;
	LionVecRefine *ref;
	LionVecLocRun runs[LIONVEC_LOC_MAXRUNS];
	int			nruns = 0;
	Oid			valueid = InvalidOid;
	bool		ok;

	if (!lv_place_ok(so) || item->rblk >= so->nblocks)
		return -1;
	buf = ReadBuffer(index, item->rblk);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	lionvec_check_page(index, page, item->rblk, LIONVEC_PAGE_REFINE);
	ref = lionvec_refine_at(index, page, item->rblk, item->roff, dimsp,
							so->meta.refloc_runs, &item->tid);
	if (ref != NULL && ref->nloc > 0 && ref->nloc <= LIONVEC_LOC_MAXRUNS)
	{
		LionVecRefineLoc *rl = LionVecRefineGetLoc(ref, dimsp);

		nruns = ref->nloc;
		valueid = rl->valueid;
		memcpy(runs, rl->runs, sizeof(LionVecLocRun) * nruns);
	}
	UnlockReleaseBuffer(buf);
	if (nruns == 0)
		return -1;

	so->nchecked++;
	so->nfetched++;
	if (!lv_heap_fetch(scan, so, &item->tid))
		return 0;
	lv_check_pointer(scan, so, valueid);
	ok = lionvec_place_read(so->toastrel, so->tsnap, so->tnblocks, valueid,
							runs, nruns, so->textsize, VARDATA(so->tvalue)) &&
		lv_value_ok(so);
	so->ttried++;
	if (!ok)
	{
		so->tfailed++;
		so->nplacefail++;
		return lv_slot_distance(scan, so, dist) ? 1 : 0;
	}
	ExecClearTuple(so->slot);
	so->nplaced++;
	*dist = lv_value_distance(scan, so);
	return 1;
}

/*
 * A row by its exact distance, back into the queue; first, every so often,
 * the heap blocks of the rows to fetch next, and the TOAST blocks of those
 * placed.
 */
static void
lv_rerank(IndexScanDesc scan, LvScanOpaque *so, LvItem *item)
{
	double		dist;
	int			got = -1;

	lv_prefetch_due(scan, so, &so->fpfdue, &so->rows, LV_CODES8, true);
	if (item->tp && so->tref > 0)
		got = lv_rerank_placed(scan, so, item, &dist);
	if (got < 0)
		got = lv_fetch_distance(scan, so, &item->tid, &dist) ? 1 : 0;
	if (got == 0)
	{
		lv_mark_done(so, item);
		return;
	}
	item->level = LV_EXACT;
	item->pf = false;
	item->tp = false;
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

/*
 * The index's three readaheads (lion_readahead.h), as the scan reads now:
 * mode, or plain reads where it reads nothing ahead (so->ipf of 0), and as
 * many pins each.  They are the scan's, in its own context, kept from one
 * rescan to the next while the mode and pins stay, so that a nested loop's
 * inner scan does not begin three read streams a row; lv_readahead_reset()
 * leaves each holding nothing.  The heap's stream, the exact path's alone,
 * is lv_exact()'s.
 */
static void
lv_readahead_begin(IndexScanDesc scan, LvScanOpaque *so, int mode, int pins)
{
	Relation	index = scan->indexRelation;
	MemoryContext old;

	if (so->ipf <= 0)
		mode = LION_READAHEAD_OFF;
	if (so->chains != NULL &&
		lion_readahead_mode(so->chains) == mode && so->rpins == pins)
	{
		/* what the last query's reads found says nothing of this one's */
		lion_readahead_awaken(so->chains);
		lion_readahead_awaken(so->crefs);
		lion_readahead_awaken(so->rrefs);
		return;
	}
	lv_readahead_end(so);
	old = MemoryContextSwitchTo(so->cxt);
	so->chains = lion_readahead_begin(index, mode, pins, &so->iask,
									  lv_chains_refill, so);
	so->crefs = lion_readahead_begin(index, mode, pins, &so->iask,
									 lv_crefs_refill, scan);
	so->rrefs = lion_readahead_begin(index, mode, pins, &so->iask,
									 lv_rrefs_refill, scan);
	so->rpins = pins;
	MemoryContextSwitchTo(old);
}

/*
 * How far ahead the scan reads (DESIGN.md §44.4, "Prefetch" and "Read
 * streams"): each relation's tablespace's effective_io_concurrency pages, to
 * LIONVEC_PREFETCH_MAX, while pg_lion.vector_prefetch is not off; the
 * heap's only if it is the heap access method's, whose TIDs name its blocks,
 * and its vectors are not in TOAST (lionvec_vector_inline()).  0 reads nothing
 * ahead.  Through streams, each holds twice its relation's pages ahead in
 * pins, and at least LION_READAHEAD_MIN_PINS - those it reads ahead, and
 * those a read out of their order pulled from it first - to an
 * LV_RA_SHARE'th of the backend's fair share of shared_buffers, reading
 * half as many pages ahead where that is less; with fewer than
 * LION_READAHEAD_MIN_PINS, and on 16, the scan asks the kernel instead.
 */
static void
lv_start_prefetch(IndexScanDesc scan, LvScanOpaque *so)
{
	Relation	index = scan->indexRelation;
	Relation	heap = scan->heapRelation;
	int			pins = 0;

	so->ipf = so->hpf = 0;
	so->perpage = lionvec_page_items(LIONVEC_CODE_SIZE(Max(so->meta.dimsp, 64)));
	lion_ask_reset(&so->iask);
	lion_ask_reset(&so->hask);
	so->inrest = false;
	so->restpf = 0;
	so->rmode = lionvec_prefetch;
	if (so->rmode != LION_READAHEAD_OFF)
	{
		so->ipf = Min(get_tablespace_io_concurrency(index->rd_rel->reltablespace),
					  LIONVEC_PREFETCH_MAX);
		if (heap != NULL && heap->rd_tableam == GetHeapamTableAmRoutine() &&
			lionvec_vector_inline(index, heap, so->info->dims, so->info->half))
		{
			so->hpf = Min(get_tablespace_io_concurrency(heap->rd_rel->reltablespace),
						  LIONVEC_PREFETCH_MAX);
			so->hblocks = so->hpf > 0 ? RelationGetNumberOfBlocks(heap) : 0;
		}
		so->ipf = Max(so->ipf, 0);
		so->hpf = Max(so->hpf, 0);
	}
#if PG_VERSION_NUM < 170000
	if (so->rmode == LION_READAHEAD_STREAM)
		so->rmode = LION_READAHEAD_FADVISE;
#endif
	if (so->rmode == LION_READAHEAD_STREAM)
	{
		pins = (int) Min((uint32) Max(2 * Max(so->ipf, so->hpf),
									  LION_READAHEAD_MIN_PINS),
						 lion_pin_fair_share() / LV_RA_SHARE);
		if (pins < LION_READAHEAD_MIN_PINS)
			so->rmode = LION_READAHEAD_FADVISE;
		else
		{
			so->ipf = Min(so->ipf, pins / 2);
			so->hpf = Min(so->hpf, pins / 2);
		}
	}
	lv_readahead_begin(scan, so, so->rmode, pins);
	if (so->ipf > 0 || so->hpf > 0)
	{
		so->pfkey = palloc(sizeof(double) * LV_PF_TOP(LIONVEC_PREFETCH_MAX));
		so->pfat = palloc(sizeof(int) * LV_PF_TOP(LIONVEC_PREFETCH_MAX));
		so->pfsel = palloc(sizeof(int) * LIONVEC_PREFETCH_MAX);
	}
	so->rpfdue = so->fpfdue = so->cpfdue = 1;
}

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

	/*
	 * Nor at all while PostgreSQL 16's old_snapshot_threshold is set
	 * (DESIGN.md §9, "old_snapshot_threshold"; §44.5).  VACUUM then removes
	 * the items of rows an old snapshot still sees, which core's access
	 * methods notice by testing every page they read (TestForOldSnapshot())
	 * and lionvec does not - nor does its heap fetch, 16's
	 * heap_hot_search_buffer() testing no page - so such a row would be
	 * missing without a word.  The planner prices every lionvec path out
	 * (lionveccostestimate()); a plan with nothing else left stops here,
	 * before the scan reads a page, in each process of a parallel scan.  The
	 * snapshot is not known yet in ambeginscan (index_beginscan() sets it
	 * after), which is why the refusal is in the call that reads.
	 */
	if (lion_old_snapshot_threshold_active())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("lionvec index \"%s\" cannot be read while old_snapshot_threshold is set",
						RelationGetRelationName(index)),
				 errdetail("A lionvec index does not detect \"snapshot too old\", so it could miss rows an old snapshot must see."),
				 errhint("Set old_snapshot_threshold to -1 and restart the server.")));

	so->info = lionvec_get_info(index);
	so->metric = so->info->metric;
	lionvec_read_meta(index, &so->meta);
	so->spill = (so->meta.flags & LIONVEC_META_SPILL) != 0;
	so->nblocks = RelationGetNumberOfBlocks(index);
	so->lastdist = so->lastkey = -INFINITY;
	so->shared = lv_shared(scan);
	Assert(so->shared == NULL || !so->restricted);
	lv_start_prefetch(scan, so);

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
	so->q.qw = palloc(sizeof(int16) * so->info->dimsp);
	lionvec_query_init(&so->q, so->metric, so->info->mean, x, lionvec_epsilon);
	lv_start_toast(scan, so);

	/* how many rows the scan is of */
	tuples = index->rd_rel->reltuples > 0 ? index->rd_rel->reltuples :
		so->meta.ntuples;
	members = tuples;
	if (so->restricted)
	{
		/*
		 * Few members, or few a partition (lionvec_exact_limit(), which the
		 * planner's cost asks of the same meta page's count): every one's
		 * distance from the heap.
		 */
		members = lv_members(so);
		if (members <= lionvec_exact_limit((double) so->meta.nparts))
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

	/*
	 * L: pg_lion.vector_batch, or the rows the query takes when the scan was
	 * told them and they are more, to an eighth of the rows work_mem holds,
	 * where REST stops growing it too (lv_rest()).  A LIMIT past L found its
	 * rows in batches of L, each REST probing every chain again for four
	 * times as many, and paused on L by count: on DBpedia's 115,186 rows a
	 * LIMIT 1000 under a 3% or 10% filter made 2.5 to 2.75 passes and read
	 * 11,800 to 21,800 pages a query for 0.9840 to 0.9998 of its rows at a
	 * patience of 30.  In one pass it reads 5,300 to 7,900 for 0.9863 to
	 * 1.0000, in a half to two thirds of the time, and with the pause off
	 * returns the same rows (DESIGN.md §44.4, "A known LIMIT").  A query
	 * that wants more than it said, a join's or a filter's above, makes a
	 * REST as before.
	 */
	so->batch = lionvec_batch;
	if (so->limit > so->batch)
		so->batch = (int) Min(so->limit,
							  (int64) Max(so->batch, lv_cap(so) / 8));
	lv_reset_rows(so);
	lv_read_parts(scan, so);

	/*
	 * The rows in no partition first, then the partitions as they come; in a
	 * parallel scan, the first process to get here probes the REST chain
	 */
	so->probed = palloc(sizeof(uint32) * ((Size) so->meta.nparts + 1));
	so->npasses++;
	so->pass = 1;
	if (lv_claim(so, so->meta.nparts))
	{
		so->probed[so->nprobed++] = LV_REST_CHAIN;
		lv_probe(scan, so, so->meta.rest_head, false, 0);
	}
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
	{
		/* a scan, once per scan or rescan, as every index AM counts one */
		pgstat_count_index_scan(scan->indexRelation);
		lv_start(scan, so);
	}

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
						if (lv_claimed(so, LionVecCentPartno(&item.tid)))
							so->nclaimed++;	/* another process's: dropped unrefined */
						else if (item.level == LV_CENT1)
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
							if (so->shared != NULL && so->spill &&
								!lv_take_row(so, &item))
							{
								/* another process returned it, from its other item */
								so->ntaken++;
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
				{
					LvExact    *e;

					if (so->epos >= so->nexact)
					{
						lv_next_phase(scan, so);
						break;
					}
					e = &so->exact[so->epos++];
					/* read by place: returned only if the scan sees the row */
					if (e->how == LV_X_PLACED &&
						!lv_check_placed(scan, so, &e->tid, e->valueid))
						break;
					lv_return(scan, so, &e->tid, e->dist);
					return true;
				}

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
