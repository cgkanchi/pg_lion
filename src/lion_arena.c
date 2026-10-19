/*-------------------------------------------------------------------------
 * lion_arena.c
 *	  An allocator over a temporary file mapped into memory (lion_arena.h;
 *	  DESIGN.md §30.4, "Spilling").
 *
 *	  A region is mapped once and never moves, so a block's address holds
 *	  for the life of the arena.  Blocks are taken from the current region's
 *	  tail, then from any other region with room, then from a new region;
 *	  one given back goes on its class's free list, and the next block of
 *	  that class takes it.  The classes are powers of two, so a container
 *	  that grows by a few members a flush (lo_gather_flush()) goes back to
 *	  the block it just left, and only its passage into the next class
 *	  leaves a block behind, for a key at that size to take.
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include <errno.h>
#include <string.h>
#include <sys/mman.h>

#include "lion_arena.h"

/* A block at its smallest holds the free list's link. */
StaticAssertDecl(LION_ARENA_ALIGN >= sizeof(void *), "a block holds a pointer");
StaticAssertDecl(LION_ARENA_SMALL_MAX % LION_ARENA_ALIGN == 0,
				 "the small class is aligned");
StaticAssertDecl(LION_ARENA_GRAIN % 4096 == 0, "a region is whole pages");

void
lion_arena_init(LionArena *a, LionArenaRegion *regions, int maxregions,
				LionArenaExtend extend, LionArenaFd fd, void *arg)
{
	memset(a, 0, sizeof(LionArena));
	a->regions = regions;
	a->maxregions = maxregions;
	a->cur = -1;
	a->nextsize = LION_ARENA_GRAIN;
	a->extend = extend;
	a->fd = fd;
	a->arg = arg;
}

/* The region p is in, or -1: a bisection of the regions by base. */
static int
lion_arena_region_of(const LionArena *a, const void *p)
{
	const char *c = (const char *) p;
	int			lo = 0;
	int			hi = a->nregions - 1;

	while (lo <= hi)
	{
		int			mid = lo + (hi - lo) / 2;
		const LionArenaRegion *r = &a->regions[mid];

		if (c < r->base)
			hi = mid - 1;
		else if (c >= r->base + r->size)
			lo = mid + 1;
		else
			return mid;
	}
	return -1;
}

bool
lion_arena_contains(const LionArena *a, const void *p)
{
	return lion_arena_region_of(a, p) >= 0;
}

/*
 * Map a new region of at least `need` bytes: the file extended by its size,
 * and the mapping put among the regions by base.  Its index, or -1 with
 * errno set.
 */
static int
lion_arena_add_region(LionArena *a, Size need)
{
	Size		size;
	void	   *base;
	int			i;

	if (a->nregions >= a->maxregions)
	{
		errno = ENOMEM;
		return -1;
	}
	size = Max(a->nextsize, need);
	size = ((size + LION_ARENA_GRAIN - 1) / LION_ARENA_GRAIN) * LION_ARENA_GRAIN;
	if (!a->extend(a->arg, a->filesize, a->filesize + size))
		return -1;
	base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, a->fd(a->arg),
				(off_t) a->filesize);
	if (base == MAP_FAILED)
		return -1;				/* the file's tail stays, zero, for a retry */

	/* in by base: the regions are bisected (lion_arena_region_of()) */
	for (i = a->nregions; i > 0 && a->regions[i - 1].base > (char *) base; i--)
		a->regions[i] = a->regions[i - 1];
	a->regions[i].base = (char *) base;
	a->regions[i].size = size;
	a->regions[i].fileoff = a->filesize;
	a->regions[i].used = 0;
	a->nregions++;
	a->filesize += size;
	/* the next region doubles the file, to LION_ARENA_REGION_MAX */
	a->nextsize = Min(a->filesize, LION_ARENA_REGION_MAX);
	return i;
}

void *
lion_arena_alloc(LionArena *a, Size size)
{
	int			cls;
	Size		bsize;
	LionArenaRegion *r;
	void	   *p;
	int			i;

	if (size > LION_ARENA_ALLOC_MAX)
	{
		errno = EINVAL;
		return NULL;
	}
	cls = lion_arena_class(size);
	bsize = lion_arena_class_size(cls);
	Assert(bsize >= size && bsize % LION_ARENA_ALIGN == 0);

	/* a block of the class given back */
	p = a->freelist[cls];
	if (p != NULL)
	{
		a->freelist[cls] = *(void **) p;
		return p;
	}

	/* the current region's tail, else any region's, else a new one's */
	if (a->cur < 0 || a->regions[a->cur].used + bsize > a->regions[a->cur].size)
	{
		for (i = 0; i < a->nregions; i++)
		{
			if (a->regions[i].used + bsize <= a->regions[i].size)
				break;
		}
		if (i == a->nregions)
		{
			i = lion_arena_add_region(a, bsize);
			if (i < 0)
				return NULL;
		}
		a->cur = i;
	}
	r = &a->regions[a->cur];
	p = r->base + r->used;
	r->used += bsize;
	return p;
}

void
lion_arena_free(LionArena *a, void *p, Size size)
{
	int			cls = lion_arena_class(size);

	Assert(lion_arena_contains(a, p));
	Assert(((uintptr_t) p) % LION_ARENA_ALIGN == 0);
	*(void **) p = a->freelist[cls];
	a->freelist[cls] = p;
}

void
lion_arena_reset(LionArena *a)
{
	int			i;

	for (i = 0; i < a->nregions; i++)
		a->regions[i].used = 0;
	memset(a->freelist, 0, sizeof(a->freelist));
	a->cur = a->nregions > 0 ? 0 : -1;
}

void
lion_arena_release(LionArena *a)
{
	int			i;

	for (i = 0; i < a->nregions; i++)
		(void) munmap(a->regions[i].base, a->regions[i].size);
	a->nregions = 0;
	a->cur = -1;
	a->filesize = 0;
	a->nextsize = LION_ARENA_GRAIN;
	memset(a->freelist, 0, sizeof(a->freelist));
}

Size
lion_arena_used(const LionArena *a)
{
	Size		used = 0;
	int			i;

	for (i = 0; i < a->nregions; i++)
		used += a->regions[i].used;
	return used;
}
