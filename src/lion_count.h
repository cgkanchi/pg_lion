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
	bool		found;			/* false: the key has no entry at all */
	bool		is_inline;		/* payload/paylen valid, else head valid */
	BlockNumber head;			/* CHAIN: first container page */
	char	   *payload;		/* INLINE: private copy of the payload */
	Size		paylen;
	Buffer		pinbuf;			/* INLINE: pinned bucket page, else Invalid */
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
 * Locate the posting set of the rows whose key is NULL: the reserved entry of
 * bucket 0 (DESIGN.md §14).  Returns false when the index holds no NULLs.
 */
extern bool lion_posting_set_lookup_null(Relation index, LionPostingSet *ps);

/*
 * Locate the posting set of key in index.  Returns false (and fills *ps with
 * a released, not-found set) when the key has no entry.  keytype may differ
 * from the index's opcintype as long as the opfamily has a strategy-1
 * operator and a hash function for it; InvalidOid means "the index's own
 * type".  ERRORs when the type cannot be used with this index.
 */
extern bool lion_posting_set_lookup(Relation index, Datum key, Oid keytype,
								   LionPostingSet *ps);

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
 */
extern int lion_posting_set_lookup_many(Relation index, Oid keytype,
									   int nvalues, const Datum *values,
									   const bool *isnull,
									   LionPostingSet *sets, int *nfound);

/* Drop whatever pin/memory the posting set holds.  Idempotent. */
extern void lion_posting_set_release(LionPostingSet *ps);

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
 */
extern int64 lion_count_sources_cached(Relation heap, Snapshot snapshot,
									  int nsources, LionCountSource *sources,
									  LionCountStats *stats,
									  LionVisCache *cache);

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
 * Can (tree over sets) select anything at all?  False when a key the tree
 * requires has no entry in the index, which lets a caller skip the whole
 * merge - and, in the GROUP BY path, every group of it.
 */
extern bool lion_sets_satisfiable(int nsets, LionPostingSet *sets,
								 LionKeyNode *tree);

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
	LionState   *state;
	BlockNumber blkno;			/* directory leaf to read next */
	bool		haslast;		/* the key below is valid */
	int			lastkind;		/* LION_KIND_* of the last entry returned */
	uint32		lasthash;
	char	   *lastkey;		/* its stored bytes, in cxt */
	Size		lastkeylen;
	int			onpage;			/* entries returned from the current leaf */
	MemoryContext cxt;
	bool		done;
} LionEntryScan;

extern void lion_entry_scan_begin(LionEntryScan *es, Relation index);

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
