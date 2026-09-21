/*-------------------------------------------------------------------------
 * rbi_container.h
 *	  Roaring-style containers for roaring_index.  See DESIGN.md §3.
 *
 *	  This module depends only on c.h and port/pg_bitutils.h so that it can be
 *	  unit-tested outside the server (test/unit/container_test.c, built with
 *	  -DFRONTEND).  No palloc, no elog: every function is total over valid
 *	  inputs and reports impossible states through Assert().
 *
 *	  Mutators operate IN PLACE on a caller-supplied buffer that must have at
 *	  least RBI_CONTAINER_MAX_SIZE bytes of capacity, because a mutation may
 *	  change the representation and therefore the size.  Read-only functions
 *	  accept a pointer directly into a page.
 *-------------------------------------------------------------------------
 */
#ifndef RBI_CONTAINER_H
#define RBI_CONTAINER_H

#include "c.h"
#include "rbi_tid.h"

/*
 * Item types.  The first three are containers and belong to this module;
 * RBI_CT_SPARSE is the sparse segment of DESIGN.md §13, which shares the
 * header below but has its own payload and its own module (rbi_sparse.[ch]).
 * Every function here rejects it: use rbi_item_size() when an item may be of
 * either kind.
 */
typedef enum RBIContainerType
{
	RBI_CT_ARRAY = 1,
	RBI_CT_BITSET = 2,
	RBI_CT_RUN = 3,
	RBI_CT_SPARSE = 4			/* not a container; see rbi_sparse.h */
} RBIContainerType;

typedef struct RBIContainer
{
	uint32		ckey;			/* container key: tid code >> RBI_CONTAINER_BITS */
	uint16		cardinality;	/* number of members, 0 .. RBI_CONTAINER_RANGE */
	uint8		type;			/* RBIContainerType */
	uint8		flags;			/* reserved, must be 0 */
	/* payload follows; see RBI_CONTAINER_PAYLOAD() */
} RBIContainer;

typedef struct RBIRun
{
	uint16		start;
	uint16		len_minus_1;	/* run covers start .. start + len_minus_1 */
} RBIRun;

#define RBI_CONTAINER_HDRSZ		((Size) sizeof(RBIContainer))	/* 8 */
#define RBI_BITSET_WORDS		(RBI_CONTAINER_RANGE / 64)	/* 512 */
#define RBI_BITSET_BYTES		(RBI_BITSET_WORDS * sizeof(uint64))	/* 4096 */
#define RBI_ARRAY_MAX_CARD		(RBI_BITSET_BYTES / sizeof(uint16))	/* 2048 */
#define RBI_RUN_MAX_NRUNS		((RBI_BITSET_BYTES - sizeof(uint16)) / sizeof(RBIRun))	/* 1023 */
#define RBI_CONTAINER_MAX_SIZE	(RBI_CONTAINER_HDRSZ + RBI_BITSET_BYTES)	/* 4104 */

#define RBI_CONTAINER_PAYLOAD(c)	((char *) (c) + RBI_CONTAINER_HDRSZ)
#define RBI_ARRAY_DATA(c)			((uint16 *) RBI_CONTAINER_PAYLOAD(c))
#define RBI_BITSET_DATA(c)			((uint64 *) RBI_CONTAINER_PAYLOAD(c))
#define RBI_RUN_NRUNS(c)			(*(uint16 *) RBI_CONTAINER_PAYLOAD(c))
#define RBI_RUN_DATA(c)				((RBIRun *) (RBI_CONTAINER_PAYLOAD(c) + sizeof(uint16)))

/* Size in bytes of the whole container (header + payload) for its current type. */
extern Size rbi_container_size(const RBIContainer *c);
/* Size a container of the given type/cardinality/nruns would occupy. */
extern Size rbi_container_size_for(RBIContainerType type, uint32 cardinality, uint32 nruns);

/* Initialise an empty ARRAY container for ckey in buf. */
extern void rbi_container_init(RBIContainer *c, uint32 ckey);

extern bool rbi_container_contains(const RBIContainer *c, uint16 lo);
extern uint32 rbi_container_cardinality(const RBIContainer *c);	/* == c->cardinality */

/*
 * Mutators (buffer capacity RBI_CONTAINER_MAX_SIZE).  add/remove return true
 * if the membership actually changed.  They enforce the size invariant
 * (DESIGN.md §3) but do not search for the smallest representation.
 */
extern bool rbi_container_add(RBIContainer *c, uint16 lo);
extern bool rbi_container_remove(RBIContainer *c, uint16 lo);

/*
 * Bulk builder: append lo values in strictly ascending order to a container
 * that started from rbi_container_init().  Cheaper than repeated add().
 * The caller calls rbi_container_optimize() once at the end.
 */
extern void rbi_container_append_sorted(RBIContainer *c, uint16 lo);

/* Convert to the smallest representation among ARRAY/BITSET/RUN. */
extern void rbi_container_optimize(RBIContainer *c);
/* Force BITSET representation (used when a RUN/ARRAY would exceed its limits). */
extern void rbi_container_to_bitset(RBIContainer *c);

/* Iteration in ascending lo order; callback returns false to stop early. */
typedef bool (*rbi_lo_callback) (uint16 lo, void *arg);
extern void rbi_container_iterate(const RBIContainer *c, rbi_lo_callback cb, void *arg);
/* Materialise all members (out must hold RBI_CONTAINER_RANGE uint16s); returns count. */
extern uint32 rbi_container_to_array(const RBIContainer *c, uint16 *out);

/*
 * Remove every member for which pred() returns true (VACUUM).  Returns the
 * number removed.  Enforces the size invariant; the caller should then call
 * rbi_container_optimize() and delete the container if cardinality == 0.
 */
typedef bool (*rbi_lo_predicate) (uint16 lo, void *arg);
extern uint32 rbi_container_remove_if(RBIContainer *c, rbi_lo_predicate pred, void *arg);

/*
 * Set algebra between two containers with the same ckey.  dest has capacity
 * RBI_CONTAINER_MAX_SIZE and receives an optimized result with dest->ckey set.
 * Each returns the result cardinality.  a and b may point into pages.
 */
extern uint32 rbi_container_and(const RBIContainer *a, const RBIContainer *b, RBIContainer *dest);
extern uint32 rbi_container_or(const RBIContainer *a, const RBIContainer *b, RBIContainer *dest);
extern uint32 rbi_container_andnot(const RBIContainer *a, const RBIContainer *b, RBIContainer *dest);
extern uint32 rbi_container_and_cardinality(const RBIContainer *a, const RBIContainer *b);

/*
 * Range helpers used by the phase-2 visibility-map mask: number of members
 * with lo in [lo_start, lo_end] inclusive, and removal of that whole range.
 */
extern uint32 rbi_container_range_cardinality(const RBIContainer *c, uint16 lo_start, uint16 lo_end);
extern uint32 rbi_container_remove_range(RBIContainer *c, uint16 lo_start, uint16 lo_end);

/*
 * Structural validation for roaring_index_verify(): checks type, cardinality
 * consistency, ordering, run adjacency, and that the container fits in
 * avail_bytes.  Returns false and sets *errmsg (static string) on failure.
 */
extern bool rbi_container_check(const RBIContainer *c, Size avail_bytes, const char **errmsg);

#endif							/* RBI_CONTAINER_H */
