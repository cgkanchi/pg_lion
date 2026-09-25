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
 *	  handed to a caller is below LION_CONTAINER_RANGE, and a mutator never
 *	  leaves a container larger than LION_CONTAINER_MAX_SIZE, whatever the
 *	  header claimed.  What a damaged container yields is unspecified (but
 *	  deterministic); lion_container_check() is what finds it.  A reader that
 *	  hands in a container straight from a page must still know that
 *	  lion_container_size() of it lies inside the item, as lion_inline_fetch()
 *	  checks - reading LION_CONTAINER_MAX_SIZE bytes from the start of a short
 *	  item could leave the page.  lion_container.c, "untrusted containers".
 *
 *	  Mutators operate IN PLACE on a caller-supplied buffer that must have at
 *	  least LION_CONTAINER_MAX_SIZE bytes of capacity, because a mutation may
 *	  change the representation and therefore the size.  Read-only functions
 *	  accept a pointer directly into a page.
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
 * The caller calls lion_container_optimize() once at the end.
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
 * Range helpers used by the phase-2 visibility-map mask: number of members
 * with lo in [lo_start, lo_end] inclusive, and removal of that whole range.
 */
extern uint32 lion_container_range_cardinality(const LionContainer *c, uint16 lo_start, uint16 lo_end);
extern uint32 lion_container_remove_range(LionContainer *c, uint16 lo_start, uint16 lo_end);

/*
 * Structural validation for lion_index_verify(): checks type, cardinality
 * consistency, ordering, run adjacency, and that the container fits in
 * avail_bytes.  Returns false and sets *errmsg (static string) on failure.
 */
extern bool lion_container_check(const LionContainer *c, Size avail_bytes, const char **errmsg);

#endif							/* LION_CONTAINER_H */
