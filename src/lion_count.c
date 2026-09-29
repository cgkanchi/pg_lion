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
#include "catalog/pg_collation.h"
#include "catalog/pg_index.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/tablespace.h"
#include "common/hashfn.h"
#include "executor/nodeHash.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_coerce.h"
#include "parser/parse_oper.h"
#include "port/atomics.h"
#include "port/pg_bitutils.h"
#include "storage/barrier.h"
#include "storage/buffile.h"
#include "storage/bufmgr.h"
#include "storage/dsm.h"
#include "storage/predicate.h"
#include "storage/sharedfileset.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/dsa.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/resowner.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"
#include "utils/spccache.h"
#include "utils/syscache.h"
#include "utils/wait_event.h"

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
 * LION_MATERIALIZE_MAX_CONTAINERS and LION_MATERIALIZE_MAX_BYTES (lion_count.h,
 * because the cost model needs them too): a posting set is worth
 * materializing (DESIGN.md section 9 and the comment on
 * lion_posting_set_materialize()) when it is this small.  Either bound is
 * enough: 64 containers cover 4096 heap blocks however fat they are, and a
 * set of many thin containers is cheap to keep as long as it stays under the
 * byte budget.  A set that fails both bounds keeps being walked page by page.
 */

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
	bool		droppins;		/* the caller needs no interlock at all: every
								 * cursor lets go of a posting leaf as soon as
								 * it has copied it (a plain index scan under
								 * an MVCC snapshot, DESIGN.md §29.5) */
	bool		farseeks;		/* every seek is to a key many leaves past the
								 * last: descend at once (lion_stream_far()) */
	bool		raw;			/* the merge's intermediate containers are
								 * left unoptimized: every one is counted or
								 * handed to the next operation, never kept
								 * (DESIGN.md §15, "Unions and intersections
								 * unoptimized").  A stream's are optimized:
								 * its callers copy them. */
	LionVisCache *cache;			/* per-query visibility cache, or NULL */

	/*
	 * A test every counted row must pass as well (DESIGN.md §17, "A query
	 * known only at run time"), or NULL.  With one, the visibility map is not
	 * asked at all: every candidate goes to the heap, where the row the
	 * snapshot sees is tested (lion_recheck_heap_filtered()).
	 */
	LionRowFilter *filter;
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

	/*
	 * Neither: the containers of the intersection are COPIED instead of
	 * counted, and nothing is asked of the visibility map or the heap
	 * (lion_sources_collect(), DESIGN.md §27).  NULL for a count.
	 */
	struct LionCollect *collect;

	/*
	 * The one set of source slot 0 - a group's, a join key's fk set - whose
	 * containers stats.key_containers counts (DESIGN.md §27, "Where a key's
	 * time goes"), or NULL.
	 */
	const LionPostingSet *keyset;
} LionCountCtx;

/*
 * A collected set too large for its memory, written to a temporary file
 * instead (DESIGN.md §27, "The fact filters, collected once", and §32, "A
 * range as a source"): the containers in ascending key order, back to back,
 * and in memory only where each one is and its key - what a cursor reads
 * one container at a time and a seek searches (lion_cursor_next_item(),
 * lion_cursor_seek()): sixteen bytes a container key of the heap, however
 * full the container.
 */
typedef struct LionSpillEnt
{
	pgoff_t		off;			/* in segment `fileno` of the file */
	uint32		ckey;
	uint16		size;			/* at most LION_CONTAINER_MAX_SIZE */
	int16		fileno;
} LionSpillEnt;

typedef struct LionSpill
{
	MemoryContext cxt;			/* where the entries and the file live */
	BufFile    *file;
	LionSpillEnt *ents;
	int			nents;
	int			cap;

	/*
	 * A chunk of a copy shared by a parallel plan's participants
	 * (LionSharedCopy) spills to a file of the plan's file set, named, which
	 * the others open to read; any other spill to a temporary file of the
	 * backend's own.
	 */
	FileSet    *fileset;
	const char *name;
} LionSpill;

/*
 * The containers an intersection yields, copied out as the merge produces
 * them: in ascending container key order, each MAXALIGNed in buf so that a
 * BITSET keeps its uint64 alignment - the layout of a LionMatSet, which is
 * what lion_sources_collect() turns this into.  failed says the copy has
 * outgrown maxbytes; the merge stops at the next container boundary.  Unless
 * the caller allowed it to SPILL: the copy then goes on in a temporary file
 * (spilled), and what buf held so far is moved there first.
 */
typedef struct LionCollect
{
	MemoryContext cxt;			/* where the copy lives: the caller's */
	Size		maxbytes;
	char	   *buf;
	Size		used;
	Size		cap;
	Size	   *offs;
	int			noffs;
	int			offcap;
	uint64		members;
	bool		failed;
	bool		spill;			/* past maxbytes, a file rather than failed */
	bool		spilled;
	LionSpill	sp;

	/*
	 * A chunk of the intersection only (ranged): its container keys from lo
	 * up to, not including, hi - what one participant of a parallel plan
	 * collects of a shared copy (LionSharedCopy), or of the WHERE of a
	 * parallel GROUP BY for one range of keys (lion_sources_collect_range()).
	 */
	bool		ranged;
	uint32		lo;
	uint64		hi;
} LionCollect;

/*
 * What the cursors of ONE expression node may hold open at once (DESIGN.md
 * §15, "Bounded cursors"): memory and buffer pins.  lion_open_budget_init()
 * sets it from work_mem and from the list pin budget; lion_plan_node() is
 * what obeys it.
 */
typedef struct LionOpenBudget
{
	Size		mem;			/* bytes of cursors, staging buffers, images */
	int			pins;			/* posting pages pinned by cursors */
} LionOpenBudget;

/*
 * Floors of the budget.  work_mem may be as small as 64 kB and the list pin
 * budget may be spent entirely by the lists' own INLINE leaves; a batch of
 * a few dozen sets still has to fit, or a list would be counted a set or two
 * at a time.  LION_BATCH_MIN_SETS is the fewest found sets one batch of a
 * disjoint list takes, and it fits both floors - which is what keeps a batch
 * from ever being planned as a windowed union.
 */
#define LION_OPEN_MIN_BYTES		(256 * 1024)
#define LION_OPEN_MIN_PINS		16
#define LION_BATCH_MIN_SETS		16

StaticAssertDecl(LION_BATCH_MIN_SETS <= LION_OPEN_MIN_PINS,
				 "pg_lion: a minimal batch must fit the pin floor");

/*
 * A posting set's containers copied out of the index, private to the backend
 * and holding no pin.  Containers are MAXALIGNed inside buf so that a BITSET
 * payload keeps its uint64 alignment, and are in ascending ckey order, which
 * is what the merge in lion_count_posting_sets() requires.  A collected set
 * may keep them elsewhere: containers[] then points at wherever they are
 * (a range's union, lion_range_collect()), or - SPILLED - containers is NULL
 * and they are in `file`, described by spill[] (LionSpillEnt), which the set
 * owns and lion_posting_set_release() closes.  A spilled intersection
 * (lion_sources_collect()) keeps its FIRST container in memory as well
 * (first): every cursor over it reads that one when it is built, and the
 * FK-side join's copy of its fact filters (DESIGN.md §27) and a GROUP BY's of
 * its WHERE items (§10) get a cursor per count, which read it back from the
 * file each time.
 *
 * A collected copy is also INDEXED by its container keys (lion_mat_index(),
 * DESIGN.md §27, "The copy, looked up by key"), because every count against
 * it seeks it at each of its key's containers: keys[] holds them in order,
 * four bytes a container searched without touching the containers, and where
 * they are dense, dir[] answers a seek in one read - dir[k - dirbase] is the
 * first container whose key is at or above k, for every k from the first
 * container's key to the last's.
 */
typedef struct LionMatSet
{
	int			ncontainers;
	Size		bytes;
	Size		held;			/* what the copy takes from its context */
	char	   *buf;
	LionContainer **containers;
	BufFile    *file;			/* spilled: the containers, or NULL */
	LionSpillEnt *spill;		/* spilled: where each one is */
	LionContainer *first;		/* spilled: container 0, or NULL */
	uint32	   *keys;			/* the containers' keys, or NULL */
	uint32	   *dir;			/* the direct index, or NULL */
	uint32		dirbase;		/* the key dir[0] stands for */
	uint32		dirlen;			/* keys dir[] covers */
	bool		collected;		/* every item a container, one a key: an
								 * intersection lion_sources_collect() made */

	/*
	 * A participant's view of a copy in dynamic shared memory
	 * (LionSharedCopy): containers[i] points into that memory, or is NULL
	 * where the container went to a file, which spill[i] then locates in
	 * files[c], its chunk's file of the plan's file set, opened read-only;
	 * chunkfirst[c] is the first container of chunk c, and chunkfirst[nchunks]
	 * the containers.  keys[], dir[] and spill[] are in the shared memory
	 * too.  NULL and 0 for any other copy.
	 */
	int			nchunks;
	const int  *chunkfirst;
	BufFile   **files;
} LionMatSet;

/* Is container i of a copy in memory - where containers[i] points? */
static inline bool
lion_mat_inmem(const LionMatSet *mat, int i)
{
	return mat->containers != NULL && mat->containers[i] != NULL;
}

/* Does a copy keep any of its containers in a file? */
static inline bool
lion_mat_spills(const LionMatSet *mat)
{
	return mat->file != NULL || mat->files != NULL;
}

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

	/*
	 * INLINE sets: offset into the payload copy, and the aligned buffer an
	 * item is copied into (payloads are packed).  The buffer is sized for the
	 * PAYLOAD, not for the largest item there is: lion_inline_fetch() never
	 * copies more than is left of the payload, so an entry of three rows
	 * needs a few dozen bytes where LION_CONTAINER_MAX_SIZE - 4104 bytes, which
	 * the allocator rounds up to 8 kB - was a whole page per value of an IN
	 * list (lion_inline_stage_size()).
	 */
	Size		payoff;
	LionContainer *cbuf;

	/* materialized sets: index into set->mat->containers */
	int			matidx;

	/* CHAIN sets: the page image being consumed */
	BlockNumber nextblk;
	OffsetNumber off;
	OffsetNumber maxoff;
	PGAlignedBlock *imgbuf;		/* private copy of the current container page */
	Page		img;
	BlockNumber imgblk;			/* the block img is a copy of */
	bool		haspage;		/* img holds a leaf whose items are being
								 * consumed; pinbuf pins it unless the cursor
								 * was told to drop its pins (cx->droppins) */

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

	/*
	 * The sparse segment being expanded, and how far into its pairs.  segbuf
	 * receives one container key's members at a time, which a well-formed
	 * segment keeps below LION_SPARSE_THRESHOLD; it is sized for that and
	 * grown only if a segment ever holds more (segcap is its capacity).
	 */
	const LionContainer *seg;
	uint32		segpos;
	LionContainer *segbuf;
	Size		segcap;

	/*
	 * Pin on the source page of the current container.  For an INLINE set
	 * this is the LionPostingSet's own bucket-page pin, which the cursor
	 * borrows and must not release (ownpin is false).
	 *
	 * droppins: this cursor carries no DESIGN.md §9 interlock for anyone, so
	 * it lets go of every posting leaf as soon as it has copied it - because
	 * the whole walk needs none (cx->droppins), or because the expression
	 * above it has decided that another cursor carries the interlock or that
	 * none can (the plan of lion_plan_node(): an AND past the pin budget, a
	 * windowed union).  A pin that carries no interlock only makes VACUUM
	 * wait and uses up a buffer.
	 */
	Buffer		pinbuf;
	bool		ownpin;
	bool		droppins;

	LionCountCtx *cx;			/* for statistics */
} LionSetCursor;

static void lion_cursor_next(LionSetCursor *cur);
static void lion_cursor_seek(LionSetCursor *cur, uint32 target);
static void lion_recheck_flush(LionCountCtx *cx);
static void lion_count_container_masks(LionCountCtx *cx, const LionContainer *c,
									   uint64 members, uint64 allvis);
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
 * Is a value of type `typid` one of this key column's own values?  Asked of a
 * class declared on a POLYMORPHIC type (enum_ops is FOR TYPE anyenum), whose
 * input type says nothing about which enum the column holds: the key type,
 * state->typid, does - the column's type as the index's tuple descriptor has
 * it (DESIGN.md §17's key type resolution).  Domains are looked through on
 * both sides, as the parser does: a domain over the enum has the enum's
 * representation, and the index's column may itself be of the domain.
 */
static bool
lion_type_is_column(LionState *state, Oid typid)
{
	return OidIsValid(typid) &&
		getBaseType(typid) == getBaseType(state->typid);
}

/*
 * The type to name when a value cannot be compared with a key column: the
 * class's input type, or the column's actual type where that is polymorphic
 * ("the index is on type anyenum" says nothing about which one).  A multi-key
 * column's key type is the ELEMENT type (array_ops on text[] stores text), not
 * what the column holds, so there the class's type is named as before.
 */
static Oid
lion_column_type(LionState *state, Oid opcintype)
{
	return (IsPolymorphicType(opcintype) && !state->multikey) ?
		state->typid : opcintype;
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

	/*
	 * The column's OWN type: nothing to resolve.  That is the opclass's input
	 * type, or - for a class declared on a polymorphic type, enum_ops being
	 * FOR TYPE anyenum - the type the column actually has, whatever it is
	 * called.  A scan key names the class's member by its declared type
	 * (sk_subtype is anyenum), but the elements of `col = ANY (array)` are
	 * of the array's element type, which is the column's own enum: the
	 * operator is polymorphic, so the parser left the array as it was
	 * (make_scalar_array_op()).  Both are the class's own values, and looking
	 * up a cross-type (anyenum, mood) member for the second answered a plain
	 * index scan of `mood IN (...)` with "type mood cannot be compared with
	 * index" (2026-09-25 review).  lion_type_is_column() compares BASE types,
	 * because a domain over the enum is the enum's representation, and never
	 * accepts a different enum: its OIDs mean nothing to this column.
	 */
	if (!OidIsValid(keytype) || keytype == opcintype ||
		(IsPolymorphicType(opcintype) && lion_type_is_column(state, keytype)))
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
						   format_type_be(lion_column_type(state, opcintype)))));

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
static void lion_fill_posting_set_entry(Relation index, LionState *state,
										const LionEntryTuple *entry,
										Size itemsz, BlockNumber blkno,
										OffsetNumber offnum,
										LionPostingSet *ps, bool *keeppin);

static void
lion_fill_posting_set(Relation index, LionState *state, Buffer buf,
					 OffsetNumber offnum, LionPostingSet *ps, bool *keeppin)
{
	Page		page = BufferGetPage(buf);
	ItemId		iid = PageGetItemId(page, offnum);

	lion_fill_posting_set_entry(index, state,
								(LionEntryTuple *) PageGetItem(page, iid),
								ItemIdGetLength(iid), BufferGetBlockNumber(buf),
								offnum, ps, keeppin);
}

/*
 * The posting set of the entry at (buf, offnum), which the caller holds
 * locked: an INLINE one keeps a pin of its own on the leaf, as a located set
 * does (DESIGN.md §9), so the caller may let the leaf go.
 */
void
lion_posting_set_at(Relation index, LionState *state, Buffer buf,
					OffsetNumber offnum, LionPostingSet *ps)
{
	bool		keeppin;

	lion_fill_posting_set(index, state, buf, offnum, ps, &keeppin);
	if (keeppin)
	{
		IncrBufferRefCount(buf);
		ps->pinbuf = buf;
	}
}

/*
 * The same from an entry tuple of itemsz bytes that was at (blkno, offnum): a
 * leaf the caller holds locked, or a private copy of one taken under that lock
 * while the caller still holds the leaf's pin (lion_entry_scan_next()), which
 * is the pin *keeppin asks it to hand over.
 */
static void
lion_fill_posting_set_entry(Relation index, LionState *state,
							const LionEntryTuple *entry, Size itemsz,
							BlockNumber blkno, OffsetNumber offnum,
							LionPostingSet *ps, bool *keeppin)
{
	memset(ps, 0, sizeof(LionPostingSet));
	ps->index = index;
	ps->attno = entry->attno;
	ps->pinbuf = InvalidBuffer;
	ps->found = true;
	ps->ntids = entry->ntids;
	ps->ncontainers = entry->ncontainers;
	ps->entryblk = blkno;
	ps->entryoff = offnum;
	ps->cxt = CurrentMemoryContext;
	ps->nuses = 0;
	ps->mat = NULL;
	ps->matfailed = false;

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
		ps->paylen = LION_ENTRY_PAYLOAD_LEN(entry, itemsz);
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
	uint32		ncmp;			/* comparisons made by the sort */
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
 * The same, as the sort of a whole list calls it: a list whose length is a
 * parameter's has no cap (DESIGN.md §15), its sort calls the opclass's
 * comparison some n log n times, and nothing else in it would answer a
 * cancel.  No lock is held while a list is sorted.
 */
static int
lion_probe_sort_cmp(const void *a, const void *b, void *arg)
{
	LionProbeSort *ctx = (LionProbeSort *) arg;

	if ((++ctx->ncmp & 0xffff) == 0)
		CHECK_FOR_INTERRUPTS();
	return lion_probe_key_cmp(a, b, arg);
}

/*
 * Sort the non-NULL values of a list into the order a lookup locates them in
 * (lion_probe_key_cmp(): the probe's own comparison, then the hash), and hand
 * back each one's hash.  Returns how many there are.  Two values of one
 * equality class compare equal and hash alike, so they come out adjacent and
 * with equal hashes: a caller that cuts the list only where the hash changes
 * never splits a class between two pieces, which is what lets a plain scan
 * locate a long list piece by piece without ever returning an entry twice
 * (DESIGN.md §29.4).
 */
int
lion_probe_sort(Relation index, AttrNumber attno, Oid keytype, int nvalues,
				const Datum *values, const bool *isnull, Datum *sorted,
				uint32 *hashes)
{
	LionState   *state = lion_index_column_state(index, attno);
	LionProbe	probe;
	LionProbeSort sortctx;
	LionProbeKey *probes;
	int			n = 0;
	int			i;

	lion_probe_init(index, state, keytype, &probe);
	probes = (LionProbeKey *) palloc_extended(sizeof(LionProbeKey) *
											  Max(nvalues, 1),
											  MCXT_ALLOC_HUGE);
	for (i = 0; i < nvalues; i++)
	{
		if (isnull != NULL && isnull[i])
			continue;
		probes[n].hash = lion_probe_hash(state, &probe, values[i]);
		probes[n].idx = i;
		n++;
		if ((n & 0x3ff) == 0)
			CHECK_FOR_INTERRUPTS();
	}

	sortctx.values = values;
	sortctx.probe = &probe;
	sortctx.collation = state->collation;
	sortctx.ncmp = 0;
	if (n > 1)
		qsort_arg(probes, n, sizeof(LionProbeKey), lion_probe_sort_cmp,
				  &sortctx);

	for (i = 0; i < n; i++)
	{
		sorted[i] = values[probes[i].idx];
		hashes[i] = probes[i].hash;
	}
	pfree(probes);
	return n;
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

static uint32 lion_list_pin_budget(Relation index);
static void lion_list_pin_charge(LionPostingSet *ps);

/*
 * One of many single lookups a caller keeps located together - the keys of a
 * multi-key clause, as many as LION_MAX_QUERY_KEYS of them, and one clause
 * per `@>` or `@@` of the query - under the list pin budget (DESIGN.md §15,
 * "The pin budget").  They used to keep every INLINE leaf pin whatever the
 * budget said, which a clause whose query is a parameter (DESIGN.md §17, "A
 * query known only at run time") sets to a thousand pins a clause with
 * nothing in the query text to show for it (2026-09-28 review).
 *
 * The lookup is lion_posting_set_lookup_col()'s; only what the set keeps
 * differs.  A pin on the leaf the caller's previous set took (*lastpinned)
 * costs no buffer and is not charged, as in a list; a pin on another leaf is
 * charged while the budget lasts, and past it the set lets its leaf go and
 * comes out NOPIN, which every count copes with (lion_count_sources_run()).
 * The keys are looked up in the order the query names them, not in the
 * directory's, so the same leaf may be charged twice: the budget runs out
 * early, never late.
 */
bool
lion_posting_set_lookup_budgeted_col(Relation index, AttrNumber attno,
									 Datum key, Oid keytype,
									 LionPostingSet *ps, Buffer *lastpinned)
{
	bool		found;

	found = lion_posting_set_lookup_col(index, attno, key, keytype, ps);
	if (!BufferIsValid(ps->pinbuf))
		return found;			/* not found, or a CHAIN set: no leaf kept */

	if (ps->pinbuf == *lastpinned)
		return found;
	if (lion_list_pin_budget(index) == 0)
	{
		lion_posting_set_unpin(ps);
		return found;
	}
	lion_list_pin_charge(ps);
	*lastpinned = ps->pinbuf;
	return found;
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
 * together.  It is the smallest of
 *
 *	- LION_LOOKUP_MAX_PINS, 1000: the longest list the planner accepts as a
 *	  literal, so that a literal list pins exactly what it always did - a set
 *	  that is NOPIN costs a second descent in the disjoint sum and the visibility
 *	  map in an OR, and an ordinary query must not pay either;
 *	- an EIGHTH of the buffer pool the index is read into, so that one
 *	  backend never pins more than a modest fraction of the pool however many
 *	  lists its query has.  The failure this budget exists for was 9000 leaves
 *	  against a 2048-buffer pool (16MB): the query ran itself out of buffers,
 *	  and one short of that it would have starved everyone else.  With an
 *	  eighth, 256 there, the query keeps seven eighths for its own heap,
 *	  visibility-map and chain pages and for every other backend.  It only
 *	  binds below 64MB of shared_buffers.  The pool of a TEMPORARY index is
 *	  the backend's own local buffers, temp_buffers (1024 by default, and as
 *	  few as 100), which nothing else shares and which run out just the same:
 *	  "no empty local buffer available" (lion_pin_pool());
 *	- for the SHARED pool, LION_LOOKUP_SHARES times the backend's fair share
 *	  of it, NBuffers / MaxBackends, but never less than LION_LOOKUP_MIN_PINS
 *	  (2026-09-28 review).  The eighth keeps one backend from taking the
 *	  pool, but eight backends running such lists at once still took all of
 *	  it; with the share, the backends that must do so are a sixteenth of all
 *	  the server is configured for, however many that is.  On a stock server
 *	  (128MB, 100 connections: some 130 buffers a backend) the share is 2000
 *	  and the thousand binds, so an ordinary configuration pins what it always
 *	  did; the share binds where max_connections is large for shared_buffers.
 *
 * and a participant of a parallel plan gets its SHARE of that: the limit
 * divided by the participants the plan was started with
 * (lion_list_pin_participants()).  Each participant of a parallel FK-side
 * join locates the fact filters for itself (DESIGN.md §27), and a leader with
 * seven workers took eight budgets for one query.
 *
 * None of them is the backend's plain "fair share" (GetAdditionalPinLimit()
 * of 18): 86 buffers on a stock 128MB server, and a budget of it sent
 * ordinary thousand-value lists to the heap.  No bound kept per backend can
 * promise the pool to every backend at once - that needs a count in shared
 * memory, which an extension that need not be preloaded does not have - and a
 * set past the budget only costs time: it comes out NOPIN, never wrong.
 *
 * The count is backend-wide because the budget is: two unbounded lists in one
 * query share it instead of taking one budget each.  Every set that took a
 * NEW leaf for it is marked `budgeted` and gives it back when released or
 * unpinned.  A set abandoned by an error is never released - but its pin is,
 * by the resource owner that was current when it was taken, and that owner is
 * released with the transaction, subtransaction or portal the error ends.  So
 * each pin is charged to its OWNER as well (lion_list_pin_charge()), and an
 * owner that is released gives back what its sets still had charged
 * (lion_list_pins_resowner()).  A subtransaction failing in a loop - a
 * PL/pgSQL EXCEPTION block around a count - used to leave its charges behind
 * until the top-level transaction ended, and every list after it came out
 * NOPIN (2026-09-28 review); now they go when its pins do, while the sets of
 * a portal that outlives it (a cursor FETCHed inside it) keep theirs, as they
 * keep their pins.  The count is still zeroed at the end of every top-level
 * transaction, when no set can be left.
 *
 * The CURSORS that read the located sets draw on what is left of the same
 * limit (lion_open_budget_init()): a CHAIN set pins the posting page its
 * current container came from, and a list of CHAIN entries located no leaf
 * pin at all and then pinned a page per value when its cursors were built.
 */
#define LION_LOOKUP_MAX_PINS	1000
#define LION_LOOKUP_SHARES		16
#define LION_LOOKUP_MIN_PINS	64

static uint32 lion_list_pins = 0;
static bool lion_list_pins_cb = false;
static int	lion_list_participants = 1;
static ResourceOwner lion_list_participants_owner = NULL;

/*
 * The pins charged to one resource owner.  There are as many of these as
 * owners that hold list pins at one time - the portal a count runs in, a
 * cursor's, a function's - which is a handful, so they are an array searched
 * from its end.
 */
typedef struct LionPinCharge
{
	ResourceOwner owner;
	uint32		pins;
} LionPinCharge;

static LionPinCharge *lion_pin_charges = NULL;
static int	lion_pin_ncharges = 0;
static int	lion_pin_chargecap = 0;

/*
 * The buffer pool a relation's pages are pinned in: the backend's local
 * buffers for a temporary relation, shared_buffers otherwise.  temp_buffers
 * cannot change once the session has touched a temporary table, so the
 * setting is the pool.
 */
static int
lion_pin_pool(Relation rel)
{
	if (rel != NULL && RelationUsesLocalBuffers(rel))
		return num_temp_buffers;
	return NBuffers;
}

static uint32
lion_pin_limit(Relation rel)
{
	uint32		limit;

	limit = (uint32) Min(LION_LOOKUP_MAX_PINS, Max(lion_pin_pool(rel) / 8, 1));
	if (rel == NULL || !RelationUsesLocalBuffers(rel))
	{
		int64		share;

		share = (int64) LION_LOOKUP_SHARES * NBuffers / Max(MaxBackends, 1);
		share = Max(share, (int64) LION_LOOKUP_MIN_PINS);
		limit = (uint32) Min((int64) limit, share);
	}
	if (lion_list_participants > 1)
		limit = Max(limit / (uint32) lion_list_participants, (uint32) 1);
	return limit;
}

/*
 * Set by the count pushdown when a parallel plan starts (DESIGN.md §27): how
 * many participants it was started with, the leader included, each of which
 * locates lists of its own.  Back to 1 when the leader's node ends, and at
 * the end of every top-level transaction.
 */
void
lion_list_pin_participants(int participants)
{
	lion_list_participants = Max(participants, 1);

	/*
	 * An ERROR in a subtransaction ends the query without the node's End, so
	 * the share is also given back when the owner the node ran under is
	 * released (lion_list_pins_resowner()), and the callbacks that do it must
	 * exist by then.
	 */
	lion_list_participants_owner =
		(lion_list_participants > 1) ? CurrentResourceOwner : NULL;
	(void) lion_list_pin_budget(NULL);
}

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
			lion_pin_ncharges = 0;
			lion_list_participants = 1;
			lion_list_participants_owner = NULL;
			break;
		default:
			break;
	}
}

/*
 * A resource owner is being released - a portal's, a subtransaction's, the
 * transaction's - and with it every buffer pin it still holds, among them the
 * pins of the sets an error abandoned: their charges go with them.  The
 * callback runs with CurrentResourceOwner set to the owner being released,
 * once per phase.  A set charged to it that is released after all finds no
 * charge left and gives nothing back (lion_list_pin_return()).
 */
static void
lion_list_pins_resowner(ResourceReleasePhase phase, bool isCommit,
						bool isTopLevel, void *arg)
{
	int			i;

	if (phase != RESOURCE_RELEASE_BEFORE_LOCKS)
		return;

	/* The parallel query that divided the budget is over, one way or another. */
	if (lion_list_participants_owner != NULL &&
		lion_list_participants_owner == CurrentResourceOwner)
	{
		lion_list_participants = 1;
		lion_list_participants_owner = NULL;
	}

	if (lion_pin_ncharges == 0)
		return;
	for (i = lion_pin_ncharges - 1; i >= 0; i--)
	{
		if (lion_pin_charges[i].owner != CurrentResourceOwner)
			continue;
		lion_list_pins -= Min(lion_list_pins, lion_pin_charges[i].pins);
		lion_pin_charges[i] = lion_pin_charges[--lion_pin_ncharges];
		break;
	}
}

static uint32
lion_list_pin_budget(Relation index)
{
	uint32		limit = lion_pin_limit(index);

	if (!lion_list_pins_cb)
	{
		RegisterXactCallback(lion_list_pins_xact, NULL);
		RegisterResourceReleaseCallback(lion_list_pins_resowner, NULL);
		lion_list_pins_cb = true;
	}
	return (lion_list_pins < limit) ? limit - lion_list_pins : 0;
}

/*
 * The open budget of one count, or of one bitmap walk (DESIGN.md §15,
 * "Bounded cursors").
 *
 *	mem		work_mem: the executor's answer to "how much may one node keep",
 *			which is what the recheck batch and the visibility cache are
 *			bounded by as well.  Each is a separate allowance, as the inputs
 *			of a hash join each get one.
 *	pins	what the lists this backend has located have left of the list pin
 *			budget above, so that the leaves a list keeps pinned and the
 *			pages its cursors pin come out of ONE limit per backend - an
 *			eighth of the pool, at most a thousand buffers.
 *
 * rel names the pool (lion_pin_pool()): the heap for a count, whose indexes
 * share its persistence, the index for a bitmap walk.
 */
static void
lion_open_budget_init(LionOpenBudget *budget, Relation rel)
{
	uint32		limit = lion_pin_limit(rel);
	uint32		left = (lion_list_pins < limit) ? limit - lion_list_pins : 0;

	budget->mem = Max((Size) work_mem * 1024, (Size) LION_OPEN_MIN_BYTES);
	budget->pins = (int) Max(left, (uint32) LION_OPEN_MIN_PINS);
}

/*
 * A set takes a leaf of the budget.  The pin it has just taken belongs to the
 * current resource owner, and so does the charge.  The callbacks are
 * registered by lion_list_pin_budget(), which every caller has asked first.
 */
static void
lion_list_pin_charge(LionPostingSet *ps)
{
	ResourceOwner owner = CurrentResourceOwner;
	int			i;

	Assert(lion_list_pins_cb);
	for (i = lion_pin_ncharges - 1; i >= 0; i--)
	{
		if (lion_pin_charges[i].owner == owner)
			break;
	}
	if (i < 0)
	{
		if (lion_pin_ncharges >= lion_pin_chargecap)
		{
			int			newcap = Max(lion_pin_chargecap * 2, 8);

			if (lion_pin_charges == NULL)
				lion_pin_charges = (LionPinCharge *)
					MemoryContextAlloc(TopMemoryContext,
									   sizeof(LionPinCharge) * newcap);
			else
				lion_pin_charges = (LionPinCharge *)
					repalloc(lion_pin_charges, sizeof(LionPinCharge) * newcap);
			lion_pin_chargecap = newcap;
		}
		i = lion_pin_ncharges++;
		lion_pin_charges[i].owner = owner;
		lion_pin_charges[i].pins = 0;
	}
	lion_pin_charges[i].pins++;
	lion_list_pins++;
	ps->budgeted = true;
	ps->pinowner = owner;
}

/*
 * A set that took a leaf of the budget gives it back - to the owner it was
 * charged to, unless that owner has been released since and given it back
 * already.
 */
static inline void
lion_list_pin_return(LionPostingSet *ps)
{
	int			i;

	if (!ps->budgeted)
		return;
	ps->budgeted = false;
	for (i = lion_pin_ncharges - 1; i >= 0; i--)
	{
		if (lion_pin_charges[i].owner != ps->pinowner)
			continue;
		if (lion_pin_charges[i].pins > 0)
			lion_pin_charges[i].pins--;
		if (lion_list_pins > 0)
			lion_list_pins--;
		if (lion_pin_charges[i].pins == 0)
			lion_pin_charges[i] = lion_pin_charges[--lion_pin_ncharges];
		break;
	}
	ps->pinowner = NULL;
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

	budget = lion_list_pin_budget(index);

	/* A binary coercion - the only one taken - changes no value. */
	vals = values;

	/* an array parameter has no length cap (DESIGN.md §15) */
	probes = (LionProbeKey *) palloc_extended(sizeof(LionProbeKey) * nvalues,
											  MCXT_ALLOC_HUGE);
	for (i = 0; i < nvalues; i++)
	{
		if (isnull != NULL && isnull[i])
			continue;			/* `col = NULL` is never true */
		probes[nprobe].hash = lion_probe_hash(state, &probe, vals[i]);
		probes[nprobe].idx = i;
		nprobe++;
		if ((nprobe & 0x3ff) == 0)
			CHECK_FOR_INTERRUPTS();
	}

	sortctx.values = vals;
	sortctx.probe = &probe;
	sortctx.collation = state->collation;
	sortctx.ncmp = 0;
	if (nprobe > 1)
		qsort_arg(probes, nprobe, sizeof(LionProbeKey), lion_probe_sort_cmp,
				  &sortctx);

	for (i = 0; i < nprobe; i++)
	{
		LionSearchKey sk;
		OffsetNumber off;
		bool		dup = false;
		bool		located;
		int			steps;

		/*
		 * Every so often, with no lock held.  The walk keeps the leaf it
		 * stands on LOCKED from one value to the next, and a content lock
		 * holds interrupts off, so the check this loop used to make answered
		 * no cancel however long the list (2026-09-28 review).  The leaf is
		 * let go here, and the next value descends again - what it does
		 * anyway whenever it lies more than a few leaves further right.  The
		 * pins the sets took are theirs and stay.
		 */
		if (i > 0 && (i & 0x3f) == 0)
		{
			if (BufferIsValid(buf))
			{
				UnlockReleaseBuffer(buf);
				buf = InvalidBuffer;
			}
			CHECK_FOR_INTERRUPTS();
		}

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
						lion_list_pin_charge(&sets[nsets]);
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
	}

	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);

	pfree(probes);
	if (nfound != NULL)
		*nfound = found;
	return nsets;
}

/* ---------------------------------------------------------------------
 * A walk of the leaves for keys that come one at a time (DESIGN.md §27)
 * --------------------------------------------------------------------- */

/*
 * Start a walk of key column `attno` of index for keys of type keytype
 * (InvalidOid: the column's own).  The resolution is lion_probe_init()'s, made
 * once for every key instead of once per lookup, and what the walk keeps -
 * the probe's FmgrInfos and the copy of a high key - is allocated in the
 * current memory context, which must outlive the walk.  The column state it
 * keeps is valid for as long as the caller keeps the index open, a relcache
 * flush included (LionAmCache, lion_pages.c).  Nothing is read yet.
 */
void
lion_lookup_walk_begin(LionLookupWalk *walk, Relation index, AttrNumber attno,
					   Oid keytype)
{
	memset(walk, 0, sizeof(LionLookupWalk));
	walk->index = index;
	walk->state = lion_index_column_state(index, attno);
	lion_probe_init(index, walk->state, keytype, &walk->probe);
	walk->blk = InvalidBlockNumber;
	walk->buf = InvalidBuffer;
	walk->rightlink = InvalidBlockNumber;
	walk->prefetched = InvalidBlockNumber;

	/* a high key is a pivot: a header and a key, never a payload */
	walk->hikey = (LionEntryTuple *)
		palloc(MAXALIGN(LION_ENTRY_HDRSZ + LION_MAX_KEY_SIZE));
	walk->hikeylen = 0;

	/*
	 * Stepping right over s leaves reads s pages where a descent reads the
	 * root, the levels below it and the leaf: height + 1.  So a walk steps
	 * over at most `height` of them and descends for a key further away.
	 */
	walk->maxsteps = (int) Max(walk->state->ix->meta.height, 1);
	walk->stepok = true;

	/*
	 * The leaves come in the order the keys do, so the next one can be asked
	 * for before it is needed - where the storage can overlap the reads at
	 * all, which is what the tablespace's io concurrency says.
	 */
	walk->prefetch =
		(get_tablespace_io_concurrency(index->rd_rel->reltablespace) > 0);
}

/*
 * Can the keys be sorted into the directory's order (lion_lookup_walk_cmp())?
 * Without that each key is located by a descent of its own, in any order.
 */
bool
lion_lookup_walk_ordered(const LionLookupWalk *walk)
{
	return walk->probe.walk;
}

/* The hash a key is sorted by, after its comparison, and searched with. */
uint32
lion_lookup_walk_hash(LionLookupWalk *walk, Datum key)
{
	return lion_probe_hash(walk->state, &walk->probe,
						   lion_probe_value(&walk->probe, key));
}

/*
 * The directory's order of two keys (lion_probe_key_cmp()): the probe's own
 * comparison of two values of the key type, then the hash, which is all an
 * unordered column's directory is ordered by.  Keys that tie are the same
 * position in the directory - the same entry, or a run of hash collisions
 * that a lookup of either scans from its start.  A long sort calls this some
 * n log n times through the opclass, and nothing else in it answers a cancel;
 * the caller holds no lock while it sorts.
 */
int
lion_lookup_walk_cmp(LionLookupWalk *walk, Datum a, uint32 ahash, Datum b,
					 uint32 bhash)
{
	if ((++walk->ncmp & 0xffff) == 0)
		CHECK_FOR_INTERRUPTS();
	if (walk->probe.hassort)
	{
		int32		c = DatumGetInt32(FunctionCall2Coll(&walk->probe.sortproc,
														walk->state->collation,
														a, b));

		if (c != 0)
			return c < 0 ? -1 : 1;
	}
	if (ahash != bhash)
		return ahash < bhash ? -1 : 1;
	return 0;
}

/* The order of lion_lookup_walk_cmp(), as lion_walk_order_equal() compares it. */
void
lion_lookup_walk_order(const LionLookupWalk *walk, LionWalkOrder *order)
{
	memset(order, 0, sizeof(LionWalkOrder));
	order->valid = walk->probe.walk;
	order->hassort = walk->probe.hassort;
	if (walk->probe.hassort)
		order->sortproc = walk->probe.sortproc.fn_oid;
	order->hashproc = walk->probe.crosstype ? walk->probe.hashinfo.fn_oid :
		walk->state->hashproc.fn_oid;
	order->collation = walk->state->collation;
}

bool
lion_walk_order_equal(const LionWalkOrder *a, const LionWalkOrder *b)
{
	return a->valid && b->valid && a->hassort == b->hassort &&
		a->sortproc == b->sortproc && a->hashproc == b->hashproc &&
		a->collation == b->collation;
}

/* Let go of the leaf the walk stands on, and remember where it was. */
void
lion_lookup_walk_pause(LionLookupWalk *walk)
{
	if (BufferIsValid(walk->buf))
		ReleaseBuffer(walk->buf);
	walk->buf = InvalidBuffer;
}

/* ... and forget it too: the next key may be anywhere. */
void
lion_lookup_walk_restart(LionLookupWalk *walk)
{
	lion_lookup_walk_pause(walk);
	walk->blk = InvalidBlockNumber;
	walk->rightlink = InvalidBlockNumber;
	walk->hikeylen = 0;
	walk->stepok = true;
}

/*
 * Locate the posting set of key, whose hash is lion_lookup_walk_hash()'s,
 * into *ps - exactly the set lion_posting_set_lookup_col() would locate, and
 * as a single lookup does, an INLINE set keeps a pin of its own on its leaf
 * (DESIGN.md §9).  The key must not sort before the last one this walk
 * located since it was begun or restarted.
 *
 * Where the walk starts looking:
 *
 *	- below the high key the last leaf had: that leaf, pinned still or read
 *	  again by its block number.  If it has split since, the key may have gone
 *	  to a page on its right, and the walk moves right as a descent does;
 *	- at or above it, while the keys have been close: the leaf that was its
 *	  right sibling.  That page's lower bound was the high key just passed, and
 *	  a lower bound never moves, so the key is there or further right - even if
 *	  the last leaf has split and has a new right sibling in between, which then
 *	  holds only keys below that bound.  At most maxsteps pages further right;
 *	- otherwise, or past those: a descent from the root.  A descent that lands
 *	  on the leaf right of the last one says the keys are close again.
 *
 * A key found by following a run of hash collisions across a page boundary
 * leaves the walk to the right of where that run begins, and the next key may
 * belong to it: the walk forgets its place then, as the list walk does
 * (lion_posting_set_lookup_many_col()).
 *
 * No lock is held on return, and a pin only on the leaf the key was located
 * on, when the set is INLINE and pins it as well, or when the key has no
 * entry and no count follows; lion_lookup_walk_pause() lets it go.  A CHAIN
 * set's count reads the posting tree for as long as it takes, and the leaf is
 * not held through it.
 */
bool
lion_lookup_walk_find(LionLookupWalk *walk, Datum key, uint32 hash,
					  LionPostingSet *ps)
{
	Relation	index = walk->index;
	LionSearchKey sk;
	Buffer		buf = InvalidBuffer;
	OffsetNumber off;
	BlockNumber prevright = walk->rightlink;
	bool		had = BlockNumberIsValid(walk->blk);
	bool		stepping = false;
	bool		located;
	bool		moved = false;
	bool		keeppin = false;
	int			steps;

	memset(ps, 0, sizeof(LionPostingSet));
	ps->index = index;
	ps->attno = walk->state->attno;
	ps->pinbuf = InvalidBuffer;
	ps->head = InvalidBlockNumber;
	ps->entryblk = InvalidBlockNumber;
	ps->entryoff = InvalidOffsetNumber;

	key = lion_probe_value(&walk->probe, key);
	lion_probe_search_key(walk->state, &walk->probe, key, hash, &sk);

	if (!walk->probe.walk)
	{
		/*
		 * The keys are in no order the directory knows: every one is located
		 * as the single lookup locates it - a descent, or the leaf walk.
		 */
		if (!lion_dir_find(index, NULL, walk->state->ix, &sk,
						   BUFFER_LOCK_SHARE, false, &buf, &off, NULL))
		{
			if (BufferIsValid(buf))
				UnlockReleaseBuffer(buf);
			return false;
		}
		lion_posting_set_take(index, walk->state, buf, off, ps);
		return true;
	}

	if (had)
	{
		if (walk->hikeylen == 0 || lion_cmp_entry(walk->hikey, &sk) > 0)
		{
			/* on the last leaf, or right of it if it has split since */
			if (BufferIsValid(walk->buf))
			{
				buf = walk->buf;
				walk->buf = InvalidBuffer;
				LockBuffer(buf, BUFFER_LOCK_SHARE);
			}
			else
				buf = lion_dir_read_leaf(index, walk->blk);
			walk->stepok = true;
		}
		else
		{
			lion_lookup_walk_pause(walk);
			if (walk->stepok && BlockNumberIsValid(walk->rightlink))
			{
				buf = lion_dir_read_leaf(index, walk->rightlink);
				stepping = true;
			}
		}

		/*
		 * Right, past what split off since - as far as it takes, as in a
		 * descent - or past leaves with no key of the caller's, a few at most.
		 */
		for (steps = 1; BufferIsValid(buf); steps++)
		{
			Page		page = BufferGetPage(buf);

			if (LionPageIsRightmost(page) ||
				lion_cmp_entry(lion_dir_highkey(page), &sk) > 0)
				break;
			if (stepping && steps >= walk->maxsteps)
			{
				UnlockReleaseBuffer(buf);
				buf = InvalidBuffer;
				walk->stepok = false;
				break;
			}
			buf = lion_dir_step_right(index, buf, BUFFER_LOCK_SHARE);
		}
	}
	else
		lion_lookup_walk_pause(walk);

	if (BufferIsValid(buf))
		off = lion_dir_binsrch(BufferGetPage(buf), &sk);
	else
	{
		buf = lion_dir_search(index, NULL, walk->state->ix, &sk,
							  BUFFER_LOCK_SHARE, false, &off);
		if (had)
			walk->stepok = (BufferGetBlockNumber(buf) == prevright);
	}

	/*
	 * Where the walk stands now: the leaf the key's run begins on, its right
	 * sibling and its high key, copied while it is locked.  A high key longer
	 * than a pivot can be is damage the next read of the page reports; the
	 * walk just does not keep its place past it.
	 */
	{
		Page		page = BufferGetPage(buf);

		walk->blk = BufferGetBlockNumber(buf);
		walk->rightlink = LionPageGetOpaque(page)->rightlink;
		walk->hikeylen = 0;
		if (!LionPageIsRightmost(page))
		{
			ItemId		iid = PageGetItemId(page, FirstOffsetNumber);
			LionEntryTuple *hk = (LionEntryTuple *) PageGetItem(page, iid);
			Size		len = ItemIdGetLength(iid);

			if (len >= LION_ENTRY_HDRSZ &&
				len <= MAXALIGN(LION_ENTRY_HDRSZ + LION_MAX_KEY_SIZE) &&
				(Size) hk->keylen <= len - LION_ENTRY_HDRSZ)
			{
				memcpy(walk->hikey, hk, len);
				walk->hikeylen = len;
			}
			else
				walk->blk = InvalidBlockNumber;
		}
	}

	located = lion_dir_scan_run(index, &sk, BUFFER_LOCK_SHARE, &buf, &off,
								&moved);
	if (located)
	{
		lion_fill_posting_set(index, walk->state, buf, off, ps, &keeppin);
		if (keeppin)
		{
			IncrBufferRefCount(buf);
			ps->pinbuf = buf;
		}
	}

	if (moved)
	{
		UnlockReleaseBuffer(buf);
		lion_lookup_walk_restart(walk);
		return located;
	}

	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	if (located && !keeppin)
		ReleaseBuffer(buf);
	else
		walk->buf = buf;

	/*
	 * Keys that come close together will want the next leaf too: ask for it
	 * now, with nothing locked, so that the read overlaps the count.
	 */
	if (walk->prefetch && walk->stepok && BlockNumberIsValid(walk->blk) &&
		BlockNumberIsValid(walk->rightlink) &&
		walk->rightlink != walk->prefetched)
	{
		(void) PrefetchBuffer(index, MAIN_FORKNUM, walk->rightlink);
		walk->prefetched = walk->rightlink;
	}

	return located;
}

/*
 * One key's set by a descent of its own, as lion_posting_set_lookup_col()
 * locates it, but with the probe lion_lookup_walk_begin() resolved: for a key
 * of another type than the column's, lion_probe_init() looks up the family's
 * cross-type operators and support functions in the catalogs and sets up
 * their FmgrInfos, which a lookup per key of an FK-side join made thousands
 * of times over for the same answer.  The key needs no order and the walk
 * keeps its place: the leaf found is released here, as a single lookup's is.
 */
bool
lion_lookup_walk_descend(LionLookupWalk *walk, Datum key, LionPostingSet *ps)
{
	uint32		hash;

	key = lion_probe_value(&walk->probe, key);
	hash = lion_probe_hash(walk->state, &walk->probe, key);
	return lion_posting_set_locate(walk->index, walk->state, &walk->probe, key,
								   hash, ps);
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

	/* a spilled copy owns its temporary file (lion_spill_finish()) */
	if (ps->mat != NULL && ps->mat->file != NULL)
	{
		BufFileClose(ps->mat->file);
		ps->mat->file = NULL;
	}

	/*
	 * ... and a view of a shared copy the files it opened of its chunks
	 * (lion_copy_view()); the files are the plan's, and stay.
	 */
	if (ps->mat != NULL && ps->mat->files != NULL)
	{
		int			c;

		for (c = 0; c < ps->mat->nchunks; c++)
		{
			if (ps->mat->files[c] != NULL)
				BufFileClose(ps->mat->files[c]);
			ps->mat->files[c] = NULL;
		}
		ps->mat->files = NULL;
	}
	ps->mat = NULL;				/* ... and so does the materialized copy */
	ps->matfailed = false;
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
 *
 * maxbytes is what the copy may take at most: what is left of the budget of
 * all the copies one count's sources hold (lion_count_sources_run(); DESIGN.md
 * §15, "Bounded cursors").  A set that does not fit is given up on for good
 * (ps->matfailed) and keeps being walked page by page.
 */
static bool
lion_posting_set_materialize(LionPostingSet *ps, Size maxbytes)
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
		BlockNumber imgblk = blkno;
		OffsetNumber off;
		OffsetNumber maxoff;

		blkno = LionPageGetOpaque(img)->rightlink;
		maxoff = PageGetMaxOffsetNumber(img);

		for (off = FirstOffsetNumber; off <= maxoff; off = OffsetNumberNext(off))
		{
			/* checked for what the copy and its readers need (§3) */
			const LionContainer *c = lion_page_item_fetch(ps->index, img,
														  imgblk, off);
			Size		sz = lion_item_size(c);

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

			/*
			 * ... and whatever it is, once the count's copies are spent.  The
			 * copy is kept at its exact size (below), so this is what it will
			 * hold.
			 */
			if (sizeof(LionMatSet) + MAXALIGN(used + sz) +
				sizeof(LionContainer *) * (noffs + 1) > maxbytes)
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
			Buffer		pagebuf;
			Page		page;

			lion_posting_pages_read++;
			pagebuf = ReadBuffer(ps->index, blkno);
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
		LionMatSet  *mat = (LionMatSet *) palloc0(sizeof(LionMatSet));

		/*
		 * The buffer grew by doubling from a page; what is kept is the exact
		 * size, because a GROUP BY may keep hundreds of these for as long as
		 * the relation is counted, and a set of one small segment would
		 * otherwise hold eight kilobytes.
		 */
		if (cap > used)
		{
			char	   *exact = (char *) palloc(Max(used, (Size) 1));

			memcpy(exact, buf, used);
			pfree(buf);
			buf = exact;
		}

		mat->ncontainers = noffs;
		mat->bytes = used;
		mat->held = sizeof(LionMatSet) + MAXALIGN(Max(used, (Size) 1)) +
			sizeof(LionContainer *) * Max(noffs, 1);
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
		ps->matfailed = true;
	}

	pfree(offs);
	MemoryContextSwitchTo(oldcxt);

	return ok;
}

/*
 * The sets lion_count_sources_run() gives up on copying: hopeless even as an
 * ARRAY of members - the test it makes before it tries - or tried and too big
 * for what was left of the budget.  Each count walks their pages again.
 */
bool
lion_posting_set_rewalked(const LionPostingSet *ps)
{
	if (!ps->found || ps->is_inline || ps->mat != NULL)
		return false;
	return ps->matfailed ||
		(ps->ncontainers > LION_MATERIALIZE_MAX_CONTAINERS &&
		 ps->ntids > LION_MATERIALIZE_MAX_BYTES / sizeof(uint16));
}


/* ---------------------------------------------------------------------
 * Spilled copies (LionSpill)
 * --------------------------------------------------------------------- */

/*
 * Start a spill whose entries - and file - live in cxt.  The file honours
 * temp_tablespaces, which the caller has had PrepareTempTablespaces() look up
 * BEFORE it started to read the index: the lookup reads catalogs, which may
 * process invalidations, and that must not happen here, with a walk of the
 * index under way.
 */
static void
lion_spill_begin(LionSpill *sp, MemoryContext cxt)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(cxt);

	sp->cxt = cxt;
	sp->file = (sp->fileset != NULL) ?
		BufFileCreateFileSet(sp->fileset, sp->name) :
		BufFileCreateTemp(false);
	sp->cap = 256;
	sp->ents = (LionSpillEnt *) palloc(sizeof(LionSpillEnt) * sp->cap);
	sp->nents = 0;
	MemoryContextSwitchTo(oldcxt);
}

/* Append one container: the next in ascending key order, one per key. */
static void
lion_spill_add(LionSpill *sp, const LionContainer *c)
{
	Size		sz = lion_item_size(c);
	LionSpillEnt *e;
	int			fileno;
	pgoff_t		off;

	Assert(sz <= LION_CONTAINER_MAX_SIZE);
	Assert(sp->nents == 0 || sp->ents[sp->nents - 1].ckey < c->ckey);

	if (sp->nents >= sp->cap)
	{
		sp->cap *= 2;
		sp->ents = (LionSpillEnt *)
			repalloc_huge(sp->ents, sizeof(LionSpillEnt) * sp->cap);
	}
	BufFileTell(sp->file, &fileno, &off);
	if (fileno > PG_INT16_MAX)
		elog(ERROR, "lion index: a spilled posting set of more than %d file segments",
			 PG_INT16_MAX);

	e = &sp->ents[sp->nents++];
	e->off = off;
	e->ckey = c->ckey;
	e->size = (uint16) sz;
	e->fileno = (int16) fileno;
	BufFileWrite(sp->file, c, sz);
}

/*
 * The set a finished spill makes.  What it holds in memory is its entries and
 * the file's one-block buffer; the entries are cut to their number.
 */
static LionMatSet *
lion_spill_finish(LionSpill *sp)
{
	LionMatSet *mat;
	Size		bytes = 0;
	int			i;

	mat = (LionMatSet *) MemoryContextAllocZero(sp->cxt, sizeof(LionMatSet));
	if (sp->nents < sp->cap)
		sp->ents = (LionSpillEnt *)
			repalloc_huge(sp->ents, sizeof(LionSpillEnt) * Max(sp->nents, 1));
	for (i = 0; i < sp->nents; i++)
		bytes += sp->ents[i].size;

	mat->ncontainers = sp->nents;
	mat->bytes = bytes;
	mat->held = sizeof(LionMatSet) + sizeof(LionSpillEnt) * Max(sp->nents, 1) +
		BLCKSZ;
	mat->file = sp->file;
	mat->spill = sp->ents;
	sp->file = NULL;
	sp->ents = NULL;
	return mat;
}

/* Give up on a spill: close its file. */
static void
lion_spill_abandon(LionSpill *sp)
{
	if (sp->file != NULL)
		BufFileClose(sp->file);
	sp->file = NULL;
	if (sp->ents != NULL)
		pfree(sp->ents);
	sp->ents = NULL;
}

/* Read container i of a spilled set into buf, LION_CONTAINER_MAX_SIZE long. */
static void
lion_spill_read(const LionMatSet *mat, int i, LionContainer *buf)
{
	const LionSpillEnt *e = &mat->spill[i];
	BufFile    *file = mat->file;

	/* a view of a shared copy: the file of the chunk container i is of */
	if (file == NULL)
	{
		int			lo = 0;
		int			hi = mat->nchunks - 1;

		while (lo < hi)
		{
			int			mid = lo + (hi - lo + 1) / 2;

			if (mat->chunkfirst[mid] <= i)
				lo = mid;
			else
				hi = mid - 1;
		}
		file = mat->files[lo];
	}
	if (file == NULL || e->fileno < 0 || e->size > LION_CONTAINER_MAX_SIZE)
		elog(ERROR, "lion index: container %d of a spilled copy is in no file", i);

	if (BufFileSeek(file, e->fileno, e->off, SEEK_SET) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek in the temporary file of a spilled lion posting set")));
	BufFileReadExact(file, buf, e->size);
}

/*
 * Keep a finished spill's first container in memory (LionMatSet.first): what
 * every cursor built over the set reads first, whatever it is sought to next.
 */
static void
lion_spill_keep_first(LionMatSet *mat, MemoryContext cxt)
{
	Size		size;

	if (mat->ncontainers == 0)
		return;
	size = MAXALIGN(Max((Size) mat->spill[0].size, LION_CONTAINER_HDRSZ));
	mat->first = (LionContainer *) MemoryContextAlloc(cxt, size);
	lion_spill_read(mat, 0, mat->first);
	mat->held += size;
}


/* ---------------------------------------------------------------------
 * Seeking a private copy (LionMatSet)
 * --------------------------------------------------------------------- */

/*
 * The most key slots a direct index spends on one container: four bytes a
 * slot, so at most sixteen bytes a container, the size of a spilled
 * container's entry and less than any container in memory takes with its
 * pointer.  A copy whose keys are sparser than that is searched instead.
 */
#define LION_MAT_DIR_SPREAD		4

/* Container i's key: the last one an item covers, as a seek compares it. */
static inline uint32
lion_mat_key(const LionMatSet *mat, int i)
{
	if (mat->keys != NULL)
		return mat->keys[i];
	if (mat->spill != NULL)
		return mat->spill[i].ckey;
	return lion_item_last_ckey(mat->containers[i]);
}

/*
 * The first container at or after `from` whose key is at or above target
 * (ncontainers when there is none): what a cursor over the copy is sought
 * to.  Direct where the copy has an index of its keys (lion_mat_index()),
 * else a binary search - over keys[], where the copy keeps them, without a
 * branch to mispredict at every step; the keys of a FK-side join's counts
 * come in no order the copy can predict.
 */
static inline int
lion_mat_seek(const LionMatSet *mat, int from, uint32 target)
{
	int			lo = from;
	int			hi = mat->ncontainers;

	if (lo >= hi)
		return hi;

	if (mat->dir != NULL)
	{
		uint32		slot;

		if (target <= mat->dirbase)
			return lo;
		slot = target - mat->dirbase;
		if (slot >= mat->dirlen)
			return hi;
		return Max(lo, (int) mat->dir[slot]);
	}

	if (mat->keys != NULL)
	{
		const uint32 *base = mat->keys + lo;
		int			n = hi - lo;

		while (n > 1)
		{
			int			half = n / 2;

			base = (base[half] < target) ? base + half : base;
			n -= half;
		}
		return (int) (base - mat->keys) + (*base < target ? 1 : 0);
	}

	while (lo < hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (lion_mat_key(mat, mid) < target)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/*
 * Index a copy by its container keys, for the seeks every count makes of it
 * (DESIGN.md §27, "The copy, looked up by key"): the keys in an array of
 * their own - what a binary search reads, four bytes a container, where it
 * used to follow a pointer to each container it compared - and, where they
 * are dense, the direct index, which answers a seek with one read.  A copy
 * is never changed once it is made, so neither goes stale.
 *
 * A key an item covers is its last (lion_item_last_ckey()): the seek looks
 * for the first item whose last key is at or above the target, as it always
 * has.  The items of a copy are in ascending key order and never overlap, so
 * those keys ascend strictly, which is what both rely on; a copy whose keys
 * would not is left as it is.
 *
 * `room` is what the index may take from the copy's memory; what it takes is
 * added to mat->held.  A spilled copy keeps its keys in its entries already,
 * and gets the direct index alone - sixteen bytes a container at most, the
 * size of those entries.
 */
static void
lion_mat_index(LionMatSet *mat, MemoryContext cxt, Size room)
{
	uint32	   *keys;
	uint32		first;
	uint32		last;
	Size		dirbytes;
	int			n = mat->ncontainers;
	int			i;

	if (n < 2)
		return;

	keys = (uint32 *) MemoryContextAlloc(cxt, sizeof(uint32) * n);
	for (i = 0; i < n; i++)
	{
		keys[i] = lion_mat_key(mat, i);
		if (i > 0 && keys[i] <= keys[i - 1])
		{
			pfree(keys);
			return;
		}
	}

	first = keys[0];
	last = keys[n - 1];
	dirbytes = sizeof(uint32) * ((Size) (last - first) + 1);

	if (mat->file == NULL)
	{
		if (sizeof(uint32) * n > room)
		{
			pfree(keys);
			return;
		}
		mat->keys = keys;
		mat->held += sizeof(uint32) * n;
		room -= sizeof(uint32) * n;
	}
	else
		room = dirbytes;		/* bounded by the spread alone */

	if ((uint64) last - first + 1 <= (uint64) n * LION_MAT_DIR_SPREAD &&
		dirbytes <= room)
	{
		uint32	   *dir = (uint32 *) MemoryContextAlloc(cxt, dirbytes);
		uint64		k = 0;

		/* slot k: the first container whose key is at or above first + k */
		for (i = 0; i < n; i++)
		{
			while ((uint64) first + k <= keys[i])
				dir[k++] = (uint32) i;
		}
		Assert(k == (uint64) last - first + 1);
		mat->dir = dir;
		mat->dirbase = first;
		mat->dirlen = (uint32) k;
		mat->held += dirbytes;
	}

	if (mat->keys == NULL)
		pfree(keys);
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
	cur->haspage = false;
}

/*
 * The staging buffer an INLINE cursor copies its items into.
 * lion_inline_fetch() copies at most what is left of the payload, header
 * peek included, so the payload's own length is always enough; it is never
 * more than LION_CONTAINER_MAX_SIZE, the largest item there is.  palloc()
 * MAXALIGNs the buffer whatever its length, which is what a BITSET item's
 * uint64 words need.
 */
static inline Size
lion_inline_stage_size(Size paylen)
{
	Size		size = Max(paylen, LION_CONTAINER_HDRSZ + sizeof(uint16));

	return MAXALIGN(Min(size, LION_CONTAINER_MAX_SIZE));
}

static void lion_inline_skip(const char *payload, Size paylen, Size *off,
							 uint32 target, LionContainer *buf);

/*
 * A cursor over `set`, standing at its first container whose key is at or
 * above `target` - lion_cursor_seek() from nothing in hand: a CHAIN set's
 * first leaf comes from a descent for that key rather than for key 0 and the
 * leaves between.  Nothing below the target is read, so nothing of it is
 * counted or pinned (§9 asks nothing of it).  `image` is where a CHAIN set's
 * leaves are copied to, the caller's to keep - a walk that sets up many
 * cursors again and again gives each the same one every time - or NULL for
 * one of the cursor's own.
 */
static void
lion_cursor_init_at(LionSetCursor *cur, const LionPostingSet *set,
					LionCountCtx *cx, bool droppins, uint32 target,
					PGAlignedBlock *image)
{
	memset(cur, 0, sizeof(LionSetCursor));
	cur->set = set;
	cur->cx = cx;
	cur->pinbuf = InvalidBuffer;
	cur->nextblk = InvalidBlockNumber;
	cur->droppins = droppins || cx->droppins;
	cur->mintarget = target;

	if (!set->found)
		return;

	if (set->mat != NULL)
	{
		/* a private copy: nothing to pin, nothing to walk */
		cur->matidx = 0;
		if (target > 0)
		{
			cx->stats.copy_seeks++;
			cur->matidx = lion_mat_seek(set->mat, 0, target);
		}

		/* ... and when it is spilled, a container at a time is read back */
		if (lion_mat_spills(set->mat))
			cur->cbuf = (LionContainer *)
				palloc(MAXALIGN(LION_CONTAINER_MAX_SIZE));
	}
	else if (set->is_inline)
	{
		cur->cbuf = (LionContainer *) palloc(lion_inline_stage_size(set->paylen));
		cur->payoff = 0;
		if (target > 0)
			lion_inline_skip(set->payload, set->paylen, &cur->payoff, target,
							 cur->cbuf);
		/* borrowed, not owned: the LionPostingSet releases it */
		cur->pinbuf = set->pinbuf;
		cur->ownpin = false;
	}
	else
	{
		/*
		 * The entry's head block is the ROOT of the posting tree (DESIGN.md
		 * §22), so the first leaf comes from a descent for the first key
		 * wanted - which is also how a root push-down cannot be raced: the
		 * descent hands back a page that WAS a leaf under the lock it read it
		 * with.
		 */
		cur->imgbuf = (image != NULL) ? image :
			(PGAlignedBlock *) palloc(sizeof(PGAlignedBlock));
		cur->img = (Page) cur->imgbuf->data;
		cur->nextblk = InvalidBlockNumber;
		cur->descend = true;
		cur->seekckey = target;

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

/* A cursor over `set`, standing at its first container. */
static void
lion_cursor_init(LionSetCursor *cur, const LionPostingSet *set, LionCountCtx *cx,
				 bool droppins)
{
	lion_cursor_init_at(cur, set, cx, droppins, 0, NULL);
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
	cur->imgblk = BufferGetBlockNumber(buf);
	cur->haspage = true;

	/*
	 * A caller that needs no interlock (DESIGN.md §29.5: a plain index scan
	 * under an MVCC snapshot, which visits the heap for every TID) keeps the
	 * image and nothing else.  Everything the cursor does next - the items,
	 * the right link, maxckey for a seek - is read from the image, so the pin
	 * was only ever the §9 interlock, and a pin held across a paused scan
	 * would make every VACUUM of the table wait for it.  The same holds for
	 * a cursor the expression above it has told to carry no interlock
	 * (cur->droppins, see LionSetCursor).
	 */
	if (cur->droppins)
	{
		UnlockReleaseBuffer(buf);
		cur->pinbuf = InvalidBuffer;
		cur->ownpin = false;
	}
	else
	{
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		cur->pinbuf = buf;
		cur->ownpin = true;
	}

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
		if (!lion_mat_inmem(mat, cur->matidx))
		{
			if (cur->matidx == 0 && mat->first != NULL)
			{
				cur->matidx++;
				return mat->first;
			}
			lion_spill_read(mat, cur->matidx++, cur->cbuf);
			cur->cx->stats.copy_file_reads++;
			return cur->cbuf;
		}
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

		/*
		 * Finish the page we already have in hand.  An item goes to the
		 * container code only once lion_page_item_fetch() has made sure it
		 * holds what its header says (DESIGN.md §3, "untrusted containers").
		 */
		if (cur->haspage && cur->off <= cur->maxoff)
		{
			OffsetNumber off = cur->off;

			cur->off = OffsetNumberNext(off);
			return lion_page_item_fetch(cur->set->index, cur->img, cur->imgblk,
										off);
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
			cur->off = lion_page_find_item(cur->set->index, cur->img,
										   cur->imgblk, cur->seekckey, &found);
			CHECK_FOR_INTERRUPTS();
			continue;
		}

		if (!BlockNumberIsValid(cur->nextblk))
			return NULL;

		lion_posting_pages_read++;
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
 * The cursor stands at a container it has read: what EXPLAIN ANALYZE counts
 * of it (LionCountStats) - every container, and whether it came from the
 * count's own key set or from a private copy.
 */
static inline void
lion_cursor_counted(LionSetCursor *cur)
{
	LionCountCtx *cx = cur->cx;

	cx->stats.containers_visited++;
	if (cur->set->mat != NULL)
		cx->stats.copy_containers++;
	if (cur->set == cx->keyset)
		cx->stats.key_containers++;
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
	/* the pairs a reader may look at, and los[] where that many ckeys end */
	uint32		n = lion_sparse_npairs(seg);
	const uint16 *los = LION_SPARSE_LOS_CONST_AT(seg, n);
	uint32		ckey;
	uint32		end;
	Size		need;

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

	ckey = ckeys[cur->segpos];

	/*
	 * Allocated on first use, not per cursor: an IN list of a thousand values
	 * (DESIGN.md §15) is a thousand cursors, and most posting sets hold no
	 * sparse segment at all.  And sized for what one container key of a
	 * segment holds, fewer than LION_SPARSE_THRESHOLD members, rather than
	 * for the largest container: a sparse column's IN list used to cost a
	 * second 8 kB per value here.  A segment is not trusted to keep that
	 * promise, though - the buffer grows to whatever this key's pairs need,
	 * which an ARRAY container of up to LION_ARRAY_MAX_CARD members and
	 * LION_CONTAINER_MAX_SIZE beyond that (lion_container_append_sorted()
	 * turns it into a BITSET) always covers.
	 */
	for (end = cur->segpos; end < n && ckeys[end] == ckey; end++)
		;
	if (end - cur->segpos > LION_ARRAY_MAX_CARD)
		need = LION_CONTAINER_MAX_SIZE;
	else
		need = LION_CONTAINER_HDRSZ +
			Max(end - cur->segpos, LION_SPARSE_THRESHOLD) * sizeof(uint16);
	if (cur->segbuf == NULL || cur->segcap < need)
	{
		/* (the old one, if any, goes with the context: only a malformed
		 * segment can get here twice) */
		cur->segcap = MAXALIGN(need);
		cur->segbuf = (LionContainer *) palloc(cur->segcap);
	}

	/*
	 * A segment's pairs are data (DESIGN.md §3): a lo is masked into range
	 * before the builder sees it, and the builder copes with pairs that are
	 * out of order or repeated.
	 */
	lion_container_init(cur->segbuf, ckey);
	do
	{
		lion_container_append_sorted(cur->segbuf,
									 (uint16) (los[cur->segpos] & LION_LO_MASK));
		cur->segpos++;
	} while (cur->segpos < n && ckeys[cur->segpos] == ckey);

	cur->cur = cur->segbuf;
	cur->valid = true;
	lion_cursor_counted(cur);
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
			lion_cursor_counted(cur);
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

		if (!cur->haspage)
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
			cur->off = lion_page_find_item(cur->set->index, cur->img,
										   cur->imgblk, target, &found);
			return;
		}

		if (steps >= LION_POSTING_SEEK_STEPS || cur->cx->farseeks)
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

		lion_posting_pages_read++;
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
		/*
		 * A private copy is an array: its index answers where the target is
		 * (lion_mat_seek()) - or a binary search does, a spilled one by the
		 * keys it keeps in memory, one per container.
		 */
		cur->cx->stats.copy_seeks++;
		cur->matidx = lion_mat_seek(cur->set->mat, cur->matidx, target);
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
 *	AND		the intersection (`tags @> '{a,b}'`, the AND nodes of a tsquery,
 *			DESIGN.md §17, and a lion index scan's AND of its quals' sets,
 *			§29.2): the children are wound forward until they all stand at
 *			one container key, and the container is the AND of theirs.  A
 *			container key whose intersection comes out empty is skipped here
 *			rather than handed up.  The winding is the count's own leapfrog
 *			(lion_leapfrog()): the child with the fewest members drives, the
 *			others are sought to its keys fewest first, and a key is given up
 *			at the first child that rules it out.
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
 *
 * Two shapes keep FEWER pins than that, and both are chosen by the plan
 * (lion_plan_node(), DESIGN.md §15 "Bounded cursors") only when a node's
 * children would hold more than the open budget: an AND that keeps pins on
 * the one child the rule needs (the intersection's members are all in that
 * child's container, whose page it pins), and a WIDE OR, a windowed union
 * that holds none (lion_wide_fill()).  The plan says which, and says so in
 * the same `pinned` the count trusts the visibility map by - a wide union is
 * never pinned - so no shape can weaken the rule without the count knowing.
 */
typedef struct LionOrHeapEnt
{
	uint32		ckey;			/* sub[child].ckey when it was pushed */
	int32		child;
	const LionContainer *cur;	/* sub[child].cur when it was pushed */
} LionOrHeapEnt;

/*
 * THE CURSOR PLAN (DESIGN.md §15, "Bounded cursors").
 *
 * An expression cursor opens every leaf below it at once, and each open leaf
 * costs memory - its LionExprCursor and a staging buffer, or a whole page
 * image for a CHAIN set - and, for a CHAIN set read the pinned way, a buffer
 * pin from the moment the cursor is built until it moves past its current
 * page.  Nothing about the query bounded how many leaves that was: an IN list
 * whose length is a parameter is a leaf per value, and a list of 50000 values
 * held 880 MB and, over a temporary table, ran out of local buffers.
 *
 * So a tree is PLANNED against a budget (LionOpenBudget) before any of its
 * cursors is built, and the plan says, node by node, how the node's cursor is
 * built:
 *
 *	- an OR whose children would together hold more memory or more pins than
 *	  the budget is WIDE: a windowed union that opens its children one at a
 *	  time and holds no pin at all between them (lion_wide_fill());
 *	- an AND whose children would together hold more PINS than the budget
 *	  keeps them on ONE child - the first whose containers all come pinned -
 *	  and every other child lets go of each posting leaf as soon as it has
 *	  copied it.  §9 asks one pin of an intersection, not one per input, and
 *	  that is the one it keeps;
 *	- everything else is built exactly as it was before the budget existed.
 *
 * The plan also says whether every container the node yields comes with a
 * live pin on the page it was copied from - `pinned`, the DESIGN.md §9
 * property of which lion_count_sources_run() needs one positive source - and
 * it is decided by the very function that decides the shape, so the two
 * cannot disagree: a WIDE union is never pinned, and a trimmed AND is pinned
 * exactly when its designated child is, which is when any child is.
 *
 * With no budget (NULL) nothing is ever wide or trimmed.  That is what a
 * plain index scan's stream gets - §29.5 relies on the pins of every OR child
 * under a non-MVCC snapshot, and its lists are batched by lion_scan.c
 * already - and it is how the disjoint-list batching of
 * lion_count_sources_run() prices a whole list opened at once.
 */
typedef struct LionNodePlan
{
	const LionKeyNode *node;	/* NULL: a source with no sets, never valid */
	int			nsub;
	struct LionNodePlan *sub;	/* the children's plans, nsub of them */
	bool		wide;			/* OR: a windowed union (lion_wide_fill()) */
	int			maximg;			/* wide: images one window may hold */
	int			keep;			/* AND: LION_KEEP_ALL, the child that keeps
								 * its pins, or LION_KEEP_NONE */
	bool		pinned;			/* every container comes with a live pin */
	Size		mem;			/* what its cursors hold open, estimated */
	int			pins;			/* posting pages its cursors pin at once */
} LionNodePlan;

#define LION_KEEP_ALL	(-1)
#define LION_KEEP_NONE	(-2)

struct LionWideOr;				/* a windowed union, below */

typedef struct LionExprCursor
{
	const LionKeyNode *node;		/* NULL: an empty source, never valid */
	const LionNodePlan *plan;	/* how it was built (lion_plan_node()) */
	LionKeyNodeKind kind;

	/* LION_KN_KEY */
	LionSetCursor leaf;

	/* LION_KN_OR planned wide: everything is in here instead */
	struct LionWideOr *wide;

	/* LION_KN_AND / LION_KN_OR */
	int			nsub;
	struct LionExprCursor *sub;
	LionContainer *acc[2];		/* AND/OR accumulators, only when nsub > 1 */

	/*
	 * LION_KN_AND, the leapfrog (lion_leapfrog()): the children in the order
	 * they are sought, fewest members first (lion_node_members()), so that
	 * sub[order[0]] drives.
	 */
	int		   *order;

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

	/*
	 * LION_KN_OR under a leapfrog (DESIGN.md §29.11, "Unions probed"): a
	 * source of the count's merge or a child of an AND node, whose container
	 * is only ever ANDed with the running intersection.  `lazy` says the
	 * union of the children standing at a key is not built when the cursor
	 * gets there; `pending` that it has not been built for the key it
	 * stands on, and cur is NULL until lion_ecursor_container() builds it -
	 * or never, when lion_leapfrog() looks the intersection's few members up
	 * in the children's containers instead (lion_or_probe()), which hotc[]
	 * holds for it.
	 */
	bool		lazy;
	bool		pending;
	const LionContainer **hotc;
	struct LionCountCtx *cx;	/* the statistics its unions are counted in */

	bool		raw;			/* its container may be left unoptimized:
								 * the count's, and a child's of an AND node
								 * or of a raw OR (lion_ecursor_init_ex()) */

	/* the container the cursor currently stands on */
	bool		valid;
	uint32		ckey;
	const LionContainer *cur;

	bool		advance;		/* top level only: took part in this key */
} LionExprCursor;

static void lion_ecursor_init(LionExprCursor *c, const LionNodePlan *plan,
							  LionPostingSet *sets, int nsets,
							  LionCountCtx *cx, bool droppins);
static void lion_ecursor_init_ex(LionExprCursor *c, const LionNodePlan *plan,
								 LionPostingSet *sets, int nsets,
								 LionCountCtx *cx, bool droppins, bool raw,
								 bool lazy);
static const LionContainer *lion_ecursor_container(LionExprCursor *c);
static bool lion_or_probe_pays(const LionExprCursor *c,
							   const LionContainer *acc);
static void lion_or_probe(LionExprCursor *c, const LionContainer *acc,
						  LionContainer *dest);
static void lion_ecursor_build(LionExprCursor *c);
static void lion_ecursor_next(LionExprCursor *c);
static void lion_ecursor_seek(LionExprCursor *c, uint32 target);
static void lion_ecursor_close(LionExprCursor *c);

/* ---- the leapfrog: the AND of cursors ---- */

/*
 * How many rows a node's sets hold, from what the located sets carry already
 * (`ntids`, the entries' own counts), so that no page is read to decide it:
 * a set's own, a union's sets together - an upper bound - and the least of an
 * intersection's children, which it cannot exceed.  It orders the sources of
 * a leapfrog (lion_leapfrog()); a wrong guess costs pages, never an answer.
 */
static double
lion_node_members(const LionKeyNode *node, const LionPostingSet *sets)
{
	double		m = 0;
	int			i;

	check_stack_depth();

	if (node == NULL)
		return 0;
	if (node->kind == LION_KN_KEY)
		return sets[node->keyno].found ? (double) sets[node->keyno].ntids : 0;
	for (i = 0; i < node->nargs; i++)
	{
		double		c = lion_node_members(node->args[i], sets);

		if (node->kind == LION_KN_OR)
			m += c;
		else if (i == 0 || c < m)
			m = c;
	}
	return m;
}

/*
 * order[0 .. n-1] = 0 .. n-1 sorted by est[], ascending.  An insertion sort,
 * because an AND has a handful of sources and this runs once per cursor; it
 * is STABLE, so of equal estimates the first comes first.
 */
static void
lion_leapfrog_order(const double *est, int n, int *order)
{
	int			i;
	int			j;

	for (i = 0; i < n; i++)
	{
		for (j = i; j > 0 && est[order[j - 1]] > est[i]; j--)
			order[j] = order[j - 1];
		order[j] = i;
	}
}

/*
 * THE LEAPFROG (DESIGN.md §22, §25): the AND of cursors, as the count's merge
 * makes it (lion_run_merge()) and as an AND node of the evaluator makes it
 * (lion_ecursor_build()) - which is what a lion index scan's AND of its quals'
 * sets is, in its stream and in its bitmap (DESIGN.md §29.2), and what the
 * count's multi-key AND trees are.  One function, so that the two cannot do
 * different work on the same sets.
 *
 * It winds cur[order[0 .. n-1]] forward to the next container key at which
 * every one of them has a container and their AND holds a member, and
 * returns that AND, its key in *key; NULL once one of them runs out, or the
 * key reaches `hi` (a chunk of a collection, LionCollect.ranged).
 *
 * order[0] is the DRIVER, the cursor with the fewest members, and the others
 * follow in ascending members.  The driver is the only one that ever steps
 * past a key; the others are left standing and are SOUGHT to the key the
 * driver has reached, which costs each one probe instead of a walk - stepping
 * all of them would cost each a container at key + 1 that the seek is about
 * to skip anyway.  They are taken in that order, each sought and folded into
 * the running intersection before the next one is touched at all, and the key
 * is ABANDONED the moment the intersection is empty, or a cursor sought lands
 * past the key: the ones after it in the order are neither sought nor read.
 * The fewer members a cursor has, the likelier it is to be the one that kills
 * the key, and the smaller the containers ANDed early are, which is what
 * keeps the dense ones - a bitset a key - out of all but the last ANDs.  Any
 * other order is still correct, and reads more.
 *
 * The AND is built in work[0] and work[1] alternately; *w comes back as the
 * one it is NOT in, for a caller that goes on with it (the merge's negated
 * sources).  The containers are left unoptimized (lion_container_and_raw()):
 * they are ANDed again, counted, or optimized once by an AND node whose
 * caller keeps what it hands out (DESIGN.md §15, "Unions and intersections
 * unoptimized").  With n = 1 it is the cursor's own container.  `stats`,
 * when not NULL, counts the seeks an abandoned key saved (EXPLAIN's Probes
 * Avoided): a cursor standing at the key or beyond would not have been
 * sought anyway, so only the ones still below it count.
 *
 * A UNION IS BUILT ONLY WHEN IT PAYS (DESIGN.md §29.11, "Unions probed").  A
 * cursor that is an IN list, a multi-key `&&` or an OR across columns stands
 * at a key with the children that have a container there, and - a lazy one,
 * which every OR under a leapfrog is - without their union built.  The
 * driver's union is built, since the intersection starts from it.  Any other
 * one's is built and ANDed with the running intersection only when that is
 * the cheaper way (lion_or_probe_pays()); otherwise the intersection's
 * members are looked up in the children's containers and the ones found
 * kept (lion_or_probe()), which is the same AND - acc ∩ (c1 ∪ ... ∪ ck) - at
 * the cost of the members the intersection has rather than of those the
 * union would have.
 *
 * THE DESIGN.md §9 PIN DISCIPLINE IS UNCHANGED, and this is the argument.
 * The rule is that the visibility-map question about a container's heap
 * blocks is asked before the pin on the page that container came from is
 * released.  A key that is abandoned asks NO such question - nothing of it
 * reaches the count - so there is no obligation to discharge for any of the
 * pages it touched, sought or not.  The cursors that are not sought keep
 * their PINS exactly where they stood: a pin too many never makes a count
 * wrong, it only makes VACUUM wait (see lion_ecursor_next()).  The pages the
 * sought ones let go of are let go by lion_ecursor_seek(), at keys nothing
 * was counted from.  And the key that is returned has every cursor standing
 * on it, each with the pin its container came with: the caller asks the map
 * before it steps the driver (lion_count_container()), and an AND node hands
 * the key up to a caller that does.  A union probed rather than built changes
 * none of it: its children were sought to the key exactly as they are for a
 * union built (lion_ecursor_seek()), the ones standing there keep the pins
 * their containers came with until the cursor moves past the key, and what
 * the probe keeps is a subset of the running intersection, whose members lie
 * in containers the cursors before it stand on with their pins.
 */
static const LionContainer *
lion_leapfrog(LionExprCursor *cur, const int *order, int n,
			  LionContainer *const *work, int *w, uint64 hi,
			  LionCountStats *stats, uint32 *key)
{
	for (;;)
	{
		const LionContainer *acc = NULL;
		uint32		target;
		int			k;
		int			m;

		for (k = 0; k < n; k++)
		{
			if (!cur[order[k]].valid)
				return NULL;	/* one ran out: so has the intersection */
		}

		target = cur[order[0]].ckey;
		for (k = 1; k < n; k++)
		{
			if (cur[order[k]].ckey > target)
				target = cur[order[k]].ckey;
		}
		if ((uint64) target >= hi)
			return NULL;

		*w = 0;
		for (k = 0; k < n; k++)
		{
			LionExprCursor *c = &cur[order[k]];

			if (c->ckey < target)
			{
				lion_ecursor_seek(c, target);
				if (!c->valid)
					return NULL;
			}

			if (c->ckey > target)
			{
				/*
				 * No container at the target at all: the key is dead, as with
				 * an empty intersection, one step earlier.  The cursors after
				 * this one are left standing, and the round starts again at
				 * the key it found, the next one that can possibly survive -
				 * the driver sought there first.
				 */
				if (stats != NULL)
				{
					for (m = k + 1; m < n; m++)
					{
						if (cur[order[m]].ckey < target)
							stats->probes_avoided++;
					}
				}
				target = c->ckey;
				if ((uint64) target >= hi)
					return NULL;
				acc = NULL;
				*w = 0;
				k = -1;
				CHECK_FOR_INTERRUPTS();
				continue;
			}

			if (acc == NULL)
				acc = lion_ecursor_container(c);	/* a union is built here */
			else
			{
				if (c->pending && lion_or_probe_pays(c, acc))
					lion_or_probe(c, acc, work[*w]);
				else
					lion_container_and_raw(acc, lion_ecursor_container(c),
										   work[*w]);
				acc = work[*w];
				*w ^= 1;
			}

			if (lion_container_cardinality(acc) == 0)
				break;
		}

		if (k >= n)
		{
			*key = target;
			return acc;
		}

		/*
		 * Abandoned: nothing of this key survives, so nothing asks the map
		 * about it and the driver's page may go.  Only the driver steps; the
		 * others still stand at or below the key it leaves and are sought
		 * from there on the next round.
		 */
		if (stats != NULL)
		{
			for (m = k + 1; m < n; m++)
			{
				if (cur[order[m]].ckey < target)
					stats->probes_avoided++;
			}
		}
		lion_ecursor_next(&cur[order[0]]);
		CHECK_FOR_INTERRUPTS();
	}
}

/* ---- planning a tree against the open budget ---- */

/*
 * What an allocation of `size` bytes really takes from an AllocSet: small
 * chunks are rounded up to a power of two, and each carries a header.  An
 * estimate - the plan needs the order of magnitude, and the rounding is what
 * made a 4104-byte staging buffer cost 8 kB.
 */
static inline Size
lion_alloc_size(Size size)
{
	if (size <= 8192)
		size = pg_nextpower2_size_t(Max(size, (Size) 8));
	return size + 16;
}

/* One leaf: its cursor, and what it copies into or pins. */
static void
lion_leaf_cost(const LionPostingSet *ps, bool droppins, Size *mem, int *pins)
{
	*mem = sizeof(LionExprCursor);
	*pins = 0;
	if (ps->found && ps->mat != NULL && lion_mat_spills(ps->mat))
	{
		/* a spilled copy: the container it reads back */
		*mem += lion_alloc_size(MAXALIGN(LION_CONTAINER_MAX_SIZE));
		return;
	}
	if (!ps->found || ps->mat != NULL)
		return;					/* nothing to walk, or a private copy */

	/* the segment buffer, on the first sparse segment (small either way) */
	*mem += lion_alloc_size(LION_CONTAINER_HDRSZ +
							LION_SPARSE_THRESHOLD * sizeof(uint16));
	if (ps->is_inline)
	{
		/* the payload is the set's own; the cursor borrows its leaf pin */
		*mem += lion_alloc_size(lion_inline_stage_size(ps->paylen));
		return;
	}
	*mem += lion_alloc_size(sizeof(PGAlignedBlock));	/* the page image */
	if (!droppins)
		*pins = 1;
}

/*
 * What each child of an OR adds to it: its heap and hot entries, and the
 * container it stands on (hotc).  lion_batch_end() sizes a batch's union by
 * the same amount, so that a batch that fits is never planned wide.
 */
#define LION_OR_CHILD_OVERHEAD \
	(2 * sizeof(LionOrHeapEnt) + sizeof(LionContainer *))

/* What an OR or AND node of nargs children adds to them. */
static inline Size
lion_node_overhead(LionKeyNodeKind kind, int nargs)
{
	Size		mem = sizeof(LionExprCursor);

	if (nargs > 1)
		mem += 2 * lion_alloc_size(LION_CONTAINER_MAX_SIZE);	/* acc[] */
	if (kind == LION_KN_AND)
		mem += (Size) nargs * sizeof(int);	/* order */
	if (kind == LION_KN_OR)
	{
		mem += (Size) nargs * LION_OR_CHILD_OVERHEAD;	/* heap, hot, hotc */
		if (nargs > 2)
			mem += lion_alloc_size(LION_BITSET_BYTES);	/* bits */
	}
	return mem;
}

/*
 * A windowed union holds its images, their keys, the container it hands out
 * and two memory contexts' first blocks; the image count is what the budget
 * pays for, half of it, so that the child open at the time has the other
 * half.
 */
#define LION_WIDE_MIN_IMAGES	8

static int
lion_wide_images(const LionOpenBudget *budget)
{
	Size		n = (budget->mem / 2) / lion_alloc_size(LION_BITSET_BYTES);

	return (int) Min(Max(n, (Size) LION_WIDE_MIN_IMAGES), (Size) (INT_MAX / 2));
}

static inline Size
lion_wide_mem(int maximg)
{
	return sizeof(LionExprCursor) + 2 * ALLOCSET_DEFAULT_INITSIZE +
		lion_alloc_size(LION_CONTAINER_MAX_SIZE) +
		(Size) maximg * (lion_alloc_size(LION_BITSET_BYTES) +
						 sizeof(uint32) + sizeof(uint64 *));
}

/*
 * Plan one node (the comment on LionNodePlan).  With build, p->sub is
 * allocated and every child is planned into it, which is what a cursor is
 * then built from; without, the children are evaluated into a scratch plan
 * and only *p's own fields are filled, which is what the questions below ask
 * (is it pinned, what would it cost) without allocating a plan per leaf of a
 * fifty-thousand-value list.  One pass over the tree either way.
 *
 * droppins says the cursors of this node will not keep their pins whatever
 * the plan says (a bitmap walk, a child of a wide union or a trimmed AND):
 * they cost no pins then, and only memory can make an OR wide.
 */
static void
lion_plan_node(LionNodePlan *p, const LionKeyNode *node,
			   const LionPostingSet *sets, const LionOpenBudget *budget,
			   bool droppins, bool build)
{
	LionNodePlan scratch;
	Size		summem;
	Size		maxmem = 0;
	int			sumpins = 0;
	int			firstpinned = -1;
	int			firstpins = 0;
	bool		allpinned = true;
	bool		anypinned = false;
	int			i;

	check_stack_depth();

	p->node = node;
	p->nsub = 0;
	p->sub = NULL;
	p->wide = false;
	p->maximg = 0;
	p->keep = LION_KEEP_ALL;
	p->pinned = true;
	p->mem = 0;
	p->pins = 0;

	if (node == NULL)
		return;					/* yields nothing: pinned vacuously */

	if (node->kind == LION_KN_KEY)
	{
		const LionPostingSet *ps = &sets[node->keyno];

		lion_leaf_cost(ps, droppins, &p->mem, &p->pins);
		/* the leaf rule of lion_source_pinned() */
		p->pinned = (!ps->found || (ps->mat == NULL && !ps->nopin));
		return;
	}

	Assert(node->nargs >= 1);
	if (build)
		p->sub = (LionNodePlan *) palloc0(sizeof(LionNodePlan) * node->nargs);
	p->nsub = node->nargs;

	summem = lion_node_overhead(node->kind, node->nargs);
	for (i = 0; i < node->nargs; i++)
	{
		LionNodePlan *cp = build ? &p->sub[i] : &scratch;

		lion_plan_node(cp, node->args[i], sets, budget, droppins, build);
		summem += cp->mem;
		maxmem = Max(maxmem, cp->mem);
		sumpins += cp->pins;
		if (cp->pinned)
		{
			anypinned = true;
			if (firstpinned < 0)
			{
				firstpinned = i;
				firstpins = cp->pins;
			}
		}
		else
			allpinned = false;
	}

	if (node->kind == LION_KN_OR)
	{
		if (budget != NULL && node->nargs > 1 &&
			(summem > budget->mem || sumpins > budget->pins))
		{
			/*
			 * Too wide to open at once: a windowed union.  Its children are
			 * opened one at a time, with no pin kept, so it holds its window
			 * and the largest child and nothing else - and no container it
			 * yields has a pin behind it, whatever its children would have
			 * had (lion_wide_fill()).
			 */
			p->wide = true;
			p->maximg = lion_wide_images(budget);
			p->mem = lion_wide_mem(p->maximg) + maxmem;
			p->pins = 0;
			p->pinned = false;
		}
		else
		{
			p->mem = summem;
			p->pins = sumpins;
			/* which children contributed is not known in advance: all */
			p->pinned = allpinned;
		}
		return;
	}

	Assert(node->kind == LION_KN_AND);
	p->mem = summem;
	/* every child stands at the key the result was built from: any one */
	p->pinned = anypinned;
	if (budget != NULL && !droppins && sumpins > budget->pins)
	{
		/*
		 * One pin is all the intersection needs, so only the first child that
		 * has one at every key keeps its pins; with none that does, no child
		 * does, because nothing they hold could carry the interlock anyway.
		 */
		p->keep = (firstpinned >= 0) ? firstpinned : LION_KEEP_NONE;
		p->pins = (firstpinned >= 0) ? firstpins : 0;
	}
	else
		p->pins = sumpins;
}

/* A node's plan, children and all, allocated in the current context. */
static LionNodePlan *
lion_plan_build(const LionKeyNode *node, const LionPostingSet *sets,
				const LionOpenBudget *budget, bool droppins)
{
	LionNodePlan *p = (LionNodePlan *) palloc0(sizeof(LionNodePlan));

	lion_plan_node(p, node, sets, budget, droppins, true);
	return p;
}

/* ---- the windowed union: an OR too wide to open at once ---- */

/*
 * A WIDE OR NODE (DESIGN.md §15, "Bounded cursors").
 *
 * The k-way merge below opens every child for the whole walk.  A wide union
 * opens them ONE AT A TIME instead: for a window of container keys it reads
 * each child, in turn, from the window's first key up to its end, ORs what it
 * finds into one bitset image per container key, and closes the child before
 * the next is opened.  The images then come out in ascending key order,
 * exactly as the merge would have produced them, and the next window starts
 * at the smallest key any child had past this one.  What it holds is the
 * window and one child's cursors, whatever the number of children: the
 * window's images are at most maximg - half the budget - and its END is
 * not fixed in advance but moves down to whatever key the images run out at,
 * so a sparse union over a vast heap is still ONE window and a dense one is
 * as many as it has to be.
 *
 * The price is that a child is opened once per window instead of once, and
 * the pins.  Every child is walked with droppins, so a container the union
 * yields has no pin behind it at all: a wide union is never `pinned`, and
 * DESIGN.md §9 needs another positive source to carry the interlock for it,
 * or the count rechecks every candidate in the heap (cx.novm), exactly as it
 * does for a set located past the list pin budget.  The disjoint lists that
 * make most wide unions are not left to this: lion_count_sources_run()
 * counts them in pinned batches instead, and a union only goes wide where a
 * batch would not be exact - an OR across columns (§19), a multi-key OR
 * (§17), the bitmap walk of a multicolumn index.
 *
 * Correctness of the window, the part worth arguing:
 *
 *	- every child's containers below wend were ORed into the images, because
 *	  a child is only abandoned at its first key at or past wend;
 *	- wend only ever moves DOWN while the window is filled.  When the images
 *	  are all in use and a key arrives that has none, the largest key the
 *	  window holds is evicted and becomes wend - or, if the new key is larger
 *	  still, the new key does - so every image left is below the new wend,
 *	  and whatever an earlier child had between the new wend and the old one
 *	  was that evicted key alone (it was the largest image);
 *	- nextkey is the smallest key at or past wend that any child had: each
 *	  child reports the key it stopped at, and an eviction reports the
 *	  evicted key, which is below every key an earlier child stopped at.
 *
 * So the next window, started at nextkey, misses nothing, and no key is ever
 * handed out twice because windows never overlap.
 */
#define LION_WIDE_END	(((uint64) PG_UINT32_MAX) + 1)	/* past every key */

typedef struct LionWideOr
{
	MemoryContext cxt;			/* this struct, the images, the arrays */
	MemoryContext childcxt;		/* the one child cursor open at a time */
	LionPostingSet *sets;
	int			nsets;
	LionCountCtx *cx;
	int			maximg;
	int			nimg;			/* images of this window, keys ascending */
	int			nalloc;			/* images allocated; img[nimg..] are free */
	uint32	   *keys;
	uint64	  **img;
	int			pos;			/* the image the cursor stands on */
	uint64		wend;			/* the window ends before this key */
	uint64		nextkey;		/* where the next window starts, or END */
	LionContainer *out;			/* img[pos] as a container */
} LionWideOr;

/*
 * The image of `key`, made if it has none; NULL when the window is full and
 * the key is past everything it holds, in which case the key is the new end.
 */
static uint64 *
lion_wide_image(LionWideOr *w, uint32 key)
{
	int			lo = 0;
	int			hi = w->nimg;
	uint64	   *img;

	while (lo < hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (w->keys[mid] < key)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo < w->nimg && w->keys[lo] == key)
		return w->img[lo];

	if (w->nimg >= w->maximg)
	{
		uint32		last = w->keys[w->nimg - 1];

		if (key > last)
		{
			/* the window ends here; this key starts the next one */
			w->wend = key;
			if (key < w->nextkey)
				w->nextkey = key;
			return NULL;
		}

		/* the largest key leaves the window and ends it; its image is free */
		w->wend = last;
		if (last < w->nextkey)
			w->nextkey = last;
		w->nimg--;
	}

	if (w->nimg < w->nalloc)
		img = w->img[w->nimg];	/* the first free image */
	else
	{
		img = (uint64 *) MemoryContextAlloc(w->cxt, LION_BITSET_BYTES);
		w->nalloc++;
	}
	memset(img, 0, LION_BITSET_BYTES);

	memmove(&w->keys[lo + 1], &w->keys[lo], sizeof(uint32) * (w->nimg - lo));
	memmove(&w->img[lo + 1], &w->img[lo], sizeof(uint64 *) * (w->nimg - lo));
	w->keys[lo] = key;
	w->img[lo] = img;
	w->nimg++;
	return img;
}

/*
 * Where a windowed union's keys end: past every key, or where the chunk a
 * collection is making ends (LionCollect.ranged) - no window reads its
 * children past that, and the union is over there.
 */
static inline uint64
lion_wide_last(const LionWideOr *w)
{
	if (w->cx->collect != NULL && w->cx->collect->ranged)
		return Min(w->cx->collect->hi, LION_WIDE_END);
	return LION_WIDE_END;
}

/*
 * Fill the window that starts at `start` (see the comment on LionWideOr).
 * Each child is built in childcxt with droppins, read up to the window's end,
 * closed, and its memory reset before the next one is built.
 */
static void
lion_wide_fill(LionExprCursor *c, uint64 start)
{
	LionWideOr *w = c->wide;
	const LionNodePlan *plan = c->plan;
	uint64		last = lion_wide_last(w);
	int			i;

	Assert(start < LION_WIDE_END);
	w->nimg = 0;
	w->pos = 0;
	w->wend = last;
	w->nextkey = LION_WIDE_END;

	for (i = 0; i < plan->nsub; i++)
	{
		LionExprCursor sub;
		MemoryContext oldcxt = MemoryContextSwitchTo(w->childcxt);

		lion_ecursor_init(&sub, &plan->sub[i], w->sets, w->nsets, w->cx, true);
		if (sub.valid && (uint64) sub.ckey < start)
			lion_ecursor_seek(&sub, (uint32) start);

		while (sub.valid)
		{
			uint64	   *img;

			if ((uint64) sub.ckey >= w->wend)
			{
				if ((uint64) sub.ckey < w->nextkey &&
					(uint64) sub.ckey < last)
					w->nextkey = sub.ckey;
				break;
			}
			img = lion_wide_image(w, sub.ckey);
			if (img == NULL)
				break;			/* the key is the new end, and next */
			lion_container_or_into_bitset(sub.cur, img);
			lion_ecursor_next(&sub);
			CHECK_FOR_INTERRUPTS();
		}

		lion_ecursor_close(&sub);
		MemoryContextSwitchTo(oldcxt);
		MemoryContextReset(w->childcxt);
		CHECK_FOR_INTERRUPTS();
	}
}

static void
lion_wide_init(LionExprCursor *c, LionPostingSet *sets, int nsets,
			   LionCountCtx *cx)
{
	MemoryContext cxt;
	LionWideOr *w;

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"lion index union window",
								ALLOCSET_DEFAULT_SIZES);
	w = (LionWideOr *) MemoryContextAllocZero(cxt, sizeof(LionWideOr));
	w->cxt = cxt;
	w->childcxt = AllocSetContextCreate(cxt, "lion index union child",
										ALLOCSET_DEFAULT_SIZES);
	w->sets = sets;
	w->nsets = nsets;
	w->cx = cx;
	w->maximg = Max(c->plan->maximg, 1);
	w->keys = (uint32 *) MemoryContextAlloc(cxt, sizeof(uint32) * w->maximg);
	w->img = (uint64 **) MemoryContextAllocZero(cxt,
												sizeof(uint64 *) * w->maximg);
	w->out = (LionContainer *) MemoryContextAlloc(cxt, LION_CONTAINER_MAX_SIZE);
	c->wide = w;

	/* the first window: a collection's chunk starts at its own first key */
	lion_wide_fill(c, (cx->collect != NULL && cx->collect->ranged) ?
				   cx->collect->lo : 0);
}

/* Stand on the first non-empty image at or after pos, filling as needed. */
static void
lion_wide_build(LionExprCursor *c)
{
	LionWideOr *w = c->wide;

	for (;;)
	{
		for (; w->pos < w->nimg; w->pos++)
		{
			lion_bits_to_container(w->img[w->pos], w->keys[w->pos], w->out);
			if (lion_container_cardinality(w->out) > 0)
			{
				c->ckey = w->keys[w->pos];
				c->cur = w->out;
				c->valid = true;
				return;
			}
		}
		if (w->nextkey >= LION_WIDE_END)
			return;				/* every child is exhausted */
		lion_wide_fill(c, w->nextkey);
	}
}

/* Skip every key below target; lion_ecursor_seek() rebuilds afterwards. */
static void
lion_wide_seek(LionExprCursor *c, uint32 target)
{
	LionWideOr *w = c->wide;

	if ((uint64) target < w->wend)
	{
		while (w->pos < w->nimg && w->keys[w->pos] < target)
			w->pos++;
	}
	else
	{
		/* past this window: the next one starts at the target, or later */
		w->pos = w->nimg;
		if (w->nextkey < LION_WIDE_END && w->nextkey < (uint64) target)
			w->nextkey = target;
	}
}

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
 * The containers are ORed into the image by lion_container_or_into_bitset(),
 * the container library's own: this file used to carry a copy of it, which
 * lacked the library's masks and wrote up to 12 KiB past the image for a
 * damaged container (2026-09-27 review).
 *
 * Turn the accumulated image into a container in dest (capacity
 * LION_CONTAINER_MAX_SIZE), in the smallest representation, exactly as
 * lion_container_or() would have left it.
 */
void
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

/*
 * The union of the containers standing at one key, for a count (c->raw;
 * DESIGN.md §15, "Unions and intersections unoptimized"), whose merge ANDs
 * it with the other sources and counts it and never keeps it.  Unoptimized:
 *
 *	- ARRAYs of LION_OR_FOLD_MEMBERS members or fewer together are folded
 *	  pairwise, each union a merge of two arrays and nothing else - the
 *	  fold's cost grows with the containers times their members, so only
 *	  while those are small;
 *	- anything else is ORed into one bitset image, written in place as the
 *	  payload of a BITSET container and counted once.  The image costs a
 *	  fixed 4 kB clear and count, which a union of a hundred-odd members
 *	  already repays: the pairwise fold of four dense ARRAYs merged, counted
 *	  its runs and converted to a RUN three times over at every key, only for
 *	  the AND after it to fill it back into a bitset; and three ARRAYs of 110
 *	  members, folded in two merges, took a sixth longer than their image on
 *	  the repro of §15's measurements.
 *
 * Which of the two the old rule took depended on the containers alone
 * (LION_OR_BITSET_MIN), which is still where a fold stops.
 */
#define LION_OR_FOLD_MEMBERS	128

static const LionContainer *
lion_or_hot_raw(LionExprCursor *c, uint32 ckey)
{
	const LionContainer *a;
	uint32		members = 0;
	bool		arrays = true;
	int			w = 0;
	int			i;

	for (i = 0; i < c->nhot; i++)
	{
		members += lion_container_cardinality(c->hot[i].cur);
		if (c->hot[i].cur->type != LION_CT_ARRAY)
			arrays = false;
	}

	if (arrays && members <= LION_OR_FOLD_MEMBERS &&
		c->nhot < LION_OR_BITSET_MIN)
	{
		a = c->hot[0].cur;
		for (i = 1; i < c->nhot; i++)
		{
			lion_container_or_raw(a, c->hot[i].cur, c->acc[w]);
			a = c->acc[w];
			w ^= 1;
		}
		return a;
	}

	lion_container_bitset_init(c->acc[0], ckey);
	for (i = 0; i < c->nhot; i++)
		lion_container_or_into_bitset(c->hot[i].cur,
									  LION_BITSET_DATA(c->acc[0]));
	(void) lion_container_bitset_recount(c->acc[0]);
	return c->acc[0];
}

/*
 * Build the cursor of one planned node (lion_plan_node()).  droppins: carry
 * no interlock anywhere below - a child of a wide union, a child of a
 * trimmed AND other than the one that keeps its pins.  Its container is
 * what cx->raw says: unoptimized for a count, optimized for a stream, whose
 * caller copies it.
 */
static void
lion_ecursor_init(LionExprCursor *c, const LionNodePlan *plan,
				 LionPostingSet *sets, int nsets, LionCountCtx *cx,
				 bool droppins)
{
	lion_ecursor_init_ex(c, plan, sets, nsets, cx, droppins, cx->raw, false);
}

/*
 * ... with raw: its containers may be left unoptimized - a count's, and those
 * of every child of an AND node, which the leapfrog ANDs and never hands on
 * as they are (the node optimizes what it hands up when it is not raw
 * itself); and lazy: under a leapfrog, whose OR is not built until it is
 * asked for (lion_ecursor_container(), DESIGN.md §29.11, "Unions probed").
 * An OR's children are what the OR is: its union of one child is that
 * child's container.
 */
static void
lion_ecursor_init_ex(LionExprCursor *c, const LionNodePlan *plan,
					 LionPostingSet *sets, int nsets, LionCountCtx *cx,
					 bool droppins, bool raw, bool lazy)
{
	const LionKeyNode *node = plan->node;
	int			i;

	check_stack_depth();

	memset(c, 0, sizeof(LionExprCursor));
	c->node = node;
	c->plan = plan;
	c->raw = raw;
	c->cx = cx;
	if (node == NULL)
		return;					/* a source with no sets at all */

	c->kind = node->kind;
	c->lazy = lazy && node->kind == LION_KN_OR && !plan->wide;

	if (node->kind == LION_KN_KEY)
	{
		Assert(node->keyno >= 0 && node->keyno < nsets);
		lion_cursor_init(&c->leaf, &sets[node->keyno], cx, droppins);
	}
	else if (plan->wide)
	{
		Assert(node->kind == LION_KN_OR);
		lion_wide_init(c, sets, nsets, cx);
	}
	else
	{
		/* the children of a leapfrog: raw, and lazy where they are unions */
		bool		under = (node->kind == LION_KN_AND && node->nargs > 1);

		Assert(node->nargs >= 1 && plan->nsub == node->nargs);
		c->nsub = node->nargs;
		c->sub = (LionExprCursor *) palloc0(sizeof(LionExprCursor) * c->nsub);
		for (i = 0; i < c->nsub; i++)
			lion_ecursor_init_ex(&c->sub[i], &plan->sub[i], sets, nsets, cx,
								 droppins ||
								 (plan->keep != LION_KEEP_ALL && plan->keep != i),
								 under || raw, under);

		if (c->nsub > 1)
		{
			c->acc[0] = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
			c->acc[1] = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
		}

		if (node->kind == LION_KN_AND)
		{
			/* the leapfrog's order: fewest members first (lion_leapfrog()) */
			double	   *est = (double *) palloc(sizeof(double) * c->nsub);

			for (i = 0; i < c->nsub; i++)
				est[i] = lion_node_members(node->args[i], sets);
			c->order = (int *) palloc(sizeof(int) * c->nsub);
			lion_leapfrog_order(est, c->nsub, c->order);
			pfree(est);
		}

		if (node->kind == LION_KN_OR)
		{
			c->hot = (LionOrHeapEnt *) palloc(sizeof(LionOrHeapEnt) * c->nsub);
			c->heap = (LionOrHeapEnt *) palloc(sizeof(LionOrHeapEnt) * c->nsub);
			if (c->nsub > 2)
				c->bits = (uint64 *) palloc(LION_BITSET_BYTES);
			if (c->lazy)
				c->hotc = (const LionContainer **)
					palloc(sizeof(LionContainer *) * c->nsub);

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
 * The union of the children standing at the key, c->hot[] (at least two), in
 * c->cur - unoptimized for a raw cursor (lion_or_hot_raw()), else in the
 * smallest representation, as a stream's caller copies it.
 */
static void
lion_or_build(LionExprCursor *c)
{
	int			w = 0;
	int			i;

	Assert(c->kind == LION_KN_OR && c->nhot > 1);
	c->cx->stats.unions_built++;
	c->pending = false;

	if (c->raw)
		c->cur = lion_or_hot_raw(c, c->ckey);
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
			lion_container_or_into_bitset(c->hot[i].cur, c->bits);
		lion_bits_to_container(c->bits, c->ckey, c->acc[0]);
		c->cur = c->acc[0];
	}
}

/*
 * The container the cursor stands on, the union of a lazy OR's children
 * built if it has not been (lion_leapfrog()).
 */
static const LionContainer *
lion_ecursor_container(LionExprCursor *c)
{
	Assert(c->valid);
	if (c->pending)
		lion_or_build(c);
	return c->cur;
}

/*
 * BUILD THE UNION, OR PROBE ITS MEMBERS (DESIGN.md §29.11, "Unions probed").
 *
 * The AND of a running intersection acc with the union of the containers a
 * lazy OR's children have at the key can be made two ways, with the same
 * members as the result: build the union (lion_or_build()) and AND acc with
 * it, or look acc's members up in each child's container in turn, a member
 * once found not again, and keep the ones found (lion_or_probe(),
 * lion_container_and_union_raw()).  The first costs every member of the
 * union - a bitset image cleared, filled and counted, or for a few small
 * ARRAYs a fold of merges - and then the AND; the second costs acc's members
 * a child - a bit test in a BITSET, a galloping search through an ARRAY, a
 * search of a RUN's runs or a merge with them - and acc's extraction when
 * acc is not an ARRAY.  So the probe wins where the intersection is small
 * against the union - a few rows of a selective AND against a list's
 * containers of hundreds or thousands - and the union where the intersection
 * is dense, or the list's containers are small.
 *
 * Which is cheaper is estimated here from what the containers' headers say,
 * in quarters of a nanosecond as a harness beside the container library
 * measured them (-O2, containers of a heap of 70 rows a block, members at
 * every offset a row can have): a bit test 2 ns; a gallop through an ARRAY
 * of s members 2 ns a step there and 3 in a count, whose containers are not
 * all in the first-level cache, 1 + log2(1 + s/a) steps a member of acc; a
 * search of r runs 1.5 ns a step, a merge with them 0.75 ns a run and 3 a
 * member; and for the union a member of an ARRAY 1 ns, a BITSET 170, a run 2,
 * the image's clear and count 150 (52 ns of which is the count on PostgreSQL
 * 18's AVX-512 popcount; 16's takes 670), a fold 1.25 ns a member a fold,
 * and acc's AND with it 1.5 ns a member.  The estimate is integer
 * arithmetic and its logarithms fixed point (lion_log2_16()): it is made at
 * every key a union is met at, and made in floating point it took 2 to 3%
 * of a count that builds its unions anyway.  Rounded to whole steps instead,
 * it took 120 members against two ARRAYs of 90 for one step each where they
 * take 1.8, probed them, and the count that built its union in 2.8 ms took
 * 4.2.  It takes every member of acc to be looked up in every child, where
 * one found is not looked up again: it errs towards the union.  An acc of
 * more than an ARRAY's worth of members is never probed.
 */
#define LION_UP_BIT			8	/* quarters of a nanosecond */
#define LION_UP_GALLOP		12
#define LION_UP_RUNSEARCH	6
#define LION_UP_RUNMERGE	3
#define LION_UP_RUNMERGE_A	12
#define LION_UP_EXTRACT		400 /* a BITSET's 512 words scanned */
#define LION_UP_MEMBER_OR	4
#define LION_UP_BITSET_OR	680
#define LION_UP_RUN_OR		8
#define LION_UP_IMAGE		600
#define LION_UP_FOLD		5
#define LION_UP_AND			6

/* GUC pg_lion.enable_union_probe (DESIGN.md §29.11, "Unions probed") */
bool		lion_enable_union_probe = true;

/*
 * 16 log2(x) for x >= 1, within a sixteenth or so: the leading bit's position
 * and the four bits after it, as a straight line between two powers of two.
 */
static inline uint32
lion_log2_16(uint32 x)
{
	int			k = pg_leftmost_one_pos32(x);
	uint32		rest = x - ((uint32) 1 << k);

	return (uint32) k * 16 + ((k >= 4) ? rest >> (k - 4) : rest << (4 - k));
}

static bool
lion_or_probe_pays(const LionExprCursor *c, const LionContainer *acc)
{
	uint64		a = lion_container_cardinality(acc);
	uint64		probe = 0;
	uint64		unite = 0;
	uint64		members = 0;
	bool		arrays = true;
	int			i;

	Assert(c->pending && c->nhot > 1);
	if (!lion_enable_union_probe || a > LION_ARRAY_MAX_CARD)
		return false;
	a = Max(a, 1);

	if (acc->type == LION_CT_BITSET)
		probe += LION_UP_EXTRACT + 4 * a;
	else if (acc->type == LION_CT_RUN)
		probe += 4 * a;

	for (i = 0; i < c->nhot; i++)
	{
		const LionContainer *h = c->hot[i].cur;
		uint64		m = lion_container_cardinality(h);

		members += m;
		if (h->type == LION_CT_BITSET)
		{
			probe += LION_UP_BIT * a;
			unite += LION_UP_BITSET_OR;
			arrays = false;
		}
		else if (h->type == LION_CT_RUN)
		{
			uint32		r = Min((uint32) LION_RUN_NRUNS((LionContainer *) h),
								(uint32) LION_RUN_MAX_NRUNS);

			probe += ((uint64) r >= 8 * a) ?
				LION_UP_RUNSEARCH * a * lion_log2_16(r + 1) / 16 :
				LION_UP_RUNMERGE * r + LION_UP_RUNMERGE_A * a;
			unite += LION_UP_RUN_OR * (uint64) r;
			arrays = false;
		}
		else
		{
			/* 1 + log2(1 + m/a) steps: 16 + 16 log2(16 + 16 m/a) - 64 sixteenths */
			probe += LION_UP_GALLOP * a *
				(lion_log2_16((uint32) (16 * m / a) + 16) - 48) / 16;
			unite += LION_UP_MEMBER_OR * m;
		}
	}

	/* lion_or_hot_raw()'s two ways, and acc's AND with what it builds */
	if (arrays && members <= LION_OR_FOLD_MEMBERS &&
		c->nhot < LION_OR_BITSET_MIN)
		unite = LION_UP_FOLD * members * (uint64) c->nhot +
			LION_UP_AND * (a + members);
	else
		unite += LION_UP_IMAGE +
			((acc->type == LION_CT_ARRAY) ? LION_UP_AND * a : LION_UP_IMAGE);

	return probe < unite;
}

/*
 * dest = acc AND the union of c->hot[], without building it: acc's members
 * looked up in the children's containers (lion_container_and_union_raw()).
 * The union stays unbuilt; nothing else asks for it at this key.
 */
static void
lion_or_probe(LionExprCursor *c, const LionContainer *acc, LionContainer *dest)
{
	int			i;

	Assert(c->pending && c->hotc != NULL);
	for (i = 0; i < c->nhot; i++)
		c->hotc[i] = c->hot[i].cur;
	(void) lion_container_and_union_raw(acc, c->hotc, (uint32) c->nhot, dest);
	c->cx->stats.unions_probed++;
}

/*
 * Recompute the cursor's current container from its children.
 */
static void
lion_ecursor_build(LionExprCursor *c)
{
	const LionContainer *acc;
	int			w = 0;

	c->valid = false;
	c->pending = false;
	c->cur = NULL;

	if (c->node == NULL)
		return;

	if (c->wide != NULL)
	{
		lion_wide_build(c);
		return;
	}

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

		c->ckey = minckey;
		c->valid = true;

		/*
		 * The union of one child is its container.  A lazy cursor's of more
		 * is built when lion_leapfrog() asks for it, which it does only for
		 * the driver and where building it is the cheaper way to AND it
		 * (DESIGN.md §29.11, "Unions probed").
		 */
		if (c->nhot == 1)
			c->cur = c->hot[0].cur;
		else if (c->lazy)
			c->pending = true;
		else
			lion_or_build(c);
		return;
	}

	Assert(c->kind == LION_KN_AND);

	/*
	 * The children, wound forward to the next key their AND holds a member
	 * at, as the count's merge winds its sources (lion_leapfrog(), DESIGN.md
	 * §22): the one with the fewest members drives, the others are sought to
	 * its keys in ascending members, and a key is abandoned as soon as the
	 * intersection is empty.  Nothing of the keys they pass over ever reaches
	 * the visibility map, so the §9 rule is untouched - this is the same
	 * window the code has always wound laggards forward in.
	 */
	acc = lion_leapfrog(c->sub, c->order, c->nsub, c->acc, &w, LION_WIDE_END,
						NULL, &c->ckey);
	if (acc == NULL)
		return;

	/*
	 * The AND was made of unoptimized containers, and the one handed up is
	 * optimized here when the caller copies it (a stream's, DESIGN.md
	 * §29.3) - once, rather than at every step of the AND as it was.  With
	 * two children or more it is in c->acc[].
	 */
	if (!c->raw && c->nsub > 1)
	{
		Assert(acc == c->acc[0] || acc == c->acc[1]);
		lion_container_optimize((LionContainer *) acc);
	}
	c->cur = acc;
	c->valid = true;
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

	if (c->wide != NULL)
	{
		/* the next image; it holds no pin to let go of */
		c->wide->pos++;
		lion_ecursor_build(c);
		return;
	}

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
			 * Every child stands at c->ckey, but only ONE of them has to step,
			 * the driver: lion_ecursor_build() then finds it ahead of the
			 * others and SEEKS them to it (DESIGN.md §22), which is one probe
			 * each instead of a walk.  Stepping all of them would cost every
			 * child a container at c->ckey + 1 that the seek is about to skip
			 * anyway.
			 */
			lion_ecursor_next(&c->sub[c->order[0]]);
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

	if (c->wide != NULL)
	{
		lion_wide_seek(c, target);
		lion_ecursor_build(c);
		return;
	}

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

			/*
			 * Only the driver: lion_ecursor_build() seeks the others to
			 * wherever it lands, one at a time, and not at all past a child
			 * that rules the key out (lion_leapfrog()).
			 */
			lion_ecursor_seek(&c->sub[c->order[0]], target);
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

	if (c->wide != NULL)
	{
		/* no child is open between two calls, and none holds a pin */
		MemoryContextDelete(c->wide->cxt);
		c->wide = NULL;
	}
	else if (c->kind == LION_KN_KEY)
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
 *	  contributed to a given container key is not known in advance;
 *	- and an OR too wide for the open budget has it never: it is read as a
 *	  windowed union whose children keep no pin (DESIGN.md §15, "Bounded
 *	  cursors").
 *
 * That is lion_plan_node()'s `pinned`, and it is asked of it rather than
 * computed again here, so that what the count trusts and how the cursors are
 * built are one decision.  budget is the one the cursors will be built with;
 * NULL means the whole tree opened at once, as a batch of a disjoint list is.
 */
static bool
lion_source_pinned(const LionKeyNode *node, const LionPostingSet *sets,
				   const LionOpenBudget *budget)
{
	LionNodePlan p;

	lion_plan_node(&p, node, sets, budget, false, false);
	return p.pinned;
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
 * visibilitymap_get_status() leaves it - and each move adds one to *pins, when
 * pins is not NULL (EXPLAIN ANALYZE's "Visibility Map Pages Pinned").
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
					   Buffer *vmbuf, int64 *pins)
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
	{
		(void) visibilitymap_get_status(heap, blk, vmbuf);
		if (pins != NULL)
			(*pins)++;
	}

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
							 int n, Buffer *vmbuf, int64 *pins)
{
	uint64		mask = 0;
	uint64		seg;

	Assert(n > 0 && n < LION_BLOCKS_PER_CONTAINER);
	Assert(n % LION_VM_HEAPBLOCKS_PER_BYTE == 0);

	seg = wanted & ((UINT64CONST(1) << n) - 1);
	if (seg != 0)
		mask |= lion_vm_allvisible_page(heap, firstblk, seg, 0, vmbuf, pins);

	seg = wanted >> n;
	if (seg != 0)
		mask |= lion_vm_allvisible_page(heap, firstblk + (BlockNumber) n, seg,
									   n, vmbuf, pins);

	return mask;
}

static pg_always_inline uint64
lion_vm_allvisible_mask(Relation heap, BlockNumber firstblk, uint64 wanted,
					   Buffer *vmbuf, int64 *pins)
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
		return lion_vm_allvisible_mask_split(heap, firstblk, wanted, n, vmbuf,
											pins);

	return lion_vm_allvisible_page(heap, firstblk, wanted, 0, vmbuf, pins);
}

/*
 * The other half of the pair - which of the heap blocks a container covers
 * have at least one member, in the same bit numbering - is the container
 * library's lion_container_block_mask().  This file used to carry its own,
 * which took a damaged container's members and runs unmasked: past the range
 * they shifted by 64 or more, which is undefined and could drop a block with
 * members from the mask, and at BLCKSZ 16K and 32K they set bits past the
 * flags lion_count_container_vm() keeps per block (2026-09-27 review).
 */

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
 * The comparison below on the blocks of ONE map page, with that page SHARE
 * locked.  `seg` are the wanted bits, shifted so that bit 0 is block blk.
 */
static void
lion_vm_mask_check_page(Relation heap, BlockNumber blk, uint64 seg)
{
	Buffer		vmbuf = InvalidBuffer;
	uint64		expect = 0;
	uint64		got;
	uint64		m = seg;

	if (seg == 0)
		return;

	/* pin the map page, the way visibilitymap_get_status() does */
	(void) visibilitymap_get_status(heap, blk, &vmbuf);
	if (!BufferIsValid(vmbuf))
		return;					/* the fork does not reach it: nothing set */

	LockBuffer(vmbuf, BUFFER_LOCK_SHARE);
	got = lion_vm_allvisible_page(heap, blk, seg, 0, &vmbuf, NULL);
	while (m != 0)
	{
		int			b = pg_rightmost_one_pos64(m);

		m &= m - 1;
		/* the same page: visibilitymap_get_status() keeps the pin, reads */
		if ((visibilitymap_get_status(heap, blk + (BlockNumber) b, &vmbuf) &
			 VISIBILITYMAP_ALL_VISIBLE) != 0)
			expect |= UINT64CONST(1) << b;
	}
	LockBuffer(vmbuf, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(vmbuf);

	/* the mask answers whole map bytes: only the wanted bits are compared */
	Assert((got & seg) == expect);
}

/*
 * visibilitymap_get_status() for every block that has members - the only bits
 * of the mask the count looks at - must agree with the mask.
 *
 * It legitimately might not: both read the map without a lock, and a
 * concurrent VACUUM or DML may change a bit between the two reads - even
 * twice, all-visible to not and back (A to B to A), which a second unlocked
 * read of the mask cannot tell from no change at all, so re-reading the mask
 * and comparing only when it came out the same was not enough (2026-09-25
 * review).  So a disagreement is settled by comparing the two again with the
 * map page SHARE locked: every writer of a map bit - visibilitymap_set(),
 * visibilitymap_clear(), 19's visibilitymap_set_vmbits() - holds the page
 * EXCLUSIVE, so under the share lock both read the same bytes and must agree,
 * and what is compared is what the check is for, the two functions.  The lock
 * is only ever taken in an assert build, on a disagreement, with no other
 * buffer locked; a pin on the heap's index pages is all the caller holds.
 */
static void
lion_vm_mask_check(Relation heap, BlockNumber firstblk, uint64 members,
				  uint64 allvis, Buffer *vmbuf)
{
	uint64		expect = 0;
	uint64		m = members;
	int			n;

	while (m != 0)
	{
		int			b = pg_rightmost_one_pos64(m);

		m &= m - 1;
		if ((visibilitymap_get_status(heap, firstblk + (BlockNumber) b, vmbuf) &
			 VISIBILITYMAP_ALL_VISIBLE) != 0)
			expect |= UINT64CONST(1) << b;
	}

	if ((allvis & members) == expect)
		return;

	/* a container's blocks lie on one map page, or on two (see above) */
	n = (int) (LION_VM_HEAPBLOCKS_PER_PAGE -
			   (firstblk % LION_VM_HEAPBLOCKS_PER_PAGE));
	if (n >= LION_BLOCKS_PER_CONTAINER)
		lion_vm_mask_check_page(heap, firstblk, members);
	else
	{
		lion_vm_mask_check_page(heap, firstblk,
								members & ((UINT64CONST(1) << n) - 1));
		lion_vm_mask_check_page(heap, firstblk + (BlockNumber) n,
								members >> n);
	}
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

	/*
	 * The row filter every count of this node execution applies, or NULL
	 * (lion_vis_cache_set_filter()).  It is the caller's and outlives no
	 * relation it names; a reset leaves it alone.
	 */
	LionRowFilter *filter;

	/*
	 * What one count of this node execution leaves the next (DESIGN.md §27,
	 * "The per-key path, end to end"), so that a count's fixed cost is not
	 * paid again for every key of an FK-side join, every group of a GROUP
	 * BY:
	 *
	 *	scratch		the memory a count works in - its cursors and their
	 *				staging buffers, the merge's two work containers, the
	 *				plans, the recheck list - made once, in `parent`, with a
	 *				first block large enough for an ordinary count, and RESET
	 *				when a count is over rather than made and deleted by every
	 *				count.  scratchbusy while a count uses it: a count made
	 *				inside another (none is, today) makes its own.
	 *	vmbuf		the visibility-map page the last count left pinned, of
	 *				relation vmrelid, which the next count of that relation
	 *				starts from instead of pinning it again - between the rows
	 *				of a node as well, as an index-only scan keeps its map page
	 *				(ioss_VMBuffer).  It is a map page, not an index page: no
	 *				part of the §9 interlock, and no VACUUM waits for its pin
	 *				(setting a bit takes the page's content lock, and only a
	 *				truncation, under an AccessExclusiveLock this query's lock
	 *				excludes, would drop it).  The holder of the cache lets go
	 *				of it when it is done, rescans, ends or moves to another
	 *				relation (lion_vis_cache_release_vm()); a reset and a
	 *				destroy do too.
	 */
	MemoryContext parent;
	MemoryContext scratch;
	bool		scratchbusy;
	Buffer		vmbuf;
	Oid			vmrelid;
};

/*
 * The first block of a count's scratch memory (LionVisCache.scratch): the two
 * work containers of a merge and a spilled copy's staging buffer are 8 kB
 * chunks each, and a count of a few sources holds some 30 kB in all.
 */
#define LION_COUNT_SCRATCH_BLOCK	(64 * 1024)

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
	cache->parent = parent;
	cache->scratch = NULL;
	cache->scratchbusy = false;
	cache->vmbuf = InvalidBuffer;
	cache->vmrelid = InvalidOid;
	return cache;
}

/*
 * Let go of the visibility-map page the counts kept pinned from one to the
 * next (LionVisCache.vmbuf): when the node is done with the relation.
 */
void
lion_vis_cache_release_vm(LionVisCache *cache)
{
	if (cache == NULL || !BufferIsValid(cache->vmbuf))
		return;
	ReleaseBuffer(cache->vmbuf);
	cache->vmbuf = InvalidBuffer;
	cache->vmrelid = InvalidOid;
}

void
lion_vis_cache_reset(LionVisCache *cache)
{
	if (cache == NULL)
		return;
	lion_vis_cache_release_vm(cache);
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
	lion_vis_cache_release_vm(cache);
	cache->ht = NULL;
	cache->filter = NULL;
	if (cache->cxt != NULL)
		MemoryContextDelete(cache->cxt);
	cache->cxt = NULL;
	if (cache->scratch != NULL)
		MemoryContextDelete(cache->scratch);
	cache->scratch = NULL;
	cache->scratchbusy = false;
}

void
lion_vis_cache_set_filter(LionVisCache *cache, LionRowFilter *filter)
{
	if (cache == NULL)
	{
		if (filter != NULL)
			elog(ERROR, "lion index count: a row filter needs a visibility cache");
		return;
	}
	cache->filter = filter;
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
 * Copy one container of a collected intersection (DESIGN.md §27).  The merge
 * hands them over in ascending container key order, one per key, which is the
 * order a LionMatSet keeps.  A copy that would outgrow its budget is given up
 * on: failed is set, the merge stops at the next container boundary
 * (lion_exists_settled()), and the caller counts the ordinary way instead.
 *
 * The merge's results come unoptimized (LionCountCtx.raw): what is kept is
 * the smallest representation, as the optimizing set algebra used to leave
 * it, so that a copy takes what it always took.
 */
static void
lion_collect_container(LionCollect *col, const LionContainer *c)
{
	union
	{
		LionContainer hdr;
		uint64		align;
		char		data[LION_CONTAINER_MAX_SIZE];
	}			opt;
	Size		sz;
	MemoryContext oldcxt;
	int			i;

	if (col->failed)
		return;
	if (lion_container_cardinality(c) == 0)
		return;

	memcpy(opt.data, c, lion_container_size(c));
	lion_container_optimize(&opt.hdr);
	c = &opt.hdr;
	sz = lion_item_size(c);
	if (!col->spilled &&
		sizeof(LionMatSet) + MAXALIGN(col->used + sz) +
		(sizeof(LionContainer *) + sizeof(uint32)) * (col->noffs + 1) >
		col->maxbytes)
	{
		if (!col->spill)
		{
			col->failed = true;
			return;
		}

		/*
		 * Past the memory, and allowed to SPILL: what the copy holds so far
		 * goes to a temporary file, and so does everything after it.
		 */
		lion_spill_begin(&col->sp, col->cxt);
		for (i = 0; i < col->noffs; i++)
			lion_spill_add(&col->sp,
						   (const LionContainer *) (col->buf + col->offs[i]));
		pfree(col->buf);
		pfree(col->offs);
		col->buf = NULL;
		col->offs = NULL;
		col->spilled = true;
	}
	if (col->spilled)
	{
		lion_spill_add(&col->sp, c);
		col->members += lion_container_cardinality(c);
		return;
	}

	oldcxt = MemoryContextSwitchTo(col->cxt);
	while (col->used + sz > col->cap)
	{
		col->cap *= 2;
		col->buf = (char *) repalloc(col->buf, col->cap);
	}
	if (col->noffs >= col->offcap)
	{
		col->offcap *= 2;
		col->offs = (Size *) repalloc(col->offs, sizeof(Size) * col->offcap);
	}
	MemoryContextSwitchTo(oldcxt);

	memcpy(col->buf + col->used, c, sz);
	col->offs[col->noffs++] = col->used;
	col->used += MAXALIGN(sz);
	col->members += lion_container_cardinality(c);
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

	/* A collection copies the container and asks nothing (DESIGN.md §27). */
	if (cx->collect != NULL)
	{
		lion_collect_container(cx->collect, c);
		return;
	}

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
	else if (cx->filter != NULL)
		allvis = 0;				/* the map vouches for visibility, and every
								 * row has to be tested as well */
	else
	{
		allvis = lion_vm_allvisible_mask(cx->heap, firstblk, members,
										&cx->vmbuf, &cx->stats.vm_pins);
		cx->stats.vm_checks++;
#ifdef LION_VM_MASK_CHECK
		lion_vm_mask_check(cx->heap, firstblk, members, allvis, &cx->vmbuf);
#endif
	}
	lion_count_container_masks(cx, c, members, allvis);
}

/*
 * Step 2 of counting a container, once the visibility map has been asked
 * about its blocks under the pins of DESIGN.md section 9: `members` has the
 * bit of every heap block c has a member on (lion_container_block_mask()),
 * `allvis` those the map marks all-visible.  Count the members of those
 * blocks outright, and queue the others' TIDs for the heap recheck.  The
 * grouped walk (lion_count_groups_copy()) comes here too, with one answer of
 * the map for all the groups' containers at a key.
 */
static void
lion_count_container_masks(LionCountCtx *cx, const LionContainer *c,
						   uint64 members, uint64 allvis)
{
	BlockNumber firstblk = lion_ckey_first_block(c->ckey);
	uint64		dirty;			/* blocks with members that need a heap recheck */

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

		/*
		 * Only a damaged container can hold more members in some blocks than
		 * its header says it holds in all of them (DESIGN.md §3: the header's
		 * cardinality is a claim).  Its count is then wrong either way, but
		 * not by four billion, and not by an assertion failure.
		 */
		if (unlikely(dirty_members > lion_container_cardinality(c)))
			dirty_members = lion_container_cardinality(c);

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
 * Is ckey past the chunk a collection is making (LionCollect.ranged)?  Never
 * for a count, or a collection of the whole intersection.
 */
static inline bool
lion_collect_past(const LionCountCtx *cx, uint32 ckey)
{
	return cx->collect != NULL && cx->collect->ranged &&
		(uint64) ckey >= cx->collect->hi;
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
 * A MATERIALIZED set has none either: it is a private copy, which is only
 * ever counted while another source of the same intersection carries the
 * interlock (lion_posting_set_materialize()).  No caller hands one over on
 * its own today - the copies are made for the WHERE sets of a GROUP BY, and a
 * lone set is never copied - but nothing would stop one (2026-09-25 review),
 * and counting it from the visibility map would be exactly the stale read §9
 * forbids.  So it is counted from its CHAIN instead, walked page by page
 * under the cursor's own pins as any located CHAIN set is: the head is the
 * posting tree's root, which a set keeps for the life of the index (§18),
 * and a page that no longer belongs to the set ends the walk.
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
	bool		relocated = false;

	if (ps->nopin)
	{
		if (!lion_posting_set_relocate(ps, &fresh))
			return;
		ps = &fresh;
		relocated = true;
	}
	else if (ps->mat != NULL)
	{
		Assert(ps->found && !ps->is_inline);

		/*
		 * A COLLECTED intersection (lion_sources_collect()) has no chain to
		 * fall back on, and is only ever COUNTED beside a located set that
		 * carries the interlock (DESIGN.md §27).  A collection is no count:
		 * it asks the visibility map nothing and needs no pin
		 * (lion_count_container_vm()), so it copies such a set as it stands.
		 * That is what the FK-side join does when its one fact filter is a
		 * range already collected into memory (§32): one source of one set,
		 * the shape this function is the shortcut for.  It used to be refused
		 * here, and the join's first row failed.
		 */
		if (!BlockNumberIsValid(ps->head))
		{
			if (cx->collect == NULL)
				elog(ERROR, "lion index: a collected posting set counted on its own");
		}
		else
		{
			fresh = *ps;
			fresh.mat = NULL;	/* the chain itself, not the copy */
			fresh.budgeted = false; /* nothing of it is the list's to return */
			ps = &fresh;
		}
	}

	/* a chunk of a collection (ranged): from its first key, up to its end */
	lion_cursor_init_at(&cur, ps, cx, false,
						(cx->collect != NULL && cx->collect->ranged) ?
						cx->collect->lo : 0, NULL);
	while (cur.valid)
	{
		if (lion_collect_past(cx, cur.cur->ckey))
			break;
		lion_count_container_vm(cx, cur.cur);
		lion_cursor_next(&cur);
		if (lion_exists_settled(cx))
			break;
		CHECK_FOR_INTERRUPTS();
	}
	lion_cursor_close(&cur);

	if (relocated)
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
	/* A collection that outgrew its budget stops the same way (§27). */
	if (cx->collect != NULL)
		return cx->collect->failed;

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

bool
lion_row_filter_test(LionRowFilter *filter, HeapTuple tuple)
{
	TupleDesc	desc = RelationGetDescr(filter->heap);
	MemoryContext oldcxt = MemoryContextSwitchTo(filter->tmpcxt);
	bool		pass = true;
	int			i;

	for (i = 0; i < filter->nclauses && pass; i++)
	{
		LionRowFilterClause *c = &filter->clauses[i];
		Datum		d;
		bool		isnull;

		d = heap_getattr(tuple, c->attno, desc, &isnull);
		if (isnull)
			pass = false;		/* a strict operator, or IS NOT NULL */
		else if (!c->notnull)
		{
			LOCAL_FCINFO(fcinfo, 2);
			Datum		result;

			InitFunctionCallInfoData(*fcinfo, &c->flinfo, 2, c->collation,
									 NULL, NULL);
			fcinfo->args[0].value = d;
			fcinfo->args[0].isnull = false;
			fcinfo->args[1].value = c->value;
			fcinfo->args[1].isnull = false;
			result = FunctionCallInvoke(fcinfo);
			pass = (!fcinfo->isnull && DatumGetBool(result));
		}
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextReset(filter->tmpcxt);
	return pass;
}

/*
 * lion_recheck_heap_heap() for a count with a row filter (DESIGN.md §17, "A
 * query known only at run time"): the row the snapshot sees is not only
 * found, it is tested, and counted when it passes.
 *
 * The chain of each candidate is resolved under the share lock exactly as
 * there - heap_hot_search_buffer(), its serializable conflict checks and
 * tuple predicate locks included - and the offsets of the visible members
 * are noted.  The filter runs after the lock is released, on the tuples the
 * pin alone keeps in place, which is what core's page-at-a-time heap scans do
 * (heap_prepare_pagescan()): nothing moves a tuple while another backend holds
 * a pin, since pruning and defragmentation need the cleanup lock, and the
 * operator it calls may read TOAST or run for a while, neither of which may
 * happen under a buffer content lock.
 *
 * The visibility cache is neither read nor filled: it knows which offsets
 * are visible, but the filter needs their tuples.
 */
static int64
lion_recheck_heap_filtered(LionCountCtx *cx)
{
	LionRowFilter *filter = cx->filter;
	OffsetNumber vis[MaxHeapTuplesPerPage];
	int64		passed = 0;
	int			i = 0;

	while (i < cx->ntids)
	{
		BlockNumber blk = ItemPointerGetBlockNumber(&cx->tids[i]);
		Buffer		buf;
		Page		page;
		int			nvis = 0;
		int			j;

		buf = ReadBuffer(cx->heap, blk);
		lion_heap_page_prune_opt(cx->heap, buf, &cx->vmbuf, cx->rel_read_only);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		cx->stats.blocks_rechecked++;

		do
		{
			ItemPointerData tid = cx->tids[i];	/* callee updates it */
			HeapTupleData heapTuple;

			if (heap_hot_search_buffer(&tid, cx->heap, buf, cx->snapshot,
									   &heapTuple, NULL, true))
			{
				/* one visible member per chain, one chain per candidate */
				if (nvis >= MaxHeapTuplesPerPage)
					elog(ERROR, "lion index count: more visible tuples than a heap page holds");
				vis[nvis++] = ItemPointerGetOffsetNumber(&tid);
			}
			i++;
		} while (i < cx->ntids &&
				 ItemPointerGetBlockNumber(&cx->tids[i]) == blk);

		LockBuffer(buf, BUFFER_LOCK_UNLOCK);

		page = BufferGetPage(buf);
		for (j = 0; j < nvis; j++)
		{
			ItemId		lp = PageGetItemId(page, vis[j]);
			HeapTupleData tuple;

			tuple.t_data = (HeapTupleHeader) PageGetItem(page, lp);
			tuple.t_len = ItemIdGetLength(lp);
			tuple.t_tableOid = RelationGetRelid(cx->heap);
			ItemPointerSet(&tuple.t_self, blk, vis[j]);

			if (lion_row_filter_test(filter, &tuple))
				passed++;
			else
				cx->stats.rows_removed++;
		}

		ReleaseBuffer(buf);
		CHECK_FOR_INTERRUPTS();
	}

	return passed;
}

int64
lion_count_heap_filtered(Relation heap, Snapshot snapshot,
						 LionRowFilter *filter, LionCountStats *stats)
{
	TableScanDesc scan;
	HeapTuple	tuple;
	BlockNumber lastblk = InvalidBlockNumber;
	int64		count = 0;

	if (filter == NULL || filter->heap != heap)
		elog(ERROR, "lion index count: a heap scan without its row filter");

	/*
	 * An ordinary sequential scan under the count's snapshot: it takes the
	 * relation's predicate lock under SERIALIZABLE and prunes on access as
	 * any scan does.  table_beginscan_strat() rather than table_beginscan(),
	 * whose arguments changed in PostgreSQL 19; these are its defaults.
	 */
	scan = table_beginscan_strat(heap, snapshot, 0, NULL, true, true);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		BlockNumber blk = ItemPointerGetBlockNumber(&tuple->t_self);

		if (blk != lastblk)
		{
			stats->blocks_rechecked++;
			lastblk = blk;
		}
		stats->tids_rechecked++;
		if (lion_row_filter_test(filter, tuple))
			count++;
		else
			stats->rows_removed++;
		CHECK_FOR_INTERRUPTS();
	}
	table_endscan(scan);

	return count;
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

	/*
	 * A filtered count reads the tuples themselves, which only the heap AM
	 * gives it that way; the count pushdown refuses every other table AM
	 * before it gets here (DESIGN.md §10).
	 */
	if (cx->filter != NULL)
	{
		if (cx->heap->rd_tableam != GetHeapamTableAmRoutine())
			elog(ERROR, "lion index count: a row filter over a table that is not a heap");
		visible = lion_recheck_heap_filtered(cx);
	}
	else if (cx->heap->rd_tableam == GetHeapamTableAmRoutine())
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
 * The collected copy a count's other source is, when it is one: a source of
 * one set, a copy lion_sources_collect() made (LionMatSet.collected).  NULL
 * for anything else.
 */
static const LionPostingSet *
lion_source_collected(const LionCountSource *src, const LionNodePlan *plan)
{
	const LionPostingSet *ps;

	if (src->negated || src->nsets != 1 || plan->node == NULL ||
		plan->node->kind != LION_KN_KEY)
		return NULL;
	ps = &src->sets[plan->node->keyno];
	if (!ps->found || ps->mat == NULL || !ps->mat->collected)
		return NULL;
	return ps;
}

/*
 * Container i of a collected copy, where it is: in memory, or read back from
 * the spilled copy's file into buf (its first container is kept in memory).
 * What EXPLAIN ANALYZE counts of it is what a cursor over it would count.
 */
static inline const LionContainer *
lion_mat_container(LionCountCtx *cx, const LionMatSet *mat, int i,
				   LionContainer *buf)
{
	cx->stats.containers_visited++;
	cx->stats.copy_containers++;
	if (lion_mat_inmem(mat, i))
		return mat->containers[i];
	if (i == 0 && mat->first != NULL)
		return mat->first;
	lion_spill_read(mat, i, buf);
	cx->stats.copy_file_reads++;
	return buf;
}

/*
 * THE MERGE OF A SET AND A COLLECTED COPY (DESIGN.md §27, "The copy, looked
 * up by key"): lion_run_merge() for its commonest pair, the key's set of an
 * FK-side join - or a group's set, §10 - driving, and the copy of the WHERE
 * sources that every one of those counts is ANDed with.  The copy is an array
 * of containers in key order, never changed once made, so it needs no cursor:
 * each of the driver's containers looks its key up in it (lion_mat_seek(),
 * direct where the copy's keys are dense) and is ANDed with the container
 * found there, and where the copy has none at that key the driver is sought
 * to the next key it has, as the leapfrog of the general merge would seek it.
 * What that saves is the general merge's bookkeeping round every container
 * of the key: a cursor over the copy sought, advanced and rebuilt, the
 * sources' keys compared, their order walked.
 *
 * The §9 rule reads as in lion_count_container(): a container of the result
 * is put to the visibility map before the driver - the one source that
 * carries a pin - moves past it, and a key the copy has nothing at, or
 * whose AND is empty, asks the map nothing and lets the driver go on.  The
 * copy holds no pin and is only ever counted beside the driver's pinned
 * containers, which is lion_sources_collect()'s argument.
 */
static void
lion_run_merge_copy(LionCountCtx *cx, LionCountSource *drvsrc,
					const LionNodePlan *drvplan, const LionPostingSet *copy)
{
	const LionMatSet *mat = copy->mat;
	LionExprCursor drv;
	LionContainer *work;
	LionContainer *buf = NULL;
	int			idx = 0;

	work = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	if (lion_mat_spills(mat))
		buf = (LionContainer *) palloc(MAXALIGN(LION_CONTAINER_MAX_SIZE));

	lion_ecursor_init(&drv, drvplan, drvsrc->sets, drvsrc->nsets, cx, false);
	while (drv.valid)
	{
		uint32		key;

		cx->stats.copy_seeks++;
		idx = lion_mat_seek(mat, idx, drv.ckey);
		if (idx >= mat->ncontainers)
			break;				/* nothing of the copy at or past the key */
		key = lion_mat_key(mat, idx);
		if (key != drv.ckey)
		{
			/* the copy has nothing here: on to the next key it has (§22) */
			lion_ecursor_seek(&drv, key);
			CHECK_FOR_INTERRUPTS();
			continue;
		}

		if (lion_container_and_raw(drv.cur,
								   lion_mat_container(cx, mat, idx, buf),
								   work) > 0)
		{
			/* the map is asked before the driver lets its page go */
			lion_count_container_vm(cx, work);
			lion_ecursor_next(&drv);
			if (lion_exists_settled(cx))
				break;
		}
		else
			lion_ecursor_next(&drv);
		CHECK_FOR_INTERRUPTS();
	}
	lion_ecursor_close(&drv);

	pfree(work);
	if (buf != NULL)
		pfree(buf);
}

/*
 * One pass of the merge: intersect the positive sources container key by
 * container key, subtract the negated ones, and count what is left against the
 * visibility map.  Everything the caller set up in *cx - the recheck batch, the
 * visibility map pin, the visibility cache, the statistics - is accumulated
 * into, so this may be called more than once for one count: the batches of a
 * disjoint list below are the caller that does (lion_run_batches()).  Each
 * source's cursor is built from its plan (lion_plan_node()); cx->novm must
 * already say whether any positive source's plan is pinned.
 *
 * This is where DESIGN.md §9 lives; nothing about the interlock changes with
 * how often it runs, because each pass takes its pins, asks the visibility map
 * and drops them again inside lion_count_container().
 */
static void
lion_run_merge(LionCountCtx *cx, int nsources, LionCountSource *sources,
			  LionNodePlan **plans)
{
	LionExprCursor *cursors;
	LionContainer *work[2];
	double	   *est;			/* members of each positive source */
	int		   *probeord;		/* the positive sources, least first */
	int			nprobe = 0;
	int			driver;
	uint64		hi;				/* the merge ends before this key */
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
	 * A set against a collected copy - what an FK-side join's every count
	 * is, and a GROUP BY's once its WHERE is collected - with the set
	 * driving: the copy is looked up at the set's keys, and needs no cursor
	 * (lion_run_merge_copy()).  A collection is a merge of the sources
	 * themselves, never of a copy.
	 */
	if (nsources == 2 && nprobe == 2 && cx->collect == NULL)
	{
		const LionPostingSet *copy = lion_source_collected(&sources[probeord[1]],
														   plans[probeord[1]]);

		if (copy != NULL)
		{
			lion_run_merge_copy(cx, &sources[driver], plans[driver], copy);
			pfree(cursors);
			pfree(est);
			pfree(probeord);
			return;
		}
	}

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

	/*
	 * The positive sources are lazy: a union among them is built only when
	 * the leapfrog finds that cheaper than probing its members (DESIGN.md
	 * §29.11, "Unions probed").  A negated one is subtracted whole.
	 */
	for (i = 0; i < nsources; i++)
		lion_ecursor_init_ex(&cursors[i], plans[i], sources[i].sets,
							 sources[i].nsets, cx, false, cx->raw,
							 !sources[i].negated);

	/*
	 * A chunk of a shared copy (LionCollect.ranged) begins at its first key:
	 * the positive sources are sought there - a collection holds no pin, so
	 * nothing they pass over carries an answer - and the merge ends where the
	 * chunk does (`hi`, below).  The negated ones are sought when they are
	 * needed, as always.
	 */
	if (cx->collect != NULL && cx->collect->ranged && cx->collect->lo > 0)
	{
		for (i = 0; i < nsources; i++)
		{
			if (!sources[i].negated)
				lion_ecursor_seek(&cursors[i], cx->collect->lo);
		}
	}

	/*
	 * Past a container key only the DRIVER steps; the others are left standing
	 * where they are and are sought to wherever the driver has got to
	 * (DESIGN.md §22).  A cursor that keeps its place also keeps its pin,
	 * which is a pin too many and never a wrong answer (see
	 * lion_ecursor_next()).
	 */
	for (i = 0; i < nsources; i++)
		cursors[i].advance = false;
	cursors[driver].advance = true;
	hi = (cx->collect != NULL && cx->collect->ranged) ?
		cx->collect->hi : LION_WIDE_END;

	/*
	 * Merge the sources by container key.  Containers are stored in ascending
	 * ckey order both inline and along a chain, and an expression cursor
	 * preserves that, so a single forward pass over all of them is enough.
	 */
	for (;;)
	{
		uint32		maxckey;
		const LionContainer *acc;
		int			w;

		/*
		 * THE INTERSECTION OF THE POSITIVE SOURCES, BUILT AS THEY ARE SOUGHT
		 * (DESIGN.md §25): the leapfrog every AND of posting sets is made by,
		 * a lion index scan's included (lion_leapfrog(), which carries the §9
		 * argument).  The sources are taken in probe order, each sought to the
		 * driver's key and folded into the accumulator before the next one is
		 * touched at all, and a key is abandoned - the sources after it in the
		 * order not sought, the pages their probes would have read not read -
		 * the moment the accumulator is empty.  Before §25 the whole round of
		 * seeks was made first and the intersection looked at afterwards,
		 * which read a dense source's leaf at every container key the driver
		 * produced, including the ones where the selective sources had
		 * already ruled the key out.  A chunk of a collection ends at its last
		 * key (lion_collect_past()).
		 */
		acc = lion_leapfrog(cursors, probeord, nprobe, work, &w, hi,
							&cx->stats, &maxckey);
		if (acc == NULL)
			goto merge_done;

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
				lion_container_andnot_raw(acc, cursors[i].cur, work[w]);
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
 * is precisely the merge - but the same disjointness still lets the merge take
 * the list a BATCH of entries at a time and add the passes up
 * (lion_run_batches()).
 */
static bool
lion_source_disjoint_list(const LionCountSource *src)
{
	Relation	index = NULL;
	AttrNumber	attno = 0;
	int			i;

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

static bool
lion_sources_disjoint_sum(int nsources, const LionCountSource *sources)
{
	return nsources == 1 && lion_source_disjoint_list(&sources[0]);
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
 * THE BATCHED DISJOINT LIST (DESIGN.md §15, "Bounded cursors").
 *
 * The disjoint sum counts a list's entries one at a time, and only when the
 * list is the only positive source.  Anywhere else - `k = ANY ($1) AND x = 1`,
 * a dense list the sum would be slow for, a GROUP BY on another column - the
 * list is a source of the merge, and the merge used to build a cursor for
 * every entry at once: 17 kB each (8 since the staging buffers were sized)
 * and, for a CHAIN entry, a buffer pin from the moment it was built.  A list
 * of 50000 values held 880 MB whatever work_mem said, and a temporary table's
 * list of 1100 CHAIN entries ran out of local buffers.
 *
 * The disjointness that makes the sum exact makes batches exact too.  With
 * the list's sets S_1 .. S_n pairwise disjoint and cut into batches whose
 * unions are U_1 .. U_m, R the intersection of the other positive sources and
 * N the union of the negated ones,
 *
 *		|((U_1 ∪ ... ∪ U_m) ∩ R) \ N|  =  Σ_j |(U_j ∩ R) \ N|
 *
 * because the (U_j ∩ R) \ N are pairwise disjoint and together make up the
 * left side.  So the ordinary merge runs once per batch, with the list
 * replaced by that batch's union, and the passes add up - into cx, which
 * every pass accumulates into anyway.
 *
 * DESIGN.md §9 needs no new argument.  Each pass is an ordinary merge: it
 * builds its cursors, asks the visibility map under their pins and closes
 * them before the next pass builds any.  A batch is sized to fit the open
 * budget, so it is never planned wide, and its union is pinned exactly when
 * each of its sets is; whether a pass may trust the map is decided per pass,
 * from that pass's plans.  Across passes the count shares only what the
 * disjoint sum already shares - the recheck queue, the visibility-map pin,
 * the visibility cache - and no TID is in two batches, a scalar index holding
 * a row under exactly one entry.  (A line pointer VACUUM frees between two
 * passes and an insert reuses under another entry is a row inserted after our
 * snapshot: invisible to the heap recheck, and its page cannot be all-visible
 * while our snapshot is registered.)
 *
 * The price is that every other source is read once per pass.  A batch is as
 * large as the budget allows, so an ordinary list is one pass - no batching at
 * all, lion_batch_source() says -1 - and nothing about it changes.
 */

/*
 * The source, if any, that is a disjoint list too large to open at once: the
 * one whose cursors would hold the most memory, opened whole.  -1 if none.
 */
static int
lion_batch_source(int nsources, const LionCountSource *sources,
				  LionKeyNode **trees, const LionOpenBudget *budget)
{
	int			best = -1;
	Size		bestmem = 0;
	int			i;

	for (i = 0; i < nsources; i++)
	{
		LionNodePlan p;

		if (!lion_source_disjoint_list(&sources[i]))
			continue;
		lion_plan_node(&p, trees[i], sources[i].sets, NULL, false, false);
		if (p.mem <= budget->mem && p.pins <= budget->pins)
			continue;
		if (best < 0 || p.mem > bestmem)
		{
			best = i;
			bestmem = p.mem;
		}
	}
	return best;
}

/*
 * Where the batch that starts at sets[start] ends: as many sets as the open
 * budget allows, and at least LION_BATCH_MIN_SETS that have an entry.  The
 * sets without one are left out of the batch's union and cost nothing.  The
 * price is what lion_plan_node() charges for an OR over the found ones, plus
 * the accumulators it charges only from two or three children on, so that a
 * batch that fits is never planned wide.
 */
static int
lion_batch_end(const LionCountSource *src, int start,
			   const LionOpenBudget *budget)
{
	Size		mem = lion_node_overhead(LION_KN_OR, 3) -
		3 * LION_OR_CHILD_OVERHEAD;
	int			pins = 0;
	int			n = 0;
	int			end;

	for (end = start; end < src->nsets; end++)
	{
		Size		m;
		int			p;

		if (!src->sets[end].found)
			continue;
		lion_leaf_cost(&src->sets[end], false, &m, &p);
		m += LION_OR_CHILD_OVERHEAD;
		if (n >= LION_BATCH_MIN_SETS &&
			(mem + m > budget->mem || pins + p > budget->pins))
			break;
		mem += m;
		pins += p;
		n++;
	}
	return end;
}

/*
 * Count the merge of sources[] with sources[b], a disjoint list, taken a
 * batch at a time (see above).  trees[] are the sources' own; the list's is
 * rebuilt per batch over the batch's found sets.
 */
static void
lion_run_batches(LionCountCtx *cx, int nsources, LionCountSource *sources,
				 LionKeyNode **trees, int b, const LionOpenBudget *budget)
{
	LionCountSource *list = &sources[b];
	LionCountSource *pass;
	LionNodePlan **plans;
	MemoryContext passcxt;
	int			start = 0;
	int			i;

	/* The other sources are planned once: their plans do not change. */
	plans = (LionNodePlan **) palloc0(sizeof(LionNodePlan *) * nsources);
	for (i = 0; i < nsources; i++)
	{
		if (i != b)
			plans[i] = lion_plan_build(trees[i], sources[i].sets, budget, false);
	}
	pass = (LionCountSource *) palloc(sizeof(LionCountSource) * nsources);
	memcpy(pass, sources, sizeof(LionCountSource) * nsources);

	/*
	 * Everything one pass builds - the batch's tree and plan, and every
	 * cursor of the merge - lives here and is gone before the next pass.
	 */
	passcxt = AllocSetContextCreate(CurrentMemoryContext,
									"lion index count batch",
									ALLOCSET_DEFAULT_SIZES);

	while (start < list->nsets)
	{
		int			end = lion_batch_end(list, start, budget);
		MemoryContext oldcxt;
		LionKeyNode **args;
		int			nargs = 0;
		int			ncarry = 0;

		oldcxt = MemoryContextSwitchTo(passcxt);

		/* the batch's union, over the sets that have an entry */
		args = (LionKeyNode **) palloc(sizeof(LionKeyNode *) * (end - start));
		for (i = start; i < end; i++)
		{
			if (!list->sets[i].found)
				continue;
			args[nargs] = (LionKeyNode *) palloc0(sizeof(LionKeyNode));
			args[nargs]->kind = LION_KN_KEY;
			args[nargs]->keyno = i - start;
			nargs++;
		}

		if (nargs > 0)
		{
			LionKeyNode *tree = args[0];

			if (nargs > 1)
			{
				tree = (LionKeyNode *) palloc0(sizeof(LionKeyNode));
				tree->kind = LION_KN_OR;
				tree->nargs = nargs;
				tree->args = args;
			}
			pass[b].sets = &list->sets[start];
			pass[b].nsets = end - start;
			pass[b].tree = tree;
			plans[b] = lion_plan_build(tree, pass[b].sets, budget, false);
			Assert(!plans[b]->wide);

			/* §9: does this pass have a positive source that carries it? */
			for (i = 0; i < nsources; i++)
			{
				if (!pass[i].negated && plans[i]->pinned)
					ncarry++;
			}
			cx->novm = (ncarry == 0);

			lion_run_merge(cx, nsources, pass, plans);
		}

		MemoryContextSwitchTo(oldcxt);
		MemoryContextReset(passcxt);
		start = end;

		/* An existence test needs one batch with a visible row (§26). */
		if (lion_exists_settled(cx))
			break;
		CHECK_FOR_INTERRUPTS();
	}

	MemoryContextDelete(passcxt);
	pfree(pass);
	pfree(plans);
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
									bool exists, LionCollect *collect);

int64
lion_count_sources_cached(Relation heap, Snapshot snapshot, int nsources,
						 LionCountSource *sources, LionCountStats *stats,
						 LionVisCache *cache, bool rel_read_only)
{
	return lion_count_sources_run(heap, snapshot, nsources, sources, stats,
								  cache, rel_read_only, false, NULL);
}

bool
lion_exists_sources_cached(Relation heap, Snapshot snapshot, int nsources,
						  LionCountSource *sources, LionCountStats *stats,
						  LionVisCache *cache, bool rel_read_only)
{
	return lion_count_sources_run(heap, snapshot, nsources, sources, stats,
								  cache, rel_read_only, true, NULL) > 0;
}

/*
 * The intersection of the positive sources minus the negated ones, as a
 * private posting set (DESIGN.md §27): the containers the merge of a count
 * would have put through the visibility map, copied into memory instead, and
 * nothing asked of the map or the heap.  *out comes back a found, pinless,
 * materialized set of no index entry of its own - head is invalid - and holds
 * every row of the intersection, visible to the caller's snapshot or not.  It
 * is what the FK-side join intersects with each dimension row's fk set, where
 * the merge of the fact filters would otherwise be built again at every one
 * of that set's container keys, per dimension row.
 *
 * A copy made this way is a stale copy by the time it is counted, exactly as
 * a materialized set is, and it is safe on exactly the same terms (the
 * argument is on lion_posting_set_materialize()): it may only ever be counted
 * beside a located set that carries the DESIGN.md §9 interlock - a dead TID
 * it still lists is then either gone from that set's container, or on a heap
 * block whose all-visible bit VACUUM cannot have set yet - and it cannot be
 * missing a row the snapshot sees, because it is read after the snapshot was
 * taken and a visible row was in every index before its transaction
 * committed.  lion_count_one_set() refuses to count one on its own.
 *
 * Returns false, with *out not found and nothing allocated, when the copy
 * would take more than maxbytes.  The caller then counts the ordinary way.  A
 * list too long to open at once is read as a windowed union, not in the
 * batches a count takes it in (lion_count_sources_run()).  The copy is
 * allocated in the current memory context.
 *
 * With spill, a copy past maxbytes goes on in a temporary file instead
 * (LionSpill, 2026-09-28 review): what memory then keeps is sixteen bytes a
 * container, and a count reads the file a container at a time.  The ordinary
 * way it would otherwise fall back to seeks every source at every container
 * key of every located set it is counted beside - for a long IN list among
 * the sources, a union of all its sets built again for each of them.
 *
 * It returns false as well where no source has a found set to copy from.
 */
static bool lion_sources_collect_keys(Relation heap, Snapshot snapshot,
									  int nsources, LionCountSource *sources,
									  Size maxbytes, bool spill, bool ranged,
									  uint32 lo, uint64 hi,
									  LionPostingSet *out, bool *spilled,
									  LionCountStats *stats);

bool
lion_sources_collect(Relation heap, Snapshot snapshot, int nsources,
					 LionCountSource *sources, Size maxbytes, bool spill,
					 LionPostingSet *out, bool *spilled, LionCountStats *stats)
{
	return lion_sources_collect_keys(heap, snapshot, nsources, sources,
									 maxbytes, spill, false, 0, 0,
									 out, spilled, stats);
}

/*
 * The same copy of the container keys from lo up to, not including, hi alone
 * (LION_KEYS_END: every key from lo on) - one range of lion_key_ranges()'s
 * cut, which one participant of a parallel GROUP BY counts its groups against
 * (DESIGN.md §10, "A GROUP BY in parallel").  The sources are sought to lo and
 * the merge stops at hi, as for a chunk of a shared copy (LionCollect.ranged);
 * what it holds is the whole copy's containers at those keys, on the same
 * terms.
 */
bool
lion_sources_collect_range(Relation heap, Snapshot snapshot, int nsources,
						   LionCountSource *sources, Size maxbytes, bool spill,
						   uint32 lo, uint64 hi, LionPostingSet *out,
						   bool *spilled, LionCountStats *stats)
{
	return lion_sources_collect_keys(heap, snapshot, nsources, sources,
									 maxbytes, spill, true, lo, hi,
									 out, spilled, stats);
}

static bool
lion_sources_collect_keys(Relation heap, Snapshot snapshot, int nsources,
						  LionCountSource *sources, Size maxbytes, bool spill,
						  bool ranged, uint32 lo, uint64 hi,
						  LionPostingSet *out, bool *spilled,
						  LionCountStats *stats)
{
	LionCollect col;
	LionMatSet *mat;
	Relation	index = NULL;
	int			i;
	int			j;

	memset(out, 0, sizeof(LionPostingSet));
	out->pinbuf = InvalidBuffer;
	out->head = InvalidBlockNumber;
	*spilled = false;

	for (i = 0; i < nsources && index == NULL; i++)
	{
		for (j = 0; j < sources[i].nsets; j++)
		{
			if (sources[i].sets[j].found)
			{
				index = sources[i].sets[j].index;
				break;
			}
		}
	}
	if (index == NULL)
		return false;

	/* the catalogs a spill's file needs, looked up before the merge starts */
	if (spill)
		PrepareTempTablespaces();

	memset(&col, 0, sizeof(col));
	col.cxt = CurrentMemoryContext;
	col.maxbytes = maxbytes;
	col.spill = spill;
	col.ranged = ranged;
	col.lo = lo;
	col.hi = hi;
	col.cap = 8192;
	col.buf = (char *) palloc(col.cap);
	col.offcap = 256;
	col.offs = (Size *) palloc(sizeof(Size) * col.offcap);

	(void) lion_count_sources_run(heap, snapshot, nsources, sources, stats,
								  NULL, false, false, &col);

	if (col.failed)
	{
		pfree(col.buf);
		pfree(col.offs);
		return false;
	}

	/*
	 * An empty intersection is a set that selects nothing, which is how a key
	 * with no entry reads: not found.
	 */
	out->index = index;
	out->attno = 1;
	out->cxt = CurrentMemoryContext;
	out->entryblk = InvalidBlockNumber;
	out->entryoff = InvalidOffsetNumber;
	if (col.spilled)
	{
		mat = lion_spill_finish(&col.sp);

		/*
		 * Every count against the copy - the FK-side join's per dimension
		 * key, a GROUP BY's per group - builds a cursor over it, and a cursor
		 * starts at the first container: keep that one in memory, so that a
		 * count reads the file once, for the container it is sought to, and
		 * not twice.
		 */
		lion_spill_keep_first(mat, CurrentMemoryContext);

		/* ... and seek it by its keys, directly where they are dense */
		lion_mat_index(mat, CurrentMemoryContext, 0);
		mat->collected = true;
		out->found = true;
		out->mat = mat;
		out->ntids = col.members;
		out->ncontainers = (uint32) mat->ncontainers;
		*spilled = true;
		return true;
	}
	if (col.noffs == 0)
	{
		pfree(col.buf);
		pfree(col.offs);
		return true;
	}

	mat = (LionMatSet *) palloc0(sizeof(LionMatSet));
	mat->ncontainers = col.noffs;
	mat->bytes = col.used;
	mat->held = sizeof(LionMatSet) + MAXALIGN(Max(col.used, (Size) 1)) +
		sizeof(LionContainer *) * Max(col.noffs, 1);
	mat->buf = col.buf;
	mat->containers = (LionContainer **)
		palloc(sizeof(LionContainer *) * Max(col.noffs, 1));
	for (i = 0; i < col.noffs; i++)
		mat->containers[i] = (LionContainer *) (col.buf + col.offs[i]);
	pfree(col.offs);

	/*
	 * The keys, which the budget above made room for, and the direct index
	 * where they are dense and it fits what is left: every count against the
	 * copy seeks it at each container of its own key's set.
	 */
	lion_mat_index(mat, CurrentMemoryContext,
				   maxbytes > mat->held ? maxbytes - mat->held : 0);
	mat->collected = true;

	out->found = true;
	out->mat = mat;
	out->ntids = col.members;
	out->ncontainers = (uint32) col.noffs;

	return true;
}


/* ---------------------------------------------------------------------
 * The groups of a walk, counted together (DESIGN.md §10)
 * --------------------------------------------------------------------- */

/*
 * A group's cursor on the grouped walk's heap: the container key it stands
 * at, and which group it is.
 */
typedef struct LionGroupEnt
{
	uint32		ckey;
	int32		g;
} LionGroupEnt;

static void
lion_group_heap_push(LionGroupEnt *heap, int *nheap, uint32 ckey, int g)
{
	int			i = (*nheap)++;

	while (i > 0)
	{
		int			parent = (i - 1) / 2;

		if (heap[parent].ckey <= ckey)
			break;
		heap[i] = heap[parent];
		i = parent;
	}
	heap[i].ckey = ckey;
	heap[i].g = g;
}

static LionGroupEnt
lion_group_heap_pop(LionGroupEnt *heap, int *nheap)
{
	LionGroupEnt top = heap[0];
	LionGroupEnt last;
	int			i = 0;

	Assert(*nheap > 0);
	if (--(*nheap) == 0)
		return top;

	last = heap[*nheap];
	for (;;)
	{
		int			l = 2 * i + 1;
		int			r = l + 1;
		int			small = i;
		uint32		smallkey = last.ckey;

		if (l < *nheap && heap[l].ckey < smallkey)
		{
			small = l;
			smallkey = heap[l].ckey;
		}
		if (r < *nheap && heap[r].ckey < smallkey)
			small = r;
		if (small == i)
			break;
		heap[i] = heap[small];
		i = small;
	}
	heap[i] = last;
	return top;
}

/*
 * How many groups one lion_count_groups_copy() takes at most.  Each holds a
 * cursor - a page image for a CHAIN set - and a pin on the posting leaf it
 * stands on, so a batch takes what the cursors of one count may hold open
 * (LionOpenBudget, DESIGN.md §15 "Bounded cursors"): work_mem of cursors, and
 * the pins the lists have left.  Never fewer than LION_OPEN_MIN_PINS, nor
 * more than LION_GROUP_BATCH_MAX (lion_count.h): past a few hundred groups a
 * batch saves nothing more, each reading of the copy being shared by that
 * many already.
 */

/*
 * The members of the copy's container at a key, times the groups that stand
 * there, from which the grouped walk makes that container a bitset image to
 * test the groups against; below it each group ANDs its container with the
 * copy's directly.  An image costs a clear and a count of 4 kB, 0.3 to 0.4 us
 * shared by the groups, and a test against it a nanosecond a member of a
 * group's container; the direct AND looks the copy's few members up in the
 * group's container, or merges the two.  A WHERE of one or two rows a key
 * against twenty dense groups is the case below it; a hundred groups against
 * a WHERE of twenty rows a key, above.
 */
#define LION_GROUP_IMAGE_MIN	128

int
lion_count_groups_batch(Relation index)
{
	LionOpenBudget budget;
	Size		per = sizeof(LionSetCursor) + sizeof(LionCountCtx) +
		sizeof(PGAlignedBlock) + 4 * sizeof(int64);
	Size		n;

	lion_open_budget_init(&budget, index);
	n = Min(budget.mem / per, (Size) budget.pins);
	n = Min(n, (Size) LION_GROUP_BATCH_MAX);
	return (int) Max(n, (Size) LION_OPEN_MIN_PINS);
}

/*
 * THE GROUPS OF A WALK, COUNTED TOGETHER (DESIGN.md §10, "The groups of a
 * walk, counted together").  counts[g] = the rows of groups[g] that the
 * collected copy `copy` (lion_sources_collect()) holds, visible to snapshot,
 * for each of ngroups located posting sets - what a count of each against the
 * copy (lion_run_merge_copy()) answers, made in ONE walk of container keys
 * for all of them.
 *
 * Counted one at a time, each group's containers looked the copy up at their
 * keys and were ANDed with the copy's container there: every container of
 * the copy was read, sought and merged again by every group that had a
 * container at its key - a GROUP BY of many groups over every page of the
 * heap read its collected WHERE once per group, each time an AND of a
 * group's few members with a dense ARRAY by galloping search.  Here the groups' cursors
 * stand on a heap ordered by container key, and at each key of the copy that
 * a group has a container at, the copy's container is set in a bitset image
 * ONCE and every group standing there is tested against it: a bit test per
 * member of a group's ARRAY, which gives the intersection's count and block
 * mask without building it (lion_container_and_image_count()).  The map is
 * asked once per key too, about every block the copy's container has a
 * member on - every group's intersection there lies on those.
 *
 * WHY IT IS EXACT, AND DESIGN.md §9.  Each group's set is located afresh by
 * the caller and read by a cursor of its own, which pins the page each of its
 * containers came from - the pinned source each of its counts had - and the
 * copy is the same pinless copy those counts were ANDed with, on the same
 * argument (lion_sources_collect()).  At a key, the map is asked after every
 * group standing there has copied its container under its pin, and before
 * any of them moves past it: lion_count_container()'s order, for all of them
 * at once.  A group whose set carries no pin of its own - located past the
 * pin budget, or a private copy - trusts no map (novm, per group), as its
 * count alone would; so does every group while a row filter applies
 * (DESIGN.md §17), whose rows all go to the heap.  A key the copy has nothing
 * at, and a group whose intersection there is empty, ask the map nothing, and
 * the groups sought past such keys let go of what they held there: no answer
 * rests on it.  Each group keeps its own recheck queue, as its count did, and
 * they share the visibility cache as the counts did.
 *
 * `images`, when not NULL, is ngroups page images of the caller's that the
 * groups' cursors copy their leaves to, so that a caller walking batch after
 * batch - a parallel GROUP BY's ranges above all - does not allocate and free
 * a page for every group every time.
 */
void
lion_count_groups_copy(Relation heap, Snapshot snapshot, int ngroups,
					   LionPostingSet *groups, const LionPostingSet *copy,
					   int64 *counts, LionCountStats *stats,
					   LionVisCache *cache, bool rel_read_only,
					   PGAlignedBlock *images)
{
	const LionMatSet *mat;
	MemoryContext cxt;
	MemoryContext oldcxt;
	LionCountCtx base;
	LionCountCtx *gcx;
	LionSetCursor *cur;
	bool	   *trust;
	LionGroupEnt *gheap;
	int		   *hot;
	int			nheap = 0;
	LionContainer *img;
	LionContainer *work;
	LionContainer *buf = NULL;
	int			batchmax;
	int			idx = 0;
	int			g;

	for (g = 0; g < ngroups; g++)
		counts[g] = 0;
	if (ngroups == 0 || !copy->found || copy->mat == NULL ||
		copy->mat->ncontainers == 0)
		return;
	mat = copy->mat;
	Assert(mat->collected);

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"lion index grouped walk",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	/* what lion_count_sources_run() sets up for one count, once for all */
	memset(&base, 0, sizeof(base));
	base.cxt = cxt;
	base.heap = heap;
	base.snapshot = snapshot;
	base.vmbuf = InvalidBuffer;
	if (cache != NULL && BufferIsValid(cache->vmbuf))
	{
		if (cache->vmrelid == RelationGetRelid(heap))
		{
			base.vmbuf = cache->vmbuf;
			cache->vmbuf = InvalidBuffer;
			cache->vmrelid = InvalidOid;
		}
		else
			lion_vis_cache_release_vm(cache);
	}
	base.serializable = IsolationIsSerializable();
	/* a copy is never made in recovery; were it, nothing would trust the map */
	base.in_recovery = RecoveryInProgress();
	base.rel_read_only = rel_read_only;
	base.raw = true;
	base.tids_sorted = true;

	/*
	 * The groups share the cache as their counts did, one after the other:
	 * each is a count begun, which is what lets the cache resolve a dirty
	 * block on its second visit rather than only record it.
	 */
	for (g = 0; g < ngroups; g++)
		lion_vis_cache_begin(cache, heap, snapshot);
	base.cache = cache;
	base.filter = (cache != NULL) ? cache->filter : NULL;
	if (base.filter != NULL && base.filter->heap != heap)
		elog(ERROR, "lion index count: a row filter for another relation");

	/* each group's recheck queue takes its share of one count's */
	batchmax = Max(lion_recheck_budget() / ngroups, LION_RECHECK_MIN_BATCH);

	gcx = (LionCountCtx *) palloc(sizeof(LionCountCtx) * ngroups);
	cur = (LionSetCursor *) palloc(sizeof(LionSetCursor) * ngroups);
	trust = (bool *) palloc(sizeof(bool) * ngroups);
	gheap = (LionGroupEnt *) palloc(sizeof(LionGroupEnt) * ngroups);
	hot = (int *) palloc(sizeof(int) * ngroups);
	img = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	work = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	if (lion_mat_spills(mat))
		buf = (LionContainer *) palloc(MAXALIGN(LION_CONTAINER_MAX_SIZE));

	for (g = 0; g < ngroups; g++)
	{
		LionPostingSet *ps = &groups[g];

		gcx[g] = base;
		gcx[g].vmbuf = InvalidBuffer;
		gcx[g].batchmax = batchmax;
		gcx[g].keyset = ps;
		trust[g] = ps->found && !ps->nopin && ps->mat == NULL;
		gcx[g].novm = !trust[g];

		/* nothing below the copy's first key can be counted: begin there */
		lion_cursor_init_at(&cur[g], ps, &gcx[g], false, lion_mat_key(mat, 0),
							images != NULL ? &images[g] : NULL);
		if (cur[g].valid)
			lion_group_heap_push(gheap, &nheap, cur[g].cur->ckey, g);
	}

	while (nheap > 0)
	{
		uint32		key = gheap[0].ckey;
		const LionContainer *w;
		const LionContainer *wc;
		uint64		wanted;
		uint64		allvis;
		int			nhot = 0;
		int			i;

		/* the copy at or past the smallest key a group stands at */
		base.stats.copy_seeks++;
		idx = lion_mat_seek(mat, idx, key);
		if (idx >= mat->ncontainers)
			break;
		if (lion_mat_key(mat, idx) != key)
		{
			uint32		target = lion_mat_key(mat, idx);

			/*
			 * The copy has nothing below target: every group below it is
			 * sought there (DESIGN.md §22), and nothing of the keys it passes
			 * is counted.
			 */
			while (nheap > 0 && gheap[0].ckey < target)
			{
				LionGroupEnt e = lion_group_heap_pop(gheap, &nheap);

				lion_cursor_seek(&cur[e.g], target);
				if (cur[e.g].valid)
					lion_group_heap_push(gheap, &nheap, cur[e.g].cur->ckey,
										 e.g);
			}
			CHECK_FOR_INTERRUPTS();
			continue;
		}

		/* every group standing at the key, its container read and pinned */
		while (nheap > 0 && gheap[0].ckey == key)
			hot[nhot++] = lion_group_heap_pop(gheap, &nheap).g;

		/*
		 * The copy's container there, once, as a bitset image - unless it is
		 * a few members, which each group's AND looks up in the group's
		 * container instead (lion_container_and_raw()): a sparse WHERE
		 * against dense groups would otherwise test every member of every
		 * group's container against an image of one or two.
		 */
		w = lion_mat_container(&base, mat, idx, buf);
		if (w->type == LION_CT_BITSET)
			wc = w;
		else if (lion_container_cardinality(w) * (uint32) nhot <
				 LION_GROUP_IMAGE_MIN)
			wc = NULL;
		else
		{
			lion_container_bitset_init(img, key);
			lion_container_or_into_bitset(w, LION_BITSET_DATA(img));
			(void) lion_container_bitset_recount(img);
			wc = img;
		}

		/*
		 * The map, once for every group here: every block any of their
		 * intersections has a member on is one of the copy's container's.
		 * The test hook of lion_count_container_vm(): the groups' containers
		 * copied, their pages pinned, the map not asked yet.
		 */
		LION_INJECTION_POINT("lion-count-containers-pinned");
		wanted = lion_container_block_mask(w);
		if (base.in_recovery || base.filter != NULL)
			allvis = 0;
		else
		{
			allvis = lion_vm_allvisible_mask(heap, lion_ckey_first_block(key),
											 wanted, &base.vmbuf,
											 &base.stats.vm_pins);
			base.stats.vm_checks++;
#ifdef LION_VM_MASK_CHECK
			lion_vm_mask_check(heap, lion_ckey_first_block(key), wanted,
							   allvis, &base.vmbuf);
#endif
		}

		for (i = 0; i < nhot; i++)
		{
			LionCountCtx *cx = &gcx[hot[i]];
			const LionContainer *c = cur[hot[i]].cur;
			uint64		av = trust[hot[i]] ? allvis : 0;
			uint64		blocks;
			uint32		n;
			bool		built = false;

			if (wc != NULL && c->type == LION_CT_ARRAY)
				n = lion_container_and_image_count(c, LION_BITSET_DATA(wc),
												   &blocks);
			else
			{
				n = lion_container_and_raw(c, wc != NULL ? wc : w, work);
				blocks = lion_container_block_mask(work);
				built = true;
			}
			if (n == 0)
				continue;

			if ((blocks & ~av) == 0 && !cx->serializable)
			{
				/* every row of it on an all-visible block: counted */
				cx->count += n;
				cx->stats.blocks_skipped_via_vm += pg_popcount64(blocks);
				continue;
			}

			/* the heap has to see some of it, or a predicate lock be taken */
			if (!built)
				(void) lion_container_and_raw(c, wc, work);
			lion_count_container_masks(cx, work, blocks, av);
		}

		/*
		 * Only now may the groups here let go of their pages (§9), and each
		 * goes straight to the copy's next key, the next one a count can be
		 * made at: a step where that is the next key, a seek past the keys
		 * between otherwise (DESIGN.md §22).  Past the copy's last key there
		 * is nothing left to count.
		 */
		if (idx + 1 >= mat->ncontainers)
			break;
		{
			uint32		next = lion_mat_key(mat, idx + 1);

			for (i = 0; i < nhot; i++)
			{
				int			hg = hot[i];

				if (next == key + 1)
					lion_cursor_next(&cur[hg]);
				else
					lion_cursor_seek(&cur[hg], next);
				if (cur[hg].valid)
					lion_group_heap_push(gheap, &nheap, cur[hg].cur->ckey, hg);
			}
		}
		CHECK_FOR_INTERRUPTS();
	}

	for (g = 0; g < ngroups; g++)
		lion_cursor_close(&cur[g]);

	/* no index page is pinned any more: the heap answers the rest */
	for (g = 0; g < ngroups; g++)
	{
		lion_recheck_flush(&gcx[g]);
		counts[g] = gcx[g].count + gcx[g].recheck_count;
		if (BufferIsValid(gcx[g].vmbuf))
			ReleaseBuffer(gcx[g].vmbuf);
		if (stats != NULL)
			lion_count_stats_add(stats, &gcx[g].stats);
	}
	if (stats != NULL)
		lion_count_stats_add(stats, &base.stats);

	if (BufferIsValid(base.vmbuf))
	{
		if (cache != NULL && !BufferIsValid(cache->vmbuf))
		{
			cache->vmbuf = base.vmbuf;
			cache->vmrelid = RelationGetRelid(heap);
		}
		else
			ReleaseBuffer(base.vmbuf);
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);
}


/* ---------------------------------------------------------------------
 * A copy shared by the participants of a parallel plan (LionSharedCopy)
 * --------------------------------------------------------------------- */

/*
 * ONE COPY PER QUERY (DESIGN.md §27, "One copy per query").  A parallel
 * FK-side join used to have every participant collect the fact filters for
 * itself - the same merge, the same copy, as many times as there were
 * processes.  Now they collect it once, together, into the Gather's dynamic
 * shared memory, the way a Parallel Hash builds one table:
 *
 *	- COLLECTING.  The heap's container keys are cut into chunks of `width`
 *	  keys, the last one open-ended (the heap may have grown since the chunks
 *	  were cut), and each participant claims the next chunk nobody has from
 *	  an atomic counter and collects it: the merge of lion_sources_collect()
 *	  over its own located sources, sought to the chunk's first key and
 *	  stopped at its end (LionCollect.ranged).  A chunk goes into the query's
 *	  DSA when the copy's memory still has room for it, and into a file of the
 *	  plan's file set otherwise, named for the chunk, with where each
 *	  container is in it in the DSA - a Parallel Hash's batches spill the
 *	  same way.  Whoever arrives last is ELECTED;
 *	- INDEXING.  The elected participant indexes the chunks as one copy: where
 *	  every container is (LionSpillEnt, in memory or in its chunk's file), the
 *	  keys and, where they are dense, the direct index (lion_mat_index()'s),
 *	  all in the DSA.  The others wait;
 *	- DONE.  Every participant reads the copy through a view of its own
 *	  (lion_copy_view()): pointers to its containers in memory, the files of
 *	  the chunks that spilled opened read-only, and the index read where it
 *	  is.  Nothing of the copy is ever written again.
 *
 * A participant that attaches late joins whatever phase the copy is in: it
 * collects what chunks are left, waits for the index, or just reads.  One
 * that never attaches is waited for by nobody.  An error or a cancel in any
 * participant ends the query, and the leader's with it: a worker's error is
 * rethrown in the leader, whose abort terminates the other workers, and a
 * worker waiting at the barrier answers that (ConditionVariableSleep() checks
 * for interrupts); the DSA goes with the query and the files with its DSM.
 *
 * Why the copy is as safe as the serial node's (DESIGN.md §27, "Why a stale
 * copy is safe").  Every chunk is read after the query's snapshot was taken -
 * which is all the argument asks of a copy: it cannot lack a row the snapshot
 * sees - and each participant counts it only ever beside its own located fk
 * set, which carries the interlock, exactly as a copy of its own.  The chunks
 * together are the intersection: they cover every container key once, and a
 * key's containers are the merge's at that key whoever collected them.
 */
#define LION_COPY_MAX_CHUNKS		64
#define LION_COPY_CHUNKS_EACH		4	/* chunks a participant: the work evens out */

/* the phases of LionSharedCopy.barrier */
#define LION_COPY_COLLECTING		0
#define LION_COPY_INDEXING			1
#define LION_COPY_DONE				2

/*
 * The fewest container keys a range covers when the executor cuts them,
 * pg_lion.parallel_range_keys: a range seeks every source to its first key,
 * and a parallel GROUP BY walks every group's entry again for each.  A testing
 * knob rather than a tuning one - the regression suite lowers it to cut a
 * table of a few megabytes into several ranges - so the planner prices the
 * ranges of the default width, LION_PARALLEL_RANGE_KEYS, whatever it is set
 * to.
 */
int			lion_parallel_range_keys = LION_PARALLEL_RANGE_KEYS;

/*
 * Cut the heap's container keys into ranges for the participants of a
 * parallel plan: a few for each, so that one that is slow to start or has
 * denser keys is made up for by the others, and no fewer than minkeys keys
 * each.  Returns how many there are, and in *ckeys the container keys they
 * are cut from, which lion_key_range() divides among them as evenly as whole
 * keys go - no range is empty.  The last one is open-ended.  The shared
 * copy's chunks are these ranges, and so are the ranges a parallel GROUP BY
 * counts its groups over (DESIGN.md §10, "A GROUP BY in parallel").
 */
int
lion_key_ranges(BlockNumber heapblocks, int participants, int minkeys,
				uint32 *ckeys)
{
	uint64		keys = (uint64) heapblocks / LION_BLOCKS_PER_CONTAINER + 1;
	uint64		n;

	n = Min((uint64) LION_COPY_MAX_CHUNKS,
			(uint64) Max(participants, 1) * LION_COPY_CHUNKS_EACH);
	n = Min(n, Max(keys / (uint64) Max(minkeys, 1), (uint64) 1));
	*ckeys = (uint32) keys;
	return (int) n;
}

/*
 * The container keys of range r of lion_key_ranges()'s cut of ckeys keys
 * into nranges: lo up to, not including, hi.  The last range has no end - the
 * heap may have grown since the ranges were cut - and takes every key from
 * its first on (LION_KEYS_END).
 */
void
lion_key_range(int nranges, uint32 ckeys, int r, uint32 *lo, uint64 *hi)
{
	Assert(r >= 0 && r < nranges);
	*lo = (uint32) ((uint64) r * ckeys / (uint64) nranges);
	*hi = (r == nranges - 1) ? LION_KEYS_END :
		(uint64) (r + 1) * ckeys / (uint64) nranges;
}

typedef struct LionCopyChunk
{
	dsa_pointer buf;			/* in memory: its containers, back to back */
	dsa_pointer ents;			/* in its file: where each one is */
	Size		bytes;			/* its containers' bytes */
	int			ncontainers;
	uint64		members;
	bool		spilled;
} LionCopyChunk;

struct LionSharedCopy
{
	Barrier		barrier;
	pg_atomic_uint32 nextchunk;	/* the next chunk nobody has claimed */
	pg_atomic_uint64 held;		/* the chunks' bytes in shared memory */
	Size		memory;			/* what they may take */
	int			participants;
	dsm_handle	seg;			/* the plan's DSM, which a worker attaches
								 * the file set through */
	int			nchunks;
	uint32		ckeys;			/* the container keys they are cut from */

	/* the index, made by the participant elected once every chunk is in */
	int			ncontainers;
	uint64		members;
	Size		bytes;
	int			nspilled;		/* chunks in files */
	dsa_pointer ents;			/* LionSpillEnt[ncontainers] */
	dsa_pointer keys;			/* uint32[ncontainers] */
	dsa_pointer dir;			/* uint32[dirlen], or InvalidDsaPointer */
	uint32		dirbase;
	uint32		dirlen;
	int			chunkfirst[LION_COPY_MAX_CHUNKS + 1];

	SharedFileSet fileset;
	LionCopyChunk chunk[LION_COPY_MAX_CHUNKS];
};

Size
lion_shared_copy_size(void)
{
	return MAXALIGN(sizeof(LionSharedCopy));
}

/* The wait at the barrier, as pg_stat_activity names it. */
static uint32
lion_copy_wait_event(void)
{
#if PG_VERSION_NUM >= 170000
	static uint32 event = 0;

	if (event == 0)
		event = WaitEventExtensionNew("LionFactFilterCopy");
	return event;
#else
	return PG_WAIT_EXTENSION;
#endif
}

/* A chunk's file in the plan's file set. */
static void
lion_copy_chunk_name(char *name, Size len, int c)
{
	snprintf(name, len, "lioncopy.%d", c);
}

/* Cut the heap's container keys into chunks (lion_key_ranges()). */
static void
lion_copy_cut(LionSharedCopy *sc, BlockNumber heapblocks)
{
	sc->nchunks = lion_key_ranges(heapblocks, sc->participants,
								  lion_parallel_range_keys, &sc->ckeys);
}

/* The copy as it is before anyone collects it. */
static void
lion_copy_empty(LionSharedCopy *sc, BlockNumber heapblocks)
{
	BarrierInit(&sc->barrier, 0);
	pg_atomic_write_u32(&sc->nextchunk, 0);
	pg_atomic_write_u64(&sc->held, 0);
	sc->ncontainers = 0;
	sc->members = 0;
	sc->bytes = 0;
	sc->nspilled = 0;
	sc->ents = InvalidDsaPointer;
	sc->keys = InvalidDsaPointer;
	sc->dir = InvalidDsaPointer;
	sc->dirbase = 0;
	sc->dirlen = 0;
	memset(sc->chunkfirst, 0, sizeof(sc->chunkfirst));
	memset(sc->chunk, 0, sizeof(sc->chunk));
	lion_copy_cut(sc, heapblocks);
}

/*
 * In the leader, once its DSM is made.  The file set is the DSM's: its files
 * go when the last process detaches from it, whatever happened.
 */
void
lion_shared_copy_init(LionSharedCopy *sc, dsm_segment *seg, int participants,
					  Size memory, BlockNumber heapblocks)
{
	memset(sc, 0, sizeof(LionSharedCopy));
	pg_atomic_init_u32(&sc->nextchunk, 0);
	pg_atomic_init_u64(&sc->held, 0);
	sc->memory = memory;
	sc->participants = participants;
	sc->seg = dsm_segment_handle(seg);
	SharedFileSetInit(&sc->fileset, seg);
	lion_copy_empty(sc, heapblocks);
}

/* In a worker, before it runs: the file set, through the DSM it attached. */
void
lion_shared_copy_attach(LionSharedCopy *sc)
{
	dsm_segment *seg = dsm_find_mapping(sc->seg);

	if (seg == NULL)
		elog(ERROR, "lion index: a parallel worker has not attached its plan's shared memory");
	SharedFileSetAttach(&sc->fileset, seg);
}

/*
 * Between two runs, in the leader, with no participant running (the Gather
 * has ended its workers, and the leader's own view of the last copy is gone):
 * the last copy's memory and files are freed, and the next run collects
 * again - with the new parameters of a rescan, over the heap as it is now.
 */
void
lion_shared_copy_reinit(LionSharedCopy *sc, dsa_area *area,
						BlockNumber heapblocks)
{
	int			c;

	if (area != NULL)
	{
		for (c = 0; c < LION_COPY_MAX_CHUNKS; c++)
		{
			if (DsaPointerIsValid(sc->chunk[c].buf))
				dsa_free(area, sc->chunk[c].buf);
			if (DsaPointerIsValid(sc->chunk[c].ents))
				dsa_free(area, sc->chunk[c].ents);
		}
		if (DsaPointerIsValid(sc->ents))
			dsa_free(area, sc->ents);
		if (DsaPointerIsValid(sc->keys))
			dsa_free(area, sc->keys);
		if (DsaPointerIsValid(sc->dir))
			dsa_free(area, sc->dir);
	}
	SharedFileSetDeleteAll(&sc->fileset);
	lion_copy_empty(sc, heapblocks);
}

/*
 * Collect chunk c: the part of the intersection whose container keys are the
 * chunk's, into this participant's memory first - at most what the copy has
 * left of its memory - and from there into the DSA, or, past that, into the
 * chunk's file.  A chunk that holds all the copy has left of its memory is
 * held twice for a moment, here and in the DSA; a chunk is a small part of
 * the heap's keys whenever the heap is large (lion_copy_cut()).
 */
static void
lion_copy_chunk(LionSharedCopy *sc, dsa_area *area, Relation heap,
				Snapshot snapshot, int nsources, LionCountSource *sources,
				int c, LionCountStats *stats)
{
	LionCopyChunk *ch = &sc->chunk[c];
	MemoryContext cxt;
	MemoryContext oldcxt;
	LionCollect col;
	char		name[MAXPGPATH];
	uint64		held = pg_atomic_read_u64(&sc->held);
	Size		room = (held < sc->memory) ? sc->memory - (Size) held : 0;
	int			i;

	cxt = AllocSetContextCreate(CurrentMemoryContext, "lion shared copy chunk",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);
	lion_copy_chunk_name(name, sizeof(name), c);

	memset(&col, 0, sizeof(col));
	col.cxt = cxt;
	col.maxbytes = room;
	col.spill = true;
	col.sp.fileset = &sc->fileset.fs;
	col.sp.name = name;
	col.ranged = true;
	lion_key_range(sc->nchunks, sc->ckeys, c, &col.lo, &col.hi);
	col.cap = 8192;
	col.buf = (char *) palloc(col.cap);
	col.offcap = 256;
	col.offs = (Size *) palloc(sizeof(Size) * col.offcap);

	(void) lion_count_sources_run(heap, snapshot, nsources, sources, stats,
								  NULL, false, false, &col);
	ch->members = col.members;

	if (!col.spilled && col.noffs > 0)
	{
		/*
		 * Into the shared memory, when the copy's memory still has room: the
		 * room is reserved before the memory is taken, and given back if the
		 * DSA cannot give it.
		 */
		uint64		before = pg_atomic_fetch_add_u64(&sc->held, col.used);
		dsa_pointer dp = InvalidDsaPointer;

		if (before + col.used <= sc->memory)
			dp = dsa_allocate_extended(area, col.used,
									   DSA_ALLOC_HUGE | DSA_ALLOC_NO_OOM);
		if (DsaPointerIsValid(dp))
		{
			memcpy(dsa_get_address(area, dp), col.buf, col.used);
			ch->buf = dp;
			ch->bytes = col.used;
			ch->ncontainers = col.noffs;
		}
		else
		{
			/* ... else into the chunk's file after all */
			(void) pg_atomic_fetch_sub_u64(&sc->held, col.used);
			lion_spill_begin(&col.sp, cxt);
			for (i = 0; i < col.noffs; i++)
				lion_spill_add(&col.sp,
							   (const LionContainer *) (col.buf + col.offs[i]));
			col.spilled = true;
		}
	}

	if (col.spilled)
	{
		/*
		 * The file is closed, for the others to open by its name, and where
		 * each container is in it goes into the shared memory: sixteen bytes
		 * a container, which a spilled copy keeps in memory in any case.
		 */
		LionSpill  *sp = &col.sp;
		dsa_pointer dp;

		BufFileClose(sp->file);
		sp->file = NULL;
		dp = dsa_allocate_extended(area,
								   sizeof(LionSpillEnt) * Max(sp->nents, 1),
								   DSA_ALLOC_HUGE);
		memcpy(dsa_get_address(area, dp), sp->ents,
			   sizeof(LionSpillEnt) * sp->nents);
		ch->ents = dp;
		ch->ncontainers = sp->nents;
		ch->bytes = 0;
		for (i = 0; i < sp->nents; i++)
			ch->bytes += sp->ents[i].size;
		ch->spilled = true;
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);
}

/*
 * The elected participant's part, once every chunk is in: where every
 * container of the copy is, in memory (fileno -1, and its offset in its
 * chunk's memory) or in its chunk's file, its key, and the direct index where
 * the keys are dense - what lion_mat_index() makes of a copy of one process.
 * The chunks cover ascending ranges of keys and each is in key order, so the
 * keys ascend across them.
 */
static void
lion_copy_index(LionSharedCopy *sc, dsa_area *area)
{
	LionSpillEnt *ents;
	uint32	   *keys;
	int			n = 0;
	int			c;
	int			i;
	int			j;

	for (c = 0; c < sc->nchunks; c++)
	{
		sc->chunkfirst[c] = n;
		n += sc->chunk[c].ncontainers;
		sc->members += sc->chunk[c].members;
		sc->bytes += sc->chunk[c].bytes;
		if (sc->chunk[c].spilled)
			sc->nspilled++;
	}
	sc->chunkfirst[sc->nchunks] = n;
	sc->ncontainers = n;
	if (n == 0)
		return;

	sc->ents = dsa_allocate_extended(area, sizeof(LionSpillEnt) * n,
									 DSA_ALLOC_HUGE);
	sc->keys = dsa_allocate_extended(area, sizeof(uint32) * n, DSA_ALLOC_HUGE);
	ents = (LionSpillEnt *) dsa_get_address(area, sc->ents);
	keys = (uint32 *) dsa_get_address(area, sc->keys);

	i = 0;
	for (c = 0; c < sc->nchunks; c++)
	{
		LionCopyChunk *ch = &sc->chunk[c];

		if (ch->spilled)
		{
			const LionSpillEnt *src = (const LionSpillEnt *)
				dsa_get_address(area, ch->ents);

			for (j = 0; j < ch->ncontainers; j++, i++)
			{
				ents[i] = src[j];
				keys[i] = src[j].ckey;
			}
		}
		else if (ch->ncontainers > 0)
		{
			const char *base = (const char *) dsa_get_address(area, ch->buf);
			Size		off = 0;

			for (j = 0; j < ch->ncontainers; j++, i++)
			{
				const LionContainer *item = (const LionContainer *) (base + off);
				Size		size = lion_item_size(item);

				ents[i].off = (pgoff_t) off;
				ents[i].ckey = item->ckey;
				ents[i].size = (uint16) size;
				ents[i].fileno = -1;
				keys[i] = item->ckey;
				off += MAXALIGN(size);
			}
			Assert(off == ch->bytes);
		}
	}
	Assert(i == n);

	for (i = 1; i < n; i++)
	{
		if (keys[i] <= keys[i - 1])
			elog(ERROR, "lion index: the chunks of a shared copy are out of key order");
	}

	if (n >= 2 &&
		(uint64) keys[n - 1] - keys[0] + 1 <= (uint64) n * LION_MAT_DIR_SPREAD)
	{
		uint32		first = keys[0];
		uint32	   *dir;
		uint64		k = 0;

		sc->dirlen = keys[n - 1] - first + 1;
		sc->dir = dsa_allocate_extended(area, sizeof(uint32) * sc->dirlen,
										DSA_ALLOC_HUGE);
		dir = (uint32 *) dsa_get_address(area, sc->dir);
		for (i = 0; i < n; i++)
		{
			while ((uint64) first + k <= keys[i])
				dir[k++] = (uint32) i;
		}
		Assert(k == sc->dirlen);
		sc->dirbase = first;
	}
}

/*
 * A participant's view of the finished copy, in the current memory context:
 * a posting set as lion_sources_collect() makes one - found, pinless, of no
 * index entry - whose containers are read where they are.  An empty copy is a
 * set that selects nothing, not found.  The files of the chunks that spilled
 * are opened here and closed by lion_posting_set_release(); the copy's first
 * container, which every cursor over it reads when it is built, is kept in
 * this process's memory when it is in a file.
 */
static void
lion_copy_view(LionSharedCopy *sc, dsa_area *area, Relation index,
			   LionPostingSet *out)
{
	LionMatSet *mat;
	LionSpillEnt *ents;
	int			n = sc->ncontainers;
	int			c;
	int			i;

	memset(out, 0, sizeof(LionPostingSet));
	out->pinbuf = InvalidBuffer;
	out->head = InvalidBlockNumber;
	out->entryblk = InvalidBlockNumber;
	out->entryoff = InvalidOffsetNumber;
	out->index = index;
	out->attno = 1;
	out->cxt = CurrentMemoryContext;
	if (n == 0)
		return;

	ents = (LionSpillEnt *) dsa_get_address(area, sc->ents);
	mat = (LionMatSet *) palloc0(sizeof(LionMatSet));
	mat->ncontainers = n;
	mat->bytes = sc->bytes;
	mat->spill = ents;
	mat->keys = (uint32 *) dsa_get_address(area, sc->keys);
	if (DsaPointerIsValid(sc->dir))
	{
		mat->dir = (uint32 *) dsa_get_address(area, sc->dir);
		mat->dirbase = sc->dirbase;
		mat->dirlen = sc->dirlen;
	}
	mat->containers = (LionContainer **) palloc(sizeof(LionContainer *) * n);
	mat->nchunks = sc->nchunks;
	mat->chunkfirst = sc->chunkfirst;
	mat->held = sizeof(LionMatSet) + sizeof(LionContainer *) * n;
	if (sc->nspilled > 0)
	{
		mat->files = (BufFile **) palloc0(sizeof(BufFile *) * sc->nchunks);
		mat->held += sizeof(BufFile *) * sc->nchunks;
	}

	for (c = 0; c < sc->nchunks; c++)
	{
		LionCopyChunk *ch = &sc->chunk[c];

		if (!ch->spilled)
		{
			char	   *base = (ch->ncontainers > 0) ?
				(char *) dsa_get_address(area, ch->buf) : NULL;

			for (i = sc->chunkfirst[c]; i < sc->chunkfirst[c + 1]; i++)
				mat->containers[i] = (LionContainer *) (base + ents[i].off);
		}
		else
		{
			char		name[MAXPGPATH];

			for (i = sc->chunkfirst[c]; i < sc->chunkfirst[c + 1]; i++)
				mat->containers[i] = NULL;
			lion_copy_chunk_name(name, sizeof(name), c);
			mat->files[c] = BufFileOpenFileSet(&sc->fileset.fs, name,
											   O_RDONLY, false);
		}
	}
	if (mat->containers[0] == NULL)
	{
		Size		size = MAXALIGN(Max((Size) ents[0].size, LION_CONTAINER_HDRSZ));

		mat->first = (LionContainer *) palloc(size);
		lion_spill_read(mat, 0, mat->first);
		mat->held += size;
	}
	mat->collected = true;

	out->found = true;
	out->mat = mat;
	out->ntids = sc->members;
	out->ncontainers = (uint32) n;
}

void
lion_shared_copy_collect(LionSharedCopy *sc, dsa_area *area, Relation heap,
						 Snapshot snapshot, int nsources,
						 LionCountSource *sources, LionPostingSet *out,
						 int *chunks, bool *built, bool *spilled,
						 LionCountStats *stats)
{
	Relation	index = NULL;
	int			phase;
	int			i;
	int			j;

	*chunks = 0;
	*built = false;

	/* the index the copy is named after, as lion_sources_collect() names it */
	for (i = 0; i < nsources && index == NULL; i++)
	{
		for (j = 0; j < sources[i].nsets; j++)
		{
			if (sources[i].sets[j].found)
			{
				index = sources[i].sets[j].index;
				break;
			}
		}
	}

	phase = BarrierAttach(&sc->barrier);
	if (phase == LION_COPY_COLLECTING)
	{
		for (;;)
		{
			uint32		c = pg_atomic_fetch_add_u32(&sc->nextchunk, 1);

			if (c >= (uint32) sc->nchunks)
				break;
			lion_copy_chunk(sc, area, heap, snapshot, nsources, sources,
							(int) c, stats);
			(*chunks)++;
			CHECK_FOR_INTERRUPTS();
		}
		if (BarrierArriveAndWait(&sc->barrier, lion_copy_wait_event()))
		{
			lion_copy_index(sc, area);
			*built = true;
		}
		phase = LION_COPY_INDEXING;
	}
	if (phase == LION_COPY_INDEXING)
		(void) BarrierArriveAndWait(&sc->barrier, lion_copy_wait_event());
	BarrierDetach(&sc->barrier);

	*spilled = (sc->nspilled > 0);
	lion_copy_view(sc, area, index, out);
}

/*
 * The union lion_range_collect() builds: one container per container key,
 * each the OR of what every set of the range has at that key.  The sets come
 * in key order, which for a column stored in the heap's order is container
 * key order too, but in general is not - so the containers are kept by key
 * in a hash table and put in order at the end.
 *
 * Only the container keys in [lo, hi) are kept: all of them, until a
 * WINDOWED collection runs out of memory and lowers hi
 * (lion_range_union_evict()).
 *
 * DENSE ACCUMULATION (DESIGN.md §32, "Summed ranges: dense and probed").  A
 * range over a column in no heap order gives every container key a member or
 * two from each summary, and folding each of them in with a
 * lion_container_or() rebuilt the union so far every time: a merge of the
 * whole ARRAY while it was one, and past 2048 members a 4 KB image filled,
 * counted, optimized and copied back, per member.  So a union keeps the form
 * it grows in instead.  Once it is a BITSET the containers are ORed into it
 * in place (lion_container_or_inplace()) and it is optimized once, when the
 * walk is over (lion_range_union_finish()).  While it is an ARRAY the
 * members of incoming ARRAYs are only appended to `pend`, and folded in -
 * one pass through an image, lion_container_add_many() - once half as many
 * as the union holds have come: each member is then moved a bounded number
 * of times, and `pend` is at most half the union's own size.  Anything else
 * (a RUN on either side, a column in heap order) is folded as before.
 */
typedef struct LionRangeUnionEnt
{
	uint32		ckey;			/* hash key */
	uint32		size;
	LionContainer *c;
	uint16	   *pend;			/* members still to fold into c, any order */
	uint32		npend;
	uint32		pendcap;
} LionRangeUnionEnt;

typedef struct LionRangeUnion
{
	HTAB	   *byckey;
	MemoryContext cxt;			/* the containers */
	Size		held;			/* what they take, with overhead */
	Size		maxbytes;
	bool		failed;
	bool		window;			/* past maxbytes, lower hi rather than fail */
	uint64		lo;				/* the container keys kept: [lo, hi) */
	uint64		hi;
	LionContainer *tmp;			/* LION_CONTAINER_MAX_SIZE bytes */
	uint64	   *img;			/* LION_BITSET_BYTES: a fold's image */
} LionRangeUnion;

/* No upper bound on the container keys of a window. */
#define LION_CKEY_END				((uint64) PG_UINT32_MAX + 1)

/* What one kept container costs beyond its chunk: its hash entry. */
#define LION_RANGE_UNION_OVERHEAD	(sizeof(LionRangeUnionEnt) + 16)

/*
 * The least memory a window has, whatever is left of the caller's: several
 * bitsets, so that a window always keeps some keys and gets on.
 */
#define LION_RANGE_WINDOW_MIN		(32 * 1024)

static int
lion_ckey_cmp(const void *a, const void *b)
{
	uint32		x = *(const uint32 *) a;
	uint32		y = *(const uint32 *) b;

	return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/*
 * A windowed collection past its memory: keep the lower half of the keys
 * the window holds, give the others back, and end the window where they
 * began - the next window starts there, and reads the range again for them.
 * With the window's memory at least LION_RANGE_WINDOW_MIN there are always
 * two keys or more to split.
 */
static void
lion_range_union_evict(LionRangeUnion *u)
{
	long		n = hash_get_num_entries(u->byckey);
	HASH_SEQ_STATUS seq;
	LionRangeUnionEnt *e;
	uint32	   *keys;
	uint32		cut;
	long		i = 0;

	if (n < 2)
		return;
	keys = (uint32 *) MemoryContextAlloc(u->cxt, sizeof(uint32) * n);
	hash_seq_init(&seq, u->byckey);
	while ((e = (LionRangeUnionEnt *) hash_seq_search(&seq)) != NULL)
		keys[i++] = e->ckey;
	Assert(i == n);
	qsort(keys, n, sizeof(uint32), lion_ckey_cmp);
	cut = keys[n / 2];
	pfree(keys);

	/* deleting the entry just returned is allowed during a hash_seq_search */
	hash_seq_init(&seq, u->byckey);
	while ((e = (LionRangeUnionEnt *) hash_seq_search(&seq)) != NULL)
	{
		uint32		ckey = e->ckey;

		if (ckey < cut)
			continue;
		u->held -= Min(u->held,
					   GetMemoryChunkSpace(e->c) + LION_RANGE_UNION_OVERHEAD);
		pfree(e->c);
		if (e->pend != NULL)
		{
			u->held -= Min(u->held, GetMemoryChunkSpace(e->pend));
			pfree(e->pend);
		}
		(void) hash_search(u->byckey, &ckey, HASH_REMOVE, NULL);
	}
	u->hi = cut;
}

/* Make src, a container of e's key, e's union: at its own size. */
static void
lion_range_union_store(LionRangeUnion *u, LionRangeUnionEnt *e,
					   const LionContainer *src)
{
	Size		size = lion_container_size(src);

	if (size != e->size)
	{
		u->held -= Min(u->held, GetMemoryChunkSpace(e->c));
		pfree(e->c);
		e->c = (LionContainer *) MemoryContextAlloc(u->cxt, size);
		u->held += GetMemoryChunkSpace(e->c);
		e->size = (uint32) size;
	}
	memcpy(e->c, src, size);
}

/*
 * Fold e's pending members into its union, in one pass through an image
 * (lion_container_add_many()).  A union that is no ARRAY afterwards takes
 * no more pending members, and gives their buffer back.
 */
static void
lion_range_union_fold(LionRangeUnion *u, LionRangeUnionEnt *e)
{
	if (e->npend == 0)
		return;
	memcpy(u->tmp, e->c, e->size);
	(void) lion_container_add_many(u->tmp, e->pend, e->npend, u->img);
	e->npend = 0;
	lion_range_union_store(u, e, u->tmp);
	if (e->c->type != LION_CT_ARRAY)
	{
		u->held -= Min(u->held, GetMemoryChunkSpace(e->pend));
		pfree(e->pend);
		e->pend = NULL;
		e->pendcap = 0;
	}
}

/*
 * The walk is over: every union folded and optimized, which is the form it
 * is handed out in.  A BITSET that was ORed into in place may now be smaller
 * as an ARRAY or a RUN.
 */
static void
lion_range_union_finish(LionRangeUnion *u)
{
	HASH_SEQ_STATUS seq;
	LionRangeUnionEnt *e;

	hash_seq_init(&seq, u->byckey);
	while ((e = (LionRangeUnionEnt *) hash_seq_search(&seq)) != NULL)
	{
		lion_range_union_fold(u, e);
		if (e->pend != NULL)
		{
			u->held -= Min(u->held, GetMemoryChunkSpace(e->pend));
			pfree(e->pend);
			e->pend = NULL;
			e->pendcap = 0;
		}
		if (e->c->type == LION_CT_BITSET)
		{
			memcpy(u->tmp, e->c, e->size);
			lion_container_optimize(u->tmp);
			lion_range_union_store(u, e, u->tmp);
		}
	}
}

static bool
lion_range_union_cb(const LionContainer *c, void *arg)
{
	LionRangeUnion *u = (LionRangeUnion *) arg;
	LionRangeUnionEnt *e;
	uint32		ckey = c->ckey;
	bool		found;
	Size		size;

	if (lion_container_cardinality(c) == 0)
		return true;
	if ((uint64) ckey < u->lo || (uint64) ckey >= u->hi)
		return true;			/* another window's */

	/*
	 * What a container takes is counted as the allocator hands it out
	 * (GetMemoryChunkSpace()): a BITSET's 4104 bytes rounded up to a chunk of
	 * 8 kB was counted as 4104, and a union held twice what it said.  The
	 * containers' context gives anything over a kilobyte a block of its own
	 * (lion_range_collect()), so that no longer happens either.
	 */
	e = (LionRangeUnionEnt *) hash_search(u->byckey, &ckey, HASH_ENTER, &found);
	if (!found)
	{
		size = lion_container_size(c);
		e->c = (LionContainer *) MemoryContextAlloc(u->cxt, size);
		memcpy(e->c, c, size);
		e->size = (uint32) size;
		e->pend = NULL;
		e->npend = 0;
		e->pendcap = 0;
		u->held += GetMemoryChunkSpace(e->c) + LION_RANGE_UNION_OVERHEAD;
	}
	else if (e->c->type == LION_CT_BITSET)
	{
		/* dense: ORed in place, optimized once the walk is over */
		(void) lion_container_or_inplace(e->c, c);
	}
	else if (e->c->type == LION_CT_ARRAY && c->type == LION_CT_ARRAY &&
			 c->cardinality <= LION_RANGE_UNION_PEND_MIN)
	{
		/*
		 * A few members: pending, until half as many as the union holds
		 * have come.  The buffer grows with the union, never past that.
		 */
		uint32		fold = Max((uint32) LION_RANGE_UNION_PEND_MIN,
							   (uint32) e->c->cardinality / 2);
		uint32		want = fold + LION_RANGE_UNION_PEND_MIN;

		if (e->pendcap < want)
		{
			Size		had = (e->pend != NULL) ?
				GetMemoryChunkSpace(e->pend) : 0;

			e->pend = (e->pend != NULL) ?
				(uint16 *) repalloc(e->pend, sizeof(uint16) * want) :
				(uint16 *) MemoryContextAlloc(u->cxt, sizeof(uint16) * want);
			e->pendcap = want;
			u->held += GetMemoryChunkSpace(e->pend);
			u->held -= Min(u->held, had);
		}
		memcpy(&e->pend[e->npend], LION_ARRAY_DATA((LionContainer *) c),
			   sizeof(uint16) * c->cardinality);
		e->npend += c->cardinality;
		if (e->npend >= fold)
			lion_range_union_fold(u, e);
	}
	else
	{
		lion_range_union_fold(u, e);
		if (e->c->type == LION_CT_BITSET)
			(void) lion_container_or_inplace(e->c, c);
		else
		{
			(void) lion_container_or(e->c, c, u->tmp);
			lion_range_union_store(u, e, u->tmp);
		}
	}

	if (u->held > u->maxbytes)
	{
		if (!u->window)
		{
			u->failed = true;
			return false;
		}
		while (u->held > u->maxbytes && hash_get_num_entries(u->byckey) > 1)
			lion_range_union_evict(u);
	}
	return true;
}

static int
lion_range_union_cmp(const void *a, const void *b)
{
	uint32		x = (*(LionRangeUnionEnt *const *) a)->ckey;
	uint32		y = (*(LionRangeUnionEnt *const *) b)->ckey;

	return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/*
 * One window of a range's union (above): walk the range and keep what its
 * sets hold at the window's keys.  nsets and nsummaries, when given, say what
 * the walk read.
 */
static void
lion_range_union_walk(Relation index, AttrNumber attno, LionRange *range,
					  LionRangeUnion *u, int64 *nsets, int64 *nsummaries)
{
	LionEntryScan es;
	LionPostingSet ps;
	Datum		key;

	lion_entry_scan_begin_sum(&es, index, attno, range, LION_WALK_INSIDE);
	while (!u->failed && lion_entry_scan_next(&es, &key, &ps))
	{
		if (ps.found)
			(void) lion_sets_iterate(1, &ps, NULL, lion_range_union_cb, u);
		lion_posting_set_release(&ps);
		if (nsets != NULL)
			(*nsets)++;
		CHECK_FOR_INTERRUPTS();
	}
	if (nsummaries != NULL)
		*nsummaries = es.nsummaries;
	lion_entry_scan_end(&es);
}

/* The union's entries, in key order, in the current memory context. */
static LionRangeUnionEnt **
lion_range_union_sorted(LionRangeUnion *u, long *n)
{
	HASH_SEQ_STATUS seq;
	LionRangeUnionEnt *e;
	LionRangeUnionEnt **ents;
	long		i = 0;

	*n = hash_get_num_entries(u->byckey);
	ents = (LionRangeUnionEnt **)
		palloc(sizeof(LionRangeUnionEnt *) * Max(*n, 1));
	hash_seq_init(&seq, u->byckey);
	while ((e = (LionRangeUnionEnt *) hash_seq_search(&seq)) != NULL)
		ents[i++] = e;
	Assert(i == *n);
	if (*n > 1)
		qsort(ents, *n, sizeof(LionRangeUnionEnt *), lion_range_union_cmp);
	return ents;
}

/*
 * The rows of one range, collected (DESIGN.md §32, "A range as a source").
 *
 * The walk is the one a summed range makes (lion_entry_scan_begin_sum()):
 * the entries of the range's partial buckets and the summaries of its whole
 * ones, disjoint sets of one scalar column whose union is exactly the rows
 * whose key lies in the range.  Each set's containers are ORed into the union
 * as they come; its pin, if it has one, is dropped once it has been read.
 *
 * What comes out is safe on lion_sources_collect()'s terms, for the same
 * reasons: a stale copy - a TID VACUUM has since removed - is only ever
 * counted beside a located set that holds the §9 pin, which settles it, and
 * no row visible to the caller's snapshot can be missing, because the copy is
 * read after the snapshot was taken and every such row was in the index
 * before its transaction committed.  The same goes for the summaries: an
 * insert puts its row under its key and then under its bucket's summary, both
 * before it commits, and the walk reads the keys of a bucket or its summary,
 * never both (DESIGN.md §32, "Readers").
 *
 * A union that fits in maxbytes IS the set: its containers stay where the
 * union built them, in a context that becomes the set's, and are handed out
 * in key order through containers[].  It used to be copied into one buffer
 * at the end, while the hash table still held it, which took twice what the
 * union did at the peak (2026-09-28 review).
 *
 * One that does not fit fails - the caller walks the range at every count
 * instead - unless spill says it may not: an OR's leaf, which cannot be taken
 * apart that way and used to be collected whatever it took.  It is then
 * collected a WINDOW of container keys at a time into a temporary file
 * (LionSpill): a window keeps the keys from where the last one ended; when
 * its memory - maxbytes, and never below LION_RANGE_WINDOW_MIN - runs out it
 * gives back the upper half of its keys and ends where they began
 * (lion_range_union_evict()); at the end of the walk its containers go to
 * the file in key order, and the next window reads the range again from its
 * end.  Every window is a walk of the range, so a union n times the memory
 * costs about 2n walks; each is read after the snapshot like the first, and
 * the windows are disjoint ranges of TIDs, so the file holds each row once.
 * The set reads the file a container at a time; what it keeps in memory is
 * sixteen bytes a container key.
 */
bool
lion_range_collect(Relation index, AttrNumber attno, LionRange *range,
				   Size maxbytes, bool spill, LionPostingSet *out, Size *held,
				   bool *spilled, int64 *nsets, int64 *nsummaries)
{
	LionRangeUnion u;
	LionSpill	sp;
	HASHCTL		ctl;
	LionRangeUnionEnt **ents;
	LionMatSet *mat;
	MemoryContext cxt;
	uint64		lo = 0;
	uint64		members = 0;
	bool		windowed = false;
	long		n;
	long		i;

	memset(out, 0, sizeof(LionPostingSet));
	out->pinbuf = InvalidBuffer;
	out->head = InvalidBlockNumber;
	out->index = index;
	out->attno = attno;
	out->cxt = CurrentMemoryContext;
	out->entryblk = InvalidBlockNumber;
	out->entryoff = InvalidOffsetNumber;
	*held = 0;
	*spilled = false;
	*nsets = 0;
	*nsummaries = 0;
	memset(&sp, 0, sizeof(sp));

	/* the catalogs a spill's file needs, looked up before the walk starts */
	if (spill)
		PrepareTempTablespaces();

	for (;;)
	{
		/*
		 * One window, in a context of its own.  Anything over a kilobyte - a
		 * BITSET, a long ARRAY - is a block of its own there, allocated at
		 * its size and given back to malloc when freed (ALLOCSET_SMALL_SIZES
		 * put the chunk limit at 1 kB), so the union takes about what its
		 * containers are.
		 */
		cxt = AllocSetContextCreate(CurrentMemoryContext, "lion range collect",
									ALLOCSET_SMALL_SIZES);
		memset(&u, 0, sizeof(u));
		u.cxt = cxt;
		u.maxbytes = spill ? Max(maxbytes, (Size) LION_RANGE_WINDOW_MIN) :
			maxbytes;
		u.window = spill;
		u.lo = lo;
		u.hi = LION_CKEY_END;
		u.tmp = (LionContainer *) MemoryContextAlloc(cxt,
													 LION_CONTAINER_MAX_SIZE);
		u.img = (uint64 *) MemoryContextAlloc(cxt, LION_BITSET_BYTES);
		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(uint32);
		ctl.entrysize = sizeof(LionRangeUnionEnt);
		ctl.hcxt = cxt;
		u.byckey = hash_create("lion range union", 256, &ctl,
							   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

		/* the first window's walk is the one that says what the range read */
		lion_range_union_walk(index, attno, range, &u,
							  windowed ? NULL : nsets,
							  windowed ? NULL : nsummaries);

		if (u.failed)
		{
			Assert(!windowed);
			MemoryContextDelete(cxt);
			return false;
		}

		lion_range_union_finish(&u);
		ents = lion_range_union_sorted(&u, &n);

		if (!windowed && u.hi == LION_CKEY_END)
		{
			/*
			 * All of it, in memory: the containers stay where they are, and
			 * cxt - with nothing else left in it - is the set's.
			 */
			if (n == 0)
			{
				pfree(ents);
				MemoryContextDelete(cxt);
				return true;	/* the range selects nothing: not found */
			}
			mat = (LionMatSet *) palloc0(sizeof(LionMatSet));
			mat->ncontainers = (int) n;
			mat->containers = (LionContainer **)
				palloc(sizeof(LionContainer *) * n);
			for (i = 0; i < n; i++)
			{
				mat->containers[i] = ents[i]->c;
				mat->bytes += ents[i]->size;
				members += lion_container_cardinality(ents[i]->c);
			}
			pfree(ents);
			pfree(u.tmp);
			pfree(u.img);
			hash_destroy(u.byckey);
			mat->held = sizeof(LionMatSet) + sizeof(LionContainer *) * n +
				MemoryContextMemAllocated(cxt, true);

			out->found = true;
			out->mat = mat;
			out->ntids = members;
			out->ncontainers = (uint32) n;
			*held = mat->held;
			return true;
		}

		/* A window of more: its containers, in key order, to the file. */
		if (!windowed)
		{
			lion_spill_begin(&sp, CurrentMemoryContext);
			windowed = true;
		}
		for (i = 0; i < n; i++)
		{
			lion_spill_add(&sp, ents[i]->c);
			members += lion_container_cardinality(ents[i]->c);
		}
		pfree(ents);
		MemoryContextDelete(cxt);

		if (u.hi == LION_CKEY_END)
			break;
		Assert(u.hi > lo);
		lo = u.hi;
	}

	if (sp.nents == 0)
	{
		lion_spill_abandon(&sp);
		return true;			/* the range selects nothing: not found */
	}
	mat = lion_spill_finish(&sp);
	out->found = true;
	out->mat = mat;
	out->ntids = members;
	out->ncontainers = (uint32) mat->ncontainers;
	*held = mat->held;
	*spilled = true;
	return true;
}

/* ---------------------------------------------------------------------
 * A summed range PROBED at the rows of the other sources (DESIGN.md §32,
 * "Summed ranges: dense and probed")
 * --------------------------------------------------------------------- */

/*
 * A sum over a range counts the sets its walk hands out - the keys of its
 * partial buckets and the summaries of its whole ones - one count each, ANDed
 * with the other sources F.  When the column's rows lie all over the heap
 * every summary has a container at nearly every container key it can, a few
 * members each, and every one of those counts is a merge of thousands of tiny
 * containers with F: the summary drives, F is sought at each of its keys, or
 * F drives and the summary is sought at each of F's.  Either way the work is
 * the number of summaries times the smaller side, with the set-up of a count
 * on top of each, however few rows F has.
 *
 * PROBED, the sum turns around.  F is collected once - the intersection of
 * its sources, pinless, lion_sources_collect() - and every set of the walk is
 * then read only at F's container keys: its cursor is sought from one of them
 * to the next, and what it holds there is marked against F's members.  An
 * ARRAY container of F keeps a bit per member (lion_container_mark_members()),
 * any other a bitset image the range's containers are ORed into.  What the
 * walk leaves is the range's rows among F's, at most F's size whatever the
 * range covers; ONE count of that set ANDed with F answers the sum.
 *
 * WHY IT IS EXACT.  The sets of a summed walk are disjoint (§32, "Readers")
 * and their union is the rows of the part, so the sum of their counts is the
 * count of their union ANDed with F, and so is the count of any set that
 * holds every visible row of that union that F holds and nothing outside the
 * union.  The marks are that set: a member is marked only when a set of the
 * walk holds it, and every visible row of the union that F holds is in the
 * collected copy of F, because the copy is read after the caller's snapshot
 * was taken and a visible row was in every index before its transaction
 * committed - the collected set's argument (lion_sources_collect()).
 *
 * WHY §9 STILL HOLDS.  The walk's sets are read without pins, as a collected
 * range's are (§32, "A range as a source"), and the marks are a copy.  So the
 * count of them is made beside F as the count reads it - the view below, in
 * which a set an earlier count copied into memory is walked from its chain
 * again - and only when a positive source of F carries the interlock there
 * (lion_source_pinned()); otherwise the caller sums the old way, where each
 * set of the walk carries it.  Every member the count takes from the map is
 * then in the container of that source it holds a pin on, which is all the
 * §9 argument asks of a candidate (lion_posting_set_materialize()).
 */
struct LionRangeProbe
{
	MemoryContext cxt;			/* everything below */
	MemoryContext setcxt;		/* one added set's cursor, reset after it */
	int			nsources;		/* F, as the final count reads it */
	LionCountSource *sources;
	LionPostingSet probe;		/* F's intersection, collected */
	int			n;				/* its containers */
	uint32	   *keys;			/* ... their keys, ascending */
	const LionContainer **conts;
	uint64	  **marks;			/* per container: a bit per ARRAY member, or
								 * an image (NULL until something lands) */
	Relation	index;			/* the range's, from the first set added */
	uint16		attno;
	LionCountCtx cx;			/* the cursors': no pins, statistics only */
};

/* First i >= from with keys[i] >= target, or n: a galloping search. */
static inline int
lion_ckey_gallop(const uint32 *keys, int n, int from, uint32 target)
{
	int			lo = from;
	int			step = 1;
	int			hi;

	if (from >= n || keys[from] >= target)
		return from;
	while (from + step < n && keys[from + step] < target)
	{
		lo = from + step;
		step *= 2;
	}
	hi = Min(from + step, n);
	lo++;
	while (lo < hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (keys[mid] < target)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/*
 * Begin a probed sum beside the sources F (sources[0 .. nsources - 1]): NULL
 * when it cannot be taken - F has no positive source, holds a range still to
 * be walked, has no positive source that would carry the §9 interlock, or its
 * collected copy and the marks would take more than maxbytes - and the
 * caller then sums as it always has.  The probe is allocated in a context of
 * its own under the current one; lion_range_probe_end() frees it.  F must
 * stay located until then.
 */
LionRangeProbe *
lion_range_probe_begin(Relation heap, Snapshot snapshot, int nsources,
					   LionCountSource *sources, Size maxbytes,
					   LionCountStats *stats)
{
	LionRangeProbe *rp;
	MemoryContext cxt;
	MemoryContext oldcxt;
	LionOpenBudget budget;
	LionMatSet *mat;
	uint64	   *words;
	Size		need;
	Size		nwords = 0;
	bool		positive = false;
	bool		carried = false;
	bool		spilled;
	int			i;
	int			j;

	for (i = 0; i < nsources; i++)
	{
		if (sources[i].rangewalk != NULL)
			return NULL;
		if (!sources[i].negated)
			positive = true;
	}
	if (!positive)
		return NULL;

	cxt = AllocSetContextCreate(CurrentMemoryContext, "lion range probe",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);
	rp = (LionRangeProbe *) palloc0(sizeof(LionRangeProbe));
	rp->cxt = cxt;

	/*
	 * F as the final count reads it: copies of its sets, of which a CHAIN set
	 * an earlier count copied into memory (lion_posting_set_materialize()) is
	 * walked from its root again, under the pins of its own cursor.  None is
	 * copied into memory by that count - a copy would carry no interlock,
	 * and it reads each set once - and the sets themselves are left as they
	 * are for the counts of the walk before and after this one.  The copies
	 * share the sets' payloads and pins, which stay the caller's.
	 */
	lion_open_budget_init(&budget, heap);
	rp->nsources = nsources;
	rp->sources = (LionCountSource *) palloc(sizeof(LionCountSource) * nsources);
	for (i = 0; i < nsources; i++)
	{
		LionCountSource *s = &rp->sources[i];

		*s = sources[i];
		s->sets = (LionPostingSet *)
			palloc(sizeof(LionPostingSet) * Max(s->nsets, 1));
		memcpy(s->sets, sources[i].sets, sizeof(LionPostingSet) * s->nsets);
		for (j = 0; j < s->nsets; j++)
		{
			LionPostingSet *ps = &s->sets[j];

			ps->matfailed = true;
			if (ps->found && ps->mat != NULL && !ps->is_inline &&
				BlockNumberIsValid(ps->head))
			{
				ps->mat = NULL;
				ps->budgeted = false;	/* the original returns its pin */
			}
		}
		if (!s->negated &&
			lion_source_pinned(lion_source_tree(s), s->sets, &budget))
			carried = true;
	}
	if (!carried)
	{
		MemoryContextSwitchTo(oldcxt);
		MemoryContextDelete(cxt);
		return NULL;
	}

	/* F's rows, copied: half of the memory, the marks and the count the rest */
	if (!lion_sources_collect(heap, snapshot, nsources, sources, maxbytes / 2,
							  false, &rp->probe, &spilled, stats))
	{
		MemoryContextSwitchTo(oldcxt);
		MemoryContextDelete(cxt);
		return NULL;
	}
	Assert(!spilled);

	mat = rp->probe.found ? rp->probe.mat : NULL;
	rp->n = (mat != NULL) ? mat->ncontainers : 0;
	rp->keys = (uint32 *) palloc(sizeof(uint32) * Max(rp->n, 1));
	rp->conts = (const LionContainer **)
		palloc(sizeof(LionContainer *) * Max(rp->n, 1));
	rp->marks = (uint64 **) palloc0(sizeof(uint64 *) * Max(rp->n, 1));

	/*
	 * What the marks and the count's set can take: a bit per member of an
	 * ARRAY and the ARRAY itself again, an image and a BITSET's worth for
	 * any other.  Checked before anything is walked, so that nothing has to
	 * be given up halfway.
	 */
	need = (mat != NULL) ? mat->held : 0;
	for (i = 0; i < rp->n; i++)
	{
		const LionContainer *c = mat->containers[i];

		rp->keys[i] = c->ckey;
		rp->conts[i] = c;
		if (c->type == LION_CT_ARRAY)
		{
			uint32		card = Min((uint32) c->cardinality,
								   (uint32) LION_ARRAY_MAX_CARD);

			nwords += (card + 63) / 64;
			need += lion_container_size_for(LION_CT_ARRAY, card, 0) + 16;
		}
		else
			need += 2 * (LION_CONTAINER_MAX_SIZE + 16);
	}
	need += nwords * sizeof(uint64) +
		(Size) rp->n * (sizeof(uint32) + 2 * sizeof(void *));
	if (need > maxbytes)
	{
		MemoryContextSwitchTo(oldcxt);
		MemoryContextDelete(cxt);
		return NULL;
	}

	words = (uint64 *) palloc0(sizeof(uint64) * Max(nwords, 1));
	for (i = 0; i < rp->n; i++)
	{
		const LionContainer *c = rp->conts[i];

		if (c->type != LION_CT_ARRAY)
			continue;
		rp->marks[i] = words;
		words += (Min((uint32) c->cardinality, (uint32) LION_ARRAY_MAX_CARD) +
				  63) / 64;
	}

	/* The cursors of the walk's sets: they carry nothing, they only read. */
	rp->cx.cxt = cxt;
	rp->cx.vmbuf = InvalidBuffer;
	rp->cx.droppins = true;
	rp->setcxt = AllocSetContextCreate(cxt, "lion range probe set",
									   ALLOCSET_DEFAULT_SIZES);

	MemoryContextSwitchTo(oldcxt);
	return rp;
}

/* What c, at the key of F's container i, holds of F's rows. */
static void
lion_range_probe_mark(LionRangeProbe *rp, int i, const LionContainer *c)
{
	const LionContainer *p = rp->conts[i];

	if (p->type == LION_CT_ARRAY)
	{
		(void) lion_container_mark_members(c,
										   LION_ARRAY_DATA((LionContainer *) p),
										   Min((uint32) p->cardinality,
											   (uint32) LION_ARRAY_MAX_CARD),
										   rp->marks[i]);
		return;
	}
	if (rp->marks[i] == NULL)
		rp->marks[i] = (uint64 *) MemoryContextAllocZero(rp->cxt,
														 LION_BITSET_BYTES);
	lion_container_or_into_bitset(c, rp->marks[i]);
}

/*
 * Add one set the walk handed out.  Its cursor holds no pin and is sought
 * from each of F's container keys to the next: a set with nothing at them
 * costs the skips of its cursor - over a sparse segment's pairs, across a
 * page by a descent - and not a container each.  The set stays the caller's
 * to release.
 */
void
lion_range_probe_add(LionRangeProbe *rp, LionPostingSet *set)
{
	LionSetCursor cur;
	MemoryContext oldcxt;
	int			i = 0;

	if (!set->found)
		return;
	if (rp->index == NULL)
	{
		rp->index = set->index;
		rp->attno = set->attno;
	}
	if (rp->n == 0)
		return;					/* F selects nothing */

	oldcxt = MemoryContextSwitchTo(rp->setcxt);
	lion_cursor_init(&cur, set, &rp->cx, true);
	while (cur.valid)
	{
		uint32		ckey = cur.cur->ckey;

		if (rp->keys[i] < ckey)
		{
			i = lion_ckey_gallop(rp->keys, rp->n, i, ckey);
			if (i >= rp->n)
				break;
		}
		if (rp->keys[i] == ckey)
		{
			lion_range_probe_mark(rp, i, cur.cur);
			lion_cursor_next(&cur);
		}
		else
			lion_cursor_seek(&cur, rp->keys[i]);
		CHECK_FOR_INTERRUPTS();
	}
	lion_cursor_close(&cur);
	MemoryContextSwitchTo(oldcxt);
	MemoryContextReset(rp->setcxt);
}

/*
 * The sum: the rows the added sets hold among F's, ANDed with F as the count
 * reads it, counted once - under the visibility map, the recheck and the
 * row filter of any count (lion_count_sources_cached()).
 */
int64
lion_range_probe_count(LionRangeProbe *rp, Relation heap, Snapshot snapshot,
					   LionCountStats *stats, LionVisCache *cache,
					   bool rel_read_only)
{
	MemoryContext oldcxt;
	LionMatSet *mat;
	LionPostingSet acc;
	LionCountSource *srcs;
	LionContainer *tmp;
	LionContainer *res;
	uint64		members = 0;
	int64		result = 0;
	int			k = 0;
	int			i;

	if (stats != NULL)
		stats->containers_visited += rp->cx.stats.containers_visited;
	rp->cx.stats.containers_visited = 0;
	if (rp->n == 0 || rp->index == NULL)
		return 0;

	oldcxt = MemoryContextSwitchTo(rp->cxt);
	tmp = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	res = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	mat = (LionMatSet *) palloc0(sizeof(LionMatSet));
	mat->containers = (LionContainer **)
		palloc(sizeof(LionContainer *) * rp->n);

	for (i = 0; i < rp->n; i++)
	{
		const LionContainer *p = rp->conts[i];
		const uint64 *m = rp->marks[i];
		Size		size;

		if (m == NULL)
			continue;
		if (p->type == LION_CT_ARRAY)
		{
			const uint16 *arr = LION_ARRAY_DATA((LionContainer *) p);
			uint32		card = Min((uint32) p->cardinality,
								   (uint32) LION_ARRAY_MAX_CARD);
			uint32		j;

			lion_container_init(res, p->ckey);
			for (j = 0; j < card; j++)
				if ((m[j >> 6] >> (j & 63)) & 1)
					lion_container_append_sorted(res, arr[j]);
			lion_container_optimize(res);
		}
		else
		{
			/* the image holds the range's rows at this key: F's among them */
			lion_bits_to_container(m, p->ckey, tmp);
			(void) lion_container_and(tmp, p, res);
		}
		if (lion_container_cardinality(res) == 0)
			continue;

		size = lion_container_size(res);
		mat->containers[k] = (LionContainer *) palloc(size);
		memcpy(mat->containers[k], res, size);
		mat->bytes += size;
		members += lion_container_cardinality(res);
		k++;
	}

	if (k > 0)
	{
		mat->ncontainers = k;
		mat->held = sizeof(LionMatSet) + sizeof(LionContainer *) * rp->n +
			mat->bytes + (Size) k * 16;

		/*
		 * A collected set of the range's index: in recovery the count trusts
		 * the map only when that index, like every other it reads, replays
		 * under cleanup locks (lion_sources_all_rmgr()), as when each of the
		 * walk's sets was counted on its own.
		 */
		memset(&acc, 0, sizeof(acc));
		acc.index = rp->index;
		acc.attno = rp->attno;
		acc.found = true;
		acc.head = InvalidBlockNumber;
		acc.pinbuf = InvalidBuffer;
		acc.cxt = rp->cxt;
		acc.mat = mat;
		acc.matfailed = true;
		acc.ntids = members;
		acc.ncontainers = (uint32) k;
		acc.entryblk = InvalidBlockNumber;
		acc.entryoff = InvalidOffsetNumber;

		srcs = (LionCountSource *)
			palloc0(sizeof(LionCountSource) * (rp->nsources + 1));
		srcs[0].nsets = 1;
		srcs[0].sets = &acc;
		memcpy(&srcs[1], rp->sources, sizeof(LionCountSource) * rp->nsources);

		result = lion_count_sources_run(heap, snapshot, rp->nsources + 1, srcs,
										stats, cache, rel_read_only, false,
										NULL);
	}

	MemoryContextSwitchTo(oldcxt);
	return result;
}

void
lion_range_probe_end(LionRangeProbe *rp)
{
	/* the collected copy is in memory, never a file: nothing to close */
	MemoryContextDelete(rp->cxt);
}

static int64
lion_count_sources_run(Relation heap, Snapshot snapshot, int nsources,
					   LionCountSource *sources, LionCountStats *stats,
					   LionVisCache *cache, bool rel_read_only, bool exists,
					   LionCollect *collect)
{
	MemoryContext cxt;
	MemoryContext oldcxt;
	LionCountCtx cx;
	LionKeyNode **trees;
	LionOpenBudget budget;
	int64		result;
	int			ncarry;
	bool	   *carry;
	bool		summed;
	bool		oneset;
	bool		scratch = false;
	int			batchsrc;
	Size		matheld;
	Size		matbudget = (Size) work_mem * 1024;
	int			npositive = 0;
	int			i;
	int			j;

	Assert(nsources >= 1);

	/*
	 * The shape of each source, and whether it can select anything at all: a
	 * positive source that cannot makes the whole intersection empty, and a
	 * negated one that cannot simply subtracts nothing.  A range source still
	 * to be walked is no set at all, and is the caller's to expand
	 * (LionCountSource.rangewalk).
	 */
	trees = (LionKeyNode **) palloc0(sizeof(LionKeyNode *) * nsources);
	for (i = 0; i < nsources; i++)
	{
		if (sources[i].rangewalk != NULL)
			elog(ERROR, "lion index: a range source to be walked handed to a count");
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

	/*
	 * A collection (DESIGN.md §27) wants the union itself, not the sum of its
	 * counts, and needs no interlock to build it: a NOPIN set is read from
	 * its own copy like any other.
	 */
	if (collect != NULL)
		summed = false;
	oneset = !summed && lion_sources_one_set(nsources, sources);

	/*
	 * Everything else is a merge, and a merge builds every cursor of every
	 * source at once.  What they may hold open is budgeted (DESIGN.md §15,
	 * "Bounded cursors"): a disjoint list too big for the budget is taken a
	 * batch at a time (lion_run_batches()), and any other union too big for
	 * it is read as a windowed union, pinless (lion_plan_node()).  Both only
	 * happen past the budget, so an ordinary query is planned exactly as it
	 * always was.
	 */
	lion_open_budget_init(&budget, heap);

	/*
	 * A collection is never batched.  The passes of a batched list each
	 * yield every container key of their own, so a copy of them would come
	 * out of order; and the reason a COUNT batches a disjoint list rather than
	 * read it as a windowed union - that a batch keeps its pins, and so the
	 * DESIGN.md §9 interlock - is nothing to a collection, whose cursors drop
	 * every pin anyway (DESIGN.md §27).  A list too wide to open at once is
	 * therefore read the way any other too-wide union is (lion_plan_node()):
	 * a window of container keys at a time, its sets opened one after the
	 * other, every key in order.  That is what lets an IN list whose length
	 * the planner could not see - a parameter, an expression (§27) - be
	 * collected once however long it turns out, instead of being merged
	 * again by every count; such a list used to be refused a copy here.
	 */
	batchsrc = (summed || oneset || collect != NULL) ? -1 :
		lion_batch_source(nsources, sources, trees, &budget);

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
	 *	   it yields, which is what lion_source_pinned() decides - under the
	 *	   open budget the cursors will be built with, which makes a union too
	 *	   wide for it no carrier at all.  A list taken in batches is priced
	 *	   whole instead: each batch of it is pinned exactly when all of its
	 *	   sets are.
	 *
	 * A negated set may always be copied: a stale copy can only hold TIDs
	 * whose rows are dead (a live row's key cannot change without the row
	 * getting a new TID), and subtracting a dead TID cannot take a live row
	 * out of the count.
	 *
	 * And the copies are BUDGETED (DESIGN.md §15, "Bounded cursors").  They
	 * live as long as the sets do - for a GROUP BY, the whole of a relation's
	 * turn - and a list on another column made every one of its CHAIN sets a
	 * copy, up to 256 kB each, with nothing bounding the total.  So all the
	 * copies this count's sources hold, those made by earlier counts of the
	 * same sets included, stay within work_mem; a set that does not fit is
	 * walked page by page, as a set too big to copy always was.
	 */
	carry = (bool *) palloc0(sizeof(bool) * nsources);
	ncarry = 0;
	matheld = 0;
	for (i = 0; i < nsources; i++)
	{
		for (j = 0; j < sources[i].nsets; j++)
		{
			if (sources[i].sets[j].found)
				sources[i].sets[j].nuses++;
			if (sources[i].sets[j].mat != NULL)
				matheld += sources[i].sets[j].mat->held;
		}

		if (sources[i].negated)
			continue;
		carry[i] = lion_source_pinned(trees[i], sources[i].sets,
									  i == batchsrc ? NULL : &budget);
		if (carry[i])
			ncarry++;
	}

	/*
	 * A summed source counts every set exactly once, so there is nothing a
	 * private copy could save; skipping the decision also keeps the one
	 * positive source of each pass on the pinned path by construction.  A
	 * collection reads each set once too.
	 */
	for (i = 0; !summed && collect == NULL && i < nsources; i++)
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
			if (ps->matfailed)
				continue;		/* tried, and too big for what was left */
			if (matheld >= matbudget)
				continue;		/* the budget is spent */

			if (!lion_posting_set_materialize(ps, matbudget - matheld))
				continue;
			matheld += ps->mat->held;

			if (!sources[i].negated && carry[i] &&
				!lion_source_pinned(trees[i], sources[i].sets,
									i == batchsrc ? NULL : &budget))
			{
				carry[i] = false;
				ncarry--;
			}
		}
	}

	/*
	 * A collection counts nothing, keeps no answer and asks the map nothing
	 * (below): it has no use for the node's cache.
	 */
	if (collect != NULL)
		cache = NULL;

	/*
	 * The memory the count works in: the node's scratch context, reset when
	 * the count is over, where the caller keeps a cache for its execution
	 * (LionVisCache.scratch) - one AllocSet made per node instead of one per
	 * count, whose first block, and the two blocks past it that a merge's 8 kB
	 * work containers used to take, were allocated and freed again for every
	 * key of an FK-side join.
	 */
	if (cache != NULL && !cache->scratchbusy)
	{
		if (cache->scratch == NULL)
			cache->scratch = AllocSetContextCreate(cache->parent,
												   "lion index count",
												   ALLOCSET_DEFAULT_MINSIZE,
												   LION_COUNT_SCRATCH_BLOCK,
												   ALLOCSET_DEFAULT_MAXSIZE);
		cxt = cache->scratch;
		cache->scratchbusy = true;
		scratch = true;
	}
	else
		cxt = AllocSetContextCreate(CurrentMemoryContext,
									"lion index count",
									ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	memset(&cx, 0, sizeof(cx));
	cx.cxt = cxt;
	cx.heap = heap;
	cx.snapshot = snapshot;

	/*
	 * The visibility-map page the last count of this relation left pinned
	 * (LionVisCache.vmbuf): the map of a large heap is a few pages, and
	 * every count of the node asks the same one or two, so a count takes the
	 * pin over rather than pinning the page again - a buffer lookup and two
	 * atomic operations on a buffer header that every participant of a
	 * parallel plan shares.  A pin of another relation's map is let go.
	 */
	cx.vmbuf = InvalidBuffer;
	if (cache != NULL && BufferIsValid(cache->vmbuf))
	{
		if (cache->vmrelid == RelationGetRelid(heap))
		{
			cx.vmbuf = cache->vmbuf;
			cache->vmbuf = InvalidBuffer;
			cache->vmrelid = InvalidOid;
		}
		else
			lion_vis_cache_release_vm(cache);
	}
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
	 *
	 * The same goes for a union too wide for the open budget, which is read
	 * without pins (lion_wide_fill()).  Whether any positive source carries
	 * the interlock is therefore asked of the plans the cursors are built
	 * from, after the materialization above, by the merge path below - once,
	 * or once per batch of a list (lion_run_batches()).
	 */
	cx.novm = false;
	cx.rel_read_only = rel_read_only;
	cx.raw = true;
	cx.tids_sorted = true;
	cx.batchmax = lion_recheck_budget();
	cx.exists = exists;

	/*
	 * A collection counts nothing, so nothing it reads needs a pin once it
	 * has been copied: the cursors let go of every leaf as they go, as a
	 * bitmap walk's do (DESIGN.md §27).
	 */
	cx.collect = collect;
	if (collect != NULL)
		cx.droppins = true;

	/*
	 * The count's own key set, when source slot 0 is one (a group's set, an
	 * FK-side join's fk set): what stats.key_containers counts the reads of.
	 */
	cx.keyset = (collect == NULL && !sources[0].negated &&
				 sources[0].nsets == 1) ? &sources[0].sets[0] : NULL;

	/*
	 * The visibility cache, if the caller keeps one for this node execution.
	 * It is emptied here if it holds answers for another relation or another
	 * snapshot, so a partitioned count may hand the same handle to every
	 * partition (DESIGN.md §9 and §16).
	 */
	lion_vis_cache_begin(cache, heap, snapshot);
	cx.cache = cache;

	/*
	 * ... and the row filter the node asks of every count of this execution
	 * (DESIGN.md §17, "A query known only at run time").  A collection has no
	 * cache and is not filtered: it copies the superset, and the counts of
	 * that copy are.
	 */
	cx.filter = (cache != NULL) ? cache->filter : NULL;
	if (cx.filter != NULL && cx.filter->heap != heap)
		elog(ERROR, "lion index count: a row filter for another relation");

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
		 * sparse segment it expands: up to ten kilobytes or so, a thousand
		 * times over for the longest list the planner allows.  A context that
		 * is RESET after each pass hands the same memory out again, so the
		 * pass runs in cache instead of walking a dozen megabytes of fresh
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
	else if (batchsrc >= 0)
		lion_run_batches(&cx, nsources, sources, trees, batchsrc, &budget);
	else
	{
		LionNodePlan **plans;
		int			ncarried = 0;

		plans = (LionNodePlan **) palloc(sizeof(LionNodePlan *) * nsources);
		for (i = 0; i < nsources; i++)
		{
			plans[i] = lion_plan_build(trees[i], sources[i].sets, &budget,
									   false);
			if (!sources[i].negated && plans[i]->pinned)
				ncarried++;
		}
		cx.novm = (ncarried == 0);
		lion_run_merge(&cx, nsources, sources, plans);
	}

	/*
	 * Everything that could be answered from the visibility map has been;
	 * what is left of the last batch needs the heap and the snapshot.  No
	 * index page is pinned any more, which is fine: the decisions that needed
	 * a pin were all made above.
	 */
	lion_recheck_flush(&cx);
	result = cx.count + cx.recheck_count;

	/*
	 * The map page stays pinned for the next count, when there is a cache to
	 * keep it in (and nothing kept there since, which no count does).
	 */
	if (BufferIsValid(cx.vmbuf))
	{
		if (cache != NULL && !BufferIsValid(cache->vmbuf))
		{
			cache->vmbuf = cx.vmbuf;
			cache->vmrelid = RelationGetRelid(heap);
		}
		else
			ReleaseBuffer(cx.vmbuf);
	}

	MemoryContextSwitchTo(oldcxt);
	if (scratch)
	{
		MemoryContextReset(cxt);
		cache->scratchbusy = false;
	}
	else
		MemoryContextDelete(cxt);

	if (stats != NULL)
		lion_count_stats_add(stats, &cx.stats);

	return result;
}

void
lion_count_stats_add(LionCountStats *dst, const LionCountStats *src)
{
	dst->blocks_skipped_via_vm += src->blocks_skipped_via_vm;
	dst->tids_rechecked += src->tids_rechecked;
	dst->blocks_rechecked += src->blocks_rechecked;
	dst->containers_visited += src->containers_visited;
	dst->probes_avoided += src->probes_avoided;
	dst->unions_built += src->unions_built;
	dst->unions_probed += src->unions_probed;
	dst->cache_hits += src->cache_hits;
	dst->cache_full += src->cache_full;
	dst->sets_summed += src->sets_summed;
	dst->rows_removed += src->rows_removed;
	dst->key_containers += src->key_containers;
	dst->copy_containers += src->copy_containers;
	dst->copy_seeks += src->copy_seeks;
	dst->copy_file_reads += src->copy_file_reads;
	dst->vm_checks += src->vm_checks;
	dst->vm_pins += src->vm_pins;
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
 * A pull interface over the evaluator: the containers of one expression over
 * located posting sets, in ascending container key, one per call (DESIGN.md
 * §29.3).  This is what a plain index scan streams its TIDs from, and what
 * lion_sets_iterate() below - the bitmap scan's push form - loops over.
 *
 * keeppins says whether the cursors keep the §9 pin on the page each current
 * container came from until the stream moves past it (the count's rule, and
 * what a scan under a non-MVCC snapshot needs, §29.5), or let go of every
 * posting leaf as soon as they have copied it (a plain scan under an MVCC
 * snapshot).  An INLINE set's own pin is the caller's business either way:
 * lion_posting_set_unpin() it first when no pin is wanted.
 */
struct LionSetStream
{
	LionCountCtx cx;			/* the cursors' statistics and pin mode */
	LionExprCursor cursor;
	bool		empty;			/* no sets at all: nothing to stream */
	bool		first;			/* the cursor stands on the first container,
								 * which has not been handed out yet */
};

/*
 * budget NULL: every node built as it always was (a plain index scan's
 * stream, DESIGN.md §29.5, whose pins a non-MVCC snapshot relies on and
 * whose long lists lion_scan.c batches itself); otherwise the tree is planned
 * against it (lion_plan_node()), which is what the bitmap walk below wants.
 */
static LionSetStream *
lion_stream_begin_budget(int nsets, LionPostingSet *sets, LionKeyNode *tree,
						 bool keeppins, const LionOpenBudget *budget)
{
	LionSetStream *st = (LionSetStream *) palloc0(sizeof(LionSetStream));
	LionCountSource src;
	LionKeyNode *node;

	memset(&src, 0, sizeof(src));
	src.nsets = nsets;
	src.sets = sets;
	src.tree = tree;

	st->cx.vmbuf = InvalidBuffer;
	st->cx.droppins = !keeppins;

	node = lion_source_tree(&src);
	if (node == NULL)
	{
		st->empty = true;
		return st;
	}

	lion_ecursor_init(&st->cursor,
					  lion_plan_build(node, sets, budget, !keeppins),
					  sets, nsets, &st->cx, false);
	st->first = true;
	return st;
}

LionSetStream *
lion_stream_begin(int nsets, LionPostingSet *sets, LionKeyNode *tree,
				  bool keeppins)
{
	return lion_stream_begin_budget(nsets, sets, tree, keeppins, NULL);
}

/*
 * Skip every container key below target.  Only before the first
 * lion_stream_next(): the stream then starts at the first container at or
 * above target, reached by the cursors' seeks (§22) rather than a walk.
 */
void
lion_stream_seek(LionSetStream *st, uint32 target)
{
	Assert(st->first);
	if (!st->empty && st->cursor.valid && st->cursor.ckey < target)
		lion_ecursor_seek(&st->cursor, target);
}

/*
 * The next container, or NULL at the end.  It is valid until the next call:
 * moving past it is what lets go of the pins it was read under.
 */
const LionContainer *
lion_stream_next(LionSetStream *st)
{
	if (st->empty)
		return NULL;

	if (st->first)
		st->first = false;
	else
	{
		lion_ecursor_next(&st->cursor);
		CHECK_FOR_INTERRUPTS();
	}

	return st->cursor.valid ? st->cursor.cur : NULL;
}

const LionContainer *
lion_stream_at(LionSetStream *st, uint32 target)
{
	if (st->empty)
		return NULL;

	/*
	 * Standing on a container already handed out is standing on it still:
	 * the seek moves the cursors past every key below target, and nothing
	 * else.
	 */
	if (st->cursor.valid && st->cursor.ckey < target)
	{
		lion_ecursor_seek(&st->cursor, target);
		CHECK_FOR_INTERRUPTS();
	}
	st->first = false;

	return st->cursor.valid ? st->cursor.cur : NULL;
}

void
lion_stream_far(LionSetStream *st)
{
	st->cx.farseeks = true;
}

void
lion_stream_end(LionSetStream *st)
{
	if (!st->empty)
		lion_ecursor_close(&st->cursor);
	st->empty = true;
}

/*
 * The stream of lion_sets_iterate(), below, for a caller that pulls it: a
 * bitmap scan's intersection of a range with other columns (DESIGN.md §28,
 * "Bitmap scans").
 */
LionSetStream *
lion_stream_begin_bounded(int nsets, LionPostingSet *sets, LionKeyNode *tree)
{
	LionOpenBudget budget;
	Relation	rel = NULL;
	int			i;

	for (i = 0; i < nsets && rel == NULL; i++)
	{
		if (sets[i].found)
			rel = sets[i].index;
	}
	lion_open_budget_init(&budget, rel);
	return lion_stream_begin_budget(nsets, sets, tree, false, &budget);
}

/*
 * Walk the containers of one expression over located posting sets, without
 * any visibility-map interlock: what a bitmap scan of a multi-key opclass
 * (DESIGN.md §17) or of several key columns (§24) needs.  Every TID goes to
 * the executor, which visits the heap for all of them, so no pin has anything
 * to protect here: the cursors let go of every posting leaf as soon as they
 * have copied it (they used to keep them, one per CHAIN set of an IN list, for
 * nothing).  And the tree is planned against work_mem (DESIGN.md §15,
 * "Bounded cursors"), so a union too wide to open at once - the multicolumn
 * scan of `k = ANY ($1) AND x = 1` over a list of 100000 values held 1.7 GB -
 * is read as a windowed union, in ascending container key like any other.
 */
int64
lion_sets_iterate(int nsets, LionPostingSet *sets, LionKeyNode *tree,
				 lion_container_callback cb, void *arg)
{
	LionSetStream *st;
	const LionContainer *c;
	int64		total = 0;

	st = lion_stream_begin_bounded(nsets, sets, tree);

	while ((c = lion_stream_next(st)) != NULL)
	{
		total += (int64) lion_container_cardinality(c);
		if (!cb(c, arg))
			break;
	}

	lion_stream_end(st);
	pfree(st);

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
 * Range restrictions (DESIGN.md §28)
 * --------------------------------------------------------------------- */


void
lion_range_init(LionRange *range, Relation index, AttrNumber attno)
{
	memset(range, 0, sizeof(LionRange));
	range->state = lion_index_column_state(index, attno);
	range->ordered = range->state->ordered;
	range->lower = -1;
	range->upper = -1;
	range->nupper = 0;
	range->empty = false;
}

/*
 * Add one bound, `key <strategy> value`, to the range.  opfuncid is the
 * function of the operator the clause names, and valtype the type of value;
 * isnull says the value is NULL, which no strict comparison is satisfied by,
 * so the range is then empty.
 *
 * The comparison is the one §21's probe resolution finds for a value of this
 * type (lion_probe_init()), so the walk reads the directory in exactly the
 * order a lookup of the same value would: the column's own proc 4 for its own
 * type and for a binary coercion to it, the family's cross-type proc 4
 * otherwise - btint48cmp for an int8 bound on an int4 column, which compares
 * the two widths exactly, so a bound outside the column's domain needs no
 * special case.  Where that resolution finds none, the bound is tested with
 * the operator itself and the walk can no longer be bounded at all.
 */
void
lion_range_add(LionRange *range, Relation index, StrategyNumber strategy,
			   Oid opfuncid, Oid valtype, Datum value, bool isnull,
			   Oid collation)
{
	LionRangeBound *b;
	LionProbe	probe;

	if (!LION_STRAT_IS_RANGE(strategy))
		elog(ERROR, "lion index: strategy %d is not a range comparison",
			 (int) strategy);

	if (isnull)
	{
		range->empty = true;
		return;
	}

	if (range->nbounds >= range->maxbounds)
	{
		range->maxbounds = Max(4, range->maxbounds * 2);
		range->bounds = (range->bounds == NULL) ?
			(LionRangeBound *) palloc0(sizeof(LionRangeBound) * range->maxbounds) :
			(LionRangeBound *) repalloc(range->bounds,
										sizeof(LionRangeBound) * range->maxbounds);
	}
	b = &range->bounds[range->nbounds];
	memset(b, 0, sizeof(LionRangeBound));
	b->strategy = strategy;
	b->value = value;
	b->collation = collation;

	/*
	 * A class declared on a polymorphic type (enum_ops, FOR TYPE anyenum)
	 * compares its keys with values of the column's own type, whatever that
	 * type is called: they are the class's own, not a cross-type search.
	 */
	if (IsPolymorphicType(index->rd_opcintype[range->state->attno - 1]))
		valtype = InvalidOid;

	lion_probe_init(index, range->state, valtype, &probe);
	if (probe.hascmp && !probe.needscan)
	{
		fmgr_info_copy(&b->cmpproc, &probe.cmpproc, CurrentMemoryContext);
		b->hascmp = true;
	}
	else
	{
		fmgr_info(opfuncid, &b->opproc);
		b->hascmp = false;
		range->ordered = false;
	}

	if (LION_STRAT_IS_LOWER(strategy))
	{
		if (range->lower < 0)
			range->lower = range->nbounds;
	}
	else
	{
		range->upper = (range->nupper == 0) ? range->nbounds : -1;
		range->nupper++;
	}
	range->nbounds++;
}

/* Does the stored key satisfy one bound? */
static bool
lion_range_bound_ok(LionRange *range, LionRangeBound *b, Datum key)
{
	int32		c;

	if (!b->hascmp)
		return DatumGetBool(FunctionCall2Coll(&b->opproc, b->collation,
											  key, b->value));

	c = DatumGetInt32(FunctionCall2Coll(&b->cmpproc, range->state->collation,
										key, b->value));
	switch (b->strategy)
	{
		case LION_STRAT_LT:
			return c < 0;
		case LION_STRAT_LE:
			return c <= 0;
		case LION_STRAT_GE:
			return c >= 0;
		default:
			return c > 0;
	}
}

/*
 * Does one entry of the range's column satisfy every bound?
 *
 * The reserved entries never do: a NULL key satisfies no comparison and the
 * EMPTY entry has no key at all.  In an ORDERED range the first entry that
 * fails an UPPER bound ends the walk - every later entry sorts at or above it
 * (the order leads with proc 4 within a column and a kind, §21) and so fails
 * that bound too - while one that fails only a LOWER bound is skipped, and the
 * entries that do are a prefix of what the walk visits: those of the landing
 * leaf below the bound it descended to, and those below any other lower bound.
 * Without an order nothing ends the walk early.
 *
 * The caller has checked the entry's column and holds the page it is on.
 */
int
lion_range_test(LionRange *range, const LionEntryTuple *entry)
{
	LionState  *state = range->state;
	Datum		key;
	bool		skip = false;
	int			i;

	if (range->empty)
		return LION_RANGE_END;
	if (lion_entry_kind(entry) != LION_KIND_VALUE)
		return LION_RANGE_SKIP;

	key = lion_fetch_key(state, LionEntryGetKey(entry));

	for (i = 0; i < range->nbounds; i++)
	{
		LionRangeBound *b = &range->bounds[i];

		if (lion_range_bound_ok(range, b, key))
			continue;
		if (range->ordered && !LION_STRAT_IS_LOWER(b->strategy))
			return LION_RANGE_END;
		skip = true;
	}

	return skip ? LION_RANGE_SKIP : LION_RANGE_MATCH;
}

/*
 * Does a VALUE entry of the range's column fail an UPPER bound?  In an ordered
 * range those entries are the column's last ones (the walk ABOVE the range,
 * DESIGN.md §28), and every entry after the first of them fails one too.
 */
bool
lion_range_fails_upper(LionRange *range, const LionEntryTuple *entry)
{
	Datum		key;
	int			i;

	Assert(lion_entry_kind(entry) == LION_KIND_VALUE);
	key = lion_fetch_key(range->state, LionEntryGetKey(entry));

	for (i = 0; i < range->nbounds; i++)
	{
		LionRangeBound *b = &range->bounds[i];

		if (!LION_STRAT_IS_LOWER(b->strategy) &&
			!lion_range_bound_ok(range, b, key))
			return true;
	}
	return false;
}

/*
 * Where a descent for bound number `bound` lands among the column's entries of
 * `kind` - VALUE, or SUMMARY for the summaries of DESIGN.md §32 - and a copy of
 * the first item at or after that position, moving right past a leaf whose
 * last item is below it; *posp is NULL at the very end of the directory.  The
 * leaf returned is the one that item is on.
 *
 * The descent is an ordinary lookup's (lion_dir_search()) with a search key of
 * the kind whose key is the bound, whose hash is 0 and which has no stored
 * form, so it compares as the SMALLEST member of its own run (§21): the item
 * it lands on is the first one whose proc 4 is not below the bound.  The walk
 * that starts there re-reads the leaf with nothing held in between, which is
 * safe for the reason every resumed entry scan is: a split moves entries only
 * rightwards, onto a page the walk has yet to reach, and a directory leaf is
 * never unlinked (§21).
 *
 * The item is what puts the landings of SEVERAL bounds in order
 * (lion_cmp_entries(), the directory order itself), which is how a walk finds
 * the tightest of them (lion_range_side_leaf()).
 *
 * An item that sorts BEFORE the search key is stepped over, as
 * lion_dir_search_first() does: never there on a sound directory, it is what
 * a descent to a summary lands on in an index an earlier version damaged (a
 * SUMLAST pivot above the summary it routes to, DESIGN.md §32), and taking it
 * as E_j made the walk count whole buckets below the range's bound.
 */
static BlockNumber
lion_range_landing(Relation index, LionRange *range, int bound, int kind,
				   LionEntryTuple **posp)
{
	LionRangeBound *b = &range->bounds[bound];
	LionSearchKey sk;
	Buffer		buf;
	OffsetNumber off;
	BlockNumber blk;

	Assert(b->hascmp);
	lion_search_key_init(range->state, &sk, kind, b->value, 0);
	sk.cmpproc = &b->cmpproc;

	buf = lion_dir_search(index, NULL, range->state->ix, &sk,
						  BUFFER_LOCK_SHARE, false, &off);
	for (;;)
	{
		Page		page = BufferGetPage(buf);

		if (off <= PageGetMaxOffsetNumber(page))
		{
			ItemId		iid = PageGetItemId(page, off);

			if (lion_cmp_entry(lion_page_entry(page, off), &sk) < 0)
			{
				off = OffsetNumberNext(off);
				continue;
			}
			*posp = (LionEntryTuple *) palloc(ItemIdGetLength(iid));
			memcpy(*posp, PageGetItem(page, iid), ItemIdGetLength(iid));
			break;
		}
		if (LionPageIsRightmost(page))
		{
			*posp = NULL;
			break;
		}
		buf = lion_dir_step_right(index, buf, BUFFER_LOCK_SHARE);
		off = lion_page_first_data(BufferGetPage(buf));
	}
	blk = BufferGetBlockNumber(buf);
	UnlockReleaseBuffer(buf);

	return blk;
}

/*
 * The landing of the TIGHTEST bound of one side of an ordered range, among the
 * column's entries of `kind`: of its LOWER bounds the one that lands furthest
 * right, of its UPPER bounds the one that lands furthest left.  Every entry
 * before the first fails that lower bound, so a walk of what the range selects
 * may start there; every entry before the second passes every upper bound, so
 * a walk of what lies ABOVE the range must start there and may.  With one
 * bound of the side this is that bound's descent, as it always was; with two
 * (`k < 10 AND k <= $1`) it used to be the first one's, or - for the walk
 * above - where the range itself started, which made the complement no
 * cheaper than the range (DESIGN.md §28, fixed in §32).
 *
 * Returns InvalidBlockNumber when the side has no bound.  *posp is the copy
 * of the item the landing marks, NULL at the end of the directory.
 */
static BlockNumber
lion_range_side_leaf(Relation index, LionRange *range, bool lower, int kind,
					 LionEntryTuple **posp)
{
	BlockNumber best = InvalidBlockNumber;
	LionEntryTuple *bestpos = NULL;
	int			i;

	/*
	 * The column state is the index's relcache entry's, and a relcache
	 * invalidation since the range was built - any catalog read may process
	 * one - frees the LionIndexState it points back to, which the descent
	 * reads the cached root from.  The column states themselves live on in
	 * rd_indexcxt, so the stale one still says which column it is; look the
	 * current one up by that and re-point the range (the bounds' comparison
	 * functions are copies and need nothing).
	 */
	range->state = lion_index_column_state(index, range->state->attno);

	for (i = 0; i < range->nbounds; i++)
	{
		LionEntryTuple *pos;
		BlockNumber blk;
		bool		better;

		if (LION_STRAT_IS_LOWER(range->bounds[i].strategy) != lower)
			continue;

		blk = lion_range_landing(index, range, i, kind, &pos);
		if (!BlockNumberIsValid(best))
			better = true;
		else if (pos == NULL || bestpos == NULL)
			better = lower ? (pos == NULL && bestpos != NULL) :
				(bestpos == NULL && pos != NULL);
		else
		{
			int			c = lion_cmp_entries(range->state->ix, pos, bestpos);

			better = lower ? (c > 0) : (c < 0);
		}

		if (better)
		{
			if (bestpos != NULL)
				pfree(bestpos);
			best = blk;
			bestpos = pos;
		}
		else if (pos != NULL)
			pfree(pos);
	}

	if (posp != NULL)
		*posp = bestpos;
	else if (bestpos != NULL)
		pfree(bestpos);
	return best;
}

/*
 * The directory leaf a walk of the range starts on: where the first entry at
 * or above its tightest lower bound lives, or - without a lower bound, or
 * without an order to descend by - where the column's entries begin.
 */
BlockNumber
lion_range_first_leaf(Relation index, LionRange *range)
{
	BlockNumber blk = InvalidBlockNumber;

	if (range->ordered)
		blk = lion_range_side_leaf(index, range, true, LION_KIND_VALUE, NULL);
	if (!BlockNumberIsValid(blk))
	{
		range->state = lion_index_column_state(index, range->state->attno);
		blk = lion_dir_column_first(index, range->state, NULL);
	}
	return blk;
}

/*
 * Where a column's summaries begin (DESIGN.md §32): the leaf a descent to
 * (attno, SUMMARY) with no key lands on.  A search key of kind SUMMARY and no
 * comparison compares only on its hash, 0, and without a stored form it is the
 * smallest member of its run, so it sorts below every summary of the column
 * and above every value.
 */
static BlockNumber
lion_summary_first_leaf(Relation index, LionState *col)
{
	LionSearchKey sk;
	Buffer		buf;
	OffsetNumber off;
	BlockNumber blk;

	lion_search_key_init(col, &sk, LION_KIND_SUMMARY, (Datum) 0, 0);
	sk.cmpproc = NULL;

	buf = lion_dir_search(index, NULL, col->ix, &sk, BUFFER_LOCK_SHARE, false,
						  &off);
	blk = BufferGetBlockNumber(buf);
	UnlockReleaseBuffer(buf);

	return blk;
}

/*
 * How many summaries a column has and the rows they hold, read off the
 * entries' counters from where the column's summaries begin - at most
 * LION_SUMMARY_SHAPE_LEAVES leaves of them, the first buckets, which is where
 * keys that arrive in descending order go (DESIGN.md §32, "Costs").  The cost
 * of a summed range needs it: buckets close at summary_tids rows only when
 * keys arrive in order, and a column whose keys arrive in descending order -
 * or in none - puts its rows into a few buckets far larger than that, whose
 * keys a range walks one by one.  complete says every summary of the column
 * was read.
 */
void
lion_summary_shape(Relation index, LionState *col, LionSumShape *shape)
{
	LionSearchKey sk;
	Buffer		buf;
	OffsetNumber off;
	int			nleaves = 1;

	memset(shape, 0, sizeof(LionSumShape));

	/* where the column's summaries begin, as lion_summary_first_leaf() */
	lion_search_key_init(col, &sk, LION_KIND_SUMMARY, (Datum) 0, 0);
	sk.cmpproc = NULL;
	buf = lion_dir_search(index, NULL, col->ix, &sk, BUFFER_LOCK_SHARE, false,
						  &off);
	for (;;)
	{
		Page		page = BufferGetPage(buf);
		OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

		for (; off <= maxoff; off++)
		{
			LionEntryTuple *e = lion_page_entry(page, off);

			if (e->attno != col->attno || !LionEntryIsSummary(e))
			{
				shape->complete = true;
				break;
			}
			shape->nsummaries += 1.0;
			shape->rows += (double) e->ntids;
		}
		if (shape->complete || LionPageIsRightmost(page))
		{
			shape->complete = true;
			break;
		}
		if (nleaves >= LION_SUMMARY_SHAPE_LEAVES)
			break;
		buf = lion_dir_step_right(index, buf, BUFFER_LOCK_SHARE);
		off = lion_page_first_data(BufferGetPage(buf));
		nleaves++;
	}
	UnlockReleaseBuffer(buf);
}

/* The leaf a descent to (attno, VALUE, key) lands on, key a stored one. */
static BlockNumber
lion_value_leaf(Relation index, LionState *col, const char *raw)
{
	LionSearchKey sk;
	Buffer		buf;
	OffsetNumber off;
	BlockNumber blk;

	lion_search_key_init(col, &sk, LION_KIND_VALUE, lion_fetch_key(col, raw),
						 0);
	buf = lion_dir_search(index, NULL, col->ix, &sk, BUFFER_LOCK_SHARE, false,
						  &off);
	blk = BufferGetBlockNumber(buf);
	UnlockReleaseBuffer(buf);

	return blk;
}


/* ---------------------------------------------------------------------
 * Iterating every entry of an index
 * --------------------------------------------------------------------- */

/*
 * The most entries one leaf can hold: every item at least an entry header and
 * a line pointer.  A batch is at most one leaf's worth (DESIGN.md §28).
 */
#define LION_LEAF_MAX_ENTRIES \
	((int) (BLCKSZ / (MAXALIGN(LION_ENTRY_HDRSZ) + sizeof(ItemIdData))) + 1)

/* Everything but where the walk starts. */
static void
lion_entry_scan_init(LionEntryScan *es, Relation index, AttrNumber attno)
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
	 * kind any stored entry has, so "resume after the last key examined" -
	 * which is what every leaf read does - starts at the column's first entry
	 * without a special case.
	 */
	es->lastkind = LION_KIND_MINF;
	es->lasthash = 0;
	es->lastkeylen = 0;
	es->lastkey = (char *) MemoryContextAllocZero(es->cxt, 1);
	es->haslast = true;
	es->blkno = InvalidBlockNumber;
	es->range = NULL;
	es->part = LION_WALK_ALL;
	es->done = false;

	/* No summaries unless lion_entry_scan_begin_sum() plans them (§32). */
	es->usesum = false;
	es->phase = LION_PHASE_VALUES;
	es->nextphase = LION_PHASE_VALUES;
	es->resumekind = -1;
	es->hasclipmax = false;
	es->hasclipmin = false;
	es->hassumprev = false;
	es->clipmax = es->clipmin = es->sumprev = NULL;
	es->clipmaxlen = es->clipminlen = es->sumprevlen = 0;
	es->sumprevhash = 0;
	es->phasecxt = NULL;
	es->nsummaries = 0;

	/*
	 * The batch lives beside the position and is allocated once: one leaf's
	 * entries fit in a block's worth of bytes, because that is where they
	 * were copied from.
	 */
	es->batchcxt = AllocSetContextCreate(CurrentMemoryContext,
										 "lion entry scan batch",
										 ALLOCSET_DEFAULT_SIZES);
	es->maxbatch = LION_LEAF_MAX_ENTRIES;
	es->bentry = (LionEntryTuple **)
		MemoryContextAlloc(es->batchcxt, sizeof(LionEntryTuple *) * es->maxbatch);
	es->bsize = (Size *)
		MemoryContextAlloc(es->batchcxt, sizeof(Size) * es->maxbatch);
	es->boff = (OffsetNumber *)
		MemoryContextAlloc(es->batchcxt, sizeof(OffsetNumber) * es->maxbatch);
	es->bpage = (char *) MemoryContextAlloc(es->batchcxt, BLCKSZ);
	es->nbatch = 0;
	es->nextbatch = 0;
	es->lastinline = -1;
	es->batchblk = InvalidBlockNumber;
	es->batchbuf = InvalidBuffer;
	es->nleaves = 0;
}

void
lion_entry_scan_begin_col(LionEntryScan *es, Relation index, AttrNumber attno)
{
	lion_entry_scan_init(es, index, attno);
	es->blkno = lion_dir_column_first(index, es->state, NULL);
}

/*
 * The walk of one column bounded by a range (DESIGN.md §28).  Only where it
 * STARTS differs from lion_entry_scan_begin_col(): the leaf the range's lower
 * bound lives on instead of the column's first.  The resume position is still
 * the column's (attno, MINF), so the entries of an earlier column that share
 * that leaf are passed by the ordinary resume comparison, and the entries of
 * this column below the bound by lion_range_test().
 */
void
lion_entry_scan_begin_range(LionEntryScan *es, Relation index,
							AttrNumber attno, LionRange *range)
{
	if (range == NULL)
		lion_entry_scan_begin_col(es, index, attno);
	else
		lion_entry_scan_begin_part(es, index, attno, range, LION_WALK_INSIDE);
}

/*
 * ... and of the parts of it the range does NOT select (DESIGN.md §28, "The
 * complement").  BELOW starts where the column does and stops at the first
 * entry the range selects or that fails an upper bound.  ABOVE starts where
 * the first entry that fails an upper bound lives - the landing of the
 * tightest upper bound (lion_range_side_leaf()) - and returns every entry that
 * fails one until the column ends.  INSIDE starts at the landing of the
 * tightest lower bound.
 */
void
lion_entry_scan_begin_part(LionEntryScan *es, Relation index,
						   AttrNumber attno, LionRange *range, int part)
{
	lion_entry_scan_init(es, index, attno);

	/* The scan's state is the current one (see lion_range_side_leaf()). */
	range->state = es->state;
	es->range = range;
	es->part = part;
	if (range->empty)
	{
		es->done = true;
		return;
	}

	switch (part)
	{
		case LION_WALK_INSIDE:
			es->blkno = lion_range_first_leaf(index, range);
			break;
		case LION_WALK_BELOW:
			Assert(range->ordered);
			es->blkno = lion_dir_column_first(index, es->state, NULL);
			break;
		case LION_WALK_ABOVE:
			Assert(range->ordered);
			if (range->nupper == 0)
			{
				es->done = true;
				return;
			}
			es->blkno = lion_range_side_leaf(index, range, false,
											 LION_KIND_VALUE, NULL);
			break;
		default:
			elog(ERROR, "lion index: unknown part %d of a range walk", part);
	}
	range->state = es->state = lion_index_column_state(index, attno);
}

/*
 * Keep a copy of an entry's stored key in *bufp, a buffer of the walk's phase
 * context that holds any key (LION_MAX_KEY_SIZE): a SUMS phase copies the key
 * of every bucket it takes into the same one.
 */
static void
lion_scan_keycopy(LionEntryScan *es, const LionEntryTuple *entry, char **bufp,
				  Size *lenp)
{
	if (unlikely(entry->keylen > LION_MAX_KEY_SIZE))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": a summary key of %u bytes",
						RelationGetRelationName(es->index), entry->keylen)));
	if (*bufp == NULL)
		*bufp = (char *) MemoryContextAlloc(es->phasecxt, LION_MAX_KEY_SIZE);
	memcpy(*bufp, LionEntryGetKey(entry), entry->keylen);
	*lenp = entry->keylen;
}

/*
 * Compare a stored key of the walk's column with another stored one, under the
 * column's own comparison: how a key is put on one side or the other of a
 * bucket boundary (DESIGN.md §32).
 */
static int
lion_scan_keycmp(LionEntryScan *es, const char *a, const char *b)
{
	LionState  *st = es->state;

	return DatumGetInt32(FunctionCall2Coll(&st->cmpproc, st->collation,
										   lion_fetch_key(st, a),
										   lion_fetch_key(st, b)));
}

/*
 * Is the bucket whose summary this is - the keys above the previous summary's
 * key and at or below this one's - wholly in the walk's part?  The SUMS phase
 * starts at a bucket whose lower boundary is already on the right side of the
 * part's lower end (lion_entry_scan_begin_sum()), and every later one's is too,
 * so what is left to ask is whether the bucket's UPPER boundary - its key, the
 * largest key it may hold - is on the right side of the part's upper end:
 *
 *	INSIDE	it passes every upper bound;
 *	BELOW	it fails a lower bound and passes every upper bound;
 *	ABOVE, ALL	there is no upper end.
 *
 * The last summary of a column keeps the largest key its bucket holds, which
 * is the same question asked of its keys so far; a key an insert adds above
 * it later belongs to a row this walk's snapshot cannot see.
 */
static bool
lion_scan_bucket_inside(LionEntryScan *es, const LionEntryTuple *entry)
{
	LionRange  *range = es->range;
	bool		failslower = false;
	Datum		key;
	int			i;

	if (es->part == LION_WALK_ALL || es->part == LION_WALK_ABOVE)
		return true;

	key = lion_fetch_key(es->state, LionEntryGetKey(entry));
	for (i = 0; i < range->nbounds; i++)
	{
		LionRangeBound *b = &range->bounds[i];
		bool		ok = lion_range_bound_ok(range, b, key);

		if (LION_STRAT_IS_LOWER(b->strategy))
		{
			if (!ok)
				failslower = true;
		}
		else if (!ok)
			return false;
	}
	return (es->part == LION_WALK_INSIDE) ? true : failslower;
}

/*
 * Plan a walk that uses the column's summaries (DESIGN.md §32): find the first
 * bucket the part covers whole, and set the walk up for the phase it starts
 * in.  The buckets of a column are (previous summary's key, this summary's
 * key], in key order, and a bucket is wholly in the part when both of its
 * boundaries are on the right side of the part's ends:
 *
 *	- the lower one.  INSIDE and ABOVE have a lower end: the first summary at
 *	  or above the TIGHTEST bound that makes it - the range's tightest lower
 *	  bound, or the tightest upper bound for ABOVE, whose rows are the ones
 *	  that fail one - is E_j, and every bucket after it has only keys above
 *	  E_j's key, which is at or above that bound and every other one of that
 *	  side.  So the first whole bucket is E_j's successor, and the part's
 *	  VALUE entries at or below E_j's key are walked first, one by one (LOWER).
 *	  BELOW and ALL start at the column's first bucket.
 *	- the upper one, the summary's own key: lion_scan_bucket_inside(), asked
 *	  of each summary in turn (SUMS).  The first that fails ends the phase, and
 *	  the part's VALUE entries above the last whole bucket are walked one by one
 *	  (UPPER).
 *
 * When E_j is the column's last summary, or not a summary of this column at
 * all, or its key is already above the part's upper end, no bucket is whole
 * and the walk is the plain one.  Returns whether summaries are used.
 */
static bool
lion_entry_scan_plan_sum(LionEntryScan *es, Relation index, LionRange *range,
						 int part)
{
	LionState  *col = es->state;
	LionEntryTuple *ej = NULL;
	BlockNumber jblk = InvalidBlockNumber;
	bool		haslowerend;

	if (!col->summarized)
		return false;
	if (range != NULL && (!range->ordered || range->empty))
		return false;
	if (part == LION_WALK_ABOVE && range->nupper == 0)
		return false;

	haslowerend = (part == LION_WALK_ABOVE) ||
		(part == LION_WALK_INSIDE && range->lower >= 0);

	if (haslowerend)
	{
		jblk = lion_range_side_leaf(index, range, part != LION_WALK_ABOVE,
									LION_KIND_SUMMARY, &ej);
		if (!BlockNumberIsValid(jblk) || ej == NULL ||
			LionEntryIsPivot(ej) || ej->attno != col->attno ||
			lion_entry_kind(ej) != LION_KIND_SUMMARY)
			return false;

		/*
		 * A range whose upper end is already below E_j's key ends inside E_j's
		 * bucket: there is nothing whole to read.
		 */
		if (part == LION_WALK_INSIDE && !lion_scan_bucket_inside(es, ej))
			return false;
	}

	es->usesum = true;

	/*
	 * Not a child of cxt: that one is reset at every leaf read, and a reset
	 * deletes a context's children.
	 */
	es->phasecxt = AllocSetContextCreate(CurrentMemoryContext,
										 "lion entry scan phases",
										 ALLOCSET_SMALL_SIZES);
	if (haslowerend)
	{
		/* LOWER: the part's values up to E_j's key, from the part's start. */
		es->phase = LION_PHASE_LOWER;
		es->hasclipmax = true;
		lion_scan_keycopy(es, ej, &es->clipmax, &es->clipmaxlen);
		es->hassumprev = true;
		lion_scan_keycopy(es, ej, &es->sumprev, &es->sumprevlen);
		es->sumprevhash = ej->hash;
		es->blkno = (part == LION_WALK_INSIDE) ?
			lion_range_first_leaf(index, range) :
			lion_range_side_leaf(index, range, false, LION_KIND_VALUE, NULL);
	}
	else
	{
		/* SUMS from the column's first summary. */
		es->phase = LION_PHASE_SUMS;
		es->resumekind = LION_KIND_VALUE;
		es->blkno = lion_summary_first_leaf(index, col);
	}
	es->nextphase = es->phase;
	if (ej != NULL)
		pfree(ej);
	es->state = lion_index_column_state(index, es->attno);
	if (range != NULL)
		range->state = es->state;
	return true;
}

bool
lion_entry_scan_begin_summed(LionEntryScan *es, Relation index,
							 AttrNumber attno, LionRange *range, int part)
{
	lion_entry_scan_init(es, index, attno);
	es->range = range;
	es->part = (range == NULL) ? LION_WALK_ALL : part;
	if (range != NULL)
		range->state = es->state;

	if ((range == NULL || !range->empty) &&
		lion_entry_scan_plan_sum(es, index, range, es->part))
		return true;

	lion_entry_scan_end(es);
	return false;
}

void
lion_entry_scan_begin_sum(LionEntryScan *es, Relation index, AttrNumber attno,
						  LionRange *range, int part)
{
	if (range != NULL && range->empty)
	{
		lion_entry_scan_init(es, index, attno);
		es->range = range;
		es->part = part;
		range->state = es->state;
		es->done = true;
		return;
	}

	if (lion_entry_scan_begin_summed(es, index, attno, range, part))
		return;

	/* No summaries to use: the plain walk. */
	if (range == NULL)
		lion_entry_scan_begin_col(es, index, attno);
	else
		lion_entry_scan_begin_part(es, index, attno, range, part);
}

/*
 * The phase that just ended hands over to the next one, which the next leaf
 * read sets up (lion_entry_scan_setup_phase()) - with nothing pinned, which a
 * descent in the middle of a leaf read could not promise.
 */
static void
lion_entry_scan_end_phase(LionEntryScan *es, bool partended)
{
	switch (es->phase)
	{
		case LION_PHASE_LOWER:
			/* A range that ended at or below E_j's key has nothing after it. */
			es->nextphase = partended ? LION_PHASE_DONE : LION_PHASE_SUMS;
			break;
		case LION_PHASE_SUMS:
			/* The column's summaries ran out: every bucket was whole. */
			es->nextphase = partended ? LION_PHASE_DONE : LION_PHASE_UPPER;
			break;
		default:
			es->nextphase = LION_PHASE_DONE;
			break;
	}
	if (es->nextphase == LION_PHASE_DONE)
		es->done = true;
	es->blkno = InvalidBlockNumber;
}

static void
lion_entry_scan_setup_phase(LionEntryScan *es)
{
	/*
	 * Test hook: one phase of a summed walk is over and the next is about to
	 * descend to where it begins (DESIGN.md §32), holding nothing of the
	 * directory.  test/isolation/summary_race.spec parks here while inserts
	 * close and open buckets and split the leaves the next phase reads.
	 * Compiles to nothing without --enable-injection-points.
	 */
	LION_INJECTION_POINT("lion-entry-scan-phase");

	switch (es->nextphase)
	{
		case LION_PHASE_SUMS:

			/*
			 * After E_j, whose leaf a descent to its key finds again: the
			 * summaries resume after (SUMMARY, E_j's key).
			 */
			Assert(es->hassumprev);
			MemoryContextReset(es->cxt);
			es->hasclipmax = false;
			es->lastkind = LION_KIND_SUMMARY;
			es->lasthash = es->sumprevhash;
			es->lastkeylen = es->sumprevlen;
			es->lastkey = (char *) MemoryContextAlloc(es->cxt,
													  Max(es->sumprevlen, 1));
			memcpy(es->lastkey, es->sumprev, es->sumprevlen);
			es->haslast = true;
			es->resumekind = -1;
			{
				LionSearchKey sk;
				Buffer		buf;
				OffsetNumber off;

				es->state = lion_index_column_state(es->index, es->attno);
				lion_search_key_init(es->state, &sk, LION_KIND_SUMMARY,
									 lion_fetch_key(es->state, es->sumprev), 0);
				buf = lion_dir_search(es->index, NULL, es->state->ix, &sk,
									  BUFFER_LOCK_SHARE, false, &off);
				es->blkno = BufferGetBlockNumber(buf);
				UnlockReleaseBuffer(buf);
			}
			break;

		case LION_PHASE_UPPER:

			/*
			 * The part's values above the last whole bucket: from where a
			 * descent to its key lands, every key at or below it passed over
			 * by the boundary test, which is what decides - the resume
			 * position only saves reading the entries before it.
			 */
			es->state = lion_index_column_state(es->index, es->attno);
			MemoryContextReset(es->cxt);
			es->lastkind = LION_KIND_MINF;
			es->lasthash = 0;
			es->lastkeylen = 0;
			es->lastkey = (char *) MemoryContextAllocZero(es->cxt, 1);
			es->haslast = true;
			es->resumekind = -1;
			if (es->hassumprev)
			{
				es->hasclipmin = true;
				es->clipmin = (char *) MemoryContextAlloc(es->phasecxt,
														  Max(es->sumprevlen, 1));
				memcpy(es->clipmin, es->sumprev, es->sumprevlen);
				es->clipminlen = es->sumprevlen;
				es->blkno = lion_value_leaf(es->index, es->state, es->sumprev);
			}
			else
			{
				/*
				 * The very first bucket was not whole: the part is walked from
				 * its own start, as if there were no summaries.
				 */
				es->hasclipmin = false;
				es->blkno = (es->part == LION_WALK_INSIDE) ?
					lion_range_first_leaf(es->index, es->range) :
					lion_dir_column_first(es->index, es->state, NULL);
			}
			break;

		default:
			Assert(false);
	}
	if (es->range != NULL)
		es->range->state = es->state;
	es->phase = es->nextphase;
}

/* Remember where to resume, as a KEY (see the comment on LionEntryScan). */
static void
lion_entry_scan_remember(LionEntryScan *es, const LionEntryTuple *entry)
{
	MemoryContextReset(es->cxt);
	es->lastkind = lion_entry_kind(entry);
	es->lasthash = entry->hash;
	es->lastkeylen = entry->keylen;
	es->resumekind = -1;

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
 * Does the walk return this entry of its column?  *stop is set when no later
 * entry of the phase can be returned either: the first entry past an upper
 * bound ends a walk of what a range selects (DESIGN.md §28), and the first
 * entry the range selects - or that fails an upper bound - ends the walk below
 * it.  *partended says the PART ended there, not only the phase.
 */
static bool
lion_entry_scan_selects(LionEntryScan *es, const LionEntryTuple *entry,
						bool *stop, bool *partended)
{
	int			kind = lion_entry_kind(entry);
	int			r;

	*partended = false;

	/* The summaries of whole buckets (DESIGN.md §32). */
	if (es->phase == LION_PHASE_SUMS)
	{
		if (kind < LION_KIND_SUMMARY)
			return false;		/* the column's values, on the landing leaf */
		if (!lion_scan_bucket_inside(es, entry))
		{
			*stop = true;
			return false;
		}
		lion_scan_keycopy(es, entry, &es->sumprev, &es->sumprevlen);
		es->sumprevhash = entry->hash;
		es->hassumprev = true;
		return true;
	}

	/*
	 * A walk of values ends where the column's summaries begin: they sort
	 * after its last value (§32).
	 */
	if (kind >= LION_KIND_SUMMARY)
	{
		*stop = true;
		*partended = true;
		return false;
	}

	if (es->usesum)
	{
		/* The NULL entry is in no bucket, and a sum of values never wants it. */
		if (kind != LION_KIND_VALUE)
			return false;
		if (es->hasclipmin &&
			lion_scan_keycmp(es, LionEntryGetKey(entry), es->clipmin) <= 0)
			return false;
		if (es->hasclipmax &&
			lion_scan_keycmp(es, LionEntryGetKey(entry), es->clipmax) > 0)
		{
			*stop = true;
			return false;
		}
	}

	switch (es->part)
	{
		case LION_WALK_ALL:
			return true;
		case LION_WALK_INSIDE:
			r = lion_range_test(es->range, entry);
			if (r == LION_RANGE_END)
				*stop = *partended = true;
			return r == LION_RANGE_MATCH;
		case LION_WALK_BELOW:
			if (kind != LION_KIND_VALUE)
				return false;
			r = lion_range_test(es->range, entry);
			if (r != LION_RANGE_SKIP)
				*stop = *partended = true;
			return r == LION_RANGE_SKIP;
		case LION_WALK_ABOVE:
			if (kind != LION_KIND_VALUE)
				return false;
			return lion_range_fails_upper(es->range, entry);
	}
	return false;
}

/*
 * Read the leaf the walk stands at, ONCE, and take what the walk selects from
 * it: copies of the entries into the batch (copy), or only how many there are.
 * Returns how many were taken.
 *
 * Everything is decided under one share lock: where to resume on this leaf
 * (the first key above the last one examined, however the leaf has changed
 * since the walk left it), which entries the walk returns, where the next
 * read resumes (after the last entry examined here, which may be one the walk
 * passed over) and which leaf it reads next - the right link as it stands,
 * unless the walk ended here.  The lock is then given up; the pin is kept
 * while the batch holds an INLINE copy (see lion_entry_scan_next()).
 *
 * A walk that uses summaries (DESIGN.md §32) moves from one phase to the next
 * here, and sets the next phase up at the start of the following read.
 */
static int64
lion_entry_scan_fill(LionEntryScan *es, bool copy)
{
	LionState  *state;
	Buffer		buf;
	Page		page;
	OffsetNumber off;
	OffsetNumber maxoff;
	LionEntryTuple *last = NULL;
	char	   *dst = es->bpage;
	int64		ntaken = 0;
	bool		stop = false;
	bool		partended = false;

	Assert(es->nextbatch >= es->nbatch);
	Assert(!BufferIsValid(es->batchbuf));
	es->nbatch = 0;
	es->nextbatch = 0;
	es->lastinline = -1;

	if (es->nextphase != es->phase)
		lion_entry_scan_setup_phase(es);
	state = es->state;

	/*
	 * Test hook: the walk is between two leaves and holds nothing of the
	 * directory at all - no lock, and no pin of its own - so a concurrent
	 * insert may split the leaf it has just left and the one it is about to
	 * read.  It fires before every leaf but the first, in a counting walk
	 * (lion_entry_scan_skip_leaf()) as in a fetching one;
	 * test/isolation/count_range_split_race.spec parks the race of
	 * lion_range_choose() here.  Compiles to nothing without
	 * --enable-injection-points.
	 */
	if (es->nleaves > 0)
		LION_INJECTION_POINT("lion-entry-scan-leaf");

	buf = ReadBuffer(es->index, es->blkno);
	lion_dir_pages_read++;
	es->nleaves++;
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	if (!LionPageIsLeaf(page))
	{
		UnlockReleaseBuffer(buf);
		elog(ERROR, "lion index: block %u is not a directory leaf",
			 es->blkno);
	}

	if (es->resumekind >= 0)
	{
		/*
		 * After every entry of the column up to a kind (§32: where a column's
		 * summaries begin), which a key cannot say.  The landing leaf holds at
		 * most the tail of the run before it, passed over here.
		 */
		off = lion_page_first_data(page);
	}
	else if (es->haslast)
	{
		LionSearchKey sk;

		sk.attno = es->attno;
		sk.col = state;
		sk.kind = es->lastkind;
		sk.key = LION_KIND_HAS_KEY(es->lastkind) ?
			lion_fetch_key(state, es->lastkey) : (Datum) 0;
		sk.hash = es->lasthash;
		sk.cmpproc = state->ordered ? &state->cmpproc : NULL;
		sk.eqproc = &state->eqproc;
		sk.collation = state->collation;
		sk.raw = es->lastkey;
		sk.rawlen = es->lastkeylen;

		/*
		 * Everything on this page may already be behind us, which is what a
		 * split of the page we were on looks like from here.
		 */
		if (!LionPageIsRightmost(page) &&
			lion_cmp_entry(lion_dir_highkey(page), &sk) <= 0)
		{
			es->blkno = LionPageGetOpaque(page)->rightlink;
			UnlockReleaseBuffer(buf);
			return 0;
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

		if (!ItemIdIsUsed(iid))
			continue;
		entry = (LionEntryTuple *) PageGetItem(page, iid);

		if (es->resumekind >= 0)
		{
			if (entry->attno < es->attno ||
				(entry->attno == es->attno &&
				 lion_entry_kind(entry) <= es->resumekind))
				continue;
		}

		/*
		 * The walk is bounded to one key column (DESIGN.md §24): the entries
		 * are sorted by attno, so the first entry of the next column ends it.
		 */
		if (entry->attno != es->attno)
		{
			stop = true;
			partended = true;
			break;
		}

		/*
		 * A bounded walk (DESIGN.md §28) returns only the entries of its part
		 * of the range, and may end here.
		 */
		if (!lion_entry_scan_selects(es, entry, &stop, &partended))
		{
			if (stop)
				break;
			last = entry;
			continue;
		}
		last = entry;

		/*
		 * An entry whose posting set is empty can never produce a group.
		 * VACUUM deletes those (DESIGN.md §18), but one can be seen here
		 * between the moment its last TID was filtered out and the moment the
		 * leaf's final step removes it.
		 */
		if (entry->ntids == 0)
			continue;

		ntaken++;
		if (copy)
		{
			Size		sz = ItemIdGetLength(iid);
			int			n = es->nbatch;

			if (unlikely(n >= es->maxbatch ||
						 dst + sz > es->bpage + BLCKSZ))
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("lion index \"%s\": directory leaf %u holds more entries than fit on a page",
								RelationGetRelationName(es->index),
								BufferGetBlockNumber(buf))));
			memcpy(dst, entry, sz);
			es->bentry[n] = (LionEntryTuple *) dst;
			es->bsize[n] = sz;
			es->boff[n] = off;
			if ((entry->flags & LION_ENTRY_INLINE) != 0)
				es->lastinline = n;
			es->nbatch = n + 1;
			dst += MAXALIGN(sz);
			if (LionEntryIsSummary(entry))
				es->nsummaries++;
		}
	}

	/* The next read of any leaf resumes after the last entry examined here. */
	if (last != NULL)
		lion_entry_scan_remember(es, last);

	es->batchblk = BufferGetBlockNumber(buf);
	if (stop)
	{
		if (es->usesum)
			lion_entry_scan_end_phase(es, partended);
		else
			es->done = true;
	}
	else
	{
		/*
		 * Everything this leaf holds after the resume position has been
		 * examined, so the walk goes on at the right sibling this leaf had
		 * when it was read, without reading this leaf again.  What a split of
		 * it moves to a new page in between after that are entries the batch
		 * has already, or entries inserted since, which hold no row this
		 * walk's snapshot can see (DESIGN.md §28, "One read per leaf").
		 */
		es->blkno = LionPageGetOpaque(page)->rightlink;
		if (!BlockNumberIsValid(es->blkno))
		{
			if (es->usesum)
				lion_entry_scan_end_phase(es, true);
			else
				es->done = true;
		}
	}

	/*
	 * DESIGN.md section 9: an INLINE copy is counted against the visibility
	 * map, so the leaf it was copied from stays pinned until that set has been
	 * handed out.  A batch without one needs no pin at all.
	 */
	if (es->lastinline >= 0)
	{
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		es->batchbuf = buf;
	}
	else
		UnlockReleaseBuffer(buf);

	return ntaken;
}

/*
 * Fetch the next entry of the walk, in directory order.
 *
 * The entries come out of the batch the last leaf read copied (DESIGN.md §28,
 * "One read per leaf"), so a leaf is read once however many of its entries
 * the walk returns - it used to be read again for every entry, to find "the
 * first key above the last one returned" on it, which made a range over n
 * keys read n leaves and more.  Handing out a copy after the lock is gone is
 * safe for the reasons a resumed scan always was:
 *
 *	- a split moves entries only rightwards, onto a page the walk has not
 *	  passed; the ones that were on this leaf when it was read are in the
 *	  batch, and the walk reads next the right sibling the leaf had then, so
 *	  nothing is skipped or returned twice;
 *	- a deleted entry had an empty posting set, which nothing this walk's
 *	  snapshot can see was in (§18), and a CHAIN copy whose set was freed
 *	  meanwhile reads as empty (the owner check of §18);
 *	- an entry inserted after the leaf was read holds only rows no older
 *	  snapshot can see (§29.5 case 3), so missing it is right - the same
 *	  freedom the walk has always had behind its position;
 *	- and an INLINE payload is counted under the pin of the leaf it was copied
 *	  from, which the scan holds from the copy until the set is handed out and
 *	  the set holds after that: §9's rule, only with the pin taken once for the
 *	  leaf instead of once per entry.
 */
bool
lion_entry_scan_next(LionEntryScan *es, Datum *key, LionPostingSet *ps)
{
	int			i;
	bool		keeppin;

	while (es->nextbatch >= es->nbatch)
	{
		/* a phase that has ended hands over at the next read (§32) */
		if (es->done ||
			(!BlockNumberIsValid(es->blkno) && es->nextphase == es->phase))
		{
			es->done = true;
			return false;
		}
		CHECK_FOR_INTERRUPTS();
		(void) lion_entry_scan_fill(es, true);
	}

	i = es->nextbatch++;

	/*
	 * Test hook: the walk is between two entries of one leaf and holds no lock
	 * on it at all (a pin only while an INLINE copy is still to come), so a
	 * concurrent VACUUM is free to delete entries and a concurrent insert to
	 * split the page.  It fires once per leaf, which is what lets an isolation
	 * test park a walk here exactly once; test/isolation/vacuum_entry_delete,
	 * dir_split_scan and count_range_split_race are such cases.  Compiles to
	 * nothing without --enable-injection-points.
	 */
	if (i == 1)
		LION_INJECTION_POINT("lion-entry-scan-resumed");

	lion_fill_posting_set_entry(es->index, es->state, es->bentry[i],
								es->bsize[i], es->batchblk, es->boff[i], ps,
								&keeppin);
	*key = ps->storedkey;
	if (keeppin)
	{
		/*
		 * DESIGN.md section 9: the INLINE payload needs a pin of its own on the
		 * leaf it was copied from.  The last INLINE copy of the batch takes the
		 * scan's pin over.
		 */
		Assert(BufferIsValid(es->batchbuf));
		if (i == es->lastinline)
		{
			ps->pinbuf = es->batchbuf;
			es->batchbuf = InvalidBuffer;
		}
		else
		{
			IncrBufferRefCount(es->batchbuf);
			ps->pinbuf = es->batchbuf;
		}
	}

	return true;
}

/*
 * The same walk for a caller that needs neither a located set nor a pin: a
 * scan that hands every TID it reads to the executor, which visits each one
 * in the heap (see lion_count.h).  The copies are the batch's own, so an
 * entry stays valid until the next call - which may read the next leaf into
 * the same buffer - and the pin the batch keeps for its INLINE copies, which
 * only a count needs (DESIGN.md §9), is let go of as soon as it is taken.
 */
LionEntryTuple *
lion_entry_scan_next_copy(LionEntryScan *es, Size *itemlen)
{
	int			i;

	while (es->nextbatch >= es->nbatch)
	{
		/* a phase that has ended hands over at the next read (§32) */
		if (es->done ||
			(!BlockNumberIsValid(es->blkno) && es->nextphase == es->phase))
		{
			es->done = true;
			return NULL;
		}
		CHECK_FOR_INTERRUPTS();
		(void) lion_entry_scan_fill(es, true);
	}

	if (BufferIsValid(es->batchbuf))
	{
		ReleaseBuffer(es->batchbuf);
		es->batchbuf = InvalidBuffer;
	}
	es->lastinline = -1;

	i = es->nextbatch++;
	*itemlen = es->bsize[i];
	return es->bentry[i];
}

int64
lion_entry_scan_skip_leaf(LionEntryScan *es)
{
	Assert(es->nextbatch >= es->nbatch);

	if (es->done ||
		(!BlockNumberIsValid(es->blkno) && es->nextphase == es->phase))
	{
		es->done = true;
		return 0;
	}
	return lion_entry_scan_fill(es, false);
}

/*
 * No pin across a row (see lion_count.h).  Called with at least one entry of
 * the batch handed out, which is where the leaf is read again from: the walk
 * resumes after it, so the INLINE copies dropped here come back in the next
 * batch - or, if VACUUM or a split has moved them meanwhile, from wherever
 * the key-based resume finds them.  A batch with nothing handed out yet (no
 * caller pauses there) keeps its pin rather than lose its position.
 */
void
lion_entry_scan_pause(LionEntryScan *es)
{
	if (!BufferIsValid(es->batchbuf) || es->nextbatch == 0)
		return;

	ReleaseBuffer(es->batchbuf);
	es->batchbuf = InvalidBuffer;
	lion_entry_scan_remember(es, es->bentry[es->nextbatch - 1]);
	es->nbatch = es->nextbatch;
	es->lastinline = -1;
	es->blkno = es->batchblk;
	es->done = false;
}

void
lion_entry_scan_end(LionEntryScan *es)
{
	es->done = true;
	if (BufferIsValid(es->batchbuf))
	{
		ReleaseBuffer(es->batchbuf);
		es->batchbuf = InvalidBuffer;
	}
	es->nbatch = 0;
	es->nextbatch = 0;
	if (es->cxt != NULL)
	{
		MemoryContextDelete(es->cxt);
		es->cxt = NULL;
	}
	if (es->batchcxt != NULL)
	{
		MemoryContextDelete(es->batchcxt);
		es->batchcxt = NULL;
	}
	if (es->phasecxt != NULL)
	{
		MemoryContextDelete(es->phasecxt);
		es->phasecxt = NULL;
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
 * The type a SQL count's key is looked up as: what `col = key` in the query
 * the count stands for would compare it as, or an ERROR where that query
 * would find no operator.  The function's argument is polymorphic, so its
 * type is whatever the caller wrote, and it has to be brought to one of the
 * column's opclass members by the rules the parser would apply:
 *
 *	- a domain is its base type, as it is to operator resolution;
 *	- a class declared on a POLYMORPHIC type (enum_ops, FOR TYPE anyenum)
 *	  takes values of the column's own type and nothing else: its members
 *	  are (anyenum, anyenum), which the parser only lets two values of ONE
 *	  enum meet at.  A different enum would pass for "an enum", and its OIDs
 *	  would be looked up in this column's directory as if they meant
 *	  something there.  The resolved type is the class's own, opcintype.
 *	- otherwise the class's own type, or a type the family has a strategy-1
 *	  member for with it (int8 on an int4 column: int48eq, exactly as in the
 *	  query), which lion_probe_init() resolves further;
 *	- or, lacking such a member, a BINARY coercion to the class's type -
 *	  varchar to text - where the parser makes it with `col = key` too:
 *	  there is no `text = varchar`, so it relabels the key and calls the
 *	  class's own `text = text`.  The bytes are the same, so the key is then
 *	  simply one of the column's own values.  A cast FUNCTION is not taken,
 *	  for §21's reason (implicit does not mean lossless), and neither is a
 *	  coercion the parser would not make (bpchar and text, below).
 *
 * This is the whole of the key type check: until 2026-09-25 it compared the
 * key with rd_opcintype exactly, so enum_ops, a DEFAULT class, could never be
 * counted at all ("the index is on type anyenum" for the column's own enum),
 * and neither could a varchar key on the varchar column it indexes.
 */
static Oid
lion_count_key_type(Relation index, AttrNumber col, Oid keytype)
{
	LionState  *state = lion_index_column_state(index, col);
	Oid			opfamily = index->rd_opfamily[col - 1];
	Oid			opcintype = index->rd_opcintype[col - 1];
	Oid			basetype = getBaseType(keytype);

	if (IsPolymorphicType(opcintype))
	{
		if (lion_type_is_column(state, basetype))
			return opcintype;
	}
	else
	{
		Oid			eqopr;
		Oid			lefttype;
		Oid			righttype;

		if (basetype == opcintype ||
			OidIsValid(get_opfamily_member(opfamily, opcintype, basetype, 1)))
			return basetype;

		/*
		 * The binary coercion only where `col = key` makes it: the operator
		 * the parser picks for (the column's type, the key's type) has to be
		 * the class's own (opcintype, opcintype), reached without a cast
		 * function (compatible_oper()).  That a coercion EXISTS is not
		 * enough.  text is binary-coercible to bpchar, but `bpcharcol =
		 * 'x '::text` resolves to `text = text` - text is the preferred type
		 * of its category - and casts the COLUMN with rtrim1(), so it matches
		 * no row that bpchar's own equality, which ignores trailing blanks,
		 * would count.
		 */
		eqopr = compatible_oper_opid(list_make1(makeString(pstrdup("="))),
									 state->typid, basetype, true);
		if (OidIsValid(eqopr))
		{
			op_input_types(eqopr, &lefttype, &righttype);
			if (lefttype == opcintype && righttype == opcintype)
				return opcintype;
		}
	}

	ereport(ERROR,
			(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("type %s cannot be compared with index \"%s\"",
					format_type_be(keytype),
					RelationGetRelationName(index)),
			 errdetail("The index is on type %s.",
					   format_type_be(lion_column_type(state, opcintype)))));
	return InvalidOid;			/* keep the compiler quiet */
}

/*
 * The collation a SQL count's argument argnum brings to `col = key`: that of
 * its expression in the call, or - called with none, from C - the call's own.
 * The two-key form has one call collation for both keys, where the query it
 * stands for compares each under its own; so each is taken from its own
 * argument.
 */
static Oid
lion_count_arg_collation(FunctionCallInfo fcinfo, int argnum)
{
	Node	   *expr = (fcinfo->flinfo != NULL) ? fcinfo->flinfo->fn_expr : NULL;

	if (expr != NULL && IsA(expr, FuncExpr))
	{
		List	   *args = ((FuncExpr *) expr)->args;

		if (argnum < list_length(args))
			return exprCollation((Node *) list_nth(args, argnum));
	}
	return PG_GET_COLLATION();
}

/*
 * The collation key column col's own values bring to `col = key`: the table
 * column's, or its expression's when the column is an expression.  The
 * index's may be another - `(c COLLATE "x")` keeps c, and compares under x.
 */
static Oid
lion_count_column_collation(Relation heap, Relation index, AttrNumber col)
{
	AttrNumber	attnum = index->rd_index->indkey.values[col - 1];
	List	   *exprs;
	int			nth = 0;
	int			c;

	if (attnum != 0)
		return TupleDescAttr(RelationGetDescr(heap), attnum - 1)->attcollation;

	for (c = 0; c < col - 1; c++)
	{
		if (index->rd_index->indkey.values[c] == 0)
			nth++;
	}
	exprs = lion_index_stored_exprs(index, Anum_pg_index_indexprs);
	if (nth >= list_length(exprs))
		elog(ERROR, "index \"%s\" has too few expressions",
			 RelationGetRelationName(index));
	return exprCollation((Node *) list_nth(exprs, nth));
}

/*
 * Would the index answer `col = key` - or, with no key, `GROUP BY col` - as
 * the query does, collation and all?  The index hashed and compared its keys
 * under its own collation; the query compares under the key's, when the key
 * brings one of its own (an explicit COLLATE, or a column of another
 * collation), and otherwise under the column's: a literal or a parameter
 * brings the default collation, which gives way to the column's in the
 * parser's rule for an operator's inputs.  (So an explicit COLLATE "default"
 * is taken for no COLLATE at all.)  Two deterministic collations agree on
 * which values are equal - each calls them equal when their bytes are - and
 * a nondeterministic one agrees with no other: `c COLLATE case_insensitive =
 * 'abc'` counts 'ABC' and `c = 'abc'` does not.  So a count under another
 * collation than the index's is refused when either is nondeterministic, and
 * made otherwise.
 */
static void
lion_count_check_collation(Relation heap, Relation index, AttrNumber col,
						   Oid keycoll)
{
	Oid			idxcoll = index->rd_indcollation[col - 1];
	Oid			collation = keycoll;

	if (!OidIsValid(idxcoll))
		return;					/* the key type is not collatable */
	if (!OidIsValid(collation) || collation == DEFAULT_COLLATION_OID)
		collation = lion_count_column_collation(heap, index, col);
	if (!OidIsValid(collation) || collation == idxcoll)
		return;
	if (get_collation_isdeterministic(collation) &&
		get_collation_isdeterministic(idxcoll))
		return;

	ereport(ERROR,
			(errcode(ERRCODE_COLLATION_MISMATCH),
			 errmsg("cannot count through index \"%s\" under collation \"%s\"",
					RelationGetRelationName(index),
					get_collation_name(collation)),
			 errdetail("Key column %d of the index is under collation \"%s\", and a nondeterministic collation does not agree with any other on which values are equal.",
					   col, get_collation_name(idxcoll)),
			 errhint("Count with the query itself, or through an index built under the collation it compares with.")));
}

/*
 * Open and vet the indexes of one SQL count: relkind, access method, key
 * type, collation, privileges, row-level security and snapshot eligibility.
 * keytype and keycoll may be NULL, which means the caller has no search key
 * at all (the grouped form below, which walks every entry instead of looking
 * one up); keycoll[i] is the collation key i brings
 * (lion_count_arg_collation()).
 */
static void
lion_count_open_indexes(Snapshot snapshot, int nidx, const Oid *idxoid,
					   const Oid *keytype, const Oid *keycoll,
					   AttrNumber wantcol, LionCountCall *call)
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
		 * The key's type, resolved the way `col = key` would resolve it
		 * (lion_count_key_type()); what lion_probe_init() is handed from here
		 * on, and what the EXECUTE check below names the equality by.
		 * Raising the errors here keeps them out of the middle of the count.
		 */
		if (OidIsValid(call->keytype[i]))
			call->keytype[i] = lion_count_key_type(index,
												   (wantcol == 0) ? 1 : wantcol,
												   call->keytype[i]);

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

			lion_count_check_collation(call->heap, index, col,
									   (keycoll != NULL) ? keycoll[i] :
									   InvalidOid);
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
	 * A materialized view created WITH NO DATA has an empty heap and empty
	 * indexes, so every count through them was 0 - where the query the count
	 * stands for refuses to run at all (2026-09-25 review).  Refuse as
	 * ExecOpenScanRelation() does, and where the executor does: after the
	 * range table's privileges, before the scan's quals and the aggregate are
	 * initialised and their functions checked.  The pushdown node makes the
	 * same check (lion_begin_custom_scan()).
	 */
	if (!RelationIsScannable(call->heap))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("materialized view \"%s\" has not been populated",
						RelationGetRelationName(call->heap)),
				 errhint("Use the REFRESH MATERIALIZED VIEW command.")));

	/*
	 * The query also CALLS count() and the equality the key is looked up
	 * with, so the count asks for EXECUTE on both, as the executor would of
	 * that query (2026-09-23 review).  `col = key` is strategy 1 of the key
	 * column's opfamily for (opcintype, the key's type as
	 * lion_count_key_type() resolved it) - int48eq for an int8 key on an int4
	 * column, texteq for a varchar key on a text_ops column (the parser
	 * relabels it), enum_eq for the column's own enum, exactly as in the
	 * query - and `col = ANY (keys)` calls the same function per element.
	 * The resolved type is also what the lookup is made as, so the function
	 * checked here is the one whose meaning the count reproduces.  The
	 * grouped form stands for
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

		/*
		 * And none at all under PostgreSQL 16's old_snapshot_threshold, which
		 * the planner's pushdown declines for the same reason (DESIGN.md §9).
		 */
		lion_check_old_snapshot(call->index[i], snapshot);
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
	Oid			keycoll[2];
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
		keycoll[i] = lion_count_arg_collation(fcinfo, 2 * i + 1);
	}

	lion_count_open_indexes(snapshot, nkeys, idxoid, keytype, keycoll, 0,
						   call);

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
	Oid			keycoll;
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

	keycoll = lion_count_arg_collation(fcinfo, 1);
	lion_count_open_indexes(snapshot, 1, &idxoid, &elemtype, &keycoll, 0,
						   &call);
	PredicateLockRelation(call.index[0], snapshot);

	get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
	deconstruct_array(arr, elemtype, elmlen, elmbyval, elmalign,
					  &elems, &nulls, &nelems);

	/*
	 * The elements are deconstructed as what the array holds, and looked up
	 * as the type lion_count_open_indexes() resolved that to: a varchar[] is
	 * looked up as text on a text_ops column, the same bytes.
	 */
	sets = (LionPostingSet *) palloc0(sizeof(LionPostingSet) * Max(nelems, 1));
	nsets = lion_posting_set_lookup_many_col(call.index[0], 1, call.keytype[0],
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

	lion_count_open_indexes(snapshot, 1, &idxoid, NULL, NULL, attno, &call);

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
