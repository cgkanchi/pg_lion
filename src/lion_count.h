/*-------------------------------------------------------------------------
 *
 * lion_count.h
 *		Heap-skipping count(*) over roaring posting sets, and the entry
 *		points of the count pushdown CustomScan.
 *
 *		See DESIGN.md sections 9 (the visibility-map interlock and why it is
 *		safe), 10 (the CustomScan) and 11 (the VACUUM rule this relies on).
 *
 *-------------------------------------------------------------------------
 */
#ifndef LION_COUNT_H
#define LION_COUNT_H

#include "access/htup.h"
#include "access/skey.h"
#include "fmgr.h"
#include "nodes/pathnodes.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "storage/buf.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"

#include "lion.h"

/* ---------------------------------------------------------------------
 * Counting
 * --------------------------------------------------------------------- */

/*
 * Two thresholds of the k-way union that the COST MODEL has to know as well as
 * the executor, so that a path is priced as the path it will take (DESIGN.md
 * §15).  The arguments for the numbers are where they are used, in
 * lion_expr.c's lion_ecursor_build() for the first and lion_count.c's
 * lion_sum_is_cheaper() for both.
 *
 *	LION_OR_BITSET_MIN		containers at one container key above which an OR
 *							node accumulates them in a bitset image instead of
 *							folding them pairwise.
 *	LION_SUM_MAX_DENSITY	rows per container key in one entry up to which the
 *							disjoint sum is cheaper than the union.
 */
#ifndef LION_OR_BITSET_MIN
#define LION_OR_BITSET_MIN	32
#endif
#ifndef LION_SUM_MAX_DENSITY
#define LION_SUM_MAX_DENSITY	4
#endif

/*
 * Two more the cost model shares with the executor: how small a posting set
 * is worth a private copy (lion_posting_set_materialize() in lion_set_copy.c,
 * where the argument is) - which decides whether a GROUP BY's lone WHERE set
 * is collected (DESIGN.md §10) - and the most groups one walk of
 * lion_count_groups_copy() counts together.
 */
#define LION_MATERIALIZE_MAX_CONTAINERS	64
#define LION_MATERIALIZE_MAX_BYTES		(256 * 1024)
#define LION_GROUP_BATCH_MAX	256

/*
 * An `IN` list longer than this is not pushed down: every listed value needs
 * its own posting set, and each of those may hold a buffer pin for as long as
 * the node runs (DESIGN.md §9).  A long list is also exactly the case where
 * the ordinary bitmap plan does well.  A list whose length the planner cannot
 * see is answered at any length, and one longer than this may be located a
 * batch at a time (lion_array_batch_size()).
 */
#define LION_MAX_ARRAY_ELEMS		1000

/*
 * Instrumentation, reported by lion_index_count_stats() and by
 * EXPLAIN ANALYZE of the LionCount node.
 */
typedef struct LionCountStats
{
	int64		blocks_skipped_via_vm;	/* heap blocks counted from the VM */
	int64		tids_rechecked; /* TIDs resolved by a heap recheck, cache hits
								 * included */
	int64		blocks_rechecked;	/* heap blocks pinned for those rechecks */
	int64		containers_visited; /* containers read from the indexes */

	/*
	 * Seeks the AND merge did NOT make (DESIGN.md §25).  At a container key
	 * whose running intersection is already empty - or that one source simply
	 * has no container at - the sources after it in the probe order are left
	 * standing instead of being sought, and each one of them that was still
	 * below the key is counted here.  Before the early exit every one of these
	 * was a probe, and a probe is a descent or a step or two right, so this is
	 * the index pages the merge did not have to read.  Zero means every source
	 * had to be consulted at every container key the driver produced, which is
	 * what a set of dense, uncorrelated sources looks like.
	 */
	int64		probes_avoided;

	/*
	 * How the AND met its unions (DESIGN.md §29.11, "Unions probed"): the
	 * container keys at which the union of two or more containers of an IN
	 * list, a multi-key `&&` or an OR across columns was built, and the ones
	 * at which the running intersection was looked up in those containers
	 * instead and the union never built.  A union that drives the AND, or is
	 * the only source, is always built.
	 */
	int64		unions_built;
	int64		unions_probed;

	/*
	 * How the AND met its nested trees (DESIGN.md §29.11, "Trees probed"): an
	 * AND of ORs, an OR of ANDs - a source, or a child of an AND node, whose
	 * own tree is more than one union - met at a key the running
	 * intersection has reached from other sources.  trees_built counts the
	 * keys at which the tree's container was built there and ANDed with the
	 * intersection, trees_probed those at which the intersection's members
	 * were looked up in the tree's leaves instead, combined by its AND and
	 * OR nodes, and nothing of it was built.  A tree that drives the AND, or
	 * is the only source, is built and counted in neither.
	 */
	int64		trees_built;
	int64		trees_probed;

	/*
	 * The per-query visibility cache (LionVisCache below).  cache_hits counts
	 * the heap block visits it answered without touching the buffer manager,
	 * so cache_hits + blocks_rechecked is the number of block visits the
	 * recheck asked for.  cache_full counts the block visits that had to be
	 * fetched because the cache had reached its work_mem budget.
	 */
	int64		cache_hits;
	int64		cache_full;

	/*
	 * Posting sets counted on their own and added up instead of being merged
	 * (the disjoint-sum short-circuit of DESIGN.md §15).  Zero means every
	 * container key went through the k-way union, which is what a multi-key
	 * clause and an IN list ANDed with something else still do.
	 */
	int64		sets_summed;

	/*
	 * Candidate rows the snapshot sees that a row filter (LionRowFilter
	 * below) turned away: rows of a superset the posting sets answered for a
	 * multi-key query they could not answer exactly (DESIGN.md §17).  Zero
	 * whenever no count had a filter.
	 */
	int64		rows_removed;

	/*
	 * What a count's work is made of, container by container (DESIGN.md §27,
	 * "Where a key's time goes"), which the FK-side join reports per key:
	 *
	 *	key_containers	containers read from the one set of source slot 0 -
	 *					a group's set, a join key's fk set - so the count's
	 *					own share of containers_visited
	 *	copy_containers	containers read from a private copy: a collected
	 *					intersection (the join's copy of its fact filters, a
	 *					GROUP BY's of its WHERE items) or a materialized set
	 *	copy_seeks		binary searches of such a copy, one a probe
	 *	copy_file_reads	containers of a SPILLED copy read back from its
	 *					temporary file, one read each
	 *	vm_checks		containers whose heap blocks the visibility map was
	 *					asked about
	 *	vm_pins			visibility map pages pinned to answer them
	 */
	int64		key_containers;
	int64		copy_containers;
	int64		copy_seeks;
	int64		copy_file_reads;
	int64		vm_checks;
	int64		vm_pins;
} LionCountStats;

/* dst += src, every field. */
extern void lion_count_stats_add(LionCountStats *dst,
								 const LionCountStats *src);

/*
 * A per-query cache of heap visibility answers for blocks that are not
 * all-visible (DESIGN.md §9).
 *
 * A TID's visibility under one MVCC snapshot cannot change while that
 * snapshot is held, so the answer for every root line pointer of a heap page
 * may be resolved once and reused by every later recheck of that page - which
 * is what turns the GROUP BY path's one-recheck-per-group-per-dirty-page into
 * one pass over the dirty pages.  The safety argument is in lion_vis.c above
 * lion_vis_cache_lookup().
 *
 * The handle is opaque and is created once per count node execution, in a
 * context the caller owns; lion_count_sources_cached() empties it by itself if
 * it is ever handed a different relation or a different snapshot, so one
 * handle can serve every group of every partition of a partitioned count.
 * Passing NULL means "no cache": every recheck then fetches its own blocks,
 * exactly as before this cache existed.
 */
typedef struct LionVisCache LionVisCache;

extern LionVisCache *lion_vis_cache_create(MemoryContext parent);
extern void lion_vis_cache_reset(LionVisCache *cache);
extern void lion_vis_cache_destroy(LionVisCache *cache);

/*
 * The counts handed one cache keep a visibility-map page pinned from one to
 * the next, and their working memory (DESIGN.md §27, "The per-key path, end
 * to end").  The holder of the cache lets go of the pin with this when it is
 * done with the relation; a reset and a destroy do it too.
 */
extern void lion_vis_cache_release_vm(LionVisCache *cache);

/*
 * A test every row a count counts has to pass as well, made on the heap tuple
 * the snapshot sees (DESIGN.md §17, "A query known only at run time").  The
 * count pushdown sets one while a multi-key clause whose query it only had at
 * run time - a generic plan's parameter, a stable expression - came out as one
 * the posting sets cannot answer exactly: its sources are then a SUPERSET of
 * the rows (or, for a query no key narrows at all, the other clauses' rows
 * alone), and each clause is tested here instead.
 *
 * Each clause is `column op value`, the column on the left, of a strict
 * operator: a NULL column fails it, and so does a NULL result.  notnull makes
 * a clause the test `column IS NOT NULL` alone, which is all the heap-scan
 * fallback (lion_count_heap_filtered()) needs besides.  attno is the heap
 * column in the numbering of `heap`, the relation being counted - a
 * partition's own (DESIGN.md §16).
 */
typedef struct LionRowFilterClause
{
	AttrNumber	attno;
	bool		notnull;
	FmgrInfo	flinfo;			/* the operator's function */
	Oid			collation;
	Datum		value;
} LionRowFilterClause;

typedef struct LionRowFilter
{
	Relation	heap;
	int			nclauses;
	LionRowFilterClause *clauses;
	MemoryContext tmpcxt;		/* one row's evaluation, reset after it */
} LionRowFilter;

/*
 * Every count that is handed `cache` afterwards (lion_count_sources_cached(),
 * lion_exists_sources_cached()) sends each candidate TID to the heap - the
 * visibility map vouches for visibility, not for the filter - and counts the
 * row the snapshot sees only when it passes `filter`.  NULL removes it.  The
 * cache is the handle the count node already gives every count of one
 * execution; lion_vis_cache_reset() leaves the filter where it is, and a
 * count of another relation than filter->heap is an error.  A collection
 * (lion_sources_collect()) is never filtered: the superset it copies is
 * filtered when it is counted.
 */
extern void lion_vis_cache_set_filter(LionVisCache *cache,
									  LionRowFilter *filter);

/* Does the row pass?  tuple is a heap tuple of filter->heap. */
extern bool lion_row_filter_test(LionRowFilter *filter, HeapTuple tuple);

/*
 * The rows of `heap` visible to snapshot that pass filter, by a sequential
 * scan: what the count pushdown falls back to when a filtered clause was its
 * only source and there is no candidate set to recheck (DESIGN.md §17).  The
 * rows scanned are counted as rechecked in stats.
 */
extern int64 lion_count_heap_filtered(Relation heap, Snapshot snapshot,
									  LionRowFilter *filter,
									  LionCountStats *stats);

/*
 * A located posting set: everything the counting code needs in order to
 * iterate the containers of one (index, key) pair.
 *
 * For an INLINE entry the payload has been copied out of its bucket page,
 * and that bucket page stays PINNED for the whole life of the LionPostingSet:
 * DESIGN.md section 9 makes the pin the interlock against ambulkdelete, and
 * ambulkdelete rewrites INLINE payloads under a cleanup lock (section 11).
 * Unless the lookup was over its pin budget (DESIGN.md §15): the set is then
 * NOPIN, a private copy exactly like a materialized one below, and the count
 * either locates it again under a pin of its own when it gets to it or lets
 * another source carry the interlock (lion_count_sources_cached()).
 *
 * For a CHAIN entry only the head block is remembered here; pins on the
 * container pages are taken and released by the cursor that walks the chain,
 * one page at a time, under the same rule.
 *
 * A CHAIN entry that is counted more than once (the GROUP BY path of section
 * 10 intersects the same WHERE sets with every group) may instead be
 * MATERIALIZED: its containers are copied into palloc'd memory once and the
 * chain is not walked again.  A materialized set holds no pin and therefore
 * carries no visibility-map interlock of its own, so lion_count_posting_sets()
 * only ever materializes a set while at least one other set of the same
 * intersection is still read the pinned way.  The argument for why that is
 * enough is in lion_set_copy.c above lion_posting_set_materialize().
 */
struct LionMatSet;				/* private to the count engine */

typedef struct LionPostingSet
{
	Relation	index;			/* index the set belongs to */
	uint16		attno;			/* and which of its KEY COLUMNS (§24): what
								 * the located entry tuple carries, so a
								 * caller that has the set has the column */
	bool		found;			/* false: the key has no entry at all */
	bool		is_inline;		/* payload/paylen valid, else head valid */
	BlockNumber head;			/* CHAIN: first container page */
	char	   *payload;		/* INLINE: private copy of the payload */
	Size		paylen;
	Buffer		pinbuf;			/* INLINE: pinned bucket page, else Invalid */
	bool		nopin;			/* INLINE, but located without its pin */
	bool		budgeted;		/* its pin took a leaf of the list budget */
	struct ResourceOwnerData *pinowner; /* ... charged to this owner, the
										 * one the pin belongs to */
	uint64		ntids;			/* entry's recorded member count (a hint) */
	uint32		ncontainers;	/* entry's recorded ITEM count (a hint):
								 * containers and sparse segments */

	/*
	 * Where the entry tuple itself lives: its bucket page and the offset on
	 * it.  That pair identifies the entry inside the index - entry offsets are
	 * stable (DESIGN.md §18, lion_entry_scan_next()) - which is how
	 * lion_posting_set_lookup_many() proves that two values of an IN list
	 * found DIFFERENT entries, and therefore disjoint posting sets, even when
	 * the values themselves are not bytewise equal (DESIGN.md §15).
	 */
	BlockNumber entryblk;
	OffsetNumber entryoff;

	/*
	 * Bookkeeping for materialization.  cxt is the memory context the set was
	 * located in, which is where the copies go; nuses counts the calls of
	 * lion_count_posting_sets() this set has taken part in, and is what tells
	 * a one-shot count apart from a set the GROUP BY path reuses.  matfailed
	 * says a copy was tried and given up on - the set is too big, or the
	 * copies its count's sources already hold have spent their budget
	 * (DESIGN.md §15, "Bounded cursors") - so it is not walked again for a
	 * copy that would fail the same way at every group.
	 */
	MemoryContext cxt;
	int			nuses;
	struct LionMatSet *mat;		/* materialized containers, or NULL */
	bool		matfailed;

	/*
	 * A private copy of the key exactly as the index stored it.  Callers that
	 * looked the entry up with a cross-type constant need this to report the
	 * column's own value rather than the constant's.  keyisnull marks the
	 * reserved NULL-key entry (DESIGN.md §14), which has no key at all.
	 */
	Datum		storedkey;
	bool		hasstoredkey;
	bool		keyisnull;
} LionPostingSet;

/*
 * One input of a count: the posting sets one clause selects.
 *
 *	- nsets == 1 and !negated: an ordinary `col = value`.
 *	- nsets > 1: an IN list (DESIGN.md §15).  The sets are UNIONed, which is
 *	  exact because a count of a union of sets that may overlap is not a sum;
 *	  the merge below ORs the containers that share a container key.
 *	- negated: the members are subtracted from the intersection instead of
 *	  being intersected into it, which is how `col IS NOT NULL` is counted
 *	  (DESIGN.md §14: the rows whose key is NULL are exactly the members of
 *	  the reserved NULL entry, so everything else is the complement).
 *
 * A negated source never drives the merge: it is consulted only at container
 * keys the positive sources have in common.
 */
typedef struct LionCountSource
{
	int			nsets;
	LionPostingSet *sets;		/* nsets located posting sets */
	bool		negated;

	/*
	 * How the sets combine (DESIGN.md §17).  NULL means the union of all of
	 * them, which is what §15's IN lists want and what every caller written
	 * before multi-key opclasses existed gets.  Otherwise it is a boolean
	 * tree whose LION_KN_KEY leaves name sets by their position in sets[]:
	 * an AND for `tags @> '{a,b}'`, an OR for `&&`, and whatever shape a
	 * tsquery of ANDs and ORs has.
	 *
	 * A negated source is subtracted whatever its shape.
	 */
	LionKeyNode *tree;

	/*
	 * Keep every set of this source on the pinned path: never serve one from
	 * a private copy, however often it is counted (DESIGN.md §19).  The count
	 * pushdown sets this on the source an OR across columns becomes, where a
	 * dead TID may be contributed by a single leaf and the interlock the
	 * source carries is "every leaf holds a pin" (lion_source_pinned(): OR ->
	 * every child).  Everything else is free to be materialized under the
	 * rules on lion_posting_set_materialize().
	 */
	bool		nomaterialize;

	/*
	 * The sets are pairwise DISJOINT: they are distinct entries of one SCALAR
	 * lion index, so no row can be a member of two of them (DESIGN.md §15).
	 * The count of their union is then the SUM of their counts and no merge is
	 * needed, which is what lion_count_sources_cached() short-circuits when
	 * this is the only positive source.
	 *
	 * Only the caller that located the sets can know it: an IN list located
	 * through lion_posting_set_lookup_many() has it, because that function
	 * drops duplicates BY ENTRY and not merely by value.  It is a promise, not
	 * a hint - setting it on sets that may overlap double-counts rows - and
	 * lion_count_sources_cached() re-checks the cheap half of it (one index,
	 * not multi-key) before acting on it.
	 */
	bool		disjoint;

	/*
	 * A RANGE on a column that does not drive the count, taken as a source
	 * (DESIGN.md §32, "A range as a source") that was too large to collect
	 * into memory: the rows whose key lies in the range, which the caller
	 * counts as the SUM over the sets a walk of the range hands out - disjoint
	 * entries and summaries of one scalar column - each ANDed with the other
	 * sources in its place.  The count functions below never take one; the
	 * count pushdown, which defines the structure, expands it before it calls
	 * them (lion_node_count()).
	 */
	struct LionRangeSource *rangewalk;
} LionCountSource;

/*
 * Locate the posting set of the rows whose key column `attno` is NULL: that
 * column's reserved entry (DESIGN.md §14, §24).  Returns false when the
 * column holds no NULLs.
 *
 * Every lookup below names an INDEX COLUMN, 1-based, exactly as a ScanKey's
 * sk_attno and an IndexOptInfo's indexkeys[i] + 1 do.  The wrappers without
 * the column are that column being 1, which is all a single-column index has.
 */
extern bool lion_posting_set_lookup_null_col(Relation index, AttrNumber attno,
											LionPostingSet *ps);

static inline bool
lion_posting_set_lookup_null(Relation index, LionPostingSet *ps)
{
	return lion_posting_set_lookup_null_col(index, 1, ps);
}

/*
 * Locate the posting set of key in index.  Returns false (and fills *ps with
 * a released, not-found set) when the key has no entry.  keytype may differ
 * from the index's opcintype as long as the opfamily has a strategy-1
 * operator and a hash function for it; InvalidOid means "the index's own
 * type".  ERRORs when the type cannot be used with this index.
 */
extern bool lion_posting_set_lookup_col(Relation index, AttrNumber attno,
									   Datum key, Oid keytype,
									   LionPostingSet *ps);

static inline bool
lion_posting_set_lookup(Relation index, Datum key, Oid keytype,
					   LionPostingSet *ps)
{
	return lion_posting_set_lookup_col(index, 1, key, keytype, ps);
}

/*
 * Locate the posting sets of nvalues keys of one index at once: the IN list
 * of DESIGN.md §15.  isnull may be NULL (no value is NULL), and NULL values
 * are skipped, as `col = NULL` is never true.  The keys are hashed first and
 * their entries located in (bucket, hash) order, so the bucket pages are read
 * in ascending block order, and duplicates are dropped in one pass over that
 * order rather than by comparing every value with every earlier one.
 *
 * No two of the located sets are the same entry.  That is stronger than
 * "no two values were bytewise equal", and it is what the disjoint-sum
 * short-circuit of DESIGN.md §15 rests on: an opclass whose equality is not
 * byte equality (citext) has distinct values that reach ONE entry, and summing
 * that entry twice would count its rows twice.
 *
 * sets must have room for nvalues; the located sets come out packed at the
 * front and the return value is how many there are - every one of which the
 * caller must release.  *nfound (optional) is how many have an entry at all,
 * so *nfound == 0 means the union selects no rows.
 *
 * The pins the located INLINE sets keep are BOUNDED, however long the list:
 * past a budget of distinct leaves the sets come out NOPIN (DESIGN.md §15).
 */
extern int lion_posting_set_lookup_many_col(Relation index, AttrNumber attno,
										   Oid keytype, int nvalues,
										   const Datum *values,
										   const bool *isnull,
										   LionPostingSet *sets, int *nfound);

static inline int
lion_posting_set_lookup_many(Relation index, Oid keytype, int nvalues,
							const Datum *values, const bool *isnull,
							LionPostingSet *sets, int *nfound)
{
	return lion_posting_set_lookup_many_col(index, 1, keytype, nvalues, values,
										   isnull, sets, nfound);
}

/*
 * The posting set of the entry at (buf, off) of a leaf the caller holds
 * locked; an INLINE set takes a pin of its own on the leaf.
 */
extern void lion_posting_set_at(Relation index, LionState *state, Buffer buf,
								OffsetNumber off, LionPostingSet *ps);

/* Drop whatever pin/memory the posting set holds.  Idempotent. */
extern void lion_posting_set_release(LionPostingSet *ps);

/*
 * Drop an INLINE set's pin but keep its payload: the set becomes NOPIN.  For
 * callers that need no visibility-map interlock at all - a bitmap scan, whose
 * every TID is checked in the heap by the executor - and for the count
 * pushdown between two rows, whose later counts take a NOPIN set as they take
 * one located past the pin budget (lion_count_sources_cached()).
 */
extern void lion_posting_set_unpin(LionPostingSet *ps);

/*
 * lion_posting_set_lookup_col() for a caller that keeps MANY single lookups
 * located at once - the keys of a multi-key clause (DESIGN.md §17), up to
 * LION_MAX_QUERY_KEYS of them per clause: a pin the set keeps is charged to
 * the list pin budget of DESIGN.md §15, and a set found past that budget
 * comes out NOPIN, exactly like a list's.  *lastpinned is the leaf the
 * caller's previous set took (InvalidBuffer to start): another pin on it
 * costs no buffer and is not charged.
 */
extern bool lion_posting_set_lookup_budgeted_col(Relation index,
												 AttrNumber attno, Datum key,
												 Oid keytype,
												 LionPostingSet *ps,
												 Buffer *lastpinned);

/*
 * How many participants the parallel plan now starting has, the leader
 * included (DESIGN.md §27): each gets that share of the list pin budget.
 * 1 again when the plan ends.
 */
extern void lion_list_pin_participants(int participants);

/*
 * Everything needed to probe one key column with values of one search type:
 * DESIGN.md §21's cross-type resolution, made ONCE and shared by every path
 * that looks values up - the single and the batched lookup of lion_set.c
 * and the bitmap scan of lion_scan.c - because they walk the same tree, and a
 * path that descended where another scans would read the directory in an
 * order it is not in.
 *
 * A value of the index's own type uses the column's hash, equality and (when
 * the column is ordered) comparison.  A value of another type resolves, in
 * this order:
 *
 *	- UNORDERED column: hash and cross-type equality only - no comparison of
 *	  any kind, the family's cross-type proc 4 included, may be used on a
 *	  directory in (kind, hash, bytes) order;
 *	- the family's cross-type proc 4 (hascmp): descend as usual.  A list is
 *	  walked only when the family also orders the search type itself (proc 4
 *	  for (keytype, keytype)), which is what it is sorted with; otherwise each
 *	  value descends alone (walk = false);
 *	- a BINARY coercion to the key type (coerce): the value becomes one of the
 *	  index's own.  A cast function is never taken - implicit is not lossless;
 *	- neither (needscan): the leaves are walked with the cross-type equality.
 *
 * walk says that a list sorted with lion_probe_key_cmp() - sortproc when
 * hassort, else the hash - is in the directory's order, so it may be located
 * in one left-to-right leaf walk.
 */
typedef struct LionProbe
{
	bool		crosstype;		/* the keys are not the index's own type */
	FmgrInfo	eqproc;			/* crosstype: stored = search comparison */
	FmgrInfo	hashinfo;		/* crosstype: the search type's own hash */
	FmgrInfo	cmpproc;		/* stored <=> search (§21) */
	bool		hascmp;
	FmgrInfo	sortproc;		/* two SEARCH values, in directory order */
	bool		hassort;
	bool		walk;			/* the sorted list is in directory order */
	int16		typlen;			/* the search type, for datumIsEqual() */
	bool		typbyval;
	bool		coerce;			/* binary-coerced to the key type */
	bool		needscan;		/* no ordering for these values: walk leaves */
} LionProbe;

extern void lion_probe_init(Relation index, LionState *state, Oid keytype,
							LionProbe *probe);
extern bool lion_probe_find(Relation index, LionState *state, LionProbe *probe,
							Datum value, int lockmode, Buffer *buf,
							OffsetNumber *off);

/*
 * A WALK of one key column's directory leaves, for the single lookups of many
 * keys that a caller hands over one at a time, in the directory's order: the
 * FK-side join's dimension keys, sorted a batch at a time (DESIGN.md §27,
 * "Lookups in key order").  Where lion_posting_set_lookup_many_col() locates
 * a whole list under one walk, this keeps the walk's PLACE between lookups -
 * the leaf the last key was located on - because between two of them the
 * caller counts a set, which reads the heap, and no directory page may be
 * locked while it does.  So the walk holds no lock between lookups, and a pin
 * only on the leaf the last key's INLINE set pins as well (or, for a key with
 * no entry, until the next lookup): lion_lookup_walk_pause() lets it go, and
 * the next lookup reads the leaf again by its block number.
 *
 * A key sorted after the last one is on that leaf, or to its right: a
 * directory page never changes level and is never freed, and splits move keys
 * only rightwards (DESIGN.md §21, "Readers").  The walk stays on the leaf
 * while the key is below the high key it copied from it, steps to the right
 * sibling while the keys come that close together, and descends from the root
 * for a key further away, so a batch whose keys are dense reads each leaf once
 * and one whose keys are sparse costs a descent a key, as single lookups do.
 *
 * Keys must be handed over in the order lion_lookup_walk_cmp() sorts them, or
 * the walk would look for a key to the right of where it is; a caller whose
 * keys cannot be sorted so (lion_lookup_walk_ordered() false) still gets every
 * key located, each by a descent of its own.
 */
typedef struct LionLookupWalk
{
	Relation	index;
	LionState  *state;
	LionProbe	probe;
	BlockNumber blk;			/* the leaf the last key was located on */
	Buffer		buf;			/* ... still pinned, not locked; or Invalid */
	BlockNumber rightlink;		/* its right sibling when it was read */
	LionEntryTuple *hikey;		/* a copy of its high key then ... */
	Size		hikeylen;		/* ... this long; 0 if it was the rightmost */
	int			maxsteps;		/* a step costs a page, a descent height + 1 */
	bool		stepok;			/* the keys are close: step right, not descend */
	bool		prefetch;		/* read the next leaf ahead of the walk */
	BlockNumber prefetched;		/* the last block prefetched */
	uint32		ncmp;			/* comparisons made by a sort (interrupts) */
} LionLookupWalk;

extern void lion_lookup_walk_begin(LionLookupWalk *walk, Relation index,
								   AttrNumber attno, Oid keytype);
extern bool lion_lookup_walk_ordered(const LionLookupWalk *walk);
extern uint32 lion_lookup_walk_hash(LionLookupWalk *walk, Datum key);
extern int	lion_lookup_walk_cmp(LionLookupWalk *walk, Datum a, uint32 ahash,
								 Datum b, uint32 bhash);
extern bool lion_lookup_walk_find(LionLookupWalk *walk, Datum key, uint32 hash,
								  LionPostingSet *ps);
extern void lion_lookup_walk_pause(LionLookupWalk *walk);
extern void lion_lookup_walk_restart(LionLookupWalk *walk);

/*
 * What the order lion_lookup_walk_cmp() sorts keys into is made of: the
 * probe's comparison, its hash and the collation both are called under.  Two
 * walks whose orders are equal (the leaf partitions of a partitioned fact
 * table, whose fk indexes are usually one partitioned index's) take keys
 * sorted for either; others each need the keys sorted for themselves
 * (DESIGN.md §27, "A partitioned fact table").  `valid` is false for a walk
 * whose keys cannot be sorted at all (lion_lookup_walk_ordered()).
 */
typedef struct LionWalkOrder
{
	bool		valid;
	bool		hassort;
	Oid			sortproc;
	Oid			hashproc;
	Oid			collation;
} LionWalkOrder;

extern void lion_lookup_walk_order(const LionLookupWalk *walk,
								   LionWalkOrder *order);
extern bool lion_walk_order_equal(const LionWalkOrder *a,
								  const LionWalkOrder *b);

/*
 * One key in any order, by a descent of its own - what
 * lion_posting_set_lookup_col() does - with the probe the walk resolved once
 * instead of one resolved for every key (DESIGN.md §27, "The per-key path,
 * end to end").  The walk's place is neither used nor moved.
 */
extern bool lion_lookup_walk_descend(LionLookupWalk *walk, Datum key,
									 LionPostingSet *ps);

/*
 * Count the members of the intersection of nsets already located posting
 * sets, applying snapshot to every heap block that is not all-visible.
 * *stats is accumulated into (not reset) when it is not NULL.
 */
extern int64 lion_count_posting_sets(Relation heap, Snapshot snapshot,
									int nsets, LionPostingSet *sets,
									LionCountStats *stats);

/*
 * The general form: count the members of
 *
 *		(intersection of the positive sources) minus (the negated ones)
 *
 * where each source is itself the union of its posting sets.  At least one
 * positive source is required.
 */
extern int64 lion_count_sources(Relation heap, Snapshot snapshot,
							   int nsources, LionCountSource *sources,
							   LionCountStats *stats);

/*
 * The same, sharing one visibility cache across calls (DESIGN.md §9).  This
 * is what a driver that counts the same relation many times under one
 * snapshot - the GROUP BY path of §10, one call per group - should use; cache
 * may be NULL, and then this is exactly lion_count_sources().
 *
 * rel_read_only says that the statement neither modifies nor row-locks heap,
 * decided the way ScanRelIsReadOnly() decides it for core's scans; on
 * PostgreSQL 19 and later it lets the on-access pruning of the heap recheck
 * mark the pages it cleans all-visible (DESIGN.md §11).  It is a hint about
 * wasted work, never about correctness; pass false when in doubt.
 */
extern int64 lion_count_sources_cached(Relation heap, Snapshot snapshot,
									  int nsources, LionCountSource *sources,
									  LionCountStats *stats,
									  LionVisCache *cache,
									  bool rel_read_only);

/*
 * Does the same expression hold at least ONE row visible to snapshot?  The
 * existence test of count(DISTINCT k) (DESIGN.md §26): the very merge of
 * lion_count_sources_cached(), with the same visibility-map interlock and the
 * same recheck, stopped at the first container that shows a visible row.
 */
extern bool lion_exists_sources_cached(Relation heap, Snapshot snapshot,
									   int nsources, LionCountSource *sources,
									   LionCountStats *stats,
									   LionVisCache *cache,
									   bool rel_read_only);

/*
 * The members of the same intersection, copied into a private, pinless
 * posting set instead of counted (DESIGN.md §27): what a caller that
 * intersects one fixed set of sources with many located sets in turn builds
 * once.  Nothing is checked against the visibility map or the heap, so the
 * copy may only ever be counted beside a located set that carries the §9
 * interlock.  False when it would take more than maxbytes - unless spill
 * allows the copy to go on in a temporary file past them, and *spilled then
 * says whether it did.
 */
extern bool lion_sources_collect(Relation heap, Snapshot snapshot,
								 int nsources, LionCountSource *sources,
								 Size maxbytes, bool spill,
								 LionPostingSet *out, bool *spilled,
								 LionCountStats *stats);

/*
 * The heap's container keys cut into ranges for the participants of a
 * parallel plan: how many, and the keys they are cut from - no fewer than
 * minkeys a range, which the executor takes from pg_lion.parallel_range_keys
 * and the planner prices at its default.  lion_key_range() gives range r's
 * first key and the key past its last, the last range having no end
 * (LION_KEYS_END); lion_sources_collect_range() is the copy above of the keys
 * of one range alone.
 */
#define LION_KEYS_END	((uint64) PG_UINT32_MAX + 1)
#define LION_PARALLEL_RANGE_KEYS	16	/* pg_lion.parallel_range_keys */
extern PGDLLIMPORT int lion_parallel_range_keys;
extern int	lion_key_ranges(BlockNumber heapblocks, int participants,
							int minkeys, uint32 *ckeys);
extern void lion_key_range(int nranges, uint32 ckeys, int r, uint32 *lo,
						   uint64 *hi);
extern bool lion_sources_collect_range(Relation heap, Snapshot snapshot,
									   int nsources, LionCountSource *sources,
									   Size maxbytes, bool spill, uint32 lo,
									   uint64 hi, LionPostingSet *out,
									   bool *spilled, LionCountStats *stats);

/*
 * The counts of many located sets against ONE such copy, made in one walk of
 * container keys for all of them (DESIGN.md §10, "The groups of a walk,
 * counted together"): counts[g] is what lion_count_sources_cached() answers
 * for the AND of groups[g] and copy, a collected set (lion_sources_collect())
 * - each group's own set carrying the §9 interlock, as it does there.  The
 * copy's container at each key is read once and made a bitset image that
 * every group's container there is tested against, and the visibility map is
 * asked once per key for all of them.  The sets stay the caller's to release.
 * images is NULL, or ngroups page images the cursors may use as theirs.
 */
extern void lion_count_groups_copy(Relation heap, Snapshot snapshot,
								   int ngroups, LionPostingSet *groups,
								   const LionPostingSet *copy, int64 *counts,
								   LionCountStats *stats, LionVisCache *cache,
								   bool rel_read_only, PGAlignedBlock *images);

/* How many groups of index one lion_count_groups_copy() may take. */
extern int	lion_count_groups_batch(Relation index);

/*
 * THE GROUPS OF SEVERAL COLUMNS, DECODED KEY BY KEY (DESIGN.md §34,
 * lion_count_decode.c).  One pass of a GROUP BY of ncol columns: column c's
 * values in the pass are the nsets located sets of cols[c], and the rows of a
 * combination (v0, ..., vn) are those in every one of its sets and in the
 * collected WHERE (`where`, or every row without one).  Column 0's sets carry
 * the §9 interlock and must be located with their pins; the others' are read
 * with none.  Each combination with a visible row is added to the tally under
 * its code, v0 * radix[1] * ... + ... + vn, radix[c] being cols[c].nsets.
 */
#define LION_MAX_DECODE_COLS	8

typedef struct LionDecodeCol
{
	int			nsets;
	LionPostingSet *sets;
	PGAlignedBlock *images;		/* nsets page images for the cursors of
								 * CHAIN sets, or NULL for their own */
} LionDecodeCol;

/* What a decoded walk did, for EXPLAIN ANALYZE; summed over its passes. */
typedef struct LionDecodeStats
{
	int64		keys;			/* container keys decoded */
	int64		rows;			/* rows tallied, from the map or the heap */
	int64		rechecked;		/* rows the heap was asked about */
	int64		spills;			/* times the tally went to its temporary file */
} LionDecodeStats;

typedef struct LionDecodeTally LionDecodeTally;

extern LionDecodeTally *lion_decode_tally_create(MemoryContext cxt,
												 Size maxbytes);
extern bool lion_decode_tally_begin(LionDecodeTally *t, int ncol,
									const int *radix);
extern bool lion_decode_tally_next(LionDecodeTally *t, uint64 *code,
								   int64 *count);
extern void lion_decode_tally_end(LionDecodeTally *t);
extern Size lion_count_cursor_bytes(void);
extern void lion_decode_code_split(uint64 code, int ncol, const int *radix,
								   int *vals);
extern void lion_count_groups_decode(Relation heap, Snapshot snapshot,
									 int ncol, LionDecodeCol *cols,
									 const LionPostingSet *where,
									 LionDecodeTally *tally,
									 LionCountStats *stats,
									 LionDecodeStats *dstats,
									 LionVisCache *cache, bool rel_read_only);

/*
 * The same intersection collected ONCE for all the participants of a parallel
 * plan, into its dynamic shared memory (DESIGN.md §27, "One copy per query"):
 * the participants divide its container keys into chunks and collect them
 * together, the chunks past the memory the copy may take spill to files of
 * the plan's file set, and every participant then reads the one copy through
 * a view of its own (*out), which it releases as it would any posting set.
 * The shared state is lion_shared_copy_size() bytes of the plan's DSM chunk:
 * made by the leader (_init(), when the DSM is), attached to by each worker
 * (_attach()) and emptied between runs (_reinit(), when no participant is
 * running).  memory is what the copy may keep in shared memory; the rest of
 * it goes to files.  *chunks is how many chunks this participant collected,
 * *built whether it was the one that indexed the copy - once a run - and
 * *spilled whether any chunk went to a file.
 */
typedef struct LionSharedCopy LionSharedCopy;
struct dsa_area;
struct dsm_segment;
extern Size lion_shared_copy_size(void);
extern void lion_shared_copy_init(LionSharedCopy *sc, struct dsm_segment *seg,
								  int participants, Size memory,
								  BlockNumber heapblocks);
extern void lion_shared_copy_attach(LionSharedCopy *sc);
extern void lion_shared_copy_reinit(LionSharedCopy *sc, struct dsa_area *area,
									BlockNumber heapblocks);
extern void lion_shared_copy_collect(LionSharedCopy *sc, struct dsa_area *area,
									 Relation heap, Snapshot snapshot,
									 int nsources, LionCountSource *sources,
									 LionPostingSet *out, int *chunks,
									 bool *built, bool *spilled,
									 LionCountStats *stats);

/*
 * Does a count that reads ps again and again walk its pages every time?  A
 * CHAIN set that lion_count_sources_cached() keeps no private copy of - one
 * that is hopeless even as an ARRAY of members, or whose copy was tried and
 * did not fit (DESIGN.md §15, "Bounded cursors").  What the count pushdown
 * asks of a GROUP BY's lone WHERE set before it collects it (DESIGN.md §10,
 * "The WHERE sets, collected once").
 */
extern bool lion_posting_set_rewalked(const LionPostingSet *ps);

/*
 * How a collected range's union grows (lion_range_union_cb()), which the
 * planner prices (lion_cost_range_union(), DESIGN.md §32, "What collecting a
 * range costs"): an ARRAY container of at most this many members that comes
 * to an ARRAY union waits with the others that came, and is folded in once
 * the waiting members are LION_RANGE_UNION_PEND_MIN or half the union's,
 * whichever is more; anything larger is merged in at once.
 */
#define LION_RANGE_UNION_PEND_MIN	32

/*
 * The rows of one range over key column `attno` of index - every set a walk of
 * the range's INSIDE hands out, summaries included (DESIGN.md §32) - as ONE
 * private, pinless posting set: their union, collected in memory.  Like a
 * collected intersection it may only ever be counted beside a located set that
 * carries the §9 interlock.  False, with nothing allocated, when the union
 * would take more than maxbytes - unless spill allows it to be built a window
 * of container keys at a time into a temporary file, and *spilled then says
 * whether it was.  *held is what the set takes in memory, and *nsets and
 * *nsummaries say what the walk read.
 */
struct LionRange;
extern bool lion_range_collect(Relation index, AttrNumber attno,
							   struct LionRange *range, Size maxbytes,
							   bool spill, LionPostingSet *out, Size *held,
							   bool *spilled, int64 *nsets,
							   int64 *nsummaries);

/*
 * A summed range PROBED at the rows of the other sources F (DESIGN.md §32,
 * "Summed ranges: dense and probed"): F is collected once, every set a walk
 * of the range hands out is read only at F's container keys and marked
 * against F's rows, and the rows marked are counted once, ANDed with F.  The
 * same answer as the sum of the sets' counts, for a walk whose sets are
 * disjoint - a summed walk's are - in one count instead of one per set.
 *
 * lion_range_probe_begin() returns NULL when it cannot be taken: F has no
 * positive source, holds a range still to be walked (rangewalk), has no
 * positive source that carries the §9 interlock, or would not fit maxbytes
 * with the marks.  F must stay located until lion_range_probe_end().
 */
typedef struct LionRangeProbe LionRangeProbe;

extern LionRangeProbe *lion_range_probe_begin(Relation heap, Snapshot snapshot,
											  int nsources,
											  LionCountSource *sources,
											  Size maxbytes,
											  LionCountStats *stats);
extern void lion_range_probe_add(LionRangeProbe *rp, LionPostingSet *set);
extern int64 lion_range_probe_count(LionRangeProbe *rp, Relation heap,
									Snapshot snapshot, LionCountStats *stats,
									LionVisCache *cache, bool rel_read_only);
extern void lion_range_probe_end(LionRangeProbe *rp);

/*
 * The DESIGN.md section 9 entry point: locate nkeys (index, key) pairs and
 * count the intersection of their posting sets.  keytypes may be NULL, which
 * means every key already has its index's opcintype.
 */
extern int64 lion_count_keys(Relation heap, Snapshot snapshot, int nkeys,
							Relation *indexes, Datum *keys, Oid *keytypes,
							LionCountStats *stats);

/*
 * Walk the containers of (tree over sets) in ascending container-key order,
 * calling cb for each one; the container is only valid until cb returns, and
 * cb returning false stops the walk.  tree may be NULL, which means the union
 * of every set, exactly as in LionCountSource.
 *
 * This is the set algebra of lion_count_sources() without the visibility map:
 * what the bitmap scan of a multi-key opclass needs (DESIGN.md §17), where
 * every matching TID is handed to the executor and no heap page is skipped.
 * Returns the number of members emitted.
 *
 * The caller must have located every set and must release them afterwards.
 */
typedef bool (*lion_container_callback) (const LionContainer *c, void *arg);

extern int64 lion_sets_iterate(int nsets, LionPostingSet *sets,
							  LionKeyNode *tree,
							  lion_container_callback cb, void *arg);

/*
 * The same walk as a PULL: lion_stream_next() hands out one container per
 * call, in strictly ascending container key, each valid until the next call,
 * and NULL at the end (DESIGN.md §29.3).  keeppins keeps the §9 pin on the
 * page each current container came from until the stream moves past it; with
 * it off the cursors copy each posting leaf and let go of it at once, which
 * is what a plain index scan under an MVCC snapshot wants (§29.5).  An INLINE
 * set's own leaf pin is the caller's: lion_posting_set_unpin() it for no pin
 * at all.  The stream and its cursors are allocated in the current memory
 * context; lion_stream_end() drops every pin they hold.
 */
typedef struct LionSetStream LionSetStream;

extern LionSetStream *lion_stream_begin(int nsets, LionPostingSet *sets,
										LionKeyNode *tree, bool keeppins);

/*
 * The stream lion_sets_iterate() pulls, for a caller that pulls it itself: no
 * pins kept, and the tree planned against work_mem, so that a union too wide
 * to open at once is read as a windowed union (DESIGN.md §15, "Bounded
 * cursors").  What a bitmap scan wants (§28, "Bitmap scans").
 */
extern LionSetStream *lion_stream_begin_bounded(int nsets,
												LionPostingSet *sets,
												LionKeyNode *tree);
extern void lion_stream_seek(LionSetStream *st, uint32 target);
extern const LionContainer *lion_stream_next(LionSetStream *st);

/*
 * The first container at or above target, reached by the cursors' seeks
 * (§22) wherever the stream stands - the one it stands on, when that is at or
 * above target - or NULL at the end; valid until the stream moves again.
 * Targets only ever ascend: what a caller that SAMPLES a set's container
 * keys asks (DESIGN.md §29.11, "Correlated sets").
 */
extern const LionContainer *lion_stream_at(LionSetStream *st, uint32 target);

/*
 * The stream's targets lie far apart - a sample's strata, each many posting
 * leaves past the last: a seek past the leaf in hand descends the posting
 * tree at once, rather than stepping right first (LION_POSTING_SEEK_STEPS),
 * which only pays for a target a leaf or two away.  Before the first
 * container is asked for.
 */
extern void lion_stream_far(LionSetStream *st);

/*
 * A bitset image of one container key's whole range (LION_BITSET_WORDS
 * words), which lion_container_or_into_bitset() ORs containers into: turn it
 * back into a container in the smallest representation (dest has
 * LION_CONTAINER_MAX_SIZE bytes).
 */
extern void lion_bits_to_container(const uint64 *w, uint32 ckey,
								   LionContainer *dest);

/* A list's non-NULL values in lookup order, with their hashes (§29.4). */
extern int	lion_probe_sort(Relation index, AttrNumber attno, Oid keytype,
							int nvalues, const Datum *values,
							const bool *isnull, Datum *sorted, uint32 *hashes);
extern int	lion_probe_sort_unique(Datum *sorted, uint32 *hashes, int n,
								   bool typbyval, int16 typlen);

/*
 * The most values of one IN list a count locates at once (DESIGN.md §15, "A
 * list too long to locate at once"), the pushdown's plain count and
 * lion_index_count_any() alike.
 */
extern int	lion_array_batch_size(void);
extern void lion_stream_end(LionSetStream *st);

/*
 * A source of the TIDs one set of scan keys selects (DESIGN.md §29.3), in
 * lion_scan.c: what a plain index scan returns, independent of any
 * IndexScanDesc, so that a caller that wants lion's answer to a WHERE clause
 * as a stream - or as a set to probe - can open one.  lion_source_next()
 * hands out one container at a time, and never a TID the index does not
 * hold; when lion_source_sorted() the whole answer is one strictly ascending
 * run of container keys, each TID exactly once (SETS, UNION, WINDOW); a WALK
 * streams entry by entry and a long IN list batch by batch, ascending within
 * each.
 * lion_source_exact() is false when the TIDs are a superset that the caller
 * must recheck (§29.6).  keys must stay valid while the source is open.
 */
typedef struct LionSource LionSource;

extern LionSource *lion_source_open(Relation index, ScanKey keys, int nkeys,
									bool keeppins, MemoryContext cxt);

/*
 * The posting sets ONE scan key selects and the tree that combines them,
 * located as a scan locates them (§29.2), for a caller outside a scan - the
 * planner's intersection probe (DESIGN.md §29.11, "Correlated sets").  False
 * when the key is no tree over sets (`IS NOT NULL`, a range, a multi-key
 * query that needs every row); *nomatch when it selects nothing.  The caller
 * releases the *nsets sets, whether or not true is returned.
 */
extern bool lion_scankey_sets(Relation index, ScanKey skey, int *nsets,
							  LionPostingSet **sets, LionKeyNode **tree,
							  bool *nomatch);

extern const LionContainer *lion_source_next(LionSource *src);
extern bool lion_source_sorted(LionSource *src);
extern bool lion_source_exact(LionSource *src);
extern void lion_source_close(LionSource *src);

/*
 * The TIDs of one ordered scalar key column in the column's order (DESIGN.md
 * §30.11), in lion_scan.c: the entries its range keys select - every `<`,
 * `<=`, `>=` and `>` key on the column, and `IS NOT NULL` - walked in key
 * order, ascending or descending, and the TIDs of each entry in heap order;
 * the NULL entry's before them or after them, or not at all.  Rows with equal
 * keys come in no particular order.  The walk holds no pin and no lock between
 * calls: it reads each directory leaf and each posting page into private
 * memory, which is what an MVCC snapshot allows (§29.5), and every TID it
 * hands out is for the caller to look up in the heap.  *entries and *leaves
 * count what it has read.
 */
typedef struct LionOrderWalk LionOrderWalk;

#define LION_ORDER_NULLS_NONE	0	/* the NULL entry is not walked */
#define LION_ORDER_NULLS_FIRST	1
#define LION_ORDER_NULLS_LAST	2

extern LionOrderWalk *lion_order_walk_begin(Relation index, AttrNumber attno,
											ScanKey keys, int nkeys,
											bool backward, int nulls,
											MemoryContext cxt);
extern bool lion_order_walk_next(LionOrderWalk *w, ItemPointer tid);
extern void lion_order_walk_counts(LionOrderWalk *w, int64 *entries,
								   int64 *leaves);
extern void lion_order_walk_end(LionOrderWalk *w);

/*
 * Can (tree over sets) select anything at all?  False when a key the tree
 * requires has no entry in the index, which lets a caller skip the whole
 * merge - and, in the GROUP BY path, every group of it.
 */
extern bool lion_sets_satisfiable(int nsets, LionPostingSet *sets,
								 LionKeyNode *tree);

/* ---------------------------------------------------------------------
 * Range restrictions on one key column (DESIGN.md §28)
 * --------------------------------------------------------------------- */

/*
 * One bound of a range: `key <strategy> value`, strategy one of
 * LION_STRAT_LT .. LION_STRAT_GT.
 *
 * It is compared the way §21's probe resolution says a value of its type is
 * compared with the stored keys (lion_probe_init()): the column's own proc 4,
 * or the family's cross-type one (hascmp).  A bound that resolution finds no
 * comparison for - an unordered column, or a family that names a cross-type
 * range operator and no cross-type proc 4, which lionvalidate() refuses but a
 * catalogue may still hold - is tested with the operator itself (opproc).
 */
typedef struct LionRangeBound
{
	StrategyNumber strategy;
	Datum		value;
	bool		hascmp;
	FmgrInfo	cmpproc;		/* hascmp: stored key <=> value */
	FmgrInfo	opproc;			/* otherwise: stored key <op> value */
	Oid			collation;		/* ... called under the clause's collation */
} LionRangeBound;

/*
 * Every range clause of one key column, ANDed: what one bounded walk of that
 * column's entries returns (DESIGN.md §28).
 *
 *	ordered	every bound has a comparison and the column's directory is in
 *			its order, so the walk may DESCEND to its first lower bound and
 *			STOP at the first entry past an upper one.  Otherwise it tests
 *			every entry of the column, which is correct and linear.
 *	lower	the lower bound the walk descends to, or -1 for none: the walk
 *			then starts where the column does.
 *	upper	the upper bound a walk of what lies ABOVE the range descends to
 *			when it is the only one (nupper == 1), or -1.
 *	empty	a bound is NULL, so nothing can satisfy the range.
 *
 * The bound values are the caller's and must outlive the range.
 */
typedef struct LionRange
{
	LionState  *state;			/* the key column */
	int			nbounds;
	int			maxbounds;
	LionRangeBound *bounds;
	bool		ordered;
	int			lower;
	int			upper;
	int			nupper;
	bool		empty;
} LionRange;

/* What lion_range_test() says about one entry. */
#define LION_RANGE_MATCH	0	/* the entry satisfies every bound */
#define LION_RANGE_SKIP		1	/* it does not; a later entry may */
#define LION_RANGE_END		2	/* it does not, and no later entry will */

extern void lion_range_init(LionRange *range, Relation index,
							AttrNumber attno);
extern void lion_range_add(LionRange *range, Relation index,
						   StrategyNumber strategy, Oid opfuncid, Oid valtype,
						   Datum value, bool isnull, Oid collation);
extern int	lion_range_test(LionRange *range, const LionEntryTuple *entry);
extern bool lion_range_fails_upper(LionRange *range,
								   const LionEntryTuple *entry);
extern BlockNumber lion_range_first_leaf(Relation index, LionRange *range);
extern BlockNumber lion_range_last_leaf(Relation index, LionRange *range);

/*
 * The summaries of a column as the planner sees them (DESIGN.md §32,
 * "Costs"): the first ones of them, read off at most
 * LION_SUMMARY_SHAPE_LEAVES directory leaves.
 */
typedef struct LionSumShape
{
	double		nsummaries;
	double		rows;			/* the rows they hold */
	bool		complete;		/* every summary of the column was read */
} LionSumShape;

#define LION_SUMMARY_SHAPE_LEAVES	4

extern void lion_summary_shape(Relation index, LionState *col,
							   LionSumShape *shape);

/* ---------------------------------------------------------------------
 * Iterating every entry of an index (the GROUP BY path of section 10)
 * --------------------------------------------------------------------- */

/*
 * An ordered walk of every entry: the leftmost directory leaf, then the right
 * links (DESIGN.md §21).  For an ordered opclass the entries therefore come
 * out in key order, which is what lets the count pushdown claim pathkeys.
 *
 * The scan reads each leaf ONCE: it copies the entries it selects from the
 * leaf into a private batch under one share lock, gives the lock up, and
 * hands the copies out one by one (DESIGN.md §28, "One read per leaf").  The
 * position it comes back to for the next leaf is a KEY and not an offset: a
 * sorted directory inserts in the middle of a leaf and splits it, so offsets
 * are not stable the way they were on a bucket page.  Resuming at "the first
 * key above the last one examined" is stable under both, and under the entry
 * deletion of §18.
 *
 * An INLINE copy is counted against the visibility map, so the leaf it came
 * from stays PINNED from the copy until that set is handed out, which then
 * takes the pin over (DESIGN.md §9).  batchbuf is that pin; it is dropped as
 * soon as the batch holds no INLINE copy still to come, so a batch of CHAIN
 * entries pins nothing.
 */
typedef struct LionEntryScan
{
	Relation	index;
	LionState   *state;			/* the key column being walked (§24) */
	uint16		attno;			/* ... and its number; the walk ends at the
								 * first entry of the next column */
	BlockNumber blkno;			/* directory leaf to read next */
	BlockNumber stepfrom;		/* ... the leaf whose right link it is, or
								 * InvalidBlockNumber when the walk did not
								 * step there (a start, a phase, a pause) */
	LionRightWalk walk;			/* the steps right since the walk (or its
								 * phase) started (lion.h) */
	bool		haslast;		/* the key below is valid */
	int			lastkind;		/* LION_KIND_* of the last entry examined */
	uint32		lasthash;
	char	   *lastkey;		/* its stored bytes, in cxt */
	Size		lastkeylen;
	LionRange  *range;			/* the entries returned are bounded by this
								 * (DESIGN.md §28), or NULL */
	int			part;			/* LION_WALK_*: which of them */
	MemoryContext cxt;
	bool		done;

	/*
	 * SUMMARY POSTING SETS (DESIGN.md §32).  A walk whose caller only adds up
	 * what it hands out may read a column's summaries instead of its keys: it
	 * then walks in up to three PHASES - the VALUE entries of its part up to
	 * the first bucket it covers whole (LOWER), the SUMMARY entries of the
	 * buckets it covers whole (SUMS), and the VALUE entries after the last of
	 * them (UPPER).  The three are disjoint and together are exactly the rows
	 * of the part.  clipmax and clipmin are the bucket boundaries a VALUE phase
	 * is cut at - keys at or below clipmax, keys above clipmin, compared with
	 * the column's own comparison - and sumprev is the key of the last bucket
	 * the SUMS phase has taken or started after.
	 */
	bool		usesum;			/* the caller allows summaries */
	int			phase;			/* LION_PHASE_* being walked */
	int			nextphase;		/* ... and the one to set up at the next read */
	int			resumekind;		/* resume after every entry of the column up
								 * to this kind, instead of after lastkey */
	bool		hasclipmax;
	bool		hasclipmin;
	bool		hassumprev;
	char	   *clipmax;		/* in phasecxt */
	Size		clipmaxlen;
	char	   *clipmin;
	Size		clipminlen;
	char	   *sumprev;
	Size		sumprevlen;
	uint32		sumprevhash;
	MemoryContext phasecxt;
	int64		nsummaries;		/* summary entries handed out */

	/* The batch: what the last leaf read selected, copied out of it. */
	MemoryContext batchcxt;
	LionEntryTuple **bentry;	/* the copies */
	Size	   *bsize;			/* their item sizes */
	OffsetNumber *boff;			/* where they were on the leaf */
	char	   *bpage;			/* BLCKSZ bytes the copies are packed into */
	int			nbatch;
	int			maxbatch;
	int			nextbatch;		/* the next one to hand out */
	int			lastinline;		/* the last INLINE one, or -1 */
	BlockNumber batchblk;		/* the leaf they came from */
	Buffer		batchbuf;		/* its pin, while an INLINE copy is to come */
	int64		nleaves;		/* leaves this walk has read */
} LionEntryScan;

/*
 * Which entries of a range's column a walk returns (DESIGN.md §28).  An
 * ordered range selects one contiguous run of the column's VALUE entries, and
 * its complement is the run before it and the run after it:
 *
 *	INSIDE	the entries the range selects (lion_entry_scan_begin_range())
 *	BELOW	the VALUE entries that fail a LOWER bound and no upper one: the
 *			column's first entries, up to where the range begins
 *	ABOVE	the VALUE entries that fail an UPPER bound: the column's last ones
 *
 * BELOW and ABOVE are disjoint, and with INSIDE they are every VALUE entry of
 * the column.  Both need an ordered range (LionRange.ordered).
 */
#define LION_WALK_ALL		0	/* no range: every entry */
#define LION_WALK_INSIDE	1
#define LION_WALK_BELOW		2
#define LION_WALK_ABOVE		3

/* The phases of a walk that uses summaries (DESIGN.md §32). */
#define LION_PHASE_VALUES	0	/* no summaries: the part's VALUE entries */
#define LION_PHASE_LOWER	1	/* VALUE entries at or below clipmax */
#define LION_PHASE_SUMS		2	/* the summaries of whole buckets */
#define LION_PHASE_UPPER	3	/* VALUE entries above clipmin */
#define LION_PHASE_DONE		4

extern void lion_entry_scan_begin_col(LionEntryScan *es, Relation index,
									 AttrNumber attno);

static inline void
lion_entry_scan_begin(LionEntryScan *es, Relation index)
{
	lion_entry_scan_begin_col(es, index, 1);
}

/*
 * The same walk, returning only the entries a range selects (DESIGN.md §28):
 * it starts at the leaf the range's lower bound lives on and ends at the first
 * entry past an upper bound.  The reserved NULL and EMPTY entries never come
 * out of it.  range may be NULL, which is lion_entry_scan_begin_col().
 */
extern void lion_entry_scan_begin_range(LionEntryScan *es, Relation index,
									   AttrNumber attno, LionRange *range);

/*
 * The walk of one part of an ordered range's column (LION_WALK_*, above):
 * INSIDE is lion_entry_scan_begin_range(), BELOW starts where the column
 * starts and ends where the range begins, ABOVE starts at the leaf an upper
 * bound lives on and ends with the column.
 */
extern void lion_entry_scan_begin_part(LionEntryScan *es, Relation index,
									  AttrNumber attno, LionRange *range,
									  int part);

/*
 * The same walk for a caller that only ADDS UP what it hands out - a sum, or
 * a union - which may therefore be handed a column's SUMMARY entries in place
 * of the keys they cover (DESIGN.md §32): the entries are still pairwise
 * disjoint and still cover exactly the rows of the part.  On a column without
 * summaries, and for an unordered range, it is lion_entry_scan_begin_part().
 * The NULL entry never comes out of a walk that uses summaries; LION_WALK_ALL
 * is then every VALUE entry, which is what the sum over a column needs.
 */
extern void lion_entry_scan_begin_sum(LionEntryScan *es, Relation index,
									 AttrNumber attno, LionRange *range,
									 int part);

/*
 * ... and only that: the walk is set up and true is returned when it WILL
 * read summaries; otherwise false, with nothing left to end, and the caller
 * walks the keys its own way.  What the bitmap and plain index scans ask
 * (DESIGN.md §28, "Bitmap scans"), whose key walks predate §32.
 */
extern bool lion_entry_scan_begin_summed(LionEntryScan *es, Relation index,
										 AttrNumber attno, LionRange *range,
										 int part);

/*
 * Fetch the next entry.  On true, *key is a private copy of the entry's key
 * (palloc'd in the current context) and *ps is its located posting set, which
 * the caller must hand to lion_posting_set_release().  Entries whose posting
 * set is empty are skipped.  The reserved NULL entry (DESIGN.md §14) comes
 * out like any other, with ps->keyisnull set and *key meaningless.
 */
extern bool lion_entry_scan_next(LionEntryScan *es, Datum *key,
								LionPostingSet *ps);

/*
 * The next entry as the copy the leaf read made, valid until the next call:
 * no set located, no key copied and no pin kept - the batch's pin for its
 * INLINE copies (DESIGN.md §9) is dropped at once.  For a caller that hands
 * every TID it reads to the executor, which visits each in the heap: a bitmap
 * or plain index scan (§28, §29.5).  A walk is read either this way or with
 * lion_entry_scan_next(), never both.  *itemlen is the entry's item size.
 */
extern LionEntryTuple *lion_entry_scan_next_copy(LionEntryScan *es,
												 Size *itemlen);

/*
 * Read ONE more leaf of the walk and say how many entries on it the walk
 * would return, without copying or locating any of them: the cheap half of
 * deciding how to evaluate a range (DESIGN.md §28).  A walk is either counted
 * this way or fetched with lion_entry_scan_next(), never both.
 */
extern int64 lion_entry_scan_skip_leaf(LionEntryScan *es);

/*
 * How many entries of the leaf the walk read last are still to be handed out
 * by lion_entry_scan_next() before it reads another.
 */
static inline int
lion_entry_scan_batch_left(const LionEntryScan *es)
{
	return es->nbatch - es->nextbatch;
}

/*
 * The caller is about to hand a row to the executor and may not come back
 * for as long as a cursor stays open.  A walk must not keep a directory leaf
 * pinned across that (VACUUM would wait for it), so the INLINE copies still
 * to come are dropped with their pin and the leaf is read again on the next
 * call, from the last entry handed out.  A batch of CHAIN copies is kept.
 */
extern void lion_entry_scan_pause(LionEntryScan *es);
extern void lion_entry_scan_end(LionEntryScan *es);

/* ---------------------------------------------------------------------
 * Shared helpers
 * --------------------------------------------------------------------- */

/*
 * Oid of the "lion" access method, from the AMNAME syscache (never a
 * process-local static: DROP EXTENSION + CREATE EXTENSION in one backend
 * gives the access method a new Oid).
 */
extern Oid	lion_get_am_oid(void);

/*
 * EXECUTE on a function, or on an aggregate and its support functions, that
 * the query a count stands for would call - the checks ExecInitFunc() and
 * ExecInitAgg() make, with the same errors and the object-access hook.  Made
 * when the count starts, never at plan time (DESIGN.md §9, "Privileges").
 */
extern void lion_check_execute(Oid funcid);
extern void lion_check_aggregate_execute(Oid aggfnoid);

/* ---------------------------------------------------------------------
 * the LionCount custom scan, lion_plan_*.c and lion_exec_*.c (DESIGN.md
 * section 10)
 * --------------------------------------------------------------------- */

extern PGDLLIMPORT bool lion_enable_count_pushdown;

/*
 * Whether an FK-side join that probes its fact filters may collect them part
 * way through a run, once probing has cost what collecting them would
 * (DESIGN.md §27, "Probed, then collected"): an executor setting, for
 * comparing the two ways.
 */
extern PGDLLIMPORT bool lion_enable_filter_switch;
extern PGDLLIMPORT create_upper_paths_hook_type lion_prev_create_upper_paths_hook;

extern void lion_count_scan_register(void);
extern void lion_create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
								   RelOptInfo *input_rel,
								   RelOptInfo *output_rel,
								   void *extra);

/*
 * The FK-side semi and anti join as a join path (DESIGN.md §27, "The semi and
 * anti join as a join path"): pg_lion.enable_semijoin, and the
 * set_join_pathlist_hook that offers it, chained to the previous one.
 */
extern PGDLLIMPORT bool lion_enable_semijoin;
extern PGDLLIMPORT set_join_pathlist_hook_type lion_prev_set_join_pathlist_hook;

extern void lion_set_join_pathlist(PlannerInfo *root, RelOptInfo *joinrel,
								   RelOptInfo *outerrel, RelOptInfo *innerrel,
								   JoinType jointype,
								   JoinPathExtraData *extra);

/*
 * The cost model's shared terms (DESIGN.md section 10, "The units"): what a
 * merge of posting sets costs in CPU, used by the count pushdown and by
 * lioncostestimate() for the AND of sets an index scan makes; and a column's
 * correlation with the heap order less ANALYZE's tie-break (lion_amcost.c).
 */
extern double lion_merge_cpu_cost(int nsrc, const double *members,
								  const double *containers, const bool *inmem,
								  double tuples, double isect, double *probes);
extern double lion_merge_cpu_cost_sets(int nsrc, const double *members,
									   const double *containers,
									   const double *nsets,
									   const double *setcontainers,
									   const double *setpages,
									   const bool *inmem, double tuples,
									   double isect, double *probes);

/*
 * The AND of the posting sets of n clauses of one relation (DESIGN.md §29.11,
 * "One price for the AND of sets"): the price lion_cost_count_rel() charges
 * an ungrouped count's WHERE sources, and lioncostestimate() a scan that ANDs
 * the same sets - the index's work only.  Returns the total; *out has its
 * parts, whose costs add up to it:
 *
 *	leafpages / leafcost	the lookups' directory leaves: one read at
 *							random_page_cost a lookup, an IN list's in key
 *							order
 *	setpages / setcost		the posting pages: the driver's walked in order,
 *							the others' as far as the seeks reach
 *	cpu						the descents, the IN lists' unions and the AND's
 *							merge
 */
typedef struct LionAndCost
{
	int			nsrc;
	double		leafpages;
	Cost		leafcost;
	double		setpages;
	Cost		setcost;
	Cost		cpu;
} LionAndCost;

extern Cost lion_cost_set_and(PlannerInfo *root, RelOptInfo *rel, int n,
							  IndexOptInfo **idxs, const AttrNumber *cols,
							  Node **clauses, double isect, LionAndCost *out);
extern double lion_containers_for(double heap_pages, double members);
extern double lion_index_column_posting_share(PlannerInfo *root,
											  RelOptInfo *rel,
											  IndexOptInfo *idx,
											  AttrNumber col);
extern double lion_var_heap_correlation(PlannerInfo *root, Index relid,
										Var *var);

/* ---------------------------------------------------------------------
 * lion_ordered.c (DESIGN.md section 30): the LionOrdered CustomScan
 * --------------------------------------------------------------------- */

extern PGDLLIMPORT bool lion_enable_ordered_scan;

/* GUC, scan methods and set_rel_pathlist_hook; called from _PG_init. */
extern void lion_ordered_init(void);

/* ---------------------------------------------------------------------
 * lion_selfuncs.c (DESIGN.md §28, "The endpoint probe")
 * --------------------------------------------------------------------- */

/* get_relation_stats_hook; called from _PG_init. */
extern void lion_selfuncs_init(void);

/*
 * While lion prices one of its own accesses to rel, a range among `clauses`
 * that bounds a column past its histogram's ends is estimated with the ends
 * a lion index on the column holds.  lion_probe_begin() says whether a scope
 * began; lion_probe_end() must then close it, on error too.  Inside it,
 * lion_probe_rel_rows() is rel's rows as that estimate sees them (rel->rows
 * outside).  lion_amcostestimate() is lioncostestimate() in such a scope.
 */
extern bool lion_probe_begin(PlannerInfo *root, RelOptInfo *rel,
							 List *clauses);
extern void lion_probe_end(void);
extern double lion_probe_rel_rows(PlannerInfo *root, RelOptInfo *rel);

/*
 * What the intersection probe found of rel's own restriction clauses: the
 * factor lion_probe_rel_rows() applies, 1 where it measured nothing that
 * differs from core's estimate.
 */
extern double lion_probe_rel_factor(PlannerInfo *root, RelOptInfo *rel);

/*
 * The intersection probe (DESIGN.md §29.11, "Correlated sets"): what lion's
 * estimate of a conjunction holding the n set clauses - on key columns
 * cols[] (1-based) of lion index idx of rel - is to be multiplied by, 1 when
 * the probe measured nothing that differs from core's estimate of them.
 * lion_probe_rel_rows() applies it to rel's own restriction clauses.
 */
extern PGDLLIMPORT bool lion_enable_intersection_probe;

/*
 * Whether an AND of posting sets may look its running intersection up in the
 * containers of a union at a key instead of building the union (DESIGN.md
 * §29.11, "Unions probed"): an executor setting, for comparing the two.
 */
extern PGDLLIMPORT bool lion_enable_union_probe;

/*
 * ... and whether it may do the same with a nested tree - an AND of ORs, an
 * OR of ANDs - evaluating the tree for the intersection's members alone
 * rather than building its container (DESIGN.md §29.11, "Trees probed").
 * Off, a nested tree is evaluated whole at every key it is sought to, as it
 * was; the answers are the same either way.
 */
extern PGDLLIMPORT bool lion_enable_tree_probe;
extern double lion_isect_factor(PlannerInfo *root, RelOptInfo *rel,
								IndexOptInfo *idx, int n,
								const AttrNumber *cols, Node **clauses);

/*
 * One form for one filter (DESIGN.md §29.11, "An OR of equalities is its IN
 * list"): an OR of equalities on one expression with constants - one
 * operator, collation and constant type - as the `expr = ANY (array)` it
 * spells, or NULL; and a restriction clause as lion's estimates take it,
 * that list for such an OR and `col = true` / `col = false` for a boolean
 * column tested by itself, bare otherwise.
 */
extern Node *lion_or_as_array(Node *clause);
extern Node *lion_canonical_clause(Node *clause);
#if PG_VERSION_NUM < 180000

/*
 * PostgreSQL 16 and 17: the unparameterized paths of rel's lion indexes
 * lionidx that answer such an OR as that list, as 18's own index matching
 * builds them (DESIGN.md §29.11, "An OR of equalities is its IN list").
 */
extern List *lion_or_list_paths(PlannerInfo *root, RelOptInfo *rel,
								List *lionidx);
#endif
extern void lion_amcostestimate(PlannerInfo *root, IndexPath *path,
								double loop_count, Cost *indexStartupCost,
								Cost *indexTotalCost,
								Selectivity *indexSelectivity,
								double *indexCorrelation, double *indexPages);

/*
 * The part of a plain scan's price above cost_index()'s uncorrelated end,
 * which lion charges the path once rel's paths are built (DESIGN.md §29.11).
 */
extern void lion_plain_note_remainder(PlannerInfo *root, IndexPath *path,
									  Cost remainder);

#endif							/* LION_COUNT_H */
