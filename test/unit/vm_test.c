/*-------------------------------------------------------------------------
 * vm_test.c
 *	  Standalone unit tests for the word-at-a-time read of the visibility
 *	  map in src/lion_vm.h (lion_vm_squeeze(), lion_vm_allvisible_bytes();
 *	  DESIGN.md §9, "The map pages a reader keeps pinned").
 *
 *	  Built by "make unit PG_CONFIG=..." with -DFRONTEND, so this program
 *	  links against nothing but libc and libpgport.
 *
 *	  Every answer is checked against a reader of its own that knows only
 *	  the map's documented layout - two bits a heap block, four blocks a
 *	  byte, block 4j + k at bits 2k (all-visible) and 2k + 1 (all-frozen) of
 *	  byte j - and reads it a bit at a time.  Each run length the extension
 *	  reads (4, 8 and 16 bytes: a container at 32, 16 and 8 kB pages) is
 *	  checked at every alignment of a buffer, on every value of every byte,
 *	  on runs where only the frozen bits are set, and on random runs.
 *
 *	  Exit status is 0 only if every check passed.
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lion_vm.h"

/* ----------------------------------------------------------------
 *							bookkeeping
 * ----------------------------------------------------------------
 */

static const char *cur_phase = "startup";
static long nchecks = 0;
static long nfail = 0;

/* Count one check, and print the first 40 that fail. */
static void
check_impl(bool ok, int line, const char *msg)
{
	nchecks++;
	if (!ok)
	{
		nfail++;
		if (nfail <= 40)
			printf("FAIL [%s] vm_test.c:%d: %s\n", cur_phase, line, msg);
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

/* ----------------------------------------------------------------
 *			an independent reader of the documented layout
 * ----------------------------------------------------------------
 */

/* The all-visible bit of the run's i-th heap block. */
static bool
ref_allvisible(const unsigned char *map, int i)
{
	return ((map[i / 4] >> (2 * (i % 4))) & 1) != 0;
}

/* lion_vm_allvisible_bytes() as the layout says it must come out. */
static uint64
ref_mask(const unsigned char *map, int nbytes)
{
	uint64		mask = 0;
	int			i;

	for (i = 0; i < 4 * nbytes; i++)
	{
		if (ref_allvisible(map, i))
			mask |= UINT64CONST(1) << i;
	}
	return mask;
}

/* the extension's read of nbytes at map, against the reference */
static void
check_run(const unsigned char *map, int nbytes, const char *what)
{
	uint64		got = lion_vm_allvisible_bytes((const char *) map, nbytes);
	uint64		want = ref_mask(map, nbytes);

	CHECK(got == want, what);
	/* nothing past the run's blocks is ever set */
	if (nbytes < 16)
		CHECK((got >> (4 * nbytes)) == 0, "bits past the run");
}

/* ----------------------------------------------------------------
 *								tests
 * ----------------------------------------------------------------
 */

static const int run_lengths[] = {4, 8, 16};

/* lion_vm_squeeze() on its own: each pair's low bit, packed in order */
static void
test_squeeze(void)
{
	int			i;

	phase("squeeze");
	CHECK(lion_vm_squeeze(0) == 0, "zero");
	CHECK(lion_vm_squeeze(~UINT64CONST(0)) == UINT64CONST(0xffffffff), "all ones");
	CHECK(lion_vm_squeeze(UINT64CONST(0x5555555555555555)) ==
		  UINT64CONST(0xffffffff), "every all-visible bit");
	CHECK(lion_vm_squeeze(UINT64CONST(0xaaaaaaaaaaaaaaaa)) == 0,
		  "every all-frozen bit");
	for (i = 0; i < 32; i++)
	{
		CHECK(lion_vm_squeeze(UINT64CONST(1) << (2 * i)) ==
			  (UINT64CONST(1) << i), "one all-visible bit");
		CHECK(lion_vm_squeeze(UINT64CONST(1) << (2 * i + 1)) == 0,
			  "one all-frozen bit");
		CHECK(lion_vm_squeeze(UINT64CONST(3) << (2 * i)) ==
			  (UINT64CONST(1) << i), "one pair, both bits");
	}
}

/* every value of every byte of a run, the other bytes zero and all ones */
static void
test_each_byte(void)
{
	unsigned char map[16];
	int			r;

	phase("each byte");
	for (r = 0; r < (int) lengthof(run_lengths); r++)
	{
		int			nbytes = run_lengths[r];
		int			j;

		for (j = 0; j < nbytes; j++)
		{
			int			v;

			for (v = 0; v < 256; v++)
			{
				memset(map, 0, sizeof(map));
				map[j] = (unsigned char) v;
				check_run(map, nbytes, "one byte set");

				memset(map, 0xff, sizeof(map));
				map[j] = (unsigned char) v;
				check_run(map, nbytes, "one byte cleared");
			}
		}
	}
}

/* the run read at every alignment of a buffer, with garbage around it */
static void
test_alignment(void)
{
	unsigned char buf[64];
	int			r;

	phase("alignment");
	srand(4242);
	for (r = 0; r < (int) lengthof(run_lengths); r++)
	{
		int			nbytes = run_lengths[r];
		int			off;

		for (off = 0; off + nbytes <= (int) sizeof(buf); off++)
		{
			int			k;
			int			rep;

			for (rep = 0; rep < 64; rep++)
			{
				for (k = 0; k < (int) sizeof(buf); k++)
					buf[k] = (unsigned char) (rand() & 0xff);
				check_run(buf + off, nbytes, "random run at an offset");
			}
		}
	}
}

/* runs with only frozen bits, which say nothing about visibility */
static void
test_frozen_only(void)
{
	unsigned char map[16];
	int			r;

	phase("frozen only");
	for (r = 0; r < (int) lengthof(run_lengths); r++)
	{
		int			nbytes = run_lengths[r];
		int			i;

		memset(map, 0xaa, sizeof(map));
		CHECK(lion_vm_allvisible_bytes((const char *) map, nbytes) == 0,
			  "all frozen, none visible");
		for (i = 0; i < 4 * nbytes; i++)
		{
			memset(map, 0xaa, sizeof(map));
			map[i / 4] |= (unsigned char) (1 << (2 * (i % 4)));
			CHECK(lion_vm_allvisible_bytes((const char *) map, nbytes) ==
				  (UINT64CONST(1) << i), "one visible among frozen");
		}
	}
}

/* Every test; exit status 1 if any check failed. */
int
main(void)
{
	test_squeeze();
	test_each_byte();
	test_alignment();
	test_frozen_only();

	printf("vm_test: %ld checks, %ld failures\n", nchecks, nfail);
	if (nfail != 0)
		return 1;
	printf("ALL TESTS PASSED\n");
	return 0;
}
