/*-------------------------------------------------------------------------
 * rbi_tid.h
 *	  Mapping between heap TIDs and roaring container coordinates.
 *	  See DESIGN.md §2.  This header is frozen; do not change constants.
 *-------------------------------------------------------------------------
 */
#ifndef RBI_TID_H
#define RBI_TID_H

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
 */
#if BLCKSZ <= 8192
#define RBI_OFFSET_BITS 9
#elif BLCKSZ <= 16384
#define RBI_OFFSET_BITS 10
#elif BLCKSZ <= 32768
#define RBI_OFFSET_BITS 11
#else
#error "roaring_index: unsupported BLCKSZ (MaxHeapTuplesPerPage too large)"
#endif

#ifndef FRONTEND
StaticAssertDecl(MaxHeapTuplesPerPage < (1 << RBI_OFFSET_BITS),
				 "roaring_index: RBI_OFFSET_BITS too small for MaxHeapTuplesPerPage");
#endif

#define RBI_CONTAINER_BITS		15
#define RBI_CONTAINER_RANGE		(1 << RBI_CONTAINER_BITS)	/* 32768 lo values */
#define RBI_LO_MASK				((uint64) (RBI_CONTAINER_RANGE - 1))
#define RBI_MAX_OFFSET			((OffsetNumber) ((1 << RBI_OFFSET_BITS) - 1))
#define RBI_BLOCKS_PER_CONTAINER (1 << (RBI_CONTAINER_BITS - RBI_OFFSET_BITS))	/* 64 at 8K */

#ifndef FRONTEND
/* Callers must check ItemPointerGetOffsetNumber(tid) <= RBI_MAX_OFFSET first. */
static inline uint64
rbi_tid_to_code(ItemPointer tid)
{
	return (((uint64) ItemPointerGetBlockNumber(tid)) << RBI_OFFSET_BITS) |
		(uint64) ItemPointerGetOffsetNumber(tid);
}

static inline void
rbi_code_to_tid(uint64 code, ItemPointer tid)
{
	ItemPointerSet(tid,
				   (BlockNumber) (code >> RBI_OFFSET_BITS),
				   (OffsetNumber) (code & ((1 << RBI_OFFSET_BITS) - 1)));
}
#endif							/* !FRONTEND */

static inline uint32
rbi_code_ckey(uint64 code)
{
	return (uint32) (code >> RBI_CONTAINER_BITS);
}

static inline uint16
rbi_code_lo(uint64 code)
{
	return (uint16) (code & RBI_LO_MASK);
}

/* Reassemble a code from container coordinates. */
static inline uint64
rbi_make_code(uint32 ckey, uint16 lo)
{
	return (((uint64) ckey) << RBI_CONTAINER_BITS) | (uint64) lo;
}

#ifndef FRONTEND
/* First heap block covered by a container. */
static inline BlockNumber
rbi_ckey_first_block(uint32 ckey)
{
	return (BlockNumber) (((uint64) ckey) << (RBI_CONTAINER_BITS - RBI_OFFSET_BITS));
}

/* Split a lo value into (block within container, offset). */
static inline void
rbi_lo_split(uint16 lo, uint16 *blk_in_container, OffsetNumber *off)
{
	*blk_in_container = (uint16) (lo >> RBI_OFFSET_BITS);
	*off = (OffsetNumber) (lo & ((1 << RBI_OFFSET_BITS) - 1));
}
#endif							/* !FRONTEND */

#endif							/* RBI_TID_H */
