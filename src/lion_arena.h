/*-------------------------------------------------------------------------
 * lion_arena.h
 *	  An allocator over a temporary file mapped into memory: where a
 *	  LionOrdered set lives once it outgrows hash_mem (DESIGN.md §30.4,
 *	  "Spilling").
 *
 *	  The arena maps the file in regions, each a whole number of
 *	  LION_ARENA_GRAIN bytes, doubling in size to LION_ARENA_REGION_MAX, and
 *	  hands out blocks from them in size classes - the powers of two from 16
 *	  bytes, with one class for the largest container, 4,104 bytes aligned -
 *	  each class with a free list of the blocks given back, taken last in,
 *	  first out.  A block never moves: what the arena hands out stays at its
 *	  address until the arena is reset or released, so a set's containers
 *	  are probed through plain pointers whether they are in memory or here.
 *
 *	  The file is the caller's.  The arena asks for it to be extended, zero
 *	  filled and allocated on disk, through `extend`, and for its descriptor
 *	  through `fd`, and maps what was extended, shared, so that the kernel
 *	  writes the pages to the file and takes them back under memory
 *	  pressure, which is the spill: a page is in memory while the page cache
 *	  keeps it, and no setting of the server's counts it.  Zero filling
 *	  through the caller is what makes the pages exist before they are
 *	  mapped - a page allocated on a full disk at the first write to it would
 *	  end the process with SIGBUS - and what lets the caller's file layer
 *	  count the bytes (temp_file_limit, log_temp_files).
 *
 *	  No palloc, no elog: the struct and the regions' array are the
 *	  caller's, a failure comes back as NULL with errno set, and the file is
 *	  built with -DFRONTEND for test/unit/arena_test.c.
 *-------------------------------------------------------------------------
 */
#ifndef LION_ARENA_H
#define LION_ARENA_H

#include "c.h"

#include "port/pg_bitutils.h"

/* Every block is aligned to this, and a size class is a multiple of it. */
#define LION_ARENA_ALIGN		16

/*
 * The size classes: 16 << c for c = 0 .. 8 (16 bytes to 4,096), then
 * LION_ARENA_SMALL_MAX (a container at its largest, 4,104 bytes, aligned),
 * then 16 << (c - 1) for c = 10 .. (8,192 bytes on).  The largest block is
 * 16 << (LION_ARENA_NCLASSES - 2), 2 GB.
 */
#define LION_ARENA_SMALL_MAX	4112
#define LION_ARENA_NCLASSES		29
#define LION_ARENA_ALLOC_MAX	((Size) 16 << (LION_ARENA_NCLASSES - 2))

/* A region is a whole number of these, and grows to LION_ARENA_REGION_MAX. */
#define LION_ARENA_GRAIN		((Size) 1024 * 1024)
#define LION_ARENA_REGION_MAX	((Size) 64 * 1024 * 1024)

typedef struct LionArenaRegion
{
	char	   *base;			/* where it is mapped */
	Size		size;
	Size		fileoff;		/* the file bytes it maps */
	Size		used;			/* bytes handed out from its start */
} LionArenaRegion;

/*
 * Make the file's bytes from `from` to `to` exist, zero filled: false, with
 * errno set, when that failed.  (The server's side reports its own errors
 * and never returns false.)
 */
typedef bool (*LionArenaExtend) (void *arg, Size from, Size to);

/* The file's descriptor, open, to map from. */
typedef int (*LionArenaFd) (void *arg);

typedef struct LionArena
{
	LionArenaRegion *regions;	/* [maxregions], in ascending base */
	int			nregions;
	int			maxregions;
	int			cur;			/* the region blocks are taken from, or -1 */
	Size		filesize;		/* the file's bytes: the regions' sizes */
	Size		nextsize;		/* the size of the next region */
	void	   *freelist[LION_ARENA_NCLASSES];	/* blocks given back, by class */
	LionArenaExtend extend;
	LionArenaFd fd;
	void	   *arg;
} LionArena;

/* The class of a block of `size` bytes, and a class's block size. */
static inline int
lion_arena_class(Size size)
{
	if (size <= 16)
		return 0;
	if (size <= 4096)
		return (int) pg_ceil_log2_64(size) - 4;
	if (size <= LION_ARENA_SMALL_MAX)
		return 9;
	return (int) pg_ceil_log2_64(size) - 4 + 1;
}

static inline Size
lion_arena_class_size(int cls)
{
	if (cls <= 8)
		return (Size) 16 << cls;
	if (cls == 9)
		return LION_ARENA_SMALL_MAX;
	return (Size) 16 << (cls - 1);
}

/* Whether this platform can map a file at all (not on Windows). */
extern bool lion_arena_available(void);

/*
 * Set up an arena over no regions yet, with room for maxregions of them in
 * the caller's array; the file is extended and mapped through the
 * callbacks, with arg.
 */
extern void lion_arena_init(LionArena *a, LionArenaRegion *regions,
							int maxregions, LionArenaExtend extend,
							LionArenaFd fd, void *arg);

/*
 * A block of at least `size` bytes, aligned to LION_ARENA_ALIGN, from the
 * free list of its class or from a region's unused tail - a new region
 * when none has room.  NULL, with errno set, when the file could not be
 * extended or mapped, when every region is in use (ENOMEM), or when the
 * block would be larger than LION_ARENA_ALLOC_MAX (EINVAL).  The block's
 * contents are whatever they were.
 */
extern void *lion_arena_alloc(LionArena *a, Size size);

/* Give back a block of `size` bytes lion_arena_alloc() handed out. */
extern void lion_arena_free(LionArena *a, void *p, Size size);

/* Whether p is in one of the arena's regions. */
extern bool lion_arena_contains(const LionArena *a, const void *p);

/* Every block given back, the regions kept mapped for the next build. */
extern void lion_arena_reset(LionArena *a);

/* Unmap every region; the file is left to its owner. */
extern void lion_arena_release(LionArena *a);

/*
 * The bytes the regions have handed out, blocks given back included: what
 * of the file a build wrote.
 */
extern Size lion_arena_used(const LionArena *a);

#endif							/* LION_ARENA_H */
