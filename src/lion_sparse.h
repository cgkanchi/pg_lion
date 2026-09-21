/*-------------------------------------------------------------------------
 * lion_sparse.h
 *	  Sparse segments for pg_lion.  See DESIGN.md §13.
 *
 *	  A segment is a fourth kind of item (LION_CT_SPARSE) that may appear
 *	  wherever a container may appear: as a page item on a container page, or
 *	  inside an INLINE entry payload.  It shares the 8-byte LionContainer
 *	  header and holds a sorted list of (ckey, lo) pairs:
 *
 *		ckey		= first container key covered by the segment
 *		cardinality	= number of pairs n, 1 .. LION_SPARSE_MAX_PAIRS
 *		payload		= uint32 ckeys[n], then uint16 los[n]
 *		size		= 8 + 6n <= LION_CONTAINER_MAX_SIZE
 *
 *	  Pairs are sorted by (ckey, lo) and are unique.  A segment therefore
 *	  covers the ckey range [ckeys[0], ckeys[n-1]], and ckeys[0] is repeated
 *	  in the header so that page code can order items by their first ckey
 *	  without looking at the payload.
 *
 *	  The point of the format is density: a container costs 12 bytes plus
 *	  MAXALIGN even for a single member, while a pair costs 6 bytes.  A ckey
 *	  with fewer than LION_SPARSE_THRESHOLD members lives in a segment; from
 *	  that many on, a regular ARRAY container (8 + 2n) is cheaper and the
 *	  ckey gets one.
 *
 *	  THE NON-INTERLEAVING RULE.  Every ckey of a posting set is covered by
 *	  at most one item, and the ckey ranges of the items of a posting set are
 *	  ordered and disjoint: item i's last ckey is strictly below item i+1's
 *	  first ckey, on a page, across a chain and inside an INLINE payload.
 *	  That is what lets every reader keep treating a posting set as one
 *	  ascending run of ckeys, and what the insert path takes such care to
 *	  preserve when it splits a segment around a ckey it has promoted to a
 *	  container.
 *
 *	  Like lion_container.[ch] this module depends only on c.h (plus the
 *	  container library, for building extracted containers), so that it can
 *	  be unit-tested outside the server with -DFRONTEND
 *	  (test/unit/sparse_test.c).  No palloc, no elog.
 *
 *	  Mutators work IN PLACE on a caller-supplied buffer with at least
 *	  LION_CONTAINER_MAX_SIZE bytes of capacity.  Read-only functions accept a
 *	  pointer directly into a page item (a page item is MAXALIGNed, so its
 *	  uint32 ckeys[] is aligned); a segment inside an INLINE payload is
 *	  unaligned and must be copied out with lion_inline_fetch() first, exactly
 *	  like a container.
 *-------------------------------------------------------------------------
 */
#ifndef LION_SPARSE_H
#define LION_SPARSE_H

#include "c.h"

#include "lion_container.h"

/* Bytes one (ckey, lo) pair costs. */
#define LION_SPARSE_PAIR_SIZE	(sizeof(uint32) + sizeof(uint16))	/* 6 */

/* Largest number of pairs that fits the common item size bound. */
#define LION_SPARSE_MAX_PAIRS \
	((uint32) ((LION_CONTAINER_MAX_SIZE - LION_CONTAINER_HDRSZ) / LION_SPARSE_PAIR_SIZE))

/*
 * Members of one ckey from which a regular container is cheaper than pairs:
 * an ARRAY container costs 8 + 2n bytes against 6n, so they break even at
 * n = 2 and the container also saves the per-item line pointer and MAXALIGN
 * padding from n = 4 on.  Below this many members a ckey lives in a segment.
 */
#define LION_SPARSE_THRESHOLD	4

StaticAssertDecl(LION_SPARSE_MAX_PAIRS == 682,
				 "pg_lion: unexpected LION_SPARSE_MAX_PAIRS");

/* Payload accessors.  los[] follows ckeys[], so it moves when n changes. */
#define LION_SPARSE_CKEYS(s) \
	((uint32 *) ((char *) (s) + LION_CONTAINER_HDRSZ))
#define LION_SPARSE_CKEYS_CONST(s) \
	((const uint32 *) ((const char *) (s) + LION_CONTAINER_HDRSZ))
#define LION_SPARSE_LOS_AT(s, n) \
	((uint16 *) ((char *) (s) + LION_CONTAINER_HDRSZ + (Size) (n) * sizeof(uint32)))
#define LION_SPARSE_LOS(s)			LION_SPARSE_LOS_AT((s), (s)->cardinality)
#define LION_SPARSE_LOS_CONST(s) \
	((const uint16 *) ((const char *) (s) + LION_CONTAINER_HDRSZ + \
					   (Size) (s)->cardinality * sizeof(uint32)))

/* Size of a segment with npairs pairs. */
static inline Size
lion_sparse_size_for(uint32 npairs)
{
	return LION_CONTAINER_HDRSZ + (Size) npairs * LION_SPARSE_PAIR_SIZE;
}

static inline Size
lion_sparse_size(const LionContainer *s)
{
	Assert(s->type == LION_CT_SPARSE);
	return lion_sparse_size_for(s->cardinality);
}

static inline bool
lion_item_is_sparse(const LionContainer *item)
{
	return item->type == LION_CT_SPARSE;
}

/*
 * Size of any item of a posting set, container or segment.  Everything that
 * derives an item's length from its header goes through this.
 */
static inline Size
lion_item_size(const LionContainer *item)
{
	if (item->type == LION_CT_SPARSE)
		return lion_sparse_size(item);
	return lion_container_size(item);
}

/* First ckey an item covers (the header field, for every item kind). */
static inline uint32
lion_item_first_ckey(const LionContainer *item)
{
	return item->ckey;
}

/* Last ckey an item covers: a container covers one, a segment a range. */
static inline uint32
lion_item_last_ckey(const LionContainer *item)
{
	if (item->type == LION_CT_SPARSE)
	{
		Assert(item->cardinality > 0);
		return LION_SPARSE_CKEYS_CONST(item)[item->cardinality - 1];
	}
	return item->ckey;
}

/* Does an item cover ckey? */
static inline bool
lion_item_covers(const LionContainer *item, uint32 ckey)
{
	return ckey >= lion_item_first_ckey(item) && ckey <= lion_item_last_ckey(item);
}

/* Number of members an item holds (pairs, for a segment). */
static inline uint32
lion_item_cardinality(const LionContainer *item)
{
	return item->cardinality;
}

extern bool lion_sparse_contains(const LionContainer *s, uint32 ckey, uint16 lo);

/* Is (ckey, lo) a member of this item?  The item must cover ckey. */
static inline bool
lion_item_contains(const LionContainer *item, uint32 ckey, uint16 lo)
{
	Assert(lion_item_covers(item, ckey));
	if (item->type == LION_CT_SPARSE)
		return lion_sparse_contains(item, ckey, lo);
	return lion_container_contains(item, lo);
}

/* Initialise an empty segment in buf.  ckey is a placeholder until the first
 * pair is inserted; an empty segment must never be stored. */
extern void lion_sparse_init(LionContainer *s, uint32 ckey);

/*
 * Range of pairs belonging to ckey: *first receives the index of the first
 * one (or of the insert position when there is none) and *count how many
 * there are.  Binary search.
 */
extern void lion_sparse_find(const LionContainer *s, uint32 ckey,
							uint32 *first, uint32 *count);

/* Number of members of ckey inside the segment. */
extern uint32 lion_sparse_count(const LionContainer *s, uint32 ckey);

/*
 * Insert one pair in sorted position.  Returns false and changes nothing if
 * the segment is already at LION_SPARSE_MAX_PAIRS pairs; returns true with
 * *dup set (and nothing changed) when the pair is already there.  dup may be
 * NULL.  Buffer capacity must be LION_CONTAINER_MAX_SIZE.
 */
extern bool lion_sparse_insert(LionContainer *s, uint32 ckey, uint16 lo,
							  bool *dup);

/* Remove one pair; returns true if it was there. */
extern bool lion_sparse_remove(LionContainer *s, uint32 ckey, uint16 lo);

/*
 * Remove every pair for which pred() returns true (VACUUM).  Returns the
 * number removed; the caller deletes the segment when cardinality reaches 0.
 * A segment can only shrink, so this never needs more room.
 */
typedef bool (*lion_pair_predicate) (uint32 ckey, uint16 lo, void *arg);
extern uint32 lion_sparse_remove_if(LionContainer *s, lion_pair_predicate pred,
								   void *arg);

/*
 * Move every member of ckey out of the segment into out, which must have
 * LION_CONTAINER_MAX_SIZE bytes and receives an ARRAY container (not
 * optimized: the caller may still add members to it).  Returns the number of
 * members moved, which is 0 when the segment does not cover ckey at all.
 */
extern uint32 lion_sparse_extract(LionContainer *s, uint32 ckey,
								 LionContainer *out);

/*
 * Split s in half at a ckey boundary, so that no ckey ends up in both parts.
 * Returns false (and leaves left/right undefined) when every pair of s has
 * the same ckey and no such boundary exists.
 */
extern bool lion_sparse_split_half(const LionContainer *s, LionContainer *left,
								  LionContainer *right);

/*
 * Split s around ckey: left receives the pairs below it, right the pairs
 * above it.  s must not contain ckey itself any more (extract it first).
 * Either part may come back empty; the caller drops empty parts.
 */
extern void lion_sparse_split_at(const LionContainer *s, uint32 ckey,
								LionContainer *left, LionContainer *right);

/*
 * Concatenate two adjacent segments (every ckey of a is below every ckey of
 * b) into out.  Returns false if they are not adjacent in that sense or if
 * the result would exceed LION_SPARSE_MAX_PAIRS.
 */
extern bool lion_sparse_merge(const LionContainer *a, const LionContainer *b,
							 LionContainer *out);

/* Iterate the pairs in (ckey, lo) order; the callback stops on false. */
typedef bool (*lion_pair_callback) (uint32 ckey, uint16 lo, void *arg);
extern void lion_sparse_iterate(const LionContainer *s, lion_pair_callback cb,
							   void *arg);

/*
 * Structural validation for lion_index_verify(): type, flags, pair count,
 * the header ckey, sortedness and uniqueness, and that the segment fits in
 * avail_bytes.  Returns false and sets *errmsg (a static string) on failure.
 */
extern bool lion_sparse_check(const LionContainer *s, Size avail_bytes,
							 const char **errmsg);

#endif							/* LION_SPARSE_H */
