/*-------------------------------------------------------------------------
 *
 * lion_vm.h
 *		Reading the heap's visibility map: the layout of a map page, the
 *		all-visible bits of a run of heap blocks a word at a time, and the
 *		few map pages a reader keeps pinned (DESIGN.md §9, "The map pages a
 *		reader keeps pinned").
 *
 * The word-at-a-time half is plain arithmetic on map bytes and is unit
 * tested standalone (test/unit/vm_test.c, built with -DFRONTEND); the pins
 * are the backend's.
 *
 *-------------------------------------------------------------------------
 */
#ifndef LION_VM_H
#define LION_VM_H

#include "c.h"

#include "port/pg_bswap.h"

/*
 * pg_always_inline is the newer spelling of pg_attribute_always_inline, and
 * master's only one; released 16.x and 17.x minors have only the older
 * (lion_compat.h says the same; this header is also built without it).
 */
#ifndef pg_always_inline
#define pg_always_inline pg_attribute_always_inline
#endif

#ifndef FRONTEND
#include "access/visibilitymap.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "utils/rel.h"

#include "lion_compat.h"
#endif

/*
 * The all-visible bits of the 32 heap blocks one 64-bit word of the map
 * covers.  Each block has two bits, all-visible the low one of the pair
 * (VISIBILITYMAP_ALL_VISIBLE, asserted in lion_count_int.h), and the blocks
 * run from the low bits of the word up; squeezing out every other bit leaves
 * block i's answer at bit i.  Five shift-and-mask steps for 32 blocks, where
 * a byte at a time takes three for four.
 */
static pg_always_inline uint64
lion_vm_squeeze(uint64 x)
{
	x &= UINT64CONST(0x5555555555555555);
	x = (x | (x >> 1)) & UINT64CONST(0x3333333333333333);
	x = (x | (x >> 2)) & UINT64CONST(0x0f0f0f0f0f0f0f0f);
	x = (x | (x >> 4)) & UINT64CONST(0x00ff00ff00ff00ff);
	x = (x | (x >> 8)) & UINT64CONST(0x0000ffff0000ffff);
	x = (x | (x >> 16)) & UINT64CONST(0x00000000ffffffff);
	return x;
}

/*
 * The all-visible bits of the heap blocks whose map bits are the `nbytes`
 * bytes at `map`, four blocks a byte: bit i of the result is the i-th block.
 * nbytes is 4, 8 or 16 - a container's worth at 32, 16 and 8 kB pages - and
 * a constant at every call, so this compiles to one or two loads and the
 * squeezes.  The map is a byte array, block 4j + k at bits 2k and 2k + 1 of
 * byte j; read as a little-endian word that is the order the squeeze wants,
 * so a big-endian machine swaps the bytes first.  `map` needs no alignment.
 */
static pg_always_inline uint64
lion_vm_allvisible_bytes(const char *map, int nbytes)
{
	uint64		lo;
	uint64		hi;
	uint32		half;

	Assert(nbytes == 4 || nbytes == 8 || nbytes == 16);

	if (nbytes == 4)
	{
		memcpy(&half, map, sizeof(half));
#ifdef WORDS_BIGENDIAN
		half = pg_bswap32(half);
#endif
		return lion_vm_squeeze((uint64) half);
	}

	memcpy(&lo, map, sizeof(lo));
#ifdef WORDS_BIGENDIAN
	lo = pg_bswap64(lo);
#endif
	if (nbytes == 8)
		return lion_vm_squeeze(lo);

	memcpy(&hi, map + sizeof(lo), sizeof(hi));
#ifdef WORDS_BIGENDIAN
	hi = pg_bswap64(hi);
#endif
	return lion_vm_squeeze(lo) | (lion_vm_squeeze(hi) << 32);
}

#ifndef FRONTEND

/*
 * The layout of a visibility map page.  These mirror the private macros of
 * the same name in src/backend/access/heap/visibilitymap.c, which is the only
 * place the format is written down; nothing outside that file exports them.
 * Keep them in step with it.  lion_count_int.h's static assertions pin down
 * the parts of the format that visibilitymapdefs.h does export, which is what
 * the bit twiddling above actually depends on.
 */
#define LION_VM_MAPSIZE				(BLCKSZ - MAXALIGN(SizeOfPageHeaderData))
#define LION_VM_HEAPBLOCKS_PER_BYTE	(BITS_PER_BYTE / BITS_PER_HEAPBLOCK)
#define LION_VM_HEAPBLOCKS_PER_PAGE	(LION_VM_MAPSIZE * LION_VM_HEAPBLOCKS_PER_BYTE)
#define LION_VM_HEAPBLK_TO_MAPBLOCK(x) ((x) / LION_VM_HEAPBLOCKS_PER_PAGE)
#define LION_VM_HEAPBLK_TO_MAPBYTE(x) \
	(((x) % LION_VM_HEAPBLOCKS_PER_PAGE) / LION_VM_HEAPBLOCKS_PER_BYTE)

/*
 * The map pages a reader keeps pinned, up to LION_VM_PINS of them, map page m
 * in slot m % LION_VM_PINS (DESIGN.md §9, "The map pages a reader keeps
 * pinned").
 *
 * One pin, which is what core's index-only scan keeps (ioss_VMBuffer), is
 * enough for a reader that goes through the heap in order.  A reader that
 * goes through it many times over - a count per key of a join, each key's
 * rows scattered over the heap - goes from map page to map page once per key,
 * and a single pin is let go of and taken again at every step: a buffer
 * lookup under a partition lock and two atomic operations on a buffer header
 * every participant of a parallel plan shares.  A semi join's count over
 * 250,000 keys of a 750 MB heap, whose map is three pages, pinned them 590,000
 * times, a sixth of its time.  With a slot for each map page, nothing is
 * pinned twice.
 *
 * A map page covers some 255 MB of heap at 8 kB pages, so eight slots hold
 * every map page of a heap up to 2 GB.  Past that, map pages that share a
 * slot take turns in it, and those that do not still stay pinned.  Slots are
 * filled only as map pages are asked for: a heap of one map page holds one
 * pin.  And on a pool small for the connections it serves - a backend's fair
 * share of it (lion_pin_fair_share()) under four times LION_VM_PINS - the set
 * has one slot, the single pin a reader always held, which is what the pin
 * budget of DESIGN.md §15 leaves room for.  The pages are the same for every
 * set over the same heap, so one heap's map takes no more buffers of the pool
 * than it has pages however many sets and backends read it: at most
 * LION_VM_PINS for a heap up to 2 GB.  Past that the bound is LION_VM_PINS a
 * set, and a backend can hold several - one for each node of its plan that
 * reads the map, and one for each group or column context a count rechecks
 * through.
 *
 * Every pin is of the same relation's map; whoever fills the set says which
 * (LionVisCache.vmrelid).  A map page is no index page: no part of the §9
 * interlock, and nothing waits for its pin - setting or clearing a bit takes
 * the page's content lock, and only a truncation of the map, under an
 * AccessExclusiveLock a reader's own lock excludes, would.  A zeroed struct is
 * an empty set of one slot (InvalidBuffer is 0).
 */
#define LION_VM_PINS	8
#define LION_VM_PINS_MIN_SHARE	(4 * LION_VM_PINS)

StaticAssertDecl((LION_VM_PINS & (LION_VM_PINS - 1)) == 0,
				 "pg_lion: LION_VM_PINS is not a power of two");

typedef struct LionVmPins
{
	Buffer		buf[LION_VM_PINS];
	uint32		mask;			/* the slots in use, less one: LION_VM_PINS -
								 * 1, or 0 for one slot */
} LionVmPins;

/* An empty set, of as many slots as the pool allows. */
static inline void
lion_vm_pins_init(LionVmPins *vm)
{
	int			i;

	for (i = 0; i < LION_VM_PINS; i++)
		vm->buf[i] = InvalidBuffer;
	vm->mask = (lion_pin_fair_share() >= LION_VM_PINS_MIN_SHARE) ?
		LION_VM_PINS - 1 : 0;
}

static inline bool
lion_vm_pins_held(const LionVmPins *vm)
{
	int			i;

	for (i = 0; i < LION_VM_PINS; i++)
	{
		if (BufferIsValid(vm->buf[i]))
			return true;
	}
	return false;
}

/* Let go of every pin; the set stays the size it was. */
static inline void
lion_vm_pins_release(LionVmPins *vm)
{
	int			i;

	for (i = 0; i < LION_VM_PINS; i++)
	{
		if (BufferIsValid(vm->buf[i]))
			ReleaseBuffer(vm->buf[i]);
		vm->buf[i] = InvalidBuffer;
	}
}

/*
 * The slot of heap block blk's map page: a Buffer to hand to
 * visibilitymap_get_status(), VM_ALL_VISIBLE() or 19's
 * heap_page_prune_opt(), which move the pin there to the page they need, as
 * they would move a caller's single pin.  What the slot holds is checked by
 * its block number (visibilitymap_pin_ok()), never remembered beside it, so
 * one of those moving it is always seen.
 */
static pg_always_inline Buffer *
lion_vm_pins_slot(LionVmPins *vm, BlockNumber blk)
{
	Assert(vm->mask == 0 || vm->mask == LION_VM_PINS - 1);
	return &vm->buf[LION_VM_HEAPBLK_TO_MAPBLOCK(blk) & vm->mask];
}

/*
 * The map page of heap block blk, pinned in its slot - taken the way
 * visibilitymap_get_status() takes it, with the same refusal to extend the
 * fork - or InvalidBuffer when the fork stops short of it, in which case
 * none of its blocks is all-visible.  Each pin taken adds one to *pins, when
 * pins is not NULL (EXPLAIN ANALYZE's "Visibility Map Pages Pinned").
 */
static pg_always_inline Buffer
lion_vm_pin(Relation heap, BlockNumber blk, LionVmPins *vm, int64 *pins)
{
	Buffer	   *slot = lion_vm_pins_slot(vm, blk);

	if (!visibilitymap_pin_ok(blk, *slot))
	{
		(void) visibilitymap_get_status(heap, blk, slot);
		if (pins != NULL)
			(*pins)++;
	}
	return *slot;
}

/* VM_ALL_VISIBLE() with the set's pins: one heap block's all-visible bit. */
static inline bool
lion_vm_all_visible(Relation heap, BlockNumber blk, LionVmPins *vm)
{
	return VM_ALL_VISIBLE(heap, blk, lion_vm_pins_slot(vm, blk));
}

#endif							/* !FRONTEND */

#endif							/* LION_VM_H */
