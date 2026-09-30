/*-------------------------------------------------------------------------
 *
 * lion_count_int.h
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
 * This header is private to the count engine, the files lion_count.h
 * declares the interface of: lion_set.c, lion_set_copy.c, lion_cursor.c,
 * lion_expr.c, lion_vis.c, lion_count.c, lion_count_groups.c,
 * lion_count_shared.c, lion_rangesrc.c, lion_range.c and
 * lion_count_sql.c.  What they share and nothing else needs is declared
 * here.
 *
 *-------------------------------------------------------------------------
 */
#ifndef LION_COUNT_INT_H
#define LION_COUNT_INT_H


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
	LionRightWalk walk;			/* its steps right over its whole life, seeks
								 * and all: a cursor only moves forward and a
								 * seek lands no further left than it stands,
								 * so a cycle of damaged links still ends in
								 * an ERROR (lion.h) */
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


/*
 * How far a cursor walks right before it gives up and descends instead
 * (DESIGN.md §22).  Stepping right is one buffer read per page; a descent is
 * `height` reads and a binary search per level, so the two are about even at
 * two pages and the descent wins from there.  The number only decides how
 * much work a seek does, never what it finds.
 */
#define LION_POSTING_SEEK_STEPS		2


/*
 * The most key slots a direct index spends on one container: four bytes a
 * slot, so at most sixteen bytes a container, the size of a spilled
 * container's entry and less than any container in memory takes with its
 * pointer.  A copy whose keys are sparser than that is searched instead.
 */
#define LION_MAT_DIR_SPREAD		4
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
/*
 * What each child of an OR adds to it: its heap and hot entries, and the
 * container it stands on (hotc).  lion_batch_end() sizes a batch's union by
 * the same amount, so that a batch that fits is never planned wide.
 */
#define LION_OR_CHILD_OVERHEAD \
	(2 * sizeof(LionOrHeapEnt) + sizeof(LionContainer *))
#define LION_WIDE_END	(((uint64) PG_UINT32_MAX) + 1)	/* past every key */
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
 * Assert builds cross-check every mask against the function it replaces.  This
 * reinstates exactly the per-block cost the mask exists to avoid, so timings
 * taken on a cassert cluster want -DRBI_NO_VM_MASK_CHECK.
 */
#if defined(USE_ASSERT_CHECKING) && !defined(LION_NO_VM_MASK_CHECK)
#define LION_VM_MASK_CHECK 1
#endif
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

/* Defined in one of the count engine files and used in another. */

/* lion_set.c */
extern bool lion_type_is_column(LionState *state, Oid typid);
extern Oid lion_column_type(LionState *state, Oid opcintype);
extern void lion_fill_posting_set_entry(Relation index, LionState *state,
										const LionEntryTuple *entry,
										Size itemsz, BlockNumber blkno,
										OffsetNumber offnum,
										LionPostingSet *ps, bool *keeppin);
extern void lion_open_budget_init(LionOpenBudget *budget, Relation rel);
extern bool lion_posting_set_relocate(const LionPostingSet *ps,
									  LionPostingSet *fresh);

/* lion_set_copy.c */
extern bool lion_posting_set_materialize(LionPostingSet *ps, Size maxbytes);
extern void lion_spill_begin(LionSpill *sp, MemoryContext cxt);
extern void lion_spill_add(LionSpill *sp, const LionContainer *c);
extern LionMatSet *lion_spill_finish(LionSpill *sp);
extern void lion_spill_abandon(LionSpill *sp);
extern void lion_spill_read(const LionMatSet *mat, int i, LionContainer *buf);
extern void lion_spill_keep_first(LionMatSet *mat, MemoryContext cxt);
extern void lion_mat_index(LionMatSet *mat, MemoryContext cxt, Size room);

/* lion_cursor.c */
extern void lion_cursor_init_at(LionSetCursor *cur, const LionPostingSet *set,
								LionCountCtx *cx, bool droppins, uint32 target,
								PGAlignedBlock *image);
extern void lion_cursor_init(LionSetCursor *cur, const LionPostingSet *set,
							 LionCountCtx *cx, bool droppins);
extern void lion_cursor_next(LionSetCursor *cur);
extern void lion_cursor_seek(LionSetCursor *cur, uint32 target);
extern void lion_cursor_close(LionSetCursor *cur);

/* lion_expr.c */
extern const LionContainer *lion_leapfrog(LionExprCursor *cur,
										  const int *order, int n,
										  LionContainer *const *work, int *w,
										  uint64 hi, LionCountStats *stats,
										  uint32 *key);
extern void lion_leaf_cost(const LionPostingSet *ps, bool droppins, Size *mem,
						   int *pins);
extern void lion_plan_node(LionNodePlan *p, const LionKeyNode *node,
						   const LionPostingSet *sets,
						   const LionOpenBudget *budget, bool droppins,
						   bool build);
extern LionNodePlan *lion_plan_build(const LionKeyNode *node,
									 const LionPostingSet *sets,
									 const LionOpenBudget *budget,
									 bool droppins);
extern bool lion_leapfrog_lazy(const LionKeyNode *node);
extern void lion_ecursor_init(LionExprCursor *c, const LionNodePlan *plan,
							  LionPostingSet *sets, int nsets,
							  LionCountCtx *cx, bool droppins);
extern void lion_ecursor_init_ex(LionExprCursor *c, const LionNodePlan *plan,
								 LionPostingSet *sets, int nsets,
								 LionCountCtx *cx, bool droppins, bool raw,
								 bool lazy);
extern void lion_ecursor_next(LionExprCursor *c);
extern void lion_ecursor_seek(LionExprCursor *c, uint32 target);
extern void lion_ecursor_close(LionExprCursor *c);
extern LionKeyNode *lion_source_tree(const LionCountSource *src);
extern bool lion_source_satisfiable(const LionKeyNode *node,
									const LionPostingSet *sets);
extern bool lion_source_pinned(const LionKeyNode *node,
							   const LionPostingSet *sets,
							   const LionOpenBudget *budget);

/* lion_vis.c */
extern void lion_vm_mask_check(Relation heap, BlockNumber firstblk,
							   uint64 members, uint64 allvis, Buffer *vmbuf);
extern void lion_vis_cache_begin(LionVisCache *cache, Relation heap,
								 Snapshot snapshot);
extern LionVisEntry *lion_vis_cache_lookup(LionCountCtx *cx,
										   BlockNumber blkno);
extern LionVisEntry *lion_vis_cache_prepare(LionCountCtx *cx,
											BlockNumber blkno,
											LionVisEntry *e);
extern void lion_vis_fill_page(LionCountCtx *cx, Buffer buf, BlockNumber blkno,
							   LionVisEntry *e);

/* lion_count.c */
extern int lion_recheck_budget(void);
extern void lion_count_container_masks(LionCountCtx *cx,
									   const LionContainer *c, uint64 members,
									   uint64 allvis);
extern void lion_recheck_flush(LionCountCtx *cx);
extern int64 lion_count_sources_run(Relation heap, Snapshot snapshot,
									int nsources, LionCountSource *sources,
									LionCountStats *stats, LionVisCache *cache,
									bool rel_read_only, bool exists,
									LionCollect *collect);

/* Inline helpers more than one of the files uses. */

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

static inline bool
lion_vis_entry_visible(const LionVisEntry *e, OffsetNumber off)
{
	int			bit = (int) off - 1;

	if (off < FirstOffsetNumber || bit >= MaxHeapTuplesPerPage)
		return false;			/* no heap page can hold that line pointer */
	return (e->vis[bit / 64] & (UINT64CONST(1) << (bit % 64))) != 0;
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

#endif							/* LION_COUNT_INT_H */
