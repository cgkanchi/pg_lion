/*-------------------------------------------------------------------------
 *
 * lion_count.c
 *		Heap-skipping count(*) over roaring posting sets, interlocked with
 *		the visibility map.  DESIGN.md section 9 is the specification and
 *		the safety argument; this file is deliberately shaped so that the
 *		argument can be checked by reading it.
 *
 * The rule the whole thing rests on:
 *
 *		A container may only be counted against the visibility map while the
 *		page it was copied out of is still PINNED.
 *
 * ambulkdelete() takes LockBufferForCleanup() on every page whose containers
 * or INLINE payloads it rewrites (DESIGN.md section 11), and a cleanup lock
 * waits for all pins to go away.  So while we pin the page a container came
 * from, VACUUM cannot have finished removing that container's dead TIDs from
 * this index, and therefore cannot yet have set all-visible on any heap page
 * those dead TIDs live on.  A heap block that the visibility map reports as
 * all-visible while we hold that pin can therefore not contain a dead tuple
 * of ours, and its members can be counted without looking at the heap.
 *
 * Content locks are a different matter: they are dropped as soon as the
 * containers have been copied into backend-local memory.  Only the pin is
 * load bearing.
 *
 * What the merge intersects are SOURCES, not single posting sets: a source is
 * the union of one or more sets (an IN list, DESIGN.md §15) and may be
 * NEGATED, in which case its members are subtracted from the result instead
 * of intersected into it (`col IS NOT NULL`, DESIGN.md §14).  Neither changes
 * the rule above - every sub-cursor that contributed a container still pins
 * the page it came from until the merged container has been through the
 * visibility map - but the pin that carries the interlock has to belong to a
 * POSITIVE source, because only those are guaranteed to hold a container at
 * every container key that gets counted.
 *
 * The rule says "the page a container was copied out of", not "every page
 * every container of the intersection came from", and that is deliberate:
 * VACUUM sets a heap page all-visible only after ambulkdelete() has finished
 * on EVERY index of the table, so one pin that blocks one index's
 * ambulkdelete blocks the all-visible bit for all of them.  That is what lets
 * lion_posting_set_materialize() serve the WHERE sets of a GROUP BY from
 * pinless private copies while the group's own set is read the pinned way;
 * see the comment on that function.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/visibilitymap.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/index.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_am.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "common/hashfn.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "storage/predicate.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/injection_point.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "lion.h"
#include "lion_count.h"

PG_FUNCTION_INFO_V1(lion_index_count);
PG_FUNCTION_INFO_V1(lion_index_count2);
PG_FUNCTION_INFO_V1(lion_index_count_stats);
PG_FUNCTION_INFO_V1(lion_index_count_group_stats);
PG_FUNCTION_INFO_V1(lion_index_count_any);

/* First size of the recheck TID array, and the step it grows past its budget. */
#define LION_RECHECK_INIT_TIDS	256
#define LION_RECHECK_GROW_TIDS	1024

/*
 * Floor and ceiling of the recheck batch budget (in TIDs).  The floor keeps a
 * tiny work_mem from turning the batched block recheck back into one block
 * visit per TID; the ceiling is an allocation bound, not a policy: a batch
 * must stay well inside what repalloc() will hand out.
 */
#define LION_RECHECK_MIN_BATCH	4096
#define LION_RECHECK_MAX_BATCH	((int) (MaxAllocSize / sizeof(ItemPointerData) / 2))

/*
 * Above this many containers at one container key, an OR node stops folding
 * them pairwise and accumulates them in a bitset image instead (see
 * lion_ecursor_build()).  Pairwise is cheaper while the containers are few and
 * small, because it touches only their members; the image costs a fixed pass
 * over the whole container key's range however few members arrive.  Measured
 * on 1M rows with IN lists of 3, 10, 100 and 1000 values.
 */
#ifndef LION_OR_BITSET_MIN
#define LION_OR_BITSET_MIN	32
#endif

/*
 * A posting set is worth materializing (DESIGN.md section 9 and the comment
 * on lion_posting_set_materialize()) when it is this small.  Either bound is
 * enough: 64 containers cover 4096 heap blocks however fat they are, and a
 * set of many thin containers is cheap to keep as long as it stays under the
 * byte budget.  A set that fails both bounds keeps being walked page by page.
 */
#define LION_MATERIALIZE_MAX_CONTAINERS	64
#define LION_MATERIALIZE_MAX_BYTES		(256 * 1024)

/*
 * Per-call state of one count.  The visibility map buffer is kept for the
 * whole call (one VM page covers ~32k heap blocks) and released at the end.
 */
typedef struct LionCountCtx
{
	Relation	heap;
	Snapshot	snapshot;
	Buffer		vmbuf;			/* pinned VM page, or InvalidBuffer */
	bool		serializable;	/* IsolationIsSerializable() at start: take page predicate locks */
	bool		in_recovery;	/* hot standby: the pin interlock does not hold, recheck everything */
	LionVisCache *cache;			/* per-query visibility cache, or NULL */
	int64		count;			/* members counted straight from the VM */
	LionCountStats stats;

	/* TIDs on heap blocks that were not all-visible, in ascending order */
	ItemPointerData *tids;
	int			ntids;
	int			maxtids;
	int			batchmax;		/* flush the list once it holds this many */
	bool		tids_sorted;	/* they came out in order (they always do) */
	int64		recheck_count;	/* rows counted by the batches flushed so far */
} LionCountCtx;

/*
 * A posting set's containers copied out of the index, private to the backend
 * and holding no pin.  Containers are MAXALIGNed inside buf so that a BITSET
 * payload keeps its uint64 alignment, and are in ascending ckey order, which
 * is what the merge in lion_count_posting_sets() requires.
 */
typedef struct LionMatSet
{
	int			ncontainers;
	Size		bytes;
	char	   *buf;
	LionContainer **containers;
} LionMatSet;

/*
 * A cursor over the containers of one posting set, in ascending ckey order.
 *
 * Invariant (DESIGN.md section 9): whenever cur is valid, pinbuf is a pin on
 * the page cur was copied out of.  The pin is dropped only by
 * lion_cursor_next() / lion_cursor_close(), never anywhere else, so every place
 * that lets go of a source page is visible in this file as a call to one of
 * those two functions.
 *
 * Sparse segments (DESIGN.md §13) are presented as containers, so that the
 * merge, the AND and the visibility-map mask below need not know they exist:
 * a segment is expanded one container key at a time into segbuf, each time
 * as a temporary ARRAY container holding that key's (at most
 * LION_SPARSE_THRESHOLD - 1) members.  The keys come out in ascending order,
 * exactly like real containers, so the merge still sees one ascending run of
 * container keys per set, and lion_count_container() still does one
 * visibility-map read per container key.
 *
 * The pin discipline is unchanged by that, and this is the reason it works:
 * a segment being expanded lives in the cursor's page image (or in the
 * INLINE payload copy), and the source page pin is only dropped when the
 * cursor moves past the LAST item of that page -- which cannot happen while
 * a segment of it still has container keys left, because only
 * lion_cursor_next_item() advances the page and it is not called until then.
 */
typedef struct LionSetCursor
{
	const LionPostingSet *set;
	bool		valid;			/* cur points at a container */
	const LionContainer *cur;

	/* INLINE sets: offset into the payload copy */
	Size		payoff;
	LionContainer *cbuf;			/* aligned staging buffer (payloads are packed) */

	/* materialized sets: index into set->mat->containers */
	int			matidx;

	/* CHAIN sets: the page image being consumed */
	BlockNumber nextblk;
	OffsetNumber off;
	OffsetNumber maxoff;
	PGAlignedBlock *imgbuf;		/* private copy of the current container page */
	Page		img;

	/* the sparse segment being expanded, and how far into its pairs */
	const LionContainer *seg;
	uint32		segpos;
	LionContainer *segbuf;		/* one container key's members, built here */

	/*
	 * Pin on the source page of the current container.  For an INLINE set
	 * this is the LionPostingSet's own bucket-page pin, which the cursor
	 * borrows and must not release (ownpin is false).
	 */
	Buffer		pinbuf;
	bool		ownpin;

	LionCountCtx *cx;			/* for statistics */
} LionSetCursor;

static void lion_cursor_next(LionSetCursor *cur);
static void lion_recheck_flush(LionCountCtx *cx);


/* ---------------------------------------------------------------------
 * Small helpers
 * --------------------------------------------------------------------- */

/*
 * Oid of the "lion" access method.
 *
 * NOT cached in a static: the extension can be dropped and recreated inside
 * one backend (DROP EXTENSION ... CASCADE takes the indexes with it, so no
 * index survives to pin the old Oid), and the new pg_am row legitimately gets
 * a different Oid.  A process-local cache would then reject every index as
 * "not a lion index".  get_am_oid() is a GetSysCacheOid1(AMNAME) lookup,
 * which the syscache invalidates correctly and answers from memory.
 */
Oid
lion_get_am_oid(void)
{
	return get_am_oid("lion", false);
}

/*
 * Everything needed to probe an index with keys of one search type: the hash
 * function to use, and, when the type is not the index's own, the cross-type
 * equality function to compare stored keys against it.  This is the same
 * dance lion_scan.c does for scan keys: the hash must come from the argument
 * type's own support function, the comparison from the opfamily's strategy-1
 * operator with the stored type on the left.
 *
 * It is a struct rather than a call per key because an IN list probes one
 * index with up to LION_MAX_ARRAY_ELEMS keys of the same type (DESIGN.md §15),
 * and the catalogue lookups behind it are the same every time.
 */
typedef struct LionProbe
{
	bool		crosstype;		/* the keys are not the index's own type */
	FmgrInfo	eqproc;			/* crosstype: stored = search comparison */
	FmgrInfo	hashinfo;		/* crosstype: the search type's own hash */
	int16		typlen;			/* the search type, for datumIsEqual() */
	bool		typbyval;
} LionProbe;

static void
lion_probe_init(Relation index, LionState *state, Oid keytype, LionProbe *probe)
{
	Oid			opfamily = index->rd_opfamily[0];
	Oid			opcintype = index->rd_opcintype[0];
	Oid			eqopr;
	Oid			hashproc;

	memset(probe, 0, sizeof(LionProbe));

	if (!OidIsValid(keytype) || keytype == opcintype)
	{
		probe->typlen = state->typlen;
		probe->typbyval = state->typbyval;
		return;
	}

	eqopr = get_opfamily_member(opfamily, opcintype, keytype, 1);
	if (!OidIsValid(eqopr))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("type %s cannot be compared with index \"%s\"",
						format_type_be(keytype),
						RelationGetRelationName(index)),
				 errdetail("The index is on type %s.",
						   format_type_be(opcintype))));

	hashproc = get_opfamily_proc(opfamily, keytype, keytype, 1);
	if (!OidIsValid(hashproc))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("missing support function 1 for type %s in operator family \"%s\"",
						format_type_be(keytype),
						get_opfamily_name(opfamily, false))));

	fmgr_info(get_opcode(eqopr), &probe->eqproc);
	fmgr_info(hashproc, &probe->hashinfo);
	probe->crosstype = true;
	get_typlenbyval(keytype, &probe->typlen, &probe->typbyval);
}

static inline uint32
lion_probe_hash(LionState *state, LionProbe *probe, Datum key)
{
	if (!probe->crosstype)
		return lion_hash_key(state, key);
	return DatumGetUInt32(FunctionCall1Coll(&probe->hashinfo, state->collation,
											key));
}


/* ---------------------------------------------------------------------
 * Locating posting sets
 * --------------------------------------------------------------------- */

/*
 * Fill *ps from the entry tuple at (buf, offnum), which the caller holds
 * locked SHARE.  Returns with the lock still held; the caller decides what to
 * do with the buffer.  When the entry is INLINE the payload is copied out and
 * *keeppin is set: the caller must keep a pin on buf and store it in
 * ps->pinbuf (DESIGN.md section 9).
 */
static void
lion_fill_posting_set(Relation index, LionState *state, Buffer buf,
					 OffsetNumber offnum, LionPostingSet *ps, bool *keeppin)
{
	Page		page = BufferGetPage(buf);
	ItemId		iid = PageGetItemId(page, offnum);
	LionEntryTuple *entry = (LionEntryTuple *) PageGetItem(page, iid);

	memset(ps, 0, sizeof(LionPostingSet));
	ps->index = index;
	ps->pinbuf = InvalidBuffer;
	ps->found = true;
	ps->ntids = entry->ntids;
	ps->ncontainers = entry->ncontainers;
	ps->cxt = CurrentMemoryContext;
	ps->nuses = 0;
	ps->mat = NULL;

	/*
	 * lion_fetch_key() points into the page for by-reference types, so copy
	 * the key out while the buffer is still locked.  The reserved NULL entry
	 * (DESIGN.md §14) has no key bytes to copy.
	 */
	ps->keyisnull = LionEntryIsNullKey(entry);
	if (ps->keyisnull)
	{
		ps->storedkey = (Datum) 0;
		ps->hasstoredkey = true;
	}
	else
	{
		ps->storedkey = datumCopy(lion_fetch_key(state, LionEntryGetKey(entry)),
								  state->typbyval, state->typlen);
		ps->hasstoredkey = true;
	}

	if ((entry->flags & LION_ENTRY_INLINE) != 0)
	{
		ps->is_inline = true;
		ps->head = InvalidBlockNumber;
		ps->paylen = LION_ENTRY_PAYLOAD_LEN(entry, ItemIdGetLength(iid));
		if (ps->paylen > 0)
		{
			ps->payload = (char *) palloc(ps->paylen);
			memcpy(ps->payload, LionEntryGetPayload(entry), ps->paylen);
		}
		*keeppin = true;
	}
	else
	{
		Assert((entry->flags & LION_ENTRY_CHAIN) != 0);
		ps->is_inline = false;
		ps->head = entry->head;
		*keeppin = false;
	}
}

/*
 * One key of an IN list, in the order its entry should be looked up in:
 * bucket first, so the bucket pages are visited in ascending block order,
 * then hash, so equal values (which hash equally) end up adjacent and the
 * duplicate check is a look at the neighbours.
 */
typedef struct LionProbeKey
{
	uint32		bucket;
	uint32		hash;
	int32		idx;			/* position in the caller's value array */
} LionProbeKey;

static int
lion_probe_key_cmp(const void *a, const void *b)
{
	const LionProbeKey *x = (const LionProbeKey *) a;
	const LionProbeKey *y = (const LionProbeKey *) b;

	if (x->bucket != y->bucket)
		return x->bucket < y->bucket ? -1 : 1;
	if (x->hash != y->hash)
		return x->hash < y->hash ? -1 : 1;
	return x->idx < y->idx ? -1 : (x->idx > y->idx ? 1 : 0);
}

/*
 * Locate the entry of one key whose hash has already been computed.  This is
 * lion_posting_set_lookup() from the bucket read onwards, split out so that a
 * whole IN list can have its hashes taken - and its bucket pages visited in
 * order - before any page is read (lion_posting_set_lookup_many()).
 */
static bool
lion_posting_set_locate(Relation index, LionState *state, LionProbe *probe,
					   Datum key, uint32 hash, uint32 bucket,
					   LionPostingSet *ps)
{
	Buffer		headbuf;
	Buffer		entrybuf;
	OffsetNumber entryoff;
	bool		keeppin;

	memset(ps, 0, sizeof(LionPostingSet));
	ps->index = index;
	ps->pinbuf = InvalidBuffer;
	ps->head = InvalidBlockNumber;

	headbuf = ReadBuffer(index, LION_BUCKET_BLKNO(bucket));
	LockBuffer(headbuf, BUFFER_LOCK_SHARE);

	if (!lion_find_entry_ext(index, state, headbuf, BUFFER_LOCK_SHARE,
							key, hash,
							probe->crosstype ? &probe->eqproc : NULL,
							state->collation,
							&entrybuf, &entryoff))
	{
		UnlockReleaseBuffer(headbuf);
		return false;
	}

	lion_fill_posting_set(index, state, entrybuf, entryoff, ps, &keeppin);

	if (keeppin)
	{
		/*
		 * DESIGN.md section 9: an INLINE payload's interlock is a pin on the
		 * bucket page it lives on, so drop the content lock but hold on to
		 * the pin until lion_posting_set_release().
		 */
		LockBuffer(entrybuf, BUFFER_LOCK_UNLOCK);
		ps->pinbuf = entrybuf;
		if (entrybuf != headbuf)
			UnlockReleaseBuffer(headbuf);
	}
	else
	{
		if (entrybuf != headbuf)
			UnlockReleaseBuffer(entrybuf);
		UnlockReleaseBuffer(headbuf);
	}

	return true;
}

bool
lion_posting_set_lookup(Relation index, Datum key, Oid keytype,
					   LionPostingSet *ps)
{
	LionState   *state = lion_get_state(index);
	LionProbe	probe;
	uint32		hash;

	lion_probe_init(index, state, keytype, &probe);
	hash = lion_probe_hash(state, &probe, key);

	return lion_posting_set_locate(index, state, &probe, key, hash,
								  lion_bucket_of(hash, state->meta.nbuckets),
								  ps);
}

/*
 * Locate the posting sets of many keys of one index at once: the IN list of
 * DESIGN.md §15, whose union the merge in this file then evaluates.
 *
 * Two things are done here that a loop over lion_posting_set_lookup() cannot:
 *
 *	- the keys are hashed first and the entries are then located in (bucket,
 *	  hash) order, so the bucket pages are read in ascending block order and
 *	  each one that several keys land in is read once while it is hot, instead
 *	  of in whatever order the array happened to list its values;
 *	- duplicates are dropped in one pass over that order instead of by
 *	  comparing every value with every earlier one, which at the 1000 values
 *	  the planner allows is half a million datumIsEqual() calls.  Equal values
 *	  hash equally, so they are adjacent in this order; the comparison is the
 *	  bytewise one, so two values that compare equal without being identical
 *	  still get a set each, which is harmless (a union does not double-count).
 *
 * *sets must have room for nvalues sets; the located ones come out packed at
 * the front, in bucket order, and the return value is how many there are.
 * Every one of them - found or not - must be handed to
 * lion_posting_set_release().  *nfound, if given, is how many of them have an
 * entry in the index at all: nfound == 0 means the union selects nothing.
 */
int
lion_posting_set_lookup_many(Relation index, Oid keytype, int nvalues,
							const Datum *values, const bool *isnull,
							LionPostingSet *sets, int *nfound)
{
	LionState   *state = lion_get_state(index);
	LionProbe	probe;
	LionProbeKey *probes;
	int			nprobe = 0;
	int			nsets = 0;
	int			found = 0;
	int			i;

	Assert(nvalues >= 0);
	if (nfound != NULL)
		*nfound = 0;
	if (nvalues == 0)
		return 0;

	lion_probe_init(index, state, keytype, &probe);

	probes = (LionProbeKey *) palloc(sizeof(LionProbeKey) * nvalues);
	for (i = 0; i < nvalues; i++)
	{
		if (isnull != NULL && isnull[i])
			continue;			/* `col = NULL` is never true */
		probes[nprobe].hash = lion_probe_hash(state, &probe, values[i]);
		probes[nprobe].bucket = lion_bucket_of(probes[nprobe].hash,
											  state->meta.nbuckets);
		probes[nprobe].idx = i;
		nprobe++;
	}

	if (nprobe > 1)
		qsort(probes, nprobe, sizeof(LionProbeKey), lion_probe_key_cmp);

	for (i = 0; i < nprobe; i++)
	{
		bool		dup = false;
		int			j;

		/*
		 * A duplicate can only be among the entries with this very hash, and
		 * the sort has put those together; a run of them is as long as the
		 * number of values that collide, which is one in practice.
		 */
		for (j = i - 1; j >= 0 && probes[j].hash == probes[i].hash; j--)
		{
			if (datumIsEqual(values[probes[i].idx], values[probes[j].idx],
							 probe.typbyval, probe.typlen))
			{
				dup = true;
				break;
			}
		}
		if (dup)
			continue;

		if (lion_posting_set_locate(index, state, &probe,
								   values[probes[i].idx], probes[i].hash,
								   probes[i].bucket, &sets[nsets]))
			found++;
		nsets++;

		if ((i & 0x3f) == 0)
			CHECK_FOR_INTERRUPTS();
	}

	pfree(probes);
	if (nfound != NULL)
		*nfound = found;
	return nsets;
}

/*
 * The same for the rows whose key is NULL (DESIGN.md §14).  The entry is
 * found by its flag in bucket 0; everything after that - the pin discipline
 * of DESIGN.md §9 included - is identical to a real key's.
 */
bool
lion_posting_set_lookup_null(Relation index, LionPostingSet *ps)
{
	LionState   *state = lion_get_state(index);
	Buffer		headbuf;
	Buffer		entrybuf;
	OffsetNumber entryoff;
	bool		keeppin;

	memset(ps, 0, sizeof(LionPostingSet));
	ps->index = index;
	ps->pinbuf = InvalidBuffer;
	ps->head = InvalidBlockNumber;

	headbuf = ReadBuffer(index, LION_BUCKET_BLKNO(LION_NULLKEY_BUCKET));
	LockBuffer(headbuf, BUFFER_LOCK_SHARE);

	if (!lion_find_null_entry(index, headbuf, BUFFER_LOCK_SHARE,
							 &entrybuf, &entryoff))
	{
		UnlockReleaseBuffer(headbuf);
		return false;
	}

	lion_fill_posting_set(index, state, entrybuf, entryoff, ps, &keeppin);

	if (keeppin)
	{
		LockBuffer(entrybuf, BUFFER_LOCK_UNLOCK);
		ps->pinbuf = entrybuf;
		if (entrybuf != headbuf)
			UnlockReleaseBuffer(headbuf);
	}
	else
	{
		if (entrybuf != headbuf)
			UnlockReleaseBuffer(entrybuf);
		UnlockReleaseBuffer(headbuf);
	}

	return true;
}

void
lion_posting_set_release(LionPostingSet *ps)
{
	if (BufferIsValid(ps->pinbuf))
		ReleaseBuffer(ps->pinbuf);
	ps->pinbuf = InvalidBuffer;
	ps->payload = NULL;			/* the memory belongs to the caller's context */
	ps->paylen = 0;
	ps->mat = NULL;				/* ... and so does the materialized copy */
	ps->nuses = 0;
	ps->hasstoredkey = false;
	ps->keyisnull = false;
	ps->found = false;
}


/* ---------------------------------------------------------------------
 * Materializing a posting set
 * --------------------------------------------------------------------- */

/*
 * Copy every container of a CHAIN posting set into private memory, so that
 * later counts can read it without touching the index again.  Returns false
 * (and leaves ps->mat NULL) when the set turns out to be too big to be worth
 * copying; the caller then keeps walking the chain page by page.
 *
 * Why this is safe, and why only *some* sets may be materialized
 * -------------------------------------------------------------
 * A materialized set holds no pin, so it gives up the DESIGN.md section 9
 * interlock for itself: between the copy and the visibility-map check a
 * VACUUM may have run, so the copy may still list a TID that has since been
 * removed from the index and pruned from the heap.
 *
 * That does not make a wrong count possible, because the number this file
 * produces is the count of the INTERSECTION, and the intersection is only
 * ever counted from the visibility map while at least one participating set
 * is being read the pinned way - lion_count_posting_sets() enforces that.
 * Take a TID t that the stale copy still contains and that is really dead:
 *
 *	- either t is no longer in the pinned set's container, and it is not in
 *	  the intersection at all, so it is not counted;
 *	- or it is, which means we read that container before VACUUM removed t
 *	  from that index.  VACUUM only removes a TID from a page under a cleanup
 *	  lock on it (section 11), so it cannot have got past the page we are
 *	  pinning; it has therefore not finished ambulkdelete() on that index, and
 *	  it only sets all-visible on a heap page after ambulkdelete() has
 *	  finished on EVERY index of the table.  The visibility map therefore
 *	  cannot say all-visible for t's heap block, t goes to the heap recheck,
 *	  and the snapshot decides - which is the right answer for a dead tuple.
 *
 * The other direction, a copy that is missing a TID, cannot happen: an index
 * entry is written before the inserting transaction commits, so every row
 * visible to our snapshot was already in the index when we took the copy.
 *
 * The copy is walked exactly the way lion_cursor_next() walks a chain - items
 * and rightlink read together under one SHARE lock - so a concurrent page
 * split (which only ever moves items to a new page to the right) cannot make
 * us miss or duplicate a container.
 */
static bool
lion_posting_set_materialize(LionPostingSet *ps)
{
	MemoryContext oldcxt;
	PGAlignedBlock *imgbuf;
	Page		img;
	BlockNumber blkno;
	char	   *buf;
	Size		cap;
	Size		used = 0;
	Size	   *offs;
	int			noffs = 0;
	int			offcap;
	bool		ok = true;
	bool		sorted = true;
	int			i;

	Assert(ps->found && !ps->is_inline && ps->mat == NULL);

	oldcxt = MemoryContextSwitchTo(ps->cxt != NULL ? ps->cxt : CurrentMemoryContext);

	offcap = Min(LION_MATERIALIZE_MAX_CONTAINERS * 2, 256);
	offs = (Size *) palloc(sizeof(Size) * offcap);
	cap = 8192;
	buf = (char *) palloc(cap);
	imgbuf = (PGAlignedBlock *) palloc(sizeof(PGAlignedBlock));
	img = (Page) imgbuf->data;

	for (blkno = ps->head; BlockNumberIsValid(blkno);)
	{
		Buffer		pagebuf;
		Page		page;
		OffsetNumber off;
		OffsetNumber maxoff;

		pagebuf = ReadBuffer(ps->index, blkno);
		LockBuffer(pagebuf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(pagebuf);
		if (!LionPageIsContainer(page))
		{
			UnlockReleaseBuffer(pagebuf);
			elog(ERROR, "lion index: block %u is not a container page",
				 blkno);
		}
		memcpy(img, page, BLCKSZ);
		UnlockReleaseBuffer(pagebuf);

		blkno = LionPageGetOpaque(img)->rightlink;
		maxoff = PageGetMaxOffsetNumber(img);

		for (off = FirstOffsetNumber; off <= maxoff; off = OffsetNumberNext(off))
		{
			ItemId		iid = PageGetItemId(img, off);
			const LionContainer *c;
			Size		sz;

			if (!ItemIdIsUsed(iid))
				continue;
			c = (const LionContainer *) PageGetItem(img, iid);
			sz = lion_item_size(c);
			Assert(sz <= LION_CONTAINER_MAX_SIZE);

			/*
			 * Give up as soon as the set fails BOTH budgets: a wide set is
			 * cheaper to re-walk than to keep a copy of.  A sparse segment
			 * counts as the one item it is, which is also how the entry
			 * counts it in ncontainers.
			 */
			if (noffs >= LION_MATERIALIZE_MAX_CONTAINERS &&
				used + sz > LION_MATERIALIZE_MAX_BYTES)
			{
				ok = false;
				break;
			}

			while (used + sz > cap)
			{
				cap *= 2;
				buf = (char *) repalloc(buf, cap);
			}
			if (noffs >= offcap)
			{
				offcap *= 2;
				offs = (Size *) repalloc(offs, sizeof(Size) * offcap);
			}

			memcpy(buf + used, c, sz);
			offs[noffs++] = used;
			used += MAXALIGN(sz);
		}

		if (!ok)
			break;

		CHECK_FOR_INTERRUPTS();
	}

	pfree(imgbuf);

	if (ok)
	{
		LionMatSet  *mat = (LionMatSet *) palloc(sizeof(LionMatSet));

		mat->ncontainers = noffs;
		mat->bytes = used;
		mat->buf = buf;
		mat->containers = (LionContainer **)
			palloc(sizeof(LionContainer *) * Max(noffs, 1));
		for (i = 0; i < noffs; i++)
		{
			mat->containers[i] = (LionContainer *) (buf + offs[i]);
			if (i > 0 &&
				lion_item_first_ckey(mat->containers[i]) <=
				lion_item_last_ckey(mat->containers[i - 1]))
				sorted = false;
		}

		/*
		 * Chains are built and maintained in ascending ckey order, with item
		 * ranges that never overlap (DESIGN.md §13), so this never fires; the
		 * merge would silently under-count if it ever did, which is worth one
		 * comparison per item to rule out.
		 */
		if (!sorted)
			elog(ERROR, "lion index: containers of \"%s\" are out of order",
				 RelationGetRelationName(ps->index));

		ps->mat = mat;
	}
	else
	{
		pfree(buf);
	}

	pfree(offs);
	MemoryContextSwitchTo(oldcxt);

	return ok;
}


/* ---------------------------------------------------------------------
 * Container cursors
 * --------------------------------------------------------------------- */

/*
 * Release the pin the cursor owns, if any.  The only two callers are
 * lion_cursor_next() (page exhausted) and lion_cursor_close().
 */
static void
lion_cursor_unpin(LionSetCursor *cur)
{
	if (cur->ownpin && BufferIsValid(cur->pinbuf))
		ReleaseBuffer(cur->pinbuf);
	cur->pinbuf = InvalidBuffer;
	cur->ownpin = false;
}

static void
lion_cursor_init(LionSetCursor *cur, const LionPostingSet *set, LionCountCtx *cx)
{
	memset(cur, 0, sizeof(LionSetCursor));
	cur->set = set;
	cur->cx = cx;
	cur->pinbuf = InvalidBuffer;
	cur->nextblk = InvalidBlockNumber;

	if (!set->found)
		return;

	if (set->mat != NULL)
	{
		/* a private copy: nothing to pin, nothing to walk */
		cur->matidx = 0;
	}
	else if (set->is_inline)
	{
		cur->cbuf = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
		cur->payoff = 0;
		/* borrowed, not owned: the LionPostingSet releases it */
		cur->pinbuf = set->pinbuf;
		cur->ownpin = false;
	}
	else
	{
		cur->imgbuf = (PGAlignedBlock *) palloc(sizeof(PGAlignedBlock));
		cur->img = (Page) cur->imgbuf->data;
		cur->nextblk = set->head;
	}

	lion_cursor_next(cur);
}

/*
 * Advance to the next ITEM of the set: a container or a sparse segment, in
 * ascending ckey order, or NULL at the end.
 *
 * DESIGN.md section 9: for a CHAIN set this is the one place a source page
 * pin is dropped, and it happens only once the caller has finished with every
 * item of that page - see lion_count_container(), which calls
 * lion_cursor_next() immediately after the visibility-map checks and nowhere
 * else.
 */
static const LionContainer *
lion_cursor_next_item(LionSetCursor *cur)
{
	if (cur->set->mat != NULL)
	{
		const LionMatSet *mat = cur->set->mat;

		if (cur->matidx >= mat->ncontainers)
			return NULL;
		return mat->containers[cur->matidx++];
	}

	if (cur->set->is_inline)
	{
		if (lion_inline_fetch(cur->set->payload, cur->set->paylen,
							 &cur->payoff, cur->cbuf) == 0)
			return NULL;		/* payload exhausted; the set keeps its pin */
		return cur->cbuf;
	}

	for (;;)
	{
		Buffer		buf;
		Page		page;

		/* Finish the page we already have in hand. */
		while (BufferIsValid(cur->pinbuf) && cur->off <= cur->maxoff)
		{
			ItemId		iid = PageGetItemId(cur->img, cur->off);

			cur->off = OffsetNumberNext(cur->off);
			if (!ItemIdIsUsed(iid))
				continue;
			return (const LionContainer *) PageGetItem(cur->img, iid);
		}

		/* Its items have all been consumed: the pin may go. */
		lion_cursor_unpin(cur);

		if (!BlockNumberIsValid(cur->nextblk))
			return NULL;

		buf = ReadBuffer(cur->set->index, cur->nextblk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (!LionPageIsContainer(page))
		{
			UnlockReleaseBuffer(buf);
			elog(ERROR, "lion index: block %u is not a container page",
				 cur->nextblk);
		}

		/*
		 * Copy the page - items and rightlink together, as lion_scan.c does -
		 * then drop the content lock but keep the pin (DESIGN.md section 9).
		 */
		memcpy(cur->img, page, BLCKSZ);
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		cur->pinbuf = buf;
		cur->ownpin = true;

		cur->nextblk = LionPageGetOpaque(cur->img)->rightlink;
		cur->off = FirstOffsetNumber;
		cur->maxoff = PageGetMaxOffsetNumber(cur->img);

		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * Build the next container key of the segment being expanded into segbuf and
 * point the cursor at it.  Returns false when the segment is used up.
 *
 * The result is a throw-away ARRAY container: the merge, the AND and the
 * visibility-map mask treat it exactly like one read from a page, and it
 * lives only until the next call.  The segment itself is left alone, and so
 * is the pin on the page it came from.
 */
static bool
lion_cursor_emit_segment(LionSetCursor *cur)
{
	const LionContainer *seg = cur->seg;
	const uint32 *ckeys = LION_SPARSE_CKEYS_CONST(seg);
	const uint16 *los = LION_SPARSE_LOS_CONST(seg);
	uint32		n = seg->cardinality;
	uint32		ckey;

	if (cur->segpos >= n)
		return false;

	/*
	 * Allocated on first use, not per cursor: an IN list of a thousand values
	 * (DESIGN.md §15) is a thousand cursors, and most posting sets hold no
	 * sparse segment at all.
	 */
	if (cur->segbuf == NULL)
		cur->segbuf = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);

	ckey = ckeys[cur->segpos];
	lion_container_init(cur->segbuf, ckey);
	do
	{
		lion_container_append_sorted(cur->segbuf, los[cur->segpos]);
		cur->segpos++;
	} while (cur->segpos < n && ckeys[cur->segpos] == ckey);

	cur->cur = cur->segbuf;
	cur->valid = true;
	cur->cx->stats.containers_visited++;
	return true;
}

/*
 * Advance to the next container of the set, expanding sparse segments one
 * container key at a time (see the comment on LionSetCursor).
 */
static void
lion_cursor_next(LionSetCursor *cur)
{
	cur->valid = false;
	cur->cur = NULL;

	if (cur->set == NULL || !cur->set->found)
		return;

	/* Still inside a segment?  Its next container key is the next container. */
	if (cur->seg != NULL)
	{
		if (lion_cursor_emit_segment(cur))
			return;
		cur->seg = NULL;
	}

	for (;;)
	{
		const LionContainer *item = lion_cursor_next_item(cur);

		if (item == NULL)
			return;

		if (item->type != LION_CT_SPARSE)
		{
			cur->cur = item;
			cur->valid = true;
			cur->cx->stats.containers_visited++;
			return;
		}

		cur->seg = item;
		cur->segpos = 0;
		if (lion_cursor_emit_segment(cur))
			return;
		cur->seg = NULL;		/* an empty segment: nothing to present */
	}
}

static void
lion_cursor_close(LionSetCursor *cur)
{
	lion_cursor_unpin(cur);
	cur->seg = NULL;
	cur->valid = false;
	cur->cur = NULL;
}


/* ---------------------------------------------------------------------
 * Expression cursors: one source of the merge
 * --------------------------------------------------------------------- */

/*
 * A cursor over a boolean expression of posting sets, presenting one
 * ascending run of container keys just as a single set does, so that the
 * merge below need not know what is behind a source.
 *
 * Three node kinds, which are exactly the three LionKeyNode kinds:
 *
 *	LEAF	one posting set, walked by an LionSetCursor.
 *	OR		the union (DESIGN.md §15's IN lists, and `tags && '{a,b}'`): the
 *			cursor stands at the SMALLEST container key any child has left,
 *			and its container is the OR of the containers of every child
 *			standing at that key.  With a thousand children - which §15's
 *			longest IN list has - neither of those may be done by walking all
 *			of them: the smallest key comes off a binary MIN-HEAP of the
 *			children, keyed by their current container key, and the union of
 *			the children that stand at it is accumulated in one pass through
 *			a bitset image instead of k-1 pairwise unions that each build and
 *			re-optimize an intermediate container.  Both costs are then
 *			proportional to the containers that actually take part rather
 *			than to the length of the list.
 *	AND		the intersection (`tags @> '{a,b}'`, and the AND nodes of a
 *			tsquery, DESIGN.md §17): the children are wound forward until
 *			they all stand at one container key, and the container is the AND
 *			of theirs.  A container key whose intersection comes out empty is
 *			skipped here rather than handed up.
 *
 * THE PIN RULE (DESIGN.md §9) IS UNCHANGED BY EITHER OPERATOR.  Every leaf
 * that contributed a container to the result still pins the page that
 * container was copied from, because a leaf is only advanced by
 * lion_ecursor_next(), and the merge only calls that from
 * lion_count_container(), after the visibility map has been consulted for the
 * merged container.  Leaves that are ahead of the current key hold their own
 * pins as well, which is harmless: a pin too many never makes a count wrong,
 * it only makes VACUUM wait.
 *
 * Skipping is the one place a pin goes without anything having been counted
 * from it - an AND winding a lagging child forward, or dropping a container
 * key whose intersection is empty.  That is safe for the reason the merge's
 * own `!alleq` branch is safe: nothing of that container key reaches the
 * visibility map, so no answer rests on it.
 */
typedef struct LionOrHeapEnt
{
	uint32		ckey;			/* sub[child].ckey when it was pushed */
	int32		child;
} LionOrHeapEnt;

typedef struct LionExprCursor
{
	const LionKeyNode *node;		/* NULL: an empty source, never valid */
	LionKeyNodeKind kind;

	/* LION_KN_KEY */
	LionSetCursor leaf;

	/* LION_KN_AND / LION_KN_OR */
	int			nsub;
	struct LionExprCursor *sub;
	LionContainer *acc[2];		/* AND/OR accumulators, only when nsub > 1 */

	/*
	 * LION_KN_OR, the k-way merge.  heap[0 .. nheap-1] is a min-heap of every
	 * child that still has a container and is not standing at the current
	 * key; hot[0 .. nhot-1] are the children that are, the ones whose
	 * containers the current result was built from and whose pins therefore
	 * carry the §9 interlock.  bits is the accumulator for a union of many of
	 * them.
	 *
	 * The heap carries each child's container key INSIDE the entry rather
	 * than reading sub[i].ckey while it sifts: a thousand-element IN list is
	 * a thousand LionExprCursors, a third of a megabyte, and chasing them
	 * through the heap's random access pattern cost more than the linear scan
	 * over all children that the heap replaced (measured: +4 ms on a
	 * 1000-value list at 1M rows).  A child's key only changes when the child
	 * is advanced, which is also when it is pushed back on.
	 */
	struct LionOrHeapEnt *heap;
	int			nheap;
	int		   *hot;
	int			nhot;
	uint64	   *bits;

	/* the container the cursor currently stands on */
	bool		valid;
	uint32		ckey;
	const LionContainer *cur;

	bool		advance;		/* top level only: took part in this key */
} LionExprCursor;

static void lion_ecursor_build(LionExprCursor *c);
static void lion_ecursor_next(LionExprCursor *c);

/* ---- the OR node's min-heap of children, keyed by container key ---- */

static inline void
lion_or_heap_push(LionExprCursor *c, int child)
{
	LionOrHeapEnt ent;
	int			i = c->nheap++;

	Assert(c->sub[child].valid);
	ent.ckey = c->sub[child].ckey;
	ent.child = child;

	while (i > 0)
	{
		int			parent = (i - 1) / 2;

		if (c->heap[parent].ckey <= ent.ckey)
			break;
		c->heap[i] = c->heap[parent];
		i = parent;
	}
	c->heap[i] = ent;
}

static inline int
lion_or_heap_pop(LionExprCursor *c)
{
	int			top = c->heap[0].child;
	LionOrHeapEnt last;
	int			i = 0;

	Assert(c->nheap > 0);
	if (--c->nheap == 0)
		return top;

	last = c->heap[c->nheap];
	for (;;)
	{
		int			l = 2 * i + 1;
		int			r = l + 1;
		int			small = i;
		uint32		smallkey = last.ckey;

		if (l < c->nheap && c->heap[l].ckey < smallkey)
		{
			small = l;
			smallkey = c->heap[l].ckey;
		}
		if (r < c->nheap && c->heap[r].ckey < smallkey)
			small = r;
		if (small == i)
			break;
		c->heap[i] = c->heap[small];
		i = small;
	}
	c->heap[i] = last;
	return top;
}

/* ---- the union of more than two containers, in one pass ---- */

/*
 * OR one container into a bitset image of a whole container key's range.
 * This is lion_container.c's own container_or_bitset(), which is private to
 * that module; it is repeated here rather than exported because the union of
 * k containers is this file's problem (DESIGN.md §15) and the shape of a
 * container payload is lion_container.h's published interface.
 */
static void
lion_bits_or_container(uint64 *w, const LionContainer *c)
{
	const char *payload = (const char *) c + LION_CONTAINER_HDRSZ;
	uint32		i;

	switch (c->type)
	{
		case LION_CT_ARRAY:
			{
				const uint16 *arr = (const uint16 *) payload;

				for (i = 0; i < c->cardinality; i++)
					w[arr[i] >> 6] |= UINT64CONST(1) << (arr[i] & 63);
				break;
			}
		case LION_CT_BITSET:
			{
				const uint64 *src = (const uint64 *) payload;
				int			k;

				for (k = 0; k < LION_BITSET_WORDS; k++)
					w[k] |= src[k];
				break;
			}
		case LION_CT_RUN:
			{
				uint32		nruns = *(const uint16 *) payload;
				const LionRun *runs = (const LionRun *) (payload + sizeof(uint16));

				for (i = 0; i < nruns; i++)
				{
					uint32		first = runs[i].start;
					uint32		last = first + runs[i].len_minus_1;
					uint32		fw = first >> 6;
					uint32		lw = last >> 6;
					uint64		fmask = PG_UINT64_MAX << (first & 63);
					uint64		lmask = PG_UINT64_MAX >> (63 - (last & 63));

					Assert(last < LION_CONTAINER_RANGE);
					if (fw == lw)
						w[fw] |= fmask & lmask;
					else
					{
						uint32		j;

						w[fw] |= fmask;
						for (j = fw + 1; j < lw; j++)
							w[j] = PG_UINT64_MAX;
						w[lw] |= lmask;
					}
				}
				break;
			}
		default:
			Assert(false);		/* a sparse segment is never a container */
			break;
	}
}

/*
 * Turn the accumulated image into a container in dest (capacity
 * LION_CONTAINER_MAX_SIZE), in the smallest representation, exactly as
 * lion_container_or() would have left it.
 */
static void
lion_bits_to_container(const uint64 *w, uint32 ckey, LionContainer *dest)
{
	uint64		card = 0;
	int			k;

	for (k = 0; k < LION_BITSET_WORDS; k++)
		card += pg_popcount64(w[k]);

	lion_container_init(dest, ckey);
	if (card == 0)
		return;					/* an empty ARRAY; the caller drops it */

	/*
	 * Written straight into the payload rather than through
	 * lion_container_append_sorted() once per member: the image IS a BITSET
	 * payload, so the whole container key costs one memcpy whatever its
	 * cardinality.  lion_container_optimize() then picks the representation,
	 * and in assert builds lion_container_check() confirms that what was built
	 * by hand is a container the rest of the code may be handed.
	 */
	Assert(card <= LION_CONTAINER_RANGE);
	lion_container_to_bitset(dest);
	memcpy(LION_BITSET_DATA(dest), w, LION_BITSET_BYTES);
	dest->cardinality = (uint16) card;
	lion_container_optimize(dest);

#ifdef USE_ASSERT_CHECKING
	{
		const char *why = NULL;

		Assert(lion_container_check(dest, LION_CONTAINER_MAX_SIZE, &why));
	}
#endif
}

static void
lion_ecursor_init(LionExprCursor *c, const LionKeyNode *node,
				 LionPostingSet *sets, int nsets, LionCountCtx *cx)
{
	int			i;

	check_stack_depth();

	memset(c, 0, sizeof(LionExprCursor));
	c->node = node;
	if (node == NULL)
		return;					/* a source with no sets at all */

	c->kind = node->kind;

	if (node->kind == LION_KN_KEY)
	{
		Assert(node->keyno >= 0 && node->keyno < nsets);
		lion_cursor_init(&c->leaf, &sets[node->keyno], cx);
	}
	else
	{
		Assert(node->nargs >= 1);
		c->nsub = node->nargs;
		c->sub = (LionExprCursor *) palloc0(sizeof(LionExprCursor) * c->nsub);
		for (i = 0; i < c->nsub; i++)
			lion_ecursor_init(&c->sub[i], node->args[i], sets, nsets, cx);

		if (c->nsub > 1)
		{
			c->acc[0] = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
			c->acc[1] = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
		}

		if (node->kind == LION_KN_OR)
		{
			c->heap = (LionOrHeapEnt *) palloc(sizeof(LionOrHeapEnt) * c->nsub);
			c->hot = (int *) palloc(sizeof(int) * c->nsub);
			if (c->nsub > 2)
				c->bits = (uint64 *) palloc(LION_BITSET_BYTES);

			/*
			 * Every child that has a container goes on the heap; build()
			 * takes the ones standing at the smallest key back off it.
			 */
			for (i = 0; i < c->nsub; i++)
			{
				if (c->sub[i].valid)
					lion_or_heap_push(c, i);
			}
		}
	}

	lion_ecursor_build(c);
}

/*
 * Recompute the cursor's current container from its children.
 */
static void
lion_ecursor_build(LionExprCursor *c)
{
	const LionContainer *acc;
	int			w = 0;
	int			i;

	c->valid = false;
	c->cur = NULL;

	if (c->node == NULL)
		return;

	if (c->kind == LION_KN_KEY)
	{
		if (!c->leaf.valid)
			return;
		c->cur = c->leaf.cur;
		c->ckey = c->cur->ckey;
		c->valid = true;
		return;
	}

	if (c->kind == LION_KN_OR)
	{
		uint32		minckey;

		/*
		 * The k-way merge.  Everything that still has a container is on the
		 * heap, so its root IS the smallest container key any child has left;
		 * the children standing at it come off the heap into hot[] and stay
		 * there until lion_ecursor_next() moves past the key, which is what
		 * keeps their pins - and with them the §9 interlock - in place for as
		 * long as the result is being counted.
		 */
		Assert(c->nhot == 0);
		if (c->nheap == 0)
			return;				/* every child is exhausted */

		minckey = c->heap[0].ckey;
		do
		{
			c->hot[c->nhot++] = lion_or_heap_pop(c);
		} while (c->nheap > 0 && c->heap[0].ckey == minckey);

		if (c->nhot == 1)
			c->cur = c->sub[c->hot[0]].cur;
		else if (c->nhot < LION_OR_BITSET_MIN)
		{
			const LionContainer *a = c->sub[c->hot[0]].cur;

			for (i = 1; i < c->nhot; i++)
			{
				lion_container_or(a, c->sub[c->hot[i]].cur, c->acc[w]);
				a = c->acc[w];
				w ^= 1;
			}
			c->cur = a;
		}
		else
		{
			/* One pass over the containers, one container built at the end. */
			memset(c->bits, 0, LION_BITSET_BYTES);
			for (i = 0; i < c->nhot; i++)
				lion_bits_or_container(c->bits, c->sub[c->hot[i]].cur);
			lion_bits_to_container(c->bits, minckey, c->acc[0]);
			c->cur = c->acc[0];
		}

		c->ckey = minckey;
		c->valid = true;
		return;
	}

	Assert(c->kind == LION_KN_AND);

	for (;;)
	{
		uint32		maxckey;
		bool		alleq = true;

		for (i = 0; i < c->nsub; i++)
		{
			if (!c->sub[i].valid)
				return;			/* a child ran out: so has the intersection */
		}

		maxckey = c->sub[0].ckey;
		for (i = 1; i < c->nsub; i++)
		{
			if (c->sub[i].ckey > maxckey)
				maxckey = c->sub[i].ckey;
		}

		/* Wind the laggards forward; their containers cannot contribute. */
		for (i = 0; i < c->nsub; i++)
		{
			if (c->sub[i].ckey != maxckey)
			{
				alleq = false;
				lion_ecursor_next(&c->sub[i]);
			}
		}
		if (!alleq)
		{
			CHECK_FOR_INTERRUPTS();
			continue;
		}

		acc = c->sub[0].cur;
		w = 0;
		for (i = 1; i < c->nsub; i++)
		{
			lion_container_and(acc, c->sub[i].cur, c->acc[w]);
			acc = c->acc[w];
			w ^= 1;
		}

		if (lion_container_cardinality(acc) > 0)
		{
			c->ckey = maxckey;
			c->cur = acc;
			c->valid = true;
			return;
		}

		/*
		 * Nothing of this container key survives the intersection, so nothing
		 * will ask the visibility map about it and every child may move on.
		 */
		for (i = 0; i < c->nsub; i++)
			lion_ecursor_next(&c->sub[i]);

		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * Move past the current container key.  Only the children that stand at it
 * move; the ones that are ahead (an OR's) stay where they are.  This is the
 * only place a source lets go of a page pin that carried an answer.
 */
static void
lion_ecursor_next(LionExprCursor *c)
{
	int			i;

	if (c->node == NULL || !c->valid)
		return;

	switch (c->kind)
	{
		case LION_KN_KEY:
			lion_cursor_next(&c->leaf);
			break;

		case LION_KN_OR:

			/*
			 * Only the children that stood at this key move; the ones still
			 * on the heap are ahead of it and stay where they are.  A child
			 * that has a container again goes back on the heap, which is the
			 * one place an OR lets go of a page that carried an answer.
			 */
			for (i = 0; i < c->nhot; i++)
			{
				int			child = c->hot[i];

				lion_ecursor_next(&c->sub[child]);
				if (c->sub[child].valid)
					lion_or_heap_push(c, child);
			}
			c->nhot = 0;
			break;

		case LION_KN_AND:
			/* every child stands at c->ckey and contributed to the result */
			for (i = 0; i < c->nsub; i++)
				lion_ecursor_next(&c->sub[i]);
			break;
	}

	lion_ecursor_build(c);
}

static void
lion_ecursor_close(LionExprCursor *c)
{
	int			i;

	if (c->node == NULL)
		return;

	if (c->kind == LION_KN_KEY)
		lion_cursor_close(&c->leaf);
	else
	{
		for (i = 0; i < c->nsub; i++)
			lion_ecursor_close(&c->sub[i]);
	}

	c->nheap = 0;
	c->nhot = 0;
	c->valid = false;
	c->cur = NULL;
}

/* ---------------------------------------------------------------------
 * Source expressions
 * --------------------------------------------------------------------- */

/*
 * The tree a source combines its sets with: its own, or the implicit union of
 * all of them.  NULL when the source has no sets at all, which only a negated
 * source can have (a `col IS NOT NULL` on a column with no NULLs).
 */
static LionKeyNode *
lion_source_tree(const LionCountSource *src)
{
	LionKeyNode *node;
	LionKeyNode **args;
	int			i;

	if (src->tree != NULL)
		return src->tree;
	if (src->nsets == 0)
		return NULL;

	args = (LionKeyNode **) palloc(sizeof(LionKeyNode *) * src->nsets);
	for (i = 0; i < src->nsets; i++)
	{
		args[i] = (LionKeyNode *) palloc0(sizeof(LionKeyNode));
		args[i]->kind = LION_KN_KEY;
		args[i]->keyno = i;
	}
	if (src->nsets == 1)
		return args[0];

	node = (LionKeyNode *) palloc0(sizeof(LionKeyNode));
	node->kind = LION_KN_OR;
	node->nargs = src->nsets;
	node->args = args;
	return node;
}

/*
 * Can this expression select anything at all?  A key with no entry in the
 * index selects nothing, and an AND of one such key selects nothing however
 * many other keys it has.  Answering that up front is what lets a count over
 * an impossible clause cost one bucket lookup per key and no merge.
 */
static bool
lion_source_satisfiable(const LionKeyNode *node, const LionPostingSet *sets)
{
	int			i;

	if (node == NULL)
		return false;

	switch (node->kind)
	{
		case LION_KN_KEY:
			return sets[node->keyno].found;

		case LION_KN_AND:
			for (i = 0; i < node->nargs; i++)
			{
				if (!lion_source_satisfiable(node->args[i], sets))
					return false;
			}
			return true;

		case LION_KN_OR:
			for (i = 0; i < node->nargs; i++)
			{
				if (lion_source_satisfiable(node->args[i], sets))
					return true;
			}
			return false;
	}

	return false;
}

/*
 * Does every container this expression can yield come with a live buffer pin
 * on the page it was read from?  That is the DESIGN.md §9 interlock, and
 * lion_count_sources() has to keep at least one positive source that has it
 * (see the comment on lion_posting_set_materialize()).
 *
 *	- a leaf has it unless its set has been materialized; a leaf whose key has
 *	  no entry yields nothing, so it has it vacuously;
 *	- an AND has it if ANY child has it, because every child stands at the
 *	  container key the result was built from and so every child's pin is
 *	  still held when the result is counted;
 *	- an OR has it only if EVERY child has it, because which children
 *	  contributed to a given container key is not known in advance.
 */
static bool
lion_source_pinned(const LionKeyNode *node, const LionPostingSet *sets)
{
	int			i;

	if (node == NULL)
		return true;			/* yields nothing */

	switch (node->kind)
	{
		case LION_KN_KEY:
			return !sets[node->keyno].found || sets[node->keyno].mat == NULL;

		case LION_KN_AND:
			for (i = 0; i < node->nargs; i++)
			{
				if (lion_source_pinned(node->args[i], sets))
					return true;
			}
			return false;

		case LION_KN_OR:
			for (i = 0; i < node->nargs; i++)
			{
				if (!lion_source_pinned(node->args[i], sets))
					return false;
			}
			return true;
	}

	return false;
}


/* ---------------------------------------------------------------------
 * The visibility map, a container at a time
 * --------------------------------------------------------------------- */

/*
 * The layout of a visibility map page.  These mirror the private macros of
 * the same name in src/backend/access/heap/visibilitymap.c, which is the only
 * place the format is written down; nothing outside that file exports them.
 * Keep them in step with it.  The static assertions below pin down the parts
 * of the format that visibilitymapdefs.h does export, which is what the bit
 * twiddling in lion_vm_allvisible_mask() actually depends on.
 */
#define LION_VM_MAPSIZE				(BLCKSZ - MAXALIGN(SizeOfPageHeaderData))
#define LION_VM_HEAPBLOCKS_PER_BYTE	(BITS_PER_BYTE / BITS_PER_HEAPBLOCK)
#define LION_VM_HEAPBLOCKS_PER_PAGE	(LION_VM_MAPSIZE * LION_VM_HEAPBLOCKS_PER_BYTE)
#define LION_VM_HEAPBLK_TO_MAPBYTE(x) \
	(((x) % LION_VM_HEAPBLOCKS_PER_PAGE) / LION_VM_HEAPBLOCKS_PER_BYTE)

StaticAssertDecl(BITS_PER_HEAPBLOCK == 2,
				 "pg_lion: the visibility map is no longer two bits per heap block");
StaticAssertDecl(VISIBILITYMAP_VALID_BITS == 0x03,
				 "pg_lion: unexpected visibility map bit assignment");
StaticAssertDecl(VISIBILITYMAP_ALL_VISIBLE == 0x01,
				 "pg_lion: all-visible is no longer the low bit of each pair");
/* the masks below are uint64s, one bit per heap block a container covers */
StaticAssertDecl(LION_BLOCKS_PER_CONTAINER <= 64,
				 "pg_lion: a container covers more heap blocks than a mask holds");
/*
 * A container's heap blocks start at a multiple of LION_BLOCKS_PER_CONTAINER,
 * and both that and the number of blocks per map page are multiples of the
 * number of blocks per map byte, so every run of blocks lion_vm_allvisible_mask()
 * reads begins and ends on a byte boundary of the map.
 */
StaticAssertDecl(LION_BLOCKS_PER_CONTAINER % LION_VM_HEAPBLOCKS_PER_BYTE == 0,
				 "pg_lion: container block range is not map-byte aligned");
StaticAssertDecl(LION_VM_HEAPBLOCKS_PER_PAGE % LION_VM_HEAPBLOCKS_PER_BYTE == 0,
				 "pg_lion: map page does not hold a whole number of map bytes");

/*
 * The all-visible bits of the LION_BLOCKS_PER_CONTAINER consecutive heap blocks
 * a container covers, as one mask: bit i is set iff heap block firstblk + i is
 * marked all-visible.  *vmbuf is the caller's visibility map pin; it is moved
 * to whatever map page is needed and left pinned for the next call, exactly as
 * visibilitymap_get_status() leaves it.
 *
 * This is visibilitymap_get_status() for a whole container at once.  It exists
 * because asking a block at a time costs a buffer-manager lookup per heap block
 * covered - 64 of them per container at 8K - and that was the entire cost of a
 * count over an all-visible table, where DESIGN.md section 9 wants O(1) work per
 * container.  The map bytes are read straight off the page with no lock, just as
 * visibilitymap_get_status() reads its one byte; its comment is the license:
 *
 *		"NOTE: This function is typically called without a lock on the heap
 *		 page, so somebody else could change the bit just after we look at it.
 *		 In fact, since we don't lock the visibility map page either, it's even
 *		 possible that someone else could have changed the bit just before we
 *		 look at it, but yet we might see the old value.  It is the caller's
 *		 responsibility to deal with all concurrency issues!"
 *
 * and DESIGN.md section 9 is where this file deals with them: a bit that is
 * stale in the "recently cleared" direction can only have been cleared by a
 * transaction whose tuples our snapshot cannot see (the index-only scan
 * argument, spelled out in that same comment), and a bit cannot become set
 * behind our back because the caller still pins the index page the container
 * was read from, which is what stops VACUUM finishing ambulkdelete().  Reading
 * sixteen adjacent bytes instead of one changes none of that: each byte is
 * still an independent unlocked read of the same page.
 *
 * The map fork is never extended.  visibilitymap_pin() would extend it - it
 * calls vm_readbuf() with extend = true, which writes - and a count has no
 * business growing the map, so the pin is taken the way
 * visibilitymap_get_status() takes it, and blocks the fork does not reach are
 * simply not all-visible.  That also covers the blocks past the end of the heap
 * that a container's range may include: they have no members, so nothing the
 * mask says about them is ever read.
 */
static uint64
lion_vm_allvisible_mask(Relation heap, BlockNumber firstblk, Buffer *vmbuf)
{
	uint64		mask = 0;
	int			b = 0;

	Assert(firstblk % LION_BLOCKS_PER_CONTAINER == 0);

	/*
	 * Usually one pass.  LION_VM_HEAPBLOCKS_PER_PAGE (32672 at 8K) is NOT a
	 * multiple of LION_BLOCKS_PER_CONTAINER, so roughly one container in five
	 * hundred straddles two map pages and needs two - which is why this is a
	 * loop and not sixteen bytes read in one go.
	 */
	while (b < LION_BLOCKS_PER_CONTAINER)
	{
		BlockNumber blk = firstblk + (BlockNumber) b;
		int			n;

		/* how many of the blocks still wanted live on blk's map page */
		n = (int) (LION_VM_HEAPBLOCKS_PER_PAGE -
				   (blk % LION_VM_HEAPBLOCKS_PER_PAGE));
		n = Min(n, LION_BLOCKS_PER_CONTAINER - b);
		Assert(n > 0 && n % LION_VM_HEAPBLOCKS_PER_BYTE == 0);

		/*
		 * Take the pin the way visibilitymap_get_status() does: same buffer
		 * reuse, same refusal to extend the fork.  Its return value is the
		 * status of blk itself, which the byte read below repeats.
		 */
		if (!visibilitymap_pin_ok(blk, *vmbuf))
			(void) visibilitymap_get_status(heap, blk, vmbuf);

		if (BufferIsValid(*vmbuf))
		{
			const char *map = (const char *) PageGetContents(BufferGetPage(*vmbuf));
			uint32		mapbyte = LION_VM_HEAPBLK_TO_MAPBYTE(blk);
			int			nbytes = n / LION_VM_HEAPBLOCKS_PER_BYTE;
			int			j;

			for (j = 0; j < nbytes; j++)
			{
				/*
				 * Squeeze the four all-visible bits of the byte - the low bit
				 * of each pair - down into a nibble of the result.
				 */
				uint8		v = (uint8) (map[mapbyte + j] & 0x55);

				v = (uint8) ((v | (v >> 1)) & 0x33);
				v = (uint8) ((v | (v >> 2)) & 0x0f);
				mask |= ((uint64) v) << (b + j * LION_VM_HEAPBLOCKS_PER_BYTE);
			}
		}
		/* else the fork stops short of these blocks: none of them is all-visible */

		b += n;
	}

	return mask;
}

/*
 * The other half of the pair: which of the heap blocks a container covers have
 * at least one member, in the same bit numbering.
 *
 * One pass over the container, whatever its representation.  The obvious
 * alternative - lion_container_range_cardinality() once per block range - is
 * LION_BLOCKS_PER_CONTAINER binary searches whether the container holds three
 * members or thirty-two thousand, and that cost is paid even when the answer
 * is going to be "all of it is all-visible, count the cardinality".
 */
#define LION_BITSET_WORDS_PER_BLOCK	(LION_BITSET_WORDS / LION_BLOCKS_PER_CONTAINER)

StaticAssertDecl(LION_BITSET_WORDS_PER_BLOCK * LION_BLOCKS_PER_CONTAINER ==
				 LION_BITSET_WORDS,
				 "pg_lion: bitset words do not divide evenly among heap blocks");

static uint64
lion_container_block_mask(const LionContainer *c)
{
	const char *payload = (const char *) c + LION_CONTAINER_HDRSZ;
	uint64		mask = 0;
	uint32		i;

	switch (c->type)
	{
		case LION_CT_ARRAY:
			{
				const uint16 *arr = (const uint16 *) payload;

				for (i = 0; i < c->cardinality; i++)
					mask |= UINT64CONST(1) << (arr[i] >> LION_OFFSET_BITS);
				break;
			}
		case LION_CT_BITSET:
			{
				const uint64 *w = (const uint64 *) payload;
				int			b;

				for (b = 0; b < LION_BLOCKS_PER_CONTAINER; b++)
				{
					uint64		any = 0;
					int			k;

					for (k = 0; k < LION_BITSET_WORDS_PER_BLOCK; k++)
						any |= w[b * LION_BITSET_WORDS_PER_BLOCK + k];
					if (any != 0)
						mask |= UINT64CONST(1) << b;
				}
				break;
			}
		case LION_CT_RUN:
			{
				uint32		nruns = *(const uint16 *) payload;
				const LionRun *runs = (const LionRun *) (payload + sizeof(uint16));

				for (i = 0; i < nruns; i++)
				{
					uint32		first = runs[i].start >> LION_OFFSET_BITS;
					uint32		last = (((uint32) runs[i].start +
										 runs[i].len_minus_1) >> LION_OFFSET_BITS);

					Assert(last < LION_BLOCKS_PER_CONTAINER);
					mask |= (PG_UINT64_MAX >> (63 - last)) &
						(PG_UINT64_MAX << first);
				}
				break;
			}
		default:
			Assert(false);
			break;
	}

	return mask;
}

/*
 * Assert builds cross-check every mask against the function it replaces.  This
 * reinstates exactly the per-block cost the mask exists to avoid, so timings
 * taken on a cassert cluster want -DRBI_NO_VM_MASK_CHECK.
 */
#if defined(USE_ASSERT_CHECKING) && !defined(LION_NO_VM_MASK_CHECK)
#define LION_VM_MASK_CHECK 1
#endif

#ifdef LION_VM_MASK_CHECK
/*
 * visibilitymap_get_status() for every block that has members - the only bits
 * of the mask the count looks at - must agree with the mask.
 *
 * It legitimately might not, if a concurrent VACUUM or DML changed a bit
 * between the two reads, so the comparison is only made when a fresh read of
 * the whole mask still matches the one under test.  That second read is what
 * keeps this assertion from being a race.
 */
static void
lion_vm_mask_check(Relation heap, BlockNumber firstblk, uint64 members,
				  uint64 allvis, Buffer *vmbuf)
{
	uint64		expect = 0;
	uint64		m = members;

	while (m != 0)
	{
		int			b = pg_rightmost_one_pos64(m);

		m &= m - 1;
		if ((visibilitymap_get_status(heap, firstblk + (BlockNumber) b, vmbuf) &
			 VISIBILITYMAP_ALL_VISIBLE) != 0)
			expect |= UINT64CONST(1) << b;
	}

	if (lion_vm_allvisible_mask(heap, firstblk, vmbuf) == allvis)
		Assert((allvis & members) == expect);
}
#endif

/* ---------------------------------------------------------------------
 * The per-query visibility cache
 * --------------------------------------------------------------------- */

/*
 * One heap page's answer: which of its root line pointers hold a tuple
 * visible to the snapshot the cache was filled under.  Bit (off - 1) stands
 * for offset number off; offsets above MaxHeapTuplesPerPage cannot exist on a
 * heap page and are never asked about (lion_vis_entry_visible() says no).
 *
 * 291 bits at the default page size, so 40 bytes of bitmap and 48 of entry.
 */
#define LION_VIS_WORDS	(((MaxHeapTuplesPerPage - 1) / 64) + 1)

typedef struct LionVisEntry
{
	BlockNumber blkno;			/* hash key: the heap block */
	bool		filled;			/* false: only the visit was recorded */
	char		status;			/* simplehash's own field */
	uint64		vis[LION_VIS_WORDS];
} LionVisEntry;

#define SH_PREFIX		lion_visht
#define SH_ELEMENT_TYPE LionVisEntry
#define SH_KEY_TYPE		BlockNumber
#define SH_KEY			blkno
#define SH_HASH_KEY(tb, key)	murmurhash32(key)
#define SH_EQUAL(tb, a, b)		((a) == (b))
#define SH_SCOPE		static inline
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

/*
 * The cache itself.  relid and the snapshot fields are not a lookup key but a
 * guard: a handle that is handed a different relation or a different snapshot
 * empties itself rather than answering from entries that were resolved under
 * something else (DESIGN.md §16 walks the partitions of one count one at a
 * time, which is exactly that case).
 */
struct LionVisCache
{
	MemoryContext cxt;			/* holds ht and nothing else */
	lion_visht_hash *ht;
	Oid			relid;			/* relation the entries belong to */
	Snapshot	snapshot;		/* snapshot they were resolved under ... */
	TransactionId xmin;			/* ... and enough of its identity to notice */
	TransactionId xmax;			/* that it has been replaced */
	CommandId	curcid;
	int			ncounts;		/* counts served since the last reset */
	int			maxentries;		/* work_mem budget, in entries */
	bool		full;			/* budget reached: stop inserting */
};

/*
 * How many entries work_mem allows.
 *
 * An open-addressing table keeps more slots than members (simplehash grows at
 * a fill factor of 0.9) and allocates the bigger array before freeing the
 * smaller one while it grows, so the nominal entry budget is a third of what
 * work_mem would buy outright; that keeps the real high-water mark at or
 * below work_mem, which is the promise the GUC makes.
 */
static int
lion_vis_cache_budget(void)
{
	int64		budget = ((int64) work_mem * INT64CONST(1024)) /
		((int64) sizeof(LionVisEntry) * 3);

	if (budget < 64)
		budget = 64;			/* a tiny work_mem still caches something */
	if (budget > INT_MAX / 2)
		budget = INT_MAX / 2;
	return (int) budget;
}

LionVisCache *
lion_vis_cache_create(MemoryContext parent)
{
	LionVisCache *cache;

	cache = (LionVisCache *) MemoryContextAllocZero(parent, sizeof(LionVisCache));
	cache->cxt = AllocSetContextCreate(parent,
									   "LionCount visibility cache",
									   ALLOCSET_SMALL_SIZES);
	return cache;
}

void
lion_vis_cache_reset(LionVisCache *cache)
{
	if (cache == NULL)
		return;
	cache->ht = NULL;
	MemoryContextReset(cache->cxt);
	cache->relid = InvalidOid;
	cache->snapshot = NULL;
	cache->ncounts = 0;
	cache->full = false;
}

void
lion_vis_cache_destroy(LionVisCache *cache)
{
	if (cache == NULL)
		return;
	cache->ht = NULL;
	if (cache->cxt != NULL)
		MemoryContextDelete(cache->cxt);
	cache->cxt = NULL;
}

/*
 * Attach the cache to one count: empty it if it was filled for another
 * relation or under another snapshot, and note that another count has begun.
 *
 * The relation is the check that matters in practice - §16 walks the
 * partitions of one count one at a time, and block numbers mean different
 * things in each - and it is exact.  The snapshot check is a guard rather
 * than a proof: what makes reuse safe is that the driver holds one snapshot
 * for the whole node execution, so the object it hands us cannot be freed and
 * replaced underneath it; two distinct snapshots with the same pointer, xmin,
 * xmax and curcid could still differ in their in-progress list.  A caller
 * that wants a different snapshot must call lion_vis_cache_reset().
 */
static void
lion_vis_cache_begin(LionVisCache *cache, Relation heap, Snapshot snapshot)
{
	if (cache == NULL || snapshot == NULL)
		return;

	if (cache->relid != RelationGetRelid(heap) ||
		cache->snapshot != snapshot ||
		cache->xmin != snapshot->xmin ||
		cache->xmax != snapshot->xmax ||
		cache->curcid != snapshot->curcid)
	{
		lion_vis_cache_reset(cache);
		cache->relid = RelationGetRelid(heap);
		cache->snapshot = snapshot;
		cache->xmin = snapshot->xmin;
		cache->xmax = snapshot->xmax;
		cache->curcid = snapshot->curcid;
	}

	cache->maxentries = lion_vis_cache_budget();
	if (cache->ncounts < INT_MAX)
		cache->ncounts++;
}

static inline bool
lion_vis_entry_visible(const LionVisEntry *e, OffsetNumber off)
{
	int			bit = (int) off - 1;

	if (off < FirstOffsetNumber || bit >= MaxHeapTuplesPerPage)
		return false;			/* no heap page can hold that line pointer */
	return (e->vis[bit / 64] & (UINT64CONST(1) << (bit % 64))) != 0;
}

/*
 * The cached answer for a heap block, or NULL when there is none.
 *
 * WHY A CACHED ANSWER IS STILL THE RIGHT ANSWER (this is the whole point of
 * the cache, so it is argued here rather than in DESIGN.md alone)
 * ---------------------------------------------------------------------------
 * The cache is consulted in exactly one place, lion_recheck_heap_heap(), which
 * is where a TID that the visibility map could not answer for is resolved
 * against the snapshot.  The §9 pin rule is untouched: the visibility-map
 * question is still asked in lion_count_container() while the index page is
 * pinned, and only the TIDs it could not answer reach this code.  What is
 * claimed here is narrower: that
 *
 *		heap_hot_search_buffer(root TID, snapshot)
 *
 * is a function of the snapshot alone, so it may be evaluated once per (page,
 * snapshot) and reused for the rest of the query.  Four things could break
 * that, and none of them can happen:
 *
 *	1. The tuple we found could be removed.  It is visible to our snapshot,
 *	   which is registered, so it is not dead to all: neither HOT pruning nor
 *	   VACUUM may remove it or its root line pointer.
 *	2. HOT pruning could rewrite the chain under us.  It may: it can turn the
 *	   root line pointer into a redirect and drop intermediate versions.  But
 *	   it only removes versions that are dead to ALL snapshots - therefore
 *	   invisible to ours - and it keeps every surviving version reachable from
 *	   the root in chain order, which is precisely what
 *	   heap_hot_search_buffer() walks.  The version it finds from a given root
 *	   is unchanged.  (This is the same guarantee a bitmap heap scan relies on
 *	   between building its TID list and visiting the heap.)
 *	3. An answer of "not visible" could become "visible".  That needs a tuple
 *	   whose xmin our snapshot accepts to appear at that offset.  Every tuple
 *	   written after we looked belongs to a transaction that is either still
 *	   in progress at our snapshot, or began after it, or is our own with a
 *	   command id at or above the snapshot's curcid - invisible in all three
 *	   cases.  Line pointer numbers never move (page compaction moves tuple
 *	   data, not line pointers), so an existing visible tuple cannot arrive at
 *	   a different offset either.
 *	4. An answer of "visible" could become "not visible".  A delete by a
 *	   concurrent transaction leaves the tuple visible to our snapshot whether
 *	   it commits or not, and a delete by our own transaction is stamped with
 *	   a command id at or above curcid, which HeapTupleSatisfiesMVCC also
 *	   reports as still visible.
 *
 * Serializable isolation needs one addition, because a cache hit calls
 * neither HeapCheckForSerializableConflictOut() nor PredicateLockTID():
 * lion_vis_fill_page() takes PredicateLockPage() for the block it resolves.
 * That covers every later hit, and the two directions of rw-conflict
 * detection are then both closed: a write that happened BEFORE we resolved
 * the page is caught by the conflict-out check inside the sweep (which walks
 * every chain on the page, so it tests a superset of the tuples any single
 * recheck would have), and a write AFTER it is caught by the writer's own
 * CheckForSerializableConflictIn() against that page lock.
 */
static LionVisEntry *
lion_vis_cache_lookup(LionCountCtx *cx, BlockNumber blkno)
{
	if (cx->cache == NULL || cx->cache->ht == NULL)
		return NULL;
	return lion_visht_lookup(cx->cache->ht, blkno);
}

/*
 * The entry whose bitmap the caller should resolve for blkno, or NULL when
 * there is nothing to resolve: no cache, a single count with nothing to
 * reuse, the block's first visit (which is only recorded), or a cache that
 * has spent its budget.
 *
 * The sweep costs one visibility test per line pointer of the page instead of
 * one per TID this count wants, so it is only worth doing once reuse is
 * evident: the first count records nothing, the second records the blocks it
 * visits, and a block is resolved on its second visit.  A one-shot count -
 * one call, every heap block visited once - therefore does exactly what it
 * did before the cache existed, down to the last buffer visit.  This mirrors
 * the `nuses >= 2` rule that decides when a posting set is worth
 * materializing.
 *
 * Called BEFORE the page is read, so that the hash table's allocations never
 * happen under a buffer content lock.
 */
static LionVisEntry *
lion_vis_cache_prepare(LionCountCtx *cx, BlockNumber blkno, LionVisEntry *e)
{
	LionVisCache *cache = cx->cache;
	bool		found;

	if (cache == NULL || cache->ncounts < 2)
		return NULL;

	if (e != NULL)
		return e;				/* second visit: resolve the whole page */

	if (cache->ht == NULL)
		cache->ht = lion_visht_create(cache->cxt, 256, NULL);
	else if (cache->full ||
			 cache->ht->members >= (uint64) cache->maxentries)
	{
		/* The budget is spent: this block keeps being fetched per batch. */
		cache->full = true;
		cx->stats.cache_full++;
		return NULL;
	}

	/* First visit: remember only that it happened. */
	e = lion_visht_insert(cache->ht, blkno, &found);
	Assert(!found);
	e->filled = false;
	return NULL;
}

/*
 * Resolve every root line pointer of the page in buf, which the caller holds
 * share locked, into *e.
 */
static void
lion_vis_fill_page(LionCountCtx *cx, Buffer buf, BlockNumber blkno,
				  LionVisEntry *e)
{
	Page		page = BufferGetPage(buf);
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	OffsetNumber off;

	Assert(!e->filled);
	if (maxoff > (OffsetNumber) MaxHeapTuplesPerPage)
		maxoff = (OffsetNumber) MaxHeapTuplesPerPage;	/* cannot happen */

	memset(e->vis, 0, sizeof(e->vis));
	for (off = FirstOffsetNumber; off <= maxoff; off = OffsetNumberNext(off))
	{
		ItemPointerData tid;
		HeapTupleData heapTuple;

		ItemPointerSet(&tid, blkno, off);

		/*
		 * The same call the per-TID recheck makes, for every line pointer
		 * instead of the ones this count happens to want.  An offset that is
		 * not the root of a chain (an unused or dead line pointer, or a
		 * heap-only tuple) yields false, which is exactly what a recheck of
		 * that TID would have returned - and no index TID points at one.
		 */
		if (heap_hot_search_buffer(&tid, cx->heap, buf, cx->snapshot,
								   &heapTuple, NULL, true))
			e->vis[(off - 1) / 64] |= UINT64CONST(1) << ((off - 1) % 64);
	}
	e->filled = true;

	/*
	 * Later hits on this block do no per-tuple predicate locking, so lock the
	 * page once now, the way an index-only scan does for a block it skips
	 * (see the argument on lion_vis_cache_lookup()).
	 */
	if (cx->serializable)
		PredicateLockPage(cx->heap, blkno, cx->snapshot);
}


/* ---------------------------------------------------------------------
 * Counting one container
 * --------------------------------------------------------------------- */

typedef struct LionRecheckCollector
{
	LionCountCtx *cx;
	uint32		ckey;
	const bool *needrecheck;	/* LION_BLOCKS_PER_CONTAINER entries */
} LionRecheckCollector;

/*
 * How many TIDs one recheck batch may hold (DESIGN.md §9).
 *
 * The list used to grow by doubling until the whole merge was over: 20
 * million candidates - which a hot standby produces for any 20-million-row
 * posting set, because it never trusts the visibility map - meant a 192 MiB
 * array, and past 134 million candidates an allocation request larger than
 * palloc will serve.  work_mem is the executor's own answer to "how much may
 * one node keep", so it is the budget here too.
 */
static int
lion_recheck_budget(void)
{
	int64		budget = ((int64) work_mem * INT64CONST(1024)) /
		(int64) sizeof(ItemPointerData);

	if (budget < LION_RECHECK_MIN_BATCH)
		budget = LION_RECHECK_MIN_BATCH;
	if (budget > LION_RECHECK_MAX_BATCH)
		budget = LION_RECHECK_MAX_BATCH;
	return (int) budget;
}

static void
lion_recheck_add(LionCountCtx *cx, uint64 code)
{
	ItemPointerData tid;

	lion_code_to_tid(code, &tid);

	/*
	 * Bounded batches.  When the list is full, recheck what it holds right
	 * now, add the visible rows to the running total and start over - rather
	 * than keeping every candidate TID until the merge ends.
	 *
	 * Why flushing here, in the middle of the merge and with the source pages
	 * still pinned, is safe (this is the §9 interlock, so it has to be
	 * argued): the ordering rule is that the VISIBILITY MAP question about a
	 * container's heap blocks must be asked before the pin on the page that
	 * container came from is released, and nothing here touches that - the VM
	 * checks for this container have already happened (they are what put
	 * these TIDs on the list) and the pins are still held.  Rechecking a TID
	 * in the heap under our snapshot needs no index pin at all:
	 *
	 *	- if VACUUM has removed that TID from the index and from the heap page
	 *	  meanwhile, the tuple was dead to every snapshot including ours, so
	 *	  not counting it is right - and table_fetch_tid()/
	 *	  heap_hot_search_buffer() find nothing there;
	 *	- if the line pointer has since been reused by a brand-new tuple, that
	 *	  tuple's xmin is later than our snapshot, so it is invisible to it
	 *	  and is not counted either.
	 *
	 * The only cost of an early flush is that a heap block whose TIDs
	 * straddle two batches is pinned twice, so the flush waits for a block
	 * boundary; that also keeps blocks_rechecked exact.
	 */
	if (cx->ntids >= cx->batchmax &&
		(!cx->tids_sorted ||
		 ItemPointerGetBlockNumber(&cx->tids[cx->ntids - 1]) !=
		 ItemPointerGetBlockNumber(&tid)))
		lion_recheck_flush(cx);

	if (cx->ntids >= cx->maxtids)
	{
		int			newmax;

		if (cx->maxtids == 0)
			newmax = Min(LION_RECHECK_INIT_TIDS, cx->batchmax);
		else if (cx->maxtids < cx->batchmax)
			newmax = Min(cx->maxtids * 2, cx->batchmax);
		else
			newmax = cx->maxtids + LION_RECHECK_GROW_TIDS;	/* a long block */

		if (cx->tids == NULL)
			cx->tids = (ItemPointerData *)
				palloc(sizeof(ItemPointerData) * newmax);
		else
			cx->tids = (ItemPointerData *)
				repalloc(cx->tids, sizeof(ItemPointerData) * newmax);
		cx->maxtids = newmax;
	}

	cx->tids[cx->ntids] = tid;

	/*
	 * The list is built in ckey order, and inside a container in ascending lo
	 * order, so it comes out sorted by (block, offset) - which is what lets
	 * lion_recheck_heap() process it one heap block at a time.  Verify rather
	 * than assume: one comparison per TID buys the right to skip the sort.
	 */
	if (cx->ntids > 0 &&
		ItemPointerCompare(&cx->tids[cx->ntids - 1], &cx->tids[cx->ntids]) >= 0)
		cx->tids_sorted = false;

	cx->ntids++;
}

static bool
lion_recheck_cb(uint16 lo, void *arg)
{
	LionRecheckCollector *rc = (LionRecheckCollector *) arg;
	uint16		blkinc;
	OffsetNumber off;

	lion_lo_split(lo, &blkinc, &off);
	Assert(blkinc < LION_BLOCKS_PER_CONTAINER);
	if (rc->needrecheck[blkinc])
		lion_recheck_add(rc->cx, lion_make_code(rc->ckey, lo));

	return true;
}

/*
 * Count one container of the intersection, then let the cursors move on.
 *
 * This function is the heart of DESIGN.md section 9.  On entry every cursor
 * still pins the page its current container was copied from; *c was computed
 * from exactly those containers.  The two things that must happen in this
 * order are both here, adjacent, and nowhere else:
 *
 *	1. ask the visibility map about every heap block that has members, and
 *	   either count the members outright (plus a predicate lock, as an
 *	   index-only scan would take) or queue the block's TIDs for a heap
 *	   recheck under the caller's snapshot;
 *	2. advance the cursors, which is what releases the source page pins.
 *
 * Doing (2) before (1) would let a concurrent VACUUM finish ambulkdelete on
 * the page we just read, prune the heap and set all-visible, after which
 * step (1) would count dead tuples.
 */
static void
lion_count_container(LionCountCtx *cx, const LionContainer *c,
					LionExprCursor *cursors, int nsources)
{
	BlockNumber firstblk = lion_ckey_first_block(c->ckey);
	uint64		members;		/* blocks of this container that have members */
	uint64		allvis;			/* blocks marked all-visible in the VM */
	uint64		dirty;			/* blocks with members that need a heap recheck */
	int			i;

	/*
	 * Test hook: the containers have been copied out, the source pages are
	 * still pinned, and the visibility map has not been consulted yet.  A
	 * VACUUM that reaches ambulkdelete while a backend waits here must block
	 * on the cleanup lock; test/isolation/count_vacuum_race.spec proves it.
	 * Compiles to nothing without --enable-injection-points.
	 */
	INJECTION_POINT("lion-count-containers-pinned", NULL);

	/*
	 * One pass over the container and one read of the visibility map for all
	 * of its heap blocks, instead of a VM probe and a binary search per block.
	 * VM_ALL_VISIBLE only, never VM_ALL_FROZEN: freezing says nothing about a
	 * tuple being visible to *this* snapshot.  The map read is unlocked and
	 * may be slightly stale in the "bit was just cleared" direction, which is
	 * harmless for the same reason it is harmless for index-only scans (see
	 * visibilitymap_get_status and lion_vm_allvisible_mask).
	 */
	members = lion_container_block_mask(c);
	if (cx->in_recovery)
		allvis = 0;				/* see lion_count_sources(): no interlock on a standby */
	else
	{
		allvis = lion_vm_allvisible_mask(cx->heap, firstblk, &cx->vmbuf);
#ifdef LION_VM_MASK_CHECK
		lion_vm_mask_check(cx->heap, firstblk, members, allvis, &cx->vmbuf);
#endif
	}
	dirty = members & ~allvis;

	if (dirty == 0)
	{
		/* The common case on a vacuumed table: O(1) per container. */
		cx->count += lion_container_cardinality(c);
		cx->stats.blocks_skipped_via_vm += pg_popcount64(members);
	}
	else
	{
		bool		needrecheck[LION_BLOCKS_PER_CONTAINER];
		uint32		dirty_members = 0;
		uint64		m = dirty;
		LionRecheckCollector rc;

		memset(needrecheck, 0, sizeof(needrecheck));
		while (m != 0)
		{
			int			b = pg_rightmost_one_pos64(m);
			uint16		lo_start = (uint16) (b << LION_OFFSET_BITS);
			uint16		lo_end = (uint16) (lo_start + LION_MAX_OFFSET);

			m &= m - 1;
			needrecheck[b] = true;
			dirty_members += lion_container_range_cardinality(c, lo_start, lo_end);
		}
		Assert(dirty_members <= lion_container_cardinality(c));

		cx->count += lion_container_cardinality(c) - dirty_members;
		cx->stats.blocks_skipped_via_vm += pg_popcount64(members & allvis);

		/* Queue the members of the non-all-visible blocks for a heap recheck. */
		rc.cx = cx;
		rc.ckey = c->ckey;
		rc.needrecheck = needrecheck;
		lion_container_iterate(c, lion_recheck_cb, &rc);
	}

	/*
	 * We are not visiting the heap for the blocks counted above, so lock
	 * those pages as an index-only scan would (heapam_indexscan.c).  Only a
	 * serializable transaction can need them.
	 */
	if (cx->serializable)
	{
		uint64		m = members & allvis;

		while (m != 0)
		{
			int			b = pg_rightmost_one_pos64(m);

			m &= m - 1;
			PredicateLockPage(cx->heap, firstblk + (BlockNumber) b, cx->snapshot);
		}
	}

	/*
	 * DESIGN.md section 9: every heap block of *c has now been checked
	 * against the visibility map, so - and only now - the pins on the pages
	 * the source containers came from may be released.  lion_ecursor_next() is
	 * what releases them, and only the sources that contributed to *c (the
	 * ones the merge flagged) move on.
	 */
	for (i = 0; i < nsources; i++)
	{
		if (cursors[i].advance)
			lion_ecursor_next(&cursors[i]);
	}
}

static int
lion_tid_cmp(const void *a, const void *b)
{
	return ItemPointerCompare((ItemPointer) a, (ItemPointer) b);
}

/*
 * Recheck through the table AM, one TID at a time.  This is the portable
 * path; table_fetch_tid() pins, share-locks and unpins the block for every
 * TID, which is what lion_recheck_heap_heap() avoids for the heap AM.
 *
 * The TIDs come from an index, so each one is the root of a HOT chain, which
 * is exactly what table_fetch_tid() expects: it walks the chain and reports
 * whether any version of the row satisfies the snapshot.  Under an MVCC
 * snapshot at most one version can, so a visible chain counts as one row.
 *
 * (DESIGN.md section 9 step 4 describes this in terms of
 * table_index_fetch_tuple()/call_again; that API no longer exists in
 * PostgreSQL 20devel, where the index-scan callbacks moved into the table AM.
 * table_fetch_tid() is its direct replacement for TID-at-a-time lookups.)
 */
static int64
lion_recheck_heap_am(LionCountCtx *cx)
{
	int64		visible = 0;
	BlockNumber lastblk = InvalidBlockNumber;
	int			i;

	for (i = 0; i < cx->ntids; i++)
	{
		ItemPointerData tid = cx->tids[i];	/* mutable copy: callee updates it */
		BlockNumber blk = ItemPointerGetBlockNumber(&tid);

		if (blk != lastblk)
		{
			cx->stats.blocks_rechecked++;
			lastblk = blk;
		}

		if (table_fetch_tid(cx->heap, &tid, cx->snapshot, NULL))
			visible++;

		if ((i & 0x3ff) == 0)
			CHECK_FOR_INTERRUPTS();
	}

	return visible;
}

/*
 * The same thing for the heap AM, one heap BLOCK at a time.
 *
 * table_fetch_tid() is heapam_fetch_tid(), which is ReadBuffer + share lock +
 * heap_hot_search_buffer() + unlock + unpin.  Our TID list is sorted, and
 * after a plain DELETE every heap block of the posting set is dirty, so the
 * per-TID version pins, locks, unlocks and unpins the same buffer once for
 * every member of the set that lives on the block - a million buffer lookups
 * for a million-row table, which measured slower than the sequential scan the
 * pushdown is supposed to beat.  Reading the buffer once per block and
 * calling heap_hot_search_buffer() under one share lock is the same work
 * without the per-TID buffer manager traffic.
 *
 * heap_hot_search_buffer() is the very function the AM callback uses, so the
 * semantics are unchanged, including the ones that are easy to lose:
 *
 *	- it starts at the root of the HOT chain, which is what an index stores,
 *	  and follows redirects and HOT updates to the one version our snapshot
 *	  can see (first_call = true, one call per TID: with an MVCC snapshot at
 *	  most one chain member is visible, and heapam_fetch_tid() looks no
 *	  further either);
 *	- it calls HeapCheckForSerializableConflictOut() on every chain member it
 *	  tests and PredicateLockTID() on the one it returns, so a SERIALIZABLE
 *	  transaction takes exactly the tuple-level predicate locks it would have
 *	  taken through the table AM.  (The all-visible blocks we never look at
 *	  are predicate-locked page-wise in lion_count_container(), as an
 *	  index-only scan does.)
 *
 * all_dead is passed as NULL, as the table_fetch_tid() call it replaces did:
 * we have no index tuple to mark killed, and asking for it would cost a
 * GlobalVisTest per invisible chain for nothing.
 *
 * The per-query visibility cache sits here and nowhere else: a block whose
 * answer is already known is served from the bitmap with no buffer access at
 * all, and the blocks that are left are fetched once each, as they always
 * were.  lion_vis_cache_lookup() carries the argument for why a remembered
 * answer is still the right one.
 */
static int64
lion_recheck_heap_heap(LionCountCtx *cx)
{
	int64		visible = 0;
	int			i = 0;

	while (i < cx->ntids)
	{
		BlockNumber blk = ItemPointerGetBlockNumber(&cx->tids[i]);
		LionVisEntry *e = lion_vis_cache_lookup(cx, blk);
		Buffer		buf;

		/* Already resolved: no ReadBuffer, no content lock, no heap at all. */
		if (e != NULL && e->filled)
		{
			cx->stats.cache_hits++;
			do
			{
				if (lion_vis_entry_visible(e, ItemPointerGetOffsetNumber(&cx->tids[i])))
					visible++;
				i++;
			} while (i < cx->ntids &&
					 ItemPointerGetBlockNumber(&cx->tids[i]) == blk);

			CHECK_FOR_INTERRUPTS();
			continue;
		}

		/* Decide (and allocate) before the page is locked. */
		e = lion_vis_cache_prepare(cx, blk, e);

		buf = ReadBuffer(cx->heap, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		cx->stats.blocks_rechecked++;

		if (e != NULL)
		{
			/* Resolve the whole page once, then read this count's TIDs off. */
			lion_vis_fill_page(cx, buf, blk, e);
			do
			{
				if (lion_vis_entry_visible(e, ItemPointerGetOffsetNumber(&cx->tids[i])))
					visible++;
				i++;
			} while (i < cx->ntids &&
					 ItemPointerGetBlockNumber(&cx->tids[i]) == blk);
		}
		else
		{
			do
			{
				ItemPointerData tid = cx->tids[i];	/* callee updates it */
				HeapTupleData heapTuple;

				if (heap_hot_search_buffer(&tid, cx->heap, buf, cx->snapshot,
										   &heapTuple, NULL, true))
					visible++;
				i++;
			} while (i < cx->ntids &&
					 ItemPointerGetBlockNumber(&cx->tids[i]) == blk);
		}

		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buf);

		/* Only now: an interrupt cannot be serviced under a buffer lock. */
		CHECK_FOR_INTERRUPTS();
	}

	return visible;
}

/*
 * Visit the heap for the TIDs of blocks that were not all-visible.
 */
static int64
lion_recheck_heap(LionCountCtx *cx)
{
	int64		visible;

	if (cx->ntids == 0)
		return 0;

	/*
	 * Both paths below want the list in (block, offset) order; it is produced
	 * that way, and lion_recheck_add() checks that it was.
	 */
	if (!cx->tids_sorted)
		qsort(cx->tids, cx->ntids, sizeof(ItemPointerData), lion_tid_cmp);

	if (cx->heap->rd_tableam == GetHeapamTableAmRoutine())
		visible = lion_recheck_heap_heap(cx);
	else
		visible = lion_recheck_heap_am(cx);

	cx->stats.tids_rechecked += cx->ntids;
	return visible;
}

/*
 * Recheck the batch accumulated so far and empty the list.  Called from
 * lion_recheck_add() whenever the budget is reached (the safety argument is
 * there) and once more when the merge is over.
 */
static void
lion_recheck_flush(LionCountCtx *cx)
{
	if (cx->ntids == 0)
		return;

	cx->recheck_count += lion_recheck_heap(cx);
	cx->ntids = 0;
	cx->tids_sorted = true;
}


/* ---------------------------------------------------------------------
 * The merge
 * --------------------------------------------------------------------- */

int64
lion_count_sources(Relation heap, Snapshot snapshot, int nsources,
				  LionCountSource *sources, LionCountStats *stats)
{
	return lion_count_sources_cached(heap, snapshot, nsources, sources, stats,
									NULL);
}

int64
lion_count_sources_cached(Relation heap, Snapshot snapshot, int nsources,
						 LionCountSource *sources, LionCountStats *stats,
						 LionVisCache *cache)
{
	MemoryContext cxt;
	MemoryContext oldcxt;
	LionCountCtx cx;
	LionExprCursor *cursors;
	LionKeyNode **trees;
	LionContainer *work[2];
	int64		result;
	int			ncarry;
	bool	   *carry;
	int			npositive = 0;
	int			i;
	int			j;

	Assert(nsources >= 1);

	/*
	 * The shape of each source, and whether it can select anything at all: a
	 * positive source that cannot makes the whole intersection empty, and a
	 * negated one that cannot simply subtracts nothing.
	 */
	trees = (LionKeyNode **) palloc0(sizeof(LionKeyNode *) * nsources);
	for (i = 0; i < nsources; i++)
	{
		trees[i] = lion_source_tree(&sources[i]);

		if (sources[i].negated)
			continue;
		npositive++;
		if (!lion_source_satisfiable(trees[i], sources[i].sets))
			return 0;
	}
	if (npositive == 0)
		elog(ERROR, "lion index count needs at least one positive source");

	/*
	 * Decide which sets to serve from a private copy this time (DESIGN.md
	 * section 9; the argument is on lion_posting_set_materialize()).
	 *
	 * Two rules, and the safety of the whole thing rests on the second:
	 *
	 *	1. only a set that has been counted before, which in practice means
	 *	   the WHERE sets of the GROUP BY path in lion_customscan.c, where the
	 *	   same sets are intersected with every group in turn and walking
	 *	   their chains again per group is the dominant cost.  A one-shot
	 *	   count never pays for a copy it would use once.
	 *
	 *	2. never the last POSITIVE source that still carries the interlock.
	 *	   A set that is INLINE, or that is walked page by page, holds a pin
	 *	   while its containers are counted against the visibility map, and
	 *	   that pin is what keeps VACUUM from having finished ambulkdelete() -
	 *	   on this index, and therefore from having set all-visible on any
	 *	   heap page at all.  One source is enough, but there must be one, and
	 *	   it has to be a positive one that holds a pin at EVERY container key
	 *	   it yields, which is what lion_source_pinned() decides.
	 *
	 * A negated set may always be copied: a stale copy can only hold TIDs
	 * whose rows are dead (a live row's key cannot change without the row
	 * getting a new TID), and subtracting a dead TID cannot take a live row
	 * out of the count.
	 */
	carry = (bool *) palloc0(sizeof(bool) * nsources);
	ncarry = 0;
	for (i = 0; i < nsources; i++)
	{
		for (j = 0; j < sources[i].nsets; j++)
		{
			if (sources[i].sets[j].found)
				sources[i].sets[j].nuses++;
		}

		if (sources[i].negated)
			continue;
		carry[i] = lion_source_pinned(trees[i], sources[i].sets);
		if (carry[i])
			ncarry++;
	}
	Assert(ncarry > 0);

	for (i = 0; i < nsources; i++)
	{
		for (j = 0; j < sources[i].nsets; j++)
		{
			LionPostingSet *ps = &sources[i].sets[j];

			if (!ps->found)
				continue;
			if (ps->is_inline || ps->mat != NULL)
				continue;		/* nothing to gain: already a private copy */
			if (ps->nuses < 2)
				continue;		/* rule 1 */
			if (!sources[i].negated && carry[i] && ncarry <= 1)
				continue;		/* rule 2: this is the last interlock */
			if (ps->ncontainers > LION_MATERIALIZE_MAX_CONTAINERS &&
				ps->ntids > LION_MATERIALIZE_MAX_BYTES / sizeof(uint16))
				continue;		/* hopeless even as an ARRAY of members */

			if (lion_posting_set_materialize(ps) && !sources[i].negated &&
				carry[i] && !lion_source_pinned(trees[i], sources[i].sets))
			{
				carry[i] = false;
				ncarry--;
			}
		}
	}
	Assert(ncarry > 0);

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"lion index count",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	memset(&cx, 0, sizeof(cx));
	cx.heap = heap;
	cx.snapshot = snapshot;
	cx.vmbuf = InvalidBuffer;
	/* SerializationNeededForRead() begins with exactly this test; hoisting it
	 * lets non-serializable counts skip the per-block PredicateLockPage loop. */
	cx.serializable = IsolationIsSerializable();
	/*
	 * On a hot standby the §9 interlock does not exist: WAL replay of our
	 * generic records takes ordinary exclusive locks, not cleanup locks, so a
	 * reader's pin does not stop ambulkdelete's records from being replayed,
	 * and the heap records that follow can set all-visible while this backend
	 * still holds a copy of the old containers.  Until the AM has its own
	 * resource manager whose redo takes cleanup locks, a standby rechecks
	 * every candidate TID in the heap and never trusts the visibility map.
	 */
	cx.in_recovery = RecoveryInProgress();
	cx.tids_sorted = true;
	cx.batchmax = lion_recheck_budget();

	/*
	 * The visibility cache, if the caller keeps one for this node execution.
	 * It is emptied here if it holds answers for another relation or another
	 * snapshot, so a partitioned count may hand the same handle to every
	 * partition (DESIGN.md §9 and §16).
	 */
	lion_vis_cache_begin(cache, heap, snapshot);
	cx.cache = cache;

	cursors = (LionExprCursor *) palloc0(sizeof(LionExprCursor) * nsources);
	work[0] = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	work[1] = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);

	for (i = 0; i < nsources; i++)
		lion_ecursor_init(&cursors[i], trees[i], sources[i].sets,
						 sources[i].nsets, &cx);

	/*
	 * Merge the sources by container key.  Containers are stored in ascending
	 * ckey order both inline and along a chain, and an expression cursor
	 * preserves that, so a single forward pass over all of them is enough.
	 */
	for (;;)
	{
		uint32		maxckey = 0;
		bool		havemax = false;
		bool		alleq = true;
		const LionContainer *acc = NULL;
		int			w = 0;

		/* The positive sources drive the merge; all must still have data. */
		for (i = 0; i < nsources; i++)
		{
			if (sources[i].negated)
				continue;
			if (!cursors[i].valid)
				goto merge_done;
			if (!havemax || cursors[i].ckey > maxckey)
			{
				maxckey = cursors[i].ckey;
				havemax = true;
			}
		}

		for (i = 0; i < nsources; i++)
		{
			cursors[i].advance = false;
			if (!sources[i].negated && cursors[i].ckey != maxckey)
				alleq = false;
		}

		if (!alleq)
		{
			/*
			 * A ckey that is missing from some set contributes nothing, so
			 * the lagging containers carry no visibility-map obligation and
			 * their pages may be let go straight away.
			 */
			for (i = 0; i < nsources; i++)
			{
				if (!sources[i].negated && cursors[i].ckey < maxckey)
					lion_ecursor_next(&cursors[i]);
			}
			CHECK_FOR_INTERRUPTS();
			continue;
		}

		/* The intersection of the positive sources ... */
		for (i = 0; i < nsources; i++)
		{
			if (sources[i].negated)
				continue;
			cursors[i].advance = true;
			if (acc == NULL)
				acc = cursors[i].cur;
			else
			{
				lion_container_and(acc, cursors[i].cur, work[w]);
				acc = work[w];
				w ^= 1;
			}
		}

		/* ... minus the negated ones (DESIGN.md §14, `col IS NOT NULL`). */
		for (i = 0; i < nsources; i++)
		{
			if (!sources[i].negated)
				continue;
			while (cursors[i].valid && cursors[i].ckey < maxckey)
				lion_ecursor_next(&cursors[i]);	/* nothing to subtract there */
			if (!cursors[i].valid || cursors[i].ckey != maxckey)
				continue;

			cursors[i].advance = true;
			if (lion_container_cardinality(acc) > 0)
			{
				lion_container_andnot(acc, cursors[i].cur, work[w]);
				acc = work[w];
				w ^= 1;
			}
		}

		if (lion_container_cardinality(acc) > 0)
			lion_count_container(&cx, acc, cursors, nsources);
		else
		{
			/*
			 * Nothing of this container key survives, so no visibility-map
			 * question is asked about it and the source pages may go.
			 */
			for (i = 0; i < nsources; i++)
			{
				if (cursors[i].advance)
					lion_ecursor_next(&cursors[i]);
			}
		}
		CHECK_FOR_INTERRUPTS();
	}

merge_done:
	for (i = 0; i < nsources; i++)
		lion_ecursor_close(&cursors[i]);

	/*
	 * Everything that could be answered from the visibility map has been;
	 * what is left of the last batch needs the heap and the snapshot.  No
	 * index page is pinned any more, which is fine: the decisions that needed
	 * a pin were all made above.
	 */
	lion_recheck_flush(&cx);
	result = cx.count + cx.recheck_count;

	if (BufferIsValid(cx.vmbuf))
		ReleaseBuffer(cx.vmbuf);

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);

	if (stats != NULL)
	{
		stats->blocks_skipped_via_vm += cx.stats.blocks_skipped_via_vm;
		stats->tids_rechecked += cx.stats.tids_rechecked;
		stats->blocks_rechecked += cx.stats.blocks_rechecked;
		stats->containers_visited += cx.stats.containers_visited;
		stats->cache_hits += cx.stats.cache_hits;
		stats->cache_full += cx.stats.cache_full;
	}

	return result;
}

bool
lion_sets_satisfiable(int nsets, LionPostingSet *sets, LionKeyNode *tree)
{
	LionCountSource src;

	memset(&src, 0, sizeof(src));
	src.nsets = nsets;
	src.sets = sets;
	src.tree = tree;

	return lion_source_satisfiable(lion_source_tree(&src), sets);
}

/*
 * Walk the containers of one expression over located posting sets, without
 * any visibility-map interlock: what a bitmap scan of a multi-key opclass
 * needs (DESIGN.md §17).  Every TID goes to the executor, which visits the
 * heap for all of them, so no pin has anything to protect here - but the
 * cursors take and drop their pins exactly as they do for a count, which is
 * why the very same evaluator serves both.
 */
int64
lion_sets_iterate(int nsets, LionPostingSet *sets, LionKeyNode *tree,
				 lion_container_callback cb, void *arg)
{
	LionCountSource src;
	LionExprCursor cursor;
	LionCountCtx cx;
	LionKeyNode *node;
	int64		total = 0;

	memset(&src, 0, sizeof(src));
	src.nsets = nsets;
	src.sets = sets;
	src.tree = tree;

	node = lion_source_tree(&src);
	if (node == NULL)
		return 0;

	memset(&cx, 0, sizeof(cx));
	cx.vmbuf = InvalidBuffer;

	lion_ecursor_init(&cursor, node, sets, nsets, &cx);

	while (cursor.valid)
	{
		total += (int64) lion_container_cardinality(cursor.cur);
		if (!cb(cursor.cur, arg))
			break;
		lion_ecursor_next(&cursor);
		CHECK_FOR_INTERRUPTS();
	}

	lion_ecursor_close(&cursor);

	return total;
}

/*
 * The plain form: count the intersection of nsets posting sets.
 */
int64
lion_count_posting_sets(Relation heap, Snapshot snapshot, int nsets,
					   LionPostingSet *sets, LionCountStats *stats)
{
	LionCountSource *sources;
	int64		result;
	int			i;

	Assert(nsets >= 1);

	sources = (LionCountSource *) palloc0(sizeof(LionCountSource) * nsets);
	for (i = 0; i < nsets; i++)
	{
		sources[i].nsets = 1;
		sources[i].sets = &sets[i];
		sources[i].negated = false;
	}

	result = lion_count_sources(heap, snapshot, nsets, sources, stats);

	pfree(sources);
	return result;
}

int64
lion_count_keys(Relation heap, Snapshot snapshot, int nkeys, Relation *indexes,
			   Datum *keys, Oid *keytypes, LionCountStats *stats)
{
	LionPostingSet *sets;
	int64		result;
	int			i;

	Assert(nkeys >= 1);

	sets = (LionPostingSet *) palloc0(sizeof(LionPostingSet) * nkeys);

	for (i = 0; i < nkeys; i++)
		lion_posting_set_lookup(indexes[i], keys[i],
							   keytypes ? keytypes[i] : InvalidOid,
							   &sets[i]);

	result = lion_count_posting_sets(heap, snapshot, nkeys, sets, stats);

	for (i = 0; i < nkeys; i++)
		lion_posting_set_release(&sets[i]);

	pfree(sets);
	return result;
}


/* ---------------------------------------------------------------------
 * Iterating every entry of an index
 * --------------------------------------------------------------------- */

void
lion_entry_scan_begin(LionEntryScan *es, Relation index)
{
	es->index = index;
	es->state = lion_get_state(index);
	es->bucket = 0;
	es->blkno = LION_BUCKET_BLKNO(0);
	es->off = FirstOffsetNumber;
	es->done = false;
}

bool
lion_entry_scan_next(LionEntryScan *es, Datum *key, LionPostingSet *ps)
{
	LionState   *state = es->state;

	while (!es->done)
	{
		Buffer		buf;
		Page		page;
		OffsetNumber maxoff;
		BlockNumber next;
		bool		got = false;

		buf = ReadBuffer(es->index, es->blkno);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (!LionPageIsBucket(page))
		{
			UnlockReleaseBuffer(buf);
			elog(ERROR, "lion index: block %u is not a bucket page",
				 es->blkno);
		}
		maxoff = PageGetMaxOffsetNumber(page);

		while (es->off <= maxoff)
		{
			ItemId		iid = PageGetItemId(page, es->off);
			LionEntryTuple *entry;
			OffsetNumber thisoff = es->off;
			bool		keeppin;

			es->off = OffsetNumberNext(es->off);

			if (!ItemIdIsUsed(iid))
				continue;
			entry = (LionEntryTuple *) PageGetItem(page, iid);

			/*
			 * VACUUM keeps entries whose posting set has become empty
			 * (DESIGN.md section 5); they can never produce a group.
			 */
			if (entry->ntids == 0)
				continue;

			lion_fill_posting_set(es->index, state, buf, thisoff, ps, &keeppin);
			*key = ps->storedkey;
			if (keeppin)
			{
				/*
				 * DESIGN.md section 9: the INLINE payload we just copied out
				 * needs a pin of its own on this bucket page, independent of
				 * the scan's position.
				 */
				IncrBufferRefCount(buf);
				ps->pinbuf = buf;
			}

			got = true;
			break;
		}

		next = LionPageGetOpaque(page)->rightlink;
		UnlockReleaseBuffer(buf);

		if (got)
			return true;

		/* This bucket page is done: next page of the chain, or next bucket. */
		if (BlockNumberIsValid(next))
			es->blkno = next;
		else if (++es->bucket < state->meta.nbuckets)
			es->blkno = LION_BUCKET_BLKNO(es->bucket);
		else
		{
			es->done = true;
			break;
		}
		es->off = FirstOffsetNumber;

		CHECK_FOR_INTERRUPTS();
	}

	return false;
}

void
lion_entry_scan_end(LionEntryScan *es)
{
	es->done = true;
}


/* ---------------------------------------------------------------------
 * SQL interface
 * --------------------------------------------------------------------- */

/*
 * Common argument validation for the lion_index_count* functions.
 * Everything is opened here and closed by lion_count_sql_close().
 */
typedef struct LionCountCall
{
	int			nkeys;
	Relation	heap;
	Relation	index[2];
	Datum		key[2];
	Oid			keytype[2];
} LionCountCall;

/*
 * Open and vet the indexes of one SQL count: relkind, access method, key
 * type, privileges, row-level security and snapshot eligibility.  keytype may
 * be NULL, which means the caller has no search key at all (the grouped form
 * below, which walks every entry instead of looking one up).
 */
static void
lion_count_open_indexes(Snapshot snapshot, int nidx, const Oid *idxoid,
					   const Oid *keytype, LionCountCall *call)
{
	Oid			heapoid = InvalidOid;
	int			i;

	call->nkeys = nidx;
	call->heap = NULL;
	for (i = 0; i < 2; i++)
	{
		call->index[i] = NULL;
		call->keytype[i] = InvalidOid;
	}

	for (i = 0; i < nidx; i++)
	{
		Oid			hoid;

		if (keytype != NULL)
			call->keytype[i] = keytype[i];

		if (get_rel_relkind(idxoid[i]) != RELKIND_INDEX)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is not an index", get_rel_name(idxoid[i]))));

		hoid = IndexGetRelation(idxoid[i], false);
		if (i == 0)
			heapoid = hoid;
		else if (hoid != heapoid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("indexes \"%s\" and \"%s\" are not on the same table",
							get_rel_name(idxoid[0]),
							get_rel_name(idxoid[i]))));
	}

	call->heap = table_open(heapoid, AccessShareLock);

	for (i = 0; i < nidx; i++)
	{
		Relation	index = index_open(idxoid[i], AccessShareLock);

		call->index[i] = index;

		if (index->rd_rel->relam != lion_get_am_oid())
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("index \"%s\" is not a lion index",
							RelationGetRelationName(index))));
		if (IndexRelationGetNumberOfKeyAttributes(index) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("lion index \"%s\" must have exactly one key column",
							RelationGetRelationName(index))));

		/*
		 * The key must be the index's own type, or a type the opfamily can
		 * compare it with (integer cross-type equality, for instance).
		 * lion_probe_init() would raise the same errors later; raising them
		 * here keeps them out of the middle of the count.
		 */
		if (OidIsValid(call->keytype[i]) &&
			call->keytype[i] != index->rd_opcintype[0] &&
			!OidIsValid(get_opfamily_member(index->rd_opfamily[0],
											index->rd_opcintype[0],
											call->keytype[i], 1)))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("type %s cannot be compared with index \"%s\"",
							format_type_be(call->keytype[i]),
							RelationGetRelationName(index)),
					 errdetail("The index is on type %s.",
							   format_type_be(index->rd_opcintype[0]))));
	}

	/*
	 * Privileges: exactly what the equivalent query needs.  SELECT count(*)
	 * FROM t WHERE col = key references only col, so SELECT on the table or
	 * on every indexed column is required; with less than that the count
	 * would let a caller probe values it is not allowed to read.
	 */
	if (pg_class_aclcheck(heapoid, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
	{
		for (i = 0; i < nidx; i++)
		{
			AttrNumber	attnum = call->index[i]->rd_index->indkey.values[0];

			if (pg_attribute_aclcheck(heapoid, attnum, GetUserId(),
									  ACL_SELECT) != ACLCHECK_OK)
				aclcheck_error(ACLCHECK_NO_PRIV,
							   get_relkind_objtype(call->heap->rd_rel->relkind),
							   RelationGetRelationName(call->heap));
		}
	}

	/*
	 * Row-level security: the policies would have to be evaluated per row,
	 * and the whole point of this count is not to look at rows.  Refuse.
	 * The same query through the planner still works: the pushdown declines
	 * relations with security quals and the ordinary plan applies them.
	 */
	if (check_enable_rls(heapoid, InvalidOid, false) == RLS_ENABLED)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("cannot count through index \"%s\" because row-level security is enabled on table \"%s\"",
						RelationGetRelationName(call->index[0]),
						RelationGetRelationName(call->heap))));

	/*
	 * Snapshot eligibility.  The planner decides this for a query that
	 * mentions the table; a direct SQL count was handed an index nobody
	 * vetted, so it asks the same question itself - once the snapshot is
	 * known, which is why this is the last thing lion_count_sql() does before
	 * the lookup.  An index that indcheckxmin makes unusable does not contain
	 * the HOT-chain versions an old snapshot still sees, and rechecking
	 * cannot invent a TID that is not in the posting set (DESIGN.md §9).
	 */
	for (i = 0; i < nidx; i++)
	{
		const char *why;

		if (!lion_index_usable(call->index[i], snapshot, &why))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot count through index \"%s\" because %s",
							RelationGetRelationName(call->index[i]), why)));
	}
}

/*
 * The same for the lion_index_count(idx, key [, idx2, key2]) functions,
 * whose arguments alternate index and key.
 */
static void
lion_count_sql_open(FunctionCallInfo fcinfo, int nkeys, Snapshot snapshot,
				   LionCountCall *call)
{
	Oid			idxoid[2];
	Oid			keytype[2];
	Datum		key[2];
	int			i;

	Assert(nkeys >= 1 && nkeys <= 2);

	for (i = 0; i < nkeys; i++)
	{
		idxoid[i] = PG_GETARG_OID(2 * i);
		key[i] = PG_GETARG_DATUM(2 * i + 1);
		keytype[i] = get_fn_expr_argtype(fcinfo->flinfo, 2 * i + 1);
		if (!OidIsValid(keytype[i]))
			elog(ERROR, "could not determine the type of the search key");
	}

	lion_count_open_indexes(snapshot, nkeys, idxoid, keytype, call);

	for (i = 0; i < nkeys; i++)
		call->key[i] = key[i];
}

static void
lion_count_sql_close(LionCountCall *call)
{
	int			i;

	for (i = 0; i < call->nkeys; i++)
	{
		if (call->index[i] != NULL)
			index_close(call->index[i], AccessShareLock);
	}
	if (call->heap != NULL)
		table_close(call->heap, AccessShareLock);
}

static int64
lion_count_sql(FunctionCallInfo fcinfo, int nkeys, LionCountStats *stats)
{
	LionCountCall call;
	Snapshot	snapshot;
	int64		result;
	int			i;

	snapshot = GetActiveSnapshot();
	if (snapshot == NULL)
		elog(ERROR, "lion index count requires an active snapshot");

	lion_count_sql_open(fcinfo, nkeys, snapshot, &call);

	/*
	 * index_beginscan() takes a relation-level predicate lock on an index
	 * whose AM has no ampredlocks (indexam.c).  We read the index without a
	 * scan, so take the same lock ourselves - before looking, so that an
	 * absent key is covered too: otherwise two SERIALIZABLE transactions could
	 * each count an absent key, insert it, and both commit.
	 */
	for (i = 0; i < nkeys; i++)
		PredicateLockRelation(call.index[i], snapshot);

	result = lion_count_keys(call.heap, snapshot, nkeys, call.index,
							call.key, call.keytype, stats);

	lion_count_sql_close(&call);
	return result;
}

Datum
lion_index_count(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(lion_count_sql(fcinfo, 1, NULL));
}

Datum
lion_index_count2(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(lion_count_sql(fcinfo, 2, NULL));
}

Datum
lion_index_count_stats(PG_FUNCTION_ARGS)
{
	LionCountStats stats;
	TupleDesc	tupdesc;
	Datum		values[5];
	bool		nulls[5] = {false, false, false, false, false};
	int64		count;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	memset(&stats, 0, sizeof(stats));
	count = lion_count_sql(fcinfo, 1, &stats);

	values[0] = Int64GetDatum(count);
	values[1] = Int64GetDatum(stats.blocks_skipped_via_vm);
	values[2] = Int64GetDatum(stats.tids_rechecked);
	values[3] = Int64GetDatum(stats.blocks_rechecked);
	/* one count never revisits a heap block, so this is always 0 here */
	values[4] = Int64GetDatum(stats.cache_hits);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * lion_index_count_any(idx, keys) - count(*) WHERE col = ANY (keys), the
 * SQL form of the IN list of DESIGN.md §15: the union of the listed values'
 * posting sets, counted against the visibility map like any other count.
 *
 * The values are located with lion_posting_set_lookup_many(), so the bucket
 * pages are read in order and duplicates cost nothing, and the union is the
 * k-way merge of lion_ecursor_build().  Unlike the pushdown, this has no limit
 * on the number of values other than the pins it holds - one bucket page per
 * INLINE entry - so a caller that hands it a very long list should expect to
 * hold that many buffer pins for the duration.
 */
Datum
lion_index_count_any(PG_FUNCTION_ARGS)
{
	Oid			idxoid = PG_GETARG_OID(0);
	ArrayType  *arr = PG_GETARG_ARRAYTYPE_P(1);
	Oid			elemtype = ARR_ELEMTYPE(arr);
	LionCountCall call;
	LionCountSource src;
	LionPostingSet *sets;
	Snapshot	snapshot;
	Datum	   *elems;
	bool	   *nulls;
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	int			nelems;
	int			nsets;
	int			nfound;
	int64		count = 0;
	int			i;

	snapshot = GetActiveSnapshot();
	if (snapshot == NULL)
		elog(ERROR, "lion index count requires an active snapshot");

	lion_count_open_indexes(snapshot, 1, &idxoid, &elemtype, &call);
	PredicateLockRelation(call.index[0], snapshot);

	get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
	deconstruct_array(arr, elemtype, elmlen, elmbyval, elmalign,
					  &elems, &nulls, &nelems);

	sets = (LionPostingSet *) palloc0(sizeof(LionPostingSet) * Max(nelems, 1));
	nsets = lion_posting_set_lookup_many(call.index[0], elemtype, nelems,
										elems, nulls, sets, &nfound);

	/* An empty array, an all-NULL one, or no listed value with an entry. */
	if (nfound > 0)
	{
		memset(&src, 0, sizeof(src));
		src.nsets = nsets;
		src.sets = sets;
		count = lion_count_sources(call.heap, snapshot, 1, &src, NULL);
	}

	for (i = 0; i < nsets; i++)
		lion_posting_set_release(&sets[i]);

	lion_count_sql_close(&call);

	PG_RETURN_INT64(count);
}

/*
 * lion_index_count_group_stats(idx) - count every key of one index under
 * one snapshot, the way the GROUP BY path of DESIGN.md §10 does, sharing one
 * visibility cache across the groups.
 *
 * This is the SQL image of lion_next_group() in lion_customscan.c: same entry
 * scan, same one lion_count_sources_cached() call per group, same single cache
 * for the whole run.  It exists because the cache can only pay off across
 * counts, so nothing a single lion_index_count() does can exercise it -
 * and a regression test should not have to go through the planner to prove
 * that the dirty pages of a grouped count are visited once instead of once
 * per group.  use_cache = false runs the very same loop with no cache at all,
 * which is what every caller did before the cache existed.
 */
Datum
lion_index_count_group_stats(PG_FUNCTION_ARGS)
{
	Oid			idxoid = PG_GETARG_OID(0);
	bool		usecache = PG_GETARG_BOOL(1);
	LionCountCall call;
	Snapshot	snapshot;
	LionCountStats stats;
	LionVisCache *cache;
	LionEntryScan es;
	MemoryContext percxt;
	MemoryContext oldcxt;
	TupleDesc	tupdesc;
	Datum		values[7];
	bool		nulls[7] = {false, false, false, false, false, false, false};
	int64		groups = 0;
	int64		total = 0;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	snapshot = GetActiveSnapshot();
	if (snapshot == NULL)
		elog(ERROR, "lion index count requires an active snapshot");

	lion_count_open_indexes(snapshot, 1, &idxoid, NULL, &call);

	/*
	 * A multi-key opclass (DESIGN.md §17) stores one entry per extracted key,
	 * so its entries are not column values and a row appears under several of
	 * them: the sum over the entries is not a row count and neither is any
	 * single entry a group.  lion_customscan.c refuses to drive a GROUP BY
	 * from such an index for the same reason.
	 */
	if (OidIsValid(get_opfamily_proc(call.index[0]->rd_opfamily[0],
									 call.index[0]->rd_opcintype[0],
									 call.index[0]->rd_opcintype[0],
									 LION_EXTRACTVALUE_PROC)))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("index \"%s\" has a multi-key operator class, whose entries are not column values",
						RelationGetRelationName(call.index[0]))));

	PredicateLockRelation(call.index[0], snapshot);

	memset(&stats, 0, sizeof(stats));
	/* use_cache = false is the pre-cache behaviour, for an A/B in one query */
	cache = usecache ? lion_vis_cache_create(CurrentMemoryContext) : NULL;
	percxt = AllocSetContextCreate(CurrentMemoryContext,
								   "lion index group count",
								   ALLOCSET_SMALL_SIZES);

	lion_entry_scan_begin(&es, call.index[0]);

	for (;;)
	{
		LionCountSource src;
		LionPostingSet ps;
		Datum		key;
		int64		n;

		CHECK_FOR_INTERRUPTS();

		MemoryContextReset(percxt);
		oldcxt = MemoryContextSwitchTo(percxt);

		if (!lion_entry_scan_next(&es, &key, &ps))
		{
			MemoryContextSwitchTo(oldcxt);
			break;
		}

		memset(&src, 0, sizeof(src));
		src.nsets = 1;
		src.sets = &ps;

		n = lion_count_sources_cached(call.heap, snapshot, 1, &src, &stats,
									 cache);
		lion_posting_set_release(&ps);
		MemoryContextSwitchTo(oldcxt);

		/* A group exists only if one of its rows is visible (§10). */
		if (n > 0)
		{
			groups++;
			total += n;
		}
	}

	lion_entry_scan_end(&es);
	lion_vis_cache_destroy(cache);
	MemoryContextDelete(percxt);
	lion_count_sql_close(&call);

	values[0] = Int64GetDatum(groups);
	values[1] = Int64GetDatum(total);
	values[2] = Int64GetDatum(stats.blocks_skipped_via_vm);
	values[3] = Int64GetDatum(stats.tids_rechecked);
	values[4] = Int64GetDatum(stats.blocks_rechecked);
	values[5] = Int64GetDatum(stats.cache_hits);
	values[6] = Int64GetDatum(stats.cache_full);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}
