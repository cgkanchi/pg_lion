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
#include "catalog/objectaccess.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "catalog/pg_index.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "common/hashfn.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_coerce.h"
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "storage/predicate.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"
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
 * LION_OR_BITSET_MIN (lion_count.h, because the cost model needs the same
 * number): above this many containers at one container key, an OR node stops
 * folding them pairwise and accumulates them in a bitset image instead (see
 * lion_ecursor_build()).  Pairwise is cheaper while the containers are few and
 * small, because it touches only their members; the image costs a fixed pass
 * over the whole container key's range however few members arrive.  It is not a
 * small effect in either direction: forcing the pairwise fold at every key
 * (LION_OR_BITSET_MIN raised past any list length) took `c20k IN (1000 values)
 * AND c2 = 1` from 11.1 ms to 21.0 and `c200 IN (1000 values) AND c2 = 1` from
 * 7.7 ms to 263 - the fold is quadratic in the containers at a key - and the
 * threshold is also what decides whether the SUM or the MERGE is cheaper for a
 * dense list (lion_sum_is_cheaper()).
 *
 * A third way of building the union was tried and REJECTED, which is recorded
 * because the shape that suggests it is the common one.  A union over SPARSE
 * SEGMENTS (DESIGN.md §13) arrives at every container key as a hundred or two
 * throw-away ARRAYs of one member each, and the image looks wasteful for that:
 * 4 KiB of memset and a 512-word pass to produce two hundred members.
 * Gathering the members into one buffer, sorting and writing them out as an
 * ARRAY instead cannot win more than the image's FIXED cost, and that cost is
 * per CONTAINER KEY: the heap has only `heap_pages / LION_BLOCKS_PER_CONTAINER`
 * of them - 241 at one million rows, and the count scales with the heap, so the
 * ratio does not improve with size - which puts the whole image path at well
 * under a millisecond of that eleven-millisecond query.  What the query spends
 * its time on is the per-CHILD work: 45006 sub-cursor advances and their heap
 * sifts.  Anything that makes the ANDed case faster has to come off that.
 */

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
	MemoryContext cxt;			/* lives for the whole count: the recheck list
								 * goes here, because the merge itself may run
								 * under a context that is reset between the
								 * passes of a disjoint sum (DESIGN.md §15) */
	Relation	heap;
	Snapshot	snapshot;
	Buffer		vmbuf;			/* pinned VM page, or InvalidBuffer */
	bool		serializable;	/* IsolationIsSerializable() at start: take page predicate locks */
	bool		in_recovery;	/* hot standby: the pin interlock does not hold, recheck everything */
	bool		novm;			/* no source carries the interlock (NOPIN
								 * sets, DESIGN.md §15): recheck everything */
	bool		rel_read_only;	/* the statement does not modify heap: on-access
								 * pruning may set the VM (DESIGN.md §11) */
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

	/*
	 * An EXISTENCE test rather than a count (DESIGN.md §26): stop at the first
	 * row known to be visible.  lion_exists_settled() is the only reader.
	 */
	bool		exists;
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

	/*
	 * SEEKING (DESIGN.md §22).  `descend` says the next leaf must come from a
	 * descent of the posting tree for `seekckey` rather than from a rightlink;
	 * it is how a cursor jumps forward instead of streaming, and it is also
	 * how the FIRST leaf is found, because the entry's head block is the tree's
	 * ROOT and is only a leaf while the set fits one page.
	 *
	 * `mintarget` is the container key a seek asked for.  It only ever moves
	 * forward, and it is what makes a sparse segment that STARTS below the
	 * target skip to the right pair instead of replaying the whole segment.
	 */
	bool		descend;
	uint32		seekckey;
	uint32		mintarget;

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
static void lion_cursor_seek(LionSetCursor *cur, uint32 target);
static void lion_recheck_flush(LionCountCtx *cx);
static bool lion_exists_settled(LionCountCtx *cx);

/*
 * How far a cursor walks right before it gives up and descends instead
 * (DESIGN.md §22).  Stepping right is one buffer read per page; a descent is
 * `height` reads and a binary search per level, so the two are about even at
 * two pages and the descent wins from there.  The number only decides how
 * much work a seek does, never what it finds.
 */
#define LION_POSTING_SEEK_STEPS		2


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
 *
 * InvalidOid, not an error, where the access method does not exist: with the
 * library in shared_preload_libraries the planner hooks run in every
 * database, including those without CREATE EXTENSION pg_lion (template1 and
 * postgres, where pg_upgrade's own count(*) queries run), and there no index
 * or operator can match - which is what every caller does with InvalidOid.
 */
Oid
lion_get_am_oid(void)
{
	return get_am_oid("lion", true);
}

/*
 * LionProbe, the one cross-type resolution of DESIGN.md §21, is declared in
 * lion_count.h: the bitmap scan (lion_scan.c) resolves its scan keys through
 * lion_probe_init() and lion_probe_find() as well, so the two paths cannot
 * come to different conclusions about how to descend for a value.
 */
void
lion_probe_init(Relation index, LionState *state, Oid keytype, LionProbe *probe)
{
	/* The opclass of the KEY COLUMN this probe is for (DESIGN.md §24). */
	int			ci = state->attno - 1;
	Oid			opfamily = index->rd_opfamily[ci];
	Oid			opcintype = index->rd_opcintype[ci];
	Oid			eqopr;
	Oid			hashproc;
	Oid			cmpproc;
	Oid			sortproc;

	memset(probe, 0, sizeof(LionProbe));

	if (!OidIsValid(keytype) || keytype == opcintype)
	{
		probe->typlen = state->typlen;
		probe->typbyval = state->typbyval;
		if (state->ordered)
		{
			probe->cmpproc = state->cmpproc;
			probe->hascmp = true;
			probe->sortproc = state->cmpproc;
			probe->hassort = true;
		}
		/* sorted by the directory's comparison, or by the hash it leads with */
		probe->walk = true;
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

	/*
	 * An UNORDERED directory is in (kind, hash, bytes) order whatever the
	 * family offers: no comparison may be used on it, the family's cross-type
	 * proc 4 included, because descending a hash-ordered tree in value order
	 * finds nothing (§21 rule 3 can leave a column unordered although its
	 * family names comparisons).  The value's own hash is the family's
	 * cross-type hash, which agrees with the stored keys' by the family's
	 * contract, so a descent - and a list sorted by hash - is exact.
	 */
	if (!state->ordered)
	{
		probe->walk = true;
		return;
	}

	/*
	 * The family's cross-type ordering (proc 4 for the pair): descend as
	 * usual.  A LIST of such values must be sorted into the directory order
	 * before it can be walked, and the cross-type function cannot compare two
	 * values of the search type with each other.  The family's own proc 4 for
	 * (keytype, keytype) can: it is the family's statement of how it orders
	 * that type, the same promise that makes its cross-type proc 4 usable at
	 * all.  The search type's DEFAULT btree order is not used: it is the
	 * directory's order only when the opclass happens to sort that way (a
	 * reverse comparison does not), and a list sorted against the direction
	 * of the leaves loses every value but the first to the walk, which only
	 * steps right.  Without a comparison of its own the list is not walked:
	 * every value descends by itself.
	 */
	cmpproc = get_opfamily_proc(opfamily, opcintype, keytype, LION_CMP_PROC);
	if (OidIsValid(cmpproc))
	{
		fmgr_info(cmpproc, &probe->cmpproc);
		probe->hascmp = true;

		sortproc = get_opfamily_proc(opfamily, keytype, keytype,
									 LION_CMP_PROC);
		if (OidIsValid(sortproc))
		{
			fmgr_info(sortproc, &probe->sortproc);
			probe->hassort = true;
			probe->walk = true;
		}
		return;
	}

	/*
	 * The tree is ordered by a comparison this value cannot take part in.  A
	 * BINARY coercion to the key type - the same bytes, varchar to text -
	 * makes it one of the index's own values, and then hash, equality and
	 * ordering are all the index's own.  That is only correct if the family's
	 * cross-type EQUALITY is that same predicate: a family may declare
	 * `text = bpchar` through its own function with different semantics
	 * ('x' vs 'x ' differ as text, agree as bpchar), and then relabelling the
	 * probe would silently swap the family's equality for the key type's.  So
	 * the shortcut is taken only when the family's cross-type strategy-1
	 * operator is implemented by the SAME function as the key type's own
	 * strategy-1 operator - the two predicates are then one function applied
	 * to the same bytes.  A cast FUNCTION is never taken, implicit or not:
	 * implicit does not mean lossless (text -> name truncates to 63 bytes),
	 * and PostgreSQL has no way to say that a cast is a bijection.  Anything
	 * else keeps the family's cross-type equality and walks the leaves.
	 */
	{
		Oid			castfunc = InvalidOid;
		Oid			sameeq = get_opfamily_member(opfamily, opcintype, opcintype, 1);

		if (find_coercion_pathway(opcintype, keytype, COERCION_IMPLICIT,
								  &castfunc) == COERCION_PATH_RELABELTYPE &&
			OidIsValid(sameeq) &&
			get_opcode(sameeq) == get_opcode(eqopr))
		{
			/* From here on the probe values ARE the index's own type. */
			probe->coerce = true;
			probe->crosstype = false;
			probe->typlen = state->typlen;
			probe->typbyval = state->typbyval;
			probe->cmpproc = state->cmpproc;
			probe->hascmp = true;
			probe->sortproc = state->cmpproc;
			probe->hassort = true;
			probe->walk = true;
			return;
		}
	}

	/*
	 * Neither: the leaves are walked with the family's cross-type EQUALITY
	 * (lion_dir_find()), per value.  Correct and linear.
	 */
	probe->needscan = true;
}

/*
 * The Datum to probe with: the caller's value, which a binary coercion to the
 * key type (the only one lion_probe_init() takes) leaves as it is.
 */
static inline Datum
lion_probe_value(LionProbe *probe, Datum value)
{
	return value;				/* a binary coercion needs no work at all */
}

/* A search key for one probe value. */
static void
lion_probe_search_key(LionState *state, LionProbe *probe, Datum key,
					 uint32 hash, LionSearchKey *sk)
{
	lion_search_key_init(state, sk, LION_KIND_VALUE, key, hash);
	if (probe->crosstype)
	{
		sk->eqproc = &probe->eqproc;
		sk->cmpproc = probe->hascmp ? &probe->cmpproc : NULL;
	}
}

static inline uint32
lion_probe_hash(LionState *state, LionProbe *probe, Datum key)
{
	if (!probe->crosstype)
		return lion_hash_key(state, key);
	return DatumGetUInt32(FunctionCall1Coll(&probe->hashinfo, state->collation,
											key));
}

/*
 * Locate the entry of one value through a resolved probe: lion_dir_find()
 * with the search key lion_probe_init() decided on.  On true *buf is the leaf
 * locked in lockmode and *off the entry; on false *buf may be a locked leaf
 * or InvalidBuffer, and the caller releases it when it is valid.
 */
bool
lion_probe_find(Relation index, LionState *state, LionProbe *probe,
				Datum value, int lockmode, Buffer *buf, OffsetNumber *off)
{
	LionSearchKey sk;
	uint32		hash;

	value = lion_probe_value(probe, value);
	hash = lion_probe_hash(state, probe, value);
	lion_probe_search_key(state, probe, value, hash, &sk);

	return lion_dir_find(index, NULL, state->ix, &sk, lockmode, false,
						 buf, off, NULL);
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
	ps->attno = entry->attno;
	ps->pinbuf = InvalidBuffer;
	ps->found = true;
	ps->ntids = entry->ntids;
	ps->ncontainers = entry->ncontainers;
	ps->entryblk = BufferGetBlockNumber(buf);
	ps->entryoff = offnum;
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
 * One key of an IN list, in the order its entry is looked up in: the
 * DIRECTORY order (DESIGN.md §21), so that the whole list is located in one
 * left-to-right walk of the leaves and duplicates - which sort together - are
 * dropped by looking at the neighbours.
 */
typedef struct LionProbeKey
{
	uint32		hash;
	int32		idx;			/* position in the caller's value array */
} LionProbeKey;

typedef struct LionProbeSort
{
	const Datum *values;
	LionProbe  *probe;
	Oid			collation;
} LionProbeSort;

static int
lion_probe_key_cmp(const void *a, const void *b, void *arg)
{
	const LionProbeKey *x = (const LionProbeKey *) a;
	const LionProbeKey *y = (const LionProbeKey *) b;
	LionProbeSort *ctx = (LionProbeSort *) arg;

	if (ctx->probe->hassort)
	{
		int32		c = DatumGetInt32(FunctionCall2Coll(&ctx->probe->sortproc,
														ctx->collation,
														ctx->values[x->idx],
														ctx->values[y->idx]));

		if (c != 0)
			return c < 0 ? -1 : 1;
	}
	if (x->hash != y->hash)
		return x->hash < y->hash ? -1 : 1;
	return x->idx < y->idx ? -1 : (x->idx > y->idx ? 1 : 0);
}

/*
 * Fill *ps from the entry the caller has located at (buf, offnum), which is a
 * directory leaf held SHARE, and release the buffer - keeping its pin when the
 * entry is INLINE, because that pin is the DESIGN.md §9 interlock.
 *
 * Always: one lookup is one pin, and what a caller's loop of them holds is
 * bounded by the query, not by the data (DESIGN.md §15).
 */
static void
lion_posting_set_take(Relation index, LionState *state, Buffer buf,
					 OffsetNumber offnum, LionPostingSet *ps)
{
	bool		keeppin;

	lion_fill_posting_set(index, state, buf, offnum, ps, &keeppin);

	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	if (keeppin)
		ps->pinbuf = buf;
	else
		ReleaseBuffer(buf);
}

/*
 * Locate the entry of one key whose hash has already been computed.  This is
 * lion_posting_set_lookup() from the descent onwards, split out so that a
 * whole IN list can be sorted before any page is read
 * (lion_posting_set_lookup_many()).
 */
static bool
lion_posting_set_locate(Relation index, LionState *state, LionProbe *probe,
					   Datum key, uint32 hash, LionPostingSet *ps)
{
	LionSearchKey sk;
	Buffer		buf;
	OffsetNumber off;

	memset(ps, 0, sizeof(LionPostingSet));
	ps->index = index;
	ps->attno = state->attno;
	ps->pinbuf = InvalidBuffer;
	ps->head = InvalidBlockNumber;
	ps->entryblk = InvalidBlockNumber;
	ps->entryoff = InvalidOffsetNumber;

	lion_probe_search_key(state, probe, key, hash, &sk);

	if (!lion_dir_find(index, NULL, state->ix, &sk, BUFFER_LOCK_SHARE, false,
					   &buf, &off, NULL))
	{
		if (BufferIsValid(buf))
			UnlockReleaseBuffer(buf);
		return false;
	}

	lion_posting_set_take(index, state, buf, off, ps);
	return true;
}

bool
lion_posting_set_lookup_col(Relation index, AttrNumber attno, Datum key,
						   Oid keytype, LionPostingSet *ps)
{
	LionState   *state = lion_index_column_state(index, attno);
	LionProbe	probe;
	uint32		hash;

	lion_probe_init(index, state, keytype, &probe);
	key = lion_probe_value(&probe, key);
	hash = lion_probe_hash(state, &probe, key);

	return lion_posting_set_locate(index, state, &probe, key, hash, ps);
}

/*
 * How many leaves a walk may step right over before it is cheaper to descend
 * again (DESIGN.md §21).  A dense list steps; a list of a handful of values
 * spread over the whole index descends, instead of reading every leaf in
 * between.
 */
#define LION_LOOKUP_WALK_MAX	8

/*
 * THE LIST PIN BUDGET (DESIGN.md §15): how many distinct leaves the IN lists
 * this backend has located and not yet released may keep pinned, all of them
 * together.  It is the smaller of
 *
 *	- LION_LOOKUP_MAX_PINS, 1000: the longest list the planner accepts as a
 *	  literal, so that a literal list pins exactly what it always did - a set
 *	  that is NOPIN costs a second descent in the disjoint sum and the visibility
 *	  map in an OR, and an ordinary query must not pay either;
 *	- an EIGHTH of shared_buffers, so that one backend never pins more than a
 *	  modest fraction of the pool however many lists its query has.  The
 *	  failure this budget exists for was 9000 leaves against a 2048-buffer pool
 *	  (16MB): the query ran itself out of buffers, and one short of that it
 *	  would have starved everyone else.  With an eighth, 256 there, the query
 *	  keeps seven eighths for its own heap, visibility-map and chain pages and
 *	  for every other backend.  It only binds below 64MB of shared_buffers.
 *
 * Neither depends on the backend's "fair share" (GetAdditionalPinLimit() of
 * 18): that is NBuffers / MaxBackends, 86 buffers on a stock 128MB server, and
 * a budget of it sent ordinary thousand-value lists to the heap.
 *
 * The count is backend-wide because the budget is: two unbounded lists in one
 * query share it instead of taking one budget each.  Every set that took a
 * NEW leaf for it is marked `budgeted` and gives it back when released or
 * unpinned; a set abandoned by an error is not released, so the count is
 * zeroed at the end of every top-level transaction, when no set can be left.
 * Between an error and that point it can only be too high, which makes sets
 * NOPIN early - slower, never wrong.
 */
#define LION_LOOKUP_MAX_PINS	1000

static uint32 lion_list_pins = 0;
static bool lion_list_pins_cb = false;

static void
lion_list_pins_xact(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
		case XACT_EVENT_PREPARE:
			lion_list_pins = 0;
			break;
		default:
			break;
	}
}

static uint32
lion_list_pin_budget(void)
{
	uint32		limit = Min(LION_LOOKUP_MAX_PINS, Max(NBuffers / 8, 1));

	if (!lion_list_pins_cb)
	{
		RegisterXactCallback(lion_list_pins_xact, NULL);
		lion_list_pins_cb = true;
	}
	return (lion_list_pins < limit) ? limit - lion_list_pins : 0;
}

/* A set that took a leaf of the budget gives it back. */
static inline void
lion_list_pin_return(LionPostingSet *ps)
{
	if (ps->budgeted)
	{
		if (lion_list_pins > 0)
			lion_list_pins--;
		ps->budgeted = false;
	}
}

/*
 * Locate the posting sets of many keys of one index at once: the IN list of
 * DESIGN.md §15, whose union the merge in this file then evaluates.
 *
 * Two things are done here that a loop over lion_posting_set_lookup() cannot:
 *
 *	- the values are sorted into the DIRECTORY order first and the leaves are
 *	  then walked left to right, stepping right while the next value is only a
 *	  few pages ahead and descending again when it is further, so a thousand
 *	  values cost one pass over the leaves they live on instead of a thousand
 *	  random page reads (DESIGN.md §21);
 *	- duplicates are dropped in one pass over that order instead of by
 *	  comparing every value with every earlier one, which at the 1000 values
 *	  the planner allows is half a million datumIsEqual() calls.
 *
 * Duplicates are dropped TWICE OVER, and the second pass is the one that
 * matters (DESIGN.md §15).  The bytewise comparison comes first because it is
 * free and saves the lookup, but it is not exhaustive: an opclass whose
 * equality is not byte equality - citext - has distinct values that reach ONE
 * entry.  So every located entry's STORED KEY is also compared, with the
 * index's own equality, against the ones the same run has already found, and
 * a repeat is released again.  A union would not have cared (a set ORed with
 * itself is that set); the disjoint-SUM short-circuit does, because it would
 * add the entry's rows twice.
 *
 * *sets must have room for nvalues sets; the located ones come out packed at
 * the front, in key order, and the return value is how many there are.  Every
 * one of them - found or not - must be handed to lion_posting_set_release().
 * *nfound, if given, is how many of them have an entry in the index at all:
 * nfound == 0 means the union selects nothing.
 *
 * The pins are BUDGETED (DESIGN.md §15, 2026-09-23 review).  Each INLINE set
 * keeps a pin on the leaf its payload was copied from, the §9 interlock, and
 * nothing bounded how many leaves that came to: an array parameter over an
 * index with more leaves than shared_buffers ran out of buffers.  So at most
 * what is left of the backend's list pin budget (above) - and the INLINE
 * sets found past that come out NOPIN: their payload is copied and their leaf
 * let go.  The count copes with those without weakening §9 - see
 * lion_count_sources_run(), which either locates such a set again under a pin
 * of its own when it gets to it, or counts it in an intersection another
 * source carries the interlock for, or trusts no visibility map at all.
 */
int
lion_posting_set_lookup_many_col(Relation index, AttrNumber attno, Oid keytype,
								int nvalues, const Datum *values,
								const bool *isnull, LionPostingSet *sets,
								int *nfound)
{
	LionState   *state = lion_index_column_state(index, attno);
	LionProbe	probe;
	LionProbeSort sortctx;
	LionProbeKey *probes;
	const Datum *vals;
	Buffer		buf = InvalidBuffer;
	Buffer		lastpinned = InvalidBuffer;
	bool		lastmoved = false;
	uint32		budget;
	uint32		npinned = 0;
	int			nprobe = 0;
	int			nsets = 0;
	int			found = 0;
	int			runstart = 0;	/* first set located under this sort run */
	int			i;
	int			j;

	Assert(nvalues >= 0);
	if (nfound != NULL)
		*nfound = 0;
	if (nvalues == 0)
		return 0;

	/*
	 * One resolution for both lookups (DESIGN.md §21).  Whatever
	 * lion_probe_init() decides - the family's cross-type ordering, a cast to
	 * the key type, or no ordering at all - applies here exactly as it does to
	 * lion_posting_set_lookup(), because the two walk the same tree and a
	 * batched lookup that descended where the single one scans would read the
	 * directory in an order it is not in.
	 */
	lion_probe_init(index, state, keytype, &probe);

	budget = lion_list_pin_budget();

	/* A binary coercion - the only one taken - changes no value. */
	vals = values;

	probes = (LionProbeKey *) palloc(sizeof(LionProbeKey) * nvalues);
	for (i = 0; i < nvalues; i++)
	{
		if (isnull != NULL && isnull[i])
			continue;			/* `col = NULL` is never true */
		probes[nprobe].hash = lion_probe_hash(state, &probe, vals[i]);
		probes[nprobe].idx = i;
		nprobe++;
	}

	sortctx.values = vals;
	sortctx.probe = &probe;
	sortctx.collation = state->collation;
	if (nprobe > 1)
		qsort_arg(probes, nprobe, sizeof(LionProbeKey), lion_probe_key_cmp,
				  &sortctx);

	for (i = 0; i < nprobe; i++)
	{
		LionSearchKey sk;
		OffsetNumber off;
		bool		dup = false;
		bool		located;
		int			steps;

		/*
		 * A new sort run starts a new set of possible duplicates: with an
		 * ordering that is the run of equal values, without one the run of
		 * equal hashes.
		 */
		if (i > 0 &&
			(probe.hassort ?
			 (lion_probe_key_cmp(&probes[i - 1], &probes[i], &sortctx) != 0 &&
			  probes[i].hash != probes[i - 1].hash) :
			 probes[i].hash != probes[i - 1].hash))
			runstart = nsets;

		/* The cheap half: bytewise-equal neighbours need no lookup at all. */
		if (i > 0 &&
			datumIsEqual(vals[probes[i].idx], vals[probes[i - 1].idx],
						 probe.typbyval, probe.typlen))
			continue;

		lion_probe_search_key(state, &probe, vals[probes[i].idx],
							 probes[i].hash, &sk);

		memset(&sets[nsets], 0, sizeof(LionPostingSet));
		sets[nsets].index = index;
		sets[nsets].attno = state->attno;
		sets[nsets].pinbuf = InvalidBuffer;
		sets[nsets].head = InvalidBlockNumber;
		sets[nsets].entryblk = InvalidBlockNumber;
		sets[nsets].entryoff = InvalidOffsetNumber;

		if (!probe.walk)
		{
			/*
			 * The values are not sorted in the directory's order (no ordering
			 * for them at all, or a cross-type one that cannot sort a list):
			 * there is no walk to keep, and every value is located exactly as
			 * the single lookup locates it - a descent, or the leaf walk.
			 */
			if (BufferIsValid(buf))
			{
				UnlockReleaseBuffer(buf);
				buf = InvalidBuffer;
			}
			located = lion_dir_find(index, NULL, state->ix, &sk,
									BUFFER_LOCK_SHARE, false, &buf, &off,
									NULL);
		}
		else
		{
			/*
			 * Stay on the leaf the last value was found on when the next one is
			 * at most a few pages to the right; otherwise descend again.
			 *
			 * Not when the last lookup followed its prefix run across a page
			 * boundary, though: the walk then stands to the RIGHT of where
			 * that run begins, and the next value may belong to the same run
			 * (a hash collision) and be stored on one of the pages it has
			 * passed.  The leaf a lookup lands on otherwise is the one its
			 * run begins on, so a value sorted after it can only be there or
			 * further right.
			 */
			if (lastmoved && BufferIsValid(buf))
			{
				UnlockReleaseBuffer(buf);
				buf = InvalidBuffer;
			}

			for (steps = 0; BufferIsValid(buf); steps++)
			{
				Page		page = BufferGetPage(buf);

				if (LionPageIsRightmost(page) ||
					lion_cmp_entry(lion_dir_highkey(page), &sk) > 0)
					break;
				if (steps >= LION_LOOKUP_WALK_MAX)
				{
					UnlockReleaseBuffer(buf);
					buf = InvalidBuffer;
					break;
				}
				buf = lion_dir_step_right(index, buf, BUFFER_LOCK_SHARE);
			}

			if (!BufferIsValid(buf))
				buf = lion_dir_search(index, NULL, state->ix, &sk,
									  BUFFER_LOCK_SHARE, false, &off);
			else
				off = lion_dir_binsrch(BufferGetPage(buf), &sk);

			located = lion_dir_scan_run(index, &sk, BUFFER_LOCK_SHARE,
										&buf, &off, &lastmoved);
		}

		if (located)
		{
			bool		keeppin;

			lion_fill_posting_set(index, state, buf, off, &sets[nsets],
								  &keeppin);
			if (keeppin)
			{
				/*
				 * DESIGN.md §9: the INLINE payload just copied out needs a pin
				 * of its own on this leaf, independent of the walk's position
				 * - if the leaf is one of the first `budget`.  Another pin on
				 * the leaf the last set pinned costs no buffer; a new leaf is
				 * counted.  Without a walk the leaves come in hash order and
				 * may repeat, which counts some twice: the budget can only be
				 * reached early, never overrun.
				 */
				if (buf != lastpinned && npinned >= budget)
					sets[nsets].nopin = true;
				else
				{
					if (buf != lastpinned)
					{
						npinned++;
						lion_list_pins++;
						sets[nsets].budgeted = true;
					}
					lastpinned = buf;
					IncrBufferRefCount(buf);
					sets[nsets].pinbuf = buf;
				}
			}

			/*
			 * Two values that are not bytewise equal may still be the same
			 * entry (citext).  The stored keys are of the index's own type, so
			 * its own equality settles it exactly.
			 */
			for (j = runstart; j < nsets; j++)
			{
				if (sets[j].found && sets[j].hasstoredkey &&
					sets[nsets].hasstoredkey &&
					sets[j].keyisnull == sets[nsets].keyisnull &&
					(sets[nsets].keyisnull ||
					 lion_keys_equal(state, sets[j].storedkey,
									 sets[nsets].storedkey)))
				{
					/*
					 * If it took a new leaf, forget the leaf too: with its
					 * pin gone the buffer may be another page by the time the
					 * walk lands on it again.  Sets before it may still pin
					 * the leaf, and the next set there counts it once more -
					 * early, never over.
					 */
					if (sets[nsets].budgeted)
						lastpinned = InvalidBuffer;
					lion_posting_set_release(&sets[nsets]);
					dup = true;
					break;
				}
			}
			if (dup)
				continue;
			found++;
		}

		nsets++;

		if ((i & 0x3f) == 0)
			CHECK_FOR_INTERRUPTS();
	}

	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);

	pfree(probes);
	if (nfound != NULL)
		*nfound = found;
	return nsets;
}

/*
 * The same for the rows whose key is NULL (DESIGN.md §14).  The entry sorts
 * before every real key (LION_KIND_NULL), so the descent finds it on the
 * leftmost leaf; everything after that - the pin discipline of DESIGN.md §9
 * included - is identical to a real key's.
 */
bool
lion_posting_set_lookup_null_col(Relation index, AttrNumber attno,
								LionPostingSet *ps)
{
	LionState   *state = lion_index_column_state(index, attno);
	Buffer		buf;
	OffsetNumber off;

	memset(ps, 0, sizeof(LionPostingSet));
	ps->index = index;
	ps->attno = state->attno;
	ps->pinbuf = InvalidBuffer;
	ps->head = InvalidBlockNumber;
	ps->entryblk = InvalidBlockNumber;
	ps->entryoff = InvalidOffsetNumber;

	if (!lion_find_null_entry(index, state, BUFFER_LOCK_SHARE, &buf, &off))
	{
		if (BufferIsValid(buf))
			UnlockReleaseBuffer(buf);
		return false;
	}

	lion_posting_set_take(index, state, buf, off, ps);
	return true;
}

void
lion_posting_set_unpin(LionPostingSet *ps)
{
	if (BufferIsValid(ps->pinbuf))
	{
		ReleaseBuffer(ps->pinbuf);
		ps->pinbuf = InvalidBuffer;
		ps->nopin = true;
	}
	lion_list_pin_return(ps);
}

/*
 * Locate the entry of a NOPIN set again, this time keeping the pin (a single
 * lookup always does): what lion_count_one_set() counts a NOPIN set from.  The
 * stored key is of the index's own type, so this is a plain lookup of it -
 * the same entry, since a column has one entry per key, as it is NOW, which
 * is all a count that reads the index after locating the entry ever gets (a
 * chain is read page by page as the count goes, too).  An entry that has gone
 * meanwhile held nothing visible to anyone, because VACUUM deletes an entry
 * only once its set is empty; false then, with *fresh not found.
 */
static bool
lion_posting_set_relocate(const LionPostingSet *ps, LionPostingSet *fresh)
{
	LionState   *state = lion_index_column_state(ps->index, ps->attno);
	LionProbe	probe;
	Datum		key;

	Assert(ps->found && ps->nopin && ps->hasstoredkey);

	if (ps->keyisnull)
	{
		Buffer		buf;
		OffsetNumber off;

		memset(fresh, 0, sizeof(LionPostingSet));
		fresh->index = ps->index;
		fresh->attno = ps->attno;
		fresh->pinbuf = InvalidBuffer;
		fresh->head = InvalidBlockNumber;
		if (!lion_find_null_entry(ps->index, state, BUFFER_LOCK_SHARE, &buf,
								  &off))
		{
			if (BufferIsValid(buf))
				UnlockReleaseBuffer(buf);
			return false;
		}
		lion_posting_set_take(ps->index, state, buf, off, fresh);
		return true;
	}

	lion_probe_init(ps->index, state, InvalidOid, &probe);
	key = lion_probe_value(&probe, ps->storedkey);

	return lion_posting_set_locate(ps->index, state, &probe, key,
								   lion_probe_hash(state, &probe, key), fresh);
}

void
lion_posting_set_release(LionPostingSet *ps)
{
	if (BufferIsValid(ps->pinbuf))
		ReleaseBuffer(ps->pinbuf);
	lion_list_pin_return(ps);
	ps->pinbuf = InvalidBuffer;
	ps->nopin = false;
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

	/*
	 * DESIGN.md §22: the walk starts at the leftmost LEAF, which the descent
	 * hands back locked - the entry's head block is the tree's root and is
	 * only a leaf while the set fits one page.
	 */
	{
		Buffer		firstbuf = lion_posting_search(ps->index, NULL, 0, ps->head,
												   0, BUFFER_LOCK_SHARE, false);

		if (!BufferIsValid(firstbuf))
			blkno = InvalidBlockNumber;
		else
		{
			blkno = BufferGetBlockNumber(firstbuf);
			memcpy(img, BufferGetPage(firstbuf), BLCKSZ);
			UnlockReleaseBuffer(firstbuf);
		}
	}

	while (BlockNumberIsValid(blkno))
	{
		OffsetNumber off;
		OffsetNumber maxoff;

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

		if (!ok || !BlockNumberIsValid(blkno))
			break;

		CHECK_FOR_INTERRUPTS();

		{
			Buffer		pagebuf = ReadBuffer(ps->index, blkno);
			Page		page;

			LockBuffer(pagebuf, BUFFER_LOCK_SHARE);
			page = BufferGetPage(pagebuf);

			/* The ownership check of DESIGN.md §18; see lion_cursor_next_item(). */
			if (!lion_page_owns(page, ps->head) || !LionPageIsPostingLeaf(page))
			{
				UnlockReleaseBuffer(pagebuf);
				break;
			}
			memcpy(img, page, BLCKSZ);
			UnlockReleaseBuffer(pagebuf);
		}
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
		/*
		 * The entry's head block is the ROOT of the posting tree (DESIGN.md
		 * §22), so the first leaf comes from a descent for container key 0 -
		 * which is also how a root push-down cannot be raced: the descent
		 * hands back a page that WAS a leaf under the lock it read it with.
		 */
		cur->imgbuf = (PGAlignedBlock *) palloc(sizeof(PGAlignedBlock));
		cur->img = (Page) cur->imgbuf->data;
		cur->nextblk = InvalidBlockNumber;
		cur->descend = true;
		cur->seekckey = 0;

		/*
		 * Test hook: the entry has been copied and its leaf released, and
		 * nothing of the posting set is pinned yet - all this cursor holds is
		 * the root's block number.  test/isolation/vacuum_regrow_pushdown.spec
		 * sends the descent into a VACUUM that is in the middle of pushing
		 * that root down.  Compiles to nothing without injection points.
		 */
		LION_INJECTION_POINT("lion-count-chain-entered");
	}

	lion_cursor_next(cur);
}

/*
 * Take over a leaf the caller holds SHARE-locked: copy the page - items and
 * rightlink together, as lion_scan.c does - then drop the content lock but
 * keep the pin (DESIGN.md §9).
 */
static void
lion_cursor_take_page(LionSetCursor *cur, Buffer buf)
{
	memcpy(cur->img, BufferGetPage(buf), BLCKSZ);
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	cur->pinbuf = buf;
	cur->ownpin = true;

	cur->nextblk = LionPageGetOpaque(cur->img)->rightlink;
	cur->off = FirstOffsetNumber;
	cur->maxoff = PageGetMaxOffsetNumber(cur->img);
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

		if (cur->descend)
		{
			bool		found;

			/*
			 * A descent for seekckey: the first leaf of the set, or the leaf a
			 * seek jumped to (DESIGN.md §22).  It comes back locked, so no
			 * root push-down and no split can slip in between.
			 */
			cur->descend = false;
			buf = lion_posting_search(cur->set->index, NULL, 0, cur->set->head,
									  cur->seekckey, BUFFER_LOCK_SHARE, false);
			if (!BufferIsValid(buf))
			{
				cur->nextblk = InvalidBlockNumber;
				return NULL;
			}
			lion_cursor_take_page(cur, buf);
			cur->off = lion_page_find_item(cur->img, cur->seekckey, &found);
			CHECK_FOR_INTERRUPTS();
			continue;
		}

		if (!BlockNumberIsValid(cur->nextblk))
			return NULL;

		buf = ReadBuffer(cur->set->index, cur->nextblk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);

		/*
		 * DESIGN.md §18: every page reached through an entry's head or a
		 * rightlink has to still claim that posting set.  A root block is the
		 * one block this index never recycles, so owner_head identifies the
		 * set for the whole life of the index and a mismatch can only mean
		 * that the set was freed after it was located - which VACUUM does only
		 * once the set held nothing visible to anyone.  The cursor then simply
		 * ends here and contributes nothing further; it is never an error
		 * outside verify().  This is also what keeps a standby reader safe,
		 * where replay can reuse a page under a held pin because generic WAL
		 * cannot raise a recovery conflict.
		 *
		 * A leaf's rightlink always names another leaf, so a page above level
		 * zero is the same kind of accident and ends the walk the same way.
		 */
		if (!lion_page_owns(page, cur->set->head) ||
			!LionPageIsPostingLeaf(page))
		{
			UnlockReleaseBuffer(buf);
			cur->nextblk = InvalidBlockNumber;
			return NULL;
		}

		lion_cursor_take_page(cur, buf);

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

	/*
	 * A seek may have asked for a container key inside this segment's range
	 * (DESIGN.md §22).  Its pairs are sorted, so the ones below the target are
	 * skipped here rather than presented and thrown away by the merge; when no
	 * seek has happened mintarget is 0 or already behind us and this is a
	 * no-op.
	 */
	while (cur->segpos < n && ckeys[cur->segpos] < cur->mintarget)
		cur->segpos++;

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

/* Pull items until one of them yields a container. */
static void
lion_cursor_advance(LionSetCursor *cur)
{
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

	lion_cursor_advance(cur);
}

/*
 * Skip a packed INLINE payload forward to the first item that can hold
 * `target`, leaving *off in front of it.
 *
 * DEVIATION from DESIGN.md §22, which said an INLINE set would seek by binary
 * search: an inline payload is a sequence of items packed without padding and
 * carries no offsets to search, so this walks the item headers instead.  It
 * costs nothing over the sequential walk it replaces - one forward pass over
 * the payload either way - and an offset index built to allow a binary search
 * would have to make that very pass to build itself.
 */
static void
lion_inline_skip(const char *payload, Size paylen, Size *off, uint32 target,
				 LionContainer *buf)
{
	Size		prev = *off;

	while (lion_inline_fetch(payload, paylen, off, buf) > 0)
	{
		if (lion_item_last_ckey(buf) >= target)
		{
			*off = prev;		/* leave it for the walk to pick up */
			return;
		}
		prev = *off;
	}
}

/*
 * Position a CHAIN cursor's leaf on `target`: step right while the current
 * leaf cannot hold it and the walk is short, else descend (DESIGN.md §22).
 *
 * THE §9 PIN RULE.  This drops the pin on the leaf the cursor is standing on,
 * so it may only be called where lion_cursor_next() may: at a container key
 * whose container has already been through the visibility-map check, or whose
 * container carried no visibility-map obligation at all because nothing of it
 * reached the count.  All three callers are of the second kind, and all three
 * are the very places that used to call lion_ecursor_next() for the same
 * reason: lion_run_merge()'s `!alleq` branch, the AND node's wind-forward
 * loop, and the merge winding a NEGATED source up to the key it is about to
 * subtract at - everything such a source passes over is below that key and
 * contributes nothing to it.
 */
static void
lion_cursor_seek_leaf(LionSetCursor *cur, uint32 target)
{
	int			steps = 0;

	for (;;)
	{
		BlockNumber blk;
		Buffer		buf;
		Page		page;
		bool		found;

		if (!BufferIsValid(cur->pinbuf))
		{
			/* nothing in hand: let the next page come from a descent */
			cur->descend = true;
			cur->seekckey = target;
			return;
		}

		if (LionPageGetOpaque(cur->img)->maxckey >= target ||
			!BlockNumberIsValid(cur->nextblk))
		{
			/*
			 * The target is at or before the end of this page, or there is no
			 * page after it.  Either way this is where the walk resumes; an
			 * offset past the last item simply ends the cursor.
			 */
			cur->off = lion_page_find_item(cur->img, target, &found);
			return;
		}

		if (steps >= LION_POSTING_SEEK_STEPS)
		{
			cur->descend = true;
			cur->seekckey = target;
			lion_cursor_unpin(cur);
			cur->off = OffsetNumberNext(cur->maxoff);
			return;
		}

		blk = cur->nextblk;
		lion_cursor_unpin(cur);
		cur->off = OffsetNumberNext(cur->maxoff);

		buf = ReadBuffer(cur->set->index, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (!lion_page_owns(page, cur->set->head) ||
			!LionPageIsPostingLeaf(page))
		{
			UnlockReleaseBuffer(buf);
			cur->nextblk = InvalidBlockNumber;
			return;
		}
		lion_cursor_take_page(cur, buf);
		steps++;

		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * Move the cursor to the first container whose key is at or above `target`
 * (DESIGN.md §22).  A no-op when it is already there or past it, and when the
 * set is exhausted.
 *
 * This is what turns an intersection from a stream into a set of probes: the
 * merge advances whichever source has the largest container key and seeks the
 * others to it, so the work tracks the most selective side instead of the sum
 * of all of them.  See lion_cursor_seek_leaf() for the §9 pin rule it obeys.
 */
static void
lion_cursor_seek(LionSetCursor *cur, uint32 target)
{
	if (cur->set == NULL || !cur->set->found || !cur->valid)
		return;
	if (cur->cur->ckey >= target)
		return;

	cur->mintarget = target;
	cur->valid = false;
	cur->cur = NULL;

	/* Still inside a segment?  emit_segment() skips its pairs for us. */
	if (cur->seg != NULL)
	{
		if (lion_cursor_emit_segment(cur))
			return;
		cur->seg = NULL;
	}

	if (cur->set->mat != NULL)
	{
		const LionMatSet *mat = cur->set->mat;
		int			lo = cur->matidx;
		int			hi = mat->ncontainers;

		/* a private copy is an array: binary search it */
		while (lo < hi)
		{
			int			mid = lo + (hi - lo) / 2;

			if (lion_item_last_ckey(mat->containers[mid]) < target)
				lo = mid + 1;
			else
				hi = mid;
		}
		cur->matidx = lo;
	}
	else if (cur->set->is_inline)
		lion_inline_skip(cur->set->payload, cur->set->paylen, &cur->payoff,
						 target, cur->cbuf);
	else
		lion_cursor_seek_leaf(cur, target);

	lion_cursor_advance(cur);
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
	const LionContainer *cur;	/* sub[child].cur when it was pushed */
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
	 * The heap carries each child's container key AND its container INSIDE the
	 * entry rather than reading sub[i] while it sifts and again while it
	 * unions: a thousand-element IN list is a thousand LionExprCursors, a
	 * third of a megabyte, and chasing them through the heap's random access
	 * pattern cost more than the linear scan over all children that the heap
	 * replaced (measured: +4 ms on a 1000-value list at 1M rows).  Both fields
	 * only change when the child is advanced, which is also when it is pushed
	 * back on, so the copy is never stale; hot[] is the same entry moved
	 * across.
	 */
	struct LionOrHeapEnt *heap;
	int			nheap;
	struct LionOrHeapEnt *hot;
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
static void lion_ecursor_seek(LionExprCursor *c, uint32 target);

/* ---- the OR node's min-heap of children, keyed by container key ---- */

static inline void
lion_or_heap_push(LionExprCursor *c, int child)
{
	LionOrHeapEnt ent;
	int			i = c->nheap++;

	Assert(c->sub[child].valid);
	ent.ckey = c->sub[child].ckey;
	ent.child = child;
	ent.cur = c->sub[child].cur;

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

static inline LionOrHeapEnt
lion_or_heap_pop(LionExprCursor *c)
{
	LionOrHeapEnt top = c->heap[0];
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
			c->hot = (LionOrHeapEnt *) palloc(sizeof(LionOrHeapEnt) * c->nsub);
			c->heap = (LionOrHeapEnt *) palloc(sizeof(LionOrHeapEnt) * c->nsub);
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
			c->cur = c->hot[0].cur;
		else if (c->nhot < LION_OR_BITSET_MIN)
		{
			const LionContainer *a = c->hot[0].cur;

			for (i = 1; i < c->nhot; i++)
			{
				lion_container_or(a, c->hot[i].cur, c->acc[w]);
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
				lion_bits_or_container(c->bits, c->hot[i].cur);
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

		/*
		 * Wind the laggards forward; their containers cannot contribute.
		 * DESIGN.md §22: a laggard SEEKS to the key the others stand at
		 * instead of stepping through everything in between, which is the
		 * whole point of the posting tree.  Nothing of the keys it skips ever
		 * reaches the visibility map, so the §9 rule is untouched - this is
		 * the same window the code used to call lion_ecursor_next() in.
		 */
		for (i = 0; i < c->nsub; i++)
		{
			if (c->sub[i].ckey != maxckey)
			{
				alleq = false;
				lion_ecursor_seek(&c->sub[i], maxckey);
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
				int			child = c->hot[i].child;

				lion_ecursor_next(&c->sub[child]);
				if (c->sub[child].valid)
					lion_or_heap_push(c, child);
			}
			c->nhot = 0;
			break;

		case LION_KN_AND:

			/*
			 * Every child stands at c->ckey, but only ONE of them has to step:
			 * lion_ecursor_build() then finds it ahead of the others and SEEKS
			 * them to it (DESIGN.md §22), which is one probe each instead of a
			 * walk.  Stepping all of them would cost every child a container
			 * at c->ckey + 1 that the seek is about to skip anyway.
			 */
			lion_ecursor_next(&c->sub[0]);
			break;
	}

	lion_ecursor_build(c);
}

/*
 * Move past every container key below `target` (DESIGN.md §22).
 *
 * This is lion_ecursor_next() with a destination instead of a step, and it
 * obeys the same §9 rule: it is only ever called at a container key whose
 * containers carried no visibility-map obligation - a key the intersection
 * cannot use - so the pins it lets go had nothing counted from them.
 */
static void
lion_ecursor_seek(LionExprCursor *c, uint32 target)
{
	int			i;

	if (c->node == NULL || !c->valid || c->ckey >= target)
		return;

	switch (c->kind)
	{
		case LION_KN_KEY:
			lion_cursor_seek(&c->leaf, target);
			break;

		case LION_KN_OR:

			/*
			 * The children standing at the current key move to the target, and
			 * so does every child the heap still holds below it; the ones
			 * already at or above it stay where they are.  Popping from a
			 * min-heap while its root is below the target touches exactly
			 * those and no more.
			 */
			for (i = 0; i < c->nhot; i++)
			{
				int			child = c->hot[i].child;

				lion_ecursor_seek(&c->sub[child], target);
				if (c->sub[child].valid)
					lion_or_heap_push(c, child);
			}
			c->nhot = 0;

			while (c->nheap > 0 && c->heap[0].ckey < target)
			{
				LionOrHeapEnt ent = lion_or_heap_pop(c);

				lion_ecursor_seek(&c->sub[ent.child], target);
				if (c->sub[ent.child].valid)
					lion_or_heap_push(c, ent.child);
			}
			break;

		case LION_KN_AND:
			/* every child stands at c->ckey, and the result needs all of them */
			for (i = 0; i < c->nsub; i++)
				lion_ecursor_seek(&c->sub[i], target);
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
 *	- a leaf has it unless its set has been materialized or was located
 *	  without its pin (NOPIN, DESIGN.md §15); a leaf whose key has no entry
 *	  yields nothing, so it has it vacuously;
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
			return (!sets[node->keyno].found ||
					(sets[node->keyno].mat == NULL && !sets[node->keyno].nopin));

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
 * marked all-visible.  Only the bits of `wanted` are answered for - the caller
 * never looks at the others, and skipping them is what keeps a one-member
 * container to a single map byte.  *vmbuf is the caller's visibility map pin;
 * it is moved
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
 *
 * The work is split in three so that what the compiler sees at the call site is
 * the case that happens.  This runs once per container - tens of thousands of
 * times for one IN list - and all but one container in five hundred is answered
 * by reading a run of bytes off ONE map page; the loop that used to be here
 * spelled out the two-page case inline, and whether gcc inlined the whole thing
 * or none of it moved this query by a third in an -O1 build, in whichever
 * direction an unrelated edit to the file happened to push it.  Now the common
 * path is small and always inlined, and the straddling case is out of line.
 */

/*
 * The blocks of one map page: `seg`, the wanted bits shifted down so that bit 0
 * is block blk, whose answers belong at bit `b` of the result.  seg != 0.
 */
static pg_always_inline uint64
lion_vm_allvisible_page(Relation heap, BlockNumber blk, uint64 seg, int b,
					   Buffer *vmbuf)
{
	uint64		mask = 0;
	const char *map;
	uint32		mapbyte;
	int			jlo;
	int			jhi;
	int			j;

	Assert(seg != 0);

	/*
	 * Take the pin the way visibilitymap_get_status() does: same buffer reuse,
	 * same refusal to extend the fork.  Its return value is the status of blk
	 * itself, which the byte read below repeats.
	 */
	if (!visibilitymap_pin_ok(blk, *vmbuf))
		(void) visibilitymap_get_status(heap, blk, vmbuf);

	/* the fork stops short of these blocks: none of them is all-visible */
	if (!BufferIsValid(*vmbuf))
		return 0;

	map = (const char *) PageGetContents(BufferGetPage(*vmbuf));
	mapbyte = LION_VM_HEAPBLK_TO_MAPBYTE(blk);

	/*
	 * Only the map bytes that hold a block the caller asked about.  A container
	 * built from a sparse segment (DESIGN.md §13) has its members on ONE of the
	 * sixty-four heap blocks it covers, so that is one byte instead of sixteen
	 * - and a disjoint sum (§15) asks this question once per entry per
	 * container key, tens of thousands of times for one IN list.  A container
	 * with members everywhere, which is what a low-cardinality column has,
	 * still reads its sixteen bytes in one unbroken run.
	 */
	jlo = pg_rightmost_one_pos64(seg) / LION_VM_HEAPBLOCKS_PER_BYTE;
	jhi = pg_leftmost_one_pos64(seg) / LION_VM_HEAPBLOCKS_PER_BYTE;

	for (j = jlo; j <= jhi; j++)
	{
		/*
		 * Squeeze the four all-visible bits of the byte - the low bit of each
		 * pair - down into a nibble of the result.
		 */
		uint8		v = (uint8) (map[mapbyte + j] & 0x55);

		v = (uint8) ((v | (v >> 1)) & 0x33);
		v = (uint8) ((v | (v >> 2)) & 0x0f);
		mask |= ((uint64) v) << (b + j * LION_VM_HEAPBLOCKS_PER_BYTE);
	}

	return mask;
}

/*
 * The rare container whose blocks straddle two map pages.
 * LION_VM_HEAPBLOCKS_PER_PAGE (32672 at 8K) is not a multiple of
 * LION_BLOCKS_PER_CONTAINER, so roughly one container in five hundred does; it
 * can never be three pages, a map page holding five hundred times a
 * container's worth of blocks.  `n` is how many of the container's blocks are
 * on the first of the two.
 */
static pg_noinline uint64
lion_vm_allvisible_mask_split(Relation heap, BlockNumber firstblk, uint64 wanted,
							 int n, Buffer *vmbuf)
{
	uint64		mask = 0;
	uint64		seg;

	Assert(n > 0 && n < LION_BLOCKS_PER_CONTAINER);
	Assert(n % LION_VM_HEAPBLOCKS_PER_BYTE == 0);

	seg = wanted & ((UINT64CONST(1) << n) - 1);
	if (seg != 0)
		mask |= lion_vm_allvisible_page(heap, firstblk, seg, 0, vmbuf);

	seg = wanted >> n;
	if (seg != 0)
		mask |= lion_vm_allvisible_page(heap, firstblk + (BlockNumber) n, seg,
									   n, vmbuf);

	return mask;
}

static pg_always_inline uint64
lion_vm_allvisible_mask(Relation heap, BlockNumber firstblk, uint64 wanted,
					   Buffer *vmbuf)
{
	int			n;

	Assert(firstblk % LION_BLOCKS_PER_CONTAINER == 0);

	/* No member on any of these blocks: not even the pin is needed. */
	if (wanted == 0)
		return 0;

	/* how many of the container's blocks live on firstblk's map page */
	n = (int) (LION_VM_HEAPBLOCKS_PER_PAGE -
			   (firstblk % LION_VM_HEAPBLOCKS_PER_PAGE));
	if (unlikely(n < LION_BLOCKS_PER_CONTAINER))
		return lion_vm_allvisible_mask_split(heap, firstblk, wanted, n, vmbuf);

	return lion_vm_allvisible_page(heap, firstblk, wanted, 0, vmbuf);
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

	if (lion_vm_allvisible_mask(heap, firstblk, members, vmbuf) == allvis)
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
 *	2. HOT pruning could rewrite the chain under us - a core scan's, or on 19
 *	   and later the count's own, just before it reads the block (see
 *	   lion_recheck_heap_heap()).  It may: it can turn the
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
		MemoryContext oldcxt = MemoryContextSwitchTo(cx->cxt);
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
		MemoryContextSwitchTo(oldcxt);
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
 * Step 1 of counting a container: ask the visibility map about every heap
 * block that has members, and either count the members outright (plus a
 * predicate lock, as an index-only scan would take) or queue the block's TIDs
 * for a heap recheck under the caller's snapshot.
 *
 * THIS IS THE HEART OF DESIGN.md SECTION 9, and its contract is an ordering
 * one: on entry the page every source container was copied from is still
 * PINNED, and the caller may only let those pins go - which means advancing a
 * cursor - after this function has returned.  Doing it the other way round
 * would let a concurrent VACUUM finish ambulkdelete on the page just read,
 * prune the heap and set all-visible, after which this would count dead
 * tuples.
 *
 * Two callers, and both are written so that the order can be checked by
 * reading them: lion_count_container() below, which advances the merge's
 * cursors straight afterwards, and lion_count_one_set(), the inner loop of
 * the disjoint sum of DESIGN.md §15, which advances its single cursor.
 */
static void
lion_count_container_vm(LionCountCtx *cx, const LionContainer *c)
{
	BlockNumber firstblk = lion_ckey_first_block(c->ckey);
	uint64		members;		/* blocks of this container that have members */
	uint64		allvis;			/* blocks marked all-visible in the VM */
	uint64		dirty;			/* blocks with members that need a heap recheck */

	/*
	 * Test hook: the containers have been copied out, the source pages are
	 * still pinned, and the visibility map has not been consulted yet.  A
	 * VACUUM that reaches ambulkdelete while a backend waits here must block
	 * on the cleanup lock; test/isolation/count_vacuum_race.spec proves it.
	 * Compiles to nothing without --enable-injection-points.
	 */
	LION_INJECTION_POINT("lion-count-containers-pinned");

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
	if (cx->in_recovery || cx->novm)
		allvis = 0;				/* see lion_count_sources(): no interlock */
	else
	{
		allvis = lion_vm_allvisible_mask(cx->heap, firstblk, members,
										&cx->vmbuf);
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

}

/*
 * The merge's form: count the container, then let the cursors move on.
 *
 * DESIGN.md section 9: every heap block of *c has been checked against the
 * visibility map by the time the loop below runs, so - and only now - the pins
 * on the pages the source containers came from may be released.
 * lion_ecursor_next() is what releases them.
 *
 * Only the DRIVER is flagged to move on (DESIGN.md §22).  The others stay
 * where they are and are SOUGHT to wherever the driver gets to on the next
 * round of the merge, which is what makes their work one probe per container
 * key of the driver rather than a walk; a cursor that keeps its place also
 * keeps its pin, which is a pin too many and never a wrong answer.
 */
static void
lion_count_container(LionCountCtx *cx, const LionContainer *c,
					LionExprCursor *cursors, int nsources)
{
	int			i;

	lion_count_container_vm(cx, c);

	for (i = 0; i < nsources; i++)
	{
		if (cursors[i].advance)
			lion_ecursor_next(&cursors[i]);
	}
}

/*
 * Count ONE posting set on its own.
 *
 * Three callers, all of them counts that have nothing to merge: each entry of
 * the disjoint sum of DESIGN.md §15, each entry of the sum-over-all of §14, and
 * any count that comes down to a single set - a plain `WHERE k = 5`, or one
 * group of a GROUP BY with no other clause.  It is lion_run_merge() with every
 * source but one removed, written out because that takes an expression cursor,
 * a tree node and a per-container pass over the source array out of a loop that
 * runs tens of thousands of times for one IN list.
 *
 * The DESIGN.md §9 ordering is the same and is visible in the same two adjacent
 * statements: consult the visibility map, and only then advance the cursor,
 * which is the one thing that unpins the page the container was copied from.
 * The set's OWN pin (an inline entry's) is the caller's and is not touched
 * here: lion_cursor_init() borrows it and lion_cursor_close() leaves it.
 *
 * A NOPIN set has no such pin, and nothing else is counted with it to carry
 * the interlock, so its entry is located again here and counted from that
 * copy, under the pin the new lookup keeps until the count of it is done
 * (DESIGN.md §15).  One set at a time: the pass holds that one pin.
 *
 * An existence test (DESIGN.md §26) stops after the first container that
 * settles it; the cursor has moved on by then, which is where the §9 ordering
 * says it may.
 */
static void
lion_count_one_set(LionCountCtx *cx, LionPostingSet *ps)
{
	LionPostingSet fresh;
	LionSetCursor cur;

	if (ps->nopin)
	{
		if (!lion_posting_set_relocate(ps, &fresh))
			return;
		ps = &fresh;
	}

	lion_cursor_init(&cur, ps, cx);
	while (cur.valid)
	{
		lion_count_container_vm(cx, cur.cur);
		lion_cursor_next(&cur);
		if (lion_exists_settled(cx))
			break;
		CHECK_FOR_INTERRUPTS();
	}
	lion_cursor_close(&cur);

	if (ps == &fresh)
		lion_posting_set_release(&fresh);
}

/*
 * Is an EXISTENCE test answered yet (DESIGN.md §26)?  Always false for a
 * count.  Called by the merge after each container it has put through the
 * visibility map - so after lion_count_container_vm() has asked its question
 * under the source pins, which is the whole of the DESIGN.md §9 obligation -
 * and never anywhere else.
 *
 *	- A member on an all-visible block (cx->count > 0) is a visible row, by
 *	  exactly the argument that lets a count add it: the settle is immediate,
 *	  and the recheck TIDs the same container may have queued are dropped
 *	  unread.  They could only have added to an answer already known.
 *	- Otherwise whatever this container queued is rechecked now instead of at
 *	  the end of the merge.  The flush is the ordinary one (its argument is on
 *	  lion_recheck_add(): a recheck needs no index pin), and a container
 *	  boundary is a heap-block boundary, so no block is split across batches.
 *	  One visible row settles the test.
 *
 * So a test on an all-visible heap reads the first container of the
 * intersection and stops, and one on a dirty heap rechecks container by
 * container until its first visible row - never more than one container past
 * it.
 */
static bool
lion_exists_settled(LionCountCtx *cx)
{
	if (!cx->exists)
		return false;

	if (cx->count > 0)
	{
		cx->ntids = 0;
		cx->tids_sorted = true;
		return true;
	}

	lion_recheck_flush(cx);
	return cx->recheck_count > 0;
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

		if (lion_table_fetch_tid(cx->heap, &tid, cx->snapshot, NULL))
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
 *
 * PRUNING ON ACCESS (PostgreSQL 19 and later; DESIGN.md §11, "On-access
 * pruning sets the visibility map too").  Every block that is read is first
 * offered to heap_page_prune_opt(), exactly as a bitmap heap scan offers it
 * (BitmapHeapScanNextBlock()): pinned, not locked, with the count's own
 * visibility map pin to reuse.  If the page qualifies - something prunable,
 * little free space, the cleanup lock free right now - it is pruned, and when
 * the statement does not modify the relation (cx->rel_read_only) and what is
 * left is visible to every snapshot, it is marked all-visible, so that the
 * next count, or the next group of this one, reads it from the map instead of
 * rechecking it.  That a map bit set this way is as good as one VACUUM set -
 * a pinned container's TID on an all-visible page is exactly one visible row -
 * is the argument of that DESIGN.md subsection, checked against pruneheap.c.
 * Here it only has to be safe to call:
 *
 *	- no lock is held that it could wait behind: the index pages of the merge
 *	  are pinned, never locked, at a flush, the heap cleanup lock is only ever
 *	  tried, and the VM page is pinned before that and locked only briefly,
 *	  as by every core scan;
 *	- the answers below are taken after it, from the pruned page, and pruning
 *	  removes only versions no snapshot can see and never moves a root line
 *	  pointer, which is all the per-TID recheck and the cache rely on (point 2
 *	  of the argument on lion_vis_cache_lookup());
 *	- it does nothing in recovery (its own first test), and on 16-18 the call
 *	  compiles to nothing (lion_compat.h), where core's scans still prune.
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
		lion_heap_page_prune_opt(cx->heap, buf, &cx->vmbuf, cx->rel_read_only);
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

/*
 * The SQL-callable counts come through here and cannot see the statement that
 * called them, which may be modifying the relation: they prune on access but
 * never ask pruning to set the visibility map (rel_read_only = false, as core
 * passes for a scan of a result relation; DESIGN.md §11).
 */
int64
lion_count_sources(Relation heap, Snapshot snapshot, int nsources,
				  LionCountSource *sources, LionCountStats *stats)
{
	return lion_count_sources_cached(heap, snapshot, nsources, sources, stats,
									NULL, false);
}

/*
 * One pass of the merge: intersect the positive sources container key by
 * container key, subtract the negated ones, and count what is left against the
 * visibility map.  Everything the caller set up in *cx - the recheck batch, the
 * visibility map pin, the visibility cache, the statistics - is accumulated
 * into, so this may be called more than once for one count.  The disjoint-sum
 * short-circuit below is the caller that does.
 *
 * This is where DESIGN.md §9 lives; nothing about the interlock changes with
 * how often it runs, because each pass takes its pins, asks the visibility map
 * and drops them again inside lion_count_container().
 */
static void
lion_run_merge(LionCountCtx *cx, int nsources, LionCountSource *sources,
			  LionKeyNode **trees)
{
	LionExprCursor *cursors;
	LionContainer *work[2];
	double	   *est;			/* members of each positive source */
	int		   *probeord;		/* the positive sources, least first */
	int			nprobe = 0;
	int			driver;
	int			i;

	cursors = (LionExprCursor *) palloc0(sizeof(LionExprCursor) * nsources);

	/*
	 * WHICH SOURCE DRIVES, AND IN WHICH ORDER THE OTHERS ARE PROBED
	 * (DESIGN.md §22, §25).  The merge is a leapfrog join: one source is
	 * walked sequentially and the others are PROBED at the container keys it
	 * produces.  The driver has to be the most selective one, because the work
	 * is its container count times the number of sources; a dense driver would
	 * make every other source probe at every container key of the heap, which
	 * is the walk this replaces.
	 *
	 * The others are probed in ASCENDING SELECTIVITY, which is what lets the
	 * loop below stop at the first one that empties the intersection: the
	 * fewer members a source has, the likelier it is to be the one that kills
	 * the container key, and everything after it in this order is then never
	 * sought at all.  Ordering by anything else would still be correct and
	 * would only read more pages.
	 *
	 * The estimate is the sum of the entries' own row counts, which the
	 * located posting sets carry already (`ntids`), so no page is read to
	 * decide it.  It is exact for the ordinary one-set source and an upper
	 * bound for a union; a wrong guess costs performance and never an answer.
	 * The sort is an insertion sort because a query has a handful of sources
	 * and this runs once per count; it is STABLE, so probeord[0] is the first
	 * source with the fewest members - the same one this used to pick with a
	 * single `est < bestest` pass, and the same one the cost model picks.
	 */
	est = (double *) palloc0(sizeof(double) * nsources);
	probeord = (int *) palloc(sizeof(int) * nsources);
	for (i = 0; i < nsources; i++)
	{
		int			j;

		if (sources[i].negated)
			continue;
		for (j = 0; j < sources[i].nsets; j++)
			est[i] += (double) sources[i].sets[j].ntids;

		for (j = nprobe++; j > 0 && est[probeord[j - 1]] > est[i]; j--)
			probeord[j] = probeord[j - 1];
		probeord[j] = i;
	}
	Assert(nprobe > 0);
	driver = probeord[0];

	/*
	 * Only an intersection needs a place to put one: a single source hands its
	 * own container straight to lion_count_container(), and the disjoint sum
	 * of DESIGN.md §15 runs this once per entry, where two 4 KiB buffers per
	 * pass are the bulk of the work.
	 */
	work[0] = work[1] = NULL;
	if (nsources > 1)
	{
		work[0] = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
		work[1] = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	}

	for (i = 0; i < nsources; i++)
		lion_ecursor_init(&cursors[i], trees[i], sources[i].sets,
						 sources[i].nsets, cx);

	/*
	 * Merge the sources by container key.  Containers are stored in ascending
	 * ckey order both inline and along a chain, and an expression cursor
	 * preserves that, so a single forward pass over all of them is enough.
	 */
	for (;;)
	{
		uint32		maxckey;
		const LionContainer *acc = NULL;
		bool		abandoned = false;
		int			w = 0;
		int			k;

		/* The positive sources drive the merge; all must still have data. */
		for (k = 0; k < nprobe; k++)
		{
			if (!cursors[probeord[k]].valid)
				goto merge_done;
		}

		maxckey = cursors[probeord[0]].ckey;
		for (k = 1; k < nprobe; k++)
		{
			if (cursors[probeord[k]].ckey > maxckey)
				maxckey = cursors[probeord[k]].ckey;
		}

		/*
		 * Past this container key only the DRIVER steps; the others are left
		 * standing where they are and the next round of this loop seeks them
		 * to wherever the driver has got to (DESIGN.md §22).  A cursor that
		 * keeps its place also keeps its pin, which is a pin too many and
		 * never a wrong answer (see lion_ecursor_next()).
		 */
		for (i = 0; i < nsources; i++)
			cursors[i].advance = false;
		cursors[driver].advance = true;

		/*
		 * THE INTERSECTION OF THE POSITIVE SOURCES, BUILT AS THEY ARE SOUGHT
		 * (DESIGN.md §25).  The sources are taken in probe order - the driver,
		 * which stands at or below the target already, then the others least
		 * selective-first - and each one is sought to the target and folded
		 * into the accumulator before the next one is touched at all.  The
		 * moment the accumulator is empty the container key is ABANDONED: the
		 * sources after it in the order are not sought, and the pages their
		 * probes would have read are not read.  Before this the whole round of
		 * seeks was made first and the intersection looked at afterwards,
		 * which read a dense source's leaf at every container key the driver
		 * produced, including the ones where the selective sources had already
		 * ruled the key out.
		 *
		 * THE DESIGN.md §9 PIN DISCIPLINE IS UNCHANGED, and this is the
		 * argument.  The rule is that the visibility-map question about a
		 * container's heap blocks is asked before the pin on the page that
		 * container came from is released.  An abandoned container key asks NO
		 * such question - nothing of it reaches lion_count_container(), so
		 * nothing of it reaches the count - so there is no obligation to
		 * discharge for any of the pages it touched, sought or not.  What the
		 * sources that were not sought keep is their PINS, exactly where they
		 * stood: a pin too many never makes a count wrong, it only makes
		 * VACUUM wait (see lion_ecursor_next()).  The pages the sources that
		 * WERE sought let go of are let go by lion_ecursor_seek(), which is
		 * the same window, at the same kind of key, as before §25.
		 */
		for (k = 0; k < nprobe; k++)
		{
			int			s = probeord[k];
			int			m;

			if (cursors[s].ckey < maxckey)
			{
				/*
				 * DESIGN.md §22: a lagging source SEEKS to the largest key any
				 * positive source stands at rather than stepping one container
				 * at a time.  That is the leapfrog join the posting tree
				 * exists for.
				 */
				lion_ecursor_seek(&cursors[s], maxckey);
				if (!cursors[s].valid)
					goto merge_done;
			}

			if (cursors[s].ckey > maxckey)
			{
				/*
				 * This source has no container at the target at all, so the
				 * intersection there is empty and the key is dead - the same
				 * abandonment as an empty accumulator, one step earlier.  The
				 * sources after it in the order are left standing; the round
				 * starts again at the key this one found, which is the next
				 * one that can possibly survive.
				 */
				for (m = k + 1; m < nprobe; m++)
				{
					if (cursors[probeord[m]].ckey < maxckey)
						cx->stats.probes_avoided++;
				}

				maxckey = cursors[s].ckey;
				acc = NULL;
				w = 0;
				k = -1;
				CHECK_FOR_INTERRUPTS();
				continue;
			}

			if (acc == NULL)
				acc = cursors[s].cur;
			else
			{
				lion_container_and(acc, cursors[s].cur, work[w]);
				acc = work[w];
				w ^= 1;
			}

			if (lion_container_cardinality(acc) == 0)
			{
				/*
				 * Nothing can come back once the accumulator is empty, so the
				 * rest of the order is not sought.  What that saves is one
				 * seek each - a descent, or a step or two right - and it is
				 * counted for EXPLAIN: a source standing at the target or
				 * beyond it would not have been sought anyway, so only the
				 * ones still below it count.
				 */
				for (m = k + 1; m < nprobe; m++)
				{
					if (cursors[probeord[m]].ckey < maxckey)
						cx->stats.probes_avoided++;
				}
				abandoned = true;
				break;
			}
		}

		if (abandoned)
		{
			/*
			 * Nothing of this container key survives, so no visibility-map
			 * question is asked about it and the driver's page may go.  Only
			 * the driver steps: the others are still standing at (or below)
			 * the key it is leaving and are sought from there on the next
			 * round.
			 */
			for (i = 0; i < nsources; i++)
			{
				if (cursors[i].advance)
					lion_ecursor_next(&cursors[i]);
			}
			CHECK_FOR_INTERRUPTS();
			continue;
		}

		/* ... minus the negated ones (DESIGN.md §14, `col IS NOT NULL`). */
		for (i = 0; i < nsources; i++)
		{
			if (!sources[i].negated)
				continue;
			if (cursors[i].valid && cursors[i].ckey < maxckey)
				lion_ecursor_seek(&cursors[i], maxckey);	/* nothing to subtract there */
			if (!cursors[i].valid || cursors[i].ckey != maxckey)
				continue;

			if (lion_container_cardinality(acc) > 0)
			{
				lion_container_andnot(acc, cursors[i].cur, work[w]);
				acc = work[w];
				w ^= 1;
			}
		}

		if (lion_container_cardinality(acc) > 0)
		{
			lion_count_container(cx, acc, cursors, nsources);

			/*
			 * An existence test (DESIGN.md §26) is done at the first container
			 * that shows a visible row.  The VM question about this one has
			 * been asked under its pins, and the cursors have moved on; what
			 * they still pin is released below, as at the end of any merge.
			 */
			if (lion_exists_settled(cx))
				goto merge_done;
		}
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

	pfree(cursors);
	pfree(est);
	pfree(probeord);
	if (work[0] != NULL)
	{
		pfree(work[0]);
		pfree(work[1]);
	}
}

/*
 * Is this tree the plain union of every one of the source's sets, each set
 * appearing exactly once?  That is the shape lion_locate_leaf() gives an IN
 * list and lion_source_tree() gives a source with no tree of its own, and it
 * is the only shape the disjoint-sum short-circuit below may be applied to: a
 * set that appears twice in the tree, or an AND anywhere in it, would make the
 * union something other than the sum.
 */
static bool
lion_tree_is_flat_union(const LionKeyNode *node, int nsets)
{
	int			i;

	if (node == NULL)
		return true;			/* the implicit union of all the sets */
	if (node->kind == LION_KN_KEY)
		return nsets == 1 && node->keyno == 0;
	if (node->kind != LION_KN_OR || node->nargs != nsets)
		return false;
	for (i = 0; i < nsets; i++)
	{
		if (node->args[i]->kind != LION_KN_KEY || node->args[i]->keyno != i)
			return false;
	}
	return true;
}

/*
 * THE DISJOINT-SUM SHORT-CIRCUIT (DESIGN.md §15)
 * ==============================================
 * The count of a union is not in general the sum of the counts - a row in two
 * of the sets would be counted twice - but for the posting sets of DIFFERENT
 * entries of one SCALAR lion index it is, because those sets are disjoint by
 * construction:
 *
 *	- a scalar opclass extracts exactly ONE key from a row's column value, so
 *	  the build and the insert path put that row's TID under exactly one entry:
 *	  the entry of its value, or the reserved NULL entry of DESIGN.md §14 when
 *	  the value is NULL, or (for a multi-key opclass only) the reserved EMPTY
 *	  entry of §17;
 *	- the reserved entries are therefore disjoint from every value entry as
 *	  well, and from each other;
 *	- so no TID is in two entries, and the union of any set of entries has
 *	  exactly as many members as their counts add up to.
 *
 * DESIGN.md §14 already relies on exactly this: `col IS NOT NULL` with nothing
 * else to drive the merge is answered by summing the counts of every entry of
 * the column's index, and that is only the count of their union because the
 * entries are disjoint.  This is the same argument applied to the entries an
 * IN list names instead of to all of them.
 *
 * A MULTI-KEY opclass (DESIGN.md §17) is excluded, and this is the whole
 * reason the test below asks the index rather than trusting the caller: one
 * row yields many keys there, so it appears under several entries and the sum
 * over them is not a row count.  §17 refuses such an index as a GROUP BY or
 * sum-over-all driver for the same reason.
 *
 * What the caller has to promise, because this code cannot see it, is that no
 * two of the sets are the SAME entry: `src->disjoint`.
 * lion_posting_set_lookup_many() keeps that promise by dropping duplicates by
 * entry identity and not merely by value.
 *
 * Finally, the short-circuit only applies while this source is the ONLY
 * positive one.  An intersection has to be evaluated container key by
 * container key, and for that the union has to be materialised per key, which
 * is precisely the merge.
 */
static bool
lion_sources_disjoint_sum(int nsources, const LionCountSource *sources)
{
	const LionCountSource *src = &sources[0];
	Relation	index = NULL;
	AttrNumber	attno = 0;
	int			i;

	if (nsources != 1)
		return false;
	if (src->negated || !src->disjoint)
		return false;
	if (src->nsets < 2)
		return false;			/* one set is its own union already */
	if (!lion_tree_is_flat_union(src->tree, src->nsets))
		return false;

	for (i = 0; i < src->nsets; i++)
	{
		if (!src->sets[i].found)
			continue;
		if (index == NULL)
		{
			index = src->sets[i].index;
			attno = (AttrNumber) src->sets[i].attno;
		}
		else if (src->sets[i].index != index)
			return false;		/* entries of two indexes are not disjoint */
		else if ((AttrNumber) src->sets[i].attno != attno)
			return false;		/* nor are two COLUMNS of one index (§24) */
	}
	if (index == NULL)
		return false;

	return !lion_index_column_state(index, attno)->multikey;
}

/*
 * ... and is it FASTER?  The short-circuit above is about whether summing is
 * the RIGHT answer; this is about whether it is the cheap one, and the two are
 * independent.
 *
 * Both paths read every container of every set exactly once and count the same
 * members.  What they do not share is where the per-container overheads land:
 *
 *	- the SUM asks the visibility map once per (set, container key), because
 *	  each set is counted on its own;
 *	- the MERGE asks it once per container key, for the union, and pays instead
 *	  a heap sift and a union step per container.
 *
 * So the sum wins exactly when there is little to amortize.  Two things say
 * there is, and either one is enough to send the count back to the merge:
 *
 *	- DENSE sets.  If an entry has many rows in the SAME container key, the
 *	  merge folds all the sets' members at that key into one container and asks
 *	  the visibility map about it once.  Measured at 1M rows (15385 heap pages,
 *	  241 container keys), `k IN (1000 values)` with the sum against the merge:
 *	  20000 distinct keys (1.0 rows per key per container key) 3.6 ms against
 *	  9.9, 5000 keys (1.0) 11.5 against 21.5, 2000 keys (2.1) 25.1 against 32.2,
 *	  500 keys (8.3) 21.2 against 9.0, and 200 keys (20.7) 14.0 against 6.5.
 *	  The crossover is between two and eight rows per container key, so the
 *	  test is four.
 *	- and MANY of them, because the merge only becomes good at dense sets once
 *	  it reaches its bitset image (LION_OR_BITSET_MIN containers at one key);
 *	  below that it folds them pairwise, which is quadratic in the sets.  On the
 *	  200-key column above, sum against merge at 5 values is 0.49 ms against
 *	  0.64, at 15 values 1.14 against 2.37, at 30 values 2.15 against 7.6 - and
 *	  at 50, where the image takes over, 3.5 against 2.5.
 *
 * The density is read off the entries' own row counts, which the posting sets
 * carry already (`ntids`), against the number of container keys the heap has;
 * no extra page is touched to decide this.  lion_cost_count_rel() makes the
 * same test from the planner's estimates, so that the price the node is chosen
 * on is the price of the path it will take; LION_SUM_MAX_DENSITY and
 * LION_OR_BITSET_MIN live in lion_count.h for that reason.
 */
static bool
lion_sum_is_cheaper(Relation heap, const LionCountSource *src)
{
	double		ckeys;
	double		density;
	int64		ntids = 0;
	int			nfound = 0;
	int			i;

	/* Too few sets for the merge's bitset image: it would fold them pairwise. */
	if (src->nsets < LION_OR_BITSET_MIN)
		return true;

	for (i = 0; i < src->nsets; i++)
	{
		if (!src->sets[i].found)
			continue;
		ntids += src->sets[i].ntids;
		nfound++;
	}
	if (nfound < LION_OR_BITSET_MIN)
		return true;

	ckeys = (double) RelationGetNumberOfBlocks(heap) / LION_BLOCKS_PER_CONTAINER;
	if (ckeys < 1.0)
		ckeys = 1.0;

	/* rows per container key in the average entry */
	density = ((double) ntids / nfound) / ckeys;

	return density <= LION_SUM_MAX_DENSITY;
}

/*
 * Is every posting set of every source held in an index whose records replay
 * under a cleanup lock (DESIGN.md §25)?  That is the condition for trusting
 * the visibility map in recovery; see the comment at cx.in_recovery below.
 */
static bool
lion_sources_all_rmgr(int nsources, LionCountSource *sources)
{
	int			i,
				j;

	for (i = 0; i < nsources; i++)
	{
		for (j = 0; j < sources[i].nsets; j++)
		{
			LionPostingSet *ps = &sources[i].sets[j];

			if (!ps->found)
				continue;		/* an absent key contributes no TID */
			if (ps->index == NULL ||
				lion_wal_mode(ps->index) != LION_WAL_MODE_RMGR)
				return false;
		}
	}

	return true;
}

/*
 * Does this source hold a set that was located without its pin (DESIGN.md
 * §15)?
 */
static bool
lion_source_has_nopin(const LionCountSource *src)
{
	int			j;

	for (j = 0; j < src->nsets; j++)
	{
		if (src->sets[j].found && src->sets[j].nopin)
			return true;
	}
	return false;
}

/*
 * Does the whole count come down to ONE posting set?
 *
 * That is not the short-circuit above and needs none of its argument: there is
 * no union to take apart, so nothing about disjointness, about the opclass or
 * about how the caller found the set comes into it.  It is only worth asking
 * because it is the shape of every group of a GROUP BY, of every entry of the
 * sum-over-all of DESIGN.md §14, and of a plain `WHERE k = 5` - and because
 * lion_count_one_set() answers it without an expression cursor, a tree node or
 * a per-container pass over a one-element source array.
 */
static bool
lion_sources_one_set(int nsources, const LionCountSource *sources)
{
	return (nsources == 1 &&
			!sources[0].negated &&
			sources[0].nsets == 1 &&
			lion_tree_is_flat_union(sources[0].tree, 1));
}

/*
 * The count - or, with exists set, the existence test of DESIGN.md §26 - of
 * (intersection of the positive sources) minus (the negated ones).  Both
 * public forms below are this one function, so that an existence test reads
 * containers, asks the visibility map and rechecks the heap exactly the way a
 * count does, and differs only in where it stops (lion_exists_settled()).
 */
static int64 lion_count_sources_run(Relation heap, Snapshot snapshot,
									int nsources, LionCountSource *sources,
									LionCountStats *stats,
									LionVisCache *cache, bool rel_read_only,
									bool exists);

int64
lion_count_sources_cached(Relation heap, Snapshot snapshot, int nsources,
						 LionCountSource *sources, LionCountStats *stats,
						 LionVisCache *cache, bool rel_read_only)
{
	return lion_count_sources_run(heap, snapshot, nsources, sources, stats,
								  cache, rel_read_only, false);
}

bool
lion_exists_sources_cached(Relation heap, Snapshot snapshot, int nsources,
						  LionCountSource *sources, LionCountStats *stats,
						  LionVisCache *cache, bool rel_read_only)
{
	return lion_count_sources_run(heap, snapshot, nsources, sources, stats,
								  cache, rel_read_only, true) > 0;
}

static int64
lion_count_sources_run(Relation heap, Snapshot snapshot, int nsources,
					   LionCountSource *sources, LionCountStats *stats,
					   LionVisCache *cache, bool rel_read_only, bool exists)
{
	MemoryContext cxt;
	MemoryContext oldcxt;
	LionCountCtx cx;
	LionKeyNode **trees;
	int64		result;
	int			ncarry;
	bool	   *carry;
	bool		summed;
	bool		oneset;
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
	 * The disjoint-sum short-circuit: count each entry's posting set on its own
	 * and add the results up, instead of merging a thousand sub-cursors into
	 * one union.  Nothing else about the count changes - each set goes through
	 * the same single-set machinery below, with the same visibility-map check
	 * per container and the same recheck queue - so DESIGN.md §9 reads exactly
	 * as it does for a one-key count, one set at a time.
	 *
	 * Two questions, and both have to be yes: is the sum the same answer as the
	 * union (lion_sources_disjoint_sum(), which carries the argument) and is it
	 * the cheaper way to get it (lion_sum_is_cheaper(), which is where the
	 * measurements are).
	 *
	 * Or the list was longer than the lookup's pin budget and some of its sets
	 * are NOPIN (DESIGN.md §15).  The sum is then taken however the costs
	 * compare, because it is the one way to count them that keeps the §9
	 * interlock: one set at a time, each located again under a pin of its own
	 * (lion_count_one_set()).  The union would have nothing to carry it - the
	 * list is the only positive source - and could only recheck every TID.
	 */
	summed = (lion_sources_disjoint_sum(nsources, sources) &&
			  (lion_source_has_nopin(&sources[0]) ||
			   lion_sum_is_cheaper(heap, &sources[0])));
	oneset = !summed && lion_sources_one_set(nsources, sources);

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

	/*
	 * A summed source counts every set exactly once, so there is nothing a
	 * private copy could save; skipping the decision also keeps the one
	 * positive source of each pass on the pinned path by construction.
	 */
	for (i = 0; !summed && i < nsources; i++)
	{
		/*
		 * Rule 3 (DESIGN.md §19): a source may forbid it outright.  An OR
		 * across columns does, because a dead TID may be contributed by any
		 * single leaf of the union and the interlock the source carries is
		 * that EVERY leaf holds a pin (lion_source_pinned()); keeping that
		 * property is simpler than reasoning about which other source
		 * happened to carry the interlock at the container key in question.
		 */
		if (sources[i].nomaterialize)
			continue;

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

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"lion index count",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	memset(&cx, 0, sizeof(cx));
	cx.cxt = cxt;
	cx.heap = heap;
	cx.snapshot = snapshot;
	cx.vmbuf = InvalidBuffer;
	/* SerializationNeededForRead() begins with exactly this test; hoisting it
	 * lets non-serializable counts skip the per-block PredicateLockPage loop. */
	cx.serializable = IsolationIsSerializable();
	/*
	 * HOT STANDBY (DESIGN.md §9, §25).
	 *
	 * The §9 interlock is "a reader that holds a pin on the page a container
	 * came from cannot be overtaken by whatever removes that container's dead
	 * TIDs", and on the primary it holds because ambulkdelete takes a CLEANUP
	 * lock on every page it removes a TID from (§11), and a cleanup lock waits
	 * for pins.
	 *
	 * Replay of a GENERIC record takes an ordinary exclusive lock, which does
	 * not wait for pins, so on a standby a reader's pin does not stop
	 * ambulkdelete's records from being replayed and the heap records that
	 * follow can set all-visible while this backend still holds a copy of the
	 * old containers.  A generic-mode index therefore rechecks every candidate
	 * TID in the heap on a standby and never trusts the visibility map.
	 *
	 * Replay of an RMGR-mode record does take the cleanup lock: every record
	 * that removes a TID or deletes an item (VACUUM_PAGE, ITEM_DELETE,
	 * PAGE_DELETED, and the ENTRY record VACUUM deletes entries with) declares
	 * the page it removes them from in its cleanup mask, and lion_redo() takes
	 * that block with XLogReadBufferForRedoExtended(..., get_cleanup_lock =
	 * true).  §9's argument then reads on the standby exactly as it reads on
	 * the primary, with "VACUUM" replaced by "the startup process": while this
	 * backend holds a pin on the page it took a container from, replay cannot
	 * have removed a TID from that page, so it cannot have replayed the heap
	 * record that set any of that container's heap pages all-visible.
	 *
	 * That alone is NOT enough, and the reason is the page-split hole of
	 * §11: the TIDs this backend copied may since have MOVED - a split of the
	 * leaf to its right sibling, a root push-down (§22) to a new child, an
	 * INLINE payload spilled off the directory leaf - and be removed from the
	 * page they moved to, which this backend does not pin.  On the primary
	 * the removal cannot happen before VACUUM has held a cleanup lock on the
	 * page they came from, because VACUUM visits every page (and every page
	 * of a posting tree's descent) in chain order; but a page VACUUM visits
	 * without changing writes no record of its own.  So VACUUM carries those
	 * visits as a BARRIER on its next removal record (or in a VACUUM_VISIT
	 * record of their own), and redo cleanup-locks every page of it, one at a
	 * time with nothing else held, before it applies the removal (§25,
	 * xl_lion_visit in lion_wal.h).  A page is only ever linked in to the
	 * right of the page whose split created it and replay applies records in
	 * the order the primary wrote them, so replay meets the barrier for the
	 * page this backend pins before the removal that could hurt it -
	 * test/recovery/run.sh phase 3 parks a standby reader in exactly that
	 * window for the split, the push-down and the spill.  The reader pays for
	 * it the way a primary reader does: the standby's replay waits, which is
	 * a recovery conflict resolved by max_standby_streaming_delay rather than
	 * a wrong answer.
	 *
	 * ONE generic-mode source is enough to lose it, because a container of the
	 * intersection carries the dead TIDs of every source it came from, and the
	 * interlock has to hold for all of them.  So the map is trusted only when
	 * every index this count reads is in rmgr mode.
	 */
	cx.in_recovery = RecoveryInProgress() &&
		!lion_sources_all_rmgr(nsources, sources);

	/*
	 * NOPIN sets (DESIGN.md §15).  Materializing never takes the last carrier
	 * away (rule 2), so a merge with no positive source that holds a pin at
	 * every container key is one whose sets were located over the pin budget:
	 * an IN list under an OR across columns, say, or intersected only with
	 * sets that are themselves NOPIN.  Nothing then stops VACUUM from having
	 * removed a TID the copies still list and set its heap page all-visible,
	 * so the map is not asked at all and every candidate goes to the heap,
	 * as on a standby.  The sum and the one-set count do not need this: they
	 * locate a NOPIN set again, pinned, before they count it.
	 */
	cx.novm = (ncarry == 0 && !summed && !oneset);
	cx.rel_read_only = rel_read_only;
	cx.tids_sorted = true;
	cx.batchmax = lion_recheck_budget();
	cx.exists = exists;

	/*
	 * The visibility cache, if the caller keeps one for this node execution.
	 * It is emptied here if it holds answers for another relation or another
	 * snapshot, so a partitioned count may hand the same handle to every
	 * partition (DESIGN.md §9 and §16).
	 */
	lion_vis_cache_begin(cache, heap, snapshot);
	cx.cache = cache;

	if (summed)
	{
		/*
		 * One pass per entry, each with the source reduced to that one set.
		 * The passes share the recheck queue, the visibility-map pin and the
		 * visibility cache, so a dirty heap page is still visited once for the
		 * whole list and the batching of DESIGN.md §9 still bounds the memory.
		 */
		MemoryContext setcxt;

		/*
		 * One pass allocates a cursor, a staging buffer for the inline
		 * payload or a whole page image for a chain, and a buffer for the
		 * sparse segment it expands: twelve kilobytes or so, a thousand times
		 * over for the longest list the planner allows.  A context that is
		 * RESET after each pass hands the same twelve kilobytes out again, so
		 * the pass runs in cache instead of walking a dozen megabytes of fresh
		 * memory.  The keeper block is sized to hold all of it, which is what
		 * makes the reset free.
		 */
		setcxt = AllocSetContextCreate(cxt, "lion index count entry",
									   32 * 1024, 32 * 1024,
									   ALLOCSET_DEFAULT_MAXSIZE);

		for (j = 0; j < sources[0].nsets; j++)
		{
			if (!sources[0].sets[j].found)
				continue;

			MemoryContextSwitchTo(setcxt);
			lion_count_one_set(&cx, &sources[0].sets[j]);
			MemoryContextSwitchTo(cxt);
			MemoryContextReset(setcxt);

			cx.stats.sets_summed++;

			/* An existence test needs one entry with a visible row (§26). */
			if (lion_exists_settled(&cx))
				break;
			CHECK_FOR_INTERRUPTS();
		}
		MemoryContextDelete(setcxt);
	}
	else if (oneset)
		lion_count_one_set(&cx, &sources[0].sets[0]);
	else
		lion_run_merge(&cx, nsources, sources, trees);

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
		stats->probes_avoided += cx.stats.probes_avoided;
		stats->cache_hits += cx.stats.cache_hits;
		stats->cache_full += cx.stats.cache_full;
		stats->sets_summed += cx.stats.sets_summed;
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
		lion_posting_set_lookup_col(indexes[i], 1, keys[i],
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
lion_entry_scan_begin_col(LionEntryScan *es, Relation index, AttrNumber attno)
{
	es->index = index;
	es->state = lion_index_column_state(index, attno);
	es->attno = es->state->attno;
	es->cxt = AllocSetContextCreate(CurrentMemoryContext,
									"lion entry scan position",
									ALLOCSET_SMALL_SIZES);

	/*
	 * The walk starts at (attno, MINF), a position below every entry of this
	 * column and above every entry of the columns before it, and ends at the
	 * first entry whose attno is not this one (DESIGN.md §24).  MINF is not a
	 * kind any stored entry has, so "resume after the last key returned" -
	 * which is what every later call does - starts at the column's first
	 * entry without a special case.
	 */
	es->lastkind = LION_KIND_MINF;
	es->lasthash = 0;
	es->lastkeylen = 0;
	es->lastkey = (char *) MemoryContextAllocZero(es->cxt, 1);
	es->haslast = true;
	es->blkno = lion_dir_column_first(index, es->state, NULL);
	es->onpage = 0;
	es->done = false;
}

/* Remember where to resume, as a KEY (see the comment on LionEntryScan). */
static void
lion_entry_scan_remember(LionEntryScan *es, const LionEntryTuple *entry)
{
	MemoryContextReset(es->cxt);
	es->lastkind = lion_entry_kind(entry);
	es->lasthash = entry->hash;
	es->lastkeylen = entry->keylen;

	/*
	 * Always a real pointer, even for the key-less reserved entries: a search
	 * key whose `raw` is NULL compares as the SMALLEST member of its own run
	 * (lion_cmp_entry()), and "resume after the last key" would then resume AT
	 * it and hand the same entry out for ever.
	 */
	es->lastkey = (char *) MemoryContextAlloc(es->cxt,
											  Max((Size) entry->keylen, 1));
	if (entry->keylen > 0)
		memcpy(es->lastkey, LionEntryGetKey(entry), entry->keylen);
	es->haslast = true;
}

/*
 * Fetch the next entry of the index, in directory order.
 *
 * The scan gives up its lock on a leaf between calls and comes back to the
 * first key ABOVE the last one it returned, which is what makes it safe
 * against everything a sorted directory does to offsets: an insert in the
 * middle of a leaf shifts them, a split moves the upper half to a page
 * further right, and VACUUM deletes entries outright (DESIGN.md §18, §21).
 *
 * Nothing is therefore skipped or returned twice.  A split moves entries only
 * rightwards onto a page this walk has not passed, and their keys are still
 * above the last one returned, so they come out exactly once.  A deleted
 * entry is simply gone, and only an EMPTY posting set is ever deleted, so its
 * group had nothing this scan's snapshot could have counted.  An entry
 * INSERTED behind the walk is missed, which is the same freedom the bucket
 * walk had: it can only hold TIDs no older snapshot can see.
 */
bool
lion_entry_scan_next(LionEntryScan *es, Datum *key, LionPostingSet *ps)
{
	LionState  *state = es->state;

	while (!es->done && BlockNumberIsValid(es->blkno))
	{
		Buffer		buf;
		Page		page;
		OffsetNumber maxoff;
		OffsetNumber off;
		BlockNumber next;
		bool		got = false;
		LionSearchKey sk;

		/*
		 * Test hook: the scan is between two entries of one leaf and holds no
		 * lock on it at all, so a concurrent VACUUM is free to delete entries
		 * and a concurrent insert to split the page.  It fires once per leaf,
		 * which is what lets an isolation test park a GROUP BY here exactly
		 * once; test/isolation/vacuum_entry_delete.spec and
		 * test/isolation/dir_split_scan.spec are those two cases.  Compiles to
		 * nothing without --enable-injection-points.
		 */
		if (es->onpage == 1)
			LION_INJECTION_POINT("lion-entry-scan-resumed");

		buf = ReadBuffer(es->index, es->blkno);
		lion_dir_pages_read++;
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (!LionPageIsLeaf(page))
		{
			UnlockReleaseBuffer(buf);
			elog(ERROR, "lion index: block %u is not a directory leaf",
				 es->blkno);
		}

		if (es->haslast)
		{
			sk.attno = es->attno;
			sk.col = state;
			sk.kind = es->lastkind;
			sk.key = (es->lastkind == LION_KIND_VALUE) ?
				lion_fetch_key(state, es->lastkey) : (Datum) 0;
			sk.hash = es->lasthash;
			sk.cmpproc = state->ordered ? &state->cmpproc : NULL;
			sk.eqproc = &state->eqproc;
			sk.collation = state->collation;
			sk.raw = es->lastkey;
			sk.rawlen = es->lastkeylen;

			/*
			 * Everything on this page may already be behind us, which is what
			 * a split of the page we were on looks like from here.
			 */
			if (!LionPageIsRightmost(page) &&
				lion_cmp_entry(lion_dir_highkey(page), &sk) <= 0)
			{
				next = LionPageGetOpaque(page)->rightlink;
				UnlockReleaseBuffer(buf);
				es->blkno = next;
				es->onpage = 0;
				CHECK_FOR_INTERRUPTS();
				continue;
			}

			off = lion_dir_binsrch(page, &sk);
			while (off <= PageGetMaxOffsetNumber(page) &&
				   lion_cmp_entry(lion_page_entry(page, off), &sk) <= 0)
				off = OffsetNumberNext(off);
		}
		else
			off = lion_page_first_data(page);

		maxoff = PageGetMaxOffsetNumber(page);

		for (; off <= maxoff; off++)
		{
			ItemId		iid = PageGetItemId(page, off);
			LionEntryTuple *entry;
			bool		keeppin;

			if (!ItemIdIsUsed(iid))
				continue;
			entry = (LionEntryTuple *) PageGetItem(page, iid);

			/*
			 * The walk is bounded to one key column (DESIGN.md §24): the
			 * entries are sorted by attno, so the first entry of the next
			 * column ends the scan.
			 */
			if (entry->attno != es->attno)
			{
				UnlockReleaseBuffer(buf);
				es->done = true;
				return false;
			}

			/*
			 * An entry whose posting set is empty can never produce a group.
			 * VACUUM deletes those (DESIGN.md §18), but one can be seen here
			 * between the moment its last TID was filtered out and the moment
			 * the leaf's final step removes it.
			 */
			if (entry->ntids == 0)
			{
				lion_entry_scan_remember(es, entry);
				continue;
			}

			lion_fill_posting_set(es->index, state, buf, off, ps, &keeppin);
			*key = ps->storedkey;
			if (keeppin)
			{
				/*
				 * DESIGN.md section 9: the INLINE payload we just copied out
				 * needs a pin of its own on this leaf, independent of the
				 * scan's position.
				 */
				IncrBufferRefCount(buf);
				ps->pinbuf = buf;
			}

			lion_entry_scan_remember(es, entry);
			es->onpage++;
			got = true;
			break;
		}

		next = LionPageGetOpaque(page)->rightlink;
		UnlockReleaseBuffer(buf);

		if (got)
			return true;

		es->blkno = next;
		es->onpage = 0;
		CHECK_FOR_INTERRUPTS();
	}

	es->done = true;
	return false;
}

void
lion_entry_scan_end(LionEntryScan *es)
{
	es->done = true;
	if (es->cxt != NULL)
	{
		MemoryContextDelete(es->cxt);
		es->cxt = NULL;
	}
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
 * An index's expressions or predicate AS STORED in pg_index, NIL when it has
 * none.  RelationGetIndexExpressions()/RelationGetIndexPredicate() hand back
 * the planner's simplified form, in which an inlinable SQL function has
 * already been replaced by its body - which is right for evaluating it and
 * wrong for asking which functions the query calls.
 */
static List *
lion_index_stored_exprs(Relation index, int attnum)
{
	HeapTuple	tup;
	Datum		d;
	bool		isnull;
	List	   *result = NIL;

	tup = SearchSysCache1(INDEXRELID,
						  ObjectIdGetDatum(RelationGetRelid(index)));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for index %u",
			 RelationGetRelid(index));
	d = SysCacheGetAttr(INDEXRELID, tup, attnum, &isnull);
	if (!isnull)
	{
		void	   *node = stringToNode(TextDatumGetCString(d));

		result = (attnum == Anum_pg_index_indpred) ?
			make_ands_implicit((Expr *) node) : (List *) node;
	}
	ReleaseSysCache(tup);

	return result;
}

/*
 * EXECUTE on a function the query a count stands for would call, asked as
 * ExecInitFunc() asks it: of the current user, failing with core's own
 * "permission denied for function", then the object-access hook core fires
 * for every function it is about to run (DESIGN.md §9, "Privileges").  The
 * pushdown asks at executor startup and the SQL functions when called, never
 * at plan time: a cached plan outlives both a REVOKE and a SET ROLE.
 */
void
lion_check_execute(Oid funcid)
{
	AclResult	aclresult;

	aclresult = object_aclcheck(ProcedureRelationId, funcid, GetUserId(),
								ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION, get_func_name(funcid));
	InvokeFunctionExecuteHook(funcid);
}

/*
 * The same for an aggregate, as ExecInitAgg() asks it: EXECUTE on the
 * aggregate for the current user ("permission denied for aggregate"), then
 * EXECUTE on its final and transition functions for the aggregate's OWNER.
 * count()'s are the bootstrap superuser's, so the second half cannot fail for
 * it; it is here so that the node asks what the Agg it replaces asks, hooks
 * included.
 */
void
lion_check_aggregate_execute(Oid aggfnoid)
{
	AclResult	aclresult;
	HeapTuple	tup;
	Oid			transfn;
	Oid			finalfn;
	Oid			owner;

	aclresult = object_aclcheck(ProcedureRelationId, aggfnoid, GetUserId(),
								ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_AGGREGATE, get_func_name(aggfnoid));
	InvokeFunctionExecuteHook(aggfnoid);

	tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(aggfnoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for aggregate %u", aggfnoid);
	transfn = ((Form_pg_aggregate) GETSTRUCT(tup))->aggtransfn;
	finalfn = ((Form_pg_aggregate) GETSTRUCT(tup))->aggfinalfn;
	ReleaseSysCache(tup);

	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(aggfnoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for function %u", aggfnoid);
	owner = ((Form_pg_proc) GETSTRUCT(tup))->proowner;
	ReleaseSysCache(tup);

	if (OidIsValid(finalfn))
	{
		aclresult = object_aclcheck(ProcedureRelationId, finalfn, owner,
									ACL_EXECUTE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_FUNCTION, get_func_name(finalfn));
		InvokeFunctionExecuteHook(finalfn);
	}
	aclresult = object_aclcheck(ProcedureRelationId, transfn, owner,
								ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION, get_func_name(transfn));
	InvokeFunctionExecuteHook(transfn);
}

/*
 * EXECUTE on every function an index expression or predicate calls
 * (lion_count_open_indexes()): the same question the executor asks of the
 * query the count stands for, in the same walk core uses to find the
 * functions of an expression.
 */
static bool
lion_check_function_acl(Oid funcid, void *context)
{
	lion_check_execute(funcid);
	return false;
}

static bool
lion_check_functions_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	/* the stored form has not been through the planner's fix_opfuncids() */
	if (IsA(node, OpExpr) || IsA(node, DistinctExpr) || IsA(node, NullIfExpr))
		set_opfuncid((OpExpr *) node);
	else if (IsA(node, ScalarArrayOpExpr))
		set_sa_opfuncid((ScalarArrayOpExpr *) node);
	(void) check_functions_in_node(node, lion_check_function_acl, context);
	return expression_tree_walker(node, lion_check_functions_walker, context);
}

/*
 * The error for a relation named by an OID that has no relation behind it:
 * one dropped since the caller named it, or one that never existed (a
 * regclass argument accepts any number).
 */
static void
lion_count_no_relation(Oid relid)
{
	ereport(ERROR,
			(errcode(ERRCODE_UNDEFINED_TABLE),
			 errmsg("relation with OID %u does not exist", relid)));
}

/*
 * Open and vet the indexes of one SQL count: relkind, access method, key
 * type, privileges, row-level security and snapshot eligibility.  keytype may
 * be NULL, which means the caller has no search key at all (the grouped form
 * below, which walks every entry instead of looking one up).
 */
static void
lion_count_open_indexes(Snapshot snapshot, int nidx, const Oid *idxoid,
					   const Oid *keytype, AttrNumber wantcol,
					   LionCountCall *call)
{
	Oid			heapoid = InvalidOid;
	char	   *heapname;
	int			i;

	/* index[] and keytype[] hold two; every caller opens one or two */
	Assert(nidx >= 1 && nidx <= (int) lengthof(call->index));

	call->nkeys = nidx;
	call->heap = NULL;
	for (i = 0; i < 2; i++)
	{
		call->index[i] = NULL;
		call->keytype[i] = InvalidOid;
	}

	/*
	 * Nothing is locked yet, so every catalog answer below may be about a
	 * relation that is being dropped: a relation that is gone is reported by
	 * its OID, never as "(null)" or as a failed cache lookup.
	 */
	for (i = 0; i < nidx; i++)
	{
		char	   *idxname = get_rel_name(idxoid[i]);
		char		relkind = get_rel_relkind(idxoid[i]);
		Oid			hoid;

		if (keytype != NULL)
			call->keytype[i] = keytype[i];

		if (idxname == NULL || relkind == '\0')
			lion_count_no_relation(idxoid[i]);
		if (relkind != RELKIND_INDEX)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is not an index", idxname)));

		hoid = IndexGetRelation(idxoid[i], true);
		if (!OidIsValid(hoid))
			lion_count_no_relation(idxoid[i]);
		if (i == 0)
			heapoid = hoid;
		else if (hoid != heapoid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("indexes \"%s\" and \"%s\" are not on the same table",
							get_rel_name(idxoid[0]), idxname)));
	}

	/*
	 * The cheap half of the privilege check, BEFORE any lock (2026-09-23
	 * review): the caller must hold SELECT on the table or on at least one of
	 * its columns, which is the least any count through any of its indexes
	 * needs.  Without it a role with no privilege at all could take - or
	 * queue for - a lock on any table that has a lion index, and hold up
	 * everything that queues behind it.  The exact check, which reads the
	 * index definition, follows once the locks are held, because only then
	 * can that definition be trusted.  A table dropped meanwhile makes
	 * pg_class_aclcheck() raise "does not exist".
	 */
	heapname = get_rel_name(heapoid);
	if (heapname == NULL)
		lion_count_no_relation(heapoid);
	if (pg_class_aclcheck(heapoid, GetUserId(), ACL_SELECT) != ACLCHECK_OK &&
		pg_attribute_aclcheck_all(heapoid, GetUserId(), ACL_SELECT,
								  ACLMASK_ANY) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV,
					   get_relkind_objtype(get_rel_relkind(heapoid)),
					   heapname);

	call->heap = try_table_open(heapoid, AccessShareLock);
	if (call->heap == NULL)
		lion_count_no_relation(heapoid);
	lion_check_table_am(call->heap);	/* the visibility map is the heap's */

	for (i = 0; i < nidx; i++)
	{
		Relation	index = try_index_open(idxoid[i], AccessShareLock);

		if (index == NULL)
			lion_count_no_relation(idxoid[i]);
		call->index[i] = index;

		if (index->rd_rel->relam != lion_get_am_oid())
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("index \"%s\" is not a lion index",
							RelationGetRelationName(index))));
		/*
		 * wantcol = 0 means the caller names an index and a key but no COLUMN,
		 * so a multicolumn index (DESIGN.md §24) has nothing to tell it which
		 * key set is meant.  The planner's pushdown has the column from the
		 * clause and is not restricted this way.
		 */
		if (wantcol == 0)
		{
			if (IndexRelationGetNumberOfKeyAttributes(index) != 1)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("lion index \"%s\" has %d key columns, and this function needs exactly one",
								RelationGetRelationName(index),
								IndexRelationGetNumberOfKeyAttributes(index))));
		}
		else if (wantcol < 1 ||
				 wantcol > IndexRelationGetNumberOfKeyAttributes(index))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("lion index \"%s\" has no key column %d",
							RelationGetRelationName(index), wantcol)));

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

		/*
		 * A multi-key opclass (DESIGN.md §17) stores one entry per extracted
		 * key, so its entries are not column values: a search key of the
		 * column's own type - a whole tsvector - is not what any entry holds,
		 * and hashing and comparing it as if it were answered a meaningless
		 * count; and a row appears under several entries, so the sum over
		 * them is not a row count and no single entry is a group either.  The
		 * keyed functions and the grouped one refuse such a column alike;
		 * lion_customscan.c refuses to drive a GROUP BY from one for the same
		 * reason, and answers `@>` or `@@` through lion_extract_query().
		 */
		{
			AttrNumber	col = (wantcol == 0) ? 1 : wantcol;

			if (lion_index_column_state(index, col)->multikey)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("key column %d of index \"%s\" has a multi-key operator class, whose entries are not column values",
								col, RelationGetRelationName(index))));
		}
	}

	/*
	 * Privileges: exactly what the equivalent query needs.  SELECT count(*)
	 * FROM t WHERE col = key references col, so SELECT on the table or on
	 * every column the index reads is required; with less than that the
	 * count would let a caller probe values it is not allowed to read.
	 *
	 * "Every column the index reads" is more than its key columns.  The count
	 * of a PARTIAL index is the count of the rows that satisfy its predicate,
	 * so the query it stands for is `... WHERE pred AND col = key` and reads
	 * the predicate's columns too - with SELECT(id) alone, a partial index
	 * `(id) WHERE secret` answers 1 or 0 and so reveals `secret` a row at a
	 * time (2026-09-23 review).  An expression column reads whatever its
	 * expression does, and a whole-row reference reads every column, which
	 * only a table-level grant covers.  An index that reads NO column at all
	 * - one on a constant - stands for `SELECT count(*) FROM t`, which needs
	 * SELECT on the table or on at least one of its columns, and the same
	 * rule applies here.
	 *
	 * And the query CALLS every function in the index's expressions and
	 * predicate, which needs EXECUTE on each of them whatever the table
	 * grants say; so does the count, or it would let a caller probe the
	 * results of a function it may not run.
	 */
	{
		bool		tablesel = pg_class_aclcheck(heapoid, GetUserId(),
												 ACL_SELECT) == ACLCHECK_OK;

		for (i = 0; i < nidx; i++)
		{
			Relation	index = call->index[i];
			List	   *exprs = lion_index_stored_exprs(index, Anum_pg_index_indexprs);
			List	   *pred = lion_index_stored_exprs(index, Anum_pg_index_indpred);
			Bitmapset  *cols = NULL;
			int			c;
			int			m;

			(void) lion_check_functions_walker((Node *) exprs, NULL);
			(void) lion_check_functions_walker((Node *) pred, NULL);

			if (tablesel)
				continue;

			/* EVERY key column is referenced, not just the first (§24). */
			for (c = 0; c < IndexRelationGetNumberOfKeyAttributes(index); c++)
			{
				AttrNumber	attnum = index->rd_index->indkey.values[c];

				if (attnum != 0)
					cols = bms_add_member(cols,
										  attnum - FirstLowInvalidHeapAttributeNumber);
			}
			pull_varattnos((Node *) exprs, 1, &cols);
			pull_varattnos((Node *) pred, 1, &cols);

			if (bms_is_empty(cols))
			{
				if (pg_attribute_aclcheck_all(heapoid, GetUserId(), ACL_SELECT,
											  ACLMASK_ANY) != ACLCHECK_OK)
					aclcheck_error(ACLCHECK_NO_PRIV,
								   get_relkind_objtype(call->heap->rd_rel->relkind),
								   RelationGetRelationName(call->heap));
				continue;
			}

			m = -1;
			while ((m = bms_next_member(cols, m)) >= 0)
			{
				AttrNumber	attnum = m + FirstLowInvalidHeapAttributeNumber;

				if (attnum == InvalidAttrNumber ||
					pg_attribute_aclcheck(heapoid, attnum, GetUserId(),
										  ACL_SELECT) != ACLCHECK_OK)
					aclcheck_error(ACLCHECK_NO_PRIV,
								   get_relkind_objtype(call->heap->rd_rel->relkind),
								   RelationGetRelationName(call->heap));
			}
		}
	}

	/*
	 * The query also CALLS count() and the equality the key is looked up
	 * with, so the count asks for EXECUTE on both, as the executor would of
	 * that query (2026-09-23 review).  `col = key` is strategy 1 of the key
	 * column's opfamily for (opcintype, the key's type) - int48eq for an int8
	 * key on an int4 column, exactly as in the query - and `col = ANY (keys)`
	 * calls the same function per element.  The grouped form stands for
	 * `SELECT col, count(*) ... GROUP BY col`, whose Agg compares groups with
	 * the type's equality; the pushdown only groups by an index whose
	 * strategy 1 IS that equality (DESIGN.md §10), so strategy 1 for
	 * (opcintype, opcintype) is the function there.
	 */
	for (i = 0; i < nidx; i++)
	{
		Relation	index = call->index[i];
		AttrNumber	col = (wantcol == 0) ? 1 : wantcol;
		Oid			opfamily = index->rd_opfamily[col - 1];
		Oid			opcintype = index->rd_opcintype[col - 1];
		Oid			eqop = InvalidOid;

		if (OidIsValid(call->keytype[i]))
			eqop = get_opfamily_member(opfamily, opcintype,
									   call->keytype[i], 1);
		if (!OidIsValid(eqop))
			eqop = get_opfamily_member(opfamily, opcintype, opcintype, 1);
		if (!OidIsValid(eqop))
			elog(ERROR, "missing equality operator for type %u in opfamily %u",
				 opcintype, opfamily);
		lion_check_execute(get_opcode(eqop));
	}
	lion_check_aggregate_execute(F_COUNT_);

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

	lion_count_open_indexes(snapshot, nkeys, idxoid, keytype, 0, call);

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
 * k-way merge of lion_ecursor_build() or the disjoint sum.  Unlike the
 * pushdown's literal lists, this has no limit on the number of values: the
 * pins the lookup keeps are budgeted instead (DESIGN.md §15), and the sets
 * past the budget are counted one at a time.
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

	lion_count_open_indexes(snapshot, 1, &idxoid, &elemtype, 0, &call);
	PredicateLockRelation(call.index[0], snapshot);

	get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
	deconstruct_array(arr, elemtype, elmlen, elmbyval, elmalign,
					  &elems, &nulls, &nelems);

	sets = (LionPostingSet *) palloc0(sizeof(LionPostingSet) * Max(nelems, 1));
	nsets = lion_posting_set_lookup_many_col(call.index[0], 1, elemtype,
											nelems, elems, nulls, sets,
											&nfound);

	/* An empty array, an all-NULL one, or no listed value with an entry. */
	if (nfound > 0)
	{
		memset(&src, 0, sizeof(src));
		src.nsets = nsets;
		src.sets = sets;

		/*
		 * lion_posting_set_lookup_many() dropped duplicate ENTRIES, so the sets
		 * are distinct entries of one index and their union is their sum
		 * whenever the opclass is scalar: the disjoint-sum short-circuit of
		 * DESIGN.md §15, which lion_count_sources() takes from here.
		 */
		src.disjoint = true;
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
	AttrNumber	attno = (AttrNumber) PG_GETARG_INT16(2);
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

	/*
	 * Column 0 is no column, and it is refused before anything else happens.
	 * lion_count_open_indexes() takes wantcol = 0 to mean "the caller names
	 * none", which a one-column index satisfies, so attno = 0 used to go on
	 * to read the operator class of column -1 (2026-09-23 review).
	 */
	if (attno < 1)
	{
		char	   *idxname = get_rel_name(idxoid);

		if (idxname == NULL)
			lion_count_no_relation(idxoid);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("lion index \"%s\" has no key column %d",
						idxname, attno)));
	}

	lion_count_open_indexes(snapshot, 1, &idxoid, NULL, attno, &call);

	PredicateLockRelation(call.index[0], snapshot);

	memset(&stats, 0, sizeof(stats));
	/* use_cache = false is the pre-cache behaviour, for an A/B in one query */
	cache = usecache ? lion_vis_cache_create(CurrentMemoryContext) : NULL;
	percxt = AllocSetContextCreate(CurrentMemoryContext,
								   "lion index group count",
								   ALLOCSET_SMALL_SIZES);

	lion_entry_scan_begin_col(&es, call.index[0], attno);

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
									 cache, false);
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
