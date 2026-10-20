/*-------------------------------------------------------------------------
 *
 * lion_selfuncs.c
 *		Three corrections to what the planner's own machinery tells lion's
 *		cost model: the endpoint probe - a range bound beyond a column's
 *		histogram, estimated from the column's first or last live key in the
 *		lion directory (DESIGN.md §28, "The endpoint probe"); the intersection
 *		probe - a conjunction of set clauses on one lion index, measured from
 *		the index where core's product of their selectivities may be far off
 *		(DESIGN.md §29.11, "Correlated sets", lion_isect_factor()); and the
 *		part of a plain scan's price that cost_index() cannot charge
 *		(DESIGN.md §29.11, lion_plain_note_remainder()).
 *
 * ANALYZE's histogram ends at the largest value its sample saw.  A column
 * that grows at one end - a timestamp, a serial id - puts every row inserted
 * since past that end, where core estimates an inequality at a hundredth of
 * one bin: `ts >= now() - interval '30 days'` on a table grown by a tenth
 * since its last ANALYZE came out at 330 rows for 148,760.  Core corrects
 * that with get_actual_variable_range(), which reads the column's actual
 * minimum or maximum from an index and puts it in place of the histogram's
 * end before interpolating, whenever the histogram's binary search is about
 * to compare with an end.  It reads only ordered indexes that can return
 * their first column - a btree - so a column indexed by lion alone never gets
 * the correction.
 *
 * Lion gives its OWN cost model the same correction, from its directory,
 * which is sorted by the key within each key column (DESIGN.md §21): the
 * column's first VALUE entry is where lion_dir_value_start() lands and its
 * last is just left of where lion_dir_value_end() lands.  Everything else is
 * core's: whether an end is wanted is decided by core's own binary search,
 * repeated here, and the selectivity is core's clauselist_selectivity() over a
 * copy of the column's pg_statistic row whose histogram ends are the probed
 * keys, handed to it through get_relation_stats_hook while lion prices one of
 * its own paths (lion_probe_begin() .. lion_probe_end()).  Core's estimates
 * for every other path, and the relation's row count, stay core's.
 *
 * Core's safeguards, kept:
 *	- the clause's operator is called on the histogram's values only when
 *	  statistic_proc_security_check() allows it, as core's search asks;
 *	- a descent or two: the probe reads the leaf an end is on and, when the
 *	  entries there have no live row, LION_PROBE_LEAVES leaves at most;
 *	- a live row, not just an entry: a key whose rows are all deleted is not
 *	  an end.  A row is accepted by the visibility map, or by
 *	  SnapshotNonVacuumable, which takes recently dead and uncommitted rows
 *	  as core's probe does, and the probe gives up after
 *	  LION_PROBE_HEAP_PAGES heap pages or LION_PROBE_HEAP_TIDS TIDs without
 *	  one, leaving the histogram's own end in place;
 *	- neither hypothetical nor partial indexes are read, nor the parent of an
 *	  inheritance tree;
 *	- each end is read once per planner run (lion_probe_cache_for()), and an
 *	  end found empty is not read again until the table or the index has
 *	  changed (lion_probe_missed()), which stands in for core's marking the
 *	  dead btree entries it steps over.
 *
 * The same hook, and get_index_stats_hook, carry a third correction, which is
 * every estimate's and not only lion's: a column's n_distinct, taken from the
 * count of its keys a lion index keeps on its meta page, where ANALYZE's
 * sample misses the rare values of a column that has many (DESIGN.md §33,
 * lion_ndistinct_stats()).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/visibilitymap.h"
#include "catalog/pg_class.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_statistic.h"
#include "executor/instrument.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"
#include "parser/parse_coerce.h"
#include "parser/parsetree.h"
#include "storage/bufmgr.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/attoptcache.h"
#include "utils/datum.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "lion.h"
#include "lion_compat.h"
#include "lion_container.h"
#include "lion_count.h"
#include "lion_tid.h"

/*
 * The heap pages the probe visits, looking for a live row under the entries
 * at one end of a column, before it gives up and the histogram's own end
 * stands: core's VISITED_PAGES_LIMIT (get_actual_variable_endpoint()), for
 * the same reason - an end whose rows were all just deleted would otherwise be
 * read row by row at every plan.  A page counts once while the probe stays on
 * it; a page the visibility map vouches for is not visited at all.
 */
#define LION_PROBE_HEAP_PAGES	100

/*
 * The TIDs the probe asks the heap about at one end of a column, however few
 * pages they are on, before it gives up the same way.  Core bounds pages
 * alone because it asks about a TID once: a btree entry whose rows it finds
 * dead is marked so (kill_prior_tuple) and skipped by the next plan.  A
 * posting set marks nothing, so every plan until VACUUM asks about the same
 * dead TIDs again, and LION_PROBE_HEAP_PAGES pages of small rows hold up to
 * 29,100 of them.  10,000 is those pages at 100 rows a page, rows of about
 * 80 bytes: as many as core would read, once, on such a table.
 */
#define LION_PROBE_HEAP_TIDS	10000

/*
 * The directory leaves the probe reads at one end of a column: the one the
 * descent lands on, and the ones beside it while the entries there have no
 * live row - dead rows not yet vacuumed, whose heap pages
 * LION_PROBE_HEAP_PAGES bounds, and leaves VACUUM has emptied, which stay
 * linked (DESIGN.md §18) where a btree would have deleted its pages.  As many
 * as core's heap pages: a leaf read costs about what a heap page read does,
 * and 100 leaves hold 3,400 to 15,000 one-row entries of a column, as inserts
 * or a build filled them.  An end with more than that deleted leaves the
 * histogram's own end in place, as core's probe does when it gives up.
 */
#define LION_PROBE_LEAVES		100

/*
 * The pages lion_dir_step_left() walks right to find a left sibling that has
 * split since its left link was set, before the probe gives up.
 */
#define LION_PROBE_LEFT_STEPS	8

/* ---------------------------------------------------------------------
 * The ends, cached per planner run
 * --------------------------------------------------------------------- */

/* One column's ends as the probe found them: [0] the first key, [1] the last. */
typedef struct LionEndpoint
{
	Oid			relid;			/* the table */
	AttrNumber	attno;			/* its column */
	bool		probed[2];
	bool		found[2];
	Datum		value[2];		/* in the cache's memory */
} LionEndpoint;

/*
 * The ends read in one planner run.  A run is one PlannerGlobal, shared by
 * every PlannerInfo of the query; the cache lives in the memory that holds it
 * and is forgotten when that memory is reset or deleted.  While it exists no
 * other PlannerGlobal can be at the same address, so a new run - even one
 * planned in the same memory - always starts a new cache.
 */
typedef struct LionProbeCache
{
	PlannerGlobal *glob;
	MemoryContext cxt;
	List	   *ends;			/* LionEndpoint */
	List	   *remainders;		/* LionPlainRemainder */
	List	   *isects;			/* LionIsect: the intersection probe's */
	int			nisectprobes;	/* ... how many it has made */
	int64		isectbuffers;	/* ... and the buffers they accessed */
	int			isectwidest;	/* ... the most sets one of them sought
								 * (lion_isect_run_budget()) */
	List	   *rowsfixed;		/* RelOptInfos whose rows the probe corrected */
} LionProbeCache;

static LionProbeCache lion_probe_cache;

static void
lion_probe_cache_reset(void *arg)
{
	if (lion_probe_cache.glob == (PlannerGlobal *) arg)
		memset(&lion_probe_cache, 0, sizeof(lion_probe_cache));
}

static LionProbeCache *
lion_probe_cache_for(PlannerInfo *root)
{
	if (lion_probe_cache.glob != root->glob)
	{
		MemoryContext cxt = GetMemoryChunkContext(root->glob);
		MemoryContextCallback *cb;

		cb = (MemoryContextCallback *) MemoryContextAlloc(cxt, sizeof(*cb));
		cb->func = lion_probe_cache_reset;
		cb->arg = root->glob;
		MemoryContextRegisterResetCallback(cxt, cb);

		lion_probe_cache.glob = root->glob;
		lion_probe_cache.cxt = cxt;
		lion_probe_cache.ends = NIL;
		lion_probe_cache.remainders = NIL;
		lion_probe_cache.isects = NIL;
		lion_probe_cache.rowsfixed = NIL;
		lion_probe_cache.nisectprobes = 0;
		lion_probe_cache.isectbuffers = 0;
		lion_probe_cache.isectwidest = 0;
	}
	return &lion_probe_cache;
}

/* ---------------------------------------------------------------------
 * The ends found empty, remembered across planner runs
 * --------------------------------------------------------------------- */

/*
 * An end where the probe found no live key - it gave up at one of its
 * bounds, or read the column's whole run - is not probed again until
 * something that could change the answer has happened.  An end whose rows
 * were deleted in bulk used to be walked to the bounds at every plan until
 * VACUUM: core's probe marks the btree entries it finds dead and skips them
 * the next time, and a posting set has nothing to mark.
 *
 * A dead row never becomes live again, so what can change the answer is new
 * rows, VACUUM (which removes entries and their leaves' contents), and a new
 * index or table under the same OIDs.  The memory is forgotten
 *	- when the pg_class row of the table or of the index changes, which VACUUM
 *	  and ANALYZE make it do whenever they update the statistics there, and
 *	  every DDL that rewrites or drops either (lion_probe_miss_inval(), a
 *	  syscache callback: a relcache callback would take one of the ten slots a
 *	  backend has for them all, and running out of those is FATAL);
 *	- when the table or the index has grown since - rel->pages and the index's
 *	  pages as the planner has just read them - which rows added past the end
 *	  soon make it.
 * Rows that neither grow the table or the index nor come with a VACUUM or an
 * ANALYZE are not noticed until one of those; until then the estimate is the
 * histogram's own end, which is what core's probe leaves when it gives up.
 *
 * A handful per backend, the oldest replaced first.
 */
typedef struct LionProbeMiss
{
	Oid			indexoid;		/* InvalidOid: a free slot */
	AttrNumber	col;			/* the index's key column */
	bool		last;			/* its last end, or its first */
	uint32		heaphash;		/* the pg_class syscache hash of the table */
	uint32		indexhash;		/* ... and of the index */
	BlockNumber heappages;		/* rel->pages when the probe found nothing */
	BlockNumber indexpages;		/* ... and the index's */
} LionProbeMiss;

#define LION_PROBE_MISSES		16

static LionProbeMiss lion_probe_misses[LION_PROBE_MISSES];
static int	lion_probe_miss_next = 0;
static bool lion_probe_miss_callback = false;

/* A pg_class row changed, or (hashvalue 0) any may have. */
static void
lion_probe_miss_inval(Datum arg, LionSysCacheId cacheid, uint32 hashvalue)
{
	int			i;

	for (i = 0; i < LION_PROBE_MISSES; i++)
	{
		LionProbeMiss *m = &lion_probe_misses[i];

		if (hashvalue == 0 || m->heaphash == hashvalue ||
			m->indexhash == hashvalue)
			m->indexoid = InvalidOid;
	}
}

/* Did the probe find nothing at this end, with nothing changed since? */
static bool
lion_probe_missed(RelOptInfo *rel, IndexOptInfo *idx, AttrNumber col,
				  bool last)
{
	int			i;

	for (i = 0; i < LION_PROBE_MISSES; i++)
	{
		LionProbeMiss *m = &lion_probe_misses[i];

		if (m->indexoid == idx->indexoid && m->col == col && m->last == last)
		{
			if (m->heappages == rel->pages && m->indexpages == idx->pages)
				return true;
			m->indexoid = InvalidOid;	/* grown since: probe again */
			return false;
		}
	}
	return false;
}

static void
lion_probe_miss_note(RelOptInfo *rel, IndexOptInfo *idx, Oid heapoid,
					 AttrNumber col, bool last)
{
	LionProbeMiss *m = NULL;
	int			i;

	if (!lion_probe_miss_callback)
	{
		CacheRegisterSyscacheCallback(RELOID, lion_probe_miss_inval, (Datum) 0);
		lion_probe_miss_callback = true;
	}

	/* the slot this end had, or the oldest */
	for (i = 0; i < LION_PROBE_MISSES && m == NULL; i++)
	{
		if (lion_probe_misses[i].indexoid == idx->indexoid &&
			lion_probe_misses[i].col == col &&
			lion_probe_misses[i].last == last)
			m = &lion_probe_misses[i];
	}
	if (m == NULL)
	{
		m = &lion_probe_misses[lion_probe_miss_next];
		lion_probe_miss_next = (lion_probe_miss_next + 1) % LION_PROBE_MISSES;
	}
	m->indexoid = idx->indexoid;
	m->col = col;
	m->last = last;
	m->heaphash = GetSysCacheHashValue1(RELOID, ObjectIdGetDatum(heapoid));
	m->indexhash = GetSysCacheHashValue1(RELOID,
										 ObjectIdGetDatum(idx->indexoid));
	m->heappages = rel->pages;
	m->indexpages = idx->pages;
}

/* ---------------------------------------------------------------------
 * Is an entry's key live?
 * --------------------------------------------------------------------- */

typedef struct LionProbeHeap
{
	Relation	heap;
	SnapshotData snapshot;		/* SnapshotNonVacuumable */
	LionTidFetch fetch;			/* one fetch state for every TID */
	Buffer		vmbuf;
	uint32		ckey;			/* the container being read */
	BlockNumber lastblk;		/* the heap page visited last */
	int			npages;			/* heap pages visited */
	int			ntids;			/* TIDs asked about */
	bool		alive;			/* the entry has a row the probe accepts */
	bool		gaveup;			/* LION_PROBE_HEAP_PAGES or _TIDS were not
								 * enough */
} LionProbeHeap;

static bool
lion_probe_member(uint16 lo, void *arg)
{
	LionProbeHeap *ph = (LionProbeHeap *) arg;
	ItemPointerData tid;
	BlockNumber blk;

	lion_code_to_tid(((uint64) ph->ckey << LION_CONTAINER_BITS) | lo, &tid);
	blk = ItemPointerGetBlockNumber(&tid);

	if (VM_ALL_VISIBLE(ph->heap, blk, &ph->vmbuf) ||
		lion_tid_fetch(&ph->fetch, &tid, &ph->snapshot, NULL))
	{
		ph->alive = true;
		return false;
	}
	if (++ph->ntids >= LION_PROBE_HEAP_TIDS)
	{
		ph->gaveup = true;
		return false;
	}
	if (blk != ph->lastblk)
	{
		ph->lastblk = blk;
		if (++ph->npages > LION_PROBE_HEAP_PAGES)
		{
			ph->gaveup = true;
			return false;
		}
	}
	return true;
}

/*
 * Does the entry, a private copy of itemsz bytes, hold a row the probe
 * accepts?  Its members are read in heap order until one does, through a
 * stream of its one set that pins nothing between containers (DESIGN.md
 * §29.3), as a WALK reads a walked entry.  A CHAIN copy whose set VACUUM has
 * freed since reads as empty (DESIGN.md §18).
 */
static bool
lion_probe_entry_alive(LionProbeHeap *ph, Relation index,
					   LionEntryTuple *entry, Size itemsz)
{
	LionPostingSet ps;
	LionSetStream *st;
	const LionContainer *c;

	if (lion_entry_ntids(entry) == 0)
		return false;

	memset(&ps, 0, sizeof(ps));
	ps.index = index;
	ps.attno = entry->attno;
	ps.found = true;
	ps.pinbuf = InvalidBuffer;
	ps.ntids = lion_entry_ntids(entry);
	ps.ncontainers = lion_entry_ncontainers(entry);
	ps.entryblk = InvalidBlockNumber;
	ps.entryoff = InvalidOffsetNumber;
	ps.cxt = CurrentMemoryContext;
	if ((entry->flags & LION_ENTRY_INLINE) != 0)
	{
		ps.is_inline = true;
		ps.head = InvalidBlockNumber;
		ps.payload = LionEntryGetPayload(entry);
		ps.paylen = LION_ENTRY_PAYLOAD_LEN(entry, itemsz);
	}
	else
		ps.head = lion_entry_head(entry);

	ph->alive = false;
	st = lion_stream_begin(1, &ps, NULL, false);
	while (!ph->alive && !ph->gaveup && (c = lion_stream_next(st)) != NULL)
	{
		ph->ckey = c->ckey;
		lion_container_iterate(c, lion_probe_member, ph);
	}
	lion_stream_end(st);

	return ph->alive;
}

/* ---------------------------------------------------------------------
 * The probe
 * --------------------------------------------------------------------- */

/*
 * The first (last = false) or last key of index key column col with a live
 * row: a descent to where the column's VALUE entries begin
 * (lion_dir_value_start()) or end (lion_dir_value_end()), and a walk from
 * there towards the other end, a leaf at a time, until an entry has a row.
 *
 * Each leaf's VALUE entries of the column are copied under its share lock and
 * examined once it is released, so that no heap page is read with a directory
 * lock held.  The walk then reads the leaf again for its link and steps right
 * (lion_dir_step_right()) or left (lion_dir_step_left()); a leaf that split
 * meanwhile moved entries only to a new page on its right, which going right
 * reads and going left passes by - entries already examined, and ones
 * inserted since, which is as good an end for an estimate.  A leaf whose
 * entries are all dead is read like any other, and so is one VACUUM emptied
 * and has not taken out of the tree yet (DESIGN.md §21, "Page deletion"): an
 * end whose last LION_PROBE_LEAVES leaves hold no live key, which is what
 * deleting the newest rows in bulk leaves until VACUUM, is given up on.
 *
 * *settled is whether the answer holds until the index or its table changes
 * (lion_probe_miss_note()): an end found, a bound reached, or the column's
 * whole run read.  It is false only when a left sibling could not be found
 * among the pages concurrent splits put in the way.
 */
static bool
lion_probe_walk(LionProbeHeap *ph, Relation index, LionState *state,
				bool last, Datum *value, bool *settled)
{
	char	   *copy = (char *) palloc(BLCKSZ);
	LionEntryTuple **items;
	Size	   *sizes;
	int			maxitems;
	OffsetNumber boundary;
	Buffer		buf;
	int			nleaves = 0;
	bool		found = false;
	LionRightWalk walk;

	*settled = false;
	lion_rightwalk_init(&walk);

	/* every item at least an entry header and a line pointer, as a batch */
	maxitems = BLCKSZ / (MAXALIGN(LION_ENTRY_SHORT_HDRSZ) + sizeof(ItemIdData)) + 1;
	items = (LionEntryTuple **) palloc(sizeof(LionEntryTuple *) * maxitems);
	sizes = (Size *) palloc(sizeof(Size) * maxitems);

	buf = last ? lion_dir_value_end(index, state, &boundary) :
		lion_dir_value_start(index, state, &boundary);
	for (;;)
	{
		Page		page = BufferGetPage(buf);
		BlockNumber blkno = BufferGetBlockNumber(buf);
		OffsetNumber from = lion_page_first_data(page);
		OffsetNumber upto = OffsetNumberNext(PageGetMaxOffsetNumber(page));
		OffsetNumber off;
		char	   *dst = copy;
		int			n = 0;
		bool		runends = false;	/* the run's other end is on this leaf */
		int			i;

		/* the first leaf from or up to where the descent landed */
		if (nleaves == 0)
		{
			if (last)
				upto = boundary;
			else
				from = boundary;
		}

		for (off = from; off < upto; off = OffsetNumberNext(off))
		{
			ItemId		iid = PageGetItemId(page, off);
			LionEntryTuple *entry;
			int			c;
			Size		sz;

			if (!ItemIdIsUsed(iid))
				continue;
			entry = lion_entry_at(page, iid);

			/* before (< 0), in (0) or past (> 0) the column's VALUE run */
			if (entry->attno != state->attno)
				c = (entry->attno < state->attno) ? -1 : 1;
			else if (lion_entry_kind(entry) != LION_KIND_VALUE)
				c = (lion_entry_kind(entry) < LION_KIND_VALUE) ? -1 : 1;
			else
				c = 0;
			if (c < 0)
			{
				/* going left, the run begins here: nothing before counts */
				runends |= last;
				n = 0;
				dst = copy;
				continue;
			}
			if (c > 0)
			{
				/* going right, the run ends here */
				runends |= !last;
				break;
			}

			sz = ItemIdGetLength(iid);
			if (unlikely(n >= maxitems || dst + sz > copy + BLCKSZ))
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("lion index \"%s\": directory leaf %u holds more entries than fit on a page",
								RelationGetRelationName(index), blkno)));
			memcpy(dst, entry, sz);
			items[n] = (LionEntryTuple *) dst;
			sizes[n] = sz;
			n++;
			dst += MAXALIGN(sz);
		}
		if (last ? !BlockNumberIsValid(LionPageGetOpaque(page)->leftlink) :
			LionPageIsRightmost(page))
			runends = true;
		UnlockReleaseBuffer(buf);
		nleaves++;

		for (i = 0; i < n && !found && !ph->gaveup; i++)
		{
			int			k = last ? n - 1 - i : i;

			if (lion_probe_entry_alive(ph, index, items[k], sizes[k]))
			{
				*value = datumCopy(lion_entry_key(state, items[k]),
								   state->typbyval, state->typlen);
				found = true;
			}
		}
		if (found || ph->gaveup || runends || nleaves >= LION_PROBE_LEAVES)
		{
			*settled = true;
			break;
		}

		/* the next leaf, as this one's links now say */
		buf = ReadBuffer(index, blkno);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		if (!LionPageIsLeaf(BufferGetPage(buf)))
		{
			UnlockReleaseBuffer(buf);
			elog(ERROR, "lion index \"%s\": block %u is not a directory leaf",
				 RelationGetRelationName(index), blkno);
		}
		if (last)
			buf = lion_dir_step_left(index, buf, LION_PROBE_LEFT_STEPS);
		else if (LionPageIsRightmost(BufferGetPage(buf)))
		{
			UnlockReleaseBuffer(buf);
			buf = InvalidBuffer;
		}
		else
			buf = lion_dir_step_right(index, buf, BUFFER_LOCK_SHARE, &walk);
		if (!BufferIsValid(buf))
			break;
	}

	pfree(items);
	pfree(sizes);
	pfree(copy);
	return found;
}

/*
 * A lion index whose key column holds rel's column attno and whose
 * directory the probe may read for it, or NULL; *colp is that key column.
 * Not hypothetical (there is nothing to read) and not partial (its ends are
 * not the column's), as core requires of the indexes it probes.
 */
static IndexOptInfo *
lion_probe_index(RelOptInfo *rel, AttrNumber attno, AttrNumber *colp)
{
	Oid			amoid = lion_get_am_oid();
	ListCell   *lc;

	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);
		int			c;

		if (idx->relam != amoid || idx->hypothetical || idx->indpred != NIL)
			continue;
		for (c = 0; c < idx->nkeycolumns; c++)
		{
			if (idx->indexkeys[c] == attno)
			{
				*colp = (AttrNumber) (c + 1);
				return idx;
			}
		}
	}
	return NULL;
}

/*
 * Is the index column's directory in the order of the histogram - the
 * column's own `<` under its own collation - and are its keys the
 * histogram's values?  The directory is in the order of its opclass's
 * comparison (DESIGN.md §21), which for every shipped class is the key type's
 * default btree comparison, and the key type is the column's own or one it
 * is binary-coercible to (varchar under text_ops).
 */
static bool
lion_probe_order_ok(LionState *state, Oid valtype, Oid staop, Oid stacoll)
{
	TypeCacheEntry *tce;
	int16		len1,
				len2;
	bool		byval1,
				byval2;

	if (state->multikey || !state->ordered || state->collation != stacoll)
		return false;
	tce = lookup_type_cache(state->typid, TYPECACHE_CMP_PROC | TYPECACHE_LT_OPR);
	if (!OidIsValid(tce->cmp_proc) || tce->cmp_proc != state->cmpproc.fn_oid ||
		!OidIsValid(tce->lt_opr) ||
		!comparison_ops_are_compatible(tce->lt_opr, staop))
		return false;
	if (state->typid == valtype)
		return true;
	if (!IsBinaryCoercible(state->typid, valtype) &&
		!IsBinaryCoercible(valtype, state->typid))
		return false;
	get_typlenbyval(state->typid, &len1, &byval1);
	get_typlenbyval(valtype, &len2, &byval2);
	return len1 == len2 && byval1 == byval2;
}

/*
 * The first (last = false) or last key of rel's column attno that has a row,
 * read from a lion index on it, once per planner run.  False when no lion
 * index can answer, or the probe gave up - now, or in an earlier run with
 * nothing changed since (lion_probe_missed()).
 */
static bool
lion_probe_endpoint(PlannerInfo *root, RelOptInfo *rel, AttrNumber attno,
					Oid valtype, Oid staop, Oid stacoll, bool last,
					Datum *value)
{
	RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);
	LionProbeCache *cache = lion_probe_cache_for(root);
	LionEndpoint *ep = NULL;
	IndexOptInfo *idx;
	AttrNumber	col = 0;
	int			end = last ? 1 : 0;
	ListCell   *lc;

	foreach(lc, cache->ends)
	{
		LionEndpoint *e = (LionEndpoint *) lfirst(lc);

		if (e->relid == rte->relid && e->attno == attno)
		{
			ep = e;
			break;
		}
	}
	if (ep == NULL)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(cache->cxt);

		ep = (LionEndpoint *) palloc0(sizeof(*ep));
		ep->relid = rte->relid;
		ep->attno = attno;
		cache->ends = lappend(cache->ends, ep);
		MemoryContextSwitchTo(oldcxt);
	}

	if (!ep->probed[end] &&
		(idx = lion_probe_index(rel, attno, &col)) != NULL &&
		!lion_probe_missed(rel, idx, col, last))
	{
		MemoryContext probecxt = AllocSetContextCreate(CurrentMemoryContext,
													   "lion endpoint probe",
													   ALLOCSET_DEFAULT_SIZES);
		MemoryContext oldcxt = MemoryContextSwitchTo(probecxt);
		Relation	heap = table_open(rte->relid, NoLock);
		Relation	index = index_open(idx->indexoid, NoLock);
		LionState  *state = lion_index_column_state(index, col);
		Datum		key = (Datum) 0;
		bool		found = false;

		if (lion_probe_order_ok(state, valtype, staop, stacoll))
		{
			LionProbeHeap ph;
			bool		settled;

			memset(&ph, 0, sizeof(ph));
			ph.heap = heap;
			ph.vmbuf = InvalidBuffer;
			ph.lastblk = InvalidBlockNumber;
			InitNonVacuumableSnapshot(ph.snapshot, GlobalVisTestFor(heap));
			lion_tid_fetch_begin(&ph.fetch, heap);

			found = lion_probe_walk(&ph, index, state, last, &key, &settled);
			lion_tid_fetch_end(&ph.fetch);
			if (BufferIsValid(ph.vmbuf))
				ReleaseBuffer(ph.vmbuf);
			if (!found && settled)
				lion_probe_miss_note(rel, idx, rte->relid, col, last);
			if (found)
			{
				MemoryContextSwitchTo(cache->cxt);
				ep->value[end] = datumCopy(key, state->typbyval,
										   state->typlen);
				MemoryContextSwitchTo(probecxt);
			}
		}
		index_close(index, NoLock);
		table_close(heap, NoLock);
		MemoryContextSwitchTo(oldcxt);
		MemoryContextDelete(probecxt);

		ep->found[end] = found;
	}
	ep->probed[end] = true;

	if (ep->found[end])
		*value = ep->value[end];
	return ep->found[end];
}

/* ---------------------------------------------------------------------
 * The histogram, with the probed ends
 * --------------------------------------------------------------------- */

/*
 * Which ends of the histogram core's ineq_histogram_selectivity() would ask
 * get_actual_variable_range() for, estimating `var op constval`: an end
 * whenever its binary search is about to compare the constant with it, and
 * both for a histogram of two.  This is that search, with the same
 * comparisons in the same order, so lion probes exactly where core would.
 */
static void
lion_probe_ends_wanted(AttStatsSlot *sslot, FmgrInfo *opproc, Oid collation,
					   bool isgt, Datum constval, bool *first, bool *last)
{
	int			lobound = 0;
	int			hibound = sslot->nvalues;

	if (sslot->nvalues == 2)
	{
		*first = *last = true;
		return;
	}
	while (lobound < hibound)
	{
		int			probe = (lobound + hibound) / 2;
		bool		ltcmp;

		if (probe == 0)
			*first = true;
		else if (probe == sslot->nvalues - 1)
			*last = true;
		ltcmp = DatumGetBool(FunctionCall2Coll(opproc, collation,
											   sslot->values[probe],
											   constval));
		if (isgt)
			ltcmp = !ltcmp;
		if (ltcmp)
			lobound = probe + 1;
		else
			hibound = probe;
	}
}

/*
 * Would core read this column's ends itself, from an ordered index that can
 * return its first column (get_actual_variable_range())?  Then every estimate
 * of the column is corrected already, lion's among them.
 */
static bool
lion_core_probes(RelOptInfo *rel, AttrNumber attno)
{
	ListCell   *lc;

	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);

		if (idx->sortopfamily != NULL && idx->indpred == NIL &&
			!idx->hypothetical && idx->canreturn[0] &&
			idx->indexkeys[0] == attno)
			return true;
	}
	return false;
}

/* One column whose statistics the probe corrects. */
typedef struct LionProbeAtt
{
	AttrNumber	attno;
	bool		first;			/* its histogram's first end is wanted */
	bool		last;			/* ... its last */
	HeapTuple	stats;			/* pg_statistic, with the ends probed */
	bool		acl_ok;			/* what examine_variable() said of the real
								 * row */
	List	   *rinfos;			/* the RestrictInfos that bound it */
} LionProbeAtt;

/*
 * A RestrictInfo's cached selectivity, set aside while a scope lasts: core
 * caches a clause's selectivity in the RestrictInfo the first time it is
 * asked (clause_selectivity_ext()), which is when the relation's size is
 * estimated, so every later estimate of the clause - genericcostestimate()'s
 * included - returns that number whatever the statistics say.  The scope
 * clears it on each clause it corrects, so that it is estimated again, and
 * puts it back when it ends.
 */
typedef struct LionProbeSaved
{
	RestrictInfo *rinfo;
	Selectivity norm_selec;
	Selectivity outer_selec;
} LionProbeSaved;

/*
 * A copy of a pg_statistic row whose histogram has the probed ends in place of
 * its own; NULL when neither end was found.
 */
static HeapTuple
lion_probe_stats(PlannerInfo *root, RelOptInfo *rel, VariableStatData *vardata,
				 LionProbeAtt *att)
{
	Form_pg_statistic stats = (Form_pg_statistic) GETSTRUCT(vardata->statsTuple);
	AttStatsSlot sslot;
	Datum	   *values;
	Datum		end;
	bool		changed = false;
	int			slot;
	HeapTuple	result = NULL;

	for (slot = 0; slot < STATISTIC_NUM_SLOTS; slot++)
	{
		if ((&stats->stakind1)[slot] == STATISTIC_KIND_HISTOGRAM)
			break;
	}
	if (slot >= STATISTIC_NUM_SLOTS ||
		!get_attstatsslot(&sslot, vardata->statsTuple,
						  STATISTIC_KIND_HISTOGRAM, InvalidOid,
						  ATTSTATSSLOT_VALUES))
		return NULL;

	values = (Datum *) palloc(sizeof(Datum) * sslot.nvalues);
	memcpy(values, sslot.values, sizeof(Datum) * sslot.nvalues);
	if (att->first &&
		lion_probe_endpoint(root, rel, att->attno, sslot.valuetype,
							sslot.staop, sslot.stacoll, false, &end))
	{
		values[0] = end;
		changed = true;
	}
	if (att->last &&
		lion_probe_endpoint(root, rel, att->attno, sslot.valuetype,
							sslot.staop, sslot.stacoll, true, &end))
	{
		values[sslot.nvalues - 1] = end;
		changed = true;
	}

	if (changed)
	{
		int16		typlen;
		bool		typbyval;
		char		typalign;
		ArrayType  *arr;
		Relation	sd;
		int			attnum = Anum_pg_statistic_stavalues1 + slot;
		Datum		repl;
		bool		isnull = false;

		get_typlenbyvalalign(sslot.valuetype, &typlen, &typbyval, &typalign);
		arr = construct_array(values, sslot.nvalues, sslot.valuetype,
							  typlen, typbyval, typalign);
		repl = PointerGetDatum(arr);
		sd = table_open(StatisticRelationId, AccessShareLock);
		result = heap_modify_tuple_by_cols(vardata->statsTuple,
										   RelationGetDescr(sd), 1, &attnum,
										   &repl, &isnull);
		table_close(sd, AccessShareLock);
	}
	free_attstatsslot(&sslot);
	pfree(values);
	return result;
}

/* ---------------------------------------------------------------------
 * The scope in which lion's own estimates see the probed ends
 * --------------------------------------------------------------------- */

typedef struct LionProbeScope
{
	PlannerGlobal *glob;
	Index		rti;			/* the rel, by its range table index */
	Oid			relid;			/* ... and its table */
	List	   *atts;			/* LionProbeAtt */
	List	   *saved;			/* LionProbeSaved */
	double		rows;			/* rel's rows seen so, or -1 */
	MemoryContext cxt;
	struct LionProbeScope *outer;
} LionProbeScope;

static LionProbeScope *lion_probe_scope = NULL;
static get_relation_stats_hook_type lion_prev_relation_stats_hook = NULL;
static set_rel_pathlist_hook_type lion_prev_set_rel_pathlist_hook = NULL;
static void lion_plain_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
										Index rti, RangeTblEntry *rte);

/* n_distinct from the directory (DESIGN.md §33), at the end of this file */
static get_index_stats_hook_type lion_prev_index_stats_hook = NULL;
static bool lion_enable_index_ndistinct = true;
static bool lion_ndistinct_nested = false;
static bool lion_ndistinct_stats(PlannerInfo *root, RangeTblEntry *rte,
								 AttrNumber attnum, VariableStatData *vardata);
static bool lion_index_expr_stats(PlannerInfo *root, Oid indexOid,
								  AttrNumber indexattnum,
								  VariableStatData *vardata);

static bool
lion_relation_stats(PlannerInfo *root, RangeTblEntry *rte, AttrNumber attnum,
					VariableStatData *vardata)
{
	LionProbeScope *scope = lion_probe_scope;

	if (scope != NULL && root != NULL && root->glob == scope->glob &&
		rte->rtekind == RTE_RELATION && !rte->inh && rte->relid == scope->relid)
	{
		ListCell   *lc;

		foreach(lc, scope->atts)
		{
			LionProbeAtt *att = (LionProbeAtt *) lfirst(lc);

			if (att->attno == attnum && att->stats != NULL)
			{
				vardata->statsTuple = heap_copytuple(att->stats);
				vardata->freefunc = heap_freetuple;
				vardata->acl_ok = att->acl_ok;
				return true;
			}
		}
	}

	/*
	 * The lookup lion_ndistinct_stats() asks core for: core's own row and
	 * acl_ok, which the hooks before this one have declined already.
	 */
	if (lion_ndistinct_nested)
		return false;
	if (lion_prev_relation_stats_hook != NULL &&
		lion_prev_relation_stats_hook(root, rte, attnum, vardata))
		return true;
	return lion_ndistinct_stats(root, rte, attnum, vardata);
}

/*
 * _PG_init(): the hook through which the probed ends are seen, and the one
 * that charges a plain scan what cost_index() could not.  Called after
 * lion_ordered_init(), so that the plain scans are repriced before LionOrdered
 * compares its path with them.
 */
void
lion_selfuncs_init(void)
{
	lion_prev_relation_stats_hook = get_relation_stats_hook;
	get_relation_stats_hook = lion_relation_stats;
	lion_prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = lion_plain_set_rel_pathlist;

	/*
	 * DESIGN.md §33: a column's n_distinct from a lion index's count of its
	 * keys, through both statistics hooks, the one above for a column and
	 * get_index_stats_hook for an expression an index holds.
	 */
	lion_prev_index_stats_hook = get_index_stats_hook;
	get_index_stats_hook = lion_index_expr_stats;
	DefineCustomBoolVariable("pg_lion.enable_index_ndistinct",
							 "Takes a column's number of distinct values from a lion index on it.",
							 "Off, the planner uses the number ANALYZE estimated from its sample.",
							 &lion_enable_index_ndistinct,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);
}

/*
 * The column of rel that `clause` bounds, as a range comparison a lion index
 * on it answers with a constant at plan time (a Param or a volatile
 * expression has no value to probe with), and the ends of its histogram core
 * would read for it.  0 when it is no such clause.
 */
static AttrNumber
lion_probe_clause(PlannerInfo *root, RelOptInfo *rel, Node *clause,
				  bool *first, bool *last)
{
	OpExpr	   *op;
	VariableStatData vardata;
	Node	   *other;
	Node	   *node;
	bool		varonleft;
	Oid			opno;
	AttrNumber	attno = 0;
	AttrNumber	col;
	IndexOptInfo *idx;
	int			strategy;

	if (IsA(clause, RestrictInfo))
		clause = (Node *) ((RestrictInfo *) clause)->clause;
	if (!IsA(clause, OpExpr) || list_length(((OpExpr *) clause)->args) != 2)
		return 0;
	op = (OpExpr *) clause;

	if (!get_restriction_variable(root, op->args, rel->relid, &vardata,
								  &other, &varonleft))
		return 0;

	node = (Node *) vardata.var;
	while (node != NULL && IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;
	opno = varonleft ? op->opno : get_commutator(op->opno);

	/*
	 * The search below calls the clause's operator on the histogram's values,
	 * which are rows the user may not be allowed to read: a column without
	 * SELECT, or a table behind a security barrier view or a row-level
	 * policy.  Core's ineq_histogram_selectivity() asks
	 * statistic_proc_security_check() before its own search and uses no
	 * histogram at all when the answer is no (the operator is not leakproof);
	 * the probe asks the same of the same row and, told no, reads nothing -
	 * there would be no histogram for its ends to go into.
	 */
	if (node != NULL && IsA(node, Var) &&
		(Index) ((Var *) node)->varno == rel->relid &&
		((Var *) node)->varattno > 0 &&
		IsA(other, Const) && !((Const *) other)->constisnull &&
		OidIsValid(opno) && HeapTupleIsValid(vardata.statsTuple) &&
		statistic_proc_security_check(&vardata, get_opcode(opno)) &&
		(idx = lion_probe_index(rel, ((Var *) node)->varattno, &col)) != NULL &&
		LION_STRAT_IS_RANGE(strategy =
							get_op_opfamily_strategy(opno,
													 idx->opfamily[col - 1])))
	{
		AttStatsSlot sslot;

		/* core's own conditions for using the histogram at all */
		if (get_attstatsslot(&sslot, vardata.statsTuple,
							 STATISTIC_KIND_HISTOGRAM, InvalidOid,
							 ATTSTATSSLOT_VALUES))
		{
			if (sslot.nvalues > 1 && sslot.stacoll == op->inputcollid &&
				comparison_ops_are_compatible(sslot.staop, opno))
			{
				FmgrInfo	opproc;

				fmgr_info(get_opcode(opno), &opproc);
				lion_probe_ends_wanted(&sslot, &opproc, op->inputcollid,
									   LION_STRAT_IS_LOWER(strategy),
									   ((Const *) other)->constvalue,
									   first, last);
				if (*first || *last)
					attno = ((Var *) node)->varattno;
			}
			free_attstatsslot(&sslot);
		}
	}
	ReleaseVariableStats(vardata);
	return attno;
}

/*
 * Begin pricing one of lion's own accesses to rel with the endpoint probe
 * applied to the range comparisons among `clauses` (RestrictInfos or bare
 * clauses): while the scope lasts, the statistics of each column one of them
 * bounds past a histogram end - where core would read the column's actual
 * end - are seen with the ends a lion index holds.  True when anything was
 * corrected; the caller must then call lion_probe_end(), on error too.
 */
bool
lion_probe_begin(PlannerInfo *root, RelOptInfo *rel, List *clauses)
{
	RangeTblEntry *rte;
	LionProbeScope *scope;
	MemoryContext cxt;
	MemoryContext oldcxt;
	List	   *atts = NIL;
	ListCell   *lc;
	bool		any = false;

	if (root == NULL || root->glob == NULL || rel == NULL ||
		rel->relid == 0 || rel->rtekind != RTE_RELATION ||
		rel->indexlist == NIL || clauses == NIL)
		return false;
	rte = planner_rt_fetch(rel->relid, root);
	if (rte->rtekind != RTE_RELATION || rte->inh ||
		rte->relkind == RELKIND_PARTITIONED_TABLE)
		return false;

	/* An enclosing scope for the same relation corrects it already. */
	for (scope = lion_probe_scope; scope != NULL; scope = scope->outer)
	{
		if (scope->glob == root->glob && scope->relid == rte->relid)
			return false;
	}

	cxt = AllocSetContextCreate(CurrentMemoryContext, "lion probe scope",
								ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	foreach(lc, clauses)
	{
		bool		first = false;
		bool		last = false;
		AttrNumber	attno = lion_probe_clause(root, rel, (Node *) lfirst(lc),
											  &first, &last);
		LionProbeAtt *att = NULL;
		ListCell   *lc2;

		if (attno == 0 || lion_core_probes(rel, attno))
			continue;
		foreach(lc2, atts)
		{
			if (((LionProbeAtt *) lfirst(lc2))->attno == attno)
				att = (LionProbeAtt *) lfirst(lc2);
		}
		if (att == NULL)
		{
			att = (LionProbeAtt *) palloc0(sizeof(LionProbeAtt));
			att->attno = attno;
			atts = lappend(atts, att);
		}
		att->first |= first;
		att->last |= last;
		if (IsA(lfirst(lc), RestrictInfo))
			att->rinfos = lappend(att->rinfos, lfirst(lc));
	}

	foreach(lc, atts)
	{
		LionProbeAtt *att = (LionProbeAtt *) lfirst(lc);
		VariableStatData vardata;
		Oid			vartype;
		int32		vartypmod;
		Oid			varcollid;
		Var		   *var;

		get_atttypetypmodcoll(rte->relid, att->attno, &vartype, &vartypmod,
							  &varcollid);
		var = makeVar(rel->relid, att->attno, vartype, vartypmod, varcollid, 0);
		examine_variable(root, (Node *) var, rel->relid, &vardata);
		if (HeapTupleIsValid(vardata.statsTuple))
		{
			att->acl_ok = vardata.acl_ok;
			att->stats = lion_probe_stats(root, rel, &vardata, att);
			any |= (att->stats != NULL);
		}
		ReleaseVariableStats(vardata);
	}

	if (!any)
	{
		MemoryContextSwitchTo(oldcxt);
		MemoryContextDelete(cxt);
		return false;
	}

	scope = (LionProbeScope *) MemoryContextAllocZero(cxt, sizeof(*scope));
	scope->glob = root->glob;
	scope->rti = rel->relid;
	scope->relid = rte->relid;
	scope->atts = atts;
	scope->rows = -1.0;
	scope->cxt = cxt;

	/* the corrected clauses are estimated again, and only while this lasts */
	foreach(lc, atts)
	{
		LionProbeAtt *att = (LionProbeAtt *) lfirst(lc);
		ListCell   *lc2;

		if (att->stats == NULL)
			continue;
		foreach(lc2, att->rinfos)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc2);
			LionProbeSaved *sv;

			sv = (LionProbeSaved *) MemoryContextAlloc(cxt, sizeof(*sv));
			sv->rinfo = rinfo;
			sv->norm_selec = rinfo->norm_selec;
			sv->outer_selec = rinfo->outer_selec;
			rinfo->norm_selec = -1;
			rinfo->outer_selec = -1;
			scope->saved = lappend(scope->saved, sv);
		}
	}
	MemoryContextSwitchTo(oldcxt);

	scope->outer = lion_probe_scope;
	lion_probe_scope = scope;
	return true;
}

void
lion_probe_end(void)
{
	LionProbeScope *scope = lion_probe_scope;
	ListCell   *lc;

	Assert(scope != NULL);
	lion_probe_scope = scope->outer;
	foreach(lc, scope->saved)
	{
		LionProbeSaved *sv = (LionProbeSaved *) lfirst(lc);

		sv->rinfo->norm_selec = sv->norm_selec;
		sv->rinfo->outer_selec = sv->outer_selec;
	}
	MemoryContextDelete(scope->cxt);
}

/* ---------------------------------------------------------------------
 * One form for one filter (DESIGN.md §29.11, "An OR of equalities is its
 * IN list")
 * --------------------------------------------------------------------- */

/*
 * `k = 1 OR k = 7 OR k = 9` and `k IN (1, 7, 9)` select the same rows, and
 * lion answers, prices and probes them as ONE clause: the IN list.  The OR
 * restriction is what core keeps in baserestrictinfo whatever it makes of it
 * for an index (PostgreSQL 18 matches it to an index column as the list,
 * match_orclause_to_indexcol(); 16 and 17 do not), and lion used to take it
 * for a union of single-key leaves (DESIGN.md §19): each leaf looked up and
 * priced alone, the union priced by another formula than the list's, no
 * disjoint sum, and no intersection probe at all.
 *
 * The list the OR spells, as core builds it for the index: every arm `expr op
 * Const` or `Const op expr` (the operator commuted), one operator, one input
 * collation and one constant type across the arms, the same expr in each, no
 * NULL constant, and an array type for the constants' type.  Anything else is
 * NULL, and stays the OR it is.  Whether the operator is an equality of the
 * index that answers expr is the caller's to ask, as for any IN list.  Only
 * constants: an OR over Params is left to core, as its lookups are the
 * executor's.
 */
Node *
lion_or_as_array(Node *clause)
{
	BoolExpr   *orclause;
	Node	   *expr = NULL;
	Oid			opno = InvalidOid;
	Oid			consttype = InvalidOid;
	Oid			inputcollid = InvalidOid;
	Oid			arraytype;
	Datum	   *elems;
	bool	   *nulls;
	int			dims[1];
	int			lbs[1] = {1};
	int16		typlen;
	bool		typbyval;
	char		typalign;
	ScalarArrayOpExpr *saop;
	int			n = 0;
	ListCell   *lc;

	if (clause != NULL && IsA(clause, RestrictInfo))
		clause = (Node *) ((RestrictInfo *) clause)->clause;
	if (clause == NULL || !IsA(clause, BoolExpr) ||
		((BoolExpr *) clause)->boolop != OR_EXPR ||
		list_length(((BoolExpr *) clause)->args) < 2)
		return NULL;
	orclause = (BoolExpr *) clause;

	elems = (Datum *) palloc(sizeof(Datum) * list_length(orclause->args));
	foreach(lc, orclause->args)
	{
		OpExpr	   *op = (OpExpr *) lfirst(lc);
		Node	   *left;
		Node	   *right;
		Node	   *side;
		Const	   *con;
		Oid			armop;

		if (!IsA(op, OpExpr) || list_length(op->args) != 2)
			break;
		left = (Node *) linitial(op->args);
		right = (Node *) lsecond(op->args);
		if (IsA(right, Const) && !IsA(left, Const))
		{
			side = left;
			con = (Const *) right;
			armop = op->opno;
		}
		else if (IsA(left, Const) && !IsA(right, Const))
		{
			side = right;
			con = (Const *) left;
			armop = get_commutator(op->opno);
		}
		else
			break;
		if (!OidIsValid(armop) || con->constisnull ||
			contain_volatile_functions(side))
			break;
		if (n == 0)
		{
			expr = side;
			opno = armop;
			consttype = con->consttype;
			inputcollid = op->inputcollid;
			if (consttype == RECORDOID || exprType(side) == RECORDOID)
				break;
		}
		else if (armop != opno || con->consttype != consttype ||
				 op->inputcollid != inputcollid || !equal(side, expr))
			break;
		elems[n++] = con->constvalue;
	}
	arraytype = (lc == NULL) ? get_array_type(consttype) : InvalidOid;
	if (!OidIsValid(arraytype))
	{
		pfree(elems);
		return NULL;
	}

	/* the array Const as make_SAOP_expr() builds it */
	nulls = (bool *) palloc0(sizeof(bool) * n);
	dims[0] = n;
	get_typlenbyvalalign(consttype, &typlen, &typbyval, &typalign);
	saop = makeNode(ScalarArrayOpExpr);
	saop->opno = opno;
	saop->opfuncid = get_opcode(opno);
	saop->hashfuncid = InvalidOid;
	saop->negfuncid = InvalidOid;
	saop->useOr = true;
	saop->inputcollid = inputcollid;
	saop->args = list_make2(copyObject(expr),
							makeConst(arraytype, -1, inputcollid, -1,
									  PointerGetDatum(construct_md_array(elems, nulls,
																		 1, dims, lbs,
																		 consttype,
																		 typlen,
																		 typbyval,
																		 typalign)),
									  false, false));
	saop->location = -1;
	pfree(elems);
	pfree(nulls);
	return (Node *) saop;
}

/*
 * A restriction clause in the form lion's estimates take it: an OR of
 * equalities as its IN list (lion_or_as_array()), and a boolean column by
 * itself - `col`, `NOT col`, `col IS TRUE`, `col IS FALSE`, which is how
 * eval_const_expressions() leaves `col = true` and `col = false` - as the
 * equality core's match_boolean_index_clause() hands an index scan.  Any
 * other clause is itself, bare.
 */
Node *
lion_canonical_clause(Node *clause)
{
	Node	   *arr;
	Node	   *arg;
	Node	   *bare;
	bool		val = true;

	if (clause != NULL && IsA(clause, RestrictInfo))
		clause = (Node *) ((RestrictInfo *) clause)->clause;
	if (clause == NULL)
		return NULL;
	if ((arr = lion_or_as_array(clause)) != NULL)
		return arr;

	arg = clause;
	if (IsA(clause, BoolExpr) && ((BoolExpr *) clause)->boolop == NOT_EXPR &&
		list_length(((BoolExpr *) clause)->args) == 1)
	{
		arg = (Node *) linitial(((BoolExpr *) clause)->args);
		val = false;
	}
	else if (IsA(clause, BooleanTest) &&
			 (((BooleanTest *) clause)->booltesttype == IS_TRUE ||
			  ((BooleanTest *) clause)->booltesttype == IS_FALSE))
	{
		arg = (Node *) ((BooleanTest *) clause)->arg;
		val = (((BooleanTest *) clause)->booltesttype == IS_TRUE);
	}
	bare = arg;
	while (bare != NULL && IsA(bare, RelabelType))
		bare = (Node *) ((RelabelType *) bare)->arg;
	if (bare != NULL && IsA(bare, Var) &&
		getBaseType(exprType(bare)) == BOOLOID)
		return (Node *) make_opclause(BooleanEqualOperator, BOOLOID, false,
									  (Expr *) arg,
									  (Expr *) makeBoolConst(val, false),
									  InvalidOid, InvalidOid);
	return clause;
}

#if PG_VERSION_NUM < 180000
/* Where ptr is in list, by pointer, or -1. */
static int
lion_list_position(List *list, void *ptr)
{
	ListCell   *lc;

	foreach(lc, list)
	{
		if (lfirst(lc) == ptr)
			return foreach_current_index(lc);
	}
	return -1;
}

/*
 * Does the list arr - lion_or_as_array()'s - name a key column of one of the
 * lion indexes, under an operator of its family and a collation it matches:
 * would 18's match_orclause_to_indexcol() match the OR to it?
 */
static bool
lion_or_list_matches(Node *arr, List *lionidx)
{
	ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) arr;
	Node	   *expr = (Node *) linitial(saop->args);
	ListCell   *lc;

	foreach(lc, lionidx)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);
		int			c;

		for (c = 0; c < idx->nkeycolumns; c++)
		{
			if (match_index_to_operand(expr, c, idx) &&
				op_in_opfamily(saop->opno, idx->opfamily[c]) &&
				(!OidIsValid(idx->indexcollations[c]) ||
				 idx->indexcollations[c] == saop->inputcollid))
				return true;
		}
	}
	return false;
}

/* Does the path's tree answer one of the lists (their RestrictInfos)? */
static bool
lion_or_list_uses(Path *p, List *lists)
{
	ListCell   *lc;

	if (IsA(p, BitmapHeapPath))
		return lion_or_list_uses(((BitmapHeapPath *) p)->bitmapqual, lists);
	if (IsA(p, BitmapAndPath) || IsA(p, BitmapOrPath))
	{
		List	   *quals = IsA(p, BitmapAndPath) ?
			((BitmapAndPath *) p)->bitmapquals :
			((BitmapOrPath *) p)->bitmapquals;

		foreach(lc, quals)
		{
			if (lion_or_list_uses((Path *) lfirst(lc), lists))
				return true;
		}
		return false;
	}
	if (IsA(p, IndexPath))
	{
		foreach(lc, ((IndexPath *) p)->indexclauses)
		{
			if (list_member_ptr(lists, ((IndexClause *) lfirst(lc))->rinfo))
				return true;
		}
	}
	return false;
}

/*
 * Hand a path built on the scratch copy back to rel: its IndexPaths to the
 * indexes they copied, and each IndexClause of a list to the OR restriction
 * the list stands for, the list its index qual - 18's IndexClause.
 */
static void
lion_or_list_restore(Path *p, RelOptInfo *rel, List *copies, List *indexes,
					 List *lists, List *ors)
{
	ListCell   *lc;

	if (IsA(p, BitmapHeapPath))
	{
		p->parent = rel;
		lion_or_list_restore(((BitmapHeapPath *) p)->bitmapqual, rel, copies,
							 indexes, lists, ors);
	}
	else if (IsA(p, BitmapAndPath) || IsA(p, BitmapOrPath))
	{
		List	   *quals = IsA(p, BitmapAndPath) ?
			((BitmapAndPath *) p)->bitmapquals :
			((BitmapOrPath *) p)->bitmapquals;

		p->parent = rel;
		foreach(lc, quals)
			lion_or_list_restore((Path *) lfirst(lc), rel, copies, indexes,
								 lists, ors);
	}
	else if (IsA(p, IndexPath))
	{
		IndexPath  *ipath = (IndexPath *) p;
		int			i = lion_list_position(copies, ipath->indexinfo);

		p->parent = rel;
		if (i >= 0)
			ipath->indexinfo = (IndexOptInfo *) list_nth(indexes, i);
		foreach(lc, ipath->indexclauses)
		{
			IndexClause *iclause = (IndexClause *) lfirst(lc);
			int			j = lion_list_position(lists, iclause->rinfo);

			if (j >= 0)
				iclause->rinfo = (RestrictInfo *) list_nth(ors, j);
		}
	}
}

/*
 * PostgreSQL 16 and 17: the lion index paths of rel that answer an OR of
 * equalities as the IN list it spells (lion_or_as_array(), DESIGN.md §29.11,
 * "An OR of equalities is its IN list").  18 matches such an OR to an index
 * column as that list (match_orclause_to_indexcol()): an IndexClause whose
 * rinfo is the OR restriction and whose index qual is the list, not lossy.
 * 16 and 17 match it to nothing but the arms of a BitmapOr, so that
 * `k IN (1, 7)` was ANDed with the other clauses in one lion scan and
 * `k = 1 OR k = 7` was left to the heap filter, or to a BitmapOr of a scan per
 * arm.  These are the paths 18 builds, built as LionOrdered builds its lion
 * side: create_index_paths() on a scratch copy of rel that sees the given lion
 * indexes, with each such OR replaced by its list among their restriction
 * clauses and no join clause - then handed back to rel, each list's
 * IndexClause pointing to the OR as 18's does, so that the plan's index
 * condition is the list, its recheck the OR, and neither is left to the
 * filter.  Unparameterized, non-partial paths only, and only those that
 * answer a list; NIL when no OR of rel spells one for a key column of the
 * indexes.  A path of the others is one core has built already.
 */
List *
lion_or_list_paths(PlannerInfo *root, RelOptInfo *rel, List *lionidx)
{
	List	   *ors = NIL;
	List	   *lists = NIL;
	List	   *canon = NIL;
	List	   *copies = NIL;
	List	   *result = NIL;
	RelOptInfo *scratch;
	ListCell   *lc;

	if (lionidx == NIL || rel->reloptkind != RELOPT_BASEREL ||
		!bms_is_empty(rel->lateral_relids))
		return NIL;

	/*
	 * Not beside a security barrier's or a row-level policy's quals, whose
	 * order against the lists core's matching would have to keep: those
	 * relations keep the paths core builds.
	 */
	foreach(lc, rel->baserestrictinfo)
	{
		if (lfirst_node(RestrictInfo, lc)->security_level > 0)
			return NIL;
	}

	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
		Node	   *arr = rinfo->pseudoconstant ? NULL :
			lion_or_as_array((Node *) rinfo);

		if (arr != NULL && lion_or_list_matches(arr, lionidx))
		{
			RestrictInfo *lrinfo = make_simple_restrictinfo(root, (Expr *) arr);

			lrinfo->security_level = rinfo->security_level;
			ors = lappend(ors, rinfo);
			lists = lappend(lists, lrinfo);
			canon = lappend(canon, lrinfo);
		}
		else
			canon = lappend(canon, rinfo);
	}
	if (ors == NIL)
	{
		list_free(canon);
		return NIL;
	}

	/* the indexes, their restriction clauses with the lists for the ORs */
	foreach(lc, lionidx)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);
		IndexOptInfo *copy = makeNode(IndexOptInfo);
		ListCell   *lc2;

		memcpy(copy, idx, sizeof(IndexOptInfo));
		copy->indrestrictinfo = NIL;
		foreach(lc2, idx->indrestrictinfo)
		{
			int			i = lion_list_position(ors, lfirst(lc2));

			copy->indrestrictinfo = lappend(copy->indrestrictinfo,
											i >= 0 ? list_nth(lists, i) :
											lfirst(lc2));
		}
		copies = lappend(copies, copy);
	}

	scratch = makeNode(RelOptInfo);
	memcpy(scratch, rel, sizeof(RelOptInfo));
	scratch->indexlist = copies;
	scratch->baserestrictinfo = canon;
	scratch->pathlist = NIL;
	scratch->ppilist = NIL;
	scratch->partial_pathlist = NIL;
	scratch->cheapest_startup_path = NULL;
	scratch->cheapest_total_path = NULL;
	scratch->cheapest_parameterized_paths = NIL;
	scratch->joininfo = NIL;
	scratch->has_eclass_joins = false;
	scratch->consider_parallel = false;
	create_index_paths(root, scratch);

	foreach(lc, scratch->pathlist)
	{
		Path	   *p = (Path *) lfirst(lc);

		if (p->param_info != NULL || !lion_or_list_uses(p, lists))
			continue;
		lion_or_list_restore(p, rel, copies, lionidx, lists, ors);
		result = lappend(result, p);
	}
	return result;
}
#endif

/* ---------------------------------------------------------------------
 * The intersection probe (DESIGN.md §29.11, "Correlated sets")
 * --------------------------------------------------------------------- */

/*
 * Core estimates a conjunction as the product of its clauses' selectivities,
 * as though the columns were independent.  Filters on columns derived from
 * one hidden attribute are not, and their AND comes out as many times too
 * small as the product of the other clauses' selectivities: a benchmark's
 * count over such filters was estimated at a small fraction of its rows, the
 * heap paths were priced for that, and the count that reads no heap was
 * priced out of the plan.
 *
 * Each of those clauses is a posting set of a lion index with an exact row
 * count, and the index can AND their containers at any container key.  So
 * where the AND of two or more set clauses on one lion index may be far from
 * core's product, lion MEASURES it for its own estimates: it samples
 * container keys of the smallest set (the driver), ANDs the other sets'
 * containers at each, and takes the share of the driver's rows that survive
 * as the conjunction's share of them.  Bounded like the endpoint probe:
 *
 *	- where it can matter: core's estimate of the AND at most
 *	  1/LION_ISECT_ERROR of the smallest clause's own - the most the AND can
 *	  hold - so that the probe can find it off by that much;
 *	- LION_ISECT_KEYS driver containers, however many sets the clauses
 *	  locate: by their rank among the driver's own where the driver can be
 *	  ranked (lion_isect_by_rank()), and otherwise in strata of the heap's
 *	  container keys - the first container of the driver's in each, so that
 *	  the sample spreads over the heap, and every container of a heap of no
 *	  more container keys than that.  The strata still ahead widen, or the
 *	  ranks ahead thin, when the samples so far, at what they cost, would
 *	  overrun the probe's budget, and a driver that lies in a few strata only
 *	  is sampled again, in strata of its own stretch.  A set is sought there
 *	  by a descent of its posting tree, the samples being many leaves apart;
 *	- LION_ISECT_MAX_SETS sets sought at each container, and a budget of
 *	  buffer accesses a planner run that grows with the widest conjunction
 *	  the run measures (lion_isect_run_budget()), of which a probe may take
 *	  what is left - the relation's own conjunction first.  A conjunction of
 *	  more sets than that, or than what is left affords LION_ISECT_MIN_KEYS
 *	  samples of, is measured without its widest clauses, each taken as
 *	  independent of the rest, as core takes it (lion_isect_measured_part());
 *	  a probe that cannot afford LION_ISECT_MIN_KEYS samples even so gives
 *	  up, and what the run measured of a part of the conjunction stands
 *	  (lion_isect_part_factor()), or core's estimate where it measured none;
 *	  LION_ISECT_RUN_PROBES probes a planner run, each conjunction measured
 *	  once in it, by whichever lion index of its table answers it first
 *	  (LionIsect);
 *	- a sample in which fewer than LION_ISECT_MIN_HITS containers kept a row
 *	  of the AND raises an estimate no further than the rows of it the sample
 *	  met, or than the longest part of the AND that enough containers did
 *	  keep a row of says, and never past what the sample can rule out; one
 *	  that lowers an estimate lowers it only as far as the sample can rule
 *	  out (lion_isect_probe());
 *	- no heap page is read, and no lock or pin is held between two containers:
 *	  the sets are located and let go of before the first container is read,
 *	  and the streams copy each posting leaf and release it (DESIGN.md §29.5).
 *	  The TIDs are counted whatever their visibility, which is what makes that
 *	  possible: a dead row not yet vacuumed counts as a row of its sets, as it
 *	  does in the sets' own counts, and dilutes the survivors and the driver
 *	  alike.
 *
 * The result corrects lion's OWN estimates only - the selectivity
 * lioncostestimate() returns for lion's plain and bitmap paths, and the rows
 * the count pushdown's recheck is priced for (lion_probe_rel_rows()) - as a
 * factor on core's estimate of the same clauses, applied when the probe finds
 * it off by LION_ISECT_ERROR or more either way.  rel->rows, and every other
 * path's estimate, stay core's: the relation is sized before its paths are
 * built, and a row count changed while they are would leave the paths built
 * before it priced for another relation than the ones built after.
 */

/* GUC pg_lion.enable_intersection_probe (DESIGN.md §29.11) */
bool		lion_enable_intersection_probe = true;

/* GUC pg_lion.enable_rows_correction (DESIGN.md §29.11) */
bool		lion_enable_rows_correction = true;

/*
 * The driver containers the probe samples: LION_ISECT_KEYS, however many sets
 * it seeks at each - the run's budget grows with them instead
 * (lion_isect_run_budget()) - and at least LION_ISECT_MIN_KEYS, or it gives
 * up.  A seek is a descent of a posting tree - two or three pages - or a step
 * to the next leaf, and most are a buffer access or two: the leaf the last
 * seek copied often holds the next key too.  A few dozen containers of a set
 * that has hundreds of rows in each put a factor of LION_ISECT_ERROR far
 * outside what the sample can err by: every conjunction measured on 100k and
 * 2M rows, uniform, clustered by the hidden attribute or sparse, came out
 * within 6% of its count at 8 to 32 containers (DESIGN.md §29.11,
 * "Correlated sets").  The sample does not shrink as a filter widens: the
 * wider a filter of correlated columns, the smaller the share of its driver's
 * rows its AND keeps, and the fewer of a small sample's containers hold a row
 * of it (DESIGN.md §29.11, "Wide filters").
 */
#define LION_ISECT_KEYS			32
#define LION_ISECT_MIN_KEYS		8

/*
 * The seeks of the other sources' sets a COUNT of a walked driver may make:
 * a driver whose containers left after the presence walk are no more than
 * that over the other sources' sets, or than LION_ISECT_KEYS, has every one
 * ANDed (lion_isect_by_rank()).
 */
#define LION_ISECT_SEEKS		128

/*
 * The buffer accesses a seek of one set is budgeted at: a descent of a
 * posting tree of two levels, which is what a sample's strata, many leaves
 * apart, cost - 1.9 a seek measured, the locate and the driver's own seeks
 * included (DESIGN.md §29.11, "Wide filters").
 */
#define LION_ISECT_SEEK_BUFFERS	2

/*
 * The posting sets a probe seeks at each container it samples at most: a
 * list's values, a query's keys.  A conjunction of more is measured without
 * its widest clauses (lion_isect_measured_part()), each taken as independent
 * of the rest, as core takes it; a list of more values is never located for
 * the probe (lion_isect_scankey()), and is taken so too.
 */
#define LION_ISECT_MAX_SETS		32

/*
 * The sampled containers that must keep a row of the AND for the probe to
 * raise an estimate as far as the sample says: fewer is a sample whose error
 * is as large as what it says, which raises it as far as the rows of the AND
 * it met, or as a part of the conjunction the sample met more often says
 * (lion_isect_factor_run()).
 */
#define LION_ISECT_MIN_HITS		3

/*
 * How the driver's containers are chosen (DESIGN.md §29.11, "A sample of the
 * driver's own rows"): by their RANK among the driver's own, so that the
 * sample stands for its rows however they lie in the heap.  A driver of at
 * most LION_ISECT_WALK_PAGES posting leaves and LION_ISECT_WALK_KEYS
 * containers is walked once to rank them - no more pages than its samples'
 * seeks would read; a single set of more is ranked by its posting tree's
 * separators, its leaves being about equal bytes of it, in at most
 * LION_ISECT_SEPARATOR_PAGES pages: the root and, below a root of fewer
 * children than samples, the level under it.  What can be ranked neither way
 * - a list or a multi-key query of more - is sampled in strata of the heap's
 * container keys.
 */
#define LION_ISECT_WALK_PAGES		32
#define LION_ISECT_WALK_KEYS		8192
#define LION_ISECT_SEPARATOR_PAGES	(LION_ISECT_KEYS + 1)

/*
 * The normal quantile of the one-sided bound a sample lowers an estimate to
 * at most: 95% (lion_isect_upper()).
 */
#define LION_ISECT_BOUND_Z		1.645

/*
 * The buffer accesses a planner run's probes together may make, locating
 * their sets and seeking them, beside what LION_ISECT_KEYS samples of the
 * widest conjunction the run measures cost at LION_ISECT_SEEK_BUFFERS a seek
 * of each of its sets (lion_isect_run_budget()): 1,024 more for a
 * conjunction of 16 sets, 2,048 for one of LION_ISECT_MAX_SETS, so that the
 * relation's own conjunction, measured first, leaves the run's other probes
 * this much whatever its width.  A probe may take whatever the run has left:
 * the bound is checked before each clause is located and before each sample,
 * so a probe overruns it by one clause's lookups (an IN list of
 * LION_ISECT_MAX_SETS values) or one sample's seeks at most, and the strata
 * ahead widen, or the ranks ahead thin, to keep a sample's worth of it to
 * spare (lion_isect_pass(), lion_isect_sample_at()).  A conjunction that what
 * the run has left cannot afford LION_ISECT_MIN_KEYS samples of is measured
 * without its widest clauses (lion_isect_measured_part()).  At worst a run
 * accesses 3,072 buffers, one sample's seeks and one list's lookups: a few
 * milliseconds of shared-buffer hits, or that many reads on a cold cache
 * (DESIGN.md §29.11, "Wide filters").
 */
#define LION_ISECT_RUN_BUFFERS	1024

/* The probes one planner run makes: a conjunction is measured once in it. */
#define LION_ISECT_RUN_PROBES	8

/* One past the last container key: the end of the first pass's range. */
#define LION_ISECT_KEY_END		((double) PG_UINT32_MAX + 1.0)

/*
 * How far core's estimate of a conjunction must be able to be off - and how
 * far the probe must find it off, either way - for the probe's number to be
 * taken instead.  Less than that is within what ANALYZE's per-column
 * estimates and the sample leave of an independent conjunction, and would
 * move plans on noise.
 */
#define LION_ISECT_ERROR		3.0

/* One set clause of a probed conjunction. */
typedef struct LionIsectClause
{
	AttrNumber	col;			/* the index's key column, 1-based */
	AttrNumber	attno;			/* ... its table's column, 0 for an
								 * expression */
	Node	   *clause;			/* as the caller has it */
	ScanKeyData skey;			/* ... as the scan's key */
	Selectivity sel;			/* core's selectivity of it */
} LionIsectClause;

/*
 * A conjunction measured in this planner run, and what it came to.  It is the
 * table's, not an index's: its clauses are matched by the table's columns, so
 * that every lion index of the table that answers the same clauses takes the
 * one measurement, whichever of them asked first - the share of the rows an
 * AND keeps is the heap's, and each index holds the same TIDs.  Measured
 * again for each index, the run's budget and its LION_ISECT_RUN_PROBES would
 * run out on the second or the third, which would keep core's estimate: the
 * relation's rows (lion_isect_rel_factor(), from one of its indexes) and the
 * path of that index (lioncostestimate(), from its own) would disagree, and
 * that path, priced for core's product, would be taken for it.
 */
typedef struct LionIsect
{
	Oid			relid;			/* the table */
	int			n;
	AttrNumber *attnos;			/* the table's column of each clause */
	Node	  **clauses;		/* bare, in the cache's memory */
	double		factor;			/* core's estimate is multiplied by it */
	bool		measured;		/* by a probe of its own, or taken from a
								 * part of it (lion_isect_part_factor()) */
} LionIsect;

/* What a probe measured (lion_isect_measure()). */
typedef struct LionIsectResult
{
	double		sel;			/* the share of the relation's rows the AND
								 * holds */
	double		ub;				/* ... at most, by what the sample can rule
								 * out (lion_isect_upper()) */
	double		lb;				/* ... at least: the rows of it the sample
								 * met */
	int			hits;			/* the containers sampled that kept a row of
								 * it; INT_MAX for a count */
	int64		used;			/* the buffers the probe accessed */
	int			partn;			/* the longest part of the conjunction that
								 * LION_ISECT_MIN_HITS containers kept a row
								 * of, in the order the AND is made, or 0 */
	int		   *part;			/* ... its clauses, as indexes of cl[] */
	double		partsel;		/* ... and the share of the rows it holds */
	double		partrows;		/* ... and the rows of it the sample met */
	double		androws;		/* the rows of the whole AND the sample met */
} LionIsectResult;

static Node *
lion_isect_bare(Node *clause)
{
	if (IsA(clause, RestrictInfo))
		return (Node *) ((RestrictInfo *) clause)->clause;
	return clause;
}

static Node *
lion_isect_strip(Node *node)
{
	while (node != NULL && IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;
	return node;
}

/*
 * Is `clause` a set clause of index key column col0 (0-based) with a value to
 * look up now - an equality or a multi-key query with a constant, an IN list
 * of at most LION_ISECT_MAX_SETS constants, `IS NULL` - and what is it as the
 * scan's key?  The indexed expression may stand on either side of an
 * operator, as core matches it (match_index_to_operand()).  The clause is
 * taken in its canonical form (lion_canonical_clause()): an OR of equalities
 * on the column is the IN list it spells, and `NOT col` of a boolean column
 * is `col = false`, as an index scan is handed them.
 */
static bool
lion_isect_scankey(IndexOptInfo *idx, int col0, Node *clause, ScanKey skey)
{
	Oid			opfamily = idx->opfamily[col0];
	bool		multikey = OidIsValid(get_opfamily_proc(opfamily,
														idx->opcintype[col0],
														idx->opcintype[col0],
														LION_EXTRACTVALUE_PROC));
	Oid			opno;
	Oid			collid;
	Node	   *value;
	int			flags = 0;
	int			strategy;
	Oid			lefttype;
	Oid			righttype;

	clause = lion_canonical_clause(clause);
	if (clause == NULL)
		return false;
	if (IsA(clause, NullTest))
	{
		NullTest   *nt = (NullTest *) clause;

		if (nt->nulltesttype != IS_NULL || nt->argisrow ||
			!match_index_to_operand((Node *) nt->arg, col0, idx))
			return false;
		ScanKeyEntryInitialize(skey, SK_ISNULL | SK_SEARCHNULL,
							   (AttrNumber) (col0 + 1), InvalidStrategy,
							   InvalidOid, InvalidOid, InvalidOid, (Datum) 0);
		return true;
	}
	if (IsA(clause, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;
		Const	   *arr;

		if (!saop->useOr || list_length(saop->args) != 2 ||
			!match_index_to_operand((Node *) linitial(saop->args), col0, idx))
			return false;
		value = lion_isect_strip((Node *) lsecond(saop->args));
		if (value == NULL || !IsA(value, Const))
			return false;
		arr = (Const *) value;
		if (arr->constisnull ||
			ArrayGetNItems(ARR_NDIM(DatumGetArrayTypeP(arr->constvalue)),
						   ARR_DIMS(DatumGetArrayTypeP(arr->constvalue))) >
			LION_ISECT_MAX_SETS)
			return false;
		opno = saop->opno;
		collid = saop->inputcollid;
		flags = SK_SEARCHARRAY;
	}
	else if (IsA(clause, OpExpr) && list_length(((OpExpr *) clause)->args) == 2)
	{
		OpExpr	   *op = (OpExpr *) clause;

		if (match_index_to_operand((Node *) linitial(op->args), col0, idx))
		{
			opno = op->opno;
			value = lion_isect_strip((Node *) lsecond(op->args));
		}
		else if (match_index_to_operand((Node *) lsecond(op->args), col0, idx))
		{
			opno = get_commutator(op->opno);
			value = lion_isect_strip((Node *) linitial(op->args));
		}
		else
			return false;
		if (!OidIsValid(opno) || value == NULL || !IsA(value, Const) ||
			((Const *) value)->constisnull)
			return false;
		collid = op->inputcollid;
	}
	else
		return false;

	if (!op_in_opfamily(opno, opfamily))
		return false;
	get_op_opfamily_properties(opno, opfamily, false, &strategy, &lefttype,
							   &righttype);
	if (multikey ? !LION_STRAT_IS_MULTI(strategy) :
		strategy != LION_STRAT_EQUAL)
		return false;

	ScanKeyEntryInitialize(skey, flags, (AttrNumber) (col0 + 1),
						   (StrategyNumber) strategy, righttype, collid,
						   get_opcode(opno), ((Const *) value)->constvalue);
	return true;
}

/*
 * The posting sets a set clause's scan key locates, as the probe expects them
 * before it locates them: an IN list's values, one for any other clause - a
 * multi-key query's keys are known once located (lion_isect_measure()).
 */
static int
lion_isect_clause_sets(const ScanKeyData *skey)
{
	ArrayType  *arr;

	if ((skey->sk_flags & SK_SEARCHARRAY) == 0)
		return 1;
	arr = DatumGetArrayTypeP(skey->sk_argument);
	return Max(ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr)), 1);
}

/*
 * The buffer accesses a planner run's probes may make together: what
 * LION_ISECT_KEYS samples of the widest conjunction the run measures cost,
 * at LION_ISECT_SEEK_BUFFERS a seek of each of its sets, and
 * LION_ISECT_RUN_BUFFERS beside that for the run's other probes.
 */
static int64
lion_isect_run_budget(const LionProbeCache *cache)
{
	return (int64) LION_ISECT_RUN_BUFFERS +
		(int64) LION_ISECT_KEYS * LION_ISECT_SEEK_BUFFERS *
		(int64) Min(cache->isectwidest, LION_ISECT_MAX_SETS);
}

/* The buffers this backend has accessed, hits and reads, shared and local. */
static int64
lion_isect_buffers(void)
{
	return pgBufferUsage.shared_blks_hit + pgBufferUsage.shared_blks_read +
		pgBufferUsage.local_blks_hit + pgBufferUsage.local_blks_read;
}

/*
 * One probe's sample (lion_isect_measure()): the sources' streams, the driver
 * first, and what the containers sampled so far came to.
 */
typedef struct LionIsectSample
{
	int			n;				/* the sources */
	LionSetStream **st;			/* ... their streams, in the clauses' order */
	int		   *order;			/* order[0] the driver, the others by rows */
	bool	   *done;			/* a source with nothing left at or past a key */
	LionContainer *acc;			/* the running AND, and room for the next */
	LionContainer *tmp;
	int64		start;			/* lion_isect_buffers() when the probe began */
	int64		budget;			/* ... and what it may add to that */
	double		drvrows;		/* the driver's rows in the containers sampled,
								 * weighted */
	double		androws;		/* ... and those the AND kept */
	int			nsampled;		/* the containers sampled */
	int			hits;			/* ... and those in which the AND kept a row */
	double		units;			/* ... the units of evidence they are
								 * (lion_isect_take()) */
	double		andunits;		/* ... and the rows of the AND among them */
	double	   *prefrows;		/* prefrows[j]: the driver's rows that the AND
								 * of the sources order[0 .. j] kept,
								 * weighted as androws */
	int		   *prefhits;		/* ... and the containers in which it kept a
								 * row */
	double	   *prefcount;		/* ... and the rows it kept, unweighted */
	double		scale;			/* the share of the driver's rows the
								 * containers sampled from stand for: 1 but
								 * where the others' absence ruled the rest
								 * out (lion_isect_by_rank()) */
	double		walkedrows;		/* the driver's rows, counted by a walk of
								 * it, or 0 */
	uint32		first;			/* the first container sampled */
	double		end;			/* the key past the last one's stratum */
} LionIsectSample;

static bool
lion_isect_spent(const LionIsectSample *s)
{
	return lion_isect_buffers() - s->start > s->budget;
}

/*
 * The members of container c on the heap blocks of `blocks`, a
 * lion_container_block_mask() of c's or a part of it.
 */
static double
lion_isect_block_members(const LionContainer *c, uint64 blocks)
{
	double		members = 0;
	int			b;

	if (blocks == lion_container_block_mask(c))
		return (double) lion_container_cardinality(c);
	for (b = 0; b < LION_BLOCKS_PER_CONTAINER; b++)
	{
		if ((blocks & (UINT64CONST(1) << b)) == 0)
			continue;
		members += (double)
			lion_container_range_cardinality(c,
											 (uint16) (b << LION_OFFSET_BITS),
											 (uint16) (((b + 1) << LION_OFFSET_BITS) - 1));
	}
	return members;
}

/*
 * Sample the driver's container c: AND the other sources' containers at its
 * key, the sources of fewer rows first, and add what it came to, both
 * weighted by `weight` - what c stands for of the driver.
 *
 * And what it can rule out (DESIGN.md §29.11, "What a sample can rule out").
 * A sample is of containers, and the rows of one are no independent draws:
 * on a heap loaded in batches a container holds one stretch of rows, and a
 * source whose rows lie in other stretches is absent from it whatever the
 * AND holds elsewhere.  So a container where some source has no container
 * at all is ONE unit of evidence, and stops the AND there, as it always
 * did; one where every source has members is as many units as the driver
 * has members on the heap blocks every source has members on - rows side by
 * side on one page, where the AND, or its absence, is a fact about rows and
 * not about which stretch the sample met - and one at least.  The sources
 * after the one that empties the AND are still sought, for their blocks.
 *
 * And what each PART of it kept: the AND of the sources before each one is
 * made on the way, and what it kept is added up beside the whole's
 * (s->prefrows, s->prefhits), so that a sample in which the whole AND kept
 * too few rows to say how many it holds still measures the longest part of it
 * that kept enough (lion_isect_measure()) - at no seek more.
 */
static void
lion_isect_take(LionIsectSample *s, const LionContainer *c, double weight)
{
	double		members = (double) lion_container_cardinality(c);
	double		survive = members;
	uint64		blocks = lion_container_block_mask(c);
	bool		present = true;
	int			j;

	/* nothing of the driver's to stand for: no sample */
	if (members <= 0 || weight <= 0)
		return;

	memcpy(s->acc, c, lion_container_size(c));
	for (j = 1; j < s->n; j++)
	{
		int			k = s->order[j];
		const LionContainer *cj = NULL;

		if (!s->done[k])
			cj = lion_stream_at(s->st[k], c->ckey);
		if (cj == NULL)
			s->done[k] = true;	/* nothing at or past this key, ever */
		if (cj == NULL || cj->ckey != c->ckey)
		{
			present = false;
			survive = 0;
			break;
		}
		blocks &= lion_container_block_mask(cj);
		if (survive > 0)
		{
			LionContainer *swap;

			survive = (double) lion_container_and(s->acc, cj, s->tmp);
			swap = s->acc;
			s->acc = s->tmp;
			s->tmp = swap;
			if (survive > 0)
			{
				s->prefrows[j] += survive * weight;
				s->prefhits[j]++;
				s->prefcount[j] += survive;
			}
		}
	}
	s->drvrows += members * weight;
	s->androws += survive * weight;
	if (survive > 0)
		s->hits++;
	if (present)
	{
		s->units += Max(lion_isect_block_members(c, blocks), 1.0);
		s->andunits += survive;
	}
	else
		s->units += 1.0;
	s->nsampled++;
}

/*
 * The most the share of the driver's rows that survive can be, at 95%, by
 * the sample's units of evidence: `units` draws of which `hits` were rows of
 * the AND (lion_isect_take()) - Wilson and Hilferty's form of the Poisson's
 * upper limit, (h + 1)(1 - 1/9(h + 1) + z/3 sqrt(h + 1))^3 over the draws:
 * 2.97 for none, the rule of three's 3.00; 4.73 for one (4.74), 6.29 for two
 * (6.30), within 1% from there on.  What a sample cannot put the share
 * below.  lion_isect_upper_count() is the limit itself, of the rows: the
 * most a sample that met `hits` of them can have been expected to meet.
 */
static double
lion_isect_upper_count(double hits)
{
	double		h1 = Max(hits, 0.0) + 1.0;
	double		w = 1.0 - 1.0 / (9.0 * h1) + LION_ISECT_BOUND_Z / (3.0 * sqrt(h1));

	return h1 * w * w * w;
}

static double
lion_isect_upper(double hits, double units)
{
	if (units <= 0.0)
		return 1.0;
	return Min(lion_isect_upper_count(hits) / units, 1.0);
}

/*
 * One pass of the sample over the container keys [lo, hi), in `keys` strata
 * of [lo, end) - end the key past the last the heap is expected to hold -
 * and as many more past end as the heap has grown: the driver's first
 * container at or past each stratum's first key, ANDed with the other
 * sources' containers at its key, both weighted by the stratum's width.  The
 * strata still ahead widen when the samples so far, at what they cost, would
 * not all fit the budget, so that the sample spreads over the heap whatever
 * the seeks cost.  False when a sample would not fit it before
 * LION_ISECT_MIN_KEYS were taken, or the pass met twice as many containers as
 * strata (a heap grown far past rel->pages).
 */
static bool
lion_isect_pass(LionIsectSample *s, double lo, double hi, double end, int keys)
{
	int64		passstart = lion_isect_buffers();
	double		stride = Max((end - lo) / (double) keys, 1.0);
	double		origin = lo;	/* where the strata of this width begin */
	double		target = lo;
	int			taken = 0;

	while (target < hi)
	{
		const LionContainer *c;
		double		next;
		double		per;
		double		affordable;

		if (lion_isect_spent(s))
			return taken >= LION_ISECT_MIN_KEYS;
		c = lion_stream_at(s->st[s->order[0]], (uint32) target);
		if (c == NULL || (double) c->ckey >= hi)
			break;
		if (taken >= 2 * keys)
			return false;

		if (taken == 0)
			s->first = c->ckey;
		lion_isect_take(s, c, stride);
		taken++;

		/*
		 * The next stratum's first key, past this container in any case -
		 * unless the strata left before end would cost more than the budget
		 * has left, at what a sample has cost so far: then those fit it,
		 * wider, with a sample's worth to spare - or, when even that would
		 * leave the pass short of LION_ISECT_MIN_KEYS samples, the probe gives
		 * up now, and what it has not spent is left to the run's other probes
		 * (DESIGN.md §29.11, "Wide filters").  The wider strata begin past
		 * this container, each as wide as the weight its sample is given.
		 */
		next = Max(origin + (floor(((double) c->ckey - origin) / stride) + 1.0) *
				   stride, (double) c->ckey + 1.0);
		per = Max((double) (lion_isect_buffers() - passstart) / taken, 1.0);
		affordable = floor((double) (s->budget -
									 (lion_isect_buffers() - s->start)) / per) - 1.0;
		if (next < end && (end - next) / stride > affordable)
		{
			if (affordable < 1.0 || taken + affordable < LION_ISECT_MIN_KEYS)
				return taken >= LION_ISECT_MIN_KEYS;
			origin = (double) c->ckey + 1.0;
			stride = Max((end - origin) / affordable, 1.0);
			next = origin;
		}
		target = ceil(next);
	}
	s->end = Min(target, hi);
	return true;
}

/* How a driver's containers came to be sampled (lion_isect_by_rank()). */
typedef enum LionIsectRanked
{
	LION_ISECT_UNRANKED,		/* not: the heap's strata it is */
	LION_ISECT_RANKED,			/* by rank among the driver's own */
	LION_ISECT_EVERY,			/* ... every one of them: a count */
	LION_ISECT_OVER				/* the probe's budget ran out */
} LionIsectRanked;

/*
 * Walk the driver - its sets, as its own stream - and keep its container
 * keys, in order, and their members: *nkeys of them, palloc'd, the members
 * in *members.  NULL when it has more than LION_ISECT_WALK_KEYS or reads
 * more than LION_ISECT_WALK_PAGES buffers, or the probe's budget runs out
 * first.
 */
static uint32 *
lion_isect_walk(LionIsectSample *s, int nsets, LionPostingSet *sets,
				LionKeyNode *tree, int *nkeys, double **members)
{
	LionSetStream *st = lion_stream_begin(nsets, sets, tree, false);
	int64		start = lion_isect_buffers();
	uint32	   *keys = (uint32 *) palloc(sizeof(uint32) * LION_ISECT_WALK_KEYS);
	double	   *m = (double *) palloc(sizeof(double) * LION_ISECT_WALK_KEYS);
	const LionContainer *c;
	int			n = 0;

	while ((c = lion_stream_next(st)) != NULL)
	{
		if (n >= LION_ISECT_WALK_KEYS ||
			lion_isect_buffers() - start > LION_ISECT_WALK_PAGES ||
			lion_isect_spent(s))
		{
			n = -1;
			break;
		}
		if (lion_container_cardinality(c) > 0)
		{
			keys[n] = c->ckey;
			m[n++] = (double) lion_container_cardinality(c);
		}
	}
	lion_stream_end(st);
	pfree(st);
	if (n < 0)
	{
		pfree(keys);
		pfree(m);
		return NULL;
	}
	*nkeys = n;
	*members = m;
	return keys;
}

/*
 * Which of the driver's walked containers, keys[0 .. nkeys), every other
 * source has a container at: cand[i] cleared for the others.  The AND has
 * no row in a container some source is absent from, so those are known to
 * be empty of it, not sampled: on a heap loaded in batches the AND of a
 * filter is often confined to a few stretches of the driver's - the rows of
 * one batch that the others' keys pick out - which a sample of the driver's
 * containers meets once or not at all.  The sources are walked along the
 * keys, as a merge seeks them, the fewest rows first, at most
 * LION_ISECT_WALK_PAGES buffers each; false when that or the probe's budget
 * runs out first, and nothing is cleared then.
 */
static bool
lion_isect_presence(LionIsectSample *s, const int *nsets,
					LionPostingSet **sets, LionKeyNode **trees,
					const uint32 *keys, int nkeys, bool *cand)
{
	bool	   *keep = (bool *) palloc(sizeof(bool) * Max(nkeys, 1));
	int			i;
	int			j;

	memcpy(keep, cand, sizeof(bool) * nkeys);
	for (j = 1; j < s->n; j++)
	{
		int			k = s->order[j];
		LionSetStream *st = lion_stream_begin(nsets[k], sets[k], trees[k],
											  false);
		int64		start = lion_isect_buffers();
		bool		over = false;

		for (i = 0; i < nkeys && !over; i++)
		{
			const LionContainer *c;

			if (!keep[i])
				continue;
			if (lion_isect_buffers() - start > LION_ISECT_WALK_PAGES ||
				lion_isect_spent(s))
			{
				over = true;
				break;
			}
			c = lion_stream_at(st, keys[i]);
			if (c == NULL)
			{
				/* nothing at or past this key: none of the rest has one */
				for (; i < nkeys; i++)
					keep[i] = false;
				break;
			}
			if (c->ckey != keys[i] || lion_container_cardinality(c) == 0)
				keep[i] = false;
		}
		lion_stream_end(st);
		pfree(st);
		if (over)
		{
			pfree(keep);
			return false;
		}
	}
	memcpy(cand, keep, sizeof(bool) * nkeys);
	pfree(keep);
	return true;
}

/*
 * Sample the driver at the keys targets[0 .. ntargets), ascending: its first
 * container at or past each, which a target that lands where the last one
 * did does not sample again.  bybytes: each stands for an equal share of the
 * driver's posting BYTES, the first container of a leaf or a subtree - which
 * is a container met in proportion to its size, as the one a boundary
 * between equal parts of a sequence of items falls in is - and is weighted
 * by its bytes' inverse, which makes the sample stand for containers again.
 *
 * The targets still ahead thin when the samples so far, at what they cost,
 * would not all fit the budget: every k-th of them, evenly, so that the
 * sample still spreads over the driver's ranks, with a sample's worth of the
 * budget to spare (lion_isect_pass() widens its strata so).  False when the
 * budget ran out, or would before LION_ISECT_MIN_KEYS were taken.
 */
static bool
lion_isect_sample_at(LionIsectSample *s, const uint32 *targets, int ntargets,
					 bool bybytes)
{
	int64		start = lion_isect_buffers();
	bool		any = false;
	uint32		last = 0;
	double		pos = 0.0;
	double		step = 1.0;
	int			taken = 0;

	while (pos < (double) ntargets)
	{
		const LionContainer *c;
		double		per;
		double		affordable;
		double		ahead;

		if (lion_isect_spent(s))
			return false;
		c = lion_stream_at(s->st[s->order[0]], targets[(int) pos]);
		if (c == NULL)
			break;
		pos += step;
		if (any && c->ckey == last)
			continue;
		if (!any)
			s->first = c->ckey;
		any = true;
		last = c->ckey;
		lion_isect_take(s, c,
						bybytes ?
						1.0 / (double) (lion_container_size(c) + sizeof(ItemIdData)) :
						1.0);
		taken++;

		per = Max((double) (lion_isect_buffers() - start) / taken, 1.0);
		affordable = floor((double) (s->budget -
									 (lion_isect_buffers() - s->start)) / per) - 1.0;
		ahead = ceil(((double) ntargets - pos) / step);
		if (ahead > affordable)
		{
			if (affordable < 1.0 || taken + affordable < LION_ISECT_MIN_KEYS)
				return false;
			step = ((double) ntargets - pos) / affordable;
		}
	}
	return true;
}

/*
 * Is a source's tree its sets' union - one set, or an OR of sets - rather
 * than a multi-key query's expression over them?
 */
static bool
lion_isect_tree_is_union(const LionKeyNode *tree)
{
	int			i;

	if (tree == NULL || tree->kind == LION_KN_KEY)
		return true;
	if (tree->kind != LION_KN_OR)
		return false;
	for (i = 0; i < tree->nargs; i++)
	{
		if (tree->args[i] == NULL || tree->args[i]->kind != LION_KN_KEY)
			return false;
	}
	return true;
}

/*
 * Sample the driver by the RANK of its containers among its own (DESIGN.md
 * §29.11, "A sample of the driver's own rows"), keys of them: what the
 * heap's strata cannot do where the driver's rows lie in stretches with
 * other rows between - a stratum that begins between two stretches samples
 * the first container of the next, and on a heap loaded in batches the
 * first rows of a batch are no sample of its rows.  Two ways:
 *
 *	- a driver of a few posting leaves is walked once and its containers'
 *	  keys kept (lion_isect_walk()), those some other source has no
 *	  container at ruled out - the AND has no row there
 *	  (lion_isect_presence()) - and the rest sampled every n/keys-th, each
 *	  standing for its share of them and they for theirs of the driver's
 *	  rows (s->scale); or every one when there are no more than `whole`, as
 *	  many as the other sources' sets can be sought at within the probe's
 *	  seeks: then the AND is counted, not sampled;
 *	- a single set of more, which its posting tree's separators divide into
 *	  leaves of about equal bytes (lion_posting_separators()), at keys of
 *	  them spread evenly, each weighted by its bytes' inverse.
 *
 * LION_ISECT_UNRANKED, nothing sampled, for a driver neither can rank: a
 * list or a multi-key query too large to walk.
 */
static LionIsectRanked
lion_isect_by_rank(LionIsectSample *s, Relation index, const int *allnsets,
				   LionPostingSet **allsets, LionKeyNode **alltrees, int keys,
				   int whole)
{
	int			nsets = allnsets[s->order[0]];
	LionPostingSet *sets = allsets[s->order[0]];
	LionKeyNode *tree = alltrees[s->order[0]];
	uint32	   *seps = NULL;
	uint32	   *targets;
	int			nseps = 0;
	int			found = 0;
	int			single = -1;
	bool		large = false;
	int			i;

	for (i = 0; i < nsets; i++)
	{
		if (sets[i].found)
		{
			found++;
			single = i;
		}
	}

	/* a single posting tree: its separators, where it has an internal level */
	if (lion_isect_tree_is_union(tree) && found == 1 &&
		!sets[single].is_inline && sets[single].mat == NULL)
	{
		int			level = 0;
		int			pages = 0;

		seps = lion_posting_separators(index, sets[single].head, keys,
									   LION_ISECT_SEPARATOR_PAGES, &nseps,
									   &level, &pages);
		large = (seps != NULL &&
				 (level > 1 || nseps > LION_ISECT_WALK_PAGES));
		if (lion_isect_spent(s))
			return LION_ISECT_OVER;
	}

	if (!large)
	{
		int			nkeys = 0;
		double	   *members = NULL;
		uint32	   *walked = lion_isect_walk(s, nsets, sets, tree, &nkeys,
											 &members);

		if (walked == NULL && lion_isect_spent(s))
			return LION_ISECT_OVER;
		if (walked != NULL)
		{
			bool	   *cand = (bool *) palloc(sizeof(bool) * Max(nkeys, 1));
			uint32	   *candkeys = (uint32 *) palloc(sizeof(uint32) *
													 Max(nkeys, 1));
			double		all = 0;
			double		within = 0;
			int			ncand = 0;
			int			ntargets;

			/* the containers the AND can have rows in: every source's */
			for (i = 0; i < nkeys; i++)
				cand[i] = true;
			if (!lion_isect_presence(s, allnsets, allsets, alltrees, walked,
									 nkeys, cand) &&
				lion_isect_spent(s))
				return LION_ISECT_OVER;
			for (i = 0; i < nkeys; i++)
			{
				all += members[i];
				if (cand[i])
				{
					within += members[i];
					candkeys[ncand++] = walked[i];
				}
			}
			s->walkedrows = all;

			/*
			 * Every one of them, when the others can be sought at each within
			 * the probe's seeks: a count.  Else every n/keys-th, from the
			 * middle of the first n/keys, standing for its share of them -
			 * and they for theirs of the driver's rows.
			 */
			ntargets = (ncand <= whole) ? ncand : Min(ncand, keys);
			targets = (uint32 *) palloc(sizeof(uint32) * Max(ntargets, 1));
			for (i = 0; i < ntargets; i++)
				targets[i] = candkeys[(int) (((double) i + 0.5) *
											 (double) ncand / (double) ntargets)];
			if (ntargets < ncand)
			{
				for (i = 0; i < s->n; i++)
					lion_stream_far(s->st[i]);
			}
			if (!lion_isect_sample_at(s, targets, ntargets, false))
				return LION_ISECT_OVER;
			if (within <= 0)
			{
				/* no container of every source: the AND is empty */
				s->drvrows = all;
				return LION_ISECT_EVERY;
			}

			/*
			 * The containers sampled are the ones walked, unless the index
			 * changed between the walk and the sample: then nothing it
			 * measured is a count, and a driver met empty is not measured.
			 */
			if (s->drvrows <= 0)
				return LION_ISECT_OVER;
			s->scale = within / all;
			s->drvrows /= s->scale;
			return (ntargets == ncand && s->nsampled == ntargets) ?
				LION_ISECT_EVERY : LION_ISECT_RANKED;
		}
	}

	/* the separators: keys of them, spread evenly, or every one of fewer */
	if (seps != NULL && nseps >= LION_ISECT_MIN_KEYS)
	{
		int			ntargets = Min(nseps, keys);

		targets = (uint32 *) palloc(sizeof(uint32) * ntargets);
		for (i = 0; i < ntargets; i++)
			targets[i] = seps[(int) (((double) i + 0.5) *
									 (double) nseps / (double) ntargets)];
		for (i = 0; i < s->n; i++)
			lion_stream_far(s->st[i]);
		if (!lion_isect_sample_at(s, targets, ntargets, true))
			return LION_ISECT_OVER;
		return LION_ISECT_RANKED;
	}
	return LION_ISECT_UNRANKED;
}

/*
 * Measure the AND of the n clauses: the share of the driver's rows that
 * survive it at the sampled containers, times the driver's rows - its
 * sets' exact count when that is its rows (one set, or an IN list's disjoint
 * entries), core's estimate of it otherwise (a multi-key query's keys
 * overlap).  At most `budget` buffer accesses; false when a bound was reached
 * first.  What it measured goes in *res: res->sel; res->hits, how many of the
 * containers sampled kept a row of the AND - INT_MAX when every container of
 * the driver was one, or of the heap, and the AND is counted rather than
 * sampled; res->ub the most the AND can hold by what the sample can rule out
 * (lion_isect_upper()), and res->lb the least: the rows of the AND the sample
 * met, which it holds whatever the rest of it is - both res->sel itself for a
 * count.  And res->part, the longest part of the AND - its first sources, in
 * the order it is made - that LION_ISECT_MIN_HITS of the containers sampled
 * kept a row of, with the share res->partsel of the rows it holds: what a
 * sample that met the whole AND in fewer says (lion_isect_factor_run()).
 *
 * The clauses come narrowest first (lion_isect_measured_part()), and one
 * whose sets would take the sources past LION_ISECT_MAX_SETS - a multi-key
 * query of more keys than its clause showed - is left out: used[i] says which
 * were measured, every one for an AND found empty.
 */
static bool
lion_isect_measure(RelOptInfo *rel, IndexOptInfo *idx, int n,
				   LionIsectClause *cl, int64 budget, bool *used,
				   LionIsectResult *res)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext,
											  "lion intersection probe",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContext oldcxt = MemoryContextSwitchTo(cxt);
	int64		buffers = lion_isect_buffers();
	Relation	index = index_open(idx->indexoid, NoLock);
	LionPostingSet **sets = (LionPostingSet **)
		palloc0(sizeof(LionPostingSet *) * n);
	int		   *nsets = (int *) palloc0(sizeof(int) * n);
	LionKeyNode **trees = (LionKeyNode **) palloc0(sizeof(LionKeyNode *) * n);
	double	   *ntids = (double *) palloc0(sizeof(double) * n);
	bool	   *exact = (bool *) palloc0(sizeof(bool) * n);
	int		   *src = (int *) palloc0(sizeof(int) * n);
	LionIsectSample s;
	double		tuples = Max(rel->tuples, 1.0);
	double		ckeys = Max(ceil((double) rel->pages / LION_BLOCKS_PER_CONTAINER),
							1.0);
	double		stride = 1.0;
	LionIsectRanked ranked = LION_ISECT_UNRANKED;
	int			keys = LION_ISECT_KEYS;
	int			totalsets = 0;
	int			m = 0;
	int			driver = 0;
	bool		ok = true;
	bool		empty = false;
	int			i;
	int			j;

	memset(&s, 0, sizeof(s));
	s.start = buffers;
	s.budget = budget;
	s.scale = 1.0;
	memset(res, 0, sizeof(*res));
	res->ub = 1.0;
	for (i = 0; i < n; i++)
		used[i] = false;

	/*
	 * ---- locate every clause's sets, and let go of their pins ----
	 *
	 * Each measured clause is a source of the sample, sources[0 .. m) in
	 * the arrays above, src[] its clause.
	 */
	for (i = 0; i < n && ok; i++)
	{
		LionState  *state = lion_index_column_state(index, cl[i].col);
		LionPostingSet *located = NULL;
		LionKeyNode *tree = NULL;
		int			nlocated = 0;
		bool		nomatch = false;
		bool		found;

		if (lion_isect_spent(&s))
		{
			ok = false;
			break;
		}
		found = lion_scankey_sets(index, &cl[i].skey, &nlocated, &located,
								  &tree, &nomatch);
		if (!found || totalsets + nlocated > LION_ISECT_MAX_SETS)
		{
			/* not measured: its sets let go of here, the AND without it */
			for (j = 0; j < nlocated; j++)
				lion_posting_set_release(&located[j]);
			if (!found)
				ok = false;
			else if (nomatch)
				empty = true;
			continue;
		}
		if (nomatch)
			empty = true;
		sets[m] = located;
		nsets[m] = nlocated;
		trees[m] = tree;
		for (j = 0; j < nlocated; j++)
		{
			if (!located[j].found)
				continue;
			lion_posting_set_unpin(&located[j]);
			ntids[m] += (double) located[j].ntids;
		}
		exact[m] = (nlocated == 1 || !state->multikey);
		src[m] = i;
		used[i] = true;
		totalsets += nlocated;
		m++;
	}
	if (ok && empty)
	{
		/* an AND some clause of it empties is measured whole: no row */
		for (i = 0; i < n; i++)
			used[i] = true;
	}
	else if (m < 2)
		ok = false;

	s.n = m;
	s.st = (LionSetStream **) palloc0(sizeof(LionSetStream *) * Max(m, 1));
	s.order = (int *) palloc0(sizeof(int) * Max(m, 1));
	s.done = (bool *) palloc0(sizeof(bool) * Max(m, 1));
	s.prefrows = (double *) palloc0(sizeof(double) * Max(m, 1));
	s.prefhits = (int *) palloc0(sizeof(int) * Max(m, 1));
	s.prefcount = (double *) palloc0(sizeof(double) * Max(m, 1));
	s.acc = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	s.tmp = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);

	if (ok && !empty)
	{
		/* the driver is the set of the fewest rows; the others by their rows */
		for (i = 0; i < m; i++)
		{
			int			k;

			for (k = i; k > 0 && ntids[s.order[k - 1]] > ntids[i]; k--)
				s.order[k] = s.order[k - 1];
			s.order[k] = i;
			s.st[i] = lion_stream_begin(nsets[i], sets[i], trees[i], false);
		}
		driver = s.order[0];

		/* ---- the driver's own containers, by their rank among them ---- */
		ranked = lion_isect_by_rank(&s, index, nsets, sets, trees, keys,
									Max(keys, LION_ISECT_SEEKS /
										Max(totalsets - nsets[driver], 1)));
		if (ranked == LION_ISECT_OVER)
			ok = false;
		else if (ranked == LION_ISECT_UNRANKED)
		{
			/*
			 * ... or, where they have no rank to sample by, the heap's
			 * container keys in `keys` strata, and the driver's first
			 * container in each: every container, when there are no more keys
			 * than strata.  A heap grown past rel->pages makes more strata
			 * than that, up to twice as many before the probe gives up.
			 * Strata of several container keys each are as many posting
			 * leaves apart, and a set is sought there by a descent of its
			 * tree, not by stepping right (lion_stream_far()).
			 */
			stride = Max(ckeys / (double) keys, 1.0);
			if (stride > 1.0)
			{
				for (i = 0; i < m; i++)
					lion_stream_far(s.st[i]);
			}
			ok = lion_isect_pass(&s, 0.0, LION_ISECT_KEY_END, ckeys, keys);

			/*
			 * A driver whose rows lie in a stretch of the heap a few strata
			 * wide - a hidden attribute that follows the order the rows came
			 * in - was sampled at a few keys only.  Its keys lie in [first,
			 * the end of the last one's stratum): sample that stretch again,
			 * in strata of its own, and take the second pass when it
			 * completes.
			 */
			if (ok && s.nsampled > 0 && s.nsampled < LION_ISECT_MIN_KEYS &&
				stride > 1.0)
			{
				LionIsectSample again = s;
				double		lo = (double) s.first;
				double		hi = s.end;

				for (i = 0; i < m; i++)
				{
					lion_stream_end(s.st[i]);
					s.st[i] = lion_stream_begin(nsets[i], sets[i], trees[i],
												false);
					lion_stream_far(s.st[i]);
					s.done[i] = false;
				}
				again.drvrows = 0;
				again.androws = 0;
				again.nsampled = 0;
				again.hits = 0;
				again.units = 0;
				again.andunits = 0;
				again.prefrows = (double *) palloc0(sizeof(double) * m);
				again.prefhits = (int *) palloc0(sizeof(int) * m);
				again.prefcount = (double *) palloc0(sizeof(double) * m);
				if (lion_isect_pass(&again, lo, hi, hi, keys))
					s = again;
			}
		}

		for (i = 0; i < m; i++)
			lion_stream_end(s.st[i]);
	}

	if (ok)
	{
		double		rows = 0;
		double		drows = (s.walkedrows > 0) ? s.walkedrows :
			exact[driver] ? ntids[driver] : cl[src[driver]].sel * tuples;
		bool		counted = empty || ranked == LION_ISECT_EVERY ||
			(ranked == LION_ISECT_UNRANKED && stride <= 1.0);

		if (!empty && s.drvrows > 0)
			rows = s.androws / s.drvrows * drows;
		res->sel = Min(rows / tuples, 1.0);
		res->ub = counted ? res->sel :
			Min(lion_isect_upper(s.andunits, s.units) * s.scale * drows / tuples,
				1.0);
		res->lb = counted ? res->sel : Min(s.andunits / tuples, res->sel);
		res->hits = counted ? INT_MAX : s.hits;
		res->androws = s.andunits;

		/* the longest part of the AND - two sources or more - met enough */
		for (j = m - 2; !counted && s.drvrows > 0 && j >= 1; j--)
		{
			if (s.prefhits[j] >= LION_ISECT_MIN_HITS)
			{
				res->partn = j + 1;
				res->part = (int *) MemoryContextAlloc(oldcxt,
													   sizeof(int) * (j + 1));
				for (i = 0; i <= j; i++)
					res->part[i] = src[s.order[i]];
				res->partsel = Min(s.prefrows[j] / s.drvrows * drows / tuples,
								   1.0);
				res->partrows = s.prefcount[j];
				break;
			}
		}
	}
	res->used = lion_isect_buffers() - buffers;
	elog(DEBUG2, "lion: intersection probe of %d of %d clauses (%d sets) on \"%s\": %s, %d of the driver's containers, %d with a row of the AND, %.0f units of evidence with %.0f rows of it: %.6g, at most %.6g; %d of the clauses in %d of them: %.6g; in %lld buffers of %lld",
		 m, n, totalsets, RelationGetRelationName(index),
		 !ok ? "over budget" : empty ? "a clause matches nothing" :
		 ranked == LION_ISECT_EVERY ? "every one" :
		 ranked == LION_ISECT_RANKED ? "by rank" :
		 stride <= 1.0 ? "every heap key" : "by heap key",
		 s.nsampled, s.hits, s.units, s.andunits,
		 ok ? res->sel : -1.0, ok ? res->ub : -1.0,
		 res->partn, res->partn > 0 ? s.prefhits[res->partn - 1] : 0,
		 res->partn > 0 ? res->partsel : -1.0,
		 (long long) res->used, (long long) budget);

	for (i = 0; i < m; i++)
		for (j = 0; j < nsets[i]; j++)
			lion_posting_set_release(&sets[i][j]);
	index_close(index, NoLock);
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);
	return ok;
}

/*
 * May the probe count rows of rel by these clauses' columns?  Only when the
 * user may read every row of them - SELECT on the table or on each column,
 * and no security barrier view's or row-level policy's quals - as core asks
 * before a statistic of a column reaches a user's function
 * (all_rows_selectable()): the probe's count is of the rows themselves,
 * those the user may not see included, and an estimate is shown by EXPLAIN.
 * An inheritance child is asked about as its parent is, the table the query
 * names; a column there is not the child's, so only the whole table will
 * do.  An expression key is no column: only the whole table will do either.
 */
static bool
lion_isect_readable(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *idx,
					int n, const LionIsectClause *cl)
{
	Index		varno = rel->relid;
	Oid			userid = OidIsValid(rel->userid) ? rel->userid : GetUserId();
	bool		child = false;
	RangeTblEntry *rte;
	int			i;

	while (root->append_rel_array != NULL &&
		   root->append_rel_array[varno] != NULL &&
		   planner_rt_fetch(root->append_rel_array[varno]->parent_relid,
							root)->rtekind == RTE_RELATION)
	{
		varno = root->append_rel_array[varno]->parent_relid;
		child = true;
	}
	rte = planner_rt_fetch(varno, root);
	if (rte->securityQuals != NIL)
		return false;
	if (pg_class_aclcheck(rte->relid, userid, ACL_SELECT) == ACLCHECK_OK)
		return true;
	if (child)
		return false;
	for (i = 0; i < n; i++)
	{
		AttrNumber	attno = idx->indexkeys[cl[i].col - 1];

		if (attno <= 0 ||
			pg_attribute_aclcheck(rte->relid, attno, userid,
								  ACL_SELECT) != ACLCHECK_OK)
			return false;
	}
	return true;
}

/*
 * Is the cached conjunction e this one: the same clauses of table relid, in
 * any order, on any of its lion indexes?
 */
static bool
lion_isect_same(LionIsect *e, Oid relid, int n, LionIsectClause *cl)
{
	bool	   *used;
	bool		same = true;
	int			i;
	int			j;

	if (e->relid != relid || e->n != n)
		return false;
	used = (bool *) palloc0(sizeof(bool) * n);
	for (i = 0; i < n && same; i++)
	{
		Node	   *bare = lion_isect_bare(cl[i].clause);

		same = false;
		for (j = 0; j < n && !same; j++)
		{
			if (!used[j] && e->attnos[j] == cl[i].attno &&
				equal(e->clauses[j], bare))
				used[j] = same = true;
		}
	}
	pfree(used);
	return same;
}

/* Is every clause of e among the n clauses cl (of e's table)? */
static bool
lion_isect_holds(LionIsect *e, int n, LionIsectClause *cl)
{
	int			i;
	int			j;

	for (j = 0; j < e->n; j++)
	{
		bool		found = false;

		for (i = 0; i < n && !found; i++)
			found = (cl[i].attno == e->attnos[j] &&
					 equal(lion_isect_bare(cl[i].clause), e->clauses[j]));
		if (!found)
			return false;
	}
	return true;
}

/* ... and every one of the n clauses among e's? */
static bool
lion_isect_within(LionIsect *e, int n, LionIsectClause *cl)
{
	int			i;
	int			j;

	for (i = 0; i < n; i++)
	{
		bool		found = false;

		for (j = 0; j < e->n && !found; j++)
			found = (cl[i].attno == e->attnos[j] &&
					 equal(lion_isect_bare(cl[i].clause), e->clauses[j]));
		if (!found)
			return false;
	}
	return true;
}

/*
 * What a conjunction the run could not measure is taken to be, from the ones
 * it did measure of the same table (relid), on any of its lion indexes:
 * between the largest of them that is a part of it and the smallest it is a
 * part of, geometrically in the number of clauses - the factor that grows
 * from the part's to the whole's as clauses are added, each clause its share
 * of it.  With a part only, the part's: its other clauses independent of
 * those, as core takes them and as the probe's factor takes a range or a
 * clause it has no value for (lion_isect_factor()); with a whole only, from 1
 * at one clause; with neither, 1.  A filter's conjunction and the ones its
 * paths answer share most of their clauses: on the repro of DESIGN.md
 * §29.11, "Wide filters", the five clauses beside four lists measured 3.1,
 * all nine 186, and the five with one list 9.1, where this says 8.6.
 */
static double
lion_isect_part_factor(LionProbeCache *cache, Oid relid, int n,
					   LionIsectClause *cl)
{
	LionIsect  *part = NULL;
	LionIsect  *whole = NULL;
	double		lo;
	double		lon;
	ListCell   *lc;

	foreach(lc, cache->isects)
	{
		LionIsect  *e = (LionIsect *) lfirst(lc);

		if (!e->measured || e->relid != relid)
			continue;
		if (e->n < n && (part == NULL || e->n > part->n) &&
			lion_isect_holds(e, n, cl))
			part = e;
		else if (e->n > n && (whole == NULL || e->n < whole->n) &&
				 lion_isect_within(e, n, cl))
			whole = e;
	}
	lo = (part != NULL) ? part->factor : 1.0;
	lon = (part != NULL) ? (double) part->n : 1.0;
	if (whole == NULL || whole->factor <= 0.0 || lo <= 0.0)
		return lo;
	return lo * pow(whole->factor / lo,
					((double) n - lon) / ((double) whole->n - lon));
}

/*
 * The IN list that an arm of one of rel's ORs of equalities is a part of
 * (lion_or_as_array()), or NULL: a clause that is itself one of rel's
 * restriction clauses, or no such arm.  Core builds a path per arm of an OR
 * for a BitmapOr, each ANDing the arm with the clauses beside the OR, and
 * lion's estimate of each is its share of the conjunction with the whole list
 * in its place (lion_isect_factor_run()).
 */
static Node *
lion_or_arm_list(RelOptInfo *rel, Node *clause)
{
	ListCell   *lc;

	foreach(lc, rel->baserestrictinfo)
	{
		if (equal(((RestrictInfo *) lfirst(lc))->clause, clause))
			return NULL;
	}
	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
		ListCell   *la;

		if (!is_orclause(rinfo->clause))
			continue;
		foreach(la, ((BoolExpr *) rinfo->clause)->args)
		{
			if (equal(lfirst(la), clause))
				return lion_or_as_array((Node *) rinfo);
		}
	}
	return NULL;
}

/*
 * The set clauses among rel's own restriction clauses that lion index idx
 * answers - each at the first key column it is a set clause of
 * (lion_isect_scankey()), the RestrictInfo itself in *clauses - and how many
 * there are.  *cols and *clauses are palloc'd.
 */
static int
lion_isect_rel_clauses(RelOptInfo *rel, IndexOptInfo *idx, AttrNumber **cols,
					   Node ***clauses)
{
	int			n = 0;
	ListCell   *lc;

	*cols = (AttrNumber *) palloc(sizeof(AttrNumber) *
								  Max(list_length(rel->baserestrictinfo), 1));
	*clauses = (Node **) palloc(sizeof(Node *) *
								Max(list_length(rel->baserestrictinfo), 1));
	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
		ScanKeyData skey;
		int			c;

		if (rinfo->pseudoconstant)
			continue;
		for (c = 0; c < idx->nkeycolumns; c++)
		{
			if (lion_isect_scankey(idx, c, (Node *) rinfo, &skey))
			{
				(*cols)[n] = (AttrNumber) (c + 1);
				(*clauses)[n++] = (Node *) rinfo;
				break;
			}
		}
	}
	return n;
}

/*
 * The part of the conjunction cl[0 .. n) a probe measures, copied to mcl[]
 * narrowest first, and how many clauses it is: every one, unless their sets
 * are more than LION_ISECT_MAX_SETS, or more than `budget` affords
 * LION_ISECT_MIN_KEYS samples of at LION_ISECT_SEEK_BUFFERS a seek; then the
 * clauses of the fewest sets that are not, the more selective of two of as
 * many first, and every other is left to core's estimate, independent of
 * those, as core takes it and as the probe's factor takes a clause it has no
 * value for (lion_isect_factor()).  The clauses of fewer sets are the
 * cheaper to seek at each sample, and the more selective ones decide more of
 * what the AND keeps; a wide list is the dearest and the least selective.
 */
static int
lion_isect_measured_part(int n, const LionIsectClause *cl, int64 budget,
						 LionIsectClause *mcl)
{
	int		   *nsets = (int *) palloc(sizeof(int) * Max(n, 1));
	int		   *order = (int *) palloc(sizeof(int) * Max(n, 1));
	int			total = 0;
	int			m = 0;
	int			i;

	for (i = 0; i < n; i++)
	{
		int			k;

		nsets[i] = lion_isect_clause_sets(&cl[i].skey);
		for (k = i; k > 0 &&
			 (nsets[order[k - 1]] > nsets[i] ||
			  (nsets[order[k - 1]] == nsets[i] && cl[order[k - 1]].sel > cl[i].sel));
			 k--)
			order[k] = order[k - 1];
		order[k] = i;
	}
	for (i = 0; i < n; i++)
	{
		int			k = order[i];

		if (total + nsets[k] > LION_ISECT_MAX_SETS ||
			(double) (total + nsets[k]) * LION_ISECT_MIN_KEYS *
			LION_ISECT_SEEK_BUFFERS > (double) budget)
			break;
		total += nsets[k];
		mcl[m++] = cl[k];
	}
	pfree(nsets);
	pfree(order);
	return m;
}

/* Core's estimate of the AND of the clauses cl[idx[0 .. n)], or of cl[0 .. n). */
static Selectivity
lion_isect_core_sel(PlannerInfo *root, const LionIsectClause *cl,
					const int *idx, int n)
{
	List	   *group = NIL;
	Selectivity sel;
	int			i;

	for (i = 0; i < n; i++)
		group = lappend(group, cl[idx != NULL ? idx[i] : i].clause);
	sel = clauselist_selectivity(root, group, 0, JOIN_INNER, NULL);
	list_free(group);
	return sel;
}

/*
 * Remember a conjunction of this run - the n clauses cl[] of table relid,
 * bare copies in the cache's memory - and what its estimate is to be
 * multiplied by.
 */
static LionIsect *
lion_isect_remember(LionProbeCache *cache, Oid relid, int n,
					const LionIsectClause *cl, double factor, bool measured)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(cache->cxt);
	LionIsect  *e = (LionIsect *) palloc(sizeof(LionIsect));
	int			i;

	e->relid = relid;
	e->n = n;
	e->attnos = (AttrNumber *) palloc(sizeof(AttrNumber) * n);
	e->clauses = (Node **) palloc(sizeof(Node *) * n);
	for (i = 0; i < n; i++)
	{
		e->attnos[i] = cl[i].attno;
		e->clauses[i] = (Node *) copyObject(lion_isect_bare(cl[i].clause));
	}
	e->factor = factor;
	e->measured = measured;
	cache->isects = lappend(cache->isects, e);
	MemoryContextSwitchTo(oldcxt);
	return e;
}

/*
 * Probe the conjunction of the ncl clauses cl[] - core's estimate of it
 * coresel, its smallest clause's minsel - remember what it came to in the
 * run's cache, and return its factor.
 *
 * What is measured is the part of it lion_isect_measured_part() picks,
 * every clause unless the conjunction is too wide for a probe or for what
 * the run has left; its factor - what the probe found of the part, over
 * core's estimate of the part - is the whole's, the rest of the clauses taken
 * as core takes them (lion_isect_factor()), and the part is remembered as
 * measured too.  Where nothing can be - the run's probes or its budget are
 * spent, or the probe gives up - the conjunction is taken to be as the parts
 * of it the run did measure say (lion_isect_part_factor()), and is remembered
 * so only when a probe of it gave up.
 */
static double
lion_isect_probe(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *idx,
				 LionProbeCache *cache, int ncl, LionIsectClause *cl,
				 Selectivity coresel, Selectivity minsel)
{
	Oid			relid = planner_rt_fetch(rel->relid, root)->relid;
	LionIsectClause *mcl = (LionIsectClause *) palloc(sizeof(LionIsectClause) * ncl);
	bool	   *used = (bool *) palloc0(sizeof(bool) * ncl);
	LionIsectResult res;
	Selectivity msel = coresel;
	Selectivity mminsel = minsel;
	double		probesel = 0.0;
	double		factor = 1.0;
	int64		left;
	int			esets = 0;
	int			m = 0;
	int			nused = 0;
	bool		measured = false;
	bool		raised = false;
	int			i;

	memset(&res, 0, sizeof(res));
	for (i = 0; i < ncl; i++)
		esets += lion_isect_clause_sets(&cl[i].skey);
	cache->isectwidest = Max(cache->isectwidest,
							 Min(esets, LION_ISECT_MAX_SETS));
	left = lion_isect_run_budget(cache) - cache->isectbuffers;
	if (cache->nisectprobes < LION_ISECT_RUN_PROBES && left > 0)
		m = lion_isect_measured_part(ncl, cl, left, mcl);
	if (m < 2)
	{
		/* nothing the run has left can measure: remembered as nothing */
		pfree(mcl);
		pfree(used);
		return lion_isect_part_factor(cache, relid, ncl, cl);
	}
	cache->nisectprobes++;
	measured = lion_isect_measure(rel, idx, m, mcl, left, used, &res);
	cache->isectbuffers += res.used;

	/*
	 * The clauses measured: core's estimate of them, which the factor is
	 * taken against, the clauses left out being independent of them.
	 */
	if (measured)
	{
		int		   *idxs = (int *) palloc(sizeof(int) * m);

		mminsel = 1.0;
		for (i = 0; i < m; i++)
		{
			if (!used[i])
				continue;
			idxs[nused++] = i;
			mminsel = Min(mminsel, mcl[i].sel);
		}
		if (nused < ncl)
			msel = lion_isect_core_sel(root, mcl, idxs, nused);
		pfree(idxs);
		if (msel <= 0.0)
			measured = false;
	}

	/*
	 * A sample in which fewer than LION_ISECT_MIN_HITS containers kept a
	 * row of the AND says little of how many rows it holds: on a heap whose
	 * rows lie in stretches of their hidden group, a wide filter's sample
	 * meets the few stretches that hold its rows once or not at all, and one
	 * that does stands for its whole stratum - many times the rows, or a
	 * fraction of them (DESIGN.md §29.11, "Wide filters").  Such a
	 * measure raises an estimate as far as the rows of the AND it met, which
	 * the AND holds whatever the rest of it is, or as far as the longest
	 * part of the AND that enough of the containers did keep a row of says
	 * - its factor, the rest of the clauses taken as independent of it, as a
	 * part is taken (lion_isect_part_factor()), and no further than the
	 * sample can rule out - whichever is more; to less than LION_ISECT_ERROR
	 * times core's it is no measure.
	 *
	 * And one that lowers it lowers it only as far as the sample can rule
	 * out (DESIGN.md §29.11, "What a sample can rule out"): to the most the
	 * AND can hold by the sample's units of evidence, at 95%, and not below.
	 * A count of every container of the driver (hits INT_MAX) is no sample,
	 * and stands.
	 */
	if (measured && res.hits < LION_ISECT_MIN_HITS)
	{
		double		hi = Min(res.ub, mminsel);
		double		lo = res.lb;

		if (res.partn >= 2 && res.partrows > 0.0)
		{
			Selectivity psel = lion_isect_core_sel(root, mcl, res.part,
												   res.partn);
			double		keep;

			/*
			 * The share of the part's rows that keep a row of the whole:
			 * core's estimate of the rest's, as far as the sample allows - no
			 * more of the part's rows it met than the rows of the whole it
			 * met can have come from, at 95% (lion_isect_upper_count()).  A
			 * rest whose rows lie away from the part's, which core's product
			 * cannot see either, raises nothing then.
			 */
			if (psel > 0.0)
			{
				keep = Min(Min(msel / psel, 1.0),
						   lion_isect_upper_count(res.androws) / res.partrows);
				lo = Max(lo, Min(res.partsel * keep, hi));
			}
		}
		lo = Min(lo, mminsel);
		if (lo >= msel * LION_ISECT_ERROR)
		{
			probesel = lo;
			raised = true;
		}
		else if (Min(res.sel, mminsel) >= msel * LION_ISECT_ERROR)
			measured = false;
	}
	if (measured)
	{
		if (!raised)
		{
			probesel = Min(res.sel, mminsel);
			if (probesel < msel)
				probesel = Min(Max(probesel, Min(res.ub, mminsel)), msel);
		}
		if (probesel >= msel * LION_ISECT_ERROR ||
			probesel * LION_ISECT_ERROR <= msel)
			factor = probesel / msel;
	}
	else
		factor = lion_isect_part_factor(cache, relid, ncl, cl);

	elog(DEBUG2, "lion: intersection probe's factor for %d clauses: %.6g, %s%s; %d of them measured, core's estimate of those %.6g, the probe's %.6g",
		 ncl, factor,
		 !measured ? "not measured: as the run's parts of it say" :
		 raised ? "raised by a sample of few containers with a row of the AND" :
		 factor == 1.0 ? "core's estimate stands" : "measured",
		 measured && res.hits == INT_MAX ? " (counted)" : "",
		 measured ? nused : 0, measured ? msel : coresel,
		 measured ? probesel : -1.0);

	(void) lion_isect_remember(cache, relid, ncl, cl, factor,
							   measured && nused == ncl);

	/* ... and the part measured, when it is not the whole */
	if (measured && nused >= 2 && nused < ncl)
	{
		LionIsectClause *pcl = (LionIsectClause *) palloc(sizeof(LionIsectClause) * nused);
		ListCell   *lc;
		bool		known = false;
		int			k = 0;

		for (i = 0; i < m; i++)
		{
			if (used[i])
				pcl[k++] = mcl[i];
		}
		foreach(lc, cache->isects)
		{
			if (lion_isect_same((LionIsect *) lfirst(lc), relid, nused, pcl))
				known = true;
		}
		if (!known)
			(void) lion_isect_remember(cache, relid, nused, pcl, factor, true);
		pfree(pcl);
	}
	if (res.part != NULL)
		pfree(res.part);
	pfree(mcl);
	pfree(used);
	return factor;
}

static double lion_isect_factor_run(PlannerInfo *root, RelOptInfo *rel,
									IndexOptInfo *idx, int n,
									const AttrNumber *cols, Node **clauses,
									bool relfirst);

/*
 * What lion's estimate of the AND of the n clauses - RestrictInfos or bare
 * clauses, each on key column cols[i] (1-based) of lion index idx of rel - is
 * to be multiplied by: 1 unless the intersection probe measured the set
 * clauses among them to be LION_ISECT_ERROR times more or less selective
 * together than core's estimate of them says.  The factor applies to core's
 * estimate of any conjunction that holds those clauses: what else it holds -
 * a range, a clause the probe has no value for - is taken to be independent
 * of them, as core takes it.
 */
double
lion_isect_factor(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *idx,
				  int n, const AttrNumber *cols, Node **clauses)
{
	return lion_isect_factor_run(root, rel, idx, n, cols, clauses, true);
}

/*
 * ... relfirst: before a conjunction not measured yet in this planner run is,
 * the relation's OWN conjunction on idx - every set clause of its
 * restriction clauses that idx answers (lion_isect_rel_clauses()) - is, when
 * it is another one.  That is the AND the count pushdown makes and a scan of
 * every clause makes, and it was the one left without a budget: core prices
 * the paths of subsets first - an index path of the clauses its columns
 * answer, each arm of a BitmapOr with the clauses beside it - and a filter
 * written with ORs had its arms' conjunctions spend the run's budget before
 * the relation's own was asked about (DESIGN.md §29.11, "Wide filters").
 */
static double
lion_isect_factor_run(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *idx,
					  int n, const AttrNumber *cols, Node **clauses,
					  bool relfirst)
{
	LionProbeCache *cache;
	LionIsectClause *cl;
	LionIsect  *e = NULL;
	RangeTblEntry *rte;
	List	   *group = NIL;
	Selectivity coresel;
	Selectivity minsel = 1.0;
	double		factor;
	int			ncl = 0;
	int			i;
	ListCell   *lc;

	/*
	 * No lion index may be read while PostgreSQL 16's old_snapshot_threshold
	 * is set (DESIGN.md §9), and every lion path is priced out then anyway.
	 */
	if (!lion_enable_intersection_probe || n < 2 || root == NULL ||
		root->glob == NULL || rel == NULL || rel->relid == 0 ||
		rel->rtekind != RTE_RELATION || idx == NULL || idx->hypothetical ||
		idx->indpred != NIL || idx->relam != lion_get_am_oid() ||
		lion_old_snapshot_threshold_active())
		return 1.0;
	rte = planner_rt_fetch(rel->relid, root);
	if (rte->rtekind != RTE_RELATION || rte->inh ||
		rte->relkind == RELKIND_PARTITIONED_TABLE)
		return 1.0;

	/*
	 * The clauses it has a value to look up for, each in its canonical form
	 * (lion_canonical_clause()): an OR of equalities is estimated, measured
	 * and remembered as the IN list it spells, so that the two spellings of
	 * one filter are one conjunction here - core estimates the OR as though
	 * its arms could overlap, and the list as the disjoint entries they are.
	 *
	 * And an arm of such an OR (lion_or_arm_list()) - what the path of one
	 * arm of a BitmapOr answers - is the whole list here: the conjunction
	 * measured is the one with the list in the arm's place, and its factor is
	 * applied to core's estimate of the arm's, which is the list's times the
	 * arm's share of it.  The arm's rows are then the list's share, as the
	 * probe found them, and each OR of the filter costs the run one
	 * conjunction to measure however many arms it has - the relation's own,
	 * once the others are lists as well - where each arm used to cost one.
	 */
	cl = (LionIsectClause *) palloc(sizeof(LionIsectClause) * n);
	for (i = 0; i < n; i++)
	{
		Node	   *canon;
		Node	   *list;
		bool		dup = false;
		int			j;

		if (cols[i] < 1 || cols[i] > idx->nkeycolumns ||
			!lion_isect_scankey(idx, cols[i] - 1, clauses[i], &cl[ncl].skey))
			continue;
		canon = lion_canonical_clause(clauses[i]);
		if ((list = lion_or_arm_list(rel, canon)) != NULL &&
			lion_isect_scankey(idx, cols[i] - 1, list, &cl[ncl].skey))
		{
			for (j = 0; j < ncl && !dup; j++)
				dup = (cl[j].col == cols[i] &&
					   equal(lion_isect_bare(cl[j].clause), list));
			if (dup)
				continue;
			canon = list;
		}
		cl[ncl].col = cols[i];
		cl[ncl].attno = idx->indexkeys[cols[i] - 1];
		cl[ncl].clause = (canon == lion_isect_bare(clauses[i])) ?
			clauses[i] : canon;
		cl[ncl].sel = clause_selectivity(root, cl[ncl].clause, 0, JOIN_INNER,
										 NULL);
		minsel = Min(minsel, cl[ncl].sel);
		group = lappend(group, cl[ncl].clause);
		ncl++;
	}

	/*
	 * Where it can matter: the AND holds at most the smallest clause's rows,
	 * so core's estimate can be too small by at most that over it.
	 */
	coresel = (ncl >= 2) ?
		clauselist_selectivity(root, group, 0, JOIN_INNER, NULL) : 0.0;
	if (ncl < 2 || coresel <= 0.0 || coresel * LION_ISECT_ERROR > minsel ||
		!lion_isect_readable(root, rel, idx, ncl, cl))
	{
		list_free(group);
		pfree(cl);
		return 1.0;
	}
	list_free(group);

	cache = lion_probe_cache_for(root);
	foreach(lc, cache->isects)
	{
		if (lion_isect_same((LionIsect *) lfirst(lc), rte->relid, ncl, cl))
		{
			e = (LionIsect *) lfirst(lc);
			break;
		}
	}

	/* the relation's own conjunction first, when this is another one */
	if (e == NULL && relfirst)
	{
		AttrNumber *relcols;
		Node	  **relclauses;
		int			nrel = lion_isect_rel_clauses(rel, idx, &relcols,
												  &relclauses);

		if (nrel >= 2)
		{
			(void) lion_isect_factor_run(root, rel, idx, nrel, relcols,
										 relclauses, false);
			foreach(lc, cache->isects)
			{
				if (lion_isect_same((LionIsect *) lfirst(lc), rte->relid,
									ncl, cl))
				{
					e = (LionIsect *) lfirst(lc);
					break;
				}
			}
		}
		pfree(relcols);
		pfree(relclauses);
	}

	factor = (e != NULL) ? e->factor :
		lion_isect_probe(root, rel, idx, cache, ncl, cl, coresel, minsel);
	pfree(cl);
	return factor;
}

/*
 * The factor for rel's own restriction clauses: those of them that are set
 * clauses of ONE lion index - the one that has the most, two at least - as
 * the count pushdown ANDs them.  The measurement is the table's (LionIsect):
 * the path of another lion index that answers the same clauses takes the
 * same factor, and so agrees with rel->rows.
 */
static double
lion_isect_rel_factor(PlannerInfo *root, RelOptInfo *rel)
{
	Oid			amoid;
	IndexOptInfo *best = NULL;
	AttrNumber *bestcols = NULL;
	Node	  **bestclauses = NULL;
	int			bestn = 0;
	double		factor = 1.0;
	ListCell   *lc;

	if (!lion_enable_intersection_probe ||
		list_length(rel->baserestrictinfo) < 2 ||
		rel->reloptkind == RELOPT_JOINREL || rel->indexlist == NIL)
		return 1.0;

	/*
	 * Every path's cost asks this: each index's arrays are let go of once it
	 * is not the one taken, or once that one is measured, as
	 * lion_isect_factor_run() lets go of its own, rather than piling up in
	 * the planner's context (2026-10 review).
	 */
	amoid = lion_get_am_oid();
	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);
		AttrNumber *cols;
		Node	  **clauses;
		int			n;

		if (idx->relam != amoid || idx->hypothetical || idx->indpred != NIL)
			continue;
		n = lion_isect_rel_clauses(rel, idx, &cols, &clauses);
		if (n > bestn)
		{
			if (best != NULL)
			{
				pfree(bestcols);
				pfree(bestclauses);
			}
			best = idx;
			bestn = n;
			bestcols = cols;
			bestclauses = clauses;
		}
		else
		{
			pfree(cols);
			pfree(clauses);
		}
	}

	if (best != NULL && bestn >= 2)
		factor = lion_isect_factor_run(root, rel, best, bestn, bestcols,
									   bestclauses, false);
	if (best != NULL)
	{
		pfree(bestcols);
		pfree(bestclauses);
	}
	return factor;
}

double
lion_probe_rel_factor(PlannerInfo *root, RelOptInfo *rel)
{
	if (root == NULL)
		return 1.0;
	return lion_isect_rel_factor(root, rel);
}

/*
 * rel's restriction clauses as lion's estimates take them: an OR of
 * equalities as the IN list it spells (lion_or_as_array()), every other
 * clause as it is; and whether any of them differs from core's.
 */
static List *
lion_rel_canonical_clauses(RelOptInfo *rel, bool *changed)
{
	List	   *result = NIL;
	ListCell   *lc;

	*changed = false;
	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
		Node	   *arr = rinfo->pseudoconstant ? NULL :
			lion_or_as_array((Node *) rinfo);

		if (arr != NULL)
		{
			result = lappend(result, arr);
			*changed = true;
		}
		else
			result = lappend(result, rinfo);
	}
	return result;
}

/*
 * rel's rows as lion's estimates see them: core's own count (rel->rows), or,
 * inside a scope that corrects rel, its restriction clauses' selectivity
 * with the probed ends, as set_baserel_size_estimates() computes rel->rows -
 * either of them times what the intersection probe found of the set clauses
 * among them (lion_isect_rel_factor()).  For a cost formula that reads the
 * rows the WHERE leaves: the count pushdown's recheck, and LionOrdered's
 * rows; the row count the planner compares paths by stays core's.
 *
 * An OR of equalities counts as the IN list it spells here too
 * (lion_rel_canonical_clauses()): core estimates `k = 1 OR k = 7` as though
 * the arms could overlap, 1 - (1 - s1)(1 - s2), and the list as the disjoint
 * entries they are, s1 + s2, so the same filter came out with fewer rows
 * written as an OR.  The rows are then computed as set_baserel_size_estimates()
 * computes rel->rows, from the list.
 *
 * The factor applies to core's rows before they are rounded and clamped to
 * one: a wide filter of correlated columns is where core's product comes out
 * at a fraction of a row, and rel->rows, one, times the factor measured
 * against that fraction would be as many times too many.
 */
double
lion_probe_rel_rows(PlannerInfo *root, RelOptInfo *rel)
{
	LionProbeScope *scope;
	double		rows = rel->rows;
	double		factor;
	List	   *canon = NIL;
	bool		changed = false;
	bool		scoped = false;

	if (root == NULL)
		return rows;
	canon = lion_rel_canonical_clauses(rel, &changed);
	for (scope = lion_probe_scope; scope != NULL; scope = scope->outer)
	{
		if (scope->glob == root->glob && scope->rti == rel->relid)
		{
			if (scope->rows < 0)
				scope->rows = rel->tuples *
					clauselist_selectivity(root, canon, 0, JOIN_INNER, NULL);
			rows = scope->rows;
			scoped = true;
			break;
		}
	}
	/* rel->rows already is what this would compute (lion_correct_rel_rows()) */
	if (!scoped && lion_probe_cache.glob == root->glob &&
		list_member_ptr(lion_probe_cache.rowsfixed, rel))
	{
		list_free(canon);
		return clamp_row_est(rows);
	}
	factor = lion_isect_rel_factor(root, rel);
	if (!scoped && (changed || factor != 1.0))
		rows = rel->tuples * clauselist_selectivity(root, canon, 0,
													JOIN_INNER, NULL);
	list_free(canon);
	return clamp_row_est(rows * factor);
}

/*
 * The index AM's amcostestimate: lioncostestimate() inside a probe scope over
 * the path's own quals, so that genericcostestimate()'s selectivity, the
 * entries of lion_range_walk_cost() and the heap side priced from them see
 * a range past the histogram as the directory has it.
 */
void
lion_amcostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
					Cost *indexStartupCost, Cost *indexTotalCost,
					Selectivity *indexSelectivity, double *indexCorrelation,
					double *indexPages)
{
	List	   *quals = NIL;
	ListCell   *lc;

	foreach(lc, path->indexclauses)
		quals = list_concat(quals, ((IndexClause *) lfirst(lc))->indexquals);

	if (!lion_probe_begin(root, path->indexinfo->rel, quals))
	{
		lioncostestimate(root, path, loop_count, indexStartupCost,
						 indexTotalCost, indexSelectivity, indexCorrelation,
						 indexPages);
		return;
	}
	PG_TRY();
	{
		lioncostestimate(root, path, loop_count, indexStartupCost,
						 indexTotalCost, indexSelectivity, indexCorrelation,
						 indexPages);
	}
	PG_FINALLY();
	{
		lion_probe_end();
	}
	PG_END_TRY();
}

/* ---------------------------------------------------------------------
 * What cost_index() cannot charge a plain scan (DESIGN.md §29.11)
 * --------------------------------------------------------------------- */

typedef struct LionPlainRemainder
{
	IndexPath  *path;
	Cost		remainder;
} LionPlainRemainder;

/*
 * lion_plain_heap_correlation() prices a plain scan in heap order as the
 * bitmap heap scan of the same rows and its per-row fetches, and hands
 * cost_index() the correlation that makes its heap side come out at that.
 * cost_index() interpolates between two ends and charges no more than the
 * uncorrelated one, Mackert and Lohman's pages at random_page_cost: at a
 * random_page_cost near seq_page_cost, a result of many rows on nearly every
 * page costs more than that end, by the fetches that are not the bitmap
 * scan's, and the correlation cannot say so.  The plain scan then came out
 * within 1% of the bitmap scan of the same rows, which add_path() takes for a
 * tie that the plain scan's lower startup cost wins, and the bitmap path was
 * thrown away - 13% to 25% faster warm and 2 to 5 times cold at 43% of 8M
 * rows, 18 to a page (2026-09-27).  What is left over is noted here, for an
 * unparameterized path, and charged once the relation's paths are built.
 */
void
lion_plain_note_remainder(PlannerInfo *root, IndexPath *path, Cost remainder)
{
	LionProbeCache *cache;
	LionPlainRemainder *r;
	MemoryContext oldcxt;

	if (root == NULL || root->glob == NULL || remainder <= 0)
		return;
	cache = lion_probe_cache_for(root);
	oldcxt = MemoryContextSwitchTo(cache->cxt);
	r = (LionPlainRemainder *) palloc(sizeof(LionPlainRemainder));
	r->path = path;
	r->remainder = remainder;
	cache->remainders = lappend(cache->remainders, r);
	MemoryContextSwitchTo(oldcxt);
}

/*
 * set_rel_pathlist_hook, first: rel's row count as the intersection probe
 * measures its restriction clauses (DESIGN.md §29.11, "The relation's rows").
 * Where the probe found the AND of the set clauses of one lion index at least
 * LION_ISECT_ERROR times off core's estimate, rel->rows becomes
 * lion_probe_rel_rows() - core's estimate of the clauses with that AND
 * measured - and so do the rows of every path already built for rel, which
 * were core's: an unparameterized path's rows are rel->rows (divided among a
 * partial path's workers), and its cost does not depend on them, each scan
 * pricing its own clauses' selectivity; a parameterized path's rows and its
 * ParamPathInfo's are the same restriction clauses with its join clauses
 * ANDed in, and take the same factor.  Joins are sized from rel->rows after
 * this, and the paths and Gathers built from here on read it.
 *
 * Only a plain relation of its own: an inheritance parent's rows are its
 * children's sum, made before any child's paths are built, and a child's
 * correction would not reach it.
 */
static void
lion_correct_rel_rows(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	double		oldrows = rel->rows;
	double		newrows;
	double		ratio;
	MemoryContext oldcxt;
	ListCell   *lc;

	if (!lion_enable_rows_correction || !lion_enable_intersection_probe ||
		root->glob == NULL || rel->reloptkind != RELOPT_BASEREL ||
		rte->rtekind != RTE_RELATION || rte->inh || rel->indexlist == NIL ||
		lion_isect_rel_factor(root, rel) == 1.0)
		return;

	newrows = lion_probe_rel_rows(root, rel);
	if (newrows == oldrows)
		return;
	ratio = newrows / Max(oldrows, 1.0);

	rel->rows = newrows;
	foreach(lc, rel->pathlist)
		((Path *) lfirst(lc))->rows =
			clamp_row_est(((Path *) lfirst(lc))->rows * ratio);
	foreach(lc, rel->partial_pathlist)
		((Path *) lfirst(lc))->rows =
			clamp_row_est(((Path *) lfirst(lc))->rows * ratio);
	foreach(lc, rel->ppilist)
		((ParamPathInfo *) lfirst(lc))->ppi_rows =
			clamp_row_est(((ParamPathInfo *) lfirst(lc))->ppi_rows * ratio);

	/* lion_probe_rel_rows() reads it as it is from here on */
	oldcxt = MemoryContextSwitchTo(lion_probe_cache_for(root)->cxt);
	lion_probe_cache.rowsfixed = lappend(lion_probe_cache.rowsfixed, rel);
	MemoryContextSwitchTo(oldcxt);
}

/*
 * set_rel_pathlist_hook: each plain lion path of rel with a remainder noted
 * pays it, and is offered again with the bitmap heap scan of the same index
 * path beside it, which add_path() may have discarded for the plain one - the
 * paths added here are compared as any others (set_rel_pathlist() allows a
 * hook to modify the core paths).  The plain path keeps its startup cost, which
 * is what a LIMIT asks of it.
 *
 * The bitmap heap scan is not all the plain path may have displaced before it
 * paid: set_plain_rel_pathlist() offers the sequential scan before the index
 * paths, and a plain path priced short by the remainder can dominate it -
 * add_path() frees what it discards - and the TID scans likewise.  Those are
 * rebuilt and offered again as core builds them; where the plain path had
 * displaced nothing, each is a copy of a path that is still there, and
 * add_path() throws the copy away.  The paths of the other indexes cannot be
 * rebuilt without building the relation's index paths again, lion's among
 * them, and are not: they would have had to cost less than the plain path
 * with its remainder and more than it without.
 */
static void
lion_plain_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
							RangeTblEntry *rte)
{
	lion_correct_rel_rows(root, rel, rte);

#if PG_VERSION_NUM < 180000

	/*
	 * An OR of equalities as the index clause 18 matches it to: the paths
	 * that answer it as the IN list it spells (lion_or_list_paths()), before
	 * the plain ones among them pay their remainder below, as every plain
	 * lion path does.
	 */
	if (rel->reloptkind == RELOPT_BASEREL && rte->rtekind == RTE_RELATION &&
		rel->indexlist != NIL && !lion_old_snapshot_threshold_active())
	{
		Oid			amoid = lion_get_am_oid();
		List	   *lionidx = NIL;
		ListCell   *lc;

		foreach(lc, rel->indexlist)
		{
			IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);

			if (idx->relam == amoid && !idx->hypothetical)
				lionidx = lappend(lionidx, idx);
		}
		foreach(lc, lion_or_list_paths(root, rel, lionidx))
			add_path(rel, (Path *) lfirst(lc));
		list_free(lionidx);
	}
#endif

	if (root->glob != NULL && lion_probe_cache.glob == root->glob &&
		lion_probe_cache.remainders != NIL)
	{
		List	   *repriced = NIL;
		ListCell   *lc;

		foreach(lc, rel->pathlist)
		{
			Path	   *p = (Path *) lfirst(lc);
			ListCell   *lc2;
			bool		found = false;

			if (!IsA(p, IndexPath) || p->pathtype != T_IndexScan ||
				p->param_info != NULL)
				continue;
			foreach(lc2, lion_probe_cache.remainders)
			{
				LionPlainRemainder *r = (LionPlainRemainder *) lfirst(lc2);

				if (r->path == (IndexPath *) p && r->remainder > 0)
				{
					p->total_cost += r->remainder;
					r->remainder = 0;
					found = true;
					break;
				}
			}
			if (found)
			{
				repriced = lappend(repriced, p);
				rel->pathlist = foreach_delete_current(rel->pathlist, lc);
			}
		}

		foreach(lc, repriced)
		{
			Path	   *p = (Path *) lfirst(lc);

			add_path(rel, p);
			add_path(rel, (Path *) create_bitmap_heap_path(root, rel, p, NULL,
														   1.0, 0));
		}
		if (repriced != NIL && rte->rtekind == RTE_RELATION &&
			rte->tablesample == NULL)
		{
			/* as set_plain_rel_pathlist() offers them */
			add_path(rel, create_seqscan_path(root, rel, rel->lateral_relids,
											  0));
			(void) create_tidscan_paths(root, rel);
		}
		list_free(repriced);
	}

	if (lion_prev_set_rel_pathlist_hook != NULL)
		lion_prev_set_rel_pathlist_hook(root, rel, rti, rte);
}

/* ---------------------------------------------------------------------
 * n_distinct from the directory (DESIGN.md §33)
 * --------------------------------------------------------------------- */

/*
 * ANALYZE estimates a column's distinct values from a sample of 300 rows per
 * unit of statistics target, and a column with a long tail of rare values -
 * a foreign key whose referenced rows have a handful of rows each - is where
 * that estimate falls short: most of its values are not in the sample at all,
 * and Haas and Stokes' estimator, which ANALYZE extrapolates the rest with,
 * cannot see them.  Every estimate that divides by n_distinct inherits the
 * error - the groups of a GROUP BY or a DISTINCT, the size of a hash
 * aggregate, `col = x` for a value that is not a most common one, a join on
 * the column.
 *
 * A lion index on the column has the answer: each VALUE entry of a scalar key
 * column is one distinct value, and the build, VACUUM and the ANALYZE of a
 * small directory count them onto the meta page (lionbuild(),
 * lion_vac_count(), lion_vacuum_count_keys()).  So where a plain
 * column of a table has a lion index on it, examine_variable() is handed the
 * column's pg_statistic row with stadistinct taken from the index, and
 * everything else in it - the null fraction, the most common values, the
 * histogram - as ANALYZE left it.  The same for an expression a lion index
 * holds, through get_index_stats_hook, with the statistics ANALYZE keeps for
 * that index column.
 *
 * What is supplied, and when (DESIGN.md §33, "The hook"):
 *	- only where core has a row to supply: a column never analyzed stays
 *	  without statistics, and the index's count waits for the first ANALYZE;
 *	- never for a column whose n_distinct the user set (ALTER TABLE ... ALTER
 *	  COLUMN ... SET (n_distinct = ...)), which ANALYZE has put in the row;
 *	- never for inheritance-tree statistics or a partitioned table's own,
 *	  which one index of one table does not count;
 *	- from a valid, non-partial, non-hypothetical lion index whose key column
 *	  is exactly the column (or the expression), with a scalar opclass whose
 *	  equality is the column type's own, under the column's collation or one
 *	  with the same equality: a multi-key opclass's keys are elements, a
 *	  partial index counts a part of the table, and an opclass that equates
 *	  what the type does not counts classes the planner would not;
 *	- only while the rows the count was taken over are within a factor of 2
 *	  of the rows the planner finds the table to have (lion_nd_current());
 *	- of several, the one counted over the rows nearest the table's now, and
 *	  of those the largest count;
 *	- in ANALYZE's own convention: a count above a tenth of the rows the
 *	  index held when it was counted goes in as the negative fraction of them,
 *	  which the planner scales by the table's current size, and any other as
 *	  the count (lion_ndistinct_value()).
 *
 * acl_ok - whether the user may read every row of the column, which decides
 * whether a non-leakproof operator is handed the row's values - is core's
 * own: the row is looked up by examine_variable() itself, with both hooks
 * standing aside for that one lookup (lion_ndistinct_nested), and its acl_ok is
 * what the copy is handed out with.  Nothing here decides a permission.
 *
 * The copy is made once per planner run and column and handed out as is, with
 * a release function that frees nothing: it lives, like the meta pages read,
 * in the memory of the planner run (lion_nd_cache_for()).  A run reads each
 * index's meta page once; the next run reads it again, so a count VACUUM,
 * ANALYZE or a REINDEX has written since is what the next plan sees.
 */

/* One index's meta page, as this planner run read it. */
typedef struct LionNdistinctIndex
{
	Oid			indexoid;
	bool		have;			/* the meta page carries counts at all */
	LionMetaNdistinct nd;
} LionNdistinctIndex;

/*
 * One column's row with the count in it: a table's column, or an index's
 * expression column.  stats is NULL when this run decided there is none to
 * supply.
 */
typedef struct LionNdistinctRow
{
	Oid			relid;			/* the table, or the index */
	AttrNumber	attnum;
	HeapTuple	stats;
} LionNdistinctRow;

typedef struct LionNdistinctCache
{
	PlannerGlobal *glob;
	MemoryContext cxt;
	Oid			amoid;			/* lion's, or InvalidOid without the AM */
	List	   *indexes;		/* LionNdistinctIndex */
	List	   *rows;			/* LionNdistinctRow */
} LionNdistinctCache;

static LionNdistinctCache lion_nd_cache;

static void
lion_nd_cache_reset(void *arg)
{
	if (lion_nd_cache.glob == (PlannerGlobal *) arg)
		memset(&lion_nd_cache, 0, sizeof(lion_nd_cache));
}

/* The cache of root's planner run, as lion_probe_cache_for() keeps its own. */
static LionNdistinctCache *
lion_nd_cache_for(PlannerInfo *root)
{
	if (lion_nd_cache.glob != root->glob)
	{
		MemoryContext cxt = GetMemoryChunkContext(root->glob);
		MemoryContextCallback *cb;

		cb = (MemoryContextCallback *) MemoryContextAlloc(cxt, sizeof(*cb));
		cb->func = lion_nd_cache_reset;
		cb->arg = root->glob;
		MemoryContextRegisterResetCallback(cxt, cb);

		lion_nd_cache.glob = root->glob;
		lion_nd_cache.cxt = cxt;
		lion_nd_cache.amoid = lion_get_am_oid();
		lion_nd_cache.indexes = NIL;
		lion_nd_cache.rows = NIL;
	}
	return &lion_nd_cache;
}

/* The copies belong to the planner run's memory, not to the caller. */
static void
lion_nd_release(HeapTuple tuple)
{
}

/*
 * The counts on an index's meta page, read once per planner run: one pin and
 * a SHARE lock.  The planner holds the index locked since get_relation_info().
 */
static LionNdistinctIndex *
lion_nd_index(LionNdistinctCache *cache, Oid indexoid)
{
	LionNdistinctIndex *ent;
	MemoryContext oldcxt;
	Relation	index;
	ListCell   *lc;

	foreach(lc, cache->indexes)
	{
		ent = (LionNdistinctIndex *) lfirst(lc);
		if (ent->indexoid == indexoid)
			return ent;
	}

	oldcxt = MemoryContextSwitchTo(cache->cxt);
	ent = (LionNdistinctIndex *) palloc0(sizeof(LionNdistinctIndex));
	ent->indexoid = indexoid;
	cache->indexes = lappend(cache->indexes, ent);
	MemoryContextSwitchTo(oldcxt);

	index = index_open(indexoid, NoLock);
	ent->have = lion_read_meta_ndistinct(index, &ent->nd);
	index_close(index, NoLock);

	return ent;
}

static LionNdistinctRow *
lion_nd_row(LionNdistinctCache *cache, Oid relid, AttrNumber attnum)
{
	ListCell   *lc;

	foreach(lc, cache->rows)
	{
		LionNdistinctRow *row = (LionNdistinctRow *) lfirst(lc);

		if (row->relid == relid && row->attnum == attnum)
			return row;
	}
	return NULL;
}

/*
 * Remember what this run decided for the column: stats, a row whose
 * stadistinct is the index's (copied into the run's memory), or none.
 */
static LionNdistinctRow *
lion_nd_remember(LionNdistinctCache *cache, Oid relid, AttrNumber attnum,
				 HeapTuple stats, float4 stadistinct)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(cache->cxt);
	LionNdistinctRow *row;

	row = (LionNdistinctRow *) palloc(sizeof(LionNdistinctRow));
	row->relid = relid;
	row->attnum = attnum;
	row->stats = NULL;
	if (HeapTupleIsValid(stats))
	{
		row->stats = heap_copytuple(stats);
		((Form_pg_statistic) GETSTRUCT(row->stats))->stadistinct = stadistinct;
	}
	cache->rows = lappend(cache->rows, row);
	MemoryContextSwitchTo(oldcxt);

	return row;
}

/*
 * Does key column col of idx hold the values of an expression of this type
 * and collation as the planner counts them: one key per class of the type's
 * own equality, under the collation?  The index's equality is its opclass's
 * strategy 1, under the index column's collation.  Every scalar opclass
 * pg_lion ships has the type's `=` (varchar reaches text's through binary
 * coercion, as the type's default btree class does); a multi-key one has no
 * strategy 1 at all.  Two deterministic collations have one equality, the
 * bytes'.
 */
static bool
lion_nd_same_equality(IndexOptInfo *idx, int col, Oid type, Oid collation)
{
	TypeCacheEntry *tce = lookup_type_cache(type, TYPECACHE_EQ_OPR);
	Oid			indexcoll = idx->indexcollations[col];
	Oid			eqop;

	eqop = get_opfamily_member(idx->opfamily[col], idx->opcintype[col],
							   idx->opcintype[col], LION_STRAT_EQUAL);
	if (!OidIsValid(eqop) || eqop != tce->eq_opr)
		return false;
	if (indexcoll == collation)
		return true;
	return OidIsValid(indexcoll) && OidIsValid(collation) &&
		get_collation_isdeterministic(indexcoll) &&
		get_collation_isdeterministic(collation);
}

/*
 * stadistinct for count distinct values among rows rows, in ANALYZE's
 * convention (compute_scalar_stats(), compute_distinct_stats()): a positive
 * number is the count, a negative one a fraction of the table's rows, taken
 * when the values are more than a tenth of the rows - "it seems likely that
 * the number of distinct values will scale with the table" - which the
 * planner multiplies by the rows it finds the table to have now
 * (get_variable_numdistinct()).  So a column whose values grow with the table,
 * an ever-growing foreign key, keeps its proportion between two counts, and
 * one with a fixed set of values keeps its number.  rows counts the NULLs too,
 * as ANALYZE's totalrows does.
 */
static float4
lion_ndistinct_value(uint64 count, uint64 rows)
{
	if (rows > 0 && (double) count > 0.1 * (double) rows)
		return (float4) -Min((double) count / (double) rows, 1.0);
	return (float4) count;
}

/*
 * Is a count taken over rows rows still one for a table the planner finds
 * to hold tuples rows now?  Within a factor of 2 either way (DESIGN.md §33,
 * "Stale counts").  A count is refreshed by every VACUUM that deletes and by
 * the ANALYZE of a table whose directory is small (lion_vacuum_count_keys());
 * the directory of a large table that only grows is counted by neither, and
 * its count falls further behind with every insert.  A fraction keeps up
 * with a column whose values grow with the table, but a count of a fixed set
 * of values does not shrink with it, and a column whose tail of rare values
 * is what grows is off either way: past a factor of 2 the planner is better
 * served by ANALYZE's estimate, which that ANALYZE has kept up to date.
 */
static bool
lion_nd_current(uint64 rows, double tuples)
{
	return rows > 0 && tuples >= 0.5 * (double) rows &&
		tuples <= 2.0 * (double) rows;
}

/*
 * The count of key column col of idx, if its meta page has one this column's
 * values may be counted by (above) and the table of tuples rows has not
 * outgrown; and how far the table has drifted from the rows it was taken
 * over, as |ln(rows / tuples)|, 0 for none.
 */
static bool
lion_nd_column_count(LionNdistinctCache *cache, IndexOptInfo *idx, int col,
					 Oid type, Oid collation, double tuples,
					 float4 *stadistinct, double *drift, uint64 *count)
{
	LionNdistinctIndex *ent;

	if (idx->relam != cache->amoid || idx->hypothetical ||
		idx->indpred != NIL || col >= LION_META_MAX_COLS ||
		!lion_nd_same_equality(idx, col, type, collation))
		return false;
	ent = lion_nd_index(cache, idx->indexoid);
	if (!ent->have || (ent->nd.valid_cols & (((uint32) 1) << col)) == 0 ||
		ent->nd.ndistinct[col] == 0 ||
		!lion_nd_current(ent->nd.rows, tuples))
		return false;

	*stadistinct = lion_ndistinct_value(ent->nd.ndistinct[col], ent->nd.rows);
	*drift = fabs(log((double) ent->nd.rows / tuples));
	*count = ent->nd.ndistinct[col];
	return true;
}

/*
 * Core's own lookup of node's statistics, with lion's hooks standing aside:
 * the row core would have used, and the acl_ok it computed for this very
 * reference to it.
 */
static void
lion_nd_core_lookup(PlannerInfo *root, Node *node, int varRelid,
					VariableStatData *core)
{
	bool		save = lion_ndistinct_nested;

	lion_ndistinct_nested = true;
	PG_TRY();
	{
		examine_variable(root, node, varRelid, core);
	}
	PG_FINALLY();
	{
		lion_ndistinct_nested = save;
	}
	PG_END_TRY();
}

/*
 * get_relation_stats_hook, for column attnum of the table rte names: its
 * pg_statistic row with stadistinct from a lion index on it (above).
 */
static bool
lion_ndistinct_stats(PlannerInfo *root, RangeTblEntry *rte, AttrNumber attnum,
					 VariableStatData *vardata)
{
	LionNdistinctCache *cache;
	LionNdistinctRow *row;
	RelOptInfo *rel = NULL;
	Index		varno = 0;
	Oid			vartype;
	int32		vartypmod;
	Oid			varcollid;
	VariableStatData core;
	float4		stadistinct = 0;
	bool		keyed = false;
	ListCell   *lc;
	int			i;

	if (!lion_enable_index_ndistinct || root == NULL || root->glob == NULL ||
		root->simple_rte_array == NULL || attnum <= 0 ||
		rte->rtekind != RTE_RELATION || rte->inh ||
		rte->relkind == RELKIND_PARTITIONED_TABLE)
		return false;

	/*
	 * The rel of this query level that rte is the range table entry of: core
	 * hands the hook root->simple_rte_array[varno] itself
	 * (examine_simple_variable()), which is also what planner_rt_fetch()
	 * gives the index cost estimators that ask.
	 */
	for (i = 1; i < root->simple_rel_array_size; i++)
	{
		if (root->simple_rte_array[i] == rte)
		{
			varno = (Index) i;
			rel = root->simple_rel_array[i];
			break;
		}
	}
	if (rel == NULL || rel->indexlist == NIL)
		return false;

	/* the common case first: no lion index of the table has the column */
	cache = lion_nd_cache_for(root);
	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);

		for (i = 0; i < idx->nkeycolumns && !keyed; i++)
			keyed = (idx->relam == cache->amoid && idx->indexkeys[i] == attnum);
	}
	if (!keyed)
		return false;

	row = lion_nd_row(cache, rte->relid, attnum);
	if (row != NULL && row->stats == NULL)
		return false;

	/*
	 * Core's row, and the acl_ok it gives this reference to the column, first:
	 * a column never analyzed has nothing to supply until ANALYZE has run, and
	 * no meta page is read for it.
	 */
	get_atttypetypmodcoll(rte->relid, attnum, &vartype, &vartypmod,
						  &varcollid);
	lion_nd_core_lookup(root,
						(Node *) makeVar(varno, attnum, vartype, vartypmod,
										 varcollid, 0),
						0, &core);
	if (!HeapTupleIsValid(core.statsTuple) ||
		((Form_pg_statistic) GETSTRUCT(core.statsTuple))->starelid != rte->relid ||
		((Form_pg_statistic) GETSTRUCT(core.statsTuple))->staattnum != attnum ||
		((Form_pg_statistic) GETSTRUCT(core.statsTuple))->stainherit)
	{
		if (row == NULL && !HeapTupleIsValid(core.statsTuple))
			(void) lion_nd_remember(cache, rte->relid, attnum, NULL, 0);
		ReleaseVariableStats(core);
		return false;
	}

	if (row == NULL)
	{
		AttributeOpts *aopt = get_attribute_options(rte->relid, attnum);
		bool		found = false;
		double		best_drift = 0;
		uint64		best = 0;

		/*
		 * The index counted over the rows nearest the table's now, and then
		 * the largest count.
		 */
		if (aopt == NULL || aopt->n_distinct == 0)
		{
			foreach(lc, rel->indexlist)
			{
				IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);
				int			c;

				for (c = 0; c < idx->nkeycolumns; c++)
				{
					float4		value;
					double		drift;
					uint64		count;

					if (idx->indexkeys[c] != attnum ||
						!lion_nd_column_count(cache, idx, c, vartype,
											  varcollid, rel->tuples,
											  &value, &drift, &count))
						continue;
					if (!found || drift < best_drift ||
						(drift == best_drift && count > best))
					{
						found = true;
						best_drift = drift;
						best = count;
						stadistinct = value;
					}
				}
			}
		}
		row = lion_nd_remember(cache, rte->relid, attnum,
							   found ? core.statsTuple : NULL, stadistinct);
		if (row->stats == NULL)
		{
			ReleaseVariableStats(core);
			return false;
		}
	}

	vardata->statsTuple = row->stats;
	vardata->freefunc = lion_nd_release;
	vardata->acl_ok = core.acl_ok;
	ReleaseVariableStats(core);
	return true;
}

/*
 * get_index_stats_hook, for expression column indexattnum of index indexOid,
 * which examine_variable() has matched vardata->var to: that index column's
 * pg_statistic row, which ANALYZE keeps for an expression, with stadistinct
 * from the index when it is a lion index that may say (above).  The index
 * core asks about is the one whose count is used.
 */
static bool
lion_index_expr_stats(PlannerInfo *root, Oid indexOid, AttrNumber indexattnum,
					  VariableStatData *vardata)
{
	LionNdistinctCache *cache;
	LionNdistinctRow *row;
	IndexOptInfo *idx = NULL;
	RangeTblEntry *rte;
	VariableStatData core;
	float4		stadistinct = 0;
	ListCell   *lc;

	if (lion_ndistinct_nested)
		return false;
	if (lion_prev_index_stats_hook != NULL &&
		lion_prev_index_stats_hook(root, indexOid, indexattnum, vardata))
		return true;

	if (!lion_enable_index_ndistinct || root == NULL || root->glob == NULL ||
		vardata->rel == NULL || vardata->var == NULL ||
		!IS_SIMPLE_REL(vardata->rel) || indexattnum < 1)
		return false;

	/*
	 * Not for an inheritance tree or a partitioned table, as above: the
	 * parent's index counts no partition's rows, and a partitioned index has
	 * no pages to read the count from.
	 */
	rte = planner_rt_fetch(vardata->rel->relid, root);
	if (rte->rtekind != RTE_RELATION || rte->inh ||
		rte->relkind == RELKIND_PARTITIONED_TABLE)
		return false;
	foreach(lc, vardata->rel->indexlist)
	{
		if (((IndexOptInfo *) lfirst(lc))->indexoid == indexOid)
			idx = (IndexOptInfo *) lfirst(lc);
	}
	if (idx == NULL || indexattnum > idx->nkeycolumns ||
		idx->indexkeys[indexattnum - 1] != 0)
		return false;

	cache = lion_nd_cache_for(root);
	row = lion_nd_row(cache, indexOid, indexattnum);
	if (row != NULL && row->stats == NULL)
		return false;
	if (row == NULL)
	{
		double		drift;
		uint64		count;

		if (!lion_nd_column_count(cache, idx, indexattnum - 1,
								  exprType((Node *) vardata->var),
								  exprCollation((Node *) vardata->var),
								  vardata->rel->tuples,
								  &stadistinct, &drift, &count))
		{
			(void) lion_nd_remember(cache, indexOid, indexattnum, NULL, 0);
			return false;
		}
	}

	/*
	 * Core's lookup of the same expression walks the same indexes to this
	 * one, whose row it takes; one that another index's row stopped at, where
	 * a hook before this one declined to supply, is left to core.
	 */
	lion_nd_core_lookup(root, (Node *) vardata->var, vardata->rel->relid,
						&core);
	if (!HeapTupleIsValid(core.statsTuple) ||
		((Form_pg_statistic) GETSTRUCT(core.statsTuple))->starelid != indexOid ||
		((Form_pg_statistic) GETSTRUCT(core.statsTuple))->staattnum != indexattnum)
	{
		if (row == NULL && !HeapTupleIsValid(core.statsTuple))
			(void) lion_nd_remember(cache, indexOid, indexattnum, NULL, 0);
		ReleaseVariableStats(core);
		return false;
	}
	if (row == NULL)
		row = lion_nd_remember(cache, indexOid, indexattnum, core.statsTuple,
							   stadistinct);

	vardata->statsTuple = row->stats;
	vardata->freefunc = lion_nd_release;
	vardata->acl_ok = core.acl_ok;
	ReleaseVariableStats(core);
	return true;
}
