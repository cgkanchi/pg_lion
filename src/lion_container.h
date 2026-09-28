/*-------------------------------------------------------------------------
 * lion_container.h
 *	  Roaring-style containers for pg_lion.  See DESIGN.md §3.
 *
 *	  This module depends only on c.h and port/pg_bitutils.h so that it can be
 *	  unit-tested outside the server (test/unit/container_test.c, built with
 *	  -DFRONTEND).  No palloc, no elog: every function is total over valid
 *	  inputs, and Assert()s its caller's side of the contract.
 *
 *	  DAMAGED INPUT.  A container read from disk is data.  Every function is
 *	  memory-safe for any payload behind a header of a valid type: nothing
 *	  here reads more than LION_CONTAINER_MAX_SIZE bytes from the start of a
 *	  container or writes past a buffer of the documented size, every lo value
 *	  handed to a caller - and every member of a set-algebra result - is
 *	  below LION_CONTAINER_RANGE, and a mutator never leaves a container
 *	  larger than LION_CONTAINER_MAX_SIZE, whatever the header claimed.
 *	  Nothing Assert()s what a container's bytes say, so an assert-enabled
 *	  build does not stop on a damaged page either.  What a damaged container
 *	  yields is unspecified (but deterministic); lion_container_check() is
 *	  what finds it.  A reader that hands in a container straight from a page
 *	  must still know that lion_container_size() of it lies inside the item,
 *	  as lion_inline_fetch() checks for an INLINE payload and
 *	  lion_page_item_fetch() for a page item - reading
 *	  LION_CONTAINER_MAX_SIZE bytes from the start of a short item could leave
 *	  the page.  lion_container.c, "untrusted containers".
 *
 *	  Mutators operate IN PLACE on a caller-supplied buffer that must have at
 *	  least LION_CONTAINER_MAX_SIZE bytes of capacity, because a mutation may
 *	  change the representation and therefore the size.  Read-only functions
 *	  accept a pointer directly into a page.
 *
 *	  GROWTH IN PLACE, the one exception, which the in-place insert
 *	  (lion_insert_container_inplace()) and its WAL redo (LION_OP_CONTAINER_ADD)
 *	  rely on to call lion_container_add() on a page item whose allotted
 *	  length is short of LION_CONTAINER_MAX_SIZE.  add() writes at most:
 *
 *		ARRAY with fewer than LION_ARRAY_MAX_CARD members	size + 2 bytes
 *		RUN with fewer than LION_RUN_MAX_NRUNS runs			size + 4 bytes
 *		BITSET												its 4104 bytes
 *
 *	  and in those cases never changes the representation.  An ARRAY at
 *	  LION_ARRAY_MAX_CARD members, or a RUN at LION_RUN_MAX_NRUNS runs that
 *	  needs a new one, converts (to a BITSET, and to an ARRAY or a BITSET),
 *	  which needs the full buffer; callers check the count first.  Nothing
 *	  else may be called on an item in place.
 *-------------------------------------------------------------------------
 */
#ifndef LION_CONTAINER_H
#define LION_CONTAINER_H

#include "c.h"
#include "lion_tid.h"

/*
 * Item types.  The first three are containers and belong to this module;
 * LION_CT_SPARSE is the sparse segment of DESIGN.md §13, which shares the
 * header below but has its own payload and its own module (lion_sparse.[ch]).
 * Every function here rejects it: use lion_item_size() when an item may be of
 * either kind.
 */
typedef enum LionContainerType
{
	LION_CT_ARRAY = 1,
	LION_CT_BITSET = 2,
	LION_CT_RUN = 3,
	LION_CT_SPARSE = 4			/* not a container; see lion_sparse.h */
} LionContainerType;

typedef struct LionContainer
{
	uint32		ckey;			/* container key: tid code >> LION_CONTAINER_BITS */
	uint16		cardinality;	/* number of members, 0 .. LION_CONTAINER_RANGE */
	uint8		type;			/* LionContainerType */
	uint8		flags;			/* reserved, must be 0 */
	/* payload follows; see LION_CONTAINER_PAYLOAD() */
} LionContainer;

typedef struct LionRun
{
	uint16		start;
	uint16		len_minus_1;	/* run covers start .. start + len_minus_1 */
} LionRun;

#define LION_CONTAINER_HDRSZ		((Size) sizeof(LionContainer))	/* 8 */
#define LION_BITSET_WORDS		(LION_CONTAINER_RANGE / 64)	/* 512 */
#define LION_BITSET_BYTES		(LION_BITSET_WORDS * sizeof(uint64))	/* 4096 */
#define LION_ARRAY_MAX_CARD		(LION_BITSET_BYTES / sizeof(uint16))	/* 2048 */
#define LION_RUN_MAX_NRUNS		((LION_BITSET_BYTES - sizeof(uint16)) / sizeof(LionRun))	/* 1023 */
#define LION_CONTAINER_MAX_SIZE	(LION_CONTAINER_HDRSZ + LION_BITSET_BYTES)	/* 4104 */

#define LION_CONTAINER_PAYLOAD(c)	((char *) (c) + LION_CONTAINER_HDRSZ)
#define LION_ARRAY_DATA(c)			((uint16 *) LION_CONTAINER_PAYLOAD(c))
#define LION_BITSET_DATA(c)			((uint64 *) LION_CONTAINER_PAYLOAD(c))
#define LION_RUN_NRUNS(c)			(*(uint16 *) LION_CONTAINER_PAYLOAD(c))
#define LION_RUN_DATA(c)				((LionRun *) (LION_CONTAINER_PAYLOAD(c) + sizeof(uint16)))

/* Size in bytes of the whole container (header + payload) for its current type. */
extern Size lion_container_size(const LionContainer *c);
/* Size a container of the given type/cardinality/nruns would occupy. */
extern Size lion_container_size_for(LionContainerType type, uint32 cardinality, uint32 nruns);

/* Initialise an empty ARRAY container for ckey in buf. */
extern void lion_container_init(LionContainer *c, uint32 ckey);

extern bool lion_container_contains(const LionContainer *c, uint16 lo);
extern uint32 lion_container_cardinality(const LionContainer *c);	/* == c->cardinality */

/*
 * Mutators (buffer capacity LION_CONTAINER_MAX_SIZE).  add/remove return true
 * if the membership actually changed.  They enforce the size invariant
 * (DESIGN.md §3) but do not search for the smallest representation.
 */
extern bool lion_container_add(LionContainer *c, uint16 lo);
extern bool lion_container_remove(LionContainer *c, uint16 lo);

/*
 * Bulk builder: append lo values in strictly ascending order to a container
 * that started from lion_container_init().  Cheaper than repeated add().
 * The caller calls lion_container_optimize() once at the end.  The values
 * may come off a page (a sparse segment's pairs), so the order is checked,
 * not trusted: one that is out of order or repeated is add()ed instead, and
 * one past the range is masked into it.
 */
extern void lion_container_append_sorted(LionContainer *c, uint16 lo);

/* Convert to the smallest representation among ARRAY/BITSET/RUN. */
extern void lion_container_optimize(LionContainer *c);
/* Force BITSET representation (used when a RUN/ARRAY would exceed its limits). */
extern void lion_container_to_bitset(LionContainer *c);

/* Iteration in ascending lo order; callback returns false to stop early. */
typedef bool (*lion_lo_callback) (uint16 lo, void *arg);
extern void lion_container_iterate(const LionContainer *c, lion_lo_callback cb, void *arg);
/*
 * Materialise all members, in iterate() order - ascending for a well-formed
 * container.  out must hold LION_CONTAINER_RANGE uint16s, which is as many as
 * any container yields, damaged or not.  Returns the count.
 */
extern uint32 lion_container_to_array(const LionContainer *c, uint16 *out);

/*
 * Remove every member for which pred() returns true (VACUUM).  Returns the
 * number removed.  Enforces the size invariant; the caller should then call
 * lion_container_optimize() and delete the container if cardinality == 0.
 */
typedef bool (*lion_lo_predicate) (uint16 lo, void *arg);
extern uint32 lion_container_remove_if(LionContainer *c, lion_lo_predicate pred, void *arg);

/*
 * Set algebra between two containers with the same ckey.  dest has capacity
 * LION_CONTAINER_MAX_SIZE and receives an optimized result with dest->ckey set.
 * Each returns the result cardinality.  a and b may point into pages.
 */
extern uint32 lion_container_and(const LionContainer *a, const LionContainer *b, LionContainer *dest);
extern uint32 lion_container_or(const LionContainer *a, const LionContainer *b, LionContainer *dest);
extern uint32 lion_container_andnot(const LionContainer *a, const LionContainer *b, LionContainer *dest);
extern uint32 lion_container_and_cardinality(const LionContainer *a, const LionContainer *b);

/*
 * The same algebra, UNOPTIMIZED: for a caller that counts the result or hands
 * it on to the next operation and never stores it (the count engine's merge,
 * DESIGN.md §15, "Unions and intersections unoptimized").  The result is left
 * in the representation the operation builds it in -
 *
 *	and		an ARRAY when either operand is one, a RUN when both are RUNs and
 *			the runs fit, else a BITSET;
 *	andnot	an ARRAY when a is one, else a BITSET;
 *	or		an ARRAY when both are ARRAYs whose members fit one, else a BITSET
 *
 * - and may be larger than lion_container_optimize() would make it (a BITSET
 * of a few members), but it holds exactly the members the optimizing form's
 * result holds, with the cardinality set, and every function here takes it.
 * A caller that keeps one optimizes it first.  dest (capacity
 * LION_CONTAINER_MAX_SIZE) must not overlap a or b: it is written in place.
 */
extern uint32 lion_container_and_raw(const LionContainer *a, const LionContainer *b,
									 LionContainer *dest);
extern uint32 lion_container_andnot_raw(const LionContainer *a, const LionContainer *b,
										LionContainer *dest);
extern uint32 lion_container_or_raw(const LionContainer *a, const LionContainer *b,
									LionContainer *dest);

/*
 * dest = a AND (b[0] OR ... OR b[nb - 1]), without building the union: each
 * of a's members is looked up in b[0], the ones not found in b[1], and so on
 * (the count engine's AND of a running intersection with an IN list's or a
 * multi-key `&&`'s containers at one key, DESIGN.md §29.11, "Unions
 * probed").  a holds at most LION_ARRAY_MAX_CARD members, which the caller
 * checks by its cardinality; the result is an ARRAY of those found, not
 * optimized, with a's ckey, and its cardinality is returned.  Every b has
 * a's ckey; dest (capacity LION_CONTAINER_MAX_SIZE) overlaps none of them.
 * a and the b's may come off pages.
 */
extern uint32 lion_container_and_union_raw(const LionContainer *a,
										   const LionContainer *const *b,
										   uint32 nb, LionContainer *dest);

/*
 * A BITSET a caller ORs containers into (the count engine's union of several
 * containers of one key): bitset_init() makes dest (capacity
 * LION_CONTAINER_MAX_SIZE) an empty BITSET of ckey, whose words
 * lion_container_or_into_bitset(c, LION_BITSET_DATA(dest)) sets, and
 * bitset_recount() sets its cardinality from them, which it returns.  The
 * result is not optimized either.
 */
extern void lion_container_bitset_init(LionContainer *dest, uint32 ckey);
extern uint32 lion_container_bitset_recount(LionContainer *c);

/*
 * The members of an ARRAY c that are set in the image w (LION_BITSET_WORDS
 * words): how many, returned, and in *blocks the heap blocks they lie on, in
 * lion_container_block_mask()'s numbering - the cardinality and block mask of
 * c AND w, without building it (the count engine's grouped walk, DESIGN.md
 * §10, "The groups of a walk, counted together").  c may come off a page.
 */
extern uint32 lion_container_and_image_count(const LionContainer *c,
											 const uint64 *w, uint64 *blocks);

/*
 * Range helpers used by the phase-2 visibility-map mask: number of members
 * with lo in [lo_start, lo_end] inclusive, and removal of that whole range.
 */
extern uint32 lion_container_range_cardinality(const LionContainer *c, uint16 lo_start, uint16 lo_end);
extern uint32 lion_container_remove_range(LionContainer *c, uint16 lo_start, uint16 lo_end);

/*
 * w |= c, for a caller that accumulates the union of several containers of
 * one container key in a bitset image of LION_BITSET_WORDS words (the count
 * engine's OR of k containers, DESIGN.md §15).  w must not overlap c.
 */
extern void lion_container_or_into_bitset(const LionContainer *c, uint64 *w);

/*
 * Dense accumulation of a union that grows a few members at a time (a
 * range's sets, collected or probed: DESIGN.md §32, "Summed ranges: dense
 * and probed").
 *
 * lion_container_or_inplace(): acc |= c for an acc that is a BITSET with its
 * full LION_CONTAINER_MAX_SIZE bytes, in place and without optimizing it;
 * acc's cardinality is kept, and the members that were new are returned.
 *
 * lion_container_add_many(): acc ∪= the n values of vals - any order,
 * repeats allowed - through the caller's image w of LION_BITSET_WORDS words
 * (not overlapping acc); acc (capacity LION_CONTAINER_MAX_SIZE) comes back
 * optimized, and its cardinality is returned.
 *
 * lion_container_mark_members(): for every j < n with sorted[j] a member of
 * c, set bit j of marks ((n + 63) / 64 words); sorted is strictly ascending,
 * below LION_CONTAINER_RANGE.  Returns how many bits were newly set.
 *
 * c may come off a page in all three, and is taken as iterate() takes it.
 */
extern uint32 lion_container_or_inplace(LionContainer *acc, const LionContainer *c);
extern uint32 lion_container_add_many(LionContainer *acc, const uint16 *vals,
									  uint32 n, uint64 *w);
extern uint32 lion_container_mark_members(const LionContainer *c,
										  const uint16 *sorted, uint32 n,
										  uint64 *marks);

/*
 * Which of the LION_BLOCKS_PER_CONTAINER heap blocks the container covers
 * hold a member: bit b for the block of lo values b << LION_OFFSET_BITS ..
 * ((b + 1) << LION_OFFSET_BITS) - 1, which is the count engine's
 * visibility-map mask (lion_count_container_vm()).  For any payload, damaged
 * or not, the bit of every block that iterate() hands out a member of is
 * set, and no bit at or above LION_BLOCKS_PER_CONTAINER is.
 */
extern uint64 lion_container_block_mask(const LionContainer *c);

/*
 * Structural validation for lion_index_verify(): checks type, cardinality
 * consistency, ordering, run adjacency, and that the container fits in
 * avail_bytes.  Returns false and sets *errmsg (static string) on failure.
 */
extern bool lion_container_check(const LionContainer *c, Size avail_bytes, const char **errmsg);

#endif							/* LION_CONTAINER_H */
