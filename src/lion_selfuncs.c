/*-------------------------------------------------------------------------
 *
 * lion_selfuncs.c
 *		Two corrections to what the planner's own machinery tells lion's cost
 *		model: the endpoint probe - a range bound beyond a column's histogram,
 *		estimated from the column's first or last live key in the lion
 *		directory (DESIGN.md §28, "The endpoint probe") - and the part of a
 *		plain scan's price that cost_index() cannot charge (DESIGN.md §29.11,
 *		lion_plain_note_remainder()).
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
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/visibilitymap.h"
#include "catalog/pg_class.h"
#include "catalog/pg_statistic.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "parser/parse_coerce.h"
#include "parser/parsetree.h"
#include "storage/bufmgr.h"
#include "utils/array.h"
#include "utils/datum.h"
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

	if (entry->ntids == 0)
		return false;

	memset(&ps, 0, sizeof(ps));
	ps.index = index;
	ps.attno = entry->attno;
	ps.found = true;
	ps.pinbuf = InvalidBuffer;
	ps.ntids = entry->ntids;
	ps.ncontainers = entry->ncontainers;
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
		ps.head = entry->head;

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
 * inserted since, which is as good an end for an estimate.  A leaf VACUUM
 * emptied stays linked (DESIGN.md §18) and is read like any other: an end
 * whose last LION_PROBE_LEAVES leaves hold no live key, which is what deleting
 * the newest rows in bulk leaves, is given up on.
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

	*settled = false;

	/* every item at least an entry header and a line pointer, as a batch */
	maxitems = BLCKSZ / (MAXALIGN(LION_ENTRY_HDRSZ) + sizeof(ItemIdData)) + 1;
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
			entry = (LionEntryTuple *) PageGetItem(page, iid);

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
				*value = datumCopy(lion_fetch_key(state,
												  LionEntryGetKey(items[k])),
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
			buf = lion_dir_step_right(index, buf, BUFFER_LOCK_SHARE);
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
	if (lion_prev_relation_stats_hook != NULL)
		return lion_prev_relation_stats_hook(root, rte, attnum, vardata);
	return false;
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

/*
 * rel's rows as lion's estimates see them: core's own count (rel->rows), or,
 * inside a scope that corrects rel, its restriction clauses' selectivity
 * with the probed ends, as set_baserel_size_estimates() computes rel->rows.
 * For a cost formula that reads the rows a range leaves; the row count the
 * planner compares paths by stays core's.
 */
double
lion_probe_rel_rows(PlannerInfo *root, RelOptInfo *rel)
{
	LionProbeScope *scope;

	for (scope = lion_probe_scope; scope != NULL; scope = scope->outer)
	{
		if (root != NULL && scope->glob == root->glob && scope->rti == rel->relid)
		{
			if (scope->rows < 0)
				scope->rows = clamp_row_est(rel->tuples *
											clauselist_selectivity(root,
																   rel->baserestrictinfo,
																   0, JOIN_INNER,
																   NULL));
			return scope->rows;
		}
	}
	return rel->rows;
}

/*
 * The index AM's amcostestimate: lioncostestimate() inside a probe scope over
 * the path's own quals, so that genericcostestimate()'s selectivity, the
 * entries of lion_range_entry_cost() and the heap side priced from them see
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
 * set_rel_pathlist_hook: each plain lion path of rel with a remainder noted
 * pays it, and is offered again with the bitmap heap scan of the same index
 * path beside it, which add_path() may have discarded for the plain one - the
 * paths added here are compared as any others (set_rel_pathlist() allows a
 * hook to modify the core paths).  The plain path keeps its startup cost, which
 * is what a LIMIT asks of it.
 */
static void
lion_plain_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
							RangeTblEntry *rte)
{
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
		list_free(repriced);
	}

	if (lion_prev_set_rel_pathlist_hook != NULL)
		lion_prev_set_rel_pathlist_hook(root, rel, rti, rte);
}
