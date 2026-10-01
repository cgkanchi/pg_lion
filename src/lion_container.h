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
 *		NARROW, a member at an offset below 128				its 1032 bytes
 *
 *	  and in those cases never changes the representation.  An ARRAY at
 *	  LION_ARRAY_MAX_CARD members, a RUN at LION_RUN_MAX_NRUNS runs that
 *	  needs a new one, or a NARROW given a member at an offset it has no bit
 *	  for converts (to a BITSET, to an ARRAY or a BITSET, and to a BITSET),
 *	  which needs the full buffer.  lion_container_inplace_need() is that
 *	  table, and both callers ask it before they call add() on an item.
 *	  Nothing else may be called on an item in place.
 *-------------------------------------------------------------------------
 */
#ifndef LION_CONTAINER_H
#define LION_CONTAINER_H

#include "c.h"
#include "lion_tid.h"

/*
 * Item types.  ARRAY, BITSET, RUN and NARROW are containers and belong to
 * this module; LION_CT_SPARSE is the sparse segment of DESIGN.md §13, which
 * shares the header below but has its own payload and its own module
 * (lion_sparse.[ch]).  Every function here rejects it: use lion_item_size()
 * when an item may be of either kind.  NARROW (DESIGN.md §38) came after the
 * segment, which is why it is 5: the numbers are on disk.
 */
typedef enum LionContainerType
{
	LION_CT_ARRAY = 1,
	LION_CT_BITSET = 2,
	LION_CT_RUN = 3,
	LION_CT_SPARSE = 4,			/* not a container; see lion_sparse.h */
	LION_CT_NARROW = 5			/* DESIGN.md §38 */
} LionContainerType;

/* Is type one of the four container kinds? */
static inline bool
lion_container_type_valid(uint32 type)
{
	return type == LION_CT_ARRAY || type == LION_CT_BITSET ||
		type == LION_CT_RUN || type == LION_CT_NARROW;
}

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

/*
 * NARROW (DESIGN.md §38): a bitset of the offsets below LION_NARROW_OFFSETS
 * of each of the container's LION_BLOCKS_PER_CONTAINER heap blocks, for a
 * set whose members all have such offsets - every set of a table with fewer
 * than 128 rows a page.  Member lo is bit
 *
 *		(lo >> LION_OFFSET_BITS) << LION_NARROW_OFFSET_BITS | (lo & 127)
 *
 * so that each block is LION_NARROW_WORDS_PER_BLOCK whole words, the first
 * of the block's words in a BITSET: 1024 payload bytes at 8K against a
 * BITSET's 4096.  Its words are NOT a BITSET's, and nothing outside this
 * module may read them as one; lion_container_or_into_bitset() is the way
 * to an image of a container of any kind.
 */
#define LION_NARROW_OFFSET_BITS	7
#define LION_NARROW_OFFSETS		(1 << LION_NARROW_OFFSET_BITS)	/* 128 */
#define LION_NARROW_WORDS_PER_BLOCK	(LION_NARROW_OFFSETS / 64)	/* 2 */
#define LION_NARROW_WORDS		(LION_BLOCKS_PER_CONTAINER * LION_NARROW_WORDS_PER_BLOCK)	/* 128 at 8K */
#define LION_NARROW_BYTES		(LION_NARROW_WORDS * sizeof(uint64))	/* 1024 at 8K */
#define LION_NARROW_SIZE		(LION_CONTAINER_HDRSZ + LION_NARROW_BYTES)	/* 1032 at 8K */
/* An ARRAY of this many members is as large as a NARROW: 512 at 8K. */
#define LION_NARROW_ARRAY_CARD	(LION_NARROW_BYTES / sizeof(uint16))

/* Can a NARROW hold lo: is its offset below LION_NARROW_OFFSETS? */
static inline bool
lion_lo_is_narrow(uint32 lo)
{
	return (lo & ((1U << LION_OFFSET_BITS) - 1)) < LION_NARROW_OFFSETS;
}

/* Size in bytes of the whole container (header + payload) for its current type. */
extern Size lion_container_size(const LionContainer *c);
/* Size a container of the given type/cardinality/nruns would occupy. */
extern Size lion_container_size_for(LionContainerType type, uint32 cardinality, uint32 nruns);

/* Initialise an empty ARRAY container for ckey in buf. */
extern void lion_container_init(LionContainer *c, uint32 ckey);

/*
 * GROWTH IN PLACE (above): the bytes from the start of c that
 * lion_container_add(c, lo) may write when it keeps c's representation, or 0
 * when it would change it - an ARRAY at LION_ARRAY_MAX_CARD members, a RUN at
 * LION_RUN_MAX_NRUNS runs, a NARROW and a member at an offset it has no bit
 * for - or c is no container.  The in-place insert and its redo call add()
 * on a page item only when this is not 0 and the item's allotted length is
 * at least this.  It reads c's header (and a RUN's run count) only.
 */
extern Size lion_container_inplace_need(const LionContainer *c, uint16 lo);

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

/*
 * Convert to the smallest representation: ARRAY, RUN, NARROW or BITSET, a
 * tie going to the first of them (DESIGN.md §3, §38).  NARROW is a candidate
 * only when every member's offset is below LION_NARROW_OFFSETS.
 * lion_container_optimize_ext() with allow_narrow false leaves NARROW out,
 * which is the choice for an index whose format predates it (DESIGN.md §38,
 * "Format"): what it makes is what optimize() made before §38, and a NARROW
 * handed to it comes out something else.  The result is a function of the
 * members and of allow_narrow alone.
 */
extern void lion_container_optimize(LionContainer *c);
extern void lion_container_optimize_ext(LionContainer *c, bool allow_narrow);
/* Force BITSET representation (used when a RUN/ARRAY would exceed its limits). */
extern void lion_container_to_bitset(LionContainer *c);
/* Could c be a NARROW: is every member's offset below LION_NARROW_OFFSETS? */
extern bool lion_container_narrow_feasible(const LionContainer *c);

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
 *			the runs fit, a NARROW when either operand is one, else a BITSET;
 *	andnot	an ARRAY when a is one, a NARROW when a is one, else a BITSET;
 *	or		an ARRAY when both are ARRAYs whose members fit one, a NARROW
 *			when both are NARROWs, else a BITSET
 *
 * (an empty operand aside: and is then an empty ARRAY, and or and andnot a
 * copy of the operand that is not empty, or of a)
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
 * The three steps of that probe, for the count engine's evaluation of a
 * nested tree - an AND of ORs, an OR of ANDs - for a few members (DESIGN.md
 * §29.11, "Trees probed"):
 *
 * - lion_container_extract_members(): c's members, ascending, as iterate()
 *   hands them out, into out (room for cap); the count, or cap + 1 when
 *   there are more;
 * - lion_container_probe_members(): of the positions pending[0 .. np - 1]
 *   into the ascending vals[], mark in found[] (a bit a position) those whose
 *   value b holds, keep in pending[], in order, those it does not, and
 *   return how many are kept;
 * - lion_container_array_from_marks(): dest (capacity
 *   LION_CONTAINER_MAX_SIZE) becomes the ARRAY of ckey of vals[p] for each
 *   position p < n (at most LION_ARRAY_MAX_CARD) marked, masked and
 *   ascending whatever vals[] holds; its cardinality is returned.
 *
 * Containers may come off pages; no index leaves the arrays given.
 */
extern uint32 lion_container_extract_members(const LionContainer *c,
											 uint16 *out, uint32 cap);
extern uint32 lion_container_probe_members(const LionContainer *b,
										   const uint16 *vals, uint16 *pending,
										   uint32 np, uint64 *found);
extern uint32 lion_container_array_from_marks(LionContainer *dest, uint32 ckey,
											  const uint16 *vals, uint32 n,
											  const uint64 *marks);

/*
 * A BITSET a caller ORs containers into (the count engine's union of several
 * containers of one key): bitset_init() makes dest (capacity
 * LION_CONTAINER_MAX_SIZE) an empty BITSET of ckey, whose words
 * lion_container_or_into_bitset(c, LION_BITSET_DATA(dest)) sets, and
 * bitset_recount() sets its cardinality from them, which it returns.  The
 * result is not optimized either.  Both are for a BITSET only: a NARROW's
 * words are not a BITSET's (LION_NARROW_OFFSET_BITS above).
 */
extern void lion_container_bitset_init(LionContainer *dest, uint32 ckey);
extern uint32 lion_container_bitset_recount(LionContainer *c);

/*
 * The number of bits set in an image of LION_BITSET_WORDS words, which a
 * caller that builds a container key's members in an image of its own
 * (lion_bits_to_container()) needs: the library's popcount of a whole
 * bitset, AVX2 or POPCNT where the CPU has them (lion_container.c,
 * "whole-bitset kernels").
 */
extern uint32 lion_container_image_cardinality(const uint64 *w);

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
 * engine's OR of k containers, DESIGN.md §15).  w must not overlap c.  A
 * container of any kind: a NARROW's words go where a BITSET has them.
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

/*
 * HEAP TUPLE OFFSETS (DESIGN.md §2).  A member is the low LION_CONTAINER_BITS
 * of a heap TID's code - the block within the container's range, above
 * LION_OFFSET_BITS of offset number - so it names a tuple only when that
 * offset is one a heap page can have, 1 .. maxoff, where maxoff is the
 * server's MaxHeapTuplesPerPage: this module is frontend code and takes it
 * from the caller.  Structurally any lo is a member, which is why
 * lion_container_check() does not ask, and nothing but lion_index_verify()
 * does: a member at offset 0 passed it, and every query that turned the key
 * into TIDs then failed with "tuple offset out of range: 0" (2026-09-29
 * review).
 */
static inline bool
lion_lo_is_tuple(uint32 lo, uint32 maxoff)
{
	uint32		off = lo & ((1U << LION_OFFSET_BITS) - 1);

	return off >= 1 && off <= maxoff;
}

/*
 * For lion_index_verify(), of a container lion_container_check() has passed:
 * every member is a tuple (lion_lo_is_tuple()), which for a RUN means each
 * run lies inside the offsets 1 .. maxoff of one heap block.  maxoff has to
 * be below 1 << LION_OFFSET_BITS.  Returns false and sets *errmsg (a static
 * string) on failure.
 */
extern bool lion_container_check_offsets(const LionContainer *c,
										 uint32 maxoff, const char **errmsg);

/*
 * The passes of the library's whole-bitset kernels (lion_container.c,
 * "whole-bitset kernels"): what a pass over all LION_BITSET_WORDS words of a
 * (and b) writes to d, and what it counts.  Internal to the library, but for
 * lion_container_bits_pass() below.
 */
typedef enum LionBitsOp
{
	LION_BITS_COUNT,			/* |a| */
	LION_BITS_RUNS,				/* the runs of set bits in a */
	LION_BITS_AND_COUNT,		/* |a & b|, nothing written */
	LION_BITS_AND,				/* d = a & b, |d| */
	LION_BITS_OR,				/* d = a | b, |d| */
	LION_BITS_ANDNOT,			/* d = a & ~b, |d| */
	LION_BITS_OR_NEW			/* d = a | b, |b & ~a| */
} LionBitsOp;

#ifdef FRONTEND
/*
 * For the unit tests: which implementation of the whole-bitset kernels
 * (lion_container.c) the library runs.  The server always runs the best its
 * CPU has, picked at the first call, which LION_SIMD_AUTO restores.  force()
 * returns false, and changes nothing, for one this build or this CPU does
 * not have: the x86-64 ones outside x86-64 GCC and clang, or under
 * LION_NO_SIMD.  Every implementation gives the same results.
 */
typedef enum LionSimdImpl
{
	LION_SIMD_AUTO = 0,			/* the best this CPU runs */
	LION_SIMD_PORTABLE,			/* plain C */
	LION_SIMD_POPCNT,			/* x86-64: a POPCNT a word */
	LION_SIMD_AVX2				/* x86-64: AVX2 */
} LionSimdImpl;

extern bool lion_container_simd_force(LionSimdImpl impl);
extern LionSimdImpl lion_container_simd_current(void);

/*
 * One pass of the implementation in use, which the set algebra makes on
 * BITSET payloads: its count returned, and d written for the ops that write
 * (d may be a or b; NULL for the ones that do not).  narrow_pass() is the
 * same pass over LION_NARROW_WORDS words, which the set algebra makes on
 * NARROW payloads; its LION_BITS_RUNS counts the runs of the words as they
 * lie, across the blocks' boundaries, which the library corrects for.
 */
extern uint32 lion_container_bits_pass(LionBitsOp op, uint64 *d,
									   const uint64 *a, const uint64 *b);
extern uint32 lion_container_narrow_pass(LionBitsOp op, uint64 *d,
										 const uint64 *a, const uint64 *b);
#endif

#endif							/* LION_CONTAINER_H */
