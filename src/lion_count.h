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

#include "access/skey.h"
#include "nodes/pathnodes.h"
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
 * lion_count.c: lion_ecursor_build() for the first and lion_sum_is_cheaper()
 * for both.
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
} LionCountStats;

/*
 * A per-query cache of heap visibility answers for blocks that are not
 * all-visible (DESIGN.md §9).
 *
 * A TID's visibility under one MVCC snapshot cannot change while that
 * snapshot is held, so the answer for every root line pointer of a heap page
 * may be resolved once and reused by every later recheck of that page - which
 * is what turns the GROUP BY path's one-recheck-per-group-per-dirty-page into
 * one pass over the dirty pages.  The safety argument is in lion_count.c above
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
 * enough is in lion_count.c above lion_posting_set_materialize().
 */
struct LionMatSet;				/* private to lion_count.c */

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
	 * a one-shot count apart from a set the GROUP BY path reuses.
	 */
	MemoryContext cxt;
	int			nuses;
	struct LionMatSet *mat;		/* materialized containers, or NULL */

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

/* Drop whatever pin/memory the posting set holds.  Idempotent. */
extern void lion_posting_set_release(LionPostingSet *ps);

/*
 * Drop an INLINE set's pin but keep its payload: the set becomes NOPIN.  For
 * callers that need no visibility-map interlock at all - a bitmap scan, whose
 * every TID is checked in the heap by the executor.
 */
extern void lion_posting_set_unpin(LionPostingSet *ps);

/*
 * Everything needed to probe one key column with values of one search type:
 * DESIGN.md §21's cross-type resolution, made ONCE and shared by every path
 * that looks values up - the single and the batched lookup of lion_count.c
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
extern void lion_stream_seek(LionSetStream *st, uint32 target);
extern const LionContainer *lion_stream_next(LionSetStream *st);

/*
 * A bitset image of one container key's whole range (LION_BITSET_WORDS
 * words): OR a container into it, and turn it back into a container in the
 * smallest representation (dest has LION_CONTAINER_MAX_SIZE bytes).
 */
extern void lion_bits_or_container(uint64 *w, const LionContainer *c);
extern void lion_bits_to_container(const uint64 *w, uint32 ckey,
								   LionContainer *dest);

/* A list's non-NULL values in lookup order, with their hashes (§29.4). */
extern int	lion_probe_sort(Relation index, AttrNumber attno, Oid keytype,
							int nvalues, const Datum *values,
							const bool *isnull, Datum *sorted, uint32 *hashes);
extern void lion_stream_end(LionSetStream *st);

/*
 * A source of the TIDs one set of scan keys selects (DESIGN.md §29.3), in
 * lion_scan.c: what a plain index scan returns, independent of any
 * IndexScanDesc, so that a caller that wants lion's answer to a WHERE clause
 * as a stream - or as a set to probe - can open one.  lion_source_next()
 * hands out one container at a time, and never a TID the index does not
 * hold; when lion_source_sorted() the whole answer is one strictly ascending
 * run of container keys, each TID exactly once (SETS, UNION); a WALK streams
 * entry by entry and a long IN list batch by batch, ascending within each.
 * lion_source_exact() is false when the TIDs are a superset that the caller
 * must recheck (§29.6).  keys must stay valid while the source is open.
 */
typedef struct LionSource LionSource;

extern LionSource *lion_source_open(Relation index, ScanKey keys, int nkeys,
									bool keeppins, MemoryContext cxt);
extern const LionContainer *lion_source_next(LionSource *src);
extern bool lion_source_sorted(LionSource *src);
extern bool lion_source_exact(LionSource *src);
extern void lion_source_close(LionSource *src);

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
extern BlockNumber lion_range_first_leaf(Relation index, LionRange *range);

/* ---------------------------------------------------------------------
 * Iterating every entry of an index (the GROUP BY path of section 10)
 * --------------------------------------------------------------------- */

/*
 * An ordered walk of every entry: the leftmost directory leaf, then the right
 * links (DESIGN.md §21).  For an ordered opclass the entries therefore come
 * out in key order, which is what lets the count pushdown claim pathkeys.
 *
 * The scan gives up its lock between two entries, and the position it comes
 * back to is a KEY and not an offset: a sorted directory inserts in the
 * middle of a leaf and splits it, so offsets are not stable the way they were
 * on a bucket page.  Resuming at "the first key above the last one returned"
 * is stable under both, and under the entry deletion of §18.
 */
typedef struct LionEntryScan
{
	Relation	index;
	LionState   *state;			/* the key column being walked (§24) */
	uint16		attno;			/* ... and its number; the walk ends at the
								 * first entry of the next column */
	BlockNumber blkno;			/* directory leaf to read next */
	bool		haslast;		/* the key below is valid */
	int			lastkind;		/* LION_KIND_* of the last entry returned */
	uint32		lasthash;
	char	   *lastkey;		/* its stored bytes, in cxt */
	Size		lastkeylen;
	int			onpage;			/* entries returned from the current leaf */
	LionRange  *range;			/* the entries returned are bounded by this
								 * (DESIGN.md §28), or NULL */
	MemoryContext cxt;
	bool		done;
} LionEntryScan;

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
 * Fetch the next entry.  On true, *key is a private copy of the entry's key
 * (palloc'd in the current context) and *ps is its located posting set, which
 * the caller must hand to lion_posting_set_release().  Entries whose posting
 * set is empty are skipped.  The reserved NULL entry (DESIGN.md §14) comes
 * out like any other, with ps->keyisnull set and *key meaningless.
 */
extern bool lion_entry_scan_next(LionEntryScan *es, Datum *key,
								LionPostingSet *ps);
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
 * lion_customscan.c (DESIGN.md section 10)
 * --------------------------------------------------------------------- */

extern PGDLLIMPORT bool lion_enable_count_pushdown;
extern PGDLLIMPORT create_upper_paths_hook_type lion_prev_create_upper_paths_hook;

extern void lion_create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
								   RelOptInfo *input_rel,
								   RelOptInfo *output_rel,
								   void *extra);

#endif							/* LION_COUNT_H */
