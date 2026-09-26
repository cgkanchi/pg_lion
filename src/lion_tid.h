/*-------------------------------------------------------------------------
 * lion_tid.h
 *	  Mapping between heap TIDs and roaring container coordinates.
 *	  See DESIGN.md §2.  This header is frozen; do not change constants.
 *-------------------------------------------------------------------------
 */
#ifndef LION_TID_H
#define LION_TID_H

#include "c.h"
#ifndef FRONTEND
#include "access/htup_details.h"	/* MaxHeapTuplesPerPage */
#include "storage/itemptr.h"
#endif

/*
 * Bits needed for an offset number on this build's page size (see DESIGN.md).
 * MaxHeapTuplesPerPage cannot be tested with #if: it expands to a cast
 * expression, which is not a constant expression for the preprocessor.  Switch
 * on BLCKSZ instead (9 at 8K, 10 at 16K, 11 at 32K) and verify the real bound
 * with a static assertion.
 *
 * Below 8K the encoding would work, but a 4104-byte BITSET container is not
 * an item any page of that size can hold (DESIGN.md §2; lion.h asserts the
 * page capacity itself), so such a build is refused here, where every file
 * sees it first.
 */
#if BLCKSZ < 8192
#error "pg_lion: BLCKSZ below 8192 is not supported (a BITSET container does not fit on a page)"
#elif BLCKSZ == 8192
#define LION_OFFSET_BITS 9
#elif BLCKSZ <= 16384
#define LION_OFFSET_BITS 10
#elif BLCKSZ <= 32768
#define LION_OFFSET_BITS 11
#else
#error "pg_lion: unsupported BLCKSZ (MaxHeapTuplesPerPage too large)"
#endif

#ifndef FRONTEND
StaticAssertDecl(MaxHeapTuplesPerPage < (1 << LION_OFFSET_BITS),
				 "pg_lion: LION_OFFSET_BITS too small for MaxHeapTuplesPerPage");
#endif

#define LION_CONTAINER_BITS		15
#define LION_CONTAINER_RANGE		(1 << LION_CONTAINER_BITS)	/* 32768 lo values */
#define LION_LO_MASK				((uint64) (LION_CONTAINER_RANGE - 1))
#define LION_MAX_OFFSET			((OffsetNumber) ((1 << LION_OFFSET_BITS) - 1))
#define LION_BLOCKS_PER_CONTAINER (1 << (LION_CONTAINER_BITS - LION_OFFSET_BITS))	/* 64 at 8K */

#ifndef FRONTEND
/* Callers must check ItemPointerGetOffsetNumber(tid) <= LION_MAX_OFFSET first. */
static inline uint64
lion_tid_to_code(ItemPointer tid)
{
	return (((uint64) ItemPointerGetBlockNumber(tid)) << LION_OFFSET_BITS) |
		(uint64) ItemPointerGetOffsetNumber(tid);
}

static inline void
lion_code_to_tid(uint64 code, ItemPointer tid)
{
	ItemPointerSet(tid,
				   (BlockNumber) (code >> LION_OFFSET_BITS),
				   (OffsetNumber) (code & ((1 << LION_OFFSET_BITS) - 1)));
}
#endif							/* !FRONTEND */

static inline uint32
lion_code_ckey(uint64 code)
{
	return (uint32) (code >> LION_CONTAINER_BITS);
}

static inline uint16
lion_code_lo(uint64 code)
{
	return (uint16) (code & LION_LO_MASK);
}

/* Reassemble a code from container coordinates. */
static inline uint64
lion_make_code(uint32 ckey, uint16 lo)
{
	return (((uint64) ckey) << LION_CONTAINER_BITS) | (uint64) lo;
}

#ifndef FRONTEND
/* First heap block covered by a container. */
static inline BlockNumber
lion_ckey_first_block(uint32 ckey)
{
	return (BlockNumber) (((uint64) ckey) << (LION_CONTAINER_BITS - LION_OFFSET_BITS));
}

/* Split a lo value into (block within container, offset). */
static inline void
lion_lo_split(uint16 lo, uint16 *blk_in_container, OffsetNumber *off)
{
	*blk_in_container = (uint16) (lo >> LION_OFFSET_BITS);
	*off = (OffsetNumber) (lo & ((1 << LION_OFFSET_BITS) - 1));
}
#endif							/* !FRONTEND */

#endif							/* LION_TID_H */
