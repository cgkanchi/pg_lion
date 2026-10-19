/*-------------------------------------------------------------------------
 * arena_test.c
 *	  Standalone unit tests for src/lion_arena.c: the allocator over a
 *	  mapped temporary file that holds a LionOrdered set past hash_mem
 *	  (DESIGN.md §30.4, "Spilling").
 *
 *	  Built by "make unit PG_CONFIG=..." with -DFRONTEND.  The file is a
 *	  tmpfile() of this process, extended by ftruncate() - sparse, so the
 *	  regions cost no disk until a page is written - and the properties
 *	  under test are:
 *
 *	  - the size classes: every size goes to the smallest class that holds
 *		it, each class a multiple of the alignment, the container's largest
 *		size in a class of its own, and a request past the largest class is
 *		refused;
 *	  - a block is aligned, in a region, writable to its last byte, and
 *		overlaps no other live block; its contents hold until it is freed;
 *	  - a block given back is the next one its class gets, last in first
 *		out, and no other class gets it;
 *	  - the regions: the first a grain, each next one doubling the file to
 *		the cap, a block larger than the next region's size getting one of
 *		its own, a region's tail too small for a block left for the block
 *		to start a new region, and the file's size the regions' sum;
 *	  - a file that cannot be extended, a mapping refused, and the regions'
 *		array full each come back as NULL with errno, leave the arena as it
 *		was, and a later request that can be met is;
 *	  - a reset frees everything, keeps the regions, and hands them out
 *		again from the first; a release unmaps them and leaves an arena
 *		that can be set up again;
 *	  - under random requests and returns of random sizes, the shadow model
 *		holds: no overlap, every live block's bytes intact, containment
 *		true for every live block and false outside the arena, and the
 *		bytes used never past the file's size.
 *
 *	  Exit status is 0 only if every check passed.
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lion_arena.h"

/* ----------------------------------------------------------------
 *							bookkeeping
 * ----------------------------------------------------------------
 */

static const char *cur_phase = "startup";
static long nchecks = 0;
static long nfail = 0;

static void
check_impl(bool ok, int line, const char *msg)
{
	nchecks++;
	if (!ok)
	{
		nfail++;
		if (nfail <= 40)
			printf("FAIL [%s] arena_test.c:%d: %s\n", cur_phase, line, msg);
		else if (nfail == 41)
			printf("... further failures suppressed\n");
	}
}

#define CHECK(ok, msg)	check_impl((ok), __LINE__, (msg))

static void
phase(const char *name)
{
	cur_phase = name;
}

/* A small deterministic PRNG (xorshift64*), so a failure reproduces. */
static uint64 rng_state = UINT64CONST(0x9E3779B97F4A7C15);

static uint64
rng(void)
{
	rng_state ^= rng_state >> 12;
	rng_state ^= rng_state << 25;
	rng_state ^= rng_state >> 27;
	return rng_state * UINT64CONST(2685821657736338717);
}

/* ----------------------------------------------------------------
 *							the file
 * ----------------------------------------------------------------
 */

/* The file the arena is over, and how its callbacks are to behave. */
typedef struct TestFile
{
	FILE	   *f;
	Size		size;			/* what extend made it */
	int			extends;		/* calls to extend */
	Size		lastfrom;		/* the last call's range */
	Size		lastto;
	bool		refuse;			/* extend fails with ENOSPC */
	bool		badfd;			/* fd hands back a closed descriptor */
} TestFile;

static bool
test_extend(void *arg, Size from, Size to)
{
	TestFile   *tf = (TestFile *) arg;

	tf->extends++;
	tf->lastfrom = from;
	tf->lastto = to;
	if (tf->refuse)
	{
		errno = ENOSPC;
		return false;
	}
	if (from != tf->size)
	{
		/* only the tail is ever extended; a mapping that failed left it */
		if (from > tf->size)
		{
			errno = EINVAL;
			return false;
		}
	}
	if (ftruncate(fileno(tf->f), (off_t) to) != 0)
		return false;
	tf->size = to;
	return true;
}

static int
test_fd(void *arg)
{
	TestFile   *tf = (TestFile *) arg;

	if (tf->badfd)
		return -1;
	return fileno(tf->f);
}

static void
test_file_open(TestFile *tf)
{
	memset(tf, 0, sizeof(*tf));
	tf->f = tmpfile();
	if (tf->f == NULL)
	{
		printf("FAIL: cannot create a temporary file\n");
		exit(1);
	}
}

static void
test_file_close(TestFile *tf)
{
	fclose(tf->f);
	tf->f = NULL;
}

/* ----------------------------------------------------------------
 *							the tests
 * ----------------------------------------------------------------
 */

#define NREGIONS	64

static void
test_classes(void)
{
	Size		size;
	int			cls;

	phase("classes");
	CHECK(lion_arena_class(1) == 0 && lion_arena_class_size(0) == 16, "1 byte: 16");
	CHECK(lion_arena_class(16) == 0, "16 bytes: 16");
	CHECK(lion_arena_class(17) == 1 && lion_arena_class_size(1) == 32, "17 bytes: 32");
	CHECK(lion_arena_class(4096) == 8 && lion_arena_class_size(8) == 4096, "4096: 4096");
	CHECK(lion_arena_class(4097) == 9 && lion_arena_class_size(9) == LION_ARENA_SMALL_MAX,
		  "4097: the container class");
	CHECK(lion_arena_class(4104) == 9, "4104 (a container at most): the container class");
	CHECK(lion_arena_class(LION_ARENA_SMALL_MAX) == 9, "4112: the container class");
	CHECK(lion_arena_class(LION_ARENA_SMALL_MAX + 1) == 10 &&
		  lion_arena_class_size(10) == 8192, "4113: 8192");
	CHECK(lion_arena_class(8192) == 10, "8192: 8192");
	CHECK(lion_arena_class(8193) == 11 && lion_arena_class_size(11) == 16384, "8193: 16384");
	CHECK(lion_arena_class(LION_ARENA_ALLOC_MAX) == LION_ARENA_NCLASSES - 1,
		  "the largest block is the last class");
	CHECK(lion_arena_class_size(LION_ARENA_NCLASSES - 1) == LION_ARENA_ALLOC_MAX,
		  "the last class is the largest block");

	/* every size to the smallest class that holds it, aligned */
	for (size = 1; size <= 70000; size++)
	{
		cls = lion_arena_class(size);
		if (cls < 0 || cls >= LION_ARENA_NCLASSES)
		{
			CHECK(false, "a class out of range");
			break;
		}
		if (lion_arena_class_size(cls) < size)
		{
			CHECK(false, "a class too small for its size");
			break;
		}
		if (cls > 0 && lion_arena_class_size(cls - 1) >= size)
		{
			CHECK(false, "a smaller class would have held the size");
			break;
		}
		if (lion_arena_class_size(cls) % LION_ARENA_ALIGN != 0)
		{
			CHECK(false, "a class not a multiple of the alignment");
			break;
		}
	}
	CHECK(size > 70000, "every size to 70000 is classed");
	for (cls = 1; cls < LION_ARENA_NCLASSES; cls++)
	{
		if (lion_arena_class_size(cls) <= lion_arena_class_size(cls - 1))
		{
			CHECK(false, "the classes do not ascend");
			break;
		}
	}
	CHECK(cls == LION_ARENA_NCLASSES, "the classes ascend");
}

static void
test_blocks(void)
{
	TestFile	tf;
	LionArena	a;
	LionArenaRegion regions[NREGIONS];
	char	   *p1;
	char	   *p2;
	char	   *p3;
	char	   *q;
	int			onstack = 0;

	phase("blocks");
	test_file_open(&tf);
	lion_arena_init(&a, regions, NREGIONS, test_extend, test_fd, &tf);
	CHECK(lion_arena_available(), "this platform maps files");
	CHECK(a.nregions == 0 && a.filesize == 0, "an arena starts with no region");
	CHECK(lion_arena_used(&a) == 0, "... and nothing used");
	CHECK(!lion_arena_contains(&a, &onstack), "contains nothing yet");

	p1 = (char *) lion_arena_alloc(&a, 100);
	CHECK(p1 != NULL, "a block of 100");
	CHECK(a.nregions == 1 && a.filesize == LION_ARENA_GRAIN, "... from a first region of a grain");
	CHECK(tf.extends == 1 && tf.lastfrom == 0 && tf.lastto == LION_ARENA_GRAIN,
		  "... the file extended once, by the grain");
	CHECK(((uintptr_t) p1) % LION_ARENA_ALIGN == 0, "... aligned");
	CHECK(lion_arena_contains(&a, p1), "... in the arena");
	CHECK(lion_arena_contains(&a, p1 + 99), "... to its last byte");
	CHECK(!lion_arena_contains(&a, &onstack), "the stack is not");
	CHECK(lion_arena_used(&a) == 128, "100 bytes take a block of 128");
	memset(p1, 0xA5, 100);

	p2 = (char *) lion_arena_alloc(&a, 4104);
	CHECK(p2 != NULL && p2 != p1, "a container-sized block, another");
	CHECK(p2 >= p1 + 128, "... past the first");
	CHECK(lion_arena_used(&a) == 128 + LION_ARENA_SMALL_MAX, "... of the container class");
	memset(p2, 0x5A, 4104);
	CHECK(p1[0] == (char) 0xA5 && p1[99] == (char) 0xA5, "the first block's bytes hold");

	/* given back: the next of its class, LIFO; not another class's */
	lion_arena_free(&a, p1, 100);
	p3 = (char *) lion_arena_alloc(&a, 4104);
	CHECK(p3 != NULL && p3 != p1, "another class does not take it");
	CHECK(lion_arena_used(&a) == 128 + 2 * LION_ARENA_SMALL_MAX, "... but a new block");
	q = (char *) lion_arena_alloc(&a, 128);
	CHECK(q == p1, "its class takes it back");
	CHECK(lion_arena_used(&a) == 128 + 2 * LION_ARENA_SMALL_MAX, "... at no new bytes");
	lion_arena_free(&a, q, 128);
	lion_arena_free(&a, p3, 4104);
	lion_arena_free(&a, p2, 4104);
	CHECK(lion_arena_alloc(&a, 4100) == p2, "last in, first out");
	CHECK(lion_arena_alloc(&a, 4100) == p3, "... then the one before");
	q = (char *) lion_arena_alloc(&a, 4100);
	CHECK(q != NULL && q != p2 && q != p3 && q != p1, "... then a new block");
	CHECK(lion_arena_alloc(&a, 65) == p1, "a smaller size of the same class takes it");

	/* too large */
	errno = 0;
	CHECK(lion_arena_alloc(&a, LION_ARENA_ALLOC_MAX + 1) == NULL && errno == EINVAL,
		  "a block past the largest class is refused");
	CHECK(a.nregions == 1, "... and no region was added");

	lion_arena_release(&a);
	CHECK(a.nregions == 0 && a.filesize == 0, "released: no region");
	CHECK(!lion_arena_contains(&a, p1), "... contains nothing");
	test_file_close(&tf);
}

static void
test_regions(void)
{
	TestFile	tf;
	LionArena	a;
	LionArenaRegion regions[NREGIONS];
	Size		mfile = 0;		/* the model: the file, the next region, the room */
	Size		mnext = LION_ARENA_GRAIN;
	Size		mroom = 0;
	int			mregions = 0;
	Size		off;
	char	   *p;
	int			i;

	phase("regions");
	test_file_open(&tf);
	lion_arena_init(&a, regions, NREGIONS, test_extend, test_fd, &tf);

	/*
	 * Grain-sized blocks: the first region is a grain, each next one doubles
	 * the file to the cap, and a region is filled before another is added.
	 */
	for (i = 0; i < 200; i++)
	{
		p = (char *) lion_arena_alloc(&a, LION_ARENA_GRAIN);
		if (p == NULL)
		{
			CHECK(false, "a grain-sized block");
			break;
		}
		if (mroom < LION_ARENA_GRAIN)
		{
			Size		size = Max(mnext, LION_ARENA_GRAIN);

			mfile += size;
			mroom = size;
			mnext = Min(mfile, LION_ARENA_REGION_MAX);
			mregions++;
		}
		mroom -= LION_ARENA_GRAIN;
		if (a.filesize != mfile || a.nregions != mregions)
		{
			printf("  block %d: file %zu MB in %d regions, expected %zu MB in %d\n",
				   i, a.filesize / LION_ARENA_GRAIN, a.nregions,
				   mfile / LION_ARENA_GRAIN, mregions);
			CHECK(false, "the regions double the file to the cap");
			break;
		}
	}
	CHECK(i == 200, "200 grain-sized blocks");
	CHECK(a.nregions == 10 && a.filesize == 256 * LION_ARENA_GRAIN,
		  "... in ten regions: 1, 1, 2, 4, 8, 16, 32, 64, 64 and 64 MB");
	CHECK(tf.extends == 10 && tf.size == a.filesize, "... the file extended for each");
	CHECK(lion_arena_used(&a) == 200 * LION_ARENA_GRAIN, "... 200 grains used");
	/* the regions are kept by base, and between them map the whole file */
	for (i = 1; i < a.nregions; i++)
	{
		if (a.regions[i].base <= a.regions[i - 1].base)
			break;
	}
	CHECK(i == a.nregions, "the regions ascend by base");
	off = 0;
	for (i = 0; i < a.nregions; i++)
	{
		int			j;

		for (j = 0; j < a.nregions; j++)
		{
			if (a.regions[j].fileoff == off)
				break;
		}
		if (j == a.nregions)
			break;
		off += a.regions[j].size;
	}
	CHECK(i == a.nregions && off == a.filesize, "the regions tile the file");

	/* a block larger than the next region: a region of its own, rounded */
	p = (char *) lion_arena_alloc(&a, 100 * LION_ARENA_GRAIN + 1);
	CHECK(p != NULL && a.nregions == 11, "a block of 100 MB and a byte: its own region");
	CHECK(a.regions[a.cur].size == 128 * LION_ARENA_GRAIN,
		  "... of its class, 128 MB");
	CHECK(a.nextsize == LION_ARENA_REGION_MAX, "... and the next region is capped");
	lion_arena_release(&a);
	test_file_close(&tf);

	/* a tail too small for a block: the block starts a new region */
	test_file_open(&tf);
	lion_arena_init(&a, regions, NREGIONS, test_extend, test_fd, &tf);
	for (i = 0; i < 255; i++)
	{
		if (lion_arena_alloc(&a, 4096) == NULL)
			break;
	}
	CHECK(i == 255 && a.nregions == 1, "255 blocks of 4 kB in the first region");
	CHECK(lion_arena_used(&a) == 255 * 4096, "... 4 kB of it left");
	p = (char *) lion_arena_alloc(&a, 4104);
	CHECK(p != NULL && a.nregions == 2, "a container does not fit its tail: a second region");
	CHECK(p == a.regions[a.cur].base, "... at its start");
	CHECK(lion_arena_used(&a) == 255 * 4096 + LION_ARENA_SMALL_MAX,
		  "... the tail not counted as used");
	lion_arena_release(&a);
	test_file_close(&tf);
}

static void
test_failures(void)
{
	TestFile	tf;
	LionArena	a;
	LionArenaRegion regions[2];
	char	   *p;

	phase("failures");
	test_file_open(&tf);
	lion_arena_init(&a, regions, 2, test_extend, test_fd, &tf);

	/* the file cannot be extended */
	tf.refuse = true;
	errno = 0;
	p = (char *) lion_arena_alloc(&a, 100);
	CHECK(p == NULL && errno == ENOSPC, "no room on disk: NULL, ENOSPC");
	CHECK(a.nregions == 0 && a.filesize == 0 && tf.extends == 1,
		  "... the arena as it was");
	tf.refuse = false;
	p = (char *) lion_arena_alloc(&a, 100);
	CHECK(p != NULL && a.nregions == 1, "... and the next request is met");

	/* the mapping refused */
	tf.badfd = true;
	errno = 0;
	p = (char *) lion_arena_alloc(&a, LION_ARENA_GRAIN);
	CHECK(p == NULL && errno != 0, "a mapping refused: NULL, errno");
	CHECK(a.nregions == 1 && a.filesize == LION_ARENA_GRAIN, "... no region added");
	CHECK(tf.size == 2 * LION_ARENA_GRAIN, "... though the file was extended");
	tf.badfd = false;
	p = (char *) lion_arena_alloc(&a, LION_ARENA_GRAIN);
	CHECK(p != NULL && a.nregions == 2 && a.filesize == 2 * LION_ARENA_GRAIN,
		  "... which the retry maps");
	CHECK(tf.lastfrom == LION_ARENA_GRAIN && tf.lastto == 2 * LION_ARENA_GRAIN,
		  "... extending the same tail again");

	/* the regions' array full */
	errno = 0;
	p = (char *) lion_arena_alloc(&a, LION_ARENA_GRAIN);
	CHECK(p == NULL && errno == ENOMEM, "no room for a region: NULL, ENOMEM");
	CHECK(a.nregions == 2, "... the arena as it was");
	p = (char *) lion_arena_alloc(&a, 100);
	CHECK(p != NULL, "... a block the regions hold is still given");

	lion_arena_release(&a);
	test_file_close(&tf);
}

static void
test_reset(void)
{
	TestFile	tf;
	LionArena	a;
	LionArenaRegion regions[NREGIONS];
	char	   *blocks[150];
	char	   *first;
	char	   *p;
	int			i;

	phase("reset");
	test_file_open(&tf);
	lion_arena_init(&a, regions, NREGIONS, test_extend, test_fd, &tf);
	first = (char *) lion_arena_alloc(&a, 64);
	for (i = 0; i < 300; i++)
	{
		p = (char *) lion_arena_alloc(&a, 4104);
		if (p == NULL)
			break;
		if (i % 2 == 0)
			blocks[i / 2] = p;
	}
	CHECK(i == 300 && a.nregions == 2, "300 containers: two regions");
	for (i = 0; i < 150; i++)
		lion_arena_free(&a, blocks[i], 4104);
	CHECK(a.freelist[9] != NULL, "... half given back: a free list with blocks on it");

	lion_arena_reset(&a);
	CHECK(a.nregions == 2 && a.filesize == 2 * LION_ARENA_GRAIN, "reset keeps the regions");
	CHECK(lion_arena_used(&a) == 0, "... with nothing used");
	CHECK(a.freelist[9] == NULL, "... and the free lists empty");
	CHECK(tf.extends == 2, "... the file not touched");
	p = (char *) lion_arena_alloc(&a, 4104);
	CHECK(p == a.regions[0].base, "the next block is the first region's first");
	CHECK(lion_arena_contains(&a, first), "an old block's address is still in the arena");
	for (i = 0; i < 600; i++)
	{
		if (lion_arena_alloc(&a, 4104) == NULL)
			break;
	}
	CHECK(i == 600 && a.nregions == 3 && tf.extends == 3,
		  "600 more fill the two and add a third, of 2 MB");
	CHECK(a.regions[a.cur].size == 2 * LION_ARENA_GRAIN, "... of 2 MB");

	lion_arena_release(&a);
	CHECK(a.nregions == 0 && lion_arena_used(&a) == 0, "released");
	lion_arena_init(&a, regions, NREGIONS, test_extend, test_fd, &tf);
	p = (char *) lion_arena_alloc(&a, 64);
	CHECK(p != NULL && a.nregions == 1, "set up again over the same file: a region");
	lion_arena_release(&a);
	test_file_close(&tf);
}

/* ----------------------------------------------------------------
 *						random, against a shadow
 * ----------------------------------------------------------------
 */

typedef struct Live
{
	char	   *p;
	Size		size;
	unsigned char pattern;
} Live;

#define MAXLIVE 4000

static bool
overlaps(const Live *a, const char *p, Size size)
{
	return p < a->p + a->size && a->p < p + size;
}

static void
test_random(void)
{
	TestFile	tf;
	LionArena	a;
	LionArenaRegion regions[NREGIONS];
	Live	   *live = (Live *) malloc(sizeof(Live) * MAXLIVE);
	int			nlive = 0;
	long		step;
	long		nalloc = 0;
	long		nfree = 0;
	Size		maxused = 0;
	int			i;
	int			j;
	int			outside = 0;

	phase("random");
	test_file_open(&tf);
	lion_arena_init(&a, regions, NREGIONS, test_extend, test_fd, &tf);
	for (step = 0; step < 60000; step++)
	{
		if (nlive < MAXLIVE && (nlive == 0 || (rng() % 100) < 55))
		{
			/* sizes over every class a set uses, a few of them large */
			uint64		r = rng() % 1000;
			Size		size = r < 700 ? 1 + rng() % 600 :
				r < 990 ? 1 + rng() % 4104 : 1 + rng() % 70000;
			char	   *p = (char *) lion_arena_alloc(&a, size);
			Live	   *l;

			if (p == NULL)
			{
				CHECK(false, "a request not met");
				break;
			}
			if (((uintptr_t) p) % LION_ARENA_ALIGN != 0)
			{
				CHECK(false, "a block not aligned");
				break;
			}
			if (!lion_arena_contains(&a, p) || !lion_arena_contains(&a, p + size - 1))
			{
				CHECK(false, "a block not in the arena");
				break;
			}
			for (i = 0; i < nlive; i++)
			{
				if (overlaps(&live[i], p, size))
					break;
			}
			if (i < nlive)
			{
				CHECK(false, "a block overlaps a live one");
				break;
			}
			l = &live[nlive++];
			l->p = p;
			l->size = size;
			l->pattern = (unsigned char) (rng() & 0xFF);
			memset(p, l->pattern, size);
			nalloc++;
		}
		else
		{
			int			k = (int) (rng() % nlive);
			Live	   *l = &live[k];

			for (i = 0; i < (int) l->size; i++)
			{
				if ((unsigned char) l->p[i] != l->pattern)
					break;
			}
			if (i < (int) l->size)
			{
				CHECK(false, "a live block's bytes changed");
				break;
			}
			lion_arena_free(&a, l->p, l->size);
			live[k] = live[--nlive];
			nfree++;
		}
		if (lion_arena_used(&a) > a.filesize)
		{
			CHECK(false, "more used than the file holds");
			break;
		}
		if (lion_arena_used(&a) > maxused)
			maxused = lion_arena_used(&a);
		if (step % 5000 == 0 && lion_arena_contains(&a, &outside))
			outside++;
	}
	CHECK(step == 60000, "60,000 random steps");
	CHECK(outside == 0, "an address outside is never contained");
	/* every live block intact at the end, and freeing them all */
	for (j = 0; j < nlive; j++)
	{
		Live	   *l = &live[j];

		for (i = 0; i < (int) l->size; i++)
		{
			if ((unsigned char) l->p[i] != l->pattern)
				break;
		}
		if (i < (int) l->size)
			break;
		lion_arena_free(&a, l->p, l->size);
	}
	CHECK(j == nlive, "every live block intact at the end");
	printf("  random: %ld allocations, %ld frees, %d live at the end, %zu kB used at most in %d regions of %zu MB\n",
		   nalloc, nfree, nlive, maxused / 1024, a.nregions,
		   a.filesize / LION_ARENA_GRAIN);
	lion_arena_release(&a);
	test_file_close(&tf);
	free(live);
}

int
main(void)
{
	test_classes();
	test_blocks();
	test_regions();
	test_failures();
	test_reset();
	test_random();

	printf("arena_test: %ld checks, %ld failures\n", nchecks, nfail);
	return nfail == 0 ? 0 : 1;
}
