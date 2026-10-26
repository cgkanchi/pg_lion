/*-------------------------------------------------------------------------
 * lion_planes.c
 *	  Bit-plane arithmetic, and the extension's choice of SIMD
 *	  implementation (lion_planes.h, DESIGN.md §50).
 *
 *	  Nothing here allocates: every array is the caller's, of the length
 *	  lion_planes.h gives, so the same code runs in a backend and in
 *	  test/unit/planes_test.c, whose buffers are exact-size heap allocations
 *	  that `make unit SANITIZE=1` checks to the byte.
 *-------------------------------------------------------------------------
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "c.h"
#endif

#include <string.h>

#include "port/pg_bitutils.h"

#include "lion_planes.h"

#ifdef LION_HAVE_X86_SIMD
#include <immintrin.h>
#endif

/*
 * pg_always_inline is the newer spelling of pg_attribute_always_inline, and
 * master's only one; released 16.x and 17.x minors have only the older.
 */
#ifndef pg_always_inline
#define pg_always_inline pg_attribute_always_inline
#endif

/* ----------------------------------------------------------------
 *						the implementations
 *
 * One choice for the whole extension: the container library's whole-bitset
 * kernels (lion_container.c), lionvec's (lion_vec_math.c, whose plane-1
 * kernel is lion_planes_dot_interleaved() below) and the kernels below each
 * dispatch on lion_simd_current(), which is resolved at the first call - as
 * PostgreSQL's own pg_popcount64 resolves at its first - to the best
 * implementation the build has and the CPU runs.  A FRONTEND build may force
 * another, for the unit tests, and every kernel follows at its next call.
 *
 *	PORTABLE	plain C, every build
 *	POPCNT		x86-64: the POPCNT instruction a word at a time
 *	AVX2		x86-64: 32 bytes at a time, each byte's popcount looked up a
 *				nibble at a time with VPSHUFB (Mula)
 *	AVX512		x86-64: AVX2's, popcounts by VPOPCNTDQ (Ice Lake, Zen 4 and
 *				on) on 256-bit vectors; AVX-512F and VL with it, for the
 *				256-bit forms and their masked loads.  No 512-bit
 *				instruction runs, for which some cores lower their clock
 *				(DESIGN.md §44.3, "Kernels")
 *
 * A module without a kernel of an implementation runs its best below it:
 * the container library has no AVX-512 kernel and runs its AVX2 one, and
 * lionvec runs its AVX2 ones but for plane 1, which is here.
 * ----------------------------------------------------------------
 */

LionSimdImpl lion_simd_in_use = LION_SIMD_AUTO;

static const char *const lion_simd_names[LION_SIMD_NIMPLS] = {
	"auto", "portable", "popcnt", "avx2", "avx512"
};

const char *
lion_simd_name(LionSimdImpl impl)
{
	if ((int) impl < 0 || impl >= LION_SIMD_NIMPLS)
		return "?";
	return lion_simd_names[impl];
}

bool
lion_simd_has(LionSimdImpl impl)
{
#ifdef LION_HAVE_X86_SIMD
	/* a no-op after libgcc's constructor, which has run by any first call */
	__builtin_cpu_init();
#endif
	switch (impl)
	{
		case LION_SIMD_AUTO:
		case LION_SIMD_PORTABLE:
			return true;
#ifdef LION_HAVE_X86_SIMD
		case LION_SIMD_POPCNT:
			return __builtin_cpu_supports("popcnt");
		case LION_SIMD_AVX2:
			return __builtin_cpu_supports("avx2");
#ifdef LION_HAVE_X86_AVX512
		case LION_SIMD_AVX512:
			return __builtin_cpu_supports("avx2") &&
				__builtin_cpu_supports("avx512f") &&
				__builtin_cpu_supports("avx512vl") &&
				__builtin_cpu_supports("avx512vpopcntdq");
#endif
#endif
		default:
			return false;
	}
}

LionSimdImpl
lion_simd_best(void)
{
	int			impl;

	for (impl = LION_SIMD_AVX512; impl > LION_SIMD_PORTABLE; impl--)
	{
		if (lion_simd_has((LionSimdImpl) impl))
			return (LionSimdImpl) impl;
	}
	return LION_SIMD_PORTABLE;
}

LionSimdImpl
lion_simd_resolve(void)
{
	if (lion_simd_in_use == LION_SIMD_AUTO)
		lion_simd_in_use = lion_simd_best();
	return lion_simd_in_use;
}

bool
lion_simd_f16c(void)
{
#ifdef LION_HAVE_X86_F16C
	__builtin_cpu_init();
	return __builtin_cpu_supports("f16c");
#else
	return false;
#endif
}

#ifdef FRONTEND
bool
lion_simd_force(LionSimdImpl impl)
{
	if (impl == LION_SIMD_AUTO)
	{
		lion_simd_in_use = lion_simd_best();
		return true;
	}
	if (!lion_simd_has(impl))
		return false;
	lion_simd_in_use = impl;
	return true;
}
#endif

/* ----------------------------------------------------------------
 *						popcount per plane
 *
 * counts[j] = sum over w of popcount(bits[w] & plane_j[w]), j < k.  Every
 * kernel takes the planes four at a time, a group, its four sums in
 * registers through one pass over the words, and the last k % 4 as a
 * shorter group; bits is read once a pass, from L1 after the first.
 *
 *	portable,	a word at a time: four sums, one a plane, so that neither
 *	POPCNT		their adds nor POPCNT's false dependency on its destination
 *				(Intel, before Cannon Lake) chain the words
 *	AVX2		INTERLEAVED, lionvec's: a word's four planes are one vector,
 *				ANDed with the bits' word broadcast, each byte counted with
 *				VPSHUFB, lane j counting plane j; the byte counts add up
 *				over 31 words, 248 a byte at most, before VPSADBW sums each
 *				lane's.  Up to four groups a pass, a word's 16 planes, two
 *				cache lines: a group a pass read each line of 64 planes
 *				twice, and took a fifth longer.  STRIDED: four words of the
 *				bits against four of each of the group's planes, one vector
 *				a plane, byte counts over 31 vectors before VPSADBW, as
 *				lion_container.c counts an AND
 *	AVX512		AVX2's, each lane's word counted whole by VPOPCNTDQ into its
 *				64-bit sum, with no byte counts to add up
 *
 * Each group's sums are named registers, not an array indexed in a loop:
 * GCC kept such an array in memory, and four planes took longer than four
 * calls of one.
 *
 * nwords and k are any: the last nwords % 4 words of a STRIDED vector pass,
 * and the last k % 4 planes of an INTERLEAVED one where their vector would
 * reach past the planes, are read through zero-padded copies (AVX2) or
 * masked loads (AVX-512), never past the caller's arrays; those last
 * planes' vector otherwise reads the next word's planes into its spare
 * lanes, whose sums are dropped.
 * ----------------------------------------------------------------
 */

/*
 * lionvec's query planes, four interleaved: every INTERLEAVED kernel runs a
 * pass compiled for that stride, and the weighted sum one of its own
 * (planes_dot4_*() below).
 */
#define PLANES_K_QUERY		4

/* The word-at-a-time popcount: POPCNT's own when hw, else lion_popcount64(). */
#ifdef LION_HAVE_X86_SIMD
#define planes_popcount(hw, w) \
	((hw) ? (uint64) __builtin_popcountll(w) : (uint64) lion_popcount64(w))
#else
#define planes_popcount(hw, w)	((uint64) lion_popcount64(w))
#endif

/*
 * One group of r planes (1 .. 4), a word at a time: plane i's word w is
 * p[i][w * step].  r, step and hw are constants wherever this is inlined.
 */
static pg_always_inline void
planes_group_words(const uint64 *bits, const uint64 *const *p, uint32 step,
				   uint32 nwords, uint32 r, bool hw, uint32 *counts)
{
	const uint64 *p0 = p[0];
	const uint64 *p1 = r > 1 ? p[1] : NULL;
	const uint64 *p2 = r > 2 ? p[2] : NULL;
	const uint64 *p3 = r > 3 ? p[3] : NULL;
	uint64		s0 = 0;
	uint64		s1 = 0;
	uint64		s2 = 0;
	uint64		s3 = 0;
	uint32		w;

	for (w = 0; w < nwords; w++)
	{
		uint64		b = bits[w];
		Size		at = (Size) w * step;

		s0 += planes_popcount(hw, b & p0[at]);
		if (r > 1)
			s1 += planes_popcount(hw, b & p1[at]);
		if (r > 2)
			s2 += planes_popcount(hw, b & p2[at]);
		if (r > 3)
			s3 += planes_popcount(hw, b & p3[at]);
	}
	counts[0] = (uint32) s0;
	if (r > 1)
		counts[1] = (uint32) s1;
	if (r > 2)
		counts[2] = (uint32) s2;
	if (r > 3)
		counts[3] = (uint32) s3;
}

/*
 * Every group, a word at a time: of planes, INTERLEAVED, or of strided's
 * arrays.  interleaved and hw are constants wherever this is inlined.
 */
static pg_always_inline void
planes_count_words(const uint64 *bits, const uint64 *planes,
				   const uint64 *const *strided, bool interleaved,
				   uint32 nwords, uint32 k, bool hw, uint32 *counts)
{
	uint32		step = interleaved ? k : 1;
	uint32		j0;

	for (j0 = 0; j0 < k; j0 += 4)
	{
		const uint64 *p[4];
		uint32		r = Min(4, k - j0);
		uint32		i;

		for (i = 0; i < r; i++)
			p[i] = interleaved ? planes + j0 + i : strided[j0 + i];
		switch (r)
		{
			case 4:
				planes_group_words(bits, p, step, nwords, 4, hw, counts + j0);
				break;
			case 3:
				planes_group_words(bits, p, step, nwords, 3, hw, counts + j0);
				break;
			case 2:
				planes_group_words(bits, p, step, nwords, 2, hw, counts + j0);
				break;
			default:
				planes_group_words(bits, p, step, nwords, 1, hw, counts + j0);
				break;
		}
	}
}

static void
planes_interleaved_portable(const uint64 *bits, const uint64 *planes,
							uint32 nwords, uint32 k, uint32 *counts)
{
	if (k == PLANES_K_QUERY)
		planes_count_words(bits, planes, NULL, true, nwords, PLANES_K_QUERY, false, counts);
	else
		planes_count_words(bits, planes, NULL, true, nwords, k, false, counts);
}

static void
planes_strided_portable(const uint64 *bits, const uint64 *const *planes,
						uint32 nwords, uint32 k, uint32 *counts)
{
	planes_count_words(bits, NULL, planes, false, nwords, k, false, counts);
}

#ifdef LION_HAVE_X86_SIMD

/* ---------- POPCNT ---------- */

static LION_TARGET("popcnt") void
planes_interleaved_popcnt(const uint64 *bits, const uint64 *planes,
						  uint32 nwords, uint32 k, uint32 *counts)
{
	if (k == PLANES_K_QUERY)
		planes_count_words(bits, planes, NULL, true, nwords, PLANES_K_QUERY, true, counts);
	else
		planes_count_words(bits, planes, NULL, true, nwords, k, true, counts);
}

static LION_TARGET("popcnt") void
planes_strided_popcnt(const uint64 *bits, const uint64 *const *planes,
					  uint32 nwords, uint32 k, uint32 *counts)
{
	planes_count_words(bits, NULL, planes, false, nwords, k, true, counts);
}

/* ---------- AVX2 ---------- */

/* The popcount of each byte of v, 0 .. 8: two nibbles looked up (Mula). */
static pg_always_inline LION_TARGET("avx2") __m256i
planes_avx2_byte_counts(__m256i v)
{
	const __m256i lut = _mm256_setr_epi8(0, 1, 1, 2, 1, 2, 2, 3,
										 1, 2, 2, 3, 2, 3, 3, 4,
										 0, 1, 1, 2, 1, 2, 2, 3,
										 1, 2, 2, 3, 2, 3, 3, 4);
	const __m256i nibble = _mm256_set1_epi8(0x0f);
	__m256i		lo = _mm256_and_si256(v, nibble);
	__m256i		hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), nibble);

	return _mm256_add_epi8(_mm256_shuffle_epi8(lut, lo),
						   _mm256_shuffle_epi8(lut, hi));
}

/* bytes plus the byte counts of b AND the four words at p */
static pg_always_inline LION_TARGET("avx2") __m256i
planes_avx2_add(__m256i bytes, __m256i b, const uint64 *p)
{
	return _mm256_add_epi8(bytes,
						   planes_avx2_byte_counts(_mm256_and_si256(b,
																	_mm256_loadu_si256((const __m256i *) p))));
}

/* The four 64-bit lanes of v added up. */
static pg_always_inline LION_TARGET("avx2") uint64
planes_avx2_hsum(__m256i v)
{
	__m128i		s = _mm_add_epi64(_mm256_castsi256_si128(v),
								  _mm256_extracti128_si256(v, 1));

	return (uint64) _mm_cvtsi128_si64(s) + (uint64) _mm_extract_epi64(s, 1);
}

/* The lanes of v, four 64-bit sums, to counts[0 .. r). */
static pg_always_inline LION_TARGET("avx2") void
planes_avx2_lanes_out(__m256i v, uint32 r, uint32 *counts)
{
	uint64		s[4];
	uint32		i;

	_mm256_storeu_si256((__m256i *) s, v);
	for (i = 0; i < r; i++)
		counts[i] = (uint32) s[i];
}

/* The four 64-bit lanes of v weighed, lane j by 2^j: lionvec's sum. */
static pg_always_inline LION_TARGET("avx2") uint64
planes_avx2_weigh4(__m256i v)
{
	uint64		s[4];

	_mm256_storeu_si256((__m256i *) s, v);
	return s[0] + 2 * s[1] + 4 * s[2] + 8 * s[3];
}

/* The vectors a byte count adds up before VPSADBW: 31 of at most 8, 248. */
#define PLANES_AVX2_RUN		31

/*
 * INTERLEAVED, g full groups at once (1 .. 4), planes j0 .. j0 + 4 g - 1: a
 * word's 4 g planes, two cache lines at g = 4, in g vectors against the
 * bits' word broadcast, vector i's lane l counting plane j0 + 4 i + l.  A
 * full group's vector never reaches past the planes.  With counts NULL, of
 * one group, its four counts weighed are returned instead (lionvec's sum).
 */
static pg_always_inline LION_TARGET("avx2") uint64
planes_interleaved_groups_avx2(const uint64 *bits, const uint64 *planes,
							   uint32 nwords, uint32 k, uint32 j0, uint32 g,
							   uint32 *counts)
{
	const __m256i zero = _mm256_setzero_si256();
	__m256i		s0 = zero;
	__m256i		s1 = zero;
	__m256i		s2 = zero;
	__m256i		s3 = zero;
	uint32		w = 0;

	while (w < nwords)
	{
		uint32		end = Min(nwords, w + PLANES_AVX2_RUN);
		__m256i		c0 = zero;
		__m256i		c1 = zero;
		__m256i		c2 = zero;
		__m256i		c3 = zero;

		for (; w < end; w++)
		{
			__m256i		b = _mm256_set1_epi64x((long long) bits[w]);
			const uint64 *at = planes + (Size) w * k + j0;

			c0 = planes_avx2_add(c0, b, at);
			if (g > 1)
				c1 = planes_avx2_add(c1, b, at + 4);
			if (g > 2)
				c2 = planes_avx2_add(c2, b, at + 8);
			if (g > 3)
				c3 = planes_avx2_add(c3, b, at + 12);
		}
		s0 = _mm256_add_epi64(s0, _mm256_sad_epu8(c0, zero));
		if (g > 1)
			s1 = _mm256_add_epi64(s1, _mm256_sad_epu8(c1, zero));
		if (g > 2)
			s2 = _mm256_add_epi64(s2, _mm256_sad_epu8(c2, zero));
		if (g > 3)
			s3 = _mm256_add_epi64(s3, _mm256_sad_epu8(c3, zero));
	}
	if (counts == NULL)
		return planes_avx2_weigh4(s0);
	planes_avx2_lanes_out(s0, 4, counts);
	if (g > 1)
		planes_avx2_lanes_out(s1, 4, counts + 4);
	if (g > 2)
		planes_avx2_lanes_out(s2, 4, counts + 8);
	if (g > 3)
		planes_avx2_lanes_out(s3, 4, counts + 12);
	return 0;
}

/*
 * INTERLEAVED, the last r planes (1 .. 3), from j0: a vector of four words
 * from each word's plane j0, through a zero-padded copy where that reaches
 * past the planes; the lanes past r count the next word's planes, or the
 * padding, and are dropped.
 */
static pg_always_inline LION_TARGET("avx2") void
planes_interleaved_last_avx2(const uint64 *bits, const uint64 *planes,
							 uint32 nwords, uint32 k, uint32 j0, uint32 r,
							 uint32 *counts)
{
	const __m256i zero = _mm256_setzero_si256();
	Size		total = (Size) nwords * k;
	__m256i		sum = zero;
	uint32		w = 0;

	while (w < nwords)
	{
		uint32		end = Min(nwords, w + PLANES_AVX2_RUN);
		__m256i		bytes = zero;

		for (; w < end; w++)
		{
			__m256i		b = _mm256_set1_epi64x((long long) bits[w]);
			Size		at = (Size) w * k + j0;

			if (at + 4 <= total)
				bytes = planes_avx2_add(bytes, b, planes + at);
			else
			{
				uint64		pad[4] = {0, 0, 0, 0};

				memcpy(pad, planes + at, (total - at) * sizeof(uint64));
				bytes = planes_avx2_add(bytes, b, pad);
			}
		}
		sum = _mm256_add_epi64(sum, _mm256_sad_epu8(bytes, zero));
	}
	planes_avx2_lanes_out(sum, r, counts);
}

static LION_TARGET("avx2") void
planes_interleaved_avx2(const uint64 *bits, const uint64 *planes,
						uint32 nwords, uint32 k, uint32 *counts)
{
	uint32		j0 = 0;

	if (k == PLANES_K_QUERY)
	{
		planes_interleaved_groups_avx2(bits, planes, nwords, PLANES_K_QUERY, 0, 1, counts);
		return;
	}

	for (; j0 + 16 <= k; j0 += 16)
		planes_interleaved_groups_avx2(bits, planes, nwords, k, j0, 4, counts + j0);
	switch ((k - j0) / 4)
	{
		case 3:
			planes_interleaved_groups_avx2(bits, planes, nwords, k, j0, 3, counts + j0);
			break;
		case 2:
			planes_interleaved_groups_avx2(bits, planes, nwords, k, j0, 2, counts + j0);
			break;
		case 1:
			planes_interleaved_groups_avx2(bits, planes, nwords, k, j0, 1, counts + j0);
			break;
		default:
			break;
	}
	j0 = k - k % 4;
	if (j0 < k)
		planes_interleaved_last_avx2(bits, planes, nwords, k, j0, k - j0, counts + j0);
}

/* p's words w .. w + n - 1, n < 4, and zeros: a vector */
static pg_always_inline LION_TARGET("avx2") __m256i
planes_avx2_padded(const uint64 *p, uint32 n)
{
	uint64		pad[4] = {0, 0, 0, 0};

	memcpy(pad, p, n * sizeof(uint64));
	return _mm256_loadu_si256((const __m256i *) pad);
}

/*
 * STRIDED, one group of r planes (1 .. 4): four words of the bits against
 * four of each plane, one byte count a plane, VPSADBW every PLANES_AVX2_RUN
 * vectors; the last nwords % 4 words through zero-padded copies.
 */
static pg_always_inline LION_TARGET("avx2") void
planes_strided_group_avx2(const uint64 *bits, const uint64 *const *p,
						  uint32 nwords, uint32 r, uint32 *counts)
{
	const __m256i zero = _mm256_setzero_si256();
	const uint64 *p0 = p[0];
	const uint64 *p1 = r > 1 ? p[1] : p0;
	const uint64 *p2 = r > 2 ? p[2] : p0;
	const uint64 *p3 = r > 3 ? p[3] : p0;
	__m256i		s0 = zero;
	__m256i		s1 = zero;
	__m256i		s2 = zero;
	__m256i		s3 = zero;
	uint32		nvec = nwords & ~(uint32) 3;
	uint32		w = 0;

	while (w < nvec)
	{
		uint32		end = Min(nvec, w + 4 * PLANES_AVX2_RUN);
		__m256i		c0 = zero;
		__m256i		c1 = zero;
		__m256i		c2 = zero;
		__m256i		c3 = zero;

		for (; w < end; w += 4)
		{
			__m256i		b = _mm256_loadu_si256((const __m256i *) (bits + w));

			c0 = planes_avx2_add(c0, b, p0 + w);
			if (r > 1)
				c1 = planes_avx2_add(c1, b, p1 + w);
			if (r > 2)
				c2 = planes_avx2_add(c2, b, p2 + w);
			if (r > 3)
				c3 = planes_avx2_add(c3, b, p3 + w);
		}
		s0 = _mm256_add_epi64(s0, _mm256_sad_epu8(c0, zero));
		if (r > 1)
			s1 = _mm256_add_epi64(s1, _mm256_sad_epu8(c1, zero));
		if (r > 2)
			s2 = _mm256_add_epi64(s2, _mm256_sad_epu8(c2, zero));
		if (r > 3)
			s3 = _mm256_add_epi64(s3, _mm256_sad_epu8(c3, zero));
	}
	if (w < nwords)
	{
		uint32		n = nwords - w;
		__m256i		b = planes_avx2_padded(bits + w, n);

#define PLANES_AVX2_TAIL(s, pi) \
		s = _mm256_add_epi64(s, _mm256_sad_epu8(planes_avx2_byte_counts(_mm256_and_si256(b, \
								planes_avx2_padded((pi) + w, n))), zero))
		PLANES_AVX2_TAIL(s0, p0);
		if (r > 1)
			PLANES_AVX2_TAIL(s1, p1);
		if (r > 2)
			PLANES_AVX2_TAIL(s2, p2);
		if (r > 3)
			PLANES_AVX2_TAIL(s3, p3);
#undef PLANES_AVX2_TAIL
	}
	counts[0] = (uint32) planes_avx2_hsum(s0);
	if (r > 1)
		counts[1] = (uint32) planes_avx2_hsum(s1);
	if (r > 2)
		counts[2] = (uint32) planes_avx2_hsum(s2);
	if (r > 3)
		counts[3] = (uint32) planes_avx2_hsum(s3);
}

static LION_TARGET("avx2") void
planes_strided_avx2(const uint64 *bits, const uint64 *const *planes,
					uint32 nwords, uint32 k, uint32 *counts)
{
	uint32		j0;

	for (j0 = 0; j0 + 4 <= k; j0 += 4)
		planes_strided_group_avx2(bits, planes + j0, nwords, 4, counts + j0);
	switch (k - j0)
	{
		case 3:
			planes_strided_group_avx2(bits, planes + j0, nwords, 3, counts + j0);
			break;
		case 2:
			planes_strided_group_avx2(bits, planes + j0, nwords, 2, counts + j0);
			break;
		case 1:
			planes_strided_group_avx2(bits, planes + j0, nwords, 1, counts + j0);
			break;
		default:
			break;
	}
}

#ifdef LION_HAVE_X86_AVX512

/* ---------- AVX-512 (VPOPCNTDQ, on 256-bit vectors) ---------- */

#define LION_TARGET_AVX512	LION_TARGET("avx2,avx512f,avx512vl,avx512vpopcntdq")

/* sum plus the popcounts of b AND v, lane by lane */
static pg_always_inline LION_TARGET_AVX512 __m256i
planes_avx512_add(__m256i sum, __m256i b, __m256i v)
{
	return _mm256_add_epi64(sum, _mm256_popcnt_epi64(_mm256_and_si256(b, v)));
}

#define planes_avx512_load(p)	_mm256_loadu_si256((const __m256i *) (p))

/*
 * INTERLEAVED, g full groups at once, as AVX2's, each lane's word counted
 * whole into its 64-bit sum; counts NULL as AVX2's.
 */
static pg_always_inline LION_TARGET_AVX512 uint64
planes_interleaved_groups_avx512(const uint64 *bits, const uint64 *planes,
								 uint32 nwords, uint32 k, uint32 j0, uint32 g,
								 uint32 *counts)
{
	const __m256i zero = _mm256_setzero_si256();
	__m256i		s0 = zero;
	__m256i		s1 = zero;
	__m256i		s2 = zero;
	__m256i		s3 = zero;
	uint32		w;

	for (w = 0; w < nwords; w++)
	{
		__m256i		b = _mm256_set1_epi64x((long long) bits[w]);
		const uint64 *at = planes + (Size) w * k + j0;

		s0 = planes_avx512_add(s0, b, planes_avx512_load(at));
		if (g > 1)
			s1 = planes_avx512_add(s1, b, planes_avx512_load(at + 4));
		if (g > 2)
			s2 = planes_avx512_add(s2, b, planes_avx512_load(at + 8));
		if (g > 3)
			s3 = planes_avx512_add(s3, b, planes_avx512_load(at + 12));
	}
	if (counts == NULL)
		return planes_avx2_weigh4(s0);
	planes_avx2_lanes_out(s0, 4, counts);
	if (g > 1)
		planes_avx2_lanes_out(s1, 4, counts + 4);
	if (g > 2)
		planes_avx2_lanes_out(s2, 4, counts + 8);
	if (g > 3)
		planes_avx2_lanes_out(s3, 4, counts + 12);
	return 0;
}

/*
 * INTERLEAVED, the last r planes (1 .. 3), loaded under a mask of r lanes,
 * which reads nothing past them.
 */
static pg_always_inline LION_TARGET_AVX512 void
planes_interleaved_last_avx512(const uint64 *bits, const uint64 *planes,
							   uint32 nwords, uint32 k, uint32 j0, uint32 r,
							   uint32 *counts)
{
	const __mmask8 lanes = (__mmask8) ((1u << r) - 1);
	__m256i		sum = _mm256_setzero_si256();
	uint32		w;

	for (w = 0; w < nwords; w++)
		sum = planes_avx512_add(sum, _mm256_set1_epi64x((long long) bits[w]),
								_mm256_maskz_loadu_epi64(lanes, planes + (Size) w * k + j0));
	planes_avx2_lanes_out(sum, r, counts);
}

static LION_TARGET_AVX512 void
planes_interleaved_avx512(const uint64 *bits, const uint64 *planes,
						  uint32 nwords, uint32 k, uint32 *counts)
{
	uint32		j0 = 0;

	if (k == PLANES_K_QUERY)
	{
		planes_interleaved_groups_avx512(bits, planes, nwords, PLANES_K_QUERY, 0, 1, counts);
		return;
	}

	for (; j0 + 16 <= k; j0 += 16)
		planes_interleaved_groups_avx512(bits, planes, nwords, k, j0, 4, counts + j0);
	switch ((k - j0) / 4)
	{
		case 3:
			planes_interleaved_groups_avx512(bits, planes, nwords, k, j0, 3, counts + j0);
			break;
		case 2:
			planes_interleaved_groups_avx512(bits, planes, nwords, k, j0, 2, counts + j0);
			break;
		case 1:
			planes_interleaved_groups_avx512(bits, planes, nwords, k, j0, 1, counts + j0);
			break;
		default:
			break;
	}
	j0 = k - k % 4;
	if (j0 < k)
		planes_interleaved_last_avx512(bits, planes, nwords, k, j0, k - j0, counts + j0);
}

/* STRIDED, one group of r planes, as AVX2's; the last nwords % 4 words masked. */
static pg_always_inline LION_TARGET_AVX512 void
planes_strided_group_avx512(const uint64 *bits, const uint64 *const *p,
							uint32 nwords, uint32 r, uint32 *counts)
{
	const __m256i zero = _mm256_setzero_si256();
	const uint64 *p0 = p[0];
	const uint64 *p1 = r > 1 ? p[1] : p0;
	const uint64 *p2 = r > 2 ? p[2] : p0;
	const uint64 *p3 = r > 3 ? p[3] : p0;
	__m256i		s0 = zero;
	__m256i		s1 = zero;
	__m256i		s2 = zero;
	__m256i		s3 = zero;
	uint32		nvec = nwords & ~(uint32) 3;
	uint32		w;

	for (w = 0; w < nvec; w += 4)
	{
		__m256i		b = planes_avx512_load(bits + w);

		s0 = planes_avx512_add(s0, b, planes_avx512_load(p0 + w));
		if (r > 1)
			s1 = planes_avx512_add(s1, b, planes_avx512_load(p1 + w));
		if (r > 2)
			s2 = planes_avx512_add(s2, b, planes_avx512_load(p2 + w));
		if (r > 3)
			s3 = planes_avx512_add(s3, b, planes_avx512_load(p3 + w));
	}
	if (w < nwords)
	{
		const __mmask8 tail = (__mmask8) ((1u << (nwords - w)) - 1);
		__m256i		b = _mm256_maskz_loadu_epi64(tail, bits + w);

		s0 = planes_avx512_add(s0, b, _mm256_maskz_loadu_epi64(tail, p0 + w));
		if (r > 1)
			s1 = planes_avx512_add(s1, b, _mm256_maskz_loadu_epi64(tail, p1 + w));
		if (r > 2)
			s2 = planes_avx512_add(s2, b, _mm256_maskz_loadu_epi64(tail, p2 + w));
		if (r > 3)
			s3 = planes_avx512_add(s3, b, _mm256_maskz_loadu_epi64(tail, p3 + w));
	}
	counts[0] = (uint32) planes_avx2_hsum(s0);
	if (r > 1)
		counts[1] = (uint32) planes_avx2_hsum(s1);
	if (r > 2)
		counts[2] = (uint32) planes_avx2_hsum(s2);
	if (r > 3)
		counts[3] = (uint32) planes_avx2_hsum(s3);
}

static LION_TARGET_AVX512 void
planes_strided_avx512(const uint64 *bits, const uint64 *const *planes,
					  uint32 nwords, uint32 k, uint32 *counts)
{
	uint32		j0;

	for (j0 = 0; j0 + 4 <= k; j0 += 4)
		planes_strided_group_avx512(bits, planes + j0, nwords, 4, counts + j0);
	switch (k - j0)
	{
		case 3:
			planes_strided_group_avx512(bits, planes + j0, nwords, 3, counts + j0);
			break;
		case 2:
			planes_strided_group_avx512(bits, planes + j0, nwords, 2, counts + j0);
			break;
		case 1:
			planes_strided_group_avx512(bits, planes + j0, nwords, 1, counts + j0);
			break;
		default:
			break;
	}
}

#endif							/* LION_HAVE_X86_AVX512 */
#endif							/* LION_HAVE_X86_SIMD */

/* ---------- the choice ---------- */

void
lion_planes_count_interleaved(const uint64 *bits, const uint64 *planes,
							  uint32 nwords, uint32 k, uint32 *counts)
{
	switch (lion_simd_current())
	{
#ifdef LION_HAVE_X86_SIMD
#ifdef LION_HAVE_X86_AVX512
		case LION_SIMD_AVX512:
			planes_interleaved_avx512(bits, planes, nwords, k, counts);
			return;
#endif
		case LION_SIMD_AVX2:
			planes_interleaved_avx2(bits, planes, nwords, k, counts);
			return;
		case LION_SIMD_POPCNT:
			planes_interleaved_popcnt(bits, planes, nwords, k, counts);
			return;
#endif
		default:
			planes_interleaved_portable(bits, planes, nwords, k, counts);
			return;
	}
}

void
lion_planes_count_strided(const uint64 *bits, const uint64 *const *planes,
						  uint32 nwords, uint32 k, uint32 *counts)
{
	switch (lion_simd_current())
	{
#ifdef LION_HAVE_X86_SIMD
#ifdef LION_HAVE_X86_AVX512
		case LION_SIMD_AVX512:
			planes_strided_avx512(bits, planes, nwords, k, counts);
			return;
#endif
		case LION_SIMD_AVX2:
			planes_strided_avx2(bits, planes, nwords, k, counts);
			return;
		case LION_SIMD_POPCNT:
			planes_strided_popcnt(bits, planes, nwords, k, counts);
			return;
#endif
		default:
			planes_strided_portable(bits, planes, nwords, k, counts);
			return;
	}
}

/* sum over j < k of 2^j counts[j], modulo 2^64 */
static inline uint64
planes_weigh(const uint32 *counts, uint32 k)
{
	uint64		sum = 0;
	uint32		j;

	for (j = 0; j < k; j++)
		sum += (uint64) counts[j] << j;
	return sum;
}

/*
 * The weighted sum of lionvec's four query planes, each implementation's
 * pass and its weighing in one function, the stride a constant and the
 * lanes weighed in registers.  Through the counts of any k - a call more,
 * an array of 64 counts on the stack, a loop of shifts - an AVX-512 sum over
 * 24 words (1536 dimensions) took 22 - 28 ns where lionvec's own kernel
 * took 11 - 15; this way 16 against 15, and AVX2's 26 against 29, POPCNT's
 * 38 against 35 (DESIGN.md §50, Measured).  These are lionvec's plane-1
 * kernels now (lionvec_plane_dot(), DESIGN.md §44.3, "Kernels").
 *
 * The portable one is kept out of line, as the others are by their target:
 * inlined into lion_planes_dot_interleaved(), its loop's registers made
 * that function save six and grow a frame on every call, whichever
 * implementation it then jumped to, where it is now a test of k, a load of
 * the choice and a jump.
 */
static pg_noinline uint64
planes_dot4_portable(const uint64 *bits, const uint64 *planes, uint32 nwords)
{
	uint32		c[PLANES_K_QUERY];

	planes_count_words(bits, planes, NULL, true, nwords, PLANES_K_QUERY, false, c);
	return (uint64) c[0] + 2 * (uint64) c[1] + 4 * (uint64) c[2] + 8 * (uint64) c[3];
}

#ifdef LION_HAVE_X86_SIMD
static LION_TARGET("popcnt") uint64
planes_dot4_popcnt(const uint64 *bits, const uint64 *planes, uint32 nwords)
{
	uint32		c[PLANES_K_QUERY];

	planes_count_words(bits, planes, NULL, true, nwords, PLANES_K_QUERY, true, c);
	return (uint64) c[0] + 2 * (uint64) c[1] + 4 * (uint64) c[2] + 8 * (uint64) c[3];
}

static LION_TARGET("avx2") uint64
planes_dot4_avx2(const uint64 *bits, const uint64 *planes, uint32 nwords)
{
	return planes_interleaved_groups_avx2(bits, planes, nwords, PLANES_K_QUERY, 0, 1, NULL);
}

#ifdef LION_HAVE_X86_AVX512
static LION_TARGET_AVX512 uint64
planes_dot4_avx512(const uint64 *bits, const uint64 *planes, uint32 nwords)
{
	return planes_interleaved_groups_avx512(bits, planes, nwords, PLANES_K_QUERY, 0, 1, NULL);
}
#endif
#endif

/* Any k, through the counts. */
static pg_noinline uint64
planes_dot_counts(const uint64 *bits, const uint64 *planes, uint32 nwords,
				  uint32 k)
{
	uint32		counts[LION_PLANES_MAX_BITS];

	lion_planes_count_interleaved(bits, planes, nwords, k, counts);
	return planes_weigh(counts, k);
}

uint64
lion_planes_dot_interleaved(const uint64 *bits, const uint64 *planes,
							uint32 nwords, uint32 k)
{
	Assert(k <= LION_PLANES_MAX_BITS);
	if (k != PLANES_K_QUERY)
		return planes_dot_counts(bits, planes, nwords, k);
	switch (lion_simd_current())
	{
#ifdef LION_HAVE_X86_SIMD
#ifdef LION_HAVE_X86_AVX512
		case LION_SIMD_AVX512:
			return planes_dot4_avx512(bits, planes, nwords);
#endif
		case LION_SIMD_AVX2:
			return planes_dot4_avx2(bits, planes, nwords);
		case LION_SIMD_POPCNT:
			return planes_dot4_popcnt(bits, planes, nwords);
#endif
		default:
			return planes_dot4_portable(bits, planes, nwords);
	}
}

uint64
lion_planes_dot_strided(const uint64 *bits, const uint64 *const *planes,
						uint32 nwords, uint32 k)
{
	uint32		counts[LION_PLANES_MAX_BITS];

	Assert(k <= LION_PLANES_MAX_BITS);
	lion_planes_count_strided(bits, planes, nwords, k, counts);
	return planes_weigh(counts, k);
}

/* ----------------------------------------------------------------
 *						the transpose
 *
 * A 64 x 64 bit matrix, word r its row r, bit c of it column c, transposed
 * as Hacker's Delight's 7-3 transposes 32 x 32: in six rounds, of strides
 * j = 32, 16, .., 1, each swapping the off-diagonal j x j blocks of every
 * 2j x 2j block - word k's bits from j up (columns with bit j of c set) for
 * word k + j's bits below j, where bit j of k is clear - a mask, two shifts
 * and three XORs a pair of words, 192 pairs.  Round j exchanges bit j of a
 * bit's row index with bit j of its column index, so the rounds commute and
 * the six together exchange all of them: the transpose.
 *
 * The planes of values of nbits bits are the first nbits words of the
 * transpose of the values (scatter), and values the transpose of the
 * planes padded with zero words (gather); for nbits up to p, a power of
 * two below 64, each needs only some of the work:
 *
 *	scatter		rows below p of the result.  Round j < p mixes rows below
 *				p only among themselves; round j >= p, which runs first,
 *				needs to make only rows below j - its pairs' first words -
 *				from rows below 2j: j half-swaps
 *	gather		rows from p up zero.  Run in the other order, j = 1 .. 32,
 *				rounds j < p mix rows below p among themselves and leave the
 *				zeros; round j >= p finds rows from j up zero, and splits
 *				each row k below j into itself and row k + j
 *
 * so that 4-bit values (lionvec's query, a 16-valued column) take 60
 * half-swaps and four swaps a word rather than 192 swaps.
 * ----------------------------------------------------------------
 */

/* Round j's mask: the columns of bit j clear (j = 1 << i, mask i). */
static const uint64 planes_round_mask[6] = {
	UINT64CONST(0x5555555555555555),
	UINT64CONST(0x3333333333333333),
	UINT64CONST(0x0F0F0F0F0F0F0F0F),
	UINT64CONST(0x00FF00FF00FF00FF),
	UINT64CONST(0x0000FFFF0000FFFF),
	UINT64CONST(0x00000000FFFFFFFF)
};

/* Rows k and k + j (bit j of k clear) exchange columns: round j's pair. */
static pg_always_inline void
planes_swap(uint64 *m, uint32 k, uint32 j, uint64 mask)
{
	uint64		t = ((m[k] >> j) ^ m[k + j]) & mask;

	m[k] ^= t << j;
	m[k + j] ^= t;
}

/* The pairs of round j among rows below n, n a multiple of 2j. */
static pg_always_inline void
planes_round(uint64 *m, uint32 j, uint64 mask, uint32 n)
{
	uint32		k;

	for (k = 0; k < n; k = ((k | j) + 1) & ~j)
		planes_swap(m, k, j, mask);
}

void
lion_planes_transpose64(uint64 *m)
{
	int			i;

	for (i = 5; i >= 0; i--)
		planes_round(m, 1u << i, planes_round_mask[i], 64);
}

/* p: the power of two of 1 .. 64 that nbits rounds up to. */
static inline uint32
planes_pow2(uint32 nbits)
{
	uint32		p = 1;

	Assert(nbits >= 1 && nbits <= LION_PLANES_MAX_BITS);
	while (p < nbits)
		p <<= 1;
	return p;
}

/*
 * Rows below p of the transpose of m, p a power of two (scatter); rows from
 * p up are left undefined.
 */
static void
planes_transpose_rows(uint64 *m, uint32 p)
{
	int			i;

	for (i = 5; i >= 0; i--)
	{
		uint32		j = 1u << i;
		uint64		mask = planes_round_mask[i];

		if (j >= p)
		{
			uint32		k;

			/* the first words of the pairs below 2j, all that is read on */
			for (k = 0; k < j; k++)
				m[k] ^= (((m[k] >> j) ^ m[k + j]) & mask) << j;
		}
		else
			planes_round(m, j, mask, p);
	}
}

/*
 * The transpose of m whose rows from p up are zero, p a power of two
 * (gather): every row of it.
 */
static void
planes_transpose_from_rows(uint64 *m, uint32 p)
{
	int			i;

	for (i = 0; i < 6; i++)
	{
		uint32		j = 1u << i;
		uint64		mask = planes_round_mask[i];

		if (j >= p)
		{
			uint32		k;

			/* rows from j up are zero: row k's columns from j go to row k + j */
			for (k = 0; k < j; k++)
			{
				uint64		t = (m[k] >> j) & mask;

				m[k] ^= t << j;
				m[k + j] = t;
			}
		}
		else
			planes_round(m, j, mask, p);
	}
}

/* Word w's 64 values (zero past n) into m. */
static inline void
planes_load_values(uint64 *m, const uint64 *values, uint32 n, uint32 w)
{
	uint32		have = Min(64, n - w * 64);

	memcpy(m, values + (Size) w * 64, have * sizeof(uint64));
	if (have < 64)
		memset(m + have, 0, (64 - have) * sizeof(uint64));
}

void
lion_planes_scatter_interleaved(const uint64 *values, uint32 n, uint32 nbits,
								uint64 *planes)
{
	uint32		nwords = (n + 63) / 64;
	uint32		p = planes_pow2(nbits);
	uint32		w;

	for (w = 0; w < nwords; w++)
	{
		uint64		m[64];

		planes_load_values(m, values, n, w);
		planes_transpose_rows(m, p);
		memcpy(planes + (Size) w * nbits, m, nbits * sizeof(uint64));
	}
}

void
lion_planes_scatter_strided(const uint64 *values, uint32 n, uint32 nbits,
							uint64 *const *planes)
{
	uint32		nwords = (n + 63) / 64;
	uint32		p = planes_pow2(nbits);
	uint32		w;
	uint32		j;

	for (w = 0; w < nwords; w++)
	{
		uint64		m[64];

		planes_load_values(m, values, n, w);
		planes_transpose_rows(m, p);
		for (j = 0; j < nbits; j++)
			planes[j][w] = m[j];
	}
}

/* Word w's values, from m, as many as remain of n. */
static inline void
planes_store_values(const uint64 *m, uint64 *values, uint32 n, uint32 w)
{
	memcpy(values + (Size) w * 64, m, Min(64, n - w * 64) * sizeof(uint64));
}

void
lion_planes_gather_interleaved(const uint64 *planes, uint32 n, uint32 nbits,
							   uint64 *values)
{
	uint32		nwords = (n + 63) / 64;
	uint32		p = planes_pow2(nbits);
	uint32		w;

	for (w = 0; w < nwords; w++)
	{
		uint64		m[64];

		memcpy(m, planes + (Size) w * nbits, nbits * sizeof(uint64));
		memset(m + nbits, 0, (64 - nbits) * sizeof(uint64));
		planes_transpose_from_rows(m, p);
		planes_store_values(m, values, n, w);
	}
}

void
lion_planes_gather_strided(const uint64 *const *planes, uint32 n, uint32 nbits,
						   uint64 *values)
{
	uint32		nwords = (n + 63) / 64;
	uint32		p = planes_pow2(nbits);
	uint32		w;
	uint32		j;

	for (w = 0; w < nwords; w++)
	{
		uint64		m[64];

		for (j = 0; j < nbits; j++)
			m[j] = planes[j][w];
		memset(m + nbits, 0, (64 - nbits) * sizeof(uint64));
		planes_transpose_from_rows(m, p);
		planes_store_values(m, values, n, w);
	}
}
