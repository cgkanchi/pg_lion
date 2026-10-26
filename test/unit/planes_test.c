/*-------------------------------------------------------------------------
 * planes_test.c
 *	  Standalone unit tests for src/lion_planes.c: bit-plane arithmetic and
 *	  the extension's choice of SIMD implementation (DESIGN.md §50).
 *
 *	  Built by "make unit PG_CONFIG=..." with -DFRONTEND, and linked with
 *	  src/lion_vec_math.c, whose plane-1 kernel and query planes this
 *	  module's must agree with.  The properties under test:
 *
 *	  - the choice: the best implementation is the best the build has and
 *		the CPU runs, it is what runs until one is forced, forcing one the
 *		build or the CPU lacks changes nothing, LION_SIMD_AUTO goes back to
 *		the best, and a build without the x86-64 kernels has the portable
 *		one alone;
 *	  - popcounts per plane, every implementation the build and the CPU
 *		have, INTERLEAVED and STRIDED, against a count a bit at a time: k
 *		of 1, 3, 4, 5, 32 and 64 planes over 1, 3, 4, 24, 100 and 512 words,
 *		of every pattern - none, all, the first or last bit of every word, a
 *		single bit at a word's edge, random words, densities from 0.05% to
 *		99.95% - each plane its own, in exact-size heap buffers at every
 *		8-byte alignment modulo 32, the STRIDED planes each its own
 *		allocation; the inputs left as they were; a single set bit at every
 *		word boundary; every word count up to 130 with every bit set, which
 *		is the most a byte count adds up; and the weighted sums against the
 *		values the planes hold summed over the bits' rows, modulo 2^64;
 *	  - the transpose: against a bit-at-a-time one, every single bit, and
 *		its own inverse;
 *	  - values to planes and back, both layouts, every width of 1 to 64
 *		bits: against a bit-at-a-time scatter, the last word's padding zero,
 *		bits past the width ignored, and the gather the scatter's inverse,
 *		whatever the padding holds;
 *	  - lionvec: the INTERLEAVED weighted sum of four planes is
 *		lionvec_plane_dot() under every pair of implementations, and the
 *		scatter of a query's 4-bit codes is the planes lionvec_query_init()
 *		builds, to the bit.
 *
 *	  With --bench it times the kernels instead (make planes-bench, at -O2).
 *	  Exit status is 0 only if every check passed.
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lion_planes.h"
#include "lion_vec_math.h"

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
			printf("FAIL [%s] planes_test.c:%d: %s\n", cur_phase, line, msg);
		else if (nfail == 41)
			printf("... further failures suppressed\n");
	}
}

#define CHECK(ok, msg)	check_impl((ok), __LINE__, (msg))

/* xorshift64*, from a fixed seed, so that a failure reproduces */
static uint64 rng_state = UINT64CONST(0x9E3779B97F4A7C15);

static uint64
rnd64(void)
{
	uint64		x = rng_state;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	rng_state = x;
	return x * UINT64CONST(2685821657736338717);
}

static uint32
rnd_below(uint32 n)
{
	return (uint32) ((rnd64() >> 32) % n);
}

static void *
xmalloc(Size n)
{
	void	   *p = calloc(1, n > 0 ? n : 1);

	if (p == NULL)
	{
		printf("out of memory\n");
		exit(2);
	}
	return p;
}

/*
 * nwords words at shift bytes past a 32-byte boundary (the AVX2 and AVX-512
 * kernels' vector width), ending where the allocation ends, so that a
 * sanitized build sees any read past them.  *base is what to free().
 */
static uint64 *
words_alloc(uint32 nwords, uint32 shift, void **base)
{
	void	   *p = NULL;

	if (posix_memalign(&p, 32, shift + Max(nwords, 1) * sizeof(uint64)) != 0)
	{
		printf("out of memory\n");
		exit(2);
	}
	*base = p;
	return (uint64 *) ((char *) p + shift);
}

/* The implementations, as forced. */
static const LionSimdImpl impls[] = {
	LION_SIMD_PORTABLE, LION_SIMD_POPCNT, LION_SIMD_AVX2, LION_SIMD_AVX512
};
#define NIMPLS	lengthof(impls)

/* ---------- the choice ---------- */

static void
test_choice(void)
{
	LionSimdImpl best = lion_simd_best();
	LionSimdImpl first = lion_simd_current();
	int			i;
	int			ran = 0;

	cur_phase = "choice";
	CHECK(first == best, "what runs before any force is not the best");
	CHECK(lion_simd_in_use == best, "lion_simd_current() did not resolve the choice");
	CHECK(lion_simd_has(LION_SIMD_AUTO) && lion_simd_has(LION_SIMD_PORTABLE),
		  "every build has the portable kernels");
	CHECK(best != LION_SIMD_AUTO && lion_simd_has(best), "the best is not one this CPU runs");
	for (i = best + 1; i < LION_SIMD_NIMPLS; i++)
		CHECK(!lion_simd_has((LionSimdImpl) i), "an implementation better than the best runs");
	CHECK(!lion_simd_has(LION_SIMD_AVX512) || lion_simd_has(LION_SIMD_AVX2),
		  "AVX-512 without AVX2");
	CHECK(!lion_simd_has((LionSimdImpl) LION_SIMD_NIMPLS), "an implementation past the last");
#ifdef LION_NO_SIMD
	CHECK(best == LION_SIMD_PORTABLE, "LION_NO_SIMD builds no x86 kernel");
	CHECK(!lion_simd_f16c(), "LION_NO_SIMD knows F16C");
#endif
#ifndef LION_HAVE_X86_SIMD
	CHECK(best == LION_SIMD_PORTABLE, "an x86 kernel outside x86-64 GCC and clang");
#endif
	CHECK(strcmp(lion_simd_name(LION_SIMD_AUTO), "auto") == 0 &&
		  strcmp(lion_simd_name(LION_SIMD_PORTABLE), "portable") == 0 &&
		  strcmp(lion_simd_name(LION_SIMD_POPCNT), "popcnt") == 0 &&
		  strcmp(lion_simd_name(LION_SIMD_AVX2), "avx2") == 0 &&
		  strcmp(lion_simd_name(LION_SIMD_AVX512), "avx512") == 0,
		  "the implementations' names");

	printf("planes_test: %s picked for this CPU (F16C %s); kernels in this build and CPU:",
		   lion_simd_name(best), lion_simd_f16c() ? "yes" : "no");
	for (i = 0; i < (int) NIMPLS; i++)
	{
		LionSimdImpl before = lion_simd_current();
		bool		has = lion_simd_has(impls[i]);

		CHECK(lion_simd_force(impls[i]) == has, "force() is not has()");
		if (!has)
		{
			CHECK(lion_simd_current() == before, "a refused force() changed the choice");
			printf(" [%s: not in this build or CPU]", lion_simd_name(impls[i]));
			continue;
		}
		CHECK(lion_simd_current() == impls[i], "force() did not take effect");
		printf(" %s", lion_simd_name(impls[i]));
		ran++;
	}
	printf("\n");
	CHECK(ran >= 1, "no implementation runs");
	CHECK(lion_simd_force(LION_SIMD_AUTO) && lion_simd_current() == best,
		  "force(LION_SIMD_AUTO) goes back to the best");
}

/* ---------- popcounts per plane ---------- */

/* Bit i of the vector w. */
static inline uint32
bit_at(const uint64 *w, uint32 i)
{
	return (uint32) ((w[i >> 6] >> (i & 63)) & 1);
}

/*
 * The counts and the weighted sum a bit at a time, no popcount: for each
 * row the bits select, each plane's bit there; and the value the planes
 * hold at that row, added up modulo 2^64.  planes[j] is plane j.
 */
static uint64
ref_counts(const uint64 *bits, uint64 *const *planes, uint32 nwords, uint32 k,
		   uint32 *counts)
{
	uint64		sum = 0;
	uint32		i;
	uint32		j;

	memset(counts, 0, k * sizeof(uint32));
	for (i = 0; i < nwords * 64; i++)
	{
		uint64		value = 0;

		if (!bit_at(bits, i))
			continue;
		for (j = 0; j < k; j++)
		{
			uint32		b = bit_at(planes[j], i);

			counts[j] += b;
			if (j < 64)
				value |= (uint64) b << j;
		}
		sum += value;
	}
	return sum;
}

#define NPATTERNS	16

/*
 * Pattern p of NPATTERNS over nwords words: none, all, the first or the last
 * bit of every word, one bit at an edge of word param % nwords, random
 * words, and densities.
 */
static void
fill_pattern(uint64 *w, uint32 nwords, int p, uint32 param)
{
	static const uint32 ppm[] = {500, 10000, 100000, 300000, 500000,
	700000, 900000, 990000, 999500};
	uint32		i;

	memset(w, 0, nwords * sizeof(uint64));
	switch (p)
	{
		case 0:
			break;
		case 1:
			memset(w, 0xFF, nwords * sizeof(uint64));
			break;
		case 2:
			for (i = 0; i < nwords; i++)
				w[i] = 1;
			break;
		case 3:
			for (i = 0; i < nwords; i++)
				w[i] = UINT64CONST(1) << 63;
			break;
		case 4:
			w[param % nwords] = UINT64CONST(1) << 63;
			break;
		case 5:
			w[param % nwords] = 1;
			break;
		case 6:
			for (i = 0; i < nwords; i++)
				w[i] = rnd64();
			break;
		default:				/* 7 .. 15: densities */
			for (i = 0; i < nwords * 64; i++)
				if (rnd_below(1000000) < ppm[p - 7])
					w[i >> 6] |= UINT64CONST(1) << (i & 63);
			break;
	}
}

/*
 * An input of the count tests: the bits and k planes, STRIDED in k
 * allocations of their own and INTERLEAVED in one, at alignments that
 * cycle with c.
 */
typedef struct CountCase
{
	uint32		nwords;
	uint32		k;
	uint64	   *bits;
	uint64	  **planes;			/* [k], each [nwords] */
	uint64	   *inter;			/* [nwords * k] */
	void	   *bits_base;
	void	  **planes_base;
	void	   *inter_base;
	uint64	   *bits_copy;
	uint64	   *inter_copy;
} CountCase;

static void
case_alloc(CountCase *cc, uint32 nwords, uint32 k, uint32 c)
{
	uint32		j;

	cc->nwords = nwords;
	cc->k = k;
	cc->bits = words_alloc(nwords, (c % 4) * 8, &cc->bits_base);
	cc->planes = (uint64 **) xmalloc(k * sizeof(uint64 *));
	cc->planes_base = (void **) xmalloc(k * sizeof(void *));
	for (j = 0; j < k; j++)
		cc->planes[j] = words_alloc(nwords, ((c + j) % 4) * 8, &cc->planes_base[j]);
	cc->inter = words_alloc(nwords * k, ((c / 4 + 1) % 4) * 8, &cc->inter_base);
	cc->bits_copy = (uint64 *) xmalloc(nwords * sizeof(uint64));
	cc->inter_copy = (uint64 *) xmalloc((Size) nwords * k * sizeof(uint64));
}

static void
case_free(CountCase *cc)
{
	uint32		j;

	free(cc->bits_base);
	for (j = 0; j < cc->k; j++)
		free(cc->planes_base[j]);
	free(cc->planes);
	free(cc->planes_base);
	free(cc->inter_base);
	free(cc->bits_copy);
	free(cc->inter_copy);
}

/* The INTERLEAVED planes, and copies of the inputs, from the STRIDED ones. */
static void
case_interleave(CountCase *cc)
{
	uint32		w;
	uint32		j;

	for (w = 0; w < cc->nwords; w++)
		for (j = 0; j < cc->k; j++)
			cc->inter[(Size) w * cc->k + j] = cc->planes[j][w];
	memcpy(cc->bits_copy, cc->bits, cc->nwords * sizeof(uint64));
	memcpy(cc->inter_copy, cc->inter, (Size) cc->nwords * cc->k * sizeof(uint64));
}

static long count_calls = 0;
static char phase_buf[160];

/*
 * Every implementation the build and the CPU have, both layouts and the
 * weighted sums, against want (counts) and want_sum; the inputs unchanged.
 * what names the case in a failure.
 */
static void
case_check(CountCase *cc, const uint32 *want, uint64 want_sum, const char *what)
{
	const char *phase = cur_phase;
	uint32	   *got = (uint32 *) xmalloc(Max(cc->k, 1) * sizeof(uint32));
	uint32		i;

	for (i = 0; i < NIMPLS; i++)
	{
		if (!lion_simd_force(impls[i]))
			continue;
		snprintf(phase_buf, sizeof(phase_buf), "%s: %u words, %u planes, %s; %s",
				 phase, cc->nwords, cc->k, what, lion_simd_name(impls[i]));
		cur_phase = phase_buf;
		memset(got, 0xA5, cc->k * sizeof(uint32));
		lion_planes_count_interleaved(cc->bits, cc->inter, cc->nwords, cc->k, got);
		CHECK(memcmp(got, want, cc->k * sizeof(uint32)) == 0,
			  "INTERLEAVED counts differ from the count a bit at a time");
		memset(got, 0x5A, cc->k * sizeof(uint32));
		lion_planes_count_strided(cc->bits, (const uint64 *const *) cc->planes,
								  cc->nwords, cc->k, got);
		CHECK(memcmp(got, want, cc->k * sizeof(uint32)) == 0,
			  "STRIDED counts differ from the count a bit at a time");
		if (cc->k <= LION_PLANES_MAX_BITS)
		{
			CHECK(lion_planes_dot_interleaved(cc->bits, cc->inter, cc->nwords, cc->k) == want_sum,
				  "an INTERLEAVED weighted sum is not the rows' values summed");
			CHECK(lion_planes_dot_strided(cc->bits, (const uint64 *const *) cc->planes,
										  cc->nwords, cc->k) == want_sum,
				  "a STRIDED weighted sum is not the rows' values summed");
		}
		count_calls += 4;
	}
	lion_simd_force(LION_SIMD_AUTO);
	CHECK(memcmp(cc->bits, cc->bits_copy, cc->nwords * sizeof(uint64)) == 0 &&
		  memcmp(cc->inter, cc->inter_copy, (Size) cc->nwords * cc->k * sizeof(uint64)) == 0,
		  "a kernel wrote to its inputs");
	cur_phase = phase;
	free(got);
}

static void
test_counts(void)
{
	static const uint32 ks[] = {1, 3, 4, 5, 32, 64};
	static const uint32 nws[] = {1, 3, 4, 24, 100, 512};
	uint32		a;
	uint32		b;
	long		ncases = 0;

	cur_phase = "counts";
	for (a = 0; a < lengthof(nws); a++)
	{
		for (b = 0; b < lengthof(ks); b++)
		{
			uint32		nwords = nws[a];
			uint32		k = ks[b];
			uint32	   *want = (uint32 *) xmalloc(k * sizeof(uint32));
			uint32		c;

			/*
			 * Each pattern for the bits twice, the planes' patterns cycling
			 * through all of them, the same one for every plane in a third
			 * of the cases.
			 */
			for (c = 0; c < 2 * NPATTERNS; c++)
			{
				CountCase cc;
				uint64		want_sum;
				char		what[32];
				uint32		j;

				case_alloc(&cc, nwords, k, c);
				fill_pattern(cc.bits, nwords, (int) (c % NPATTERNS), c * 5 + 1);
				for (j = 0; j < k; j++)
				{
					int			p = (c % 3 == 0) ? (int) ((c / 2) % NPATTERNS) :
						(int) ((c * 7 + j * 3) % NPATTERNS);

					fill_pattern(cc.planes[j], nwords, p, c + j * 11);
				}
				case_interleave(&cc);
				want_sum = ref_counts(cc.bits, cc.planes, nwords, k, want);
				snprintf(what, sizeof(what), "case %u", c);
				case_check(&cc, want, want_sum, what);
				ncases++;
				case_free(&cc);
			}
			free(want);
		}
	}
	printf("planes_test: counts: %ld inputs, %ld kernel calls\n", ncases, count_calls);
}

/*
 * One bit set in the bits, at each side of every word boundary and at both
 * ends, against random planes: plane j's count is its bit there.
 */
static void
test_counts_edges(void)
{
	static const uint32 ks[] = {1, 5, 64};
	static const uint32 nws[] = {1, 3, 24, 100, 512};
	uint32		a;
	uint32		b;

	cur_phase = "counts, a bit at every word boundary";
	for (a = 0; a < lengthof(nws); a++)
	{
		for (b = 0; b < lengthof(ks); b++)
		{
			uint32		nwords = nws[a];
			uint32		k = ks[b];
			uint32	   *want = (uint32 *) xmalloc(k * sizeof(uint32));
			CountCase	cc;
			uint32		w;
			uint32		j;

			case_alloc(&cc, nwords, k, a + b);
			for (j = 0; j < k; j++)
				fill_pattern(cc.planes[j], nwords, 6, 0);
			for (w = 0; w <= nwords; w++)
			{
				int			side;

				for (side = 0; side < 2; side++)
				{
					uint32		pos;
					uint64		want_sum = 0;
					char		what[32];

					/* the bit before boundary w, and the one after it */
					if ((side == 0 && w == 0) || (side == 1 && w == nwords))
						continue;
					pos = side == 0 ? w * 64 - 1 : w * 64;
					memset(cc.bits, 0, nwords * sizeof(uint64));
					cc.bits[pos >> 6] = UINT64CONST(1) << (pos & 63);
					for (j = 0; j < k; j++)
					{
						want[j] = bit_at(cc.planes[j], pos);
						want_sum += (uint64) want[j] << j;
					}
					case_interleave(&cc);
					snprintf(what, sizeof(what), "bit %u alone", pos);
					case_check(&cc, want, want_sum, what);
				}
			}
			case_free(&cc);
			free(want);
		}
	}
}

/*
 * Every bit set, at every word count up to 130: the most each byte count
 * adds up before VPSADBW, across every length of a run and of a tail.
 */
static void
test_counts_full(void)
{
	static const uint32 ks[] = {1, 2, 3, 4, 5, 7, 8};
	uint32		nwords;
	uint32		b;

	cur_phase = "counts, every bit set";
	for (nwords = 1; nwords <= 130; nwords++)
	{
		for (b = 0; b < lengthof(ks); b++)
		{
			uint32		k = ks[b];
			uint32	   *want = (uint32 *) xmalloc(k * sizeof(uint32));
			uint64		want_sum = 0;
			CountCase	cc;
			uint32		j;

			case_alloc(&cc, nwords, k, nwords + b);
			fill_pattern(cc.bits, nwords, 1, 0);
			for (j = 0; j < k; j++)
			{
				fill_pattern(cc.planes[j], nwords, 1, 0);
				want[j] = 64 * nwords;
				want_sum += (uint64) want[j] << j;
			}
			case_interleave(&cc);
			case_check(&cc, want, want_sum, "every bit set");
			case_free(&cc);
			free(want);
		}
	}

	/* none at all: nothing read, and no planes */
	{
		uint32		counts[3] = {7, 7, 7};
		uint64		one = 1;
		const uint64 *const planes[3] = {&one, &one, &one};
		uint32		i;

		for (i = 0; i < NIMPLS; i++)
		{
			if (!lion_simd_force(impls[i]))
				continue;
			lion_planes_count_interleaved(&one, &one, 0, 3, counts);
			CHECK(counts[0] == 0 && counts[1] == 0 && counts[2] == 0, "no words count 0");
			counts[0] = 7;
			lion_planes_count_strided(&one, planes, 0, 3, counts);
			CHECK(counts[0] == 0 && counts[1] == 0 && counts[2] == 0, "no words count 0");
			counts[0] = 7;
			lion_planes_count_interleaved(&one, &one, 1, 0, counts);
			lion_planes_count_strided(&one, planes, 1, 0, counts);
			CHECK(counts[0] == 7, "no planes write no count");
			CHECK(lion_planes_dot_strided(&one, planes, 1, 0) == 0 &&
				  lion_planes_dot_interleaved(&one, &one, 0, 1) == 0, "an empty sum");
		}
		lion_simd_force(LION_SIMD_AUTO);
	}
}

/* ---------- the transpose ---------- */

static void
ref_transpose(const uint64 *m, uint64 *t)
{
	uint32		r;
	uint32		c;

	memset(t, 0, 64 * sizeof(uint64));
	for (r = 0; r < 64; r++)
		for (c = 0; c < 64; c++)
			t[c] |= ((m[r] >> c) & 1) << r;
}

static void
test_transpose(void)
{
	uint64		m[64];
	uint64		t[64];
	uint64		orig[64];
	int			trial;
	uint32		r;
	uint32		c;

	cur_phase = "transpose";
	for (trial = 0; trial < 200; trial++)
	{
		for (r = 0; r < 64; r++)
		{
			int			kind = trial % 4;

			m[r] = kind == 0 ? rnd64() : kind == 1 ? rnd64() & rnd64() & rnd64() :
				kind == 2 ? ~(rnd64() & rnd64()) : (r == (uint32) trial % 64 ? ~UINT64CONST(0) : 0);
		}
		memcpy(orig, m, sizeof(m));
		ref_transpose(m, t);
		lion_planes_transpose64(m);
		CHECK(memcmp(m, t, sizeof(m)) == 0, "the transpose differs from the bit-at-a-time one");
		lion_planes_transpose64(m);
		CHECK(memcmp(m, orig, sizeof(m)) == 0, "the transpose is not its own inverse");
	}
	for (r = 0; r < 64; r++)
	{
		for (c = 0; c < 64; c++)
		{
			memset(m, 0, sizeof(m));
			m[r] = UINT64CONST(1) << c;
			lion_planes_transpose64(m);
			memset(t, 0, sizeof(t));
			t[c] = UINT64CONST(1) << r;
			CHECK(memcmp(m, t, sizeof(m)) == 0, "bit (r, c) does not go to (c, r)");
		}
	}
}

/* ---------- values and planes ---------- */

static uint64
width_mask(uint32 nbits)
{
	return nbits >= 64 ? ~UINT64CONST(0) : (UINT64CONST(1) << nbits) - 1;
}

/* The planes of values a bit at a time: plane j of the ref, word w. */
static void
ref_scatter(const uint64 *values, uint32 n, uint32 nbits, uint64 *const *planes)
{
	uint32		nwords = (n + 63) / 64;
	uint32		i;
	uint32		j;

	for (j = 0; j < nbits; j++)
		memset(planes[j], 0, nwords * sizeof(uint64));
	for (i = 0; i < n; i++)
		for (j = 0; j < nbits; j++)
			planes[j][i >> 6] |= ((values[i] >> j) & 1) << (i & 63);
}

/* A value of a random length, or 0, or all ones, with random bits past it. */
static uint64
rnd_value(uint32 nbits)
{
	uint32		kind = rnd_below(8);
	uint64		v;

	if (kind == 0)
		v = 0;
	else if (kind == 1)
		v = ~UINT64CONST(0);
	else
		v = rnd64() >> rnd_below(64);
	return v;
}

static long scatter_cases = 0;

/* The planes of n values of nbits, both ways, both layouts. */
static void
scatter_case(uint32 n, uint32 nbits)
{
	const char *phase = cur_phase;
	bool		ok_strided = true;
	bool		ok_inter = true;
	bool		back_strided = true;
	bool		back_inter = true;
	uint32		nwords = (n + 63) / 64;
	uint64		mask = width_mask(nbits);
	uint64	   *values = (uint64 *) xmalloc(n * sizeof(uint64));
	uint64	   *back = (uint64 *) xmalloc(n * sizeof(uint64));
	uint64	  **ref = (uint64 **) xmalloc(nbits * sizeof(uint64 *));
	uint64	  **strided = (uint64 **) xmalloc(nbits * sizeof(uint64 *));
	void	  **strided_base = (void **) xmalloc(nbits * sizeof(void *));
	void	   *inter_base;
	uint64	   *inter = words_alloc(nwords * nbits, (n % 4) * 8, &inter_base);
	uint32		i;
	uint32		j;
	uint32		w;

	for (i = 0; i < n; i++)
		values[i] = rnd_value(nbits);
	for (j = 0; j < nbits; j++)
	{
		ref[j] = (uint64 *) xmalloc(nwords * sizeof(uint64));
		strided[j] = words_alloc(nwords, ((n + j) % 4) * 8, &strided_base[j]);
		memset(strided[j], 0xA5, nwords * sizeof(uint64));
	}
	memset(inter, 0x5A, (Size) nwords * nbits * sizeof(uint64));
	ref_scatter(values, n, nbits, ref);

	lion_planes_scatter_strided(values, n, nbits, strided);
	lion_planes_scatter_interleaved(values, n, nbits, inter);
	for (j = 0; j < nbits; j++)
		for (w = 0; w < nwords; w++)
		{
			ok_strided &= strided[j][w] == ref[j][w];
			ok_inter &= inter[(Size) w * nbits + j] == ref[j][w];
		}

	/* back, after the padding past n is filled with ones */
	if (n % 64 != 0)
		for (j = 0; j < nbits; j++)
		{
			strided[j][nwords - 1] |= ~UINT64CONST(0) << (n % 64);
			inter[(Size) (nwords - 1) * nbits + j] |= ~UINT64CONST(0) << (n % 64);
		}
	memset(back, 0xEE, n * sizeof(uint64));
	lion_planes_gather_strided((const uint64 *const *) strided, n, nbits, back);
	for (i = 0; i < n; i++)
		back_strided &= back[i] == (values[i] & mask);
	memset(back, 0xEE, n * sizeof(uint64));
	lion_planes_gather_interleaved(inter, n, nbits, back);
	for (i = 0; i < n; i++)
		back_inter &= back[i] == (values[i] & mask);

	snprintf(phase_buf, sizeof(phase_buf), "%s: %u values of %u bits", phase, n, nbits);
	cur_phase = phase_buf;
	CHECK(ok_strided, "STRIDED scatter differs from the bit-at-a-time planes");
	CHECK(ok_inter, "INTERLEAVED scatter differs from the bit-at-a-time planes");
	CHECK(back_strided, "STRIDED gather is not the values, to their width");
	CHECK(back_inter, "INTERLEAVED gather is not the values, to their width");
	cur_phase = phase;

	for (j = 0; j < nbits; j++)
	{
		free(ref[j]);
		free(strided_base[j]);
	}
	free(ref);
	free(strided);
	free(strided_base);
	free(inter_base);
	free(values);
	free(back);
	scatter_cases++;
}

static void
test_scatter(void)
{
	static const uint32 ns[] = {1, 2, 63, 64, 65, 127, 128, 129, 200, 1000};
	static const uint32 wide_bits[] = {1, 2, 3, 4, 5, 8, 16, 17, 31, 32, 33, 63, 64};
	uint32		nbits;
	uint32		a;

	cur_phase = "scatter and gather";
	for (nbits = 1; nbits <= 64; nbits++)
		for (a = 0; a < lengthof(ns); a++)
			scatter_case(ns[a], nbits);
	/* a container's rows, 32768, and a ragged count past it */
	for (a = 0; a < lengthof(wide_bits); a++)
	{
		scatter_case(32768, wide_bits[a]);
		scatter_case(4096 + 17, wide_bits[a]);
	}

	/* gather, then scatter: the planes again, but the padding cleared */
	{
		uint32		n = 100;
		uint32		nbits_g = 7;
		uint64		planes[2 * 7];
		uint64		again[2 * 7];
		uint64		values[100];
		uint32		i;
		bool		ok = true;

		for (i = 0; i < lengthof(planes); i++)
			planes[i] = rnd64();
		lion_planes_gather_interleaved(planes, n, nbits_g, values);
		lion_planes_scatter_interleaved(values, n, nbits_g, again);
		for (i = 0; i < lengthof(planes); i++)
			ok &= again[i] == (i < nbits_g ? planes[i] :
							   planes[i] & (~UINT64CONST(0) >> (128 - n)));
		CHECK(ok, "gather then scatter is not the planes, less the padding");
	}
	printf("planes_test: scatter and gather: %ld cases\n", scatter_cases);
}

/* ---------- lionvec ---------- */

/*
 * The INTERLEAVED weighted sum of four planes against lionvec_plane_dot(),
 * every implementation of each against every one of the other, at every
 * word count an index takes, and the planes of a query's 4-bit codes
 * against those lionvec_query_init() builds.
 */
#define TEST_MAX_DIMSP		8128

static void
test_lionvec(void)
{
	uint32		nwords;
	int			which;
	uint32		i;
	long		dots = 0;
	int			dimsp;
	long		queries = 0;

	cur_phase = "lionvec";
	lionvec_tables_init();
	for (nwords = 1; nwords <= TEST_MAX_DIMSP / 64; nwords++)
	{
		void	   *bits_base;
		void	   *planes_base;
		uint64	   *bits = words_alloc(nwords, (nwords % 4) * 8, &bits_base);
		uint64	   *planes = words_alloc(nwords * LIONVEC_QBITS, ((nwords / 4) % 4) * 8, &planes_base);
		int			trial;

		for (trial = 0; trial < 3; trial++)
		{
			uint32		w;

			for (w = 0; w < nwords; w++)
				bits[w] = trial == 2 ? ~UINT64CONST(0) : rnd64() & (trial == 0 ? rnd64() : ~UINT64CONST(0));
			for (w = 0; w < nwords * LIONVEC_QBITS; w++)
				planes[w] = trial == 2 ? ~UINT64CONST(0) : rnd64();
			for (which = 0; which <= LIONVEC_KERNELS_MAX; which++)
			{
				uint32		theirs;

				if (!lionvec_kernel_force(which))
					continue;
				theirs = lionvec_plane_dot(bits, planes, (int) nwords);
				for (i = 0; i < NIMPLS; i++)
				{
					if (!lion_simd_force(impls[i]))
						continue;
					CHECK(lion_planes_dot_interleaved(bits, planes, nwords, LIONVEC_QBITS) ==
						  (uint64) theirs,
						  "the weighted sum of four interleaved planes is not lionvec_plane_dot()");
					dots++;
				}
			}
		}
		free(bits_base);
		free(planes_base);
	}
	lionvec_kernels_init();
	lion_simd_force(LION_SIMD_AUTO);

	/* the query's planes */
	for (dimsp = 64; dimsp <= TEST_MAX_DIMSP; dimsp += (dimsp < 512 ? 64 : 960))
	{
		int			metric;

		for (metric = 1; metric <= LIONVEC_NSTRATEGIES; metric++)
		{
			LionVecQuery q;
			float	   *mean = (float *) xmalloc(dimsp * sizeof(float));
			float	   *x = (float *) xmalloc(dimsp * sizeof(float));
			uint64	   *codes = (uint64 *) xmalloc(dimsp * sizeof(uint64));
			uint64	   *back = (uint64 *) xmalloc(dimsp * sizeof(uint64));
			uint64	   *planes = (uint64 *) xmalloc((dimsp / 64) * LIONVEC_QBITS * sizeof(uint64));
			int			levels = (1 << LIONVEC_QBITS) - 1;
			int			d;

			q.dimsp = dimsp;
			q.qc = (float *) xmalloc(dimsp * sizeof(float));
			q.planes = (uint64 *) xmalloc((dimsp / 64) * LIONVEC_QBITS * sizeof(uint64));
			for (d = 0; d < dimsp; d++)
			{
				/* sums of uniforms: Gaussian enough, and some of one sign */
				double		g = 0.0;
				int			s;

				for (s = 0; s < 4; s++)
					g += (double) (rnd64() >> 11) / 9007199254740992.0 - 0.5;
				mean[d] = (float) (metric == LIONVEC_COSINE ? 0.0 : g * 0.3);
				x[d] = (float) (mean[d] + g * (d % 7 == 0 ? 3.0 : 1.0));
			}
			lionvec_query_init(&q, metric, metric == LIONVEC_COSINE ? NULL : mean, x, 3.0);

			/* each coordinate's code, as lionvec_query_init() rounds it */
			for (d = 0; d < dimsp; d++)
			{
				double		c = q.qc[d];
				double		t = (c - q.qlo) / q.qdelta + 0.5;
				int			u = t >= levels ? levels : (t > 0.0 ? (int) t : 0);

				codes[d] = (uint64) u;
			}
			lion_planes_scatter_interleaved(codes, (uint32) dimsp, LIONVEC_QBITS, planes);
			CHECK(memcmp(planes, q.planes, (dimsp / 64) * LIONVEC_QBITS * sizeof(uint64)) == 0,
				  "the scatter of a query's codes is not lionvec_query_init()'s planes");
			lion_planes_gather_interleaved(q.planes, (uint32) dimsp, LIONVEC_QBITS, back);
			CHECK(memcmp(back, codes, dimsp * sizeof(uint64)) == 0,
				  "the gather of lionvec_query_init()'s planes is not the query's codes");
			queries++;

			free(q.qc);
			free(q.planes);
			free(mean);
			free(x);
			free(codes);
			free(back);
			free(planes);
		}
	}
	printf("planes_test: lionvec: %ld weighted sums against lionvec_plane_dot(), %ld queries' planes\n",
		   dots, queries);
}

/* ---------- timings (--bench) ---------- */

static volatile uint64 bench_sink;

static double
now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double) ts.tv_sec * 1e9 + (double) ts.tv_nsec;
}

static int
cmp_double(const void *a, const void *b)
{
	double		x = *(const double *) a;
	double		y = *(const double *) b;

	return x < y ? -1 : x > y;
}

#define BENCH_ROUNDS	9

/* What a bench times: one of the calls below, its arguments in the struct. */
typedef struct Bench
{
	int			what;
	const uint64 *bits;
	const uint64 *inter;
	const uint64 *const *strided;
	uint32		nwords;
	uint32		k;
	uint32	   *counts;
	uint64	   *values;
	uint64	   *m;
} Bench;

enum
{
	B_INTER, B_STRIDED, B_STRIDED_EACH, B_LIONVEC, B_DOT4, B_TRANSPOSE,
	B_SCATTER, B_GATHER
};

static void
bench_call(const Bench *b)
{
	uint32		j;

	switch (b->what)
	{
		case B_INTER:
			lion_planes_count_interleaved(b->bits, b->inter, b->nwords, b->k, b->counts);
			bench_sink += b->counts[0];
			break;
		case B_STRIDED:
			lion_planes_count_strided(b->bits, b->strided, b->nwords, b->k, b->counts);
			bench_sink += b->counts[0];
			break;
		case B_STRIDED_EACH:
			for (j = 0; j < b->k; j++)
				lion_planes_count_strided(b->bits, b->strided + j, b->nwords, 1, b->counts + j);
			bench_sink += b->counts[0];
			break;
		case B_LIONVEC:
			bench_sink += lionvec_plane_dot(b->bits, b->inter, (int) b->nwords);
			break;
		case B_DOT4:
			bench_sink += lion_planes_dot_interleaved(b->bits, b->inter, b->nwords, 4);
			break;
		case B_TRANSPOSE:
			lion_planes_transpose64(b->m);
			bench_sink += b->m[0];
			break;
		case B_SCATTER:
			lion_planes_scatter_strided(b->values, b->nwords * 64, b->k,
										(uint64 *const *) b->strided);
			bench_sink += b->strided[0][0];
			break;
		case B_GATHER:
			lion_planes_gather_strided(b->strided, b->nwords * 64, b->k, b->values);
			bench_sink += b->values[0];
			break;
	}
}

/* ns a call: the median of BENCH_ROUNDS rounds of about 2 ms each. */
static double
bench_time(const Bench *b)
{
	double		rounds[BENCH_ROUNDS];
	double		t0;
	double		once;
	long		n;
	long		i;
	int			r;

	t0 = now_ns();
	bench_call(b);
	bench_call(b);
	once = Max((now_ns() - t0) / 2, 1.0);
	n = Max(1, (long) (2e6 / once));
	for (r = 0; r < BENCH_ROUNDS; r++)
	{
		t0 = now_ns();
		for (i = 0; i < n; i++)
			bench_call(b);
		rounds[r] = (now_ns() - t0) / n;
	}
	qsort(rounds, BENCH_ROUNDS, sizeof(double), cmp_double);
	return rounds[BENCH_ROUNDS / 2];
}

static void
bench(void)
{
	static const uint32 ks[] = {4, 32, 64};
	const uint32 nwords = 512;
	uint64	   *bits = (uint64 *) xmalloc(nwords * sizeof(uint64));
	uint64	   *inter = (uint64 *) xmalloc((Size) nwords * 64 * sizeof(uint64));
	uint64	   *strided[64];
	uint64	   *values = (uint64 *) xmalloc((Size) nwords * 64 * sizeof(uint64));
	uint32		counts[64];
	uint64		m[64];
	Bench		b;
	uint32		i;
	uint32		a;
	uint32		j;
	FILE	   *f;
	char		line[256];

	f = fopen("/proc/cpuinfo", "r");
	if (f != NULL)
	{
		while (fgets(line, sizeof(line), f) != NULL)
			if (strncmp(line, "model name", 10) == 0)
			{
				printf("cpu: %s", strchr(line, ':') + 2);
				break;
			}
		fclose(f);
	}
	printf("best implementation: %s; ns a call, the median of %d rounds\n",
		   lion_simd_name(lion_simd_best()), BENCH_ROUNDS);

	for (i = 0; i < nwords; i++)
		bits[i] = rnd64();
	for (i = 0; i < nwords * 64; i++)
		inter[i] = rnd64();
	for (j = 0; j < 64; j++)
	{
		strided[j] = (uint64 *) xmalloc(nwords * sizeof(uint64));
		for (i = 0; i < nwords; i++)
			strided[j][i] = rnd64();
	}
	for (i = 0; i < 64; i++)
		m[i] = rnd64();
	memset(&b, 0, sizeof(b));
	b.bits = bits;
	b.inter = inter;
	b.strided = (const uint64 *const *) strided;
	b.counts = counts;
	b.values = values;
	b.m = m;

	printf("\n512 words (a BITSET) x k planes:\n");
	printf("%-9s %-24s %10s %10s %10s\n", "impl", "call", "k = 4", "k = 32", "k = 64");
	for (i = 0; i < NIMPLS; i++)
	{
		static const int whats[] = {B_STRIDED, B_INTER, B_STRIDED_EACH};
		static const char *const names[] = {"strided", "interleaved", "strided, a plane a call"};
		uint32		c;

		if (!lion_simd_force(impls[i]))
			continue;
		for (c = 0; c < lengthof(whats); c++)
		{
			printf("%-9s %-24s", lion_simd_name(impls[i]), names[c]);
			for (a = 0; a < lengthof(ks); a++)
			{
				b.what = whats[c];
				b.nwords = nwords;
				b.k = ks[a];
				printf(" %10.1f", bench_time(&b));
			}
			printf("\n");
		}
	}

	printf("\nlionvec's plane 1 at 1536 dimensions (24 words x 4 interleaved planes):\n");
	for (i = 0; i < NIMPLS; i++)
	{
		int			which = impls[i] - LION_SIMD_PORTABLE;	/* lionvec's numbering */
		double		ours;
		double		theirs = -1;

		if (!lion_simd_force(impls[i]))
			continue;
		b.what = B_DOT4;
		b.nwords = 24;
		ours = bench_time(&b);
		if (lionvec_kernel_force(which))
		{
			b.what = B_LIONVEC;
			theirs = bench_time(&b);
		}
		printf("%-9s lion_planes_dot_interleaved() %6.1f   lionvec_plane_dot() %6.1f\n",
			   lion_simd_name(impls[i]), ours, theirs);
	}
	lionvec_kernels_init();
	lion_simd_force(LION_SIMD_AUTO);

	printf("\nthe transpose, and a container's 32768 values to and from planes (strided):\n");
	b.what = B_TRANSPOSE;
	printf("lion_planes_transpose64()              %10.1f\n", bench_time(&b));
	for (a = 0; a < 4; a++)
	{
		static const uint32 bitsof[] = {4, 16, 32, 64};

		for (i = 0; i < nwords * 64; i++)
			values[i] = rnd64() & width_mask(bitsof[a]);
		b.k = bitsof[a];
		b.nwords = nwords;
		b.what = B_SCATTER;
		printf("scatter, %2u bits                       %10.1f\n", bitsof[a], bench_time(&b));
		b.what = B_GATHER;
		printf("gather, %2u bits                        %10.1f\n", bitsof[a], bench_time(&b));
	}

	for (j = 0; j < 64; j++)
		free(strided[j]);
	free(bits);
	free(inter);
	free(values);
}

int
main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "--bench") == 0)
	{
		bench();
		return 0;
	}

	test_choice();
	test_counts();
	test_counts_edges();
	test_counts_full();
	test_transpose();
	test_scatter();
	test_lionvec();

	printf("planes_test: %ld checks, %ld failed\n", nchecks, nfail);
	if (nfail == 0)
		printf("ALL TESTS PASSED\n");
	return nfail == 0 ? 0 : 1;
}
